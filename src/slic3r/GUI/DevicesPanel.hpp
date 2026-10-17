///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GUI_DevicesPanel_hpp_
#define slic3r_GUI_DevicesPanel_hpp_

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxStaticText;
class wxScrolledWindow;
class wxBoxSizer;

namespace Slic3r {
namespace GUI {

// One bound printer. Addons (cameras, dryers, ...) can later attach extra modules
// through DeviceModuleRegistry without changing this record.
struct DeviceEntry
{
    std::string id;
    std::string name;
    std::string brand;
    std::string model;
    std::string host;
    std::string link;
    std::string status;
    std::string reported;
    std::string detail;
    // Resin printer. The device page shows a vat instead of filament spools and nozzle controls.
    bool        resin { false };
    bool        active { false };
    // No network address. The printer still belongs in the list, and a print can be tracked locally.
    bool        offline { false };
};

// A tile the dashboard and the device page can show.
// Register a new kind from an addon; the list and the dashboard pick it up.
struct DeviceModuleDesc
{
    std::string key;
    wxString    title;
    std::function<wxPanel*(wxWindow* parent, const DeviceEntry& device)> make;
};

class DeviceTabBar;

class DeviceModuleRegistry
{
public:
    static DeviceModuleRegistry& instance();

    void register_module(DeviceModuleDesc desc);
    const std::vector<DeviceModuleDesc>& modules() const { return m_modules; }
    const DeviceModuleDesc* find(const std::string& key) const;

private:
    DeviceModuleRegistry();
    std::vector<DeviceModuleDesc> m_modules;
};

// Bound printers, read from the physical printer collection.
std::vector<DeviceEntry> bound_devices();

// Five minutes added to a tracked offline print, covering the time to start it by hand.
inline int offline_print_pad_seconds() { return 5 * 60; }

// Name shown when an export can be tracked locally. Empty when this preset already has a network printer.
std::string offline_export_printer_name();
// estimate_seconds is the slicer estimate. The task runs for that long, plus offline_print_pad_seconds().
void begin_offline_print(const std::string& job_name, int estimate_seconds, int layers);
// Clear a tracked offline job. The printer is not contacted.
void stop_offline_print(const std::string& printer_id);
void refresh_offline_jobs(bool notify_ui);
std::string offline_dashboard_text(const std::string& printer_name);
bool offline_job_printing(const std::string& printer_name);
// Remember that a hostless printer was removed, so it is not added back on its own.
void dismiss_offline_model(const std::string& model_id);
void undismiss_offline_model(const std::string& model_id);

// Devices page. The dashboard is the main page. Opening a printer shows its cards.
class DevicesPanel : public wxPanel
{
public:
    explicit DevicesPanel(wxWindow* parent);
    ~DevicesPanel() override;
    void reload();
    // Add a hostless device for the selected printer once the main window is up.
    void adopt_selected_offline_printer();
    // Put a Print task module on the dashboard for a printer that is being tracked locally.
    void ensure_print_task_tile(const std::string& device_id);
    // Refresh every saved printer. Used when the Devices tab is opened.
    void on_tab_activated();
    void apply_dashboard_site();
    void add_dashboard_tile(const std::string& device_id, const std::string& module_key, int row = -1, int column = -1, const std::string& stack_path = {});
    // Remember a drop, then apply it only after the drag's mouse event has returned.
    // stack_path is empty for a side of the row. "0" and "1" are the top and bottom of a vertical container.
    // "0.1" is the bottom slot of the container in the top slot, and so on.
    void queue_dashboard_drop(const std::string& device_id, const std::string& module_key, int row, int column, const std::string& stack_path);
    void queue_dashboard_move(int from_row, int from_column, const std::string& from_path, int to_row, int to_column, const std::string& to_path);
    void finish_module_drag();
    // Printer list label follows the live job, so a running printer is not shown as free.
    void apply_live_status(const std::string& id);

private:
    void poll_jobs();
    void rebuild_lists();
    void refresh_selected();
    void refresh_device(const std::string& id);
    void show_selected_device();
    void show_mode();
    void rebuild_dashboard();
    void scale_dashboard_modules();
    void load_dashboard();
    void save_dashboard();
    void update_share_status();
    void set_dashboard_editing(bool editing);
    void add_dashboard_row();
    void set_row_split(int row, int percent);
    void set_row_height(int row, int height);
    void set_stack_split(int row, int column, const std::string& path, int percent);
    void clear_dashboard_cell(int row, int column, const std::string& stack_path = {});
    void remove_dashboard_row(int row);
    std::vector<DeviceEntry> visible_devices(bool active_only) const;

