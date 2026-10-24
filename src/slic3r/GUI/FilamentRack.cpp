///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "FilamentRack.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "../Utils/Anycubic.hpp"
#include "../Utils/PrintHost.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <boost/nowide/fstream.hpp>

#include <functional>
#include <map>
#include <memory>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/menu.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

namespace {

std::string normalize_color(std::string color)
{
    if (color.empty())
        return {};
    if (color.front() == '#')
        color.erase(color.begin());
    if (color.size() != 6)
        return {};
    for (char& ch : color)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    for (char ch : color)
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return {};
    return "#" + color;
}

bool parse_color(const std::string& color, int& r, int& g, int& b)
{
    const std::string hex = normalize_color(color);
    if (hex.size() != 7)
        return false;
    r = std::stoi(hex.substr(1, 2), nullptr, 16);
    g = std::stoi(hex.substr(3, 2), nullptr, 16);
    b = std::stoi(hex.substr(5, 2), nullptr, 16);
    return true;
}

int color_distance(const std::string& a, const std::string& b)
{
    int ar, ag, ab, br, bg, bb;
    if (!parse_color(a, ar, ag, ab) || !parse_color(b, br, bg, bb))
        return 1 << 30;
    const int dr = ar - br;
    const int dg = ag - bg;
    const int db = ab - bb;
    return dr * dr + dg * dg + db * db;
}

std::string clean_material(std::string text)
{
    text.erase(std::remove_if(text.begin(), text.end(), [](char ch) {
        return ch == '|' || ch == ',' || ch == ';' || ch == '#' || ch == '\n' || ch == '\r';
    }), text.end());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    if (text.size() > 24)
        text.resize(24);
    return text;
}

std::string config_key(std::string name)
{
    for (char& ch : name)
        if (ch == '=' || ch == '[' || ch == ']' || ch == '#' || ch == ';')
            ch = '_';
    return name;
}

const PhysicalPrinter* find_named_printer(const std::string& printer_name)
{
    if (printer_name.empty() || !wxGetApp().preset_bundle)
        return nullptr;
    return wxGetApp().preset_bundle->physical_printers.find_printer(printer_name, true);
}

int default_slot_count(const std::string& printer_name, bool& multi_color_unit)
{
    multi_color_unit = true;
    const PhysicalPrinter* printer = find_named_printer(printer_name);
    int nozzles = 1;
    bool single_nozzle_material = false;
    PrintHostType host = htOctoPrint;
    if (printer && wxGetApp().preset_bundle && !printer->preset_names.empty()) {
        if (const auto* type = printer->config.option<ConfigOptionEnum<PrintHostType>>("host_type"))
            host = type->value;
        std::string preset_name = *printer->preset_names.begin();
        if (preset_name.find(PhysicalPrinter::separator()) != std::string::npos)
            preset_name = PhysicalPrinter::get_preset_name(preset_name);
        if (const Preset* preset = wxGetApp().preset_bundle->printers.find_preset(preset_name, false)) {
            if (preset->printer_technology() == ptSLA) {
                multi_color_unit = false;
                return 1;
            }
            if (const auto* diameters = preset->config.option<ConfigOptionFloats>("nozzle_diameter"))
                nozzles = std::max(1, int(diameters->values.size()));
            if (preset->config.has("single_extruder_multi_material"))
                single_nozzle_material = preset->config.opt_bool("single_extruder_multi_material");
        }
    }
    if (nozzles > 1 && !single_nozzle_material) {
        multi_color_unit = false;
        return nozzles;
    }
    if (host == htPrusaLink || host == htPrusaConnect || host == htPrusaConnectNew)
        return 5;
    return 4;
}

bool printer_uses_ace(const std::string& printer_name)
{
    const PhysicalPrinter* printer = find_named_printer(printer_name);
    if (!printer)
        return false;
    if (const auto* type = printer->config.option<ConfigOptionEnum<PrintHostType>>("host_type"))
        return type->value == htAnycubic;
    return false;
}

std::string short_material(const std::string& name)
{
    std::string lower = name;
    for (char& ch : lower)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    const char* tokens[] = { "petg", "pla", "abs", "asa", "tpu", "hips", "pva", "nylon", "pc", "pa" };
    for (const char* token : tokens) {
        if (lower.find(token) != std::string::npos) {
            std::string label = token;
            for (char& ch : label)
                ch = char(std::toupper(static_cast<unsigned char>(ch)));
            return label;
        }
    }
    std::string trimmed = name;
    const size_t at = trimmed.find(" @");
    if (at != std::string::npos)
        trimmed.resize(at);
    return clean_material(trimmed);
}

std::vector<RackSlot> parse_slot_list(const std::string& text)
{
    std::vector<RackSlot> slots;
    size_t cursor = 0;
    while (cursor <= text.size()) {
        const size_t end = text.find(',', cursor);
        const std::string item = text.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        if (!(item.empty() && end == std::string::npos && cursor == text.size())) {
            RackSlot slot;
            const size_t split = item.find('|');
            if (split == std::string::npos)
                slot.color = normalize_color(item);
            else {
                slot.color = normalize_color(item.substr(0, split));
                slot.material = clean_material(item.substr(split + 1));
            }
            slots.push_back(std::move(slot));
        }
        if (end == std::string::npos)
            break;
        cursor = end + 1;
    }
    return slots;
}

std::string encode_slot_list(const std::vector<RackSlot>& slots)
{
    std::string stored;
    for (const RackSlot& slot : slots) {
        if (!stored.empty())
            stored.push_back(',');
        const std::string color = normalize_color(slot.color);
        if (color.size() == 7)
            stored += color.substr(1);
        stored.push_back('|');
        stored += clean_material(slot.material);
    }
    return stored;
}

MaterialBay make_bay(std::string key, std::string title, size_t count, bool dryer)
{
    MaterialBay bay;
    bay.key = std::move(key);
    bay.title = std::move(title);
    bay.slots.resize(count);
    bay.dryer = dryer;
    return bay;
}

void flatten_bays(FilamentRack& rack)
{
    rack.slots.clear();
    for (const MaterialBay& bay : rack.bays)
        rack.slots.insert(rack.slots.end(), bay.slots.begin(), bay.slots.end());
}

std::vector<int> suggest_map(const std::vector<ProjectFilament>& filaments, const std::vector<RackSlot>& slots)
{
    std::vector<int> map(filaments.size(), 0);
    std::vector<bool> used(slots.size(), false);
    for (size_t tool = 0; tool < filaments.size(); ++tool) {
        int best = -1;
        int best_distance = 1 << 30;
        for (size_t slot = 0; slot < slots.size(); ++slot) {
            if (used[slot] || slots[slot].color.empty() || filaments[tool].color.empty())
                continue;
            const int distance = color_distance(filaments[tool].color, slots[slot].color);
            if (distance < best_distance) {
                best_distance = distance;
                best = int(slot);
            }
        }
        if (best < 0) {
            for (size_t slot = 0; slot < slots.size(); ++slot) {
                if (!used[slot]) {
                    best = int(slot);
                    break;
                }
            }
        }
        if (best < 0)
            best = int(std::min(tool, slots.empty() ? 0 : slots.size() - 1));
        map[tool] = best;
        if (best >= 0 && size_t(best) < used.size())
            used[size_t(best)] = true;
    }
    return map;
}

std::vector<int> load_saved_map(const std::string& printer_name, size_t tools, size_t slots)
{
    std::vector<int> map;
    if (!wxGetApp().app_config || printer_name.empty() || slots == 0)
        return map;
    const std::string stored = wxGetApp().app_config->get("filament_map", config_key(printer_name));
    if (stored.empty())
        return map;
    map.assign(tools, 0);
    size_t cursor = 0;
    while (cursor < stored.size()) {
        const size_t end = stored.find(',', cursor);
        const std::string item = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? stored.size() : end + 1;
        const size_t split = item.find(':');
        if (split == std::string::npos)
            continue;
        try {
            const int tool = std::stoi(item.substr(0, split));
            const int slot = std::stoi(item.substr(split + 1));
            if (tool >= 0 && size_t(tool) < tools && slot >= 0 && size_t(slot) < slots)
                map[size_t(tool)] = slot;
        } catch (const std::exception&) {
        }
    }
    return map;
}

void save_map(const std::string& printer_name, const std::vector<int>& map)
{
    if (!wxGetApp().app_config || printer_name.empty())
        return;
    std::string stored;
    for (size_t tool = 0; tool < map.size(); ++tool) {
        if (!stored.empty())
            stored.push_back(',');
        stored += std::to_string(tool) + ":" + std::to_string(map[tool]);
    }
    wxGetApp().app_config->set("filament_map", config_key(printer_name), stored);
    wxGetApp().app_config->save();
}

wxString slot_label(const RackSlot& slot, int index, bool multi_color_unit)
{
    wxString label = wxString::Format(multi_color_unit ? _L("Slot %d") : _L("Extruder %d"), index + 1);
            if (!slot.material.empty())
                label += "  -  " + from_u8(slot.material);
            label += slot.color.empty() ? "  -  " + wxString(_L("empty")) : "  -  " + from_u8(slot.color);
    return label;
}

wxString slot_choice_label(const FilamentRack& rack, size_t index)
{
    size_t cursor = 0;
    for (const MaterialBay& bay : rack.bays) {
        for (size_t i = 0; i < bay.slots.size(); ++i, ++cursor) {
            if (cursor != index)
                continue;
            const RackSlot& slot = bay.slots[i];
            wxString label;
            if (bay.title.empty())
                label = wxString::Format(rack.multi_color_unit ? _L("Slot %d") : _L("Extruder %d"), int(i) + 1);
            else if (bay.slots.size() == 1)
                label = from_u8(bay.title);
            else
                label = from_u8(bay.title) + wxString::Format(" %d", int(i) + 1);
            if (!slot.material.empty())
                label += "  -  " + from_u8(slot.material);
            else if (!slot.color.empty())
                label += "  -  " + from_u8(slot.color);
            else
                label += "  -  " + wxString(_L("empty"));
            return label;
        }
    }
    if (index < rack.slots.size())
        return slot_label(rack.slots[index], int(index), rack.multi_color_unit);
    return {};
}

wxString filament_short_name(const ProjectFilament& filament, int tool)
{
    const std::string& name = filament.name;
    static const char* types[] = {
        "PLA-CF", "PETG-CF", "PA6-CF", "PAHT-CF", "PET-CF", "PC-CF", "PC-GF",
        "PETG", "PLA", "ABS", "ASA", "TPU", "PVA", "PET", "PA", "PC"
    };
    for (const char* type : types)
        if (name.find(type) != std::string::npos)
            return wxString::FromUTF8(type);
    if (name.empty())
        return wxString::Format(_L("Filament %d"), tool + 1);
    return from_u8(name);
}

wxString slot_place(const FilamentRack& rack, size_t index)
{
    size_t cursor = 0;
    for (const MaterialBay& bay : rack.bays) {
        for (size_t i = 0; i < bay.slots.size(); ++i, ++cursor) {
            if (cursor != index)
                continue;
            if (bay.slots.size() == 1 || bay.title.empty())
                return bay.title.empty() ? wxString::Format("%d", int(i) + 1) : from_u8(bay.title);
            return from_u8(bay.title) + wxString::Format(" %d", int(i) + 1);
        }
    }
    return wxString::Format("%d", int(index) + 1);
}

wxString slot_short_label(const FilamentRack& rack, size_t index)
{
    size_t cursor = 0;
    for (const MaterialBay& bay : rack.bays) {
        for (size_t i = 0; i < bay.slots.size(); ++i, ++cursor) {
            if (cursor != index)
                continue;
            wxString label = bay.slots.size() == 1 || bay.title.empty()
                ? (bay.title.empty() ? wxString::Format("%d", int(i) + 1) : from_u8(bay.title))
                : from_u8(bay.title) + wxString::Format(" %d", int(i) + 1);
            if (!bay.slots[i].material.empty())
                label += "  " + from_u8(bay.slots[i].material);
            return label;
        }
    }
    return wxString::Format("%d", int(index) + 1);
}

wxColour sheet_page()
{
    return wxColour(32, 32, 38);
}

// Text has to go through the graphics context. Drawing it on the same device
// context while that context is still alive crashes on Windows.
void paint_text(wxGraphicsContext* gc, const wxFont& font, const wxColour& colour, const wxString& text, double x, double y)
{
    gc->SetFont(font, colour);
    gc->DrawText(text, x, y);
}

void text_extent(wxGraphicsContext* gc, const wxFont& font, const wxString& text, double& width, double& height)
{
    gc->SetFont(font, *wxWHITE);
    width = 0;
    height = 0;
    gc->GetTextExtent(text.empty() ? wxString(" ") : text, &width, &height);
}

wxColour ink_for(int red, int green, int blue)
{
    return (red * 299 + green * 587 + blue * 114) / 1000 > 150 ? wxColour(20, 20, 24) : *wxWHITE;
}

wxString material_title(const std::string& name)
{
    const wxString text = from_u8(name);
    const wxString upper = text.Upper();
    static const char* known[] = {
        "PLA-CF", "PETG-CF", "PA6-CF", "PAHT-CF", "PET-CF", "PC-CF", "PC-GF",
        "PETG", "TPU", "ASA", "ABS", "PVA", "PLA", "PA", "PC"
    };
    for (const char* key : known) {
        if (upper.Contains(key))
            return wxString::FromUTF8(key);
    }
    if (text.empty())
        return L"/";
    const int space = text.Find(' ');
    return space > 0 ? text.Left(space) : text;
}

struct SlotChoice
{
    int flat { 0 };
    int number { 1 };
    bool empty { true };
    int red { 70 };
    int green { 70 };
    int blue { 78 };
    wxString material;
};

// ACE slots only. The external rack stays off this list while an ACE is connected.
std::vector<SlotChoice> slot_choices(const FilamentRack& rack)
{
    std::vector<SlotChoice> choices;
    int cursor = 0;
    for (const MaterialBay& bay : rack.bays) {
        const bool skip = rack.anycubic_ace && bay.key == "rack";
        for (size_t i = 0; i < bay.slots.size(); ++i, ++cursor) {
            if (skip)
                continue;
            SlotChoice choice;
            choice.flat = cursor;
            choice.number = int(i) + 1;
            int red = 0, green = 0, blue = 0;
            choice.empty = !parse_color(bay.slots[i].color, red, green, blue);
            if (!choice.empty) {
                choice.red = red;
                choice.green = green;
                choice.blue = blue;
            }
            choice.material = from_u8(bay.slots[i].material);
            choices.push_back(choice);
        }
    }
    if (!choices.empty())
        return choices;
    for (size_t i = 0; i < rack.slots.size(); ++i) {
        SlotChoice choice;
        choice.flat = int(i);
        choice.number = int(i) + 1;
        int red = 0, green = 0, blue = 0;
        choice.empty = !parse_color(rack.slots[i].color, red, green, blue);
        if (!choice.empty) {
            choice.red = red;
            choice.green = green;
            choice.blue = blue;
        }
        choice.material = from_u8(rack.slots[i].material);
        choices.push_back(choice);
    }
    return choices;
}

const SlotChoice* find_choice(const std::vector<SlotChoice>& choices, int flat)
{
    for (const SlotChoice& choice : choices)
        if (choice.flat == flat)
            return &choice;
    return nullptr;
}

int prefer_selectable(const FilamentRack& rack, int slot)
{
    const std::vector<SlotChoice> choices = slot_choices(rack);
    if (find_choice(choices, slot))
        return slot;
    return choices.empty() ? 0 : choices.front().flat;
}

std::string material_key(const std::string& name)
{
    const wxString title = material_title(name);
    const auto utf8 = title.utf8_str();
    std::string key = utf8.data() ? std::string(utf8.data()) : std::string();
    for (char& ch : key)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return key;
}

std::string material_key(const wxString& name)
{
    const auto utf8 = name.utf8_str();
    return material_key(utf8.data() ? std::string(utf8.data()) : std::string());
}

// Spools whose material family matches the project filament. PLA does not offer PLA-CF.
std::vector<SlotChoice> matching_slots(const FilamentRack& rack, const std::string& project_name)
{
    const std::string want = material_key(project_name);
    const std::vector<SlotChoice> all = slot_choices(rack);
    if (want.empty() || want == "/")
        return all;
    std::vector<SlotChoice> matched;
    for (const SlotChoice& choice : all) {
        if (choice.empty || choice.material.empty())
            continue;
        if (material_key(choice.material) == want)
            matched.push_back(choice);
    }
    return matched;
}

int prefer_material_slot(const FilamentRack& rack, int slot, const std::string& project_name)
{
    const std::vector<SlotChoice> choices = matching_slots(rack, project_name);
    if (find_choice(choices, slot))
        return slot;
    return choices.empty() ? slot : choices.front().flat;
}

std::vector<SlotChoice> selector_slots(const FilamentRack& rack)
{
    std::vector<SlotChoice> choices;
    int cursor = 0;
    for (const MaterialBay& bay : rack.bays) {
        if (rack.anycubic_ace && bay.key == "rack") {
            cursor += int(bay.slots.size());
            continue;
        }
        bool loaded = false;
        for (const RackSlot& slot : bay.slots)
            if (!slot.color.empty() || !slot.material.empty())
                loaded = true;
        if (!loaded && bay.key != "ace1") {
            cursor += int(bay.slots.size());
            continue;
        }
        for (size_t i = 0; i < bay.slots.size(); ++i, ++cursor) {
            SlotChoice choice;
            choice.flat = cursor;
            choice.number = int(i) + 1;
            int red = 0, green = 0, blue = 0;
            choice.empty = !parse_color(bay.slots[i].color, red, green, blue);
            if (!choice.empty) {
                choice.red = red;
                choice.green = green;
                choice.blue = blue;
            }
            choice.material = from_u8(bay.slots[i].material);
            choices.push_back(choice);
        }
    }
    if (!choices.empty())
        return choices;
    return slot_choices(rack);
}

void apply_live_ace(FilamentRack& rack, const AnycubicReadings& readings)
{
    if (readings.ace_slots.empty())
        return;
    for (const AnycubicAceSlot& reported : readings.ace_slots) {
        MaterialBay* bay = nullptr;
        if (reported.box == 0) {
            for (MaterialBay& candidate : rack.bays)
                if (candidate.key == "ace1")
                    bay = &candidate;
        } else if (reported.box == 1) {
            for (MaterialBay& candidate : rack.bays)
                if (candidate.key == "ace2")
                    bay = &candidate;
        }
        if (!bay || reported.index < 0 || size_t(reported.index) >= bay->slots.size())
            continue;
        RackSlot& slot = bay->slots[size_t(reported.index)];
        if (reported.empty) {
            // Keep a spool that is already saved. The reply omitted its color.
            if (!slot.color.empty() || !slot.material.empty())
                continue;
            slot.color.clear();
            slot.material.clear();
            continue;
        }
        if (reported.has_color) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%02X%02X%02X", reported.red, reported.green, reported.blue);
            slot.color = hex;
        }
        if (!reported.material.empty())
            slot.material = reported.material;
    }
    flatten_bays(rack);
}

