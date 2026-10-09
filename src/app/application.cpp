#include <luma-ui.h>
#include <luma-appearance.h>
#include <gtk/gtkunixprint.h>
#include "app/application.hpp"

#include "canvas/document_canvas.hpp"
#include "commands/command_ids.h"
#include "commands/command_registry.h"
#include "compatibility/format_policy.hpp"
#include "compatibility/mvp_file_policy.hpp"
#include "engine/engine_factory.hpp"
#include "luma_config.hpp"
#include "persistence/atomic_save.hpp"
#include "ui/write_window.hpp"
#include "ui/comment_data.hpp"

#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>

#include <algorithm>
#include <charconv>
#include <clocale>
#include <cstdio>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luma::write::app {
namespace {

constexpr const char* kCompatibilityCommand = "write.document.compatibility";
constexpr const char* kAboutCommand = "suite.app.about";
constexpr const char* kQuitCommand = "suite.app.quit";
constexpr const char* kAlignLeftCommand = "write.paragraph.align-left";
constexpr const char* kAlignCenterCommand = "write.paragraph.align-center";
constexpr const char* kAlignRightCommand = "write.paragraph.align-right";
constexpr const char* kAlignJustifyCommand = "write.paragraph.align-justify";
constexpr const char* kBulletsCommand = "write.paragraph.bullets";
constexpr const char* kNumberingCommand = "write.paragraph.numbering";

class ApplicationState;
class WindowSession;

struct EngineEventBridge {
  std::mutex mutex;
  WindowSession* target{nullptr};
};

struct QueuedEngineEvent {
  std::shared_ptr<EngineEventBridge> bridge;
  engine::EngineEvent event;
};

struct EngineCommandBinding {
  std::string_view command_id;
  engine::EngineCommand engine_command;
  bool checkable{false};
};

constexpr std::array kEngineCommands{
    EngineCommandBinding{commands::ids::suite::kUndo,
                         engine::EngineCommand::undo, false},
    EngineCommandBinding{commands::ids::suite::kRedo,
                         engine::EngineCommand::redo, false},
    EngineCommandBinding{commands::ids::write::kBold,
                         engine::EngineCommand::bold, true},
    EngineCommandBinding{commands::ids::write::kItalic,
                         engine::EngineCommand::italic, true},
    EngineCommandBinding{commands::ids::write::kUnderline,
                         engine::EngineCommand::underline, true},
    EngineCommandBinding{commands::ids::write::kStrikethrough,
                         engine::EngineCommand::strikethrough, true},
    EngineCommandBinding{kAlignLeftCommand, engine::EngineCommand::align_left,
                         true},
    EngineCommandBinding{kAlignCenterCommand,
                         engine::EngineCommand::align_center, true},
    EngineCommandBinding{kAlignRightCommand,
                         engine::EngineCommand::align_right, true},
    EngineCommandBinding{kAlignJustifyCommand,
                         engine::EngineCommand::align_justify, true},
    EngineCommandBinding{kBulletsCommand,
                         engine::EngineCommand::toggle_bullets, true},
    EngineCommandBinding{kNumberingCommand,
                         engine::EngineCommand::toggle_numbering, true},
    EngineCommandBinding{commands::ids::write::kInsertPageBreak,
                         engine::EngineCommand::insert_page_break, false},
    EngineCommandBinding{commands::ids::write::kClearCharacterOverrides,
                         engine::EngineCommand::clear_character_overrides,
                         false},
    EngineCommandBinding{commands::ids::write::kClearParagraphOverrides,
                         engine::EngineCommand::clear_paragraph_overrides,
                         false},
    EngineCommandBinding{commands::ids::write::kSuperscript,
                         engine::EngineCommand::superscript, true},
    EngineCommandBinding{commands::ids::write::kSubscript,
                         engine::EngineCommand::subscript, true},
    EngineCommandBinding{commands::ids::suite::kSelectAll,
                         engine::EngineCommand::select_all, false},
    EngineCommandBinding{commands::ids::write::kTrackChanges,
                         engine::EngineCommand::track_changes, true},
    EngineCommandBinding{commands::ids::write::kNextChange,
                         engine::EngineCommand::next_change, false},
    EngineCommandBinding{commands::ids::write::kPreviousChange,
                         engine::EngineCommand::previous_change, false},
    EngineCommandBinding{commands::ids::write::kAcceptChange,
                         engine::EngineCommand::accept_change, false},
    EngineCommandBinding{commands::ids::write::kRejectChange,
                         engine::EngineCommand::reject_change, false},
};

// Plain commands Write gives a control of its own, dispatched by name. Each
// is in the palette with the words people use for it.
struct UnoCommandBinding {
  std::string_view command_id;
  const char* uno;
  const char* label;
  const char* description;
  std::initializer_list<const char*> synonyms;
  const char* shortcut;
  bool modifies;
  // The engine state that says whether this command is "on", if it has one.
  const char* state;
};

const std::array kUnoCommands{
    UnoCommandBinding{commands::ids::write::kGrowFont, ".uno:Grow", "Increase font size",
                      "Make the selected text one step larger", {"bigger text", "grow"},
                      "Primary+Shift+Period", true, nullptr},
    UnoCommandBinding{commands::ids::write::kShrinkFont, ".uno:Shrink", "Decrease font size",
                      "Make the selected text one step smaller", {"smaller text", "shrink"},
                      "Primary+Shift+Comma", true, nullptr},
    UnoCommandBinding{commands::ids::write::kFormatPainter, ".uno:FormatPaintbrush",
                      "Copy formatting", "Copy formatting from the selection to the next text you select",
                      {"format painter", "paint format", "clone formatting"}, "Primary+Alt+C",
                      false, ".uno:FormatPaintbrush"},
    UnoCommandBinding{commands::ids::write::kIndentMore, ".uno:IncrementIndent",
                      "Increase indent", "Indent the paragraph further", {"indent", "tab in"},
                      "Primary+BracketRight", true, nullptr},
    UnoCommandBinding{commands::ids::write::kIndentLess, ".uno:DecrementIndent",
                      "Decrease indent", "Move the paragraph back toward the margin",
                      {"outdent", "unindent"}, "Primary+BracketLeft", true, nullptr},
    UnoCommandBinding{commands::ids::write::kSpaceMore, ".uno:ParaspaceIncrease",
                      "More paragraph spacing", "Add space above and below the paragraph",
                      {"paragraph spacing", "space between paragraphs"}, nullptr, true, nullptr},
    UnoCommandBinding{commands::ids::write::kSpaceLess, ".uno:ParaspaceDecrease",
                      "Less paragraph spacing", "Remove space above and below the paragraph",
                      {"tighter paragraphs"}, nullptr, true, nullptr},
    UnoCommandBinding{commands::ids::write::kInsertFootnote, ".uno:InsertFootnote",
                      "Insert footnote", "Add a numbered note at the foot of the page",
                      {"footnote", "note", "citation"}, nullptr, true, nullptr},
    UnoCommandBinding{commands::ids::write::kUpdateContents, ".uno:UpdateAllIndexes",
                      "Update table of contents", "Refresh the table of contents and page numbers",
                      {"refresh contents", "update toc"}, nullptr, true, nullptr},
    UnoCommandBinding{commands::ids::write::kAcceptAllChanges, ".uno:AcceptAllTrackedChanges",
                      "Accept all changes", "Accept every tracked change in the document",
                      {"accept changes", "finalize"}, nullptr, true, nullptr},
    UnoCommandBinding{commands::ids::write::kRejectAllChanges, ".uno:RejectAllTrackedChanges",
                      "Reject all changes", "Undo every tracked change in the document",
                      {"reject changes", "revert changes"}, nullptr, true, nullptr},
    UnoCommandBinding{commands::ids::write::kSpelling, ".uno:SpellOnline",
                      "Check spelling as you type", "Underline misspelled words",
                      {"spell check", "spelling", "red underline"}, nullptr, false,
                      ".uno:SpellOnline"},
};

std::string display_path(const std::filesystem::path& path) {
  char* display = g_filename_display_name(path.c_str());
  std::string result = display == nullptr ? path.string() : display;
  g_free(display);
  return result;
}

std::string display_basename(const std::filesystem::path& path) {
  char* display = g_filename_display_basename(path.c_str());
  std::string result = display == nullptr ? path.filename().string() : display;
  g_free(display);
  return result;
}

std::string uppercase_extension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  if (!extension.empty() && extension.front() == '.') {
    extension.erase(extension.begin());
  }
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  return extension.empty() ? "FILE" : extension;
}

std::string selection_context_label(const std::string_view engine_context) {
  std::string lowered{engine_context};
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (lowered.find("table") != std::string::npos) {
    return "Table";
  }
  if (lowered.find("graphic") != std::string::npos ||
      lowered.find("image") != std::string::npos ||
      lowered.find("draw") != std::string::npos) {
    return "Image";
  }
  if (lowered.find("header") != std::string::npos) {
    return "Header";
  }
  if (lowered.find("footer") != std::string::npos) {
    return "Footer";
  }
  if (lowered.find("comment") != std::string::npos ||
      lowered.find("annotation") != std::string::npos) {
    return "Comment";
  }
  if (lowered.find("text") != std::string::npos ||
      lowered.find("paragraph") != std::string::npos) {
    return "Text";
  }
  return "Document";
}

std::string format_label(const engine::DocumentFormat format) {
  switch (format) {
    case engine::DocumentFormat::docx:
      return "DOCX";
    case engine::DocumentFormat::odt:
      return "ODT";
    case engine::DocumentFormat::legacy_doc:
      return "DOC";
    case engine::DocumentFormat::text:
      return "TXT";
    case engine::DocumentFormat::pdf:
      return "PDF";
    case engine::DocumentFormat::unknown:
      return "FILE";
  }
  return "FILE";
}

commands::CommandState ui_state_from_engine(
    const engine::CommandState& state, const bool document_available,
    const bool editable) {
  commands::CommandState result;
  result.enabled = document_available && editable &&
                   state.kind != engine::CommandStateKind::unavailable;
  switch (state.kind) {
    case engine::CommandStateKind::unavailable:
      break;
    case engine::CommandStateKind::off:
      result.checked = commands::CheckState::Unchecked;
      break;
    case engine::CommandStateKind::on:
      result.checked = commands::CheckState::Checked;
      break;
    case engine::CommandStateKind::mixed:
      result.checked = commands::CheckState::Mixed;
      break;
    case engine::CommandStateKind::value:
      break;
  }
  return result;
}

GtkFileFilter* editable_document_filter() {
  GtkFileFilter* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Editable documents");
  gtk_file_filter_add_mime_type(
      filter,
      "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
  gtk_file_filter_add_mime_type(filter,
                                "application/vnd.oasis.opendocument.text");
  gtk_file_filter_add_mime_type(filter, "application/msword");
  gtk_file_filter_add_mime_type(filter, "text/plain");
  return filter;
}

GtkFileFilter* docx_filter() {
  GtkFileFilter* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Word Document (.docx)");
  gtk_file_filter_add_pattern(filter, "*.docx");
  return filter;
}

GtkFileFilter* odt_filter() {
  GtkFileFilter* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "OpenDocument Text (.odt)");
  gtk_file_filter_add_pattern(filter, "*.odt");
  return filter;
}

GtkFileFilter* pdf_filter() {
  GtkFileFilter* filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "PDF Document (.pdf)");
  gtk_file_filter_add_pattern(filter, "*.pdf");
  return filter;
}

bool validate_pdf_output(const std::filesystem::path& path,
                         std::string& error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    error = "The exported PDF could not be reopened.";
    return false;
  }
  std::array<char, 5> header{};
  stream.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (stream.gcount() != static_cast<std::streamsize>(header.size()) ||
      std::string_view(header.data(), header.size()) != "%PDF-") {
    error = "The exported file does not have a PDF header.";
    return false;
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < std::streamoff{64}) {
    error = "The exported PDF is unexpectedly small.";
    return false;
  }
  const auto tail_size = std::min<std::streamoff>(size, 1024);
  stream.seekg(size - tail_size);
  std::string tail(static_cast<std::size_t>(tail_size), '\0');
  stream.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  if (tail.find("%%EOF") == std::string::npos) {
    error = "The exported PDF has no end-of-file marker.";
    return false;
  }
  return true;
}

bool supported_mvp_open_format(const std::filesystem::path& path,
                               std::string& error) {
  const auto detected = compatibility::detect_document_format(path);
  const auto& policy = compatibility::format_policy(detected);
  if (!policy.can_open || policy.open_mode == compatibility::OpenMode::ProtectedView) {
    error = std::string{policy.display_name} +
            " is not editable in this MVP. Open a DOCX, ODT, DOC, or plain-text file.";
    return false;
  }
  return detected == compatibility::DocumentFormat::Docx ||
         detected == compatibility::DocumentFormat::Odt ||
         detected == compatibility::DocumentFormat::LegacyDoc ||
         detected == compatibility::DocumentFormat::PlainText;
}

// JSON arguments for an engine command, built only from values Write chose.
std::string uno_string(const char* name, std::string_view value) {
  std::string escaped;
  for (const char character : value) {
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) < 0x20U) {
          char code[8];
          std::snprintf(code, sizeof(code), "\\u%04x", static_cast<unsigned>(character));
          escaped += code;
        } else {
          escaped.push_back(character);
        }
    }
  }
  return std::string{"\""} + name + "\":{\"type\":\"string\",\"value\":\"" + escaped + "\"}";
}

std::string uno_number(const char* name, const char* type, const long long value) {
  return std::string{"\""} + name + "\":{\"type\":\"" + type + "\",\"value\":" +
         std::to_string(value) + "}";
}

std::string uno_bool(const char* name, const bool value) {
  return std::string{"\""} + name + "\":{\"type\":\"boolean\",\"value\":" +
         (value ? "true" : "false") + "}";
}

std::string uno_args(std::initializer_list<std::string> parts) {
  std::string json = "{";
  bool first = true;
  for (const auto& part : parts) {
    if (!first) json += ",";
    json += part;
    first = false;
  }
  return json + "}";
}

std::vector<long long> integers_in(std::string_view text) {
  std::vector<long long> numbers;
  std::string digits;
  const auto flush = [&] {
    if (!digits.empty()) {
      numbers.push_back(std::atoll(digits.c_str()));
      digits.clear();
    }
  };
  for (const char character : text) {
    if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
      digits.push_back(character);
    } else if (character != ',' || digits.empty()) {
      flush();
    }
  }
  flush();
  return numbers;
}

long long colour_value(std::string_view hex) {
  if (hex == "auto" || hex.size() != 7 || hex.front() != '#') {
    return -1;  // COL_AUTO: the style's own colour.
  }
  long long value = 0;
  std::from_chars(hex.data() + 1, hex.data() + 7, value, 16);
  return value;
}

// The Advanced view: LibreOffice commands that act directly on the selection
// and need no dialog, grouped the way a person looks for them. Nothing here
// is shown in the default view.
std::vector<ui::AdvancedCommandGroup> advanced_catalog() {
  const auto group = [](const char* title,
                        std::initializer_list<std::pair<const char*, const char*>> items) {
    ui::AdvancedCommandGroup result{.title = title};
    for (const auto& [label, uno] : items) {
      result.commands.push_back({.uno_command = uno, .label = label});
    }
    return result;
  };
  return {
      group("Characters",
            {{"Small capitals", ".uno:SmallCaps"},
             {"UPPERCASE", ".uno:ChangeCaseToUpper"},
             {"lowercase", ".uno:ChangeCaseToLower"},
             {"Title Case", ".uno:ChangeCaseToTitleCase"},
             {"Sentence case", ".uno:ChangeCaseToSentenceCase"},
             {"tOGGLE cASE", ".uno:ChangeCaseRotateCase"},
             {"Double underline", ".uno:UnderlineDouble"},
             {"Overline", ".uno:Overline"},
             {"Shadowed", ".uno:Shadowed"},
             {"Outline font", ".uno:OutlineFont"},
             {"Clear direct formatting", ".uno:ResetAttributes"}}),
      group("Paragraph",
            {{"Left-to-right", ".uno:ParaLeftToRight"},
             {"Right-to-left", ".uno:ParaRightToLeft"},
             {"Single line spacing", ".uno:SpacePara1"},
             {"1.5 line spacing", ".uno:SpacePara15"},
             {"Double line spacing", ".uno:SpacePara2"},
             {"Update style from selection", ".uno:StyleUpdateByExample"}}),
      group("Lists",
            {{"Promote one level", ".uno:DecrementLevel"},
             {"Demote one level", ".uno:IncrementLevel"},
             {"Restart numbering", ".uno:NumberingStart"},
             {"Continue previous numbering", ".uno:ContinueNumbering"},
             {"Remove from list", ".uno:RemoveBullets"}}),
      group("Insert",
            {{"Line break", ".uno:InsertLinebreak"},
             {"Column break", ".uno:InsertColumnBreak"},
             {"Non-breaking space", ".uno:InsertNonBreakingSpace"},
             {"Soft hyphen", ".uno:InsertSoftHyphen"},
             {"Endnote", ".uno:InsertEndnote"},
             {"Date field", ".uno:InsertDateField"},
             {"Time field", ".uno:InsertTimeField"},
             {"Page number field", ".uno:InsertPageNumberField"},
             {"Page count field", ".uno:InsertPageCountField"},
             {"Title field", ".uno:InsertTitleField"}}),
      group("Tables",
            {{"Insert row above", ".uno:InsertRowsBefore"},
             {"Insert row below", ".uno:InsertRowsAfter"},
             {"Insert column before", ".uno:InsertColumnsBefore"},
             {"Insert column after", ".uno:InsertColumnsAfter"},
             {"Delete rows", ".uno:DeleteRows"},
             {"Delete columns", ".uno:DeleteColumns"},
             {"Delete table", ".uno:DeleteTable"},
             {"Merge cells", ".uno:MergeCells"},
             {"Select table", ".uno:SelectTable"},
             {"Optimal row height", ".uno:SetOptimalRowHeight"},
             {"Distribute columns evenly", ".uno:DistributeColumns"}}),
      group("Review",
            {{"Show tracked changes", ".uno:ShowTrackedChanges"},
             {"Previous change", ".uno:PreviousTrackedChange"},
             {"Next change", ".uno:NextTrackedChange"},
             {"Accept change", ".uno:AcceptTrackedChange"},
             {"Reject change", ".uno:RejectTrackedChange"},
             {"Accept all changes", ".uno:AcceptAllTrackedChanges"},
             {"Reject all changes", ".uno:RejectAllTrackedChanges"},
             {"Delete all comments", ".uno:DeleteAllNotes"}}),
      group("Document",
            {{"Update all fields and contents", ".uno:UpdateAll"},
             {"Update table of contents", ".uno:UpdateAllIndexes"},
             {"Rotate page orientation", ".uno:ToggleOrientation"},
             {"Formatting marks", ".uno:ControlCodes"},
             {"Field shadings", ".uno:Marks"}}),
  };
}

