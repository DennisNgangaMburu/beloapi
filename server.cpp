// =============================================================================
//  BeloAPI - an unofficial Airbnb JSON API and MCP server, in one C++ file.
// -----------------------------------------------------------------------------
//  Author  : Dennis Nganga Mburu (CrackBoy), Kenya
//  Contact : crackboyshop@proton.me
//  License : MIT - see LICENSE
//
//  This is NOT affiliated with, endorsed by, or supported by Airbnb, Inc.
//  It reads the same public web endpoints the airbnb.com browser client uses.
//  Respect Airbnb's Terms of Service and local law when you use it.
//
//  What this program does
//  ----------------------
//    * Runs a small HTTP server on 127.0.0.1:8787 by default.
//    * GET  /api/*   -> JSON API (search stays, autocomplete, markets, ...).
//    * POST /mcp     -> Model Context Protocol over JSON-RPC 2.0, so AI agents
//                       can call the same functions as tools.
//
//  Design notes
//  ------------
//    * No third-party libraries. On Windows the HTTP client uses WinHTTP; on
//      Linux and macOS it shells out to the `curl` binary.
//    * The HTTP server itself is plain Winsock on Windows and BSD sockets
//      elsewhere, so the same file builds on every desktop platform.
//    * Upstream responses are passed through unchanged, with their original
//      HTTP status. Failures that happen inside BeloAPI (bad input, network
//      error) return a small {"error":{...}} document.
//
//  Build
//  -----
//    Windows (MinGW):  g++ -std=c++17 -O2 -static -o beloapi.exe server.cpp -lwinhttp -lws2_32
//    Linux / macOS:    g++ -std=c++17 -O2 -o beloapi server.cpp -pthread
//    (see docs/SETUP.md and CMakeLists.txt for the packaged builds)
//
//  Run
//  ---
//    beloapi                 start on 127.0.0.1:8787
//    beloapi --port 9000     start on another port
//    beloapi --mcp-stdio     speak MCP over stdin/stdout instead of HTTP
//    beloapi --help          list every option
//    beloapi --test          check every upstream route, then exit
// =============================================================================

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace beloapi {

// =============================================================================
//  Section 1 - Configuration
// =============================================================================

constexpr const char* kName = "BeloAPI";
constexpr const char* kVersion = "1.1.1";
constexpr const char* kAuthor = "Dennis Nganga Mburu (CrackBoy), Kenya";
constexpr const char* kContact = "crackboyshop@proton.me";
constexpr const char* kRepoUrl = "https://github.com/DennisNgangaMburu/beloapi";

// Kept short so it fits in logging and default responses.
constexpr int kDefaultPort = 8787;
constexpr size_t kMaxBodyBytes = 8u * 1024u * 1024u;  // protects the server
constexpr size_t kMaxHeaderBytes = 64u * 1024u;

// Set once from main() before the listening socket opens, then only read.
// Because it never changes after startup, worker threads need no locking.
struct Config {
    std::string host = "127.0.0.1";
    int port = kDefaultPort;
    std::string upstream = "https://www.airbnb.com";

    // Public web API key that airbnb.com ships in its own JavaScript bundle.
    // It is not a secret or a user credential; it identifies the web client.
    std::string api_key = "d306zoyjsyarp7ifhu67rjxn52tv0t20";

    std::string user_agent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

    // Version of the airbnb.com front-end bundle the persisted queries were
    // captured against. Bump it if Airbnb starts rejecting requests.
    std::string client_version = "8925c43afcf34195b8402375ac9845ca021daf17";
};

static Config g_config;
static std::time_t g_started_at = 0;

// =============================================================================
//  Section 2 - Minimal JSON
// -----------------------------------------------------------------------------
//  The server needs three things from JSON: to read MCP arguments, to read the
//  place id out of an autocomplete reply, and to build replies. A small
//  recursive-descent parser is enough and keeps the project dependency-free.
// =============================================================================

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                                  // String value
    std::vector<Json> items;                           // Array value
    std::vector<std::pair<std::string, Json>> members; // Object value

    const Json* find(const std::string& key) const {
        if (type != Object) return nullptr;
        for (const auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }

    // Convenience accessors: return the fallback when absent or of wrong type.
    std::string str(const std::string& key, const std::string& fallback = "") const {
        const Json* v = find(key);
        return (v && v->type == String) ? v->text : fallback;
    }

    bool flag(const std::string& key, bool fallback = false) const {
        const Json* v = find(key);
        return (v && v->type == Bool) ? v->boolean : fallback;
    }
};

namespace json_detail {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    int depth = 0;

    explicit Parser(const std::string& input) : s(input) {}

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }

    bool consume(char c) {
        skip_ws();
        if (i < s.size() && s[i] == c) { i++; return true; }
        return false;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) return false;
            char esc = s[i++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    // UTF-8 encode. Surrogate pairs are passed through as-is;
                    // they only matter for emoji in free text, which we do not
                    // send upstream.
                    if (cp < 0x80) {
                        out.push_back((char)cp);
                    } else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    Json parse_value() {
        Json v;
        if (++depth > 200) { ok = false; return v; }  // guard against hostile input
        skip_ws();
        if (i >= s.size()) { ok = false; return v; }

        char c = s[i];
        if (c == '{') {
            v.type = Json::Object;
            i++;
            skip_ws();
            if (consume('}')) { depth--; return v; }
            for (;;) {
                std::string key;
                if (!parse_string(key)) { ok = false; return v; }
                if (!consume(':')) { ok = false; return v; }
                Json child = parse_value();
                if (!ok) return v;
                v.members.emplace_back(std::move(key), std::move(child));
                if (consume(',')) continue;
                if (consume('}')) break;
                ok = false;
                return v;
            }
        } else if (c == '[') {
            v.type = Json::Array;
            i++;
            skip_ws();
            if (consume(']')) { depth--; return v; }
            for (;;) {
                Json child = parse_value();
                if (!ok) return v;
                v.items.push_back(std::move(child));
                if (consume(',')) continue;
                if (consume(']')) break;
                ok = false;
                return v;
            }
        } else if (c == '"') {
            v.type = Json::String;
            if (!parse_string(v.text)) ok = false;
        } else if (c == 't' && s.compare(i, 4, "true") == 0) {
            v.type = Json::Bool; v.boolean = true; i += 4;
        } else if (c == 'f' && s.compare(i, 5, "false") == 0) {
            v.type = Json::Bool; v.boolean = false; i += 5;
        } else if (c == 'n' && s.compare(i, 4, "null") == 0) {
            v.type = Json::Null; i += 4;
        } else {
            // Number. strtod accepts leading '-' and exponents.
            char* end = nullptr;
            double d = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) { ok = false; return v; }
            v.type = Json::Number;
            v.number = d;
            i = (size_t)(end - s.c_str());
        }
        depth--;
        return v;
    }
};

}  // namespace json_detail

// Parse a complete JSON document. Returns a Null value and sets `ok=false` on
// malformed input; callers should check `ok` before using the result.
inline Json json_parse(const std::string& text, bool* ok = nullptr) {
    json_detail::Parser p(text);
    Json v = p.parse_value();
    p.skip_ws();
    bool good = p.ok && p.i == text.size();
    if (ok) *ok = good;
    return good ? v : Json();
}

// Escape a string for embedding in a JSON document.
inline std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

// Quoted JSON string.
inline std::string quote(const std::string& s) { return "\"" + json_escape(s) + "\""; }

// Render a Json value back to text. Used to echo JSON-RPC ids unchanged.
inline std::string json_dump(const Json& v) {
    switch (v.type) {
        case Json::Bool: return v.boolean ? "true" : "false";
        case Json::Number: {
            char buf[40];
            // %g keeps integers short; precision is ample for request ids.
            std::snprintf(buf, sizeof(buf), "%.17g", v.number);
            return buf;
        }
        case Json::String: return quote(v.text);
        case Json::Array: {
            std::string out = "[";
            for (size_t i = 0; i < v.items.size(); i++) {
                if (i) out += ",";
                out += json_dump(v.items[i]);
            }
            return out + "]";
        }
        case Json::Object: {
            std::string out = "{";
            for (size_t i = 0; i < v.members.size(); i++) {
                if (i) out += ",";
                out += quote(v.members[i].first) + ":" + json_dump(v.members[i].second);
            }
            return out + "}";
        }
        case Json::Null:
        default: return "null";
    }
}

