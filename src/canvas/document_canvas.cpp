#include "document_canvas.hpp"
#include "ui/bar_menu.hpp"
#include "ui/comment_data.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace luma::write::canvas {
namespace {

constexpr int kLogicalTileEdge = 512;
constexpr std::uint32_t kLeftButton = 1;
constexpr std::uint32_t kShiftModifier = 0x1000;
constexpr std::uint32_t kControlModifier = 0x2000;
constexpr std::uint32_t kAltModifier = 0x4000;
constexpr std::uint32_t kMetaModifier = 0x8000;
constexpr gsize kPasteChunkBytes = 64U * 1024U;
const char* kPasteMimeTypes[] = {"text/plain;charset=utf-8", "text/plain",
                                 nullptr};

[[nodiscard]] bool is_primary_shortcut(const GdkModifierType state) noexcept {
  constexpr auto kPrimaryModifiers = static_cast<GdkModifierType>(
      GDK_CONTROL_MASK | GDK_META_MASK | GDK_SUPER_MASK);
  constexpr auto kDisallowedModifiers =
      static_cast<GdkModifierType>(GDK_SHIFT_MASK | GDK_ALT_MASK);
  return (state & kPrimaryModifiers) != 0 &&
         (state & kDisallowedModifiers) == 0;
}

[[nodiscard]] std::uint32_t engine_modifiers(const GdkModifierType state) noexcept {
  std::uint32_t result = 0;
  if ((state & GDK_SHIFT_MASK) != 0) {
    result |= kShiftModifier;
  }
  if ((state & GDK_CONTROL_MASK) != 0) {
    result |= kControlModifier;
  }
  if ((state & GDK_ALT_MASK) != 0) {
    result |= kAltModifier;
  }
  if ((state & GDK_META_MASK) != 0 || (state & GDK_SUPER_MASK) != 0) {
    result |= kMetaModifier;
  }
  return result;
}

[[nodiscard]] std::uint32_t engine_key(const guint keyval) noexcept {
  switch (keyval) {
    case GDK_KEY_Down:
      return 1024;
    case GDK_KEY_Up:
      return 1025;
    case GDK_KEY_Left:
      return 1026;
    case GDK_KEY_Right:
      return 1027;
    case GDK_KEY_Home:
      return 1028;
    case GDK_KEY_End:
      return 1029;
    case GDK_KEY_Page_Up:
      return 1030;
    case GDK_KEY_Page_Down:
      return 1031;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      return 1280;
    case GDK_KEY_Escape:
      return 1281;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
      return 1282;
    case GDK_KEY_BackSpace:
      return 1283;
    case GDK_KEY_space:
      return 1284;
    case GDK_KEY_Insert:
      return 1285;
    case GDK_KEY_Delete:
      return 1286;
    default:
      return 0;
  }
}

void swap_red_blue(engine::RenderedTile& tile) {
  if (tile.format != engine::PixelFormat::rgba8) {
    return;
  }
  for (std::size_t index = 0; index + 3 < tile.pixels.size(); index += 4) {
    std::swap(tile.pixels[index], tile.pixels[index + 2]);
  }
  tile.format = engine::PixelFormat::bgra8;
}

}  // namespace

struct DocumentCanvas::ClipboardBridge {
  std::mutex mutex;
  DocumentCanvas* target{nullptr};
  bool paste_pending{false};
};

struct DocumentCanvas::PasteRequest {
  PasteRequest(std::shared_ptr<ClipboardBridge> clipboard_bridge,
               GCancellable* operation_cancellable)
      : bridge(std::move(clipboard_bridge)),
        cancellable(G_CANCELLABLE(g_object_ref(operation_cancellable))) {}

  ~PasteRequest() {
    if (stream != nullptr) {
      g_object_unref(stream);
    }
    g_object_unref(cancellable);
    std::lock_guard lock(bridge->mutex);
    bridge->paste_pending = false;
  }

  std::shared_ptr<ClipboardBridge> bridge;
  GCancellable* cancellable{nullptr};
  GInputStream* stream{nullptr};
  std::string mime_type;
  std::vector<std::byte> bytes;
};

DocumentCanvas::DocumentCanvas(engine::DocumentEngine& document_engine)
    : engine_(document_engine),
      clipboard_bridge_(std::make_shared<ClipboardBridge>()),
      paste_cancellable_(g_cancellable_new()) {
  clipboard_bridge_->target = this;
  image_bridge_ = std::make_shared<ClipboardBridgeRef>();
  image_bridge_->target = this;
  drawing_area_ = gtk_drawing_area_new();
  gtk_widget_add_css_class(drawing_area_, "document-canvas");
  gtk_widget_set_focusable(drawing_area_, TRUE);
  gtk_widget_set_hexpand(drawing_area_, TRUE);
  gtk_widget_set_vexpand(drawing_area_, TRUE);
  gtk_widget_set_overflow(drawing_area_, GTK_OVERFLOW_HIDDEN);
  gtk_accessible_update_property(GTK_ACCESSIBLE(drawing_area_), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Document editing canvas", -1);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area_), &DocumentCanvas::draw, this,
                                 nullptr);

  // The kit positions this shared bubble above the engine's actual selected
  // range; Write supplies only commands and the range's document coordinates.
  const auto selection_action = [](const char* icon, const char* command, const char* name) {
    LumaBarItem* item = luma_bar_item_new_action(icon, nullptr, command);
    luma_bar_item_set_tooltip(item, name);
    return item;
  };
  auto* style_item = luma_bar_item_new_action("pilcrow", "Body", "write.choose-style");
  auto* size_item = luma_bar_item_new_action("type", "12", "write.choose-font-size");
  const std::array<LumaBarItem*, 9> selection_actions{
      style_item, size_item,
      selection_action("bold", "write.bar-bold", "Bold"),
      selection_action("italic", "write.bar-italic", "Italic"),
      selection_action("underline", "write.bar-underline", "Underline"),
      selection_action("highlighter", "write.bar-highlight", "Highlight"),
      selection_action("link", "write.bar-link", "Link"),
      luma_bar_item_new_separator(),
      selection_action("message-square", "write.bar-comment", "Comment")};
  selection_bubble_ = LUMA_SELECTION_BUBBLE(luma_selection_bubble_new(
      drawing_area_, selection_actions.data(), static_cast<guint>(selection_actions.size()),
      "Formatting selection"));
  g_object_ref_sink(selection_bubble_);
  // The bubble's action registry requires glyph identities. Keep those action
  // items, then give their buttons the same BarMenu composition as the main bar.
  for (GtkWidget* control = gtk_widget_get_first_child(GTK_WIDGET(selection_bubble_));
       control != nullptr; control = gtk_widget_get_next_sibling(control)) {
    if (!GTK_IS_BUTTON(control)) continue;
    const char* action = gtk_actionable_get_action_name(GTK_ACTIONABLE(control));
    if (g_strcmp0(action, "write.choose-style") == 0)
      ui::configure_bar_menu(control, "Body", "Paragraph style");
    if (g_strcmp0(action, "write.choose-font-size") == 0)
      ui::configure_bar_menu(control, "12", "Font size");
  }
  for (LumaBarItem* item : selection_actions) g_object_unref(item);

  page_layer_ = gtk_fixed_new();
  gtk_widget_add_css_class(page_layer_, "document-page-layer");
  gtk_widget_set_can_target(page_layer_, FALSE);
  GtkWidget* layers = gtk_overlay_new();
  layers_ = layers;
  // Luma's ActionCenter changes this end clearance as its row moves. Fit page
  // must use the unobstructed height, including after a bar resize or fold.
  g_signal_connect_swapped(layers, "notify::margin-bottom",
                           G_CALLBACK(+[](gpointer data) {
                             static_cast<DocumentCanvas*>(data)->apply_fit_zoom();
                           }), this);
  gtk_overlay_set_child(GTK_OVERLAY(layers), page_layer_);
  gtk_overlay_add_overlay(GTK_OVERLAY(layers), drawing_area_);
  gtk_overlay_set_measure_overlay(GTK_OVERLAY(layers), drawing_area_, TRUE);
  g_signal_connect_swapped(drawing_area_, "resize",
                           G_CALLBACK(+[](gpointer data) {
                             static_cast<DocumentCanvas*>(data)->queue_page_layout();
                           }),
                           this);

  scroller_ = gtk_scrolled_window_new();
  gtk_widget_add_css_class(scroller_, "document-scroller");
  // The page is the scrolling surface. External policy hides scrollbar chrome
  // while preserving both adjustments and wheel/trackpad/keyboard scrolling.
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_EXTERNAL,
                                 GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), layers);
  gtk_accessible_update_property(GTK_ACCESSIBLE(scroller_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Document editing canvas", -1);

  GtkEventController* scroll = gtk_event_controller_scroll_new(
      GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
  g_signal_connect(scroll, "scroll", G_CALLBACK(+[](
      GtkEventControllerScroll* controller, double, double dy, gpointer data) -> gboolean {
    const auto state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    if ((state & GDK_CONTROL_MASK) == 0 || dy == 0.0) return FALSE;
    auto* self = static_cast<DocumentCanvas*>(data);
    // Wheel steps and smooth trackpad deltas share the canvas's bounded zoom
    // path, which keeps the viewport centre anchored and updates the label.
    self->set_zoom(self->zoom_ * std::pow(1.1, -dy));
    return TRUE;
  }), this);
  gtk_widget_add_controller(scroller_, scroll);

  GtkGesture* click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "pressed", G_CALLBACK(pointer_pressed), this);
  g_signal_connect(click, "released", G_CALLBACK(pointer_released), this);
  gtk_widget_add_controller(drawing_area_, GTK_EVENT_CONTROLLER(click));

  GtkGesture* secondary = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
  g_signal_connect(secondary, "pressed", G_CALLBACK(secondary_pressed), this);
  gtk_widget_add_controller(drawing_area_, GTK_EVENT_CONTROLLER(secondary));

  GtkEventController* motion = gtk_event_controller_motion_new();
  g_signal_connect(motion, "motion", G_CALLBACK(pointer_moved), this);
  gtk_widget_add_controller(drawing_area_, motion);

  input_method_ = gtk_im_multicontext_new();
  gtk_im_context_set_client_widget(input_method_, drawing_area_);
  gtk_im_context_set_use_preedit(input_method_, TRUE);
  g_signal_connect(input_method_, "commit", G_CALLBACK(input_committed), this);
  g_signal_connect(input_method_, "preedit-changed", G_CALLBACK(preedit_changed), this);

  GtkEventController* key_controller = gtk_event_controller_key_new();
  gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key_controller), input_method_);
  g_signal_connect(key_controller, "key-pressed", G_CALLBACK(key_pressed), this);
  g_signal_connect(key_controller, "key-released", G_CALLBACK(key_released), this);
  gtk_widget_add_controller(drawing_area_, key_controller);
  install_drop_target();

  GtkEventController* focus_controller = gtk_event_controller_focus_new();
  g_signal_connect(focus_controller, "enter", G_CALLBACK(focus_entered), this);
  g_signal_connect(focus_controller, "leave", G_CALLBACK(focus_left), this);
  gtk_widget_add_controller(drawing_area_, focus_controller);

  for (GtkAdjustment* adjustment :
       {gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)),
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_))}) {
    g_signal_connect_swapped(
        adjustment, "notify::page-size",
        G_CALLBACK(+[](gpointer data) {
          static_cast<DocumentCanvas*>(data)->apply_fit_zoom();
        }),
        this);
  }
  g_signal_connect(
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_)),
      "value-changed", G_CALLBACK(+[](GtkAdjustment*, gpointer data) {
        auto* self = static_cast<DocumentCanvas*>(data);
        self->queue_page_layout();
        if (self->selection_bubble_ != nullptr)
          luma_selection_bubble_hide(self->selection_bubble_);
        if (self->zoom_listener_) {
          // Scrolling changes the page being read; listeners refresh it.
          self->zoom_listener_(self->zoom_);
        }
      }),
      this);

  update_content_size();
}

