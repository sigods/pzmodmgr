#include "core/flat_map.hpp"
#include "ui/ui.hpp"
#include "ui/mod_icon.hpp"
#include "ui/glyphs.hpp"
#include "core/commands.hpp"
#include "core/thread.hpp"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/inotify.h>
#endif

namespace pz {
namespace {
constexpr int kW = 920, kH = 720;
constexpr int kMargin = 24;
constexpr int kInputX = 24, kInputY = 84, kInputW = 644, kInputH = 38;
constexpr int kInstallX = 684, kInstallW = 212;
constexpr int kFilterY = 158, kFilterH = 24;
constexpr int kListTop = 192, kRowH = 64, kRowGap = 6, kRows = 5;
constexpr int kCardW = 872;

std::uint64_t monotonic_millis() {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}
// The scrollbar lives in the right margin, outside the rows, so clicking the
// end of a row never pages the list. kScrollGrab adds a little slack around
// the thumb: a near miss starts a drag instead of jumping by a page.
constexpr int kScrollX = kMargin + kCardW + 6, kScrollW = 6, kScrollMinThumb = 24;
constexpr int kScrollGrab = 3;
constexpr int kListBottom = kListTop + kRows * kRowH + (kRows - 1) * kRowGap;
constexpr int kActionY = 554, kActionH = 32;
constexpr int kActivityY = 602, kActivityH = 80;
constexpr int kActivityLines = 3;
constexpr int kActivityTrackTop = kActivityY + 32;
constexpr int kActivityTrackBottom = kActivityY + kActivityH - 8;
// The log bar sits inside the panel, next to its right border, so it reads as
// part of the activity box instead of a second list gutter.
constexpr int kActivityScrollX = kMargin + kCardW - kScrollW - 12;
constexpr int kStatusY = 702;
constexpr int kSettingsX = 130, kSettingsY = 140;
constexpr int kSettingsW = 660, kSettingsH = 430;
constexpr int kSettingsFieldX = kSettingsX + 32;
constexpr int kSettingsFieldW = kSettingsW - 64;
constexpr int kSettingsFieldH = 38;
constexpr int kSettingsZomboidY = kSettingsY + 104;
constexpr int kSettingsSteamcmdY = kSettingsY + 178;
constexpr int kSettingsUserY = kSettingsY + 252;
constexpr int kSettingsButtonsY = kSettingsY + 392;
constexpr int kTargetsRowY = kSettingsY + 104;
constexpr int kTargetsRowH = 36;
constexpr int kTargetsRows = 7;
constexpr int kTargetsButtonsY = kSettingsY + 360;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

struct Palette {
    unsigned long bg, surface, field, border, border_strong, text, muted;
    unsigned long accent, accent_hover, accent_soft, danger, danger_soft;
    unsigned long brand, brand_soft, brand_border, cream, hover, success;
    unsigned long on_accent;
};

enum class Btn {
    Nothing, Install, Settings, Update, Remove, UpdateAll, SelectAll, Clear, Copy,
    Lock, Save, Cancel, Theme, FilterAll, FilterUpdates, Sort, Check, Enable,
    ShareServer, SelfUpdate, EnableTarget, DisableTarget, ShareTargets, CopyNames,
    CloseTargets
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

// The add field accepts several IDs at once, so a pasted list may span lines.
// Collapse whitespace into single spaces instead of keeping only the first
// line.
std::string clean_workshop_paste(std::string value) {
    const auto nul = value.find('\0');
    if (nul != std::string::npos) value.resize(nul);
    std::string out;
    for (char c : value) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
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
    const glyphs::Glyph* glyph = nullptr;
    switch (b) {
    case Btn::Install: glyph = &glyphs::kInstall; break;
    case Btn::Save: glyph = &glyphs::kSave; break;
    case Btn::Update: glyph = &glyphs::kUpdate; break;
    case Btn::UpdateAll: glyph = &glyphs::kUpdateAll; break;
    case Btn::Remove: glyph = &glyphs::kRemove; break;
    case Btn::SelectAll: glyph = &glyphs::kSelectAll; break;
    case Btn::Clear:
    case Btn::Cancel: glyph = &glyphs::kClear; break;
    case Btn::FilterAll:
    case Btn::FilterUpdates: glyph = &glyphs::kFilter; break;
    case Btn::Sort: glyph = &glyphs::kSort; break;
    default: return;   // Check intentionally has no glyph.
    }

    XSetForeground(d, gc, color);
    const int origin_x = x + 1, origin_y = cy - 8;
    glyphs::visit(*glyph, [&](int gx, int gy) {
        XFillRectangle(d, w, gc, origin_x + gx, origin_y + gy, 1, 1);
    });
}

XImage* make_icon_image(Display* d, int scr, const ModIcon& icon,
                        unsigned long background) {
    Visual* visual = DefaultVisual(d, scr);
    if (!visual || visual->c_class != TrueColor || icon.size <= 0) return nullptr;

    XColor bg{};
    bg.pixel = background;
    XQueryColor(d, DefaultColormap(d, scr), &bg);
    const int bg_r = bg.red >> 8;
    const int bg_g = bg.green >> 8;
    const int bg_b = bg.blue >> 8;

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
            const int r = (icon.rgba[p] * a + bg_r * (255 - a)) / 255;
            const int g = (icon.rgba[p + 1] * a + bg_g * (255 - a)) / 255;
            const int b = (icon.rgba[p + 2] * a + bg_b * (255 - a)) / 255;
            XPutPixel(image, x, y, visual_pixel(visual, r, g, b));
        }
    }
    return image;
}

// Upload the icon once and keep only the server-side pixmap. Keeping an
// XImage per mod costs 48x48x4 bytes of client heap and repeats the upload on
// every repaint; a pixmap lets XCopyArea reuse the already converted pixels.
struct CachedIcon {
    Pixmap pixmap = None;
    unsigned width = 0;
    unsigned height = 0;
};

CachedIcon make_icon_pixmap(Display* d, Window w, int scr, GC gc,
                            const ModIcon& icon, unsigned long background) {
    CachedIcon cached;
    XImage* image = make_icon_image(d, scr, icon, background);
    if (!image) return cached;

    const Pixmap pixmap = XCreatePixmap(d, w, static_cast<unsigned>(icon.size),
                                        static_cast<unsigned>(icon.size),
                                        static_cast<unsigned>(DefaultDepth(d, scr)));
    if (pixmap != None) {
        XPutImage(d, pixmap, gc, image, 0, 0, 0, 0,
                  static_cast<unsigned>(icon.size), static_cast<unsigned>(icon.size));
        cached.pixmap = pixmap;
        cached.width = static_cast<unsigned>(icon.size);
        cached.height = static_cast<unsigned>(icon.size);
    }
    XDestroyImage(image);
    return cached;
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
    pal.brand = alloc_color(dpy, scr, 44, 99, 69);
    pal.brand_soft = alloc_color(dpy, scr, 235, 244, 238);
    pal.brand_border = alloc_color(dpy, scr, 182, 204, 188);
    pal.cream = alloc_color(dpy, scr, 255, 248, 234);
    pal.hover = alloc_color(dpy, scr, 244, 248, 245);
    pal.success = alloc_color(dpy, scr, 36, 122, 80);
    pal.on_accent = pal.surface;

    const Palette light_pal = pal;
    Palette dark_pal;
    dark_pal.bg = alloc_color(dpy, scr, 18, 21, 24);
    dark_pal.surface = alloc_color(dpy, scr, 28, 32, 36);
    dark_pal.field = alloc_color(dpy, scr, 22, 26, 30);
    dark_pal.border = alloc_color(dpy, scr, 58, 64, 70);
    dark_pal.border_strong = alloc_color(dpy, scr, 78, 86, 94);
    dark_pal.text = alloc_color(dpy, scr, 232, 236, 240);
    dark_pal.muted = alloc_color(dpy, scr, 154, 163, 173);
    dark_pal.accent = alloc_color(dpy, scr, 232, 116, 58);
    dark_pal.accent_hover = alloc_color(dpy, scr, 245, 132, 72);
    dark_pal.accent_soft = alloc_color(dpy, scr, 58, 36, 27);
    dark_pal.danger = alloc_color(dpy, scr, 230, 100, 100);
    dark_pal.danger_soft = alloc_color(dpy, scr, 52, 28, 30);
    dark_pal.brand = alloc_color(dpy, scr, 96, 180, 130);
    dark_pal.brand_soft = alloc_color(dpy, scr, 26, 46, 36);
    dark_pal.brand_border = alloc_color(dpy, scr, 64, 110, 84);
    dark_pal.cream = alloc_color(dpy, scr, 255, 248, 234);
    dark_pal.hover = alloc_color(dpy, scr, 36, 44, 40);
    dark_pal.success = alloc_color(dpy, scr, 96, 180, 130);
    dark_pal.on_accent = dark_pal.cream;

    bool dark_mode = mm.config().dark_mode;
    pal = dark_mode ? dark_pal : light_pal;

    Window wnd = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 80, 60, kW, kH, 1,
                                     pal.border, pal.bg);
    XStoreName(dpy, wnd, "PZ Mod Manager");

