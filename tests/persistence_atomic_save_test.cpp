#include "persistence/atomic_save.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

class TestDirectory {
 public:
  TestDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("luma-write-atomic-save-test-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
  output.close();
  if (!output) {
    throw std::runtime_error("could not create test file");
  }
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::span<const std::byte> bytes_of(const std::string& value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}

persistence::FileValidator exact_validator(const std::string expected) {
  return [expected](const int descriptor, std::string& error) {
    std::string actual(expected.size(), '\0');
    std::size_t offset = 0;
    while (offset < actual.size()) {
      const ssize_t count =
          ::read(descriptor, actual.data() + offset, actual.size() - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        error = "saved file ended early";
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
    if (actual != expected) {
      error = "saved bytes differ";
      return false;
    }
    std::array<char, 1> extra{};
    if (::read(descriptor, extra.data(), extra.size()) != 0) {
      error = "saved file contains trailing bytes";
      return false;
    }
    return true;
  };
}

persistence::SaveFailureInjector fail_at(const persistence::SaveStage stage) {
  return [stage](const persistence::SaveStage current)
             -> std::optional<std::error_code> {
    if (current == stage) {
      return std::make_error_code(std::errc::io_error);
    }
    return std::nullopt;
  };
}

std::size_t temporary_count(const std::filesystem::path& directory) {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().filename().string().starts_with(".luma-write-save-")) {
      ++count;
    }
  }
  return count;
}

void successful_replace_is_validated_and_preserves_mode() {
  TestDirectory directory;
  const auto target = directory.path() / "report.docx";
  write_file(target, "old document");
  CHECK(::chmod(target.c_str(), 0600) == 0);
  const std::string replacement = "new validated document";

  const auto result = persistence::AtomicFileSaver::save_bytes(
      target, bytes_of(replacement), exact_validator(replacement));
  CHECK(result.outcome == persistence::SaveOutcome::Saved);
  CHECK(result.saved());
  CHECK(result.target_replaced);
  CHECK(read_file(target) == replacement);
  CHECK(!result.recovery_path.has_value());
  CHECK(temporary_count(directory.path()) == 0U);

  struct stat status {};
  CHECK(::stat(target.c_str(), &status) == 0);
  CHECK((status.st_mode & 0777) == 0600);
}

void failures_before_replace_leave_original_untouched() {
  for (const persistence::SaveStage stage :
       {persistence::SaveStage::WriteTemporary,
        persistence::SaveStage::ApplyPermissions,
        persistence::SaveStage::FlushTemporary,
        persistence::SaveStage::ValidateTemporary}) {
    TestDirectory directory;
    const auto target = directory.path() / "report.docx";
    write_file(target, "last good file");
    const std::string replacement = "uncommitted bytes";
    const auto result = persistence::AtomicFileSaver::save_bytes(
        target, bytes_of(replacement), exact_validator(replacement), {},
        fail_at(stage));
    CHECK(!result.saved());
    CHECK(!result.target_replaced);
    CHECK(result.stage == stage);
    CHECK(read_file(target) == "last good file");
    CHECK(temporary_count(directory.path()) == 0U);
  }

  TestDirectory directory;
  const auto target = directory.path() / "report.docx";
  write_file(target, "last good file");
  const std::string replacement = "invalid output";
  const auto result = persistence::AtomicFileSaver::save_bytes(
      target, bytes_of(replacement),
      [](int, std::string& error) {
        error = "fixture could not be reopened";
        return false;
      });
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::ValidateTemporary);
  CHECK(read_file(target) == "last good file");
  CHECK(temporary_count(directory.path()) == 0U);
}

void replacement_failure_retains_recovery_copy() {
  TestDirectory directory;
  const auto target = directory.path() / "contract.docx";
  write_file(target, "last good file");
  const std::string replacement = "validated but not installed";

  const auto result = persistence::AtomicFileSaver::save_bytes(
      target, bytes_of(replacement), exact_validator(replacement), {},
      fail_at(persistence::SaveStage::ReplaceTarget));
  CHECK(!result.saved());
  CHECK(!result.target_replaced);
  CHECK(result.stage == persistence::SaveStage::ReplaceTarget);
  CHECK(read_file(target) == "last good file");
  CHECK(result.recovery_path.has_value());
  if (result.recovery_path.has_value()) {
    CHECK(result.recovery_path->parent_path() == target.parent_path());
    CHECK(read_file(*result.recovery_path) == replacement);
  }
  CHECK(temporary_count(directory.path()) == 1U);
}

void post_rename_flush_failure_is_reported_truthfully() {
  TestDirectory directory;
  const auto target = directory.path() / "notes.odt";
  write_file(target, "old");
  const std::string replacement = "new";

  const auto result = persistence::AtomicFileSaver::save_bytes(
      target, bytes_of(replacement), exact_validator(replacement), {},
      fail_at(persistence::SaveStage::FlushParentDirectory));
  CHECK(result.saved());
  CHECK(result.outcome ==
        persistence::SaveOutcome::SavedDurabilityUncertain);
  CHECK(result.target_replaced);
  CHECK(read_file(target) == replacement);
}

void symbolic_link_target_is_never_followed() {
  TestDirectory directory;
  const auto victim = directory.path() / "victim.docx";
  const auto link = directory.path() / "chosen.docx";
  write_file(victim, "do not overwrite");
  std::error_code symlink_error;
  std::filesystem::create_symlink(victim.filename(), link, symlink_error);
  CHECK(!symlink_error);
  if (symlink_error) {
    return;
  }

  const std::string replacement = "attacker-controlled overwrite";
  const auto result =
      persistence::AtomicFileSaver::save_bytes(link, bytes_of(replacement));
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::InspectTarget);
  CHECK(read_file(victim) == "do not overwrite");
  CHECK(std::filesystem::is_symlink(link));
  CHECK(temporary_count(directory.path()) == 0U);
}

