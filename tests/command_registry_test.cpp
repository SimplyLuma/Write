#include "commands/command_catalog.h"
#include "commands/command_ids.h"
#include "commands/command_registry.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace luma::commands;
using luma::suite::ApplicationFamily;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

CommandDefinition test_command(std::string id, std::string label,
                               std::optional<std::string> shortcut =
                                   std::nullopt) {
  return CommandDefinition{
      .id = std::move(id),
      .label = std::move(label),
      .description = "Test command",
      .search_synonyms = {"test action"},
      .shortcut = std::move(shortcut),
      .icon_name = std::nullopt,
      .menu = {"test", 0, 0},
      .context = CommandContextRequirement::Global,
      .checkable = false,
  };
}

void test_id_and_shortcut_contracts() {
  expect(is_valid_command_id(ids::suite::kSave),
         "stable suite command IDs validate");
  expect(is_valid_command_id(ids::write::kBold),
         "stable Write command IDs validate");
  expect(is_valid_command_id("grid.format.currency"),
         "Grid namespace is reserved");
  expect(is_valid_command_id("slides.insert.slide"),
         "future Slides namespace is reserved");
  expect(!is_valid_command_id("write.Bold"),
         "uppercase IDs cannot silently fork command identity");
  expect(!is_valid_command_id("unknown.file.open"),
         "unknown command namespaces are rejected");
  expect(!is_valid_command_id("suite.open"),
         "commands require category and action segments");

  expect(command_is_available_in(ids::suite::kSave, ApplicationFamily::Write) &&
             command_is_available_in(ids::suite::kSave,
                                     ApplicationFamily::Grid) &&
             command_is_available_in(ids::suite::kSave,
                                     ApplicationFamily::Slides),
         "suite commands transfer across all applications");
  expect(command_is_available_in(ids::write::kBold, ApplicationFamily::Write) &&
             !command_is_available_in(ids::write::kBold,
                                      ApplicationFamily::Grid),
         "Write commands stay in their application namespace");

  expect(canonicalize_shortcut("shift+primary+p") == "Primary+Shift+P",
         "modifier ordering is canonical and portable");
  expect(canonicalize_shortcut("Primary+plus") == "Primary+Plus",
         "named keys normalize consistently");
  expect(!canonicalize_shortcut("Primary+Shift"),
         "a shortcut without a key is invalid");
  expect(!canonicalize_shortcut("Primary+Primary+S"),
         "duplicate modifiers are invalid");
  expect(!canonicalize_shortcut("Primary+S+"),
         "a trailing accelerator separator is invalid");
}

void test_catalog_registration_and_uniqueness() {
  CommandRegistry registry;
  const auto results = register_write_command_catalog(registry);
  expect(!results.empty(), "core catalog contains commands");
  for (const auto& result : results) {
    expect(static_cast<bool>(result), "every core catalog entry registers");
  }
  expect(registry.size() == core_suite_command_definitions().size() +
                                core_write_command_definitions().size(),
         "catalog registration has no duplicate command IDs");

  const auto* save = registry.definition(ids::suite::kSave);
  expect(save != nullptr && save->shortcut == "Primary+S",
         "registered shortcut metadata is canonical");
  const auto duplicate_id = registry.register_command(
      test_command(std::string{ids::suite::kSave}, "Duplicate Save"));
  expect(duplicate_id.error == RegistrationError::DuplicateId,
         "duplicate stable IDs are rejected");

  const auto duplicate_shortcut = registry.register_command(test_command(
      "write.test.duplicate-shortcut", "Duplicate Shortcut",
      "shift+primary+s"));
  expect(duplicate_shortcut.error == RegistrationError::DuplicateShortcut,
         "shortcut collisions are detected after normalization");

  auto invalid_synonym = test_command("write.test.bad-synonym", "Bad Synonym");
  invalid_synonym.search_synonyms = {"same phrase", " Same  Phrase "};
  const auto synonym_result =
      registry.register_command(std::move(invalid_synonym));
  expect(synonym_result.error == RegistrationError::InvalidSynonym,
         "duplicate plain-language synonyms are rejected");

  const auto* stable_save_reference = save;
  expect(static_cast<bool>(registry.register_command(
             test_command("grid.format.currency", "Currency"))),
         "Grid command can use the registry");
  expect(static_cast<bool>(registry.register_command(
             test_command("slides.insert.slide", "New Slide"))),
         "future Slides command can use the registry");
  expect(stable_save_reference == registry.definition(ids::suite::kSave),
         "definition references stay stable as apps add commands");
}

