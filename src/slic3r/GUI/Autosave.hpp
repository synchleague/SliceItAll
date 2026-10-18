///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GUI_Autosave_hpp_
#define slic3r_GUI_Autosave_hpp_

#include <string>

namespace Slic3r {
namespace GUI {

// One silent recovery file per open project. A file is removed when that project
// is saved, when its tab is closed, or when the app closes normally. Files left
// behind by a crash are offered together on the next launch.
class Autosave
{
public:
    static Autosave& instance();

    // Ask to restore projects left behind by a crash. True when the user restored any.
    bool offer_recovery();
    void start();
    // Normal shutdown. Drops every recovery file so the next launch stays quiet.
    void stop();
    // The user saved the project that is currently open.
    void forget();
    // The user closed that project tab.
    void forget_id(int autosave_id);
    // The open project was just parked on its tab. Its model is already copied there.
    void note_parked(int autosave_id, const std::string& title);
    // That project is back on the platter.
    void note_activated(int autosave_id);

private:
    Autosave();
    ~Autosave();
    Autosave(const Autosave&) = delete;
    Autosave& operator=(const Autosave&) = delete;

    struct Impl;
    Impl* m_impl;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Autosave_hpp_
