#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ui/x11/x11_ui_c.h"

#include "core/commands_c.h"
#include "core/mod_info_fs_c.h"
#include "core/paths_c.h"
#include "ui/glyphs_c.h"
#include "ui/mod_icon_c.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

#define PZ_WINDOW_WIDTH 920
#define PZ_WINDOW_HEIGHT 720
#define PZ_MARGIN 24
#define PZ_INPUT_X 24
#define PZ_INPUT_Y 84
#define PZ_INPUT_W 524
#define PZ_INPUT_H 38
#define PZ_INSTALL_X 560
#define PZ_INSTALL_W 212
#define PZ_IMPORT_X 784
#define PZ_IMPORT_W 112
#define PZ_FILTER_Y 158
#define PZ_FILTER_H 24
#define PZ_FILTER_W 180
#define PZ_FILTER_MENU_X 312
#define PZ_FILTER_MENU_W 170
#define PZ_FILTER_MENU_ROW_H 26
#define PZ_LIST_TOP 192
#define PZ_ROW_H 64
#define PZ_ROW_GAP 6
#define PZ_ROWS 5
#define PZ_CARD_W 872
#define PZ_SCROLL_X 902
#define PZ_SCROLL_W 6
#define PZ_SCROLL_MIN_THUMB 24
#define PZ_SCROLL_GRAB 3
#define PZ_LIST_BOTTOM (PZ_LIST_TOP + PZ_ROWS * PZ_ROW_H + (PZ_ROWS - 1) * PZ_ROW_GAP)
#define PZ_ACTION_Y 554
#define PZ_ACTION_H 32
#define PZ_ACTIVITY_Y 602
#define PZ_ACTIVITY_H 80
#define PZ_ACTIVITY_LINES 3
#define PZ_ACTIVITY_TRACK_TOP (PZ_ACTIVITY_Y + 32)
#define PZ_ACTIVITY_TRACK_BOTTOM (PZ_ACTIVITY_Y + PZ_ACTIVITY_H - 8)
#define PZ_ACTIVITY_SCROLL_X (PZ_MARGIN + PZ_CARD_W - PZ_SCROLL_W - 12)
#define PZ_STATUS_Y 702
#define PZ_SETTINGS_X 130
#define PZ_SETTINGS_Y 140
#define PZ_SETTINGS_W 660
#define PZ_SETTINGS_H 500
#define PZ_SETTINGS_FIELD_X (PZ_SETTINGS_X + 32)
#define PZ_SETTINGS_FIELD_W (PZ_SETTINGS_W - 64)
#define PZ_SETTINGS_FIELD_H 38
#define PZ_SETTINGS_ZOMBOID_Y (PZ_SETTINGS_Y + 104)
#define PZ_SETTINGS_STEAMCMD_Y (PZ_SETTINGS_Y + 178)
#define PZ_SETTINGS_ROOT_Y (PZ_SETTINGS_Y + 252)
#define PZ_SETTINGS_USER_Y (PZ_SETTINGS_Y + 326)
#define PZ_SETTINGS_BUTTONS_Y (PZ_SETTINGS_Y + 458)
#define PZ_TARGETS_ROW_Y (PZ_SETTINGS_Y + 104)
#define PZ_TARGETS_ROW_H 36
#define PZ_TARGETS_ROWS 7
#define PZ_TARGETS_BUTTONS_Y (PZ_SETTINGS_Y + 360)
#define PZ_ROWS_INITIAL 64
#define PZ_MAX_FOLDERS 8
#define PZ_MAX_LOG 300
#define PZ_MAX_LINE 2048
#define PZ_MAX_IDS 2048
#define PZ_MAX_TARGETS 64
#define PZ_MAX_TARGET_IDS 512
#define PZ_MAX_PENDING 64

typedef struct Rect {
    int x, y, w, h;
} Rect;

typedef struct Palette {
    unsigned long bg, surface, field, border, border_strong, text, muted;
    unsigned long accent, accent_hover, accent_soft, danger, danger_soft;
    unsigned long brand, brand_soft, brand_border, cream, hover, success;
    unsigned long on_accent;
} Palette;

typedef enum Btn {
    BtnNothing, BtnInstall, BtnImport, BtnSettings, BtnUpdate, BtnRemove,
    BtnUpdateAll, BtnSelectAll, BtnClear, BtnCopy, BtnLock, BtnSave,
    BtnCancel, BtnTheme, BtnFilterMenu, BtnSort, BtnCheck, BtnEnable,
    BtnShareServer, BtnSelfUpdate, BtnEnableTarget, BtnDisableTarget,
    BtnShareTargets, BtnCopyNames, BtnCloseTargets
} Btn;

typedef enum FilterMode {
    FilterAll, FilterUpdates, FilterLocked, FilterNontracked
} FilterMode;

typedef enum PasteField {
    PasteInput, PasteFilter, PasteSettingsZomboid, PasteSettingsPath,
    PasteSettingsRoot, PasteSettingsUser
} PasteField;

typedef struct Row {
    uint64_t workshop_id;
    char name[256];
    char folders[PZ_MAX_FOLDERS][256];
    int folder_count;
    uint64_t time_updated;
    int update_available;
    int locked;
    int imported;
} Row;

typedef struct InfoEntry {
    char folder[256];
    pz_mod_info info;
    int ok;
} InfoEntry;

typedef struct IconEntry {
    char key[1024];
    Pixmap pixmap;
    unsigned width;
    unsigned height;
} IconEntry;

typedef struct Ui {
    Display* display;
    int screen;
    Window window;
    GC gc;
    Pixmap backbuf;
    Drawable target;
    XFontStruct* body;
    XFontStruct* heading;
    Palette pal;
    Palette light_pal;
    Palette dark_pal;
    Atom wm_delete;
    Atom clipboard;
    Atom utf8;
    Atom paste_prop;

    pz_mod_manager* manager;
    char mods_dir[1024];
    char data_dir[1024];
    char game_version[64];

    pz_str log[PZ_MAX_LOG];
    int log_start;
    int log_count;
    char input[PZ_MAX_LINE];
    int busy;
    int app_update_available;
    int auto_check_due;
    int auto_check_started;

    uint64_t selected_ids[PZ_MAX_IDS];
    int selected_count;
    char (*selected_folders)[256];
    int selected_folder_capacity;
    int selected_folder_count;
    int confirm_remove;

    char filter_text[256];
    FilterMode filter_mode;
    int filter_total;
    int filter_updates;
    int filter_locked;
    int filter_nontracked;
    int filter_menu_open;
    int filter_menu_hover;
    int filter_focus;
    int sort_mode;
    int list_offset;
    int scroll_drag;
    int scroll_drag_y;
    int scroll_drag_offset;

    Row* rows;
    int row_count;
    int row_capacity;
    int* visible;
    int visible_count;
    int* row_enabled;
    int list_dirty;
    int activity_offset;
    int activity_drag;
    int activity_drag_y;
    int activity_drag_offset;
    int activity_follow;

    int settings_open;
    int settings_dark;
    int settings_auto;
    int settings_field;
    char settings_zomboid[1024];
    char settings_path[1024];
    char settings_root[1024];
    char settings_user[256];

    int targets_open;
    int target_focus;
    int target_offset;
    pz_activation_target_data targets[PZ_MAX_TARGETS];
    int target_count;
    char target_mod_ids[PZ_MAX_TARGET_IDS][128];
    int target_mod_id_count;

    char dependency_warning[256];
    char dependency_signature[1024];

    InfoEntry* infos;
    int info_count;
    int info_capacity;
    IconEntry* icons;
    int icon_count;
    int icon_capacity;
    IconEntry* target_icons;
    int target_icon_count;
    int target_icon_capacity;

    pthread_mutex_t pending_mu;
    char pending[PZ_MAX_PENDING][PZ_MAX_LINE];
    int pending_count;
    int worker_done;
    pthread_t worker;
    int worker_joinable;
    int wake[2];
    int watch_fd;
    int reload_pending;

    PasteField paste_field;
    Atom paste_selection;
    Time paste_time;
    int paste_pending;
    int paste_fallback;

    int hover_button;
    int hover_row;
    int last_mouse_x;
    int last_mouse_y;
    int anchor_index;
    int last_click_index;
    uint64_t last_click_time;
    int frame_pending;
    char clipboard_text[PZ_MAX_IDS * 24];

    int running;
} Ui;

