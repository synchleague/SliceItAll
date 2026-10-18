///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Anycubic.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <exception>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/uuid/detail/md5.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <ncrypt.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#endif

#include "Http.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {
namespace {

constexpr int k_lan_port = 18910;
constexpr int k_mqtt_port = 9883;

struct HttpResult
{
    bool        ok = false;
    unsigned    status = 0;
    std::string body;
    std::string error;
};

struct LanSession
{
    std::string model_name;
    std::string model_id;
    std::string device_id;
    std::string username;
    std::string password;
    std::string certificate;
    std::string private_key;
    std::string upload_url;
    std::string broker_host;
};

std::string json_text(const pt::ptree& tree, const char* key)
{
    const auto child = tree.get_child_optional(key);
    if (!child)
        return {};
    try {
        return child->get_value<std::string>();
    } catch (const std::exception&) {
        return {};
    }
}

const pt::ptree* child_or(const pt::ptree& tree, const char* key)
{
    const auto child = tree.get_child_optional(key);
    return child ? &*child : nullptr;
}

std::string printer_address(std::string host)
{
    const auto scheme = host.find("://");
    if (scheme != std::string::npos)
        host.erase(0, scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos)
        host.erase(slash);
    if (!host.empty() && host.front() == '[') {
        const auto end = host.find(']');
        if (end != std::string::npos)
            return host.substr(0, end + 1);
    }
    const auto colon = host.rfind(':');
    if (colon != std::string::npos)
        host.erase(colon);
    return host;
}

std::string url_encode(const std::string& value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(char(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c >= 0x20)
                out.push_back(char(c));
            break;
        }
    }
    return out;
}

std::string unescape_newlines(std::string value)
{
    if (value.find('\n') != std::string::npos)
        return value;
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
        } else
            out.push_back(value[i]);
    }
    return out;
}

std::string random_token(size_t length, bool hex)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static const char* hexdigits = "0123456789abcdef";
    const char* chars = hex ? hexdigits : alphabet;
    const size_t span = hex ? 16 : 62;
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<size_t> pick(0, span - 1);
    std::string out(length, '0');
    for (char& c : out)
        c = chars[pick(generator)];
    return out;
}

std::string md5_hex(const std::string& data)
{
    using boost::uuids::detail::md5;
    md5 hash;
    md5::digest_type digest{};
    hash.process_bytes(data.data(), data.size());
    hash.get_digest(digest);
    // Boost writes each MD5 word backwards on little-endian machines.
    // The printer checks a normal MD5, so put the bytes back in order.
    static const char* hex = "0123456789abcdef";
    std::string out(32, '0');
    const auto* bytes = reinterpret_cast<const unsigned char*>(digest);
    for (int i = 0; i < 16; ++i) {
#if BOOST_ENDIAN_LITTLE_BYTE
        const int source = (i & ~3) + (3 - (i & 3));
#else
        const int source = i;
#endif
        const unsigned char piece = bytes[source];
        out[i * 2]     = hex[piece >> 4];
        out[i * 2 + 1] = hex[piece & 0x0f];
    }
    return out;
}

std::string decode_base64(const std::string& text)
{
    std::string filtered;
    filtered.reserve(text.size());
    for (char c : text) {
        if (c == '=' || c == '+' || c == '/' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            filtered.push_back(c);
    }
    if (filtered.empty())
        return {};
    std::string out;
    out.reserve(filtered.size() * 3 / 4);
    int value = 0;
    int bits = 0;
    for (char c : filtered) {
        if (c == '=')
            break;
        int digit = 0;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else digit = 63;
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(char((value >> bits) & 0xff));
        }
    }
    return out;
}

bool aes128_cbc_decrypt(const std::string& cipher, const std::string& key, const std::string& iv, std::string& plain)
{
    if (key.size() < 16 || iv.size() < 16 || cipher.empty())
        return false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
            sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    if (BCryptGenerateSymmetricKey(algorithm, &key_handle, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 16, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    UCHAR iv_bytes[16];
    std::memcpy(iv_bytes, iv.data(), 16);
    std::string out(cipher.size(), '\0');
    ULONG written = 0;
    const NTSTATUS status = BCryptDecrypt(
        key_handle,
        reinterpret_cast<PUCHAR>(const_cast<char*>(cipher.data())),
        static_cast<ULONG>(cipher.size()),
        nullptr,
        iv_bytes,
        16,
        reinterpret_cast<PUCHAR>(&out[0]),
        static_cast<ULONG>(out.size()),
        &written,
        BCRYPT_BLOCK_PADDING);
    BCryptDestroyKey(key_handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0 || written > out.size())
        return false;
    out.resize(written);
    plain = std::move(out);
    return true;
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    std::string out(cipher.size() + 16, '\0');
    int first = 0;
    int second = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
            reinterpret_cast<const unsigned char*>(key.data()),
            reinterpret_cast<const unsigned char*>(iv.data())) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&out[0]), &first,
            reinterpret_cast<const unsigned char*>(cipher.data()), int(cipher.size())) == 1
        && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]) + first, &second) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    out.resize(size_t(first + second));
    plain = std::move(out);
    return true;
#endif
}

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string sign_request(const std::string& token, long long timestamp, const std::string& nonce)
{
    const std::string first = md5_hex(token.substr(0, 16));
    return md5_hex(first + std::to_string(timestamp) + nonce);
}

