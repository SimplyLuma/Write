#include "ui/write_window_private.hpp"
#include "ui/bar_menu.hpp"

#include "commands/command_ids.h"
#include "luma_config.hpp"

#include <luma-corner-pill.h>
#include <luma-action-center.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include <tuple>
#include <utility>

namespace luma::write::ui {

namespace ids = luma::commands::ids;

namespace {

constexpr const char* kCssResource = "/org/projectluma/Write/ui/css/write.css";
constexpr const char* kIconResourceRoot = "/org/projectluma/Write/ui/icons";
constexpr const char* kCompatibilityCommand = "write.document.compatibility";
constexpr const char* kAboutCommand = "suite.app.about";
constexpr const char* kQuitCommand = "suite.app.quit";
// The kit's toolbar glyph size (design/spacing-and-sizing: 14px in toolbars
// and icon buttons, 16px in menus).
constexpr int kToolIconSize = 14;

GtkWidget* child_with_class(GtkWidget* root, const char* css_class) {
  if (root == nullptr) return nullptr;
  if (gtk_widget_has_css_class(root, css_class)) return root;
  for (GtkWidget* child = gtk_widget_get_first_child(root); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (GtkWidget* found = child_with_class(child, css_class)) return found;
  }
  return nullptr;
}

void hide_child_images(GtkWidget* root) {
  if (root == nullptr) return;
  if (GTK_IS_IMAGE(root)) gtk_widget_set_visible(root, FALSE);
  for (GtkWidget* child = gtk_widget_get_first_child(root); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    hide_child_images(child);
  }
}

void hide_photo_size_icons(GtkWidget* root) {
  if (root == nullptr) return;
  if (GTK_IS_ACTIONABLE(root)) {
    const char* action = gtk_actionable_get_action_name(GTK_ACTIONABLE(root));
    if (action != nullptr && g_str_has_prefix(action, "write.photo-size-")) {
      hide_child_images(root);
      if (g_strcmp0(action, "write.photo-size-full") != 0)
        gtk_widget_add_css_class(root, "write-photo-size-compact");
    }
  }
  for (GtkWidget* child = gtk_widget_get_first_child(root); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    hide_photo_size_icons(child);
  }
}

std::string with_thousands(const int value) {
  std::string digits = std::to_string(std::max(value, 0));
  for (int index = static_cast<int>(digits.size()) - 3; index > 0; index -= 3) {
    digits.insert(static_cast<std::size_t>(index), ",");
  }
  return digits;
}

std::string format_zoom(const int percent) {
  return std::to_string(std::clamp(percent, 10, 800)) + "%";
}

void install_visual_resources(GdkDisplay* display) {
  static bool installed = false;
  if (installed || display == nullptr) {
    return;
  }
  installed = true;
  // The kit owns the sheet so it is parsed again when the treatment changes;
  // the canvas names @luma_content and must follow the desktop into dark.
  luma_ui_add_style_resource(kCssResource,
                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1U);
  // Opt-in local styling for the HOTFIX session; the installed app keeps its
  // bundled stylesheet. Monitor the directory so atomic editor saves reload.
  const char* live_css = g_getenv("LUMA_WRITE_HOTFIX_CSS");
  if (g_getenv("LUMA_WRITE_PREVIEW") != nullptr && live_css != nullptr) {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_path(provider, live_css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2U);
    g_object_set_data_full(G_OBJECT(provider), "hotfix-css", g_strdup(live_css), g_free);
    GFile* file = g_file_new_for_path(live_css);
    GFile* parent = g_file_get_parent(file);
    GFileMonitor* monitor = g_file_monitor_directory(parent, G_FILE_MONITOR_NONE,
                                                    nullptr, nullptr);
    g_object_unref(parent);
    g_object_unref(file);
    if (monitor != nullptr) {
      g_signal_connect(monitor, "changed", G_CALLBACK(+[](
          GFileMonitor*, GFile*, GFile*, GFileMonitorEvent event, gpointer data) {
        if (event == G_FILE_MONITOR_EVENT_CHANGED ||
            event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
            event == G_FILE_MONITOR_EVENT_CREATED) {
          auto* css = GTK_CSS_PROVIDER(data);
          const auto* path = static_cast<const char*>(
              g_object_get_data(G_OBJECT(css), "hotfix-css"));
          if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            gtk_css_provider_load_from_path(css, path);
            g_message("HOTFIX stylesheet reloaded");
          }
        }
      }), provider);
      g_object_set_data_full(G_OBJECT(provider), "hotfix-monitor", monitor, g_object_unref);
    }
    g_object_set_data_full(G_OBJECT(display), "write-hotfix-style", provider, g_object_unref);
  }
  GtkIconTheme* theme = gtk_icon_theme_get_for_display(display);
  gtk_icon_theme_add_resource_path(theme, kIconResourceRoot);
}

// A status-bar figure: the number in ink, its unit muted, as the design sets
// them ("313 words").
void set_figure(GtkLabel* label, const int value, const char* singular,
                const char* plural, const int selected = -1) {
  const std::string number = with_thousands(value);
  char* markup = nullptr;
  if (selected >= 0) {
    // "3 of 1,204 words" while text is selected.
    const std::string part = with_thousands(selected);
    markup = g_markup_printf_escaped("<b>%s</b> of %s %s", part.c_str(), number.c_str(),
                                     value == 1 ? singular : plural);
  } else {
    markup = g_markup_printf_escaped("<b>%s</b> %s", number.c_str(),
                                     value == 1 ? singular : plural);
  }
  gtk_label_set_markup(label, markup);
  g_free(markup);
}

bool focus_is_text_entry(GtkWindow* window) {
  GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(window));
  return focus != nullptr && (GTK_IS_TEXT(focus) || GTK_IS_EDITABLE(focus) ||
                              GTK_IS_TEXT_VIEW(focus));
}

}  // namespace

namespace detail {

void add_css_classes(GtkWidget* widget, const std::initializer_list<const char*> classes) {
  for (const char* css_class : classes) {
    gtk_widget_add_css_class(widget, css_class);
  }
}

void set_accessible_label(GtkWidget* widget, const char* label) {
  gtk_widget_set_tooltip_text(widget, label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

GtkWidget* icon(const char* name, const int size) {
  GtkWidget* image = gtk_image_new_from_icon_name(name);
  gtk_image_set_pixel_size(GTK_IMAGE(image), size);
  return image;
}

GtkWidget* left_label(const char* text, const char* css_class) {
  GtkWidget* label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  if (css_class != nullptr) {
    gtk_widget_add_css_class(label, css_class);
  }
  return label;
}

GtkWidget* rule() {
  GtkWidget* line = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(line, "luma-bar-rule");
  gtk_widget_set_valign(line, GTK_ALIGN_CENTER);
  return line;
}

GtkWidget* connected_group(const char* extra_class) {
  GtkWidget* group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(group, "luma-connected-buttons");
  gtk_widget_set_valign(group, GTK_ALIGN_CENTER);
  if (extra_class != nullptr) {
    gtk_widget_add_css_class(group, extra_class);
  }
  return group;
}

}  // namespace detail

using namespace detail;

std::string style_display_name(const std::string_view engine_name) {
  if (engine_name.empty() || engine_name == "Default Paragraph Style" ||
      engine_name == "Standard" || engine_name == "Normal") {
    return "Body";
  }
  if (engine_name == "Text body" || engine_name == "Body Text") {
    return "Body Text";
  }
  if (engine_name == "Quotations" || engine_name == "Quote") {
    return "Quote";
  }
  if (engine_name == "List Paragraph") {
    return "List";
  }
  return std::string{engine_name};
}

std::string style_engine_name(const std::string_view display_name) {
  if (display_name == "Body") {
    return "Default Paragraph Style";
  }
  if (display_name == "Quote") {
    return "Quotations";
  }
  return std::string{display_name};
}


void WriteWindow::Impl::dispatch(const std::string_view command_id) const {
  if (callbacks.on_command) {
    callbacks.on_command(command_id);
  }
}

void WriteWindow::Impl::dispatch_argument(const std::string_view command_id,
                                          const std::string_view argument) const {
  if (callbacks.on_command_argument) {
    callbacks.on_command_argument(command_id, argument);
  }
}

void WriteWindow::Impl::command_clicked(GtkButton*, gpointer data) {
  const auto* binding = static_cast<CommandBinding*>(data);
  binding->self->dispatch(binding->command_id);
}

void WriteWindow::Impl::argument_clicked(GtkButton* button, gpointer data) {
  const auto* binding = static_cast<CommandBinding*>(data);
  // A row inside a popover closes it, as a menu item would.
  if (GtkWidget* popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)) {
    gtk_popover_popdown(GTK_POPOVER(popover));
  }
  binding->self->dispatch_argument(binding->command_id, binding->argument);
}

void WriteWindow::Impl::menu_action_activated(GSimpleAction*, GVariant*, gpointer data) {
  const auto* binding = static_cast<CommandBinding*>(data);
  if (binding->argument.empty()) {
    binding->self->dispatch(binding->command_id);
  } else {
    binding->self->dispatch_argument(binding->command_id, binding->argument);
  }
}

void WriteWindow::Impl::bind_command(GtkWidget* widget, const std::string_view command_id) {
  auto binding = std::make_unique<CommandBinding>();
  binding->self = this;
  binding->command_id = std::string{command_id};
  g_signal_connect(widget, "clicked", G_CALLBACK(command_clicked), binding.get());
  command_widgets.emplace_back(binding->command_id, widget);
  command_bindings.push_back(std::move(binding));
}

void WriteWindow::Impl::bind_argument(GtkWidget* widget, const std::string_view command_id,
                                      std::string argument) {
  auto binding = std::make_unique<CommandBinding>();
  binding->self = this;
  binding->command_id = std::string{command_id};
  binding->argument = std::move(argument);
  g_signal_connect(widget, "clicked", G_CALLBACK(argument_clicked), binding.get());
  command_bindings.push_back(std::move(binding));
}

GtkWidget* WriteWindow::Impl::command_button(const char* icon_name,
                                             const char* accessible_name,
                                             const std::string_view command_id,
                                             const bool toggle) {
  GtkWidget* button = toggle ? gtk_toggle_button_new() : gtk_button_new();
  std::string glyph{icon_name};
  if (glyph.starts_with("write-") && glyph.ends_with("-symbolic")) {
    glyph = glyph.substr(6, glyph.size() - 6 - 9);
    if (glyph == "highlight") glyph = "highlighter";
    if (glyph == "list-bulleted") glyph = "list";
    if (glyph == "list-numbered") glyph = "list-ordered";
    if (glyph == "indent-less") glyph = "indent-decrease";
    if (glyph == "indent-more") glyph = "indent-increase";
    if (glyph == "size-down") glyph = "a-arrow-down";
    if (glyph == "size-up") glyph = "a-arrow-up";
    if (glyph == "list-check") glyph = "list-checks";
    if (glyph == "comment") glyph = "message-square";
    if (glyph == "close") glyph = "x";
  }
  GtkWidget* image = luma_ui_icon_new(glyph.c_str());
  gtk_image_set_pixel_size(GTK_IMAGE(image), kToolIconSize);
  gtk_button_set_child(GTK_BUTTON(button), image);
  gtk_widget_add_css_class(button, "luma-icon-button");
  set_accessible_label(button, accessible_name);
  if (!command_id.empty()) {
    bind_command(button, command_id);
  }
  return button;
}

// A row in one of Write's popover menus: icon, label, shortcut. It closes its
// popover and runs the same command the toolbar and palette run.
GtkWidget* WriteWindow::Impl::menu_row(const char* icon_name, const char* label,
                                       const char* accelerator,
                                       const std::string_view command_id,
                                       std::string argument) {
  GtkWidget* button = gtk_button_new();
  gtk_widget_add_css_class(button, "write-menu-row");
  GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  if (icon_name != nullptr) {
    gtk_box_append(GTK_BOX(row), icon(icon_name, 16));
  }
  GtkWidget* text = left_label(label, "write-menu-label");
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_append(GTK_BOX(row), text);
  if (accelerator != nullptr) {
    gtk_box_append(GTK_BOX(row), left_label(accelerator, "write-menu-accel"));
  }
  gtk_button_set_child(GTK_BUTTON(button), row);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 label, -1);
  if (argument.empty()) {
    auto binding = std::make_unique<CommandBinding>();
    binding->self = this;
    binding->command_id = std::string{command_id};
    g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* self_button, gpointer data) {
                       auto* bound = static_cast<CommandBinding*>(data);
                       if (GtkWidget* popover = gtk_widget_get_ancestor(
                               GTK_WIDGET(self_button), GTK_TYPE_POPOVER)) {
                         gtk_popover_popdown(GTK_POPOVER(popover));
                       }
                       bound->self->dispatch(bound->command_id);
                     }),
                     binding.get());
    command_widgets.emplace_back(binding->command_id, button);
    command_bindings.push_back(std::move(binding));
  } else {
    bind_argument(button, command_id, std::move(argument));
  }
  return button;
}

