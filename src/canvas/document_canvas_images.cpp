#include "canvas/document_canvas.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <functional>
#include <span>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Pictures arriving by paste or drop. Every read is asynchronous: a picture
// on the clipboard is read as a texture (or SVG as text, to stay vector), a
// file list is filtered to pictures, and rich HTML has its remote pictures
// fetched in the background and embedded before it is pasted. Pictures are
// written to a private folder, embedded in the document (never linked), set
// in line with the text and fitted to the text width by the engine.

namespace luma::write::canvas {

namespace {

constexpr std::size_t kMaximumImageBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumRemoteImages = 16;

bool is_image_file(GFile* file) {
  char* path = g_file_get_path(file);
  if (path == nullptr) {
    return false;
  }
  gboolean uncertain = FALSE;
  char* type = g_content_type_guess(path, nullptr, 0, &uncertain);
  g_free(path);
  const bool image = type != nullptr && g_str_has_prefix(type, "image/");
  g_free(type);
  return image;
}

}  // namespace

namespace {

// Reads a whole clipboard format into memory, then calls back on the main
// context with the bytes (nullptr when it could not be read).
struct ByteRead {
  std::shared_ptr<DocumentCanvas::ClipboardBridgeRef> owner;
  std::string mime_type;
  GCancellable* cancellable{nullptr};
  std::function<void(DocumentCanvas&, const std::string&, GBytes*)> done;
};

}  // namespace

void DocumentCanvas::read_clipboard_bytes(
    std::initializer_list<const char*> mime_types,
    std::function<void(DocumentCanvas&, const std::string&, GBytes*)> done) {
  std::vector<const char*> types{mime_types};
  types.push_back(nullptr);
  auto* read = new ByteRead{image_bridge_, {}, G_CANCELLABLE(g_object_ref(paste_cancellable_)),
                            std::move(done)};
  gdk_clipboard_read_async(
      gtk_widget_get_clipboard(drawing_area_), types.data(), G_PRIORITY_DEFAULT,
      read->cancellable,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        auto* job = static_cast<ByteRead*>(data);
        const char* mime = nullptr;
        GInputStream* stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result, &mime, nullptr);
        if (stream == nullptr || job->owner->target == nullptr) {
          if (stream != nullptr) g_object_unref(stream);
          if (job->owner->target != nullptr) job->done(*job->owner->target, {}, nullptr);
          g_object_unref(job->cancellable);
          delete job;
          return;
        }
        job->mime_type = mime == nullptr ? "" : mime;
        GOutputStream* memory = g_memory_output_stream_new_resizable();
        g_output_stream_splice_async(
            memory, stream,
            static_cast<GOutputStreamSpliceFlags>(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
                                                  G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
            G_PRIORITY_DEFAULT, job->cancellable,
            +[](GObject* target_stream, GAsyncResult* spliced, gpointer inner) {
              auto* pending = static_cast<ByteRead*>(inner);
              const gssize size =
                  g_output_stream_splice_finish(G_OUTPUT_STREAM(target_stream), spliced, nullptr);
              GBytes* bytes = nullptr;
              if (size >= 0 && static_cast<std::size_t>(size) <= kMaximumImageBytes) {
                bytes = g_memory_output_stream_steal_as_bytes(
                    G_MEMORY_OUTPUT_STREAM(target_stream));
              }
              if (pending->owner->target != nullptr) {
                pending->done(*pending->owner->target, pending->mime_type, bytes);
              }
              if (bytes != nullptr) g_bytes_unref(bytes);
              g_object_unref(target_stream);
              g_object_unref(pending->cancellable);
              delete pending;
            },
            job);
        g_object_unref(stream);
      },
      read);
}

std::filesystem::path DocumentCanvas::image_directory() {
  if (image_directory_.empty()) {
    if (char* folder = g_dir_make_tmp("luma-write-pictures-XXXXXX", nullptr); folder != nullptr) {
      image_directory_ = folder;
      g_free(folder);
    }
  }
  return image_directory_;
}

