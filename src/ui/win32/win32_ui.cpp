#include "ui/ui.hpp"
#include "ui/mod_icon.hpp"
#include "ui/glyphs.hpp"
#include "core/commands.hpp"
// Keep std::min/std::max usable; windows.h would otherwise define macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <exception>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pz {
namespace {
constexpr int ID_INPUT = 101, ID_ADD = 102, ID_LIST = 103, ID_LOG = 104;
constexpr int ID_UPDATE = 105, ID_REMOVE = 106, ID_UPDATE_ALL = 107, ID_SETTINGS = 108;
constexpr int ID_SELECT_ALL = 109, ID_CLEAR = 110;
constexpr int ID_FILTER = 111, ID_FILTER_ALL = 112, ID_FILTER_UPDATES = 113;
constexpr int ID_SORT = 114, ID_CHECK = 115, ID_COPY = 116, ID_ENABLE = 117;
constexpr int ID_SHARE_SERVER = 118, ID_SELF_UPDATE = 119;
constexpr int ID_SET_SAVE = 201, ID_SET_CANCEL = 202, ID_SET_THEME = 203, ID_SET_AUTO = 204;
constexpr int ID_TGT_LIST = 301, ID_TGT_ENABLE = 302, ID_TGT_DISABLE = 303;
constexpr int ID_TGT_COPY = 304, ID_TGT_CLOSE = 305, ID_TGT_COPY_NAMES = 306;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr UINT WM_APP_RELOAD = WM_APP + 3;
constexpr UINT_PTR kAutoCheckTimer = 1;
constexpr int kWindowW = 920, kWindowH = 720;
constexpr int kRowH = 64;

// Thin flat scrollbars, sized like the X11 ones: a 6px bar with 3px of click
// slack on each side, drawn in the window instead of by the OS.
constexpr int kScrollW = 6;
constexpr int kScrollGrab = 3;
constexpr int kScrollWinW = kScrollW + 2 * kScrollGrab;
constexpr int kScrollMinThumb = 24;
struct ScrollBar {
    HWND wnd = nullptr;
    HWND target = nullptr;
    bool log = false;          // multiline edit instead of a listbox
    bool on_surface = false;   // background matches a card, not the window
    bool dragging = false;
    int drag_offset = 0;
    int last_total = -1, last_page = -1, last_pos = -1;
};
std::vector<ScrollBar*> g_bars;

struct Theme {
    COLORREF bg, surface, field, border, border_strong, text, muted;
    COLORREF accent, accent_hover, accent_soft, danger, danger_soft;
    COLORREF brand, brand_soft, brand_border, cream, hover, success, on_accent;
};
Theme g_theme{};
bool g_dark_mode = false;

ModManager* g_mm = nullptr;
HWND g_hwnd = nullptr;
HWND g_title = nullptr;
HWND g_subtitle = nullptr;
HWND g_brand_mark = nullptr;
HWND g_brand_line = nullptr;
HWND g_input = nullptr;
HWND g_add = nullptr;
HWND g_filter = nullptr;
HWND g_filter_all = nullptr;
HWND g_filter_updates = nullptr;
HWND g_sort = nullptr;
HWND g_check = nullptr;
HWND g_list = nullptr;
HWND g_empty = nullptr;
WNDPROC g_list_old_proc = nullptr;
WNDPROC g_log_old_proc = nullptr;
WNDPROC g_tgt_list_old_proc = nullptr;
HWND g_log = nullptr;
HWND g_update = nullptr;
HWND g_remove = nullptr;
HWND g_update_all = nullptr;
HWND g_settings_btn = nullptr;
HWND g_self_update = nullptr;
HWND g_enable_btn = nullptr;
HWND g_share_server_btn = nullptr;
HWND g_select_all = nullptr;
HWND g_clear = nullptr;
HWND g_copy = nullptr;
HWND g_details = nullptr;
HWND g_status = nullptr;
HWND g_count = nullptr;
HWND g_activity_label = nullptr;

HWND g_settings_wnd = nullptr;
HWND g_set_zomboid = nullptr;
HWND g_set_path = nullptr;
HWND g_set_user = nullptr;
HWND g_set_auto = nullptr;
bool g_settings_dark = false;
HWND g_targets_wnd = nullptr;
HWND g_tgt_list = nullptr;
HWND g_tgt_status = nullptr;
HWND g_tgt_copy = nullptr;
HWND g_tgt_copy_names = nullptr;
std::vector<ActivationTarget> g_targets;
std::vector<std::vector<std::string>> g_row_mod_ids;
std::vector<char> g_row_enabled;
std::vector<ActivationTarget> g_activation;
std::vector<std::string> g_target_mod_ids;

HFONT g_font_title = nullptr;
HFONT g_font_row = nullptr;
HFONT g_font_brand = nullptr;
HFONT g_font_body = nullptr;
HFONT g_font_small = nullptr;
HBRUSH g_bg_brush = nullptr;
HBRUSH g_card_brush = nullptr;
HBRUSH g_field_brush = nullptr;
HBRUSH g_selected_brush = nullptr;
HBRUSH g_border_brush = nullptr;
HBRUSH g_accent_brush = nullptr;
HBRUSH g_brand_brush = nullptr;
HBRUSH g_brand_soft_brush = nullptr;
HBRUSH g_border_strong_brush = nullptr;

std::thread g_worker;
std::thread g_watch_worker;
HANDLE g_watch_change = nullptr;
HANDLE g_watch_stop = nullptr;
std::mutex g_pending_mu;
std::deque<std::string> g_pending;
std::vector<std::wstring> g_row_names;
std::vector<std::wstring> g_row_meta;
std::vector<std::string> g_row_folders;
std::vector<std::string> g_row_icon_paths;
std::vector<std::uint64_t> g_row_ids;
std::vector<char> g_row_updates;
std::vector<std::uint64_t> g_selected_ids;
std::vector<std::string> g_selected_folders;
bool g_only_updates = false;
int g_sort_mode = 0;
std::map<std::string, HBITMAP> g_icons;
std::map<std::string, HBITMAP> g_target_icons;
std::map<std::string, std::optional<ModInfo>> g_info_cache;
std::string g_dependency_warning;
std::string g_dependency_signature;
bool g_busy = false;
bool g_reload_pending = false;

struct EditPlaceholder {
    WNDPROC old_proc = nullptr;
    const wchar_t* text = nullptr;
};
std::map<HWND, EditPlaceholder> g_edit_placeholders;

std::wstring to_w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string to_u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr,
                        nullptr);
    return s;
}

void stop_watcher() {
    if (g_watch_stop) SetEvent(g_watch_stop);
    if (g_watch_worker.joinable()) g_watch_worker.join();
    if (g_watch_change) {
        FindCloseChangeNotification(g_watch_change);
        g_watch_change = nullptr;
    }
    if (g_watch_stop) {
        CloseHandle(g_watch_stop);
        g_watch_stop = nullptr;
    }
}

bool start_watcher(const fs::path& dir) {
    stop_watcher();
    g_watch_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_watch_stop) return false;
    const std::wstring path = to_w(dir.string());
    g_watch_change = FindFirstChangeNotificationW(
        path.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE);
    if (g_watch_change == INVALID_HANDLE_VALUE) {
        g_watch_change = nullptr;
        CloseHandle(g_watch_stop);
        g_watch_stop = nullptr;
        return false;
    }
    try {
        g_watch_worker = std::thread([] {
            HANDLE handles[2] = {g_watch_change, g_watch_stop};
            for (;;) {
                const DWORD result =
                    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0 + 1) break;
                if (result != WAIT_OBJECT_0) break;
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_RELOAD, 0, 0);
                if (!FindNextChangeNotification(g_watch_change)) break;
            }
        });
    } catch (...) {
        stop_watcher();
        return false;
    }
    return true;
}

Theme make_theme(bool dark) {
    Theme t;
    if (!dark) {
        t.bg = RGB(246, 247, 249);
        t.surface = RGB(255, 255, 255);
        t.field = RGB(255, 255, 255);
        t.border = RGB(220, 225, 232);
        t.border_strong = RGB(194, 202, 214);
        t.text = RGB(31, 38, 46);
        t.muted = RGB(108, 118, 132);
        t.accent = RGB(224, 100, 43);
        t.accent_hover = RGB(198, 82, 30);
        t.accent_soft = RGB(255, 242, 234);
        t.danger = RGB(190, 55, 55);
        t.danger_soft = RGB(253, 237, 237);
        t.brand = RGB(44, 99, 69);
        t.brand_soft = RGB(235, 244, 238);
        t.brand_border = RGB(182, 204, 188);
        t.cream = RGB(255, 248, 234);
        t.hover = RGB(244, 248, 245);
        t.success = RGB(36, 122, 80);
        t.on_accent = RGB(255, 255, 255);
    } else {
        t.bg = RGB(18, 21, 24);
        t.surface = RGB(28, 32, 36);
        t.field = RGB(22, 26, 30);
        t.border = RGB(58, 64, 70);
        t.border_strong = RGB(78, 86, 94);
        t.text = RGB(232, 236, 240);
        t.muted = RGB(154, 163, 173);
        t.accent = RGB(232, 116, 58);
        t.accent_hover = RGB(245, 132, 72);
        t.accent_soft = RGB(58, 36, 27);
        t.danger = RGB(230, 100, 100);
        t.danger_soft = RGB(52, 28, 30);
        t.brand = RGB(96, 180, 130);
        t.brand_soft = RGB(26, 46, 36);
        t.brand_border = RGB(64, 110, 84);
        t.cream = RGB(255, 248, 234);
        t.hover = RGB(36, 44, 40);
        t.success = RGB(96, 180, 130);
        t.on_accent = RGB(255, 248, 234);
    }
    return t;
}

void destroy_theme_brushes() {
    for (HBRUSH* brush : {&g_bg_brush, &g_card_brush, &g_field_brush,
                          &g_selected_brush, &g_border_brush, &g_accent_brush,
                          &g_brand_brush, &g_brand_soft_brush,
                          &g_border_strong_brush}) {
        if (*brush) DeleteObject(*brush);
        *brush = nullptr;
    }
}

void create_theme_brushes() {
    g_bg_brush = CreateSolidBrush(g_theme.bg);
    g_card_brush = CreateSolidBrush(g_theme.surface);
    g_field_brush = CreateSolidBrush(g_theme.field);
    g_selected_brush = CreateSolidBrush(g_theme.accent_soft);
    g_border_brush = CreateSolidBrush(g_theme.border);
    g_accent_brush = CreateSolidBrush(g_theme.accent);
    g_brand_brush = CreateSolidBrush(g_theme.brand);
    g_brand_soft_brush = CreateSolidBrush(g_theme.brand_soft);
    g_border_strong_brush = CreateSolidBrush(g_theme.border_strong);
}

