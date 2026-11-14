#include "ui/win32/win32_ui_c.h"

#include "core/commands_c.h"
#include "core/mod_info_fs_c.h"
#include "core/paths_c.h"
#include "ui/glyphs_c.h"
#include "ui/mod_icon_c.h"

#include <windows.h>
#include <shellapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#define PZ_WIN_MAX_ROWS 512
#define PZ_WIN_MAX_FOLDERS 8
#define PZ_WIN_MAX_IDS 512
#define PZ_WIN_MAX_TARGETS 64
#define PZ_WIN_MAX_TARGET_IDS 512
#define PZ_WIN_MAX_INFOS 512
#define PZ_WIN_MAX_ICONS 256
#define PZ_WIN_MAX_TARGET_ICONS 64
#define PZ_WIN_MAX_PENDING 64
#define PZ_WIN_MAX_LINE 2048
#define PZ_WIN_MAX_PATH 1024
#define PZ_WIN_MAX_EDITS 8
#define PZ_WIN_MAX_BARS 4

#define PZ_WIN_ID_INPUT 101
#define PZ_WIN_ID_ADD 102
#define PZ_WIN_ID_LIST 103
#define PZ_WIN_ID_LOG 104
#define PZ_WIN_ID_UPDATE 105
#define PZ_WIN_ID_REMOVE 106
#define PZ_WIN_ID_UPDATE_ALL 107
#define PZ_WIN_ID_SETTINGS 108
#define PZ_WIN_ID_SELECT_ALL 109
#define PZ_WIN_ID_CLEAR 110
#define PZ_WIN_ID_FILTER 111
#define PZ_WIN_ID_FILTER_MODE 112
#define PZ_WIN_ID_SORT 114
#define PZ_WIN_ID_CHECK 115
#define PZ_WIN_ID_COPY 116
#define PZ_WIN_ID_ENABLE 117
#define PZ_WIN_ID_SHARE_SERVER 118
#define PZ_WIN_ID_SELF_UPDATE 119
#define PZ_WIN_ID_FILTER_CLEAR 120
#define PZ_WIN_ID_LOCK 121
#define PZ_WIN_ID_IMPORT 122
#define PZ_WIN_ID_INPUT_CLEAR 123
#define PZ_WIN_ID_OPEN_MODS 124
#define PZ_WIN_ID_SET_SAVE 201
#define PZ_WIN_ID_SET_CANCEL 202
#define PZ_WIN_ID_SET_THEME 203
#define PZ_WIN_ID_SET_AUTO 204
#define PZ_WIN_ID_TGT_LIST 301
#define PZ_WIN_ID_TGT_ENABLE 302
#define PZ_WIN_ID_TGT_DISABLE 303
#define PZ_WIN_ID_TGT_COPY 304
#define PZ_WIN_ID_TGT_CLOSE 305
#define PZ_WIN_ID_TGT_COPY_NAMES 306
#define PZ_WIN_ID_TGT_LOCK 307

#define PZ_WIN_WM_LOG (WM_APP + 1)
#define PZ_WIN_WM_DONE (WM_APP + 2)
#define PZ_WIN_WM_RELOAD (WM_APP + 3)
#define PZ_WIN_AUTO_TIMER 1

#define PZ_WIN_WIDTH 920
#define PZ_WIN_HEIGHT 720
#define PZ_WIN_ROW_H 64
#define PZ_WIN_SCROLL_W 6
#define PZ_WIN_SCROLL_GRAB 3
#define PZ_WIN_SCROLL_WIN_W (PZ_WIN_SCROLL_W + 2 * PZ_WIN_SCROLL_GRAB)
#define PZ_WIN_SCROLL_MIN_THUMB 24

#define PZ_WIN_SET_CLIENT_W 660
#define PZ_WIN_SET_CLIENT_H 600
#define PZ_WIN_SET_PANEL_X 24
#define PZ_WIN_SET_PANEL_Y 72
#define PZ_WIN_SET_PANEL_W 612
#define PZ_WIN_SET_PANEL_H 500
#define PZ_WIN_SET_FIELD_X 56
#define PZ_WIN_SET_FIELD_W 548
#define PZ_WIN_SET_FIELD_H 38
#define PZ_WIN_SET_ZOMBOID_Y 116
#define PZ_WIN_SET_PATH_Y 190
#define PZ_WIN_SET_ROOT_Y 264
#define PZ_WIN_SET_USER_Y 338
#define PZ_WIN_SET_CHECK_Y 384
#define PZ_WIN_SET_BUTTONS_Y 458

#define PZ_WIN_TGT_LIST_X 56
#define PZ_WIN_TGT_LIST_Y 104
#define PZ_WIN_TGT_LIST_W 548
#define PZ_WIN_TGT_LIST_H 264
#define PZ_WIN_TGT_ROW_H 44
#define PZ_WIN_TGT_BAR_X 615
#define PZ_WIN_TGT_STATUS_Y 374
#define PZ_WIN_TGT_BUTTONS_Y 410
#define PZ_WIN_TGT_BUTTON_H 38

typedef struct WinTheme {
    COLORREF bg, surface, field, border, border_strong, text, muted;
    COLORREF accent, accent_hover, accent_soft, danger, danger_soft;
    COLORREF brand, brand_soft, brand_border, cream, hover, success, on_accent;
} WinTheme;

typedef enum WinFilter { WinFilterAll, WinFilterUpdates, WinFilterLocked,
                         WinFilterNontracked } WinFilter;

typedef struct WinRow {
    uint64_t workshop_id;
    char name[256];
    wchar_t wname[256];
    wchar_t wmeta[1024];
    char folder[256];
    char folders[PZ_WIN_MAX_FOLDERS][256];
    int folder_count;
    char icon_path[PZ_WIN_MAX_PATH];
    char mod_ids[PZ_WIN_MAX_FOLDERS][128];
    int mod_id_count;
    uint64_t time_updated;
    int update_available;
    int locked;
    int imported;
    int enabled;
} WinRow;

typedef struct WinInfo {
    char folder[256];
    pz_mod_info info;
    int ok;
} WinInfo;

typedef struct WinIcon {
    char key[PZ_WIN_MAX_PATH];
    HBITMAP bitmap;
} WinIcon;

typedef struct WinScrollBar {
    HWND wnd;
    HWND target;
    int log;
    int on_surface;
    int dragging;
    int drag_offset;
    int last_total;
    int last_page;
    int last_pos;
} WinScrollBar;

typedef struct WinEditPlaceholder {
    HWND edit;
    WNDPROC old_proc;
    const wchar_t* text;
} WinEditPlaceholder;

typedef struct WinUi {
    pz_mod_manager* manager;
    char mods_dir[PZ_WIN_MAX_PATH];
    char data_dir[PZ_WIN_MAX_PATH];
    char game_version[64];

    HWND hwnd;
    HWND brand_mark;
    HWND brand_line;
    HWND title;
    HWND subtitle;
    HWND input;
    HWND input_clear;
    HWND add;
    HWND import;
    HWND open_mods;
    HWND filter;
    HWND filter_clear;
    HWND filter_combo;
    HWND sort;
    HWND check;
    HWND list;
    HWND empty;
    HWND log;
    HWND update;
    HWND remove;
    HWND update_all;
    HWND settings_btn;
    HWND self_update;
    HWND enable_btn;
    HWND share_server_btn;
    HWND select_all;
    HWND clear;
    HWND copy;
    HWND lock;
    HWND details;
    HWND status;
    HWND count;
    HWND activity_label;
    WNDPROC list_old_proc;
    WNDPROC log_old_proc;
    WNDPROC tgt_list_old_proc;

    HWND settings_wnd;
    HWND set_zomboid;
    HWND set_path;
    HWND set_root;
    HWND set_user;
    HWND set_auto;
    int set_auto_checked;
    int settings_dark;
    HWND targets_wnd;
    HWND tgt_list;
    HWND tgt_status;
    HWND tgt_copy;
    HWND tgt_lock;
    HWND tgt_copy_names;
    int tgt_lock_checked;

    HFONT font_title;
    HFONT font_row;
    HFONT font_brand;
    HFONT font_body;
    HFONT font_small;
    HBRUSH bg_brush;
    HBRUSH card_brush;
    HBRUSH field_brush;
    HBRUSH selected_brush;
    HBRUSH border_brush;
    HBRUSH accent_brush;
    HBRUSH brand_brush;
    HBRUSH brand_soft_brush;
    HBRUSH border_strong_brush;
    WinTheme theme;
    int dark_mode;

    WinRow* all_rows;
    int all_count;
    int all_capacity;
    WinRow* rows;
    int row_count;
    int row_capacity;
    WinFilter filter_mode;
    int sort_mode;
    int filter_total;
    int filter_updates;
    int filter_locked;
    int filter_nontracked;
    WinFilter filter_choices[4];
    int filter_choice_count;
    int filter_combo_updating;

    uint64_t selected_ids[PZ_WIN_MAX_IDS];
    int selected_count;
    char selected_folders[PZ_WIN_MAX_IDS][256];
    int selected_folder_count;

    pz_activation_target_data targets[PZ_WIN_MAX_TARGETS];
    int target_count;
    char target_mod_ids[PZ_WIN_MAX_TARGET_IDS][128];
    int target_mod_id_count;

    WinInfo infos[PZ_WIN_MAX_INFOS];
    int info_count;
    WinIcon icons[PZ_WIN_MAX_ICONS];
    int icon_count;
    WinIcon target_icons[PZ_WIN_MAX_TARGET_ICONS];
    int target_icon_count;

    WinScrollBar bars[PZ_WIN_MAX_BARS];
    int bar_count;
    WinEditPlaceholder edits[PZ_WIN_MAX_EDITS];
    int edit_count;

    CRITICAL_SECTION pending_mu;
    char pending[PZ_WIN_MAX_PENDING][PZ_WIN_MAX_LINE];
    int pending_count;
    HANDLE worker;
    char worker_line[PZ_WIN_MAX_LINE];

    HANDLE watcher;
    HANDLE watch_stop;
    HANDLE watch_change;

    int busy;
    int reload_pending;
    int app_update_available;
    int auto_check_due;
    char dependency_signature[1024];
    char dependency_warning[512];
} WinUi;

static WinUi* g_ui_ptr = NULL;
#define g_ui (*g_ui_ptr)
static int g_scroll_class_registered = 0;

/* ------------------------------------------------------------------ text */

static void str_copy(char* out, size_t capacity, const char* value) {
    if (!out || capacity == 0) return;
    size_t n = 0;
    if (value) {
        while (value[n] != '\0' && n + 1 < capacity) {
            out[n] = value[n];
            ++n;
        }
    }
    out[n] = '\0';
}

static void str_copy_n(char* out, size_t capacity, const char* value,
                       size_t length) {
    if (!out || capacity == 0) return;
    if (!value) length = 0;
    if (length + 1 > capacity) length = capacity - 1;
    if (length != 0) memcpy(out, value, length);
    out[length] = '\0';
}

static void str_append_n(char* out, size_t capacity, const char* value,
                         size_t length) {
    const size_t used = strlen(out);
    if (used + 1 >= capacity) return;
    str_copy_n(out + used, capacity - used, value, length);
}

static void str_append(char* out, size_t capacity, const char* value) {
    str_append_n(out, capacity, value, value ? strlen(value) : 0);
}

static void str_append_char(char* out, size_t capacity, char value) {
    const size_t used = strlen(out);
    if (used + 1 >= capacity) return;
    out[used] = value;
    out[used + 1] = '\0';
}

static void str_append_u64(char* out, size_t capacity, uint64_t value) {
    char digits[24];
    int count = 0;
    if (value == 0) digits[count++] = '0';
    while (value != 0) {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (count != 0) str_append_char(out, capacity, digits[--count]);
}

static void copy_pz(char* out, size_t capacity, const pz_str* value) {
    if (!value) {
        if (capacity != 0) out[0] = '\0';
        return;
    }
    str_copy_n(out, capacity, value->data, value->length);
}

static char lower_char(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int text_contains_ci(const char* haystack, const char* needle) {
    if (!needle || !*needle) return 1;
    const size_t length = strlen(needle);
    for (const char* start = haystack; *start; ++start) {
        size_t i = 0;
        while (i < length && start[i] &&
               lower_char(start[i]) == lower_char(needle[i]))
            ++i;
        if (i == length) return 1;
    }
    return 0;
}

static void wcopy(wchar_t* out, size_t capacity, const wchar_t* value) {
    if (!out || capacity == 0) return;
    size_t n = 0;
    if (value) {
        while (value[n] != L'\0' && n + 1 < capacity) {
            out[n] = value[n];
            ++n;
        }
    }
    out[n] = L'\0';
}

static void wappend(wchar_t* out, size_t capacity, const wchar_t* value) {
    if (!out || capacity == 0) return;
    const size_t used = wcslen(out);
    if (used + 1 >= capacity) return;
    wcopy(out + used, capacity - used, value);
}

static void wappend_ascii(wchar_t* out, size_t capacity, const char* value) {
    if (!out || capacity == 0) return;
    size_t used = wcslen(out);
    if (!value) return;
    while (*value && used + 1 < capacity) out[used++] = (wchar_t)*value++;
    out[used] = L'\0';
}

static int utf8_to_wide(const char* value, wchar_t* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = L'\0';
    if (!value || !*value) return 1;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
    if (needed <= 0) {
        wappend_ascii(out, capacity, value);
        return 1;
    }
    int written = MultiByteToWideChar(CP_UTF8, 0, value, -1, out,
                                      (int)capacity);
    if (written <= 0) {
        wappend_ascii(out, capacity, value);
        return 1;
    }
    out[capacity - 1] = L'\0';
    return 1;
}

static int wide_to_utf8(const wchar_t* value, char* out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    out[0] = '\0';
    if (!value || !*value) return 1;
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0,
                                           NULL, NULL);
    if (needed <= 0) return 0;
    int written = WideCharToMultiByte(CP_UTF8, 0, value, -1, out,
                                      (int)capacity, NULL, NULL);
    if (written <= 0) return 0;
    out[capacity - 1] = '\0';
    return 1;
}

static int read_edit(HWND edit, char* out, size_t capacity) {
    if (!edit || !out || capacity == 0) return 0;
    const int length = GetWindowTextLengthW(edit);
    wchar_t* buffer = (wchar_t*)malloc(((size_t)length + 1) * sizeof(wchar_t));
    if (!buffer) return 0;
    GetWindowTextW(edit, buffer, length + 1);
    buffer[length] = L'\0';
    const int ok = wide_to_utf8(buffer, out, capacity);
    free(buffer);
    return ok;
}

static void set_text_u8(HWND hwnd, const char* value) {
    if (!hwnd) return;
    wchar_t buffer[PZ_WIN_MAX_LINE];
    utf8_to_wide(value, buffer, sizeof buffer / sizeof buffer[0]);
    SetWindowTextW(hwnd, buffer);
}

static void format_date(uint64_t epoch, char* out, size_t capacity) {
    out[0] = '\0';
    if (epoch == 0) return;
    const time_t when = (time_t)epoch;
    const struct tm* value = localtime(&when);
    if (!value) return;
    if (strftime(out, capacity, "%Y-%m-%d", value) == 0) out[0] = '\0';
}

static const char* filter_name(WinFilter filter) {
    switch (filter) {
    case WinFilterUpdates: return "Updates";
    case WinFilterLocked: return "Locked";
    case WinFilterNontracked: return "Nontracked";
    case WinFilterAll: break;
    }
    return "All";
}

static int filter_count(WinUi* ui, WinFilter filter) {
    if (filter == WinFilterUpdates) return ui->filter_updates;
    if (filter == WinFilterLocked) return ui->filter_locked;
    if (filter == WinFilterNontracked) return ui->filter_nontracked;
    return ui->filter_total;
}

/* --------------------------------------------------------------- geometry */

static void fill_color(HDC dc, const RECT* rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, rect, brush);
    DeleteObject(brush);
}

static void draw_field_frame(HDC dc, const RECT* field, int focused) {
    RECT outer = {field->left - 1, field->top - 1, field->right + 1,
                  field->bottom + 1};
    fill_color(dc, &outer, focused ? g_ui.theme.accent : g_ui.theme.border);
}

static void set_font(HWND hwnd, HFONT font) {
    if (hwnd && font) SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
}