DocumentCanvas::~DocumentCanvas() {
  if (selection_bubble_source_ != 0) {
    g_source_remove(selection_bubble_source_);
    selection_bubble_source_ = 0;
  }
  if (selection_bubble_ != nullptr) {
    luma_selection_bubble_hide(selection_bubble_);
    if (GtkWidget* parent = gtk_widget_get_parent(GTK_WIDGET(selection_bubble_));
        parent != nullptr && GTK_IS_OVERLAY(parent))
      gtk_overlay_remove_overlay(GTK_OVERLAY(parent), GTK_WIDGET(selection_bubble_));
    g_object_unref(selection_bubble_);
    selection_bubble_ = nullptr;
  }
  if (context_popover_ != nullptr) {
    luma_floating_menu_close(LUMA_FLOATING_MENU(context_popover_));
    g_object_unref(context_popover_);
    context_popover_ = nullptr;
  }
  if (context_anchor_ != nullptr) {
    gtk_overlay_remove_overlay(GTK_OVERLAY(layers_), context_anchor_);
    context_anchor_ = nullptr;
  }
  if (page_layout_source_ != 0) {
    g_source_remove(page_layout_source_);
    page_layout_source_ = 0;
  }
  {
    std::lock_guard lock(clipboard_bridge_->mutex);
    clipboard_bridge_->target = nullptr;
  }
  image_bridge_->target = nullptr;
  remove_image_directory();
  if (paste_cancellable_ != nullptr) {
    g_cancellable_cancel(paste_cancellable_);
    g_object_unref(paste_cancellable_);
    paste_cancellable_ = nullptr;
  }
  if (input_method_ != nullptr && !input_method_detached_) {
    gtk_im_context_set_client_widget(input_method_, nullptr);
  }
  if (input_method_ != nullptr) {
    g_object_unref(input_method_);
  }
}

void DocumentCanvas::prepare_for_window_destruction() {
  if (input_method_ == nullptr || input_method_detached_) {
    return;
  }
  gtk_im_context_focus_out(input_method_);
  gtk_im_context_set_client_widget(input_method_, nullptr);
  input_method_detached_ = true;
}

void DocumentCanvas::set_zoom(const double zoom) {
  zoom_mode_ = ZoomMode::fixed;
  apply_zoom(zoom, false);
}

void DocumentCanvas::apply_zoom(const double zoom, const bool from_fit) {
  const double next = std::clamp(zoom, 0.25, 4.0);
  if (std::abs(next - zoom_) < 0.0001) {
    if (!from_fit && zoom_listener_) {
      zoom_listener_(zoom_);
    }
    return;
  }
  // Keep the point at the centre of the viewport where it is, so zooming does
  // not throw the reader to another part of the document.
  GtkAdjustment* vertical =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
  const double page_size = gtk_adjustment_get_page_size(vertical);
  const double anchor =
      (gtk_adjustment_get_value(vertical) + page_size / 2.0 - document_margin_y_) /
      std::max(zoom_, 0.01);
  zoom_ = next;
  tile_cache_.clear();
  update_content_size();
  gtk_adjustment_set_upper(
      vertical, std::max(gtk_adjustment_get_upper(vertical),
                         anchor * zoom_ + page_size));
  gtk_adjustment_set_value(
      vertical, std::max(0.0, anchor * zoom_ + document_margin_y_ - page_size / 2.0));
  gtk_widget_queue_draw(drawing_area_);
  if (zoom_listener_) {
    zoom_listener_(zoom_);
  }
}

void DocumentCanvas::set_zoom_mode(const ZoomMode mode) {
  zoom_mode_ = mode;
  apply_fit_zoom();
}

