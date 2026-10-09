#include "suite/suite_identity.h"
#include "suite/suite_context.h"
#include "suite/suite_shell.h"
#include "suite/suite_tokens.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace luma::suite;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool has_violation(const std::vector<ShellContractViolation>& violations,
                   const std::string& code) {
  return std::any_of(violations.begin(), violations.end(),
                     [&code](const auto& violation) {
                       return violation.code == code;
                     });
}

void test_identity_catalog() {
  const auto& write = identity_for(ApplicationFamily::Write);
  expect(write.product_name == "Write", "Write name is centralized");
  expect(write.display_name == "Write", "Write chrome name is centralized");
  expect(write.application_id == "org.projectluma.Write",
         "application ID is centralized");
  expect(write.default_document_extension == "docx",
         "new Write documents default to DOCX");
  expect(validate_identity(write).empty(), "Write identity validates");

  const auto& grid = identity_for(ApplicationFamily::Grid);
  const auto& slides = identity_for(ApplicationFamily::Slides);
  expect(validate_identity(grid).empty(), "Grid identity seam validates");
  expect(validate_identity(slides).empty(), "Slides identity seam validates");
  expect(command_namespace_for(ApplicationFamily::Write) == "write",
         "Write command namespace is stable");
  expect(command_namespace_for(ApplicationFamily::Grid) == "grid",
         "Grid command namespace is reserved");
  expect(command_namespace_for(ApplicationFamily::Slides) == "slides",
         "Slides command namespace is reserved");

  SuiteIdentity invalid = write;
  invalid.application_id = "not an app id";
  invalid.default_document_extension = ".DOCX";
  invalid.display_name.clear();
  expect(validate_identity(invalid).size() == 3,
         "invalid replaceable identity values are reported");
}

void test_presentation_and_input_capabilities() {
  const auto desktop = context_from_values({}, {}, false);
  expect(desktop.presentation == PresentationMode::Windowed &&
             desktop.input == InputMode::Pointer && desktop.decorated() &&
             !desktop.touch_targets(),
         "desktop capability defaults are windowed and pointer-driven");

  const auto handheld = context_from_values({}, {}, true);
  expect(handheld.presentation == PresentationMode::FullscreenMobile &&
             handheld.input == InputMode::Touch && !handheld.decorated() &&
             handheld.touch_targets(),
         "handheld capability defaults are fullscreen and touch-driven");

  const auto explicit_override =
      context_from_values(" WINDOWED ", "pointer", true);
  expect(explicit_override.presentation == PresentationMode::Windowed &&
             explicit_override.input == InputMode::Pointer,
         "explicit presentation and input capabilities override device class");
}

void test_tokens() {
  const auto& tokens = default_suite_tokens();
  expect(validate_tokens(tokens).empty(), "default suite tokens validate");
  expect(tokens.geometry.studio_bar_height == ExtentRange{46, 50, 52},
         "studio bar range is 46-52px");
  expect(tokens.geometry.command_bar_height == ExtentRange{42, 44, 46},
         "command bar range is 42-46px");
  expect(tokens.geometry.status_bar_height == ExtentRange{26, 28, 30},
         "status bar range is 26-30px");
  expect(tokens.geometry.compact_maximum_open_side_panels == 1,
         "compact layouts keep at most one side panel open");
  // Updated deliberately with the Fable port: the suite's accent is the Fable
  // slate blue, not the indigo this file used to pin.
  expect(to_css_color(tokens.colors.accent) == "#527FAE",
         "accent converts to the shared CSS value");
  expect(to_css_color(tokens.colors.hairline) == "rgba(39,48,56,0.10)",
         "translucent hairline converts to CSS");
  expect(tokens.geometry.panel_radius == 15.0 &&
             tokens.geometry.control_radius == 10.0,
         "Fable ships exactly two chrome radii, 15 and 10");

  // Both treatments are real bundles, and the dark one keeps the page white.
  const auto& dark = suite_tokens(SuiteTheme::kDark);
  expect(validate_tokens(dark).empty(), "dark suite tokens validate");
  expect(to_css_color(dark.colors.page) == "#FFFFFF",
         "the page stays white in dark treatment");
  expect(dark.geometry == tokens.geometry,
         "a treatment changes colour, never proportion");

  // The ramps are contracts, not suggestions.
  SuiteTokens off_ramp = tokens;
  off_ramp.geometry.spacing.snug = off_ramp.geometry.spacing.base;
  expect(!validate_tokens(off_ramp).empty(),
         "a spacing step out of order is rejected");
  SuiteTokens off_scale = tokens;
  off_scale.typography.body = off_scale.typography.title;
  expect(!validate_tokens(off_scale).empty(),
         "a type step out of order is rejected");

  SuiteTokens invalid = tokens;
  invalid.geometry.command_bar_height = {50, 40, 30};
  invalid.colors.accent.alpha = 2.0;
  expect(validate_tokens(invalid).size() == 2,
         "bad geometry and color values are independently reported");
}