// Anycubic Slicer Next stores this as pc_device_id and sends it as the did
// query parameter. It is this computer's id, not a secret API key.
std::string anycubic_client_id()
{
    wxString appdata;
    if (!wxGetEnv("APPDATA", &appdata) || appdata.empty())
        return {};
    const std::string path = (boost::filesystem::path(boost::nowide::narrow(appdata.ToStdWstring())) / "AnycubicSlicerNext" / "AnycubicSlicerNext.conf").string();
    boost::nowide::ifstream file(path);
    if (!file)
        return {};
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string key = "\"pc_device_id\"";
    const size_t at = text.find(key);
    if (at == std::string::npos)
        return {};
    const size_t colon = text.find(':', at + key.size());
    const size_t open = colon == std::string::npos ? std::string::npos : text.find('"', colon + 1);
    const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
    if (close == std::string::npos || close <= open + 1)
        return {};
    const std::string id = text.substr(open + 1, close - open - 1);
    if (id.size() < 16 || id.size() > 64)
        return {};
    for (unsigned char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return {};
    return id;
}

// The printer remembers this computer by id. A new id on every test looks like
// another computer and can fill the printer's LAN client list.
std::string pc_device_id()
{
    static const std::string id = [] {
        const std::string existing = anycubic_client_id();
        if (!existing.empty())
            return existing;
        static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device device;
        std::mt19937 generator(device());
        std::uniform_int_distribution<size_t> pick(0, 35);
        std::string value(32, 'A');
        for (char& c : value)
            c = alphabet[pick(generator)];
        return value;
    }();
    return id;
}

HttpResult http_call(Http http, bool accept_json = true)
{
    HttpResult result;
    try {
        http.timeout_connect(8).timeout_max(60);
        if (accept_json)
            http.header("Accept", "application/json");
        http
            .on_complete([&](std::string body, unsigned status) {
                result.status = status;
                result.body = std::move(body);
                result.ok = status >= 200 && status < 300;
                if (!result.ok)
                    result.error = "HTTP " + std::to_string(status);
            })
            .on_error([&](std::string body, std::string error, unsigned status) {
                result.ok = false;
                result.status = status;
                result.body = std::move(body);
                result.error = error.empty() ? "HTTP " + std::to_string(status) : std::move(error);
            })
            .perform_sync();
    } catch (const std::exception& ex) {
        result.ok = false;
        result.error = ex.what();
    }
    return result;
}

bool read_json(const std::string& body, pt::ptree& tree)
{
    try {
        std::stringstream stream(body);
        pt::read_json(stream, tree);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool safe_printer_message(const std::string& message)
{
    if (message.empty() || message.size() > 180)
        return false;
    if (message.find("BEGIN") != std::string::npos || message.find("PRIVATE") != std::string::npos)
        return false;
    if (message.find("token") != std::string::npos || message.find("password") != std::string::npos)
        return false;
    return true;
}

std::string handshake_refusal(const HttpResult& response)
{
    pt::ptree tree;
    if (read_json(response.body, tree)) {
        const std::string code = json_text(tree, "code");
        if (code == "19002")
            return "This printer already has as many computers connected as LAN Mode allows. Disconnect another computer from the printer and try again.";
        std::string message = json_text(tree, "message");
        if (message.empty())
            message = json_text(tree, "msg");
        std::string error = "The printer refused the LAN handshake.";
        if (safe_printer_message(message))
            error += " " + message;
        else if (!code.empty() && code != "200" && code != "0")
            error += " (Code " + code + ")";
        else if (!response.error.empty())
            error += " (" + response.error + ")";
        return error;
    }
    std::string error = "The printer refused the LAN handshake.";
    if (!response.error.empty())
        error += " (" + response.error + ")";
    std::string excerpt;
    for (unsigned char c : response.body) {
        if (excerpt.size() >= 80)
            break;
        if (c >= 32 && c < 127)
            excerpt.push_back(char(c));
    }
    if (safe_printer_message(excerpt))
        error += " " + excerpt;
    return error;
}

void append_mqtt_string(std::string& packet, const std::string& value)
{
    const size_t length = value.size();
    packet.push_back(char((length >> 8) & 0xff));
    packet.push_back(char(length & 0xff));
    packet.append(value);
}

void append_remaining_length(std::string& packet, size_t length)
{
    do {
        unsigned char byte = static_cast<unsigned char>(length % 128);
        length /= 128;
        if (length > 0)
            byte |= 0x80;
        packet.push_back(char(byte));
    } while (length > 0);
}

std::string mqtt_connect_packet(const std::string& client_id, const std::string& username, const std::string& password)
{
    std::string variable;
    append_mqtt_string(variable, "MQTT");
    variable.push_back(4);
    variable.push_back(char(0x80 | 0x40 | 0x02));
    variable.push_back(0);
    variable.push_back(30);
    append_mqtt_string(variable, client_id);
    append_mqtt_string(variable, username);
    append_mqtt_string(variable, password);

    std::string packet;
    packet.push_back(char(0x10));
    append_remaining_length(packet, variable.size());
    packet.append(variable);
    return packet;
}

std::string mqtt_publish_packet(const std::string& topic, const std::string& payload)
{
    std::string variable;
    append_mqtt_string(variable, topic);
    variable.append(payload);
    std::string packet;
    packet.push_back(char(0x30));
    append_remaining_length(packet, variable.size());
    packet.append(variable);
    return packet;
}

bool ends_with_gcode(const std::string& name)
{
    if (name.size() < 6)
        return false;
    const char* tail = name.c_str() + name.size() - 6;
    return tail[0] == '.'
        && std::tolower(static_cast<unsigned char>(tail[1])) == 'g'
        && std::tolower(static_cast<unsigned char>(tail[2])) == 'c'
        && std::tolower(static_cast<unsigned char>(tail[3])) == 'o'
        && std::tolower(static_cast<unsigned char>(tail[4])) == 'd'
        && std::tolower(static_cast<unsigned char>(tail[5])) == 'e';
}

// The printer answers "Invalid gcode file" unless the stored name ends in .gcode.
std::string printer_gcode_name(std::string name)
{
    if (name.empty())
        name = "print";
    if (!ends_with_gcode(name))
        name += ".gcode";
    return name;
}

std::string file_md5(const fs::path& path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return md5_hex(buffer.str());
}

std::string print_command(const LanSession& session, const std::string& filename, const std::string& filepath, uintmax_t filesize, const std::string& md5, std::string& topic)
{
    // The file is already on the printer. A filepath string makes it try to
    // fetch that path and answer with a file download error. A local start
    // uses taskid -1, filepath null, and a dummy url that the printer ignores.
    (void)filepath;
    const std::string message_id = random_token(8, true) + "-" + random_token(4, true) + "-"
        + random_token(4, true) + "-" + random_token(4, true) + "-" + random_token(12, true);
    topic = "anycubic/anycubicCloud/v1/slicer/printer/" + session.model_id + "/" + session.device_id + "/print";
    return std::string("{\"type\":\"print\",\"action\":\"start\",\"timestamp\":") + std::to_string(now_ms())
        + ",\"msgid\":\"" + message_id
        + "\",\"data\":{\"taskid\":\"-1\",\"url\":\"https://anycubic.com/store/aaa.gcode\",\"filename\":\""
        + json_escape(filename)
        + "\",\"filepath\":null,\"filetype\":1,\"project_type\":1,\"filesize\":" + std::to_string(filesize)
        + ",\"md5\":\"" + json_escape(md5) + "\"}}";
}

#ifdef _WIN32
bool pem_to_der(const std::string& pem, const char* begin_mark, const char* end_mark, std::vector<unsigned char>& der)
{
    const size_t begin = pem.find(begin_mark);
    const size_t end = begin == std::string::npos ? std::string::npos : pem.find(end_mark, begin);
    if (begin == std::string::npos || end == std::string::npos)
        return false;
    const std::string body = pem.substr(begin + std::strlen(begin_mark), end - (begin + std::strlen(begin_mark)));
    DWORD size = 0;
    if (!CryptStringToBinaryA(body.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr) || size == 0)
        return false;
    der.resize(size);
    return CryptStringToBinaryA(body.c_str(), 0, CRYPT_STRING_BASE64, der.data(), &size, nullptr, nullptr) == TRUE;
}

void append_der_length(std::vector<unsigned char>& out, size_t length)
{
    if (length < 128)
        out.push_back(static_cast<unsigned char>(length));
    else if (length < 256) {
        out.push_back(0x81);
        out.push_back(static_cast<unsigned char>(length));
    } else {
        out.push_back(0x82);
        out.push_back(static_cast<unsigned char>((length >> 8) & 0xff));
        out.push_back(static_cast<unsigned char>(length & 0xff));
    }
}

std::vector<unsigned char> wrap_pkcs1_as_pkcs8(const std::vector<unsigned char>& pkcs1)
{
    static const unsigned char algorithm[] = {
        0x30, 0x0d,
        0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01,
        0x05, 0x00
    };
    std::vector<unsigned char> octet;
    octet.push_back(0x04);
    append_der_length(octet, pkcs1.size());
    octet.insert(octet.end(), pkcs1.begin(), pkcs1.end());

    std::vector<unsigned char> body{ 0x02, 0x01, 0x00 };
    body.insert(body.end(), algorithm, algorithm + sizeof(algorithm));
    body.insert(body.end(), octet.begin(), octet.end());

    std::vector<unsigned char> wrapped{ 0x30 };
    append_der_length(wrapped, body.size());
    wrapped.insert(wrapped.end(), body.begin(), body.end());
    return wrapped;
}

struct LanTls
{
    SOCKET              socket = INVALID_SOCKET;
    CredHandle          credentials{};
    bool                have_credentials = false;
    CtxtHandle          context{};
    bool                have_context = false;
    PCCERT_CONTEXT      certificate = nullptr;
    NCRYPT_PROV_HANDLE  provider = 0;
    NCRYPT_KEY_HANDLE   key = 0;
    SecPkgContext_StreamSizes sizes{};
    std::string         incoming;
    std::string         plain;

    ~LanTls() { close(); }

    void close()
    {
        if (have_context) {
            DeleteSecurityContext(&context);
            have_context = false;
        }
        if (have_credentials) {
            FreeCredentialsHandle(&credentials);
            have_credentials = false;
        }
        if (certificate) {
            CertFreeCertificateContext(certificate);
            certificate = nullptr;
        }
        if (key) {
            NCryptFreeObject(key);
            key = 0;
        }
        if (provider) {
            NCryptFreeObject(provider);
            provider = 0;
        }
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
    }

    bool install_certificate(const std::string& cert_pem, const std::string& key_pem)
    {
        std::vector<unsigned char> cert_der;
        if (!pem_to_der(cert_pem, "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----", cert_der))
            return false;
        certificate = CertCreateCertificateContext(X509_ASN_ENCODING, cert_der.data(), static_cast<DWORD>(cert_der.size()));
        if (!certificate)
            return false;

        std::vector<unsigned char> key_der;
        const bool pkcs8 = key_pem.find("BEGIN PRIVATE KEY") != std::string::npos;
        const bool pkcs1 = key_pem.find("BEGIN RSA PRIVATE KEY") != std::string::npos;
        if (pkcs8) {
            if (!pem_to_der(key_pem, "-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----", key_der))
                return false;
        } else if (pkcs1) {
            std::vector<unsigned char> rsa;
            if (!pem_to_der(key_pem, "-----BEGIN RSA PRIVATE KEY-----", "-----END RSA PRIVATE KEY-----", rsa))
                return false;
            key_der = wrap_pkcs1_as_pkcs8(rsa);
        } else
            return false;

        if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != 0)
            return false;
        if (NCryptImportKey(provider, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, &key,
                key_der.data(), static_cast<DWORD>(key_der.size()), NCRYPT_SILENT_FLAG) != 0)
            return false;
        DWORD spec = CERT_NCRYPT_KEY_SPEC;
        return CertSetCertificateContextProperty(certificate, CERT_NCRYPT_KEY_HANDLE_PROP_ID,
                CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &key) == TRUE
            && CertSetCertificateContextProperty(certificate, CERT_KEY_SPEC_PROP_ID,
                CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &spec) == TRUE;
    }

    bool raw_send(const char* data, size_t size)
    {
        size_t sent = 0;
        while (sent < size) {
            const int wrote = ::send(socket, data + sent, static_cast<int>(size - sent), 0);
            if (wrote <= 0)
                return false;
            sent += static_cast<size_t>(wrote);
        }
        return true;
    }

    bool connect_socket(const std::string& host)
    {
        static bool sockets_ready = false;
        if (!sockets_ready) {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                return false;
            sockets_ready = true;
        }
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(k_mqtt_port).c_str(), &hints, &result) != 0)
            return false;
        for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
            socket = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (socket == INVALID_SOCKET)
                continue;
            if (::connect(socket, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0)
                break;
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        freeaddrinfo(result);
        if (socket == INVALID_SOCKET)
            return false;
        const DWORD timeout = 8000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        return true;
    }

    bool handshake(const std::string& host)
    {
        const DWORD request = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
            | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_MANUAL_CRED_VALIDATION;
        const std::wstring wide_host = boost::nowide::widen(host);
        SecBuffer outgoing[1]{};
        outgoing[0].BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outgoing_desc{ SECBUFFER_VERSION, 1, outgoing };
        DWORD flags = 0;
        SECURITY_STATUS status = InitializeSecurityContextW(
            &credentials, nullptr, wide_host.empty() ? nullptr : const_cast<wchar_t*>(wide_host.c_str()),
            request, 0, SECURITY_NATIVE_DREP, nullptr, 0, &context, &outgoing_desc, &flags, nullptr);
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED && status != SEC_E_INCOMPLETE_MESSAGE) {
            if (outgoing[0].pvBuffer)
                FreeContextBuffer(outgoing[0].pvBuffer);
            return false;
        }
        have_context = true;

        while (true) {
            if (outgoing[0].pvBuffer && outgoing[0].cbBuffer) {
                const bool sent = raw_send(static_cast<char*>(outgoing[0].pvBuffer), outgoing[0].cbBuffer);
                FreeContextBuffer(outgoing[0].pvBuffer);
                outgoing[0].pvBuffer = nullptr;
                outgoing[0].cbBuffer = 0;
                if (!sent)
                    return false;
            }
            if (status == SEC_E_OK)
                break;
            if (status != SEC_I_CONTINUE_NEEDED && status != SEC_E_INCOMPLETE_MESSAGE)
                return false;
            if (status == SEC_E_INCOMPLETE_MESSAGE || incoming.empty()) {
                char buffer[16384];
                const int got = recv(socket, buffer, sizeof(buffer), 0);
                if (got <= 0)
                    return false;
                incoming.append(buffer, static_cast<size_t>(got));
            }

            SecBuffer incoming_buffers[2]{};
            incoming_buffers[0].pvBuffer = incoming.empty() ? nullptr : &incoming[0];
            incoming_buffers[0].cbBuffer = static_cast<ULONG>(incoming.size());
            incoming_buffers[0].BufferType = SECBUFFER_TOKEN;
            incoming_buffers[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc incoming_desc{ SECBUFFER_VERSION, 2, incoming_buffers };
            outgoing[0].pvBuffer = nullptr;
            outgoing[0].cbBuffer = 0;
            outgoing[0].BufferType = SECBUFFER_TOKEN;
            status = InitializeSecurityContextW(
                &credentials, &context, wide_host.empty() ? nullptr : const_cast<wchar_t*>(wide_host.c_str()),
                request, 0, SECURITY_NATIVE_DREP, &incoming_desc, 0, nullptr, &outgoing_desc, &flags, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE)
                continue;
            if (incoming_buffers[1].BufferType == SECBUFFER_EXTRA && incoming_buffers[1].cbBuffer > 0
                && incoming_buffers[1].cbBuffer <= incoming.size())
                incoming.erase(0, incoming.size() - incoming_buffers[1].cbBuffer);
            else
                incoming.clear();
        }
        return QueryContextAttributes(&context, SECPKG_ATTR_STREAM_SIZES, &sizes) == SEC_E_OK;
    }

    bool open(const LanSession& session, std::string& error)
    {
        // The broker authenticates the MQTT username and password. It does not
        // ask for a client certificate, and presenting the LAN certificate makes
        // Schannel refuse to start the connection.
        SCHANNEL_CRED identity{};
        identity.dwVersion = SCHANNEL_CRED_VERSION;
        identity.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK;
        identity.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        wchar_t package[] = UNISP_NAME_W;
        if (AcquireCredentialsHandleW(nullptr, package, SECPKG_CRED_OUTBOUND, nullptr, &identity,
                nullptr, nullptr, &credentials, nullptr) != SEC_E_OK) {
            error = "Could not start a secure connection to the printer.";
            return false;
        }
        have_credentials = true;
        if (!connect_socket(session.broker_host) || !handshake(session.broker_host)) {
            error = "Could not open the printer's LAN print channel. Leave LAN Mode on and try again.";
            return false;
        }
        return true;
    }

    bool write_all(const std::string& packet)
    {
        const size_t limit = sizes.cbMaximumMessage ? sizes.cbMaximumMessage : 16384;
        size_t offset = 0;
        while (offset < packet.size()) {
            const size_t chunk = std::min(limit, packet.size() - offset);
            std::vector<char> buffer(sizes.cbHeader + chunk + sizes.cbTrailer);
            std::memcpy(buffer.data() + sizes.cbHeader, packet.data() + offset, chunk);
            SecBuffer parts[4]{};
            parts[0].pvBuffer = buffer.data();
            parts[0].cbBuffer = sizes.cbHeader;
            parts[0].BufferType = SECBUFFER_STREAM_HEADER;
            parts[1].pvBuffer = buffer.data() + sizes.cbHeader;
            parts[1].cbBuffer = static_cast<ULONG>(chunk);
            parts[1].BufferType = SECBUFFER_DATA;
            parts[2].pvBuffer = buffer.data() + sizes.cbHeader + chunk;
            parts[2].cbBuffer = sizes.cbTrailer;
            parts[2].BufferType = SECBUFFER_STREAM_TRAILER;
            parts[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, parts };
            if (EncryptMessage(&context, 0, &desc, 0) != SEC_E_OK)
                return false;
            const size_t total = static_cast<size_t>(parts[0].cbBuffer) + parts[1].cbBuffer + parts[2].cbBuffer;
            if (!raw_send(buffer.data(), total))
                return false;
            offset += chunk;
        }
        return true;
    }

    bool take_plain(std::string& out, size_t wanted)
    {
        if (plain.empty())
            return false;
        const size_t take = std::min(wanted - out.size(), plain.size());
        out.append(plain, 0, take);
        plain.erase(0, take);
        return true;
    }

    bool decrypt_incoming()
    {
        while (!incoming.empty()) {
            SecBuffer parts[4]{};
            parts[0].pvBuffer = &incoming[0];
            parts[0].cbBuffer = static_cast<ULONG>(incoming.size());
            parts[0].BufferType = SECBUFFER_DATA;
            parts[1].BufferType = SECBUFFER_EMPTY;
            parts[2].BufferType = SECBUFFER_EMPTY;
            parts[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{ SECBUFFER_VERSION, 4, parts };
            const SECURITY_STATUS status = DecryptMessage(&context, &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE)
                return !plain.empty();
            if (status != SEC_E_OK)
                return false;
            std::string extra;
            std::string data;
            for (const SecBuffer& part : parts) {
                if (part.BufferType == SECBUFFER_DATA && part.cbBuffer && part.pvBuffer)
                    data.append(static_cast<char*>(part.pvBuffer), part.cbBuffer);
                else if (part.BufferType == SECBUFFER_EXTRA && part.cbBuffer && part.pvBuffer)
                    extra.append(static_cast<char*>(part.pvBuffer), part.cbBuffer);
            }
            plain.append(data);
            incoming = std::move(extra);
            if (!plain.empty())
                return true;
        }
        return false;
    }

    bool read_some(std::string& out, size_t wanted)
    {
        out.clear();
        while (out.size() < wanted) {
            if (take_plain(out, wanted))
                continue;
            if (decrypt_incoming() && take_plain(out, wanted))
                continue;
            char buffer[16384];
            const int got = recv(socket, buffer, sizeof(buffer), 0);
            if (got <= 0)
                return out.size() >= 4;
            incoming.append(buffer, static_cast<size_t>(got));
        }
        return true;
    }
};

bool publish_mqtt(const LanSession& session, const std::string& topic, const std::string& payload, std::string& error)
{
    LanTls tls;
    if (!tls.open(session, error))
        return false;
    const std::string client_id = "sia" + random_token(8, true);
    bool sent = tls.write_all(mqtt_connect_packet(client_id, session.username, session.password));
    std::string connack;
    sent = sent && tls.read_some(connack, 4) && connack.size() >= 4
        && static_cast<unsigned char>(connack[0]) == 0x20
        && static_cast<unsigned char>(connack[3]) == 0x00;
    sent = sent && tls.write_all(mqtt_publish_packet(topic, payload));
    tls.write_all(std::string("\xe0\x00", 2));
    if (!sent) {
        error = "The printer did not accept the command.";
        return false;
    }
    return true;
}

bool publish_print(const LanSession& session, const std::string& filename, const std::string& filepath, uintmax_t filesize, const std::string& md5, std::string& error)
{
    std::string topic;
    const std::string payload = print_command(session, filename, filepath, filesize, md5, topic);
    if (!publish_mqtt(session, topic, payload, error)) {
        if (error == "The printer did not accept the command.")
            error = "The file is on the printer, but the printer did not accept the print command.";
        return false;
    }
    return true;
}
#else
void set_socket_timeout(int fd, int milliseconds)
{
    if (fd < 0)
        return;
    struct timeval timeout;
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool bio_write_all(BIO* bio, const std::string& packet)
{
    size_t sent = 0;
    while (sent < packet.size()) {
        const int wrote = BIO_write(bio, packet.data() + sent, int(packet.size() - sent));
        if (wrote <= 0)
            return false;
        sent += size_t(wrote);
    }
    return true;
}

bool bio_read_some(BIO* bio, std::string& out, size_t wanted)
{
    out.clear();
    while (out.size() < wanted) {
        char buffer[64];
        const int got = BIO_read(bio, buffer, int(std::min(sizeof(buffer), wanted - out.size())));
        if (got <= 0)
            return !out.empty() && out.size() >= 4;
        out.append(buffer, size_t(got));
    }
    return true;
}

bool publish_mqtt(const LanSession& session, const std::string& topic, const std::string& payload, std::string& error)
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        error = "Could not start a secure connection to the printer.";
        return false;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    if (!session.certificate.empty() && !session.private_key.empty()) {
        BIO* cert_bio = BIO_new_mem_buf(session.certificate.data(), int(session.certificate.size()));
        X509* cert = cert_bio ? PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr) : nullptr;
        if (cert_bio)
            BIO_free(cert_bio);
        BIO* key_bio = BIO_new_mem_buf(session.private_key.data(), int(session.private_key.size()));
        EVP_PKEY* key = key_bio ? PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr) : nullptr;
        if (key_bio)
            BIO_free(key_bio);
        const bool installed = cert && key
            && SSL_CTX_use_certificate(ctx, cert) == 1
            && SSL_CTX_use_PrivateKey(ctx, key) == 1;
        if (cert)
            X509_free(cert);
        if (key)
            EVP_PKEY_free(key);
        if (!installed) {
            SSL_CTX_free(ctx);
            error = "The printer did not provide a usable LAN certificate.";
            return false;
        }
    }

    BIO* bio = BIO_new_ssl_connect(ctx);
    if (!bio) {
        SSL_CTX_free(ctx);
        error = "Could not start a secure connection to the printer.";
        return false;
    }
    const std::string target = session.broker_host + ":" + std::to_string(k_mqtt_port);
    BIO_set_conn_hostname(bio, target.c_str());
    if (BIO_do_connect(bio) != 1 || BIO_do_handshake(bio) != 1) {
        BIO_free_all(bio);
        SSL_CTX_free(ctx);
        error = "Could not open the printer's LAN print channel. Leave LAN Mode on and try again.";
        return false;
    }

    SSL* ssl = nullptr;
    BIO_get_ssl(bio, &ssl);
    if (ssl)
        set_socket_timeout(SSL_get_fd(ssl), 8000);

    const std::string client_id = "sia" + random_token(8, true);
    bool sent = bio_write_all(bio, mqtt_connect_packet(client_id, session.username, session.password));
    std::string connack;
    sent = sent && bio_read_some(bio, connack, 4) && connack.size() >= 4
        && static_cast<unsigned char>(connack[0]) == 0x20
        && static_cast<unsigned char>(connack[3]) == 0x00;
    sent = sent && bio_write_all(bio, mqtt_publish_packet(topic, payload));
    bio_write_all(bio, std::string("\xe0\x00", 2));
    BIO_free_all(bio);
    SSL_CTX_free(ctx);
    if (!sent) {
        error = "The printer did not accept the command.";
        return false;
    }
    return true;
}

