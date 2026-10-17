///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher

#include "CatalogDialog.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <set>
#include <sstream>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "Sidebar.hpp"

#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {
namespace {

struct FormLock {
    bool &flag;
    bool previous;
    explicit FormLock(bool &flag) : flag(flag), previous(flag) { flag = true; }
    ~FormLock() { flag = previous; }
};

bool sla_technology(const std::string &technology)
{
    return boost::iequals(technology, "SLA") || boost::iequals(technology, "DLP") || boost::iequals(technology, "MSLA");
}

std::string settings_text(const CatalogEntry &entry)
{
    std::string text;
    for (const auto &kv : entry.settings) {
        text += kv.first;
        text += " = ";
        text += kv.second;
        text += "\n";
    }
    return text;
}

std::vector<std::pair<std::string, std::string>> parse_settings(const std::string &text)
{
    std::vector<std::pair<std::string, std::string>> settings;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        boost::trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        std::string key = boost::trim_copy(line.substr(0, pos));
        std::string value = boost::trim_copy(line.substr(pos + 1));
        if (key.empty() || key == "technology" || key == "inherits" || key == "optimized_for" || key == "brand" || key == "source")
            continue;
        settings.emplace_back(std::move(key), std::move(value));
    }
    return settings;
}

bool valid_catalog_name(const std::string &name, wxString &error)
{
    if (name.empty()) {
        error = _L("Enter a name.");
        return false;
    }
    if (name.find_first_of("[]\r\n") != std::string::npos) {
        error = _L("The name cannot contain brackets or line breaks.");
        return false;
    }
    if (name.find_first_of("\\/:*?\"<>|") != std::string::npos) {
        error = _L("The name cannot contain characters that are invalid in a file name.");
        return false;
    }
    return true;
}

wxString merge_message(const CatalogMergeStats &stats)
{
    wxString message;
    if (stats.added > 0)
        message << wxString::Format(_L("Added %d presets."), stats.added) << "\n";
    if (stats.updated > 0)
        message << wxString::Format(_L("Updated %d presets."), stats.updated) << "\n";
    if (stats.kept > 0)
        message << wxString::Format(_L("Kept %d of your presets."), stats.kept) << "\n";
    if (stats.removed > 0)
        message << wxString::Format(_L("Removed %d shared presets that the new catalog no longer contains."), stats.removed) << "\n";
    if (message.empty())
        message = _L("The file has no printer or material presets.");
    return message;
}

void refresh_preset_ui()
{
    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    wxGetApp().load_current_presets(false);
    if (wxGetApp().plater() != nullptr)
        wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_PRINTER);
}

void persist_catalog(const CatalogDocument &previous, const CatalogDocument &doc)
{
    save_catalog_file(PresetBundle::user_catalog_path(), doc, true);
    PresetBundle *bundle = wxGetApp().preset_bundle;
    for (const CatalogEntry &old : previous.entries) {
        const CatalogEntry *now = nullptr;
        for (const CatalogEntry &entry : doc.entries)
            if (catalog_same_identity(entry, old))
                now = &entry;
        if (now == nullptr || !catalog_same_collection(*now, old))
            bundle->uninstall_catalog_entry(old);
    }
    bundle->install_catalog(doc, true);
    refresh_preset_ui();
}

std::string safe_file_name(std::string name)
{
    for (char &ch : name) {
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            ch = '_';
    }
    if (name.empty())
        name = "preset";
    return name + ".ini";
}

wxString last_config_dir()
{
    return from_u8(wxGetApp().app_config->get_last_dir());
}

CatalogDialog *g_open_catalog = nullptr;

} // namespace

