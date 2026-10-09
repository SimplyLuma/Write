#include "recovery.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace luma::write::persistence {
namespace {

constexpr std::string_view kMagic = "LUMA_WRITE_RECOVERY";
constexpr std::size_t kMaximumMetadataBytes = 1024U * 1024U;
constexpr std::size_t kMaximumIdentifierBytes = 1024U;
constexpr std::size_t kMaximumPathBytes = 32U * 1024U;
constexpr std::size_t kMaximumDigestBytes = 4096U;

std::int64_t to_epoch_milliseconds(const RecoveryClock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             value.time_since_epoch())
      .count();
}

RecoveryClock::time_point from_epoch_milliseconds(const std::int64_t value) {
  return RecoveryClock::time_point{std::chrono::milliseconds{value}};
}

std::string_view reason_key(const RecoveryReason reason) {
  switch (reason) {
    case RecoveryReason::PeriodicAutosave:
      return "periodic";
    case RecoveryReason::BeforeRiskyOperation:
      return "before-risky-operation";
    case RecoveryReason::EngineFailure:
      return "engine-failure";
    case RecoveryReason::UncleanShutdown:
      return "unclean-shutdown";
  }
  return "periodic";
}

std::optional<RecoveryReason> parse_reason(const std::string_view value) {
  if (value == "periodic") {
    return RecoveryReason::PeriodicAutosave;
  }
  if (value == "before-risky-operation") {
    return RecoveryReason::BeforeRiskyOperation;
  }
  if (value == "engine-failure") {
    return RecoveryReason::EngineFailure;
  }
  if (value == "unclean-shutdown") {
    return RecoveryReason::UncleanShutdown;
  }
  return std::nullopt;
}

RecoveryChoice recovery_choice(const RecoveryChoiceKind kind, std::string label,
                               std::string explanation,
                               const bool recommended = false) {
  return {.kind = kind,
          .label = std::move(label),
          .explanation = std::move(explanation),
          .recommended = recommended};
}

bool read_expected_key(std::istringstream& stream, const std::string_view expected,
                       std::string& error) {
  std::string key;
  if (!(stream >> key) || key != expected) {
    error = "Recovery metadata is missing the “" + std::string(expected) +
            "” field.";
    return false;
  }
  return true;
}

}  // namespace

bool autosave_due(const AutosavePolicy& policy, const bool document_modified,
                  const std::optional<RecoveryClock::time_point> last_autosave,
                  const RecoveryClock::time_point now) noexcept {
  if (!policy.enabled || !document_modified || policy.interval.count() <= 0) {
    return false;
  }
  return !last_autosave.has_value() || now - *last_autosave >= policy.interval;
}

std::string serialize_recovery_metadata(
    const RecoverySnapshotMetadata& metadata) {
  std::ostringstream output;
  output << kMagic << ' ' << metadata.schema_version << '\n';
  output << "document_id " << std::quoted(metadata.document_id) << '\n';
  output << "original_path " << (metadata.original_path.has_value() ? 1 : 0);
  if (metadata.original_path.has_value()) {
    output << ' ' << std::quoted(metadata.original_path->string());
  }
  output << '\n';
  output << "snapshot_path " << std::quoted(metadata.snapshot_path.string()) << '\n';
  output << "source_format "
         << compatibility::format_key(metadata.source_format) << '\n';
  output << "created_at_ms " << to_epoch_milliseconds(metadata.created_at) << '\n';
  output << "last_explicit_save_ms "
         << (metadata.last_explicit_save.has_value() ? 1 : 0);
  if (metadata.last_explicit_save.has_value()) {
    output << ' ' << to_epoch_milliseconds(*metadata.last_explicit_save);
  }
  output << '\n';
  output << "sequence " << metadata.sequence << '\n';
  output << "byte_size " << metadata.byte_size << '\n';
  output << "content_digest " << std::quoted(metadata.content_digest) << '\n';
  output << "validated " << (metadata.validated ? 1 : 0) << '\n';
  output << "reason " << reason_key(metadata.reason) << '\n';
  return output.str();
}

