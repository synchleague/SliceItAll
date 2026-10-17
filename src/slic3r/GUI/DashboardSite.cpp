///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "DashboardSite.hpp"

#include "DevicesPanel.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "FilamentRack.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "../Utils/Anycubic.hpp"

#ifdef _WIN32
#include <objidl.h>
#include <gdiplus.h>
#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#endif
#endif

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
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
.bays { display: flex; flex-direction: column; gap: 12px; margin-top: 8px; }
.bay-title { font-size: 12px; color: #c9c9d4; margin-bottom: 6px; }
.dry { color: #9b9ba8; font-size: 12px; margin: 0 0 6px; }
.spools { display: flex; flex-wrap: wrap; gap: 10px; }
.spool { width: 68px; text-align: center; }
.reel { height: 52px; display: flex; align-items: center; justify-content: center; }
.flange { width: 14px; height: 46px; border-radius: 8px; background: #babac0; }
.core { width: 30px; height: 32px; border-radius: 4px; margin: 0 -5px; z-index: 1; box-shadow: inset 0 0 0 6px rgba(16, 16, 20, 0.28); }
.spool-name { font-size: 12px; margin-top: 4px; word-break: break-word; }
.spool-name.empty { color: #8d8d98; }
.badge { width: 22px; height: 22px; margin: 4px auto 0; border-radius: 50%; background: #303038; font-size: 11px; line-height: 22px; }
.cam { position: relative; min-height: 160px; margin-top: 6px; border-radius: 10px; background: #101014; overflow: hidden; display: flex; align-items: center; justify-content: center; }
.cam img { width: 100%; height: auto; max-height: 280px; object-fit: contain; background: #101014; display: block; }
.cam-note { position: absolute; left: 12px; right: 12px; margin: 0; text-align: center; color: #c9c9d4; font-size: 13px; }
.cam-play { position: absolute; left: 8px; bottom: 8px; border: 0; border-radius: 999px; padding: 6px 12px; background: #FE32AD; color: white; font: inherit; cursor: pointer; }
.cam-play:disabled { background: #3a3a44; color: #9b9ba8; cursor: default; }
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
function findChild(parent, key) {
  for (const child of parent.children) {
    if (child.dataset && child.dataset.module === key)
      return child;
  }
  return null;
}
function spoolView(slot) {
  const spool = node("div", "spool");
  const reel = node("div", "reel");
  reel.append(node("span", "flange"));
  const core = node("span", "core");
  if (slot.color)
    core.style.background = slot.color;
  else
    core.style.background = slot.filled ? "#d6d6da" : "#3a3a42";
  reel.append(core);
  reel.append(node("span", "flange"));
  spool.append(reel);
  const name = slot.material || (slot.filled ? "" : "/");
  spool.append(node("div", "spool-name" + (slot.filled ? "" : " empty"), name || "/"));
  if (slot.badge)
    spool.append(node("div", "badge", slot.badge));
  return spool;
}
function fillMaterials(block, device) {
  const info = device && device.materials;
  if (!info)
    return;
  if (info.kind === "resin") {
    block.append(node("p", "", info.name || "Choose a resin in the material list."));
    block.append(node("p", "dry", "One resin sits in the vat. This printer has no filament spools."));
    return;
  }
  const bays = node("div", "bays");
  for (const bay of info.bays || []) {
    const group = node("div", "bay");
    if (bay.title)
      group.append(node("div", "bay-title", bay.title));
    if (bay.drying)
      group.append(node("p", "dry", bay.drying));
    const row = node("div", "spools");
    for (const slot of bay.slots || [])
      row.append(spoolView(slot));
    group.append(row);
    bays.append(group);
  }
  block.append(bays);
}
function pullFrame(block) {
  const img = block.querySelector("img");
  const note = block.querySelector(".cam-note");
  if (!img || block.dataset.on !== "1")
    return;
  fetch("/api/camera/frame?device=" + encodeURIComponent(block.dataset.camera), { cache: "no-store" })
    .then(async (response) => {
      if (block.dataset.on !== "1")
        return;
      if (response.status === 200) {
        const blob = await response.blob();
        if (!blob.size)
          return;
        const url = URL.createObjectURL(blob);
        const previous = img.dataset.url;
        img.src = url;
        img.hidden = false;
        img.dataset.url = url;
        if (previous)
          URL.revokeObjectURL(previous);
        if (note)
          note.textContent = "";
        return;
      }
      if (note && response.status !== 204)
        note.textContent = (await response.text()) || "The camera did not answer.";
    })
    .catch(() => {
      if (note && block.dataset.on === "1")
        note.textContent = "Waiting for Slice it All.";
    });
}
function stopPlayer(block) {
  if (block._timer) {
    clearInterval(block._timer);
    block._timer = 0;
  }
  const img = block.querySelector("img");
  if (img && img.dataset.url) {
    URL.revokeObjectURL(img.dataset.url);
    img.removeAttribute("src");
    delete img.dataset.url;
    img.hidden = true;
  }
}
function toggleCamera(block) {
  const button = block.querySelector("button");
  const note = block.querySelector(".cam-note");
  const start = block.dataset.on !== "1";
  if (button)
    button.disabled = true;
  if (note)
    note.textContent = start ? "Starting the camera." : "Stopping the camera.";
  fetch("/api/camera?device=" + encodeURIComponent(block.dataset.camera) + "&action=" + (start ? "start" : "stop"), { cache: "no-store" })
    .then(async (response) => {
      const data = await response.json();
      if (!response.ok || !data.ok) {
        if (note)
          note.textContent = (data && data.note) || "The camera did not answer.";
        if (button)
          button.disabled = false;
        return;
      }
      block.dataset.on = data.running ? "1" : "0";
      if (button) {
        button.textContent = data.running ? "Stop" : "Play";
        button.disabled = false;
      }
      if (data.running) {
        if (note)
          note.textContent = "Starting the camera. The first picture can take a few seconds.";
        if (block._timer)
          clearInterval(block._timer);
        pullFrame(block);
        block._timer = setInterval(() => pullFrame(block), 250);
      } else {
        stopPlayer(block);
        if (note)
          note.textContent = "Camera is stopped. Press Play to ask this printer for a picture.";
      }
    })
    .catch(() => {
      if (note)
        note.textContent = "Waiting for Slice it All.";
      if (button)
        button.disabled = false;
    });
}
function fillCamera(block, device) {
  if (block.dataset.ready === "1")
    return;
  block.dataset.ready = "1";
  block.dataset.camera = (device && device.name) || "";
  const stage = node("div", "cam");
  const img = document.createElement("img");
  img.alt = "";
  img.hidden = true;
  const note = node("p", "cam-note", "");
  const button = node("button", "cam-play", "Play");
  button.type = "button";
  const linked = !!(device && device.camera);
  if (!linked) {
    button.disabled = true;
    note.textContent = device && device.host
      ? "This connection does not provide a camera picture."
      : "No camera. A network connection is required before a picture can appear.";
  } else {
    note.textContent = "Camera is stopped. Press Play to ask this printer for a picture.";
  }
  button.addEventListener("click", () => toggleCamera(block));
  stage.append(img, note, button);
  block.append(stage);
}
function moduleBlock(card, modules, key, device) {
  const info = (modules && modules[key]) || { title: key, body: "" };
  let block = findChild(card, key);
  if (!block) {
    block = node("div", "module");
    block.dataset.module = key;
    card.append(block);
  }
  let title = block.querySelector("h3");
  const heading = (key === "material" && device && device.materials && device.materials.kind === "resin")
    ? "Resin"
    : (info.title || key);
  if (!title) {
    title = node("h3", "", heading);
    block.prepend(title);
  } else {
    title.textContent = heading;
  }
  if (key === "camera") {
    fillCamera(block, device);
    return;
  }
  for (const child of [...block.children]) {
    if (child !== title)
      child.remove();
  }
  if (key === "material") {
    fillMaterials(block, device);
    return;
  }
  const body = (key === "now_printing" && device && device.job) ? device.job : (info.body || "");
  if (body)
    block.append(node("p", "", body));
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
  const devices = data.devices || [];
  const tiles = data.tiles || [];
  const byName = new Map(devices.map(device => [device.name, device]));
  if (!tiles.length) {
    for (const card of [...board.children]) {
      if (card._timer)
        clearInterval(card._timer);
      for (const block of card.querySelectorAll ? card.querySelectorAll("[data-module]") : [])
        stopPlayer(block);
    }
    board.replaceChildren(node("p", "note", "No devices yet. Add printers in the configuration wizard and they show up here."));
    return;
  }
  const wanted = new Set();
  const modulesFor = new Map();
  for (const tile of tiles) {
    wanted.add(tile.device);
    if (!modulesFor.has(tile.device))
      modulesFor.set(tile.device, []);
    modulesFor.get(tile.device).push(tile.module);
  }
  for (const card of [...board.children]) {
    const name = card.dataset ? card.dataset.device : "";
    if (!name || !wanted.has(name)) {
      for (const block of card.querySelectorAll ? card.querySelectorAll("[data-module]") : [])
        stopPlayer(block);
      card.remove();
    }
  }
  for (const name of wanted) {
    const device = byName.get(name) || { name: name, brand: "", status: "Offline", host: "" };
    let card = null;
    for (const child of board.children) {
      if (child.dataset && child.dataset.device === name)
        card = child;
    }
    if (!card) {
      card = node("article");
      card.dataset.device = name;
      board.append(card);
    }
    let heading = card.querySelector("h2");
    if (!heading) {
      heading = node("h2", "", device.name || "Printer");
      card.prepend(heading);
    } else {
      heading.textContent = device.name || "Printer";
    }
    const bits = [];
    if (device.brand) bits.push(device.brand);
    if (device.host) bits.push(device.host);
    let meta = null;
    for (const child of card.children) {
      if (child.classList && child.classList.contains("meta"))
        meta = child;
    }
    if (bits.length) {
      if (!meta) {
        meta = node("div", "meta", bits.join("  ·  "));
        heading.after(meta);
      } else {
        meta.textContent = bits.join("  ·  ");
      }
    } else if (meta) {
      meta.remove();
    }
    let badge = card.querySelector(".status");
    const status = device.status || "Offline";
    if (!badge) {
      badge = node("div", "status", status);
      const anchor = meta || heading;
      anchor.after(badge);
    }
    badge.className = "status" + (known.includes(status) ? " " + status : "");
    badge.textContent = status;
    const keys = modulesFor.get(name) || [];
    for (const key of keys)
      moduleBlock(card, data.modules, key, device);
    for (const block of [...card.querySelectorAll("[data-module]")]) {
      if (!keys.includes(block.dataset.module)) {
        stopPlayer(block);
        block.remove();
      }
    }
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

struct HttpTarget
{
    std::string method;
    std::string path;
    std::string query;
};

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

std::string query_value(const std::string& query, const std::string& key)
{
    size_t cursor = 0;
    while (cursor < query.size()) {
        const size_t end = query.find('&', cursor);
        const std::string pair = query.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? query.size() : end + 1;
        const size_t eq = pair.find('=');
        const std::string name = url_decode(eq == std::string::npos ? pair : pair.substr(0, eq));
        if (name != key)
            continue;
        return url_decode(eq == std::string::npos ? std::string() : pair.substr(eq + 1));
    }
    return {};
}

HttpTarget request_target(const std::string& request)
{
    HttpTarget target;
    const size_t line_end = request.find("\r\n");
    const std::string line = request.substr(0, line_end);
    const size_t method_end = line.find(' ');
    if (method_end == std::string::npos)
        return target;
    target.method = line.substr(0, method_end);
    if (target.method != "GET")
        return target;
    const size_t path_begin = method_end + 1;
    const size_t path_end = line.find(' ', path_begin);
    if (path_end == std::string::npos)
        return target;
    std::string path = line.substr(path_begin, path_end - path_begin);
    const size_t query = path.find('?');
    if (query != std::string::npos) {
        target.query = path.substr(query + 1);
        path.resize(query);
    }
    target.path = std::move(path);
    return target;
}

std::string camera_json(bool ok, bool running, const std::string& note)
{
    return "{\"ok\":" + std::string(ok ? "true" : "false")
        + ",\"running\":" + std::string(running ? "true" : "false")
        + ",\"note\":\"" + json_escape(note) + "\"}";
}

#ifdef _WIN32
bool rgb_to_jpeg(const unsigned char* rgb, int width, int height, std::vector<unsigned char>& out)
{
    struct Session {
        ULONG_PTR token { 0 };
        Session()
        {
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&token, &input, nullptr);
        }
    };
    static Session session;
    if (session.token == 0 || width < 1 || height < 1 || rgb == nullptr)
        return false;

    Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData data;
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return false;
    const int stride = data.Stride;
    auto* origin = static_cast<unsigned char*>(data.Scan0);
    for (int y = 0; y < height; ++y) {
        unsigned char* dst = origin + (stride < 0 ? stride * (height - 1 - y) : stride * y);
        const unsigned char* src = rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 255;
        }
    }
    bitmap.UnlockBits(&data);

    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0)
        return false;
    std::vector<unsigned char> codecs(bytes);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(codecs.data());
    if (Gdiplus::GetImageEncoders(count, bytes, info) != Gdiplus::Ok)
        return false;
    CLSID clsid {};
    bool found = false;
    for (UINT i = 0; i < count; ++i) {
        if (info[i].MimeType && wcscmp(info[i].MimeType, L"image/jpeg") == 0) {
            clsid = info[i].Clsid;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK || stream == nullptr)
        return false;
    Gdiplus::EncoderParameters params;
    ULONG quality = 70;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &quality;
    const Gdiplus::Status saved = bitmap.Save(stream, &clsid, &params);
    bool wrote = false;
    if (saved == Gdiplus::Ok) {
        STATSTG stat {};
        if (stream->Stat(&stat, STATFLAG_NONAME) == S_OK && stat.cbSize.QuadPart > 0) {
            LARGE_INTEGER zero {};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            const ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);
            out.resize(size);
            ULONG read = 0;
            wrote = stream->Read(out.data(), size, &read) == S_OK && read > 0;
            if (wrote)
                out.resize(read);
        }
    }
    stream->Release();
    return wrote;
}
#else
bool rgb_to_jpeg(const unsigned char*, int, int, std::vector<unsigned char>&)
{
    return false;
}
#endif

bool slot_filled(const RackSlot& slot)
{
    return !slot.color.empty() || !slot.material.empty();
}

std::string slot_hex(const RackSlot& slot)
{
    int red = 0;
    int green = 0;
    int blue = 0;
    if (!rack_color_rgb(slot.color, red, green, blue))
        return {};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", red, green, blue);
    return buf;
}

std::string materials_json(const std::string& printer_name, bool resin)
{
    if (resin) {
        std::string name;
        if (wxGetApp().preset_bundle) {
            const Preset& selected = wxGetApp().preset_bundle->sla_materials.get_selected_preset();
            if (!selected.name.empty() && !selected.is_default)
                name = selected.name;
        }
        return "{\"kind\":\"resin\",\"name\":\"" + json_escape(name) + "\"}";
    }

    FilamentRack rack = load_filament_rack(printer_name);
    apply_remembered_ace(printer_name, rack);
    std::string bays;
    for (const MaterialBay& bay : rack.bays) {
        if (bay.key == "ace2") {
            bool any = false;
            for (const RackSlot& slot : bay.slots)
                any = any || slot_filled(slot);
            if (!any)
                continue;
        }
        std::string drying;
        if (bay.drying) {
            drying = "Drying";
            if (!bay.dry_material.empty())
                drying += " " + bay.dry_material;
            if (bay.dry_celsius >= 0)
                drying += " at " + std::to_string(bay.dry_celsius) + " C";
        }
        std::string slots;
        for (size_t i = 0; i < bay.slots.size(); ++i) {
            const RackSlot& slot = bay.slots[i];
            const bool filled = slot_filled(slot);
            const std::string badge = rack.multi_color_unit
                ? std::to_string(i + 1)
                : "E" + std::to_string(i + 1);
            if (!slots.empty())
                slots.push_back(',');
            slots += "{\"material\":\"" + json_escape(slot.material)
                + "\",\"color\":\"" + json_escape(slot_hex(slot))
                + "\",\"badge\":\"" + json_escape(badge)
                + "\",\"filled\":" + (filled ? "true" : "false") + "}";
        }
        if (!bays.empty())
            bays.push_back(',');
        bays += "{\"title\":\"" + json_escape(bay.title)
            + "\",\"drying\":\"" + json_escape(drying)
            + "\",\"slots\":[" + slots + "]}";
    }
    return "{\"kind\":\"spools\",\"bays\":[" + bays + "]}";
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

struct LiveCamera
{
    std::mutex mutex;
    std::string host;
    bool running { false };
    std::string note;
    std::shared_ptr<AnycubicPreview> preview;
    int serial { 0 };
    std::vector<unsigned char> jpeg;
};

struct DashboardSite::Impl
{
    std::mutex life;
    std::mutex snapshot_mutex;
    std::mutex clients_mutex;
    std::mutex cameras_mutex;
    std::condition_variable clients_done;
    std::string json = "{\"devices\":[],\"tiles\":[],\"modules\":{}}";
    std::map<std::string, DashboardSite::PrinterLink> printers;
    std::map<std::string, std::shared_ptr<LiveCamera>> cameras;
    boost::asio::io_context io;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor;
    std::thread thread;
    std::vector<std::shared_ptr<boost::asio::ip::tcp::socket>> sockets;
    int active_clients = 0;
    unsigned short port = 0;
    bool running = false;

    void arm_accept();
    void serve(boost::asio::ip::tcp::socket& socket);
    std::string camera_action(const std::string& name, const std::string& action);
    std::string camera_frame(const std::string& name, std::string& type, int& status, std::string& reason);
    void stop_cameras();
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

bool drive_camera(LiveCamera& camera, bool start, std::string& error)
{
    DynamicPrintConfig config;
    config.set_key_value("print_host", new ConfigOptionString(camera.host));
    Anycubic printer(&config);
    if (!printer.set_camera_capture(start, error))
        return false;
    if (start) {
        std::string light_error;
        printer.set_chamber_light(true, light_error);
        camera.preview = std::make_shared<AnycubicPreview>(camera.host);
        camera.preview->start();
        camera.running = true;
        camera.serial = 0;
        camera.jpeg.clear();
        camera.note.clear();
    } else {
        camera.preview.reset();
        camera.running = false;
        camera.serial = 0;
        camera.jpeg.clear();
        std::string light_error;
        printer.set_chamber_light(false, light_error);
        camera.note.clear();
    }
    return true;
}

std::string DashboardSite::Impl::camera_action(const std::string& name, const std::string& action)
{
    DashboardSite::PrinterLink link;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex);
        const auto found = printers.find(name);
        if (found == printers.end())
            return camera_json(false, false, "This printer is not on the dashboard.");
        link = found->second;
    }
    if (!link.camera || link.host.empty()) {
        const std::string note = link.host.empty()
            ? "No camera. A network connection is required before a picture can appear."
            : "This connection does not provide a camera picture.";
        return camera_json(false, false, note);
    }

    std::shared_ptr<LiveCamera> camera;
    {
        std::lock_guard<std::mutex> lock(cameras_mutex);
        auto found = cameras.find(name);
        if (found == cameras.end()) {
            camera = std::make_shared<LiveCamera>();
            cameras.emplace(name, camera);
        } else {
            camera = found->second;
        }
    }

    const bool start = action != "stop";
    std::lock_guard<std::mutex> lock(camera->mutex);
    camera->host = link.host;
    if (start == camera->running && (start || !camera->preview))
        return camera_json(true, camera->running, camera->note);
    std::string error;
    try {
        if (!drive_camera(*camera, start, error))
            return camera_json(false, camera->running, error.empty() ? "The camera did not answer." : error);
    } catch (const std::exception& ex) {
        return camera_json(false, camera->running, ex.what());
    }
    return camera_json(true, camera->running, camera->note);
}

std::string DashboardSite::Impl::camera_frame(const std::string& name, std::string& type, int& status, std::string& reason)
{
    std::shared_ptr<LiveCamera> camera;
    {
        std::lock_guard<std::mutex> lock(cameras_mutex);
        const auto found = cameras.find(name);
        if (found != cameras.end())
            camera = found->second;
    }
    if (!camera) {
        type = "text/plain; charset=utf-8";
        status = 404;
        reason = "Not Found";
        return "Camera is stopped.\n";
    }
    std::lock_guard<std::mutex> lock(camera->mutex);
    if (!camera->running || !camera->preview) {
        type = "text/plain; charset=utf-8";
        status = 404;
        reason = "Not Found";
        return "Camera is stopped.\n";
    }
    AnycubicPicture picture;
    picture.serial = camera->serial;
    if (camera->preview->copy_picture(picture)
        && picture.width > 0
        && picture.rgb.size() >= static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 3) {
        std::vector<unsigned char> jpeg;
        if (rgb_to_jpeg(picture.rgb.data(), picture.width, picture.height, jpeg)) {
            camera->jpeg.assign(jpeg.begin(), jpeg.end());
            camera->serial = picture.serial;
        }
    }
    if (!camera->jpeg.empty()) {
        type = "image/jpeg";
        status = 200;
        reason = "OK";
        return std::string(reinterpret_cast<const char*>(camera->jpeg.data()), camera->jpeg.size());
    }
    const std::string error = camera->preview->last_error();
    if (!error.empty()) {
        type = "text/plain; charset=utf-8";
        status = 503;
        reason = "Service Unavailable";
        return error;
    }
    type = "text/plain; charset=utf-8";
    status = 204;
    reason = "No Content";
    return {};
}

void DashboardSite::Impl::stop_cameras()
{
    std::map<std::string, std::shared_ptr<LiveCamera>> open;
    {
        std::lock_guard<std::mutex> lock(cameras_mutex);
        open.swap(cameras);
    }
    for (auto& item : open) {
        std::lock_guard<std::mutex> lock(item.second->mutex);
        if (!item.second->running && !item.second->preview)
            continue;
        std::string error;
        try {
            drive_camera(*item.second, false, error);
        } catch (const std::exception&) {
        }
    }
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
    const HttpTarget target = request_target(request);
    std::string response;
    if (target.method.empty() && !request.empty())
        response = http_response("text/plain; charset=utf-8", "Method not allowed\n", 405, "Method Not Allowed");
    else if (target.method != "GET" && !target.method.empty())
        response = http_response("text/plain; charset=utf-8", "Method not allowed\n", 405, "Method Not Allowed");
    else if (target.path == "/" || target.path == "/index.html")
        response = http_response("text/html; charset=utf-8", page_html(), 200, "OK");
    else if (target.path == "/api/dashboard") {
        std::string body;
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex);
            body = json;
        }
        response = http_response("application/json; charset=utf-8", body, 200, "OK");
    } else if (target.path == "/api/camera") {
        const std::string name = query_value(target.query, "device");
        const std::string action = query_value(target.query, "action");
        response = http_response("application/json; charset=utf-8", camera_action(name, action), 200, "OK");
    } else if (target.path == "/api/camera/frame") {
        std::string type;
        int status = 404;
        std::string reason = "Not Found";
        const std::string body = camera_frame(query_value(target.query, "device"), type, status, reason);
        response = http_response(type, body, status, reason.c_str());
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
    m_impl->stop_cameras();
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

void DashboardSite::set_snapshot(std::string json, std::vector<PrinterLink> printers)
{
    std::map<std::string, PrinterLink> links;
    for (PrinterLink& printer : printers) {
        const std::string name = printer.name;
        links.emplace(name, std::move(printer));
    }
    std::lock_guard<std::mutex> lock(m_impl->snapshot_mutex);
    m_impl->json = std::move(json);
    m_impl->printers = std::move(links);
}

void publish_dashboard_snapshot()
{
    refresh_offline_jobs(false);
    std::string devices_json;
    std::vector<std::string> device_names;
    std::vector<DashboardSite::PrinterLink> links;
    const std::vector<DeviceEntry> catalog = bound_devices();
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
            const DeviceEntry* entry = nullptr;
            for (const DeviceEntry& candidate : catalog) {
                if (candidate.id == printer.name) {
                    entry = &candidate;
                    break;
                }
            }
            const bool resin = entry && entry->resin;
            const bool camera = entry && entry->link == "Anycubic LAN" && !host.empty();
            if (!devices_json.empty())
                devices_json.push_back(',');
            devices_json += "{\"name\":\"" + json_escape(printer.name)
                + "\",\"brand\":\"" + json_escape(brand)
                + "\",\"status\":\"" + status
                + "\",\"host\":\"" + json_escape(host)
                + "\",\"offline\":" + (has_host ? "false" : "true")
                + ",\"camera\":" + (camera ? "true" : "false")
                + ",\"job\":\"" + json_escape(job)
                + "\",\"materials\":" + materials_json(printer.name, resin) + "}";
            device_names.push_back(printer.name);
            DashboardSite::PrinterLink link;
            link.name = printer.name;
            link.host = host;
            link.camera = camera;
            links.push_back(std::move(link));
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
        const char* modules[] = { "now_printing", "material", "temperatures", "camera" };
        for (const std::string& name : device_names)
            for (const char* module : modules)
                tiles.emplace_back(name, module);
    }
    for (const std::string& name : device_names) {
        bool has_material = false;
        for (const auto& tile : tiles)
            has_material = has_material || (tile.first == name && tile.second == "material");
        if (!has_material)
            tiles.emplace_back(name, "material");
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
    const std::string material_title = into_u8(_L("Material"));

    std::string json = "{\"devices\":[" + devices_json + "],\"tiles\":[" + tiles_json + "],\"modules\":{"
        "\"now_printing\":{\"title\":\"" + json_escape(printing_title) + "\",\"body\":\"" + json_escape(now_printing) + "\"},"
        "\"material\":{\"title\":\"" + json_escape(material_title) + "\",\"body\":\"\"},"
        "\"temperatures\":{\"title\":\"" + json_escape(temperatures_title) + "\",\"body\":\"" + json_escape(temperatures) + "\"},"
        "\"camera\":{\"title\":\"" + json_escape(camera_title) + "\",\"body\":\"\"}}}";
    DashboardSite::instance().set_snapshot(std::move(json), std::move(links));
}

}}
