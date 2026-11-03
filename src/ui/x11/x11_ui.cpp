#include "ui/ui.hpp"
#include "ui/mod_icon.hpp"
#include "core/commands.hpp"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pz {
namespace {
constexpr int kW = 1000, kH = 720;
constexpr int kMargin = 24;
constexpr int kInputX = 24, kInputY = 84, kInputW = 700, kInputH = 38;
constexpr int kInstallX = 740, kInstallW = 212;
constexpr int kFilterY = 158, kFilterH = 24;
constexpr int kListTop = 192, kRowH = 64, kRowGap = 6, kRows = 5;
constexpr int kCardW = 952;
constexpr int kListBottom = kListTop + kRows * kRowH + (kRows - 1) * kRowGap;
constexpr int kActionY = 554, kActionH = 32;
constexpr int kActivityY = 602, kActivityH = 80;
constexpr int kStatusY = 702;
constexpr int kSettingsX = 170, kSettingsY = 140;
constexpr int kSettingsW = 660, kSettingsH = 430;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Palette {
    unsigned long bg, surface, field, border, border_strong, text, muted;
    unsigned long accent, accent_hover, accent_soft, danger, danger_soft;
    unsigned long hover, success;
};

enum class Btn {
    Nothing, Install, Settings, Update, Remove, UpdateAll, SelectAll, Clear, Save,
    Cancel, FilterAll, FilterUpdates, Sort, Check
};

bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

unsigned long alloc_color(Display* d, int scr, int r, int g, int b) {
    XColor c{};
    c.red = static_cast<unsigned short>(r * 257);
    c.green = static_cast<unsigned short>(g * 257);
    c.blue = static_cast<unsigned short>(b * 257);
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(d, DefaultColormap(d, scr), &c)) return BlackPixel(d, scr);
    return c.pixel;
}

void fill(Display* d, Window w, GC gc, int x, int y, int width, int height,
          unsigned long color) {
    if (width <= 0 || height <= 0) return;
    XSetForeground(d, gc, color);
    XFillRectangle(d, w, gc, x, y, static_cast<unsigned>(width),
                   static_cast<unsigned>(height));
}

void frame(Display* d, Window w, GC gc, int x, int y, int width, int height,
           unsigned long color) {
    if (width <= 0 || height <= 0) return;
    XSetForeground(d, gc, color);
    XDrawRectangle(d, w, gc, x, y, static_cast<unsigned>(std::max(0, width - 1)),
                   static_cast<unsigned>(std::max(0, height - 1)));
}

void text(Display* d, Window w, GC gc, XFontStruct* font, int x, int y,
          const std::string& s, unsigned long color) {
    if (!font || s.empty()) return;
    XSetFont(d, gc, font->fid);
    XSetForeground(d, gc, color);
    XDrawString(d, w, gc, x, y, s.c_str(), static_cast<int>(s.size()));
}

int text_width(XFontStruct* font, const std::string& s) {
    return font ? XTextWidth(font, s.c_str(), static_cast<int>(s.size())) : 0;
}

int baseline_for(XFontStruct* font, const Rect& r) {
    if (!font) return r.y + r.h / 2;
    return r.y + (r.h + font->ascent - font->descent) / 2;
}

std::string elide(XFontStruct* font, const std::string& s, int max_width) {
    if (!font || max_width <= 0) return {};
    if (text_width(font, s) <= max_width) return s;
    const std::string suffix = "...";
    for (std::size_t n = s.size(); n > 0; --n) {
        std::string candidate = s.substr(0, n) + suffix;
        if (text_width(font, candidate) <= max_width) return candidate;
    }
    return suffix;
}

std::string elide_tail(XFontStruct* font, const std::string& s, int max_width) {
    if (!font || max_width <= 0) return {};
    if (text_width(font, s) <= max_width) return s;
    const std::string prefix = "...";
    for (std::size_t start = 0; start < s.size(); ++start) {
        std::string candidate = prefix + s.substr(start);
        if (text_width(font, candidate) <= max_width) return candidate;
    }
    return prefix;
}

std::string lowercase(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

std::string clean_pasted_text(std::string value) {
    const auto nul = value.find('\0');
    if (nul != std::string::npos) value.resize(nul);
    const auto line = value.find_first_of("\r\n");
    if (line != std::string::npos) value.resize(line);
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) --end;
    return value.substr(begin, end - begin);
}

unsigned long visual_pixel(Visual* visual, int r, int g, int b) {
    auto channel = [](unsigned long mask, int value) -> unsigned long {
        if (!mask) return 0;
        int shift = 0;
        while (((mask >> shift) & 1UL) == 0 && shift < 31) ++shift;
        const unsigned long scaled =
            (mask >> shift) == 255 ? static_cast<unsigned long>(value)
                                    : (static_cast<unsigned long>(value) * (mask >> shift)) / 255;
        return (scaled << shift) & mask;
    };
    return channel(visual->red_mask, r) |
           channel(visual->green_mask, g) |
           channel(visual->blue_mask, b);
}

