///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher

#include "Catalog.hpp"
#include "PresetBundle.hpp"

#include "Exception.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace Slic3r {
namespace {

const char *builtin_catalog()
{
    return
        "# Slice it All printer and material catalog\n"
        "# Edit this database in the app. Presets with source = user are yours.\n"
        "# A newer catalog adds and refreshes shared presets and keeps yours.\n"
        "#\n"
        "# Materials are available on every printer of the same kind.\n"
        "# optimized_for lists printer model ids (or printer preset names) the profile was tuned for.\n"
        "# Leave it empty for a general profile.\n"
        "#\n"
        "# inherits names a profile that is already installed.\n"
        "# Any other key is a print setting, for example temperature or exposure_time.\n"
        "\n"
        "[catalog]\n"
        "version = 1\n"
        "\n"
        "# [material:My PLA]\n"
        "# technology = FFF\n"
        "# inherits = Generic PLA\n"
        "# optimized_for = KOBRAS1, ENDER3\n"
        "# source = user\n"
        "# temperature = 210\n"
        "# first_layer_temperature = 215\n"
        "# bed_temperature = 60\n"
        "# filament_type = PLA\n"
        "\n"
        "# [material:My Resin]\n"
        "# technology = SLA\n"
        "# inherits = Generic Resin\n"
        "# optimized_for = MARS4MAX\n"
        "# source = user\n"
        "# exposure_time = 2.5\n"
        "\n"
        "# [printer:My Printer]\n"
        "# technology = FFF\n"
        "# inherits = Ender-3\n"
        "# printer_model = MYPRINTER\n"
        "# source = user\n"
        "\n";
}

bool is_sla_technology(const std::string &technology)
{
    return boost::iequals(technology, "SLA") || boost::iequals(technology, "DLP") || boost::iequals(technology, "MSLA");
}

bool is_meta_key(const std::string &key)
{
    return key == "technology" || key == "inherits" || key == "optimized_for" || key == "brand" || key == "source";
}

std::vector<std::string> split_flags(const std::string &text)
{
    std::vector<std::string> flags;
    std::string item;
    for (char c : text) {
        if (c == ',' || c == ';') {
            boost::trim(item);
            if (!item.empty())
                flags.push_back(item);
            item.clear();
        } else
            item.push_back(c);
    }
    boost::trim(item);
    if (!item.empty())
        flags.push_back(item);
    return flags;
}

std::map<std::string, std::string> settings_map(const CatalogEntry &entry)
{
    return std::map<std::string, std::string>(entry.settings.begin(), entry.settings.end());
}

bool same_body(const CatalogEntry &lhs, const CatalogEntry &rhs)
{
    return lhs.technology == rhs.technology
        && lhs.inherits == rhs.inherits
        && lhs.brand == rhs.brand
        && lhs.optimized_for == rhs.optimized_for
        && settings_map(lhs) == settings_map(rhs);
}

const CatalogEntry *find_entry(const CatalogDocument &doc, const CatalogEntry &entry)
{
    for (const CatalogEntry &other : doc.entries)
        if (catalog_same_identity(other, entry))
            return &other;
    return nullptr;
}

CatalogEntry *find_entry(CatalogDocument &doc, const CatalogEntry &entry)
{
    return const_cast<CatalogEntry *>(find_entry(static_cast<const CatalogDocument &>(doc), entry));
}

bool parse_section_name(const std::string &section, CatalogEntry &entry)
{
    const auto pos = section.find(':');
    if (pos == std::string::npos)
        return false;
    const std::string kind = section.substr(0, pos);
    entry.name = boost::trim_copy(section.substr(pos + 1));
    if (entry.name.empty())
        return false;
    if (kind == "printer") {
        entry.printer = true;
        return true;
    }
    if (kind == "material") {
        entry.printer = false;
        return true;
    }
    if (kind == "filament") {
        entry.printer = false;
        entry.technology = "FFF";
        return true;
    }
    if (kind == "sla_material") {
        entry.printer = false;
        entry.technology = "SLA";
        return true;
    }
    return false;
}

void read_entry(const boost::property_tree::ptree &section, CatalogEntry &entry)
{
    if (const auto technology = section.get_optional<std::string>("technology"))
        entry.technology = boost::trim_copy(*technology);
    if (entry.technology.empty())
        entry.technology = "FFF";
    entry.inherits = boost::trim_copy(section.get<std::string>("inherits", ""));
    entry.brand = boost::trim_copy(section.get<std::string>("brand", ""));
    entry.optimized_for = boost::trim_copy(section.get<std::string>("optimized_for", ""));
    if (const auto source = section.get_optional<std::string>("source")) {
        entry.source_explicit = true;
        entry.user_owned = !boost::iequals(boost::trim_copy(*source), "catalog");
    }
    for (const auto &kv : section) {
        if (is_meta_key(kv.first))
            continue;
        entry.settings.emplace_back(kv.first, kv.second.data());
    }
}

PresetCollection &collection_for(PresetBundle &bundle, const CatalogEntry &entry)
{
    if (entry.printer)
        return bundle.printers;
    if (is_sla_technology(entry.technology))
        return bundle.sla_materials;
    return bundle.filaments;
}

DynamicPrintConfig config_from_entry(PresetCollection &collection, const CatalogEntry &entry)
{
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
    const Preset *base = nullptr;
    if (!entry.inherits.empty())
        base = collection.find_preset(entry.inherits, false, false);
    if (base == nullptr || base->is_default) {
        DynamicPrintConfig hint;
        hint.set_deserialize_nothrow("printer_technology", is_sla_technology(entry.technology) ? "SLA" : "FFF", ctxt);
        base = &collection.default_preset_for(hint);
    }

    DynamicPrintConfig cfg = base->config;
    for (const auto &kv : entry.settings) {
        if (!print_config_def.has(kv.first))
            continue;
        cfg.set_deserialize_nothrow(kv.first, kv.second, ctxt);
    }
    if (entry.printer)
        cfg.set_deserialize_nothrow("printer_technology", is_sla_technology(entry.technology) ? "SLA" : "FFF", ctxt);
    Preset::inherits(cfg) = entry.inherits;
    if (!entry.printer && !entry.brand.empty() && cfg.has("filament_vendor"))
        cfg.set_deserialize_nothrow("filament_vendor", entry.brand, ctxt);
    Preset::normalize(cfg);
    Preset::remove_invalid_keys(cfg, collection.default_preset_for(cfg).config);
    return cfg;
}

void assign_loaded_preset(Preset &preset, const std::string &file, DynamicPrintConfig &&cfg, const std::vector<std::string> &flags)
{
    preset.file = file;
    preset.config = std::move(cfg);
    preset.loaded = true;
    preset.is_dirty = false;
    preset.is_default = false;
    preset.is_system = false;
    preset.is_external = false;
    preset.is_visible = true;
    preset.vendor = nullptr;
    preset.alias.clear();
    preset.renamed_from.clear();
    preset.optimized_for = flags;
}

std::string preset_file(PresetCollection &collection, const std::string &name)
{
    std::string safe = name;
    for (char &ch : safe)
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
            ch = '_';
    std::string file = collection.path_from_name(safe);
    if (file.empty() || file == PresetBundle::user_catalog_path())
        file = safe + ".ini";
    return file;
}

// Sections edited by hand, and sections that are not in the shipped catalog, belong to the user.
bool stamp_user_ownership(CatalogDocument &user, const CatalogDocument &factory)
{
    bool changed = false;
    for (CatalogEntry &entry : user.entries) {
        if (entry.source_explicit)
            continue;
        const CatalogEntry *shipped = find_entry(factory, entry);
        entry.user_owned = shipped == nullptr || !same_body(entry, *shipped);
        entry.source_explicit = true;
        changed = true;
    }
    return changed;
}

void write_entry(std::ostream &out, const CatalogEntry &entry)
{
    out << "\n[" << (entry.printer ? "printer:" : "material:") << entry.name << "]\n";
    out << "technology = " << entry.technology << "\n";
    if (!entry.inherits.empty())
        out << "inherits = " << entry.inherits << "\n";
    if (!entry.optimized_for.empty())
        out << "optimized_for = " << entry.optimized_for << "\n";
    if (!entry.brand.empty())
        out << "brand = " << entry.brand << "\n";
    out << "source = " << (entry.user_owned ? "user" : "catalog") << "\n";
    for (const auto &kv : entry.settings)
        out << kv.first << " = " << kv.second << "\n";
}

} // namespace

