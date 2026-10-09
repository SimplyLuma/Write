#include "ui/write_window_private.hpp"

#include "commands/command_ids.h"
#include "ui/comment_data.hpp"

#if __has_include(<luma-creative-properties.h>)
#include <luma-creative-properties.h>
#define WRITE_HAS_CREATIVE_PROPERTIES 1
#endif

#include <algorithm>
#include <clocale>
#include <cmath>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>

namespace luma::write::ui {

namespace ids = luma::commands::ids;
using namespace detail;

namespace {

constexpr const char* kMixed = "Mixed";

#ifdef WRITE_HAS_CREATIVE_PROPERTIES
// Keep the kit's menu and interaction when an imported value has no preset.
// Its current API exposes only "Mixed" for an unselected value.
void choice_word(GtkWidget* widget, const char* name, const char* word) {
  luma_property_choice_set_mixed(LUMA_PROPERTY_CHOICE(widget));
  const auto visit = [word, name](const auto& self, GtkWidget* child) -> void {
    if (GTK_IS_LABEL(child) && g_strcmp0(gtk_label_get_text(GTK_LABEL(child)), "Mixed") == 0)
      gtk_label_set_text(GTK_LABEL(child), word);
    if (GTK_IS_BUTTON(child)) {
      const std::string spoken = std::string{name} + ", " + word;
      gtk_accessible_update_property(GTK_ACCESSIBLE(child), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     spoken.c_str(), -1);
    }
    for (GtkWidget* next = gtk_widget_get_first_child(child); next != nullptr;
         next = gtk_widget_get_next_sibling(next)) self(self, next);
  };
  visit(visit, widget);
}
#endif

// The styles the picker offers first, in the order a document uses them.
constexpr std::array<const char*, 8> kCommonStyles{
    "Title", "Subtitle", "Heading 1", "Heading 2", "Heading 3", "Body", "Quote", "List"};

// Colours offered for text and highlight. Deliberately few: a document is
// carried by its styles, and these are the marks a reader can still tell
// apart in print.
struct Swatch {
  const char* name;
  const char* hex;
};
constexpr std::array<Swatch, 10> kTextSwatches{
    Swatch{"Black", "#000000"},  Swatch{"Graphite", "#4b5563"}, Swatch{"Red", "#c62828"},
    Swatch{"Orange", "#d9661f"}, Swatch{"Amber", "#b88a00"},    Swatch{"Green", "#2e7d32"},
    Swatch{"Teal", "#00796b"},   Swatch{"Blue", "#1f5fbf"},     Swatch{"Violet", "#6a3fb5"},
    Swatch{"Pink", "#b8336a"}};
constexpr std::array<Swatch, 8> kHighlightSwatches{
    Swatch{"Yellow", "#fff176"}, Swatch{"Green", "#b9f6ca"},  Swatch{"Cyan", "#a7ffeb"},
    Swatch{"Blue", "#bbdefb"},   Swatch{"Pink", "#f8bbd0"},   Swatch{"Orange", "#ffd8a8"},
    Swatch{"Violet", "#e1bee7"}, Swatch{"Grey", "#e0e0e0"}};

constexpr std::array<const char*, 48> kCharacters{
    "—", "–", "…", "“", "”", "‘", "’", "«", "»", "•", "·", "§", "¶", "†", "‡", "©",
    "®", "™", "°", "±", "×", "÷", "≈", "≠", "≤", "≥", "∞", "√", "π", "µ", "Ω", "∑",
    "€", "£", "¥", "¢", "½", "¼", "¾", "²", "³", "→", "←", "↑", "↓", "✓", "✗", "★"};

#ifndef WRITE_HAS_CREATIVE_PROPERTIES
// The baseline kit's heading while the creative property section is pending.
GtkWidget* eyebrow(const char* text) {
  std::string upper{text};
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  GtkWidget* label = left_label(upper.c_str(), "write-group-title");
  return label;
}
#endif

// One inspector group: a heading and what it holds, spaced as a unit.
GtkWidget* group(const char* title, GtkWidget* body) {
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  GtkWidget* section = luma_property_section_new(title);
  luma_property_section_append(LUMA_PROPERTY_SECTION(section), body);
  return section;
#else
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(box, "write-group");
  gtk_box_append(GTK_BOX(box), eyebrow(title));
  gtk_box_append(GTK_BOX(box), body);
  return box;
#endif
}

GtkWidget* property_list() {
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(list, "write-properties");
  return list;
}

GtkWidget* property_row(const char* name, GtkWidget* value) {
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(row, "write-property");
  GtkWidget* label = left_label(name, "write-property-name");
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(row), label);
  gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), value);
  gtk_accessible_update_relation(GTK_ACCESSIBLE(value), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                 label, nullptr, -1);
  return row;
}

GtkWidget* value_label(GtkLabel** out) {
  GtkWidget* label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(label), 1.0F);
  gtk_widget_add_css_class(label, "write-property-value");
  *out = GTK_LABEL(label);
  return label;
}

// A text action at the foot of a group: the kit's small button.
GtkWidget* quiet_action(const char* label) {
  GtkWidget* button = gtk_button_new_with_label(label);
  add_css_classes(button, {"luma-button", "small", "write-group-action"});
  gtk_widget_set_halign(button, GTK_ALIGN_START);
  return button;
}

[[maybe_unused]] GtkWidget* segment(std::initializer_list<std::pair<const char*, const char*>> items,
                   const char* extra_class) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  add_css_classes(box, {"luma-segment", "write-segment", extra_class});
  gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
  GtkToggleButton* first = nullptr;
  for (const auto& [id, label] : items) {
    GtkWidget* button = gtk_toggle_button_new_with_label(label);
    g_object_set_data_full(G_OBJECT(button), "write-segment-id", g_strdup(id), g_free);
    if (first == nullptr) {
      first = GTK_TOGGLE_BUTTON(button);
    } else {
      gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), first);
    }
    gtk_box_append(GTK_BOX(box), button);
  }
  return box;
}

void select_segment(GtkWidget* box, const std::string_view id) {
  for (GtkWidget* child = gtk_widget_get_first_child(box); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    const auto* candidate =
        static_cast<const char*>(g_object_get_data(G_OBJECT(child), "write-segment-id"));
    const bool match = candidate != nullptr && id == candidate;
    if (match) {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child), TRUE);
      return;
    }
  }
  // No choice matches (a custom value): show none selected.
  for (GtkWidget* child = gtk_widget_get_first_child(box); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child), FALSE);
  }
}

[[maybe_unused]] guint string_list_find(GtkStringList* list, const std::string_view value) {
  const guint count = g_list_model_get_n_items(G_LIST_MODEL(list));
  for (guint index = 0; index < count; ++index) {
    if (value == gtk_string_list_get_string(list, index)) {
      return index;
    }
  }
  return GTK_INVALID_LIST_POSITION;
}

[[maybe_unused]] GtkWidget* compact_dropdown(GtkStringList* model, const char* accessible_name) {
  GtkExpression* expression =
      gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, nullptr, "string");
  // The dropdown takes the model (and the expression): callers must not unref it.
  GtkWidget* dropdown = gtk_drop_down_new(G_LIST_MODEL(model), expression);
  add_css_classes(dropdown, {"luma-dropdown", "small", "inspector-dropdown"});
  gtk_accessible_update_property(GTK_ACCESSIBLE(dropdown), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 accessible_name, -1);
  gtk_widget_set_tooltip_text(dropdown, accessible_name);
  return dropdown;
}

}  // namespace

// ── Outline ────────────────────────────────────────────────────────────────

GtkWidget* WriteWindow::Impl::build_navigator() {
  GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(panel, "wr-outline-panel");
  add_css_classes(panel, {"write-panel", "write-outline"});
#ifndef WRITE_HAS_CREATIVE_WORKSPACE
  navigator_back = gtk_button_new_with_label("Back to document");
  add_css_classes(navigator_back, {"luma-button", "quiet", "small", "write-panel-back"});
  set_accessible_label(navigator_back, "Close outline");
  gtk_widget_set_visible(navigator_back, FALSE);
  g_signal_connect(navigator_back, "clicked", G_CALLBACK(close_navigator), this);
  gtk_box_append(GTK_BOX(panel), navigator_back);

  // The island's own title row, as the kit's app bar.
  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  add_css_classes(header, {"luma-appbar", "write-panel-header"});
  GtkWidget* title = left_label("Outline", "luma-appbar-title");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_box_append(GTK_BOX(header), title);
  GtkWidget* sidebar_toggle = luma_sidebar_toggle_new(GTK_WIDGET(navigator_view), TRUE);
  gtk_widget_set_name(sidebar_toggle, "wr-outline-toggle");
  gtk_box_append(GTK_BOX(header), sidebar_toggle);
  gtk_box_append(GTK_BOX(panel), header);
#endif

  outline_stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(outline_stack), FALSE);
#ifndef WRITE_HAS_CREATIVE_WORKSPACE
  GtkWidget* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
#endif
  outline_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(outline_list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(outline_list), TRUE);
  add_css_classes(outline_list, {"write-outline-list"});
  gtk_accessible_update_property(GTK_ACCESSIBLE(outline_list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Document outline", -1);
  g_signal_connect(outline_list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     self->dispatch_argument(ids::write::kOutlineJump,
                                             std::to_string(gtk_list_box_row_get_index(row)));
                   }),
                   this);
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  gtk_stack_add_named(outline_stack, outline_list, "list");
#else
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), outline_list);
  gtk_stack_add_named(outline_stack, scroll, "list");
#endif

  // An empty list is one quiet line where the headings will be, as the kit's
  // list empty state has it; a pane-sized glyph would outweigh the document.
  GtkWidget* empty = gtk_label_new("Headings you add appear here.");
  gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
  gtk_label_set_xalign(GTK_LABEL(empty), 0.0F);
  gtk_widget_set_valign(empty, GTK_ALIGN_START);
  add_css_classes(empty, {"luma-list-empty", "write-outline-empty"});
  gtk_stack_add_named(outline_stack, empty, "empty");
  gtk_stack_set_visible_child_name(outline_stack, "empty");
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(outline_stack));
  return panel;
}

