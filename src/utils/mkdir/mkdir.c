/* mkdir.c - create directories.
 *
 *   mkdir [-p] <dir>...
 *
 * Without -p a directory is created in a parent that must already exist, and an
 * existing target is an error. With -p every missing ancestor is created and an
 * existing directory is not an error, so `mkdir -p /a/b/c` is idempotent and
 * needs no prior knowledge of which prefix is already there.
 *
 * The work is one FS_IPC_MKDIR_REQ per component via libc's mkdir(); this tool
 * adds only the argument parsing and the walk. fs-manager routes each component
 * independently, so a -p walk that crosses a mount point creates the rest of the
 * path inside the mounted filesystem rather than in the one covering it.
 */
#include <stdint.h>

#include "stdio.h"
#include "string.h"
#include "sys/stat.h"
#include "wasmos/api.h"
#include "wasmos/startup.h"

/* Longest argument string the process manager delivers (WASMOS_STARTUP_ARGS_MAX
 * truncates at 255), plus its NUL. */
#define MKDIR_ARGS_MAX 256
/* Longest single operand. fs-manager refuses a longer path than its own cwd
 * ceiling anyway, so a bigger buffer here would only defer the refusal. */
#define MKDIR_PATH_MAX 256

/* Exit codes. Distinct so a caller can tell a usage mistake from a filesystem
 * refusal; the packed reason from the FS is printed rather than returned,
 * because a process exit status is a byte and cannot carry one. */
#define MKDIR_EXIT_OK 0
#define MKDIR_EXIT_USAGE 1
#define MKDIR_EXIT_FAILED 2

static void usage(void) {
    puts("usage: mkdir [-p] <dir>...");
}

/* Copy the next whitespace-delimited token out of `args` starting at *pos,
 * advancing *pos past it. Returns 0 when there is no token left. */
static int next_token(const char* args, uint32_t* pos, char* out, uint32_t out_cap) {
    uint32_t i = *pos;
    uint32_t n = 0;

    while (args[i] == ' ' || args[i] == '\t') {
        i++;
    }
    if (args[i] == '\0') {
        *pos = i;
        return 0;
    }
    while (args[i] != '\0' && args[i] != ' ' && args[i] != '\t') {
        if (n + 1u >= out_cap) {
            /* Refuse rather than truncate: a shortened path names a different
             * directory, which is not the one the caller asked to create. */
            *pos = i;
            return -1;
        }
        out[n++] = args[i++];
    }
    out[n] = '\0';
    *pos = i;
    return 1;
}

/* Whether `rc` from mkdir() means the directory is already there.
 *
 * For -p that is success: the point of the flag is that a prefix which already
 * exists is not an obstacle. Without it, it is the error the caller wants told. */
static int is_exists(int rc) {
    return rc == (int)WASMOS_ERR_FS_EXISTS;
}

static void report(const char* path, int rc) {
    printf("mkdir: %s: %s\n", path, wasmos_strerror((wasmos_error_code_t)rc));
}

/* Create every missing component of `path`, shallowest first.
 *
 * Each prefix is created in turn, so the walk needs no lookup: an existing
 * component answers EXISTS and is stepped over. A trailing slash is harmless
 * because the final component is created by the same step that would have
 * handled it.
 *
 * Returns 0 once the whole path exists, or the packed reason it does not. */
static int mkdir_parents(char* path) {
    uint32_t i = 0;

    /* A leading slash is the root, which always exists and is nobody's to
     * create; start after it. */
    if (path[0] == '/') {
        i = 1;
    }
    for (;;) {
        char saved;
        int rc;

        while (path[i] != '\0' && path[i] != '/') {
            i++;
        }
        if (i == 0) {
            return 0;
        }
        saved = path[i];
        path[i] = '\0';
        rc = mkdir(path, 0755);
        path[i] = saved;
        if (rc != 0 && !is_exists(rc)) {
            return rc;
        }
        if (saved == '\0') {
            return 0;
        }
        /* Step over the separator, and over a run of them: "//" names the same
         * directory as "/" and there is nothing between them to create. */
        while (path[i] == '/') {
            i++;
        }
        if (path[i] == '\0') {
            return 0;
        }
    }
}

int main(void) {
    char args[MKDIR_ARGS_MAX];
    char path[MKDIR_PATH_MAX];
    uint32_t pos = 0;
    int parents = 0;
    int operands = 0;
    int status = MKDIR_EXIT_OK;
    int token;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));

    /* Flags first, then operands. Only -p is accepted, and it is accepted only
     * before the first operand, so a directory literally named "-p" stays
     * reachable as "./-p" rather than being silently taken for a flag. */
    while ((token = next_token(args, &pos, path, sizeof(path))) == 1) {
        if (operands == 0 && strcmp(path, "-p") == 0) {
            parents = 1;
            continue;
        }
        if (path[0] == '-' && path[1] != '\0') {
            printf("mkdir: unknown option: %s\n", path);
            usage();
            return MKDIR_EXIT_USAGE;
        }
        operands++;
        if (parents) {
            int rc = mkdir_parents(path);
            if (rc != 0) {
                report(path, rc);
                status = MKDIR_EXIT_FAILED;
            }
        } else {
            int rc = mkdir(path, 0755);
            if (rc != 0) {
                report(path, rc);
                status = MKDIR_EXIT_FAILED;
            }
        }
    }
    if (token < 0) {
        puts("mkdir: path too long");
        return MKDIR_EXIT_USAGE;
    }
    if (operands == 0) {
        usage();
        return MKDIR_EXIT_USAGE;
    }
    return status;
}