bool catalog_same_identity(const CatalogEntry &lhs, const CatalogEntry &rhs)
{
    return lhs.printer == rhs.printer && lhs.name == rhs.name;
}

bool catalog_same_collection(const CatalogEntry &lhs, const CatalogEntry &rhs)
{
    return lhs.printer == rhs.printer && is_sla_technology(lhs.technology) == is_sla_technology(rhs.technology);
}

CatalogDocument load_catalog_file(const std::string &path)
{
    CatalogDocument doc;
    if (path.empty() || !boost::filesystem::exists(path))
        return doc;

    boost::property_tree::ptree tree;
    try {
        boost::nowide::ifstream in(path);
        if (!in.good())
            throw Slic3r::RuntimeError("Could not open " + path);
        boost::property_tree::read_ini(in, tree);
    } catch (const std::exception &ex) {
        throw Slic3r::RuntimeError(std::string("Could not read the catalog: ") + ex.what());
    }

    if (tree.get_child_optional("catalog")) {
        doc.full_catalog = true;
        doc.version = tree.get<int>("catalog.version", 1);
    }

    for (const auto &section : tree) {
        CatalogEntry entry;
        if (!parse_section_name(section.first, entry))
            continue;
        read_entry(section.second, entry);
        if (CatalogEntry *existing = find_entry(doc, entry))
            *existing = std::move(entry);
        else
            doc.entries.push_back(std::move(entry));
    }
    return doc;
}

