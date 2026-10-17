///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "DashboardSite.hpp"

#include "DevicesPanel.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <vector>
#include <cstdio>
#include <mutex>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#ifdef _WIN32
#include <iphlpapi.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib")
#endif
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace Slic3r {
namespace GUI {
namespace {

std::string json_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::vector<std::string> lan_ipv4()
{
    std::vector<std::string> addresses;
    auto add = [&](const std::string& ip) {
        if (ip.empty() || ip == "0.0.0.0" || ip == "127.0.0.1" || ip.rfind("169.254.", 0) == 0)
            return;
        if (std::find(addresses.begin(), addresses.end(), ip) == addresses.end())
            addresses.push_back(ip);
    };

#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == NO_ERROR) {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                char text[INET_ADDRSTRLEN] = {};
                const auto* address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
                    add(text);
            }
        }
    }
#else
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs* iface = interfaces; iface; iface = iface->ifa_next) {
            if (!iface->ifa_addr || iface->ifa_addr->sa_family != AF_INET)
                continue;
            if ((iface->ifa_flags & IFF_LOOPBACK) != 0)
                continue;
            char text[INET_ADDRSTRLEN] = {};
            const auto* address = reinterpret_cast<sockaddr_in*>(iface->ifa_addr);
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
                add(text);
        }
        freeifaddrs(interfaces);
    }
#endif
    return addresses;
}

