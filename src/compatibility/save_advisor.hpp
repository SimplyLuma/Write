#pragma once

#include "document_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luma::write::compatibility {

enum class SaveChoiceKind {
  SaveDocxCopy,
  SaveOdtCopy,
  KeepCurrentFormat,
  ExportLegacyDoc,
  ExportPdf,
  ExportPlainText,
  Cancel,
};

struct SaveChoice {
  SaveChoiceKind kind = SaveChoiceKind::Cancel;
  std::string label;
  std::string explanation;
  bool recommended = false;
};

struct EngineFormatCapabilities {
  bool legacy_doc_export = false;
  // Macro formats remain blocked unless preservation without execution has been
  // proved for the exact pinned engine. This flag is intentionally opt-in.
  bool macro_payload_roundtrip_verified = false;
};

struct SaveAdvice {
  DocumentFormat target_format = DocumentFormat::Unknown;
  bool allowed = false;
  bool replaces_working_document = false;
  bool requires_confirmation = false;
  std::string headline;
  std::string explanation;
  std::vector<CompatibilityIssue> issues;
  std::vector<SaveChoice> choices;
};

[[nodiscard]] SaveAdvice advise_save(
    const DocumentState& state, DocumentFormat target_format,
    const EngineFormatCapabilities& capabilities = {});

}  // namespace luma::write::compatibility
