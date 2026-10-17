///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "DevicesPanel.hpp"

#include "DashboardSite.hpp"
#include "FilamentRack.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "../Utils/Anycubic.hpp"
#include "../Utils/PrintHost.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/image.h>
#include <wx/dialog.h>
#include <wx/graphics.h>
#include <wx/wrapsizer.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/datetime.h>
#include <wx/stattext.h>
#include <wx/scrolwin.h>
#include <wx/dnd.h>
#include <wx/filename.h>
#include <wx/statbmp.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>

namespace Slic3r {
namespace GUI {

class DeviceTabBar : public wxPanel
{
public:
    DeviceTabBar(wxWindow* parent, std::vector<wxString> labels, std::function<void(int)> on_select)
        : wxPanel(parent)
        , m_labels(std::move(labels))
        , m_on_select(std::move(on_select))
    {
        SetBackgroundColour(wxGetApp().get_window_default_clr());
        SetFont(wxGetApp().small_font());
        Bind(wxEVT_PAINT, &DeviceTabBar::on_paint, this);
        Bind(wxEVT_LEFT_UP, &DeviceTabBar::on_click, this);
        Bind(wxEVT_MOTION, &DeviceTabBar::on_motion, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            if (m_hover != -1) {
                m_hover = -1;
                Refresh();
            }
            event.Skip();
        });
        layout();
    }

    void set_active(int index)
    {
        if (index == m_active)
            return;
        m_active = index;
        Refresh();
    }

private:
    void layout()
    {
        const int pad_x = 12;
        const int pad_y = 7;
        m_cells.clear();
        int x = 0;
        int height = 0;
        for (const wxString& label : m_labels) {
            const wxSize text = GetTextExtent(label);
            const int w = text.x + pad_x * 2;
            const int h = text.y + pad_y * 2 + 3;
            m_cells.push_back(wxRect(x, 0, w, h));
            x += w;
            height = std::max(height, h);
        }
        const wxSize wanted(std::max(1, x), std::max(1, height));
        if (GetMinSize() != wanted)
            SetMinSize(wanted);
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBrush(GetBackgroundColour());
        dc.SetPen(GetBackgroundColour());
        dc.DrawRectangle(0, 0, size.x, size.y);
        dc.SetFont(GetFont());
        const wxColour plain = wxGetApp().get_label_clr_default();
        const wxColour accent(254, 50, 173);
        for (size_t i = 0; i < m_labels.size() && i < m_cells.size(); ++i) {
            const wxRect& rect = m_cells[i];
            const bool active = int(i) == m_active;
            const bool hover = int(i) == m_hover;
            if (active || hover) {
                dc.SetBrush(active ? wxColour(47, 47, 56) : wxColour(60, 60, 68));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(rect);
            }
            dc.SetTextForeground(active ? *wxWHITE : plain);
            const wxSize text = dc.GetTextExtent(m_labels[i]);
            dc.DrawText(m_labels[i], rect.x + (rect.width - text.x) / 2, rect.y + 7);
            if (active) {
                dc.SetPen(wxPen(accent, 2));
                dc.DrawLine(rect.x + 10, rect.GetBottom() - 1, rect.GetRight() - 10, rect.GetBottom() - 1);
            }
        }
    }

    void on_click(wxMouseEvent& event)
    {
        for (size_t i = 0; i < m_cells.size(); ++i) {
            if (m_cells[i].Contains(event.GetPosition())) {
                set_active(int(i));
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
            if (m_cells[i].Contains(event.GetPosition())) {
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
    std::vector<wxRect>   m_cells;
    std::function<void(int)> m_on_select;
    int m_active { 0 };
    int m_hover { -1 };
};

namespace {

const wxColour k_card(48, 48, 60);
const wxColour k_card_edge(86, 86, 102);
const wxColour k_muted(168, 168, 176);
const wxColour k_track(36, 36, 46);

// Paint into a memory bitmap, then copy it. A graphics context built straight
// from the paint DC of a scrolled page is shifted, so borders look dashed and
// hit tests miss the picture.
template <typename Draw>
void paint_buffered(wxWindow* window, wxPaintDC& window_dc, Draw draw)
{
    const wxSize size = window->GetClientSize();
    if (size.x < 1 || size.y < 1)
        return;
    wxBitmap bitmap(size.x, size.y, wxBITMAP_SCREEN_DEPTH);
    {
        wxMemoryDC memory(bitmap);
        memory.SetBackground(wxBrush(window->GetBackgroundColour()));
        memory.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(memory));
        if (gc)
            draw(*gc, size);
    }
    // A paint DC inside a scrolled page can carry a shifted origin. Draw in
    // client coordinates so the new edge lands on the pixels that just appeared.
    const wxPoint origin = window_dc.GetDeviceOrigin();
    window_dc.SetDeviceOrigin(0, 0);
    window_dc.DrawBitmap(bitmap, 0, 0, false);
    window_dc.SetDeviceOrigin(origin.x, origin.y);
}

void bind_card_chrome(wxWindow* window)
{
    // Clip children out of this paint. A full-card blit otherwise covers the
    // contents for a frame on every resize, which is the flicker.
    window->SetWindowStyleFlag(window->GetWindowStyleFlag() | wxFULL_REPAINT_ON_RESIZE | wxCLIP_CHILDREN);
    window->SetBackgroundStyle(wxBG_STYLE_PAINT);
    window->SetBackgroundColour(k_card);
    window->Bind(wxEVT_PAINT, [window](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(window);
        const wxSize size = window->GetClientSize();
        if (size.x < 1 || size.y < 1)
            return;
        const wxColour page = wxGetApp().get_window_default_clr();
        dc.SetBrush(wxBrush(page));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, size.x, size.y);
        dc.SetBrush(wxBrush(k_card));
        dc.SetPen(wxPen(k_card_edge, 2));
        dc.DrawRoundedRectangle(2, 2, std::max(1, size.x - 4), std::max(1, size.y - 4), 12);
    });
}

wxPanel* make_card(wxWindow* parent, const wxString& title)
{
    auto* card = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
    bind_card_chrome(card);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (!title.empty()) {
        auto* heading = new wxStaticText(card, wxID_ANY, title);
        heading->SetFont(wxGetApp().bold_font());
        heading->SetForegroundColour(*wxWHITE);
        heading->SetBackgroundColour(k_card);
        sizer->Add(heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }
    card->SetSizer(sizer);
    return card;
}

void card_add(wxPanel* card, wxWindow* child, int flags = wxEXPAND)
{
    card->GetSizer()->Add(child, 0, flags | wxLEFT | wxRIGHT | wxTOP, 8);
}

wxStaticText* muted_line(wxWindow* parent, const wxString& text)
{
    auto* line = new wxStaticText(parent, wxID_ANY, text);
    line->SetForegroundColour(k_muted);
    line->SetBackgroundColour(parent->GetBackgroundColour());
    return line;
}

void fit_card_text(wxStaticText* text)
{
    if (!text)
        return;
    // Wrap() used to lock the note to the first narrow guess, so a wide card
    // still drew the explanation as a thin column down the left side.
    text->SetWindowStyleFlag(text->GetWindowStyleFlag() | wxST_NO_AUTORESIZE);
    auto source = std::make_shared<wxString>(text->GetLabel());
    auto applied = std::make_shared<int>(0);
    auto busy = std::make_shared<bool>(false);
    text->Wrap(420);
    // Width stays small. The wrapped text used to report its 420px line as the
    // minimum, which pushed the next module past the window edge.
    text->SetMinSize(wxSize(40, text->GetBestSize().GetHeight()));
    auto rewrap = [text, source, applied, busy]() {
        if (*busy)
            return;
        const int width = text->GetClientSize().GetWidth();
        // A scrollbar is about 17px. Ignore that, or the note and the scrollbar chase each other.
        if (width < 160 || std::abs(width - *applied) < 24)
            return;
        *applied = width;
        text->SetLabel(*source);
        text->Wrap(width);
        text->SetMinSize(wxSize(40, text->GetBestSize().GetHeight()));
        text->InvalidateBestSize();
        wxWindow* card = text->GetParent();
        if (!card)
            return;
        *busy = true;
        card->Layout();
        if (wxWindow* page = card->GetParent())
            page->Layout();
        *busy = false;
    };
    text->Bind(wxEVT_SIZE, [rewrap](wxSizeEvent& event) {
        event.Skip();
        rewrap();
    });
    text->CallAfter(rewrap);
}

wxPanel* make_placeholder(wxWindow* parent, const wxString& title, const wxString& body)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
    bind_card_chrome(panel);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (!title.empty()) {
        auto* heading = new wxStaticText(panel, wxID_ANY, title);
        heading->SetFont(wxGetApp().bold_font());
        heading->SetForegroundColour(*wxWHITE);
        heading->SetBackgroundColour(k_card);
        sizer->Add(heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);
    }
    auto* text = new wxStaticText(panel, wxID_ANY, body);
    text->SetForegroundColour(k_muted);
    text->SetBackgroundColour(k_card);
    fit_card_text(text);
    sizer->Add(text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    sizer->AddSpacer(16);
    panel->SetSizer(sizer);
    return panel;
}

std::string device_display_name(const DeviceEntry& device)
{
    std::string display = device.name;
    if (device.brand.empty())
        return display;
    const std::string twice = device.brand + " " + device.brand;
    if (display.size() < twice.size())
        return display;
    for (size_t i = 0; i < twice.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(display[i]);
        unsigned char b = static_cast<unsigned char>(twice[i]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return display;
    }
    return device.brand + display.substr(twice.size());
}

wxString device_type_text(const DeviceEntry& device)
{
    if (!device.model.empty())
        return from_u8(device.model);
    if (!device.brand.empty())
        return from_u8(device.brand);
    if (!device.link.empty())
        return from_u8(device.link);
    return _L("Printer");
}

wxString status_label(const std::string& status)
{
    if (status == "Connected" || status == "Ready")
        return _L("Connected");
    if (status == "Checking")
        return _L("Checking…");
    if (status == "Printing")
        return _L("Printing");
    if (status == "Failed")
        return _L("Could not connect");
    if (status == "Idle")
        return _L("Not checked");
    return _L("Offline");
}

wxString list_status_label(const std::string& status)
{
    if (status == "Connected" || status == "Ready")
        return _L("Free");
    return status_label(status);
}

wxColour status_colour(const std::string& status, bool linked)
{
    if (status == "Connected" || status == "Ready")
        return wxColour(80, 190, 120);
    if (status == "Checking" || status == "Printing")
        return wxColour(254, 50, 173);
    if (status == "Offline")
        return wxColour(232, 176, 74);
    if (status == "Failed" || !linked)
        return wxColour(220, 90, 90);
    return wxColour(168, 168, 176);
}

std::string safe_printer_text(std::string text)
{
    const char* secrets[] = {
        "BEGIN PRIVATE KEY",
        "BEGIN RSA PRIVATE KEY",
        "devicepk",
        "devicecrt",
    };
    for (const char* secret : secrets) {
        if (text.find(secret) != std::string::npos)
            return "The printer answered, but the reply contained a key or certificate, so it is not shown.";
    }
    constexpr size_t limit = 500;
    if (text.size() > limit)
        text.resize(limit);
    return text;
}

std::string printer_reported_name(const std::string& text)
{
    if (text.empty() || text.size() > 80 || text.find('\n') != std::string::npos)
        return {};
    const char* blocked[] = { "BEGIN", "PRIVATE", "token", "password", "devicepk", "devicecrt" };
    for (const char* word : blocked)
        if (text.find(word) != std::string::npos)
            return {};
    for (unsigned char c : text)
        if (c < 32 || c > 126)
            return {};
    return text;
}

void finish_card(wxPanel* card)
{
    card->GetSizer()->AddSpacer(16);
}

wxStaticText* add_reading(wxPanel* card, wxSizer* grid, const wxString& name, const wxString& value)
{
    auto* cell = new wxPanel(card);
    cell->SetBackgroundColour(wxColour(38, 38, 46));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* title = muted_line(cell, name);
    title->SetBackgroundColour(wxColour(38, 38, 46));
    sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 8);
    auto* number = new wxStaticText(cell, wxID_ANY, value);
    number->SetForegroundColour(*wxWHITE);
    number->SetBackgroundColour(wxColour(38, 38, 46));
    number->SetFont(wxGetApp().bold_font());
    sizer->Add(number, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
    cell->SetSizer(sizer);
    grid->Add(cell, 1, wxEXPAND | wxALL, 3);
    return number;
}

class SpoolView : public wxPanel
{
public:
    SpoolView(wxWindow* parent, const RackSlot& slot, const wxString& badge, std::function<void()> on_edit)
        : wxPanel(parent)
        , m_material(slot.material.empty() ? wxString("/") : from_u8(slot.material))
        , m_badge(badge)
        , m_filled(!slot.color.empty() || !slot.material.empty())
        , m_on_edit(std::move(on_edit))
    {
        int red = 214, green = 214, blue = 218;
        rack_color_rgb(slot.color, red, green, blue);
        m_color = wxColour(red, green, blue);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(parent->GetBackgroundColour());
        SetInitialSize(wxSize(76, 118));
        SetMinSize(wxSize(24, 72));
        SetToolTip(_L("Set the color and material loaded in this spool."));
        Bind(wxEVT_PAINT, &SpoolView::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_on_edit)
                m_on_edit();
        });
    }

    void set_on_edit(std::function<void()> on_edit) { m_on_edit = std::move(on_edit); }

    void set_slot(const RackSlot& slot, const wxString& badge)
    {
        m_material = slot.material.empty() ? wxString("/") : from_u8(slot.material);
        m_badge = badge;
        m_filled = !slot.color.empty() || !slot.material.empty();
        int red = 214, green = 214, blue = 218;
        rack_color_rgb(slot.color, red, green, blue);
        m_color = wxColour(red, green, blue);
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        paint_buffered(this, dc, [&](wxGraphicsContext& context, const wxSize& size) {
        wxGraphicsContext* gc = &context;

        const double scale = std::min(size.x / 76.0, size.y / 118.0);
        gc->PushState();
        gc->Translate((size.x - 76.0 * scale) / 2.0, (size.y - 118.0 * scale) / 2.0);
        gc->Scale(scale, scale);
        const double cx = 38;
        const double spool_y = 8;
        const double spool_h = 46;
        const wxColour flange(186, 186, 192);
        const wxColour flange_edge(150, 150, 158);
        gc->SetBrush(wxBrush(flange));
        gc->SetPen(wxPen(flange_edge, 1));
        gc->DrawEllipse(cx - 30, spool_y, 16, spool_h);
        gc->DrawEllipse(cx + 14, spool_y, 16, spool_h);
        gc->SetBrush(wxBrush(m_color));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(cx - 18, spool_y + 8, 36, spool_h - 16, 4);
        gc->SetBrush(wxBrush(wxColour(70, 70, 76)));
        gc->DrawEllipse(cx - 5, spool_y + spool_h / 2.0 - 5, 10, 10);

        gc->SetFont(wxGetApp().normal_font(), m_filled ? *wxWHITE : k_muted);
        double text_w = 0, text_h = 0;
        gc->GetTextExtent(m_material, &text_w, &text_h);
        gc->DrawText(m_material, std::max(0.0, cx - text_w / 2.0), spool_y + spool_h + 6);

        const double badge = 22;
        const double badge_x = cx - badge / 2.0;
        const double badge_y = spool_y + spool_h + 8 + text_h + 4;
        gc->SetBrush(wxBrush(wxColour(48, 48, 56)));
        gc->DrawEllipse(badge_x, badge_y, badge, badge);
        gc->SetFont(wxGetApp().small_font(), *wxWHITE);
        double badge_w = 0, badge_h = 0;
        gc->GetTextExtent(m_badge, &badge_w, &badge_h);
        gc->DrawText(m_badge, badge_x + (badge - badge_w) / 2.0, badge_y + (badge - badge_h) / 2.0);
        gc->PopState();
        });
    }

    wxString m_material;
    wxString m_badge;
    wxColour m_color;
    bool     m_filled { false };
    std::function<void()> m_on_edit;
};

class DryerSwitch : public wxPanel
{
public:
    DryerSwitch(wxWindow* parent, bool on, std::function<void(bool)> on_toggle)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(46, 26))
        , m_on(on)
        , m_on_toggle(std::move(on_toggle))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(46, 26));
        SetBackgroundColour(parent->GetBackgroundColour());
        SetToolTip(_L("Remember whether this ACE should be drying. This does not switch the printer."));
        Bind(wxEVT_PAINT, &DryerSwitch::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_on_toggle)
                m_on_toggle(!m_on);
        });
    }

    void set_on(bool on)
    {
        if (m_on == on)
            return;
        m_on = on;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        paint_buffered(this, dc, [&](wxGraphicsContext& context, const wxSize& size) {
            context.SetBrush(GetBackgroundColour());
            context.SetPen(*wxTRANSPARENT_PEN);
            context.DrawRectangle(0, 0, size.x, size.y);
            context.SetBrush(wxBrush(m_on ? wxColour(210, 210, 216) : wxColour(58, 58, 66)));
            context.DrawRoundedRectangle(0, 2, 46, 22, 11);
            context.SetBrush(wxBrush(*wxWHITE));
            const double knob = m_on ? 24 : 2;
            context.DrawEllipse(knob, 4, 18, 18);
        });
    }

    bool m_on { false };
    std::function<void(bool)> m_on_toggle;
};

struct DryPreset
{
    const char* name;
    int         celsius;
    int         hours;
};

// PLA matches the Anycubic drying dialog: 50°C for 8 hours.
// The other rows follow Anycubic's published dryer temperatures.
const DryPreset k_dry_presets[] = {
    { "PLA", 50, 8 },
    { "PLA-CF", 55, 8 },
    { "PETG", 60, 8 },
    { "PETG-CF", 65, 8 },
    { "TPU", 55, 8 },
    { "ASA", 65, 8 },
    { "ABS", 65, 8 },
    { "PVA", 50, 12 },
    { "PA", 70, 12 },
    { "PA6-CF", 70, 12 },
    { "PET-CF", 65, 8 },
    { "PC", 80, 8 },
    { "PC-CF", 80, 8 },
    { "PC-GF", 80, 8 },
    { "PAHT-CF", 80, 12 },
};

wxString dry_temp_text(int celsius)
{
    return wxString::Format(wxT("%d\u00B0C"), celsius);
}

wxString dry_span_text(int hours)
{
    return wxString::Format("%dh", hours);
}

wxString dry_remaining_text(const MaterialBay& bay)
{
    int seconds = bay.dry_hours > 0 ? bay.dry_hours * 3600 : 0;
    if (bay.dry_started > 0 && bay.dry_hours > 0) {
        const long long end = bay.dry_started + static_cast<long long>(bay.dry_hours) * 3600;
        const long long now = static_cast<long long>(std::time(nullptr));
        seconds = now >= end ? 0 : static_cast<int>(end - now);
    }
    const int hours = seconds / 3600;
    const int minutes = (seconds % 3600) / 60;
    return wxString::Format("%dh%dm", hours, minutes);
}