void DocumentCanvas::insert_image_file(const std::filesystem::path& file) {
  if (!engine_.has_document() || engine_.status().read_only) {
    gtk_widget_error_bell(drawing_area_);
    return;
  }
  if (const auto result = engine_.insert_image(file); !result.ok) {
    g_warning("Write could not insert a picture: %s", result.message.c_str());
    gtk_widget_error_bell(drawing_area_);
    return;
  }
  g_debug("Write inserted a picture from %s", file.c_str());
  refresh_document();
}

void DocumentCanvas::insert_image_bytes(GBytes* bytes, const char* extension) {
  const auto folder = image_directory();
  if (folder.empty() || bytes == nullptr) {
    gtk_widget_error_bell(drawing_area_);
    return;
  }
  const auto file = folder / ("picture-" + std::to_string(++image_counter_) + extension);
  gsize size = 0;
  const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  if (!g_file_set_contents(file.c_str(), data, static_cast<gssize>(size), nullptr)) {
    gtk_widget_error_bell(drawing_area_);
    return;
  }
  insert_image_file(file);
}

void DocumentCanvas::insert_texture(GdkTexture* texture) {
  if (texture == nullptr) {
    gtk_widget_error_bell(drawing_area_);
    return;
  }
  GBytes* png = gdk_texture_save_to_png_bytes(texture);
  insert_image_bytes(png, ".png");
  g_bytes_unref(png);
}

void DocumentCanvas::insert_files(GSList* files) {
  int inserted = 0;
  for (GSList* item = files; item != nullptr; item = item->next) {
    auto* file = G_FILE(item->data);
    if (!is_image_file(file)) continue;
    char* path = g_file_get_path(file);
    if (path != nullptr) {
      insert_image_file(path);
      ++inserted;
    }
    g_free(path);
  }
  if (inserted == 0) {
    gtk_widget_error_bell(drawing_area_);
  }
}

// Paste: pictures first (files, then SVG, then any image), rich text next,
// plain text last.
bool DocumentCanvas::paste_rich_content() {
  GdkClipboard* clipboard = gtk_widget_get_clipboard(drawing_area_);
  GdkContentFormats* formats = gdk_clipboard_get_formats(clipboard);
  if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST)) {
    gdk_clipboard_read_value_async(
        clipboard, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, paste_cancellable_,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          auto owner = std::unique_ptr<std::shared_ptr<ClipboardBridgeRef>>(
              static_cast<std::shared_ptr<ClipboardBridgeRef>*>(data));
          const GValue* value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), result, nullptr);
          if (value == nullptr || (*owner)->target == nullptr) return;
          auto* files = static_cast<GSList*>(g_value_get_boxed(value));
          (*owner)->target->insert_files(files);
        },
        new std::shared_ptr<ClipboardBridgeRef>(image_bridge_));
    return true;
  }
  if (gdk_content_formats_contain_mime_type(formats, "image/svg+xml")) {
    read_clipboard_bytes({"image/svg+xml"}, [](DocumentCanvas& self, const std::string&, GBytes* bytes) {
      self.insert_image_bytes(bytes, ".svg");
    });
    return true;
  }
  if (gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE)) {
    gdk_clipboard_read_texture_async(
        clipboard, paste_cancellable_,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          auto owner = std::unique_ptr<std::shared_ptr<ClipboardBridgeRef>>(
              static_cast<std::shared_ptr<ClipboardBridgeRef>*>(data));
          GdkTexture* texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result, nullptr);
          if ((*owner)->target != nullptr) {
            (*owner)->target->insert_texture(texture);
          }
          if (texture != nullptr) g_object_unref(texture);
        },
        new std::shared_ptr<ClipboardBridgeRef>(image_bridge_));
    return true;
  }
  if (gdk_content_formats_contain_mime_type(formats, "text/html")) {
    read_clipboard_bytes({"text/html"}, [](DocumentCanvas& self, const std::string&, GBytes* bytes) {
      if (bytes == nullptr) {
        gtk_widget_error_bell(self.drawing_area_);
        return;
      }
      gsize size = 0;
      const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
      self.paste_html(std::string{data, size});
    });
    return true;
  }
  return false;
}

