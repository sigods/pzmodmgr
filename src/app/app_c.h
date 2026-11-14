#ifndef PZ_APP_APP_C_H
#define PZ_APP_APP_C_H

#ifdef __cplusplus
extern "C" {
#endif

/* Shared body of the Linux and Windows entry points. Runs the updater helper,
 * the CLI, or the native UI, and returns the process exit code. */
int pz_app_run(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif
