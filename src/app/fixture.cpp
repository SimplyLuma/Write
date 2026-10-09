#include "app/fixture.hpp"

#include "ui/write_window.hpp"

#include <json-glib/json-glib.h>
#include <adwaita.h>
#include <fontconfig/fontconfig.h>
#include <luma-ui.h>

#include <filesystem>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace luma::write::app {
namespace {

struct FixtureState {
  JsonParser* parser{nullptr};
  std::filesystem::path directory;
  std::string requested_mode;
  std::unique_ptr<ui::WriteWindow> window;
  GtkWidget* canvas{nullptr};
  LumaSelectionBubble* selection_bubble{nullptr};
};

const char* string(JsonObject* object, const char* key, const char* fallback = "") {
  return json_object_has_member(object, key) ? json_object_get_string_member(object, key) : fallback;
}

std::pair<int, int> text_counts(const std::string& value) {
  int words = 0;
  int characters = 0;
  bool in_word = false;
  for (const char* at = value.c_str(); *at != '\0'; at = g_utf8_next_char(at)) {
    const gunichar ch = g_utf8_get_char(at);
    const bool space = g_unichar_isspace(ch) != FALSE;
    if (!space && !in_word) ++words;
    if (!space) ++characters;
    in_word = !space;
  }
  return {words, characters};
}

// The paper contains author-formatted document text. At a fitted desktop
// width, browser zoom scales that formatting together with the page.
void style_document_label(GtkWidget* widget, const char* text,
                          const std::string_view type, const double zoom) {
  PangoAttrList* attrs = pango_attr_list_new();
  const bool heading = type == "h1" || type == "h2" || type == "h3";
  const bool subtitle = type == "subtitle";
  const bool quote = type == "blockquote";
  const bool caption = type == "figcaption" || type == "foot";
  const bool phone=gtk_widget_get_root(widget) && luma_ui_is_phone_width(GTK_WIDGET(gtk_widget_get_root(widget)));
  const double size = type == "h1" ? (phone ? 28.0 : 38.0) : type == "h2" ? 22.0 :
                      type == "h3" ? 16.0 : subtitle ? 17.0 : quote ? 19.0 :
                      caption ? 12.0 : type == "table-header" ? 12.0 :
                      type == "table-cell" ? 13.0 : 16.5;
  const double line_height = type == "h1" ? 1.1 : type == "h2" ? 1.25 :
                             type == "h3" ? 1.3 : subtitle ? 1.5 :
                             quote ? 1.55 : caption ? 1.2 : 1.72;
  const bool ui_face = heading || subtitle || caption || type == "table-header" ||
                       type == "table-cell";
  if (ui_face) {
    pango_attr_list_insert(attrs, pango_attr_family_new("Figtree"));
  } else {
    // The reference uses font-optical-sizing:auto. Fontconfig otherwise
    // leaves this variable face at its wider default optical size, changing
    // the document's line breaks even when the font size matches.
    PangoFontDescription* face = pango_font_description_from_string("Newsreader");
    const std::string variations = "opsz=" + std::to_string(size);
    pango_font_description_set_variations(face, variations.c_str());
    pango_attr_list_insert(attrs, pango_attr_font_desc_new(face));
    pango_font_description_free(face);
  }
  pango_attr_list_insert(attrs, pango_attr_size_new_absolute(
      static_cast<int>(size * zoom * PANGO_SCALE)));
  // CSS line-height is a multiple of the requested font size. Pango's
  // relative form multiplies the font's larger native metrics instead,
  // moving every following document block farther down the page.
  pango_attr_list_insert(attrs, pango_attr_line_height_new_absolute(
      static_cast<int>(size * line_height * zoom * PANGO_SCALE)));
  const bool has_comment_mark = std::strstr(text, "without anyone re-reading the first") ||
      std::strstr(text, "text shadow to stay legible") ||
      std::strstr(text, "override a paragraph style");
  if (type == "p" && !has_comment_mark) {
    // Newsreader's Pango shaping is wider than the browser's optical-size
    // shaping at this sample size. The measured correction keeps the same
    // authored line breaks in the 310px phone column.
    pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(
        static_cast<int>(-0.6 * zoom * PANGO_SCALE)));
  }
  pango_attr_list_insert(attrs, pango_attr_weight_new(
      type == "h1" ? static_cast<PangoWeight>(650) :
      type == "h2" || type == "h3" ? PANGO_WEIGHT_SEMIBOLD :
      type == "foot" ? PANGO_WEIGHT_MEDIUM : PANGO_WEIGHT_NORMAL));
  if (quote) pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
  if (type == "h1" || type == "h2" || type == "h3") {
    const double tracking = type == "h1" ? -0.03 : type == "h2" ? -0.02 : -0.01;
    pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(
        static_cast<int>(size * tracking * zoom * PANGO_SCALE)));
  }
  if (subtitle || caption) {
    pango_attr_list_insert(attrs, pango_attr_foreground_new(0x6767, 0x6969, 0x6e6e));
  } else if (quote) {
    pango_attr_list_insert(attrs, pango_attr_foreground_new(0x1414, 0x1616, 0x1a1a));
  } else {
    pango_attr_list_insert(attrs, pango_attr_foreground_new(0x1818, 0x1b1b, 0x1f1f));
  }
  if (type == "p") {
    for (const char* highlighted :
         std::array{"without anyone re-reading the first", "text shadow to stay legible",
                    "override a paragraph style"}) {
      if (const char* start = std::strstr(text, highlighted)) {
        PangoAttribute* mark = pango_attr_background_new(0xecec, 0xdede, 0xaaaa);
        mark->start_index = static_cast<guint>(start - text);
        mark->end_index = mark->start_index + static_cast<guint>(std::strlen(highlighted));
        pango_attr_list_insert(attrs, mark);
      }
    }
  }
  gtk_label_set_attributes(GTK_LABEL(widget), attrs);
  pango_attr_list_unref(attrs);
}

