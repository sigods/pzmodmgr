#define _POSIX_C_SOURCE 200809L

#include "core/fs_c.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures = 0;

static void check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int str_equals(const pz_str* value, const char* expected) {
    return value && value->length == strlen(expected) &&
           (value->length == 0 ||
            memcmp(value->data, expected, value->length) == 0);
}

static void path_join(pz_path* out, const char* base, const char* leaf) {
    pz_path root;

    pz_path_init(&root);
    pz_path_assign_cstr(&root, base);
    pz_path_join(out, &root, leaf, strlen(leaf));
    pz_path_free(&root);
}

static int write_text(const pz_path* path, const char* text) {
    pz_fs_file* file = NULL;
    pz_fs_error error;
    size_t put;

    pz_fs_error_clear(&error);
    if (!pz_fs_open_write(path, &file, &error)) return 0;
    put = pz_fs_write(file, text, strlen(text));
    if (!pz_fs_close(file, &error)) return 0;
    return put == strlen(text);
}

static void test_paths(void) {
    pz_path path;
    pz_path out;
    pz_str name;
    pz_fs_error error;
    char expected[128];
    const char sep = pz_path_separator();

    pz_path_init(&path);
    pz_path_init(&out);
    pz_str_init(&name);

    check(sep == '/' || sep == '\\', "path separator is a native separator");

    pz_path_assign_cstr(&path, "a/./b/../c");
    snprintf(expected, sizeof expected, "a%cc", sep);
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, expected),
          "lexically_normal collapses dot segments");
    pz_path_assign_cstr(&path, "a//b///c/");
    snprintf(expected, sizeof expected, "a%cb%cc", sep, sep);
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, expected),
          "lexically_normal collapses separators and trailing separator");
    pz_path_assign_cstr(&path, "../../x");
    snprintf(expected, sizeof expected, "..%c..%cx", sep, sep);
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, expected),
          "lexically_normal keeps leading parent refs");
    pz_path_assign_cstr(&path, "/a/../../b");
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, "/b"),
          "lexically_normal stops at the root");
    pz_path_assign_cstr(&path, "a/..");
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, "."),
          "lexically_normal returns dot for an empty relative path");
    pz_path_assign_cstr(&path, ".");
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, "."),
          "lexically_normal keeps a lone dot");

#ifdef _WIN32
    pz_path_assign_cstr(&path, "/abs");
    check(!pz_path_is_absolute(&path), "a root-relative path is not absolute");
    pz_path_assign_cstr(&path, "C:\\abs");
    check(pz_path_is_absolute(&path), "a drive path is absolute");
#else
    pz_path_assign_cstr(&path, "/abs");
    check(pz_path_is_absolute(&path), "absolute path detected");
#endif
    pz_path_assign_cstr(&path, "rel");
    check(!pz_path_is_absolute(&path), "relative path detected");

    {
        pz_path base;
        pz_path_init(&base);
        pz_path_assign_cstr(&base, "base");
        snprintf(expected, sizeof expected, "base%cleaf", sep);
        check(pz_path_join(&out, &base, "leaf", 4) && str_equals(&out.text, expected),
              "join adds a separator");
        check(pz_path_join(&out, &base, "/absolute", 9) &&
                  str_equals(&out.text, "/absolute"),
              "join lets an absolute leaf replace the base");
        check(pz_path_join(&out, &base, "", 0) && str_equals(&out.text, "base"),
              "join with an empty leaf leaves the base alone");
#ifdef _WIN32
        check(pz_path_join(&out, &base, "C:\\absolute", 11) &&
                  str_equals(&out.text, "C:\\absolute"),
              "join lets a drive path replace the base");
#endif
        pz_path_free(&base);
    }

    pz_path_assign_cstr(&path, "a/b/c");
    check(pz_path_parent(&path, &out) && str_equals(&out.text, "a/b"),
          "parent drops the last component");
    check(pz_path_filename(&path, &name) && str_equals(&name, "c"),
          "filename returns the last component");
    pz_path_assign_cstr(&path, "single");
    check(pz_path_parent(&path, &out) && str_equals(&out.text, ""),
          "parent of a single component is empty");
#ifdef _WIN32
    pz_path_assign_cstr(&path, "C:\\");
    check(pz_path_lexically_normal(&path, &out) && str_equals(&out.text, "C:\\"),
          "normalization keeps a drive root");
    check(pz_path_parent(&path, &out) && str_equals(&out.text, "C:\\"),
          "parent of a drive root is itself");
    pz_path_assign_cstr(&path, "C:");
    check(pz_path_filename(&path, &name) && str_equals(&name, ""),
          "filename of a drive-only path is empty");
    check(pz_path_parent(&path, &out) && str_equals(&out.text, "C:"),
          "parent of a drive-relative root is the drive");
