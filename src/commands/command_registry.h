#pragma once

#include "suite/suite_identity.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace luma::commands {

enum class CommandNamespace {
  Suite,
  Write,
  Grid,
  Slides,
  Invalid,
};

/** The minimum product context required before a command may be enabled. */
enum class CommandContextRequirement {
  Global,
  Document,
  EditableDocument,
  TextSelection,
  TableSelection,
  ImageSelection,
  ReviewSelection,
};

/** Menu placement is metadata; menus and other surfaces use the same command. */
struct MenuPlacement {
  std::string menu_id;
  int section_order{0};
  int item_order{0};

  bool operator==(const MenuPlacement&) const = default;
};

/** Immutable searchable metadata for one centrally defined action. */
struct CommandDefinition {
  std::string id;
  std::string label;
  std::string description;
  std::vector<std::string> search_synonyms;
  std::optional<std::string> shortcut;
  std::optional<std::string> icon_name;
  MenuPlacement menu;
  CommandContextRequirement context{CommandContextRequirement::Global};
  bool checkable{false};

  bool operator==(const CommandDefinition&) const = default;
};

enum class CheckState {
  Unchecked,
  Checked,
  Mixed,
};

/** Canonical state consumed by buttons, menus, shortcuts, and the palette. */
struct CommandState {
  bool enabled{true};
  bool visible{true};
  CheckState checked{CheckState::Unchecked};

  bool operator==(const CommandState&) const = default;
};

using CommandHandler = std::function<void()>;
using CommandStateResolver = std::function<CommandState()>;

struct CommandCallbacks {
  CommandHandler execute;
  CommandStateResolver resolve_state;
};

enum class RegistrationError {
  None,
  InvalidId,
  DuplicateId,
  EmptyLabel,
  InvalidShortcut,
  DuplicateShortcut,
  InvalidSynonym,
};

struct RegistrationResult {
  RegistrationError error{RegistrationError::None};
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == RegistrationError::None;
  }
};

enum class ExecutionResult {
  Executed,
  UnknownCommand,
  Hidden,
  Disabled,
  MissingHandler,
};

struct SearchOptions {
  std::optional<suite::ApplicationFamily> application;
  bool include_hidden{false};
  std::size_t limit{20};
};

struct CommandSearchResult {
  std::string id;
  std::string label;
  std::string matched_text;
  int score{0};
  CommandState state;

  bool operator==(const CommandSearchResult&) const = default;
};

/** Returns the reserved namespace encoded by a stable command ID. */
CommandNamespace command_namespace(std::string_view command_id) noexcept;

/** Valid IDs are lowercase dotted paths rooted at suite/write/grid/slides. */
bool is_valid_command_id(std::string_view command_id) noexcept;

/** True for suite commands or commands belonging to the requested app. */
bool command_is_available_in(std::string_view command_id,
                             suite::ApplicationFamily family) noexcept;

/**
 * Produces a portable accelerator such as Primary+Shift+P. Primary deliberately
 * maps to the platform's standard command modifier in the UI adapter.
 */
std::optional<std::string> canonicalize_shortcut(std::string_view shortcut);

/**
 * Single source of truth for definitions, shortcuts, searchable vocabulary,
 * state, and execution. References returned by definition() remain valid until
 * this registry is destroyed.
 */
class CommandRegistry {
 public:
  CommandRegistry() = default;

  RegistrationResult register_command(
      CommandDefinition definition, CommandCallbacks callbacks = {});

  [[nodiscard]] const CommandDefinition* definition(
      std::string_view command_id) const noexcept;
  [[nodiscard]] std::optional<CommandState> state(
      std::string_view command_id) const;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  /** Updates stored engine state. Mixed is accepted only for checkable actions. */
  bool update_state(std::string_view command_id, CommandState state);
  bool set_handler(std::string_view command_id, CommandHandler handler);
  bool set_state_resolver(std::string_view command_id,
                          CommandStateResolver resolver);

  ExecutionResult execute(std::string_view command_id);

  /** Plain-language search across labels, synonyms, descriptions, and IDs. */
  [[nodiscard]] std::vector<CommandSearchResult> search(
      std::string_view query, SearchOptions options = {}) const;

 private:
  struct Entry {
    CommandDefinition definition;
    CommandState stored_state;
    CommandCallbacks callbacks;
    std::size_t registration_order{0};
  };

  [[nodiscard]] const Entry* entry(std::string_view command_id) const noexcept;
  [[nodiscard]] Entry* entry(std::string_view command_id) noexcept;
  [[nodiscard]] CommandState resolved_state(const Entry& entry) const;

  std::deque<Entry> entries_;
  std::unordered_map<std::string, std::size_t> id_index_;
  std::unordered_map<std::string, std::string> shortcut_owners_;
};

}  // namespace luma::commands