const char* page_html()
{
    return R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Slice it All</title>
<style>
:root { color-scheme: dark; }
body { margin: 0; font-family: "Segoe UI", sans-serif; background: #14141a; color: #ececf1; }
header { padding: 22px 24px 6px; }
h1 { margin: 0; font-size: 22px; font-weight: 650; }
h1 span { color: #FE32AD; }
.note { color: #9b9ba8; max-width: 44rem; line-height: 1.45; }
main { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 12px; padding: 12px 24px 36px; }
article { background: #1e1e27; border: 1px solid #2c2c38; border-radius: 14px; padding: 14px 16px 16px; }
.meta { color: #9b9ba8; font-size: 13px; margin-top: 4px; }
.status { display: inline-block; margin-top: 8px; font-size: 12px; padding: 2px 8px; border-radius: 999px; background: #33343c; }
.status.Ready { background: #1d4d32; }
.status.Printing { background: #4F099F; }
.status.Idle { background: #3d3a22; }
.status.Offline { background: #3d2a2a; }
.module { margin-top: 12px; }
.module h3 { margin: 0 0 4px; font-size: 13px; color: #FE32AD; font-weight: 650; }
.module p { margin: 0; white-space: pre-line; }
.cam { height: 128px; margin-top: 6px; border-radius: 10px; background: #101014; display: flex; align-items: center; justify-content: center; color: #8d8d98; }
</style>
</head>
<body>
<header>
  <h1>Slice <span>it All</span></h1>
  <p class="note">Private dashboard on this home network. Slice it All has to be open on the computer that hosts it. From outside the house, connect a VPN to this network first.</p>
  <p id="fresh" class="note"></p>
</header>
<main id="board"></main>
<script>
const known = ["Ready", "Printing", "Idle", "Offline"];
function node(tag, className, text) {
  const el = document.createElement(tag);
  if (className) el.className = className;
  if (text) el.textContent = text;
  return el;
}
function moduleBlock(card, modules, key, device) {
  const info = (modules && modules[key]) || { title: key, body: "" };
  const block = node("div", "module");
  block.append(node("h3", "", info.title || key));
  if (key === "camera") {
    block.append(node("div", "cam", (device && device.name) || "Camera"));
  } else {
    const body = (key === "now_printing" && device && device.job) ? device.job : (info.body || "");
    if (body)
      block.append(node("p", "", body));
  }
  card.append(block);
}
async function load() {
  const fresh = document.getElementById("fresh");
  let data;
  try {
    const response = await fetch("/api/dashboard", { cache: "no-store" });
    data = await response.json();
    fresh.textContent = "Updated just now.";
  } catch (err) {
    fresh.textContent = "Waiting for Slice it All.";
    return;
  }
  const board = document.getElementById("board");
  board.replaceChildren();
  const devices = data.devices || [];
  const tiles = data.tiles || [];
  const byName = new Map(devices.map(device => [device.name, device]));
  if (!tiles.length) {
    board.append(node("p", "note", "No devices yet. Add printers in the configuration wizard and they show up here."));
    return;
  }
  const seen = new Map();
  for (const tile of tiles) {
    const device = byName.get(tile.device) || { name: tile.device, brand: "", status: "Offline", host: "" };
    let card = seen.get(tile.device);
    if (!card) {
      card = node("article");
      card.append(node("h2", "", device.name || "Printer"));
      const bits = [];
      if (device.brand) bits.push(device.brand);
      if (device.host) bits.push(device.host);
      if (bits.length) card.append(node("div", "meta", bits.join("  ·  ")));
      const badge = node("div", "status" + (known.includes(device.status) ? " " + device.status : ""), device.status || "Offline");
      card.append(badge);
      seen.set(tile.device, card);
      board.append(card);
    }
    moduleBlock(card, data.modules, tile.module, device);
  }
}
load();
setInterval(load, 3000);
</script>
</body>
</html>
)html";
}

std::string http_response(const std::string& type, const std::string& body, int status, const char* reason)
{
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
        "Content-Type: " + type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n" + body;
}

std::string request_path(const std::string& request)
{
    const size_t line_end = request.find("\r\n");
    const std::string line = request.substr(0, line_end);
    if (line.rfind("GET ", 0) != 0)
        return {};
    const size_t path_begin = 4;
    const size_t path_end = line.find(' ', path_begin);
    if (path_end == std::string::npos)
        return {};
    std::string path = line.substr(path_begin, path_end - path_begin);
    const size_t query = path.find('?');
    if (query != std::string::npos)
        path.resize(query);
    return path;
}

void set_socket_timeout(boost::asio::ip::tcp::socket& socket)
{
#ifdef _WIN32
    const DWORD milliseconds = 2000;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
#else
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

} // namespace

struct DashboardSite::Impl
{
    std::mutex life;
    std::mutex snapshot_mutex;
    std::mutex clients_mutex;
    std::condition_variable clients_done;
    std::string json = "{\"devices\":[],\"tiles\":[],\"modules\":{}}";
    boost::asio::io_context io;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
    std::thread thread;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> sockets;
    int active_clients = 0;
    unsigned short port = 0;
    bool running = false;

    void arm_accept();
    void serve(boost::asio::ip::tcp::socket& socket);
};

void DashboardSite::Impl::arm_accept()
{
    if (!acceptor)
        return;
    auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io);
    acceptor->async_accept(*socket, [this, socket](const boost::system::error_code& error) {
        if (error)
            return;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            sockets.push_back(socket);
            ++active_clients;
        }
        std::thread([this, socket] {
            try {
                serve(*socket);
            } catch (...) {
            }
            boost::system::error_code ignore;
            socket->close(ignore);
            std::lock_guard<std::mutex> lock(clients_mutex);
            sockets.erase(std::remove(sockets.begin(), sockets.end(), socket), sockets.end());
            if (--active_clients == 0)
                clients_done.notify_all();
        }).detach();
        arm_accept();
    });
}

void DashboardSite::Impl::serve(boost::asio::ip::tcp::socket& socket)
{
    set_socket_timeout(socket);
    std::string request;
    char buffer[1024];
    boost::system::error_code error;
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        const size_t count = socket.read_some(boost::asio::buffer(buffer), error);
        if (error || count == 0)
            break;
        request.append(buffer, count);
    }
    const std::string path = request_path(request);
    std::string response;
    if (path.empty() && !request.empty())
        response = http_response("text/plain; charset=utf-8", "Method not allowed\n", 405, "Method Not Allowed");
    else if (path == "/" || path == "/index.html")
        response = http_response("text/html; charset=utf-8", page_html(), 200, "OK");
    else if (path == "/api/dashboard") {
        std::string body;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            body = json;
        }
        response = http_response("application/json; charset=utf-8", body, 200, "OK");
    } else
        response = http_response("text/plain; charset=utf-8", "Not found\n", 404, "Not Found");

    boost::asio::write(socket, boost::asio::buffer(response), error);
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error);
    socket.close(error);
}

DashboardSite& DashboardSite::instance()
{
    static DashboardSite site;
    return site;
}

DashboardSite::DashboardSite()
    : m_impl(std::make_unique<Impl>())
{
}

DashboardSite::~DashboardSite()
{
    stop();
}

bool DashboardSite::start(unsigned short port, std::string& error)
{
    std::lock_guard<std::mutex> lock(m_impl->life);
    if (m_impl->running)
        return true;

    m_impl->io.restart();
    m_impl->acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(m_impl->io);
    boost::system::error_code ec;
    m_impl->acceptor->open(boost::asio::ip::tcp::v4(), ec);
    if (!ec)
        m_impl->acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (!ec)
        m_impl->acceptor->bind(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), port), ec);
    if (!ec)
        m_impl->acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        m_impl->acceptor.reset();
        error = ec == boost::asio::error::address_in_use ? "in_use" : "failed";
        return false;
    }

    m_impl->port = port;
    m_impl->arm_accept();
    m_impl->thread = std::thread([this] { m_impl->io.run(); });
    m_impl->running = true;
    return true;
}

void DashboardSite::stop()
{
    std::thread io_thread;
    {
        std::lock_guard<std::mutex> lock(m_impl->life);
        if (!m_impl->running && !m_impl->thread.joinable())
            return;
        if (m_impl->acceptor) {
            boost::asio::post(m_impl->io, [this] {
                boost::system::error_code ec;
                if (m_impl->acceptor)
                    m_impl->acceptor->close(ec);
            });
        }
        io_thread = std::move(m_impl->thread);
        m_impl->running = false;
        m_impl->port = 0;
    }
    if (io_thread.joinable())
        io_thread.join();

    std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> open_sockets;
    {
        std::lock_guard<std::mutex> lock(m_impl->clients_mutex);
        open_sockets = m_impl->sockets;
    }
    for (const std::shared_ptr<boost::asio::ip::tcp::socket>& socket : open_sockets) {
        boost::system::error_code ec;
        socket->close(ec);
    }
    {
        std::unique_lock<std::mutex> lock(m_impl->clients_mutex);
        m_impl->clients_done.wait(lock, [this] { return m_impl->active_clients == 0; });
    }

    std::lock_guard<std::mutex> lock(m_impl->life);
    m_impl->acceptor.reset();
}

bool DashboardSite::running() const
{
    std::lock_guard<std::mutex> lock(m_impl->life);
    return m_impl->running;
}

unsigned short DashboardSite::port() const
{
    std::lock_guard<std::mutex> lock(m_impl->life);
    return m_impl->port;
}

std::vector<std::string> DashboardSite::urls() const
{
    const unsigned short open_port = port();
    if (open_port == 0)
        return {};
    std::vector<std::string> urls;
    for (const std::string& address : lan_ipv4())
        urls.push_back("http://" + address + ":" + std::to_string(open_port));
    if (urls.empty())
        urls.push_back("http://127.0.0.1:" + std::to_string(open_port));
    return urls;
}

void DashboardSite::set_snapshot(std::string json)
{
    std::lock_guard<std::mutex> lock(m_impl->snapshot_mutex);
    m_impl->json = std::move(json);
}

void publish_dashboard_snapshot()
{
    refresh_offline_jobs(false);
    std::string devices_json;
    std::vector<std::string> device_names;
    if (wxGetApp().preset_bundle) {
        for (const PhysicalPrinter& printer : wxGetApp().preset_bundle->physical_printers) {
            std::string brand;
            if (!printer.preset_names.empty()) {
                const std::string preset_name = PhysicalPrinter::get_preset_name(*printer.preset_names.begin());
                if (const Preset* preset = wxGetApp().preset_bundle->printers.find_preset(preset_name, false)) {
                    const PresetWithVendorProfile with_vendor = wxGetApp().preset_bundle->printers.get_preset_with_vendor_profile(*preset);
                    if (with_vendor.vendor)
                        brand = with_vendor.vendor->name;
                }
            }
            const bool has_host = PhysicalPrinter::has_print_host_information(printer.config);
            const bool selected = printer.name == wxGetApp().preset_bundle->physical_printers.get_selected_printer_name();
            const bool printing = offline_job_printing(printer.name);
            const char* status = printing ? "Printing" : (!has_host ? "Offline" : (selected ? "Ready" : "Idle"));
            std::string host;
            if (const auto* option = printer.config.option<ConfigOptionString>("print_host"))
                host = option->value;
            std::string job = offline_dashboard_text(printer.name);
            if (job.empty() && !has_host)
                job = into_u8(_L("No network connection. Export a file and choose Track it to show the estimate here."));
            if (!devices_json.empty())
                devices_json.push_back(',');
            devices_json += "{\"name\":\"" + json_escape(printer.name)
                + "\",\"brand\":\"" + json_escape(brand)
                + "\",\"status\":\"" + status
                + "\",\"host\":\"" + json_escape(host)
                + "\",\"offline\":" + (has_host ? "false" : "true")
                + ",\"job\":\"" + json_escape(job) + "\"}";
            device_names.push_back(printer.name);
        }
    }

    auto known_device = [&](const std::string& name) {
        return std::find(device_names.begin(), device_names.end(), name) != device_names.end();
    };

    std::vector<std::pair<std::string, std::string>> tiles;
    if (wxGetApp().app_config) {
        const std::string stored = wxGetApp().app_config->get("devices_dashboard");
        size_t cursor = 0;
        while (cursor < stored.size()) {
            const size_t end = stored.find_first_of("\n;", cursor);
            std::string line = stored.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
            cursor = end == std::string::npos ? stored.size() : end + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t colon = line.find(':');
            const size_t tab = line.find('\t');
            if (colon != std::string::npos && (tab == std::string::npos || colon < tab))
                line = line.substr(colon + 1);
            size_t cell_cursor = 0;
            while (cell_cursor <= line.size()) {
                const size_t pipe = line.find('|', cell_cursor);
                const std::string cell = line.substr(cell_cursor, pipe == std::string::npos ? std::string::npos : pipe - cell_cursor);
                cell_cursor = pipe == std::string::npos ? line.size() + 1 : pipe + 1;
                std::vector<std::string> parts;
                size_t part_at = 0;
                while (part_at <= cell.size()) {
                    const size_t tab = cell.find('\t', part_at);
                    parts.push_back(cell.substr(part_at, tab == std::string::npos ? std::string::npos : tab - part_at));
                    if (tab == std::string::npos)
                        break;
                    part_at = tab + 1;
                }
                if (parts.size() < 2)
                    continue;
                size_t index = 0;
                std::function<void()> walk = [&]() {
                    if (index + 1 >= parts.size())
                        return;
                    const std::string id = parts[index++];
                    const std::string key = parts[index++];
                    if (key == "vertical") {
                        if (index < parts.size())
                            ++index;
                        walk();
                        walk();
                        return;
                    }
                    if (!id.empty() && known_device(id))
                        tiles.emplace_back(id, key);
                };
                walk();
            }
        }
    }
    if (tiles.empty()) {
        const char* modules[] = { "now_printing", "temperatures", "camera" };
        for (const std::string& name : device_names)
            for (const char* module : modules)
                tiles.emplace_back(name, module);
    }

    std::string tiles_json;
    for (const auto& tile : tiles) {
        if (!tiles_json.empty())
            tiles_json.push_back(',');
        tiles_json += "{\"device\":\"" + json_escape(tile.first) + "\",\"module\":\"" + json_escape(tile.second) + "\"}";
    }

    const std::string now_printing = into_u8(_L("The current job shows up here."));
    const std::string temperatures = into_u8(_L("Nozzle —\nBed —"));
    const std::string printing_title = into_u8(_L("Now printing"));
    const std::string temperatures_title = into_u8(_L("Temperatures"));
    const std::string camera_title = into_u8(_L("Camera"));

    std::string json = "{\"devices\":[" + devices_json + "],\"tiles\":[" + tiles_json + "],\"modules\":{"
        "\"now_printing\":{\"title\":\"" + json_escape(printing_title) + "\",\"body\":\"" + json_escape(now_printing) + "\"},"
        "\"temperatures\":{\"title\":\"" + json_escape(temperatures_title) + "\",\"body\":\"" + json_escape(temperatures) + "\"},"
        "\"camera\":{\"title\":\"" + json_escape(camera_title) + "\",\"body\":\"\"}}}";
    DashboardSite::instance().set_snapshot(std::move(json));
}

}}