#endif

    pz_fs_error_clear(&error);
    check(pz_fs_error_format(&error, &name) && str_equals(&name, "ok"),
          "formats a clear error");
    error.code = PZ_FS_ACCESS;
    error.system_code = 13;
    check(pz_fs_error_format(&error, &name) &&
              str_equals(&name, "access denied (system 13)"),
          "formats a portable error with its system code");

    pz_str_free(&name);
    pz_path_free(&out);
    pz_path_free(&path);
}

static void test_directory(const char* root_text) {
    pz_path root;
    pz_path sub;
    pz_path file;
    pz_path missing;
    pz_vec entries;
    pz_fs_error error;
    int exists = 0;
    int empty = 0;
    uint64_t size = 0;
    pz_fs_kind kind;

    pz_path_init(&root);
    pz_path_init(&sub);
    pz_path_init(&file);
    pz_path_init(&missing);
    pz_path_assign_cstr(&root, root_text);
    pz_vec_init(&entries, sizeof(pz_fs_entry));

    path_join(&sub, root_text, "sub");
    path_join(&file, root_text, "b.txt");
    path_join(&missing, root_text, "nope");

    pz_fs_error_clear(&error);
    check(pz_fs_create_directories(&sub, &error), "creates nested directories");
    check(write_text(&file, "abc"), "writes a file through a handle");
    path_join(&file, root_text, "a.txt");
    check(write_text(&file, "x"), "writes a second file");

    pz_fs_error_clear(&error);
    check(pz_fs_exists(&missing, &exists, &error) && !exists,
          "missing path reports exists=0");
    check(!pz_fs_kind_of(&missing, &kind, &error) &&
              error.code == PZ_FS_NOT_FOUND,
          "missing path reports not found");

    check(pz_fs_exists(&root, &exists, &error) && exists, "root exists");
    check(pz_fs_list_dir(&root, &entries, &error), "lists the directory");
    check(entries.length == 3, "listing has three entries");
    if (entries.length == 3) {
        const pz_fs_entry* first = (const pz_fs_entry*)pz_vec_at_const(&entries, 0);
        const pz_fs_entry* second = (const pz_fs_entry*)pz_vec_at_const(&entries, 1);
        const pz_fs_entry* third = (const pz_fs_entry*)pz_vec_at_const(&entries, 2);
        check(str_equals(&first->name, "a.txt") && first->kind == PZ_FS_FILE,
              "listing is sorted and reports files");
        check(str_equals(&second->name, "b.txt"), "listing keeps name order");
        check(str_equals(&third->name, "sub") && third->kind == PZ_FS_DIR,
              "listing reports directories");
    }
    pz_fs_entries_free(&entries);

    path_join(&file, root_text, "b.txt");
    pz_fs_error_clear(&error);
    check(pz_fs_file_size(&file, &size, &error) && size == 3,
          "file_size reports the byte count");
    check(pz_fs_is_empty(&file, &empty, &error) && !empty,
          "a non-empty file is not empty");
    path_join(&file, root_text, "empty.txt");
    check(write_text(&file, ""), "writes an empty file");
    check(pz_fs_is_empty(&file, &empty, &error) && empty,
          "an empty file is empty");
    check(pz_fs_is_empty(&root, &empty, &error) && !empty,
          "a populated directory is not empty");

    {
        pz_path empty_dir;
        pz_vec empty_entries;
        pz_path_init(&empty_dir);
        pz_vec_init(&empty_entries, sizeof(pz_fs_entry));
        path_join(&empty_dir, root_text, "empty-dir");
        check(pz_fs_create_directories(&empty_dir, &error),
              "creates an empty directory");
        check(pz_fs_list_dir(&empty_dir, &empty_entries, &error) &&
                  empty_entries.length == 0,
              "listing an empty directory succeeds with no entries");
        pz_fs_entries_free(&empty_entries);
        pz_path_free(&empty_dir);
    }

    pz_path_free(&missing);
    pz_path_free(&file);
    pz_path_free(&sub);
    pz_path_free(&root);
}