    // The layout is fixed-size. Vanilla dwm treats a window whose min and max
    // sizes match as floating, so this also floats the app without a dwm rule.
    XSizeHints size_hints{};
    size_hints.flags = PMinSize | PMaxSize;
    size_hints.min_width = size_hints.max_width = kW;
    size_hints.min_height = size_hints.max_height = kH;
    XSetWMNormalHints(dpy, wnd, &size_hints);

    XSelectInput(dpy, wnd,
                 ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     StructureNotifyMask | PointerMotionMask | LeaveWindowMask);
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, wnd, &wm_delete, 1);
    GC gc = XCreateGC(dpy, wnd, 0, nullptr);
    XMapWindow(dpy, wnd);

    Atom clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom paste_prop = XInternAtom(dpy, "PZMODMGR_PASTE", False);
    std::string clipboard_text;   // owned while this window owns CLIPBOARD

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
        XDestroyWindow(dpy, wnd);
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

    // Every frame is composed off-screen and copied in one operation. Drawing
    // directly to the window exposed partial frames while scrolling or while
    // the compositor was repainting, which showed up as flickering rows.
    Pixmap backbuf = XCreatePixmap(dpy, wnd, kW, kH, DefaultDepth(dpy, scr));
    if (!backbuf) {
        std::fprintf(stderr, "Cannot create X11 back buffer\n");
        if (heading != body) XFreeFont(dpy, heading);
        XFreeFont(dpy, body);
        XFreeGC(dpy, gc);
        XDestroyWindow(dpy, wnd);
        XCloseDisplay(dpy);
        ::close(wake[0]);
        ::close(wake[1]);
        return 1;
    }
    Drawable win = backbuf;

    std::vector<std::string> log;
    std::string input;
    bool busy = false;
    bool app_update_available = mm.app_update_available();

    enum class PasteField { Input, Filter, SettingsZomboid, SettingsPath, SettingsUser };
    PasteField paste_field = PasteField::Input;
    Atom paste_selection = None;
    Time paste_time = CurrentTime;
    bool paste_pending = false;
    bool paste_fallback = false;

    pz::flat_set<std::uint64_t> selected_ids;
    pz::flat_set<std::string> selected_folders;
    bool confirm_remove = false;
    std::string filter_text;
    bool only_updates = false;
    bool filter_focus = false;
    int sort_mode = 0;   // 0 = name, 1 = updated
    int list_offset = 0;
    bool scroll_drag = false;
    int scroll_drag_y = 0;
    int scroll_drag_offset = 0;
    int activity_offset = 0;   // first visible line; follows the newest by default
    bool activity_follow = true;
    bool activity_drag = false;
    int activity_drag_y = 0;
    int activity_drag_offset = 0;
    int hover_button = -1;
    int hover_row = -1;
    int last_mouse_x = -1;
    int last_mouse_y = -1;
    int anchor_index = -1;
    int last_click_index = -1;
    std::uint64_t last_click_time = 0;
    std::vector<ModRow> visible;
    bool list_dirty = true;
    bool frame_pending = false;

    auto selected_any = [&]() {
        return !selected_ids.empty() || !selected_folders.empty();
    };
    auto row_selected = [&](const ModRow& row) {
        if (row.workshop_id != 0) return selected_ids.count(row.workshop_id) != 0;
        return !row.folders.empty() && selected_folders.count(row.folders.front()) != 0;
    };
    auto select_row = [&](const ModRow& row) {
        if (row.workshop_id != 0) selected_ids.insert(row.workshop_id);
        else if (!row.folders.empty()) selected_folders.insert(row.folders.front());
    };
    auto unselect_row = [&](const ModRow& row) {
        if (row.workshop_id != 0) selected_ids.erase(row.workshop_id);
        else if (!row.folders.empty()) selected_folders.erase(row.folders.front());
    };
    auto clear_all = [&]() {
        selected_ids.clear();
        selected_folders.clear();
    };

    bool settings_open = false;
    bool settings_dark = false;
    bool settings_auto = false;
    int settings_field = 0;
    std::string settings_zomboid, settings_path, settings_user;

    bool targets_open = false;
    int target_focus = -1;
    int target_offset = 0;
    std::vector<ActivationTarget> activation;
    std::vector<std::vector<std::string>> row_mod_ids;
    std::vector<char> row_enabled;
    std::vector<std::string> target_mod_ids;

    std::string dependency_warning;
    std::string dependency_signature;

    pz::flat_map<std::string, CachedIcon> icon_cache;
    pz::flat_map<std::string, CachedIcon> target_icon_cache;
    pz::flat_map<std::string, std::optional<ModInfo>> info_cache;

    pz::mutex pending_mu;
    std::vector<std::string> pending;
    bool worker_done = false;
    thread worker;
    int watch_fd = -1;
    bool reload_pending = false;

    auto push = [&log](const std::string& s) {
        log.push_back(s);
        while (log.size() > 300) log.erase(log.begin());
    };

    auto refresh_dependency_warning = [&]() {
        const auto notice = dependency_notice(mm.missing_requirements());
        if (notice.signature != dependency_signature) {
            if (notice.messages.empty()) {
                if (!dependency_signature.empty()) push("required mods satisfied");
            } else {
                for (const auto& line : notice.messages) push(line);
            }
            dependency_signature = notice.signature;
        }
        dependency_warning = notice.status;
    };

    auto wake_ui = [&]() {
        char b = 1;
        ssize_t ignored = ::write(wake[1], &b, 1);
        (void)ignored;
    };

    auto enqueue = [&](const std::string& s) {
        {
            pz::lock_guard lock(pending_mu);
            pending.push_back(s);
        }
        wake_ui();
    };

    auto drain_pending = [&]() {
        std::vector<std::string> batch;
        {
            pz::lock_guard lock(pending_mu);
            batch.swap(pending);
        }
        for (const auto& s : batch) push(s);
        pz::lock_guard lock(pending_mu);
        bool done = worker_done;
        worker_done = false;
        return done;
    };

    auto build_command = [](const std::string& verb, const pz::flat_set<std::uint64_t>& ids) {
        std::string cmd = verb;
        for (auto id : ids) cmd += " " + std::to_string(id);
        return cmd;
    };

    auto button_rect = [&](Btn b) -> Rect {
        if (targets_open) {
            const int x = kSettingsFieldX;
            const int y = kTargetsButtonsY;
            if (b == Btn::EnableTarget) return {x, y, 108, 38};
            if (b == Btn::DisableTarget) return {x + 120, y, 108, 38};
            if (b == Btn::ShareTargets) return {x + 240, y, 130, 38};
            if (b == Btn::CopyNames) return {x + 382, y, 108, 38};
            if (b == Btn::CloseTargets) return {x + 502, y, 108, 38};
            return {};
        }
        if (settings_open) {
            if (b == Btn::Save) return {kSettingsFieldX, kSettingsButtonsY, 120, 38};
            if (b == Btn::Cancel) return {kSettingsFieldX + 136, kSettingsButtonsY, 120, 38};
            if (b == Btn::Theme) return {kSettingsFieldX + 280, kSettingsButtonsY, 190, 38};
            return {};
        }
        switch (b) {
        case Btn::Install: return {kInstallX, kInputY, kInstallW, kInputH};
        case Btn::Enable: return {688, 24, 100, 30};
        case Btn::ShareServer: return {560, 24, 116, 30};
        case Btn::Settings: return {800, 24, 96, 30};
        case Btn::SelectAll: return {24, kActionY, 116, kActionH};
        case Btn::Clear: return {148, kActionY, 116, kActionH};
        case Btn::Lock: return {272, kActionY, 108, kActionH};
        case Btn::Copy: return {392, kActionY, 104, kActionH};
        case Btn::Update: return {506, kActionY, 120, kActionH};
        case Btn::UpdateAll: return {636, kActionY, 120, kActionH};
        case Btn::Remove: return {766, kActionY, 130, kActionH};
        case Btn::FilterAll: return {312, kFilterY, 60, kFilterH};
        case Btn::FilterUpdates: return {378, kFilterY, 112, kFilterH};
        case Btn::Check: return {496, kFilterY, 120, kFilterH};
        case Btn::Sort: return {626, kFilterY, 140, kFilterH};
        case Btn::SelfUpdate: return {440, 24, 108, 30};
        default: return {};
        }
    };

    auto button_enabled = [&](Btn b) -> bool {
        if (b == Btn::Nothing) return false;
        if (targets_open) {
            if (b == Btn::CloseTargets) return true;
            if (b == Btn::ShareTargets) return !busy && target_focus >= 0;
            if (b == Btn::CopyNames)
                return !busy && target_focus >= 0 &&
                       !activation[static_cast<std::size_t>(target_focus)].mod_ids.empty();
            if (b == Btn::EnableTarget || b == Btn::DisableTarget)
                return !busy && target_focus >= 0 && !target_mod_ids.empty();
            return false;
        }
        if (settings_open)
            return b == Btn::Save || b == Btn::Cancel || b == Btn::Theme;
        if (busy) return false;
        switch (b) {
        case Btn::Install:
        case Btn::Settings:
        case Btn::ShareServer:
        case Btn::UpdateAll: return true;
        case Btn::SelfUpdate: return app_update_available;
        case Btn::Update:
        case Btn::Remove:
        case Btn::Lock:
        case Btn::Copy: return !selected_ids.empty();
        case Btn::Enable:
        case Btn::Clear: return selected_any();
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
        case Btn::Clear: return "Deselect";
        case Btn::Copy: return "Copy IDs";
        case Btn::Lock: {
            bool all_locked = !selected_ids.empty();
            for (auto id : selected_ids)
                if (!mm.manually_locked(id)) { all_locked = false; break; }
            return all_locked ? "Unlock" : "Lock";
        }
        case Btn::Enable: return "Enable...";
        case Btn::ShareServer: return "Share server";
        case Btn::SelfUpdate: return "Update app";
        case Btn::EnableTarget: return "Enable";
        case Btn::DisableTarget: return "Disable";
        case Btn::CopyNames: return "Copy names";
        case Btn::ShareTargets:
            return target_focus >= 0 &&
                       static_cast<std::size_t>(target_focus) < activation.size() &&
                       activation[static_cast<std::size_t>(target_focus)].kind ==
                           ActivationKind::Server
                ? "Copy server IDs" : "Copy IDs";
        case Btn::CloseTargets: return "Close";
        case Btn::Save: return "Save";
        case Btn::Cancel: return "Cancel";
        case Btn::FilterAll: return "All";
        case Btn::FilterUpdates: return "Updates";
        case Btn::Sort: return sort_mode == 0 ? "Sort: Name" : "Sort: Updated";
        case Btn::Theme: return settings_dark ? "Theme: Dark" : "Theme: Light";
        case Btn::Check: return "Check updates";
        default: return {};
        }
    };

    auto button_at = [&](int x, int y) -> Btn {
        if (targets_open) {
            for (Btn b : {Btn::EnableTarget, Btn::DisableTarget, Btn::ShareTargets,
                          Btn::CopyNames, Btn::CloseTargets}) {
                if (!button_enabled(b)) continue;
                Rect r = button_rect(b);
                if (in_rect(x, y, r.x, r.y, r.w, r.h)) return b;
            }
            return Btn::Nothing;
        }
        if (settings_open) {
            for (Btn b : {Btn::Save, Btn::Cancel, Btn::Theme}) {
                if (!button_enabled(b)) continue;
                Rect r = button_rect(b);
                if (in_rect(x, y, r.x, r.y, r.w, r.h)) return b;
            }
            return Btn::Nothing;
        }
        for (Btn b : {Btn::Install, Btn::ShareServer, Btn::SelfUpdate, Btn::Enable,
                      Btn::Settings, Btn::Update, Btn::Remove, Btn::UpdateAll,
                      Btn::SelectAll, Btn::Clear, Btn::Lock, Btn::Copy, Btn::FilterAll,
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
        const bool primary = b == Btn::Install || b == Btn::Save ||
                             b == Btn::EnableTarget;
        const bool danger = b == Btn::Remove && confirm_remove;
        const bool chip = b == Btn::FilterAll || b == Btn::FilterUpdates;
        const bool active = (b == Btn::FilterAll && !only_updates) ||
                            (b == Btn::FilterUpdates && only_updates);
        const bool hot = enabled && hover_button == static_cast<int>(b);
        unsigned long fill_color = pal.surface;
        unsigned long border_color = pal.border;
        unsigned long text_color = pal.text;
        unsigned long icon_color = pal.brand;
        if (!enabled) {
            fill_color = pal.bg;
            text_color = pal.muted;
            icon_color = pal.muted;
        } else if (danger) {
            fill_color = pal.danger_soft;
            border_color = pal.danger;
            text_color = pal.danger;
            icon_color = pal.danger;
        } else if (chip && active) {
            fill_color = pal.brand_soft;
            border_color = pal.brand;
            text_color = pal.brand;
            icon_color = pal.brand;
        } else if (primary) {
            fill_color = hot ? pal.accent_hover : pal.accent;
            border_color = fill_color;
            text_color = pal.on_accent;
            icon_color = pal.on_accent;
        } else if (b == Btn::SelfUpdate) {
            fill_color = hot ? pal.brand : pal.success;
            border_color = fill_color;
            text_color = dark_mode ? pal.bg : pal.on_accent;
            icon_color = text_color;
        } else if (hot) {
            fill_color = pal.hover;
            border_color = pal.brand_border;
        }
        fill(dpy, win, gc, r.x, r.y, r.w, r.h, fill_color);
        frame(dpy, win, gc, r.x, r.y, r.w, r.h, border_color);
        const std::string label = button_label(b);
        const bool show_icon = b != Btn::Settings && b != Btn::Check &&
                               b != Btn::Theme && b != Btn::Enable &&
                               b != Btn::Lock &&
                               b != Btn::ShareServer && b != Btn::SelfUpdate &&
                               b != Btn::EnableTarget && b != Btn::DisableTarget &&
                               b != Btn::CloseTargets &&
                               b != Btn::ShareTargets && b != Btn::CopyNames;
        const int icon_w = show_icon ? 18 : 0;
        const int gap = show_icon ? 7 : 0;
        const int label_w = text_width(body, label);
        const int start = r.x + (r.w - icon_w - gap - label_w) / 2;
        if (show_icon)
            draw_button_icon(dpy, win, gc, b, start, r.y + r.h / 2, icon_color);
        text(dpy, win, gc, body, start + icon_w + gap, baseline_for(body, r), label,
             text_color);
        XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
    };

    auto clear_icons = [&]() {
        for (auto& [folder, image] : icon_cache) {
            (void)folder;
            if (image.pixmap != None) XFreePixmap(dpy, image.pixmap);
        }
        icon_cache.clear();
        info_cache.clear();
    };

    auto folder_info = [&](const std::string& folder) -> const std::optional<ModInfo>& {
        auto it = info_cache.find(folder);
        if (it == info_cache.end())
            it = info_cache.emplace(folder, mm.info_for(folder)).first;
        return it->second;
    };

    auto get_info = [&](const ModRow& m) -> const std::optional<ModInfo>& {
        return folder_info(m.folders.front());
    };

    auto get_icon = [&](const ModRow& m) -> CachedIcon {
        auto it = icon_cache.find(m.folders.front());
        if (it != icon_cache.end()) return it->second;
        const auto& info = get_info(m);
        const fs::path path = info ? info->icon : fs::path{};
        CachedIcon image;
        if (!path.empty()) {
            ModIcon icon;
            if (load_mod_icon(path, 48, icon))
                image = make_icon_pixmap(dpy, wnd, scr, gc, icon, pal.surface);
        }
        icon_cache[m.folders.front()] = image;
        return image;
    };

    auto clear_target_icons = [&]() {
        for (auto& [path, image] : target_icon_cache) {
            (void)path;
            if (image.pixmap != None) XFreePixmap(dpy, image.pixmap);
        }
        target_icon_cache.clear();
    };

    auto get_target_icon = [&](const ActivationTarget& target) -> CachedIcon {
        if (target.kind != ActivationKind::Save || target.thumbnail.empty()) return {};
        const std::string key = target.thumbnail.string();
        auto it = target_icon_cache.find(key);
        if (it != target_icon_cache.end()) return it->second;
        ModIcon icon;
        CachedIcon image;
        if (load_mod_icon(target.thumbnail, 24, icon))
            image = make_icon_pixmap(dpy, wnd, scr, gc, icon, pal.surface);
        target_icon_cache[key] = image;
        return image;
    };

    auto rebuild_visible = [&]() {
        visible = visible_mods(mm.list_all(), filter_text, only_updates, sort_mode,
            [&](std::uint64_t id) { return mm.update_available(id); });
        for (auto& row : visible) row.locked = mm.locked(row.workshop_id);
        activation = mm.activation_targets();
        row_mod_ids.clear();
        row_enabled.clear();
        row_mod_ids.reserve(visible.size());
        row_enabled.reserve(visible.size());
        for (const auto& m : visible) {
            std::vector<std::string> ids;
            for (const auto& folder : m.folders) {
                const auto& info = folder_info(folder);
                if (!info || info->id.empty()) continue;
                if (std::find(ids.begin(), ids.end(), info->id) == ids.end())
                    ids.push_back(info->id);
            }
            row_enabled.push_back(enabled_in(activation, ids).empty() ? 0 : 1);
            row_mod_ids.push_back(std::move(ids));
        }
        list_offset = std::min(std::max(0, list_offset),
                               std::max(0, static_cast<int>(visible.size()) - kRows));
        pz::flat_set<std::uint64_t> present;
        pz::flat_set<std::string> present_folders;
        for (const auto& m : visible) {
            present.insert(m.workshop_id);
            if (m.workshop_id == 0 && !m.folders.empty())
                present_folders.insert(m.folders.front());
        }
        for (auto it = selected_ids.begin(); it != selected_ids.end();) {
            if (*it == 0 || present.count(*it) == 0) it = selected_ids.erase(it);
            else ++it;
        }
        for (auto it = selected_folders.begin(); it != selected_folders.end();) {
            if (present_folders.count(*it) == 0) it = selected_folders.erase(it);
            else ++it;
        }
        if (!selected_any()) confirm_remove = false;
        list_dirty = false;
    };

    auto filter_clear_rect = []() -> Rect {
        return {kMargin + 280 - 24, kFilterY + 2, 20, kFilterH - 4};
    };

    auto draw_input = [&](const Rect& r, const std::string& value,
                          const std::string& placeholder, bool focused,
                          bool enabled, int right_pad) {
        fill(dpy, win, gc, r.x, r.y, r.w, r.h, pal.field);
        frame(dpy, win, gc, r.x, r.y, r.w, r.h,
              focused && enabled ? pal.accent : pal.border);
        const bool empty = value.empty();
        const int text_x = r.x + 12;
        const int inner_w = r.w - 24 - right_pad;
        const std::string shown = empty ? elide(body, placeholder, inner_w)
                                        : elide_tail(body, value, inner_w);
        text(dpy, win, gc, body, text_x, baseline_for(body, r), shown,
             empty ? pal.muted : pal.text);
        if (focused && enabled && !empty) {
            const int caret_x = text_x + text_width(body, shown) + 1;
            fill(dpy, win, gc, caret_x, r.y + 10, 2, r.h - 20, pal.accent);
        }
    };

    auto clamp_list_offset = [&]() {
        list_offset = std::min(std::max(0, list_offset),
                               std::max(0, static_cast<int>(visible.size()) - kRows));
    };

    auto scroll_metrics = [&](int total, int visible_count, int top, int bottom, int offset,
                              int& thumb_y, int& thumb_h) -> bool {
        if (total <= visible_count) {
            thumb_y = 0;
            thumb_h = 0;
            return false;
        }
        const int track_h = bottom - top;
        thumb_h = std::max(kScrollMinThumb, track_h * visible_count / total);
        const int travel = track_h - thumb_h;
        const int max_offset = total - visible_count;
        const int clamped = std::min(std::max(0, offset), max_offset);
        thumb_y = top + travel * clamped / max_offset;
        return true;
    };

    auto scrollbar_metrics = [&](int& thumb_y, int& thumb_h) -> bool {
        return scroll_metrics(static_cast<int>(visible.size()), kRows, kListTop,
                              kListBottom, list_offset, thumb_y, thumb_h);
    };

    auto activity_metrics = [&](int& thumb_y, int& thumb_h) -> bool {
        return scroll_metrics(static_cast<int>(log.size()), kActivityLines,
                              kActivityTrackTop, kActivityTrackBottom,
                              activity_offset, thumb_y, thumb_h);
    };

    auto activity_bar_at = [&](int x, int y) {
        return x >= kActivityScrollX - kScrollGrab &&
               x <= kActivityScrollX + kScrollW + kScrollGrab &&
               y >= kActivityTrackTop && y < kActivityTrackBottom;
    };

    auto clamp_activity_offset = [&]() {
        const int max_offset = std::max(0, static_cast<int>(log.size()) - kActivityLines);
        activity_offset = std::min(std::max(0, activity_offset), max_offset);
        activity_follow = activity_offset >= max_offset;
    };

    auto scrollbar_at = [&](int x, int y) {
        return x >= kScrollX - kScrollGrab && x <= kScrollX + kScrollW + kScrollGrab &&
               y >= kListTop && y < kListBottom;
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


    auto draw_settings = [&]() {
        fill(dpy, win, gc, 0, 0, kW, kH, pal.bg);
        text(dpy, win, gc, heading, kMargin, 42, "Settings", pal.text);
        text(dpy, win, gc, body, kMargin, 62,
             "Choose the Zomboid folder and configure steamcmd.", pal.muted);
        fill(dpy, win, gc, kSettingsX, kSettingsY, kSettingsW, kSettingsH, pal.surface);
        frame(dpy, win, gc, kSettingsX, kSettingsY, kSettingsW, kSettingsH, pal.border);

        const int x = kSettingsFieldX;
        text(dpy, win, gc, body, x, kSettingsY + 92, "Zomboid folder", pal.text);
        text(dpy, win, gc, body, x + 150, kSettingsY + 92,
             "Absolute path containing mods/ and pzmodmgr/", pal.muted);
        draw_input({x, kSettingsZomboidY, kSettingsFieldW, kSettingsFieldH},
                   settings_zomboid, "Default is ~/Zomboid", settings_field == 0, true, 0);

        text(dpy, win, gc, body, x, kSettingsY + 166, "steamcmd executable", pal.text);
        text(dpy, win, gc, body, x + 210, kSettingsY + 166,
             "Blank uses the built-in copy", pal.muted);
        draw_input({x, kSettingsSteamcmdY, kSettingsFieldW, kSettingsFieldH},
                   settings_path, "Built-in steamcmd", settings_field == 1, true, 0);

        text(dpy, win, gc, body, x, kSettingsY + 240, "Steam username", pal.text);
        draw_input({x, kSettingsUserY, kSettingsFieldW, kSettingsFieldH},
                   settings_user, "anonymous", settings_field == 2, true, 0);
        const bool auto_hot = in_rect(last_mouse_x, last_mouse_y, x, kSettingsY + 292,
                                      kSettingsFieldW, 22);
        const Rect auto_box{x, kSettingsY + 292, 18, 18};
        fill(dpy, win, gc, auto_box.x, auto_box.y, auto_box.w, auto_box.h,
             settings_auto ? pal.accent : pal.field);
        frame(dpy, win, gc, auto_box.x, auto_box.y, auto_box.w, auto_box.h,
              auto_hot ? pal.accent : pal.border_strong);
        if (settings_auto) {
            XSetForeground(dpy, gc, pal.on_accent);
            XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
            XDrawLine(dpy, win, gc, auto_box.x + 4, auto_box.y + 9,
                      auto_box.x + 7, auto_box.y + 12);
            XDrawLine(dpy, win, gc, auto_box.x + 7, auto_box.y + 12,
                      auto_box.x + 14, auto_box.y + 4);
            XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
        }
        text(dpy, win, gc, body, x + 28, kSettingsY + 306,
             "Enable newly installed mods in mods/default.txt", pal.text);
        text(dpy, win, gc, body, x, kSettingsY + 330,
             "No password is stored. Run steamcmd manually once so its login is cached.",
             pal.muted);
        text(dpy, win, gc, body, x, kSettingsY + 348,
             "Use anonymous for public Workshop items.", pal.muted);
        draw_button(Btn::Save);
        draw_button(Btn::Cancel);
        draw_button(Btn::Theme);
    };

    auto draw_targets = [&]() {
        fill(dpy, win, gc, 0, 0, kW, kH, pal.bg);
        text(dpy, win, gc, heading, kMargin, 42, "Enable mods", pal.text);
        text(dpy, win, gc, body, kMargin, 62,
             "Choose a Mods list, save, or server for the selected mods.", pal.muted);
        fill(dpy, win, gc, kSettingsX, kSettingsY, kSettingsW, kSettingsH, pal.surface);
        frame(dpy, win, gc, kSettingsX, kSettingsY, kSettingsW, kSettingsH, pal.border);

        if (activation.empty()) {
            text(dpy, win, gc, body, kSettingsFieldX, kSettingsY + 120,
                 "No Mods list, save, or server was found in the Zomboid folder.",
                 pal.muted);
        } else {
            const int total = static_cast<int>(activation.size());
            const int last = std::min(total, target_offset + kTargetsRows);
            const std::string range = total <= kTargetsRows ? std::to_string(total) +
                (total == 1 ? " target" : " targets")
                : std::to_string(target_offset + 1) + "-" + std::to_string(last) +
                  " of " + std::to_string(total);
            text(dpy, win, gc, body, kSettingsX + kSettingsW - 24 -
                     text_width(body, range), kSettingsY + 86, range, pal.muted);

            for (int slot = 0; slot < kTargetsRows; ++slot) {
                const int index = target_offset + slot;
                if (index >= total) break;
                const auto& target = activation[static_cast<std::size_t>(index)];
                const Rect row{kSettingsFieldX, kTargetsRowY + slot * kTargetsRowH,
                               kSettingsFieldW, kTargetsRowH - 6};
                const bool focused = index == target_focus;
                fill(dpy, win, gc, row.x, row.y, row.w, row.h,
                     focused ? pal.accent_soft : pal.bg);
                frame(dpy, win, gc, row.x, row.y, row.w, row.h,
                       focused ? pal.accent : pal.border);
                const int count = activation_count(target, target_mod_ids);
                std::string state = target_mod_ids.empty() ? ""
                    : count == 0 ? "Off"
                    : count == static_cast<int>(target_mod_ids.size()) ? "On"
                    : std::to_string(count) + " of " +
                      std::to_string(target_mod_ids.size()) + " on";
                const int state_w = text_width(body, state);
                const CachedIcon image = get_target_icon(target);
                if (image.pixmap != None) {
                    XCopyArea(dpy, image.pixmap, win, gc, 0, 0, image.width,
                              image.height, row.x + 8, row.y + (row.h - 24) / 2);
                }
                const int text_x = row.x + (image.pixmap != None ? 42 : 12);
                const int label_w = row.w - state_w - (image.pixmap != None ? 54 : 36);
                text(dpy, win, gc, body, text_x, row.y + row.h / 2 + 5,
                     elide(body, target.label, label_w), pal.text);
                text(dpy, win, gc, body, row.x + row.w - 12 - state_w,
                     row.y + row.h / 2 + 5, state,
                     count == 0 ? pal.muted : pal.success);
            }
        }

        text(dpy, win, gc, body, kSettingsFieldX, kSettingsY + 326,
             "Enable/disable writes only the selected mod IDs to this target.",
             pal.muted);
        draw_button(Btn::EnableTarget);
        draw_button(Btn::DisableTarget);
        draw_button(Btn::ShareTargets);
        draw_button(Btn::CopyNames);
        draw_button(Btn::CloseTargets);
    };

    auto redraw = [&]() {
        frame_pending = false;
        fill(dpy, win, gc, 0, 0, kW, kH, pal.bg);
        if (targets_open) {
            draw_targets();
        } else if (settings_open) {
            draw_settings();
        } else {

        const Rect mark{kMargin, 20, 36, 36};
        fill(dpy, win, gc, mark.x, mark.y, mark.w, mark.h, pal.brand);
        const std::string monogram = "PZ";
        text(dpy, win, gc, heading,
             mark.x + (mark.w - text_width(heading, monogram)) / 2,
             mark.y + (mark.h + heading->ascent - heading->descent) / 2,
             monogram, pal.cream);
        fill(dpy, win, gc, kMargin + 8, 49, 20, 3, pal.accent);
        text(dpy, win, gc, heading, kMargin + 48, 42, "PZ Mod Manager", pal.text);
        text(dpy, win, gc, body, kMargin + 48, 62,
             "Project Zomboid Workshop installer", pal.muted);
        draw_button(Btn::ShareServer);
        draw_button(Btn::SelfUpdate);
        draw_button(Btn::Enable);
        draw_button(Btn::Settings);

        draw_input({kInputX, kInputY, kInputW, kInputH}, input,
                   "Paste Workshop URLs or IDs", !filter_focus && !busy, !busy, 0);
        draw_button(Btn::Install);
        text(dpy, win, gc, body, kMargin, 142,
             "Press Enter to install. Separate several links with spaces; collections are supported.", pal.muted);

        if (list_dirty && !busy) rebuild_visible();

        draw_input({kMargin, kFilterY, 280, kFilterH}, filter_text,
                   "Filter name or folder", filter_focus && !busy, !busy,
                   filter_text.empty() ? 0 : 26);
        if (!filter_text.empty()) {
            const Rect clear = filter_clear_rect();
            const bool hot = !busy && in_rect(last_mouse_x, last_mouse_y, clear.x,
                                              clear.y, clear.w, clear.h);
            text(dpy, win, gc, body, clear.x + (clear.w - text_width(body, "X")) / 2,
                 baseline_for(body, clear), "X", hot ? pal.accent : pal.muted);
        }
        draw_button(Btn::FilterAll);
        draw_button(Btn::FilterUpdates);
        draw_button(Btn::Sort);
        draw_button(Btn::Check);
        const int total = static_cast<int>(visible.size());
        std::string count;
        if (busy) {
            count = "Working...";
        } else if (total == 0) {
            count = "0 mods";
        } else if (total <= kRows) {
            count = std::to_string(total) + (total == 1 ? " mod" : " mods");
        } else {
            const int last = std::min(total, list_offset + kRows);
            count = std::to_string(list_offset + 1) + "-" + std::to_string(last) +
                    " of " + std::to_string(total);
        }
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
                const bool selected = row_selected(m);
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

                const CachedIcon image = get_icon(m);
                const int icon_x = card.x + 10, icon_y = card.y + 8;
                if (image.pixmap != None) {
                    XCopyArea(dpy, image.pixmap, win, gc, 0, 0, image.width,
                              image.height, icon_x, icon_y);
                } else {
                    fill(dpy, win, gc, icon_x, icon_y, 48, 48, pal.accent_soft);
                    const std::string initial = m.name.empty() ? "?" : m.name.substr(0, 1);
                    text(dpy, win, gc, heading,
                         icon_x + (48 - text_width(heading, initial)) / 2,
                         icon_y + (48 + heading->ascent - heading->descent) / 2,
                         initial, pal.accent);
                }

                const bool has_update = mm.update_available(m.workshop_id);
                const bool switchable = !activation.empty();
                const bool is_enabled = index < static_cast<int>(row_enabled.size()) &&
                                        row_enabled[static_cast<std::size_t>(index)] != 0;
                const auto& mod_ids = row_mod_ids[static_cast<std::size_t>(index)];
                const std::string on_targets = enabled_in(activation, mod_ids);
                constexpr int pill_w = 64, pill_gap = 6, pill_margin = 12;
                const int on_x = card.x + card.w - pill_margin - pill_w;
                const int update_x = switchable ? on_x - pill_gap - pill_w : on_x;
                const int text_x = card.x + 72;
                const int text_right = (has_update ? update_x
                                                  : switchable ? on_x
                                                               : card.x + card.w - pill_margin) - 8;
                const int text_w = text_right - text_x;
                text(dpy, win, gc, heading, text_x, card.y + 26,
                     elide(heading, m.name.empty() ? m.folders.front() : m.name, text_w),
                     pal.text);
                const auto& info = get_info(m);
                const std::string meta = mod_meta(
                    m, info ? &info.value() : nullptr, mm.game_version(), on_targets);
                text(dpy, win, gc, body, text_x, card.y + 46,
                     elide(body, meta, text_w), pal.muted);
                if (switchable) {
                    const Rect pill{on_x, card.y + 22, pill_w, 20};
                    fill(dpy, win, gc, pill.x, pill.y, pill.w, pill.h,
                         is_enabled ? pal.success : pal.bg);
                    frame(dpy, win, gc, pill.x, pill.y, pill.w, pill.h,
                           is_enabled ? pal.success : pal.border_strong);
                    const std::string label = is_enabled ? "ON" : "OFF";
                    text(dpy, win, gc, body,
                         pill.x + (pill.w - text_width(body, label)) / 2,
                         pill.y + 14, label,
                         is_enabled ? pal.on_accent : pal.muted);
                }
                if (has_update) {
                    const Rect pill{update_x, card.y + 22, pill_w, 20};
                    fill(dpy, win, gc, pill.x, pill.y, pill.w, pill.h, pal.accent);
                    text(dpy, win, gc, body,
                         pill.x + (pill.w - text_width(body, "UPDATE")) / 2,
                         pill.y + 14, "UPDATE", pal.on_accent);
                }
            }

            int thumb_y = 0, thumb_h = 0;
            if (scrollbar_metrics(thumb_y, thumb_h)) {
                fill(dpy, win, gc, kScrollX, kListTop, kScrollW,
                     kListBottom - kListTop, pal.border);
                fill(dpy, win, gc, kScrollX, thumb_y, kScrollW, thumb_h,
                     scroll_drag ? pal.accent : pal.border_strong);
            }
        }

        draw_button(Btn::SelectAll);
        draw_button(Btn::Clear);
        draw_button(Btn::Lock);
        draw_button(Btn::Copy);
        draw_button(Btn::Update);
        draw_button(Btn::UpdateAll);
        draw_button(Btn::Remove);
        const std::size_t selected_count = selected_ids.size() + selected_folders.size();
        const std::string selected = selected_count == 0
            ? "No selection"
            : std::to_string(selected_count) + " selected";
        text(dpy, win, gc, body, 276, kActionY + 21, selected, pal.muted);

        fill(dpy, win, gc, kMargin, kActivityY, kCardW, kActivityH, pal.surface);
        frame(dpy, win, gc, kMargin, kActivityY, kCardW, kActivityH, pal.border);
        text(dpy, win, gc, body, kMargin + 12, kActivityY + 20, "ACTIVITY", pal.brand);
        const int total_log = static_cast<int>(log.size());
        const int log_max_offset = std::max(0, total_log - kActivityLines);
        if (activity_follow) activity_offset = log_max_offset;
        activity_offset = std::min(std::max(0, activity_offset), log_max_offset);
        int ly = kActivityY + 40;
        for (int i = activity_offset;
             i < total_log && i < activity_offset + kActivityLines; ++i, ly += 16)
            text(dpy, win, gc, body, kMargin + 12, ly,
                 elide(body, log[static_cast<std::size_t>(i)], kCardW - 48), pal.muted);
        if (log.empty())
            text(dpy, win, gc, body, kMargin + 12, kActivityY + 40,
                 "Ready for a Workshop link.", pal.muted);

        int activity_thumb_y = 0, activity_thumb_h = 0;
        if (activity_metrics(activity_thumb_y, activity_thumb_h)) {
            fill(dpy, win, gc, kActivityScrollX, kActivityTrackTop, kScrollW,
                 kActivityTrackBottom - kActivityTrackTop, pal.border);
            fill(dpy, win, gc, kActivityScrollX, activity_thumb_y, kScrollW, activity_thumb_h,
                 activity_drag ? pal.accent : pal.border_strong);
        }

        const std::string status_text = busy
            ? "Working - steamcmd may take a while..."
            : (dependency_warning.empty() ? "Ready" : dependency_warning);
        text(dpy, win, gc, body, kMargin, kStatusY,
             elide(body, status_text, kCardW - 110),
             busy ? pal.accent : (dependency_warning.empty() ? pal.brand : pal.danger));
        const std::string mod_count = std::to_string(visible.size()) +
                                      (visible.size() == 1 ? " mod" : " mods");
        text(dpy, win, gc, body, kMargin + kCardW - text_width(body, mod_count), kStatusY,
             mod_count, pal.muted);
        }

        XCopyArea(dpy, win, wnd, gc, 0, 0, kW, kH, 0, 0);
        // Xlib buffers requests; flush before blocking in poll.
        XFlush(dpy);
    };

    auto close_watch = [&]() {
        if (watch_fd >= 0) {
            ::close(watch_fd);
            watch_fd = -1;
        }
    };

    auto refresh_watch = [&]() {
        close_watch();
#if defined(__linux__)
        watch_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (watch_fd < 0) return;
        const std::string dir = mm.config().data_dir.string();
        if (::inotify_add_watch(watch_fd, dir.c_str(),
                                IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
                                    IN_DELETE) < 0) {
            close_watch();
        }
#endif
    };

    auto handle_watch = [&]() {
        bool changed = false;
#if defined(__linux__)
        if (watch_fd < 0) return;
        alignas(inotify_event) char buf[4096];
        for (;;) {
            const ssize_t n = ::read(watch_fd, buf, sizeof buf);
            if (n <= 0) break;
            for (const char* p = buf; p < buf + n;) {
                const auto* event = reinterpret_cast<const inotify_event*>(p);
                if ((event->mask & IN_Q_OVERFLOW) != 0) changed = true;
                if (event->len != 0 &&
                    std::strcmp(event->name, "installed.tsv") == 0) {
                    changed = true;
                }
                p += sizeof(inotify_event) + event->len;
            }
        }
#endif
        if (!changed) return;
        if (busy) {
            reload_pending = true;
            return;
        }
        if (mm.reload_installed()) {
            clear_icons();
            list_dirty = true;
            refresh_dependency_warning();
            redraw();
        }
    };

    refresh_watch();
    // Close the gap between the initial load and the watch starting.
    mm.reload_installed();

    auto start_command = [&](const std::string& line) {
        if (busy || line.empty()) return;
        confirm_remove = false;
        push("> " + line);
        busy = true;
        if (worker.joinable()) worker.join();
        worker = thread([&, line]() {
            auto r = run_command(mm, line, enqueue);
            {
                pz::lock_guard lock(pending_mu);
                pending.push_back(std::string(r.ok ? "OK: " : "ERROR: ") + r.message);
                worker_done = true;
            }
            wake_ui();
        });
        if (!worker.joinable()) {
            busy = false;
            push("ERROR: cannot start worker");
        }
        redraw();
    };

    auto submit = [&]() {
        if (busy) return;
        if (input.empty()) {
            push("Enter one or more Workshop URLs or IDs first.");
            redraw();
            return;
        }
        const bool lock_command = input.compare(0, 5, "lock ") == 0 ||
                                  input.compare(0, 7, "unlock ") == 0;
        std::string line = lock_command ? input : "add " + input;
        input.clear();
        start_command(line);
    };

    auto copy_ids = [&]() {
        if (selected_ids.empty()) {
            push(selected_folders.empty()
                ? "Select one or more tracked mods first."
                : "Manual mods have no Workshop ID to copy.");
            redraw();
            return;
        }
        const std::vector<std::uint64_t> ids(selected_ids.begin(), selected_ids.end());
        clipboard_text = join_workshop_ids(ids);
        XSetSelectionOwner(dpy, clipboard, wnd, CurrentTime);
        if (XGetSelectionOwner(dpy, clipboard) != wnd) {
            clipboard_text.clear();
            push("Could not take clipboard ownership.");
        } else {
            push("Copied " + std::to_string(ids.size()) +
                 " Workshop ID(s). Paste them into Add, or type lock first.");
        }
        redraw();
    };

    auto clamp_target_offset = [&]() {
        target_offset = std::min(std::max(0, target_offset),
            std::max(0, static_cast<int>(activation.size()) - kTargetsRows));
    };

    auto target_row_at = [&](int x, int y) -> int {
        if (x < kSettingsX || x > kSettingsX + kSettingsW) return -1;
        if (y < kTargetsRowY) return -1;
        const int slot = (y - kTargetsRowY) / kTargetsRowH;
        if (slot < 0 || slot >= kTargetsRows) return -1;
        const int index = target_offset + slot;
        return index < static_cast<int>(activation.size()) ? index : -1;
    };

    auto open_targets = [&](bool prefer_server = false, bool allow_empty = false) {
        if (!allow_empty && !selected_any()) {
            push("Select one or more mods first.");
            redraw();
            return;
        }
        if (list_dirty) rebuild_visible();
        target_mod_ids.clear();
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (!row_selected(visible[i])) continue;
            if (i >= row_mod_ids.size()) continue;
            for (const auto& id : row_mod_ids[i]) {
                if (std::find(target_mod_ids.begin(), target_mod_ids.end(), id) ==
                    target_mod_ids.end())
                    target_mod_ids.push_back(id);
            }
        }
        activation = mm.activation_targets();
        clear_target_icons();
        target_offset = 0;
        target_focus = activation.empty() ? -1 : 0;
        if (prefer_server) {
            for (std::size_t i = 0; i < activation.size(); ++i)
                if (activation[i].kind == ActivationKind::Server) {
                    target_focus = static_cast<int>(i);
                    break;
                }
            target_offset = std::max(0, target_focus - kTargetsRows + 1);
            clamp_target_offset();
        }
        targets_open = true;
        redraw();
    };

    auto selection_command = [&](int index, bool enabled) {
        std::string line = "set-enabled " + std::to_string(index) +
                           (enabled ? " on" : " off");
        for (auto id : selected_ids) line += " w:" + std::to_string(id);
        for (const auto& folder : selected_folders)
            line += " " + folder_token(folder);
        return line;
    };

    auto copy_target_ids = [&]() {
        if (target_focus < 0 || target_focus >= static_cast<int>(activation.size())) return;
        const auto& target = activation[static_cast<std::size_t>(target_focus)];
        const auto ids = mm.workshop_ids_for(target);
        if (ids.empty()) {
            push("No Workshop IDs found for " + activation_name(target) +
                 ". Use Copy names so the list can still be shared.");
            redraw();
            return;
        }
        clipboard_text = join_workshop_ids(ids);
        XSetSelectionOwner(dpy, clipboard, wnd, CurrentTime);
        if (XGetSelectionOwner(dpy, clipboard) != wnd) {
            clipboard_text.clear();
            push("Could not take clipboard ownership.");
        } else {
            const auto missing = mm.unmapped_mod_ids(target);
            std::string message = "Copied " + std::to_string(ids.size()) +
                                  " Workshop ID(s) from " + activation_name(target) +
                                  ". Paste into Add, or type lock first to pin them.";
            if (!missing.empty())
                message += " " + std::to_string(missing.size()) +
                           " enabled Mod ID(s) have no installed Workshop ID; use Copy names.";
            push(message);
        }
        redraw();
    };

    auto copy_target_names = [&]() {
        if (target_focus < 0 || target_focus >= static_cast<int>(activation.size())) return;
        const auto& target = activation[static_cast<std::size_t>(target_focus)];
        const auto names = mm.enabled_mod_names(target);
        if (names.empty()) {
            push("No enabled Mod names found for " + activation_name(target) + ".");
            redraw();
            return;
        }
        clipboard_text.clear();
        for (const auto& name : names) {
            if (!clipboard_text.empty()) clipboard_text += '\n';
            clipboard_text += name;
        }
        XSetSelectionOwner(dpy, clipboard, wnd, CurrentTime);
        if (XGetSelectionOwner(dpy, clipboard) != wnd) {
            clipboard_text.clear();
            push("Could not take clipboard ownership.");
        } else {
            push("Copied " + std::to_string(names.size()) + " enabled Mod name(s) from " +
                 activation_name(target) + ".");
        }
        redraw();
    };

    auto remove_selected = [&]() {
        if (busy) return;
        if (selected_ids.empty()) {
            push("Select a tracked mod first; manual folders are not managed by pzmodmgr.");
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
        settings_zomboid = mm.config().zomboid_dir.string();
        settings_path = mm.config().steamcmd_exe.string();
        settings_user = mm.config().steam_user;
        settings_dark = mm.config().dark_mode;
        settings_auto = mm.config().auto_enable;
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
        auto r3 = run_command(mm, "config zomboid_dir " + settings_zomboid, push);
        auto r4 = run_command(mm, std::string("config dark_mode ") +
                                   (settings_dark ? "on" : "off"), push);
        auto r5 = run_command(mm, std::string("config auto_enable ") +
                                   (settings_auto ? "on" : "off"), push);
        if (r1.ok && r2.ok && r3.ok && r4.ok && r5.ok) {
            dark_mode = settings_dark;
            pal = dark_mode ? dark_pal : light_pal;
            XSetWindowBackground(dpy, wnd, pal.bg);
            XSetWindowBorder(dpy, wnd, pal.border);
            refresh_watch();
            mm.reload_installed();
            clear_icons();
            list_dirty = true;
            refresh_dependency_warning();
            settings_open = false;
            push("Settings saved");
        } else {
            const auto& failed = !r1.ok ? r1
                : (!r2.ok ? r2 : (!r3.ok ? r3 : (!r4.ok ? r4 : r5)));
            push("ERROR: " + failed.message);
        }
        redraw();
    };

    auto paste_into_field = [&](const std::string& raw) {
        if (!settings_open && paste_field != PasteField::Filter) {
            const std::string ids = clean_workshop_paste(raw);
            if (ids.empty()) {
                push("Clipboard is empty.");
            } else {
                if (!input.empty() && input.back() != ' ') input += ' ';
                input += ids;
            }
            redraw();
            return;
        }
        const std::string value = clean_pasted_text(raw);
        if (value.empty()) {
            push("Clipboard is empty.");
            redraw();
            return;
        }
        if (settings_open) {
            if (paste_field == PasteField::SettingsZomboid) settings_zomboid += value;
            else if (paste_field == PasteField::SettingsPath) settings_path += value;
            else settings_user += value;
        } else {
            filter_text += value;
            list_offset = 0;
            list_dirty = true;
        }
        redraw();
    };

    auto request_paste = [&](Atom selection, bool fallback, Time time) {
        if (busy) return;
        paste_field = settings_open
            ? (settings_field == 0 ? PasteField::SettingsZomboid
               : settings_field == 1 ? PasteField::SettingsPath
                                     : PasteField::SettingsUser)
            : (filter_focus ? PasteField::Filter : PasteField::Input);
        paste_selection = selection;
        paste_time = time;
        paste_fallback = fallback;
        paste_pending = true;
        XConvertSelection(dpy, selection, fallback ? XA_STRING : utf8, paste_prop, wnd,
                          time);
        XFlush(dpy);
    };

    const bool auto_check_due = mm.update_check_due();
    bool auto_check_started = false;
    push("Paste Workshop URLs or IDs, then press Enter.");
    push("Click mods to toggle their selection; Shift-click selects a range.");
    refresh_dependency_warning();
    redraw();

    bool running = true;
    auto handle_events = [&]() {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                redraw();
                // Start the once-a-day check after the window has painted, so
                // startup never waits on curl or the Steam API.
                if (!auto_check_started) {
                    auto_check_started = true;
                    if (auto_check_due) start_command("check");
                }
                break;
            case MotionNotify: {
                last_mouse_x = ev.xmotion.x;
                last_mouse_y = ev.xmotion.y;
                if (scroll_drag) {
                    int thumb_y = 0, thumb_h = 0;
                    if (scrollbar_metrics(thumb_y, thumb_h)) {
                        const int travel = (kListBottom - kListTop) - thumb_h;
                        const int max_offset = static_cast<int>(visible.size()) - kRows;
                        if (travel > 0 && max_offset > 0) {
                            const int delta = ev.xmotion.y - scroll_drag_y;
                            list_offset = scroll_drag_offset + delta * max_offset / travel;
                            clamp_list_offset();
                        }
                    }
                    frame_pending = true;
                    break;
                }
                if (activity_drag) {
                    int thumb_y = 0, thumb_h = 0;
                    if (activity_metrics(thumb_y, thumb_h)) {
                        const int travel = (kActivityTrackBottom - kActivityTrackTop) - thumb_h;
                        const int max_offset = static_cast<int>(log.size()) - kActivityLines;
                        if (travel > 0 && max_offset > 0) {
                            const int delta = ev.xmotion.y - activity_drag_y;
                            activity_offset = activity_drag_offset + delta * max_offset / travel;
                            clamp_activity_offset();
                        }
                    }
                    frame_pending = true;
                    break;
                }
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
            case ButtonRelease:
                if (scroll_drag || activity_drag) {
                    scroll_drag = false;
                    activity_drag = false;
                    redraw();
                }
                break;
            case ButtonPress: {
                if (busy) break;
                const int x = ev.xbutton.x, y = ev.xbutton.y;
                if (targets_open) {
                    if (ev.xbutton.button == Button4 || ev.xbutton.button == Button5) {
                        target_offset += ev.xbutton.button == Button4 ? -1 : 1;
                        clamp_target_offset();
                        redraw();
                        break;
                    }
                    if (ev.xbutton.button != Button1) break;
                    const Btn b = button_at(x, y);
                    if (b == Btn::CloseTargets) {
                        targets_open = false;
                        redraw();
                        break;
                    }
                    if (b == Btn::ShareTargets) {
                        copy_target_ids();
                        break;
                    }
                    if (b == Btn::CopyNames) {
                        copy_target_names();
                        break;
                    }
                    if (b == Btn::EnableTarget || b == Btn::DisableTarget) {
                        if (target_focus < 0 || target_mod_ids.empty()) break;
                        const std::string line = selection_command(
                            target_focus, b == Btn::EnableTarget);
                        targets_open = false;
                        start_command(line);
                        break;
                    }
                    const int row = target_row_at(x, y);
                    if (row >= 0) {
                        target_focus = row;
                        redraw();
                    }
                    break;
                }
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
                    if (b == Btn::Theme) {
                        settings_dark = !settings_dark;
                        redraw();
                        break;
                    }
                    if (in_rect(x, y, kSettingsFieldX, kSettingsY + 292,
                                kSettingsFieldW, 22)) {
                        settings_auto = !settings_auto;
                        redraw();
                        break;
                    }
                    int field = -1;
                    if (in_rect(x, y, kSettingsFieldX, kSettingsZomboidY,
                                kSettingsFieldW, kSettingsFieldH))
                        field = 0;
                    else if (in_rect(x, y, kSettingsFieldX, kSettingsSteamcmdY,
                                     kSettingsFieldW, kSettingsFieldH))
                        field = 1;
                    else if (in_rect(x, y, kSettingsFieldX, kSettingsUserY,
                                     kSettingsFieldW, kSettingsFieldH))
                        field = 2;
                    if (field >= 0) settings_field = field;
                    if (ev.xbutton.button == Button2 && field >= 0) {
                        request_paste(XA_PRIMARY, false, ev.xbutton.time);
                        redraw();
                        break;
                    }
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
                if (ev.xbutton.button == Button1 && activity_bar_at(x, y)) {
                    int thumb_y = 0, thumb_h = 0;
                    if (activity_metrics(thumb_y, thumb_h)) {
                        if (y >= thumb_y - kScrollGrab &&
                            y < thumb_y + thumb_h + kScrollGrab) {
                            activity_drag = true;
                            activity_drag_y = y;
                            activity_drag_offset = activity_offset;
                        } else {
                            activity_offset += y < thumb_y ? -kActivityLines : kActivityLines;
                            clamp_activity_offset();
                        }
                        redraw();
                    }
                    break;
                }
                if (ev.xbutton.button == Button1 && scrollbar_at(x, y)) {
                    int thumb_y = 0, thumb_h = 0;
                    if (scrollbar_metrics(thumb_y, thumb_h)) {
                        if (y >= thumb_y - kScrollGrab &&
                            y < thumb_y + thumb_h + kScrollGrab) {
                            scroll_drag = true;
                            scroll_drag_y = y;
                            scroll_drag_offset = list_offset;
                        } else {
                            list_offset += y < thumb_y ? -kRows : kRows;
                            clamp_list_offset();
                        }
                        redraw();
                    }
                    break;
                }
                if (ev.xbutton.button == Button4) {
                    if (activity_bar_at(x, y) ||
                        in_rect(x, y, kMargin, kActivityY, kCardW, kActivityH)) {
                        activity_offset -= 1;
                        clamp_activity_offset();
                        frame_pending = true;
                        break;
                    }
                    if (row_at(x, y) >= 0 || scrollbar_at(x, y) ||
                        in_rect(x, y, kMargin, kListTop, kCardW,
                                kListBottom - kListTop)) {
                        list_offset -= 1;
                        clamp_list_offset();
                    }
                    frame_pending = true;
                    break;
                }
                if (ev.xbutton.button == Button5) {
                    if (activity_bar_at(x, y) ||
                        in_rect(x, y, kMargin, kActivityY, kCardW, kActivityH)) {
                        activity_offset += 1;
                        clamp_activity_offset();
                        frame_pending = true;
                        break;
                    }
                    if (row_at(x, y) >= 0 || scrollbar_at(x, y) ||
                        in_rect(x, y, kMargin, kListTop, kCardW,
                                kListBottom - kListTop)) {
                        list_offset += 1;
                        clamp_list_offset();
                    }
                    frame_pending = true;
                    break;
                }

                if (in_rect(x, y, kInputX, kInputY, kInputW, kInputH)) {
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (!filter_text.empty()) {
                    const Rect clear = filter_clear_rect();
                    if (in_rect(x, y, clear.x, clear.y, clear.w, clear.h)) {
                        filter_text.clear();
                        list_offset = 0;
                        list_dirty = true;
                        filter_focus = true;
                        redraw();
                        break;
                    }
                }
                if (in_rect(x, y, kMargin, kFilterY, 280, kFilterH)) {
                    filter_focus = true;
                    redraw();
                    break;
                }

                const Btn b = button_at(x, y);
                if (b == Btn::FilterAll) {
                    only_updates = false;
                    list_offset = 0;
                    list_dirty = true;
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (b == Btn::FilterUpdates) {
                    only_updates = true;
                    list_offset = 0;
                    list_dirty = true;
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (b == Btn::Sort) {
                    sort_mode = 1 - sort_mode;
                    list_offset = 0;
                    list_dirty = true;
                    filter_focus = false;
                    redraw();
                    break;
                }
                if (b == Btn::Check) {
                    start_command("check");
                    break;
                }
                if (b == Btn::SelfUpdate) {
                    start_command("self-update");
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
                if (b == Btn::Enable) {
                    open_targets(false);
                    break;
                }
                if (b == Btn::ShareServer) {
                    open_targets(true, true);
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
                    for (const auto& m : visible) select_row(m);
                    confirm_remove = false;
                    redraw();
                    break;
                }
                if (b == Btn::Clear) {
                    clear_all();
                    confirm_remove = false;
                    redraw();
                    break;
                }
                if (b == Btn::Lock) {
                    bool all_locked = !selected_ids.empty();
                    for (auto id : selected_ids)
                        if (!mm.manually_locked(id)) { all_locked = false; break; }
                    start_command(build_command(all_locked ? "unlock" : "lock", selected_ids));
                    break;
                }
                if (b == Btn::Copy) {
                    copy_ids();
                    break;
                }

                const int index = row_at(x, y);
                if (index >= 0) {
                    const auto& row = visible[static_cast<std::size_t>(index)];
                    const auto id = row.workshop_id;
                    const std::uint64_t now = monotonic_millis();
                    const bool double_click =
                        last_click_index == index && last_click_time != 0 &&
                        now - last_click_time < 450;
                    if (double_click && id != 0) {
                        clear_all();
                        select_row(row);
                        last_click_index = -1;
                        start_command("update " + std::to_string(id));
                        break;
                    }

                    if (ev.xbutton.state & ShiftMask) {
                        if (anchor_index >= 0) {
                            clear_all();
                            const int lo = std::min(anchor_index, index);
                            const int hi = std::max(anchor_index, index);
                            for (int i = lo; i <= hi && i < static_cast<int>(visible.size());
                                 ++i)
                                select_row(visible[static_cast<std::size_t>(i)]);
                        } else {
                            select_row(row);
                            anchor_index = index;
                        }
                    } else {
                        // A plain row click toggles that row without replacing
                        // the rest of the selection.
                        if (row_selected(row)) unselect_row(row);
                        else select_row(row);
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
            case SelectionClear:
                if (ev.xselectionclear.selection == clipboard) clipboard_text.clear();
                break;
            case SelectionRequest: {
                const XSelectionRequestEvent& request = ev.xselectionrequest;
                XSelectionEvent reply{};
                reply.type = SelectionNotify;
                reply.display = dpy;
                reply.requestor = request.requestor;
                reply.selection = request.selection;
                reply.target = request.target;
                reply.property = None;
                reply.time = request.time;
                if (request.target == XA_STRING || request.target == utf8) {
                    const Atom property = request.property == None ? request.target
                                                                   : request.property;
                    XChangeProperty(dpy, request.requestor, property, request.target, 8,
                                    PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(clipboard_text.data()),
                                    static_cast<int>(clipboard_text.size()));
                    reply.property = property;
                }
                XSendEvent(dpy, request.requestor, False, 0,
                           reinterpret_cast<XEvent*>(&reply));
                XFlush(dpy);
                break;
            }
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
                int rc = XGetWindowProperty(dpy, wnd, ev.xselection.property, 0, 65536,
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

                if (targets_open) {
                    if (ks == XK_Escape) {
                        targets_open = false;
                        redraw();
                    } else if (ks == XK_Return || ks == XK_KP_Enter) {
                        if (target_focus >= 0 && !target_mod_ids.empty()) {
                            const std::string line = selection_command(target_focus, true);
                            targets_open = false;
                            start_command(line);
                        }
                    }
                    break;
                }

                if (settings_open) {
                    if (ks == XK_Return || ks == XK_KP_Enter) {
                        save_settings();
                    } else if (ks == XK_Escape) {
                        settings_open = false;
                        redraw();
                    } else if (ks == XK_Tab) {
                        settings_field = (settings_field + 1) % 3;
                        redraw();
                    } else if (ks == XK_BackSpace) {
                        std::string& field = settings_field == 0 ? settings_zomboid
                            : settings_field == 1 ? settings_path : settings_user;
                        if (!field.empty()) field.pop_back();
                        redraw();
                    } else if (n > 0 && static_cast<unsigned char>(buf[0]) >= 32) {
                        std::string& field = settings_field == 0 ? settings_zomboid
                            : settings_field == 1 ? settings_path : settings_user;
                        field.append(buf, static_cast<std::size_t>(n));
                        redraw();
                    }
                    break;
                }

                if (!filter_focus && (ks == XK_Page_Up || ks == XK_Page_Down)) {
                    list_offset += ks == XK_Page_Up ? -kRows : kRows;
                    clamp_list_offset();
                    redraw();
                    break;
                }

                if (ks == XK_Return || ks == XK_KP_Enter) {
                    if (!filter_focus) submit();
                } else if (ks == XK_BackSpace) {
                    if (filter_focus) {
                        if (!filter_text.empty()) {
                            filter_text.pop_back();
                            list_offset = 0;
                            list_dirty = true;
                        }
                    } else if (!input.empty()) {
                        input.pop_back();
                    }
                    redraw();
                } else if (ks == XK_Escape) {
                    if (!filter_text.empty()) {
                        filter_text.clear();
                        list_offset = 0;
                        list_dirty = true;
                    } else if (only_updates) {
                        only_updates = false;
                        list_offset = 0;
                        list_dirty = true;
                    } else if (selected_any()) {
                        clear_all();
                        confirm_remove = false;
                    } else if (!input.empty()) {
                        input.clear();
                    }
                    redraw();
                } else if (n > 0 && static_cast<unsigned char>(buf[0]) >= 32) {
                    if (filter_focus) {
                        filter_text.append(buf, static_cast<std::size_t>(n));
                        list_offset = 0;
                        list_dirty = true;
                    } else {
                        input.append(buf, static_cast<std::size_t>(n));
                    }
                    redraw();
                }
                break;
            }
            default:
                break;
            }
        }
        // A wheel or drag can queue many events in one poll. Paint once with
        // the final offset instead of redrawing the whole window per event.
        if (frame_pending) redraw();
    };

    while (running) {
        pollfd fds[3] = {{ConnectionNumber(dpy), POLLIN, 0},
                         {wake[0], POLLIN, 0},
                         {watch_fd, POLLIN, 0}};
        const nfds_t nfds = watch_fd >= 0 ? 3 : 2;
        int rc = ::poll(fds, nfds, -1);
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
                app_update_available = mm.app_update_available();
                if (reload_pending) {
                    reload_pending = false;
                    mm.reload_installed();
                }
                clear_icons();
                list_dirty = true;
                hover_row = -1;
                refresh_dependency_warning();
            }
            redraw();
        }
        if (watch_fd >= 0 && (fds[2].revents & POLLIN)) handle_watch();
        if (fds[0].revents & POLLIN) handle_events();
    }

    if (worker.joinable()) worker.join();
    close_watch();
    clear_icons();
    clear_target_icons();
    ::close(wake[0]);
    ::close(wake[1]);
    if (heading != body) XFreeFont(dpy, heading);
    XFreeFont(dpy, body);
    XFreePixmap(dpy, backbuf);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, wnd);
    XCloseDisplay(dpy);
    return 0;
}
}  // namespace pz