void draw_button_icon(Display* d, Window w, GC gc, Btn b, int x, int cy,
                      unsigned long color) {
    XSetForeground(d, gc, color);
    XSetLineAttributes(d, gc, 1, LineSolid, CapRound, JoinRound);
    switch (b) {
    case Btn::Install:
    case Btn::Save:
        XDrawLine(d, w, gc, x + 7, cy - 7, x + 7, cy + 3);
        XDrawLine(d, w, gc, x + 2, cy - 2, x + 7, cy + 3);
        XDrawLine(d, w, gc, x + 12, cy - 2, x + 7, cy + 3);
        XDrawLine(d, w, gc, x, cy + 8, x + 14, cy + 8);
        break;
    case Btn::Update:
    case Btn::UpdateAll:
        XDrawLine(d, w, gc, x + 7, cy + 7, x + 7, cy - 7);
        XDrawLine(d, w, gc, x + 2, cy - 2, x + 7, cy - 7);
        XDrawLine(d, w, gc, x + 12, cy - 2, x + 7, cy - 7);
        if (b == Btn::UpdateAll) XDrawLine(d, w, gc, x + 1, cy + 7, x + 13, cy + 7);
        break;
    case Btn::Remove:
        XDrawRectangle(d, w, gc, x + 2, cy - 3, 10, 9);
        XDrawLine(d, w, gc, x, cy - 6, x + 14, cy - 6);
        XDrawLine(d, w, gc, x + 5, cy - 8, x + 9, cy - 8);
        XDrawLine(d, w, gc, x + 5, cy, x + 5, cy + 4);
        XDrawLine(d, w, gc, x + 9, cy, x + 9, cy + 4);
        break;
    case Btn::SelectAll:
        XDrawLine(d, w, gc, x + 1, cy, x + 5, cy + 4);
        XDrawLine(d, w, gc, x + 5, cy + 4, x + 13, cy - 6);
        break;
    case Btn::Clear:
    case Btn::Cancel:
        XDrawLine(d, w, gc, x + 2, cy - 6, x + 12, cy + 6);
        XDrawLine(d, w, gc, x + 12, cy - 6, x + 2, cy + 6);
        break;
    case Btn::Settings:
        XDrawArc(d, w, gc, x + 1, cy - 6, 12, 12, 0, 360 * 64);
        XDrawLine(d, w, gc, x + 7, cy - 9, x + 7, cy - 6);
        XDrawLine(d, w, gc, x + 7, cy + 6, x + 7, cy + 9);
        XDrawLine(d, w, gc, x - 2, cy, x + 1, cy);
        XDrawLine(d, w, gc, x + 13, cy, x + 16, cy);
        break;
    case Btn::FilterAll:
    case Btn::FilterUpdates:
        XDrawLine(d, w, gc, x, cy - 7, x + 14, cy - 7);
        XDrawLine(d, w, gc, x + 14, cy - 7, x + 9, cy - 1);
        XDrawLine(d, w, gc, x + 9, cy - 1, x + 9, cy + 6);
        XDrawLine(d, w, gc, x + 9, cy + 6, x + 5, cy + 8);
        XDrawLine(d, w, gc, x + 5, cy + 8, x + 5, cy - 1);
        XDrawLine(d, w, gc, x + 5, cy - 1, x, cy - 7);
        break;
    case Btn::Sort:
        XDrawLine(d, w, gc, x + 4, cy + 7, x + 4, cy - 7);
        XDrawLine(d, w, gc, x + 1, cy - 3, x + 4, cy - 7);
        XDrawLine(d, w, gc, x + 4, cy - 7, x + 7, cy - 3);
        XDrawLine(d, w, gc, x + 10, cy - 7, x + 10, cy + 7);
        XDrawLine(d, w, gc, x + 7, cy + 3, x + 10, cy + 7);
        XDrawLine(d, w, gc, x + 10, cy + 7, x + 13, cy + 3);
        break;
    case Btn::Check:
        XDrawArc(d, w, gc, x + 1, cy - 6, 12, 13, 45 * 64, 270 * 64);
        XDrawLine(d, w, gc, x + 7, cy - 8, x + 7, cy - 4);
        XDrawLine(d, w, gc, x + 7, cy - 8, x + 11, cy - 6);
        break;
    default:
        break;
    }
}

