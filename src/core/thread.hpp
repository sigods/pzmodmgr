#pragma once
#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace pz {
namespace thread_detail {

struct Task {
    virtual ~Task() = default;
    virtual void run() = 0;
};

template <typename F>
struct TaskImpl final : Task {
    F function;
    explicit TaskImpl(F&& f) : function(std::move(f)) {}
    void run() override { function(); }
};

#ifdef _WIN32
inline unsigned __stdcall entry(void* context) {
    static_cast<Task*>(context)->run();
    return 0;
}
#else
inline void* entry(void* context) {
    static_cast<Task*>(context)->run();
    return nullptr;
}
#endif

}  // namespace thread_detail

// Non-throwing replacements for std::mutex and std::lock_guard. The UI only
// needs a plain mutex, and these avoid the libstdc++ mutex/error machinery.
class mutex {
public:
    mutex() {
#ifdef _WIN32
        InitializeCriticalSection(&handle_);
#else
        pthread_mutex_init(&handle_, nullptr);
#endif
    }
    ~mutex() {
#ifdef _WIN32
        DeleteCriticalSection(&handle_);
#else
        pthread_mutex_destroy(&handle_);
#endif
    }

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;

    void lock() {
#ifdef _WIN32
        EnterCriticalSection(&handle_);
#else
        pthread_mutex_lock(&handle_);
#endif
    }
    void unlock() {
#ifdef _WIN32
        LeaveCriticalSection(&handle_);
#else
        pthread_mutex_unlock(&handle_);
#endif
    }

private:
#ifdef _WIN32
    CRITICAL_SECTION handle_{};
#else
    pthread_mutex_t handle_{};
#endif
};

class lock_guard {
public:
    explicit lock_guard(mutex& value) : mutex_(value) { mutex_.lock(); }
    ~lock_guard() { mutex_.unlock(); }
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    mutex& mutex_;
};

// A std::thread replacement that cannot throw. A failed start leaves the
// object non-joinable; callers report that instead of catching std::system_error.
class thread {
public:
    thread() = default;

    template <typename F>
    explicit thread(F&& function) {
        auto* task = new thread_detail::TaskImpl<std::decay_t<F>>(
            std::forward<F>(function));
#ifdef _WIN32
        handle_ = reinterpret_cast<HANDLE>(
            _beginthreadex(nullptr, 0, &thread_detail::entry, task, 0, nullptr));
        if (!handle_) {
            delete task;
            return;
        }
#else
        if (pthread_create(&handle_, nullptr, &thread_detail::entry, task) != 0) {
            delete task;
            return;
        }
#endif
        task_ = task;
        joinable_ = true;
    }

    ~thread() {
        // Match std::thread's contract without dragging in the exception
        // runtime: destroying a joinable thread is a programming error.
        if (joinable_) std::abort();
    }

    thread(const thread&) = delete;
    thread& operator=(const thread&) = delete;

    thread(thread&& other) noexcept
        : task_(other.task_), joinable_(other.joinable_)
#ifdef _WIN32
        , handle_(other.handle_)
#else
        , handle_(other.handle_)
#endif
    {
        other.task_ = nullptr;
        other.joinable_ = false;
#ifdef _WIN32
        other.handle_ = nullptr;
#else
        other.handle_ = {};
#endif
    }

    thread& operator=(thread&& other) noexcept {
        if (this == &other) return *this;
        if (joinable_) std::abort();
        task_ = other.task_;
        joinable_ = other.joinable_;
        handle_ = other.handle_;
        other.task_ = nullptr;
        other.joinable_ = false;
#ifdef _WIN32
        other.handle_ = nullptr;
#else
        other.handle_ = {};
#endif
        return *this;
    }

    bool joinable() const { return joinable_; }

    void join() {
        if (!joinable_) return;
#ifdef _WIN32
        WaitForSingleObject(handle_, INFINITE);
        CloseHandle(handle_);
        handle_ = nullptr;
#else
        pthread_join(handle_, nullptr);
        handle_ = {};
#endif
        delete task_;
        task_ = nullptr;
        joinable_ = false;
    }

private:
    thread_detail::Task* task_ = nullptr;
    bool joinable_ = false;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    pthread_t handle_{};
#endif
};

inline void sleep_milliseconds(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    ::usleep(milliseconds * 1000u);
#endif
}

}  // namespace pz
