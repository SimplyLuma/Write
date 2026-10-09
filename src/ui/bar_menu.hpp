#pragma once

#include <luma-ui-kit.h>
#include <initializer_list>
#include <string>

namespace luma::write::ui {

// Match the kit's BarMenu composition: retained text and a trailing chevron
// inside a well. Place/view dropdowns have different typography and geometry.
inline void configure_bar_menu(GtkWidget* button, const char* text, const char* name) {
  for (const char* css_class : {"lumaui-bar-button", "dropdown", "text", "icon-only"})
    gtk_widget_remove_css_class(button, css_class);
  gtk_widget_add_css_class(button, "lumaui-bar-menu");
  if (g_strcmp0(name, "Font size") == 0)
    gtk_widget_add_css_class(button, "wr-font-size-selector");
  auto* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  auto* label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_add_css_class(label, "wr-bar-menu-label");
  gtk_box_append(GTK_BOX(line), label);
  auto* chevron = luma_ui_icon_new("chevron-down");
  gtk_widget_add_css_class(chevron, "lumaui-bar-menu-chevron");
  gtk_box_append(GTK_BOX(line), chevron);
  gtk_button_set_child(GTK_BUTTON(button), line);
  const auto description = std::string{name} + ": " + text;
  gtk_widget_set_tooltip_text(button, name);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button),
      GTK_ACCESSIBLE_PROPERTY_HAS_POPUP, TRUE,
      GTK_ACCESSIBLE_PROPERTY_LABEL, description.c_str(), -1);
}

inline GtkWidget* bar_menu_new(const char* text, const char* action, const char* name) {
  auto* button = gtk_button_new();
  configure_bar_menu(button, text, name);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
  return button;
}

}  // namespace luma::write::ui
