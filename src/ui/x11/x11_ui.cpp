#include "ui/ui.hpp"
#include "core/commands.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pz {
namespace {
constexpr int kW = 800, kH = 620;
constexpr int kListTop = 80, kRowH = 16, kListMaxY = 272;
constexpr int kVisibleRows = (kListMaxY - kListTop) / kRowH + 1;
constexpr int kActionY = 286, kActionH = 24;
constexpr int kUpdateX = 20, kRemoveX = 170, kUpdateAllX = 320, kSettingsX = 450;
constexpr int kSelectAllX = 560, kClearX = 670;
constexpr int kActionW = 140, kUpdateAllW = 120, kSettingsW = 100;
constexpr int kSelectAllW = 100, kClearW = 80;
constexpr int kAddX = 150, kAddY = 14, kAddW = 480, kAddH = 24;
constexpr int kAddButtonX = 640, kAddButtonW = 140;
constexpr int kLogTop = 364;

void draw(Display* d, Window w, GC gc, int x, int y, const std::string& s) {
    XDrawString(d, w, gc, x, y, s.c_str(), static_cast<int>(s.size()));
}

bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}
}  // namespace

int run_ui(ModManager& mm) {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "Cannot open X display (DISPLAY unset? XWayland?)\n");
        return 1;
    }

    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) {
        std::fprintf(stderr, "Cannot create worker wakeup pipe: %s\n", std::strerror(errno));
        XCloseDisplay(dpy);
        return 1;
    }
    for (int fd : wake) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, kW, kH, 1,
                                     BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, win, "Project Zomboid Mod Manager");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XMapWindow(dpy, win);

    std::deque<std::string> log;
    std::string input;
    bool busy = false;

    std::set<std::uint64_t> selected_ids;
    bool confirm_remove = false;
    int list_offset = 0;
    std::vector<InstalledMod> visible;

    bool settings_open = false;
    int settings_field = 0;
    std::string settings_path, settings_user;

    std::mutex pending_mu;
    std::deque<std::string> pending;
    bool worker_done = false;
    std::thread worker;

    auto push = [&log](const std::string& s) {
        log.push_back(s);
        while (log.size() > 300) log.pop_front();
    };

    auto wake_ui = [&]() {
        char b = 1;
        ssize_t ignored = ::write(wake[1], &b, 1);
        (void)ignored;
    };

    auto enqueue = [&](const std::string& s) {
        {
            std::lock_guard<std::mutex> lock(pending_mu);
            pending.push_back(s);
        }
        wake_ui();
    };

    auto drain_pending = [&]() {
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> lock(pending_mu);
            batch.swap(pending);
        }
        for (const auto& s : batch) push(s);
        std::lock_guard<std::mutex> lock(pending_mu);
        bool done = worker_done;
        worker_done = false;
        return done;
    };

    auto build_command = [](const std::string& verb, const std::set<std::uint64_t>& ids) {
        std::string cmd = verb;
        for (auto id : ids) cmd += " " + std::to_string(id);
        return cmd;
    };

    auto redraw = [&]() {
        XClearWindow(dpy, win);

        if (settings_open) {
            draw(dpy, win, gc, 20, 30, "Settings");
            draw(dpy, win, gc, 20, 68, "steamcmd executable (blank = built-in):");
            XDrawRectangle(dpy, win, gc, 20, 78, 740, 24);
            draw(dpy, win, gc, 28, 95, settings_path + (settings_field == 0 ? "_" : ""));
            draw(dpy, win, gc, 20, 128, "Steam username:");
            XDrawRectangle(dpy, win, gc, 20, 138, 740, 24);
            draw(dpy, win, gc, 28, 155, settings_user + (settings_field == 1 ? "_" : ""));
            draw(dpy, win, gc, 20, 190,
                 "No password is stored. Run steamcmd manually once so its login is cached.");
            XDrawRectangle(dpy, win, gc, 20, 220, 120, 26);
            draw(dpy, win, gc, 50, 238, "Save");
            XDrawRectangle(dpy, win, gc, 150, 220, 120, 26);
            draw(dpy, win, gc, 178, 238, "Cancel");
            return;
        }

        draw(dpy, win, gc, 10, 31, "Workshop URL/ID:");
        XDrawRectangle(dpy, win, gc, kAddX, kAddY, kAddW, kAddH);
        draw(dpy, win, gc, kAddX + 8, kAddY + 17, input + (busy ? "" : "_"));
        XDrawRectangle(dpy, win, gc, kAddButtonX, kAddY, kAddButtonW, kAddH);
        draw(dpy, win, gc, kAddButtonX + 20, kAddY + 17, "Add / Install");
        draw(dpy, win, gc, 10, 62, "Installed mods (click rows to select multiple):");

        if (!busy) {
            visible = mm.list();
            const int total = static_cast<int>(visible.size());
            list_offset = std::min(std::max(0, list_offset), std::max(0, total - kVisibleRows));

            std::set<std::uint64_t> present;
            for (const auto& m : visible) present.insert(m.workshop_id);
            for (auto it = selected_ids.begin(); it != selected_ids.end();) {
                if (present.count(*it) == 0) it = selected_ids.erase(it);
                else ++it;
            }
            if (selected_ids.empty()) confirm_remove = false;

            if (visible.empty())
                draw(dpy, win, gc, 20, kListTop, "No mods installed yet.");
            int y = kListTop;
            for (int i = list_offset; i < total && y <= kListMaxY; ++i, y += kRowH) {
                const auto& m = visible[static_cast<std::size_t>(i)];
                const bool sel = selected_ids.count(m.workshop_id) != 0;
                std::string row = std::string(sel ? "[x] " : "[ ] ") + m.name + "  [" +
                                  m.folder + "]  id=" + std::to_string(m.workshop_id);
                draw(dpy, win, gc, 20, y, row);
                if (sel) XDrawRectangle(dpy, win, gc, 10, y - 12, 770, 16);
            }
            if (total > kVisibleRows)
                draw(dpy, win, gc, 600, 62, "mouse wheel to scroll");
        } else {
            visible.clear();
            draw(dpy, win, gc, 20, kListTop, "(command running)");
        }

        auto button = [&](int x, int w, const std::string& label) {
            XDrawRectangle(dpy, win, gc, x, kActionY, w, kActionH);
            draw(dpy, win, gc, x + 10, kActionY + 16, label);
        };
        button(kUpdateX, kActionW, "Update selected");
        button(kRemoveX, kActionW, confirm_remove ? "Confirm remove" : "Remove selected");
        button(kUpdateAllX, kUpdateAllW, "Update all");
        button(kSettingsX, kSettingsW, "Settings");
        button(kSelectAllX, kSelectAllW, "Select all");
        button(kClearX, kClearW, "Clear");

        std::string details = "Selected: none";
        if (selected_ids.size() == 1) {
            for (const auto& m : visible) {
                if (m.workshop_id != *selected_ids.begin()) continue;
                details = "Selected: " + m.name + "  [" + m.folder + "]  id=" +
                          std::to_string(m.workshop_id);
                break;
            }
        } else if (selected_ids.size() > 1) {
            details = "Selected: " + std::to_string(selected_ids.size()) + " mods";
        }
        draw(dpy, win, gc, 10, 326, details);
        draw(dpy, win, gc, 10, 350, "Activity");

        const int rows = (kH - 40 - kLogTop) / 16;
        int start = static_cast<int>(log.size()) - rows;
        if (start < 0) start = 0;
        int ly = kLogTop;
        for (int i = start; i < static_cast<int>(log.size()); ++i, ly += 16)
            draw(dpy, win, gc, 10, ly, log[i]);

        draw(dpy, win, gc, 10, kH - 22,
             busy ? "Working... steamcmd may take a while" : "Ready");

        // Xlib buffers requests. Flush before blocking in poll(), otherwise
        // MapWindow and the first paint never reach the server.
        XFlush(dpy);
    };

    auto start_command = [&](const std::string& line) {
        if (busy || line.empty()) return;
        confirm_remove = false;
        push("> " + line);
        busy = true;
        if (worker.joinable()) worker.join();
        try {
            worker = std::thread([&, line]() {
                auto r = run_command(mm, line, enqueue);
                {
                    std::lock_guard<std::mutex> lock(pending_mu);
                    pending.push_back(std::string(r.ok ? "OK: " : "ERROR: ") + r.message);
                    worker_done = true;
                }
                wake_ui();
            });
        } catch (const std::exception& e) {
            busy = false;
            push(std::string("ERROR: cannot start worker: ") + e.what());
        }
        redraw();
    };

    auto open_settings = [&]() {
        if (busy) return;
        settings_path = mm.config().steamcmd_exe.string();
        settings_user = mm.config().steam_user;
        settings_field = 0;
        settings_open = true;
        redraw();
    };

    auto save_settings = [&]() {
        if (settings_user.empty()) {
            push("Steam username is required.");
            redraw();
            return;
        }
        auto r1 = run_command(mm, "config steamcmd_exe " + settings_path, push);
        auto r2 = run_command(mm, "config steam_user " + settings_user, push);
        if (r1.ok && r2.ok) {
            settings_open = false;
            push("settings saved");
        } else {
            push("ERROR: could not save settings");
        }
        redraw();
    };

    push("Paste a Workshop URL or ID, then click Add / Install.");
    push("Click rows to select multiple, then Update selected / Remove selected.");
    redraw();

    bool running = true;
    auto handle_events = [&]() {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                redraw();
                break;
            case ButtonPress: {
                if (busy) break;
                const int x = ev.xbutton.x, y = ev.xbutton.y;

                if (settings_open) {
                    if (in_rect(x, y, 20, 78, 740, 24)) settings_field = 0;
                    else if (in_rect(x, y, 20, 138, 740, 24)) settings_field = 1;
                    else if (in_rect(x, y, 20, 220, 120, 26)) save_settings();
                    else if (in_rect(x, y, 150, 220, 120, 26)) settings_open = false;
                    redraw();
                    break;
                }

                if (ev.xbutton.button == Button4) {
                    list_offset = std::max(0, list_offset - 3);
                    redraw();
                    break;
                }
                if (ev.xbutton.button == Button5) {
                    list_offset += 3;
                    redraw();
                    break;
                }

                if (in_rect(x, y, kAddButtonX, kAddY, kAddButtonW, kAddH)) {
                    if (input.empty()) {
                        push("Enter a Workshop URL or ID first.");
                        redraw();
                    } else {
                        const std::string line = "add " + input;
                        input.clear();
                        start_command(line);
                    }
                } else if (in_rect(x, y, kUpdateX, kActionY, kActionW, kActionH)) {
                    if (selected_ids.empty()) push("Select a mod first.");
                    else start_command(build_command("update", selected_ids));
                } else if (in_rect(x, y, kRemoveX, kActionY, kActionW, kActionH)) {
                    if (selected_ids.empty()) {
                        push("Select a mod first.");
                        redraw();
                    } else if (!confirm_remove) {
                        confirm_remove = true;
                        push("Click Confirm remove again to delete " +
                             std::to_string(selected_ids.size()) + " selected mod(s).");
                        redraw();
                    } else {
                        start_command(build_command("remove", selected_ids));
                    }
                } else if (in_rect(x, y, kUpdateAllX, kActionY, kUpdateAllW, kActionH)) {
                    start_command("update");
                } else if (in_rect(x, y, kSettingsX, kActionY, kSettingsW, kActionH)) {
                    open_settings();
                } else if (in_rect(x, y, kSelectAllX, kActionY, kSelectAllW, kActionH)) {
                    for (const auto& m : visible) selected_ids.insert(m.workshop_id);
                    confirm_remove = false;
                    redraw();
                } else if (in_rect(x, y, kClearX, kActionY, kClearW, kActionH)) {
                    selected_ids.clear();
                    confirm_remove = false;
                    redraw();
                } else if (y >= kListTop - 12 && y <= kListMaxY + 4) {
                    const int row = (y - (kListTop - 12)) / kRowH;
                    const int index = list_offset + row;
                    if (row >= 0 && index >= 0 && index < static_cast<int>(visible.size())) {
                        auto id = visible[static_cast<std::size_t>(index)].workshop_id;
                        if (selected_ids.count(id)) selected_ids.erase(id);
                        else selected_ids.insert(id);
                        confirm_remove = false;
                        redraw();
                    }
                }
                break;
            }
            case ClientMessage:
                if (static_cast<Atom>(ev.xclient.data.l[0]) == wm_delete) {
                    if (busy) {
                        push("A command is still running; wait for it before closing.");
                        redraw();
                    } else {
                        running = false;
                    }
                }
                break;
            case KeyPress: {
                if (busy) break;
                char buf[32];
                KeySym ks;
                int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, nullptr);

                if (settings_open) {
                    if (ks == XK_Return || ks == XK_KP_Enter) {
                        save_settings();
                    } else if (ks == XK_Escape) {
                        settings_open = false;
                        redraw();
                    } else if (ks == XK_Tab) {
                        settings_field = 1 - settings_field;
                        redraw();
                    } else if (ks == XK_BackSpace) {
                        auto& field = settings_field == 0 ? settings_path : settings_user;
                        if (!field.empty()) field.pop_back();
                        redraw();
                    } else if (n > 0 && static_cast<unsigned char>(buf[0]) >= 32) {
                        auto& field = settings_field == 0 ? settings_path : settings_user;
                        field.append(buf, static_cast<std::size_t>(n));
                        redraw();
                    }
                    break;
                }

                if (ks == XK_Return || ks == XK_KP_Enter) {
                    if (input.empty()) {
                        push("Enter a Workshop URL or ID first.");
                        redraw();
                    } else {
                        const std::string line = "add " + input;
                        input.clear();
                        start_command(line);
                    }
                } else if (ks == XK_BackSpace) {
                    if (!input.empty()) input.pop_back();
                    redraw();
                } else if (ks == XK_Escape) {
                    running = false;
                } else if (n > 0 && static_cast<unsigned char>(buf[0]) >= 32) {
                    input.append(buf, static_cast<std::size_t>(n));
                    redraw();
                }
                break;
            }
            default:
                break;
            }
        }
    };

    while (running) {
        pollfd fds[2] = {{ConnectionNumber(dpy), POLLIN, 0}, {wake[0], POLLIN, 0}};
        int rc = ::poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "poll failed: %s\n", std::strerror(errno));
            break;
        }

        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (::read(wake[0], buf, sizeof buf) > 0) {}
            if (drain_pending()) {
                if (worker.joinable()) worker.join();
                busy = false;
            }
            redraw();
        }
        if (fds[0].revents & POLLIN) handle_events();
    }

    if (worker.joinable()) worker.join();
    ::close(wake[0]);
    ::close(wake[1]);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
}  // namespace pz
