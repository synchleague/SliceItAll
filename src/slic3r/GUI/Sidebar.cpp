///|/ Copyright (c) Prusa Research 2018 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Tomáš Mészáros @tamasmeszaros, David Kocík @kocikdav, Lukáš Hejl @hejllukas, Pavel Mikuš @Godrak, Filip Sykala @Jony01, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2022 Michael Kirsch
///|/ Copyright (c) 2021 Boleslaw Ciesielski
///|/ Copyright (c) 2019 John Drake @foxox
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
#include "Sidebar.hpp"
#include "EditorPlugin.hpp"
#include "FrequentlyChangedParameters.hpp"
#include "Plater.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <boost/algorithm/string.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/dialog.h>
#include <wx/slider.h>
#include <wx/bmpcbox.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/wupdlock.h> // IWYU pragma: keep
#include "libslic3r/MultipleBeds.hpp"
#include "wx/generic/stattextg.h"
#ifdef _WIN32
#include <wx/richtooltip.h>
#include <wx/custombgwin.h>
#include <wx/popupwin.h>
#endif

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ModelProcessing.hpp"
#include "libslic3r/SLA/Workflows.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "GUI_ObjectLayers.hpp"
#include "wxExtensions.hpp"
#include "format.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "I18N.hpp"
#include "OptionsGroup.hpp"

#include <wx/choice.h>
#include <wx/wrapsizer.h>

#include "NotificationManager.hpp"
#include "PresetComboBoxes.hpp"
#include "MsgDialog.hpp"
#include "FilamentRack.hpp"
#include "GLCanvas3D.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "Gizmos/GLGizmoPainterBase.hpp"

#include <wx/menu.h>
#include <wx/dcclient.h>
#include "FullSpectrumDialog.hpp"
#include "MainFrame.hpp"

using Slic3r::Preset;
using Slic3r::GUI::format_wxstr;

namespace Slic3r {
namespace GUI {

static bool
renumber_virtual_extruders(Plater& plater, unsigned int num_physical, bool take_snapshot = false)
{
    Model& model = plater.model();
    if (model.virtual_extruders.empty()) {
        return false;
    }

    std::map<unsigned int, unsigned int> remap;
    unsigned int next_id = num_physical + 1;
    for (FullSpectrum::VirtualExtruder& ve : model.virtual_extruders) {
        if (ve.id != next_id) {
            remap[ve.id] = next_id;
            ve.id        = next_id;
        }

        ++next_id;
    }

    if (remap.empty()) {
        return false;
    }

    if (take_snapshot) {
        plater.take_snapshot(_L("Renumber virtual extruders"));
    }

    for (ModelObject* object : model.objects) {
        if (!object) {
            continue;
        }

        if (object->config.has("extruder")) {
            auto it = remap.find(static_cast<unsigned int>(object->config.extruder()));
            if (it != remap.end()) {
                object->config.set_key_value("extruder", new ConfigOptionInt(int(it->second)));
            }
        }
        for (ModelVolume* volume : object->volumes) {
            if (!volume) {
                continue;
            }
            if (volume->config.has("extruder")) {
                auto it = remap.find(static_cast<unsigned int>(volume->config.extruder()));
                if (it != remap.end()) {
                    volume->config.set_key_value("extruder", new ConfigOptionInt(int(it->second)));
                }
            }

            if (volume->is_mm_painted()) {
                bool needs = false;
                const std::vector<bool>& used =
                    volume->mm_segmentation_facets.get_data().used_states;
                for (const auto& [old_id, new_id] : remap) {
                    if (old_id < used.size() && used[old_id]) {
                        needs = true;
                        break;
                    }
                }

                if (needs) {
                    TriangleSelector selector(volume->mesh());
                    selector.deserialize(volume->mm_segmentation_facets.get_data(), false);
                    std::map<TriangleStateType, TriangleStateType> state_remap;
                    for (const auto& [old_id, new_id] : remap) {
                        state_remap[TriangleStateType(old_id)] = TriangleStateType(new_id);
                    }

                    selector.remap_states(state_remap);
                    volume->mm_segmentation_facets.set(selector);
                }
            }
        }
    }
    return true;
}

class ObjectInfo : public wxStaticBoxSizer
{
    std::string m_warning_icon_name{ "exclamation" };
public:
    ObjectInfo(wxWindow *parent);

    wxStaticBitmap *manifold_warning_icon;
    wxStaticBitmap *info_icon;
    wxStaticText *info_size;
    wxStaticText *info_volume;
    wxStaticText *info_facets;
    wxStaticText *info_manifold;

    wxStaticText *label_volume;
    std::vector<wxStaticText *> sla_hidden_items;

    bool        showing_manifold_warning_icon;
    void        show_sizer(bool show);
    void        update_warning_icon(const std::string& warning_icon_name);
};

ObjectInfo::ObjectInfo(wxWindow *parent) :
    wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, _L("Info")), wxVERTICAL)
{
    GetStaticBox()->SetFont(wxGetApp().bold_font());
    wxGetApp().UpdateDarkUI(GetStaticBox());

    auto *grid_sizer = new wxFlexGridSizer(4, 5, 15);
    grid_sizer->SetFlexibleDirection(wxHORIZONTAL);

    auto init_info_label = [parent, grid_sizer](wxStaticText **info_label, wxString text_label, wxSizer* sizer_with_icon=nullptr) {
        auto *text = new wxStaticText(parent, wxID_ANY, text_label + ":");
        text->SetFont(wxGetApp().small_font());
        *info_label = new wxStaticText(parent, wxID_ANY, "");
        (*info_label)->SetFont(wxGetApp().small_font());
        grid_sizer->Add(text, 0);
        if (sizer_with_icon) {
            sizer_with_icon->Insert(0, *info_label, 0);
            grid_sizer->Add(sizer_with_icon, 0, wxEXPAND);
        }
        else
            grid_sizer->Add(*info_label, 0);
        return text;
    };

    init_info_label(&info_size, _L("Size"));

    info_icon = new wxStaticBitmap(parent, wxID_ANY, *get_bmp_bundle("info"));
    info_icon->SetToolTip(_L("For a multipart object, this value isn't accurate.\n"
                             "It doesn't take account of intersections and negative volumes."));
    auto* volume_info_sizer = new wxBoxSizer(wxHORIZONTAL);
    volume_info_sizer->Add(info_icon, 0, wxLEFT, 10);
    label_volume = init_info_label(&info_volume, _L("Volume"), volume_info_sizer);

    init_info_label(&info_facets, _L("Facets"));
    Add(grid_sizer, 0, wxEXPAND);

    info_manifold = new wxStaticText(parent, wxID_ANY, "");
    info_manifold->SetFont(wxGetApp().small_font());
    manifold_warning_icon = new wxStaticBitmap(parent, wxID_ANY, *get_bmp_bundle(m_warning_icon_name));
    auto *sizer_manifold = new wxBoxSizer(wxHORIZONTAL);
    sizer_manifold->Add(manifold_warning_icon, 0, wxLEFT, 2);
    sizer_manifold->Add(info_manifold, 0, wxLEFT, 2);
    Add(sizer_manifold, 0, wxEXPAND | wxTOP, 4);

    sla_hidden_items = { label_volume, info_volume, };

    // Fixes layout issues on plater, short BitmapComboBoxes with some Windows scaling, see GH issue #7414.
    this->Show(false);
}

void ObjectInfo::show_sizer(bool show)
{
    Show(show);
    if (show)
        manifold_warning_icon->Show(showing_manifold_warning_icon && show);
}

void ObjectInfo::update_warning_icon(const std::string& warning_icon_name)
{
    if ((showing_manifold_warning_icon = !warning_icon_name.empty())) {
        m_warning_icon_name = warning_icon_name;
        manifold_warning_icon->SetBitmap(*get_bmp_bundle(m_warning_icon_name));
    }
}


// Note: SlicedInfoIdxs are used in code as indexes, so enum is preferred here than enum class
enum SlicedInfoIdx
{
    siFilament_g,
    siFilament_m,
    siFilament_mm3,
    siMaterial_unit,
    siCost,
    siEstimatedTime,
    siWTNumberOfToolchanges,

    siCount
};

class SlicedInfo : public wxStaticBoxSizer
{
public:
    SlicedInfo(wxWindow *parent);
    void SetTextAndShow(SlicedInfoIdx idx, const wxString& text, const wxString& new_label="");

private:
    std::vector<std::pair<wxStaticText*, wxStaticText*>> info_vec;
};

SlicedInfo::SlicedInfo(wxWindow *parent) :
    wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, _L("Sliced Info")), wxVERTICAL)
{
    GetStaticBox()->SetFont(wxGetApp().bold_font());
    wxGetApp().UpdateDarkUI(GetStaticBox());

    auto *grid_sizer = new wxFlexGridSizer(2, 5, 15);
    grid_sizer->SetFlexibleDirection(wxVERTICAL);

    info_vec.reserve(siCount);

    auto init_info_label = [this, parent, grid_sizer](wxString text_label) {
        auto *text = new wxStaticText(parent, wxID_ANY, text_label);
        text->SetFont(wxGetApp().small_font());
        auto info_label = new wxStaticText(parent, wxID_ANY, "N/A");
        info_label->SetFont(wxGetApp().small_font());
        grid_sizer->Add(text, 0);
        grid_sizer->Add(info_label, 0);
        info_vec.push_back(std::pair<wxStaticText*, wxStaticText*>(text, info_label));
    };

    init_info_label(_L("Used Filament (g)"));
    init_info_label(_L("Used Filament (m)"));
    init_info_label(_L("Used Filament (mm³)"));
    init_info_label(_L("Used Material (unit)"));
    init_info_label(_L("Cost (money)"));
    init_info_label(_L("Estimated printing time"));
    init_info_label(_L("Number of tool changes"));

    Add(grid_sizer, 0, wxEXPAND);
    this->Show(false);
}

void SlicedInfo::SetTextAndShow(SlicedInfoIdx idx, const wxString& text, const wxString& new_label/*=""*/)
{
    const bool show = text != "N/A";
    if (show)
        info_vec[idx].second->SetLabelText(text);
    if (!new_label.IsEmpty())
        info_vec[idx].first->SetLabelText(new_label);
    info_vec[idx].first->Show(show);
    info_vec[idx].second->Show(show);
}


// Sidebar / private

static void reparent_sizer_windows(wxSizer* sizer, wxWindow* parent)
{
    if (!sizer)
        return;
    for (size_t i = 0; i < sizer->GetItemCount(); ++i) {
        wxSizerItem* item = sizer->GetItem(i);
        if (item->IsWindow() && item->GetWindow())
            item->GetWindow()->Reparent(parent);
        else if (item->IsSizer())
            reparent_sizer_windows(item->GetSizer(), parent);
    }
}

void Sidebar::host_print_tab(Tab* tab)
{
    if (!tab || !m_print_settings_panel || !m_print_settings_sizer)
        return;
    tab->Hide();
    tab->Reparent(m_print_settings_panel);
    tab->fit_into_sidebar();
    m_print_settings_sizer->Add(tab, 1, wxEXPAND);
    tab->OnActivate();
    m_print_tabs.push_back(tab);
    update_print_settings_tab();
}

void Sidebar::update_print_settings_tab()
{
    if (!m_print_settings_sizer)
        return;
    const bool sla = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA;
    bool any = false;
    for (Tab* tab : m_print_tabs) {
        bool show = false;
        if (sla) {
            show = false;
        } else if (m_revealed_settings != Preset::TYPE_INVALID) {
            show = tab->type() == m_revealed_settings;
        }
        m_print_settings_sizer->Show(tab, show);
        any = any || show;
    }
    if (m_print_settings_panel)
        m_print_settings_panel->Show(any);
    if (m_print_settings_panel)
        m_print_settings_panel->Layout();
    if (m_scrolled_panel)
        m_scrolled_panel->Layout();
}

void Sidebar::focus_settings_tab(Preset::Type type)
{
    if (m_revealed_settings == type && m_print_settings_panel && m_print_settings_panel->IsShown()) {
        m_revealed_settings = Preset::TYPE_INVALID;
        update_print_settings_tab();
        return;
    }
    m_revealed_settings = type;
    update_print_settings_tab();
    if (!m_print_settings_panel || !m_scrolled_panel)
        return;
    Tab* target = nullptr;
    for (Tab* tab : m_print_tabs)
        if (tab->type() == type) {
            target = tab;
            break;
        }
    if (!target)
        return;
    int xu = 1;
    int yu = 1;
    m_scrolled_panel->GetScrollPixelsPerUnit(&xu, &yu);
    if (yu < 1)
        yu = 1;
    const int y = m_print_settings_panel->GetPosition().y + target->GetPosition().y;
    m_scrolled_panel->Scroll(-1, y / yu);
    target->SetFocus();
}

void Sidebar::focus_print_settings()
{
    const bool sla = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA;
    focus_settings_tab(sla ? Preset::TYPE_SLA_PRINT : Preset::TYPE_PRINT);
}

void Sidebar::move_model_tools(ModelsPanel* panel)
{
    if (!panel || !m_object_list || !m_params_sizer)
        return;

    m_object_list->Reparent(panel);
    m_params_sizer->Detach(m_object_list->get_sizer());
    panel->tools_sizer()->Add(m_object_list->get_sizer(), 1, wxEXPAND | wxLEFT | wxRIGHT, 6);

    auto move_og = [&](OG_Settings* og) {
        if (!og || !og->get_sizer())
            return;
        reparent_sizer_windows(og->get_sizer(), panel);
        og->set_parent(panel);
        m_params_sizer->Detach(og->get_sizer());
        panel->tools_sizer()->Add(og->get_sizer(), 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 6);
    };
    move_og(m_object_manipulation.get());
    move_og(m_object_settings.get());
    move_og(m_object_layers.get());
    panel->set_object_list(m_object_list);
    panel->Layout();
    if (m_scrolled_panel)
        m_scrolled_panel->Layout();
}

void Sidebar::show_preset_comboboxes()
{
    const bool showSLA = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA;

    // 0-2 printer (label, combo, connect), 3-4 filament, 5 color mixing,
    // 6-7 resin, 8-9 FDM print, 10-11 DLP print, 12-13 workflow.
    auto show_row = [this](size_t index, bool show, bool reveal_children = false) {
        if (!m_presets_sizer || index >= m_presets_sizer->GetItemCount())
            return;
        wxSizerItem* item = m_presets_sizer->GetItem(index);
        if (!item)
            return;
        if (reveal_children && item->IsSizer() && item->GetSizer())
            item->GetSizer()->ShowItems(show);
        m_presets_sizer->Show(index, show);
        item->SetMinSize(show ? wxSize(-1, -1) : wxSize(0, 0));
    };
    show_row(3, false);
    show_row(4, !showSLA);
    show_row(5, !showSLA && m_btn_full_spectrum && int(m_combos_filament.size()) >= 2);
    show_row(6, showSLA);
    show_row(7, showSLA, true);
    if (wxSizerItem* material = m_presets_sizer->GetItem(6); material && material->IsWindow()) {
        if (auto* text = dynamic_cast<wxStaticText*>(material->GetWindow())) {
            text->SetLabel(_L("Material"));
            text->SetFont(wxGetApp().bold_font());
        }
    }
    show_row(8, !showSLA, true);
    show_row(9, !showSLA, true);
    show_row(10, showSLA);
    show_row(11, showSLA, true);
    if (wxSizerItem* process = m_presets_sizer->GetItem(10); process && process->IsWindow()) {
        if (auto* text = dynamic_cast<wxStaticText*>(process->GetWindow())) {
            text->SetLabel(_L("Process"));
            text->SetFont(wxGetApp().bold_font());
        }
    }

    m_frequently_changed_parameters->Show(!showSLA);
    if (m_params_sizer && m_frequently_changed_parameters->get_sizer())
        m_params_sizer->Show(m_frequently_changed_parameters->get_sizer(), false);
    if (m_plate_type_row)
        m_plate_type_row->ShowItems(!showSLA);
    update_process_tab_visibility(showSLA);
    if (m_process_panel)
        m_process_panel->Show(true);
    if (m_presets_panel)
        m_presets_panel->Layout();
    sync_plate_type();
    reload_process_settings();

    const Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    bool is_prusa_slx = showSLA && tab != nullptr && tab->is_prusa_printer() && tab->printer_model() == "SLX";
    show_row(12, is_prusa_slx);
    show_row(13, is_prusa_slx, true);

    m_scrolled_panel->GetParent()->Layout();
    m_scrolled_panel->Refresh();
    update_print_settings_tab();
}

#ifdef _WIN32
using wxRichToolTipPopup = wxCustomBackgroundWindow<wxPopupTransientWindow>;
static wxRichToolTipPopup* get_rtt_popup(wxButton* btn)
{
    auto children = btn->GetChildren();
    for (auto child : children)
        if (child->IsShown())
            return dynamic_cast<wxRichToolTipPopup*>(child);
    return nullptr;
}

// Help function to find and check if some combobox is dropped down and then dismiss it
static bool found_and_dismiss_shown_dropdown(wxWindow* win)
{
    auto children = win->GetChildren();
    if (children.IsEmpty()) {
        if (auto dd = dynamic_cast<DropDown*>(win); dd && dd->IsShown()) {
            dd->CallDismissAndNotify();
            return true;
        }
    }

    for (auto child : children) {
        if (found_and_dismiss_shown_dropdown(child))
            return true;
    }
    return false;
}

