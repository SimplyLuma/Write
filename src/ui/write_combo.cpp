#include "ui/write_window_private.hpp"

#include "commands/command_ids.h"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// One editable combo for the three values a person types as often as they
// pick: typeface, size and paragraph style. The value is an entry; clicking it
// opens the list under it without taking the keyboard, typing filters the
// list (or, for size, is the size), Up and Down move through the list (or step
// the size), Enter applies, Escape puts the value back.

namespace luma::write::ui {

namespace ids = luma::commands::ids;
using namespace detail;

namespace {

constexpr const char* kSizes[] = {"8",  "9",  "10", "11", "12", "14", "16", "18",
                                  "20", "24", "28", "36", "48", "72"};
// LibreOffice accepts sizes from 2 to 999.9 points in tenths.
constexpr double kMinimumSize = 2.0;
constexpr double kMaximumSize = 999.9;

std::string folded(std::string_view text) {
  std::string result{text};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = text.find_last_not_of(" \t");
  return std::string{text.substr(first, last - first + 1)};
}

std::string size_text(const double points) {
  char text[32];
  std::snprintf(text, sizeof(text), "%g", std::round(points * 10.0) / 10.0);
  return text;
}

GtkWidget* section_row(const char* title) {
  GtkWidget* row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_widget_set_focusable(row, FALSE);
  std::string upper{title};
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), left_label(upper.c_str(), "write-group-title"));
  gtk_widget_add_css_class(row, "write-combo-section");
  return row;
}

// A row carries the value it applies; `searchable` rows are the ones a typed
// filter matches (a face listed under Recent is not listed twice).
GtkWidget* value_row(const std::string& value, const std::string& shown, const bool current,
                     const bool searchable, const bool in_own_face) {
  GtkWidget* row = gtk_list_box_row_new();
  gtk_widget_set_focusable(row, FALSE);
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* label = left_label(shown.c_str(), "write-combo-label");
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  if (in_own_face) {
    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_family_new(value.c_str()));
    gtk_label_set_attributes(GTK_LABEL(label), attributes);
    pango_attr_list_unref(attributes);
  }
  gtk_box_append(GTK_BOX(box), label);
  GtkWidget* check = icon("write-check-symbolic", 14);
  gtk_widget_set_opacity(check, current ? 1.0 : 0.0);
  gtk_box_append(GTK_BOX(box), check);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_add_css_class(row, "write-combo-row");
  gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 shown.c_str(), -1);
  g_object_set_data_full(G_OBJECT(row), "write-value", g_strdup(value.c_str()), g_free);
  g_object_set_data_full(G_OBJECT(row), "write-key", g_strdup(folded(shown).c_str()), g_free);
  if (searchable) {
    g_object_set_data(G_OBJECT(row), "write-searchable", GINT_TO_POINTER(1));
  }
  return row;
}

bool row_is_value(GtkWidget* row) {
  return g_object_get_data(G_OBJECT(row), "write-value") != nullptr;
}

}  // namespace

