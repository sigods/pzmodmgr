#include "core/updater_json_c.h"

#include "core/json_c.h"

#include <string.h>

static int is_trim_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int assign_trimmed(pz_str* out, const char* data, size_t length) {
    size_t first = 0;
    size_t last = length;

    while (first < last && is_trim_space(data[first])) ++first;
    while (last > first && is_trim_space(data[last - 1])) --last;
    return pz_str_assign(out, data + first, last - first);
}

void pz_app_update_init(pz_app_update* update) {
    if (!update) return;
    memset(update, 0, sizeof(*update));
    pz_str_init(&update->current_version);
    pz_str_init(&update->latest_version);
    pz_str_init(&update->asset_name);
    pz_str_init(&update->asset_url);
    pz_str_init(&update->asset_digest);
    pz_str_init(&update->message);
}

void pz_app_update_free(pz_app_update* update) {
    if (!update) return;
    pz_str_free(&update->current_version);
    pz_str_free(&update->latest_version);
    pz_str_free(&update->asset_name);
    pz_str_free(&update->asset_url);
    pz_str_free(&update->asset_digest);
    pz_str_free(&update->message);
}

static void version_parts(const char* version, size_t length, int parts[4]) {
    int number = 0;
    int have_digits = 0;
    size_t count = 0;
    size_t i;

    for (i = 0; i < length; ++i) {
        const char c = version[i];
        if (c >= '0' && c <= '9') {
            have_digits = 1;
            number = number * 10 + (c - '0');
        } else if (have_digits) {
            parts[count++] = number;
            number = 0;
            have_digits = 0;
            if (count == 4) break;
        }
    }
    if (have_digits && count < 4) parts[count++] = number;
    while (count < 4) parts[count++] = 0;
}

int pz_app_version_newer(const char* latest, size_t latest_length,
                         const char* current, size_t current_length) {
    int left[4];
    int right[4];
    size_t i;

    version_parts(latest, latest_length, left);
    version_parts(current, current_length, right);
    for (i = 0; i < 4; ++i) {
        if (left[i] != right[i]) return left[i] > right[i];
    }
    return 0;
}

static int ends_with(const char* value, size_t length, const char* suffix) {
    const size_t suffix_length = strlen(suffix);
    return length >= suffix_length &&
           memcmp(value + length - suffix_length, suffix, suffix_length) == 0;
}

static int platform_asset(const char* name, size_t length, const char* platform) {
    if (platform && strcmp(platform, "win") == 0) {
        return strstr(name, "win64") != NULL &&
               strstr(name, "setup") == NULL && ends_with(name, length, ".zip");
    }
    return strstr(name, "linux") != NULL && ends_with(name, length, ".tar.gz");
}

static int append_message(pz_str* message, const char* first,
                          const pz_str* second, const char* third,
                          const pz_str* fourth, const char* fifth) {
    if (!pz_str_assign_cstr(message, first)) return 0;
    if (second && !pz_str_append(message, second->data, second->length)) return 0;
    if (third && !pz_str_append_cstr(message, third)) return 0;
    if (fourth && !pz_str_append(message, fourth->data, fourth->length)) return 0;
    if (fifth && !pz_str_append_cstr(message, fifth)) return 0;
    return 1;
}

