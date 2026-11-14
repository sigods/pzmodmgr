/* C99 tests for activation block/INI parsing and rewriting. */
#include "core/activation_c.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
}

static const pz_str* item(const pz_vec* list, size_t index) {
    return (const pz_str*)pz_vec_at_const(list, index);
}

static int item_is(const pz_vec* list, size_t index, const char* expected) {
    const pz_str* value = item(list, index);
    return value && pz_str_equals(value, expected, strlen(expected));
}

static int make_strings(pz_vec* list, const char* const* values, size_t count) {
    size_t i;
    if (!pz_vec_init(list, sizeof(pz_str))) return 0;
    for (i = 0; i < count; ++i) {
        pz_str* slot = (pz_str*)pz_vec_push(list);
        if (!slot || !pz_str_assign_cstr(slot, values[i])) {
            if (slot) {
                pz_str_free(slot);
                --list->length;
            }
            pz_activation_string_list_free(list);
            return 0;
        }
    }
    return 1;
}

static int make_ids(pz_vec* list, const uint64_t* values, size_t count) {
    size_t i;
    if (!pz_vec_init(list, sizeof(uint64_t))) return 0;
    for (i = 0; i < count; ++i) {
        uint64_t* slot = (uint64_t*)pz_vec_push(list);
        if (!slot) return 0;
        *slot = values[i];
    }
    return 1;
}

static void test_parse_block(void) {
    const char* text =
        "VERSION = 1,\r\n"
        "\r\n"
        "mods\r\n"
        "{\r\n"
        "    mod = Alpha,\r\n"
        "    mod = \\Beta,\r\n"
        "}\r\n"
        "\r\n"
        "maps\r\n"
        "{\r\n"
        "}\r\n";
    pz_vec ids;

    check(pz_activation_parse_block_ids(text, strlen(text), &ids),
          "block parse");
    check(ids.length == 2, "block ID count");
    check(item_is(&ids, 0, "Alpha"), "block first ID");
    check(item_is(&ids, 1, "Beta"), "block legacy prefix stripped");
    check(pz_activation_block_uses_backslash(text, strlen(text)),
          "block backslash detected");
    pz_activation_string_list_free(&ids);
}

static void test_parse_ini(void) {
    const char* text =
        "PublicName=X\r\n"
        "  Mods = \\Alpha;\\Beta\r\n"
        "WorkshopItems=111;222;0\r\n"
        "Map=Muldraugh, KY\r\n";
    pz_vec ids;
    pz_vec workshops;
    int backslash = 0;

    check(pz_activation_parse_ini_ids(text, strlen(text), &ids, &backslash),
          "INI Mods parse");
    check(backslash == 1, "INI backslash detected");
    check(ids.length == 2, "INI ID count");
    check(item_is(&ids, 0, "Alpha") && item_is(&ids, 1, "Beta"),
          "INI IDs normalized");
    pz_activation_string_list_free(&ids);

    check(pz_activation_parse_ini_workshop_ids(text, strlen(text), &workshops),
          "INI WorkshopItems parse");
    check(workshops.length == 2, "WorkshopItems count");
    check(*(const uint64_t*)pz_vec_at_const(&workshops, 0) == 111 &&
              *(const uint64_t*)pz_vec_at_const(&workshops, 1) == 222,
          "WorkshopItems values");
    pz_vec_free(&workshops);
}

static void test_rewrite_block(void) {
    const char* text =
        "VERSION = 1,\r\n\r\nmods\r\n{\r\n"
        "    mod = Alpha,\r\n"
        "    mod = \\Beta,\r\n"
        "}\r\n\r\nmaps\r\n{\r\n}\r\n";
    const char* const values[] = {"Beta", "Gamma"};
    pz_vec ids;
    pz_vec parsed;
    pz_str out;
    int changed = 0;

    check(make_strings(&ids, values, 2), "rewrite fixture IDs");
    pz_str_init(&out);
    check(pz_activation_rewrite_block_ids(text, strlen(text), &ids, &out,
                                          &changed),
          "block rewrite");
    check(changed == 1, "block rewrite change flag");
    check(strstr(out.data, "mod = Gamma,") != NULL, "new block ID written");
    check(strstr(out.data, "mod = \\Gamma,") == NULL,
          "block rewrite uses plain IDs");
    check(strstr(out.data, "mod = Alpha") == NULL, "old block ID removed");
    check(strstr(out.data, "maps") != NULL && strstr(out.data, "\r\n") != NULL,
          "block rewrite preserves maps and CRLF");
    check(pz_activation_parse_block_ids(out.data, out.length, &parsed),
          "rewritten block parses");
    check(parsed.length == 2 && item_is(&parsed, 0, "Beta") &&
              item_is(&parsed, 1, "Gamma"),
          "rewritten block round trip");
    pz_activation_string_list_free(&parsed);
    pz_str_free(&out);
    pz_activation_string_list_free(&ids);
}

static void test_rewrite_ini(void) {
    const char* text =
        "PublicName=X\r\nMods=\\Alpha;\\Beta\r\n"
        "WorkshopItems=111;222\r\nMap=Muldraugh, KY\r\n";
    const char* const values[] = {"Beta", "Gamma"};
    const uint64_t workshop_values[] = {111, 333};
    pz_vec ids;
    pz_vec workshops;
    pz_str out;
    int changed = 0;

    check(make_strings(&ids, values, 2), "INI rewrite fixture IDs");
    pz_str_init(&out);
    check(pz_activation_rewrite_ini_mods(text, strlen(text), &ids, 1, &out,
                                         &changed),
          "INI Mods rewrite");
    check(changed == 1, "INI Mods change flag");
    check(strstr(out.data, "Mods=\\Beta;\\Gamma") != NULL,
          "INI Mods rewrite contents");
    check(strstr(out.data, "WorkshopItems=111;222") != NULL &&
              strstr(out.data, "Map=Muldraugh") != NULL,
          "INI rewrite preserves other lines");

    check(make_ids(&workshops, workshop_values, 2), "workshop fixture IDs");
    check(pz_activation_rewrite_ini_workshop_items(out.data, out.length,
                                                   &workshops, &out, &changed),
          "WorkshopItems rewrite");
    check(changed == 1, "WorkshopItems change flag");
    check(strstr(out.data, "WorkshopItems=111;333") != NULL,
          "WorkshopItems rewrite contents");
    pz_vec_free(&workshops);
    pz_str_free(&out);
    pz_activation_string_list_free(&ids);
}

static void test_empty(void) {
    pz_str out;
    const char* const values[] = {"Beta"};
    pz_vec ids;
    int changed = 0;

    check(make_strings(&ids, values, 1), "empty fixture IDs");
    pz_str_init(&out);
    check(pz_activation_rewrite_ini_mods("", 0, &ids, 0, &out, &changed),
          "empty INI Mods rewrite");
    check(changed == 1, "empty INI rewrite change flag");
    check(pz_str_equals(&out, "Mods=Beta\n", 10), "empty INI rewrite contents");
    pz_str_free(&out);
    pz_activation_string_list_free(&ids);
}

int main(void) {
    test_parse_block();
    test_parse_ini();
    test_rewrite_block();
    test_rewrite_ini();
    test_empty();
    if (g_failures != 0) {
        printf("%d C activation test(s) failed\n", g_failures);
        return 1;
    }
    printf("C activation tests passed\n");
    return 0;
}