ComboField* WriteWindow::Impl::combo_field(const ComboKind kind, const char* accessible_name) {
  auto combo = std::make_unique<ComboField>();
  combo->owner = this;
  combo->kind = kind;
  GtkWidget* entry = gtk_entry_new();
  add_css_classes(entry, {"write-combo"});
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 accessible_name, -1);
  gtk_widget_set_tooltip_text(entry, accessible_name);
  // Entries measure their natural width by their longest allowed text, so
  // both are set: a combo never pushes its island wider.
  switch (kind) {
    case ComboKind::font:
      gtk_editable_set_width_chars(GTK_EDITABLE(entry), 10);
      gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 10);
      gtk_widget_add_css_class(entry, "write-combo-font");
      break;
    case ComboKind::size:
      gtk_editable_set_width_chars(GTK_EDITABLE(entry), 3);
      gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 4);
      gtk_editable_set_alignment(GTK_EDITABLE(entry), 0.5F);
      gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_NUMBER);
      gtk_widget_add_css_class(entry, "write-combo-size");
      break;
    case ComboKind::style:
      gtk_editable_set_width_chars(GTK_EDITABLE(entry), 10);
      gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 10);
      gtk_widget_add_css_class(entry, "write-combo-style");
      break;
  }
  if (kind != ComboKind::size) {
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
                                      "write-chevron-down-symbolic");
    gtk_entry_set_icon_activatable(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY, TRUE);
  }
  combo->entry = GTK_ENTRY(entry);

  GtkWidget* popover = gtk_popover_new();
  add_css_classes(popover, {"write-popover", "write-combo-popover"});
  // The list opens under the value without taking the keyboard from it.
  gtk_popover_set_autohide(GTK_POPOVER(popover), FALSE);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
  gtk_widget_set_focusable(popover, FALSE);
  GtkWidget* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
  gtk_widget_set_focusable(list, FALSE);
  gtk_widget_add_css_class(list, "write-combo-list");
  gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 accessible_name, -1);
  GtkWidget* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll),
                                             kind == ComboKind::size ? 300 : 360);
  gtk_widget_set_size_request(scroll, kind == ComboKind::size ? 88 : 240, -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  gtk_popover_set_child(GTK_POPOVER(popover), scroll);
  gtk_widget_set_parent(popover, entry);
  combo->popover = popover;
  combo->list = GTK_LIST_BOX(list);
  combo->scroll = scroll;

  ComboField* raw = combo.get();
  gtk_list_box_set_filter_func(
      GTK_LIST_BOX(list),
      +[](GtkListBoxRow* row, gpointer data) -> gboolean {
        const auto* self = static_cast<ComboField*>(data);
        if (self->filter.empty()) {
          return g_object_get_data(G_OBJECT(row), "write-create") == nullptr;
        }
        if (g_object_get_data(G_OBJECT(row), "write-create") != nullptr) {
          return self->offer_create;
        }
        if (g_object_get_data(G_OBJECT(row), "write-searchable") == nullptr) {
          return FALSE;
        }
        const auto* key = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "write-key"));
        return key != nullptr && std::string_view{key}.find(self->filter) != std::string_view::npos;
      },
      raw, nullptr);

  // Clicking the value (or its chevron) opens the list and selects the text,
  // so typing replaces it.
  GtkGesture* click = gtk_gesture_click_new();
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     self->owner->open_combo(*self);
                     gtk_editable_select_region(GTK_EDITABLE(self->entry), 0, -1);
                   }),
                   raw);
  gtk_widget_add_controller(entry, GTK_EVENT_CONTROLLER(click));
  g_signal_connect(entry, "icon-release", G_CALLBACK(+[](GtkEntry*, GtkEntryIconPosition, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     self->owner->open_combo(*self);
                   }),
                   raw);
  g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     if (self->setting_text) return;
                     self->owner->filter_combo(*self, gtk_editable_get_text(editable));
                   }),
                   raw);
  g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     self->owner->commit_combo(*self);
                   }),
                   raw);
  GtkEventController* keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint,
                                                        GdkModifierType, gpointer data) -> gboolean {
                     auto* self = static_cast<ComboField*>(data);
                     switch (keyval) {
                       case GDK_KEY_Escape:
                         self->owner->revert_combo(*self, true);
                         return TRUE;
                       case GDK_KEY_Up:
                       case GDK_KEY_KP_Up:
                         self->owner->step_combo(*self, -1);
                         return TRUE;
                       case GDK_KEY_Down:
                       case GDK_KEY_KP_Down:
                         self->owner->step_combo(*self, 1);
                         return TRUE;
                       default:
                         return FALSE;
                     }
                   }),
                   raw);
  gtk_widget_add_controller(entry, keys);
  // Leaving the field puts the value back, after a moment, so a click on a
  // row in the list still lands.
  GtkEventController* focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "leave", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     if (self->close_source != 0) g_source_remove(self->close_source);
                     self->close_source = g_timeout_add(
                         220,
                         +[](gpointer inner) -> gboolean {
                           auto* field = static_cast<ComboField*>(inner);
                           field->close_source = 0;
                           field->owner->revert_combo(*field, false);
                           return G_SOURCE_REMOVE;
                         },
                         self);
                   }),
                   raw);
  gtk_widget_add_controller(entry, focus);
  g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     self->owner->apply_combo_row(*self, GTK_WIDGET(row));
                   }),
                   raw);
  g_signal_connect(entry, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     auto* self = static_cast<ComboField*>(data);
                     if (self->close_source != 0) {
                       g_source_remove(self->close_source);
                       self->close_source = 0;
                     }
                     gtk_widget_unparent(self->popover);
                   }),
                   raw);
  combos.push_back(std::move(combo));
  return raw;
}

