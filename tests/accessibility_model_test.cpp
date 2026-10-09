#include "accessibility/accessibility_model.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace accessibility = luma::write::accessibility;
namespace engine = luma::write::engine;

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

bool has_notification(
    const std::vector<accessibility::AccessibilityNotification>& notifications,
    const accessibility::AccessibilityNotificationKind kind) {
  return std::any_of(
      notifications.begin(), notifications.end(),
      [kind](const accessibility::AccessibilityNotification& notification) {
        return notification.kind == kind;
      });
}

const accessibility::AccessibilityNotification* find_notification(
    const std::vector<accessibility::AccessibilityNotification>& notifications,
    const accessibility::AccessibilityNotificationKind kind) {
  const auto found = std::find_if(
      notifications.begin(), notifications.end(),
      [kind](const accessibility::AccessibilityNotification& notification) {
        return notification.kind == kind;
      });
  return found == notifications.end() ? nullptr : &*found;
}

const accessibility::AccessibleSemanticNode* find_semantic(
    const accessibility::AccessibleDocumentState& state,
    const std::string& stable_id) {
  const auto found = std::find_if(
      state.nearby_semantics.begin(), state.nearby_semantics.end(),
      [&stable_id](const accessibility::AccessibleSemanticNode& node) {
        return node.stable_id == stable_id;
      });
  return found == state.nearby_semantics.end() ? nullptr : &*found;
}

void bounded_unicode_text_and_caret() {
  accessibility::AccessibilityLimits limits;
  limits.maximum_text_codepoints = 32;
  limits.context_before_caret = 8;
  accessibility::CanvasAccessibilityController controller(limits);

  engine::AccessibilitySnapshot snapshot;
  snapshot.focused_paragraph_utf8 = std::string(100, 'a');
  snapshot.caret_offset = 50;
  controller.apply_snapshot(snapshot);
  const auto& text = controller.state().text;
  CHECK(text.source_length == 100U);
  CHECK((text.source_range == accessibility::TextRange{42, 74}));
  CHECK(text.utf8 == std::string(32, 'a'));
  CHECK(text.caret_known);
  CHECK(text.caret_source_offset == 50U);
  CHECK(text.caret_window_offset == 8U);
  CHECK(text.truncated_before);
  CHECK(text.truncated_after);

  snapshot.focused_paragraph_utf8 = "A🙂é文Z";
  snapshot.caret_offset = 4;
  controller.apply_snapshot(snapshot);
  CHECK(controller.state().text.source_length == 5U);
  CHECK(controller.state().text.utf8 == "A🙂é文Z");
  CHECK(controller.state().text.caret_window_offset == 4U);
  CHECK(!controller.state().text.truncated_before);
  CHECK(!controller.state().text.truncated_after);

  snapshot.focused_paragraph_utf8 = std::string("ok\xFFtail", 7);
  snapshot.caret_offset = -1;
  controller.apply_snapshot(snapshot);
  CHECK(controller.state().text.utf8 == "ok�tail");
  CHECK(!controller.state().text.caret_known);
  CHECK(controller.state().text.caret_source_offset == 0U);
}

