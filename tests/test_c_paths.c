/* C99 tests for path helpers and config parsing. */
#include "core/paths_c.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
}

static int str_is(const pz_str* value, const char* expected) {
    return pz_str_equals(value, expected, strlen(expected));
}

static void test_path_helpers(void) {
    pz_str out;

    pz_str_init(&out);
#ifdef _WIN32
    check(pz_paths_is_absolute("C:\\Users\\pz", strlen("C:\\Users\\pz")),
          "drive path absolute");
    check(pz_paths_is_absolute("\\\\server\\share\\pz",
                              strlen("\\\\server\\share\\pz")),
          "UNC path absolute");
    check(!pz_paths_is_absolute("C:relative", strlen("C:relative")),
          "drive-relative path rejected");
    check(pz_paths_join(&out, "C:\\base", 7, "mods", 4), "Windows join");
    check(str_is(&out, "C:\\base\\mods"), "Windows join contents");
#else
    check(pz_paths_is_absolute("/tmp", 4), "absolute path");
    check(!pz_paths_is_absolute("tmp", 3), "relative path rejected");
    check(pz_paths_join(&out, "/base", 5, "mods", 4), "path join");
    check(str_is(&out, "/base/mods"), "path join contents");
    check(pz_paths_join(&out, "/base/", 6, "mods", 4), "trailing separator join");
    check(str_is(&out, "/base/mods"), "trailing separator join contents");
    check(pz_paths_join(&out, "/base", 5, "/absolute", 9),
          "absolute child replaces base");
    check(str_is(&out, "/absolute"), "absolute child contents");
    check(pz_paths_join(&out, "/base", 5, NULL, 0), "empty child keeps base");
    check(str_is(&out, "/base"), "empty child contents");
    check(pz_paths_builtin_steamcmd("/data", 5, &out),
          "builtin steamcmd path");
    check(str_is(&out, "/data/steamcmd/steamcmd.sh"),
          "builtin steamcmd contents");
#endif
    pz_str_free(&out);
}

static void set_string(pz_str* value, const char* text) {
    check(pz_str_assign_cstr(value, text), "fixture string");
}

static void test_config_parse(void) {
    const char* text =
        "steamcmd_root=/new-root\r\n"
        "steam_user=alice\r\n"
        "zomboid_dir=/new-zomboid\r\n"
        "dark_mode=on\r\n"
        "auto_enable=1\r\n"
        "locked_target=save:A\r\n"
        "locked_target=save:A\r\n"
        "locked_target=server:B\r\n";
    pz_paths_config config;

    pz_paths_config_init(&config);
    set_string(&config.zomboid_dir, "/old-zomboid");
    set_string(&config.user_mods_dir, "/old-zomboid/mods");
    set_string(&config.data_dir, "/old-zomboid/pzmodmgr");
    set_string(&config.steamcmd_exe, "/old-zomboid/pzmodmgr/steamcmd/steamcmd.sh");
    set_string(&config.steam_user, "old-user");

    check(pz_paths_config_parse(&config, text, strlen(text)),
          "config parse");
    check(str_is(&config.zomboid_dir, "/new-zomboid"), "zomboid dir applied");
    check(str_is(&config.user_mods_dir, "/new-zomboid/mods"),
          "mods dir rebased");
    check(str_is(&config.data_dir, "/new-zomboid/pzmodmgr"),
          "data dir rebased");
    check(str_is(&config.steamcmd_exe,
                 "/new-zomboid/pzmodmgr/steamcmd/steamcmd.sh"),
          "builtin steamcmd follows zomboid dir");
    check(str_is(&config.steamcmd_root, "/new-root"), "steamcmd root parsed");
    check(str_is(&config.steam_user, "alice"), "steam user parsed");
    check(config.dark_mode == 1 && config.auto_enable == 1,
          "boolean settings parsed");
    check(config.locked_targets.length == 2, "locked targets deduplicated");
    check(str_is((const pz_str*)pz_vec_at_const(&config.locked_targets, 0),
                 "save:A"),
          "first locked target");
    check(str_is((const pz_str*)pz_vec_at_const(&config.locked_targets, 1),
                 "server:B"),
          "second locked target");

    check(pz_paths_config_parse(&config, "zomboid_dir=relative/Zomboid\n",
                                strlen("zomboid_dir=relative/Zomboid\n")),
          "relative zomboid config accepted");
    check(str_is(&config.zomboid_dir, "/new-zomboid"),
          "relative zomboid dir does not rebase");

    check(pz_paths_config_parse(&config, "steam_user=bob\n",
                                strlen("steam_user=bob\n")),
          "second config parse");
    check(str_is(&config.steam_user, "bob"), "second parse updates user");
    check(str_is(&config.zomboid_dir, "/new-zomboid"),
          "missing zomboid key preserves path");
    check(config.dark_mode == 1 && config.auto_enable == 1,
          "missing booleans preserve values");

    pz_paths_config_free(&config);
    check(config.steam_user.data == NULL && config.locked_targets.data == NULL,
          "config free releases fields");
}

static void test_config_format(void) {
    pz_paths_config config;
    pz_str formatted;

    pz_paths_config_init(&config);
    set_string(&config.steamcmd_exe, "/data/steamcmd/steamcmd.sh");
    set_string(&config.steamcmd_root, "/root");
    set_string(&config.steam_user, "user");
    set_string(&config.zomboid_dir, "/zomboid");
    config.dark_mode = 1;
    config.auto_enable = 0;
    check(pz_paths_config_parse(&config,
                                "locked_target=default\nsteam_user=user\n",
                                strlen("locked_target=default\nsteam_user=user\n")),
          "format fixture lock");
    pz_str_init(&formatted);
    check(pz_paths_config_format(&config, &formatted),
          "config format");
    check(strstr(formatted.data,
                 "steamcmd_exe=/data/steamcmd/steamcmd.sh\n") != NULL,
          "format steamcmd path");
    check(strstr(formatted.data, "steamcmd_root=/root\n") != NULL,
          "format steamcmd root");
    check(strstr(formatted.data, "steam_user=user\n") != NULL,
          "format steam user");
    check(strstr(formatted.data, "zomboid_dir=/zomboid\n") != NULL,
          "format zomboid dir");
    check(strstr(formatted.data, "dark_mode=1\n") != NULL,
          "format dark mode");
    check(strstr(formatted.data, "auto_enable=0\n") != NULL,
          "format auto enable");
    check(strstr(formatted.data, "locked_target=default\n") != NULL,
          "format locked target");
    pz_str_free(&formatted);
    pz_paths_config_free(&config);
}

int main(void) {
    test_path_helpers();
    test_config_parse();
    test_config_format();
    if (g_failures != 0) {
        printf("%d C paths test(s) failed\n", g_failures);
        return 1;
    }
    printf("C paths tests passed\n");
    return 0;
}