bool publish_print(const LanSession& session, const std::string& filename, const std::string& filepath, uintmax_t filesize, const std::string& md5, std::string& error)
{
    std::string topic;
    const std::string payload = print_command(session, filename, filepath, filesize, md5, topic);
    if (!publish_mqtt(session, topic, payload, error)) {
        if (error == "The printer did not accept the command.")
            error = "The file is on the printer, but the printer did not accept the print command.";
        return false;
    }
    return true;
}
#endif

bool open_session(const std::string& address, LanSession& session, std::string& error)
{
    if (address.empty()) {
        error = "Enter the printer's Wi-Fi address.";
        return false;
    }
    const std::string root = "http://" + address + ":" + std::to_string(k_lan_port);
    const HttpResult info_response = http_call(Http::get(root + "/info"));
    if (!info_response.ok) {
        error = "Could not reach the printer. On the printer, open Settings, then Network, then LAN Mode, and use the Wi-Fi address shown there.";
        if (!info_response.error.empty())
            error += " (" + info_response.error + ")";
        return false;
    }
    pt::ptree info;
    if (!read_json(info_response.body, info)) {
        error = "The printer answered, but not with LAN information. Turn LAN Mode on and try again.";
        return false;
    }
    const std::string token = json_text(info, "token");
    if (token.size() < 32) {
        error = "The printer did not return a LAN token. Turn LAN Mode on and try again.";
        return false;
    }
    session.model_name = json_text(info, "modelName");
    if (session.model_name.empty())
        session.model_name = json_text(info, "deviceName");
    session.model_id = json_text(info, "modelId");
    session.upload_url = json_text(info, "fileUploadurl");
    if (session.upload_url.empty())
        session.upload_url = root + "/gcode_upload?s=" + url_encode(token);

    const long long timestamp = now_ms();
    const std::string nonce = random_token(6, false);
    const std::string signature = sign_request(token, timestamp, nonce);
    std::string ctrl_url = json_text(info, "ctrlInfoUrl");
    if (ctrl_url.empty())
        ctrl_url = root + "/ctrl";
    ctrl_url += (ctrl_url.find('?') == std::string::npos ? "?" : "&");
    ctrl_url += "ts=" + std::to_string(timestamp)
        + "&nonce=" + nonce
        + "&sign=" + signature
        + "&did=" + pc_device_id();

    // Match the POST Anycubic Slicer Next sends: no form body and no extra
    // content type. Curl's default form header makes this printer answer HTTP 400.
    Http ctrl_request = Http::post(ctrl_url);
    ctrl_request.set_post_body(std::string());
    ctrl_request.remove_header("Content-Type");
    ctrl_request.remove_header("User-Agent");
    ctrl_request.header("Connection", "close");
    const HttpResult ctrl_response = http_call(std::move(ctrl_request), false);
    pt::ptree ctrl;
    if (!read_json(ctrl_response.body, ctrl)) {
        error = ctrl_response.ok
            ? "The printer's LAN handshake could not be read."
            : handshake_refusal(ctrl_response);
        return false;
    }
    const std::string code = json_text(ctrl, "code");
    if (!ctrl_response.ok || (!code.empty() && code != "200" && code != "0")) {
        error = handshake_refusal(ctrl_response);
        return false;
    }
    const pt::ptree* data = child_or(ctrl, "data");
    if (!data)
        data = &ctrl;
    const std::string encrypted = json_text(*data, "info");
    const std::string iv = json_text(*data, "token");
    std::string plain;
    if (!aes128_cbc_decrypt(decode_base64(encrypted), token.substr(16, 16), iv, plain) || !read_json(plain, ctrl)) {
        error = "The printer's LAN handshake could not be read.";
        return false;
    }
    const pt::ptree* credentials = child_or(ctrl, "data");
    if (!credentials || json_text(*credentials, "deviceId").empty())
        credentials = &ctrl;
    session.device_id = json_text(*credentials, "deviceId");
    session.username = json_text(*credentials, "username");
    session.password = json_text(*credentials, "password");
    session.certificate = unescape_newlines(json_text(*credentials, "devicecrt"));
    session.private_key = unescape_newlines(json_text(*credentials, "devicepk"));
    const std::string mode_id = json_text(*credentials, "modeId");
    if (!mode_id.empty())
        session.model_id = mode_id;
    if (session.model_id.empty())
        session.model_id = json_text(*credentials, "modelId");
    session.broker_host = address;
    const std::string broker = json_text(*credentials, "broker");
    const std::string broker_host = printer_address(broker);
    if (!broker_host.empty())
        session.broker_host = broker_host;
    if (session.device_id.empty() || session.username.empty() || session.model_id.empty()) {
        error = "The printer's LAN handshake did not include a device id.";
        return false;
    }
    return true;
}

