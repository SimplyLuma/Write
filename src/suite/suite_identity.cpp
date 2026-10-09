#include "suite/suite_identity.h"

#include "luma_config.hpp"

#include <array>
#include <cctype>

namespace luma::suite {
namespace {

const SuiteIdentity kWriteIdentity{
    .suite_name = ::luma::identity::suite_name,
    .product_name = ::luma::identity::app_name,
    .display_name = "Write",
    .application_id = ::luma::identity::app_id,
    .document_type_label = "Document",
    .default_document_extension = "docx",
    .family = ApplicationFamily::Write,
};

// These provisional entries reserve coherent identity seams for the next two
// suite applications; they do not imply those products are implemented yet.
const SuiteIdentity kGridIdentity{
    .suite_name = "Project Luma",
    .product_name = "Luma Grid",
    .display_name = "Grid",
    .application_id = "io.luma.Grid",
    .document_type_label = "Spreadsheet",
    .default_document_extension = "xlsx",
    .family = ApplicationFamily::Grid,
};

const SuiteIdentity kSlidesIdentity{
    .suite_name = "Project Luma",
    .product_name = "Luma Slides",
    .display_name = "Slides",
    .application_id = "io.luma.Slides",
    .document_type_label = "Presentation",
    .default_document_extension = "pptx",
    .family = ApplicationFamily::Slides,
};

bool is_valid_application_id(std::string_view value) {
  if (value.empty() || value.front() == '.' || value.back() == '.') {
    return false;
  }

  std::size_t segment_count = 1;
  bool at_segment_start = true;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character == '.') {
      if (at_segment_start) {
        return false;
      }
      ++segment_count;
      at_segment_start = true;
      continue;
    }
    if (!(std::isalnum(character) || character == '_' || character == '-')) {
      return false;
    }
    at_segment_start = false;
  }
  return !at_segment_start && segment_count >= 3;
}

bool is_valid_extension(std::string_view value) {
  if (value.empty() || value.front() == '.') {
    return false;
  }
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (!(std::islower(character) || std::isdigit(character))) {
      return false;
    }
  }
  return true;
}

}  // namespace

const SuiteIdentity& identity_for(const ApplicationFamily family) {
  switch (family) {
    case ApplicationFamily::Write:
      return kWriteIdentity;
    case ApplicationFamily::Grid:
      return kGridIdentity;
    case ApplicationFamily::Slides:
      return kSlidesIdentity;
  }
  return kWriteIdentity;
}

std::string_view command_namespace_for(const ApplicationFamily family) noexcept {
  switch (family) {
    case ApplicationFamily::Write:
      return "write";
    case ApplicationFamily::Grid:
      return "grid";
    case ApplicationFamily::Slides:
      return "slides";
  }
  return "write";
}

std::vector<std::string> validate_identity(const SuiteIdentity& identity) {
  std::vector<std::string> problems;
  if (identity.suite_name.empty()) {
    problems.emplace_back("suite_name must not be empty");
  }
  if (identity.product_name.empty()) {
    problems.emplace_back("product_name must not be empty");
  }
  if (identity.display_name.empty()) {
    problems.emplace_back("display_name must not be empty");
  }
  if (!is_valid_application_id(identity.application_id)) {
    problems.emplace_back("application_id must be a reverse-DNS identifier");
  }
  if (identity.document_type_label.empty()) {
    problems.emplace_back("document_type_label must not be empty");
  }
  if (!is_valid_extension(identity.default_document_extension)) {
    problems.emplace_back(
        "default_document_extension must be lowercase and omit the dot");
  }
  return problems;
}

}  // namespace luma::suite