void test_standard_shell_contract() {
  const auto& contract = standard_suite_shell_contract();
  const auto& tokens = default_suite_tokens();
  expect(contract.version() == 1, "shell familiarity contract is versioned");
  expect(contract.validate().empty(), "canonical shell contract validates");
  expect(contract.surfaces().size() == 6,
         "all six shared shell surfaces are present");

  const auto* studio = contract.find_surface(ShellSurface::StudioBar);
  const auto* commands = contract.find_surface(ShellSurface::CommandBar);
  const auto* status = contract.find_surface(ShellSurface::StatusBar);
  expect(studio != nullptr && studio->edge == ShellEdge::Top &&
             studio->visual_order == 0 &&
             studio->extent == tokens.geometry.studio_bar_height &&
             studio->focus_order == 10,
         "studio bar placement, height, order, and focus are explicit");
  expect(commands != nullptr && commands->edge == ShellEdge::Top &&
             commands->visual_order == 1 &&
             commands->extent == tokens.geometry.command_bar_height &&
             commands->focus_order == 20,
         "command bar follows the studio bar without changing height");
  expect(status != nullptr && status->edge == ShellEdge::Bottom &&
             status->extent == tokens.geometry.status_bar_height &&
             status->focus_order == 60,
         "status bar has a fixed bottom placement and focus rank");

  expect(studio->action_groups.size() == 6,
         "studio bar owns the complete common action sequence");
  expect(studio->action_groups.front().stable_id ==
             action_group_ids::kDocumentIdentity &&
             studio->action_groups.back().stable_id == action_group_ids::kWindow,
         "studio action ordering begins with identity and ends with windows");
  expect(commands->action_groups.size() == 3 &&
             commands->action_groups.back().stable_id ==
                 action_group_ids::kContextual,
         "context actions transform inside the fixed command bar");

  const std::vector<ShellSurface> expected_focus{
      ShellSurface::StudioBar, ShellSurface::CommandBar, ShellSurface::Navigator,
      ShellSurface::Canvas, ShellSurface::Inspector, ShellSurface::StatusBar};
  expect(contract.focus_sequence() == expected_focus,
         "keyboard traversal is identical across suite applications");

  expect(contract.responsive().collapse_labels_before_canvas,
         "labels collapse before the canvas becomes unusably small");
  expect(contract.responsive().maximum_open_side_panels == 1,
         "both side panels do not consume compact windows by default");
}

void test_future_application_slots() {
  const auto& contract = standard_suite_shell_contract();
  expect(contract.extension_slots().size() == 8,
         "suite shell exposes focused extension seams");
  for (const auto& slot : contract.extension_slots()) {
    expect(!slot.may_change_host_extent,
           "extensions cannot make shared bar heights jump");
    expect(contract.accepts(slot.stable_id, ApplicationFamily::Write),
           "Write can contribute to each declared slot");
    expect(contract.accepts(slot.stable_id, ApplicationFamily::Grid),
           "Grid can reuse each declared slot");
    expect(contract.accepts(slot.stable_id, ApplicationFamily::Slides),
           "future Slides can reuse each declared slot");
  }
  expect(contract.find_extension_slot(extension_slot_ids::kContextualPrimary) !=
             nullptr,
         "contextual app controls have a stable primary slot");
  expect(!contract.accepts("suite.slot.unknown", ApplicationFamily::Write),
         "undeclared extension slots are rejected");
}

void test_contract_rejects_familiarity_drift() {
  const auto& standard = standard_suite_shell_contract();
  auto moved_surfaces = standard.surfaces();
  auto studio = std::find_if(moved_surfaces.begin(), moved_surfaces.end(),
                             [](const auto& surface) {
                               return surface.surface == ShellSurface::StudioBar;
                             });
  studio->edge = ShellEdge::Bottom;
  SuiteShellContract moved{standard.version(), std::move(moved_surfaces),
                           standard.extension_slots(), standard.responsive()};
  expect(has_violation(moved.validate(), "surface.invariant"),
         "moving a familiar bar is an explicit contract violation");

  auto resizing_slots = standard.extension_slots();
  resizing_slots.front().may_change_host_extent = true;
  SuiteShellContract resizing{standard.version(), standard.surfaces(),
                              std::move(resizing_slots), standard.responsive()};
  expect(has_violation(resizing.validate(), "slot.extent"),
         "an app cannot resize shared chrome through an extension slot");
}

}  // namespace

int main() {
  test_identity_catalog();
  test_presentation_and_input_capabilities();
  test_tokens();
  test_standard_shell_contract();
  test_future_application_slots();
  test_contract_rejects_familiarity_drift();
  std::cout << "suite_contract_test: PASS\n";
  return 0;
}