static void show_rich_tip(const wxString& tooltip, wxButton* btn)
{   
    if (tooltip.IsEmpty())
        return;

    // Currently state (propably wxWidgets issue) : 
    // When second wxPopupTransientWindow is popped up, then first wxPopupTransientWindow doesn't receive EVT_DISMISS and stay on the top. 
    // New comboboxes use wxPopupTransientWindow as DropDown now
    // That is why DropDown stay on top, when we show rich tooltip for btn.
    // (see https://github.com/prusa3d/PrusaSlicer/issues/11988)

    // So, check the combo boxes and close them if necessary before showing the rich tip.
    found_and_dismiss_shown_dropdown(btn->GetParent());

    wxRichToolTip tip(tooltip, "");
    tip.SetIcon(wxICON_NONE);
    tip.SetTipKind(wxTipKind_BottomRight);
    tip.SetTitleFont(wxGetApp().normal_font());
    tip.SetBackgroundColour(wxGetApp().get_window_default_clr());

    tip.ShowFor(btn);
    // Every call of the ShowFor() creates new RichToolTip and show it.
    // Every one else are hidden. 
    // So, set a text color just for the shown rich tooltip
    if (wxRichToolTipPopup* popup = get_rtt_popup(btn)) {
        auto children = popup->GetChildren();
        for (auto child : children) {
            child->SetForegroundColour(wxGetApp().get_label_clr_default());
            // we neen just first text line for out rich tooltip
            return;
        }
    }
}

static void hide_rich_tip(wxButton* btn)
{
    if (wxRichToolTipPopup* popup = get_rtt_popup(btn))
        popup->Dismiss();
}
#endif

// Sidebar / public

Sidebar::Sidebar(Plater *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(42 * wxGetApp().em_unit(), -1)), m_plater(parent)
{
    m_scrolled_panel = new wxScrolledWindow(this);
    m_scrolled_panel->SetScrollRate(0, 5);

    SetFont(wxGetApp().normal_font());
#ifndef __APPLE__
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDarkUI(m_scrolled_panel);
#else
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif
#endif

    // Sizer in the scrolled area
    auto *scrolled_sizer = new wxBoxSizer(wxVERTICAL);
    m_scrolled_panel->SetSizer(scrolled_sizer);

    // The preset chooser
    m_presets_sizer = new wxFlexGridSizer(12, 1, 1, 2);
    m_presets_sizer->AddGrowableCol(0, 1);
    m_presets_sizer->SetFlexibleDirection(wxBOTH);

#ifdef __WINDOWS__
    m_scrolled_panel->SetDoubleBuffered(true);
#endif
    // Printer and filament stay above the Models / Settings tabs.
    m_presets_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    wxGetApp().UpdateDarkUI(m_presets_panel);
    m_presets_panel->SetSizer(m_presets_sizer);

    m_filaments_sizer = new wxBoxSizer(wxVERTICAL);

    const int margin_5 = int(0.5 * wxGetApp().em_unit());// 5;

    auto init_combo = [this, margin_5](PlaterPresetComboBox **combo, wxString label, Preset::Type preset_type, bool filament) {
        *combo = new PlaterPresetComboBox(m_presets_panel, preset_type);
        const int gear_gap = int(0.3 * wxGetApp().em_unit());
        auto *sizer_presets = this->m_presets_sizer;
        auto *sizer_filaments = this->m_filaments_sizer;

        auto add_header = [&](const wxString& title, ScalableButton* gear) {
            auto* header = new wxBoxSizer(wxHORIZONTAL);
            auto* text = new wxStaticText(m_presets_panel, wxID_ANY, title);
            text->SetFont(wxGetApp().bold_font());
            header->Add(text, 0, wxALIGN_CENTER_VERTICAL);
            header->AddStretchSpacer(1);
            if (gear)
                header->Add(gear, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gear_gap);
            sizer_presets->Add(header, 0, wxEXPAND | wxBOTTOM, gear_gap);
            return header;
        };

        if (preset_type == Preset::TYPE_PRINTER) {
            if ((*combo)->edit_btn)
                (*combo)->edit_btn->SetToolTip(_L("Edit printer preset"));
            add_header(_L("Printer"), (*combo)->edit_btn);

            auto* column = new wxBoxSizer(wxVERTICAL);
            column->Add(*combo, 0, wxEXPAND | wxBOTTOM, gear_gap);

            m_plate_type_row = new wxBoxSizer(wxHORIZONTAL);
            auto* plate_label = new wxStaticText(m_presets_panel, wxID_ANY, _L("Plate type"));
            plate_label->SetFont(wxGetApp().small_font());
            m_plate_type = new wxChoice(m_presets_panel, wxID_ANY);
            const wxString plate_labels[] = {
                _L("Textured PEI Plate"),
                _L("Smooth PEI Plate"),
                _L("Cool Plate"),
                _L("Engineering Plate"),
                _L("High Temp Plate")
            };
            for (const wxString& plate : plate_labels)
                m_plate_type->Append(plate);
            m_plate_type->SetToolTip(_L("Build plate surface for this printer."));
            m_plate_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
                if (m_plate_syncing || !m_plate_type || !wxGetApp().app_config)
                    return;
                static const char* ids[] = {
                    "Textured PEI Plate", "Smooth PEI Plate", "Cool Plate", "Engineering Plate", "High Temp Plate"
                };
                const int sel = m_plate_type->GetSelection();
                if (sel >= 0 && sel < int(sizeof(ids) / sizeof(ids[0])))
                    wxGetApp().app_config->set("plate_type", ids[sel]);
            });
#ifdef _WIN32
            wxGetApp().UpdateDarkUI(m_plate_type);
#endif
            m_plate_type_row->Add(plate_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gear_gap);
            m_plate_type_row->Add(m_plate_type, 1, wxEXPAND);
            column->Add(m_plate_type_row, 0, wxEXPAND);
            sizer_presets->Add(column, 0, wxEXPAND | wxBOTTOM, 1);
            sync_plate_type();

            if ((*combo)->connect_info_sizer) {
                auto tmp_h_sizer = new wxBoxSizer(wxHORIZONTAL);
                tmp_h_sizer->Add((*combo)->connect_info_sizer, 1, wxEXPAND);
                sizer_presets->Add(tmp_h_sizer, 0, wxBOTTOM, int(0.3 * wxGetApp().em_unit()));
            }
            return;
        }

        if (preset_type == Preset::TYPE_PRINT) {
            if ((*combo)->edit_btn)
                (*combo)->edit_btn->SetToolTip(_L("Edit process preset"));
            add_header(_L("Process"), (*combo)->edit_btn);
            sizer_presets->Show(sizer_presets->GetItemCount() - 1, false);

            auto* column = new wxBoxSizer(wxVERTICAL);
            column->Add(*combo, 0, wxEXPAND);
            sizer_presets->Add(column, 0, wxEXPAND | wxBOTTOM, 1);
            sizer_presets->Show(sizer_presets->GetItemCount() - 1, false);
            (void)margin_5;
            return;
        }

        auto *text = new wxStaticText(m_presets_panel, wxID_ANY, label + ":");
        text->SetFont(wxGetApp().small_font());
        auto combo_and_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
        combo_and_btn_sizer->Add(*combo, 1, wxEXPAND);
        if ((*combo)->edit_btn)
            combo_and_btn_sizer->Add((*combo)->edit_btn, 0, wxALIGN_CENTER_VERTICAL|wxLEFT|wxRIGHT, gear_gap);

        text->Show(false);
        sizer_presets->Add(text, 0, wxALIGN_LEFT | wxEXPAND | wxRIGHT, 4);
        if (! filament) {
            combo_and_btn_sizer->ShowItems(false);
            sizer_presets->Add(combo_and_btn_sizer, 0, wxEXPAND |
#ifdef __WXGTK3__
                wxRIGHT, margin_5);
#else
                wxBOTTOM, 1);
                (void)margin_5;
#endif
        } else {
            text->SetLabel("");
            text->SetMinSize(wxSize(0, 0));
            text->Hide();
            combo_and_btn_sizer->Detach(*combo);
            if ((*combo)->edit_btn) {
                combo_and_btn_sizer->Detach((*combo)->edit_btn);
                (*combo)->edit_btn->Hide();
            }
            delete combo_and_btn_sizer;
            (*combo)->set_extruder_idx(0);
            this->make_filament_row(*combo, 0);
            sizer_filaments->ShowItems(false);
            sizer_presets->Add(sizer_filaments, 1, wxEXPAND);
            (void)margin_5;
        }
    };

    m_combos_filament.push_back(nullptr);
    build_filament_header();
    init_combo(&m_combo_printer,       _L("Printer"),            Preset::TYPE_PRINTER,       false);
    init_combo(&m_combos_filament[0],  _L("Filament"),           Preset::TYPE_FILAMENT,      true);

    m_btn_full_spectrum = new wxButton(m_presets_panel, wxID_ANY,
                                       _L("Virtual Extruders - Color Mixing"),
                                       wxDefaultPosition, wxDefaultSize,
                                       wxBU_EXACTFIT);
    m_btn_full_spectrum->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (GLCanvas3D *canvas = m_plater->canvas3D(); canvas != nullptr)
            canvas->reset_all_gizmos();
        FullSpectrumDialog dialog(static_cast<wxWindow*>(wxGetApp().mainframe), m_plater->model(),
                                  wxGetApp().preset_bundle->full_config());
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        m_plater->take_snapshot(_L("Update virtual extruders"));
        Model &model = m_plater->model();
        for (unsigned int removed_id : dialog.removed_ids()) {
            for (ModelObject *object : model.objects) {
                if (object == nullptr) continue;
                if (object->config.has("extruder") && object->config.extruder() == int(removed_id))
                    object->config.erase("extruder");
                for (ModelVolume *volume : object->volumes) {
                    if (volume == nullptr) continue;
                    if (volume->config.has("extruder") && volume->config.extruder() == int(removed_id))
                        volume->config.erase("extruder");
                    if (volume->is_mm_painted()) {
                        const auto& used = volume->mm_segmentation_facets.get_data().used_states;
                        if (removed_id < used.size() && used[removed_id]) {
                            TriangleSelector selector(volume->mesh());
                            selector.deserialize(volume->mm_segmentation_facets.get_data(), false);
                            selector.remap_states({{TriangleStateType(removed_id), TriangleStateType::NONE}});
                            volume->mm_segmentation_facets.set(selector);
                        }
                    }
                }
            }
        }
        model.virtual_extruders = FullSpectrum::normalize_virtual_extruders(
            dialog.result_virtual_extruders());

        renumber_virtual_extruders(*m_plater, static_cast<unsigned int>(m_combos_filament.size()));

        for (size_t obj_idx = 0; obj_idx < model.objects.size(); ++obj_idx)
            m_object_list->update_info_items(obj_idx);

        m_plater->schedule_background_process();
        update_objects_list_extruder_column(m_combos_filament.size());
        m_object_list->Refresh();
        if (GLCanvas3D *canvas = m_plater->canvas3D(); canvas != nullptr) {
            canvas->reload_scene(true);
        }
    });
    m_btn_full_spectrum->Show(false);
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(m_btn_full_spectrum, true);
#endif
    {
        auto *btn_row_sizer = new wxBoxSizer(wxHORIZONTAL);
        btn_row_sizer->Add(m_btn_full_spectrum, 1, wxEXPAND);
        const int gear_col_width = m_combos_filament[0]->edit_btn->GetBestSize().GetWidth()
                                 + 2 * int(0.3 * wxGetApp().em_unit());
        btn_row_sizer->AddSpacer(gear_col_width);
        m_presets_sizer->Add(btn_row_sizer, 0, wxEXPAND | wxBOTTOM, 2);
    }

    init_combo(&m_combo_sla_material,  _L("Material"),           Preset::TYPE_SLA_MATERIAL,  false);
    init_combo(&m_combo_print,         _L("Print settings"),     Preset::TYPE_PRINT,         false);
    init_combo(&m_combo_sla_print,     _L("Print settings"),     Preset::TYPE_SLA_PRINT,     false);

    init_workflow_combo(margin_5);

    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        const int width = GetClientSize().GetWidth();
        if (width < 40)
            return;
        const int cap = std::max(80, width - 8);
        auto fit = [cap](wxWindow* win) {
            if (win && win->GetMaxSize().GetWidth() != cap)
                win->SetMaxSize(wxSize(cap, -1));
        };
        fit(m_combo_printer);
        fit(m_combo_print);
        fit(m_combo_sla_print);
        fit(m_combo_sla_material);
        fit(m_plate_type);
        for (PlaterPresetComboBox* combo : m_combos_filament)
            fit(combo);
    });

    m_params_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* params_sizer = m_params_sizer;

    build_process_panel();
    params_sizer->Add(m_process_panel, 0, wxEXPAND | wxTOP | wxBOTTOM, margin_5);

    // Frequently changed parameters stay for DLP. FDM uses the process pages above.
    m_frequently_changed_parameters = std::make_unique<FreqChangedParams>(m_scrolled_panel);
    params_sizer->Add(m_frequently_changed_parameters->get_sizer(), 0, wxEXPAND | wxTOP | wxBOTTOM
#ifdef __WXGTK3__
        | wxRIGHT
#endif // __WXGTK3__
        , wxOSX ? 1 : margin_5);
    if (m_btn_full_spectrum)
        m_presets_sizer->Show(size_t(5), int(m_combos_filament.size()) >= 2);

    m_print_settings_panel = new wxPanel(m_scrolled_panel);
    m_print_settings_sizer = new wxBoxSizer(wxVERTICAL);
    m_print_settings_panel->SetSizer(m_print_settings_sizer);
    params_sizer->Add(m_print_settings_panel, 1, wxEXPAND | wxTOP, margin_5);

    // Object List
    m_object_list = new ObjectList(m_scrolled_panel);
    params_sizer->Add(m_object_list->get_sizer(), 1, wxEXPAND);

    // Object Manipulations
    m_object_manipulation = std::make_unique<ObjectManipulation>(m_scrolled_panel);
    m_object_manipulation->Hide();
    params_sizer->Add(m_object_manipulation->get_sizer(), 0, wxEXPAND | wxTOP, margin_5);

    // Frequently Object Settings
    m_object_settings = std::make_unique<ObjectSettings>(m_scrolled_panel);
    m_object_settings->Hide();
    params_sizer->Add(m_object_settings->get_sizer(), 0, wxEXPAND | wxTOP, margin_5);

    // Object Layers
    m_object_layers = std::make_unique<ObjectLayers>(m_scrolled_panel);
    m_object_layers->Hide();
    params_sizer->Add(m_object_layers->get_sizer(), 0, wxEXPAND | wxTOP, margin_5);

    // Info boxes
    m_object_info = new ObjectInfo(m_scrolled_panel);
    m_sliced_info = new SlicedInfo(m_scrolled_panel);

    int size_margin = wxGTK3 ? wxLEFT | wxRIGHT : wxLEFT;

    scrolled_sizer->Add(params_sizer, 1, wxEXPAND | size_margin, margin_5);
    scrolled_sizer->Add(m_object_info, 0, wxEXPAND | wxTOP | size_margin, margin_5);
    scrolled_sizer->Add(m_sliced_info, 0, wxEXPAND | wxTOP | size_margin, margin_5);

    // Buttons underneath the scrolled area

    // rescalable bitmap buttons "Send to printer" and "Remove device" 

    auto init_scalable_btn = [this](ScalableButton** btn, const std::string& icon_name, wxString tooltip = wxEmptyString)
    {
#ifdef __APPLE__
        int bmp_px_cnt = 16;
#else
        int bmp_px_cnt = 32;
#endif //__APPLE__
        ScalableBitmap bmp = ScalableBitmap(this, icon_name, bmp_px_cnt);
        *btn = new ScalableButton(this, wxID_ANY, bmp, "", wxBU_EXACTFIT);
        wxGetApp().SetWindowVariantForButton((*btn));

#ifdef _WIN32
        (*btn)->SetToolTip(tooltip);
        (*btn)->Bind(wxEVT_ENTER_WINDOW, [btn, this](wxMouseEvent& event) {
            const wxString tip = (*btn)->GetToolTip() ? (*btn)->GetToolTip()->GetTip() : wxString();
            show_rich_tip(tip, *btn);
            event.Skip();
        });
        (*btn)->Bind(wxEVT_LEAVE_WINDOW, [btn, this](wxMouseEvent& event) {
            hide_rich_tip(*btn);
            event.Skip();
        });
#else
        (*btn)->SetToolTip(tooltip);
#endif // _WIN32
        (*btn)->Hide();
    };

    init_scalable_btn(&m_btn_export_gcode, "export_gcode", _L("Export G-code") + " " + GUI::shortkey_ctrl_prefix() + "G");
	init_scalable_btn(&m_btn_export_gcode_removable, "export_to_sd", _L("Export to SD card / Flash drive") + " " + GUI::shortkey_ctrl_prefix() + "U");

    // Slice now, then Send to print once the plate is sliced. Export stays a small icon.

#ifdef _WIN32
    const int scaled_height = m_btn_export_gcode_removable->GetBitmapHeight();
#else
    const int scaled_height = m_btn_export_gcode_removable->GetBitmapHeight() + 4;