void save_catalog_file(const std::string &path, const CatalogDocument &doc, bool full_catalog)
{
    boost::nowide::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.good())
        throw Slic3r::RuntimeError("Could not write " + path);

    out << "# Slice it All printer and material catalog\n";
    out << "# Presets with source = user belong to the user and are kept when this file is updated.\n";
    out << "# A file with a [catalog] section refreshes shared presets. A file without it adds presets.\n";
    if (full_catalog) {
        out << "\n[catalog]\n";
        out << "version = " << std::max(doc.version, 1) << "\n";
    }
    for (const CatalogEntry &entry : doc.entries)
        write_entry(out, entry);
}

CatalogDocument merge_catalog(const CatalogDocument &current, const CatalogDocument &incoming, bool full_update, CatalogMergeStats &stats)
{
    CatalogDocument out = current;
    stats = {};
    if (full_update)
        out.version = std::max(out.version, incoming.version);

    std::set<std::string> incoming_keys;
    for (const CatalogEntry &inc : incoming.entries) {
        incoming_keys.insert(inc.key());
        CatalogEntry *existing = find_entry(out, inc);
        if (existing == nullptr) {
            CatalogEntry added = inc;
            if (!full_update)
                added.user_owned = true;
            added.source_explicit = true;
            out.entries.push_back(std::move(added));
            ++stats.added;
        } else if (full_update && existing->user_owned) {
            ++stats.kept;
        } else {
            const bool user_owned = full_update ? inc.user_owned : true;
            *existing = inc;
            existing->user_owned = user_owned;
            existing->source_explicit = true;
            ++stats.updated;
        }
    }

    if (full_update) {
        const auto removed = std::remove_if(out.entries.begin(), out.entries.end(), [&](const CatalogEntry &entry) {
            return !entry.user_owned && incoming_keys.find(entry.key()) == incoming_keys.end();
        });
        stats.removed = int(std::distance(removed, out.entries.end()));
        out.entries.erase(removed, out.entries.end());
    }
    out.full_catalog = true;
    return out;
}

CatalogEntry preset_to_catalog_entry(const PresetCollection &presets, const Preset &preset, bool printer, const std::string &technology)
{
    CatalogEntry entry;
    entry.printer = printer;
    entry.name = preset.name;
    entry.technology = technology.empty() ? (preset.printer_technology() == ptSLA ? "SLA" : "FFF") : technology;
    entry.inherits = preset.inherits();
    if (entry.inherits == entry.name)
        entry.inherits.clear();
    entry.user_owned = true;
    entry.source_explicit = true;
    for (const std::string &flag : preset.optimized_for) {
        if (!entry.optimized_for.empty())
            entry.optimized_for += ", ";
        entry.optimized_for += flag;
    }
    if (const auto *brand = preset.config.option<ConfigOptionString>("filament_vendor"))
        entry.brand = brand->value;

    const Preset *parent = nullptr;
    if (!entry.inherits.empty())
        parent = presets.find_preset(entry.inherits, false, false);
    if (parent == nullptr || parent == &preset)
        parent = &presets.default_preset_for(preset.config);

    const std::vector<std::string> diff = preset.config.diff(parent->config);
    for (const std::string &key : diff) {
        if (is_meta_key(key) || key == "inherits" || key == "filament_vendor")
            continue;
        if (!preset.config.has(key))
            continue;
        entry.settings.emplace_back(key, preset.config.opt_serialize(key));
    }
    return entry;
}

std::string PresetBundle::user_catalog_path()
{
    return (boost::filesystem::path(data_dir()) / "catalog.ini").string();
}

