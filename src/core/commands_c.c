#include "core/commands_c.h"

#include "core/fs_c.h"
#include "core/paths_c.h"
#include "core/updater_c.h"
#include "core/url_c.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PZMODMGR_VERSION
#define PZMODMGR_VERSION "0.0.0"
#endif

/* ------------------------------------------------------------------ text */

typedef struct text_slice {
    const char* data;
    size_t length;
} text_slice;

typedef struct builder {
    pz_str text;
    int ok;
} builder;

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static text_slice slice_trim_left(text_slice value) {
    while (value.length != 0 && is_space(value.data[0])) {
        ++value.data;
        --value.length;
    }
    return value;
}

static text_slice split_first(text_slice line, text_slice* rest) {
    line = slice_trim_left(line);
    size_t end = 0;
    while (end < line.length && !is_space(line.data[end])) ++end;
    text_slice first = {line.data, end};
    text_slice tail = {line.data + end, line.length - end};
    *rest = slice_trim_left(tail);
    return first;
}

static int slice_equals(text_slice value, const char* text) {
    const size_t length = strlen(text);
    return value.length == length &&
           (length == 0 || memcmp(value.data, text, length) == 0);
}

static int parse_decimal(text_slice value, uint64_t* out) {
    if (value.length == 0) return 0;
    uint64_t number = 0;
    for (size_t i = 0; i < value.length; ++i) {
        const unsigned char c = (unsigned char)value.data[i];
        if (c < '0' || c > '9') return 0;
        const unsigned digit = (unsigned)(c - '0');
        if (number > (UINT64_MAX - digit) / 10) return 0;
        number = number * 10 + digit;
    }
    *out = number;
    return 1;
}

static int parse_index(text_slice value, size_t* out) {
    uint64_t number = 0;
    if (!parse_decimal(value, &number)) return 0;
    if (number > (uint64_t)SIZE_MAX) return 0;
    *out = (size_t)number;
    return 1;
}

static unsigned char lower_ascii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

static int str_equals_ci(const pz_str* value, text_slice token) {
    if (!value || value->length != token.length) return 0;
    for (size_t i = 0; i < token.length; ++i) {
        if (lower_ascii((unsigned char)value->data[i]) !=
            lower_ascii((unsigned char)token.data[i]))
            return 0;
    }
    return 1;
}

