/* sdk_hello - the toolchain's own smoke app: what `wasmos-clang hello.c -o hello`
 * has to produce.
 *
 * Deliberately plain C -- stdio, argv, and open/read -- with no WASMOS-specific
 * call, and, unlike every other app in the tree, no linker.metadata beside it. It
 * is built by the staged SDK (cmake/WasmosSdk.cmake) with no flags but -o, so it
 * exercises the parts a developer never sees: the sysroot, crt1.o, libc.a, the
 * wasm linker defaults, and the default manifest the driver falls back to when
 * the developer supplies none.
 *
 * The three things it prints are three separate claims about the SDK, in
 * increasing depth:
 *   - printf reaches the console (crt1 + libc + the console host call);
 *   - argv is real (crt1 tokenizes the spawn-info argument string);
 *   - open/read reach the filesystem SERVICE over IPC (libc's managed-IPC path,
 *     which is where a broken sysroot header or a missing archive object shows
 *     up rather than at the link step).
 *
 * If this stops running, the SDK's zero-configuration path is broken even when
 * the in-tree build is green -- the two use different link lines. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

/* A file the boot filesystem always carries, used when no path is given so the
 * filesystem claim is tested on a bare `sdkhello` too. */
#define SDK_HELLO_DEFAULT_PATH "/boot/system/net/interfaces"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : SDK_HELLO_DEFAULT_PATH;
    char head[64];
    int fd;
    int i;

    printf("Hello WASMOS from the SDK!\n");

    printf("argc=%d", argc);
    for (i = 1; i < argc; ++i) {
        printf(" argv[%d]=%s", i, argv[i]);
    }
    printf("\n");

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("fs: open %s failed (%d)\n", path, fd);
        return 1;
    }
    int n = (int)read(fd, head, sizeof(head) - 1);
    close(fd);
    if (n < 0) {
        printf("fs: read failed (%d)\n", n);
        return 1;
    }
    head[n] = '\0';
    for (i = 0; i < n; ++i) {
        if (head[i] == '\n' || head[i] == '\r') {
            head[i] = '\0';
            break;
        }
    }
    printf("fs: read %d bytes from %s: %s\n", n, path, head);
    return 0;
}
