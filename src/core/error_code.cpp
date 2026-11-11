#include "core/error_code.hpp"
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pz {

std::string error_code::message() const {
    if (value_ == 0) return "Success";
#ifdef _WIN32
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(value_), 0, reinterpret_cast<char*>(&buffer), 0,
        nullptr);
    if (length && buffer) {
        std::string text(buffer, length);
        LocalFree(buffer);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' ||
                                 text.back() == ' ' || text.back() == '.'))
            text.pop_back();
        if (!text.empty()) return text;
    }
#else
    const char* text = std::strerror(value_);
    if (text && *text) return text;
#endif
    return "error " + std::to_string(value_);
}

}  // namespace pz