void new_file_permissions_and_writer_exceptions() {
  TestDirectory directory;
  const auto target = directory.path() / "new.docx";
  const std::string contents = "new file";
  persistence::AtomicSaveOptions options;
  options.new_file_permissions = 0640;
  auto result = persistence::AtomicFileSaver::save_bytes(
      target, bytes_of(contents), exact_validator(contents), options);
  CHECK(result.saved());
  struct stat status {};
  CHECK(::stat(target.c_str(), &status) == 0);
  CHECK((status.st_mode & 0777) == 0640);

  const auto second_target = directory.path() / "exception.docx";
  write_file(second_target, "safe");
  result = persistence::AtomicFileSaver::save(
      second_target,
      [](int, std::string&) -> bool { throw std::runtime_error("injected"); });
  CHECK(!result.saved());
  CHECK(read_file(second_target) == "safe");
  CHECK(temporary_count(directory.path()) == 0U);
}

void path_only_writer_is_validated_before_atomic_replace() {
  TestDirectory directory;
  const auto target = directory.path() / "engine.docx";
  write_file(target, "last known good");

  auto result = persistence::AtomicFileSaver::save_via_path(
      target,
      [](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "engine output");
        return true;
      },
      [](const std::filesystem::path& temporary, std::string& error) {
        if (read_file(temporary) != "engine output") {
          error = "reopened output differs";
          return false;
        }
        return true;
      });
  CHECK(result.saved());
  CHECK(read_file(target) == "engine output");
  CHECK(temporary_count(directory.path()) == 0U);

  write_file(target, "still good");
  result = persistence::AtomicFileSaver::save_via_path(
      target,
      [](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "bad output");
        return true;
      },
      [](const std::filesystem::path&, std::string& error) {
        error = "engine reopen rejected the output";
        return false;
      });
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::ValidateTemporary);
  CHECK(read_file(target) == "still good");
  CHECK(temporary_count(directory.path()) == 0U);
}

void path_only_writer_cannot_swap_in_a_link_or_clobber_a_changed_target() {
  TestDirectory directory;
  const auto target = directory.path() / "chosen.docx";
  const auto victim = directory.path() / "victim.docx";
  write_file(target, "original");
  write_file(victim, "victim");

  auto result = persistence::AtomicFileSaver::save_via_path(
      target,
      [&victim](const std::filesystem::path& temporary, std::string&) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        if (error) {
          return false;
        }
        std::filesystem::create_symlink(victim, temporary, error);
        return !error;
      });
  CHECK(!result.saved());
  CHECK(read_file(target) == "original");
  CHECK(read_file(victim) == "victim");

  const auto displaced = directory.path() / "other-save.docx";
  result = persistence::AtomicFileSaver::save_via_path(
      target,
      [&target, &displaced](const std::filesystem::path& temporary,
                            std::string&) {
        write_file(temporary, "our output");
        write_file(displaced, "other application output");
        std::error_code error;
        std::filesystem::rename(displaced, target, error);
        return !error;
      });
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::ReplaceTarget);
  CHECK(read_file(target) == "other application output");
  CHECK(temporary_count(directory.path()) == 0U);
}