bool advanced_command_known(std::string_view uno) {
  for (const auto& group : advanced_catalog()) {
    for (const auto& command : group.commands) {
      if (command.uno_command == uno) {
        return true;
      }
    }
  }
  return false;
}

class WindowSession final {
 public:
  WindowSession(ApplicationState& owner, ApplicationOptions& options);
  ~WindowSession();
  WindowSession(const WindowSession&) = delete;
  WindowSession& operator=(const WindowSession&) = delete;

  void present();
  void execute_argument(std::string_view command_id, std::string_view argument);
 public:
  void start() { if (!has_document() && !luma_welcome_view_should_show(LUMA_APP_ID)) new_document(); }
  void new_document();
  void choose_open();
  void open_path(const std::filesystem::path& path);
  void save();
  void choose_save_as();
  void choose_export_pdf();
  void execute(std::string_view command_id);
  bool request_close();
  void request_documents();
  void return_to_documents();
  // The facts the platform's save sheet needs, or nullptr when nothing is
  // unsaved. A new reference.
  LumaSaveDocument* unsaved_document() const;
  // Acts on one answer from the save sheet, for a close or for a quit.
  void answer_save(LumaSaveRequest* request, LumaSaveChoice choice, GFile* destination);
  void close_without_saving();
  void deliver_engine_event(const engine::EngineEvent& event);
  std::vector<ui::PaletteItem> search(std::string_view query) const;
  void set_recent_documents(const std::vector<std::string>& paths);
  void show_warning(std::string message);
  [[nodiscard]] GtkWindow* gtk_window() const noexcept;
  [[nodiscard]] bool has_document() const noexcept {
    return engine_->has_document();
  }
  [[nodiscard]] bool is_closing() const noexcept { return closing_; }

 private:
  enum class AfterSave { None, CloseWindow };
  enum class PendingFileOperation { SaveDocument, ExportPdf };

  void register_commands();
  void register_feature_commands();
  void dispatch_uno(const char* uno, const std::string& arguments, bool modifies);
  void read_engine_state();
  void schedule_refresh();
  void schedule_outline(unsigned delay_ms);
  void refresh_outline();
  void apply_page_theme();
  void set_zoom(std::string_view level);
  void find(std::string_view text, bool backwards);
  void step_search(bool backwards);
  void replace(std::string_view argument, bool all);
  void choose_image();
  void add_comment();
  void add_link();
  void toggle_checklist();
  void insert_page_numbers();
  void set_page_part(bool header, bool shown);
  std::string current_page_style() const;
  void print_document();
  void toggle_focus_mode();
  void toggle_advanced();
  void run_advanced(std::string_view uno_command);
  void remember_zoom();
  void register_command(commands::CommandDefinition definition,
                        commands::CommandHandler handler,
                        commands::CommandState initial_state = {});
  void register_engine_command(const EngineCommandBinding& binding,
                               std::string label, std::string description,
                               std::vector<std::string> synonyms,
                               std::optional<std::string> shortcut = std::nullopt);
  void execute_engine(engine::EngineCommand command);
  void execute_canvas_command(std::string_view command_id);
  void install_document_view();
  void refresh_document();
  void refresh_comments();
  void refresh_command_states();
  void save_to_destination(
      const compatibility::PreparedDestination& destination,
      std::optional<persistence::TargetFingerprint> expected_target =
          std::nullopt);
  void finish_save_selection(GFile* file);
  void choose_direct_test_save(std::string_view initial_name);
  void export_to_destination(
      const compatibility::PreparedDestination& destination,
      std::optional<persistence::TargetFingerprint> expected_target =
          std::nullopt);
  void confirm_overwrite(compatibility::PreparedDestination destination,
                         PendingFileOperation operation);
  void finish_after_save();
  void cancel_after_save();
  void begin_close_prompt();
  void handle_engine_event(const engine::EngineEvent& event);
  void report_engine_failure(const engine::Result& result,
                             std::string_view operation);

  static void open_finished(GObject* source, GAsyncResult* result, gpointer data);
  static void save_finished(GObject* source, GAsyncResult* result, gpointer data);
  static void direct_test_save_accepted(GtkButton* button, gpointer data);
  static void direct_test_save_cancelled(GtkButton* button, gpointer data);
  static gboolean direct_test_save_closed(GtkWindow* window, gpointer data);
  static void export_finished(GObject* source, GAsyncResult* result, gpointer data);
  static void overwrite_finished(GObject* source, GAsyncResult* result,
                                 gpointer data);
  static void close_answered(LumaSaveRequest* request, LumaSaveChoice choice,
                             LumaSaveDocument* document, GFile* destination,
                             gpointer data);

  ApplicationState& owner_;
  ApplicationOptions& options_;
  std::unique_ptr<engine::DocumentEngine> engine_;
  std::unique_ptr<ui::WriteWindow> window_;
  commands::CommandRegistry commands_;
  GtkWidget* canvas_{nullptr};
  std::unique_ptr<canvas::DocumentCanvas> document_canvas_;
  engine::Result initialization_;
  ui::DocumentSnapshot snapshot_;
  std::shared_ptr<EngineEventBridge> event_bridge_;
  std::optional<std::filesystem::path> private_profile_directory_;
  std::optional<compatibility::PreparedDestination> pending_destination_;
  std::optional<persistence::TargetFingerprint> pending_target_fingerprint_;
  std::optional<persistence::TargetFingerprint> document_target_fingerprint_;
  GtkWindow* direct_test_save_dialog_{nullptr};
  GtkEntry* direct_test_save_name_{nullptr};
  PendingFileOperation pending_file_operation_{PendingFileOperation::SaveDocument};
  AfterSave after_save_{AfterSave::None};
  bool dialog_active_{false};
  // The save sheet waiting on a save it asked for; answered from
  // finish_after_save() and cancel_after_save().
  LumaSaveRequest* close_request_{nullptr};
  std::string save_problem_;
  LumaSurfacePolicy* surface_policy_{nullptr};
  gulong surface_changed_handler_{0};
  bool moon_{false};
  guint refresh_source_{0};
  guint outline_source_{0};
  std::vector<engine::OutlineHeading> headings_;
  bool outline_ready_{false};
  bool fonts_loaded_{false};
  bool comments_need_refresh_{true};
  bool focus_mode_{false};
  bool advanced_visible_{false};
  // What the next engine dialog should do: confirm it with its defaults (a
  // table of contents) or, for anything unexpected, close it.
  std::string confirm_dialog_;
  std::string search_text_;
  std::vector<std::string> search_rectangles_;
  bool search_counting_{false};
  bool search_step_backwards_{false};
  engine::Rect picture_twips_{};
  std::string pending_toast_after_replace_;
  bool force_close_{false};
  bool closing_{false};
  bool returning_to_documents_{false};
  bool removal_scheduled_{false};
};

class ApplicationState final {
 public:
  explicit ApplicationState(ApplicationOptions options)
      : options_(std::move(options)) {}

  ~ApplicationState() {
    if (hotfix_signal_ != 0) g_source_remove(hotfix_signal_);
    const std::vector<guint> pending_removals(removal_sources_.begin(),
                                              removal_sources_.end());
    for (const guint source : pending_removals) {
      g_source_remove(source);
    }
    removal_sources_.clear();
    windows_.clear();
    if (settings_ != nullptr) {
      g_object_unref(settings_);
    }
    if (application_ != nullptr) {
      g_object_unref(application_);
    }
  }

  int run(const int argc, char** argv) {
    g_set_application_name(LUMA_APP_NAME);
    const char* preview_id = g_getenv("LUMA_WRITE_PREVIEW_ID");
    const char* application_id = g_getenv("LUMA_WRITE_PREVIEW") == nullptr
        ? LUMA_APP_ID
        : (preview_id != nullptr && preview_id[0] != '\0'
               ? preview_id : "org.projectluma.Write.LumaUIPreview");
    application_ = gtk_application_new(application_id, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(application_, "startup", G_CALLBACK(startup), this);
    g_signal_connect(application_, "activate", G_CALLBACK(activate), this);
    g_signal_connect(application_, "open", G_CALLBACK(open), this);
    return g_application_run(G_APPLICATION(application_), argc, argv);
  }

  WindowSession& ensure_window() {
    // MVP invariant: the process owns either no session or exactly one live or
    // closing session. Never construct a replacement until removal completes.
    if (!windows_.empty()) {
      return *windows_.front();
    }
    auto session = std::make_unique<WindowSession>(*this, options_);
    WindowSession& reference = *session;
    windows_.push_back(std::move(session));
    reference.set_recent_documents(recent_paths_);
    return reference;
  }

  void schedule_remove(WindowSession* session) {
    struct RemovalRequest {
      ApplicationState* owner;
      WindowSession* session;
      guint source_id{0};
    };
    auto* request = new RemovalRequest{this, session, 0};
    request->source_id = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer data) -> gboolean {
          auto* queued = static_cast<RemovalRequest*>(data);
          queued->owner->removal_sources_.erase(queued->source_id);
          std::erase_if(queued->owner->windows_,
                        [queued](const auto& candidate) {
                          return candidate.get() == queued->session;
                        });
          return G_SOURCE_REMOVE;
        },
        request,
        [](gpointer data) { delete static_cast<RemovalRequest*>(data); });
    removal_sources_.insert(request->source_id);
  }

  void request_quit() {
    // Several unsaved documents: one sheet asks about all of them, then
    // reviews each in its own window. One or none: each window asks for itself.
    std::vector<LumaSaveDocument*> unsaved;
    for (const auto& session : windows_) {
      if (!session->is_closing()) {
        if (LumaSaveDocument* document = session->unsaved_document()) {
          unsaved.push_back(document);
        }
      }
    }
    if (unsaved.size() >= 2U) {
      GtkWindow* asking = gtk_application_get_active_window(application_);
      if (asking == nullptr) {
        asking = luma_save_document_get_window(unsaved.front());
      }
      LumaSaveRequest* request =
          luma_save_request_new(unsaved.data(), static_cast<guint>(unsaved.size()), TRUE);
      for (LumaSaveDocument* document : unsaved) {
        g_object_unref(document);
      }
      g_signal_connect(request, "response", G_CALLBACK(quit_answered), this);
      luma_save_request_present(request, asking);
      g_object_unref(request);
      return;
    }
    for (LumaSaveDocument* document : unsaved) {
      g_object_unref(document);
    }
    std::vector<GtkWindow*> windows;
    windows.reserve(windows_.size());
    for (const auto& session : windows_) {
      if (!session->is_closing()) {
        windows.push_back(session->gtk_window());
      }
    }
    for (GtkWindow* window : windows) {
      gtk_window_close(window);
    }
  }

  static void quit_answered(LumaSaveRequest* request, LumaSaveChoice choice,
                            LumaSaveDocument* document, GFile* destination,
                            gpointer data) {
    auto* self = static_cast<ApplicationState*>(data);
    if (choice == LUMA_SAVE_CHOICE_QUIT) {
      // Every document is answered, or the person chose Discard all.
      std::vector<WindowSession*> sessions;
      for (const auto& session : self->windows_) {
        if (!session->is_closing()) {
          sessions.push_back(session.get());
        }
      }
      for (WindowSession* session : sessions) {
        session->close_without_saving();
      }
      return;
    }
    if (document == nullptr) {
      return;
    }
    GtkWindow* window = luma_save_document_get_window(document);
    for (const auto& session : self->windows_) {
      if (window != nullptr && session->gtk_window() == window && !session->is_closing()) {
        session->answer_save(request, choice, destination);
        return;
      }
    }
  }

  void remember_path(const std::filesystem::path& path) {
    const std::string canonical = path.lexically_normal().string();
    const char* resume = g_getenv("LUMA_WRITE_HOTFIX_RESUME");
    if (g_getenv("LUMA_WRITE_PREVIEW") != nullptr && resume != nullptr) {
      g_file_set_contents(resume, canonical.c_str(), -1, nullptr);
    }
    recent_paths_.erase(
        std::remove(recent_paths_.begin(), recent_paths_.end(), canonical),
        recent_paths_.end());
    recent_paths_.insert(recent_paths_.begin(), canonical);
    if (recent_paths_.size() > 12U) {
      recent_paths_.resize(12U);
    }
    if (settings_ != nullptr) {
      std::vector<const char*> values;
      values.reserve(recent_paths_.size() + 1U);
      for (const std::string& value : recent_paths_) {
        values.push_back(value.c_str());
      }
      values.push_back(nullptr);
      g_settings_set_strv(settings_, "recent-files", values.data());
    }
    for (const auto& window : windows_) {
      if (!window->is_closing()) {
        window->set_recent_documents(recent_paths_);
      }
    }
  }

  GtkApplication* application() const noexcept { return application_; }
  GSettings* settings() const noexcept { return settings_; }

 private:
  void load_settings() {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    if (source == nullptr) {
      return;
    }
    GSettingsSchema* schema =
        g_settings_schema_source_lookup(source, LUMA_APP_ID, TRUE);
    if (schema == nullptr) {
      return;
    }
    settings_ = g_settings_new_full(schema, nullptr, nullptr);
    g_settings_schema_unref(schema);
    char** recent = g_settings_get_strv(settings_, "recent-files");
    if (recent != nullptr) {
      for (std::size_t index = 0; recent[index] != nullptr; ++index) {
        if (*recent[index] != '\0') {
          recent_paths_.emplace_back(recent[index]);
        }
      }
      g_strfreev(recent);
    }
  }

  static void startup(GApplication*, gpointer data) {
    auto* self = static_cast<ApplicationState*>(data);
    self->load_settings();
    if (g_getenv("LUMA_WRITE_HOTFIX") != nullptr &&
        g_getenv("LUMA_WRITE_PREVIEW") != nullptr) {
      self->hotfix_signal_ = g_unix_signal_add(SIGUSR1, +[](gpointer owner) -> gboolean {
        // A source rebuild uses the ordinary save-aware Quit workflow.
        static_cast<ApplicationState*>(owner)->request_quit();
        return G_SOURCE_CONTINUE;
      }, self);
    }
  }

  static void activate(GApplication*, gpointer data) {
    auto* self = static_cast<ApplicationState*>(data);
    WindowSession& session = self->ensure_window();
    if (!session.is_closing()) {
      session.start();
      session.present();
    }
  }

  static void open(GApplication*, GFile** files, const gint file_count,
                   const char*, gpointer data) {
    auto* self = static_cast<ApplicationState*>(data);
    WindowSession& session = self->ensure_window();
    if (session.is_closing()) {
      return;
    }
    session.present();
    if (file_count <= 0) {
      return;
    }
    if (session.has_document()) {
      session.show_warning(
          "Write opens one document at a time in this MVP. The current "
          "document remains open; the requested files were not opened.");
      return;
    }

    char* path = g_file_get_path(files[0]);
    if (path == nullptr) {
      session.show_warning(
          "Only local documents can be opened in this release.");
    } else {
      session.open_path(path);
    }
    g_free(path);

    if (file_count > 1) {
      session.show_warning(
          "Write opens one document at a time in this MVP. The additional "
          "startup files were not opened.");
    }
  }

  GtkApplication* application_{nullptr};
  guint hotfix_signal_{0};
  ApplicationOptions options_;
  GSettings* settings_{nullptr};
  std::vector<std::string> recent_paths_;
  std::vector<std::unique_ptr<WindowSession>> windows_;
  std::unordered_set<guint> removal_sources_;
};

