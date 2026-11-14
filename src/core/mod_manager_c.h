#ifndef PZ_CORE_MOD_MANAGER_C_H
#define PZ_CORE_MOD_MANAGER_C_H

/* C99 manager state and operations.
 *
 * The services table keeps network, activation, and mod.info access outside
 * the state machine, so the same manager can be driven by the app and tests.
 */

#include "core/activation_c.h"
#include "core/c_util.h"
#include "core/fs_c.h"
#include "core/mod_info_c.h"
#include "core/paths_c.h"
#include "core/updater_json_c.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pz_installed_mod {
    uint64_t workshop_id;
    pz_str folder;
    pz_str name;
    uint64_t time_updated;
    int owned;
    int locked;
    int imported;
} pz_installed_mod;

void pz_installed_mod_init(pz_installed_mod* mod);
void pz_installed_mod_free(pz_installed_mod* mod);

typedef struct pz_missing_requirement {
    pz_str mod_name;
    pz_str folder;
    pz_str required_id;
} pz_missing_requirement;

void pz_missing_requirement_init(pz_missing_requirement* value);
void pz_missing_requirement_free(pz_missing_requirement* value);

typedef struct pz_activation_target_data {
    int kind; /* 0 default, 1 save, 2 server */
    pz_str name;
    pz_str label;
    pz_str path;
    pz_str thumbnail;
    pz_vec mod_ids;      /* pz_str */
    pz_vec workshop_ids; /* uint64_t */
    int backslash;
    int exists;
    int lock_updates;
} pz_activation_target_data;

void pz_activation_target_data_init(pz_activation_target_data* target);
void pz_activation_target_data_free(pz_activation_target_data* target);

typedef struct pz_remote_details {
    uint32_t result;
    uint64_t time_updated;
    pz_str title;
} pz_remote_details;

void pz_remote_details_init(pz_remote_details* details);
void pz_remote_details_free(pz_remote_details* details);

typedef struct pz_mm_services {
    void* context;
    int (*read_mod_info)(void* context, const char* path, size_t path_length,
                         const char* game_version, size_t game_length,
                         pz_mod_info* out);
    int (*discover_mod_roots)(void* context, const char* path, size_t path_length,
                              pz_vec* out);
    int (*discover_targets)(void* context, const char* zomboid, size_t zomboid_length,
                            const char* game_version, size_t game_length,
                            pz_vec* out);
    int (*write_target)(void* context, pz_activation_target_data* target,
                        const pz_vec* add, const pz_vec* workshops, int enabled);
    uint64_t (*remote_time)(void* context, uint64_t workshop_id);
    int (*remote_details)(void* context, uint64_t workshop_id,
                          pz_remote_details* out);
    int (*collection_children)(void* context, uint64_t collection_id, pz_vec* out);
    int (*download)(void* context, const pz_paths_config* config,
                    uint64_t workshop_id, const pz_path* install_dir,
                    pz_path* content_dir, pz_str* detail, const pz_log* log);
    int (*check_app_update)(void* context, pz_app_update* out);
    int (*save_config)(void* context, const pz_paths_config* config);
    int (*builtin_steamcmd)(void* context, const char* data_dir,
                            size_t data_dir_length, pz_str* out);
    const char* (*app_version)(void* context);
} pz_mm_services;

typedef struct pz_mm_result {
    int ok;
    pz_str message;
} pz_mm_result;

void pz_mm_result_init(pz_mm_result* result);
void pz_mm_result_free(pz_mm_result* result);

typedef struct pz_mod_manager pz_mod_manager;

pz_mod_manager* pz_mod_manager_create(const pz_paths_config* config,
                                      const pz_mm_services* services);
void pz_mod_manager_destroy(pz_mod_manager* manager);

void pz_mod_manager_get_config(const pz_mod_manager* manager,
                               pz_paths_config* out);
void pz_mod_manager_set_config(pz_mod_manager* manager,
                               const pz_paths_config* config);