void DocumentCanvas::update_formatting_controls(const std::string& style,
                                                const std::string& size) {
  const auto visit = [&](const auto& self, GtkWidget* widget, const char* text) -> void {
    if (GTK_IS_ACTIONABLE(widget)) {
      const char* action = gtk_actionable_get_action_name(GTK_ACTIONABLE(widget));
      if (g_strcmp0(action, "write.choose-style") == 0) text = style.c_str();
      if (g_strcmp0(action, "write.choose-font-size") == 0) text = size.c_str();
    }
    if (GTK_IS_LABEL(widget) && text != nullptr &&
        gtk_widget_has_css_class(widget, "wr-bar-menu-label"))
      gtk_label_set_text(GTK_LABEL(widget), *text == '\0' ? "—" : text);
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) self(self, child, text);
  };
  visit(visit, GTK_WIDGET(selection_bubble_), nullptr);
  for (const auto& [glyph, command] :
       {std::pair{"bold", engine::EngineCommand::bold},
        std::pair{"italic", engine::EngineCommand::italic},
        std::pair{"underline", engine::EngineCommand::underline}}) {
    const auto state = engine_.command_state(command);
    luma_selection_bubble_set_active(selection_bubble_, glyph,
                                    state.kind == engine::CommandStateKind::on);
    // Mixed marks need their own visual and accessible state in the bubble.
    for (GtkWidget* control = gtk_widget_get_first_child(GTK_WIDGET(selection_bubble_));
         control != nullptr; control = gtk_widget_get_next_sibling(control)) {
      if (!GTK_IS_ACTIONABLE(control)) continue;
      const std::string action = std::string{"write.bar-"} + glyph;
      if (g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(control)), action.c_str()) != 0) continue;
      if (state.kind == engine::CommandStateKind::mixed) gtk_widget_add_css_class(control, "mixed");
      else gtk_widget_remove_css_class(control, "mixed");
      const auto pressed = state.kind == engine::CommandStateKind::mixed ? GTK_ACCESSIBLE_TRISTATE_MIXED :
          state.kind == engine::CommandStateKind::on ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE;
      gtk_accessible_update_state(GTK_ACCESSIBLE(control), GTK_ACCESSIBLE_STATE_PRESSED, pressed, -1);
    }
  }
}

void DocumentCanvas::set_page_parts(const bool header, const bool footer,
                                    const std::int64_t margin_twips) {
  if (header_on_ == header && footer_on_ == footer &&
      (margin_twips == 0 || page_margin_twips_ == margin_twips))
    return;
  header_on_ = header;
  footer_on_ = footer;
  if (margin_twips > 0) page_margin_twips_ = margin_twips;
  if (!header) header_height_twips_ = 300;
  if (!footer) footer_height_twips_ = 300;
  gtk_widget_queue_draw(drawing_area_);
}

void DocumentCanvas::apply_fit_zoom() {
  if (zoom_mode_ == ZoomMode::fixed || !engine_.has_document()) {
    return;
  }
  const auto status = engine_.status();
  std::int64_t page_width = 0;
  std::int64_t page_height = 0;
  for (const auto& page : status.page_areas_twips) {
    page_width = std::max(page_width, page.width);
    page_height = std::max(page_height, page.height);
  }
  if (page_width <= 0) {
    page_width = std::max<std::int64_t>(status.extent_twips.width, 11906);
  }
  if (page_height <= 0) {
    page_height = 16838;
  }
  const double view_width = gtk_adjustment_get_page_size(
      gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller_)));
  const double view_height = gtk_adjustment_get_page_size(
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_))) -
      gtk_widget_get_margin_bottom(layers_);
  if (view_width < 120.0) {
    return;
  }
  const double unit = engine::CoordinateSpace::kCssPixelsPerInch /
                      engine::CoordinateSpace::kTwipsPerInch;
  const double width_fit = (view_width - 2.0 * document_margin_x_) /
                           (static_cast<double>(page_width) * unit);
  double zoom = width_fit;
  if (zoom_mode_ == ZoomMode::fit_page && view_height > 120.0) {
    zoom = std::min(width_fit, (view_height - 2.0 * document_margin_y_) /
                                   (static_cast<double>(page_height) * unit));
  } else if (zoom_mode_ == ZoomMode::automatic) {
    zoom = std::min(kAutomaticZoom, width_fit);
  }
  apply_zoom(zoom, true);
}

void DocumentCanvas::set_page_appearance(const PageAppearance& appearance) {
  if (appearance == appearance_) {
    return;
  }
  const bool page_changed = appearance.moon != appearance_.moon;
  appearance_ = appearance;
  if (page_changed) {
    tile_cache_.clear();
  }
  if (appearance_.moon) {
    gtk_widget_add_css_class(page_layer_, "moon");
  } else {
    gtk_widget_remove_css_class(page_layer_, "moon");
  }
  gtk_widget_queue_draw(drawing_area_);
}

void DocumentCanvas::scroll_to_twips(const std::int64_t y_twips) {
  const auto space = coordinates();
  GtkAdjustment* vertical =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
  gtk_adjustment_set_value(
      vertical, std::max(0.0, space.twips_y_to_pixels(y_twips) - 24.0));
}

int DocumentCanvas::current_page() const {
  const auto status = engine_.status();
  if (status.page_areas_twips.empty()) {
    return 1;
  }
  const auto space = coordinates();
  GtkAdjustment* vertical =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
  const double middle = gtk_adjustment_get_value(vertical) +
                        gtk_adjustment_get_page_size(vertical) / 3.0;
  const auto y = space.pixel_y_to_twips(middle);
  int page_number = 1;
  for (std::size_t index = 0; index < status.page_areas_twips.size(); ++index) {
    if (status.page_areas_twips[index].y <= y) {
      page_number = static_cast<int>(index) + 1;
    }
  }
  return page_number;
}

void DocumentCanvas::refresh_document() {
  refresh_comments();
  tile_cache_.clear();
  cursor_twips_.reset();
  selection_twips_.clear();
  graphic_selection_twips_.reset();
  update_content_size();
  apply_fit_zoom();
  gtk_widget_queue_draw(drawing_area_);
  gtk_widget_grab_focus(drawing_area_);
}

void DocumentCanvas::handle_engine_event(const engine::EngineEvent& event) {
  switch (event.kind) {
    case engine::EngineEventKind::tiles_invalidated:
      tile_cache_.invalidate(event.area_twips, event.revision);
      gtk_widget_queue_draw(drawing_area_);
      break;
    case engine::EngineEventKind::cursor_changed:
      cursor_twips_ = event.area_twips;
      if (cursor_twips_) {
        for (const auto& page : engine_.status().page_areas_twips) {
          const auto& cursor = *cursor_twips_;
          if (cursor.y < page.y || cursor.y >= page.y + page.height) continue;
          const auto header_top = page.y + page_margin_twips_;
          const auto footer_bottom = page.y + page.height - page_margin_twips_;
          // Follow growing header/footer text; the engine leaves a 5mm gap
          // before the body, so body caret moves do not change the guides.
          if (header_on_ && cursor.y >= header_top &&
              cursor.y <= header_top + header_height_twips_ + 140)
            header_height_twips_ = std::max<std::int64_t>(300, cursor.y + cursor.height - header_top + 80);
          if (footer_on_ && cursor.y >= footer_bottom - footer_height_twips_ - 140 &&
              cursor.y < footer_bottom)
            footer_height_twips_ = std::max<std::int64_t>(300, footer_bottom - cursor.y + 80);
        }
      }
      update_input_cursor();
      gtk_widget_queue_draw(drawing_area_);
      break;
    case engine::EngineEventKind::cursor_visibility_changed:
      cursor_visible_ = event.numeric_value != 0;
      gtk_widget_queue_draw(drawing_area_);
      break;
    case engine::EngineEventKind::text_selection_changed:
      selection_twips_ = event.areas_twips;
      if (selection_bubble_source_ != 0) {
        g_source_remove(selection_bubble_source_);
        selection_bubble_source_ = 0;
      }
      luma_selection_bubble_hide(selection_bubble_);
      if (!selection_twips_.empty()) {
        selection_bubble_source_ = g_timeout_add(200, +[](gpointer data) -> gboolean {
          auto* self = static_cast<DocumentCanvas*>(data);
          self->selection_bubble_source_ = 0;
          if (self->selection_twips_.empty() ||
              !gtk_widget_get_mapped(self->drawing_area_)) return G_SOURCE_REMOVE;
          const engine::Rect& selected = self->selection_twips_.front();
          const engine::CoordinateSpace space = self->coordinates();
          const GdkRectangle bounds{
              static_cast<int>(std::lround(space.twips_x_to_pixels(selected.x))),
              static_cast<int>(std::lround(space.twips_y_to_pixels(selected.y))),
              std::max(1, static_cast<int>(std::lround(
                              static_cast<double>(selected.width) * space.pixels_per_twip()))),
              std::max(1, static_cast<int>(std::lround(
                              static_cast<double>(selected.height) * space.pixels_per_twip())))};
          luma_selection_bubble_show_for(self->selection_bubble_, &bounds);
          return G_SOURCE_REMOVE;
        }, this);
      }
      gtk_widget_queue_draw(drawing_area_);
      break;
    case engine::EngineEventKind::graphic_selection_changed:
      graphic_selection_twips_ = event.area_twips;
      gtk_widget_queue_draw(drawing_area_);
      break;
    case engine::EngineEventKind::context_menu:
      show_context_menu(event.text);
      break;
    case engine::EngineEventKind::document_extent_changed:
      update_content_size();
      tile_cache_.clear();
      apply_fit_zoom();
      gtk_widget_queue_draw(drawing_area_);
      break;
    default:
      break;
  }
}