WindowSession::WindowSession(ApplicationState& owner, ApplicationOptions& options)
    : owner_(owner),
      options_(options),
      engine_(engine::create_document_engine()) {
  engine::EngineConfiguration configuration;
  GError* profile_error = nullptr;
  char* profile = g_dir_make_tmp("luma-write-lok-XXXXXX", &profile_error);
  if (profile == nullptr) {
    initialization_ = engine::Result::failure(
        std::string{"Could not create a private LibreOffice profile: "} +
        (profile_error == nullptr ? "unknown error" : profile_error->message));
    g_clear_error(&profile_error);
  } else {
    private_profile_directory_ = std::filesystem::path{profile};
    configuration.private_profile_directory = private_profile_directory_;
    g_free(profile);
    initialization_ = engine_->initialize(configuration);
  }

  event_bridge_ = std::make_shared<EngineEventBridge>();
  event_bridge_->target = this;
  engine_->set_event_sink([bridge = event_bridge_](const engine::EngineEvent& event) {
    auto* queued = new QueuedEngineEvent{bridge, event};
    g_main_context_invoke_full(
        nullptr, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
          auto* pending = static_cast<QueuedEngineEvent*>(data);
          WindowSession* target = nullptr;
          {
            std::lock_guard lock(pending->bridge->mutex);
            target = pending->bridge->target;
          }
          if (target != nullptr) {
            target->deliver_engine_event(pending->event);
          }
          return G_SOURCE_REMOVE;
        },
        queued,
        [](gpointer data) { delete static_cast<QueuedEngineEvent*>(data); });
  });

  ui::WriteWindowCallbacks callbacks;
  callbacks.on_command = [this](const std::string_view command_id) {
    execute(command_id);
  };
  callbacks.on_command_argument = [this](const std::string_view command_id,
                                        const std::string_view argument) {
    execute_argument(command_id, argument);
  };
  callbacks.on_search_commands = [this](const std::string_view query) {
    return search(query);
  };
  callbacks.on_open_recent = [this](const std::string_view path) {
    open_path(std::filesystem::path{path});
  };
  callbacks.on_close_requested = [this] { return request_close(); };
  callbacks.on_documents_requested = [this] { request_documents(); };
  callbacks.on_recent_fonts_changed = [this](const std::vector<std::string>& families) {
    if (GSettings* settings = owner_.settings(); settings != nullptr) {
      std::vector<const char*> names;
      for (const auto& family : families) names.push_back(family.c_str());
      names.push_back(nullptr);
      g_settings_set_strv(settings, "recent-fonts", names.data());
    }
  };
  window_ = ui::WriteWindow::create(owner_.application(), std::move(callbacks));
  register_commands();
  register_feature_commands();
  refresh_command_states();

  // Moon or Sun follows the appearance until the person chooses: Dark and
  // Glass read as Moon, Light and Frost as Sun.
  surface_policy_ = luma_surface_policy_new(LUMA_SURFACE_TARGET_APPLICATION);
  surface_changed_handler_ = g_signal_connect(
      surface_policy_, "changed", G_CALLBACK(+[](LumaSurfacePolicy*, gpointer data) {
        static_cast<WindowSession*>(data)->apply_page_theme();
      }),
      this);
  if (GSettings* settings = owner_.settings(); settings != nullptr) {
    advanced_visible_ = g_settings_get_boolean(settings, "show-advanced");
    const bool show_outline = g_settings_get_boolean(settings, "show-navigator");
    static_cast<void>(show_outline);
  }
  if (GSettings* settings = owner_.settings(); settings != nullptr) {
    gchar** recent = g_settings_get_strv(settings, "recent-fonts");
    std::vector<std::string> families;
    for (gchar** name = recent; name != nullptr && *name != nullptr; ++name) {
      families.emplace_back(*name);
    }
    g_strfreev(recent);
    window_->set_recent_fonts(std::move(families));
  }
  window_->set_advanced_commands(advanced_catalog());
  window_->set_advanced_visible(advanced_visible_);
  apply_page_theme();
}

WindowSession::~WindowSession() {
  g_clear_object(&close_request_);
  for (guint* source : {&refresh_source_, &outline_source_}) {
    if (*source != 0) {
      g_source_remove(*source);
      *source = 0;
    }
  }
  if (surface_policy_ != nullptr) {
    g_signal_handler_disconnect(surface_policy_, surface_changed_handler_);
    g_clear_object(&surface_policy_);
  }
  {
    std::lock_guard lock(event_bridge_->mutex);
    event_bridge_->target = nullptr;
  }
  engine_->set_event_sink({});
  // Destroy GTK event controllers while their DocumentCanvas callback target
  // is still alive, then release the canvas and finally close the engine.
  if (document_canvas_ != nullptr) {
    document_canvas_->prepare_for_window_destruction();
  }
  window_.reset();
  canvas_ = nullptr;
  document_canvas_.reset();
  engine_->close();
  engine_.reset();
  if (private_profile_directory_.has_value()) {
    std::error_code ignored;
    std::filesystem::remove_all(*private_profile_directory_, ignored);
  }
}

void WindowSession::present() { window_->present(); }

GtkWindow* WindowSession::gtk_window() const noexcept { return window_->window(); }

void WindowSession::show_warning(std::string message) {
  window_->show_toast(std::move(message), ui::ToastKind::Warning, 5600);
}

void WindowSession::deliver_engine_event(const engine::EngineEvent& event) {
  if (!closing_) {
    handle_engine_event(event);
  }
}

void WindowSession::register_command(commands::CommandDefinition definition,
                                     commands::CommandHandler handler,
                                     commands::CommandState initial_state) {
  const std::string id = definition.id;
  const auto result = commands_.register_command(
      std::move(definition), {.execute = std::move(handler)});
  if (!result) {
    g_warning("Could not register command %s: %s", id.c_str(),
              result.message.c_str());
    return;
  }
  commands_.update_state(id, initial_state);
  window_->update_command_state(id, initial_state);
}

void WindowSession::register_engine_command(
    const EngineCommandBinding& binding, std::string label,
    std::string description, std::vector<std::string> synonyms,
    std::optional<std::string> shortcut) {
  register_command(
      {
          .id = std::string{binding.command_id},
          .label = std::move(label),
          .description = std::move(description),
          .search_synonyms = std::move(synonyms),
          .shortcut = std::move(shortcut),
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::EditableDocument,
          .checkable = binding.checkable,
      },
      [this, command = binding.engine_command] { execute_engine(command); },
      {.enabled = false});
}

void WindowSession::register_commands() {
  const auto add_global = [this](const std::string_view id, std::string label,
                                 std::string description,
                                 std::vector<std::string> synonyms,
                                 std::optional<std::string> shortcut,
                                 commands::CommandHandler handler) {
    register_command(
        {
            .id = std::string{id},
            .label = std::move(label),
            .description = std::move(description),
            .search_synonyms = std::move(synonyms),
            .shortcut = std::move(shortcut),
            .menu = {"", 0, 0},
            .context = commands::CommandContextRequirement::Global,
        },
        std::move(handler));
  };

  add_global(commands::ids::suite::kNewDocument, "New document",
             "Create a blank document", {"blank page", "new file"},
             "Primary+N", [this] { new_document(); });
  add_global(commands::ids::suite::kOpen, "Open…", "Open an existing document",
             {"open docx", "open odt", "browse"}, "Primary+O",
             [this] { choose_open(); });
  add_global(commands::ids::suite::kCommandPalette, "Search commands",
             "Find a command by name or plain-language description",
             {"command palette", "what can I do"}, "Primary+K",
             [this] { window_->show_command_palette(); });
  add_global(kAboutCommand, "About", "About this application", {"credits"},
             std::nullopt, [this] { window_->show_about(); });
  add_global(kQuitCommand, "Quit", "Close Write", {"exit"},
             "Primary+Q", [this] { owner_.request_quit(); });

  register_command(
      {
          .id = std::string{commands::ids::suite::kSave},
          .label = "Save",
          .description = "Save changes in the current document format",
          .search_synonyms = {"write file", "keep changes"},
          .shortcut = "Primary+S",
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::Document,
      },
      [this] { save(); }, {.enabled = false});
  register_command(
      {
          .id = std::string{commands::ids::suite::kSaveAs},
          .label = "Save As…",
          .description = "Save a DOCX or ODT copy",
          .search_synonyms = {"save copy", "change format"},
          .shortcut = "Primary+Shift+S",
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::Document,
      },
      [this] { choose_save_as(); }, {.enabled = false});
  register_command(
      {
          .id = std::string{commands::ids::suite::kExportPdf},
          .label = "Export PDF…",
          .description = "Create a PDF copy for sharing or printing",
          .search_synonyms = {"make pdf", "publish pdf"},
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::Document,
      },
      [this] { choose_export_pdf(); }, {.enabled = false});

  register_command(
      {
          .id = std::string{commands::ids::suite::kCut},
          .label = "Cut",
          .description = "Move the selected text to the clipboard",
          .search_synonyms = {"cut selection", "move to clipboard"},
          .shortcut = "Primary+X",
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::EditableDocument,
      },
      [this] {
        if (document_canvas_ != nullptr) {
          document_canvas_->cut_selection();
        }
      },
      {.enabled = false});
  register_command(
      {
          .id = std::string{commands::ids::suite::kCopy},
          .label = "Copy",
          .description = "Copy the selected text to the clipboard",
          .search_synonyms = {"copy selection", "clipboard"},
          .shortcut = "Primary+C",
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::Document,
      },
      [this] {
        if (document_canvas_ != nullptr) {
          document_canvas_->copy_selection();
        }
      },
      {.enabled = false});
  register_command(
      {
          .id = std::string{commands::ids::suite::kPaste},
          .label = "Paste",
          .description = "Paste plain text from the clipboard",
          .search_synonyms = {"paste clipboard", "insert copied text"},
          .shortcut = "Primary+V",
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::EditableDocument,
      },
      [this] {
        if (document_canvas_ != nullptr) {
          document_canvas_->paste_from_clipboard();
        }
      },
      {.enabled = false});

  for (const auto& binding : kEngineCommands) {
    if (binding.command_id == commands::ids::suite::kUndo) {
      register_engine_command(binding, "Undo", "Undo the last change",
                              {"go back"}, "Primary+Z");
    } else if (binding.command_id == commands::ids::suite::kRedo) {
      register_engine_command(binding, "Redo", "Redo the last undone change",
                              {"go forward"}, "Primary+Shift+Z");
    } else if (binding.command_id == commands::ids::write::kBold) {
      register_engine_command(binding, "Bold", "Apply or remove bold",
                              {"heavy text", "strong"}, "Primary+B");
    } else if (binding.command_id == commands::ids::write::kItalic) {
      register_engine_command(binding, "Italic", "Apply or remove italic",
                              {"emphasis", "slanted text"}, "Primary+I");
    } else if (binding.command_id == commands::ids::write::kUnderline) {
      register_engine_command(binding, "Underline", "Apply or remove underline",
                              {"underlined text"}, "Primary+U");
    } else if (binding.command_id == commands::ids::write::kStrikethrough) {
      register_engine_command(binding, "Strikethrough",
                              "Apply or remove strikethrough",
                              {"strike text", "cross out"});
    } else if (binding.command_id == kAlignLeftCommand) {
      register_engine_command(binding, "Align left", "Align the paragraph left",
                              {"left paragraph"});
    } else if (binding.command_id == kAlignCenterCommand) {
      register_engine_command(binding, "Align center",
                              "Center the current paragraph", {"center text"});
    } else if (binding.command_id == kAlignRightCommand) {
      register_engine_command(binding, "Align right", "Align the paragraph right",
                              {"right paragraph"});
    } else if (binding.command_id == kAlignJustifyCommand) {
      register_engine_command(binding, "Justify", "Align text to both margins",
                              {"full alignment"});
    } else if (binding.command_id == kBulletsCommand) {
      register_engine_command(binding, "Bulleted list", "Toggle a bulleted list",
                              {"bullet points", "unordered list"});
    } else if (binding.command_id == kNumberingCommand) {
      register_engine_command(binding, "Numbered list", "Toggle a numbered list",
                              {"ordered list", "numbering"});
    } else if (binding.command_id == commands::ids::write::kInsertPageBreak) {
      register_engine_command(binding, "Page break",
                              "Insert a page break at the caret",
                              {"new page", "insert page"});
    } else if (binding.command_id == commands::ids::write::kSuperscript) {
      register_engine_command(binding, "Superscript", "Raise the selected text",
                              {"exponent", "raised"}, "Primary+Period");
    } else if (binding.command_id == commands::ids::write::kSubscript) {
      register_engine_command(binding, "Subscript", "Lower the selected text",
                              {"chemical formula", "lowered"}, "Primary+Comma");
    } else if (binding.command_id == commands::ids::suite::kSelectAll) {
      register_engine_command(binding, "Select all", "Select the whole document",
                              {"select everything"}, "Primary+A");
    } else if (binding.command_id == commands::ids::write::kTrackChanges) {
      register_engine_command(binding, "Track changes",
                              "Record edits so others can review them",
                              {"suggesting", "review mode", "redline"});
    } else if (binding.command_id == commands::ids::write::kNextChange) {
      register_engine_command(binding, "Next change", "Go to the next tracked change",
                              {"next edit"});
    } else if (binding.command_id == commands::ids::write::kPreviousChange) {
      register_engine_command(binding, "Previous change",
                              "Go to the previous tracked change", {"previous edit"});
    } else if (binding.command_id == commands::ids::write::kAcceptChange) {
      register_engine_command(binding, "Accept change", "Keep the selected tracked change",
                              {"approve edit"});
    } else if (binding.command_id == commands::ids::write::kRejectChange) {
      register_engine_command(binding, "Reject change", "Undo the selected tracked change",
                              {"decline edit"});
    } else if (binding.command_id ==
               commands::ids::write::kClearCharacterOverrides) {
      register_engine_command(binding, "Clear character overrides",
                              "Remove only local character formatting",
                              {"remove weird formatting", "reset font"});
    } else if (binding.command_id ==
               commands::ids::write::kClearParagraphOverrides) {
      register_engine_command(binding, "Clear paragraph overrides",
                              "Remove only local paragraph formatting",
                              {"reset paragraph", "fix spacing"});
    }
  }

  register_command(
      {
          .id = kCompatibilityCommand,
          .label = "Compatibility details",
          .description = "Review document-format and font risks",
          .search_synonyms = {"missing fonts", "format warning"},
          .menu = {"", 0, 0},
          .context = commands::CommandContextRequirement::Document,
      },
      [this] {
        const auto status = engine_->status();
        const bool requires_safe_copy =
            !compatibility::can_save_in_place(status.identity.save_format);
        const bool requires_writable_copy = !status.source_writable;
        const bool requires_tracked_copy =
            status.identity.original_path.has_value() &&
            !requires_safe_copy && !requires_writable_copy &&
            !document_target_fingerprint_.has_value();
        if (status.missing_fonts.empty() && !requires_safe_copy &&
            !requires_writable_copy && !requires_tracked_copy) {
          window_->show_toast("No compatibility risks are currently reported.",
                              ui::ToastKind::Success);
          return;
        }
        std::string message;
        if (requires_safe_copy) {
          message += "This source format cannot be overwritten; save edits as DOCX or ODT. ";
        }
        if (status.identity.save_format == engine::DocumentFormat::legacy_doc) {
          message += "Complex legacy layouts can change on conversion. Keep the original and review the saved copy. ";
        }
        if (requires_writable_copy) {
          message += "The source is not writable; Save As is required. ";
        }
        if (requires_tracked_copy) {
          message +=
              "The source version cannot be verified safely; Save As is required. ";
        }
        if (!status.missing_fonts.empty()) {
          message += "Missing fonts: ";
          for (std::size_t index = 0; index < status.missing_fonts.size(); ++index) {
            if (index > 0) {
              message += ", ";
            }
            message += status.missing_fonts[index];
          }
        }
        window_->show_toast(std::move(message), ui::ToastKind::Warning, 5200);
      },
      {.enabled = false});

  const auto add_canvas_command = [this](const std::string_view id,
                                         std::string label,
                                         std::string description,
                                         std::vector<std::string> synonyms) {
    register_command(
        {
            .id = std::string{id},
            .label = std::move(label),
            .description = std::move(description),
            .search_synonyms = std::move(synonyms),
            .menu = {"", 0, 0},
            .context = commands::CommandContextRequirement::Document,
        },
        [this, command_id = std::string{id}] {
          execute_canvas_command(command_id);
        },
        {.enabled = false, .visible = false});
  };
  add_canvas_command(commands::ids::suite::kZoomIn, "Zoom in",
                     "Increase the document zoom", {"larger page"});
  add_canvas_command(commands::ids::suite::kZoomOut, "Zoom out",
                     "Decrease the document zoom", {"smaller page"});
  add_canvas_command(commands::ids::suite::kFitWidth, "Fit width",
                     "Fit the page width to the canvas", {"fill window"});
}