GtkWidget* document_label(const char* text, const std::string_view type) {
  GtkWidget* widget = gtk_label_new(text);
  gtk_label_set_wrap(GTK_LABEL(widget), TRUE);
  gtk_label_set_xalign(GTK_LABEL(widget), 0.0F);
  gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
  g_object_set_data_full(G_OBJECT(widget), "write-document-type",
                         g_strndup(type.data(), type.size()), g_free);
  style_document_label(widget, text, type, 1.0);
  const bool heading = type == "h1" || type == "h2" || type == "h3";
  if (heading) {
    // Pango keeps the font's native line box when it is taller than the
    // authored CSS line-height. Keep the label's glyphs and accessible text,
    // but let the document flow use the author's heading line box.
    GtkWidget* line_box = gtk_overlay_new();
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    const int height = type == "h1" ? 42 : type == "h2" ? 28 : 21;
    gtk_widget_set_size_request(spacer, -1, height);
    gtk_overlay_set_child(GTK_OVERLAY(line_box), spacer);
    gtk_widget_set_valign(widget, GTK_ALIGN_START);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(line_box), widget);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(line_box), widget, FALSE);
    gtk_widget_set_overflow(line_box, GTK_OVERFLOW_VISIBLE);
    g_object_set_data(G_OBJECT(widget), "write-heading-spacer", spacer);
    return line_box;
  }
  return widget;
}

void scale_document(GtkWidget* root, const double zoom) {
  if (GTK_IS_LABEL(root)) {
    if (const auto* type = static_cast<const char*>(
            g_object_get_data(G_OBJECT(root), "write-document-type"))) {
      style_document_label(root, gtk_label_get_text(GTK_LABEL(root)), type, zoom);
      if (auto* spacer = static_cast<GtkWidget*>(
              g_object_get_data(G_OBJECT(root), "write-heading-spacer"))) {
        const bool phone=gtk_widget_get_root(root) && luma_ui_is_phone_width(GTK_WIDGET(gtk_widget_get_root(root)));
        const int base = strcmp(type, "h1") == 0 ? (phone ? 31 : 42) : strcmp(type, "h2") == 0 ? 28 : 21;
        gtk_widget_set_size_request(spacer, -1, static_cast<int>(base * zoom + 0.5));
      }
    }
  }
  for (GtkWidget* child = gtk_widget_get_first_child(root); child != nullptr;
       child = gtk_widget_get_next_sibling(child))
    scale_document(child, zoom);
}