bool refresh_ace_slots(const std::string& printer_name, FilamentRack& rack)
{
    if (!rack.anycubic_ace)
        return false;
    const PhysicalPrinter* printer = find_named_printer(printer_name);
    if (!printer)
        return false;
    DynamicPrintConfig config = printer->config;
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&config));
    auto* anycubic = dynamic_cast<Anycubic*>(host.get());
    if (!anycubic)
        return false;
    AnycubicReadings readings;
    std::string error;
    try {
        if (!anycubic->read_status(readings, error))
            return false;
    } catch (const std::exception&) {
        return false;
    }
    remember_printer_activity(printer_name, readings);
    apply_live_ace(rack, readings);
    return !readings.ace_slots.empty();
}

struct PrinterActivity
{
    bool job_known { false };
    bool printing { false };
    bool ace_known { false };
    AnycubicReadings ace;
};

std::map<std::string, PrinterActivity>& printer_activity()
{
    static std::map<std::string, PrinterActivity> items;
    return items;
}

std::string rack_signature(const FilamentRack& rack)
{
    std::string text;
    for (const MaterialBay& bay : rack.bays) {
        text += bay.key;
        text.push_back('=');
        for (const RackSlot& slot : bay.slots) {
            text += slot.color;
            text.push_back('|');
            text += slot.material;
            text.push_back(';');
        }
    }
    return text;
}

class SlotPopup : public wxPopupTransientWindow
{
public:
    static const int k_row = 30;

    SlotPopup(wxWindow* parent, std::vector<SlotChoice> choices, std::function<void(int)> pick, int width, const wxString& empty_note = wxString())
        : wxPopupTransientWindow(parent, wxBORDER_NONE)
        , m_choices(std::move(choices))
        , m_pick(std::move(pick))
        , m_empty_note(empty_note)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(24, 24, 28));
        const int rows = std::max(1, int(m_choices.size()));
        m_size = wxSize(std::max(96, width), 8 + rows * k_row);
        SetInitialSize(m_size);
        SetClientSize(m_size);
        SetSize(m_size);
        Bind(wxEVT_PAINT, &SlotPopup::on_paint, this);
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            const int row = (event.GetY() - 4) / k_row;
            if (row != m_hover) {
                m_hover = row;
                Refresh();
            }
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const int row = (event.GetY() - 4) / k_row;
            if (row < 0 || row >= int(m_choices.size()))
                return;
            const int flat = m_choices[size_t(row)].flat;
            auto pick = m_pick;
            Dismiss();
            if (pick)
                pick(flat);
        });
    }

    wxSize DoGetBestSize() const override { return m_size; }