void WindowSession::register_feature_commands() {
  for (const auto& binding : kUnoCommands) {
    std::vector<std::string> synonyms;
    for (const char* synonym : binding.synonyms) {
      synonyms.emplace_back(synonym);
    }
    register_command(
        {
            .id = std::string{binding.command_id},
            .label = binding.label,
            .description = binding.description,
            .search_synonyms = std::move(synonyms),
            .shortcut = binding.shortcut == nullptr ? std::nullopt
                                                    : std::optional<std::string>{binding.shortcut},
            .menu = {"", 0, 0},
            .context = commands::CommandContextRequirement::EditableDocument,
            .checkable = binding.state != nullptr,
        },
        [this, uno = binding.uno, modifies = binding.modifies] {
          dispatch_uno(uno, "", modifies);
        },
        {.enabled = false});
  }
  const auto add = [this](const std::string_view id, std::string label, std::string description,
                          std::vector<std::string> synonyms, std::optional<std::string> shortcut,
                          const commands::CommandContextRequirement context,
                          commands::CommandHandler handler) {
    register_command(
        {
            .id = std::string{id},
            .label = std::move(label),
            .description = std::move(description),
            .search_synonyms = std::move(synonyms),
            .shortcut = std::move(shortcut),
            .menu = {"", 0, 0},
            .context = context,
        },
        std::move(handler), {.enabled = false});
  };
  using Context = commands::CommandContextRequirement;
  namespace w = commands::ids::write;
  add(w::kChecklist, "Checklist", "Start or remove a checklist item",
      {"todo", "to-do", "checkbox", "tick box"}, "Primary+Shift+9", Context::EditableDocument,
      [this] { toggle_checklist(); });
  add(w::kInsertLink, "Insert link…", "Link the selection to a web address",
      {"hyperlink", "url", "web address"}, "Primary+Shift+K", Context::EditableDocument,
      [this] { add_link(); });
  add(w::kAddComment, "Add comment…", "Comment on the selection",
      {"annotation", "review note", "note"}, "Primary+Alt+M", Context::EditableDocument,
      [this] { add_comment(); });
  add(w::kInsertImage, "Insert image…", "Place a picture at the caret",
      {"picture", "photo", "graphic"}, std::nullopt, Context::EditableDocument,
      [this] { choose_image(); });
  add(w::kInsertTable, "Insert table", "Insert a three-by-three table",
      {"grid", "rows and columns"}, std::nullopt, Context::EditableDocument, [this] {
        execute_argument(commands::ids::write::kInsertTableSized, "3x3");
      });
  add(w::kInsertContents, "Insert table of contents",
      "List the document's headings with their page numbers",
      {"toc", "contents", "index"}, std::nullopt, Context::EditableDocument, [this] {
        confirm_dialog_ = "TocDialog";
        dispatch_uno(".uno:InsertMultiIndex", "", true);
      });
  add(w::kInsertPageNumber, "Add page numbers", "Number every page in the footer",
      {"page numbers", "numbering pages", "footer number"}, std::nullopt,
      Context::EditableDocument, [this] { insert_page_numbers(); });
  // Header and footer switch on and off; the switch in the Page inspector
  // shows which the document has.
  add(w::kToggleHeader, "Header", "Show or remove the header at the top of every page",
      {"page header", "running head", "add header"}, std::nullopt, Context::EditableDocument,
      [this] { set_page_part(true, !snapshot_.header_on); });
  add(w::kToggleFooter, "Footer", "Show or remove the footer at the bottom of every page",
      {"page footer", "add footer"}, std::nullopt, Context::EditableDocument,
      [this] { set_page_part(false, !snapshot_.footer_on); });
  add(commands::ids::suite::kFind, "Find…", "Find text in the document",
      {"search text", "look for"}, "Primary+F", Context::Document,
      [this] { window_->show_find(false); });
  add(w::kReplace, "Find and replace…", "Replace text throughout the document",
      {"substitute", "change all"}, "Primary+H", Context::EditableDocument,
      [this] { window_->show_find(true); });
  add(commands::ids::suite::kPrint, "Print…", "Print the document in its own colours",
      {"printer", "paper copy"}, "Primary+P", Context::Document, [this] { print_document(); });
  add(w::kFocusMode, "Focus", "Fill the screen with the page", {"fullscreen", "distraction free"},
      "F11", Context::Document, [this] { toggle_focus_mode(); });
  add(w::kAdvancedView, "Advanced tools", "Show every tool the document engine offers",
      {"more tools", "power user", "all commands"}, std::nullopt, Context::Global,
      [this] { toggle_advanced(); });
}

void WindowSession::execute(const std::string_view command_id) {
  const auto result = commands_.execute(command_id);
  if (result == commands::ExecutionResult::MissingHandler ||
      result == commands::ExecutionResult::UnknownCommand) {
    window_->show_toast("That command is not available in this build.",
                        ui::ToastKind::Warning);
  }
}

std::vector<ui::PaletteItem> WindowSession::search(
    const std::string_view query) const {
  const auto matches = commands_.search(
      query, {.application = suite::ApplicationFamily::Write, .limit = 10});
  std::vector<ui::PaletteItem> results;
  results.reserve(matches.size());
  for (const auto& match : matches) {
    const auto* definition = commands_.definition(match.id);
    if (definition == nullptr) {
      continue;
    }
    results.push_back({
        .command_id = match.id,
        .label = match.label,
        .detail = definition->description,
        .shortcut = definition->shortcut.value_or(""),
        .enabled = match.state.enabled,
        .checked = match.state.checked == commands::CheckState::Checked,
    });
  }
  return results;
}

void WindowSession::report_engine_failure(const engine::Result& result,
                                          const std::string_view operation) {
  std::string message{operation};
  if (!result.message.empty()) {
    message += ": ";
    message += result.message;
  }
  window_->show_toast(std::move(message), ui::ToastKind::Error, 5600);
}

void WindowSession::new_document() {
  if (!initialization_.ok) {
    report_engine_failure(initialization_, "The document engine is unavailable");
    return;
  }
  if (engine_->has_document()) {
    window_->show_toast(
        "Write opens one document at a time in this MVP. Close the current "
        "document before creating another.",
        ui::ToastKind::Warning, 5600);
    return;
  }
  const auto result = engine_->create_blank();
  if (!result.ok) {
    report_engine_failure(result, "Could not create a blank document");
    return;
  }
  document_target_fingerprint_.reset();
  snapshot_.header_on = snapshot_.footer_on = false;
  snapshot_.header_state_known = snapshot_.footer_state_known = true;
  install_document_view();
  refresh_document();
  window_->show_editor();
  if (document_canvas_ != nullptr) {
    document_canvas_->refresh_document();
  }
  window_->show_toast("Blank document ready", ui::ToastKind::Success);
}

void WindowSession::choose_open() {
  if (dialog_active_) {
    return;
  }
  if (engine_->has_document()) {
    window_->show_toast(
        "Write opens one document at a time in this MVP. Close the current "
        "document before opening another.",
        ui::ToastKind::Warning, 5600);
    return;
  }
  dialog_active_ = true;
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Open a document");
  GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  GtkFileFilter* documents = editable_document_filter();
  g_list_store_append(filters, documents);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, documents);
  g_object_unref(documents);
  g_object_unref(filters);
  gtk_file_dialog_open(dialog, window_->window(), nullptr, open_finished, this);
  g_object_unref(dialog);
}

void WindowSession::open_finished(GObject* source, GAsyncResult* result,
                                  gpointer data) {
  auto* self = static_cast<WindowSession*>(data);
  self->dialog_active_ = false;
  GError* error = nullptr;
  GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == nullptr) {
    if (error != nullptr &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      self->window_->show_toast(error->message, ui::ToastKind::Error);
    }
    g_clear_error(&error);
    return;
  }
  char* path = g_file_get_path(file);
  if (path == nullptr) {
    self->window_->show_toast("Only local documents can be opened in this release.",
                              ui::ToastKind::Warning);
  } else {
    self->open_path(path);
  }
  g_free(path);
  g_object_unref(file);
}

void WindowSession::open_path(const std::filesystem::path& path) {
  if (!initialization_.ok) {
    report_engine_failure(initialization_, "The document engine is unavailable");
    return;
  }
  if (engine_->has_document()) {
    window_->show_toast(
        "Write opens one document at a time in this MVP. Close the current "
        "document before opening another.",
        ui::ToastKind::Warning, 5600);
    return;
  }
  std::string policy_error;
  if (!supported_mvp_open_format(path, policy_error)) {
    window_->show_toast(std::move(policy_error), ui::ToastKind::Warning, 5600);
    return;
  }
  std::error_code initial_fingerprint_error;
  const auto initial_fingerprint =
      persistence::AtomicFileSaver::capture_target_fingerprint(
          path, initial_fingerprint_error);
  const auto result = engine_->open(path);
  if (!result.ok) {
    report_engine_failure(result, "Could not open the document");
    return;
  }
  snapshot_.header_state_known = snapshot_.footer_state_known = false;
  snapshot_.header_on = snapshot_.footer_on = false;

  std::error_code fingerprint_error;
  auto opened_fingerprint =
      persistence::AtomicFileSaver::capture_target_fingerprint(
          path, fingerprint_error);
  if (initial_fingerprint.has_value() && opened_fingerprint.has_value() &&
      *initial_fingerprint == *opened_fingerprint) {
    document_target_fingerprint_ = std::move(opened_fingerprint);
  } else {
    document_target_fingerprint_.reset();
    std::string message =
        "The document opened, but Write could not safely track changes to "
        "its source file. Use Save As to preserve your work.";
    if (fingerprint_error) {
      message += " ";
      message += fingerprint_error.message();
    } else if (initial_fingerprint_error) {
      message += " ";
      message += initial_fingerprint_error.message();
    } else {
      message += " The source changed while it was opening.";
    }
    window_->show_toast(std::move(message), ui::ToastKind::Warning, 7200);
  }
  owner_.remember_path(path);
  if (engine_->status().read_only) {
    window_->show_toast("This document is open in another app, so it opened read-only. "
                        "Use Save As to keep a copy of your changes.",
                        ui::ToastKind::Warning, 7200);
  }
  install_document_view();
  refresh_document();
  window_->show_editor();
  if (document_canvas_ != nullptr) {
    document_canvas_->refresh_document();
  }
}

void WindowSession::install_document_view() {
  if (canvas_ != nullptr) {
    return;
  }
  if (options_.canvas_factory) {
    canvas_ = options_.canvas_factory(*engine_);
  } else {
    document_canvas_ = std::make_unique<canvas::DocumentCanvas>(*engine_);
    canvas_ = document_canvas_->widget();
  }
  if (canvas_ != nullptr) {
    window_->set_canvas(canvas_);
    if (document_canvas_ != nullptr) {
      document_canvas_->set_zoom_listener([this](double) {
        snapshot_.zoom_percent =
            static_cast<int>(std::lround(document_canvas_->zoom() * 100.0));
        const int page = document_canvas_->current_page();
        if (page != snapshot_.current_page) {
          snapshot_.current_page = page;
        }
        window_->update_document(snapshot_);
      });
      auto appearance = document_canvas_->page_appearance();
      appearance.moon = moon_;
      document_canvas_->set_page_appearance(appearance);
      document_canvas_->refresh_document();
    }
    for (const std::string_view id : {commands::ids::suite::kZoomIn,
                                      commands::ids::suite::kZoomOut,
                                      commands::ids::suite::kFitWidth}) {
      commands::CommandState state{.enabled = document_canvas_ != nullptr ||
                                       static_cast<bool>(
                                           options_.canvas_command_handler),
                                   .visible = true};
      commands_.update_state(id, state);
      window_->update_command_state(id, state);
    }
  }
}

void WindowSession::refresh_document() {
  if (!engine_->has_document()) {
    return;
  }
  read_engine_state();
  if (comments_need_refresh_) refresh_comments();
  if (!fonts_loaded_) {
    // The font list is fixed for the session; read it once.
    const std::string fonts = engine_->command_values(".uno:CharFontName");
    const auto values = fonts.find("\"commandValues\"");
    if (values != std::string::npos) {
      std::vector<std::string> families;
      std::size_t at = fonts.find('{', values);
      while (at != std::string::npos) {
        const auto open = fonts.find('"', at + 1);
        if (open == std::string::npos) break;
        const auto close = fonts.find('"', open + 1);
        if (close == std::string::npos) break;
        families.push_back(fonts.substr(open + 1, close - open - 1));
        at = fonts.find(']', close);
        if (at != std::string::npos) at = fonts.find_first_of(",}", at);
        if (at == std::string::npos || fonts[at] == '}') break;
      }
      if (!families.empty()) {
        fonts_loaded_ = true;
        // The document's own paragraph styles, for the style combo.
        const std::string styles = engine_->command_values(".uno:StyleApply");
        const auto list = styles.find("\"ParagraphStyles\"");
        if (list != std::string::npos) {
          const auto open = styles.find('[', list);
          const auto close = styles.find(']', open);
          std::vector<std::string> names;
          for (std::size_t quote = styles.find('"', open);
               quote != std::string::npos && quote < close;) {
            const auto end = styles.find('"', quote + 1);
            if (end == std::string::npos || end > close) break;
            names.push_back(styles.substr(quote + 1, end - quote - 1));
            quote = styles.find('"', end + 1);
          }
          snapshot_.paragraph_styles = std::move(names);
        }
        window_->set_font_families(std::move(families));
      }
    }
  }
  if (!outline_ready_ && outline_source_ == 0) {
    schedule_outline(200);
  }
  const auto status = engine_->status();
  snapshot_.modified = status.modified;
  snapshot_.read_only = status.read_only;
  snapshot_.page_count = std::max(status.page_count, 1);
  snapshot_.current_page = std::min(snapshot_.current_page, snapshot_.page_count);
  snapshot_.format_label = format_label(status.identity.save_format);
  const bool source_tracking_unavailable =
      status.identity.original_path.has_value() && status.source_writable &&
      compatibility::can_save_in_place(status.identity.save_format) &&
      !document_target_fingerprint_.has_value();
  snapshot_.compatibility_issue_count =
      static_cast<int>(status.missing_fonts.size()) +
      (!compatibility::can_save_in_place(status.identity.save_format) ? 1 : 0) +
      (!status.source_writable ? 1 : 0) +
      (source_tracking_unavailable ? 1 : 0);
  snapshot_.save_state_label = status.read_only ? "Read only"
                               : status.modified  ? "Edited"
                                                  : "Saved";
  if (status.identity.original_path.has_value()) {
    snapshot_.title = display_basename(*status.identity.original_path);
    snapshot_.location_label =
        display_path(status.identity.original_path->parent_path());
    if (!status.source_writable ||
        !compatibility::can_save_in_place(status.identity.save_format) ||
        source_tracking_unavailable) {
      snapshot_.save_state_label =
          status.modified ? "Unsaved — Save As required" : "Save As required";
    }
  } else {
    snapshot_.title = "Untitled document";
    snapshot_.location_label = "Not saved yet";
    snapshot_.save_state_label = "Not saved";
  }
  if (document_canvas_ != nullptr) {
    snapshot_.zoom_percent = static_cast<int>(
        std::lround(document_canvas_->zoom() * 100.0));
  }
  window_->update_document(snapshot_);
  refresh_command_states();
}

void WindowSession::refresh_comments() {
  comments_need_refresh_ = false;
  snapshot_.comments.clear();
  if (!engine_->has_document()) return;
  const std::string source = engine_->command_values(".uno:ViewAnnotations");
  snapshot_.comments = ui::comment_cards_from_json(source);
}