class DryingSettingsDialog : public wxDialog
{
public:
    DryingSettingsDialog(wxWindow* parent, const MaterialBay& bay)
        : wxDialog(parent, wxID_ANY, _L("Drying Settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxGetApp().UpdateDarkUI(this);
        SetBackgroundColour(wxColour(28, 28, 32));
        m_index = 0;
        for (int i = 0; i < int(sizeof(k_dry_presets) / sizeof(k_dry_presets[0])); ++i) {
            if (bay.dry_material == k_dry_presets[i].name) {
                m_index = i;
                break;
            }
        }

        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* heading = new wxBoxSizer(wxHORIZONTAL);
        auto* ace = new wxStaticText(this, wxID_ANY, _L("ACE Materials"));
        ace->SetForegroundColour(*wxWHITE);
        ace->SetFont(wxGetApp().bold_font());
        m_summary = new wxStaticText(this, wxID_ANY, wxString());
        m_summary->SetForegroundColour(wxColour(90, 160, 255));
        m_summary->SetFont(wxGetApp().bold_font());
        heading->Add(ace, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        heading->Add(m_summary, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(heading, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

        auto* chosen_row = new wxBoxSizer(wxHORIZONTAL);
        m_chosen = new wxButton(this, wxID_ANY, wxString());
        m_chosen->SetBackgroundColour(wxColour(27, 74, 140));
        m_chosen->SetForegroundColour(*wxWHITE);
        chosen_row->Add(m_chosen, 0, wxRIGHT, 8);
        root->Add(chosen_row, 0, wxLEFT | wxRIGHT | wxTOP, 16);

        auto* popular = new wxStaticText(this, wxID_ANY, _L("Popular Materials"));
        popular->SetForegroundColour(*wxWHITE);
        popular->SetFont(wxGetApp().bold_font());
        root->Add(popular, 0, wxLEFT | wxRIGHT | wxTOP, 16);

        auto* wrap = new wxWrapSizer(wxHORIZONTAL);
        for (int i = 0; i < int(sizeof(k_dry_presets) / sizeof(k_dry_presets[0])); ++i) {
            auto* chip = new wxButton(this, wxID_ANY, wxString::FromUTF8(k_dry_presets[i].name));
            chip->SetMinSize(wxSize(96, 36));
            const int index = i;
            chip->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { select(index); });
            m_chips.push_back(chip);
            wrap->Add(chip, 0, wxALL, 4);
        }
        root->Add(wrap, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        auto* footer = new wxPanel(this);
        footer->SetBackgroundColour(wxColour(42, 42, 48));
        auto* footer_row = new wxBoxSizer(wxHORIZONTAL);
        auto* temp_col = new wxBoxSizer(wxVERTICAL);
        auto* temp_label = new wxStaticText(footer, wxID_ANY, _L("Drying Temp"));
        temp_label->SetForegroundColour(k_muted);
        temp_label->SetBackgroundColour(footer->GetBackgroundColour());
        m_temp = new wxStaticText(footer, wxID_ANY, wxString());
        m_temp->SetForegroundColour(*wxWHITE);
        m_temp->SetBackgroundColour(footer->GetBackgroundColour());
        m_temp->SetFont(wxGetApp().bold_font());
        temp_col->Add(temp_label, 0, wxBOTTOM, 4);
        temp_col->Add(m_temp, 0);
        auto* time_col = new wxBoxSizer(wxVERTICAL);
        auto* time_label = new wxStaticText(footer, wxID_ANY, _L("Drying Time"));
        time_label->SetForegroundColour(k_muted);
        time_label->SetBackgroundColour(footer->GetBackgroundColour());
        m_time = new wxStaticText(footer, wxID_ANY, wxString());
        m_time->SetForegroundColour(*wxWHITE);
        m_time->SetBackgroundColour(footer->GetBackgroundColour());
        m_time->SetFont(wxGetApp().bold_font());
        time_col->Add(time_label, 0, wxBOTTOM, 4);
        time_col->Add(m_time, 0);
        footer_row->Add(temp_col, 1, wxEXPAND | wxALL, 12);
        footer_row->Add(time_col, 1, wxEXPAND | wxALL, 12);
        footer->SetSizer(footer_row);
        root->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        auto* save = new wxButton(this, wxID_OK, _L("Save"));
        wxGetApp().UpdateDarkUI(cancel, true);
        wxGetApp().UpdateDarkUI(save, true);
        save->SetBackgroundColour(wxColour(37, 120, 255));
        save->SetForegroundColour(*wxWHITE);
        buttons->AddStretchSpacer(1);
        buttons->Add(cancel, 0, wxRIGHT, 8);
        buttons->Add(save, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, 16);

        SetSizer(root);
        select(m_index);
        root->Fit(this);
        SetMinSize(wxSize(460, -1));
        CentreOnParent();
    }

    void apply(MaterialBay& bay) const
    {
        const DryPreset& preset = k_dry_presets[m_index];
        bay.dry_material = preset.name;
        bay.dry_celsius = preset.celsius;
        bay.dry_hours = preset.hours;
    }

private:
    void select(int index)
    {
        m_index = index;
        const DryPreset& preset = k_dry_presets[index];
        const wxString name = wxString::FromUTF8(preset.name);
        m_chosen->SetLabel(name);
        m_summary->SetLabel(dry_temp_text(preset.celsius) + "/" + dry_span_text(preset.hours));
        m_temp->SetLabel(wxString(L"\u2014/") + dry_temp_text(preset.celsius));
        m_time->SetLabel(dry_span_text(preset.hours));
        for (int i = 0; i < int(m_chips.size()); ++i) {
            const bool chosen = i == index;
            m_chips[size_t(i)]->SetBackgroundColour(chosen ? wxColour(27, 74, 140) : wxColour(54, 54, 62));
            m_chips[size_t(i)]->SetForegroundColour(*wxWHITE);
        }
        Layout();
    }

    int m_index { 0 };
    wxButton* m_chosen { nullptr };
    wxStaticText* m_summary { nullptr };
    wxStaticText* m_temp { nullptr };
    wxStaticText* m_time { nullptr };
    std::vector<wxButton*> m_chips;
};

bool edit_drying_settings(wxWindow* parent, MaterialBay& bay)
{
    DryingSettingsDialog dialog(parent, bay);
    if (dialog.ShowModal() != wxID_OK)
        return false;
    dialog.apply(bay);
    return true;
}

void post_printer(const std::string& host, const std::shared_ptr<bool>& alive, std::function<bool(Anycubic&, std::string&)> work, std::function<void(bool, const std::string&)> done);

struct CameraSession
{
    bool running { false };
    bool light { false };
    bool light_known { false };
    std::shared_ptr<AnycubicPreview> preview;
    std::map<int, std::function<void()>> watchers;
};

struct ControlReading
{
    wxString nozzle;
    wxString bed;
    wxString chamber;
    wxString model_fan;
    wxString aux_fan;
    wxString light;
    // How many ACE units the printer reported. Below 1 means it has not answered yet.
    int ace_units { 0 };
    bool job_known { false };
    bool printing { false };
    wxString job_name;
    int progress { -1 };
    int layer { 0 };
    int layers { 0 };
    int remain_minutes { -1 };
    int elapsed_minutes { -1 };
    // True when the job was started from an export, not reported by the printer.
    bool local { false };
};

std::map<std::string, ControlReading>& control_readings();
std::map<std::string, CameraSession>& camera_sessions();
int next_camera_watch();

wxPanel* fill_material_card(wxWindow* parent, const DeviceEntry& device, bool titled = true)
{
    if (device.resin) {
        auto* material = make_card(parent, titled ? wxString(_L("Resin")) : wxString());
        wxString resin_name = _L("Choose a resin in the material list.");
        if (wxGetApp().preset_bundle) {
            const Preset& selected = wxGetApp().preset_bundle->sla_materials.get_selected_preset();
            if (!selected.name.empty() && !selected.is_default)
                resin_name = from_u8(selected.name);
        }
        auto* line = new wxStaticText(material, wxID_ANY, resin_name);
        line->SetForegroundColour(*wxWHITE);
        line->SetBackgroundColour(k_card);
        line->SetFont(wxGetApp().bold_font());
        card_add(material, line);
        auto* note = muted_line(material, _L("One resin sits in the vat. This printer has no filament spools."));
        fit_card_text(note);
        card_add(material, note);
        finish_card(material);
        return material;
    }
    auto* material = make_card(parent, titled ? wxString(_L("Material")) : wxString());
    auto state = std::make_shared<FilamentRack>(load_filament_rack(device.id));
    apply_remembered_ace(device.id, *state);
    const std::string printer_name = device.id;
    const std::string printer_host = device.host;
    const bool ace_link = device.link == "Anycubic LAN";

    // ACE1 is shown until the printer says how many units are connected.
    // ACE2 appears only after a second unit answers. The external rack is always a tab.
    auto ace_units = std::make_shared<int>(1);
    if (ace_link) {
        const int known = control_readings()[printer_name].ace_units;
        if (known > 0)
            *ace_units = known;
    }
    auto order = std::make_shared<std::vector<int>>();
    auto fill_order = [=]() {
        order->clear();
        if (state->anycubic_ace) {
            const int units = std::max(1, *ace_units);
            int rack = -1;
            int ace1 = -1;
            int ace2 = -1;
            for (int i = 0; i < int(state->bays.size()); ++i) {
                const std::string& key = state->bays[size_t(i)].key;
                if (key == "rack")
                    rack = i;
                else if (key == "ace1")
                    ace1 = i;
                else if (key == "ace2")
                    ace2 = i;
            }
            if (rack >= 0)
                order->push_back(rack);
            if (ace1 >= 0 && units >= 1)
                order->push_back(ace1);
            if (ace2 >= 0 && units >= 2)
                order->push_back(ace2);
        }
        if (order->empty()) {
            for (int i = 0; i < int(state->bays.size()); ++i)
                order->push_back(i);
        }
    };
    fill_order();

    auto selected = std::make_shared<int>(0);
    for (int i = 0; i < int(order->size()); ++i) {
        if (state->bays[size_t((*order)[size_t(i)])].key == "ace1")
            *selected = i;
    }

    auto* body = new wxPanel(material);
    body->SetBackgroundColour(k_card);
    auto* body_sizer = new wxBoxSizer(wxVERTICAL);
    body->SetSizer(body_sizer);

    auto* clock = new wxTimer();
    clock->SetOwner(material);
    auto editing = std::make_shared<bool>(false);
    auto alive = std::make_shared<bool>(true);
    material->Bind(wxEVT_DESTROY, [clock, alive](wxWindowDestroyEvent& event) {
        *alive = false;
        clock->Stop();
        delete clock;
        event.Skip();
    });

    struct BayPage {
        int bay_index { -1 };
        wxPanel* panel { nullptr };
        std::vector<SpoolView*> spools;
        DryerSwitch* toggle { nullptr };
        wxPanel* pill { nullptr };
        wxStaticText* pill_temp { nullptr };
        wxPanel* status { nullptr };
        wxStaticText* status_temp { nullptr };
        wxStaticText* status_time { nullptr };
    };
    auto bay_pages = std::make_shared<std::vector<BayPage>>();
    auto* pages_host = new wxPanel(body);
    pages_host->SetBackgroundColour(k_card);
    auto* pages_sizer = new wxBoxSizer(wxVERTICAL);
    pages_host->SetSizer(pages_sizer);
    body_sizer->Add(pages_host, 0, wxEXPAND);

    auto show_bay = std::make_shared<std::function<void()>>();
    material->Bind(wxEVT_TIMER, [=](wxTimerEvent& event) {
        if (event.GetId() != clock->GetId() || !show_bay || !*show_bay || *editing)
            return;
        (*show_bay)();
    }, clock->GetId());
    *show_bay = [=]() {
        // Pages stay alive. Destroying them on a tab click or after Save
        // frees a window the click is still using and corrupts the heap.
        auto badge_for = [=](int slot_index) {
            return state->multi_color_unit
                ? wxString::Format("%d", slot_index + 1)
                : wxString::Format("E%d", slot_index + 1);
        };
        while (bay_pages->size() < order->size()) {
            const int bay_index = (*order)[bay_pages->size()];
            MaterialBay& bay = state->bays[size_t(bay_index)];
            BayPage page;
            page.bay_index = bay_index;
            page.panel = new wxPanel(pages_host);
            page.panel->SetBackgroundColour(k_card);
            auto* page_sizer = new wxBoxSizer(wxVERTICAL);
            page.panel->SetSizer(page_sizer);
            auto* row_host = new wxPanel(page.panel);
        row_host->SetBackgroundColour(k_card);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < bay.slots.size(); ++i) {
            const int slot_index = int(i);
            const wxString badge = state->multi_color_unit
                ? wxString::Format("%d", int(i) + 1)
                : wxString::Format("E%d", int(i) + 1);
            auto* spool = new SpoolView(row_host, bay.slots[i], badge, {});
            spool->set_on_edit([=]() {
                if (size_t(bay_index) >= state->bays.size())
                    return;
                MaterialBay& current = state->bays[size_t(bay_index)];
                if (slot_index < 0 || size_t(slot_index) >= current.slots.size())
                    return;
                *editing = true;
                const bool accepted = edit_rack_slot(body, current.slots[size_t(slot_index)], slot_index, state->multi_color_unit);
                *editing = false;
                if (!accepted)
                    return;
                try {
                    save_filament_rack(printer_name, *state);
                } catch (const std::exception& ex) {
                    wxMessageDialog(wxGetApp().GetTopWindow(), from_u8(ex.what()), _L("Material"), wxOK | wxICON_ERROR).ShowModal();
                    return;
                }
                const std::string bay_key = current.key;
                int box = -1;
                if (bay_key == "ace1")
                    box = 0;
                else if (bay_key == "ace2")
                    box = 1;
                if (ace_link && !printer_host.empty() && box >= 0) {
                    const RackSlot saved = current.slots[size_t(slot_index)];
                    int red = 0, green = 0, blue = 0;
                    rack_color_rgb(saved.color, red, green, blue);
                    const bool empty = saved.color.empty() && saved.material.empty();
                    const int slot_number = slot_index;
                    const std::string material_name = saved.material;
                    post_printer(printer_host, alive,
                        [=](Anycubic& printer, std::string& error) {
                            return printer.set_ace_slot(box, slot_number, red, green, blue, material_name, empty, error);
                        },
                        [](bool ok, const std::string& error) {
                            if (ok)
                                return;
                            wxMessageDialog(wxGetApp().GetTopWindow(),
                                from_u8(error.empty() ? std::string("The ACE did not accept the spool change.") : error),
                                _L("ACE"), wxOK | wxICON_WARNING).ShowModal();
                        });
                }
                spool->set_slot(current.slots[size_t(slot_index)], badge_for(slot_index));
            });
            page.spools.push_back(spool);
            row->Add(spool, 1, wxEXPAND | wxRIGHT, 8);
        }
        row_host->SetSizer(row);
        row_host->SetMinSize(wxSize(48, 96));
        page_sizer->Add(row_host, 0, wxEXPAND | wxTOP, 4);

        if (bay.dryer) {
            auto open_settings = [=]() {
                MaterialBay& current = state->bays[size_t(bay_index)];
                *editing = true;
                const bool accepted = edit_drying_settings(body, current);
                *editing = false;
                if (!accepted)
                    return;
                if (current.drying)
                    current.dry_started = static_cast<long long>(std::time(nullptr));
                save_filament_rack(printer_name, *state);
                if (show_bay && *show_bay)
                    (*show_bay)();
            };

            auto* drying = new wxBoxSizer(wxHORIZONTAL);
            auto* label = new wxStaticText(page.panel, wxID_ANY, _L("Drying"));
            label->SetForegroundColour(*wxWHITE);
            label->SetBackgroundColour(k_card);
            label->SetFont(wxGetApp().bold_font());
            drying->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

            {
                auto* pill = new wxPanel(page.panel);
                pill->SetBackgroundColour(wxColour(196, 84, 28));
                pill->SetMinSize(wxSize(72, 26));
                auto* pill_sizer = new wxBoxSizer(wxHORIZONTAL);
                auto* temp = new wxStaticText(pill, wxID_ANY, dry_temp_text(bay.dry_celsius));
                temp->SetForegroundColour(wxColour(255, 210, 170));
                temp->SetBackgroundColour(pill->GetBackgroundColour());
                temp->SetFont(wxGetApp().bold_font());
                pill_sizer->Add(temp, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 10);
                pill->SetSizer(pill_sizer);
                pill->SetToolTip(_L("Target drying temperature. The printer has not reported the temperature inside the dryer, so the live number is not shown."));
                pill->Bind(wxEVT_LEFT_UP, [=](wxMouseEvent&) { open_settings(); });
                temp->Bind(wxEVT_LEFT_UP, [=](wxMouseEvent&) { open_settings(); });
                drying->Add(pill, 0, wxALIGN_CENTER_VERTICAL);
                page.pill = pill;
                page.pill_temp = temp;
                pill->Show(bay.drying && bay.dry_celsius >= 0);
            }
            drying->AddStretchSpacer(1);
            auto* toggle = new DryerSwitch(page.panel, bay.drying, [=](bool on) {
                MaterialBay& current = state->bays[size_t(bay_index)];
                if (on && (current.dry_material.empty() || current.dry_celsius < 0 || current.dry_hours <= 0)) {
                    *editing = true;
                    const bool accepted = edit_drying_settings(body, current);
                    *editing = false;
                    if (!accepted)
                        return;
                }
                current.drying = on;
                if (on)
                    current.dry_started = static_cast<long long>(std::time(nullptr));
                save_filament_rack(printer_name, *state);
                if (show_bay && *show_bay)
                    (*show_bay)();
            });
            drying->Add(toggle, 0, wxALIGN_CENTER_VERTICAL);
            page.toggle = toggle;
            page_sizer->Add(drying, 0, wxEXPAND | wxTOP, 12);

            {
                auto* status = new wxPanel(page.panel);
                status->SetBackgroundColour(wxColour(38, 38, 44));
                status->SetCursor(wxCursor(wxCURSOR_HAND));
                status->SetToolTip(_L("Change the drying material, temperature, and time."));
                auto* status_row = new wxBoxSizer(wxHORIZONTAL);
                auto* temp_col = new wxBoxSizer(wxVERTICAL);
                auto* temp_name = new wxStaticText(status, wxID_ANY, _L("Drying Temp"));
                temp_name->SetForegroundColour(k_muted);
                temp_name->SetBackgroundColour(status->GetBackgroundColour());
                auto* temp_value = new wxStaticText(status, wxID_ANY, wxString(L"\u2014/") + dry_temp_text(bay.dry_celsius));
                temp_value->SetForegroundColour(*wxWHITE);
                temp_value->SetBackgroundColour(status->GetBackgroundColour());
                temp_value->SetFont(wxGetApp().bold_font());
                temp_col->Add(temp_name, 0, wxBOTTOM, 4);
                temp_col->Add(temp_value, 0);
                auto* time_col = new wxBoxSizer(wxVERTICAL);
                auto* time_name = new wxStaticText(status, wxID_ANY, _L("Remaining Drying Time"));
                time_name->SetForegroundColour(k_muted);
                time_name->SetBackgroundColour(status->GetBackgroundColour());
                auto* time_value = new wxStaticText(status, wxID_ANY, dry_remaining_text(bay));
                time_value->SetForegroundColour(*wxWHITE);
                time_value->SetBackgroundColour(status->GetBackgroundColour());
                time_value->SetFont(wxGetApp().bold_font());
                time_col->Add(time_name, 0, wxBOTTOM, 4);
                time_col->Add(time_value, 0);
                auto* more = new wxStaticText(status, wxID_ANY, L"\u203A");
                more->SetForegroundColour(k_muted);
                more->SetBackgroundColour(status->GetBackgroundColour());
                status_row->Add(temp_col, 1, wxEXPAND | wxALL, 10);
                status_row->Add(time_col, 1, wxEXPAND | wxALL, 10);
                status_row->Add(more, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
                status->SetSizer(status_row);
                auto clicked = [=](wxMouseEvent&) { open_settings(); };
                status->Bind(wxEVT_LEFT_UP, clicked);
                temp_name->Bind(wxEVT_LEFT_UP, clicked);
                temp_value->Bind(wxEVT_LEFT_UP, clicked);
                time_name->Bind(wxEVT_LEFT_UP, clicked);
                time_value->Bind(wxEVT_LEFT_UP, clicked);
                more->Bind(wxEVT_LEFT_UP, clicked);
                page_sizer->Add(status, 0, wxEXPAND | wxTOP, 8);
                page.status = status;
                page.status_temp = temp_value;
                page.status_time = time_value;
                status->Show(bay.drying && bay.dry_celsius >= 0 && bay.dry_hours > 0);
            }
        }

            pages_sizer->Add(page.panel, 0, wxEXPAND);
            bay_pages->push_back(page);
        }

        for (BayPage& page : *bay_pages) {
            if (page.bay_index < 0 || size_t(page.bay_index) >= state->bays.size())
                continue;
            const MaterialBay& bay = state->bays[size_t(page.bay_index)];
            for (size_t i = 0; i < page.spools.size() && i < bay.slots.size(); ++i)
                page.spools[i]->set_slot(bay.slots[i], badge_for(int(i)));
            if (page.toggle)
                page.toggle->set_on(bay.drying);
            const bool hot = bay.drying && bay.dry_celsius >= 0;
            if (page.pill) {
                page.pill->Show(hot);
                if (hot && page.pill_temp)
                    page.pill_temp->SetLabel(dry_temp_text(bay.dry_celsius));
            }
            const bool timed = hot && bay.dry_hours > 0;
            if (page.status) {
                page.status->Show(timed);
                if (timed && page.status_temp)
                    page.status_temp->SetLabel(wxString(L"\u2014/") + dry_temp_text(bay.dry_celsius));
                if (timed && page.status_time)
                    page.status_time->SetLabel(dry_remaining_text(bay));
            }
        }
        for (size_t i = 0; i < bay_pages->size(); ++i)
            pages_sizer->Show((*bay_pages)[i].panel, int(i) == *selected);

        bool drying_now = false;
        for (const MaterialBay& item : state->bays)
            if (item.drying)
                drying_now = true;
        if (drying_now && !clock->IsRunning())
            clock->Start(30000);
        else if (!drying_now && clock->IsRunning())
            clock->Stop();

        pages_host->Layout();
        body->Layout();
        material->Layout();
    };

    auto* tabs_host = new wxPanel(material);
    tabs_host->SetBackgroundColour(k_card);
    tabs_host->SetSizer(new wxBoxSizer(wxVERTICAL));
    auto refresh_tabs = std::make_shared<std::function<void()>>();
    auto tab_signature = std::make_shared<std::string>();
    *refresh_tabs = [=]() {
        fill_order();
        if (*selected < 0 || *selected >= int(order->size()))
            *selected = 0;
        std::string signature;
        if (!order->empty() && (state->anycubic_ace || order->size() > 1)) {
            for (int index : *order) {
                signature += state->bays[size_t(index)].title;
                signature.push_back('|');
            }
        }
        if (signature != *tab_signature) {
            *tab_signature = signature;
            wxSizer* tabs_sizer = tabs_host->GetSizer();
            // The tab bar is only replaced when the set of bays changes.
            // Switching tabs and saving a color leave it in place.
            std::vector<wxWindow*> old_tabs;
            for (wxSizerItem* item : tabs_sizer->GetChildren())
                if (item && item->GetWindow())
                    old_tabs.push_back(item->GetWindow());
            tabs_sizer->Clear(false);
            for (wxWindow* old : old_tabs) {
                tabs_host->RemoveChild(old);
                delete old;
            }
            if (!signature.empty()) {
                std::vector<wxString> labels;
                for (int index : *order)
                    labels.push_back(from_u8(state->bays[size_t(index)].title));
                auto* tabs = new DeviceTabBar(tabs_host, labels, [selected, show_bay, alive](int page) {
                    if (!alive || !*alive)
                        return;
                    *selected = page;
                    if (show_bay && *show_bay)
                        (*show_bay)();
                });
                if (*selected != 0)
                    tabs->set_active(*selected);
                tabs_sizer->Add(tabs, 0, wxEXPAND);
                tabs_host->Show();
            } else {
                tabs_host->Hide();
            }
            tabs_host->Layout();
        }
        if (show_bay && *show_bay)
            (*show_bay)();
    };
    card_add(material, tabs_host, wxEXPAND);
    const int material_watch = next_camera_watch();
    camera_sessions()[printer_name].watchers.emplace(material_watch, [=]() {
        const bool slots_changed = apply_remembered_ace(printer_name, *state);
        const int reported = control_readings()[printer_name].ace_units;
        const bool units_changed = reported >= 1 && reported != *ace_units;
        if (!slots_changed && !units_changed)
            return;
        if (units_changed)
            *ace_units = reported;
        if (!alive || !*alive || *editing)
            return;
        material->CallAfter([refresh_tabs, alive]() {
            if (!alive || !*alive || !refresh_tabs || !*refresh_tabs)
                return;
            (*refresh_tabs)();
        });
    });
    material->Bind(wxEVT_DESTROY, [printer_name, material_watch](wxWindowDestroyEvent& event) {
        const auto found = camera_sessions().find(printer_name);
        if (found != camera_sessions().end())
            found->second.watchers.erase(material_watch);
        event.Skip();
    });
    card_add(material, body);
    (*refresh_tabs)();

    const wxString material_note_text = state->anycubic_ace
        ? _L("Click a spool to say what is loaded. A slash means that spool is empty. Click the drying row to choose the material, temperature, and time.")
        : (state->multi_color_unit
            ? (titled
                ? _L("Click a spool to say what is loaded. These spools share one nozzle, so they are not extra extruders. Match this print chooses which spool feeds each color.")
                : _L("Click a spool to say what is loaded. These spools share one nozzle, so they are not extra extruders."))
            : (titled
                ? _L("Each extruder holds one filament. Click a spool to say what is loaded. Match this print chooses which extruder feeds each color.")
                : _L("Each extruder holds one filament. Click a spool to say what is loaded.")));
    auto* material_note = muted_line(material, material_note_text);
    fit_card_text(material_note);
    card_add(material, material_note);
    // Matching a print is a project action. The dashboard only shows what is loaded.
    if (titled) {
        auto* match = new wxButton(material, wxID_ANY, _L("Match this print"));
        wxGetApp().UpdateDarkUI(match, true);
        match->Bind(wxEVT_BUTTON, [material, printer_name](wxCommandEvent&) {
            std::vector<int> map;
            ask_filament_match(material, printer_name, map);
        });
        card_add(material, match);
    }
    finish_card(material);
    return material;
}

std::map<std::string, ControlReading>& control_readings()
{
    static std::map<std::string, ControlReading> readings;
    return readings;
}

std::map<std::string, CameraSession>& camera_sessions();

void notify_camera_watchers(const std::string& id)
{
    const auto found = camera_sessions().find(id);
    if (found == camera_sessions().end())
        return;
    const auto watchers = found->second.watchers;
    for (const auto& watcher : watchers)
        if (watcher.second)
            watcher.second();
}

int next_camera_watch()
{
    static int next = 1;
    return next++;
}

void post_printer(const std::string& host, const std::shared_ptr<bool>& alive, std::function<bool(Anycubic&, std::string&)> work, std::function<void(bool, const std::string&)> done)
{
    std::thread([host, alive, work = std::move(work), done = std::move(done)]() {
        std::string error;
        bool ok = false;
        try {
            DynamicPrintConfig config;
            config.set_key_value("print_host", new ConfigOptionString(host));
            Anycubic printer(&config);
            ok = work(printer, error);
        } catch (const std::exception& ex) {
            error = ex.what();
        }
        wxGetApp().CallAfter([alive, done = std::move(done), ok, error]() {
            if (alive && *alive)
                done(ok, error);
        });
    }).detach();
}

std::map<std::string, CameraSession>& camera_sessions()
{
    static std::map<std::string, CameraSession> sessions;
    return sessions;
}

bool camera_has_light(const DeviceEntry& device)
{
    return device.link == "Anycubic LAN";
}

class CameraIconButton : public wxPanel
{
public:
    enum Glyph { Bulb, Play, Stop, Corners, CornersIn };

    CameraIconButton(wxWindow* parent, Glyph glyph, const wxString& tip, std::function<void()> on_press)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(36, 36))
        , m_glyph(glyph)
        , m_on_press(std::move(on_press))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(16, 16, 20));
        SetMinSize(wxSize(36, 36));
        SetToolTip(tip);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &CameraIconButton::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
            m_hover = true;
            Refresh();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            m_hover = false;
            Refresh();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_enabled && m_on_press)
                m_on_press();
        });
    }

    void set_glyph(Glyph glyph)
    {
        if (m_glyph == glyph)
            return;
        m_glyph = glyph;
        Refresh();
    }

    void set_lit(bool lit)
    {
        if (m_lit == lit)
            return;
        m_lit = lit;
        Refresh();
    }

    void set_enabled(bool enabled)
    {
        if (m_enabled == enabled)
            return;
        m_enabled = enabled;
        SetCursor(wxCursor(enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        paint_buffered(this, dc, [&](wxGraphicsContext& gc, const wxSize& size) {
            const wxColour ink = m_enabled ? *wxWHITE : wxColour(120, 120, 128);
            const wxColour plate = m_hover && m_enabled ? wxColour(58, 58, 70) : wxColour(32, 32, 40);
            gc.SetPen(*wxTRANSPARENT_PEN);
            gc.SetBrush(wxBrush(plate));
            gc.DrawRoundedRectangle(1, 1, std::max(1, size.x - 2), std::max(1, size.y - 2), 8);

            if (m_glyph == Bulb)
                draw_bulb(gc, m_lit && m_enabled, ink);
            else if (m_glyph == Play)
                draw_play(gc, ink);
            else if (m_glyph == Stop)
                draw_stop(gc, ink);
            else
                draw_corners(gc, ink, m_glyph == CornersIn);
        });
    }

    static void draw_bulb(wxGraphicsContext& gc, bool on, const wxColour& ink)
    {
        const wxColour glow(255, 214, 90);
        gc.SetPen(wxPen(on ? glow : ink, 1.6));
        gc.SetBrush(on ? wxBrush(glow) : *wxTRANSPARENT_BRUSH);
        gc.DrawEllipse(11, 6, 14, 15);
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        gc.StrokeLine(14.5, 21, 14.5, 24);
        gc.StrokeLine(21.5, 21, 21.5, 24);
        gc.StrokeLine(14, 24, 22, 24);
        gc.StrokeLine(15, 27, 21, 27);
        if (!on)
            return;
        gc.SetPen(wxPen(glow, 1.4));
        gc.StrokeLine(18, 3, 18, 1);
        gc.StrokeLine(7, 9, 5, 7);
        gc.StrokeLine(29, 9, 31, 7);
    }

    static void draw_play(wxGraphicsContext& gc, const wxColour& ink)
    {
        wxGraphicsPath path = gc.CreatePath();
        path.MoveToPoint(13, 10);
        path.AddLineToPoint(13, 26);
        path.AddLineToPoint(26, 18);
        path.CloseSubpath();
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(ink));
        gc.FillPath(path);
    }

    static void draw_stop(wxGraphicsContext& gc, const wxColour& ink)
    {
        gc.SetPen(*wxTRANSPARENT_PEN);
        gc.SetBrush(wxBrush(ink));
        gc.DrawRoundedRectangle(12, 12, 12, 12, 2);
    }

    static void draw_corners(wxGraphicsContext& gc, const wxColour& ink, bool inward)
    {
        gc.SetPen(wxPen(ink, 1.8));
        gc.SetBrush(*wxTRANSPARENT_BRUSH);
        const double arm = 7;
        if (!inward) {
            gc.StrokeLine(9, 9 + arm, 9, 9);
            gc.StrokeLine(9, 9, 9 + arm, 9);
            gc.StrokeLine(27 - arm, 9, 27, 9);
            gc.StrokeLine(27, 9, 27, 9 + arm);
            gc.StrokeLine(9, 27 - arm, 9, 27);
            gc.StrokeLine(9, 27, 9 + arm, 27);
            gc.StrokeLine(27 - arm, 27, 27, 27);
            gc.StrokeLine(27, 27, 27, 27 - arm);
            return;
        }
        gc.StrokeLine(9, 15, 15, 15);
        gc.StrokeLine(15, 15, 15, 9);
        gc.StrokeLine(21, 9, 21, 15);
        gc.StrokeLine(21, 15, 27, 15);
        gc.StrokeLine(9, 21, 15, 21);
        gc.StrokeLine(15, 21, 15, 27);
        gc.StrokeLine(21, 27, 21, 21);
        gc.StrokeLine(21, 21, 27, 21);
    }

    Glyph m_glyph;
    std::function<void()> m_on_press;
    bool m_lit { false };
    bool m_hover { false };
    bool m_enabled { true };
};