MetadataParseResult parse_recovery_metadata(const std::string_view text) {
  MetadataParseResult result;
  if (text.size() > kMaximumMetadataBytes) {
    result.error = "Recovery metadata is unreasonably large.";
    return result;
  }
  std::istringstream input{std::string(text)};
  std::string header;
  if (!std::getline(input, header)) {
    result.error = "Unsupported or damaged recovery metadata header.";
    return result;
  }
  std::istringstream header_stream(header);
  std::string magic;
  std::uint32_t schema_version = 0;
  std::string header_trailing;
  if (!(header_stream >> magic >> schema_version) || magic != kMagic ||
      (header_stream >> header_trailing) || schema_version != 1U) {
    result.error = "Unsupported or damaged recovery metadata header.";
    return result;
  }

  RecoverySnapshotMetadata metadata;
  metadata.schema_version = schema_version;
  if (!read_expected_key(input, "document_id", result.error) ||
      !(input >> std::quoted(metadata.document_id))) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid document ID.";
    }
    return result;
  }

  int has_original = 0;
  if (!read_expected_key(input, "original_path", result.error) ||
      !(input >> has_original) || (has_original != 0 && has_original != 1)) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid original path marker.";
    }
    return result;
  }
  if (has_original == 1) {
    std::string path;
    if (!(input >> std::quoted(path))) {
      result.error = "Recovery metadata has an invalid original path.";
      return result;
    }
    metadata.original_path = std::filesystem::path(path);
  }

  std::string snapshot_path;
  if (!read_expected_key(input, "snapshot_path", result.error) ||
      !(input >> std::quoted(snapshot_path))) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid snapshot path.";
    }
    return result;
  }
  metadata.snapshot_path = snapshot_path;

  std::string format;
  if (!read_expected_key(input, "source_format", result.error) ||
      !(input >> format)) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid source format.";
    }
    return result;
  }
  metadata.source_format = compatibility::document_format_from_key(format);
  if (metadata.source_format == compatibility::DocumentFormat::Unknown &&
      format != "unknown") {
    result.error = "Recovery metadata names an unsupported source format.";
    return result;
  }

  std::int64_t created_at = 0;
  if (!read_expected_key(input, "created_at_ms", result.error) ||
      !(input >> created_at)) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid creation time.";
    }
    return result;
  }
  metadata.created_at = from_epoch_milliseconds(created_at);

  int has_explicit_save = 0;
  if (!read_expected_key(input, "last_explicit_save_ms", result.error) ||
      !(input >> has_explicit_save) ||
      (has_explicit_save != 0 && has_explicit_save != 1)) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid explicit-save marker.";
    }
    return result;
  }
  if (has_explicit_save == 1) {
    std::int64_t explicit_save = 0;
    if (!(input >> explicit_save)) {
      result.error = "Recovery metadata has an invalid explicit-save time.";
      return result;
    }
    metadata.last_explicit_save = from_epoch_milliseconds(explicit_save);
  }

  if (!read_expected_key(input, "sequence", result.error) ||
      !(input >> metadata.sequence) ||
      !read_expected_key(input, "byte_size", result.error) ||
      !(input >> metadata.byte_size) ||
      !read_expected_key(input, "content_digest", result.error) ||
      !(input >> std::quoted(metadata.content_digest))) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has invalid size or digest information.";
    }
    return result;
  }

  int validated = 0;
  if (!read_expected_key(input, "validated", result.error) ||
      !(input >> validated) || (validated != 0 && validated != 1)) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid validation marker.";
    }
    return result;
  }
  metadata.validated = validated == 1;

  std::string reason;
  if (!read_expected_key(input, "reason", result.error) || !(input >> reason)) {
    if (result.error.empty()) {
      result.error = "Recovery metadata has an invalid recovery reason.";
    }
    return result;
  }
  const auto parsed_reason = parse_reason(reason);
  if (!parsed_reason.has_value()) {
    result.error = "Recovery metadata names an unsupported recovery reason.";
    return result;
  }
  metadata.reason = *parsed_reason;

  std::string trailing;
  if (input >> trailing) {
    result.error = "Recovery metadata contains unexpected trailing fields.";
    return result;
  }
  if (metadata.document_id.empty() || metadata.snapshot_path.empty()) {
    result.error = "Recovery metadata is missing its document ID or snapshot path.";
    return result;
  }
  if (metadata.document_id.size() > kMaximumIdentifierBytes ||
      metadata.snapshot_path.string().size() > kMaximumPathBytes ||
      (metadata.original_path.has_value() &&
       metadata.original_path->string().size() > kMaximumPathBytes) ||
      metadata.content_digest.size() > kMaximumDigestBytes) {
    result.error = "Recovery metadata contains an unreasonably large field.";
    return result;
  }

  result.metadata = std::move(metadata);
  return result;
}