// Remote pictures in pasted HTML are fetched in the background and embedded
// as data, so the engine never reaches out to the network while it pastes.
void DocumentCanvas::paste_html(std::string html) {
  struct Fetch {
    std::shared_ptr<ClipboardBridgeRef> owner;
    std::string html;
    std::vector<std::pair<std::string, std::string>> replacements;  // url, data URI
    int pending{0};
  };
  auto fetch = std::make_shared<Fetch>();
  fetch->owner = image_bridge_;
  fetch->html = std::move(html);
  std::vector<std::string> urls;
  for (std::size_t at = fetch->html.find("src=\""); at != std::string::npos;
       at = fetch->html.find("src=\"", at + 5)) {
    const auto start = at + 5;
    const auto end = fetch->html.find('"', start);
    if (end == std::string::npos) break;
    std::string url = fetch->html.substr(start, end - start);
    if ((url.starts_with("http://") || url.starts_with("https://")) &&
        std::find(urls.begin(), urls.end(), url) == urls.end() && urls.size() < kMaximumRemoteImages) {
      urls.push_back(url);
    }
  }
  const auto finish = [](const std::shared_ptr<Fetch>& job) {
    if (job->owner->target == nullptr) return;
    std::string text = job->html;
    for (const auto& [url, data] : job->replacements) {
      for (auto at = text.find(url); at != std::string::npos; at = text.find(url, at + data.size())) {
        text.replace(at, url.size(), data);
      }
    }
    DocumentCanvas& self = *job->owner->target;
    const auto result = self.engine_.paste(
        "text/html", std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                                text.size()));
    if (!result.ok) gtk_widget_error_bell(self.drawing_area_);
  };
  if (urls.empty()) {
    finish(fetch);
    return;
  }
  fetch->pending = static_cast<int>(urls.size());
  for (const auto& url : urls) {
    GFile* file = g_file_new_for_uri(url.c_str());
    auto* context = new std::pair<std::shared_ptr<Fetch>, std::string>(fetch, url);
    g_file_load_contents_async(
        file, paste_cancellable_,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::pair<std::shared_ptr<Fetch>, std::string>> pending(
              static_cast<std::pair<std::shared_ptr<Fetch>, std::string>*>(data));
          char* contents = nullptr;
          gsize length = 0;
          if (g_file_load_contents_finish(G_FILE(source), result, &contents, &length, nullptr,
                                          nullptr) &&
              length > 0 && length <= kMaximumImageBytes) {
            char* type = g_content_type_guess(nullptr, reinterpret_cast<const guchar*>(contents),
                                              length, nullptr);
            char* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
            if (mime != nullptr && g_str_has_prefix(mime, "image/")) {
              char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(contents), length);
              pending->first->replacements.emplace_back(
                  pending->second, std::string{"data:"} + mime + ";base64," + encoded);
              g_free(encoded);
            }
            g_free(mime);
            g_free(type);
          }
          g_free(contents);
          if (--pending->first->pending == 0) {
            // All answered (or failed): paste what arrived. A picture that
            // could not be fetched is left as the page had it.
            auto job = pending->first;
            if (job->owner->target != nullptr) {
              std::string text = job->html;
              for (const auto& [source_url, uri] : job->replacements) {
                for (auto at = text.find(source_url); at != std::string::npos;
                     at = text.find(source_url, at + uri.size())) {
                  text.replace(at, source_url.size(), uri);
                }
              }
              DocumentCanvas& self = *job->owner->target;
              const auto pasted = self.engine_.paste(
                  "text/html", std::span<const std::byte>(
                                   reinterpret_cast<const std::byte*>(text.data()), text.size()));
              if (!pasted.ok) gtk_widget_error_bell(self.drawing_area_);
            }
          }
        },
        context);
    g_object_unref(file);
  }
}