// =============================================================================
//  Section 3 - String and query helpers
// =============================================================================

// Percent-encode a value for use in a query string.
inline std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Decode %XX and '+' from a query-string component.
inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() &&
                   std::isxdigit((unsigned char)s[i + 1]) &&
                   std::isxdigit((unsigned char)s[i + 2])) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                return (std::tolower((unsigned char)c) - 'a') + 10;
            };
            out.push_back((char)(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

using Params = std::map<std::string, std::string>;

// Turn "a=1&b=two%20words" into a map. First occurrence of a key wins.
inline Params parse_query(const std::string& query) {
    Params out;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string key = url_decode(pair.substr(0, eq));
            std::string val = (eq == std::string::npos) ? "" : url_decode(pair.substr(eq + 1));
            if (!key.empty() && out.find(key) == out.end()) out[key] = val;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

// Read a parameter under any of several aliases, or "" when none is present.
inline std::string pick(const Params& p, std::initializer_list<const char*> keys,
                        const std::string& fallback = "") {
    for (const char* k : keys) {
        auto it = p.find(k);
        if (it != p.end() && !it->second.empty()) return it->second;
    }
    return fallback;
}

// "2026-12-10" shape check; Airbnb expects ISO dates.
inline bool is_iso_date(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// Keep numeric query values from reaching upstream as junk.
inline std::string sanitize_count(const std::string& s, const std::string& fallback) {
    if (s.empty()) return fallback;
    for (char c : s)
        if (!std::isdigit((unsigned char)c)) return fallback;
    return s;
}

// The HTTP date form used for the Date response header.
inline std::string http_date() {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &now);
#else
    gmtime_r(&now, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return buf;
}

// =============================================================================
//  Section 4 - Outbound HTTP client
// -----------------------------------------------------------------------------
//  One function, `http_request`, hides the platform difference. Everything
//  upstream goes through it.
// =============================================================================

struct ApiResult {
    int status = 0;
    std::string body;
    std::string error;  // non-empty means the request never reached Airbnb

    bool ok() const { return error.empty(); }
};

// Split "https://host:port/path?q" into its parts.
struct UrlParts {
    bool https = true;
    std::string host;
    int port = 443;
    std::string path = "/";
};

inline UrlParts parse_url(const std::string& url) {
    UrlParts u;
    size_t scheme_end = url.find("://");
    std::string rest = url;
    if (scheme_end != std::string::npos) {
        u.https = url.compare(0, scheme_end, "https") == 0;
        rest = url.substr(scheme_end + 3);
    }
    size_t slash = rest.find_first_of("/?#");
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (u.path.empty() || u.path[0] != '/') u.path.insert(u.path.begin(), '/');

    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = std::atoi(authority.c_str() + colon + 1);
        if (u.port <= 0) u.port = u.https ? 443 : 80;
    } else {
        u.host = authority;
        u.port = u.https ? 443 : 80;
    }
    return u;
}

#ifdef _WIN32

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// WinHTTP implementation. WinHTTP ships with every supported Windows version,
// so the Windows build has no external dependencies at all.
inline ApiResult http_request(const std::string& method, const std::string& url,
                              const std::vector<std::string>& headers,
                              const std::string& body) {
    ApiResult r;
    UrlParts parts = parse_url(url);
    if (parts.host.empty()) {
        r.error = "invalid upstream URL";
        return r;
    }

    HINTERNET session = WinHttpOpen(L"BeloAPI/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.error = "WinHttpOpen failed";
        return r;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 60000);

    HINTERNET connection = WinHttpConnect(session, widen(parts.host).c_str(),
                                          (INTERNET_PORT)parts.port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        r.error = "could not connect to " + parts.host;
        return r;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection, widen(method).c_str(), widen(parts.path).c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        r.error = "WinHttpOpenRequest failed";
        return r;
    }

    for (const auto& h : headers) {
        std::wstring wh = widen(h);
        WinHttpAddRequestHeaders(request, wh.c_str(), (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    BOOL sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);
    if (sent) sent = WinHttpReceiveResponse(request, nullptr);  // follows redirects

    if (sent) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)code;

        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk((size_t)available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) break;
            chunk.resize(read);
            r.body += chunk;
        }
    } else {
        r.error = "request to www.airbnb.com failed";
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return r;
}

#else  // ------------------------- Linux / macOS ----------------------------

// Quote one shell argument safely: wrap in single quotes and escape any quote.
inline std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

// POSIX implementation. There is no TLS stack in the C++ standard library and
// this project refuses third-party dependencies, so the `curl` binary (present
// on every mainstream Linux and macOS install) performs the transfer. Anyone
// who wants a native client can replace this one function with libcurl; see
// CONTRIBUTING.md.
inline ApiResult http_request(const std::string& method, const std::string& url,
                              const std::vector<std::string>& headers,
                              const std::string& body) {
    ApiResult r;
    std::string body_file;

    if (!body.empty()) {
        char path_template[] = "/tmp/beloapi_body_XXXXXX";
        int fd = mkstemp(path_template);
        if (fd < 0) {
            r.error = "could not create a temporary file";
            return r;
        }
        body_file = path_template;
        size_t written = 0;
        while (written < body.size()) {
            ssize_t n = write(fd, body.data() + written, body.size() - written);
            if (n <= 0) break;
            written += (size_t)n;
        }
        close(fd);
        if (written != body.size()) {
            std::remove(body_file.c_str());
            r.error = "could not write the request body";
            return r;
        }
    }

    std::string command = "curl -sS --max-time 90 --compressed -X " + shell_quote(method);
    for (const auto& h : headers) command += " -H " + shell_quote(h);
    if (!body_file.empty()) command += " --data-binary @" + shell_quote(body_file);
    command += " -w " + shell_quote("\n__BELOAPI_STATUS__%{http_code}") + " " + shell_quote(url);
    command += " 2>/dev/null";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        if (!body_file.empty()) std::remove(body_file.c_str());
        r.error = "could not run curl (is it installed?)";
        return r;
    }

    std::string output;
    char buffer[8192];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, got);
    int exit_code = pclose(pipe);

    if (!body_file.empty()) std::remove(body_file.c_str());

    const std::string marker = "__BELOAPI_STATUS__";
    size_t marker_at = output.rfind(marker);
    if (marker_at == std::string::npos) {
        r.error = "curl failed (exit " + std::to_string(exit_code) + ")";
        return r;
    }

    r.status = std::atoi(output.c_str() + marker_at + marker.size());
    r.body = output.substr(0, marker_at);
    if (!r.body.empty() && r.body.back() == '\n') r.body.pop_back();
    if (r.status == 0) r.error = "curl failed (exit " + std::to_string(exit_code) + ")";
    return r;
}

#endif

// =============================================================================
//  Section 5 - Airbnb upstream calls
// =============================================================================

// Headers that the airbnb.com web client sends with every XHR. Cookies are
// deliberately never forwarded: this API is anonymous and reads public data.
inline std::vector<std::string> base_headers() {
    return {
        "Accept: application/json",
        "Accept-Language: en-US,en;q=0.9",
        "Content-Type: application/json",
        "User-Agent: " + g_config.user_agent,
        "X-Airbnb-API-Key: " + g_config.api_key,
        "X-Airbnb-GraphQL-Platform: web",
        "X-Airbnb-GraphQL-Platform-Client: minimalist-niobe",
        "X-Airbnb-Supports-Airlock-V2: true",
        "X-CSRF-Token: ",
        "X-CSRF-Without-Token: 1",
        "X-Client-Version: " + g_config.client_version,
        "Origin: " + g_config.upstream,
        "Referer: " + g_config.upstream + "/",
    };
}

// Call an upstream path such as "/api/v2/user_markets?locale=en".
inline ApiResult upstream_get(const std::string& path_and_query) {
    return http_request("GET", g_config.upstream + path_and_query, base_headers(), "");
}

inline ApiResult upstream_post(const std::string& path_and_query, const std::string& body) {
    return http_request("POST", g_config.upstream + path_and_query, base_headers(), body);
}

// GraphQL persisted queries are identified by a sha256 hash; the server keeps
// the query text, so only the hash and the variables travel over the wire.
inline std::string graphql_extensions(const std::string& hash) {
    return "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"" + hash + "\"}}";
}

// GET flavour, used by the queries that airbnb.com issues as GET.
inline ApiResult graphql_get(const std::string& operation, const std::string& hash,
                             const std::string& variables, const std::string& locale,
                             const std::string& currency) {
    std::string path = "/api/v3/" + operation + "/" + hash;
    path += "?operationName=" + url_encode(operation);
    path += "&locale=" + url_encode(locale);
    path += "&currency=" + url_encode(currency);
    path += "&variables=" + url_encode(variables);
    path += "&extensions=" + url_encode(graphql_extensions(hash));
    return upstream_get(path);
}

// POST flavour, used by the search and homepage queries.
inline ApiResult graphql_post(const std::string& operation, const std::string& hash,
                              const std::string& variables, const std::string& locale,
                              const std::string& currency) {
    std::string path = "/api/v3/" + operation + "/" + hash;
    path += "?operationName=" + url_encode(operation);
    path += "&locale=" + url_encode(locale);
    path += "&currency=" + url_encode(currency);

    std::string body = "{\"operationName\":" + quote(operation) +
                       ",\"variables\":" + variables +
                       ",\"extensions\":" + graphql_extensions(hash) + "}";
    return upstream_post(path, body);
}

// Persisted-query hashes captured from the live site. If Airbnb rotates one,
// the matching route starts returning a GraphQL error and the hash in
// server.cpp needs refreshing from a browser network log.
constexpr const char* kHashStaysSearch =
    "721e300ca0bb1d5b980854763b64af3e53a7c2eb05bc6eb8c9cc032f835e2dfb";
constexpr const char* kHashTabbedHomepage =
    "8e185d44aa21af6f5a0aa8ed968225ab47e528e0ef2206b0f82b116af13d822d";
constexpr const char* kHashAnnouncements =
    "8b2c445223b0e7dd51309008c0b10a0a0de8e3c0ea9821c37061241be54efdd8";
constexpr const char* kHashPromotions =
    "0096005e4cc36a5dfb934e96ff1413f3e7fbeeb28563647888046a6d57324eae";
constexpr const char* kHashHeader =
    "997186c1a85e5e3887f961a60f9cf37f45caeb93100564c22626996e7e2a9147";
constexpr const char* kHashConsent =
    "2c65c7d0b25c6d6c5a6a46b41d7bb87f158fcfd3b18294c038ecd090b1b99e15";

// =============================================================================
//  Section 6 - API routes
// -----------------------------------------------------------------------------
//  Every handler takes the parsed query/arguments and returns an ApiResult.
//  The HTTP layer and the MCP tools call exactly the same handlers.
// =============================================================================

// Build a BeloAPI-side error document (never an upstream pass-through).
inline ApiResult local_error(int status, const std::string& code, const std::string& message) {
    ApiResult r;
    r.status = status;
    r.body = "{\"error\":{\"code\":" + quote(code) + ",\"message\":" + quote(message) +
             ",\"docs\":" + quote(kRepoUrl) + "}}";
    return r;
}

// Both helpers find a good default so a bare request still works.
inline std::string locale_of(const Params& p) { return pick(p, {"locale", "language"}, "en"); }
inline std::string currency_of(const Params& p) { return pick(p, {"currency"}, "USD"); }

// --- Autocomplete -----------------------------------------------------------
// Symbol source: /api/v2/autocompletes-personalized
inline ApiResult handle_autocomplete(const Params& p) {
    std::string query = pick(p, {"query", "q", "user_input", "location"});
    if (query.empty())
        return local_error(400, "missing_parameter", "Please pass ?query=, for example ?query=Mombasa");

    std::string path = "/api/v2/autocompletes-personalized";
    path += "?locale=" + url_encode(locale_of(p));
    path += "&currency=" + url_encode(currency_of(p));
    path += "&country=" + url_encode(pick(p, {"country"}, "US"));
    path += "&key=" + url_encode(g_config.api_key);
    path += "&language=" + url_encode(locale_of(p));
    path += "&num_results=" + url_encode(sanitize_count(pick(p, {"num_results", "limit"}), "5"));
    path += "&user_input=" + url_encode(query);
    path += "&api_version=1.2.0";
    return upstream_get(path);
}

// Resolve a free-text place to the place id the search query needs. Airbnb's
// own homepage does the same lookup before searching.
struct PlaceLookup {
    std::string place_id;
    std::string query;
    std::string display_name;
    ApiResult error;
};

inline PlaceLookup resolve_place(const std::string& location, const std::string& locale,
                                 const std::string& currency) {
    PlaceLookup result;
    Params p;
    p["query"] = location;
    p["locale"] = locale;
    p["currency"] = currency;
    ApiResult answer = handle_autocomplete(p);
    result.error = answer;
    if (!answer.ok() || answer.status < 200 || answer.status >= 300) return result;

    bool parsed = false;
    Json root = json_parse(answer.body, &parsed);
    const Json* terms = root.find("autocomplete_terms");
    if (!parsed || !terms || terms->type != Json::Array) return result;

    for (const Json& term : terms->items) {
        const Json* search = term.find("explore_search_params");
        if (!search) continue;
        const Json* id = search->find("place_id");
        if (!id || id->type != Json::String || id->text.empty()) continue;
        result.place_id = id->text;
        result.query = search->str("query", location);
        result.display_name = term.str("display_name", result.query);
        break;
    }
    return result;
}

// Append one {filterName, filterValues} entry to a rawParams array.
inline std::string raw_param(const std::string& name, const std::string& value) {
    return "{\"filterName\":" + quote(name) + ",\"filterValues\":[" + quote(value) + "]}";
}

// --- Stays search -----------------------------------------------------------
// Symbol source: POST /api/v3/StaysSearch/<hash>
inline ApiResult handle_search(const Params& p) {
    std::string location = pick(p, {"location", "query", "destination", "q"});
    std::string place_id = pick(p, {"place_id", "placeId"});
    if (location.empty() && place_id.empty())
        return local_error(400, "missing_parameter",
                           "Please pass ?location=, for example ?location=Mombasa");

    std::string locale = locale_of(p);
    std::string currency = currency_of(p);
    std::string checkin = pick(p, {"checkin", "check_in"});
    std::string checkout = pick(p, {"checkout", "check_out"});
    if (!checkin.empty() && !is_iso_date(checkin))
        return local_error(400, "invalid_parameter", "checkin must look like 2026-12-10");
    if (!checkout.empty() && !is_iso_date(checkout))
        return local_error(400, "invalid_parameter", "checkout must look like 2026-12-12");

    std::string search_query = location;
    if (place_id.empty()) {
        PlaceLookup lookup = resolve_place(location, locale, currency);
        if (!lookup.error.ok())
            return local_error(502, "upstream_unreachable", lookup.error.error);
        if (lookup.error.status < 200 || lookup.error.status >= 300)
            return lookup.error;  // pass Airbnb's own status/body through
        if (lookup.place_id.empty())
            return local_error(404, "location_not_found",
                               "Airbnb has no place matching \"" + location + "\"");
        place_id = lookup.place_id;
        search_query = lookup.query;
    }

    std::vector<std::string> filters;
    filters.push_back(raw_param("cdnCacheSafe", "false"));
    if (!checkin.empty()) filters.push_back(raw_param("checkin", checkin));
    if (!checkout.empty()) filters.push_back(raw_param("checkout", checkout));
    filters.push_back(raw_param("datePickerType", "CALENDAR"));
    filters.push_back(raw_param("adults", sanitize_count(pick(p, {"adults"}), "1")));
    if (!pick(p, {"children"}).empty())
        filters.push_back(raw_param("children", sanitize_count(pick(p, {"children"}), "0")));
    if (!pick(p, {"infants"}).empty())
        filters.push_back(raw_param("infants", sanitize_count(pick(p, {"infants"}), "0")));
    if (!pick(p, {"pets"}).empty())
        filters.push_back(raw_param("pets", sanitize_count(pick(p, {"pets"}), "0")));
    filters.push_back(raw_param("itemsPerGrid", sanitize_count(pick(p, {"items", "limit"}), "18")));
    filters.push_back(raw_param("placeId", place_id));
    if (!search_query.empty()) filters.push_back(raw_param("query", search_query));
    filters.push_back(raw_param("refinementPaths", "/homes"));
    filters.push_back(raw_param("screenSize", "large"));
    filters.push_back(raw_param("tabId", "home_tab"));
    filters.push_back(raw_param("version", "1.8.8"));

    std::string joined;
    for (size_t i = 0; i < filters.size(); i++) {
        if (i) joined += ",";
        joined += filters[i];
    }

    // Both request shapes are required: the page query and the map query.
    std::string request_object =
        "{\"metadataOnly\":false,\"requestedPageType\":\"STAYS_SEARCH\","
        "\"searchType\":\"HOMEPAGE_CAROUSEL_CLICK\",\"treatmentFlags\":[],"
        "\"maxMapItems\":9999,\"rawParams\":[" + joined + "]}";

    std::string variables =
        "{\"staysSearchRequest\":" + request_object +
        ",\"staysMapSearchRequestV2\":" + request_object + "}";

    return graphql_post("StaysSearch", kHashStaysSearch, variables, locale, currency);
}

// --- Homepage ---------------------------------------------------------------
// Symbol source: POST /api/v3/TabbedVerticalHomepage/<hash>
inline ApiResult handle_homepage(const Params& p) {
    std::string vertical = pick(p, {"vertical"}, "STAYS");
    std::string variables =
        "{\"mockIdentifier\":null,\"vertical\":" + quote(vertical) +
        ",\"treatmentFlags\":[\"unified_homepage_nav\",\"hotels_tab_visible\"],"
        "\"promotionOptions\":null}";
    return graphql_post("TabbedVerticalHomepage", kHashTabbedHomepage, variables,
                        locale_of(p), currency_of(p));
}

// --- Markets ----------------------------------------------------------------
// Symbol source: /api/v2/user_markets
inline ApiResult handle_markets(const Params& p) {
    std::string path = "/api/v2/user_markets";
    path += "?locale=" + url_encode(locale_of(p));
    path += "&currency=" + url_encode(currency_of(p));
    path += "&language=" + url_encode(locale_of(p));
    return upstream_get(path);
}

// --- Announcements ----------------------------------------------------------
inline ApiResult handle_announcements(const Params& p) {
    std::string variables = "{\"appTarget\":\"GUEST\",\"shouldSkipTaskFooter\":false}";
    return graphql_get("NaviServerAnnouncementsQuery", kHashAnnouncements, variables,
                       locale_of(p), currency_of(p));
}

// --- Promotions -------------------------------------------------------------
inline ApiResult handle_promotions(const Params& p) {
    std::string placements = pick(p, {"placements", "placement"}, "HOME_PAGE_POPOVER");
    std::string list;
    size_t start = 0;
    while (start <= placements.size()) {
        size_t comma = placements.find(',', start);
        std::string one = placements.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!one.empty()) {
            if (!list.empty()) list += ",";
            list += quote(one);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (list.empty()) list = "\"HOME_PAGE_POPOVER\"";

    std::string variables =
        "{\"placements\":[" + list + "],\"context\":{\"incentiveCamapignContext\":null,"
        "\"hotelTonightContext\":null,\"partnerProgramContext\":null}}";
    return graphql_get("PromotionsByPlacementsQuery", kHashPromotions, variables,
                       locale_of(p), currency_of(p));
}

// --- Header -----------------------------------------------------------------
inline ApiResult handle_header(const Params& p) {
    std::string variables =
        "{\"cdnCacheSafe\":false,\"isInitialLoad\":false,\"source\":\"EXPLORE\","
        "\"supportsM13ListingsSetupFlow\":true}";
    return graphql_get("Header", kHashHeader, variables, locale_of(p), currency_of(p));
}

// --- Consent flags ----------------------------------------------------------
inline ApiResult handle_consent(const Params& p) {
    std::string device_id = pick(p, {"device_id", "deviceId"}, "beloapi-device");
    std::string consent_id = pick(p, {"consent_id", "consentId"}, "beloapi-consent");
    std::string variables = "{\"deviceId\":" + quote(device_id) +
                            ",\"consentId\":" + quote(consent_id) + "}";
    return graphql_get("GetConsentFlagsQuery", kHashConsent, variables,
                       locale_of(p), currency_of(p));
}

// --- Client configs ---------------------------------------------------------
inline ApiResult handle_config(const Params& p) {
    std::string path = "/api/v2/client_configs";
    path += "?locale=" + url_encode(locale_of(p));
    path += "&currency=" + url_encode(currency_of(p));
    std::string body =
        "{\"configs\":[\"user\"],\"userAttributes\":[\"id\",\"preferred_locale\","
        "\"native_currency\"],\"trebuchetKeys\":[]}";
    return upstream_post(path, body);
}

// =============================================================================
//  Section 7 - MCP (Model Context Protocol)
// -----------------------------------------------------------------------------
//  A dependency-free JSON-RPC 2.0 endpoint. Supported methods:
//    initialize, notifications/initialized, ping, tools/list, tools/call.
//  Transports: plain HTTP POST with JSON in and JSON out.
// =============================================================================

struct Tool {
    const char* name;
    const char* description;
    const char* schema;  // JSON Schema for the tool arguments
    ApiResult (*handler)(const Params&);
};

inline const std::vector<Tool>& tool_table() {
    static const std::vector<Tool> tools = {
        {"airbnb_autocomplete",
         "Suggest places and destinations for a free-text query. Returns Airbnb's raw "
         "autocomplete document, including place_id values usable with airbnb_search_stays.",
         R"json({"type":"object","properties":{"query":{"type":"string","description":"Place name, for example Mombasa"},"locale":{"type":"string","description":"Locale such as en or fr"},"currency":{"type":"string","description":"ISO currency such as USD"},"country":{"type":"string","description":"ISO country code such as KE"},"num_results":{"type":"integer","description":"How many suggestions to return (default 5)"}},"required":["query"]})json",
         handle_autocomplete},

        {"airbnb_search_stays",
         "Search Airbnb stays for a destination and date range. Returns the raw StaysSearch "
         "response: listings, prices, ratings and pagination info.",
         R"json({"type":"object","properties":{"location":{"type":"string","description":"Destination, for example Mombasa. Either location or place_id is required"},"place_id":{"type":"string","description":"Google place id from airbnb_autocomplete. Either location or place_id is required"},"checkin":{"type":"string","description":"Check-in date, YYYY-MM-DD"},"checkout":{"type":"string","description":"Check-out date, YYYY-MM-DD"},"adults":{"type":"integer","description":"Number of adults (default 1)"},"children":{"type":"integer"},"infants":{"type":"integer"},"pets":{"type":"integer","description":"1 when travelling with pets"},"items":{"type":"integer","description":"Results per page (default 18). Keep this small: the raw document is large"},"locale":{"type":"string"},"currency":{"type":"string"}},"anyOf":[{"required":["location"]},{"required":["place_id"]}]})json",
         handle_search},

        {"airbnb_markets",
         "Return the markets Airbnb maps the caller's region to, plus the satori token used "
         "for personalised autocomplete.",
         R"json({"type":"object","properties":{"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_markets},

        {"airbnb_homepage",
         "Return the curated homepage feed: destination carousels, cards and promotion sections.",
         R"json({"type":"object","properties":{"vertical":{"type":"string","description":"STAYS, EXPERIENCES or SERVICES"},"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_homepage},

        {"airbnb_announcements",
         "Return the site-wide announcements Airbnb shows in the navigation area.",
         R"json({"type":"object","properties":{"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_announcements},

        {"airbnb_promotions",
         "Return promotions for one or more placement slots.",
         R"json({"type":"object","properties":{"placements":{"type":"string","description":"Comma-separated placements, default HOME_PAGE_POPOVER"},"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_promotions},

        {"airbnb_header",
         "Return the logged-out header menu structure: top-level items and profile menu entries.",
         R"json({"type":"object","properties":{"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_header},

        {"airbnb_consent",
         "Return the consent flags Airbnb would show for a visitor.",
         R"json({"type":"object","properties":{"device_id":{"type":"string"},"consent_id":{"type":"string"},"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_consent},

        {"airbnb_client_config",
         "Return the client configuration document (a timestamp and feature flags) for a visitor.",
         R"json({"type":"object","properties":{"locale":{"type":"string"},"currency":{"type":"string"}},"required":[]})json",
         handle_config},
    };
    return tools;
}

// Convert MCP tool arguments into the string map the handlers expect.
inline Params params_from_json(const Json& args) {
    Params out;
    if (args.type != Json::Object) return out;
    for (const auto& member : args.members) {
        const Json& v = member.second;
        switch (v.type) {
            case Json::String: out[member.first] = v.text; break;
            case Json::Bool: out[member.first] = v.boolean ? "true" : "false"; break;
            case Json::Number: {
                double n = v.number;
                if (n == (double)(long long)n) out[member.first] = std::to_string((long long)n);
                else out[member.first] = std::to_string(n);
                break;
            }
            case Json::Array: {
                std::string list;
                for (const Json& item : v.items) {
                    if (item.type != Json::String) continue;
                    if (!list.empty()) list += ",";
                    list += item.text;
                }
                out[member.first] = list;
                break;
            }
            default: break;
        }
    }
    return out;
}

inline std::string rpc_result(const std::string& id, const std::string& result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}";
}

inline std::string rpc_error(const std::string& id, int code, const std::string& message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" +
           std::to_string(code) + ",\"message\":" + quote(message) + "}}";
}

// Handle one JSON-RPC message. `empty_reply` signals a notification (the HTTP
// layer answers 202 with no body).
inline std::string mcp_process(const std::string& body, bool* empty_reply = nullptr) {
    if (empty_reply) *empty_reply = false;

    bool parsed = false;
    Json request = json_parse(body, &parsed);
    if (!parsed || request.type != Json::Object) return rpc_error("null", -32700, "Parse error");

    const Json* id_node = request.find("id");
    if (!id_node) {
        if (empty_reply) *empty_reply = true;  // notification, nothing to answer
        return "";
    }
    std::string id = json_dump(*id_node);

    std::string method = request.str("method");
    const Json* params = request.find("params");

    if (method == "initialize") {
        std::string protocol = "2024-11-05";
        if (params) {
            const Json* requested = params->find("protocolVersion");
            if (requested && requested->type == Json::String && !requested->text.empty())
                protocol = requested->text;
        }
        std::string result =
            "{\"protocolVersion\":" + quote(protocol) +
            ",\"capabilities\":{\"tools\":{\"listChanged\":false}},"
            "\"serverInfo\":{\"name\":\"beloapi\",\"title\":\"BeloAPI\",\"version\":\"" +
            std::string(kVersion) + "\"},"
            "\"instructions\":\"Unofficial Airbnb data for AI agents. Start with "
            "airbnb_autocomplete to turn a place name into a place_id, then call "
            "airbnb_search_stays. Responses are the raw Airbnb JSON documents.\"}";
        return rpc_result(id, result);
    }

    if (method == "ping") return rpc_result(id, "{}");

    if (method == "tools/list") {
        std::string result = "{\"tools\":[";
        const auto& tools = tool_table();
        for (size_t i = 0; i < tools.size(); i++) {
            if (i) result += ",";
            result += "{\"name\":" + quote(tools[i].name) +
                      ",\"description\":" + quote(tools[i].description) +
                      ",\"inputSchema\":" + tools[i].schema + "}";
        }
        result += "]}";
        return rpc_result(id, result);
    }

    if (method == "tools/call") {
        std::string name = params ? params->str("name") : "";
        if (name.empty()) return rpc_error(id, -32602, "tools/call needs a tool name");

        Json arguments;
        if (params) {
            const Json* args = params->find("arguments");
            if (args) arguments = *args;
        }

        for (const Tool& tool : tool_table()) {
            if (name != tool.name) continue;
            ApiResult answer = tool.handler(params_from_json(arguments));
            bool failed = !answer.ok() || answer.status < 200 || answer.status >= 300;
            std::string payload;
            if (!answer.ok()) {
                payload = "{\"error\":{\"code\":\"upstream_unreachable\",\"message\":" +
                          quote(answer.error) + "}}";
            } else {
                payload = answer.body;
            }
            std::string result =
                "{\"content\":[{\"type\":\"text\",\"text\":" + quote(payload) + "}],"
                "\"isError\":" + (failed ? "true" : "false") + "}";
            return rpc_result(id, result);
        }
        return rpc_error(id, -32602, "Unknown tool: " + name);
    }

    return rpc_error(id, -32601, "Method not found: " + method);
}

// MCP over stdio: most desktop AI clients (Claude Desktop, Cursor, VS Code,
// LM Studio) launch a server as a child process and exchange one JSON-RPC
// message per line over stdin/stdout. stdout must carry protocol traffic only,
// so the ready line and every diagnostic go to stderr.
// Reads stdin with C stdio rather than <iostream>: the iostream and locale
// machinery would add about 2 MB to the statically linked release binary for
// no gain here.
inline int run_mcp_stdio() {
    std::fprintf(stderr, "%s %s MCP stdio server ready\n", kName, kVersion);
    std::fflush(stderr);

    auto handle = [](std::string& message) {
        while (!message.empty() && (message.back() == '\r' || message.back() == ' ' ||
                                    message.back() == '\t' || message.back() == '\n'))
            message.pop_back();
        if (message.empty()) return;

        bool notification = false;
        std::string reply = mcp_process(message, &notification);
        if (notification || reply.empty()) return;  // nothing to say back

        std::fwrite(reply.data(), 1, reply.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };

    std::string pending;
    char buffer[65536];
    while (std::fgets(buffer, (int)sizeof(buffer), stdin)) {
        pending += buffer;
        if (pending.back() == '\n') {
            handle(pending);
            pending.clear();
        }
    }
    handle(pending);  // final message may arrive without a trailing newline
    return 0;
}

// =============================================================================
//  Section 8 - HTTP server (platform sockets)
// =============================================================================

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;  // keys lower-cased
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

inline void close_socket(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

inline bool send_all(socket_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

inline const char* status_text(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        default: return "OK";
    }
}

// Serialise an HttpResponse into a wire-format HTTP/1.1 message.
inline std::string build_response(const HttpResponse& res) {
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " +
                      status_text(res.status) + "\r\n";
    out += "Content-Type: " + res.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "Date: " + http_date() + "\r\n";
    out += "Server: BeloAPI/" + std::string(kVersion) + "\r\n";
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id\r\n";
    out += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    for (const auto& h : res.extra_headers) out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    out += res.body;
    return out;
}

// Read request bytes until the header terminator, then until Content-Length.
// Returns false on malformed or oversized input.
inline bool read_http_request(socket_t client, HttpRequest& req, int& parse_status) {
    std::string raw;
    char buffer[8192];
    size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        int got = recv(client, buffer, (int)sizeof(buffer), 0);
        if (got <= 0) return false;
        raw.append(buffer, (size_t)got);
        header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos && raw.size() > kMaxHeaderBytes) {
            parse_status = 413;
            return false;
        }
    }

    std::string head = raw.substr(0, header_end);
    size_t line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t question = target.find('?');
    req.path = (question == std::string::npos) ? target : target.substr(0, question);
    req.query = (question == std::string::npos) ? "" : target.substr(question + 1);

    // Headers, one per line, names lower-cased.
    size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t eol = head.find("\r\n", pos);
        std::string line = head.substr(pos, eol == std::string::npos ? std::string::npos
                                                                     : eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            for (char& c : name) c = (char)std::tolower((unsigned char)c);
            req.headers[name] = value;
        }
        if (eol == std::string::npos) break;
        pos = eol + 2;
    }

    size_t content_length = 0;
    auto length_it = req.headers.find("content-length");
    if (length_it != req.headers.end()) content_length = (size_t)std::strtoull(length_it->second.c_str(), nullptr, 10);
    if (content_length > kMaxBodyBytes) {
        parse_status = 413;
        return false;
    }

    req.body = raw.substr(header_end + 4);
    while (req.body.size() < content_length) {
        int got = recv(client, buffer, (int)sizeof(buffer), 0);
        if (got <= 0) break;
        req.body.append(buffer, (size_t)got);
    }
    if (req.body.size() > content_length) req.body.resize(content_length);
    return true;
}

