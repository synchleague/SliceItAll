///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#ifndef slic3r_GUI_DashboardSite_hpp_
#define slic3r_GUI_DashboardSite_hpp_

#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// Private page for the Devices dashboard. It listens on this computer's
// home-network address. Reaching it from outside the house is a VPN into
// that network.
class DashboardSite
{
public:
    static constexpr unsigned short default_port = 8765;

    static DashboardSite& instance();

    // error is "in_use" or "failed" when this returns false.
    bool start(unsigned short port, std::string& error);
    void stop();
    bool running() const;
    unsigned short port() const;
    std::vector<std::string> urls() const;
    void set_snapshot(std::string json);

private:
    DashboardSite();
    ~DashboardSite();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Copies the current device list into the site. Call this on the UI thread.
void publish_dashboard_snapshot();

}}

#endif