static int str_contains_ci(const pz_str* value, text_slice token) {
    if (!value) return 0;
    if (token.length == 0) return 1;
    if (value->length < token.length) return 0;
    for (size_t i = 0; i + token.length <= value->length; ++i) {
        size_t j = 0;
        while (j < token.length &&
               lower_ascii((unsigned char)value->data[i + j]) ==
                   lower_ascii((unsigned char)token.data[j]))
            ++j;
        if (j == token.length) return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- builders */

static void build_init(builder* value) {
    pz_str_init(&value->text);
    value->ok = 1;
}

static void build_text(builder* value, const char* data, size_t length) {
    if (value->ok) value->ok = pz_str_append(&value->text, data, length);
}

static void build_slice(builder* value, text_slice text) {
    build_text(value, text.data, text.length);
}

static void build_cstr(builder* value, const char* text) {
    if (value->ok) value->ok = pz_str_append_cstr(&value->text, text);
}

static void build_char(builder* value, char c) {
    if (value->ok) value->ok = pz_str_append_char(&value->text, c);
}

static void build_u64(builder* value, uint64_t number) {
    if (value->ok) value->ok = pz_str_append_u64(&value->text, number);
}

/* Moves the builder's text into the result. An allocation failure reports
 * ok=0 with an empty message, like the C manager helpers. */
static pz_mm_result build_finish(builder* value, int ok) {
    pz_mm_result result;
    pz_mm_result_init(&result);
    result.ok = ok && value->ok ? 1 : 0;
    if (value->ok) {
        result.message = value->text;
        pz_str_init(&value->text);
    } else {
        pz_str_free(&value->text);
    }
    return result;
}

static pz_mm_result result_cstr(int ok, const char* text) {
    builder value;
    build_init(&value);
    build_cstr(&value, text);
    return build_finish(&value, ok);
}

static pz_mm_result result_prefix(int ok, const char* prefix, text_slice text) {
    builder value;
    build_init(&value);
    build_cstr(&value, prefix);
    build_slice(&value, text);
    return build_finish(&value, ok);
}

static pz_mm_result out_of_memory(void) {
    return result_cstr(0, "out of memory");
}

/* ------------------------------------------------------------------ ids */

static int ids_add_unique(pz_vec* ids, uint64_t id) {
    if (id == 0) return 1;
    for (size_t i = 0; i < ids->length; ++i) {
        if (*(const uint64_t*)pz_vec_at_const(ids, i) == id) return 1;
    }
    return pz_vec_push_copy(ids, &id);
}

static void parse_ids(text_slice input, pz_vec* ids) {
    pz_vec_init(ids, sizeof(uint64_t));
    pz_url_ids parsed;
    parsed.items = NULL;
    parsed.count = 0;
    if (!pz_url_parse_workshop_ids(input.data, input.length, &parsed)) {
        pz_url_ids_free(&parsed);
        return;
    }
    for (size_t i = 0; i < parsed.count; ++i)
        pz_vec_push_copy(ids, &parsed.items[i]);
    pz_url_ids_free(&parsed);
}

static int ids_append(pz_vec* out, const pz_vec* values) {
    for (size_t i = 0; i < values->length; ++i) {
        if (!ids_add_unique(out, *(const uint64_t*)pz_vec_at_const(values, i)))
            return 0;
    }
    return 1;
}

static void format_id(pz_str* out, uint64_t id) {
    pz_str_init(out);
    pz_str_append_u64(out, id);
}

static void log_id_result(const pz_log* log, uint64_t id,
                          const pz_str* message) {
    if (!log || !log->callback) return;
    builder line;
    build_init(&line);
    build_u64(&line, id);
    build_cstr(&line, ": ");
    build_text(&line, message->data, message->length);
    pz_log_emit_str(log, &line.text);
    pz_str_free(&line.text);
}

static pz_mm_result add_one(pz_mod_manager* manager, uint64_t id,
                            const pz_log* log) {
    pz_str text;
    format_id(&text, id);
    if (!text.data) return out_of_memory();
    pz_mm_result result =
        pz_mod_manager_add(manager, text.data, text.length, log);
    pz_str_free(&text);
    return result;
}

static pz_mm_result batch_result(const char* verb, const char* noun,
                                 size_t total, int successes, int failures,
                                 const char* failure_word) {
    builder value;
    build_init(&value);
    build_cstr(&value, verb);
    build_char(&value, ' ');
    build_u64(&value, (uint64_t)successes);
    build_cstr(&value, " of ");
    build_u64(&value, (uint64_t)total);
    build_char(&value, ' ');
    build_cstr(&value, noun);
    if (failures != 0) {
        build_cstr(&value, ", ");
        build_u64(&value, (uint64_t)failures);
        build_char(&value, ' ');
        build_cstr(&value, failure_word);
    }
    return build_finish(&value, failures == 0);
}

static pz_mm_result add_workshop_ids(pz_mod_manager* manager,
                                     const pz_vec* ids, const pz_log* log) {
    if (ids->length == 0) return result_cstr(0, "No Workshop ID found");
    if (ids->length == 1)
        return add_one(manager, *(const uint64_t*)pz_vec_at_const(ids, 0), log);

    int successes = 0;
    int failures = 0;
    for (size_t i = 0; i < ids->length; ++i) {
        const uint64_t id = *(const uint64_t*)pz_vec_at_const(ids, i);
        pz_mm_result result = add_one(manager, id, log);
        if (result.ok) ++successes;
        else ++failures;
        log_id_result(log, id, &result.message);
        pz_mm_result_free(&result);
    }
    return batch_result("Installed", "Workshop item(s)", ids->length, successes,
                        failures, "failed");
}

static pz_mm_result add_workshop_input(pz_mod_manager* manager,
                                       text_slice input, const pz_log* log) {
    pz_vec ids;
    parse_ids(input, &ids);
    if (ids.length == 0) {
        pz_vec_free(&ids);
        return result_prefix(0, "No Workshop ID found in: ", input);
    }
    pz_mm_result result = add_workshop_ids(manager, &ids, log);
    pz_vec_free(&ids);
    return result;
}

/* --------------------------------------------------------------- locking */

/* Returns 1 on a unique match, 0 on no/ambiguous match, and -1 when an
 * allocation failed. */
static int lock_match(const pz_vec* mods, text_slice token, pz_vec* out) {
    uint64_t workshop_id = 0;
    if (pz_url_parse_workshop_id(token.data, token.length, &workshop_id)) {
        for (size_t i = 0; i < mods->length; ++i) {
            const pz_installed_mod* mod = pz_vec_at_const(mods, i);
            if (mod->workshop_id == workshop_id)
                return ids_add_unique(out, workshop_id) ? 1 : -1;
        }
        return 0;
    }

    pz_vec exact;
    pz_vec partial;
    pz_vec_init(&exact, sizeof(uint64_t));
    pz_vec_init(&partial, sizeof(uint64_t));
    int alloc_ok = 1;
    for (size_t i = 0; i < mods->length; ++i) {
        const pz_installed_mod* mod = pz_vec_at_const(mods, i);
        if (mod->workshop_id == 0) continue;
        if (str_equals_ci(&mod->name, token) ||
            str_equals_ci(&mod->folder, token)) {
            if (!ids_add_unique(&exact, mod->workshop_id)) alloc_ok = 0;
        } else if (str_contains_ci(&mod->name, token) ||
                   str_contains_ci(&mod->folder, token)) {
            if (!ids_add_unique(&partial, mod->workshop_id)) alloc_ok = 0;
        }
    }

    int matched = 0;
    if (!alloc_ok) {
        matched = -1;
    } else if (exact.length == 1) {
        matched = ids_add_unique(
            out, *(const uint64_t*)pz_vec_at_const(&exact, 0)) ? 1 : -1;
    } else if (exact.length == 0 && partial.length == 1) {
        matched = ids_add_unique(
            out, *(const uint64_t*)pz_vec_at_const(&partial, 0)) ? 1 : -1;
    }

    pz_vec_free(&exact);
    pz_vec_free(&partial);
    return matched;
}

static pz_mm_result resolve_lock_ids(pz_mod_manager* manager, text_slice arg,
                                     pz_vec* ids) {
    pz_vec_init(ids, sizeof(uint64_t));
    if (arg.length == 0)
        return result_cstr(0, "expected Workshop IDs or mod names");

    pz_vec mods;
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list_all(manager, &mods);

    pz_vec whole;
    pz_vec_init(&whole, sizeof(uint64_t));
    const int whole_match = lock_match(&mods, arg, &whole);
    if (whole_match < 0) {
        pz_vec_free(&whole);
        pz_mod_manager_free_mods(&mods);
        return out_of_memory();
    }
    if (whole_match && whole.length != 0) {
        *ids = whole; /* ids takes ownership of whole's buffer */
        pz_mod_manager_free_mods(&mods);
        return result_cstr(1, "");
    }
    pz_vec_free(&whole);

    text_slice remaining = arg;
    while (remaining.length != 0) {
        const text_slice token = split_first(remaining, &remaining);
        if (token.length == 0) break;
        pz_vec matched;
        pz_vec_init(&matched, sizeof(uint64_t));
        const int match = lock_match(&mods, token, &matched);
        if (match < 0) {
            pz_vec_free(&matched);
            pz_vec_free(ids);
            pz_mod_manager_free_mods(&mods);
            return out_of_memory();
        }
        if (match == 0 || matched.length == 0) {
            pz_vec_free(&matched);
            pz_vec_free(ids);
            pz_mod_manager_free_mods(&mods);
            return result_prefix(0, "unknown or ambiguous mod: ", token);
        }
        ids_append(ids, &matched);
        pz_vec_free(&matched);
    }

    pz_mod_manager_free_mods(&mods);
    if (ids->length == 0) {
        pz_vec_free(ids);
        return result_prefix(0, "no installed Workshop item matched: ", arg);
    }
    return result_cstr(1, "");
}

/* ------------------------------------------------------------- folder ids */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_folder_token(text_slice token, pz_str* out) {
    pz_str_init(out);
    if (token.length < 3 || token.data[0] != 'f' || token.data[1] != ':' ||
        (token.length - 2) % 2 != 0)
        return 0;
    for (size_t i = 2; i + 1 < token.length; i += 2) {
        const int high = hex_digit(token.data[i]);
        const int low = hex_digit(token.data[i + 1]);
        if (high < 0 || low < 0) return 0;
        if (!pz_str_append_char(out, (char)((high << 4) | low))) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ config */

static int default_zomboid_dir(pz_str* out) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    if (!home) return 0;
    pz_str_init(out);
    if (!pz_str_assign_cstr(out, home)) return 0;
    if (!pz_str_append_char(out, pz_path_separator())) return 0;
    return pz_str_append_cstr(out, "Zomboid");
}

static int truthy(text_slice value, int allow_dark) {
    return slice_equals(value, "1") || slice_equals(value, "on") ||
           slice_equals(value, "true") || slice_equals(value, "yes") ||
           (allow_dark && slice_equals(value, "dark"));
}

static pz_mm_result config_command(pz_mod_manager* manager, text_slice key,
                                   text_slice value) {
    if (slice_equals(key, "zomboid_dir")) {
        pz_str dir;
        pz_str_init(&dir);
        const int assigned =
            value.length != 0
                ? pz_str_assign(&dir, value.data, value.length)
                : default_zomboid_dir(&dir);
        if (!assigned) {
            pz_str_free(&dir);
            return out_of_memory();
        }
        pz_mm_result result =
            pz_mod_manager_set_zomboid_dir(manager, dir.data, dir.length);
        pz_str_free(&dir);
        if (!result.ok) return result;
        pz_mm_result_free(&result);
        if (!pz_mod_manager_save_config(manager))
            return result_cstr(0, "could not save config");
        builder saved;
        build_init(&saved);
        build_cstr(&saved, "saved ");
        build_slice(&saved, key);
        return build_finish(&saved, 1);
    }

    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(manager, &config);

    int recognized = 1;
    int alloc_ok = 1;
    if (slice_equals(key, "steamcmd_exe")) {
        alloc_ok = pz_str_assign(&config.steamcmd_exe, value.data, value.length);
    } else if (slice_equals(key, "steamcmd_root")) {
        alloc_ok = pz_str_assign(&config.steamcmd_root, value.data, value.length);
    } else if (slice_equals(key, "steam_user")) {
        alloc_ok = pz_str_assign(&config.steam_user, value.data, value.length);
    } else if (slice_equals(key, "dark_mode")) {
        config.dark_mode = truthy(value, 1);
    } else if (slice_equals(key, "auto_enable")) {
        config.auto_enable = truthy(value, 0);
    } else {
        recognized = 0;
    }

    if (!recognized) {
        pz_paths_config_free(&config);
        return result_prefix(0, "unknown config key: ", key);
    }
    if (!alloc_ok) {
        pz_paths_config_free(&config);
        return out_of_memory();
    }

    pz_mod_manager_set_config(manager, &config);
    pz_paths_config_free(&config);
    if (!pz_mod_manager_save_config(manager))
        return result_cstr(0, "could not save config");

    builder saved;
    build_init(&saved);
    build_cstr(&saved, "saved ");
    build_slice(&saved, key);
    return build_finish(&saved, 1);
}

/* ---------------------------------------------------------------- updater */

static pz_mm_result stage_self_update(pz_mod_manager* manager,
                                      const pz_log* log) {
    pz_paths_config config;
    pz_paths_config_init(&config);
    pz_mod_manager_get_config(manager, &config);

    pz_path data_dir;
    pz_path_init(&data_dir);
    pz_path_assign(&data_dir, config.data_dir.data, config.data_dir.length);

    pz_str message;
    pz_str_init(&message);
    const char* version = PZMODMGR_VERSION;
    const char* platform = pz_updater_platform_name();
    const int ok = pz_updater_stage(
        &data_dir, version, strlen(version), platform, strlen(platform), log,
        &message);

    pz_mm_result result;
    pz_mm_result_init(&result);
    result.ok = ok;
    if (message.data) {
        result.message = message;
        pz_str_init(&message);
    }
    pz_str_free(&message);
    pz_path_free(&data_dir);
    pz_paths_config_free(&config);
    return result;
}

static pz_mm_result check_command(pz_mod_manager* manager, const pz_log* log) {
    pz_mm_result result = pz_mod_manager_check_updates(manager, log);
    pz_mm_result app = pz_mod_manager_check_app_release(manager, log);
    if (app.ok) {
        pz_str_append_char(&result.message, ' ');
        pz_str_append(&result.message, app.message.data, app.message.length);
    }
    pz_mm_result_free(&app);
    return result;
}

/* ----------------------------------------------------------------- targets */

static pz_mm_result targets_command(pz_mod_manager* manager, const pz_log* log) {
    pz_vec targets;
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);

    for (size_t i = 0; i < targets.length; ++i) {
        const pz_activation_target_data* target = pz_vec_at_const(&targets, i);
        builder line;
        build_init(&line);
        build_u64(&line, (uint64_t)i);
        build_cstr(&line, ": ");
        build_text(&line, target->label.data, target->label.length);
        build_cstr(&line, " (");
        build_u64(&line, (uint64_t)target->mod_ids.length);
        build_cstr(&line, " mod ID(s))");
        if (target->lock_updates) build_cstr(&line, " [updates locked]");
        if (log && log->callback) pz_log_emit_str(log, &line.text);
        pz_str_free(&line.text);
    }

    builder message;
    build_init(&message);
    build_u64(&message, (uint64_t)targets.length);
    build_cstr(&message,
               " activation target(s); use enable/disable <index> <Workshop "
               "IDs>");
    pz_mod_manager_free_targets(&targets);
    return build_finish(&message, 1);
}

static pz_mm_result target_lock_command(pz_mod_manager* manager,
                                        text_slice arg, const pz_log* log) {
    text_slice state;
    const text_slice index_text = split_first(arg, &state);
    if (index_text.length == 0 ||
        (!slice_equals(state, "on") && !slice_equals(state, "off")))
        return result_cstr(0, "expected: target-lock <index> on|off");

    size_t index = 0;
    if (!parse_index(index_text, &index))
        return result_prefix(0, "unknown target index: ", index_text);

    pz_vec targets;
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);
    if (index >= targets.length) {
        pz_mod_manager_free_targets(&targets);
        return result_prefix(0, "unknown target index: ", index_text);
    }
    const pz_activation_target_data* target = pz_vec_at_const(&targets, index);
    pz_mm_result result = pz_mod_manager_set_target_locked(
        manager, target->kind, target->name.data, target->name.length,
        slice_equals(state, "on"), log);
    pz_mod_manager_free_targets(&targets);
    return result;
}

static pz_mm_result set_enabled_command(pz_mod_manager* manager,
                                        text_slice arg, const pz_log* log) {
    text_slice rest;
    text_slice state;
    const text_slice index_text = split_first(arg, &rest);
    state = split_first(rest, &rest);
    if (index_text.length == 0 ||
        (!slice_equals(state, "on") && !slice_equals(state, "off")))
        return result_cstr(
            0, "expected: set-enabled <index> on|off [w:ID|f:folder]...");

    size_t index = 0;
    if (!parse_index(index_text, &index))
        return result_prefix(0, "unknown target index: ", index_text);

    pz_vec targets;
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);
    if (index >= targets.length) {
        pz_mod_manager_free_targets(&targets);
        return result_prefix(0, "unknown target index: ", index_text);
    }

    pz_vec ids;
    pz_vec folders; /* pz_str */
    pz_vec_init(&ids, sizeof(uint64_t));
    pz_vec_init(&folders, sizeof(pz_str));
    const char* error_text = NULL;
    text_slice error_value = {NULL, 0};
    int failed = 0;

    text_slice remaining = rest;
    while (remaining.length != 0) {
        const text_slice token = split_first(remaining, &remaining);
        if (token.length == 0) break;
        if (token.length >= 2 && token.data[0] == 'w' && token.data[1] == ':') {
            const text_slice number = {token.data + 2, token.length - 2};
            uint64_t id = 0;
            if (!parse_decimal(number, &id) || !pz_vec_push_copy(&ids, &id)) {
                failed = 1;
                error_text = "bad Workshop ID token: ";
                error_value = token;
                break;
            }
        } else if (token.length >= 2 && token.data[0] == 'f' &&
                   token.data[1] == ':') {
            pz_str folder;
            if (!decode_folder_token(token, &folder)) {
                pz_str_free(&folder);
                failed = 1;
                error_text = "bad folder token: ";
                error_value = token;
                break;
            }
            if (!pz_vec_push_copy(&folders, &folder)) {
                pz_str_free(&folder);
                failed = 1;
                error_text = "out of memory";
                break;
            }
        } else {
            failed = 1;
            error_text = "unknown token: ";
            error_value = token;
            break;
        }
    }

    pz_mm_result result;
    pz_mm_result_init(&result);
    if (failed) {
        result = error_value.length != 0
                     ? result_prefix(0, error_text, error_value)
                     : result_cstr(0, error_text);
    } else {
        const pz_activation_target_data* target =
            pz_vec_at_const(&targets, index);
        const char** folder_names = NULL;
        size_t* folder_lengths = NULL;
        int alloc_ok = 1;
        if (folders.length != 0) {
            folder_names =
                (const char**)calloc(folders.length, sizeof(const char*));
            folder_lengths = (size_t*)calloc(folders.length, sizeof(size_t));
            if (!folder_names || !folder_lengths) {
                alloc_ok = 0;
            } else {
                for (size_t i = 0; i < folders.length; ++i) {
                    const pz_str* folder = pz_vec_at_const(&folders, i);
                    folder_names[i] = folder->data;
                    folder_lengths[i] = folder->length;
                }
            }
        }
        if (!alloc_ok) {
            result = out_of_memory();
        } else {
            result = pz_mod_manager_set_selection_enabled(
                manager, target->kind, target->name.data, target->name.length,
                (const uint64_t*)ids.data, ids.length, folder_names,
                folder_lengths, folders.length, slice_equals(state, "on"),
                log);
        }
        free(folder_names);
        free(folder_lengths);
    }

    for (size_t i = 0; i < folders.length; ++i)
        pz_str_free((pz_str*)pz_vec_at(&folders, i));
    pz_vec_free(&folders);
    pz_vec_free(&ids);
    pz_mod_manager_free_targets(&targets);
    return result;
}

