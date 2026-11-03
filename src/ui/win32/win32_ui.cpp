#include "ui/ui.hpp"
#include "core/commands.hpp"
#include <windows.h>
#include <algorithm>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pz {
namespace {
constexpr int ID_INPUT = 101, ID_ADD = 102, ID_LIST = 103, ID_LOG = 104;
constexpr int ID_UPDATE = 105, ID_REMOVE = 106, ID_UPDATE_ALL = 107, ID_SETTINGS = 108;
constexpr int ID_SELECT_ALL = 109;
constexpr int ID_SET_SAVE = 201, ID_SET_CANCEL = 202;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;

ModManager* g_mm = nullptr;
HWND g_hwnd = nullptr;
HWND g_input = nullptr;
HWND g_add = nullptr;
HWND g_list = nullptr;
HWND g_log = nullptr;
HWND g_update = nullptr;
HWND g_remove = nullptr;
HWND g_update_all = nullptr;
HWND g_settings_btn = nullptr;
HWND g_select_all = nullptr;
HWND g_details = nullptr;
HWND g_status = nullptr;

HWND g_settings_wnd = nullptr;
HWND g_set_path = nullptr;
HWND g_set_user = nullptr;

std::thread g_worker;
std::mutex g_pending_mu;
std::deque<std::string> g_pending;
std::deque<std::wstring> g_row_text;          // stable strings for LB_ADDSTRING
std::vector<std::uint64_t> g_row_ids;         // workshop id per list row
std::vector<std::uint64_t> g_selected_ids;    // selected workshop ids
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
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
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

void update_actions() {
    const bool has = !g_selected_ids.empty();
    EnableWindow(g_add, !g_busy);
    EnableWindow(g_update_all, !g_busy);
    EnableWindow(g_settings_btn, !g_busy);
    EnableWindow(g_select_all, !g_busy && !g_row_ids.empty());
    EnableWindow(g_update, !g_busy && has);
    EnableWindow(g_remove, !g_busy && has);
}

std::vector<std::uint64_t> selected_ids() {
    std::vector<std::uint64_t> ids;
    int count = (int)SendMessageW(g_list, LB_GETSELCOUNT, 0, 0);
    if (count < 0) {   // single-selection fallback
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
        SetWindowTextW(g_details, L"Selected: none");
        return;
    }
    if (g_selected_ids.size() > 1) {
        std::wstring text = L"Selected: " + to_w(std::to_string(g_selected_ids.size())) + L" mods";
        SetWindowTextW(g_details, text.c_str());
        return;
    }

    int index = -1;
    SendMessageW(g_list, LB_GETSELITEMS, 1, (LPARAM)&index);
    wchar_t row[512]{};
    if (index >= 0) SendMessageW(g_list, LB_GETTEXT, index, (LPARAM)row);
    std::wstring text = std::wstring(L"Selected: ") + row;
    SetWindowTextW(g_details, text.c_str());
}

void refresh_list() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    g_row_ids.clear();
    g_row_text.clear();

    for (const auto& m : g_mm->list()) {
        std::wstring row = to_w(m.name + "  [" + m.folder + "]  id=" + std::to_string(m.workshop_id));
        g_row_text.push_back(row);
        LRESULT added = SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)g_row_text.back().c_str());
        if (added >= 0) g_row_ids.push_back(m.workshop_id);
    }

    // Keep only selections that still exist, then restore them.
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

    update_actions();
    set_details();
    set_status(std::to_string(g_row_ids.size()) + " mod(s) installed");
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

