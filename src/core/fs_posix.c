#define _POSIX_C_SOURCE 200809L

#include "core/fs_c_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct pz_fs_file {
    FILE* stream;
    pz_fs_code error;
};

static pz_fs_code code_for_errno(int value) {
    switch (value) {
    case ENOENT:
    case ENOTDIR:
        return PZ_FS_NOT_FOUND;
    case EEXIST:
        return PZ_FS_EXISTS;
    case EACCES:
    case EPERM:
    case EROFS:
        return PZ_FS_ACCESS;
    case EBUSY:
    case ETXTBSY:
        return PZ_FS_BUSY;
    case EXDEV:
        return PZ_FS_CROSS_DEVICE;
    case ENOTEMPTY:
        return PZ_FS_NOT_EMPTY;
    case EINVAL:
    case ELOOP:
    case ENOSYS:
        return PZ_FS_INVALID;
    case ENAMETOOLONG:
        return PZ_FS_TOO_LONG;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return PZ_FS_NO_SPACE;
    case EIO:
        return PZ_FS_IO;
    default:
        return PZ_FS_SYSTEM;
    }
}

static void fail_errno(pz_fs_error* error, int value) {
    pz_fs_error_set(error, code_for_errno(value), value);
}

static const char* path_text(const pz_path* path) {
    return (path && path->text.data) ? path->text.data : "";
}

int pz_fs_exists(const pz_path* path, int* exists, pz_fs_error* error) {
    struct stat info;

    if (!path || !exists) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (lstat(path_text(path), &info) == 0) {
        *exists = 1;
        return 1;
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        *exists = 0;
        pz_fs_error_clear(error);
        return 1;
    }
    fail_errno(error, errno);
    return 0;
}

