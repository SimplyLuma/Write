#pragma once

#include "document_engine.hpp"

#include <memory>

namespace luma::write::engine {

// All LibreOfficeKit declarations are hidden behind Impl. This header remains
// usable by the rest of the app without including unstable engine headers.
class LibreOfficeEngine final : public DocumentEngine {
 public:
  LibreOfficeEngine();
  ~LibreOfficeEngine() override;
  LibreOfficeEngine(const LibreOfficeEngine&) = delete;
  LibreOfficeEngine& operator=(const LibreOfficeEngine&) = delete;

  Result initialize(const EngineConfiguration& configuration) override;
  [[nodiscard]] std::string engine_version() const override;
  void set_event_sink(EventSink sink) override;
  Result create_blank() override;
  Result open(const std::filesystem::path& path) override;
  void close() noexcept override;
  [[nodiscard]] bool has_document() const noexcept override;
  [[nodiscard]] DocumentStatus status() const override;
  Result render_tile(const TileRequest& request, RenderedTile& output) override;
  Result set_visible_area(const Rect& area_twips) override;
  Result send_key(const KeyEvent& event) override;
  Result send_composition(const CompositionEvent& event) override;
  Result send_pointer(const PointerEvent& event) override;
  Result execute(const CommandInvocation& invocation) override;
  [[nodiscard]] CommandState command_state(EngineCommand command) const override;
  Result paste(std::string_view mime_type, std::span<const std::byte> bytes) override;
  Result insert_image(const std::filesystem::path& file) override;
  Result drag_graphic_handle(bool start, std::int64_t x_twips, std::int64_t y_twips) override;
  [[nodiscard]] std::string selected_text() const override;
  Result save_as(const SaveRequest& request) override;
  Result validate_saved_document(const std::filesystem::path& path) override;
  Result adopt_saved_document(const std::filesystem::path& path,
                              DocumentFormat format) override;
  Result export_pdf(const std::filesystem::path& destination) override;
  [[nodiscard]] AccessibilitySnapshot accessibility_snapshot() const override;
  Result dispatch_command(std::string_view uno_command, std::string_view json_arguments,
                          bool modifies_document) override;
  [[nodiscard]] std::optional<std::string> command_value(
      std::string_view uno_command) const override;
  [[nodiscard]] std::string command_values(std::string_view uno_command) override;
  Result answer_dialog(int window_id, bool confirm) override;
  Result request_context_menu(std::int64_t x_twips, std::int64_t y_twips) override;
  Result set_view_theme(ViewTheme theme) override;
  [[nodiscard]] std::vector<OutlineHeading> outline() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace luma::write::engine