engine::CoordinateSpace DocumentCanvas::coordinates() const {
  const auto status = engine_.status();
  const engine::CoordinateSpace local(zoom_, gtk_widget_get_scale_factor(drawing_area_));
  const double document_width =
      static_cast<double>(status.extent_twips.width) * local.pixels_per_twip();
  const double allocation_width = static_cast<double>(gtk_widget_get_width(drawing_area_));
  const double origin_x = std::floor(std::max(
      static_cast<double>(document_margin_x_),
      (allocation_width - comment_gutter_ - document_width) / 2.0));
  return engine::CoordinateSpace(zoom_, gtk_widget_get_scale_factor(drawing_area_), origin_x,
                                 static_cast<double>(document_margin_y_));
}

void DocumentCanvas::update_content_size() {
  const auto status = engine_.status();
  const engine::CoordinateSpace local(zoom_, gtk_widget_get_scale_factor(drawing_area_));
  const int width = static_cast<int>(std::ceil(
      static_cast<double>(std::max<std::int64_t>(status.extent_twips.width, 11906)) *
      local.pixels_per_twip()));
  const int height = static_cast<int>(std::ceil(
      static_cast<double>(std::max<std::int64_t>(status.extent_twips.height, 16838)) *
      local.pixels_per_twip()));
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(drawing_area_),
                                     width + document_margin_x_ * 2 + comment_gutter_);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(drawing_area_),
                                      height + document_margin_y_ * 2);
  queue_page_layout();
}

void DocumentCanvas::draw(GtkDrawingArea*, cairo_t* context, int width, int height,
                          gpointer data) {
  static_cast<DocumentCanvas*>(data)->draw_impl(context, width, height);
}

void DocumentCanvas::draw_impl(cairo_t* context, [[maybe_unused]] const int width,
                               [[maybe_unused]] const int height) {
  // The ground is the host's CSS background (@write_canvas), which follows
  // the treatment; a fixed fill here kept the ground light inside a dark
  // window. Only the page and its shadow are painted.
  if (!engine_.has_document()) {
    return;
  }

  double clip_x1 = 0;
  double clip_y1 = 0;
  double clip_x2 = 0;
  double clip_y2 = 0;
  cairo_clip_extents(context, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
  const auto status = engine_.status();
  const auto coordinate_space = coordinates();
  // The pages themselves (paper and shadow) are widgets beneath this area,
  // styled by the toolkit's island rule. The engine paints the space between pages too; the canvas owns that
  // space (its ground and the paper shadows), so tiles land on pages only.
  cairo_save(context);
  clip_to_pages(context, coordinate_space, status);
  draw_tiles(context, coordinate_space, status, clip_x1, clip_y1, clip_x2, clip_y2);
  cairo_restore(context);
  draw_overlays(context, coordinate_space);

  const auto visible = coordinate_space.pixel_rect_to_twips(
      clip_x1, clip_y1, std::max(0.0, clip_x2 - clip_x1), std::max(0.0, clip_y2 - clip_y1));
  static_cast<void>(engine_.set_visible_area(visible));
}

DocumentCanvas::PageBox DocumentCanvas::page_box(
    const engine::Rect& page, const engine::CoordinateSpace& coordinate_space) {
  // Whole pixels, shared by the paper widgets and the tile clip, so a page's
  // paper and its text can never be a sub-pixel apart.
  const double x = std::round(coordinate_space.twips_x_to_pixels(page.x));
  const double y = std::round(coordinate_space.twips_y_to_pixels(page.y));
  const double right = std::round(coordinate_space.twips_x_to_pixels(page.x + page.width));
  const double bottom = std::round(coordinate_space.twips_y_to_pixels(page.y + page.height));
  return {x, y, std::max(0.0, right - x), std::max(0.0, bottom - y)};
}

void DocumentCanvas::queue_page_layout() {
  if (page_layout_source_ != 0) {
    return;
  }
  // Widgets cannot be moved while GTK is allocating or drawing, and the
  // triggers (resize, scroll, zoom) arrive in exactly those phases.
  page_layout_source_ = g_idle_add_full(
      GDK_PRIORITY_REDRAW - 1,
      [](gpointer data) -> gboolean {
        auto* self = static_cast<DocumentCanvas*>(data);
        self->page_layout_source_ = 0;
        self->layout_pages();
        return G_SOURCE_REMOVE;
      },
      this, nullptr);
}

void DocumentCanvas::layout_pages() {
  std::size_t used = 0;
  if (engine_.has_document()) {
    const auto status = engine_.status();
    std::vector<engine::Rect> pages = status.page_areas_twips;
    if (pages.empty() && status.extent_twips.width > 0 && status.extent_twips.height > 0) {
      pages.push_back({0, 0, status.extent_twips.width, status.extent_twips.height});
    }
    const auto coordinate_space = coordinates();
    GtkAdjustment* vertical =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller_));
    const double view_top = gtk_adjustment_get_value(vertical);
    const double view_height = std::max(gtk_adjustment_get_page_size(vertical), 600.0);
    // Only pages near the viewport get a widget, so a long document costs
    // the same as a short one. One screen of slack either side keeps a fast
    // scroll from showing a page without its paper.
    const double near_top = view_top - view_height;
    const double near_bottom = view_top + 2.0 * view_height;
    for (const auto& page : pages) {
      const auto box = page_box(page, coordinate_space);
      if (box.y + box.height < near_top || box.y > near_bottom) {
        continue;
      }
      if (used == page_widgets_.size()) {
        GtkWidget* paper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        // The toolkit's island is Luma's resting surface; a page is a sheet
        // resting on the canvas, so it takes the island's shadow tokens and
        // stays in step with every appearance mode.
        gtk_widget_add_css_class(paper, "luma-island");
        gtk_widget_add_css_class(paper, "write-page");
        gtk_widget_set_can_target(paper, FALSE);
        gtk_fixed_put(GTK_FIXED(page_layer_), paper, 0, 0);
        page_widgets_.push_back(paper);
      }
      GtkWidget* paper = page_widgets_[used++];
      gtk_widget_set_size_request(paper, static_cast<int>(box.width),
                                  static_cast<int>(box.height));
      gtk_fixed_move(GTK_FIXED(page_layer_), paper, box.x, box.y);
      gtk_widget_set_visible(paper, TRUE);
    }
  }
  for (std::size_t index = used; index < page_widgets_.size(); ++index) {
    gtk_widget_set_visible(page_widgets_[index], FALSE);
  }
  layout_comments();
}

namespace {

std::string json_field(const std::string& object, const char* key) {
  const std::string quoted = std::string{"\""} + key + "\"";
  auto at = object.find(quoted);
  if (at == std::string::npos) return {};
  at = object.find(':', at + quoted.size());
  if (at == std::string::npos) return {};
  ++at;
  while (at < object.size() && (object[at] == ' ' || object[at] == '\n')) ++at;
  if (at >= object.size()) return {};
  if (object[at] == '"') {
    std::string value;
    for (std::size_t index = at + 1; index < object.size(); ++index) {
      const char character = object[index];
      if (character == '\\' && index + 1 < object.size()) {
        const char next = object[++index];
        value.push_back(next == 'n' ? '\n' : next);
      } else if (character == '"') {
        break;
      } else {
        value.push_back(character);
      }
    }
    return value;
  }
  const auto end = object.find_first_of(",}\n", at);
  return object.substr(at, end == std::string::npos ? std::string::npos : end - at);
}




}  // namespace