// The value the field shows when nobody is typing in it.
std::string WriteWindow::Impl::combo_value(const ComboField& combo) const {
  switch (combo.kind) {
    case ComboKind::font:
      return snapshot.typeface_mixed ? std::string{"Mixed"} : snapshot.typeface_label;
    case ComboKind::size: {
      const double size =
          snapshot.size_label.empty() ? 0.0 : std::atof(snapshot.size_label.c_str());
      return size > 0.0 ? size_text(size) : std::string{};
    }
    case ComboKind::style:
      return snapshot.paragraph_style;
  }
  return {};
}

void WriteWindow::Impl::show_combo_value(ComboField& combo) {
  if (combo.editing) {
    return;
  }
  const std::string value = combo_value(combo);
  combo.setting_text = true;
  gtk_editable_set_text(GTK_EDITABLE(combo.entry), value.c_str());
  combo.setting_text = false;
  if (combo.kind == ComboKind::size) {
    gtk_entry_set_placeholder_text(combo.entry, value.empty() ? "—" : nullptr);
  }
  if (combo.kind == ComboKind::font)
    gtk_entry_set_placeholder_text(combo.entry, value.empty() ? "Choose font" : nullptr);
  // The closed font field uses the inspector's UI face. Only the picker rows
  // preview their typefaces; a decorative name here changes the control itself.
  if (combo.kind == ComboKind::font) gtk_entry_set_attributes(combo.entry, nullptr);
}

void WriteWindow::Impl::fill_combo(ComboField& combo) {
  const std::string current = combo_value(combo);
  if (combo.filled_generation == combo_generation && combo.filled_current == current) {
    return;
  }
  combo.filled_generation = combo_generation;
  combo.filled_current = current;
  gtk_list_box_remove_all(combo.list);
  const auto add = [&](const std::string& value, const std::string& shown, bool searchable) {
    gtk_list_box_append(combo.list, value_row(value, shown, value == current, searchable,
                                              combo.kind == ComboKind::font));
  };
  switch (combo.kind) {
    case ComboKind::font: {
      const auto installed = [this](const std::string& family) {
        return std::find(font_families.begin(), font_families.end(), family) !=
               font_families.end();
      };
      std::vector<std::string> recent;
      for (const auto& family : recent_fonts) {
        if (installed(family)) recent.push_back(family);
      }
      if (!recent.empty()) {
        gtk_list_box_append(combo.list, section_row("Recent"));
        for (const auto& family : recent) add(family, family, false);
      }
      std::vector<std::string> in_document;
      for (const auto& family : document_fonts) {
        if (installed(family) && std::find(recent.begin(), recent.end(), family) == recent.end()) {
          in_document.push_back(family);
        }
      }
      if (!in_document.empty()) {
        gtk_list_box_append(combo.list, section_row("In this document"));
        for (const auto& family : in_document) add(family, family, false);
      }
      gtk_list_box_append(combo.list, section_row("All fonts"));
      for (const auto& family : font_families) add(family, family, true);
      break;
    }
    case ComboKind::size:
      for (const char* size : kSizes) add(size, size, true);
      break;
    case ComboKind::style: {
      std::vector<std::string> styles = {"Title", "Subtitle", "Heading 1", "Heading 2",
                                         "Heading 3", "Body", "Quote", "List"};
      for (const auto& style : snapshot.paragraph_styles) {
        const std::string shown = style_display_name(style);
        if (std::find(styles.begin(), styles.end(), shown) == styles.end()) {
          styles.push_back(shown);
        }
      }
      for (const auto& style : styles) add(style, style, true);
      // Typing a name no style has offers to make one from the selection.
      GtkWidget* create = gtk_list_box_row_new();
      gtk_widget_set_focusable(create, FALSE);
      GtkWidget* create_label = left_label("", "write-combo-create");
      gtk_label_set_ellipsize(GTK_LABEL(create_label), PANGO_ELLIPSIZE_END);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(create), create_label);
      g_object_set_data(G_OBJECT(create), "write-create", GINT_TO_POINTER(1));
      gtk_widget_add_css_class(create, "write-combo-row");
      combo.create_label = GTK_LABEL(create_label);
      gtk_list_box_append(combo.list, create);
      break;
    }
  }
}

