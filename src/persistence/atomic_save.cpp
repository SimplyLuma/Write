#include "atomic_save.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <unistd.h>

namespace luma::write::persistence {
namespace {

std::atomic<std::uint64_t> g_temporary_counter{0};

class FileDescriptor {
 public:
  explicit FileDescriptor(const int value = -1) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      ::close(value_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        ::close(value_);
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  int value_ = -1;
};

std::error_code errno_code() { return {errno, std::generic_category()}; }

[[nodiscard]] bool same_inode(const struct stat& left,
                              const struct stat& right) noexcept {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

[[nodiscard]] const struct timespec& modification_time(
    const struct stat& status) noexcept {
#if defined(__APPLE__)
  return status.st_mtimespec;
#else
  return status.st_mtim;
#endif
}

[[nodiscard]] const struct timespec& change_time(
    const struct stat& status) noexcept {
#if defined(__APPLE__)
  return status.st_ctimespec;
#else
  return status.st_ctim;
#endif
}

[[nodiscard]] TargetFingerprint fingerprint_for(
    const struct stat& status) noexcept {
  const struct timespec& modified = modification_time(status);
  const struct timespec& changed = change_time(status);
  return {
      .device = static_cast<std::uint64_t>(status.st_dev),
      .inode = static_cast<std::uint64_t>(status.st_ino),
      .link_count = static_cast<std::uint64_t>(status.st_nlink),
      .mode = static_cast<std::uint64_t>(status.st_mode),
      .owner = static_cast<std::uint64_t>(status.st_uid),
      .group = static_cast<std::uint64_t>(status.st_gid),
      .size = static_cast<std::uint64_t>(status.st_size),
      .modification_seconds = static_cast<std::int64_t>(modified.tv_sec),
      .modification_nanoseconds = static_cast<std::int64_t>(modified.tv_nsec),
      .change_seconds = static_cast<std::int64_t>(changed.tv_sec),
      .change_nanoseconds = static_cast<std::int64_t>(changed.tv_nsec),
  };
}

// Identity alone does not detect another editor truncating and rewriting the
// destination in place. Include every stable metadata field that reflects
// content, ownership, permissions, or link changes; access time is deliberately
// excluded because validation/readers may update it.
[[nodiscard]] bool same_file_snapshot(const struct stat& left,
                                      const struct stat& right) noexcept {
  return fingerprint_for(left) == fingerprint_for(right);
}

[[nodiscard]] int directory_flags(const bool reject_symlink) noexcept {
  int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#ifdef O_NOFOLLOW
  if (reject_symlink) {
    flags |= O_NOFOLLOW;
  }
#else
  if (reject_symlink) {
    errno = ENOTSUP;
    return -1;
  }
#endif
  return flags;
}

[[nodiscard]] FileDescriptor open_parent_directory(
    const std::filesystem::path& parent, const bool reject_symlinks,
    std::error_code& error) {
  const int flags = directory_flags(reject_symlinks);
  if (flags < 0) {
    error = errno_code();
    return FileDescriptor{};
  }
  FileDescriptor result(::open(parent.c_str(), flags));
  if (result.get() < 0) {
    error = errno_code();
    return FileDescriptor{};
  }
  return result;
}

[[nodiscard]] bool directory_path_still_names(
    const std::filesystem::path& parent, const bool reject_symlinks,
    const struct stat& expected, std::error_code& error) {
  FileDescriptor current =
      open_parent_directory(parent, reject_symlinks, error);
  if (current.get() < 0) {
    return false;
  }
  struct stat status {};
  if (::fstat(current.get(), &status) != 0) {
    error = errno_code();
    return false;
  }
  if (!S_ISDIR(status.st_mode) || !same_inode(status, expected)) {
    error = std::make_error_code(std::errc::state_not_recoverable);
    return false;
  }
  return true;
}

[[nodiscard]] int rename_without_replacing(
    const int directory, const char* source, const char* destination) noexcept {
#if defined(__APPLE__)
  return ::renameatx_np(directory, source, directory, destination, RENAME_EXCL);
#elif defined(__linux__)
  return static_cast<int>(
      ::syscall(SYS_renameat2, directory, source, directory, destination,
                RENAME_NOREPLACE));
#else
  (void)directory;
  (void)source;
  (void)destination;
  errno = ENOTSUP;
  return -1;
#endif
}

std::optional<std::error_code> injected(
    const SaveFailureInjector& injector, const SaveStage stage) {
  if (!injector) {
    return std::nullopt;
  }
  try {
    return injector(stage);
  } catch (...) {
    return std::make_error_code(std::errc::operation_canceled);
  }
}

SaveResult failure(const std::filesystem::path& target, const SaveStage stage,
                   std::error_code error, std::string message) {
  return {
      .outcome = SaveOutcome::FailedOriginalUnchanged,
      .stage = stage,
      .error = std::move(error),
      .message = std::move(message),
      .target_path = target,
  };
}

std::string temporary_name() {
  const auto sequence = g_temporary_counter.fetch_add(1, std::memory_order_relaxed);
  return ".luma-write-save-" + std::to_string(static_cast<long long>(::getpid())) +
         "-" + std::to_string(sequence) + ".tmp";
}

bool is_valid_filename(const std::filesystem::path& filename) {
  return !filename.empty() && filename != "." && filename != ".." &&
         filename.string().find('/') == std::string::npos;
}

}  // namespace

std::string_view save_stage_name(const SaveStage stage) noexcept {
  switch (stage) {
    case SaveStage::InspectTarget:
      return "inspect target";
    case SaveStage::OpenParentDirectory:
      return "open parent directory";
    case SaveStage::CreateTemporary:
      return "create temporary file";
    case SaveStage::WriteTemporary:
      return "write temporary file";
    case SaveStage::ApplyPermissions:
      return "apply file permissions";
    case SaveStage::FlushTemporary:
      return "flush temporary file";
    case SaveStage::ValidateTemporary:
      return "validate temporary file";
    case SaveStage::ReplaceTarget:
      return "replace target";
    case SaveStage::FlushParentDirectory:
      return "flush parent directory";
    case SaveStage::CleanupTemporary:
      return "clean up temporary file";
  }
  return "save";
}

bool AtomicFileSaver::write_all(const int descriptor,
                                std::span<const std::byte> bytes,
                                std::error_code& error) noexcept {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const std::size_t request =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t count =
        ::write(descriptor, bytes.data() + offset, static_cast<size_t>(request));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      error = errno_code();
      return false;
    }
    if (count == 0) {
      error = std::make_error_code(std::errc::io_error);
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<TargetFingerprint> AtomicFileSaver::capture_target_fingerprint(
    const std::filesystem::path& target, std::error_code& error,
    const bool reject_symlink_parent) {
  error.clear();
  if (target.empty() || !is_valid_filename(target.filename())) {
    error = std::make_error_code(std::errc::invalid_argument);
    return std::nullopt;
  }
  const std::filesystem::path parent =
      target.has_parent_path() ? target.parent_path() : std::filesystem::path{"."};
  std::error_code directory_error;
  FileDescriptor directory =
      open_parent_directory(parent, reject_symlink_parent, directory_error);
  if (directory.get() < 0) {
    error = directory_error;
    return std::nullopt;
  }

  struct stat status {};
  const std::string target_name = target.filename().string();
  if (::fstatat(directory.get(), target_name.c_str(), &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    error = errno_code();
    return std::nullopt;
  }
  if (S_ISLNK(status.st_mode)) {
    error = std::make_error_code(std::errc::too_many_symbolic_link_levels);
    return std::nullopt;
  }
  if (!S_ISREG(status.st_mode)) {
    error = std::make_error_code(std::errc::invalid_argument);
    return std::nullopt;
  }
  return fingerprint_for(status);
}

SaveResult AtomicFileSaver::save_bytes(
    const std::filesystem::path& target, const std::span<const std::byte> bytes,
    const FileValidator& validator, const AtomicSaveOptions& options,
    const SaveFailureInjector& failure_injector) {
  return save(
      target,
      [bytes](const int descriptor, std::string& error_text) {
        std::error_code error;
        if (!write_all(descriptor, bytes, error)) {
          error_text = error.message();
          return false;
        }
        return true;
      },
      validator, options, failure_injector);
}

SaveResult save_impl(
    const std::filesystem::path& target,
    const std::function<bool(int, const std::filesystem::path&,
                             std::string&)>& writer,
    const std::function<bool(int, const std::filesystem::path&,
                             std::string&)>& validator,
    const bool reopen_output_by_path, const AtomicSaveOptions& options,
    const SaveFailureInjector& failure_injector) {
  if (const auto error = injected(failure_injector, SaveStage::InspectTarget)) {
    return failure(target, SaveStage::InspectTarget, *error,
                   "Could not inspect the destination: " + error->message());
  }
  if (target.empty() || !is_valid_filename(target.filename()) || !writer) {
    return failure(target, SaveStage::InspectTarget,
                   std::make_error_code(std::errc::invalid_argument),
                   "The save destination or writer is invalid.");
  }
  if (options.require_target_absent &&
      options.expected_existing_target.has_value()) {
    return failure(
        target, SaveStage::InspectTarget,
        std::make_error_code(std::errc::invalid_argument),
        "The save cannot simultaneously require a missing destination and an "
        "existing destination fingerprint.");
  }

  const std::filesystem::path parent =
      target.has_parent_path() ? target.parent_path() : std::filesystem::path{"."};
  const std::string target_name = target.filename().string();

  if (const auto error =
          injected(failure_injector, SaveStage::OpenParentDirectory)) {
    return failure(target, SaveStage::OpenParentDirectory, *error,
                   "Could not open the destination folder: " + error->message());
  }

  std::error_code directory_error;
  FileDescriptor directory = open_parent_directory(
      parent, options.reject_symlink_parent, directory_error);
  if (directory.get() < 0) {
    return failure(target, SaveStage::OpenParentDirectory, directory_error,
                   "Could not safely open the destination folder.");
  }
  struct stat directory_status {};
  if (::fstat(directory.get(), &directory_status) != 0) {
    return failure(target, SaveStage::OpenParentDirectory, errno_code(),
                   "Could not safely inspect the destination folder.");
  }
  if (!S_ISDIR(directory_status.st_mode)) {
    return failure(target, SaveStage::OpenParentDirectory,
                   std::make_error_code(std::errc::not_a_directory),
                   "The save destination parent is not a directory.");
  }

  struct stat existing_status {};
  bool target_exists = false;
  if (::fstatat(directory.get(), target_name.c_str(), &existing_status,
                AT_SYMLINK_NOFOLLOW) == 0) {
    target_exists = true;
    if (S_ISLNK(existing_status.st_mode)) {
      return failure(target, SaveStage::InspectTarget,
                     std::make_error_code(std::errc::too_many_symbolic_link_levels),
                     "Refusing to replace a symbolic link. Choose the real file "
                     "explicitly so another file is not overwritten by mistake.");
    }
    if (!S_ISREG(existing_status.st_mode)) {
      return failure(target, SaveStage::InspectTarget,
                     std::make_error_code(std::errc::invalid_argument),
                     "The save destination is not a regular file.");
    }
    if (options.require_target_absent) {
      return failure(
          target, SaveStage::InspectTarget,
          std::make_error_code(std::errc::file_exists),
          "A file now exists at the destination. It was left unchanged so the "
          "caller can request overwrite confirmation for that exact path.");
    }
    if (options.expected_existing_target.has_value() &&
        fingerprint_for(existing_status) !=
            *options.expected_existing_target) {
      return failure(
          target, SaveStage::InspectTarget,
          std::make_error_code(std::errc::state_not_recoverable),
          "The destination changed after overwrite confirmation. It was left "
          "unchanged so the caller can show the updated file before asking again.");
    }
  } else if (errno != ENOENT) {
    return failure(target, SaveStage::InspectTarget, errno_code(),
                   "Could not inspect the existing destination.");
  } else if (options.expected_existing_target.has_value()) {
    return failure(
        target, SaveStage::InspectTarget,
        std::make_error_code(std::errc::no_such_file_or_directory),
        "The confirmed overwrite destination disappeared before saving. No new "
        "file was installed without a fresh destination decision.");
  }

  if (const auto error = injected(failure_injector, SaveStage::CreateTemporary)) {
    return failure(target, SaveStage::CreateTemporary, *error,
                   "Could not create a temporary save file: " + error->message());
  }

  std::string temp_name;
  FileDescriptor temporary;
  constexpr int kMaximumAttempts = 128;
  for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    temp_name = temporary_name();
    const int descriptor =
        ::openat(directory.get(), temp_name.c_str(),
                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC
#ifdef O_NOFOLLOW
                     | O_NOFOLLOW
#endif
                 ,
                 S_IRUSR | S_IWUSR);
    if (descriptor >= 0) {
      temporary = FileDescriptor(descriptor);
      break;
    }
    if (errno != EEXIST) {
      return failure(target, SaveStage::CreateTemporary, errno_code(),
                     "Could not create a sibling temporary save file.");
    }
  }
  if (temporary.get() < 0) {
    return failure(target, SaveStage::CreateTemporary,
                   std::make_error_code(std::errc::file_exists),
                   "Could not reserve a unique sibling temporary save file.");
  }

  const std::filesystem::path temp_path = parent / temp_name;
  bool keep_temporary = false;
  const auto clean_temporary = [&]() -> std::error_code {
    if (keep_temporary || temp_name.empty()) {
      return {};
    }
    if (const auto error =
            injected(failure_injector, SaveStage::CleanupTemporary)) {
      return *error;
    }

    // A path-only writer must never be able to move the last link to the
    // original destination onto the stage name and then have failure cleanup
    // unlink it. Restore that inode when the destination is vacant; otherwise
    // retain it as a discoverable recovery artifact.
    struct stat staged_status {};
    if (::fstatat(directory.get(), temp_name.c_str(), &staged_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        return {};
      }
      return errno_code();
    }
    if (target_exists && same_inode(staged_status, existing_status)) {
      struct stat current_target {};
      if (::fstatat(directory.get(), target_name.c_str(), &current_target,
                    AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
          return errno_code();
        }
        if (rename_without_replacing(directory.get(), temp_name.c_str(),
                                     target_name.c_str()) == 0) {
          return {};
        }
        return errno_code();
      }
      if (!same_inode(current_target, existing_status)) {
        return std::make_error_code(std::errc::device_or_resource_busy);
      }
    }
    if (::unlinkat(directory.get(), temp_name.c_str(), 0) != 0 && errno != ENOENT) {
      return errno_code();
    }
    return {};
  };
  const auto fail_before_replace = [&](const SaveStage stage,
                                       std::error_code error,
                                       std::string message) {
    const std::error_code cleanup_error = clean_temporary();
    SaveResult result =
        failure(target, stage, std::move(error), std::move(message));
    if (cleanup_error) {
      result.message += " Temporary-file cleanup also failed: " +
                        cleanup_error.message();
      result.recovery_path = temp_path;
    }
    return result;
  };

  if (const auto error = injected(failure_injector, SaveStage::WriteTemporary)) {
    return fail_before_replace(
        SaveStage::WriteTemporary, *error,
        "Could not write the temporary save file: " + error->message());
  }
  try {
    std::string writer_error;
    if (!writer(temporary.get(), temp_path, writer_error)) {
      if (writer_error.empty()) {
        writer_error = "The document writer did not provide more information.";
      }
      return fail_before_replace(
          SaveStage::WriteTemporary, std::make_error_code(std::errc::io_error),
          "The document could not be written. " + writer_error);
    }
  } catch (const std::exception& exception) {
    return fail_before_replace(
        SaveStage::WriteTemporary, std::make_error_code(std::errc::io_error),
        std::string("The document writer failed: ") + exception.what());
  } catch (...) {
    return fail_before_replace(
        SaveStage::WriteTemporary, std::make_error_code(std::errc::io_error),
        "The document writer failed unexpectedly.");
  }

  if (reopen_output_by_path) {
    std::error_code parent_error;
    if (!directory_path_still_names(parent, options.reject_symlink_parent,
                                    directory_status, parent_error)) {
      return fail_before_replace(
          SaveStage::WriteTemporary, parent_error,
          "The destination folder changed while the document engine was "
          "writing. No staged output was installed.");
    }
    const int reopened =
        ::openat(directory.get(), temp_name.c_str(), O_RDWR | O_CLOEXEC
#ifdef O_NOFOLLOW
                                                     | O_NOFOLLOW
#endif
        );
    if (reopened < 0) {
      return fail_before_replace(
          SaveStage::WriteTemporary, errno_code(),
          "The document engine did not leave a safe sibling output file.");
    }
    temporary = FileDescriptor(reopened);
  }

  struct stat temporary_status {};
  if (::fstat(temporary.get(), &temporary_status) != 0) {
    return fail_before_replace(
        SaveStage::WriteTemporary, errno_code(),
        "The temporary output could not be inspected safely.");
  }
  if (!S_ISREG(temporary_status.st_mode) || temporary_status.st_nlink != 1) {
    return fail_before_replace(
        SaveStage::WriteTemporary,
        std::make_error_code(std::errc::invalid_argument),
        "The temporary output is no longer a safe regular file.");
  }
  if (target_exists && same_inode(temporary_status, existing_status)) {
    struct stat current_target {};
    if (::fstatat(directory.get(), target_name.c_str(), &current_target,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_inode(current_target, existing_status)) {
      return fail_before_replace(
          SaveStage::WriteTemporary,
          std::make_error_code(std::errc::state_not_recoverable),
          "The document writer displaced the original destination instead of "
          "producing an independent staged file.");
    }
  }

  if (const auto error = injected(failure_injector, SaveStage::ApplyPermissions)) {
    return fail_before_replace(
        SaveStage::ApplyPermissions, *error,
        "Could not preserve file permissions: " + error->message());
  }
  const mode_t final_mode = static_cast<mode_t>(
      target_exists && options.preserve_existing_permissions
          ? existing_status.st_mode & static_cast<mode_t>(0777)
          : options.new_file_permissions & 0777U);
  if (::fchmod(temporary.get(), final_mode) != 0) {
    return fail_before_replace(SaveStage::ApplyPermissions, errno_code(),
                               "Could not apply destination file permissions.");
  }

  if (const auto error = injected(failure_injector, SaveStage::FlushTemporary)) {
    return fail_before_replace(
        SaveStage::FlushTemporary, *error,
        "Could not flush the temporary save file: " + error->message());
  }
  if (::fsync(temporary.get()) != 0) {
    return fail_before_replace(SaveStage::FlushTemporary, errno_code(),
                               "Could not flush the temporary save file to disk.");
  }

  struct stat validated_snapshot {};
  if (::fstat(temporary.get(), &validated_snapshot) != 0) {
    return fail_before_replace(
        SaveStage::ValidateTemporary, errno_code(),
        "Could not inspect the flushed temporary output.");
  }
  const auto staged_entry_matches_snapshot = [&]() -> std::error_code {
    struct stat descriptor_status {};
    if (::fstat(temporary.get(), &descriptor_status) != 0) {
      return errno_code();
    }
    struct stat entry_status {};
    if (::fstatat(directory.get(), temp_name.c_str(), &entry_status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      return errno_code();
    }
    if (!S_ISREG(entry_status.st_mode) || entry_status.st_nlink != 1 ||
        !same_file_snapshot(descriptor_status, validated_snapshot) ||
        !same_file_snapshot(entry_status, validated_snapshot)) {
      return std::make_error_code(std::errc::state_not_recoverable);
    }
    return {};
  };

  if (const auto error = injected(failure_injector, SaveStage::ValidateTemporary)) {
    return fail_before_replace(
        SaveStage::ValidateTemporary, *error,
        "Could not validate the saved document: " + error->message());
  }
  if (validator) {
    if (reopen_output_by_path) {
      std::error_code parent_error;
      if (!directory_path_still_names(parent, options.reject_symlink_parent,
                                      directory_status, parent_error)) {
        return fail_before_replace(
            SaveStage::ValidateTemporary, parent_error,
            "The destination folder changed before validation. No staged "
            "output was installed.");
      }
    }
    if (::lseek(temporary.get(), 0, SEEK_SET) < 0) {
      return fail_before_replace(SaveStage::ValidateTemporary, errno_code(),
                                 "Could not rewind the saved document for validation.");
    }
    try {
      std::string validator_error;
      if (!validator(temporary.get(), temp_path, validator_error)) {
        if (validator_error.empty()) {
          validator_error = "The output could not be reopened.";
        }
        return fail_before_replace(
            SaveStage::ValidateTemporary,
            std::make_error_code(std::errc::illegal_byte_sequence),
            "The new file failed validation. The previous file is unchanged. " +
                validator_error);
      }
    } catch (const std::exception& exception) {
      return fail_before_replace(
          SaveStage::ValidateTemporary,
          std::make_error_code(std::errc::illegal_byte_sequence),
          std::string("Saved-document validation failed: ") + exception.what());
    } catch (...) {
      return fail_before_replace(
          SaveStage::ValidateTemporary,
          std::make_error_code(std::errc::illegal_byte_sequence),
          "Saved-document validation failed unexpectedly.");
    }
  }

  if (reopen_output_by_path) {
    std::error_code parent_error;
    if (!directory_path_still_names(parent, options.reject_symlink_parent,
                                    directory_status, parent_error)) {
      return fail_before_replace(
          SaveStage::ValidateTemporary, parent_error,
          "The destination folder changed during validation. No staged output "
          "was installed.");
    }
  }
  if (const std::error_code stage_error = staged_entry_matches_snapshot()) {
    return fail_before_replace(
        SaveStage::ValidateTemporary, stage_error,
        "The validated staging path or its contents changed before commit. The "
        "previous destination was left unchanged.");
  }

  const auto replacement_failure = [&](std::error_code error) {
    keep_temporary = options.retain_recovery_on_replace_failure;
    SaveResult result = failure(
        target, SaveStage::ReplaceTarget, std::move(error),
        "The validated file could not replace the destination. The previous file "
        "is unchanged.");
    if (keep_temporary) {
      result.recovery_path = temp_path;
    } else if (const std::error_code cleanup_error = clean_temporary()) {
      result.message += " Temporary-file cleanup also failed: " +
                        cleanup_error.message();
      result.recovery_path = temp_path;
    }
    return result;
  };

  // The injector runs before the final snapshots so a deterministic test (or a
  // future pre-commit hook) cannot mutate either path after it was checked.
  if (const auto error = injected(failure_injector, SaveStage::ReplaceTarget)) {
    return replacement_failure(*error);
  }
  if (reopen_output_by_path) {
    std::error_code parent_error;
    if (!directory_path_still_names(parent, options.reject_symlink_parent,
                                    directory_status, parent_error)) {
      return fail_before_replace(
          SaveStage::ReplaceTarget, parent_error,
          "The destination folder changed before commit. No staged output was "
          "installed.");
    }
  }
  if (const std::error_code stage_error = staged_entry_matches_snapshot()) {
    return fail_before_replace(
        SaveStage::ReplaceTarget, stage_error,
        "The staging path changed immediately before commit. The previous "
        "destination was left unchanged.");
  }

  // Do not replace a destination that changed while the document engine was
  // writing or validation was running. Identity alone is insufficient: another
  // editor can rewrite the same inode, so compare its complete stable snapshot.
  struct stat current_target_status {};
  const int target_check = ::fstatat(directory.get(), target_name.c_str(),
                                     &current_target_status,
                                     AT_SYMLINK_NOFOLLOW);
  const std::error_code target_check_error =
      target_check == 0 ? std::error_code{} : errno_code();
  if (target_exists) {
    if (target_check != 0 ||
        !same_file_snapshot(current_target_status, existing_status)) {
      return fail_before_replace(
          SaveStage::ReplaceTarget,
          std::make_error_code(std::errc::state_not_recoverable),
          "The destination changed while saving. The validated output was not "
          "installed; review the other file before trying again.");
    }
  } else if (target_check == 0 || target_check_error.value() != ENOENT) {
    return fail_before_replace(
        SaveStage::ReplaceTarget,
        std::make_error_code(std::errc::file_exists),
        "A file appeared at the destination while saving. It was left unchanged.");
  }

  const int rename_result =
      target_exists
          ? ::renameat(directory.get(), temp_name.c_str(), directory.get(),
                       target_name.c_str())
          : rename_without_replacing(directory.get(), temp_name.c_str(),
                                     target_name.c_str());
  if (rename_result != 0) {
    return replacement_failure(errno_code());
  }

  if (const auto error =
          injected(failure_injector, SaveStage::FlushParentDirectory)) {
    return {
        .outcome = SaveOutcome::SavedDurabilityUncertain,
        .stage = SaveStage::FlushParentDirectory,
        .error = *error,
        .message =
            "The document was replaced, but the folder update could not be flushed. "
            "The new file is readable, though a sudden power loss could still lose "
            "the rename.",
        .target_path = target,
        .target_replaced = true,
    };
  }
  if (::fsync(directory.get()) != 0) {
    return {
        .outcome = SaveOutcome::SavedDurabilityUncertain,
        .stage = SaveStage::FlushParentDirectory,
        .error = errno_code(),
        .message =
            "The document was replaced, but the folder update could not be flushed. "
            "The new file is readable, though a sudden power loss could still lose "
            "the rename.",
        .target_path = target,
        .target_replaced = true,
    };
  }

  return {
      .outcome = SaveOutcome::Saved,
      .stage = SaveStage::FlushParentDirectory,
      .message = "The document was saved and validated.",
      .target_path = target,
      .target_replaced = true,
  };
}

SaveResult AtomicFileSaver::save(
    const std::filesystem::path& target, const FileWriter& writer,
    const FileValidator& validator, const AtomicSaveOptions& options,
    const SaveFailureInjector& failure_injector) {
  std::function<bool(int, const std::filesystem::path&, std::string&)>
      internal_writer;
  if (writer) {
    internal_writer = [writer](const int descriptor,
                               const std::filesystem::path&,
                               std::string& error) {
      return writer(descriptor, error);
    };
  }
  return save_impl(
      target, internal_writer,
      validator
          ? std::function<bool(int, const std::filesystem::path&, std::string&)>{
                [validator](const int descriptor,
                            const std::filesystem::path&, std::string& error) {
                  return validator(descriptor, error);
                }}
          : std::function<bool(int, const std::filesystem::path&, std::string&)>{},
      false, options, failure_injector);
}

SaveResult AtomicFileSaver::save_via_path(
    const std::filesystem::path& target, const PathWriter& writer,
    const PathValidator& validator, const AtomicSaveOptions& options,
    const SaveFailureInjector& failure_injector) {
  std::function<bool(int, const std::filesystem::path&, std::string&)>
      internal_writer;
  if (writer) {
    internal_writer = [writer](int, const std::filesystem::path& path,
                               std::string& error) {
      return writer(path, error);
    };
  }
  return save_impl(
      target, internal_writer,
      validator
          ? std::function<bool(int, const std::filesystem::path&, std::string&)>{
                [validator](int, const std::filesystem::path& path,
                            std::string& error) {
                  return validator(path, error);
                }}
          : std::function<bool(int, const std::filesystem::path&, std::string&)>{},
      true, options, failure_injector);
}

}  // namespace luma::write::persistence