#endif
    auto init_btn = [this](wxButton **btn, wxString label, const int button_height) {
        *btn = new wxButton(this, wxID_ANY, label, wxDefaultPosition,
                            wxSize(-1, button_height), wxBU_EXACTFIT);
        wxGetApp().SetWindowVariantForButton((*btn));
        (*btn)->SetFont(wxGetApp().bold_font());
        wxGetApp().UpdateDarkUI((*btn), true);
    };

    init_btn(&m_btn_send_gcode, _L("Send to print"), scaled_height);
    init_btn(&m_btn_reslice     , _L("Slice now")            , scaled_height);
    init_btn(&m_btn_generate_supports, _L("Generate supports"), scaled_height);
    init_btn(&m_btn_connect_gcode, _L("Send to Connect"), scaled_height);
    m_btn_send_gcode->SetToolTip(_L("Send to print") + " " + GUI::shortkey_ctrl_prefix() + "Shift+G");
    m_btn_generate_supports->SetToolTip(_L("Generate supports for the loaded models so you can inspect and move them without slicing the whole print."));
    m_btn_generate_supports->Hide();

    enable_buttons(false);

    m_btns_sizer = new wxBoxSizer(wxVERTICAL);

    auto *complect_btns_sizer = new wxBoxSizer(wxHORIZONTAL);
    complect_btns_sizer->Add(m_btn_send_gcode, 1, wxEXPAND);
    complect_btns_sizer->Add(m_btn_connect_gcode, 1, wxEXPAND | wxLEFT, margin_5);
    complect_btns_sizer->Add(m_btn_export_gcode, 0, wxLEFT, margin_5);
	complect_btns_sizer->Add(m_btn_export_gcode_removable, 0, wxLEFT, margin_5);

    auto* slice_row = new wxBoxSizer(wxHORIZONTAL);
    slice_row->Add(m_btn_generate_supports, 1, wxEXPAND | wxRIGHT, margin_5);
    slice_row->Add(m_btn_reslice, 1, wxEXPAND);
    m_btns_sizer->Add(slice_row, 0, wxEXPAND | wxTOP, margin_5);
    m_btns_sizer->Add(complect_btns_sizer, 0, wxEXPAND | wxTOP, margin_5);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    m_content_sizer = new wxBoxSizer(wxVERTICAL);
    m_content_sizer->Add(m_scrolled_panel, 1, wxEXPAND);
    sizer->Add(m_content_sizer, 1, wxEXPAND);

    const int buttons_sizer_flags{
        wxEXPAND
        | wxLEFT
        | wxBOTTOM
#ifndef _WIN32
        | wxRIGHT
#endif // __linux__
    };
    sizer->Add(m_btns_sizer, 0, buttons_sizer_flags, margin_5);

    m_autoslicing_btns_sizer = new wxBoxSizer(wxHORIZONTAL);

	init_scalable_btn(&m_btn_export_all_gcode_removable, "export_to_sd", _L("Export all to SD card / Flash drive") + " " + GUI::shortkey_ctrl_prefix() + "U");
    init_btn(&m_btn_export_all_gcode, _L("Export all G-codes") + dots, scaled_height);
    init_btn(&m_btn_connect_gcode_all, _L("Send all to Connect"), scaled_height);

    m_autoslicing_btns_sizer->Add(m_btn_export_all_gcode, 1, wxEXPAND);
    m_autoslicing_btns_sizer->Add(m_btn_connect_gcode_all, 1, wxEXPAND | wxLEFT, margin_5);
	m_autoslicing_btns_sizer->Add(m_btn_export_all_gcode_removable, 0, wxLEFT, margin_5);

    m_autoslicing_btns_sizer->Show(false);

    sizer->Add(m_autoslicing_btns_sizer, 0, buttons_sizer_flags | wxTOP, margin_5);
    SetSizer(sizer);

    // Events
    m_btn_export_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_plater->export_gcode(false); });
    m_btn_generate_supports->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_plater->canvas3D()->get_gizmos_manager().is_in_editing_mode(true))
            return;
        m_plater->generate_supports();
    });
    m_btn_reslice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        if (m_plater->canvas3D()->get_gizmos_manager().is_in_editing_mode(true))
            return;

        const bool export_gcode_after_slicing = wxGetKeyState(WXK_SHIFT);
        if (export_gcode_after_slicing)
            m_plater->export_gcode(true);
        else
            m_plater->reslice();
        m_plater->select_view_3D("Preview");
    });

#ifdef _WIN32
    m_btn_reslice->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        show_rich_tip(m_reslice_btn_tooltip, m_btn_reslice);
        event.Skip();
    });
    m_btn_reslice->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        hide_rich_tip(m_btn_reslice);
        event.Skip();
    });
#endif // _WIN32

    m_btn_send_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_plater->send_gcode(); });
    m_btn_export_gcode_removable->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_plater->export_gcode(true); });
    m_btn_connect_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_plater->connect_gcode(); });

    this->Bind(wxEVT_COMBOBOX, &Sidebar::on_select_preset, this);

    m_btn_export_all_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        this->m_plater->export_all_gcodes(false);
    });

    m_btn_export_all_gcode_removable->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        this->m_plater->export_all_gcodes(true);
    });

    m_btn_connect_gcode_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        this->m_plater->connect_gcode_all();
    });

    install_pane_bar();
    if (GetSizer() && m_presets_panel)
        GetSizer()->Insert(0, m_presets_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin_5);
}

Sidebar::~Sidebar() {}

static constexpr int k_max_filament_slots = 16;

static void erase_config_index(ConfigOption* opt, size_t idx)
{
    if (opt == nullptr || !opt->is_vector())
        return;
    switch (opt->type()) {
    case coFloats: {
        auto* values = static_cast<ConfigOptionFloats*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    case coInts: {
        auto* values = static_cast<ConfigOptionInts*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    case coStrings: {
        auto* values = static_cast<ConfigOptionStrings*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    case coPercents: {
        auto* values = static_cast<ConfigOptionPercents*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    case coFloatsOrPercents: {
        auto* values = static_cast<ConfigOptionFloatsOrPercents*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    case coPoints: {
        auto* values = static_cast<ConfigOptionPoints*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    case coBools: {
        auto* values = static_cast<ConfigOptionBools*>(opt);
        if (values->values.size() > idx)
            values->values.erase(values->values.begin() + idx);
        break;
    }
    default:
        break;
    }
}

static void erase_extruder_slot(DynamicPrintConfig& cfg, size_t idx)
{
    for (const std::string& key : print_config_def.extruder_option_keys()) {
        if (key == "default_filament_profile")
            continue;
        erase_config_index(cfg.option(key, false), idx);
    }
}

static void shift_extruder_id(ModelConfig& cfg, int removed_1based, int old_count)
{
    if (!cfg.has("extruder"))
        return;
    const int extruder = cfg.opt_int("extruder");
    if (extruder == removed_1based)
        cfg.erase("extruder");
    else if (extruder > removed_1based && extruder <= old_count)
        cfg.set_key_value("extruder", new ConfigOptionInt(extruder - 1));
}

static void remap_model_after_filament_removed(Model& model, int removed_1based, int old_count)
{
    std::map<TriangleStateType, TriangleStateType> paint_remap;
    paint_remap[TriangleStateType(removed_1based)] = TriangleStateType::NONE;
    for (int extruder = removed_1based + 1; extruder <= old_count; ++extruder)
        paint_remap[TriangleStateType(extruder)] = TriangleStateType(extruder - 1);

    for (ModelObject* object : model.objects) {
        if (object == nullptr)
            continue;
        shift_extruder_id(object->config, removed_1based, old_count);
        for (auto& range : object->layer_config_ranges)
            shift_extruder_id(range.second, removed_1based, old_count);
        for (ModelVolume* volume : object->volumes) {
            if (volume == nullptr)
                continue;
            shift_extruder_id(volume->config, removed_1based, old_count);
            if (!volume->is_mm_painted())
                continue;
            TriangleSelector selector(volume->mesh());
            selector.deserialize(volume->mm_segmentation_facets.get_data(), false);
            selector.remap_states(paint_remap);
            volume->mm_segmentation_facets.set(selector);
        }
    }
}

void Sidebar::build_filament_header()
{
    const int gap = int(0.4 * wxGetApp().em_unit());
    auto* header = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxBoxSizer(wxHORIZONTAL);
    auto* label = new wxStaticText(m_presets_panel, wxID_ANY, _L("Filament"));
    label->SetFont(wxGetApp().bold_font());
    title->Add(label, 0, wxALIGN_CENTER_VERTICAL);

    m_btn_filament_gear = new ScalableButton(m_presets_panel, wxID_ANY, "cog");
    m_btn_filament_gear->SetToolTip(_L("Edit filament preset"));
    m_btn_filament_gear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_combos_filament.empty() && m_combos_filament[0])
            m_combos_filament[0]->show_edit_menu();
    });
    title->Add(m_btn_filament_gear, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap);

    m_btn_flushing = new wxButton(m_presets_panel, wxID_ANY, _L("Flushing volumes"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btn_flushing->SetToolTip(_L("Set how much filament is purged when the printer switches from one filament to another."));
    m_btn_flushing->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_flushing_volumes(); });
    title->AddStretchSpacer(1);
    title->Add(m_btn_flushing, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap);
    header->Add(title, 0, wxEXPAND | wxBOTTOM, gap);

    auto* tools = new wxBoxSizer(wxHORIZONTAL);
    m_btn_paint_filaments = new wxButton(m_presets_panel, wxID_ANY, _L("Paint"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btn_paint_filaments->SetToolTip(_L("Paint filaments onto the selected model."));
    m_btn_paint_filaments->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_filament_paint(false); });
    m_btn_layer_filaments = new wxButton(m_presets_panel, wxID_ANY, _L("Layers"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btn_layer_filaments->SetToolTip(_L("Paint a height band with a filament. In the sliced view, right-click the layer bar to switch filament at a layer. A height range in the model list can set a filament too."));
    m_btn_layer_filaments->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_filament_paint(true); });
    m_btn_add_filament = new wxButton(m_presets_panel, wxID_ANY, _L("Add"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btn_add_filament->SetToolTip(_L("Add another filament to this project."));
    m_btn_add_filament->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_filament_slot(); });
    m_btn_import_filaments = new wxButton(m_presets_panel, wxID_ANY, _L("From printer"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_btn_import_filaments->SetToolTip(_L("Add the filaments loaded on the selected printer to this project, including their colors."));
    m_btn_import_filaments->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { import_printer_filaments(); });
    tools->Add(m_btn_paint_filaments, 0, wxRIGHT, gap);
    tools->Add(m_btn_layer_filaments, 0, wxRIGHT, gap);
    tools->AddStretchSpacer(1);
    tools->Add(m_btn_import_filaments, 0, wxRIGHT, gap);
    tools->Add(m_btn_add_filament, 0);
    header->Add(tools, 0, wxEXPAND | wxBOTTOM, gap);

#ifdef _WIN32
    wxGetApp().UpdateDarkUI(m_btn_flushing, true);
    wxGetApp().UpdateDarkUI(m_btn_paint_filaments, true);
    wxGetApp().UpdateDarkUI(m_btn_layer_filaments, true);
    wxGetApp().UpdateDarkUI(m_btn_add_filament, true);
    wxGetApp().UpdateDarkUI(m_btn_import_filaments, true);
#endif
    m_filaments_sizer->Add(header, 0, wxEXPAND | wxBOTTOM, gap);
}

namespace {
struct ApplyGuard {
    bool& flag;
    explicit ApplyGuard(bool& flag) : flag(flag) { flag = true; }
    ~ApplyGuard() { flag = false; }
};
}

class ProcessScopeToggle : public wxPanel
{
public:
    ProcessScopeToggle(wxWindow* parent, std::function<void(bool objects)> on_select)
        : wxPanel(parent)
        , m_on_select(std::move(on_select))
    {
        SetBackgroundColour(wxGetApp().get_window_default_clr());
        SetFont(wxGetApp().normal_font());
        SetMinSize(wxSize(-1, std::max(28, wxGetApp().em_unit() * 2)));
        SetToolTip(_L("Global edits the defaults every object inherits. Objects edits the selected model, and those values override the global defaults."));
        Bind(wxEVT_PAINT, &ProcessScopeToggle::on_paint, this);
        Bind(wxEVT_LEFT_UP, &ProcessScopeToggle::on_click, this);
    }

    void set_objects(bool objects)
    {
        if (m_objects == objects)
            return;
        m_objects = objects;
        Refresh();
    }

private:
    bool m_objects{ false };
    std::function<void(bool)> m_on_select;

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        const wxColour background = GetBackgroundColour();
        dc.SetPen(background);
        dc.SetBrush(background);
        dc.DrawRectangle(0, 0, size.x, size.y);
        if (size.x < 4 || size.y < 4)
            return;

        const int radius = size.y / 2;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxColour(32, 32, 40));
        dc.DrawRoundedRectangle(wxRect(0, 0, size.x, size.y), radius);

        const int inset = 2;
        const int half = size.x / 2;
        const wxRect selected = m_objects
            ? wxRect(half, inset, size.x - half - inset, size.y - inset * 2)
            : wxRect(inset, inset, half - inset, size.y - inset * 2);
        dc.SetBrush(wxColour(254, 50, 173));
        dc.DrawRoundedRectangle(selected, selected.height / 2);

        dc.SetFont(GetFont());
        auto draw_label = [&](const wxString& text, const wxRect& rect, bool selected_label) {
            dc.SetTextForeground(selected_label ? *wxWHITE : wxColour(214, 214, 220));
            const wxSize extent = dc.GetTextExtent(text);
            dc.DrawText(text,
                rect.x + (rect.width - extent.x) / 2,
                rect.y + (rect.height - extent.y) / 2);
        };
        draw_label(_L("Global"), wxRect(0, 0, half, size.y), !m_objects);
        draw_label(_L("Objects"), wxRect(half, 0, size.x - half, size.y), m_objects);
    }

    void on_click(wxMouseEvent& event)
    {
        const bool objects = event.GetX() >= GetClientSize().x / 2;
        if (objects == m_objects)
            return;
        if (m_on_select)
            m_on_select(objects);
    }
};

class ProcessTabBar : public wxPanel
{
public:
    // scale 1 keeps the compact settings tabs. Models/Settings uses a larger scale.
    ProcessTabBar(wxWindow* parent, std::vector<wxString> labels, std::function<void(int)> on_select, double scale = 1.0)
        : wxPanel(parent)
        , m_labels(std::move(labels))
        , m_on_select(std::move(on_select))
        , m_scale(scale < 1.0 ? 1.0 : scale)
    {
        SetBackgroundColour(wxGetApp().get_window_default_clr());
        if (m_scale > 1.05) {
            wxFont font = wxGetApp().bold_font();
            const int base = std::max(1, wxGetApp().small_font().GetPointSize());
            font.SetPointSize(std::max(base + 1, int(base * m_scale + 0.5)));
            SetFont(font);
        } else {
            SetFont(wxGetApp().small_font());
        }
        m_pad_x = std::max(4, int(10 * m_scale + 0.5));
        m_pad_y = std::max(2, int(6 * m_scale + 0.5));
        m_extra = std::max(2, int(3 * m_scale + 0.5));
        m_pen = std::max(2, int(2 * m_scale + 0.5));
        m_inset = std::max(4, int(8 * m_scale + 0.5));
        m_radius = std::max(3, int(4 * m_scale + 0.5));
        Bind(wxEVT_PAINT, &ProcessTabBar::on_paint, this);
        Bind(wxEVT_LEFT_UP, &ProcessTabBar::on_click, this);
        Bind(wxEVT_MOTION, &ProcessTabBar::on_motion, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            if (m_hover != -1) {
                m_hover = -1;
                Refresh();
            }
            event.Skip();
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            const int before = GetMinSize().y;
            layout_tabs();
            if (GetMinSize().y != before && GetParent())
                GetParent()->Layout();
            event.Skip();
        });
        layout_tabs();
    }

    void set_active(int index)
    {
        if (index == m_active)
            return;
        m_active = index;
        Refresh();
    }

private:
    struct Cell { wxRect rect; };

    void layout_tabs()
    {
        wxClientDC dc(this);
        dc.SetFont(GetFont());
        int width = GetClientSize().GetWidth();
        if (width < 40) {
            wxWindow* parent = GetParent();
            width = parent && parent->GetClientSize().GetWidth() > 40
                ? parent->GetClientSize().GetWidth()
                : 28 * std::max(1, wxGetApp().em_unit());
        }
        m_cells.assign(m_labels.size(), {});
        int x = 0;
        int y = 0;
        int row_h = 0;
        for (size_t i = 0; i < m_labels.size(); ++i) {
            const wxSize text = dc.GetTextExtent(m_labels[i]);
            const int w = text.x + m_pad_x * 2;
            const int h = text.y + m_pad_y * 2 + m_extra;
            if (x > 0 && x + w > width) {
                x = 0;
                y += row_h;
                row_h = 0;
            }
            m_cells[i].rect = wxRect(x, y, w, h);
            x += w;
            row_h = std::max(row_h, h);
        }
        const int total = std::max(1, y + row_h + 3);
        if (GetMinSize().y != total)
            SetMinSize(wxSize(-1, total));
    }

    void on_paint(wxPaintEvent&)
    {
        if (m_cells.size() != m_labels.size())
            layout_tabs();
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        const wxColour background = GetBackgroundColour();
        dc.SetBrush(background);
        dc.SetPen(background);
        dc.DrawRectangle(0, 0, size.x, size.y);
        dc.SetFont(GetFont());
        const wxColour plain = wxGetApp().get_label_clr_default();
        const wxColour accent(254, 50, 173);
        for (size_t i = 0; i < m_labels.size(); ++i) {
            const wxRect& rect = m_cells[i].rect;
            const bool active = int(i) == m_active;
            const bool hover = int(i) == m_hover;
            if (active || hover) {
                const wxColour fill = active ? wxColour(47, 47, 56) : wxColour(60, 60, 68);
                dc.SetBrush(fill);
                dc.SetPen(wxPen(fill, 1));
                dc.DrawRoundedRectangle(rect, m_radius);
            }
            dc.SetTextForeground(active ? *wxWHITE : plain);
            const wxSize text = dc.GetTextExtent(m_labels[i]);
            const int text_y = m_scale > 1.05
                ? rect.y + (rect.height - text.y) / 2 - 1
                : rect.y + m_pad_y;
            dc.DrawText(m_labels[i], rect.x + (rect.width - text.x) / 2, text_y);
            if (active) {
                dc.SetPen(wxPen(accent, m_pen));
                const int y_line = rect.GetBottom() - (m_pen > 2 ? 2 : 1);
                dc.DrawLine(rect.x + m_inset, y_line, rect.GetRight() - m_inset, y_line);
            }
        }
        dc.SetPen(wxPen(wxColour(90, 90, 104), 1));
        dc.DrawLine(0, size.y - 1, size.x, size.y - 1);
    }

    void on_click(wxMouseEvent& event)
    {
        for (size_t i = 0; i < m_cells.size(); ++i) {
            if (m_cells[i].rect.Contains(event.GetPosition())) {
                if (m_on_select)
                    m_on_select(int(i));
                return;
            }
        }
    }

    void on_motion(wxMouseEvent& event)
    {
        int hover = -1;
        for (size_t i = 0; i < m_cells.size(); ++i) {
            if (m_cells[i].rect.Contains(event.GetPosition())) {
                hover = int(i);
                break;
            }
        }
        if (hover != m_hover) {
            m_hover = hover;
            Refresh();
        }
        event.Skip();
    }

    std::vector<wxString> m_labels;
    std::vector<Cell> m_cells;
    std::function<void(int)> m_on_select;
    double m_scale{ 1.0 };
    int m_pad_x{ 10 };
    int m_pad_y{ 6 };
    int m_extra{ 3 };
    int m_pen{ 2 };
    int m_inset{ 8 };
    int m_radius{ 4 };
    int m_active{ 0 };
    int m_hover{ -1 };
};

static bool config_flag(const char* key, bool fallback)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config || !config->has(key))
        return fallback;
    return config->get_bool(key);
}