// Pictures dropped from Filer or a browser land where they are dropped.
void DocumentCanvas::install_drop_target() {
  GType types[] = {GDK_TYPE_FILE_LIST, GDK_TYPE_TEXTURE};
  GtkDropTarget* target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
  gtk_drop_target_set_gtypes(target, types, G_N_ELEMENTS(types));
  g_signal_connect(target, "drop", G_CALLBACK(+[](GtkDropTarget*, const GValue* value, double x,
                                                   double y, gpointer data) -> gboolean {
                     auto& self = *static_cast<DocumentCanvas*>(data);
                     if (!self.engine_.has_document() || self.engine_.status().read_only) {
                       return FALSE;
                     }
                     self.place_caret_at(x, y);
                     if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
                       self.insert_files(static_cast<GSList*>(g_value_get_boxed(value)));
                       return TRUE;
                     }
                     if (G_VALUE_HOLDS(value, GDK_TYPE_TEXTURE)) {
                       self.insert_texture(GDK_TEXTURE(g_value_get_object(value)));
                       return TRUE;
                     }
                     return FALSE;
                   }),
                   this);
  gtk_widget_add_controller(drawing_area_, GTK_EVENT_CONTROLLER(target));
}


// ── The selected picture ────────────────────────────────────────────────────

namespace {

// Handle points, clockwise from the top-left corner.
std::array<std::pair<std::int64_t, std::int64_t>, 8> handle_points(const engine::Rect& rect) {
  const auto x0 = rect.x;
  const auto y0 = rect.y;
  const auto x1 = rect.x + rect.width;
  const auto y1 = rect.y + rect.height;
  const auto xm = rect.x + rect.width / 2;
  const auto ym = rect.y + rect.height / 2;
  return {{{x0, y0}, {xm, y0}, {x1, y0}, {x1, ym}, {x1, y1}, {xm, y1}, {x0, y1}, {x0, ym}}};
}

}  // namespace