class CameraView : public wxPanel
{
public:
    CameraView(wxWindow* parent, const DeviceEntry& device, int min_height, bool fullscreen, std::function<void()> changed)
        : wxPanel(parent)
        , m_id(device.id)
        , m_host(device.host)
        , m_linked(!device.host.empty())
        , m_anycubic(camera_has_light(device))
        , m_fullscreen(fullscreen)
        , m_changed(std::move(changed))
        , m_alive(std::make_shared<bool>(true))
    {
        SetWindowStyleFlag(GetWindowStyleFlag() | wxCLIP_CHILDREN);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(16, 16, 20));
        // Both axes are set. An unset width lets the placed buttons become the
        // minimum, and the card can then only grow.
        if (m_fullscreen)
            SetMinSize(wxSize(320, 240));
        else
            SetMinSize(wxSize(64, std::max(120, min_height > 0 ? min_height : 160)));

        // The controls are placed by hand. A sizer wraps the status text before
        // this view has a width, and that pushes Start and Fullscreen below the card.
        if (m_anycubic) {
            m_light = new CameraIconButton(this, CameraIconButton::Bulb, _L("Switch the chamber light on this printer."), [this]() { toggle_light(); });
            m_light->set_enabled(m_linked);
        }
        m_message = new wxStaticText(this, wxID_ANY, wxString());
        m_message->SetForegroundColour(k_muted);
        m_message->SetBackgroundColour(GetBackgroundColour());
        m_power = new CameraIconButton(this, CameraIconButton::Play, m_linked
            ? _L("Start or stop this printer's camera.")
            : _L("This printer has no network address, so the camera cannot be started."), [this]() { toggle_camera(); });
        m_power->set_enabled(m_linked);
        m_full = new CameraIconButton(this, m_fullscreen ? CameraIconButton::CornersIn : CameraIconButton::Corners,
            m_fullscreen ? _L("Close") : _L("Fullscreen"), [this]() { on_fullscreen(); });
        Bind(wxEVT_PAINT, &CameraView::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            event.Skip();
            layout_overlay();
            // Paint stretches the current frame. Rescaling a new bitmap on every
            // size step is what flashes the picture while the window is dragged.
            Refresh(false);
        });
        m_watch = next_camera_watch();
        camera_sessions()[m_id].watchers.emplace(m_watch, [this]() { sync(); });
        m_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { on_tick(); }, m_timer.GetId());
        if (camera_sessions()[m_id].running && camera_sessions()[m_id].preview)
            m_timer.Start(120);
        sync();
    }

    // Child buttons are placed at the current edges. Their positions must not
    // become this view's best size, or the card refuses to shrink.
    wxSize DoGetBestSize() const override
    {
        return GetMinSize();
    }

    void limit_height(int height)
    {
        if (m_fullscreen)
            return;
        height = std::max(120, height);
        const wxSize want(64, height);
        if (GetMinSize() == want)
            return;
        SetMinSize(want);
        InvalidateBestSize();
    }

    ~CameraView() override
    {
        *m_alive = false;
        m_timer.Stop();
        const auto found = camera_sessions().find(m_id);
        if (found != camera_sessions().end())
            found->second.watchers.erase(m_watch);
    }

    void sync()
    {
        const CameraSession& session = camera_sessions()[m_id];
        if (m_power && !m_busy)
            m_power->set_glyph(session.running ? CameraIconButton::Stop : CameraIconButton::Play);
        if (m_light && !m_busy)
            m_light->set_lit(session.light);
        wxString text;
        if (!m_note.empty())
            text = m_note;
        else if (!m_linked)
            text = _L("No camera. A network connection is required before a picture can appear.");
        else if (!m_anycubic)
            text = _L("This connection does not provide a camera picture.");
        else if (!session.running)
            text = _L("Camera is stopped. Press Start to ask this printer for a picture.");
        else if (!m_frame.IsOk())
            text = _L("Starting the camera. The first picture can take a few seconds.");
        else
            text.clear();
        if (m_message) {
            m_message->Show(!text.empty());
            m_message->SetLabel(text);
        }
        layout_overlay();
        Refresh(false);
    }

private:
    void layout_overlay()
    {
        const int width = std::max(1, GetClientSize().GetWidth());
        const int height = std::max(1, GetClientSize().GetHeight());
        auto place = [](wxWindow* button, int x, int y) {
            if (!button)
                return;
            button->SetSize(std::max(0, x), std::max(0, y), 36, 36);
            button->Raise();
        };
        if (m_light)
            place(m_light, width - 44, 8);
        place(m_power, 8, height - 44);
        place(m_full, width - 44, height - 44);
        if (m_message && m_message->IsShown()) {
            const int wrap = std::max(160, width - 32);
            if (wrap != m_wrap_width) {
                m_wrap_width = wrap;
                m_message->Wrap(wrap);
            }
            const wxSize text = m_message->GetBestSize();
            const int text_w = std::min(text.GetWidth(), std::max(1, width - 32));
            const int text_h = text.GetHeight();
            const int text_y = std::max(44, (height - text_h) / 2);
            m_message->SetSize((width - text_w) / 2, std::min(text_y, std::max(44, height - 44 - text_h)), text_w, text_h);
        }
    }

    void publish()
    {
        sync();
        if (m_changed)
            m_changed();
    }

    void set_busy(bool busy)
    {
        m_busy = busy;
        if (m_power)
            m_power->set_enabled(!busy && m_linked);
        if (m_light)
            m_light->set_enabled(!busy && m_linked);
    }

    void toggle_light()
    {
        if (m_busy || !m_anycubic || m_host.empty())
            return;
        const bool turn_on = !camera_sessions()[m_id].light;
        set_busy(true);
        m_note = turn_on ? _L("Switching the chamber light on.") : _L("Switching the chamber light off.");
        sync();
        const std::string id = m_id;
        post_printer(m_host, m_alive,
            [turn_on](Anycubic& printer, std::string& error) { return printer.set_chamber_light(turn_on, error); },
            [this, id, turn_on](bool ok, const std::string& error) {
                set_busy(false);
                if (ok) {
                    camera_sessions()[id].light = turn_on;
                    camera_sessions()[id].light_known = true;
                    control_readings()[id].light = turn_on ? wxString(_L("On")) : wxString(_L("Off"));
                    m_note.clear();
                    notify_camera_watchers(id);
                } else
                    m_note = from_u8(error.empty() ? std::string("The chamber light did not switch.") : error);
                publish();
            });
    }

    void toggle_camera()
    {
        if (m_busy || m_host.empty())
            return;
        if (!m_anycubic) {
            m_note = _L("This connection does not provide a camera picture.");
            sync();
            return;
        }
        CameraSession& session = camera_sessions()[m_id];
        const bool start = !session.running;
        set_busy(true);
        m_note = start ? _L("Starting the camera.") : _L("Stopping the camera.");
        sync();
        if (!start && session.preview)
            session.preview->stop();
        const std::string id = m_id;
        const std::string host = m_host;
        post_printer(host, m_alive,
            [start](Anycubic& printer, std::string& error) { return printer.set_camera_capture(start, error); },
            [this, id, host, start](bool ok, const std::string& error) {
                set_busy(false);
                CameraSession& current = camera_sessions()[id];
                if (!ok) {
                    m_note = from_u8(error.empty() ? std::string("The camera did not answer.") : error);
                    if (!start)
                        current.running = false;
                    publish();
                    return;
                }
                current.running = start;
                if (start) {
                    current.light = true;
                    current.light_known = true;
                    control_readings()[id].light = _L("On");
                    notify_camera_watchers(id);
                    current.preview = std::make_shared<AnycubicPreview>(host);
                    current.preview->start();
                    m_note.clear();
                    m_frame = wxBitmap();
                    m_scaled_width = 0;
                    m_wait_ticks = 0;
                    if (!m_timer.IsRunning())
                        m_timer.Start(120);
                } else {
                    current.preview.reset();
                    current.light = false;
                    current.light_known = true;
                    control_readings()[id].light = _L("Off");
                    notify_camera_watchers(id);
                    m_timer.Stop();
                    m_frame = wxBitmap();
                    m_scaled_width = 0;
                    m_note.clear();
                }
                publish();
            });
    }

    void on_tick()
    {
        CameraSession& session = camera_sessions()[m_id];
        if (!session.preview)
            return;
        AnycubicPicture picture;
        picture.serial = m_serial;
        if (session.preview->copy_picture(picture)
            && picture.width > 0
            && picture.rgb.size() >= static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 3) {
            wxImage image(picture.width, picture.height);
            const size_t bytes = static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 3;
            std::memcpy(image.GetData(), picture.rgb.data(), bytes);
            const bool first = !m_frame.IsOk();
            m_frame = wxBitmap(image);
            m_serial = picture.serial;
            rescale();
            if (!m_note.empty())
                m_note.clear();
            // The first picture hides the status line. Later pictures only replace
            // the bitmap, so the view is not cleared between frames.
            if (first)
                sync();
            else
                Refresh(false);
            return;
        }
        const std::string error = session.preview->last_error();
        if (!error.empty() && from_u8(error) != m_note) {
            m_note = from_u8(error);
            sync();
            return;
        }
        if (m_frame.IsOk() || !error.empty())
            return;
        ++m_wait_ticks;
        if (m_wait_ticks == 80 && m_note.empty()) {
            m_note = _L("The camera is on, but no picture has arrived.");
            sync();
        }
    }

    void rescale()
    {
        if (!m_frame.IsOk())
            return;
        const wxSize size = GetClientSize();
        const int source_w = m_frame.GetWidth();
        const int source_h = m_frame.GetHeight();
        if (source_w < 1 || source_h < 1 || size.x < 2 || size.y < 2)
            return;
        const double scale = std::min(double(size.x) / source_w, double(size.y) / source_h);
        const int dest_w = std::max(1, int(source_w * scale));
        const int dest_h = std::max(1, int(source_h * scale));
        if (m_scaled.IsOk() && m_scaled_width == dest_w && m_scaled_height == dest_h && m_scaled_serial == m_serial)
            return;
        m_scaled = wxBitmap(m_frame.ConvertToImage().Scale(dest_w, dest_h, wxIMAGE_QUALITY_BILINEAR));
        m_scaled_width = dest_w;
        m_scaled_height = dest_h;
        m_scaled_serial = m_serial;
    }

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(GetBackgroundColour()));
        dc.DrawRectangle(0, 0, size.x, size.y);
        wxBitmap frame = m_scaled.IsOk() ? m_scaled : m_frame;
        if (!frame.IsOk() || frame.GetWidth() < 1 || frame.GetHeight() < 1 || size.x < 2 || size.y < 2)
            return;
        const double scale = std::min(double(size.x) / frame.GetWidth(), double(size.y) / frame.GetHeight());
        const int dest_w = std::max(1, int(frame.GetWidth() * scale));
        const int dest_h = std::max(1, int(frame.GetHeight() * scale));
        const int x = (size.x - dest_w) / 2;
        const int y = (size.y - dest_h) / 2;
        if (dest_w == frame.GetWidth() && dest_h == frame.GetHeight()) {
            dc.DrawBitmap(frame, x, y, false);
            return;
        }
        wxMemoryDC source(frame);
        dc.StretchBlit(x, y, dest_w, dest_h, &source, 0, 0, frame.GetWidth(), frame.GetHeight());
    }

    void on_fullscreen()
    {
        if (m_fullscreen) {
            if (wxDialog* dialog = dynamic_cast<wxDialog*>(wxGetTopLevelParent(this)))
                dialog->EndModal(wxID_CLOSE);
            return;
        }
        DeviceEntry device;
        device.id = m_id;
        device.host = m_host;
        device.link = m_anycubic ? "Anycubic LAN" : "";
        wxDialog dialog(this, wxID_ANY, _L("Camera"), wxDefaultPosition, wxSize(960, 640), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX);
        wxGetApp().UpdateDarkUI(&dialog);
        dialog.SetBackgroundColour(wxGetApp().get_window_default_clr());
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* stage = new CameraView(&dialog, device, -1, true, [this]() { sync(); });
        sizer->Add(stage, 1, wxEXPAND | wxALL, 8);
        dialog.SetSizer(sizer);
        dialog.ShowModal();
        sync();
    }

    std::string m_id;
    std::string m_host;
    bool m_linked { false };
    bool m_anycubic { false };
    bool m_fullscreen { false };
    int m_wrap_width { -1 };
    bool m_busy { false };
    std::function<void()> m_changed;
    std::shared_ptr<bool> m_alive;
    wxString m_note;
    wxStaticText* m_message { nullptr };
    CameraIconButton* m_power { nullptr };
    CameraIconButton* m_full { nullptr };
    CameraIconButton* m_light { nullptr };
    wxTimer m_timer;
    wxBitmap m_frame;
    wxBitmap m_scaled;
    int m_scaled_width { 0 };
    int m_scaled_height { 0 };
    int m_scaled_serial { -1 };
    int m_serial { 0 };
    int m_wait_ticks { 0 };
    int m_watch { 0 };
};

class AxisPad : public wxPanel
{
public:
    explicit AxisPad(wxWindow* parent)
        : wxPanel(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(k_card);
        SetMinSize(wxSize(160, 160));
        Bind(wxEVT_PAINT, &AxisPad::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh();
            event.Skip();
        });
        Bind(wxEVT_LEFT_DOWN, &AxisPad::on_down, this);
        Bind(wxEVT_LEFT_UP, &AxisPad::on_up, this);
        Bind(wxEVT_MOTION, &AxisPad::on_motion, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            if (!m_pressed.empty())
                return;
            m_hover.clear();
            Refresh();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            m_pressed.clear();
            Refresh();
        });
    }

    void set_status(wxStaticText* status) { m_status = status; }

private:
    enum Hit
    {
        None,
        Step1, Step15, Step50,
        HomeXY, Motors,
        YUp, XLeft, XRight, YDown, Home,
        ZUp, ZHome, ZDown
    };

    struct Geom
    {
        wxRect bar;
        wxRect steps[3];
        wxRect home_xy;
        wxRect motors;
        wxRect z_up;
        wxRect z_home;
        wxRect z_down;
        double cx { 0 };
        double cy { 0 };
        double radius { 0 };
        double inner { 0 };
    };

    Geom geom() const
    {
        Geom g;
        const wxSize size = GetClientSize();
        const int width = std::max(1, size.x);
        const int height = std::max(1, size.y);
        g.bar = wxRect(8, 4, std::max(24, width - 16), 34);
        const int segment = g.bar.width / 3;
        for (int i = 0; i < 3; ++i) {
            const int x = g.bar.x + segment * i;
            const int right = i == 2 ? g.bar.GetRight() : x + segment;
            g.steps[i] = wxRect(x, g.bar.y, std::max(1, right - x), g.bar.height);
        }
        const int top = g.bar.GetBottom() + 14;
        const int avail = std::max(48, height - top - 8);
        const int side = 40;
        const int gap = 16;
        int radius = std::min(avail / 2, (width - 16 - side * 2 - gap * 2) / 2);
        if (radius < 36)
            radius = 36;
        if (radius > 78)
            radius = 78;
        g.radius = radius;
        g.inner = std::max(16.0, radius * 0.34);
        g.cx = width / 2.0;
        g.cy = top + avail / 2.0;
        const int left = int(g.cx - g.radius - gap - side);
        const int right = int(g.cx + g.radius + gap);
        g.home_xy = wxRect(left, int(g.cy) - side - 6, side, side);
        g.motors = wxRect(left, int(g.cy) + 6, side, side);
        const int z_h = 34;
        const int z_w = 46;
        const int z_top = int(g.cy) - (z_h * 3 + 8) / 2;
        g.z_up = wxRect(right, z_top, z_w, z_h);
        g.z_home = wxRect(right, z_top + z_h + 4, z_w, z_h);
        g.z_down = wxRect(right, z_top + (z_h + 4) * 2, z_w, z_h);
        return g;
    }

    Hit hit_test(const wxPoint& point) const
    {
        const Geom g = geom();
        for (int i = 0; i < 3; ++i)
            if (g.steps[i].Contains(point))
                return i == 0 ? Step1 : i == 1 ? Step15 : Step50;
        if (g.home_xy.Contains(point))
            return HomeXY;
        if (g.motors.Contains(point))
            return Motors;
        if (g.z_up.Contains(point))
            return ZUp;
        if (g.z_home.Contains(point))
            return ZHome;
        if (g.z_down.Contains(point))
            return ZDown;
        const double dx = point.x - g.cx;
        const double dy = point.y - g.cy;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist > g.radius)
            return None;
        if (dist <= g.inner)
            return Home;
        const double angle = std::atan2(dy, dx);
        constexpr double pi = 3.141592653589793;
        if (angle >= -3 * pi / 4 && angle < -pi / 4)
            return YUp;
        if (angle >= -pi / 4 && angle < pi / 4)
            return XRight;
        if (angle >= pi / 4 && angle < 3 * pi / 4)
            return YDown;
        return XLeft;
    }

    void draw_home(wxGraphicsContext* gc, double x, double y, double size) const
    {
        wxGraphicsPath roof = gc->CreatePath();
        roof.MoveToPoint(x, y + size * 0.42);
        roof.AddLineToPoint(x + size / 2, y);
        roof.AddLineToPoint(x + size, y + size * 0.42);
        gc->SetPen(wxPen(wxColour(90, 170, 255), 2));
        gc->StrokePath(roof);
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(x + size * 0.22, y + size * 0.40, size * 0.56, size * 0.48, 1);
    }

    void draw_hand(wxGraphicsContext* gc, double x, double y, double size) const
    {
        gc->SetPen(wxPen(wxColour(90, 170, 255), 2));
        gc->SetBrush(*wxTRANSPARENT_BRUSH);
        gc->DrawRoundedRectangle(x + size * 0.28, y + size * 0.38, size * 0.44, size * 0.46, 4);
        for (int i = 0; i < 3; ++i)
            gc->StrokeLine(x + size * (0.36 + i * 0.14), y + size * 0.38, x + size * (0.36 + i * 0.14), y + size * 0.16);
    }

    void draw_label(wxGraphicsContext* gc, const wxString& text, double x, double y) const
    {
        gc->SetFont(wxGetApp().small_font(), wxColour(90, 170, 255));
        double w = 0, h = 0;
        gc->GetTextExtent(text, &w, &h);
        gc->DrawText(text, x - w / 2, y - h / 2);
    }

    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        paint_buffered(this, dc, [&](wxGraphicsContext& context, const wxSize&) {
        wxGraphicsContext* gc = &context;
        const Geom g = geom();
        const wxColour blue(47, 140, 255);
        const wxColour icon(90, 170, 255);
        gc->SetBrush(wxBrush(k_card));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(0, 0, GetClientSize().x, GetClientSize().y);

        gc->SetBrush(wxBrush(wxColour(40, 40, 48)));
        gc->DrawRoundedRectangle(g.bar.x, g.bar.y, g.bar.width, g.bar.height, 17);
        const int selected = m_step == 1 ? 0 : m_step == 15 ? 1 : 2;
        const wxRect& pill = g.steps[selected];
        gc->SetBrush(wxBrush(blue));
        gc->DrawRoundedRectangle(pill.x + 3, pill.y + 3, pill.width - 6, pill.height - 6, 14);
        const wxString steps[3] = { wxT("1 mm"), wxT("15 mm"), wxT("50 mm") };
        for (int i = 0; i < 3; ++i) {
            gc->SetFont(wxGetApp().bold_font(), i == selected ? *wxWHITE : k_muted);
            double w = 0, h = 0;
            gc->GetTextExtent(steps[i], &w, &h);
            gc->DrawText(steps[i], g.steps[i].x + (g.steps[i].width - w) / 2, g.steps[i].y + (g.steps[i].height - h) / 2);
        }

        auto circle_button = [&](const wxRect& rect, Hit id) {
            const bool hot = m_pressed == hit_name(id) || m_hover == hit_name(id);
            gc->SetBrush(wxBrush(hot ? wxColour(58, 64, 78) : wxColour(36, 36, 44)));
            gc->SetPen(wxPen(wxColour(62, 62, 74), 1));
            gc->DrawEllipse(rect.x, rect.y, rect.width, rect.height);
        };
        circle_button(g.home_xy, HomeXY);
        circle_button(g.motors, Motors);
        draw_home(gc, g.home_xy.x + 10, g.home_xy.y + 9, 20);
        draw_hand(gc, g.motors.x + 8, g.motors.y + 8, 24);

        const bool pad_hot = m_pressed == "Y+" || m_pressed == "Y-" || m_pressed == "X+" || m_pressed == "X-" || m_hover == "Y+" || m_hover == "Y-" || m_hover == "X+" || m_hover == "X-";
        gc->SetBrush(wxBrush(pad_hot ? wxColour(52, 52, 62) : wxColour(40, 40, 48)));
        gc->SetPen(wxPen(wxColour(62, 62, 74), 1));
        gc->DrawEllipse(g.cx - g.radius, g.cy - g.radius, g.radius * 2, g.radius * 2);
        gc->SetPen(wxPen(wxColour(28, 28, 34), 3));
        const double diag = g.radius * 0.72;
        gc->StrokeLine(g.cx - diag, g.cy - diag, g.cx + diag, g.cy + diag);
        gc->StrokeLine(g.cx - diag, g.cy + diag, g.cx + diag, g.cy - diag);
        gc->SetBrush(wxBrush(m_pressed == "home" || m_hover == "home" ? wxColour(58, 64, 78) : wxColour(30, 30, 36)));
        gc->SetPen(wxPen(wxColour(70, 70, 82), 1));
        gc->DrawEllipse(g.cx - g.inner, g.cy - g.inner, g.inner * 2, g.inner * 2);
        draw_home(gc, g.cx - 8, g.cy - 8, 16);
        draw_label(gc, wxT("Y"), g.cx, g.cy - g.radius * 0.62);
        draw_label(gc, wxT("Y"), g.cx, g.cy + g.radius * 0.62);
        draw_label(gc, wxT("X"), g.cx - g.radius * 0.62, g.cy);
        draw_label(gc, wxT("X"), g.cx + g.radius * 0.62, g.cy);

        auto z_button = [&](const wxRect& rect, int direction, const char* id) {
            const bool hot = m_pressed == id || m_hover == id;
            gc->SetBrush(wxBrush(hot ? wxColour(58, 64, 78) : wxColour(36, 36, 44)));
            gc->SetPen(wxPen(wxColour(62, 62, 74), 1));
            gc->DrawRoundedRectangle(rect.x, rect.y, rect.width, rect.height, 8);
            const double cx = rect.x + rect.width / 2.0;
            const double cy = rect.y + rect.height / 2.0;
            if (direction != 0) {
                draw_label(gc, wxT("Z"), cx - 7, cy);
                wxGraphicsPath arrow = gc->CreatePath();
                const double tip = cy - direction * 8;
                arrow.MoveToPoint(cx + 6, tip);
                arrow.AddLineToPoint(cx + 11, tip + direction * 6);
                arrow.AddLineToPoint(cx + 1, tip + direction * 6);
                arrow.CloseSubpath();
                gc->SetBrush(wxBrush(wxColour(90, 170, 255)));
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->FillPath(arrow);
            }
        };
        z_button(g.z_up, 1, "Z+");
        z_button(g.z_home, 0, "Zh");
        draw_home(gc, g.z_home.x + 12, g.z_home.y + 8, 18);
        z_button(g.z_down, -1, "Z-");
        });
    }

    static const char* hit_name(Hit hit)
    {
        switch (hit) {
        case Step1: return "1";
        case Step15: return "15";
        case Step50: return "50";
        case HomeXY: return "home-xy";
        case Motors: return "motors";
        case YUp: return "Y+";
        case XLeft: return "X-";
        case XRight: return "X+";
        case YDown: return "Y-";
        case Home: return "home";
        case ZUp: return "Z+";
        case ZHome: return "Zh";
        case ZDown: return "Z-";
        default: return "";
        }
    }

    void show_status(const wxString& text)
    {
        if (!m_status)
            return;
        m_status->SetLabel(text);
        const int width = m_status->GetParent() ? m_status->GetParent()->GetClientSize().GetWidth() : 0;
        m_status->Wrap(width > 120 ? width - 32 : 480);
        if (wxWindow* parent = m_status->GetParent())
            parent->Layout();
    }

    void on_down(wxMouseEvent& event)
    {
        m_press_hit = hit_test(event.GetPosition());
        m_press_point = event.GetPosition();
        m_pressed = hit_name(m_press_hit);
        if (!m_pressed.empty() && !HasCapture())
            CaptureMouse();
        Refresh();
    }

    void on_up(wxMouseEvent& event)
    {
        if (HasCapture())
            ReleaseMouse();
        Hit hit = hit_test(event.GetPosition());
        const int dx = event.GetPosition().x - m_press_point.x;
        const int dy = event.GetPosition().y - m_press_point.y;
        if (hit == None && dx * dx + dy * dy <= 18 * 18)
            hit = m_press_hit;
        const Hit pressed = m_press_hit;
        m_press_hit = None;
        m_pressed.clear();
        if (pressed != None && pressed == hit)
            activate(hit);
        Refresh();
    }

    void on_motion(wxMouseEvent& event)
    {
        const std::string hover = hit_name(hit_test(event.GetPosition()));
        if (hover != m_hover) {
            m_hover = hover;
            Refresh();
        }
    }

    void activate(Hit hit)
    {
        if (hit == Step1 || hit == Step15 || hit == Step50) {
            m_step = hit == Step1 ? 1 : hit == Step15 ? 15 : 50;
            show_status(wxString::Format(_L("Each press will be %d mm. Nothing is sent to the printer."), m_step));
            return;
        }
        wxString action;
        switch (hit) {
        case YUp: action = wxString::Format(_L("Y+ %d mm"), m_step); break;
        case YDown: action = wxString::Format(_L("Y− %d mm"), m_step); break;
        case XLeft: action = wxString::Format(_L("X− %d mm"), m_step); break;
        case XRight: action = wxString::Format(_L("X+ %d mm"), m_step); break;
        case ZUp: action = wxString::Format(_L("Z+ %d mm"), m_step); break;
        case ZDown: action = wxString::Format(_L("Z− %d mm"), m_step); break;
        case HomeXY: action = _L("Home X and Y"); break;
        case Home: action = _L("Home all"); break;
        case ZHome: action = _L("Home Z"); break;
        case Motors: action = _L("Release motors"); break;
        default: return;
        }
        show_status(wxString::Format(_L("%s. Not sent, so the nozzle cannot be driven into the bed from here."), action));
    }

    int m_step { 1 };
    Hit m_press_hit { None };
    wxPoint m_press_point;
    std::string m_pressed;
    std::string m_hover;
    wxStaticText* m_status { nullptr };
};

