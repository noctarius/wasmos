/* Host tests for the .rc script runner (src/libc/src/script.c, contract in
 * wasmos/script.h): the step API a host uses to wait for a command without
 * blocking, and wasmos_script_run, which is now a synchronous loop over it.
 *
 * script.c is compiled for the host and reads real files, which each case
 * writes under /tmp with a fixed prefix and main() removes at the end. The hooks only record what
 * they were asked to do; a case decides whether a hook completes at once or goes pending.
 */
#include <stdio.h>
#include <string.h>

#include "wasmos/script.h"
#include "test_shuffle.h"

/* Every script file is RC_PREFIX<name>; nested `script`/`source` lines name
 * their files the same way. g_written lists them for removal. */
#define RC_PREFIX "/tmp/wasmos_script_runner_test_"

/* Standard C, declared here because the in-tree stdio.h -- which a tool reading
 * this file with the guest include path picks up -- does not declare it. The
 * test binary links the host libc's. */
int remove(const char* path);
static const char* g_written[16];
static int g_written_n;

/* Every hook call, as "<hook>:<argument>" lines. */
static char g_log[4096];
static int g_pending_mask; /* bit per hook: 1 start, 2 spawn, 4 exec, 8 wait */
static int g_start_rc;
static int32_t g_exec_code;

static void log_call(const char* hook, const char* arg) {
    size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof(g_log) - n, "%s:%s\n", hook, arg ? arg : "");
}

static int hook_start(void* user, const char* path) {
    (void)user;
    log_call("start", path);
    return (g_pending_mask & 1) ? WASMOS_SCRIPT_HOOK_PENDING : g_start_rc;
}
static int hook_spawn(void* user, const char* path) {
    (void)user;
    log_call("spawn", path);
    return (g_pending_mask & 2) ? WASMOS_SCRIPT_HOOK_PENDING : 0;
}
static int hook_exec(void* user, const char* path, const char* args, int32_t* out) {
    (void)user;
    (void)args;
    log_call("exec", path);
    if (g_pending_mask & 4) {
        return WASMOS_SCRIPT_HOOK_PENDING;
    }
    *out = g_exec_code;
    return 0;
}
static int hook_wait(void* user, const char* name) {
    (void)user;
    log_call("wait", name);
    return (g_pending_mask & 8) ? WASMOS_SCRIPT_HOOK_PENDING : 0;
}
static void hook_echo(void* user, const char* text) {
    (void)user;
    log_call("echo", text);
}

static wasmos_script_ops_t g_ops = {
    .on_start = hook_start,
    .on_spawn = hook_spawn,
    .on_exec = hook_exec,
    .on_wait_svc = hook_wait,
    .on_echo = hook_echo,
};

static wasmos_script_runner_t g_runner;
static wasmos_script_state_t g_state;

static void fresh(void) {
    g_log[0] = '\0';
    g_pending_mask = 0;
    g_start_rc = 0;
    g_exec_code = 0;
    wasmos_script_state_dispose(&g_state);
    wasmos_script_state_init(&g_state);
}

/* Writes `body` to <dir>/<name> and returns the path in a static buffer
 * (one of four, so a case can hold a few at once). */
static const char* write_rc(const char* name, const char* body) {
    static char paths[4][320];
    static int next;
    char* p = paths[next++ % 4];
    snprintf(p, 320, RC_PREFIX "%s", name);
    if (g_written_n < (int)(sizeof(g_written) / sizeof(g_written[0]))) {
        static char names[16][320];
        snprintf(names[g_written_n], 320, "%s", p);
        g_written[g_written_n] = names[g_written_n];
        g_written_n++;
    }
    FILE* f = fopen(p, "w");
    if (f) {
        fputs(body, f);
        fclose(f);
    }
    return p;
}

/* Steps until something other than MORE. */
static int step_until_stop(void) {
    int r;
    do {
        r = wasmos_script_runner_step(&g_runner);
    } while (r == WASMOS_SCRIPT_STEP_MORE);
    return r;
}