static pz_mm_result enable_command(pz_mod_manager* manager, text_slice arg,
                                   int enabled, const pz_log* log) {
    text_slice ids_text;
    const text_slice index_text = split_first(arg, &ids_text);
    pz_vec ids;
    parse_ids(ids_text, &ids);
    if (index_text.length == 0 || ids.length == 0) {
        pz_vec_free(&ids);
        builder message;
        build_init(&message);
        build_cstr(&message, "expected: ");
        build_cstr(&message, enabled ? "enable" : "disable");
        build_cstr(&message, " <target index> <Workshop ID>...");
        return build_finish(&message, 0);
    }

    size_t index = 0;
    if (!parse_index(index_text, &index)) {
        pz_vec_free(&ids);
        return result_prefix(0, "unknown target index: ", index_text);
    }

    pz_vec targets;
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);
    if (index >= targets.length) {
        pz_vec_free(&ids);
        pz_mod_manager_free_targets(&targets);
        return result_prefix(0, "unknown target index: ", index_text);
    }
    const pz_activation_target_data* target = pz_vec_at_const(&targets, index);
    pz_mm_result result = pz_mod_manager_set_mods_enabled(
        manager, target->kind, target->name.data, target->name.length,
        (const uint64_t*)ids.data, ids.length, enabled, log);
    pz_vec_free(&ids);
    pz_mod_manager_free_targets(&targets);
    return result;
}