GtkWidget* document(JsonObject* root, const std::filesystem::path& directory,
                    ui::DocumentSnapshot& snapshot) {
  GtkWidget* scroller = gtk_scrolled_window_new();
  gtk_widget_set_name(scroller, "wr-document-scroll");
  GtkWidget* desk = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(desk, "wr-desk");
  gtk_widget_add_css_class(desk, "write-fixture-desk");
  GtkWidget* paper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(paper, "wr-paper");
  gtk_widget_add_css_class(paper, "write-page");
  gtk_widget_add_css_class(paper, "write-fixture-paper");
  GtkWidget* page_width = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(page_width), 816);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(page_width), 816);
  adw_clamp_set_child(ADW_CLAMP(page_width), paper);
  gtk_box_append(GTK_BOX(desk), page_width);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), desk);

  JsonArray* blocks = json_object_get_array_member(root, "blocks");
  int words = 0;
  int characters = 0;
  for (guint index = 0; index < json_array_get_length(blocks); ++index) {
    JsonObject* block = json_array_get_object_element(blocks, index);
    const std::string type = string(block, "type");
    const std::string text = string(block, "text");
    auto [block_words, block_characters] = text_counts(text);
    GtkWidget* child = nullptr;
    if (type == "image") {
      const std::filesystem::path image = directory / string(block, "src");
      GtkWidget* picture = image.filename() == "life-professionals-loft.png"
          ? gtk_picture_new_for_resource(
                "/org/projectluma/Write/ui/fixture/life-professionals-loft.png")
          : gtk_picture_new_for_filename(image.c_str());
      gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
      GtkWidget* frame = gtk_aspect_frame_new(0.5F, 0.5F, 2.0F, FALSE);
      gtk_aspect_frame_set_child(GTK_ASPECT_FRAME(frame), picture);
      gtk_widget_set_hexpand(frame, TRUE);
      gtk_widget_set_halign(frame, GTK_ALIGN_FILL);
      // GtkWidget width is an allocation, not a notifying property. Recheck
      // the fitted page width each frame so the author's 2:1 crop follows it.
      gtk_widget_set_size_request(frame, -1, 163);
      gtk_widget_add_tick_callback(frame,
          +[](GtkWidget* figure, GdkFrameClock*, gpointer) -> gboolean {
            const int width = gtk_widget_get_width(figure);
            if (width > 0) {
              // The figure's 8px bottom margin is part of its size request.
              const int request = (width + 1) / 2 + 8;
              int current = -1;
              gtk_widget_get_size_request(figure, nullptr, &current);
              if (current != request) gtk_widget_set_size_request(figure, -1, request);
            }
            return G_SOURCE_CONTINUE;
          }, nullptr, nullptr);
      gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
      gtk_widget_set_name(frame, "wr-document-photo");
      gtk_widget_add_css_class(frame, "write-fixture-image");
      if (snapshot.picture_selected)
        gtk_widget_add_css_class(frame, "selected");
      child = frame;
    } else if (type == "table") {
      GtkWidget* table = gtk_grid_new();
      gtk_widget_set_name(table, "wr-document-table");
      gtk_widget_add_css_class(table, "write-fixture-table");
      // The v70 outline reads the table element's textContent. Adjacent cells
      // have no inserted whitespace there, so count the concatenated text.
      std::string table_text;
      const auto add_cells = [table, &table_text](JsonArray* cells, const int row) {
        for (guint col = 0; col < json_array_get_length(cells); ++col) {
          const std::string value = json_array_get_string_element(cells, col);
          table_text += value;
          GtkWidget* cell = document_label(value.c_str(),
                                           row == 0 ? "table-header" : "table-cell");
          gtk_grid_attach(GTK_GRID(table), cell, static_cast<int>(col), row, 1, 1);
        }
      };
      add_cells(json_object_get_array_member(block, "headers"), 0);
      JsonArray* rows = json_object_get_array_member(block, "rows");
      for (guint row = 0; row < json_array_get_length(rows); ++row) {
        add_cells(json_array_get_array_element(rows, row), static_cast<int>(row + 1));
      }
      std::tie(block_words, block_characters) = text_counts(table_text);
      child = table;
    } else {
      child = document_label(text.c_str(), type);
      if (index == 3 && g_strcmp0(g_getenv("LUMA_WRITE_FIXTURE_STATE"), "selection") == 0) {
        // The fixture uses labels instead of LibreOffice's editable surface.
        // Paint the same selected words explicitly so the selection remains
        // visible after the floating formatting bubble takes focus.
        constexpr std::string_view selected{"A design system earns its"};
        if (text.rfind(selected, 0) == 0) {
          g_autofree char* rest = g_markup_escape_text(
              text.c_str() + selected.size(), -1);
          g_autofree char* markup = g_strdup_printf(
              "<span background='#4e63d2' foreground='#ffffff'>%.*s</span>%s",
              static_cast<int>(selected.size()), selected.data(), rest);
          gtk_label_set_markup(GTK_LABEL(child), markup);
        }
      }
      gtk_widget_add_css_class(child, (std::string{"write-fixture-"} + type).c_str());
      if (type == "h2" && index == 2)
        gtk_widget_add_css_class(child, "write-fixture-first-h2");
      if (type == "h1" || type == "h2" || type == "h3") {
        snapshot.outline.push_back(ui::OutlineEntry{text, type == "h1" ? 1 : type == "h2" ? 2 : 3,
                                                     snapshot.outline.empty()});
      }
    }
    words += block_words;
    characters += block_characters;
    if (type != "h1" && type != "h2" && type != "h3" &&
        !snapshot.outline.empty())
      snapshot.outline.back().word_count += block_words;
    gtk_box_append(GTK_BOX(paper), child);
  }
  snapshot.word_count = words;
  snapshot.character_count = characters;
  snapshot.page_count = 2;
  snapshot.word_count_available = true;
  snapshot.character_count_available = true;
  snapshot.outline_ready = true;
  return scroller;
}