class JobTrack : public wxPanel
{
public:
    explicit JobTrack(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 8), wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(k_card);
        SetMinSize(wxSize(40, 8));
        Bind(wxEVT_PAINT, &JobTrack::on_paint, this);
    }

    void set_percent(int percent)
    {
        m_percent = percent;
        Refresh();
    }

private:
    int m_percent { -1 };

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize size = GetClientSize();
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(k_track));
        gc->DrawRoundedRectangle(0, 0, std::max(1, size.x), std::max(1, size.y), 3);
        if (m_percent < 0)
            return;
        const int pct = std::max(0, std::min(100, m_percent));
        const double width = size.x * (pct / 100.0);
        if (width < 1)
            return;
        gc->SetBrush(wxBrush(wxColour(254, 50, 173)));
        gc->DrawRoundedRectangle(0, 0, width, size.y, 3);
    }
};

DevicesPanel* g_devices_panel = nullptr;

std::set<std::string>& job_queries()
{
    static std::set<std::string> ids;
    return ids;
}

void store_control_reading(const std::string& id, const AnycubicReadings& fetched)
{
    ControlReading& reading = control_readings()[id];
    auto put_temp = [](wxString& slot, bool has, int value) {
        if (has)
            slot = wxString::Format("%d", value) + wxString(L"\u00B0C");
    };
    put_temp(reading.nozzle, fetched.has_nozzle, fetched.nozzle);
    put_temp(reading.bed, fetched.has_bed, fetched.bed);
    put_temp(reading.chamber, fetched.has_chamber, fetched.chamber);
    if (fetched.has_model_fan)
        reading.model_fan = wxString::Format("%d%%", fetched.model_fan);
    if (fetched.has_aux_fan)
        reading.aux_fan = wxString::Format("%d%%", fetched.aux_fan);
    if (fetched.has_light) {
        reading.light = fetched.light_on ? wxString(_L("On")) : wxString(_L("Off"));
        camera_sessions()[id].light = fetched.light_on;
        camera_sessions()[id].light_known = true;
    }
    if (fetched.has_ace_count)
        reading.ace_units = fetched.ace_count;
    if (fetched.has_job) {
        reading.local = false;
        reading.job_known = true;
        reading.printing = fetched.printing;
        reading.job_name = from_u8(fetched.job_name);
        reading.progress = fetched.has_progress ? fetched.progress : -1;
        reading.layer = fetched.layer;
        reading.layers = fetched.layers;
        reading.remain_minutes = fetched.has_remain ? fetched.remain_minutes : -1;
        reading.elapsed_minutes = fetched.has_elapsed ? fetched.elapsed_minutes : -1;
    }
    remember_printer_activity(id, fetched);
    notify_camera_watchers(id);
    if (g_devices_panel)
        g_devices_panel->apply_live_status(id);
}

wxString print_span(int minutes)
{
    if (minutes < 0)
        return wxString(L"—");
    if (minutes < 60)
        return wxString::Format("%d min", minutes);
    return wxString::Format("%dh %dm", minutes / 60, minutes % 60);
}

wxString finish_clock(int remain_minutes)
{
    if (remain_minutes < 0)
        return wxString(L"—");
    wxDateTime when = wxDateTime::Now();
    when.Add(wxTimeSpan::Minutes(remain_minutes));
    if (when.IsSameDate(wxDateTime::Today()))
        return when.Format("%H:%M");
    return when.Format("%a %H:%M");
}

struct OfflineJob
{
    std::string printer_id;
    std::string name;
    long long   started { 0 };
    int         duration_seconds { 0 };
    int         layers { 0 };
};

const char* dismissed_models_key()
{
    return "dismissed_offline_models";
}

const char* offline_jobs_key()
{
    return "offline_print_jobs";
}

std::set<std::string> dismissed_models()
{
    std::set<std::string> ids;
    if (!wxGetApp().app_config)
        return ids;
    const std::string stored = wxGetApp().app_config->get(dismissed_models_key());
    size_t cursor = 0;
    while (cursor < stored.size()) {
        const size_t end = stored.find('\n', cursor);
        std::string id = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        if (!id.empty() && id.back() == '\r')
            id.pop_back();
        if (!id.empty())
            ids.insert(std::move(id));
        if (end == std::string::npos)
            break;
        cursor = end + 1;
    }
    return ids;
}

void store_dismissed_models(const std::set<std::string>& ids)
{
    if (!wxGetApp().app_config)
        return;
    std::string stored;
    for (const std::string& id : ids) {
        if (id.empty())
            continue;
        if (!stored.empty())
            stored.push_back('\n');
        stored += id;
    }
    if (wxGetApp().app_config->set(dismissed_models_key(), stored))
        wxGetApp().app_config->save();
}

struct OfflineJobStore
{
    bool loaded { false };
    std::map<std::string, OfflineJob> jobs;
};

OfflineJobStore& offline_job_store()
{
    static OfflineJobStore store;
    return store;
}

void persist_offline_jobs()
{
    if (!wxGetApp().app_config)
        return;
    std::string stored;
    for (const auto& item : offline_job_store().jobs) {
        const OfflineJob& job = item.second;
        std::string name = job.name;
        for (char& c : name)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        std::string id = job.printer_id;
        for (char& c : id)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        if (!stored.empty())
            stored.push_back('\n');
        stored += id + "\t" + name + "\t" + std::to_string(job.started) + "\t"
            + std::to_string(job.duration_seconds) + "\t" + std::to_string(job.layers);
    }
    if (wxGetApp().app_config->set(offline_jobs_key(), stored))
        wxGetApp().app_config->save();
}

void load_offline_jobs()
{
    OfflineJobStore& store = offline_job_store();
    if (store.loaded)
        return;
    store.loaded = true;
    if (!wxGetApp().app_config)
        return;
    const std::string stored = wxGetApp().app_config->get(offline_jobs_key());
    const long long now = static_cast<long long>(std::time(nullptr));
    size_t cursor = 0;
    while (cursor < stored.size()) {
        const size_t end = stored.find('\n', cursor);
        std::string line = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        cursor = end == std::string::npos ? stored.size() : end + 1;
        std::vector<std::string> parts;
        size_t at = 0;
        while (at <= line.size()) {
            const size_t tab = line.find('\t', at);
            parts.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos)
                break;
            at = tab + 1;
        }
        if (parts.size() < 5 || parts[0].empty())
            continue;
        OfflineJob job;
        job.printer_id = parts[0];
        job.name = parts[1];
        try {
            job.started = std::stoll(parts[2]);
            job.duration_seconds = std::stoi(parts[3]);
            job.layers = std::stoi(parts[4]);
        } catch (const std::exception&) {
            continue;
        }
        if (job.duration_seconds <= 0 || now >= job.started + job.duration_seconds)
            continue;
        store.jobs[job.printer_id] = std::move(job);
    }
}

void apply_offline_job(const OfflineJob& job, long long now)
{
    ControlReading& reading = control_readings()[job.printer_id];
    const int elapsed = int(std::max(0LL, now - job.started));
    const int remain = std::max(0, job.duration_seconds - elapsed);
    reading.local = true;
    reading.job_known = true;
    reading.job_name = from_u8(job.name);
    reading.layers = job.layers;
    reading.printing = remain > 0;
    reading.progress = job.duration_seconds > 0 ? int(std::min(100LL, elapsed * 100LL / job.duration_seconds)) : 0;
    reading.elapsed_minutes = elapsed / 60;
    reading.remain_minutes = remain > 0 ? (remain + 59) / 60 : 0;
    if (job.layers > 0 && job.duration_seconds > 0)
        reading.layer = std::min(job.layers, int(job.layers * (double(elapsed) / double(job.duration_seconds))));
    else
        reading.layer = 0;
}

std::string preset_model_id(const Preset& preset)
{
    return preset.config.opt_string("printer_model");
}

bool printer_covers_preset(const PhysicalPrinter& printer, const std::string& preset_name, const std::string& model)
{
    if (!preset_name.empty() && printer.preset_names.count(preset_name))
        return true;
    if (model.empty() || !wxGetApp().preset_bundle)
        return false;
    for (const std::string& stored : printer.preset_names) {
        const Preset* preset = wxGetApp().preset_bundle->printers.find_preset(stored, false);
        if (preset == nullptr && stored.find(PhysicalPrinter::separator()) != std::string::npos)
            preset = wxGetApp().preset_bundle->printers.find_preset(PhysicalPrinter::get_preset_name(stored), false);
        if (preset && preset->config.opt_string("printer_model") == model)
            return true;
    }
    return false;
}

std::string create_offline_printer(const Preset& preset)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return {};
    PhysicalPrinterCollection& printers = bundle->physical_printers;
    const std::string model = preset_model_id(preset);
    std::set<std::string> used;
    for (const PhysicalPrinter& printer : printers)
        used.insert(printer.name);
    std::string name = preset.name.empty() ? std::string("Printer") : preset.name;
    if (!used.insert(name).second) {
        for (unsigned n = 2;; ++n) {
            const std::string candidate = preset.name + " " + std::to_string(n);
            if (used.insert(candidate).second) {
                name = candidate;
                break;
            }
        }
    }
    PhysicalPrinter printer(name, printers.default_config());
    printer.config.set_key_value("print_host", new ConfigOptionString(std::string()));
    PrinterTechnology technology = ptFFF;
    if (const auto* option = preset.config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology"))
        technology = option->value;
    printer.config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(technology));
    const std::string vendor_name = preset.vendor ? preset.vendor->name : std::string();
    printer.config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(default_host_type(technology, vendor_name)));
    bool attached = false;
    for (const Preset& item : bundle->printers) {
        if (!model.empty() && item.config.opt_string("printer_model") != model)
            continue;
        if (model.empty() && item.name != preset.name)
            continue;
        if (!item.is_visible && item.name != preset.name)
            continue;
        printer.add_preset(item.name);
        attached = true;
    }
    if (!attached)
        printer.add_preset(preset.name);
    printer.update_preset_names_in_config();
    try {
        printers.save_printer(printer);
        printers.select_printer(name, preset.name);
    } catch (const std::exception&) {
        return {};
    } catch (...) {
        return {};
    }
    return name;
}

std::string resolve_offline_printer(bool create)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle || bundle->printers.get_selected_idx() == size_t(-1))
        return {};
    const Preset& preset = bundle->printers.get_selected_preset();
    if (preset.name.empty())
        return {};
    const std::string model = preset_model_id(preset);
    PhysicalPrinterCollection& printers = bundle->physical_printers;

    std::string offline_name;
    bool networked = false;
    for (const PhysicalPrinter& printer : printers) {
        if (!printer_covers_preset(printer, preset.name, model))
            continue;
        if (PhysicalPrinter::has_print_host_information(printer.config))
            networked = true;
        else if (offline_name.empty() || (printers.has_selection() && printer.name == printers.get_selected_printer_name()))
            offline_name = printer.name;
    }
    if (printers.has_selection()) {
        const PhysicalPrinter& selected = printers.get_selected_printer();
        if (PhysicalPrinter::has_print_host_information(selected.config) && printer_covers_preset(selected, preset.name, model))
            return {};
    }
    if (!offline_name.empty()) {
        if (PhysicalPrinter* saved = printers.find_printer(offline_name, false)) {
            const auto* type = saved->config.option<ConfigOptionEnum<PrintHostType>>("host_type");
            PrinterTechnology technology = ptFFF;
            if (const auto* option = saved->config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology"))
                technology = option->value;
            const std::string vendor_name = preset.vendor ? preset.vendor->name : std::string();
            if (type && type->value == htPrusaLink && !printer_can_connect(technology, vendor_name)
                && !PhysicalPrinter::has_print_host_information(saved->config)) {
                saved->config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htOffline));
                try {
                    printers.save_printer(*saved);
                } catch (const std::exception&) {
                }
            }
        }
        return offline_name;
    }
    if (networked)
        return {};
    if (!model.empty() && dismissed_models().count(model))
        return {};
    if (!create || preset.is_default)
        return create ? std::string() : (preset.is_default ? std::string() : preset.name);
    return create_offline_printer(preset);
}

void ensure_selected_offline_device()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return;
    const Preset& preset = bundle->printers.get_selected_preset();
    if (preset.is_default || preset.name.empty())
        return;
    resolve_offline_printer(true);
}

} // namespace

std::string offline_export_printer_name()
{
    return resolve_offline_printer(false);
}

void begin_offline_print(const std::string& job_name, int estimate_seconds, int layers)
{
    const std::string printer_id = resolve_offline_printer(true);
    if (printer_id.empty())
        return;
    if (wxGetApp().preset_bundle) {
        const Preset& preset = wxGetApp().preset_bundle->printers.get_selected_preset();
        const std::string model = preset.config.opt_string("printer_model");
        if (!model.empty())
            undismiss_offline_model(model);
    }
    load_offline_jobs();
    OfflineJob job;
    job.printer_id = printer_id;
    job.name = job_name.empty() ? std::string("Print") : job_name;
    for (char& c : job.name)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    job.started = static_cast<long long>(std::time(nullptr));
    const int estimate = std::max(0, estimate_seconds);
    job.duration_seconds = estimate + offline_print_pad_seconds();
    job.layers = std::max(0, layers);
    offline_job_store().jobs[printer_id] = job;
    persist_offline_jobs();
    refresh_offline_jobs(false);
    if (g_devices_panel) {
        g_devices_panel->reload();
        g_devices_panel->ensure_print_task_tile(printer_id);
    }
    publish_dashboard_snapshot();
}

void stop_offline_print(const std::string& printer_id)
{
    if (printer_id.empty())
        return;
    load_offline_jobs();
    if (offline_job_store().jobs.erase(printer_id) > 0)
        persist_offline_jobs();
    ControlReading& reading = control_readings()[printer_id];
    reading.local = true;
    reading.job_known = true;
    reading.printing = false;
    reading.job_name.clear();
    reading.progress = -1;
    reading.layer = 0;
    reading.layers = 0;
    reading.remain_minutes = -1;
    reading.elapsed_minutes = -1;
    notify_camera_watchers(printer_id);
    if (g_devices_panel)
        g_devices_panel->apply_live_status(printer_id);
    publish_dashboard_snapshot();
}

void refresh_offline_jobs(bool notify_ui)
{
    load_offline_jobs();
    const long long now = static_cast<long long>(std::time(nullptr));
    std::vector<std::string> touched;
    bool dropped = false;
    for (auto it = offline_job_store().jobs.begin(); it != offline_job_store().jobs.end(); ) {
        if (now >= it->second.started + it->second.duration_seconds) {
            ControlReading& reading = control_readings()[it->first];
            reading.local = true;
            reading.job_known = true;
            reading.printing = false;
            reading.progress = 100;
            reading.remain_minutes = 0;
            reading.elapsed_minutes = it->second.duration_seconds / 60;
            reading.layer = it->second.layers;
            reading.layers = it->second.layers;
            touched.push_back(it->first);
            it = offline_job_store().jobs.erase(it);
            dropped = true;
            continue;
        }
        apply_offline_job(it->second, now);
        touched.push_back(it->first);
        ++it;
    }
    if (dropped)
        persist_offline_jobs();
    if (!notify_ui)
        return;
    for (const std::string& id : touched) {
        notify_camera_watchers(id);
        if (g_devices_panel)
            g_devices_panel->apply_live_status(id);
    }
}

std::string offline_dashboard_text(const std::string& printer_name)
{
    const auto found = offline_job_store().jobs.find(printer_name);
    if (found == offline_job_store().jobs.end())
        return {};
    const OfflineJob& job = found->second;
    const long long now = static_cast<long long>(std::time(nullptr));
    const int elapsed = int(std::max(0LL, now - job.started));
    const int remain = std::max(0, job.duration_seconds - elapsed);
    const int progress = job.duration_seconds > 0 ? int(std::min(100LL, elapsed * 100LL / job.duration_seconds)) : 0;
    const int remain_minutes = remain > 0 ? (remain + 59) / 60 : 0;
    wxString body = from_u8(job.name);
    body += "\n";
    body += wxString::Format("%d%%", progress);
    body += wxString(L"  ·  ");
    body += print_span(remain_minutes);
    body += " ";
    body += _L("left");
    body += wxString(L"  ·  ");
    body += finish_clock(remain_minutes);
    return into_u8(body);
}

bool offline_job_printing(const std::string& printer_name)
{
    const auto found = offline_job_store().jobs.find(printer_name);
    if (found == offline_job_store().jobs.end())
        return false;
    const long long now = static_cast<long long>(std::time(nullptr));
    return now < found->second.started + found->second.duration_seconds;
}

void dismiss_offline_model(const std::string& model_id)
{
    if (model_id.empty())
        return;
    std::set<std::string> ids = dismissed_models();
    if (!ids.insert(model_id).second)
        return;
    store_dismissed_models(ids);
}

void undismiss_offline_model(const std::string& model_id)
{
    if (model_id.empty())
        return;
    std::set<std::string> ids = dismissed_models();
    if (!ids.erase(model_id))
        return;
    store_dismissed_models(ids);
}