static pz_mm_result share_command(pz_mod_manager* manager, text_slice arg,
                                  const pz_log* log) {
    text_slice ignored;
    const text_slice index_text = split_first(arg, &ignored);
    size_t index = 0;
    if (index_text.length == 0 || !parse_index(index_text, &index))
        return result_cstr(0, "expected: share <target index>");

    pz_vec targets;
    pz_vec_init(&targets, sizeof(pz_activation_target_data));
    pz_mod_manager_activation_targets(manager, &targets);
    if (index >= targets.length) {
        pz_mod_manager_free_targets(&targets);
        return result_prefix(0, "unknown target index: ", index_text);
    }
    const pz_activation_target_data* target = pz_vec_at_const(&targets, index);

    pz_vec ids;
    pz_vec_init(&ids, sizeof(uint64_t));
    pz_mod_manager_workshop_ids_for(manager, target, &ids);

    builder list;
    build_init(&list);
    for (size_t i = 0; i < ids.length; ++i) {
        if (list.text.length != 0) build_char(&list, ' ');
        build_u64(&list, *(const uint64_t*)pz_vec_at_const(&ids, i));
    }
    if (log && log->callback) {
        if (ids.length == 0)
            pz_log_emit(log, "(no Workshop IDs)");
        else
            pz_log_emit_str(log, &list.text);
    }

    builder message;
    build_init(&message);
    build_u64(&message, (uint64_t)ids.length);
    build_cstr(&message, " Workshop ID(s) from ");
    build_text(&message, target->label.data, target->label.length);
    build_cstr(&message, "; paste into Add");
    if (ids.length != 0) {
        build_cstr(&message, ", or pin them with: lock ");
        build_text(&message, list.text.data, list.text.length);
    }

    pz_str_free(&list.text);
    pz_vec_free(&ids);
    pz_mod_manager_free_targets(&targets);
    return build_finish(&message, 1);
}