static HFONT make_font(int points, int weight) {
    HDC dc = GetDC(NULL);
    const int height = -MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

/* ---------------------------------------------------------------- theme */

static WinTheme make_theme(int dark) {
    WinTheme t;
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

static void destroy_theme_brushes(void) {
    HBRUSH* brushes[] = {&g_ui.bg_brush, &g_ui.card_brush, &g_ui.field_brush,
                         &g_ui.selected_brush, &g_ui.border_brush,
                         &g_ui.accent_brush, &g_ui.brand_brush,
                         &g_ui.brand_soft_brush, &g_ui.border_strong_brush};
    for (size_t i = 0; i < sizeof brushes / sizeof brushes[0]; ++i) {
        if (*brushes[i]) DeleteObject(*brushes[i]);
        *brushes[i] = NULL;
    }
}

static void create_theme_brushes(void) {
    g_ui.bg_brush = CreateSolidBrush(g_ui.theme.bg);
    g_ui.card_brush = CreateSolidBrush(g_ui.theme.surface);
    g_ui.field_brush = CreateSolidBrush(g_ui.theme.field);
    g_ui.selected_brush = CreateSolidBrush(g_ui.theme.accent_soft);
    g_ui.border_brush = CreateSolidBrush(g_ui.theme.border);
    g_ui.accent_brush = CreateSolidBrush(g_ui.theme.accent);
    g_ui.brand_brush = CreateSolidBrush(g_ui.theme.brand);
    g_ui.brand_soft_brush = CreateSolidBrush(g_ui.theme.brand_soft);
    g_ui.border_strong_brush = CreateSolidBrush(g_ui.theme.border_strong);
}

static void apply_theme(int dark, HWND hwnd) {
    g_ui.dark_mode = dark;
    g_ui.theme = make_theme(dark);
    destroy_theme_brushes();
    create_theme_brushes();
    if (hwnd)
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void apply_dark_title_bar(HWND hwnd, int dark) {
    typedef HRESULT(WINAPI * SetAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    static SetAttributeFn set_attribute = NULL;
    static int resolved = 0;
    if (!resolved) {
        HMODULE dll = LoadLibraryW(L"dwmapi.dll");
        if (dll)
            set_attribute = (SetAttributeFn)(void*)GetProcAddress(
                dll, "DwmSetWindowAttribute");
        resolved = 1;
    }
    if (!set_attribute || !hwnd) return;
    const BOOL value = dark ? TRUE : FALSE;
    if (FAILED(set_attribute(hwnd, 20, &value, sizeof value)))
        set_attribute(hwnd, 19, &value, sizeof value);
}

static void apply_dark_control_theme(HWND hwnd, int dark) {
    typedef HRESULT(WINAPI * SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
    typedef BOOL(WINAPI * AllowDarkFn)(HWND, BOOL);
    static HMODULE uxtheme = NULL;
    static SetWindowThemeFn set_theme = NULL;
    static AllowDarkFn allow_dark = NULL;
    static int resolved = 0;
    if (!resolved) {
        uxtheme = LoadLibraryW(L"uxtheme.dll");
        if (uxtheme) {
            set_theme = (SetWindowThemeFn)(void*)GetProcAddress(
                uxtheme, "SetWindowTheme");
            allow_dark = (AllowDarkFn)(void*)GetProcAddress(
                uxtheme, MAKEINTRESOURCEA(135));
        }
        resolved = 1;
    }
    if (!hwnd) return;
    if (allow_dark) allow_dark(hwnd, dark ? TRUE : FALSE);
    if (set_theme)
        set_theme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
}

static void invalidate_bars(void);

static void refresh_theme(HWND hwnd, int dark) {
    apply_theme(dark, hwnd);
    apply_dark_title_bar(hwnd, dark);
    apply_dark_control_theme(g_ui.input, dark);
    apply_dark_control_theme(g_ui.filter, dark);
    apply_dark_control_theme(g_ui.filter_combo, dark);
    apply_dark_control_theme(g_ui.log, dark);
    invalidate_bars();
}

/* ------------------------------------------------------------ scrollbars */

typedef struct ScrollMetrics {
    int total;
    int page;
    int pos;
    int needed;
} ScrollMetrics;

static int edit_page_lines(HWND edit) {
    RECT rc;
    GetClientRect(edit, &rc);
    HDC dc = GetDC(edit);
    if (!dc) return 1;
    HFONT font = (HFONT)SendMessageW(edit, WM_GETFONT, 0, 0);
    HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    if (old) SelectObject(dc, old);
    ReleaseDC(edit, dc);
    const int height = rc.bottom - rc.top;
    return tm.tmHeight > 0 ? (height / tm.tmHeight > 1 ? height / tm.tmHeight : 1)
                           : 1;
}

static ScrollMetrics bar_metrics(const WinScrollBar* bar) {
    ScrollMetrics m;
    memset(&m, 0, sizeof m);
    RECT rc;
    GetClientRect(bar->target, &rc);
    if (!bar->log) {
        m.total = (int)SendMessageW(bar->target, LB_GETCOUNT, 0, 0);
        const int item_h =
            (int)SendMessageW(bar->target, LB_GETITEMHEIGHT, 0, 0);
        m.page = item_h > 0 ? rc.bottom / item_h : 1;
        if (m.page < 1) m.page = 1;
        m.pos = (int)SendMessageW(bar->target, LB_GETTOPINDEX, 0, 0);
    } else {
        m.total = (int)SendMessageW(bar->target, EM_GETLINECOUNT, 0, 0);
        m.pos = (int)SendMessageW(bar->target, EM_GETFIRSTVISIBLELINE, 0, 0);
        m.page = edit_page_lines(bar->target);
    }
    if (m.total < 0) m.total = 0;
    if (m.page < 1) m.page = 1;
    const int max_pos = m.total - m.page > 0 ? m.total - m.page : 0;
    if (m.pos < 0) m.pos = 0;
    if (m.pos > max_pos) m.pos = max_pos;
    m.needed = m.total > m.page;
    return m;
}

static int thumb_height(const ScrollMetrics* m, int track) {
    int h = track * m->page / (m->total > 0 ? m->total : 1);
    if (h < PZ_WIN_SCROLL_MIN_THUMB) h = PZ_WIN_SCROLL_MIN_THUMB;
    return h < track ? h : track;
}

static int thumb_rect(const WinScrollBar* bar, const ScrollMetrics* m,
                      RECT* thumb) {
    RECT rc;
    GetClientRect(bar->wnd, &rc);
    const int track = rc.bottom - rc.top;
    if (!m->needed || track <= 0) return 0;
    const int h = thumb_height(m, track);
    const int travel = track - h;
    const int max_pos = m->total - m->page > 1 ? m->total - m->page : 1;
    const int y = travel * m->pos / max_pos;
    thumb->left = PZ_WIN_SCROLL_GRAB;
    thumb->top = y;
    thumb->right = PZ_WIN_SCROLL_GRAB + PZ_WIN_SCROLL_W;
    thumb->bottom = y + h;
    return 1;
}

static void update_count_label(void);

static void bar_scroll_to(WinScrollBar* bar, int pos) {
    const ScrollMetrics m = bar_metrics(bar);
    const int max_pos = m.total - m.page > 0 ? m.total - m.page : 0;
    if (pos < 0) pos = 0;
    if (pos > max_pos) pos = max_pos;
    if (!bar->log) {
        SendMessageW(bar->target, LB_SETTOPINDEX, pos, 0);
    } else {
        const int delta = pos - (int)SendMessageW(
            bar->target, EM_GETFIRSTVISIBLELINE, 0, 0);
        if (delta != 0) SendMessageW(bar->target, EM_LINESCROLL, 0, delta);
    }
    bar->last_total = m.total;
    bar->last_page = m.page;
    bar->last_pos = pos;
    InvalidateRect(bar->wnd, NULL, FALSE);
    if (!bar->log) update_count_label();
}

static void update_count_label(void) {
    if (!g_ui.count || !g_ui.list) return;
    const int total = (int)SendMessageW(g_ui.list, LB_GETCOUNT, 0, 0);
    const int item_h =
        (int)SendMessageW(g_ui.list, LB_GETITEMHEIGHT, 0, 0);
    RECT rc;
    GetClientRect(g_ui.list, &rc);
    int rows = item_h > 0 ? rc.bottom / item_h : 1;
    if (rows < 1) rows = 1;
    int first = (int)SendMessageW(g_ui.list, LB_GETTOPINDEX, 0, 0);
    if (first < 0) first = 0;

    wchar_t text[128];
    if (g_ui.busy) {
        wcopy(text, sizeof text / sizeof text[0], L"Working...");
    } else if (total <= 0) {
        wcopy(text, sizeof text / sizeof text[0], L"0 mods");
    } else if (total <= rows) {
        wchar_t number[32];
        _snwprintf(number, sizeof number / sizeof number[0], L"%d", total);
        wcopy(text, sizeof text / sizeof text[0], number);
        wappend(text, sizeof text / sizeof text[0],
                total == 1 ? L" mod" : L" mods");
    } else {
        const int last = first + rows < total ? first + rows : total;
        _snwprintf(text, sizeof text / sizeof text[0], L"%d-%d of %d",
                   first + 1, last, total);
    }
    SetWindowTextW(g_ui.count, text);
}

static void sync_bars(void);

static void invalidate_bars(void) {
    for (int i = 0; i < g_ui.bar_count; ++i) {
        if (!g_ui.bars[i].wnd) continue;
        g_ui.bars[i].last_total = -1;
        InvalidateRect(g_ui.bars[i].wnd, NULL, FALSE);
    }
}

static void paint_scrollbar(const WinScrollBar* bar, HDC dc) {
    RECT rc;
    GetClientRect(bar->wnd, &rc);
    fill_color(dc, &rc, bar->on_surface ? g_ui.theme.surface : g_ui.theme.bg);
    const ScrollMetrics m = bar_metrics(bar);
    if (!m.needed) return;
    RECT track = {PZ_WIN_SCROLL_GRAB, 0,
                  PZ_WIN_SCROLL_GRAB + PZ_WIN_SCROLL_W, rc.bottom};
    FillRect(dc, &track, g_ui.border_brush);
    RECT thumb;
    if (thumb_rect(bar, &m, &thumb))
        FillRect(dc, &thumb,
                 bar->dragging ? g_ui.accent_brush : g_ui.border_strong_brush);
}

static LRESULT CALLBACK ScrollProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinScrollBar* bar =
        (WinScrollBar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (bar) paint_scrollbar(bar, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!bar) return 0;
        SetFocus(bar->target);
        const int y = (short)HIWORD(lp);
        const ScrollMetrics m = bar_metrics(bar);
        RECT thumb;
        if (thumb_rect(bar, &m, &thumb) && y >= thumb.top - PZ_WIN_SCROLL_GRAB &&
            y < thumb.bottom + PZ_WIN_SCROLL_GRAB) {
            bar->dragging = 1;
            bar->drag_offset = y - thumb.top;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (m.needed) {
            bar_scroll_to(bar, y < thumb.top ? m.pos - m.page : m.pos + m.page);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!bar || !bar->dragging) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        const ScrollMetrics m = bar_metrics(bar);
        const int track = rc.bottom - rc.top;
        const int travel = track - thumb_height(&m, track);
        const int max_pos = m.total - m.page > 0 ? m.total - m.page : 0;
        if (travel > 0 && max_pos > 0) {
            const int y = (short)HIWORD(lp);
            int offset = y - bar->drag_offset;
            if (offset < 0) offset = 0;
            if (offset > travel) offset = travel;
            bar_scroll_to(bar, (offset * max_pos + travel / 2) / travel);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (bar && bar->dragging) {
            bar->dragging = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (bar && bar->dragging) {
            bar->dragging = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (bar) SendMessageW(bar->target, WM_MOUSEWHEEL, wp, lp);
        return 0;
    case WM_NCDESTROY:
        if (bar) {
            for (int i = 0; i < g_ui.bar_count; ++i) {
                if (&g_ui.bars[i] == bar) {
                    memmove(&g_ui.bars[i], &g_ui.bars[i + 1],
                            (size_t)(g_ui.bar_count - i - 1) *
                                sizeof g_ui.bars[0]);
                    --g_ui.bar_count;
                    break;
                }
            }
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void register_scrollbar_class(void) {
    if (g_scroll_class_registered) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ScrollProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PZScrollBar";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    g_scroll_class_registered = 1;
}

static HWND create_scrollbar(HWND parent, HWND target, int log,
                             int on_surface, int x, int y, int w, int h) {
    if (g_ui.bar_count >= PZ_WIN_MAX_BARS) return NULL;
    register_scrollbar_class();
    HWND hwnd = CreateWindowExW(0, L"PZScrollBar", L"", WS_CHILD | WS_VISIBLE,
                                x, y, w, h, parent, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!hwnd) return NULL;
    WinScrollBar* bar = &g_ui.bars[g_ui.bar_count++];
    memset(bar, 0, sizeof *bar);
    bar->wnd = hwnd;
    bar->target = target;
    bar->log = log;
    bar->on_surface = on_surface;
    bar->last_total = -1;
    bar->last_page = -1;
    bar->last_pos = -1;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)bar);
    sync_bars();
    return hwnd;
}

static void sync_bars(void) {
    for (int i = 0; i < g_ui.bar_count; ++i) {
        WinScrollBar* bar = &g_ui.bars[i];
        if (!bar->wnd) continue;
        const ScrollMetrics m = bar_metrics(bar);
        if (m.total != bar->last_total || m.page != bar->last_page ||
            m.pos != bar->last_pos) {
            bar->last_total = m.total;
            bar->last_page = m.page;
            bar->last_pos = m.pos;
            InvalidateRect(bar->wnd, NULL, FALSE);
        }
    }
    update_count_label();
}

/* -------------------------------------------------------------- manager */

static void sync_paths(WinUi* ui) {
    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(ui->manager, &config);
    copy_pz(ui->mods_dir, sizeof ui->mods_dir, &config.user_mods_dir);
    copy_pz(ui->data_dir, sizeof ui->data_dir, &config.data_dir);
    const pz_str* version = pz_mod_manager_game_version(ui->manager);
    copy_pz(ui->game_version, sizeof ui->game_version, version);
    pz_paths_config_free(&config);
}

static WinInfo* find_info(WinUi* ui, const char* folder) {
    for (int i = 0; i < ui->info_count; ++i)
        if (strcmp(ui->infos[i].folder, folder) == 0) return &ui->infos[i];
    return NULL;
}

static pz_mod_info* get_info(WinUi* ui, const char* folder) {
    WinInfo* existing = find_info(ui, folder);
    if (existing) return existing->ok ? &existing->info : NULL;
    if (ui->info_count >= PZ_WIN_MAX_INFOS) return NULL;

    WinInfo* entry = &ui->infos[ui->info_count++];
    str_copy(entry->folder, sizeof entry->folder, folder);
    pz_mod_info_init(&entry->info);
    entry->ok = 0;

    pz_path base;
    pz_path path;
    pz_path_init(&base);
    pz_path_init(&path);
    if (pz_path_assign_cstr(&base, ui->mods_dir) &&
        pz_path_join(&path, &base, folder, strlen(folder))) {
        entry->ok = pz_mod_info_read(&path, ui->game_version,
                                     strlen(ui->game_version), &entry->info);
    }
    pz_path_free(&path);
    pz_path_free(&base);
    if (!entry->ok) pz_mod_info_free(&entry->info);
    return entry->ok ? &entry->info : NULL;
}

static void clear_infos(WinUi* ui) {
    for (int i = 0; i < ui->info_count; ++i)
        pz_mod_info_free(&ui->infos[i].info);
    ui->info_count = 0;
}

static void clear_icons(WinUi* ui) {
    for (int i = 0; i < ui->icon_count; ++i)
        if (ui->icons[i].bitmap) DeleteObject(ui->icons[i].bitmap);
    ui->icon_count = 0;
    clear_infos(ui);
}

static void clear_target_icons(WinUi* ui) {
    for (int i = 0; i < ui->target_icon_count; ++i)
        if (ui->target_icons[i].bitmap)
            DeleteObject(ui->target_icons[i].bitmap);
    ui->target_icon_count = 0;
}

static HBITMAP load_icon_bitmap(const char* path, int size) {
    if (!path || !*path || size <= 0) return NULL;
    pz_mod_icon icon;
    pz_mod_icon_init(&icon);
    if (!pz_mod_icon_load(path, strlen(path), size, &icon)) return NULL;

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bitmap =
        CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap || !bits) {
        pz_mod_icon_free(&icon);
        if (bitmap) DeleteObject(bitmap);
        return NULL;
    }

    unsigned char* pixels = (unsigned char*)bits;
    const int bg_r = GetRValue(g_ui.theme.surface);
    const int bg_g = GetGValue(g_ui.theme.surface);
    const int bg_b = GetBValue(g_ui.theme.surface);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t src = ((size_t)y * (size_t)size + (size_t)x) * 4;
            const int a = icon.rgba[src + 3];
            pixels[src + 0] = (unsigned char)((icon.rgba[src + 2] * a +
                                                bg_b * (255 - a)) / 255);
            pixels[src + 1] = (unsigned char)((icon.rgba[src + 1] * a +
                                                bg_g * (255 - a)) / 255);
            pixels[src + 2] = (unsigned char)((icon.rgba[src + 0] * a +
                                                bg_r * (255 - a)) / 255);
            pixels[src + 3] = 255;
        }
    }
    pz_mod_icon_free(&icon);
    return bitmap;
}

static WinIcon* find_icon(WinIcon* entries, int count, const char* key) {
    for (int i = 0; i < count; ++i)
        if (strcmp(entries[i].key, key) == 0) return &entries[i];
    return NULL;
}

static WinIcon* icon_for(WinUi* ui, const WinRow* row) {
    WinIcon* existing = find_icon(ui->icons, ui->icon_count, row->folder);
    if (existing) return existing;
    if (ui->icon_count >= PZ_WIN_MAX_ICONS) return NULL;
    WinIcon* entry = &ui->icons[ui->icon_count++];
    str_copy(entry->key, sizeof entry->key, row->folder);
    entry->bitmap = load_icon_bitmap(row->icon_path, 48);
    return entry;
}

static WinIcon* target_icon_for(WinUi* ui, const pz_activation_target_data* target) {
    if (target->kind != 1 || target->thumbnail.length == 0) return NULL;
    char key[PZ_WIN_MAX_PATH];
    copy_pz(key, sizeof key, &target->thumbnail);
    WinIcon* existing =
        find_icon(ui->target_icons, ui->target_icon_count, key);
    if (existing) return existing;
    if (ui->target_icon_count >= PZ_WIN_MAX_TARGET_ICONS) return NULL;
    WinIcon* entry = &ui->target_icons[ui->target_icon_count++];
    str_copy(entry->key, sizeof entry->key, key);
    entry->bitmap = load_icon_bitmap(key, 36);
    return entry;
}

static void clear_targets(WinUi* ui) {
    for (int i = 0; i < ui->target_count; ++i)
        pz_activation_target_data_free(&ui->targets[i]);
    ui->target_count = 0;
}

static void load_targets(WinUi* ui) {
    clear_targets(ui);
    pz_vec values;
    pz_vec_init(&values, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(ui->manager, &values);
    for (size_t i = 0; i < values.length; ++i) {
        pz_activation_target_data* value =
            (pz_activation_target_data*)pz_vec_at(&values, i);
        if (ui->target_count < PZ_WIN_MAX_TARGETS)
            ui->targets[ui->target_count++] = *value;
        else
            pz_activation_target_data_free(value);
    }
    pz_vec_free(&values);
}

static int target_has_id(const pz_activation_target_data* target,
                         const pz_str* id) {
    for (size_t i = 0; i < target->mod_ids.length; ++i) {
        const pz_str* value = pz_vec_at_const(&target->mod_ids, i);
        if (pz_str_equals(value, id->data, id->length)) return 1;
    }
    return 0;
}

static void activation_name(const pz_activation_target_data* target, char* out,
                            size_t capacity) {
    if (target->kind == 2) {
        str_copy(out, capacity, "Server: ");
        str_append_n(out, capacity, target->name.data, target->name.length);
    } else if (target->kind == 1) {
        str_copy_n(out, capacity, target->name.data, target->name.length);
    } else {
        str_copy(out, capacity, "Default");
    }
}

static void target_state(WinUi* ui, const pz_activation_target_data* target,
                         char* out, size_t capacity) {
    if (ui->target_mod_id_count == 0) {
        str_copy(out, capacity, "no Mod IDs");
        return;
    }
    int count = 0;
    for (int i = 0; i < ui->target_mod_id_count; ++i) {
        pz_str id;
        pz_str_init(&id);
        pz_str_assign_cstr(&id, ui->target_mod_ids[i]);
        if (target_has_id(target, &id)) ++count;
        pz_str_free(&id);
    }
    if (count == 0) {
        str_copy(out, capacity, "off");
    } else if (count == ui->target_mod_id_count) {
        str_copy(out, capacity, "on");
    } else {
        str_copy(out, capacity, "");
        str_append_u64(out, capacity, (uint64_t)count);
        str_append(out, capacity, " of ");
        str_append_u64(out, capacity, (uint64_t)ui->target_mod_id_count);
        str_append(out, capacity, " on");
    }
}

/* ----------------------------------------------------------------- rows */

static void append_log(const char* line);
static void set_status(const char* text);
static void update_actions(void);
static void update_input_clear(void);
static void update_filter_clear(void);
static void refresh_filter_combo(void);
static void set_details(void);

static int ascii_less(const char* a, const char* b) {
    const size_t common = strlen(a) < strlen(b) ? strlen(a) : strlen(b);
    for (size_t i = 0; i < common; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb;
    }
    return strlen(a) < strlen(b);
}

static int reserve_rows(WinRow** rows, int* capacity, int needed) {
    if (needed <= *capacity) return 1;
    int next = *capacity ? *capacity * 2 : 32;
    while (next < needed) next *= 2;
    WinRow* grown = (WinRow*)realloc(*rows, (size_t)next * sizeof(WinRow));
    if (!grown) return 0;
    *rows = grown;
    *capacity = next;
    return 1;
}

static WinRow* find_all_row(WinUi* ui, uint64_t id) {
    for (int i = 0; i < ui->all_count; ++i)
        if (ui->all_rows[i].workshop_id == id) return &ui->all_rows[i];
    return NULL;
}

static void build_all_rows(WinUi* ui) {
    ui->all_count = 0;

    pz_vec mods;
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list_all(ui->manager, &mods);
    reserve_rows(&ui->all_rows, &ui->all_capacity, (int)mods.length);
    for (size_t i = 0; i < mods.length && ui->all_count < ui->all_capacity;
         ++i) {
        const pz_installed_mod* mod = pz_vec_at_const(&mods, i);
        WinRow* row = mod->workshop_id != 0 ? find_all_row(ui, mod->workshop_id)
                                            : NULL;
        if (!row) {
            row = &ui->all_rows[ui->all_count++];
            memset(row, 0, sizeof *row);
            row->workshop_id = mod->workshop_id;
            copy_pz(row->name, sizeof row->name, &mod->name);
            utf8_to_wide(row->name, row->wname,
                         sizeof row->wname / sizeof row->wname[0]);
        }
        if (row->folder_count < PZ_WIN_MAX_FOLDERS) {
            copy_pz(row->folders[row->folder_count],
                    sizeof row->folders[0], &mod->folder);
            ++row->folder_count;
        }
        if (mod->time_updated > row->time_updated)
            row->time_updated = mod->time_updated;
        row->locked |= mod->locked != 0;
        row->imported |= mod->imported != 0;
    }
    pz_mod_manager_free_mods(&mods);

    ui->filter_total = ui->all_count;
    ui->filter_updates = 0;
    ui->filter_locked = 0;
    ui->filter_nontracked = 0;
    for (int i = 0; i < ui->all_count; ++i) {
        WinRow* row = &ui->all_rows[i];
        row->update_available = row->workshop_id != 0 &&
            pz_mod_manager_update_available(ui->manager, row->workshop_id);
        row->locked = row->locked ||
            (row->workshop_id != 0 &&
             pz_mod_manager_locked(ui->manager, row->workshop_id));
        if (row->update_available) ++ui->filter_updates;
        if (row->locked) ++ui->filter_locked;
        if (row->workshop_id == 0) ++ui->filter_nontracked;
    }
}

static int filter_available(WinUi* ui, WinFilter filter) {
    if (filter == WinFilterUpdates) return ui->filter_updates > 0;
    if (filter == WinFilterLocked) return ui->filter_locked > 0;
    if (filter == WinFilterNontracked) return ui->filter_nontracked > 0;
    return 1;
}

static void row_enabled_text(WinUi* ui, const WinRow* row, char* out,
                             size_t capacity) {
    out[0] = '\0';
    for (int t = 0; t < ui->target_count; ++t) {
        int enabled = 0;
        for (int f = 0; f < row->folder_count && !enabled; ++f) {
            pz_mod_info* info = get_info(ui, row->folders[f]);
            if (!info || info->id.length == 0) continue;
            for (int m = 0; m < row->mod_id_count && !enabled; ++m) {
                if (pz_str_equals(&info->id, row->mod_ids[m],
                                  strlen(row->mod_ids[m])))
                    enabled = 1;
            }
        }
        if (!enabled) continue;
        if (out[0]) str_append(out, capacity, ", ");
        char name[256];
        activation_name(&ui->targets[t], name, sizeof name);
        str_append(out, capacity, name);
    }
}

static void row_meta(WinUi* ui, WinRow* row, char* out, size_t capacity) {
    out[0] = '\0';
    if (row->folder_count == 1) str_append(out, capacity, row->folders[0]);
    if (row->imported) {
        if (out[0]) str_append(out, capacity, "  |  ");
        str_append(out, capacity, "Imported");
    }
    if (row->locked) {
        if (out[0]) str_append(out, capacity, "  |  ");
        str_append(out, capacity, "Locked");
    }
    char on[512];
    row_enabled_text(ui, row, on, sizeof on);
    if (on[0]) {
        if (out[0]) str_append(out, capacity, "  |  ");
        str_append(out, capacity, "On: ");
        str_append(out, capacity, on);
    }
    char date[32];
    format_date(row->time_updated, date, sizeof date);
    if (date[0]) {
        if (out[0]) str_append(out, capacity, "  |  ");
        str_append(out, capacity, "Updated ");
        str_append(out, capacity, date);
    }
    pz_mod_info* info = get_info(ui, row->folders[0]);
    if (info && !pz_mod_info_supports_game_version(
                    info, ui->game_version, strlen(ui->game_version))) {
        pz_str range;
        pz_str_init(&range);
        if (pz_mod_info_version_range(info, &range)) {
            if (out[0]) str_append(out, capacity, "  |  ");
            str_append_n(out, capacity, range.data, range.length);
        }
        pz_str_free(&range);
    }
}

static void filter_rows(WinUi* ui) {
    if (!filter_available(ui, ui->filter_mode)) ui->filter_mode = WinFilterAll;
    char needle[256];
    read_edit(g_ui.filter, needle, sizeof needle);

    reserve_rows(&ui->rows, &ui->row_capacity, ui->all_count);
    ui->row_count = 0;
    for (int i = 0; i < ui->all_count; ++i) {
        WinRow* row = &ui->all_rows[i];
        if (ui->filter_mode == WinFilterUpdates && !row->update_available) continue;
        if (ui->filter_mode == WinFilterLocked && !row->locked) continue;
        if (ui->filter_mode == WinFilterNontracked && row->workshop_id != 0)
            continue;
        if (needle[0]) {
            char haystack[1024];
            str_copy(haystack, sizeof haystack, row->name);
            for (int f = 0; f < row->folder_count; ++f) {
                str_append_char(haystack, sizeof haystack, ' ');
                str_append(haystack, sizeof haystack, row->folders[f]);
            }
            if (!text_contains_ci(haystack, needle)) continue;
        }
        if (ui->row_count >= ui->row_capacity) break;
        ui->rows[ui->row_count++] = *row;
    }

    for (int i = 1; i < ui->row_count; ++i) {
        WinRow value = ui->rows[i];
        int j = i;
        while (j > 0) {
            const WinRow* previous = &ui->rows[j - 1];
            int before;
            if (ui->sort_mode && value.time_updated != previous->time_updated)
                before = value.time_updated > previous->time_updated;
            else
                before = ascii_less(value.name, previous->name);
            if (!before) break;
            ui->rows[j] = ui->rows[j - 1];
            --j;
        }
        ui->rows[j] = value;
    }

    for (int i = 0; i < ui->row_count; ++i) {
        WinRow* row = &ui->rows[i];
        pz_mod_info* info = get_info(ui, row->folders[0]);
        const char* title = (info && info->name.length)
            ? info->name.data
            : (row->name[0] ? row->name : row->folders[0]);
        utf8_to_wide(title, row->wname, sizeof row->wname / sizeof row->wname[0]);
        row->folder[0] = '\0';
        str_copy(row->folder, sizeof row->folder, row->folders[0]);
        row->icon_path[0] = '\0';
        if (info && info->icon.length)
            str_copy_n(row->icon_path, sizeof row->icon_path, info->icon.data,
                       info->icon.length);

        row->mod_id_count = 0;
        for (int f = 0; f < row->folder_count; ++f) {
            pz_mod_info* folder_info = get_info(ui, row->folders[f]);
            if (!folder_info || folder_info->id.length == 0) continue;
            int duplicate = 0;
            for (int m = 0; m < row->mod_id_count; ++m)
                if (pz_str_equals(&folder_info->id, row->mod_ids[m],
                                  strlen(row->mod_ids[m])))
                    duplicate = 1;
            if (!duplicate && row->mod_id_count < PZ_WIN_MAX_FOLDERS) {
                str_copy_n(row->mod_ids[row->mod_id_count],
                           sizeof row->mod_ids[0], folder_info->id.data,
                           folder_info->id.length);
                ++row->mod_id_count;
            }
        }
        row->enabled = 0;
        for (int t = 0; t < ui->target_count && !row->enabled; ++t)
            for (int m = 0; m < row->mod_id_count && !row->enabled; ++m) {
                pz_str id;
                pz_str_init(&id);
                pz_str_assign_cstr(&id, row->mod_ids[m]);
                row->enabled = target_has_id(&ui->targets[t], &id);
                pz_str_free(&id);
            }
        char meta[1024];
        row_meta(ui, row, meta, sizeof meta);
        utf8_to_wide(meta, row->wmeta,
                     sizeof row->wmeta / sizeof row->wmeta[0]);
    }
}

/* ------------------------------------------------------------- list procs */

static void sync_selection(WinUi* ui);

static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ui.busy && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
        const int mx = (short)LOWORD(lp);
        const int my = (short)HIWORD(lp);
        const LRESULT hit =
            SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(mx, my));
        const int item = (int)LOWORD(hit);
        if (HIWORD(hit) == 0 && item >= 0 && item < g_ui.row_count) {
            RECT item_rc;
            if (SendMessageW(hwnd, LB_GETITEMRECT, item, (LPARAM)&item_rc) !=
                LB_ERR) {
                if (msg == WM_LBUTTONDOWN &&
                    (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                    const LRESULT selected =
                        SendMessageW(hwnd, LB_GETSEL, item, 0);
                    SendMessageW(hwnd, LB_SETSEL, selected ? FALSE : TRUE,
                                 item);
                    SendMessageW(hwnd, LB_SETANCHORINDEX, item, 0);
                    SendMessageW(hwnd, LB_SETCARETINDEX, item, FALSE);
                    SetFocus(hwnd);
                    sync_selection(&g_ui);
                    InvalidateRect(hwnd, &item_rc, FALSE);
                    return 0;
                }
            }
        }
    }
    const LRESULT result =
        CallWindowProcW(g_ui.list_old_proc, hwnd, msg, wp, lp);
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

static LRESULT CALLBACK LogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const LRESULT result =
        CallWindowProcW(g_ui.log_old_proc, hwnd, msg, wp, lp);
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

/* ------------------------------------------------------------ selection */

static void collect_selected(WinUi* ui) {
    ui->selected_count = 0;
    ui->selected_folder_count = 0;
    if (!ui->list) return;

    int count = (int)SendMessageW(ui->list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) {
        const int sel = (int)SendMessageW(ui->list, LB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < ui->row_count) {
            if (ui->rows[sel].workshop_id != 0)
                ui->selected_ids[ui->selected_count++] =
                    ui->rows[sel].workshop_id;
            else
                str_copy(ui->selected_folders[ui->selected_folder_count++],
                         sizeof ui->selected_folders[0], ui->rows[sel].folder);
        }
        return;
    }
    if (count > PZ_WIN_MAX_ROWS) count = PZ_WIN_MAX_ROWS;
    int items[PZ_WIN_MAX_ROWS];
    const int got = (int)SendMessageW(ui->list, LB_GETSELITEMS, count,
                                      (LPARAM)items);
    for (int i = 0; i < got; ++i) {
        const int index = items[i];
        if (index < 0 || index >= ui->row_count) continue;
        if (ui->rows[index].workshop_id != 0) {
            int duplicate = 0;
            for (int s = 0; s < ui->selected_count; ++s)
                if (ui->selected_ids[s] == ui->rows[index].workshop_id)
                    duplicate = 1;
            if (!duplicate && ui->selected_count < PZ_WIN_MAX_IDS)
                ui->selected_ids[ui->selected_count++] =
                    ui->rows[index].workshop_id;
        } else {
            int duplicate = 0;
            for (int s = 0; s < ui->selected_folder_count; ++s)
                if (strcmp(ui->selected_folders[s], ui->rows[index].folder) == 0)
                    duplicate = 1;
            if (!duplicate && ui->selected_folder_count < PZ_WIN_MAX_IDS)
                str_copy(ui->selected_folders[ui->selected_folder_count++],
                         sizeof ui->selected_folders[0], ui->rows[index].folder);
        }
    }
}

static void sync_selection(WinUi* ui) {
    collect_selected(ui);
    update_actions();
    set_details();
}

static int row_selected(WinUi* ui, const WinRow* row) {
    if (row->workshop_id != 0) {
        for (int i = 0; i < ui->selected_count; ++i)
            if (ui->selected_ids[i] == row->workshop_id) return 1;
        return 0;
    }
    for (int i = 0; i < ui->selected_folder_count; ++i)
        if (strcmp(ui->selected_folders[i], row->folder) == 0) return 1;
    return 0;
}

static void set_details(void) {
    wchar_t text[1024];
    text[0] = L'\0';
    int names = 0;
    for (int i = 0; i < g_ui.row_count; ++i) {
        if (!row_selected(&g_ui, &g_ui.rows[i])) continue;
        if (names) wappend(text, sizeof text / sizeof text[0], L", ");
        wappend(text, sizeof text / sizeof text[0], g_ui.rows[i].wname);
        ++names;
    }
    if (!names) wcopy(text, sizeof text / sizeof text[0], L"No selection");
    SetWindowTextW(g_ui.details, text);
}

static void update_actions(void) {
    const int has_ids = g_ui.selected_count != 0;
    const int has_any = has_ids || g_ui.selected_folder_count != 0;
    if (g_ui.add) EnableWindow(g_ui.add, !g_ui.busy);
    if (g_ui.import) EnableWindow(g_ui.import, !g_ui.busy);
    if (g_ui.update_all) EnableWindow(g_ui.update_all, !g_ui.busy);
    if (g_ui.settings_btn) EnableWindow(g_ui.settings_btn, !g_ui.busy);
    if (g_ui.self_update) {
        ShowWindow(g_ui.self_update,
                   g_ui.app_update_available ? SW_SHOW : SW_HIDE);
        EnableWindow(g_ui.self_update,
                     !g_ui.busy && g_ui.app_update_available);
    }
    if (g_ui.share_server_btn)
        EnableWindow(g_ui.share_server_btn, !g_ui.busy);
    if (g_ui.select_all)
        EnableWindow(g_ui.select_all, !g_ui.busy && g_ui.row_count != 0);
    if (g_ui.clear) EnableWindow(g_ui.clear, !g_ui.busy && has_any);
    if (g_ui.copy) EnableWindow(g_ui.copy, !g_ui.busy && has_ids);
    if (g_ui.lock) {
        int all_locked = has_ids;
        for (int i = 0; i < g_ui.selected_count; ++i)
            if (!pz_mod_manager_manually_locked(g_ui.manager,
                                                g_ui.selected_ids[i])) {
                all_locked = 0;
                break;
            }
        SetWindowTextW(g_ui.lock, all_locked ? L"Unlock" : L"Lock");
        EnableWindow(g_ui.lock, !g_ui.busy && has_ids);
    }
    if (g_ui.enable_btn)
        EnableWindow(g_ui.enable_btn, !g_ui.busy && has_any);
    if (g_ui.update) EnableWindow(g_ui.update, !g_ui.busy && has_ids);
    if (g_ui.remove) EnableWindow(g_ui.remove, !g_ui.busy && has_ids);
    if (g_ui.filter) EnableWindow(g_ui.filter, !g_ui.busy);
    if (g_ui.filter_combo) EnableWindow(g_ui.filter_combo, !g_ui.busy);
    update_filter_clear();
    update_input_clear();
    if (g_ui.sort) EnableWindow(g_ui.sort, !g_ui.busy);
    if (g_ui.check) EnableWindow(g_ui.check, !g_ui.busy);
}

static void update_filter_clear(void) {
    if (!g_ui.filter || !g_ui.filter_clear) return;
    const int has_text = GetWindowTextLengthW(g_ui.filter) > 0;
    ShowWindow(g_ui.filter_clear, has_text ? SW_SHOW : SW_HIDE);
    EnableWindow(g_ui.filter_clear, has_text && !g_ui.busy);
    InvalidateRect(g_ui.filter_clear, NULL, TRUE);
}

static void update_input_clear(void) {
    if (!g_ui.input || !g_ui.input_clear) return;
    const int has_text = GetWindowTextLengthW(g_ui.input) > 0;
    ShowWindow(g_ui.input_clear, has_text ? SW_SHOW : SW_HIDE);
    EnableWindow(g_ui.input_clear, has_text && !g_ui.busy);
    InvalidateRect(g_ui.input_clear, NULL, TRUE);
}

/* ---------------------------------------------------------------- filter */

static void refresh_filter_combo(void) {
    if (!g_ui.filter_combo) return;
    if (!filter_available(&g_ui, g_ui.filter_mode))
        g_ui.filter_mode = WinFilterAll;

    int count = 0;
    g_ui.filter_choices[count++] = WinFilterAll;
    if (g_ui.filter_updates > 0)
        g_ui.filter_choices[count++] = WinFilterUpdates;
    if (g_ui.filter_locked > 0)
        g_ui.filter_choices[count++] = WinFilterLocked;
    if (g_ui.filter_nontracked > 0)
        g_ui.filter_choices[count++] = WinFilterNontracked;
    g_ui.filter_choice_count = count;

    g_ui.filter_combo_updating = 1;
    SendMessageW(g_ui.filter_combo, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (int i = 0; i < count; ++i) {
        char label[64];
        str_copy(label, sizeof label, filter_name(g_ui.filter_choices[i]));
        str_append(label, sizeof label, " (");
        str_append_u64(label, sizeof label,
                       (uint64_t)filter_count(&g_ui, g_ui.filter_choices[i]));
        str_append(label, sizeof label, ")");
        wchar_t wide[64];
        utf8_to_wide(label, wide, sizeof wide / sizeof wide[0]);
        SendMessageW(g_ui.filter_combo, CB_ADDSTRING, 0, (LPARAM)wide);
        if (g_ui.filter_choices[i] == g_ui.filter_mode) selected = i;
    }
    SendMessageW(g_ui.filter_combo, CB_SETCURSEL, selected, 0);
    g_ui.filter_combo_updating = 0;
}

/* ----------------------------------------------------------------- list */

static void prune_selection(void) {
    uint64_t ids[PZ_WIN_MAX_IDS];
    int id_count = 0;
    for (int s = 0; s < g_ui.selected_count; ++s) {
        int found = 0;
        for (int i = 0; i < g_ui.row_count; ++i)
            if (g_ui.rows[i].workshop_id == g_ui.selected_ids[s]) found = 1;
        if (found) ids[id_count++] = g_ui.selected_ids[s];
    }
    char folders[PZ_WIN_MAX_IDS][256];
    int folder_count = 0;
    for (int s = 0; s < g_ui.selected_folder_count; ++s) {
        int found = 0;
        for (int i = 0; i < g_ui.row_count; ++i)
            if (g_ui.rows[i].workshop_id == 0 &&
                strcmp(g_ui.rows[i].folder, g_ui.selected_folders[s]) == 0)
                found = 1;
        if (found)
            str_copy(folders[folder_count++], sizeof folders[0],
                     g_ui.selected_folders[s]);
    }
    g_ui.selected_count = id_count;
    for (int i = 0; i < id_count; ++i) g_ui.selected_ids[i] = ids[i];
    g_ui.selected_folder_count = folder_count;
    for (int i = 0; i < folder_count; ++i)
        str_copy(g_ui.selected_folders[i], sizeof g_ui.selected_folders[0],
                 folders[i]);
}

static void refresh_list(void) {
    if (!g_ui.manager) return;
    SendMessageW(g_ui.list, LB_RESETCONTENT, 0, 0);
    sync_paths(&g_ui);
    load_targets(&g_ui);
    build_all_rows(&g_ui);
    refresh_filter_combo();
    filter_rows(&g_ui);
    prune_selection();

    for (int i = 0; i < g_ui.row_count; ++i)
        SendMessageW(g_ui.list, LB_ADDSTRING, 0,
                     (LPARAM)g_ui.rows[i].wname);

    for (int i = 0; i < g_ui.row_count; ++i) {
        if (row_selected(&g_ui, &g_ui.rows[i]))
            SendMessageW(g_ui.list, LB_SETSEL, TRUE, (LPARAM)i);
    }

    if (g_ui.empty) {
        char filter_text[256];
        read_edit(g_ui.filter, filter_text, sizeof filter_text);
        const int filtered =
            filter_text[0] != '\0' || g_ui.filter_mode != WinFilterAll;
        SetWindowTextW(g_ui.empty,
                       filtered ? L"No mods match the current filters."
                                : L"No mods installed yet. Paste a Workshop link above to begin.");
        ShowWindow(g_ui.empty, g_ui.row_count == 0 ? SW_SHOW : SW_HIDE);
    }
    SetWindowTextW(g_ui.sort, g_ui.sort_mode == 0 ? L"Sort: Name" : L"Sort: Updated");
    InvalidateRect(g_ui.sort, NULL, TRUE);
    sync_bars();
    update_actions();
    set_details();
    set_status(g_ui.dependency_warning[0] ? g_ui.dependency_warning : "Ready");
}

/* ----------------------------------------------------------------- log */

static void set_status(const char* text) {
    if (!g_ui.status) return;
    set_text_u8(g_ui.status, text);
    InvalidateRect(g_ui.status, NULL, TRUE);
}

static void append_log(const char* line) {
    if (!g_ui.log) return;
    wchar_t wide[PZ_WIN_MAX_LINE];
    utf8_to_wide(line, wide, sizeof wide / sizeof wide[0]);
    const int length = (int)wcslen(wide);
    if (length + 3 < (int)(sizeof wide / sizeof wide[0])) {
        wide[length] = L'\r';
        wide[length + 1] = L'\n';
        wide[length + 2] = L'\0';
    }
    const int len = GetWindowTextLengthW(g_ui.log);
    SendMessageW(g_ui.log, EM_SETSEL, len, len);
    SendMessageW(g_ui.log, EM_REPLACESEL, FALSE, (LPARAM)wide);
    sync_bars();
}

static void refresh_dependency_warning(void) {
    if (!g_ui.manager) return;
    pz_vec values;
    pz_vec_init(&values, sizeof(pz_missing_requirement));
    pz_mod_manager_missing_requirements(g_ui.manager, &values);

    char signature[1024];
    signature[0] = '\0';
    for (size_t i = 0; i < values.length; ++i) {
        const pz_missing_requirement* req =
            pz_vec_at_const(&values, i);
        str_append_n(signature, sizeof signature, req->folder.data,
                     req->folder.length);
        str_append_char(signature, sizeof signature, '\t');
        str_append_n(signature, sizeof signature, req->required_id.data,
                     req->required_id.length);
        str_append_char(signature, sizeof signature, '\n');
    }
    if (strcmp(signature, g_ui.dependency_signature) != 0) {
        if (values.length == 0) {
            if (g_ui.dependency_signature[0]) append_log("required mods satisfied");
        } else {
            for (size_t i = 0; i < values.length; ++i) {
                const pz_missing_requirement* req =
                    pz_vec_at_const(&values, i);
                char line[1024];
                str_copy(line, sizeof line, "warning: ");
                str_append_n(line, sizeof line, req->mod_name.data,
                             req->mod_name.length);
                str_append(line, sizeof line, " requires ");
                str_append_n(line, sizeof line, req->required_id.data,
                             req->required_id.length);
                str_append(line, sizeof line, " (not installed)");
                append_log(line);
            }
        }
        str_copy(g_ui.dependency_signature, sizeof g_ui.dependency_signature,
                 signature);
    }
    if (values.length == 0) {
        g_ui.dependency_warning[0] = '\0';
    } else if (values.length == 1) {
        const pz_missing_requirement* req = pz_vec_at_const(&values, 0);
        str_copy(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                 "Missing required mod: ");
        str_append_n(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                     req->required_id.data, req->required_id.length);
        str_append(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                   " (needed by ");
        str_append_n(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                     req->mod_name.data, req->mod_name.length);
        str_append(g_ui.dependency_warning, sizeof g_ui.dependency_warning, ")");
    } else {
        str_copy(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                 "Missing ");
        str_append_u64(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                       (uint64_t)values.length);
        str_append(g_ui.dependency_warning, sizeof g_ui.dependency_warning,
                   " required mods - see Activity");
    }
    pz_mod_manager_free_requirements(&values);
    set_status(g_ui.dependency_warning[0] ? g_ui.dependency_warning : "Ready");
}

/* -------------------------------------------------------------- commands */

static void push_pending(const char* line) {
    EnterCriticalSection(&g_ui.pending_mu);
    if (g_ui.pending_count < PZ_WIN_MAX_PENDING) {
        str_copy(g_ui.pending[g_ui.pending_count++], PZ_WIN_MAX_LINE, line);
    }
    LeaveCriticalSection(&g_ui.pending_mu);
}

static void worker_log(void* context, const char* line) {
    (void)context;
    push_pending(line);
    PostMessageW(g_ui.hwnd, PZ_WIN_WM_LOG, 0, 0);
}

static void drain_pending(void) {
    for (;;) {
        char line[PZ_WIN_MAX_LINE];
        EnterCriticalSection(&g_ui.pending_mu);
        if (g_ui.pending_count == 0) {
            LeaveCriticalSection(&g_ui.pending_mu);
            break;
        }
        str_copy(line, sizeof line, g_ui.pending[0]);
        memmove(&g_ui.pending[0], &g_ui.pending[1],
                (size_t)(g_ui.pending_count - 1) * PZ_WIN_MAX_LINE);
        --g_ui.pending_count;
        LeaveCriticalSection(&g_ui.pending_mu);
        append_log(line);
    }
}

static DWORD WINAPI worker_thread(LPVOID parameter) {
    (void)parameter;
    pz_log log;
    pz_log_init(&log);
    pz_log_set(&log, worker_log, &g_ui);
    pz_mm_result result = pz_command_run(
        g_ui.manager, g_ui.worker_line, strlen(g_ui.worker_line), &log);
    char message[PZ_WIN_MAX_LINE];
    str_copy(message, sizeof message, result.ok ? "OK: " : "ERROR: ");
    str_append_n(message, sizeof message, result.message.data,
                 result.message.length);
    push_pending(message);
    pz_mm_result_free(&result);
    PostMessageW(g_ui.hwnd, PZ_WIN_WM_DONE, 0, 0);
    return 0;
}

static void start_command(const char* line) {
    if (g_ui.busy || !line || !*line) return;
    char shown[PZ_WIN_MAX_LINE];
    str_copy(shown, sizeof shown, "> ");
    str_append(shown, sizeof shown, line);
    append_log(shown);
    g_ui.busy = 1;
    EnableWindow(g_ui.input, FALSE);
    EnableWindow(g_ui.list, FALSE);
    EnableWindow(g_ui.filter, FALSE);
    update_actions();
    update_count_label();
    set_status("Working - steamcmd may take a while...");

    if (g_ui.worker) {
        WaitForSingleObject(g_ui.worker, INFINITE);
        CloseHandle(g_ui.worker);
        g_ui.worker = NULL;
    }
    str_copy(g_ui.worker_line, sizeof g_ui.worker_line, line);
    g_ui.worker = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    if (!g_ui.worker) {
        g_ui.busy = 0;
        EnableWindow(g_ui.input, TRUE);
        EnableWindow(g_ui.list, TRUE);
        update_actions();
        update_count_label();
        set_status("Ready");
        append_log("ERROR: cannot start worker");
    }
}

static void add_input(void) {
    if (g_ui.busy) return;
    char id[PZ_WIN_MAX_LINE];
    read_edit(g_ui.input, id, sizeof id);
    if (!id[0]) {
        set_status("Enter one or more Workshop URLs or IDs first");
        SetFocus(g_ui.input);
        return;
    }
    SetWindowTextW(g_ui.input, L"");
    update_input_clear();
    const int lock_command =
        strncmp(id, "lock ", 5) == 0 || strncmp(id, "unlock ", 7) == 0;
    char command[PZ_WIN_MAX_LINE];
    command[0] = '\0';
    if (!lock_command) str_append(command, sizeof command, "add ");
    str_append(command, sizeof command, id);
    start_command(command);
}

static void update_selected(void) {
    if (g_ui.busy) return;
    collect_selected(&g_ui);
    if (g_ui.selected_count == 0) {
        append_log("Select a mod first.");
        return;
    }
    char command[PZ_WIN_MAX_LINE];
    str_copy(command, sizeof command, "update");
    for (int i = 0; i < g_ui.selected_count; ++i) {
        str_append_char(command, sizeof command, ' ');
        str_append_u64(command, sizeof command, g_ui.selected_ids[i]);
    }
    start_command(command);
}

static void toggle_lock_selected(void) {
    if (g_ui.busy) return;
    collect_selected(&g_ui);
    if (g_ui.selected_count == 0) {
        append_log("Select a mod first.");
        return;
    }
    int all_locked = 1;
    for (int i = 0; i < g_ui.selected_count; ++i)
        if (!pz_mod_manager_manually_locked(g_ui.manager,
                                            g_ui.selected_ids[i])) {
            all_locked = 0;
            break;
        }
    char command[PZ_WIN_MAX_LINE];
    str_copy(command, sizeof command, all_locked ? "unlock" : "lock");
    for (int i = 0; i < g_ui.selected_count; ++i) {
        str_append_char(command, sizeof command, ' ');
        str_append_u64(command, sizeof command, g_ui.selected_ids[i]);
    }
    start_command(command);
}

static void remove_selected(void) {
    if (g_ui.busy) return;
    collect_selected(&g_ui);
    if (g_ui.selected_count == 0) {
        append_log("Select a mod first.");
        return;
    }

    wchar_t message[1024];
    if (g_ui.selected_count == 1) {
        const wchar_t* name = L"this mod";
        for (int i = 0; i < g_ui.row_count; ++i)
            if (g_ui.rows[i].workshop_id == g_ui.selected_ids[0])
                name = g_ui.rows[i].wname;
        wcopy(message, sizeof message / sizeof message[0],
              L"Remove this installed mod?\r\n\r\n");
        wappend(message, sizeof message / sizeof message[0], name);
    } else {
        wcopy(message, sizeof message / sizeof message[0], L"Remove ");
        wchar_t count[32];
        _snwprintf(count, sizeof count / sizeof count[0], L"%d",
                   g_ui.selected_count);
        wappend(message, sizeof message / sizeof message[0], count);
        wappend(message, sizeof message / sizeof message[0],
                L" selected mods?");
    }
    if (MessageBoxW(g_ui.hwnd, message, L"Remove mods",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    char command[PZ_WIN_MAX_LINE];
    str_copy(command, sizeof command, "remove");
    for (int i = 0; i < g_ui.selected_count; ++i) {
        str_append_char(command, sizeof command, ' ');
        str_append_u64(command, sizeof command, g_ui.selected_ids[i]);
    }
    start_command(command);
}

static void open_mods_folder(HWND owner) {
    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(g_ui.manager, &config);
    wchar_t path[PZ_WIN_MAX_PATH];
    utf8_to_wide(pz_str_cstr(&config.user_mods_dir), path,
                 sizeof path / sizeof path[0]);
    const HINSTANCE result = ShellExecuteW(owner, L"open", path, NULL, NULL,
                                           SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        wchar_t message[PZ_WIN_MAX_PATH];
        wcopy(message, sizeof message / sizeof message[0],
              L"Could not open the mods folder:\n");
        wappend(message, sizeof message / sizeof message[0], path);
        MessageBoxW(owner, message, L"Mods folder", MB_OK | MB_ICONERROR);
    }
    pz_paths_config_free(&config);
}

static int copy_plain_text(const char* value) {
    if (!value || !*value) return 0;
    wchar_t text[PZ_WIN_MAX_LINE];
    utf8_to_wide(value, text, sizeof text / sizeof text[0]);
    if (!OpenClipboard(g_ui.hwnd)) return 0;
    EmptyClipboard();
    int copied = 0;
    const SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            memcpy(target, text, bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != NULL;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

static int copy_workshop_ids(const uint64_t* ids, int count) {
    if (!ids || count <= 0) return 0;
    char text[PZ_WIN_MAX_IDS * 24];
    text[0] = '\0';
    for (int i = 0; i < count; ++i) {
        if (i) str_append_char(text, sizeof text, ' ');
        str_append_u64(text, sizeof text, ids[i]);
    }
    return copy_plain_text(text);
}

static void copy_selected_ids(void) {
    collect_selected(&g_ui);
    if (g_ui.selected_count == 0) return;
    if (copy_workshop_ids(g_ui.selected_ids, g_ui.selected_count)) {
        char message[256];
        str_copy(message, sizeof message, "Copied ");
        str_append_u64(message, sizeof message, (uint64_t)g_ui.selected_count);
        str_append(message, sizeof message,
                   " Workshop ID(s). Paste them into Add, or type lock first.");
        append_log(message);
    } else {
        append_log("Could not write to the clipboard.");
    }
}

/* -------------------------------------------------------------- drawing */

static int glyph_for_id(int id) {
    switch (id) {
    case PZ_WIN_ID_ADD: return PzGlyphInstall;
    case PZ_WIN_ID_IMPORT:
    case PZ_WIN_ID_OPEN_MODS: return PzGlyphFolder;
    case PZ_WIN_ID_UPDATE:
    case PZ_WIN_ID_SELF_UPDATE: return PzGlyphUpdate;
    case PZ_WIN_ID_UPDATE_ALL: return PzGlyphUpdateAll;
    case PZ_WIN_ID_REMOVE: return PzGlyphRemove;
    case PZ_WIN_ID_SELECT_ALL: return PzGlyphSelectAll;
    case PZ_WIN_ID_CLEAR:
    case PZ_WIN_ID_SET_CANCEL:
    case PZ_WIN_ID_TGT_DISABLE:
    case PZ_WIN_ID_TGT_CLOSE: return PzGlyphClear;
    case PZ_WIN_ID_COPY:
    case PZ_WIN_ID_TGT_COPY:
    case PZ_WIN_ID_TGT_COPY_NAMES: return PzGlyphCopy;
    case PZ_WIN_ID_LOCK: return PzGlyphLock;
    case PZ_WIN_ID_FILTER_MODE: return PzGlyphFilter;
    case PZ_WIN_ID_SORT: return PzGlyphSort;
    case PZ_WIN_ID_SETTINGS: return PzGlyphSettings;
    case PZ_WIN_ID_SET_SAVE: return PzGlyphSave;
    case PZ_WIN_ID_SET_THEME: return PzGlyphTheme;
    case PZ_WIN_ID_CHECK: return PzGlyphRefresh;
    case PZ_WIN_ID_ENABLE:
    case PZ_WIN_ID_TGT_ENABLE: return PzGlyphCheck;
    case PZ_WIN_ID_SHARE_SERVER: return PzGlyphServer;
    default: return -1;
    }
}

static void draw_button_icon(HDC dc, int id, int x, int cy, COLORREF color) {
    const int glyph = glyph_for_id(id);
    if (glyph < 0 || glyph >= PzGlyphCount) return;
    HBRUSH brush = CreateSolidBrush(color);
    const int origin_x = x + 1;
    const int origin_y = cy - 8;
    for (int row = 0; row < 16; ++row) {
        const char* pixels = pz_glyphs[glyph].rows[row];
        for (int column = 0; column < 16 && pixels[column]; ++column) {
            if (pixels[column] != '#') continue;
            RECT pixel = {origin_x + column, origin_y + row,
                          origin_x + column + 1, origin_y + row + 1};
            FillRect(dc, &pixel, brush);
        }
    }
    DeleteObject(brush);
}

static void draw_flat_button(const DRAWITEMSTRUCT* dis, int id, int primary) {
    if (!dis) return;
    const int disabled = (dis->itemState & ODS_DISABLED) != 0;
    const int pressed = (dis->itemState & ODS_SELECTED) != 0;
    RECT r = dis->rcItem;
    HBRUSH fill_brush = g_ui.card_brush;
    HBRUSH border_brush = g_ui.border_brush;
    COLORREF text_color = g_ui.theme.text;
    COLORREF icon_color = g_ui.theme.brand;
    if (disabled) {
        fill_brush = g_ui.bg_brush;
        text_color = g_ui.theme.muted;
        icon_color = g_ui.theme.muted;
    } else if (primary) {
        fill_brush = g_ui.accent_brush;
        border_brush = g_ui.accent_brush;
        text_color = g_ui.theme.on_accent;
        icon_color = g_ui.theme.on_accent;
    } else if (id == PZ_WIN_ID_SELF_UPDATE) {
        fill_brush = g_ui.brand_brush;
        border_brush = g_ui.brand_brush;
        text_color = g_ui.dark_mode ? g_ui.theme.bg : g_ui.theme.cream;
        icon_color = text_color;
    } else if (pressed) {
        fill_brush = g_ui.brand_soft_brush;
        border_brush = g_ui.brand_brush;
        icon_color = g_ui.theme.brand;
    }
    FillRect(dis->hDC, &r, fill_brush);
    FrameRect(dis->hDC, &r, border_brush);

    wchar_t label[128];
    label[0] = L'\0';
    GetWindowTextW(dis->hwndItem, label, 127);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text_color);
    HGDIOBJ old = SelectObject(dis->hDC, g_ui.font_body);
    SIZE size;
    GetTextExtentPoint32W(dis->hDC, label, lstrlenW(label), &size);
    const int icon_w = 18;
    const int gap = 7;
    const int start = r.left + (r.right - r.left - icon_w - gap - size.cx) / 2;
    draw_button_icon(dis->hDC, id, start, (r.top + r.bottom) / 2, icon_color);
    RECT text_rc = {start + icon_w + gap, r.top, r.right - 4, r.bottom};
    DrawTextW(dis->hDC, label, -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

static void draw_checkbox(HDC dc, int x, int y, int checked) {
    RECT box = {x, y, x + 18, y + 18};
    fill_color(dc, &box, checked ? g_ui.theme.accent : g_ui.theme.field);
    HBRUSH frame = CreateSolidBrush(checked ? g_ui.theme.accent
                                            : g_ui.theme.border_strong);
    FrameRect(dc, &box, frame);
    DeleteObject(frame);
    if (!checked) return;
    HPEN pen = CreatePen(PS_SOLID, 2, g_ui.theme.on_accent);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x + 4, y + 9, NULL);
    LineTo(dc, x + 7, y + 12);
    LineTo(dc, x + 14, y + 4);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void draw_checkbox_button(const DRAWITEMSTRUCT* dis, int checked) {
    if (!dis) return;
    RECT r = dis->rcItem;
    FillRect(dis->hDC, &r, g_ui.card_brush);
    draw_checkbox(dis->hDC, r.left, r.top + (r.bottom - r.top - 18) / 2,
                  checked);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_ui.theme.text);
    HGDIOBJ old_font = SelectObject(dis->hDC, g_ui.font_body);
    wchar_t label[128];
    label[0] = L'\0';
    GetWindowTextW(dis->hwndItem, label, 127);
    RECT text = {r.left + 28, r.top, r.right, r.bottom};
    DrawTextW(dis->hDC, label, -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old_font);
}

static void draw_filter_clear(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    const int pressed = (dis->itemState & ODS_SELECTED) != 0;
    RECT r = dis->rcItem;
    fill_color(dis->hDC, &r, pressed ? g_ui.theme.hover : g_ui.theme.field);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, pressed ? g_ui.theme.accent : g_ui.theme.muted);
    HGDIOBJ old = SelectObject(dis->hDC, g_ui.font_small);
    DrawTextW(dis->hDC, L"X", -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

static void draw_dialog_button(const DRAWITEMSTRUCT* dis, int primary) {
    if (!dis) return;
    const int disabled = (dis->itemState & ODS_DISABLED) != 0;
    const int pressed = (dis->itemState & ODS_SELECTED) != 0;
    RECT r = dis->rcItem;
    COLORREF fill = g_ui.theme.surface;
    COLORREF border = g_ui.theme.border;
    COLORREF text = g_ui.theme.text;
    if (disabled) {
        fill = g_ui.theme.bg;
        text = g_ui.theme.muted;
    } else if (primary) {
        fill = pressed ? g_ui.theme.accent_hover : g_ui.theme.accent;
        border = fill;
        text = g_ui.theme.on_accent;
    } else if (pressed) {
        fill = g_ui.theme.hover;
        border = g_ui.theme.brand_border;
    }
    fill_color(dis->hDC, &r, fill);
    HBRUSH frame = CreateSolidBrush(border);
    FrameRect(dis->hDC, &r, frame);
    DeleteObject(frame);

    wchar_t label[128];
    label[0] = L'\0';
    GetWindowTextW(dis->hwndItem, label, 127);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, text);
    HGDIOBJ old = SelectObject(dis->hDC, g_ui.font_body);
    SIZE size;
    GetTextExtentPoint32W(dis->hDC, label, lstrlenW(label), &size);
    const int icon_w = 18;
    const int gap = 7;
    const int start = r.left + (r.right - r.left - icon_w - gap - size.cx) / 2;
    draw_button_icon(dis->hDC, (int)dis->CtlID, start, (r.top + r.bottom) / 2,
                     text);
    RECT text_rc = {start + icon_w + gap, r.top, r.right - 4, r.bottom};
    DrawTextW(dis->hDC, label, -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old);
}

static void draw_filter_combo_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) return;
    wchar_t label[160];
    label[0] = L'\0';
    SendMessageW(dis->hwndItem, CB_GETLBTEXT, (WPARAM)dis->itemID,
                 (LPARAM)label);
    const int edit = (dis->itemState & ODS_COMBOBOXEDIT) != 0;
    const int selected = (dis->itemState & ODS_SELECTED) != 0;
    RECT r = dis->rcItem;
    HBRUSH background = edit ? g_ui.card_brush
        : (selected ? g_ui.brand_soft_brush : g_ui.bg_brush);
    FillRect(dis->hDC, &r, background);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC,
                 selected ? g_ui.theme.brand : g_ui.theme.text);
    HGDIOBJ old = SelectObject(dis->hDC, g_ui.font_small);
    int text_left = r.left + 5;
    if (edit) {
        draw_button_icon(dis->hDC, PZ_WIN_ID_FILTER_MODE, r.left + 4,
                         (r.top + r.bottom) / 2, g_ui.theme.brand);
        text_left += 18 + 6;
    }
    RECT text_rc = {text_left, r.top, r.right - 4, r.bottom};
    DrawTextW(dis->hDC, label, -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
    SelectObject(dis->hDC, old);
}

static void draw_list_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) return;
    const int index = (int)dis->itemID;
    if (index < 0 || index >= g_ui.row_count) return;
    WinRow* row = &g_ui.rows[index];

    const int selected = (dis->itemState & ODS_SELECTED) != 0;
    RECT card = dis->rcItem;
    card.bottom = card.top + PZ_WIN_ROW_H;
    FillRect(dis->hDC, &card, selected ? g_ui.selected_brush : g_ui.card_brush);
    FrameRect(dis->hDC, &card,
              selected ? g_ui.accent_brush : g_ui.border_brush);

    const int has_update = row->update_available;
    const int is_locked = row->locked;
    const int show_badge = has_update || is_locked;
    const int switchable = g_ui.target_count != 0;
    RECT icon_rc = {card.left + 10, card.top + 8, card.left + 58,
                    card.top + 56};
    int drew_icon = 0;
    WinIcon* icon = icon_for(&g_ui, row);
    if (icon && icon->bitmap) {
        HDC memory = CreateCompatibleDC(dis->hDC);
        HGDIOBJ old = SelectObject(memory, icon->bitmap);
        BitBlt(dis->hDC, icon_rc.left, icon_rc.top, 48, 48, memory, 0, 0,
               SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
        drew_icon = 1;
    }
    if (!drew_icon) {
        HBRUSH soft = CreateSolidBrush(g_ui.theme.accent_soft);
        FillRect(dis->hDC, &icon_rc, soft);
        DeleteObject(soft);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, g_ui.theme.accent);
        HGDIOBJ old = SelectObject(dis->hDC, g_ui.font_title);
        wchar_t initial[2] = {row->wname[0], L'\0'};
        DrawTextW(dis->hDC, initial, -1, &icon_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
    }

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_ui.theme.text);
    HGDIOBJ old_font = SelectObject(dis->hDC, g_ui.font_row);
    const int pill_w = 64;
    const int pill_gap = 6;
    const int pill_margin = 12;
    const int on_left = card.right - pill_margin - pill_w;
    const int update_left = switchable ? on_left - pill_gap - pill_w : on_left;
    const int text_right = (show_badge ? update_left
                                       : switchable ? on_left
                                                    : card.right - pill_margin) -
                           8;
    RECT name_rc = {card.left + 72, card.top + 7, text_right, card.top + 34};
    DrawTextW(dis->hDC, row->wname, -1, &name_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    SelectObject(dis->hDC, g_ui.font_small);
    SetTextColor(dis->hDC, g_ui.theme.muted);
    RECT meta_rc = {card.left + 72, card.top + 34, text_right, card.top + 57};
    DrawTextW(dis->hDC, row->wmeta, -1, &meta_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    if (switchable) {
        RECT pill = {on_left, card.top + 22, on_left + pill_w, card.top + 42};
        HBRUSH fill = CreateSolidBrush(row->enabled ? g_ui.theme.success
                                                     : g_ui.theme.surface);
        FillRect(dis->hDC, &pill, fill);
        DeleteObject(fill);
        if (!row->enabled) FrameRect(dis->hDC, &pill, g_ui.border_brush);
        SetTextColor(dis->hDC, row->enabled ? g_ui.theme.on_accent
                                            : g_ui.theme.muted);
        DrawTextW(dis->hDC, row->enabled ? L"ON" : L"OFF", -1, &pill,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (show_badge) {
        RECT pill = {update_left, card.top + 22, update_left + pill_w,
                     card.top + 42};
        HBRUSH fill = CreateSolidBrush(is_locked ? g_ui.theme.surface
                                                  : g_ui.theme.accent);
        FillRect(dis->hDC, &pill, fill);
        DeleteObject(fill);
        if (is_locked) FrameRect(dis->hDC, &pill, g_ui.border_strong_brush);
        SetTextColor(dis->hDC, is_locked ? g_ui.theme.muted
                                          : g_ui.theme.on_accent);
        DrawTextW(dis->hDC, is_locked ? L"LOCKED" : L"UPDATE", -1, &pill,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dis->hDC, old_font);
}

/* --------------------------------------------------------- edit subclass */

static WinEditPlaceholder* find_edit(HWND edit) {
    for (int i = 0; i < g_ui.edit_count; ++i)
        if (g_ui.edits[i].edit == edit) return &g_ui.edits[i];
    return NULL;
}

static void sync_edit_cue_caret(HWND edit) {
    if (GetFocus() != edit) return;
    if (GetWindowTextLengthW(edit) == 0)
        HideCaret(edit);
    else
        ShowCaret(edit);
}

static void draw_edit_placeholder(HWND edit, const wchar_t* text) {
    if (!text || GetWindowTextLengthW(edit) != 0) return;
    HDC dc = GetDC(edit);
    if (!dc) return;
    RECT rc;
    SendMessageW(edit, EM_GETRECT, 0, (LPARAM)&rc);
    const int old_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_color = SetTextColor(dc, g_ui.theme.muted);
    HFONT font = (HFONT)SendMessageW(edit, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = font ? SelectObject(dc, font) : NULL;
    DrawTextW(dc, text, -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    if (old_font) SelectObject(dc, old_font);
    SetTextColor(dc, old_color);
    SetBkMode(dc, old_mode);
    ReleaseDC(edit, dc);
}

static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinEditPlaceholder* placeholder = find_edit(hwnd);
    if (!placeholder || !placeholder->old_proc)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_PAINT: {
        const LRESULT result =
            CallWindowProcW(placeholder->old_proc, hwnd, msg, wp, lp);
        draw_edit_placeholder(hwnd, placeholder->text);
        sync_edit_cue_caret(hwnd);
        return result;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT: {
        const LRESULT result =
            CallWindowProcW(placeholder->old_proc, hwnd, msg, wp, lp);
        sync_edit_cue_caret(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return result;
    }
    case WM_PASTE: {
        if (hwnd == g_ui.input && OpenClipboard(hwnd)) {
            HANDLE data = GetClipboardData(CF_UNICODETEXT);
            if (data) {
                const wchar_t* raw = (const wchar_t*)GlobalLock(data);
                if (raw) {
                    wchar_t text[PZ_WIN_MAX_LINE];
                    wcopy(text, sizeof text / sizeof text[0], raw);
                    GlobalUnlock(data);
                    for (size_t i = 0; text[i]; ++i)
                        if (text[i] == L'\r' || text[i] == L'\n' ||
                            text[i] == L'\t')
                            text[i] = L' ';
                    SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)text);
                    CloseClipboard();
                    return 0;
                }
            }
            CloseClipboard();
        }
        return CallWindowProcW(placeholder->old_proc, hwnd, msg, wp, lp);
    }
    case WM_CHAR:
    case WM_LBUTTONUP: {
        const LRESULT result =
            CallWindowProcW(placeholder->old_proc, hwnd, msg, wp, lp);
        sync_edit_cue_caret(hwnd);
        return result;
    }
    case WM_NCDESTROY: {
        const LRESULT result =
            CallWindowProcW(placeholder->old_proc, hwnd, msg, wp, lp);
        for (int i = 0; i < g_ui.edit_count; ++i) {
            if (g_ui.edits[i].edit == hwnd) {
                memmove(&g_ui.edits[i], &g_ui.edits[i + 1],
                        (size_t)(g_ui.edit_count - i - 1) *
                            sizeof g_ui.edits[0]);
                --g_ui.edit_count;
                break;
            }
        }
        return result;
    }
    default:
        return CallWindowProcW(placeholder->old_proc, hwnd, msg, wp, lp);
    }
}

static void set_edit_placeholder(HWND edit, const wchar_t* text) {
    WinEditPlaceholder* placeholder = find_edit(edit);
    if (placeholder) {
        placeholder->text = text;
        InvalidateRect(edit, NULL, TRUE);
        return;
    }
    if (g_ui.edit_count >= PZ_WIN_MAX_EDITS) return;
    WNDPROC old = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(
        edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    if (!old) return;
    WinEditPlaceholder* entry = &g_ui.edits[g_ui.edit_count++];
    entry->edit = edit;
    entry->old_proc = old;
    entry->text = text;
    InvalidateRect(edit, NULL, TRUE);
}

/* --------------------------------------------------------------- watcher */

static void stop_watcher(void) {
    if (g_ui.watch_stop) SetEvent(g_ui.watch_stop);
    if (g_ui.watcher) {
        WaitForSingleObject(g_ui.watcher, INFINITE);
        CloseHandle(g_ui.watcher);
        g_ui.watcher = NULL;
    }
    if (g_ui.watch_change) {
        FindCloseChangeNotification(g_ui.watch_change);
        g_ui.watch_change = NULL;
    }
    if (g_ui.watch_stop) {
        CloseHandle(g_ui.watch_stop);
        g_ui.watch_stop = NULL;
    }
}

static DWORD WINAPI watcher_thread(LPVOID parameter) {
    (void)parameter;
    HANDLE handles[2] = {g_ui.watch_change, g_ui.watch_stop};
    for (;;) {
        const DWORD result =
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 + 1) break;
        if (result != WAIT_OBJECT_0) break;
        if (g_ui.hwnd) PostMessageW(g_ui.hwnd, PZ_WIN_WM_RELOAD, 0, 0);
        if (!FindNextChangeNotification(g_ui.watch_change)) break;
    }
    return 0;
}

static int start_watcher(const char* dir) {
    stop_watcher();
    g_ui.watch_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ui.watch_stop) return 0;
    wchar_t path[PZ_WIN_MAX_PATH];
    utf8_to_wide(dir, path, sizeof path / sizeof path[0]);
    g_ui.watch_change = FindFirstChangeNotificationW(
        path, FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_SIZE);
    if (g_ui.watch_change == INVALID_HANDLE_VALUE) {
        g_ui.watch_change = NULL;
        CloseHandle(g_ui.watch_stop);
        g_ui.watch_stop = NULL;
        return 0;
    }
    g_ui.watcher = CreateThread(NULL, 0, watcher_thread, NULL, 0, NULL);
    if (!g_ui.watcher) {
        stop_watcher();
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------------- settings */

static void sync_log(void* context, const char* line) {
    (void)context;
    append_log(line);
}

static int run_sync_command(const char* line, char* error, size_t capacity) {
    pz_log log;
    pz_log_init(&log);
    pz_log_set(&log, sync_log, &g_ui);
    pz_mm_result result =
        pz_command_run(g_ui.manager, line, strlen(line), &log);
    const int ok = result.ok;
    if (!ok && error && capacity) {
        str_copy_n(error, capacity, result.message.data, result.message.length);
    }
    char message[PZ_WIN_MAX_LINE];
    str_copy(message, sizeof message, ok ? "OK: " : "ERROR: ");
    str_append_n(message, sizeof message, result.message.data,
                 result.message.length);
    append_log(message);
    pz_mm_result_free(&result);
    return ok;
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_ui.settings_wnd = hwnd;
        pz_paths_config config;
        pz_paths_config_init(&config);
        pz_mod_manager_get_config(g_ui.manager, &config);
        g_ui.settings_dark = config.dark_mode;
        apply_dark_title_bar(hwnd, g_ui.settings_dark);

        HWND fields[4];
        const int y_positions[4] = {PZ_WIN_SET_ZOMBOID_Y, PZ_WIN_SET_PATH_Y,
                                    PZ_WIN_SET_ROOT_Y, PZ_WIN_SET_USER_Y};
        for (int i = 0; i < 4; ++i) {
            fields[i] = CreateWindowW(
                L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                PZ_WIN_SET_FIELD_X, y_positions[i], PZ_WIN_SET_FIELD_W,
                PZ_WIN_SET_FIELD_H, hwnd, NULL, NULL, NULL);
            set_font(fields[i], g_ui.font_body);
            SendMessageW(fields[i], EM_SETMARGINS,
                         EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
            apply_dark_control_theme(fields[i], g_ui.settings_dark);
        }
        g_ui.set_zomboid = fields[0];
        g_ui.set_path = fields[1];
        g_ui.set_root = fields[2];
        g_ui.set_user = fields[3];

        g_ui.set_auto = CreateWindowW(
            L"BUTTON", L"Enable newly installed mods in mods/default.txt",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            PZ_WIN_SET_FIELD_X, PZ_WIN_SET_CHECK_Y, 470, 24, hwnd,
            (HMENU)PZ_WIN_ID_SET_AUTO, NULL, NULL);
        set_font(g_ui.set_auto, g_ui.font_body);

        HWND save = CreateWindowW(L"BUTTON", L"Save",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, PZ_WIN_SET_FIELD_X,
            PZ_WIN_SET_BUTTONS_Y, 120, 38, hwnd,
            (HMENU)PZ_WIN_ID_SET_SAVE, NULL, NULL);
        set_font(save, g_ui.font_body);
        HWND cancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, PZ_WIN_SET_FIELD_X + 136,
            PZ_WIN_SET_BUTTONS_Y, 120, 38, hwnd,
            (HMENU)PZ_WIN_ID_SET_CANCEL, NULL, NULL);
        set_font(cancel, g_ui.font_body);
        HWND theme = CreateWindowW(
            L"BUTTON", g_ui.settings_dark ? L"Theme: Dark" : L"Theme: Light",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, PZ_WIN_SET_FIELD_X + 280,
            PZ_WIN_SET_BUTTONS_Y, 190, 38, hwnd,
            (HMENU)PZ_WIN_ID_SET_THEME, NULL, NULL);
        set_font(theme, g_ui.font_body);

        wchar_t wide[PZ_WIN_MAX_PATH];
        utf8_to_wide(pz_str_cstr(&config.zomboid_dir), wide,
                     sizeof wide / sizeof wide[0]);
        SetWindowTextW(g_ui.set_zomboid, wide);
        utf8_to_wide(pz_str_cstr(&config.steamcmd_exe), wide,
                     sizeof wide / sizeof wide[0]);
        SetWindowTextW(g_ui.set_path, wide);
        utf8_to_wide(pz_str_cstr(&config.steamcmd_root), wide,
                     sizeof wide / sizeof wide[0]);
        SetWindowTextW(g_ui.set_root, wide);
        utf8_to_wide(pz_str_cstr(&config.steam_user), wide,
                     sizeof wide / sizeof wide[0]);
        SetWindowTextW(g_ui.set_user, wide);
        g_ui.set_auto_checked = config.auto_enable;
        pz_paths_config_free(&config);
        SetFocus(g_ui.set_zomboid);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_ui.bg_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_ui.bg_brush);
        SetBkMode(dc, TRANSPARENT);

        HGDIOBJ old = SelectObject(dc, g_ui.font_title);
        SetTextColor(dc, g_ui.theme.text);
        RECT heading = {24, 12, 620, 44};
        DrawTextW(dc, L"Settings", -1, &heading,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_ui.font_body);
        SetTextColor(dc, g_ui.theme.muted);
        RECT subtitle = {24, 44, 630, 64};
        DrawTextW(dc, L"Choose the Zomboid folder and configure steamcmd.", -1,
                  &subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT panel = {PZ_WIN_SET_PANEL_X, PZ_WIN_SET_PANEL_Y,
                      PZ_WIN_SET_PANEL_X + PZ_WIN_SET_PANEL_W,
                      PZ_WIN_SET_PANEL_Y + PZ_WIN_SET_PANEL_H};
        FillRect(dc, &panel, g_ui.card_brush);
        FrameRect(dc, &panel, g_ui.border_brush);

        struct LabelSpec { int y; const wchar_t* text; const wchar_t* hint; int hint_x; };
        const struct LabelSpec labels[] = {
            {92, L"Zomboid folder", L"Absolute path containing mods/ and pzmodmgr/", 150},
            {166, L"steamcmd executable", L"Blank uses the built-in copy", 210},
            {240, L"SteamCMD download root", L"Folder containing steamapps/workshop", 220},
            {314, L"Steam username", NULL, 0},
        };
        for (size_t i = 0; i < sizeof labels / sizeof labels[0]; ++i) {
            SelectObject(dc, g_ui.font_body);
            SetTextColor(dc, g_ui.theme.text);
            RECT r = {PZ_WIN_SET_FIELD_X, labels[i].y,
                      PZ_WIN_SET_FIELD_X + 320, labels[i].y + 20};
            DrawTextW(dc, labels[i].text, -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (!labels[i].hint) continue;
            SelectObject(dc, g_ui.font_small);
            SetTextColor(dc, g_ui.theme.muted);
            RECT h = {PZ_WIN_SET_FIELD_X + labels[i].hint_x, labels[i].y,
                      PZ_WIN_SET_FIELD_X + PZ_WIN_SET_FIELD_W,
                      labels[i].y + 20};
            DrawTextW(dc, labels[i].hint, -1, &h,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        }

        HWND edits[4] = {g_ui.set_zomboid, g_ui.set_path, g_ui.set_root,
                         g_ui.set_user};
        for (int i = 0; i < 4; ++i) {
            if (!edits[i]) continue;
            RECT r;
            GetWindowRect(edits[i], &r);
            MapWindowPoints(NULL, hwnd, (POINT*)&r, 2);
            draw_field_frame(dc, &r, GetFocus() == edits[i]);
        }

        SelectObject(dc, g_ui.font_small);
        SetTextColor(dc, g_ui.theme.muted);
        RECT note = {PZ_WIN_SET_FIELD_X, 412,
                     PZ_WIN_SET_FIELD_X + PZ_WIN_SET_FIELD_W, 430};
        DrawTextW(dc,
                  L"No password is stored. Run steamcmd manually once so its "
                  L"login is cached.",
                  -1, &note, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT note2 = {PZ_WIN_SET_FIELD_X, 430,
                      PZ_WIN_SET_FIELD_X + PZ_WIN_SET_FIELD_W, 448};
        DrawTextW(dc, L"Use anonymous for public Workshop items.", -1, &note2,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (wp == PZ_WIN_ID_SET_AUTO) {
            draw_checkbox_button(dis, g_ui.set_auto_checked);
            return TRUE;
        }
        if (wp == PZ_WIN_ID_SET_SAVE || wp == PZ_WIN_ID_SET_CANCEL ||
            wp == PZ_WIN_ID_SET_THEME) {
            draw_dialog_button(dis, wp == PZ_WIN_ID_SET_SAVE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_ui.theme.text);
        SetBkColor((HDC)wp, g_ui.theme.field);
        return (LRESULT)g_ui.field_brush;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            RECT r;
            GetWindowRect((HWND)lp, &r);
            MapWindowPoints(NULL, hwnd, (POINT*)&r, 2);
            InflateRect(&r, 2, 2);
            InvalidateRect(hwnd, &r, FALSE);
            return 0;
        }
        if (id == PZ_WIN_ID_SET_AUTO && code == BN_CLICKED) {
            g_ui.set_auto_checked = !g_ui.set_auto_checked;
            InvalidateRect(g_ui.set_auto, NULL, TRUE);
            return 0;
        }
        if (id == PZ_WIN_ID_SET_THEME && code == BN_CLICKED) {
            g_ui.settings_dark = !g_ui.settings_dark;
            refresh_theme(g_ui.hwnd, g_ui.settings_dark);
            apply_dark_title_bar(hwnd, g_ui.settings_dark);
            HWND edits[4] = {g_ui.set_zomboid, g_ui.set_path, g_ui.set_root,
                             g_ui.set_user};
            for (int i = 0; i < 4; ++i)
                apply_dark_control_theme(edits[i], g_ui.settings_dark);
            SetWindowTextW(GetDlgItem(hwnd, PZ_WIN_ID_SET_THEME),
                           g_ui.settings_dark ? L"Theme: Dark"
                                              : L"Theme: Light");
            RedrawWindow(hwnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
            return 0;
        }
        if (id == PZ_WIN_ID_SET_SAVE) {
            char zomboid[PZ_WIN_MAX_PATH];
            char path[PZ_WIN_MAX_PATH];
            char root[PZ_WIN_MAX_PATH];
            char user[256];
            read_edit(g_ui.set_zomboid, zomboid, sizeof zomboid);
            read_edit(g_ui.set_path, path, sizeof path);
            read_edit(g_ui.set_root, root, sizeof root);
            read_edit(g_ui.set_user, user, sizeof user);
            if (!user[0]) {
                MessageBoxW(hwnd, L"Steam username is required.", L"Settings",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            const int auto_enable = g_ui.set_auto_checked;
            char command[PZ_WIN_MAX_LINE];
            char error[1024];
            int ok = 1;
            str_copy(command, sizeof command, "config steamcmd_exe ");
            str_append(command, sizeof command, path);
            if (!run_sync_command(command, error, sizeof error)) ok = 0;
            if (ok) {
                str_copy(command, sizeof command, "config steamcmd_root ");
                str_append(command, sizeof command, root);
                if (!run_sync_command(command, error, sizeof error)) ok = 0;
            }
            if (ok) {
                str_copy(command, sizeof command, "config steam_user ");
                str_append(command, sizeof command, user);
                if (!run_sync_command(command, error, sizeof error)) ok = 0;
            }
            if (ok) {
                str_copy(command, sizeof command, "config zomboid_dir ");
                str_append(command, sizeof command, zomboid);
                if (!run_sync_command(command, error, sizeof error)) ok = 0;
            }
            if (ok) {
                str_copy(command, sizeof command, "config dark_mode ");
                str_append(command, sizeof command,
                           g_ui.settings_dark ? "on" : "off");
                if (!run_sync_command(command, error, sizeof error)) ok = 0;
            }
            if (ok) {
                str_copy(command, sizeof command, "config auto_enable ");
                str_append(command, sizeof command, auto_enable ? "on" : "off");
                if (!run_sync_command(command, error, sizeof error)) ok = 0;
            }
            if (!ok) {
                wchar_t wide_error[1024];
                utf8_to_wide(error, wide_error,
                             sizeof wide_error / sizeof wide_error[0]);
                MessageBoxW(hwnd, wide_error, L"Settings",
                            MB_OK | MB_ICONERROR);
                return 0;
            }
            DestroyWindow(hwnd);
            pz_paths_config config;
            pz_paths_config_init(&config);
            pz_mod_manager_get_config(g_ui.manager, &config);
            refresh_theme(g_ui.hwnd, config.dark_mode);
            start_watcher(pz_str_cstr(&config.data_dir));
            pz_paths_config_free(&config);
            pz_mod_manager_reload_installed(g_ui.manager);
            clear_icons(&g_ui);
            refresh_dependency_warning();
            refresh_list();
            append_log("Settings saved");
            return 0;
        }
        if (id == PZ_WIN_ID_SET_CANCEL) {
            pz_paths_config config;
            pz_paths_config_init(&config);
            pz_mod_manager_get_config(g_ui.manager, &config);
            if (g_ui.settings_dark != config.dark_mode)
                refresh_theme(g_ui.hwnd, config.dark_mode);
            pz_paths_config_free(&config);
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE: {
        pz_paths_config config;
        pz_paths_config_init(&config);
        pz_mod_manager_get_config(g_ui.manager, &config);
        if (g_ui.settings_dark != config.dark_mode)
            refresh_theme(g_ui.hwnd, config.dark_mode);
        pz_paths_config_free(&config);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        g_ui.settings_wnd = NULL;
        g_ui.set_auto = NULL;
        g_ui.set_zomboid = NULL;
        g_ui.set_path = NULL;
        g_ui.set_root = NULL;
        g_ui.set_user = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_settings(HWND owner) {
    if (g_ui.settings_wnd) return;
    static int registered = 0;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"PZSettingsWnd";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = 1;
    }

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT wr = {0, 0, PZ_WIN_SET_CLIENT_W, PZ_WIN_SET_CLIENT_H};
    AdjustWindowRect(&wr, style, FALSE);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    RECT r;
    GetWindowRect(owner, &r);
    const int x = r.left + ((r.right - r.left) - w) / 2;
    const int y = r.top + ((r.bottom - r.top) - h) / 2;
    HWND dlg = CreateWindowExW(0, L"PZSettingsWnd", L"Settings", style, x, y,
                               w, h, owner, NULL, hi, NULL);
    if (!dlg) return;

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    apply_dark_title_bar(dlg, g_ui.dark_mode);

    MSG msg;
    while (g_ui.settings_wnd && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetFocus(g_ui.input);
    set_status("Settings saved");
}

/* --------------------------------------------------------------- targets */

static LRESULT CALLBACK TargetsListProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
    const LRESULT result =
        CallWindowProcW(g_ui.tgt_list_old_proc, hwnd, msg, wp, lp);
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

static void update_target_copy_label(void) {
    if (!g_ui.tgt_copy) return;
    const int sel = (int)SendMessageW(g_ui.tgt_list, LB_GETCURSEL, 0, 0);
    const int server = sel >= 0 && sel < g_ui.target_count &&
                       g_ui.targets[sel].kind == 2;
    SetWindowTextW(g_ui.tgt_copy, server ? L"Copy server IDs" : L"Copy IDs");
}

static void sync_target_lock(void) {
    if (!g_ui.tgt_lock) return;
    const int sel = (int)SendMessageW(g_ui.tgt_list, LB_GETCURSEL, 0, 0);
    const int valid = sel >= 0 && sel < g_ui.target_count;
    g_ui.tgt_lock_checked = valid && g_ui.targets[sel].lock_updates;
    InvalidateRect(g_ui.tgt_lock, NULL, TRUE);
    EnableWindow(g_ui.tgt_lock, valid);
}

static void fill_target_list(void) {
    SendMessageW(g_ui.tgt_list, LB_RESETCONTENT, 0, 0);
    const int keep = (int)SendMessageW(g_ui.tgt_list, LB_GETCURSEL, 0, 0);
    for (int i = 0; i < g_ui.target_count; ++i) {
        const pz_activation_target_data* target = &g_ui.targets[i];
        char state[64];
        target_state(&g_ui, target, state, sizeof state);
        char line[512];
        str_copy(line, sizeof line, pz_str_cstr(&target->label));
        str_append(line, sizeof line, "   [");
        str_append(line, sizeof line, state);
        str_append(line, sizeof line, "]");
        wchar_t wide[512];
        utf8_to_wide(line, wide, sizeof wide / sizeof wide[0]);
        SendMessageW(g_ui.tgt_list, LB_ADDSTRING, 0, (LPARAM)wide);
    }
    if (g_ui.target_count > 0)
        SendMessageW(g_ui.tgt_list, LB_SETCURSEL,
                     keep >= 0 && keep < g_ui.target_count ? keep : 0, 0);

    if (g_ui.tgt_status) {
        char text[1024];
        text[0] = '\0';
        if (g_ui.target_count == 0) {
            str_copy(text, sizeof text,
                     "No mods/default.txt, save, or server was found.");
        } else {
            int names = 0;
            for (int i = 0; i < g_ui.row_count; ++i)
                if (row_selected(&g_ui, &g_ui.rows[i])) {
                    if (!names) str_copy(text, sizeof text, "Selected: ");
                    else str_append(text, sizeof text, ", ");
                    str_append(text, sizeof text, g_ui.rows[i].name);
                    ++names;
                }
            if (!names) {
                str_copy(text, sizeof text,
                         "Choose a target. Copy server IDs gives a paste-ready "
                         "list for Add on another pzmodmgr.");
            } else {
                str_append(text, sizeof text,
                           ". Enable or disable them in the highlighted target.");
            }
        }
        set_text_u8(g_ui.tgt_status, text);
    }
    update_target_copy_label();
    sync_target_lock();
    sync_bars();
    if (g_ui.targets_wnd) {
        const int selection =
            (int)SendMessageW(g_ui.tgt_list, LB_GETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(g_ui.targets_wnd, PZ_WIN_ID_TGT_ENABLE),
                     g_ui.target_mod_id_count != 0);
        EnableWindow(GetDlgItem(g_ui.targets_wnd, PZ_WIN_ID_TGT_DISABLE),
                     g_ui.target_mod_id_count != 0);
        EnableWindow(GetDlgItem(g_ui.targets_wnd, PZ_WIN_ID_TGT_COPY_NAMES),
                     selection >= 0 && selection < g_ui.target_count &&
                         g_ui.targets[selection].mod_ids.length != 0);
    }
}

static void target_command(int sel, int enabled, char* out, size_t capacity) {
    out[0] = '\0';
    str_append(out, capacity, "set-enabled ");
    str_append_u64(out, capacity, (uint64_t)sel);
    str_append(out, capacity, enabled ? " on" : " off");
    for (int i = 0; i < g_ui.selected_count; ++i) {
        str_append(out, capacity, " w:");
        str_append_u64(out, capacity, g_ui.selected_ids[i]);
    }
    for (int i = 0; i < g_ui.selected_folder_count; ++i) {
        const char* folder = g_ui.selected_folders[i];
        static const char* digits = "0123456789abcdef";
        str_append_char(out, capacity, ' ');
        str_append_char(out, capacity, 'f');
        str_append_char(out, capacity, ':');
        for (size_t c = 0; folder[c]; ++c) {
            const unsigned char value = (unsigned char)folder[c];
            str_append_char(out, capacity, digits[value >> 4]);
            str_append_char(out, capacity, digits[value & 15]);
        }
    }
}

static void draw_target_item(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) return;
    const int index = (int)dis->itemID;
    if (index < 0 || index >= g_ui.target_count) return;
    const pz_activation_target_data* target = &g_ui.targets[index];

    const int selected = (dis->itemState & ODS_SELECTED) != 0;
    RECT row = dis->rcItem;
    FillRect(dis->hDC, &row,
             selected ? g_ui.selected_brush : g_ui.card_brush);
    FrameRect(dis->hDC, &row,
              selected ? g_ui.accent_brush : g_ui.border_brush);

    RECT icon_rc = {row.left + 8, row.top + 6, row.left + 44, row.top + 42};
    WinIcon* icon = target_icon_for(&g_ui, target);
    if (icon && icon->bitmap) {
        HDC memory = CreateCompatibleDC(dis->hDC);
        HGDIOBJ old = SelectObject(memory, icon->bitmap);
        BitBlt(dis->hDC, icon_rc.left, icon_rc.top, 36, 36, memory, 0, 0,
               SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
    } else {
        HBRUSH soft = CreateSolidBrush(g_ui.theme.accent_soft);
        FillRect(dis->hDC, &icon_rc, soft);
        DeleteObject(soft);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, g_ui.theme.accent);
        HGDIOBJ old = SelectObject(dis->hDC, g_ui.font_small);
        const wchar_t* badge = target->kind == 0 ? L"DEF"
            : target->kind == 1 ? L"SAV" : L"SRV";
        DrawTextW(dis->hDC, badge, -1, &icon_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, old);
    }

    char state_text[64];
    target_state(&g_ui, target, state_text, sizeof state_text);
    wchar_t state[64];
    utf8_to_wide(state_text, state, sizeof state / sizeof state[0]);
    SetBkMode(dis->hDC, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dis->hDC, g_ui.font_small);
    SIZE state_size;
    GetTextExtentPoint32W(dis->hDC, state, (int)wcslen(state), &state_size);
    const int state_right = row.right - 12;
    RECT state_rc = {state_right - state_size.cx, row.top, state_right,
                     row.bottom};
    SelectObject(dis->hDC, g_ui.font_body);
    RECT label_rc = {icon_rc.right + 10, row.top, state_rc.left - 12,
                     row.bottom};
    SetTextColor(dis->hDC, g_ui.theme.text);
    const pz_str* label = &target->label;
    wchar_t wide_label[512];
    utf8_to_wide(pz_str_cstr(label), wide_label,
                 sizeof wide_label / sizeof wide_label[0]);
    DrawTextW(dis->hDC, wide_label, -1, &label_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
    SelectObject(dis->hDC, g_ui.font_small);
    SetTextColor(dis->hDC, (strcmp(state_text, "off") == 0 ||
                            strcmp(state_text, "no Mod IDs") == 0)
                               ? g_ui.theme.muted
                               : g_ui.theme.success);
    DrawTextW(dis->hDC, state, -1, &state_rc,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dis->hDC, old_font);
}

LRESULT CALLBACK TargetsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_ui.targets_wnd = hwnd;
        apply_dark_title_bar(hwnd, g_ui.dark_mode);
        g_ui.tgt_list = CreateWindowW(
            L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
            PZ_WIN_TGT_LIST_X, PZ_WIN_TGT_LIST_Y, PZ_WIN_TGT_LIST_W,
            PZ_WIN_TGT_LIST_H, hwnd, (HMENU)PZ_WIN_ID_TGT_LIST, NULL, NULL);
        set_font(g_ui.tgt_list, g_ui.font_body);
        SendMessageW(g_ui.tgt_list, LB_SETITEMHEIGHT, 0, PZ_WIN_TGT_ROW_H);
        g_ui.tgt_list_old_proc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(
            g_ui.tgt_list, GWLP_WNDPROC, (LONG_PTR)TargetsListProc);
        create_scrollbar(hwnd, g_ui.tgt_list, 0, 1, PZ_WIN_TGT_BAR_X,
                         PZ_WIN_TGT_LIST_Y, PZ_WIN_SCROLL_WIN_W,
                         PZ_WIN_TGT_LIST_H);

        g_ui.tgt_status = CreateWindowW(
            L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
            PZ_WIN_TGT_LIST_X, PZ_WIN_TGT_STATUS_Y, PZ_WIN_TGT_LIST_W, 32,
            hwnd, NULL, NULL, NULL);
        set_font(g_ui.tgt_status, g_ui.font_small);
        g_ui.tgt_lock = CreateWindowW(
            L"BUTTON", L"Lock updates for this target",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            PZ_WIN_TGT_LIST_X, 78, 330, 20, hwnd,
            (HMENU)PZ_WIN_ID_TGT_LOCK, NULL, NULL);
        set_font(g_ui.tgt_lock, g_ui.font_body);

        struct ButtonSpec { const wchar_t* label; int id; int x; int w; };
        const struct ButtonSpec buttons[] = {
            {L"Enable", PZ_WIN_ID_TGT_ENABLE, 56, 98},
            {L"Disable", PZ_WIN_ID_TGT_DISABLE, 160, 98},
            {L"Copy IDs", PZ_WIN_ID_TGT_COPY, 264, 118},
            {L"Copy names", PZ_WIN_ID_TGT_COPY_NAMES, 390, 98},
            {L"Close", PZ_WIN_ID_TGT_CLOSE, 496, 98},
        };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
            HWND button = CreateWindowW(
                L"BUTTON", buttons[i].label,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, buttons[i].x,
                PZ_WIN_TGT_BUTTONS_Y, buttons[i].w, PZ_WIN_TGT_BUTTON_H,
                hwnd, (HMENU)(INT_PTR)buttons[i].id, NULL, NULL);
            set_font(button, g_ui.font_body);
            if (buttons[i].id == PZ_WIN_ID_TGT_COPY) g_ui.tgt_copy = button;
            if (buttons[i].id == PZ_WIN_ID_TGT_COPY_NAMES)
                g_ui.tgt_copy_names = button;
        }
        fill_target_list();
        SetFocus(g_ui.tgt_list);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_ui.bg_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_ui.bg_brush);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g_ui.font_title);
        SetTextColor(dc, g_ui.theme.text);
        RECT heading = {24, 12, 620, 44};
        DrawTextW(dc, L"Enable mods", -1, &heading,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_ui.font_body);
        SetTextColor(dc, g_ui.theme.muted);
        RECT subtitle = {24, 44, 630, 64};
        DrawTextW(dc, L"Choose a Mods list, save, or server for the selected mods.",
                  -1, &subtitle,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT panel = {PZ_WIN_SET_PANEL_X, PZ_WIN_SET_PANEL_Y,
                      PZ_WIN_SET_PANEL_X + PZ_WIN_SET_PANEL_W,
                      PZ_WIN_SET_PANEL_Y + PZ_WIN_SET_PANEL_H};
        FillRect(dc, &panel, g_ui.card_brush);
        FrameRect(dc, &panel, g_ui.border_brush);
        RECT list = {PZ_WIN_TGT_LIST_X - 1, PZ_WIN_TGT_LIST_Y - 1,
                     PZ_WIN_TGT_LIST_X + PZ_WIN_TGT_LIST_W + 1,
                     PZ_WIN_TGT_LIST_Y + PZ_WIN_TGT_LIST_H + 1};
        FrameRect(dc, &list, g_ui.border_brush);
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
        SetBkMode((HDC)wp, OPAQUE);
        SetBkColor((HDC)wp, g_ui.theme.surface);
        SetTextColor((HDC)wp, g_ui.theme.text);
        return (LRESULT)g_ui.card_brush;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, OPAQUE);
        SetBkColor((HDC)wp, g_ui.theme.surface);
        SetTextColor((HDC)wp, g_ui.theme.muted);
        return (LRESULT)g_ui.card_brush;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (wp == PZ_WIN_ID_TGT_LIST) {
            draw_target_item(dis);
            return TRUE;
        }
        if (wp == PZ_WIN_ID_TGT_LOCK) {
            draw_checkbox_button(dis, g_ui.tgt_lock_checked);
            return TRUE;
        }
        if (wp == PZ_WIN_ID_TGT_ENABLE || wp == PZ_WIN_ID_TGT_DISABLE ||
            wp == PZ_WIN_ID_TGT_COPY || wp == PZ_WIN_ID_TGT_COPY_NAMES ||
            wp == PZ_WIN_ID_TGT_CLOSE) {
            draw_dialog_button(dis, wp == PZ_WIN_ID_TGT_ENABLE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == PZ_WIN_ID_TGT_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        const int sel = (int)SendMessageW(g_ui.tgt_list, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= g_ui.target_count) return 0;
        if (id == PZ_WIN_ID_TGT_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            update_target_copy_label();
            sync_target_lock();
            return 0;
        }
        if (id == PZ_WIN_ID_TGT_LOCK && HIWORD(wp) == BN_CLICKED) {
            g_ui.tgt_lock_checked = !g_ui.tgt_lock_checked;
            InvalidateRect(g_ui.tgt_lock, NULL, TRUE);
            char line[64];
            str_copy(line, sizeof line, "target-lock ");
            str_append_u64(line, sizeof line, (uint64_t)sel);
            str_append(line, sizeof line, g_ui.tgt_lock_checked ? " on" : " off");
            char error[512];
            const int ok = run_sync_command(line, error, sizeof error);
            if (!ok) {
                g_ui.tgt_lock_checked = !g_ui.tgt_lock_checked;
                InvalidateRect(g_ui.tgt_lock, NULL, TRUE);
            } else {
                load_targets(&g_ui);
                fill_target_list();
                refresh_list();
            }
            return 0;
        }
        if (id == PZ_WIN_ID_TGT_ENABLE || id == PZ_WIN_ID_TGT_DISABLE) {
            char line[PZ_WIN_MAX_LINE];
            target_command(sel, id == PZ_WIN_ID_TGT_ENABLE, line, sizeof line);
            char error[512];
            if (run_sync_command(line, error, sizeof error)) {
                load_targets(&g_ui);
                fill_target_list();
                refresh_list();
            }
        } else if (id == PZ_WIN_ID_TGT_COPY) {
            pz_vec ids;
            pz_vec_init(&ids, sizeof(uint64_t));
            pz_mod_manager_workshop_ids_for(g_ui.manager, &g_ui.targets[sel],
                                            &ids);
            if (ids.length == 0) {
                char message[512];
                str_copy(message, sizeof message, "No Workshop IDs found for ");
                activation_name(&g_ui.targets[sel], message + strlen(message),
                                sizeof message - strlen(message));
                str_append(message, sizeof message,
                           ". Use Copy names so the list can still be shared.");
                append_log(message);
            } else if (copy_workshop_ids((const uint64_t*)ids.data,
                                         (int)ids.length)) {
                char message[512];
                str_copy(message, sizeof message, "Copied ");
                str_append_u64(message, sizeof message, (uint64_t)ids.length);
                str_append(message, sizeof message, " Workshop ID(s) from ");
                activation_name(&g_ui.targets[sel], message + strlen(message),
                                sizeof message - strlen(message));
                str_append(message, sizeof message,
                           ". Paste into Add, or type lock first to pin them.");
                pz_vec missing;
                pz_vec_init(&missing, sizeof(pz_str));
                pz_mod_manager_unmapped_mod_ids(g_ui.manager,
                                                &g_ui.targets[sel], &missing);
                if (missing.length != 0) {
                    str_append_char(message, sizeof message, ' ');
                    str_append_u64(message, sizeof message,
                                   (uint64_t)missing.length);
                    str_append(message, sizeof message,
                               " enabled Mod ID(s) have no installed Workshop "
                               "ID; use Copy names.");
                }
                pz_activation_string_list_free(&missing);
                append_log(message);
            } else {
                append_log("Could not write to the clipboard.");
            }
            pz_vec_free(&ids);
        } else if (id == PZ_WIN_ID_TGT_COPY_NAMES) {
            pz_vec names;
            pz_vec_init(&names, sizeof(pz_str));
            pz_mod_manager_enabled_mod_names(g_ui.manager, &g_ui.targets[sel],
                                             &names);
            char text[PZ_WIN_MAX_LINE];
            text[0] = '\0';
            for (size_t i = 0; i < names.length; ++i) {
                const pz_str* name = pz_vec_at_const(&names, i);
                if (i) str_append_char(text, sizeof text, '\n');
                str_append_n(text, sizeof text, name->data, name->length);
            }
            if (!text[0]) {
                char message[512];
                str_copy(message, sizeof message, "No enabled Mod names found for ");
                activation_name(&g_ui.targets[sel], message + strlen(message),
                                sizeof message - strlen(message));
                str_append(message, sizeof message, ".");
                append_log(message);
            } else if (copy_plain_text(text)) {
                char message[512];
                str_copy(message, sizeof message, "Copied ");
                str_append_u64(message, sizeof message, (uint64_t)names.length);
                str_append(message, sizeof message, " enabled Mod name(s) from ");
                activation_name(&g_ui.targets[sel], message + strlen(message),
                                sizeof message - strlen(message));
                str_append(message, sizeof message, ".");
                append_log(message);
            } else {
                append_log("Could not write to the clipboard.");
            }
            pz_activation_string_list_free(&names);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        clear_target_icons(&g_ui);
        g_ui.targets_wnd = NULL;
        g_ui.tgt_list = NULL;
        g_ui.tgt_status = NULL;
        g_ui.tgt_lock = NULL;
        g_ui.tgt_copy = NULL;
        g_ui.tgt_copy_names = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_targets(HWND owner, int prefer_server, int allow_empty) {
    if (g_ui.targets_wnd) return;
    if (!allow_empty && g_ui.selected_count == 0 &&
        g_ui.selected_folder_count == 0)
        return;

    g_ui.target_mod_id_count = 0;
    for (int i = 0; i < g_ui.row_count; ++i) {
        if (!row_selected(&g_ui, &g_ui.rows[i])) continue;
        for (int m = 0; m < g_ui.rows[i].mod_id_count; ++m) {
            int duplicate = 0;
            for (int t = 0; t < g_ui.target_mod_id_count; ++t)
                if (strcmp(g_ui.target_mod_ids[t], g_ui.rows[i].mod_ids[m]) == 0)
                    duplicate = 1;
            if (!duplicate && g_ui.target_mod_id_count < PZ_WIN_MAX_TARGET_IDS)
                str_copy(g_ui.target_mod_ids[g_ui.target_mod_id_count++],
                         sizeof g_ui.target_mod_ids[0], g_ui.rows[i].mod_ids[m]);
        }
    }
    if (g_ui.target_mod_id_count == 0 && !allow_empty) {
        append_log("No Mod ID found for the selected mod(s).");
        return;
    }

    load_targets(&g_ui);
    if (prefer_server) {
        for (int i = 0; i < g_ui.target_count; ++i) {
            if (g_ui.targets[i].kind == 2) {
                pz_activation_target_data server = g_ui.targets[i];
                memmove(&g_ui.targets[1], &g_ui.targets[0],
                        (size_t)i * sizeof g_ui.targets[0]);
                g_ui.targets[0] = server;
                break;
            }
        }
    }

    static int registered = 0;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = TargetsProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"PZTargetsWnd";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = 1;
    }

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT wr = {0, 0, PZ_WIN_SET_CLIENT_W, PZ_WIN_SET_CLIENT_H};
    AdjustWindowRect(&wr, style, FALSE);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    RECT r;
    GetWindowRect(owner, &r);
    const int x = r.left + ((r.right - r.left) - w) / 2;
    const int y = r.top + ((r.bottom - r.top) - h) / 2;
    HWND dlg = CreateWindowExW(0, L"PZTargetsWnd", L"Enable mods", style, x,
                               y, w, h, owner, NULL, hi, NULL);
    if (!dlg) return;
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    apply_dark_title_bar(dlg, g_ui.dark_mode);

    MSG msg;
    while (g_ui.targets_wnd && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetFocus(g_ui.input);
    refresh_list();
}

/* ------------------------------------------------------------ main window */

static void create_main_controls(HWND hwnd) {
    g_ui.hwnd = hwnd;
    g_ui.brand_mark = CreateWindowW(
        L"STATIC", L"PZ",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, 24,
        20, 36, 36, hwnd, NULL, NULL, NULL);
    set_font(g_ui.brand_mark, g_ui.font_brand);
    g_ui.brand_line = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 32,
                                    49, 20, 3, hwnd, NULL, NULL, NULL);
    g_ui.title = CreateWindowW(L"STATIC", L"PZ Mod Manager",
                               WS_CHILD | WS_VISIBLE, 72, 12, 600, 38, hwnd,
                               NULL, NULL, NULL);
    set_font(g_ui.title, g_ui.font_title);
    g_ui.subtitle = CreateWindowW(
        L"STATIC", L"Project Zomboid Workshop installer",
        WS_CHILD | WS_VISIBLE, 72, 51, 600, 18, hwnd, NULL, NULL, NULL);
    set_font(g_ui.subtitle, g_ui.font_small);
    g_ui.share_server_btn = CreateWindowW(
        L"BUTTON", L"Share server", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        560, 24, 116, 30, hwnd, (HMENU)PZ_WIN_ID_SHARE_SERVER, NULL, NULL);
    set_font(g_ui.share_server_btn, g_ui.font_body);
    g_ui.enable_btn = CreateWindowW(
        L"BUTTON", L"Enable...",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 688, 24, 100, 30,
        hwnd, (HMENU)PZ_WIN_ID_ENABLE, NULL, NULL);
    set_font(g_ui.enable_btn, g_ui.font_body);
    g_ui.self_update = CreateWindowW(
        L"BUTTON", L"Update app", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 440,
        24, 108, 30, hwnd, (HMENU)PZ_WIN_ID_SELF_UPDATE, NULL, NULL);
    set_font(g_ui.self_update, g_ui.font_body);
    g_ui.settings_btn = CreateWindowW(
        L"BUTTON", L"Settings", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 800, 24,
        96, 30, hwnd, (HMENU)PZ_WIN_ID_SETTINGS, NULL, NULL);
    set_font(g_ui.settings_btn, g_ui.font_body);
    g_ui.open_mods = CreateWindowW(
        L"BUTTON", L"Mods folder", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 320,
        24, 110, 30, hwnd, (HMENU)PZ_WIN_ID_OPEN_MODS, NULL, NULL);
    set_font(g_ui.open_mods, g_ui.font_body);

    g_ui.input = CreateWindowW(
        L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 24,
        84, 520, 38, hwnd, (HMENU)PZ_WIN_ID_INPUT, NULL, NULL);
    set_font(g_ui.input, g_ui.font_body);
    set_edit_placeholder(g_ui.input, L"Paste Workshop URLs or IDs");
    SendMessageW(g_ui.input, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, 26));
    g_ui.input_clear = CreateWindowW(
        L"BUTTON", L"X", WS_CHILD | BS_OWNERDRAW, 522, 86, 20, 34, hwnd,
        (HMENU)PZ_WIN_ID_INPUT_CLEAR, NULL, NULL);
    set_font(g_ui.input_clear, g_ui.font_small);
    g_ui.add = CreateWindowW(
        L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 560, 84,
        212, 38, hwnd, (HMENU)PZ_WIN_ID_ADD, NULL, NULL);
    set_font(g_ui.add, g_ui.font_body);
    g_ui.import = CreateWindowW(
        L"BUTTON", L"Import", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 784, 84,
        112, 38, hwnd, (HMENU)PZ_WIN_ID_IMPORT, NULL, NULL);
    set_font(g_ui.import, g_ui.font_body);

    HWND label = CreateWindowW(
        L"STATIC",
        L"Press Enter to install, or Import a URL/ID to track an existing local copy.",
        WS_CHILD | WS_VISIBLE, 24, 132, 872, 18, hwnd, NULL, NULL, NULL);
    set_font(label, g_ui.font_small);

    g_ui.filter = CreateWindowW(
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24,
        158, 280, 24, hwnd, (HMENU)PZ_WIN_ID_FILTER, NULL, NULL);
    set_font(g_ui.filter, g_ui.font_small);
    set_edit_placeholder(g_ui.filter, L"Filter name or folder");
    SendMessageW(g_ui.filter, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, 26));
    g_ui.filter_clear = CreateWindowW(
        L"BUTTON", L"X", WS_CHILD | BS_OWNERDRAW, 282, 159, 20, 22, hwnd,
        (HMENU)PZ_WIN_ID_FILTER_CLEAR, NULL, NULL);
    set_font(g_ui.filter_clear, g_ui.font_small);
    g_ui.filter_combo = CreateWindowW(
        L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
        312, 158, 170, 200, hwnd, (HMENU)PZ_WIN_ID_FILTER_MODE, NULL, NULL);
    set_font(g_ui.filter_combo, g_ui.font_small);
    SendMessageW(g_ui.filter_combo, CB_SETITEMHEIGHT, (WPARAM)-1, 18);
    g_ui.check = CreateWindowW(
        L"BUTTON", L"Check updates", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 492,
        158, 120, 24, hwnd, (HMENU)PZ_WIN_ID_CHECK, NULL, NULL);
    g_ui.sort = CreateWindowW(
        L"BUTTON", L"Sort: Name", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 622,
        158, 140, 24, hwnd, (HMENU)PZ_WIN_ID_SORT, NULL, NULL);
    set_font(g_ui.check, g_ui.font_small);
    set_font(g_ui.sort, g_ui.font_small);
    g_ui.count = CreateWindowW(L"STATIC", L"0 mods",
                               WS_CHILD | WS_VISIBLE | SS_RIGHT, 776, 158, 120,
                               18, hwnd, NULL, NULL, NULL);
    set_font(g_ui.count, g_ui.font_small);

    g_ui.list = CreateWindowW(
        L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LBS_NOTIFY |
            LBS_EXTENDEDSEL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
            LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
        24, 192, 856, 344, hwnd, (HMENU)PZ_WIN_ID_LIST, NULL, NULL);
    g_ui.list_old_proc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(
        g_ui.list, GWLP_WNDPROC, (LONG_PTR)ListProc);
    SendMessageW(g_ui.list, LB_SETITEMHEIGHT, 0, 68);
    create_scrollbar(hwnd, g_ui.list, 0, 0, 882, 192, PZ_WIN_SCROLL_WIN_W,
                     344);
    g_ui.empty = CreateWindowW(
        L"STATIC", L"",
        WS_CHILD | WS_CLIPSIBLINGS | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
        24, 192, 856, 68, hwnd, NULL, NULL, NULL);
    set_font(g_ui.empty, g_ui.font_body);

    g_ui.select_all = CreateWindowW(
        L"BUTTON", L"Select all", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 24,
        554, 116, 32, hwnd, (HMENU)PZ_WIN_ID_SELECT_ALL, NULL, NULL);
    g_ui.clear = CreateWindowW(
        L"BUTTON", L"Deselect", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 148,
        554, 116, 32, hwnd, (HMENU)PZ_WIN_ID_CLEAR, NULL, NULL);
    g_ui.lock = CreateWindowW(
        L"BUTTON", L"Lock",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 272, 554, 108, 32,
        hwnd, (HMENU)PZ_WIN_ID_LOCK, NULL, NULL);
    g_ui.copy = CreateWindowW(
        L"BUTTON", L"Copy IDs",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 392, 554, 104, 32,
        hwnd, (HMENU)PZ_WIN_ID_COPY, NULL, NULL);
    g_ui.update = CreateWindowW(
        L"BUTTON", L"Update",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 506, 554, 120, 32,
        hwnd, (HMENU)PZ_WIN_ID_UPDATE, NULL, NULL);
    g_ui.update_all = CreateWindowW(
        L"BUTTON", L"Update all", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 636,
        554, 120, 32, hwnd, (HMENU)PZ_WIN_ID_UPDATE_ALL, NULL, NULL);
    g_ui.remove = CreateWindowW(
        L"BUTTON", L"Remove",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW, 766, 554, 130, 32,
        hwnd, (HMENU)PZ_WIN_ID_REMOVE, NULL, NULL);
    set_font(g_ui.select_all, g_ui.font_body);
    set_font(g_ui.clear, g_ui.font_body);
    set_font(g_ui.lock, g_ui.font_body);
    set_font(g_ui.copy, g_ui.font_body);
    set_font(g_ui.update, g_ui.font_body);
    set_font(g_ui.update_all, g_ui.font_body);
    set_font(g_ui.remove, g_ui.font_body);

    g_ui.details = CreateWindowW(
        L"STATIC", L"No selection",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 24,
        594, 872, 18, hwnd, NULL, NULL, NULL);
    set_font(g_ui.details, g_ui.font_small);
    g_ui.activity_label = CreateWindowW(
        L"STATIC", L"ACTIVITY", WS_CHILD | WS_VISIBLE, 24, 616, 200, 18, hwnd,
        NULL, NULL, NULL);
    set_font(g_ui.activity_label, g_ui.font_small);
    g_ui.log = CreateWindowW(
        L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
            ES_AUTOHSCROLL,
        25, 635, 856, 60, hwnd, (HMENU)PZ_WIN_ID_LOG, NULL, NULL);
    set_font(g_ui.log, g_ui.font_small);
    g_ui.log_old_proc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(
        g_ui.log, GWLP_WNDPROC, (LONG_PTR)LogProc);
    create_scrollbar(hwnd, g_ui.log, 1, 1, 882, 635, PZ_WIN_SCROLL_WIN_W, 60);
    g_ui.status = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                                24, 702, 500, 18, hwnd, NULL, NULL, NULL);
    set_font(g_ui.status, g_ui.font_small);

    append_log("Paste Workshop URLs or IDs, then press Enter.");
    append_log("Click mods to toggle their selection; Shift-click selects a range.");
    refresh_dependency_warning();
    refresh_list();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        create_main_controls(hwnd);
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, g_ui.bg_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_ui.bg_brush);
        RECT card = {24, 610, 896, 698};
        FillRect(dc, &card, g_ui.card_brush);
        FrameRect(dc, &card, g_ui.border_brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetBkColor((HDC)wp, g_ui.theme.bg);
        SetTextColor((HDC)wp, g_ui.theme.text);
        return (LRESULT)g_ui.bg_brush;
    case WM_CTLCOLORSTATIC: {
        HWND control = (HWND)lp;
        if (control == g_ui.brand_mark) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_ui.theme.brand);
            SetTextColor((HDC)wp, g_ui.theme.cream);
            return (LRESULT)g_ui.brand_brush;
        }
        if (control == g_ui.brand_line) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_ui.theme.accent);
            return (LRESULT)g_ui.accent_brush;
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        if (control == g_ui.empty || control == g_ui.activity_label) {
            SetBkMode((HDC)wp, OPAQUE);
            SetBkColor((HDC)wp, g_ui.theme.surface);
            SetTextColor((HDC)wp, control == g_ui.activity_label
                                      ? g_ui.theme.brand
                                      : g_ui.theme.muted);
            return (LRESULT)g_ui.card_brush;
        }
        if (control == g_ui.details) {
            SetTextColor((HDC)wp, g_ui.theme.muted);
        } else if (control == g_ui.status) {
            SetTextColor((HDC)wp, g_ui.busy ? g_ui.theme.accent
                : (g_ui.dependency_warning[0] ? g_ui.theme.danger
                                              : g_ui.theme.brand));
        } else {
            SetTextColor((HDC)wp, g_ui.theme.text);
        }
        return (LRESULT)g_ui.bg_brush;
    }
    case WM_CTLCOLOREDIT: {
        HWND control = (HWND)lp;
        if (control == g_ui.log) {
            SetTextColor((HDC)wp, g_ui.theme.text);
            SetBkColor((HDC)wp, g_ui.theme.surface);
            return (LRESULT)g_ui.card_brush;
        }
        SetTextColor((HDC)wp, g_ui.theme.text);
        SetBkColor((HDC)wp, g_ui.theme.field);
        return (LRESULT)g_ui.field_brush;
    }
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, g_ui.theme.text);
        SetBkColor((HDC)wp, g_ui.theme.bg);
        return (LRESULT)g_ui.bg_brush;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
        if (wp == PZ_WIN_ID_FILTER_MODE) {
            mis->itemHeight = 22;
            return TRUE;
        }
        return FALSE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lp;
        if (wp == PZ_WIN_ID_LIST) {
            draw_list_item(dis);
            return TRUE;
        }
        if (wp == PZ_WIN_ID_FILTER_CLEAR || wp == PZ_WIN_ID_INPUT_CLEAR) {
            draw_filter_clear(dis);
            return TRUE;
        }
        if (wp == PZ_WIN_ID_FILTER_MODE) {
            draw_filter_combo_item(dis);
            return TRUE;
        }
        if (wp == PZ_WIN_ID_ADD || wp == PZ_WIN_ID_UPDATE ||
            wp == PZ_WIN_ID_REMOVE || wp == PZ_WIN_ID_UPDATE_ALL ||
            wp == PZ_WIN_ID_SELECT_ALL || wp == PZ_WIN_ID_CLEAR ||
            wp == PZ_WIN_ID_LOCK || wp == PZ_WIN_ID_COPY ||
            wp == PZ_WIN_ID_SETTINGS || wp == PZ_WIN_ID_ENABLE ||
            wp == PZ_WIN_ID_IMPORT || wp == PZ_WIN_ID_SHARE_SERVER ||
            wp == PZ_WIN_ID_SORT || wp == PZ_WIN_ID_CHECK ||
            wp == PZ_WIN_ID_SELF_UPDATE || wp == PZ_WIN_ID_OPEN_MODS) {
            draw_flat_button(dis, (int)wp, wp == PZ_WIN_ID_ADD);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case PZ_WIN_ID_ADD:
            add_input();
            return 0;
        case PZ_WIN_ID_IMPORT: {
            char pasted[PZ_WIN_MAX_LINE];
            read_edit(g_ui.input, pasted, sizeof pasted);
            char command[PZ_WIN_MAX_LINE];
            str_copy(command, sizeof command, "import");
            if (pasted[0]) {
                str_append_char(command, sizeof command, ' ');
                str_append(command, sizeof command, pasted);
                SetWindowTextW(g_ui.input, L"");
                update_input_clear();
            }
            start_command(command);
            return 0;
        }
        case PZ_WIN_ID_OPEN_MODS:
            open_mods_folder(hwnd);
            return 0;
        case PZ_WIN_ID_UPDATE:
            update_selected();
            return 0;
        case PZ_WIN_ID_LOCK:
            toggle_lock_selected();
            return 0;
        case PZ_WIN_ID_REMOVE:
            remove_selected();
            return 0;
        case PZ_WIN_ID_UPDATE_ALL:
            start_command("update");
            return 0;
        case PZ_WIN_ID_SETTINGS:
            show_settings(hwnd);
            return 0;
        case PZ_WIN_ID_ENABLE:
            show_targets(hwnd, 0, 0);
            return 0;
        case PZ_WIN_ID_SHARE_SERVER:
            show_targets(hwnd, 1, 1);
            return 0;
        case PZ_WIN_ID_SELECT_ALL:
            if (g_ui.row_count != 0) {
                SendMessageW(g_ui.list, LB_SETSEL, TRUE, -1);
                sync_selection(&g_ui);
            }
            return 0;
        case PZ_WIN_ID_CLEAR:
            SendMessageW(g_ui.list, LB_SETSEL, FALSE, -1);
            g_ui.selected_count = 0;
            g_ui.selected_folder_count = 0;
            update_actions();
            set_details();
            return 0;
        case PZ_WIN_ID_COPY:
            copy_selected_ids();
            return 0;
        case PZ_WIN_ID_INPUT:
            if (HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(g_ui.input, NULL, TRUE);
                update_input_clear();
            }
            return 0;
        case PZ_WIN_ID_INPUT_CLEAR:
            SetWindowTextW(g_ui.input, L"");
            update_input_clear();
            SetFocus(g_ui.input);
            return 0;
        case PZ_WIN_ID_FILTER:
            if (HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(g_ui.filter, NULL, TRUE);
                update_filter_clear();
                refresh_list();
            }
            return 0;
        case PZ_WIN_ID_FILTER_CLEAR:
            SetWindowTextW(g_ui.filter, L"");
            refresh_list();
            update_filter_clear();
            SetFocus(g_ui.filter);
            return 0;
        case PZ_WIN_ID_FILTER_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE && !g_ui.filter_combo_updating) {
                const int selected =
                    (int)SendMessageW(g_ui.filter_combo, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected < g_ui.filter_choice_count) {
                    g_ui.filter_mode = g_ui.filter_choices[selected];
                    refresh_list();
                }
            }
            return 0;
        case PZ_WIN_ID_SORT:
            g_ui.sort_mode = 1 - g_ui.sort_mode;
            refresh_list();
            return 0;
        case PZ_WIN_ID_CHECK:
            start_command("check");
            return 0;
        case PZ_WIN_ID_SELF_UPDATE:
            start_command("self-update");
            return 0;
        case PZ_WIN_ID_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE)
                sync_selection(&g_ui);
            else if (HIWORD(wp) == LBN_DBLCLK)
                update_selected();
            return 0;
        default:
            return 0;
        }
    case PZ_WIN_WM_LOG:
        drain_pending();
        return 0;
    case PZ_WIN_WM_RELOAD:
        if (g_ui.busy) {
            g_ui.reload_pending = 1;
        } else if (g_ui.manager && pz_mod_manager_reload_installed(g_ui.manager)) {
            clear_icons(&g_ui);
            refresh_dependency_warning();
            refresh_list();
        }
        return 0;
    case PZ_WIN_WM_DONE:
        if (g_ui.worker) {
            WaitForSingleObject(g_ui.worker, INFINITE);
            CloseHandle(g_ui.worker);
            g_ui.worker = NULL;
        }
        drain_pending();
        g_ui.busy = 0;
        g_ui.app_update_available =
            g_ui.manager && pz_mod_manager_app_update_available(g_ui.manager);
        if (g_ui.reload_pending) {
            g_ui.reload_pending = 0;
            if (g_ui.manager) pz_mod_manager_reload_installed(g_ui.manager);
        }
        EnableWindow(g_ui.input, TRUE);
        EnableWindow(g_ui.list, TRUE);
        refresh_dependency_warning();
        clear_icons(&g_ui);
        refresh_list();
        SetFocus(g_ui.input);
        return 0;
    case WM_TIMER:
        if (wp == PZ_WIN_AUTO_TIMER) {
            if (g_ui.settings_wnd) {
                SetTimer(hwnd, PZ_WIN_AUTO_TIMER, 300, NULL);
                return 0;
            }
            KillTimer(hwnd, PZ_WIN_AUTO_TIMER);
            if (!g_ui.busy && g_ui.manager &&
                pz_mod_manager_update_check_due(g_ui.manager))
                start_command("check");
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_ui.busy) {
            append_log("A command is still running; wait for it before closing.");
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_ui.worker) {
            WaitForSingleObject(g_ui.worker, INFINITE);
            CloseHandle(g_ui.worker);
            g_ui.worker = NULL;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int pz_win32_run_ui(pz_mod_manager* manager) {
    g_ui_ptr = (WinUi*)calloc(1, sizeof(WinUi));
    if (!g_ui_ptr) return 1;
    memset(&g_ui, 0, sizeof g_ui);
    InitializeCriticalSection(&g_ui.pending_mu);
    g_ui.manager = manager;
    g_ui.filter_mode = WinFilterAll;
    g_ui.app_update_available =
        manager && pz_mod_manager_app_update_available(manager);
    sync_paths(&g_ui);

    SetProcessDPIAware();
    g_ui.font_title = make_font(15, FW_SEMIBOLD);
    g_ui.font_row = make_font(10, FW_SEMIBOLD);
    g_ui.font_brand = make_font(12, FW_BOLD);
    g_ui.font_body = make_font(10, FW_NORMAL);
    g_ui.font_small = make_font(8, FW_NORMAL);

    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(manager, &config);
    refresh_theme(NULL, config.dark_mode);
    pz_paths_config_free(&config);

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"PZModMgrWnd";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    register_scrollbar_class();

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                        WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT wr = {0, 0, PZ_WIN_WIDTH, PZ_WIN_HEIGHT};
    AdjustWindowRect(&wr, style, FALSE);
    HWND window = CreateWindowExW(0, L"PZModMgrWnd", L"PZ Mod Manager",
                                  style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  wr.right - wr.left, wr.bottom - wr.top, NULL,
                                  NULL, hi, NULL);
    if (!window) {
        DeleteCriticalSection(&g_ui.pending_mu);
        free(g_ui_ptr);
        g_ui_ptr = NULL;
        return 1;
    }
    apply_dark_title_bar(window, g_ui.dark_mode);
    ShowWindow(window, SW_SHOW);
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(manager, &config);
    start_watcher(pz_str_cstr(&config.data_dir));
    if (pz_mod_manager_reload_installed(manager)) refresh_list();
    SetFocus(g_ui.input);
    if (pz_mod_manager_update_check_due(manager))
        SetTimer(window, PZ_WIN_AUTO_TIMER, 300, NULL);
    pz_paths_config_free(&config);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == g_ui.input && !g_ui.busy) {
            add_input();
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            GetFocus() == g_ui.filter) {
            SetWindowTextW(g_ui.filter, L"");
            refresh_list();
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
            GetFocus() == g_ui.input) {
            SetWindowTextW(g_ui.input, L"");
            update_input_clear();
            continue;
        }
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    stop_watcher();
    clear_icons(&g_ui);
    clear_target_icons(&g_ui);
    clear_targets(&g_ui);
    destroy_theme_brushes();
    if (g_ui.font_small) DeleteObject(g_ui.font_small);
    if (g_ui.font_body) DeleteObject(g_ui.font_body);
    if (g_ui.font_brand) DeleteObject(g_ui.font_brand);
    if (g_ui.font_row) DeleteObject(g_ui.font_row);
    if (g_ui.font_title) DeleteObject(g_ui.font_title);
    free(g_ui.all_rows);
    free(g_ui.rows);
    DeleteCriticalSection(&g_ui.pending_mu);
    free(g_ui_ptr);
    g_ui_ptr = NULL;
    return (int)msg.wParam;
}