CatalogDialog::CatalogDialog(wxWindow *parent, CatalogDocument doc)
    : DPIDialog(parent, wxID_ANY, _L("Printer and material catalog"), wxDefaultPosition, wxSize(72 * wxGetApp().em_unit(), 44 * wxGetApp().em_unit()), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_doc(std::move(doc))
{
    SetFont(wxGetApp().normal_font());
    g_open_catalog = this;

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *columns = new wxBoxSizer(wxHORIZONTAL);

    auto *left = new wxBoxSizer(wxVERTICAL);
    m_filter = new wxChoice(this, wxID_ANY);
    m_filter->Append(_L("All"));
    m_filter->Append(_L("Printers"));
    m_filter->Append(_L("Materials"));
    m_filter->SetSelection(0);
    m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE);
    left->Add(m_filter, 0, wxEXPAND | wxBOTTOM, 6);
    left->Add(m_list, 1, wxEXPAND);

    auto *grid = new wxFlexGridSizer(2, 6, 8);
    grid->AddGrowableCol(1);
    auto add_row = [&](const wxString &label, wxWindow *control) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(control, 1, wxEXPAND);
    };
    m_name = new wxTextCtrl(this, wxID_ANY);
    m_kind = new wxChoice(this, wxID_ANY);
    m_kind->Append(_L("Printer"));
    m_kind->Append(_L("Material"));
    m_technology = new wxChoice(this, wxID_ANY);
    m_technology->Append(_L("FFF"));
    m_technology->Append(_L("SLA"));
    m_inherits = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN);
    m_brand = new wxTextCtrl(this, wxID_ANY);
    m_optimized = new wxTextCtrl(this, wxID_ANY);
    m_keep = new wxCheckBox(this, wxID_ANY, _L("Keep this preset when the catalog is updated"));
    m_settings = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, 10 * wxGetApp().em_unit()), wxTE_MULTILINE);
    add_row(_L("Name"), m_name);
    add_row(_L("Kind"), m_kind);
    add_row(_L("Technology"), m_technology);
    add_row(_L("Inherits"), m_inherits);
    add_row(_L("Brand"), m_brand);
    add_row(_L("Optimized for"), m_optimized);
    grid->AddSpacer(0);
    grid->Add(m_keep, 0, wxTOP, 4);

    auto *right = new wxBoxSizer(wxVERTICAL);
    right->Add(grid, 0, wxEXPAND);
    right->Add(new wxStaticText(this, wxID_ANY, _L("Settings")), 0, wxTOP, 8);
    right->Add(m_settings, 1, wxEXPAND | wxTOP, 4);
    m_inherits->SetToolTip(_L("Profile this preset is based on. It has to be installed."));
    m_optimized->SetToolTip(_L("Printer model ids, or printer names, this profile was tuned for. Separate them with commas."));
    m_keep->SetToolTip(_L("A catalog update keeps this preset and leaves its settings as they are."));
    m_settings->SetToolTip(_L("One setting on each line, written as key = value."));

    columns->Add(left, 2, wxEXPAND | wxRIGHT, 12);
    columns->Add(right, 3, wxEXPAND);
    root->Add(columns, 1, wxEXPAND | wxALL, 10);

    auto *note = new wxStaticText(this, wxID_ANY, _L("Import merges presets into this catalog. A full catalog refreshes shared presets and keeps the ones marked as yours. Export writes a preset file that can be sent and merged."));
    note->Wrap(68 * wxGetApp().em_unit());
    root->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

    auto *buttons = new wxWrapSizer(wxHORIZONTAL);
    auto add_button = [&](const wxString &label, const std::function<void()> &handler) {
        auto *button = new wxButton(this, wxID_ANY, label);
        button->Bind(wxEVT_BUTTON, [handler](wxCommandEvent &) { handler(); });
        buttons->Add(button, 0, wxRIGHT, 6);
        return button;
    };
    add_button(_L("New"), [this]() {
        if (!commit_form())
            return;
        CatalogEntry entry;
        entry.printer = m_filter->GetSelection() != 2;
        entry.technology = "FFF";
        entry.user_owned = true;
        entry.source_explicit = true;
        const std::string base = entry.printer ? "New printer" : "New material";
        entry.name = base;
        for (int n = 2; n < 1000; ++n) {
            bool taken = false;
            for (const CatalogEntry &other : m_doc.entries)
                taken = taken || catalog_same_identity(other, entry);
            if (!taken)
                break;
            entry.name = base + " " + std::to_string(n);
        }
        m_doc.entries.push_back(entry);
        m_dirty = true;
        rebuild_list(int(m_doc.entries.size()) - 1);
    });
    add_button(_L("Delete"), [this]() {
        if (m_form_index < 0 || m_form_index >= int(m_doc.entries.size()))
            return;
        const int index = m_form_index;
        m_form_index = -1;
        m_doc.entries.erase(m_doc.entries.begin() + index);
        m_dirty = true;
        rebuild_list(std::min(index, int(m_doc.entries.size()) - 1));
    });
    add_button(_L("Add printer"), [this]() {
        if (!commit_form())
            return;
        PresetBundle *bundle = wxGetApp().preset_bundle;
        const std::string name = bundle->printers.get_selected_preset_name();
        if (name.empty()) {
            show_info(this, _L("Select a printer in the editor first."), _L("Printer and material catalog"));
            return;
        }
        const Preset &edited = bundle->printers.get_edited_preset();
        const std::string technology = edited.printer_technology() == ptSLA ? "SLA" : "FFF";
        CatalogEntry entry = preset_to_catalog_entry(bundle->printers, edited, true, technology);
        entry.name = name;
        entry.user_owned = true;
        upsert(std::move(entry));
    });
    add_button(_L("Add material"), [this]() {
        if (!commit_form())
            return;
        PresetBundle *bundle = wxGetApp().preset_bundle;
        const bool sla = bundle->printers.get_edited_preset().printer_technology() == ptSLA;
        PresetCollection &materials = sla ? static_cast<PresetCollection &>(bundle->sla_materials) : static_cast<PresetCollection &>(bundle->filaments);
        const std::string name = materials.get_selected_preset_name();
        if (name.empty()) {
            show_info(this, _L("Select a material in the editor first."), _L("Printer and material catalog"));
            return;
        }
        CatalogEntry entry = preset_to_catalog_entry(materials, materials.get_edited_preset(), false, sla ? "SLA" : "FFF");
        entry.name = name;
        entry.user_owned = true;
        upsert(std::move(entry));
    });
    add_button(_L("Import"), [this]() {
        if (!commit_form())
            return;
        if (m_dirty && !save_document())
            return;
        import_printer_or_material(this);
    });
    add_button(_L("Export preset"), [this]() {
        if (!commit_form())
            return;
        if (m_form_index < 0) {
            show_info(this, _L("Select a preset to export."), _L("Printer and material catalog"));
            return;
        }
        const CatalogEntry &entry = m_doc.entries[m_form_index];
        wxFileDialog dlg(this, _L("Export preset"), last_config_dir(), from_u8(safe_file_name(entry.name)), file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        try {
            CatalogDocument one;
            one.entries.push_back(entry);
            save_catalog_file(into_u8(dlg.GetPath()), one, false);
        } catch (const std::exception &ex) {
            show_error(this, ex.what());
            return;
        }
        wxGetApp().app_config->update_config_dir(into_u8(dlg.GetDirectory()));
    });
    add_button(_L("Export catalog"), [this]() {
        if (!commit_form())
            return;
        wxFileDialog dlg(this, _L("Export catalog"), last_config_dir(), "catalog.ini", file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        try {
            save_catalog_file(into_u8(dlg.GetPath()), m_doc, true);
        } catch (const std::exception &ex) {
            show_error(this, ex.what());
            return;
        }
        wxGetApp().app_config->update_config_dir(into_u8(dlg.GetDirectory()));
    });
    add_button(_L("Save"), [this]() { save_document(); });
    add_button(_L("Close"), [this]() { Close(); });
    root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

    SetSizer(root);
    wxGetApp().UpdateDlgDarkUI(this);

    m_filter->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (m_lock)
            return;
        if (!commit_form()) {
            FormLock lock(m_lock);
            m_filter->SetSelection(m_filter_selection);
            return;
        }
        m_filter_selection = m_filter->GetSelection();
        rebuild_list(m_form_index);
    });
    m_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
        if (m_lock)
            return;
        const int row = m_list->GetSelection();
        const int index = row == wxNOT_FOUND ? -1 : m_visible[row];
        if (index == m_form_index)
            return;
        if (!commit_form()) {
            FormLock lock(m_lock);
            for (int i = 0; i < int(m_visible.size()); ++i)
                if (m_visible[i] == m_form_index)
                    m_list->SetSelection(i);
            return;
        }
        m_form_index = index;
        load_form(index);
        if (index >= 0 && !entry_visible(m_doc.entries[index]))
            rebuild_list(-1);
        else
            refresh_row(index);
    });
    auto mark_dirty = [this](wxCommandEvent &) {
        if (!m_lock)
            m_dirty = true;
    };
    m_name->Bind(wxEVT_TEXT, mark_dirty);
    m_kind->Bind(wxEVT_CHOICE, [this](wxCommandEvent &event) {
        if (!m_lock) {
            m_dirty = true;
            if (m_form_index >= 0)
                fill_inherits(m_doc.entries[m_form_index]);
        }
        event.Skip();
    });
    m_technology->Bind(wxEVT_CHOICE, [this](wxCommandEvent &event) {
        if (!m_lock) {
            m_dirty = true;
            if (m_form_index >= 0)
                fill_inherits(m_doc.entries[m_form_index]);
        }
        event.Skip();
    });
    m_inherits->Bind(wxEVT_TEXT, mark_dirty);
    m_brand->Bind(wxEVT_TEXT, mark_dirty);
    m_optimized->Bind(wxEVT_TEXT, mark_dirty);
    m_keep->Bind(wxEVT_CHECKBOX, mark_dirty);
    m_settings->Bind(wxEVT_TEXT, mark_dirty);

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
        if (m_dirty) {
            if (!commit_form()) {
                event.Veto();
                return;
            }
            const int answer = wxMessageBox(_L("Save the catalog before closing?"), _L("Printer and material catalog"), wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
            if (answer == wxCANCEL) {
                event.Veto();
                return;
            }
            if (answer == wxYES && !save_document()) {
                event.Veto();
                return;
            }
        }
        event.Skip();
    });

    rebuild_list(m_doc.entries.empty() ? -1 : 0);
    m_dirty = false;
}

