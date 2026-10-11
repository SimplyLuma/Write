// SPDX-License-Identifier: Apache-2.0
#include "engine/office_locations.hpp"
#include <fstream>
#include <iostream>
int main() {
  const auto path = std::filesystem::temp_directory_path() / ("write-office-discovery-" + std::to_string(std::rand()));
  std::error_code error;
  if (!std::filesystem::create_directory(path, error)) return 1;
  const auto fail = [&] { std::filesystem::remove_all(path); return 1; };
  if (luma::office::looks_like_program_dir(path)) return fail();
#if defined(__ANDROID__)
  const char* marker = "sofficerc";
#elif defined(__APPLE__)
  const char* marker = "libsofficeapp.dylib";
#else
  const char* marker = "libsofficeapp.so";
#endif
  // A directory named as the marker is not a usable runtime.
  std::filesystem::create_directory(path / marker);
  if (luma::office::looks_like_program_dir(path)) return fail();
  std::filesystem::remove(path / marker);
  std::ofstream(path / marker) << "fixture";
  if (!luma::office::looks_like_program_dir(path)) return fail();
  std::vector<std::filesystem::path> candidates;
  luma::office::add_platform_candidates(candidates);
#if defined(__ANDROID__)
  if (!candidates.empty()) return fail();
#elif defined(__APPLE__)
  const char* home = std::getenv("HOME");
  if (candidates.size() != (home && *home ? 3U : 2U)) return fail();
#else
  if (candidates.size() != 2 || candidates[0] != "/usr/lib64/libreoffice/program") return fail();
#endif
  std::filesystem::remove_all(path);
  std::cout << "office discovery passed\n";
}