void apply_theme(bool dark, HWND hwnd) {
    g_dark_mode = dark;
    g_theme = make_theme(dark);
    destroy_theme_brushes();
    create_theme_brushes();
    // Every window class uses a null background brush and erases itself from
    // the palette, so there is no class brush to keep in sync here.
    if (hwnd) {
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

// dwmapi is resolved at run time: the app keeps its current link dependencies
// and simply keeps a light caption on Windows versions without the attribute.
void apply_dark_title_bar(HWND hwnd, bool dark) {
    using SetAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static SetAttributeFn set_attribute = []() -> SetAttributeFn {
        HMODULE dll = LoadLibraryW(L"dwmapi.dll");
        if (!dll) return nullptr;
        return reinterpret_cast<SetAttributeFn>(
            reinterpret_cast<void*>(GetProcAddress(dll, "DwmSetWindowAttribute")));
    }();
    if (!set_attribute || !hwnd) return;
    const BOOL value = dark ? TRUE : FALSE;
    // 20 on Windows 10 2004+, 19 on 1809-1909.
    if (FAILED(set_attribute(hwnd, 20, &value, sizeof(value))))
        set_attribute(hwnd, 19, &value, sizeof(value));
}

// Edit controls draw their own scrollbar from the system theme, ignoring
// WM_CTLCOLOR. Ask uxtheme for the dark variant where it exists; the
// AllowDarkModeForWindow entry point is an ordinal, so resolve it dynamically.
void apply_dark_control_theme(HWND hwnd, bool dark) {
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    using AllowDarkFn = BOOL(WINAPI*)(HWND, BOOL);
    static HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    static SetWindowThemeFn set_theme = uxtheme
        ? reinterpret_cast<SetWindowThemeFn>(
              reinterpret_cast<void*>(GetProcAddress(uxtheme, "SetWindowTheme")))
        : nullptr;
    static AllowDarkFn allow_dark = uxtheme
        ? reinterpret_cast<AllowDarkFn>(reinterpret_cast<void*>(
              GetProcAddress(uxtheme, MAKEINTRESOURCEA(135))))
        : nullptr;
    if (!hwnd) return;
    if (allow_dark) allow_dark(hwnd, dark ? TRUE : FALSE);
    if (set_theme) set_theme(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void invalidate_bars();

void refresh_theme(HWND hwnd, bool dark) {
    apply_theme(dark, hwnd);
    apply_dark_title_bar(hwnd, dark);
    apply_dark_control_theme(g_input, dark);
    apply_dark_control_theme(g_filter, dark);
    apply_dark_control_theme(g_log, dark);
    invalidate_bars();
}

void fill_color(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

void draw_field_frame(HDC dc, const RECT& field, bool focused) {
    RECT outer{field.left - 1, field.top - 1, field.right + 1, field.bottom + 1};
    fill_color(dc, outer, focused ? g_theme.accent : g_theme.border);
}

// The 18px checkbox from the X11 settings panel.
void draw_checkbox(HDC dc, int x, int y, bool checked) {
    RECT box{x, y, x + 18, y + 18};
    fill_color(dc, box, checked ? g_theme.accent : g_theme.field);
    HBRUSH frame = CreateSolidBrush(checked ? g_theme.accent : g_theme.border_strong);
    FrameRect(dc, &box, frame);
    DeleteObject(frame);
    if (!checked) return;
    HPEN pen = CreatePen(PS_SOLID, 2, g_theme.on_accent);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x + 4, y + 9, nullptr);
    LineTo(dc, x + 7, y + 12);
    LineTo(dc, x + 14, y + 4);
    SelectObject(dc, old);
    DeleteObject(pen);
}

// Buttons in the modal windows: same flat look as the main toolbar, without
// the glyphs (the glyph set only covers the main actions).
void draw_dialog_button(const DRAWITEMSTRUCT* dis, bool primary) {
    if (!dis) return;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    RECT r = dis->rcItem;
    COLORREF fill = g_theme.surface;
    COLORREF border = g_theme.border;
    COLORREF text = g_theme.text;
    if (disabled) {
        fill = g_theme.bg;
        text = g_theme.muted;
    } else if (primary) {
        fill = pressed ? g_theme.accent_hover : g_theme.accent;
        border = fill;
        text = g_theme.on_accent;
    } else if (pressed) {
        fill = g_theme.hover;
        border = g_theme.brand_border;
    }
    fill_color(dis->hDC, r, fill);
    HBRUSH frame = CreateSolidBrush(border);
    FrameRect(dis->hDC, &r, frame);
    DeleteObject(frame);

    wchar_t label[128]{};
    GetWindowTextW(dis->hwndItem, label, 127);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    HGDIOBJ old = SelectObject(dis->hDC, g_font_body);
    DrawTextW(dis->hDC, label, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

// --- thin scrollbars -------------------------------------------------------

struct ScrollMetrics {
    int total = 0;
    int page = 1;
    int pos = 0;
    bool needed = false;
};

int edit_page_lines(HWND edit) {
    RECT rc{};
    GetClientRect(edit, &rc);
    HDC dc = GetDC(edit);
    if (!dc) return 1;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    if (old) SelectObject(dc, old);
    ReleaseDC(edit, dc);
    const int height = static_cast<int>(rc.bottom - rc.top);
    return tm.tmHeight > 0 ? std::max(1, height / static_cast<int>(tm.tmHeight))
                           : 1;
}

ScrollMetrics bar_metrics(const ScrollBar& bar) {
    ScrollMetrics m;
    RECT rc{};
    GetClientRect(bar.target, &rc);
    if (!bar.log) {
        m.total = static_cast<int>(SendMessageW(bar.target, LB_GETCOUNT, 0, 0));
        const int item_h =
            static_cast<int>(SendMessageW(bar.target, LB_GETITEMHEIGHT, 0, 0));
        m.page = item_h > 0
            ? std::max(1, static_cast<int>(rc.bottom - rc.top) / item_h)
            : 1;
        m.pos = static_cast<int>(SendMessageW(bar.target, LB_GETTOPINDEX, 0, 0));
    } else {
        m.total = static_cast<int>(SendMessageW(bar.target, EM_GETLINECOUNT, 0, 0));
        m.pos = static_cast<int>(SendMessageW(bar.target, EM_GETFIRSTVISIBLELINE, 0, 0));
        m.page = edit_page_lines(bar.target);
    }
    if (m.total < 0) m.total = 0;
    if (m.page < 1) m.page = 1;
    m.pos = std::min(std::max(0, m.pos), std::max(0, m.total - m.page));
    m.needed = m.total > m.page;
    return m;
}

int thumb_height(const ScrollMetrics& m, int track) {
    int h = track * m.page / std::max(1, m.total);
    h = std::max(kScrollMinThumb, h);
    return std::min(h, track);
}

bool thumb_rect(const ScrollBar& bar, const ScrollMetrics& m, RECT& thumb) {
    RECT rc{};
    GetClientRect(bar.wnd, &rc);
    const int track = rc.bottom - rc.top;
    if (!m.needed || track <= 0) return false;
    const int h = thumb_height(m, track);
    const int travel = track - h;
    const int max_pos = std::max(1, m.total - m.page);
    const int y = travel * m.pos / max_pos;
    thumb = {kScrollGrab, y, kScrollGrab + kScrollW, y + h};
    return true;
}

void update_count_label();

void bar_scroll_to(ScrollBar& bar, int pos) {
    const ScrollMetrics m = bar_metrics(bar);
    pos = std::min(std::max(0, pos), std::max(0, m.total - m.page));
    if (!bar.log) {
        SendMessageW(bar.target, LB_SETTOPINDEX, pos, 0);
    } else {
        const int delta = pos - static_cast<int>(
            SendMessageW(bar.target, EM_GETFIRSTVISIBLELINE, 0, 0));
        if (delta != 0) SendMessageW(bar.target, EM_LINESCROLL, 0, delta);
    }
    bar.last_total = m.total;
    bar.last_page = m.page;
    bar.last_pos = pos;
    InvalidateRect(bar.wnd, nullptr, FALSE);
    if (!bar.log) update_count_label();
}

// The counter beside the filter row: the X11 UI shows "1-5 of 12" while the
// list is scrolled, so the visible window into the mods is always obvious.
void update_count_label() {
    if (!g_count || !g_list) return;
    const int total = static_cast<int>(SendMessageW(g_list, LB_GETCOUNT, 0, 0));
    const int item_h =
        static_cast<int>(SendMessageW(g_list, LB_GETITEMHEIGHT, 0, 0));
    RECT rc{};
    GetClientRect(g_list, &rc);
    const int rows = item_h > 0
        ? std::max(1, static_cast<int>(rc.bottom - rc.top) / item_h)
        : 1;
    int first = static_cast<int>(SendMessageW(g_list, LB_GETTOPINDEX, 0, 0));
    if (first < 0) first = 0;

    std::wstring text;
    if (g_busy) {
        text = L"Working...";
    } else if (total <= 0) {
        text = L"0 mods";
    } else if (total <= rows) {
        text = to_w(std::to_string(total)) + (total == 1 ? L" mod" : L" mods");
    } else {
        const int last = std::min(total, first + rows);
        text = to_w(std::to_string(first + 1) + "-" + std::to_string(last) +
                    " of " + std::to_string(total));
    }
    static std::wstring shown;
    if (text == shown) return;
    shown = text;
    SetWindowTextW(g_count, text.c_str());
}

void sync_bars() {
    for (ScrollBar* bar : g_bars) {
        if (!bar || !bar->wnd) continue;
        const ScrollMetrics m = bar_metrics(*bar);
        if (m.total != bar->last_total || m.page != bar->last_page ||
            m.pos != bar->last_pos) {
            bar->last_total = m.total;
            bar->last_page = m.page;
            bar->last_pos = m.pos;
            InvalidateRect(bar->wnd, nullptr, FALSE);
        }
    }
    update_count_label();
}

void invalidate_bars() {
    for (ScrollBar* bar : g_bars) {
        if (!bar || !bar->wnd) continue;
        bar->last_total = -1;   // force a repaint with the new palette
        InvalidateRect(bar->wnd, nullptr, FALSE);
    }
}

void paint_scrollbar(const ScrollBar& bar, HDC dc) {
    RECT rc{};
    GetClientRect(bar.wnd, &rc);
    fill_color(dc, rc, bar.on_surface ? g_theme.surface : g_theme.bg);
    const ScrollMetrics m = bar_metrics(bar);
    if (!m.needed) return;
    RECT track{kScrollGrab, 0, kScrollGrab + kScrollW, rc.bottom};
    FillRect(dc, &track, g_border_brush);
    RECT thumb{};
    if (thumb_rect(bar, m, thumb))
        FillRect(dc, &thumb, bar.dragging ? g_accent_brush : g_border_strong_brush);
}

LRESULT CALLBACK ScrollProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* bar = reinterpret_cast<ScrollBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (bar) paint_scrollbar(*bar, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!bar) return 0;
        SetFocus(bar->target);
        const int y = static_cast<short>(HIWORD(lp));
        const ScrollMetrics m = bar_metrics(*bar);
        RECT thumb{};
        if (thumb_rect(*bar, m, thumb) && y >= thumb.top - kScrollGrab &&
            y < thumb.bottom + kScrollGrab) {
            bar->dragging = true;
            bar->drag_offset = y - thumb.top;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (m.needed) {
            bar_scroll_to(*bar, y < thumb.top ? m.pos - m.page : m.pos + m.page);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!bar || !bar->dragging) return 0;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const ScrollMetrics m = bar_metrics(*bar);
        const int track = rc.bottom - rc.top;
        const int travel = track - thumb_height(m, track);
        const int max_pos = std::max(0, m.total - m.page);
        if (travel > 0 && max_pos > 0) {
            const int y = static_cast<short>(HIWORD(lp));
            const int offset = std::min(std::max(0, y - bar->drag_offset), travel);
            bar_scroll_to(*bar, (offset * max_pos + travel / 2) / travel);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (bar && bar->dragging) {
            bar->dragging = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (bar && bar->dragging) {
            bar->dragging = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (bar) SendMessageW(bar->target, WM_MOUSEWHEEL, wp, lp);
        return 0;
    case WM_NCDESTROY: {
        auto it = std::find(g_bars.begin(), g_bars.end(), bar);
        if (it != g_bars.end()) g_bars.erase(it);
        delete bar;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_scrollbar_class() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ScrollProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PZScrollBar";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    registered = true;
}

HWND create_scrollbar(HWND parent, HWND target, bool log, bool on_surface,
                      int x, int y, int w, int h) {
    register_scrollbar_class();
    HWND bar = CreateWindowExW(0, L"PZScrollBar", L"", WS_CHILD | WS_VISIBLE,
                               x, y, w, h, parent, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!bar) return nullptr;
    auto* state = new ScrollBar;
    state->wnd = bar;
    state->target = target;
    state->log = log;
    state->on_surface = on_surface;
    g_bars.push_back(state);
    SetWindowLongPtrW(bar, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    sync_bars();
    return bar;
}

void set_font(HWND h, HFONT font) {
    if (h && font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
}

HFONT make_font(int points, int weight) {
    HDC dc = GetDC(nullptr);
    const int height = -MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, dc);
    LOGFONTW lf{};
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

void append_log(const std::string& line) {
    std::wstring w = to_w(line) + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    sync_bars();
}

void set_status(const std::string& s) {
    SetWindowTextW(g_status, to_w(s).c_str());
    InvalidateRect(g_status, nullptr, TRUE);
}

void refresh_dependency_warning() {
    if (!g_mm) return;
    const auto notice = dependency_notice(g_mm->missing_requirements());
    if (notice.signature != g_dependency_signature) {
        if (notice.messages.empty()) {
            if (!g_dependency_signature.empty()) append_log("required mods satisfied");
        } else {
            for (const auto& line : notice.messages) append_log(line);
        }
        g_dependency_signature = notice.signature;
    }
    g_dependency_warning = notice.status;
}

std::string read_edit(HWND edit);

void update_actions() {
    const bool has_ids = !g_selected_ids.empty();
    const bool has_any = has_ids || !g_selected_folders.empty();
    EnableWindow(g_add, !g_busy);
    EnableWindow(g_update_all, !g_busy);
    EnableWindow(g_settings_btn, !g_busy);
    EnableWindow(g_self_update, !g_busy);
    EnableWindow(g_share_server_btn, !g_busy);
    EnableWindow(g_select_all, !g_busy && !g_row_ids.empty());
    EnableWindow(g_clear, !g_busy && has_any);
    EnableWindow(g_copy, !g_busy && has_ids);
    EnableWindow(g_enable_btn, !g_busy && has_any);
    EnableWindow(g_update, !g_busy && has_ids);
    EnableWindow(g_remove, !g_busy && has_ids);
    EnableWindow(g_filter, !g_busy);
    EnableWindow(g_filter_all, !g_busy);
    EnableWindow(g_filter_updates, !g_busy);
    EnableWindow(g_sort, !g_busy);
    EnableWindow(g_check, !g_busy);
}

std::vector<std::uint64_t> selected_ids() {
    std::vector<std::uint64_t> ids;
    int count = (int)SendMessageW(g_list, LB_GETSELCOUNT, 0, 0);
    if (count < 0) {
        int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)g_row_ids.size() && g_row_ids[(std::size_t)sel] != 0)
            ids.push_back(g_row_ids[(std::size_t)sel]);
        return ids;
    }
    if (count == 0) return ids;

    std::vector<int> rows((std::size_t)count);
    int got = (int)SendMessageW(g_list, LB_GETSELITEMS, count, (LPARAM)rows.data());
    for (int i = 0; i < got; ++i) {
        int index = rows[(std::size_t)i];
        if (index < 0 || index >= (int)g_row_ids.size()) continue;
        auto id = g_row_ids[(std::size_t)index];
        if (id == 0) continue;
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    return ids;
}

std::vector<std::string> selected_folders() {
    std::vector<std::string> folders;
    int count = (int)SendMessageW(g_list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) {
        const int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)g_row_ids.size() && g_row_ids[(std::size_t)sel] == 0 &&
            sel < (int)g_row_folders.size())
            folders.push_back(g_row_folders[(std::size_t)sel]);
        return folders;
    }
    std::vector<int> rows((std::size_t)count);
    const int got = (int)SendMessageW(g_list, LB_GETSELITEMS, count, (LPARAM)rows.data());
    for (int i = 0; i < got; ++i) {
        const int index = rows[(std::size_t)i];
        if (index < 0 || index >= (int)g_row_ids.size() || index >= (int)g_row_folders.size())
            continue;
        if (g_row_ids[(std::size_t)index] != 0) continue;
        const auto& folder = g_row_folders[(std::size_t)index];
        if (std::find(folders.begin(), folders.end(), folder) == folders.end())
            folders.push_back(folder);
    }
    return folders;
}

std::vector<std::wstring> selected_row_names() {
    std::vector<std::wstring> names;
    for (std::size_t i = 0; i < g_row_names.size(); ++i) {
        const std::uint64_t id = i < g_row_ids.size() ? g_row_ids[i] : 0;
        const bool selected = id != 0
            ? std::find(g_selected_ids.begin(), g_selected_ids.end(), id) !=
                  g_selected_ids.end()
            : i < g_row_folders.size() &&
                  std::find(g_selected_folders.begin(), g_selected_folders.end(),
                            g_row_folders[i]) != g_selected_folders.end();
        if (selected) names.push_back(g_row_names[i]);
    }
    return names;
}

void set_details() {
    const auto names = selected_row_names();
    if (names.empty()) {
        SetWindowTextW(g_details, L"No selection");
        return;
    }
    std::wstring text;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) text += L", ";
        text += names[i];
    }
    // The static control uses SS_ENDELLIPSIS, so a long selection stays on one
    // line while still naming every selected mod.
    SetWindowTextW(g_details, text.c_str());
}

HBITMAP load_icon_bitmap(const fs::path& path, int size = 48) {
    ModIcon icon;
    if (path.empty() || size <= 0 || !load_mod_icon(path, size, icon)) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return nullptr;

    auto* pixels = static_cast<std::uint8_t*>(bits);
    const int bg_r = GetRValue(g_theme.surface);
    const int bg_g = GetGValue(g_theme.surface);
    const int bg_b = GetBValue(g_theme.surface);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * size + x) * 4;
            const std::size_t dst = src;
            const int a = icon.rgba[src + 3];
            pixels[dst + 0] = static_cast<std::uint8_t>(
                (icon.rgba[src + 2] * a + bg_b * (255 - a)) / 255);
            pixels[dst + 1] = static_cast<std::uint8_t>(
                (icon.rgba[src + 1] * a + bg_g * (255 - a)) / 255);
            pixels[dst + 2] = static_cast<std::uint8_t>(
                (icon.rgba[src + 0] * a + bg_r * (255 - a)) / 255);
            pixels[dst + 3] = 255;
        }
    }
    return bitmap;
}

void clear_icons() {
    for (auto& [folder, bitmap] : g_icons) {
        (void)folder;
        if (bitmap) DeleteObject(bitmap);
    }
    g_icons.clear();
    g_info_cache.clear();
}

void clear_target_icons() {
    for (auto& [path, bitmap] : g_target_icons) {
        (void)path;
        if (bitmap) DeleteObject(bitmap);
    }
    g_target_icons.clear();
}

HBITMAP target_icon_bitmap(const ActivationTarget& target) {
    if (target.kind != ActivationKind::Save || target.thumbnail.empty()) return nullptr;
    const std::string key = target.thumbnail.string();
    auto it = g_target_icons.find(key);
    if (it == g_target_icons.end())
        it = g_target_icons.emplace(key, load_icon_bitmap(target.thumbnail, 36)).first;
    return it->second;
}

const std::optional<ModInfo>& info_for_cached(const std::string& folder) {
    auto it = g_info_cache.find(folder);
    if (it == g_info_cache.end())
        it = g_info_cache.emplace(folder, g_mm->info_for(folder)).first;
    return it->second;
}

void refresh_list() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_row_ids.clear();
    g_row_names.clear();
    g_row_meta.clear();
    g_row_folders.clear();
    g_row_updates.clear();
    g_row_icon_paths.clear();
    g_row_mod_ids.clear();
    g_row_enabled.clear();
    g_activation = g_mm->activation_targets();

    const std::string filter = read_edit(g_filter);
    auto visible = visible_mods(g_mm->list_all(), filter, g_only_updates, g_sort_mode,
        [&](std::uint64_t id) { return g_mm->update_available(id); });

    for (const auto& m : visible) {
        const auto& info = info_for_cached(m.folders.front());
        const std::string title = (info && !info->name.empty()) ? info->name
            : (m.name.empty() ? m.folders.front() : m.name);
        std::vector<std::string> mod_ids;
        for (const auto& folder : m.folders) {
            const auto& folder_meta = info_for_cached(folder);
            if (!folder_meta || folder_meta->id.empty()) continue;
            if (std::find(mod_ids.begin(), mod_ids.end(), folder_meta->id) ==
                mod_ids.end())
                mod_ids.push_back(folder_meta->id);
        }
        const std::string on_targets = enabled_in(g_activation, mod_ids);
        const std::string meta = mod_meta(
            m, info ? &info.value() : nullptr, g_mm->game_version(), on_targets);
        g_row_enabled.push_back(enabled_in(g_activation, mod_ids).empty() ? 0 : 1);
        g_row_mod_ids.push_back(std::move(mod_ids));
        g_row_names.push_back(to_w(title));
        g_row_meta.push_back(to_w(meta));
        g_row_folders.push_back(m.folders.front());
        g_row_icon_paths.push_back(info && !info->icon.empty() ? info->icon.string()
                                                              : std::string{});
        g_row_updates.push_back(g_mm->update_available(m.workshop_id) ? 1 : 0);
        LRESULT added = SendMessageW(g_list, LB_ADDSTRING, 0,
                                     (LPARAM)g_row_names.back().c_str());
        if (added >= 0) g_row_ids.push_back(m.workshop_id);
    }

    std::vector<std::uint64_t> keep;
    for (auto id : g_selected_ids) {
        if (std::find(g_row_ids.begin(), g_row_ids.end(), id) == g_row_ids.end()) continue;
        if (std::find(keep.begin(), keep.end(), id) == keep.end()) keep.push_back(id);
    }
    g_selected_ids.swap(keep);
    std::vector<std::string> keep_folders;
    for (const auto& folder : g_selected_folders) {
        if (std::find(g_row_folders.begin(), g_row_folders.end(), folder) ==
            g_row_folders.end()) continue;
        if (std::find(keep_folders.begin(), keep_folders.end(), folder) == keep_folders.end())
            keep_folders.push_back(folder);
    }
    g_selected_folders.swap(keep_folders);
    for (std::size_t i = 0; i < g_row_ids.size(); ++i) {
        const bool selected = g_row_ids[i] != 0
            ? std::find(g_selected_ids.begin(), g_selected_ids.end(), g_row_ids[i]) !=
                  g_selected_ids.end()
            : std::find(g_selected_folders.begin(), g_selected_folders.end(),
                        g_row_folders[i]) != g_selected_folders.end();
        if (selected) SendMessageW(g_list, LB_SETSEL, TRUE, (LPARAM)i);
    }

    if (g_empty) {
        const bool filtered = !filter.empty() || g_only_updates;
        SetWindowTextW(g_empty,
            filtered ? L"No mods match the current filters."
                     : L"No mods installed yet. Paste a Workshop link above to begin.");
        ShowWindow(g_empty, g_row_ids.empty() ? SW_SHOW : SW_HIDE);
    }
    SetWindowTextW(g_sort, g_sort_mode == 0 ? L"Sort: Name" : L"Sort: Updated");
    for (HWND b : {g_filter_all, g_filter_updates, g_sort}) InvalidateRect(b, nullptr, TRUE);
    sync_bars();
    update_actions();
    set_details();
    set_status(g_dependency_warning.empty() ? "Ready" : g_dependency_warning);
}

void sync_selection() {
    g_selected_ids = selected_ids();
    g_selected_folders = selected_folders();
    update_actions();
    set_details();
}

LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_busy && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
        const int mx = static_cast<short>(LOWORD(lp));
        const int my = static_cast<short>(HIWORD(lp));
        const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0,
                                         MAKELPARAM(mx, my));
        const int item = static_cast<int>(LOWORD(hit));
        if (HIWORD(hit) == 0 && item >= 0 && item < static_cast<int>(g_row_ids.size())) {
            RECT item_rc{};
            if (SendMessageW(hwnd, LB_GETITEMRECT, item, (LPARAM)&item_rc) != LB_ERR) {
                if (msg == WM_LBUTTONDOWN &&
                    (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                    (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                    // A plain row click toggles that row without replacing the
                    // rest of the selection. Ctrl/Shift keep their native
                    // listbox behavior.
                    const LRESULT selected = SendMessageW(hwnd, LB_GETSEL, item, 0);
                    SendMessageW(hwnd, LB_SETSEL, selected ? FALSE : TRUE, item);
                    SendMessageW(hwnd, LB_SETANCHORINDEX, item, 0);
                    SendMessageW(hwnd, LB_SETCARETINDEX, item, FALSE);
                    SetFocus(hwnd);
                    sync_selection();
                    InvalidateRect(hwnd, &item_rc, FALSE);
                    return 0;
                }
            }
        }
    }
    const LRESULT result = CallWindowProcW(g_list_old_proc, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_SIZE:
    case WM_SETREDRAW:
    case WM_TIMER:
        sync_bars();
        break;
    default:
        break;
    }
    return result;
}

LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const LRESULT result = CallWindowProcW(g_log_old_proc, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_LBUTTONUP:
    case WM_SIZE:
    case WM_SETREDRAW:
    case WM_TIMER:
        sync_bars();
        break;
    default:
        break;
    }
    return result;
}

// The targets list scrolls itself; keep its bar in step the same way.
LRESULT CALLBACK TargetsListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const LRESULT result = CallWindowProcW(g_tgt_list_old_proc, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_SIZE:
    case WM_SETREDRAW:
    case WM_TIMER:
        sync_bars();
        break;
    default:
        break;
    }
    return result;
}