std::string control_message(const char* type, const char* action, const std::string& data_json)
{
    const std::string message_id = random_token(8, true) + "-" + random_token(4, true) + "-"
        + random_token(4, true) + "-" + random_token(4, true) + "-" + random_token(12, true);
    return std::string("{\"type\":\"") + type + "\",\"action\":\"" + action
        + "\",\"timestamp\":" + std::to_string(now_ms())
        + ",\"msgid\":\"" + message_id + "\",\"data\":" + data_json + "}";
}

std::string control_topic(const LanSession& session, const char* type)
{
    return "anycubic/anycubicCloud/v1/web/printer/" + session.model_id + "/" + session.device_id + "/" + type;
}

bool send_control(const std::string& host, const char* type, const char* action, const std::string& data_json, std::string& error)
{
    LanSession session;
    if (!open_session(printer_address(host), session, error))
        return false;
    return publish_mqtt(session, control_topic(session, type), control_message(type, action, data_json), error);
}

int clamp_channel(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return value;
}

bool read_channel(const pt::ptree& node, int& out)
{
    try {
        out = clamp_channel(node.get_value<int>());
        return true;
    } catch (const std::exception&) {
        try {
            out = clamp_channel(static_cast<int>(node.get_value<double>()));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
}

bool hex_color(const std::string& text, int& red, int& green, int& blue)
{
    std::string hex = text;
    if (!hex.empty() && hex[0] == '#')
        hex.erase(hex.begin());
    if (hex.size() < 6)
        return false;
    for (char ch : hex)
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return false;
    red = clamp_channel(std::stoi(hex.substr(0, 2), nullptr, 16));
    green = clamp_channel(std::stoi(hex.substr(2, 2), nullptr, 16));
    blue = clamp_channel(std::stoi(hex.substr(4, 2), nullptr, 16));
    return true;
}

// color may be [r,g,b], [r,g,b,a], [[r,g,b,a], ...], or a hex string.
bool read_rgb(const pt::ptree& node, int& red, int& green, int& blue)
{
    try {
        if (hex_color(node.get_value<std::string>(), red, green, blue))
            return true;
    } catch (const std::exception&) {
    }
    int channel = 0;
    int parts[3] = { 0, 0, 0 };
    for (const auto& part : node) {
        int value = 0;
        if (read_channel(part.second, value)) {
            if (channel < 3)
                parts[channel] = value;
            ++channel;
            if (channel >= 3)
                break;
            continue;
        }
        if (channel == 0 && read_rgb(part.second, red, green, blue))
            return true;
    }
    if (channel < 3)
        return false;
    red = parts[0];
    green = parts[1];
    blue = parts[2];
    return true;
}

bool json_int_field(const pt::ptree& tree, const char* key, int& out)
{
    const auto child = tree.get_child_optional(key);
    if (!child)
        return false;
    try {
        out = child->get_value<int>();
        return true;
    } catch (const std::exception&) {
        try {
            out = static_cast<int>(child->get_value<double>());
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
}

bool job_state_active(const std::string& state, bool& known)
{
    std::string lower = state;
    for (char& ch : lower)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // These words belong to the message envelope, not the printer.
    if (lower == "done" || lower == "updated" || lower == "ok" || lower == "success")
        return false;
    const char* idle[] = {
        "free", "idle", "ready", "finish", "finished", "complete", "completed",
        "stoped", "stopped", "canceled", "cancelled", "failed", "error"
    };
    for (const char* word : idle) {
        if (lower == word) {
            known = true;
            return false;
        }
    }
    const char* busy[] = {
        "busy", "printing", "print", "paused", "pause", "pausing", "heating",
        "preheating", "preparing", "checking", "leveling", "downloading", "stopping"
    };
    for (const char* word : busy) {
        if (lower == word) {
            known = true;
            return true;
        }
    }
    return false;
}

void absorb_job(const pt::ptree& data, AnycubicReadings& readings)
{
    if (const pt::ptree* project = child_or(data, "project"))
        absorb_job(*project, readings);

    bool known = false;
    const std::string state = json_text(data, "state");
    if (!state.empty()) {
        const bool active = job_state_active(state, known);
        if (known) {
            readings.has_job = true;
            readings.printing = active;
        }
    } else {
        int code = 0;
        if (json_int_field(data, "state", code)) {
            readings.has_job = true;
            readings.printing = code == 1 || code == 2;
        }
    }

    const std::string name = json_text(data, "filename");
    if (!name.empty()) {
        readings.job_name = name;
        readings.has_job = true;
    }
    int number = 0;
    if (json_int_field(data, "progress", number)) {
        readings.progress = number;
        readings.has_progress = true;
        readings.has_job = true;
        // A print report often has no state word. Progress between 1 and 99 is a running job.
        // An explicit idle state from this same object still wins.
        if (!known && !name.empty() && number > 0 && number < 100)
            readings.printing = true;
        if (!known && number >= 100)
            readings.printing = false;
    }
    if (json_int_field(data, "curr_layer", number)) {
        readings.layer = number;
        readings.has_layer = true;
    }
    if (json_int_field(data, "total_layers", number)) {
        readings.layers = number;
        readings.has_total_layers = true;
    }
    if (json_int_field(data, "remain_time", number)) {
        readings.remain_minutes = number;
        readings.has_remain = true;
    }
    if (json_int_field(data, "print_time", number)) {
        readings.elapsed_minutes = number;
        readings.has_elapsed = true;
    }
}

void absorb_status_tree(const pt::ptree& data, AnycubicReadings& readings)
{
    absorb_job(data, readings);
    int value = 0;
    if (json_int_field(data, "curr_nozzle_temp", value)) {
        readings.nozzle = value;
        readings.has_nozzle = true;
    }
    if (json_int_field(data, "curr_hotbed_temp", value)) {
        readings.bed = value;
        readings.has_bed = true;
    }
    if (json_int_field(data, "curr_chamber_temp", value)) {
        readings.chamber = value;
        readings.has_chamber = true;
    }
    if (json_int_field(data, "fan_speed_pct", value)) {
        readings.model_fan = value;
        readings.has_model_fan = true;
    }
    if (json_int_field(data, "aux_fan_speed_pct", value)) {
        readings.aux_fan = value;
        readings.has_aux_fan = true;
    }
    if (const pt::ptree* temp = child_or(data, "temp"))
        absorb_status_tree(*temp, readings);
    if (const pt::ptree* boxes = child_or(data, "multi_color_box")) {
        int count = 0;
        readings.ace_reported = true;
        readings.ace_slots.clear();
        for (const auto& item : *boxes) {
            int id = -1;
            if (!json_int_field(item.second, "id", id) || id < 0)
                continue;
            int status = 1;
            json_int_field(item.second, "status", status);
            if (status == 0)
                continue;
            ++count;
            if (const pt::ptree* slots = child_or(item.second, "slots")) {
                for (const auto& slot_item : *slots) {
                    AnycubicAceSlot slot;
                    slot.box = id;
                    json_int_field(slot_item.second, "index", slot.index);
                    slot.material = json_text(slot_item.second, "type");
                    if (slot.material.empty())
                        slot.material = json_text(slot_item.second, "material");
                    int edit_status = 1;
                    json_int_field(slot_item.second, "edit_status", edit_status);
                    if (const pt::ptree* color = child_or(slot_item.second, "color"))
                        slot.has_color = read_rgb(*color, slot.red, slot.green, slot.blue);
                    if (!slot.has_color)
                        if (const pt::ptree* colors = child_or(slot_item.second, "colors"))
                            slot.has_color = read_rgb(*colors, slot.red, slot.green, slot.blue);
                    // A missing type or color is not an empty slot. Clearing the
                    // saved spool in that case hides the colors on the start sheet.
                    slot.empty = edit_status == 2 && !slot.has_color && slot.material.empty();
                    if (!slot.empty && !slot.has_color && slot.material.empty())
                        continue;
                    readings.ace_slots.push_back(std::move(slot));
                }
            }
        }
        if (count > 0) {
            readings.has_ace_count = true;
            readings.ace_count = count;
        }
        // Some reports number slots from 1. A list that already contains 0 is 0-based.
        bool seen_box[2] = { false, false };
        bool has_zero[2] = { false, false };
        for (const AnycubicAceSlot& slot : readings.ace_slots) {
            if (slot.box < 0 || slot.box > 1)
                continue;
            seen_box[slot.box] = true;
            if (slot.index == 0)
                has_zero[slot.box] = true;
        }
        for (AnycubicAceSlot& slot : readings.ace_slots) {
            if (slot.box < 0 || slot.box > 1)
                continue;
            if (seen_box[slot.box] && !has_zero[slot.box] && slot.index > 0)
                --slot.index;
        }
    }
    if (const pt::ptree* lights = child_or(data, "lights")) {
        for (const auto& item : *lights) {
            int type = -1;
            int status = 0;
            if (!json_int_field(item.second, "type", type) || type != 2)
                continue;
            if (!json_int_field(item.second, "status", status))
                continue;
            readings.has_light = true;
            readings.light_on = status != 0;
        }
    } else {
        int type = -1;
        int status = 0;
        if (json_int_field(data, "type", type) && type == 2 && json_int_field(data, "status", status)) {
            readings.has_light = true;
            readings.light_on = status != 0;
        }
    }
}

void absorb_status_json(const std::string& payload, AnycubicReadings& readings)
{
    pt::ptree tree;
    if (payload.empty() || !read_json(payload, tree))
        return;
    const pt::ptree* data = child_or(tree, "data");
    if (!data)
        data = &tree;
    absorb_status_tree(*data, readings);
}

#ifdef _WIN32
std::string mqtt_subscribe_packet(const std::string& topic)
{
    std::string variable;
    variable.push_back(0);
    variable.push_back(1);
    append_mqtt_string(variable, topic);
    variable.push_back(0);
    std::string packet;
    packet.push_back(char(0x82));
    append_remaining_length(packet, variable.size());
    packet.append(variable);
    return packet;
}

bool read_mqtt_packet(LanTls& tls, unsigned char& header, std::string& body)
{
    std::string byte;
    if (!tls.read_some(byte, 1) || byte.empty())
        return false;
    header = static_cast<unsigned char>(byte[0]);
    size_t value = 0;
    size_t multiplier = 1;
    for (int i = 0; i < 4; ++i) {
        byte.clear();
        if (!tls.read_some(byte, 1) || byte.empty())
            return false;
        const unsigned char digit = static_cast<unsigned char>(byte[0]);
        value += size_t(digit & 0x7f) * multiplier;
        if ((digit & 0x80) == 0)
            break;
        multiplier *= 128;
        if (i == 3)
            return false;
    }
    if (value > 256 * 1024)
        return false;
    body.clear();
    if (value == 0)
        return true;
    return tls.read_some(body, value) && body.size() == value;
}

std::string mqtt_payload(unsigned char header, const std::string& body)
{
    if ((header & 0xf0) != 0x30 || body.size() < 2)
        return {};
    const size_t topic_len = (size_t(static_cast<unsigned char>(body[0])) << 8) | static_cast<unsigned char>(body[1]);
    size_t cursor = 2 + topic_len;
    if (((header >> 1) & 0x03) > 0)
        cursor += 2;
    if (cursor > body.size())
        return {};
    return body.substr(cursor);
}

bool collect_status(const LanSession& session, AnycubicReadings& readings, std::string& error)
{
    LanTls tls;
    if (!tls.open(session, error))
        return false;
    const std::string client_id = "sia" + random_token(8, true);
    if (!tls.write_all(mqtt_connect_packet(client_id, session.username, session.password))) {
        error = "The printer did not accept the command.";
        return false;
    }
    std::string connack;
    if (!tls.read_some(connack, 4) || connack.size() < 4
        || static_cast<unsigned char>(connack[0]) != 0x20
        || static_cast<unsigned char>(connack[3]) != 0x00) {
        error = "The printer did not accept the command.";
        return false;
    }
    const std::string reports = "anycubic/anycubicCloud/v1/printer/public/" + session.model_id + "/" + session.device_id + "/#";
    if (!tls.write_all(mqtt_subscribe_packet(reports))) {
        error = "The printer did not accept the command.";
        return false;
    }
    // info and print carry the job name, progress, and busy/free state.
    // Their data field is null. The others use an empty object.
    const char* kinds[] = { "info", "print", "tempature", "fan", "light" };
    for (const char* kind : kinds) {
        const std::string kind_name = kind;
        const bool job_query = kind_name == "info" || kind_name == "print";
        const std::string payload = control_message(kind, "query", job_query ? "null" : "{}");
        if (!tls.write_all(mqtt_publish_packet(control_topic(session, kind), payload))) {
            error = "The printer did not accept the command.";
            return false;
        }
    }
    const std::string ace_query = control_message("multiColorBox", "getInfo", "{}");
    tls.write_all(mqtt_publish_packet(control_topic(session, "multiColorBox"), ace_query));
    if (tls.socket != INVALID_SOCKET) {
        const DWORD timeout = 3000;
        setsockopt(tls.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }
    for (int packet = 0; packet < 24; ++packet) {
        unsigned char header = 0;
        std::string body;
        if (!read_mqtt_packet(tls, header, body))
            break;
        absorb_status_json(mqtt_payload(header, body), readings);
        // Keep reading until the ACE reply arrives. Stopping on temperature alone
        // drops the slot colors, so the start sheet shows an empty rack.
        const bool sensors = readings.ace_reported && readings.has_nozzle && readings.has_bed
            && readings.has_model_fan && readings.has_light;
        // The first print reply is often just the file name and a percent.
        // Layer and remaining time follow in the info report, so keep reading
        // while a job is on and those are still missing.
        const bool job_detail = !readings.printing || (readings.has_layer && readings.has_total_layers && readings.has_remain && readings.has_elapsed);
        if (sensors && readings.has_job && job_detail)
            break;
        // The job reply can follow the temperatures. Wait briefly for it, then give up
        // so an idle printer that never sends a print report does not stall the poll.
        if (sensors && tls.socket != INVALID_SOCKET) {
            const DWORD brief = 400;
            setsockopt(tls.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&brief), sizeof(brief));
        }
        if (sensors && packet >= 6)
            break;
    }
    tls.write_all(std::string("\xe0\x00", 2));
    if (!readings.has_nozzle && !readings.has_bed && !readings.has_chamber
        && !readings.has_model_fan && !readings.has_aux_fan && !readings.has_light) {
        error = "The printer did not report its temperatures or light.";
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: printer readings received";
    return true;
}
#else
bool collect_status(const LanSession&, AnycubicReadings&, std::string& error)
{
    error = "The printer's readings are shown on Windows.";
    return false;
}
#endif

} // namespace

Anycubic::Anycubic(DynamicPrintConfig* config)
    : m_host(config->opt_string("print_host"))
{}

const char* Anycubic::get_name() const { return "Anycubic"; }

wxString Anycubic::get_test_ok_msg() const
{
    return _L("Connection to the Anycubic printer works. LAN Mode is on, and nothing else needs to be installed on the printer.");
}

wxString Anycubic::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s\n\n%s", _L("Could not connect to the Anycubic printer"), msg);
}

bool Anycubic::test(wxString& msg) const
{
    LanSession session;
    std::string error;
    if (!open_session(printer_address(m_host), session, error)) {
        msg = GUI::from_u8(error);
        return false;
    }
    if (!session.model_name.empty())
        msg = GUI::from_u8(session.model_name);
    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: connected to " << session.model_name << " (" << session.model_id << ")";
    return true;
}

bool Anycubic::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn) const
{
    LanSession session;
    std::string error;
    if (!open_session(printer_address(m_host), session, error)) {
        error_fn(GUI::from_u8(error));
        return false;
    }

    const fs::path filename = upload_data.upload_path.filename();
    const std::string filename_utf8 = printer_gcode_name(boost::nowide::narrow(filename.wstring()));
    boost::system::error_code size_error;
    const uintmax_t filesize = fs::file_size(upload_data.source_path, size_error);
    if (size_error) {
        error_fn(GUI::from_u8("The sliced file could not be read."));
        return false;
    }
    const std::string digest = file_md5(upload_data.source_path);

    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: uploading " << filename_utf8;
    bool uploaded = true;
    std::string stored_path = filename_utf8;
    // The file part has to be named "gcode". The name "file" is answered
    // with "Invalid gcode file". X-File-Length lets the printer finish the transfer.
    Http upload = Http::post(session.upload_url);
    upload.timeout_connect(8)
        .timeout_max(0)
        .header("X-File-Length", std::to_string(filesize))
        .form_add("filename", filename_utf8)
        .form_add_file("gcode", upload_data.source_path, filename_utf8)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: upload HTTP " << status;
            pt::ptree tree;
            if (read_json(body, tree)) {
                const std::string code = json_text(tree, "code");
                if (!code.empty() && code != "200" && code != "0") {
                    uploaded = false;
                    error = json_text(tree, "message");
                    if (error.empty())
                        error = json_text(tree, "msg");
                    if (error.empty())
                        error = "The printer rejected the file.";
                }
                const pt::ptree* data = child_or(tree, "data");
                if (!data)
                    data = &tree;
                const std::string returned = json_text(*data, "filepath");
                if (!returned.empty())
                    stored_path = returned;
                else {
                    const std::string returned_name = json_text(*data, "filename");
                    if (!returned_name.empty())
                        stored_path = returned_name;
                }
            }
            if (status < 200 || status >= 300) {
                uploaded = false;
                if (error.empty())
                    error = "HTTP " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string http_error, unsigned status) {
            uploaded = false;
            error = http_error.empty() ? "HTTP " + std::to_string(status) : http_error;
            if (!body.empty() && body.size() < 300)
                error += ": " + body;
            BOOST_LOG_TRIVIAL(error) << "Anycubic LAN: upload failed: " << error;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel)
                uploaded = false;
        });
    try {
        upload.perform_sync();
    } catch (const std::exception& ex) {
        error_fn(GUI::from_u8(ex.what()));
        return false;
    }
    if (!uploaded) {
        error_fn(GUI::from_u8(error.empty() ? std::string("The printer rejected the file.") : error));
        return false;
    }
    if (upload_data.post_action != PrintHostPostUploadAction::StartPrint)
        return true;
    if (!publish_print(session, filename_utf8, stored_path, filesize, digest, error)) {
        error_fn(GUI::from_u8(error));
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: print start sent for " << filename_utf8;
    return true;
}

bool Anycubic::set_chamber_light(bool on, std::string& error) const
{
    const std::string data = on
        ? "{\"type\":2,\"status\":1,\"brightness\":100}"
        : "{\"type\":2,\"status\":0,\"brightness\":0}";
    if (!send_control(m_host, "light", "control", data, error))
        return false;
    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: chamber light " << (on ? "on" : "off");
    return true;
}

bool Anycubic::set_camera_capture(bool start, std::string& error) const
{
    if (!send_control(m_host, "video", start ? "startCapture" : "stopCapture", "{}", error))
        return false;
    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: camera capture " << (start ? "started" : "stopped");
    return true;
}

bool Anycubic::set_ace_slot(int box, int index, int red, int green, int blue, const std::string& material, bool empty, std::string& error) const
{
    if (box < 0 || index < 0) {
        error = "This spool is not on an ACE unit.";
        return false;
    }
    auto clamp_byte = [](int value) {
        if (value < 0)
            return 0;
        if (value > 255)
            return 255;
        return value;
    };
    red = empty ? 0 : clamp_byte(red);
    green = empty ? 0 : clamp_byte(green);
    blue = empty ? 0 : clamp_byte(blue);
    // edit_status 1 is a color typed in by hand. 2 is an empty slot.
    const std::string data = "{\"multi_color_box\":[{\"id\":" + std::to_string(box)
        + ",\"slots\":[{\"index\":" + std::to_string(index)
        + ",\"type\":\"" + json_escape(empty ? std::string() : material)
        + "\",\"color\":[" + std::to_string(red) + "," + std::to_string(green) + "," + std::to_string(blue)
        + "],\"edit_status\":" + (empty ? "2" : "1") + "}]}]}";
    if (!send_control(m_host, "multiColorBox", "setInfo", data, error))
        return false;
    BOOST_LOG_TRIVIAL(info) << "Anycubic LAN: ACE slot updated";
    return true;
}

bool Anycubic::read_status(AnycubicReadings& readings, std::string& error) const
{
    LanSession session;
    if (!open_session(printer_address(m_host), session, error))
        return false;
    return collect_status(session, readings, error);
}

#ifdef _WIN32
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {

struct PreviewState
{
    std::string host;
    std::atomic<bool> stop { false };
    std::mutex socket_mutex;
    SOCKET socket { INVALID_SOCKET };
    std::mutex picture_mutex;
    int width { 0 };
    int height { 0 };
    int serial { 0 };
    std::vector<unsigned char> rgb;
    std::string error;
    std::thread thread;

    void fail(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(picture_mutex);
        error = text;
    }

    void publish(int frame_width, int frame_height, std::vector<unsigned char> frame)
    {
        std::lock_guard<std::mutex> lock(picture_mutex);
        width = frame_width;
        height = frame_height;
        rgb = std::move(frame);
        ++serial;
        error.clear();
    }
};

void nv12_to_rgb(const unsigned char* nv12, int width, int height, int stride, std::vector<unsigned char>& rgb)
{
    rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    const unsigned char* y_plane = nv12;
    const unsigned char* uv_plane = nv12 + stride * height;
    size_t out = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int c = int(y_plane[y * stride + x]) - 16;
            const int d = int(uv_plane[(y / 2) * stride + (x & ~1)]) - 128;
            const int e = int(uv_plane[(y / 2) * stride + (x & ~1) + 1]) - 128;
            const int r = (298 * c + 409 * e + 128) >> 8;
            const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            const int b = (298 * c + 516 * d + 128) >> 8;
            rgb[out++] = static_cast<unsigned char>(std::max(0, std::min(255, r)));
            rgb[out++] = static_cast<unsigned char>(std::max(0, std::min(255, g)));
            rgb[out++] = static_cast<unsigned char>(std::max(0, std::min(255, b)));
        }
    }
}