namespace {

void fill_print_task(wxPanel* card, const DeviceEntry& device)
{
    auto* job = new wxStaticText(card, wxID_ANY, _L("Waiting for this printer…"),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    job->SetForegroundColour(*wxWHITE);
    job->SetBackgroundColour(k_card);
    job->SetFont(wxGetApp().bold_font());
    job->SetMinSize(wxSize(40, -1));
    card_add(card, job);
    auto* track = new JobTrack(card);
    card_add(card, track);
    auto* stats = new wxWrapSizer(wxHORIZONTAL);
    auto* layers_line = muted_line(card, _L("Layers  —"));
    auto* time_line = muted_line(card, _L("Time  — / —"));
    auto* finish_line = muted_line(card, _L("Finishes  —"));
    stats->Add(layers_line, 0, wxALIGN_CENTER_VERTICAL);
    stats->AddSpacer(18);
    stats->Add(time_line, 0, wxALIGN_CENTER_VERTICAL);
    stats->AddSpacer(18);
    stats->Add(finish_line, 0, wxALIGN_CENTER_VERTICAL);
    card->GetSizer()->Add(stats, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    const wxString offline_note = _L("No network connection. Export a file and choose Track it to show the estimate here.");
    auto* note = muted_line(card, device.host.empty() ? offline_note : wxString());
    card_add(card, note);
    if (device.host.empty())
        fit_card_text(note);
    else
        note->Hide();

    const std::string id = device.id;
    const bool linked = !device.host.empty();
    wxButton* stop = nullptr;
    if (!linked) {
        stop = new wxButton(card, wxID_ANY, _L("Stop task"));
        wxGetApp().UpdateDarkUI(stop, true);
        stop->SetToolTip(_L("Clear this tracked task. The printer is not contacted, so a print that already started there keeps going."));
        stop->Hide();
        card->GetSizer()->Add(stop, 0, wxLEFT | wxRIGHT | wxTOP, 8);
        stop->Bind(wxEVT_BUTTON, [id](wxCommandEvent&) { stop_offline_print(id); });
    }
    auto show_idle = [=]() {
        layers_line->SetLabel(_L("Layers  —"));
        time_line->SetLabel(_L("Time  — / —"));
        finish_line->SetLabel(_L("Finishes  —"));
        track->set_percent(-1);
    };
    auto show_stop = [=](bool visible) {
        if (!stop || stop->IsShown() == visible)
            return;
        stop->Show(visible);
        card->Layout();
    };
    auto apply = std::make_shared<std::function<void()>>();
    *apply = [=]() {
        const ControlReading& reading = control_readings()[id];
        if (!reading.job_known) {
            job->SetLabel(linked ? _L("Waiting for this printer…") : _L("No print is being tracked"));
            show_idle();
            show_stop(false);
            if (!linked) {
                note->SetLabel(offline_note);
                note->Show();
            }
            return;
        }
        if (!reading.printing) {
            job->SetLabel(linked ? _L("No job is running") : _L("No print is being tracked"));
            show_idle();
            show_stop(false);
            if (linked)
                note->Hide();
            else {
                note->SetLabel(offline_note);
                note->Show();
            }
            card->Layout();
            return;
        }
        show_stop(reading.local);
        wxString name = reading.job_name.empty() ? wxString(_L("Print in progress")) : reading.job_name;
        if (name.Lower().EndsWith(".gcode"))
            name.RemoveLast(6);
        job->SetLabel(name);
        wxString layers = _L("Layers  —");
        if (reading.layers > 0)
            layers = _L("Layers") + wxString::Format("  %d / %d", reading.layer, reading.layers);
        else if (reading.layer > 0)
            layers = _L("Layers") + wxString::Format("  %d", reading.layer);
        layers_line->SetLabel(layers);
        const int elapsed = reading.elapsed_minutes;
        const int remain = reading.remain_minutes;
        const int total = elapsed >= 0 && remain >= 0 ? elapsed + remain : -1;
        time_line->SetLabel(_L("Time") + "  " + print_span(elapsed) + " / " + print_span(total));
        finish_line->SetLabel(_L("Finishes") + "  " + finish_clock(remain));
        track->set_percent(reading.progress);
        note->SetLabel(reading.local
            ? _L("Tracked on this computer. The finish time is the slicer estimate plus 5 minutes.")
            : _L("This printer is printing, so it cannot be chosen for another print."));
        note->Show();
        card->Layout();
    };
    (*apply)();
    const int watch = next_camera_watch();
    camera_sessions()[id].watchers.emplace(watch, [apply]() {
        if (apply && *apply)
            (*apply)();
    });
    card->Bind(wxEVT_DESTROY, [id, watch](wxWindowDestroyEvent& event) {
        const auto found = camera_sessions().find(id);
        if (found != camera_sessions().end())
            found->second.watchers.erase(watch);
        event.Skip();
    });
}

wxString format_mm(double value)
{
    const double tenths = std::round(value * 10.0) / 10.0;
    if (std::abs(tenths - std::round(tenths)) < 0.05)
        return wxString::Format("%d", int(std::lround(tenths)));
    return wxString::Format("%.1f", tenths);
}

const Preset* preset_for_device(const std::string& printer_name)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return nullptr;
    const PhysicalPrinter* printer = bundle->physical_printers.find_printer(printer_name, false);
    if (!printer || printer->preset_names.empty())
        return nullptr;
    std::string preset_name = *printer->preset_names.begin();
    if (preset_name.find(PhysicalPrinter::separator()) != std::string::npos)
        preset_name = PhysicalPrinter::get_preset_name(preset_name);
    return bundle->printers.find_preset(preset_name, false);
}

void add_resin_facts(wxPanel* info, const Preset* preset)
{
    if (!info || !preset)
        return;
    const ConfigOptionFloat* width = preset->config.option<ConfigOptionFloat>("display_width");
    const ConfigOptionFloat* height = preset->config.option<ConfigOptionFloat>("display_height");
    const ConfigOptionInt* pixels_x = preset->config.option<ConfigOptionInt>("display_pixels_x");
    const ConfigOptionInt* pixels_y = preset->config.option<ConfigOptionInt>("display_pixels_y");
    const ConfigOptionFloat* depth = preset->config.option<ConfigOptionFloat>("max_print_height");
    if (width && height && width->value > 0.0 && height->value > 0.0)
        card_add(info, muted_line(info, _L("Screen  ") + format_mm(width->value) + wxString(L" \u00D7 ") + format_mm(height->value) + wxString(" mm")));
    if (pixels_x && pixels_y && pixels_x->value > 0 && pixels_y->value > 0) {
        wxString pixels = wxString::Format("%d", pixels_x->value) + wxString(L" \u00D7 ") + wxString::Format("%d", pixels_y->value);
        if (width && width->value > 0.0) {
            const double microns = width->value / double(pixels_x->value) * 1000.0;
            if (microns > 1.0 && microns < 200.0)
                pixels += wxString::Format(", %.0f ", microns) + _L("microns");
        }
        card_add(info, muted_line(info, _L("Pixels  ") + pixels));
    }
    if (depth && depth->value > 0.0)
        card_add(info, muted_line(info, _L("Height  ") + format_mm(depth->value) + " mm"));
    if (const auto* orientation = preset->config.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation"))
        card_add(info, muted_line(info, _L("Orientation  ") + (orientation->value == sladoPortrait ? _L("Portrait") : _L("Landscape"))));
}

void fill_printer_board(wxWindow* parent, wxSizer* board, const DeviceEntry& device)
{
    const bool linked = !device.host.empty();
    const wxString missing = _L("—");

    auto* print_task = make_card(parent, _L("Print task"));
    fill_print_task(print_task, device);
    finish_card(print_task);

    auto* camera = make_card(parent, _L("Camera"));
    auto* view = new CameraView(camera, device, 720, false, nullptr);
    card_add(camera, view);
    finish_card(camera);

    wxPanel* material = device.resin ? nullptr : fill_material_card(parent, device);

    auto* info = make_card(parent, _L("Device info"));
    auto* info_name = new wxStaticText(info, wxID_ANY, from_u8(device.name));
    info_name->SetForegroundColour(*wxWHITE);
    info_name->SetBackgroundColour(k_card);
    info_name->SetFont(wxGetApp().bold_font());
    card_add(info, info_name);
    auto* info_status = new wxStaticText(info, wxID_ANY, status_label(device.status));
    info_status->SetForegroundColour(status_colour(device.status, linked));
    info_status->SetBackgroundColour(k_card);
    card_add(info, info_status);
    const std::string info_id = device.id;
    const std::string info_base = device.status;
    if (control_readings()[info_id].job_known && control_readings()[info_id].printing) {
        info_status->SetLabel(status_label("Printing"));
        info_status->SetForegroundColour(status_colour("Printing", linked));
    }
    const int info_watch = next_camera_watch();
    camera_sessions()[info_id].watchers.emplace(info_watch, [=]() {
        const ControlReading& reading = control_readings()[info_id];
        if (!reading.job_known)
            return;
        const std::string shown = reading.printing ? std::string("Printing") : info_base;
        info_status->SetLabel(status_label(shown));
        info_status->SetForegroundColour(status_colour(shown, linked));
    });
    info->Bind(wxEVT_DESTROY, [info_id, info_watch](wxWindowDestroyEvent& event) {
        const auto found = camera_sessions().find(info_id);
        if (found != camera_sessions().end())
            found->second.watchers.erase(info_watch);
        event.Skip();
    });
    if (!device.reported.empty())
        card_add(info, muted_line(info, _L("Name from printer  ") + from_u8(device.reported)));
    wxString model_line = device.model.empty() ? wxString(missing) : from_u8(device.model);
    if (!device.brand.empty() && device.model.empty())
        model_line = from_u8(device.brand);
    else if (!device.brand.empty() && model_line.find(from_u8(device.brand)) == wxString::npos)
        model_line = from_u8(device.brand) + "  " + model_line;
    card_add(info, muted_line(info, _L("Model  ") + model_line));
    if (device.resin) {
        card_add(info, muted_line(info, _L("Kind  ") + _L("Resin")));
        add_resin_facts(info, preset_for_device(device.id));
    }
    const wxString link_text = (!linked && device.resin) ? wxString(_L("Offline"))
        : (device.link.empty() ? wxString(missing) : from_u8(device.link));
    card_add(info, muted_line(info, _L("Link  ") + link_text));
    if (linked)
        card_add(info, muted_line(info, _L("IP  ") + from_u8(device.host)));
    wxString info_note_text;
    if (device.status == "Checking")
        info_note_text = _L("Contacting this printer. This can take a few seconds.");
    else if (device.status == "Failed")
        info_note_text = device.detail.empty()
            ? wxString(_L("The printer did not answer."))
            : from_u8(device.detail);
    else if (device.status != "Connected" && linked)
        info_note_text = _L("The address is saved. Press Reload to ask this printer whether it is there.");
    else if (!linked && device.resin)
        info_note_text = _L("This resin printer is not on the network. Slice the file, copy it to the printer, and start it there. Choose Track it when exporting to follow the estimate on this computer.");
    else if (!linked)
        info_note_text = _L("This printer has no network address. The configuration wizard explains where its IP is shown. Choose Track it when exporting to follow the estimate on this computer.");
    if (!info_note_text.empty()) {
        auto* info_note = muted_line(info, info_note_text);
        fit_card_text(info_note);
        card_add(info, info_note);
    }
    finish_card(info);

    wxPanel* vat = nullptr;
    if (device.resin) {
        vat = make_card(parent, _L("Resin"));
        wxString resin_name = _L("Choose a resin in the material list.");
        if (wxGetApp().preset_bundle) {
            const Preset& selected = wxGetApp().preset_bundle->sla_materials.get_selected_preset();
            if (!selected.name.empty() && !selected.is_default)
                resin_name = from_u8(selected.name);
        }
        auto* resin_line = new wxStaticText(vat, wxID_ANY, resin_name);
        resin_line->SetForegroundColour(*wxWHITE);
        resin_line->SetBackgroundColour(k_card);
        resin_line->SetFont(wxGetApp().bold_font());
        card_add(vat, resin_line);
        auto* vat_note = muted_line(vat, _L("One resin sits in the vat. This printer has no filament spools, nozzle, heated bed, or fans to control from here."));
        fit_card_text(vat_note);
        card_add(vat, vat_note);
        finish_card(vat);
    }

    wxPanel* control = nullptr;
    wxPanel* motion = nullptr;
    if (!device.resin) {
    control = make_card(parent, _L("Control"));
    auto* reading_host = new wxPanel(control);
    reading_host->SetBackgroundColour(k_card);
    auto* readings = new wxFlexGridSizer(3, 6, 6);
    readings->AddGrowableCol(0, 1);
    readings->AddGrowableCol(1, 1);
    readings->AddGrowableCol(2, 1);
    auto* nozzle_value = add_reading(reading_host, readings, _L("Nozzle"), missing);
    auto* bed_value = add_reading(reading_host, readings, _L("Bed"), missing);
    auto* chamber_value = add_reading(reading_host, readings, _L("Chamber"), missing);
    auto* model_fan_value = add_reading(reading_host, readings, _L("Model fan"), missing);
    auto* aux_fan_value = add_reading(reading_host, readings, _L("Aux fan"), missing);
    wxStaticText* light_value = nullptr;
    CameraIconButton* light_button = nullptr;
    auto light_toggle = std::make_shared<std::function<void()>>();
    auto light_busy = std::make_shared<bool>(false);
    if (device.link == "Anycubic LAN") {
        auto* cell = new wxPanel(reading_host);
        cell->SetBackgroundColour(wxColour(38, 38, 46));
        auto* cell_sizer = new wxBoxSizer(wxVERTICAL);
        auto* title = muted_line(cell, _L("Light"));
        title->SetBackgroundColour(wxColour(38, 38, 46));
        cell_sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, 8);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        light_value = new wxStaticText(cell, wxID_ANY, missing);
        light_value->SetForegroundColour(*wxWHITE);
        light_value->SetBackgroundColour(wxColour(38, 38, 46));
        light_value->SetFont(wxGetApp().bold_font());
        light_button = new CameraIconButton(cell, CameraIconButton::Bulb, _L("Switch the chamber light on this printer."), [light_toggle]() {
            if (*light_toggle)
                (*light_toggle)();
        });
        light_button->set_enabled(!device.host.empty());
        if (camera_sessions()[device.id].light_known)
            light_button->set_lit(camera_sessions()[device.id].light);
        row->Add(light_value, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxBOTTOM, 8);
        row->AddStretchSpacer(1);
        row->Add(light_button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, 4);
        cell_sizer->Add(row, 1, wxEXPAND);
        cell->SetSizer(cell_sizer);
        readings->Add(cell, 1, wxEXPAND | wxALL, 3);
    } else {
        light_value = add_reading(reading_host, readings, _L("Light"), missing);
    }
    reading_host->SetSizer(readings);
    card_add(control, reading_host);
    const std::string control_id = device.id;
    if (camera_sessions()[control_id].light_known && control_readings()[control_id].light.empty())
        control_readings()[control_id].light = camera_sessions()[control_id].light ? wxString(_L("On")) : wxString(_L("Off"));
    auto apply_readings = [=]() {
        const ControlReading& reading = control_readings()[control_id];
        if (!reading.nozzle.empty())
            nozzle_value->SetLabel(reading.nozzle);
        if (!reading.bed.empty())
            bed_value->SetLabel(reading.bed);
        if (!reading.chamber.empty())
            chamber_value->SetLabel(reading.chamber);
        if (!reading.model_fan.empty())
            model_fan_value->SetLabel(reading.model_fan);
        if (!reading.aux_fan.empty())
            aux_fan_value->SetLabel(reading.aux_fan);
        if (!reading.light.empty())
            light_value->SetLabel(reading.light);
        if (light_button && !*light_busy) {
            const CameraSession& session = camera_sessions()[control_id];
            if (session.light_known)
                light_button->set_lit(session.light);
        }
    };
    apply_readings();
    const int control_watch = next_camera_watch();
    camera_sessions()[control_id].watchers.emplace(control_watch, apply_readings);
    auto control_alive = std::make_shared<bool>(true);
    control->Bind(wxEVT_DESTROY, [=](wxWindowDestroyEvent& event) {
        *control_alive = false;
        const auto found = camera_sessions().find(control_id);
        if (found != camera_sessions().end())
            found->second.watchers.erase(control_watch);
        event.Skip();
    });
    if (device.link == "Anycubic LAN" && !device.host.empty() && light_button) {
        const std::string control_host = device.host;
        *light_toggle = [=]() {
            if (*light_busy || !*control_alive)
                return;
            const bool turn_on = !camera_sessions()[control_id].light;
            *light_busy = true;
            light_button->set_enabled(false);
            post_printer(control_host, control_alive,
                [turn_on](Anycubic& printer, std::string& error) { return printer.set_chamber_light(turn_on, error); },
                [=](bool ok, const std::string&) {
                    *light_busy = false;
                    if (!*control_alive)
                        return;
                    light_button->set_enabled(true);
                    if (!ok) {
                        light_value->SetLabel(_L("Did not switch"));
                        return;
                    }
                    camera_sessions()[control_id].light = turn_on;
                    camera_sessions()[control_id].light_known = true;
                    control_readings()[control_id].light = turn_on ? wxString(_L("On")) : wxString(_L("Off"));
                    notify_camera_watchers(control_id);
                });
        };
    }
    auto* control_note = muted_line(control, _L("Every printer uses these same readings. A dash means this printer has not reported that value."));
    fit_card_text(control_note);
    card_add(control, control_note);
    finish_card(control);

    motion = make_card(parent, _L("Axis move"));
    auto* axis = new AxisPad(motion);
    auto* axis_note = muted_line(motion, _L("Choose a distance, then press a direction. Nothing is sent, so the nozzle cannot hit the bed from here."));
    fit_card_text(axis_note);
    axis->set_status(axis_note);
    card_add(motion, axis);
    card_add(motion, axis_note);
    finish_card(motion);
    }

    auto* left = new wxBoxSizer(wxVERTICAL);
    auto* right = new wxBoxSizer(wxVERTICAL);
    left->Add(print_task, 0, wxEXPAND | wxBOTTOM, 16);
    left->Add(camera, 0, wxEXPAND);
    right->Add(info, 0, wxEXPAND | wxBOTTOM, 16);
    if (device.resin) {
        right->Add(vat, 0, wxEXPAND);
    } else {
        right->Add(control, 0, wxEXPAND | wxBOTTOM, 16);
        right->Add(material, 0, wxEXPAND | wxBOTTOM, 16);
        right->Add(motion, 0, wxEXPAND);
    }
    board->Add(left, 1, wxEXPAND | wxRIGHT, 10);
    board->Add(right, 1, wxEXPAND | wxLEFT, 10);
}

int status_rank(const std::string& status)
{
    if (status == "Connected" || status == "Ready")
        return 0;
    if (status == "Printing")
        return 1;
    if (status == "Checking")
        return 2;
    if (status == "Idle" || status == "Offline")
        return 3;
    if (status == "Failed")
        return 4;
    return 5;
}

void identify_printer(const PhysicalPrinter& printer, std::string& brand, std::string& model)
{
    brand.clear();
    model.clear();
    if (printer.preset_names.empty() || !wxGetApp().preset_bundle)
        return;
    std::string preset_name = *printer.preset_names.begin();
    if (preset_name.find(PhysicalPrinter::separator()) != std::string::npos)
        preset_name = PhysicalPrinter::get_preset_name(preset_name);
    const Preset* preset = wxGetApp().preset_bundle->printers.find_preset(preset_name, false);
    if (!preset)
        return;
    const std::string model_id = preset->config.opt_string("printer_model");
    const PresetWithVendorProfile with_vendor = wxGetApp().preset_bundle->printers.get_preset_with_vendor_profile(*preset);
    if (with_vendor.vendor) {
        if (!with_vendor.vendor->name.empty())
            brand = with_vendor.vendor->name;
        for (const VendorProfile::PrinterModel& item : with_vendor.vendor->models) {
            if (item.id == model_id) {
                model = item.name;
                break;
            }
        }
    }
    if (model.empty())
        model = model_id.empty() ? preset->name : model_id;
}

std::string link_name(PrintHostType type)
{
    switch (type) {
    case htOctoPrint: return "OctoPrint";
    case htMoonraker: return "Klipper";
    case htPrusaLink: return "PrusaLink";
    case htPrusaConnect:
    case htPrusaConnectNew: return "Prusa Connect";
    case htDuet: return "Duet";
    case htRepetier: return "Repetier";
    case htMKS: return "MKS";
    case htFlashAir: return "FlashAir";
    case htAstroBox: return "AstroBox";
    case htAnycubic: return "Anycubic LAN";
    case htOffline: return "Offline";
    default: return {};
    }
}

class DashboardDropTarget : public wxTextDropTarget
{
public:
    DashboardDropTarget(DevicesPanel* panel, int row, int column, std::string stack_path, std::function<void(bool)> highlight)
        : m_panel(panel), m_row(row), m_column(column), m_stack_path(std::move(stack_path)), m_highlight(std::move(highlight)) {}
    bool OnDropText(wxCoord x, wxCoord y, const wxString& text) override;
    wxDragResult OnDragOver(wxCoord x, wxCoord y, wxDragResult def) override;
    void OnLeave() override;
private:
    DevicesPanel* m_panel;
    int m_row;
    int m_column;
    std::string m_stack_path;
    std::function<void(bool)> m_highlight;
};

bool limit_cameras(wxWindow* window, int height)
{
    if (!window)
        return false;
    if (auto* camera = dynamic_cast<CameraView*>(window)) {
        const wxSize before = camera->GetMinSize();
        camera->limit_height(height);
        return camera->GetMinSize() != before;
    }
    bool changed = false;
    for (wxWindowList::const_iterator it = window->GetChildren().begin(); it != window->GetChildren().end(); ++it)
        changed = limit_cameras(*it, height) || changed;
    return changed;
}

// Keep a camera inside the visible page. Its height never follows its width.
void fit_cards_inside(wxScrolledWindow* host, int preferred)
{
    static bool inside = false;
    if (inside || !host)
        return;
    const int height = std::max(120, preferred);
    inside = true;
    if (limit_cameras(host, height)) {
        host->Layout();
        host->FitInside();
    }
    inside = false;
}

const char* k_local_device = "local";
const char* k_recent_module = "recent_projects";
const char* k_stack_module = "vertical";

wxImage load_project_preview(const wxString& path)
{
    if (!path.Lower().EndsWith(".3mf") || !wxFileExists(path))
        return wxImage();
    wxFileInputStream file(path);
    if (!file.IsOk())
        return wxImage();
    wxZipInputStream zip(file);
    if (!zip.IsOk())
        return wxImage();
    std::unique_ptr<wxZipEntry> entry;
    while (entry.reset(zip.GetNextEntry()), entry) {
        const wxString name = entry->GetName().Lower();
        const bool png = name.Contains("thumbnail") && name.EndsWith(".png");
        const bool jpeg = name.Contains("thumbnail") && (name.EndsWith(".jpg") || name.EndsWith(".jpeg"));
        if (!png && !jpeg)
            continue;
        wxImage image(zip, png ? wxBITMAP_TYPE_PNG : wxBITMAP_TYPE_JPEG);
        if (!image.IsOk() || image.GetWidth() < 1 || image.GetHeight() < 1)
            continue;
        return image;
    }
    return wxImage();
}

wxBitmap preview_bitmap(const wxImage& source, int width, int height)
{
    width = std::max(1, width);
    height = std::max(1, height);
    wxBitmap framed(width, height);
    wxMemoryDC dc(framed);
    dc.SetBackground(wxBrush(wxColour(24, 24, 28)));
    dc.Clear();
    if (source.IsOk() && source.GetWidth() > 0 && source.GetHeight() > 0) {
        const double scale = std::min(double(width) / source.GetWidth(), double(height) / source.GetHeight());
        const int drawn_w = std::max(1, int(source.GetWidth() * scale));
        const int drawn_h = std::max(1, int(source.GetHeight() * scale));
        wxImage scaled = source.Copy();
        scaled.Rescale(drawn_w, drawn_h, wxIMAGE_QUALITY_BILINEAR);
        dc.DrawBitmap(wxBitmap(scaled), (width - drawn_w) / 2, (height - drawn_h) / 2, true);
    } else {
        dc.SetPen(wxPen(wxColour(254, 50, 173), 2));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        const int inset = std::max(8, std::min(width, height) / 8);
        dc.DrawRoundedRectangle(inset, inset, std::max(1, width - 2 * inset), std::max(1, height - 2 * inset), 8);
    }
    dc.SelectObject(wxNullBitmap);
    return framed;
}

class RecentProjectsCard : public wxPanel
{
public:
    explicit RecentProjectsCard(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxTAB_TRAVERSAL)
        , m_alive(std::make_shared<bool>(true))
    {
        bind_card_chrome(this);
        SetMinSize(wxSize(120, 120));
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHSCROLL | wxBORDER_NONE);
        m_scroll->SetMinSize(wxSize(48, 48));
        m_scroll->SetScrollRate(20, 0);
        m_scroll->SetBackgroundColour(k_card);
        m_strip = new wxBoxSizer(wxHORIZONTAL);
        m_scroll->SetSizer(m_strip);
        sizer->Add(m_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        SetSizer(sizer);

        if (wxGetApp().app_config) {
            for (const std::string& path : wxGetApp().app_config->get_recent_projects()) {
                if (m_items.size() >= 10)
                    break;
                Item item;
                item.path = from_u8(path);
                item.exists = wxFileExists(item.path);
                item.source = load_project_preview(item.path);
                m_items.push_back(std::move(item));
            }
        }
        if (m_items.empty()) {
            auto* empty = new wxStaticText(m_scroll, wxID_ANY, _L("No projects have been opened yet."));
            empty->SetForegroundColour(wxColour(180, 180, 188));
            empty->SetBackgroundColour(k_card);
            empty->SetMinSize(wxSize(40, -1));
            m_strip->Add(empty, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        }
        for (Item& item : m_items) {
            auto* tile = new wxPanel(m_scroll, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            tile->SetBackgroundColour(k_card);
            tile->SetToolTip(item.path);
            if (item.exists)
                tile->SetCursor(wxCURSOR_HAND);
            auto* tile_sizer = new wxBoxSizer(wxVERTICAL);
            item.thumb = new wxStaticBitmap(tile, wxID_ANY, preview_bitmap(item.source, 216, 162));
            item.thumb->SetBackgroundColour(k_card);
            tile_sizer->Add(item.thumb, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, 4);
            item.name = new wxStaticText(tile, wxID_ANY, wxFileName(item.path).GetFullName(),
                wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL | wxST_ELLIPSIZE_END);
            item.name->SetFont(wxGetApp().bold_font());
            item.name->SetForegroundColour(item.exists ? *wxWHITE : wxColour(140, 140, 148));
            item.name->SetBackgroundColour(k_card);
            item.name->SetToolTip(item.path);
            tile_sizer->Add(item.name, 0, wxEXPAND | wxTOP | wxBOTTOM, 4);
            tile->SetSizer(tile_sizer);
            if (item.exists) {
                const wxString path = item.path;
                auto open = [path](wxMouseEvent&) {
                    if (wxGetApp().mainframe)
                        wxGetApp().mainframe->open_project_tab(path);
                };
                tile->Bind(wxEVT_LEFT_UP, open);
                item.thumb->Bind(wxEVT_LEFT_UP, open);
                item.name->Bind(wxEVT_LEFT_UP, open);
                item.thumb->SetCursor(wxCURSOR_HAND);
                item.name->SetCursor(wxCURSOR_HAND);
            }
            auto wheel = [this](wxMouseEvent& event) { on_wheel(event); };
            tile->Bind(wxEVT_MOUSEWHEEL, wheel);
            item.thumb->Bind(wxEVT_MOUSEWHEEL, wheel);
            item.name->Bind(wxEVT_MOUSEWHEEL, wheel);
            m_strip->Add(tile, 0, wxEXPAND | wxRIGHT, 10);
            item.tile = tile;
        }
        m_scroll->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) { on_wheel(event); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            event.Skip();
            if (m_layout_pending || m_items.empty())
                return;
            m_layout_pending = true;
            std::shared_ptr<bool> alive = m_alive;
            CallAfter([this, alive]() {
                if (!*alive)
                    return;
                m_layout_pending = false;
                relayout();
            });
        });
    }

    ~RecentProjectsCard() override
    {
        *m_alive = false;
    }

    wxSize DoGetBestSize() const override
    {
        return GetMinSize();
    }

private:
    struct Item {
        wxString path;
        wxImage source;
        bool exists { false };
        wxPanel* tile { nullptr };
        wxStaticBitmap* thumb { nullptr };
        wxStaticText* name { nullptr };
    };

    void on_wheel(wxMouseEvent& event)
    {
        if (!m_scroll)
            return;
        if (m_scroll->GetVirtualSize().GetWidth() <= m_scroll->GetClientSize().GetWidth()) {
            event.Skip();
            return;
        }
        int x = 0;
        int y = 0;
        m_scroll->GetViewStart(&x, &y);
        const int delta = event.GetWheelRotation() / std::max(1, event.GetWheelDelta());
        m_scroll->Scroll(std::max(0, x - delta * 3), y);
    }

    void relayout()
    {
        if (!m_scroll || m_items.empty())
            return;
        const int client_h = GetClientSize().GetHeight();
        const int client_w = GetClientSize().GetWidth();
        if (client_h < 48 || client_w < 48)
            return;
        const int count = int(m_items.size());
        const int gap = 10;
        int thumb_h = std::max(72, client_h - 64);
        int thumb_w = std::max(72, thumb_h * 4 / 3);
        const int natural = count * thumb_w + std::max(0, count - 1) * gap;
        if (count > 0 && natural > client_w - 8) {
            thumb_w = std::max(48, (client_w - 8 - std::max(0, count - 1) * gap) / count);
            thumb_h = std::max(36, thumb_w * 3 / 4);
        } else if (count > 0) {
            const int grown = std::max(thumb_w, (client_w - 8 - std::max(0, count - 1) * gap) / count);
            const int grown_h = grown * 3 / 4;
            if (grown_h + 48 <= client_h) {
                thumb_w = grown;
                thumb_h = grown_h;
            }
        }
        if (thumb_h == m_laid_out_h && thumb_w == m_laid_out_w)
            return;
        m_laid_out_h = thumb_h;
        m_laid_out_w = thumb_w;
        const int tile_h = thumb_h + 36;
        for (Item& item : m_items) {
            if (!item.tile || !item.thumb || !item.name)
                continue;
            item.thumb->SetBitmap(preview_bitmap(item.source, thumb_w, thumb_h));
            item.thumb->SetMinSize(wxSize(thumb_w, thumb_h));
            item.thumb->SetMaxSize(wxSize(thumb_w, thumb_h));
            item.name->SetMinSize(wxSize(thumb_w, 22));
            item.name->SetMaxSize(wxSize(thumb_w, 22));
            item.tile->SetMinSize(wxSize(thumb_w, tile_h));
            item.tile->SetMaxSize(wxSize(thumb_w, tile_h));
        }
        m_scroll->Layout();
        m_scroll->FitInside();
    }

    wxScrolledWindow* m_scroll { nullptr };
    wxBoxSizer* m_strip { nullptr };
    std::vector<Item> m_items;
    std::shared_ptr<bool> m_alive;
    bool m_layout_pending { false };
    int m_laid_out_h { -1 };
    int m_laid_out_w { -1 };
};

} // namespace

DeviceModuleRegistry& DeviceModuleRegistry::instance()
{
    static DeviceModuleRegistry registry;
    return registry;
}

DeviceModuleRegistry::DeviceModuleRegistry()
{
    register_module({
        "now_printing",
        _L("Print task"),
        [](wxWindow* parent, const DeviceEntry& device) {
            auto* panel = make_card(parent, wxString());
            fill_print_task(panel, device);
            finish_card(panel);
            return panel;
        }
    });
    register_module({
        "material",
        _L("Material"),
        [](wxWindow* parent, const DeviceEntry& device) {
            return fill_material_card(parent, device, false);
        }
    });
    register_module({
        "temperatures",
        _L("Control"),
        [](wxWindow* parent, const DeviceEntry& device) {
            const wxString body = device.resin
                ? _L("This is a resin printer. It has no nozzle, heated bed, or fans to read from here.")
                : (device.host.empty()
                ? _L("No network address is saved, so nozzle, bed, and chamber stay blank. Placing this module does not contact the printer.")
                : _L("Nozzle —\nBed —\nChamber —\nThis connection has not reported those readings. Placing this module does not open a connection."));
            return make_placeholder(parent, wxString(), body);
        }
    });
    register_module({
        "camera",
        _L("Camera"),
        [](wxWindow* parent, const DeviceEntry& device) {
            auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
            bind_card_chrome(panel);
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            auto* view = new CameraView(panel, device, 400, false, nullptr);
            view->SetMinSize(wxSize(64, 160));
            sizer->Add(view, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
            sizer->AddSpacer(16);
            panel->SetSizer(sizer);
            return panel;
        }
    });
    register_module({
        k_recent_module,
        _L("Recent projects"),
        [](wxWindow* parent, const DeviceEntry&) {
            return new RecentProjectsCard(parent);
        }
    });
}

void DeviceModuleRegistry::register_module(DeviceModuleDesc desc)
{
    for (DeviceModuleDesc& existing : m_modules) {
        if (existing.key == desc.key) {
            existing = std::move(desc);
            return;
        }
    }
    m_modules.push_back(std::move(desc));
}

const DeviceModuleDesc* DeviceModuleRegistry::find(const std::string& key) const
{
    for (const DeviceModuleDesc& desc : m_modules)
        if (desc.key == key)
            return &desc;
    return nullptr;
}

std::vector<DeviceEntry> bound_devices()
{
    std::vector<DeviceEntry> devices;
    if (!wxGetApp().preset_bundle)
        return devices;

    const PhysicalPrinterCollection& printers = wxGetApp().preset_bundle->physical_printers;
    for (const PhysicalPrinter& printer : printers) {
        DeviceEntry entry;
        entry.id = printer.name;
        entry.name = printer.name;
        identify_printer(printer, entry.brand, entry.model);
        entry.host = printer.config.opt_string("print_host");
        const bool technology_known = printer.config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology") != nullptr;
        if (const auto* technology = printer.config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology"))
            entry.resin = technology->value == ptSLA;
        if (!technology_known && !printer.preset_names.empty() && wxGetApp().preset_bundle) {
            std::string preset_name = *printer.preset_names.begin();
            if (preset_name.find(PhysicalPrinter::separator()) != std::string::npos)
                preset_name = PhysicalPrinter::get_preset_name(preset_name);
            if (const Preset* preset = wxGetApp().preset_bundle->printers.find_preset(preset_name, false))
                entry.resin = preset->printer_technology() == ptSLA;
        }
        if (const auto* type = printer.config.option<ConfigOptionEnum<PrintHostType>>("host_type"))
            entry.link = link_name(type->value);
        if (entry.resin && entry.host.empty())
            entry.link = "Offline";
        const bool has_host = PhysicalPrinter::has_print_host_information(printer.config);
        entry.active = true;
        entry.offline = !has_host;
        entry.status = has_host ? "Idle" : "Offline";
        devices.push_back(std::move(entry));
    }
    return devices;
}

namespace {

wxDragResult DashboardDropTarget::OnDragOver(wxCoord, wxCoord, wxDragResult def)
{
    if (m_highlight)
        m_highlight(true);
    return def == wxDragNone ? wxDragCopy : def;
}

void DashboardDropTarget::OnLeave()
{
    if (m_highlight)
        m_highlight(false);
}

bool DashboardDropTarget::OnDropText(wxCoord, wxCoord, const wxString& text)
{
    const std::string payload = into_u8(text);
    if (m_panel == nullptr || m_row < 0 || m_column < 0)
        return false;
    if (m_highlight)
        m_highlight(false);
    // The window that started this drag is still inside its mouse event.
    // Rebuilding now would delete it before the event returns.
    if (payload.rfind("move\n", 0) == 0) {
        try {
            const std::string rest = payload.substr(5);
            const size_t tab = rest.find('\t');
            if (tab == std::string::npos)
                return false;
            const std::string tail = rest.substr(tab + 1);
            const size_t tab2 = tail.find('\t');
            const std::string from_path = tab2 == std::string::npos ? std::string() : tail.substr(tab2 + 1);
            m_panel->queue_dashboard_move(std::stoi(rest.substr(0, tab)), std::stoi(tab2 == std::string::npos ? tail : tail.substr(0, tab2)), from_path, m_row, m_column, m_stack_path);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
    const size_t split = payload.find('\n');
    if (split == std::string::npos)
        return false;
    m_panel->queue_dashboard_drop(payload.substr(0, split), payload.substr(split + 1), m_row, m_column, m_stack_path);
    return true;
}

void start_module_drag(DevicesPanel* panel, wxWindow* source, const wxString& payload)
{
    if (panel == nullptr || source == nullptr || payload.empty())
        return;
    if (wxWindow::GetCapture() == source)
        source->ReleaseMouse();
    wxTextDataObject data(payload);
    wxDropSource drag(data, source);
    drag.DoDragDrop(wxDrag_CopyOnly);
    panel->finish_module_drag();
}

constexpr int k_device_list_min_width = 260;

int saved_device_list_width()
{
    int width = k_device_list_min_width;
    if (!wxGetApp().app_config)
        return width;
    const std::string saved = wxGetApp().app_config->get("devices_sidebar_width");
    if (saved.empty())
        return width;
    try {
        const int value = std::stoi(saved);
        if (value > width)
            width = value;
    } catch (const std::exception&) {
    }
    return width;
}

class DashSlot : public wxPanel
{
public:
    DashSlot(wxWindow* parent, bool editing, bool filled, bool compact = false)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE | wxCLIP_CHILDREN)
        , m_editing(editing)
        , m_filled(filled)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxGetApp().get_window_default_clr());
        if (!filled)
            SetMinSize(compact ? wxSize(120, 72) : wxSize(180, 200));
        Bind(wxEVT_PAINT, &DashSlot::on_paint, this);
    }

    void set_hot(bool hot)
    {
        if (m_hot == hot)
            return;
        m_hot = hot;
        Refresh();
    }

private:
    bool m_editing;
    bool m_filled;
    bool m_hot { false };

    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        const wxColour page = GetBackgroundColour();
        dc.SetPen(page);
        dc.SetBrush(m_hot && m_editing && !m_filled ? wxColour(72, 36, 86) : page);
        dc.DrawRectangle(0, 0, size.x, size.y);
        if (!m_editing || size.x < 8 || size.y < 8)
            return;
        const wxColour edge = wxColour(254, 50, 173);
        if (m_hot || !m_filled) {
            dc.SetPen(wxPen(edge, m_hot ? 3 : 2, m_hot || m_filled ? wxPENSTYLE_SOLID : wxPENSTYLE_SHORT_DASH));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(4, 4, size.x - 8, size.y - 8, 12);
        }
        if (m_filled)
            return;
        dc.SetFont(wxGetApp().bold_font());
        dc.SetTextForeground(m_hot ? *wxWHITE : wxColour(254, 50, 173));
        const wxString label = m_hot ? _L("Release to place here") : _L("Drop here");
        const wxSize text = dc.GetTextExtent(label);
        dc.DrawText(label, std::max(12, (size.x - text.x) / 2), (size.y - text.y) / 2);
    }
};

class RowSplitSash : public wxPanel
{
public:
    RowSplitSash(wxWindow* parent, std::function<void(int)> on_split, std::function<void()> on_done)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(8, -1))
        , m_on_split(std::move(on_split))
        , m_on_done(std::move(on_done))
    {
        SetMinSize(wxSize(8, -1));
        SetBackgroundColour(wxColour(60, 60, 68));
        SetCursor(wxCursor(wxCURSOR_SIZEWE));
        SetToolTip(_L("Drag to change how wide each side is."));
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            CaptureMouse();
            m_drag = true;
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (HasCapture())
                ReleaseMouse();
            m_drag = false;
            if (m_on_done)
                m_on_done();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_drag = false; });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_drag || !m_on_split || !GetParent())
                return;
            const int width = std::max(1, GetParent()->GetClientSize().GetWidth());
            const int x = GetParent()->ScreenToClient(ClientToScreen(event.GetPosition())).x;
            int percent = x * 100 / width;
            if (percent < 20)
                percent = 20;
            if (percent > 80)
                percent = 80;
            m_on_split(percent);
        });
    }