void selection_normalization_and_clipping() {
  accessibility::AccessibilityLimits limits;
  limits.maximum_text_codepoints = 32;
  limits.context_before_caret = 8;
  accessibility::CanvasAccessibilityController controller(limits);

  engine::AccessibilitySnapshot snapshot;
  snapshot.focused_paragraph_utf8 = std::string(100, 'x');
  snapshot.caret_offset = 90;
  accessibility::SnapshotContext context;
  context.selection = accessibility::SourceSelection{95, 60};
  controller.apply_snapshot(snapshot, context);

  const auto& text = controller.state().text;
  CHECK((text.source_range == accessibility::TextRange{68, 100}));
  CHECK(text.selection.has_value());
  if (text.selection.has_value()) {
    CHECK(text.selection->anchor == 95U);
    CHECK(text.selection->focus == 60U);
    CHECK(text.selection->reversed);
    CHECK((text.selection->source_range == accessibility::TextRange{60, 95}));
    CHECK((text.selection->window_range == accessibility::TextRange{0, 27}));
    CHECK(text.selection->clipped_before);
    CHECK(!text.selection->clipped_after);
  }
  CHECK(controller.state().coverage.selection_offsets_available);

  context.selection = accessibility::SourceSelection{1000, 1000};
  controller.apply_snapshot(snapshot, context);
  CHECK((controller.state().text.selection->source_range ==
         accessibility::TextRange{100, 100}));

  context.selection = accessibility::SourceSelection{-1, 10};
  controller.apply_snapshot(snapshot, context);
  CHECK(!controller.state().text.selection.has_value());
  CHECK(!controller.state().coverage.selection_offsets_available);
}

void document_state_and_semantic_projection() {
  accessibility::AccessibilityLimits limits;
  limits.maximum_text_codepoints = 32;
  limits.context_before_caret = 8;
  accessibility::CanvasAccessibilityController controller(limits);
  engine::AccessibilitySnapshot snapshot;
  snapshot.focused_paragraph_utf8 = std::string(120, 'd');
  snapshot.caret_offset = 50;
  snapshot.current_page = 9;
  snapshot.page_count = 3;
  snapshot.editable = false;

  const std::vector<accessibility::SourceSemanticNode> semantics{
      {.stable_id = "heading",
       .role = accessibility::SemanticRole::Heading,
       .name = "Financial outlook",
       .start_offset = 40,
       .end_offset = 60,
       .page_number = 3,
       .hierarchy_level = 9},
      {.stable_id = "list",
       .role = accessibility::SemanticRole::List,
       .name = "Risks",
       .start_offset = 60,
       .end_offset = 85,
       .page_number = 3,
       .hierarchy_level = 0},
      {.stable_id = "table",
       .role = accessibility::SemanticRole::Table,
       .name = "Revenue by region",
       .start_offset = 90,
       .end_offset = 110,
       .page_number = 3},
      {.stable_id = "image-with-alt",
       .role = accessibility::SemanticRole::Image,
       .name = "",
       .alt_text = "Bar chart: revenue rose in every region",
       .start_offset = 55,
       .end_offset = 55,
       .page_number = 3},
      {.stable_id = "image-without-alt",
       .role = accessibility::SemanticRole::Image,
       .name = "",
       .start_offset = 56,
       .end_offset = 56,
       .page_number = 3},
      {.stable_id = "comment",
       .role = accessibility::SemanticRole::Comment,
       .name = "Comment by Avery",
       .start_offset = 58,
       .end_offset = 64,
       .page_number = 3,
       .focused = true},
      {.stable_id = "not-nearby",
       .role = accessibility::SemanticRole::Paragraph,
       .name = "Other page",
       .start_offset = 115,
       .end_offset = 120,
       .page_number = 1},
  };
  accessibility::SnapshotContext context;
  context.document_name = "Quarterly Report";
  context.nearby_semantics = semantics;
  context.canvas_focused = true;
  controller.apply_snapshot(snapshot, context);

  const auto& state = controller.state();
  CHECK(state.name == "Quarterly Report");
  CHECK(state.current_page == 3);
  CHECK(state.page_count == 3);
  CHECK(!state.editable);
  CHECK(state.canvas_focused);
  CHECK(state.nearby_semantics.size() == 6U);
  CHECK(find_semantic(state, "not-nearby") == nullptr);
  CHECK(state.coverage.semantic_context_available);
  CHECK(!state.coverage.complete_at_spi_bridge);
  CHECK(!state.coverage.document_wide_text);
  CHECK(!state.coverage.character_geometry);

  const auto* heading = find_semantic(state, "heading");
  CHECK(heading != nullptr);
  if (heading != nullptr) {
    CHECK(heading->role == accessibility::SemanticRole::Heading);
    CHECK(heading->hierarchy_level == 6);
  }
  const auto* list = find_semantic(state, "list");
  CHECK(list != nullptr && list->hierarchy_level == 1);
  const auto* image_with_alt = find_semantic(state, "image-with-alt");
  CHECK(image_with_alt != nullptr);
  if (image_with_alt != nullptr) {
    CHECK(image_with_alt->alt_text ==
          std::optional<std::string>{"Bar chart: revenue rose in every region"});
    CHECK(image_with_alt->name ==
          "Bar chart: revenue rose in every region");
  }
  const auto* image_without_alt = find_semantic(state, "image-without-alt");
  CHECK(image_without_alt != nullptr);
  if (image_without_alt != nullptr) {
    CHECK(!image_without_alt->alt_text.has_value());
    CHECK(image_without_alt->name == "Image (no alternative text)");
  }
  CHECK(find_semantic(state, "comment") != nullptr);
}

