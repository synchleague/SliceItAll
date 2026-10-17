///|/ Slice it All
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
#include "EditorPlugin.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "Plater.hpp"
#include "WebView.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/webview.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r::GUI {
namespace {

std::string path_utf8(const boost::filesystem::path& path);

std::vector<EditorPlugin> g_plugins;

// The tool pages run on https and only accept a model through a file drop, a file
// input, or (Support Fins) a ?stl= URL. A loopback server lets the page fetch the
// mesh and post the export back, with a token so nothing else on the machine can
// read it. CORS and the private-network header are what those fetches require.
class LoopbackBridge
{
public:
    using ResultHandler = std::function<void(boost::filesystem::path, std::string)>;

    ~LoopbackBridge() { stop(); }

    bool start(std::string token, ResultHandler on_result)
    {
        stop();
        m_token = std::move(token);
        m_on_result = std::move(on_result);
        m_model = std::make_shared<const std::string>();

        boost::system::error_code ec;
        m_acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(m_io);
        m_acceptor->open(boost::asio::ip::tcp::v4(), ec);
        if (!ec)
            m_acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (!ec)
            m_acceptor->bind(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0), ec);
        if (!ec)
            m_acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            m_acceptor.reset();
            return false;
        }
        m_port = m_acceptor->local_endpoint().port();
        m_running.store(true);
        m_thread = std::thread([this] { serve_loop(); });
        return true;
    }

    void stop()
    {
        if (!m_running.exchange(false) && !m_thread.joinable())
            return;
        boost::system::error_code ec;
        if (m_acceptor)
            m_acceptor->close(ec);
        {
            std::lock_guard<std::mutex> lock(m_socket_mutex);
            if (m_socket)
                m_socket->close(ec);
        }
        if (m_thread.joinable())
            m_thread.join();
        m_acceptor.reset();
        m_port = 0;
    }

    bool running() const { return m_running.load() && m_port != 0; }
    unsigned short port() const { return m_port; }

    void set_model(std::string bytes)
    {
        auto next = std::make_shared<const std::string>(std::move(bytes));
        std::lock_guard<std::mutex> lock(m_model_mutex);
        m_model = std::move(next);
    }

    std::string model_url() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port) + "/t/" + m_token + "/model.stl";
    }

    std::string result_url() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port) + "/t/" + m_token + "/result?name=";
    }