static void test_copy(const char* root_text) {
    pz_path source;
    pz_path dest;
    pz_path file;
    pz_path copy;
    pz_path blocker;
    pz_str text;
    pz_fs_error error;
    pz_fs_kind kind;

    pz_path_init(&source);
    pz_path_init(&dest);
    pz_path_init(&file);
    pz_path_init(&copy);
    pz_path_init(&blocker);
    pz_str_init(&text);

    path_join(&source, root_text, "tree");
    path_join(&dest, root_text, "tree-copy");
    path_join(&file, root_text, "tree/inner.txt");
    pz_fs_error_clear(&error);
    check(pz_fs_create_directories(&source, &error), "creates the copy source");
    check(write_text(&file, "payload"), "writes the copy payload");

    path_join(&copy, root_text, "tree-copy/inner.txt");
    check(pz_fs_copy_tree(&source, &dest, PZ_FS_COPY_RECURSIVE, &error),
          "copies a tree recursively");
    check(pz_fs_read_all(&copy, &text, &error) && str_equals(&text, "payload"),
          "copied contents match");

    check(write_text(&copy, "changed"), "rewrites the copy");
    check(!pz_fs_copy_tree(&source, &dest, PZ_FS_COPY_RECURSIVE, &error) &&
              error.code == PZ_FS_EXISTS,
          "copy without overwrite reports exists");
    check(pz_fs_read_all(&copy, &text, &error) && str_equals(&text, "changed"),
          "copy without overwrite leaves the destination alone");
    check(pz_fs_copy_tree(&source, &dest,
                          PZ_FS_COPY_RECURSIVE | PZ_FS_COPY_OVERWRITE, &error),
          "copy with overwrite replaces files");
    check(pz_fs_read_all(&copy, &text, &error) && str_equals(&text, "payload"),
          "overwriting copy restores the source contents");

    path_join(&blocker, root_text, "blocked");
    check(write_text(&blocker, "file"), "writes a directory blocker");
    check(!pz_fs_copy_tree(&source, &blocker, PZ_FS_COPY_RECURSIVE, &error) &&
              error.code == PZ_FS_INVALID,
          "a file/directory collision is invalid");

    path_join(&file, root_text, "tree/inner.txt");
    check(pz_fs_kind_of(&file, &kind, &error) && kind == PZ_FS_FILE,
          "copied file has file kind");

    pz_str_free(&text);
    pz_path_free(&blocker);
    pz_path_free(&copy);
    pz_path_free(&file);
    pz_path_free(&dest);
    pz_path_free(&source);
}

static void test_remove(const char* root_text) {
    pz_path dir;
    pz_path file;
    pz_path link;
    pz_path target;
    pz_fs_error error;
    uint64_t removed = 0;
    pz_fs_kind kind;

    pz_path_init(&dir);
    pz_path_init(&file);
    pz_path_init(&link);
    pz_path_init(&target);

    path_join(&dir, root_text, "remove-me");
    path_join(&file, root_text, "remove-me/nested.txt");
    pz_fs_error_clear(&error);
    check(pz_fs_create_directories(&dir, &error), "creates the removal tree");
    check(write_text(&file, "data"), "writes inside the removal tree");

    check(!pz_fs_remove(&dir, &error) && error.code == PZ_FS_NOT_EMPTY,
          "non-recursive remove rejects a non-empty directory");
    check(pz_fs_remove_all(&dir, &removed, &error) && removed >= 2,
          "remove_all clears the tree");
    {
        int exists = 1;
        check(pz_fs_exists(&dir, &exists, &error) && !exists,
              "remove_all leaves nothing behind");
    }

    path_join(&target, root_text, "link-target");
    path_join(&link, root_text, "link");
    check(pz_fs_create_directories(&target, &error), "creates the link target");
    path_join(&file, root_text, "link-target/keep.txt");
    check(write_text(&file, "keep"), "writes into the link target");
#ifndef _WIN32
    if (symlink("link-target", link.text.data) == 0) {
        check(pz_fs_kind_of(&link, &kind, &error) && kind == PZ_FS_LINK,
              "kind_of reports a symlink without following it");
        check(pz_fs_remove_all(&link, &removed, &error), "remove_all unlinks");
        {
            int exists = 0;
            check(pz_fs_exists(&link, &exists, &error) && !exists,
                  "the link itself is gone");
            check(pz_fs_exists(&target, &exists, &error) && exists,
                  "the link target survives");
        }
    }
#endif

    pz_path_free(&target);
    pz_path_free(&link);
    pz_path_free(&file);
    pz_path_free(&dir);
}

static void test_timestamps(const char* root_text) {
    pz_path file;
    pz_fs_error error;
    int64_t value = 0;
    int64_t now = 0;

    pz_path_init(&file);
    path_join(&file, root_text, "stamp.txt");
    check(write_text(&file, "t"), "writes the timestamp file");

    pz_fs_error_clear(&error);
    check(pz_fs_set_last_write_time(&file, 1000000000, &error),
          "sets the last write time");
    check(pz_fs_last_write_time(&file, &value, &error) && value == 1000000000,
          "reads back the last write time");
    check(pz_fs_file_time_now(&now) && now > 1000000000,
          "file_time_now returns epoch seconds");

    pz_path_free(&file);
}

