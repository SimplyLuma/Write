#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace luma::write::persistence {

enum class SaveStage {
  InspectTarget,
  OpenParentDirectory,
  CreateTemporary,
  WriteTemporary,
  ApplyPermissions,
  FlushTemporary,
  ValidateTemporary,
  ReplaceTarget,
  FlushParentDirectory,
  CleanupTemporary,
};

enum class SaveOutcome {
  Saved,
  FailedOriginalUnchanged,
  SavedDurabilityUncertain,
};

// Opaque-to-callers POSIX metadata captured immediately before an overwrite
// confirmation. Supplying it back in AtomicSaveOptions prevents a destination
// changed while the prompt was open from becoming the saver's new baseline.
struct TargetFingerprint {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t link_count = 0;
  std::uint64_t mode = 0;
  std::uint64_t owner = 0;
  std::uint64_t group = 0;
  std::uint64_t size = 0;
  std::int64_t modification_seconds = 0;
  std::int64_t modification_nanoseconds = 0;
  std::int64_t change_seconds = 0;
  std::int64_t change_nanoseconds = 0;

  bool operator==(const TargetFingerprint&) const = default;
};

struct AtomicSaveOptions {
  bool preserve_existing_permissions = true;
  bool reject_symlink_parent = true;
  bool retain_recovery_on_replace_failure = true;
  // Set after the caller has presented a "new destination" overwrite decision.
  // The save then fails if the name already exists at initial inspection or
  // appears before commit. The default continues to allow replacing a target
  // that already exists when AtomicFileSaver starts.
  bool require_target_absent = false;
  // Capture before showing overwrite confirmation, then supply the same value
  // for the confirmed save. A missing or changed destination is refused.
  std::optional<TargetFingerprint> expected_existing_target;
  // Applied to a newly created target after the temporary file was initially
  // created as owner-only. Incomplete data is never exposed with this final mode.
  std::uint32_t new_file_permissions = 0644U;
};

struct SaveResult {
  SaveOutcome outcome = SaveOutcome::FailedOriginalUnchanged;
  SaveStage stage = SaveStage::InspectTarget;
  std::error_code error;
  std::string message;
  std::filesystem::path target_path;
  std::optional<std::filesystem::path> recovery_path;
  bool target_replaced = false;

  [[nodiscard]] bool saved() const noexcept {
    return outcome == SaveOutcome::Saved ||
           outcome == SaveOutcome::SavedDurabilityUncertain;
  }
};

// The callback must write to the supplied descriptor without closing it. It can
// stream arbitrarily large output and should put a useful explanation in `error`
// when returning false.
using FileWriter = std::function<bool(int descriptor, std::string& error)>;
// Validation reads the same temporary inode that will be renamed. The descriptor
// is rewound before this callback. The callback must not close or modify it.
using FileValidator = std::function<bool(int descriptor, std::string& error)>;
// Some document engines can only serialize to a filesystem pathname. This
// variant receives a pre-reserved, owner-only sibling path. The saver reopens
// and verifies that path without following links before validation/commit. A
// writer may replace the reserved inode with its output. The validator must
// only read the supplied path; changing or replacing it fails the save.
using PathWriter = std::function<bool(const std::filesystem::path& path,
                                      std::string& error)>;
using PathValidator = std::function<bool(const std::filesystem::path& path,
                                         std::string& error)>;
// Returning an error injects a failure immediately before the named stage. This
// keeps error paths deterministic in tests without test-only filesystem tricks.
using SaveFailureInjector =
    std::function<std::optional<std::error_code>(SaveStage stage)>;

class AtomicFileSaver {
 public:
  [[nodiscard]] static std::optional<TargetFingerprint>
  capture_target_fingerprint(const std::filesystem::path& target,
                             std::error_code& error,
                             bool reject_symlink_parent = true);

  [[nodiscard]] static SaveResult save(
      const std::filesystem::path& target, const FileWriter& writer,
      const FileValidator& validator = {},
      const AtomicSaveOptions& options = {},
      const SaveFailureInjector& failure_injector = {});

  [[nodiscard]] static SaveResult save_bytes(
      const std::filesystem::path& target, std::span<const std::byte> bytes,
      const FileValidator& validator = {},
      const AtomicSaveOptions& options = {},
      const SaveFailureInjector& failure_injector = {});

  [[nodiscard]] static SaveResult save_via_path(
      const std::filesystem::path& target, const PathWriter& writer,
      const PathValidator& validator = {},
      const AtomicSaveOptions& options = {},
      const SaveFailureInjector& failure_injector = {});

  [[nodiscard]] static bool write_all(int descriptor,
                                      std::span<const std::byte> bytes,
                                      std::error_code& error) noexcept;
};

[[nodiscard]] std::string_view save_stage_name(SaveStage stage) noexcept;

}  // namespace luma::write::persistence