private:
    void serve_loop()
    {
        while (m_running.load()) {
            boost::system::error_code ec;
            auto socket = std::make_shared<boost::asio::ip::tcp::socket>(m_io);
            m_acceptor->accept(*socket, ec);
            if (ec || !m_running.load())
                break;
            {
                std::lock_guard<std::mutex> lock(m_socket_mutex);
                m_socket = socket;
            }
            try {
                serve(*socket);
            } catch (...) {
            }
            socket->close(ec);
            {
                std::lock_guard<std::mutex> lock(m_socket_mutex);
                if (m_socket == socket)
                    m_socket.reset();
            }
        }
    }

    void serve(boost::asio::ip::tcp::socket& socket)
    {
        set_timeout(socket);
        std::string request;
        char buffer[4096];
        boost::system::error_code ec;
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
            const size_t count = socket.read_some(boost::asio::buffer(buffer), ec);
            if (ec || count == 0)
                break;
            request.append(buffer, count);
        }
        const size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            reply(socket, 400, "text/plain", "Bad request\n", {});
            return;
        }
        const std::string headers = request.substr(0, header_end);
        std::string body = request.substr(header_end + 4);
        const std::string origin = header_value(headers, "Origin");

        std::string method;
        std::string target;
        {
            const size_t line_end = headers.find("\r\n");
            const std::string line = headers.substr(0, line_end == std::string::npos ? headers.size() : line_end);
            const size_t sp1 = line.find(' ');
            const size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                reply(socket, 400, "text/plain", "Bad request\n", origin);
                return;
            }
            method = line.substr(0, sp1);
            target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        }

        const size_t query_at = target.find('?');
        const std::string path = query_at == std::string::npos ? target : target.substr(0, query_at);
        const std::string query = query_at == std::string::npos ? std::string{} : target.substr(query_at + 1);
        const std::string model_path = "/t/" + m_token + "/model.stl";
        const std::string result_path = "/t/" + m_token + "/result";

        if (method == "OPTIONS") {
            reply(socket, 204, "text/plain", {}, origin, header_value(headers, "Access-Control-Request-Headers"));
            return;
        }
        if (method == "GET" && path == model_path) {
            std::shared_ptr<const std::string> model;
            {
                std::lock_guard<std::mutex> lock(m_model_mutex);
                model = m_model;
            }
            if (!model || model->empty()) {
                reply(socket, 404, "text/plain", "No model\n", origin);
                return;
            }
            reply(socket, 200, "application/octet-stream", *model, origin);
            return;
        }
        if (method == "POST" && path == result_path) {
            const std::string length_text = header_value(headers, "Content-Length");
            if (length_text.empty()) {
                reply(socket, 411, "text/plain", "Length required\n", origin);
                return;
            }
            char* end = nullptr;
            const unsigned long long length = std::strtoull(length_text.c_str(), &end, 10);
            constexpr unsigned long long k_max = 512ull * 1024ull * 1024ull;
            if (end == length_text.c_str() || length == 0 || length > k_max) {
                reply(socket, 413, "text/plain", "Payload too large\n", origin);
                return;
            }
            body.resize(static_cast<size_t>(length));
            size_t have = std::min(body.size(), request.size() - (header_end + 4));
            while (have < body.size()) {
                const size_t count = socket.read_some(boost::asio::buffer(body.data() + have, body.size() - have), ec);
                if (ec || count == 0)
                    break;
                have += count;
            }
            if (have != body.size()) {
                reply(socket, 400, "text/plain", "Short body\n", origin);
                return;
            }
            const std::string filename = safe_download_name(query_value(query, "name"));
            const boost::filesystem::path path_on_disk = boost::filesystem::temp_directory_path()
                / ("sliceitall-" + m_token + "-" + std::to_string(m_seq.fetch_add(1)) + "-" + filename);
            {
                boost::nowide::ofstream out(path_utf8(path_on_disk).c_str(), std::ios::binary);
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                out.close();
                if (!out) {
                    reply(socket, 500, "text/plain", "Could not store the result\n", origin);
                    return;
                }
            }
            reply(socket, 204, "text/plain", {}, origin);
            if (m_on_result)
                m_on_result(path_on_disk, filename);
            return;
        }
        reply(socket, 404, "text/plain", "Not found\n", origin);
    }

    static void set_timeout(boost::asio::ip::tcp::socket& socket)
    {
#ifdef _WIN32
        const DWORD milliseconds = 120000;
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
#else
        struct timeval timeout;
        timeout.tv_sec = 120;
        timeout.tv_usec = 0;
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }

    static void reply(boost::asio::ip::tcp::socket& socket, int status, const char* content_type,
        const std::string& body, const std::string& origin, const std::string& allow_headers = {})
    {
        const char* reason = "OK";
        switch (status) {
        case 204: reason = "No Content"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 411: reason = "Length Required"; break;
        case 413: reason = "Payload Too Large"; break;
        case 500: reason = "Internal Server Error"; break;
        default: break;
        }
        std::string allow = allow_headers.empty() ? "Content-Type" : allow_headers;
        if (allow.find('\r') != std::string::npos || allow.find('\n') != std::string::npos)
            allow = "Content-Type";
        std::string allow_origin = "*";
        if (!origin.empty() && origin.find('\r') == std::string::npos && origin.find('\n') == std::string::npos
            && (origin.rfind("https://", 0) == 0 || origin.rfind("http://", 0) == 0))
            allow_origin = origin;
        std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Cache-Control: no-store\r\n"
            "Access-Control-Allow-Origin: " + allow_origin + "\r\n"
            "Vary: Origin\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: " + allow + "\r\n"
            "Access-Control-Allow-Private-Network: true\r\n"
            "Access-Control-Max-Age: 600\r\n"
            "Connection: close\r\n\r\n";
        boost::system::error_code ec;
        boost::asio::write(socket, boost::asio::buffer(head), ec);
        if (!ec && !body.empty())
            boost::asio::write(socket, boost::asio::buffer(body.data(), body.size()), ec);
    }

    static std::string header_value(const std::string& headers, const char* key)
    {
        const std::string needle = std::string(key) + ":";
        size_t at = 0;
        while (at < headers.size()) {
            const size_t line_end = headers.find("\r\n", at);
            const std::string line = headers.substr(at, line_end == std::string::npos ? std::string::npos : line_end - at);
            if (line.size() >= needle.size() && std::equal(needle.begin(), needle.end(), line.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                })) {
                size_t value = needle.size();
                while (value < line.size() && line[value] == ' ')
                    ++value;
                return line.substr(value);
            }
            if (line_end == std::string::npos)
                break;
            at = line_end + 2;
        }
        return {};
    }

    static int hex_value(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static std::string percent_decode(const std::string& text)
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

    static std::string query_value(const std::string& query, const char* key)
    {
        const std::string needle = std::string(key) + "=";
        size_t at = 0;
        while (at < query.size()) {
            const size_t amp = query.find('&', at);
            const std::string pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
            if (pair.rfind(needle, 0) == 0)
                return percent_decode(pair.substr(needle.size()));
            if (amp == std::string::npos)
                break;
            at = amp + 1;
        }
        return {};
    }

    static std::string safe_download_name(std::string name)
    {
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        std::string ext;
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) {
            ext = name.substr(dot);
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            name.resize(dot);
        }
        if (ext != ".stl" && ext != ".obj" && ext != ".3mf")
            ext = ".stl";
        std::string stem;
        for (unsigned char c : name) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
                stem.push_back(static_cast<char>(c));
            else if (c == ' ')
                stem.push_back('_');
            if (stem.size() >= 80)
                break;
        }
        if (stem.empty())
            stem = "result";
        return stem + ext;
    }

    boost::asio::io_context m_io;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> m_acceptor;
    std::shared_ptr<boost::asio::ip::tcp::socket> m_socket;
    std::mutex m_socket_mutex;
    std::mutex m_model_mutex;
    std::shared_ptr<const std::string> m_model;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<int> m_seq{ 0 };
    ResultHandler m_on_result;
    std::string m_token;
    unsigned short m_port{ 0 };
};