void WriteWindow::Impl::open_combo(ComboField& combo) {
  if (combo.close_source != 0) {
    g_source_remove(combo.close_source);
    combo.close_source = 0;
  }
  combo.editing = true;
  combo.filter.clear();
  combo.offer_create = false;
  fill_combo(combo);
  gtk_list_box_invalidate_filter(combo.list);
  // Start on the current value.
  gtk_list_box_unselect_all(combo.list);
  for (GtkWidget* row = gtk_widget_get_first_child(GTK_WIDGET(combo.list)); row != nullptr;
       row = gtk_widget_get_next_sibling(row)) {
    const auto* value = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "write-value"));
    if (value != nullptr && combo.filled_current == value &&
        g_object_get_data(G_OBJECT(row), "write-searchable") != nullptr) {
      gtk_list_box_select_row(combo.list, GTK_LIST_BOX_ROW(row));
      break;
    }
  }
  gtk_popover_popup(GTK_POPOVER(combo.popover));
  gtk_widget_grab_focus(GTK_WIDGET(combo.entry));
  if (GtkListBoxRow* selected = gtk_list_box_get_selected_row(combo.list)) {
    // Scroll the chosen value into view once the list has a size.
    g_idle_add_full(G_PRIORITY_LOW,
                    +[](gpointer data) -> gboolean {
                      auto* self = static_cast<ComboField*>(data);
                      if (GtkListBoxRow* row = gtk_list_box_get_selected_row(self->list)) {
                        graphene_point_t point{};
                        graphene_point_t out{};
                        if (gtk_widget_compute_point(GTK_WIDGET(row), GTK_WIDGET(self->list), &point, &out)) {
                          GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(
                              GTK_SCROLLED_WINDOW(self->scroll));
                          gtk_adjustment_set_value(adjustment, std::max(0.0, out.y - 60.0));
                        }
                      }
                      return G_SOURCE_REMOVE;
                    },
                    &combo, nullptr);
    static_cast<void>(selected);
  }
}

void WriteWindow::Impl::filter_combo(ComboField& combo, const std::string_view typed) {
  if (!combo.editing) {
    open_combo(combo);
  }
  if (combo.kind == ComboKind::size) {
    return;  // A size is typed, not searched for.
  }
  combo.filter = folded(trimmed(typed));
  combo.offer_create = false;
  if (combo.kind == ComboKind::style && !combo.filter.empty() && combo.create_label != nullptr) {
    bool exact = false;
    for (GtkWidget* row = gtk_widget_get_first_child(GTK_WIDGET(combo.list)); row != nullptr;
         row = gtk_widget_get_next_sibling(row)) {
      const auto* key = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "write-key"));
      if (key != nullptr && combo.filter == key) exact = true;
    }
    combo.offer_create = !exact;
    const std::string name = trimmed(typed);
    char* text = g_strdup_printf("New style “%s” from selection", name.c_str());
    gtk_label_set_text(combo.create_label, text);
    g_free(text);
  }
  gtk_list_box_invalidate_filter(combo.list);
  // The first match is the one Enter applies.
  gtk_list_box_unselect_all(combo.list);
  for (GtkWidget* row = gtk_widget_get_first_child(GTK_WIDGET(combo.list)); row != nullptr;
       row = gtk_widget_get_next_sibling(row)) {
    if (gtk_list_box_row_get_selectable(GTK_LIST_BOX_ROW(row)) &&
        (row_is_value(row) || g_object_get_data(G_OBJECT(row), "write-create") != nullptr)) {
      // Visibility settles after the filter runs; check the filter directly.
      const auto* key = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "write-key"));
      const bool matches =
          combo.filter.empty() ||
          (key != nullptr && g_object_get_data(G_OBJECT(row), "write-searchable") != nullptr &&
           std::string_view{key}.find(combo.filter) != std::string_view::npos) ||
          (g_object_get_data(G_OBJECT(row), "write-create") != nullptr && combo.offer_create);
      if (matches) {
        gtk_list_box_select_row(combo.list, GTK_LIST_BOX_ROW(row));
        break;
      }
    }
  }
}