private:
    std::function<void(int)> m_on_split;
    std::function<void()> m_on_done;
    bool m_drag { false };
};

class StackSplitSash : public wxPanel
{
public:
    StackSplitSash(wxWindow* parent, std::function<void(int)> on_split, std::function<void()> on_done)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 8))
        , m_on_split(std::move(on_split))
        , m_on_done(std::move(on_done))
    {
        SetMinSize(wxSize(-1, 8));
        SetBackgroundColour(wxColour(60, 60, 68));
        SetCursor(wxCursor(wxCURSOR_SIZENS));
        SetToolTip(_L("Drag to change how tall each module is."));
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            CaptureMouse();
            m_drag = true;
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (HasCapture())
                ReleaseMouse();
            m_drag = false;
            if (m_on_done)
                m_on_done();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_drag = false; });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_drag || !m_on_split || !GetParent())
                return;
            const int height = std::max(1, GetParent()->GetClientSize().GetHeight());
            const int y = GetParent()->ScreenToClient(ClientToScreen(event.GetPosition())).y;
            int percent = y * 100 / height;
            if (percent < 20)
                percent = 20;
            if (percent > 80)
                percent = 80;
            m_on_split(percent);
        });
    }

private:
    std::function<void(int)> m_on_split;
    std::function<void()> m_on_done;
    bool m_drag { false };
};

class RowHeightSash : public wxPanel
{
public:
    RowHeightSash(wxWindow* parent, wxWindow* row, wxScrolledWindow* canvas, std::function<void(int)> on_height, std::function<void()> on_done)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 8))
        , m_row(row)
        , m_canvas(canvas)
        , m_on_height(std::move(on_height))
        , m_on_done(std::move(on_done))
    {
        SetMinSize(wxSize(-1, 8));
        SetBackgroundColour(wxColour(60, 60, 68));
        SetCursor(wxCursor(wxCURSOR_SIZENS));
        SetToolTip(_L("Drag to change how tall this row is."));
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            CaptureMouse();
            m_drag = true;
            m_start_y = ClientToScreen(event.GetPosition()).y;
            m_start_h = m_row ? m_row->GetSize().GetHeight() : 220;
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (HasCapture())
                ReleaseMouse();
            m_drag = false;
            if (m_on_done)
                m_on_done();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_drag = false; });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_drag || !m_row || !m_on_height)
                return;
            int height = m_start_h + ClientToScreen(event.GetPosition()).y - m_start_y;
            if (height < 140)
                height = 140;
            if (height > 1600)
                height = 1600;
            m_on_height(height);
            limit_cameras(m_row, std::max(120, height - 72));
            m_row->SetMinSize(wxSize(48, height));
            if (m_canvas) {
                m_canvas->Layout();
                m_canvas->FitInside();
            }
        });
    }

private:
    wxWindow* m_row;
    wxScrolledWindow* m_canvas;
    std::function<void(int)> m_on_height;
    std::function<void()> m_on_done;
    bool m_drag { false };
    int m_start_y { 0 };
    int m_start_h { 0 };
};

class DeviceListSash : public wxPanel
{
public:
    DeviceListSash(wxWindow* parent, wxWindow* target)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(6, -1))
        , m_target(target)
    {
        SetMinSize(wxSize(6, -1));
        SetBackgroundColour(wxColour(60, 60, 68));
        SetCursor(wxCursor(wxCURSOR_SIZEWE));
        SetToolTip(_L("Drag to resize"));
        Bind(wxEVT_LEFT_DOWN, &DeviceListSash::on_down, this);
        Bind(wxEVT_LEFT_UP, &DeviceListSash::on_up, this);
        Bind(wxEVT_MOTION, &DeviceListSash::on_motion, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_drag = false; });
    }

private:
    void on_down(wxMouseEvent& event)
    {
        if (!m_target)
            return;
        CaptureMouse();
        m_drag = true;
        m_start_x = ClientToScreen(event.GetPosition()).x;
        m_start_w = m_target->GetSize().GetWidth();
    }

    void on_up(wxMouseEvent&)
    {
        if (HasCapture())
            ReleaseMouse();
        if (m_drag && m_target && wxGetApp().app_config) {
            wxGetApp().app_config->set("devices_sidebar_width", std::to_string(m_target->GetSize().GetWidth()));
            wxGetApp().app_config->save();
        }
        m_drag = false;
    }

    void on_motion(wxMouseEvent& event)
    {
        if (!m_drag || !m_target || !GetParent())
            return;
        const int delta = ClientToScreen(event.GetPosition()).x - m_start_x;
        const int em = std::max(1, wxGetApp().em_unit());
        const int max_w = std::max(k_device_list_min_width + em, GetParent()->GetSize().GetWidth() - 20 * em);
        int width = m_start_w + delta;
        if (width < k_device_list_min_width)
            width = k_device_list_min_width;
        if (width > max_w)
            width = max_w;
        m_target->SetMinSize(wxSize(width, -1));
        GetParent()->Layout();
    }

    wxWindow* m_target { nullptr };
    bool      m_drag { false };
    int       m_start_x { 0 };
    int       m_start_w { 0 };
};

class DeviceCard : public wxPanel
{
public:
    DeviceCard(wxWindow* parent, std::string id, const wxString& name, const wxString& type, const wxString& status, const wxColour& status_colour, bool selected, bool offline, std::function<void()> on_select)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
        , m_id(std::move(id))
        , m_name(name)
        , m_type(type)
        , m_status(status)
        , m_status_colour(status_colour)
        , m_selected(selected)
        , m_offline(offline)
        , m_on_select(std::move(on_select))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(parent->GetBackgroundColour());
        const int text_height = wxGetApp().em_unit() * 9;
        SetMinSize(wxSize(-1, std::max(m_offline ? 120 : 108, text_height)));
        if (m_offline)
            SetToolTip(_L("Offline printer. It has no network connection, so a print is tracked on this computer."));
        Bind(wxEVT_PAINT, &DeviceCard::on_paint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (m_on_select)
                m_on_select();
        });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) {
            m_hover = true;
            Refresh();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            m_hover = false;
            Refresh();
        });
    }

    const std::string& device_id() const { return m_id; }

    void set_selected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        Refresh();
    }

    void set_status(const wxString& status, const wxColour& colour)
    {
        m_status = status;
        m_status_colour = colour;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        paint_buffered(this, dc, [&](wxGraphicsContext& context, const wxSize& size) {
        wxGraphicsContext* gc = &context;

        const double inset = 1.0;
        const wxColour fill = m_hover && !m_selected ? wxColour(44, 44, 52) : wxColour(32, 32, 38);
        gc->SetBrush(wxBrush(fill));
        gc->SetPen(m_selected ? wxPen(wxColour(47, 140, 255), 2) : wxPen(wxColour(54, 54, 62), 1));
        gc->DrawRoundedRectangle(inset, inset, std::max(1.0, size.x - inset * 2), std::max(1.0, size.y - inset * 2), 8);

        wxFont name_font = wxGetApp().bold_font();
        name_font.SetPointSize(std::max(8, name_font.GetPointSize() * 2));
        wxFont type_font = wxGetApp().normal_font();
        type_font.SetPointSize(std::max(8, type_font.GetPointSize() * 2));
        double name_w = 0, name_h = 0, status_w = 0, status_h = 0;
        gc->SetFont(name_font, m_status_colour);
        gc->GetTextExtent(m_status, &status_w, &status_h);
        gc->SetFont(name_font, *wxWHITE);
        gc->GetTextExtent(m_name, &name_w, &name_h);

        const double pad = 16;
        const double name_y = 16;
        const double status_x = std::max(pad, size.GetWidth() - pad - status_w);
        const double name_limit = std::max(0.0, status_x - pad - 10);
        gc->Clip(pad, 0, name_limit, size.y);
        gc->DrawText(m_name, pad, name_y);
        gc->ResetClip();

        gc->SetFont(name_font, m_status_colour);
        gc->DrawText(m_status, status_x, name_y);

        gc->SetFont(type_font, wxColour(158, 158, 166));
        const double type_width = m_offline
            ? std::max(0.0, size.GetWidth() - pad - 46)
            : std::max(0.0, size.GetWidth() - pad * 2);
        gc->Clip(pad, 0, type_width, size.y);
        gc->DrawText(m_type, pad, name_y + name_h + 6);
        gc->ResetClip();

        if (m_offline) {
            const double box = 22;
            const double x = size.GetWidth() - 16 - box;
            const double y = size.GetHeight() - 14 - box;
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(wxColour(58, 49, 28)));
            gc->DrawRoundedRectangle(x, y, box, box, 6);
            gc->SetBrush(wxBrush(wxColour(232, 176, 74)));
            gc->DrawEllipse(x + 5, y + 8, 4.5, 4.5);
            gc->DrawEllipse(x + 12.5, y + 8, 4.5, 4.5);
            gc->SetPen(wxPen(wxColour(232, 176, 74), 2));
            gc->StrokeLine(x + 5, y + 17, x + 17, y + 5);
        }
        });
    }

    std::string m_id;
    wxString    m_name;
    wxString    m_type;
    wxString    m_status;
    wxColour    m_status_colour;
    bool        m_selected { false };
    bool        m_offline { false };
    bool        m_hover { false };
    std::function<void()> m_on_select;
};

void mark_device_cards(wxSizer* rows, const std::string& selected_id)
{
    if (!rows)
        return;
    for (size_t i = 0; i < rows->GetItemCount(); ++i) {
        wxSizerItem* item = rows->GetItem(i);
        auto* card = item ? dynamic_cast<DeviceCard*>(item->GetWindow()) : nullptr;
        if (card)
            card->set_selected(card->device_id() == selected_id);
    }
}

} // namespace

DevicesPanel::DevicesPanel(wxWindow* parent)
    : wxPanel(parent)
    , m_alive(std::make_shared<bool>(true))
{
    g_devices_panel = this;
    DeviceModuleRegistry::instance();
    wxGetApp().UpdateDarkUI(this);

    m_share_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { publish_dashboard_snapshot(); }, m_share_timer.GetId());
    m_job_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll_jobs(); }, m_job_timer.GetId());

    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* share = new wxPanel(this);
    wxGetApp().UpdateDarkUI(share);
    auto* share_row = new wxBoxSizer(wxHORIZONTAL);
    auto* share_text = new wxBoxSizer(wxVERTICAL);
    m_share = new wxCheckBox(share, wxID_ANY, _L("Share this dashboard on the home network"));
    m_share->SetValue(wxGetApp().app_config && wxGetApp().app_config->get_bool("dashboard_site"));
    m_share_status = new wxStaticText(share, wxID_ANY, wxString());
    share_text->Add(m_share, 0, wxEXPAND);
    share_text->Add(m_share_status, 0, wxEXPAND | wxTOP, 4);
    share_row->Add(share_text, 1, wxEXPAND | wxRIGHT, 8);
    m_back = new wxButton(share, wxID_ANY, _L("Dashboard"));
    m_back->SetToolTip(_L("Return to the dashboard."));
    wxGetApp().UpdateDarkUI(m_back, true);
    m_back->Hide();
    share_row->Add(m_back, 0, wxALIGN_TOP | wxRIGHT, 8);
    m_edit = new wxButton(share, wxID_ANY, _L("Edit dashboard"));
    m_edit->SetToolTip(_L("Show the modules you can add, and let you rearrange the ones already on the dashboard."));
    wxGetApp().UpdateDarkUI(m_edit, true);
    share_row->Add(m_edit, 0, wxALIGN_TOP | wxRIGHT, 8);
    m_reload = new wxButton(share, wxID_ANY, _L("Reload"));
    m_reload->SetToolTip(_L("Ask the selected printer if it is there, and refresh this page."));
    wxGetApp().UpdateDarkUI(m_reload, true);
    share_row->Add(m_reload, 0, wxALIGN_TOP);
    m_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_view = 1;
        show_mode();
    });
    m_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_dashboard_editing(!m_dashboard_edit); });
    share->SetSizer(share_row);
    m_reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_selected(); });
    outer->Add(share, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    m_share->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        if (!wxGetApp().app_config)
            return;
        wxGetApp().app_config->set("dashboard_site", event.IsChecked() ? "1" : "0");
        wxGetApp().app_config->save();
        apply_dashboard_site();
    });

    auto* root = new wxBoxSizer(wxHORIZONTAL);

    auto* left = new wxPanel(this);
    m_list_panel = left;
    wxGetApp().UpdateDarkUI(left);
    auto* left_sizer = new wxBoxSizer(wxVERTICAL);
    m_list_tabs = new DeviceTabBar(left, { _L("Active"), _L("All") }, [this, left](int page) {
        m_list_page = page;
        if (m_sort_wrap)
            m_sort_wrap->Show(page == 1);
        rebuild_lists();
        left->Layout();
    });
    left_sizer->Add(m_list_tabs, 0, wxEXPAND | wxLEFT | wxTOP, 4);
    m_sort_wrap = new wxPanel(left);
    wxGetApp().UpdateDarkUI(m_sort_wrap);
    auto* sort_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_sort_tabs = new DeviceTabBar(m_sort_wrap, { _L("Status"), _L("Brand") }, [this](int page) {
        m_sort_brand = page == 1;
        rebuild_lists();
    });
    sort_sizer->Add(m_sort_tabs, 0, wxEXPAND);
    m_sort_wrap->SetSizer(sort_sizer);
    m_sort_wrap->Hide();
    left_sizer->Add(m_sort_wrap, 0, wxEXPAND | wxLEFT, 4);
    m_device_list = new wxScrolledWindow(left, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_device_list->SetScrollRate(0, 16);
    m_device_list->SetBackgroundColour(wxGetApp().get_window_default_clr());
    m_device_rows = new wxBoxSizer(wxVERTICAL);
    m_device_list->SetSizer(m_device_rows);
    left_sizer->Add(m_device_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    left->SetSizer(left_sizer);
    left->SetMinSize(wxSize(saved_device_list_width(), -1));
    root->Add(left, 0, wxEXPAND);
    m_list_sash = new DeviceListSash(this, left);
    root->Add(m_list_sash, 0, wxEXPAND);

    auto* right = new wxPanel(this);
    wxGetApp().UpdateDarkUI(right);
    auto* right_sizer = new wxBoxSizer(wxVERTICAL);

    m_detail = new wxScrolledWindow(right, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_detail->SetScrollRate(0, 16);
    m_detail->SetBackgroundColour(wxGetApp().get_window_default_clr());
    m_detail->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_detail->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        fit_cards_inside(m_detail, 720);
    });
    right_sizer->Add(m_detail, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    m_dashboard = new wxPanel(right);
    wxGetApp().UpdateDarkUI(m_dashboard);
    auto* dash_sizer = new wxBoxSizer(wxVERTICAL);
    m_dash_hint = new wxStaticText(m_dashboard, wxID_ANY,
        _L("Drop a module on a highlighted side. Drag the bar between the two sides to change their width. Drop a vertical container on a side, or into another vertical container, to stack two things."));
    m_dash_hint->SetMinSize(wxSize(40, -1));
    m_dash_hint->Wrap(640);
    m_dash_hint->Hide();
    dash_sizer->Add(m_dash_hint, 0, wxEXPAND | wxALL, 6);

    auto* dash_split = new wxBoxSizer(wxHORIZONTAL);
    m_palette = new wxScrolledWindow(m_dashboard, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_palette->SetScrollRate(0, 16);
    m_palette->SetMinSize(wxSize(220, -1));
    wxGetApp().UpdateDarkUI(m_palette);
    m_palette->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_palette->Hide();
    dash_split->Add(m_palette, 0, wxEXPAND | wxRIGHT, 6);

    m_canvas = new wxScrolledWindow(m_dashboard, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_canvas->SetScrollRate(0, 16);
    wxGetApp().UpdateDarkUI(m_canvas);
    m_canvas_sizer = new wxBoxSizer(wxVERTICAL);
    m_canvas->SetSizer(m_canvas_sizer);
#ifdef __WINDOWS__
    // Present the whole dashboard in one blit. Without this, each card erases
    // and repaints on its own while the window frame is dragged.
    m_canvas->SetDoubleBuffered(true);
#endif
    m_canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        event.Skip();
        fit_cards_inside(m_canvas, 400);
        scale_dashboard_modules();
    });
    dash_split->Add(m_canvas, 1, wxEXPAND);
    dash_sizer->Add(dash_split, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
    m_dashboard->SetSizer(dash_sizer);
    m_dashboard->Hide();
    right_sizer->Add(m_dashboard, 1, wxEXPAND);
    right->SetSizer(right_sizer);
    root->Add(right, 1, wxEXPAND);
    outer->Add(root, 1, wxEXPAND);
    SetSizer(outer);

    Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {
        evt.Skip();
        if (evt.IsShown())
            rebuild_lists();
    });
    Bind(wxEVT_SIZE, [left](wxSizeEvent& event) {
        event.Skip();
        const int em = std::max(1, wxGetApp().em_unit());
        const int max_w = std::max(k_device_list_min_width, event.GetSize().GetWidth() - 20 * em);
        if (left->GetMinSize().GetWidth() > max_w) {
            left->SetMinSize(wxSize(max_w, -1));
            left->GetParent()->Layout();
        }
    });

    load_dashboard();
    rebuild_lists();
    show_mode();
    apply_dashboard_site();
    m_job_timer.Start(5000);
    poll_jobs();
}

std::vector<DeviceEntry> DevicesPanel::visible_devices(bool active_only) const
{
    std::vector<DeviceEntry> shown;
    for (const DeviceEntry& device : m_devices)
        if (!active_only || device.active)
            shown.push_back(device);

    const bool by_brand = !active_only && m_sort_brand;
    std::sort(shown.begin(), shown.end(), [by_brand](const DeviceEntry& a, const DeviceEntry& b) {
        if (by_brand) {
            if (a.brand != b.brand)
                return a.brand < b.brand;
        } else if (status_rank(a.status) != status_rank(b.status))
            return status_rank(a.status) < status_rank(b.status);
        return a.name < b.name;
    });
    return shown;
}

DevicesPanel::~DevicesPanel()
{
    if (m_alive)
        *m_alive = false;
    if (g_devices_panel == this)
        g_devices_panel = nullptr;
}

void DevicesPanel::poll_jobs()
{
    refresh_offline_jobs(true);
    if (!m_alive || !*m_alive)
        return;
    static int network_tick = 0;
    if (++network_tick % 3 != 1)
        return;
    for (const DeviceEntry& device : m_devices) {
        if (device.link != "Anycubic LAN" || device.host.empty())
            continue;
        if (!job_queries().insert(device.id).second)
            continue;
        const std::string id = device.id;
        const std::string host = device.host;
        auto alive = m_alive;
        std::thread([id, host, alive]() {
            AnycubicReadings fetched;
            bool ok = false;
            try {
                DynamicPrintConfig config;
                config.set_key_value("print_host", new ConfigOptionString(host));
                Anycubic printer(&config);
                std::string error;
                ok = printer.read_status(fetched, error);
            } catch (const std::exception&) {
                ok = false;
            }
            if (!wxTheApp)
                return;
            wxTheApp->CallAfter([id, fetched, ok, alive]() {
                job_queries().erase(id);
                if (!ok || !alive || !*alive)
                    return;
                store_control_reading(id, fetched);
            });
        }).detach();
    }
}

void DevicesPanel::apply_live_status(const std::string& id)
{
    const auto live = control_readings().find(id);
    if (live == control_readings().end() || !live->second.job_known)
        return;
    std::string status;
    for (DeviceEntry& device : m_devices) {
        if (device.id != id)
            continue;
        if (live->second.printing)
            device.status = "Printing";
        else if (device.status == "Printing") {
            const auto check = m_checks.find(id);
            if (check != m_checks.end())
                device.status = check->second.status;
            else if (device.host.empty())
                device.status = "Offline";
            else
                device.status = "Connected";
        }
        status = device.status;
    }
    if (status.empty() || m_device_rows == nullptr)
        return;
    const wxString label = list_status_label(status);
    const wxColour colour = status_colour(status, true);
    for (wxSizerItemList::compatibility_iterator node = m_device_rows->GetChildren().GetFirst(); node; node = node->GetNext()) {
        wxSizerItem* item = node->GetData();
        auto* card = item == nullptr ? nullptr : dynamic_cast<DeviceCard*>(item->GetWindow());
        if (card != nullptr && card->device_id() == id)
            card->set_status(label, colour);
    }
}

void DevicesPanel::reload()
{
    rebuild_lists();
}

void DevicesPanel::ensure_print_task_tile(const std::string& device_id)
{
    if (device_id.empty())
        return;
    std::function<bool(const DashItem&)> holds_task = [&](const DashItem& item) {
        if (item.is_stack()) {
            for (const DashItem& child : item.stack)
                if (holds_task(child))
                    return true;
            return false;
        }
        return item.device_id == device_id && item.module_key == "now_printing";
    };
    for (const DashRow& row : m_rows) {
        for (const DashCell& cell : row.cells) {
            if (!cell.is_stack() && cell.device_id == device_id && cell.module_key == "now_printing")
                return;
            if (cell.is_stack() && (holds_task(cell.stack[0]) || holds_task(cell.stack[1])))
                return;
        }
    }
    add_dashboard_tile(device_id, "now_printing");
}

void DevicesPanel::refresh_selected()
{
    refresh_device(m_selected_id);
}

void DevicesPanel::on_tab_activated()
{
    rebuild_lists();
    std::vector<std::string> ids;
    ids.reserve(m_devices.size());
    for (const DeviceEntry& device : m_devices)
        ids.push_back(device.id);
    for (const std::string& id : ids)
        refresh_device(id);
}

void DevicesPanel::refresh_device(const std::string& id)
{
    if (id.empty() || !wxGetApp().preset_bundle)
        return;
    const PhysicalPrinter* printer = wxGetApp().preset_bundle->physical_printers.find_printer(id, true);
    if (!printer)
        return;

    const std::string device_id = id;
    const std::string host = printer->config.opt_string("print_host");
    if (host.empty()) {
        DeviceCheck check;
        check.status = "Offline";
        check.detail = into_u8(_L("Offline printer. There is no network address. Export a file and choose Track it to show the print here."));
        m_checks[device_id] = std::move(check);
        rebuild_lists();
        return;
    }

    DynamicPrintConfig config(printer->config);
    if (config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology") == nullptr
        && !printer->preset_names.empty()) {
        const std::string preset_name = PhysicalPrinter::get_preset_name(*printer->preset_names.begin());
        if (const Preset* preset = wxGetApp().preset_bundle->printers.find_preset(preset_name, false)) {
            if (const auto* technology = preset->config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology"))
                config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(technology->value));
        }
    }
    bool resin_sl1 = false;
    if (const auto* technology = config.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology")) {
        if (technology->value == ptSLA) {
            const auto* type = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
            resin_sl1 = type == nullptr || type->value != htPrusaConnectNew;
        }
    }

    const int generation = ++m_check_generation;
    DeviceCheck pending;
    pending.generation = generation;
    pending.status = "Checking";
    m_checks[device_id] = pending;
    ++m_checks_in_flight;
    if (m_reload)
        m_reload->Enable(false);
    rebuild_lists();

    std::shared_ptr<bool> alive = m_alive;
    std::thread([alive, this, id = device_id, generation, config, resin_sl1]() {
        bool ok = false;
        std::string reported;
        std::string detail;
        try {
            DynamicPrintConfig local(config);
            std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&local));
            if (!host) {
                detail = "This printer has no connection that can be opened.";
            } else if (!host->can_test()) {
                detail = "This host does not answer a connection test. The address is saved, but it was not checked.";
            } else {
                wxString curl_msg;
                ok = host->test(curl_msg);
                if (ok)
                    reported = printer_reported_name(into_u8(curl_msg));
                const wxString message = ok ? host->get_test_ok_msg() : host->get_test_failed_msg(curl_msg);
                detail = safe_printer_text(into_u8(message));
            }
        } catch (const std::exception& ex) {
            detail = safe_printer_text(ex.what());
        } catch (...) {
            detail = "The connection test stopped unexpectedly.";
        }

        if (!wxTheApp)
            return;
        wxTheApp->CallAfter([alive, this, id, generation, ok, reported, detail, resin_sl1]() {
            if (!alive || !*alive)
                return;
            if (m_checks_in_flight > 0)
                --m_checks_in_flight;
            if (m_reload)
                m_reload->Enable(m_checks_in_flight == 0);
            const auto found = m_checks.find(id);
            if (found == m_checks.end() || found->second.generation != generation)
                return;
            found->second.status = ok ? "Connected" : "Failed";
            found->second.reported = reported;
            found->second.detail = detail;
            if (resin_sl1)
                found->second.detail += into_u8(_L("\n\nThis is a resin printer, so the test used the SL1 connection. That is the connection used when a print is sent."));
            rebuild_lists();
        });
    }).detach();
}

