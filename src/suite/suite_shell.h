#pragma once

#include "suite/suite_identity.h"
#include "suite/suite_tokens.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luma::suite {

/** Stable shell surfaces shared by Write, Grid, and Slides. */
enum class ShellSurface {
  StudioBar,
  CommandBar,
  Navigator,
  Canvas,
  Inspector,
  StatusBar,
};

enum class ShellEdge {
  Top,
  Bottom,
  Left,
  Right,
  Center,
};

enum class VisibilityPolicy {
  Always,
  UserToggle,
  ResponsiveToggle,
};

/**
 * One stable group within a bar. Both visual and keyboard order are explicit;
 * applications may fill a group but may not reorder common groups.
 */
struct ActionGroupInvariant {
  std::string stable_id;
  int visual_order{0};
  int focus_order{0};

  bool operator==(const ActionGroupInvariant&) const = default;
};

/** Toolkit-neutral placement and sizing contract for a shell surface. */
struct SurfaceInvariant {
  ShellSurface surface{ShellSurface::Canvas};
  std::string stable_id;
  ShellEdge edge{ShellEdge::Center};
  int visual_order{0};
  std::optional<ExtentRange> extent;
  int focus_order{0};
  VisibilityPolicy visibility{VisibilityPolicy::Always};
  std::vector<ActionGroupInvariant> action_groups;

  bool operator==(const SurfaceInvariant&) const = default;
};

/**
 * A named injection seam for app-specific UI. Slots accept contributions from
 * every suite family while preserving their host surface's geometry.
 */
struct ExtensionSlotInvariant {
  std::string stable_id;
  ShellSurface host{ShellSurface::CommandBar};
  int visual_order{0};
  std::vector<ApplicationFamily> supported_families;
  bool may_change_host_extent{false};

  bool operator==(const ExtensionSlotInvariant&) const = default;
};

/** Responsive rules that keep the document/canvas useful at laptop sizes. */
struct ResponsiveInvariant {
  int compact_width{1280};
  int compact_height{720};
  int maximum_open_side_panels{1};
  int minimum_canvas_width{640};
  bool collapse_labels_before_canvas{true};

  bool operator==(const ResponsiveInvariant&) const = default;
};

struct ShellContractViolation {
  std::string code;
  std::string message;

  bool operator==(const ShellContractViolation&) const = default;
};

namespace surface_ids {
inline constexpr std::string_view kStudioBar = "suite.surface.studio-bar";
inline constexpr std::string_view kCommandBar = "suite.surface.command-bar";
inline constexpr std::string_view kNavigator = "suite.surface.navigator";
inline constexpr std::string_view kCanvas = "suite.surface.canvas";
inline constexpr std::string_view kInspector = "suite.surface.inspector";
inline constexpr std::string_view kStatusBar = "suite.surface.status-bar";
}  // namespace surface_ids

namespace action_group_ids {
inline constexpr std::string_view kDocumentIdentity =
    "suite.group.document-identity";
inline constexpr std::string_view kSaveState = "suite.group.save-state";
inline constexpr std::string_view kHistory = "suite.group.history";
inline constexpr std::string_view kCommandSearch =
    "suite.group.command-search";
inline constexpr std::string_view kView = "suite.group.view";
inline constexpr std::string_view kWindow = "suite.group.window";
inline constexpr std::string_view kEssentialPrimary =
    "suite.group.essential-primary";
inline constexpr std::string_view kEssentialSecondary =
    "suite.group.essential-secondary";
inline constexpr std::string_view kContextual = "suite.group.contextual";
inline constexpr std::string_view kLocation = "suite.group.location";
inline constexpr std::string_view kDocumentMetrics =
    "suite.group.document-metrics";
inline constexpr std::string_view kLanguage = "suite.group.language";
inline constexpr std::string_view kFormat = "suite.group.format";
inline constexpr std::string_view kDocumentHealth =
    "suite.group.document-health";
inline constexpr std::string_view kZoom = "suite.group.zoom";
}  // namespace action_group_ids

namespace extension_slot_ids {
inline constexpr std::string_view kStudioApplicationActions =
    "suite.slot.studio.application-actions";
inline constexpr std::string_view kEssentialPrimary =
    "suite.slot.command.essential-primary";
inline constexpr std::string_view kEssentialSecondary =
    "suite.slot.command.essential-secondary";
inline constexpr std::string_view kContextualPrimary =
    "suite.slot.command.contextual-primary";
inline constexpr std::string_view kContextualSecondary =
    "suite.slot.command.contextual-secondary";
inline constexpr std::string_view kNavigatorContent =
    "suite.slot.navigator.content";
inline constexpr std::string_view kInspectorContent =
    "suite.slot.inspector.content";
inline constexpr std::string_view kStatusApplicationIndicators =
    "suite.slot.status.application-indicators";
}  // namespace extension_slot_ids

/**
 * Versioned invariant contract for spatial familiarity across the suite.
 *
 * Changing a bar edge, height, common group order, or focus order is a suite
 * migration and must increment the contract version in every application.
 */
class SuiteShellContract {
 public:
  SuiteShellContract(std::uint32_t version,
                     std::vector<SurfaceInvariant> surfaces,
                     std::vector<ExtensionSlotInvariant> extension_slots,
                     ResponsiveInvariant responsive);

  [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
  [[nodiscard]] const std::vector<SurfaceInvariant>& surfaces() const noexcept {
    return surfaces_;
  }
  [[nodiscard]] const std::vector<ExtensionSlotInvariant>& extension_slots()
      const noexcept {
    return extension_slots_;
  }
  [[nodiscard]] const ResponsiveInvariant& responsive() const noexcept {
    return responsive_;
  }

  [[nodiscard]] const SurfaceInvariant* find_surface(
      ShellSurface surface) const noexcept;
  [[nodiscard]] const ExtensionSlotInvariant* find_extension_slot(
      std::string_view stable_id) const noexcept;
  [[nodiscard]] bool accepts(std::string_view slot_id,
                             ApplicationFamily family) const noexcept;

  /** Returns focusable surfaces in the suite-wide keyboard traversal order. */
  [[nodiscard]] std::vector<ShellSurface> focus_sequence() const;

  /** Returns all structural and suite-familiarity violations. */
  [[nodiscard]] std::vector<ShellContractViolation> validate(
      const SuiteTokens& tokens = default_suite_tokens()) const;

 private:
  std::uint32_t version_{1};
  std::vector<SurfaceInvariant> surfaces_;
  std::vector<ExtensionSlotInvariant> extension_slots_;
  ResponsiveInvariant responsive_;
};

/** Canonical v1 shell contract. */
const SuiteShellContract& standard_suite_shell_contract();

}  // namespace luma::suite
