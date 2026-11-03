#include "ui/ui.hpp"
#include "ui/mod_icon.hpp"
#include "core/commands.hpp"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <exception>
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
constexpr int ID_SORT = 114, ID_CHECK = 115;
constexpr int ID_SET_SAVE = 201, ID_SET_CANCEL = 202;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr int kWindowW = 1000, kWindowH = 720;
constexpr int kRowH = 64;

ModManager* g_mm = nullptr;
HWND g_hwnd = nullptr;
HWND g_title = nullptr;
HWND g_subtitle = nullptr;
HWND g_accent_bar = nullptr;
HWND g_input = nullptr;
HWND g_add = nullptr;
HWND g_filter = nullptr;
HWND g_filter_all = nullptr;
HWND g_filter_updates = nullptr;
HWND g_sort = nullptr;
HWND g_check = nullptr;
HWND g_list = nullptr;
HWND g_log = nullptr;
HWND g_update = nullptr;
HWND g_remove = nullptr;
HWND g_update_all = nullptr;
HWND g_settings_btn = nullptr;
HWND g_select_all = nullptr;
HWND g_clear = nullptr;
HWND g_details = nullptr;
HWND g_status = nullptr;
HWND g_count = nullptr;

HWND g_settings_wnd = nullptr;
HWND g_set_path = nullptr;
HWND g_set_user = nullptr;

HFONT g_font_title = nullptr;
HFONT g_font_body = nullptr;
HFONT g_font_small = nullptr;
HBRUSH g_bg_brush = nullptr;
HBRUSH g_card_brush = nullptr;
HBRUSH g_selected_brush = nullptr;
HBRUSH g_border_brush = nullptr;
HBRUSH g_accent_brush = nullptr;

std::thread g_worker;
std::mutex g_pending_mu;
std::deque<std::string> g_pending;
std::vector<std::wstring> g_row_names;
std::vector<std::wstring> g_row_meta;
std::vector<std::string> g_row_folders;
std::vector<std::uint64_t> g_row_ids;
std::vector<char> g_row_updates;
std::vector<std::uint64_t> g_selected_ids;
bool g_only_updates = false;
int g_sort_mode = 0;
std::map<std::string, HBITMAP> g_icons;
bool g_busy = false;

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

std::string lowercase(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
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
}

void set_status(const std::string& s) {
    SetWindowTextW(g_status, to_w(s).c_str());
}

std::string read_edit(HWND edit);

void update_actions() {
    const bool has = !g_selected_ids.empty();
    EnableWindow(g_add, !g_busy);
    EnableWindow(g_update_all, !g_busy);
    EnableWindow(g_settings_btn, !g_busy);
    EnableWindow(g_select_all, !g_busy && !g_row_ids.empty());
    EnableWindow(g_clear, !g_busy && has);
    EnableWindow(g_update, !g_busy && has);
    EnableWindow(g_remove, !g_busy && has);
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
        if (sel >= 0 && sel < (int)g_row_ids.size()) ids.push_back(g_row_ids[(std::size_t)sel]);
        return ids;
    }
    if (count == 0) return ids;

    std::vector<int> rows((std::size_t)count);
    int got = (int)SendMessageW(g_list, LB_GETSELITEMS, count, (LPARAM)rows.data());
    for (int i = 0; i < got; ++i) {
        int index = rows[(std::size_t)i];
        if (index < 0 || index >= (int)g_row_ids.size()) continue;
        auto id = g_row_ids[(std::size_t)index];
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    return ids;
}

void set_details() {
    if (g_selected_ids.empty()) {
        SetWindowTextW(g_details, L"No selection");
        return;
    }
    if (g_selected_ids.size() > 1) {
        SetWindowTextW(g_details,
            (to_w(std::to_string(g_selected_ids.size())) + L" selected").c_str());
        return;
    }
    int index = -1;
    SendMessageW(g_list, LB_GETSELITEMS, 1, (LPARAM)&index);
    std::wstring name = (index >= 0 && index < (int)g_row_names.size())
        ? g_row_names[(std::size_t)index] : L"1 selected";
    SetWindowTextW(g_details, (L"Selected: " + name).c_str());
}

