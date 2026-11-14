#define _POSIX_C_SOURCE 200809L

#include "core/net_c.h"
#include "core/process_c.h"
#include "core/updater_c.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int str_equals(const pz_str* value, const char* expected) {
    return value && value->length == strlen(expected) &&
           (value->length == 0 || memcmp(value->data, expected, value->length) == 0);
}

static void join(pz_path* out, const pz_path* base, const char* leaf) {
    pz_path_join(out, base, leaf, strlen(leaf));
}

static int write_text(const pz_path* path, const char* text) {
    pz_path parent;
    pz_fs_file* file = NULL;
    pz_fs_error error;
    size_t length = strlen(text);
    size_t written;
    int ok = 0;

    pz_path_init(&parent);
    pz_fs_error_clear(&error);
    if (!pz_path_parent(path, &parent) || !parent.text.length ||
        !pz_fs_create_directories(&parent, &error))
        goto done;
    pz_fs_error_clear(&error);
    if (!pz_fs_open_write(path, &file, &error)) goto done;
    written = pz_fs_write(file, text, length);
    ok = pz_fs_close(file, &error) && written == length;

done:
    pz_path_free(&parent);
    return ok;
}

static int exists(const pz_path* path) {
    int found = 0;
    pz_fs_error error;

    pz_fs_error_clear(&error);
    return pz_fs_exists(path, &found, &error) && found != 0;
}

static void test_cleanup(const pz_path* root) {
    pz_path data;
    pz_path updates;
    pz_path dir;
    pz_path file;
    pz_str pid_text;

    pz_path_init(&data);
    pz_path_init(&updates);
    pz_path_init(&dir);
    pz_path_init(&file);
    pz_str_init(&pid_text);

    join(&data, root, "Zomboid/pzmodmgr");
    join(&updates, &data, "updates");

    join(&dir, &updates, "0.3.0");
    join(&file, &dir, "pending.txt");
    check(write_text(&file, "pid=0\nversion=0.3.0\n"),
          "abandoned marker is written");

    join(&dir, &updates, "0.4.0");
    join(&file, &dir, "pending.txt");
    pz_str_assign_cstr(&pid_text, "pid=");
    pz_str_append_u64(&pid_text, pz_process_current_id());
    pz_str_append_cstr(&pid_text, "\nversion=0.4.0\n");
    check(write_text(&file, pid_text.data), "live marker is written");

    join(&dir, &updates, "recent");
    join(&file, &dir, "package.zip");
    check(write_text(&file, "new"), "completed staging folder is written");

    join(&dir, &data, "update_helper");
    join(&file, &dir, "pzmodmgr");
    check(write_text(&file, "old helper"), "leftover helper is written");

    pz_updater_cleanup_cache(&data);

    join(&dir, &updates, "0.3.0");
    check(!exists(&dir), "abandoned staged update is pruned");
    join(&dir, &updates, "0.4.0");
    check(exists(&dir), "staged update with a live helper is kept");
    join(&dir, &updates, "recent");
    check(!exists(&dir), "completed update staging folder is pruned");
    join(&dir, &data, "update_helper");
    check(!exists(&dir), "leftover update helper is pruned");

    pz_str_free(&pid_text);
    pz_path_free(&file);
    pz_path_free(&dir);
    pz_path_free(&updates);
    pz_path_free(&data);
}

static void test_helper_entry(void) {
    char name[] = "pzmodmgr";
    char flag[] = "--apply-update";
    char other[] = "add";
    char* helper[] = {name, flag};
    char* plain[] = {name, other};

    check(!pz_updater_is_helper_invocation(1, helper),
          "a bare invocation is not the update helper");
    check(pz_updater_is_helper_invocation(2, helper),
          "--apply-update selects the update helper");
    check(!pz_updater_is_helper_invocation(2, plain),
          "another command is not the update helper");
    check(pz_updater_run_helper(2, helper) == 1,
          "the helper rejects an incomplete path set");
}

static void test_unsupported_archive(const pz_path* root) {
    pz_path archive;
    pz_path dest;
    pz_str detail;

    pz_path_init(&archive);
    pz_path_init(&dest);
    pz_str_init(&detail);

    join(&archive, root, "package.rar");
    check(write_text(&archive, "not an archive"), "unknown archive is written");
    join(&dest, root, "extract");

    check(!pz_net_extract(&archive, &dest, &detail),
          "an unsupported archive is rejected");
    check(str_equals(&detail, "unsupported update archive"),
          "the rejection names the archive type");

    pz_str_free(&detail);
    pz_path_free(&dest);
    pz_path_free(&archive);
}

int main(void) {
    pz_path temp;
    pz_path root;
    pz_fs_error error;

    pz_path_init(&temp);
    pz_path_init(&root);
    pz_fs_error_clear(&error);
    check(pz_fs_temp_dir(&temp, &error), "temporary directory is available");
    join(&root, &temp, "pzmodmgr-updater-test");
    {
        uint64_t removed = 0;

        pz_fs_error_clear(&error);
        pz_fs_remove_all(&root, &removed, &error);
        pz_fs_error_clear(&error);
        check(pz_fs_create_directories(&root, &error),
              "test root is created");
    }

    test_cleanup(&root);
    test_helper_entry();
    test_unsupported_archive(&root);

    {
        uint64_t removed = 0;

        pz_fs_error_clear(&error);
        pz_fs_remove_all(&root, &removed, &error);
    }
    pz_path_free(&root);
    pz_path_free(&temp);

    if (failures) {
        fprintf(stderr, "%d updater test(s) failed\n", failures);
        return 1;
    }
    printf("all updater tests passed\n");
    return 0;
}