void select_all() {
    if (g_row_ids.empty()) return;
    SendMessageW(g_list, LB_SETSEL, TRUE, -1);
    sync_selection();
}

void clear_selection() {
    SendMessageW(g_list, LB_SETSEL, FALSE, -1);
    g_selected_ids.clear();
    g_selected_folders.clear();
    update_actions();
    set_details();
}

bool copy_plain_text(const std::string& value) {
    if (value.empty()) return false;
    const std::wstring text = to_w(value);
    if (!OpenClipboard(g_hwnd)) return false;
    EmptyClipboard();
    bool copied = false;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

bool copy_workshop_ids(const std::vector<std::uint64_t>& ids) {
    if (ids.empty()) return false;
    const std::wstring text = to_w(join_workshop_ids(ids));
    if (!OpenClipboard(g_hwnd)) {
        append_log("Could not open the clipboard.");
        return false;
    }
    EmptyClipboard();
    bool copied = false;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

void copy_selected_ids() {
    const auto ids = selected_ids();
    if (ids.empty()) return;
    if (copy_workshop_ids(ids))
        append_log("Copied " + std::to_string(ids.size()) +
                   " Workshop ID(s). Paste them into another pzmodmgr.");
    else
        append_log("Could not write to the clipboard.");
}

void drain_pending() {
    std::deque<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(g_pending_mu);
        batch.swap(g_pending);
    }
    for (const auto& line : batch) append_log(line);
}

void worker_main(std::string line) {
    auto r = run_command(*g_mm, line, [](const std::string& s) {
        {
            std::lock_guard<std::mutex> lock(g_pending_mu);
            g_pending.push_back(s);
        }
        PostMessageW(g_hwnd, WM_APP_LOG, 0, 0);
    });

    {
        std::lock_guard<std::mutex> lock(g_pending_mu);
        g_pending.push_back(std::string(r.ok ? "OK: " : "ERROR: ") + r.message);
    }
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

void start_command(const std::string& line) {
    if (g_busy || line.empty()) return;

    append_log("> " + line);
    g_busy = true;
    EnableWindow(g_input, FALSE);
    EnableWindow(g_list, FALSE);
    EnableWindow(g_filter, FALSE);
    update_actions();
    update_count_label();
    set_status("Working - steamcmd may take a while...");

    if (g_worker.joinable()) g_worker.join();
    try {
        g_worker = std::thread(worker_main, line);
    } catch (const std::exception& e) {
        g_busy = false;
        EnableWindow(g_input, TRUE);
        EnableWindow(g_list, TRUE);
        update_actions();
        update_count_label();
        set_status("Ready");
        append_log(std::string("ERROR: cannot start worker: ") + e.what());
    }
}

std::string read_edit(HWND edit) {
    int len = GetWindowTextLengthW(edit);
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(edit, buf.data(), len + 1);
    buf.resize(len);
    return to_u8(buf);
}

// The X11 UI keeps its cue visible whenever the field is empty. Draw it after
// the edit paints so Win32 does not fall back to a focus-sensitive cue banner.
void sync_edit_cue_caret(HWND edit) {
    if (GetFocus() != edit) return;
    if (GetWindowTextLengthW(edit) == 0) HideCaret(edit);
    else ShowCaret(edit);
}

void draw_edit_placeholder(HWND edit, const wchar_t* text) {
    if (!text || GetWindowTextLengthW(edit) != 0) return;
    HDC dc = GetDC(edit);
    if (!dc) return;

    RECT rc{};
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rc));
    const int old_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_color = SetTextColor(dc, g_theme.muted);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    DrawTextW(dc, text, -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (old_font) SelectObject(dc, old_font);
    SetTextColor(dc, old_color);
    SetBkMode(dc, old_mode);
    ReleaseDC(edit, dc);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto it = g_edit_placeholders.find(hwnd);
    if (it == g_edit_placeholders.end() || !it->second.old_proc)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT: {
        const LRESULT result = CallWindowProcW(it->second.old_proc, hwnd, msg, wp, lp);
        draw_edit_placeholder(hwnd, it->second.text);
        sync_edit_cue_caret(hwnd);
        return result;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT: {
        const LRESULT result = CallWindowProcW(it->second.old_proc, hwnd, msg, wp, lp);
        sync_edit_cue_caret(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return result;
    }
    case WM_PASTE: {
        // The add field takes several IDs at once, so keep line breaks from a
        // pasted list instead of letting a single-line edit drop them.
        if (hwnd == g_input && OpenClipboard(hwnd)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (data) {
                const wchar_t* raw = static_cast<const wchar_t*>(GlobalLock(data));
                if (raw) {
                    std::wstring text(raw);
                    GlobalUnlock(data);
                    for (wchar_t& c : text)
                        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                    SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
                    CloseClipboard();
                    return 0;
                }
            }
            CloseClipboard();
        }
        return CallWindowProcW(it->second.old_proc, hwnd, msg, wp, lp);
    }
    case WM_CHAR:
    case WM_LBUTTONUP: {
        const LRESULT result = CallWindowProcW(it->second.old_proc, hwnd, msg, wp, lp);
        sync_edit_cue_caret(hwnd);
        return result;
    }
    case WM_NCDESTROY: {
        const LRESULT result = CallWindowProcW(it->second.old_proc, hwnd, msg, wp, lp);
        g_edit_placeholders.erase(hwnd);
        return result;
    }
    default:
        return CallWindowProcW(it->second.old_proc, hwnd, msg, wp, lp);
    }
}

void set_edit_placeholder(HWND edit, const wchar_t* text) {
    auto it = g_edit_placeholders.find(edit);
    if (it != g_edit_placeholders.end()) {
        it->second.text = text;
        InvalidateRect(edit, nullptr, TRUE);
        return;
    }
    WNDPROC old = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    if (!old) return;
    g_edit_placeholders[edit] = {old, text};
    InvalidateRect(edit, nullptr, TRUE);
}

void add_input() {
    if (g_busy) return;
    std::string id = read_edit(g_input);
    if (id.empty()) {
        set_status("Enter one or more Workshop URLs or IDs first");
        SetFocus(g_input);
        return;
    }
    SetWindowTextW(g_input, L"");
    start_command("add " + id);
}

void update_selected() {
    if (g_busy) return;
    auto ids = selected_ids();
    if (ids.empty()) {
        append_log("Select a mod first.");
        return;
    }
    std::string cmd = "update";
    for (auto id : ids) cmd += " " + std::to_string(id);
    start_command(cmd);
}

void remove_selected() {
    if (g_busy) return;
    auto ids = selected_ids();
    if (ids.empty()) {
        append_log("Select a mod first.");
        return;
    }

    std::wstring msg;
    if (ids.size() == 1) {
        int index = -1;
        SendMessageW(g_list, LB_GETSELITEMS, 1, (LPARAM)&index);
        std::wstring name = (index >= 0 && index < (int)g_row_names.size())
            ? g_row_names[(std::size_t)index] : L"this mod";
        msg = L"Remove this installed mod?\r\n\r\n" + name;
    } else {
        msg = L"Remove " + to_w(std::to_string(ids.size())) + L" selected mods?";
    }
    if (MessageBoxW(g_hwnd, msg.c_str(), L"Remove mods",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    std::string cmd = "remove";
    for (auto id : ids) cmd += " " + std::to_string(id);
    start_command(cmd);
}

void draw_button_icon(HDC hdc, int id, int x, int cy, COLORREF color) {
    const glyphs::Glyph* glyph = nullptr;
    switch (id) {
    case ID_ADD: glyph = &glyphs::kInstall; break;
    case ID_UPDATE: glyph = &glyphs::kUpdate; break;
    case ID_UPDATE_ALL: glyph = &glyphs::kUpdateAll; break;
    case ID_REMOVE: glyph = &glyphs::kRemove; break;
    case ID_SELECT_ALL: glyph = &glyphs::kSelectAll; break;
    case ID_CLEAR: glyph = &glyphs::kClear; break;
    case ID_FILTER_ALL:
    case ID_FILTER_UPDATES: glyph = &glyphs::kFilter; break;
    case ID_SORT: glyph = &glyphs::kSort; break;
    default: return;   // Check intentionally has no glyph.
    }

    HBRUSH brush = CreateSolidBrush(color);
    const int origin_x = x + 1, origin_y = cy - 8;
    glyphs::visit(*glyph, [&](int gx, int gy) {
        RECT pixel{origin_x + gx, origin_y + gy, origin_x + gx + 1, origin_y + gy + 1};
        FillRect(hdc, &pixel, brush);
    });
    DeleteObject(brush);
}

void draw_flat_button(const DRAWITEMSTRUCT* dis, int id, bool primary) {
    if (!dis) return;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool active_chip = (id == ID_FILTER_ALL && !g_only_updates) ||
                             (id == ID_FILTER_UPDATES && g_only_updates);
    RECT r = dis->rcItem;
    HBRUSH fill_brush = g_card_brush;
    HBRUSH border_brush = g_border_brush;
    COLORREF text_color = g_theme.text;
    COLORREF icon_color = g_theme.brand;
    if (disabled) {
        fill_brush = g_bg_brush;
        text_color = g_theme.muted;
        icon_color = g_theme.muted;
    } else if (active_chip) {
        fill_brush = g_brand_soft_brush;
        border_brush = g_brand_brush;
        text_color = g_theme.brand;
        icon_color = g_theme.brand;
    } else if (primary) {
        fill_brush = g_accent_brush;
        border_brush = g_accent_brush;
        text_color = g_theme.on_accent;
        icon_color = g_theme.on_accent;
    } else if (pressed) {
        fill_brush = g_brand_soft_brush;
        border_brush = g_brand_brush;
        icon_color = g_theme.brand;
    }
    FillRect(dis->hDC, &r, fill_brush);
    FrameRect(dis->hDC, &r, border_brush);

    wchar_t label[128]{};
    GetWindowTextW(dis->hwndItem, label, 127);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text_color);
    HGDIOBJ old = SelectObject(dis->hDC, g_font_body);
    SIZE size{};
    GetTextExtentPoint32W(dis->hDC, label, lstrlenW(label), &size);
    const bool show_icon = id != ID_SETTINGS && id != ID_CHECK &&
                           id != ID_SELF_UPDATE;
    const int icon_w = show_icon ? 18 : 0;
    const int gap = show_icon ? 7 : 0;
    const int start = r.left + (r.right - r.left - icon_w - gap - size.cx) / 2;
    if (show_icon)
        draw_button_icon(dis->hDC, id, start, (r.top + r.bottom) / 2, icon_color);
    RECT text_rc{start + icon_w + gap, r.top, r.right - 4, r.bottom};
    DrawTextW(dis->hDC, label, -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

void draw_list_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) return;
    const std::size_t index = dis->itemID;
    if (index >= g_row_names.size()) return;

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    RECT card = dis->rcItem;
    card.bottom = card.top + kRowH;
    FillRect(dis->hDC, &card, selected ? g_selected_brush : g_card_brush);
    FrameRect(dis->hDC, &card, selected ? g_accent_brush : g_border_brush);

    const bool has_update = index < g_row_updates.size() && g_row_updates[index] != 0;
    const bool switchable = !g_activation.empty();
    const bool is_enabled = index < g_row_enabled.size() && g_row_enabled[index] != 0;
    RECT icon_rc{card.left + 10, card.top + 8, card.left + 58, card.top + 56};
    bool drew_icon = false;
    // Decode only the icons that are actually painted, and keep them cached
    // across list refreshes. A large library would otherwise pay a full PNG
    // decode per mod on the UI thread every time the filter changes.
    auto it = g_icons.find(g_row_folders[index]);
    if (it == g_icons.end()) {
        HBITMAP bitmap = nullptr;
        if (index < g_row_icon_paths.size() && !g_row_icon_paths[index].empty())
            bitmap = load_icon_bitmap(fs::path(g_row_icon_paths[index]));
        it = g_icons.emplace(g_row_folders[index], bitmap).first;
    }
    if (it->second) {
        HDC memory = CreateCompatibleDC(dis->hDC);
        HGDIOBJ old = SelectObject(memory, it->second);
        BitBlt(dis->hDC, icon_rc.left, icon_rc.top, 48, 48, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
        drew_icon = true;
    }
    if (!drew_icon) {
        HBRUSH soft = CreateSolidBrush(g_theme.accent_soft);
        FillRect(dis->hDC, &icon_rc, soft);
        DeleteObject(soft);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, g_theme.accent);
        HGDIOBJ old = SelectObject(dis->hDC, g_font_title);
        std::wstring initial = g_row_names[index].substr(0, 1);
        DrawTextW(dis->hDC, initial.c_str(), -1, &icon_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
    }

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_theme.text);
    HGDIOBJ old_font = SelectObject(dis->hDC, g_font_row);
    const int text_right = card.right - (has_update ? (switchable ? 256 : 132)
                                                   : (switchable ? 198 : 50));
    RECT name_rc{card.left + 72, card.top + 7, text_right, card.top + 34};
    DrawTextW(dis->hDC, g_row_names[index].c_str(), -1, &name_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dis->hDC, g_font_small);
    SetTextColor(dis->hDC, g_theme.muted);
    RECT meta_rc{card.left + 72, card.top + 34, text_right, card.top + 57};
    DrawTextW(dis->hDC, g_row_meta[index].c_str(), -1, &meta_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (switchable) {
        RECT pill{card.right - (has_update ? 190 : 124), card.top + 22,
                  card.right - (has_update ? 126 : 60), card.top + 42};
        HBRUSH fill_brush = CreateSolidBrush(is_enabled ? g_theme.success : g_theme.surface);
        FillRect(dis->hDC, &pill, fill_brush);
        DeleteObject(fill_brush);
        if (!is_enabled) FrameRect(dis->hDC, &pill, g_border_brush);
        SetTextColor(dis->hDC, is_enabled ? g_theme.on_accent : g_theme.muted);
        DrawTextW(dis->hDC, is_enabled ? L"ON" : L"OFF", -1, &pill,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (has_update) {
        RECT pill{card.right - 124, card.top + 22, card.right - 60, card.top + 42};
        HBRUSH orange = CreateSolidBrush(g_theme.accent);
        FillRect(dis->hDC, &pill, orange);
        DeleteObject(orange);
        SetTextColor(dis->hDC, g_theme.on_accent);
        DrawTextW(dis->hDC, L"UPDATE", -1, &pill,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(dis->hDC, old_font);
}

// Settings dialog layout, mirroring the X11 panel.
constexpr int kSetClientW = 660, kSetClientH = 470;
constexpr int kSetPanelX = 24, kSetPanelY = 72, kSetPanelW = 612, kSetPanelH = 380;
constexpr int kSetFieldX = 56, kSetFieldW = 548, kSetFieldH = 38;
constexpr int kSetZomboidY = 116, kSetPathY = 190, kSetUserY = 264;
constexpr int kSetCheckY = 310, kSetButtonsY = 396;

// Targets dialog: the same panel as Settings, with a borderless list.
constexpr int kTgtListX = 56, kTgtListY = 104, kTgtListW = 548, kTgtListH = 264;
constexpr int kTgtRowH = 44, kTgtBarX = 615;
constexpr int kTgtStatusY = 374;
constexpr int kTgtButtonsY = 410, kTgtButtonH = 38;

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_settings_wnd = hwnd;
        g_settings_dark = g_mm->config().dark_mode;
        apply_dark_title_bar(hwnd, g_settings_dark);
        {
            // Only the three text fields stay native (input, IME, clipboard).
            // They have no border: WM_PAINT frames them so the frame follows
            // the palette and the keyboard focus, as in the X11 dialog.
            auto make_field = [&](int y) {
                HWND edit = CreateWindowW(L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                    kSetFieldX, y, kSetFieldW, kSetFieldH, hwnd, nullptr, nullptr,
                    nullptr);
                set_font(edit, g_font_body);
                SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                             MAKELPARAM(12, 12));
                apply_dark_control_theme(edit, g_settings_dark);
                return edit;
            };
            g_set_zomboid = make_field(kSetZomboidY);
            g_set_path = make_field(kSetPathY);
            g_set_user = make_field(kSetUserY);

            g_set_auto = CreateWindowW(L"BUTTON",
                L"Enable newly installed mods in mods/default.txt",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                kSetFieldX, kSetCheckY, 470, 24, hwnd, (HMENU)ID_SET_AUTO,
                nullptr, nullptr);
            set_font(g_set_auto, g_font_body);

            HWND save = CreateWindowW(L"BUTTON", L"Save",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, kSetFieldX, kSetButtonsY,
                120, 38, hwnd, (HMENU)ID_SET_SAVE, nullptr, nullptr);
            set_font(save, g_font_body);
            HWND cancel = CreateWindowW(L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, kSetFieldX + 136,
                kSetButtonsY, 120, 38, hwnd, (HMENU)ID_SET_CANCEL, nullptr,
                nullptr);
            set_font(cancel, g_font_body);
            HWND theme = CreateWindowW(L"BUTTON",
                g_settings_dark ? L"Theme: Dark" : L"Theme: Light",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, kSetFieldX + 280,
                kSetButtonsY, 190, 38, hwnd, (HMENU)ID_SET_THEME, nullptr,
                nullptr);
            set_font(theme, g_font_body);
        }

        SetWindowTextW(g_set_zomboid,
                       to_w(g_mm->config().zomboid_dir.string()).c_str());
        SetWindowTextW(g_set_path, to_w(g_mm->config().steamcmd_exe.string()).c_str());
        SetWindowTextW(g_set_user, to_w(g_mm->config().steam_user).c_str());
        SendMessageW(g_set_auto, BM_SETCHECK,
                     g_mm->config().auto_enable ? BST_CHECKED : BST_UNCHECKED, 0);
        SetFocus(g_set_zomboid);
        return 0;
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_bg_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_bg_brush);
        SetBkMode(dc, TRANSPARENT);

        HGDIOBJ old = SelectObject(dc, g_font_title);
        SetTextColor(dc, g_theme.text);
        RECT heading{24, 12, 620, 44};
        DrawTextW(dc, L"Settings", -1, &heading,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_font_body);
        SetTextColor(dc, g_theme.muted);
        RECT subtitle{24, 44, 630, 64};
        DrawTextW(dc, L"Choose the Zomboid folder and configure steamcmd.", -1,
                  &subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT panel{kSetPanelX, kSetPanelY, kSetPanelX + kSetPanelW,
                   kSetPanelY + kSetPanelH};
        FillRect(dc, &panel, g_card_brush);
        FrameRect(dc, &panel, g_border_brush);

        auto label = [&](int y, const wchar_t* text, const wchar_t* hint,
                         int hint_x) {
            SelectObject(dc, g_font_body);
            SetTextColor(dc, g_theme.text);
            RECT r{kSetFieldX, y, kSetFieldX + 320, y + 20};
            DrawTextW(dc, text, -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (!hint) return;
            SelectObject(dc, g_font_small);
            SetTextColor(dc, g_theme.muted);
            RECT h{kSetFieldX + hint_x, y, kSetFieldX + kSetFieldW, y + 20};
            DrawTextW(dc, hint, -1, &h,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        };
        label(92, L"Zomboid folder",
              L"Absolute path containing mods/ and pzmodmgr/", 150);
        label(166, L"steamcmd executable", L"Blank uses the built-in copy", 210);
        label(240, L"Steam username", nullptr, 0);

        for (HWND edit : {g_set_zomboid, g_set_path, g_set_user}) {
            if (!edit) continue;
            RECT r{};
            GetWindowRect(edit, &r);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&r), 2);
            draw_field_frame(dc, r, GetFocus() == edit);
        }

        SelectObject(dc, g_font_small);
        SetTextColor(dc, g_theme.muted);
        RECT note{kSetFieldX, 336, kSetFieldX + kSetFieldW, 354};
        DrawTextW(dc,
                  L"No password is stored. Run steamcmd manually once so its "
                  L"login is cached.",
                  -1, &note, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT note2{kSetFieldX, 354, kSetFieldX + kSetFieldW, 372};
        DrawTextW(dc, L"Use anonymous for public Workshop items.", -1, &note2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (wp == ID_SET_AUTO) {
            RECT r = dis->rcItem;
            FillRect(dis->hDC, &r, g_card_brush);
            const bool checked =
                SendMessageW(g_set_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            draw_checkbox(dis->hDC, r.left, r.top + (r.bottom - r.top - 18) / 2,
                          checked);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HGDIOBJ old_font = SelectObject(dis->hDC, g_font_body);
            wchar_t label[128]{};
            GetWindowTextW(dis->hwndItem, label, 127);
            RECT text{r.left + 28, r.top, r.right, r.bottom};
            DrawTextW(dis->hDC, label, -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dis->hDC, old_font);
            return TRUE;
        }
        if (wp == ID_SET_SAVE || wp == ID_SET_CANCEL || wp == ID_SET_THEME) {
            draw_dialog_button(dis, wp == ID_SET_SAVE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLOREDIT: {
        SetTextColor((HDC)wp, g_theme.text);
        SetBkColor((HDC)wp, g_theme.field);
        return (LRESULT)g_field_brush;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            RECT r{};
            GetWindowRect(reinterpret_cast<HWND>(lp), &r);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&r), 2);
            InflateRect(&r, 2, 2);
            InvalidateRect(hwnd, &r, FALSE);
            return 0;
        }
        if (id == ID_SET_AUTO && code == BN_CLICKED) {
            // Owner-draw buttons do not toggle themselves.
            const bool checked =
                SendMessageW(g_set_auto, BM_GETCHECK, 0, 0) != BST_CHECKED;
            SendMessageW(g_set_auto, BM_SETCHECK,
                         checked ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(g_set_auto, nullptr, TRUE);
            return 0;
        }
        if (id == ID_SET_THEME && code == BN_CLICKED) {
            // Preview only, like the X11 Theme button; Save persists it.
            g_settings_dark = !g_settings_dark;
            refresh_theme(g_hwnd, g_settings_dark);
            apply_dark_title_bar(hwnd, g_settings_dark);
            for (HWND edit : {g_set_zomboid, g_set_path, g_set_user})
                apply_dark_control_theme(edit, g_settings_dark);
            SetWindowTextW(GetDlgItem(hwnd, ID_SET_THEME),
                           g_settings_dark ? L"Theme: Dark" : L"Theme: Light");
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
        if (id == ID_SET_SAVE) {
            std::string zomboid = read_edit(g_set_zomboid);
            std::string path = read_edit(g_set_path);
            std::string user = read_edit(g_set_user);
            if (user.empty()) {
                MessageBoxW(hwnd, L"Steam username is required.", L"Settings",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            const bool auto_enable =
                SendMessageW(g_set_auto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            auto log = [](const std::string& s) { append_log(s); };
            auto r1 = run_command(*g_mm, "config steamcmd_exe " + path, log);
            auto r2 = run_command(*g_mm, "config steam_user " + user, log);
            auto r3 = run_command(*g_mm, "config zomboid_dir " + zomboid, log);
            auto r4 = run_command(*g_mm, std::string("config dark_mode ") +
                                       (g_settings_dark ? "on" : "off"), log);
            auto r5 = run_command(*g_mm, std::string("config auto_enable ") +
                                       (auto_enable ? "on" : "off"), log);
            if (r1.ok && r2.ok && r3.ok && r4.ok && r5.ok) {
                DestroyWindow(hwnd);
                refresh_theme(g_hwnd, g_mm->config().dark_mode);
                start_watcher(g_mm->config().data_dir);
                g_mm->reload_installed();
                clear_icons();
                refresh_dependency_warning();
                refresh_list();
                append_log("Settings saved");
            } else {
                const auto& failed = !r1.ok ? r1
                    : (!r2.ok ? r2 : (!r3.ok ? r3 : (!r4.ok ? r4 : r5)));
                MessageBoxW(hwnd, to_w(failed.message).c_str(), L"Settings",
                            MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (id == ID_SET_CANCEL) {
            if (g_settings_dark != g_mm->config().dark_mode)
                refresh_theme(g_hwnd, g_mm->config().dark_mode);
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_settings_dark != g_mm->config().dark_mode)
            refresh_theme(g_hwnd, g_mm->config().dark_mode);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_settings_wnd = nullptr;
        g_set_auto = nullptr;
        g_set_zomboid = nullptr;
        g_set_path = nullptr;
        g_set_user = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_settings(HWND owner) {
    if (g_settings_wnd) return;

    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = SettingsProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PZSettingsWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No class brush: WM_ERASEBKGND uses the current palette, so switching the
    // theme while the dialog is open cannot leave a deleted brush behind.
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT wr{0, 0, kSetClientW, kSetClientH};
    AdjustWindowRect(&wr, style, FALSE);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    RECT r{};
    GetWindowRect(owner, &r);
    const int x = r.left + ((r.right - r.left) - w) / 2;
    const int y = r.top + ((r.bottom - r.top) - h) / 2;
    HWND dlg = CreateWindowExW(0, L"PZSettingsWnd", L"Settings",
        style, x, y, w, h, owner, nullptr, hi, nullptr);
    if (!dlg) return;

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    // After the first show, so DWM picks the attribute up on the new frame.
    apply_dark_title_bar(dlg, g_dark_mode);

    MSG msg;
    while (g_settings_wnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetFocus(g_input);
    set_status("Settings saved");
}

std::string target_state(const ActivationTarget& target) {
    if (g_target_mod_ids.empty()) return "no Mod IDs";
    const int count = activation_count(target, g_target_mod_ids);
    if (count == 0) return "off";
    if (count == static_cast<int>(g_target_mod_ids.size())) return "on";
    return std::to_string(count) + " of " +
           std::to_string(g_target_mod_ids.size()) + " on";
}

void draw_target_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) return;
    const std::size_t index = dis->itemID;
    if (index >= g_targets.size()) return;
    const auto& target = g_targets[index];

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    RECT row = dis->rcItem;
    FillRect(dis->hDC, &row, selected ? g_selected_brush : g_card_brush);
    FrameRect(dis->hDC, &row, selected ? g_accent_brush : g_border_brush);

    RECT icon_rc{row.left + 8, row.top + 6, row.left + 44, row.top + 42};
    HBITMAP bitmap = target_icon_bitmap(target);
    if (bitmap) {
        HDC memory = CreateCompatibleDC(dis->hDC);
        HGDIOBJ old = SelectObject(memory, bitmap);
        BitBlt(dis->hDC, icon_rc.left, icon_rc.top, 36, 36, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
    } else {
        HBRUSH soft = CreateSolidBrush(g_theme.accent_soft);
        FillRect(dis->hDC, &icon_rc, soft);
        DeleteObject(soft);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, g_theme.accent);
        HGDIOBJ old = SelectObject(dis->hDC, g_font_small);
        const wchar_t* badge = target.kind == ActivationKind::Default ? L"DEF"
            : target.kind == ActivationKind::Save ? L"SAV" : L"SRV";
        DrawTextW(dis->hDC, badge, -1, &icon_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
    }

    const std::string state_text = target_state(target);
    const std::wstring state = to_w(state_text);
    SetBkMode(dis->hDC, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dis->hDC, g_font_small);
    SIZE state_size{};
    GetTextExtentPoint32W(dis->hDC, state.c_str(), (int)state.size(), &state_size);
    const int state_right = row.right - 12;
    RECT state_rc{state_right - state_size.cx, row.top, state_right, row.bottom};
    SelectObject(dis->hDC, g_font_body);
    RECT label_rc{icon_rc.right + 10, row.top, state_rc.left - 12, row.bottom};
    SetTextColor(dis->hDC, g_theme.text);
    DrawTextW(dis->hDC, to_w(target.label).c_str(), -1, &label_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dis->hDC, g_font_small);
    SetTextColor(dis->hDC, (state_text == "off" || state_text == "no Mod IDs")
                                ? g_theme.muted : g_theme.success);
    DrawTextW(dis->hDC, state.c_str(), -1, &state_rc,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old_font);
}

void update_target_copy_label() {
    if (!g_tgt_copy) return;
    const int sel = (int)SendMessageW(g_tgt_list, LB_GETCURSEL, 0, 0);
    const bool server = sel >= 0 && sel < (int)g_targets.size() &&
                        g_targets[(std::size_t)sel].kind == ActivationKind::Server;
    SetWindowTextW(g_tgt_copy, server ? L"Copy server IDs" : L"Copy IDs");
}

void fill_target_list() {
    SendMessageW(g_tgt_list, LB_RESETCONTENT, 0, 0);
    const int keep = (int)SendMessageW(g_tgt_list, LB_GETCURSEL, 0, 0);
    for (const auto& target : g_targets) {
        const std::wstring line = to_w(target.label + "   [" + target_state(target) + "]");
        SendMessageW(g_tgt_list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (!g_targets.empty())
        SendMessageW(g_tgt_list, LB_SETCURSEL,
                     keep >= 0 && keep < (int)g_targets.size() ? keep : 0, 0);
    if (g_tgt_status) {
        const auto names = selected_row_names();
        std::string text;
        if (g_targets.empty()) {
            text = "No mods/default.txt, save, or server was found.";
        } else if (names.empty()) {
            text = "Choose a target. Copy server IDs gives a paste-ready list for "
                   "Add on another pzmodmgr.";
        } else {
            text = "Selected: ";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) text += ", ";
                text += to_u8(names[i]);
            }
            text += ". Enable or disable them in the highlighted target.";
        }
        SetWindowTextW(g_tgt_status, to_w(text).c_str());
    }
    update_target_copy_label();
    sync_bars();
    if (g_targets_wnd) {
        const int selection = (int)SendMessageW(g_tgt_list, LB_GETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(g_targets_wnd, ID_TGT_ENABLE), !g_target_mod_ids.empty());
        EnableWindow(GetDlgItem(g_targets_wnd, ID_TGT_DISABLE), !g_target_mod_ids.empty());
        EnableWindow(GetDlgItem(g_targets_wnd, ID_TGT_COPY_NAMES),
                     selection >= 0 && selection < (int)g_targets.size() &&
                         !g_targets[(std::size_t)selection].mod_ids.empty());
    }
}

std::string target_command(int sel, bool enabled) {
    std::string line = "set-enabled " + std::to_string(sel) +
                       (enabled ? " on" : " off");
    for (auto id : g_selected_ids) line += " w:" + std::to_string(id);
    for (const auto& folder : g_selected_folders)
        line += " " + folder_token(folder);
    return line;
}

LRESULT CALLBACK TargetsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_targets_wnd = hwnd;
        apply_dark_title_bar(hwnd, g_dark_mode);
        {
            g_tgt_list = CreateWindowW(L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                    LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                kTgtListX, kTgtListY, kTgtListW, kTgtListH, hwnd,
                (HMENU)ID_TGT_LIST, nullptr, nullptr);
            set_font(g_tgt_list, g_font_body);
            SendMessageW(g_tgt_list, LB_SETITEMHEIGHT, 0, kTgtRowH);
            g_tgt_list_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                g_tgt_list, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(TargetsListProc)));
            create_scrollbar(hwnd, g_tgt_list, false, true, kTgtBarX, kTgtListY,
                             kScrollWinW, kTgtListH);

            g_tgt_status = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_NOPREFIX, kTgtListX, kTgtStatusY,
                kTgtListW, 32, hwnd, nullptr, nullptr, nullptr);
            set_font(g_tgt_status, g_font_small);

            struct DialogButton {
                const wchar_t* label;
                int id;
                int x;
                int w;
            };
            const DialogButton buttons[] = {
                {L"Enable", ID_TGT_ENABLE, 56, 98},
                {L"Disable", ID_TGT_DISABLE, 160, 98},
                {L"Copy IDs", ID_TGT_COPY, 264, 118},
                {L"Copy names", ID_TGT_COPY_NAMES, 390, 98},
                {L"Close", ID_TGT_CLOSE, 496, 98},
            };
            for (const auto& spec : buttons) {
                HWND button = CreateWindowW(L"BUTTON", spec.label,
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, spec.x, kTgtButtonsY,
                    spec.w, kTgtButtonH, hwnd, (HMENU)(INT_PTR)spec.id, nullptr,
                    nullptr);
                set_font(button, g_font_body);
                if (spec.id == ID_TGT_COPY) g_tgt_copy = button;
                if (spec.id == ID_TGT_COPY_NAMES) g_tgt_copy_names = button;
            }
        }
        fill_target_list();
        SetFocus(g_tgt_list);
        return 0;
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_bg_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_bg_brush);
        SetBkMode(dc, TRANSPARENT);

        HGDIOBJ old = SelectObject(dc, g_font_title);
        SetTextColor(dc, g_theme.text);
        RECT heading{24, 12, 620, 44};
        DrawTextW(dc, L"Enable mods", -1, &heading,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_font_body);
        SetTextColor(dc, g_theme.muted);
        RECT subtitle{24, 44, 630, 64};
        DrawTextW(dc,
                  L"Choose a Mods list, save, or server for the selected mods.",
                  -1, &subtitle,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT panel{kSetPanelX, kSetPanelY, kSetPanelX + kSetPanelW,
                   kSetPanelY + kSetPanelH};
        FillRect(dc, &panel, g_card_brush);
        FrameRect(dc, &panel, g_border_brush);

        // The list has no border of its own, so the frame is painted here and
        // follows the palette.
        RECT list{kTgtListX - 1, kTgtListY - 1, kTgtListX + kTgtListW + 1,
                  kTgtListY + kTgtListH + 1};
        FrameRect(dc, &list, g_border_brush);

        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORLISTBOX: {
        SetBkMode((HDC)wp, OPAQUE);
        SetBkColor((HDC)wp, g_theme.surface);
        SetTextColor((HDC)wp, g_theme.text);
        return (LRESULT)g_card_brush;
    }
    case WM_CTLCOLORSTATIC: {
        SetBkMode((HDC)wp, OPAQUE);
        SetBkColor((HDC)wp, g_theme.surface);
        SetTextColor((HDC)wp, g_theme.muted);
        return (LRESULT)g_card_brush;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (wp == ID_TGT_LIST) {
            draw_target_item(dis);
            return TRUE;
        }
        if (wp == ID_TGT_ENABLE || wp == ID_TGT_DISABLE || wp == ID_TGT_COPY ||
            wp == ID_TGT_COPY_NAMES || wp == ID_TGT_CLOSE) {
            draw_dialog_button(dis, wp == ID_TGT_ENABLE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == ID_TGT_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        const int sel = (int)SendMessageW(g_tgt_list, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)g_targets.size()) return 0;
        if (id == ID_TGT_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            update_target_copy_label();
            return 0;
        }
        if (id == ID_TGT_ENABLE || id == ID_TGT_DISABLE) {
            const std::string line = target_command(sel, id == ID_TGT_ENABLE);
            auto log = [](const std::string& s) { append_log(s); };
            auto result = run_command(*g_mm, line, log);
            append_log(std::string(result.ok ? "OK: " : "ERROR: ") + result.message);
            if (result.ok) {
                g_targets = g_mm->activation_targets();
                fill_target_list();
                refresh_list();
            }
        } else if (id == ID_TGT_COPY) {
            const auto& target = g_targets[(std::size_t)sel];
            const auto ids = g_mm->workshop_ids_for(target);
            if (ids.empty()) {
                append_log("No Workshop IDs found for " + activation_name(target) +
                           ". Use Copy names so the list can still be shared.");
            } else if (copy_workshop_ids(ids)) {
                std::string message = "Copied " + std::to_string(ids.size()) +
                    " Workshop ID(s) from " + activation_name(target) +
                    ". Paste the list into Add on the other pzmodmgr.";
                const auto missing = g_mm->unmapped_mod_ids(target);
                if (!missing.empty())
                    message += " " + std::to_string(missing.size()) +
                               " enabled Mod ID(s) have no installed Workshop ID; use Copy names.";
                append_log(message);
            } else {
                append_log("Could not write to the clipboard.");
            }
        } else if (id == ID_TGT_COPY_NAMES) {
            const auto& target = g_targets[(std::size_t)sel];
            const auto names = g_mm->enabled_mod_names(target);
            std::string text;
            for (const auto& name : names) {
                if (!text.empty()) text += '\n';
                text += name;
            }
            if (text.empty()) {
                append_log("No enabled Mod names found for " + activation_name(target) + ".");
            } else if (copy_plain_text(text)) {
                append_log("Copied " + std::to_string(names.size()) +
                           " enabled Mod name(s) from " + activation_name(target) + ".");
            } else {
                append_log("Could not write to the clipboard.");
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        clear_target_icons();
        g_targets_wnd = nullptr;
        g_tgt_list = nullptr;
        g_tgt_status = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void show_targets(HWND owner, bool prefer_server = false, bool allow_empty = false) {
    if (g_targets_wnd) return;
    if (!allow_empty && g_selected_ids.empty() && g_selected_folders.empty()) return;

    g_target_mod_ids.clear();
    for (std::size_t i = 0; i < g_row_ids.size() && i < g_row_mod_ids.size(); ++i) {
        const bool selected = g_row_ids[i] != 0
            ? std::find(g_selected_ids.begin(), g_selected_ids.end(), g_row_ids[i]) !=
                  g_selected_ids.end()
            : std::find(g_selected_folders.begin(), g_selected_folders.end(),
                        g_row_folders[i]) != g_selected_folders.end();
        if (!selected) continue;
        for (const auto& id : g_row_mod_ids[i]) {
            if (std::find(g_target_mod_ids.begin(), g_target_mod_ids.end(), id) ==
                g_target_mod_ids.end())
                g_target_mod_ids.push_back(id);
        }
    }
    if (g_target_mod_ids.empty() && !allow_empty) {
        append_log("No Mod ID found for the selected mod(s).");
        return;
    }

    g_targets = g_mm->activation_targets();
    if (prefer_server) {
        for (std::size_t i = 0; i < g_targets.size(); ++i)
            if (g_targets[i].kind == ActivationKind::Server) {
                // fill_target_list preserves this selection on creation.
                g_targets.insert(g_targets.begin(), g_targets[i]);
                g_targets.erase(g_targets.begin() + static_cast<std::ptrdiff_t>(i + 1));
                break;
            }
    }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = TargetsProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PZTargetsWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Null class brush: the dialog erases itself from the live palette.
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT wr{0, 0, kSetClientW, kSetClientH};
    AdjustWindowRect(&wr, style, FALSE);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    RECT r{};
    GetWindowRect(owner, &r);
    const int x = r.left + ((r.right - r.left) - w) / 2;
    const int y = r.top + ((r.bottom - r.top) - h) / 2;
    HWND dlg = CreateWindowExW(0, L"PZTargetsWnd",
        L"Enable mods", style, x, y, w, h, owner, nullptr, hi, nullptr);
    if (!dlg) return;

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    apply_dark_title_bar(dlg, g_dark_mode);

    MSG msg;
    while (g_targets_wnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetFocus(g_input);
    refresh_list();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        {
            g_brand_mark = CreateWindowW(L"STATIC", L"PZ",
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
                24, 20, 36, 36, hwnd, nullptr, nullptr, nullptr);
            set_font(g_brand_mark, g_font_brand);
            g_brand_line = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE, 32, 49, 20, 3, hwnd, nullptr, nullptr, nullptr);
            g_title = CreateWindowW(L"STATIC", L"PZ Mod Manager",
                WS_CHILD | WS_VISIBLE, 72, 12, 600, 38, hwnd, nullptr, nullptr, nullptr);
            set_font(g_title, g_font_title);
            g_subtitle = CreateWindowW(L"STATIC", L"Project Zomboid Workshop installer",
                WS_CHILD | WS_VISIBLE, 72, 51, 600, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(g_subtitle, g_font_small);
            g_share_server_btn = CreateWindowW(L"BUTTON", L"Share server",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 560, 24, 116, 30,
                hwnd, (HMENU)ID_SHARE_SERVER, nullptr, nullptr);
            set_font(g_share_server_btn, g_font_body);
            g_enable_btn = CreateWindowW(L"BUTTON", L"Enable...",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 688, 24, 100, 30,
                hwnd, (HMENU)ID_ENABLE, nullptr, nullptr);
            set_font(g_enable_btn, g_font_body);
            g_self_update = CreateWindowW(L"BUTTON", L"Update app",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 440, 24, 108, 30, hwnd,
                (HMENU)ID_SELF_UPDATE, nullptr, nullptr);
            set_font(g_self_update, g_font_body);
            g_settings_btn = CreateWindowW(L"BUTTON", L"Settings",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 800, 24, 96, 30, hwnd,
                (HMENU)ID_SETTINGS, nullptr, nullptr);
            set_font(g_settings_btn, g_font_body);

            g_input = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 24, 84, 644, 38,
                hwnd, (HMENU)ID_INPUT, nullptr, nullptr);
            set_font(g_input, g_font_body);
            set_edit_placeholder(g_input, L"Paste Workshop URLs or IDs");
            g_add = CreateWindowW(L"BUTTON", L"Install",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 684, 84, 212, 38, hwnd,
                (HMENU)ID_ADD, nullptr, nullptr);
            set_font(g_add, g_font_body);

            HWND label = CreateWindowW(L"STATIC",
                L"Paste Workshop URLs or IDs separated by spaces. Press Enter to install; collections are supported.",
                WS_CHILD | WS_VISIBLE, 24, 132, 872, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(label, g_font_small);

            g_filter = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                24, 158, 280, 24, hwnd, (HMENU)ID_FILTER, nullptr, nullptr);
            set_font(g_filter, g_font_small);
            set_edit_placeholder(g_filter, L"Filter name or folder");
            g_filter_all = CreateWindowW(L"BUTTON", L"All",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 312, 158, 60, 24, hwnd,
                (HMENU)ID_FILTER_ALL, nullptr, nullptr);
            g_filter_updates = CreateWindowW(L"BUTTON", L"Updates",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 378, 158, 112, 24, hwnd,
                (HMENU)ID_FILTER_UPDATES, nullptr, nullptr);
            g_check = CreateWindowW(L"BUTTON", L"Check updates",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 496, 158, 120, 24, hwnd,
                (HMENU)ID_CHECK, nullptr, nullptr);
            g_sort = CreateWindowW(L"BUTTON", L"Sort: Name",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 626, 158, 140, 24, hwnd,
                (HMENU)ID_SORT, nullptr, nullptr);
            for (HWND b : {g_filter_all, g_filter_updates, g_check, g_sort})
                set_font(b, g_font_small);
            g_count = CreateWindowW(L"STATIC", L"0 mods",
                WS_CHILD | WS_VISIBLE | SS_RIGHT, 776, 158, 120, 18, hwnd, nullptr,
                nullptr, nullptr);
            set_font(g_count, g_font_small);

            g_list = CreateWindowW(L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LBS_NOTIFY |
                    LBS_EXTENDEDSEL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                    LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                24, 192, 856, 344, hwnd, (HMENU)ID_LIST, nullptr, nullptr);
            g_list_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                g_list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListProc)));
            SendMessageW(g_list, LB_SETITEMHEIGHT, 0, 68);
            create_scrollbar(hwnd, g_list, false, false, 882, 192, kScrollWinW, 344);
            g_empty = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_CLIPSIBLINGS | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
                24, 192, 856, 68, hwnd, nullptr, nullptr, nullptr);
            set_font(g_empty, g_font_body);

            g_select_all = CreateWindowW(L"BUTTON", L"Select all",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 24, 554, 116, 32, hwnd,
                (HMENU)ID_SELECT_ALL, nullptr, nullptr);
            g_clear = CreateWindowW(L"BUTTON", L"Deselect",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 148, 554, 116, 32, hwnd,
                (HMENU)ID_CLEAR, nullptr, nullptr);
            g_copy = CreateWindowW(L"BUTTON", L"Copy IDs",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 392, 554, 104, 32,
                hwnd, (HMENU)ID_COPY, nullptr, nullptr);
            g_update = CreateWindowW(L"BUTTON", L"Update",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 506, 554, 120, 32,
                hwnd, (HMENU)ID_UPDATE, nullptr, nullptr);
            g_update_all = CreateWindowW(L"BUTTON", L"Update all",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 636, 554, 120, 32, hwnd,
                (HMENU)ID_UPDATE_ALL, nullptr, nullptr);
            g_remove = CreateWindowW(L"BUTTON", L"Remove",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 766, 554, 130, 32,
                hwnd, (HMENU)ID_REMOVE, nullptr, nullptr);
            for (HWND b : {g_select_all, g_clear, g_copy, g_update, g_update_all, g_remove})
                set_font(b, g_font_body);

            g_details = CreateWindowW(L"STATIC", L"No selection",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
                24, 594, 872, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(g_details, g_font_small);
            g_activity_label = CreateWindowW(L"STATIC", L"ACTIVITY",
                WS_CHILD | WS_VISIBLE, 24, 616, 200, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(g_activity_label, g_font_small);
            // No word wrap: EM_GETLINECOUNT then matches what the scrollbar
            // maps, which keeps the thumb honest on long steamcmd lines.
            g_log = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                25, 635, 856, 60, hwnd, (HMENU)ID_LOG, nullptr, nullptr);
            set_font(g_log, g_font_small);
            g_log_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                g_log, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(LogProc)));
            create_scrollbar(hwnd, g_log, true, true, 882, 635, kScrollWinW, 60);
            g_status = CreateWindowW(L"STATIC", L"Ready",
                WS_CHILD | WS_VISIBLE, 24, 702, 500, 18, hwnd, nullptr, nullptr,
                nullptr);
            set_font(g_status, g_font_small);
            append_log("Paste Workshop URLs or IDs, then press Enter.");
            append_log("Click a mod to select it; Ctrl/Shift-click for more.");
            refresh_dependency_warning();
            refresh_list();
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_bg_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_bg_brush);
        // Card behind the activity log; the label and the edit sit on top of
        // it, so only the frame and the padding show.
        RECT card{24, 610, 896, 698};
        FillRect(dc, &card, g_card_brush);
        FrameRect(dc, &card, g_border_brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN: {
        SetBkMode((HDC)wp, TRANSPARENT);
        SetBkColor((HDC)wp, g_theme.bg);
        SetTextColor((HDC)wp, g_theme.text);
        return (LRESULT)g_bg_brush;
    }
    case WM_CTLCOLORSTATIC: {
        if ((HWND)lp == g_brand_mark) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_theme.brand);
            SetTextColor((HDC)wp, g_theme.cream);
            return (LRESULT)g_brand_brush;
        }
        if ((HWND)lp == g_brand_line) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_theme.accent);
            return (LRESULT)g_accent_brush;
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == g_log) {
            SetBkColor((HDC)wp, g_theme.surface);
            SetTextColor((HDC)wp, g_theme.text);
            return (LRESULT)g_card_brush;
        }
        if ((HWND)lp == g_empty) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_theme.surface);
            SetTextColor((HDC)wp, g_theme.muted);
            return (LRESULT)g_card_brush;
        }
        if ((HWND)lp == g_activity_label) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_theme.surface);
            SetTextColor((HDC)wp, g_theme.brand);
            return (LRESULT)g_card_brush;
        } else if ((HWND)lp == g_details) {
            SetTextColor((HDC)wp, g_theme.muted);
        } else if ((HWND)lp == g_status) {
            SetTextColor((HDC)wp, g_busy ? g_theme.accent
                                         : (g_dependency_warning.empty()
                                                ? g_theme.brand
                                                : g_theme.danger));
        } else {
            SetTextColor((HDC)wp, g_theme.text);
        }
        return (LRESULT)g_bg_brush;
    }
    case WM_CTLCOLOREDIT: {
        SetTextColor((HDC)wp, g_theme.text);
        SetBkColor((HDC)wp, g_theme.field);
        return (LRESULT)g_field_brush;
    }
    case WM_CTLCOLORLISTBOX: {
        SetTextColor((HDC)wp, g_theme.text);
        SetBkColor((HDC)wp, g_theme.bg);
        return (LRESULT)g_bg_brush;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (wp == ID_LIST) {
            draw_list_item(dis);
            return TRUE;
        }
        if (wp == ID_ADD || wp == ID_UPDATE || wp == ID_REMOVE ||
            wp == ID_UPDATE_ALL || wp == ID_SELECT_ALL || wp == ID_CLEAR ||
            wp == ID_COPY || wp == ID_SETTINGS || wp == ID_ENABLE ||
            wp == ID_SHARE_SERVER || wp == ID_FILTER_ALL ||
            wp == ID_FILTER_UPDATES || wp == ID_SORT || wp == ID_CHECK ||
            wp == ID_SELF_UPDATE) {
            draw_flat_button(dis, (int)wp, wp == ID_ADD);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ADD:
            add_input();
            return 0;
        case ID_UPDATE:
            update_selected();
            return 0;
        case ID_REMOVE:
            remove_selected();
            return 0;
        case ID_UPDATE_ALL:
            start_command("update");
            return 0;
        case ID_SETTINGS:
            show_settings(hwnd);
            return 0;
        case ID_ENABLE:
            show_targets(hwnd);
            return 0;
        case ID_SHARE_SERVER:
            show_targets(hwnd, true, true);
            return 0;
        case ID_SELECT_ALL:
            select_all();
            return 0;
        case ID_CLEAR:
            clear_selection();
            return 0;
        case ID_COPY:
            copy_selected_ids();
            return 0;
        case ID_INPUT:
            if (HIWORD(wp) == EN_CHANGE) InvalidateRect(g_input, nullptr, TRUE);
            return 0;
        case ID_FILTER:
            if (HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(g_filter, nullptr, TRUE);
                refresh_list();
            }
            return 0;
        case ID_FILTER_ALL:
            g_only_updates = false;
            refresh_list();
            return 0;
        case ID_FILTER_UPDATES:
            g_only_updates = true;
            refresh_list();
            return 0;
        case ID_SORT:
            g_sort_mode = 1 - g_sort_mode;
            refresh_list();
            return 0;
        case ID_CHECK:
            start_command("check");
            return 0;
        case ID_SELF_UPDATE:
            start_command("self-update");
            return 0;
        case ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) sync_selection();
            else if (HIWORD(wp) == LBN_DBLCLK) update_selected();
            return 0;
        default:
            return 0;
        }
    case WM_APP_LOG:
        drain_pending();
        return 0;
    case WM_APP_RELOAD:
        if (g_busy) {
            g_reload_pending = true;
        } else if (g_mm && g_mm->reload_installed()) {
            clear_icons();
            refresh_dependency_warning();
            refresh_list();
        }
        return 0;
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        drain_pending();
        g_busy = false;
        if (g_reload_pending) {
            g_reload_pending = false;
            if (g_mm) g_mm->reload_installed();
        }
        EnableWindow(g_input, TRUE);
        EnableWindow(g_list, TRUE);
        refresh_dependency_warning();
        clear_icons();
        refresh_list();
        SetFocus(g_input);
        return 0;
    case WM_TIMER:
        if (wp == kAutoCheckTimer) {
            // Do not start a check while the modal Settings dialog can still
            // change the database on the UI thread. Re-arm until it closes.
            if (g_settings_wnd) {
                SetTimer(hwnd, kAutoCheckTimer, 300, nullptr);
                return 0;
            }
            KillTimer(hwnd, kAutoCheckTimer);
            if (!g_busy && g_mm && g_mm->update_check_due()) start_command("check");
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_busy) {
            append_log("A command is still running; wait for it before closing.");
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_worker.joinable()) g_worker.join();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

int run_ui(ModManager& mm) {
    g_mm = &mm;
    g_busy = false;
    g_reload_pending = false;
    g_only_updates = false;
    g_sort_mode = 0;
    g_selected_ids.clear();
    g_selected_folders.clear();
    g_row_ids.clear();
    g_row_names.clear();
    g_row_meta.clear();
    g_row_folders.clear();
    g_row_updates.clear();
    g_row_icon_paths.clear();
    g_row_mod_ids.clear();
    g_row_enabled.clear();
    g_activation.clear();
    g_dependency_warning.clear();
    g_dependency_signature.clear();
    clear_icons();

    SetProcessDPIAware();
    g_font_title = make_font(15, FW_SEMIBOLD);
    g_font_row = make_font(10, FW_SEMIBOLD);
    g_font_brand = make_font(12, FW_BOLD);
    g_font_body = make_font(10, FW_NORMAL);
    g_font_small = make_font(8, FW_NORMAL);
    refresh_theme(nullptr, mm.config().dark_mode);

    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PZModMgrWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Null class brush; WM_ERASEBKGND paints the current palette.
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    register_scrollbar_class();

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                        WS_CLIPCHILDREN;
    RECT wr{0, 0, kWindowW, kWindowH};
    AdjustWindowRect(&wr, style, FALSE);
    HWND w = CreateWindowExW(0, L"PZModMgrWnd", L"PZ Mod Manager", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left,
                             wr.bottom - wr.top, nullptr, nullptr, hi, nullptr);
    if (!w) return 1;
    apply_dark_title_bar(w, g_dark_mode);
    ShowWindow(w, SW_SHOW);
    start_watcher(mm.config().data_dir);
    // Close the gap between the initial load and the watch starting.
    if (mm.reload_installed()) refresh_list();
    SetFocus(g_input);
    // Give the first paint a moment, then run the daily check without blocking
    // the UI thread. The timer fires once per launch only.
    if (mm.update_check_due()) SetTimer(w, kAutoCheckTimer, 300, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && GetFocus() == g_input &&
            !g_busy) {
            add_input();
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && GetFocus() == g_filter) {
            SetWindowTextW(g_filter, L"");
            refresh_list();
            continue;
        }
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    stop_watcher();
    clear_icons();
    clear_target_icons();
    g_edit_placeholders.clear();
    g_settings_wnd = nullptr;
    for (HWND b : {g_brand_mark, g_brand_line, g_title, g_subtitle, g_input, g_add,
                   g_list, g_empty, g_log, g_update, g_remove, g_update_all, g_settings_btn,
                   g_self_update, g_select_all, g_clear, g_copy, g_details, g_status, g_count,
                   g_activity_label}) {
        (void)b;
    }
    destroy_theme_brushes();
    DeleteObject(g_font_small);
    DeleteObject(g_font_body);
    DeleteObject(g_font_brand);
    DeleteObject(g_font_row);
    DeleteObject(g_font_title);
    return (int)m.wParam;
}
}  // namespace pz