void WriteWindow::Impl::rebuild_outline() {
  const bool had_headings = !shown_outline.empty();
  const bool has_headings = snapshot.outline_ready && !snapshot.outline.empty();
  if (!has_headings) {
    navigator_requested = false;
    wide_navigator_requested = false;
  } else if (!had_headings) {
    wide_navigator_requested = true;
    navigator_requested = !compact && !focus_mode;
  }
  shown_outline = snapshot.outline;
  outline_shown_ready = snapshot.outline_ready;
  gtk_list_box_remove_all(GTK_LIST_BOX(outline_list));
  const auto count = snapshot.outline.size();
  gtk_stack_set_visible_child_name(outline_stack, count == 0 ? "empty" : "list");
  apply_panel_visibility();
  // The top level sets the indent; a document whose headings start at level 2
  // is not pushed right.
  int top = 9;
  for (const OutlineEntry& entry : snapshot.outline) {
    top = std::min(top, std::clamp(entry.level, 1, 4));
  }
  for (const OutlineEntry& entry : snapshot.outline) {
    GtkWidget* label = left_label(entry.title.c_str(), "write-outline-label");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    const int depth = std::clamp(entry.level, 1, 4) - top;
    gtk_widget_set_margin_start(label, depth * 12);
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(content), label);
    if (entry.level > 1 && entry.word_count > 0) {
      GtkWidget* badge = luma_count_badge_new(entry.word_count, FALSE);
      gtk_widget_set_name(badge, "wr-outline-count");
      const std::string count_label = std::to_string(entry.word_count) + " words";
      gtk_widget_set_tooltip_text(badge, count_label.c_str());
      gtk_accessible_update_property(GTK_ACCESSIBLE(badge), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     count_label.c_str(), -1);
      gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(content), badge);
    }
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
    add_css_classes(row, {"luma-navigation-row", "write-outline-row"});
    gtk_widget_add_css_class(row, (std::string{"depth-"} + std::to_string(depth)).c_str());
    gtk_widget_set_tooltip_text(row, entry.title.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   entry.title.c_str(), -1);
    gtk_list_box_append(GTK_LIST_BOX(outline_list), row);
    if (entry.active) {
      gtk_list_box_select_row(GTK_LIST_BOX(outline_list), GTK_LIST_BOX_ROW(row));
    }
  }
}

// ── Inspector ──────────────────────────────────────────────────────────────

GtkWidget* WriteWindow::Impl::build_text_inspector() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  add_css_classes(page, {"write-inspector-page"});

  // The first two sections follow the selected paragraph. Their inputs are
  // the existing engine-backed combos; only their placement is Write-owned.
  ComboField* style = combo_field(ComboKind::style, "Paragraph style");
  g_object_set(style->entry, "width-chars", 8, nullptr);
  GtkWidget* style_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(style_body), GTK_WIDGET(style->entry));
  GtkWidget* style_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* update = quiet_action("Update style");
  gtk_widget_set_tooltip_text(update, "Update the selected paragraph style from this formatting");
  bind_argument(update, "write.advanced.run", ".uno:StyleUpdateByExample");
  gtk_box_append(GTK_BOX(style_actions), update);
  GtkWidget* create = quiet_action("New style…");
  gtk_widget_set_name(create, "wr-new-style");
  bind_argument(create, ids::write::kNewStyle, "");
  gtk_box_append(GTK_BOX(style_actions), create);
  GtkWidget* style_options = gtk_expander_new("Style options");
  gtk_widget_set_name(style_options, "wr-style-options");
  gtk_expander_set_child(GTK_EXPANDER(style_options), style_actions);
  gtk_widget_add_css_class(style_options, "write-inspector-disclosure");
  gtk_box_append(GTK_BOX(style_body), style_options);
  gtk_box_append(GTK_BOX(page), group("Paragraph style", style_body));

  GtkWidget* font_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  ComboField* font = combo_field(ComboKind::font, "Font");
  g_object_set(font->entry, "width-chars", 8, nullptr);
  gtk_box_append(GTK_BOX(font_body), GTK_WIDGET(font->entry));
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  inspector_size_number = luma_property_number_new("Size", "pt");
  gtk_widget_add_css_class(inspector_size_number, "compact");
  gtk_widget_set_name(inspector_size_number, "wr-inspector-size");
  luma_property_number_set_name(LUMA_PROPERTY_NUMBER(inspector_size_number), "Font size");
  luma_property_number_set_range(LUMA_PROPERTY_NUMBER(inspector_size_number), 1.0, 999.9);
  luma_property_number_set_step(LUMA_PROPERTY_NUMBER(inspector_size_number), 0.5, 1);
  g_signal_connect(inspector_size_number, "value-changed",
                   G_CALLBACK(+[](LumaPropertyNumber*, double value, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     if (self->updating_inspector) return;
                     char text[32];
                     std::snprintf(text, sizeof(text), "%g", value);
                     self->dispatch_argument(ids::write::kFontSize, text);
                   }), this);
  gtk_box_append(GTK_BOX(font_body), inspector_size_number);
#else
  GtkWidget* font_values = property_list();
  gtk_box_append(GTK_BOX(font_values), property_row("Size", size_stepper()));
  gtk_box_append(GTK_BOX(font_values),
                 property_row("Line", value_label(&inspector_line_height_value)));
  gtk_box_append(GTK_BOX(font_body), font_values);
#endif

  GtkWidget* marks_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget* marks_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(marks_row, "lumaui-ac-row");
  gtk_widget_set_name(marks_row, "wr-font-tools");
  GtkWidget* marks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(marks, "lumaui-bar-group");
  gtk_box_set_homogeneous(GTK_BOX(marks), TRUE);
  gtk_widget_set_hexpand(marks, TRUE);
  const auto tool = [this](const char* glyph, const char* label, std::string_view command, bool checkable) {
    LumaBarItem* item = luma_bar_item_new_action(glyph, nullptr, nullptr);
    luma_bar_item_set_tooltip(item, label);
    GtkWidget* control = luma_bar_item_create_control(item, "tool");
    g_object_unref(item);
    const std::string name = std::string{"wr-inspector-"} + glyph;
    gtk_widget_set_name(control, name.c_str());
    if (checkable) g_object_set_data(G_OBJECT(control), "write-checkable", GINT_TO_POINTER(1));
    bind_command(control, command);
    return control;
  };
  gtk_box_append(GTK_BOX(marks), tool("bold", "Bold · Ctrl+B", ids::write::kBold, true));
  gtk_box_append(GTK_BOX(marks), tool("italic", "Italic · Ctrl+I", ids::write::kItalic, true));
  gtk_box_append(GTK_BOX(marks), tool("underline", "Underline · Ctrl+U", ids::write::kUnderline, true));
  gtk_box_append(GTK_BOX(marks), tool("strikethrough", "Strikethrough · Ctrl+Shift+X", ids::write::kStrikethrough, true));
  gtk_box_append(GTK_BOX(marks_row), marks);
  gtk_box_append(GTK_BOX(marks_row), tool("link", "Insert link… · Ctrl+Shift+K", ids::write::kInsertLink, false));
  gtk_box_append(GTK_BOX(marks_body), marks_row);
  gtk_box_append(GTK_BOX(font_body), marks_body);
  gtk_box_append(GTK_BOX(page), group("Font", font_body));

#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  GtkWidget* colour_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  const auto colour_field = [this, colour_body](const char* title,
                                               std::string_view command, bool highlight) {
    GtkWidget* field = luma_property_color_new(title);
    const auto& swatches = highlight ? kHighlightSwatches.data() : kTextSwatches.data();
    const auto count = highlight ? kHighlightSwatches.size() : kTextSwatches.size();
    std::vector<GdkRGBA> palette(count);
    for (std::size_t index = 0; index < count; ++index)
      gdk_rgba_parse(&palette[index], swatches[index].hex);
    luma_property_color_set_palette(LUMA_PROPERTY_COLOR(field), palette.data(),
                                     static_cast<guint>(palette.size()));
    auto binding = std::make_unique<CommandBinding>();
    binding->self = this;
    binding->command_id = command;
    g_signal_connect(field, "color-changed", G_CALLBACK(+[](
        LumaPropertyColor*, const GdkRGBA* rgba, gpointer data) {
      auto* bound = static_cast<CommandBinding*>(data);
      if (bound->self->updating_inspector) return;
      GdkRGBA opaque = *rgba;
      opaque.alpha = 1.0F;
      char* hex = luma_property_color_format(&opaque);
      const std::string value = std::string{"#"} + hex;
      g_free(hex);
      bound->self->dispatch_argument(bound->command_id, value);
    }), binding.get());
    command_bindings.push_back(std::move(binding));
    // Automatic/None remain semantic values until the person starts typing
    // a colour. Entering the field only enables the kit's hex editor.
    for (GtkWidget* child = gtk_widget_get_first_child(field); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
      if (!GTK_IS_ENTRY(child)) continue;
      gtk_widget_set_tooltip_text(child, "Enter a hex colour, or choose from the colour swatch");
      GtkEventController* focus = gtk_event_controller_focus_new();
      g_signal_connect(focus, "enter", G_CALLBACK(+[](GtkEventControllerFocus* controller, gpointer data) {
        GtkWidget* entry = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
        if (!gtk_editable_get_editable(GTK_EDITABLE(entry))) {
          luma_property_color_set_word(LUMA_PROPERTY_COLOR(data), nullptr);
          gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
        }
      }), field);
      gtk_widget_add_controller(child, focus);
    }
    GtkWidget* reset = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(reset), luma_ui_icon_new("rotate-ccw"));
    gtk_widget_add_css_class(reset, "luma-icon-button");
    set_accessible_label(reset, highlight ? "No highlight" : "Automatic text colour");
    bind_argument(reset, command, "auto");
    luma_property_color_add_suffix(LUMA_PROPERTY_COLOR(field), reset);
    gtk_box_append(GTK_BOX(colour_body), left_label(title, "write-property-name"));
    gtk_box_append(GTK_BOX(colour_body), field);
    return field;
  };
  inspector_text_colour = colour_field("Text colour", ids::write::kTextColor, false);
  inspector_highlight_colour = colour_field("Highlight", ids::write::kHighlight, true);
  gtk_box_append(GTK_BOX(page), group("Colour", colour_body));
