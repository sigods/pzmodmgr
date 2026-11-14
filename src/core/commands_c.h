#ifndef PZ_CORE_COMMANDS_C_H
#define PZ_CORE_COMMANDS_C_H

/* C99 command parser shared by the CLI and both native UIs.
 *
 * The parser owns no manager state; it calls pz_mod_manager_* directly and
 * returns a pz_mm_result whose message the caller frees with
 * pz_mm_result_free(). */

#include "core/mod_manager_c.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

pz_mm_result pz_command_run(pz_mod_manager* manager, const char* line,
                            size_t length, const pz_log* log);

#ifdef __cplusplus
}
#endif

#endif