/* ------------------------------------------------------------------- lists */

static pz_mm_result lock_command(pz_mod_manager* manager, text_slice arg,
                                 int locked, const pz_log* log) {
    pz_vec ids;
    pz_mm_result resolved = resolve_lock_ids(manager, arg, &ids);
    if (!resolved.ok) return resolved;
    pz_mm_result_free(&resolved);

    pz_mm_result result = pz_mod_manager_set_locked(
        manager, (const uint64_t*)ids.data, ids.length, locked, log);
    if (!locked) {
        for (size_t i = 0; i < ids.length; ++i) {
            const uint64_t id = *(const uint64_t*)pz_vec_at_const(&ids, i);
            if (pz_mod_manager_server_locked(manager, id)) {
                if (log && log->callback) {
                    builder line;
                    build_init(&line);
                    build_cstr(&line,
                               "still locked by an enabled target: ");
                    build_u64(&line, id);
                    pz_log_emit_str(log, &line.text);
                    pz_str_free(&line.text);
                }
            }
        }
    }
    pz_vec_free(&ids);
    return result;
}

static pz_mm_result update_command(pz_mod_manager* manager, text_slice arg,
                                   const pz_log* log) {
    if (arg.length == 0) return pz_mod_manager_update_all(manager, log);

    pz_vec ids;
    parse_ids(arg, &ids);
    if (ids.length == 0) {
        pz_vec_free(&ids);
        return result_cstr(0, "expected a Workshop ID");
    }
    if (ids.length == 1) {
        pz_mm_result result = pz_mod_manager_update_one(
            manager, *(const uint64_t*)pz_vec_at_const(&ids, 0), log);
        pz_vec_free(&ids);
        return result;
    }

    int successes = 0;
    int failures = 0;
    for (size_t i = 0; i < ids.length; ++i) {
        const uint64_t id = *(const uint64_t*)pz_vec_at_const(&ids, i);
        pz_mm_result result = pz_mod_manager_update_one(manager, id, log);
        if (result.ok) ++successes;
        else ++failures;
        log_id_result(log, id, &result.message);
        pz_mm_result_free(&result);
    }
    pz_mm_result result = batch_result("Updated", "selected mod(s)", ids.length,
                                       successes, failures, "failed");
    pz_vec_free(&ids);
    return result;
}