HBITMAP load_icon_bitmap(const fs::path& path) {
    ModIcon icon;
    if (path.empty() || !load_mod_icon(path, 48, icon)) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 48;
    bmi.bmiHeader.biHeight = -48;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return nullptr;

    auto* pixels = static_cast<std::uint8_t*>(bits);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * 48 + x) * 4;
            const std::size_t dst = src;
            const int a = icon.rgba[src + 3];
            pixels[dst + 0] = static_cast<std::uint8_t>(
                (icon.rgba[src + 2] * a + 255 * (255 - a)) / 255);
            pixels[dst + 1] = static_cast<std::uint8_t>(
                (icon.rgba[src + 1] * a + 255 * (255 - a)) / 255);
            pixels[dst + 2] = static_cast<std::uint8_t>(
                (icon.rgba[src + 0] * a + 255 * (255 - a)) / 255);
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
}

void refresh_list() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_row_ids.clear();
    g_row_names.clear();
    g_row_meta.clear();
    g_row_folders.clear();
    g_row_updates.clear();
    clear_icons();

    const std::string needle = lowercase(read_edit(g_filter));
    auto visible = g_mm->list();
    visible.erase(std::remove_if(visible.begin(), visible.end(),
        [&](const InstalledMod& m) {
            if (g_only_updates && !g_mm->update_available(m.workshop_id)) return true;
            if (needle.empty()) return false;
            std::string haystack = m.name + " " + m.folder;
            if (const auto info = g_mm->info_for(m.folder))
                haystack += " " + info->author + " " + info->mod_version;
            return lowercase(haystack).find(needle) == std::string::npos;
        }), visible.end());

    if (g_sort_mode == 0) {
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

    for (const auto& m : visible) {
        const auto info = g_mm->info_for(m.folder);
        const std::string title = (info && !info->name.empty()) ? info->name
            : (m.name.empty() ? m.folder : m.name);
        std::string meta;
        if (info) {
            if (!info->author.empty()) meta += info->author + "  |  ";
            if (!info->mod_version.empty()) meta += "v" + info->mod_version + "  |  ";
        }
        meta += m.folder;
        if (info && !supports_game_version(*info, g_mm->game_version())) {
            if (!info->version_min.empty() && !info->version_max.empty())
                meta += "  |  PZ " + info->version_min + "-" + info->version_max;
            else if (!info->version_min.empty())
                meta += "  |  PZ " + info->version_min + "+";
            else if (!info->version_max.empty())
                meta += "  |  PZ up to " + info->version_max;
        }
        g_row_names.push_back(to_w(title));
        g_row_meta.push_back(to_w(meta));
        g_row_folders.push_back(m.folder);
        g_row_updates.push_back(g_mm->update_available(m.workshop_id) ? 1 : 0);
        LRESULT added = SendMessageW(g_list, LB_ADDSTRING, 0,
                                     (LPARAM)g_row_names.back().c_str());
        if (added >= 0) g_row_ids.push_back(m.workshop_id);
        if (auto bitmap = load_icon_bitmap(g_mm->icon_for(m.folder)))
            g_icons[m.folder] = bitmap;
    }

    std::vector<std::uint64_t> keep;
    for (auto id : g_selected_ids) {
        if (std::find(g_row_ids.begin(), g_row_ids.end(), id) == g_row_ids.end()) continue;
        if (std::find(keep.begin(), keep.end(), id) == keep.end()) keep.push_back(id);
    }
    g_selected_ids.swap(keep);
    for (std::size_t i = 0; i < g_row_ids.size(); ++i) {
        if (std::find(g_selected_ids.begin(), g_selected_ids.end(), g_row_ids[i]) !=
            g_selected_ids.end())
            SendMessageW(g_list, LB_SETSEL, TRUE, (LPARAM)i);
    }

    SetWindowTextW(g_count,
        (to_w(std::to_string(g_row_ids.size())) + L" shown").c_str());
    SetWindowTextW(g_sort, g_sort_mode == 0 ? L"Sort: Name" : L"Sort: Updated");
    for (HWND b : {g_filter_all, g_filter_updates, g_sort}) InvalidateRect(b, nullptr, TRUE);
    update_actions();
    set_details();
    set_status("Ready");
}

void sync_selection() {
    g_selected_ids = selected_ids();
    update_actions();
    set_details();
}

