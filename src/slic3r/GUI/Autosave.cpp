///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "Autosave.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultipleBeds.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/filename.h>
#include <wx/timer.h>

namespace Slic3r {
namespace GUI {

namespace {

constexpr int k_tick_ms = 2000;
constexpr auto k_quiet = std::chrono::seconds(8);
constexpr auto k_max_gap = std::chrono::seconds(45);

struct Stamp {
    size_t snapshot { size_t(-1) };
    bool   presets { false };
    bool operator==(const Stamp& other) const
    {
        return snapshot == other.snapshot && presets == other.presets;
    }
};

namespace fs = boost::filesystem;

fs::path autosave_dir()
{
    return fs::path(data_dir()) / "autosave";
}

bool ends_with(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool digits_after(const std::string& prefix, const std::string& value)
{
    if (value.size() <= prefix.size() || value.compare(0, prefix.size(), prefix) != 0)
        return false;
    for (size_t i = prefix.size(); i < value.size(); ++i)
        if (value[i] < '0' || value[i] > '9')
            return false;
    return true;
}

// recovery.3mf is the single file written before each project had its own.
bool is_autosave_filename(const std::string& name)
{
    if (name == "source.txt" || name == "recovery.3mf" || name == "recovery.3mf.new")
        return true;
    std::string stem = name;
    if (ends_with(stem, ".new"))
        stem.erase(stem.size() - 4);
    if (ends_with(stem, ".3mf"))
        stem.erase(stem.size() - 4);
    else if (ends_with(stem, ".txt"))
        stem.erase(stem.size() - 4);
    else
        return false;
    return digits_after("p", stem) || digits_after("hold-", stem);
}

fs::path project_file(int id)
{
    if (id <= 0)
        return autosave_dir() / "recovery.3mf";
    return autosave_dir() / ("p" + std::to_string(id) + ".3mf");
}

fs::path part_path(const fs::path& file)
{
    return fs::path(file.string() + ".new");
}

fs::path meta_path_for(const fs::path& file)
{
    if (file.filename() == "recovery.3mf")
        return file.parent_path() / "source.txt";
    fs::path meta = file;
    meta.replace_extension(".txt");
    return meta;
}

fs::path project_meta(int id)
{
    return meta_path_for(project_file(id));
}

void remove_file(const fs::path& path)
{
    boost::system::error_code ec;
    fs::remove(path, ec);
}

bool file_ready(const fs::path& path)
{
    boost::system::error_code ec;
    return fs::is_regular_file(path, ec) && !ec && fs::file_size(path, ec) > 0 && !ec;
}

bool file_exists(const fs::path& path)
{
    boost::system::error_code ec;
    return fs::exists(path, ec) && !ec;
}

void write_meta(const fs::path& path, const std::string& project, const std::string& title)
{
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    if (out.good())
        out << project << "\n" << title << "\n";
}

void read_meta(const fs::path& path, std::string& project, std::string& title)
{
    project.clear();
    title.clear();
    boost::nowide::ifstream in(path.string());
    if (!in.good())
        return;
    std::getline(in, project);
    std::getline(in, title);
    while (!project.empty() && (project.back() == '\r' || project.back() == '\n'))
        project.pop_back();
    while (!title.empty() && (title.back() == '\r' || title.back() == '\n'))
        title.pop_back();
}

Stamp current_stamp(const Plater* plater)
{
    Stamp stamp;
    if (!plater)
        return stamp;
    stamp.snapshot = plater->undo_redo_stack_main().active_snapshot_time();
    stamp.presets = plater->is_presets_dirty();
    return stamp;
}

struct RecoveryItem {
    fs::path    file;
    std::string source;
    std::string title;
};

void remove_autosave_files()
{
    boost::system::error_code ec;
    if (!fs::is_directory(autosave_dir(), ec) || ec)
        return;
    fs::directory_iterator it(autosave_dir(), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->path(), ec) || ec)
            continue;
        if (is_autosave_filename(it->path().filename().string()))
            remove_file(it->path());
    }
}

// A crash between deleting the previous file and renaming the new one leaves only the partial.
void promote_partials()
{
    boost::system::error_code ec;
    if (!fs::is_directory(autosave_dir(), ec) || ec)
        return;
    std::vector<fs::path> partials;
    fs::directory_iterator it(autosave_dir(), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (ends_with(name, ".3mf.new") && is_autosave_filename(name))
            partials.push_back(it->path());
    }
    for (const fs::path& partial : partials) {
        const std::string name = partial.filename().string();
        const fs::path final_file = partial.parent_path() / name.substr(0, name.size() - 4);
        if (file_ready(final_file))
            remove_file(partial);
        else {
            fs::rename(partial, final_file, ec);
            if (ec)
                remove_file(partial);
        }
    }
}

std::vector<RecoveryItem> list_recoveries()
{
    promote_partials();
    std::vector<RecoveryItem> items;
    boost::system::error_code ec;
    if (!fs::is_directory(autosave_dir(), ec) || ec)
        return items;
    fs::directory_iterator it(autosave_dir(), ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!ends_with(name, ".3mf") || ends_with(name, ".new") || !is_autosave_filename(name))
            continue;
        if (!file_ready(it->path()))
            continue;
        RecoveryItem item;
        item.file = it->path();
        read_meta(meta_path_for(item.file), item.source, item.title);
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const RecoveryItem& left, const RecoveryItem& right) {
        return left.file.filename().string() < right.file.filename().string();
    });
    return items;
}