static pz_mm_result remove_command(pz_mod_manager* manager, text_slice arg,
                                   const pz_log* log) {
    pz_vec ids;
    parse_ids(arg, &ids);
    if (ids.length == 0) {
        pz_vec_free(&ids);
        return result_cstr(0, "expected a Workshop ID");
    }
    if (ids.length == 1) {
        pz_mm_result result = pz_mod_manager_remove(
            manager, *(const uint64_t*)pz_vec_at_const(&ids, 0), log);
        pz_vec_free(&ids);
        return result;
    }

    int successes = 0;
    for (size_t i = 0; i < ids.length; ++i) {
        pz_mm_result result = pz_mod_manager_remove(
            manager, *(const uint64_t*)pz_vec_at_const(&ids, i), log);
        if (result.ok) ++successes;
        pz_mm_result_free(&result);
    }
    pz_mm_result result = batch_result("Removed", "selected mod(s)", ids.length,
                                       successes,
                                       (int)ids.length - successes,
                                       "not installed");
    pz_vec_free(&ids);
    return result;
}

static pz_mm_result list_command(pz_mod_manager* manager, const pz_log* log) {
    pz_vec mods;
    pz_vec_init(&mods, sizeof(pz_installed_mod));
    pz_mod_manager_list(manager, &mods);

    for (size_t i = 0; i < mods.length; ++i) {
        const pz_installed_mod* mod = pz_vec_at_const(&mods, i);
        builder line;
        build_init(&line);
        build_text(&line, mod->name.data, mod->name.length);
        build_cstr(&line, " [");
        build_text(&line, mod->folder.data, mod->folder.length);
        build_cstr(&line, "] id=");
        build_u64(&line, mod->workshop_id);
        if (mod->imported) build_cstr(&line, " [imported]");
        if (mod->locked) {
            build_cstr(&line, " [locked]");
        } else if (pz_mod_manager_server_locked(manager, mod->workshop_id)) {
            build_cstr(&line, " [target lock]");
        }
        if (log && log->callback) pz_log_emit_str(log, &line.text);
        pz_str_free(&line.text);
    }

    builder message;
    build_init(&message);
    build_u64(&message, (uint64_t)mods.length);
    build_cstr(&message, " mod(s)");
    pz_mod_manager_free_mods(&mods);
    return build_finish(&message, 1);
}