void test_state_and_execution() {
  CommandRegistry registry;
  register_write_command_catalog(registry);

  expect(registry.update_state(
             ids::write::kBold,
             {.enabled = true,
              .visible = true,
              .checked = CheckState::Mixed}),
         "checkable commands honestly represent mixed engine state");
  expect(registry.state(ids::write::kBold)->checked == CheckState::Mixed,
         "mixed state is returned to every command surface");
  expect(!registry.update_state(
             ids::suite::kSave,
             {.enabled = true,
              .visible = true,
              .checked = CheckState::Checked}),
         "non-checkable commands cannot acquire a false checked state");

  int executions = 0;
  expect(registry.set_handler(ids::write::kBold, [&executions] { ++executions; }),
         "handlers can be bound after catalog registration");
  expect(registry.update_state(
             ids::write::kBold,
             {.enabled = false,
              .visible = true,
              .checked = CheckState::Unchecked}),
         "engine state can disable a command");
  expect(registry.execute(ids::write::kBold) == ExecutionResult::Disabled &&
             executions == 0,
         "disabled commands never invoke their handler");

  bool engine_enabled = true;
  expect(registry.set_state_resolver(
             ids::write::kBold,
             [&engine_enabled] {
               return CommandState{.enabled = engine_enabled,
                                   .visible = true,
                                   .checked = CheckState::Checked};
             }),
         "canonical state may come directly from the engine adapter");
  expect(registry.execute(ids::write::kBold) == ExecutionResult::Executed &&
             executions == 1,
         "enabled centralized command executes exactly once");
  engine_enabled = false;
  expect(registry.execute(ids::write::kBold) == ExecutionResult::Disabled,
         "state is resolved again instead of trusting a stale toolbar toggle");
  expect(registry.execute(ids::suite::kOpen) ==
             ExecutionResult::MissingHandler,
         "unbound catalog entries cannot pretend to execute");
  expect(registry.execute("write.unknown.command") ==
             ExecutionResult::UnknownCommand,
         "unknown commands fail explicitly");
}

void test_plain_language_search() {
  CommandRegistry registry;
  register_write_command_catalog(registry);

  auto results = registry.search("remove weird formatting");
  expect(!results.empty() &&
             results.front().id == ids::write::kClearCharacterOverrides,
         "Formatting Lens action is found through a human synonym");
  results = registry.search("landscape");
  expect(!results.empty() && results.front().id == ids::write::kLandscape,
         "page orientation is discoverable by intent");
  results = registry.search("wrap image");
  expect(!results.empty() && results.front().id == ids::write::kImageWrap,
         "image placement language finds the real wrap command");
  results = registry.search("settings");
  expect(!results.empty() && results.front().id == ids::suite::kPreferences,
         "common command synonyms are searchable");

  SearchOptions grid_only;
  grid_only.application = ApplicationFamily::Grid;
  results = registry.search("bold", grid_only);
  expect(results.empty(), "Write-only commands do not leak into Grid search");
  results = registry.search("save", grid_only);
  expect(!results.empty() && results.front().id == ids::suite::kSave,
         "suite commands remain familiar in Grid search");

  expect(registry.update_state(
             ids::suite::kPreferences,
             {.enabled = true,
              .visible = false,
              .checked = CheckState::Unchecked}),
         "visibility can follow application policy");
  expect(registry.search("settings").empty(),
         "hidden commands are omitted from search by default");
  SearchOptions include_hidden;
  include_hidden.include_hidden = true;
  expect(!registry.search("settings", include_hidden).empty(),
         "diagnostics may explicitly include hidden commands");
}

}  // namespace

int main() {
  test_id_and_shortcut_contracts();
  test_catalog_registration_and_uniqueness();
  test_state_and_execution();
  test_plain_language_search();
  std::cout << "command_registry_test: PASS\n";
  return 0;
}
