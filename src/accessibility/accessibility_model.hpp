#pragma once

#include "engine/engine_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luma::write::accessibility {

/**
 * Safety and memory bounds for the canvas accessibility projection.
 *
 * This module is not a complete AT-SPI implementation. It retains a nearby
 * text/semantic window and emits toolkit-neutral changes. A GTK adapter still
 * has to publish these properties and notifications through the GTK version
 * available on Fedora, and document-wide navigation must remain engine-backed.
 */
struct AccessibilityLimits {
  std::size_t maximum_text_codepoints{4096};
  std::size_t context_before_caret{1536};
  std::size_t maximum_semantic_nodes{64};
  std::size_t maximum_label_codepoints{256};
  std::size_t maximum_notification_codepoints{512};
  std::size_t maximum_pending_notifications{128};
};

struct SourceSelection {
  // Offsets use Unicode scalar values in the focused paragraph. Negative values
  // mean the engine did not provide a usable selection offset.
  std::int64_t anchor_offset{-1};
  std::int64_t focus_offset{-1};

  friend constexpr bool operator==(const SourceSelection&,
                                   const SourceSelection&) = default;
};

struct TextRange {
  std::size_t start{0};
  std::size_t end{0};

  [[nodiscard]] constexpr bool empty() const noexcept { return start == end; }
  [[nodiscard]] constexpr std::size_t length() const noexcept {
    return end >= start ? end - start : 0;
  }

  friend constexpr bool operator==(const TextRange&, const TextRange&) = default;
};

struct NormalizedSelection {
  std::size_t anchor{0};
  std::size_t focus{0};
  TextRange source_range;
  TextRange window_range;
  bool reversed{false};
  bool clipped_before{false};
  bool clipped_after{false};

  friend constexpr bool operator==(const NormalizedSelection&,
                                   const NormalizedSelection&) = default;
};

struct AccessibleTextWindow {
  std::string utf8;
  TextRange source_range;
  std::size_t source_length{0};
  std::size_t caret_source_offset{0};
  std::size_t caret_window_offset{0};
  bool caret_known{false};
  bool truncated_before{false};
  bool truncated_after{false};
  std::optional<NormalizedSelection> selection;

  friend bool operator==(const AccessibleTextWindow&,
                         const AccessibleTextWindow&) = default;
};

enum class SemanticRole {
  Paragraph,
  Heading,
  List,
  ListItem,
  Table,
  TableRow,
  TableCell,
  Image,
  Comment,
};

struct SourceSemanticNode {
  std::string stable_id;
  SemanticRole role{SemanticRole::Paragraph};
  std::string name;
  std::optional<std::string> alt_text;
  std::int64_t start_offset{0};
  std::int64_t end_offset{0};
  int page_number{0};
  int hierarchy_level{0};
  bool focused{false};

  friend bool operator==(const SourceSemanticNode&,
                         const SourceSemanticNode&) = default;
};

struct AccessibleSemanticNode {
  std::string stable_id;
  SemanticRole role{SemanticRole::Paragraph};
  std::string name;
  std::optional<std::string> alt_text;
  TextRange source_range;
  int page_number{0};
  int hierarchy_level{0};
  bool focused{false};
  bool range_clamped{false};

  friend bool operator==(const AccessibleSemanticNode&,
                         const AccessibleSemanticNode&) = default;
};

struct SnapshotContext {
  std::string document_name{"Untitled document"};
  std::optional<SourceSelection> selection;
  std::span<const SourceSemanticNode> nearby_semantics;
  bool canvas_focused{false};
};

struct AccessibilityCoverage {
  // These flags are deliberately conservative. Consumers must not advertise a
  // complete document tree or complete AT-SPI text support from this model.
  bool complete_at_spi_bridge{false};
  bool document_wide_text{false};
  bool character_geometry{false};
  bool selection_offsets_available{false};
  bool semantic_context_available{false};

  friend constexpr bool operator==(const AccessibilityCoverage&,
                                   const AccessibilityCoverage&) = default;
};

struct AccessibleDocumentState {
  std::string name{"Untitled document"};
  AccessibleTextWindow text;
  std::vector<AccessibleSemanticNode> nearby_semantics;
  int current_page{1};
  int page_count{1};
  bool editable{true};
  bool canvas_focused{false};
  bool semantics_truncated{false};
  std::size_t dropped_notification_count{0};
  std::uint64_t generation{0};
  AccessibilityCoverage coverage;

  friend bool operator==(const AccessibleDocumentState&,
                         const AccessibleDocumentState&) = default;
};

enum class AccessibilityNotificationKind {
  TextInserted,
  TextDeleted,
  CaretMoved,
  SelectionChanged,
  FocusEntered,
  FocusLeft,
  SemanticFocusChanged,
  PageChanged,
  EditableChanged,
  NameChanged,
  SemanticContextChanged,
};

struct AccessibilityNotification {
  AccessibilityNotificationKind kind{
      AccessibilityNotificationKind::SemanticContextChanged};
  TextRange source_range;
  std::string text;
  bool text_truncated{false};
  bool exact{true};

  friend bool operator==(const AccessibilityNotification&,
                         const AccessibilityNotification&) = default;
};

enum class EngineEventResponse {
  Ignored,
  RefreshAccessibilitySnapshot,
};

/**
 * Bounded controller between DocumentEngine snapshots/events and a future GTK
 * canvas accessibility adapter. All retained text and notification queues are
 * bounded. The controller does not call GTK and does not claim to expose AT-SPI
 * objects by itself.
 */
class CanvasAccessibilityController {
 public:
  explicit CanvasAccessibilityController(AccessibilityLimits limits = {});

  void apply_snapshot(const engine::AccessibilitySnapshot& snapshot,
                      const SnapshotContext& context = {});
  [[nodiscard]] EngineEventResponse handle_engine_event(
      const engine::EngineEvent& event);
  void set_canvas_focused(bool focused);

  [[nodiscard]] const AccessibleDocumentState& state() const noexcept {
    return state_;
  }
  [[nodiscard]] const AccessibilityLimits& limits() const noexcept {
    return limits_;
  }
  [[nodiscard]] bool refresh_requested() const noexcept {
    return refresh_requested_;
  }
  void clear_refresh_request() noexcept { refresh_requested_ = false; }
  [[nodiscard]] std::vector<AccessibilityNotification> drain_notifications();

 private:
  void push_notification(AccessibilityNotification notification);

  AccessibilityLimits limits_;
  AccessibleDocumentState state_;
  std::vector<AccessibilityNotification> pending_notifications_;
  std::uint64_t source_fingerprint_{0};
  bool initialized_{false};
  bool refresh_requested_{false};
};

[[nodiscard]] std::string_view semantic_role_name(SemanticRole role) noexcept;

}  // namespace luma::write::accessibility