CatalogDialog::~CatalogDialog()
{
    if (g_open_catalog == this)
        g_open_catalog = nullptr;
}

void CatalogDialog::replace_document(CatalogDocument doc)
{
    m_doc = std::move(doc);
    m_dirty = false;
    m_form_index = -1;
    rebuild_list(m_doc.entries.empty() ? -1 : 0);
}

void CatalogDialog::on_dpi_changed(const wxRect &)
{
    const int em = em_unit();
    SetMinSize(wxSize(60 * em, 36 * em));
    Layout();
}

bool CatalogDialog::entry_visible(const CatalogEntry &entry) const
{
    if (m_filter->GetSelection() == 1)
        return entry.printer;
    if (m_filter->GetSelection() == 2)
        return !entry.printer;
    return true;
}

wxString CatalogDialog::entry_label(const CatalogEntry &entry) const
{
    wxString label = from_u8(entry.name);
    label << " - " << (entry.printer ? _L("Printer") : _L("Material"));
    label << " - " << (entry.user_owned ? _L("Yours") : _L("Shared"));
    return label;
}

void CatalogDialog::fill_inherits(const CatalogEntry &entry)
{
    const wxString current = m_inherits->GetValue();
    CatalogEntry probe = entry;
    probe.printer = m_kind->GetSelection() == 0;
    probe.technology = m_technology->GetSelection() == 1 ? "SLA" : "FFF";
    PresetBundle *bundle = wxGetApp().preset_bundle;
    PresetCollection &collection = probe.printer ? static_cast<PresetCollection &>(bundle->printers)
        : (sla_technology(probe.technology) ? static_cast<PresetCollection &>(bundle->sla_materials) : static_cast<PresetCollection &>(bundle->filaments));
    FormLock lock(m_lock);
    m_inherits->Clear();
    m_inherits->Append(wxEmptyString);
    for (const Preset &preset : collection()) {
        if (!preset.is_default)
            m_inherits->Append(from_u8(preset.name));
    }
    if (!m_inherits->SetStringSelection(current))
        m_inherits->SetValue(current);
}