// UI thread only: drains lines queued by the worker.
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
    update_actions();
    set_status("Working... steamcmd may take a while");

    if (g_worker.joinable()) g_worker.join();   // previous command finished
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
        wchar_t name[256]{};
        if (index >= 0) SendMessageW(g_list, LB_GETTEXT, index, (LPARAM)name);
        msg = std::wstring(L"Remove this installed mod?\r\n\r\n") + name;
    } else {
        msg = L"Remove " + to_w(std::to_string(ids.size())) + L" selected mods?";
    }
    if (MessageBoxW(g_hwnd, msg.c_str(), L"Remove mods", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    std::string cmd = "remove";
    for (auto id : ids) cmd += " " + std::to_string(id);
    start_command(cmd);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_settings_wnd = hwnd;
        CreateWindowW(L"STATIC", L"steamcmd executable (blank = built-in):",
            WS_CHILD | WS_VISIBLE, 20, 16, 560, 20, hwnd, nullptr, nullptr, nullptr);
        g_set_path = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            20, 38, 560, 24, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Steam username:",
            WS_CHILD | WS_VISIBLE, 20, 74, 560, 20, hwnd, nullptr, nullptr, nullptr);
        g_set_user = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            20, 96, 560, 24, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC",
            L"No password is stored. Run steamcmd manually once so its login is cached.",
            WS_CHILD | WS_VISIBLE, 20, 130, 560, 20, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            20, 160, 100, 26, hwnd, (HMENU)ID_SET_SAVE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
            130, 160, 100, 26, hwnd, (HMENU)ID_SET_CANCEL, nullptr, nullptr);

        SetWindowTextW(g_set_path, to_w(g_mm->config().steamcmd_exe.string()).c_str());
        SetWindowTextW(g_set_user, to_w(g_mm->config().steam_user).c_str());
        SetFocus(g_set_path);
        return 0;
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
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    RECT r{};
    GetWindowRect(owner, &r);
    const int w = 620, h = 230;
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
        CreateWindowW(L"STATIC", L"Workshop URL/ID:",
            WS_CHILD | WS_VISIBLE, 10, 16, 130, 20, hwnd, nullptr, nullptr, nullptr);
        g_input = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            150, 12, 470, 24, hwnd, (HMENU)ID_INPUT, nullptr, nullptr);
        g_add = CreateWindowW(L"BUTTON", L"Add / Install",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 630, 12, 150, 24, hwnd,
            (HMENU)ID_ADD, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Installed mods (Ctrl/Shift-click for multiple):",
            WS_CHILD | WS_VISIBLE, 10, 48, 500, 20, hwnd, nullptr, nullptr, nullptr);
        g_list = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            LBS_NOTIFY | LBS_EXTENDEDSEL | WS_TABSTOP, 10, 68, 780, 210, hwnd,
            (HMENU)ID_LIST, nullptr, nullptr);
        g_update = CreateWindowW(L"BUTTON", L"Update selected",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 10, 288, 140, 24, hwnd, (HMENU)ID_UPDATE,
            nullptr, nullptr);
        g_remove = CreateWindowW(L"BUTTON", L"Remove selected",
            WS_CHILD | WS_VISIBLE | WS_DISABLED, 160, 288, 140, 24, hwnd, (HMENU)ID_REMOVE,
            nullptr, nullptr);
        g_update_all = CreateWindowW(L"BUTTON", L"Update all",
            WS_CHILD | WS_VISIBLE, 310, 288, 120, 24, hwnd, (HMENU)ID_UPDATE_ALL,
            nullptr, nullptr);
        g_settings_btn = CreateWindowW(L"BUTTON", L"Settings",
            WS_CHILD | WS_VISIBLE, 440, 288, 100, 24, hwnd, (HMENU)ID_SETTINGS,
            nullptr, nullptr);
        g_select_all = CreateWindowW(L"BUTTON", L"Select all",
            WS_CHILD | WS_VISIBLE, 550, 288, 100, 24, hwnd, (HMENU)ID_SELECT_ALL,
            nullptr, nullptr);
        g_details = CreateWindowW(L"STATIC", L"Selected: none",
            WS_CHILD | WS_VISIBLE, 10, 322, 780, 20, hwnd, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Activity",
            WS_CHILD | WS_VISIBLE, 10, 350, 100, 20, hwnd, nullptr, nullptr, nullptr);
        g_log = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
            10, 370, 780, 220, hwnd, (HMENU)ID_LOG, nullptr, nullptr);
        g_status = CreateWindowW(L"STATIC", L"Ready",
            WS_CHILD | WS_VISIBLE, 10, 602, 780, 20, hwnd, nullptr, nullptr, nullptr);
        refresh_list();
        return 0;
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
        if (g_busy) {   // keep the window alive; the worker owns the manager
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
    g_selected_ids.clear();
    g_row_ids.clear();
    g_row_text.clear();
    HINSTANCE hi = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PZModMgrWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND w = CreateWindowExW(0, L"PZModMgrWnd", L"Project Zomboid Mod Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 680, nullptr, nullptr, hi, nullptr);
    ShowWindow(w, SW_SHOW);
    SetFocus(g_input);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) {   // Enter triggers the default Add button
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return (int)m.wParam;
}
}  // namespace pz