    struct DeviceCheck {
        int         generation { 0 };
        std::string status;
        std::string reported;
        std::string detail;
    };

    DeviceTabBar*      m_list_tabs { nullptr };
    wxButton*          m_reload { nullptr };
    wxButton*          m_edit { nullptr };
    wxStaticText*      m_dash_hint { nullptr };
    bool               m_dashboard_edit { false };
    bool               m_rebuilding_dashboard { false };
    bool               m_scaling_dashboard { false };
    // Row index and the panel that draws it. Used to grow and shrink rows with the window.
    std::vector<std::pair<int, wxWindow*>> m_dash_rows;
    int                m_checks_in_flight { 0 };
    std::shared_ptr<bool> m_alive;
    std::map<std::string, DeviceCheck> m_checks;
    int                m_check_generation { 0 };
    DeviceTabBar*      m_sort_tabs { nullptr };
    wxPanel*           m_sort_wrap { nullptr };
    wxScrolledWindow*  m_device_list { nullptr };
    wxBoxSizer*        m_device_rows { nullptr };
    wxPanel*           m_list_panel { nullptr };
    wxPanel*           m_list_sash { nullptr };
    wxButton*          m_back { nullptr };
    int                m_list_page { 0 };
    bool               m_sort_brand { false };
    // 1 is the dashboard. 0 is the open printer.
    int                m_view { 1 };
    wxCheckBox*        m_share { nullptr };
    wxStaticText*      m_share_status { nullptr };
    wxTimer            m_share_timer;
    wxTimer            m_job_timer;
    wxScrolledWindow*  m_detail { nullptr };
    wxPanel*           m_dashboard { nullptr };
    wxScrolledWindow*  m_palette { nullptr };
    wxScrolledWindow*  m_canvas { nullptr };
    wxBoxSizer*        m_canvas_sizer { nullptr };

    std::vector<DeviceEntry> m_devices;
    std::string              m_selected_id;
    std::string              m_pending_device;
    std::string              m_pending_module;
    bool                     m_drop_pending { false };
    int                      m_pending_row { -1 };
    int                      m_pending_column { -1 };
    std::string              m_pending_stack;
    int                      m_move_row { -1 };
    int                      m_move_column { -1 };
    std::string              m_move_stack;
    // A module, or a vertical container whose two slots can hold modules or more containers.
    struct DashItem {
        std::string device_id;
        std::string module_key;
        int stack_split { 50 };
        std::vector<DashItem> stack;
        bool filled() const { return !device_id.empty() && !module_key.empty(); }
        bool is_stack() const { return module_key == "vertical"; }
    };
    struct DashCell {
        std::string device_id;
        std::string module_key;
        // Percent of a vertical container used by the top module. 50 is an even split.
        int stack_split { 50 };
        DashItem stack[2];
        bool filled() const { return !device_id.empty() && !module_key.empty(); }
        bool is_stack() const { return module_key == "vertical"; }
    };
    struct DashRow {
        // Percent of the row used by the left side. 50 is an even split.
        int split { 50 };
        // Pixel height of the row. 0 lets the modules choose their own height.
        int height { 0 };
        DashCell cells[2];
    };
    DashItem node_from_cell(const DashCell& cell) const;
    void write_node_to_cell(DashCell& cell, DashItem node);
    DashItem* node_at(DashCell& cell, const std::string& path);
    void park_node(DashItem displaced);
    void clear_matching_module(DashItem& item, const std::string& device_id, const std::string& module_key);
    void forget_recent(const DashItem& item);
    static bool stack_path_ok(const std::string& path);
    static int stack_depth(const std::string& path);
    static int nested_span(const DashItem& item);
    static bool path_contains(const std::string& outer, const std::string& inner);
    std::vector<DashRow>     m_rows;
};

}}

#endif
