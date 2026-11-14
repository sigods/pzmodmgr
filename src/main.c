#include "app/app_c.h"

#if !defined(_WIN32)
int main(int argc, char** argv) {
    return pz_app_run(argc, argv);
}
#endif
