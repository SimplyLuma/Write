#pragma once

#include "../compatibility/format_policy.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luma::write::persistence {

using RecoveryClock = std::chrono::system_clock;

struct AutosavePolicy {
  bool enabled = true;
  std::chrono::seconds interval = std::chrono::minutes(2);
};

[[nodiscard]] bool autosave_due(
    const AutosavePolicy& policy, bool document_modified,
    std::optional<RecoveryClock::time_point> last_autosave,
    RecoveryClock::time_point now) noexcept;

enum class RecoveryReason {
  PeriodicAutosave,
  BeforeRiskyOperation,
  EngineFailure,
  UncleanShutdown,
};

struct RecoverySnapshotMetadata {
  std::uint32_t schema_version = 1;
  std::string document_id;
  std::optional<std::filesystem::path> original_path;
  std::filesystem::path snapshot_path;
  compatibility::DocumentFormat source_format =
      compatibility::DocumentFormat::Docx;
  RecoveryClock::time_point created_at{};
  std::optional<RecoveryClock::time_point> last_explicit_save;
  std::uint64_t sequence = 0;
  std::uintmax_t byte_size = 0;
  std::string content_digest;
  bool validated = false;
  RecoveryReason reason = RecoveryReason::PeriodicAutosave;
};

struct MetadataParseResult {
  std::optional<RecoverySnapshotMetadata> metadata;
  std::string error;
};

[[nodiscard]] std::string serialize_recovery_metadata(
    const RecoverySnapshotMetadata& metadata);
[[nodiscard]] MetadataParseResult parse_recovery_metadata(std::string_view text);

struct RecoveryProbe {
  bool snapshot_exists = true;
  bool original_exists = false;
  std::optional<RecoveryClock::time_point> original_modified_at;
};

enum class RecoveryDisposition {
  OfferRecovery,
  StaleSnapshot,
  InvalidSnapshot,
};

enum class RecoveryChoiceKind {
  OpenRecovered,
  OpenOriginal,
  SaveRecoveredAs,
  DiscardRecovery,
};

struct RecoveryChoice {
  RecoveryChoiceKind kind = RecoveryChoiceKind::DiscardRecovery;
  std::string label;
  std::string explanation;
  bool recommended = false;
};

struct RecoveryAdvice {
  RecoveryDisposition disposition = RecoveryDisposition::InvalidSnapshot;
  std::string headline;
  std::string explanation;
  std::vector<RecoveryChoice> choices;
};

[[nodiscard]] RecoveryAdvice advise_recovery(
    const RecoverySnapshotMetadata& metadata, const RecoveryProbe& probe);

}  // namespace luma::write::persistence