XImage* make_icon_image(Display* d, int scr, const ModIcon& icon) {
    Visual* visual = DefaultVisual(d, scr);
    if (!visual || visual->c_class != TrueColor || icon.size <= 0) return nullptr;

    XImage* image = XCreateImage(d, visual, static_cast<unsigned>(DefaultDepth(d, scr)),
                                 ZPixmap, 0, nullptr, icon.size, icon.size, 32, 0);
    if (!image) return nullptr;
    image->data = static_cast<char*>(
        std::calloc(static_cast<std::size_t>(image->bytes_per_line),
                    static_cast<std::size_t>(image->height)));
    if (!image->data) {
        XDestroyImage(image);
        return nullptr;
    }

    for (int y = 0; y < icon.size; ++y) {
        for (int x = 0; x < icon.size; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * icon.size + x) * 4;
            const int a = icon.rgba[p + 3];
            const int r = (icon.rgba[p] * a + 255 * (255 - a)) / 255;
            const int g = (icon.rgba[p + 1] * a + 255 * (255 - a)) / 255;
            const int b = (icon.rgba[p + 2] * a + 255 * (255 - a)) / 255;
            XPutPixel(image, x, y, visual_pixel(visual, r, g, b));
        }
    }
    return image;
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

    const int scr = DefaultScreen(dpy);
    Palette pal;
    pal.bg = alloc_color(dpy, scr, 246, 247, 249);
    pal.surface = alloc_color(dpy, scr, 255, 255, 255);
    pal.field = alloc_color(dpy, scr, 255, 255, 255);
    pal.border = alloc_color(dpy, scr, 220, 225, 232);
    pal.border_strong = alloc_color(dpy, scr, 194, 202, 214);
    pal.text = alloc_color(dpy, scr, 31, 38, 46);
    pal.muted = alloc_color(dpy, scr, 108, 118, 132);
    pal.accent = alloc_color(dpy, scr, 224, 100, 43);
    pal.accent_hover = alloc_color(dpy, scr, 198, 82, 30);
    pal.accent_soft = alloc_color(dpy, scr, 255, 242, 234);
    pal.danger = alloc_color(dpy, scr, 190, 55, 55);
    pal.danger_soft = alloc_color(dpy, scr, 253, 237, 237);
    pal.hover = alloc_color(dpy, scr, 248, 250, 252);
    pal.success = alloc_color(dpy, scr, 36, 122, 80);

    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 80, 60, kW, kH, 1,
                                     pal.border, pal.bg);
    XStoreName(dpy, win, "PZ Mod Manager");
    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask |
                     PointerMotionMask | LeaveWindowMask);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XMapWindow(dpy, win);

    Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom paste_prop = XInternAtom(dpy, "PZMODMGR_PASTE", False);

    auto load_font = [&](const char* pattern, XFontStruct* fallback) -> XFontStruct* {
        XFontStruct* loaded = XLoadQueryFont(dpy, pattern);
        return loaded ? loaded : fallback;
    };
    XFontStruct* body = XLoadQueryFont(
        dpy, "-misc-fixed-medium-r-normal--13-120-75-75-c-60-iso8859-1");
    if (!body) body = XLoadQueryFont(dpy, "fixed");
    if (!body) {
        std::fprintf(stderr, "Cannot load an X11 font\n");
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        ::close(wake[0]);
        ::close(wake[1]);
        return 1;
    }
    XFontStruct* heading = load_font(
        "-misc-fixed-bold-r-normal--18-120-75-75-c-80-iso8859-1", body);
    if (heading == body)
        heading = load_font("-misc-fixed-medium-r-normal--15-120-75-75-c-80-iso8859-1",
                            body);

    std::deque<std::string> log;
    std::string input;
    bool busy = false;

    enum class PasteField { Input, Filter, SettingsPath, SettingsUser };
    PasteField paste_field = PasteField::Input;
    Atom paste_selection = None;
    Time paste_time = CurrentTime;
    bool paste_pending = false;
    bool paste_fallback = false;

    std::set<std::uint64_t> selected_ids;
    bool confirm_remove = false;
    std::string filter_text;
    bool only_updates = false;
    bool filter_focus = false;
    int sort_mode = 0;   // 0 = name, 1 = updated
    int list_offset = 0;
    int hover_button = -1;
    int hover_row = -1;
    int anchor_index = -1;
    int last_click_index = -1;
    std::chrono::steady_clock::time_point last_click_time;
    std::vector<InstalledMod> visible;

    bool settings_open = false;
    int settings_field = 0;
    std::string settings_path, settings_user;

    std::map<std::string, XImage*> icon_cache;

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

    auto button_rect = [&](Btn b) -> Rect {
        if (settings_open) {
            if (b == Btn::Save) return {kSettingsX + 32, kSettingsY + 300, 120, 38};
            if (b == Btn::Cancel) return {kSettingsX + 168, kSettingsY + 300, 120, 38};
            return {};
        }
        switch (b) {
        case Btn::Install: return {kInstallX, kInputY, kInstallW, kInputH};
        case Btn::Settings: return {880, 24, 96, 30};
        case Btn::Update: return {24, kActionY, 100, kActionH};
        case Btn::Remove: return {136, kActionY, 130, kActionH};
        case Btn::UpdateAll: return {278, kActionY, 110, kActionH};
        case Btn::SelectAll: return {400, kActionY, 90, kActionH};
        case Btn::Clear: return {502, kActionY, 60, kActionH};
        case Btn::FilterAll: return {312, kFilterY, 60, kFilterH};
        case Btn::FilterUpdates: return {378, kFilterY, 112, kFilterH};
        case Btn::Sort: return {506, kFilterY, 140, kFilterH};
        case Btn::Check: return {660, kFilterY, 100, kFilterH};
        default: return {};
        }
    };

    auto button_enabled = [&](Btn b) -> bool {
        if (b == Btn::Nothing) return false;
        if (settings_open)
            return b == Btn::Save || b == Btn::Cancel;
        if (busy) return false;
        switch (b) {
        case Btn::Install:
        case Btn::Settings:
        case Btn::UpdateAll: return true;
        case Btn::Update:
        case Btn::Remove:
        case Btn::Clear: return !selected_ids.empty();
        case Btn::SelectAll: return !visible.empty();
        case Btn::FilterAll:
        case Btn::FilterUpdates:
        case Btn::Sort:
        case Btn::Check: return true;
        default: return false;
        }
    };

    auto button_label = [&](Btn b) -> std::string {
        switch (b) {
        case Btn::Install: return "Install";
        case Btn::Settings: return "Settings";
        case Btn::Update: return "Update";
        case Btn::Remove: return confirm_remove ? "Confirm remove" : "Remove";
        case Btn::UpdateAll: return "Update all";
        case Btn::SelectAll: return "Select all";
        case Btn::Clear: return "Clear";
        case Btn::Save: return "Save";
        case Btn::Cancel: return "Cancel";
        case Btn::FilterAll: return "All";
        case Btn::FilterUpdates: return "Updates";
        case Btn::Sort: return sort_mode == 0 ? "Sort: Name" : "Sort: Updated";
        case Btn::Check: return "Check";
        default: return {};
        }
    };

    auto button_at = [&](int x, int y) -> Btn {
        if (settings_open) {
            for (Btn b : {Btn::Save, Btn::Cancel}) {
                if (!button_enabled(b)) continue;
                Rect r = button_rect(b);
                if (in_rect(x, y, r.x, r.y, r.w, r.h)) return b;
            }
            return Btn::Nothing;
        }
        for (Btn b : {Btn::Install, Btn::Settings, Btn::Update, Btn::Remove,
                      Btn::UpdateAll, Btn::SelectAll, Btn::Clear, Btn::FilterAll,
                      Btn::FilterUpdates, Btn::Sort, Btn::Check}) {
            if (!button_enabled(b)) continue;
            Rect r = button_rect(b);
            if (in_rect(x, y, r.x, r.y, r.w, r.h)) return b;
        }
        return Btn::Nothing;
    };

    auto draw_button = [&](Btn b) {
        Rect r = button_rect(b);
        if (r.w == 0) return;
        const bool enabled = button_enabled(b);
        const bool primary = b == Btn::Install || b == Btn::Save;
        const bool danger = b == Btn::Remove && confirm_remove;
        const bool chip = b == Btn::FilterAll || b == Btn::FilterUpdates;
        const bool active = (b == Btn::FilterAll && !only_updates) ||
                            (b == Btn::FilterUpdates && only_updates);
        const bool hot = enabled && hover_button == static_cast<int>(b);
        unsigned long fill_color = pal.surface;
        unsigned long border_color = pal.border;
        unsigned long text_color = pal.text;
        if (!enabled) {
            fill_color = pal.bg;
            text_color = pal.muted;
        } else if (danger) {
            fill_color = pal.danger_soft;
            border_color = pal.danger;
            text_color = pal.danger;
        } else if (chip && active) {
            fill_color = pal.accent_soft;
            border_color = pal.accent;
            text_color = pal.accent;
        } else if (primary) {
            fill_color = hot ? pal.accent_hover : pal.accent;
            border_color = fill_color;
            text_color = pal.surface;
        } else if (hot) {
            fill_color = pal.hover;
            border_color = pal.border_strong;
        }
        fill(dpy, win, gc, r.x, r.y, r.w, r.h, fill_color);
        frame(dpy, win, gc, r.x, r.y, r.w, r.h, border_color);
        const std::string label = button_label(b);
        const int icon_w = 14, gap = 6;
        const int start = r.x + (r.w - icon_w - gap - text_width(body, label)) / 2;
        draw_button_icon(dpy, win, gc, b, start, r.y + r.h / 2, text_color);
        text(dpy, win, gc, body, start + icon_w + gap, baseline_for(body, r), label,
             text_color);
        XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
    };

    auto clear_icons = [&]() {
        for (auto& [folder, image] : icon_cache) {
            (void)folder;
            if (image) XDestroyImage(image);
        }
        icon_cache.clear();
    };

    auto get_icon = [&](const InstalledMod& m) -> XImage* {
        auto it = icon_cache.find(m.folder);
        if (it != icon_cache.end()) return it->second;
        const fs::path path = mm.icon_for(m.folder);
        XImage* image = nullptr;
        if (!path.empty()) {
            ModIcon icon;
            if (load_mod_icon(path, 48, icon)) image = make_icon_image(dpy, scr, icon);
        }
        icon_cache[m.folder] = image;
        return image;
    };

    auto draw_input = [&](const Rect& r, const std::string& value,
                          const std::string& placeholder, bool focused,
                          bool enabled) {
        fill(dpy, win, gc, r.x, r.y, r.w, r.h, pal.field);
        frame(dpy, win, gc, r.x, r.y, r.w, r.h,
              focused && enabled ? pal.accent : pal.border);
        const int inner_w = r.w - 24;
        const bool empty = value.empty();
        const std::string shown = empty ? elide(body, placeholder, inner_w)
                                        : elide_tail(body, value, inner_w);
        text(dpy, win, gc, body, r.x + 12, baseline_for(body, r), shown,
             empty ? pal.muted : pal.text);
        if (focused && enabled) {
            const int caret_x = r.x + 12 + text_width(body, empty ? "" : shown) + 1;
            fill(dpy, win, gc, caret_x, r.y + 10, 2, r.h - 20, pal.accent);
        }
    };

    auto row_at = [&](int x, int y) -> int {
        if (x < kMargin || x > kMargin + kCardW || y < kListTop || y > kListBottom)
            return -1;
        const int slot = (y - kListTop) / (kRowH + kRowGap);
        if (slot < 0 || slot >= kRows) return -1;
        const int within = (y - kListTop) % (kRowH + kRowGap);
        if (within >= kRowH) return -1;
        const int index = list_offset + slot;
        return index >= 0 && index < static_cast<int>(visible.size()) ? index : -1;
    };

    auto draw_rounded_check = [&](int cx, int cy, bool selected) {
        if (selected) {
            XSetForeground(dpy, gc, pal.accent);
            XFillArc(dpy, win, gc, cx - 10, cy - 10, 20, 20, 0, 360 * 64);
            XSetForeground(dpy, gc, pal.surface);
            XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
            XDrawLine(dpy, win, gc, cx - 4, cy, cx - 1, cy + 3);
            XDrawLine(dpy, win, gc, cx - 1, cy + 3, cx + 5, cy - 4);
            XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
        } else {
            XSetForeground(dpy, gc, pal.border_strong);
            XDrawArc(dpy, win, gc, cx - 10, cy - 10, 20, 20, 0, 360 * 64);
        }
    };

    auto draw_settings = [&]() {
        fill(dpy, win, gc, 0, 0, kW, kH, pal.bg);
        text(dpy, win, gc, heading, kMargin, 42, "Settings", pal.text);
        text(dpy, win, gc, body, kMargin, 62,
             "Configure steamcmd and the account used for Workshop downloads.", pal.muted);
        fill(dpy, win, gc, kSettingsX, kSettingsY, kSettingsW, kSettingsH, pal.surface);
        frame(dpy, win, gc, kSettingsX, kSettingsY, kSettingsW, kSettingsH, pal.border);

        const int x = kSettingsX + 32;
        const int path_y = kSettingsY + 104;
        const int user_y = kSettingsY + 178;
        text(dpy, win, gc, body, x, kSettingsY + 92, "steamcmd executable", pal.text);
        text(dpy, win, gc, body, x + 210, kSettingsY + 92,
             "Blank uses the built-in copy", pal.muted);
        draw_input({x, path_y, kSettingsW - 64, 38}, settings_path,
                   "Built-in steamcmd", settings_field == 0, true);

        text(dpy, win, gc, body, x, kSettingsY + 166, "Steam username", pal.text);
        draw_input({x, user_y, kSettingsW - 64, 38}, settings_user,
                   "anonymous", settings_field == 1, true);
        text(dpy, win, gc, body, x, kSettingsY + 246,
             "No password is stored. Run steamcmd manually once so its login is cached.",
             pal.muted);
        text(dpy, win, gc, body, x, kSettingsY + 266,
             "Use anonymous for public Workshop items.", pal.muted);
        draw_button(Btn::Save);
        draw_button(Btn::Cancel);
        XFlush(dpy);
    };

    auto redraw = [&]() {
        fill(dpy, win, gc, 0, 0, kW, kH, pal.bg);
        if (settings_open) {
            draw_settings();
            return;
        }

        fill(dpy, win, gc, kMargin, 24, 4, 24, pal.accent);
        text(dpy, win, gc, heading, kMargin + 14, 42, "PZ Mod Manager", pal.text);
        text(dpy, win, gc, body, kMargin + 14, 62,
             "Project Zomboid Workshop installer", pal.muted);
        draw_button(Btn::Settings);

        draw_input({kInputX, kInputY, kInputW, kInputH}, input,
                   "Paste a Workshop URL or numeric ID", !filter_focus && !busy, !busy);
        draw_button(Btn::Install);
        text(dpy, win, gc, body, kMargin, 142,
             "Press Enter to install. Collections are supported.", pal.muted);

        if (!busy) {
            const std::string needle = lowercase(filter_text);
            visible.clear();
            for (const auto& m : mm.list()) {
                if (only_updates && !mm.update_available(m.workshop_id)) continue;
                if (!needle.empty()) {
                    std::string haystack = m.name + " " + m.folder;
                    if (const auto info = mm.info_for(m.folder)) {
                        haystack += " " + info->author + " " + info->mod_version;
                    }
                    if (lowercase(haystack).find(needle) == std::string::npos) continue;
                }
                visible.push_back(m);
            }
            if (sort_mode == 0) {
                std::stable_sort(visible.begin(), visible.end(),
                    [](const InstalledMod& a, const InstalledMod& b) {
                        return lowercase(a.name) < lowercase(b.name);
                    });
            } else {
                std::stable_sort(visible.begin(), visible.end(),
                    [](const InstalledMod& a, const InstalledMod& b) {
                        if (a.time_updated != b.time_updated)
                            return a.time_updated > b.time_updated;
                        return lowercase(a.name) < lowercase(b.name);
                    });
            }
            const int total = static_cast<int>(visible.size());
            list_offset = std::min(std::max(0, list_offset),
                                   std::max(0, total - kRows));
            std::set<std::uint64_t> present;
            for (const auto& m : visible) present.insert(m.workshop_id);
            for (auto it = selected_ids.begin(); it != selected_ids.end();) {
                if (present.count(*it) == 0) it = selected_ids.erase(it);
                else ++it;
            }
            if (selected_ids.empty()) confirm_remove = false;
        } else {
            visible.clear();
        }

        draw_input({kMargin, kFilterY, 280, kFilterH}, filter_text,
                   "Filter name, author, version, or folder", filter_focus && !busy,
                   !busy);
        draw_button(Btn::FilterAll);
        draw_button(Btn::FilterUpdates);
        draw_button(Btn::Sort);
        draw_button(Btn::Check);
        const std::string count = busy ? "Working..."
                                       : std::to_string(visible.size()) + " shown";
        text(dpy, win, gc, body, kMargin + kCardW - text_width(body, count),
             kFilterY + 17, count, pal.muted);

        if (busy) {
            fill(dpy, win, gc, kMargin, kListTop, kCardW, 64, pal.surface);
            frame(dpy, win, gc, kMargin, kListTop, kCardW, 64, pal.border);
            text(dpy, win, gc, body, kMargin + 20, kListTop + 38,
                 "Working - steamcmd may take a while...", pal.muted);
        } else if (visible.empty()) {
            fill(dpy, win, gc, kMargin, kListTop, kCardW, 64, pal.surface);
            frame(dpy, win, gc, kMargin, kListTop, kCardW, 64, pal.border);
            const bool filtered = !filter_text.empty() || only_updates;
            text(dpy, win, gc, body, kMargin + 20, kListTop + 38,
                 filtered ? "No mods match the current filters."
                          : "No mods installed yet. Paste a Workshop link above to begin.",
                 pal.muted);
        } else {
            for (int slot = 0; slot < kRows; ++slot) {
                const int index = list_offset + slot;
                if (index >= static_cast<int>(visible.size())) break;
                const auto& m = visible[static_cast<std::size_t>(index)];
                const Rect card{kMargin, kListTop + slot * (kRowH + kRowGap),
                                kCardW, kRowH};
                const bool selected = selected_ids.count(m.workshop_id) != 0;
                const bool hot = hover_row == index;
                unsigned long card_fill = pal.surface;
                unsigned long card_border = pal.border;
                if (selected) {
                    card_fill = pal.accent_soft;
                    card_border = pal.accent;
                } else if (hot) {
                    card_fill = pal.hover;
                    card_border = pal.border_strong;
                }
                fill(dpy, win, gc, card.x, card.y, card.w, card.h, card_fill);
                frame(dpy, win, gc, card.x, card.y, card.w, card.h, card_border);

                XImage* image = get_icon(m);
                const int icon_x = card.x + 10, icon_y = card.y + 8;
                if (image) {
                    XPutImage(dpy, win, gc, image, 0, 0, icon_x, icon_y,
                              static_cast<unsigned>(image->width),
                              static_cast<unsigned>(image->height));
                } else {
                    fill(dpy, win, gc, icon_x, icon_y, 48, 48, pal.accent_soft);
                    const std::string initial = m.name.empty() ? "?" : m.name.substr(0, 1);
                    text(dpy, win, gc, heading,
                         icon_x + (48 - text_width(heading, initial)) / 2,
                         icon_y + (48 + heading->ascent - heading->descent) / 2,
                         initial, pal.accent);
                }

                const bool has_update = mm.update_available(m.workshop_id);
                const int text_x = card.x + 72;
                const int text_w = card.w - (has_update ? 190 : 120);
                text(dpy, win, gc, heading, text_x, card.y + 26,
                     elide(heading, m.name.empty() ? m.folder : m.name, text_w), pal.text);
                const auto info = mm.info_for(m.folder);
                std::string meta;
                if (info) {
                    if (!info->author.empty()) meta += info->author + "  |  ";
                    if (!info->mod_version.empty()) meta += "v" + info->mod_version + "  |  ";
                }
                meta += m.folder;
                if (info && !supports_game_version(*info, mm.game_version())) {
                    if (!info->version_min.empty() && !info->version_max.empty())
                        meta += "  |  PZ " + info->version_min + "-" + info->version_max;
                    else if (!info->version_min.empty())
                        meta += "  |  PZ " + info->version_min + "+";
                    else if (!info->version_max.empty())
                        meta += "  |  PZ up to " + info->version_max;
                }
                text(dpy, win, gc, body, text_x, card.y + 46,
                     elide(body, meta, text_w), pal.muted);
                if (has_update) {
                    const Rect pill{card.x + card.w - 124, card.y + 22, 64, 20};
                    fill(dpy, win, gc, pill.x, pill.y, pill.w, pill.h, pal.accent);
                    text(dpy, win, gc, body,
                         pill.x + (pill.w - text_width(body, "UPDATE")) / 2,
                         pill.y + 14, "UPDATE", pal.surface);
                }
                draw_rounded_check(card.x + card.w - 30, card.y + card.h / 2, selected);
            }
        }

        draw_button(Btn::Update);
        draw_button(Btn::Remove);
        draw_button(Btn::UpdateAll);
        draw_button(Btn::SelectAll);
        draw_button(Btn::Clear);
        const std::string selected = selected_ids.empty()
            ? "No selection"
            : std::to_string(selected_ids.size()) + " selected";
        text(dpy, win, gc, body, 600, kActionY + 21, selected, pal.muted);

        fill(dpy, win, gc, kMargin, kActivityY, kCardW, kActivityH, pal.surface);
        frame(dpy, win, gc, kMargin, kActivityY, kCardW, kActivityH, pal.border);
        text(dpy, win, gc, body, kMargin + 12, kActivityY + 20, "ACTIVITY", pal.muted);
        const int max_lines = 3;
        int start = static_cast<int>(log.size()) - max_lines;
        if (start < 0) start = 0;
        int ly = kActivityY + 40;
        for (int i = start; i < static_cast<int>(log.size()); ++i, ly += 16)
            text(dpy, win, gc, body, kMargin + 12, ly,
                 elide(body, log[static_cast<std::size_t>(i)], kCardW - 24), pal.muted);
        if (log.empty())
            text(dpy, win, gc, body, kMargin + 12, kActivityY + 40,
                 "Ready for a Workshop link.", pal.muted);

        text(dpy, win, gc, body, kMargin, kStatusY,
             busy ? "Working - steamcmd may take a while" : "Ready",
             busy ? pal.accent : pal.muted);
        const std::string status = std::to_string(visible.size()) +
                                   (visible.size() == 1 ? " mod" : " mods");
        text(dpy, win, gc, body, kMargin + kCardW - text_width(body, status), kStatusY,
             status, pal.muted);

        // Xlib buffers requests; flush before blocking in poll.
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

    auto submit = [&]() {
        if (busy) return;
        if (input.empty()) {
            push("Enter a Workshop URL or ID first.");
            redraw();
            return;
        }
        std::string line = "add " + input;
        input.clear();
        start_command(line);
    };

    auto remove_selected = [&]() {
        if (busy) return;
        if (selected_ids.empty()) {
            push("Select a mod first.");
            redraw();
            return;
        }
        if (!confirm_remove) {
            confirm_remove = true;
            push("Click Confirm remove again to delete " +
                 std::to_string(selected_ids.size()) + " selected mod(s).");
            redraw();
            return;
        }
        start_command(build_command("remove", selected_ids));
    };

    auto open_settings = [&]() {
        if (busy) return;
        settings_path = mm.config().steamcmd_exe.string();
        settings_user = mm.config().steam_user;
        settings_field = 0;
        settings_open = true;
        filter_focus = false;
        hover_button = -1;
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

    auto paste_into_field = [&](const std::string& raw) {
        const std::string value = clean_pasted_text(raw);
        if (value.empty()) {
            push("Clipboard is empty.");
            redraw();
            return;
        }
        if (settings_open) {
            if (paste_field == PasteField::SettingsPath) settings_path += value;
            else settings_user += value;
        } else if (paste_field == PasteField::Filter) {
            filter_text += value;
        } else {
            input += value;
        }
        redraw();
    };

    auto request_paste = [&](Atom selection, bool fallback, Time time) {
        if (busy) return;
        paste_field = settings_open
            ? (settings_field == 0 ? PasteField::SettingsPath : PasteField::SettingsUser)
            : (filter_focus ? PasteField::Filter : PasteField::Input);
        paste_selection = selection;
        paste_time = time;
        paste_fallback = fallback;
        paste_pending = true;
        XConvertSelection(dpy, selection, fallback ? XA_STRING : utf8, paste_prop, win,
                          time);
        XFlush(dpy);
    };

    push("Paste a Workshop URL or ID, then press Enter.");
    push("Click a mod to select it; Ctrl/Shift-click for more.");
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
            case MotionNotify: {
                const Btn next_button = button_at(ev.xmotion.x, ev.xmotion.y);
                const int next_row = row_at(ev.xmotion.x, ev.xmotion.y);
                if (static_cast<int>(next_button) != hover_button || next_row != hover_row) {
                    hover_button = static_cast<int>(next_button);
                    hover_row = next_row;
                    redraw();
                }
                break;
            }
            case LeaveNotify:
                if (hover_button != -1 || hover_row != -1) {
                    hover_button = -1;
                    hover_row = -1;
                    redraw();
                }
                break;
            case ButtonPress: {
                if (busy) break;
                const int x = ev.xbutton.x, y = ev.xbutton.y;
                if (settings_open) {
                    const Btn b = button_at(x, y);
                    if (b == Btn::Save) {
                        save_settings();
                        break;
                    }
                    if (b == Btn::Cancel) {
                        settings_open = false;
                        redraw();
                        break;
                    }
                    if (ev.xbutton.button == Button2) {
                        if (in_rect(x, y, kSettingsX + 32, kSettingsY + 104,
                                    kSettingsW - 64, 38)) {
                            settings_field = 0;
                            request_paste(XA_PRIMARY, false, ev.xbutton.time);
                        } else if (in_rect(x, y, kSettingsX + 32, kSettingsY + 178,
                                           kSettingsW - 64, 38)) {
                            settings_field = 1;
                            request_paste(XA_PRIMARY, false, ev.xbutton.time);
                        }
                        redraw();
                        break;
                    }
                    if (in_rect(x, y, kSettingsX + 32, kSettingsY + 104,
                                kSettingsW - 64, 38))
                        settings_field = 0;
                    else if (in_rect(x, y, kSettingsX + 32, kSettingsY + 178,
                                     kSettingsW - 64, 38))
                        settings_field = 1;
                    redraw();
                    break;
                }

                if (ev.xbutton.button == Button2) {
                    if (in_rect(x, y, kInputX, kInputY, kInputW, kInputH)) {
                        filter_focus = false;
                        request_paste(XA_PRIMARY, false, ev.xbutton.time);
                    } else if (in_rect(x, y, kMargin, kFilterY, 280, kFilterH)) {
                        filter_focus = true;
                        request_paste(XA_PRIMARY, false, ev.xbutton.time);
                    }
                    break;
                }
                if (ev.xbutton.button == Button4) {
                    if (row_at(x, y) >= 0 || in_rect(x, y, kMargin, kListTop, kCardW,
                                                    kListBottom - kListTop))
                        list_offset = std::max(0, list_offset - 1);
                    redraw();
                    break;
                }
                if (ev.xbutton.button == Button5) {
                    if (row_at(x, y) >= 0 || in_rect(x, y, kMargin, kListTop, kCardW,
                                                    kListBottom - kListTop))
                        list_offset += 1;
                    redraw();
                    break;
                }

                if (in_rect(x, y, kInputX, kInputY, kInputW, kInputH)) {
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (in_rect(x, y, kMargin, kFilterY, 280, kFilterH)) {
                    filter_focus = true;
                    redraw();
                    break;
                }

                const Btn b = button_at(x, y);
                if (b == Btn::FilterAll) {
                    only_updates = false;
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (b == Btn::FilterUpdates) {
                    only_updates = true;
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (b == Btn::Sort) {
                    sort_mode = 1 - sort_mode;
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (b == Btn::Check) {
                    start_command("check");
                    break;
                }
                if (b == Btn::Install) {
                    submit();
                    break;
                }
                if (b == Btn::Settings) {
                    open_settings();
                    break;
                }
                if (b == Btn::Update) {
                    if (!selected_ids.empty())
                        start_command(build_command("update", selected_ids));
                    break;
                }
                if (b == Btn::Remove) {
                    remove_selected();
                    break;
                }
                if (b == Btn::UpdateAll) {
                    start_command("update");
                    break;
                }
                if (b == Btn::SelectAll) {
                    for (const auto& m : visible) selected_ids.insert(m.workshop_id);
                    confirm_remove = false;
                    redraw();
                    break;
                }
                if (b == Btn::Clear) {
                    selected_ids.clear();
                    confirm_remove = false;
                    redraw();
                    break;
                }

                const int index = row_at(x, y);
                if (index >= 0) {
                    const auto id = visible[static_cast<std::size_t>(index)].workshop_id;
                    const auto now = std::chrono::steady_clock::now();
                    const bool double_click =
                        last_click_index == index &&
                        now - last_click_time < std::chrono::milliseconds(450);
                    if (double_click) {
                        selected_ids.clear();
                        selected_ids.insert(id);
                        last_click_index = -1;
                        start_command("update " + std::to_string(id));
                        break;
                    }

                    if (ev.xbutton.state & ShiftMask) {
                        if (anchor_index >= 0) {
                            selected_ids.clear();
                            const int lo = std::min(anchor_index, index);
                            const int hi = std::max(anchor_index, index);
                            for (int i = lo; i <= hi && i < static_cast<int>(visible.size());
                                 ++i)
                                selected_ids.insert(visible[static_cast<std::size_t>(i)].workshop_id);
                        } else {
                            selected_ids.insert(id);
                            anchor_index = index;
                        }
                    } else if (ev.xbutton.state & ControlMask) {
                        if (selected_ids.count(id)) selected_ids.erase(id);
                        else selected_ids.insert(id);
                        anchor_index = index;
                    } else {
                        selected_ids.clear();
                        selected_ids.insert(id);
                        anchor_index = index;
                    }
                    last_click_index = index;
                    last_click_time = now;
                    confirm_remove = false;
                    redraw();
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
            case SelectionNotify: {
                if (!paste_pending || ev.xselection.selection != paste_selection) break;
                if (ev.xselection.property == None) {
                    if (!paste_fallback) {
                        request_paste(paste_selection, true, paste_time);
                    } else {
                        paste_pending = false;
                        push("Nothing to paste from the clipboard.");
                        redraw();
                    }
                    break;
                }

                Atom type;
                int format = 0;
                unsigned long count = 0, remaining = 0;
                unsigned char* data = nullptr;
                int rc = XGetWindowProperty(dpy, win, ev.xselection.property, 0, 65536,
                                            True, AnyPropertyType, &type, &format,
                                            &count, &remaining, &data);
                paste_pending = false;
                if (rc != Success || !data || format != 8) {
                    if (data) XFree(data);
                    push("Could not read the clipboard.");
                    redraw();
                    break;
                }
                std::string value(reinterpret_cast<char*>(data),
                                  static_cast<std::size_t>(count));
                XFree(data);
                paste_into_field(value);
                break;
            }
            case KeyPress: {
                if (busy) break;
                char buf[32];
                KeySym ks;
                int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, nullptr);
                const bool ctrl = (ev.xkey.state & ControlMask) != 0;
                const bool shift = (ev.xkey.state & ShiftMask) != 0;
                if (ctrl && (ks == XK_v || ks == XK_V)) {
                    request_paste(clipboard, false, ev.xkey.time);
                    break;
                }
                if (shift && ks == XK_Insert) {
                    request_paste(XA_PRIMARY, false, ev.xkey.time);
                    break;
                }

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
                    if (!filter_focus) submit();
                } else if (ks == XK_BackSpace) {
                    if (filter_focus) {
                        if (!filter_text.empty()) filter_text.pop_back();
                    } else if (!input.empty()) {
                        input.pop_back();
                    }
                    redraw();
                } else if (ks == XK_Escape) {
                    if (!filter_text.empty()) {
                        filter_text.clear();
                    } else if (only_updates) {
                        only_updates = false;
                    } else if (!selected_ids.empty()) {
                        selected_ids.clear();
                        confirm_remove = false;
                    } else if (!input.empty()) {
                        input.clear();
                    }
                    redraw();
                } else if (n > 0 && static_cast<unsigned char>(buf[0]) >= 32) {
                    if (filter_focus) filter_text.append(buf, static_cast<std::size_t>(n));
                    else input.append(buf, static_cast<std::size_t>(n));
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
                clear_icons();
                hover_row = -1;
            }
            redraw();
        }
        if (fds[0].revents & POLLIN) handle_events();
    }

    if (worker.joinable()) worker.join();
    clear_icons();
    ::close(wake[0]);
    ::close(wake[1]);
    if (heading != body) XFreeFont(dpy, heading);
    XFreeFont(dpy, body);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
}  // namespace pz