void PresetBundle::ensure_user_catalog()
{
    const boost::filesystem::path user(user_catalog_path());
    const boost::filesystem::path factory = boost::filesystem::path(resources_dir()) / "profiles" / "catalog.ini";
    const boost::filesystem::path bundle_dir = boost::filesystem::path(data_dir()) / "catalog";
    boost::system::error_code ec;
    boost::filesystem::create_directories(bundle_dir, ec);

    const boost::filesystem::path readme = bundle_dir / "README.txt";
    if (!boost::filesystem::exists(readme)) {
        boost::nowide::ofstream out(readme.string());
        out <<
            "The printer and material catalog is edited in the app.\n"
            "Import merges a preset file into catalog.ini, next to this folder.\n"
            "A full catalog file (one with a [catalog] section) refreshes shared presets.\n"
            "Presets with source = user are kept, including when a newer catalog is installed with the app.\n"
            "Export writes one preset, or the whole catalog, so it can be sent and merged.\n";
    }

    if (!boost::filesystem::exists(user)) {
        if (boost::filesystem::exists(factory))
            boost::filesystem::copy_file(factory, user, ec);
        if (!boost::filesystem::exists(user)) {
            boost::nowide::ofstream out(user.string());
            out << builtin_catalog();
        }
        return;
    }

    CatalogDocument user_doc;
    CatalogDocument factory_doc;
    try {
        user_doc = load_catalog_file(user.string());
        if (boost::filesystem::exists(factory))
            factory_doc = load_catalog_file(factory.string());
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Could not prepare the printer and material catalog: " << ex.what();
        return;
    }

    bool rewrite = stamp_user_ownership(user_doc, factory_doc);
    if (factory_doc.full_catalog && factory_doc.version > user_doc.version) {
        CatalogMergeStats stats;
        user_doc = merge_catalog(user_doc, factory_doc, true, stats);
        user_doc.version = factory_doc.version;
        rewrite = true;
        BOOST_LOG_TRIVIAL(info) << "Merged catalog version " << factory_doc.version
            << " (" << stats.added << " added, " << stats.updated << " updated, "
            << stats.kept << " kept, " << stats.removed << " shared presets removed).";
    }
    if (!rewrite)
        return;
    try {
        save_catalog_file(user.string(), user_doc, true);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Could not write the printer and material catalog: " << ex.what();
    }
}

void PresetBundle::install_catalog(const CatalogDocument &doc, bool update_existing)
{
    for (const CatalogEntry &entry : doc.entries) {
        if (entry.name.empty())
            continue;
        PresetCollection &collection = collection_for(*this, entry);
        const std::vector<std::string> flags = split_flags(entry.optimized_for);
        Preset *existing = collection.find_preset(entry.name, false, false);
        if (existing != nullptr && (existing->is_default || existing->is_system)) {
            existing->optimized_for = flags;
            continue;
        }
        // Shared catalog presets pick up a newer catalog. Presets the user owns stay as saved.
        if (existing != nullptr && !update_existing && entry.user_owned) {
            existing->optimized_for = flags;
            continue;
        }

        DynamicPrintConfig cfg = config_from_entry(collection, entry);
        const std::string file = preset_file(collection, entry.name);
        if (existing == nullptr) {
            Preset &added = collection.load_preset(file, entry.name, std::move(cfg), false);
            added.is_default = false;
            added.is_system = false;
            added.is_external = false;
            added.is_visible = true;
            added.vendor = nullptr;
            added.alias.clear();
            added.renamed_from.clear();
            added.optimized_for = flags;
            added.save();
        } else {
            assign_loaded_preset(*existing, file, std::move(cfg), flags);
            existing->save();
        }
    }
}

void PresetBundle::uninstall_catalog_entry(const CatalogEntry &entry)
{
    PresetCollection &collection = collection_for(*this, entry);
    Preset *existing = collection.find_preset(entry.name, false, false);
    if (existing == nullptr || existing->is_default || existing->is_system)
        return;
    if (existing->file == user_catalog_path())
        existing->file.clear();
    collection.delete_preset(entry.name);
}

void PresetBundle::load_user_catalog()
{
    CatalogDocument previous;
    try {
        if (boost::filesystem::exists(user_catalog_path()))
            previous = load_catalog_file(user_catalog_path());
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Could not read the printer and material catalog: " << ex.what();
    }

    ensure_user_catalog();

    const boost::filesystem::path bundle_dir = boost::filesystem::path(data_dir()) / "catalog";
    boost::system::error_code ec;
    if (boost::filesystem::is_directory(bundle_dir, ec)) {
        for (const auto &dir_entry : boost::filesystem::directory_iterator(bundle_dir)) {
            if (!is_ini_file(dir_entry))
                continue;
            try {
                PresetBundle other;
                other.load_configbundle(dir_entry.path().string(), LoadConfigBundleAttribute::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
                const std::vector<std::string> duplicates = this->merge_presets(std::move(other));
                if (!duplicates.empty())
                    BOOST_LOG_TRIVIAL(warning) << "Catalog bundle " << dir_entry.path().filename().string() << " repeats presets that are already installed.";
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(error) << "Could not load catalog bundle " << dir_entry.path().string() << ": " << ex.what();
            }
        }
    }

    CatalogDocument current;
    try {
        current = load_catalog_file(user_catalog_path());
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Could not load the printer and material catalog: " << ex.what();
        return;
    }

    for (const CatalogEntry &old : previous.entries) {
        if (old.user_owned || find_entry(current, old) != nullptr)
            continue;
        this->uninstall_catalog_entry(old);
    }
    this->install_catalog(current, false);
    this->update_system_maps();
}

} // namespace Slic3r
