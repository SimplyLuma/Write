#pragma once

#include "engine_types.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luma::write::engine {

struct EngineConfiguration {
  std::optional<std::filesystem::path> office_program_directory;
  std::optional<std::filesystem::path> private_profile_directory;
  bool disable_active_content{true};
  bool enable_accessibility{true};
};

struct SaveRequest {
  std::filesystem::path destination;
  DocumentFormat format{DocumentFormat::docx};
  bool take_ownership{false};
};

class DocumentEngine {
 public:
  using EventSink = std::function<void(const EngineEvent&)>;

  virtual ~DocumentEngine() = default;

  virtual Result initialize(const EngineConfiguration& configuration) = 0;
  [[nodiscard]] virtual std::string engine_version() const = 0;
  virtual void set_event_sink(EventSink sink) = 0;

  virtual Result create_blank() = 0;
  virtual Result open(const std::filesystem::path& path) = 0;
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual bool has_document() const noexcept = 0;
  [[nodiscard]] virtual DocumentStatus status() const = 0;

  virtual Result render_tile(const TileRequest& request, RenderedTile& output) = 0;
  virtual Result set_visible_area(const Rect& area_twips) = 0;
  virtual Result send_key(const KeyEvent& event) = 0;
  virtual Result send_composition(const CompositionEvent& event) = 0;
  virtual Result send_pointer(const PointerEvent& event) = 0;
  virtual Result execute(const CommandInvocation& invocation) = 0;
  [[nodiscard]] virtual CommandState command_state(EngineCommand command) const = 0;

  virtual Result paste(std::string_view mime_type, std::span<const std::byte> bytes) = 0;
  // Embeds the image file at the caret, in line with the text, no larger than
  // the text width. The file may be removed once this returns.
  virtual Result insert_image(const std::filesystem::path& file) = 0;
  // Drags a handle of the selected picture or shape: `start` at the handle's
  // point, then the end at the point it is dropped on (twips).
  virtual Result drag_graphic_handle(bool start, std::int64_t x_twips, std::int64_t y_twips) = 0;
  [[nodiscard]] virtual std::string selected_text() const = 0;

  virtual Result save_as(const SaveRequest& request) = 0;
  virtual Result validate_saved_document(const std::filesystem::path& path) = 0;
  // Called only after a validated staged output has atomically replaced its
  // final path. This updates logical identity/clean state without serializing
  // again, preserving the live document's undo/view state.
  virtual Result adopt_saved_document(const std::filesystem::path& path,
                                      DocumentFormat format) = 0;
  virtual Result export_pdf(const std::filesystem::path& destination) = 0;

  [[nodiscard]] virtual AccessibilitySnapshot accessibility_snapshot() const = 0;

  // A LibreOffice command by name, with adapter JSON arguments. Callers name
  // commands from Write's own catalog only; nothing typed by a user or read
  // from a document reaches this.
  virtual Result dispatch_command(std::string_view uno_command,
                                  std::string_view json_arguments,
                                  bool modifies_document) = 0;
  // The last state the engine reported for a command, verbatim ("Heading 1",
  // "12", "true", "disabled"), or nothing if it has not reported one.
  [[nodiscard]] virtual std::optional<std::string> command_value(
      std::string_view uno_command) const = 0;
  // Engine-provided choices for a command (fonts, styles), as JSON.
  [[nodiscard]] virtual std::string command_values(std::string_view uno_command) = 0;
  virtual Result set_view_theme(ViewTheme theme) = 0;
  // Answers an engine dialog: `confirm` presses its default button, anything
  // else closes it.
  virtual Result answer_dialog(int window_id, bool confirm) = 0;
  // A right click at a document position; the engine answers with a
  // context_menu event (spelling suggestions, table and link actions).
  virtual Result request_context_menu(std::int64_t x_twips, std::int64_t y_twips) = 0;
  // The document's headings in order. Reads a private copy of the document,
  // so it costs a serialisation; callers debounce it.
  [[nodiscard]] virtual std::vector<OutlineHeading> outline() = 0;
};

}  // namespace luma::write::engine
