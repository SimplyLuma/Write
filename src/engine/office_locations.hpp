// SPDX-License-Identifier: MPL-2.0
// Adapted from ProjectLuma/Write luma-port/write, c042f5e3666af230e9051ecbcd03fdce3af06199.
// Only discovery is shared here. Each platform's exact engine still needs qualification.
#pragma once
#include <filesystem>
#include <vector>
#include <string>
#include <cstdlib>

namespace luma::office {
inline void add_platform_candidates(std::vector<std::filesystem::path>& candidates) {
#if defined(__ANDROID__)
  (void)candidates; // The APK launcher supplies LUMA_LIBREOFFICE_PROGRAM.
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME"); home && *home) {
    candidates.emplace_back(std::filesystem::path(home) /
      "Library/Application Support/Luma/LibreOffice/current/Contents/Frameworks");
  }
  candidates.emplace_back("/Library/Application Support/Luma/LibreOffice/current/Contents/Frameworks");
  candidates.emplace_back("/Applications/LibreOffice.app/Contents/Frameworks");
#else
  candidates.emplace_back("/usr/lib64/libreoffice/program");
  candidates.emplace_back("/usr/lib/libreoffice/program");
#endif
}
inline bool looks_like_program_dir(const std::filesystem::path& path) {
  std::error_code error;
#if defined(__ANDROID__)
  // Android's code is linked from liblo-native-code.so; this directory contains data.
  return std::filesystem::is_regular_file(path / "sofficerc", error);
#elif defined(__APPLE__)
  return std::filesystem::is_regular_file(path / "libsofficeapp.dylib", error) ||
         std::filesystem::is_regular_file(path / "libmergedlo.dylib", error);
#else
  return std::filesystem::is_regular_file(path / "libsofficeapp.so", error) ||
         std::filesystem::is_regular_file(path / "libmergedlo.so", error);
#endif
}
inline std::string where_to_install() {
#if defined(__ANDROID__)
  return "The APK must include its matching LibreOffice engine and program data.";
#elif defined(__APPLE__)
  return "The app needs a qualified native LibreOfficeKit runtime.";
#else
  return "Expected its program directory under /usr/lib64/libreoffice or LUMA_LIBREOFFICE_PROGRAM.";
#endif
}
} // namespace luma::office