private:
    std::vector<SlotChoice> m_choices;
    std::function<void(int)> m_pick;
    wxString m_empty_note;
    wxSize m_size;
    int m_hover { -1 };

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(24, 24, 28)));
        gc->DrawRectangle(0, 0, size.x, size.y);
        const wxFont font = wxGetApp().small_font();
        if (m_choices.empty()) {
            double note_w = 0;
            double note_h = 0;
            const wxString note = m_empty_note.empty() ? wxString(_L("No matching spool")) : m_empty_note;
            text_extent(gc.get(), font, note, note_w, note_h);
            paint_text(gc.get(), font, wxColour(180, 180, 188), note, 12, (size.y - note_h) / 2);
            return;
        }
        for (size_t i = 0; i < m_choices.size(); ++i) {
            const SlotChoice& choice = m_choices[i];
            const int y = 4 + int(i) * k_row;
            const wxColour chip = choice.empty ? wxColour(58, 58, 66) : wxColour(choice.red, choice.green, choice.blue);
            gc->SetBrush(wxBrush(chip));
            gc->SetPen(int(i) == m_hover ? wxPen(*wxWHITE, 1) : *wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(4, y, std::max(1, size.x - 8), k_row - 4, 5);
            const wxColour ink = choice.empty ? *wxWHITE : ink_for(choice.red, choice.green, choice.blue);
            const wxString material = choice.material.empty() ? wxString(L"/") : choice.material;
            const wxString number = wxString::Format("%d", choice.number);
            double material_h = 0;
            double ignored = 0;
            text_extent(gc.get(), font, material, ignored, material_h);
            paint_text(gc.get(), font, ink, material, 10, y + (k_row - 4 - material_h) / 2);
            double number_w = 0;
            double number_h = 0;
            text_extent(gc.get(), font, number, number_w, number_h);
            paint_text(gc.get(), font, ink, number, size.x - 12 - number_w, y + (k_row - 4 - number_h) / 2);
        }
    }
};

class MappedMaterialCard : public wxPanel
{
public:
    MappedMaterialCard(wxWindow* parent, const FilamentRack* rack, int slot, const std::string& project_color, const std::string& project_name, std::function<void()> on_change)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_rack(rack)
        , m_slot(std::max(0, slot))
        , m_on_change(std::move(on_change))
    {
        int red = 180, green = 180, blue = 180;
        if (!parse_color(project_color, red, green, blue)) {
            red = 180;
            green = 180;
            blue = 180;
        }
        m_project = wxColour(red, green, blue);
        m_project_name = project_name;
        m_title = material_title(project_name);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(168, 84));
        SetCursor(wxCURSOR_HAND);
        SetToolTip(_L("Choose the ACE slot that feeds this color."));
        Bind(wxEVT_PAINT, &MappedMaterialCard::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (event.GetY() < GetClientSize().y / 2)
                return;
            open_slots();
        });
    }

    void reset(const FilamentRack* rack, int slot)
    {
        m_rack = rack;
        m_slot = rack ? prefer_material_slot(*rack, slot, m_project_name) : std::max(0, slot);
        Refresh();
    }

    int slot() const { return m_slot; }

private:
    const FilamentRack* m_rack;
    int m_slot;
    wxColour m_project;
    std::string m_project_name;
    wxString m_title;
    std::function<void()> m_on_change;

    void open_slots()
    {
        if (!m_rack)
            return;
        std::vector<SlotChoice> choices = matching_slots(*m_rack, m_project_name);
        const wxString family = material_title(m_project_name);
        const wxString note = family.empty() || family == "/"
            ? wxString(_L("No spool is loaded in the ACE"))
            : wxString::Format(_L("No %s spool is loaded in the ACE"), family);
        auto* popup = new SlotPopup(this, std::move(choices), [this](int flat) {
            m_slot = flat;
            Refresh();
            if (m_on_change)
                m_on_change();
        }, std::max(180, GetSize().GetWidth()), note);
        const wxSize popup_size = popup->GetBestSize();
        popup->SetSize(popup_size);
        wxPoint origin = ClientToScreen(wxPoint(0, GetClientSize().GetHeight()));
        const wxRect screen = wxDisplay(wxGetTopLevelParent(this)).GetClientArea();
        if (origin.y + popup_size.y > screen.GetBottom())
            origin.y = std::max(screen.GetTop(), ClientToScreen(wxPoint(0, 0)).y - popup_size.y);
        if (origin.x + popup_size.x > screen.GetRight())
            origin.x = std::max(screen.GetLeft(), screen.GetRight() - popup_size.x);
        popup->SetSize(origin.x, origin.y, popup_size.x, popup_size.y);
        popup->Popup();
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || size.x < 8 || size.y < 8)
            return;
        const int mid = size.y * 58 / 100;
        SlotChoice loaded;
        loaded.number = m_slot + 1;
        if (m_rack) {
            // Keep the list alive. A pointer into the temporary would dangle
            // before the copy, and the selected row would lose its material name.
            const std::vector<SlotChoice> choices = slot_choices(*m_rack);
            if (const SlotChoice* found = find_choice(choices, m_slot))
                loaded = *found;
        }
        const wxColour bottom = loaded.empty ? wxColour(232, 232, 236) : wxColour(loaded.red, loaded.green, loaded.blue);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(bottom));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, 8);
        gc->Clip(0, 0, size.x, mid);
        gc->SetBrush(wxBrush(m_project));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, 8);
        gc->ResetClip();

        const wxFont title_font = wxGetApp().bold_font();
        wxString shown = m_title;
        double title_w = 0;
        double title_h = 0;
        text_extent(gc.get(), title_font, shown, title_w, title_h);
        while (shown.length() > 1 && title_w > size.x - 16) {
            shown.RemoveLast();
            text_extent(gc.get(), title_font, shown, title_w, title_h);
        }
        paint_text(gc.get(), title_font, ink_for(m_project.Red(), m_project.Green(), m_project.Blue()), shown, (size.x - title_w) / 2, (mid - title_h) / 2);

        const wxColour bottom_ink = loaded.empty ? wxColour(40, 40, 46) : ink_for(loaded.red, loaded.green, loaded.blue);
        const wxString number = wxString::Format("%d", loaded.number);
        wxString spool = loaded.material.empty() ? wxString(L"—") : loaded.material;
        const wxFont number_font = wxGetApp().normal_font();
        double number_w = 0;
        double number_h = 0;
        double spool_w = 0;
        double spool_h = 0;
        text_extent(gc.get(), number_font, number, number_w, number_h);
        text_extent(gc.get(), number_font, spool, spool_w, spool_h);
        while (spool.length() > 1 && spool_w > size.x - number_w - 36) {
            spool.RemoveLast();
            text_extent(gc.get(), number_font, spool, spool_w, spool_h);
        }
        const int y = mid + int((size.y - mid - std::max(number_h, spool_h)) / 2);
        gc->SetPen(wxPen(bottom_ink, 2));
        wxGraphicsPath chevron = gc->CreatePath();
        const double cy = mid + (size.y - mid) / 2.0 - 2;
        chevron.MoveToPoint(10, cy);
        chevron.AddLineToPoint(14, cy + 4);
        chevron.AddLineToPoint(18, cy);
        gc->StrokePath(chevron);
        paint_text(gc.get(), number_font, bottom_ink, spool, 24, y);
        paint_text(gc.get(), number_font, bottom_ink, number, size.x - 10 - number_w, y);
    }
};

class PrintPreviewCard : public wxPanel
{
public:
    PrintPreviewCard(wxWindow* parent, const wxImage* preview, const wxString& sliced_for, const wxString& print_time, const wxString& filament_use)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
        , m_for(sliced_for)
        , m_time(print_time.empty() ? wxString(L"—") : print_time)
        , m_use(filament_use.empty() ? wxString(L"—") : filament_use)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(-1, 176));
        if (preview && preview->IsOk())
            m_model = wxBitmap(*preview);
        Bind(wxEVT_PAINT, &PrintPreviewCard::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh(false);
            event.Skip();
        });
    }

private:
    wxBitmap m_model;
    wxString m_for;
    wxString m_time;
    wxString m_use;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc || size.x < 4 || size.y < 4)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        if (size.y > 1)
            gc->SetBrush(gc->CreateLinearGradientBrush(0, 0, 0, size.y, wxColour(92, 112, 140), wxColour(198, 208, 220)));
        else
            gc->SetBrush(wxBrush(wxColour(92, 112, 140)));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, 12);
        if (m_model.IsOk() && size.x > 40 && size.y > 40) {
            const double scale = std::min((size.x - 80.0) / m_model.GetWidth(), (size.y - 36.0) / m_model.GetHeight());
            const double drawn_w = m_model.GetWidth() * scale;
            const double drawn_h = m_model.GetHeight() * scale;
            gc->DrawBitmap(m_model, (size.x - drawn_w) / 2.0, (size.y - drawn_h) / 2.0 + 8, drawn_w, drawn_h);
        }
        if (!m_for.empty())
            paint_text(gc.get(), wxGetApp().normal_font(), wxColour(245, 245, 248), _L("for ") + m_for, 16, 12);

        const wxFont stats_font = wxGetApp().small_font();
        double time_w = 0;
        double time_h = 0;
        double use_w = 0;
        double use_h = 0;
        text_extent(gc.get(), stats_font, m_time, time_w, time_h);
        text_extent(gc.get(), stats_font, m_use, use_w, use_h);
        const int card_w = int(std::max(time_w, use_w)) + 44;
        const int card_h = 58;
        const int card_x = 12;
        const int card_y = size.y - card_h - 12;
        gc->SetBrush(wxBrush(wxColour(246, 246, 248)));
        gc->DrawRoundedRectangle(card_x, card_y, card_w, card_h, 8);
        paint_text(gc.get(), stats_font, wxColour(28, 28, 34), m_time, card_x + 28, card_y + 8);
        paint_text(gc.get(), stats_font, wxColour(28, 28, 34), m_use, card_x + 28, card_y + 30);
        gc->SetPen(wxPen(wxColour(90, 90, 100), 2));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawEllipse(card_x + 8, card_y + 8, 14, 14);
        gc->StrokeLine(card_x + 15, card_y + 15, card_x + 15, card_y + 11);
        gc->StrokeLine(card_x + 15, card_y + 15, card_x + 18, card_y + 16);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(wxColour(90, 90, 100)));
        gc->DrawEllipse(card_x + 10, card_y + 32, 10, 10);
        gc->DrawRoundedRectangle(card_x + 13, card_y + 28, 4, 8, 1);
    }
};

class PrinterPill : public wxPanel
{
public:
    PrinterPill(wxWindow* parent, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(220, 40));
        SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT, &PrinterPill::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (m_on_click) m_on_click(); });
    }

    void set_label(const wxString& label)
    {
        m_label = label;
        Refresh();
    }

    void set_slots(const std::vector<SlotChoice>& slots)
    {
        m_slots = slots;
        SetMinSize(wxSize(220 + int(m_slots.size()) * 22, 40));
        Refresh();
    }