void semantic_list_is_bounded() {
  accessibility::AccessibilityLimits limits;
  limits.maximum_semantic_nodes = 3;
  accessibility::CanvasAccessibilityController controller(limits);
  engine::AccessibilitySnapshot snapshot;
  snapshot.focused_paragraph_utf8 = "Nearby text";
  snapshot.caret_offset = 4;

  std::vector<accessibility::SourceSemanticNode> semantics;
  for (int index = 0; index < 10; ++index) {
    semantics.push_back({
        .stable_id = "node-" + std::to_string(index),
        .role = accessibility::SemanticRole::Comment,
        .name = "Comment " + std::to_string(index),
        .start_offset = index,
        .end_offset = index + 1,
        .page_number = 1,
        .focused = index == 9,
    });
  }
  accessibility::SnapshotContext context;
  context.nearby_semantics = semantics;
  controller.apply_snapshot(snapshot, context);
  CHECK(controller.state().nearby_semantics.size() == 3U);
  CHECK(controller.state().semantics_truncated);
  CHECK(find_semantic(controller.state(), "node-9") != nullptr);
}

void off_window_edits_still_emit_conservative_notifications() {
  accessibility::AccessibilityLimits limits;
  limits.maximum_text_codepoints = 32;
  limits.context_before_caret = 8;
  accessibility::CanvasAccessibilityController controller(limits);
  engine::AccessibilitySnapshot snapshot;
  snapshot.focused_paragraph_utf8 = std::string(100, 'x');
  snapshot.caret_offset = 4;
  controller.apply_snapshot(snapshot);
  (void)controller.drain_notifications();

  snapshot.focused_paragraph_utf8 += '!';
  controller.apply_snapshot(snapshot);
  const auto notifications = controller.drain_notifications();
  const auto* inserted = find_notification(
      notifications, accessibility::AccessibilityNotificationKind::TextInserted);
  CHECK(inserted != nullptr);
  if (inserted != nullptr) {
    CHECK(!inserted->exact);
    CHECK(inserted->text.empty());
  }
}

