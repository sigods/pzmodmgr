// Firefox native messaging host: one framed JSON request per process.
// Supported request: {"url":"<workshop item or collection url/id>"}
#include "core/commands.hpp"
#include "core/paths.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr std::uint32_t kMaxMessage = 1024 * 1024;

bool read_exact(void* dst, std::size_t n) {
    return std::fread(dst, 1, n, stdin) == n;
}

bool write_exact(const void* src, std::size_t n) {
    return std::fwrite(src, 1, n, stdout) == n;
}

bool read_message(std::string& message) {
    std::uint32_t len = 0;
    if (!read_exact(&len, sizeof len)) return false;
    if (len == 0 || len > kMaxMessage) return false;
    message.resize(len);
    return read_exact(message.data(), len);
}

bool write_message(const std::string& message) {
    const auto len = static_cast<std::uint32_t>(message.size());
    return write_exact(&len, sizeof len) && write_exact(message.data(), message.size()) &&
           std::fflush(stdout) == 0;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7]{};
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

// Minimal parser for the one field the host accepts. The extension sends a
// JSON object, so the key is not inside another string.
std::string json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};

    std::string out;
    for (std::size_t i = pos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') break;
        if (c == '\\' && i + 1 < json.size()) {
            const char esc = json[++i];
            switch (esc) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
                if (i + 4 < json.size()) {
                    out += '?';   // URLs are ASCII; non-ASCII is not needed
                    i += 4;
                }
                break;
            default: out += esc; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    std::string message;
    if (!read_message(message)) return 1;

    const std::string url = json_string_field(message, "url");
    pz::Config cfg = pz::default_config();
    pz::load_config(cfg);
    pz::ModManager mm(cfg);

    std::vector<std::string> log;
    pz::Result result;
    if (url.empty()) {
        result = {false, "Missing Workshop URL or ID."};
    } else {
        result = pz::run_command(mm, "add " + url, [&log](const std::string& line) {
            log.push_back(line.size() > 500 ? line.substr(0, 500) : line);
        });
    }

    if (log.size() > 40) log.erase(log.begin(), log.end() - 40);

    std::string response = "{\"ok\":";
    response += result.ok ? "true" : "false";
    response += ",\"message\":\"" + json_escape(result.message) + "\"";
    if (!log.empty()) {
        response += ",\"log\":[";
        for (std::size_t i = 0; i < log.size(); ++i) {
            if (i != 0) response += ',';
            response += '"' + json_escape(log[i]) + '"';
        }
        response += ']';
    }
    response += '}';

    return write_message(response) ? 0 : 1;
}