int DocumentCanvas::graphic_handle_at(const double x, const double y) const {
  if (!graphic_selection_twips_.has_value() || engine_.status().read_only) {
    return -1;
  }
  const auto space = coordinates();
  const auto points = handle_points(*graphic_selection_twips_);
  for (std::size_t index = 0; index < points.size(); ++index) {
    const double px = space.twips_x_to_pixels(points[index].first);
    const double py = space.twips_y_to_pixels(points[index].second);
    if (std::abs(px - x) <= 7.0 && std::abs(py - y) <= 7.0) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

// Where the picture would be if the handle were let go now. Corners keep the
// picture's proportions unless Shift was held when the drag began; the
// opposite corner or edge stays put.
engine::Rect DocumentCanvas::resize_preview_twips() const {
  const auto& start = resize_start_;
  std::int64_t left = start.x;
  std::int64_t top = start.y;
  std::int64_t right = start.x + start.width;
  std::int64_t bottom = start.y + start.height;
  const int handle = resize_handle_;
  const bool moves_left = handle == 0 || handle == 6 || handle == 7;
  const bool moves_right = handle == 2 || handle == 3 || handle == 4;
  const bool moves_top = handle == 0 || handle == 1 || handle == 2;
  const bool moves_bottom = handle == 4 || handle == 5 || handle == 6;
  constexpr std::int64_t kMinimum = 144;  // a tenth of an inch
  if (moves_left) left = std::min(resize_x_, right - kMinimum);
  if (moves_right) right = std::max(resize_x_, left + kMinimum);
  if (moves_top) top = std::min(resize_y_, bottom - kMinimum);
  if (moves_bottom) bottom = std::max(resize_y_, top + kMinimum);
  const bool corner = handle % 2 == 0;
  if (corner && !resize_free_ && start.width > 0 && start.height > 0) {
    const double scale = std::max(static_cast<double>(right - left) / static_cast<double>(start.width),
                                  static_cast<double>(bottom - top) / static_cast<double>(start.height));
    const auto width = static_cast<std::int64_t>(std::llround(static_cast<double>(start.width) * scale));
    const auto height = static_cast<std::int64_t>(std::llround(static_cast<double>(start.height) * scale));
    if (moves_left) left = right - width; else right = left + width;
    if (moves_top) top = bottom - height; else bottom = top + height;
  }
  return {left, top, right - left, bottom - top};
}

void DocumentCanvas::draw_graphic_selection(cairo_t* context,
                                            const engine::CoordinateSpace& space) const {
  if (!graphic_selection_twips_.has_value()) {
    return;
  }
  const engine::Rect rect = resize_handle_ >= 0 ? resize_preview_twips() : *graphic_selection_twips_;
  const double x = space.twips_x_to_pixels(rect.x);
  const double y = space.twips_y_to_pixels(rect.y);
  const double width = static_cast<double>(rect.width) * space.pixels_per_twip();
  const double height = static_cast<double>(rect.height) * space.pixels_per_twip();
  cairo_save(context);
  cairo_set_source_rgb(context, 64.0 / 255.0, 110.0 / 255.0, 230.0 / 255.0);
  cairo_set_line_width(context, 1.5);
  if (resize_handle_ >= 0) {
    const double dash[] = {5.0, 3.0};
    cairo_set_dash(context, dash, 2, 0);
  }
  cairo_rectangle(context, x, y, width, height);
  cairo_stroke(context);
  cairo_set_dash(context, nullptr, 0, 0);
  // Eight handles: white squares ringed in the selection blue.
  for (const auto& [hx, hy] : handle_points(rect)) {
    const double px = space.twips_x_to_pixels(hx);
    const double py = space.twips_y_to_pixels(hy);
    cairo_rectangle(context, px - 4.0, py - 4.0, 8.0, 8.0);
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_fill_preserve(context);
    cairo_set_source_rgb(context, 64.0 / 255.0, 110.0 / 255.0, 230.0 / 255.0);
    cairo_set_line_width(context, 1.25);
    cairo_stroke(context);
  }
  // While resizing, the size as the document measures it.
  if (resize_handle_ >= 0) {
    const char* locale = std::setlocale(LC_MEASUREMENT, nullptr);
    const bool metric = locale == nullptr || std::string_view{locale}.find("_US") == std::string_view::npos;
    char text[64];
    if (metric) {
      std::snprintf(text, sizeof(text), "%.1f × %.1f cm", static_cast<double>(rect.width) * 2.54 / 1440.0,
                    static_cast<double>(rect.height) * 2.54 / 1440.0);
    } else {
      std::snprintf(text, sizeof(text), "%.2f × %.2f in", static_cast<double>(rect.width) / 1440.0,
                    static_cast<double>(rect.height) / 1440.0);
    }
    PangoLayout* layout = gtk_widget_create_pango_layout(drawing_area_, text);
    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    const double label_x = x + width - text_width - 12.0;
    const double label_y = y + height + 8.0;
    cairo_set_source_rgba(context, 0.1, 0.12, 0.15, 0.88);
    cairo_rectangle(context, label_x - 6.0, label_y - 3.0, text_width + 12.0, text_height + 6.0);
    cairo_fill(context);
    cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
    cairo_move_to(context, label_x, label_y);
    pango_cairo_show_layout(context, layout);
    g_object_unref(layout);
  }
  cairo_restore(context);
}

// The engine drags the handle it was given to the point it is let go at;
// the point is the constrained corner or edge of the preview.
void DocumentCanvas::finish_resize(const double x, const double y) {
  const auto point = pointer_event(engine::PointerPhase::release, 1, x, y, 1);
  resize_x_ = point.x_twips;
  resize_y_ = point.y_twips;
  const engine::Rect target = resize_preview_twips();
  const auto from = handle_points(resize_start_)[static_cast<std::size_t>(resize_handle_)];
  const auto to = handle_points(target)[static_cast<std::size_t>(resize_handle_)];
  resize_handle_ = -1;
  if (from != to) {
    static_cast<void>(engine_.drag_graphic_handle(true, from.first, from.second));
    static_cast<void>(engine_.drag_graphic_handle(false, to.first, to.second));
  }
  gtk_widget_queue_draw(drawing_area_);
}

// The bar under a selected picture, as Docs has it: how text flows around
// it, where it sits, and the few things done to a picture most often.
void DocumentCanvas::image_command(const char* uno) {
  static_cast<void>(engine_.dispatch_command(uno, "", true));
}

// Right-click on a picture: how it sits with the text, its size, and the
// picture itself.
void DocumentCanvas::show_image_menu(const double x, const double y) {
  if (context_popover_ != nullptr) {
    gtk_popover_popdown(GTK_POPOVER(context_popover_));
    gtk_widget_unparent(context_popover_);
    context_popover_ = nullptr;
  }
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_add_css_class(list, "context-menu");
  const auto separator = [list] {
    GtkWidget* line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(line, "write-menu-separator");
    gtk_box_append(GTK_BOX(list), line);
  };
  const auto row = [this, list](const char* label, std::vector<const char*> commands) {
    GtkWidget* button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "write-menu-row");
    if (GtkWidget* child = gtk_button_get_child(GTK_BUTTON(button)); GTK_IS_LABEL(child)) {
      gtk_label_set_xalign(GTK_LABEL(child), 0.0F);
    }
    auto* stored = new std::vector<std::string>(commands.begin(), commands.end());
    g_object_set_data_full(G_OBJECT(button), "write-commands", stored,
                           +[](gpointer data) { delete static_cast<std::vector<std::string>*>(data); });
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
                       auto* self = static_cast<DocumentCanvas*>(data);
                       gtk_popover_popdown(GTK_POPOVER(self->context_popover_));
                       const auto* names = static_cast<std::vector<std::string>*>(
                           g_object_get_data(G_OBJECT(clicked), "write-commands"));
                       for (const auto& name : *names) {
                         if (name == "replace") {
                           self->replace_image();
                         } else {
                           self->image_command(name.c_str());
                         }
                       }
                     }),
                     this);
    gtk_box_append(GTK_BOX(list), button);
  };
  row("In line with text", {".uno:SetAnchorToChar"});
  row("Wrap text", {".uno:SetAnchorToPara", ".uno:WrapIdeal"});
  row("Break text", {".uno:SetAnchorToPara", ".uno:WrapOff"});
  row("Behind text", {".uno:SetAnchorToPara", ".uno:WrapThroughTransparent"});
  row("In front of text", {".uno:SetAnchorToPara", ".uno:WrapThrough"});
  separator();
  row("Move with text", {".uno:SetAnchorToPara"});
  row("Fix position on page", {".uno:SetAnchorToPage"});
  separator();
  row("Reset size", {".uno:OriginalSize"});
  row("Replace image…", {"replace"});
  separator();
  row("Delete", {".uno:Delete"});
  context_popover_ = gtk_popover_new();
  gtk_widget_add_css_class(context_popover_, "write-popover");
  gtk_popover_set_has_arrow(GTK_POPOVER(context_popover_), FALSE);
  gtk_popover_set_position(GTK_POPOVER(context_popover_), GTK_POS_BOTTOM);
  gtk_popover_set_child(GTK_POPOVER(context_popover_), list);
  gtk_widget_set_parent(context_popover_, drawing_area_);
  const GdkRectangle point{static_cast<int>(x), static_cast<int>(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(context_popover_), &point);
  gtk_popover_popup(GTK_POPOVER(context_popover_));
}

