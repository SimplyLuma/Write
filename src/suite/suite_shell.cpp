#include "suite/suite_shell.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace luma::suite {
namespace {

using FamilyList = std::vector<ApplicationFamily>;

const FamilyList kEveryFamily{
    ApplicationFamily::Write,
    ApplicationFamily::Grid,
    ApplicationFamily::Slides,
};

ActionGroupInvariant group(const std::string_view id, const int order) {
  return ActionGroupInvariant{std::string{id}, order, order};
}

SurfaceInvariant bar(const ShellSurface surface, const std::string_view id,
                     const ShellEdge edge, const int visual_order,
                     const ExtentRange extent, const int focus_order,
                     std::vector<ActionGroupInvariant> groups) {
  return SurfaceInvariant{
      .surface = surface,
      .stable_id = std::string{id},
      .edge = edge,
      .visual_order = visual_order,
      .extent = extent,
      .focus_order = focus_order,
      .visibility = VisibilityPolicy::Always,
      .action_groups = std::move(groups),
  };
}

ExtensionSlotInvariant slot(const std::string_view id, const ShellSurface host,
                            const int visual_order) {
  return ExtensionSlotInvariant{
      .stable_id = std::string{id},
      .host = host,
      .visual_order = visual_order,
      .supported_families = kEveryFamily,
      .may_change_host_extent = false,
  };
}

std::vector<SurfaceInvariant> standard_surfaces(const SuiteTokens& tokens) {
  return {
      bar(ShellSurface::StudioBar, surface_ids::kStudioBar, ShellEdge::Top, 0,
          tokens.geometry.studio_bar_height, 10,
          {group(action_group_ids::kDocumentIdentity, 0),
           group(action_group_ids::kSaveState, 1),
           group(action_group_ids::kHistory, 2),
           group(action_group_ids::kCommandSearch, 3),
           group(action_group_ids::kView, 4),
           group(action_group_ids::kWindow, 5)}),
      bar(ShellSurface::CommandBar, surface_ids::kCommandBar, ShellEdge::Top, 1,
          tokens.geometry.command_bar_height, 20,
          {group(action_group_ids::kEssentialPrimary, 0),
           group(action_group_ids::kEssentialSecondary, 1),
           group(action_group_ids::kContextual, 2)}),
      SurfaceInvariant{
          .surface = ShellSurface::Navigator,
          .stable_id = std::string{surface_ids::kNavigator},
          .edge = ShellEdge::Left,
          .visual_order = 0,
          .extent = tokens.geometry.navigator_width,
          .focus_order = 30,
          .visibility = VisibilityPolicy::ResponsiveToggle,
      },
      SurfaceInvariant{
          .surface = ShellSurface::Canvas,
          .stable_id = std::string{surface_ids::kCanvas},
          .edge = ShellEdge::Center,
          .visual_order = 0,
          .extent = std::nullopt,
          .focus_order = 40,
          .visibility = VisibilityPolicy::Always,
      },
      SurfaceInvariant{
          .surface = ShellSurface::Inspector,
          .stable_id = std::string{surface_ids::kInspector},
          .edge = ShellEdge::Right,
          .visual_order = 0,
          .extent = tokens.geometry.inspector_width,
          .focus_order = 50,
          .visibility = VisibilityPolicy::ResponsiveToggle,
      },
      bar(ShellSurface::StatusBar, surface_ids::kStatusBar, ShellEdge::Bottom,
          0, tokens.geometry.status_bar_height, 60,
          {group(action_group_ids::kLocation, 0),
           group(action_group_ids::kDocumentMetrics, 1),
           group(action_group_ids::kLanguage, 2),
           group(action_group_ids::kFormat, 3),
           group(action_group_ids::kDocumentHealth, 4),
           group(action_group_ids::kZoom, 5)}),
  };
}

std::vector<ExtensionSlotInvariant> standard_slots() {
  return {
      slot(extension_slot_ids::kStudioApplicationActions,
           ShellSurface::StudioBar, 0),
      slot(extension_slot_ids::kEssentialPrimary, ShellSurface::CommandBar, 0),
      slot(extension_slot_ids::kEssentialSecondary, ShellSurface::CommandBar,
           1),
      slot(extension_slot_ids::kContextualPrimary, ShellSurface::CommandBar, 2),
      slot(extension_slot_ids::kContextualSecondary, ShellSurface::CommandBar,
           3),
      slot(extension_slot_ids::kNavigatorContent, ShellSurface::Navigator, 0),
      slot(extension_slot_ids::kInspectorContent, ShellSurface::Inspector, 0),
      slot(extension_slot_ids::kStatusApplicationIndicators,
           ShellSurface::StatusBar, 0),
  };
}

ResponsiveInvariant standard_responsive(const SuiteTokens& tokens) {
  return ResponsiveInvariant{
      .compact_width = tokens.geometry.compact_width,
      .compact_height = tokens.geometry.compact_height,
      .maximum_open_side_panels =
          tokens.geometry.compact_maximum_open_side_panels,
      .minimum_canvas_width = tokens.geometry.minimum_canvas_width,
      .collapse_labels_before_canvas = true,
  };
}

void add_violation(std::vector<ShellContractViolation>& problems,
                   std::string code, std::string message) {
  problems.push_back({std::move(code), std::move(message)});
}

bool contains_family(const std::vector<ApplicationFamily>& families,
                     const ApplicationFamily family) {
  return std::find(families.begin(), families.end(), family) != families.end();
}

}  // namespace

