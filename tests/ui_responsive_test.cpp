#include "ui/write_window.hpp"
#include "canvas/document_canvas.hpp"
#include "engine/engine_factory.hpp"
#include "ui/comment_data.hpp"

#include <luma-ui.h>
#include <luma-creative-properties.h>
#if __has_include(<luma-creative-workspace.h>)
#include <luma-creative-workspace.h>
#endif

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void require(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void settle() {
  for (int index = 0; index < 100; ++index) {
    while (g_main_context_iteration(nullptr, FALSE)) {}
    g_usleep(5000);
  }
}

GtkWidget* named(GtkWidget* widget, const char* name) {
  if (std::string{gtk_widget_get_name(widget)} == name) return widget;
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (GtkWidget* found = named(child, name)) return found;
  }
  return nullptr;
}

GtkWidget* with_class(GtkWidget* widget, const char* css_class) {
  if (gtk_widget_has_css_class(widget, css_class)) return widget;
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (GtkWidget* found = with_class(child, css_class)) return found;
  }
  return nullptr;
}

GtkWidget* with_action(GtkWidget* widget, const char* action_name) {
  if (GTK_IS_ACTIONABLE(widget) &&
      g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(widget)), action_name) == 0)
    return widget;
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child)) {
    if (GtkWidget* found = with_action(child, action_name)) return found;
  }
  return nullptr;
}

bool has_label(GtkWidget* widget, const char* text) {
  if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0) return true;
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr; child = gtk_widget_get_next_sibling(child))
    if (has_label(child, text)) return true;
  return false;
}

bool check_icons(GtkWidget* widget, GtkIconTheme* theme) {
  bool all_present = true;
  if (GTK_IS_IMAGE(widget)) {
    const char* icon = gtk_image_get_icon_name(GTK_IMAGE(widget));
    if (icon != nullptr && !gtk_icon_theme_has_icon(theme, icon)) {
      std::cerr << "missing installed icon: " << icon << '\n';
      all_present = false;
    }
  }
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child))
    all_present = check_icons(child, theme) && all_present;
  return all_present;
}

bool has_icon(GtkWidget* widget, const char* name) {
  if (GTK_IS_IMAGE(widget) &&
      g_strcmp0(gtk_image_get_icon_name(GTK_IMAGE(widget)), name) == 0)
    return true;
  for (GtkWidget* child = gtk_widget_get_first_child(widget); child != nullptr;
       child = gtk_widget_get_next_sibling(child))
    if (has_icon(child, name)) return true;
  return false;
}

bool panel_open(GtkWidget* panel) {
#if __has_include(<luma-creative-workspace.h>)
  if (LUMA_IS_FLOATING_PANEL(panel))
    return !luma_floating_panel_get_folded(LUMA_FLOATING_PANEL(panel));
#endif
  return gtk_widget_get_mapped(panel) != FALSE;
}

void capture_panel(GtkWidget* window, GtkWidget* panel, const char* category) {
  const char* directory = g_getenv("LUMA_WRITE_UI_CAPTURE_DIR");
  if (directory == nullptr) return;
  g_mkdir_with_parents(directory, 0700);
  GdkPaintable* paintable = gtk_widget_paintable_new(panel);
  GtkSnapshot* snapshot = gtk_snapshot_new();
  const int width = gtk_widget_get_width(panel);
  const int height = gtk_widget_get_height(panel);
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
  GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
  require(node != nullptr, "panel capture has no render node");
  graphene_rect_t bounds;
  graphene_rect_init(&bounds, 0, 0, static_cast<float>(width), static_cast<float>(height));
  GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(window));
  GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
  const std::string path = std::string{directory} + "/inspector-" + category + ".png";
  require(gdk_texture_save_to_png(texture, path.c_str()), "could not save panel capture");
  g_object_unref(texture);
  gsk_render_node_unref(node);
  g_object_unref(paintable);
}

