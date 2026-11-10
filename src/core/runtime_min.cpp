// Small allocation and fatal-error replacements for the static C++ runtime.
// They keep libstdc++'s exception machinery, unwinder, and demangler out of the
// executable. Allocation failure is fatal, matching the application's previous
// behavior of not handling std::bad_alloc.
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

void* allocate(std::size_t size) {
    void* memory = std::malloc(size ? size : 1);
    if (!memory) std::abort();
    return memory;
}

}  // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size ? size : 1);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    std::free(memory);
}

// libstdc++ headers emit calls to these helpers for vector/string growth and
// mutex failures. The application cannot recover from either condition, so
// terminate without pulling eh_throw, the DWARF unwinder, or the demangler.
#ifdef _WIN32
// libstdc++'s default verbose terminate handler pulls in the full demangler
// (about 49 KB in the static MinGW runtime) just to print a type name. This
// application has exceptions disabled and cannot recover from termination, so
// abort without the demangler.
namespace __gnu_cxx {
[[noreturn]] void __verbose_terminate_handler() { std::abort(); }
}  // namespace __gnu_cxx
#endif

namespace std {

void __throw_bad_alloc() { std::abort(); }
void __throw_bad_array_new_length() { std::abort(); }
void __throw_length_error(const char*) { std::abort(); }
void __throw_out_of_range(const char*) { std::abort(); }
void __throw_out_of_range_fmt(const char*, ...) { std::abort(); }
void __throw_logic_error(const char*) { std::abort(); }
void __throw_domain_error(const char*) { std::abort(); }
void __throw_invalid_argument(const char*) { std::abort(); }
void __throw_runtime_error(const char*) { std::abort(); }
void __throw_range_error(const char*) { std::abort(); }
void __throw_overflow_error(const char*) { std::abort(); }
void __throw_underflow_error(const char*) { std::abort(); }
void __glibcxx_assert_fail(const char*, int, const char*, const char*) {
    std::abort();
}

}  // namespace std