void WriteWindow::Impl::step_combo(ComboField& combo, const int direction) {
  if (combo.kind == ComboKind::size) {
    // Arrow keys step whole points and apply, as the stepper buttons do.
    const char* typed = gtk_editable_get_text(GTK_EDITABLE(combo.entry));
    double size = typed != nullptr && *typed != '\0' ? std::atof(typed) : 12.0;
    if (size <= 0.0) size = 12.0;
    const double next = std::clamp(direction > 0 ? std::floor(size) + 1.0
                                                 : std::ceil(size) - 1.0,
                                   kMinimumSize, kMaximumSize);
    const std::string text = size_text(next);
    combo.setting_text = true;
    gtk_editable_set_text(GTK_EDITABLE(combo.entry), text.c_str());
    combo.setting_text = false;
    gtk_editable_set_position(GTK_EDITABLE(combo.entry), -1);
    dispatch_argument(ids::write::kFontSize, text);
    return;
  }
  if (!combo.editing) {
    open_combo(combo);
  }
  std::vector<GtkWidget*> rows;
  for (GtkWidget* row = gtk_widget_get_first_child(GTK_WIDGET(combo.list)); row != nullptr;
       row = gtk_widget_get_next_sibling(row)) {
    if (gtk_widget_get_child_visible(row) && gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(row))) {
      rows.push_back(row);
    }
  }
  if (rows.empty()) return;
  GtkListBoxRow* selected = gtk_list_box_get_selected_row(combo.list);
  auto at = std::find(rows.begin(), rows.end(), GTK_WIDGET(selected));
  std::ptrdiff_t index = at == rows.end() ? (direction > 0 ? -1 : 0) : at - rows.begin();
  index = std::clamp<std::ptrdiff_t>(index + direction, 0,
                                     static_cast<std::ptrdiff_t>(rows.size()) - 1);
  GtkWidget* next = rows[static_cast<std::size_t>(index)];
  gtk_list_box_select_row(combo.list, GTK_LIST_BOX_ROW(next));
  graphene_point_t point{};
  graphene_point_t out{};
  if (gtk_widget_compute_point(next, GTK_WIDGET(combo.list), &point, &out)) {
    GtkAdjustment* adjustment =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(combo.scroll));
    const double top = gtk_adjustment_get_value(adjustment);
    const double page = gtk_adjustment_get_page_size(adjustment);
    const double height = gtk_widget_get_height(next);
    if (out.y < top) {
      gtk_adjustment_set_value(adjustment, out.y);
    } else if (out.y + height > top + page) {
      gtk_adjustment_set_value(adjustment, out.y + height - page);
    }
  }
}

void WriteWindow::Impl::commit_combo(ComboField& combo) {
  const std::string typed = trimmed(gtk_editable_get_text(GTK_EDITABLE(combo.entry)));
  if (combo.kind == ComboKind::size) {
    char* end = nullptr;
    std::string normalised = typed;
    std::replace(normalised.begin(), normalised.end(), ',', '.');
    if (normalised.size() > 2 && normalised.ends_with("pt")) {
      normalised = trimmed(normalised.substr(0, normalised.size() - 2));
    }
    const double size = std::strtod(normalised.c_str(), &end);
    if (normalised.empty() || end == nullptr || *end != '\0' || !std::isfinite(size) ||
        size < kMinimumSize || size > kMaximumSize) {
      gtk_widget_error_bell(GTK_WIDGET(combo.entry));
      revert_combo(combo, false);
      return;
    }
    const std::string text = size_text(size);
    finish_combo(combo);
    dispatch_argument(ids::write::kFontSize, text);
    return;
  }
  if (GtkListBoxRow* selected = gtk_list_box_get_selected_row(combo.list);
      selected != nullptr && combo.editing) {
    apply_combo_row(combo, GTK_WIDGET(selected));
    return;
  }
  revert_combo(combo, true);
}