// Wrap an upstream answer: pass the document and status through; turn a
// transport failure into a 502 with a BeloAPI error document.
inline HttpResponse from_upstream(const ApiResult& answer) {
    HttpResponse res;
    if (!answer.ok()) {
        res.status = 502;
        res.body = "{\"error\":{\"code\":\"upstream_unreachable\",\"message\":" +
                   quote(answer.error) + "}}";
        return res;
    }
    res.status = answer.status;
    res.body = answer.body;
    return res;
}

inline HttpResponse json_response(int status, const std::string& body) {
    HttpResponse res;
    res.status = status;
    res.body = body;
    return res;
}

// Small landing document, also useful as a smoke test.
inline std::string index_document() {
    std::string body = "{\"name\":" + quote(kName) + ",\"version\":" + quote(kVersion) +
                       ",\"description\":\"Unofficial Airbnb JSON API and MCP server\","
                       "\"author\":" + quote(kAuthor) +
                       ",\"contact\":" + quote(kContact) +
                       ",\"disclaimer\":\"Not affiliated with Airbnb, Inc.\","
                       "\"endpoints\":["
                       "\"GET /api/health\","
                       "\"GET /api/autocomplete?query=\","
                       "\"GET /api/search?location=&checkin=&checkout=&adults=\","
                       "\"GET /api/markets\","
                       "\"GET /api/homepage\","
                       "\"GET /api/announcements\","
                       "\"GET /api/promotions\","
                       "\"GET /api/header\","
                       "\"GET /api/consent\","
                       "\"GET /api/config\","
                       "\"GET /openapi.json\","
                       "\"POST /mcp\"],"
                                                        "\"mcp\":{\"transports\":[\"http\",\"stdio\"],\"protocolVersion\":\"2024-11-05\"},"
                                 "\"repository\":" + quote(kRepoUrl) + "}";
    return body;
}

