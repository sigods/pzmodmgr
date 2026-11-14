#define _POSIX_C_SOURCE 200809L

#include "core/net_c.h"

#include "core/process_c.h"

#include <string.h>

static int ends_with(const char* text, size_t length, const char* suffix) {
    const size_t suffix_length = strlen(suffix);

    if (length < suffix_length) return 0;
    return memcmp(text + length - suffix_length, suffix, suffix_length) == 0;
}

/* Reproduces the old error_code::message() text: the OS message when a raw
 * system code is available, otherwise the portable fs code name. */
static void format_fs_error(const pz_fs_error* error, pz_str* detail) {
    pz_error mapped;

    pz_error_clear(&mapped);
    if (error->system_code != 0)
        pz_error_set_system(&mapped, error->system_code);
    else
        pz_error_set(&mapped, (int)error->code);
    pz_error_format(&mapped, detail);
}

/* First non-empty line of a command's output, capped at 300 bytes. */
static void one_line(pz_str* detail, const pz_str* output, const char* fallback) {
    size_t length = output->length;
    size_t i;

    while (length && (output->data[length - 1] == '\n' || output->data[length - 1] == '\r'))
        --length;
    for (i = 0; i < length; ++i) {
        if (output->data[i] == '\n') {
            length = i;
            break;
        }
    }
    if (length > 300) length = 300;
    if (length)
        pz_str_assign(detail, output->data, length);
    else
        pz_str_assign_cstr(detail, fallback);
}

int pz_net_get(const char* url, size_t length, pz_str* response, pz_str* detail) {
    pz_str command;
    int exit_code = -1;
    int ok;

    if (response) pz_str_clear(response);
    if (detail) pz_str_clear(detail);
    if (!response || !detail || (!url && length)) return 0;

    pz_str_init(&command);
    ok = pz_str_append_cstr(&command,
                            "curl -sS --fail --location --connect-timeout 10 --max-time 30 "
                            "-H \"Accept: application/vnd.github+json\" "
                            "-H \"User-Agent: pzmodmgr\" ") &&
         pz_process_quote(&command, url, length) &&
         pz_str_append_cstr(&command, " 2>&1") &&
         pz_process_capture(command.data, command.length, response, NULL, &exit_code) &&
         exit_code == 0;
    pz_str_free(&command);
    if (!ok) one_line(detail, response, "could not reach the update server");
    return ok;
}

int pz_net_post(const char* url, size_t url_length, const char* body,
                size_t body_length, pz_str* response) {
    pz_str command;
    int exit_code = -1;
    int ok;

    if (response) pz_str_clear(response);
    if (!response || (!url && url_length) || (!body && body_length)) return 0;

    pz_str_init(&command);
    ok = pz_str_append_cstr(&command,
                            "curl -sS --fail --connect-timeout 5 --max-time 20 "
                            "-X POST --data ") &&
         pz_process_quote(&command, body, body_length) &&
         pz_str_append_char(&command, ' ') &&
         pz_process_quote(&command, url, url_length) &&
         pz_str_append_cstr(&command, " 2>&1") &&
         pz_process_capture(command.data, command.length, response, NULL, &exit_code) &&
         exit_code == 0;
    pz_str_free(&command);
    return ok;
}