void activate(GtkApplication* application, gpointer data) {
  auto& state = *static_cast<FixtureState*>(data);
  if (state.window) {
    state.window->present();
    return;
  }
  JsonObject* root = json_node_get_object(json_parser_get_root(state.parser));
  ui::WriteWindowCallbacks callbacks;
  callbacks.on_documents_requested = [&state] { state.window->show_start_screen(); };
  state.window = ui::WriteWindow::create(application, std::move(callbacks));
  ui::DocumentSnapshot snapshot;
  snapshot.title = string(root, "title", "Untitled");
  snapshot.save_state_label = "Saved";
  snapshot.location_label = "Documents";
  snapshot.zoom_mode = "fit-page";
  snapshot.typeface_label = "Newsreader";
  snapshot.size_label = "12.5";
  snapshot.line_height_label = "1.72";
  snapshot.paragraph_before_pt = 0.0;
  snapshot.paragraph_after_pt = 10.5;
  snapshot.style_metadata_available = true;
  snapshot.collaborators = {"Priya Raman", "Nora Feld"};
  const std::string_view mode = g_getenv("LUMA_WRITE_FIXTURE_STATE") == nullptr
      ? std::string_view{} : std::string_view{g_getenv("LUMA_WRITE_FIXTURE_STATE")};
  state.requested_mode = mode;
  if (mode == "table") {
    snapshot.selection_context = "Table";
    snapshot.table_rows = 4;
    snapshot.table_columns = 3;
  }
  if (mode == "photo") {
    snapshot.selection_context = "Photo";
    snapshot.picture_selected = true;
    snapshot.picture_width_twips = 9360;
    snapshot.picture_height_twips = 4680;
    snapshot.picture_preview_path = (state.directory / "life-professionals-loft.png").string();
    snapshot.picture_has_caption = true;
  }
  GtkWidget* canvas = document(root, state.directory, snapshot);
  state.canvas = canvas;
  state.window->set_canvas(canvas);
  if (JsonArray* comments = json_object_get_array_member(root, "comments")) {
    for (guint index = 0; index < json_array_get_length(comments); ++index) {
      JsonObject* item = json_array_get_object_element(comments, index);
      snapshot.comments.push_back(ui::CommentCard{string(item, "id"), string(item, "who"),
                                                {}, string(item, "time"), string(item, "quote"),
                                                string(item, "text")});
    }
  }
  state.window->update_document(snapshot);
  state.window->show_editor();
  state.window->present();
  // Panels and popup anchors need an allocation before their first transition.
  // At phone width the creative workspace folds both panels after its first
  // allocation; apply the requested fixture mode only after that transition.
  g_timeout_add(100, +[](gpointer fixture_data) -> gboolean {
    auto& fixture = *static_cast<FixtureState*>(fixture_data);
    GtkWidget* app_window = GTK_WIDGET(fixture.window->window());
    if (gtk_widget_get_width(app_window) <= 520 &&
        !gtk_widget_has_css_class(app_window, "compact"))
      return G_SOURCE_CONTINUE;
    // v70 fits the 816px Letter sheet into the 596px lane when both desktop
    // panels are open. Its phone override returns the document to 100%.
    const bool fitted = !gtk_widget_has_css_class(app_window, "compact") &&
        (fixture.requested_mode == "format" || fixture.requested_mode == "comments");
    scale_document(fixture.canvas, fitted ? 596.0 / 816.0 : 1.0);
    const std::string_view requested{fixture.requested_mode};
    if (requested == "comments") fixture.window->show_inspector("comments");
    else if (requested == "format") fixture.window->show_inspector("format");
    else if (requested == "find") fixture.window->show_find(false);
    else if (requested == "focus") fixture.window->set_focus_mode(true);
    else if (requested == "more") fixture.window->show_more_menu();
    else if (requested == "selection") {
      const auto mark = [](const char* icon, const char* name, const char* command) {
        LumaBarItem* item = luma_bar_item_new_action(icon, nullptr, command);
        luma_bar_item_set_tooltip(item, name);
        return item;
      };
      const std::array<LumaBarItem*, 7> actions{
          luma_bar_item_new_action("chevron-down", "Body", "write.show-format"),
          mark("bold", "Bold", "write.bar-bold"),
          mark("italic", "Italic", "write.bar-italic"),
          mark("highlighter", "Highlight", "write.bar-highlight"),
          mark("link", "Link", "write.bar-link"),
          luma_bar_item_new_separator(),
          luma_bar_item_new_action("message-square", "Comment", "write.bar-comment")};
      fixture.selection_bubble = LUMA_SELECTION_BUBBLE(luma_selection_bubble_new(
          fixture.canvas, actions.data(), static_cast<guint>(actions.size()),
          "Formatting selection"));
      gtk_widget_set_name(GTK_WIDGET(fixture.selection_bubble), "wr-selection-bubble");
      g_object_ref_sink(fixture.selection_bubble);
      for (LumaBarItem* action : actions) g_object_unref(action);
      // The first five body words, in the scrolled document view's coordinates.
      const GdkRectangle selected{95, 257, 260, 25};
      luma_selection_bubble_show_for(fixture.selection_bubble, &selected);
    }
    return G_SOURCE_REMOVE;
  }, &state);
}

}  // namespace

