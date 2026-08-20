/* sdk_hello - the toolchain's own smoke app: what `wasmos-clang hello.c -o hello`
 * has to produce.
 *
 * Deliberately plain C with no WASMOS-specific code and, unlike every other app
 * in the tree, no linker.metadata beside it. It is built by the staged SDK
 * (cmake/WasmosSdk.cmake) with no flags but -o, so it exercises the parts a
 * developer never sees: the sysroot, crt1.o, libc.a, the wasm linker defaults,
 * and the default manifest the driver falls back to when the developer supplies
 * none.
 *
 * If this stops running, the SDK's zero-configuration path is broken even when
 * the in-tree build is green -- the two use different link lines. */
#include <stdio.h>

int main(void) {
    printf("Hello WASMOS from the SDK!\n");
    return 0;
}