#else
  const auto swatches = [this](
      std::initializer_list<std::tuple<const char*, const char*, const char*>> choices,
      const std::string_view command) {
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    guint index = 0;
    for (const auto& [name, colour, value] : choices) {
      GtkWidget* button = gtk_button_new();
      gtk_widget_add_css_class(button, "colour-swatch");
      gtk_widget_set_size_request(button, 26, 26);
      set_accessible_label(button, name);
      GtkCssProvider* provider = gtk_css_provider_new();
      const std::string css = std::string{"button { background: "} + colour + "; }";
      gtk_css_provider_load_from_string(provider, css.c_str());
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gtk_style_context_add_provider(gtk_widget_get_style_context(button),
                                     GTK_STYLE_PROVIDER(provider),
                                     GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
      G_GNUC_END_IGNORE_DEPRECATIONS
      g_object_unref(provider);
      bind_argument(button, command, value);
      gtk_grid_attach(GTK_GRID(grid), button, static_cast<int>(index++), 0, 1, 1);
    }
    return grid;
  };
  GtkWidget* colour_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_box_append(GTK_BOX(colour_body), swatches({
      {"Ink", "#1f2126", "#1f2126"}, {"Grey", "#6b6e75", "#6b6e75"},
      {"Blue", "#2f62c9", "#2f62c9"}, {"Green", "#2f8a55", "#2f8a55"},
      {"Red", "#c8433a", "#c8433a"}, {"Amber", "#b7791f", "#b7791f"}},
      ids::write::kTextColor));
  gtk_box_append(GTK_BOX(colour_body), left_label("Highlight", "write-property-name"));
  gtk_box_append(GTK_BOX(colour_body), swatches({
      {"Highlight Yellow", "#fde68a", "#fde68a"},
      {"Highlight Green", "#bbf7d0", "#bbf7d0"},
      {"Highlight Blue", "#bfdbfe", "#bfdbfe"},
      {"Highlight Pink", "#fbcfe8", "#fbcfe8"},
      {"Highlight Lilac", "#ddd6fe", "#ddd6fe"},
      {"Highlight None", "transparent", "auto"}}, ids::write::kHighlight));
  gtk_box_append(GTK_BOX(page), group("Colour", colour_body));
#endif
  return page;
}

GtkWidget* WriteWindow::Impl::build_paragraph_inspector() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  add_css_classes(page, {"write-inspector-page"});
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  GtkWidget* paragraph = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  inspector_line_height_number = luma_property_number_new("Line", "×");
  gtk_widget_set_name(inspector_line_height_number, "wr-inspector-line");
  luma_property_number_set_name(LUMA_PROPERTY_NUMBER(inspector_line_height_number), "Line spacing");
  luma_property_number_set_range(LUMA_PROPERTY_NUMBER(inspector_line_height_number), 0.5, 5.0);
  luma_property_number_set_step(LUMA_PROPERTY_NUMBER(inspector_line_height_number), 0.05, 2);
  g_signal_connect(inspector_line_height_number, "value-changed",
      G_CALLBACK(+[](LumaPropertyNumber*, double value, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        if (self->updating_inspector) return;
        char text[32];
        std::snprintf(text, sizeof(text), "%g", value);
        self->dispatch_argument(ids::write::kLineSpacing, text);
      }), this);
  gtk_box_append(GTK_BOX(paragraph), inspector_line_height_number);
#endif

  GtkWidget* alignment = connected_group("write-fill-group");
  gtk_box_set_homogeneous(GTK_BOX(alignment), TRUE);
  gtk_box_append(GTK_BOX(alignment), command_button("write-align-left-symbolic", "Left",
                                                    "write.paragraph.align-left", true));
  gtk_box_append(GTK_BOX(alignment), command_button("write-align-center-symbolic",
                                                    "Centre",
                                                    "write.paragraph.align-center", true));
  gtk_box_append(GTK_BOX(alignment), command_button("write-align-right-symbolic",
                                                    "Right",
                                                    "write.paragraph.align-right", true));
  gtk_box_append(GTK_BOX(alignment), command_button("write-align-justify-symbolic", "Justify",
                                                    "write.paragraph.align-justify", true));
  gtk_box_append(GTK_BOX(page), group("Alignment", alignment));

#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  GtkWidget* gap = connected_group();
  gtk_box_append(GTK_BOX(gap), command_button("write-minus-symbolic", "Less space between paragraphs",
                                             ids::write::kSpaceLess));
  gtk_box_append(GTK_BOX(gap), command_button("write-plus-symbolic", "More space between paragraphs",
                                             ids::write::kSpaceMore));
  gtk_box_append(GTK_BOX(paragraph), property_row("Paragraph gap", gap));
  GtkWidget* custom_spacing = quiet_action("Set paragraph spacing…");
  gtk_widget_set_name(custom_spacing, "wr-paragraph-spacing");
  gtk_widget_set_tooltip_text(custom_spacing, "Set exact space before and after the selected paragraphs");
  bind_argument(custom_spacing, ids::write::kParagraphSpacing, "");
  gtk_box_append(GTK_BOX(paragraph), custom_spacing);
#endif

  GtkWidget* lists_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* lists = connected_group("write-fill-group");
  gtk_box_set_homogeneous(GTK_BOX(lists), TRUE);
  gtk_widget_set_hexpand(lists, TRUE);
  gtk_box_append(GTK_BOX(lists), command_button("write-list-bulleted-symbolic",
                                                "Bulleted list", "write.paragraph.bullets",
                                                true));
  gtk_box_append(GTK_BOX(lists), command_button("write-list-numbered-symbolic",
                                                "Numbered list", "write.paragraph.numbering",
                                                true));
  gtk_box_append(GTK_BOX(lists), command_button("write-list-check-symbolic", "Checklist",
                                                ids::write::kChecklist));
  gtk_box_append(GTK_BOX(lists_row), lists);
  GtkWidget* indent = connected_group();
  gtk_box_append(GTK_BOX(indent), command_button("write-indent-less-symbolic",
                                                 "Decrease indent", ids::write::kIndentLess));
  gtk_box_append(GTK_BOX(indent), command_button("write-indent-more-symbolic",
                                                 "Increase indent", ids::write::kIndentMore));
  gtk_box_append(GTK_BOX(lists_row), indent);
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  gtk_box_append(GTK_BOX(page), group("Lists and indent", lists_row));
  gtk_box_append(GTK_BOX(page), group("Spacing", paragraph));
  GtkWidget* clear = quiet_action("Reset paragraph formatting");
  bind_command(clear, ids::write::kClearParagraphOverrides);
  gtk_box_append(GTK_BOX(page), clear);
#else
  gtk_box_append(GTK_BOX(page), group("Lists and indent", lists_row));

  line_spacing_segment =
      segment({{"1", "Single"}, {"1.15", "1.15"}, {"1.5", "1.5"}, {"2", "Double"}},
              "spacing-segment");
  for (GtkWidget* child = gtk_widget_get_first_child(line_spacing_segment); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    g_signal_connect(child, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                       auto* self = static_cast<Impl*>(data);
                       if (self->updating_inspector || !gtk_toggle_button_get_active(button))
                         return;
                       self->dispatch_argument(
                           ids::write::kLineSpacing,
                           static_cast<const char*>(
                               g_object_get_data(G_OBJECT(button), "write-segment-id")));
                     }),
                     this);
  }
  gtk_box_append(GTK_BOX(page), group("Line spacing", line_spacing_segment));

  GtkWidget* spacing_list = property_list();
  GtkWidget* spacing = connected_group();
  gtk_box_append(GTK_BOX(spacing), command_button("write-size-down-symbolic",
                                                  "Less space between paragraphs",
                                                  ids::write::kSpaceLess));
  gtk_box_append(GTK_BOX(spacing), command_button("write-size-up-symbolic",
                                                  "More space between paragraphs",
                                                  ids::write::kSpaceMore));
  gtk_box_append(GTK_BOX(spacing_list), property_row("Space between", spacing));
  GtkWidget* spacing_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(spacing_body), spacing_list);
  GtkWidget* clear = quiet_action("Clear paragraph overrides");
  bind_command(clear, ids::write::kClearParagraphOverrides);
  gtk_box_append(GTK_BOX(spacing_body), clear);
  gtk_box_append(GTK_BOX(page), group("Paragraph spacing", spacing_body));
#endif
  return page;
}

GtkWidget* WriteWindow::Impl::build_page_inspector() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  add_css_classes(page, {"write-inspector-page"});
  GtkWidget* properties = property_list();

#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  const auto choice = [this](LumaChoiceKind kind, const char* name,
      std::initializer_list<std::pair<const char*, const char*>> options,
      std::string_view command) {
    GtkWidget* control = luma_property_choice_new(kind, name);
    for (const auto& [key, label] : options)
      luma_property_choice_add(LUMA_PROPERTY_CHOICE(control), key, label, nullptr);
    auto binding = std::make_unique<CommandBinding>();
    binding->self = this;
    binding->command_id = command;
    g_signal_connect(control, "changed", G_CALLBACK(+[](
        LumaPropertyChoice*, const char* key, gpointer data) {
      auto* bound = static_cast<CommandBinding*>(data);
      if (!bound->self->updating_inspector)
        bound->self->dispatch_argument(bound->command_id, key);
    }), binding.get());
    command_bindings.push_back(std::move(binding));
    return control;
  };
  page_size_choice = choice(LUMA_CHOICE_KIND_MENU, "Paper size",
      {{"Letter", "Letter"}, {"Legal", "Legal"}, {"A4", "A4"}, {"A5", "A5"}},
      ids::write::kPageSize);
  margins_choice = choice(LUMA_CHOICE_KIND_MENU, "Page margins",
      {{"Normal", "Normal"}, {"Narrow", "Narrow"}, {"Moderate", "Moderate"}, {"Wide", "Wide"},
       {"Custom", "Custom…"}},
      ids::write::kPageMargins);
  orientation_choice = choice(LUMA_CHOICE_KIND_SEGMENTS, "Page orientation",
      {{"portrait", "Portrait"}, {"landscape", "Landscape"}}, ids::write::kPageOrientation);
  gtk_widget_set_name(page_size_choice, "wr-page-paper");
  gtk_widget_set_name(margins_choice, "wr-page-margins");
  gtk_widget_set_name(orientation_choice, "wr-page-orientation");
  gtk_box_append(GTK_BOX(properties), property_row("Paper", page_size_choice));
  gtk_box_append(GTK_BOX(properties), left_label("Orientation", "write-property-name"));
  gtk_box_append(GTK_BOX(properties), orientation_choice);
  gtk_box_append(GTK_BOX(properties), property_row("Margins", margins_choice));
  gtk_box_append(GTK_BOX(page), group("Page layout", properties));
