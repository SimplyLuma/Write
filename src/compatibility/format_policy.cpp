#include "format_policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace luma::write::compatibility {
namespace {

constexpr FormatPolicy kDocx{
    .format = DocumentFormat::Docx,
    .display_name = "Microsoft Word document",
    .short_label = "DOCX",
    .canonical_extension = ".docx",
    .mime_type =
        "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    .open_mode = OpenMode::Editable,
    .save_mode = SaveMode::PreserveFormat,
    .recommended_save_format = DocumentFormat::Docx,
    .can_open = true,
    .can_edit = true,
    .can_export = true,
    .explanation =
        "DOCX is a first-class editable format. Save keeps the document as DOCX.",
};

constexpr FormatPolicy kOdt{
    .format = DocumentFormat::Odt,
    .display_name = "OpenDocument text document",
    .short_label = "ODT",
    .canonical_extension = ".odt",
    .mime_type = "application/vnd.oasis.opendocument.text",
    .open_mode = OpenMode::Editable,
    .save_mode = SaveMode::PreserveFormat,
    .recommended_save_format = DocumentFormat::Odt,
    .can_open = true,
    .can_edit = true,
    .can_export = true,
    .explanation =
        "ODT is a first-class editable open format and is preserved by Save.",
};

constexpr FormatPolicy kLegacyDoc{
    .format = DocumentFormat::LegacyDoc,
    .display_name = "Word 97–2003 document",
    .short_label = "DOC",
    .canonical_extension = ".doc",
    .mime_type = "application/msword",
    .open_mode = OpenMode::Editable,
    .save_mode = SaveMode::RequireSaveAsDocx,
    .recommended_save_format = DocumentFormat::Docx,
    .can_open = true,
    .can_edit = true,
    .can_export = true,
    .explanation =
        "Legacy DOC files can be opened, but edited documents should be saved as "
        "DOCX. Legacy export is an explicit compatibility action.",
};

constexpr FormatPolicy kPdf{
    .format = DocumentFormat::Pdf,
    .display_name = "Portable Document Format",
    .short_label = "PDF",
    .canonical_extension = ".pdf",
    .mime_type = "application/pdf",
    .open_mode = OpenMode::NotEditable,
    .save_mode = SaveMode::ExportOnly,
    .recommended_save_format = DocumentFormat::Pdf,
    .can_open = false,
    .can_edit = false,
    .can_export = true,
    .explanation = "PDF is available for export and printing, not document editing.",
};

constexpr FormatPolicy kPlainText{
    .format = DocumentFormat::PlainText,
    .display_name = "Plain text",
    .short_label = "TXT",
    .canonical_extension = ".txt",
    .mime_type = "text/plain",
    .open_mode = OpenMode::ImportAsNewDocument,
    .save_mode = SaveMode::RequireSaveAsDocx,
    .recommended_save_format = DocumentFormat::Docx,
    .can_open = true,
    .can_edit = true,
    .can_export = true,
    .explanation =
        "Plain text is imported into a new document. It can be exported again, "
        "but it cannot preserve document formatting.",
};

constexpr FormatPolicy kMacroDocx{
    .format = DocumentFormat::MacroEnabledDocx,
    .display_name = "Macro-enabled Word document",
    .short_label = "DOCM",
    .canonical_extension = ".docm",
    .mime_type = "application/vnd.ms-word.document.macroEnabled.12",
    .open_mode = OpenMode::ProtectedView,
    .save_mode = SaveMode::RequireSaveAsDocx,
    .recommended_save_format = DocumentFormat::Docx,
    .can_open = true,
    .can_edit = false,
    .can_export = false,
    .macro_enabled = true,
    .macro_execution_allowed = false,
    .explanation =
        "Macro-enabled documents open with macros disabled. Editing requires a "
        "safe DOCX copy, which removes macros.",
};

constexpr FormatPolicy kMacroTemplate{
    .format = DocumentFormat::MacroEnabledTemplate,
    .display_name = "Macro-enabled Word template",
    .short_label = "DOTM",
    .canonical_extension = ".dotm",
    .mime_type = "application/vnd.ms-word.template.macroEnabled.12",
    .open_mode = OpenMode::ProtectedView,
    .save_mode = SaveMode::RequireSaveAsDocx,
    .recommended_save_format = DocumentFormat::Docx,
    .can_open = true,
    .can_edit = false,
    .can_export = false,
    .macro_enabled = true,
    .macro_execution_allowed = false,
    .explanation =
        "Macro-enabled templates open with macros disabled. Create a safe DOCX "
        "copy before editing.",
};

constexpr FormatPolicy kUnknown{
    .format = DocumentFormat::Unknown,
    .display_name = "Unknown document format",
    .short_label = "UNKNOWN",
    .canonical_extension = "",
    .mime_type = "application/octet-stream",
    .open_mode = OpenMode::Unsupported,
    .save_mode = SaveMode::Unsupported,
    .recommended_save_format = DocumentFormat::Docx,
    .can_open = false,
    .can_edit = false,
    .can_export = false,
    .explanation = "This document format is not supported.",
};

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

}  // namespace

