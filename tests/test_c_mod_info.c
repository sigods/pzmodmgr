/* C99 tests for the mod.info parser and version rules. */
#include "core/mod_info_c.h"

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

static void test_parse(void) {
    const char* text =
        "name=My Mod\r\n"
        "id=mymod\r\n"
        "author=Alice\r\n"
        "modversion=1.2.3\r\n"
        "versionMin=41.78\r\n"
        "versionMax=42.1\r\n"
        "require=\\damnlib, \\DamnLibGlass, damnlib\r\n"
        "requires=Extra; damnlib\r\n"
        "icon=art/custom.png\r\n"
        "ignored line\r\n";
    pz_mod_info info;

    pz_mod_info_init(&info);
    check(pz_mod_info_parse(&info, text, strlen(text)), "mod.info parse");
    check(str_is(&info.name, "My Mod"), "name parsed");
    check(str_is(&info.id, "mymod"), "id parsed");
    check(str_is(&info.author, "Alice"), "author parsed");
    check(str_is(&info.mod_version, "1.2.3"), "modversion parsed");
    check(str_is(&info.version_min, "41.78"), "versionMin parsed");
    check(str_is(&info.version_max, "42.1"), "versionMax parsed");
    check(str_is(&info.icon, "art/custom.png"), "icon raw value parsed");
    check(pz_mod_info_required_count(&info) == 3,
          "required IDs deduplicated across lines");
    check(str_is(pz_mod_info_required_at(&info, 0), "damnlib"),
          "first required ID");
    check(str_is(pz_mod_info_required_at(&info, 1), "DamnLibGlass"),
          "second required ID");
    check(str_is(pz_mod_info_required_at(&info, 2), "Extra"),
          "third required ID");
    check(pz_mod_info_required_at(&info, 3) == NULL,
          "required ID bounds check");

    pz_mod_info_free(&info);
    check(info.id.data == NULL && info.required_ids.data == NULL,
          "mod.info free releases fields");
}

static void test_parse_edges(void) {
    pz_mod_info info;

    pz_mod_info_init(&info);
    check(pz_mod_info_parse(&info, NULL, 0), "empty buffer accepted");
    check(info.name.length == 0, "empty buffer leaves fields empty");
    check(pz_mod_info_parse_line(&info, "=value", 6), "empty key ignored");
    check(pz_mod_info_parse_line(&info, "name=", 5), "empty value accepted");
    check(info.name.length == 0, "empty value clears field");

    check(pz_mod_info_parse_line(&info, "require=\"\\Quoted\", /Slash/",
                                 strlen("require=\"\\Quoted\", /Slash/")),
          "quoted and slash requirements parsed");
    check(pz_mod_info_required_count(&info) == 2,
          "quoted requirement count");
    check(str_is(pz_mod_info_required_at(&info, 0), "\\Quoted"),
          "quote removal follows slash removal");
    check(str_is(pz_mod_info_required_at(&info, 1), "Slash/"),
          "leading slash removed only");
    pz_mod_info_free(&info);
}

static void test_versions(void) {
    pz_mod_info info;
    pz_str range;

    pz_str_init(&range);
    check(pz_version_range("41.78", 5, "42.1", 4, &range),
          "two-sided range formats");
    check(str_is(&range, "PZ 41.78-42.1"), "two-sided range contents");
    check(pz_version_range("41.78", 5, NULL, 0, &range),
          "open-ended range formats");
    check(str_is(&range, "PZ 41.78+"), "open-ended range contents");
    check(pz_version_range(NULL, 0, "42.1", 4, &range),
          "upper-only range formats");
    check(str_is(&range, "PZ up to 42.1"), "upper-only range contents");
    check(pz_version_range(NULL, 0, NULL, 0, &range), "empty range formats");
    check(range.length == 0, "empty range stays empty");

    check(pz_supports_game_version("41.78", 5, "42.1", 4, "42.21.0", 7),
          "range includes later patch");
    check(!pz_supports_game_version("41.78", 5, "42.1", 4, "43.0.0", 6),
          "range excludes next major");
    check(pz_supports_game_version("42.22", 5, NULL, 0, "42.21.0", 7),
          "minimum only accepts same major");
    check(!pz_supports_game_version("42.22", 5, NULL, 0, "43.0.0", 6),
          "minimum only rejects next major");
    check(pz_supports_game_version(NULL, 0, "41.0", 4, "41.78", 5),
          "maximum only accepts matching major");
    check(!pz_supports_game_version(NULL, 0, "41.0", 4, "42.0", 4),
          "maximum only rejects next major");
    check(pz_supports_game_version("41", 2, "42", 2, "", 0),
          "empty game version accepts");
    check(pz_supports_game_version("41", 2, "42", 2, "unknown", 7),
          "version without digits accepts");

    pz_mod_info_init(&info);
    pz_str_assign_cstr(&info.version_min, "41.78");
    pz_str_assign_cstr(&info.version_max, "42.1");
    check(pz_mod_info_version_range(&info, &range), "struct range formats");
    check(str_is(&range, "PZ 41.78-42.1"), "struct range contents");
    check(pz_mod_info_supports_game_version(&info, "42.21", 5),
          "struct version check");
    pz_mod_info_free(&info);
    pz_str_free(&range);
}

static void test_ranks(void) {
    check(pz_version_rank("42", 2) == 42, "numeric rank");
    check(pz_version_rank("042", 3) == 42, "leading zero rank");
    check(pz_version_rank("0", 1) == 0, "zero rank");
    check(pz_version_rank("", 0) == -1, "empty rank rejected");
    check(pz_version_rank("42a", 3) == -1, "non-numeric rank rejected");
    check(pz_version_rank("999999999999999999999", 21) == -1,
          "overflowing rank rejected");
}

int main(void) {
    test_parse();
    test_parse_edges();
    test_versions();
    test_ranks();
    if (g_failures != 0) {
        printf("%d C mod.info test(s) failed\n", g_failures);
        return 1;
    }
    printf("C mod.info tests passed\n");
    return 0;
}