void Sidebar::install_pane_bar()
{
    if (!wxGetApp().is_editor() || !GetSizer())
        return;

    m_pane_tabs = new ProcessTabBar(this, {
        _L("Models"), _L("Settings")
    }, [this](int index) { show_models_pane(index == 0); }, 1.4);

    GetSizer()->Insert(0, m_pane_tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, int(0.4 * wxGetApp().em_unit()));
    m_pane_tabs->set_active(m_show_models ? 0 : 1);
    refresh_layout_buttons();
}

void Sidebar::refresh_layout_buttons()
{
    // Models and Settings share one sidebar. Once models has its own pane, the switch is unused.
    const bool split = m_split || config_flag("sidebar_split", false);
    if (m_pane_tabs) {
        m_pane_tabs->Show(!split);
        if (GetSizer())
            GetSizer()->Show(m_pane_tabs, !split);
    }
    Layout();
}

void Sidebar::show_models_pane(bool models)
{
    if (m_split)
        return;
    m_show_models = models;
    if (m_pane_tabs)
        m_pane_tabs->set_active(models ? 0 : 1);
    apply_pane_visibility();
}

void Sidebar::apply_pane_visibility()
{
    if (!m_content_sizer)
        return;
    const bool models_here = m_models_panel && m_models_panel->GetParent() == this && !m_split;
    m_content_sizer->Show(m_scrolled_panel, !models_here || !m_show_models);
    if (m_models_panel && m_models_panel->GetParent() == this)
        m_content_sizer->Show(m_models_panel, models_here && m_show_models);
    Layout();
}

void Sidebar::adopt_models_panel(ModelsPanel* panel)
{
    m_models_panel = panel;
    if (!panel || !m_content_sizer)
        return;
    if (panel->GetParent() != this)
        panel->Reparent(this);
    panel->SetMinSize(wxSize(16 * wxGetApp().em_unit(), -1));
    m_content_sizer->Add(panel, 1, wxEXPAND);
    panel->Hide();
    apply_pane_visibility();
}

wxWindow* Sidebar::models_window() const
{
    return m_models_panel;
}

void Sidebar::dock_models()
{
    m_split = false;
    if (!m_models_panel || !m_content_sizer)
        return;
    if (m_models_panel->GetParent() != this)
        m_models_panel->Reparent(this);
    bool found = false;
    for (size_t i = 0; i < m_content_sizer->GetItemCount(); ++i) {
        if (m_content_sizer->GetItem(i)->GetWindow() == m_models_panel) {
            found = true;
            break;
        }
    }
    if (!found)
        m_content_sizer->Add(m_models_panel, 1, wxEXPAND);
    apply_pane_visibility();
    refresh_layout_buttons();
}

void Sidebar::undock_models()
{
    m_split = true;
    if (m_models_panel && m_content_sizer)
        m_content_sizer->Detach(m_models_panel);
    apply_pane_visibility();
    refresh_layout_buttons();
}

void Sidebar::build_process_panel()
{
    const int gap = int(0.4 * wxGetApp().em_unit());
    m_process_panel = new wxPanel(m_scrolled_panel);
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(m_process_panel);
#endif
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_process_panel->SetSizer(sizer);

    m_process_scope = new ProcessScopeToggle(m_process_panel, [this](bool objects) {
        set_process_objects_mode(objects);
    });
    sizer->Add(m_process_scope, 0, wxEXPAND | wxBOTTOM, gap);

    m_process_hint = new wxStaticText(m_process_panel, wxID_ANY, _L("Select one object to change its process settings."));
    m_process_hint->Hide();
    sizer->Add(m_process_hint, 0, wxEXPAND | wxBOTTOM, gap);
    m_process_panel->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        const int em = std::max(1, wxGetApp().em_unit());
        const int avail = m_process_panel->GetClientSize().GetWidth();
        if (avail < 40)
            return;
        if (m_process_hint) {
            const int wrap = std::max(8 * em, avail - 4);
            m_process_hint->SetMaxSize(wxSize(wrap, -1));
            m_process_hint->Wrap(wrap);
        }
        const int field_w = std::max(6 * em, avail - 13 * em);
        if (field_w == m_process_field_width)
            return;
        m_process_field_width = field_w;
        for (const std::shared_ptr<ConfigOptionsGroup>& group : m_process_groups)
            if (group)
                group->constrain_field_widths(field_w);
        for (const std::shared_ptr<ConfigOptionsGroup>& group : m_sla_groups)
            if (group)
                group->constrain_field_widths(field_w);
        m_process_panel->Layout();
    });

    m_process_tabs = new ProcessTabBar(m_process_panel, {
        _L("Quality"), _L("Strength"), _L("Speed"), _L("Support"), _L("Multimaterial"), _L("Others")
    }, [this](int index) { set_process_category(index); });
    sizer->Add(m_process_tabs, 0, wxEXPAND | wxBOTTOM, gap);

    m_sla_process_tabs = new ProcessTabBar(m_process_panel, {
        _L("Layers"), _L("Exposure"), _L("Supports"), _L("Pad"), _L("Advanced")
    }, [this](int index) { set_sla_category(index); });
    m_sla_process_tabs->Hide();
    sizer->Add(m_sla_process_tabs, 0, wxEXPAND | wxBOTTOM, gap);

    m_process_pages_sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_process_pages_sizer, 0, wxEXPAND);

    // A field with an empty label keeps the preset name. Advanced and Expert
    // fields stay hidden until the app is in that mode.
    struct ProcessOpt {
        std::string key;
        wxString label;
        ConfigOptionMode mode { comSimple };
    };
    struct ProcessSection {
        wxString title;
        std::vector<ProcessOpt> options;
    };
    auto add_page = [this](const std::vector<ProcessSection>& sections, const std::string& plugin_slot = {}) {
        auto* page = new wxPanel(m_process_panel);
#ifdef _WIN32
        wxGetApp().UpdateDarkUI(page);
#endif
        auto* page_sizer = new wxBoxSizer(wxVERTICAL);
        if (!plugin_slot.empty()) {
            ensure_builtin_editor_plugins();
            for (const EditorPlugin& plugin : editor_plugins()) {
                if (plugin.slot != plugin_slot)
                    continue;
                auto* plugin_btn = new wxButton(page, wxID_ANY, from_u8(plugin.title), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                plugin_btn->SetToolTip(format_wxstr(_L("%1% by %2%. %3%"), from_u8(plugin.title), from_u8(plugin.credit), from_u8(plugin.summary)));
#ifdef _WIN32
                wxGetApp().UpdateDarkUI(plugin_btn, true);
#endif
                plugin_btn->Bind(wxEVT_BUTTON, [id = plugin.id](wxCommandEvent&) { open_editor_plugin(id); });
                page_sizer->Add(plugin_btn, 0, wxBOTTOM, int(0.4 * wxGetApp().em_unit()));
            }
        }

        for (const ProcessSection& section : sections) {
            auto group = std::make_shared<ConfigOptionsGroup>(page, section.title);
            group->set_config(&wxGetApp().preset_bundle->prints.get_edited_preset().config);
            group->label_width = 12;
            group->on_change = [this](const t_config_option_key& opt_id, const boost::any&) {
                on_process_option(opt_id);
            };
            for (const ProcessOpt& spec : section.options) {
                Option option = group->get_option(spec.key);
                if (!spec.label.empty())
                    option.opt.label = into_u8(spec.label);
                option.opt.mode = spec.mode;
                option.opt.full_width = true;
                group->append_single_option_line(option);
            }
            group->activate();
            page_sizer->Add(group->sizer, 0, wxEXPAND | wxTOP, section.title.empty() ? 0 : int(0.6 * wxGetApp().em_unit()));
            m_process_groups.push_back(std::move(group));
        }
        page->SetSizer(page_sizer);
        m_process_pages_sizer->Add(page, 0, wxEXPAND);
        m_process_pages.push_back(page);
    };

    add_page({
        { "", {
            { "layer_height", L("Layer height") },
            { "first_layer_height", L("First layer height") },
            { "perimeters", L("Wall loops") },
            { "top_solid_layers", L("Top shell layers") },
            { "bottom_solid_layers", L("Bottom shell layers") },
            { "seam_position", L("Seam position") }
        }},
        { L("Walls"), {
            { "spiral_vase", L("Spiral vase"), comAdvanced },
            { "extra_perimeters", L("Extra walls"), comAdvanced },
            { "extra_perimeters_on_overhangs", L("Extra walls on overhangs"), comAdvanced },
            { "thin_walls", L("Thin walls"), comAdvanced },
            { "overhangs", L("Detect bridging"), comAdvanced },
            { "thick_bridges", L("Thick bridges"), comAdvanced },
            { "external_perimeters_first", L("Outer wall first"), comAdvanced },
            { "gap_fill_enabled", L("Fill gaps"), comAdvanced },
            { "top_solid_min_thickness", L("Min top thickness"), comAdvanced },
            { "bottom_solid_min_thickness", L("Min bottom thickness"), comAdvanced },
            { "ensure_vertical_shell_thickness", L("Vertical shell thickness"), comAdvanced }
        }},
        { L("Seams"), {
            { "seam_gap_distance", L("Seam gap"), comExpert },
            { "staggered_inner_seams", L("Stagger inner seams"), comExpert },
            { "scarf_seam_placement", L("Scarf seam"), comExpert },
            { "avoid_crossing_perimeters", L("Avoid crossing walls"), comExpert },
            { "avoid_crossing_perimeters_max_detour", L("Max detour"), comExpert },
            { "avoid_crossing_curled_overhangs", L("Avoid curled overhangs"), comExpert },
            { "perimeter_generator", L("Perimeter generator"), comExpert },
            { "top_one_perimeter_type", L("One top perimeter"), comExpert },
            { "only_one_perimeter_first_layer", L("One perimeter on first layer"), comExpert },
            { "elefant_foot_compensation", L("Elephant foot"), comExpert },
            { "xy_size_compensation", L("XY compensation"), comExpert }
        }},
        { L("Line width"), {
            { "extrusion_width", L("Default width"), comExpert },
            { "first_layer_extrusion_width", L("First layer width"), comExpert },
            { "perimeter_extrusion_width", L("Wall width"), comExpert },
            { "external_perimeter_extrusion_width", L("Outer wall width"), comExpert },
            { "infill_extrusion_width", L("Infill width"), comExpert },
            { "solid_infill_extrusion_width", L("Solid infill width"), comExpert },
            { "top_infill_extrusion_width", L("Top infill width"), comExpert }
        }}
    });
    add_page({
        { "", {
            { "fill_density", L("Sparse infill density") },
            { "fill_pattern", L("Sparse infill pattern") },
            { "top_fill_pattern", L("Top fill pattern") },
            { "bottom_fill_pattern", L("Bottom fill pattern") },
            { "infill_every_layers", L("Infill combination") },
            { "solid_infill_every_layers", L("Solid infill every") }
        }},
        { L("Infill"), {
            { "fill_angle", L("Fill angle"), comAdvanced },
            { "infill_anchor", L("Infill anchor"), comAdvanced },
            { "infill_anchor_max", L("Infill anchor max"), comAdvanced },
            { "solid_infill_below_area", L("Solid infill threshold"), comAdvanced },
            { "infill_overlap", L("Infill overlap"), comAdvanced },
            { "bridge_angle", L("Bridge angle"), comAdvanced },
            { "bridge_flow_ratio", L("Bridge flow"), comAdvanced },
            { "automatic_infill_combination", L("Combine infill"), comExpert },
            { "automatic_infill_combination_max_layer_height", L("Combine infill max height"), comExpert },
            { "infill_first", L("Infill first"), comExpert },
            { "only_retract_when_crossing_perimeters", L("Retract when crossing walls"), comExpert }
        }}
    });
    add_page({
        { "", {
            { "perimeter_speed", L("Wall speed") },
            { "external_perimeter_speed", L("Outer wall") },
            { "infill_speed", L("Sparse infill") },
            { "solid_infill_speed", L("Solid infill") },
            { "travel_speed", L("Travel") },
            { "first_layer_speed", L("First layer") }
        }},
        { L("More speeds"), {
            { "small_perimeter_speed", L("Small walls"), comAdvanced },
            { "top_solid_infill_speed", L("Top solid infill"), comAdvanced },
            { "support_material_speed", L("Support"), comAdvanced },
            { "support_material_interface_speed", L("Support interface"), comAdvanced },
            { "bridge_speed", L("Bridges"), comAdvanced },
            { "over_bridge_speed", L("Over bridges"), comAdvanced },
            { "gap_fill_speed", L("Gap fill"), comAdvanced },
            { "ironing_speed", L("Ironing"), comAdvanced },
            { "first_layer_infill_speed", L("First layer infill"), comAdvanced },
            { "first_layer_speed_over_raft", L("First layer over raft"), comAdvanced },
            { "travel_speed_z", L("Z travel"), comAdvanced }
        }},
        { L("Overhangs"), {
            { "enable_dynamic_overhang_speeds", L("Dynamic overhang speed"), comExpert },
            { "overhang_speed_0", L("0% overlap"), comExpert },
            { "overhang_speed_1", L("25% overlap"), comExpert },
            { "overhang_speed_2", L("50% overlap"), comExpert },
            { "overhang_speed_3", L("75% overlap"), comExpert }
        }},
        { L("Acceleration"), {
            { "default_acceleration", L("Default"), comExpert },
            { "perimeter_acceleration", L("Walls"), comExpert },
            { "external_perimeter_acceleration", L("Outer wall"), comExpert },
            { "infill_acceleration", L("Sparse infill"), comExpert },
            { "solid_infill_acceleration", L("Solid infill"), comExpert },
            { "top_solid_infill_acceleration", L("Top solid infill"), comExpert },
            { "bridge_acceleration", L("Bridges"), comExpert },
            { "first_layer_acceleration", L("First layer"), comExpert },
            { "travel_acceleration", L("Travel"), comExpert }
        }},
        { L("Flow limit"), {
            { "max_volumetric_speed", L("Max volumetric speed"), comExpert }
        }}
    });
    add_page({
        { "", {
            { "slice_bed_as_one_object", L("Treat build plate as one object") },
            { "support_material", L("Enable support") },
            { "support_material_auto", L("Automatic supports") },
            { "support_material_style", L("Type") },
            { "support_material_threshold", L("Threshold angle") },
            { "raft_first_layer_density", L("First layer density") },
            { "raft_first_layer_expansion", L("First layer expansion") },
            { "support_material_buildplate_only", L("On build plate only") },
            { "dont_support_bridges", L("Don't support bridges") },
            { "raft_layers", L("Raft layers") },
            { "support_material_extruder", L("Support/raft base") },
            { "support_material_interface_extruder", L("Support/raft interface") },
            { "support_material_contact_distance", L("Top Z distance") },
            { "support_material_bottom_contact_distance", L("Bottom Z distance") },
            { "support_material_pattern", L("Base pattern") },
            { "support_material_spacing", L("Base pattern spacing") },
            { "support_material_angle", L("Pattern angle") }
        }},
        { L("More support"), {
            { "support_material_enforce_layers", L("Enforce first layers"), comAdvanced },
            { "raft_contact_distance", L("Raft contact distance"), comAdvanced },
            { "raft_expansion", L("Raft expansion"), comAdvanced },
            { "support_material_xy_spacing", L("XY separation"), comAdvanced },
            { "support_material_interface_layers", L("Top interface layers"), comAdvanced },
            { "support_material_bottom_interface_layers", L("Bottom interface layers"), comAdvanced },
            { "support_material_interface_pattern", L("Interface pattern"), comAdvanced },
            { "support_material_interface_spacing", L("Interface spacing"), comAdvanced },
            { "support_material_interface_contact_loops", L("Interface loops"), comAdvanced },
            { "support_material_with_sheath", L("Sheath around support"), comAdvanced },
            { "support_material_closing_radius", L("Closing radius"), comAdvanced },
            { "support_material_synchronize_layers", L("Synchronize layers"), comAdvanced }
        }},
        { L("Organic supports"), {
            { "support_tree_angle", L("Branch angle"), comExpert },
            { "support_tree_angle_slow", L("Slow branch angle"), comExpert },
            { "support_tree_branch_diameter", L("Branch diameter"), comExpert },
            { "support_tree_tip_diameter", L("Tip diameter"), comExpert },
            { "support_tree_branch_distance", L("Branch distance"), comExpert },
            { "support_tree_top_rate", L("Top rate"), comExpert }
        }}
    }, "supports");
    add_page({
        { "", {
            { "wipe_tower", L("Prime tower") },
            { "wipe_tower_width", L("Prime tower width") },
            { "interface_shells", L("Interface shells") },
            { "ooze_prevention", L("Ooze prevention") }
        }},
        { L("Extruders"), {
            { "perimeter_extruder", L("Walls"), comAdvanced },
            { "infill_extruder", L("Infill"), comAdvanced },
            { "solid_infill_extruder", L("Solid infill"), comAdvanced },
            { "wipe_tower_extruder", L("Prime tower"), comAdvanced },
            { "standby_temperature_delta", L("Standby temperature"), comAdvanced }
        }},
        { L("Prime tower"), {
            { "wipe_tower_brim_width", L("Brim width"), comAdvanced },
            { "wipe_tower_cone_angle", L("Cone angle"), comAdvanced },
            { "wipe_tower_extra_spacing", L("Extra spacing"), comAdvanced },
            { "wipe_tower_extra_flow", L("Extra flow"), comAdvanced },
            { "wipe_tower_bridging", L("Bridging"), comAdvanced },
            { "wipe_tower_no_sparse_layers", L("No sparse layers"), comExpert }
        }},
        { L("Interlocking"), {
            { "mmu_segmented_region_max_width", L("Segmented region width"), comExpert },
            { "mmu_segmented_region_interlocking_depth", L("Segmented interlocking depth"), comExpert },
            { "interlocking_beam", L("Interlocking beam"), comExpert },
            { "interlocking_depth", L("Interlocking depth"), comExpert }
        }}
    });
    add_page({
        { "", {
            { "brim_type", L("Brim type") },
            { "brim_width", L("Brim width") },
            { "brim_separation", L("Brim separation") },
            { "skirts", L("Skirt loops") },
            { "skirt_distance", L("Skirt distance") },
            { "ironing", L("Ironing") },
            { "fuzzy_skin", L("Fuzzy skin") }
        }},
        { L("Skirt, ironing, and surface"), {
            { "skirt_height", L("Skirt height"), comAdvanced },
            { "draft_shield", L("Draft shield"), comAdvanced },
            { "min_skirt_length", L("Min skirt length"), comAdvanced },
            { "ironing_type", L("Ironing type"), comAdvanced },
            { "ironing_flowrate", L("Ironing flow"), comAdvanced },
            { "ironing_spacing", L("Ironing spacing"), comAdvanced },
            { "fuzzy_skin_thickness", L("Fuzzy skin thickness"), comAdvanced },
            { "fuzzy_skin_point_dist", L("Fuzzy skin point distance"), comAdvanced },
            { "complete_objects", L("Complete individual objects"), comExpert }
        }}
    }, "surface");

    auto add_sla_page = [this](Preset::Type type, const std::vector<std::pair<std::string, wxString>>& options) {
        auto* page = new wxPanel(m_process_panel);
#ifdef _WIN32
        wxGetApp().UpdateDarkUI(page);
#endif
        auto* page_sizer = new wxBoxSizer(wxVERTICAL);
        auto group = std::make_shared<ConfigOptionsGroup>(page, "");
        DynamicPrintConfig* cfg = type == Preset::TYPE_SLA_MATERIAL
            ? &wxGetApp().preset_bundle->sla_materials.get_edited_preset().config
            : &wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
        group->set_config(cfg);
        group->label_width = 12;
        group->on_change = [this, type](const t_config_option_key& opt_id, const boost::any&) {
            on_sla_option(type, opt_id);
        };
        for (const auto& spec : options) {
            Option option = group->get_option(spec.first);
            if (!spec.second.empty())
                option.opt.label = into_u8(spec.second);
            option.opt.mode = comSimple;
            option.opt.full_width = true;
            group->append_single_option_line(option);
        }
        group->activate();
        page_sizer->Add(group->sizer, 0, wxEXPAND);
        page->SetSizer(page_sizer);
        page->Hide();
        m_process_pages_sizer->Add(page, 0, wxEXPAND);
        m_process_pages_sizer->Show(page, false);
        m_sla_pages.push_back(page);
        m_sla_groups.push_back(std::move(group));
    };

    add_sla_page(Preset::TYPE_SLA_PRINT, {
        { "layer_height", L("Layer height") },
        { "faded_layers", L("Faded layers") },
        { "bottom_layers", L("Bottom layers") }
    });
    add_sla_page(Preset::TYPE_SLA_MATERIAL, {
        { "exposure_time", L("Exposure") },
        { "initial_exposure_time", L("Bottom exposure") }
    });
    add_sla_page(Preset::TYPE_SLA_PRINT, {
        { "supports_enable", L("Enable supports") },
        { "support_tree_type", L("Type") },
        { "support_enforcers_only", L("Support enforcers only") },
        { "support_points_density_relative", L("Support density") },
        { "support_head_front_diameter", L("Head diameter") },
        { "support_head_penetration", L("Head penetration") },
        { "support_head_width", L("Head width") },
        { "support_pillar_diameter", L("Pillar diameter") },
        { "support_small_pillar_diameter_percent", L("Small pillar diameter") },
        { "support_max_bridges_on_pillar", L("Max bridges on pillar") },
        { "support_pillar_connection_mode", L("Pillar connection") },
        { "support_buildplate_only", L("On build plate only") },
        { "support_pillar_widening_factor", L("Pillar widening") },
        { "support_max_weight_on_model", L("Max weight on model") },
        { "support_base_diameter", L("Base diameter") },
        { "support_base_height", L("Base height") },
        { "support_base_safety_distance", L("Base safety distance") },
        { "support_object_elevation", L("Object elevation") },
        { "support_critical_angle", L("Critical angle") },
        { "support_max_bridge_length", L("Max bridge length") },
        { "support_max_pillar_link_distance", L("Max pillar link distance") },
        { "branchingsupport_head_front_diameter", L("Branching head diameter") },
        { "branchingsupport_head_penetration", L("Branching head penetration") },
        { "branchingsupport_head_width", L("Branching head width") },
        { "branchingsupport_pillar_diameter", L("Branching pillar diameter") },
        { "branchingsupport_small_pillar_diameter_percent", L("Branching small pillar") },
        { "branchingsupport_base_diameter", L("Branching base diameter") },
        { "branchingsupport_base_height", L("Branching base height") },
        { "branchingsupport_object_elevation", L("Branching object elevation") },
        { "branchingsupport_critical_angle", L("Branching critical angle") },
        { "branchingsupport_max_bridge_length", L("Branching max bridge length") },
        { "branchingsupport_max_pillar_link_distance", L("Branching pillar link distance") }
    });
    add_sla_page(Preset::TYPE_SLA_PRINT, {
        { "pad_enable", L("Enable pad") },
        { "pad_wall_thickness", L("Wall thickness") },
        { "pad_wall_height", L("Wall height") },
        { "pad_brim_size", L("Brim size") },
        { "pad_max_merge_distance", L("Max merge distance") },
        { "pad_wall_slope", L("Wall slope") },
        { "pad_around_object", L("Pad around object") },
        { "pad_around_object_everywhere", L("Pad everywhere") },
        { "pad_object_gap", L("Object gap") },
        { "pad_object_connector_stride", L("Connector stride") },
        { "pad_object_connector_width", L("Connector width") },
        { "pad_object_connector_penetration", L("Connector penetration") }
    });
    add_sla_page(Preset::TYPE_SLA_PRINT, {
        { "slice_closing_radius", L("Slice closing radius") },
        { "slicing_mode", L("Slicing mode") },
        { "output_filename_format", L("Output filename") }
    });

    set_process_category(0);
    set_process_objects_mode(false);
    m_process_field_width = 0;
    m_process_panel->SendSizeEvent();

    Bind(wxEVT_IDLE, [this](wxIdleEvent& event) {
        event.Skip();
        if (!m_process_objects || !m_process_panel || !m_process_panel->IsShown())
            return;
        const int idx = selected_object_idx();
        if (idx == m_process_object_idx)
            return;
        m_process_object_idx = idx;
        reload_process_settings();
    });
}

