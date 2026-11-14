#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The parsed IDs are owned by this struct and must be released with
   pz_url_ids_free(). A successful parse may still have count == 0. */
typedef struct pz_url_ids {
    uint64_t* items;
    size_t count;
} pz_url_ids;

/* Returns 1 and writes the ID to out on success, 0 otherwise. */
int pz_url_parse_workshop_id(const char* input, size_t length, uint64_t* out);

/* Parses whitespace-separated IDs/URLs and removes duplicates.
   Returns 1 on success, or 0 when any token has no valid ID. */
int pz_url_parse_workshop_ids(const char* input, size_t length, pz_url_ids* out);

void pz_url_ids_free(pz_url_ids* ids);

#ifdef __cplusplus
}
#endif