RecoveryAdvice advise_recovery(const RecoverySnapshotMetadata& metadata,
                               const RecoveryProbe& probe) {
  RecoveryAdvice advice;
  if (!probe.snapshot_exists || !metadata.validated ||
      metadata.snapshot_path.empty()) {
    advice.disposition = RecoveryDisposition::InvalidSnapshot;
    advice.headline = "Recovery copy is unavailable";
    advice.explanation =
        "The recovery snapshot is missing or did not pass validation. Write "
        "will not offer damaged content as a safe replacement.";
    if (probe.original_exists) {
      advice.choices.push_back(recovery_choice(
          RecoveryChoiceKind::OpenOriginal, "Open Original",
          "Open the last explicitly saved document.", true));
    }
    advice.choices.push_back(recovery_choice(
        RecoveryChoiceKind::DiscardRecovery, "Discard Recovery",
        "Remove this unusable recovery record."));
    return advice;
  }

  RecoveryClock::time_point comparison{};
  bool has_comparison = false;
  if (probe.original_exists && probe.original_modified_at.has_value()) {
    comparison = *probe.original_modified_at;
    has_comparison = true;
  }
  if (metadata.last_explicit_save.has_value() &&
      (!has_comparison || *metadata.last_explicit_save > comparison)) {
    comparison = *metadata.last_explicit_save;
    has_comparison = true;
  }

  if (has_comparison && metadata.created_at <= comparison) {
    advice.disposition = RecoveryDisposition::StaleSnapshot;
    advice.headline = "Saved document is newer";
    advice.explanation =
        "This recovery copy is not newer than the last explicit save, so opening "
        "it could restore older work.";
    if (probe.original_exists) {
      advice.choices.push_back(recovery_choice(
          RecoveryChoiceKind::OpenOriginal, "Open Original",
          "Open the newer explicitly saved document.", true));
    }
    advice.choices.push_back(recovery_choice(
        RecoveryChoiceKind::SaveRecoveredAs, "Save Recovered As",
        "Keep the older snapshot as a separate file for inspection."));
    advice.choices.push_back(recovery_choice(
        RecoveryChoiceKind::DiscardRecovery, "Discard Recovery",
        "Remove the older recovery snapshot."));
    return advice;
  }

  advice.disposition = RecoveryDisposition::OfferRecovery;
  advice.headline = probe.original_exists ? "Newer recovered work is available"
                                          : "Recovered work is available";
  advice.explanation = probe.original_exists
                           ? "The recovery snapshot is newer than the last explicit "
                             "save. The original file will remain unchanged until "
                             "you choose to save."
                           : "The original file is missing, but a validated recovery "
                             "snapshot is available.";
  advice.choices.push_back(recovery_choice(
      RecoveryChoiceKind::OpenRecovered, "Open Recovered",
      "Open the newer snapshot as a modified document.", true));
  if (probe.original_exists) {
    advice.choices.push_back(recovery_choice(
        RecoveryChoiceKind::OpenOriginal, "Open Original",
        "Ignore the snapshot for now and open the last explicit save."));
  }
  advice.choices.push_back(recovery_choice(
      RecoveryChoiceKind::SaveRecoveredAs, "Save Recovered As",
      "Write the recovered content to a new path without replacing the original."));
  advice.choices.push_back(recovery_choice(
      RecoveryChoiceKind::DiscardRecovery, "Discard Recovery",
      "Permanently remove this recovery snapshot."));
  return advice;
}

}  // namespace luma::write::persistence
