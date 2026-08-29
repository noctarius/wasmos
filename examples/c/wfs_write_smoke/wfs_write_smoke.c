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
 * Every step here exists because the piece it covers was previously HOST-TESTED
 * ONLY. A phase is not finished when its unit suites pass; it is finished when it
 * works through the OS, and these are the steps that make that true for
 * allocation, truncation and the namespace.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
    struct stat st;
    long before;
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

    /* 4. O_CREAT makes a new file, and O_CREAT on one that already exists OPENS it
     *    rather than failing -- which is what O_CREAT without O_EXCL means, and a
     *    choice worth pinning because the driver could plausibly have refused. */
    fd = open("/wfs/created.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail("wfs-write-smoke: O_CREAT failed");
    }
    n = write(fd, "created", 7);
    (void)close(fd);
    if (n != 7) {
        return fail("wfs-write-smoke: write to a created file short");
    }
    if (stat("/wfs/created.txt", &st) != 0 || st.st_size != 7) {
        return fail("wfs-write-smoke: a created file has the wrong size");
    }
    fd = open("/wfs/created.txt", O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        return fail("wfs-write-smoke: O_CREAT on an existing file failed");
    }
    (void)close(fd);
    /* And O_CREAT|O_TRUNC on the existing file empties it, which is the create
     *    path and the truncate path meeting. */
    fd = open("/wfs/created.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail("wfs-write-smoke: O_CREAT|O_TRUNC on an existing file failed");
    }
    (void)close(fd);
    if (stat("/wfs/created.txt", &st) != 0 || st.st_size != 0) {
        return fail("wfs-write-smoke: O_TRUNC did not empty the created file");
    }
    if (unlink("/wfs/created.txt") != 0) {
        return fail("wfs-write-smoke: unlink of the created file failed");
    }

    /* 5. APPENDING past the end, which is the only way a guest reaches the block
     *    ALLOCATOR: every earlier write patched inline bytes or overwrote blocks
     *    that already existed. */
    /* The file's size is READ, not assumed: hardcoding it ties this app to one
     * fixture, and the number that mattered here came from a different one. */
    if (stat("/wfs/docs/big.txt", &st) != 0 || st.st_size <= 0) {
        return fail("wfs-write-smoke: stat before append failed");
    }
    before = st.st_size;

    fd = open("/wfs/docs/big.txt", O_WRONLY | O_APPEND);
    if (fd < 0) {
        return fail("wfs-write-smoke: append open failed");
    }
    for (i = 0; i < 64u; ++i) {
        want[i] = pattern(i + 200u);
    }
    n = write(fd, want, 64);
    (void)close(fd);
    if (n != 64) {
        return fail("wfs-write-smoke: append short");
    }
    if (stat("/wfs/docs/big.txt", &st) != 0 || st.st_size != before + 64) {
        return fail("wfs-write-smoke: append did not grow the file");
    }
    fd = open("/wfs/docs/big.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: append reopen failed");
    }
    if (lseek(fd, before, SEEK_SET) != before) {
        (void)close(fd);
        return fail("wfs-write-smoke: append reseek failed");
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 64);
    (void)close(fd);
    if (n != 64 || memcmp(buf, want, 64) != 0) {
        return fail("wfs-write-smoke: append verify failed");
    }

    /* 6. O_TRUNC, which is how a guest reaches TRUNCATION. The file must come back
     *    at zero length, and the blocks it held must have been released -- which
     *    the create in step 8 depends on if the volume is tight. */
    fd = open("/wfs/etc/wfs.conf", O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return fail("wfs-write-smoke: trunc open failed");
    }
    (void)close(fd);
    if (stat("/wfs/etc/wfs.conf", &st) != 0 || st.st_size != 0) {
        return fail("wfs-write-smoke: O_TRUNC did not empty the file");
    }

    /* 7. mkdir, and a file created inside it: a directory that cannot be written
     *    into is not usable, so the nested create is the real check. */
    if (mkdir("/wfs/made", 0755) != 0) {
        return fail("wfs-write-smoke: mkdir failed");
    }
    fd = open("/wfs/made/inner.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail("wfs-write-smoke: nested create failed");
    }
    n = write(fd, "nested", 6);
    (void)close(fd);
    if (n != 6) {
        return fail("wfs-write-smoke: nested write short");
    }
    fd = open("/wfs/made/inner.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: nested reopen failed");
    }
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, 6);
    (void)close(fd);
    if (n != 6 || memcmp(buf, "nested", 6) != 0) {
        return fail("wfs-write-smoke: nested verify failed");
    }

    /* 8. rename, then unlink and rmdir, which must leave the tree as it was. An
     *    rmdir of a directory that still holds an entry has to be REFUSED. */
    if (rename("/wfs/made/inner.txt", "/wfs/made/moved.txt") != 0) {
        return fail("wfs-write-smoke: rename failed");
    }
    if (open("/wfs/made/inner.txt", O_RDONLY) >= 0) {
        return fail("wfs-write-smoke: the old name still opens");
    }
    fd = open("/wfs/made/moved.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: the new name does not open");
    }
    (void)close(fd);

    if (rmdir("/wfs/made") == 0) {
        return fail("wfs-write-smoke: rmdir accepted a populated directory");
    }
    if (unlink("/wfs/made/moved.txt") != 0) {
        return fail("wfs-write-smoke: unlink failed");
    }
    if (open("/wfs/made/moved.txt", O_RDONLY) >= 0) {
        return fail("wfs-write-smoke: an unlinked file still opens");
    }
    if (rmdir("/wfs/made") != 0) {
        return fail("wfs-write-smoke: rmdir of an empty directory failed");
    }
    /* 9. A file that OUTGROWS its inline area and then its inline extent map, in
     *    the guest, on a real device. A new file is created inline, so the first
     *    write past the record's capacity has to PROMOTE it to an extent map; the
     *    sparse runs below then take it past six extents and into an extent TREE.
     *    Both promotions used to be refused, which capped a file created in the OS
     *    at 144 bytes.
     *
     *    Unlinking it at the end is the other half: releasing a tree walks its
     *    leaf, and without that the file would be undeletable and its blocks
     *    unreclaimable. */
    fd = open("/wfs/grown.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return fail("wfs-write-smoke: grown open failed");
    }
    for (i = 0; i < sizeof(buf); ++i) {
        buf[i] = pattern(i + 71u);
    }
    /* 400 bytes is past WFS_INLINE_DATA_MAX (144), so the object is promoted off
     * its inline data before a single block is written. */
    if (write(fd, buf, 400) != 400) {
        return fail("wfs-write-smoke: the promoting write was short");
    }
    /* Seven runs far enough apart to be seven separate extents. The seventh is
     * the one the record cannot hold. */
    for (i = 1u; i <= 7u; ++i) {
        long at = (long)(i * 40960u);

        if (lseek(fd, at, SEEK_SET) != at) {
            return fail("wfs-write-smoke: seek to a sparse offset failed");
        }
        if (write(fd, buf, 64) != 64) {
            return fail("wfs-write-smoke: a sparse run was short");
        }
    }
    if (close(fd) != 0) {
        return fail("wfs-write-smoke: grown close failed");
    }

    /* Every run reads back through the map it ended up with, and the ranges
     * between them are holes that read as zeroes. */
    fd = open("/wfs/grown.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: grown reopen failed");
    }
    memset(want, 0, sizeof(want));
    if (read(fd, want, 400) != 400) {
        return fail("wfs-write-smoke: the promoted content was short");
    }
    if (memcmp(want, buf, 400) != 0) {
        return fail("wfs-write-smoke: the promoted content does not match");
    }
    for (i = 1u; i <= 7u; ++i) {
        long at = (long)(i * 40960u);

        if (lseek(fd, at, SEEK_SET) != at) {
            return fail("wfs-write-smoke: seek to verify a sparse run failed");
        }
        memset(want, 0, 64);
        if (read(fd, want, 64) != 64) {
            return fail("wfs-write-smoke: a sparse run read short");
        }
        if (memcmp(want, buf, 64) != 0) {
            return fail("wfs-write-smoke: a sparse run does not match");
        }
    }
    /* A hole: nothing maps it, so it must read as zeroes rather than as a
     * neighbouring run's bytes. */
    if (lseek(fd, 20480, SEEK_SET) != 20480) {
        return fail("wfs-write-smoke: seek to a hole failed");
    }
    memset(want, 0xAA, 64);
    if (read(fd, want, 64) != 64) {
        return fail("wfs-write-smoke: a hole read short");
    }
    for (i = 0; i < 64u; ++i) {
        if (want[i] != 0) {
            return fail("wfs-write-smoke: a hole did not read as zeroes");
        }
    }
    (void)close(fd);

    if (unlink("/wfs/grown.txt") != 0) {
        return fail("wfs-write-smoke: unlinking a tree-mapped file failed");
    }
    if (open("/wfs/grown.txt", O_RDONLY) >= 0) {
        return fail("wfs-write-smoke: the unlinked grown file still opens");
    }

    /* And the volume is back to what it was, with the untouched entries intact. */
    fd = open("/wfs/hello.txt", O_RDONLY);
    if (fd < 0) {
        return fail("wfs-write-smoke: an untouched file stopped opening");
    }
    (void)close(fd);

    puts("wfs-write-smoke: ok");
    return 0;
}