void select_all() {
    if (g_row_ids.empty()) return;
    SendMessageW(g_list, LB_SETSEL, TRUE, -1);
    sync_selection();
}

void clear_selection() {
    SendMessageW(g_list, LB_SETSEL, FALSE, -1);
    g_selected_ids.clear();
    update_actions();
    set_details();
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
    set_status("Working... steamcmd may take a while...");

    if (g_worker.joinable()) g_worker.join();
    try {
        g_worker = std::thread(worker_main, line);
    } catch (const std::exception& e) {
        g_busy = false;
        EnableWindow(g_input, TRUE);
        EnableWindow(g_list, TRUE);
        update_actions();
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

void add_input() {
    if (g_busy) return;
    std::string id = read_edit(g_input);
    if (id.empty()) {
        set_status("Enter a Workshop URL or ID first");
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
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    switch (id) {
    case ID_ADD:
        MoveToEx(hdc, x + 7, cy - 7, nullptr);
        LineTo(hdc, x + 7, cy + 3);
        MoveToEx(hdc, x + 2, cy - 2, nullptr);
        LineTo(hdc, x + 7, cy + 3);
        LineTo(hdc, x + 12, cy - 2);
        MoveToEx(hdc, x, cy + 8, nullptr);
        LineTo(hdc, x + 14, cy + 8);
        break;
    case ID_UPDATE:
    case ID_UPDATE_ALL:
        MoveToEx(hdc, x + 7, cy + 7, nullptr);
        LineTo(hdc, x + 7, cy - 7);
        MoveToEx(hdc, x + 2, cy - 2, nullptr);
        LineTo(hdc, x + 7, cy - 7);
        LineTo(hdc, x + 12, cy - 2);
        if (id == ID_UPDATE_ALL) {
            MoveToEx(hdc, x + 1, cy + 7, nullptr);
            LineTo(hdc, x + 13, cy + 7);
        }
        break;
    case ID_REMOVE:
        Rectangle(hdc, x + 2, cy - 3, x + 12, cy + 6);
        MoveToEx(hdc, x, cy - 6, nullptr);
        LineTo(hdc, x + 14, cy - 6);
        MoveToEx(hdc, x + 5, cy - 8, nullptr);
        LineTo(hdc, x + 9, cy - 8);
        MoveToEx(hdc, x + 5, cy, nullptr);
        LineTo(hdc, x + 5, cy + 4);
        MoveToEx(hdc, x + 9, cy, nullptr);
        LineTo(hdc, x + 9, cy + 4);
        break;
    case ID_SELECT_ALL:
        MoveToEx(hdc, x + 1, cy, nullptr);
        LineTo(hdc, x + 5, cy + 4);
        LineTo(hdc, x + 13, cy - 6);
        break;
    case ID_CLEAR:
        MoveToEx(hdc, x + 2, cy - 6, nullptr);
        LineTo(hdc, x + 12, cy + 6);
        MoveToEx(hdc, x + 12, cy - 6, nullptr);
        LineTo(hdc, x + 2, cy + 6);
        break;
    case ID_SETTINGS:
        Ellipse(hdc, x + 1, cy - 6, x + 13, cy + 6);
        MoveToEx(hdc, x + 7, cy - 9, nullptr);
        LineTo(hdc, x + 7, cy - 6);
        MoveToEx(hdc, x + 7, cy + 6, nullptr);
        LineTo(hdc, x + 7, cy + 9);
        MoveToEx(hdc, x - 2, cy, nullptr);
        LineTo(hdc, x + 1, cy);
        MoveToEx(hdc, x + 13, cy, nullptr);
        LineTo(hdc, x + 16, cy);
        break;
    case ID_FILTER_ALL:
    case ID_FILTER_UPDATES:
        MoveToEx(hdc, x, cy - 7, nullptr);
        LineTo(hdc, x + 14, cy - 7);
        LineTo(hdc, x + 9, cy - 1);
        LineTo(hdc, x + 9, cy + 6);
        LineTo(hdc, x + 5, cy + 8);
        LineTo(hdc, x + 5, cy - 1);
        LineTo(hdc, x, cy - 7);
        break;
    case ID_SORT:
        MoveToEx(hdc, x + 4, cy + 7, nullptr);
        LineTo(hdc, x + 4, cy - 7);
        MoveToEx(hdc, x + 1, cy - 3, nullptr);
        LineTo(hdc, x + 4, cy - 7);
        LineTo(hdc, x + 7, cy - 3);
        MoveToEx(hdc, x + 10, cy - 7, nullptr);
        LineTo(hdc, x + 10, cy + 7);
        MoveToEx(hdc, x + 7, cy + 3, nullptr);
        LineTo(hdc, x + 10, cy + 7);
        LineTo(hdc, x + 13, cy + 3);
        break;
    case ID_CHECK:
        Arc(hdc, x + 1, cy - 6, x + 13, cy + 7, x + 7, cy + 7, x + 7, cy - 7);
        MoveToEx(hdc, x + 7, cy - 8, nullptr);
        LineTo(hdc, x + 7, cy - 4);
        MoveToEx(hdc, x + 7, cy - 8, nullptr);
        LineTo(hdc, x + 11, cy - 6);
        break;
    default:
        break;
    }
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);
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
    COLORREF text_color = RGB(31, 38, 46);
    if (disabled) {
        fill_brush = g_bg_brush;
        text_color = RGB(108, 118, 132);
    } else if (active_chip) {
        fill_brush = g_selected_brush;
        border_brush = g_accent_brush;
        text_color = RGB(224, 100, 43);
    } else if (primary) {
        fill_brush = g_accent_brush;
        border_brush = g_accent_brush;
        text_color = RGB(255, 255, 255);
    } else if (pressed) {
        fill_brush = g_selected_brush;
        border_brush = g_accent_brush;
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
    const int start = r.left + (r.right - r.left - 14 - 6 - size.cx) / 2;
    draw_button_icon(dis->hDC, id, start, (r.top + r.bottom) / 2, text_color);
    RECT text_rc{start + 20, r.top, r.right - 4, r.bottom};
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
    RECT icon_rc{card.left + 10, card.top + 8, card.left + 58, card.top + 56};
    bool drew_icon = false;
    auto it = g_icons.find(g_row_folders[index]);
    if (it != g_icons.end() && it->second) {
        HDC memory = CreateCompatibleDC(dis->hDC);
        HGDIOBJ old = SelectObject(memory, it->second);
        BitBlt(dis->hDC, icon_rc.left, icon_rc.top, 48, 48, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
        drew_icon = true;
    }
    if (!drew_icon) {
        HBRUSH soft = CreateSolidBrush(RGB(255, 242, 234));
        FillRect(dis->hDC, &icon_rc, soft);
        DeleteObject(soft);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, RGB(224, 100, 43));
        HGDIOBJ old = SelectObject(dis->hDC, g_font_title);
        std::wstring initial = g_row_names[index].substr(0, 1);
        DrawTextW(dis->hDC, initial.c_str(), -1, &icon_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
    }

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, RGB(31, 38, 46));
    HGDIOBJ old_font = SelectObject(dis->hDC, g_font_title);
    const int text_right = card.right - (has_update ? 132 : 50);
    RECT name_rc{card.left + 72, card.top + 9, text_right, card.top + 31};
    DrawTextW(dis->hDC, g_row_names[index].c_str(), -1, &name_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dis->hDC, g_font_small);
    SetTextColor(dis->hDC, RGB(108, 118, 132));
    RECT meta_rc{card.left + 72, card.top + 32, text_right, card.top + 54};
    DrawTextW(dis->hDC, g_row_meta[index].c_str(), -1, &meta_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (has_update) {
        RECT pill{card.right - 124, card.top + 22, card.right - 60, card.top + 42};
        HBRUSH orange = CreateSolidBrush(RGB(224, 100, 43));
        FillRect(dis->hDC, &pill, orange);
        DeleteObject(orange);
        SetTextColor(dis->hDC, RGB(255, 255, 255));
        DrawTextW(dis->hDC, L"UPDATE", -1, &pill,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    const int cx = card.right - 30, cy = card.top + kRowH / 2;
    HBRUSH circle = CreateSolidBrush(selected ? RGB(224, 100, 43) : RGB(255, 255, 255));
    HPEN ring = CreatePen(PS_SOLID, 1,
                          selected ? RGB(224, 100, 43) : RGB(194, 202, 214));
    HGDIOBJ old_brush = SelectObject(dis->hDC, circle);
    HGDIOBJ old_pen = SelectObject(dis->hDC, ring);
    Ellipse(dis->hDC, cx - 9, cy - 9, cx + 9, cy + 9);
    SelectObject(dis->hDC, old_pen);
    SelectObject(dis->hDC, old_brush);
    DeleteObject(ring);
    DeleteObject(circle);

    if (selected) {
        HPEN check = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dis->hDC, check);
        MoveToEx(dis->hDC, cx - 4, cy, nullptr);
        LineTo(dis->hDC, cx - 1, cy + 3);
        LineTo(dis->hDC, cx + 5, cy - 4);
        SelectObject(dis->hDC, old);
        DeleteObject(check);
    }
    SelectObject(dis->hDC, old_font);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_settings_wnd = hwnd;
        {
            HWND label = CreateWindowW(L"STATIC", L"steamcmd executable (blank = built-in):",
                WS_CHILD | WS_VISIBLE, 24, 20, 560, 20, hwnd, nullptr, nullptr, nullptr);
            set_font(label, g_font_body);
            g_set_path = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                24, 44, 560, 28, hwnd, nullptr, nullptr, nullptr);
            set_font(g_set_path, g_font_body);
            label = CreateWindowW(L"STATIC", L"Steam username:",
                WS_CHILD | WS_VISIBLE, 24, 86, 560, 20, hwnd, nullptr, nullptr, nullptr);
            set_font(label, g_font_body);
            g_set_user = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                24, 110, 560, 28, hwnd, nullptr, nullptr, nullptr);
            set_font(g_set_user, g_font_body);
            label = CreateWindowW(L"STATIC",
                L"No password is stored. Run steamcmd manually once so its login is cached.",
                WS_CHILD | WS_VISIBLE, 24, 150, 570, 20, hwnd, nullptr, nullptr, nullptr);
            set_font(label, g_font_small);
            HWND save = CreateWindowW(L"BUTTON", L"Save",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 24, 186, 120, 30, hwnd,
                (HMENU)ID_SET_SAVE, nullptr, nullptr);
            set_font(save, g_font_body);
            HWND cancel = CreateWindowW(L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE, 158, 186, 120, 30, hwnd,
                (HMENU)ID_SET_CANCEL, nullptr, nullptr);
            set_font(cancel, g_font_body);
        }

        SetWindowTextW(g_set_path, to_w(g_mm->config().steamcmd_exe.string()).c_str());
        SetWindowTextW(g_set_user, to_w(g_mm->config().steam_user).c_str());
        SetFocus(g_set_path);
        return 0;
    case WM_CTLCOLORSTATIC: {
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)g_bg_brush;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SET_SAVE) {
            std::string path = read_edit(g_set_path);
            std::string user = read_edit(g_set_user);
            if (user.empty()) {
                MessageBoxW(hwnd, L"Steam username is required.", L"Settings",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            auto log = [](const std::string& s) { append_log(s); };
            auto r1 = run_command(*g_mm, "config steamcmd_exe " + path, log);
            auto r2 = run_command(*g_mm, "config steam_user " + user, log);
            if (r1.ok && r2.ok) {
                append_log("settings saved");
                DestroyWindow(hwnd);
            } else {
                MessageBoxW(hwnd, L"Could not save settings.", L"Settings",
                            MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_SET_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_settings_wnd = nullptr;
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
    wc.hbrBackground = g_bg_brush;
    RegisterClassW(&wc);

    RECT r{};
    GetWindowRect(owner, &r);
    const int w = 620, h = 260;
    const int x = r.left + ((r.right - r.left) - w) / 2;
    const int y = r.top + ((r.bottom - r.top) - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PZSettingsWnd", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, owner, nullptr, hi, nullptr);
    if (!dlg) return;

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);

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
    set_status("Settings updated");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        {
            g_title = CreateWindowW(L"STATIC", L"PZ Mod Manager",
                WS_CHILD | WS_VISIBLE, 24, 18, 600, 28, hwnd, nullptr, nullptr, nullptr);
            set_font(g_title, g_font_title);
            g_subtitle = CreateWindowW(L"STATIC", L"Project Zomboid Workshop installer",
                WS_CHILD | WS_VISIBLE, 24, 46, 600, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(g_subtitle, g_font_small);
            g_accent_bar = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE, 24, 24, 4, 24, hwnd, nullptr, nullptr, nullptr);
            g_settings_btn = CreateWindowW(L"BUTTON", L"Settings",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 880, 24, 96, 30, hwnd,
                (HMENU)ID_SETTINGS, nullptr, nullptr);
            set_font(g_settings_btn, g_font_body);

            g_input = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 24, 84, 700, 38,
                hwnd, (HMENU)ID_INPUT, nullptr, nullptr);
            set_font(g_input, g_font_body);
            g_add = CreateWindowW(L"BUTTON", L"Install",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 740, 84, 212, 38, hwnd,
                (HMENU)ID_ADD, nullptr, nullptr);
            set_font(g_add, g_font_body);
            SendMessageW(g_input, 0x1501, TRUE,
                         (LPARAM)L"Paste a Workshop URL or numeric ID");

            HWND label = CreateWindowW(L"STATIC",
                L"Paste a Workshop URL or numeric ID. Press Enter to install; collections are supported.",
                WS_CHILD | WS_VISIBLE, 24, 132, 928, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(label, g_font_small);

            g_filter = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                24, 158, 280, 24, hwnd, (HMENU)ID_FILTER, nullptr, nullptr);
            set_font(g_filter, g_font_small);
            SendMessageW(g_filter, 0x1501, TRUE,
                         (LPARAM)L"Filter name, author, version, or folder");
            g_filter_all = CreateWindowW(L"BUTTON", L"All",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 312, 158, 60, 24, hwnd,
                (HMENU)ID_FILTER_ALL, nullptr, nullptr);
            g_filter_updates = CreateWindowW(L"BUTTON", L"Updates",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 378, 158, 112, 24, hwnd,
                (HMENU)ID_FILTER_UPDATES, nullptr, nullptr);
            g_sort = CreateWindowW(L"BUTTON", L"Sort: Name",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 506, 158, 140, 24, hwnd,
                (HMENU)ID_SORT, nullptr, nullptr);
            g_check = CreateWindowW(L"BUTTON", L"Check",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 660, 158, 100, 24, hwnd,
                (HMENU)ID_CHECK, nullptr, nullptr);
            for (HWND b : {g_filter_all, g_filter_updates, g_sort, g_check})
                set_font(b, g_font_small);
            g_count = CreateWindowW(L"STATIC", L"0 shown",
                WS_CHILD | WS_VISIBLE | SS_RIGHT, 800, 158, 176, 18, hwnd, nullptr,
                nullptr, nullptr);
            set_font(g_count, g_font_small);

            g_list = CreateWindowW(L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL |
                    LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                24, 192, 952, 344, hwnd, (HMENU)ID_LIST, nullptr, nullptr);
            SendMessageW(g_list, LB_SETITEMHEIGHT, 0, 68);

            g_update = CreateWindowW(L"BUTTON", L"Update",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 24, 554, 100, 32,
                hwnd, (HMENU)ID_UPDATE, nullptr, nullptr);
            g_remove = CreateWindowW(L"BUTTON", L"Remove",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 136, 554, 130, 32,
                hwnd, (HMENU)ID_REMOVE, nullptr, nullptr);
            g_update_all = CreateWindowW(L"BUTTON", L"Update all",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 278, 554, 110, 32, hwnd,
                (HMENU)ID_UPDATE_ALL, nullptr, nullptr);
            g_select_all = CreateWindowW(L"BUTTON", L"Select all",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 400, 554, 90, 32, hwnd,
                (HMENU)ID_SELECT_ALL, nullptr, nullptr);
            g_clear = CreateWindowW(L"BUTTON", L"Clear",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 502, 554, 60, 32, hwnd,
                (HMENU)ID_CLEAR, nullptr, nullptr);
            for (HWND b : {g_update, g_remove, g_update_all, g_select_all, g_clear})
                set_font(b, g_font_body);

            g_details = CreateWindowW(L"STATIC", L"No selection",
                WS_CHILD | WS_VISIBLE, 600, 562, 376, 18, hwnd, nullptr, nullptr,
                nullptr);
            set_font(g_details, g_font_small);
            label = CreateWindowW(L"STATIC", L"ACTIVITY",
                WS_CHILD | WS_VISIBLE, 24, 602, 200, 18, hwnd, nullptr, nullptr, nullptr);
            set_font(label, g_font_small);
            g_log = CreateWindowW(L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                    WS_BORDER,
                24, 622, 952, 62, hwnd, (HMENU)ID_LOG, nullptr, nullptr);
            set_font(g_log, g_font_small);
            g_status = CreateWindowW(L"STATIC", L"Ready",
                WS_CHILD | WS_VISIBLE, 24, 702, 500, 18, hwnd, nullptr, nullptr,
                nullptr);
            set_font(g_status, g_font_small);
            append_log("Paste a Workshop URL or ID, then press Enter.");
            append_log("Click a mod to select it; Ctrl/Shift-click for more.");
            refresh_list();
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_bg_brush);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == g_accent_bar) {
            SetBkColor((HDC)wp, RGB(224, 100, 43));
            return (LRESULT)g_accent_brush;
        }
        SetTextColor((HDC)wp, RGB(31, 38, 46));
        return (LRESULT)g_bg_brush;
    }
    case WM_CTLCOLOREDIT: {
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)g_card_brush;
    }
    case WM_CTLCOLORLISTBOX: {
        SetBkColor((HDC)wp, RGB(246, 247, 249));
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
            wp == ID_SETTINGS || wp == ID_FILTER_ALL || wp == ID_FILTER_UPDATES ||
            wp == ID_SORT || wp == ID_CHECK) {
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
        case ID_SELECT_ALL:
            select_all();
            return 0;
        case ID_CLEAR:
            clear_selection();
            return 0;
        case ID_FILTER:
            if (HIWORD(wp) == EN_CHANGE) refresh_list();
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
    case WM_APP_DONE:
        if (g_worker.joinable()) g_worker.join();
        drain_pending();
        g_busy = false;
        EnableWindow(g_input, TRUE);
        EnableWindow(g_list, TRUE);
        refresh_list();
        SetFocus(g_input);
        return 0;
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
    g_only_updates = false;
    g_sort_mode = 0;
    g_selected_ids.clear();
    g_row_ids.clear();
    g_row_names.clear();
    g_row_meta.clear();
    g_row_folders.clear();
    g_row_updates.clear();
    clear_icons();

    SetProcessDPIAware();
    g_font_title = make_font(18, FW_SEMIBOLD);
    g_font_body = make_font(10, FW_NORMAL);
    g_font_small = make_font(8, FW_NORMAL);
    g_bg_brush = CreateSolidBrush(RGB(246, 247, 249));
    g_card_brush = CreateSolidBrush(RGB(255, 255, 255));
    g_selected_brush = CreateSolidBrush(RGB(255, 242, 234));
    g_border_brush = CreateSolidBrush(RGB(220, 225, 232));
    g_accent_brush = CreateSolidBrush(RGB(224, 100, 43));

    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PZModMgrWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bg_brush;
    RegisterClassW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                        WS_CLIPCHILDREN;
    RECT wr{0, 0, kWindowW, kWindowH};
    AdjustWindowRect(&wr, style, FALSE);
    HWND w = CreateWindowExW(0, L"PZModMgrWnd", L"PZ Mod Manager", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left,
                             wr.bottom - wr.top, nullptr, nullptr, hi, nullptr);
    if (!w) return 1;
    ShowWindow(w, SW_SHOW);
    SetFocus(g_input);

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

    clear_icons();
    for (HWND b : {g_title, g_subtitle, g_input, g_add, g_list, g_log, g_update,
                   g_remove, g_update_all, g_settings_btn, g_select_all, g_clear,
                   g_details, g_status, g_count}) {
        (void)b;
    }
    DeleteObject(g_accent_brush);
    DeleteObject(g_border_brush);
    DeleteObject(g_selected_brush);
    DeleteObject(g_card_brush);
    DeleteObject(g_bg_brush);
    DeleteObject(g_font_small);
    DeleteObject(g_font_body);
    DeleteObject(g_font_title);
    return (int)m.wParam;
}
}  // namespace pz