void Sidebar::set_process_category(int index)
{
    if (m_process_pages.empty() || !m_process_pages_sizer)
        return;
    if (index < 0 || index >= int(m_process_pages.size()))
        index = 0;
    m_process_category = index;
    const bool need_object = m_process_objects && selected_process_object() == nullptr;
    for (int i = 0; i < int(m_process_pages.size()); ++i)
        m_process_pages_sizer->Show(m_process_pages[i], !need_object && i == m_process_category);
    for (wxPanel* page : m_sla_pages)
        m_process_pages_sizer->Show(page, false);
    update_process_chrome();
    if (m_process_panel)
        m_process_panel->Layout();
    if (m_scrolled_panel)
        m_scrolled_panel->Layout();
}

void Sidebar::update_process_tab_visibility(bool show_sla)
{
    if (m_process_scope)
        m_process_scope->Show(!show_sla);
    if (m_process_tabs)
        m_process_tabs->Show(!show_sla);
    if (m_sla_process_tabs) {
        m_sla_process_tabs->Show(show_sla);
        if (show_sla)
            m_sla_process_tabs->set_active(m_sla_category);
    }
}

void Sidebar::set_sla_category(int index)
{
    if (m_sla_pages.empty() || !m_process_pages_sizer)
        return;
    if (index < 0 || index >= int(m_sla_pages.size()))
        index = 0;
    m_sla_category = index;
    for (wxPanel* page : m_process_pages)
        m_process_pages_sizer->Show(page, false);
    for (int i = 0; i < int(m_sla_pages.size()); ++i)
        m_process_pages_sizer->Show(m_sla_pages[i], i == m_sla_category);
    if (m_sla_process_tabs)
        m_sla_process_tabs->set_active(m_sla_category);
    if (m_process_hint)
        m_process_hint->Hide();
    if (m_process_panel)
        m_process_panel->Layout();
    if (m_scrolled_panel)
        m_scrolled_panel->Layout();
}

void Sidebar::on_sla_option(Preset::Type type, const std::string& opt_id)
{
    if (m_sla_applying || opt_id.empty())
        return;
    {
        ApplyGuard guard(m_sla_applying);
        if (Tab* tab = wxGetApp().get_tab(type)) {
            tab->update_dirty();
            tab->update();
        } else if (m_plater) {
            m_plater->schedule_background_process();
        }
    }
    reload_sla_groups();
}

void Sidebar::reload_sla_groups()
{
    if (m_sla_applying)
        return;
    ApplyGuard guard(m_sla_applying);
    for (const std::shared_ptr<ConfigOptionsGroup>& group : m_sla_groups) {
        if (group)
            group->reload_config();
    }
}

void Sidebar::set_process_objects_mode(bool objects)
{
    if (m_process_objects == objects && (!objects || m_process_global))
        return;
    m_process_objects = objects;
    m_process_object_idx = -2;
    if (objects)
        m_process_global.reset(new DynamicPrintConfig(wxGetApp().preset_bundle->prints.get_edited_preset().config));
    else
        m_process_global.reset();
    if (m_process_scope)
        m_process_scope->set_objects(objects);
    update_process_chrome();
    reload_process_settings();
}

void Sidebar::update_process_chrome()
{
    if (m_process_tabs)
        m_process_tabs->set_active(m_process_category);
}

int Sidebar::selected_object_idx() const
{
    if (!m_plater)
        return -1;
    GLCanvas3D* canvas = m_plater->canvas3D();
    if (!canvas)
        return -1;
    return canvas->get_selection().get_object_idx();
}

ModelObject* Sidebar::selected_process_object() const
{
    const int idx = selected_object_idx();
    if (!m_plater || idx < 0)
        return nullptr;
    Model& model = m_plater->model();
    if (idx >= int(model.objects.size()))
        return nullptr;
    return model.objects[idx];
}

void Sidebar::reload_process_settings()
{
    if (m_process_applying || m_process_groups.empty())
        return;
    ApplyGuard guard(m_process_applying);

    DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (m_process_objects)
        m_process_global.reset(new DynamicPrintConfig(print_cfg));

    ModelObject* object = m_process_objects ? selected_process_object() : nullptr;
    const bool sla = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA;
    const bool need_object = !sla && m_process_objects && object == nullptr;
    if (m_process_hint)
        m_process_hint->Show(need_object);
    if (m_process_pages_sizer) {
        for (int i = 0; i < int(m_process_pages.size()); ++i)
            m_process_pages_sizer->Show(m_process_pages[i], !sla && !need_object && i == m_process_category);
        for (int i = 0; i < int(m_sla_pages.size()); ++i)
            m_process_pages_sizer->Show(m_sla_pages[i], sla && i == m_sla_category);
    }
    reload_sla_groups();

    if (!need_object) {
        for (const std::shared_ptr<ConfigOptionsGroup>& group : m_process_groups) {
            if (!group)
                continue;
            if (!object) {
                group->reload_config();
                continue;
            }
            for (const auto& item : group->opt_map()) {
                const std::string& opt_id = item.first;
                const std::string& opt_key = item.second.first;
                const int opt_index = item.second.second;
                const DynamicPrintConfig& source = object->config.has(opt_key) ? object->config.get() : print_cfg;
                group->set_value(opt_id, group->get_config_value(source, opt_key, opt_index));
            }
        }
    }

    if (m_process_objects)
        m_process_object_idx = selected_object_idx();
    if (m_process_panel)
        m_process_panel->Layout();
}

void Sidebar::on_process_option(const std::string& opt_id)
{
    if (m_process_applying)
        return;

    std::string key = opt_id;
    if (const auto hash = key.find('#'); hash != std::string::npos)
        key.resize(hash);
    bool prompt_objects = false;

    {
        ApplyGuard guard(m_process_applying);

        DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (!print_cfg.has(key))
            return;

        const bool enable_supports = key == "support_material" && print_cfg.opt_bool("support_material");
        if (enable_supports)
            print_cfg.set_key_value("support_material_auto", new ConfigOptionBool(true));

        if (m_process_objects) {
            std::unique_ptr<ConfigOption> written(print_cfg.option(key)->clone());
            std::unique_ptr<ConfigOption> written_auto;
            if (enable_supports && print_cfg.option("support_material_auto"))
                written_auto.reset(print_cfg.option("support_material_auto")->clone());

            if (m_process_global) {
                auto restore = [&](const std::string& name) {
                    if (m_process_global->has(name))
                        print_cfg.set_key_value(name, m_process_global->option(name)->clone());
                };
                restore(key);
                if (enable_supports)
                    restore("support_material_auto");
            }

            ModelObject* object = selected_process_object();
            if (object) {
                if (m_plater)
                    m_plater->take_snapshot(_L("Object process settings"));
                object->config.set_key_value(key, written.release());
                if (written_auto)
                    object->config.set_key_value("support_material_auto", written_auto.release());
                if (m_object_list)
                    m_object_list->update_and_show_object_settings_item();
                if (m_plater)
                    m_plater->schedule_background_process();
            }
        } else if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT)) {
            tab->update_dirty();
            tab->update();
            prompt_objects = true;
        } else if (m_plater) {
            m_plater->schedule_background_process();
        }
    }

    if (prompt_objects && !key.empty()) {
        offer_global_change_to_objects(key);
        if (key == "support_material") {
            DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            if (print_cfg.opt_bool("support_material"))
                offer_global_change_to_objects("support_material_auto");
        }
    }

    reload_process_settings();
}