const FormatPolicy& format_policy(const DocumentFormat format) noexcept {
  switch (format) {
    case DocumentFormat::Docx:
      return kDocx;
    case DocumentFormat::Odt:
      return kOdt;
    case DocumentFormat::LegacyDoc:
      return kLegacyDoc;
    case DocumentFormat::Pdf:
      return kPdf;
    case DocumentFormat::PlainText:
      return kPlainText;
    case DocumentFormat::MacroEnabledDocx:
      return kMacroDocx;
    case DocumentFormat::MacroEnabledTemplate:
      return kMacroTemplate;
    case DocumentFormat::Unknown:
      return kUnknown;
  }
  return kUnknown;
}

std::string normalized_extension(const std::string_view extension) {
  std::string normalized = lowercase(extension);
  if (!normalized.empty() && normalized.front() != '.') {
    normalized.insert(normalized.begin(), '.');
  }
  return normalized;
}

DocumentFormat document_format_from_extension(
    const std::string_view extension) {
  const std::string normalized = normalized_extension(extension);
  if (normalized == ".docx") {
    return DocumentFormat::Docx;
  }
  if (normalized == ".odt") {
    return DocumentFormat::Odt;
  }
  if (normalized == ".doc") {
    return DocumentFormat::LegacyDoc;
  }
  if (normalized == ".pdf") {
    return DocumentFormat::Pdf;
  }
  if (normalized == ".txt" || normalized == ".text") {
    return DocumentFormat::PlainText;
  }
  if (normalized == ".docm") {
    return DocumentFormat::MacroEnabledDocx;
  }
  if (normalized == ".dotm") {
    return DocumentFormat::MacroEnabledTemplate;
  }
  return DocumentFormat::Unknown;
}

DocumentFormat document_format_from_mime(
    const std::string_view mime_type) {
  const std::string normalized = lowercase(mime_type);
  constexpr std::array formats{
      DocumentFormat::Docx,          DocumentFormat::Odt,
      DocumentFormat::LegacyDoc,     DocumentFormat::Pdf,
      DocumentFormat::PlainText,     DocumentFormat::MacroEnabledDocx,
      DocumentFormat::MacroEnabledTemplate,
  };
  for (const DocumentFormat format : formats) {
    if (normalized == lowercase(format_policy(format).mime_type)) {
      return format;
    }
  }
  // Some content-type probes append a charset to text/plain.
  if (normalized.starts_with("text/plain;")) {
    return DocumentFormat::PlainText;
  }
  return DocumentFormat::Unknown;
}

DocumentFormat detect_document_format(const std::filesystem::path& path,
                                      const std::string_view mime_type) {
  const DocumentFormat from_extension =
      document_format_from_extension(path.extension().string());
  if (from_extension != DocumentFormat::Unknown) {
    return from_extension;
  }
  return document_format_from_mime(mime_type);
}

std::string_view format_key(const DocumentFormat format) noexcept {
  switch (format) {
    case DocumentFormat::Docx:
      return "docx";
    case DocumentFormat::Odt:
      return "odt";
    case DocumentFormat::LegacyDoc:
      return "doc";
    case DocumentFormat::Pdf:
      return "pdf";
    case DocumentFormat::PlainText:
      return "txt";
    case DocumentFormat::MacroEnabledDocx:
      return "docm";
    case DocumentFormat::MacroEnabledTemplate:
      return "dotm";
    case DocumentFormat::Unknown:
      return "unknown";
  }
  return "unknown";
}

DocumentFormat document_format_from_key(const std::string_view key) {
  if (key == "unknown") {
    return DocumentFormat::Unknown;
  }
  return document_format_from_extension(key);
}

}  // namespace luma::write::compatibility
