#pragma once

#include <string_view>

namespace luma::suite {

enum class PresentationMode { Windowed, FullscreenMobile };
enum class InputMode { Pointer, Touch };

struct SuiteContext {
  PresentationMode presentation{PresentationMode::Windowed};
  InputMode input{InputMode::Pointer};

  [[nodiscard]] bool decorated() const noexcept {
    return presentation == PresentationMode::Windowed;
  }
  [[nodiscard]] bool touch_targets() const noexcept {
    return input == InputMode::Touch;
  }
};

/**
 * Resolve Luma's independent presentation and input capabilities.
 *
 * Width remains a layout input owned by GTK. It is deliberately not used to
 * infer whether a device is handheld or whether it has touch input.
 */
[[nodiscard]] SuiteContext context_from_values(
    std::string_view presentation, std::string_view input,
    bool handheld_device);

[[nodiscard]] SuiteContext context_from_environment();

}  // namespace luma::suite
