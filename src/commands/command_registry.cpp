#include "commands/command_registry.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace luma::commands {
namespace {

std::string trim(const std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return std::string{value.substr(first, last - first)};
}

std::string ascii_lower(const std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    result.push_back(static_cast<char>(std::tolower(character)));
  }
  return result;
}

std::string normalize_search_text(const std::string_view value) {
  std::string result;
  result.reserve(value.size());
  bool pending_space = false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isalnum(character)) {
      if (pending_space && !result.empty()) {
        result.push_back(' ');
      }
      result.push_back(static_cast<char>(std::tolower(character)));
      pending_space = false;
    } else {
      pending_space = !result.empty();
    }
  }
  return result;
}

std::vector<std::string> words(const std::string_view value) {
  std::vector<std::string> result;
  std::size_t position = 0;
  while (position < value.size()) {
    const auto next = value.find(' ', position);
    const auto end = next == std::string_view::npos ? value.size() : next;
    if (end > position) {
      result.emplace_back(value.substr(position, end - position));
    }
    position = next == std::string_view::npos ? value.size() : next + 1;
  }
  return result;
}

bool all_query_words_match(const std::vector<std::string>& query_words,
                           const std::vector<std::string>& candidate_words) {
  return std::all_of(
      query_words.begin(), query_words.end(),
      [&candidate_words](const std::string& query_word) {
        return std::any_of(candidate_words.begin(), candidate_words.end(),
                           [&query_word](const std::string& candidate_word) {
                             return candidate_word.starts_with(query_word);
                           });
      });
}

int text_match_score(const std::string& query,
                     const std::vector<std::string>& query_words,
                     const std::string_view candidate) {
  const std::string normalized = normalize_search_text(candidate);
  if (normalized.empty()) {
    return 0;
  }
  if (normalized == query) {
    return 1000;
  }
  if (normalized.starts_with(query)) {
    return 850;
  }
  if (all_query_words_match(query_words, words(normalized))) {
    return 700;
  }
  if (normalized.find(query) != std::string::npos) {
    return 550;
  }
  return 0;
}

std::optional<std::string> canonical_key(const std::string_view raw_key) {
  const std::string lowered = ascii_lower(trim(raw_key));
  if (lowered.size() == 1 &&
      std::isalnum(static_cast<unsigned char>(lowered.front()))) {
    return std::string{
        static_cast<char>(std::toupper(static_cast<unsigned char>(lowered[0])))};
  }

  const std::pair<std::string_view, std::string_view> named_keys[] = {
      {"enter", "Enter"},       {"escape", "Escape"},
      {"tab", "Tab"},           {"space", "Space"},
      {"backspace", "Backspace"}, {"delete", "Delete"},
      {"home", "Home"},         {"end", "End"},
      {"pageup", "PageUp"},     {"pagedown", "PageDown"},
      {"left", "Left"},         {"right", "Right"},
      {"up", "Up"},             {"down", "Down"},
      {"plus", "Plus"},         {"minus", "Minus"},
      {"comma", "Comma"},       {"period", "Period"},
      {"bracketleft", "BracketLeft"}, {"bracketright", "BracketRight"},
      {"backslash", "Backslash"},
  };
  for (const auto& [input, canonical] : named_keys) {
    if (lowered == input) {
      return std::string{canonical};
    }
  }
  if (lowered.size() >= 2 && lowered.front() == 'f') {
    int number = 0;
    for (std::size_t index = 1; index < lowered.size(); ++index) {
      if (!std::isdigit(static_cast<unsigned char>(lowered[index]))) {
        return std::nullopt;
      }
      number = number * 10 + (lowered[index] - '0');
    }
    if (number >= 1 && number <= 24) {
      return "F" + std::to_string(number);
    }
  }
  return std::nullopt;
}

bool valid_segment(const std::string_view segment) {
  if (segment.empty() || !std::islower(static_cast<unsigned char>(segment[0]))) {
    return false;
  }
  if (segment.back() == '-') {
    return false;
  }
  for (const char raw_character : segment) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (!(std::islower(character) || std::isdigit(character) ||
          character == '-')) {
      return false;
    }
  }
  return true;
}

}  // namespace

CommandNamespace command_namespace(const std::string_view command_id) noexcept {
  const auto separator = command_id.find('.');
  const auto root = command_id.substr(0, separator);
  if (root == "suite") {
    return CommandNamespace::Suite;
  }
  if (root == "write") {
    return CommandNamespace::Write;
  }
  if (root == "grid") {
    return CommandNamespace::Grid;
  }
  if (root == "slides") {
    return CommandNamespace::Slides;
  }
  return CommandNamespace::Invalid;
}