struct PlateExport
{
    TriangleMesh mesh;
    std::set<ObjectID> object_ids;
    size_t object_count{ 0 };
    bool from_selection{ false };
    bool partial{ false };
};

struct PrinterContext
{
    bool has_bed{ false };
    int bed_x{ 0 };
    int bed_y{ 0 };
    int bed_z{ 0 };
    double nozzle{ 0 };
    double layer_height{ 0 };
    double support_gap{ 0 };
};

std::string path_utf8(const boost::filesystem::path& path)
{
#ifdef _WIN32
    return boost::nowide::narrow(path.wstring());
#else
    return path.string();
#endif
}

std::string read_binary(const boost::filesystem::path& path)
{
    boost::nowide::ifstream in(path_utf8(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string make_token()
{
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    static const char* hex = "0123456789abcdef";
    std::string out(32, '0');
    for (char& c : out)
        c = hex[dist(rd)];
    return out;
}

std::string percent_encode(const std::string& text)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string format_decimal(double value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", value);
    for (char* p = buf; *p; ++p)
        if (*p == ',')
            *p = '.';
    return buf;
}

std::string fill_script(std::string script, const std::string& key, const std::string& value)
{
    const std::string token = "%%" + key + "%%";
    for (size_t at = 0; (at = script.find(token, at)) != std::string::npos; at += value.size())
        script.replace(at, token.size(), value);
    return script;
}

PlateExport export_plate_mesh()
{
    PlateExport out;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return out;
    GLCanvas3D* canvas = plater->canvas3D();
    if (canvas == nullptr)
        return out;
    const Selection& selection = canvas->get_selection();
    const std::set<unsigned int> selected = selection.is_empty() ? std::set<unsigned int>{} : selection.get_object_idxs();
    out.from_selection = !selected.empty();
    const int selected_instance = selection.is_from_single_instance() ? selection.get_instance_idx() : -1;
    const Model& model = plater->model();

    for (unsigned int object_idx = 0; object_idx < model.objects.size(); ++object_idx) {
        if (!selected.empty() && selected.count(object_idx) == 0)
            continue;
        const ModelObject* object = model.objects[object_idx];
        if (object == nullptr)
            continue;
        int printable = 0;
        int exported = 0;
        for (const ModelVolume* volume : object->volumes) {
            if (volume == nullptr || !volume->is_model_part())
                continue;
            for (int instance_idx = 0; instance_idx < int(object->instances.size()); ++instance_idx) {
                const ModelInstance* instance = object->instances[instance_idx];
                if (instance == nullptr || !instance->printable)
                    continue;
                ++printable;
                if (selected_instance >= 0 && instance_idx != selected_instance)
                    continue;
                ++exported;
                TriangleMesh part = volume->mesh();
                part.transform(volume->get_matrix(), true);
                part.transform(instance->get_matrix(), true);
                out.mesh.merge(part);
            }
        }
        if (exported == 0)
            continue;
        ++out.object_count;
        if (exported == printable)
            out.object_ids.insert(object->id());
        else
            out.partial = true;
    }
    if (out.partial)
        out.object_ids.clear();
    return out;
}

PrinterContext printer_context()
{
    PrinterContext ctx;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return ctx;
    const BuildVolume& volume = plater->build_volume();
    if (volume.valid()) {
        const Vec3d size = volume.bounding_volume().size();
        if (size.x() >= 1.0 && size.y() >= 1.0 && volume.max_print_height() >= 1.0) {
            ctx.has_bed = true;
            ctx.bed_x = std::max(1, (int)std::lround(size.x()));
            ctx.bed_y = std::max(1, (int)std::lround(size.y()));
            ctx.bed_z = std::max(1, (int)std::lround(volume.max_print_height()));
        }
    }
    const DynamicPrintConfig* config = plater->config();
    if (config == nullptr)
        return ctx;
    if (config->has("nozzle_diameter")) {
        const auto* nozzles = config->opt<ConfigOptionFloats>("nozzle_diameter");
        if (nozzles != nullptr && !nozzles->values.empty() && nozzles->values.front() > 0.0)
            ctx.nozzle = nozzles->values.front();
    }
    if (config->has("layer_height")) {
        const double layer = config->opt_float("layer_height");
        if (layer > 0.0)
            ctx.layer_height = layer;
    }
    if (config->has("support_material_contact_distance")) {
        const double gap = config->opt_float("support_material_contact_distance");
        if (gap >= 0.1 && gap <= 0.4)
            ctx.support_gap = gap;
    }
    return ctx;
}

std::string script_result(wxWebView* browser, const std::string& js)
{
    if (browser == nullptr)
        return {};
    wxString output;
    if (!browser->RunScript(wxString::FromUTF8(js), &output))
        return {};
    std::string text = into_u8(output);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    text = text.substr(start);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    if (text == "null" || text == "undefined")
        return {};
    return text;
}

struct PluginSession
{
    std::atomic<bool> open{ true };
    class WebAppPluginDialog* dialog{ nullptr };
};

std::map<std::string, class WebAppPluginDialog*> g_open_dialogs;

class WebAppPluginDialog : public wxDialog
{
public:
    WebAppPluginDialog(wxWindow* parent, const EditorPlugin& plugin)
        : wxDialog(parent, wxID_ANY, from_u8(plugin.title), wxDefaultPosition, wxSize(1100, 780),
            wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
        , m_plugin(plugin)
        , m_session(std::make_shared<PluginSession>())
    {
        m_session->dialog = this;
#ifdef _WIN32
        wxGetApp().UpdateDarkUI(this);
#endif
        m_plate = export_plate_mesh();
        m_printer = printer_context();
        m_token = make_token();
        const bool bridge_ok = m_bridge.start(m_token, [session = m_session](boost::filesystem::path path, std::string name) {
            wxGetApp().CallAfter([session, path = std::move(path), name = std::move(name)] {
                if (!session->open || session->dialog == nullptr)
                    return;
                session->dialog->accept_result(path, name);
            });
        });
        publish_model(m_plate);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* header = new wxBoxSizer(wxHORIZONTAL);
        auto* credit = new wxStaticText(this, wxID_ANY, format_wxstr(_L("%1% by %2%"), from_u8(plugin.title), from_u8(plugin.credit)));
        credit->SetFont(wxGetApp().bold_font());
        header->Add(credit, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

        auto* send = new wxButton(this, wxID_ANY, _L("Send model"));
        send->SetToolTip(_L("Send the current selection again. With nothing selected, every printable model is sent."));
        send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_model(); });
        auto* import_btn = new wxButton(this, wxID_ANY, _L("Import result"));
        import_btn->SetToolTip(_L("Export from the page and put that mesh on the build plate, replacing the model that was sent."));
        import_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { import_result(); });
        header->AddStretchSpacer(1);
        header->Add(send, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        header->Add(import_btn, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(header, 0, wxEXPAND | wxALL, 8);

        m_status = new wxStaticText(this, wxID_ANY, _L("Loading the tool..."));
        sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        const wxString start_url = page_url();
        m_browser = WebView::webview_new();
        if (m_browser == nullptr || start_url.empty()) {
            sizer->Add(new wxStaticText(this, wxID_ANY, format_wxstr(
                _L("Failed to load a web browser. Open %1% in a browser instead."), from_u8(plugin.url))), 0, wxALL, 8);
        } else {
            WebView::webview_create(m_browser, this, start_url, {});
            m_browser->EnableContextMenu(true);
            sizer->Add(m_browser, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
            Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent&) {
                m_hooked = false;
                m_ticks = 0;
                if (!m_timer.IsRunning())
                    m_timer.Start(400);
            }, m_browser->GetId());
            Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& evt) {
                if (m_browser != nullptr)
                    m_browser->LoadURL(evt.GetURL());
                evt.Veto();
            }, m_browser->GetId());
            Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent&) {
                note(_L("The tool page did not load. Check the connection and try again."));
            }, m_browser->GetId());
            m_timer.SetOwner(this);
            Bind(wxEVT_TIMER, [this](wxTimerEvent&) { handshake(); }, m_timer.GetId());
            m_timer.Start(400);
        }
        if (!bridge_ok)
            note(_L("The page will open, but the model could not be handed over automatically."), true);

        g_open_dialogs[m_plugin.id] = this;
        SetSizer(sizer);
        SetMinSize(wxSize(720, 520));
        CentreOnParent();
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& evt) {
            shutdown();
            evt.Skip();
        });
    }

    ~WebAppPluginDialog() override { shutdown(); }

    void accept_result(const boost::filesystem::path& path, const std::string& filename)
    {
        m_waiting_for_result = false;
        const bool replace = filename.find("fins-only") == std::string::npos && !m_plate.object_ids.empty();
        if (place_on_plate(path, replace))
            note(replace
                ? _L("Result is on the build plate and replaced the model that was sent.")
                : _L("Result is on the build plate."), true);
    }