void WindowSession::refresh_command_states() {
  const bool has_document = engine_->has_document();
  const bool editable = has_document && !engine_->status().read_only;
  for (const EngineCommandBinding& binding : kEngineCommands) {
    auto state = ui_state_from_engine(engine_->command_state(binding.engine_command),
                                      has_document, editable);
    commands_.update_state(binding.command_id, state);
    window_->update_command_state(binding.command_id, state);
  }
  commands::CommandState save_state{.enabled = has_document && editable};
  commands_.update_state(commands::ids::suite::kSave, save_state);
  window_->update_command_state(commands::ids::suite::kSave, save_state);
  for (const std::string_view id : {commands::ids::suite::kSaveAs,
                                    commands::ids::suite::kExportPdf,
                                    std::string_view{kCompatibilityCommand}}) {
    commands::CommandState state{.enabled = has_document};
    commands_.update_state(id, state);
    window_->update_command_state(id, state);
  }
  const bool canvas_clipboard_available =
      has_document && document_canvas_ != nullptr;
  for (const std::string_view id : {commands::ids::suite::kCut,
                                    commands::ids::suite::kPaste}) {
    commands::CommandState state{
        .enabled = canvas_clipboard_available && editable};
    commands_.update_state(id, state);
    window_->update_command_state(id, state);
  }
  for (const auto& binding : kUnoCommands) {
    commands::CommandState state{.enabled = has_document && editable};
    if (binding.state != nullptr) {
      const auto value = engine_->command_value(binding.state).value_or("");
      state.checked = value == "true" ? commands::CheckState::Checked
                                      : commands::CheckState::Unchecked;
      if (value == "disabled") state.enabled = false;
    }
    commands_.update_state(binding.command_id, state);
    window_->update_command_state(binding.command_id, state);
  }
  namespace w = commands::ids::write;
  commands::CommandState highlight_state{.enabled = editable,
      .checked = !snapshot_.highlight_colour.empty() && snapshot_.highlight_colour != "-1"
          && snapshot_.highlight_colour != "auto" && snapshot_.highlight_colour != "none"
          && snapshot_.highlight_colour != "mixed" && snapshot_.highlight_colour != "indeterminate"
          ? commands::CheckState::Checked : commands::CheckState::Unchecked};
  if (snapshot_.highlight_colour == "mixed" || snapshot_.highlight_colour == "indeterminate")
    highlight_state.checked = commands::CheckState::Mixed;
  window_->update_command_state(w::kHighlight, highlight_state);
  for (const std::string_view id :
       {w::kChecklist, w::kInsertLink, w::kAddComment, w::kInsertImage, w::kInsertTable,
        w::kInsertContents, w::kInsertPageNumber, w::kToggleHeader, w::kToggleFooter, w::kReplace}) {
    commands::CommandState state{.enabled = has_document && editable};
    commands_.update_state(id, state);
    window_->update_command_state(id, state);
  }
  for (const std::string_view id : {commands::ids::suite::kFind, commands::ids::suite::kPrint,
                                    w::kFocusMode}) {
    commands::CommandState state{.enabled = has_document};
    commands_.update_state(id, state);
    window_->update_command_state(id, state);
  }
  commands::CommandState advanced_state{.enabled = true};
  advanced_state.checked =
      advanced_visible_ ? commands::CheckState::Checked : commands::CheckState::Unchecked;
  commands_.update_state(w::kAdvancedView, advanced_state);
  window_->update_command_state(w::kAdvancedView, advanced_state);
  commands::CommandState copy_state{
      .enabled = canvas_clipboard_available};
  commands_.update_state(commands::ids::suite::kCopy, copy_state);
  window_->update_command_state(commands::ids::suite::kCopy, copy_state);
}

void WindowSession::execute_engine(const engine::EngineCommand command) {
  if (!engine_->has_document()) {
    return;
  }
  // Lists are applied as the engine's own list styles, so a heading (whose
  // outline level otherwise swallows the default numbering) is numbered too,
  // with its number in the paragraph's own face and size and a proper gap
  // before the text. Consecutive paragraphs continue the same list. A second
  // press takes the paragraph out of the list.
  if (command == engine::EngineCommand::toggle_numbering ||
      command == engine::EngineCommand::toggle_bullets) {
    const bool numbering = command == engine::EngineCommand::toggle_numbering;
    // The engine's status line names the paragraph's list ("Numbering 123 :
    // Level 1"); a heading's outline level alone ("Outline 1") is no list,
    // though the engine reports its numbering as on.
    const std::string status_line =
        engine_->command_value(".uno:StateTableCell").value_or("");
    const auto level_at = status_line.find(" : Level ");
    const std::string list_name =
        level_at == std::string::npos ? std::string{} : status_line.substr(0, level_at);
    const bool in_list = !list_name.empty() && list_name != "Heading Numbering" &&
                         list_name.rfind("Outline", 0) != 0;
    const auto on = in_list && engine_->command_state(command).kind == engine::CommandStateKind::on;
    g_debug("Write list: %s, %s (%s)", numbering ? "numbering" : "bullets",
            on ? "remove" : "apply", status_line.c_str());
    if (on) {
      dispatch_uno(".uno:RemoveBullets", "", true);
    } else {
      dispatch_uno(".uno:StyleApply",
                   uno_args({uno_string("Style", numbering ? "Numbering 123" : "List 1"),
                             uno_string("FamilyName", "NumberingStyles")}),
                   true);
    }
    return;
  }
  const auto result = engine_->execute({.command = command,
                                        .arguments = {},
                                        .request_completion = true});
  if (!result.ok) {
    report_engine_failure(result, "The command could not be completed");
    return;
  }
  refresh_document();
}

void WindowSession::execute_canvas_command(const std::string_view command_id) {
  if (document_canvas_ != nullptr) {
    double zoom = document_canvas_->zoom();
    if (command_id == commands::ids::suite::kZoomIn) {
      zoom *= 1.10;
    } else if (command_id == commands::ids::suite::kZoomOut) {
      zoom /= 1.10;
    } else if (command_id == commands::ids::suite::kFitWidth) {
      set_zoom("fit-width");
      return;
    } else if (false) {
      const auto status = engine_->status();
      const int allocation = gtk_widget_get_width(document_canvas_->widget());
      const double document_pixels =
          static_cast<double>(std::max<std::int64_t>(status.extent_twips.width,
                                                     11906)) *
          (96.0 / 1440.0);
      if (allocation > 160 && document_pixels > 1.0) {
        zoom = static_cast<double>(allocation - 112) / document_pixels;
      }
    } else {
      return;
    }
    document_canvas_->set_zoom(zoom);
    snapshot_.zoom_mode = "fixed";
    remember_zoom();
    return;
  }
  if (canvas_ != nullptr && options_.canvas_command_handler &&
      options_.canvas_command_handler(canvas_, command_id)) {
    return;
  }
  window_->show_toast("That view command is unavailable in this canvas.",
                      ui::ToastKind::Warning);
}

void WindowSession::save() {
  if (dialog_active_) {
    return;
  }
  if (!engine_->has_document()) {
    cancel_after_save();
    return;
  }
  const auto status = engine_->status();
  if (!status.identity.original_path.has_value() ||
      !status.source_writable ||
      !compatibility::can_save_in_place(status.identity.save_format)) {
    choose_save_as();
    return;
  }
  if (!document_target_fingerprint_.has_value()) {
    window_->show_toast(
        "Write cannot verify that the source file is still the version you "
        "opened. Choose Save As so no newer file is overwritten.",
        ui::ToastKind::Warning, 7200);
    choose_save_as();
    return;
  }
  const auto destination = compatibility::prepare_editable_destination(
      *status.identity.original_path);
  if (!destination.ok) {
    save_problem_ = destination.message;
    window_->show_toast(destination.message, ui::ToastKind::Error, 5600);
    cancel_after_save();
    return;
  }
  save_to_destination(destination, document_target_fingerprint_);
}

void WindowSession::choose_save_as() {
  if (!engine_->has_document() || dialog_active_) {
    if (!engine_->has_document()) {
      cancel_after_save();
    }
    return;
  }
  dialog_active_ = true;
  const auto status = engine_->status();
  const bool prefer_odt =
      status.identity.save_format == engine::DocumentFormat::odt;
  std::string initial_name =
      prefer_odt ? "Untitled document.odt" : "Untitled document.docx";
  if (const char* direct_test_dialog =
          g_getenv("LUMA_UI_TEST_DIRECT_FILE_CHOOSER");
      direct_test_dialog != nullptr &&
      std::string_view{direct_test_dialog} == "1") {
    choose_direct_test_save(initial_name);
    return;
  }
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Save document as");
  if (status.identity.original_path.has_value()) {
    auto suggested = *status.identity.original_path;
    suggested.replace_extension(prefer_odt ? ".odt" : ".docx");
    initial_name = display_basename(suggested);
    GFile* folder = g_file_new_for_path(
        status.identity.original_path->parent_path().c_str());
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_object_unref(folder);
  }
  gtk_file_dialog_set_initial_name(dialog, initial_name.c_str());
  GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  GtkFileFilter* docx = docx_filter();
  GtkFileFilter* odt = odt_filter();
  g_list_store_append(filters, docx);
  g_list_store_append(filters, odt);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, prefer_odt ? odt : docx);
  g_object_unref(docx);
  g_object_unref(odt);
  g_object_unref(filters);
  gtk_file_dialog_save(dialog, window_->window(), nullptr, save_finished, this);
  g_object_unref(dialog);
}

void WindowSession::save_finished(GObject* source, GAsyncResult* result,
                                  gpointer data) {
  auto* self = static_cast<WindowSession*>(data);
  self->dialog_active_ = false;
  GError* error = nullptr;
  GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == nullptr) {
    if (error != nullptr &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      self->window_->show_toast(error->message, ui::ToastKind::Error);
    }
    g_clear_error(&error);
    self->cancel_after_save();
    return;
  }
  self->finish_save_selection(file);
  g_object_unref(file);
}

void WindowSession::finish_save_selection(GFile* file) {
  char* path = g_file_get_path(file);
  if (path != nullptr) {
    auto destination = compatibility::prepare_editable_destination(path);
    if (!destination.ok) {
      save_problem_ = destination.message;
      window_->show_toast(destination.message, ui::ToastKind::Error, 5600);
      cancel_after_save();
    } else {
      confirm_overwrite(std::move(destination),
                        PendingFileOperation::SaveDocument);
    }
  } else {
    window_->show_toast(
        "Only local document destinations are supported in this MVP.",
        ui::ToastKind::Warning);
    cancel_after_save();
  }
  g_free(path);
}

void WindowSession::choose_direct_test_save(const std::string_view initial_name) {
  // Nested rootless build containers cannot run the portal backend's bwrap
  // image loader. Keep the production GtkFileDialog untouched and expose this
  // minimal, explicitly gated transport only to the native acceptance runner.
  GtkWindow* dialog = GTK_WINDOW(gtk_window_new());
  direct_test_save_dialog_ = dialog;
  gtk_window_set_title(dialog, "Save document as");
  gtk_window_set_transient_for(dialog, window_->window());
  gtk_window_set_modal(dialog, true);
  gtk_window_set_resizable(dialog, false);
  gtk_window_set_default_size(dialog, 520, -1);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 18);
  gtk_widget_set_margin_bottom(content, 18);
  gtk_widget_set_margin_start(content, 18);
  gtk_widget_set_margin_end(content, 18);
  gtk_window_set_child(dialog, content);

  GtkWidget* label = gtk_label_new("File name");
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(content), label);

  GtkWidget* name = gtk_entry_new();
  direct_test_save_name_ = GTK_ENTRY(name);
  gtk_editable_set_text(GTK_EDITABLE(name), std::string{initial_name}.c_str());
  gtk_entry_set_activates_default(GTK_ENTRY(name), true);
  gtk_accessible_update_property(GTK_ACCESSIBLE(name),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "File name", -1);
  gtk_box_append(GTK_BOX(content), name);

  GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(actions, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(content), actions);
  GtkWidget* cancel = gtk_button_new_with_label("Cancel");
  GtkWidget* accept = gtk_button_new_with_label("Save");
  gtk_widget_add_css_class(accept, "suggested-action");
  gtk_box_append(GTK_BOX(actions), cancel);
  gtk_box_append(GTK_BOX(actions), accept);
  gtk_window_set_default_widget(dialog, accept);

  g_signal_connect(cancel, "clicked", G_CALLBACK(direct_test_save_cancelled),
                   this);
  g_signal_connect(accept, "clicked", G_CALLBACK(direct_test_save_accepted),
                   this);
  g_signal_connect(dialog, "close-request", G_CALLBACK(direct_test_save_closed),
                   this);
  gtk_window_present(dialog);
  gtk_widget_grab_focus(name);
}

void WindowSession::direct_test_save_accepted(GtkButton*, gpointer data) {
  auto* self = static_cast<WindowSession*>(data);
  if (self->direct_test_save_dialog_ == nullptr ||
      self->direct_test_save_name_ == nullptr) {
    return;
  }
  const char* name =
      gtk_editable_get_text(GTK_EDITABLE(self->direct_test_save_name_));
  if (name == nullptr || *name == '\0') {
    self->window_->show_toast("Enter a file name.", ui::ToastKind::Warning);
    return;
  }
  GtkWindow* dialog = self->direct_test_save_dialog_;
  self->direct_test_save_dialog_ = nullptr;
  self->direct_test_save_name_ = nullptr;
  self->dialog_active_ = false;
  GFile* file = g_file_new_for_commandline_arg(name);
  self->finish_save_selection(file);
  g_object_unref(file);
  gtk_window_destroy(dialog);
}

void WindowSession::direct_test_save_cancelled(GtkButton*, gpointer data) {
  auto* self = static_cast<WindowSession*>(data);
  if (self->direct_test_save_dialog_ == nullptr) {
    return;
  }
  GtkWindow* dialog = self->direct_test_save_dialog_;
  self->direct_test_save_dialog_ = nullptr;
  self->direct_test_save_name_ = nullptr;
  self->dialog_active_ = false;
  self->cancel_after_save();
  gtk_window_destroy(dialog);
}

gboolean WindowSession::direct_test_save_closed(GtkWindow*, gpointer data) {
  auto* self = static_cast<WindowSession*>(data);
  if (self->direct_test_save_dialog_ != nullptr) {
    self->direct_test_save_dialog_ = nullptr;
    self->direct_test_save_name_ = nullptr;
    self->dialog_active_ = false;
    self->cancel_after_save();
  }
  return FALSE;
}

void WindowSession::save_to_destination(
    const compatibility::PreparedDestination& destination,
    std::optional<persistence::TargetFingerprint> expected_target) {
  snapshot_.busy = true;
  snapshot_.save_state_label = "Saving…";
  window_->update_document(snapshot_);

  persistence::AtomicSaveOptions save_options;
  save_options.require_target_absent =
      !destination.requires_overwrite_confirmation &&
      !expected_target.has_value();
  save_options.expected_existing_target = std::move(expected_target);
  const auto saved = persistence::AtomicFileSaver::save_via_path(
      destination.path,
      [this, format = destination.format](const std::filesystem::path& staged,
                                           std::string& error) {
        const auto result = engine_->save_as({
            .destination = staged,
            .format = format,
            .take_ownership = false,
        });
        error = result.message;
        return result.ok;
      },
      [this](const std::filesystem::path& staged, std::string& error) {
        const auto result = engine_->validate_saved_document(staged);
        error = result.message;
        return result.ok;
      },
      save_options);

  snapshot_.busy = false;
  if (!saved.saved()) {
    std::string message = saved.message.empty()
                              ? "The document was not saved."
                              : saved.message;
    if (saved.recovery_path.has_value()) {
      message += " A validated recovery copy remains at ";
      message += display_path(*saved.recovery_path);
      message += ".";
    }
    save_problem_ = message;
    window_->show_toast(std::move(message), ui::ToastKind::Error, 7200);
    cancel_after_save();
    refresh_document();
    return;
  }

  if (saved.outcome == persistence::SaveOutcome::SavedDurabilityUncertain) {
    // The target name may now contain the validated bytes, but the containing
    // directory was not durably synchronized. Do not adopt the path, clear the
    // dirty bit, or honor a deferred close: the live model is still the only
    // copy whose continued existence we can guarantee.
    document_target_fingerprint_.reset();
    std::string message =
        "The validated file was written, but durable storage could not be "
        "confirmed. This document remains changed and open; use Save As to "
        "retry before closing.";
    if (!saved.message.empty()) {
      message += " ";
      message += saved.message;
    }
    save_problem_ = message;
    window_->show_toast(std::move(message), ui::ToastKind::Error, 7200);
    cancel_after_save();
    refresh_document();
    return;
  }

  std::error_code fingerprint_error;
  auto saved_fingerprint =
      persistence::AtomicFileSaver::capture_target_fingerprint(
          destination.path, fingerprint_error);
  if (!saved_fingerprint.has_value()) {
    document_target_fingerprint_.reset();
    std::string message =
        "The validated file was written, but Write could not safely track "
        "the saved version. The document remains marked as changed; use Save As "
        "before closing.";
    if (fingerprint_error) {
      message += " ";
      message += fingerprint_error.message();
    }
    save_problem_ = message;
    window_->show_toast(std::move(message), ui::ToastKind::Error, 7200);
    cancel_after_save();
    refresh_document();
    return;
  }

  const auto adopted =
      engine_->adopt_saved_document(destination.path, destination.format);
  if (!adopted.ok) {
    document_target_fingerprint_.reset();
    report_engine_failure(
        adopted,
        "The validated file was saved, but its document identity could not be updated");
    save_problem_ = "The file was saved, but Write could not take it over as this "
                    "document. It stays open and marked as changed.";
    cancel_after_save();
    refresh_document();
    return;
  }

  document_target_fingerprint_ = std::move(saved_fingerprint);
  owner_.remember_path(destination.path);
  refresh_document();
  window_->show_toast("Saved, validated, and safely replaced",
                      ui::ToastKind::Success);
  finish_after_save();
}

