#pragma once

#include "../engine/coordinate_space.hpp"
#include "../engine/document_engine.hpp"
#include "tile_cache.hpp"

#include <gtk/gtk.h>
#include <luma-ui.h>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luma::write::canvas {

// How the page itself is presented. This is view state only: it never reaches
// the document model, so printing and export always use the document's own
// colours.
struct PageAppearance {
  // Moon: a dark page with light text, rendered by the engine's view theme.
  bool moon{false};
  // The canvas ground behind the page is dark (Dark/Glass/Frost chrome, or
  // Moon). A shadow has to be stronger to read on a dark ground.
  bool dark_ground{false};

  friend bool operator==(const PageAppearance&, const PageAppearance&) = default;
};

// automatic: the page at the design's reading size (85%, the Design Center's
// page zoom) unless the viewport is narrower, in which case it fits the
// width. The fit modes follow the viewport as it resizes.
inline constexpr double kAutomaticZoom = 0.85;
// The Moon page and ink colours, shared by the paper (write.css, .write-page
// in a .moon layer) and the engine's dark view scheme that paints the tiles.
inline constexpr std::array<int, 3> kMoonPage{0x31, 0x35, 0x3b};
inline constexpr std::array<int, 3> kMoonInk{0xe3, 0xe6, 0xe9};

enum class ZoomMode { automatic, fixed, fit_width, fit_page };

// One virtualized GTK drawing surface owns the entire document viewport. It
// never creates a widget for each page, paragraph, or object.
class DocumentCanvas {
 public:
  explicit DocumentCanvas(engine::DocumentEngine& document_engine);
  ~DocumentCanvas();
  DocumentCanvas(const DocumentCanvas&) = delete;
  DocumentCanvas& operator=(const DocumentCanvas&) = delete;

  [[nodiscard]] GtkWidget* widget() const noexcept { return scroller_; }
  [[nodiscard]] GtkWidget* focus_widget() const noexcept { return drawing_area_; }
  [[nodiscard]] double zoom() const noexcept { return zoom_; }

  void set_zoom(double zoom);
  // Fit modes follow the viewport as it resizes; set_zoom() returns to fixed.
  void set_zoom_mode(ZoomMode mode);
  [[nodiscard]] ZoomMode zoom_mode() const noexcept { return zoom_mode_; }
  void set_page_appearance(const PageAppearance& appearance);
  // Re-reads the document's comments and lays out their margin cards.
  void refresh_comments();
  [[nodiscard]] const PageAppearance& page_appearance() const noexcept {
    return appearance_;
  }
  // Called after every zoom change, including ones a fit mode makes itself.
  void set_zoom_listener(std::function<void(double)> listener) {
    zoom_listener_ = std::move(listener);
  }
  // Scrolls so the given document position (twips) is near the top.
  void scroll_to_twips(std::int64_t y_twips);
  [[nodiscard]] int current_page() const;
  void refresh_document();
  void handle_engine_event(const engine::EngineEvent& event);
  void copy_selection();
  void cut_selection();
  void paste_from_clipboard();
  void prepare_for_window_destruction();
  void update_formatting_controls(const std::string& style, const std::string& size);
  void set_page_parts(bool header, bool footer, std::int64_t margin_twips);

  // Outlives the canvas for asynchronous picture reads (document_canvas_images.cpp).
  struct ClipboardBridgeRef {
    DocumentCanvas* target{nullptr};
  };

 private:
  struct ClipboardBridge;
  struct PasteRequest;

  static void draw(GtkDrawingArea* area, cairo_t* context, int width, int height,
                   gpointer data);
  static void pointer_pressed(GtkGestureClick* gesture, int presses, double x, double y,
                              gpointer data);
  static void pointer_released(GtkGestureClick* gesture, int presses, double x, double y,
                               gpointer data);
  static void pointer_moved(GtkEventControllerMotion* controller, double x, double y,
                            gpointer data);
  static gboolean key_pressed(GtkEventControllerKey* controller, guint keyval, guint keycode,
                              GdkModifierType state, gpointer data);
  static void key_released(GtkEventControllerKey* controller, guint keyval, guint keycode,
                           GdkModifierType state, gpointer data);
  static void input_committed(GtkIMContext* context, const char* text, gpointer data);
  static void preedit_changed(GtkIMContext* context, gpointer data);
  static void focus_entered(GtkEventControllerFocus* controller, gpointer data);
  static void focus_left(GtkEventControllerFocus* controller, gpointer data);
  static void paste_stream_opened(GObject* source, GAsyncResult* result,
                                  gpointer data);
  static void paste_chunk_finished(GObject* source, GAsyncResult* result,
                                   gpointer data);

