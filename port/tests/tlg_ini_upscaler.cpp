// SPDX-License-Identifier: GPL-2.0-or-later
// Prints the upscaler the game's settings loader (TlgSettings::Load) reads from the one tlg.ini given, before any card
// check (one file per run: the loader keeps state from the file it read, as the game reads one at start). tests/test_backend_aa_env.py compares it with the launcher's copy of the rule
// (tlg_run.loaded_upscaler): ninja -C out/gpu tlg-ini-upscaler.
#include <cstdio>
#include <cstdlib>
#include "tlg_settings.h"

#ifdef _WIN32
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

int main(int argc, char** argv) {
    using namespace TlgSettings;
    if (argc != 2) {
        std::fprintf(stderr, "usage: tlg-ini-upscaler TLG_INI\n");
        return 2;
    }
    unsetenv("TLG_UPSCALER");
    unsetenv("TLG_DLAA");
    setenv("TLG_CONFIG", argv[1], 1);
    Load();
    std::printf("upscaler=%s\n", UpscalerName(Get().upscaler));
    return 0;
}