bool annexb_from_record(const unsigned char* record, size_t size, std::string& out, int& length_size)
{
    if (size < 7)
        return false;
    length_size = (record[4] & 0x03) + 1;
    size_t cursor = 5;
    const int sps_count = record[cursor] & 0x1f;
    ++cursor;
    out.clear();
    auto take = [&](int count) {
        for (int i = 0; i < count; ++i) {
            if (cursor + 2 > size)
                return false;
            const int nal_size = (int(record[cursor]) << 8) | int(record[cursor + 1]);
            cursor += 2;
            if (nal_size <= 0 || cursor + size_t(nal_size) > size)
                return false;
            out.append("\x00\x00\x00\x01", 4);
            out.append(reinterpret_cast<const char*>(record + cursor), size_t(nal_size));
            cursor += size_t(nal_size);
        }
        return true;
    };
    if (!take(sps_count) || cursor >= size)
        return false;
    const int pps_count = record[cursor];
    ++cursor;
    return take(pps_count);
}

std::string nalus_to_annexb(const unsigned char* data, size_t size, int length_size)
{
    std::string out;
    size_t cursor = 0;
    while (cursor + size_t(length_size) <= size) {
        int nal_size = 0;
        for (int i = 0; i < length_size; ++i)
            nal_size = (nal_size << 8) | int(data[cursor + i]);
        cursor += size_t(length_size);
        if (nal_size <= 0 || cursor + size_t(nal_size) > size)
            break;
        out.append("\x00\x00\x00\x01", 4);
        out.append(reinterpret_cast<const char*>(data + cursor), size_t(nal_size));
        cursor += size_t(nal_size);
    }
    return out;
}

