#include "persistence/recovery.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

namespace compatibility = luma::write::compatibility;
namespace persistence = luma::write::persistence;

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

using namespace std::chrono_literals;

bool has_choice(const persistence::RecoveryAdvice& advice,
                const persistence::RecoveryChoiceKind kind,
                const bool recommended = false) {
  return std::any_of(
      advice.choices.begin(), advice.choices.end(),
      [kind, recommended](const persistence::RecoveryChoice& choice) {
        return choice.kind == kind && (!recommended || choice.recommended);
      });
}

void autosave_schedule() {
  const auto now = persistence::RecoveryClock::time_point{10min};
  const persistence::AutosavePolicy defaults;
  CHECK(persistence::autosave_due(defaults, true, std::nullopt, now));
  CHECK(!persistence::autosave_due(defaults, false, std::nullopt, now));
  CHECK(!persistence::autosave_due(defaults, true, now - 119s, now));
  CHECK(persistence::autosave_due(defaults, true, now - 120s, now));
  CHECK(!persistence::autosave_due({.enabled = false}, true, std::nullopt, now));
  CHECK(!persistence::autosave_due({.enabled = true, .interval = 0s}, true,
                                   std::nullopt, now));
}

persistence::RecoverySnapshotMetadata sample_metadata() {
  return {
      .schema_version = 1,
      .document_id = "document-42",
      .original_path = "/work/Client \"A\"/proposal.docm",
      .snapshot_path = "/recovery/document-42/snapshot 7.docx",
      .source_format = compatibility::DocumentFormat::MacroEnabledDocx,
      .created_at = persistence::RecoveryClock::time_point{5000ms},
      .last_explicit_save = persistence::RecoveryClock::time_point{3000ms},
      .sequence = 7,
      .byte_size = 8192,
      .content_digest = "sha256:0123456789abcdef",
      .validated = true,
      .reason = persistence::RecoveryReason::EngineFailure,
  };
}

void metadata_round_trip_and_rejection() {
  const auto original = sample_metadata();
  const std::string encoded = persistence::serialize_recovery_metadata(original);
  const auto parsed = persistence::parse_recovery_metadata(encoded);
  CHECK(parsed.metadata.has_value());
  CHECK(parsed.error.empty());
  if (parsed.metadata.has_value()) {
    CHECK(parsed.metadata->document_id == original.document_id);
    CHECK(parsed.metadata->original_path == original.original_path);
    CHECK(parsed.metadata->snapshot_path == original.snapshot_path);
    CHECK(parsed.metadata->source_format == original.source_format);
    CHECK(parsed.metadata->created_at == original.created_at);
    CHECK(parsed.metadata->last_explicit_save == original.last_explicit_save);
    CHECK(parsed.metadata->sequence == original.sequence);
    CHECK(parsed.metadata->byte_size == original.byte_size);
    CHECK(parsed.metadata->content_digest == original.content_digest);
    CHECK(parsed.metadata->validated == original.validated);
    CHECK(parsed.metadata->reason == original.reason);
  }

  auto damaged = persistence::parse_recovery_metadata("not a recovery file\n");
  CHECK(!damaged.metadata.has_value());
  CHECK(!damaged.error.empty());

  damaged = persistence::parse_recovery_metadata(encoded + "unexpected field\n");
  CHECK(!damaged.metadata.has_value());
  CHECK(!damaged.error.empty());

  std::string unknown_reason = encoded;
  const auto reason_position = unknown_reason.find("reason engine-failure");
  CHECK(reason_position != std::string::npos);
  if (reason_position != std::string::npos) {
    unknown_reason.replace(reason_position, std::string("reason engine-failure").size(),
                           "reason impossible");
    damaged = persistence::parse_recovery_metadata(unknown_reason);
    CHECK(!damaged.metadata.has_value());
  }
}

void recovery_advice_uses_real_timestamps() {
  auto metadata = sample_metadata();
  persistence::RecoveryProbe probe{
      .snapshot_exists = true,
      .original_exists = true,
      .original_modified_at = persistence::RecoveryClock::time_point{4000ms},
  };
  auto advice = persistence::advise_recovery(metadata, probe);
  CHECK(advice.disposition == persistence::RecoveryDisposition::OfferRecovery);
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::OpenRecovered, true));
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::OpenOriginal));
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::SaveRecoveredAs));
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::DiscardRecovery));

  probe.original_modified_at = persistence::RecoveryClock::time_point{6000ms};
  advice = persistence::advise_recovery(metadata, probe);
  CHECK(advice.disposition == persistence::RecoveryDisposition::StaleSnapshot);
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::OpenOriginal, true));
  CHECK(!has_choice(advice, persistence::RecoveryChoiceKind::OpenRecovered));

  metadata.validated = false;
  advice = persistence::advise_recovery(metadata, probe);
  CHECK(advice.disposition == persistence::RecoveryDisposition::InvalidSnapshot);
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::OpenOriginal, true));

  metadata.validated = true;
  probe.original_exists = false;
  probe.original_modified_at.reset();
  metadata.last_explicit_save.reset();
  advice = persistence::advise_recovery(metadata, probe);
  CHECK(advice.disposition == persistence::RecoveryDisposition::OfferRecovery);
  CHECK(has_choice(advice, persistence::RecoveryChoiceKind::OpenRecovered, true));
  CHECK(!has_choice(advice, persistence::RecoveryChoiceKind::OpenOriginal));
  CHECK(advice.explanation.find("missing") != std::string::npos);
}

}  // namespace

int main() {
  autosave_schedule();
  metadata_round_trip_and_rejection();
  recovery_advice_uses_real_timestamps();
  if (failures != 0) {
    std::cerr << failures << " recovery test(s) failed\n";
    return 1;
  }
  std::cout << "recovery tests passed\n";
  return 0;
}