void CatalogDialog::load_form(int index)
{
    FormLock lock(m_lock);
    const bool enabled = index >= 0 && index < int(m_doc.entries.size());
    m_name->Enable(enabled);
    m_kind->Enable(enabled);
    m_technology->Enable(enabled);
    m_inherits->Enable(enabled);
    m_brand->Enable(enabled);
    m_optimized->Enable(enabled);
    m_keep->Enable(enabled);
    m_settings->Enable(enabled);
    if (!enabled) {
        m_name->Clear();
        m_inherits->Clear();
        m_brand->Clear();
        m_optimized->Clear();
        m_settings->Clear();
        m_keep->SetValue(true);
        return;
    }
    const CatalogEntry &entry = m_doc.entries[index];
    m_name->SetValue(from_u8(entry.name));
    m_kind->SetSelection(entry.printer ? 0 : 1);
    m_technology->SetSelection(sla_technology(entry.technology) ? 1 : 0);
    m_brand->SetValue(from_u8(entry.brand));
    m_optimized->SetValue(from_u8(entry.optimized_for));
    m_keep->SetValue(entry.user_owned);
    m_settings->SetValue(from_u8(settings_text(entry)));
    fill_inherits(entry);
    if (!m_inherits->SetStringSelection(from_u8(entry.inherits)))
        m_inherits->SetValue(from_u8(entry.inherits));
}

