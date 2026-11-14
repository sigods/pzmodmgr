#ifndef PZ_CORE_PATHS_C_H
#define PZ_CORE_PATHS_C_H

/* C99 path joins, config discovery, and config-file load/save. */

#include "core/c_util.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_paths_config {
    pz_str steamcmd_exe;
    pz_str steamcmd_root;
    pz_str steam_user;
    int dark_mode;
    int auto_enable;
    pz_vec locked_targets; /* pz_str elements */
    pz_str zomboid_dir;
    pz_str user_mods_dir;
    pz_str data_dir;
    pz_str config_file;
} pz_paths_config;

void pz_paths_config_init(pz_paths_config* config);
void pz_paths_config_free(pz_paths_config* config);

/* Applies key=value lines to an existing config, matching load_config():
 * missing string keys are left unchanged (except steam_user), missing boolean
 * keys are left unchanged, and locked_target values are appended uniquely. */
int pz_paths_config_parse(pz_paths_config* config, const char* text,
                          size_t length);

/* Serializes the same lines save_config() writes. */
int pz_paths_config_format(const pz_paths_config* config, pz_str* out);

int pz_paths_is_absolute(const char* path, size_t length);
int pz_paths_join(pz_str* out, const char* base, size_t base_length,
                 const char* child, size_t child_length);

int pz_paths_builtin_steamcmd(const char* data_dir, size_t length, pz_str* out);

/* Builds the same default paths default_config() used to: ~/Zomboid,
 * ~/Zomboid/mods, ~/Zomboid/pzmodmgr, and an installed config.ini next to the
 * executable when one exists. */
int pz_paths_config_default(pz_paths_config* config);

/* Applies an existing config file. A missing or unreadable file leaves the
 * defaults untouched and still returns 1. */
int pz_paths_config_load(pz_paths_config* config);

/* Formats and atomically writes config->config_file, creating its parent. */
int pz_paths_config_save(const pz_paths_config* config);

/* Deep-copies a config. out must be uninitialized. */
int pz_paths_config_copy(pz_paths_config* out, const pz_paths_config* in);

#ifdef __cplusplus
}
#endif

#endif
