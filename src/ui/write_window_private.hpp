#pragma once

// The window's private parts, shared by the translation units that build its
// surfaces (write_window.cpp: frame, toolbar, status bar, find bar, palette;
// write_panels.cpp: outline, inspector, popovers). Nothing outside src/ui
// includes this.

#include "ui/write_window.hpp"

#include <luma-ui.h>
#if __has_include(<luma-creative-workspace.h>)
#include <luma-creative-workspace.h>
#define WRITE_HAS_CREATIVE_WORKSPACE 1
#endif

#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace luma::write::ui {
namespace detail {

void add_css_classes(GtkWidget* widget, std::initializer_list<const char*> classes);
void set_accessible_label(GtkWidget* widget, const char* label);
GtkWidget* icon(const char* name, int size = 16);
GtkWidget* left_label(const char* text, const char* css_class = nullptr);
GtkWidget* rule();
GtkWidget* connected_group(const char* extra_class = nullptr);


}  // namespace detail

enum class ComboKind { font, size, style };

// An editable value with a list under it (write_combo.cpp).
struct ComboField {
  WriteWindow::Impl* owner{nullptr};
  ComboKind kind{ComboKind::font};
  GtkEntry* entry{nullptr};
  GtkWidget* popover{nullptr};
  GtkWidget* scroll{nullptr};
  GtkListBox* list{nullptr};
  GtkLabel* create_label{nullptr};
  std::string filter;
  std::string filled_current;
  unsigned filled_generation{~0U};
  guint close_source{0};
  bool editing{false};
  bool setting_text{false};
  bool offer_create{false};
};

struct WeightOption {
  int css{400};
  const char* name{"Regular"};
  // The face that carries this weight: the family itself for Regular and
  // Bold, or the family's named face ("Noto Sans Medium").
  std::string face;
};

struct WriteWindow::Impl {
  struct CommandBinding {
    Impl* self{nullptr};
    std::string command_id;
    std::string argument;
  };

  struct RecentBinding {
    Impl* self{nullptr};
    std::string path;
  };

  GtkApplication* application{nullptr};
  LumaContext* context{nullptr};
  GSimpleActionGroup* menu_action_group{nullptr};
  suite::SuiteIdentity identity;
  WriteWindowCallbacks callbacks;
  DocumentSnapshot snapshot;
  std::vector<RecentDocument> recent_documents;
  std::vector<std::string> font_families;
  std::vector<std::string> recent_fonts;
  std::vector<std::string> document_fonts;
  std::vector<std::string> shown_paragraph_styles;
  std::vector<std::unique_ptr<ComboField>> combos;
  unsigned combo_generation{0};
  std::vector<AdvancedCommandGroup> advanced_groups;

  GtkWindow* window{nullptr};
  GtkOverlay* overlay{nullptr};
  GtkStack* page_stack{nullptr};
  GtkWidget* start_page{nullptr};
  GtkWidget* editor_page{nullptr};

  // Toolbar.
  GtkWidget* command_bar{nullptr};
  GtkWidget* action_center{nullptr};
  std::string shown_action_center_key;
  bool table_context_dismissed{false};
  GtkWidget* phone_title{nullptr};
  GtkWidget* corner_pill{nullptr};
  GtkWidget* phone_corner_pill{nullptr};
  GtkWidget* corner_pill_stack{nullptr};
  LumaFloatingMenu* share_menu{nullptr};
  LumaFloatingMenu* more_menu{nullptr};
  LumaFloatingMenu* alignment_menu{nullptr};
  std::vector<std::string> shown_collaborators;
  GtkLabel* style_button_value{nullptr};
  GtkWidget* style_menu_list{nullptr};
  GtkWidget* style_popover{nullptr};
  GtkToggleButton* navigator_toggle{nullptr};
  GtkToggleButton* inspector_toggle{nullptr};
  GtkToggleButton* find_toggle{nullptr};
  GtkWidget* focus_button{nullptr};
  GtkWidget* alignment_button{nullptr};

  // Panels.
  AdwOverlaySplitView* navigator_view{nullptr};
  AdwOverlaySplitView* inspector_view{nullptr};
  GtkWidget* creative_workspace{nullptr};
  GtkWidget* canvas_layer_host{nullptr};
  GtkWidget* navigator_back{nullptr};
  GtkWidget* inspector_back{nullptr};
  GtkWidget* navigator_panel{nullptr};
  GtkWidget* navigator_island{nullptr};
  GtkWidget* inspector_panel{nullptr};
  GtkWidget* inspector_island{nullptr};
  GtkRevealer* mobile_inspector_drawer{nullptr};
  GtkWidget* canvas_island{nullptr};
  GtkStack* canvas_stack{nullptr};
  GtkWidget* canvas_unavailable{nullptr};
  GtkWidget* canvas{nullptr};

