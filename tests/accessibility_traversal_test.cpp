#include "accessibility/keyboard_traversal.hpp"
#include "suite/suite_shell.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace accessibility = luma::write::accessibility;
namespace suite = luma::suite;

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

void standard_order_matches_suite_contract() {
  const auto& contract = suite::standard_suite_shell_contract();
  const auto order =
      accessibility::KeyboardTraversalOrder::from_suite_shell(contract);
  const std::vector<suite::ShellSurface> expected{
      suite::ShellSurface::StudioBar, suite::ShellSurface::CommandBar,
      suite::ShellSurface::Navigator, suite::ShellSurface::Canvas,
      suite::ShellSurface::Inspector, suite::ShellSurface::StatusBar,
  };
  CHECK(order.targets().size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(order.targets()[index].surface == expected[index]);
    CHECK(order.targets()[index].focus_order ==
          static_cast<int>((index + 1U) * 10U));
    CHECK(!order.targets()[index].stable_id.empty());
    CHECK(!order.targets()[index].accessible_name.empty());
  }
  CHECK(order.matches_suite_shell(contract));
}

void forward_backward_wrap_and_unknown_current() {
  const auto order = accessibility::KeyboardTraversalOrder::from_suite_shell(
      suite::standard_suite_shell_contract());
  auto next = order.next(suite::surface_ids::kStudioBar,
                         accessibility::TraversalDirection::Forward);
  CHECK(next.has_value());
  CHECK(next->stable_id == suite::surface_ids::kCommandBar);

  next = order.next(suite::surface_ids::kStudioBar,
                    accessibility::TraversalDirection::Backward);
  CHECK(next.has_value());
  CHECK(next->stable_id == suite::surface_ids::kStatusBar);

  next = order.next(suite::surface_ids::kStatusBar,
                    accessibility::TraversalDirection::Forward, {}, false);
  CHECK(!next.has_value());

  next = order.next("unknown-surface",
                    accessibility::TraversalDirection::Forward);
  CHECK(next.has_value());
  CHECK(next->stable_id == suite::surface_ids::kStudioBar);
  next = order.next("unknown-surface",
                    accessibility::TraversalDirection::Backward);
  CHECK(next.has_value());
  CHECK(next->stable_id == suite::surface_ids::kStatusBar);
}

void hidden_responsive_panels_are_skipped() {
  const auto order = accessibility::KeyboardTraversalOrder::from_suite_shell(
      suite::standard_suite_shell_contract());
  accessibility::ShellFocusAvailability compact;
  compact.navigator = false;
  compact.inspector = false;
  const auto available = order.available_targets(compact);
  CHECK(available.size() == 4U);
  CHECK(available[0].surface == suite::ShellSurface::StudioBar);
  CHECK(available[1].surface == suite::ShellSurface::CommandBar);
  CHECK(available[2].surface == suite::ShellSurface::Canvas);
  CHECK(available[3].surface == suite::ShellSurface::StatusBar);

  auto next = order.next(suite::surface_ids::kCommandBar,
                         accessibility::TraversalDirection::Forward, compact);
  CHECK(next.has_value());
  CHECK(next->surface == suite::ShellSurface::Canvas);
  next = order.next(suite::surface_ids::kCanvas,
                    accessibility::TraversalDirection::Backward, compact);
  CHECK(next.has_value());
  CHECK(next->surface == suite::ShellSurface::CommandBar);

  next = order.next(suite::surface_ids::kNavigator,
                    accessibility::TraversalDirection::Forward, compact);
  CHECK(next.has_value());
  CHECK(next->surface == suite::ShellSurface::Canvas);
  next = order.next(suite::surface_ids::kNavigator,
                    accessibility::TraversalDirection::Backward, compact);
  CHECK(next.has_value());
  CHECK(next->surface == suite::ShellSurface::CommandBar);

  accessibility::ShellFocusAvailability none;
  none.studio_bar = false;
  none.command_bar = false;
  none.navigator = false;
  none.canvas = false;
  none.inspector = false;
  none.status_bar = false;
  CHECK(order.available_targets(none).empty());
  CHECK(!order.next(suite::surface_ids::kCanvas,
                    accessibility::TraversalDirection::Forward, none)
             .has_value());
}

void order_is_derived_not_hard_coded() {
  const auto& standard = suite::standard_suite_shell_contract();
  auto surfaces = standard.surfaces();
  for (auto& surface : surfaces) {
    if (surface.surface == suite::ShellSurface::Canvas) {
      surface.focus_order = 15;
    }
  }
  suite::SuiteShellContract custom{standard.version(), std::move(surfaces),
                                   standard.extension_slots(),
                                   standard.responsive()};
  const auto custom_order =
      accessibility::KeyboardTraversalOrder::from_suite_shell(custom);
  CHECK(custom_order.targets()[1].surface == suite::ShellSurface::Canvas);
  CHECK(!custom_order.matches_suite_shell(standard));
}

}  // namespace

int main() {
  standard_order_matches_suite_contract();
  forward_backward_wrap_and_unknown_current();
  hidden_responsive_panels_are_skipped();
  order_is_derived_not_hard_coded();
  if (failures != 0) {
    std::cerr << failures << " accessibility traversal test(s) failed\n";
    return 1;
  }
  std::cout << "accessibility traversal tests passed\n";
  return 0;
}