void WriteWindow::Impl::apply_combo_row(ComboField& combo, GtkWidget* row) {
  if (g_object_get_data(G_OBJECT(row), "write-create") != nullptr) {
    const std::string name = trimmed(gtk_editable_get_text(GTK_EDITABLE(combo.entry)));
    finish_combo(combo);
    if (!name.empty()) {
      dispatch_argument(ids::write::kNewStyle, name);
    }
    return;
  }
  const auto* value = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "write-value"));
  if (value == nullptr) {
    return;
  }
  const std::string chosen{value};
  finish_combo(combo);
  switch (combo.kind) {
    case ComboKind::font:
      remember_recent_font(chosen);
      dispatch_argument(ids::write::kFont, chosen);
      break;
    case ComboKind::size:
      dispatch_argument(ids::write::kFontSize, chosen);
      break;
    case ComboKind::style:
      dispatch_argument(ids::write::kApplyStyle, chosen);
      break;
  }
}

// Close the list and hand the keyboard back to the document.
void WriteWindow::Impl::finish_combo(ComboField& combo) {
  if (combo.close_source != 0) {
    g_source_remove(combo.close_source);
    combo.close_source = 0;
  }
  combo.editing = false;
  combo.filter.clear();
  gtk_popover_popdown(GTK_POPOVER(combo.popover));
  if (canvas != nullptr) {
    gtk_widget_grab_focus(canvas);
  }
}

void WriteWindow::Impl::revert_combo(ComboField& combo, const bool return_focus) {
  if (combo.close_source != 0) {
    g_source_remove(combo.close_source);
    combo.close_source = 0;
  }
  combo.editing = false;
  combo.filter.clear();
  gtk_popover_popdown(GTK_POPOVER(combo.popover));
  show_combo_value(combo);
  if (return_focus && canvas != nullptr) {
    gtk_widget_grab_focus(canvas);
  }
}

void WriteWindow::Impl::remember_recent_font(const std::string& family) {
  recent_fonts.erase(std::remove(recent_fonts.begin(), recent_fonts.end(), family),
                     recent_fonts.end());
  recent_fonts.insert(recent_fonts.begin(), family);
  if (recent_fonts.size() > 5) {
    recent_fonts.resize(5);
  }
  ++combo_generation;
  if (callbacks.on_recent_fonts_changed) {
    callbacks.on_recent_fonts_changed(recent_fonts);
  }
}

void WriteWindow::Impl::update_combos() {
  const std::string& face = snapshot.typeface_label;
  if (!face.empty() &&
      std::find(document_fonts.begin(), document_fonts.end(), face) == document_fonts.end()) {
    document_fonts.push_back(face);
    std::sort(document_fonts.begin(), document_fonts.end());
    ++combo_generation;
  }
  if (snapshot.paragraph_styles != shown_paragraph_styles) {
    shown_paragraph_styles = snapshot.paragraph_styles;
    ++combo_generation;
  }
  for (const auto& combo : combos) {
    show_combo_value(*combo);
  }
  update_weights();
}

// ── Weight ─────────────────────────────────────────────────────────────────

namespace {

struct WeightChoice {
  int css;
  const char* name;
};
constexpr WeightChoice kWeightNames[] = {
    {100, "Thin"},   {200, "Extra Light"}, {300, "Light"},      {400, "Regular"},
    {500, "Medium"}, {600, "Semibold"},    {700, "Bold"},       {800, "Extra Bold"},
    {900, "Black"}};

const char* weight_name(const int css) {
  for (const auto& choice : kWeightNames) {
    if (choice.css == css) return choice.name;
  }
  return "Regular";
}

int nearest_css_weight(const int fontconfig_weight) {
  const int open_type = FcWeightToOpenType(fontconfig_weight);
  return std::clamp((open_type + 50) / 100 * 100, 100, 900);
}

}  // namespace

