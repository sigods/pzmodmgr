#include "core/hash_c.h"
#include "core/json_c.h"
#include "core/steam_json_c.h"
#include "core/updater_json_c.h"

#include <stdio.h>
#include <stdlib.h>
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

static int vector_id(const pz_vec* ids, size_t index) {
    const uint64_t* value = (const uint64_t*)pz_vec_at_const(ids, index);
    return value ? (int)*value : -1;
}

static void test_sha256(void) {
    pz_str digest;

    pz_str_init(&digest);

    check(pz_sha256_hex("", 0, &digest) &&
              str_equals(&digest,
                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "sha256 empty");
    check(pz_sha256_hex("abc", 3, &digest) &&
              str_equals(&digest,
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256 abc");
    check(pz_sha256_hex("The quick brown fox jumps over the lazy dog", 43,
                        &digest) &&
              str_equals(&digest,
                         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"),
          "sha256 quick brown fox");

    {
        char* data = (char*)malloc(1000000);
        if (data) {
            memset(data, 'a', 1000000);
            check(pz_sha256_hex(data, 1000000, &digest) &&
                      str_equals(&digest,
                                 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
                  "sha256 one million a");
            free(data);
        }
    }

    pz_str_free(&digest);
}

static void test_json(void) {
    static const char* json =
        "{\"tag_name\":\"v0.3.0\",\"size\":123,\"title\":\"My \\\"Mod\\\"\","
        "\"unicode\":\"Caf\\u00e9\",\"emoji\":\"\\ud83d\\ude00\","
        "\"nested\":{\"result\":7}}";
    const size_t length = strlen(json);
    pz_str text;
    size_t pos = 0;
    size_t after = 0;
    uint64_t number = 0;

    pz_str_init(&text);

    check(pz_json_find_key(json, length, "tag_name", 0, &pos),
          "json finds key");
    check(pz_json_read_string_after(json, length, pos, &text, &after) &&
              str_equals(&text, "v0.3.0") && after > pos,
          "json reads string");
    check(pz_json_string_field(json, length, "title", 0, &text, NULL) &&
              str_equals(&text, "My \"Mod\""),
          "json decodes escaped quotes");
    check(pz_json_string_field(json, length, "unicode", 0, &text, NULL) &&
              str_equals(&text, "Caf\xc3\xa9"),
          "json decodes unicode escape");
    check(pz_json_string_field(json, length, "emoji", 0, &text, NULL) &&
              str_equals(&text, "\xf0\x9f\x98\x80"),
          "json decodes surrogate pair");
    check(pz_json_uint_field(json, length, "size", 0, &number) && number == 123,
          "json reads uint");
    check(pz_json_uint_field(json, length, "result", 0, &number) && number == 7,
          "json finds nested uint");
    check(!pz_json_uint_field(json, length, "tag_name", 0, &number),
          "json rejects non-numeric uint");
    check(!pz_json_string_field(json, length, "missing", 0, &text, NULL),
          "json rejects missing key");

    pos = (size_t)(strstr(json, "\"tag_name\"") - json);
    check(pz_json_skip_string(json, length, &pos) && json[pos] == ':',
          "json skips string");
    check(!pz_json_skip_string(json, length, &pos),
          "json skip rejects non-string");

    pz_str_free(&text);
}

static void test_json_escapes(void) {
    static const char* json =
        "{\"all\":\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"}";
    static const char* lone_high = "{\"bad\":\"\\ud83d\"}";
    static const char* bad_escape = "{\"bad\":\"\\x\"}";
    const size_t length = strlen(json);
    pz_str text;

    pz_str_init(&text);
    check(pz_json_string_field(json, length, "all", 0, &text, NULL) &&
              str_equals(&text, "a\"b\\c/d\be\ff\ng\rh\ti"),
          "json decodes every short escape");
    check(!pz_json_string_field(lone_high, strlen(lone_high), "bad", 0, &text,
                                NULL),
          "json rejects a lone high surrogate");
    check(!pz_json_string_field(bad_escape, strlen(bad_escape), "bad", 0, &text,
                                NULL),
          "json rejects an unknown escape");
    pz_str_free(&text);
}

static void test_updater_json(void) {
    static const char* json =
        "{\"tag_name\":\"v0.3.0\",\"assets\":["
        "{\"name\":\"PZModManager-0.3.0-win64-setup.exe\","
        "\"browser_download_url\":\"https://example/setup.exe\",\"size\":10},"
        "{\"name\":\"PZModManager-0.3.0-win64.zip\","
        "\"browser_download_url\":\"https://example/win.zip\",\"size\":123,"
        "\"digest\":\"sha256:abc\"},"
        "{\"name\":\"PZModManager-0.3.0-linux-x86_64.tar.gz\","
        "\"browser_download_url\":\"https://example/linux.tar.gz\",\"size\":456,"
        "\"digest\":\"sha256:def\"}]}";
    pz_app_update update;

    check(pz_app_version_newer("v0.3.0", 6, "0.2.1", 5),
          "app version newer");
    check(!pz_app_version_newer("0.2.1", 5, "0.2.1", 5),
          "app version same");
    check(!pz_app_version_newer("0.2.0", 5, "0.2.1", 5),
          "app version older");

    pz_app_update_init(&update);
    check(pz_parse_latest_app_release(json, strlen(json), "0.2.1", 5, "win", 3,
                                      &update),
          "app release parses Windows");
    check(update.ok && update.available &&
              str_equals(&update.latest_version, "0.3.0") &&
              str_equals(&update.asset_name,
                         "PZModManager-0.3.0-win64.zip") &&
              str_equals(&update.asset_url, "https://example/win.zip") &&
              update.asset_size == 123 &&
              str_equals(&update.asset_digest, "sha256:abc"),
          "app release selects Windows zip");
    pz_app_update_free(&update);

    pz_app_update_init(&update);
    check(pz_parse_latest_app_release(json, strlen(json), "0.2.1", 5, "linux",
                                      5, &update) &&
              update.ok && update.available &&
              str_equals(&update.asset_name,
                         "PZModManager-0.3.0-linux-x86_64.tar.gz"),
          "app release selects Linux tar.gz");
    pz_app_update_free(&update);

    pz_app_update_init(&update);
    check(pz_parse_latest_app_release(json, strlen(json), "0.3.0", 5, "win", 3,
                                      &update) &&
              update.ok && !update.available,
          "app release current");
    pz_app_update_free(&update);

    pz_app_update_init(&update);
    check(pz_parse_latest_app_release("{}", 2, "0.2.1", 5, "win", 3,
                                      &update) &&
              !update.ok,
          "app release without tag rejected");
    pz_app_update_free(&update);
}

static void test_steam_json(void) {
    static const char* details =
        "{\"response\":{\"result\":1,\"resultcount\":1,"
        "\"publishedfiledetails\":[{\"publishedfileid\":\"123\","
        "\"result\":9,\"title\":\"Caf\\u00e9 \\ud83d\\ude00\","
        "\"time_updated\":1700000123}]}}";
    static const char* tricky =
        "{\"response\":{\"publishedfiledetails\":[{\"title\":\"trick "
        "\\\"time_updated\\\":999\",\"time_updated\":42}]}}";
    static const char* collection =
        "{\"response\":{\"result\":1,\"resultcount\":1,\"collectiondetails\":"
        "[{\"publishedfileid\":\"3417834390\",\"result\":1,\"children\":["
        "{\"publishedfileid\":\"111\",\"sortorder\":0},"
        "{\"publishedfileid\":\"222\",\"sortorder\":1},"
        "{\"publishedfileid\":\"111\",\"sortorder\":2}]}]}}";
    pz_str title;
    pz_vec ids;

    pz_str_init(&title);
    pz_vec_init(&ids, sizeof(uint64_t));

    check(pz_parse_published_file_time_updated(details, strlen(details)) ==
              1700000123,
          "steam time_updated");
    check(pz_parse_published_file_time_updated(tricky, strlen(tricky)) == 42,
          "steam time_updated ignores string contents");
    check(pz_parse_published_file_time_updated("not json", 8) == 0,
          "steam time_updated rejects garbage");
    check(pz_parse_published_file_title(details, strlen(details), &title) &&
              str_equals(&title, "Caf\xc3\xa9 \xf0\x9f\x98\x80"),
          "steam title decodes UTF-8");
    check(pz_parse_published_file_result(details, strlen(details)) == 9,
          "steam uses item result");
    check(pz_parse_published_file_result("not json", 8) == 0,
          "steam result rejects garbage");
    check(pz_parse_published_file_title("{}", 2, &title) == 0,
          "steam title rejects missing details");

    check(pz_parse_collection_children(collection, strlen(collection), &ids),
          "steam parses collection");
    check(ids.length == 2 && vector_id(&ids, 0) == 111 &&
              vector_id(&ids, 1) == 222,
          "steam collection deduplicates children");
    pz_vec_free(&ids);

    check(pz_parse_collection_children(
              "{\"publishedfiledetails\":[{\"publishedfileid\":\"123\"}]}",
              49, &ids) &&
              ids.length == 0,
          "steam collection ignores non-collection response");
    pz_vec_free(&ids);

    pz_str_free(&title);
}

int main(void) {
    test_sha256();
    test_json();
    test_json_escapes();
    test_updater_json();
    test_steam_json();
    if (failures != 0) {
        fprintf(stderr, "%d C SHA/JSON test(s) failed\n", failures);
        return 1;
    }
    puts("C SHA/JSON tests passed");
    return 0;
}