void Sidebar::offer_global_change_to_objects(const std::string& key)
{
    if (m_process_applying || m_process_prompting || key.empty() || m_plater == nullptr)
        return;
    DynamicPrintConfig& print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (!print_cfg.has(key))
        return;
    const ConfigOption* updated = print_cfg.option(key);
    if (updated == nullptr)
        return;
    ApplyGuard prompting(m_process_prompting);

    auto display_value = [](const ConfigOption* opt) {
        if (const auto* boolean = dynamic_cast<const ConfigOptionBool*>(opt))
            return std::string(boolean->value ? "yes" : "no");
        return opt->serialize();
    };

    struct Row {
        ModelObject* object;
        std::string  label;
    };
    std::vector<Row> rows;
    for (ModelObject* object : m_plater->model().objects) {
        if (object == nullptr || !object->config.has(key))
            continue;
        const ConfigOption* current = object->config.option(key);
        if (current == nullptr || *current == *updated)
            continue;
        const std::string name = object->name.empty() ? std::string("Object") : object->name;
        rows.push_back({ object, name + "  (" + display_value(current) + ")" });
    }
    if (rows.empty())
        return;

    wxString setting(from_u8(key));
    if (const ConfigOptionDef* def = print_config_def.get(key))
        setting = _(def->label);

    wxDialog dialog(m_process_panel ? static_cast<wxWindow*>(m_process_panel) : wxGetApp().mainframe,
        wxID_ANY, _L("Update object settings"));
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(&dialog);
#endif
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* text = new wxStaticText(&dialog, wxID_ANY, format_wxstr(
        _L("Global \"%1%\" is now %2%.\nThese objects keep their own value. Update all of them, or only the ones you check."),
        setting, from_u8(display_value(updated))));
    text->Wrap(28 * wxGetApp().em_unit());
    sizer->Add(text, 0, wxALL, 12);

    wxArrayString labels;
    for (const Row& row : rows)
        labels.Add(from_u8(row.label));
    auto* list = new wxCheckListBox(&dialog, wxID_ANY, wxDefaultPosition,
        wxSize(28 * wxGetApp().em_unit(), std::min(240, 24 * int(rows.size()) + 12)));
    list->InsertItems(labels, 0);
    for (unsigned i = 0; i < list->GetCount(); ++i)
        list->Check(i, true);
    sizer->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* all = new wxButton(&dialog, wxID_ANY, _L("All objects"));
    auto* chosen = new wxButton(&dialog, wxID_OK, _L("Selected objects"));
    auto* keep = new wxButton(&dialog, wxID_CANCEL, _L("Keep object values"));
    chosen->SetDefault();
    buttons->Add(all, 0, wxRIGHT, 8);
    buttons->AddStretchSpacer(1);
    buttons->Add(chosen, 0, wxRIGHT, 8);
    buttons->Add(keep, 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 12);
    dialog.SetSizerAndFit(sizer);
    dialog.CentreOnParent();

    all->Bind(wxEVT_BUTTON, [&dialog, list](wxCommandEvent&) {
        for (unsigned i = 0; i < list->GetCount(); ++i)
            list->Check(i, true);
        dialog.EndModal(wxID_OK);
    });
    if (dialog.ShowModal() != wxID_OK)
        return;

    bool any = false;
    for (unsigned i = 0; i < rows.size() && i < list->GetCount(); ++i)
        any = any || list->IsChecked(i);
    if (!any)
        return;

    m_plater->take_snapshot(_L("Apply global process setting to objects"));
    for (unsigned i = 0; i < rows.size() && i < list->GetCount(); ++i) {
        if (!list->IsChecked(i))
            continue;
        rows[i].object->config.set_key_value(key, updated->clone());
    }
    if (m_object_list)
        m_object_list->update_and_show_object_settings_item();
    m_plater->schedule_background_process();
}

void Sidebar::sync_plate_type()
{
    if (!m_plate_type || !wxGetApp().app_config)
        return;
    static const char* ids[] = {
        "Textured PEI Plate", "Smooth PEI Plate", "Cool Plate", "Engineering Plate", "High Temp Plate"
    };
    const std::string saved = wxGetApp().app_config->get("plate_type");
    int idx = 0;
    for (int i = 0; i < int(sizeof(ids) / sizeof(ids[0])); ++i)
        if (saved == ids[i])
            idx = i;
    m_plate_syncing = true;
    m_plate_type->SetSelection(idx);
    m_plate_syncing = false;
}

void Sidebar::make_filament_row(PlaterPresetComboBox* combo, int extr_idx)
{
    const int em = wxGetApp().em_unit();
    const int gap = int(0.3 * em);
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    FilamentSlotWidgets slot;
    slot.index = new wxStaticText(m_presets_panel, wxID_ANY, wxString::Format("%d", extr_idx + 1));
    slot.index->SetMinSize(wxSize(int(1.4 * em), -1));
    const wxSize swatch_size(std::max(16, int(1.4 * em)), std::max(16, int(1.4 * em)));
    slot.swatch = new wxPanel(m_presets_panel, wxID_ANY, wxDefaultPosition, swatch_size);
    slot.swatch->SetMinSize(swatch_size);
    slot.swatch->SetToolTip(_L("Filament color. Click to change it."));
    slot.swatch->Bind(wxEVT_PAINT, [swatch = slot.swatch](wxPaintEvent&) {
        wxPaintDC dc(swatch);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(swatch->GetBackgroundColour()));
        dc.DrawRectangle(swatch->GetClientRect());
    });
    slot.swatch->Bind(wxEVT_LEFT_UP, [combo](wxMouseEvent&) { combo->change_extruder_color(); });
    slot.menu = new wxButton(m_presets_panel, wxID_ANY, L"...", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    slot.menu->SetToolTip(_L("Filament actions"));
    slot.menu->Bind(wxEVT_BUTTON, [this, extr_idx, btn = slot.menu](wxCommandEvent&) {
        show_filament_slot_menu(extr_idx, btn);
    });
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(slot.menu, true);
#endif

    row->Add(slot.index, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    row->Add(slot.swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    row->Add(combo, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);
    row->Add(slot.menu, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap);
    m_filaments_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 1);
    m_filament_slots.push_back(slot);
    update_filament_badges();
}

std::string Sidebar::filament_slot_color(size_t idx) const
{
    if (const Plater* plater = wxGetApp().plater()) {
        if (const DynamicPrintConfig* cfg = plater->config()) {
            if (const auto* colors = cfg->option<ConfigOptionStrings>("filament_colour")) {
                if (idx < colors->values.size() && !colors->values[idx].empty()) {
                    const std::string color = neutral_filament_color(colors->values[idx]);
                    return color.empty() || color.front() == '#' ? color : "#" + color;
                }
            }
        }
    }
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return {};
    if (idx < bundle->extruders_filaments.size()) {
        if (const Preset* preset = bundle->extruders_filaments[idx].get_selected_preset()) {
            const std::string color = neutral_filament_color(preset->config.opt_string("filament_colour", 0));
            if (!color.empty())
                return color.front() == '#' ? color : "#" + color;
        }
    }
    const DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
    if (const auto* colors = cfg.option<ConfigOptionStrings>("extruder_colour")) {
        if (idx < colors->values.size() && !colors->values[idx].empty()) {
            const std::string color = neutral_filament_color(colors->values[idx]);
            return color[0] == '#' ? color : "#" + color;
        }
    }
    return {};
}

void Sidebar::update_filament_badges()
{
    const int count = int(std::min(m_combos_filament.size(), m_filament_slots.size()));
    for (int i = 0; i < count; ++i) {
        FilamentSlotWidgets& slot = m_filament_slots[i];
        if (slot.index)
            slot.index->SetLabel(wxString::Format("%d", i + 1));
        if (slot.swatch) {
            wxColour color(wxString::FromUTF8(filament_slot_color(size_t(i))));
            if (!color.IsOk())
                color = wxColour(180, 180, 180);
            slot.swatch->SetBackgroundColour(color);
            slot.swatch->Refresh();
        }
    }
    if (m_btn_add_filament)
        m_btn_add_filament->Enable(int(m_combos_filament.size()) < k_max_filament_slots);
    if (m_btn_import_filaments) {
        const bool fff = wxGetApp().preset_bundle
            && wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
        m_btn_import_filaments->Enable(fff);
    }
    if (m_btn_flushing)
        m_btn_flushing->Enable(m_combos_filament.size() >= 2);
    if (m_btn_paint_filaments)
        m_btn_paint_filaments->Enable(m_combos_filament.size() >= 2);
    if (m_btn_layer_filaments)
        m_btn_layer_filaments->Enable(m_combos_filament.size() >= 2);
}

void Sidebar::add_filament_slot()
{
    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF)
        return;
    if (int(m_combos_filament.size()) >= k_max_filament_slots)
        return;
    auto* printer_tab = dynamic_cast<TabPrinter*>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
    if (printer_tab == nullptr)
        return;

    const bool was_single = m_combos_filament.size() <= 1;
    printer_tab->extruders_count_changed(m_combos_filament.size() + 1);
    if (was_single)
        enable_single_nozzle_multimaterial();
    update_all_filament_comboboxes();
    if (m_plater)
        m_plater->force_filament_colors_update();
    Layout();
}

void Sidebar::import_printer_filaments()
{
    if (!wxGetApp().preset_bundle || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF)
        return;
    auto* printer_tab = dynamic_cast<TabPrinter*>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
    if (printer_tab == nullptr)
        return;

    std::vector<LoadedSpool> spools = loaded_printer_spools(filament_rack_key());
    if (int(spools.size()) > k_max_filament_slots)
        spools.resize(size_t(k_max_filament_slots));
    if (spools.empty()) {
        wxMessageDialog(m_plater ? static_cast<wxWindow*>(m_plater) : this,
            _L("The selected printer has no filaments loaded. Set them on its material card, then try again."),
            _L("From printer"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }

    if (m_plater)
        m_plater->take_snapshot(_L("Import printer filaments"));
    const bool was_single = m_combos_filament.size() <= 1;
    if (spools.size() > m_combos_filament.size()) {
        printer_tab->extruders_count_changed(spools.size());
        if (was_single)
            enable_single_nozzle_multimaterial();
        update_all_filament_comboboxes();
    }

    auto selected_type = [](size_t index) {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!bundle || index >= bundle->extruders_filaments.size())
            return std::string();
        const Preset* preset = bundle->extruders_filaments[index].get_selected_preset();
        if (!preset || !preset->config.has("filament_type"))
            return std::string();
        return preset->config.option("filament_type")->serialize();
    };
    std::string note;
    for (size_t i = 0; i < spools.size(); ++i) {
        const std::string& want = spools[i].material;
        if (!want.empty()) {
            wxGetApp().search_and_select_filaments(want, false, i, note);
            const size_t dash = want.find('-');
            if (dash != std::string::npos && selected_type(i) != want)
                wxGetApp().search_and_select_filaments(want.substr(0, dash), false, i, note);
        }
        if (m_plater && !spools[i].color.empty())
            m_plater->set_filament_slot_color(i, spools[i].color);
    }
    update_presets(Preset::TYPE_FILAMENT);
    update_all_filament_comboboxes();
    update_filament_badges();
    if (m_plater)
        m_plater->force_filament_colors_update();
    if (auto* tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT))) {
        const size_t active = tab->get_active_extruder();
        if (active < wxGetApp().preset_bundle->extruders_filaments.size())
            tab->select_preset(wxGetApp().preset_bundle->extruders_filaments[active].get_selected_preset_name());
    }
    Layout();
}

void Sidebar::enable_single_nozzle_multimaterial()
{
    Tab* printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (printer_tab == nullptr || printer_tab->get_config() == nullptr)
        return;
    DynamicPrintConfig* printer_cfg = printer_tab->get_config();
    bool shared_nozzle = true;
    if (const auto* offsets = printer_cfg->option<ConfigOptionPoints>("extruder_offset")) {
        const Vec2d origin = offsets->values.empty() ? Vec2d::Zero() : offsets->values.front();
        for (const Vec2d& offset : offsets->values) {
            if ((offset - origin).squaredNorm() > 1.0) {
                shared_nozzle = false;
                break;
            }
        }
    }
    if (shared_nozzle && printer_cfg->has("single_extruder_multi_material") && !printer_cfg->opt_bool("single_extruder_multi_material")) {
        printer_cfg->set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
        printer_tab->update_dirty();
        printer_tab->reload_config();
    }

    Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    if (shared_nozzle && print_tab && print_tab->get_config() && print_tab->get_config()->has("wipe_tower")
        && !print_tab->get_config()->opt_bool("wipe_tower")) {
        print_tab->get_config()->set_key_value("wipe_tower", new ConfigOptionBool(true));
        print_tab->update_dirty();
        print_tab->reload_config();
    }
    if (printer_tab)
        printer_tab->update_wiping_button_visibility();
}

void Sidebar::remove_filament_slot(int idx)
{
    if (idx < 0 || m_combos_filament.size() <= 1 || size_t(idx) >= m_combos_filament.size())
        return;
    auto* printer_tab = dynamic_cast<TabPrinter*>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (printer_tab == nullptr || bundle == nullptr || printer_tab->get_config() == nullptr)
        return;

    const int old_count = int(m_combos_filament.size());
    const int removed_1based = idx + 1;
    m_plater->take_snapshot(_L("Remove filament"));

    if (size_t(idx) < bundle->extruders_filaments.size()) {
        for (size_t i = size_t(idx); i + 1 < bundle->extruders_filaments.size(); ++i) {
            bundle->extruders_filaments[i].select_filament(
                bundle->extruders_filaments[i + 1].get_selected_preset_name());
        }
    }
    erase_extruder_slot(bundle->printers.get_edited_preset().config, size_t(idx));

    if (auto* matrix = bundle->project_config.option<ConfigOptionFloats>("wiping_volumes_matrix")) {
        const int n = int(std::lround(std::sqrt(double(matrix->values.size()))));
        if (n == old_count && idx < n) {
            std::vector<double> compacted;
            compacted.reserve(size_t(n - 1) * size_t(n - 1));
            for (int row = 0; row < n; ++row) {
                if (row == idx)
                    continue;
                for (int col = 0; col < n; ++col) {
                    if (col == idx)
                        continue;
                    compacted.push_back(matrix->values[size_t(row * n + col)]);
                }
            }
            matrix->values = std::move(compacted);
        }
    }

    remap_model_after_filament_removed(m_plater->model(), removed_1based, old_count);
    printer_tab->extruders_count_changed(size_t(old_count - 1));
    update_all_filament_comboboxes();
    if (m_object_list)
        m_object_list->update_objects_list_extruder_column(size_t(old_count - 1));
    if (m_plater)
        m_plater->force_filament_colors_update();
    if (GLCanvas3D* canvas = m_plater->canvas3D())
        canvas->reload_scene(true, true);
    m_plater->schedule_background_process();
    Layout();
}

void Sidebar::edit_filament_slot(int idx)
{
    auto* tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
    if (tab)
        tab->set_active_extruder(idx);
    focus_settings_tab(Preset::TYPE_FILAMENT);
}

void Sidebar::show_filament_slot_menu(int idx, wxButton* btn)
{
    wxMenu menu;
    append_menu_item(&menu, wxID_ANY, _L("Edit filament settings"), _L("Open this filament's settings."),
        [this, idx](wxCommandEvent&) { edit_filament_slot(idx); }, "", this);
    append_menu_item(&menu, wxID_ANY, _L("Change color"), _L("Change the color used for this filament on the model and in the sliced preview."),
        [this, idx](wxCommandEvent&) {
            if (idx >= 0 && size_t(idx) < m_combos_filament.size() && m_combos_filament[idx])
                m_combos_filament[idx]->change_extruder_color();
        }, "", this);
    append_menu_item(&menu, wxID_ANY, _L("Paint with this filament"), _L("Paint this filament onto the selected model."),
        [this](wxCommandEvent&) { start_filament_paint(false); }, "", this,
        [this]() { return m_combos_filament.size() >= 2; });
    append_menu_item(&menu, wxID_ANY, _L("Remove filament"), _L("Remove this filament from the project and keep the others."),
        [this, idx](wxCommandEvent&) { remove_filament_slot(idx); }, "", this,
        [this]() { return m_combos_filament.size() >= 2; });
    if (btn)
        btn->PopupMenu(&menu);
}

void Sidebar::open_flushing_volumes()
{
    if (m_combos_filament.size() < 2) {
        if (m_plater && m_plater->get_notification_manager())
            m_plater->get_notification_manager()->push_notification(
                NotificationType::CustomNotification,
                NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("Add a second filament before setting flushing volumes."));
        return;
    }
    enable_single_nozzle_multimaterial();
    wxButton* purging = get_wiping_dialog_button();
    if (purging == nullptr)
        return;
    wxCommandEvent event(wxEVT_BUTTON, purging->GetId());
    event.SetEventObject(purging);
    purging->ProcessWindowEvent(event);
}

void Sidebar::start_filament_paint(bool height_range)
{
    if (m_combos_filament.size() < 2) {
        if (m_plater && m_plater->get_notification_manager())
            m_plater->get_notification_manager()->push_notification(
                NotificationType::CustomNotification,
                NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("Add a second filament to paint with more than one color."));
        return;
    }
    if (m_plater == nullptr)
        return;
    m_plater->select_view_3D("3D");
    GLCanvas3D* canvas = m_plater->canvas3D();
    if (canvas == nullptr)
        return;

    Selection& selection = canvas->get_selection();
    if (!selection.is_single_full_instance()) {
        if (m_plater->model().objects.empty()) {
            m_plater->get_notification_manager()->push_notification(
                NotificationType::CustomNotification,
                NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("Load a model before painting filaments."));
            return;
        }
        const ModelObject* object = m_plater->model().objects.front();
        if (object == nullptr || object->instances.empty()) {
            m_plater->get_notification_manager()->push_notification(
                NotificationType::CustomNotification,
                NotificationManager::NotificationLevel::RegularNotificationLevel,
                _u8L("Load a model before painting filaments."));
            return;
        }
        selection.clear();
        selection.add_instance(0, 0, true);
    }

    GLGizmosManager& gizmos = canvas->get_gizmos_manager();
    bool opened = gizmos.get_current_type() == GLGizmosManager::MmSegmentation;
    if (!opened)
        opened = gizmos.open_gizmo(GLGizmosManager::MmSegmentation);
    if (!opened) {
        m_plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification,
            NotificationManager::NotificationLevel::RegularNotificationLevel,
            _u8L("Select a single model on the bed to paint filaments or a height range."));
        return;
    }
    if (auto* painter = dynamic_cast<GLGizmoPainterBase*>(gizmos.get_gizmo(GLGizmosManager::MmSegmentation))) {
        if (height_range)
            painter->use_height_range_tool();
        else
            painter->use_brush_tool();
    }
    canvas->set_as_dirty();
}