  // Outline.
  GtkWidget* outline_list{nullptr};
  GtkStack* outline_stack{nullptr};
  std::vector<OutlineEntry> shown_outline;
  bool outline_shown_ready{false};

  // Inspector.
  GtkStack* inspector_stack{nullptr};
  GtkWidget* inspector_tabs{nullptr};
  GtkStack* format_stack{nullptr};
  GtkWidget* format_tabs{nullptr};
  GtkWidget* inspector_text_colour{nullptr};
  GtkWidget* inspector_highlight_colour{nullptr};
  GtkWidget* page_size_choice{nullptr};
  GtkWidget* margins_choice{nullptr};
  GtkWidget* orientation_choice{nullptr};
  GtkWidget* comments_list{nullptr};
  std::string shown_comments_key;
  GtkWidget* advanced_tab{nullptr};
  GtkWidget* image_tab{nullptr};
  GtkPicture* picture_preview{nullptr};
  std::string shown_picture_preview_path;
  GtkWidget* picture_size_modes{nullptr};
  GtkWidget* picture_flow_choice{nullptr};
  GtkWidget* picture_anchor_choice{nullptr};
  GtkSwitch* picture_caption_switch{nullptr};
  GtkEntry* picture_alt_text{nullptr};
  GtkWidget* table_tab{nullptr};
  GtkWidget* table_tools_button{nullptr};
  GtkWidget* table_size_group{nullptr};
  GtkLabel* table_rows_value{nullptr};
  GtkLabel* table_columns_value{nullptr};
  GtkEntry* picture_width{nullptr};
  GtkEntry* picture_height{nullptr};
  GtkToggleButton* picture_lock{nullptr};
  bool picture_was_selected{false};
  GtkDropDown* inspector_style{nullptr};
  GtkStringList* inspector_style_model{nullptr};
  GtkDropDown* inspector_weight{nullptr};
  GtkStringList* inspector_weight_model{nullptr};
  std::string weight_family;
  std::vector<WeightOption> weight_options;
  GtkLabel* inspector_line_height_value{nullptr};
  GtkWidget* inspector_size_number{nullptr};
  GtkWidget* inspector_line_height_number{nullptr};
  GtkWidget* line_spacing_segment{nullptr};
  GtkDropDown* page_size_dropdown{nullptr};
  GtkWidget* orientation_segment{nullptr};
  GtkDropDown* margins_dropdown{nullptr};
  GtkSwitch* header_switch{nullptr};
  GtkSwitch* footer_switch{nullptr};
  GtkWidget* header_choice{nullptr};
  GtkWidget* footer_choice{nullptr};
  GtkLabel* page_count_value{nullptr};
  GtkLabel* page_words_value{nullptr};
  GtkLabel* page_characters_value{nullptr};
  GtkWidget* advanced_list{nullptr};
  GtkSearchEntry* advanced_search{nullptr};
  bool updating_inspector{false};

  // Canvas island: find bar and status bar.
  GtkWidget* find_panel{nullptr};
  bool find_mode{false};
  GtkSearchEntry* find_entry{nullptr};
  GtkEntry* replace_entry{nullptr};
  GtkWidget* replace_row{nullptr};
  GtkLabel* find_count{nullptr};
  GtkToggleButton* replace_toggle{nullptr};
  GtkLabel* word_count_label{nullptr};
  GtkLabel* character_count_label{nullptr};
  GtkLabel* read_time_label{nullptr};
  GtkLabel* page_label{nullptr};
  GtkLabel* page_overlay_label{nullptr};
  GtkWidget* page_footer{nullptr};
  GtkWidget* zoom_footer{nullptr};
  GtkWidget* status_bar{nullptr};
  GtkWidget* status_count_group{nullptr};
  GtkWidget* status_counts_wrapper{nullptr};
  GtkLabel* save_state{nullptr};
  GtkButton* save_button{nullptr};
  GtkWidget* save_spinner{nullptr};
  GtkButton* compatibility_button{nullptr};
  GtkWidget* status_compatibility_group{nullptr};
  GtkButton* theme_button{nullptr};
  GtkButton* zoom_button{nullptr};
  GtkLabel* zoom_label{nullptr};
  GtkLabel* zoom_overlay_label{nullptr};

  // Command search, toast.
  AdwDialog* command_palette{nullptr};
  GtkSearchEntry* command_search{nullptr};
  GtkListBox* command_results{nullptr};
  GtkLabel* command_empty_label{nullptr};
  GtkWidget* palette_footer{nullptr};
  guint find_timeout{0};
  guint action_center_refresh_timeout{0};