class H264Pump
{
public:
    ~H264Pump() { reset(); }

    bool feed_sequence(const unsigned char* record, size_t size)
    {
        m_parameter_sets.clear();
        if (!annexb_from_record(record, size, m_parameter_sets, m_length_size))
            return false;
        return ensure_decoder();
    }

    bool feed_nalus(const unsigned char* data, size_t size, bool keyframe, PreviewState& state)
    {
        if (!m_decoder && !ensure_decoder())
            return false;
        std::string sample = keyframe ? m_parameter_sets : std::string();
        sample += nalus_to_annexb(data, size, m_length_size);
        if (sample.empty())
            return true;
        return decode(sample, keyframe, state);
    }

private:
    bool ensure_decoder()
    {
        if (m_decoder)
            return true;
        if (!m_started) {
            const HRESULT started = MFStartup(MF_VERSION);
            if (FAILED(started))
                return false;
            m_started = true;
        }
        // The software H.264 decoder. A hardware decoder from the enumerator is
        // often asynchronous and never returns a picture on this path.
        const CLSID h264_decoder = { 0x62ce7e72, 0x4c71, 0x4d20, { 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d } };
        HRESULT hr = CoCreateInstance(h264_decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_decoder));
        if (FAILED(hr) || !m_decoder) {
            MFT_REGISTER_TYPE_INFO input_info { MFMediaType_Video, MFVideoFormat_H264 };
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                &input_info, nullptr, &activates, &count);
            if (SUCCEEDED(hr) && count > 0)
                hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_decoder));
            if (activates) {
                for (UINT32 i = 0; i < count; ++i)
                    activates[i]->Release();
                CoTaskMemFree(activates);
            }
        }
        if (FAILED(hr) || !m_decoder)
            return false;

        IMFMediaType* input = nullptr;
        if (FAILED(MFCreateMediaType(&input)) || !input)
            return false;
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = m_decoder->SetInputType(0, input, 0);
        input->Release();
        if (FAILED(hr))
            return false;
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool accept_output_type()
    {
        // The current type is not valid yet. Clearing it is what makes the
        // decoder list NV12 for this camera.
        m_decoder->SetOutputType(0, nullptr, 0);
        IMFMediaType* fallback = nullptr;
        for (DWORD index = 0; index < 8; ++index) {
            IMFMediaType* type = nullptr;
            if (FAILED(m_decoder->GetOutputAvailableType(0, index, &type)))
                break;
            GUID subtype = GUID_NULL;
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_RGB32) {
                const HRESULT hr = m_decoder->SetOutputType(0, type, 0);
                type->Release();
                if (fallback)
                    fallback->Release();
                return SUCCEEDED(hr);
            }
            if (!fallback)
                fallback = type;
            else
                type->Release();
        }
        const bool ok = fallback && SUCCEEDED(m_decoder->SetOutputType(0, fallback, 0));
        if (fallback)
            fallback->Release();
        return ok;
    }

    bool decode(const std::string& annexb, bool keyframe, PreviewState& state)
    {
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(DWORD(annexb.size()), &buffer)))
            return false;
        BYTE* locked = nullptr;
        buffer->Lock(&locked, nullptr, nullptr);
        memcpy(locked, annexb.data(), annexb.size());
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(annexb.size()));
        sample->AddBuffer(buffer);
        sample->SetSampleTime(m_time);
        sample->SetSampleDuration(333333);
        sample->SetUINT32(MFSampleExtension_CleanPoint, keyframe ? TRUE : FALSE);
        m_time += 333333;
        buffer->Release();
        HRESULT hr = m_decoder->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {
            pull_picture(state);
            hr = m_decoder->ProcessInput(0, sample, 0);
        }
        sample->Release();
        if (FAILED(hr))
            return false;
        return pull_picture(state);
    }

    bool pull_picture(PreviewState& state)
    {

        for (int guard = 0; guard < 8; ++guard) {
            MFT_OUTPUT_STREAM_INFO info {};
            m_decoder->GetOutputStreamInfo(0, &info);
            MFT_OUTPUT_DATA_BUFFER output {};
            IMFSample* provided = nullptr;
            IMFMediaBuffer* provided_buffer = nullptr;
            const bool owns_sample = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
            if (owns_sample) {
                const DWORD bytes = info.cbSize ? info.cbSize : 1920 * 1088 * 3 / 2;
                if (FAILED(MFCreateSample(&provided)) || FAILED(MFCreateMemoryBuffer(bytes, &provided_buffer)))
                    return false;
                provided->AddBuffer(provided_buffer);
                output.pSample = provided;
            }
            DWORD status = 0;
            const HRESULT hr = m_decoder->ProcessOutput(0, 1, &output, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (provided)
                    provided->Release();
                if (provided_buffer)
                    provided_buffer->Release();
                return true;
            }
            // 0xC00D6D60 is MF_E_TRANSFORM_TYPE_NOT_SET. This decoder returns it
            // until an output type is chosen, and that is not the stream-change code.
            if (hr == MF_E_TRANSFORM_TYPE_NOT_SET || hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (provided)
                    provided->Release();
                if (provided_buffer)
                    provided_buffer->Release();
                if (output.pEvents)
                    output.pEvents->Release();
                if (!accept_output_type())
                    return false;
                continue;
            }
            IMFSample* result = output.pSample ? output.pSample : provided;
            if (FAILED(hr) || !result) {
                if (provided)
                    provided->Release();
                if (provided_buffer)
                    provided_buffer->Release();
                if (output.pEvents)
                    output.pEvents->Release();
                return false;
            }
            IMFMediaType* out_type = nullptr;
            UINT32 width = 0, height = 0, stride = 0;
            GUID subtype = GUID_NULL;
            if (SUCCEEDED(m_decoder->GetOutputCurrentType(0, &out_type))) {
                MFGetAttributeSize(out_type, MF_MT_FRAME_SIZE, &width, &height);
                out_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
                out_type->GetGUID(MF_MT_SUBTYPE, &subtype);
                out_type->Release();
            }
            IMFMediaBuffer* out_buffer = nullptr;
            result->ConvertToContiguousBuffer(&out_buffer);
            BYTE* pixels = nullptr;
            DWORD length = 0;
            if (out_buffer && SUCCEEDED(out_buffer->Lock(&pixels, nullptr, &length)) && width > 0 && height > 0 && length > 0) {
                if (stride == 0 || stride > 8192)
                    stride = width;
                std::vector<unsigned char> rgb;
                if (subtype == MFVideoFormat_RGB32 && length >= width * height * 4) {
                    rgb.resize(static_cast<size_t>(width) * height * 3);
                    for (UINT32 i = 0; i < width * height; ++i) {
                        rgb[i * 3] = pixels[i * 4 + 2];
                        rgb[i * 3 + 1] = pixels[i * 4 + 1];
                        rgb[i * 3 + 2] = pixels[i * 4];
                    }
                } else if (subtype == MFVideoFormat_NV12 || subtype == GUID_NULL) {
                    const int row = stride < int(width) ? int(width) : int(stride);
                    const size_t need = static_cast<size_t>(row) * static_cast<size_t>(height) * 3 / 2;
                    if (length >= need)
                        nv12_to_rgb(pixels, int(width), int(height), row, rgb);
                }
                if (!rgb.empty())
                    state.publish(int(width), int(height), std::move(rgb));
                out_buffer->Unlock();
            }
            if (out_buffer)
                out_buffer->Release();
            if (output.pSample && output.pSample != provided)
                output.pSample->Release();
            if (provided)
                provided->Release();
            if (provided_buffer)
                provided_buffer->Release();
            if (output.pEvents)
                output.pEvents->Release();
            return true;
        }
        return true;
    }

    void reset()
    {
        if (m_decoder) {
            m_decoder->Release();
            m_decoder = nullptr;
        }
        if (m_started) {
            MFShutdown();
            m_started = false;
        }
    }

    IMFTransform* m_decoder { nullptr };
    bool m_started { false };
    std::string m_parameter_sets;
    int m_length_size { 4 };
    LONGLONG m_time { 0 };
};