/* The synchronous run executes every command in order, as before. */
static int test_run_executes_in_order(void) {
    fresh();
    g_exec_code = 4;
    const char* p = write_rc("a.rc",
                             "# comment\n"
                             "start /s/a\n"
                             "\n"
                             "spawn /s/b\n"
                             "exec /u/c\n"
                             "echo code=${?}\n"
                             "wait-svc gfx\n");
    if (wasmos_script_run(&g_state, &g_ops, p) != 0)
        return __LINE__;
    if (strcmp(g_log, "start:/s/a\nspawn:/s/b\nexec:/u/c\necho:code=4\nwait:gfx\n") != 0)
        return __LINE__;
    return 0;
}

/* A pending hook suspends the runner: stepping again runs nothing until the
 * command is completed, and then the script continues where it stopped. */
static int test_pending_wait_suspends_until_complete(void) {
    fresh();
    g_pending_mask = 8;
    const char* p = write_rc("b.rc", "spawn /s/a\nwait-svc gfx\nspawn /s/b\n");
    if (wasmos_script_runner_start(&g_runner, &g_state, &g_ops, p) != 0)
        return __LINE__;
    if (step_until_stop() != WASMOS_SCRIPT_STEP_PENDING)
        return __LINE__;
    if (wasmos_script_runner_step(&g_runner) != WASMOS_SCRIPT_STEP_PENDING)
        return __LINE__;
    if (strcmp(g_log, "spawn:/s/a\nwait:gfx\n") != 0)
        return __LINE__;
    wasmos_script_runner_complete(&g_runner, 0, 0);
    if (step_until_stop() != WASMOS_SCRIPT_STEP_DONE)
        return __LINE__;
    if (strcmp(g_log, "spawn:/s/a\nwait:gfx\nspawn:/s/b\n") != 0)
        return __LINE__;
    return 0;
}

/* A pending exec reports its exit code through complete(), and ${?} sees it. */
static int test_pending_exec_exit_code_reaches_status(void) {
    fresh();
    g_pending_mask = 4;
    const char* p = write_rc("c.rc", "exec /u/x\necho rc=${?}\n");
    wasmos_script_runner_start(&g_runner, &g_state, &g_ops, p);
    if (step_until_stop() != WASMOS_SCRIPT_STEP_PENDING)
        return __LINE__;
    wasmos_script_runner_complete(&g_runner, 0, 7);
    if (step_until_stop() != WASMOS_SCRIPT_STEP_DONE)
        return __LINE__;
    if (strstr(g_log, "echo:rc=7\n") == 0)
        return __LINE__;
    return 0;
}

/* A pending start that completes with a failure fails the script on the next
 * step, as a synchronous failing start does; nothing after it runs. */
static int test_failed_pending_start_fails_the_script(void) {
    fresh();
    g_pending_mask = 1;
    const char* p = write_rc("d.rc", "start /s/a\nspawn /s/never\n");
    wasmos_script_runner_start(&g_runner, &g_state, &g_ops, p);
    if (step_until_stop() != WASMOS_SCRIPT_STEP_PENDING)
        return __LINE__;
    wasmos_script_runner_complete(&g_runner, -1, 0);
    if (wasmos_script_runner_step(&g_runner) != WASMOS_SCRIPT_STEP_ERROR)
        return __LINE__;
    if (strstr(g_log, "never") != 0 || g_runner.depth != 0)
        return __LINE__;

    fresh();
    g_start_rc = -1;
    p = write_rc("d2.rc", "start /s/a\nspawn /s/never\n");
    if (wasmos_script_run(&g_state, &g_ops, p) != -1 || strstr(g_log, "never") != 0)
        return __LINE__;
    return 0;
}

/* A command going pending inside a nested file suspends the whole runner and
 * resumes in that file; the nested `script` hands its exit code back to the
 * parent when it ends, and a `source`d file shares the parent's variables. */
