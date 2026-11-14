#ifndef PZ_CORE_PROCESS_C_H
#define PZ_CORE_PROCESS_C_H

/* C99 process and shell boundary.
 *
 * All command quoting, captured output, and detached child creation live here.
 * Callers build an already-quoted command line for pz_process_capture(); on
 * Windows that function adds the `cmd.exe /c "..."` wrapper itself, so callers
 * must not add their own outer quotes. Log lines from captured output are
 * complete lines without the trailing CR/LF.
 */

#include "core/c_util.h"
#include "core/fs_c.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Quotes one shell argument. POSIX escapes single quotes; Windows wraps the
 * argument in double quotes, matching the old steamcmd/updater callers.
 * Returns 1 on success. */
int pz_process_quote(pz_str* out, const char* value, size_t length);

/* Runs command through the platform shell (cmd.exe /c on Windows, popen on
 * POSIX) and replaces output with the combined stdout/stderr text. When log is
 * enabled every complete line is also reported as it arrives.
 *
 * Returns 1 when the child ran and every captured byte was stored; 0 when it
 * could not be started or an allocation failed. exit_code receives the shell's
 * status (compare against 0 for success), or -1 when the child did not run.
 * output still holds whatever was captured, including on failure. */
int pz_process_capture(const char* command, size_t length, pz_str* output,
                       const pz_log* log, int* exit_code);

/* Starts exe with args without a shell and with stdio connected to NUL
 * (/dev/null on POSIX). Does not wait. Returns 1 on success. */
int pz_process_start_detached(const pz_path* exe, const pz_str* args,
                              size_t arg_count, unsigned long* pid);

/* Waits for pid to exit. pid 0 succeeds immediately. Returns 1 when the
 * process is gone. */
int pz_process_wait(unsigned long pid);

int pz_process_is_running(unsigned long pid);
unsigned long pz_process_current_id(void);

#ifdef __cplusplus
}
#endif

#endif