/* Persists the manager's current config through its save_config service. */
int pz_mod_manager_save_config(pz_mod_manager* manager);
const pz_str* pz_mod_manager_game_version(const pz_mod_manager* manager);
int pz_mod_manager_app_update_available(const pz_mod_manager* manager);
int pz_mod_manager_update_check_due(const pz_mod_manager* manager);
int pz_mod_manager_app_update_check_due(const pz_mod_manager* manager);
int pz_mod_manager_reload_installed(pz_mod_manager* manager);
int pz_mod_manager_update_available(const pz_mod_manager* manager,
                                    uint64_t workshop_id);
int pz_mod_manager_manually_locked(const pz_mod_manager* manager,
                                   uint64_t workshop_id);
int pz_mod_manager_server_locked(pz_mod_manager* manager, uint64_t workshop_id);
int pz_mod_manager_locked(pz_mod_manager* manager, uint64_t workshop_id);
void pz_mod_manager_lock_reason(pz_mod_manager* manager, uint64_t workshop_id,
                                pz_str* out);

void pz_mod_manager_list(const pz_mod_manager* manager, pz_vec* out);
void pz_mod_manager_list_all(pz_mod_manager* manager, pz_vec* out);
void pz_mod_manager_activation_targets(pz_mod_manager* manager, pz_vec* out);
void pz_mod_manager_mod_ids_for(pz_mod_manager* manager, uint64_t workshop_id,
                                pz_vec* out);
void pz_mod_manager_workshop_ids_for(pz_mod_manager* manager,
                                     const pz_activation_target_data* target,
                                     pz_vec* out);
void pz_mod_manager_unmapped_mod_ids(pz_mod_manager* manager,
                                     const pz_activation_target_data* target,
                                     pz_vec* out);
void pz_mod_manager_enabled_mod_names(pz_mod_manager* manager,
                                      const pz_activation_target_data* target,
                                      pz_vec* out);
int pz_mod_manager_preferred_mod_id(pz_mod_manager* manager,
                                    uint64_t workshop_id, pz_str* out);
void pz_mod_manager_missing_requirements(pz_mod_manager* manager, pz_vec* out);

void pz_mod_manager_free_mods(pz_vec* mods);
void pz_mod_manager_free_targets(pz_vec* targets);
void pz_mod_manager_free_requirements(pz_vec* requirements);

pz_mm_result pz_mod_manager_add(pz_mod_manager* manager, const char* input,
                                size_t input_length, const pz_log* log);
pz_mm_result pz_mod_manager_import(pz_mod_manager* manager,
                                   const uint64_t* only_ids, size_t only_count,
                                   const pz_log* log);
pz_mm_result pz_mod_manager_set_zomboid_dir(pz_mod_manager* manager,
                                            const char* path, size_t length);
pz_mm_result pz_mod_manager_remove(pz_mod_manager* manager, uint64_t workshop_id,
                                   const pz_log* log);
pz_mm_result pz_mod_manager_update_one(pz_mod_manager* manager,
                                       uint64_t workshop_id, const pz_log* log);
pz_mm_result pz_mod_manager_update_all(pz_mod_manager* manager, const pz_log* log);
pz_mm_result pz_mod_manager_check_updates(pz_mod_manager* manager,
                                          const pz_log* log);
pz_mm_result pz_mod_manager_check_app_release(pz_mod_manager* manager,
                                              const pz_log* log);
pz_mm_result pz_mod_manager_set_locked(pz_mod_manager* manager,
                                       const uint64_t* workshop_ids, size_t count,
                                       int locked, const pz_log* log);
pz_mm_result pz_mod_manager_set_target_locked(pz_mod_manager* manager,
                                              int kind, const char* name,
                                              size_t name_length, int locked,
                                              const pz_log* log);
pz_mm_result pz_mod_manager_set_mods_enabled(pz_mod_manager* manager, int kind,
                                             const char* name, size_t name_length,
                                             const uint64_t* workshop_ids,
                                             size_t count, int enabled,
                                             const pz_log* log);
pz_mm_result pz_mod_manager_set_selection_enabled(
    pz_mod_manager* manager, int kind, const char* name, size_t name_length,
    const uint64_t* workshop_ids, size_t count, const char* const* folders,
    const size_t* folder_lengths, size_t folder_count, int enabled,
    const pz_log* log);

#ifdef __cplusplus
}
#endif

#endif