SuiteShellContract::SuiteShellContract(
    const std::uint32_t version, std::vector<SurfaceInvariant> surfaces,
    std::vector<ExtensionSlotInvariant> extension_slots,
    ResponsiveInvariant responsive)
    : version_(version),
      surfaces_(std::move(surfaces)),
      extension_slots_(std::move(extension_slots)),
      responsive_(responsive) {}

const SurfaceInvariant* SuiteShellContract::find_surface(
    const ShellSurface surface) const noexcept {
  const auto found = std::find_if(
      surfaces_.begin(), surfaces_.end(),
      [surface](const SurfaceInvariant& item) { return item.surface == surface; });
  return found == surfaces_.end() ? nullptr : &*found;
}

const ExtensionSlotInvariant* SuiteShellContract::find_extension_slot(
    const std::string_view stable_id) const noexcept {
  const auto found = std::find_if(
      extension_slots_.begin(), extension_slots_.end(),
      [stable_id](const ExtensionSlotInvariant& item) {
        return item.stable_id == stable_id;
      });
  return found == extension_slots_.end() ? nullptr : &*found;
}

bool SuiteShellContract::accepts(const std::string_view slot_id,
                                 const ApplicationFamily family) const noexcept {
  const auto* extension_slot = find_extension_slot(slot_id);
  return extension_slot != nullptr &&
         contains_family(extension_slot->supported_families, family);
}

std::vector<ShellSurface> SuiteShellContract::focus_sequence() const {
  std::vector<const SurfaceInvariant*> focusable;
  for (const auto& surface : surfaces_) {
    if (surface.focus_order > 0) {
      focusable.push_back(&surface);
    }
  }
  std::stable_sort(focusable.begin(), focusable.end(),
                   [](const auto* left, const auto* right) {
                     return left->focus_order < right->focus_order;
                   });
  std::vector<ShellSurface> result;
  result.reserve(focusable.size());
  for (const auto* item : focusable) {
    result.push_back(item->surface);
  }
  return result;
}

