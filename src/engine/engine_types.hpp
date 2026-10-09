#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luma::write::engine {

using Revision = std::uint64_t;
inline constexpr std::size_t kMaximumPlainTextClipboardBytes =
    64U * 1024U * 1024U;

struct Rect {
  std::int64_t x{0};
  std::int64_t y{0};
  std::int64_t width{0};
  std::int64_t height{0};

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }

  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

struct Size {
  std::int64_t width{0};
  std::int64_t height{0};

  friend constexpr bool operator==(const Size&, const Size&) = default;
};

enum class PixelFormat { rgba8, bgra8 };

struct TileRequest {
  Rect document_area_twips;
  int pixel_width{0};
  int pixel_height{0};
  int part{0};
  double zoom{1.0};
  int device_scale{1};
  Revision revision{0};
};

struct RenderedTile {
  TileRequest request;
  PixelFormat format{PixelFormat::bgra8};
  std::size_t stride{0};
  std::vector<std::byte> pixels;
};

enum class DocumentFormat { docx, odt, legacy_doc, text, pdf, unknown };

struct DocumentIdentity {
  std::optional<std::filesystem::path> original_path;
  std::string extension;
  std::string mime_type;
  std::string engine_filter;
  DocumentFormat save_format{DocumentFormat::docx};
};

struct DocumentStatus {
  DocumentIdentity identity;
  Size extent_twips;
  std::vector<Rect> page_areas_twips;
  Revision revision{0};
  bool modified{false};
  bool read_only{false};
  bool source_writable{true};
  int page_count{1};
  std::chrono::system_clock::time_point last_explicit_save{};
  std::chrono::system_clock::time_point last_recovery_save{};
  std::vector<std::string> missing_fonts;
};

enum class KeyPhase { pressed, released };

struct KeyEvent {
  KeyPhase phase{KeyPhase::pressed};
  std::uint32_t unicode_scalar{0};
  std::uint32_t platform_independent_key{0};
};

enum class CompositionPhase { update, commit, cancel };

struct CompositionEvent {
  CompositionPhase phase{CompositionPhase::commit};
  std::string utf8;
};

enum class PointerPhase { press, move, release };

struct PointerEvent {
  PointerPhase phase{PointerPhase::move};
  std::int64_t x_twips{0};
  std::int64_t y_twips{0};
  int click_count{1};
  std::uint32_t buttons{0};
  std::uint32_t modifiers{0};
};

enum class CommandStateKind { unavailable, off, on, mixed, value };

struct CommandState {
  CommandStateKind kind{CommandStateKind::unavailable};
  std::string value;
};

enum class EngineCommand {
  select_all,
  cut,
  undo,
  redo,
  bold,
  italic,
  underline,
  strikethrough,
  superscript,
  subscript,
  align_left,
  align_center,
  align_right,
  align_justify,
  toggle_bullets,
  toggle_numbering,
  clear_character_overrides,
  clear_paragraph_overrides,
  apply_paragraph_style,
  insert_table,
  insert_image,
  insert_page_break,
  find,
  replace,
  track_changes,
  previous_change,
  next_change,
  accept_change,
  reject_change,
  add_comment,
  update_table_of_contents,
};

// How the page is presented on screen. View state only: the engine renders
// with a different colour scheme, and never writes it into the document.
enum class ViewTheme { sun, moon };

struct OutlineHeading {
  std::string text;
  int level{1};
};

struct CommandInvocation {
  EngineCommand command{EngineCommand::undo};
  // Adapter-owned JSON. Callers construct this only through typed command
  // helpers; it is intentionally opaque to GTK widgets.
  std::string arguments;
  bool request_completion{false};
};

/**
 * Builds the invocation for a find.
 *
 * `.uno:ExecuteSearch` takes its parameters as a JSON object, and the comment
 * above says callers have no business assembling that by hand. Searching
 * backwards is a flag rather than a separate command, because that is how the
 * engine models it.
 */
CommandInvocation find_invocation(std::string_view needle, bool backwards);

enum class EngineEventKind {
  tiles_invalidated,
  cursor_changed,
  cursor_visibility_changed,
  text_selection_changed,
  graphic_selection_changed,
  document_extent_changed,
  document_modified,
  command_state_changed,
  command_completed,
  context_changed,
  comments_changed,
  tracked_changes_changed,
  table_selection_changed,
  missing_fonts_changed,
  accessibility_focus_changed,
  // How many matches the last search found, and where the current one is.
  // LibreOfficeKit reports this and nothing was listening, so a find bar had
  // no honest way to say "3 of 11".
  search_result,
  search_not_found,
  // LibreOffice opened one of its own dialogs (numeric_value is its window,
  // text its identifier). Write shows none of them, so the session either
  // confirms it with its defaults or dismisses it at once.
  dialog_opened,
  // The engine's context menu for a right click, as JSON.
  context_menu,
  password_requested,
  hyperlink_activated,
  progress,
  error,
};

struct EngineEvent {
  EngineEventKind kind{EngineEventKind::error};
  std::optional<Rect> area_twips;
  std::vector<Rect> areas_twips;
  std::string command_id;
  CommandState command_state;
  std::string text;
  int numeric_value{0};
  Revision revision{0};
};

struct AccessibilitySnapshot {
  std::string focused_paragraph_utf8;
  int caret_offset{-1};
  bool editable{true};
  int current_page{1};
  int page_count{1};
};

struct Result {
  bool ok{false};
  std::string message;

  [[nodiscard]] static Result success() { return {true, {}}; }
  [[nodiscard]] static Result failure(std::string detail) {
    return {false, std::move(detail)};
  }
};

[[nodiscard]] std::string_view format_extension(DocumentFormat format) noexcept;
[[nodiscard]] std::string_view format_mime_type(DocumentFormat format) noexcept;

}  // namespace luma::write::engine