private:
    wxString m_label;
    std::vector<SlotChoice> m_slots;
    std::function<void()> m_on_click;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(sheet_page()));
        gc->DrawRectangle(0, 0, size.x, size.y);
        gc->SetBrush(wxBrush(wxColour(43, 127, 255)));
        gc->DrawRoundedRectangle(0, 0, size.x, size.y, 8);
        gc->SetPen(wxPen(*wxWHITE, 2));
        wxGraphicsPath chevron = gc->CreatePath();
        chevron.MoveToPoint(14, size.y / 2.0 - 2);
        chevron.AddLineToPoint(18, size.y / 2.0 + 3);
        chevron.AddLineToPoint(22, size.y / 2.0 - 2);
        gc->StrokePath(chevron);
        const wxFont font = wxGetApp().normal_font();
        wxString shown = m_label;
        double text_w = 0;
        double text_h = 0;
        text_extent(gc.get(), font, shown, text_w, text_h);
        const int chips = int(m_slots.size()) * 22;
        while (shown.length() > 1 && text_w > size.x - 40 - chips) {
            shown.RemoveLast();
            text_extent(gc.get(), font, shown, text_w, text_h);
        }
        paint_text(gc.get(), font, *wxWHITE, shown, 30, (size.y - text_h) / 2);
        int chip_x = size.x - 12 - 16;
        for (int i = int(m_slots.size()) - 1; i >= 0; --i) {
            const SlotChoice& choice = m_slots[size_t(i)];
            const wxColour chip = choice.empty ? wxColour(210, 210, 214) : wxColour(choice.red, choice.green, choice.blue);
            gc->SetPen(wxPen(*wxWHITE, choice.empty ? 1 : 2));
            gc->SetBrush(wxBrush(chip));
            gc->DrawRoundedRectangle(chip_x, (size.y - 16) / 2.0, 16, 16, 3);
            if (choice.empty) {
                gc->SetPen(wxPen(wxColour(90, 90, 100), 1));
                gc->StrokeLine(chip_x + 4, size.y / 2.0, chip_x + 12, size.y / 2.0);
            }
            chip_x -= 22;
        }
    }
};

class AceSwatches : public wxPanel
{
public:
    explicit AceSwatches(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        Bind(wxEVT_PAINT, &AceSwatches::on_paint, this);
    }

    void set_rack(const FilamentRack* rack)
    {
        m_choices = rack ? selector_slots(*rack) : std::vector<SlotChoice>{};
        const int count = std::max(1, int(m_choices.size()));
        SetMinSize(wxSize(28 + count * 28, 36));
        Refresh();
    }

private:
    std::vector<SlotChoice> m_choices;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(sheet_page()));
        gc->DrawRectangle(0, 0, size.x, size.y);
        gc->SetPen(wxPen(wxColour(150, 150, 160), 2));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(2, 6, 22, 22, 4);
        gc->SetBrush(wxBrush(wxColour(150, 150, 160)));
        gc->DrawRectangle(7, 11, 4, 4);
        gc->DrawRectangle(14, 11, 4, 4);
        gc->DrawRectangle(7, 18, 4, 4);
        gc->DrawRectangle(14, 18, 4, 4);
        const wxFont font = wxGetApp().small_font();
        int x = 30;
        for (const SlotChoice& choice : m_choices) {
            const wxColour chip = choice.empty ? wxColour(58, 58, 66) : wxColour(choice.red, choice.green, choice.blue);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(chip));
            gc->DrawRoundedRectangle(x, 6, 22, 22, 4);
            if (choice.empty) {
                double dash_w = 0;
                double dash_h = 0;
                text_extent(gc.get(), font, wxString(L"-"), dash_w, dash_h);
                paint_text(gc.get(), font, wxColour(180, 180, 188), wxString(L"-"), x + (22 - dash_w) / 2, 6 + (22 - dash_h) / 2);
            }
            x += 28;
        }
    }
};

class OptionCheck : public wxPanel
{
public:
    OptionCheck(wxWindow* parent, const wxString& label, bool on)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_label(label)
        , m_on(on)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(220, 40));
        SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT, &OptionCheck::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            m_on = !m_on;
            Refresh();
        });
    }

private:
    wxString m_label;
    bool m_on;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(sheet_page()));
        gc->DrawRectangle(0, 0, size.x, size.y);
        const wxColour edge = m_on ? wxColour(43, 127, 255) : wxColour(78, 78, 88);
        gc->SetPen(wxPen(edge, 1));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, 8);
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(m_on ? wxColour(43, 127, 255) : wxColour(50, 50, 58)));
        gc->DrawRoundedRectangle(12, (size.y - 16) / 2.0, 16, 16, 3);
        if (m_on) {
            gc->SetPen(wxPen(*wxWHITE, 2));
            wxGraphicsPath check = gc->CreatePath();
            const double y = size.y / 2.0;
            check.MoveToPoint(15, y);
            check.AddLineToPoint(18, y + 4);
            check.AddLineToPoint(24, y - 3);
            gc->StrokePath(check);
        }
        const wxFont font = wxGetApp().normal_font();
        double text_w = 0;
        double text_h = 0;
        text_extent(gc.get(), font, m_label, text_w, text_h);
        paint_text(gc.get(), font, m_on ? wxColour(43, 127, 255) : *wxWHITE, m_label, 36, (size.y - text_h) / 2);
    }
};

class SheetButton : public wxPanel
{
public:
    SheetButton(wxWindow* parent, const wxString& label, bool primary, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_label(label)
        , m_primary(primary)
        , m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(primary ? 118 : 96, 36));
        SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT, &SheetButton::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (m_on_click) m_on_click(); });
    }

private:
    wxString m_label;
    bool m_primary;
    bool m_hover { false };
    std::function<void()> m_on_click;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(sheet_page()));
        gc->DrawRectangle(0, 0, size.x, size.y);
        if (m_primary)
            gc->SetBrush(wxBrush(m_hover ? wxColour(70, 148, 255) : wxColour(43, 127, 255)));
        else
            gc->SetBrush(wxBrush(m_hover ? wxColour(48, 48, 56) : sheet_page()));
        gc->SetPen(m_primary ? *wxTRANSPARENT_PEN : wxPen(wxColour(120, 120, 130), 1));
        gc->DrawRoundedRectangle(1, 1, size.x - 2, size.y - 2, size.y / 2.0);
        const wxFont font = wxGetApp().normal_font();
        double text_w = 0;
        double text_h = 0;
        text_extent(gc.get(), font, m_label, text_w, text_h);
        paint_text(gc.get(), font, *wxWHITE, m_label, (size.x - text_w) / 2, (size.y - text_h) / 2);
    }
};

class MiniIcon : public wxPanel
{
public:
    enum Kind { Pencil, Refresh };

    MiniIcon(wxWindow* parent, Kind kind, std::function<void()> on_click)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(28, 28), wxBORDER_NONE)
        , m_kind(kind)
        , m_on_click(std::move(on_click))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(28, 28));
        SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT, &MiniIcon::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (m_on_click) m_on_click(); });
    }

private:
    Kind m_kind;
    std::function<void()> m_on_click;

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetBrush(wxBrush(sheet_page()));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(0, 0, size.x, size.y);
        gc->SetPen(wxPen(*wxWHITE, 2));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        if (m_kind == Pencil) {
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(8, 20);
            path.AddLineToPoint(18, 8);
            path.AddLineToPoint(20, 10);
            path.AddLineToPoint(10, 22);
            path.AddLineToPoint(8, 20);
            gc->StrokePath(path);
            gc->StrokeLine(16, 10, 18, 12);
        } else {
            wxGraphicsPath path = gc->CreatePath();
            path.MoveToPoint(18, 8);
            path.AddArc(size.x / 2.0, size.y / 2.0, 7, -0.6, 3.6, true);
            gc->StrokePath(path);
            wxGraphicsPath head = gc->CreatePath();
            head.MoveToPoint(16, 5);
            head.AddLineToPoint(20, 8);
            head.AddLineToPoint(15, 11);
            gc->StrokePath(head);
        }
    }
};

struct DryPreset { const char* name; int celsius; int hours; };

const DryPreset* dry_presets(int& count)
{
    static const DryPreset presets[] = {
        { "PLA", 50, 8 }, { "PETG", 60, 8 }, { "TPU", 55, 8 },
        { "ABS", 65, 8 }, { "ASA", 65, 8 }, { "PA", 70, 12 },
    };
    count = int(sizeof(presets) / sizeof(presets[0]));
    return presets;
}

class RoundValue : public wxPanel
{
public:
    RoundValue(wxWindow* parent, const wxString& text)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(96, 40));
        m_edit = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTE_CENTRE | wxTE_PROCESS_ENTER);
        m_edit->SetBackgroundColour(wxColour(48, 48, 58));
        m_edit->SetForegroundColour(*wxWHITE);
        m_edit->SetFont(wxGetApp().normal_font());
        Bind(wxEVT_PAINT, &RoundValue::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            const wxSize size = GetClientSize();
            m_edit->SetSize(10, 8, std::max(8, size.x - 20), std::max(8, size.y - 16));
            event.Skip();
        });
    }

    void set_value(const wxString& text)
    {
        m_edit->ChangeValue(text);
        m_edit->SetInsertionPointEnd();
    }

    wxString value() const { return m_edit->GetValue(); }

private:
    wxTextCtrl* m_edit { nullptr };

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(sheet_page()));
        gc->DrawRectangle(0, 0, size.x, size.y);
        gc->SetBrush(wxBrush(wxColour(48, 48, 58)));
        gc->SetPen(wxPen(wxColour(90, 90, 102), 1));
        gc->DrawRoundedRectangle(1, 1, std::max(0, size.x - 2), std::max(0, size.y - 2), 10);
    }
};

class DryPicker : public wxPanel
{
public:
    explicit DryPicker(wxWindow* parent, std::function<void(int)> on_pick)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
        , m_on_pick(std::move(on_pick))
        , m_label(_L("Not enabled"))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(sheet_page());
        SetMinSize(wxSize(180, 40));
        SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT, &DryPicker::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { open_menu(); });
    }

private:
    std::function<void(int)> m_on_pick;
    wxString m_label;

    void open_menu()
    {
        wxMenu menu;
        menu.Append(wxID_HIGHEST + 1, _L("Not enabled"));
        int count = 0;
        const DryPreset* presets = dry_presets(count);
        for (int i = 0; i < count; ++i)
            menu.Append(wxID_HIGHEST + 2 + i, wxString::FromUTF8(presets[i].name));
        const int picked = GetPopupMenuSelectionFromUser(menu, wxPoint(0, GetClientSize().y));
        const int index = picked - (wxID_HIGHEST + 1);
        if (index < 0 || index > count)
            return;
        if (index == 0)
            m_label = _L("Not enabled");
        else
            m_label = wxString::FromUTF8(presets[index - 1].name);
        Refresh();
        if (m_on_pick)
            m_on_pick(index);
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(sheet_page()));
        gc->DrawRectangle(0, 0, size.x, size.y);
        gc->SetBrush(wxBrush(wxColour(48, 48, 58)));
        gc->SetPen(wxPen(wxColour(90, 90, 102), 1));
        gc->DrawRoundedRectangle(1, 1, std::max(0, size.x - 2), std::max(0, size.y - 2), 10);
        gc->SetPen(wxPen(*wxWHITE, 2));
        wxGraphicsPath chevron = gc->CreatePath();
        chevron.MoveToPoint(size.x - 22, size.y / 2.0 - 2);
        chevron.AddLineToPoint(size.x - 18, size.y / 2.0 + 3);
        chevron.AddLineToPoint(size.x - 14, size.y / 2.0 - 2);
        gc->StrokePath(chevron);
        const wxFont font = wxGetApp().normal_font();
        double text_w = 0;
        double text_h = 0;
        text_extent(gc.get(), font, m_label, text_w, text_h);
        paint_text(gc.get(), font, *wxWHITE, m_label, 14, (size.y - text_h) / 2);
    }
};