GMenuModel* WriteWindow::Impl::build_menu_model() {
  GMenu* root = g_menu_new();
  menu_action_group = g_simple_action_group_new();
  const auto add_action = [this](GMenu* section, const char* label,
                                 const char* action_name,
                                 const std::string_view command_id,
                                 const char* argument = nullptr) {
    auto binding = std::make_unique<CommandBinding>();
    binding->self = this;
    binding->command_id = std::string{command_id};
    binding->argument = argument == nullptr ? "" : argument;
    GSimpleAction* action = g_simple_action_new(action_name, nullptr);
    g_signal_connect(action, "activate", G_CALLBACK(menu_action_activated), binding.get());
    g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(action));
    if (argument == nullptr) {
      command_actions.emplace_back(binding->command_id, action);
    }
    command_bindings.push_back(std::move(binding));
    const std::string detailed = std::string{"write."} + action_name;
    g_menu_append(section, label, detailed.c_str());
    g_object_unref(action);
  };
  const auto section = [root](GMenu* items) {
    g_menu_append_section(root, nullptr, G_MENU_MODEL(items));
    g_object_unref(items);
  };

  GMenu* file = g_menu_new();
  add_action(file, "New Document", "new-document", ids::suite::kNewDocument);
  add_action(file, "Open…", "open", ids::suite::kOpen);
  add_action(file, "Save", "save", ids::suite::kSave);
  add_action(file, "Save As…", "save-as", ids::suite::kSaveAs);
  add_action(file, "Export PDF…", "export-pdf", ids::suite::kExportPdf);
  add_action(file, "Print…", "print", ids::suite::kPrint);
  section(file);

  GMenu* edit = g_menu_new();
  add_action(edit, "Find…", "find", ids::suite::kFind);
  add_action(edit, "Find and Replace…", "replace", ids::write::kReplace);
  add_action(edit, "Select All", "select-all", ids::suite::kSelectAll);
  add_action(edit, "Check Spelling as You Type", "spelling", ids::write::kSpelling);
  section(edit);

  GMenu* review = g_menu_new();
  add_action(review, "Track Changes", "track-changes", ids::write::kTrackChanges);
  add_action(review, "Next Change", "next-change", ids::write::kNextChange);
  add_action(review, "Accept All Changes", "accept-all", ids::write::kAcceptAllChanges);
  add_action(review, "Reject All Changes", "reject-all", ids::write::kRejectAllChanges);
  section(review);

  GSimpleAction* style_action = g_simple_action_new("apply-style", G_VARIANT_TYPE_STRING);
  g_signal_connect(style_action, "activate", G_CALLBACK(+[](
      GSimpleAction*, GVariant* value, gpointer data) {
    static_cast<Impl*>(data)->dispatch_argument(ids::write::kApplyStyle,
                                                g_variant_get_string(value, nullptr));
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(style_action));
  g_object_unref(style_action);
  GSimpleAction* choose_style = g_simple_action_new("choose-style", nullptr);
  g_signal_connect(choose_style, "activate", G_CALLBACK(+[](
      GSimpleAction*, GVariant*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    GtkWidget* anchor = gtk_root_get_focus(GTK_ROOT(self->window));
    if (!GTK_IS_BUTTON(anchor)) anchor = self->action_center;
    auto* menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Paragraph style"));
    g_object_ref_sink(menu);
    luma_floating_menu_set_picker(menu, TRUE);
    luma_floating_menu_set_align(menu, LUMA_FLOAT_ALIGN_START);
    std::vector<std::string> names;
    for (const char* name : {"Body", "Title", "Subtitle", "Heading 1", "Heading 2", "Heading 3", "Quote", "List"})
      names.emplace_back(name);
    for (const auto& engine_name : self->snapshot.paragraph_styles) {
      const auto name = style_display_name(engine_name);
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    for (const auto& name : names) {
      GVariant* target = g_variant_ref_sink(g_variant_new_string(name.c_str()));
      gchar* detailed = g_action_print_detailed_name("write.apply-style", target);
      luma_floating_menu_add_item(menu, name.c_str(), nullptr, nullptr, nullptr,
                                  detailed, name == self->snapshot.paragraph_style);
      g_free(detailed);
      g_variant_unref(target);
    }
    luma_floating_menu_popup(menu, anchor);
    g_object_unref(menu);
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(choose_style));
  g_object_unref(choose_style);

  GSimpleAction* font_size = g_simple_action_new("font-size", G_VARIANT_TYPE_STRING);
  g_signal_connect(font_size, "activate", G_CALLBACK(+[](
      GSimpleAction*, GVariant* value, gpointer data) {
    static_cast<Impl*>(data)->dispatch_argument(ids::write::kFontSize,
                                                g_variant_get_string(value, nullptr));
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(font_size));
  g_object_unref(font_size);
  GSimpleAction* choose_size = g_simple_action_new("choose-font-size", nullptr);
  g_signal_connect(choose_size, "activate", G_CALLBACK(+[](
      GSimpleAction*, GVariant*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    GtkWidget* anchor = gtk_root_get_focus(GTK_ROOT(self->window));
    if (!GTK_IS_BUTTON(anchor)) anchor = self->action_center;
    auto* menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Font size"));
    g_object_ref_sink(menu);
    luma_floating_menu_set_picker(menu, TRUE);
    for (const char* size : {"8", "9", "10", "11", "12", "14", "16", "18", "20", "24", "28", "32", "36", "48", "72"}) {
      const std::string label = std::string{size} + " pt";
      const std::string detailed = std::string{"write.font-size('"} + size + "')";
      luma_floating_menu_add_item(menu, label.c_str(), nullptr, nullptr, nullptr,
          detailed.c_str(), std::atof(self->snapshot.size_label.c_str()) == std::atof(size));
    }
    luma_floating_menu_popup(menu, anchor);
    g_object_unref(menu);
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(choose_size));
  g_object_unref(choose_size);

  GSimpleAction* zoom_action = g_simple_action_new("zoom-level", G_VARIANT_TYPE_STRING);
  g_signal_connect(zoom_action, "activate", G_CALLBACK(+[](
      GSimpleAction*, GVariant* value, gpointer data) {
    static_cast<Impl*>(data)->dispatch_argument(ids::write::kZoomTo,
                                                g_variant_get_string(value, nullptr));
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(zoom_action));
  g_object_unref(zoom_action);

  GMenu* view = g_menu_new();
  add_action(view, "Search Commands…", "command-search", ids::suite::kCommandPalette);
  add_action(view, "Moon Page", "page-moon", ids::write::kPageTheme, "moon");
  add_action(view, "Sun Page", "page-sun", ids::write::kPageTheme, "sun");
  add_action(view, "Fit Page Width", "fit-width", ids::write::kZoomTo, "fit-width");
  add_action(view, "Focus", "focus", ids::write::kFocusMode);
  add_action(view, "Advanced Tools", "advanced", ids::write::kAdvancedView);
  section(view);

  GMenu* help = g_menu_new();
  add_action(help, ("About " + identity.product_name).c_str(), "about", kAboutCommand);
  add_action(help, "Quit", "quit", kQuitCommand);
  section(help);

  // The action center uses the same command IDs as menus and shortcuts. The
  // actions are installed even while its C implementation is being finished.
  GMenu* bar_actions = g_menu_new();
  add_action(bar_actions, "Undo", "bar-undo", ids::suite::kUndo);
  add_action(bar_actions, "Redo", "bar-redo", ids::suite::kRedo);
  add_action(bar_actions, "Bold", "bar-bold", ids::write::kBold);
  add_action(bar_actions, "Italic", "bar-italic", ids::write::kItalic);
  add_action(bar_actions, "Underline", "bar-underline", ids::write::kUnderline);
  add_action(bar_actions, "Highlight", "bar-highlight", ids::write::kHighlight);
  add_action(bar_actions, "Link", "bar-link", ids::write::kInsertLink);
  add_action(bar_actions, "Bulleted list", "bar-bullets", "write.paragraph.bullets");
  add_action(bar_actions, "Numbered list", "bar-numbering", "write.paragraph.numbering");
  add_action(bar_actions, "Align left", "bar-align-left", "write.paragraph.align-left");
  add_action(bar_actions, "Align centre", "bar-align-center", "write.paragraph.align-center");
  add_action(bar_actions, "Align right", "bar-align-right", "write.paragraph.align-right");
  add_action(bar_actions, "Justify", "bar-align-justify", "write.paragraph.align-justify");
  // The plus opens the insert chooser; inserting a table requires an explicit
  // choice from that menu.
  GSimpleAction* insert_action = g_simple_action_new("bar-insert", nullptr);
  g_signal_connect(insert_action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    if (self->action_center == nullptr) return;
    GtkWidget* popover = self->insert_popover();
    if (luma_ui_is_phone_width(GTK_WIDGET(self->window))) {
      g_object_ref_sink(popover);
      GtkWidget* content=gtk_popover_get_child(GTK_POPOVER(popover));
      g_object_ref(content);
      gtk_popover_set_child(GTK_POPOVER(popover),nullptr);
      luma_action_center_grow_panel(LUMA_ACTION_CENTER(self->action_center),"insert",content);
      g_object_unref(content);g_object_unref(popover);return;
    }
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_TOP);
    gtk_widget_set_parent(popover, self->action_center);
    g_signal_connect_swapped(popover, "closed", G_CALLBACK(gtk_widget_unparent), popover);
    gtk_popover_popup(GTK_POPOVER(popover));
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(insert_action));
  g_object_unref(insert_action);
  for (const char* name : {"bar-share", "bar-more"}) {
    GSimpleAction* popup_action = g_simple_action_new(name, nullptr);
    g_object_set_data_full(G_OBJECT(popup_action), "wr-popup-kind", g_strdup(name), g_free);
    g_signal_connect(popup_action, "activate", G_CALLBACK(+[](GSimpleAction* source, GVariant*, gpointer data) {
      auto* self = static_cast<Impl*>(data);
      const auto* kind = static_cast<const char*>(g_object_get_data(G_OBJECT(source), "wr-popup-kind"));
      LumaFloatingMenu* menu = g_strcmp0(kind, "bar-share") == 0 ? self->share_menu : self->more_menu;
      if (menu == nullptr || self->action_center == nullptr) return;
      GtkWidget* anchor = gtk_root_get_focus(GTK_ROOT(self->window));
      if (anchor == nullptr || !gtk_widget_is_ancestor(anchor, self->action_center))
        anchor = self->action_center;
      luma_floating_menu_popup(menu, anchor);
    }), this);
    g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(popup_action));
    g_object_unref(popup_action);
  }
  add_action(bar_actions, "Comment", "bar-comment", ids::write::kAddComment);
  add_action(bar_actions, "Insert table row", "bar-table-row", ids::write::kRunAdvanced,
             ".uno:InsertRowsAfter");
  add_action(bar_actions, "Insert table column", "bar-table-column", ids::write::kRunAdvanced,
             ".uno:InsertColumnsAfter");
  add_action(bar_actions, "Delete table row", "bar-table-delete-row", ids::write::kRunAdvanced,
             ".uno:DeleteRows");
  add_action(bar_actions, "Delete table column", "bar-table-delete-column",
             ids::write::kRunAdvanced, ".uno:DeleteColumns");
  add_action(bar_actions, "Delete table", "bar-table-delete", ids::write::kRunAdvanced,
             ".uno:DeleteTable");
  add_action(bar_actions, "Replace picture", "bar-photo-replace", ids::write::kInsertImage);
  add_action(bar_actions, "Caption picture", "photo-caption", ids::write::kPictureAction,
             "caption");
  add_action(bar_actions, "Delete picture", "photo-delete", ids::write::kPictureAction,
             "delete");
  GSimpleAction* done_action = g_simple_action_new("context-done", nullptr);
  g_signal_connect(done_action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    if (self->snapshot.selection_context == "Table") {
      self->table_context_dismissed = true;
      self->shown_action_center_key.clear();
      self->rebuild_action_center();
      self->open_inspector_tab("format");
      self->update_inspector();
      if (self->canvas != nullptr) gtk_widget_child_focus(self->canvas, GTK_DIR_TAB_FORWARD);
      return;
    }
    self->dispatch_argument(ids::write::kPictureAction, "done");
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(done_action));
  g_object_unref(done_action);
  GSimpleAction* table_tools = g_simple_action_new("table-tools", nullptr);
  g_signal_connect(table_tools, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    self->table_context_dismissed = false;
    self->shown_action_center_key.clear();
    self->rebuild_action_center();
    self->open_inspector_tab("format");
    self->update_inspector();
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(table_tools));
  g_object_unref(table_tools);
  for (const auto& [name, percent] :
       std::array<std::pair<const char*, int>, 4>{
           std::pair{"photo-size-s", 34}, std::pair{"photo-size-m", 58},
           std::pair{"photo-size-l", 82}, std::pair{"photo-size-full", 100}}) {
    GSimpleAction* size_action = g_simple_action_new(name, nullptr);
    g_object_set_data(G_OBJECT(size_action), "wr-size-percent", GINT_TO_POINTER(percent));
    g_signal_connect(size_action, "activate", G_CALLBACK(+[](GSimpleAction* source, GVariant*,
                                                                gpointer data) {
      auto* self = static_cast<Impl*>(data);
      if (self->snapshot.picture_width_twips <= 0 ||
          self->snapshot.picture_height_twips <= 0) return;
      const int chosen = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(source), "wr-size-percent"));
      const long long page_width = self->snapshot.page_size_label == "A4" ? 9030 : 9360;
      const long long width = std::llround(static_cast<double>(page_width) * chosen / 100.0);
      const long long height = std::llround(
          static_cast<double>(width) *
          static_cast<double>(self->snapshot.picture_height_twips) /
          static_cast<double>(self->snapshot.picture_width_twips));
      self->dispatch_argument(ids::write::kPictureSize,
                              std::to_string(width) + "," + std::to_string(height));
    }), this);
    g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(size_action));
    g_object_unref(size_action);
  }
  GSimpleAction* photo_sizes=g_simple_action_new("phone-photo-size",nullptr);
  g_signal_connect(photo_sizes,"activate",G_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer data) {
    auto* self=static_cast<Impl*>(data);
    GtkWidget* panel=luma_bar_panel_new("Size");
    GtkWidget* sizes=luma_bar_tiles_new(4,TRUE,FALSE);
    for(const auto& [label,key]:std::array<std::pair<const char*,const char*>,4>{std::pair{"Small","s"},{"Medium","m"},{"Large","l"},{"Full","full"}}) {
      const std::string command=std::string("write.photo-size-")+key;
      luma_bar_tiles_add(sizes,"scaling",label,command.c_str(),FALSE,FALSE);
    }
    gtk_box_append(GTK_BOX(panel),sizes);
    luma_action_center_grow_panel(LUMA_ACTION_CENTER(self->action_center),"photo-size",panel);
  }),this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group),G_ACTION(photo_sizes));
  g_object_unref(photo_sizes);
  g_object_unref(bar_actions);

  const auto panel_action = [this](const char* name, const bool comments) {
    GSimpleAction* action = g_simple_action_new(name, nullptr);
    g_object_set_data(G_OBJECT(action), "wr-comments", GINT_TO_POINTER(comments));
    g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction* source, GVariant*, gpointer data) {
      auto* self = static_cast<Impl*>(data);
      self->open_inspector_tab(g_object_get_data(G_OBJECT(source), "wr-comments") != nullptr
                                   ? "comments" : "format");
    }), this);
    g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(action));
    g_object_unref(action);
  };
  panel_action("show-format", false);
  panel_action("show-comments", true);
  GSimpleAction* alignment_action = g_simple_action_new("show-alignment", nullptr);
  g_signal_connect(alignment_action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    GtkWidget* anchor = gtk_root_get_focus(GTK_ROOT(self->window));
    if (anchor == nullptr || !gtk_widget_is_ancestor(anchor, self->action_center))
      anchor = self->action_center;
    if (self->alignment_menu != nullptr) {
      luma_floating_menu_close(self->alignment_menu);
      g_object_unref(self->alignment_menu);
    }
    self->alignment_menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Alignment"));
    g_object_ref_sink(self->alignment_menu);
    luma_floating_menu_set_picker(self->alignment_menu, TRUE);
    for (const auto& [label, glyph, suffix] :
         std::array<std::tuple<const char*, const char*, const char*>, 4>{
             std::tuple{"Left", "align-left", "left"},
             std::tuple{"Centre", "align-center", "center"},
             std::tuple{"Right", "align-right", "right"},
             std::tuple{"Justify", "align-justify", "justify"}}) {
      const std::string command = std::string{"write.paragraph.align-"} + suffix;
      const auto state = self->command_states.find(command);
      const bool selected = state != self->command_states.end() &&
                            state->second.checked == commands::CheckState::Checked;
      const std::string action = std::string{"write.bar-align-"} + suffix;
      luma_floating_menu_add_item(self->alignment_menu, label, glyph, nullptr, nullptr,
                                  action.c_str(), selected);
    }
    luma_floating_menu_popup(self->alignment_menu, anchor);
  }), this);
  g_action_map_add_action(G_ACTION_MAP(menu_action_group), G_ACTION(alignment_action));
  g_object_unref(alignment_action);
  return G_MENU_MODEL(root);
}

gboolean WriteWindow::Impl::window_close_requested(GtkWindow*, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  return self->callbacks.on_close_requested && self->callbacks.on_close_requested()
             ? TRUE
             : FALSE;
}

GtkWidget* WriteWindow::Impl::build_start_page() {
  GtkWidget* welcome = luma_welcome_view_new(
      "Write", "org.projectluma.Write",
      "A calm place for letters, reports and the work that needs to read exactly as you intended.",
      "document", "org.projectluma.Write");
  bind_command(luma_welcome_view_get_new_button(LUMA_WELCOME_VIEW(welcome)),
               ids::suite::kNewDocument);
  bind_command(luma_welcome_view_get_open_button(LUMA_WELCOME_VIEW(welcome)),
               ids::suite::kOpen);
  return welcome;
}

// The document island's toolbar, laid out like Notes': a kit toolbar at the
// top of the island it controls, connected groups of kit icon buttons, 8px
// between groups and no rules. Order follows the design: history | style |
// marks | colour and link | alignment | lists | indent | Insert … find,
// comment | panels.
namespace {

// A kit text button carried by a GtkMenuButton: the kit styles the inner
// button, so there is one frame, not a frame inside a frame.
void style_menu_button_as_kit_button(GtkWidget* menu_button, const char* extra_class) {
  gtk_widget_add_css_class(menu_button, extra_class);
  if (GtkWidget* inner = gtk_widget_get_first_child(menu_button); inner != nullptr) {
    gtk_widget_add_css_class(inner, "luma-button");
    gtk_widget_add_css_class(inner, "small");
  }
}

// An icon button that opens a popover, made from a plain kit icon button so
// it measures exactly like its neighbours in a connected group.
GtkWidget* icon_menu_button(const char* icon_name, const char* accessible_name,
                            GtkWidget* popover) {
  GtkWidget* button = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(button), detail::icon(icon_name, kToolIconSize));
  gtk_widget_add_css_class(button, "luma-icon-button");
  detail::set_accessible_label(button, accessible_name);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_HAS_POPUP,
                                 TRUE, -1);
  gtk_widget_set_parent(popover, button);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
  g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     gtk_popover_popup(GTK_POPOVER(data));
                   }),
                   popover);
  g_signal_connect(button, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                     gtk_widget_unparent(GTK_WIDGET(data));
                   }),
                   popover);
  return button;
}

}  // namespace

