/* fs_leave_open - test fixture: open a file and exit with it still open.
 *
 * Usage: fs_leave_open <path>
 *
 * Opens <path> for append, creating it if it does not exist, and returns
 * without calling fclose. libc does not close files at exit, so the open file
 * outlives the process on the backend that serves it until fs-manager releases
 * it on the process's hangup. tests/test_vfs_root_mount.py runs it more times
 * than a backend has open-file slots to show that release happens.
 *
 * Prints "fs-leave-open: left open" and exits 0, or "fs-leave-open: open
 * failed" and exits 1. */
#include "stdio.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        puts("fs-leave-open: usage: fs_leave_open <path>");
        return 1;
    }
    if (!fopen(argv[1], "a")) {
        puts("fs-leave-open: open failed");
        return 1;
    }
    puts("fs-leave-open: left open");
    return 0;
}