#else
  GtkStringList* sizes = gtk_string_list_new(nullptr);
  for (const char* size : {"Letter", "Legal", "A4", "A5"}) {
    gtk_string_list_append(sizes, size);
  }
  GtkWidget* paper = compact_dropdown(sizes, "Paper size");
  page_size_dropdown = GTK_DROP_DOWN(paper);
  g_signal_connect(paper, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     if (self->updating_inspector) return;
                     auto* model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(object)));
                     const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
                     if (selected == GTK_INVALID_LIST_POSITION) return;
                     self->dispatch_argument(ids::write::kPageSize,
                                             gtk_string_list_get_string(model, selected));
                   }),
                   this);
  gtk_box_append(GTK_BOX(properties), property_row("Paper", paper));

  orientation_segment =
      segment({{"portrait", "Portrait"}, {"landscape", "Landscape"}}, "orientation-segment");
  for (GtkWidget* child = gtk_widget_get_first_child(orientation_segment); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    g_signal_connect(child, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                       auto* self = static_cast<Impl*>(data);
                       if (self->updating_inspector || !gtk_toggle_button_get_active(button))
                         return;
                       self->dispatch_argument(
                           ids::write::kPageOrientation,
                           static_cast<const char*>(
                               g_object_get_data(G_OBJECT(button), "write-segment-id")));
                     }),
                     this);
  }
  gtk_box_append(GTK_BOX(properties), property_row("Orientation", orientation_segment));

  GtkStringList* margins = gtk_string_list_new(nullptr);
  for (const char* margin : {"Normal", "Narrow", "Moderate", "Wide"}) {
    gtk_string_list_append(margins, margin);
  }
  GtkWidget* margin = compact_dropdown(margins, "Margins");
  margins_dropdown = GTK_DROP_DOWN(margin);
  gtk_drop_down_set_selected(margins_dropdown, GTK_INVALID_LIST_POSITION);
  g_signal_connect(margin, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     if (self->updating_inspector) return;
                     auto* model = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(object)));
                     const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
                     if (selected == GTK_INVALID_LIST_POSITION) return;
                     self->dispatch_argument(ids::write::kPageMargins,
                                             gtk_string_list_get_string(model, selected));
                   }),
                   this);
  gtk_box_append(GTK_BOX(properties), property_row("Margins", margin));
  gtk_box_append(GTK_BOX(page), group("Page setup", properties));

#endif

  GtkWidget* parts = property_list();
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  header_choice = choice(LUMA_CHOICE_KIND_SEGMENTS, "Header",
      {{"off", "Off"}, {"on", "On"}}, ids::write::kHeaderState);
  footer_choice = choice(LUMA_CHOICE_KIND_SEGMENTS, "Footer",
      {{"off", "Off"}, {"on", "On"}}, ids::write::kFooterState);
  gtk_widget_set_name(header_choice, "wr-header-state");
  gtk_widget_set_name(footer_choice, "wr-footer-state");
  gtk_widget_set_tooltip_text(header_choice, "Enable or remove the current section’s header");
  gtk_widget_set_tooltip_text(footer_choice, "Enable or remove the current section’s footer");
  gtk_box_append(GTK_BOX(parts), property_row("Header", header_choice));
  gtk_box_append(GTK_BOX(parts), property_row("Footer", footer_choice));
#else
  const auto part_switch = [this, parts](const char* name, std::string_view command,
                                         GtkSwitch** out) {
    GtkWidget* toggle = gtk_switch_new();
    auto binding = std::make_unique<CommandBinding>();
    binding->self = this;
    binding->command_id = std::string{command};
    g_signal_connect(toggle, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean, gpointer data) -> gboolean {
                       auto* bound = static_cast<CommandBinding*>(data);
                       if (!bound->self->updating_inspector) {
                         bound->self->dispatch(bound->command_id);
                       }
                       return FALSE;
                     }),
                     binding.get());
    command_bindings.push_back(std::move(binding));
    *out = GTK_SWITCH(toggle);
    gtk_box_append(GTK_BOX(parts), property_row(name, toggle));
  };
  part_switch("Header", ids::write::kToggleHeader, &header_switch);
  part_switch("Footer", ids::write::kToggleFooter, &footer_switch);
#endif
  GtkWidget* parts_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(parts_body), parts);
  GtkWidget* numbers = quiet_action("Insert page number");
  bind_command(numbers, ids::write::kInsertPageNumber);
  gtk_box_append(GTK_BOX(parts_body), numbers);
  gtk_box_append(GTK_BOX(page), group("Header and footer", parts_body));

  GtkWidget* measure = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_set_homogeneous(GTK_BOX(measure), TRUE);
  for (const auto& [name, value] :
       {std::pair{"Pages", &page_count_value}, std::pair{"Words", &page_words_value},
        std::pair{"Characters", &page_characters_value}}) {
    GtkWidget* count = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* number = value_label(value);
    gtk_label_set_xalign(GTK_LABEL(number), 0);
    gtk_box_append(GTK_BOX(count), left_label(name, "write-property-name"));
    gtk_box_append(GTK_BOX(count), number);
    gtk_box_append(GTK_BOX(measure), count);
  }
  gtk_box_append(GTK_BOX(page), group("Document", measure));
  return page;
}

GtkWidget* WriteWindow::Impl::build_advanced_inspector() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  add_css_classes(page, {"write-inspector-page", "write-advanced"});
  gtk_box_set_spacing(GTK_BOX(page), 12);
  GtkWidget* note = gtk_label_new(
      "Everything else the document engine can do. Each acts on the selection, "
      "as the toolbar does.");
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(note), 0.0F);
  add_css_classes(note, {"write-advanced-note"});
  gtk_box_append(GTK_BOX(page), note);
  advanced_search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_search_entry_set_placeholder_text(advanced_search, "Filter tools");
  gtk_accessible_update_property(GTK_ACCESSIBLE(advanced_search), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Filter advanced tools", -1);
  gtk_widget_add_css_class(GTK_WIDGET(advanced_search), "advanced-search");
  g_signal_connect(advanced_search, "search-changed", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                     static_cast<Impl*>(data)->rebuild_advanced(
                         gtk_editable_get_text(GTK_EDITABLE(entry)));
                   }),
                   this);
  gtk_box_append(GTK_BOX(page), GTK_WIDGET(advanced_search));
  advanced_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_box_append(GTK_BOX(page), advanced_list);
  return page;
}

void WriteWindow::Impl::rebuild_advanced(const std::string_view query) {
  advanced_bindings.clear();
  while (GtkWidget* child = gtk_widget_get_first_child(advanced_list)) {
    gtk_box_remove(GTK_BOX(advanced_list), child);
  }
  std::string needle{query};
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto& group : advanced_groups) {
    GtkWidget* rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    int shown = 0;
    for (const auto& command : group.commands) {
      std::string haystack = command.label + " " + group.title;
      std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (!needle.empty() && haystack.find(needle) == std::string::npos) {
        continue;
      }
      GtkWidget* button = gtk_button_new();
      GtkWidget* label = left_label(command.label.c_str(), "advanced-label");
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      gtk_button_set_child(GTK_BUTTON(button), label);
      add_css_classes(button, {"luma-navigation-row", "write-advanced-row"});
      gtk_widget_set_tooltip_text(button, command.label.c_str());
      auto binding = std::make_unique<CommandBinding>();
      binding->self = this;
      binding->command_id = std::string{ids::write::kRunAdvanced};
      binding->argument = command.uno_command;
      g_signal_connect(button, "clicked", G_CALLBACK(argument_clicked), binding.get());
      advanced_bindings.push_back(std::move(binding));
      gtk_box_append(GTK_BOX(rows), button);
      ++shown;
    }
    if (shown == 0) {
      g_object_ref_sink(rows);
      g_object_unref(rows);
      continue;
    }
    gtk_box_append(GTK_BOX(advanced_list), ::luma::write::ui::group(group.title.c_str(), rows));
  }
}

