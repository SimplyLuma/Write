#include "keyboard_traversal.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace luma::write::accessibility {
namespace {

std::string surface_name(const suite::ShellSurface surface) {
  switch (surface) {
    case suite::ShellSurface::StudioBar:
      return "Document controls";
    case suite::ShellSurface::CommandBar:
      return "Formatting controls";
    case suite::ShellSurface::Navigator:
      return "Document navigator";
    case suite::ShellSurface::Canvas:
      return "Document";
    case suite::ShellSurface::Inspector:
      return "Properties";
    case suite::ShellSurface::StatusBar:
      return "Document status";
  }
  return "Application controls";
}

bool available(const suite::ShellSurface surface,
               const ShellFocusAvailability& availability) {
  switch (surface) {
    case suite::ShellSurface::StudioBar:
      return availability.studio_bar;
    case suite::ShellSurface::CommandBar:
      return availability.command_bar;
    case suite::ShellSurface::Navigator:
      return availability.navigator;
    case suite::ShellSurface::Canvas:
      return availability.canvas;
    case suite::ShellSurface::Inspector:
      return availability.inspector;
    case suite::ShellSurface::StatusBar:
      return availability.status_bar;
  }
  return false;
}

}  // namespace

KeyboardTraversalOrder KeyboardTraversalOrder::from_suite_shell(
    const suite::SuiteShellContract& contract) {
  std::vector<ShellFocusTarget> targets;
  targets.reserve(contract.surfaces().size());
  for (const auto& surface : contract.surfaces()) {
    if (surface.focus_order <= 0) {
      continue;
    }
    targets.push_back({
        .surface = surface.surface,
        .stable_id = surface.stable_id,
        .accessible_name = surface_name(surface.surface),
        .focus_order = surface.focus_order,
    });
  }
  std::stable_sort(targets.begin(), targets.end(),
                   [](const ShellFocusTarget& left,
                      const ShellFocusTarget& right) {
                     return left.focus_order < right.focus_order;
                   });
  return KeyboardTraversalOrder(std::move(targets));
}

std::vector<ShellFocusTarget> KeyboardTraversalOrder::available_targets(
    const ShellFocusAvailability& availability) const {
  std::vector<ShellFocusTarget> result;
  result.reserve(targets_.size());
  for (const auto& target : targets_) {
    if (available(target.surface, availability)) {
      result.push_back(target);
    }
  }
  return result;
}

std::optional<ShellFocusTarget> KeyboardTraversalOrder::next(
    const std::string_view current_stable_id,
    const TraversalDirection direction,
    const ShellFocusAvailability& availability, const bool wrap) const {
  const auto candidates = available_targets(availability);
  if (candidates.empty() || targets_.empty()) {
    return std::nullopt;
  }
  const auto current = std::find_if(
      targets_.begin(), targets_.end(),
      [current_stable_id](const ShellFocusTarget& target) {
        return target.stable_id == current_stable_id;
      });
  if (current == targets_.end()) {
    return direction == TraversalDirection::Forward ? candidates.front()
                                                    : candidates.back();
  }

  const auto current_index =
      static_cast<std::size_t>(std::distance(targets_.begin(), current));
  for (std::size_t step = 1; step <= targets_.size(); ++step) {
    std::size_t index = 0;
    if (direction == TraversalDirection::Forward) {
      const std::size_t unwrapped = current_index + step;
      if (!wrap && unwrapped >= targets_.size()) {
        return std::nullopt;
      }
      index = unwrapped % targets_.size();
    } else {
      if (!wrap && step > current_index) {
        return std::nullopt;
      }
      index = (current_index + targets_.size() -
               (step % targets_.size())) %
              targets_.size();
    }
    if (available(targets_[index].surface, availability)) {
      return targets_[index];
    }
  }
  return std::nullopt;
}

bool KeyboardTraversalOrder::matches_suite_shell(
    const suite::SuiteShellContract& contract) const {
  return targets_ == from_suite_shell(contract).targets_;
}

}  // namespace luma::write::accessibility
