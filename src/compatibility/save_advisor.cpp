#include "save_advisor.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace luma::write::compatibility {
namespace {

SaveChoice choice(const SaveChoiceKind kind, std::string label,
                  std::string explanation, const bool recommended = false) {
  return {.kind = kind,
          .label = std::move(label),
          .explanation = std::move(explanation),
          .recommended = recommended};
}

CompatibilityIssue target_limitation(std::string id, std::string title,
                                     std::string explanation,
                                     const CompatibilitySeverity severity) {
  return {
      .id = std::move(id),
      .kind = CompatibilityIssueKind::TargetFormatLimitation,
      .severity = severity,
      .title = std::move(title),
      .explanation = std::move(explanation),
  };
}

void add_safe_copy_choices(SaveAdvice& advice) {
  advice.choices.push_back(choice(
      SaveChoiceKind::SaveDocxCopy, "Save a DOCX copy",
      "Best for sharing with Microsoft Word. Known macros are removed.", true));
  advice.choices.push_back(choice(
      SaveChoiceKind::SaveOdtCopy, "Save an ODT copy",
      "Open format with the best preservation in Luma and LibreOffice-based tools."));
  advice.choices.push_back(
      choice(SaveChoiceKind::Cancel, "Cancel", "Return to the document without saving."));
}

void deduplicate_issues(std::vector<CompatibilityIssue>& issues) {
  std::unordered_set<std::string> ids;
  std::erase_if(issues, [&ids](const CompatibilityIssue& issue) {
    return !ids.insert(issue.id).second;
  });
}

}  // namespace