// The selected picture: its size (proportions locked by default), how text
// flows around it, and where it is anchored.
GtkWidget* WriteWindow::Impl::build_image_inspector() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_name(page, "wr-photo-format");
  add_css_classes(page, {"write-inspector-page"});
  GtkWidget* preview_box = gtk_overlay_new();
  gtk_widget_set_name(preview_box, "wr-photo-preview");
  GtkWidget* preview = gtk_picture_new();
  picture_preview = GTK_PICTURE(preview);
  gtk_widget_set_name(preview, "wr-photo-preview-image");
  gtk_picture_set_content_fit(picture_preview, GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request(preview, -1, 128);
  gtk_overlay_set_child(GTK_OVERLAY(preview_box), preview);
  LumaBarItem* replace_item = luma_bar_item_new_action("image", "Replace", "write.bar-photo-replace");
  GtkWidget* replace = luma_bar_item_create_control(replace_item, "tool");
  gtk_widget_set_halign(replace, GTK_ALIGN_START);
  gtk_widget_set_valign(replace, GTK_ALIGN_END);
  gtk_widget_set_margin_start(replace, 8);
  gtk_widget_set_margin_bottom(replace, 8);
  gtk_overlay_add_overlay(GTK_OVERLAY(preview_box), replace);
  g_object_unref(replace_item);
  gtk_box_append(GTK_BOX(page), preview_box);

  picture_size_modes = luma_mode_switch_new("Photo size");
  gtk_widget_set_name(picture_size_modes, "wr-photo-size-modes");
  for (const auto& [key, label, glyph] :
       std::array<std::tuple<const char*, const char*, const char*>, 4>{
           std::tuple{"s", "S", "minimize-2"}, std::tuple{"m", "M", "square"},
           std::tuple{"l", "L", "maximize-2"}, std::tuple{"full", "Full", "maximize-2"}})
    luma_mode_switch_add(LUMA_MODE_SWITCH(picture_size_modes), key, label, glyph);
  g_signal_connect(picture_size_modes, "changed", G_CALLBACK(+[](LumaModeSwitch*, const char* key,
                                                                  gpointer data) {
    auto* self = static_cast<Impl*>(data);
    const std::string action = std::string{"write.photo-size-"} + key;
    gtk_widget_activate_action(GTK_WIDGET(self->window), action.c_str(), nullptr);
  }), this);
  gtk_box_append(GTK_BOX(page), group("Size", picture_size_modes));

  GtkWidget* caption = gtk_switch_new();
  picture_caption_switch = GTK_SWITCH(caption);
  gtk_widget_set_name(caption, "wr-photo-caption-switch");
  g_signal_connect(caption, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean on,
                                                         gpointer data) -> gboolean {
    auto* self = static_cast<Impl*>(data);
    if (on && !self->updating_inspector) self->dispatch_argument(
        ids::write::kPictureAction, "caption");
    return FALSE;
  }), this);
  gtk_box_append(GTK_BOX(page), group("Caption", property_row("Show caption", caption)));

  GtkWidget* alt = gtk_entry_new();
  picture_alt_text = GTK_ENTRY(alt);
  gtk_widget_set_name(alt, "wr-photo-alt-text");
  gtk_entry_set_placeholder_text(picture_alt_text, "Describe the photo");
  gtk_editable_set_editable(GTK_EDITABLE(alt), FALSE);
  set_accessible_label(alt, "Alt text");
  gtk_widget_set_tooltip_text(alt, "Open image properties to edit alt text");
  GtkGesture* alt_click = gtk_gesture_click_new();
  g_signal_connect(alt_click, "pressed", G_CALLBACK(+[](GtkGestureClick*, int, double, double,
                                                         gpointer data) {
    static_cast<Impl*>(data)->dispatch_argument(ids::write::kPictureAction, "properties");
  }), this);
  gtk_widget_add_controller(alt, GTK_EVENT_CONTROLLER(alt_click));
  gtk_box_append(GTK_BOX(page), group("Alt text", alt));
  GtkWidget* delete_photo = quiet_action("Delete photo");
  bind_argument(delete_photo, ids::write::kPictureAction, "delete");

  GtkWidget* layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_name(layout, "wr-photo-layout");
  gtk_box_append(GTK_BOX(page), layout);
  GtkWidget* sizes = property_list();
  const auto size_entry = [this](const char* name) {
    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 6);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 6);
    gtk_editable_set_alignment(GTK_EDITABLE(entry), 1.0F);
    add_css_classes(entry, {"write-combo", "write-picture-size"});
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry* changed, gpointer data) {
                       auto* self = static_cast<Impl*>(data);
                       const bool is_width = changed == self->picture_width;
                       const double typed = std::atof(gtk_editable_get_text(GTK_EDITABLE(changed)));
                       if (typed <= 0.0 || self->snapshot.picture_width_twips <= 0) return;
                       const char* locale = std::setlocale(LC_MEASUREMENT, nullptr);
                       const bool metric = locale == nullptr ||
                                           std::string_view{locale}.find("_US") == std::string_view::npos;
                       const double twips = typed * (metric ? 1440.0 / 2.54 : 1440.0);
                       double width = static_cast<double>(self->snapshot.picture_width_twips);
                       double height = static_cast<double>(self->snapshot.picture_height_twips);
                       const bool locked = gtk_toggle_button_get_active(self->picture_lock);
                       if (is_width) {
                         if (locked) height *= twips / width;
                         width = twips;
                       } else {
                         if (locked) width *= twips / height;
                         height = twips;
                       }
                       char value[64];
                       std::snprintf(value, sizeof(value), "%lld,%lld", std::llround(width),
                                     std::llround(height));
                       self->dispatch_argument(ids::write::kPictureSize, value);
                     }),
                     this);
    return GTK_ENTRY(entry);
  };
  picture_width = size_entry("Picture width");
  picture_height = size_entry("Picture height");
  gtk_box_append(GTK_BOX(sizes), property_row("Width", GTK_WIDGET(picture_width)));
  gtk_box_append(GTK_BOX(sizes), property_row("Height", GTK_WIDGET(picture_height)));
  GtkWidget* lock = gtk_toggle_button_new();
  gtk_button_set_child(GTK_BUTTON(lock), icon("write-link-symbolic", 14));
  add_css_classes(lock, {"luma-icon-button"});
  set_accessible_label(lock, "Keep proportions");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lock), TRUE);
  picture_lock = GTK_TOGGLE_BUTTON(lock);
  GtkWidget* lock_group = connected_group();
  gtk_box_append(GTK_BOX(lock_group), lock);
  gtk_box_append(GTK_BOX(sizes), property_row("Keep proportions", lock_group));
  GtkWidget* size_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append(GTK_BOX(size_body), sizes);
  GtkWidget* reset = quiet_action("Reset size");
  bind_argument(reset, ids::write::kPictureLayout, "reset");
  gtk_box_append(GTK_BOX(size_body), reset);
  gtk_box_append(GTK_BOX(layout), group("Dimensions", size_body));

#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  const auto layout_choice = [this](const char* name,
      std::initializer_list<std::pair<const char*, const char*>> options) {
    GtkWidget* control = luma_property_choice_new(LUMA_CHOICE_KIND_MENU, name);
    for (const auto& [key, title] : options)
      luma_property_choice_add(LUMA_PROPERTY_CHOICE(control), key, title, nullptr);
    g_signal_connect(control, "changed", G_CALLBACK(+[](
        LumaPropertyChoice*, const char* key, gpointer data) {
      auto* self = static_cast<Impl*>(data);
      if (!self->updating_inspector) self->dispatch_argument(ids::write::kPictureLayout, key);
    }), this);
    return control;
  };
  picture_flow_choice = layout_choice("Text wrapping", {{"inline", "In line with text"},
      {"wrap", "Wrap text"}, {"break", "Break text"}, {"behind", "Behind text"},
      {"front", "In front of text"}});
  picture_anchor_choice = layout_choice("Image position", {{"with-text", "Move with text"},
      {"on-page", "Fix on page"}});
  gtk_widget_set_name(picture_flow_choice, "wr-photo-flow");
  gtk_widget_set_name(picture_anchor_choice, "wr-photo-anchor");
  gtk_box_append(GTK_BOX(layout), group("Text wrapping", picture_flow_choice));
  gtk_box_append(GTK_BOX(layout), group("Position", picture_anchor_choice));
#else
  GtkWidget* wrap = quiet_action("Wrap text around image");
  bind_argument(wrap, ids::write::kPictureLayout, "wrap");
  gtk_box_append(GTK_BOX(layout), group("Text wrapping", wrap));
#endif
  gtk_box_append(GTK_BOX(page), delete_photo);
  GtkWidget* hint = left_label("Drag the image to move it. Drag a corner to resize. Click outside or press Esc to return to text.", "write-property-hint");
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_box_append(GTK_BOX(page), hint);
  return page;
}

GtkWidget* WriteWindow::Impl::build_inspector() {
  GtkWidget* panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(panel, "wr-inspector-panel");
  add_css_classes(panel, {"write-panel", "write-inspector"});
#ifndef WRITE_HAS_CREATIVE_WORKSPACE
  inspector_back = gtk_button_new_with_label("Back to document");
  add_css_classes(inspector_back, {"luma-button", "quiet", "small", "write-panel-back"});
  set_accessible_label(inspector_back, "Close inspector");
  gtk_widget_set_visible(inspector_back, FALSE);
  g_signal_connect(inspector_back, "clicked", G_CALLBACK(close_inspector), this);
  gtk_box_append(GTK_BOX(panel), inspector_back);
#endif

  inspector_stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_transition_type(inspector_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(inspector_stack, 100);
  gtk_stack_set_vhomogeneous(inspector_stack, FALSE);
  gtk_stack_set_hhomogeneous(inspector_stack, FALSE);

  // Format and Comments are one shared mode switch; the kit owns its shape,
  // spacing and keyboard behaviour.
  GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  add_css_classes(header, {"luma-appbar", "write-panel-header"});
  inspector_tabs = luma_mode_switch_new("Inspector");
  gtk_widget_set_name(inspector_tabs, "wr-inspector-tabs");
  gtk_widget_set_hexpand(inspector_tabs, TRUE);
  luma_mode_switch_set_fill(LUMA_MODE_SWITCH(inspector_tabs), TRUE);
  gtk_widget_set_valign(inspector_tabs, GTK_ALIGN_CENTER);
  luma_mode_switch_add(LUMA_MODE_SWITCH(inspector_tabs), "format", "Format", "paintbrush");
  luma_mode_switch_add(LUMA_MODE_SWITCH(inspector_tabs), "comments", "Comments", "message-square");
  // Format and Comments are the two v70 modes; context determines the Format
  // body's text, table or photo controls.
  g_signal_connect(inspector_tabs, "changed", G_CALLBACK(+[](LumaModeSwitch*, const char* id, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     const char* page = std::string_view{id} == "comments" ? "comments" :
                         self->snapshot.picture_selected ? "image" :
                         self->snapshot.selection_context == "Table" && !self->table_context_dismissed ? "table" : "format";
                     gtk_stack_set_visible_child_name(self->inspector_stack, page);
                   }),
                   this);
  gtk_box_append(GTK_BOX(header), inspector_tabs);
  gtk_box_append(GTK_BOX(panel), header);

  GtkWidget* format = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(format, "wr-format");
  table_tools_button = quiet_action("Table tools");
  gtk_widget_set_name(table_tools_button, "wr-table-tools");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(table_tools_button), "write.table-tools");
  gtk_widget_set_margin_start(table_tools_button, 12);
  gtk_widget_set_margin_end(table_tools_button, 12);
  gtk_widget_set_margin_top(table_tools_button, 6);
  gtk_widget_set_visible(table_tools_button, FALSE);
  gtk_box_append(GTK_BOX(format), table_tools_button);
  format_tabs = luma_mode_switch_new("Formatting category");
  luma_mode_switch_set_fill(LUMA_MODE_SWITCH(format_tabs), TRUE);
  luma_mode_switch_set_small(LUMA_MODE_SWITCH(format_tabs), TRUE);
  gtk_widget_set_name(format_tabs, "wr-format-tabs");
  gtk_widget_add_css_class(format_tabs, "write-format-tabs");
  luma_mode_switch_add(LUMA_MODE_SWITCH(format_tabs), "text", "Text", nullptr);
  luma_mode_switch_add(LUMA_MODE_SWITCH(format_tabs), "paragraph", "Paragraph", nullptr);
  luma_mode_switch_add(LUMA_MODE_SWITCH(format_tabs), "page", "Page", nullptr);
  format_stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_name(GTK_WIDGET(format_stack), "wr-format-stack");
  gtk_stack_set_vhomogeneous(format_stack, FALSE);
  gtk_stack_set_hhomogeneous(format_stack, FALSE);
  gtk_stack_set_transition_type(format_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(format_stack, 100);
  gtk_stack_add_named(format_stack, build_text_inspector(), "text");
  gtk_stack_add_named(format_stack, build_paragraph_inspector(), "paragraph");
  gtk_stack_add_named(format_stack, build_page_inspector(), "page");
  g_signal_connect(format_tabs, "changed", G_CALLBACK(+[](
      LumaModeSwitch*, const char* id, gpointer data) {
    gtk_stack_set_visible_child_name(static_cast<Impl*>(data)->format_stack, id);
  }), this);
  luma_mode_switch_set_current(LUMA_MODE_SWITCH(format_tabs), "text");
  gtk_box_append(GTK_BOX(format), format_tabs);
  gtk_box_append(GTK_BOX(format), GTK_WIDGET(format_stack));
  gtk_stack_add_named(inspector_stack, format, "format");
  gtk_stack_add_named(inspector_stack, build_comments_inspector(), "comments");
  advanced_tab = build_advanced_inspector();
  gtk_stack_add_named(inspector_stack, advanced_tab, "advanced");
  image_tab = build_image_inspector();
  gtk_stack_add_named(inspector_stack, image_tab, "image");
  table_tab = build_table_inspector();
  gtk_stack_add_named(inspector_stack, table_tab, "table");
  gtk_stack_set_visible_child_name(inspector_stack, "format");

#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  // FloatingPanel supplies the scroll viewport. A nested scroller reports
  // only its short minimum and collapses the inspector to one section.
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(inspector_stack));
#else
  GtkWidget* scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(inspector_stack));
  gtk_box_append(GTK_BOX(panel), scroll);