void Sidebar::init_filament_combo(PlaterPresetComboBox** combo, int extr_idx)
{
    *combo = new PlaterPresetComboBox(m_presets_panel, Slic3r::Preset::TYPE_FILAMENT);
    (*combo)->set_extruder_idx(extr_idx);
    if ((*combo)->edit_btn)
        (*combo)->edit_btn->Hide();
    make_filament_row(*combo, extr_idx);
}

void Sidebar::remove_unused_filament_combos(const size_t current_extruder_count)
{
    if (current_extruder_count >= m_combos_filament.size())
        return;
    auto* sizer_filaments = this->m_filaments_sizer;
    while (m_combos_filament.size() > current_extruder_count) {
        const int last = int(m_combos_filament.size()) - 1;
        if (m_filament_slots.size() == m_combos_filament.size()) {
            FilamentSlotWidgets slot = m_filament_slots.back();
            m_filament_slots.pop_back();
            if (sizer_filaments->GetItemCount() > 0)
                sizer_filaments->Remove(sizer_filaments->GetItemCount() - 1);
            if (slot.index)
                slot.index->Destroy();
            if (slot.swatch)
                slot.swatch->Destroy();
            if (slot.menu)
                slot.menu->Destroy();
        } else if (sizer_filaments->GetItemCount() > 0) {
            sizer_filaments->Remove(sizer_filaments->GetItemCount() - 1);
        }
        m_combos_filament[last]->Destroy();
        m_combos_filament.pop_back();
    }
}

void Sidebar::update_all_filament_comboboxes()
{
    for (PlaterPresetComboBox* cb : m_combos_filament)
        cb->update();
    update_filament_badges();
}

void Sidebar::update_workflow_combobox_if_needed()
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (tab->is_prusa_printer() && tab->printer_model() == "SLX") {
        // lmTODO -> set correct items and selection

        std::string material_uuid;
        if (const DynamicPrintConfig& config = preset_bundle.sla_materials.get_edited_preset().config; config.has("material_uuid"))
            material_uuid = config.opt_string("material_uuid");

        // Get sorted list of available workflows, including "no workflow" item with empty uuid.
        m_available_workflows = m_plater->get_workflow_manager().workflows_for_material_sorted(material_uuid);

        // Now actually update the combobox.
        m_workflow->Clear();
        int default_id = 0;
        int id = 0;
        for (size_t i=0; i<m_available_workflows.size(); ++i) {
            m_workflow->Append(from_u8(m_available_workflows[i].name), *get_bmp_bundle("sla_printer"));
            if (m_available_workflows[i].is_default)
                default_id = id;
            ++id;
        }
        m_workflow->Select(default_id);

        m_plater->model().sla_workflow_uuid = m_available_workflows[default_id].uuid;
    }
}

void Sidebar::update_all_preset_comboboxes()
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const auto print_tech = preset_bundle.printers.get_edited_preset().printer_technology();

    // Update the print choosers to only contain the compatible presets, update the dirty flags.
    if (print_tech == ptFFF)
        m_combo_print->update();
    else {
        m_combo_sla_print->update();
        m_combo_sla_material->update();

        update_workflow_combobox_if_needed();
    }
    // Update the printer choosers, update the dirty flags.
    m_combo_printer->update();
    // Update the filament choosers to only contain the compatible presets, update the color preview,
    // update the dirty flags.
    if (print_tech == ptFFF) {
        update_all_filament_comboboxes();
    }
}

void Sidebar::update_printer_presets_combobox()
{
    m_combo_printer->update();
    Layout();
}

void Sidebar::update_presets(Preset::Type preset_type)
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const auto print_tech = preset_bundle.printers.get_edited_preset().printer_technology();

    switch (preset_type) {
    case Preset::TYPE_FILAMENT:
    {
        const size_t extruder_cnt = print_tech != ptFFF ? 1 :
                                dynamic_cast<ConfigOptionFloats*>(preset_bundle.printers.get_edited_preset().config.option("nozzle_diameter"))->values.size();
        const size_t filament_cnt = m_combos_filament.size() > extruder_cnt ? extruder_cnt : m_combos_filament.size();

        for (size_t i = 0; i < filament_cnt; i++)
            m_combos_filament[i]->update();
        update_filament_badges();

        break;
    }

    case Preset::TYPE_PRINT:
        m_combo_print->update();
        reload_process_settings();
        break;

    case Preset::TYPE_SLA_PRINT:
        m_combo_sla_print->update();
        break;

    case Preset::TYPE_SLA_MATERIAL:
        m_combo_sla_material->update();
        update_workflow_combobox_if_needed();
        break;

    case Preset::TYPE_PRINTER:
    {
        update_all_preset_comboboxes();
#if 1 // #ysFIXME_delete_after_test_of (PS 2.6.2) >> it looks like CallAfter() is no need [issue with disapearing of comboboxes are not reproducible]
        show_preset_comboboxes();
#else
        // CallAfter is really needed here to correct layout of the preset comboboxes,
        // when printer technology is changed during a project loading AND/OR switching the application mode.
        // Otherwise, some of comboboxes are invisible 
        CallAfter([this]() { show_preset_comboboxes(); });
#endif
        break;
    }

    default: break;
    }

    // Synchronize config.ini with the current selections.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
}

void Sidebar::on_select_preset(wxCommandEvent& evt)
{
    /** Note: The sidebar handles all wxEVT_COMBOBOX events emitted by its owned comboboxes.
     * With the introduction of additional combobox types beyond PlaterPresetComboBox,
     * we must now explicitly verify that the event originated from other combobox before processing.
     **/
    if (evt.GetEventObject() == m_workflow) {
        on_workflow_changed();
        return;
    }

    PlaterPresetComboBox* combo = static_cast<PlaterPresetComboBox*>(evt.GetEventObject());
    Preset::Type preset_type = combo->get_type();

    // see https://github.com/prusa3d/PrusaSlicer/issues/3889
    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender"),
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter 
    int selection = evt.GetSelection();

    auto idx = combo->get_extruder_idx();

    //! Because of The MSW and GTK version of wxBitmapComboBox derived from wxComboBox,
    //! but the OSX version derived from wxOwnerDrawnCombo.
    //! So, to get selected string we do
    //!     combo->GetString(combo->GetSelection())
    //! instead of
    //!     combo->GetStringSelection().ToUTF8().data());

    std::string preset_name = wxGetApp().preset_bundle->get_preset_name_by_alias(preset_type,
                              Preset::remove_suffix_modified(into_u8(combo->GetString(selection))), idx);

    std::string last_selected_ph_printer_name = combo->get_selected_ph_printer_name();

    bool select_preset = !combo->selection_is_changed_according_to_physical_printers();
    // TODO: ?
    if (preset_type == Preset::TYPE_FILAMENT) {
        wxGetApp().preset_bundle->set_filament_preset(idx, preset_name);

        TabFilament* tab = dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT));
        if (tab && combo->get_extruder_idx() == tab->get_active_extruder() && !tab->select_preset(preset_name)) {
            // revert previously selection
            const std::string& old_name = wxGetApp().preset_bundle->filaments.get_edited_preset().name;
                                          wxGetApp().preset_bundle->set_filament_preset(idx, old_name);
        }
        else
            // Synchronize config.ini with the current selections.
            wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
        combo->update();
        update_filament_badges();
    }
    else if (select_preset) {
        wxWindowUpdateLocker noUpdates(m_presets_panel);
        wxGetApp().get_tab(preset_type)->select_preset(preset_name, false, last_selected_ph_printer_name);
    }

    if (preset_type != Preset::TYPE_PRINTER || select_preset) {
        // update plater with new config
        m_plater->on_config_change(wxGetApp().preset_bundle->full_config());
    }
    if (preset_type == Preset::TYPE_PRINTER) {
        /* Settings list can be changed after printer preset changing, so
         * update all settings items for all item had it.
         * Furthermore, Layers editing is implemented only for FFF printers
         * and for SLA presets they should be deleted
         */
        m_object_list->update_object_list_by_printer_technology();
        s_multiple_beds.stop_autoslice(false);
        this->switch_from_autoslicing_mode();
        wxQueueEvent(this->m_plater, new SimpleEvent(EVT_REGENERATE_BED_THUMBNAILS));
        this->m_plater->update();
        std::fill(s_print_statuses.begin(), s_print_statuses.end(), PrintStatus::idle);
        this->m_plater->canvas3D()->check_volumes_outside_state(false);
    }

#ifdef __WXMSW__
    // From the Win 2004 preset combobox lose a focus after change the preset selection
    // and that is why the up/down arrow doesn't work properly
    // (see https://github.com/prusa3d/PrusaSlicer/issues/5531 ).
    // So, set the focus to the combobox explicitly
    combo->SetFocus();
#endif
}

void Sidebar::update_reslice_btn_tooltip()
{
    wxString tooltip = wxString("Slice") + " [" + GUI::shortkey_ctrl_prefix() + "R]";
    if (m_mode != comSimple)
        tooltip += wxString("\n") + _L("Hold Shift to Slice & Export G-code");
#ifdef _WIN32
    m_reslice_btn_tooltip = tooltip;
#else
    m_btn_reslice->SetToolTip(tooltip);
#endif
}

void Sidebar::init_workflow_combo(int margin_5)
{
    auto *text = new wxStaticText(m_presets_panel, wxID_ANY, _L("Workflow") + ":");
    text->SetFont(wxGetApp().small_font());
    m_presets_sizer->Add(text, 0, wxALIGN_LEFT | wxEXPAND | wxRIGHT, 4);

    m_workflow = new BitmapComboBox(
        m_presets_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr,
        wxCB_READONLY
    );

    auto combo_and_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    combo_and_btn_sizer->Add(m_workflow, 1, wxEXPAND);

    /* Not a best solution, but
     * Temporary workaround for right border alignment
     */
    auto empty_btn = new ScalableButton(
        m_presets_panel, wxID_ANY, "mirroring_transparent", wxEmptyString, wxDefaultSize,
        wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER | wxTRANSPARENT_WINDOW
    );

    combo_and_btn_sizer->Add(
        empty_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, int(0.3 * wxGetApp().em_unit())
    );

    m_presets_sizer->Add(
        combo_and_btn_sizer, 0,
        wxEXPAND |
#ifdef __WXGTK3__
            wxRIGHT,
        margin_5
    );
#else
            wxBOTTOM,
        1
    );
#endif
}



void Sidebar::on_workflow_changed()
{
    if (!m_workflow->IsEmpty() && !m_available_workflows.empty()) {
        assert(m_workflow->GetCount() == m_available_workflows.size());
        m_plater->model().sla_workflow_uuid = m_available_workflows[m_workflow->GetSelection()].uuid;
    }
}

void Sidebar::msw_rescale()
{
    const int em = std::max(1, wxGetApp().em_unit());
    const int width = std::max(GetSize().GetWidth(), 28 * em);
    SetMinSize(wxSize(width, -1));
    m_combo_print       ->msw_rescale();
    m_combo_sla_print   ->msw_rescale();
    m_combo_sla_material->msw_rescale();
    m_combo_printer     ->msw_rescale();

    for (PlaterPresetComboBox* combo : m_combos_filament)
        combo->msw_rescale();

    m_frequently_changed_parameters->msw_rescale();
    for (const std::shared_ptr<ConfigOptionsGroup>& group : m_process_groups)
        if (group)
            group->msw_rescale();
    if (m_btn_filament_gear)
        m_btn_filament_gear->sys_color_changed();
    m_object_list                  ->msw_rescale();
    m_object_manipulation          ->msw_rescale();
    m_object_layers                ->msw_rescale();

#ifdef _WIN32
    const int scaled_height = m_btn_export_gcode_removable->GetBitmapHeight();
#else
    const int scaled_height = m_btn_export_gcode_removable->GetBitmapHeight() + 4;
#endif
    m_btn_send_gcode->SetMinSize(wxSize(-1, scaled_height));
    m_btn_reslice     ->SetMinSize(wxSize(-1, scaled_height));
    m_btn_generate_supports->SetMinSize(wxSize(-1, scaled_height));

    m_scrolled_panel->Layout();
}

void Sidebar::sys_color_changed()
{
#ifdef _WIN32
    wxWindowUpdateLocker noUpdates(this);

    for (wxWindow* win : std::vector<wxWindow*>{ this, m_sliced_info->GetStaticBox(), m_object_info->GetStaticBox(), m_btn_reslice, m_btn_send_gcode, m_btn_generate_supports })
        wxGetApp().UpdateDarkUI(win);
    for (wxWindow* win : std::vector<wxWindow*>{ m_scrolled_panel, m_presets_panel })
        wxGetApp().UpdateAllStaticTextDarkUI(win);
    for (wxWindow* btn : std::vector<wxWindow*>{ m_btn_reslice, m_btn_send_gcode, m_btn_generate_supports, m_btn_connect_gcode })
        wxGetApp().UpdateDarkUI(btn, true);
    if (m_btn_full_spectrum)
        wxGetApp().UpdateDarkUI(m_btn_full_spectrum, true);
    for (wxWindow* btn : std::vector<wxWindow*>{ m_btn_flushing, m_btn_add_filament, m_btn_import_filaments, m_btn_paint_filaments, m_btn_layer_filaments })
        if (btn)
            wxGetApp().UpdateDarkUI(btn, true);
    for (const FilamentSlotWidgets& slot : m_filament_slots)
        if (slot.menu)
            wxGetApp().UpdateDarkUI(slot.menu, true);

    m_frequently_changed_parameters->sys_color_changed();
    for (const std::shared_ptr<ConfigOptionsGroup>& group : m_process_groups)
        if (group)
            group->sys_color_changed();
    if (m_btn_filament_gear)
        m_btn_filament_gear->sys_color_changed();
    update_process_chrome();
    m_object_settings              ->sys_color_changed();
#endif

    m_combo_print       ->sys_color_changed();
    m_combo_sla_print   ->sys_color_changed();
    m_combo_sla_material->sys_color_changed();
    m_combo_printer     ->sys_color_changed();

    for (PlaterPresetComboBox* combo : m_combos_filament)
        combo->sys_color_changed();

    m_object_list        ->sys_color_changed();
    m_object_manipulation->sys_color_changed();
    m_object_layers      ->sys_color_changed();

    // btn...->msw_rescale() updates icon on button, so use it
    m_btn_export_gcode          ->sys_color_changed();
    m_btn_export_gcode_removable->sys_color_changed();

    m_scrolled_panel->Layout();
    m_scrolled_panel->Refresh();
}

ObjectManipulation* Sidebar::obj_manipul()
{
    return m_object_manipulation.get();
}

ObjectList* Sidebar::obj_list()
{
    return m_object_list;
}

ObjectSettings* Sidebar::obj_settings()
{
    return m_object_settings.get();
}

ObjectLayers* Sidebar::obj_layers()
{
    return m_object_layers.get();
}

ConfigOptionsGroup* Sidebar::og_freq_chng_params(const bool is_fff)
{
    return m_frequently_changed_parameters->get_og(is_fff);
}

ConfigOptionsGroup* Sidebar::og_sla_material_params()
{
    return m_frequently_changed_parameters->get_og_sla_material();
}

void Sidebar::sync_sla_material_params()
{
    m_frequently_changed_parameters->sync_sla_material();
}

wxButton* Sidebar::get_wiping_dialog_button()
{
    return m_frequently_changed_parameters->get_wiping_dialog_button();
}

void Sidebar::update_objects_list_extruder_column(size_t extruders_count)
{
    m_object_list->update_objects_list_extruder_column(extruders_count);
}

void Sidebar::show_info_sizer()
{
    Selection& selection = wxGetApp().plater()->canvas3D()->get_selection();
    ModelObjectPtrs objects = m_plater->model().objects;
    const int obj_idx = selection.get_object_idx();
    const int inst_idx = selection.get_instance_idx();

    if (
        m_mode < comExpert
        || objects.empty()
        || obj_idx < 0
        || int(objects.size()) <= obj_idx
        || inst_idx < 0
        || int(objects[obj_idx]->instances.size()) <= inst_idx
        || objects[obj_idx]->volumes.empty() // hack to avoid crash when deleting the last object on the bed
        || (selection.is_single_full_object() && objects[obj_idx]->instances.size()> 1)
        || !(selection.is_single_full_instance() || selection.is_single_volume())
    ) {
        m_object_info->Show(false);
        return;
    }

    const ModelObject* model_object = objects[obj_idx];

    bool imperial_units = wxGetApp().app_config->get_bool("use_inches");
    double koef = imperial_units ? ObjectManipulation::mm_to_in : 1.0f;

    ModelVolume* vol = nullptr;
    Transform3d t;
    if (selection.is_single_volume()) {
        std::vector<int> obj_idxs, vol_idxs;
        wxGetApp().obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
        if (vol_idxs.size() != 1)
            // Case when this fuction is called between update selection in ObjectList and on Canvas
            // Like after try to delete last solid part in object, the object is selected in ObjectLIst when just a part is still selected on Canvas
            // see https://github.com/prusa3d/PrusaSlicer/issues/7408
            return;
        vol = model_object->volumes[vol_idxs[0]];
        t = model_object->instances[inst_idx]->get_matrix() * vol->get_matrix();
    }

    Vec3d size = vol ? vol->mesh().transformed_bounding_box(t).size() : model_object->instance_bounding_box(inst_idx).size();
    m_object_info->info_size->SetLabel(wxString::Format("%.2f x %.2f x %.2f", size(0)*koef, size(1)*koef, size(2)*koef));

    const TriangleMeshStats& stats = vol ? vol->mesh().stats() : ModelProcessing::get_object_mesh_stats(model_object);

    double volume_val = stats.volume;
    if (vol)
        volume_val *= std::fabs(t.matrix().block(0, 0, 3, 3).determinant());

    m_object_info->info_volume->SetLabel(wxString::Format("%.2f", volume_val * pow(koef,3)));
    m_object_info->info_facets->SetLabel(format_wxstr(_L_PLURAL("%1% (%2$d shell)", "%1% (%2$d shells)", stats.number_of_parts),
                                                       static_cast<int>(model_object->facets_count()), stats.number_of_parts));

    wxString info_manifold_label;
    auto mesh_errors = obj_list()->get_mesh_errors_info(&info_manifold_label);
    wxString tooltip = mesh_errors.tooltip;
    m_object_info->update_warning_icon(mesh_errors.warning_icon_name);
    m_object_info->info_manifold->SetLabel(info_manifold_label);
    m_object_info->info_manifold->SetToolTip(tooltip);
    m_object_info->manifold_warning_icon->SetToolTip(tooltip);

    m_object_info->show_sizer(true);
    if (vol || model_object->volumes.size() == 1)
        m_object_info->info_icon->Hide();

    if (m_plater->printer_technology() == ptSLA) {
        for (auto item: m_object_info->sla_hidden_items)
            item->Show(false);
    }
}