wxString printer_choice_label(const std::string& name)
{
    wxString text = from_u8(name);
    const int space = text.Find(' ');
    if (space > 0) {
        const wxString first = text.Left(space);
        const wxString rest = text.Mid(space + 1);
        if (rest == first || rest.StartsWith(first + " "))
            return rest;
    }
    return text;
}

wxString physical_printer_label(const std::string& name)
{
    wxString label = printer_choice_label(name);
    if (!wxGetApp().preset_bundle)
        return label;
    const PhysicalPrinter* printer = wxGetApp().preset_bundle->physical_printers.find_printer(name, true);
    // host_type is an enum, not a string. Reading it as a string crashes.
    const auto* host_type = printer ? printer->config.option<ConfigOptionEnum<PrintHostType>>("host_type") : nullptr;
    if (host_type && host_type->value == htAnycubic && !label.Lower().Contains("lan"))
        label << " (LAN)";
    return label;
}

bool sliced_model_differs(const std::string& physical_name)
{
    if (!wxGetApp().preset_bundle || physical_name.empty())
        return false;
    const PhysicalPrinter* printer = wxGetApp().preset_bundle->physical_printers.find_printer(physical_name, true);
    if (!printer || printer->preset_names.empty())
        return false;
    const std::string preset = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    return !preset.empty() && printer->preset_names.count(preset) == 0;
}