void read_camera(const std::shared_ptr<PreviewState>& state)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto finish_com = [&]() {
        if (SUCCEEDED(com))
            CoUninitialize();
    };
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        state->fail("The camera connection could not be opened on this computer.");
        finish_com();
        return;
    }
    addrinfo hints {};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(state->host.c_str(), "18088", &hints, &result) != 0) {
        state->fail("The printer's camera address could not be found.");
        finish_com();
        return;
    }
    SOCKET socket = INVALID_SOCKET;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        if (item->ai_family != AF_INET)
            continue;
        socket = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (socket == INVALID_SOCKET)
            continue;
        u_long nonblocking = 1;
        ioctlsocket(socket, FIONBIO, &nonblocking);
        const int connected = ::connect(socket, item->ai_addr, static_cast<int>(item->ai_addrlen));
        if (connected != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(socket);
            socket = INVALID_SOCKET;
            continue;
        }
        fd_set writers;
        FD_ZERO(&writers);
        FD_SET(socket, &writers);
        timeval wait {};
        wait.tv_sec = 4;
        if (select(0, nullptr, &writers, nullptr, &wait) <= 0) {
            closesocket(socket);
            socket = INVALID_SOCKET;
            continue;
        }
        int socket_error = 0;
        int length = sizeof(socket_error);
        getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length);
        nonblocking = 0;
        ioctlsocket(socket, FIONBIO, &nonblocking);
        if (socket_error != 0) {
            closesocket(socket);
            socket = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(result);
    if (socket == INVALID_SOCKET) {
        state->fail("The printer did not open its camera. Leave LAN Mode on, then press Start again.");
        finish_com();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->socket_mutex);
        state->socket = socket;
    }
    const DWORD timeout = 2000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const std::string request = "GET /flv HTTP/1.1\r\nHost: " + state->host + ":18088\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size()) {
        const int wrote = ::send(socket, request.data() + sent, int(request.size() - sent), 0);
        if (wrote <= 0) {
            state->fail("The printer did not open its camera. Leave LAN Mode on, then press Start again.");
            closesocket(socket);
            finish_com();
            return;
        }
        sent += size_t(wrote);
    }

    std::string pending;
    bool header_done = false;
    bool stream_ready = false;
    H264Pump pump;
    bool saw_video = false;
    auto consume = [&](std::string& body) {
        while (body.size() >= 15 && !state->stop) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(body.data());
            const unsigned tag = bytes[0];
            const size_t data_size = (size_t(bytes[1]) << 16) | (size_t(bytes[2]) << 8) | size_t(bytes[3]);
            const size_t total = 11 + data_size + 4;
            if (data_size > 2 * 1024 * 1024 || total < 15) {
                body.erase(0, 1);
                continue;
            }
            if (body.size() < total)
                return;
            if (tag == 9 && data_size > 5) {
                const unsigned char* video = bytes + 11;
                const unsigned codec = video[0] & 0x0f;
                const unsigned packet = video[1];
                const bool keyframe = (video[0] >> 4) == 1;
                bool decoded = true;
                if (codec == 7 && packet == 0)
                    decoded = pump.feed_sequence(video + 5, data_size - 5);
                else if (codec == 7 && packet == 1) {
                    saw_video = true;
                    decoded = pump.feed_nalus(video + 5, data_size - 5, keyframe, *state);
                }
                if (!decoded)
                    state->fail("This computer could not decode the camera picture.");
            }
            body.erase(0, total);
        }
    };

    char chunk[16384];
    while (!state->stop) {
        const int got = recv(socket, chunk, sizeof(chunk), 0);
        if (got == 0)
            break;
        if (got < 0) {
            if (WSAGetLastError() == WSAETIMEDOUT)
                continue;
            break;
        }
        pending.append(chunk, size_t(got));
        if (!header_done) {
            const size_t split = pending.find("\r\n\r\n");
            if (split == std::string::npos) {
                if (pending.size() > 65536)
                    pending.erase(0, pending.size() - 4096);
                continue;
            }
            const std::string headers = pending.substr(0, split);
            pending.erase(0, split + 4);
            header_done = true;
            if (headers.find(" 200 ") == std::string::npos && headers.find(" 200\r") == std::string::npos) {
                state->fail("The printer answered, but it did not start a camera picture.");
                break;
            }
        }
        if (header_done && !stream_ready) {
            const size_t flv = pending.find("FLV");
            if (flv == std::string::npos || flv + 9 > pending.size())
                continue;
            const auto* flv_bytes = reinterpret_cast<const unsigned char*>(pending.data() + flv);
            const size_t offset = (size_t(flv_bytes[5]) << 24) | (size_t(flv_bytes[6]) << 16) | (size_t(flv_bytes[7]) << 8) | size_t(flv_bytes[8]);
            if (flv + offset + 4 > pending.size())
                continue;
            pending.erase(0, flv + offset + 4);
            stream_ready = true;
        }
        if (stream_ready)
            consume(pending);
    }
    {
        std::lock_guard<std::mutex> lock(state->socket_mutex);
        if (state->socket != INVALID_SOCKET) {
            closesocket(state->socket);
            state->socket = INVALID_SOCKET;
        }
    }
    if (!state->stop && !saw_video)
        state->fail("The camera started, but no picture arrived.");
    finish_com();
}

} // namespace

