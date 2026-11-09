#pragma once
#include <string>

namespace pz {

enum class error_kind { generic, system };

// Small replacement for pz::error_code. The application only needs a
// success/failure flag and a human-readable detail; avoiding pz::error_code
// keeps libstdc++'s system_error.o and the throwing exception runtime out of
// the static link.
class error_code {
public:
    error_code() = default;
    explicit error_code(int value) : value_(value) {}

    void assign(int value) { value_ = value; }
    void assign(int value, error_kind kind) {
        value_ = value;
        kind_ = kind;
    }
    void clear() {
        value_ = 0;
        kind_ = error_kind::generic;
    }

    explicit operator bool() const { return value_ != 0; }
    int value() const { return value_; }
    std::string message() const;

private:
    int value_ = 0;
    error_kind kind_ = error_kind::generic;
};

inline bool operator==(const error_code& a, const error_code& b) {
    return a.value() == b.value();
}
inline bool operator!=(const error_code& a, const error_code& b) {
    return !(a == b);
}

}  // namespace pz