GtkWidget* WriteWindow::Impl::build_command_bar() {
  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_name(bar, "wr-action-bar");
  add_css_classes(bar, {"luma-toolbar", "write-toolbar"});
  gtk_widget_set_vexpand(bar, FALSE);

  GtkWidget* history = connected_group("history-group");
  gtk_box_append(GTK_BOX(history),
                 command_button("write-undo-symbolic", "Undo", ids::suite::kUndo));
  gtk_box_append(GTK_BOX(history),
                 command_button("write-redo-symbolic", "Redo", ids::suite::kRedo));
  gtk_box_append(GTK_BOX(bar), history);

  // The paragraph style picker: the one strong control in the view, because
  // styles carry the document.
  GtkWidget* style = gtk_menu_button_new();
  GtkWidget* style_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* style_value = left_label("Body", "write-style-value");
  gtk_label_set_ellipsize(GTK_LABEL(style_value), PANGO_ELLIPSIZE_END);
  gtk_label_set_width_chars(GTK_LABEL(style_value), 10);
  gtk_label_set_max_width_chars(GTK_LABEL(style_value), 14);
  gtk_widget_set_hexpand(style_value, TRUE);
  style_button_value = GTK_LABEL(style_value);
  gtk_box_append(GTK_BOX(style_content), style_value);
  gtk_box_append(GTK_BOX(style_content), icon("write-chevron-down-symbolic", 12));
  gtk_menu_button_set_child(GTK_MENU_BUTTON(style), style_content);
  style_menu_button_as_kit_button(style, "write-style-picker");
  set_accessible_label(style, "Paragraph style");
  style_popover = gtk_popover_new();
  gtk_widget_add_css_class(style_popover, "write-popover");
  gtk_popover_set_child(GTK_POPOVER(style_popover), style_popover_content());
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(style), style_popover);
  gtk_box_append(GTK_BOX(bar), style);

  // Typeface and size, as Docs places them: the same combos as the inspector.
  GtkWidget* face = GTK_WIDGET(combo_field(ComboKind::font, "Typeface")->entry);
  gtk_widget_add_css_class(face, "write-toolbar-font");
  gtk_box_append(GTK_BOX(bar), face);
  GtkWidget* size = size_stepper();
  gtk_widget_add_css_class(size, "size-group");
  gtk_box_append(GTK_BOX(bar), size);

  GtkWidget* marks = connected_group("marks-group");
  gtk_box_append(GTK_BOX(marks),
                 command_button("write-bold-symbolic", "Bold", ids::write::kBold, true));
  gtk_box_append(GTK_BOX(marks),
                 command_button("write-italic-symbolic", "Italic", ids::write::kItalic, true));
  gtk_box_append(GTK_BOX(marks), command_button("write-underline-symbolic", "Underline",
                                                ids::write::kUnderline, true));
  gtk_box_append(GTK_BOX(marks), command_button("write-strikethrough-symbolic",
                                                "Strikethrough",
                                                ids::write::kStrikethrough, true));
  gtk_box_append(GTK_BOX(bar), marks);

  GtkWidget* colours = connected_group("colour-group");
  gtk_box_append(GTK_BOX(colours),
                 icon_menu_button("write-text-color-symbolic", "Text colour",
                                  colour_popover(ids::write::kTextColor, false)));
  gtk_box_append(GTK_BOX(colours),
                 icon_menu_button("write-highlight-symbolic", "Highlight",
                                  colour_popover(ids::write::kHighlight, true)));
  gtk_box_append(GTK_BOX(bar), colours);

  // Alignment is one button showing the paragraph's alignment; its popover
  // holds the four, as Docs does when space is short.
  GtkWidget* alignment = connected_group("alignment-group");
  GtkWidget* alignments = connected_group("write-alignment-choices");
  gtk_box_append(GTK_BOX(alignments), command_button("write-align-left-symbolic", "Align left",
                                                     "write.paragraph.align-left", true));
  gtk_box_append(GTK_BOX(alignments), command_button("write-align-center-symbolic",
                                                     "Align center",
                                                     "write.paragraph.align-center", true));
  gtk_box_append(GTK_BOX(alignments), command_button("write-align-right-symbolic",
                                                     "Align right",
                                                     "write.paragraph.align-right", true));
  gtk_box_append(GTK_BOX(alignments), command_button("write-align-justify-symbolic", "Justify",
                                                     "write.paragraph.align-justify", true));
  GtkWidget* alignment_popover = gtk_popover_new();
  add_css_classes(alignment_popover, {"write-popover", "write-alignment-popover"});
  gtk_popover_set_child(GTK_POPOVER(alignment_popover), alignments);
  alignment_button = icon_menu_button("write-align-left-symbolic", "Alignment", alignment_popover);
  gtk_box_append(GTK_BOX(alignment), alignment_button);
  gtk_box_append(GTK_BOX(bar), alignment);

  GtkWidget* lists = connected_group("list-group");
  gtk_box_append(GTK_BOX(lists), command_button("write-list-bulleted-symbolic",
                                                "Bulleted list",
                                                "write.paragraph.bullets", true));
  gtk_box_append(GTK_BOX(lists), command_button("write-list-numbered-symbolic",
                                                "Numbered list",
                                                "write.paragraph.numbering", true));
  gtk_box_append(GTK_BOX(lists), command_button("write-list-check-symbolic", "Checklist",
                                                ids::write::kChecklist));
  gtk_box_append(GTK_BOX(bar), lists);

  GtkWidget* indent = connected_group("indent-group");
  gtk_box_append(GTK_BOX(indent), command_button("write-indent-less-symbolic",
                                                 "Decrease indent", ids::write::kIndentLess));
  gtk_box_append(GTK_BOX(indent), command_button("write-indent-more-symbolic",
                                                 "Increase indent", ids::write::kIndentMore));
  gtk_box_append(GTK_BOX(bar), indent);

  GtkWidget* insert = gtk_menu_button_new();
  GtkWidget* insert_content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(insert_content), icon("write-plus-symbolic", kToolIconSize));
  gtk_box_append(GTK_BOX(insert_content), gtk_label_new("Insert"));
  gtk_menu_button_set_child(GTK_MENU_BUTTON(insert), insert_content);
  style_menu_button_as_kit_button(insert, "write-insert-button");
  set_accessible_label(insert, "Insert");
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(insert), insert_popover());
  gtk_box_append(GTK_BOX(bar), insert);

  GtkWidget* flexible = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(flexible, TRUE);
  gtk_box_append(GTK_BOX(bar), flexible);

  GtkWidget* review = connected_group("review-group");
  find_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
  gtk_widget_set_name(GTK_WIDGET(find_toggle), "wr-find-toggle");
  gtk_button_set_child(GTK_BUTTON(find_toggle), icon("write-search-symbolic", kToolIconSize));
  gtk_widget_add_css_class(GTK_WIDGET(find_toggle), "luma-icon-button");
  set_accessible_label(GTK_WIDGET(find_toggle), "Find and replace");
  g_signal_connect(find_toggle, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     const bool open = gtk_toggle_button_get_active(button) != FALSE;
                     if (open) self->open_find(false);
                     else luma_action_center_fold(LUMA_ACTION_CENTER(self->action_center));
                   }),
                   this);
  gtk_box_append(GTK_BOX(review), GTK_WIDGET(find_toggle));
  gtk_box_append(GTK_BOX(review), command_button("write-comment-symbolic", "Add comment",
                                                 ids::write::kAddComment));
  gtk_box_append(GTK_BOX(bar), review);

  GtkWidget* view = connected_group("view-group");
  navigator_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
  gtk_widget_set_name(GTK_WIDGET(navigator_toggle), "wr-outline-toggle");
  gtk_button_set_child(GTK_BUTTON(navigator_toggle),
                       icon("write-panel-left-symbolic", kToolIconSize));
  gtk_widget_add_css_class(GTK_WIDGET(navigator_toggle), "luma-icon-button");
  set_accessible_label(GTK_WIDGET(navigator_toggle), "Show outline");
  g_signal_connect(navigator_toggle, "toggled", G_CALLBACK(navigator_toggled), this);
  gtk_box_append(GTK_BOX(view), GTK_WIDGET(navigator_toggle));
  inspector_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
  gtk_widget_set_name(GTK_WIDGET(inspector_toggle), "wr-format-toggle");
  gtk_button_set_child(GTK_BUTTON(inspector_toggle),
                       icon("write-panel-right-symbolic", kToolIconSize));
  gtk_widget_add_css_class(GTK_WIDGET(inspector_toggle), "luma-icon-button");
  set_accessible_label(GTK_WIDGET(inspector_toggle), "Show inspector");
  g_signal_connect(inspector_toggle, "toggled", G_CALLBACK(inspector_toggled), this);
  gtk_box_append(GTK_BOX(view), GTK_WIDGET(inspector_toggle));
  gtk_box_append(GTK_BOX(bar), view);

  luma_ui_centre_controls(bar);
  command_bar = bar;
  return bar;
}

void WriteWindow::Impl::build_find_bar() {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_name(box, "wr-find");
  find_panel = box;
  g_object_ref_sink(find_panel);

  GtkWidget* find_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  find_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_search_entry_set_placeholder_text(find_entry, "Find in document");
  gtk_widget_set_hexpand(GTK_WIDGET(find_entry), TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(find_entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Find in document", -1);
  gtk_widget_set_name(GTK_WIDGET(find_entry), "wr-find-entry");
  g_object_ref_sink(find_entry);
  find_count = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(find_count), "find-count");
  gtk_widget_set_hexpand(GTK_WIDGET(find_count), TRUE);
  gtk_label_set_xalign(find_count, 0.0F);
  gtk_box_append(GTK_BOX(find_row), GTK_WIDGET(find_count));
  GtkWidget* steps = connected_group();
  GtkWidget* previous = command_button("write-chevron-up-symbolic", "Previous match", "");
  GtkWidget* next = command_button("write-chevron-down-symbolic", "Next match", "");
  gtk_box_append(GTK_BOX(steps), previous);
  gtk_box_append(GTK_BOX(steps), next);
  gtk_box_append(GTK_BOX(find_row), steps);
  replace_toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
  gtk_button_set_child(GTK_BUTTON(replace_toggle), icon("write-replace-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(replace_toggle), "write-tool");
  set_accessible_label(GTK_WIDGET(replace_toggle), "Show replace");
  gtk_box_append(GTK_BOX(find_row), GTK_WIDGET(replace_toggle));

  gtk_box_append(GTK_BOX(box), find_row);

  replace_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_name(replace_row, "wr-find-replace");
  replace_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(replace_entry, "Replace with");
  gtk_widget_set_hexpand(GTK_WIDGET(replace_entry), TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(replace_entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Replace with", -1);
  gtk_box_append(GTK_BOX(replace_row), GTK_WIDGET(replace_entry));
  GtkWidget* replace_one = gtk_button_new_with_label("Replace");
  add_css_classes(replace_one, {"luma-button", "small"});
  GtkWidget* replace_all = gtk_button_new_with_label("Replace All");
  add_css_classes(replace_all, {"luma-button", "small"});
  GtkWidget* replacement_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(replacement_actions, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(replacement_actions), replace_one);
  gtk_box_append(GTK_BOX(replacement_actions), replace_all);
  gtk_box_append(GTK_BOX(replace_row), replacement_actions);
  gtk_widget_set_visible(replace_row, FALSE);
  gtk_box_append(GTK_BOX(box), replace_row);

  // Search as the person types, briefly debounced so a long document is not
  // searched once per keystroke.
  g_signal_connect(find_entry, "search-changed", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     if (!self->find_mode) return;
                     const char* text = gtk_editable_get_text(GTK_EDITABLE(entry));
                     if (text == nullptr || *text == '\0') {
                       gtk_label_set_text(self->find_count, "");
                       return;
                     }
                     self->dispatch_argument(ids::write::kFindNext, text);
                   }),
                   this);
  g_signal_connect(find_entry, "activate", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                     static_cast<Impl*>(data)->dispatch_argument(
                         ids::write::kFindNext, gtk_editable_get_text(GTK_EDITABLE(entry)));
                   }),
                   this);
  g_signal_connect(find_entry, "next-match", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                     static_cast<Impl*>(data)->dispatch_argument(
                         ids::write::kFindNext, gtk_editable_get_text(GTK_EDITABLE(entry)));
                   }),
                   this);
  g_signal_connect(find_entry, "previous-match", G_CALLBACK(+[](GtkSearchEntry* entry, gpointer data) {
                     static_cast<Impl*>(data)->dispatch_argument(
                         ids::write::kFindPrevious, gtk_editable_get_text(GTK_EDITABLE(entry)));
                   }),
                   this);
  g_signal_connect(find_entry, "stop-search", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     luma_action_center_fold(LUMA_ACTION_CENTER(self->action_center));
                   }),
                   this);
  g_signal_connect(next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     self->dispatch_argument(ids::write::kFindNext,
                                             gtk_editable_get_text(GTK_EDITABLE(self->find_entry)));
                   }),
                   this);
  g_signal_connect(previous, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     self->dispatch_argument(ids::write::kFindPrevious,
                                             gtk_editable_get_text(GTK_EDITABLE(self->find_entry)));
                   }),
                   this);
  g_signal_connect(replace_toggle, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     gtk_widget_set_visible(self->replace_row,
                                            gtk_toggle_button_get_active(button));
                   }),
                   this);
  // Find and replacement text travel together, separated by a unit separator
  // that neither can contain.
  const auto replace_handler = +[](GtkButton* button, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    const bool all = g_object_get_data(G_OBJECT(button), "write-replace-all") != nullptr;
    std::string argument = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    argument += '\x1f';
    argument += gtk_editable_get_text(GTK_EDITABLE(self->replace_entry));
    self->dispatch_argument(all ? ids::write::kReplaceAll : ids::write::kReplaceOne, argument);
  };
  g_object_set_data(G_OBJECT(replace_all), "write-replace-all", GINT_TO_POINTER(1));
  g_signal_connect(replace_one, "clicked", G_CALLBACK(replace_handler), this);
  g_signal_connect(replace_all, "clicked", G_CALLBACK(replace_handler), this);
  g_signal_connect(replace_entry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     std::string argument = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
                     argument += '\x1f';
                     argument += gtk_editable_get_text(GTK_EDITABLE(self->replace_entry));
                     self->dispatch_argument(ids::write::kReplaceOne, argument);
                   }),
                   this);
  g_signal_connect(action_center, "notify::grown", G_CALLBACK(+[](GObject* center, GParamSpec*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    if (!self->find_mode || g_strcmp0(luma_action_center_get_grown(LUMA_ACTION_CENTER(center)), "find") == 0)
      return;
    self->find_mode = false;
    self->shown_action_center_key.clear();
    if (self->canvas != nullptr) gtk_widget_grab_focus(self->canvas);
    // Folding is still removing its children. Rebuild the current formatting
    // context on the next iteration, after the kit finishes returning the row.
    if (self->action_center_refresh_timeout == 0)
      self->action_center_refresh_timeout = g_timeout_add(50, +[](gpointer inner) -> gboolean {
        auto* current = static_cast<Impl*>(inner);
        current->action_center_refresh_timeout = 0;
        current->rebuild_action_center();
        return G_SOURCE_REMOVE;
      }, self);
  }), this);
}

void WriteWindow::Impl::open_find(const bool replace) {
  if (!find_mode) {
    luma_action_center_fold(LUMA_ACTION_CENTER(action_center));
    rebuild_action_center();
    find_mode = true;
    luma_action_center_grow_entry(LUMA_ACTION_CENTER(action_center), "find", find_panel,
                                  GTK_WIDGET(find_entry));
  }
  gtk_toggle_button_set_active(replace_toggle, replace);
  gtk_widget_grab_focus(GTK_WIDGET(find_entry));
  gtk_editable_select_region(GTK_EDITABLE(find_entry), 0, -1);
}

