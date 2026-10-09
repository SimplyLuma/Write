#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace luma::suite {

/** Identifies one member of the Luma creative-product family. */
enum class ApplicationFamily {
  Write,
  Grid,
  Slides,
};

/**
 * Replaceable product identity data.
 *
 * UI code should consume this model instead of embedding product names,
 * application IDs, or default document labels in widgets and dialogs.
 */
struct SuiteIdentity {
  std::string suite_name;
  std::string product_name;
  std::string display_name;
  std::string application_id;
  std::string document_type_label;
  std::string default_document_extension;
  ApplicationFamily family{ApplicationFamily::Write};

  bool operator==(const SuiteIdentity&) const = default;
};

/** Returns the provisional, centrally owned identity for an application. */
const SuiteIdentity& identity_for(ApplicationFamily family);

/** Returns the namespace reserved for application-specific command IDs. */
std::string_view command_namespace_for(ApplicationFamily family) noexcept;

/**
 * Reports human-readable identity problems. An empty result means the model is
 * safe to expose to shell and persistence components.
 */
std::vector<std::string> validate_identity(const SuiteIdentity& identity);

}  // namespace luma::suite