void Sidebar::update_sliced_info_sizer()
{
    if (m_sliced_info->IsShown(size_t(0)))
    {
        if (m_plater->printer_technology() == ptSLA)
        {
            const SLAPrintStatistics& ps = m_plater->active_sla_print().print_statistics();
            wxString new_label = _L("Used Material (ml)") + ":";
            const bool is_supports = ps.support_used_material > 0.0;
            if (is_supports)
                new_label += format_wxstr("\n    - %s\n    - %s", _L_PLURAL("object", "objects", m_plater->model().objects.size()), _L("supports and pad"));

            wxString info_text = is_supports ?
                wxString::Format("%.2f \n%.2f \n%.2f", (ps.objects_used_material + ps.support_used_material) / 1000,
                                                       ps.objects_used_material / 1000,
                                                       ps.support_used_material / 1000) :
                wxString::Format("%.2f", (ps.objects_used_material + ps.support_used_material) / 1000);
            m_sliced_info->SetTextAndShow(siMaterial_unit, info_text, new_label);

            wxString str_total_cost = "N/A";

            DynamicPrintConfig* cfg = wxGetApp().get_tab(Preset::TYPE_SLA_MATERIAL)->get_config();
            if (cfg->option("bottle_cost")->getFloat() > 0.0 &&
                cfg->option("bottle_volume")->getFloat() > 0.0)
            {
                double material_cost = cfg->option("bottle_cost")->getFloat() / 
                                       cfg->option("bottle_volume")->getFloat();
                str_total_cost = wxString::Format("%.3f", material_cost*(ps.objects_used_material + ps.support_used_material) / 1000);                
            }
            m_sliced_info->SetTextAndShow(siCost, str_total_cost, "Cost");

            wxString t_est = "N/A";
            if (! std::isnan(ps.estimated_print_time)) {
                t_est = from_u8(short_time_ui(get_time_dhms(float(ps.estimated_print_time))));
                if (ps.estimated_print_time_tolerance > 0.)
                    t_est += from_u8(" \u00B1 ") + from_u8(short_time_ui(get_time_dhms(float(ps.estimated_print_time_tolerance))));
            }

            m_sliced_info->SetTextAndShow(siEstimatedTime, t_est, _L("Estimated printing time") + ":");

            m_plater->get_notification_manager()->set_slicing_complete_print_time(_u8L("Estimated printing time") + ": " + into_u8(t_est), m_plater->is_sidebar_collapsed());

            // Hide non-SLA sliced info parameters
            m_sliced_info->SetTextAndShow(siFilament_m, "N/A");
            m_sliced_info->SetTextAndShow(siFilament_mm3, "N/A");
            m_sliced_info->SetTextAndShow(siFilament_g, "N/A");
            m_sliced_info->SetTextAndShow(siWTNumberOfToolchanges, "N/A");
        }
        else
        {
            const PrintStatistics& ps = m_plater->active_fff_print().print_statistics();
            const bool is_wipe_tower  = ps.total_wipe_tower_filament > 0;
            const bool is_flush       = ps.total_flush_filament > 0;

            bool imperial_units = wxGetApp().app_config->get_bool("use_inches");
            double koef = imperial_units ? ObjectManipulation::in_to_mm : 1000.0;

            wxString new_label =
                imperial_units ? _L("Used Filament (in)") : _L("Used Filament (m)");
            if (is_wipe_tower || is_flush) {
                new_label += format_wxstr(":\n    - %1%", _L("objects"));

                if (is_wipe_tower) {
                    new_label += format_wxstr("\n    - %1%", _L("wipe tower"));
                }

                if (is_flush) {
                    new_label += format_wxstr("\n    - %1%", _L("flush"));
                }
            }

            wxString info_text;
            if (is_wipe_tower || is_flush) {
                const double total_objects_used_filament =
                    ps.total_used_filament - ps.total_wipe_tower_filament - ps.total_flush_filament;
                info_text = wxString::Format(
                    "%.2f \n%.2f",
                    ps.total_used_filament / koef,
                    total_objects_used_filament / koef
                );

                if (is_wipe_tower) {
                    info_text += wxString::Format(" \n%.2f", ps.total_wipe_tower_filament / koef);
                }

                if (is_flush) {
                    info_text += wxString::Format(" \n%.2f", ps.total_flush_filament / koef);
                }
            } else {
                info_text = wxString::Format("%.2f", ps.total_used_filament / koef);
            }

            m_sliced_info->SetTextAndShow(siFilament_m, info_text, new_label);

            koef = imperial_units ? pow(ObjectManipulation::mm_to_in, 3) : 1.0f;
            new_label = imperial_units ? _L("Used Filament (in³)") : _L("Used Filament (mm³)");
            info_text = wxString::Format("%.2f", imperial_units ? ps.total_extruded_volume * koef : ps.total_extruded_volume);
            m_sliced_info->SetTextAndShow(siFilament_mm3,  info_text,      new_label);

            if (ps.total_weight == 0.0)
                m_sliced_info->SetTextAndShow(siFilament_g, "N/A");
            else {
                new_label = _L("Used Filament (g)");
                info_text = wxString::Format("%.2f", ps.total_weight);

                if (ps.filament_stats.size() > 1)
                    new_label += ":";

                const auto& extruders_filaments = wxGetApp().preset_bundle->extruders_filaments;
                for (const auto& [filament_id, filament_vol] : ps.filament_stats) {
                    assert(filament_id < extruders_filaments.size());
                    if (const Preset* preset = extruders_filaments[filament_id].get_selected_preset()) {
                        const double filament_density =
                            preset->config.opt_float("filament_density", 0);

                        double filament_weight;
                        if (ps.filament_stats.size() == 1) {
                            filament_weight = ps.total_weight;
                        } else {
                            filament_weight = filament_vol * filament_density/* *2.4052f*/ * 0.001; // assumes 1.75mm filament diameter;

                            new_label += "\n    - " + format_wxstr(_L("Filament at extruder %1%"), filament_id + 1);
                            info_text += wxString::Format("\n%.2f", filament_weight);
                        }

                        double spool_weight = preset->config.opt_float("filament_spool_weight", 0);
                        if (spool_weight != 0.0) {
                            new_label += "\n      " + _L("(including spool)");
                            info_text += wxString::Format(" (%.2f)\n", filament_weight + spool_weight);
                        }

                        if (is_wipe_tower || is_flush) {
                            double flush_vol = 0.;
                            if (auto it = ps.flush_stats.find(filament_id);
                                it != ps.flush_stats.end()) {
                                flush_vol = it->second;
                            }

                            double wipe_tower_vol = 0.;
                            if (auto it = ps.wipe_tower_stats.find(filament_id);
                                it != ps.wipe_tower_stats.end()) {
                                wipe_tower_vol = it->second;
                            }

                            const double objects_weight = (filament_vol - flush_vol - wipe_tower_vol) *
                                filament_density * 0.001;

                            new_label += "\n        - " + _L("objects");
                            info_text += wxString::Format("\n%.2f", objects_weight);

                            if (is_wipe_tower) {
                                new_label += "\n        - " + _L("wipe tower");
                                info_text += wxString::Format(
                                    "\n%.2f", wipe_tower_vol * filament_density * 0.001
                                );
                            }

                            if (is_flush) {
                                new_label += "\n        - " + _L("flush");
                                info_text +=
                                    wxString::Format("\n%.2f", flush_vol * filament_density * 0.001);
                            }
                        }
                    }
                }

                m_sliced_info->SetTextAndShow(siFilament_g, info_text, new_label);
            }

            new_label = _L("Cost");
            if (is_wipe_tower || is_flush) {
                new_label += format_wxstr(":\n    - %1%", _L("objects"));

                if (is_wipe_tower) {
                    new_label += format_wxstr("\n    - %1%", _L("wipe tower"));
                }

                if (is_flush) {
                    new_label += format_wxstr("\n    - %1%", _L("flush"));
                }
            }

            if (ps.total_cost == 0.) {
                info_text = "N/A";
            } else if (is_wipe_tower || is_flush) {
                const double total_objects_cost =
                    ps.total_cost - ps.total_wipe_tower_cost - ps.total_flush_cost;
                info_text = wxString::Format("%.2f \n%.2f", ps.total_cost, total_objects_cost);

                if (is_wipe_tower) {
                    info_text += wxString::Format(" \n%.2f", ps.total_wipe_tower_cost);
                }

                if (is_flush) {
                    info_text += wxString::Format(" \n%.2f", ps.total_flush_cost);
                }
            } else {
                info_text = wxString::Format("%.2f", ps.total_cost);
            }

            m_sliced_info->SetTextAndShow(siCost, info_text,      new_label);

            if (ps.estimated_normal_print_time == "N/A" && ps.estimated_silent_print_time == "N/A")
                m_sliced_info->SetTextAndShow(siEstimatedTime, "N/A");
            else {
                info_text = "";
                new_label = _L("Estimated printing time") + ":";
                if (ps.estimated_normal_print_time != "N/A") {
                    new_label += format_wxstr("\n   - %1%", _L("normal mode"));
                    info_text += format_wxstr("\n%1%", short_time_ui(ps.estimated_normal_print_time));

                    m_plater->get_notification_manager()->set_slicing_complete_print_time(_u8L("Estimated printing time") + ": " + ps.estimated_normal_print_time, m_plater->is_sidebar_collapsed());

                }
                if (ps.estimated_silent_print_time != "N/A") {
                    new_label += format_wxstr("\n   - %1%", _L("stealth mode"));
                    info_text += format_wxstr("\n%1%", short_time_ui(ps.estimated_silent_print_time));
                }
                m_sliced_info->SetTextAndShow(siEstimatedTime, info_text, new_label);
            }

            m_sliced_info->SetTextAndShow(siWTNumberOfToolchanges, ps.total_toolchanges > 0 ? wxString::Format("%.d", ps.total_toolchanges) : "N/A");

            // Hide non-FFF sliced info parameters
            m_sliced_info->SetTextAndShow(siMaterial_unit, "N/A");
        }
    }

    Layout();
}

void Sidebar::show_sliced_info_sizer(const bool show)
{
    if (m_autoslicing_mode) {
        return;
    }

    wxWindowUpdateLocker freeze_guard(this);

    m_sliced_info->Show(show);
    if (show)
        update_sliced_info_sizer();

    Layout();
    m_scrolled_panel->Refresh();
}

void Sidebar::show_btns_sizer(const bool show)
{
    if (m_autoslicing_mode) {
        return;
    }

    wxWindowUpdateLocker freeze_guard(this);
    m_btns_sizer->Show(show);

    Layout();
    m_scrolled_panel->Refresh();
}

void Sidebar::show_bulk_btns_sizer(const bool show)
{
    wxWindowUpdateLocker freeze_guard(this);
    m_autoslicing_btns_sizer->Show(show);

    Layout();
    m_scrolled_panel->Refresh();
}

void Sidebar::enable_buttons(bool enable)
{
    m_btn_generate_supports->Enable(enable);
    m_btn_reslice->Enable(enable);
    if (!enable)
        enable_export_buttons(false);
}

void Sidebar::enable_export_buttons(bool enable)
{
    m_btn_export_gcode->Enable(enable);
    m_btn_send_gcode->Enable(enable);
    m_btn_export_gcode_removable->Enable(enable);
    m_btn_connect_gcode->Enable(enable);
}

void Sidebar::enable_bulk_buttons(bool enable)
{
    m_btn_export_all_gcode->Enable(enable);
    m_btn_export_all_gcode_removable->Enable(enable);
    m_btn_connect_gcode_all->Enable(enable);
}

bool Sidebar::show_reslice(bool show) const {
    if (this->m_autoslicing_mode) {
        return false;
    }
    return m_btn_reslice->Show(show);
}
bool Sidebar::show_generate_supports(bool show) const {
    if (this->m_autoslicing_mode) {
        return false;
    }
    // FDM supports are toolpaths. Generating them does not leave a support mesh the model can be moved against.
    if (show && m_plater && m_plater->printer_technology() != ptSLA)
        show = false;
    return m_btn_generate_supports->Show(show);
}
bool Sidebar::show_export(bool show) const {
    if (this->m_autoslicing_mode) {
        return false;
    }
    return m_btn_export_gcode->Show(show);
}
bool Sidebar::show_send(bool show) const {
    if (this->m_autoslicing_mode) {
        return false;
    }
    return m_btn_send_gcode->Show(show);
}
bool Sidebar::show_export_removable(bool show) const {
    if (this->m_autoslicing_mode) {
        return false;
    }
    return m_btn_export_gcode_removable->Show(show);
}
bool Sidebar::show_connect(bool show) const {
    if (this->m_autoslicing_mode) {
        return false;
    }
    return m_btn_connect_gcode->Show(show);
}

bool Sidebar::show_export_all(bool show) const {
    return m_btn_export_all_gcode->Show(show);
};
bool Sidebar::show_export_removable_all(bool show) const {
    return m_btn_export_all_gcode_removable->Show(show);
};
bool Sidebar::show_connect_all(bool show) const {
    return m_btn_connect_gcode_all->Show(show);
};

void Sidebar::switch_to_autoslicing_mode() {
    this->show_sliced_info_sizer(false);
    this->show_btns_sizer(false);
    this->m_autoslicing_mode = true;
}

void Sidebar::switch_from_autoslicing_mode() {
    if (!this->m_autoslicing_mode) {
        return;
    }
    this->m_autoslicing_mode = false;
    this->show_sliced_info_sizer(true);
    this->show_bulk_btns_sizer(false);
}


void Sidebar::update_mode()
{
    m_mode = wxGetApp().get_mode();

    update_reslice_btn_tooltip();

    wxWindowUpdateLocker noUpdates(this);

    if (m_mode == comSimple)
        m_object_manipulation->set_coordinates_type(ECoordinatesType::World);

    m_object_list->get_sizer()->Show(m_mode > comSimple);

    for (const std::shared_ptr<ConfigOptionsGroup>& group : m_process_groups)
        if (group)
            group->update_visibility(m_mode);
    for (const std::shared_ptr<ConfigOptionsGroup>& group : m_sla_groups)
        if (group)
            group->update_visibility(m_mode);
    if (m_process_panel)
        m_process_panel->Layout();

    m_object_list->unselect_objects();
    m_object_list->update_selections();

    Layout();
}

void Sidebar::set_btn_label(const ActionButtonType btn_type, const wxString& label) const
{
    switch (btn_type)
    {
    case ActionButtonType::Reslice:   m_btn_reslice->SetLabelText(label);        break;
    case ActionButtonType::Export:    m_btn_export_gcode->SetToolTip(label);     break;
    case ActionButtonType::SendGCode: m_btn_send_gcode->SetLabelText(label);     break;
    case ActionButtonType::Connect: /*m_btn_connect_gcode->SetLabelText(label);*/ break;
    }
}

void Sidebar::collapse(bool collapse)
{
    is_collapsed = collapse;

    this->Show(!collapse);
    m_plater->Layout();

    // save collapsing state to the AppConfig
    if (wxGetApp().is_editor())
        wxGetApp().app_config->set("collapsed_sidebar", collapse ? "1" : "0");
}

void Sidebar::update_ui_from_settings()
{
    m_object_manipulation->update_ui_from_settings();
    show_info_sizer();
    update_sliced_info_sizer();
    m_object_list->apply_volumes_order();
}

void Sidebar::set_extruders_count(size_t extruders_count)
{
    if (extruders_count == m_combos_filament.size())
        return;

    renumber_virtual_extruders(*m_plater, static_cast<unsigned int>(extruders_count), true);

    dynamic_cast<TabFilament*>(wxGetApp().get_tab(Preset::TYPE_FILAMENT))->update_extruder_combobox();

    wxWindowUpdateLocker noUpdates_scrolled_panel(this);

    size_t i = m_combos_filament.size();
    while (i < extruders_count)
    {
        PlaterPresetComboBox* filament_choice;
        init_filament_combo(&filament_choice, i);
        m_combos_filament.push_back(filament_choice);

        // initialize selection
        filament_choice->update();
        ++i;
    }

    // remove unused choices if any
    remove_unused_filament_combos(extruders_count);

    if (m_btn_full_spectrum && m_presets_sizer) {
        m_presets_sizer->Show(size_t(5), int(extruders_count) >= 2);
        m_presets_sizer->Layout();
    }

    update_filament_badges();
    Layout();
    m_scrolled_panel->Refresh();
}


}}    // namespace Slic3r::GUI