int pz_fs_kind_of(const pz_path* path, pz_fs_kind* kind, pz_fs_error* error) {
    struct stat info;

    if (!path || !kind) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (lstat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    if (S_ISLNK(info.st_mode))
        *kind = PZ_FS_LINK;
    else if (S_ISDIR(info.st_mode))
        *kind = PZ_FS_DIR;
    else if (S_ISREG(info.st_mode))
        *kind = PZ_FS_FILE;
    else
        *kind = PZ_FS_UNKNOWN;
    return 1;
}

int pz_fs_file_size(const pz_path* path, uint64_t* size, pz_fs_error* error) {
    struct stat info;

    if (!path || !size) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (stat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    *size = (uint64_t)info.st_size;
    return 1;
}

int pz_fs_is_empty(const pz_path* path, int* empty, pz_fs_error* error) {
    struct stat info;

    if (!path || !empty) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (stat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    if (S_ISREG(info.st_mode)) {
        *empty = info.st_size == 0;
        return 1;
    }
    if (S_ISDIR(info.st_mode)) {
        DIR* dir = opendir(path_text(path));
        struct dirent* entry;
        int found = 0;

        if (!dir) {
            fail_errno(error, errno);
            return 0;
        }
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            found = 1;
            break;
        }
        closedir(dir);
        *empty = !found;
        return 1;
    }
    *empty = 0;
    return 1;
}

int pz_fs_is_executable(const pz_path* path, int* executable,
                        pz_fs_error* error) {
    struct stat info;

    if (!path || !executable) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (stat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    *executable = S_ISREG(info.st_mode) && access(path_text(path), X_OK) == 0;
    return 1;
}

int pz_fs_create_directories(const pz_path* path, pz_fs_error* error) {
    const char* text;
    size_t length;
    size_t i;
    char* scratch;

    if (!path || !path->text.data || path->text.length == 0) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    text = path->text.data;
    length = path->text.length;
    scratch = (char*)malloc(length + 1);
    if (!scratch) {
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    memcpy(scratch, text, length + 1);

    for (i = 1; i <= length; ++i) {
        if (i != length && scratch[i] != '/') continue;
        {
            const char saved = scratch[i];
            struct stat info;

            scratch[i] = '\0';
            if (scratch[0] == '\0') {
                scratch[i] = saved;
                continue;
            }
            if (mkdir(scratch, 0777) != 0 && errno != EEXIST) {
                const int value = errno;
                scratch[i] = saved;
                free(scratch);
                fail_errno(error, value);
                return 0;
            }
            if (stat(scratch, &info) != 0 || !S_ISDIR(info.st_mode)) {
                scratch[i] = saved;
                free(scratch);
                pz_fs_error_set(error, PZ_FS_INVALID, 0);
                return 0;
            }
            scratch[i] = saved;
        }
    }
    free(scratch);
    return 1;
}

int pz_fs_remove(const pz_path* path, pz_fs_error* error) {
    struct stat info;

    if (!path || !path->text.data) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (lstat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    if (S_ISDIR(info.st_mode)) {
        if (rmdir(path_text(path)) != 0) {
            fail_errno(error, errno);
            return 0;
        }
        return 1;
    }
    if (unlink(path_text(path)) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    return 1;
}

int pz_fs_rename(const pz_path* from, const pz_path* to, pz_fs_error* error) {
    if (!from || !to) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (rename(path_text(from), path_text(to)) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    return 1;
}

int pz_fs_copy_file(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error) {
    struct stat source;
    struct stat target;
    char buffer[65536];
    int input = -1;
    int output = -1;
    int created = 0;
    int ok = 0;

    if (!from || !to || !from->text.data || !to->text.data) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (stat(path_text(from), &source) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    if (S_ISDIR(source.st_mode)) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }

    if (lstat(path_text(to), &target) == 0) {
        if (S_ISDIR(target.st_mode)) {
            pz_fs_error_set(error, PZ_FS_INVALID, 0);
            return 0;
        }
        if (!(flags & PZ_FS_COPY_OVERWRITE)) {
            pz_fs_error_set(error, PZ_FS_EXISTS, 0);
            return 0;
        }
        if (S_ISLNK(target.st_mode)) {
            if (unlink(path_text(to)) != 0) {
                fail_errno(error, errno);
                return 0;
            }
        }
    }

    input = open(path_text(from), O_RDONLY);
    if (input < 0) {
        fail_errno(error, errno);
        return 0;
    }
    output = open(path_text(to),
                  O_WRONLY | O_CREAT |
                      ((flags & PZ_FS_COPY_OVERWRITE) ? O_TRUNC : O_EXCL),
                  0600);
    if (output < 0) {
        const int value = errno;
        close(input);
        fail_errno(error, value);
        return 0;
    }
    created = 1;

    for (;;) {
        const ssize_t got = read(input, buffer, sizeof buffer);
        ssize_t written = 0;

        if (got < 0) {
            if (errno == EINTR) continue;
            fail_errno(error, errno);
            goto done;
        }
        if (got == 0) break;
        while (written < got) {
            const ssize_t put = write(output, buffer + written,
                                      (size_t)(got - written));
            if (put < 0) {
                if (errno == EINTR) continue;
                fail_errno(error, errno);
                goto done;
            }
            written += put;
        }
    }

    if (fchmod(output, (source.st_mode & 0111) ? 0777 : 0666) != 0) {
        fail_errno(error, errno);
        goto done;
    }
    ok = 1;

done:
    if (close(input) != 0 && ok) {
        fail_errno(error, errno);
        ok = 0;
    }
    if (close(output) != 0 && ok) {
        fail_errno(error, errno);
        ok = 0;
    }
    if (!ok && created) unlink(path_text(to));
    return ok;
}

int pz_fs_copy_link(const pz_path* from, const pz_path* to, unsigned flags,
                    pz_fs_error* error) {
    char* target = NULL;
    size_t capacity = 256;
    ssize_t length;
    struct stat existing;

    if (!from || !to) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }

    for (;;) {
        char* grown = (char*)realloc(target, capacity);
        if (!grown) {
            free(target);
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        target = grown;
        length = readlink(path_text(from), target, capacity - 1);
        if (length < 0) {
            const int value = errno;
            free(target);
            fail_errno(error, value);
            return 0;
        }
        if ((size_t)length < capacity - 1) break;
        capacity *= 2;
        if (capacity > 65536) {
            free(target);
            pz_fs_error_set(error, PZ_FS_TOO_LONG, 0);
            return 0;
        }
    }
    target[length] = '\0';

    if (lstat(path_text(to), &existing) == 0) {
        if (!(flags & PZ_FS_COPY_OVERWRITE)) {
            free(target);
            pz_fs_error_set(error, PZ_FS_EXISTS, 0);
            return 0;
        }
        if (unlink(path_text(to)) != 0) {
            const int value = errno;
            free(target);
            fail_errno(error, value);
            return 0;
        }
    }

    if (symlink(target, path_text(to)) != 0) {
        const int value = errno;
        free(target);
        fail_errno(error, value);
        return 0;
    }
    free(target);
    return 1;
}

int pz_fs_replace_file(const pz_path* from, const pz_path* to,
                       pz_fs_error* error) {
    if (!from || !to) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (rename(path_text(from), path_text(to)) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    return 1;
}

int pz_fs_set_executable(const pz_path* path, int enabled, pz_fs_error* error) {
    struct stat info;

    if (!path || !path->text.data) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (stat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    {
        mode_t mode = info.st_mode & 07777;
        if (enabled)
            mode |= 0111;
        else
            mode &= (mode_t)~0111;
        if (chmod(path_text(path), mode) != 0) {
            fail_errno(error, errno);
            return 0;
        }
    }
    return 1;
}

int pz_fs_list_dir(const pz_path* dir, pz_vec* out, pz_fs_error* error) {
    DIR* handle;
    struct dirent* entry;
    pz_vec list;

    if (!dir || !dir->text.data || !out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    handle = opendir(path_text(dir));
    if (!handle) {
        fail_errno(error, errno);
        return 0;
    }

    pz_vec_init(&list, sizeof(pz_fs_entry));
    while ((entry = readdir(handle)) != NULL) {
        pz_path child;
        struct stat info;
        pz_fs_kind kind = PZ_FS_UNKNOWN;
        int stat_failed;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        pz_path_init(&child);
        if (!pz_path_join(&child, dir, entry->d_name, strlen(entry->d_name))) {
            pz_path_free(&child);
            closedir(handle);
            pz_fs_entries_free(&list);
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        stat_failed = lstat(path_text(&child), &info) != 0;
        if (!stat_failed) {
            if (S_ISLNK(info.st_mode))
                kind = PZ_FS_LINK;
            else if (S_ISDIR(info.st_mode))
                kind = PZ_FS_DIR;
            else if (S_ISREG(info.st_mode))
                kind = PZ_FS_FILE;
        }
        pz_path_free(&child);

        if (!pz_fs_entries_add(&list, entry->d_name, strlen(entry->d_name), kind)) {
            closedir(handle);
            pz_fs_entries_free(&list);
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        if (stat_failed) {
            pz_fs_entry* added = (pz_fs_entry*)pz_vec_at(&list, list.length - 1);
            added->error = 1;
        }
    }
    closedir(handle);

    pz_fs_entries_sort(&list);
    pz_fs_entries_free(out);
    *out = list;
    return 1;
}

int pz_fs_last_write_time(const pz_path* path, int64_t* seconds,
                          pz_fs_error* error) {
    struct stat info;

    if (!path || !seconds) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (stat(path_text(path), &info) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    *seconds = (int64_t)info.st_mtime;
    return 1;
}

int pz_fs_set_last_write_time(const pz_path* path, int64_t seconds,
                              pz_fs_error* error) {
    struct timespec times[2];

    if (!path) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = (time_t)seconds;
    times[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, path_text(path), times, 0) != 0) {
        fail_errno(error, errno);
        return 0;
    }
    return 1;
}

int pz_fs_executable_path(pz_path* out, pz_fs_error* error) {
    size_t capacity = 256;
    char* buffer = NULL;
    ssize_t length;

    if (!out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    for (;;) {
        char* grown = (char*)realloc(buffer, capacity);
        if (!grown) {
            free(buffer);
            pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
            return 0;
        }
        buffer = grown;
        length = readlink("/proc/self/exe", buffer, capacity - 1);
        if (length < 0) {
            const int value = errno;
            free(buffer);
            fail_errno(error, value);
            return 0;
        }
        if ((size_t)length < capacity - 1) break;
        capacity *= 2;
        if (capacity > 65536) {
            free(buffer);
            pz_fs_error_set(error, PZ_FS_TOO_LONG, 0);
            return 0;
        }
    }
    buffer[length] = '\0';
    {
        const int ok = pz_path_assign(out, buffer, (size_t)length);
        free(buffer);
        if (!ok) pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return ok;
    }
}

int pz_fs_current_dir(pz_path* out, pz_fs_error* error) {
    size_t capacity = 256;
    char* buffer = (char*)malloc(capacity);

    if (!out || !buffer) {
        free(buffer);
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    for (;;) {
        if (getcwd(buffer, capacity) != NULL) break;
        if (errno != ERANGE) {
            const int value = errno;
            free(buffer);
            fail_errno(error, value);
            return 0;
        }
        capacity *= 2;
        {
            char* grown = (char*)realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
                return 0;
            }
            buffer = grown;
        }
    }
    {
        const int ok = pz_path_assign_cstr(out, buffer);
        free(buffer);
        if (!ok) pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return ok;
    }
}

int pz_fs_temp_dir(pz_path* out, pz_fs_error* error) {
    const char* value = getenv("TMPDIR");

    if (!out) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (!value || value[0] == '\0') value = "/tmp";
    if (!pz_path_assign_cstr(out, value)) {
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    return 1;
}

static int open_file(const pz_path* path, const char* mode, pz_fs_file** file,
                     pz_fs_error* error) {
    pz_fs_file* handle;
    FILE* stream;

    if (!path || !path->text.data || !file) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    stream = fopen(path_text(path), mode);
    if (!stream) {
        fail_errno(error, errno);
        return 0;
    }
    handle = (pz_fs_file*)malloc(sizeof *handle);
    if (!handle) {
        fclose(stream);
        pz_fs_error_set(error, PZ_FS_NO_SPACE, 0);
        return 0;
    }
    handle->stream = stream;
    handle->error = PZ_FS_OK;
    *file = handle;
    return 1;
}

int pz_fs_open_read(const pz_path* path, pz_fs_file** file, pz_fs_error* error) {
    return open_file(path, "rb", file, error);
}

int pz_fs_open_write(const pz_path* path, pz_fs_file** file,
                     pz_fs_error* error) {
    return open_file(path, "wb", file, error);
}

int pz_fs_open_append(const pz_path* path, pz_fs_file** file,
                      pz_fs_error* error) {
    return open_file(path, "ab", file, error);
}

size_t pz_fs_read(pz_fs_file* file, void* buffer, size_t size) {
    size_t got;

    if (!file || !buffer || size == 0) return 0;
    got = fread(buffer, 1, size, file->stream);
    if (got < size && ferror(file->stream)) file->error = PZ_FS_IO;
    return got;
}

size_t pz_fs_write(pz_fs_file* file, const void* buffer, size_t size) {
    size_t put;

    if (!file || (!buffer && size != 0)) return 0;
    put = fwrite(buffer, 1, size, file->stream);
    if (put < size) {
        file->error = errno != 0 ? code_for_errno(errno) : PZ_FS_IO;
        if (file->error == PZ_FS_OK) file->error = PZ_FS_IO;
    }
    return put;
}

pz_fs_code pz_fs_file_error(const pz_fs_file* file) {
    return file ? file->error : PZ_FS_INVALID;
}

int pz_fs_close(pz_fs_file* file, pz_fs_error* error) {
    int result = 1;

    if (!file) {
        pz_fs_error_set(error, PZ_FS_INVALID, 0);
        return 0;
    }
    if (file->error != PZ_FS_OK) {
        pz_fs_error_set(error, file->error, errno);
        result = 0;
    }
    if (fclose(file->stream) != 0) {
        if (result) fail_errno(error, errno);
        result = 0;
    }
    free(file);
    return result;
}