struct AnycubicPreview::State : PreviewState {};

AnycubicPreview::AnycubicPreview(std::string host)
    : m_state(std::make_shared<State>())
{
    m_state->host = printer_address(std::move(host));
}

AnycubicPreview::~AnycubicPreview()
{
    stop();
}

void AnycubicPreview::start()
{
    stop();
    m_state->stop = false;
    auto state = m_state;
    m_state->thread = std::thread([state]() { read_camera(state); });
}

void AnycubicPreview::stop()
{
    if (!m_state)
        return;
    m_state->stop = true;
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(m_state->socket_mutex);
        socket = m_state->socket;
    }
    if (socket != INVALID_SOCKET)
        shutdown(socket, SD_BOTH);
    if (m_state->thread.joinable())
        m_state->thread.join();
}

bool AnycubicPreview::copy_picture(AnycubicPicture& picture)
{
    std::lock_guard<std::mutex> lock(m_state->picture_mutex);
    if (picture.serial == m_state->serial || m_state->width <= 0)
        return false;
    picture.width = m_state->width;
    picture.height = m_state->height;
    picture.serial = m_state->serial;
    picture.rgb = m_state->rgb;
    return true;
}

std::string AnycubicPreview::last_error() const
{
    std::lock_guard<std::mutex> lock(m_state->picture_mutex);
    return m_state->error;
}

#else

struct AnycubicPreview::State {};

AnycubicPreview::AnycubicPreview(std::string host) : m_state(std::make_shared<State>()) { (void)host; }
AnycubicPreview::~AnycubicPreview() = default;
void AnycubicPreview::start() {}
void AnycubicPreview::stop() {}
bool AnycubicPreview::copy_picture(AnycubicPicture&) { return false; }
std::string AnycubicPreview::last_error() const { return "The camera picture is shown on Windows."; }

#endif

} // namespace Slic3r