// The status bar belongs to the document island: one quiet row of text and
// flat buttons, counts on the left, save state, page theme and zoom on the
// right, as Notes and the design have it.
GtkWidget* WriteWindow::Impl::build_status_bar() {
  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  status_bar = bar;
  gtk_widget_set_name(bar, "wr-status");
  add_css_classes(bar, {"luma-status-bar", "write-status-bar"});

  // Counts first, each with the rule after it, then the page. The counts
  // stay hidden until the engine has finished counting a long document.
  status_count_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  const auto figure = [this](GtkLabel** out) {
    GtkWidget* label = gtk_label_new("");
    add_css_classes(label, {"luma-status-text", "write-status-text"});
    *out = GTK_LABEL(label);
    gtk_box_append(GTK_BOX(status_count_group), label);
    GtkWidget* rule_line = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(rule_line, "write-status-rule");
    gtk_widget_set_valign(rule_line, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(status_count_group), rule_line);
  };
  figure(&word_count_label);
  figure(&character_count_label);
  figure(&read_time_label);
  GtkWidget* status_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  // The counts sit in a wrapper a narrow window hides; the counts themselves
  // show once the engine has counted.
  status_counts_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(status_counts_wrapper), status_count_group);
  gtk_box_append(GTK_BOX(status_left), status_counts_wrapper);
  page_label = GTK_LABEL(gtk_label_new(""));
  add_css_classes(GTK_WIDGET(page_label), {"luma-status-text", "write-status-text"});
  gtk_box_append(GTK_BOX(status_left), GTK_WIDGET(page_label));
  gtk_box_append(GTK_BOX(bar), status_left);

  GtkWidget* flexible = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(flexible, TRUE);
  gtk_box_append(GTK_BOX(bar), flexible);

  status_compatibility_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  compatibility_button = GTK_BUTTON(gtk_button_new_with_label("Compatibility"));
  add_css_classes(GTK_WIDGET(compatibility_button), {"write-status-button", "compatibility"});
  bind_command(GTK_WIDGET(compatibility_button), kCompatibilityCommand);
  gtk_box_append(GTK_BOX(status_compatibility_group), GTK_WIDGET(compatibility_button));
  gtk_widget_set_visible(status_compatibility_group, FALSE);
  gtk_box_append(GTK_BOX(bar), status_compatibility_group);

  // Save state is text, as in Notes; clicking it saves.
  save_button = GTK_BUTTON(gtk_button_new());
  save_state = GTK_LABEL(gtk_label_new(snapshot.save_state_label.c_str()));
  add_css_classes(GTK_WIDGET(save_state), {"luma-status-text", "write-status-text"});
  gtk_button_set_child(save_button, GTK_WIDGET(save_state));
  add_css_classes(GTK_WIDGET(save_button), {"write-status-button"});
  set_accessible_label(GTK_WIDGET(save_button), "Save document");
  bind_command(GTK_WIDGET(save_button), ids::suite::kSave);
  gtk_box_append(GTK_BOX(bar), GTK_WIDGET(save_button));
  save_spinner = nullptr;

  // Moon and Sun: one flat button showing the current page. How the page is
  // shown, never what the document says.
  theme_button = GTK_BUTTON(gtk_button_new());
  add_css_classes(GTK_WIDGET(theme_button), {"luma-icon-button", "quiet", "write-status-tool"});
  gtk_button_set_child(theme_button, icon("write-moon-symbolic", kToolIconSize));
  g_signal_connect(theme_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     self->dispatch_argument(ids::write::kPageTheme,
                                             self->snapshot.moon ? "sun" : "moon");
                   }),
                   this);
  gtk_box_append(GTK_BOX(bar), GTK_WIDGET(theme_button));

  GtkWidget* zoom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(zoom, "write-zoom");
  GtkWidget* zoom_out = command_button("write-zoom-out-symbolic", "Zoom out",
                                       ids::suite::kZoomOut);
  add_css_classes(zoom_out, {"quiet", "write-status-tool"});
  gtk_box_append(GTK_BOX(zoom), zoom_out);
  zoom_button = GTK_BUTTON(gtk_button_new());
  zoom_label = GTK_LABEL(gtk_label_new("100%"));
  add_css_classes(GTK_WIDGET(zoom_label), {"luma-status-text", "write-status-text", "write-zoom-value"});
  gtk_button_set_child(zoom_button, GTK_WIDGET(zoom_label));
  add_css_classes(GTK_WIDGET(zoom_button), {"write-zoom-button"});
  set_accessible_label(GTK_WIDGET(zoom_button), "Zoom level");
  g_signal_connect(zoom_button, "clicked", G_CALLBACK(+[](GtkButton* anchor, gpointer data) {
    GtkWidget* menu = static_cast<Impl*>(data)->zoom_popover();
    g_object_ref_sink(menu);
    luma_floating_menu_popup(LUMA_FLOATING_MENU(menu), GTK_WIDGET(anchor));
    g_object_unref(menu);
  }), this);
  gtk_box_append(GTK_BOX(zoom), GTK_WIDGET(zoom_button));
  GtkWidget* zoom_in = command_button("write-zoom-in-symbolic", "Zoom in",
                                      ids::suite::kZoomIn);
  add_css_classes(zoom_in, {"quiet", "write-status-tool"});
  gtk_box_append(GTK_BOX(zoom), zoom_in);
  gtk_box_append(GTK_BOX(bar), zoom);
  luma_ui_centre_controls(bar);
  return bar;
}