bool CatalogDialog::commit_form()
{
    if (m_lock || m_form_index < 0 || m_form_index >= int(m_doc.entries.size()))
        return true;
    std::string name = into_u8(m_name->GetValue());
    boost::trim(name);
    wxString error;
    if (!valid_catalog_name(name, error)) {
        show_error(this, error);
        return false;
    }
    CatalogEntry updated = m_doc.entries[m_form_index];
    updated.name = std::move(name);
    updated.printer = m_kind->GetSelection() == 0;
    updated.technology = m_technology->GetSelection() == 1 ? "SLA" : "FFF";
    updated.inherits = into_u8(m_inherits->GetValue());
    boost::trim(updated.inherits);
    updated.brand = into_u8(m_brand->GetValue());
    boost::trim(updated.brand);
    updated.optimized_for = into_u8(m_optimized->GetValue());
    boost::trim(updated.optimized_for);
    updated.user_owned = m_keep->GetValue();
    updated.source_explicit = true;
    updated.settings = parse_settings(into_u8(m_settings->GetValue()));
    for (int i = 0; i < int(m_doc.entries.size()); ++i) {
        if (i != m_form_index && catalog_same_identity(m_doc.entries[i], updated)) {
            show_error(this, _L("A preset with this name is already in the catalog."));
            return false;
        }
    }
    m_doc.entries[m_form_index] = std::move(updated);
    return true;
}

void CatalogDialog::rebuild_list(int select_index)
{
    FormLock lock(m_lock);
    m_list->Clear();
    m_visible.clear();
    int select_row = wxNOT_FOUND;
    for (int i = 0; i < int(m_doc.entries.size()); ++i) {
        if (!entry_visible(m_doc.entries[i]))
            continue;
        if (i == select_index)
            select_row = int(m_visible.size());
        m_visible.push_back(i);
        m_list->Append(entry_label(m_doc.entries[i]));
    }
    if (select_row == wxNOT_FOUND && !m_visible.empty())
        select_row = 0;
    m_form_index = select_row == wxNOT_FOUND ? -1 : m_visible[select_row];
    if (select_row != wxNOT_FOUND)
        m_list->SetSelection(select_row);
    load_form(m_form_index);
}