void WindowSession::confirm_overwrite(
    compatibility::PreparedDestination destination,
    const PendingFileOperation operation) {
  if (!destination.requires_overwrite_confirmation) {
    if (operation == PendingFileOperation::SaveDocument) {
      save_to_destination(destination);
    } else {
      export_to_destination(destination);
    }
    return;
  }

  std::error_code fingerprint_error;
  auto fingerprint = persistence::AtomicFileSaver::capture_target_fingerprint(
      destination.path, fingerprint_error);
  if (!fingerprint.has_value()) {
    std::string message =
        "The existing destination changed before it could be confirmed.";
    if (fingerprint_error) {
      message += " ";
      message += fingerprint_error.message();
    }
    window_->show_toast(std::move(message), ui::ToastKind::Error, 7200);
    if (operation == PendingFileOperation::SaveDocument) {
      cancel_after_save();
    }
    return;
  }

  pending_destination_ = std::move(destination);
  pending_target_fingerprint_ = std::move(fingerprint);
  pending_file_operation_ = operation;
  dialog_active_ = true;
  const std::string heading = "Replace “" + pending_destination_->path.filename().string() + "”?";
  AdwAlertDialog* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
      heading.c_str(), "The existing file will be replaced only after the new output is written and validated."));
  adw_alert_dialog_add_responses(dialog, "cancel", "Cancel", "replace", "Replace", nullptr);
  adw_alert_dialog_set_response_appearance(dialog, "replace", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response(dialog, "cancel");
  adw_alert_dialog_set_default_response(dialog, "replace");
  adw_alert_dialog_choose(dialog, GTK_WIDGET(window_->window()), nullptr,
      overwrite_finished, new std::shared_ptr<EngineEventBridge>(event_bridge_));
}

void WindowSession::overwrite_finished(GObject* source, GAsyncResult* result,
                                       gpointer data) {
  const std::string choice = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  std::unique_ptr<std::shared_ptr<EngineEventBridge>> bridge(
      static_cast<std::shared_ptr<EngineEventBridge>*>(data));
  auto* self = (*bridge)->target;
  if (self == nullptr) return;
  self->dialog_active_ = false;

  if (choice != "replace" || !self->pending_destination_.has_value()) {
    if (self->pending_file_operation_ == PendingFileOperation::SaveDocument) {
      self->cancel_after_save();
    }
    self->pending_destination_.reset();
    self->pending_target_fingerprint_.reset();
    return;
  }

  auto destination = std::move(*self->pending_destination_);
  auto expected_target = std::move(self->pending_target_fingerprint_);
  self->pending_destination_.reset();
  self->pending_target_fingerprint_.reset();
  if (self->pending_file_operation_ == PendingFileOperation::SaveDocument) {
    self->save_to_destination(destination, std::move(expected_target));
  } else {
    self->export_to_destination(destination, std::move(expected_target));
  }
}

void WindowSession::finish_after_save() {
  if (close_request_ != nullptr) {
    // Saved: the sheet goes, and a quit moves on to the next document.
    LumaSaveRequest* request = std::exchange(close_request_, nullptr);
    save_problem_.clear();
    luma_save_request_finish_save(request, nullptr);
    g_object_unref(request);
  }
  if (after_save_ != AfterSave::CloseWindow) {
    after_save_ = AfterSave::None;
    return;
  }
  after_save_ = AfterSave::None;
  close_without_saving();
}

void WindowSession::cancel_after_save() {
  after_save_ = AfterSave::None;
  if (close_request_ == nullptr) {
    return;
  }
  LumaSaveRequest* request = std::exchange(close_request_, nullptr);
  if (!save_problem_.empty()) {
    // It could not be saved: say why on the sheet, which stays up to try
    // again, choose another place, or not save.
    luma_save_request_finish_save(request, save_problem_.c_str());
  } else {
    // The person cancelled the save (its file chooser): keep editing.
    returning_to_documents_ = false;
    luma_save_request_cancel(request);
  }
  save_problem_.clear();
  g_object_unref(request);
}

void WindowSession::request_documents() {
  if (dialog_active_ || luma_save_sheet_get_for_window(window_->window()) != nullptr) return;
  returning_to_documents_ = true;
  if (engine_->has_document() && engine_->status().modified) begin_close_prompt();
  else return_to_documents();
}

void WindowSession::return_to_documents() {
  returning_to_documents_ = false;
  engine_->close();
  snapshot_.margins_label.clear();
  document_target_fingerprint_.reset();
  window_->show_start_screen();
}

bool WindowSession::request_close() {
  if (closing_) {
    return false;
  }
  if (force_close_) {
    closing_ = true;
    if (!removal_scheduled_) {
      removal_scheduled_ = true;
      owner_.schedule_remove(this);
    }
    return false;
  }
  if (luma_save_sheet_get_for_window(window_->window()) != nullptr) {
    gtk_window_present(window_->window());
    return true;
  }
  if (dialog_active_) {
    window_->show_toast("Finish or cancel the open dialog before closing.",
                        ui::ToastKind::Warning);
    return true;
  }
  if (!engine_->has_document() || !engine_->status().modified) {
    closing_ = true;
    if (!removal_scheduled_) {
      removal_scheduled_ = true;
      owner_.schedule_remove(this);
    }
    return false;
  }
  begin_close_prompt();
  return true;
}

LumaSaveDocument* WindowSession::unsaved_document() const {
  if (!engine_->has_document() || !engine_->status().modified) {
    return nullptr;
  }
  const auto status = engine_->status();
  LumaSaveDocument* document = luma_save_document_new(snapshot_.title.c_str());
  // Write does not record when the first unsaved edit happened, so the sheet
  // states no edit age.
  luma_save_document_set_never_saved(document, !status.identity.original_path.has_value());
  luma_save_document_set_extension(
      document, status.identity.save_format == engine::DocumentFormat::odt ? ".odt" : ".docx");
  luma_save_document_set_window(document, window_->window());
  return document;
}

void WindowSession::begin_close_prompt() {
  // The platform's save sheet: Save on Enter, Cancel on Escape, and Don't
  // save only by choice or Ctrl+D. An untitled document is named and placed
  // on the sheet.
  LumaSaveDocument* document = unsaved_document();
  if (document == nullptr) {
    return;
  }
  dialog_active_ = true;
  LumaSaveRequest* request = luma_save_request_new(&document, 1, FALSE);
  g_object_unref(document);
  g_signal_connect_data(
      request, "response", G_CALLBACK(close_answered),
      new std::shared_ptr<EngineEventBridge>(event_bridge_),
      [](gpointer data, GClosure*) {
        delete static_cast<std::shared_ptr<EngineEventBridge>*>(data);
      },
      static_cast<GConnectFlags>(0));
  luma_save_request_present(request, window_->window());
  g_object_unref(request);
}

void WindowSession::close_answered(LumaSaveRequest* request, LumaSaveChoice choice,
                                   LumaSaveDocument*, GFile* destination,
                                   gpointer data) {
  auto* self = (*static_cast<std::shared_ptr<EngineEventBridge>*>(data))->target;
  if (self != nullptr) {
    self->answer_save(request, choice, destination);
  }
}

void WindowSession::answer_save(LumaSaveRequest* request, LumaSaveChoice choice,
                                GFile* destination) {
  dialog_active_ = false;
  if (choice == LUMA_SAVE_CHOICE_DISCARD) {
    close_without_saving();
  } else if (choice == LUMA_SAVE_CHOICE_SAVE) {
    g_set_object(&close_request_, request);
    save_problem_.clear();
    after_save_ = AfterSave::CloseWindow;
    if (destination != nullptr) {
      // Named and placed on the sheet, which checked that nothing is there:
      // write it directly, with no second file chooser.
      finish_save_selection(destination);
    } else {
      save();
    }
  } else {
    returning_to_documents_ = false;
  }
}

void WindowSession::close_without_saving() {
  if (returning_to_documents_) {
    return_to_documents();
    return;
  }
  force_close_ = true;
  gtk_window_close(window_->window());
}

void WindowSession::choose_export_pdf() {
  if (!engine_->has_document() || dialog_active_) {
    return;
  }
  dialog_active_ = true;
  const auto status = engine_->status();
  std::string initial_name = "Document.pdf";
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Export PDF");
  if (status.identity.original_path.has_value()) {
    auto suggested = *status.identity.original_path;
    suggested.replace_extension(".pdf");
    initial_name = display_basename(suggested);
    GFile* folder = g_file_new_for_path(
        status.identity.original_path->parent_path().c_str());
    gtk_file_dialog_set_initial_folder(dialog, folder);
    g_object_unref(folder);
  }
  gtk_file_dialog_set_initial_name(dialog, initial_name.c_str());
  GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  GtkFileFilter* pdf = pdf_filter();
  g_list_store_append(filters, pdf);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, pdf);
  g_object_unref(pdf);
  g_object_unref(filters);
  gtk_file_dialog_save(dialog, window_->window(), nullptr, export_finished, this);
  g_object_unref(dialog);
}

void WindowSession::export_finished(GObject* source, GAsyncResult* result,
                                    gpointer data) {
  auto* self = static_cast<WindowSession*>(data);
  self->dialog_active_ = false;
  GError* error = nullptr;
  GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
  if (file == nullptr) {
    if (error != nullptr &&
        !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      self->window_->show_toast(error->message, ui::ToastKind::Error);
    }
    g_clear_error(&error);
    return;
  }
  char* path = g_file_get_path(file);
  if (path != nullptr) {
    auto destination = compatibility::prepare_pdf_destination(path);
    if (!destination.ok) {
      self->window_->show_toast(destination.message, ui::ToastKind::Error,
                                5600);
    } else {
      self->confirm_overwrite(std::move(destination),
                              PendingFileOperation::ExportPdf);
    }
  } else {
    self->window_->show_toast(
        "Only local PDF destinations are supported in this MVP.",
        ui::ToastKind::Warning);
  }
  g_free(path);
  g_object_unref(file);
}

void WindowSession::export_to_destination(
    const compatibility::PreparedDestination& destination,
    std::optional<persistence::TargetFingerprint> expected_target) {
  snapshot_.busy = true;
  snapshot_.save_state_label = "Exporting PDF…";
  window_->update_document(snapshot_);
  persistence::AtomicSaveOptions save_options;
  save_options.require_target_absent =
      !destination.requires_overwrite_confirmation &&
      !expected_target.has_value();
  save_options.expected_existing_target = std::move(expected_target);
  const auto saved = persistence::AtomicFileSaver::save_via_path(
      destination.path,
      [this](const std::filesystem::path& staged, std::string& error) {
        const auto result = engine_->export_pdf(staged);
        error = result.message;
        return result.ok;
      },
      [](const std::filesystem::path& staged, std::string& error) {
        return validate_pdf_output(staged, error);
      },
      save_options);
  snapshot_.busy = false;
  refresh_document();
  if (!saved.saved()) {
    window_->show_toast(saved.message.empty() ? "PDF export failed."
                                              : saved.message,
                        ui::ToastKind::Error, 7200);
    return;
  }
  if (saved.outcome == persistence::SaveOutcome::SavedDurabilityUncertain) {
    window_->show_toast(saved.message, ui::ToastKind::Warning, 7200);
  } else {
    window_->show_toast("PDF exported and validated", ui::ToastKind::Success);
  }
}

void WindowSession::handle_engine_event(const engine::EngineEvent& event) {
  if (document_canvas_ != nullptr) {
    document_canvas_->handle_engine_event(event);
  }
  switch (event.kind) {
    case engine::EngineEventKind::tiles_invalidated:
    case engine::EngineEventKind::cursor_changed:
    case engine::EngineEventKind::cursor_visibility_changed:
      break;
    case engine::EngineEventKind::graphic_selection_changed: {
      // The inspector shows the selected picture's size and layout.
      const bool selected = event.area_twips.has_value();
      snapshot_.picture_selected = selected;
      if (selected) {
        picture_twips_ = *event.area_twips;
        snapshot_.picture_width_twips = event.area_twips->width;
        snapshot_.picture_height_twips = event.area_twips->height;
      }
      schedule_refresh();
      break;
    }
    case engine::EngineEventKind::text_selection_changed:
    case engine::EngineEventKind::accessibility_focus_changed:
      // Re-read the engine's selection state even when no formatting command
      // ran. The bar and inspector describe the highlighted text or new caret.
      schedule_refresh();
      break;
    case engine::EngineEventKind::document_modified:
      schedule_refresh();
      schedule_outline(1400);
      break;
    case engine::EngineEventKind::document_extent_changed:
    case engine::EngineEventKind::command_state_changed:
    case engine::EngineEventKind::command_completed:
    case engine::EngineEventKind::tracked_changes_changed:
    case engine::EngineEventKind::table_selection_changed:
    case engine::EngineEventKind::missing_fonts_changed:
      schedule_refresh();
      break;
    case engine::EngineEventKind::comments_changed:
      comments_need_refresh_ = true;
      schedule_refresh();
      if (document_canvas_ != nullptr) {
        document_canvas_->refresh_comments();
      }
      break;
    case engine::EngineEventKind::search_result: {
      // One entry per match, each with its "rectangles". A find-all lists
      // them all; a find-next lists the one it selected, which places it.
      std::vector<std::string> rectangles;
      for (std::size_t at = event.text.find("\"rectangles\"");
           at != std::string::npos; at = event.text.find("\"rectangles\"", at + 1)) {
        const auto open = event.text.find('"', event.text.find(':', at) + 1);
        const auto close = open == std::string::npos ? open : event.text.find('"', open + 1);
        if (close != std::string::npos) {
          rectangles.push_back(event.text.substr(open + 1, close - open - 1));
        }
      }
      snapshot_.search_ran = true;
      if (search_counting_) {
        search_counting_ = false;
        search_rectangles_ = rectangles;
        snapshot_.search_matches = static_cast<int>(rectangles.size());
        snapshot_.search_current = 0;
        window_->update_document(snapshot_);
        step_search(search_step_backwards_);
        break;
      }
      if (rectangles.size() == 1) {
        const auto found = std::find(search_rectangles_.begin(), search_rectangles_.end(),
                                     rectangles.front());
        snapshot_.search_current =
            found == search_rectangles_.end()
                ? 0
                : static_cast<int>(found - search_rectangles_.begin()) + 1;
      }
      window_->update_document(snapshot_);
      break;
    }
    case engine::EngineEventKind::search_not_found:
      snapshot_.search_ran = true;
      if (search_counting_) {
        search_counting_ = false;
        search_rectangles_.clear();
        snapshot_.search_matches = 0;
      }
      snapshot_.search_current = 0;
      window_->update_document(snapshot_);
      break;
    case engine::EngineEventKind::context_changed:
      snapshot_.selection_context = selection_context_label(event.text);
      schedule_refresh();
      break;
    case engine::EngineEventKind::context_menu:
      break;
    case engine::EngineEventKind::dialog_opened: {
      // Write shows none of LibreOffice's own dialogs. One it asked for is
      // confirmed with its defaults; anything else is closed at once, so an
      // invisible dialog can never hold the document.
      const bool confirm = !confirm_dialog_.empty() && event.text == confirm_dialog_;
      confirm_dialog_.clear();
      static_cast<void>(engine_->answer_dialog(event.numeric_value, confirm));
      if (!confirm) {
        window_->show_toast("That command needs a LibreOffice dialog Write does not show.",
                            ui::ToastKind::Warning);
      }
      schedule_refresh();
      schedule_outline(300);
      break;
    }
    case engine::EngineEventKind::password_requested:
      window_->show_toast(
          "This encrypted document requires a password flow that is not yet "
          "available.",
          ui::ToastKind::Warning, 5200);
      break;
    case engine::EngineEventKind::hyperlink_activated:
      window_->show_toast("External links require explicit approval.",
                          ui::ToastKind::Warning);
      break;
    case engine::EngineEventKind::progress:
      snapshot_.busy = event.numeric_value < 100;
      window_->update_document(snapshot_);
      break;
    case engine::EngineEventKind::error:
      window_->show_toast(event.text.empty() ? "The document engine reported an error."
                                             : event.text,
                          ui::ToastKind::Error, 5600);
      break;
  }
}

// Engine states arrive in bursts (a caret move reports dozens), so the
// chrome catches up once per burst rather than once per state.
void WindowSession::schedule_refresh() {
  if (refresh_source_ != 0) {
    return;
  }
  refresh_source_ = g_idle_add_full(
      G_PRIORITY_DEFAULT_IDLE,
      [](gpointer data) -> gboolean {
        auto* self = static_cast<WindowSession*>(data);
        self->refresh_source_ = 0;
        self->refresh_document();
        return G_SOURCE_REMOVE;
      },
      this, nullptr);
}

void WindowSession::schedule_outline(const unsigned delay_ms) {
  if (outline_source_ != 0) {
    g_source_remove(outline_source_);
  }
  outline_source_ = g_timeout_add(delay_ms, [](gpointer data) -> gboolean {
    auto* self = static_cast<WindowSession*>(data);
    self->outline_source_ = 0;
    self->refresh_outline();
    return G_SOURCE_REMOVE;
  }, this);
}

void WindowSession::refresh_outline() {
  if (!engine_->has_document() || closing_) {
    return;
  }
  const auto started = g_get_monotonic_time();
  headings_ = engine_->outline();
  outline_ready_ = true;
  g_debug("Write outline: %zu headings in %.1f ms", headings_.size(),
          static_cast<double>(g_get_monotonic_time() - started) / 1000.0);
  snapshot_.outline.clear();
  snapshot_.outline_ready = true;
  for (const auto& heading : headings_) {
    snapshot_.outline.push_back({.title = heading.text, .level = heading.level});
  }
  window_->update_document(snapshot_);
}

