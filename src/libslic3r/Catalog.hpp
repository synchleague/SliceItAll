///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_Catalog_hpp_
#define slic3r_Catalog_hpp_

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class Preset;
class PresetCollection;

// One printer or material in catalog.ini.
// user_owned entries are kept when a newer catalog is merged in.
struct CatalogEntry {
    bool        printer{ false };
    std::string name;
    std::string technology{ "FFF" };
    std::string inherits;
    std::string brand;
    std::string optimized_for;
    bool        user_owned{ true };
    bool        source_explicit{ false };
    std::vector<std::pair<std::string, std::string>> settings;

    std::string key() const { return (printer ? "printer:" : "material:") + name; }
};

struct CatalogDocument {
    int         version{ 1 };
    // True when the file has a [catalog] section and is a whole database, not a single preset.
    bool        full_catalog{ false };
    std::vector<CatalogEntry> entries;
};

struct CatalogMergeStats {
    int added{ 0 };
    int updated{ 0 };
    int kept{ 0 };
    int removed{ 0 };
};

CatalogDocument load_catalog_file(const std::string &path);
void            save_catalog_file(const std::string &path, const CatalogDocument &doc, bool full_catalog);
// full_update keeps user_owned entries and refreshes the shared ones.
// A preset file adds or replaces entries and marks them as the user's.
CatalogDocument merge_catalog(const CatalogDocument &current, const CatalogDocument &incoming, bool full_update, CatalogMergeStats &stats);
CatalogEntry    preset_to_catalog_entry(const PresetCollection &presets, const Preset &preset, bool printer, const std::string &technology);
bool            catalog_same_identity(const CatalogEntry &lhs, const CatalogEntry &rhs);
bool            catalog_same_collection(const CatalogEntry &lhs, const CatalogEntry &rhs);

} // namespace Slic3r

#endif
