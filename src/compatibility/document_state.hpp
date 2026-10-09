#pragma once

#include "format_policy.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace luma::write::compatibility {

enum class CompatibilitySeverity {
  Information,
  PotentialVisualChange,
  DataLoss,
};

enum class CompatibilityIssueKind {
  UnsupportedFeature,
  MissingFont,
  FontSubstitution,
  ExternalLink,
  EmbeddedObject,
  MacroContent,
  LegacyFormat,
  TargetFormatLimitation,
  OutputValidation,
};

struct CompatibilityIssue {
  std::string id;
  CompatibilityIssueKind kind = CompatibilityIssueKind::UnsupportedFeature;
  CompatibilitySeverity severity = CompatibilitySeverity::Information;
  std::string title;
  std::string explanation;
  std::optional<int> page_number;
  std::optional<std::string> object_label;
};

struct FontSubstitution {
  std::string requested_family;
  std::string replacement_family;
  std::optional<int> first_page;
};

struct ExternalLink {
  std::string target;
  bool user_approved_access = false;
  bool fetched = false;
};

struct EmbeddedObject {
  std::string description;
  bool active_content_blocked = true;
};

enum class MacroPresence {
  Unknown,
  Absent,
  Present,
};

struct MacroMetadata {
  MacroPresence presence = MacroPresence::Unknown;
  bool execution_attempted = false;

  // There is intentionally no setter or stored `enabled` bit in v0.
  [[nodiscard]] constexpr bool execution_enabled() const noexcept { return false; }
};

struct DocumentState {
  std::optional<std::filesystem::path> original_path;
  std::string original_extension;
  std::string mime_type;
  std::string engine_filter;
  DocumentFormat current_save_format = DocumentFormat::Docx;
  bool modified = false;

  std::vector<CompatibilityIssue> detected_issues;
  std::vector<std::string> missing_fonts;
  std::vector<FontSubstitution> font_substitutions;
  std::vector<ExternalLink> external_links;
  std::vector<EmbeddedObject> embedded_objects;
  MacroMetadata macros;

  std::optional<std::chrono::system_clock::time_point> last_explicit_save;
  std::optional<std::chrono::system_clock::time_point> last_autosave;
};

[[nodiscard]] CompatibilityIssue missing_font_issue(
    std::string requested_family, std::optional<int> page = std::nullopt);
[[nodiscard]] CompatibilityIssue font_substitution_issue(
    const FontSubstitution& substitution);
[[nodiscard]] CompatibilityIssue macro_content_issue();
[[nodiscard]] CompatibilityIssue legacy_format_issue();
[[nodiscard]] CompatibilityIssue external_link_issue(const ExternalLink& link);
[[nodiscard]] CompatibilityIssue embedded_object_issue(
    const EmbeddedObject& object);
[[nodiscard]] CompatibilityIssue unsupported_feature_issue(
    std::string stable_id, std::string feature_name, std::string explanation,
    bool data_loss, std::optional<int> page = std::nullopt,
    std::optional<std::string> object_label = std::nullopt);

[[nodiscard]] std::vector<CompatibilityIssue> collect_compatibility_issues(
    const DocumentState& state);
[[nodiscard]] bool has_compatibility_risk(const DocumentState& state);
[[nodiscard]] std::string format_indicator(const DocumentState& state);

}  // namespace luma::write::compatibility