bool is_valid_command_id(const std::string_view command_id) noexcept {
  if (command_id.empty() || command_id.size() > 128 ||
      command_namespace(command_id) == CommandNamespace::Invalid) {
    return false;
  }

  std::size_t segment_count = 0;
  std::size_t position = 0;
  while (position < command_id.size()) {
    const auto next = command_id.find('.', position);
    const auto end = next == std::string_view::npos ? command_id.size() : next;
    if (!valid_segment(command_id.substr(position, end - position))) {
      return false;
    }
    ++segment_count;
    position = next == std::string_view::npos ? command_id.size() : next + 1;
  }
  return segment_count >= 3 && command_id.back() != '.';
}

bool command_is_available_in(const std::string_view command_id,
                             const suite::ApplicationFamily family) noexcept {
  if (!is_valid_command_id(command_id)) {
    return false;
  }
  switch (command_namespace(command_id)) {
    case CommandNamespace::Suite:
      return true;
    case CommandNamespace::Write:
      return family == suite::ApplicationFamily::Write;
    case CommandNamespace::Grid:
      return family == suite::ApplicationFamily::Grid;
    case CommandNamespace::Slides:
      return family == suite::ApplicationFamily::Slides;
    case CommandNamespace::Invalid:
      return false;
  }
  return false;
}

std::optional<std::string> canonicalize_shortcut(
    const std::string_view shortcut) {
  const std::string input = trim(shortcut);
  if (input.empty() || input.back() == '+') {
    return std::nullopt;
  }

  bool primary = false;
  bool shift = false;
  bool alt = false;
  bool super = false;
  std::optional<std::string> key;
  std::size_t position = 0;
  while (position < input.size()) {
    const auto next = input.find('+', position);
    const auto end = next == std::string::npos ? input.size() : next;
    const std::string token = ascii_lower(trim(
        std::string_view{input}.substr(position, end - position)));
    if (token.empty()) {
      return std::nullopt;
    }

    bool* modifier = nullptr;
    if (token == "primary") {
      modifier = &primary;
    } else if (token == "shift") {
      modifier = &shift;
    } else if (token == "alt") {
      modifier = &alt;
    } else if (token == "super") {
      modifier = &super;
    }
    if (modifier != nullptr) {
      if (*modifier) {
        return std::nullopt;
      }
      *modifier = true;
    } else {
      if (key.has_value()) {
        return std::nullopt;
      }
      key = canonical_key(token);
      if (!key.has_value()) {
        return std::nullopt;
      }
    }

    position = next == std::string::npos ? input.size() : next + 1;
  }
  if (!key.has_value()) {
    return std::nullopt;
  }

  std::string result;
  const auto append_modifier = [&result](const std::string_view modifier) {
    if (!result.empty()) {
      result.push_back('+');
    }
    result.append(modifier);
  };
  if (primary) {
    append_modifier("Primary");
  }
  if (shift) {
    append_modifier("Shift");
  }
  if (alt) {
    append_modifier("Alt");
  }
  if (super) {
    append_modifier("Super");
  }
  if (!result.empty()) {
    result.push_back('+');
  }
  result += *key;
  return result;
}

RegistrationResult CommandRegistry::register_command(
    CommandDefinition definition, CommandCallbacks callbacks) {
  if (!is_valid_command_id(definition.id)) {
    return {RegistrationError::InvalidId,
            "command ID must be a lowercase dotted suite command path"};
  }
  if (id_index_.contains(definition.id)) {
    return {RegistrationError::DuplicateId,
            "command ID is already registered: " + definition.id};
  }

  definition.label = trim(definition.label);
  if (definition.label.empty()) {
    return {RegistrationError::EmptyLabel, "command label must not be empty"};
  }

  std::set<std::string> normalized_synonyms;
  for (auto& synonym : definition.search_synonyms) {
    synonym = trim(synonym);
    const auto normalized = normalize_search_text(synonym);
    if (normalized.empty() || !normalized_synonyms.insert(normalized).second) {
      return {RegistrationError::InvalidSynonym,
              "command synonyms must be non-empty and unique"};
    }
  }

  if (definition.shortcut.has_value()) {
    const auto canonical = canonicalize_shortcut(*definition.shortcut);
    if (!canonical.has_value()) {
      return {RegistrationError::InvalidShortcut,
              "shortcut must contain one key and supported modifiers"};
    }
    const auto owner = shortcut_owners_.find(*canonical);
    if (owner != shortcut_owners_.end()) {
      return {RegistrationError::DuplicateShortcut,
              *canonical + " is already owned by " + owner->second};
    }
    definition.shortcut = *canonical;
  }

  const std::size_t index = entries_.size();
  entries_.push_back(Entry{
      .definition = std::move(definition),
      .stored_state = {},
      .callbacks = std::move(callbacks),
      .registration_order = index,
  });
  id_index_.emplace(entries_.back().definition.id, index);
  if (entries_.back().definition.shortcut.has_value()) {
    shortcut_owners_.emplace(*entries_.back().definition.shortcut,
                             entries_.back().definition.id);
  }
  return {};
}