void check_window(GtkApplication* app, int requested, bool phone) {
  std::vector<std::string> commands;
  std::vector<std::string> arguments;
  int documents_requests = 0;
  luma::write::ui::WriteWindowCallbacks callbacks;
  callbacks.on_documents_requested = [&documents_requests] { ++documents_requests; };
  luma::write::ui::WriteWindow* active_window = nullptr;
  callbacks.on_command = [&commands, &active_window](std::string_view command) {
    commands.emplace_back(command);
    if (command == "suite.edit.find" && active_window != nullptr) active_window->show_find(false);
  };
  callbacks.on_command_argument = [&arguments](std::string_view command,
                                                std::string_view argument) {
    arguments.emplace_back(std::string{command} + ":" + std::string{argument});
  };
  auto write = luma::write::ui::WriteWindow::create(app, std::move(callbacks));
  active_window = write.get();
  GtkWidget* canvas = gtk_drawing_area_new();
  gtk_widget_set_focusable(canvas, TRUE);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(canvas), 1800);
  GtkWidget* document_scroller = gtk_scrolled_window_new();
  gtk_widget_add_css_class(document_scroller, "document-scroller");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(document_scroller), canvas);
  write->set_canvas(document_scroller);
  GtkIconTheme* test_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
  g_object_set(gtk_settings_get_default(), "gtk-icon-theme-name", "Prairie", nullptr);
  gtk_icon_theme_add_search_path(test_theme, "/run/host/usr/share/icons");
  gtk_icon_theme_add_search_path(test_theme, "/usr/share/icons");
  gtk_window_set_default_size(write->window(), requested, 780);
  write->present();
  settle();
  GtkWidget* window = GTK_WIDGET(write->window());
  GtkWidget* bar = named(window, "wr-action-center");
  require(bar && !gtk_widget_get_mapped(bar), "action center is exposed on splash");
  require(std::string{gtk_window_get_icon_name(write->window())} == "org.projectluma.Write",
          "titlebar does not use Write's installed application icon");
  GtkIconTheme* theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(window));
  require(gtk_icon_theme_has_icon(theme, "org.projectluma.Write"),
          "Write's installed application icon is unavailable");
  GtkWidget* title_row = with_class(window, "luma-titlebar");
  require(title_row != nullptr,
          "LumaUI titlebar is missing");
  require(has_icon(title_row, "org.projectluma.Write"),
          "titlebar identity did not select Write's installed icon");
  for (const char* control : {"window-minimize-symbolic", "window-maximize-symbolic",
                              "window-close-symbolic"})
    require(gtk_icon_theme_has_icon(theme, control), "window control icon is unavailable");
  if (!phone) {
    GtkWidget* controls = with_class(title_row, "lumaui-window-controls");
    require(controls && gtk_widget_get_mapped(controls),
            "LumaUI titlebar window controls are not mapped");
  }
  require(check_icons(window, theme), "window contains missing installed icons");
  write->show_editor();
  settle();

  if (std::abs(gtk_widget_get_width(window) - requested) > 16) {
    std::cerr << "initial window width changed: requested " << requested
              << ", got " << gtk_widget_get_width(window) << '\n';
    std::exit(1);
  }
  GtkWidget* outline = named(window, "wr-outline");
  GtkWidget* inspector = named(window, "wr-inspector");
  require(outline && inspector && bar, "LumaUI structure missing");
  require(gtk_widget_get_mapped(bar), "action center is not mapped");
  require(gtk_widget_get_margin_bottom(canvas) >= gtk_widget_get_height(bar) + 20,
          "Document end has no action-bar clearance");
  require(check_icons(window, theme), "editor contains missing installed icons");
  for (const char* glyph : {"bold", "italic", "underline", "strikethrough", "link"}) {
    const std::string name = std::string{"wr-inspector-"} + glyph;
    require(gtk_widget_has_css_class(named(window, name.c_str()), "lumaui-bar-button"),
            "Inspector formatting does not use shared Luma tool controls");
  }
  GtkWidget* panel_bold = named(window, "wr-inspector-bold");
  const auto before_state_change = commands.size();
  write->update_command_state("write.format.bold", {.checked = luma::commands::CheckState::Checked});
  require(gtk_widget_has_css_class(panel_bold, "on"), "Inspector Bold did not show its active state");
  write->update_command_state("write.format.bold", {.checked = luma::commands::CheckState::Mixed});
  require(gtk_widget_has_css_class(panel_bold, "mixed") && !gtk_widget_has_css_class(panel_bold, "on"),
          "Inspector Bold did not distinguish a mixed selection");
  write->update_command_state("write.format.bold", {.checked = luma::commands::CheckState::Unchecked});
  require(!gtk_widget_has_css_class(panel_bold, "on") && !gtk_widget_has_css_class(panel_bold, "mixed") &&
              commands.size() == before_state_change, "Updating format state dispatched an edit");
  g_signal_emit_by_name(panel_bold, "clicked");
  require(commands.back() == "write.format.bold", "Inspector Bold did not format the document");
  if (!phone) {
    GtkWidget* zoom = with_class(window, "write-footer-pill");
    // Both footer pills share appearance; the interactive one is a single button.
    while (zoom != nullptr && !GTK_IS_BUTTON(zoom)) {
      zoom = gtk_widget_get_next_sibling(zoom);
    }
    require(zoom != nullptr && GTK_IS_BUTTON(zoom), "zoom footer has nested button chrome");
    g_signal_emit_by_name(zoom, "clicked");
    settle();
    GtkWidget* menu = with_class(window, "lumaui-menu");
    require(menu != nullptr && LUMA_IS_FLOATING_MENU(menu), "zoom does not use the shared Luma menu");
    require(gtk_widget_activate_action(window, "write.zoom-level", "s", "125"),
            "zoom choice could not be activated");
    require(!arguments.empty() && arguments.back() == "write.view.zoom:125",
            "zoom choice did not dispatch the document command");
    luma_floating_menu_close(LUMA_FLOATING_MENU(menu));
    GtkWidget* bold = with_action(bar, "write.bar-bold");
    require(bold != nullptr, "Bold is missing from the document action bar");
    for (const char* action : {"write.choose-style", "write.choose-font-size"}) {
      GtkWidget* selector = with_action(bar, action);
      require(selector != nullptr && gtk_widget_has_css_class(selector, "lumaui-bar-menu"),
              "Text formatting selector does not use the kit BarMenu well");
      gtk_widget_grab_focus(selector);
      g_signal_emit_by_name(selector, "clicked");
      settle();
      GtkWidget* picker = with_class(window, "lumaui-menu");
      require(picker != nullptr && LUMA_IS_FLOATING_MENU(picker),
              "Formatting selector did not open the shared Luma picker");
      luma_floating_menu_close(LUMA_FLOATING_MENU(picker));
    }
    require(gtk_widget_activate_action(window, "write.font-size", "s", "16"),
            "Font size choice could not be activated");
    require(arguments.back() == "write.format.size:16", "Font size did not dispatch");
    require(gtk_widget_activate_action(window, "write.apply-style", "s", "Heading 1"),
            "Paragraph style choice could not be activated");
    require(arguments.back() == "write.style.apply:Heading 1", "Paragraph style did not dispatch");
    const auto command_count = commands.size();
    write->update_command_state("write.format.bold", {
        .enabled = true, .visible = true,
        .checked = luma::commands::CheckState::Checked});
    require(gtk_widget_has_css_class(bold, "on"), "Bold does not show its active state");
    write->update_command_state("write.format.bold", {
        .enabled = true, .visible = true,
        .checked = luma::commands::CheckState::Unchecked});
    require(!gtk_widget_has_css_class(bold, "on"), "Bold stays active after being switched off");
    require(commands.size() == command_count, "Updating formatting state reran a command");
    g_object_ref(bold);
    luma::write::ui::DocumentSnapshot typed;
    typed.word_count = 5;
    typed.character_count = 24;
    write->update_document(typed);
    settle();
    require(with_action(bar, "write.bar-bold") == bold,
            "typing rebuilt the document action bar");
    g_object_unref(bold);
    GtkWidget* alignment = with_action(bar, "write.show-alignment");
    require(alignment != nullptr, "Alignment is missing from the action bar");
    for (const auto& [selected, label] : std::vector<std::pair<std::string, std::string>>{
             {"left", "Alignment: Left"}, {"center", "Alignment: Centre"},
             {"right", "Alignment: Right"}, {"justify", "Alignment: Justified"}}) {
      for (const char* suffix : {"left", "center", "right", "justify"})
        write->update_command_state(std::string{"write.paragraph.align-"} + suffix,
            {.checked = selected == suffix ? luma::commands::CheckState::Checked : luma::commands::CheckState::Unchecked});
      require(g_strcmp0(gtk_widget_get_tooltip_text(alignment), label.c_str()) == 0,
              "Visible alignment control did not follow the selected paragraph");
      if (selected == "center") {
        require(gtk_widget_activate_action(window, "write.show-alignment", nullptr), "Alignment menu could not open");
        settle();
        GtkWidget* alignment_menu = with_class(window, "lumaui-menu");
        GtkWidget* selected_item = with_class(alignment_menu, "on");
        require(selected_item != nullptr && has_label(selected_item, "Centre"),
                "Alignment menu did not mark the selected paragraph's centre alignment");
        luma_floating_menu_close(LUMA_FLOATING_MENU(alignment_menu));
      }
    }
    write->update_command_state("write.paragraph.align-justify", {.checked = luma::commands::CheckState::Mixed});
    require(gtk_widget_has_css_class(alignment, "mixed") &&
                g_strcmp0(gtk_widget_get_tooltip_text(alignment), "Alignment: Mixed") == 0,
            "Mixed paragraph alignment was shown as one definite alignment");
    luma::write::ui::DocumentSnapshot selected_text;
    selected_text.paragraph_style = "Heading 1";
    selected_text.size_label = "18";
    write->update_document(selected_text);
    require(g_strcmp0(gtk_label_get_text(GTK_LABEL(with_class(with_action(bar, "write.choose-style"), "wr-bar-menu-label"))), "Heading 1") == 0 &&
                g_strcmp0(gtk_label_get_text(GTK_LABEL(with_class(with_action(bar, "write.choose-font-size"), "wr-bar-menu-label"))), "18") == 0,
            "Selected text style and size did not reach the action bar");
    selected_text.paragraph_style = "Mixed";
    selected_text.size_label.clear();
    selected_text.size_mixed = true;
    write->update_document(selected_text);
    require(g_strcmp0(gtk_label_get_text(GTK_LABEL(with_class(with_action(bar, "write.choose-font-size"), "wr-bar-menu-label"))), "Mixed") == 0,
            "Mixed font sizes were replaced with an invented value");
    write->update_document(typed);
  }
  GtkWidget* categories = named(window, "wr-format-tabs");
  GtkWidget* format_stack = named(window, "wr-format-stack");
  require(categories != nullptr && GTK_IS_STACK(format_stack), "Format categories are missing");
  for (const char* category : {"text", "paragraph", "page"}) {
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(categories), category);
    g_signal_emit_by_name(categories, "changed", category);
    settle();
    require(g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(format_stack)), category) == 0,
            "Formatting category did not change its controls");
    if (requested == 1180) {
      luma::write::ui::DocumentSnapshot formatted;
      formatted.typeface_label = "Liberation Serif";
      formatted.size_label = "12";
      formatted.line_height_label = "1.15";
      formatted.text_colour = "-1";
      formatted.highlight_colour = "-1";
      formatted.header_state_known = formatted.footer_state_known = true;
      write->update_document(formatted);
      settle();
      capture_panel(window, inspector, category);
    }
  }
  luma_mode_switch_set_current(LUMA_MODE_SWITCH(categories), "text");
  g_signal_emit_by_name(categories, "changed", "text");
  if (requested == 1180) {
    GtkWidget* size = named(window, "wr-inspector-size");
    GtkWidget* editor = gtk_widget_get_first_child(size);
    while (editor != nullptr && !GTK_IS_TEXT(editor)) editor = gtk_widget_get_next_sibling(editor);
    require(editor != nullptr && gtk_widget_grab_focus(editor), "Font size editor could not receive focus");
    gtk_editable_set_text(GTK_EDITABLE(editor), "18.");
    luma::write::ui::DocumentSnapshot refresh;
    refresh.size_label = "12";
    write->update_document(refresh);
    require(g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(editor)), "18.") == 0,
            "Background refresh overwrote a partially typed font size");
    gtk_window_set_focus(GTK_WINDOW(window), nullptr);
    refresh.read_only = true;
    write->update_document(refresh);
    require(!gtk_widget_is_sensitive(size), "Read-only document exposes editable formatting fields");
    refresh.read_only = false;
    write->update_document(refresh);
    refresh.text_colour = "-1";
    write->update_document(refresh);
    GtkWidget* colour = with_class(window, "lumaui-creative-hex");
    require(colour != nullptr && gtk_widget_grab_focus(colour), "Text colour editor could not receive focus");
    require(gtk_editable_get_editable(GTK_EDITABLE(colour)), "Automatic colour prevents typing a custom colour");
    gtk_editable_set_text(GTK_EDITABLE(colour), "#abc");
    g_signal_emit_by_name(colour, "activate");
    require(arguments.back() == "write.format.text-color:#AABBCC", "Custom colour did not dispatch its RGB value");
    gtk_window_set_focus(GTK_WINDOW(window), nullptr);
  }
  GtkWidget* line = named(window, "wr-inspector-line");
  g_signal_emit_by_name(line, "value-changed", 1.15);
  require(arguments.back() == "write.paragraph.line-spacing:1.15",
          "Line spacing field did not dispatch its custom value");
  g_signal_emit_by_name(named(window, "wr-paragraph-spacing"), "clicked");
  require(arguments.back() == "write.paragraph.spacing:",
          "Exact paragraph spacing did not request its editor");
  gtk_expander_set_expanded(GTK_EXPANDER(named(window, "wr-style-options")), TRUE);
  g_signal_emit_by_name(named(window, "wr-new-style"), "clicked");
  gtk_expander_set_expanded(GTK_EXPANDER(named(window, "wr-style-options")), FALSE);
  require(arguments.back() == "write.style.new-from-selection:", "New style did not request its name editor");
  GtkWidget* header = named(window, "wr-header-state");
  require(header != nullptr, "Header state selector is missing");
  luma::write::ui::DocumentSnapshot imported_settings;
  write->update_document(imported_settings);
  require(luma_property_choice_get_current(LUMA_PROPERTY_CHOICE(header)) == nullptr,
          "Unreported imported header state was presented as Off");
  g_signal_emit_by_name(header, "changed", "off");
  require(arguments.back() == "write.page.header-state:off", "Explicit header removal did not dispatch");
  GtkWidget* paper = named(window, "wr-page-paper");
  luma::write::ui::DocumentSnapshot custom_page;
  custom_page.page_size_label = "Custom";
  write->update_document(custom_page);
  require(g_strcmp0(luma_property_choice_get_current(LUMA_PROPERTY_CHOICE(paper)), nullptr) == 0,
          "Custom paper incorrectly selected a standard preset");
  g_signal_emit_by_name(paper, "changed", "A4");
  require(arguments.back() == "write.page.size:A4", "Paper selector did not dispatch its choice");
  g_signal_emit_by_name(named(window, "wr-page-margins"), "changed", "Custom");
  require(arguments.back() == "write.page.margins:Custom", "Custom margins did not request its editor");
  GtkWidget* documents_title = named(window, "wr-documents-title");
  require(documents_title != nullptr && bool(gtk_widget_get_mapped(documents_title)) == phone,
          "Documents title visibility is wrong");
  if (phone) {
    g_signal_emit_by_name(documents_title, "lead");
    require(documents_requests == 1, "Documents Back did not dispatch navigation");
  }
  require(!gtk_widget_get_mapped(outline) && !panel_open(outline),
          "An empty document exposed a useless outline");
  require(named(window, "wr-outline-find") == nullptr, "Find is still embedded in Outline");
  require(with_action(bar, "write.find") == nullptr, "Find is still a permanent formatting tool");
  require(panel_open(inspector) != phone, "Format must start open on desktop and closed on phone");
  luma::write::ui::DocumentSnapshot headings;
  headings.outline_ready = true;
  headings.outline = {{"Introduction", 1, true, 10}, {"Details", 2, false, 5}};
  write->update_document(headings);
  settle();
  require(bool(gtk_widget_get_mapped(outline)) != phone, "Useful outline did not follow desktop/phone policy");
  if (!phone) {
    GtkWidget* outline_toggle = named(window, "wr-outline-toggle");
    require(outline_toggle != nullptr, "LumaUI outline toggle is missing");
    g_signal_emit_by_name(outline_toggle, "clicked");
    headings.outline.front().active = false;
    headings.outline.back().active = true;
    write->update_document(headings);
    settle();
    require(!panel_open(outline), "Caret update reopened a manually folded outline");
  }
  write->update_document(custom_page);
  settle();
  require(!gtk_widget_get_mapped(outline), "Removing all headings left the outline visible");
  const bool inspector_was_open = panel_open(inspector);
  GListModel* controllers = gtk_widget_observe_controllers(window);
  gboolean handled = FALSE;
  for (guint index = 0; index < g_list_model_get_n_items(controllers) && !handled; ++index) {
    GObject* controller = G_OBJECT(g_list_model_get_item(controllers, index));
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller))
      g_signal_emit_by_name(controller, "key-pressed", GDK_KEY_f, 0U, GDK_CONTROL_MASK, &handled);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  require(handled && commands.back() == "suite.edit.find", "Ctrl+F did not open Find");
  settle();
  require(g_strcmp0(luma_action_center_get_grown(LUMA_ACTION_CENTER(bar)), "find") == 0,
          "Find did not take over the action bar");
  require(!gtk_widget_get_mapped(outline) && panel_open(inspector) == inspector_was_open,
          "Find changed the side panels");
  GtkWidget* query = named(window, "wr-find-entry");
  require(query != nullptr && gtk_widget_get_mapped(query), "Dedicated Find field is missing");
  gtk_editable_set_text(GTK_EDITABLE(query), "Introduction");
  g_signal_emit_by_name(query, "search-changed");
  require(arguments.back() == "write.find.next:Introduction", "Find did not dispatch its query");
  g_signal_emit_by_name(query, "previous-match");
  require(arguments.back() == "write.find.previous:Introduction", "Find previous lost the query");
  headings.search_ran = true;
  headings.search_matches = 2;
  headings.search_current = 1;
  headings.size_label = "14";
  write->update_document(headings);
  settle();
  require(g_strcmp0(luma_action_center_get_grown(LUMA_ACTION_CENTER(bar)), "find") == 0,
          "Engine updates interrupted Find");
  capture_panel(window, bar, phone ? "find-phone" : "find-desktop");
  g_signal_emit_by_name(query, "stop-search");
  settle();
  require(luma_action_center_get_grown(LUMA_ACTION_CENTER(bar)) == nullptr,
          "Escape did not return the formatting action bar");
  write->show_find(true);
  settle();
  require(gtk_widget_get_mapped(named(window, "wr-find-replace")), "Dedicated Replace controls did not open");
  luma_action_center_fold(LUMA_ACTION_CENTER(bar));
  write->update_document(custom_page);
  settle();

  write->show_inspector("comments");
  settle();
  require(named(window, "wr-comments-empty") != nullptr, "Empty comments panel gives no guidance");
  require(panel_open(inspector), "Comments inspector did not open");
  require(phone == (gtk_widget_has_css_class(window, "compact") != FALSE),
          "compact presentation is wrong");
  if (phone) {
    if (g_strcmp0(g_getenv("LUMA_FORM_FACTOR"), "phone") == 0)
      require(gtk_widget_has_css_class(window, "lumaui-phone-device"),
              "physical phone identity was not applied");
    graphene_rect_t drawer_bounds;
    require(gtk_widget_compute_bounds(inspector, window, &drawer_bounds),
            "phone inspector has no window bounds");
    require(drawer_bounds.origin.y > static_cast<float>(gtk_widget_get_height(window)) / 4.0F &&
                drawer_bounds.origin.x >= 0 &&
                drawer_bounds.origin.x <= 20 &&
                drawer_bounds.size.width >= static_cast<float>(gtk_widget_get_width(window) - 40) &&
                drawer_bounds.origin.x + drawer_bounds.size.width <= static_cast<float>(gtk_widget_get_width(window)),
            "phone inspector is not a bottom drawer");
    graphene_rect_t bar_bounds;
    require(gtk_widget_compute_bounds(bar, window, &bar_bounds), "bar has no bounds");
    require(drawer_bounds.origin.y + drawer_bounds.size.height <= bar_bounds.origin.y + 1,
            "phone inspector is covered by the action bar");
    luma::write::ui::DocumentSnapshot discussion;
    for (int index = 0; index < 12; ++index) {
      luma::write::ui::CommentCard comment;
      comment.id = std::to_string(index);
      comment.author = "Private fixture";
      comment.text = "A longer comment checks that the last reply and resolve controls remain reachable.";
      discussion.comments.push_back(comment);
    }
    write->update_document(discussion);
    settle();
    GtkWidget* comments = named(window, "wr-comments");
    GtkWidget* scroll = gtk_widget_get_parent(comments);
    while (scroll != nullptr && !GTK_IS_SCROLLED_WINDOW(scroll))
      scroll = gtk_widget_get_parent(scroll);
    require(scroll != nullptr, "Comments has no scrolling viewport");
    GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    require(gtk_adjustment_get_upper(adjustment) > gtk_adjustment_get_page_size(adjustment),
            "long Comments fixture does not scroll");
    gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) -
                                         gtk_adjustment_get_page_size(adjustment));
    settle();
    graphene_rect_t last_bounds;
    require(gtk_widget_compute_bounds(gtk_widget_get_last_child(comments), window, &last_bounds),
            "last comment has no bounds");
    require(last_bounds.origin.y >= 44 &&
                last_bounds.origin.y + last_bounds.size.height <= bar_bounds.origin.y + 1,
            "last comment actions cannot be reached above the bar");
  }
  require(gtk_widget_activate_action(window, "write.bar-bold", nullptr),
          "bar action could not be activated");
  require(!commands.empty() && commands.back() == "write.format.bold",
          "bar action changed the command ID");
  require(gtk_widget_activate_action(window, "write.bar-table-row", nullptr),
          "table row action could not be activated");
  require(!arguments.empty() && arguments.back() ==
              "write.advanced.run:.uno:InsertRowsAfter",
          "table row action changed the document command");
  if (phone) {
    for(const char* command:{"write.show-format","write.bar-insert","write.show-comments","write.bar-share","write.bar-more"})
      require(with_action(bar,command)!=nullptr,"v71 phone action is missing");
    require(gtk_widget_activate_action(window,"write.bar-insert",nullptr),"Insert could not be activated");
    settle();
    require(g_strcmp0(luma_action_center_get_grown(LUMA_ACTION_CENTER(bar)),"insert")==0,"Insert did not grow the phone bar");
    luma_action_center_fold(LUMA_ACTION_CENTER(bar));
    require(!panel_open(outline), "phone has two drawers open");

  } else {
    luma::write::ui::DocumentSnapshot photo;
    photo.picture_selected = true;
    photo.picture_width_twips = 9360;
    photo.picture_height_twips = 4680;
    photo.picture_flow = "wrap";
    photo.picture_anchor = "with-text";
    write->update_document(photo);
    write->show_inspector("format");
    settle();
    require(check_icons(window, theme), "photo context contains missing installed icons");
    require(gtk_widget_get_mapped(named(window, "wr-photo-format")),
            "Format did not follow the selected photo");
    require(gtk_widget_get_mapped(named(window, "wr-photo-preview-image")) &&
                gtk_widget_get_mapped(named(window, "wr-photo-caption-switch")) &&
                gtk_widget_get_mapped(named(window, "wr-photo-alt-text")),
            "Photo Format omitted preview, caption or alt text");
    require(gtk_widget_get_mapped(named(window, "wr-photo-layout")) &&
                gtk_widget_get_mapped(named(window, "wr-photo-flow")) &&
                gtk_widget_get_mapped(named(window, "wr-photo-anchor")),
            "Image dimensions, wrapping or placement controls remain hidden");
    require(g_strcmp0(luma_property_choice_get_current(LUMA_PROPERTY_CHOICE(named(window, "wr-photo-flow"))), "wrap") == 0,
            "Image wrapping control did not follow the selected picture");
    GtkWidget* modes = named(window, "wr-inspector-tabs");
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(modes), "comments");
    luma_mode_switch_set_current(LUMA_MODE_SWITCH(modes), "format");
    settle();
    require(gtk_widget_get_mapped(named(window, "wr-photo-format")),
            "switching back to Format lost the selected photo");
    require(gtk_widget_activate_action(window, "write.photo-size-m", nullptr),
            "photo size action could not be activated");
    require(!arguments.empty() && arguments.back() == "write.picture.size:5429,2715",
            "photo size action lost the selected picture's proportions");
    require(gtk_widget_activate_action(window, "write.photo-caption", nullptr) &&
                arguments.back() == "write.picture.action:caption",
            "photo caption did not dispatch the document action");
    require(gtk_widget_activate_action(window, "write.photo-delete", nullptr) &&
                arguments.back() == "write.picture.action:delete",
            "photo delete did not dispatch the selected picture action");
    require(gtk_widget_activate_action(window, "write.context-done", nullptr) &&
                arguments.back() == "write.picture.action:done",
            "Done did not leave the selected object");
    luma::write::ui::DocumentSnapshot table;
    table.selection_context = "Table";
    table.table_rows = 4;
    table.table_columns = 3;
    write->update_document(table);
    write->show_inspector("format");
    settle();
    require(check_icons(window, theme), "table context contains missing installed icons");
    require(gtk_widget_get_mapped(named(window, "wr-table-format")),
            "Format did not follow the selected table");
    require(gtk_widget_activate_action(window, "write.context-done", nullptr),
            "Table cell text formatting could not be opened");
    write->update_document(table);
    settle();
    require(gtk_widget_get_mapped(named(window, "wr-format-tabs")),
            "Table context prevents formatting its cell text");
    require(gtk_widget_activate_action(window, "write.table-tools", nullptr),
            "Cell formatting could not return to table tools");
    settle();
    require(gtk_widget_get_mapped(named(window, "wr-table-format")),
            "Table tools did not return after formatting text");
    require(gtk_widget_activate_action(window, "write.show-alignment", nullptr),
            "Alignment menu action could not be activated");
  }
  write->show_start_screen();
  settle();
  require(!gtk_widget_get_mapped(bar), "action center stayed visible on splash");
  require(!gtk_widget_get_mapped(named(window, "wr-documents-title")), "Documents title stayed visible on library");
  write->show_editor();
  settle();
  require(gtk_widget_get_mapped(bar), "action center did not return in editor");
  std::cout << requested << ": action center, outline and details pass\n";
}

}  // namespace