// Reads what the engine last reported for the caret: style, face, size and
// the counts the status bar shows.
void WindowSession::read_engine_state() {
  const auto value = [this](const char* uno) {
    return engine_->command_value(uno).value_or("");
  };
  const auto on = [&](const char* uno) { return value(uno) == "true"; };
  if (const auto style = engine_->command_value(".uno:StyleApply"); style.has_value()) {
    snapshot_.paragraph_style = style->empty() || *style == "mixed" || *style == "indeterminate"
        ? "Mixed" : ui::style_display_name(*style);
  }
  snapshot_.typeface_label = value(".uno:CharFontName");
  snapshot_.typeface_mixed = snapshot_.typeface_label == "mixed" ||
      snapshot_.typeface_label == "indeterminate";
  if (snapshot_.typeface_mixed) snapshot_.typeface_label.clear();
  const auto height = value(".uno:FontHeight");
  snapshot_.size_mixed = height == "0" || height == "mixed" || height == "indeterminate";
  snapshot_.size_label = snapshot_.size_mixed ? std::string{} : height;
  snapshot_.text_colour = value(".uno:Color");
  snapshot_.highlight_colour = value(".uno:CharBackColor");
  const auto page_style = current_page_style();
  const auto page_part_state = [&](const char* command, bool& shown, bool& known) {
    const std::string state = value(command);
    if (state.empty()) return;
    JsonParser* parser = json_parser_new();
    if (json_parser_load_from_data(parser, state.c_str(), static_cast<gssize>(state.size()), nullptr)) {
      JsonNode* root = json_parser_get_root(parser);
      if (JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject* object = json_node_get_object(root);
        shown = json_object_has_member(object, page_style.c_str());
        known = true;
      }
    }
    g_object_unref(parser);
  };
  page_part_state(".uno:InsertPageHeader", snapshot_.header_on, snapshot_.header_state_known);
  page_part_state(".uno:InsertPageFooter", snapshot_.footer_on, snapshot_.footer_state_known);
  if (document_canvas_ != nullptr)
    document_canvas_->update_formatting_controls(snapshot_.paragraph_style,
        snapshot_.size_mixed ? "Mixed" : snapshot_.size_label);
  if (snapshot_.picture_selected) {
    snapshot_.picture_flow = on(".uno:SetAnchorToChar") ? "inline" :
        on(".uno:WrapThroughTransparent") ? "behind" : on(".uno:WrapThrough") ? "front" :
        on(".uno:WrapOff") ? "break" : on(".uno:WrapIdeal") ? "wrap" : "";
    snapshot_.picture_anchor = on(".uno:SetAnchorToPage") ? "on-page" :
        on(".uno:SetAnchorToPara") || on(".uno:SetAnchorToChar") ? "with-text" : "";
  } else {
    snapshot_.picture_flow.clear();
    snapshot_.picture_anchor.clear();
  }
  snapshot_.weight_label = on(".uno:Bold") ? "Bold" : "Regular";
  snapshot_.line_height_label = on(".uno:SpacePara1")    ? "1.0"
                                : on(".uno:SpacePara15") ? "1.5"
                                : on(".uno:SpacePara2")  ? "2.0"
                                                         : "";
  const auto word_state = value(".uno:StateWordCount");
  g_debug("Write word count state: %s", word_state.c_str());
  // The engine reports the document's totals, or with a selection only the
  // selection's ("Selected: 3 words, 20 characters"); the status bar shows
  // the selection against the last totals, as Docs does.
  if (const auto counts = integers_in(word_state); counts.size() >= 2) {
    const int words = static_cast<int>(counts[counts.size() - 2]);
    const int characters = static_cast<int>(counts.back());
    if (word_state.find("Selected") != std::string::npos) {
      snapshot_.selected_word_count = words;
      snapshot_.selected_character_count = characters;
    } else {
      snapshot_.word_count = words;
      snapshot_.character_count = characters;
      snapshot_.selected_word_count = -1;
      snapshot_.selected_character_count = -1;
      snapshot_.word_count_available = true;
      snapshot_.character_count_available = true;
    }
  }
  if (const auto pages = integers_in(value(".uno:StatePageNumber")); pages.size() >= 2) {
    snapshot_.current_page = static_cast<int>(pages[0]);
    snapshot_.page_count = static_cast<int>(pages[1]);
    snapshot_.current_page_available = true;
  }
  snapshot_.track_changes = on(".uno:TrackChanges");
  if (document_canvas_ != nullptr)
    document_canvas_->set_page_parts(snapshot_.header_on, snapshot_.footer_on, 0);
  // Paper and orientation from the first page's size (twips).
  const auto status = engine_->status();
  if (!status.page_areas_twips.empty()) {
    const auto& page = status.page_areas_twips.front();
    const auto near = [](std::int64_t a, std::int64_t b) { return std::llabs(a - b) < 60; };
    const auto shorter = std::min(page.width, page.height);
    const auto longer = std::max(page.width, page.height);
    snapshot_.orientation_label = page.width > page.height ? "Landscape" : "Portrait";
    snapshot_.page_size_label = near(shorter, 12240) && near(longer, 15840)   ? "Letter"
                                : near(shorter, 12240) && near(longer, 20160) ? "Legal"
                                : near(shorter, 11906) && near(longer, 16838) ? "A4"
                                : near(shorter, 8391) && near(longer, 11906)  ? "A5"
                                                                              : "Custom";
  }
}

void WindowSession::dispatch_uno(const char* uno, const std::string& arguments,
                                 const bool modifies) {
  if (!engine_->has_document()) {
    return;
  }
  if (modifies && engine_->status().read_only) return;
  const auto result = engine_->dispatch_command(uno, arguments, modifies);
  if (!result.ok) {
    report_engine_failure(result, "The command could not be completed");
    return;
  }
  schedule_refresh();
  if (modifies) {
    schedule_outline(900);
  }
}

void WindowSession::apply_page_theme() {
  std::string preference = "auto";
  if (GSettings* settings = owner_.settings(); settings != nullptr) {
    char* stored = g_settings_get_string(settings, "page-theme");
    preference = stored == nullptr ? "auto" : stored;
    g_free(stored);
  }
  bool moon = preference == "moon";
  if (preference == "auto" && surface_policy_ != nullptr) {
    const std::string_view treatment = luma_surface_policy_get_requested(surface_policy_);
    moon = treatment == "dark" || treatment == "glass";
  }
  moon_ = moon;
  snapshot_.moon = moon;
  g_debug("Write page theme: preference %s, treatment %s, %s", preference.c_str(),
          surface_policy_ != nullptr ? luma_surface_policy_get_requested(surface_policy_) : "none",
          moon ? "moon" : "sun");
  if (document_canvas_ != nullptr) {
    auto appearance = document_canvas_->page_appearance();
    appearance.moon = moon;
    document_canvas_->set_page_appearance(appearance);
  }
  // Moon is the engine's own dark view: a dark page and automatic text in
  // light ink. Pictures, charts and every colour the document sets are drawn
  // as they are. The engine keeps the choice for documents it opens later.
  static_cast<void>(engine_->set_view_theme(moon ? engine::ViewTheme::moon
                                                 : engine::ViewTheme::sun));
  window_->update_document(snapshot_);
}

void WindowSession::set_zoom(const std::string_view level) {
  if (document_canvas_ == nullptr) {
    return;
  }
  if (level == "fit-width") {
    document_canvas_->set_zoom_mode(canvas::ZoomMode::fit_width);
    snapshot_.zoom_mode = "fit-width";
  } else if (level == "fit-page") {
    document_canvas_->set_zoom_mode(canvas::ZoomMode::fit_page);
    snapshot_.zoom_mode = "fit-page";
  } else if (level == "auto") {
    document_canvas_->set_zoom_mode(canvas::ZoomMode::automatic);
    snapshot_.zoom_mode = "auto";
  } else {
    const double percent = std::atof(std::string{level}.c_str());
    if (percent > 0) {
      document_canvas_->set_zoom(percent / 100.0);
      snapshot_.zoom_mode = "fixed";
    }
  }
  remember_zoom();
}

void WindowSession::remember_zoom() {
  if (document_canvas_ == nullptr) {
    return;
  }
  snapshot_.zoom_percent = static_cast<int>(std::lround(document_canvas_->zoom() * 100.0));
  snapshot_.current_page = document_canvas_->current_page();
  window_->update_document(snapshot_);
}

void WindowSession::find(const std::string_view text, const bool backwards) {
  if (text.empty() || !engine_->has_document()) {
    return;
  }
  const std::string needle{text};
  // A new search counts every match first (the find bar says "3 of 11");
  // the engine answers asynchronously, and the answer steps to the first
  // match from the caret (see the search_result event).
  if (needle != search_text_ || search_counting_) {
    search_text_ = needle;
    search_counting_ = true;
    search_step_backwards_ = backwards;
    static_cast<void>(engine_->dispatch_command(
        ".uno:ExecuteSearch",
        uno_args({uno_string("SearchItem.SearchString", needle),
                  uno_bool("SearchItem.Backward", false),
                  uno_number("SearchItem.Command", "long", 1)}),
        false));
    return;
  }
  step_search(backwards);
}

void WindowSession::step_search(const bool backwards) {
  if (search_text_.empty() || search_rectangles_.empty()) {
    return;
  }
  static_cast<void>(engine_->dispatch_command(
      ".uno:ExecuteSearch",
      uno_args({uno_string("SearchItem.SearchString", search_text_),
                uno_bool("SearchItem.Backward", backwards),
                uno_number("SearchItem.Command", "long", 0)}),
      false));
}

void WindowSession::replace(const std::string_view argument, const bool all) {
  const auto separator = argument.find('\x1f');
  if (separator == std::string_view::npos || separator == 0) {
    return;
  }
  const std::string needle{argument.substr(0, separator)};
  const std::string replacement{argument.substr(separator + 1)};
  const int before = snapshot_.search_matches;
  dispatch_uno(".uno:ExecuteSearch",
               uno_args({uno_string("SearchItem.SearchString", needle),
                         uno_string("SearchItem.ReplaceString", replacement),
                         uno_number("SearchItem.Command", "long", all ? 3 : 2)}),
               true);
  search_text_.clear();
  if (all && before > 0) {
    window_->show_toast("Replaced " + std::to_string(before) +
                            (before == 1 ? " match" : " matches"),
                        ui::ToastKind::Success);
  }
  find(needle, false);
}

void WindowSession::choose_image() {
  if (dialog_active_ || !engine_->has_document()) {
    return;
  }
  dialog_active_ = true;
  GtkFileDialog* dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Insert an image");
  GtkFileFilter* images = gtk_file_filter_new();
  gtk_file_filter_set_name(images, "Images");
  gtk_file_filter_add_mime_type(images, "image/*");
  GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, images);
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, images);
  g_object_unref(images);
  g_object_unref(filters);
  gtk_file_dialog_open(
      dialog, window_->window(), nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<WindowSession*>(data);
        self->dialog_active_ = false;
        GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
        if (file == nullptr) {
          return;
        }
        char* uri = g_file_get_uri(file);
        if (uri != nullptr && g_str_has_prefix(uri, "file://")) {
          self->dispatch_uno(".uno:InsertGraphic", uno_args({uno_string("FileName", uri)}),
                             true);
        } else {
          self->window_->show_toast("Only local images can be inserted.",
                                    ui::ToastKind::Warning);
        }
        g_free(uri);
        g_object_unref(file);
      },
      this);
  g_object_unref(dialog);
}

void WindowSession::add_comment() {
  if (!engine_->has_document()) {
    return;
  }
  const std::string quote = engine_->selected_text();
  window_->ask_for_text(
      "Add a comment", quote.empty() ? "" : "On “" + quote.substr(0, 80) + "”",
      {{"Comment", ""}}, "Comment", [this](std::vector<std::string> values) {
        if (values.empty() || values.front().empty()) {
          return;
        }
        dispatch_uno(".uno:InsertAnnotation", uno_args({uno_string("Text", values.front())}),
                     true);
        if (document_canvas_ != nullptr) {
          document_canvas_->refresh_comments();
        }
      });
}

void WindowSession::add_link() {
  if (!engine_->has_document()) {
    return;
  }
  const std::string selected = engine_->selected_text();
  window_->ask_for_text(
      "Add a link", "", {{"Text", selected}, {"Address", "https://"}}, "Add Link",
      [this](std::vector<std::string> values) {
        if (values.size() < 2 || values[1].empty() || values[1] == "https://") {
          return;
        }
        std::string text = values[0].empty() ? values[1] : values[0];
        dispatch_uno(".uno:SetHyperlink",
                     uno_args({uno_string("Hyperlink.Text", text),
                               uno_string("Hyperlink.URL", values[1])}),
                     true);
      });
}

// A checklist line starts with a ballot box. The command adds one to the
// paragraph or takes it away; the box itself is ordinary text, so it
// survives every format Write saves.
void WindowSession::toggle_checklist() {
  if (!engine_->has_document()) {
    return;
  }
  const auto a11y = engine_->accessibility_snapshot();
  const std::string& text = a11y.focused_paragraph_utf8;
  const bool checked = text.rfind("☑ ", 0) == 0;
  const bool open = text.rfind("☐ ", 0) == 0;
  static_cast<void>(engine_->dispatch_command(".uno:GoToStartOfPara", "", false));
  if (checked || open) {
    for (int step = 0; step < 2; ++step) {
      static_cast<void>(engine_->dispatch_command(".uno:CharRightSel", "", false));
    }
    dispatch_uno(".uno:Delete", "", true);
  } else {
    dispatch_uno(".uno:InsertText", uno_args({uno_string("Text", "☐ ")}), true);
  }
  static_cast<void>(engine_->dispatch_command(".uno:GoToEndOfPara", "", false));
}

std::string WindowSession::current_page_style() const {
  for (const char* command : {".uno:PageStyleName", ".uno:StatusPageStyle"}) {
    const auto name = engine_->command_value(command);
    if (name && !name->empty() && *name != "disabled") return *name;
  }
  return "Default Page Style";
}

void WindowSession::set_page_part(bool header, bool shown) {
  dispatch_uno(header ? ".uno:InsertPageHeader" : ".uno:InsertPageFooter",
      uno_args({uno_string("PageStyle", current_page_style()), uno_bool("On", shown)}), true);
  if (header) {
    snapshot_.header_on = shown;
    snapshot_.header_state_known = true;
  } else {
    snapshot_.footer_on = shown;
    snapshot_.footer_state_known = true;
  }
  window_->update_document(snapshot_);
}

void WindowSession::insert_page_numbers() {
  // A centred page number in the footer: turn the footer on, write the
  // field there, and return to the text.
  set_page_part(false, true);
  static_cast<void>(engine_->dispatch_command(".uno:JumpToFooter", "", false));
  dispatch_uno(".uno:CenterPara", "", true);
  dispatch_uno(".uno:InsertPageNumberField", "", true);
  snapshot_.footer_on = true;
  static_cast<void>(engine_->dispatch_command(".uno:JumpToStartOfDoc", "", false));
  static_cast<void>(engine_->dispatch_command(".uno:GoToStartOfDoc", "", false));
  window_->show_toast("Page numbers added to the footer", ui::ToastKind::Success);
}

void WindowSession::print_document() {
  if (!engine_->has_document() || dialog_active_) {
    return;
  }
  // Printing goes through a PDF of the document, always in its own colours:
  // Moon is a way of looking at the page, never a way of printing it.
  GError* error = nullptr;
  char* directory = g_dir_make_tmp("luma-write-print-XXXXXX", &error);
  if (directory == nullptr) {
    window_->show_toast(error != nullptr ? error->message : "Could not prepare printing.",
                        ui::ToastKind::Error);
    g_clear_error(&error);
    return;
  }
  const std::filesystem::path pdf = std::filesystem::path{directory} / "print.pdf";
  g_free(directory);
  // The view's colour scheme never reaches a printed page, but print from
  // Sun regardless so nothing depends on it.
  if (moon_) {
    static_cast<void>(engine_->set_view_theme(engine::ViewTheme::sun));
  }
  const auto exported = engine_->export_pdf(pdf);
  if (moon_) {
    static_cast<void>(engine_->set_view_theme(engine::ViewTheme::moon));
  }
  if (!exported.ok) {
    report_engine_failure(exported, "Could not prepare the document for printing");
    return;
  }
  GtkPrintUnixDialog* dialog =
      GTK_PRINT_UNIX_DIALOG(gtk_print_unix_dialog_new("Print", window_->window()));
  gtk_print_unix_dialog_set_manual_capabilities(
      dialog, static_cast<GtkPrintCapabilities>(GTK_PRINT_CAPABILITY_GENERATE_PDF |
                                                GTK_PRINT_CAPABILITY_COPIES));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  auto* path = new std::filesystem::path(pdf);
  g_object_set_data_full(G_OBJECT(dialog), "write-print-pdf", path, [](gpointer data) {
    auto* file = static_cast<std::filesystem::path*>(data);
    std::error_code ignored;
    std::filesystem::remove_all(file->parent_path(), ignored);
    delete file;
  });
  dialog_active_ = true;
  g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkDialog* print_dialog, int response,
                                                       gpointer data) {
                     auto* self = static_cast<WindowSession*>(data);
                     self->dialog_active_ = false;
                     auto* unix_dialog = GTK_PRINT_UNIX_DIALOG(print_dialog);
                     if (response == GTK_RESPONSE_OK) {
                       GtkPrinter* printer = gtk_print_unix_dialog_get_selected_printer(unix_dialog);
                       GtkPrintSettings* settings = gtk_print_unix_dialog_get_settings(unix_dialog);
                       GtkPageSetup* setup = gtk_print_unix_dialog_get_page_setup(unix_dialog);
                       GtkPrintJob* job = gtk_print_job_new("Write document", printer, settings, setup);
                       const auto* file = static_cast<std::filesystem::path*>(
                           g_object_get_data(G_OBJECT(print_dialog), "write-print-pdf"));
                       GError* job_error = nullptr;
                       if (gtk_print_job_set_source_file(job, file->c_str(), &job_error)) {
                         gtk_print_job_send(job, nullptr, nullptr, nullptr);
                         self->window_->show_toast("Sent to the printer", ui::ToastKind::Success);
                       } else {
                         self->window_->show_toast(job_error != nullptr ? job_error->message
                                                                        : "Printing failed.",
                                                   ui::ToastKind::Error);
                         g_clear_error(&job_error);
                       }
                       g_object_unref(job);
                       g_object_unref(settings);
                     }
                     gtk_window_destroy(GTK_WINDOW(print_dialog));
                   }),
                   this);
  gtk_window_present(GTK_WINDOW(dialog));
}