int pz_parse_latest_app_release(const char* json, size_t length,
                                const char* current_version,
                                size_t current_version_length,
                                const char* platform, size_t platform_length,
                                pz_app_update* out) {
    pz_str tag;
    size_t assets_pos;
    int found_asset = 0;
    size_t i;

    (void)platform_length;
    if ((!json && length != 0) || !current_version || !platform || !out)
        return 0;

    pz_app_update_init(out);
    if (!pz_str_assign(&out->current_version, current_version,
                       current_version_length))
        goto fail;

    pz_str_init(&tag);
    if (!pz_json_string_field(json, length, "tag_name", 0, &tag, NULL) ||
        tag.length == 0) {
        pz_str_free(&tag);
        if (!pz_str_assign_cstr(&out->message,
                                "GitHub did not return a release tag"))
            goto fail;
        return 1;
    }

    {
        const char* tag_data = tag.data;
        size_t tag_length = tag.length;
        if (tag_data[0] == 'v' || tag_data[0] == 'V') {
            ++tag_data;
            --tag_length;
        }
        if (!assign_trimmed(&out->latest_version, tag_data, tag_length)) {
            pz_str_free(&tag);
            goto fail;
        }
    }
    pz_str_free(&tag);

    out->available = pz_app_version_newer(
        pz_str_cstr(&out->latest_version), out->latest_version.length,
        current_version, current_version_length);

    if (pz_json_find_key(json, length, "assets", 0, &assets_pos)) {
        size_t array = assets_pos;
        while (array < length && json[array] != '[') ++array;
        if (array < length && json[array] == '[') {
            int depth = 0;
            size_t object_start = 0;
            int have_object = 0;

            for (i = array + 1; i < length; ++i) {
                const char c = json[i];
                if (c == '"') {
                    if (!pz_json_skip_string(json, length, &i)) break;
                    --i;
                } else if (c == '{') {
                    if (depth == 0) {
                        object_start = i;
                        have_object = 1;
                    }
                    ++depth;
                } else if (c == '}') {
                    if (depth > 0) --depth;
                    if (depth == 0 && have_object) {
                        const size_t object_end = i + 1;
                        pz_str name;
                        pz_str url;
                        pz_str digest;
                        uint64_t size = 0;

                        pz_str_init(&name);
                        pz_str_init(&url);
                        pz_str_init(&digest);
                        if (pz_json_string_field(json, object_end, "name",
                                                 object_start, &name, NULL) &&
                            pz_json_string_field(json, object_end,
                                                 "browser_download_url",
                                                 object_start, &url, NULL) &&
                            platform_asset(pz_str_cstr(&name), name.length,
                                           platform)) {
                            pz_json_uint_field(json, object_end, "size",
                                               object_start, &size);
                            pz_json_string_field(json, object_end, "digest",
                                                 object_start, &digest, NULL);
                            if (!found_asset) {
                                if (!pz_str_assign(&out->asset_name, name.data,
                                                   name.length) ||
                                    !pz_str_assign(&out->asset_url, url.data,
                                                   url.length) ||
                                    !pz_str_assign(&out->asset_digest,
                                                   digest.data, digest.length)) {
                                    pz_str_free(&name);
                                    pz_str_free(&url);
                                    pz_str_free(&digest);
                                    goto fail;
                                }
                                out->asset_size = size;
                                found_asset = 1;
                            }
                        }
                        pz_str_free(&name);
                        pz_str_free(&url);
                        pz_str_free(&digest);
                        have_object = 0;
                    }
                } else if (c == ']' && depth == 0) {
                    break;
                }
            }
        }
    }

    if (!out->available) {
        out->ok = 1;
        if (!pz_str_assign_cstr(&out->message, "PZ Mod Manager ") ||
            !pz_str_append(&out->message, out->current_version.data,
                           out->current_version.length) ||
            !pz_str_append_cstr(&out->message,
                                " is up to date (latest release ") ||
            !pz_str_append(&out->message, out->latest_version.data,
                           out->latest_version.length) ||
            !pz_str_append_cstr(&out->message, ")."))
            goto fail;
        return 1;
    }

    if (!found_asset) {
        if (!pz_str_assign_cstr(&out->message, "Release ") ||
            !pz_str_append(&out->message, out->latest_version.data,
                           out->latest_version.length) ||
            !pz_str_append_cstr(&out->message, " has no compatible ") ||
            !pz_str_append_cstr(&out->message, platform) ||
            !pz_str_append_cstr(&out->message, " archive"))
            goto fail;
        return 1;
    }

    out->ok = 1;
    if (!append_message(&out->message, "PZ Mod Manager ",
                        &out->latest_version, " is available.", NULL, NULL))
        goto fail;
    return 1;

fail:
    pz_app_update_free(out);
    return 0;
}