// The document island: toolbar, find bar, the pages, status bar.
GtkWidget* WriteWindow::Impl::build_canvas_island() {
  GtkWidget* island = luma_pane_new();
  gtk_widget_set_name(island, "wr-island");
  gtk_widget_add_css_class(island, "write-document-island");
  gtk_widget_set_overflow(island, GTK_OVERFLOW_HIDDEN);
  GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(column, TRUE);
  gtk_widget_set_vexpand(column, TRUE);
  GtkWidget* legacy_bar = build_command_bar();
  gtk_box_append(GTK_BOX(column), legacy_bar);
  {
    GtkWidget* center = luma_action_center_new(nullptr);
    gtk_widget_set_name(center, "wr-action-center");
    g_object_ref_sink(center);
    action_center = center;
    rebuild_action_center();
    gtk_widget_set_visible(legacy_bar, FALSE);
  }
  build_find_bar();

  GtkWidget* host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(host, TRUE);
  gtk_widget_set_vexpand(host, TRUE);
  gtk_widget_add_css_class(host, "write-canvas-host");
  canvas_stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_hexpand(GTK_WIDGET(canvas_stack), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(canvas_stack), TRUE);
  gtk_stack_set_transition_type(canvas_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  canvas_unavailable = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign(canvas_unavailable, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(canvas_unavailable, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(canvas_unavailable, "luma-empty-state");
  gtk_box_append(GTK_BOX(canvas_unavailable), icon("write-document-symbolic", 24));
  GtkWidget* unavailable_title = gtk_label_new("Document view unavailable");
  gtk_widget_add_css_class(unavailable_title, "write-empty-title");
  gtk_box_append(GTK_BOX(canvas_unavailable), unavailable_title);
  GtkWidget* detail = gtk_label_new(
      "The document engine has not been connected to this window.");
  gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
  gtk_label_set_justify(GTK_LABEL(detail), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(detail, "luma-empty-description");
  gtk_box_append(GTK_BOX(canvas_unavailable), detail);
  gtk_stack_add_named(canvas_stack, canvas_unavailable, "unavailable");
  gtk_stack_set_visible_child_name(canvas_stack, "unavailable");
  gtk_box_append(GTK_BOX(host), GTK_WIDGET(canvas_stack));
  gtk_box_append(GTK_BOX(column), host);
  GtkWidget* legacy_status = build_status_bar();
  gtk_widget_set_visible(legacy_status, FALSE);
  gtk_box_append(GTK_BOX(column), legacy_status);
  luma_pane_set_child(LUMA_PANE(island), column);
  canvas_island = island;
  return island;
}

void WriteWindow::Impl::rebuild_action_center() {
  if (action_center == nullptr) return;
  if (page_stack != nullptr &&
      gtk_stack_get_visible_child_name(page_stack) != nullptr &&
      std::string_view{gtk_stack_get_visible_child_name(page_stack)} == "start") {
    shown_action_center_key.clear();
    luma_action_center_hide_bar(LUMA_ACTION_CENTER(action_center));
    return;
  }
  if (find_mode) return;
  std::vector<LumaBarItem*> items;
  const auto action = [&items](const char* icon_name, const char* label,
                               const char* action_name, const char* tooltip = nullptr) {
    LumaBarItem* item = luma_bar_item_new_action(icon_name, label, action_name);
    if (tooltip != nullptr)
      luma_bar_item_set_tooltip(item, tooltip);
    items.push_back(item);
    return item;
  };
  const auto separator = [&items] { items.push_back(luma_bar_item_new_separator()); };
  const auto chip = [&items](const char* label, const char* icon_name) {
    items.push_back(luma_bar_item_new_chip(label, icon_name, FALSE));
  };
  // A newly built window has not mapped yet. Its default size is already the
  // desktop size; treating the unmapped state as phone hid most of the bar in
  // the first frame and in headless captures.
  const bool narrow = window != nullptr && gtk_widget_get_width(GTK_WIDGET(window)) > 0 &&
                      gtk_widget_get_width(GTK_WIDGET(window)) < 720;
  // Snapshot changes after every keystroke. Only rebuild when the visible
  // controls actually change, so typing cannot flash the entire bar.
  const std::string bar_key = std::string{narrow ? "phone|" : "desktop|"} +
      snapshot.selection_context + "|" + snapshot.paragraph_style + "|" + snapshot.size_label + "|" +
      (snapshot.size_mixed ? "mixed-size|" : "single-size|") +
      (snapshot.picture_selected ? "photo|" : "text|") +
      (table_context_dismissed ? "dismissed|" : "active|") +
      std::to_string(snapshot.picture_width_twips) + "|" + snapshot.page_size_label;
  if (shown_action_center_key == bar_key) return;
  shown_action_center_key = bar_key;
  if (luma_ui_is_phone_width(GTK_WIDGET(window))) {
    luma_action_center_set_phone_wide(LUMA_ACTION_CENTER(action_center),TRUE);
    if(snapshot.picture_selected || snapshot.selection_context=="Photo" || snapshot.selection_context=="Picture") {
      action("scaling",nullptr,"write.phone-photo-size","Size");
      action("captions",nullptr,"write.photo-caption","Caption");
      action("image",nullptr,"write.bar-photo-replace","Replace");
      luma_bar_item_set_danger(action("trash-2",nullptr,"write.photo-delete","Delete photo"),TRUE);
      auto* done=action("check","Done","write.context-done");
      luma_bar_item_set_primary(done,TRUE);luma_bar_item_set_keep_label(done,TRUE);
    } else if(snapshot.selection_context=="Table" && !table_context_dismissed) {
      action("between-horizontal-end",nullptr,"write.bar-table-row","Add row");
      action("between-vertical-end",nullptr,"write.bar-table-column","Add column");
      luma_bar_item_set_danger(action("trash-2",nullptr,"write.bar-table-delete-row","Delete row"),TRUE);
      auto* done=action("check","Done","write.context-done");
      luma_bar_item_set_primary(done,TRUE);luma_bar_item_set_keep_label(done,TRUE);
    } else {
      action("type",nullptr,"write.show-format","Style and format");
      action("plus",nullptr,"write.bar-insert","Insert");
      action("message-square",nullptr,"write.show-comments","Comments");
      action("share-2",nullptr,"write.bar-share","Share");
      action("ellipsis",nullptr,"write.bar-more","More");
    }
    luma_action_center_show_bar(LUMA_ACTION_CENTER(action_center),items.data(),static_cast<guint>(items.size()),nullptr);
    for(auto* item:items)g_object_unref(item);
    return;
  }

  if (snapshot.picture_selected || snapshot.selection_context == "Photo" ||
      snapshot.selection_context == "Picture") {
    chip("Photo", narrow ? nullptr : "image");
    const double fraction = snapshot.picture_width_twips > 0
        ? static_cast<double>(snapshot.picture_width_twips) /
              (snapshot.page_size_label == "A4" ? 9030.0 : 9360.0)
        : 0.0;
    for (const auto& [name, key, glyph, tooltip, target] :
         std::array<std::tuple<const char*, const char*, const char*, const char*, double>, 4>{
             std::tuple{"S", "s", "minimize-2", "Small", 0.34},
             std::tuple{"M", "m", "square", "Medium", 0.58},
             std::tuple{"L", "l", "maximize-2", "Large", 0.82},
             std::tuple{"Full", "full", "maximize-2", "Full width", 1.0}}) {
      const std::string command = std::string{"write.photo-size-"} + key;
      LumaBarItem* size = action(glyph, name, command.c_str(), tooltip);
      luma_bar_item_set_active(size, std::abs(fraction - target) < 0.04);
    }
    separator();
    action("captions", "Caption", "write.photo-caption");
    if (!narrow) {
      separator();
      action("image", "Replace", "write.bar-photo-replace");
      separator();
      LumaBarItem* remove = action("trash-2", nullptr, "write.photo-delete", "Delete photo");
      luma_bar_item_set_danger(remove, TRUE);
      separator();
      LumaBarItem* done = action("check", "Done", "write.context-done");
      luma_bar_item_set_primary(done, TRUE);
    }
  } else if (snapshot.selection_context == "Table" && !table_context_dismissed) {
    chip("Table", "table");
    action("between-horizontal-end", "Row", "write.bar-table-row");
    action("between-vertical-end", "Column", "write.bar-table-column");
    separator();
    LumaBarItem* remove = action("trash-2", nullptr, "write.bar-table-delete-row", "Delete row");
    luma_bar_item_set_danger(remove, TRUE);
    separator();
    LumaBarItem* done = action("check", "Done", "write.context-done");
    luma_bar_item_set_primary(done, TRUE);
  } else {
    items.push_back(luma_bar_item_new_widget(bar_menu_new(
        snapshot.paragraph_style.c_str(), "write.choose-style", "Paragraph style")));
    const std::string size_text = snapshot.size_mixed ? "Mixed" :
                                  snapshot.size_label.empty() ? "—" : snapshot.size_label;
    items.push_back(luma_bar_item_new_widget(bar_menu_new(
        size_text.c_str(), "write.choose-font-size", "Font size")));
    separator();
    action("bold", nullptr, "write.bar-bold", "Bold");
    action("italic", nullptr, "write.bar-italic", "Italic");
    action("underline", nullptr, "write.bar-underline", "Underline");
    if (!narrow) {
      action("highlighter", nullptr, "write.bar-highlight", "Highlight");
      action("link", nullptr, "write.bar-link", "Link");
      separator();
      action("list", nullptr, "write.bar-bullets", "Bulleted list");
      action("list-ordered", nullptr, "write.bar-numbering", "Numbered list");
      separator();
    }
    action("align-left", nullptr, "write.show-alignment", "Alignment");
    separator();
    action("plus", nullptr, "write.bar-insert", "Insert");
    action("message-square", narrow ? nullptr : "Comment", "write.bar-comment", "Comment");
  }
  separator();
  if (!narrow) {
    action("undo", nullptr, "write.bar-undo", "Undo");
    action("redo", nullptr, "write.bar-redo", "Redo");
  }
  if (!narrow || !(snapshot.picture_selected || snapshot.selection_context == "Photo" ||
                   snapshot.selection_context == "Picture"))
    action("ellipsis", nullptr, "write.bar-more", "More document actions");
  luma_action_center_show_bar(reinterpret_cast<LumaActionCenter*>(action_center),
                              items.data(), static_cast<guint>(items.size()), nullptr);
  if (narrow) hide_photo_size_icons(action_center);
  for (LumaBarItem* item : items) g_object_unref(item);
  sync_action_center_states();
}

void WriteWindow::Impl::sync_action_center_states() {
  if (action_center == nullptr) return;
  const auto visit = [this](const auto& recurse, GtkWidget* widget) -> void {
    if (GTK_IS_ACTIONABLE(widget)) {
      const char* detailed = gtk_actionable_get_action_name(GTK_ACTIONABLE(widget));
      if (g_strcmp0(detailed, "write.show-alignment") == 0 && GTK_IS_BUTTON(widget)) {
        const char* glyph = "type";
        const char* label = "Choose alignment";
        bool mixed = false;
        int active_count = 0;
        bool enabled = false;
        for (const auto& [suffix, icon, description] :
             std::array<std::tuple<const char*, const char*, const char*>, 4>{
                 std::tuple{"left", "align-left", "Alignment: Left"},
                 std::tuple{"center", "align-center", "Alignment: Centre"},
                 std::tuple{"right", "align-right", "Alignment: Right"},
                 std::tuple{"justify", "align-justify", "Alignment: Justified"}}) {
          const auto found = command_states.find(std::string{"write.paragraph.align-"} + suffix);
          if (found == command_states.end()) continue;
          enabled = enabled || found->second.enabled;
          mixed = mixed || found->second.checked == commands::CheckState::Mixed;
          if (found->second.checked == commands::CheckState::Checked) {
            ++active_count;
            glyph = icon;
            label = description;
          }
        }
        mixed = mixed || active_count > 1;
        if (mixed) { glyph = "type"; label = "Alignment: Mixed"; }
        if (g_strcmp0(static_cast<const char*>(g_object_get_data(G_OBJECT(widget), "write-alignment-glyph")), glyph) != 0) {
          gtk_button_set_child(GTK_BUTTON(widget), luma_ui_icon_new(glyph));
          g_object_set_data_full(G_OBJECT(widget), "write-alignment-glyph", g_strdup(glyph), g_free);
        }
        gtk_widget_set_tooltip_text(widget, label);
        set_accessible_label(widget, label);
        gtk_widget_set_sensitive(widget, enabled);
        if (mixed) gtk_widget_add_css_class(widget, "mixed");
        else gtk_widget_remove_css_class(widget, "mixed");
      }
      for (const auto& [id, action] : command_actions) {
        if (detailed == nullptr || std::string{"write."} + g_action_get_name(G_ACTION(action)) != detailed)
          continue;
        const auto found = command_states.find(id);
        if (found == command_states.end()) continue;
        const auto& state = found->second;
        gtk_widget_set_sensitive(widget, state.enabled);
        if (state.checked == commands::CheckState::Checked) gtk_widget_add_css_class(widget, "on");
        else gtk_widget_remove_css_class(widget, "on");
        if (state.checked == commands::CheckState::Mixed) gtk_widget_add_css_class(widget, "mixed");
        else gtk_widget_remove_css_class(widget, "mixed");
        {
          const auto pressed = state.checked == commands::CheckState::Mixed
              ? GTK_ACCESSIBLE_TRISTATE_MIXED : state.checked == commands::CheckState::Checked
              ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE;
          gtk_accessible_update_state(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_STATE_PRESSED, pressed, -1);
        }
      }
    }
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) recurse(recurse, child);
  };
  visit(visit, action_center);
}

// Three islands on the window ground, 9px apart: outline, document,
// inspector. The title row above them is the toolkit's, exactly as in Notes.
GtkWidget* WriteWindow::Impl::build_editor_page() {
  GtkWidget* responsive = nullptr;
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  navigator_panel = build_navigator();
  navigator_island = luma_floating_panel_new("Outline", "list");
  gtk_widget_set_name(navigator_island, "wr-outline");
  luma_floating_panel_set_holds(LUMA_FLOATING_PANEL(navigator_island), "outline");
  luma_floating_panel_set_closable(LUMA_FLOATING_PANEL(navigator_island), TRUE);
  luma_floating_panel_set_child(LUMA_FLOATING_PANEL(navigator_island), navigator_panel);

  inspector_panel = build_inspector();
  inspector_island = luma_floating_panel_new("Body", "pilcrow");
  gtk_widget_set_name(inspector_island, "wr-inspector");
  luma_floating_panel_set_holds(LUMA_FLOATING_PANEL(inspector_island), "inspector");
  luma_floating_panel_set_closable(LUMA_FLOATING_PANEL(inspector_island), TRUE);
  luma_floating_panel_set_child(LUMA_FLOATING_PANEL(inspector_island), inspector_panel);

  // The authored page has a wide natural size. Give its viewport a small
  // minimum, then allocate the canvas at the actual available width. GTK's
  // OverlayLayout still allocated the child at its 519px minimum on a phone.
  GtkWidget* canvas_viewport = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_layout_manager(canvas_viewport, gtk_custom_layout_new(
      nullptr,
      +[](GtkWidget*, GtkOrientation, int, int* minimum, int* natural,
          int* minimum_baseline, int* natural_baseline) {
        *minimum = 1;
        *natural = 1;
        *minimum_baseline = -1;
        *natural_baseline = -1;
      },
      +[](GtkWidget* viewport, int width, int height, int baseline) {
        if (GtkWidget* child = gtk_widget_get_first_child(viewport)) {
          GtkAllocation allocation{0, 0, width, height};
          gtk_widget_size_allocate(child, &allocation, baseline);
        }
      }));
  GtkWidget* document_canvas = build_canvas_island();
  gtk_widget_set_hexpand(document_canvas, TRUE);
  gtk_widget_set_vexpand(document_canvas, TRUE);
  canvas_layer_host = luma_layer_host_new(document_canvas);
  gtk_widget_set_hexpand(canvas_layer_host, TRUE);
  gtk_widget_set_vexpand(canvas_layer_host, TRUE);
  gtk_box_append(GTK_BOX(canvas_viewport), canvas_layer_host);
  creative_workspace = luma_creative_workspace_new(canvas_viewport);
  gtk_widget_set_name(creative_workspace, "wr-main");
  gtk_widget_set_hexpand(creative_workspace, TRUE);
  gtk_widget_set_vexpand(creative_workspace, TRUE);
  luma_creative_workspace_set_grid_visible(LUMA_CREATIVE_WORKSPACE(creative_workspace), FALSE);
  luma_creative_workspace_set_left(LUMA_CREATIVE_WORKSPACE(creative_workspace),
                                   LUMA_FLOATING_PANEL(navigator_island));
  luma_creative_workspace_set_right(LUMA_CREATIVE_WORKSPACE(creative_workspace),
                                    LUMA_FLOATING_PANEL(inspector_island));
  g_signal_connect(navigator_island, "notify::folded", G_CALLBACK(panel_visibility_changed), this);
  g_signal_connect(inspector_island, "notify::folded", G_CALLBACK(panel_visibility_changed), this);
  // The workspace changes its phone layout after allocation, not via a
  // GtkWidget "width" property notification. Reconcile after each frame;
  // apply_responsive() only mutates state when the phone threshold changes.
  gtk_widget_add_tick_callback(creative_workspace,
      +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
        static_cast<Impl*>(data)->apply_responsive();
        return G_SOURCE_CONTINUE;
      }, this, nullptr);
  navigator_requested = false;
  inspector_requested = true;
  luma_floating_panel_set_folded(LUMA_FLOATING_PANEL(inspector_island), FALSE);
  responsive = creative_workspace;
#else
  navigator_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  inspector_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  gtk_widget_set_name(GTK_WIDGET(navigator_view), "wr-outline-split");
  gtk_widget_set_name(GTK_WIDGET(inspector_view), "wr-inspector-split");
  gtk_widget_set_vexpand(GTK_WIDGET(navigator_view), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(navigator_view), TRUE);
  adw_overlay_split_view_set_sidebar_position(navigator_view, GTK_PACK_START);
  adw_overlay_split_view_set_sidebar_position(inspector_view, GTK_PACK_END);
  for (auto* view : {navigator_view, inspector_view}) {
    adw_overlay_split_view_set_sidebar_width_unit(view, ADW_LENGTH_UNIT_PX);
    // Side by side, islands cast their shadow past the split view's edge.
    gtk_widget_set_overflow(GTK_WIDGET(view), GTK_OVERFLOW_VISIBLE);
  }
  // Island widths from the design; the 9px gutter is a margin on the
  // document island, so a hidden panel takes its gutter with it.
  adw_overlay_split_view_set_min_sidebar_width(navigator_view, 244);
  adw_overlay_split_view_set_max_sidebar_width(navigator_view, 244);
  adw_overlay_split_view_set_min_sidebar_width(inspector_view, 268);
  adw_overlay_split_view_set_max_sidebar_width(inspector_view, 268);

  navigator_panel = build_navigator();
  navigator_island = luma_pane_new();
  gtk_widget_set_name(navigator_island, "wr-outline");
  gtk_widget_set_valign(navigator_island, GTK_ALIGN_START);
  gtk_widget_set_margin_top(navigator_island, 10);
  add_css_classes(navigator_island, {"write-side-island", "write-outline-island"});
  gtk_widget_set_overflow(navigator_island, GTK_OVERFLOW_HIDDEN);
  luma_pane_set_child(LUMA_PANE(navigator_island), navigator_panel);
  adw_overlay_split_view_set_sidebar(navigator_view, navigator_island);

  GtkWidget* document_island = build_canvas_island();
  adw_overlay_split_view_set_content(inspector_view, document_island);

  inspector_panel = build_inspector();
  inspector_island = luma_pane_new();
  gtk_widget_set_name(inspector_island, "wr-inspector");
  add_css_classes(inspector_island, {"write-side-island", "write-inspector-island"});
  gtk_widget_set_overflow(inspector_island, GTK_OVERFLOW_HIDDEN);
  luma_pane_set_child(LUMA_PANE(inspector_island), inspector_panel);
  adw_overlay_split_view_set_sidebar(inspector_view, inspector_island);
  adw_overlay_split_view_set_content(navigator_view, GTK_WIDGET(inspector_view));

  responsive = adw_breakpoint_bin_new();
  gtk_widget_set_name(responsive, "wr-main");
  gtk_widget_add_css_class(responsive, "editor-page");
  gtk_widget_set_size_request(responsive, 1, 1);
  adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(responsive), GTK_WIDGET(navigator_view));
  // A breakpoint bin applies one breakpoint at a time (the last added that
  // matches), so each carries its whole state: whether the panels collapse
  // into overlays, and which toolbar groups the document island sheds (they
  // are all in the inspector and the command search). They are added from
  // the widest to the narrowest.
  const auto add_breakpoint = [this, responsive](const char* condition, const bool collapse,
                                                 std::initializer_list<const char*> names,
                                                 std::initializer_list<GtkWidget*> extra = {}) {
    AdwBreakpoint* step = adw_breakpoint_new(adw_breakpoint_condition_parse(condition));
    if (collapse) {
      GValue on = G_VALUE_INIT;
      g_value_init(&on, G_TYPE_BOOLEAN);
      g_value_set_boolean(&on, TRUE);
      for (auto* view : {navigator_view, inspector_view}) {
        adw_breakpoint_add_setter(step, G_OBJECT(view), "collapsed", &on);
      }
      adw_breakpoint_add_setter(step, G_OBJECT(inspector_back), "visible", &on);
      g_value_unset(&on);
    }
    GValue off = G_VALUE_INIT;
    g_value_init(&off, G_TYPE_BOOLEAN);
    g_value_set_boolean(&off, FALSE);
    for (const char* name : names) {
      for (GtkWidget* child = gtk_widget_get_first_child(command_bar); child != nullptr;
           child = gtk_widget_get_next_sibling(child)) {
        if (gtk_widget_has_css_class(child, name)) {
          adw_breakpoint_add_setter(step, G_OBJECT(child), "visible", &off);
        }
      }
    }
    for (GtkWidget* widget : extra) {
      adw_breakpoint_add_setter(step, G_OBJECT(widget), "visible", &off);
    }
    g_value_unset(&off);
    adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(responsive), step);
  };
  // Side by side, the island narrows with the window.
  add_breakpoint("max-width: 1639px", false, {"indent-group"});
  add_breakpoint("max-width: 1519px", false, {"indent-group", "colour-group"});
  add_breakpoint("max-width: 1419px", false, {"indent-group", "colour-group", "list-group"});
  add_breakpoint("max-width: 1329px", false,
                 {"indent-group", "colour-group", "list-group", "size-group", "alignment-group"});
  add_breakpoint("max-width: 1239px", false,
                 {"indent-group", "colour-group", "list-group", "size-group", "alignment-group",
                  "write-toolbar-font"});
  // Narrower, the panels become overlays and the island has the width again.
  add_breakpoint("max-width: 939px", true, {"indent-group"});
  add_breakpoint("max-width: 839px", true, {"indent-group", "colour-group", "list-group"});
  add_breakpoint("max-width: 739px", true,
                 {"indent-group", "colour-group", "list-group", "size-group", "alignment-group"});
  // A phone-sized window keeps the page count, save state, page theme and
  // zoom in the status bar; the counts are in the Page tab.
  add_breakpoint("max-width: 699px", true,
                 {"indent-group", "colour-group", "list-group", "size-group", "alignment-group",
                  "marks-group", "write-toolbar-font"},
                 {status_counts_wrapper});
  add_breakpoint("max-width: 519px", true,
                 {"indent-group", "colour-group", "list-group", "size-group", "alignment-group",
                  "marks-group", "write-toolbar-font", "history-group", "review-group"},
                 {status_bar});
  for (auto* view : {navigator_view, inspector_view}) {
    g_signal_connect(view, "notify::show-sidebar", G_CALLBACK(panel_visibility_changed), this);
    g_signal_connect(view, "notify::collapsed", G_CALLBACK(window_size_changed), this);
  }
  navigator_requested = false;
  inspector_requested = true;
#endif
  GtkWidget* main_overlay = gtk_overlay_new();
  gtk_widget_set_name(main_overlay, "wr-main-overlay");
  gtk_overlay_set_child(GTK_OVERLAY(main_overlay), responsive);
  GtkWidget* drawer = gtk_revealer_new();
  mobile_inspector_drawer = GTK_REVEALER(drawer);
  gtk_widget_set_name(drawer, "wr-mobile-inspector-drawer");
  gtk_revealer_set_transition_type(mobile_inspector_drawer,
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_widget_set_halign(drawer, GTK_ALIGN_FILL);
  gtk_widget_set_valign(drawer, GTK_ALIGN_END);
  gtk_widget_set_margin_start(drawer, 8);
  gtk_widget_set_margin_end(drawer, 8);
  gtk_widget_set_margin_bottom(drawer, 8);
  gtk_overlay_add_overlay(GTK_OVERLAY(main_overlay), drawer);
  GtkWidget* page_pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  page_footer = page_pill;
  gtk_widget_add_css_class(page_pill, "write-page-position");
  gtk_widget_set_name(page_pill, "wr-page-position");
  gtk_widget_set_halign(page_pill, GTK_ALIGN_START);
  gtk_widget_set_valign(page_pill, GTK_ALIGN_END);
  gtk_widget_set_margin_start(page_pill, 16);
  gtk_widget_set_margin_bottom(page_pill, 26);
  page_overlay_label = GTK_LABEL(gtk_label_new("Page 1 of 1"));
  gtk_box_append(GTK_BOX(page_pill), GTK_WIDGET(page_overlay_label));
  gtk_overlay_add_overlay(GTK_OVERLAY(main_overlay), page_pill);

  GtkWidget* zoom_pill = gtk_button_new();
  zoom_footer = zoom_pill;
  gtk_widget_add_css_class(zoom_pill, "write-footer-pill");
  gtk_widget_set_halign(zoom_pill, GTK_ALIGN_END);
  gtk_widget_set_valign(zoom_pill, GTK_ALIGN_END);
  gtk_widget_set_margin_end(zoom_pill, 16);
  gtk_widget_set_margin_bottom(zoom_pill, 16);
  GtkWidget* zoom_face = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  zoom_overlay_label = GTK_LABEL(gtk_label_new("100%"));
  gtk_box_append(GTK_BOX(zoom_face), GTK_WIDGET(zoom_overlay_label));
  gtk_box_append(GTK_BOX(zoom_face), icon("write-chevron-down-symbolic", 12));
  gtk_button_set_child(GTK_BUTTON(zoom_pill), zoom_face);
  g_signal_connect(zoom_pill, "clicked", G_CALLBACK(+[](GtkButton* anchor, gpointer data) {
    GtkWidget* menu = static_cast<Impl*>(data)->zoom_popover();
    g_object_ref_sink(menu);
    luma_floating_menu_popup(LUMA_FLOATING_MENU(menu), GTK_WIDGET(anchor));
    g_object_unref(menu);
  }), this);
  set_accessible_label(zoom_pill, "Zoom level");
  gtk_overlay_add_overlay(GTK_OVERLAY(main_overlay), zoom_pill);
  {
    share_menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Share document"));
    g_object_ref_sink(share_menu);
    luma_floating_menu_add_item(share_menu, "Export PDF…", "share", nullptr, nullptr,
                                "write.export-pdf", FALSE);
    luma_floating_menu_add_item(share_menu, "Save a copy…", "copy", nullptr, nullptr,
                                "write.save-as", FALSE);
    more_menu = LUMA_FLOATING_MENU(luma_floating_menu_new("Document actions"));
    g_object_ref_sink(more_menu);
    luma_floating_menu_add_item(more_menu, "Find in document", "search", nullptr, nullptr,
                                "write.find", FALSE);
    luma_floating_menu_add_separator(more_menu);
    luma_floating_menu_add_item(more_menu, "Save a copy…", "copy", nullptr, nullptr,
                                "write.save-as", FALSE);
    luma_floating_menu_add_item(more_menu, "Export PDF…", "share", nullptr, nullptr,
                                "write.export-pdf", FALSE);
    luma_floating_menu_add_item(more_menu, "Print…", "printer", nullptr, nullptr,
                                "write.print", FALSE);
  }
  return main_overlay;
}

void WriteWindow::Impl::build_command_palette() {
  command_palette = adw_dialog_new();
  g_object_ref_sink(command_palette);
  adw_dialog_set_title(command_palette, "Search commands");
  adw_dialog_set_content_width(command_palette, 520);
  adw_dialog_set_content_height(command_palette, 420);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(content, "write-command-search");
  GtkWidget* search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class(search_row, "palette-search-row");
  gtk_box_append(GTK_BOX(search_row), icon("write-search-symbolic", 16));
  command_search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  gtk_widget_set_hexpand(GTK_WIDGET(command_search), TRUE);
  gtk_search_entry_set_placeholder_text(command_search,
                                        "Try “bold”, “save as”, or “zoom in”");
  gtk_widget_add_css_class(GTK_WIDGET(command_search), "palette-search-entry");
  g_signal_connect(command_search, "search-changed", G_CALLBACK(palette_search_changed), this);
  // GtkSearchEntry consumes Escape as stop-search before it can reach the
  // dialog. Close through the dialog's public API and retain focus restoration.
  g_signal_connect(command_search, "stop-search", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                     adw_dialog_close(static_cast<Impl*>(data)->command_palette);
                   }),
                   this);
  gtk_box_append(GTK_BOX(search_row), GTK_WIDGET(command_search));
  GtkWidget* esc = gtk_label_new("Esc");
  gtk_widget_add_css_class(esc, "shortcut-hint");
  gtk_box_append(GTK_BOX(search_row), esc);
  gtk_box_append(GTK_BOX(content), search_row);

  command_results = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(command_results, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(command_results), "palette-results");
  GtkWidget* results_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(results_scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(results_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroll),
                                GTK_WIDGET(command_results));
  gtk_box_append(GTK_BOX(content), results_scroll);
  command_empty_label =
      GTK_LABEL(gtk_label_new("Search every available command in plain language."));
  gtk_label_set_wrap(command_empty_label, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(command_empty_label), "palette-empty");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(command_empty_label));
  palette_footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(palette_footer, "palette-footer");
  gtk_box_append(GTK_BOX(palette_footer), gtk_label_new("Enter runs the first command"));
  gtk_box_append(GTK_BOX(content), palette_footer);
  GtkWidget* toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
  adw_dialog_set_child(command_palette, toolbar);
  g_signal_connect(command_search, "activate", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                     auto* self = static_cast<Impl*>(data);
                     GtkListBoxRow* first = gtk_list_box_get_row_at_index(self->command_results, 0);
                     if (first != nullptr) {
                       GtkWidget* button = gtk_list_box_row_get_child(first);
                       if (button != nullptr && gtk_widget_get_sensitive(button)) {
                         g_signal_emit_by_name(button, "clicked");
                       }
                     }
                   }),
                   this);
}

