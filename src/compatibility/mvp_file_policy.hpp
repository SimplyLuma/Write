#pragma once

#include "engine/engine_types.hpp"

#include <filesystem>
#include <string>

namespace luma::write::compatibility {

struct PreparedDestination {
  bool ok{false};
  std::filesystem::path path;
  engine::DocumentFormat format{engine::DocumentFormat::unknown};
  bool requires_overwrite_confirmation{false};
  std::string message;
};

// An omitted editable suffix means DOCX. Any other suffix must be an explicit,
// exact DOCX or ODT choice; bytes are never silently relabeled.
[[nodiscard]] PreparedDestination prepare_editable_destination(
    std::filesystem::path requested_path);

// An omitted export suffix means PDF. Conflicting suffixes are rejected.
[[nodiscard]] PreparedDestination prepare_pdf_destination(
    std::filesystem::path requested_path);

[[nodiscard]] bool can_save_in_place(engine::DocumentFormat format) noexcept;

}  // namespace luma::write::compatibility