int main() {
  const auto imported = luma::write::ui::comment_cards_from_json(R"({"comments":[
    {"id":42,"author":"Élodie Martin","dateTime":"2025-01-02T10:30:00Z","resolved":"false",
     "html":"<html><head><style>p{color:red}</style></head><body><p>Review &amp; café &#x2014; thanks</p><p>Second line</p></body></html>"},
    {"id":"43","resolved":true,"text":"Resolved"}]})");
  require(imported.size() == 1 && imported.front().id == "42", "Imported comment IDs or resolution states were misread");
  require(imported.front().text == "Review & café — thanks\nSecond line", "Imported HTML comments were not decoded as text");
  require(imported.front().initials == "ÉM" && imported.front().time_label == "2025-01-02",
          "Comment author or timestamp layout was not normalized");
  if (!gtk_init_check()) return 77;
  if (g_getenv("LUMA_WRITE_UI_DARK") != nullptr) {
    GSettings* interface = g_settings_new("org.gnome.desktop.interface");
    g_settings_set_string(interface, "color-scheme", "prefer-dark");
    g_object_unref(interface);
  }
  luma_init();
  {
    auto engine = luma::write::engine::create_document_engine();
    const char* fixture_profile = g_getenv("LUMA_WRITE_UI_ENGINE_PROFILE");
    if (fixture_profile != nullptr) {
      require(engine->initialize({.private_profile_directory = std::filesystem::path{fixture_profile}}).ok,
              "Native canvas fixture could not initialize Writer");
      require(engine->create_blank().ok, "Native canvas fixture could not create a page");
      require(engine->send_composition({.phase = luma::write::engine::CompositionPhase::commit,
                                       .utf8 = "Centred selection"}).ok, "Native selection fixture could not insert text");
      require(engine->execute({.command = luma::write::engine::EngineCommand::align_center}).ok,
              "Native selection fixture could not centre text");
      require(engine->execute({.command = luma::write::engine::EngineCommand::bold}).ok,
              "Native selection fixture could not make text bold");
      require(engine->execute({.command = luma::write::engine::EngineCommand::select_all}).ok,
              "Native selection fixture could not select text");
      settle();
      require(engine->command_state(luma::write::engine::EngineCommand::align_center).kind == luma::write::engine::CommandStateKind::on &&
                  engine->command_state(luma::write::engine::EngineCommand::bold).kind == luma::write::engine::CommandStateKind::on,
              "Writer did not report the selected text's alignment and bold state");
    }
    auto canvas = std::make_unique<luma::write::canvas::DocumentCanvas>(*engine);
    GtkWidget* widget = canvas->widget();
    GtkWidget* fit_window = gtk_window_new();
    g_object_ref_sink(fit_window);
    gtk_window_set_default_size(GTK_WINDOW(fit_window), 1180, 780);
    gtk_window_set_child(GTK_WINDOW(fit_window), widget);
    gtk_window_present(GTK_WINDOW(fit_window));
    settle();
    canvas->update_formatting_controls("Heading 1", "16");
    GtkPolicyType horizontal_policy, vertical_policy;
    gtk_scrolled_window_get_policy(GTK_SCROLLED_WINDOW(widget), &horizontal_policy, &vertical_policy);
    require(horizontal_policy == GTK_POLICY_EXTERNAL && vertical_policy == GTK_POLICY_EXTERNAL,
            "Canvas still displays scrollbars");
    GtkAdjustment* vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(widget));
    require(gtk_adjustment_get_upper(vertical) > gtk_adjustment_get_page_size(vertical),
            "Canvas lost its vertical scrolling range");
    gtk_adjustment_set_value(vertical, 40);
    require(gtk_adjustment_get_value(vertical) == 40, "Hidden canvas scrollbar disabled scrolling");
    canvas->set_zoom_mode(luma::write::canvas::ZoomMode::fit_page);
    const double full_height_zoom = canvas->zoom();
    GtkWidget* viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(widget));
    require(GTK_IS_VIEWPORT(viewport), "Native canvas has no safe-area viewport");
    gtk_widget_set_margin_bottom(gtk_viewport_get_child(GTK_VIEWPORT(viewport)), 96);
    settle();
    if (fixture_profile != nullptr)
      require(canvas->zoom() < full_height_zoom, "Fit page ignored action-bar clearance");
    canvas->prepare_for_window_destruction();
    gtk_window_destroy(GTK_WINDOW(fit_window));
    g_object_unref(fit_window);
    canvas.reset();
  }
  if (g_getenv("LUMA_WRITE_UI_ENGINE_PROFILE") != nullptr) {
    std::cout << "Native Fit page clearance passes\n";
    return 0;
  }
  auto* app = gtk_application_new("org.projectluma.Write.ResponsiveTest",
                                  G_APPLICATION_NON_UNIQUE);
  require(g_application_register(G_APPLICATION(app), nullptr, nullptr), "registration failed");
  const char* requested_width = g_getenv("LUMA_WRITE_UI_WIDTH");
  for (int width : {360, 402, 720, 1180}) {
    if (requested_width != nullptr && std::atoi(requested_width) != width) continue;
    if (width >= 720 && g_strcmp0(g_getenv("LUMA_FORM_FACTOR"), "phone") == 0) continue;
    check_window(app, width, width < 720);
  }
  g_object_unref(app);
}