namespace {

struct Shortcut {
  guint key;
  bool shift;
  bool alt;
  std::string_view command;
  const char* argument;
  // Also active while a text field (find, palette) has focus.
  bool global;
};

// Word and Docs agree on most of these; where they differ, the Docs binding
// wins unless the suite contract already owns the key (Ctrl+K is Search
// Commands in every Luma app, so a link is Ctrl+Shift+K).
constexpr std::array kShortcuts{
    Shortcut{GDK_KEY_n, false, false, ids::suite::kNewDocument, nullptr, true},
    Shortcut{GDK_KEY_o, false, false, ids::suite::kOpen, nullptr, true},
    Shortcut{GDK_KEY_s, false, false, ids::suite::kSave, nullptr, true},
    Shortcut{GDK_KEY_s, true, false, ids::suite::kSaveAs, nullptr, true},
    Shortcut{GDK_KEY_p, false, false, ids::suite::kPrint, nullptr, true},
    Shortcut{GDK_KEY_q, false, false, "suite.app.quit", nullptr, true},
    Shortcut{GDK_KEY_f, false, false, ids::suite::kFind, nullptr, true},
    Shortcut{GDK_KEY_h, false, false, ids::write::kReplace, nullptr, true},
    Shortcut{GDK_KEY_equal, false, false, ids::suite::kZoomIn, nullptr, true},
    Shortcut{GDK_KEY_plus, false, false, ids::suite::kZoomIn, nullptr, true},
    Shortcut{GDK_KEY_plus, true, false, ids::suite::kZoomIn, nullptr, true},
    Shortcut{GDK_KEY_minus, false, false, ids::suite::kZoomOut, nullptr, true},
    Shortcut{GDK_KEY_0, false, false, ids::write::kZoomTo, "100", true},
    Shortcut{GDK_KEY_z, false, false, ids::suite::kUndo, nullptr, false},
    Shortcut{GDK_KEY_z, true, false, ids::suite::kRedo, nullptr, false},
    Shortcut{GDK_KEY_y, false, false, ids::suite::kRedo, nullptr, false},
    Shortcut{GDK_KEY_a, false, false, ids::suite::kSelectAll, nullptr, false},
    Shortcut{GDK_KEY_b, false, false, ids::write::kBold, nullptr, false},
    Shortcut{GDK_KEY_i, false, false, ids::write::kItalic, nullptr, false},
    Shortcut{GDK_KEY_u, false, false, ids::write::kUnderline, nullptr, false},
    Shortcut{GDK_KEY_x, true, false, ids::write::kStrikethrough, nullptr, false},
    Shortcut{GDK_KEY_5, true, true, ids::write::kStrikethrough, nullptr, false},
    Shortcut{GDK_KEY_period, false, false, ids::write::kSuperscript, nullptr, false},
    Shortcut{GDK_KEY_comma, false, false, ids::write::kSubscript, nullptr, false},
    Shortcut{GDK_KEY_greater, true, false, ids::write::kGrowFont, nullptr, false},
    Shortcut{GDK_KEY_period, true, false, ids::write::kGrowFont, nullptr, false},
    Shortcut{GDK_KEY_less, true, false, ids::write::kShrinkFont, nullptr, false},
    Shortcut{GDK_KEY_comma, true, false, ids::write::kShrinkFont, nullptr, false},
    Shortcut{GDK_KEY_l, false, false, "write.paragraph.align-left", nullptr, false},
    Shortcut{GDK_KEY_e, false, false, "write.paragraph.align-center", nullptr, false},
    Shortcut{GDK_KEY_r, false, false, "write.paragraph.align-right", nullptr, false},
    Shortcut{GDK_KEY_j, false, false, "write.paragraph.align-justify", nullptr, false},
    Shortcut{GDK_KEY_7, true, false, "write.paragraph.numbering", nullptr, false},
    Shortcut{GDK_KEY_ampersand, true, false, "write.paragraph.numbering", nullptr, false},
    Shortcut{GDK_KEY_8, true, false, "write.paragraph.bullets", nullptr, false},
    Shortcut{GDK_KEY_asterisk, true, false, "write.paragraph.bullets", nullptr, false},
    Shortcut{GDK_KEY_9, true, false, ids::write::kChecklist, nullptr, false},
    Shortcut{GDK_KEY_parenleft, true, false, ids::write::kChecklist, nullptr, false},
    Shortcut{GDK_KEY_bracketright, false, false, ids::write::kIndentMore, nullptr, false},
    Shortcut{GDK_KEY_bracketleft, false, false, ids::write::kIndentLess, nullptr, false},
    Shortcut{GDK_KEY_backslash, false, false, ids::write::kClearCharacterOverrides, nullptr, false},
    Shortcut{GDK_KEY_space, false, false, ids::write::kClearCharacterOverrides, nullptr, false},
    Shortcut{GDK_KEY_k, true, false, ids::write::kInsertLink, nullptr, false},
    Shortcut{GDK_KEY_Return, false, false, ids::write::kInsertPageBreak, nullptr, false},
    Shortcut{GDK_KEY_m, false, true, ids::write::kAddComment, nullptr, false},
    Shortcut{GDK_KEY_0, false, true, ids::write::kApplyStyle, "Body", false},
    Shortcut{GDK_KEY_1, false, true, ids::write::kApplyStyle, "Heading 1", false},
    Shortcut{GDK_KEY_2, false, true, ids::write::kApplyStyle, "Heading 2", false},
    Shortcut{GDK_KEY_3, false, true, ids::write::kApplyStyle, "Heading 3", false},
    Shortcut{GDK_KEY_c, false, true, ids::write::kFormatPainter, nullptr, false},
    Shortcut{GDK_KEY_g, false, false, ids::write::kFindNext, "", true},
    Shortcut{GDK_KEY_g, true, false, ids::write::kFindPrevious, "", true},
};

}  // namespace

gboolean WriteWindow::Impl::command_key_pressed(GtkEventControllerKey*, const guint keyval,
                                                guint, const GdkModifierType state,
                                                gpointer data) {
  auto* self = static_cast<Impl*>(data);
  if (keyval == GDK_KEY_F11) {
    self->dispatch(ids::write::kFocusMode);
    return TRUE;
  }
  const auto modifiers = static_cast<GdkModifierType>(
      state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
  if ((modifiers & GDK_CONTROL_MASK) == 0 || (modifiers & GDK_SUPER_MASK) != 0) {
    return FALSE;
  }
  const guint key = gdk_keyval_to_lower(keyval);
  const bool shifted = (modifiers & GDK_SHIFT_MASK) != 0;
  const bool alt = (modifiers & GDK_ALT_MASK) != 0;
  if (!alt && ((!shifted && key == GDK_KEY_k) || (shifted && key == GDK_KEY_p))) {
    return palette_shortcut(GTK_WIDGET(self->window), nullptr, self);
  }
  const bool in_text = focus_is_text_entry(self->window);
  for (const auto& shortcut : kShortcuts) {
    if (shortcut.key != key || shortcut.shift != shifted || shortcut.alt != alt) {
      continue;
    }
    if (in_text && !shortcut.global) {
      return FALSE;
    }
    if (shortcut.command == ids::write::kFindNext ||
        shortcut.command == ids::write::kFindPrevious) {
      self->dispatch_argument(shortcut.command,
                              gtk_editable_get_text(GTK_EDITABLE(self->find_entry)));
    } else if (shortcut.argument != nullptr) {
      self->dispatch_argument(shortcut.command, shortcut.argument);
    } else {
      self->dispatch(shortcut.command);
    }
    return TRUE;
  }
  return FALSE;
}

void WriteWindow::Impl::install_shortcuts() {
  GtkEventController* command_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(command_keys, GTK_PHASE_CAPTURE);
  g_signal_connect(command_keys, "key-pressed", G_CALLBACK(command_key_pressed), this);
  gtk_widget_add_controller(GTK_WIDGET(window), command_keys);

  GtkShortcutController* shortcuts = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_shortcut_controller_set_scope(shortcuts, GTK_SHORTCUT_SCOPE_MANAGED);
  gtk_shortcut_controller_add_shortcut(
      shortcuts,
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, static_cast<GdkModifierType>(0)),
                       gtk_callback_action_new(
                           +[](GtkWidget*, GVariant*, gpointer data) -> gboolean {
                             auto* self = static_cast<Impl*>(data);
                             if (gtk_widget_get_mapped(GTK_WIDGET(self->command_palette))) {
                               adw_dialog_close(self->command_palette);
                               return TRUE;
                             }
                             if (gtk_window_is_fullscreen(self->window)) {
                               gtk_window_unfullscreen(self->window);
                               return TRUE;
                             }
                             return FALSE;
                           },
                           this, nullptr)));
  gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(shortcuts));
}

void WriteWindow::Impl::build() {
  luma_init();
  context = luma_context_new_from_environment();
  // Publish the real menu before the native header is constructed. The toolkit
  // resolves application identity while attaching the window to GtkApplication.
  GMenuModel* menu_model = build_menu_model();
  gtk_application_set_menubar(application, menu_model);
  // LumaApplicationWindow constructs its identity tile inside its constructor,
  // before we can set this window's icon. Give it Write's installed icon now.
  gtk_window_set_default_icon_name(luma::identity::icon_name);
  window = GTK_WINDOW(luma_application_window_new(application, context));
  gtk_widget_set_name(GTK_WIDGET(window), "wr-window");
  gtk_widget_insert_action_group(GTK_WIDGET(window), "write", G_ACTION_GROUP(menu_action_group));
  g_object_ref_sink(window);
  luma_application_window_set_identity(LUMA_APPLICATION_WINDOW(window),
                                       identity.display_name.c_str(),
                                       identity.suite_name.c_str(),
                                       "org.projectluma.Write-symbolic");
  luma_application_window_set_menu_model(LUMA_APPLICATION_WINDOW(window), menu_model);
  g_object_unref(menu_model);
  // The preview has its own application ID, but uses Write's installed icon.
  // Its ID has no icon in the host theme.
  gtk_window_set_icon_name(window, luma::identity::icon_name);
  gtk_window_set_default_size(window, 1180, 740);
  gtk_window_set_resizable(window, FALSE);
  g_signal_connect(window, "notify::width", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    if (self->action_center_refresh_timeout != 0) return;
    self->action_center_refresh_timeout = g_timeout_add(50, +[](gpointer inner) -> gboolean {
      auto* current = static_cast<Impl*>(inner);
      current->action_center_refresh_timeout = 0;
      current->rebuild_action_center();
      return G_SOURCE_REMOVE;
    }, self);
  }), this);
  gtk_widget_add_css_class(GTK_WIDGET(window), "luma-write-window");
  if (luma_context_get_presentation(context) == LUMA_PRESENTATION_FULLSCREEN_MOBILE) {
    gtk_widget_add_css_class(GTK_WIDGET(window), "mobile-presentation");
    gtk_window_fullscreen(window);
  }
  if (luma_context_get_touch_targets(context)) {
    gtk_widget_add_css_class(GTK_WIDGET(window), "touch-input");
  }
  g_signal_connect(window, "close-request", G_CALLBACK(window_close_requested), this);

  overlay = GTK_OVERLAY(gtk_overlay_new());
  gtk_widget_set_name(GTK_WIDGET(overlay), "wr-content-overlay");
  gtk_widget_set_overflow(GTK_WIDGET(overlay), GTK_OVERFLOW_HIDDEN);
  page_stack = GTK_STACK(gtk_stack_new());
  gtk_stack_set_hhomogeneous(page_stack, FALSE);
  gtk_stack_set_transition_type(page_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration(page_stack, 160);
  start_page = build_start_page();
  editor_page = build_editor_page();
  gtk_stack_add_named(page_stack, start_page, "start");
  gtk_stack_add_named(page_stack, editor_page, "editor");
  gtk_stack_set_visible_child_name(page_stack, "start");
  gtk_overlay_set_child(overlay, GTK_WIDGET(page_stack));
  GtkWidget* surface = luma_island_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_name(surface, "wr-window-island");
  gtk_box_append(GTK_BOX(surface), GTK_WIDGET(overlay));
  luma_application_window_set_body(LUMA_APPLICATION_WINDOW(window), surface);
  phone_title = luma_title_island_new(LUMA_TITLE_ISLAND_LEAD_BACK, "Documents", "");
  gtk_widget_set_name(phone_title, "wr-documents-title");
  luma_title_island_set_variant(LUMA_TITLE_ISLAND(phone_title), "creative");
  g_signal_connect(phone_title, "lead", G_CALLBACK(+[](LumaTitleIsland*, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    if (self->callbacks.on_documents_requested) self->callbacks.on_documents_requested();
  }), this);
  luma_title_island_float_over(LUMA_TITLE_ISLAND(phone_title), GTK_WIDGET(window));
  gtk_widget_set_visible(phone_title, FALSE);

  if (action_center != nullptr) {
    luma_action_center_attach(reinterpret_cast<LumaActionCenter*>(action_center),
                              canvas_island);
    luma_action_center_hide_bar(LUMA_ACTION_CENTER(action_center));
    g_object_unref(action_center);
  }
  build_command_palette();
  install_shortcuts();
}

void WriteWindow::Impl::recent_clicked(GtkButton*, gpointer data) {
  const auto* binding = static_cast<RecentBinding*>(data);
  if (binding->self->callbacks.on_open_recent) {
    binding->self->callbacks.on_open_recent(binding->path);
  }
}

void WriteWindow::Impl::palette_result_clicked(GtkButton*, gpointer data) {
  const auto* binding = static_cast<CommandBinding*>(data);
  adw_dialog_close(binding->self->command_palette);
  binding->self->dispatch(binding->command_id);
}

void WriteWindow::Impl::palette_search_changed(GtkEditable*, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  const char* text = gtk_editable_get_text(GTK_EDITABLE(self->command_search));
  self->rebuild_palette(text == nullptr ? std::string_view{} : std::string_view{text});
}