static void* grow_buffer(void* data, int* capacity, int needed,
                         size_t item_size) {
    int cap = *capacity;
    void* grown;

    if (needed <= cap) return data;
    if (cap < 16) cap = 16;
    while (cap < needed) {
        if (cap > 1000000000 / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    grown = realloc(data, (size_t)cap * item_size);
    if (!grown) return NULL;
    *capacity = cap;
    return grown;
}

static int ensure_row_capacity(Ui* ui, int needed) {
    int cap = ui->row_capacity;

    if (needed <= cap) return 1;
    if (cap < PZ_ROWS_INITIAL) cap = PZ_ROWS_INITIAL;
    while (cap < needed) {
        if (cap > 1000000000 / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    {
        Row* rows = (Row*)realloc(ui->rows, (size_t)cap * sizeof *rows);
        int* visible;
        int* enabled;

        if (!rows) return 0;
        ui->rows = rows;
        visible = (int*)realloc(ui->visible, (size_t)cap * sizeof *visible);
        if (!visible) return 0;
        ui->visible = visible;
        enabled = (int*)realloc(ui->row_enabled, (size_t)cap * sizeof *enabled);
        if (!enabled) return 0;
        ui->row_enabled = enabled;
        ui->row_capacity = cap;
    }
    return 1;
}

/* ------------------------------------------------------------------ text */

static int min_int(int a, int b) { return a < b ? a : b; }
static int max_int(int a, int b) { return a > b ? a : b; }

static int in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

static void text_copy(char* out, size_t capacity, const char* value) {
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

static void text_copy_n(char* out, size_t capacity, const char* value,
                        size_t length) {
    if (!out || capacity == 0) return;
    if (!value) length = 0;
    if (length + 1 > capacity) length = capacity - 1;
    if (length != 0) memcpy(out, value, length);
    out[length] = '\0';
}

static void text_append_n(char* out, size_t capacity, const char* value,
                          size_t length) {
    const size_t used = strlen(out);
    if (used + 1 >= capacity) return;
    text_copy_n(out + used, capacity - used, value, length);
}

static void text_append(char* out, size_t capacity, const char* value) {
    text_append_n(out, capacity, value, value ? strlen(value) : 0);
}

static void text_append_char(char* out, size_t capacity, char value) {
    const size_t used = strlen(out);
    if (used + 1 >= capacity) return;
    out[used] = value;
    out[used + 1] = '\0';
}

static void text_pop(char* out) {
    const size_t used = strlen(out);
    if (used != 0) out[used - 1] = '\0';
}

static void text_append_u64(char* out, size_t capacity, uint64_t value) {
    char digits[24];
    int count = 0;
    if (value == 0) digits[count++] = '0';
    while (value != 0) {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (count != 0) text_append_char(out, capacity, digits[--count]);
}

static void copy_pz(char* out, size_t capacity, const pz_str* value) {
    if (!value) {
        if (capacity != 0) out[0] = '\0';
        return;
    }
    text_copy_n(out, capacity, value->data, value->length);
}

static char lower_char(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int text_icontains(const char* haystack, const char* needle) {
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

/* ------------------------------------------------------------- primitives */

static unsigned long alloc_color(Display* display, int screen, int red,
                                 int green, int blue) {
    XColor color;
    color.red = (unsigned short)(red * 257);
    color.green = (unsigned short)(green * 257);
    color.blue = (unsigned short)(blue * 257);
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, DefaultColormap(display, screen), &color))
        return BlackPixel(display, screen);
    return color.pixel;
}

static void fill(Ui* ui, int x, int y, int width, int height,
                 unsigned long color) {
    if (width <= 0 || height <= 0) return;
    XSetForeground(ui->display, ui->gc, color);
    XFillRectangle(ui->display, ui->target, ui->gc, x, y,
                   (unsigned)width, (unsigned)height);
}

static void frame(Ui* ui, int x, int y, int width, int height,
                  unsigned long color) {
    if (width <= 0 || height <= 0) return;
    XSetForeground(ui->display, ui->gc, color);
    XDrawRectangle(ui->display, ui->target, ui->gc, x, y,
                   (unsigned)max_int(0, width - 1),
                   (unsigned)max_int(0, height - 1));
}

static void draw_text(Ui* ui, XFontStruct* font, int x, int y,
                      const char* value, unsigned long color) {
    if (!font || !value || !*value) return;
    XSetFont(ui->display, ui->gc, font->fid);
    XSetForeground(ui->display, ui->gc, color);
    XDrawString(ui->display, ui->target, ui->gc, x, y, value,
                (int)strlen(value));
}

static int text_width(XFontStruct* font, const char* value) {
    return font && value ? XTextWidth(font, value, (int)strlen(value)) : 0;
}

static int baseline_for(XFontStruct* font, const Rect* rect) {
    if (!font) return rect->y + rect->h / 2;
    return rect->y + (rect->h + font->ascent - font->descent) / 2;
}

static void elide(XFontStruct* font, const char* value, int max_width,
                  char* out, size_t capacity) {
    if (!font || max_width <= 0) {
        text_copy(out, capacity, "");
        return;
    }
    if (text_width(font, value) <= max_width) {
        text_copy(out, capacity, value);
        return;
    }
    const size_t length = strlen(value);
    char candidate[PZ_MAX_LINE];
    for (size_t n = length; n > 0; --n) {
        text_copy_n(candidate, sizeof candidate, value, n);
        text_append(candidate, sizeof candidate, "...");
        if (text_width(font, candidate) <= max_width) {
            text_copy(out, capacity, candidate);
            return;
        }
    }
    text_copy(out, capacity, "...");
}

static void elide_tail(XFontStruct* font, const char* value, int max_width,
                       char* out, size_t capacity) {
    if (!font || max_width <= 0) {
        text_copy(out, capacity, "");
        return;
    }
    if (text_width(font, value) <= max_width) {
        text_copy(out, capacity, value);
        return;
    }
    const size_t length = strlen(value);
    char candidate[PZ_MAX_LINE];
    for (size_t start = 0; start < length; ++start) {
        text_copy(candidate, sizeof candidate, "...");
        text_append(candidate, sizeof candidate, value + start);
        if (text_width(font, candidate) <= max_width) {
            text_copy(out, capacity, candidate);
            return;
        }
    }
    text_copy(out, capacity, "...");
}

static void clean_pasted_text(const char* value, char* out, size_t capacity) {
    size_t length = strlen(value);
    for (size_t i = 0; i < length; ++i) {
        if (value[i] == '\0' || value[i] == '\r' || value[i] == '\n') {
            length = i;
            break;
        }
    }
    size_t begin = 0;
    while (begin < length && (value[begin] == ' ' || value[begin] == '\t'))
        ++begin;
    while (length > begin &&
           (value[length - 1] == ' ' || value[length - 1] == '\t'))
        --length;
    text_copy_n(out, capacity, value + begin, length - begin);
}

static void clean_workshop_paste(const char* value, char* out,
                                 size_t capacity) {
    size_t length = strlen(value);
    for (size_t i = 0; i < length; ++i) {
        if (value[i] == '\0') {
            length = i;
            break;
        }
    }
    text_copy(out, capacity, "");
    for (size_t i = 0; i < length; ++i) {
        const char c = value[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            const size_t used = strlen(out);
            if (used != 0 && out[used - 1] != ' ')
                text_append_char(out, capacity, ' ');
        } else {
            text_append_char(out, capacity, c);
        }
    }
    while (strlen(out) != 0 && out[strlen(out) - 1] == ' ')
        text_pop(out);
}

static unsigned long visual_channel(unsigned long mask, int value) {
    if (!mask) return 0;
    int shift = 0;
    while (((mask >> shift) & 1UL) == 0 && shift < 31) ++shift;
    const unsigned long max = mask >> shift;
    return (((unsigned long)value * max) / 255) << shift & mask;
}

static unsigned long visual_pixel(Visual* visual, int red, int green, int blue) {
    return visual_channel(visual->red_mask, red) |
           visual_channel(visual->green_mask, green) |
           visual_channel(visual->blue_mask, blue);
}

/* ---------------------------------------------------------------- caches */

static void clear_infos(Ui* ui) {
    for (int i = 0; i < ui->info_count; ++i)
        pz_mod_info_free(&ui->infos[i].info);
    ui->info_count = 0;
}

static void clear_icons(Ui* ui) {
    for (int i = 0; i < ui->icon_count; ++i)
        if (ui->icons[i].pixmap != None)
            XFreePixmap(ui->display, ui->icons[i].pixmap);
    ui->icon_count = 0;
    clear_infos(ui);
}

static void clear_target_icons(Ui* ui) {
    for (int i = 0; i < ui->target_icon_count; ++i)
        if (ui->target_icons[i].pixmap != None)
            XFreePixmap(ui->display, ui->target_icons[i].pixmap);
    ui->target_icon_count = 0;
}

static InfoEntry* find_info(Ui* ui, const char* folder) {
    for (int i = 0; i < ui->info_count; ++i)
        if (strcmp(ui->infos[i].folder, folder) == 0) return &ui->infos[i];
    return NULL;
}

static pz_mod_info* get_info(Ui* ui, const char* folder) {
    InfoEntry* existing = find_info(ui, folder);
    InfoEntry* grown;
    if (existing) return existing->ok ? &existing->info : NULL;
    grown = (InfoEntry*)grow_buffer(ui->infos, &ui->info_capacity,
                                    ui->info_count + 1, sizeof *grown);
    if (!grown) return NULL;
    ui->infos = grown;

    InfoEntry* entry = &ui->infos[ui->info_count++];
    text_copy(entry->folder, sizeof entry->folder, folder);
    pz_mod_info_init(&entry->info);
    entry->ok = 0;

    pz_path base;
    pz_path target;
    pz_path_init(&base);
    pz_path_init(&target);
    if (pz_path_assign_cstr(&base, ui->mods_dir) &&
        pz_path_join(&target, &base, folder, strlen(folder))) {
        entry->ok = pz_mod_info_read(&target, ui->game_version,
                                     strlen(ui->game_version), &entry->info);
    }
    pz_path_free(&target);
    pz_path_free(&base);
    if (!entry->ok) pz_mod_info_free(&entry->info);
    return entry->ok ? &entry->info : NULL;
}

static IconEntry* find_icon(IconEntry* entries, int count, const char* key) {
    for (int i = 0; i < count; ++i)
        if (strcmp(entries[i].key, key) == 0) return &entries[i];
    return NULL;
}

static Pixmap icon_to_pixmap(Ui* ui, const pz_mod_icon* icon,
                             unsigned long background) {
    Visual* visual = DefaultVisual(ui->display, ui->screen);
    if (!visual || visual->class != TrueColor || icon->size <= 0) return None;

    XColor background_color;
    background_color.pixel = background;
    XQueryColor(ui->display, DefaultColormap(ui->display, ui->screen),
                &background_color);
    const int bg_red = background_color.red >> 8;
    const int bg_green = background_color.green >> 8;
    const int bg_blue = background_color.blue >> 8;

    XImage* image = XCreateImage(
        ui->display, visual, (unsigned)DefaultDepth(ui->display, ui->screen),
        ZPixmap, 0, NULL, (unsigned)icon->size, (unsigned)icon->size, 32, 0);
    if (!image) return None;
    image->data = (char*)calloc((size_t)image->bytes_per_line,
                                (size_t)image->height);
    if (!image->data) {
        XDestroyImage(image);
        return None;
    }

    for (int y = 0; y < icon->size; ++y) {
        for (int x = 0; x < icon->size; ++x) {
            const size_t p =
                ((size_t)y * (size_t)icon->size + (size_t)x) * 4;
            const int a = icon->rgba[p + 3];
            const int red = (icon->rgba[p] * a + bg_red * (255 - a)) / 255;
            const int green =
                (icon->rgba[p + 1] * a + bg_green * (255 - a)) / 255;
            const int blue =
                (icon->rgba[p + 2] * a + bg_blue * (255 - a)) / 255;
            XPutPixel(image, x, y, visual_pixel(visual, red, green, blue));
        }
    }

    Pixmap pixmap = XCreatePixmap(
        ui->display, ui->window, (unsigned)icon->size,
        (unsigned)icon->size, (unsigned)DefaultDepth(ui->display, ui->screen));
    if (pixmap != None)
        XPutImage(ui->display, pixmap, ui->gc, image, 0, 0, 0, 0,
                  (unsigned)icon->size, (unsigned)icon->size);
    XDestroyImage(image);
    return pixmap;
}

static IconEntry* get_icon(Ui* ui, const char* folder) {
    IconEntry* existing = find_icon(ui->icons, ui->icon_count, folder);
    IconEntry* grown;
    if (existing) return existing;
    grown = (IconEntry*)grow_buffer(ui->icons, &ui->icon_capacity,
                                    ui->icon_count + 1, sizeof *grown);
    if (!grown) return NULL;
    ui->icons = grown;

    IconEntry* entry = &ui->icons[ui->icon_count++];
    text_copy(entry->key, sizeof entry->key, folder);
    entry->pixmap = None;
    entry->width = 0;
    entry->height = 0;

    pz_mod_info* info = get_info(ui, folder);
    if (!info || info->icon.length == 0) return entry;

    pz_mod_icon icon;
    if (pz_mod_icon_load(info->icon.data, info->icon.length, 48, &icon)) {
        entry->pixmap = icon_to_pixmap(ui, &icon, ui->pal.surface);
        if (entry->pixmap != None) {
            entry->width = (unsigned)icon.size;
            entry->height = (unsigned)icon.size;
        }
        pz_mod_icon_free(&icon);
    }
    return entry;
}

static IconEntry* get_target_icon(Ui* ui,
                                  const pz_activation_target_data* target) {
    if (target->kind != 1 || target->thumbnail.length == 0) return NULL;
    char key[1024];
    copy_pz(key, sizeof key, &target->thumbnail);
    IconEntry* existing =
        find_icon(ui->target_icons, ui->target_icon_count, key);
    IconEntry* grown;
    if (existing) return existing;
    grown = (IconEntry*)grow_buffer(ui->target_icons,
                                    &ui->target_icon_capacity,
                                    ui->target_icon_count + 1, sizeof *grown);
    if (!grown) return NULL;
    ui->target_icons = grown;

    IconEntry* entry = &ui->target_icons[ui->target_icon_count++];
    text_copy(entry->key, sizeof entry->key, key);
    entry->pixmap = None;
    entry->width = 0;
    entry->height = 0;

    pz_mod_icon icon;
    if (pz_mod_icon_load(target->thumbnail.data, target->thumbnail.length, 24,
                         &icon)) {
        entry->pixmap = icon_to_pixmap(ui, &icon, ui->pal.surface);
        if (entry->pixmap != None) {
            entry->width = (unsigned)icon.size;
            entry->height = (unsigned)icon.size;
        }
        pz_mod_icon_free(&icon);
    }
    return entry;
}

/* ---------------------------------------------------------------- rows */

static void copy_target_strings(Ui* ui) {
    for (int i = 0; i < ui->target_count; ++i)
        pz_activation_target_data_free(&ui->targets[i]);
    ui->target_count = 0;

    pz_vec values;
    pz_vec_init(&values, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(ui->manager, &values);
    for (size_t i = 0; i < values.length; ++i) {
        pz_activation_target_data* value =
            (pz_activation_target_data*)pz_vec_at(&values, i);
        if (ui->target_count < PZ_MAX_TARGETS)
            ui->targets[ui->target_count++] = *value;
        else
            pz_activation_target_data_free(value);
    }
    pz_vec_free(&values);
}

static Row* find_row(Ui* ui, uint64_t id) {
    for (int i = 0; i < ui->row_count; ++i)
        if (ui->rows[i].workshop_id == id) return &ui->rows[i];
    return NULL;
}

static void rebuild_rows(Ui* ui) {
    pz_vec mods;
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list_all(ui->manager, &mods);
    if (!ensure_row_capacity(ui, (int)mods.length)) {
        pz_mod_manager_free_mods(&mods);
        return;
    }
    ui->row_count = 0;
    for (size_t i = 0; i < mods.length; ++i) {
        const pz_installed_mod* mod = pz_vec_at_const(&mods, i);
        Row* row = mod->workshop_id != 0 ? find_row(ui, mod->workshop_id) : NULL;
        if (!row) {
            row = &ui->rows[ui->row_count++];
            memset(row, 0, sizeof *row);
            row->workshop_id = mod->workshop_id;
            copy_pz(row->name, sizeof row->name, &mod->name);
        }
        if (row->folder_count < PZ_MAX_FOLDERS) {
            copy_pz(row->folders[row->folder_count], sizeof row->folders[0],
                    &mod->folder);
            ++row->folder_count;
        }
        if (mod->time_updated > row->time_updated)
            row->time_updated = mod->time_updated;
        row->locked |= mod->locked != 0;
        row->imported |= mod->imported != 0;
    }
    pz_mod_manager_free_mods(&mods);

    for (int i = 0; i < ui->row_count; ++i) {
        Row* row = &ui->rows[i];
        row->update_available = row->workshop_id != 0 &&
            pz_mod_manager_update_available(ui->manager, row->workshop_id);
        row->locked = row->locked ||
            (row->workshop_id != 0 &&
             pz_mod_manager_locked(ui->manager, row->workshop_id));
    }

    copy_target_strings(ui);
    ui->filter_total = ui->row_count;
    ui->filter_updates = 0;
    ui->filter_locked = 0;
    ui->filter_nontracked = 0;
    for (int i = 0; i < ui->row_count; ++i) {
        if (ui->rows[i].update_available) ++ui->filter_updates;
        if (ui->rows[i].locked) ++ui->filter_locked;
        if (ui->rows[i].workshop_id == 0) ++ui->filter_nontracked;
    }
    ui->list_dirty = 1;
}

static void rebuild_visible(Ui* ui) {
    ui->visible_count = 0;
    for (int i = 0; i < ui->row_count; ++i) {
        Row* row = &ui->rows[i];
        if (ui->filter_mode == FilterUpdates && !row->update_available) continue;
        if (ui->filter_mode == FilterLocked && !row->locked) continue;
        if (ui->filter_mode == FilterNontracked && row->workshop_id != 0) continue;
        if (ui->filter_text[0] != '\0') {
            char haystack[1024];
            text_copy(haystack, sizeof haystack, row->name);
            for (int f = 0; f < row->folder_count; ++f) {
                text_append_char(haystack, sizeof haystack, ' ');
                text_append(haystack, sizeof haystack, row->folders[f]);
            }
            if (!text_icontains(haystack, ui->filter_text)) continue;
        }
        ui->visible[ui->visible_count++] = i;
    }

    for (int i = 1; i < ui->visible_count; ++i) {
        const int value = ui->visible[i];
        int j = i;
        while (j > 0) {
            const Row* a = &ui->rows[ui->visible[j - 1]];
            const Row* b = &ui->rows[value];
            int before;
            if (ui->sort_mode && a->time_updated != b->time_updated)
                before = b->time_updated > a->time_updated;
            else
                before = strcasecmp(b->name, a->name) < 0;
            if (!before) break;
            ui->visible[j] = ui->visible[j - 1];
            --j;
        }
        ui->visible[j] = value;
    }

    ui->list_offset = min_int(max_int(0, ui->list_offset),
                              max_int(0, ui->visible_count - PZ_ROWS));
    for (int i = 0; i < ui->visible_count; ++i) {
        Row* row = &ui->rows[ui->visible[i]];
        ui->row_enabled[i] = 0;
        for (int f = 0; f < row->folder_count && !ui->row_enabled[i]; ++f) {
            pz_mod_info* info = get_info(ui, row->folders[f]);
            if (!info || info->id.length == 0) continue;
            for (int t = 0; t < ui->target_count && !ui->row_enabled[i]; ++t) {
                pz_activation_target_data* target = &ui->targets[t];
                for (size_t m = 0; m < target->mod_ids.length; ++m) {
                    const pz_str* id = pz_vec_at_const(&target->mod_ids, m);
                    if (pz_str_equals(id, info->id.data, info->id.length)) {
                        ui->row_enabled[i] = 1;
                        break;
                    }
                }
            }
        }
    }
    if (ui->filter_mode == FilterUpdates && ui->filter_updates == 0)
        ui->filter_mode = FilterAll;
    if (ui->filter_mode == FilterLocked && ui->filter_locked == 0)
        ui->filter_mode = FilterAll;
    if (ui->filter_mode == FilterNontracked && ui->filter_nontracked == 0)
        ui->filter_mode = FilterAll;
    if (ui->selected_count == 0 && ui->selected_folder_count == 0)
        ui->confirm_remove = 0;
    ui->list_dirty = 0;
}

/* ------------------------------------------------------------ selection */

static int row_selected(Ui* ui, const Row* row) {
    if (row->workshop_id != 0) {
        for (int i = 0; i < ui->selected_count; ++i)
            if (ui->selected_ids[i] == row->workshop_id) return 1;
        return 0;
    }
    for (int i = 0; i < ui->selected_folder_count; ++i)
        if (strcmp(ui->selected_folders[i], row->folders[0]) == 0) return 1;
    return 0;
}

static void select_row(Ui* ui, const Row* row) {
    if (row->workshop_id != 0) {
        for (int i = 0; i < ui->selected_count; ++i)
            if (ui->selected_ids[i] == row->workshop_id) return;
        if (ui->selected_count < PZ_MAX_IDS)
            ui->selected_ids[ui->selected_count++] = row->workshop_id;
    } else {
        for (int i = 0; i < ui->selected_folder_count; ++i)
            if (strcmp(ui->selected_folders[i], row->folders[0]) == 0) return;
        if (ui->selected_folder_count >= PZ_MAX_IDS) return;
        {
            char (*grown)[256] = (char(*)[256])grow_buffer(
                ui->selected_folders, &ui->selected_folder_capacity,
                ui->selected_folder_count + 1, 256);
            if (!grown) return;
            ui->selected_folders = grown;
        }
        text_copy(ui->selected_folders[ui->selected_folder_count++],
                  sizeof ui->selected_folders[0], row->folders[0]);
    }
}

static void unselect_row(Ui* ui, const Row* row) {
    if (row->workshop_id != 0) {
        for (int i = 0; i < ui->selected_count; ++i) {
            if (ui->selected_ids[i] == row->workshop_id) {
                ui->selected_ids[i] = ui->selected_ids[--ui->selected_count];
                return;
            }
        }
    } else {
        for (int i = 0; i < ui->selected_folder_count; ++i) {
            if (strcmp(ui->selected_folders[i], row->folders[0]) == 0) {
                text_copy(ui->selected_folders[i],
                          sizeof ui->selected_folders[0],
                          ui->selected_folders[--ui->selected_folder_count]);
                return;
            }
        }
    }
}

static void clear_selection(Ui* ui) {
    ui->selected_count = 0;
    ui->selected_folder_count = 0;
}

static int selected_any(Ui* ui) {
    return ui->selected_count != 0 || ui->selected_folder_count != 0;
}

/* --------------------------------------------------------------- logging */

static void push_log(Ui* ui, const char* line) {
    int index;
    if (ui->log_count < PZ_MAX_LOG) {
        index = (ui->log_start + ui->log_count) % PZ_MAX_LOG;
        pz_str_assign_cstr(&ui->log[index], line ? line : "");
        ++ui->log_count;
    } else {
        index = ui->log_start;
        pz_str_clear(&ui->log[index]);
        pz_str_assign_cstr(&ui->log[index], line ? line : "");
        ui->log_start = (ui->log_start + 1) % PZ_MAX_LOG;
    }
}

static const char* log_at(Ui* ui, int offset) {
    const pz_str* line = &ui->log[(ui->log_start + offset) % PZ_MAX_LOG];
    return line->data ? line->data : "";
}

static void wake_ui(Ui* ui) {
    const char byte = 1;
    ssize_t ignored = write(ui->wake[1], &byte, 1);
    (void)ignored;
}

static void worker_log(void* context, const char* line) {
    Ui* ui = (Ui*)context;
    pthread_mutex_lock(&ui->pending_mu);
    if (ui->pending_count < PZ_MAX_PENDING)
        text_copy(ui->pending[ui->pending_count++], sizeof ui->pending[0],
                  line);
    pthread_mutex_unlock(&ui->pending_mu);
    wake_ui(ui);
}

/* -------------------------------------------------------------- palette */

static void init_palette(Ui* ui) {
    Display* d = ui->display;
    const int s = ui->screen;
    Palette* p = &ui->light_pal;
    p->bg = alloc_color(d, s, 246, 247, 249);
    p->surface = alloc_color(d, s, 255, 255, 255);
    p->field = alloc_color(d, s, 255, 255, 255);
    p->border = alloc_color(d, s, 220, 225, 232);
    p->border_strong = alloc_color(d, s, 194, 202, 214);
    p->text = alloc_color(d, s, 31, 38, 46);
    p->muted = alloc_color(d, s, 108, 118, 132);
    p->accent = alloc_color(d, s, 224, 100, 43);
    p->accent_hover = alloc_color(d, s, 198, 82, 30);
    p->accent_soft = alloc_color(d, s, 255, 242, 234);
    p->danger = alloc_color(d, s, 190, 55, 55);
    p->danger_soft = alloc_color(d, s, 253, 237, 237);
    p->brand = alloc_color(d, s, 44, 99, 69);
    p->brand_soft = alloc_color(d, s, 235, 244, 238);
    p->brand_border = alloc_color(d, s, 182, 204, 188);
    p->cream = alloc_color(d, s, 255, 248, 234);
    p->hover = alloc_color(d, s, 244, 248, 245);
    p->success = alloc_color(d, s, 36, 122, 80);
    p->on_accent = p->surface;

    Palette* q = &ui->dark_pal;
    q->bg = alloc_color(d, s, 18, 21, 24);
    q->surface = alloc_color(d, s, 28, 32, 36);
    q->field = alloc_color(d, s, 22, 26, 30);
    q->border = alloc_color(d, s, 58, 64, 70);
    q->border_strong = alloc_color(d, s, 78, 86, 94);
    q->text = alloc_color(d, s, 232, 236, 240);
    q->muted = alloc_color(d, s, 154, 163, 173);
    q->accent = alloc_color(d, s, 232, 116, 58);
    q->accent_hover = alloc_color(d, s, 245, 132, 72);
    q->accent_soft = alloc_color(d, s, 58, 36, 27);
    q->danger = alloc_color(d, s, 230, 100, 100);
    q->danger_soft = alloc_color(d, s, 52, 28, 30);
    q->brand = alloc_color(d, s, 96, 180, 130);
    q->brand_soft = alloc_color(d, s, 26, 46, 36);
    q->brand_border = alloc_color(d, s, 64, 110, 84);
    q->cream = alloc_color(d, s, 255, 248, 234);
    q->hover = alloc_color(d, s, 36, 44, 40);
    q->success = alloc_color(d, s, 96, 180, 130);
    q->on_accent = q->cream;
}

static void apply_palette(Ui* ui) {
    ui->pal = ui->settings_dark ? ui->dark_pal : ui->light_pal;
}

/* -------------------------------------------------------------- buttons */

static Rect button_rect(Ui* ui, Btn button) {
    if (ui->targets_open) {
        const int x = PZ_SETTINGS_FIELD_X;
        const int y = PZ_TARGETS_BUTTONS_Y;
        if (button == BtnEnableTarget) return (Rect){x, y, 108, 38};
        if (button == BtnDisableTarget) return (Rect){x + 120, y, 108, 38};
        if (button == BtnShareTargets) return (Rect){x + 240, y, 130, 38};
        if (button == BtnCopyNames) return (Rect){x + 382, y, 108, 38};
        if (button == BtnCloseTargets) return (Rect){x + 502, y, 108, 38};
        return (Rect){0, 0, 0, 0};
    }
    if (ui->settings_open) {
        if (button == BtnSave) return (Rect){PZ_SETTINGS_FIELD_X, PZ_SETTINGS_BUTTONS_Y, 120, 38};
        if (button == BtnCancel) return (Rect){PZ_SETTINGS_FIELD_X + 136, PZ_SETTINGS_BUTTONS_Y, 120, 38};
        if (button == BtnTheme) return (Rect){PZ_SETTINGS_FIELD_X + 280, PZ_SETTINGS_BUTTONS_Y, 190, 38};
        return (Rect){0, 0, 0, 0};
    }
    switch (button) {
    case BtnInstall: return (Rect){PZ_INSTALL_X, PZ_INPUT_Y, PZ_INSTALL_W, PZ_INPUT_H};
    case BtnImport: return (Rect){PZ_IMPORT_X, PZ_INPUT_Y, PZ_IMPORT_W, PZ_INPUT_H};
    case BtnEnable: return (Rect){688, 24, 100, 30};
    case BtnShareServer: return (Rect){560, 24, 116, 30};
    case BtnSettings: return (Rect){800, 24, 96, 30};
    case BtnSelectAll: return (Rect){24, PZ_ACTION_Y, 116, PZ_ACTION_H};
    case BtnClear: return (Rect){148, PZ_ACTION_Y, 116, PZ_ACTION_H};
    case BtnLock: return (Rect){272, PZ_ACTION_Y, 108, PZ_ACTION_H};
    case BtnCopy: return (Rect){392, PZ_ACTION_Y, 104, PZ_ACTION_H};
    case BtnUpdate: return (Rect){506, PZ_ACTION_Y, 120, PZ_ACTION_H};
    case BtnUpdateAll: return (Rect){636, PZ_ACTION_Y, 120, PZ_ACTION_H};
    case BtnRemove: return (Rect){766, PZ_ACTION_Y, 130, PZ_ACTION_H};
    case BtnFilterMenu: return (Rect){PZ_FILTER_MENU_X, PZ_FILTER_Y, PZ_FILTER_MENU_W, PZ_FILTER_H};
    case BtnCheck: return (Rect){492, PZ_FILTER_Y, 120, PZ_FILTER_H};
    case BtnSort: return (Rect){622, PZ_FILTER_Y, 140, PZ_FILTER_H};
    case BtnSelfUpdate: return (Rect){440, 24, 108, 30};
    default: return (Rect){0, 0, 0, 0};
    }
}

static int button_enabled(Ui* ui, Btn button) {
    if (button == BtnNothing) return 0;
    if (ui->targets_open) {
        if (button == BtnCloseTargets) return 1;
        if (button == BtnShareTargets) return !ui->busy && ui->target_focus >= 0;
        if (button == BtnCopyNames)
            return !ui->busy && ui->target_focus >= 0 &&
                   ui->targets[ui->target_focus].mod_ids.length != 0;
        if (button == BtnEnableTarget || button == BtnDisableTarget)
            return !ui->busy && ui->target_focus >= 0 &&
                   ui->target_mod_id_count != 0;
        return 0;
    }
    if (ui->settings_open)
        return button == BtnSave || button == BtnCancel || button == BtnTheme;
    if (ui->busy) return 0;
    switch (button) {
    case BtnInstall:
    case BtnImport:
    case BtnSettings:
    case BtnShareServer:
    case BtnUpdateAll: return 1;
    case BtnSelfUpdate: return ui->app_update_available;
    case BtnUpdate:
    case BtnRemove:
    case BtnLock:
    case BtnCopy: return ui->selected_count != 0;
    case BtnEnable:
    case BtnClear: return selected_any(ui);
    case BtnSelectAll: return ui->visible_count != 0;
    case BtnFilterMenu:
    case BtnSort:
    case BtnCheck: return 1;
    default: return 0;
    }
}

static void button_label(Ui* ui, Btn button, char* out, size_t capacity) {
    switch (button) {
    case BtnInstall: text_copy(out, capacity, "Install"); break;
    case BtnImport: text_copy(out, capacity, "Import"); break;
    case BtnSettings: text_copy(out, capacity, "Settings"); break;
    case BtnUpdate: text_copy(out, capacity, "Update"); break;
    case BtnRemove: text_copy(out, capacity, ui->confirm_remove ? "Confirm remove" : "Remove"); break;
    case BtnUpdateAll: text_copy(out, capacity, "Update all"); break;
    case BtnSelectAll: text_copy(out, capacity, "Select all"); break;
    case BtnClear: text_copy(out, capacity, "Deselect"); break;
    case BtnCopy: text_copy(out, capacity, "Copy IDs"); break;
    case BtnLock: {
        int all_locked = ui->selected_count != 0;
        for (int i = 0; i < ui->selected_count; ++i)
            if (!pz_mod_manager_manually_locked(ui->manager, ui->selected_ids[i])) {
                all_locked = 0;
                break;
            }
        text_copy(out, capacity, all_locked ? "Unlock" : "Lock");
        break;
    }
    case BtnEnable: text_copy(out, capacity, "Enable..."); break;
    case BtnShareServer: text_copy(out, capacity, "Share server"); break;
    case BtnSelfUpdate: text_copy(out, capacity, "Update app"); break;
    case BtnEnableTarget: text_copy(out, capacity, "Enable"); break;
    case BtnDisableTarget: text_copy(out, capacity, "Disable"); break;
    case BtnCopyNames: text_copy(out, capacity, "Copy names"); break;
    case BtnShareTargets:
        text_copy(out, capacity,
                  ui->target_focus >= 0 && ui->target_focus < ui->target_count &&
                          ui->targets[ui->target_focus].kind == 2
                      ? "Copy server IDs" : "Copy IDs");
        break;
    case BtnCloseTargets: text_copy(out, capacity, "Close"); break;
    case BtnSave: text_copy(out, capacity, "Save"); break;
    case BtnCancel: text_copy(out, capacity, "Cancel"); break;
    case BtnFilterMenu: {
        static const char* names[] = {"All", "Updates", "Locked", "Nontracked"};
        text_copy(out, capacity, names[ui->filter_mode]);
        break;
    }
    case BtnSort: text_copy(out, capacity, ui->sort_mode ? "Sort: Updated" : "Sort: Name"); break;
    case BtnTheme: text_copy(out, capacity, ui->settings_dark ? "Theme: Dark" : "Theme: Light"); break;
    case BtnCheck: text_copy(out, capacity, "Check updates"); break;
    default: text_copy(out, capacity, ""); break;
    }
}

static int button_glyph(Btn button) {
    switch (button) {
    case BtnInstall: return PzGlyphInstall;
    case BtnImport: return PzGlyphFolder;
    case BtnUpdate:
    case BtnSelfUpdate: return PzGlyphUpdate;
    case BtnRemove: return PzGlyphRemove;
    case BtnUpdateAll: return PzGlyphUpdateAll;
    case BtnSelectAll: return PzGlyphSelectAll;
    case BtnClear:
    case BtnCancel:
    case BtnDisableTarget:
    case BtnCloseTargets: return PzGlyphClear;
    case BtnLock: return PzGlyphLock;
    case BtnCopy:
    case BtnCopyNames:
    case BtnShareTargets: return PzGlyphCopy;
    case BtnFilterMenu: return PzGlyphFilter;
    case BtnSort: return PzGlyphSort;
    case BtnCheck:
    case BtnEnable:
    case BtnEnableTarget: return PzGlyphCheck;
    case BtnSettings: return PzGlyphSettings;
    case BtnSave: return PzGlyphSave;
    case BtnTheme: return PzGlyphTheme;
    case BtnShareServer: return PzGlyphServer;
    default: return -1;
    }
}

static void draw_glyph(Ui* ui, int glyph, int x, int y, unsigned long color) {
    if (glyph < 0 || glyph >= PzGlyphCount) return;
    XSetForeground(ui->display, ui->gc, color);
    for (int row = 0; row < 16; ++row) {
        const char* pixels = pz_glyphs[glyph].rows[row];
        for (int column = 0; column < 16 && pixels[column]; ++column)
            if (pixels[column] == '#')
                XDrawPoint(ui->display, ui->target, ui->gc, x + column,
                           y + row);
    }
}

static void draw_button(Ui* ui, Btn button) {
    const Rect rect = button_rect(ui, button);
    if (rect.w == 0) return;
    const int enabled = button_enabled(ui, button);
    const int primary = button == BtnInstall || button == BtnSave ||
                        button == BtnEnableTarget;
    const int danger = button == BtnRemove && ui->confirm_remove;
    const int active = button == BtnFilterMenu &&
                       (ui->filter_mode != FilterAll || ui->filter_menu_open);
    const int hot = enabled && ui->hover_button == (int)button;
    unsigned long fill_color = ui->pal.surface;
    unsigned long border_color = ui->pal.border;
    unsigned long text_color = ui->pal.text;
    unsigned long icon_color = ui->pal.brand;
    if (!enabled) {
        fill_color = ui->pal.bg;
        text_color = ui->pal.muted;
        icon_color = ui->pal.muted;
    } else if (danger) {
        fill_color = ui->pal.danger_soft;
        border_color = ui->pal.danger;
        text_color = ui->pal.danger;
        icon_color = ui->pal.danger;
    } else if (active) {
        fill_color = ui->pal.brand_soft;
        border_color = ui->pal.brand;
        text_color = ui->pal.brand;
        icon_color = ui->pal.brand;
    } else if (primary) {
        fill_color = hot ? ui->pal.accent_hover : ui->pal.accent;
        border_color = fill_color;
        text_color = ui->pal.on_accent;
        icon_color = ui->pal.on_accent;
    } else if (button == BtnSelfUpdate) {
        fill_color = hot ? ui->pal.brand : ui->pal.success;
        border_color = fill_color;
        text_color = ui->pal.on_accent;
        icon_color = ui->pal.on_accent;
    } else if (hot) {
        fill_color = ui->pal.hover;
        border_color = ui->pal.brand_border;
    }
    fill(ui, rect.x, rect.y, rect.w, rect.h, fill_color);
    frame(ui, rect.x, rect.y, rect.w, rect.h, border_color);
    char label[64];
    button_label(ui, button, label, sizeof label);
    const int glyph = button_glyph(button);
    const int label_width = text_width(ui->body, label);
    const int icon_w = 18, gap = 7;
    int label_x = rect.x + (rect.w - label_width) / 2;
    if (glyph >= 0) {
        const int start = rect.x + (rect.w - icon_w - gap - label_width) / 2;
        draw_glyph(ui, glyph, start + 1, rect.y + (rect.h - 16) / 2,
                   icon_color);
        label_x = start + icon_w + gap;
    }
    draw_text(ui, ui->body, label_x, baseline_for(ui->body, &rect), label,
              text_color);
    if (button == BtnFilterMenu) {
        const int ax = rect.x + rect.w - 17;
        const int ay = rect.y + (ui->filter_menu_open ? 12 : 10);
        const int dy = ui->filter_menu_open ? -4 : 4;
        XSetForeground(ui->display, ui->gc, text_color);
        XSetLineAttributes(ui->display, ui->gc, 1, LineSolid, CapButt, JoinMiter);
        XDrawLine(ui->display, ui->target, ui->gc, ax, ay, ax + 4, ay + dy);
        XDrawLine(ui->display, ui->target, ui->gc, ax + 4, ay + dy, ax + 8, ay);
    }
}

static Btn button_at(Ui* ui, int x, int y) {
    if (ui->targets_open) {
        const Btn buttons[] = {BtnEnableTarget, BtnDisableTarget, BtnShareTargets,
                               BtnCopyNames, BtnCloseTargets};
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
            if (!button_enabled(ui, buttons[i])) continue;
            const Rect rect = button_rect(ui, buttons[i]);
            if (in_rect(x, y, rect.x, rect.y, rect.w, rect.h)) return buttons[i];
        }
        return BtnNothing;
    }
    if (ui->settings_open) {
        const Btn buttons[] = {BtnSave, BtnCancel, BtnTheme};
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
            if (!button_enabled(ui, buttons[i])) continue;
            const Rect rect = button_rect(ui, buttons[i]);
            if (in_rect(x, y, rect.x, rect.y, rect.w, rect.h)) return buttons[i];
        }
        return BtnNothing;
    }
    const Btn buttons[] = {BtnInstall, BtnImport, BtnShareServer, BtnSelfUpdate,
                           BtnEnable, BtnSettings, BtnUpdate, BtnRemove,
                           BtnUpdateAll, BtnSelectAll, BtnClear, BtnLock,
                           BtnCopy, BtnFilterMenu, BtnSort, BtnCheck};
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; ++i) {
        if (!button_enabled(ui, buttons[i])) continue;
        const Rect rect = button_rect(ui, buttons[i]);
        if (in_rect(x, y, rect.x, rect.y, rect.w, rect.h)) return buttons[i];
    }
    return BtnNothing;
}

/* --------------------------------------------------------------- drawing */

static void format_date(uint64_t epoch, char* out, size_t capacity) {
    out[0] = '\0';
    if (epoch == 0) return;
    const time_t when = (time_t)epoch;
    const struct tm* value = localtime(&when);
    if (!value) return;
    if (strftime(out, capacity, "%Y-%m-%d", value) == 0) out[0] = '\0';
}

static void activation_name(const pz_activation_target_data* target, char* out,
                            size_t capacity) {
    if (target->kind == 2) {
        text_copy(out, capacity, "Server: ");
        text_append_n(out, capacity, target->name.data, target->name.length);
    } else if (target->kind == 1) {
        text_copy_n(out, capacity, target->name.data, target->name.length);
    } else {
        text_copy(out, capacity, "Default");
    }
}

static int target_has_id(const pz_activation_target_data* target,
                         const pz_str* id) {
    for (size_t i = 0; i < target->mod_ids.length; ++i) {
        const pz_str* value = pz_vec_at_const(&target->mod_ids, i);
        if (pz_str_equals(value, id->data, id->length)) return 1;
    }
    return 0;
}

static void enabled_in_row(Ui* ui, Row* row, char* out, size_t capacity) {
    out[0] = '\0';
    for (int t = 0; t < ui->target_count; ++t) {
        int enabled = 0;
        for (int f = 0; f < row->folder_count && !enabled; ++f) {
            pz_mod_info* info = get_info(ui, row->folders[f]);
            if (info && info->id.length != 0)
                enabled = target_has_id(&ui->targets[t], &info->id);
        }
        if (!enabled) continue;
        if (out[0] != '\0') text_append(out, capacity, ", ");
        char name[256];
        activation_name(&ui->targets[t], name, sizeof name);
        text_append(out, capacity, name);
    }
}

static void mod_meta(Ui* ui, Row* row, char* out, size_t capacity) {
    out[0] = '\0';
    if (row->folder_count == 1)
        text_append(out, capacity, row->folders[0]);
    if (row->imported) {
        if (out[0]) text_append(out, capacity, "  |  ");
        text_append(out, capacity, "Imported");
    }
    if (row->locked) {
        if (out[0]) text_append(out, capacity, "  |  ");
        text_append(out, capacity, "Locked");
    }
    char on[512];
    enabled_in_row(ui, row, on, sizeof on);
    if (on[0]) {
        if (out[0]) text_append(out, capacity, "  |  ");
        text_append(out, capacity, "On: ");
        text_append(out, capacity, on);
    }
    char date[32];
    format_date(row->time_updated, date, sizeof date);
    if (date[0]) {
        if (out[0]) text_append(out, capacity, "  |  ");
        text_append(out, capacity, "Updated ");
        text_append(out, capacity, date);
    }
    pz_mod_info* info = get_info(ui, row->folders[0]);
    if (info && !pz_mod_info_supports_game_version(info, ui->game_version,
                                                   strlen(ui->game_version))) {
        pz_str range;
        pz_str_init(&range);
        if (pz_mod_info_version_range(info, &range)) {
            if (out[0]) text_append(out, capacity, "  |  ");
            text_append_n(out, capacity, range.data, range.length);
        }
        pz_str_free(&range);
    }
}

static void draw_input(Ui* ui, Rect rect, const char* value,
                       const char* placeholder, int focused, int enabled,
                       int right_pad) {
    fill(ui, rect.x, rect.y, rect.w, rect.h, ui->pal.field);
    frame(ui, rect.x, rect.y, rect.w, rect.h,
          focused && enabled ? ui->pal.accent : ui->pal.border);
    const int empty = value[0] == '\0';
    const int text_x = rect.x + 12;
    const int inner_w = rect.w - 24 - right_pad;
    char shown[PZ_MAX_LINE];
    if (empty)
        elide(ui->body, placeholder, inner_w, shown, sizeof shown);
    else
        elide_tail(ui->body, value, inner_w, shown, sizeof shown);
    draw_text(ui, ui->body, text_x, baseline_for(ui->body, &rect), shown,
              empty ? ui->pal.muted : ui->pal.text);
    if (focused && enabled && !empty) {
        const int caret_x = text_x + text_width(ui->body, shown) + 1;
        fill(ui, caret_x, rect.y + 10, 2, rect.h - 20, ui->pal.accent);
    }
}

static void scroll_metrics(int total, int visible_count, int top, int bottom,
                           int offset, int* thumb_y, int* thumb_h) {
    if (total <= visible_count) {
        *thumb_y = 0;
        *thumb_h = 0;
        return;
    }
    const int track = bottom - top;
    *thumb_h = max_int(PZ_SCROLL_MIN_THUMB, track * visible_count / total);
    const int travel = track - *thumb_h;
    const int max_offset = total - visible_count;
    const int clamped = min_int(max_int(0, offset), max_offset);
    *thumb_y = top + travel * clamped / max_offset;
}

static int scrollbar_metrics(Ui* ui, int* thumb_y, int* thumb_h) {
    scroll_metrics(ui->visible_count, PZ_ROWS, PZ_LIST_TOP, PZ_LIST_BOTTOM,
                   ui->list_offset, thumb_y, thumb_h);
    return ui->visible_count > PZ_ROWS;
}

static int activity_metrics(Ui* ui, int* thumb_y, int* thumb_h) {
    scroll_metrics(ui->log_count, PZ_ACTIVITY_LINES, PZ_ACTIVITY_TRACK_TOP,
                   PZ_ACTIVITY_TRACK_BOTTOM, ui->activity_offset, thumb_y, thumb_h);
    return ui->log_count > PZ_ACTIVITY_LINES;
}

static int scrollbar_at(int x, int y) {
    return x >= PZ_SCROLL_X - PZ_SCROLL_GRAB &&
           x <= PZ_SCROLL_X + PZ_SCROLL_W + PZ_SCROLL_GRAB &&
           y >= PZ_LIST_TOP && y < PZ_LIST_BOTTOM;
}

static int activity_bar_at(int x, int y) {
    return x >= PZ_ACTIVITY_SCROLL_X - PZ_SCROLL_GRAB &&
           x <= PZ_ACTIVITY_SCROLL_X + PZ_SCROLL_W + PZ_SCROLL_GRAB &&
           y >= PZ_ACTIVITY_TRACK_TOP && y < PZ_ACTIVITY_TRACK_BOTTOM;
}

static int row_at(Ui* ui, int x, int y) {
    if (x < PZ_MARGIN || x > PZ_MARGIN + PZ_CARD_W || y < PZ_LIST_TOP ||
        y > PZ_LIST_BOTTOM)
        return -1;
    const int slot = (y - PZ_LIST_TOP) / (PZ_ROW_H + PZ_ROW_GAP);
    if (slot < 0 || slot >= PZ_ROWS) return -1;
    if ((y - PZ_LIST_TOP) % (PZ_ROW_H + PZ_ROW_GAP) >= PZ_ROW_H) return -1;
    const int index = ui->list_offset + slot;
    return index >= 0 && index < ui->visible_count ? index : -1;
}

static Rect filter_menu_rect(Ui* ui) {
    int rows = 1;
    if (ui->filter_updates > 0) ++rows;
    if (ui->filter_locked > 0) ++rows;
    if (ui->filter_nontracked > 0) ++rows;
    return (Rect){PZ_FILTER_MENU_X, PZ_FILTER_Y + PZ_FILTER_H + 2,
                  PZ_FILTER_MENU_W, rows * PZ_FILTER_MENU_ROW_H + 2};
}

static int filter_choice_at(Ui* ui, int x, int y) {
    const Rect menu = filter_menu_rect(ui);
    if (!in_rect(x, y, menu.x, menu.y, menu.w, menu.h)) return -1;
    const int inside = y - menu.y - 1;
    if (inside < 0) return -1;
    const int row = inside / PZ_FILTER_MENU_ROW_H;
    int count = 1;
    if (ui->filter_updates > 0) ++count;
    if (ui->filter_locked > 0) ++count;
    if (ui->filter_nontracked > 0) ++count;
    return row >= 0 && row < count ? row : -1;
}

static FilterMode filter_option(Ui* ui, int index) {
    FilterMode modes[4];
    int count = 0;
    modes[count++] = FilterAll;
    if (ui->filter_updates > 0) modes[count++] = FilterUpdates;
    if (ui->filter_locked > 0) modes[count++] = FilterLocked;
    if (ui->filter_nontracked > 0) modes[count++] = FilterNontracked;
    return index >= 0 && index < count ? modes[index] : FilterAll;
}

static void draw_settings(Ui* ui) {
    fill(ui, 0, 0, PZ_WINDOW_WIDTH, PZ_WINDOW_HEIGHT, ui->pal.bg);
    draw_text(ui, ui->heading, PZ_MARGIN, 42, "Settings", ui->pal.text);
    draw_text(ui, ui->body, PZ_MARGIN, 62,
              "Choose the Zomboid folder and configure steamcmd.", ui->pal.muted);
    fill(ui, PZ_SETTINGS_X, PZ_SETTINGS_Y, PZ_SETTINGS_W, PZ_SETTINGS_H,
         ui->pal.surface);
    frame(ui, PZ_SETTINGS_X, PZ_SETTINGS_Y, PZ_SETTINGS_W, PZ_SETTINGS_H,
          ui->pal.border);

    const int x = PZ_SETTINGS_FIELD_X;
    draw_text(ui, ui->body, x, PZ_SETTINGS_Y + 92, "Zomboid folder", ui->pal.text);
    draw_text(ui, ui->body, x + 150, PZ_SETTINGS_Y + 92,
              "Absolute path containing mods/ and pzmodmgr/", ui->pal.muted);
    draw_input(ui, (Rect){x, PZ_SETTINGS_ZOMBOID_Y, PZ_SETTINGS_FIELD_W,
                          PZ_SETTINGS_FIELD_H},
               ui->settings_zomboid, "Default is ~/Zomboid",
               ui->settings_field == 0, 1, 0);

    draw_text(ui, ui->body, x, PZ_SETTINGS_Y + 166, "steamcmd executable", ui->pal.text);
    draw_text(ui, ui->body, x + 210, PZ_SETTINGS_Y + 166,
              "Blank uses the built-in copy", ui->pal.muted);
    draw_input(ui, (Rect){x, PZ_SETTINGS_STEAMCMD_Y, PZ_SETTINGS_FIELD_W,
                          PZ_SETTINGS_FIELD_H},
               ui->settings_path, "Built-in steamcmd", ui->settings_field == 1, 1, 0);

    draw_text(ui, ui->body, x, PZ_SETTINGS_Y + 240, "SteamCMD download root", ui->pal.text);
    draw_text(ui, ui->body, x + 220, PZ_SETTINGS_Y + 240,
              "Folder containing steamapps/workshop", ui->pal.muted);
    draw_input(ui, (Rect){x, PZ_SETTINGS_ROOT_Y, PZ_SETTINGS_FIELD_W,
                          PZ_SETTINGS_FIELD_H},
               ui->settings_root, "Leave blank unless importing existing downloads",
               ui->settings_field == 2, 1, 0);

    draw_text(ui, ui->body, x, PZ_SETTINGS_Y + 314, "Steam username", ui->pal.text);
    draw_input(ui, (Rect){x, PZ_SETTINGS_USER_Y, PZ_SETTINGS_FIELD_W,
                          PZ_SETTINGS_FIELD_H},
               ui->settings_user, "anonymous", ui->settings_field == 3, 1, 0);

    const int auto_hot = in_rect(ui->last_mouse_x, ui->last_mouse_y, x,
                                 PZ_SETTINGS_Y + 366, PZ_SETTINGS_FIELD_W, 22);
    fill(ui, x, PZ_SETTINGS_Y + 366, 18, 18,
         ui->settings_auto ? ui->pal.accent : ui->pal.field);
    frame(ui, x, PZ_SETTINGS_Y + 366, 18, 18,
          auto_hot ? ui->pal.accent : ui->pal.border_strong);
    if (ui->settings_auto) {
        XSetForeground(ui->display, ui->gc, ui->pal.on_accent);
        XSetLineAttributes(ui->display, ui->gc, 2, LineSolid, CapRound, JoinRound);
        XDrawLine(ui->display, ui->target, ui->gc, x + 4, PZ_SETTINGS_Y + 375,
                  x + 7, PZ_SETTINGS_Y + 378);
        XDrawLine(ui->display, ui->target, ui->gc, x + 7, PZ_SETTINGS_Y + 378,
                  x + 14, PZ_SETTINGS_Y + 370);
        XSetLineAttributes(ui->display, ui->gc, 1, LineSolid, CapButt, JoinMiter);
    }
    draw_text(ui, ui->body, x + 28, PZ_SETTINGS_Y + 380,
              "Enable newly installed mods in mods/default.txt", ui->pal.text);
    draw_text(ui, ui->body, x, PZ_SETTINGS_Y + 408,
              "No password is stored. Run steamcmd manually once so its login is cached.",
              ui->pal.muted);
    draw_text(ui, ui->body, x, PZ_SETTINGS_Y + 426,
              "Use anonymous for public Workshop items.", ui->pal.muted);
    draw_button(ui, BtnSave);
    draw_button(ui, BtnCancel);
    draw_button(ui, BtnTheme);
}

static void draw_targets(Ui* ui) {
    fill(ui, 0, 0, PZ_WINDOW_WIDTH, PZ_WINDOW_HEIGHT, ui->pal.bg);
    draw_text(ui, ui->heading, PZ_MARGIN, 42, "Enable mods", ui->pal.text);
    draw_text(ui, ui->body, PZ_MARGIN, 62,
              "Choose a Mods list, save, or server for the selected mods.",
              ui->pal.muted);
    fill(ui, PZ_SETTINGS_X, PZ_SETTINGS_Y, PZ_SETTINGS_W, PZ_SETTINGS_H,
         ui->pal.surface);
    frame(ui, PZ_SETTINGS_X, PZ_SETTINGS_Y, PZ_SETTINGS_W, PZ_SETTINGS_H,
          ui->pal.border);

    const int has_target = ui->target_focus >= 0 && ui->target_focus < ui->target_count;
    const int locked = has_target && ui->targets[ui->target_focus].lock_updates;
    fill(ui, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_Y + 68, 18, 18,
         locked ? ui->pal.accent : ui->pal.bg);
    frame(ui, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_Y + 68, 18, 18,
          has_target ? ui->pal.border_strong : ui->pal.border);
    if (locked) {
        XSetForeground(ui->display, ui->gc, ui->pal.on_accent);
        XSetLineAttributes(ui->display, ui->gc, 2, LineSolid, CapRound, JoinRound);
        XDrawLine(ui->display, ui->target, ui->gc, PZ_SETTINGS_FIELD_X + 4,
                  PZ_SETTINGS_Y + 77, PZ_SETTINGS_FIELD_X + 7,
                  PZ_SETTINGS_Y + 80);
        XDrawLine(ui->display, ui->target, ui->gc, PZ_SETTINGS_FIELD_X + 7,
                  PZ_SETTINGS_Y + 80, PZ_SETTINGS_FIELD_X + 14,
                  PZ_SETTINGS_Y + 72);
        XSetLineAttributes(ui->display, ui->gc, 1, LineSolid, CapButt, JoinMiter);
    }
    draw_text(ui, ui->body, PZ_SETTINGS_FIELD_X + 26, PZ_SETTINGS_Y + 82,
              "Lock updates for this target",
              has_target ? ui->pal.text : ui->pal.muted);

    if (ui->target_count == 0) {
        draw_text(ui, ui->body, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_Y + 120,
                  "No Mods list, save, or server was found in the Zomboid folder.",
                  ui->pal.muted);
    } else {
        const int total = ui->target_count;
        const int last = min_int(total, ui->target_offset + PZ_TARGETS_ROWS);
        char range[64];
        if (total <= PZ_TARGETS_ROWS) {
            text_copy(range, sizeof range, "");
            text_append_u64(range, sizeof range, (uint64_t)total);
            text_append(range, sizeof range, total == 1 ? " target" : " targets");
        } else {
            text_copy(range, sizeof range, "");
            text_append_u64(range, sizeof range, (uint64_t)(ui->target_offset + 1));
            text_append_char(range, sizeof range, '-');
            text_append_u64(range, sizeof range, (uint64_t)last);
            text_append(range, sizeof range, " of ");
            text_append_u64(range, sizeof range, (uint64_t)total);
        }
        draw_text(ui, ui->body,
                  PZ_SETTINGS_X + PZ_SETTINGS_W - 24 - text_width(ui->body, range),
                  PZ_SETTINGS_Y + 86, range, ui->pal.muted);

        for (int slot = 0; slot < PZ_TARGETS_ROWS; ++slot) {
            const int index = ui->target_offset + slot;
            if (index >= total) break;
            const pz_activation_target_data* target = &ui->targets[index];
            const Rect row = {PZ_SETTINGS_FIELD_X,
                              PZ_TARGETS_ROW_Y + slot * PZ_TARGETS_ROW_H,
                              PZ_SETTINGS_FIELD_W, PZ_TARGETS_ROW_H - 6};
            const int focused = index == ui->target_focus;
            fill(ui, row.x, row.y, row.w, row.h,
                 focused ? ui->pal.accent_soft : ui->pal.bg);
            frame(ui, row.x, row.y, row.w, row.h,
                  focused ? ui->pal.accent : ui->pal.border);
            int count = 0;
            for (int i = 0; i < ui->target_mod_id_count; ++i) {
                for (size_t m = 0; m < target->mod_ids.length; ++m) {
                    const pz_str* id = pz_vec_at_const(&target->mod_ids, m);
                    if (pz_str_equals(id, ui->target_mod_ids[i],
                                      strlen(ui->target_mod_ids[i]))) {
                        ++count;
                        break;
                    }
                }
            }
            char state[64];
            if (ui->target_mod_id_count == 0) {
                state[0] = '\0';
            } else if (count == 0) {
                text_copy(state, sizeof state, "Off");
            } else if (count == ui->target_mod_id_count) {
                text_copy(state, sizeof state, "On");
            } else {
                text_copy(state, sizeof state, "");
                text_append_u64(state, sizeof state, (uint64_t)count);
                text_append(state, sizeof state, " of ");
                text_append_u64(state, sizeof state, (uint64_t)ui->target_mod_id_count);
                text_append(state, sizeof state, " on");
            }
            const int state_width = text_width(ui->body, state);
            IconEntry* image = get_target_icon(ui, target);
            if (image && image->pixmap != None)
                XCopyArea(ui->display, image->pixmap, ui->target, ui->gc, 0, 0,
                          image->width, image->height, row.x + 8,
                          row.y + (row.h - 24) / 2);
            const int text_x = row.x + (image && image->pixmap != None ? 42 : 12);
            char label[256];
            elide(ui->body, target->label.data ? target->label.data : "",
                  row.w - state_width - 54, label, sizeof label);
            draw_text(ui, ui->body, text_x, row.y + row.h / 2 + 5, label,
                      ui->pal.text);
            draw_text(ui, ui->body, row.x + row.w - 12 - state_width,
                      row.y + row.h / 2 + 5, state,
                      count == 0 ? ui->pal.muted : ui->pal.success);
        }
    }

    draw_text(ui, ui->body, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_Y + 326,
              "Enable/disable writes only the selected mod IDs to this target.",
              ui->pal.muted);
    draw_button(ui, BtnEnableTarget);
    draw_button(ui, BtnDisableTarget);
    draw_button(ui, BtnShareTargets);
    draw_button(ui, BtnCopyNames);
    draw_button(ui, BtnCloseTargets);
}

static void redraw(Ui* ui) {
    ui->frame_pending = 0;
    fill(ui, 0, 0, PZ_WINDOW_WIDTH, PZ_WINDOW_HEIGHT, ui->pal.bg);
    if (ui->targets_open) {
        draw_targets(ui);
    } else if (ui->settings_open) {
        draw_settings(ui);
    } else {
        const Rect mark = {PZ_MARGIN, 20, 36, 36};
        fill(ui, mark.x, mark.y, mark.w, mark.h, ui->pal.brand);
        draw_text(ui, ui->heading,
                  mark.x + (mark.w - text_width(ui->heading, "PZ")) / 2,
                  baseline_for(ui->heading, &mark), "PZ", ui->pal.cream);
        fill(ui, PZ_MARGIN + 8, 49, 20, 3, ui->pal.accent);
        draw_text(ui, ui->heading, PZ_MARGIN + 48, 42, "PZ Mod Manager", ui->pal.text);
        draw_text(ui, ui->body, PZ_MARGIN + 48, 62,
                  "Project Zomboid Workshop installer", ui->pal.muted);
        draw_button(ui, BtnShareServer);
        if (ui->app_update_available) draw_button(ui, BtnSelfUpdate);
        draw_button(ui, BtnEnable);
        draw_button(ui, BtnSettings);

        draw_input(ui, (Rect){PZ_INPUT_X, PZ_INPUT_Y, PZ_INPUT_W, PZ_INPUT_H},
                   ui->input, "Paste Workshop URLs or IDs",
                   !ui->filter_focus && !ui->busy, !ui->busy,
                   ui->input[0] ? 26 : 0);
        if (ui->input[0]) {
            const Rect clear = {PZ_INPUT_X + PZ_INPUT_W - 24, PZ_INPUT_Y + 2, 20,
                                PZ_INPUT_H - 4};
            draw_text(ui, ui->body, clear.x + (clear.w - text_width(ui->body, "X")) / 2,
                      baseline_for(ui->body, &clear), "X", ui->pal.muted);
        }
        draw_button(ui, BtnImport);
        draw_button(ui, BtnInstall);
        draw_text(ui, ui->body, PZ_MARGIN, 142,
                  "Press Enter to install, or Import a URL/ID to track an existing local copy.",
                  ui->pal.muted);

        if (ui->list_dirty && !ui->busy) rebuild_visible(ui);

        draw_input(ui, (Rect){PZ_MARGIN, PZ_FILTER_Y, PZ_FILTER_W, PZ_FILTER_H},
                   ui->filter_text, "Filter name or folder",
                   ui->filter_focus && !ui->busy, !ui->busy,
                   ui->filter_text[0] ? 26 : 0);
        if (ui->filter_text[0]) {
            const Rect clear = {PZ_MARGIN + PZ_FILTER_W - 24, PZ_FILTER_Y + 2, 20,
                                PZ_FILTER_H - 4};
            draw_text(ui, ui->body, clear.x + (clear.w - text_width(ui->body, "X")) / 2,
                      baseline_for(ui->body, &clear), "X", ui->pal.muted);
        }
        draw_button(ui, BtnFilterMenu);
        draw_button(ui, BtnSort);
        draw_button(ui, BtnCheck);

        const int selected_count = ui->selected_count + ui->selected_folder_count;
        char selected[64];
        if (selected_count == 0) {
            text_copy(selected, sizeof selected, "No selection");
        } else {
            text_copy(selected, sizeof selected, "");
            text_append_u64(selected, sizeof selected, (uint64_t)selected_count);
            text_append(selected, sizeof selected, " selected");
        }
        const int selection_x = PZ_MARGIN + PZ_FILTER_W + 8;
        char shown[128];
        elide(ui->body, selected, 312 - 8 - selection_x, shown, sizeof shown);
        draw_text(ui, ui->body, selection_x, PZ_FILTER_Y + 17, shown, ui->pal.muted);

        char count[64];
        if (ui->busy) {
            text_copy(count, sizeof count, "Working...");
        } else if (ui->visible_count == 0) {
            text_copy(count, sizeof count, "0 mods");
        } else if (ui->visible_count <= PZ_ROWS) {
            text_copy(count, sizeof count, "");
            text_append_u64(count, sizeof count, (uint64_t)ui->visible_count);
            text_append(count, sizeof count, ui->visible_count == 1 ? " mod" : " mods");
        } else {
            const int last = min_int(ui->visible_count, ui->list_offset + PZ_ROWS);
            text_copy(count, sizeof count, "");
            text_append_u64(count, sizeof count, (uint64_t)(ui->list_offset + 1));
            text_append_char(count, sizeof count, '-');
            text_append_u64(count, sizeof count, (uint64_t)last);
            text_append(count, sizeof count, " of ");
            text_append_u64(count, sizeof count, (uint64_t)ui->visible_count);
        }
        draw_text(ui, ui->body, PZ_MARGIN + PZ_CARD_W - text_width(ui->body, count),
                  PZ_FILTER_Y + 17, count, ui->pal.muted);

        if (ui->busy) {
            fill(ui, PZ_MARGIN, PZ_LIST_TOP, PZ_CARD_W, 64, ui->pal.surface);
            frame(ui, PZ_MARGIN, PZ_LIST_TOP, PZ_CARD_W, 64, ui->pal.border);
            draw_text(ui, ui->body, PZ_MARGIN + 20, PZ_LIST_TOP + 38,
                      "Working - steamcmd may take a while...", ui->pal.muted);
        } else if (ui->visible_count == 0) {
            fill(ui, PZ_MARGIN, PZ_LIST_TOP, PZ_CARD_W, 64, ui->pal.surface);
            frame(ui, PZ_MARGIN, PZ_LIST_TOP, PZ_CARD_W, 64, ui->pal.border);
            const int filtered = ui->filter_text[0] || ui->filter_mode != FilterAll;
            draw_text(ui, ui->body, PZ_MARGIN + 20, PZ_LIST_TOP + 38,
                      filtered ? "No mods match the current filters."
                               : "No mods installed yet. Paste a Workshop link above to begin.",
                      ui->pal.muted);
        } else {
            for (int slot = 0; slot < PZ_ROWS; ++slot) {
                const int index = ui->list_offset + slot;
                if (index >= ui->visible_count) break;
                Row* row = &ui->rows[ui->visible[index]];
                const Rect card = {PZ_MARGIN,
                                   PZ_LIST_TOP + slot * (PZ_ROW_H + PZ_ROW_GAP),
                                   PZ_CARD_W, PZ_ROW_H};
                const int selected = row_selected(ui, row);
                const int hot = ui->hover_row == index;
                unsigned long card_fill = ui->pal.surface;
                unsigned long card_border = ui->pal.border;
                if (selected) {
                    card_fill = ui->pal.accent_soft;
                    card_border = ui->pal.accent;
                } else if (hot) {
                    card_fill = ui->pal.hover;
                    card_border = ui->pal.border_strong;
                }
                fill(ui, card.x, card.y, card.w, card.h, card_fill);
                frame(ui, card.x, card.y, card.w, card.h, card_border);

                IconEntry* image = get_icon(ui, row->folders[0]);
                if (image && image->pixmap != None) {
                    XCopyArea(ui->display, image->pixmap, ui->target, ui->gc, 0, 0,
                              image->width, image->height, card.x + 10, card.y + 8);
                } else {
                    fill(ui, card.x + 10, card.y + 8, 48, 48, ui->pal.accent_soft);
                    char initial[2] = {row->name[0] ? row->name[0] : '?', 0};
                    draw_text(ui, ui->heading,
                              card.x + 10 + (48 - text_width(ui->heading, initial)) / 2,
                              card.y + 8 + (48 + ui->heading->ascent - ui->heading->descent) / 2,
                              initial, ui->pal.accent);
                }

                const int show_update = row->update_available;
                const int show_lock = row->locked;
                const int switchable = ui->target_count != 0;
                const int is_enabled = ui->row_enabled[index];
                const int pill_w = 64;
                const int pill_gap = 6;
                const int pill_margin = 12;
                const int on_x = card.x + card.w - pill_margin - pill_w;
                const int update_x = switchable ? on_x - pill_gap - pill_w : on_x;
                const int text_x = card.x + 72;
                const int text_right =
                    (show_update || show_lock ? update_x
                     : switchable ? on_x : card.x + card.w - pill_margin) - 8;
                const int text_w = text_right - text_x;
                char title[256];
                elide(ui->heading, row->name[0] ? row->name : row->folders[0], text_w,
                      title, sizeof title);
                draw_text(ui, ui->heading, text_x, card.y + 26, title, ui->pal.text);
                char meta[1024];
                mod_meta(ui, row, meta, sizeof meta);
                char meta_shown[1024];
                elide(ui->body, meta, text_w, meta_shown, sizeof meta_shown);
                draw_text(ui, ui->body, text_x, card.y + 46, meta_shown, ui->pal.muted);
                if (switchable) {
                    const Rect pill = {on_x, card.y + 22, pill_w, 20};
                    fill(ui, pill.x, pill.y, pill.w, pill.h,
                         is_enabled ? ui->pal.success : ui->pal.bg);
                    frame(ui, pill.x, pill.y, pill.w, pill.h,
                          is_enabled ? ui->pal.success : ui->pal.border_strong);
                    const char* label = is_enabled ? "ON" : "OFF";
                    draw_text(ui, ui->body,
                              pill.x + (pill.w - text_width(ui->body, label)) / 2,
                              pill.y + 14, label,
                              is_enabled ? ui->pal.on_accent : ui->pal.muted);
                }
                if (show_update || show_lock) {
                    const Rect pill = {update_x, card.y + 22, pill_w, 20};
                    const char* label = show_lock ? "LOCKED" : "UPDATE";
                    if (show_lock) {
                        fill(ui, pill.x, pill.y, pill.w, pill.h, ui->pal.bg);
                        frame(ui, pill.x, pill.y, pill.w, pill.h, ui->pal.border_strong);
                    } else {
                        fill(ui, pill.x, pill.y, pill.w, pill.h, ui->pal.accent);
                    }
                    draw_text(ui, ui->body,
                              pill.x + (pill.w - text_width(ui->body, label)) / 2,
                              pill.y + 14, label,
                              show_lock ? ui->pal.muted : ui->pal.on_accent);
                }
            }

            int thumb_y = 0, thumb_h = 0;
            if (scrollbar_metrics(ui, &thumb_y, &thumb_h)) {
                fill(ui, PZ_SCROLL_X, PZ_LIST_TOP, PZ_SCROLL_W,
                     PZ_LIST_BOTTOM - PZ_LIST_TOP, ui->pal.border);
                fill(ui, PZ_SCROLL_X, thumb_y, PZ_SCROLL_W, thumb_h,
                     ui->scroll_drag ? ui->pal.accent : ui->pal.border_strong);
            }
        }

        draw_button(ui, BtnSelectAll);
        draw_button(ui, BtnClear);
        draw_button(ui, BtnLock);
        draw_button(ui, BtnCopy);
        draw_button(ui, BtnUpdate);
        draw_button(ui, BtnUpdateAll);
        draw_button(ui, BtnRemove);

        fill(ui, PZ_MARGIN, PZ_ACTIVITY_Y, PZ_CARD_W, PZ_ACTIVITY_H,
             ui->pal.surface);
        frame(ui, PZ_MARGIN, PZ_ACTIVITY_Y, PZ_CARD_W, PZ_ACTIVITY_H,
              ui->pal.border);
        draw_text(ui, ui->body, PZ_MARGIN + 12, PZ_ACTIVITY_Y + 20, "ACTIVITY",
                  ui->pal.brand);
        const int max_offset = max_int(0, ui->log_count - PZ_ACTIVITY_LINES);
        if (ui->activity_follow) ui->activity_offset = max_offset;
        ui->activity_offset = min_int(max_int(0, ui->activity_offset), max_offset);
        int ly = PZ_ACTIVITY_Y + 40;
        for (int i = max_offset; i < ui->log_count && i < max_offset + PZ_ACTIVITY_LINES;
             ++i, ly += 16) {
            char line[PZ_MAX_LINE];
            elide(ui->body, log_at(ui, i), PZ_CARD_W - 48, line, sizeof line);
            draw_text(ui, ui->body, PZ_MARGIN + 12, ly, line, ui->pal.muted);
        }
        if (ui->log_count == 0)
            draw_text(ui, ui->body, PZ_MARGIN + 12, PZ_ACTIVITY_Y + 40,
                      "Ready for a Workshop link.", ui->pal.muted);

        int activity_thumb_y = 0, activity_thumb_h = 0;
        if (activity_metrics(ui, &activity_thumb_y, &activity_thumb_h)) {
            fill(ui, PZ_ACTIVITY_SCROLL_X, PZ_ACTIVITY_TRACK_TOP, PZ_SCROLL_W,
                 PZ_ACTIVITY_TRACK_BOTTOM - PZ_ACTIVITY_TRACK_TOP, ui->pal.border);
            fill(ui, PZ_ACTIVITY_SCROLL_X, activity_thumb_y, PZ_SCROLL_W,
                 activity_thumb_h, ui->pal.border_strong);
        }

        const char* status = ui->busy ? "Working - steamcmd may take a while..."
            : (ui->dependency_warning[0] ? ui->dependency_warning : "Ready");
        char status_shown[512];
        elide(ui->body, status, PZ_CARD_W - 110, status_shown, sizeof status_shown);
        draw_text(ui, ui->body, PZ_MARGIN, PZ_STATUS_Y, status_shown,
                  ui->busy ? ui->pal.accent
                           : (ui->dependency_warning[0] ? ui->pal.danger : ui->pal.brand));

        if (ui->filter_menu_open) {
            const Rect menu = filter_menu_rect(ui);
            fill(ui, menu.x, menu.y, menu.w, menu.h, ui->pal.surface);
            frame(ui, menu.x, menu.y, menu.w, menu.h, ui->pal.border_strong);
            int count = 1;
            if (ui->filter_updates > 0) ++count;
            if (ui->filter_locked > 0) ++count;
            if (ui->filter_nontracked > 0) ++count;
            int row_y = menu.y + 1;
            for (int i = 0; i < count; ++i) {
                const FilterMode mode = filter_option(ui, i);
                const int current = mode == ui->filter_mode;
                const int hot = ui->filter_menu_hover == i;
                if (current || hot)
                    fill(ui, menu.x + 1, row_y, menu.w - 2, PZ_FILTER_MENU_ROW_H,
                         current ? ui->pal.brand_soft : ui->pal.hover);
                static const char* names[] = {"All", "Updates", "Locked", "Nontracked"};
                const int value = mode == FilterAll ? ui->filter_total
                    : mode == FilterUpdates ? ui->filter_updates
                    : mode == FilterLocked ? ui->filter_locked
                                           : ui->filter_nontracked;
                char label[64];
                text_copy(label, sizeof label, names[mode]);
                text_append(label, sizeof label, " (");
                text_append_u64(label, sizeof label, (uint64_t)value);
                text_append_char(label, sizeof label, ')');
                draw_text(ui, ui->body, menu.x + 11, row_y + 17, label,
                          current ? ui->pal.brand : ui->pal.text);
                row_y += PZ_FILTER_MENU_ROW_H;
            }
        }
    }

    XCopyArea(ui->display, ui->target, ui->window, ui->gc, 0, 0,
              PZ_WINDOW_WIDTH, PZ_WINDOW_HEIGHT, 0, 0);
    XFlush(ui->display);
}

/* ------------------------------------------------------------- commands */

static void sync_config(Ui* ui) {
    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(ui->manager, &config);
    copy_pz(ui->mods_dir, sizeof ui->mods_dir, &config.user_mods_dir);
    copy_pz(ui->data_dir, sizeof ui->data_dir, &config.data_dir);
    ui->settings_dark = config.dark_mode != 0;
    ui->settings_auto = config.auto_enable != 0;
    pz_paths_config_free(&config);

    const pz_str* version = pz_mod_manager_game_version(ui->manager);
    copy_pz(ui->game_version, sizeof ui->game_version, version);
}

static void refresh_dependency(Ui* ui) {
    pz_vec missing;
    pz_vec_init(&missing, sizeof(pz_missing_requirement));
    pz_mod_manager_missing_requirements(ui->manager, &missing);

    char signature[1024];
    text_copy(signature, sizeof signature, "");
    for (size_t i = 0; i < missing.length; ++i) {
        const pz_missing_requirement* value = pz_vec_at_const(&missing, i);
        text_append_n(signature, sizeof signature, value->folder.data,
                      value->folder.length);
        text_append_char(signature, sizeof signature, '\t');
        text_append_n(signature, sizeof signature, value->required_id.data,
                      value->required_id.length);
        text_append_char(signature, sizeof signature, '\n');
    }

    if (strcmp(signature, ui->dependency_signature) != 0) {
        if (missing.length == 0) {
            if (ui->dependency_signature[0]) push_log(ui, "required mods satisfied");
        } else {
            for (size_t i = 0; i < missing.length; ++i) {
                const pz_missing_requirement* value = pz_vec_at_const(&missing, i);
                char line[512];
                text_copy(line, sizeof line, "warning: ");
                text_append_n(line, sizeof line, value->mod_name.data,
                              value->mod_name.length);
                text_append(line, sizeof line, " requires ");
                text_append_n(line, sizeof line, value->required_id.data,
                              value->required_id.length);
                text_append(line, sizeof line, " (not installed)");
                push_log(ui, line);
            }
        }
        text_copy(ui->dependency_signature, sizeof ui->dependency_signature,
                  signature);
    }

    if (missing.length == 0) {
        ui->dependency_warning[0] = '\0';
    } else if (missing.length == 1) {
        const pz_missing_requirement* value = pz_vec_at_const(&missing, 0);
        text_copy(ui->dependency_warning, sizeof ui->dependency_warning,
                  "Missing required mod: ");
        text_append_n(ui->dependency_warning, sizeof ui->dependency_warning,
                      value->required_id.data, value->required_id.length);
        text_append(ui->dependency_warning, sizeof ui->dependency_warning,
                    " (needed by ");
        text_append_n(ui->dependency_warning, sizeof ui->dependency_warning,
                      value->mod_name.data, value->mod_name.length);
        text_append_char(ui->dependency_warning, sizeof ui->dependency_warning, ')');
    } else {
        text_copy(ui->dependency_warning, sizeof ui->dependency_warning,
                  "Missing ");
        text_append_u64(ui->dependency_warning, sizeof ui->dependency_warning,
                        (uint64_t)missing.length);
        text_append(ui->dependency_warning, sizeof ui->dependency_warning,
                    " required mods - see Activity");
    }
    pz_mod_manager_free_requirements(&missing);
}

static void sync_log(void* context, const char* line) {
    push_log((Ui*)context, line);
}

static int run_sync_command(Ui* ui, const char* line) {
    pz_log log;
    pz_log_init(&log);
    pz_log_set(&log, sync_log, ui);
    pz_mm_result result = pz_command_run(ui->manager, line, strlen(line), &log);
    if (result.message.data)
        push_log(ui, result.message.data);
    const int ok = result.ok;
    pz_mm_result_free(&result);
    return ok;
}

static void join_worker(Ui* ui) {
    if (ui->worker_joinable) {
        pthread_join(ui->worker, NULL);
        ui->worker_joinable = 0;
    }
}

typedef struct WorkerArgs {
    Ui* ui;
    char line[PZ_MAX_LINE];
} WorkerArgs;

static void* worker_main(void* context) {
    WorkerArgs* args = (WorkerArgs*)context;
    Ui* ui = args->ui;
    pz_log log;
    pz_log_init(&log);
    pz_log_set(&log, worker_log, ui);
    pz_mm_result result =
        pz_command_run(ui->manager, args->line, strlen(args->line), &log);

    pthread_mutex_lock(&ui->pending_mu);
    if (ui->pending_count < PZ_MAX_PENDING) {
        char* line = ui->pending[ui->pending_count++];
        text_copy(line, PZ_MAX_LINE, result.ok ? "OK: " : "ERROR: ");
        text_append_n(line, PZ_MAX_LINE, result.message.data,
                      result.message.length);
    }
    ui->worker_done = 1;
    pthread_mutex_unlock(&ui->pending_mu);
    wake_ui(ui);
    pz_mm_result_free(&result);
    free(args);
    return NULL;
}

static void start_command(Ui* ui, const char* line) {
    if (ui->busy || !line[0]) return;
    ui->confirm_remove = 0;
    ui->filter_menu_open = 0;
    char entry[PZ_MAX_LINE];
    text_copy(entry, sizeof entry, "> ");
    text_append(entry, sizeof entry, line);
    push_log(ui, entry);
    ui->busy = 1;
    join_worker(ui);

    WorkerArgs* args = (WorkerArgs*)calloc(1, sizeof *args);
    if (!args) {
        ui->busy = 0;
        push_log(ui, "ERROR: cannot start worker");
        redraw(ui);
        return;
    }
    args->ui = ui;
    text_copy(args->line, sizeof args->line, line);
    if (pthread_create(&ui->worker, NULL, worker_main, args) != 0) {
        free(args);
        ui->busy = 0;
        push_log(ui, "ERROR: cannot start worker");
    } else {
        ui->worker_joinable = 1;
    }
    redraw(ui);
}

static int drain_pending(Ui* ui) {
    pthread_mutex_lock(&ui->pending_mu);
    for (int i = 0; i < ui->pending_count; ++i) push_log(ui, ui->pending[i]);
    ui->pending_count = 0;
    const int done = ui->worker_done;
    ui->worker_done = 0;
    pthread_mutex_unlock(&ui->pending_mu);
    return done;
}

static void build_ids_command(Ui* ui, const char* verb, char* out,
                              size_t capacity) {
    text_copy(out, capacity, verb);
    for (int i = 0; i < ui->selected_count; ++i) {
        text_append_char(out, capacity, ' ');
        text_append_u64(out, capacity, ui->selected_ids[i]);
    }
}

static void submit(Ui* ui) {
    if (ui->busy) return;
    if (!ui->input[0]) {
        push_log(ui, "Enter one or more Workshop URLs or IDs first.");
        redraw(ui);
        return;
    }
    char line[PZ_MAX_LINE];
    if (strncmp(ui->input, "lock ", 5) == 0 ||
        strncmp(ui->input, "unlock ", 7) == 0) {
        text_copy(line, sizeof line, ui->input);
    } else {
        text_copy(line, sizeof line, "add ");
        text_append(line, sizeof line, ui->input);
    }
    ui->input[0] = '\0';
    start_command(ui, line);
}

static void remove_selected(Ui* ui) {
    if (ui->busy) return;
    if (ui->selected_count == 0) {
        push_log(ui, "Select a tracked mod first; manual folders are not managed by pzmodmgr.");
        redraw(ui);
        return;
    }
    if (!ui->confirm_remove) {
        ui->confirm_remove = 1;
        char line[128];
        text_copy(line, sizeof line, "Click Confirm remove again to delete ");
        text_append_u64(line, sizeof line, (uint64_t)ui->selected_count);
        text_append(line, sizeof line, " selected mod(s).");
        push_log(ui, line);
        redraw(ui);
        return;
    }
    char line[PZ_MAX_LINE];
    build_ids_command(ui, "remove", line, sizeof line);
    start_command(ui, line);
}

static void copy_ids(Ui* ui) {
    if (ui->selected_count == 0) {
        push_log(ui, ui->selected_folder_count
                         ? "Manual mods have no Workshop ID to copy."
                         : "Select one or more tracked mods first.");
        redraw(ui);
        return;
    }
    text_copy(ui->clipboard_text, sizeof ui->clipboard_text, "");
    for (int i = 0; i < ui->selected_count; ++i) {
        if (i) text_append_char(ui->clipboard_text, sizeof ui->clipboard_text, ' ');
        text_append_u64(ui->clipboard_text, sizeof ui->clipboard_text,
                        ui->selected_ids[i]);
    }
    XSetSelectionOwner(ui->display, ui->clipboard, ui->window, CurrentTime);
    if (XGetSelectionOwner(ui->display, ui->clipboard) != ui->window) {
        ui->clipboard_text[0] = '\0';
        push_log(ui, "Could not take clipboard ownership.");
    } else {
        char line[128];
        text_copy(line, sizeof line, "Copied ");
        text_append_u64(line, sizeof line, (uint64_t)ui->selected_count);
        text_append(line, sizeof line,
                    " Workshop ID(s). Paste them into Add, or type lock first.");
        push_log(ui, line);
    }
    redraw(ui);
}

static void folder_token(const char* folder, char* out, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    text_copy(out, capacity, "f:");
    for (const unsigned char* p = (const unsigned char*)folder; *p; ++p) {
        text_append_char(out, capacity, digits[*p >> 4]);
        text_append_char(out, capacity, digits[*p & 15]);
    }
}

static void selection_command(Ui* ui, int index, int enabled, char* out,
                              size_t capacity) {
    text_copy(out, capacity, "set-enabled ");
    text_append_u64(out, capacity, (uint64_t)index);
    text_append(out, capacity, enabled ? " on" : " off");
    for (int i = 0; i < ui->selected_count; ++i) {
        text_append(out, capacity, " w:");
        text_append_u64(out, capacity, ui->selected_ids[i]);
    }
    for (int i = 0; i < ui->selected_folder_count; ++i) {
        char token[1024];
        folder_token(ui->selected_folders[i], token, sizeof token);
        text_append_char(out, capacity, ' ');
        text_append(out, capacity, token);
    }
}

static void copy_target_ids(Ui* ui) {
    if (ui->target_focus < 0 || ui->target_focus >= ui->target_count) return;
    pz_activation_target_data* target = &ui->targets[ui->target_focus];
    pz_vec ids;
    pz_vec_init(&ids, sizeof(uint64_t));
    pz_mod_manager_workshop_ids_for(ui->manager, target, &ids);
    if (ids.length == 0) {
        char name[256];
        activation_name(target, name, sizeof name);
        char line[512];
        text_copy(line, sizeof line, "No Workshop IDs found for ");
        text_append(line, sizeof line, name);
        text_append(line, sizeof line, ". Use Copy names so the list can still be shared.");
        push_log(ui, line);
        pz_vec_free(&ids);
        redraw(ui);
        return;
    }
    text_copy(ui->clipboard_text, sizeof ui->clipboard_text, "");
    for (size_t i = 0; i < ids.length; ++i) {
        if (i) text_append_char(ui->clipboard_text, sizeof ui->clipboard_text, ' ');
        text_append_u64(ui->clipboard_text, sizeof ui->clipboard_text,
                        *(const uint64_t*)pz_vec_at_const(&ids, i));
    }
    XSetSelectionOwner(ui->display, ui->clipboard, ui->window, CurrentTime);
    if (XGetSelectionOwner(ui->display, ui->clipboard) != ui->window) {
        ui->clipboard_text[0] = '\0';
        push_log(ui, "Could not take clipboard ownership.");
    } else {
        char line[512];
        text_copy(line, sizeof line, "Copied ");
        text_append_u64(line, sizeof line, (uint64_t)ids.length);
        text_append(line, sizeof line, " Workshop ID(s) from ");
        char name[256];
        activation_name(target, name, sizeof name);
        text_append(line, sizeof line, name);
        text_append(line, sizeof line, ". Paste into Add, or type lock first to pin them.");
        pz_vec missing;
        pz_vec_init(&missing, sizeof(pz_str));
        pz_mod_manager_unmapped_mod_ids(ui->manager, target, &missing);
        if (missing.length != 0) {
            text_append_char(line, sizeof line, ' ');
            text_append_u64(line, sizeof line, (uint64_t)missing.length);
            text_append(line, sizeof line,
                        " enabled Mod ID(s) have no installed Workshop ID; use Copy names.");
        }
        pz_activation_string_list_free(&missing);
        push_log(ui, line);
    }
    pz_vec_free(&ids);
    redraw(ui);
}

static void copy_target_names(Ui* ui) {
    if (ui->target_focus < 0 || ui->target_focus >= ui->target_count) return;
    pz_activation_target_data* target = &ui->targets[ui->target_focus];
    pz_vec names;
    pz_vec_init(&names, sizeof(pz_str));
    pz_mod_manager_enabled_mod_names(ui->manager, target, &names);
    if (names.length == 0) {
        char name[256];
        activation_name(target, name, sizeof name);
        char line[512];
        text_copy(line, sizeof line, "No enabled Mod names found for ");
        text_append(line, sizeof line, name);
        text_append_char(line, sizeof line, '.');
        push_log(ui, line);
        pz_activation_string_list_free(&names);
        redraw(ui);
        return;
    }
    text_copy(ui->clipboard_text, sizeof ui->clipboard_text, "");
    for (size_t i = 0; i < names.length; ++i) {
        const pz_str* value = pz_vec_at_const(&names, i);
        if (i) text_append_char(ui->clipboard_text, sizeof ui->clipboard_text, '\n');
        text_append_n(ui->clipboard_text, sizeof ui->clipboard_text,
                      value->data, value->length);
    }
    XSetSelectionOwner(ui->display, ui->clipboard, ui->window, CurrentTime);
    if (XGetSelectionOwner(ui->display, ui->clipboard) != ui->window) {
        ui->clipboard_text[0] = '\0';
        push_log(ui, "Could not take clipboard ownership.");
    } else {
        char line[512];
        text_copy(line, sizeof line, "Copied ");
        text_append_u64(line, sizeof line, (uint64_t)names.length);
        text_append(line, sizeof line, " enabled Mod name(s) from ");
        char name[256];
        activation_name(target, name, sizeof name);
        text_append(line, sizeof line, name);
        text_append_char(line, sizeof line, '.');
        push_log(ui, line);
    }
    pz_activation_string_list_free(&names);
    redraw(ui);
}

static void clamp_target_offset(Ui* ui) {
    ui->target_offset = min_int(max_int(0, ui->target_offset),
                                max_int(0, ui->target_count - PZ_TARGETS_ROWS));
}

static int target_row_at(Ui* ui, int x, int y) {
    if (x < PZ_SETTINGS_X || x > PZ_SETTINGS_X + PZ_SETTINGS_W) return -1;
    if (y < PZ_TARGETS_ROW_Y) return -1;
    const int slot = (y - PZ_TARGETS_ROW_Y) / PZ_TARGETS_ROW_H;
    if (slot < 0 || slot >= PZ_TARGETS_ROWS) return -1;
    const int index = ui->target_offset + slot;
    return index < ui->target_count ? index : -1;
}

static void add_target_mod_id(Ui* ui, const pz_str* id) {
    if (id->length == 0) return;
    for (int i = 0; i < ui->target_mod_id_count; ++i) {
        if (strlen(ui->target_mod_ids[i]) == id->length &&
            memcmp(ui->target_mod_ids[i], id->data, id->length) == 0)
            return;
    }
    if (ui->target_mod_id_count < PZ_MAX_TARGET_IDS)
        text_copy_n(ui->target_mod_ids[ui->target_mod_id_count++],
                    sizeof ui->target_mod_ids[0], id->data, id->length);
}

static void open_targets(Ui* ui, int prefer_server, int allow_empty) {
    if (!allow_empty && !selected_any(ui)) {
        push_log(ui, "Select one or more mods first.");
        redraw(ui);
        return;
    }
    if (ui->list_dirty) rebuild_visible(ui);
    ui->target_mod_id_count = 0;
    for (int i = 0; i < ui->visible_count; ++i) {
        Row* row = &ui->rows[ui->visible[i]];
        if (!row_selected(ui, row)) continue;
        for (int f = 0; f < row->folder_count; ++f) {
            pz_mod_info* info = get_info(ui, row->folders[f]);
            if (info) add_target_mod_id(ui, &info->id);
        }
    }
    for (int i = 0; i < ui->selected_folder_count; ++i) {
        pz_mod_info* info = get_info(ui, ui->selected_folders[i]);
        if (info) add_target_mod_id(ui, &info->id);
    }
    copy_target_strings(ui);
    clear_target_icons(ui);
    ui->target_offset = 0;
    ui->target_focus = ui->target_count ? 0 : -1;
    if (prefer_server) {
        for (int i = 0; i < ui->target_count; ++i) {
            if (ui->targets[i].kind == 2) {
                ui->target_focus = i;
                break;
            }
        }
        ui->target_offset = max_int(0, ui->target_focus - PZ_TARGETS_ROWS + 1);
        clamp_target_offset(ui);
    }
    ui->targets_open = 1;
    ui->filter_menu_open = 0;
    redraw(ui);
}

static void open_settings(Ui* ui) {
    if (ui->busy) return;
    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(ui->manager, &config);
    copy_pz(ui->settings_zomboid, sizeof ui->settings_zomboid, &config.zomboid_dir);
    copy_pz(ui->settings_path, sizeof ui->settings_path, &config.steamcmd_exe);
    copy_pz(ui->settings_root, sizeof ui->settings_root, &config.steamcmd_root);
    copy_pz(ui->settings_user, sizeof ui->settings_user, &config.steam_user);
    ui->settings_dark = config.dark_mode != 0;
    ui->settings_auto = config.auto_enable != 0;
    pz_paths_config_free(&config);
    ui->settings_field = 0;
    ui->settings_open = 1;
    ui->filter_menu_open = 0;
    ui->filter_focus = 0;
    ui->hover_button = -1;
    redraw(ui);
}

static void save_settings(Ui* ui) {
    if (!ui->settings_user[0]) {
        push_log(ui, "Steam username is required.");
        redraw(ui);
        return;
    }
    char line[PZ_MAX_LINE];
    int ok = 1;
    text_copy(line, sizeof line, "config steamcmd_exe ");
    text_append(line, sizeof line, ui->settings_path);
    ok &= run_sync_command(ui, line);
    text_copy(line, sizeof line, "config steamcmd_root ");
    text_append(line, sizeof line, ui->settings_root);
    ok &= run_sync_command(ui, line);
    text_copy(line, sizeof line, "config steam_user ");
    text_append(line, sizeof line, ui->settings_user);
    ok &= run_sync_command(ui, line);
    text_copy(line, sizeof line, "config zomboid_dir ");
    text_append(line, sizeof line, ui->settings_zomboid);
    ok &= run_sync_command(ui, line);
    text_copy(line, sizeof line, "config dark_mode ");
    text_append(line, sizeof line, ui->settings_dark ? "on" : "off");
    ok &= run_sync_command(ui, line);
    text_copy(line, sizeof line, "config auto_enable ");
    text_append(line, sizeof line, ui->settings_auto ? "on" : "off");
    ok &= run_sync_command(ui, line);

    if (ok) {
        apply_palette(ui);
        XSetWindowBackground(ui->display, ui->window, ui->pal.bg);
        XSetWindowBorder(ui->display, ui->window, ui->pal.border);
        sync_config(ui);
        pz_mod_manager_reload_installed(ui->manager);
        clear_icons(ui);
        rebuild_rows(ui);
        refresh_dependency(ui);
        ui->settings_open = 0;
        push_log(ui, "Settings saved");
    } else {
        push_log(ui, "ERROR: settings were not saved");
    }
    redraw(ui);
}

/* ------------------------------------------------------------- clipboard */

static uint64_t monotonic_millis(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void request_paste(Ui* ui, Atom selection, int fallback, Time time) {
    if (ui->busy) return;
    if (ui->settings_open) {
        ui->paste_field = ui->settings_field == 0 ? PasteSettingsZomboid
            : ui->settings_field == 1 ? PasteSettingsPath
            : ui->settings_field == 2 ? PasteSettingsRoot : PasteSettingsUser;
    } else {
        ui->paste_field = ui->filter_focus ? PasteFilter : PasteInput;
    }
    ui->paste_selection = selection;
    ui->paste_time = time;
    ui->paste_fallback = fallback;
    ui->paste_pending = 1;
    XConvertSelection(ui->display, selection, fallback ? XA_STRING : ui->utf8,
                      ui->paste_prop, ui->window, time);
    XFlush(ui->display);
}

static void paste_into_field(Ui* ui, const char* raw) {
    if (!ui->settings_open && ui->paste_field != PasteFilter) {
        char ids[PZ_MAX_LINE];
        clean_workshop_paste(raw, ids, sizeof ids);
        if (!ids[0]) {
            push_log(ui, "Clipboard is empty.");
        } else {
            const size_t used = strlen(ui->input);
            if (used != 0 && ui->input[used - 1] != ' ')
                text_append_char(ui->input, sizeof ui->input, ' ');
            text_append(ui->input, sizeof ui->input, ids);
        }
        redraw(ui);
        return;
    }

    char value[PZ_MAX_LINE];
    clean_pasted_text(raw, value, sizeof value);
    if (!value[0]) {
        push_log(ui, "Clipboard is empty.");
        redraw(ui);
        return;
    }
    if (ui->settings_open) {
        char* field = ui->settings_field == 0 ? ui->settings_zomboid
            : ui->settings_field == 1 ? ui->settings_path
            : ui->settings_field == 2 ? ui->settings_root : ui->settings_user;
        const size_t capacity = ui->settings_field == 3
            ? sizeof ui->settings_user : sizeof ui->settings_zomboid;
        text_append(field, capacity, value);
    } else {
        text_append(ui->filter_text, sizeof ui->filter_text, value);
        ui->list_offset = 0;
        ui->list_dirty = 1;
    }
    redraw(ui);
}

/* ---------------------------------------------------------------- watch */

static void close_watch(Ui* ui) {
    if (ui->watch_fd >= 0) {
        close(ui->watch_fd);
        ui->watch_fd = -1;
    }
}

static void refresh_watch(Ui* ui) {
    close_watch(ui);
#if defined(__linux__)
    ui->watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ui->watch_fd < 0) return;
    if (inotify_add_watch(ui->watch_fd, ui->data_dir,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
                              IN_DELETE) < 0)
        close_watch(ui);
#endif
}

static void handle_watch(Ui* ui) {
    int changed = 0;
#if defined(__linux__)
    if (ui->watch_fd < 0) return;
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        const ssize_t count = read(ui->watch_fd, buffer, sizeof buffer);
        if (count <= 0) break;
        for (const char* p = buffer; p < buffer + count;) {
            const struct inotify_event* event = (const struct inotify_event*)p;
            if ((event->mask & IN_Q_OVERFLOW) != 0) changed = 1;
            if (event->len != 0 && strcmp(event->name, "installed.tsv") == 0)
                changed = 1;
            p += sizeof(struct inotify_event) + event->len;
        }
    }
#endif
    if (!changed) return;
    if (ui->busy) {
        ui->reload_pending = 1;
        return;
    }
    if (pz_mod_manager_reload_installed(ui->manager)) {
        clear_icons(ui);
        rebuild_rows(ui);
        refresh_dependency(ui);
        redraw(ui);
    }
}

/* ---------------------------------------------------------------- events */

static char* settings_field_text(Ui* ui) {
    if (ui->settings_field == 0) return ui->settings_zomboid;
    if (ui->settings_field == 1) return ui->settings_path;
    if (ui->settings_field == 2) return ui->settings_root;
    return ui->settings_user;
}

static size_t settings_field_capacity(Ui* ui) {
    return ui->settings_field == 3 ? sizeof ui->settings_user
                                   : sizeof ui->settings_zomboid;
}

static void clamp_activity_offset(Ui* ui) {
    const int max_offset = max_int(0, ui->log_count - PZ_ACTIVITY_LINES);
    ui->activity_offset = min_int(max_int(0, ui->activity_offset), max_offset);
    ui->activity_follow = ui->activity_offset >= max_offset;
}

static void handle_button_press(Ui* ui, XButtonEvent* event) {
    if (ui->busy) return;
    const int x = event->x;
    const int y = event->y;

    if (ui->filter_menu_open) {
        if (event->button == Button1) {
            if (in_rect(x, y, PZ_FILTER_MENU_X, PZ_FILTER_Y, PZ_FILTER_MENU_W,
                        PZ_FILTER_H)) {
                ui->filter_menu_open = 0;
                ui->filter_menu_hover = -1;
                redraw(ui);
                return;
            }
            const int choice = filter_choice_at(ui, x, y);
            if (choice >= 0) {
                ui->filter_mode = filter_option(ui, choice);
                ui->filter_menu_open = 0;
                ui->filter_menu_hover = -1;
                ui->filter_focus = 0;
                ui->list_offset = 0;
                ui->list_dirty = 1;
                redraw(ui);
                return;
            }
        }
        ui->filter_menu_open = 0;
        ui->filter_menu_hover = -1;
        redraw(ui);
        return;
    }

    if (ui->targets_open) {
        if (event->button == Button4 || event->button == Button5) {
            ui->target_offset += event->button == Button4 ? -1 : 1;
            clamp_target_offset(ui);
            redraw(ui);
            return;
        }
        if (event->button != Button1) return;
        if (in_rect(x, y, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_Y + 64, 280, 26) &&
            ui->target_focus >= 0 && ui->target_focus < ui->target_count) {
            pz_activation_target_data* target = &ui->targets[ui->target_focus];
            char line[128];
            text_copy(line, sizeof line, "target-lock ");
            text_append_u64(line, sizeof line, (uint64_t)ui->target_focus);
            text_append(line, sizeof line, target->lock_updates ? " off" : " on");
            const int keep = ui->target_focus;
            run_sync_command(ui, line);
            copy_target_strings(ui);
            ui->target_focus = ui->target_count ? min_int(keep, ui->target_count - 1) : -1;
            ui->list_dirty = 1;
            redraw(ui);
            return;
        }
        const Btn button = button_at(ui, x, y);
        if (button == BtnCloseTargets) {
            ui->targets_open = 0;
            redraw(ui);
            return;
        }
        if (button == BtnShareTargets) { copy_target_ids(ui); return; }
        if (button == BtnCopyNames) { copy_target_names(ui); return; }
        if (button == BtnEnableTarget || button == BtnDisableTarget) {
            if (ui->target_focus < 0 || ui->target_mod_id_count == 0) return;
            char line[PZ_MAX_LINE];
            selection_command(ui, ui->target_focus, button == BtnEnableTarget,
                              line, sizeof line);
            ui->targets_open = 0;
            start_command(ui, line);
            return;
        }
        const int row = target_row_at(ui, x, y);
        if (row >= 0) {
            ui->target_focus = row;
            redraw(ui);
        }
        return;
    }

    if (ui->settings_open) {
        const Btn button = button_at(ui, x, y);
        if (button == BtnSave) { save_settings(ui); return; }
        if (button == BtnCancel) { ui->settings_open = 0; redraw(ui); return; }
        if (button == BtnTheme) {
            ui->settings_dark = !ui->settings_dark;
            redraw(ui);
            return;
        }
        if (in_rect(x, y, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_Y + 366,
                    PZ_SETTINGS_FIELD_W, 22)) {
            ui->settings_auto = !ui->settings_auto;
            redraw(ui);
            return;
        }
        int field = -1;
        if (in_rect(x, y, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_ZOMBOID_Y,
                    PZ_SETTINGS_FIELD_W, PZ_SETTINGS_FIELD_H)) field = 0;
        else if (in_rect(x, y, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_STEAMCMD_Y,
                         PZ_SETTINGS_FIELD_W, PZ_SETTINGS_FIELD_H)) field = 1;
        else if (in_rect(x, y, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_ROOT_Y,
                         PZ_SETTINGS_FIELD_W, PZ_SETTINGS_FIELD_H)) field = 2;
        else if (in_rect(x, y, PZ_SETTINGS_FIELD_X, PZ_SETTINGS_USER_Y,
                         PZ_SETTINGS_FIELD_W, PZ_SETTINGS_FIELD_H)) field = 3;
        if (field >= 0) ui->settings_field = field;
        if (event->button == Button2 && field >= 0)
            request_paste(ui, XA_PRIMARY, 0, event->time);
        redraw(ui);
        return;
    }

    if (event->button == Button2) {
        if (in_rect(x, y, PZ_INPUT_X, PZ_INPUT_Y, PZ_INPUT_W, PZ_INPUT_H)) {
            ui->filter_focus = 0;
            request_paste(ui, XA_PRIMARY, 0, event->time);
        } else if (in_rect(x, y, PZ_MARGIN, PZ_FILTER_Y, PZ_FILTER_W,
                           PZ_FILTER_H)) {
            ui->filter_focus = 1;
            request_paste(ui, XA_PRIMARY, 0, event->time);
        }
        return;
    }

    if (event->button == Button1 && activity_bar_at(x, y)) {
        int thumb_y = 0, thumb_h = 0;
        if (activity_metrics(ui, &thumb_y, &thumb_h)) {
            if (y >= thumb_y - PZ_SCROLL_GRAB && y < thumb_y + thumb_h + PZ_SCROLL_GRAB) {
                ui->activity_drag = 1;
                ui->activity_drag_y = y;
                ui->activity_drag_offset = ui->activity_offset;
            } else {
                ui->activity_offset += y < thumb_y ? -PZ_ACTIVITY_LINES : PZ_ACTIVITY_LINES;
                clamp_activity_offset(ui);
            }
            redraw(ui);
        }
        return;
    }

    if (event->button == Button1 && scrollbar_at(x, y)) {
        int thumb_y = 0, thumb_h = 0;
        if (scrollbar_metrics(ui, &thumb_y, &thumb_h)) {
            if (y >= thumb_y - PZ_SCROLL_GRAB && y < thumb_y + thumb_h + PZ_SCROLL_GRAB) {
                ui->scroll_drag = 1;
                ui->scroll_drag_y = y;
                ui->scroll_drag_offset = ui->list_offset;
            } else {
                ui->list_offset += y < thumb_y ? -PZ_ROWS : PZ_ROWS;
                ui->list_offset = min_int(max_int(0, ui->list_offset),
                                          max_int(0, ui->visible_count - PZ_ROWS));
            }
            redraw(ui);
        }
        return;
    }

    if (event->button == Button4 || event->button == Button5) {
        const int step = event->button == Button4 ? -1 : 1;
        if (activity_bar_at(x, y) ||
            in_rect(x, y, PZ_MARGIN, PZ_ACTIVITY_Y, PZ_CARD_W, PZ_ACTIVITY_H)) {
            ui->activity_offset += step;
            clamp_activity_offset(ui);
            ui->frame_pending = 1;
            return;
        }
        if (row_at(ui, x, y) >= 0 || scrollbar_at(x, y) ||
            in_rect(x, y, PZ_MARGIN, PZ_LIST_TOP, PZ_CARD_W,
                    PZ_LIST_BOTTOM - PZ_LIST_TOP)) {
            ui->list_offset += step;
            ui->list_offset = min_int(max_int(0, ui->list_offset),
                                      max_int(0, ui->visible_count - PZ_ROWS));
        }
        ui->frame_pending = 1;
        return;
    }

    if (event->button == Button1 && ui->input[0]) {
        const Rect clear = {PZ_INPUT_X + PZ_INPUT_W - 24, PZ_INPUT_Y + 2, 20,
                            PZ_INPUT_H - 4};
        if (in_rect(x, y, clear.x, clear.y, clear.w, clear.h)) {
            ui->input[0] = '\0';
            ui->filter_focus = 0;
            redraw(ui);
            return;
        }
    }
    if (in_rect(x, y, PZ_INPUT_X, PZ_INPUT_Y, PZ_INPUT_W, PZ_INPUT_H)) {
        ui->filter_focus = 0;
        redraw(ui);
        return;
    }
    if (ui->filter_text[0]) {
        const Rect clear = {PZ_MARGIN + PZ_FILTER_W - 24, PZ_FILTER_Y + 2, 20,
                            PZ_FILTER_H - 4};
        if (in_rect(x, y, clear.x, clear.y, clear.w, clear.h)) {
            ui->filter_text[0] = '\0';
            ui->list_offset = 0;
            ui->list_dirty = 1;
            ui->filter_focus = 1;
            redraw(ui);
            return;
        }
    }
    if (in_rect(x, y, PZ_MARGIN, PZ_FILTER_Y, PZ_FILTER_W, PZ_FILTER_H)) {
        ui->filter_focus = 1;
        redraw(ui);
        return;
    }

    const Btn button = button_at(ui, x, y);
    if (button == BtnFilterMenu) {
        ui->filter_menu_open = 1;
        ui->filter_menu_hover = -1;
        ui->filter_focus = 0;
        redraw(ui);
        return;
    }
    if (button == BtnSort) {
        ui->sort_mode = 1 - ui->sort_mode;
        ui->list_offset = 0;
        ui->list_dirty = 1;
        ui->filter_focus = 0;
        redraw(ui);
        return;
    }
    if (button == BtnCheck) { start_command(ui, "check"); return; }
    if (button == BtnSelfUpdate) { start_command(ui, "self-update"); return; }
    if (button == BtnInstall) { submit(ui); return; }
    if (button == BtnImport) {
        char line[PZ_MAX_LINE];
        text_copy(line, sizeof line, "import");
        if (ui->input[0]) {
            text_append_char(line, sizeof line, ' ');
            text_append(line, sizeof line, ui->input);
            ui->input[0] = '\0';
        }
        start_command(ui, line);
        return;
    }
    if (button == BtnSettings) { open_settings(ui); return; }
    if (button == BtnEnable) { open_targets(ui, 0, 0); return; }
    if (button == BtnShareServer) { open_targets(ui, 1, 1); return; }
    if (button == BtnUpdate) {
        if (ui->selected_count) {
            char line[PZ_MAX_LINE];
            build_ids_command(ui, "update", line, sizeof line);
            start_command(ui, line);
        }
        return;
    }
    if (button == BtnRemove) { remove_selected(ui); return; }
    if (button == BtnUpdateAll) { start_command(ui, "update"); return; }
    if (button == BtnSelectAll) {
        for (int i = 0; i < ui->visible_count; ++i)
            select_row(ui, &ui->rows[ui->visible[i]]);
        ui->confirm_remove = 0;
        redraw(ui);
        return;
    }
    if (button == BtnClear) {
        clear_selection(ui);
        ui->confirm_remove = 0;
        redraw(ui);
        return;
    }
    if (button == BtnLock) {
        int all_locked = ui->selected_count != 0;
        for (int i = 0; i < ui->selected_count; ++i)
            if (!pz_mod_manager_manually_locked(ui->manager, ui->selected_ids[i])) {
                all_locked = 0;
                break;
            }
        char line[PZ_MAX_LINE];
        build_ids_command(ui, all_locked ? "unlock" : "lock", line, sizeof line);
        start_command(ui, line);
        return;
    }
    if (button == BtnCopy) { copy_ids(ui); return; }

    const int index = row_at(ui, x, y);
    if (index >= 0) {
        Row* row = &ui->rows[ui->visible[index]];
        const uint64_t id = row->workshop_id;
        const uint64_t now = monotonic_millis();
        const int double_click = ui->last_click_index == index &&
                                 ui->last_click_time != 0 &&
                                 now - ui->last_click_time < 450;
        if (double_click && id != 0) {
            clear_selection(ui);
            select_row(ui, row);
            ui->last_click_index = -1;
            char line[64];
            text_copy(line, sizeof line, "update ");
            text_append_u64(line, sizeof line, id);
            start_command(ui, line);
            return;
        }
        if (event->state & ShiftMask) {
            if (ui->anchor_index >= 0) {
                clear_selection(ui);
                const int lo = min_int(ui->anchor_index, index);
                const int hi = max_int(ui->anchor_index, index);
                for (int i = lo; i <= hi && i < ui->visible_count; ++i)
                    select_row(ui, &ui->rows[ui->visible[i]]);
            } else {
                select_row(ui, row);
                ui->anchor_index = index;
            }
        } else {
            if (row_selected(ui, row)) unselect_row(ui, row);
            else select_row(ui, row);
            ui->anchor_index = index;
        }
        ui->last_click_index = index;
        ui->last_click_time = now;
        ui->confirm_remove = 0;
        redraw(ui);
    }
}

static void handle_key_press(Ui* ui, XKeyEvent* event) {
    if (ui->busy) return;
    char buffer[32];
    KeySym symbol = 0;
    const int count = XLookupString(event, buffer, sizeof buffer - 1, &symbol, NULL);
    buffer[count > 0 ? count : 0] = '\0';
    const int control = (event->state & ControlMask) != 0;
    const int shift = (event->state & ShiftMask) != 0;

    if (control && (symbol == XK_v || symbol == XK_V)) {
        request_paste(ui, ui->clipboard, 0, event->time);
        return;
    }
    if (shift && symbol == XK_Insert) {
        request_paste(ui, XA_PRIMARY, 0, event->time);
        return;
    }

    if (ui->filter_menu_open) {
        int options = 1;
        if (ui->filter_updates > 0) ++options;
        if (ui->filter_locked > 0) ++options;
        if (ui->filter_nontracked > 0) ++options;
        if (symbol == XK_Escape) {
            ui->filter_menu_open = 0;
            ui->filter_menu_hover = -1;
        } else if (symbol == XK_Down || symbol == XK_Up) {
            const int step = symbol == XK_Down ? 1 : -1;
            if (ui->filter_menu_hover < 0)
                ui->filter_menu_hover = step > 0 ? 0 : options - 1;
            else
                ui->filter_menu_hover = max_int(0, min_int(options - 1,
                    ui->filter_menu_hover + step));
        } else if ((symbol == XK_Return || symbol == XK_KP_Enter) &&
                   ui->filter_menu_hover >= 0) {
            ui->filter_mode = filter_option(ui, ui->filter_menu_hover);
            ui->filter_menu_open = 0;
            ui->filter_menu_hover = -1;
            ui->list_offset = 0;
            ui->list_dirty = 1;
        }
        redraw(ui);
        return;
    }

    if (ui->targets_open) {
        if (symbol == XK_Escape) {
            ui->targets_open = 0;
            redraw(ui);
        } else if ((symbol == XK_Return || symbol == XK_KP_Enter) &&
                   ui->target_focus >= 0 && ui->target_mod_id_count != 0) {
            char line[PZ_MAX_LINE];
            selection_command(ui, ui->target_focus, 1, line, sizeof line);
            ui->targets_open = 0;
            start_command(ui, line);
        }
        return;
    }

    if (ui->settings_open) {
        if (symbol == XK_Return || symbol == XK_KP_Enter) {
            save_settings(ui);
        } else if (symbol == XK_Escape) {
            ui->settings_open = 0;
            redraw(ui);
        } else if (symbol == XK_Tab) {
            ui->settings_field = (ui->settings_field + 1) % 4;
            redraw(ui);
        } else if (symbol == XK_BackSpace) {
            text_pop(settings_field_text(ui));
            redraw(ui);
        } else if (count > 0 && (unsigned char)buffer[0] >= 32) {
            char* field = settings_field_text(ui);
            text_append_n(field, settings_field_capacity(ui), buffer, (size_t)count);
            redraw(ui);
        }
        return;
    }

    if (!ui->filter_focus && (symbol == XK_Page_Up || symbol == XK_Page_Down)) {
        ui->list_offset += symbol == XK_Page_Up ? -PZ_ROWS : PZ_ROWS;
        ui->list_offset = min_int(max_int(0, ui->list_offset),
                                  max_int(0, ui->visible_count - PZ_ROWS));
        redraw(ui);
        return;
    }
    if (symbol == XK_Return || symbol == XK_KP_Enter) {
        if (!ui->filter_focus) submit(ui);
    } else if (symbol == XK_BackSpace) {
        if (ui->filter_focus) {
            text_pop(ui->filter_text);
            ui->list_offset = 0;
            ui->list_dirty = 1;
        } else {
            text_pop(ui->input);
        }
        redraw(ui);
    } else if (symbol == XK_Escape) {
        if (ui->filter_text[0]) {
            ui->filter_text[0] = '\0';
            ui->list_offset = 0;
            ui->list_dirty = 1;
        } else if (ui->filter_mode != FilterAll) {
            ui->filter_mode = FilterAll;
            ui->list_offset = 0;
            ui->list_dirty = 1;
        } else if (selected_any(ui)) {
            clear_selection(ui);
            ui->confirm_remove = 0;
        } else {
            ui->input[0] = '\0';
        }
        redraw(ui);
    } else if (count > 0 && (unsigned char)buffer[0] >= 32) {
        if (ui->filter_focus) {
            text_append_n(ui->filter_text, sizeof ui->filter_text, buffer, (size_t)count);
            ui->list_offset = 0;
            ui->list_dirty = 1;
        } else {
            text_append_n(ui->input, sizeof ui->input, buffer, (size_t)count);
        }
        redraw(ui);
    }
}

static void handle_events(Ui* ui) {
    while (XPending(ui->display) > 0) {
        XEvent event;
        XNextEvent(ui->display, &event);
        switch (event.type) {
        case Expose:
            redraw(ui);
            if (!ui->auto_check_started) {
                ui->auto_check_started = 1;
                if (ui->auto_check_due) start_command(ui, "check");
            }
            break;
        case MotionNotify:
            ui->last_mouse_x = event.xmotion.x;
            ui->last_mouse_y = event.xmotion.y;
            if (ui->scroll_drag) {
                int thumb_y = 0, thumb_h = 0;
                if (scrollbar_metrics(ui, &thumb_y, &thumb_h)) {
                    const int travel = (PZ_LIST_BOTTOM - PZ_LIST_TOP) - thumb_h;
                    const int max_offset = ui->visible_count - PZ_ROWS;
                    if (travel > 0 && max_offset > 0) {
                        const int delta = event.xmotion.y - ui->scroll_drag_y;
                        ui->list_offset = ui->scroll_drag_offset + delta * max_offset / travel;
                        ui->list_offset = min_int(max_int(0, ui->list_offset), max_offset);
                    }
                }
                ui->frame_pending = 1;
                break;
            }
            if (ui->activity_drag) {
                int thumb_y = 0, thumb_h = 0;
                if (activity_metrics(ui, &thumb_y, &thumb_h)) {
                    const int travel = (PZ_ACTIVITY_TRACK_BOTTOM - PZ_ACTIVITY_TRACK_TOP) - thumb_h;
                    const int max_offset = ui->log_count - PZ_ACTIVITY_LINES;
                    if (travel > 0 && max_offset > 0) {
                        const int delta = event.xmotion.y - ui->activity_drag_y;
                        ui->activity_offset = ui->activity_drag_offset + delta * max_offset / travel;
                        clamp_activity_offset(ui);
                    }
                }
                ui->frame_pending = 1;
                break;
            }
            {
                const Btn next_button = button_at(ui, event.xmotion.x, event.xmotion.y);
                const int next_row = row_at(ui, event.xmotion.x, event.xmotion.y);
                const int next_filter = ui->filter_menu_open
                    ? filter_choice_at(ui, event.xmotion.x, event.xmotion.y) : -1;
                if ((int)next_button != ui->hover_button || next_row != ui->hover_row ||
                    next_filter != ui->filter_menu_hover) {
                    ui->hover_button = (int)next_button;
                    ui->hover_row = next_row;
                    ui->filter_menu_hover = next_filter;
                    redraw(ui);
                }
            }
            break;
        case LeaveNotify:
            if (ui->hover_button != -1 || ui->hover_row != -1 ||
                ui->filter_menu_hover != -1) {
                ui->hover_button = -1;
                ui->hover_row = -1;
                ui->filter_menu_hover = -1;
                redraw(ui);
            }
            break;
        case ButtonRelease:
            if (ui->scroll_drag || ui->activity_drag) {
                ui->scroll_drag = 0;
                ui->activity_drag = 0;
                redraw(ui);
            }
            break;
        case ButtonPress:
            handle_button_press(ui, &event.xbutton);
            break;
        case ClientMessage:
            if ((Atom)event.xclient.data.l[0] == ui->wm_delete) {
                if (ui->busy) {
                    push_log(ui, "A command is still running; wait for it before closing.");
                    redraw(ui);
                } else {
                    ui->running = 0;
                }
            }
            break;
        case SelectionClear:
            if (event.xselectionclear.selection == ui->clipboard)
                ui->clipboard_text[0] = '\0';
            break;
        case SelectionRequest: {
            const XSelectionRequestEvent* request = &event.xselectionrequest;
            XSelectionEvent reply;
            memset(&reply, 0, sizeof reply);
            reply.type = SelectionNotify;
            reply.display = ui->display;
            reply.requestor = request->requestor;
            reply.selection = request->selection;
            reply.target = request->target;
            reply.property = None;
            reply.time = request->time;
            if (request->target == XA_STRING || request->target == ui->utf8) {
                const Atom property = request->property == None ? request->target
                                                                 : request->property;
                XChangeProperty(ui->display, request->requestor, property,
                                request->target, 8, PropModeReplace,
                                (const unsigned char*)ui->clipboard_text,
                                (int)strlen(ui->clipboard_text));
                reply.property = property;
            }
            XSendEvent(ui->display, request->requestor, False, 0, (XEvent*)&reply);
            XFlush(ui->display);
            break;
        }
        case SelectionNotify: {
            if (!ui->paste_pending ||
                event.xselection.selection != ui->paste_selection)
                break;
            if (event.xselection.property == None) {
                if (!ui->paste_fallback) {
                    request_paste(ui, ui->paste_selection, 1, ui->paste_time);
                } else {
                    ui->paste_pending = 0;
                    push_log(ui, "Nothing to paste from the clipboard.");
                    redraw(ui);
                }
                break;
            }
            Atom type;
            int format = 0;
            unsigned long count = 0, remaining = 0;
            unsigned char* data = NULL;
            const int status = XGetWindowProperty(
                ui->display, ui->window, event.xselection.property, 0, 65536,
                True, AnyPropertyType, &type, &format, &count, &remaining,
                &data);
            ui->paste_pending = 0;
            if (status != Success || !data || format != 8) {
                if (data) XFree(data);
                push_log(ui, "Could not read the clipboard.");
                redraw(ui);
                break;
            }
            char value[PZ_MAX_LINE];
            text_copy_n(value, sizeof value, (const char*)data, (size_t)count);
            XFree(data);
            paste_into_field(ui, value);
            break;
        }
        case KeyPress:
            handle_key_press(ui, &event.xkey);
            break;
        default:
            break;
        }
    }
    if (ui->frame_pending) redraw(ui);
}

/* ------------------------------------------------------------------ main */

int pz_x11_run_ui(pz_mod_manager* manager) {
    Ui* ui = (Ui*)calloc(1, sizeof *ui);
    if (!ui) return 1;
    ui->manager = manager;
    ui->wake[0] = -1;
    ui->wake[1] = -1;
    ui->watch_fd = -1;
    ui->running = 1;
    pthread_mutex_init(&ui->pending_mu, NULL);

    ui->display = XOpenDisplay(NULL);
    if (!ui->display) {
        fprintf(stderr, "Cannot open X display (DISPLAY unset? XWayland?)\n");
        free(ui);
        return 1;
    }
    ui->screen = DefaultScreen(ui->display);
    init_palette(ui);
    sync_config(ui);
    apply_palette(ui);

    ui->window = XCreateSimpleWindow(
        ui->display, RootWindow(ui->display, ui->screen), 80, 60,
        PZ_WINDOW_WIDTH, PZ_WINDOW_HEIGHT, 1, ui->pal.border, ui->pal.bg);
    if (!ui->window) {
        fprintf(stderr, "Cannot create X11 window\n");
        XCloseDisplay(ui->display);
        free(ui);
        return 1;
    }
    XStoreName(ui->display, ui->window, "PZ Mod Manager");
    XSizeHints hints;
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = PZ_WINDOW_WIDTH;
    hints.min_height = hints.max_height = PZ_WINDOW_HEIGHT;
    XSetWMNormalHints(ui->display, ui->window, &hints);
    XSelectInput(ui->display, ui->window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                     ButtonReleaseMask | StructureNotifyMask |
                     PointerMotionMask | LeaveWindowMask);
    ui->wm_delete = XInternAtom(ui->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ui->display, ui->window, &ui->wm_delete, 1);

    ui->gc = XCreateGC(ui->display, ui->window, 0, NULL);
    ui->backbuf = XCreatePixmap(ui->display, ui->window, PZ_WINDOW_WIDTH,
                                PZ_WINDOW_HEIGHT,
                                (unsigned)DefaultDepth(ui->display, ui->screen));
    if (!ui->backbuf) {
        fprintf(stderr, "Cannot create X11 back buffer\n");
        XFreeGC(ui->display, ui->gc);
        XDestroyWindow(ui->display, ui->window);
        XCloseDisplay(ui->display);
        free(ui);
        return 1;
    }
    ui->target = ui->backbuf;

    ui->body = XLoadQueryFont(
        ui->display, "-misc-fixed-medium-r-normal--13-120-75-75-c-60-iso8859-1");
    if (!ui->body) ui->body = XLoadQueryFont(ui->display, "fixed");
    if (!ui->body) {
        fprintf(stderr, "Cannot load an X11 font\n");
        XFreePixmap(ui->display, ui->backbuf);
        XFreeGC(ui->display, ui->gc);
        XDestroyWindow(ui->display, ui->window);
        XCloseDisplay(ui->display);
        free(ui);
        return 1;
    }
    ui->heading = XLoadQueryFont(
        ui->display, "-misc-fixed-bold-r-normal--18-120-75-75-c-80-iso8859-1");
    if (!ui->heading)
        ui->heading = XLoadQueryFont(
            ui->display, "-misc-fixed-medium-r-normal--15-120-75-75-c-80-iso8859-1");
    if (!ui->heading) ui->heading = ui->body;

    ui->clipboard = XInternAtom(ui->display, "CLIPBOARD", False);
    ui->utf8 = XInternAtom(ui->display, "UTF8_STRING", False);
    ui->paste_prop = XInternAtom(ui->display, "PZMODMGR_PASTE", False);

    if (pipe(ui->wake) != 0) {
        fprintf(stderr, "Cannot create worker wakeup pipe\n");
        if (ui->heading != ui->body) XFreeFont(ui->display, ui->heading);
        XFreeFont(ui->display, ui->body);
        XFreePixmap(ui->display, ui->backbuf);
        XFreeGC(ui->display, ui->gc);
        XDestroyWindow(ui->display, ui->window);
        XCloseDisplay(ui->display);
        free(ui);
        return 1;
    }
    for (int i = 0; i < 2; ++i) {
        const int flags = fcntl(ui->wake[i], F_GETFL, 0);
        if (flags != -1) fcntl(ui->wake[i], F_SETFL, flags | O_NONBLOCK);
        fcntl(ui->wake[i], F_SETFD, FD_CLOEXEC);
    }

    pz_mod_manager_reload_installed(ui->manager);
    rebuild_rows(ui);
    rebuild_visible(ui);
    refresh_dependency(ui);
    ui->auto_check_due = pz_mod_manager_update_check_due(ui->manager);
    ui->app_update_available = pz_mod_manager_app_update_available(ui->manager);
    ui->activity_follow = 1;
    push_log(ui, "Paste Workshop URLs or IDs, then press Enter.");
    push_log(ui, "Click mods to toggle their selection; Shift-click selects a range.");

    refresh_watch(ui);
    XMapWindow(ui->display, ui->window);
    XFlush(ui->display);
    redraw(ui);

    while (ui->running) {
        struct pollfd descriptors[3];
        descriptors[0].fd = ConnectionNumber(ui->display);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = ui->wake[0];
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        descriptors[2].fd = ui->watch_fd;
        descriptors[2].events = POLLIN;
        descriptors[2].revents = 0;
        const nfds_t count = ui->watch_fd >= 0 ? 3 : 2;

        const int ready = poll(descriptors, count, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "poll failed: %s\n", strerror(errno));
            break;
        }

        if (descriptors[1].revents & POLLIN) {
            char buffer[64];
            while (read(ui->wake[0], buffer, sizeof buffer) > 0) {}
            if (drain_pending(ui)) {
                join_worker(ui);
                ui->busy = 0;
                ui->app_update_available =
                    pz_mod_manager_app_update_available(ui->manager);
                if (ui->reload_pending) {
                    ui->reload_pending = 0;
                    pz_mod_manager_reload_installed(ui->manager);
                }
                clear_icons(ui);
                rebuild_rows(ui);
                ui->hover_row = -1;
                refresh_dependency(ui);
            }
            redraw(ui);
        }
        if (ui->watch_fd >= 0 && (descriptors[2].revents & POLLIN))
            handle_watch(ui);
        if (descriptors[0].revents & POLLIN) handle_events(ui);
    }

    join_worker(ui);
    close_watch(ui);
    clear_icons(ui);
    clear_target_icons(ui);
    for (int i = 0; i < ui->target_count; ++i)
        pz_activation_target_data_free(&ui->targets[i]);
    if (ui->heading != ui->body) XFreeFont(ui->display, ui->heading);
    XFreeFont(ui->display, ui->body);
    XFreePixmap(ui->display, ui->backbuf);
    XFreeGC(ui->display, ui->gc);
    XDestroyWindow(ui->display, ui->window);
    XCloseDisplay(ui->display);
    close(ui->wake[0]);
    close(ui->wake[1]);
    pthread_mutex_destroy(&ui->pending_mu);
    for (int i = 0; i < PZ_MAX_LOG; ++i) pz_str_free(&ui->log[i]);
    free(ui->selected_folders);
    free(ui->rows);
    free(ui->visible);
    free(ui->row_enabled);
    free(ui->infos);
    free(ui->icons);
    free(ui->target_icons);
    free(ui);
    return 0;
}

