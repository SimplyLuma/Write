#include "suite/suite_context.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace luma::suite {
namespace {

std::string normalized(const std::string_view value) {
  std::string result{value};
  const auto first = std::find_if_not(result.begin(), result.end(),
                                      [](const unsigned char character) {
                                        return std::isspace(character) != 0;
                                      });
  const auto last = std::find_if_not(result.rbegin(), result.rend(),
                                     [](const unsigned char character) {
                                       return std::isspace(character) != 0;
                                     }).base();
  result = first < last ? std::string(first, last) : std::string{};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

std::string environment_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : normalized(value);
}

bool handheld_device() {
  const std::string explicit_class = environment_value("LUMA_DEVICE_CLASS");
  if (!explicit_class.empty()) {
    return explicit_class == "handheld";
  }

  std::ifstream marker{"/etc/luma-device-class"};
  std::string device_class;
  return marker.good() && std::getline(marker, device_class) &&
         normalized(device_class) == "handheld";
}

}  // namespace

SuiteContext context_from_values(const std::string_view presentation,
                                 const std::string_view input,
                                 const bool handheld) {
  SuiteContext context{
      .presentation = handheld ? PresentationMode::FullscreenMobile
                               : PresentationMode::Windowed,
      .input = handheld ? InputMode::Touch : InputMode::Pointer,
  };

  const std::string normalized_presentation = normalized(presentation);
  if (normalized_presentation == "windowed") {
    context.presentation = PresentationMode::Windowed;
  } else if (normalized_presentation == "fullscreen-mobile") {
    context.presentation = PresentationMode::FullscreenMobile;
  }

  const std::string normalized_input = normalized(input);
  if (normalized_input == "pointer") {
    context.input = InputMode::Pointer;
  } else if (normalized_input == "touch") {
    context.input = InputMode::Touch;
  }
  return context;
}

SuiteContext context_from_environment() {
  return context_from_values(environment_value("LUMA_PRESENTATION_MODE"),
                             environment_value("LUMA_INPUT_MODE"),
                             handheld_device());
}

}  // namespace luma::suite