void fresh_and_confirmed_destination_expectations_are_enforced() {
  TestDirectory directory;
  const auto target = directory.path() / "decision.docx";
  write_file(target, "appeared after the chooser check");

  bool writer_called = false;
  persistence::AtomicSaveOptions options;
  options.require_target_absent = true;
  auto result = persistence::AtomicFileSaver::save(
      target,
      [&writer_called](int, std::string&) {
        writer_called = true;
        return true;
      },
      {}, options);
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::InspectTarget);
  CHECK(result.error == std::errc::file_exists);
  CHECK(!writer_called);
  CHECK(read_file(target) == "appeared after the chooser check");
  CHECK(temporary_count(directory.path()) == 0U);

  std::error_code fingerprint_error;
  const auto fingerprint =
      persistence::AtomicFileSaver::capture_target_fingerprint(
          target, fingerprint_error);
  CHECK(!fingerprint_error);
  CHECK(fingerprint.has_value());
  if (!fingerprint.has_value()) {
    return;
  }

  // Simulate an external same-inode update while overwrite confirmation is
  // visible. Changing the mode makes the snapshot difference deterministic
  // even on filesystems with coarse timestamp resolution.
  write_file(target, "newer external version");
  CHECK(::chmod(target.c_str(), 0600) == 0);
  options = {};
  options.expected_existing_target = *fingerprint;
  writer_called = false;
  result = persistence::AtomicFileSaver::save(
      target,
      [&writer_called](int, std::string&) {
        writer_called = true;
        return true;
      },
      {}, options);
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::InspectTarget);
  CHECK(result.error == std::errc::state_not_recoverable);
  CHECK(!writer_called);
  CHECK(read_file(target) == "newer external version");

  const auto refreshed_fingerprint =
      persistence::AtomicFileSaver::capture_target_fingerprint(
          target, fingerprint_error);
  CHECK(!fingerprint_error);
  CHECK(refreshed_fingerprint.has_value());
  if (refreshed_fingerprint.has_value()) {
    const std::string confirmed_output = "confirmed replacement";
    options.expected_existing_target = *refreshed_fingerprint;
    result = persistence::AtomicFileSaver::save_bytes(
        target, bytes_of(confirmed_output), exact_validator(confirmed_output),
        options);
    CHECK(result.saved());
    CHECK(read_file(target) == confirmed_output);
  }
}

void same_inode_destination_edit_during_write_is_not_lost() {
  TestDirectory directory;
  const auto target = directory.path() / "shared.docx";
  write_file(target, "original version");
  struct stat original_status {};
  CHECK(::stat(target.c_str(), &original_status) == 0);

  const auto result = persistence::AtomicFileSaver::save_via_path(
      target,
      [&target](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "our staged output");
        write_file(target, "external version");
        return true;
      });
  struct stat changed_status {};
  CHECK(::stat(target.c_str(), &changed_status) == 0);
  CHECK(original_status.st_dev == changed_status.st_dev);
  CHECK(original_status.st_ino == changed_status.st_ino);
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::ReplaceTarget);
  CHECK(read_file(target) == "external version");
  CHECK(temporary_count(directory.path()) == 0U);
}

void path_validator_cannot_replace_the_validated_stage() {
  TestDirectory directory;
  const auto target = directory.path() / "validated.docx";
  const auto substitute = directory.path() / "unvalidated-output.docx";
  write_file(target, "last good file");

  const auto result = persistence::AtomicFileSaver::save_via_path(
      target,
      [](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "validated bytes");
        return true;
      },
      [&substitute](const std::filesystem::path& temporary,
                    std::string& error_text) {
        if (read_file(temporary) != "validated bytes") {
          error_text = "fixture did not read the expected stage";
          return false;
        }
        write_file(substitute, "different bytes after validation");
        std::error_code error;
        std::filesystem::rename(substitute, temporary, error);
        if (error) {
          error_text = error.message();
          return false;
        }
        return true;
      });
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::ValidateTemporary);
  CHECK(read_file(target) == "last good file");
  CHECK(temporary_count(directory.path()) == 0U);
}

void displaced_original_is_restored_instead_of_cleaned_as_stage() {
  TestDirectory directory;
  const auto target = directory.path() / "original.docx";
  write_file(target, "only copy of the original");

  const auto result = persistence::AtomicFileSaver::save_via_path(
      target,
      [&target](const std::filesystem::path& temporary, std::string& error_text) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        if (error) {
          error_text = error.message();
          return false;
        }
        std::filesystem::rename(target, temporary, error);
        if (error) {
          error_text = error.message();
          return false;
        }
        return true;
      });
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::WriteTemporary);
  CHECK(read_file(target) == "only copy of the original");
  CHECK(temporary_count(directory.path()) == 0U);
}