#endif
  return panel;
}

GtkWidget* WriteWindow::Impl::build_table_inspector() {
  GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_name(page, "wr-table-format");
  gtk_widget_add_css_class(page, "write-inspector-page");
  GtkWidget* text_format = quiet_action("Format cell text");
  gtk_widget_set_name(text_format, "wr-table-text");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(text_format), "write.context-done");
  gtk_box_append(GTK_BOX(page), text_format);
  GtkWidget* dimensions = property_list();
  gtk_box_append(GTK_BOX(dimensions), property_row("Rows", value_label(&table_rows_value)));
  gtk_box_append(GTK_BOX(dimensions),
                 property_row("Columns", value_label(&table_columns_value)));
  table_size_group = group("Size", dimensions);
  gtk_widget_set_visible(table_size_group, FALSE);
  gtk_box_append(GTK_BOX(page), table_size_group);

  const auto tools = [](std::initializer_list<std::tuple<const char*, const char*, const char*>> choices) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(row, "write-table-actions");
    for (const auto& [icon_name, label, action_name] : choices) {
      LumaBarItem* action = luma_bar_item_new_action(icon_name, label, action_name);
      GtkWidget* button = luma_bar_item_create_control(action, "tool");
      gtk_widget_add_css_class(button, "write-table-action");
      gtk_widget_set_hexpand(button, TRUE);
      gtk_box_append(GTK_BOX(row), button);
      g_object_unref(action);
    }
    return row;
  };
  GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(actions), tools({
      {"between-horizontal-end", "Add row", "write.bar-table-row"},
      {"between-vertical-end", "Add column", "write.bar-table-column"}}));
  gtk_box_append(GTK_BOX(actions), tools({
      {"trash-2", "Delete row", "write.bar-table-delete-row"},
      {"trash-2", "Delete column", "write.bar-table-delete-column"}}));
  gtk_box_append(GTK_BOX(page), group("Edit", actions));
  LumaBarItem* remove = luma_bar_item_new_action("trash-2", "Delete table",
                                                 "write.bar-table-delete");
  luma_bar_item_set_danger(remove, TRUE);
  GtkWidget* delete_button = luma_bar_item_create_control(remove, "tool");
  gtk_widget_add_css_class(delete_button, "write-table-delete");
  gtk_box_append(GTK_BOX(page), delete_button);
  g_object_unref(remove);
  return page;
}

GtkWidget* WriteWindow::Impl::build_comments_inspector() {
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(list, "wr-comments");
  gtk_widget_add_css_class(list, "write-comments-list");
  comments_list = list;
  GtkWidget* empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_name(empty, "wr-comments-empty");
  gtk_widget_add_css_class(empty, "luma-list-empty");
  gtk_box_append(GTK_BOX(empty), left_label("No comments yet", "comment-author"));
  GtkWidget* note = left_label("Select text, then add a comment.", "write-property-name");
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_box_append(GTK_BOX(empty), note);
  GtkWidget* add = quiet_action("Add comment…");
  bind_command(add, ids::write::kAddComment);
  gtk_box_append(GTK_BOX(empty), add);
  gtk_box_append(GTK_BOX(list), empty);
  g_object_set_data(G_OBJECT(list), "write-empty", empty);
  return list;
}

void WriteWindow::Impl::rebuild_comments() {
  if (comments_list == nullptr) return;
  std::string key = snapshot.read_only ? "read-only|" : "editable|";
  for (const CommentCard& comment : snapshot.comments) {
    key += comment.id + "|" + comment.author + "|" + comment.time_label + "|" +
           comment.quote + "|" + comment.text + "|" + (comment.resolved ? "1" : "0") + "\n";
  }
  if (key == shown_comments_key && gtk_widget_get_first_child(comments_list) != nullptr) return;
  shown_comments_key = std::move(key);
  GtkWidget* placeholder = GTK_WIDGET(g_object_get_data(G_OBJECT(comments_list), "write-empty"));
  for (GtkWidget* child = gtk_widget_get_first_child(comments_list); child != nullptr;) {
    GtkWidget* next = gtk_widget_get_next_sibling(child);
    if (child != placeholder) gtk_box_remove(GTK_BOX(comments_list), child);
    child = next;
  }
  GtkWidget* empty = GTK_WIDGET(g_object_get_data(G_OBJECT(comments_list), "write-empty"));
  const bool no_comments = std::none_of(snapshot.comments.begin(), snapshot.comments.end(),
      [](const CommentCard& comment) { return !comment.resolved; });
  gtk_widget_set_visible(empty, no_comments);
  if (no_comments) return;
  for (const CommentCard& comment : snapshot.comments) {
    if (comment.resolved) continue;
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "write-inspector-comment");
    if (!comment.quote.empty()) {
      GtkWidget* quote = gtk_label_new(comment.quote.c_str());
      gtk_label_set_xalign(GTK_LABEL(quote), 0.0F);
      gtk_label_set_ellipsize(GTK_LABEL(quote), PANGO_ELLIPSIZE_END);
      gtk_widget_add_css_class(quote, "write-comment-quote");
      gtk_box_append(GTK_BOX(card), quote);
    }
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    std::string initials = comment.initials;
    if (initials.empty()) initials = comment_initials(comment.author);
    GtkWidget* avatar = gtk_label_new(initials.c_str());
    gtk_widget_add_css_class(avatar, "comment-avatar");
    gtk_box_append(GTK_BOX(head), avatar);
    GtkWidget* author = gtk_label_new(comment.author.c_str());
    gtk_label_set_xalign(GTK_LABEL(author), 0.0F);
    gtk_widget_set_hexpand(author, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(author), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(author), 14);
    gtk_widget_add_css_class(author, "comment-author");
    gtk_box_append(GTK_BOX(head), author);
    GtkWidget* time = gtk_label_new(comment.time_label.c_str());
    gtk_widget_add_css_class(time, "comment-time");
    gtk_box_append(GTK_BOX(head), time);
    gtk_box_append(GTK_BOX(card), head);
    GtkWidget* body = gtk_label_new(comment.text.c_str());
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(body), 0.0F);
    gtk_widget_add_css_class(body, "comment-body");
    gtk_box_append(GTK_BOX(card), body);
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    for (const auto& [label, command] : {
             std::pair{"Reply", ids::write::kReplyComment},
             std::pair{"Resolve", ids::write::kResolveComment}}) {
      GtkWidget* button = gtk_button_new_with_label(label);
      gtk_widget_set_sensitive(button, !snapshot.read_only);
      gtk_widget_add_css_class(button, "write-comment-action");
      g_object_set_data_full(G_OBJECT(button), "write-comment-id",
                             g_strdup(comment.id.c_str()), g_free);
      g_object_set_data(G_OBJECT(button), "write-comment-command",
                        const_cast<char*>(command.data()));
      g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        self->dispatch_argument(
            static_cast<const char*>(g_object_get_data(G_OBJECT(clicked), "write-comment-command")),
            static_cast<const char*>(g_object_get_data(G_OBJECT(clicked), "write-comment-id")));
      }), this);
      gtk_box_append(GTK_BOX(actions), button);
    }
    gtk_box_append(GTK_BOX(card), actions);
    gtk_box_append(GTK_BOX(comments_list), card);
  }
}

void WriteWindow::Impl::step_font_size(const int direction) {
  // Whole points, as Docs steps; from a mixed selection, start at 12.
  const double size = snapshot.size_label.empty() ? 12.0 : std::atof(snapshot.size_label.c_str());
  const double next = std::clamp(direction > 0 ? std::floor(size) + 1.0 : std::ceil(size) - 1.0,
                                 2.0, 999.9);
  char value[32];
  std::snprintf(value, sizeof(value), "%g", next);
  dispatch_argument(ids::write::kFontSize, value);
}

// Size, as Docs has it: step down, the size (an editable combo of common
// sizes), step up, one connected group. The toolbar and inspector each have one.
GtkWidget* WriteWindow::Impl::size_stepper() {
  GtkWidget* size = connected_group("write-size-stepper");
  GtkWidget* size_down = command_button("write-minus-symbolic", "Decrease font size", "");
  g_signal_connect(size_down, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     static_cast<Impl*>(data)->step_font_size(-1);
                   }),
                   this);
  gtk_box_append(GTK_BOX(size), size_down);
  gtk_box_append(GTK_BOX(size), GTK_WIDGET(combo_field(ComboKind::size, "Font size")->entry));
  GtkWidget* size_up = command_button("write-plus-symbolic", "Increase font size", "");
  g_signal_connect(size_up, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     static_cast<Impl*>(data)->step_font_size(1);
                   }),
                   this);
  gtk_box_append(GTK_BOX(size), size_up);
  return size;
}

