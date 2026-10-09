#include "document_state.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace luma::write::compatibility {
namespace {

std::string location_suffix(const std::optional<int> page) {
  if (!page.has_value()) {
    return {};
  }
  return " The first affected text is on page " + std::to_string(*page) + ".";
}

void append_unique(std::vector<CompatibilityIssue>& destination,
                   std::unordered_set<std::string>& ids,
                   CompatibilityIssue issue) {
  if (ids.insert(issue.id).second) {
    destination.push_back(std::move(issue));
  }
}

}  // namespace

CompatibilityIssue missing_font_issue(std::string requested_family,
                                      const std::optional<int> page) {
  CompatibilityIssue issue;
  issue.id = "missing-font:" + requested_family;
  issue.kind = CompatibilityIssueKind::MissingFont;
  issue.severity = CompatibilitySeverity::PotentialVisualChange;
  issue.title = "Font not installed: " + requested_family;
  issue.explanation =
      "The document asks for “" + requested_family +
      "”, but that font is not installed. Text may wrap or paginate differently." +
      location_suffix(page);
  issue.page_number = page;
  return issue;
}

CompatibilityIssue font_substitution_issue(
    const FontSubstitution& substitution) {
  CompatibilityIssue issue;
  issue.id = "font-substitution:" + substitution.requested_family + ":" +
             substitution.replacement_family;
  issue.kind = CompatibilityIssueKind::FontSubstitution;
  issue.severity = CompatibilitySeverity::PotentialVisualChange;
  issue.title = substitution.requested_family + " was replaced";
  issue.explanation = "Write is displaying “" + substitution.replacement_family +
                      "” instead of “" + substitution.requested_family +
                      "”. Line breaks and page breaks may change." +
                      location_suffix(substitution.first_page);
  issue.page_number = substitution.first_page;
  return issue;
}

CompatibilityIssue macro_content_issue() {
  return {
      .id = "macro-content",
      .kind = CompatibilityIssueKind::MacroContent,
      .severity = CompatibilitySeverity::DataLoss,
      .title = "This file contains macros",
      .explanation =
          "Macros are disabled and have not run. Editing requires a DOCX copy; "
          "the copy will not contain executable macros.",
  };
}

CompatibilityIssue legacy_format_issue() {
  return {
      .id = "legacy-doc-format",
      .kind = CompatibilityIssueKind::LegacyFormat,
      .severity = CompatibilitySeverity::PotentialVisualChange,
      .title = "Older Word format",
      .explanation =
          "Word 97–2003 (.doc) cannot represent every modern layout, review, and "
          "typography feature. Save a DOCX copy for normal editing.",
  };
}

CompatibilityIssue external_link_issue(const ExternalLink& link) {
  CompatibilityIssue issue;
  issue.id = "external-link:" + link.target;
  issue.kind = CompatibilityIssueKind::ExternalLink;
  issue.severity = CompatibilitySeverity::Information;
  issue.title = "External content is blocked";
  issue.explanation =
      "The document refers to “" + link.target +
      "”. Write will not fetch it unless you approve network access.";
  return issue;
}

CompatibilityIssue embedded_object_issue(const EmbeddedObject& object) {
  CompatibilityIssue issue;
  issue.id = "embedded-object:" + object.description;
  issue.kind = CompatibilityIssueKind::EmbeddedObject;
  issue.severity = CompatibilitySeverity::PotentialVisualChange;
  issue.title = "Embedded object is not editable";
  issue.explanation = "“" + object.description +
                      "” is preserved when possible, but its active content is "
                      "blocked and it cannot be edited in this release.";
  issue.object_label = object.description;
  return issue;
}

CompatibilityIssue unsupported_feature_issue(
    std::string stable_id, std::string feature_name, std::string explanation,
    const bool data_loss, const std::optional<int> page,
    std::optional<std::string> object_label) {
  CompatibilityIssue issue;
  issue.id = std::move(stable_id);
  issue.kind = CompatibilityIssueKind::UnsupportedFeature;
  issue.severity = data_loss ? CompatibilitySeverity::DataLoss
                             : CompatibilitySeverity::PotentialVisualChange;
  issue.title = std::move(feature_name);
  issue.explanation = std::move(explanation);
  issue.page_number = page;
  issue.object_label = std::move(object_label);
  return issue;
}

std::vector<CompatibilityIssue> collect_compatibility_issues(
    const DocumentState& state) {
  std::vector<CompatibilityIssue> issues;
  std::unordered_set<std::string> ids;
  issues.reserve(state.detected_issues.size() + state.missing_fonts.size() +
                 state.font_substitutions.size() + state.external_links.size() +
                 state.embedded_objects.size() + 2U);

  for (const CompatibilityIssue& issue : state.detected_issues) {
    append_unique(issues, ids, issue);
  }
  for (const std::string& family : state.missing_fonts) {
    append_unique(issues, ids, missing_font_issue(family));
  }
  for (const FontSubstitution& substitution : state.font_substitutions) {
    append_unique(issues, ids, font_substitution_issue(substitution));
  }
  for (const ExternalLink& link : state.external_links) {
    if (!link.fetched) {
      append_unique(issues, ids, external_link_issue(link));
    }
  }
  for (const EmbeddedObject& object : state.embedded_objects) {
    append_unique(issues, ids, embedded_object_issue(object));
  }
  if (state.macros.presence == MacroPresence::Present) {
    append_unique(issues, ids, macro_content_issue());
  }
  if (state.current_save_format == DocumentFormat::LegacyDoc) {
    append_unique(issues, ids, legacy_format_issue());
  }
  return issues;
}

bool has_compatibility_risk(const DocumentState& state) {
  const auto issues = collect_compatibility_issues(state);
  return std::any_of(issues.begin(), issues.end(), [](const CompatibilityIssue& issue) {
    return issue.severity != CompatibilitySeverity::Information;
  });
}

std::string format_indicator(const DocumentState& state) {
  std::string indicator(format_policy(state.current_save_format).short_label);
  if (has_compatibility_risk(state)) {
    indicator += " · Compatibility";
  }
  return indicator;
}

}  // namespace luma::write::compatibility