static void test_handles(const char* root_text) {
    pz_path file;
    pz_fs_file* handle = NULL;
    pz_fs_error error;
    pz_str text;
    char buffer[4];

    pz_path_init(&file);
    pz_str_init(&text);
    path_join(&file, root_text, "handle.txt");

    pz_fs_error_clear(&error);
    check(pz_fs_open_write(&file, &handle, &error), "opens a file for writing");
    check(pz_fs_write(handle, "hello", 5) == 5, "writes through the handle");
    check(pz_fs_close(handle, &error), "closes the handle");

    check(pz_fs_open_read(&file, &handle, &error), "opens the file for reading");
    check(pz_fs_read(handle, buffer, 2) == 2 && buffer[0] == 'h' &&
              buffer[1] == 'e',
          "reads a prefix through the handle");
    check(pz_fs_file_error(handle) == PZ_FS_OK, "no handle error");
    check(pz_fs_close(handle, &error), "closes the read handle");

    check(pz_fs_read_all(&file, &text, &error) && str_equals(&text, "hello"),
          "read_all returns the whole file");

    check(pz_fs_open_append(&file, &handle, &error), "opens the file for appending");
    check(pz_fs_write(handle, " world", 6) == 6, "writes at the end");
    check(pz_fs_close(handle, &error), "closes the append handle");
    check(pz_fs_read_all(&file, &text, &error) &&
              str_equals(&text, "hello world"),
          "append preserved the earlier bytes");

    {
        int executable = -1;

#ifdef _WIN32
        check(pz_fs_is_executable(&file, &executable, &error) && executable,
              "a regular file is executable on Windows");
#else
        check(pz_fs_is_executable(&file, &executable, &error) && !executable,
              "a fresh file is not executable");
        check(pz_fs_set_executable(&file, 1, &error), "sets the executable bit");
        {
            struct stat info;
            check(stat(file.text.data, &info) == 0 && (info.st_mode & 0111) != 0,
                  "the executable bit is visible");
        }
        check(pz_fs_is_executable(&file, &executable, &error) && executable,
              "is_executable sees the bit");
        check(pz_fs_set_executable(&file, 0, &error), "clears the executable bit");
        check(pz_fs_is_executable(&file, &executable, &error) && !executable,
              "is_executable sees the cleared bit");
#endif
    }

    pz_str_free(&text);
    pz_path_free(&file);
}

static int make_temp_root(char* out, size_t capacity) {
#ifdef _WIN32
    char temp[MAX_PATH];
    DWORD length = GetTempPathA((DWORD)sizeof temp, temp);

    if (length == 0 || length >= sizeof temp) return 0;
    snprintf(out, capacity, "%spzfs-test-%lu", temp,
             (unsigned long)GetCurrentProcessId());
    return CreateDirectoryA(out, NULL) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
#else
    snprintf(out, capacity, "/tmp/pzfs-test-%ld", (long)getpid());
    return mkdir(out, 0700) == 0 || errno == EEXIST;
#endif
}

int main(void) {
    char root_text[512];
    pz_path root;
    pz_path probe;
    pz_fs_error error;

    if (!make_temp_root(root_text, sizeof root_text)) {
        fprintf(stderr, "FAIL: could not create the temporary root\n");
        return 1;
    }

    pz_path_init(&root);
    pz_path_init(&probe);
    pz_path_assign_cstr(&root, root_text);
    pz_fs_error_clear(&error);

    check(pz_fs_current_dir(&probe, &error) && probe.text.length != 0,
          "current_dir returns a path");
    pz_path_free(&probe);
    pz_path_init(&probe);
    check(pz_fs_temp_dir(&probe, &error) && probe.text.length != 0,
          "temp_dir returns a path");
    pz_path_free(&probe);
    pz_path_init(&probe);
    check(pz_fs_executable_path(&probe, &error) && probe.text.length != 0,
          "executable_path returns a path");

    test_paths();
    test_directory(root_text);
    test_copy(root_text);
    test_remove(root_text);
    test_timestamps(root_text);
    test_handles(root_text);

    {
        uint64_t removed = 0;
        pz_fs_remove_all(&root, &removed, &error);
    }

    pz_path_free(&probe);
    pz_path_free(&root);

    if (failures != 0) {
        fprintf(stderr, "%d C filesystem test(s) failed\n", failures);
        return 1;
    }
    puts("C filesystem tests passed");
    return 0;
}
