#include "core/commands.hpp"
#include "core/paths.hpp"
#include "ui/ui.hpp"
#include <iostream>

int main(int argc, char** argv) {
    pz::Config cfg = pz::default_config();
    pz::load_config(cfg);
    pz::ModManager mm(cfg);

    if (argc > 1) {  // CLI mode: pzmodmgr add https://steamcommunity.com/...?id=123
        std::string line;
        for (int i = 1; i < argc; ++i) {
            if (i > 1) line += ' ';
            line += argv[i];
        }
        auto r = pz::run_command(mm, line, [](const std::string& s) { std::cout << s << "\n"; });
        std::cout << (r.ok ? "OK: " : "ERROR: ") << r.message << "\n";
        return r.ok ? 0 : 1;
    }
    return pz::run_ui(mm);   // GUI mode
}
