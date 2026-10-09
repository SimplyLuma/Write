#include "compatibility/document_state.hpp"
#include "compatibility/format_policy.hpp"
#include "compatibility/save_advisor.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace compatibility = luma::write::compatibility;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "          \
                << #condition << '\n';                                         \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

bool has_choice(const compatibility::SaveAdvice& advice,
                const compatibility::SaveChoiceKind kind,
                const bool recommended = false) {
  return std::any_of(advice.choices.begin(), advice.choices.end(),
                     [kind, recommended](const compatibility::SaveChoice& choice) {
                       return choice.kind == kind &&
                              (!recommended || choice.recommended);
                     });
}

bool has_issue(const compatibility::SaveAdvice& advice,
               const std::string& id) {
  return std::any_of(advice.issues.begin(), advice.issues.end(),
                     [&id](const compatibility::CompatibilityIssue& issue) {
                       return issue.id == id;
                     });
}

void format_detection_and_policy() {
  using enum compatibility::DocumentFormat;
  CHECK(compatibility::document_format_from_extension(".DOCX") == Docx);
  CHECK(compatibility::document_format_from_extension("odt") == Odt);
  CHECK(compatibility::document_format_from_extension(".doc") == LegacyDoc);
  CHECK(compatibility::document_format_from_extension(".pdf") == Pdf);
  CHECK(compatibility::document_format_from_extension("TXT") == PlainText);
  CHECK(compatibility::document_format_from_extension(".docm") ==
        MacroEnabledDocx);
  CHECK(compatibility::document_format_from_extension(".dotm") ==
        MacroEnabledTemplate);
  CHECK(compatibility::document_format_from_extension(".rtf") == Unknown);

  CHECK(compatibility::detect_document_format(
            std::filesystem::path("report.unknown"),
            "application/vnd.oasis.opendocument.text") == Odt);
  CHECK(compatibility::document_format_from_mime("text/plain; charset=utf-8") ==
        PlainText);

  const auto& docx = compatibility::format_policy(Docx);
  CHECK(docx.can_open);
  CHECK(docx.can_edit);
  CHECK(docx.save_mode == compatibility::SaveMode::PreserveFormat);

  const auto& doc = compatibility::format_policy(LegacyDoc);
  CHECK(doc.can_open);
  CHECK(doc.save_mode == compatibility::SaveMode::RequireSaveAsDocx);
  CHECK(doc.recommended_save_format == Docx);

  const auto& pdf = compatibility::format_policy(Pdf);
  CHECK(!pdf.can_open);
  CHECK(!pdf.can_edit);
  CHECK(pdf.save_mode == compatibility::SaveMode::ExportOnly);

  for (const compatibility::DocumentFormat macro_format :
       {MacroEnabledDocx, MacroEnabledTemplate}) {
    const auto& policy = compatibility::format_policy(macro_format);
    CHECK(policy.macro_enabled);
    CHECK(!policy.macro_execution_allowed);
    CHECK(policy.open_mode == compatibility::OpenMode::ProtectedView);
    CHECK(policy.save_mode == compatibility::SaveMode::RequireSaveAsDocx);
  }
}

void document_state_issues_are_plain_and_deduplicated() {
  compatibility::DocumentState state;
  state.current_save_format = compatibility::DocumentFormat::LegacyDoc;
  state.missing_fonts = {"Aptos", "Aptos"};
  state.font_substitutions.push_back({
      .requested_family = "Calibri",
      .replacement_family = "Carlito",
      .first_page = 2,
  });
  state.external_links.push_back({
      .target = "https://example.test/chart.csv",
      .user_approved_access = false,
      .fetched = false,
  });
  state.embedded_objects.push_back({.description = "Quarterly forecast chart"});
  state.macros.presence = compatibility::MacroPresence::Present;

  const auto issues = compatibility::collect_compatibility_issues(state);
  CHECK(issues.size() == 6U);
  CHECK(std::count_if(issues.begin(), issues.end(), [](const auto& issue) {
          return issue.id == "missing-font:Aptos";
        }) == 1);
  CHECK(std::all_of(issues.begin(), issues.end(), [](const auto& issue) {
    return !issue.title.empty() && !issue.explanation.empty();
  }));
  CHECK(compatibility::has_compatibility_risk(state));
  CHECK(compatibility::format_indicator(state) == "DOC · Compatibility");
  CHECK(!state.macros.execution_enabled());
}