void WriteWindow::Impl::update_inspector() {
  if (inspector_line_height_value == nullptr && inspector_line_height_number == nullptr) {
    return;
  }
  if (format_stack != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(format_stack), !snapshot.read_only);
  if (image_tab != nullptr) gtk_widget_set_sensitive(image_tab, !snapshot.read_only);
  if (table_tab != nullptr) gtk_widget_set_sensitive(table_tab, !snapshot.read_only);
  if (table_tools_button != nullptr)
    gtk_widget_set_visible(table_tools_button, snapshot.selection_context == "Table" && table_context_dismissed);
  // A selected photo changes the Format body, not the top-level mode.
  if (image_tab != nullptr) {
    if (snapshot.picture_selected && !picture_was_selected) {
      if (std::string_view{gtk_stack_get_visible_child_name(inspector_stack)} != "comments")
        gtk_stack_set_visible_child_name(inspector_stack, "image");
    } else if (!snapshot.picture_selected && picture_was_selected &&
               std::string_view{gtk_stack_get_visible_child_name(inspector_stack)} == "image") {
      gtk_stack_set_visible_child_name(inspector_stack, "format");
    }
    picture_was_selected = snapshot.picture_selected;
    if (snapshot.picture_selected) {
      const char* locale = std::setlocale(LC_MEASUREMENT, nullptr);
      const bool metric = locale == nullptr ||
                          std::string_view{locale}.find("_US") == std::string_view::npos;
      const double unit = metric ? 1440.0 / 2.54 : 1440.0;
      char text[32];
      if (!gtk_widget_has_focus(GTK_WIDGET(picture_width)) &&
          !gtk_widget_is_focus(GTK_WIDGET(picture_width))) {
        std::snprintf(text, sizeof(text), metric ? "%.1f" : "%.2f",
                      static_cast<double>(snapshot.picture_width_twips) / unit);
        gtk_editable_set_text(GTK_EDITABLE(picture_width), text);
      }
      if (!gtk_widget_is_focus(GTK_WIDGET(picture_height))) {
        std::snprintf(text, sizeof(text), metric ? "%.1f" : "%.2f",
                      static_cast<double>(snapshot.picture_height_twips) / unit);
        gtk_editable_set_text(GTK_EDITABLE(picture_height), text);
      }
      gtk_entry_set_placeholder_text(picture_width, metric ? "cm" : "in");
    }
  }
  if (table_tab != nullptr) {
    const bool selected = snapshot.selection_context == "Table";
    if (table_size_group != nullptr)
      gtk_widget_set_visible(table_size_group, snapshot.table_rows > 0 && snapshot.table_columns > 0);
    const std::string_view current{gtk_stack_get_visible_child_name(inspector_stack)};
    if (selected && !table_context_dismissed && !snapshot.picture_selected && current != "comments" &&
        current != "advanced")
      gtk_stack_set_visible_child_name(inspector_stack, "table");
    else if (!selected && current == "table")
      gtk_stack_set_visible_child_name(inspector_stack, "format");
    gtk_label_set_text(table_rows_value,
                       snapshot.table_rows > 0 ? std::to_string(snapshot.table_rows).c_str() : "—");
    gtk_label_set_text(table_columns_value,
                       snapshot.table_columns > 0 ? std::to_string(snapshot.table_columns).c_str() : "—");
  }
  rebuild_comments();
  updating_inspector = true;
  if (picture_preview != nullptr) {
    if (shown_picture_preview_path != snapshot.picture_preview_path) {
      if (snapshot.picture_preview_path.empty())
        gtk_picture_set_paintable(picture_preview, nullptr);
      else
        gtk_picture_set_filename(picture_preview, snapshot.picture_preview_path.c_str());
      shown_picture_preview_path = snapshot.picture_preview_path;
    }
    const double fraction = snapshot.picture_width_twips > 0
        ? static_cast<double>(snapshot.picture_width_twips) /
              (snapshot.page_size_label == "A4" ? 9030.0 : 9360.0)
        : 1.0;
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(picture_size_modes),
                                 fraction < 0.46 ? "s" : fraction < 0.7 ? "m" :
                                 fraction < 0.91 ? "l" : "full");
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
    for (const auto& [control, current] :
         {std::pair{picture_flow_choice, snapshot.picture_flow},
          std::pair{picture_anchor_choice, snapshot.picture_anchor}}) {
      if (current.empty()) luma_property_choice_set_mixed(LUMA_PROPERTY_CHOICE(control));
      else luma_property_choice_set_current(LUMA_PROPERTY_CHOICE(control), current.c_str());
    }
#endif
    gtk_switch_set_active(picture_caption_switch, snapshot.picture_has_caption);
    gtk_editable_set_text(GTK_EDITABLE(picture_alt_text), snapshot.picture_alt_text.c_str());
  }
  update_combos();
  // Preserve a partially typed value while unrelated engine state refreshes.
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  const auto unknown_number = [](GtkWidget* widget, bool mixed) {
    luma_property_number_set_mixed(LUMA_PROPERTY_NUMBER(widget));
    const auto visit = [mixed](const auto& self, GtkWidget* child) -> void {
      if (GTK_IS_ENTRY(child)) gtk_entry_set_placeholder_text(GTK_ENTRY(child), mixed ? "Mixed" : "—");
      if (GTK_IS_TEXT(child)) gtk_text_set_placeholder_text(GTK_TEXT(child), mixed ? "Mixed" : "—");
      for (GtkWidget* next = gtk_widget_get_first_child(child); next != nullptr;
           next = gtk_widget_get_next_sibling(next)) self(self, next);
    };
    visit(visit, widget);
  };
  const auto set_number = [this, &unknown_number](GtkWidget* widget, const std::string& value, bool mixed) {
    GtkWidget* focus = gtk_window_get_focus(window);
    if (focus != nullptr && (focus == widget || gtk_widget_is_ancestor(focus, widget))) return;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end != value.c_str() && *end == '\0' && std::isfinite(parsed))
      luma_property_number_set_value(LUMA_PROPERTY_NUMBER(widget), parsed);
    else
      unknown_number(widget, mixed);
  };
  set_number(inspector_size_number, snapshot.size_label, snapshot.size_mixed);
  set_number(inspector_line_height_number, snapshot.line_height_label, snapshot.line_height_mixed);
  const auto set_colour = [this](GtkWidget* widget, const std::string& raw, const char* automatic) {
    GtkWidget* focus = gtk_window_get_focus(window);
    if (focus != nullptr && (focus == widget || gtk_widget_is_ancestor(focus, widget))) return;
    auto* field = LUMA_PROPERTY_COLOR(widget);
    char* end = nullptr;
    const auto value = std::strtol(raw.c_str(), &end, 10);
    if (!raw.empty() && end != raw.c_str() && *end == '\0') {
      if (value < 0) {
        const GdkRGBA transparent{0, 0, 0, 0};
        luma_property_color_set_rgba(field, &transparent);
        luma_property_color_set_word(field, automatic);
      }
      else {
        GdkRGBA rgba{
            static_cast<float>((value >> 16) & 255) / 255.0F,
            static_cast<float>((value >> 8) & 255) / 255.0F,
            static_cast<float>(value & 255) / 255.0F, 1.0F};
        luma_property_color_set_rgba(field, &rgba);
      }
    } else luma_property_color_set_word(field, raw == "mixed" || raw == "indeterminate" ? "Mixed" : "—");
  };
  set_colour(inspector_text_colour, snapshot.text_colour, "Automatic");
  set_colour(inspector_highlight_colour, snapshot.highlight_colour, "None");
#else
  gtk_label_set_text(inspector_line_height_value,
                     snapshot.line_height_label.empty() ? "—" :
                     snapshot.line_height_label.c_str());
#endif
#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  if (snapshot.header_state_known)
    luma_property_choice_set_current(LUMA_PROPERTY_CHOICE(header_choice), snapshot.header_on ? "on" : "off");
  else luma_property_choice_set_mixed(LUMA_PROPERTY_CHOICE(header_choice));
  if (snapshot.footer_state_known)
    luma_property_choice_set_current(LUMA_PROPERTY_CHOICE(footer_choice), snapshot.footer_on ? "on" : "off");
  else luma_property_choice_set_mixed(LUMA_PROPERTY_CHOICE(footer_choice));
#else
  gtk_switch_set_active(header_switch, snapshot.header_on);
  gtk_switch_set_active(footer_switch, snapshot.footer_on);
#endif
  if (line_spacing_segment != nullptr)
    select_segment(line_spacing_segment,
                   snapshot.line_height_label == "1.0"    ? "1"
                   : snapshot.line_height_label == "1.15" ? "1.15"
                   : snapshot.line_height_label == "1.5"  ? "1.5"
                   : snapshot.line_height_label == "2.0"  ? "2"
                                                          : "");

#ifdef WRITE_HAS_CREATIVE_PROPERTIES
  if (snapshot.page_size_label == "Letter" || snapshot.page_size_label == "Legal" ||
      snapshot.page_size_label == "A4" || snapshot.page_size_label == "A5")
    luma_property_choice_set_current(LUMA_PROPERTY_CHOICE(page_size_choice),
                                     snapshot.page_size_label.c_str());
  else choice_word(page_size_choice, "Paper size", "Custom");
  if (snapshot.margins_label.empty()) choice_word(margins_choice, "Page margins", "Choose…");
  else luma_property_choice_set_current(LUMA_PROPERTY_CHOICE(margins_choice),
                                        snapshot.margins_label.c_str());
  luma_property_choice_set_current(LUMA_PROPERTY_CHOICE(orientation_choice),
      snapshot.orientation_label == "Landscape" ? "landscape" : "portrait");