void exact_text_and_state_notifications() {
  accessibility::CanvasAccessibilityController controller;
  engine::AccessibilitySnapshot snapshot;
  snapshot.focused_paragraph_utf8 = "hello";
  snapshot.caret_offset = 5;
  accessibility::SnapshotContext context;
  context.document_name = "Draft";
  context.canvas_focused = true;
  controller.apply_snapshot(snapshot, context);
  auto notifications = controller.drain_notifications();
  CHECK(notifications.size() == 1U);
  CHECK(has_notification(notifications,
                         accessibility::AccessibilityNotificationKind::FocusEntered));

  snapshot.focused_paragraph_utf8 = "hello!";
  snapshot.caret_offset = 6;
  controller.apply_snapshot(snapshot, context);
  notifications = controller.drain_notifications();
  const auto* inserted = find_notification(
      notifications, accessibility::AccessibilityNotificationKind::TextInserted);
  CHECK(inserted != nullptr);
  if (inserted != nullptr) {
    CHECK((inserted->source_range == accessibility::TextRange{5, 6}));
    CHECK(inserted->text == "!");
    CHECK(inserted->exact);
    CHECK(!inserted->text_truncated);
  }
  CHECK(has_notification(notifications,
                         accessibility::AccessibilityNotificationKind::CaretMoved));

  snapshot.focused_paragraph_utf8 = "helo!";
  snapshot.caret_offset = 3;
  controller.apply_snapshot(snapshot, context);
  notifications = controller.drain_notifications();
  const auto* deleted = find_notification(
      notifications, accessibility::AccessibilityNotificationKind::TextDeleted);
  CHECK(deleted != nullptr);
  if (deleted != nullptr) {
    CHECK((deleted->source_range == accessibility::TextRange{3, 4}));
    CHECK(deleted->exact);
  }

  context.document_name = "Final";
  context.selection = accessibility::SourceSelection{0, 4};
  context.canvas_focused = false;
  snapshot.current_page = 2;
  snapshot.page_count = 2;
  snapshot.editable = false;
  controller.apply_snapshot(snapshot, context);
  notifications = controller.drain_notifications();
  CHECK(has_notification(notifications,
                         accessibility::AccessibilityNotificationKind::NameChanged));
  CHECK(has_notification(
      notifications, accessibility::AccessibilityNotificationKind::SelectionChanged));
  CHECK(has_notification(notifications,
                         accessibility::AccessibilityNotificationKind::PageChanged));
  CHECK(has_notification(
      notifications, accessibility::AccessibilityNotificationKind::EditableChanged));
  CHECK(has_notification(notifications,
                         accessibility::AccessibilityNotificationKind::FocusLeft));
}

void engine_events_and_notification_queue_are_bounded() {
  accessibility::AccessibilityLimits limits;
  limits.maximum_notification_codepoints = 4;
  limits.maximum_pending_notifications = 2;
  accessibility::CanvasAccessibilityController controller(limits);

  engine::EngineEvent event;
  event.kind = engine::EngineEventKind::accessibility_focus_changed;
  event.text = "a very long engine focus payload";
  CHECK(controller.handle_engine_event(event) ==
        accessibility::EngineEventResponse::RefreshAccessibilitySnapshot);
  CHECK(controller.refresh_requested());
  auto notifications = controller.drain_notifications();
  CHECK(notifications.size() == 1U);
  CHECK(notifications.front().kind ==
        accessibility::AccessibilityNotificationKind::SemanticFocusChanged);
  CHECK(notifications.front().text == "a ve");
  CHECK(notifications.front().text_truncated);
  CHECK(!notifications.front().exact);

  controller.clear_refresh_request();
  event.kind = engine::EngineEventKind::tiles_invalidated;
  CHECK(controller.handle_engine_event(event) ==
        accessibility::EngineEventResponse::Ignored);
  CHECK(!controller.refresh_requested());

  controller.set_canvas_focused(true);
  controller.set_canvas_focused(false);
  controller.set_canvas_focused(true);
  notifications = controller.drain_notifications();
  CHECK(notifications.size() == 2U);
  CHECK(controller.state().dropped_notification_count == 1U);
}

}  // namespace

int main() {
  bounded_unicode_text_and_caret();
  selection_normalization_and_clipping();
  document_state_and_semantic_projection();
  semantic_list_is_bounded();
  off_window_edits_still_emit_conservative_notifications();
  exact_text_and_state_notifications();
  engine_events_and_notification_queue_are_bounded();
  if (failures != 0) {
    std::cerr << failures << " accessibility model test(s) failed\n";
    return 1;
  }
  std::cout << "accessibility model tests passed\n";
  return 0;
}
