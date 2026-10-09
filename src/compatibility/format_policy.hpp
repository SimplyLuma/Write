#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace luma::write::compatibility {

// These are product formats, rather than LibreOffice filter names. Engine filter
// details stay in the adapter and are recorded separately in DocumentState.
enum class DocumentFormat {
  Docx,
  Odt,
  LegacyDoc,
  Pdf,
  PlainText,
  MacroEnabledDocx,
  MacroEnabledTemplate,
  Unknown,
};

enum class OpenMode {
  Editable,
  ImportAsNewDocument,
  ProtectedView,
  NotEditable,
  Unsupported,
};

enum class SaveMode {
  PreserveFormat,
  RequireSaveAsDocx,
  ExportOnly,
  Unsupported,
};

struct FormatPolicy {
  DocumentFormat format = DocumentFormat::Unknown;
  std::string_view display_name;
  std::string_view short_label;
  std::string_view canonical_extension;
  std::string_view mime_type;
  OpenMode open_mode = OpenMode::Unsupported;
  SaveMode save_mode = SaveMode::Unsupported;
  DocumentFormat recommended_save_format = DocumentFormat::Docx;
  bool can_open = false;
  bool can_edit = false;
  bool can_export = false;
  bool macro_enabled = false;
  // Deliberately constant in every policy in v0. It exists to make the safety
  // guarantee explicit to callers instead of leaving it to UI wording.
  bool macro_execution_allowed = false;
  std::string_view explanation;
};

[[nodiscard]] const FormatPolicy& format_policy(DocumentFormat format) noexcept;
[[nodiscard]] DocumentFormat document_format_from_extension(
    std::string_view extension);
[[nodiscard]] DocumentFormat document_format_from_mime(
    std::string_view mime_type);
[[nodiscard]] DocumentFormat detect_document_format(
    const std::filesystem::path& path,
    std::string_view mime_type = {});
[[nodiscard]] std::string normalized_extension(std::string_view extension);
[[nodiscard]] std::string_view format_key(DocumentFormat format) noexcept;
[[nodiscard]] DocumentFormat document_format_from_key(
    std::string_view key);

}  // namespace luma::write::compatibility