SaveAdvice advise_save(const DocumentState& state,
                       const DocumentFormat target_format,
                       const EngineFormatCapabilities& capabilities) {
  SaveAdvice advice;
  advice.target_format = target_format;
  advice.issues = collect_compatibility_issues(state);
  const FormatPolicy& target = format_policy(target_format);

  if (target_format == DocumentFormat::Unknown) {
    advice.headline = "Choose a supported document format";
    advice.explanation = "Write cannot safely create this file type.";
    advice.choices.push_back(
        choice(SaveChoiceKind::Cancel, "Cancel", "Return to the document."));
    return advice;
  }

  if (target.macro_enabled) {
    advice.headline = "Macro-enabled saving is unavailable";
    advice.explanation =
        capabilities.macro_payload_roundtrip_verified
            ? "The engine can preserve this payload, but v0 does not write "
              "macro-enabled files because active content cannot be audited safely."
            : "The installed document engine has not been proved to preserve the "
              "macro payload without execution.";
    advice.issues.push_back(macro_content_issue());
    deduplicate_issues(advice.issues);
    add_safe_copy_choices(advice);
    return advice;
  }

  if (target_format == DocumentFormat::LegacyDoc) {
    advice.allowed = capabilities.legacy_doc_export;
    advice.replaces_working_document = false;
    advice.requires_confirmation = true;
    advice.headline = advice.allowed ? "Export an older Word copy?"
                                     : "Legacy Word export is unavailable";
    advice.explanation = advice.allowed
                             ? "The .doc format can lose modern layout, review, "
                               "typography, and object features. Your working "
                               "document will keep its current format."
                             : "The installed document engine does not expose a "
                               "verified Word 97–2003 export filter.";
    advice.issues.push_back(target_limitation(
        "target:legacy-doc", "Older Word format may change the document",
        "Modern layout, review, typography, and object features may be changed or "
        "removed in a Word 97–2003 copy.",
        CompatibilitySeverity::DataLoss));
    if (advice.allowed) {
      advice.choices.push_back(choice(
          SaveChoiceKind::ExportLegacyDoc, "Export Legacy Word 97–2003 (.doc)",
          "Create an older-format copy and keep the working document unchanged."));
    }
    advice.choices.push_back(choice(
        SaveChoiceKind::SaveDocxCopy, "Save a DOCX copy",
        "Preserve modern document features and broad Word compatibility.", true));
    advice.choices.push_back(
        choice(SaveChoiceKind::Cancel, "Cancel", "Return without exporting."));
    return advice;
  }

  if (target_format == DocumentFormat::Pdf) {
    advice.allowed = true;
    advice.replaces_working_document = false;
    advice.headline = "Export a PDF copy";
    advice.explanation =
        "PDF preserves the printed appearance but does not replace your editable "
        "working document.";
    advice.choices.push_back(choice(
        SaveChoiceKind::ExportPdf, "Export PDF", "Create a read-only sharing copy.",
        true));
    advice.choices.push_back(
        choice(SaveChoiceKind::Cancel, "Cancel", "Return without exporting."));
    return advice;
  }

  if (target_format == DocumentFormat::PlainText) {
    advice.allowed = true;
    advice.replaces_working_document = false;
    advice.requires_confirmation = true;
    advice.headline = "Export plain text without formatting?";
    advice.explanation =
        "Plain text keeps characters and paragraphs only. Images, tables, styles, "
        "comments, and page layout are not included.";
    advice.issues.push_back(target_limitation(
        "target:plain-text", "Formatting and objects are not included",
        "A plain-text copy contains no images, tables, styles, comments, or page "
        "layout.",
        CompatibilitySeverity::DataLoss));
    advice.choices.push_back(choice(
        SaveChoiceKind::ExportPlainText, "Export plain text",
        "Create a text-only copy and keep the working document unchanged."));
    advice.choices.push_back(choice(
        SaveChoiceKind::SaveDocxCopy, "Save a DOCX copy",
        "Keep formatting, images, and document structure.", true));
    advice.choices.push_back(
        choice(SaveChoiceKind::Cancel, "Cancel", "Return without exporting."));
    return advice;
  }

  const bool source_requires_safe_copy =
      state.current_save_format == DocumentFormat::LegacyDoc ||
      state.current_save_format == DocumentFormat::MacroEnabledDocx ||
      state.current_save_format == DocumentFormat::MacroEnabledTemplate ||
      state.macros.presence == MacroPresence::Present;

  advice.allowed = target_format == DocumentFormat::Docx ||
                   target_format == DocumentFormat::Odt;
  advice.replaces_working_document = !source_requires_safe_copy &&
                                     state.current_save_format == target_format;
  advice.requires_confirmation = source_requires_safe_copy ||
                                 state.current_save_format != target_format ||
                                 has_compatibility_risk(state);
  advice.headline = target_format == DocumentFormat::Docx
                        ? "Save as a Word document"
                        : "Save as an OpenDocument file";
  advice.explanation = target.explanation;

  if (state.macros.presence == MacroPresence::Present) {
    advice.issues.push_back(target_limitation(
        "target:macro-removal", "Macros will be removed",
        "The safe DOCX or ODT copy will not contain executable macros. The original "
        "macro-enabled file is left unchanged.",
        CompatibilitySeverity::DataLoss));
  }

  if (advice.requires_confirmation) {
    const bool keeping_current_format =
        !source_requires_safe_copy && state.current_save_format == target_format;
    if (keeping_current_format) {
      advice.choices.push_back(choice(
          SaveChoiceKind::KeepCurrentFormat, "Keep current format",
          "Save the document as " + std::string(target.short_label) +
              " while keeping the listed compatibility risks visible.",
          true));
      advice.choices.push_back(choice(
          target_format == DocumentFormat::Docx ? SaveChoiceKind::SaveOdtCopy
                                                 : SaveChoiceKind::SaveDocxCopy,
          target_format == DocumentFormat::Docx ? "Save an ODT copy"
                                                 : "Save a DOCX copy",
          "Choose a different preservation format without replacing the current "
          "file."));
    } else {
      advice.choices.push_back(choice(
          target_format == DocumentFormat::Docx ? SaveChoiceKind::SaveDocxCopy
                                                 : SaveChoiceKind::SaveOdtCopy,
          target_format == DocumentFormat::Docx ? "Save a DOCX copy"
                                                 : "Save an ODT copy",
          source_requires_safe_copy
              ? "Create a safe editable copy and leave the original file unchanged."
              : "Use the selected format for this document.",
          true));
      if (state.original_path.has_value()) {
        advice.choices.push_back(choice(
            SaveChoiceKind::KeepCurrentFormat, "Keep current format",
            "Save using " +
                std::string(format_policy(state.current_save_format).short_label) +
                " instead."));
      }
    }
    advice.choices.push_back(
        choice(SaveChoiceKind::Cancel, "Cancel", "Return without saving."));
  }
  return advice;
}

}  // namespace luma::write::compatibility
