/* wfs_write_smoke - the WFS write path, exercised from a guest process.
 *
 * The host suites cover wfs_write.c and wfs_truncate.c as tasks. This covers the
 * part they cannot: that the driver's FS IPC dispatch reaches them at all, that a
 * write travels through fs-manager's routing and the client transfer buffer, and
 * that what lands is what a separate open reads back.
 *
 * Everything goes through plain libc calls, so the ABI under test is the one an
 * application actually uses.
 *
 * Preconditions: /wfs must be mounted with the volume mkfs_wfs built --
 * hello.txt stored INLINE (small enough to live in its object record) and
 * docs/big.txt spanning blocks. Both are rewritten, so the app is not idempotent
 * against a volume something else is also checking.
 *
 * Prints "wfs-write-smoke: ok" and exits 0, or a step-specific line and 1.
 *
 * What is deliberately NOT here: O_CREAT, unlink, mkdir and rename. Those need a
 * directory-record writer, which phase 2 did not build, and the driver refuses
 * them rather than pretending -- so a test of them would be a test of the
 * refusal, which the host suites already cover.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Position-dependent, so a write that lands shifted or short fails rather than
 * matching. */
static unsigned char pattern(unsigned int i) {
    return (unsigned char)(0x40u + ((i * 7u) & 0x3Fu));
}

static int fail(const char* what) {
    puts(what);
    return 1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    unsigned char buf[512];
    unsigned char want[512];
    unsigned int i;
    int fd;
    long n;

    /* 1. An INLINE file: the bytes live in the object record, so this exercises
     *    the path that touches no data block at all. */
    for (i = 0; i < 16u; ++i) {
        want[i] = pattern(i);
    }
    fd = open("/wfs/hello.txt", O_WRONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: inline open failed");
    }
    n = write(fd, want, 16);
    if (n != 16) {
        (void)close(fd);
        return fail("wfs-write-smoke: inline write short");
    }
    if (close(fd) != 0) {
        return fail("wfs-write-smoke: inline close failed");
    }

    fd = open("/wfs/hello.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: inline reopen failed");
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 16);
    (void)close(fd);
    if (n != 16 || memcmp(buf, want, 16) != 0) {
        return fail("wfs-write-smoke: inline verify failed");
    }

    /* 2. A file with extents, written across a block boundary. 4090 puts six
     *    bytes in the first block and the rest in the second, so a write that
     *    resolved the extent map only once fails here. */
    for (i = 0; i < 64u; ++i) {
        want[i] = pattern(i + 100u);
    }
    fd = open("/wfs/docs/big.txt", O_WRONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: extent open failed");
    }
    if (lseek(fd, 4090, SEEK_SET) != 4090) {
        (void)close(fd);
        return fail("wfs-write-smoke: extent seek failed");
    }
    n = write(fd, want, 64);
    if (n != 64) {
        (void)close(fd);
        return fail("wfs-write-smoke: extent write short");
    }
    (void)close(fd);

    fd = open("/wfs/docs/big.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: extent reopen failed");
    }
    if (lseek(fd, 4090, SEEK_SET) != 4090) {
        (void)close(fd);
        return fail("wfs-write-smoke: extent reseek failed");
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 64);
    (void)close(fd);
    if (n != 64 || memcmp(buf, want, 64) != 0) {
        return fail("wfs-write-smoke: extent verify failed");
    }

    /* 3. A read-only fd must refuse a write. This is an fd-MODE violation, and it
     *    is the check that stops a client writing through a descriptor it only
     *    asked to read. */
    fd = open("/wfs/hello.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: mode open failed");
    }
    n = write(fd, want, 4);
    (void)close(fd);
    if (n > 0) {
        return fail("wfs-write-smoke: a read-only fd accepted a write");
    }

    /* 4. O_CREAT is refused, not silently satisfied by an existing file: there is
     *    no directory-record writer, and a create that appeared to work would be
     *    worse than one that fails. */
    fd = open("/wfs/created.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)close(fd);
        return fail("wfs-write-smoke: O_CREAT unexpectedly succeeded");
    }

    puts("wfs-write-smoke: ok");
    return 0;
}