wxString recovery_label(const RecoveryItem& item)
{
    if (!item.source.empty()) {
        const wxString name = wxFileName(from_u8(item.source)).GetFullName();
        if (!name.empty())
            return name;
    }
    if (!item.title.empty())
        return from_u8(item.title);
    return _L("Untitled");
}

} // namespace

struct Autosave::Impl : wxEvtHandler {
    struct Track {
        Stamp saved_stamp;
        Stamp pending_stamp;
        Stamp parked_stamp;
        std::chrono::steady_clock::time_point pending_since {};
        std::chrono::steady_clock::time_point last_success {};
        bool have_success { false };
        bool file_current { false };
        bool parked_pending { false };
        bool have_parked_config { false };
        DynamicPrintConfig parked_config;
        std::string parked_project;
        std::string parked_title;
    };

    struct WriteResult {
        int id { 0 };
        int code { 0 };
        uint64_t epoch { 0 };
        uint64_t generation { 0 };
    };

    wxTimer timer;
    std::thread worker;
    std::mutex gate;
    std::mutex result_mu;
    std::map<int, uint64_t> epochs;
    std::map<int, Track> tracks;
    WriteResult result;
    bool result_ready { false };
    std::atomic<bool> writing { false };
    std::atomic<uint64_t> generation { 1 };
    bool running { false };

    Impl()
    {
        timer.SetOwner(this);
        timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    }

    ~Impl()
    {
        timer.Stop();
        generation.fetch_add(1);
        if (worker.joinable())
            worker.join();
    }

    uint64_t current_epoch(int id)
    {
        std::lock_guard<std::mutex> lock(gate);
        const auto it = epochs.find(id);
        if (it == epochs.end() || it->second == 0)
            return 1;
        return it->second;
    }

    void bump_epoch(int id)
    {
        std::lock_guard<std::mutex> lock(gate);
        uint64_t& epoch = epochs[id];
        if (epoch == 0)
            epoch = 1;
        ++epoch;
    }

    bool job_alive(int id, uint64_t job_epoch, uint64_t job_generation)
    {
        if (job_generation != generation.load())
            return false;
        return current_epoch(id) == job_epoch;
    }

    Track& track_for(int id)
    {
        const auto it = tracks.find(id);
        if (it != tracks.end())
            return it->second;
        {
            std::lock_guard<std::mutex> lock(gate);
            if (epochs[id] == 0)
                epochs[id] = 1;
        }
        return tracks.emplace(id, Track{}).first->second;
    }

    void clear_parked(Track& track)
    {
        track.parked_pending = false;
        track.have_parked_config = false;
        track.parked_config = DynamicPrintConfig();
        track.parked_project.clear();
        track.parked_title.clear();
    }