void WriteWindow::Impl::rebuild_palette(const std::string_view query) {
  palette_bindings.clear();
  while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(command_results))) {
    gtk_list_box_remove(command_results, child);
  }
  std::vector<PaletteItem> results;
  if (!query.empty() && callbacks.on_search_commands) {
    results = callbacks.on_search_commands(query);
  }
  gtk_widget_set_visible(GTK_WIDGET(command_empty_label), results.empty());
  gtk_label_set_text(command_empty_label,
                     query.empty() ? "Search every available command in plain language."
                                   : "No available command matches that search.");
  gtk_widget_set_visible(palette_footer, !results.empty());
  for (const PaletteItem& result : results) {
    GtkWidget* button = gtk_button_new();
    gtk_widget_set_sensitive(button, result.enabled);
    gtk_widget_add_css_class(button, "palette-result");
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(row),
                   icon(result.checked ? "write-check-symbolic" : "write-more-symbolic", 16));
    GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(labels, TRUE);
    GtkWidget* title = left_label(result.label.c_str(), "palette-result-title");
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(labels), title);
    if (!result.detail.empty()) {
      GtkWidget* detail = left_label(result.detail.c_str(), "palette-result-detail");
      gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
      gtk_label_set_wrap_mode(GTK_LABEL(detail), PANGO_WRAP_WORD_CHAR);
      gtk_box_append(GTK_BOX(labels), detail);
    }
    gtk_box_append(GTK_BOX(row), labels);
    if (!result.shortcut.empty()) {
      GtkWidget* accelerator = gtk_label_new(result.shortcut.c_str());
      gtk_widget_add_css_class(accelerator, "shortcut-hint");
      gtk_box_append(GTK_BOX(row), accelerator);
    }
    gtk_button_set_child(GTK_BUTTON(button), row);
    auto binding = std::make_unique<CommandBinding>();
    binding->self = this;
    binding->command_id = result.command_id;
    g_signal_connect(button, "clicked", G_CALLBACK(palette_result_clicked), binding.get());
    palette_bindings.push_back(std::move(binding));
    gtk_list_box_append(command_results, button);
  }
}

void WriteWindow::Impl::navigator_toggled(GtkToggleButton* button, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  if (self->updating_panel_toggles) {
    return;
  }
  self->navigator_requested = gtk_toggle_button_get_active(button) != FALSE;
  if (!self->compact) self->wide_navigator_requested = self->navigator_requested;
  if (self->navigator_requested && self->compact) {
    self->inspector_requested = false;
  }
  self->apply_panel_visibility();
}

void WriteWindow::Impl::inspector_toggled(GtkToggleButton* button, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  if (self->updating_panel_toggles) {
    return;
  }
  self->inspector_requested = gtk_toggle_button_get_active(button) != FALSE;
  if (!self->compact) self->wide_inspector_requested = self->inspector_requested;
  if (self->inspector_requested && self->compact) {
    self->navigator_requested = false;
  }
  self->apply_panel_visibility();
}

void WriteWindow::Impl::close_navigator(GtkButton*, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  self->navigator_requested = false;
  self->apply_panel_visibility();
}

void WriteWindow::Impl::close_inspector(GtkButton*, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  self->inspector_requested = false;
  self->apply_panel_visibility();
}

void WriteWindow::Impl::apply_panel_visibility() {
  const bool has_outline = snapshot.outline_ready && !snapshot.outline.empty();
  if (!has_outline) navigator_requested = false;
  gtk_widget_set_sensitive(GTK_WIDGET(navigator_toggle), has_outline);
  if (page_footer) gtk_widget_set_visible(page_footer, !compact);
  if (zoom_footer) gtk_widget_set_visible(zoom_footer, !compact);
  if (phone_title != nullptr)
    gtk_widget_set_visible(phone_title, compact &&
        g_strcmp0(gtk_stack_get_visible_child_name(page_stack), "editor") == 0);
  if (window != nullptr) {
    GtkWidget* titlebar = luma_application_window_get_title_bar(
        LUMA_APPLICATION_WINDOW(window));
    if (GtkWidget* controls = child_with_class(titlebar, "lumaui-window-controls"))
      gtk_widget_set_visible(controls, !compact);
  }
  // The document fixture follows the authored fit-to-page lane when the
  // right inspector opens. Keep this on the island so the fixture can lay out
  // its paper without changing the production document engine's page zoom.
  if (canvas_island != nullptr) {
    if (!compact && !focus_mode && inspector_requested)
      gtk_widget_add_css_class(canvas_island, "inspector-open");
    else
      gtk_widget_remove_css_class(canvas_island, "inspector-open");
  }
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  gtk_widget_set_visible(navigator_island, has_outline && (!compact || navigator_requested));
  gtk_widget_set_visible(inspector_island, !compact || inspector_requested);
  luma_floating_panel_set_folded(LUMA_FLOATING_PANEL(navigator_island),
                                 !navigator_requested);
  luma_floating_panel_set_folded(LUMA_FLOATING_PANEL(inspector_island),
                                 !inspector_requested);
  // The outline floats over the workspace, while the document stays centred
  // in the remaining page lane (244px panel plus its 4px gutter).
  const bool room_for_panels = gtk_widget_get_width(creative_workspace) >= 1100;
  gtk_widget_set_margin_start(canvas_layer_host,
                              room_for_panels && !compact && !focus_mode && navigator_requested ? 248 : 0);
  gtk_widget_set_margin_end(canvas_layer_host,
                            room_for_panels && !compact && !focus_mode && inspector_requested ? 276 : 0);
#else
  adw_overlay_split_view_set_show_sidebar(navigator_view, navigator_requested);
  if (compact) {
    gtk_revealer_set_reveal_child(mobile_inspector_drawer, inspector_requested);
  } else {
    adw_overlay_split_view_set_show_sidebar(inspector_view, inspector_requested);
  }
  // The 9px gutter between islands exists only beside a visible island, so
  // hiding a panel lets the document island reach the window's own gutter.
  const bool side_by_side = !compact;
  gtk_widget_set_margin_start(canvas_island, side_by_side && navigator_requested ? 9 : 0);
  gtk_widget_set_margin_end(canvas_island, side_by_side && inspector_requested ? 9 : 0);
#endif
  updating_panel_toggles = true;
  gtk_toggle_button_set_active(navigator_toggle, navigator_requested);
  gtk_toggle_button_set_active(inspector_toggle, inspector_requested);
  updating_panel_toggles = false;
}

void WriteWindow::Impl::open_inspector_tab(const char* name) {
  inspector_requested = true;
  if (!compact) {
    wide_inspector_requested = true;
  } else {
    navigator_requested = false;
  }
  apply_panel_visibility();
  const std::string_view requested{name};
  if (requested == "comments") {
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(inspector_tabs), "comments");
    gtk_stack_set_visible_child_name(inspector_stack, "comments");
  } else if (requested == "advanced") {
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(inspector_tabs), "format");
    gtk_stack_set_visible_child_name(inspector_stack, "advanced");
  } else {
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(inspector_tabs), "format");
    gtk_stack_set_visible_child_name(inspector_stack,
                                     snapshot.picture_selected ? "image" :
                                     snapshot.selection_context == "Table" && !table_context_dismissed ? "table" : "format");
  }
}

void WriteWindow::Impl::panel_visibility_changed(GObject* object, GParamSpec*, gpointer data) {
  auto& self = *static_cast<Impl*>(data);
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  const bool navigation = object == G_OBJECT(self.navigator_island);
  const bool shown = !luma_floating_panel_get_folded(
      LUMA_FLOATING_PANEL(navigation ? self.navigator_island : self.inspector_island));
  if (navigation) self.navigator_requested = shown;
  else self.inspector_requested = shown;
  self.updating_panel_toggles = true;
  gtk_toggle_button_set_active(navigation ? self.navigator_toggle : self.inspector_toggle, shown);
  self.updating_panel_toggles = false;
  self.apply_panel_visibility();
#else
  const bool navigation = object == G_OBJECT(self.navigator_view);
  if (!navigation && self.compact) return;
  auto* view = navigation ? self.navigator_view : self.inspector_view;
  const bool shown = adw_overlay_split_view_get_show_sidebar(view);
  if (navigation) {
    self.navigator_requested = shown;
  } else {
    self.inspector_requested = shown;
  }
  self.updating_panel_toggles = true;
  gtk_toggle_button_set_active(navigation ? self.navigator_toggle : self.inspector_toggle, shown);
  self.updating_panel_toggles = false;
  gtk_widget_set_margin_start(self.canvas_island,
                              !self.compact && self.navigator_requested ? 9 : 0);
  gtk_widget_set_margin_end(self.canvas_island,
                            !self.compact && self.inspector_requested ? 9 : 0);
  if (self.compact && gtk_widget_get_mapped(self.editor_page)) {
    if (shown) {
      gtk_widget_child_focus(adw_overlay_split_view_get_sidebar(view), GTK_DIR_TAB_FORWARD);
    } else {
      gtk_widget_grab_focus(
          GTK_WIDGET(navigation ? self.navigator_toggle : self.inspector_toggle));
    }
  }
#endif
}

void WriteWindow::Impl::window_size_changed(GObject*, GParamSpec*, gpointer data) {
  static_cast<Impl*>(data)->apply_responsive();
}

void WriteWindow::Impl::apply_responsive() {
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  if (creative_workspace == nullptr) return;
  if (gtk_widget_get_width(creative_workspace) <= 0) return;
  const bool next_compact = luma_creative_workspace_get_is_phone(
      LUMA_CREATIVE_WORKSPACE(creative_workspace));
  if (corner_pill_stack != nullptr)
    gtk_stack_set_visible_child_name(GTK_STACK(corner_pill_stack),
                                     next_compact ? "phone" : "desktop");
  // The ActionCenter is an overlay outside CreativeWorkspace's tools slot.
  // Reserve its actual allocation for both phone drawers; otherwise a short
  // Comments page opens underneath the bar and cannot be operated.
  int drawer_bottom = 0;
  graphene_rect_t bar_bounds;
  if (next_compact && action_center != nullptr &&
      gtk_widget_get_mapped(action_center) &&
      gtk_widget_compute_bounds(action_center, creative_workspace, &bar_bounds)) {
    drawer_bottom = std::max(0, gtk_widget_get_height(creative_workspace) -
                                  static_cast<int>(std::floor(bar_bounds.origin.y)));
  }
  gtk_widget_set_margin_bottom(navigator_island, drawer_bottom);
  gtk_widget_set_margin_bottom(inspector_island, drawer_bottom);
  if (next_compact == compact) {
    apply_panel_visibility();
    return;
  }
  compact = next_compact;
  if (compact) {
    gtk_widget_add_css_class(GTK_WIDGET(window), "compact");
    wide_navigator_requested = navigator_requested;
    wide_inspector_requested = inspector_requested;
    // A fixture or user action may have opened the inspector before the
    // workspace's first phone allocation. Keep that requested drawer open.
    const bool pending_inspector = inspector_requested;
    navigator_requested = false;
    inspector_requested = pending_inspector;
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(window), "compact");
    navigator_requested = wide_navigator_requested;
    inspector_requested = wide_inspector_requested;
  }
  apply_panel_visibility();
#else
  if (navigator_view == nullptr) return;
  if (gtk_widget_get_width(editor_page) <= 0) return;
  const bool next_compact = adw_overlay_split_view_get_collapsed(navigator_view);
  // A breakpoint sets the two native views separately. Reconcile only after
  // both have changed, so an intermediate notification cannot erase the
  // desktop panel choices or leave one sidebar closed on return to desktop.
  if (next_compact != (adw_overlay_split_view_get_collapsed(inspector_view) != FALSE)) return;
  if (corner_pill_stack != nullptr)
    gtk_stack_set_visible_child_name(GTK_STACK(corner_pill_stack),
                                     next_compact ? "phone" : "desktop");
  if (next_compact == compact) return;
  compact = next_compact;
  if (compact) {
    gtk_widget_add_css_class(GTK_WIDGET(window), "compact");
    gtk_widget_set_valign(navigator_island, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(navigator_island, 0);
    gtk_widget_set_vexpand(GTK_WIDGET(outline_stack), TRUE);
    navigator_requested = false;
    inspector_requested = false;
    g_object_ref(inspector_island);
    adw_overlay_split_view_set_sidebar(inspector_view, nullptr);
    gtk_widget_set_size_request(inspector_island, -1, 476);
    gtk_revealer_set_child(mobile_inspector_drawer, inspector_island);
    g_object_unref(inspector_island);
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(window), "compact");
    gtk_widget_set_vexpand(GTK_WIDGET(outline_stack), FALSE);
    gtk_widget_set_valign(navigator_island, GTK_ALIGN_START);
    gtk_widget_set_margin_top(navigator_island, 10);
    gtk_revealer_set_reveal_child(mobile_inspector_drawer, FALSE);
    g_object_ref(inspector_island);
    gtk_revealer_set_child(mobile_inspector_drawer, nullptr);
    gtk_widget_set_size_request(inspector_island, -1, -1);
    adw_overlay_split_view_set_sidebar(inspector_view, inspector_island);
    g_object_unref(inspector_island);
    navigator_requested = wide_navigator_requested;
    inspector_requested = wide_inspector_requested;
  }
  apply_panel_visibility();
#endif
}

gboolean WriteWindow::Impl::palette_shortcut(GtkWidget*, GVariant*, gpointer data) {
  auto* self = static_cast<Impl*>(data);
  gtk_editable_set_text(GTK_EDITABLE(self->command_search), "");
  self->rebuild_palette({});
  adw_dialog_present(self->command_palette, GTK_WIDGET(self->window));
  adw_dialog_set_focus(self->command_palette, GTK_WIDGET(self->command_search));
  gtk_widget_grab_focus(GTK_WIDGET(self->command_search));
  return TRUE;
}

void WriteWindow::Impl::update_theme_toggle() {
  // The button shows the page you have and says what a click gives you.
  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_child(theme_button)),
                               snapshot.moon ? "write-moon-symbolic" : "write-sun-symbolic");
  set_accessible_label(GTK_WIDGET(theme_button),
                       snapshot.moon ? "Moon page. Switch to Sun, a white page true to print"
                                     : "Sun page. Switch to Moon, a dark page easy on the eyes");
}

WriteWindow::WriteWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<WriteWindow> WriteWindow::create(GtkApplication* application,
                                                 WriteWindowCallbacks callbacks,
                                                 suite::SuiteIdentity identity) {
  if (application == nullptr) {
    return nullptr;
  }
  install_visual_resources(gdk_display_get_default());
  auto impl = std::make_unique<Impl>();
  impl->application = application;
  impl->identity = std::move(identity);
  impl->callbacks = std::move(callbacks);
  impl->build();
  return std::unique_ptr<WriteWindow>(new WriteWindow(std::move(impl)));
}

WriteWindow::~WriteWindow() {
  if (impl_->action_center != nullptr)
    g_signal_handlers_disconnect_by_data(impl_->action_center, impl_.get());
  if (impl_->action_center_refresh_timeout != 0) {
    g_source_remove(impl_->action_center_refresh_timeout);
    impl_->action_center_refresh_timeout = 0;
  }
  if (impl_->command_palette != nullptr) {
    adw_dialog_force_close(impl_->command_palette);
    g_clear_object(&impl_->command_palette);
  }
  if (impl_->share_menu != nullptr) {
    luma_floating_menu_close(impl_->share_menu);
    g_object_unref(impl_->share_menu);
    impl_->share_menu = nullptr;
  }
  if (impl_->more_menu != nullptr) {
    luma_floating_menu_close(impl_->more_menu);
    g_object_unref(impl_->more_menu);
    impl_->more_menu = nullptr;
  }
  if (impl_->alignment_menu != nullptr) {
    luma_floating_menu_close(impl_->alignment_menu);
    g_object_unref(impl_->alignment_menu);
    impl_->alignment_menu = nullptr;
  }
  if (impl_->window != nullptr) {
    if (gtk_window_get_application(impl_->window) != nullptr) {
      gtk_window_destroy(impl_->window);
    }
    g_object_unref(impl_->window);
    impl_->window = nullptr;
  }
  g_clear_object(&impl_->find_panel);
  g_clear_object(&impl_->find_entry);
  g_clear_object(&impl_->context);
  g_clear_object(&impl_->menu_action_group);
}

GtkWindow* WriteWindow::window() const noexcept { return impl_->window; }

void WriteWindow::present() {
  gtk_window_present(impl_->window);
  if (impl_->action_center_refresh_timeout != 0)
    g_source_remove(impl_->action_center_refresh_timeout);
  impl_->action_center_refresh_timeout = g_timeout_add(50, +[](gpointer data) -> gboolean {
    auto* self = static_cast<Impl*>(data);
    self->action_center_refresh_timeout = 0;
    self->rebuild_action_center();
    return G_SOURCE_REMOVE;
  }, impl_.get());
}

void WriteWindow::show_start_screen() {
  // Shell window memory explicitly excludes non-resizable welcome windows.
  gtk_window_set_resizable(impl_->window, FALSE);
  gtk_window_unmaximize(impl_->window);
  gtk_window_set_default_size(impl_->window, 1180, 740);
  if (impl_->phone_title != nullptr) gtk_widget_set_visible(impl_->phone_title, FALSE);
  gtk_stack_set_visible_child_name(impl_->page_stack, "start");
  impl_->shown_action_center_key.clear();
  if (impl_->action_center != nullptr)
    luma_action_center_hide_bar(LUMA_ACTION_CENTER(impl_->action_center));
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  if (impl_->corner_pill_stack != nullptr)
    gtk_widget_set_visible(impl_->corner_pill_stack, FALSE);
#endif
  luma_application_window_set_identity(
      LUMA_APPLICATION_WINDOW(impl_->window), impl_->identity.display_name.c_str(),
      impl_->identity.suite_name.c_str(), "org.projectluma.Write-symbolic");
}