class MatchDialog : public wxDialog
{
public:
    MatchDialog(wxWindow* parent, const std::string& printer_name, const FilamentRack& rack, const std::vector<ProjectFilament>& filaments, const std::vector<int>& initial, const std::string& task_name, const std::string& print_time, const std::string& filament_use, const wxImage* preview)
        : wxDialog(parent, wxID_ANY, task_name.empty()
            ? (rack.multi_color_unit ? _L("Match filaments") : _L("Match extruders"))
            : _L("Start print"),
            wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxGetApp().UpdateDarkUI(this);
        m_rack = rack;
        m_filaments = filaments;
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        const bool start_print = !task_name.empty();
        if (start_print) {
            SetBackgroundColour(sheet_page());
            if (wxGetApp().preset_bundle) {
                for (const PhysicalPrinter& printer : wxGetApp().preset_bundle->physical_printers()) {
                    if (printer.config.opt_string("print_host").empty())
                        continue;
                    if (printer.name == printer_name)
                        m_printer_index = int(m_printer_names.size());
                    m_printer_names.push_back(printer.name);
                }
            }
            if (m_printer_names.empty() && !printer_name.empty()) {
                m_printer_names.push_back(printer_name);
                m_printer_index = 0;
            }
            if (m_printer_index < 0 && !m_printer_names.empty())
                m_printer_index = 0;
        }
        if (!start_print) {
            const wxString intro = rack.multi_color_unit
                ? _L("Each color in this print is fed by one slot in the printer's multi-color unit. That unit is an AMS, ACE, MMU, or the same kind of rack on a single nozzle. It is not another extruder. Pick the slot that holds the matching filament.")
                : _L("Each color in this print is fed by one extruder. Pick the extruder that holds the matching filament.");
            auto* text = new wxStaticText(this, wxID_ANY, intro);
            text->Wrap(520);
            sizer->Add(text, 0, wxEXPAND | wxALL, 12);
            if (rack.multi_color_unit) {
                auto* hint = new wxStaticText(this, wxID_ANY, rack.anycubic_ace
                    ? _L("Rack is the external spool. ACE1 and ACE2 are the multi-color units. The list shows what is saved for this printer. Click a spool on the Devices page to change it. Empty spools can still be chosen.")
                    : _L("The colors below are the ones saved for this printer on the Devices page. Click a slot there to say what is loaded. Empty slots can still be chosen."));
                hint->Wrap(520);
                sizer->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
            }
        } else {
            auto* task_row = new wxBoxSizer(wxHORIZONTAL);
            auto* task_label = new wxStaticText(this, wxID_ANY, _L("Printing task"));
            task_label->SetFont(wxGetApp().bold_font());
            task_label->SetForegroundColour(*wxWHITE);
            task_label->SetBackgroundColour(sheet_page());
            task_row->Add(task_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_task_view = new wxStaticText(this, wxID_ANY, from_u8(task_name), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
            m_task_view->SetForegroundColour(*wxWHITE);
            m_task_view->SetBackgroundColour(sheet_page());
            task_row->Add(m_task_view, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_task = new wxTextCtrl(this, wxID_ANY, from_u8(task_name), wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
            wxGetApp().UpdateDarkUI(m_task);
            m_task->Hide();
            task_row->Add(m_task, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* pencil = new MiniIcon(this, MiniIcon::Pencil, [this]() { begin_task_edit(); });
            pencil->SetToolTip(_L("Rename the printing task"));
            task_row->Add(pencil, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(task_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 18);
            m_task->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { finish_task_edit(); });
            m_task->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
                finish_task_edit();
                event.Skip();
            });

            wxString sliced_for;
            if (wxGetApp().preset_bundle)
                sliced_for = printer_choice_label(wxGetApp().preset_bundle->printers.get_selected_preset_name());
            sizer->Add(new PrintPreviewCard(this, preview, sliced_for, from_u8(print_time), from_u8(filament_use)), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);
        }

        auto* chips = start_print ? static_cast<wxSizer*>(new wxWrapSizer(wxHORIZONTAL)) : static_cast<wxSizer*>(new wxBoxSizer(wxVERTICAL));
        for (size_t tool = 0; tool < filaments.size(); ++tool) {
            wxColour color(180, 180, 180);
            int red = 180, green = 180, blue = 180;
            if (parse_color(filaments[tool].color, red, green, blue))
                color.Set(red, green, blue);

            if (!start_print) {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                auto* swatch = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(22, 22));
                swatch->SetBackgroundColour(color);
                swatch->SetMinSize(wxSize(22, 22));
                row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
                wxString name = filaments[tool].name.empty()
                    ? wxString::Format(_L("Filament %d"), int(tool) + 1)
                    : from_u8(filaments[tool].name);
                auto* label = new wxStaticText(this, wxID_ANY, name);
                label->SetMinSize(wxSize(180, -1));
                row->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
                auto* choice = make_slot_choice(this, m_rack, initial, tool, false);
                row->Add(choice, 1, wxALIGN_CENTER_VERTICAL);
                chips->Add(row, 0, wxEXPAND | wxBOTTOM, 8);
                continue;
            }

            int slot = tool < initial.size() ? initial[tool] : 0;
            slot = prefer_material_slot(m_rack, slot, filaments[tool].name);
            auto* card = new MappedMaterialCard(this, &m_rack, slot, filaments[tool].color, filaments[tool].name, {});
            m_cards.push_back(card);
            chips->Add(card, 0, wxRIGHT | wxBOTTOM, 10);
        }
        if (start_print)
            sizer->Add(chips, 0, wxLEFT | wxRIGHT | wxTOP, 18);
        else
            sizer->Add(chips, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

        if (start_print) {
            auto* mapping = new wxStaticText(this, wxID_ANY, _L("Color mapping mismatch may cause bleeding. Edit mapping or sync from ACE Pro."));
            mapping->SetForegroundColour(wxColour(72, 196, 96));
            mapping->SetBackgroundColour(sheet_page());
            mapping->Wrap(620);
            sizer->Add(mapping, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

            auto* pick_row = new wxBoxSizer(wxHORIZONTAL);
            auto* pick = new wxStaticText(this, wxID_ANY, _L("Please select a printer"));
            pick->SetFont(wxGetApp().bold_font());
            pick->SetForegroundColour(*wxWHITE);
            pick->SetBackgroundColour(sheet_page());
            pick_row->Add(pick, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            auto* reload = new MiniIcon(this, MiniIcon::Refresh, [this]() { apply_printer(); });
            reload->SetToolTip(_L("Reload the filaments saved for this printer."));
            pick_row->Add(reload, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(pick_row, 0, wxLEFT | wxRIGHT | wxTOP, 16);

            auto* device_row = new wxBoxSizer(wxHORIZONTAL);
            m_pill = new PrinterPill(this, [this]() { choose_printer(); });
            if (m_printer_index >= 0 && size_t(m_printer_index) < m_printer_names.size())
                m_pill->set_label(physical_printer_label(m_printer_names[size_t(m_printer_index)]));
            m_pill->set_slots(selector_slots(m_rack));
            device_row->Add(m_pill, 1, wxEXPAND | wxRIGHT, 8);
            m_swatches = new AceSwatches(this);
            m_swatches->set_rack(&m_rack);
            device_row->Add(m_swatches, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(device_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

            m_mismatch = new wxStaticText(this, wxID_ANY, _L("Model mismatch, unable to initiate printing"));
            m_mismatch->SetForegroundColour(wxColour(255, 122, 46));
            m_mismatch->SetBackgroundColour(sheet_page());
            m_mismatch->SetFont(wxGetApp().bold_font());
            const bool differs = m_printer_index >= 0 && size_t(m_printer_index) < m_printer_names.size()
                && sliced_model_differs(m_printer_names[size_t(m_printer_index)]);
            m_mismatch->Show(differs);
            sizer->Add(m_mismatch, 0, wxLEFT | wxRIGHT | wxTOP, 12);

            m_busy_note = new wxStaticText(this, wxID_ANY, _L("This printer is already printing. Another print cannot be sent to it."));
            m_busy_note->SetForegroundColour(wxColour(255, 122, 46));
            m_busy_note->SetBackgroundColour(sheet_page());
            m_busy_note->SetFont(wxGetApp().bold_font());
            m_busy_note->Wrap(620);
            const bool busy = m_printer_index >= 0 && size_t(m_printer_index) < m_printer_names.size()
                && printer_is_busy(m_printer_names[size_t(m_printer_index)]);
            m_busy_note->Show(busy);
            sizer->Add(m_busy_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

            auto* options = new wxFlexGridSizer(2, 10, 10);
            options->AddGrowableCol(0, 1);
            options->AddGrowableCol(1, 1);
            options->Add(new OptionCheck(this, _L("Bed leveling"), true), 1, wxEXPAND);
            options->Add(new OptionCheck(this, _L("Resonance compensation"), false), 1, wxEXPAND);
            options->Add(new OptionCheck(this, _L("Time-lapse"), false), 1, wxEXPAND);
            options->Add(new OptionCheck(this, _L("Flow calibration"), false), 1, wxEXPAND);
            sizer->Add(options, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

            auto* drying_label = new wxStaticText(this, wxID_ANY, _L("Drying options"));
            drying_label->SetFont(wxGetApp().bold_font());
            drying_label->SetForegroundColour(*wxWHITE);
            drying_label->SetBackgroundColour(sheet_page());
            sizer->Add(drying_label, 0, wxLEFT | wxRIGHT | wxTOP, 18);

            auto* drying_row = new wxBoxSizer(wxHORIZONTAL);
            drying_row->Add(new DryPicker(this, [this](int index) {
                int count = 0;
                const DryPreset* presets = dry_presets(count);
                if (index <= 0 || index > count) {
                    if (m_dry_temp)
                        m_dry_temp->set_value(L"-");
                    if (m_dry_hours)
                        m_dry_hours->set_value(L"-");
                    return;
                }
                if (m_dry_temp)
                    m_dry_temp->set_value(wxString::Format(L"%d\u00B0C", presets[index - 1].celsius));
                if (m_dry_hours)
                    m_dry_hours->set_value(wxString::Format("%dh", presets[index - 1].hours));
            }), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_dry_temp = new RoundValue(this, L"-");
            m_dry_hours = new RoundValue(this, L"-");
            drying_row->Add(m_dry_temp, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            drying_row->Add(m_dry_hours, 1, wxALIGN_CENTER_VERTICAL);
            sizer->Add(drying_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        }

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->AddStretchSpacer(1);
        if (start_print) {
            buttons->Add(new SheetButton(this, _L("Cancel"), false, [this]() { EndModal(wxID_CANCEL); }), 0, wxRIGHT, 8);
            buttons->Add(new SheetButton(this, _L("Start print"), true, [this]() {
                if (printer_is_busy(selected_printer())) {
                    if (m_busy_note) {
                        m_busy_note->Show();
                        Layout();
                    }
                    return;
                }
                EndModal(wxID_OK);
            }), 0);
            SetEscapeId(wxID_CANCEL);
        } else {
            auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
            auto* ok = new wxButton(this, wxID_OK, _L("Use these slots"));
            ok->SetDefault();
            wxGetApp().UpdateDarkUI(cancel, true);
            wxGetApp().UpdateDarkUI(ok, true);
            buttons->Add(cancel, 0, wxRIGHT, 8);
            buttons->Add(ok, 0);
        }
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 18);
        SetSizer(sizer);
        sizer->Fit(this);
        if (start_print && GetSize().x < 680)
            SetSize(680, GetSize().y);
        SetMinSize(GetSize());
        CentreOnParent();
    }

    std::vector<int> result() const
    {
        if (!m_cards.empty()) {
            std::vector<int> map;
            map.reserve(m_cards.size());
            for (const MappedMaterialCard* card : m_cards)
                map.push_back(std::max(0, card->slot()));
            return map;
        }
        std::vector<int> map;
        map.reserve(m_choices.size());
        for (wxChoice* choice : m_choices)
            map.push_back(std::max(0, choice->GetSelection()));
        return map;
    }

    std::string selected_printer() const
    {
        if (m_printer_index >= 0 && size_t(m_printer_index) < m_printer_names.size())
            return m_printer_names[size_t(m_printer_index)];
        return {};
    }

    std::string edited_task() const
    {
        if (!m_task)
            return {};
        const auto utf8 = m_task->GetValue().utf8_str();
        return utf8.data() ? std::string(utf8.data()) : std::string();
    }

private:
    wxChoice* make_slot_choice(wxWindow* parent, const FilamentRack& rack, const std::vector<int>& initial, size_t tool, bool short_labels)
    {
        auto* choice = new wxChoice(parent, wxID_ANY);
        for (size_t slot = 0; slot < rack.slots.size(); ++slot)
            choice->Append(short_labels ? slot_short_label(rack, slot) : slot_choice_label(rack, slot));
        int selected = tool < initial.size() ? initial[tool] : 0;
        if (selected < 0 || selected >= int(choice->GetCount()))
            selected = 0;
        if (choice->GetCount() > 0)
            choice->SetSelection(selected);
        wxGetApp().UpdateDarkUI(choice);
        m_choices.push_back(choice);
        return choice;
    }

    void begin_task_edit()
    {
        if (!m_task || !m_task_view)
            return;
        m_task_view->Hide();
        m_task->Show();
        m_task->SetFocus();
        m_task->SelectAll();
        Layout();
    }

    void finish_task_edit()
    {
        if (m_finishing_task || !m_task || !m_task_view || !m_task->IsShown())
            return;
        m_finishing_task = true;
        m_task_view->SetLabel(m_task->GetValue());
        m_task->Hide();
        m_task_view->Show();
        Layout();
        m_finishing_task = false;
    }

    void choose_printer()
    {
        if (m_printer_names.empty())
            return;
        wxMenu menu;
        for (size_t i = 0; i < m_printer_names.size(); ++i) {
            wxString label = physical_printer_label(m_printer_names[i]);
            const bool busy = printer_is_busy(m_printer_names[i]);
            if (busy)
                label << _L(" (printing)");
            const int item = wxID_HIGHEST + 1 + int(i);
            menu.Append(item, label);
            if (busy)
                menu.Enable(item, false);
        }
        const wxPoint origin = m_pill ? wxPoint(m_pill->GetPosition().x, m_pill->GetRect().GetBottom()) : wxDefaultPosition;
        const int picked = GetPopupMenuSelectionFromUser(menu, origin);
        const int index = picked - (wxID_HIGHEST + 1);
        if (index < 0 || size_t(index) >= m_printer_names.size())
            return;
        m_printer_index = index;
        apply_printer();
    }

    void apply_printer()
    {
        if (m_printer_index < 0 || size_t(m_printer_index) >= m_printer_names.size())
            return;
        const std::string name = m_printer_names[size_t(m_printer_index)];
        m_rack = load_filament_rack(name);
        {
            wxBusyCursor wait;
            refresh_ace_slots(name, m_rack);
        }
        if (m_pill) {
            m_pill->set_label(physical_printer_label(name));
            m_pill->set_slots(selector_slots(m_rack));
        }
        if (m_swatches)
            m_swatches->set_rack(&m_rack);
        std::vector<int> map = load_saved_map(name, m_filaments.size(), m_rack.slots.size());
        if (map.size() != m_filaments.size())
            map = suggest_map(m_filaments, m_rack.slots);
        for (size_t tool = 0; tool < m_cards.size(); ++tool) {
            int slot = tool < map.size() ? map[tool] : 0;
            m_cards[tool]->reset(&m_rack, prefer_material_slot(m_rack, slot, m_filaments[tool].name));
        }
        if (m_mismatch) {
            m_mismatch->Show(sliced_model_differs(name));
            Layout();
        }
        if (m_busy_note) {
            m_busy_note->Show(printer_is_busy(name));
            Layout();
        }
    }

    std::vector<wxChoice*> m_choices;
    std::vector<MappedMaterialCard*> m_cards;
    std::vector<std::string> m_printer_names;
    std::vector<ProjectFilament> m_filaments;
    FilamentRack m_rack;
    int m_printer_index { -1 };
    PrinterPill* m_pill { nullptr };
    AceSwatches* m_swatches { nullptr };
    wxStaticText* m_mismatch { nullptr };
    wxStaticText* m_busy_note { nullptr };
    wxStaticText* m_task_view { nullptr };
    wxTextCtrl* m_task { nullptr };
    RoundValue* m_dry_temp { nullptr };
    RoundValue* m_dry_hours { nullptr };
    bool m_finishing_task { false };
};

} // namespace

void remember_printer_activity(const std::string& printer_name, const AnycubicReadings& readings)
{
    if (printer_name.empty())
        return;
    PrinterActivity& item = printer_activity()[printer_name];
    if (readings.has_job) {
        item.job_known = true;
        item.printing = readings.printing;
    }
    if (readings.ace_reported) {
        item.ace_known = true;
        item.ace.ace_reported = true;
        item.ace.ace_slots = readings.ace_slots;
        item.ace.ace_count = readings.ace_count;
    }
}

bool printer_is_busy(const std::string& printer_name)
{
    const auto found = printer_activity().find(printer_name);
    return found != printer_activity().end() && found->second.job_known && found->second.printing;
}

bool apply_remembered_ace(const std::string& printer_name, FilamentRack& rack)
{
    const auto found = printer_activity().find(printer_name);
    if (found == printer_activity().end() || !found->second.ace_known)
        return false;
    const std::string before = rack_signature(rack);
    apply_live_ace(rack, found->second.ace);
    return rack_signature(rack) != before;
}

bool rack_color_rgb(const std::string& color, int& r, int& g, int& b)
{
    return parse_color(color, r, g, b);
}

std::string filament_rack_key()
{
    if (!wxGetApp().preset_bundle)
        return {};
    const std::string selected = wxGetApp().preset_bundle->physical_printers.get_selected_printer_name();
    if (!selected.empty())
        return selected;
    return wxGetApp().preset_bundle->printers.get_selected_preset_name();
}

FilamentRack load_filament_rack(const std::string& printer_name)
{
    FilamentRack rack;
    const int fallback = default_slot_count(printer_name, rack.multi_color_unit);
    rack.anycubic_ace = rack.multi_color_unit && printer_uses_ace(printer_name);
    if (rack.anycubic_ace) {
        rack.bays.push_back(make_bay("ace1", "ACE1", 4, true));
        rack.bays.push_back(make_bay("ace2", "ACE2", 4, true));
        rack.bays.push_back(make_bay("rack", "Rack", 1, false));
    } else {
        rack.bays.push_back(make_bay("slots", {}, size_t(std::max(1, fallback)), false));
    }

    std::string stored;
    if (wxGetApp().app_config && !printer_name.empty()) {
        const std::string wanted = config_key(printer_name);
        stored = wxGetApp().app_config->get("filament_rack", wanted);
        // The printer may have been saved under an older name, such as the brand
        // repeated in front of the current name. Use that rack instead of an empty card.
        if (stored.rfind("v2", 0) != 0 && wxGetApp().app_config->has_section("filament_rack")) {
            std::string migrated;
            for (const auto& item : wxGetApp().app_config->get_section("filament_rack")) {
                const bool older_name = item.first.size() > wanted.size()
                    && item.first[item.first.size() - wanted.size() - 1] == ' '
                    && item.first.compare(item.first.size() - wanted.size(), wanted.size(), wanted) == 0;
                if (!older_name || item.second.rfind("v2", 0) != 0)
                    continue;
                migrated = item.second;
                break;
            }
            if (!migrated.empty()) {
                stored = std::move(migrated);
                wxGetApp().app_config->set("filament_rack", wanted, stored);
            }
        }
    }
    if (stored.rfind("v2;", 0) == 0 || stored == "v2") {
        size_t cursor = stored == "v2" ? stored.size() : 3;
        while (cursor < stored.size()) {
            const size_t end = stored.find(';', cursor);
            const std::string field = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
            cursor = end == std::string::npos ? stored.size() : end + 1;
            const size_t split = field.find('=');
            if (split == std::string::npos)
                continue;
            const std::string key = field.substr(0, split);
            const std::string value = field.substr(split + 1);
            if (key.rfind("dry_", 0) == 0) {
                const std::string bay_key = key.substr(4);
                for (MaterialBay& bay : rack.bays) {
                    if (bay.key != bay_key)
                        continue;
                    std::vector<std::string> parts;
                    size_t part_at = 0;
                    while (part_at <= value.size()) {
                        const size_t part_end = value.find(',', part_at);
                        parts.push_back(value.substr(part_at, part_end == std::string::npos ? std::string::npos : part_end - part_at));
                        if (part_end == std::string::npos)
                            break;
                        part_at = part_end + 1;
                    }
                    if (!parts.empty())
                        bay.drying = parts[0] == "1";
                    try {
                        if (parts.size() > 1 && !parts[1].empty())
                            bay.dry_celsius = std::stoi(parts[1]);
                        if (parts.size() > 2)
                            bay.dry_material = parts[2];
                        if (parts.size() > 3 && !parts[3].empty())
                            bay.dry_hours = std::stoi(parts[3]);
                        if (parts.size() > 4 && !parts[4].empty())
                            bay.dry_started = std::stoll(parts[4]);
                    } catch (const std::exception&) {
                        bay.dry_celsius = -1;
                    }
                }
                continue;
            }
            for (MaterialBay& bay : rack.bays) {
                if (bay.key != key)
                    continue;
                std::vector<RackSlot> parsed = parse_slot_list(value);
                if (parsed.size() < bay.slots.size())
                    parsed.resize(bay.slots.size());
                bay.slots = std::move(parsed);
            }
        }
    } else if (!stored.empty()) {
        std::vector<RackSlot> parsed = parse_slot_list(stored);
        MaterialBay* target = &rack.bays.front();
        for (MaterialBay& bay : rack.bays)
            if (bay.key == "ace1")
                target = &bay;
        if (parsed.size() < target->slots.size())
            parsed.resize(target->slots.size());
        target->slots = std::move(parsed);
    }

    flatten_bays(rack);
    return rack;
}

void save_filament_rack(const std::string& printer_name, const FilamentRack& rack)
{
    if (!wxGetApp().app_config || printer_name.empty())
        return;
    std::string stored = "v2";
    for (const MaterialBay& bay : rack.bays) {
        stored.push_back(';');
        stored += bay.key;
        stored.push_back('=');
        stored += encode_slot_list(bay.slots);
        if (bay.dryer) {
            stored += ";dry_" + bay.key + "=" + (bay.drying ? "1" : "0") + ","
                + std::to_string(bay.dry_celsius) + "," + bay.dry_material + ","
                + std::to_string(bay.dry_hours) + "," + std::to_string(bay.dry_started);
        }
    }
    wxGetApp().app_config->set("filament_rack", config_key(printer_name), stored);
    wxGetApp().app_config->save();
}

static std::string spool_family(const std::string& name)
{
    std::string upper = name;
    for (char& ch : upper)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    static const char* known[] = {
        "PLA-CF", "PETG-CF", "PA6-CF", "PAHT-CF", "PET-CF", "PC-CF", "PC-GF",
        "PETG", "TPU", "ASA", "ABS", "PVA", "PLA", "PA", "PC"
    };
    for (const char* key : known)
        if (upper.find(key) != std::string::npos)
            return key;
    const size_t space = name.find(' ');
    if (space == 0 || name.empty())
        return {};
    return space == std::string::npos ? name : name.substr(0, space);
}

std::vector<LoadedSpool> loaded_printer_spools(const std::string& printer_name)
{
    FilamentRack rack = load_filament_rack(printer_name);
    apply_remembered_ace(printer_name, rack);
    std::vector<LoadedSpool> loaded;
    auto consider = [&](const RackSlot& slot) {
        int red = 0, green = 0, blue = 0;
        const bool has_color = rack_color_rgb(slot.color, red, green, blue);
        const std::string family = spool_family(slot.material);
        if (!has_color && family.empty())
            return;
        LoadedSpool item;
        item.material = family;
        if (has_color) {
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", red, green, blue);
            item.color = hex;
        }
        loaded.push_back(std::move(item));
    };
    for (const char* key : { "ace1", "ace2" }) {
        for (const MaterialBay& bay : rack.bays) {
            if (bay.key != key)
                continue;
            for (const RackSlot& slot : bay.slots)
                consider(slot);
        }
    }
    for (const MaterialBay& bay : rack.bays) {
        if (bay.key == "ace1" || bay.key == "ace2")
            continue;
        for (const RackSlot& slot : bay.slots)
            consider(slot);
    }
    return loaded;
}

std::vector<ProjectFilament> current_project_filaments()
{
    std::vector<ProjectFilament> filaments;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const DynamicPrintConfig* cfg = wxGetApp().plater() ? wxGetApp().plater()->config() : nullptr;
    const auto* colors = cfg ? cfg->option<ConfigOptionStrings>("filament_colour") : nullptr;
    size_t count = 1;
    if (bundle)
        count = std::max<size_t>(1, bundle->extruders_filaments.size());
    if (colors)
        count = std::max(count, colors->values.size());
    count = std::min<size_t>(count, 16);
    filaments.reserve(count);
    for (size_t tool = 0; tool < count; ++tool) {
        ProjectFilament filament;
        filament.tool = int(tool);
        if (colors && tool < colors->values.size())
            filament.color = normalize_color(colors->values[tool]);
        if (bundle && tool < bundle->extruders_filaments.size()) {
            if (const Preset* preset = bundle->extruders_filaments[tool].get_selected_preset()) {
                filament.name = preset->name;
                if (filament.color.empty())
                    filament.color = normalize_color(preset->config.opt_string("filament_colour", 0));
            }
        }
        filaments.push_back(std::move(filament));
    }
    return filaments;
}

bool ask_filament_match(wxWindow* parent, const std::string& printer_name, std::vector<int>& slot_for_tool, const std::string& task_name, const std::string& print_time, const std::string& filament_use, std::string* edited_task, std::string* chosen_printer, const wxImage* preview)
{
    FilamentRack rack = load_filament_rack(printer_name);
    if (!task_name.empty() && rack.anycubic_ace) {
        wxBusyCursor wait;
        refresh_ace_slots(printer_name, rack);
    }
    const std::vector<ProjectFilament> filaments = current_project_filaments();
    if (filaments.empty() || rack.slots.empty()) {
        slot_for_tool.clear();
        if (edited_task)
            *edited_task = task_name;
        return true;
    }
    std::vector<int> initial = load_saved_map(printer_name, filaments.size(), rack.slots.size());
    if (initial.size() != filaments.size())
        initial = suggest_map(filaments, rack.slots);
    MatchDialog dialog(parent ? parent : wxGetApp().GetTopWindow(), printer_name, rack, filaments, initial, task_name, print_time, filament_use, preview);
    if (dialog.ShowModal() != wxID_OK)
        return false;
    slot_for_tool = dialog.result();
    const std::string printer = dialog.selected_printer().empty() ? printer_name : dialog.selected_printer();
    save_map(printer, slot_for_tool);
    if (edited_task)
        *edited_task = dialog.edited_task().empty() ? task_name : dialog.edited_task();
    if (chosen_printer)
        *chosen_printer = printer;
    return true;
}

bool edit_rack_slot(wxWindow* parent, RackSlot& slot, int index, bool multi_color_unit)
{
    // The Windows color window corrupts the heap when it is opened from the
    // painted material card. The swatches and the three number fields stay inside this dialog.
    wxWindow* owner = parent ? wxGetTopLevelParent(parent) : wxGetApp().GetTopWindow();
    wxDialog dialog(owner, wxID_ANY, multi_color_unit
        ? wxString::Format(_L("Slot %d"), index + 1)
        : wxString::Format(_L("Extruder %d"), index + 1));
    wxGetApp().UpdateDarkUI(&dialog);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* intro = new wxStaticText(&dialog, wxID_ANY, multi_color_unit
        ? _L("This is one position in the printer's multi-color unit. Set the color and material that are loaded there.")
        : _L("Set the color and material loaded in this extruder."));
    intro->Wrap(360);
    sizer->Add(intro, 0, wxEXPAND | wxALL, 12);

    int r = 254, g = 50, b = 173;
    const bool started_with_color = parse_color(slot.color, r, g, b);
    bool picked = started_with_color;
    bool cleared = false;
    auto* color_label = new wxStaticText(&dialog, wxID_ANY, _L("Color"));
    color_label->SetForegroundColour(wxColour(168, 168, 176));
    sizer->Add(color_label, 0, wxLEFT | wxRIGHT, 16);

    auto* swatch = new wxPanel(&dialog, wxID_ANY, wxDefaultPosition, wxSize(-1, 48));
    swatch->SetBackgroundColour(started_with_color ? wxColour(r, g, b) : wxColour(38, 38, 46));
    swatch->SetMinSize(wxSize(420, 48));
    sizer->Add(swatch, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    struct Swatch { int r, g, b; };
    static const Swatch swatches[] = {
        { 0x1A, 0x1A, 0x1A }, { 0xF2, 0xF2, 0xF2 }, { 0x9A, 0x9A, 0x9A },
        { 0xE2, 0x3B, 0x3B }, { 0xFF, 0x6A, 0x00 }, { 0xF5, 0xD4, 0x00 },
        { 0x3D, 0xDC, 0x84 }, { 0x2E, 0xC4, 0xE6 }, { 0x2F, 0x8C, 0xFF },
        { 0x6B, 0x2F, 0xD6 }, { 0xFE, 0x32, 0xAD }, { 0x8B, 0x5A, 0x2B },
    };
    // A fixed grid. A wrapping row is measured as one line, then wraps when
    // the dialog opens and pushes Save below the window.
    auto* chips = new wxGridSizer(6, 8, 8);
    struct Chip {
        wxPanel* frame;
        int r, g, b;
    };
    std::vector<Chip> chip_frames;
    chip_frames.reserve(sizeof(swatches) / sizeof(swatches[0]));
    auto* red_spin = new wxSpinCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(88, -1), wxSP_ARROW_KEYS, 0, 255, r);
    auto* green_spin = new wxSpinCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(88, -1), wxSP_ARROW_KEYS, 0, 255, g);
    auto* blue_spin = new wxSpinCtrl(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(88, -1), wxSP_ARROW_KEYS, 0, 255, b);
    wxGetApp().UpdateDarkUI(red_spin);
    wxGetApp().UpdateDarkUI(green_spin);
    wxGetApp().UpdateDarkUI(blue_spin);

    auto mark_chips = [&](int red, int green, int blue) {
        for (Chip& chip : chip_frames) {
            const bool on = chip.r == red && chip.g == green && chip.b == blue;
            chip.frame->SetBackgroundColour(on ? wxColour(254, 50, 173) : wxColour(28, 26, 36));
            chip.frame->Refresh();
        }
    };
    auto show_color = [&](int red, int green, int blue, bool from_user) {
        red_spin->SetValue(red);
        green_spin->SetValue(green);
        blue_spin->SetValue(blue);
        if (from_user) {
            picked = true;
            cleared = false;
        }
        swatch->SetBackgroundColour(wxColour(red, green, blue));
        swatch->Refresh();
        mark_chips(red, green, blue);
    };
    for (const Swatch& chip_color : swatches) {
        auto* frame = new wxPanel(&dialog, wxID_ANY, wxDefaultPosition, wxSize(36, 36));
        frame->SetMinSize(wxSize(36, 36));
        frame->SetBackgroundColour(wxColour(28, 26, 36));
        frame->SetCursor(wxCursor(wxCURSOR_HAND));
        auto* inner = new wxPanel(frame, wxID_ANY, wxDefaultPosition, wxSize(26, 26));
        inner->SetBackgroundColour(wxColour(chip_color.r, chip_color.g, chip_color.b));
        inner->SetMinSize(wxSize(26, 26));
        inner->SetCursor(wxCursor(wxCURSOR_HAND));
        auto* frame_sizer = new wxBoxSizer(wxVERTICAL);
        frame_sizer->Add(inner, 0, wxALIGN_CENTER | wxALL, 5);
        frame->SetSizer(frame_sizer);
        frame->Bind(wxEVT_LEFT_UP, [&, chip_color](wxMouseEvent&) {
            show_color(chip_color.r, chip_color.g, chip_color.b, true);
        });
        inner->Bind(wxEVT_LEFT_UP, [&, chip_color](wxMouseEvent&) {
            show_color(chip_color.r, chip_color.g, chip_color.b, true);
        });
        chips->Add(frame, 0, wxALIGN_CENTER);
        chip_frames.push_back({ frame, chip_color.r, chip_color.g, chip_color.b });
    }
    sizer->Add(chips, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    if (started_with_color)
        mark_chips(r, g, b);

    auto* numbers = new wxBoxSizer(wxHORIZONTAL);
    auto add_channel = [&](const wxString& name, wxSpinCtrl* spin) {
        auto* column = new wxBoxSizer(wxVERTICAL);
        auto* label = new wxStaticText(&dialog, wxID_ANY, name);
        label->SetForegroundColour(wxColour(168, 168, 176));
        column->Add(label, 0, wxBOTTOM, 4);
        column->Add(spin, 0);
        numbers->Add(column, 1, wxEXPAND | wxRIGHT, 12);
    };
    add_channel(_L("Red"), red_spin);
    add_channel(_L("Green"), green_spin);
    add_channel(_L("Blue"), blue_spin);
    auto on_spin = [&](wxSpinEvent&) {
        picked = true;
        cleared = false;
        const int red = red_spin->GetValue();
        const int green = green_spin->GetValue();
        const int blue = blue_spin->GetValue();
        swatch->SetBackgroundColour(wxColour(red, green, blue));
        swatch->Refresh();
        mark_chips(red, green, blue);
    };
    red_spin->Bind(wxEVT_SPINCTRL, on_spin);
    green_spin->Bind(wxEVT_SPINCTRL, on_spin);
    blue_spin->Bind(wxEVT_SPINCTRL, on_spin);
    sizer->Add(numbers, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

    auto* material_label = new wxStaticText(&dialog, wxID_ANY, _L("Material"));
    material_label->SetForegroundColour(wxColour(168, 168, 176));
    sizer->Add(material_label, 0, wxLEFT | wxRIGHT | wxTOP, 16);
    static const char* material_names[] = {
        "PLA", "PLA-CF", "PETG", "PETG-CF", "TPU", "ASA", "ABS", "PVA",
        "PA", "PA6-CF", "PET-CF", "PC", "PC-CF", "PC-GF", "PAHT-CF",
    };
    auto* material = new wxChoice(&dialog, wxID_ANY);
    for (const char* name : material_names)
        material->Append(wxString::FromUTF8(name));
    const wxString current_material = from_u8(slot.material);
    int material_index = material->FindString(current_material);
    if (material_index == wxNOT_FOUND && !current_material.empty())
        material_index = material->Append(current_material);
    if (material_index != wxNOT_FOUND)
        material->SetSelection(material_index);
    wxGetApp().UpdateDarkUI(material);
    sizer->Add(material, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* clear = new wxButton(&dialog, wxID_ANY, _L("Empty"));
    auto* cancel = new wxButton(&dialog, wxID_CANCEL, _L("Cancel"));
    auto* ok = new wxButton(&dialog, wxID_OK, _L("Save"));
    ok->SetDefault();
    wxGetApp().UpdateDarkUI(clear, true);
    wxGetApp().UpdateDarkUI(cancel, true);
    wxGetApp().UpdateDarkUI(ok, true);
    clear->Bind(wxEVT_BUTTON, [&dialog, &cleared](wxCommandEvent&) {
        cleared = true;
        dialog.EndModal(wxID_OK);
    });
    buttons->Add(clear, 0, wxRIGHT, 8);
    buttons->AddStretchSpacer(1);
    buttons->Add(cancel, 0, wxRIGHT, 8);
    buttons->Add(ok, 0);
    sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 16);
    dialog.SetSizer(sizer);
    dialog.SetSizerAndFit(sizer);
    const wxSize fitted = dialog.GetSize();
    dialog.SetSize(std::max(460, fitted.GetWidth()), fitted.GetHeight());
    dialog.SetMinSize(dialog.GetSize());
    dialog.CentreOnParent();
    if (dialog.ShowModal() != wxID_OK)
        return false;
    if (cleared) {
        slot.color.clear();
        slot.material.clear();
        return true;
    }
    if (picked) {
        const int red = std::clamp(red_spin->GetValue(), 0, 255);
        const int green = std::clamp(green_spin->GetValue(), 0, 255);
        const int blue = std::clamp(blue_spin->GetValue(), 0, 255);
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%02X%02X%02X", red, green, blue);
        slot.color = hex;
    } else {
        slot.color.clear();
    }
    wxString material_text;
    if (material->GetSelection() != wxNOT_FOUND)
        material_text = material->GetString(material->GetSelection());
    const auto material_utf8 = material_text.utf8_str();
    slot.material = clean_material(material_utf8.data() ? std::string(material_utf8.data()) : std::string());
    return true;
}

namespace {

bool tool_command(const std::string& line, size_t& number_begin, size_t& number_end, int& tool)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= line.size() || line[i] == ';')
        return false;
    const char* prefixes[] = { "M135 T", "M108 T", "T" };
    for (const char* prefix : prefixes) {
        const size_t length = std::strlen(prefix);
        if (line.compare(i, length, prefix) != 0)
            continue;
        size_t j = i + length;
        if (j >= line.size() || !std::isdigit(static_cast<unsigned char>(line[j])))
            continue;
        int value = 0;
        const size_t begin = j;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) {
            value = value * 10 + (line[j] - '0');
            ++j;
        }
        number_begin = begin;
        number_end = j;
        tool = value;
        return true;
    }
    return false;
}

} // namespace

bool remap_gcode_tools(const std::string& path, const std::vector<int>& slot_for_tool)
{
    if (path.empty() || slot_for_tool.empty())
        return true;
    std::string data;
    {
        boost::nowide::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        data = buffer.str();
    }
    const size_t probe = std::min<size_t>(data.size(), 1024);
    if (std::find(data.begin(), data.begin() + probe, '\0') != data.begin() + probe)
        return false;

    const std::string newline = data.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string rewritten;
    rewritten.reserve(data.size() + 32);
    bool found_toolchange = false;
    size_t cursor = 0;
    while (cursor < data.size()) {
        size_t end = data.find('\n', cursor);
        if (end == std::string::npos)
            end = data.size();
        std::string line = data.substr(cursor, end - cursor);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t begin = 0;
        size_t number_end = 0;
        int tool = 0;
        if (tool_command(line, begin, number_end, tool)) {
            found_toolchange = true;
            if (tool >= 0 && size_t(tool) < slot_for_tool.size() && slot_for_tool[size_t(tool)] >= 0)
                line.replace(begin, number_end - begin, std::to_string(slot_for_tool[size_t(tool)]));
        }
        rewritten += line;
        if (end < data.size())
            rewritten += newline;
        cursor = end < data.size() ? end + 1 : data.size();
    }

    // Slot 1 is T0. Leaving that command out makes the Kobra S1 parse the file as a
    // single-color job and reject it. Later slots already got Tn, which is why slot 4 worked.
    if (!found_toolchange && !slot_for_tool.empty() && slot_for_tool.front() >= 0) {
        const std::string insert = "T" + std::to_string(slot_for_tool.front()) + " ; multi-color slot" + newline;
        // A tool change at the top of the file is an extrusion. The Kobra S1 then
        // reports 10539 because the nozzle heat command has not been seen yet.
        auto skip_ws = [](const std::string& line) {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            return i;
        };
        auto boundary = [](char c) {
            return !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        };
        auto starts_with = [&](const std::string& line, const char* word) {
            const size_t i = skip_ws(line);
            const size_t n = std::strlen(word);
            return line.compare(i, n, word) == 0 && (i + n >= line.size() || boundary(line[i + n]));
        };
        auto has_extrusion = [&](const std::string& line) {
            const size_t i = skip_ws(line);
            for (size_t k = i; k < line.size() && line[k] != ';'; ++k) {
                if ((line[k] == 'E' || line[k] == 'e') && (k == i || boundary(line[k - 1])))
                    return true;
            }
            return false;
        };
        size_t insert_at = rewritten.size();
        size_t after_heat = std::string::npos;
        size_t scan = 0;
        while (scan < rewritten.size()) {
            size_t end = rewritten.find('\n', scan);
            if (end == std::string::npos)
                end = rewritten.size();
            std::string line = rewritten.substr(scan, end - scan);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t first = skip_ws(line);
            if (first < line.size() && line[first] != ';') {
                if (starts_with(line, "M104") || starts_with(line, "M109") || starts_with(line, "G9111"))
                    after_heat = end < rewritten.size() ? end + 1 : rewritten.size();
                else if ((starts_with(line, "G0") || starts_with(line, "G1") || starts_with(line, "G2") || starts_with(line, "G3")) && has_extrusion(line)) {
                    insert_at = after_heat != std::string::npos ? after_heat : scan;
                    break;
                }
            }
            scan = end < rewritten.size() ? end + 1 : rewritten.size();
        }
        if (insert_at == rewritten.size() && after_heat != std::string::npos)
            insert_at = after_heat;
        rewritten.insert(insert_at, insert);
    }

    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << rewritten;
    return bool(out);
}

}}