  void draw_impl(cairo_t* context, int width, int height);
  void clip_to_pages(cairo_t* context, const engine::CoordinateSpace& coordinates,
                     const engine::DocumentStatus& status) const;
  struct PageBox {
    double x;
    double y;
    double width;
    double height;
  };
  [[nodiscard]] static PageBox page_box(const engine::Rect& page,
                                        const engine::CoordinateSpace& coordinates);
  void queue_page_layout();
  void layout_pages();
  void draw_tiles(cairo_t* context, const engine::CoordinateSpace& coordinates,
                  const engine::DocumentStatus& status, double clip_x1, double clip_y1,
                  double clip_x2, double clip_y2);
  void draw_overlays(cairo_t* context, const engine::CoordinateSpace& coordinates) const;
  [[nodiscard]] engine::CoordinateSpace coordinates() const;
  [[nodiscard]] engine::PointerEvent pointer_event(engine::PointerPhase phase, int presses,
                                                   double x, double y,
                                                   std::uint32_t buttons) const;
  void publish_selection(bool cut);
  void request_paste();
  // Pictures by paste and drop (document_canvas_images.cpp).
  bool paste_rich_content();
  void paste_html(std::string html);
  void read_clipboard_bytes(std::initializer_list<const char*> mime_types,
                            std::function<void(DocumentCanvas&, const std::string&, GBytes*)> done);
  std::filesystem::path image_directory();
  void insert_image_file(const std::filesystem::path& file);
  void insert_image_bytes(GBytes* bytes, const char* extension);
  void insert_texture(GdkTexture* texture);
  void insert_files(GSList* files);
  void install_drop_target();
  void remove_image_directory();
  void place_caret_at(double x, double y);
  // The selected picture: handles, resizing and its bar (document_canvas_images.cpp).
  [[nodiscard]] int graphic_handle_at(double x, double y) const;
  [[nodiscard]] engine::Rect resize_preview_twips() const;
  void draw_graphic_selection(cairo_t* context, const engine::CoordinateSpace& coordinates) const;
  void finish_resize(double x, double y);
  void image_command(const char* uno);
  void show_image_menu(double x, double y);
  void replace_image();
  static void read_next_paste_chunk(PasteRequest* request);
  static void clipboard_error_bell(
      const std::shared_ptr<ClipboardBridge>& bridge);
  void update_content_size();
  void update_input_cursor();
  void apply_fit_zoom();
  struct Comment {
    std::string id;
    std::string author;
    std::string time;
    std::string text;
    std::int64_t anchor_y{0};
    bool resolved{false};
  };
  void layout_comments();
  void show_context_menu(const std::string& json);
  void toggle_checkbox_at_caret();
  static void secondary_pressed(GtkGestureClick* gesture, int presses, double x, double y,
                                gpointer data);
  void apply_zoom(double zoom, bool from_fit);

  engine::DocumentEngine& engine_;
  GtkWidget* scroller_{nullptr};
  GtkWidget* drawing_area_{nullptr};
  // Paper for the pages near the viewport: toolkit islands under the tiles.
  GtkWidget* page_layer_{nullptr};
  std::vector<GtkWidget*> page_widgets_;
  guint page_layout_source_{0};
  GtkWidget* layers_{nullptr};
  std::vector<Comment> comments_;
  std::vector<GtkWidget*> comment_cards_;
  GtkWidget* context_popover_{nullptr};
  GtkWidget* context_anchor_{nullptr};
  double context_x_{0};
  double context_y_{0};
  // Space kept to the right of the pages for comment cards.
  int comment_gutter_{0};
  GtkIMContext* input_method_{nullptr};
  bool input_method_detached_{false};
  std::shared_ptr<ClipboardBridge> clipboard_bridge_;
  GCancellable* paste_cancellable_{nullptr};
  std::shared_ptr<ClipboardBridgeRef> image_bridge_;
  std::filesystem::path image_directory_;
  int image_counter_{0};
  TileCache tile_cache_{96U * 1024U * 1024U};
  double zoom_{1.0};
  ZoomMode zoom_mode_{ZoomMode::automatic};
  PageAppearance appearance_;
  std::function<void(double)> zoom_listener_;
  int document_margin_x_{40};
  int document_margin_y_{32};
  bool dragging_{false};
  // A picture handle being dragged: which one (0 top-left, clockwise to 7
  // left, -1 none), the picture as it was, and the pointer in twips.
  int resize_handle_{-1};
  engine::Rect resize_start_{};
  std::int64_t resize_x_{0};
  std::int64_t resize_y_{0};
  bool resize_free_{false};
  std::optional<engine::Rect> cursor_twips_;
  bool cursor_visible_{true};
  std::vector<engine::Rect> selection_twips_;
  LumaSelectionBubble* selection_bubble_{nullptr};
  guint selection_bubble_source_{0};
  bool header_on_{false};
  bool footer_on_{false};
  std::int64_t page_margin_twips_{1440};
  std::int64_t header_height_twips_{300};
  std::int64_t footer_height_twips_{300};
  std::optional<engine::Rect> graphic_selection_twips_;
};

}  // namespace luma::write::canvas
