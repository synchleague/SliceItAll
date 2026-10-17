///|/ Copyright (c) Prusa Research 2018 - 2023 Tomáš Mészáros @tamasmeszaros, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, David Kocík @kocikdav, Lukáš Hejl @hejllukas, Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Pavel Mikuš @Godrak, Filip Sykala @Jony01, Vojtěch Král @vojtechkral
///|/
///|/ ported from lib/Slic3r/GUI/Plater.pm:
///|/ Copyright (c) Prusa Research 2016 - 2019 Vojtěch Bubník @bubnikv, Vojtěch Král @vojtechkral, Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Tomáš Mészáros @tamasmeszaros
///|/ Copyright (c) 2018 Martin Loidl @LoidlM
///|/ Copyright (c) 2017 Matthias Gazzari @qtux
///|/ Copyright (c) Slic3r 2012 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) 2015 Daren Schwenke
///|/ Copyright (c) 2014 Mark Hindess
///|/ Copyright (c) 2012 Mike Sheldrake @mesheldrake
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/ Copyright (c) 2012 Sam Wong
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Sidebar_hpp_
#define slic3r_Sidebar_hpp_

#include <memory>
#include <vector>
#include <string>

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/sizer.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/SLA/Workflows.hpp"
#include "GUI.hpp"
#include "Event.hpp"

class wxButton;
class wxChoice;
class wxPanel;
class wxScrolledWindow;
class wxStaticText;
class ScalableButton;
class ModeSizer;

namespace Slic3r {

class DynamicPrintConfig;
class ModelObject;

namespace GUI {

class ConfigOptionsGroup;
class FreqChangedParams;
class ObjectInfo;
class SlicedInfo;
class ObjectManipulation;
class ObjectSettings;
class ObjectLayers;
class Tab;
class ProcessTabBar;
class ProcessScopeToggle;
class ModelsPanel;
class ObjectList;
class PlaterPresetComboBox;
class Plater;
class BitmapComboBox;

enum class ActionButtonType : int {
    Reslice,
    Export,
    SendGCode,
    Connect
};

class Sidebar : public wxPanel
{
    ConfigOptionMode    m_mode{ConfigOptionMode::comSimple};

    Plater*             m_plater            { nullptr };

    wxScrolledWindow*   m_scrolled_panel    { nullptr };
    wxPanel*            m_presets_panel     { nullptr }; // Used for MSW better layouts

    wxFlexGridSizer*    m_presets_sizer     { nullptr };
    wxBoxSizer*         m_filaments_sizer   { nullptr };

    PlaterPresetComboBox*               m_combo_print       { nullptr };
    PlaterPresetComboBox*               m_combo_sla_print   { nullptr };
    PlaterPresetComboBox*               m_combo_sla_material{ nullptr };
    PlaterPresetComboBox*               m_combo_printer     { nullptr };
    BitmapComboBox*                     m_workflow          { nullptr };
    std::vector<sla::Workflow>          m_available_workflows;
    std::vector<PlaterPresetComboBox*>  m_combos_filament;

    ObjectList*     m_object_list               { nullptr };
    ObjectInfo*     m_object_info               { nullptr };
    SlicedInfo*     m_sliced_info               { nullptr };
    wxBoxSizer*     m_params_sizer              { nullptr };
    wxPanel*        m_print_settings_panel      { nullptr };
    wxBoxSizer*     m_print_settings_sizer      { nullptr };
    std::vector<Tab*> m_print_tabs;
    wxBoxSizer*     m_btns_sizer                { nullptr };
    wxBoxSizer*     m_autoslicing_btns_sizer    { nullptr };


    ScalableButton* m_btn_export_gcode          { nullptr };
    wxButton*       m_btn_reslice               { nullptr };
    wxButton*       m_btn_generate_supports     { nullptr };
    wxButton*       m_btn_connect_gcode         { nullptr };
    wxButton*       m_btn_send_gcode            { nullptr };
    ScalableButton* m_btn_export_gcode_removable{ nullptr }; //exports to removable drives (appears only if removable drive is connected)
                                                             //
    wxButton* m_btn_export_all_gcode                { nullptr };
    wxButton* m_btn_connect_gcode_all               { nullptr };
	ScalableButton* m_btn_export_all_gcode_removable{ nullptr };