void CatalogDialog::refresh_row(int index)
{
    for (int row = 0; row < int(m_visible.size()); ++row) {
        if (m_visible[row] == index)
            m_list->SetString(row, entry_label(m_doc.entries[index]));
    }
}

void CatalogDialog::upsert(CatalogEntry entry)
{
    const bool printer = entry.printer;
    int index = -1;
    for (int i = 0; i < int(m_doc.entries.size()); ++i) {
        if (catalog_same_identity(m_doc.entries[i], entry)) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        m_doc.entries.push_back(std::move(entry));
        index = int(m_doc.entries.size()) - 1;
    } else
        m_doc.entries[index] = std::move(entry);
    m_dirty = true;
    if (printer && m_filter->GetSelection() == 2)
        m_filter->SetSelection(0);
    if (!printer && m_filter->GetSelection() == 1)
        m_filter->SetSelection(0);
    m_filter_selection = m_filter->GetSelection();
    rebuild_list(index);
}

bool CatalogDialog::save_document()
{
    if (!commit_form())
        return false;
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Saving the catalog"), _L("The catalog will reload the presets."), false, true))
        return false;
    try {
        CatalogDocument previous;
        const std::string path = PresetBundle::user_catalog_path();
        if (boost::filesystem::exists(path))
            previous = load_catalog_file(path);
        if (m_doc.version < 1)
            m_doc.version = std::max(previous.version, 1);
        persist_catalog(previous, m_doc);
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return false;
    }
    m_dirty = false;
    return true;
}

void open_printer_material_catalog(wxWindow *parent)
{
    try {
        PresetBundle::ensure_user_catalog();
        CatalogDialog dialog(parent, load_catalog_file(PresetBundle::user_catalog_path()));
        dialog.ShowModal();
    } catch (const std::exception &ex) {
        show_error(parent, ex.what());
    }
}