private:
    bool wants_bed() const { return m_plugin.id == "printfins" && m_printer.has_bed; }
    bool page_loads_query() const { return m_plugin.id == "printfins"; }

    wxString page_url() const
    {
        if (m_plugin.url.empty())
            return {};
        if (!page_loads_query() || !m_bridge.running() || m_plate.mesh.empty())
            return from_u8(m_plugin.url);
        const std::string stl = percent_encode(m_bridge.model_url());
        return from_u8(m_plugin.url + "?stl=" + stl);
    }

    void publish_model(const PlateExport& plate)
    {
        m_plate = plate;
        m_disk_stl.clear();
        if (plate.mesh.empty()) {
            m_bridge.set_model({});
            return;
        }
        m_disk_stl = boost::filesystem::temp_directory_path() / ("sliceitall-" + m_plugin.id + ".stl");
        if (!its_write_stl_binary(path_utf8(m_disk_stl).c_str(), "sliceitall", plate.mesh.its)) {
            m_disk_stl.clear();
            m_bridge.set_model({});
            return;
        }
        m_bridge.set_model(read_binary(m_disk_stl));
    }

    void shutdown()
    {
        if (m_shutdown)
            return;
        m_shutdown = true;
        m_timer.Stop();
        const auto it = g_open_dialogs.find(m_plugin.id);
        if (it != g_open_dialogs.end() && it->second == this)
            g_open_dialogs.erase(it);
        if (m_session) {
            m_session->open = false;
            m_session->dialog = nullptr;
        }
        m_bridge.stop();
    }

    void note(const wxString& text, bool lock = false)
    {
        if (lock)
            m_status_locked = true;
        if (m_status == nullptr || text == m_status_text)
            return;
        m_status_text = text;
        m_status->SetLabel(text);
        m_status->SetToolTip(text);
    }

    void handshake()
    {
        if (m_browser == nullptr || m_shutdown)
            return;
        ++m_ticks;
        if (!m_hooked)
            m_hooked = script_result(m_browser, hook_js()) == "hooked";
        if (wants_bed() && !m_bed_sent)
            m_bed_sent = script_result(m_browser, bed_js()) == "bed";
        if (m_bridge.running() && !m_plate.mesh.empty() && !m_model_sent && !m_model_failed) {
            if (!m_model_loading) {
                if (m_force_ingest)
                    begin_ingest(true);
                else if (page_loads_query()) {
                    const std::string probe = script_result(m_browser, probe_js());
                    if (probe == "sent")
                        m_model_sent = true;
                    else if (probe == "alive" && ++m_alive_ticks >= 20)
                        begin_ingest(false);
                } else
                    begin_ingest(false);
            } else {
                const std::string poll = script_result(m_browser, "(()=>window.__sliceitall_model||'idle')()");
                if (poll == "sent") {
                    m_model_sent = true;
                    m_model_loading = false;
                } else if (poll == "error") {
                    m_model_failed = true;
                    m_model_loading = false;
                }
            }
        }
        if (!m_status_locked && !m_waiting_for_result)
            update_status();
        const bool model_settled = m_plate.mesh.empty() || m_model_sent || m_model_failed || !m_bridge.running();
        const bool bed_settled = !wants_bed() || m_bed_sent;
        if ((m_hooked && bed_settled && model_settled) || m_ticks > 220 || (m_ticks > 160 && !m_model_loading))
            m_timer.Stop();
        if (m_ticks == 160 && !m_status_locked && !m_model_sent && !m_bed_sent && !m_model_failed && !m_model_loading)
            note(_L("The tool page did not finish loading. Use Send model once it is open."), true);
    }

    void begin_ingest(bool force)
    {
        const std::string result = script_result(m_browser, ingest_js(force));
        if (result == "sent") {
            m_model_sent = true;
            m_model_loading = false;
            m_force_ingest = false;
        } else if (result == "started") {
            m_model_loading = true;
            m_force_ingest = false;
        }
    }

    void update_status()
    {
        if (m_status_locked)
            return;
        if (m_model_failed && !m_disk_stl.empty()) {
            note(format_wxstr(_L("The page did not take the model. Drop this file onto it: %1%"), from_path(m_disk_stl)));
            return;
        }
        if (m_model_loading || (m_bridge.running() && !m_plate.mesh.empty() && !m_model_sent && !m_model_failed)) {
            note(m_plate.object_count > 1
                ? _L("Sending the models to the page...")
                : _L("Sending the model to the page..."));
            return;
        }
        if (m_model_sent || m_bed_sent) {
            wxString text;
            if (m_model_sent && m_plate.partial)
                text = _L("Sent the selected instance. The result will be added next to it.");
            else if (m_model_sent && m_plate.object_count > 1)
                text = format_wxstr(_L("Sent %1% models as one mesh."), m_plate.object_count);
            else if (m_model_sent && m_plate.from_selection)
                text = _L("Sent the selected model.");
            else if (m_model_sent)
                text = _L("Sent the models on the plate.");
            if (m_bed_sent) {
                const wxString bed = format_wxstr(_L("Build volume set to %1% x %2% x %3% mm."),
                    m_printer.bed_x, m_printer.bed_y, m_printer.bed_z);
                text = text.empty() ? bed : text + " " + bed;
            }
            if (!text.empty())
                note(text);
        }
    }

    void send_model()
    {
        publish_model(export_plate_mesh());
        if (m_plate.mesh.empty()) {
            note(_L("Load a model on the plate first. Nothing is selected to send."));
            return;
        }
        if (!m_bridge.running()) {
            note(!m_disk_stl.empty()
                ? format_wxstr(_L("Saved %1%. Drop that file onto the page."), from_path(m_disk_stl))
                : _L("The model could not be written for the tool."));
            return;
        }
        m_model_sent = false;
        m_model_failed = false;
        m_model_loading = false;
        m_status_locked = false;
        m_waiting_for_result = false;
        m_force_ingest = true;
        m_alive_ticks = 100;
        m_ticks = 0;
        script_result(m_browser, "(()=>{window.__sliceitall_model=''; return 'cleared';})()");
        begin_ingest(true);
        if (!m_timer.IsRunning())
            m_timer.Start(400);
        update_status();
    }

    void import_result()
    {
        if (m_browser != nullptr) {
            script_result(m_browser, hook_js());
            const std::string result = script_result(m_browser, export_js());
            if (result == "exporting") {
                m_waiting_for_result = true;
                note(_L("Waiting for the page to finish. The result will be placed on the build plate."));
                return;
            }
        }
        const wxString downloads = wxFileName::GetHomeDir() + wxFileName::GetPathSeparator() + "Downloads";
        wxString start = wxFileName::DirExists(downloads) ? downloads : wxStandardPaths::Get().GetDocumentsDir();
        if (start.empty() && !m_disk_stl.empty())
            start = from_path(m_disk_stl.parent_path());
        wxFileDialog dialog(this, _L("Import the file exported by this tool"), start, wxEmptyString,
            _L("Mesh files") + " (*.stl;*.obj;*.3mf)|*.stl;*.STL;*.obj;*.OBJ;*.3mf;*.3MF",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) {
            note(_L("The page had nothing ready to export. Export from the page, or choose a file you already saved."), true);
            return;
        }
        if (place_on_plate(into_path(dialog.GetPath()), false))
            note(_L("Imported onto the build plate."), true);
    }

    bool place_on_plate(const boost::filesystem::path& path, bool replace_sent)
    {
        Plater* plater = wxGetApp().plater();
        if (plater == nullptr) {
            note(_L("The build plate is not open, so the file was not imported."), true);
            return false;
        }
        const std::set<ObjectID> replace = replace_sent ? m_plate.object_ids : std::set<ObjectID>{};
        plater->select_view_3D("3D");
        const std::vector<size_t> loaded = plater->load_files({ path }, true, false);
        if (loaded.empty()) {
            note(_L("The file did not load onto the build plate."), true);
            return false;
        }
        if (!replace.empty()) {
            std::vector<size_t> indexes;
            const Model& model = plater->model();
            for (size_t i = 0; i < model.objects.size(); ++i) {
                if (model.objects[i] != nullptr && replace.count(model.objects[i]->id()) != 0)
                    indexes.push_back(i);
            }
            std::sort(indexes.begin(), indexes.end(), std::greater<size_t>());
            for (size_t index : indexes)
                plater->delete_object_from_model(index);
            m_plate.object_ids.clear();
        }
        return true;
    }

    std::string hook_js() const
    {
        const std::string url = m_bridge.result_url();
        return fill_script(R"JS((function(){
if (window.__sliceitall_hook) return 'hooked';
window.__sliceitall_hook = true;
var origCreate = URL.createObjectURL;
URL.createObjectURL = function(obj) {
  var url = origCreate.apply(URL, arguments);
  if (obj instanceof Blob) window.__sliceitall_last_blob = obj;
  return url;
};
var origClick = HTMLAnchorElement.prototype.click;
HTMLAnchorElement.prototype.click = function() {
  var blob = window.__sliceitall_last_blob;
  window.__sliceitall_last_blob = null;
  if (this.download && blob && this.href && this.href.indexOf('blob:') === 0) {
    var name = this.download;
    fetch('%%URL%%' + encodeURIComponent(name), {
      method: 'POST', mode: 'cors', targetAddressSpace: 'loopback',
      headers: {'Content-Type': 'text/plain'}, body: blob
    }).then(function(res){ if (!res.ok) throw new Error('http'); }).catch(function(){
      try {
        var a = document.createElement('a');
        a.href = origCreate.call(URL, blob);
        a.download = name;
        origClick.call(a);
      } catch (e) {}
    });
    return;
  }
  return origClick.apply(this, arguments);
};
return 'hooked';
})())JS", "URL", url);
    }

    std::string bed_js() const
    {
        std::string script = R"JS((function(){
if (window.__sliceitall_bed) return 'bed';
var vol = document.getElementById('volume');
var vx = document.getElementById('vx');
var vy = document.getElementById('vy');
var vz = document.getElementById('vz');
if (!vol || !vx || !vy || !vz) return 'wait';
vol.value = 'custom';
vol.dispatchEvent(new Event('change', {bubbles:true}));
vx.value = '%%X%%'; vy.value = '%%Y%%'; vz.value = '%%Z%%';
vx.dispatchEvent(new Event('input', {bubbles:true}));
try {
  var nozzle = document.getElementById('nozzle');
  if (nozzle && %%NOZZLE%% > 0) {
    var best = null, err = 1e9;
    for (var i = 0; i < nozzle.options.length; ++i) {
      var d = Math.abs(Number(nozzle.options[i].value) - %%NOZZLE%%);
      if (d < err) { err = d; best = nozzle.options[i].value; }
    }
    if (best != null && String(nozzle.value) !== String(best)) {
      nozzle.value = best;
      nozzle.dispatchEvent(new Event('change', {bubbles:true}));
    }
  }
  var layer = document.getElementById('layer-height');
  if (layer && %%LAYER%% > 0 && %%LAYER%% >= Number(layer.min) && %%LAYER%% <= Number(layer.max)) {
    layer.value = '%%LAYER_TEXT%%';
    layer.dispatchEvent(new Event('input', {bubbles:true}));
    layer.dispatchEvent(new Event('change', {bubbles:true}));
  }
  var gap = document.getElementById('gap');
  if (gap && %%GAP%% > 0 && %%GAP%% >= Number(gap.min) && %%GAP%% <= Number(gap.max)) {
    gap.value = '%%GAP_TEXT%%';
    gap.dispatchEvent(new Event('input', {bubbles:true}));
    gap.dispatchEvent(new Event('change', {bubbles:true}));
  }
} catch (e) {}
window.__sliceitall_bed = true;
return 'bed';
})())JS";
        script = fill_script(std::move(script), "X", std::to_string(m_printer.bed_x));
        script = fill_script(std::move(script), "Y", std::to_string(m_printer.bed_y));
        script = fill_script(std::move(script), "Z", std::to_string(m_printer.bed_z));
        script = fill_script(std::move(script), "NOZZLE", m_printer.nozzle > 0 ? format_decimal(m_printer.nozzle) : "0");
        script = fill_script(std::move(script), "LAYER_TEXT", format_decimal(m_printer.layer_height));
        script = fill_script(std::move(script), "LAYER", m_printer.layer_height > 0 ? format_decimal(m_printer.layer_height) : "0");
        script = fill_script(std::move(script), "GAP_TEXT", format_decimal(m_printer.support_gap));
        script = fill_script(std::move(script), "GAP", m_printer.support_gap > 0 ? format_decimal(m_printer.support_gap) : "0");
        return script;
    }

    static std::string probe_js()
    {
        return R"JS((function(){
try {
  if (window.__sf && window.__sf.part) return 'sent';
  if (window.__sf) return 'alive';
} catch (e) {}
return 'wait';
})())JS";
    }

    std::string ingest_js(bool force) const
    {
        std::string script = R"JS((function(){
if (window.__sliceitall_model === 'loading') return 'started';
if (window.__sliceitall_model === 'sent' && !%%FORCE%%) return 'sent';
var input = document.getElementById('stl-file-input') || document.getElementById('file');
if (!input) return 'wait';
if (document.getElementById('stl-file-input')) {
  var info = document.getElementById('mesh-info');
  if (!info || !info.textContent || !info.textContent.trim()) return 'wait';
}
window.__sliceitall_model = 'loading';
fetch('%%MODEL%%', {mode:'cors', targetAddressSpace:'loopback'}).then(function(r){
  if (!r.ok) throw new Error('http');
  return r.blob();
}).then(function(blob){
  var file = new File([blob], 'sliceitall-model.stl', {type:'model/stl'});
  var dt = new DataTransfer();
  dt.items.add(file);
  input.files = dt.files;
  input.dispatchEvent(new Event('change', {bubbles:true}));
  window.__sliceitall_model = 'sent';
  try {
    var welcome = document.getElementById('welcome-got-it');
    var overlay = document.getElementById('welcome-overlay');
    if (welcome && overlay && !overlay.classList.contains('hidden')) welcome.click();
  } catch (e) {}
}).catch(function(){ window.__sliceitall_model = 'error'; });
return 'started';
})())JS";
        script = fill_script(std::move(script), "FORCE", force ? "true" : "false");
        script = fill_script(std::move(script), "MODEL", m_bridge.model_url());
        return script;
    }

    static std::string export_js()
    {
        return R"JS((function(){
var pf = document.getElementById('export-3mf');
if (pf) {
  if (!window.__sf) return 'unknown';
  var menu = document.getElementById('export-menu');
  var opener = document.getElementById('export');
  if (menu && menu.hidden && opener) opener.click();
  if (pf.disabled) return 'empty';
  pf.click();
  return 'exporting';
}
var bm = document.getElementById('export-3mf-btn') || document.getElementById('export-btn');
if (bm) {
  if (bm.disabled) return 'empty';
  bm.click();
  return 'exporting';
}
return 'unknown';
})())JS";
    }

    EditorPlugin m_plugin;
    std::shared_ptr<PluginSession> m_session;
    LoopbackBridge m_bridge;
    PlateExport m_plate;
    PrinterContext m_printer;
    std::string m_token;
    boost::filesystem::path m_disk_stl;
    wxWebView* m_browser{ nullptr };
    wxStaticText* m_status{ nullptr };
    wxString m_status_text;
    wxTimer m_timer;
    bool m_shutdown{ false };
    bool m_hooked{ false };
    bool m_bed_sent{ false };
    bool m_model_sent{ false };
    bool m_model_failed{ false };
    bool m_model_loading{ false };
    bool m_status_locked{ false };
    bool m_waiting_for_result{ false };
    bool m_force_ingest{ false };
    int m_alive_ticks{ 0 };
    int m_ticks{ 0 };
};