const CommandRegistry::Entry* CommandRegistry::entry(
    const std::string_view command_id) const noexcept {
  const auto found = id_index_.find(std::string{command_id});
  return found == id_index_.end() ? nullptr : &entries_[found->second];
}

CommandRegistry::Entry* CommandRegistry::entry(
    const std::string_view command_id) noexcept {
  const auto found = id_index_.find(std::string{command_id});
  return found == id_index_.end() ? nullptr : &entries_[found->second];
}

const CommandDefinition* CommandRegistry::definition(
    const std::string_view command_id) const noexcept {
  const auto* found = entry(command_id);
  return found == nullptr ? nullptr : &found->definition;
}

CommandState CommandRegistry::resolved_state(const Entry& found) const {
  CommandState result = found.callbacks.resolve_state
                            ? found.callbacks.resolve_state()
                            : found.stored_state;
  if (!found.definition.checkable) {
    result.checked = CheckState::Unchecked;
  }
  return result;
}

std::optional<CommandState> CommandRegistry::state(
    const std::string_view command_id) const {
  const auto* found = entry(command_id);
  if (found == nullptr) {
    return std::nullopt;
  }
  return resolved_state(*found);
}

bool CommandRegistry::update_state(const std::string_view command_id,
                                   const CommandState state_value) {
  auto* found = entry(command_id);
  if (found == nullptr ||
      (!found->definition.checkable &&
       state_value.checked != CheckState::Unchecked)) {
    return false;
  }
  found->stored_state = state_value;
  return true;
}

bool CommandRegistry::set_handler(const std::string_view command_id,
                                  CommandHandler handler) {
  auto* found = entry(command_id);
  if (found == nullptr) {
    return false;
  }
  found->callbacks.execute = std::move(handler);
  return true;
}

bool CommandRegistry::set_state_resolver(
    const std::string_view command_id, CommandStateResolver resolver) {
  auto* found = entry(command_id);
  if (found == nullptr) {
    return false;
  }
  found->callbacks.resolve_state = std::move(resolver);
  return true;
}

ExecutionResult CommandRegistry::execute(const std::string_view command_id) {
  auto* found = entry(command_id);
  if (found == nullptr) {
    return ExecutionResult::UnknownCommand;
  }
  const CommandState current = resolved_state(*found);
  if (!current.visible) {
    return ExecutionResult::Hidden;
  }
  if (!current.enabled) {
    return ExecutionResult::Disabled;
  }
  if (!found->callbacks.execute) {
    return ExecutionResult::MissingHandler;
  }
  found->callbacks.execute();
  return ExecutionResult::Executed;
}

std::vector<CommandSearchResult> CommandRegistry::search(
    const std::string_view query, const SearchOptions options) const {
  const std::string normalized_query = normalize_search_text(query);
  if (normalized_query.empty() || options.limit == 0) {
    return {};
  }
  const auto query_words = words(normalized_query);

  struct RankedResult {
    CommandSearchResult result;
    std::size_t registration_order{0};
  };
  std::vector<RankedResult> ranked;
  for (const auto& item : entries_) {
    if (options.application.has_value() &&
        !command_is_available_in(item.definition.id, *options.application)) {
      continue;
    }
    const CommandState current = resolved_state(item);
    if (!options.include_hidden && !current.visible) {
      continue;
    }

    int best_score = 0;
    std::string best_match;
    const auto consider = [&](const std::string_view text, const int weight) {
      const int score = text_match_score(normalized_query, query_words, text);
      if (score + weight > best_score) {
        best_score = score + weight;
        best_match = std::string{text};
      }
    };
    consider(item.definition.label, 120);
    for (const auto& synonym : item.definition.search_synonyms) {
      consider(synonym, 100);
    }
    consider(item.definition.description, 30);
    consider(item.definition.id, 10);

    if (best_score > 120) {
      ranked.push_back({
          .result =
              {
                  .id = item.definition.id,
                  .label = item.definition.label,
                  .matched_text = std::move(best_match),
                  .score = best_score,
                  .state = current,
              },
          .registration_order = item.registration_order,
      });
    }
  }

  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const RankedResult& left, const RankedResult& right) {
                     if (left.result.score != right.result.score) {
                       return left.result.score > right.result.score;
                     }
                     return left.registration_order < right.registration_order;
                   });
  if (ranked.size() > options.limit) {
    ranked.resize(options.limit);
  }
  std::vector<CommandSearchResult> results;
  results.reserve(ranked.size());
  for (auto& item : ranked) {
    results.push_back(std::move(item.result));
  }
  return results;
}

}  // namespace luma::commands