void DocumentCanvas::refresh_comments() {
  comments_.clear();
  if (engine_.has_document()) {
    const std::string json = engine_.command_values(".uno:ViewAnnotations");
    for (const auto& card : ui::comment_cards_from_json(json)) {
      Comment comment;
      comment.id = card.id;
      comment.author = card.author;
      comment.time = card.time_label;
      comment.text = card.text;
      comment.anchor_y = card.anchor_y;
      comments_.push_back(std::move(comment));
    }
  }
  std::sort(comments_.begin(), comments_.end(),
            [](const Comment& left, const Comment& right) { return left.anchor_y < right.anchor_y; });
  g_debug("Write comments: %zu shown", comments_.size());
  const int gutter = comments_.empty() ? 0 : 232;
  if (gutter != comment_gutter_) {
    comment_gutter_ = gutter;
    update_content_size();
    apply_fit_zoom();
  }
  for (GtkWidget* card : comment_cards_) {
    gtk_overlay_remove_overlay(GTK_OVERLAY(layers_), card);
  }
  comment_cards_.clear();
  for (const auto& comment : comments_) {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(card, "write-comment-card");
    gtk_widget_set_halign(card, GTK_ALIGN_START);
    gtk_widget_set_valign(card, GTK_ALIGN_START);
    gtk_widget_set_size_request(card, 200, -1);
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    GtkWidget* avatar = gtk_label_new(ui::comment_initials(comment.author).c_str());
    gtk_widget_add_css_class(avatar, "comment-avatar");
    gtk_box_append(GTK_BOX(head), avatar);
    GtkWidget* author = gtk_label_new(comment.author.c_str());
    gtk_label_set_xalign(GTK_LABEL(author), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(author), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(author, TRUE);
    gtk_widget_add_css_class(author, "comment-author");
    gtk_box_append(GTK_BOX(head), author);
    GtkWidget* time = gtk_label_new(comment.time.c_str());
    gtk_widget_add_css_class(time, "comment-time");
    gtk_box_append(GTK_BOX(head), time);
    gtk_box_append(GTK_BOX(card), head);
    GtkWidget* body = gtk_label_new(comment.text.c_str());
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(body), 0.0F);
    gtk_label_set_max_width_chars(GTK_LABEL(body), 24);
    gtk_label_set_selectable(GTK_LABEL(body), TRUE);
    gtk_widget_add_css_class(body, "comment-body");
    gtk_box_append(GTK_BOX(card), body);
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* resolve = gtk_button_new_with_label("Resolve");
    gtk_widget_add_css_class(resolve, "luma-button");
    gtk_widget_add_css_class(resolve, "small");
    GtkWidget* remove = gtk_button_new_from_icon_name("write-close-symbolic");
    gtk_widget_add_css_class(remove, "luma-icon-button");
    gtk_widget_set_tooltip_text(remove, "Delete comment");
    gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Delete comment", -1);
    for (GtkWidget* button : {resolve, remove}) {
      g_object_set_data_full(G_OBJECT(button), "write-comment-id", g_strdup(comment.id.c_str()),
                             g_free);
      g_object_set_data(G_OBJECT(button), "write-comment-command",
                        const_cast<char*>(button == resolve ? ".uno:ResolveComment"
                                                            : ".uno:DeleteComment"));
      g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
                         auto* self = static_cast<DocumentCanvas*>(data);
                         const auto* id = static_cast<const char*>(
                             g_object_get_data(G_OBJECT(clicked), "write-comment-id"));
                         const auto* command = static_cast<const char*>(
                             g_object_get_data(G_OBJECT(clicked), "write-comment-command"));
                         const std::string arguments =
                             std::string{"{\"Id\":{\"type\":\"string\",\"value\":\""} + id + "\"}}";
                         static_cast<void>(self->engine_.dispatch_command(command, arguments, true));
                         self->refresh_comments();
                       }),
                       this);
    }
    gtk_box_append(GTK_BOX(actions), resolve);
    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(actions), spacer);
    gtk_box_append(GTK_BOX(actions), remove);
    gtk_box_append(GTK_BOX(card), actions);
    gtk_overlay_add_overlay(GTK_OVERLAY(layers_), card);
    comment_cards_.push_back(card);
  }
  queue_page_layout();
}

void DocumentCanvas::layout_comments() {
  if (comment_cards_.empty()) {
    return;
  }
  const auto status = engine_.status();
  const auto coordinate_space = coordinates();
  double page_right = coordinate_space.twips_x_to_pixels(status.extent_twips.width);
  if (!status.page_areas_twips.empty()) {
    const auto& first = status.page_areas_twips.front();
    page_right = page_box(first, coordinate_space).x + page_box(first, coordinate_space).width;
  }
  double next_free = 0;
  for (std::size_t index = 0; index < comment_cards_.size() && index < comments_.size(); ++index) {
    GtkWidget* card = comment_cards_[index];
    double y = std::max(coordinate_space.twips_y_to_pixels(comments_[index].anchor_y) - 12.0,
                        next_free);
    int natural = 0;
    gtk_widget_measure(card, GTK_ORIENTATION_VERTICAL, 200, nullptr, &natural, nullptr, nullptr);
    gtk_widget_set_margin_start(card, static_cast<int>(page_right + 16.0));
    gtk_widget_set_margin_top(card, static_cast<int>(y));
    next_free = y + natural + 8.0;
  }
}

void DocumentCanvas::show_context_menu(const std::string& json) {
  // Preserve the engine commands, using the shared Luma menu for placement,
  // typography, keyboard navigation, dismissal and touch presentation.
  if (context_popover_ != nullptr) {
    luma_floating_menu_close(LUMA_FLOATING_MENU(context_popover_));
    g_object_unref(context_popover_);
    context_popover_ = nullptr;
  }
  auto* menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Document actions"));
  g_object_ref_sink(menu);
  int shown = 0;
  bool pending_separator = false;
  std::size_t at = 0;
  while ((at = json.find('{', at + 1)) != std::string::npos) {
    const auto close = json.find_first_of("{}", at + 1);
    if (close == std::string::npos) break;
    const std::string object = json.substr(at, close - at + 1);
    const auto type = json_field(object, "type");
    if (type == "separator") {
      pending_separator = shown > 0;
      continue;
    }
    if (type != "command") continue;
    if (json_field(object, "enabled") == "false") continue;
    std::string label = json_field(object, "text");
    label.erase(std::remove(label.begin(), label.end(), '~'), label.end());
    const std::string command = json_field(object, "command");
    if (label.empty() || command.rfind(".uno:", 0) != 0) continue;
    // Commands that open LibreOffice dialogs are not offered, nor the
    // per-word language lists: Write checks spelling in the document's
    // languages and a menu is no place to change them.
    if (command.find("Dialog") != std::string::npos ||
        command.rfind(".uno:LanguageStatus", 0) == 0 ||
        command.rfind(".uno:SetLanguage", 0) == 0 || command == ".uno:FontDialog")
      continue;
    if (pending_separator) {
      luma_floating_menu_add_separator(menu);
      pending_separator = false;
    }
    luma_floating_menu_add_item(menu, label.c_str(), nullptr, nullptr, nullptr, nullptr, FALSE);
    GtkWidget* row = gtk_widget_get_last_child(GTK_WIDGET(menu));
    g_object_set_data_full(G_OBJECT(row), "write-uno", g_strdup(command.c_str()), g_free);
    g_signal_connect(row, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
                       auto* self = static_cast<DocumentCanvas*>(data);
                       const auto* uno = static_cast<const char*>(
                           g_object_get_data(G_OBJECT(clicked), "write-uno"));
                       luma_floating_menu_close(LUMA_FLOATING_MENU(self->context_popover_));
                       const std::string_view name{uno};
                       const bool view_only = name == ".uno:Copy" || name == ".uno:SelectAll";
                       if (name == ".uno:Copy") {
                         self->copy_selection();
                       } else if (name == ".uno:Cut") {
                         self->cut_selection();
                       } else if (name == ".uno:Paste") {
                         self->paste_from_clipboard();
                       } else {
                         static_cast<void>(self->engine_.dispatch_command(uno, "", !view_only));
                       }
                     }),
                     this);
    ++shown;
  }
  if (shown == 0) {
    g_object_unref(menu);
    return;
  }
  context_popover_ = GTK_WIDGET(menu);
  // A one-pixel anchor in document coordinates makes the shared menu open at
  // the clicked position, including while the canvas is scrolled.
  if (context_anchor_ == nullptr) {
    context_anchor_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(context_anchor_, 1, 1);
    gtk_widget_set_halign(context_anchor_, GTK_ALIGN_START);
    gtk_widget_set_valign(context_anchor_, GTK_ALIGN_START);
    gtk_widget_set_can_target(context_anchor_, FALSE);
    gtk_widget_set_opacity(context_anchor_, 0.0);
    gtk_overlay_add_overlay(GTK_OVERLAY(layers_), context_anchor_);
  }
  gtk_widget_set_margin_start(context_anchor_, std::max(0, static_cast<int>(context_x_)));
  gtk_widget_set_margin_top(context_anchor_, std::max(0, static_cast<int>(context_y_)));
  luma_floating_menu_popup(menu, context_anchor_);
}

