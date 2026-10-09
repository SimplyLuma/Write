#pragma once

#include "suite/suite_shell.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luma::write::accessibility {

enum class TraversalDirection { Forward, Backward };

struct ShellFocusTarget {
  suite::ShellSurface surface{suite::ShellSurface::Canvas};
  std::string stable_id;
  std::string accessible_name;
  int focus_order{0};

  friend bool operator==(const ShellFocusTarget&,
                         const ShellFocusTarget&) = default;
};

struct ShellFocusAvailability {
  bool studio_bar{true};
  bool command_bar{true};
  bool navigator{true};
  bool canvas{true};
  bool inspector{true};
  bool status_bar{true};
};

/**
 * Toolkit-neutral traversal order derived directly from SuiteShellContract.
 * GTK widgets still own actual focus and must skip individual insensitive child
 * controls. This model only keeps surface-to-surface Tab order familiar across
 * Write, Grid, and Slides.
 */
class KeyboardTraversalOrder {
 public:
  [[nodiscard]] static KeyboardTraversalOrder from_suite_shell(
      const suite::SuiteShellContract& contract);

  [[nodiscard]] const std::vector<ShellFocusTarget>& targets() const noexcept {
    return targets_;
  }
  [[nodiscard]] std::vector<ShellFocusTarget> available_targets(
      const ShellFocusAvailability& availability) const;
  [[nodiscard]] std::optional<ShellFocusTarget> next(
      std::string_view current_stable_id, TraversalDirection direction,
      const ShellFocusAvailability& availability = {}, bool wrap = true) const;
  [[nodiscard]] bool matches_suite_shell(
      const suite::SuiteShellContract& contract) const;

 private:
  explicit KeyboardTraversalOrder(std::vector<ShellFocusTarget> targets)
      : targets_(std::move(targets)) {}

  std::vector<ShellFocusTarget> targets_;
};

}  // namespace luma::write::accessibility
