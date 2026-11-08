#include "core/commands.hpp"
#include "core/paths.hpp"
#include "core/updater.hpp"
#include "ui/ui.hpp"
#include <cstdio>

namespace pz {
// Shared body of both entry points: main() on Linux, wWinMain() on Windows.
// Windows links pzmodmgr as a GUI-subsystem program (no console window), so the
// command line is decoded there and handed over as a narrow argv.
int run_app(int argc, char** argv) {
    // The self-updater re-launches a copy of this executable with this flag.
    // It waits for the current process, swaps the installation atomically, and
    // restarts the new build without loading the UI or the mod database.
    if (is_update_helper_invocation(argc, argv)) return run_update_helper(argc, argv);

    Config cfg = default_config();
    load_config(cfg);
    cleanup_update_cache(cfg);
    ModManager mm(cfg);

    if (argc > 1) {  // CLI mode: pzmodmgr add https://steamcommunity.com/...?id=123
        std::string line;
        for (int i = 1; i < argc; ++i) {
            if (i > 1) line += ' ';
            line += argv[i];
        }
        auto r = run_command(mm, line, [](const std::string& s) {
            std::fputs(s.c_str(), stdout);
            std::fputc('\n', stdout);
        });
        std::fprintf(stdout, "%s%s\n", r.ok ? "OK: " : "ERROR: ", r.message.c_str());
        return r.ok ? 0 : 1;
    }
    return run_ui(mm);   // GUI mode
}
}  // namespace pz

#if !defined(_WIN32)
int main(int argc, char** argv) { return pz::run_app(argc, argv); }
#endif