void symbolic_link_parent_is_rejected() {
  TestDirectory directory;
  const auto real_parent = directory.path() / "real-parent";
  const auto linked_parent = directory.path() / "linked-parent";
  std::filesystem::create_directory(real_parent);
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(real_parent.filename(), linked_parent,
                                            symlink_error);
  CHECK(!symlink_error);
  if (symlink_error) {
    return;
  }

  const std::string output = "must not traverse the chosen parent link";
  const auto result = persistence::AtomicFileSaver::save_bytes(
      linked_parent / "report.docx", bytes_of(output));
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::OpenParentDirectory);
  CHECK(!std::filesystem::exists(real_parent / "report.docx"));
  CHECK(temporary_count(real_parent) == 0U);
}

void path_writer_permissions_and_replacement_cleanup_are_controlled() {
  TestDirectory directory;
  const auto target = directory.path() / "permissions.docx";
  write_file(target, "old");
  CHECK(::chmod(target.c_str(), 0640) == 0);

  auto result = persistence::AtomicFileSaver::save_via_path(
      target,
      [](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "new");
        return ::chmod(temporary.c_str(), 0777) == 0;
      });
  CHECK(result.saved());
  struct stat status {};
  CHECK(::stat(target.c_str(), &status) == 0);
  CHECK((status.st_mode & 0777) == 0640);

  write_file(target, "last good file");
  persistence::AtomicSaveOptions options;
  options.retain_recovery_on_replace_failure = false;
  result = persistence::AtomicFileSaver::save_via_path(
      target,
      [](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "validated replacement");
        return true;
      },
      {}, options, fail_at(persistence::SaveStage::ReplaceTarget));
  CHECK(!result.saved());
  CHECK(!result.recovery_path.has_value());
  CHECK(read_file(target) == "last good file");
  CHECK(temporary_count(directory.path()) == 0U);

  const auto cleanup_failure = [](const persistence::SaveStage stage)
      -> std::optional<std::error_code> {
    if (stage == persistence::SaveStage::ReplaceTarget ||
        stage == persistence::SaveStage::CleanupTemporary) {
      return std::make_error_code(std::errc::io_error);
    }
    return std::nullopt;
  };
  result = persistence::AtomicFileSaver::save_via_path(
      target,
      [](const std::filesystem::path& temporary, std::string&) {
        write_file(temporary, "retained after cleanup failure");
        return true;
      },
      {}, options, cleanup_failure);
  CHECK(!result.saved());
  CHECK(result.recovery_path.has_value());
  CHECK(read_file(target) == "last good file");
  CHECK(temporary_count(directory.path()) == 1U);
}

void destination_appearing_during_save_is_not_replaced() {
  TestDirectory directory;
  const auto target = directory.path() / "late.docx";
  const std::string staged = "our staged output";
  persistence::AtomicSaveOptions options;
  options.require_target_absent = true;
  const auto appear_before_commit = [&target](const persistence::SaveStage stage)
      -> std::optional<std::error_code> {
    if (stage == persistence::SaveStage::ReplaceTarget) {
      write_file(target, "another application's file");
    }
    return std::nullopt;
  };
  const auto result = persistence::AtomicFileSaver::save_bytes(
      target, bytes_of(staged), exact_validator(staged), options,
      appear_before_commit);
  CHECK(!result.saved());
  CHECK(result.stage == persistence::SaveStage::ReplaceTarget);
  CHECK(read_file(target) == "another application's file");
  CHECK(temporary_count(directory.path()) == 0U);
}

}  // namespace

int main() {
  successful_replace_is_validated_and_preserves_mode();
  failures_before_replace_leave_original_untouched();
  replacement_failure_retains_recovery_copy();
  post_rename_flush_failure_is_reported_truthfully();
  symbolic_link_target_is_never_followed();
  new_file_permissions_and_writer_exceptions();
  path_only_writer_is_validated_before_atomic_replace();
  path_only_writer_cannot_swap_in_a_link_or_clobber_a_changed_target();
  fresh_and_confirmed_destination_expectations_are_enforced();
  same_inode_destination_edit_during_write_is_not_lost();
  path_validator_cannot_replace_the_validated_stage();
  displaced_original_is_restored_instead_of_cleaned_as_stage();
  symbolic_link_parent_is_rejected();
  path_writer_permissions_and_replacement_cleanup_are_controlled();
  destination_appearing_during_save_is_not_replaced();
  if (failures != 0) {
    std::cerr << failures << " atomic save test(s) failed\n";
    return 1;
  }
  std::cout << "atomic save tests passed\n";
  return 0;
}
