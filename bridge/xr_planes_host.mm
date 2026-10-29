// SPDX-License-Identifier: MIT
// Host check of Apple's extended-range render targets through the frontend (XrTargetsCheck.h), against the host's own Metal: the
// bridge's bytes must match Metal's, and Metal's must still be the ones the plugin's on-device check expects.
// usage: xr_planes_host LOADER CACHE
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../NativeBridgeMetalFrontend.h"
#include "../XrTargetsCheck.h"

extern "C" {volatile uint32_t native_prime_compiling, native_prime_libraries_done, native_prime_libraries_total, native_prime_made, native_prime_rate;}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        id<MTLDevice> host = MTLCreateSystemDefaultDevice();
        id<MTLDevice> bridge = native_bridge_metal_open(argv[1], argv[2], 0x6e6d62, nullptr);
        if (!host || !bridge) {std::printf("FAIL device\n"); return 1;}
        int failed = 0;
        for (int f = 0; f < 5; ++f) {
            NSString *hostError = nil;
            const std::vector<uint8_t> want = xrcheck::render(host, xrcheck::kFormats[f], &hostError);
            if (want.empty()) {std::printf("FAIL format %lu: host %s\n", (unsigned long)xrcheck::kFormats[f], hostError.UTF8String); ++failed; continue;}
            for (int p = 0; p < 4; ++p)
                if (xrcheck::distance(xrcheck::word(want, p), xrcheck::kExpected[f][p]) > 2) {
                    std::printf("FAIL format %lu point %d: Metal gave %08x, the plugin expects %08x\n", (unsigned long)xrcheck::kFormats[f], p,
                                xrcheck::word(want, p), xrcheck::kExpected[f][p]);
                    ++failed;
                }
        }
        failed += xrcheck::checkAgainstExpected(bridge, [](const char *text) {std::printf("  %s\n", text);});
        std::printf(failed ? "FAIL extended-range targets: %d differences\n" : "PASS extended-range targets match Metal\n", failed);
        return failed ? 1 : 0;
    }
}
