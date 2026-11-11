#pragma once
#include "core/types.hpp"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pz {

// Appends a fixed-width lowercase hexadecimal number. This keeps MinGW's
// printf formatter out of the static runtime for the few places that only need
// hex digits.
inline void append_hex(std::string& out, std::uint32_t value, int digits) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
        out += kDigits[(value >> shift) & 0x0f];
}

// The core uses C stdio instead of <fstream>/<sstream>: on MinGW those headers
// pull the whole locale and iostream machinery into the statically linked
// executable. _wfopen keeps non-ASCII Zomboid/mod paths working on Windows.

inline std::FILE* open_text_read(const fs::path& file) {
#ifdef _WIN32
    return _wfopen(file.c_str(), L"rb");
#else
    return std::fopen(file.c_str(), "rb");
#endif
}

inline std::FILE* open_text_write(const fs::path& file) {
#ifdef _WIN32
    return _wfopen(file.c_str(), L"wb");
#else
    return std::fopen(file.c_str(), "wb");
#endif
}

inline bool read_text_file(const fs::path& file, std::string& out) {
    std::FILE* in = open_text_read(file);
    if (!in) return false;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, in)) > 0) out.append(buffer, n);
    const bool ok = std::ferror(in) == 0;
    std::fclose(in);
    return ok;
}

// Calls fn(line) for every line, without the trailing CR/LF. Returns false only
// when the file cannot be opened. Lines longer than the read buffer are joined.
template <typename Fn>
bool for_each_line(const fs::path& file, Fn fn) {
    std::FILE* in = open_text_read(file);
    if (!in) return false;
    std::string line;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, in)) {
        line += buffer;
        if (line.empty() || line.back() != '\n') continue;
        line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        fn(line);
        line.clear();
    }
    if (!line.empty()) {
        if (line.back() == '\r') line.pop_back();
        fn(line);
    }
    std::fclose(in);
    return true;
}

inline std::vector<std::string> split_fields(const std::string& line, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = line.find(separator, start);
        parts.push_back(line.substr(start,
            pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

// Splits "key   value" into its first word and the rest of the line, keeping
// spaces inside the value (paths can contain them).
inline std::pair<std::string, std::string> split_first_word(const std::string& line) {
    const std::size_t begin = line.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const std::size_t end = line.find_first_of(" \t\r\n", begin);
    const std::string first = line.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    std::string rest;
    if (end != std::string::npos) {
        const std::size_t value_start = line.find_first_not_of(" \t\r\n", end);
        if (value_start != std::string::npos) rest = line.substr(value_start);
    }
    return {first, rest};
}

}  // namespace pz