void DocumentCanvas::clip_to_pages(cairo_t* context,
                                   const engine::CoordinateSpace& coordinate_space,
                                   const engine::DocumentStatus& status) const {
  if (status.page_areas_twips.empty()) {
    return;
  }
  double clip_x1 = 0;
  double clip_y1 = 0;
  double clip_x2 = 0;
  double clip_y2 = 0;
  cairo_clip_extents(context, &clip_x1, &clip_y1, &clip_x2, &clip_y2);
  bool any = false;
  for (const auto& page : status.page_areas_twips) {
    const auto box = page_box(page, coordinate_space);
    if (box.y > clip_y2 || box.y + box.height < clip_y1) {
      continue;
    }
    cairo_rectangle(context, box.x, box.y, box.width, box.height);
    any = true;
  }
  if (any) {
    cairo_clip(context);
  } else {
    // Nothing on a page is exposed: tiles would only paint the gaps.
    cairo_rectangle(context, 0, 0, 0, 0);
    cairo_clip(context);
  }
}

void DocumentCanvas::draw_tiles(cairo_t* context, const engine::CoordinateSpace& coordinate_space,
                                const engine::DocumentStatus& status, const double clip_x1,
                                const double clip_y1, const double clip_x2,
                                const double clip_y2) {
  const double origin_x = coordinate_space.twips_x_to_pixels(0);
  const double origin_y = coordinate_space.twips_y_to_pixels(0);
  const double document_width =
      static_cast<double>(status.extent_twips.width) * coordinate_space.pixels_per_twip();
  const double document_height =
      static_cast<double>(status.extent_twips.height) * coordinate_space.pixels_per_twip();
  if (document_width <= 0 || document_height <= 0) {
    return;
  }

  const int first_x = static_cast<int>(std::floor(
                          std::max(0.0, clip_x1 - origin_x) / kLogicalTileEdge)) *
                      kLogicalTileEdge;
  const int first_y = static_cast<int>(std::floor(
                          std::max(0.0, clip_y1 - origin_y) / kLogicalTileEdge)) *
                      kLogicalTileEdge;
  const int last_x = static_cast<int>(std::ceil(
      std::min(document_width, std::max(0.0, clip_x2 - origin_x)) / kLogicalTileEdge));
  const int last_y = static_cast<int>(std::ceil(
      std::min(document_height, std::max(0.0, clip_y2 - origin_y)) / kLogicalTileEdge));
  const int scale = std::clamp(gtk_widget_get_scale_factor(drawing_area_), 1, 4);
  const engine::CoordinateSpace local(zoom_, scale);

  for (int row = first_y / kLogicalTileEdge; row < last_y; ++row) {
    for (int column = first_x / kLogicalTileEdge; column < last_x; ++column) {
      const int logical_x = column * kLogicalTileEdge;
      const int logical_y = row * kLogicalTileEdge;
      const int logical_width = static_cast<int>(std::ceil(
          std::min(static_cast<double>(kLogicalTileEdge), document_width - logical_x)));
      const int logical_height = static_cast<int>(std::ceil(
          std::min(static_cast<double>(kLogicalTileEdge), document_height - logical_y)));
      if (logical_width <= 0 || logical_height <= 0) {
        continue;
      }
      engine::TileRequest request{
          .document_area_twips = local.pixel_rect_to_twips(
              static_cast<double>(logical_x), static_cast<double>(logical_y),
              static_cast<double>(logical_width), static_cast<double>(logical_height)),
          .pixel_width = logical_width * scale,
          .pixel_height = logical_height * scale,
          .part = 0,
          .zoom = zoom_,
          .device_scale = scale,
          .revision = status.revision,
      };
      const auto key = tile_key(request);
      auto tile = tile_cache_.find(key);
      if (!tile) {
        auto rendered = std::make_shared<engine::RenderedTile>();
        if (const auto result = engine_.render_tile(request, *rendered); !result.ok) {
          continue;
        }
        swap_red_blue(*rendered);
        tile_cache_.insert(key, rendered);
        tile = std::move(rendered);
      }
      auto* mutable_pixels = const_cast<unsigned char*>(
          reinterpret_cast<const unsigned char*>(tile->pixels.data()));
      cairo_surface_t* surface = cairo_image_surface_create_for_data(
          mutable_pixels, CAIRO_FORMAT_ARGB32, tile->request.pixel_width,
          tile->request.pixel_height, static_cast<int>(tile->stride));
      cairo_save(context);
      cairo_translate(context, origin_x + logical_x, origin_y + logical_y);
      cairo_scale(context, 1.0 / scale, 1.0 / scale);
      cairo_set_source_surface(context, surface, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_BILINEAR);
      cairo_paint(context);
      cairo_restore(context);
      cairo_surface_destroy(surface);
    }
  }
}

void DocumentCanvas::draw_overlays(cairo_t* context,
                                   const engine::CoordinateSpace& coordinate_space) const {
  // Editing guides belong to the viewport and never enter printing or export.
  cairo_set_source_rgba(context, appearance_.moon ? 0.72 : 0.40,
                        appearance_.moon ? 0.75 : 0.43,
                        appearance_.moon ? 0.80 : 0.48, 0.35);
  cairo_set_line_width(context, 1.0);
  for (const auto& page : engine_.status().page_areas_twips) {
    const double left = coordinate_space.twips_x_to_pixels(page.x);
    const double right = coordinate_space.twips_x_to_pixels(page.x + page.width);
    const auto rule = [&](std::int64_t y_twips) {
      const double y = std::floor(coordinate_space.twips_y_to_pixels(y_twips)) + 0.5;
      cairo_move_to(context, left, y);
      cairo_line_to(context, right, y);
      cairo_stroke(context);
    };
    if (header_on_) rule(page.y + page_margin_twips_ + header_height_twips_);
    if (footer_on_) rule(page.y + page.height - page_margin_twips_ - footer_height_twips_);
  }

  cairo_set_source_rgba(context, 78.0 / 255.0, 99.0 / 255.0, 210.0 / 255.0, 0.26);
  for (const auto& selection : selection_twips_) {
    cairo_rectangle(context, coordinate_space.twips_x_to_pixels(selection.x),
                    coordinate_space.twips_y_to_pixels(selection.y),
                    static_cast<double>(selection.width) * coordinate_space.pixels_per_twip(),
                    static_cast<double>(selection.height) * coordinate_space.pixels_per_twip());
    cairo_fill(context);
  }

  if (cursor_visible_ && cursor_twips_.has_value()) {
    const auto& cursor = *cursor_twips_;
    const double x = coordinate_space.twips_x_to_pixels(cursor.x);
    const double y = coordinate_space.twips_y_to_pixels(cursor.y);
    const double height =
        std::max(12.0, static_cast<double>(cursor.height) * coordinate_space.pixels_per_twip());
    cairo_set_source_rgb(context, 64.0 / 255.0, 84.0 / 255.0, 188.0 / 255.0);
    cairo_set_line_width(context, 1.5);
    cairo_move_to(context, x, y);
    cairo_line_to(context, x, y + height);
    cairo_stroke(context);
  }

  draw_graphic_selection(context, coordinate_space);
}