void DevicesPanel::adopt_selected_offline_printer()
{
    ensure_selected_offline_device();
    rebuild_lists();
}

void DevicesPanel::rebuild_lists()
{
    refresh_offline_jobs(false);
    m_devices = bound_devices();
    for (DeviceEntry& device : m_devices) {
        const auto found = m_checks.find(device.id);
        if (found == m_checks.end())
            continue;
        device.status = found->second.status;
        device.reported = found->second.reported;
        device.detail = found->second.detail;
        const auto live = control_readings().find(device.id);
        if (live != control_readings().end() && live->second.job_known && live->second.printing)
            device.status = "Printing";
    }
    const auto devices = visible_devices(m_list_page == 0);
    if (m_selected_id.empty() && !devices.empty())
        m_selected_id = devices.front().id;
    if (m_device_rows)
        m_device_rows->Clear(true);
    for (const DeviceEntry& device : devices) {
        const bool selected = device.id == m_selected_id;
        const std::string id = device.id;
        auto* card = new DeviceCard(
            m_device_list,
            id,
            from_u8(device_display_name(device)),
            device_type_text(device),
            list_status_label(device.status),
            status_colour(device.status, !device.host.empty()),
            selected,
            device.offline,
            [this, id]() {
                m_selected_id = id;
                m_view = 0;
                mark_device_cards(m_device_rows, id);
                show_selected_device();
                show_mode();
            });
        m_device_rows->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
    }
    if (m_device_list) {
        m_device_list->Layout();
        m_device_list->FitInside();
    }
    show_selected_device();
    rebuild_dashboard();
    if (DashboardSite::instance().running())
        publish_dashboard_snapshot();
}

void DevicesPanel::update_share_status()
{
    if (!m_share_status)
        return;
    wxString text = _L("Open this page from another device on your home network.");
    for (const std::string& url : DashboardSite::instance().urls()) {
        text += "\n";
        text += from_u8(url);
    }
    text += "\n";
    text += _L("This page stays on your home network. From outside the house, connect a VPN to this network first. If Windows asks, allow Slice it All on private networks.");
    m_share_status->SetLabel(text);
    m_share_status->Wrap(640);
}

void DevicesPanel::apply_dashboard_site()
{
    const bool enabled = wxGetApp().app_config && wxGetApp().app_config->get_bool("dashboard_site");
    if (m_share && m_share->GetValue() != enabled)
        m_share->SetValue(enabled);

    if (!enabled) {
        DashboardSite::instance().stop();
        if (m_share_timer.IsRunning())
            m_share_timer.Stop();
        if (m_share_status) {
            m_share_status->SetLabel(_L("Turn this on to open the same dashboard in a browser on this network. From outside the house, use a VPN into this network."));
            m_share_status->Wrap(640);
        }
        return;
    }

    publish_dashboard_snapshot();
    std::string error;
    if (!DashboardSite::instance().running()
        && !DashboardSite::instance().start(DashboardSite::default_port, error)) {
        if (m_share_status) {
            m_share_status->SetLabel(error == "in_use"
                ? _L("Port 8765 is already in use on this computer.")
                : _L("The dashboard site could not start."));
            m_share_status->Wrap(640);
        }
        return;
    }
    if (!m_share_timer.IsRunning())
        m_share_timer.Start(2000);
    update_share_status();
}

void DevicesPanel::show_mode()
{
    const bool dashboard = m_view == 1;
    if (m_detail)
        m_detail->Show(!dashboard);
    if (m_dashboard)
        m_dashboard->Show(dashboard);
    if (m_list_panel)
        m_list_panel->Show(dashboard);
    if (m_list_sash)
        m_list_sash->Show(dashboard);
    if (m_back)
        m_back->Show(!dashboard);
    if (m_edit)
        m_edit->Show(dashboard);
    Layout();
}

void DevicesPanel::show_selected_device()
{
    if (!m_detail)
        return;
    m_detail->GetSizer()->Clear(true);

    const DeviceEntry* device = nullptr;
    for (const DeviceEntry& entry : m_devices)
        if (entry.id == m_selected_id)
            device = &entry;

    if (!device) {
        auto* empty = new wxStaticText(m_detail, wxID_ANY,
            m_devices.empty()
                ? _L("No devices yet. Open the configuration wizard and add a printer. Leave the address empty if it has no network connection.")
                : _L("Select a printer to see what it is printing, its temperatures, and its cameras."));
        fit_card_text(empty);
        m_detail->GetSizer()->Add(empty, 0, wxEXPAND | wxALL, 8);
    } else {
        auto* columns = new wxBoxSizer(wxHORIZONTAL);
        fill_printer_board(m_detail, columns, *device);
        m_detail->GetSizer()->Add(columns, 1, wxEXPAND | wxALL, 8);
    }
    fit_cards_inside(m_detail, 720);
    m_detail->Layout();
    m_detail->FitInside();
}

bool DevicesPanel::stack_path_ok(const std::string& path)
{
    if (path.empty())
        return true;
    int depth = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        const char digit = path[i];
        if (digit != '0' && digit != '1')
            return false;
        ++depth;
        if (i + 1 == path.size())
            break;
        if (path[++i] != '.')
            return false;
        if (i + 1 == path.size())
            return false;
    }
    return depth > 0 && depth <= 6;
}

int DevicesPanel::stack_depth(const std::string& path)
{
    if (path.empty())
        return 0;
    int depth = 1;
    for (char c : path)
        if (c == '.')
            ++depth;
    return depth;
}

int DevicesPanel::nested_span(const DashItem& item)
{
    if (!item.is_stack())
        return 0;
    int nested = 0;
    for (const DashItem& child : item.stack)
        nested = std::max(nested, nested_span(child));
    return 1 + nested;
}

bool DevicesPanel::path_contains(const std::string& outer, const std::string& inner)
{
    if (outer == inner)
        return true;
    if (outer.empty())
        return true;
    return inner.size() > outer.size()
        && inner.compare(0, outer.size(), outer) == 0
        && inner[outer.size()] == '.';
}

DevicesPanel::DashItem DevicesPanel::node_from_cell(const DashCell& cell) const
{
    DashItem item;
    item.device_id = cell.device_id;
    item.module_key = cell.module_key;
    item.stack_split = cell.stack_split;
    if (cell.is_stack()) {
        item.stack.push_back(cell.stack[0]);
        item.stack.push_back(cell.stack[1]);
    }
    return item;
}

void DevicesPanel::write_node_to_cell(DashCell& cell, DashItem node)
{
    cell.device_id = std::move(node.device_id);
    cell.module_key = std::move(node.module_key);
    cell.stack_split = node.stack_split;
    cell.stack[0] = {};
    cell.stack[1] = {};
    if (!cell.is_stack())
        return;
    if (cell.stack_split < 20 || cell.stack_split > 80)
        cell.stack_split = 50;
    if (node.stack.size() > 0)
        cell.stack[0] = std::move(node.stack[0]);
    if (node.stack.size() > 1)
        cell.stack[1] = std::move(node.stack[1]);
}

DevicesPanel::DashItem* DevicesPanel::node_at(DashCell& cell, const std::string& path)
{
    if (path.empty())
        return nullptr;
    DashItem* item = nullptr;
    size_t cursor = 0;
    while (cursor < path.size()) {
        const int index = path[cursor] - '0';
        if (index < 0 || index > 1)
            return nullptr;
        if (item == nullptr) {
            if (!cell.is_stack())
                return nullptr;
            item = &cell.stack[index];
        } else {
            if (!item->is_stack() || index >= int(item->stack.size()))
                return nullptr;
            item = &item->stack[index];
        }
        ++cursor;
        if (cursor == path.size())
            break;
        if (path[cursor] != '.')
            return nullptr;
        ++cursor;
        if (cursor >= path.size())
            return nullptr;
    }
    return item;
}

void DevicesPanel::park_node(DashItem displaced)
{
    if (!displaced.filled())
        return;
    for (DashRow& existing : m_rows)
        for (DashCell& cell : existing.cells)
            if (!cell.filled()) {
                write_node_to_cell(cell, std::move(displaced));
                return;
            }
    DashRow added;
    write_node_to_cell(added.cells[0], std::move(displaced));
    m_rows.push_back(std::move(added));
}

void DevicesPanel::clear_matching_module(DashItem& item, const std::string& device_id, const std::string& module_key)
{
    if (item.is_stack()) {
        for (DashItem& child : item.stack)
            clear_matching_module(child, device_id, module_key);
        return;
    }
    if (item.device_id == device_id && item.module_key == module_key)
        item = {};
}

void DevicesPanel::forget_recent(const DashItem& item)
{
    if (item.module_key == k_recent_module && wxGetApp().app_config)
        wxGetApp().app_config->set("dashboard_recent_hidden", "1");
    for (const DashItem& child : item.stack)
        forget_recent(child);
}

void DevicesPanel::queue_dashboard_drop(const std::string& device_id, const std::string& module_key, int row, int column, const std::string& stack_path)
{
    m_pending_device = device_id;
    m_pending_module = module_key;
    m_move_row = -1;
    m_move_column = -1;
    m_move_stack.clear();
    m_pending_row = row;
    m_pending_column = column;
    m_pending_stack = stack_path;
    m_drop_pending = true;
}

void DevicesPanel::queue_dashboard_move(int from_row, int from_column, const std::string& from_path, int to_row, int to_column, const std::string& to_path)
{
    m_pending_device.clear();
    m_pending_module.clear();
    m_move_row = from_row;
    m_move_column = from_column;
    m_move_stack = from_path;
    m_pending_row = to_row;
    m_pending_column = to_column;
    m_pending_stack = to_path;
    m_drop_pending = true;
}

void DevicesPanel::finish_module_drag()
{
    if (!m_drop_pending)
        return;
    const bool moving = m_move_row >= 0;
    const int from_row = m_move_row;
    const int from_column = m_move_column;
    const std::string from_path = m_move_stack;
    const int row = m_pending_row;
    const int column = m_pending_column;
    const std::string path = m_pending_stack;
    const std::string device_id = m_pending_device;
    const std::string module_key = m_pending_module;
    m_drop_pending = false;
    m_move_row = -1;
    m_move_column = -1;
    m_move_stack.clear();
    m_pending_row = -1;
    m_pending_column = -1;
    m_pending_stack.clear();
    m_pending_device.clear();
    m_pending_module.clear();
    CallAfter([this, moving, from_row, from_column, from_path, row, column, path, device_id, module_key]() {
        if (!m_dashboard_edit)
            return;
        if (!stack_path_ok(from_path) || !stack_path_ok(path))
            return;
        if (moving) {
            if (from_row < 0 || from_column < 0 || from_column > 1 || row < 0 || column < 0 || column > 1)
                return;
            if (from_row >= int(m_rows.size()) || row >= int(m_rows.size()))
                return;
            if (from_row == row && from_column == column && from_path == path)
                return;
            DashCell& source = m_rows[size_t(from_row)].cells[from_column];
            DashCell& dest = m_rows[size_t(row)].cells[column];
            if (from_row == row && from_column == column && (path_contains(from_path, path) || path_contains(path, from_path)))
                return;
            if (!from_path.empty() && node_at(source, from_path) == nullptr)
                return;
            if (!path.empty() && node_at(dest, path) == nullptr)
                return;
            DashItem from_value = from_path.empty() ? node_from_cell(source) : *node_at(source, from_path);
            DashItem to_value = path.empty() ? node_from_cell(dest) : *node_at(dest, path);
            if (stack_depth(path) + nested_span(from_value) > 6)
                return;
            if (stack_depth(from_path) + nested_span(to_value) > 6)
                return;
            if (from_path.empty())
                write_node_to_cell(source, to_value);
            else
                *node_at(source, from_path) = to_value;
            if (path.empty())
                write_node_to_cell(dest, from_value);
            else
                *node_at(dest, path) = from_value;
            save_dashboard();
            rebuild_dashboard();
            return;
        }
        add_dashboard_tile(device_id, module_key, row, column, path);
    });
}

void DevicesPanel::add_dashboard_tile(const std::string& device_id, const std::string& module_key, int row, int column, const std::string& stack_path)
{
    if (!m_dashboard_edit && row >= 0)
        return;
    if (!stack_path_ok(stack_path))
        return;
    const bool container = device_id == k_local_device && module_key == k_stack_module;
    const bool recent = device_id == k_local_device && module_key == k_recent_module;
    if (stack_depth(stack_path) + (container ? 1 : 0) > 6)
        return;
    if (!container && !recent && (device_id.empty() || DeviceModuleRegistry::instance().find(module_key) == nullptr))
        return;
    bool known = container || recent;
    for (const DeviceEntry& device : m_devices)
        if (device.id == device_id)
            known = true;
    if (!known)
        return;
    if (recent && wxGetApp().app_config)
        wxGetApp().app_config->set("dashboard_recent_hidden", "0");

    if (!container) {
        for (DashRow& existing : m_rows)
            for (DashCell& cell : existing.cells) {
                if (!cell.is_stack() && cell.device_id == device_id && cell.module_key == module_key)
                    cell = {};
                else if (cell.is_stack()) {
                    clear_matching_module(cell.stack[0], device_id, module_key);
                    clear_matching_module(cell.stack[1], device_id, module_key);
                }
            }
    }
    DashItem incoming;
    incoming.device_id = device_id;
    incoming.module_key = module_key;
    if (container) {
        incoming.stack_split = 50;
        incoming.stack.assign(2, {});
    }

    if (!stack_path.empty()) {
        if (row < 0 || column < 0 || column > 1 || row >= int(m_rows.size()))
            return;
        DashCell& dest = m_rows[size_t(row)].cells[column];
        DashItem* slot = node_at(dest, stack_path);
        if (slot == nullptr)
            return;
        DashItem displaced = *slot;
        *slot = incoming;
        if (displaced.filled() && !(displaced.device_id == device_id && displaced.module_key == module_key && !displaced.is_stack()))
            park_node(std::move(displaced));
        save_dashboard();
        rebuild_dashboard();
        if (m_view != 1) {
            m_view = 1;
            show_mode();
        }
        return;
    }

    if (row < 0 || column < 0 || column > 1) {
        bool placed = false;
        for (DashRow& existing : m_rows) {
            for (DashCell& cell : existing.cells) {
                if (!cell.filled()) {
                    write_node_to_cell(cell, incoming);
                    placed = true;
                    break;
                }
            }
            if (placed)
                break;
        }
        if (!placed) {
            DashRow added;
            write_node_to_cell(added.cells[0], incoming);
            m_rows.push_back(std::move(added));
        }
    } else {
        while (int(m_rows.size()) <= row)
            m_rows.push_back({});
        DashCell& dest = m_rows[size_t(row)].cells[column];
        DashItem displaced = node_from_cell(dest);
        write_node_to_cell(dest, incoming);
        if (displaced.filled() && !(displaced.device_id == device_id && displaced.module_key == module_key && displaced.stack.empty()))
            park_node(std::move(displaced));
    }
    save_dashboard();
    rebuild_dashboard();
    if (m_view != 1) {
        m_view = 1;
        show_mode();
    }
}

void DevicesPanel::add_dashboard_row()
{
    if (!m_dashboard_edit)
        return;
    m_rows.push_back({});
    save_dashboard();
    rebuild_dashboard();
}

void DevicesPanel::set_row_split(int row, int percent)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    if (percent < 20)
        percent = 20;
    if (percent > 80)
        percent = 80;
    m_rows[size_t(row)].split = percent;
}

void DevicesPanel::set_row_height(int row, int height)
{
    if (row < 0 || row >= int(m_rows.size()))
        return;
    if (height < 140)
        height = 140;
    if (height > 1600)
        height = 1600;
    m_rows[size_t(row)].height = height;
}

void DevicesPanel::set_stack_split(int row, int column, const std::string& path, int percent)
{
    if (row < 0 || column < 0 || column > 1 || row >= int(m_rows.size()) || !stack_path_ok(path))
        return;
    if (percent < 20)
        percent = 20;
    if (percent > 80)
        percent = 80;
    DashCell& cell = m_rows[size_t(row)].cells[column];
    if (path.empty()) {
        if (!cell.is_stack())
            return;
        cell.stack_split = percent;
        return;
    }
    DashItem* item = node_at(cell, path);
    if (item == nullptr || !item->is_stack())
        return;
    item->stack_split = percent;
}

void DevicesPanel::clear_dashboard_cell(int row, int column, const std::string& stack_path)
{
    if (row < 0 || column < 0 || column > 1 || row >= int(m_rows.size()) || !stack_path_ok(stack_path))
        return;
    DashCell& cell = m_rows[size_t(row)].cells[column];
    if (!stack_path.empty()) {
        DashItem* item = node_at(cell, stack_path);
        if (item == nullptr)
            return;
        forget_recent(*item);
        *item = {};
        save_dashboard();
        rebuild_dashboard();
        return;
    }
    forget_recent(node_from_cell(cell));
    cell = {};
    save_dashboard();
    rebuild_dashboard();
}

void DevicesPanel::remove_dashboard_row(int row)
{
    if (!m_dashboard_edit || row < 0 || row >= int(m_rows.size()))
        return;
    const DashRow& gone = m_rows[size_t(row)];
    for (const DashCell& cell : gone.cells)
        forget_recent(node_from_cell(cell));
    m_rows.erase(m_rows.begin() + row);
    save_dashboard();
    rebuild_dashboard();
}

void DevicesPanel::set_dashboard_editing(bool editing)
{
    m_dashboard_edit = editing;
    if (!editing) {
        m_rows.erase(std::remove_if(m_rows.begin(), m_rows.end(), [](const DashRow& row) {
            return !row.cells[0].filled() && !row.cells[1].filled();
        }), m_rows.end());
        save_dashboard();
    } else if (m_rows.empty()) {
        m_rows.push_back({});
    }
    if (m_edit) {
        m_edit->SetLabel(editing ? _L("Done") : _L("Edit dashboard"));
        m_edit->SetToolTip(editing
            ? _L("Leave dashboard editing.")
            : _L("Show the modules you can add, and let you rearrange the ones already on the dashboard."));
    }
    if (m_dash_hint)
        m_dash_hint->Show(editing);
    if (m_palette)
        m_palette->Show(editing);
    rebuild_dashboard();
    if (m_dashboard)
        m_dashboard->Layout();
    Layout();
}