WebAppPluginDialog* find_open_plugin_dialog(const std::string& id)
{
    const auto it = g_open_dialogs.find(id);
    return it == g_open_dialogs.end() ? nullptr : it->second;
}

} // namespace

void register_editor_plugin(EditorPlugin plugin)
{
    const auto it = std::find_if(g_plugins.begin(), g_plugins.end(), [&](const EditorPlugin& existing) {
        return existing.id == plugin.id;
    });
    if (it == g_plugins.end())
        g_plugins.push_back(std::move(plugin));
    else
        *it = std::move(plugin);
}

void ensure_builtin_editor_plugins()
{
    static bool ready = false;
    if (ready)
        return;
    ready = true;

    register_editor_plugin({
        "printfins",
        "Fins",
        "Matthew Trahan",
        "Bake breakaway fin supports into the model.",
        "https://printfins.com",
        "supports",
        {}
    });
    register_editor_plugin({
        "bumpmesh",
        "BumpMesh",
        "CNC Kitchen",
        "Bake a surface texture into the model.",
        "https://bumpmesh.com",
        "surface",
        {}
    });
}

const std::vector<EditorPlugin>& editor_plugins()
{
    return g_plugins;
}

const EditorPlugin* find_editor_plugin(const std::string& id)
{
    const auto it = std::find_if(g_plugins.begin(), g_plugins.end(), [&](const EditorPlugin& plugin) {
        return plugin.id == id;
    });
    return it == g_plugins.end() ? nullptr : &*it;
}

void open_editor_plugin(const std::string& id)
{
    ensure_builtin_editor_plugins();
    const EditorPlugin* plugin = find_editor_plugin(id);
    if (plugin == nullptr)
        return;
    if (plugin->open_native) {
        plugin->open_native();
        return;
    }
    if (plugin->url.empty())
        return;
    if (WebAppPluginDialog* open = find_open_plugin_dialog(id)) {
        open->Show();
        open->Raise();
        return;
    }
    auto* dialog = new WebAppPluginDialog(wxGetApp().GetTopWindow(), *plugin);
    dialog->Show();
}

} // namespace Slic3r::GUI