  bool compact{false};
  bool navigator_requested{false};
  bool inspector_requested{true};
  bool updating_panel_toggles{false};
  bool wide_navigator_requested{false};
  bool wide_inspector_requested{true};
  bool advanced_visible{false};
  bool focus_mode{false};

  std::vector<std::unique_ptr<CommandBinding>> command_bindings;
  std::vector<std::unique_ptr<RecentBinding>> recent_bindings;
  std::vector<std::unique_ptr<CommandBinding>> palette_bindings;
  std::vector<std::unique_ptr<CommandBinding>> outline_bindings;
  std::vector<std::unique_ptr<CommandBinding>> advanced_bindings;
  std::vector<std::pair<std::string, GtkWidget*>> command_widgets;
  std::vector<std::pair<std::string, GSimpleAction*>> command_actions;
  std::unordered_map<std::string, commands::CommandState> command_states;

  void build();
  GtkWidget* build_start_page();
  GtkWidget* build_editor_page();
  GtkWidget* build_command_bar();
  GtkWidget* build_navigator();
  GtkWidget* build_canvas_island();
  void rebuild_action_center();
  void sync_action_center_states();
  void build_find_bar();
  void open_find(bool replace);
  GtkWidget* build_inspector();
  GtkWidget* build_text_inspector();
  GtkWidget* build_paragraph_inspector();
  GtkWidget* build_page_inspector();
  GtkWidget* build_advanced_inspector();
  GtkWidget* build_image_inspector();
  GtkWidget* build_table_inspector();
  GtkWidget* build_comments_inspector();
  GtkWidget* build_status_bar();
  GMenuModel* build_menu_model();
  void build_command_palette();
  void install_shortcuts();
  void rebuild_palette(std::string_view query);
  void rebuild_outline();
  void rebuild_advanced(std::string_view query);
  void update_inspector();
  void rebuild_comments();
  void update_theme_toggle();
  void step_font_size(int direction);
  GtkWidget* size_stepper();

  // Editable combos and the weight list (write_combo.cpp).
  ComboField* combo_field(ComboKind kind, const char* accessible_name);
  std::string combo_value(const ComboField& combo) const;
  void show_combo_value(ComboField& combo);
  void fill_combo(ComboField& combo);
  void open_combo(ComboField& combo);
  void filter_combo(ComboField& combo, std::string_view typed);
  void step_combo(ComboField& combo, int direction);
  void commit_combo(ComboField& combo);
  void apply_combo_row(ComboField& combo, GtkWidget* row);
  void finish_combo(ComboField& combo);
  void revert_combo(ComboField& combo, bool return_focus);
  void remember_recent_font(const std::string& family);
  void update_combos();
  std::vector<WeightOption> weights_for(const std::string& family) const;
  void update_weights();
  void apply_weight(guint index);

  // Popovers shared by the toolbar and the inspector.
  GtkWidget* style_popover_content();
  GtkWidget* colour_popover(std::string_view command_id, bool highlight);
  GtkWidget* insert_popover();
  GtkWidget* table_popover();
  GtkWidget* character_popover();
  GtkWidget* zoom_popover();

  void bind_command(GtkWidget* widget, std::string_view command_id);
  void bind_argument(GtkWidget* widget, std::string_view command_id,
                     std::string argument);
  GtkWidget* command_button(const char* icon_name, const char* accessible_name,
                            std::string_view command_id, bool toggle = false);
  GtkWidget* menu_row(const char* icon_name, const char* label,
                      const char* accelerator, std::string_view command_id,
                      std::string argument = {});
  void dispatch(std::string_view command_id) const;
  void dispatch_argument(std::string_view command_id, std::string_view argument) const;
  void apply_responsive();
  void apply_panel_visibility();
  void open_inspector_tab(const char* name);

  static void command_clicked(GtkButton*, gpointer data);
  static void argument_clicked(GtkButton*, gpointer data);
  static void menu_action_activated(GSimpleAction*, GVariant*, gpointer data);
  static void recent_clicked(GtkButton*, gpointer data);
  static void palette_result_clicked(GtkButton*, gpointer data);
  static void palette_search_changed(GtkEditable*, gpointer data);
  static void navigator_toggled(GtkToggleButton*, gpointer data);
  static void inspector_toggled(GtkToggleButton*, gpointer data);
  static void close_navigator(GtkButton*, gpointer data);
  static void close_inspector(GtkButton*, gpointer data);
  static void window_size_changed(GObject*, GParamSpec*, gpointer data);
  static void panel_visibility_changed(GObject*, GParamSpec*, gpointer data);
  static gboolean window_close_requested(GtkWindow*, gpointer data);
  static gboolean palette_shortcut(GtkWidget*, GVariant*, gpointer data);
  static gboolean command_key_pressed(GtkEventControllerKey*, guint, guint,
                                      GdkModifierType, gpointer data);
};

}  // namespace luma::write::ui
