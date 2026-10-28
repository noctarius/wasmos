/* fs_leave_open - test fixture: open a file and exit with it still open.
 *
 * Usage: fs_leave_open <path> [count]
 *
 * Opens <path> for append `count` times (default 1), creating it if it does not
 * exist, and returns without calling fclose. libc does not close files at exit,
 * so the open files outlive the process on the backend that serves them until
 * fs-manager releases them on the process's hangup. tests/test_vfs_root_mount.py
 * leaves more files open, across a few processes, than a backend has open-file
 * slots to show that release happens.
 *
 * Prints "fs-leave-open: left open" and exits 0, or "fs-leave-open: open
 * failed" and exits 1. */
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char** argv) {
    int count = 1;
    if (argc < 2) {
        puts("fs-leave-open: usage: fs_leave_open <path> [count]");
        return 1;
    }
    if (argc >= 3) {
        count = atoi(argv[2]);
    }
    for (int i = 0; i < count; ++i) {
        if (!fopen(argv[1], "a")) {
            puts("fs-leave-open: open failed");
            return 1;
        }
    }
    puts("fs-leave-open: left open");
    return 0;
}