/* ---------------------------------------------------------------- dispatch */

pz_mm_result pz_command_run(pz_mod_manager* manager, const char* line,
                            size_t length, const pz_log* log) {
    if (!manager) return result_cstr(0, "Manager unavailable");
    if (!line) length = 0;

    const text_slice input = {line, length};
    text_slice arg;
    const text_slice command = split_first(input, &arg);

    if (slice_equals(command, "add")) return add_workshop_input(manager, arg, log);

    if (slice_equals(command, "import")) {
        if (arg.length == 0) return pz_mod_manager_import(manager, NULL, 0, log);
        pz_vec ids;
        parse_ids(arg, &ids);
        if (ids.length == 0) {
            pz_vec_free(&ids);
            return result_cstr(0, "expected Workshop URLs or IDs");
        }
        pz_mm_result result = pz_mod_manager_import(
            manager, (const uint64_t*)ids.data, ids.length, log);
        pz_vec_free(&ids);
        return result;
    }

    if (slice_equals(command, "remove")) return remove_command(manager, arg, log);

    if (slice_equals(command, "check")) return check_command(manager, log);

    if (slice_equals(command, "self-update"))
        return stage_self_update(manager, log);

    if (slice_equals(command, "targets")) return targets_command(manager, log);

    if (slice_equals(command, "target-lock"))
        return target_lock_command(manager, arg, log);

    if (slice_equals(command, "set-enabled"))
        return set_enabled_command(manager, arg, log);

    if (slice_equals(command, "enable"))
        return enable_command(manager, arg, 1, log);
    if (slice_equals(command, "disable"))
        return enable_command(manager, arg, 0, log);

    if (slice_equals(command, "share")) return share_command(manager, arg, log);

    if (slice_equals(command, "lock"))
        return lock_command(manager, arg, 1, log);
    if (slice_equals(command, "unlock"))
        return lock_command(manager, arg, 0, log);

    if (slice_equals(command, "update"))
        return update_command(manager, arg, log);

    if (slice_equals(command, "list")) return list_command(manager, log);

    if (slice_equals(command, "config")) {
        text_slice value;
        const text_slice key = split_first(arg, &value);
        return config_command(manager, key, value);
    }

    /* A pasted Workshop URL, numeric id, or whitespace-separated list with no
     * command prefix means add. */
    pz_vec ids;
    parse_ids(input, &ids);
    if (ids.length != 0) {
        pz_mm_result result = add_workshop_ids(manager, &ids, log);
        pz_vec_free(&ids);
        return result;
    }
    pz_vec_free(&ids);
    return result_prefix(0, "unknown command: ", command);
}