// The weights a family really has, from fontconfig. A weight other than
// Regular and Bold is offered only when the engine lists a face for it by
// name ("Noto Sans Medium"), because that face is how it is applied; Bold is
// always offered, as every word processor does.
std::vector<WeightOption> WriteWindow::Impl::weights_for(const std::string& family) const {
  std::vector<WeightOption> options{{400, "Regular", family}, {700, "Bold", family}};
  if (family.empty()) {
    return options;
  }
  FcPattern* pattern = FcPatternCreate();
  FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
  FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, FC_WEIGHT, FC_SLANT, nullptr);
  FcFontSet* set = FcFontList(nullptr, pattern, objects);
  if (set != nullptr) {
    for (int index = 0; index < set->nfont; ++index) {
      int weight = 0;
      int slant = 0;
      if (FcPatternGetInteger(set->fonts[index], FC_WEIGHT, 0, &weight) != FcResultMatch) continue;
      if (FcPatternGetInteger(set->fonts[index], FC_SLANT, 0, &slant) == FcResultMatch &&
          slant != FC_SLANT_ROMAN) {
        continue;
      }
      const int css = nearest_css_weight(weight);
      if (css == 400 || css == 700) continue;
      // The face's own family name for that weight, as the engine lists it.
      std::string face;
      FcChar8* name = nullptr;
      for (int n = 0; FcPatternGetString(set->fonts[index], FC_FAMILY, n, &name) == FcResultMatch; ++n) {
        const std::string candidate{reinterpret_cast<const char*>(name)};
        if (candidate != family &&
            std::find(font_families.begin(), font_families.end(), candidate) != font_families.end()) {
          face = candidate;
          break;
        }
      }
      if (face.empty()) continue;
      if (std::none_of(options.begin(), options.end(),
                       [css](const WeightOption& option) { return option.css == css; })) {
        options.push_back({css, weight_name(css), face});
      }
    }
    FcFontSetDestroy(set);
  }
  FcObjectSetDestroy(objects);
  FcPatternDestroy(pattern);
  std::sort(options.begin(), options.end(),
            [](const WeightOption& left, const WeightOption& right) { return left.css < right.css; });
  return options;
}

void WriteWindow::Impl::update_weights() {
  if (inspector_weight == nullptr) {
    return;
  }
  // A face named for its weight ("Noto Sans Medium") belongs to its family.
  std::string family = snapshot.typeface_label;
  int current = snapshot.weight_label == "Bold" ? 700 : 400;
  for (const auto& choice : kWeightNames) {
    const std::string suffix = std::string{" "} + choice.name;
    if (family.size() > suffix.size() && family.ends_with(suffix)) {
      const std::string base = family.substr(0, family.size() - suffix.size());
      if (std::find(font_families.begin(), font_families.end(), base) != font_families.end()) {
        family = base;
        if (current != 700) current = choice.css;
      }
    }
  }
  const bool was_updating = updating_inspector;
  if (family != weight_family || weight_options.empty()) {
    weight_family = family;
    weight_options = weights_for(family);
    updating_inspector = true;
    const guint count = g_list_model_get_n_items(G_LIST_MODEL(inspector_weight_model));
    std::vector<std::string> labels;
    for (const auto& option : weight_options) {
      labels.push_back(std::string{option.name} + " " + std::to_string(option.css));
    }
    std::vector<const char*> names;
    for (const auto& label : labels) names.push_back(label.c_str());
    names.push_back(nullptr);
    gtk_string_list_splice(inspector_weight_model, 0, count, names.data());
  }
  guint selected = GTK_INVALID_LIST_POSITION;
  for (guint index = 0; index < weight_options.size(); ++index) {
    if (weight_options[index].css == current) selected = index;
  }
  updating_inspector = true;
  gtk_drop_down_set_selected(inspector_weight, selected);
  gtk_widget_set_tooltip_text(GTK_WIDGET(inspector_weight),
                              weight_options.size() <= 2
                                  ? "This typeface has Regular and Bold"
                                  : "Weights this typeface has");
  updating_inspector = was_updating;
}

void WriteWindow::Impl::apply_weight(const guint index) {
  if (index >= weight_options.size()) {
    return;
  }
  const WeightOption& option = weight_options[index];
  // Face first (a named weight is its own face), then Bold on or off.
  const std::string face = option.css == 700 ? weight_family : option.face;
  if (!face.empty() && face != snapshot.typeface_label) {
    dispatch_argument(ids::write::kFont, face);
  }
  const bool bold = option.css >= 700 && option.face == weight_family;
  dispatch_argument(ids::write::kBoldState, bold ? "true" : "false");
}

}  // namespace luma::write::ui