namespace {

// A positive minimum in both axes would also freeze the height. Only the width
// is capped, so a label or a picture can no longer force the dashboard wider
// than the window.
bool release_min_width(wxWindow* window, int floor)
{
    if (!window)
        return false;
    bool changed = false;
    for (wxWindowList::const_iterator it = window->GetChildren().begin(); it != window->GetChildren().end(); ++it) {
        wxWindow* child = *it;
        if (!child)
            continue;
        const wxSize min = child->GetMinSize();
        const int width = (min.x <= 0 || min.x > floor) ? floor : min.x;
        if (min.x != width) {
            child->SetMinSize(wxSize(width, min.y));
            changed = true;
        }
        changed = release_min_width(child, floor) || changed;
    }
    return changed;
}

} // namespace

void DevicesPanel::scale_dashboard_modules()
{
    if (m_scaling_dashboard || m_rebuilding_dashboard || !m_canvas || m_dash_rows.empty())
        return;
    if (wxWindow::GetCapture())
        return;
    const int client_w = m_canvas->GetClientSize().GetWidth();
    const int client_h = m_canvas->GetClientSize().GetHeight();
    if (client_w < 80 || client_h < 80)
        return;

    const int count = int(m_dash_rows.size());
    int weight_sum = 0;
    std::vector<int> weights(count, 280);
    for (int i = 0; i < count; ++i) {
        const int index = m_dash_rows[size_t(i)].first;
        if (index >= 0 && index < int(m_rows.size()) && m_rows[size_t(index)].height >= 140)
            weights[i] = m_rows[size_t(index)].height;
        weight_sum += weights[i];
    }
    int chrome = 8 * count + 16;
    if (m_dashboard_edit)
        chrome += 56;
    int available = client_h - chrome;
    const int floor_h = 140;
    if (available < count * floor_h)
        available = count * floor_h;

    m_scaling_dashboard = true;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        wxWindow* panel = m_dash_rows[size_t(i)].second;
        if (!panel)
            continue;
        const int row_h = std::max(floor_h, available * weights[i] / std::max(1, weight_sum));
        const wxSize min = panel->GetMinSize();
        if (min.GetWidth() != 48 || min.GetHeight() != row_h) {
            panel->SetMinSize(wxSize(48, row_h));
            changed = true;
        }
        if (limit_cameras(panel, std::max(120, row_h - 72)))
            changed = true;
        if (release_min_width(panel, 48))
            changed = true;
    }
    if (changed && m_canvas_sizer) {
        const int content_h = std::max(client_h, m_canvas_sizer->CalcMin().GetHeight());
        m_canvas->SetVirtualSize(client_w, content_h);
        m_canvas->Layout();
    }
    m_scaling_dashboard = false;
}

void DevicesPanel::rebuild_dashboard()
{
    if (!m_palette || !m_canvas_sizer)
        return;
    m_rebuilding_dashboard = true;

    m_palette->GetSizer()->Clear(true);
    {
        auto* section = new wxStaticText(m_palette, wxID_ANY, _L("Dashboard"));
        section->SetFont(wxGetApp().bold_font());
        m_palette->GetSizer()->Add(section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
        auto* chip = new wxButton(m_palette, wxID_ANY, _L("Recent projects"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        wxGetApp().UpdateDarkUI(chip, true);
        chip->Bind(wxEVT_LEFT_DOWN, [this, chip](wxMouseEvent&) {
            start_module_drag(this, chip, from_u8(std::string(k_local_device) + "\n" + k_recent_module));
        });
        m_palette->GetSizer()->Add(chip, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
        auto* stack_chip = new wxButton(m_palette, wxID_ANY, _L("Vertical container"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        wxGetApp().UpdateDarkUI(stack_chip, true);
        stack_chip->SetToolTip(_L("Stacks two things, one above the other. Either thing can be a module or another vertical container."));
        stack_chip->Bind(wxEVT_LEFT_DOWN, [this, stack_chip](wxMouseEvent&) {
            start_module_drag(this, stack_chip, from_u8(std::string(k_local_device) + "\n" + k_stack_module));
        });
        m_palette->GetSizer()->Add(stack_chip, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
    }
    if (m_devices.empty()) {
        m_palette->GetSizer()->Add(new wxStaticText(m_palette, wxID_ANY, _L("Bind a printer to place its modules here.")),
            0, wxEXPAND | wxALL, 6);
    }
    for (const DeviceEntry& device : m_devices) {
        auto* name = new wxStaticText(m_palette, wxID_ANY, from_u8(device.name));
        name->SetFont(wxGetApp().bold_font());
        m_palette->GetSizer()->Add(name, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
        for (const DeviceModuleDesc& module : DeviceModuleRegistry::instance().modules()) {
            if (module.key == k_recent_module)
                continue;
            auto* chip = new wxButton(m_palette, wxID_ANY, module.title, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            wxGetApp().UpdateDarkUI(chip, true);
            const std::string device_id = device.id;
            const std::string module_key = module.key;
            chip->Bind(wxEVT_LEFT_DOWN, [this, chip, device_id, module_key](wxMouseEvent&) {
                start_module_drag(this, chip, from_u8(device_id + "\n" + module_key));
            });
            m_palette->GetSizer()->Add(chip, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
        }
    }
    m_palette->Layout();
    m_palette->FitInside();

    m_dash_rows.clear();
    m_canvas_sizer->Clear(true);
    if (m_rows.empty()) {
        auto* hint = new wxStaticText(m_canvas, wxID_ANY, m_dashboard_edit
            ? _L("Drop modules here.")
            : _L("No modules on the dashboard yet. Choose Edit dashboard to add some."));
        hint->Wrap(480);
        m_canvas_sizer->Add(hint, 0, wxEXPAND | wxALL, 12);
    }

    for (size_t r = 0; r < m_rows.size(); ++r) {
        const bool left_filled = m_rows[r].cells[0].filled();
        const bool right_filled = m_rows[r].cells[1].filled();
        if (!m_dashboard_edit && !left_filled && !right_filled)
            continue;

        auto* row_panel = new wxPanel(m_canvas, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
        wxGetApp().UpdateDarkUI(row_panel);
        auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
        const int row_index = int(r);
        int split = m_rows[r].split;
        if (split < 20)
            split = 20;
        if (split > 80)
            split = 80;

        auto build_module_card = [&](wxWindow* parent, const std::string& device_id, const std::string& module_key, int column, const std::string& stack_path) -> wxPanel* {
            const DeviceModuleDesc* module = DeviceModuleRegistry::instance().find(module_key);
            const DeviceEntry* device = nullptr;
            for (const DeviceEntry& entry : m_devices)
                if (entry.id == device_id)
                    device = &entry;
            DeviceEntry local_device;
            if (!device && device_id == k_local_device) {
                local_device.id = k_local_device;
                local_device.name = "Slice it All";
                device = &local_device;
            }
            auto* card = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
            wxGetApp().UpdateDarkUI(card);
            auto* card_sizer = new wxBoxSizer(wxVERTICAL);
            const int drag_row = row_index;
            auto add_remove = [&](wxSizer* header) {
                if (!m_dashboard_edit)
                    return;
                auto* close = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(22, 22), wxBORDER_NONE);
                close->SetBackgroundStyle(wxBG_STYLE_PAINT);
                close->SetMinSize(wxSize(22, 22));
                close->SetCursor(wxCURSOR_HAND);
                close->SetToolTip(_L("Remove"));
                close->Bind(wxEVT_PAINT, [close](wxPaintEvent&) {
                    wxAutoBufferedPaintDC dc(close);
                    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
                    if (!gc)
                        return;
                    const wxSize size = close->GetClientSize();
                    const wxColour page = close->GetParent() ? close->GetParent()->GetBackgroundColour() : wxColour(32, 32, 38);
                    gc->SetPen(*wxTRANSPARENT_PEN);
                    gc->SetBrush(wxBrush(page));
                    gc->DrawRectangle(0, 0, size.x, size.y);
                    gc->SetBrush(wxBrush(wxColour(226, 59, 59)));
                    gc->DrawEllipse(1, 1, std::max(0, size.x - 2), std::max(0, size.y - 2));
                    gc->SetPen(wxPen(*wxWHITE, 2));
                    gc->StrokeLine(7, 7, size.x - 7, size.y - 7);
                    gc->StrokeLine(size.x - 7, 7, 7, size.y - 7);
                });
                close->Bind(wxEVT_LEFT_DOWN, [](wxMouseEvent&) {});
                close->Bind(wxEVT_LEFT_UP, [this, drag_row, column, stack_path](wxMouseEvent&) {
                    clear_dashboard_cell(drag_row, column, stack_path);
                });
                header->Add(close, 0, wxLEFT, 6);
            };
            if (!module || !module->make || !device) {
                auto* header = new wxBoxSizer(wxHORIZONTAL);
                auto* title = new wxStaticText(card, wxID_ANY, _L("This module is no longer available."),
                    wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
                title->SetForegroundColour(wxColour(180, 180, 188));
                title->SetMinSize(wxSize(40, -1));
                header->Add(title, 1, wxALIGN_CENTER_VERTICAL);
                add_remove(header);
                card_sizer->Add(header, 0, wxEXPAND | wxALL, 8);
            } else {
                auto* header = new wxBoxSizer(wxHORIZONTAL);
                const wxString heading = device_id == k_local_device
                    ? module->title
                    : from_u8(device->name) + " - " + module->title;
                auto* title = new wxStaticText(card, wxID_ANY, heading,
                    wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
                title->SetFont(wxGetApp().bold_font());
                title->SetMinSize(wxSize(40, -1));
                header->Add(title, 1, wxALIGN_CENTER_VERTICAL);
                if (m_dashboard_edit) {
                    title->SetToolTip(_L("Drag to move this module."));
                    title->SetCursor(wxCURSOR_HAND);
                    wxString payload = wxString::Format("move\n%d\t%d", drag_row, column);
                    if (!stack_path.empty()) {
                        payload += "\t";
                        payload += from_u8(stack_path);
                    }
                    title->Bind(wxEVT_LEFT_DOWN, [this, card, payload](wxMouseEvent&) {
                        start_module_drag(this, card, payload);
                    });
                }
                add_remove(header);
                card_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
                wxPanel* body = nullptr;
                try {
                    body = module->make(card, *device);
                } catch (const std::exception&) {
                    body = nullptr;
                }
                if (!body) {
                    body = make_placeholder(card, module->title,
                        _L("This module could not be opened. Placing it does not contact the printer."));
                }
                card_sizer->Add(body, 1, wxEXPAND);
            }
            card->SetSizer(card_sizer);
            // An explicit width keeps a long label or a row of spools from
            // becoming the module's minimum and pushing the next module off screen.
            card->SetMinSize(wxSize(64, -1));
            return card;
        };

        auto make_slot = [&](int column) -> DashSlot* {
            const DashCell cell = m_rows[size_t(row_index)].cells[column];
            const bool filled = cell.filled();
            auto* slot = new DashSlot(row_panel, m_dashboard_edit, filled);
            auto* slot_sizer = new wxBoxSizer(wxVERTICAL);
            std::function<wxWindow*(wxWindow*, const DashItem&, const std::string&)> build_node;
            build_node = [&](wxWindow* parent, const DashItem& item, const std::string& path) -> wxWindow* {
                if (!item.is_stack()) {
                    wxPanel* card = build_module_card(parent, item.device_id, item.module_key, column, path);
                    if (!path.empty())
                        card->SetDropTarget(new DashboardDropTarget(this, row_index, column, path, [](bool) {}));
                    return card;
                }

                auto* box = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
                wxGetApp().UpdateDarkUI(box);
                auto* box_sizer = new wxBoxSizer(wxVERTICAL);
                if (m_dashboard_edit) {
                    auto* header = new wxBoxSizer(wxHORIZONTAL);
                    auto* title = new wxStaticText(box, wxID_ANY, _L("Vertical container"));
                    title->SetFont(wxGetApp().bold_font());
                    title->SetMinSize(wxSize(40, -1));
                    title->SetToolTip(_L("Drag to move this container."));
                    title->SetCursor(wxCURSOR_HAND);
                    const int drag_row = row_index;
                    const std::string drag_path = path;
                    title->Bind(wxEVT_LEFT_DOWN, [this, box, drag_row, column, drag_path](wxMouseEvent&) {
                        wxString payload = wxString::Format("move\n%d\t%d", drag_row, column);
                        if (!drag_path.empty()) {
                            payload += "\t";
                            payload += from_u8(drag_path);
                        }
                        start_module_drag(this, box, payload);
                    });
                    header->Add(title, 1, wxALIGN_CENTER_VERTICAL);
                    auto* close = new wxButton(box, wxID_ANY, _L("Remove container"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                    wxGetApp().UpdateDarkUI(close, true);
                    close->SetToolTip(_L("Delete this container and everything inside it."));
                    close->Bind(wxEVT_BUTTON, [this, drag_row, column, drag_path](wxCommandEvent&) {
                        clear_dashboard_cell(drag_row, column, drag_path);
                    });
                    header->Add(close, 0, wxLEFT, 6);
                    box_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
                }
                auto child_at = [&](int index) -> DashItem {
                    return index < int(item.stack.size()) ? item.stack[size_t(index)] : DashItem{};
                };
                auto child_path = [&](int index) {
                    const std::string index_text = index == 0 ? "0" : "1";
                    return path.empty() ? index_text : path + "." + index_text;
                };
                auto add_half = [&](int index) -> wxWindow* {
                    const DashItem child = child_at(index);
                    const std::string next = child_path(index);
                    auto* half = new wxPanel(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxCLIP_CHILDREN);
                    wxGetApp().UpdateDarkUI(half);
                    auto* half_sizer = new wxBoxSizer(wxVERTICAL);
                    if (child.filled()) {
                        half_sizer->Add(build_node(half, child, next), 1, wxEXPAND);
                        half->SetDropTarget(new DashboardDropTarget(this, row_index, column, next, [](bool) {}));
                    } else if (m_dashboard_edit) {
                        auto* empty = new DashSlot(half, true, false, true);
                        empty->SetDropTarget(new DashboardDropTarget(this, row_index, column, next, [empty](bool hot) {
                            empty->set_hot(hot);
                        }));
                        half_sizer->Add(empty, 1, wxEXPAND);
                    }
                    half->SetSizer(half_sizer);
                    return half;
                };
                wxWindow* top = add_half(0);
                wxWindow* bottom = add_half(1);
                const bool show_top = m_dashboard_edit || child_at(0).filled();
                const bool show_bottom = m_dashboard_edit || child_at(1).filled();
                int stack_split = item.stack_split;
                if (stack_split < 20 || stack_split > 80)
                    stack_split = 50;
                if (show_top && show_bottom) {
                    box_sizer->Add(top, stack_split, wxEXPAND);
                    if (m_dashboard_edit) {
                        auto* sash = new StackSplitSash(box,
                            [this, row_index, column, path, box_sizer, top, bottom, box](int percent) {
                                set_stack_split(row_index, column, path, percent);
                                if (wxSizerItem* sash_item = box_sizer->GetItem(top))
                                    sash_item->SetProportion(percent);
                                if (wxSizerItem* sash_item = box_sizer->GetItem(bottom))
                                    sash_item->SetProportion(100 - percent);
                                box->Layout();
                            },
                            [this]() { save_dashboard(); });
                        box_sizer->Add(sash, 0, wxEXPAND | wxTOP | wxBOTTOM, 2);
                    }
                    box_sizer->Add(bottom, 100 - stack_split, wxEXPAND);
                } else if (show_top) {
                    box_sizer->Add(top, 1, wxEXPAND);
                    bottom->Hide();
                } else if (show_bottom) {
                    top->Hide();
                    box_sizer->Add(bottom, 1, wxEXPAND);
                } else {
                    auto* empty = new wxStaticText(box, wxID_ANY, _L("This column is empty."));
                    empty->SetForegroundColour(wxColour(180, 180, 188));
                    box_sizer->Add(empty, 0, wxALL, 12);
                    top->Hide();
                    bottom->Hide();
                }
                box->SetSizer(box_sizer);
                box->SetMinSize(wxSize(64, -1));
                if (!path.empty())
                    box->SetDropTarget(new DashboardDropTarget(this, row_index, column, path, [](bool) {}));
                return box;
            };
            if (cell.is_stack()) {
                slot_sizer->Add(build_node(slot, node_from_cell(cell), {}), 1, wxEXPAND);
                slot->SetSizer(slot_sizer);
                return slot;
            }
            slot->SetDropTarget(new DashboardDropTarget(this, row_index, column, {}, [slot](bool hot) {
                slot->set_hot(hot);
            }));
            if (!filled) {
                slot->SetSizer(slot_sizer);
                return slot;
            }

            slot_sizer->Add(build_module_card(slot, cell.device_id, cell.module_key, column, {}), 1, wxEXPAND | wxALL, 6);
            slot->SetSizer(slot_sizer);
            return slot;
        };

        DashSlot* left = make_slot(0);
        DashSlot* right = make_slot(1);
        const bool show_left = m_dashboard_edit || left_filled;
        const bool show_right = m_dashboard_edit || right_filled;
        if (show_left && show_right) {
            row_sizer->Add(left, split, wxEXPAND | wxRIGHT, 4);
            if (m_dashboard_edit) {
                auto* sash = new RowSplitSash(row_panel,
                    [this, row_index, row_sizer, left, right, row_panel](int percent) {
                        set_row_split(row_index, percent);
                        if (wxSizerItem* item = row_sizer->GetItem(left))
                            item->SetProportion(percent);
                        if (wxSizerItem* item = row_sizer->GetItem(right))
                            item->SetProportion(100 - percent);
                        row_panel->Layout();
                    },
                    [this]() { save_dashboard(); });
                row_sizer->Add(sash, 0, wxEXPAND | wxLEFT | wxRIGHT, 2);
            }
            row_sizer->Add(right, 100 - split, wxEXPAND | wxLEFT, 4);
        } else if (show_left) {
            row_sizer->Add(left, 1, wxEXPAND);
            right->Hide();
            row_sizer->Add(right, 0);
        } else {
            left->Hide();
            row_sizer->Add(left, 0);
            row_sizer->Add(right, 1, wxEXPAND);
        }
        auto* column = new wxBoxSizer(wxVERTICAL);
        column->Add(row_sizer, 1, wxEXPAND);
        if (m_dashboard_edit) {
            auto* row_tools = new wxBoxSizer(wxHORIZONTAL);
            row_tools->AddStretchSpacer(1);
            auto* remove_row = new wxButton(row_panel, wxID_ANY, _L("Remove row"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            wxGetApp().UpdateDarkUI(remove_row, true);
            remove_row->SetToolTip(_L("Delete this row, including both sides."));
            remove_row->Bind(wxEVT_BUTTON, [this, row_index](wxCommandEvent&) { remove_dashboard_row(row_index); });
            row_tools->Add(remove_row, 0, wxALIGN_CENTER_VERTICAL);
            column->Add(row_tools, 0, wxEXPAND | wxTOP, 4);
            auto* height_sash = new RowHeightSash(row_panel, row_panel, m_canvas,
                [this, row_index](int height) { set_row_height(row_index, height); },
                [this]() { save_dashboard(); });
            column->Add(height_sash, 0, wxEXPAND | wxTOP, 4);
        }
        row_panel->SetSizer(column);
        const int saved_height = m_rows[size_t(row_index)].height;
        if (saved_height >= 140)
            row_panel->SetMinSize(wxSize(-1, saved_height));
        limit_cameras(row_panel, saved_height >= 140 ? std::max(120, saved_height - 72) : 400);
        m_dash_rows.emplace_back(row_index, row_panel);
        m_canvas_sizer->Add(row_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
    }

    if (m_dashboard_edit) {
        auto* add_row = new wxButton(m_canvas, wxID_ANY, _L("Add row"));
        wxGetApp().UpdateDarkUI(add_row, true);
        add_row->SetToolTip(_L("Add another row. Each row has two sides. A vertical container stacks two modules, or other vertical containers, in one side."));
        add_row->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_dashboard_row(); });
        m_canvas_sizer->Add(add_row, 0, wxALL, 12);
    }

    m_rebuilding_dashboard = false;
    m_canvas->Layout();
    m_canvas->FitInside();
    scale_dashboard_modules();
}

void DevicesPanel::load_dashboard()
{
    m_rows.clear();
    if (!wxGetApp().app_config)
        return;
    const std::string stored = wxGetApp().app_config->get("devices_dashboard");
    auto parse_cell = [this](const std::string& text) {
        DashCell cell;
        std::vector<std::string> parts;
        size_t cursor = 0;
        while (cursor <= text.size()) {
            const size_t tab = text.find('\t', cursor);
            parts.push_back(text.substr(cursor, tab == std::string::npos ? std::string::npos : tab - cursor));
            if (tab == std::string::npos)
                break;
            cursor = tab + 1;
        }
        size_t index = 0;
        std::function<DashItem()> parse = [&]() {
            DashItem item;
            if (index + 1 >= parts.size())
                return item;
            item.device_id = parts[index++];
            item.module_key = parts[index++];
            if (!item.is_stack())
                return item;
            if (index < parts.size()) {
                try {
                    item.stack_split = std::stoi(parts[index]);
                } catch (const std::exception&) {
                    item.stack_split = 50;
                }
                ++index;
            }
            if (item.stack_split < 20 || item.stack_split > 80)
                item.stack_split = 50;
            item.stack.resize(2);
            item.stack[0] = parse();
            item.stack[1] = parse();
            return item;
        };
        write_node_to_cell(cell, parse());
        return cell;
    };
    auto add_legacy = [this](std::vector<DashCell>& cells) {
        for (size_t i = 0; i < cells.size(); i += 2) {
            DashRow row;
            row.cells[0] = cells[i];
            if (i + 1 < cells.size())
                row.cells[1] = cells[i + 1];
            m_rows.push_back(std::move(row));
        }
    };

    const size_t first_tab = stored.find('\t');
    const size_t first_colon = stored.find(':');
    const bool modern = first_colon != std::string::npos && (first_tab == std::string::npos || first_colon < first_tab);
    if (!modern) {
        std::vector<DashCell> cells;
        size_t cursor = 0;
        while (cursor < stored.size()) {
            const size_t end = stored.find_first_of("\n;", cursor);
            std::string line = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
            cursor = end == std::string::npos ? stored.size() : end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            DashCell cell = parse_cell(line);
            if (cell.filled())
                cells.push_back(std::move(cell));
        }
        add_legacy(cells);
    } else {

    size_t cursor = 0;
    while (cursor < stored.size()) {
        const size_t end = stored.find(';', cursor);
        std::string record = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? stored.size() : end + 1;
        if (!record.empty() && record.back() == '\r')
            record.pop_back();
        const size_t colon = record.find(':');
        if (colon == std::string::npos)
            continue;
        DashRow row;
        const std::string head = record.substr(0, colon);
        const size_t comma = head.find(',');
        try {
            row.split = std::stoi(comma == std::string::npos ? head : head.substr(0, comma));
        } catch (const std::exception&) {
            row.split = 50;
        }
        if (comma != std::string::npos) {
            try {
                row.height = std::stoi(head.substr(comma + 1));
            } catch (const std::exception&) {
                row.height = 0;
            }
        }
        if (row.split < 20)
            row.split = 20;
        if (row.split > 80)
            row.split = 80;
        if (row.height < 140)
            row.height = 0;
        if (row.height > 1600)
            row.height = 1600;
        const std::string body = record.substr(colon + 1);
        const size_t pipe = body.find('|');
        row.cells[0] = parse_cell(pipe == std::string::npos ? body : body.substr(0, pipe));
        if (pipe != std::string::npos)
            row.cells[1] = parse_cell(body.substr(pipe + 1));
        if (!row.cells[0].filled() && !row.cells[1].filled())
            continue;
        m_rows.push_back(std::move(row));
    }
    }
    std::function<bool(const DashItem&)> has_recent = [&](const DashItem& item) {
        if (item.module_key == k_recent_module)
            return true;
        for (const DashItem& child : item.stack)
            if (has_recent(child))
                return true;
        return false;
    };
    bool placed = false;
    for (const DashRow& row : m_rows)
        for (const DashCell& cell : row.cells)
            if (has_recent(node_from_cell(cell)))
                placed = true;
    if (!placed && wxGetApp().app_config && !wxGetApp().app_config->get_bool("dashboard_recent_hidden")) {
        DashRow row;
        row.cells[0].device_id = k_local_device;
        row.cells[0].module_key = k_recent_module;
        m_rows.insert(m_rows.begin(), std::move(row));
    }
}

void DevicesPanel::save_dashboard()
{
    if (!wxGetApp().app_config)
        return;
    std::string stored;
    std::function<void(const DashItem&)> append_item = [&](const DashItem& item) {
        stored += item.device_id;
        stored.push_back('\t');
        stored += item.module_key;
        if (!item.is_stack())
            return;
        stored.push_back('\t');
        int split = item.stack_split;
        if (split < 20 || split > 80)
            split = 50;
        stored += std::to_string(split);
        for (int index = 0; index < 2; ++index) {
            stored.push_back('\t');
            if (index < int(item.stack.size()))
                append_item(item.stack[size_t(index)]);
            else
                stored.push_back('\t');
        }
    };
    auto append_cell = [&](const DashCell& cell) {
        if (!cell.filled())
            return;
        append_item(node_from_cell(cell));
    };
    for (const DashRow& row : m_rows) {
        if (!stored.empty())
            stored.push_back(';');
        stored += std::to_string(row.split);
        if (row.height >= 140) {
            stored.push_back(',');
            stored += std::to_string(row.height);
        }
        stored.push_back(':');
        append_cell(row.cells[0]);
        stored.push_back('|');
        append_cell(row.cells[1]);
    }
    wxGetApp().app_config->set("devices_dashboard", stored);
    if (DashboardSite::instance().running())
        publish_dashboard_snapshot();
}


}}
