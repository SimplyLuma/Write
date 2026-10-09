#pragma once

#include "document_engine.hpp"

namespace luma::write::engine {

class UnavailableEngine final : public DocumentEngine {
 public:
  Result initialize(const EngineConfiguration&) override;
  [[nodiscard]] std::string engine_version() const override;
  void set_event_sink(EventSink sink) override;
  Result create_blank() override;
  Result open(const std::filesystem::path&) override;
  void close() noexcept override;
  [[nodiscard]] bool has_document() const noexcept override;
  [[nodiscard]] DocumentStatus status() const override;
  Result render_tile(const TileRequest&, RenderedTile&) override;
  Result set_visible_area(const Rect&) override;
  Result send_key(const KeyEvent&) override;
  Result send_composition(const CompositionEvent&) override;
  Result send_pointer(const PointerEvent&) override;
  Result execute(const CommandInvocation&) override;
  [[nodiscard]] CommandState command_state(EngineCommand) const override;
  Result paste(std::string_view, std::span<const std::byte>) override;
  Result insert_image(const std::filesystem::path&) override;
  Result drag_graphic_handle(bool, std::int64_t, std::int64_t) override;
  [[nodiscard]] std::string selected_text() const override;
  Result save_as(const SaveRequest&) override;
  Result validate_saved_document(const std::filesystem::path&) override;
  Result adopt_saved_document(const std::filesystem::path&,
                              DocumentFormat) override;
  Result export_pdf(const std::filesystem::path&) override;
  [[nodiscard]] AccessibilitySnapshot accessibility_snapshot() const override;
  Result dispatch_command(std::string_view, std::string_view, bool) override;
  [[nodiscard]] std::optional<std::string> command_value(std::string_view) const override;
  [[nodiscard]] std::string command_values(std::string_view) override;
  Result answer_dialog(int, bool) override;
  Result request_context_menu(std::int64_t, std::int64_t) override;
  Result set_view_theme(ViewTheme) override;
  [[nodiscard]] std::vector<OutlineHeading> outline() override;

 private:
  [[nodiscard]] static Result unavailable();
  EventSink sink_;
};

}  // namespace luma::write::engine