// Replace: choose a picture; it takes the old one's place in the text.
void DocumentCanvas::replace_image() {
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Replace image");
  GtkFileFilter* images = gtk_file_filter_new();
  gtk_file_filter_set_name(images, "Images");
  gtk_file_filter_add_mime_type(images, "image/*");
  gtk_file_dialog_set_default_filter(dialog, images);
  g_object_unref(images);
  GtkRoot* root = gtk_widget_get_root(drawing_area_);
  gtk_file_dialog_open(
      dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : nullptr, nullptr,
      +[](GObject* source, GAsyncResult* result, gpointer data) {
        auto owner = std::unique_ptr<std::shared_ptr<ClipboardBridgeRef>>(
            static_cast<std::shared_ptr<ClipboardBridgeRef>*>(data));
        GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
        if (file == nullptr) return;
        char* path = g_file_get_path(file);
        if (path != nullptr && (*owner)->target != nullptr) {
          (*owner)->target->image_command(".uno:Delete");
          (*owner)->target->insert_image_file(path);
        }
        g_free(path);
        g_object_unref(file);
      },
      new std::shared_ptr<ClipboardBridgeRef>(image_bridge_));
  g_object_unref(dialog);
}

void DocumentCanvas::remove_image_directory() {
  if (image_directory_.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::remove_all(image_directory_, error);
  image_directory_.clear();
}

}  // namespace luma::write::canvas