int pz_net_download(const char* url, size_t length, const pz_path* dest,
                    pz_str* detail) {
    pz_path parent;
    pz_path temp;
    pz_fs_error error;
    pz_str command;
    pz_str output;
    uint64_t size = 0;
    int exit_code = -1;
    int ok = 0;

    if (detail) pz_str_clear(detail);
    if (!detail || (!url && length) || !dest || dest->text.length == 0) return 0;

    pz_path_init(&parent);
    pz_path_init(&temp);
    pz_str_init(&command);
    pz_str_init(&output);
    pz_fs_error_clear(&error);

    if (pz_path_parent(dest, &parent) && parent.text.length &&
        !pz_fs_create_directories(&parent, &error)) {
        format_fs_error(&error, detail);
        goto done;
    }
    if (!pz_path_assign(&temp, dest->text.data, dest->text.length) ||
        !pz_str_append_cstr(&temp.text, ".part")) {
        pz_str_assign_cstr(detail, "out of memory");
        goto done;
    }
    pz_fs_remove(&temp, &error);   /* a stale partial download is expected */
    pz_fs_error_clear(&error);

    if (!pz_str_append_cstr(&command,
                            "curl -sS --fail --location --connect-timeout 10 --max-time 600 -o ") ||
        !pz_process_quote(&command, temp.text.data, temp.text.length) ||
        !pz_str_append_char(&command, ' ') ||
        !pz_process_quote(&command, url, length) ||
        !pz_str_append_cstr(&command, " 2>&1")) {
        pz_str_assign_cstr(detail, "out of memory");
        goto done;
    }
    if (!pz_process_capture(command.data, command.length, &output, NULL, &exit_code) ||
        exit_code != 0) {
        pz_fs_remove(&temp, &error);
        one_line(detail, &output, "download failed");
        goto done;
    }
    if (!pz_fs_file_size(&temp, &size, &error) || size == 0) {
        pz_fs_remove(&temp, &error);
        pz_str_assign_cstr(detail, "downloaded file is empty");
        goto done;
    }
    if (!pz_fs_replace_file(&temp, dest, &error)) {
        pz_fs_remove(&temp, &error);
        format_fs_error(&error, detail);
        goto done;
    }
    ok = 1;

done:
    pz_str_free(&output);
    pz_str_free(&command);
    pz_path_free(&temp);
    pz_path_free(&parent);
    return ok;
}

int pz_net_extract(const pz_path* archive, const pz_path* dest, pz_str* detail) {
    pz_str name;
    pz_str command;
    pz_str output;
    pz_fs_error error;
    int exit_code = -1;
    int ok = 0;

    if (detail) pz_str_clear(detail);
    if (!detail || !archive || !dest) return 0;

    pz_fs_error_clear(&error);
    if (!pz_fs_create_directories(dest, &error)) {
        format_fs_error(&error, detail);
        return 0;
    }

    pz_str_init(&name);
    pz_str_init(&command);
    pz_str_init(&output);
    if (!pz_path_filename(archive, &name)) {
        pz_str_assign_cstr(detail, "unsupported update archive");
        goto done;
    }

#ifdef _WIN32
    if (ends_with(name.data, name.length, ".zip"))
        pz_str_assign_cstr(&command, "tar.exe -xf ");
    else if (ends_with(name.data, name.length, ".tar.gz") ||
             ends_with(name.data, name.length, ".tgz"))
        pz_str_assign_cstr(&command, "tar.exe -xzf ");
    else {
        pz_str_assign_cstr(detail, "unsupported update archive");
        goto done;
    }
#else
    if (ends_with(name.data, name.length, ".zip"))
        pz_str_assign_cstr(&command, "unzip -q ");
    else if (ends_with(name.data, name.length, ".tar.gz") ||
             ends_with(name.data, name.length, ".tgz"))
        pz_str_assign_cstr(&command, "tar -xzf ");
    else {
        pz_str_assign_cstr(detail, "unsupported update archive");
        goto done;
    }
#endif

    if (!pz_process_quote(&command, archive->text.data, archive->text.length) ||
        !pz_str_append_cstr(&command, " -C ") ||
        !pz_process_quote(&command, dest->text.data, dest->text.length) ||
        !pz_str_append_cstr(&command, " 2>&1") ||
        !pz_process_capture(command.data, command.length, &output, NULL, &exit_code)) {
        pz_str_assign_cstr(detail, "out of memory");
        goto done;
    }
    if (exit_code == 0) {
        ok = 1;
    } else {
        one_line(detail, &output, "could not extract the update archive");
    }

done:
    pz_str_free(&output);
    pz_str_free(&command);
    pz_str_free(&name);
    return ok;
}