std::vector<ShellContractViolation> SuiteShellContract::validate(
    const SuiteTokens& tokens) const {
  std::vector<ShellContractViolation> problems;
  if (version_ != 1) {
    add_violation(problems, "contract.version",
                  "unsupported suite shell contract version");
  }

  std::set<ShellSurface> surfaces_seen;
  std::set<std::string> surface_ids_seen;
  std::set<int> focus_orders_seen;
  for (const auto& surface : surfaces_) {
    if (!surfaces_seen.insert(surface.surface).second) {
      add_violation(problems, "surface.duplicate",
                    "each shell surface must appear exactly once");
    }
    if (surface.stable_id.empty() ||
        !surface_ids_seen.insert(surface.stable_id).second) {
      add_violation(problems, "surface.id",
                    "surface stable IDs must be non-empty and unique");
    }
    if (surface.focus_order <= 0 ||
        !focus_orders_seen.insert(surface.focus_order).second) {
      add_violation(problems, "surface.focus-order",
                    "surface focus orders must be positive and unique");
    }

    std::set<std::string> group_ids;
    std::set<int> group_visual_orders;
    std::set<int> group_focus_orders;
    for (const auto& group_item : surface.action_groups) {
      if (group_item.stable_id.empty() ||
          !group_ids.insert(group_item.stable_id).second) {
        add_violation(problems, "group.id",
                      "action group IDs must be non-empty and unique per bar");
      }
      if (!group_visual_orders.insert(group_item.visual_order).second) {
        add_violation(problems, "group.visual-order",
                      "action group visual orders must be unique per bar");
      }
      if (!group_focus_orders.insert(group_item.focus_order).second) {
        add_violation(problems, "group.focus-order",
                      "action group focus orders must be unique per bar");
      }
    }
  }

  const auto expected_surfaces = standard_surfaces(tokens);
  for (const auto& expected : expected_surfaces) {
    const auto* actual = find_surface(expected.surface);
    if (actual == nullptr) {
      add_violation(problems, "surface.missing",
                    "a required suite surface is missing");
      continue;
    }
    if (actual->stable_id != expected.stable_id ||
        actual->edge != expected.edge ||
        actual->visual_order != expected.visual_order ||
        actual->extent != expected.extent ||
        actual->focus_order != expected.focus_order ||
        actual->visibility != expected.visibility) {
      add_violation(
          problems, "surface.invariant",
          actual->stable_id +
              " changed placement, order, extent, visibility, or focus order");
    }
    if (actual->action_groups != expected.action_groups) {
      add_violation(problems, "group.invariant",
                    actual->stable_id +
                        " changed its shared action or focus sequence");
    }
  }

  std::set<std::string> slot_ids_seen;
  for (const auto& extension_slot : extension_slots_) {
    if (extension_slot.stable_id.empty() ||
        !slot_ids_seen.insert(extension_slot.stable_id).second) {
      add_violation(problems, "slot.id",
                    "extension slot IDs must be non-empty and unique");
    }
    if (find_surface(extension_slot.host) == nullptr) {
      add_violation(problems, "slot.host",
                    "extension slots must reference an existing surface");
    }
    if (extension_slot.may_change_host_extent) {
      add_violation(problems, "slot.extent",
                    "application contributions may not resize shared chrome");
    }
    for (const auto family : kEveryFamily) {
      if (!contains_family(extension_slot.supported_families, family)) {
        add_violation(problems, "slot.family",
                      "every shared slot must support Write, Grid, and Slides");
      }
    }
  }

  const auto expected_slots = standard_slots();
  for (const auto& expected : expected_slots) {
    const auto* actual = find_extension_slot(expected.stable_id);
    if (actual == nullptr) {
      add_violation(problems, "slot.missing",
                    "a required suite extension slot is missing");
      continue;
    }
    if (actual->host != expected.host ||
        actual->visual_order != expected.visual_order ||
        actual->may_change_host_extent) {
      add_violation(problems, "slot.invariant",
                    actual->stable_id + " changed host, order, or extent policy");
    }
  }

  const auto expected_responsive = standard_responsive(tokens);
  if (responsive_ != expected_responsive) {
    add_violation(problems, "responsive.invariant",
                  "responsive collapse and minimum-canvas rules changed");
  }
  return problems;
}

const SuiteShellContract& standard_suite_shell_contract() {
  const auto& tokens = default_suite_tokens();
  static const SuiteShellContract contract{
      1, standard_surfaces(tokens), standard_slots(),
      standard_responsive(tokens)};
  return contract;
}

}  // namespace luma::suite
