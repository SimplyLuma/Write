#pragma once

#include "commands/command_registry.h"
#include "suite/suite_identity.h"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luma::write::ui {

enum class ToastKind { Information, Success, Warning, Error };

// The name a person sees for an engine paragraph style ("Body" for the
// default style), and the engine name for one the picker offers.
std::string style_display_name(std::string_view engine_name);
std::string style_engine_name(std::string_view display_name);

struct RecentDocument {
  std::string display_name;
  std::string location_label;
  std::string path;
  std::string format_label;
  std::string last_opened_label;
  bool pinned{false};
};

struct PaletteItem {
  std::string command_id;
  std::string label;
  std::string detail;
  std::string shortcut;
  bool enabled{true};
  bool checked{false};
};

struct OutlineEntry {
  std::string title;
  int level{1};
  bool active{false};
  int word_count{0};
};

// A comment as the margin shows it. `anchor_y` is in document twips.
struct CommentCard {
  std::string id;
  std::string author;
  std::string initials;
  std::string time_label;
  std::string quote;
  std::string text;
  long long anchor_y{0};
  bool resolved{false};
};

// One entry of the Advanced view: a LibreOffice command Write does not give
// its own control, with a plain label.
struct AdvancedCommand {
  std::string uno_command;
  std::string label;
  std::string arguments;
  bool modifies_document{true};
};

struct AdvancedCommandGroup {
  std::string title;
  std::vector<AdvancedCommand> commands;
};

struct DocumentSnapshot {
  std::string title{"Untitled document"};
  std::string location_label{"Not saved yet"};
  std::string save_state_label{"Not saved"};
  std::string format_label{"DOCX"};
  std::string language_label;
  std::string selection_context{"Document"};
  // What the last search found. `search_matches` is only meaningful while
  // `search_ran` is true; a document nobody has searched has no count, which is
  // a different thing from a search that found nothing.
  bool search_ran{false};
  int search_matches{0};
  int search_current{0};
  int current_page{1};
  int page_count{1};
  int word_count{0};
  int character_count{0};
  // With text selected, the selection's counts; -1 otherwise.
  int selected_word_count{-1};
  int selected_character_count{-1};
  int compatibility_issue_count{0};
  int zoom_percent{100};
  // "auto", "fit-width", "fit-page" or "fixed".
  std::string zoom_mode{"auto"};
  std::string paragraph_style{"Body"};
  std::string typeface_label;
  std::string size_label;
  std::string weight_label;
  std::string line_height_label;
  bool typeface_mixed{false};
  bool size_mixed{false};
  bool line_height_mixed{false};
  std::string text_colour;
  std::string highlight_colour;
  // Exact paragraph spacing when supplied by the document engine, in points.
  std::optional<double> paragraph_before_pt;
  std::optional<double> paragraph_after_pt;
  std::string page_size_label{"Letter"};
  // An applied preset; empty when the engine has not reported the margins.
  std::string margins_label;
  std::string orientation_label{"Portrait"};
  std::vector<std::string> paragraph_styles;
  std::vector<OutlineEntry> outline;
  std::vector<CommentCard> comments;
  std::vector<std::string> collaborators;
  bool outline_ready{false};
  bool moon{false};
  bool header_on{false};
  bool header_state_known{false};
  bool footer_state_known{false};
  bool footer_on{false};
  bool track_changes{false};
  // The selected picture, in twips; empty when no picture is selected.
  bool picture_selected{false};
  long long picture_width_twips{0};
  long long picture_height_twips{0};
  std::string picture_preview_path;
  std::string picture_alt_text;
  bool picture_has_caption{false};
  std::string picture_flow;
  std::string picture_anchor;
  // Zero means the engine has not supplied the selected table's dimensions.
  int table_rows{0};
  int table_columns{0};
  bool modified{false};
  bool read_only{false};
  bool busy{false};
  bool current_page_available{false};
  bool word_count_available{false};
  bool character_count_available{false};
  bool language_available{false};
  bool style_metadata_available{false};
};

struct WriteWindowCallbacks {
  std::function<void(std::string_view command_id)> on_command;
  // Commands that carry a value: a style name, a colour, a zoom level, the
  // text to find. The window never interprets the value; the session does.
  std::function<void(std::string_view command_id, std::string_view argument)>
      on_command_argument;
  std::function<std::vector<PaletteItem>(std::string_view query)>
      on_search_commands;
  std::function<void(std::string_view path)> on_open_recent;
  // Return true to keep the window open (for example while asking whether to
  // save a dirty document).
  std::function<bool()> on_close_requested;
  std::function<void()> on_documents_requested;
  // The recent faces changed (a face was applied from a picker).
  std::function<void(const std::vector<std::string>& families)> on_recent_fonts_changed;
};

/**
 * Native GTK shell for one Luma Write document window.
 *
 * This class owns visible product surfaces but deliberately knows nothing about
 * the document-engine implementation. A real canvas is injected with
 * set_canvas(); until one is supplied, the canvas host states that the document
 * view is unavailable instead of drawing simulated document content.
 */
class WriteWindow final {
 public:
  [[nodiscard]] static std::unique_ptr<WriteWindow> create(
      GtkApplication* application, WriteWindowCallbacks callbacks,
      suite::SuiteIdentity identity =
          suite::identity_for(suite::ApplicationFamily::Write));

  ~WriteWindow();
  WriteWindow(const WriteWindow&) = delete;
  WriteWindow& operator=(const WriteWindow&) = delete;
  WriteWindow(WriteWindow&&) = delete;
  WriteWindow& operator=(WriteWindow&&) = delete;

  [[nodiscard]] GtkWindow* window() const noexcept;
  void present();

  void show_start_screen();
  void show_editor();
  void set_canvas(GtkWidget* canvas);
  [[nodiscard]] GtkWidget* canvas_host() const noexcept;

  void set_recent_documents(std::vector<RecentDocument> documents);
  void update_document(const DocumentSnapshot& snapshot);
  void update_command_state(std::string_view command_id,
                            const commands::CommandState& state);
  void set_font_families(std::vector<std::string> families);
  // The faces the person applied most recently, newest first.
  void set_recent_fonts(std::vector<std::string> families);
  void set_advanced_commands(std::vector<AdvancedCommandGroup> groups);
  void set_advanced_visible(bool visible);
  void show_find(bool replace);
  void show_inspector(const char* mode);
  void set_focus_mode(bool enabled);
  void show_more_menu();

  void show_command_palette();
  void show_about();
  void show_toast(std::string message,
                  ToastKind kind = ToastKind::Information,
                  unsigned timeout_ms = 3200);
  // A small sheet asking for text (a comment, a link). `on_done` receives the
  // fields in order, or is not called when the person cancels.
  void ask_for_text(std::string heading, std::string body,
                    std::vector<std::pair<std::string, std::string>> fields,
                    std::string action_label,
                    std::function<void(std::vector<std::string>)> on_done);

  // The window's parts (write_window_private.hpp), named here so the UI's
  // own helper types can refer to it.
  struct Impl;

 private:
  explicit WriteWindow(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace luma::write::ui