// OpenAPI 3.1 description of the HTTP API. Kept next to the router so the two
// are edited together; `--test` parses it to catch syntax mistakes.
inline std::string openapi_document() {
    static const char* kBody = R"json({
  "openapi": "3.1.0",
  "info": {
    "title": "BeloAPI",
    "version": "__VERSION__",
    "summary": "Unofficial Airbnb JSON API and MCP server in a single C++ file.",
    "description": "Read-only access to the public Airbnb web documents: stay search, destination suggestions, markets, homepage, announcements, promotions, header, consent and client configuration. Responses are the raw Airbnb JSON, passed through with their original HTTP status.",
    "license": { "name": "MIT", "identifier": "MIT" },
    "contact": { "name": "Dennis Nganga Mburu (CrackBoy)", "email": "crackboyshop@proton.me" }
  },
  "servers": [
    { "url": "http://127.0.0.1:8787", "description": "Local instance. BeloAPI binds to loopback only." }
  ],
  "tags": [
    { "name": "stays", "description": "Stay search and destinations" },
    { "name": "site", "description": "Site chrome, consent and configuration" },
    { "name": "mcp", "description": "Model Context Protocol endpoint" }
  ],
  "paths": {
    "/": {
      "get": {
        "tags": ["site"],
        "summary": "Service index",
        "operationId": "getIndex",
        "responses": {
          "200": {
            "description": "Name, version, endpoint list and repository",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Index" } } }
          }
        }
      }
    },
    "/api/health": {
      "get": {
        "tags": ["site"],
        "summary": "Health and version",
        "operationId": "getHealth",
        "responses": {
          "200": {
            "description": "Server status and uptime",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Health" } } }
          }
        }
      }
    },
    "/api/autocomplete": {
      "get": {
        "tags": ["stays"],
        "summary": "Suggest destinations for free text",
        "description": "Returns Airbnb's autocomplete document. Each term carries explore_search_params.place_id, which can be passed to /api/search as place_id.",
        "operationId": "autocomplete",
        "parameters": [
          { "$ref": "#/components/parameters/Query" },
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" },
          { "$ref": "#/components/parameters/Country" },
          { "$ref": "#/components/parameters/NumResults" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "400": { "$ref": "#/components/responses/BadRequest" },
          "404": { "$ref": "#/components/responses/NotFound" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/search": {
      "get": {
        "tags": ["stays"],
        "summary": "Search stays for a destination and date range",
        "description": "Returns the raw StaysSearch document: listings, total stay prices, ratings, coordinates, photo ids and pagination. Supply location, or a place_id from /api/autocomplete. The document is large (roughly 140-260 KB), so set items low when you only need a few results.",
        "operationId": "searchStays",
        "parameters": [
          { "$ref": "#/components/parameters/Location" },
          { "$ref": "#/components/parameters/PlaceId" },
          { "$ref": "#/components/parameters/Checkin" },
          { "$ref": "#/components/parameters/Checkout" },
          { "$ref": "#/components/parameters/Adults" },
          { "$ref": "#/components/parameters/Children" },
          { "$ref": "#/components/parameters/Infants" },
          { "$ref": "#/components/parameters/Pets" },
          { "$ref": "#/components/parameters/Items" },
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "400": { "$ref": "#/components/responses/BadRequest" },
          "404": { "$ref": "#/components/responses/NotFound" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/markets": {
      "get": {
        "tags": ["site"],
        "summary": "Markets for the caller region",
        "operationId": "getMarkets",
        "parameters": [
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/homepage": {
      "get": {
        "tags": ["site"],
        "summary": "Curated homepage feed",
        "operationId": "getHomepage",
        "parameters": [
          { "$ref": "#/components/parameters/Vertical" },
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/announcements": {
      "get": {
        "tags": ["site"],
        "summary": "Site-wide announcements",
        "operationId": "getAnnouncements",
        "parameters": [
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/promotions": {
      "get": {
        "tags": ["site"],
        "summary": "Promotions for placement slots",
        "operationId": "getPromotions",
        "parameters": [
          { "$ref": "#/components/parameters/Placements" },
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/header": {
      "get": {
        "tags": ["site"],
        "summary": "Logged-out header menu",
        "operationId": "getHeader",
        "parameters": [
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/consent": {
      "get": {
        "tags": ["site"],
        "summary": "Consent flags for a visitor",
        "operationId": "getConsent",
        "parameters": [
          { "$ref": "#/components/parameters/DeviceId" },
          { "$ref": "#/components/parameters/ConsentId" },
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/api/config": {
      "get": {
        "tags": ["site"],
        "summary": "Client configuration and feature flags",
        "operationId": "getConfig",
        "parameters": [
          { "$ref": "#/components/parameters/Locale" },
          { "$ref": "#/components/parameters/Currency" }
        ],
        "responses": {
          "200": { "$ref": "#/components/responses/Upstream" },
          "502": { "$ref": "#/components/responses/UpstreamDown" }
        }
      }
    },
    "/mcp": {
      "post": {
        "tags": ["mcp"],
        "summary": "Model Context Protocol over JSON-RPC 2.0",
        "description": "Methods: initialize, notifications/initialized, ping, tools/list, tools/call. Notifications are answered with 202 and no body. The same protocol is available over stdin/stdout by starting the binary with --mcp-stdio.",
        "operationId": "mcp",
        "requestBody": {
          "required": true,
          "content": { "application/json": { "schema": { "$ref": "#/components/schemas/JsonRpcRequest" } } }
        },
        "responses": {
          "200": {
            "description": "JSON-RPC response",
            "content": { "application/json": { "schema": { "$ref": "#/components/schemas/JsonRpcResponse" } } }
          },
          "202": { "description": "Notification accepted; no body" }
        }
      }
    },
    "/openapi.json": {
      "get": {
        "tags": ["site"],
        "summary": "This document",
        "operationId": "getOpenApi",
        "responses": {
          "200": {
            "description": "OpenAPI 3.1 description of BeloAPI",
            "content": { "application/json": { "schema": { "type": "object", "additionalProperties": true } } }
          }
        }
      }
    }
  },
  "components": {
    "parameters": {
      "Query": { "name": "query", "in": "query", "required": true, "description": "Free-text place name, for example Mombasa", "schema": { "type": "string" } },
      "Location": { "name": "location", "in": "query", "description": "Destination, for example Mombasa. Either location or place_id is required", "schema": { "type": "string" } },
      "PlaceId": { "name": "place_id", "in": "query", "description": "Google place id from /api/autocomplete. Either location or place_id is required", "schema": { "type": "string" } },
      "Checkin": { "name": "checkin", "in": "query", "description": "Check-in date, YYYY-MM-DD", "schema": { "type": "string", "format": "date" } },
      "Checkout": { "name": "checkout", "in": "query", "description": "Check-out date, YYYY-MM-DD", "schema": { "type": "string", "format": "date" } },
      "Adults": { "name": "adults", "in": "query", "schema": { "type": "integer", "minimum": 1, "default": 1 } },
      "Children": { "name": "children", "in": "query", "schema": { "type": "integer", "minimum": 0, "default": 0 } },
      "Infants": { "name": "infants", "in": "query", "schema": { "type": "integer", "minimum": 0, "default": 0 } },
      "Pets": { "name": "pets", "in": "query", "description": "1 when travelling with pets", "schema": { "type": "integer", "enum": [0, 1], "default": 0 } },
      "Items": { "name": "items", "in": "query", "description": "Results per page", "schema": { "type": "integer", "minimum": 1, "default": 18 } },
      "Locale": { "name": "locale", "in": "query", "description": "Locale such as en or fr", "schema": { "type": "string", "default": "en" } },
      "Currency": { "name": "currency", "in": "query", "description": "ISO 4217 currency such as USD or KES", "schema": { "type": "string" } },
      "Country": { "name": "country", "in": "query", "description": "ISO country code such as KE", "schema": { "type": "string" } },
      "NumResults": { "name": "num_results", "in": "query", "description": "How many suggestions to return", "schema": { "type": "integer", "minimum": 1, "default": 5 } },
      "Vertical": { "name": "vertical", "in": "query", "schema": { "type": "string", "enum": ["STAYS", "EXPERIENCES", "SERVICES"], "default": "STAYS" } },
      "Placements": { "name": "placements", "in": "query", "description": "Comma-separated placement slots", "schema": { "type": "string", "default": "HOME_PAGE_POPOVER" } },
      "DeviceId": { "name": "device_id", "in": "query", "schema": { "type": "string" } },
      "ConsentId": { "name": "consent_id", "in": "query", "schema": { "type": "string" } }
    },
    "responses": {
      "Upstream": {
        "description": "The raw Airbnb document for this call",
        "content": { "application/json": { "schema": { "type": "object", "additionalProperties": true } } }
      },
      "BadRequest": {
        "description": "Missing or invalid input",
        "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Error" } } }
      },
      "NotFound": {
        "description": "Airbnb does not recognise the requested place or route",
        "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Error" } } }
      },
      "UpstreamDown": {
        "description": "BeloAPI could not reach Airbnb",
        "content": { "application/json": { "schema": { "$ref": "#/components/schemas/Error" } } }
      }
    },
    "schemas": {
      "Error": {
        "type": "object",
        "required": ["error"],
        "properties": {
          "error": {
            "type": "object",
            "required": ["code", "message"],
            "properties": {
              "code": { "type": "string" },
              "message": { "type": "string" },
              "hint": { "type": "string" }
            }
          }
        }
      },
      "Health": {
        "type": "object",
        "properties": {
          "status": { "type": "string" },
          "name": { "type": "string" },
          "version": { "type": "string" },
          "author": { "type": "string" },
          "uptime_seconds": { "type": "integer" },
          "upstream": { "type": "string" }
        }
      },
      "Index": { "type": "object", "additionalProperties": true },
      "JsonRpcRequest": {
        "type": "object",
        "required": ["jsonrpc", "method"],
        "properties": {
          "jsonrpc": { "type": "string", "const": "2.0" },
          "id": { "oneOf": [{ "type": "string" }, { "type": "integer" }] },
          "method": { "type": "string" },
          "params": { "type": "object", "additionalProperties": true }
        }
      },
      "JsonRpcResponse": {
        "type": "object",
        "properties": {
          "jsonrpc": { "type": "string" },
          "id": { "oneOf": [{ "type": "string" }, { "type": "integer" }, { "type": "null" }] },
          "result": { "type": "object", "additionalProperties": true },
          "error": {
            "type": "object",
            "properties": { "code": { "type": "integer" }, "message": { "type": "string" } }
          }
        }
      }
    }
  }
})json";
    std::string document(kBody);
    size_t placeholder = document.find("__VERSION__");
    if (placeholder != std::string::npos) document.replace(placeholder, 11, kVersion);
    return document;
}

inline std::string health_document() {
    long uptime = (long)(std::time(nullptr) - g_started_at);
    if (uptime < 0) uptime = 0;
    return "{\"status\":\"ok\",\"name\":" + quote(kName) + ",\"version\":" + quote(kVersion) +
           ",\"author\":" + quote(kAuthor) + ",\"uptime_seconds\":" + std::to_string(uptime) +
           ",\"upstream\":" + quote(g_config.upstream) + "}";
}

// =============================================================================
//  Section 9 - Router
// =============================================================================

inline HttpResponse route(const HttpRequest& req) {
    // Normalise a trailing slash: "/api/health/" behaves like "/api/health".
    std::string path = req.path;
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    Params query = parse_query(req.query);

    if (req.method == "OPTIONS") {
        HttpResponse res;
        res.status = 204;
        res.body = "";
        return res;
    }

    if (req.method != "GET" && req.method != "POST" && req.method != "HEAD") {
        return json_response(405, "{\"error\":{\"code\":\"method_not_allowed\","
                                  "\"message\":\"Use GET for /api and POST for /mcp\"}}");
    }

    if (path == "/") return json_response(200, index_document());
    if (path == "/api" || path == "/api/") return json_response(200, index_document());
    if (path == "/api/health") return json_response(200, health_document());

    if (path == "/api/autocomplete") return from_upstream(handle_autocomplete(query));
    if (path == "/api/search") return from_upstream(handle_search(query));
    if (path == "/api/markets") return from_upstream(handle_markets(query));
    if (path == "/api/homepage") return from_upstream(handle_homepage(query));
    if (path == "/api/announcements") return from_upstream(handle_announcements(query));
    if (path == "/api/promotions") return from_upstream(handle_promotions(query));
    if (path == "/api/header") return from_upstream(handle_header(query));
    if (path == "/api/consent") return from_upstream(handle_consent(query));
    if (path == "/api/config") return from_upstream(handle_config(query));
    if (path == "/openapi.json" || path == "/api/openapi.json")
        return json_response(200, openapi_document());

    if (path == "/mcp") {
        if (req.method != "POST") {
            HttpResponse res = json_response(
                405, "{\"error\":{\"code\":\"method_not_allowed\","
                     "\"message\":\"MCP uses POST with a JSON-RPC body. "
                     "See docs/SETUP.md for a client example.\"}}");
            res.extra_headers.push_back({"Allow", "POST, OPTIONS"});
            return res;
        }
        bool notification = false;
        std::string reply = mcp_process(req.body, &notification);
        if (notification) {
            HttpResponse res;
            res.status = 202;
            res.body = "";
            return res;
        }
        return json_response(200, reply);
    }

    return json_response(404, "{\"error\":{\"code\":\"not_found\",\"message\":" +
                                  quote("No route for " + req.method + " " + req.path) +
                                  ",\"hint\":\"GET / lists every endpoint\"}}");
}

// =============================================================================
//  Section 10 - Server loop and client threads
// =============================================================================

inline void handle_client(socket_t client) {
    HttpRequest request;
    int parse_status = 400;
    if (!read_http_request(client, request, parse_status)) {
        HttpResponse failure = json_response(
            parse_status, "{\"error\":{\"code\":\"bad_request\","
                          "\"message\":\"Malformed or oversized HTTP request\"}}");
        send_all(client, build_response(failure));
        close_socket(client);
        return;
    }

    HttpResponse response;
    try {
        response = route(request);
    } catch (const std::exception& e) {
        response = json_response(500, "{\"error\":{\"code\":\"internal_error\",\"message\":" +
                                          quote(e.what()) + "}}");
    } catch (...) {
        response = json_response(500, "{\"error\":{\"code\":\"internal_error\","
                                      "\"message\":\"Unknown server error\"}}");
    }

    // HEAD gets the headers but no body.
    if (request.method == "HEAD") response.body.clear();
    send_all(client, build_response(response));
    close_socket(client);

    std::printf("%s %s -> %d\n", request.method.c_str(),
                (request.path + (request.query.empty() ? "" : "?" + request.query)).c_str(),
                response.status);
    std::fflush(stdout);
}

// Start listening and serve until the process is stopped (Ctrl+C).
inline int run_server() {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        std::fprintf(stderr, "Could not create a socket\n");
        return 1;
    }

    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)g_config.port);
    // Only loopback is supported; binding elsewhere would expose the proxy to
    // the network, which users must opt into deliberately (see docs/SETUP.md).
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (g_config.host != "127.0.0.1" && g_config.host != "localhost") {
        std::fprintf(stderr,
                     "Refusing to bind %s: BeloAPI only listens on 127.0.0.1. "
                     "Put a reverse proxy in front of it if you need remote access.\n",
                     g_config.host.c_str());
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0) {
        std::fprintf(stderr, "Could not bind 127.0.0.1:%d (is the port in use?)\n",
                     g_config.port);
        close_socket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    if (listen(listener, 32) != 0) {
        std::fprintf(stderr, "Could not listen on port %d\n", g_config.port);
        close_socket(listener);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::printf("\n  %s v%s - unofficial Airbnb API + MCP\n", kName, kVersion);
    std::printf("  by %s\n  contact: %s\n\n", kAuthor, kContact);
    std::printf("  http://127.0.0.1:%d/           landing page\n", g_config.port);
    std::printf("  http://127.0.0.1:%d/api/health health\n", g_config.port);
    std::printf("  http://127.0.0.1:%d/mcp        MCP endpoint (POST)\n\n", g_config.port);
    std::printf("  Ctrl+C to stop.\n\n");
    std::fflush(stdout);

    for (;;) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_size = sizeof(peer);
#else
        socklen_t peer_size = sizeof(peer);
#endif
        socket_t client = accept(listener, (sockaddr*)&peer, &peer_size);
        if (client == kInvalidSocket) continue;
        std::thread(handle_client, client).detach();
    }
}

// =============================================================================
//  Section 11 - Self test
// -----------------------------------------------------------------------------
//  `beloapi --test` walks every route once and reports the upstream status.
//  This is what the release checklist runs before a binary is published.
// =============================================================================

inline int run_self_test() {
    struct Case {
        const char* label;
        ApiResult (*fn)(const Params&);
        Params params;
    };

    Params autocomplete;
    autocomplete["query"] = "Mombasa";

    Params search;
    search["location"] = "Mombasa";
    search["adults"] = "2";
    search["items"] = "3";

    std::vector<Case> cases = {
        {"autocomplete", handle_autocomplete, autocomplete},
        {"search", handle_search, search},
        {"markets", handle_markets, {}},
        {"homepage", handle_homepage, {}},
        {"announcements", handle_announcements, {}},
        {"promotions", handle_promotions, {}},
        {"header", handle_header, {}},
        {"consent", handle_consent, {}},
        {"config", handle_config, {}},
    };

    std::printf("\n%s %s self test\n\n", kName, kVersion);
    int failures = 0;
    for (const Case& c : cases) {
        ApiResult answer = c.fn(c.params);
        bool ok = answer.ok() && answer.status >= 200 && answer.status < 300 &&
                  !answer.body.empty();
        if (!ok) failures++;
        std::printf("  [%s] %-14s status=%-4d bytes=%-8d %s\n", ok ? "PASS" : "FAIL",
                    c.label, answer.status, (int)answer.body.size(),
                    answer.ok() ? "" : answer.error.c_str());
        std::fflush(stdout);
    }

    // MCP is local-only, so one handshake covers the whole JSON-RPC layer.
    bool parsed = false;
    std::string reply = mcp_process(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    Json document = json_parse(reply, &parsed);
    const Json* tools = document.find("result");
    bool mcp_ok = parsed && tools && tools->find("tools");
    if (!mcp_ok) failures++;
    std::printf("  [%s] %-14s %s\n", mcp_ok ? "PASS" : "FAIL", "mcp tools/list",
                mcp_ok ? "" : "invalid JSON-RPC reply");

    // The OpenAPI document is hand-written, so parse it to catch syntax slips.
    bool spec_parsed = false;
    Json spec = json_parse(openapi_document(), &spec_parsed);
    bool spec_ok = spec_parsed && spec.find("paths") && spec.find("openapi") &&
                   spec.find("components");
    if (!spec_ok) failures++;
    std::printf("  [%s] %-14s %s\n", spec_ok ? "PASS" : "FAIL", "openapi.json",
                spec_ok ? "" : "document is not valid OpenAPI JSON");

    std::printf("\n  %d route(s) failed\n\n", failures);
    return failures == 0 ? 0 : 1;
}

}  // namespace beloapi

// =============================================================================
//  Section 12 - Entry point
// =============================================================================

static void print_usage() {
    std::printf(
        "%s v%s - unofficial Airbnb JSON API and MCP server\n"
        "Made by %s. Contact: %s\n\n"
        "Usage: beloapi [options]\n\n"
        "  --port <number>   port to listen on (default %d)\n"
        "  --mcp-stdio       serve MCP over stdin/stdout for desktop AI clients\n"
        "  --test            call every upstream route and report, then exit\n"
        "  --version         print the version and exit\n"
        "  --help            show this help\n\n"
        "Endpoints:\n"
        "  GET  /api/health\n"
        "  GET  /api/autocomplete?query=Mombasa\n"
        "  GET  /api/search?location=Mombasa&checkin=2026-12-10&checkout=2026-12-12&adults=2\n"
        "  GET  /api/markets | /api/homepage | /api/announcements | /api/promotions\n"
        "  GET  /api/header  | /api/consent  | /api/config\n"
        "  GET  /openapi.json OpenAPI 3.1 description\n"
        "  POST /mcp         MCP JSON-RPC 2.0\n\n"
        "Docs: README.md and docs/SETUP.md\n",
        beloapi::kName, beloapi::kVersion, beloapi::kAuthor, beloapi::kContact,
        beloapi::kDefaultPort);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    beloapi::g_started_at = std::time(nullptr);

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::printf("%s %s\n", beloapi::kName, beloapi::kVersion);
            return 0;
        }
        if (arg == "--test") {
            return beloapi::run_self_test();
        }
        if (arg == "--mcp-stdio") {
            return beloapi::run_mcp_stdio();
        }
        if (arg == "--port" && i + 1 < argc) {
            int port = std::atoi(argv[++i]);
            if (port < 1 || port > 65535) {
                std::fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 2;
            }
            beloapi::g_config.port = port;
            continue;
        }
        if (arg == "--host" && i + 1 < argc) {
            beloapi::g_config.host = argv[++i];
            continue;
        }
        std::fprintf(stderr, "Unknown option: %s (try --help)\n", arg.c_str());
        return 2;
    }

    return beloapi::run_server();
}
