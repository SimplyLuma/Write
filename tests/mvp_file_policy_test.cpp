#include "compatibility/mvp_file_policy.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <unistd.h>

namespace policy = luma::write::compatibility;
namespace engine = luma::write::engine;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "        \
                << #condition << '\n';                                       \
      ++failures;                                                            \
    }                                                                        \
  } while (false)

class TestDirectory {
 public:
  TestDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    path_ = std::filesystem::temp_directory_path() /
            ("luma-write-file-policy-" +
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

void suffixes_are_explicit_and_never_relabelled() {
  auto result = policy::prepare_editable_destination("Report");
  CHECK(result.ok);
  CHECK(result.path == std::filesystem::path{"Report.docx"});
  CHECK(result.format == engine::DocumentFormat::docx);

  result = policy::prepare_editable_destination("Report.ODT");
  CHECK(result.ok);
  CHECK(result.format == engine::DocumentFormat::odt);

  for (const char* unsafe : {"Report.txt", "Report.doc", "Report.docm",
                             "Report.rtf", "Report.pdf", "Report.weird"}) {
    result = policy::prepare_editable_destination(unsafe);
    CHECK(!result.ok);
  }

  result = policy::prepare_pdf_destination("Published");
  CHECK(result.ok);
  CHECK(result.path == std::filesystem::path{"Published.pdf"});
  result = policy::prepare_pdf_destination("Published.docx");
  CHECK(!result.ok);
}

void targets_are_inspected_before_confirmation() {
  TestDirectory directory;
  const auto existing = directory.path() / "existing.docx";
  std::ofstream(existing) << "existing";
  auto result = policy::prepare_editable_destination(existing);
  CHECK(result.ok);
  CHECK(result.requires_overwrite_confirmation);

  const auto fresh = directory.path() / "fresh.odt";
  result = policy::prepare_editable_destination(fresh);
  CHECK(result.ok);
  CHECK(!result.requires_overwrite_confirmation);

  const auto link = directory.path() / "linked.docx";
  std::error_code error;
  std::filesystem::create_symlink(existing.filename(), link, error);
  CHECK(!error);
  if (!error) {
    result = policy::prepare_editable_destination(link);
    CHECK(!result.ok);
  }
}

void only_first_class_formats_save_in_place() {
  CHECK(policy::can_save_in_place(engine::DocumentFormat::docx));
  CHECK(policy::can_save_in_place(engine::DocumentFormat::odt));
  for (const auto format : {engine::DocumentFormat::legacy_doc,
                            engine::DocumentFormat::text,
                            engine::DocumentFormat::pdf,
                            engine::DocumentFormat::unknown}) {
    CHECK(!policy::can_save_in_place(format));
  }
}

}  // namespace

int main() {
  suffixes_are_explicit_and_never_relabelled();
  targets_are_inspected_before_confirmation();
  only_first_class_formats_save_in_place();
  if (failures != 0) {
    std::cerr << failures << " MVP file-policy test(s) failed\n";
    return 1;
  }
  std::cout << "MVP file policy tests passed\n";
  return 0;
}