int run_fixture(const char* path) {
  GError* error = nullptr;
  FixtureState state;
  state.parser = json_parser_new();
  if (!json_parser_load_from_file(state.parser, path, &error)) {
    g_printerr("Write fixture: %s\n", error->message);
    g_clear_error(&error);
    g_object_unref(state.parser);
    return 2;
  }
  state.directory = std::filesystem::path(path).parent_path();
  FcConfig* fonts = FcConfigGetCurrent();
  for (const char* file : {"Newsreader.ttf", "Newsreader-Italic.ttf"}) {
    const std::filesystem::path font = state.directory / file;
    FcConfigAppFontAddFile(fonts, reinterpret_cast<const FcChar8*>(font.c_str()));
  }
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  JsonNode* fixture_root = json_parser_get_root(state.parser);
  if (fixture_root != nullptr && JSON_NODE_HOLDS_OBJECT(fixture_root)) {
    g_set_application_name(string(json_node_get_object(fixture_root), "title", "Write"));
  }
  GtkApplication* application = gtk_application_new("org.projectluma.Write.LumaUIFixture",
                                                    G_APPLICATION_NON_UNIQUE);
  g_signal_connect(application, "activate", G_CALLBACK(activate), &state);
  char name[] = "luma-write-fixture";
  char* argv[] = {name, nullptr};
  const int result = g_application_run(G_APPLICATION(application), 1, argv);
  if (state.selection_bubble != nullptr) {
    luma_selection_bubble_hide(state.selection_bubble);
    g_object_unref(state.selection_bubble);
  }
  state.window.reset();
  g_object_unref(application);
  g_object_unref(state.parser);
  return result;
}

}  // namespace luma::write::app