void save_advice_for_normal_and_legacy_documents() {
  compatibility::DocumentState clean_docx;
  clean_docx.current_save_format = compatibility::DocumentFormat::Docx;
  clean_docx.original_path = "/work/report.docx";

  auto advice = compatibility::advise_save(
      clean_docx, compatibility::DocumentFormat::Docx);
  CHECK(advice.allowed);
  CHECK(advice.replaces_working_document);
  CHECK(!advice.requires_confirmation);

  compatibility::DocumentState legacy;
  legacy.current_save_format = compatibility::DocumentFormat::LegacyDoc;
  legacy.original_path = "/work/contract.doc";
  advice = compatibility::advise_save(legacy, compatibility::DocumentFormat::Docx);
  CHECK(advice.allowed);
  CHECK(!advice.replaces_working_document);
  CHECK(advice.requires_confirmation);
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::SaveDocxCopy, true));

  advice = compatibility::advise_save(
      legacy, compatibility::DocumentFormat::LegacyDoc,
      {.legacy_doc_export = false});
  CHECK(!advice.allowed);
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::SaveDocxCopy, true));
  CHECK(!has_choice(advice, compatibility::SaveChoiceKind::ExportLegacyDoc));

  advice = compatibility::advise_save(
      legacy, compatibility::DocumentFormat::LegacyDoc,
      {.legacy_doc_export = true});
  CHECK(advice.allowed);
  CHECK(!advice.replaces_working_document);
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::ExportLegacyDoc));
  CHECK(has_issue(advice, "target:legacy-doc"));
}

void export_and_macro_advice() {
  compatibility::DocumentState state;
  state.current_save_format = compatibility::DocumentFormat::Docx;
  state.original_path = "/work/report.docx";

  auto advice =
      compatibility::advise_save(state, compatibility::DocumentFormat::Pdf);
  CHECK(advice.allowed);
  CHECK(!advice.replaces_working_document);
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::ExportPdf, true));

  advice =
      compatibility::advise_save(state, compatibility::DocumentFormat::PlainText);
  CHECK(advice.allowed);
  CHECK(advice.requires_confirmation);
  CHECK(has_issue(advice, "target:plain-text"));
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::SaveDocxCopy, true));

  state.current_save_format = compatibility::DocumentFormat::MacroEnabledDocx;
  state.macros.presence = compatibility::MacroPresence::Present;
  advice =
      compatibility::advise_save(state, compatibility::DocumentFormat::Docx);
  CHECK(advice.allowed);
  CHECK(!advice.replaces_working_document);
  CHECK(advice.requires_confirmation);
  CHECK(has_issue(advice, "target:macro-removal"));
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::SaveDocxCopy, true));

  advice = compatibility::advise_save(
      state, compatibility::DocumentFormat::MacroEnabledDocx,
      {.macro_payload_roundtrip_verified = true});
  CHECK(!advice.allowed);
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::SaveDocxCopy, true));
  CHECK(has_choice(advice, compatibility::SaveChoiceKind::Cancel));
}

}  // namespace

int main() {
  format_detection_and_policy();
  document_state_issues_are_plain_and_deduplicated();
  save_advice_for_normal_and_legacy_documents();
  export_and_macro_advice();
  if (failures != 0) {
    std::cerr << failures << " compatibility policy test(s) failed\n";
    return 1;
  }
  std::cout << "compatibility policy tests passed\n";
  return 0;
}