#else
  GtkStringList* sizes = GTK_STRING_LIST(gtk_drop_down_get_model(page_size_dropdown));
  gtk_drop_down_set_selected(page_size_dropdown,
                             string_list_find(sizes, snapshot.page_size_label));
  select_segment(orientation_segment,
                 snapshot.orientation_label == "Landscape" ? "landscape" : "portrait");
#endif
  const auto grouped = [](const int value) {
    std::string digits = std::to_string(std::max(value, 0));
    for (int index = static_cast<int>(digits.size()) - 3; index > 0; index -= 3) {
      digits.insert(static_cast<std::size_t>(index), ",");
    }
    return digits;
  };
  gtk_label_set_text(page_count_value, grouped(std::max(snapshot.page_count, 1)).c_str());
  gtk_label_set_text(page_words_value,
      snapshot.word_count_available ? grouped(snapshot.word_count).c_str() : "—");
  gtk_label_set_text(page_characters_value,
      snapshot.character_count_available ? grouped(snapshot.character_count).c_str() : "—");
  updating_inspector = false;
}

// ── Popovers ───────────────────────────────────────────────────────────────

GtkWidget* WriteWindow::Impl::style_popover_content() {
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_add_css_class(list, "style-menu");
  for (const char* style : kCommonStyles) {
    GtkWidget* button = gtk_button_new();
    GtkWidget* label = left_label(style, "style-sample");
    std::string sample = std::string{"style-sample-"} + style;
    std::transform(sample.begin(), sample.end(), sample.begin(), [](unsigned char c) {
      return c == ' ' ? '-' : static_cast<char>(std::tolower(c));
    });
    gtk_widget_add_css_class(label, sample.c_str());
    gtk_button_set_child(GTK_BUTTON(button), label);
    add_css_classes(button, {"write-menu-row", "style-row"});
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, style,
                                   -1);
    bind_argument(button, ids::write::kApplyStyle, style);
    gtk_box_append(GTK_BOX(list), button);
  }
  style_menu_list = list;
  return list;
}

GtkWidget* WriteWindow::Impl::colour_popover(const std::string_view command_id,
                                             const bool highlight) {
  GtkWidget* popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "write-popover");
  GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(column, "colour-menu");
  gtk_box_append(GTK_BOX(column), left_label(highlight ? "Highlight" : "Text colour",
                                             "inspector-eyebrow"));
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
  int index = 0;
  const auto add_swatch = [&](const Swatch& swatch) {
    GtkWidget* button = gtk_button_new();
    add_css_classes(button, {"colour-swatch"});
    // The swatch colour is data, not chrome: a tiny provider per swatch.
    GtkCssProvider* provider = gtk_css_provider_new();
    const std::string css = std::string{"button { background: "} + swatch.hex + "; }";
    gtk_css_provider_load_from_string(provider, css.c_str());
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_style_context_add_provider(gtk_widget_get_style_context(button),
                                   GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(provider);
    set_accessible_label(button, swatch.name);
    bind_argument(button, command_id, swatch.hex);
    gtk_grid_attach(GTK_GRID(grid), button, index % 5, index / 5, 1, 1);
    ++index;
  };
  if (highlight) {
    for (const auto& swatch : kHighlightSwatches) add_swatch(swatch);
  } else {
    for (const auto& swatch : kTextSwatches) add_swatch(swatch);
  }
  gtk_box_append(GTK_BOX(column), grid);
  GtkWidget* reset = gtk_button_new_with_label(highlight ? "No highlight" : "Automatic");
  add_css_classes(reset, {"luma-button", "small"});
  bind_argument(reset, command_id, "auto");
  gtk_box_append(GTK_BOX(column), reset);
  gtk_popover_set_child(GTK_POPOVER(popover), column);
  return popover;
}

GtkWidget* WriteWindow::Impl::table_popover() {
  GtkWidget* popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "write-popover");
  GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(column, "table-menu");
  GtkWidget* caption = left_label("Table", "inspector-eyebrow");
  gtk_box_append(GTK_BOX(column), caption);
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
  for (int row = 1; row <= 8; ++row) {
    for (int col = 1; col <= 8; ++col) {
      GtkWidget* cell = gtk_button_new();
      add_css_classes(cell, {"table-cell"});
      const std::string size = std::to_string(col) + "x" + std::to_string(row);
      const std::string name = std::to_string(col) + " by " + std::to_string(row) + " table";
      gtk_accessible_update_property(GTK_ACCESSIBLE(cell), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     name.c_str(), -1);
      g_object_set_data(G_OBJECT(cell), "write-col", GINT_TO_POINTER(col));
      g_object_set_data(G_OBJECT(cell), "write-row", GINT_TO_POINTER(row));
      bind_argument(cell, ids::write::kInsertTableSized, size);
      GtkEventController* hover = gtk_event_controller_motion_new();
      g_signal_connect(hover, "enter", G_CALLBACK(+[](GtkEventControllerMotion* controller, double,
                                                      double, gpointer data) {
                         GtkWidget* target = gtk_event_controller_get_widget(
                             GTK_EVENT_CONTROLLER(controller));
                         const int columns = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(target), "write-col"));
                         const int rows = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(target), "write-row"));
                         GtkWidget* table = gtk_widget_get_parent(target);
                         for (GtkWidget* child = gtk_widget_get_first_child(table); child != nullptr;
                              child = gtk_widget_get_next_sibling(child)) {
                           const int c = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "write-col"));
                           const int r = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "write-row"));
                           if (c <= columns && r <= rows) {
                             gtk_widget_add_css_class(child, "chosen");
                           } else {
                             gtk_widget_remove_css_class(child, "chosen");
                           }
                         }
                         const std::string text = std::to_string(columns) + " × " + std::to_string(rows);
                         gtk_label_set_text(GTK_LABEL(data), text.c_str());
                       }),
                       caption);
      gtk_widget_add_controller(cell, hover);
      gtk_grid_attach(GTK_GRID(grid), cell, col - 1, row - 1, 1, 1);
    }
  }
  gtk_box_append(GTK_BOX(column), grid);
  gtk_popover_set_child(GTK_POPOVER(popover), column);
  return popover;
}

GtkWidget* WriteWindow::Impl::character_popover() {
  GtkWidget* popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "write-popover");
  GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(column, "character-menu");
  gtk_box_append(GTK_BOX(column), left_label("Special character", "inspector-eyebrow"));
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
  int index = 0;
  for (const char* character : kCharacters) {
    GtkWidget* button = gtk_button_new_with_label(character);
    add_css_classes(button, {"character-cell"});
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   character, -1);
    bind_argument(button, ids::write::kInsertCharacter, character);
    gtk_grid_attach(GTK_GRID(grid), button, index % 8, index / 8, 1, 1);
    ++index;
  }
  gtk_box_append(GTK_BOX(column), grid);
  gtk_popover_set_child(GTK_POPOVER(popover), column);
  return popover;
}

GtkWidget* WriteWindow::Impl::insert_popover() {
  GtkWidget* popover = gtk_popover_new();
  gtk_widget_add_css_class(popover, "write-popover");
  GtkWidget* list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_add_css_class(list, "insert-menu");
  const auto submenu = [list](const char* icon_name, const char* label,
                                    GtkWidget* child_popover) {
    GtkWidget* button = gtk_menu_button_new();
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(row), icon(icon_name, 16));
    GtkWidget* text = left_label(label, "write-menu-label");
    gtk_widget_set_hexpand(text, TRUE);
    gtk_box_append(GTK_BOX(row), text);
    gtk_box_append(GTK_BOX(row), icon("go-next-symbolic", 12));
    gtk_menu_button_set_child(GTK_MENU_BUTTON(button), row);
    add_css_classes(button, {"write-menu-row", "write-submenu-row"});
    gtk_popover_set_position(GTK_POPOVER(child_popover), GTK_POS_RIGHT);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), child_popover);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                   -1);
    gtk_box_append(GTK_BOX(list), button);
  };
  gtk_box_append(GTK_BOX(list), menu_row("write-image-symbolic", "Image…", nullptr,
                                         ids::write::kInsertImage));
  submenu("write-table-symbolic", "Table", table_popover());
  gtk_box_append(GTK_BOX(list), menu_row("write-link-symbolic", "Link…", "Ctrl+Shift+K",
                                         ids::write::kInsertLink));
  gtk_box_append(GTK_BOX(list), menu_row("write-comment-add-symbolic", "Comment…",
                                         "Ctrl+Alt+M", ids::write::kAddComment));
  gtk_box_append(GTK_BOX(list), menu_row("write-footnote-symbolic", "Footnote", nullptr,
                                         ids::write::kInsertFootnote));
  submenu("write-special-character-symbolic", "Special character", character_popover());
  GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_add_css_class(separator, "write-menu-separator");
  gtk_box_append(GTK_BOX(list), separator);
  gtk_box_append(GTK_BOX(list), menu_row("write-page-break-symbolic", "Page break",
                                         "Ctrl+Enter", ids::write::kInsertPageBreak));
  gtk_box_append(GTK_BOX(list), menu_row("write-page-number-symbolic", "Page numbers", nullptr,
                                         ids::write::kInsertPageNumber));
  gtk_box_append(GTK_BOX(list), menu_row("write-contents-symbolic", "Table of contents",
                                         nullptr, ids::write::kInsertContents));
  gtk_popover_set_child(GTK_POPOVER(popover), list);
  return popover;
}

GtkWidget* WriteWindow::Impl::zoom_popover() {
  auto* menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Document zoom"));
  luma_floating_menu_set_align(menu, LUMA_FLOAT_ALIGN_END);
  luma_floating_menu_add_item(menu, "Fit width", nullptr, nullptr, nullptr,
                              "write.zoom-level('fit-width')", FALSE);
  luma_floating_menu_add_item(menu, "Fit page", nullptr, nullptr, nullptr,
                              "write.zoom-level('fit-page')", FALSE);
  luma_floating_menu_add_separator(menu);
  for (const char* level : {"50", "75", "100", "125", "150", "200"}) {
    const std::string label = std::string{level} + "%";
    const std::string command = std::string{"write.zoom-level('"} + level + "')";
    luma_floating_menu_add_item(menu, label.c_str(), nullptr, nullptr,
        std::string_view{level} == "100" ? "Ctrl+0" : nullptr, command.c_str(),
        snapshot.zoom_percent == std::atoi(level));
  }
  return GTK_WIDGET(menu);
}

}  // namespace luma::write::ui