void import_printer_or_material(wxWindow *parent)
{
    wxFileDialog dlg(parent, _L("Import printer or material"), last_config_dir(), "catalog.ini", file_wildcards(FT_INI), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    wxGetApp().app_config->update_config_dir(into_u8(dlg.GetDirectory()));

    CatalogDocument incoming;
    try {
        incoming = load_catalog_file(into_u8(dlg.GetPath()));
    } catch (const std::exception &ex) {
        show_error(parent, ex.what());
        return;
    }
    if (incoming.entries.empty()) {
        show_info(parent, _L("The file has no printer or material presets."), _L("Printer and material catalog"));
        return;
    }
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Importing printer or material"), _L("The catalog will reload the presets."), false, true))
        return;

    try {
        PresetBundle::ensure_user_catalog();
        const std::string path = PresetBundle::user_catalog_path();
        CatalogDocument current = load_catalog_file(path);
        CatalogMergeStats stats;
        CatalogDocument merged = merge_catalog(current, incoming, incoming.full_catalog, stats);
        if (merged.version < current.version)
            merged.version = current.version;
        persist_catalog(current, merged);
        if (g_open_catalog != nullptr)
            g_open_catalog->replace_document(merged);
        show_info(parent, merge_message(stats), _L("Printer and material catalog"));
    } catch (const std::exception &ex) {
        show_error(parent, ex.what());
    }
}

void export_printer_or_material(wxWindow *parent)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    struct Choice {
        wxString label;
        CatalogEntry entry;
    };
    std::vector<Choice> choices;
    const std::string printer_name = bundle->printers.get_selected_preset_name();
    if (!printer_name.empty()) {
        const Preset &edited = bundle->printers.get_edited_preset();
        const std::string technology = edited.printer_technology() == ptSLA ? "SLA" : "FFF";
        CatalogEntry entry = preset_to_catalog_entry(bundle->printers, edited, true, technology);
        entry.name = printer_name;
        entry.user_owned = true;
        choices.push_back({ _L("Printer") + ": " + from_u8(printer_name), std::move(entry) });
    }
    const bool sla = bundle->printers.get_edited_preset().printer_technology() == ptSLA;
    if (sla) {
        const std::string name = bundle->sla_materials.get_selected_preset_name();
        if (!name.empty()) {
            CatalogEntry entry = preset_to_catalog_entry(bundle->sla_materials, bundle->sla_materials.get_edited_preset(), false, "SLA");
            entry.name = name;
            entry.user_owned = true;
            choices.push_back({ _L("Material") + ": " + from_u8(name), std::move(entry) });
        }
    } else {
        std::set<std::string> seen;
        const std::string edited_name = bundle->filaments.get_selected_preset_name();
        for (const ExtruderFilaments &extruder : bundle->extruders_filaments) {
            const std::string name = extruder.get_selected_preset_name();
            if (name.empty() || !seen.insert(name).second)
                continue;
            const Preset *stored = bundle->filaments.find_preset(name, false, false);
            if (name != edited_name && stored == nullptr)
                continue;
            const Preset &source = name == edited_name ? bundle->filaments.get_edited_preset() : *stored;
            CatalogEntry entry = preset_to_catalog_entry(bundle->filaments, source, false, "FFF");
            entry.name = name;
            entry.user_owned = true;
            choices.push_back({ _L("Material") + ": " + from_u8(name), std::move(entry) });
        }
    }
    if (choices.empty()) {
        show_info(parent, _L("Select a printer or a material in the editor first."), _L("Printer and material catalog"));
        return;
    }

    wxDialog picker(parent, wxID_ANY, _L("Export printer or material"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(&picker, wxID_ANY, _L("Choose the presets to write. They are also added to your catalog.")), 0, wxALL, 10);
    std::vector<wxCheckBox *> boxes;
    for (const Choice &choice : choices) {
        auto *box = new wxCheckBox(&picker, wxID_ANY, choice.label);
        box->SetValue(true);
        sizer->Add(box, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        boxes.push_back(box);
    }
    sizer->Add(picker.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);
    picker.SetSizerAndFit(sizer);
    picker.SetMinSize(wxSize(36 * wxGetApp().em_unit(), -1));
    wxGetApp().UpdateDlgDarkUI(&picker);
    if (picker.ShowModal() != wxID_OK)
        return;

    CatalogDocument fragment;
    for (size_t i = 0; i < choices.size(); ++i)
        if (boxes[i]->GetValue())
            fragment.entries.push_back(choices[i].entry);
    if (fragment.entries.empty()) {
        show_info(parent, _L("Select a preset to export."), _L("Printer and material catalog"));
        return;
    }

    const wxString suggested = from_u8(safe_file_name(fragment.entries.front().name));
    wxFileDialog dlg(parent, _L("Export printer or material"), last_config_dir(), suggested, file_wildcards(FT_INI), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    wxGetApp().app_config->update_config_dir(into_u8(dlg.GetDirectory()));
    try {
        save_catalog_file(into_u8(dlg.GetPath()), fragment, false);
    } catch (const std::exception &ex) {
        show_error(parent, ex.what());
        return;
    }
    if (!wxGetApp().check_and_save_current_preset_changes(_L("Exporting printer or material"), _L("The exported presets will also be saved in your catalog."), false, true)) {
        show_info(parent, _L("The preset file was written."), _L("Printer and material catalog"));
        return;
    }
    try {
        PresetBundle::ensure_user_catalog();
        CatalogDocument current = load_catalog_file(PresetBundle::user_catalog_path());
        CatalogMergeStats stats;
        CatalogDocument merged = merge_catalog(current, fragment, false, stats);
        persist_catalog(current, merged);
        show_info(parent, merge_message(stats), _L("Printer and material catalog"));
    } catch (const std::exception &ex) {
        show_error(parent, ex.what());
    }
}

} // namespace GUI
} // namespace Slic3r