    wxButton*                           m_btn_full_spectrum { nullptr };
    wxButton*                           m_btn_flushing { nullptr };
    wxButton*                           m_btn_add_filament { nullptr };
    wxButton*                           m_btn_import_filaments { nullptr };
    wxButton*                           m_btn_paint_filaments { nullptr };
    wxButton*                           m_btn_layer_filaments { nullptr };
    ScalableButton*                     m_btn_filament_gear { nullptr };
    ProcessScopeToggle*                 m_process_scope { nullptr };
    wxChoice*                           m_plate_type { nullptr };
    wxSizer*                            m_plate_type_row { nullptr };
    wxPanel*                            m_process_panel { nullptr };
    wxStaticText*                       m_process_hint { nullptr };
    wxBoxSizer*                         m_process_pages_sizer { nullptr };
    ProcessTabBar*                      m_process_tabs { nullptr };
    ProcessTabBar*                      m_sla_process_tabs { nullptr };
    ProcessTabBar*                      m_pane_tabs { nullptr };
    wxBoxSizer*                         m_content_sizer { nullptr };
    ModelsPanel*                        m_models_panel { nullptr };
    bool                                m_show_models { false };
    bool                                m_split { false };
    std::vector<wxPanel*>               m_process_pages;
    std::vector<wxPanel*>               m_sla_pages;
    int m_process_field_width { 0 };
    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_process_groups;
    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_sla_groups;
    std::unique_ptr<DynamicPrintConfig> m_process_global;
    int                                 m_process_category { 0 };
    int                                 m_sla_category { 0 };
    int                                 m_process_object_idx { -2 };
    bool                                m_process_objects { false };
    bool                                m_process_applying { false };
    bool                                m_sla_applying { false };
    bool                                m_process_prompting { false };
    bool                                m_plate_syncing { false };
    Preset::Type                        m_revealed_settings { Preset::TYPE_INVALID };
    struct FilamentSlotWidgets {
        wxStaticText* index { nullptr };
        wxPanel*      swatch { nullptr };
        wxButton*     menu { nullptr };
    };
    std::vector<FilamentSlotWidgets>    m_filament_slots;
    std::unique_ptr<FreqChangedParams>  m_frequently_changed_parameters;
    std::unique_ptr<ObjectManipulation> m_object_manipulation;
    std::unique_ptr<ObjectSettings>     m_object_settings;
    std::unique_ptr<ObjectLayers>       m_object_layers;

    bool m_autoslicing_mode{ false };
#ifdef _WIN32
    wxString m_reslice_btn_tooltip;
#endif

    void init_filament_combo(PlaterPresetComboBox **combo, int extr_idx);
    void build_filament_header();
    void build_process_panel();
    void on_process_option(const std::string& opt_id);
    void on_sla_option(Preset::Type type, const std::string& opt_id);
    void set_process_category(int index);
    void set_sla_category(int index);
    void reload_sla_groups();
    void set_process_objects_mode(bool objects);
    void update_process_chrome();
    void sync_plate_type();
    int  selected_object_idx() const;
    ModelObject* selected_process_object() const;
    void make_filament_row(PlaterPresetComboBox* combo, int extr_idx);
    void add_filament_slot();
    void import_printer_filaments();
    void remove_filament_slot(int idx);
    void show_filament_slot_menu(int idx, wxButton* btn);
    void edit_filament_slot(int idx);
    void open_flushing_volumes();
    void start_filament_paint(bool height_range);
    void enable_single_nozzle_multimaterial();
    void remove_unused_filament_combos(const size_t current_extruder_count);
    void update_all_preset_comboboxes();
    void update_reslice_btn_tooltip();
    void init_workflow_combo(int margin_5);

    void show_preset_comboboxes();
    void update_process_tab_visibility(bool show_sla);
    void on_select_preset(wxCommandEvent& evt);

public:
    // Print and DLP print pages live in the sidebar instead of the top notebook.
    void host_print_tab(Tab* tab);
    void update_print_settings_tab();
    void focus_print_settings();
    void focus_settings_tab(Preset::Type type);
    void update_filament_badges();
    std::string filament_slot_color(size_t idx) const;
    void reload_process_settings();
    void offer_global_change_to_objects(const std::string& key);
    void install_pane_bar();
    void apply_pane_visibility();
    void move_model_tools(ModelsPanel* panel);
    void adopt_models_panel(ModelsPanel* panel);
    wxWindow* models_window() const;
    void show_models_pane(bool models);
    void dock_models();
    void undock_models();
    void refresh_layout_buttons();

public:
    Sidebar(Plater *parent);
    Sidebar(Sidebar &&) = delete;
    Sidebar(const Sidebar &) = delete;
    Sidebar &operator=(Sidebar &&) = delete;
    Sidebar &operator=(const Sidebar &) = delete;
    ~Sidebar();

    ObjectManipulation*     obj_manipul();
    ObjectList*             obj_list();
    ObjectSettings*         obj_settings();
    ObjectLayers*           obj_layers();

    ConfigOptionsGroup*     og_freq_chng_params(const bool is_fff);
    ConfigOptionsGroup*     og_sla_material_params();
    void                    sync_sla_material_params();
    wxButton*               get_wiping_dialog_button();

    void show_info_sizer();
    void show_sliced_info_sizer(const bool show);
    void show_btns_sizer(const bool show);
    void show_bulk_btns_sizer(const bool show);

    void update_sliced_info_sizer();

    void enable_buttons(bool enable);
    void set_btn_label(const ActionButtonType btn_type, const wxString& label) const;
    bool show_reslice(bool show) const;
    bool show_generate_supports(bool show) const;
    bool show_export(bool show) const;
    bool show_send(bool show) const;
    bool show_export_removable(bool show) const;
    bool show_connect(bool show) const;

    void enable_bulk_buttons(bool enable);
    bool show_export_all(bool show) const;
    bool show_export_removable_all(bool show) const;
    bool show_connect_all(bool show) const;

    void switch_to_autoslicing_mode();
    void switch_from_autoslicing_mode();

    void collapse(bool collapse);
    void set_extruders_count(size_t extruders_count);

    void update_mode();
    void update_ui_from_settings();
    void update_objects_list_extruder_column(size_t extruders_count);
    void update_presets(Preset::Type preset_type);
    void update_printer_presets_combobox();
    void update_all_filament_comboboxes();
    void update_workflow_combobox_if_needed();

    void on_workflow_changed();

    void msw_rescale();
    void sys_color_changed();

    bool is_collapsed{ false };
};

} // namespace GUI
} // namespace Slic3r

#endif
