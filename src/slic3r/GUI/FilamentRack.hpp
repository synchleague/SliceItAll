///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GUI_FilamentRack_hpp_
#define slic3r_GUI_FilamentRack_hpp_

#include <string>
#include <vector>

namespace Slic3r {
struct AnycubicReadings;
}

class wxImage;
class wxWindow;

namespace Slic3r { namespace GUI {

// One position in a multi-color unit (AMS, ACE, MMU, or the same kind of rack)
// or, for a true multi-nozzle printer, one extruder.
struct RackSlot
{
    std::string color;
    std::string material;
};

// One physical group of spools. An Anycubic printer has an external rack plus ACE units.
struct MaterialBay
{
    std::string           key;
    std::string           title;
    std::vector<RackSlot> slots;
    bool                  dryer { false };
    bool                  drying { false };
    // Below zero means the drying temperature has not been set.
    int                   dry_celsius { -1 };
    int                   dry_hours { 0 };
    // Unix time when this drying run started. Zero means it has not been started.
    long long             dry_started { 0 };
    std::string           dry_material;
};

struct FilamentRack
{
    // Flat list used when a print is matched to slots. ACE1, then ACE2, then the external rack.
    std::vector<RackSlot>   slots;
    std::vector<MaterialBay> bays;
    // False when the printer really has more than one nozzle.
    bool                    multi_color_unit { true };
    bool                    anycubic_ace { false };
};

struct ProjectFilament
{
    int         tool { 0 };
    std::string color;
    std::string name;
};

// One spool that actually has filament loaded. Empty positions are left out.
// color is "#RRGGBB" when the spool has a color. material is a family such as "PLA".
struct LoadedSpool
{
    std::string material;
    std::string color;
};

bool rack_color_rgb(const std::string& color, int& r, int& g, int& b);
std::string filament_rack_key();
FilamentRack load_filament_rack(const std::string& printer_name);
void save_filament_rack(const std::string& printer_name, const FilamentRack& rack);

std::vector<ProjectFilament> current_project_filaments();

// Spools saved for this printer, with the latest ACE reply laid over them.
// ACE slots come first, then the external rack. Empty positions are skipped.
std::vector<LoadedSpool> loaded_printer_spools(const std::string& printer_name);

// Asks which rack slot feeds each color in the open project.
// slot_for_tool[tool] is the 0-based slot. Returns false if the user cancels.
// When task_name is set, the dialog is the start-print sheet and edited_task receives the name the user kept.
bool ask_filament_match(wxWindow* parent, const std::string& printer_name, std::vector<int>& slot_for_tool, const std::string& task_name = {}, const std::string& print_time = {}, const std::string& filament_use = {}, std::string* edited_task = nullptr, std::string* chosen_printer = nullptr, const wxImage* preview = nullptr);

// Edit the color and material loaded in one slot. Returns false if cancelled.
bool edit_rack_slot(wxWindow* parent, RackSlot& slot, int index, bool multi_color_unit);

// Rewrite tool-change commands so each project color is requested from the chosen slot.
// Plain G-code only. A single-color file with no tool change gets one inserted when the slot is not 0.
bool remap_gcode_tools(const std::string& path, const std::vector<int>& slot_for_tool);

// Latest job and ACE reply for one physical printer. Used to block a second print
// and to fill the material card when the saved rack key does not match the current name.
void remember_printer_activity(const std::string& printer_name, const AnycubicReadings& readings);
bool printer_is_busy(const std::string& printer_name);
// Copies the last ACE reply onto the rack. False when nothing changed.
bool apply_remembered_ace(const std::string& printer_name, FilamentRack& rack);

}}

#endif
