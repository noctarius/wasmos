/* aot_probe - the fixture behind tests/test_warp_aot.py.
 *
 * One module packed three ways (examples/c/BUILD.py): "aot-fat", signed, with a
 * WARP AOT binary for every architecture; "aot-foreign", signed, with binaries
 * only for the architectures other than the tree's; "aot-unsigned", unsigned,
 * with the tree's own. Each must run on every architecture: the fat package
 * from its own architecture's binary, the other two from their bytecode,
 * because the kernel executes neither a binary tagged for another architecture
 * nor one from a package it does not trust. The loop gives the compiled code
 * something to compute; the printed sum is checked by the test. */
#include "stdio.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    volatile unsigned limit = 100u;
    unsigned sum = 0;
    for (unsigned i = 1; i <= limit; ++i) {
        sum += i;
    }
    printf("[test] aot-probe ok sum=%u\n", sum);
    return 0;
}