    bool recovery_present(int id)
    {
        if (tracks.find(id) != tracks.end())
            return true;
        const fs::path file = project_file(id);
        return file_exists(file) || file_exists(part_path(file)) || file_exists(project_meta(id));
    }

    void discard_id(int id)
    {
        if (!recovery_present(id))
            return;
        bump_epoch(id);
        tracks.erase(id);
        const fs::path file = project_file(id);
        remove_file(file);
        remove_file(part_path(file));
        remove_file(project_meta(id));
    }

    void discard_all()
    {
        generation.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(gate);
            for (auto& entry : epochs)
                ++entry.second;
        }
        tracks.clear();
        remove_autosave_files();
    }

    bool due(Track& track, const Stamp& now, std::chrono::steady_clock::time_point time)
    {
        if (track.file_current && now == track.saved_stamp)
            return false;
        if (!(now == track.pending_stamp) || track.pending_since == std::chrono::steady_clock::time_point{}) {
            track.pending_stamp = now;
            track.pending_since = time;
        }
        const bool quiet = time - track.pending_since >= k_quiet;
        const bool overdue = track.have_success
            && track.last_success != std::chrono::steady_clock::time_point{}
            && time - track.last_success >= k_max_gap;
        return quiet || overdue;
    }

    bool project_is_parked_dirty(int id) const
    {
        MainFrame* frame = wxGetApp().mainframe;
        if (!frame)
            return false;
        for (const ProjectRecoveryState& state : frame->project_recovery_states())
            if (state.id == id)
                return state.dirty && !state.live;
        return false;
    }

    void apply_result()
    {
        WriteResult finished;
        {
            std::lock_guard<std::mutex> lock(result_mu);
            if (!result_ready)
                return;
            finished = result;
            result_ready = false;
        }
        if (finished.generation != generation.load())
            return;
        const auto it = tracks.find(finished.id);
        if (it == tracks.end() || current_epoch(finished.id) != finished.epoch)
            return;
        Track& track = it->second;
        if (finished.code == 1) {
            track.have_success = true;
            track.file_current = true;
            track.last_success = std::chrono::steady_clock::now();
            if (!track.parked_pending || track.parked_stamp == track.saved_stamp)
                clear_parked(track);
        } else {
            track.have_success = false;
            track.file_current = false;
            track.saved_stamp = {};
            if (project_is_parked_dirty(finished.id))
                track.parked_pending = true;
        }
    }

    bool begin_write(int id, Model model, DynamicPrintConfig config, const std::string& project, const std::string& title, const Stamp& stamp)
    {
        if (writing.load())
            return false;
        if (worker.joinable())
            worker.join();

        Track& track = track_for(id);
        track.saved_stamp = stamp;
        track.file_current = false;
        const uint64_t job_epoch = current_epoch(id);
        const uint64_t job_generation = generation.load();
        const fs::path dest = project_file(id);
        const fs::path part = part_path(dest);
        const fs::path meta = project_meta(id);
        const std::string part_utf8 = into_u8(from_path(part));
        const bool full_pathnames = wxGetApp().app_config && wxGetApp().app_config->get_bool("export_sources_full_pathnames");

        writing.store(true);
        try {
            worker = std::thread([this, model = std::move(model), config = std::move(config), project, title, part_utf8, dest, part, meta, id, job_epoch, job_generation, full_pathnames]() mutable {
                bool ok = false;
                try {
                    fs::create_directories(autosave_dir());
                    ok = store_3mf(part_utf8.c_str(), &model, &config, full_pathnames, nullptr);
                    if (!ok || !job_alive(id, job_epoch, job_generation)) {
                        remove_file(part);
                        ok = false;
                    } else {
                        remove_file(dest);
                        if (!job_alive(id, job_epoch, job_generation)) {
                            remove_file(part);
                            ok = false;
                        } else {
                            boost::system::error_code ec;
                            fs::rename(part, dest, ec);
                            ok = !ec && file_ready(dest);
                            if (!ok) {
                                remove_file(part);
                                remove_file(dest);
                            } else if (!job_alive(id, job_epoch, job_generation)) {
                                remove_file(dest);
                                remove_file(meta);
                                ok = false;
                            } else {
                                write_meta(meta, project, title);
                                if (!job_alive(id, job_epoch, job_generation)) {
                                    remove_file(dest);
                                    remove_file(meta);
                                    ok = false;
                                }
                            }
                        }
                    }
                } catch (...) {
                    remove_file(part);
                    ok = false;
                }
                {
                    std::lock_guard<std::mutex> lock(result_mu);
                    result.id = id;
                    result.code = ok ? 1 : 2;
                    result.epoch = job_epoch;
                    result.generation = job_generation;
                    result_ready = true;
                }
                writing.store(false);
            });
        } catch (...) {
            writing.store(false);
            track.file_current = false;
            track.saved_stamp = {};
            return false;
        }
        return true;
    }

    bool write_live(const ProjectRecoveryState& state, const Stamp& stamp)
    {
        Plater* plater = wxGetApp().plater();
        if (!plater)
            return false;
        Model model;
        DynamicPrintConfig config;
        try {
            s_multiple_beds.capture_bed_locks(plater->model());
            model = plater->model();
            if (wxGetApp().preset_bundle)
                config = wxGetApp().preset_bundle->full_config_secure();
        } catch (const std::exception&) {
            return false;
        }
        return begin_write(state.id, std::move(model), std::move(config), state.filename, state.title, stamp);
    }

    bool write_parked(int id)
    {
        MainFrame* frame = wxGetApp().mainframe;
        Track& track = track_for(id);
        if (!frame)
            return false;
        Model model;
        std::string filename;
        std::string title;
        try {
            if (!frame->copy_parked_project(id, model, filename, title)) {
                track.parked_pending = false;
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
        if (filename.empty())
            filename = track.parked_project;
        if (title.empty())
            title = track.parked_title;
        DynamicPrintConfig config;
        try {
            if (track.have_parked_config)
                config = track.parked_config;
            else if (wxGetApp().preset_bundle)
                config = wxGetApp().preset_bundle->full_config_secure();
        } catch (const std::exception&) {
            return false;
        }
        return begin_write(id, std::move(model), std::move(config), filename, title, track.parked_stamp);
    }

    std::vector<ProjectRecoveryState> open_projects() const
    {
        std::vector<ProjectRecoveryState> projects;
        if (MainFrame* frame = wxGetApp().mainframe)
            projects = frame->project_recovery_states();
        if (!projects.empty())
            return projects;
        Plater* plater = wxGetApp().plater();
        if (!plater)
            return projects;
        ProjectRecoveryState only;
        only.live = true;
        only.dirty = plater->is_project_dirty();
        only.filename = into_u8(plater->get_project_filename(".3mf"));
        projects.push_back(std::move(only));
        return projects;
    }

    void tick()
    {
        if (!running)
            return;
        if (!writing.load())
            apply_result();
        if (writing.load())
            return;

        Plater* plater = wxGetApp().plater();
        if (!plater || wxGetApp().is_gcode_viewer())
            return;

        int parked_id = -1;
        const auto time = std::chrono::steady_clock::now();
        for (ProjectRecoveryState& state : open_projects()) {
            if (!state.dirty) {
                discard_id(state.id);
                continue;
            }
            Track& track = track_for(state.id);
            if (state.live) {
                const Stamp now = current_stamp(plater);
                if (due(track, now, time) && write_live(state, now))
                    return;
            } else {
                if (!track.parked_pending && !track.file_current) {
                    track.parked_pending = true;
                    track.parked_stamp.snapshot = 1;
                    track.parked_stamp.presets = false;
                    if (track.pending_since == std::chrono::steady_clock::time_point{})
                        track.pending_since = time;
                }
                if (parked_id < 0 && track.parked_pending && due(track, track.parked_stamp, time))
                    parked_id = state.id;
            }
        }
        if (parked_id >= 0)
            write_parked(parked_id);
    }

    void on_parked(int id, const std::string& title)
    {
        if (!running || id < 0)
            return;
        Plater* plater = wxGetApp().plater();
        if (!plater)
            return;
        if (!plater->is_project_dirty()) {
            discard_id(id);
            return;
        }
        Track& track = track_for(id);
        const Stamp now = current_stamp(plater);
        if (track.file_current && now == track.saved_stamp) {
            clear_parked(track);
            return;
        }
        track.parked_pending = true;
        track.parked_stamp = now;
        track.parked_project = into_u8(plater->get_project_filename(".3mf"));
        track.parked_title = title;
        track.have_parked_config = false;
        try {
            if (wxGetApp().preset_bundle) {
                track.parked_config = wxGetApp().preset_bundle->full_config_secure();
                track.have_parked_config = true;
            }
        } catch (const std::exception&) {
            track.have_parked_config = false;
        }
        if (!(now == track.pending_stamp) || track.pending_since == std::chrono::steady_clock::time_point{}) {
            track.pending_stamp = now;
            track.pending_since = std::chrono::steady_clock::now();
        }
    }

    void on_activated(int id)
    {
        if (!running)
            return;
        const auto it = tracks.find(id);
        if (it == tracks.end())
            return;
        Track& track = it->second;
        const bool needs_write = track.parked_pending || !track.file_current;
        const auto pending_since = track.pending_since;
        clear_parked(track);
        const Stamp now = current_stamp(wxGetApp().plater());
        track.saved_stamp = now;
        track.pending_stamp = now;
        track.file_current = !needs_write;
        if (needs_write && pending_since != std::chrono::steady_clock::time_point{})
            track.pending_since = pending_since;
        else
            track.pending_since = std::chrono::steady_clock::now();
    }

    bool adopt_file(int id, const fs::path& from, const std::string& source, const std::string& title)
    {
        const fs::path dest = project_file(id);
        const fs::path meta = project_meta(id);
        boost::system::error_code ec;
        if (!from.empty() && from != dest) {
            fs::remove(dest, ec);
            ec.clear();
            fs::rename(from, dest, ec);
            if (ec || !file_ready(dest)) {
                ec.clear();
                fs::copy_file(from, dest, fs::copy_options::overwrite_existing, ec);
                if (!ec)
                    fs::remove(from, ec);
            }
        }
        if (!file_ready(dest))
            return false;
        write_meta(meta, source, title);
        remove_file(part_path(dest));
        const fs::path from_meta = meta_path_for(from);
        if (!from.empty() && from_meta != meta)
            remove_file(from_meta);
        Track& track = track_for(id);
        track.saved_stamp = current_stamp(wxGetApp().plater());
        track.pending_stamp = track.saved_stamp;
        track.pending_since = std::chrono::steady_clock::now();
        track.last_success = track.pending_since;
        track.have_success = true;
        track.file_current = true;
        clear_parked(track);
        return true;
    }

    void stage_for_restore(std::vector<RecoveryItem>& items)
    {
        for (size_t i = 0; i < items.size(); ++i) {
            const fs::path hold = autosave_dir() / ("hold-" + std::to_string(i) + ".3mf");
            const fs::path hold_meta = meta_path_for(hold);
            if (items[i].file == hold)
                continue;
            boost::system::error_code ec;
            fs::remove(hold, ec);
            fs::rename(items[i].file, hold, ec);
            if (ec || !file_ready(hold))
                continue;
            const fs::path old_meta = meta_path_for(items[i].file);
            fs::remove(hold_meta, ec);
            fs::rename(old_meta, hold_meta, ec);
            if (ec)
                write_meta(hold_meta, items[i].source, items[i].title);
            items[i].file = hold;
        }
    }
};

Autosave& Autosave::instance()
{
    static Autosave autosave;
    return autosave;
}

Autosave::Autosave()
    : m_impl(new Impl())
{
}

Autosave::~Autosave()
{
    if (!m_impl)
        return;
    m_impl->running = false;
    m_impl->generation.fetch_add(1);
    if (m_impl->worker.joinable())
        m_impl->worker.join();
    delete m_impl;
    m_impl = nullptr;
}

bool Autosave::offer_recovery()
{
    const std::vector<RecoveryItem> found = list_recoveries();
    if (found.empty())
        return false;
    MainFrame* frame = wxGetApp().mainframe;
    Plater* plater = wxGetApp().plater();
    if (!frame || !plater)
        return false;

    wxString message;
    if (found.size() == 1) {
        const bool named = !found.front().source.empty() || !found.front().title.empty();
        message = named
            ? format_wxstr(_L("%1% closed unexpectedly. Restore the autosaved copy of \"%2%\"?"), wxString(SLIC3R_APP_NAME), recovery_label(found.front()))
            : format_wxstr(_L("%1% closed unexpectedly. An autosaved project can be restored."), wxString(SLIC3R_APP_NAME));
    } else {
        message = format_wxstr(_L("%1% closed unexpectedly. These autosaved projects can be restored:"), wxString(SLIC3R_APP_NAME));
        for (const RecoveryItem& item : found) {
            message += "\n- ";
            message += recovery_label(item);
        }
    }
    RichMessageDialog dialog(frame, message, _L("Recover project"), wxYES_NO | wxICON_QUESTION);
    dialog.SetYesNoLabels(found.size() > 1 ? _L("Recover all") : _L("Recover"), _L("Start fresh"));
    if (dialog.ShowModal() != wxID_YES) {
        m_impl->discard_all();
        return false;
    }

    std::vector<RecoveryItem> items = found;
    m_impl->stage_for_restore(items);

    bool any = false;
    std::vector<wxString> failed;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            frame->new_project_tab();
        const wxString original = items[i].source.empty() ? wxString() : from_u8(items[i].source);
        if (!plater->restore_autosaved_project(from_path(items[i].file), original)) {
            failed.push_back(recovery_label(items[i]));
            continue;
        }
        if (m_impl->adopt_file(frame->live_autosave_id(), items[i].file, items[i].source, items[i].title))
            any = true;
        else
            failed.push_back(recovery_label(items[i]));
    }

    if (!failed.empty()) {
        wxString warning = failed.size() == 1 && found.size() == 1
            ? wxString(_L("The autosaved project could not be opened."))
            : wxString(_L("Some autosaved projects could not be opened:"));
        if (failed.size() > 1 || found.size() > 1) {
            warning = _L("Some autosaved projects could not be opened:");
            for (const wxString& name : failed) {
                warning += "\n- ";
                warning += name;
            }
        }
        MessageDialog error(frame, warning, _L("Recover project"), wxOK | wxICON_WARNING);
        error.ShowModal();
    }
    if (any)
        frame->select_active_project_tab();
    return any;
}