void WriteWindow::show_editor() {
  gtk_window_set_resizable(impl_->window, TRUE);
  gtk_stack_set_visible_child_name(impl_->page_stack, "editor");
  impl_->rebuild_action_center();
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  if (impl_->corner_pill_stack != nullptr)
    gtk_widget_set_visible(impl_->corner_pill_stack, TRUE);
#endif
  update_document(impl_->snapshot);
  impl_->apply_responsive();
  // A narrow window opens on the document; the panels wait to be asked for.
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  if (impl_->compact) {
#else
  if (impl_->compact || adw_overlay_split_view_get_collapsed(impl_->navigator_view)) {
#endif
    impl_->navigator_requested = false;
    impl_->inspector_requested = false;
  }
  impl_->apply_panel_visibility();
}

void WriteWindow::set_canvas(GtkWidget* canvas) {
  if (canvas == nullptr || canvas == impl_->canvas) {
    return;
  }
  if (gtk_widget_get_parent(canvas) != nullptr) {
    g_warning("Write canvas must be unparented before insertion");
    return;
  }
  if (impl_->canvas != nullptr) {
    gtk_stack_remove(impl_->canvas_stack, impl_->canvas);
  }
  impl_->canvas = canvas;
  gtk_widget_set_hexpand(canvas, TRUE);
  gtk_widget_set_vexpand(canvas, TRUE);
  gtk_stack_add_named(impl_->canvas_stack, canvas, "document");
  gtk_stack_set_visible_child_name(impl_->canvas_stack, "document");
  if (GtkWidget* scroller = child_with_class(canvas, "document-scroller");
      scroller != nullptr && GTK_IS_SCROLLED_WINDOW(scroller))
    luma_action_center_attach_scroller(LUMA_ACTION_CENTER(impl_->action_center),
                                      GTK_SCROLLED_WINDOW(scroller));
}

GtkWidget* WriteWindow::canvas_host() const noexcept {
  return GTK_WIDGET(impl_->canvas_stack);
}

void WriteWindow::set_recent_documents(std::vector<RecentDocument> documents) {
  impl_->recent_documents = std::move(documents);
  std::stable_sort(impl_->recent_documents.begin(), impl_->recent_documents.end(),
                   [](const RecentDocument& left, const RecentDocument& right) {
                     return left.pinned && !right.pinned;
                   });
}

void WriteWindow::update_document(const DocumentSnapshot& snapshot) {
  auto& self = *impl_;
  self.snapshot = snapshot;
  if (snapshot.selection_context != "Table") self.table_context_dismissed = false;
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  if (self.navigator_island != nullptr) {
    const std::string count = std::to_string(snapshot.outline.size()) + " headings";
    luma_floating_panel_set_summary(LUMA_FLOATING_PANEL(self.navigator_island), count.c_str());
  }
  if (self.inspector_island != nullptr) {
    const bool table = snapshot.selection_context == "Table";
    const char* subject = snapshot.picture_selected ? "Photo" :
                          table ? "Table" : "Document";
    luma_floating_panel_set_title(LUMA_FLOATING_PANEL(self.inspector_island), subject);
    luma_floating_panel_set_icon(LUMA_FLOATING_PANEL(self.inspector_island),
                                 snapshot.picture_selected ? "image" : table ? "table" : "file-text");
  }
#endif
  self.rebuild_action_center();
  if (self.corner_pill != nullptr && self.shown_collaborators != snapshot.collaborators) {
    GtkWidget* people = nullptr;
    if (!snapshot.collaborators.empty()) {
      people = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_name(people, "wr-people");
      for (const std::string& person : snapshot.collaborators)
        gtk_box_append(GTK_BOX(people), luma_person_avatar_new(person.c_str(), 26));
    }
    luma_corner_pill_set_people(LUMA_CORNER_PILL(self.corner_pill), people);
    self.shown_collaborators = snapshot.collaborators;
  }

  gtk_label_set_text(self.save_state, snapshot.save_state_label.c_str());
  if (snapshot.modified) {
    gtk_widget_add_css_class(GTK_WIDGET(self.save_button), "unsaved");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self.save_button), "unsaved");
  }

  set_figure(self.word_count_label, snapshot.word_count, "word", "words",
             snapshot.selected_word_count);
  set_figure(self.character_count_label, snapshot.character_count, "character",
             "characters", snapshot.selected_character_count);
  const int minutes = std::max(1, (std::max(snapshot.word_count, 0) + 229) / 230);
  gtk_label_set_text(self.read_time_label,
                     (std::to_string(minutes) + " min read").c_str());
  gtk_widget_set_visible(self.status_count_group, snapshot.word_count_available);
  const std::string page = "Page " + std::to_string(std::max(snapshot.current_page, 1)) +
                           " of " + std::to_string(std::max(snapshot.page_count, 1));
  gtk_label_set_text(self.page_label, page.c_str());
  if (self.page_overlay_label != nullptr)
    gtk_label_set_text(self.page_overlay_label, page.c_str());
  gtk_label_set_text(self.zoom_label, format_zoom(snapshot.zoom_percent).c_str());
  if (self.zoom_overlay_label != nullptr)
    gtk_label_set_text(self.zoom_overlay_label, format_zoom(snapshot.zoom_percent).c_str());

  if (snapshot.compatibility_issue_count > 0) {
    const std::string label =
        "Compatibility · " + std::to_string(snapshot.compatibility_issue_count);
    gtk_button_set_label(self.compatibility_button, label.c_str());
  }
  gtk_widget_set_visible(self.status_compatibility_group,
                         snapshot.compatibility_issue_count > 0);

  gtk_widget_set_sensitive(self.replace_row, !snapshot.read_only);
  gtk_widget_set_sensitive(GTK_WIDGET(self.replace_toggle), !snapshot.read_only);
  if (snapshot.search_ran && *gtk_editable_get_text(GTK_EDITABLE(self.find_entry)) != '\0') {
    if (snapshot.search_matches <= 0) {
      gtk_label_set_text(self.find_count, "No results");
    } else if (snapshot.search_current > 0) {
      const std::string count = std::to_string(snapshot.search_current) + " of " +
                                std::to_string(snapshot.search_matches);
      gtk_label_set_text(self.find_count, count.c_str());
    } else {
      const std::string count = std::to_string(snapshot.search_matches) +
                                (snapshot.search_matches == 1 ? " match" : " matches");
      gtk_label_set_text(self.find_count, count.c_str());
    }
  }

  gtk_label_set_text(self.style_button_value, snapshot.paragraph_style.c_str());
  self.update_theme_toggle();
  if (!snapshot.outline_ready || snapshot.outline.size() != self.shown_outline.size() ||
      snapshot.outline_ready != self.outline_shown_ready ||
      !std::equal(snapshot.outline.begin(), snapshot.outline.end(),
                  self.shown_outline.begin(),
                  [](const OutlineEntry& left, const OutlineEntry& right) {
                    return left.title == right.title && left.level == right.level &&
                           left.active == right.active && left.word_count == right.word_count;
                  })) {
    self.rebuild_outline();
  }
  self.update_inspector();

  std::string title = snapshot.title;
  if (snapshot.modified) {
    title += " •";
  }
  title += " — " + self.identity.display_name;
  luma_application_window_set_identity(LUMA_APPLICATION_WINDOW(self.window),
                                       self.identity.display_name.c_str(),
                                       snapshot.title.c_str(),
                                       "org.projectluma.Write-symbolic");
  // The AppKit identity is concise chrome; the native window title retains
  // document dirty state for task switchers, automation, and assistive tools.
  gtk_window_set_title(self.window, title.c_str());
}

void WriteWindow::update_command_state(const std::string_view command_id,
                                       const commands::CommandState& state) {
  impl_->command_states[std::string{command_id}] = state;
  // The alignment button shows the alignment in effect.
  if (impl_->alignment_button != nullptr && command_id.starts_with("write.paragraph.align-") &&
      state.checked == commands::CheckState::Checked) {
    const std::string name =
        "write-align-" + std::string{command_id.substr(std::string_view{"write.paragraph.align-"}.size())} +
        "-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_child(GTK_BUTTON(impl_->alignment_button))),
                                 name.c_str());
  }
  for (const auto& [registered_id, widget] : impl_->command_widgets) {
    if (registered_id != command_id) {
      continue;
    }
    gtk_widget_set_visible(widget, state.visible);
    gtk_widget_set_sensitive(widget, state.enabled);
    if (g_object_get_data(G_OBJECT(widget), "write-checkable") != nullptr) {
      if (state.checked == commands::CheckState::Checked) gtk_widget_add_css_class(widget, "on");
      else gtk_widget_remove_css_class(widget, "on");
      if (state.checked == commands::CheckState::Mixed) gtk_widget_add_css_class(widget, "mixed");
      else gtk_widget_remove_css_class(widget, "mixed");
      const auto pressed = state.checked == commands::CheckState::Mixed
          ? GTK_ACCESSIBLE_TRISTATE_MIXED : state.checked == commands::CheckState::Checked
          ? GTK_ACCESSIBLE_TRISTATE_TRUE : GTK_ACCESSIBLE_TRISTATE_FALSE;
      gtk_accessible_update_state(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_STATE_PRESSED, pressed, -1);
    }
    if (GTK_IS_TOGGLE_BUTTON(widget)) {
      const bool checked = state.checked == commands::CheckState::Checked;
      if ((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) != FALSE) != checked) {
        // Setting the state must not run the command again.
        g_signal_handlers_block_matched(widget, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr,
                                        reinterpret_cast<gpointer>(Impl::command_clicked),
                                        nullptr);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), checked);
        g_signal_handlers_unblock_matched(widget, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr,
                                          reinterpret_cast<gpointer>(Impl::command_clicked),
                                          nullptr);
      }
      if (state.checked == commands::CheckState::Mixed) {
        gtk_widget_add_css_class(widget, "mixed");
      } else {
        gtk_widget_remove_css_class(widget, "mixed");
      }
    }
  }
  for (const auto& [registered_id, action] : impl_->command_actions) {
    if (registered_id == command_id) {
      g_simple_action_set_enabled(action, state.enabled && state.visible);
    }
  }
  impl_->sync_action_center_states();
}

void WriteWindow::set_font_families(std::vector<std::string> families) {
  impl_->font_families = std::move(families);
  ++impl_->combo_generation;
  impl_->update_inspector();
}

void WriteWindow::set_recent_fonts(std::vector<std::string> families) {
  impl_->recent_fonts = std::move(families);
  ++impl_->combo_generation;
}

void WriteWindow::set_advanced_commands(std::vector<AdvancedCommandGroup> groups) {
  impl_->advanced_groups = std::move(groups);
  impl_->rebuild_advanced({});
}

void WriteWindow::set_advanced_visible(const bool visible) {
  impl_->advanced_visible = visible;
  // Advanced remains under the document's commands; it is not an extra
  // top-level inspector mode in v70.
  if (visible) {
    impl_->open_inspector_tab("advanced");
  } else if (std::string_view{gtk_stack_get_visible_child_name(impl_->inspector_stack)} ==
             "advanced") {
    impl_->open_inspector_tab("format");
  }
}

void WriteWindow::show_find(const bool replace) {
  impl_->open_find(replace);
}

void WriteWindow::show_inspector(const char* mode) {
  impl_->open_inspector_tab(mode);
}

void WriteWindow::set_focus_mode(const bool enabled) {
  auto& self = *impl_;
  if (self.focus_mode == enabled) return;
  self.focus_mode = enabled;
  if (enabled) {
    self.navigator_requested = false;
    self.inspector_requested = false;
    gtk_widget_add_css_class(GTK_WIDGET(self.window), "write-focus-mode");
  } else {
    self.navigator_requested = self.compact ? false : self.wide_navigator_requested;
    self.inspector_requested = self.compact ? false : self.wide_inspector_requested;
    gtk_widget_remove_css_class(GTK_WIDGET(self.window), "write-focus-mode");
  }
  self.apply_panel_visibility();
#ifdef WRITE_HAS_CREATIVE_WORKSPACE
  luma_creative_workspace_set_focused(LUMA_CREATIVE_WORKSPACE(self.creative_workspace),
                                       enabled);
#endif
}

void WriteWindow::show_more_menu() {
  GtkWidget* pill = impl_->compact ? impl_->phone_corner_pill : impl_->corner_pill;
  if (pill == nullptr) return;
  for (GtkWidget* child = gtk_widget_get_first_child(pill);
       child != nullptr; child = gtk_widget_get_next_sibling(child)) {
    for (GtkWidget* button = gtk_widget_get_first_child(child); button != nullptr;
         button = gtk_widget_get_next_sibling(button)) {
      if (GTK_IS_BUTTON(button) && gtk_widget_has_css_class(button, "more")) {
        g_signal_emit_by_name(button, "clicked");
        return;
      }
    }
  }
}

void WriteWindow::show_command_palette() {
  Impl::palette_shortcut(GTK_WIDGET(impl_->window), nullptr, impl_.get());
}

void WriteWindow::show_about() {
  AdwAboutDialog* dialog = ADW_ABOUT_DIALOG(adw_about_dialog_new());
  adw_about_dialog_set_application_name(dialog, impl_->identity.product_name.c_str());
  adw_about_dialog_set_version(dialog, LUMA_VERSION);
  adw_about_dialog_set_application_icon(dialog, impl_->identity.application_id.c_str());
  adw_about_dialog_set_comments(
      dialog, "A native, interoperable word processor for the Project Luma creative suite.");
  const char* authors[] = {"Project Luma contributors", nullptr};
  adw_about_dialog_set_developers(dialog, authors);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(impl_->window));
}

void WriteWindow::show_toast(std::string message, const ToastKind kind,
                             const unsigned timeout_ms) {
  static_cast<void>(timeout_ms);  // The shared toast owns its duration.
  const char* kind_name = "done";
  switch (kind) {
    case ToastKind::Information: kind_name = "done"; break;
    case ToastKind::Success: kind_name = "done"; break;
    case ToastKind::Warning: kind_name = "warning"; break;
    case ToastKind::Error: kind_name = "error"; break;
  }
  luma_toast_show(impl_->canvas_island, message.c_str(), kind_name);
}

void WriteWindow::ask_for_text(std::string heading, std::string body,
                               std::vector<std::pair<std::string, std::string>> fields,
                               std::string action_label,
                               std::function<void(std::vector<std::string>)> on_done) {
  AdwAlertDialog* dialog =
      ADW_ALERT_DIALOG(adw_alert_dialog_new(heading.c_str(), body.empty() ? nullptr : body.c_str()));
  GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto entries = std::make_shared<std::vector<GtkEntry*>>();
  for (const auto& [placeholder, initial] : fields) {
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder.c_str());
    gtk_editable_set_text(GTK_EDITABLE(entry), initial.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   placeholder.c_str(), -1);
    gtk_box_append(GTK_BOX(column), entry);
    entries->push_back(GTK_ENTRY(entry));
  }
  adw_alert_dialog_set_extra_child(dialog, column);
  adw_alert_dialog_add_responses(dialog, "cancel", "Cancel", "done", action_label.c_str(),
                                 nullptr);
  adw_alert_dialog_set_response_appearance(dialog, "done", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(dialog, "done");
  adw_alert_dialog_set_close_response(dialog, "cancel");
  struct Pending {
    std::shared_ptr<std::vector<GtkEntry*>> entries;
    std::function<void(std::vector<std::string>)> on_done;
  };
  auto* pending = new Pending{entries, std::move(on_done)};
  g_signal_connect_data(
      dialog, "response",
      G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
        auto* waiting = static_cast<Pending*>(data);
        if (std::string_view{response} == "done" && waiting->on_done) {
          std::vector<std::string> values;
          for (GtkEntry* entry : *waiting->entries) {
            values.emplace_back(gtk_editable_get_text(GTK_EDITABLE(entry)));
          }
          waiting->on_done(std::move(values));
        }
      }),
      pending, +[](gpointer data, GClosure*) { delete static_cast<Pending*>(data); },
      static_cast<GConnectFlags>(0));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(impl_->window));
  if (!entries->empty()) {
    gtk_widget_grab_focus(GTK_WIDGET(entries->front()));
  }
}

}  // namespace luma::write::ui
