#include "unavailable_engine.hpp"

#include <utility>

namespace luma::write::engine {

Result UnavailableEngine::unavailable() {
  return Result::failure(
      "This build of Write has no document engine. Install an engine-enabled Write build "
      "to create or edit documents.");
}

Result UnavailableEngine::initialize(const EngineConfiguration&) { return unavailable(); }
std::string UnavailableEngine::engine_version() const { return "unavailable"; }
void UnavailableEngine::set_event_sink(EventSink sink) { sink_ = std::move(sink); }
Result UnavailableEngine::create_blank() { return unavailable(); }
Result UnavailableEngine::open(const std::filesystem::path&) { return unavailable(); }
void UnavailableEngine::close() noexcept {}
bool UnavailableEngine::has_document() const noexcept { return false; }
DocumentStatus UnavailableEngine::status() const { return {}; }
Result UnavailableEngine::render_tile(const TileRequest&, RenderedTile&) { return unavailable(); }
Result UnavailableEngine::set_visible_area(const Rect&) { return unavailable(); }
Result UnavailableEngine::send_key(const KeyEvent&) { return unavailable(); }
Result UnavailableEngine::send_composition(const CompositionEvent&) { return unavailable(); }
Result UnavailableEngine::send_pointer(const PointerEvent&) { return unavailable(); }
Result UnavailableEngine::execute(const CommandInvocation&) { return unavailable(); }
CommandState UnavailableEngine::command_state(EngineCommand) const { return {}; }
Result UnavailableEngine::paste(std::string_view, std::span<const std::byte>) {
  return unavailable();
}
Result UnavailableEngine::insert_image(const std::filesystem::path&) { return unavailable(); }
Result UnavailableEngine::drag_graphic_handle(bool, std::int64_t, std::int64_t) {
  return unavailable();
}
std::string UnavailableEngine::selected_text() const { return {}; }
Result UnavailableEngine::save_as(const SaveRequest&) { return unavailable(); }
Result UnavailableEngine::validate_saved_document(const std::filesystem::path&) {
  return unavailable();
}
Result UnavailableEngine::adopt_saved_document(const std::filesystem::path&,
                                               DocumentFormat) {
  return unavailable();
}
Result UnavailableEngine::export_pdf(const std::filesystem::path&) { return unavailable(); }
AccessibilitySnapshot UnavailableEngine::accessibility_snapshot() const { return {}; }

Result UnavailableEngine::dispatch_command(std::string_view, std::string_view, bool) {
  return unavailable();
}
std::optional<std::string> UnavailableEngine::command_value(std::string_view) const {
  return std::nullopt;
}
std::string UnavailableEngine::command_values(std::string_view) { return {}; }
Result UnavailableEngine::set_view_theme(ViewTheme) { return unavailable(); }
Result UnavailableEngine::answer_dialog(int, bool) { return unavailable(); }
Result UnavailableEngine::request_context_menu(std::int64_t, std::int64_t) { return unavailable(); }
std::vector<OutlineHeading> UnavailableEngine::outline() { return {}; }

}  // namespace luma::write::engine