void Autosave::start()
{
    if (m_impl->running || wxGetApp().is_gcode_viewer())
        return;
    m_impl->running = true;
    m_impl->timer.Start(k_tick_ms);
}

void Autosave::stop()
{
    if (!m_impl)
        return;
    m_impl->running = false;
    m_impl->timer.Stop();
    m_impl->generation.fetch_add(1);
    if (m_impl->worker.joinable())
        m_impl->worker.join();
    m_impl->writing.store(false);
    {
        std::lock_guard<std::mutex> lock(m_impl->gate);
        m_impl->epochs.clear();
    }
    m_impl->tracks.clear();
    remove_autosave_files();
}

void Autosave::forget()
{
    if (!m_impl)
        return;
    int id = 0;
    if (MainFrame* frame = wxGetApp().mainframe)
        id = frame->live_autosave_id();
    m_impl->discard_id(id);
}

void Autosave::forget_id(int autosave_id)
{
    if (!m_impl)
        return;
    m_impl->discard_id(autosave_id);
}

void Autosave::note_parked(int autosave_id, const std::string& title)
{
    if (!m_impl)
        return;
    m_impl->on_parked(autosave_id, title);
}

void Autosave::note_activated(int autosave_id)
{
    if (!m_impl)
        return;
    m_impl->on_activated(autosave_id);
}

} // namespace GUI
} // namespace Slic3r
