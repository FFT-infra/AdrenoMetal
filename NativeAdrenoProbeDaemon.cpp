// SPDX-License-Identifier: MIT
// Adreno probe daemon. The launchd job that names it is never loaded: launchd takes its system jobs from the signed cache in
// /System/Library/xpc/launchd.plist, so the owner plists the addon mounts over /System/Library/LaunchDaemons are inert (lx1,
// 2026-11-09). The owner root channel is the init trampoline instead (NativeInitTrampoline.cpp, patches/xnu/native-init-trampoline.patch).
// This binary is kept because a launchd that does load owner jobs, on another OS build, starts it; it runs the LaunchServices repair
// (NativeInstallFix.cpp), waits for the Adreno accelerator service, runs the shared userspace GPU test once, writes the system log for
// the UFS log export (NativeOsLogExport.mm) and exits.
#include <stdlib.h>
#include "NativeAdrenoUserTest.h"

extern "C" void nativeOsLogExport();
extern "C" int nativeInstallFix(int);

using MatchingFn = void *(*)(const char *);
using GetServiceFn = unsigned (*)(unsigned, void *);

int main() {
    nativeInstallFix(0);
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
    if (!iokit) return 1;
    auto matching = (MatchingFn)dlsym(iokit, "IOServiceMatching");
    auto getService = (GetServiceFn)dlsym(iokit, "IOServiceGetMatchingService");
    if (!matching || !getService) return 2;
    // The service is published a few seconds into boot (after the GPU is powered); poll for up to two minutes.
    unsigned port = 0;
    for (unsigned i = 0; i < 240 && !port; ++i) {
        port = getService(0, matching("IOAcceleratorES"));
        if (!port) usleep(500000);
    }
    const int status = !port ? 3 : adrenoUserTest(port, "adrenoprobed") >= 0 ? 0 : 4;
    nativeOsLogExport();
    return status;
}