engine::PointerEvent DocumentCanvas::pointer_event(const engine::PointerPhase phase,
                                                   const int presses, const double x,
                                                   const double y,
                                                   const std::uint32_t buttons) const {
  const auto coordinate_space = coordinates();
  return {.phase = phase,
          .x_twips = coordinate_space.pixel_x_to_twips(x),
          .y_twips = coordinate_space.pixel_y_to_twips(y),
          .click_count = std::max(1, presses),
          .buttons = buttons,
          .modifiers = 0};
}

void DocumentCanvas::pointer_pressed(GtkGestureClick* gesture, const int presses, const double x,
                                     const double y, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  gtk_widget_grab_focus(self.drawing_area_);
  const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (button != GDK_BUTTON_PRIMARY) {
    return;
  }
  // A press on a picture's handle resizes the picture.
  if (const int handle = self.graphic_handle_at(x, y); handle >= 0 && presses == 1) {
    const auto point = self.pointer_event(engine::PointerPhase::press, 1, x, y, kLeftButton);
    self.resize_handle_ = handle;
    self.resize_start_ = *self.graphic_selection_twips_;
    self.resize_x_ = point.x_twips;
    self.resize_y_ = point.y_twips;
    GdkModifierType modifiers = gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(gesture));
    self.resize_free_ = (modifiers & GDK_SHIFT_MASK) != 0;
    return;
  }
  self.dragging_ = true;
  static_cast<void>(self.engine_.send_pointer(
      self.pointer_event(engine::PointerPhase::press, presses, x, y, kLeftButton)));
}

void DocumentCanvas::pointer_released(GtkGestureClick* gesture, const int presses, const double x,
                                      const double y, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (button != GDK_BUTTON_PRIMARY) {
    return;
  }
  if (self.resize_handle_ >= 0) {
    self.finish_resize(x, y);
    return;
  }
  static_cast<void>(self.engine_.send_pointer(
      self.pointer_event(engine::PointerPhase::release, presses, x, y, kLeftButton)));
  self.dragging_ = false;
  // The engine owns double/triple-click selection and object hit-testing.
  // Do not dispatch text-selection commands while a picture is active.
  if (presses == 1 && !self.graphic_selection_twips_.has_value()) {
    self.toggle_checkbox_at_caret();
  }
}

// A checklist box is the first character of its paragraph; clicking it (the
// caret lands before or just after it) ticks or clears it.
void DocumentCanvas::toggle_checkbox_at_caret() {
  if (!engine_.has_document() || engine_.status().read_only || !selection_twips_.empty()) {
    return;
  }
  const auto a11y = engine_.accessibility_snapshot();
  const std::string& text = a11y.focused_paragraph_utf8;
  const bool open = text.rfind("\u2610", 0) == 0;
  const bool done = text.rfind("\u2611", 0) == 0;
  if ((!open && !done) || a11y.caret_offset < 0 || a11y.caret_offset > 1) {
    return;
  }
  static_cast<void>(engine_.dispatch_command(".uno:GoToStartOfPara", "", false));
  static_cast<void>(engine_.dispatch_command(".uno:CharRightSel", "", false));
  const std::string replacement = open ? "\u2611" : "\u2610";
  static_cast<void>(engine_.dispatch_command(
      ".uno:InsertText", "{\"Text\":{\"type\":\"string\",\"value\":\"" + replacement + "\"}}",
      true));
  static_cast<void>(engine_.dispatch_command(".uno:GoToEndOfPara", "", false));
}

void DocumentCanvas::secondary_pressed(GtkGestureClick*, int, const double x, const double y,
                                       gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  gtk_widget_grab_focus(self.drawing_area_);
  if (!self.engine_.has_document()) {
    return;
  }
  self.context_x_ = x;
  self.context_y_ = y;
  const auto event = self.pointer_event(engine::PointerPhase::press, 1, x, y, 0);
  // On a selected picture, Write's own picture menu.
  if (self.graphic_selection_twips_.has_value()) {
    const auto& picture = *self.graphic_selection_twips_;
    if (event.x_twips >= picture.x && event.x_twips <= picture.x + picture.width &&
        event.y_twips >= picture.y && event.y_twips <= picture.y + picture.height) {
      self.show_image_menu(x, y);
      return;
    }
  }
  // Right-clicking outside the selection moves the caret there first, so the
  // menu (and its spelling suggestions) is about the word under the pointer.
  if (self.selection_twips_.empty()) {
    static_cast<void>(self.engine_.send_pointer(event));
    static_cast<void>(self.engine_.send_pointer(
        self.pointer_event(engine::PointerPhase::release, 1, x, y, kLeftButton)));
  }
  static_cast<void>(self.engine_.request_context_menu(event.x_twips, event.y_twips));
}

void DocumentCanvas::pointer_moved(GtkEventControllerMotion*, const double x, const double y,
                                   gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  if (self.resize_handle_ >= 0) {
    const auto point = self.pointer_event(engine::PointerPhase::move, 1, x, y, kLeftButton);
    self.resize_x_ = point.x_twips;
    self.resize_y_ = point.y_twips;
    gtk_widget_queue_draw(self.drawing_area_);
    return;
  }
  // The pointer shows where a picture can be resized.
  if (!self.dragging_) {
    static constexpr const char* kCursors[] = {"nwse-resize", "ns-resize", "nesw-resize", "ew-resize",
                                               "nwse-resize", "ns-resize", "nesw-resize", "ew-resize"};
    const int handle = self.graphic_handle_at(x, y);
    const auto point = self.pointer_event(engine::PointerPhase::move, 1, x, y, 0);
    const auto picture = self.graphic_selection_twips_;
    const bool inside = picture.has_value() && point.x_twips >= picture->x &&
        point.x_twips <= picture->x + picture->width && point.y_twips >= picture->y &&
        point.y_twips <= picture->y + picture->height;
    gtk_widget_set_cursor_from_name(self.drawing_area_, handle >= 0 ? kCursors[handle] :
        inside && !self.engine_.status().read_only ? "move" : "text");
    return;
  }
  static_cast<void>(self.engine_.send_pointer(
      self.pointer_event(engine::PointerPhase::move, 1, x, y, kLeftButton)));
}

gboolean DocumentCanvas::key_pressed(GtkEventControllerKey*, const guint keyval, guint,
                                     const GdkModifierType state, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  if (is_primary_shortcut(state)) {
    switch (gdk_keyval_to_lower(keyval)) {
      case GDK_KEY_c:
        self.copy_selection();
        return TRUE;
      case GDK_KEY_x:
        self.cut_selection();
        return TRUE;
      case GDK_KEY_v:
        self.paste_from_clipboard();
        return TRUE;
      default:
        break;
    }
  }
  const std::uint32_t special_key = engine_key(keyval);
  const gunichar scalar = gdk_keyval_to_unicode(keyval);
  if (special_key == 0 && scalar == 0) {
    return FALSE;
  }
  const engine::KeyEvent event{
      .phase = engine::KeyPhase::pressed,
      .unicode_scalar = special_key == 0 ? static_cast<std::uint32_t>(scalar) : 0,
      .platform_independent_key = special_key | engine_modifiers(state),
  };
  return self.engine_.send_key(event).ok ? TRUE : FALSE;
}

void DocumentCanvas::copy_selection() { publish_selection(false); }

void DocumentCanvas::cut_selection() { publish_selection(true); }

void DocumentCanvas::paste_from_clipboard() {
  if (!engine_.has_document() || engine_.status().read_only) {
    return;
  }
  if (!paste_rich_content()) {
    request_paste();
  }
}

// Moves the caret to a point in the view, as a click there would.
void DocumentCanvas::place_caret_at(const double x, const double y) {
  static_cast<void>(engine_.send_pointer(pointer_event(engine::PointerPhase::press, 1, x, y, kLeftButton)));
  static_cast<void>(engine_.send_pointer(pointer_event(engine::PointerPhase::release, 1, x, y, kLeftButton)));
}

