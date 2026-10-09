#include "mvp_file_policy.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace luma::write::compatibility {
namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

PreparedDestination inspect_destination(std::filesystem::path path,
                                        const engine::DocumentFormat format) {
  PreparedDestination result{
      .ok = false,
      .path = std::move(path),
      .format = format,
  };
  if (result.path.empty() || result.path.filename().empty()) {
    result.message = "Choose a file name in a local folder.";
    return result;
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(result.path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    result.message = "The destination could not be inspected: " + error.message();
    return result;
  }
  if (!error && std::filesystem::is_symlink(status)) {
    result.message =
        "The selected destination is a symbolic link. Choose the real file "
        "explicitly so another file is not overwritten.";
    return result;
  }
  if (!error && std::filesystem::exists(status)) {
    if (!std::filesystem::is_regular_file(status)) {
      result.message = "The selected destination is not a regular file.";
      return result;
    }
    result.requires_overwrite_confirmation = true;
  }

  result.ok = true;
  return result;
}

}  // namespace

PreparedDestination prepare_editable_destination(
    std::filesystem::path requested_path) {
  std::string extension = lowercase(requested_path.extension().string());
  engine::DocumentFormat format = engine::DocumentFormat::unknown;
  if (extension.empty()) {
    requested_path += ".docx";
    format = engine::DocumentFormat::docx;
  } else if (extension == ".docx") {
    format = engine::DocumentFormat::docx;
  } else if (extension == ".odt") {
    format = engine::DocumentFormat::odt;
  } else {
    return {
        .ok = false,
        .path = std::move(requested_path),
        .format = engine::DocumentFormat::unknown,
        .message =
            "Write MVP saves editable documents only as .docx or .odt. "
            "Change the file-name suffix and try again.",
    };
  }
  return inspect_destination(std::move(requested_path), format);
}

PreparedDestination prepare_pdf_destination(
    std::filesystem::path requested_path) {
  const std::string extension = lowercase(requested_path.extension().string());
  if (extension.empty()) {
    requested_path += ".pdf";
  } else if (extension != ".pdf") {
    return {
        .ok = false,
        .path = std::move(requested_path),
        .format = engine::DocumentFormat::pdf,
        .message =
            "PDF export requires a .pdf file-name suffix. Change the name and "
            "try again.",
    };
  }
  return inspect_destination(std::move(requested_path),
                             engine::DocumentFormat::pdf);
}

bool can_save_in_place(const engine::DocumentFormat format) noexcept {
  return format == engine::DocumentFormat::docx ||
         format == engine::DocumentFormat::odt;
}

}  // namespace luma::write::compatibility