void WindowSession::toggle_focus_mode() {
  focus_mode_ = !focus_mode_;
  window_->set_focus_mode(focus_mode_);
}

void WindowSession::toggle_advanced() {
  advanced_visible_ = !advanced_visible_;
  if (GSettings* settings = owner_.settings(); settings != nullptr) {
    g_settings_set_boolean(settings, "show-advanced", advanced_visible_);
  }
  window_->set_advanced_visible(advanced_visible_);
}

void WindowSession::run_advanced(const std::string_view uno_command) {
  // Only commands from Write's own catalog run from here.
  if (!advanced_command_known(uno_command)) {
    return;
  }
  const bool view_only = uno_command == ".uno:ControlCodes" || uno_command == ".uno:Marks" ||
                         uno_command == ".uno:ShowTrackedChanges" ||
                         uno_command == ".uno:SelectTable" ||
                         uno_command.find("TrackedChange") != std::string_view::npos;
  dispatch_uno(std::string{uno_command}.c_str(), "", !view_only);
}

void WindowSession::execute_argument(const std::string_view command_id,
                                     const std::string_view argument) {
  namespace w = commands::ids::write;
  const std::string value{argument};
  if (command_id == w::kApplyStyle) {
    dispatch_uno(".uno:StyleApply",
                 uno_args({uno_string("Style", ui::style_engine_name(value)),
                           uno_string("FamilyName", "ParagraphStyles")}),
                 true);
  } else if (command_id == w::kPictureSize) {
    // "width,height" in twips: the picture's far corner is dragged there.
    const auto numbers = integers_in(value);
    if (numbers.size() >= 2 && snapshot_.picture_selected && numbers[0] > 0 && numbers[1] > 0) {
      const auto& picture = picture_twips_;
      static_cast<void>(engine_->drag_graphic_handle(true, picture.x + picture.width,
                                                     picture.y + picture.height));
      static_cast<void>(engine_->drag_graphic_handle(false, picture.x + numbers[0],
                                                     picture.y + numbers[1]));
    }
  } else if (command_id == w::kPictureLayout) {
    const auto run = [this](std::initializer_list<const char*> commands) {
      for (const char* uno : commands) dispatch_uno(uno, "", true);
    };
    if (value == "inline") run({".uno:SetAnchorToChar"});
    else if (value == "wrap") run({".uno:SetAnchorToPara", ".uno:WrapIdeal"});
    else if (value == "break") run({".uno:SetAnchorToPara", ".uno:WrapOff"});
    else if (value == "behind") run({".uno:SetAnchorToPara", ".uno:WrapThroughTransparent"});
    else if (value == "front") run({".uno:SetAnchorToPara", ".uno:WrapThrough"});
    else if (value == "with-text") run({".uno:SetAnchorToPara"});
    else if (value == "on-page") run({".uno:SetAnchorToPage"});
    else if (value == "reset") run({".uno:OriginalSize"});
  } else if (command_id == w::kPictureAction) {
    if (value == "caption") dispatch_uno(".uno:InsertCaptionDialog", "", false);
    else if (value == "delete") dispatch_uno(".uno:Delete", "", true);
    else if (value == "done") dispatch_uno(".uno:Escape", "", false);
    else if (value == "properties") dispatch_uno(".uno:GraphicDialog", "", false);
  } else if (command_id == w::kBoldState) {
    // Bold on or off, not toggled: toggle only when the caret disagrees.
    const bool want = value == "true";
    if ((engine_->command_value(".uno:Bold").value_or("false") == "true") != want) {
      dispatch_uno(".uno:Bold", "", true);
    }
  } else if (command_id == w::kNewStyle) {
    if (value.empty()) {
      window_->ask_for_text("New paragraph style", "Use the selected text’s formatting as the starting point.",
          {{"Style name", ""}}, "Create style", [this](std::vector<std::string> values) {
        if (values.empty()) return;
        const auto first = values.front().find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        const auto last = values.front().find_last_not_of(" \t\r\n");
        execute_argument(commands::ids::write::kNewStyle, values.front().substr(first, last - first + 1));
      });
    } else {
      const auto existing = std::find(snapshot_.paragraph_styles.begin(), snapshot_.paragraph_styles.end(),
                                      ui::style_engine_name(value));
      if (existing != snapshot_.paragraph_styles.end()) {
        window_->show_toast("That style already exists. Use Update style to change its formatting.",
                            ui::ToastKind::Warning);
        return;
      }
      dispatch_uno(".uno:StyleNewByExample",
                   uno_args({uno_string("Param", value), uno_number("Family", "short", 2)}),
                   true);
      snapshot_.paragraph_styles.push_back(value);
      window_->show_toast("Created the style “" + value + "” from the selection",
                          ui::ToastKind::Success);
    }
  } else if (command_id == w::kHeaderState || command_id == w::kFooterState) {
    if (value == "on" || value == "off") set_page_part(command_id == w::kHeaderState, value == "on");
  } else if (command_id == w::kFont) {
    dispatch_uno(".uno:CharFontName", uno_args({uno_string("CharFontName.FamilyName", value)}),
                 true);
  } else if (command_id == w::kFontSize) {
    const double points = std::atof(value.c_str());
    if (points > 0) {
      char number[32];
      std::snprintf(number, sizeof(number), "%g", points);
      dispatch_uno(".uno:FontHeight",
                   std::string{"{\"FontHeight.Height\":{\"type\":\"float\",\"value\":"} + number +
                       "}}",
                   true);
    }
  } else if (command_id == w::kTextColor) {
    dispatch_uno(".uno:Color", uno_args({uno_number("Color", "long", colour_value(value))}), true);
  } else if (command_id == w::kHighlight) {
    dispatch_uno(".uno:CharBackColor",
                 uno_args({uno_number("CharBackColor", "long", colour_value(value))}), true);
  } else if (command_id == w::kLineSpacing) {
    if (value == "1") dispatch_uno(".uno:SpacePara1", "", true);
    else if (value == "1.5") dispatch_uno(".uno:SpacePara15", "", true);
    else if (value == "2") dispatch_uno(".uno:SpacePara2", "", true);
    else {
      char* end = nullptr;
      const double multiple = std::strtod(value.c_str(), &end);
      if (end != value.c_str() && *end == '\0' && std::isfinite(multiple) &&
          multiple >= 0.5 && multiple <= 5.0) {
        dispatch_uno(".uno:LineSpacing",
                     uno_args({uno_number("LineSpacing.Mode", "short", 0),
                               uno_number("LineSpacing.Height", "short",
                                          static_cast<int>(std::lround(multiple * 100.0)))}),
                     true);
      }
    }
  } else if (command_id == w::kParagraphSpacing) {
    if (value.empty()) {
      window_->ask_for_text("Set paragraph spacing", "Enter both amounts in points. This sets the selected paragraphs’ spacing.",
          {{"Before (pt)", ""}, {"After (pt)", ""}}, "Apply spacing", [this](std::vector<std::string> values) {
        if (values.size() != 2) return;
        execute_argument(commands::ids::write::kParagraphSpacing, values[0] + "," + values[1]);
      });
      return;
    }
    const auto separator = value.find(',');
    if (separator == std::string::npos) return;
    const auto parse = [](const std::string& text) -> std::optional<double> {
      char* end = nullptr;
      const double points = std::strtod(text.c_str(), &end);
      if (end == text.c_str()) return std::nullopt;
      while (end != nullptr && std::isspace(static_cast<unsigned char>(*end))) ++end;
      if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(points) ||
          points < 0 || points > 999) return std::nullopt;
      return points;
    };
    const auto before = parse(value.substr(0, separator));
    const auto after = parse(value.substr(separator + 1));
    if (!before || !after) {
      window_->show_toast("Enter a number from 0 to 999 for both spacing amounts.", ui::ToastKind::Warning);
      return;
    }
    dispatch_uno(".uno:ULSpacing",
        uno_args({uno_number("ULSpacing.TopMargin", "long", static_cast<long long>(std::lround(*before * 2540 / 72))),
                  uno_number("ULSpacing.BottomMargin", "long", static_cast<long long>(std::lround(*after * 2540 / 72)))}), true);
  } else if (command_id == w::kInsertCharacter) {
    dispatch_uno(".uno:InsertText", uno_args({uno_string("Text", value)}), true);
  } else if (command_id == w::kInsertTableSized) {
    const auto size = integers_in(value);
    if (size.size() == 2) {
      dispatch_uno(".uno:InsertTable",
                   uno_args({uno_number("Columns", "long", size[0]),
                             uno_number("Rows", "long", size[1])}),
                   true);
    }
  } else if (command_id == w::kPageSize) {
    // Sizes in 1/100 mm, the unit the page-size command takes.
    long long width = 21590;
    long long height = 27940;
    if (value == "Legal") { width = 21590; height = 35560; }
    else if (value == "A4") { width = 21000; height = 29700; }
    else if (value == "A5") { width = 14800; height = 21000; }
    if (snapshot_.orientation_label == "Landscape") std::swap(width, height);
    dispatch_uno(".uno:AttributePageSize",
                 uno_args({uno_number("AttributePageSize.Width", "long", width),
                           uno_number("AttributePageSize.Height", "long", height)}),
                 true);
  } else if (command_id == w::kPageOrientation) {
    const bool landscape = value == "landscape";
    if (landscape != (snapshot_.orientation_label == "Landscape")) {
      dispatch_uno(".uno:ToggleOrientation", "", true);
    }
  } else if (command_id == w::kPageMargins) {
    if (value == "Custom" || value.starts_with("custom:")) {
      const char* locale = std::setlocale(LC_MEASUREMENT, nullptr);
      const bool inches = locale != nullptr && std::string_view{locale}.find("_US") != std::string_view::npos;
      const std::string unit = inches ? "in" : "cm";
      if (value == "Custom") {
        window_->ask_for_text("Custom margins", "Set all four margins for the current page layout.",
            {{"Top (" + unit + ")", ""}, {"Bottom (" + unit + ")", ""},
             {"Left (" + unit + ")", ""}, {"Right (" + unit + ")", ""}},
            "Apply margins", [this](std::vector<std::string> values) {
          if (values.size() != 4) return;
          execute_argument(commands::ids::write::kPageMargins,
              "custom:" + values[0] + ";" + values[1] + ";" + values[2] + ";" + values[3]);
        });
        return;
      }
      std::array<long long, 4> amounts{};
      std::size_t position = 7;
      for (std::size_t index = 0; index < amounts.size(); ++index) {
        const auto end_position = value.find(';', position);
        std::string text = value.substr(position, end_position == std::string::npos ? end_position : end_position - position);
        std::replace(text.begin(), text.end(), ',', '.');
        char* end = nullptr;
        const double amount = std::strtod(text.c_str(), &end);
        const bool parsed = end != text.c_str();
        while (end != nullptr && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (!parsed || *end != '\0' || !std::isfinite(amount) || amount < 0 || amount > 100) {
          window_->show_toast("Enter a nonnegative number for every margin.", ui::ToastKind::Warning);
          return;
        }
        amounts[index] = static_cast<long long>(std::lround(amount * (inches ? 2540 : 1000)));
        if (index != 3 && end_position == std::string::npos) return;
        position = end_position + 1;
      }
      const auto status = engine_->status();
      if (!status.page_areas_twips.empty()) {
        const auto& page = status.page_areas_twips[std::min(status.page_areas_twips.size() - 1,
            static_cast<std::size_t>(std::max(0, snapshot_.current_page - 1)))];
        if (amounts[0] + amounts[1] >= page.height * 2540 / 1440 ||
            amounts[2] + amounts[3] >= page.width * 2540 / 1440) {
          window_->show_toast("These margins leave no room for the document’s text.", ui::ToastKind::Warning);
          return;
        }
      }
      dispatch_uno(".uno:PageLRMargin", uno_args({uno_number("Page.Left", "long", amounts[2]),
          uno_number("Page.Right", "long", amounts[3])}), true);
      dispatch_uno(".uno:PageULMargin", uno_args({uno_number("Page.Upper", "long", amounts[0]),
          uno_number("Page.Lower", "long", amounts[1])}), true);
      snapshot_.margins_label = "Custom";
      schedule_refresh();
      return;
    }
    snapshot_.margins_label = value;
    long long sides = 2540;
    long long ends = 2540;
    if (value == "Narrow") { sides = 1270; ends = 1270; }
    else if (value == "Moderate") { sides = 1905; }
    else if (value == "Wide") { sides = 5080; }
    if (document_canvas_ != nullptr)
      document_canvas_->set_page_parts(snapshot_.header_on, snapshot_.footer_on,
                                       value == "Narrow" ? 720 : 1440);
    dispatch_uno(".uno:PageLRMargin",
                 uno_args({uno_number("Page.Left", "long", sides),
                           uno_number("Page.Right", "long", sides)}),
                 true);
    dispatch_uno(".uno:PageULMargin",
                 uno_args({uno_number("Page.Upper", "long", ends),
                           uno_number("Page.Lower", "long", ends)}),
                 true);
  } else if (command_id == w::kFindNext) {
    find(argument, false);
  } else if (command_id == w::kFindPrevious) {
    find(argument, true);
  } else if (command_id == w::kReplaceOne) {
    replace(argument, false);
  } else if (command_id == w::kReplaceAll) {
    replace(argument, true);
  } else if (command_id == w::kOutlineJump) {
    const auto index = static_cast<std::size_t>(std::atoi(value.c_str()));
    if (index < headings_.size()) {
      static_cast<void>(engine_->dispatch_command(
          ".uno:JumpToMark",
          uno_args({uno_string("Bookmark", headings_[index].text + "|outline")}), false));
      for (std::size_t item = 0; item < snapshot_.outline.size(); ++item) {
        snapshot_.outline[item].active = item == index;
      }
      window_->update_document(snapshot_);
      if (canvas_ != nullptr) {
        gtk_widget_grab_focus(document_canvas_ != nullptr ? document_canvas_->focus_widget()
                                                          : canvas_);
      }
    }
  } else if (command_id == w::kZoomTo) {
    set_zoom(argument);
  } else if (command_id == w::kPageTheme) {
    if (GSettings* settings = owner_.settings(); settings != nullptr) {
      g_settings_set_string(settings, "page-theme",
                            value == "moon" || value == "sun" ? value.c_str() : "auto");
    } else {
      moon_ = value == "moon";
    }
    apply_page_theme();
  } else if (command_id == w::kRunAdvanced) {
    run_advanced(argument);
  } else if (command_id == w::kResolveComment) {
    if (!argument.empty()) {
      dispatch_uno(".uno:ResolveComment", uno_args({uno_string("Id", value)}), true);
      comments_need_refresh_ = true;
    }
  } else if (command_id == w::kReplyComment) {
    if (!argument.empty()) {
      window_->ask_for_text(
          "Reply to comment", "", {{"Reply", ""}}, "Reply",
          [this, id = value](std::vector<std::string> values) {
            if (values.empty() || values.front().empty()) return;
            dispatch_uno(".uno:ReplyComment",
                         uno_args({uno_string("Id", id), uno_string("Text", values.front())}),
                         true);
            comments_need_refresh_ = true;
          });
    }
  } else {
    execute(command_id);
  }
}

void WindowSession::set_recent_documents(
    const std::vector<std::string>& paths) {
  std::vector<ui::RecentDocument> recents;
  recents.reserve(paths.size());
  for (const std::string& value : paths) {
    const std::filesystem::path path{value};
    recents.push_back({
        .display_name = display_basename(path),
        .location_label = display_path(path),
        .path = value,
        .format_label = uppercase_extension(path),
        .last_opened_label = "Recent",
    });
  }
  window_->set_recent_documents(std::move(recents));
}

}  // namespace

int run_application(const int argc, char** argv, ApplicationOptions options) {
  ApplicationState state(std::move(options));
  return state.run(argc, argv);
}

}  // namespace luma::write::app
