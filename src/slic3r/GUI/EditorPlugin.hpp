///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_EditorPlugin_hpp_
#define slic3r_EditorPlugin_hpp_

#include <functional>
#include <string>
#include <vector>

namespace Slic3r::GUI {

// A tool that extends slicing or mesh editing.
// A web plugin loads the author's site inside the editor. A native plugin supplies open_native.
struct EditorPlugin {
    std::string id;
    std::string title;
    std::string credit;
    std::string summary;
    std::string url;
    // "supports" and "surface" add a button on that process page. Empty stays in the Plugins menu only.
    std::string slot;
    std::function<void()> open_native;
};

void register_editor_plugin(EditorPlugin plugin);
void ensure_builtin_editor_plugins();
const std::vector<EditorPlugin>& editor_plugins();
const EditorPlugin* find_editor_plugin(const std::string& id);
void open_editor_plugin(const std::string& id);

} // namespace Slic3r::GUI

#endif