void DocumentCanvas::publish_selection(const bool cut) {
  const std::string selected = engine_.selected_text();
  if (selected.empty() || selected.find('\0') != std::string::npos ||
      !g_utf8_validate(selected.c_str(), -1, nullptr)) {
    return;
  }

  GValue value = G_VALUE_INIT;
  g_value_init(&value, G_TYPE_STRING);
  g_value_set_string(&value, selected.c_str());
  GdkContentProvider* provider = gdk_content_provider_new_for_value(&value);
  const gboolean published = gdk_clipboard_set_content(
      gtk_widget_get_clipboard(drawing_area_), provider);
  g_object_unref(provider);
  g_value_unset(&value);

  if (cut && published && engine_.has_document() && !engine_.status().read_only) {
    const auto result = engine_.execute({.command = engine::EngineCommand::cut,
                                         .arguments = {},
                                         .request_completion = true});
    if (!result.ok) {
      gtk_widget_error_bell(drawing_area_);
    }
  } else if (!published) {
    gtk_widget_error_bell(drawing_area_);
  }
}

void DocumentCanvas::request_paste() {
  if (!engine_.has_document() || engine_.status().read_only) {
    return;
  }
  {
    std::lock_guard lock(clipboard_bridge_->mutex);
    if (clipboard_bridge_->paste_pending) {
      return;
    }
    clipboard_bridge_->paste_pending = true;
  }
  auto* request = new PasteRequest(clipboard_bridge_, paste_cancellable_);
  gdk_clipboard_read_async(gtk_widget_get_clipboard(drawing_area_),
                           kPasteMimeTypes, G_PRIORITY_DEFAULT,
                           request->cancellable, paste_stream_opened, request);
}

void DocumentCanvas::paste_stream_opened(GObject* source, GAsyncResult* result,
                                         gpointer data) {
  std::unique_ptr<PasteRequest> request(static_cast<PasteRequest*>(data));
  GError* error = nullptr;
  const char* mime_type = nullptr;
  request->stream = gdk_clipboard_read_finish(
      GDK_CLIPBOARD(source), result, &mime_type, &error);
  if (request->stream == nullptr || error != nullptr || mime_type == nullptr ||
      (std::string_view(mime_type) != "text/plain;charset=utf-8" &&
       std::string_view(mime_type) != "text/plain")) {
    if (error == nullptr ||
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      clipboard_error_bell(request->bridge);
    }
    g_clear_error(&error);
    return;
  }
  request->mime_type = mime_type;
  read_next_paste_chunk(request.release());
}

void DocumentCanvas::read_next_paste_chunk(PasteRequest* request) {
  if (request->bytes.size() > engine::kMaximumPlainTextClipboardBytes) {
    clipboard_error_bell(request->bridge);
    delete request;
    return;
  }
  const std::size_t remaining =
      engine::kMaximumPlainTextClipboardBytes - request->bytes.size();
  const gsize read_size = remaining == 0
                              ? 1U
                              : std::min(kPasteChunkBytes,
                                         static_cast<gsize>(remaining));
  g_input_stream_read_bytes_async(
      request->stream, read_size, G_PRIORITY_DEFAULT, request->cancellable,
      paste_chunk_finished, request);
}

void DocumentCanvas::paste_chunk_finished(GObject* source, GAsyncResult* result,
                                          gpointer data) {
  std::unique_ptr<PasteRequest> request(static_cast<PasteRequest*>(data));
  GError* error = nullptr;
  GBytes* chunk = g_input_stream_read_bytes_finish(
      G_INPUT_STREAM(source), result, &error);
  if (chunk == nullptr || error != nullptr) {
    if (error == nullptr ||
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      clipboard_error_bell(request->bridge);
    }
    if (chunk != nullptr) {
      g_bytes_unref(chunk);
    }
    g_clear_error(&error);
    return;
  }

  gsize chunk_size = 0;
  const auto* chunk_data =
      static_cast<const std::byte*>(g_bytes_get_data(chunk, &chunk_size));
  const std::size_t remaining =
      engine::kMaximumPlainTextClipboardBytes - request->bytes.size();
  if (chunk_size > remaining) {
    g_bytes_unref(chunk);
    clipboard_error_bell(request->bridge);
    return;
  }
  if (chunk_size != 0) {
    request->bytes.insert(request->bytes.end(), chunk_data,
                          chunk_data + chunk_size);
    g_bytes_unref(chunk);
    read_next_paste_chunk(request.release());
    return;
  }
  g_bytes_unref(chunk);

  const bool contains_nul =
      std::find(request->bytes.begin(), request->bytes.end(), std::byte{0}) !=
      request->bytes.end();
  if (contains_nul ||
      (!request->bytes.empty() &&
       !g_utf8_validate(
           reinterpret_cast<const char*>(request->bytes.data()),
           static_cast<gssize>(request->bytes.size()), nullptr))) {
    clipboard_error_bell(request->bridge);
    return;
  }

  // All clipboard I/O callbacks and GTK teardown run on the main context that
  // initiated the read. Copy the guarded target without holding the bridge
  // lock across LibreOfficeKit dispatch, which may synchronously emit events.
  DocumentCanvas* target = nullptr;
  {
    std::lock_guard lock(request->bridge->mutex);
    target = request->bridge->target;
  }
  if (target != nullptr && target->engine_.has_document() &&
      !target->engine_.status().read_only) {
    const auto paste_result = target->engine_.paste(
        request->mime_type,
        std::span<const std::byte>(request->bytes.data(), request->bytes.size()));
    if (!paste_result.ok) {
      gtk_widget_error_bell(target->drawing_area_);
    }
  }
}

void DocumentCanvas::clipboard_error_bell(
    const std::shared_ptr<ClipboardBridge>& bridge) {
  DocumentCanvas* target = nullptr;
  {
    std::lock_guard lock(bridge->mutex);
    target = bridge->target;
  }
  if (target != nullptr) {
    gtk_widget_error_bell(target->drawing_area_);
  }
}

void DocumentCanvas::key_released(GtkEventControllerKey*, const guint keyval, guint,
                                  const GdkModifierType state, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  if (is_primary_shortcut(state)) {
    switch (gdk_keyval_to_lower(keyval)) {
      case GDK_KEY_c:
      case GDK_KEY_x:
      case GDK_KEY_v:
        return;
      default:
        break;
    }
  }
  const std::uint32_t special_key = engine_key(keyval);
  const gunichar scalar = gdk_keyval_to_unicode(keyval);
  if (special_key == 0 && scalar == 0) {
    return;
  }
  static_cast<void>(self.engine_.send_key(
      {.phase = engine::KeyPhase::released,
       .unicode_scalar = special_key == 0 ? static_cast<std::uint32_t>(scalar) : 0,
       .platform_independent_key = special_key | engine_modifiers(state)}));
}

void DocumentCanvas::input_committed(GtkIMContext*, const char* text, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  static_cast<void>(self.engine_.send_composition(
      {.phase = engine::CompositionPhase::commit, .utf8 = text == nullptr ? "" : text}));
}

void DocumentCanvas::preedit_changed(GtkIMContext* context, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  char* text = nullptr;
  PangoAttrList* attributes = nullptr;
  int cursor = 0;
  gtk_im_context_get_preedit_string(context, &text, &attributes, &cursor);
  static_cast<void>(cursor);
  static_cast<void>(self.engine_.send_composition(
      {.phase = engine::CompositionPhase::update, .utf8 = text == nullptr ? "" : text}));
  g_free(text);
  if (attributes != nullptr) {
    pango_attr_list_unref(attributes);
  }
}

void DocumentCanvas::focus_entered(GtkEventControllerFocus*, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  gtk_im_context_focus_in(self.input_method_);
}

void DocumentCanvas::focus_left(GtkEventControllerFocus*, gpointer data) {
  auto& self = *static_cast<DocumentCanvas*>(data);
  gtk_im_context_focus_out(self.input_method_);
}

void DocumentCanvas::update_input_cursor() {
  if (!cursor_twips_.has_value()) {
    return;
  }
  const auto coordinate_space = coordinates();
  const auto& cursor = *cursor_twips_;
  GdkRectangle rectangle{
      .x = static_cast<int>(std::lround(coordinate_space.twips_x_to_pixels(cursor.x))),
      .y = static_cast<int>(std::lround(coordinate_space.twips_y_to_pixels(cursor.y))),
      .width = static_cast<int>(std::max(
          1.0, static_cast<double>(cursor.width) * coordinate_space.pixels_per_twip())),
      .height = static_cast<int>(std::max(
          1.0, static_cast<double>(cursor.height) * coordinate_space.pixels_per_twip())),
  };
  gtk_im_context_set_cursor_location(input_method_, &rectangle);
}

}  // namespace luma::write::canvas