static int test_nested_files_resume_in_place(void) {
    fresh();
    g_pending_mask = 4;
    write_rc("child.rc", "spawn /c/1\nexec /c/2\nspawn /c/3\n");
    write_rc("lib.rc", "set V=from-lib\n");
    char body[512];
    snprintf(body,
             sizeof(body),
             "script " RC_PREFIX "child.rc\necho after=${?}\nsource " RC_PREFIX
             "lib.rc\necho v=${V}\n");
    const char* p = write_rc("parent.rc", body);
    wasmos_script_runner_start(&g_runner, &g_state, &g_ops, p);
    if (step_until_stop() != WASMOS_SCRIPT_STEP_PENDING)
        return __LINE__;
    if (g_runner.depth != 2)
        return __LINE__;
    wasmos_script_runner_complete(&g_runner, 0, 9);
    if (step_until_stop() != WASMOS_SCRIPT_STEP_DONE)
        return __LINE__;
    if (strcmp(g_log, "spawn:/c/1\nexec:/c/2\nspawn:/c/3\necho:after=9\necho:v=from-lib\n") != 0)
        return __LINE__;
    return 0;
}

/* The synchronous run cannot complete a pending command, so it fails rather
 * than skipping it. */
static int test_run_fails_on_a_pending_hook(void) {
    fresh();
    g_pending_mask = 2;
    const char* p = write_rc("e.rc", "spawn /s/a\nspawn /s/b\n");
    if (wasmos_script_run(&g_state, &g_ops, p) != -1)
        return __LINE__;
    if (strcmp(g_log, "spawn:/s/a\n") != 0)
        return __LINE__;
    return 0;
}

/* Nesting past WASMOS_SCRIPT_NEST_MAX fails the script instead of exhausting
 * the stack, and a nested file that does not exist fails it as before. */
static int test_nesting_limit_and_missing_file(void) {
    fresh();
    char body[400];
    snprintf(body, sizeof(body), "spawn /s/x\nsource " RC_PREFIX "self.rc\n");
    const char* p = write_rc("self.rc", body);
    if (wasmos_script_run(&g_state, &g_ops, p) != -1)
        return __LINE__;
    int spawns = 0;
    for (const char* s = g_log; (s = strstr(s, "spawn:")) != 0; s++) {
        spawns++;
    }
    if (spawns != WASMOS_SCRIPT_NEST_MAX)
        return __LINE__;

    fresh();
    snprintf(body, sizeof(body), "script " RC_PREFIX "missing.rc\nspawn /s/never\n");
    p = write_rc("f.rc", body);
    if (wasmos_script_run(&g_state, &g_ops, p) != -1 || strstr(g_log, "never") != 0)
        return __LINE__;
    return 0;
}

/* Skipped `if` branches never call a hook, so they never go pending. */
static int test_skipped_branch_calls_no_hook(void) {
    fresh();
    g_pending_mask = 2;
    const char* p = write_rc("g.rc", "if -f /no/such/file then\nspawn /s/a\nendif\n");
    if (wasmos_script_run(&g_state, &g_ops, p) != 0 || g_log[0] != '\0')
        return __LINE__;
    return 0;
}

int main(void) {
    static const wasmos_test_case_t cases[] = {
        WASMOS_TEST_CASE(test_run_executes_in_order),
        WASMOS_TEST_CASE(test_pending_wait_suspends_until_complete),
        WASMOS_TEST_CASE(test_pending_exec_exit_code_reaches_status),
        WASMOS_TEST_CASE(test_failed_pending_start_fails_the_script),
        WASMOS_TEST_CASE(test_nested_files_resume_in_place),
        WASMOS_TEST_CASE(test_run_fails_on_a_pending_hook),
        WASMOS_TEST_CASE(test_nesting_limit_and_missing_file),
        WASMOS_TEST_CASE(test_skipped_branch_calls_no_hook),
    };
    const int groups = (int)(sizeof(cases) / sizeof(cases[0]));
    int rc = wasmos_test_run_all(cases, groups);
    for (int i = 0; i < g_written_n; ++i) {
        (void)remove(g_written[i]);
    }
    if (rc != 0) {
        return 1;
    }
    printf("test_libc_script_runner: %d groups passed\n", groups);
    return 0;
}
