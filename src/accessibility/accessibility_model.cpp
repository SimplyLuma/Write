#include "accessibility_model.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <tuple>
#include <utility>

namespace luma::write::accessibility {
namespace {

constexpr char32_t kReplacementCharacter = U'\uFFFD';
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct DecodedCharacter {
  char32_t value{kReplacementCharacter};
  std::size_t bytes{1};
};

struct TextProjection {
  AccessibleTextWindow window;
  std::vector<char32_t> codepoints;
  std::uint64_t fingerprint{kFnvOffset};
};

bool continuation(const unsigned char value) {
  return (value & 0xC0U) == 0x80U;
}

DecodedCharacter decode_next(const std::string_view utf8,
                             const std::size_t offset) {
  const auto first = static_cast<unsigned char>(utf8[offset]);
  if (first <= 0x7FU) {
    return {static_cast<char32_t>(first), 1};
  }
  if (first >= 0xC2U && first <= 0xDFU && offset + 1U < utf8.size()) {
    const auto second = static_cast<unsigned char>(utf8[offset + 1U]);
    if (continuation(second)) {
      return {static_cast<char32_t>(((first & 0x1FU) << 6U) | (second & 0x3FU)),
              2};
    }
  }
  if (first >= 0xE0U && first <= 0xEFU && offset + 2U < utf8.size()) {
    const auto second = static_cast<unsigned char>(utf8[offset + 1U]);
    const auto third = static_cast<unsigned char>(utf8[offset + 2U]);
    const bool valid_second =
        (first == 0xE0U && second >= 0xA0U && second <= 0xBFU) ||
        (first == 0xEDU && second >= 0x80U && second <= 0x9FU) ||
        ((first >= 0xE1U && first <= 0xECU) && continuation(second)) ||
        ((first >= 0xEEU && first <= 0xEFU) && continuation(second));
    if (valid_second && continuation(third)) {
      return {static_cast<char32_t>(((first & 0x0FU) << 12U) |
                                   ((second & 0x3FU) << 6U) |
                                   (third & 0x3FU)),
              3};
    }
  }
  if (first >= 0xF0U && first <= 0xF4U && offset + 3U < utf8.size()) {
    const auto second = static_cast<unsigned char>(utf8[offset + 1U]);
    const auto third = static_cast<unsigned char>(utf8[offset + 2U]);
    const auto fourth = static_cast<unsigned char>(utf8[offset + 3U]);
    const bool valid_second =
        (first == 0xF0U && second >= 0x90U && second <= 0xBFU) ||
        ((first >= 0xF1U && first <= 0xF3U) && continuation(second)) ||
        (first == 0xF4U && second >= 0x80U && second <= 0x8FU);
    if (valid_second && continuation(third) && continuation(fourth)) {
      return {static_cast<char32_t>(((first & 0x07U) << 18U) |
                                   ((second & 0x3FU) << 12U) |
                                   ((third & 0x3FU) << 6U) |
                                   (fourth & 0x3FU)),
              4};
    }
  }
  return {};
}

void append_utf8(std::string& output, const char32_t value) {
  if (value <= 0x7FU) {
    output.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    output.push_back(static_cast<char>(0xC0U | ((value >> 6U) & 0x1FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    output.push_back(static_cast<char>(0xE0U | ((value >> 12U) & 0x0FU)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    output.push_back(static_cast<char>(0xF0U | ((value >> 18U) & 0x07U)));
    output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

std::string encode_utf8(const std::span<const char32_t> values) {
  std::string output;
  output.reserve(values.size());
  for (const char32_t value : values) {
    append_utf8(output, value);
  }
  return output;
}

void hash_codepoint(std::uint64_t& hash, const char32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    hash ^= static_cast<std::uint64_t>((value >> shift) & 0xFFU);
    hash *= kFnvPrime;
  }
}

std::vector<char32_t> decode_bounded(const std::string_view utf8,
                                    const std::size_t maximum) {
  std::vector<char32_t> result;
  result.reserve(std::min(maximum, utf8.size()));
  for (std::size_t offset = 0; offset < utf8.size() && result.size() < maximum;) {
    const auto decoded = decode_next(utf8, offset);
    result.push_back(decoded.value);
    offset += decoded.bytes;
  }
  return result;
}

std::string bounded_utf8(const std::string_view source,
                         const std::size_t maximum_codepoints,
                         bool* truncated = nullptr) {
  std::string result;
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < source.size() && count < maximum_codepoints) {
    const auto decoded = decode_next(source, offset);
    append_utf8(result, decoded.value);
    offset += decoded.bytes;
    ++count;
  }
  if (truncated != nullptr) {
    *truncated = offset < source.size();
  }
  return result;
}

std::size_t clamp_offset(const std::int64_t value,
                         const std::size_t source_length) {
  if (value <= 0) {
    return 0;
  }
  const auto unsigned_value = static_cast<std::uint64_t>(value);
  if (unsigned_value >= source_length) {
    return source_length;
  }
  return static_cast<std::size_t>(unsigned_value);
}

NormalizedSelection normalize_selection(const SourceSelection& source,
                                        const AccessibleTextWindow& window) {
  NormalizedSelection selection;
  selection.anchor = clamp_offset(source.anchor_offset, window.source_length);
  selection.focus = clamp_offset(source.focus_offset, window.source_length);
  selection.reversed = selection.anchor > selection.focus;
  selection.source_range = {
      std::min(selection.anchor, selection.focus),
      std::max(selection.anchor, selection.focus),
  };
  selection.clipped_before =
      selection.source_range.start < window.source_range.start;
  selection.clipped_after = selection.source_range.end > window.source_range.end;
  selection.window_range = {
      std::clamp(selection.source_range.start, window.source_range.start,
                 window.source_range.end) -
          window.source_range.start,
      std::clamp(selection.source_range.end, window.source_range.start,
                 window.source_range.end) -
          window.source_range.start,
  };
  return selection;
}

TextProjection project_text(const std::string_view source,
                            const int source_caret,
                            const std::optional<SourceSelection>& selection,
                            const AccessibilityLimits& limits) {
  TextProjection result;
  const bool caret_known = source_caret >= 0;
  const std::size_t requested_caret =
      caret_known ? static_cast<std::size_t>(source_caret) : 0U;
  const std::size_t requested_start =
      requested_caret > limits.context_before_caret
          ? requested_caret - limits.context_before_caret
          : 0U;

  std::vector<char32_t> primary;
  primary.reserve(limits.maximum_text_codepoints);
  std::deque<char32_t> tail;
  std::size_t source_length = 0;
  for (std::size_t offset = 0; offset < source.size();) {
    const auto decoded = decode_next(source, offset);
    hash_codepoint(result.fingerprint, decoded.value);
    if (source_length >= requested_start &&
        primary.size() < limits.maximum_text_codepoints) {
      primary.push_back(decoded.value);
    }
    tail.push_back(decoded.value);
    if (tail.size() > limits.maximum_text_codepoints) {
      tail.pop_front();
    }
    ++source_length;
    offset += decoded.bytes;
  }
  hash_codepoint(result.fingerprint,
                 static_cast<char32_t>(source_length & 0xFFFFFFFFU));

  const std::size_t caret = std::min(requested_caret, source_length);
  std::size_t window_start = requested_start;
  if (source_length <= limits.maximum_text_codepoints) {
    primary.assign(tail.begin(), tail.end());
    window_start = 0;
  } else if (requested_start >= source_length ||
             primary.size() < limits.maximum_text_codepoints) {
    primary.assign(tail.begin(), tail.end());
    window_start = source_length - primary.size();
  }

  result.codepoints = std::move(primary);
  result.window.source_range = {
      window_start,
      window_start + result.codepoints.size(),
  };
  result.window.source_length = source_length;
  result.window.caret_known = caret_known;
  result.window.caret_source_offset = caret;
  result.window.caret_window_offset =
      std::clamp(caret, result.window.source_range.start,
                 result.window.source_range.end) -
      result.window.source_range.start;
  result.window.truncated_before = result.window.source_range.start != 0;
  result.window.truncated_after =
      result.window.source_range.end != result.window.source_length;
  result.window.utf8 = encode_utf8(result.codepoints);
  if (selection.has_value() && selection->anchor_offset >= 0 &&
      selection->focus_offset >= 0) {
    result.window.selection = normalize_selection(*selection, result.window);
  }
  return result;
}

AccessibilityLimits normalize_limits(AccessibilityLimits limits) {
  limits.maximum_text_codepoints =
      std::clamp(limits.maximum_text_codepoints, std::size_t{32},
                 std::size_t{65536});
  limits.context_before_caret =
      std::min(limits.context_before_caret,
               limits.maximum_text_codepoints - 1U);
  limits.maximum_semantic_nodes =
      std::clamp(limits.maximum_semantic_nodes, std::size_t{1},
                 std::size_t{1024});
  limits.maximum_label_codepoints =
      std::clamp(limits.maximum_label_codepoints, std::size_t{16},
                 std::size_t{4096});
  limits.maximum_notification_codepoints =
      std::clamp(limits.maximum_notification_codepoints, std::size_t{1},
                 std::size_t{4096});
  limits.maximum_pending_notifications =
      std::clamp(limits.maximum_pending_notifications, std::size_t{1},
                 std::size_t{1024});
  return limits;
}

std::string fallback_name(const SemanticRole role) {
  switch (role) {
    case SemanticRole::Paragraph:
      return "Paragraph";
    case SemanticRole::Heading:
      return "Heading";
    case SemanticRole::List:
      return "List";
    case SemanticRole::ListItem:
      return "List item";
    case SemanticRole::Table:
      return "Table";
    case SemanticRole::TableRow:
      return "Table row";
    case SemanticRole::TableCell:
      return "Table cell";
    case SemanticRole::Image:
      return "Image (no alternative text)";
    case SemanticRole::Comment:
      return "Comment";
  }
  return "Document item";
}

bool range_is_nearby(const TextRange range, const TextRange window) {
  if (range.empty()) {
    return range.start >= window.start && range.start <= window.end;
  }
  return range.start < window.end && range.end > window.start;
}

std::vector<AccessibleSemanticNode> project_semantics(
    const std::span<const SourceSemanticNode> source_nodes,
    const AccessibleTextWindow& text, const int current_page,
    const int page_count, const AccessibilityLimits& limits,
    bool& truncated) {
  std::vector<AccessibleSemanticNode> candidates;
  candidates.reserve(std::min(source_nodes.size(), limits.maximum_semantic_nodes));
  for (const SourceSemanticNode& source : source_nodes) {
    const std::int64_t raw_start = std::min(source.start_offset, source.end_offset);
    const std::int64_t raw_end = std::max(source.start_offset, source.end_offset);
    const std::size_t start = clamp_offset(raw_start, text.source_length);
    const std::size_t end = clamp_offset(raw_end, text.source_length);
    const TextRange range{start, std::max(start, end)};
    const int normalized_page =
        source.page_number <= 0 ? 0 : std::clamp(source.page_number, 1, page_count);
    if (!range_is_nearby(range, text.source_range) &&
        normalized_page != current_page) {
      continue;
    }

    AccessibleSemanticNode node;
    node.role = source.role;
    node.source_range = range;
    node.page_number = normalized_page;
    node.range_clamped = source.start_offset < 0 || source.end_offset < 0 ||
                         source.start_offset > source.end_offset ||
                         static_cast<std::uint64_t>(
                             std::max<std::int64_t>(raw_start, 0)) >
                             text.source_length ||
                         static_cast<std::uint64_t>(
                             std::max<std::int64_t>(raw_end, 0)) >
                             text.source_length;
    node.hierarchy_level = source.hierarchy_level;
    if (source.role == SemanticRole::Heading) {
      node.hierarchy_level = std::clamp(source.hierarchy_level, 1, 6);
    } else if (source.role == SemanticRole::List ||
               source.role == SemanticRole::ListItem) {
      node.hierarchy_level = std::max(source.hierarchy_level, 1);
    } else {
      node.hierarchy_level = 0;
    }
    node.focused = source.focused;
    node.stable_id = bounded_utf8(source.stable_id,
                                  limits.maximum_label_codepoints);
    if (node.stable_id.empty()) {
      node.stable_id = "semantic:" + std::string(semantic_role_name(source.role)) +
                       ":" + std::to_string(range.start);
    }

    if (source.alt_text.has_value()) {
      std::string alt =
          bounded_utf8(*source.alt_text, limits.maximum_label_codepoints);
      if (!alt.empty()) {
        node.alt_text = std::move(alt);
      }
    }
    node.name = bounded_utf8(source.name, limits.maximum_label_codepoints);
    if (node.role == SemanticRole::Image && node.name.empty() &&
        node.alt_text.has_value()) {
      node.name = *node.alt_text;
    }
    if (node.name.empty()) {
      node.name = fallback_name(node.role);
    }
    const auto priority = [](const AccessibleSemanticNode& item) {
      return std::tuple{!item.focused, item.source_range.start,
                        item.source_range.end, item.role, item.stable_id};
    };
    if (candidates.size() < limits.maximum_semantic_nodes) {
      candidates.push_back(std::move(node));
    } else {
      truncated = true;
      const auto latest = std::max_element(
          candidates.begin(), candidates.end(),
          [&priority](const AccessibleSemanticNode& left,
                      const AccessibleSemanticNode& right) {
            return priority(left) < priority(right);
          });
      if (latest != candidates.end() && priority(node) < priority(*latest)) {
        *latest = std::move(node);
      }
    }
  }

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const AccessibleSemanticNode& left,
                      const AccessibleSemanticNode& right) {
                     return std::tie(left.source_range.start, left.source_range.end,
                                     left.role, left.stable_id) <
                            std::tie(right.source_range.start, right.source_range.end,
                                     right.role, right.stable_id);
                   });
  return candidates;
}

std::vector<AccessibilityNotification> conservative_text_change_notifications(
    const AccessibleTextWindow& previous, const AccessibleTextWindow& next) {
  std::vector<AccessibilityNotification> notifications;
  if (previous.source_length > next.source_length) {
    const std::size_t removed = previous.source_length - next.source_length;
    const std::size_t start =
        std::min(next.caret_source_offset, previous.source_length);
    notifications.push_back({
        .kind = AccessibilityNotificationKind::TextDeleted,
        .source_range = {start,
                         std::min(previous.source_length, start + removed)},
        .exact = false,
    });
  } else if (next.source_length > previous.source_length) {
    const std::size_t inserted = next.source_length - previous.source_length;
    std::size_t end = std::min(next.caret_source_offset, next.source_length);
    std::size_t start = end >= inserted ? end - inserted : 0U;
    if (end - start < inserted) {
      end = std::min(next.source_length, start + inserted);
    }
    notifications.push_back({
        .kind = AccessibilityNotificationKind::TextInserted,
        .source_range = {start, end},
        .exact = false,
    });
  } else if (next.source_length > 0U) {
    const std::size_t start =
        std::min(next.caret_source_offset, next.source_length - 1U);
    notifications.push_back({
        .kind = AccessibilityNotificationKind::TextDeleted,
        .source_range = {start, start + 1U},
        .exact = false,
    });
    notifications.push_back({
        .kind = AccessibilityNotificationKind::TextInserted,
        .source_range = {start, start + 1U},
        .exact = false,
    });
  }
  return notifications;
}

std::vector<AccessibilityNotification> text_change_notifications(
    const AccessibleTextWindow& previous, const std::uint64_t previous_fingerprint,
    const TextProjection& next, const AccessibilityLimits& limits) {
  if (previous_fingerprint == next.fingerprint) {
    return {};
  }

  std::vector<AccessibilityNotification> notifications;
  const auto old_values =
      decode_bounded(previous.utf8, limits.maximum_text_codepoints);
  const auto& new_values = next.codepoints;
  if (previous.source_range.start == next.window.source_range.start) {
    std::size_t prefix = 0;
    while (prefix < old_values.size() && prefix < new_values.size() &&
           old_values[prefix] == new_values[prefix]) {
      ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < old_values.size() - prefix &&
           suffix < new_values.size() - prefix &&
           old_values[old_values.size() - 1U - suffix] ==
               new_values[new_values.size() - 1U - suffix]) {
      ++suffix;
    }
    const std::size_t absolute_start = previous.source_range.start + prefix;
    const std::size_t old_end =
        previous.source_range.start + old_values.size() - suffix;
    const std::size_t new_end =
        next.window.source_range.start + new_values.size() - suffix;
    const bool exact = !previous.truncated_before && !previous.truncated_after &&
                       !next.window.truncated_before &&
                       !next.window.truncated_after;
    if (old_end > absolute_start) {
      notifications.push_back({
          .kind = AccessibilityNotificationKind::TextDeleted,
          .source_range = {absolute_start, old_end},
          .exact = exact,
      });
    }
    if (new_end > absolute_start) {
      const std::size_t inserted_count = new_end - absolute_start;
      const std::size_t retained_count =
          std::min(inserted_count, limits.maximum_notification_codepoints);
      notifications.push_back({
          .kind = AccessibilityNotificationKind::TextInserted,
          .source_range = {absolute_start, new_end},
          .text = encode_utf8(std::span(new_values).subspan(prefix, retained_count)),
          .text_truncated = retained_count != inserted_count,
          .exact = exact,
      });
    }
    if (notifications.empty()) {
      return conservative_text_change_notifications(previous, next.window);
    }
    return notifications;
  }

  // A caret move can slide the retained window. If text also changed, the
  // bounded projection cannot prove the exact edit location. Emit a conservative
  // notification and require the GTK bridge to refresh exposed text.
  return conservative_text_change_notifications(previous, next.window);
}

bool selection_changed(const std::optional<NormalizedSelection>& left,
                       const std::optional<NormalizedSelection>& right) {
  return left != right;
}

}  // namespace

CanvasAccessibilityController::CanvasAccessibilityController(
    AccessibilityLimits limits)
    : limits_(normalize_limits(limits)) {
  pending_notifications_.reserve(limits_.maximum_pending_notifications);
}

void CanvasAccessibilityController::apply_snapshot(
    const engine::AccessibilitySnapshot& snapshot,
    const SnapshotContext& context) {
  const auto projection =
      project_text(snapshot.focused_paragraph_utf8, snapshot.caret_offset,
                   context.selection, limits_);
  const int page_count = std::max(snapshot.page_count, 1);
  const int current_page = std::clamp(snapshot.current_page, 1, page_count);
  bool semantics_truncated = false;
  auto semantics = project_semantics(
      context.nearby_semantics, projection.window, current_page, page_count,
      limits_, semantics_truncated);
  std::string name = bounded_utf8(context.document_name,
                                  limits_.maximum_label_codepoints);
  if (name.empty()) {
    name = "Untitled document";
  }

  if (initialized_) {
    for (auto notification : text_change_notifications(
             state_.text, source_fingerprint_, projection, limits_)) {
      push_notification(std::move(notification));
    }
    if (state_.text.caret_known != projection.window.caret_known ||
        state_.text.caret_source_offset !=
            projection.window.caret_source_offset) {
      push_notification({
          .kind = AccessibilityNotificationKind::CaretMoved,
          .source_range = {projection.window.caret_source_offset,
                           projection.window.caret_source_offset},
          .exact = projection.window.caret_known,
      });
    }
    if (selection_changed(state_.text.selection, projection.window.selection)) {
      const TextRange range = projection.window.selection.has_value()
                                  ? projection.window.selection->source_range
                                  : TextRange{projection.window.caret_source_offset,
                                              projection.window.caret_source_offset};
      push_notification({
          .kind = AccessibilityNotificationKind::SelectionChanged,
          .source_range = range,
          .exact = context.selection.has_value(),
      });
    }
    if (state_.current_page != current_page || state_.page_count != page_count) {
      push_notification({
          .kind = AccessibilityNotificationKind::PageChanged,
          .source_range = projection.window.source_range,
      });
    }
    if (state_.editable != snapshot.editable) {
      push_notification({
          .kind = AccessibilityNotificationKind::EditableChanged,
          .source_range = projection.window.source_range,
      });
    }
    if (state_.name != name) {
      push_notification({
          .kind = AccessibilityNotificationKind::NameChanged,
          .text = name,
      });
    }
    if (state_.nearby_semantics != semantics ||
        state_.semantics_truncated != semantics_truncated) {
      push_notification({
          .kind = AccessibilityNotificationKind::SemanticContextChanged,
          .source_range = projection.window.source_range,
      });
    }
    if (state_.canvas_focused != context.canvas_focused) {
      push_notification({
          .kind = context.canvas_focused
                      ? AccessibilityNotificationKind::FocusEntered
                      : AccessibilityNotificationKind::FocusLeft,
          .source_range = projection.window.source_range,
      });
    }
  } else if (context.canvas_focused) {
    push_notification({
        .kind = AccessibilityNotificationKind::FocusEntered,
        .source_range = projection.window.source_range,
    });
  }

  state_.name = std::move(name);
  state_.text = projection.window;
  state_.nearby_semantics = std::move(semantics);
  state_.current_page = current_page;
  state_.page_count = page_count;
  state_.editable = snapshot.editable;
  state_.canvas_focused = context.canvas_focused;
  state_.semantics_truncated = semantics_truncated;
  state_.coverage = {
      .complete_at_spi_bridge = false,
      .document_wide_text = false,
      .character_geometry = false,
      .selection_offsets_available = state_.text.selection.has_value(),
      .semantic_context_available = !context.nearby_semantics.empty(),
  };
  ++state_.generation;
  source_fingerprint_ = projection.fingerprint;
  initialized_ = true;
  refresh_requested_ = false;
}

EngineEventResponse CanvasAccessibilityController::handle_engine_event(
    const engine::EngineEvent& event) {
  switch (event.kind) {
    case engine::EngineEventKind::cursor_changed:
    case engine::EngineEventKind::text_selection_changed:
    case engine::EngineEventKind::graphic_selection_changed:
    case engine::EngineEventKind::document_extent_changed:
    case engine::EngineEventKind::context_changed:
    case engine::EngineEventKind::comments_changed:
    case engine::EngineEventKind::table_selection_changed:
      refresh_requested_ = true;
      return EngineEventResponse::RefreshAccessibilitySnapshot;
    case engine::EngineEventKind::accessibility_focus_changed: {
      bool truncated = false;
      push_notification({
          .kind = AccessibilityNotificationKind::SemanticFocusChanged,
          .source_range = state_.text.source_range,
          .text = bounded_utf8(event.text, limits_.maximum_notification_codepoints,
                               &truncated),
          .text_truncated = truncated,
          .exact = false,
      });
      refresh_requested_ = true;
      return EngineEventResponse::RefreshAccessibilitySnapshot;
    }
    // A search moves the caret, so the snapshot has to catch up. The count
    // itself is announced by the find bar's own live region rather than here:
    // this controller describes the document, and "eleven matches" is a fact
    // about the search, not about the text.
    case engine::EngineEventKind::search_result:
    case engine::EngineEventKind::search_not_found:
      refresh_requested_ = true;
      return EngineEventResponse::RefreshAccessibilitySnapshot;
    case engine::EngineEventKind::tiles_invalidated:
    case engine::EngineEventKind::cursor_visibility_changed:
    case engine::EngineEventKind::document_modified:
    case engine::EngineEventKind::command_state_changed:
    case engine::EngineEventKind::command_completed:
    case engine::EngineEventKind::tracked_changes_changed:
    case engine::EngineEventKind::missing_fonts_changed:
    case engine::EngineEventKind::dialog_opened:
    case engine::EngineEventKind::context_menu:
    case engine::EngineEventKind::password_requested:
    case engine::EngineEventKind::hyperlink_activated:
    case engine::EngineEventKind::progress:
    case engine::EngineEventKind::error:
      return EngineEventResponse::Ignored;
  }
  return EngineEventResponse::Ignored;
}

void CanvasAccessibilityController::set_canvas_focused(const bool focused) {
  if (state_.canvas_focused == focused) {
    return;
  }
  state_.canvas_focused = focused;
  ++state_.generation;
  push_notification({
      .kind = focused ? AccessibilityNotificationKind::FocusEntered
                      : AccessibilityNotificationKind::FocusLeft,
      .source_range = state_.text.source_range,
  });
}

void CanvasAccessibilityController::push_notification(
    AccessibilityNotification notification) {
  if (pending_notifications_.size() >= limits_.maximum_pending_notifications) {
    pending_notifications_.erase(pending_notifications_.begin());
    ++state_.dropped_notification_count;
  }
  pending_notifications_.push_back(std::move(notification));
}

std::vector<AccessibilityNotification>
CanvasAccessibilityController::drain_notifications() {
  std::vector<AccessibilityNotification> result;
  result.swap(pending_notifications_);
  pending_notifications_.reserve(limits_.maximum_pending_notifications);
  return result;
}

std::string_view semantic_role_name(const SemanticRole role) noexcept {
  switch (role) {
    case SemanticRole::Paragraph:
      return "paragraph";
    case SemanticRole::Heading:
      return "heading";
    case SemanticRole::List:
      return "list";
    case SemanticRole::ListItem:
      return "list-item";
    case SemanticRole::Table:
      return "table";
    case SemanticRole::TableRow:
      return "table-row";
    case SemanticRole::TableCell:
      return "table-cell";
    case SemanticRole::Image:
      return "image";
    case SemanticRole::Comment:
      return "comment";
  }
  return "document-item";
}

}  // namespace luma::write::accessibility
