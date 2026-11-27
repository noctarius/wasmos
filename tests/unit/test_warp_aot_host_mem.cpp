/* test_warp_aot_host_mem.cpp — the aligned and virtual allocations of the
 * warp_aot host shim (src/tools/warp_aot/host_mem_utils.cpp), which also backs
 * the host WARP tests.
 *
 * WARP pairs allocAlignedMemory with freeAlignedMemory(ptr) and nothing else:
 * the free takes no size, so the shim must recover it. reallocAlignedMemory
 * releases the old block and returns one that freeAlignedMemory accepts, and
 * allocVirtualMemory pairs with freeVirtualMemory(ptr, size).
 *
 * Regression: 2026-11-27-aot-free-aligned. freeAlignedMemory read a size header
 * at ptr - sizeof(size_t) that allocAlignedMemory never wrote: allocAlignedMemory
 * returned a bare mmap base, so the read landed in whatever preceded the
 * mapping. It faulted, or handed munmap a misaligned base and a garbage length
 * and the block was never released.
 *
 * Regression: 2026-11-27-aot-aligned-size-overflow. A size within a page (plus
 * the header page) of SIZE_MAX wrapped when rounded: allocAlignedMemory
 * returned a pointer past a one-page mapping instead of throwing, and
 * reallocAlignedMemory to such a size released the original block instead of
 * reporting the failure and leaving it valid.
 *
 * "Released" is observed with msync, which fails with ENOMEM on a range that is
 * no longer mapped. Each case runs in a child process, so a fault inside the
 * shim fails that case instead of the whole suite.
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/utils/MemUtils.hpp"

using namespace vb::MemUtils;

static int g_failures;
static int g_checks;

/* Inside a child: report a failed condition through the exit status. */
#define REQUIRE(cond, code)                                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("    child: %s failed\n", #cond);                                               \
            fflush(stdout);                                                                        \
            _exit(code);                                                                           \
        }                                                                                          \
    } while (0)

static bool released(void* p, size_t len) {
    return msync(p, len, MS_ASYNC) == -1 && errno == ENOMEM;
}

static bool mapped(void* p, size_t len) {
    return msync(p, len, MS_ASYNC) == 0;
}

static void run_case(const char* name, void (*body)()) {
    g_checks++;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    if (pid < 0) {
        g_failures++;
        printf("  [FAIL] %s: fork failed (%s)\n", name, strerror(errno));
        return;
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid) {
        g_failures++;
        printf("  [FAIL] %s: waitpid failed (%s)\n", name, strerror(errno));
    } else if (WIFSIGNALED(status)) {
        g_failures++;
        printf("  [FAIL] %s: the shim died with signal %d\n", name, WTERMSIG(status));
    } else if (WEXITSTATUS(status) != 0) {
        g_failures++;
        printf("  [FAIL] %s (check %d)\n", name, WEXITSTATUS(status));
    }
}

static void free_aligned_releases_the_block() {
    size_t ps = getOSMemoryPageSize();
    uint8_t* p = allocAlignedMemory(3 * ps, ps);
    REQUIRE(reinterpret_cast<uintptr_t>(p) % ps == 0, 1);
    memset(p, 0xA5, 3 * ps);
    freeAlignedMemory(p);
    REQUIRE(released(p, 3 * ps), 2);
}

static void realloc_keeps_the_contents_and_releases_the_old_block() {
    size_t ps = getOSMemoryPageSize();
    uint8_t* old = allocAlignedMemory(ps, ps);
    memset(old, 0x5A, ps);
    uint8_t* grown = reallocAlignedMemory(old, ps, 4 * ps, ps);
    REQUIRE(grown != old, 1);
    REQUIRE(released(old, ps), 2);
    for (size_t i = 0; i < ps; ++i) {
        REQUIRE(grown[i] == 0x5A, 3);
    }
    memset(grown, 0, 4 * ps);
    freeAlignedMemory(grown);
    REQUIRE(released(grown, 4 * ps), 4);
}

static void free_aligned_leaves_neighbours_mapped() {
    size_t ps = getOSMemoryPageSize();
    uint8_t* blocks[16];
    for (auto& b : blocks) {
        b = allocAlignedMemory(ps, ps);
        memset(b, 0x11, ps);
    }
    for (size_t i = 0; i < 16; i += 2) {
        freeAlignedMemory(blocks[i]);
    }
    for (size_t i = 1; i < 16; i += 2) {
        REQUIRE(mapped(blocks[i], ps), 1);
        REQUIRE(blocks[i][ps - 1] == 0x11, 2);
        freeAlignedMemory(blocks[i]);
    }
}

static void an_unrepresentable_size_throws() {
    size_t ps = getOSMemoryPageSize();
    const size_t sizes[] = {SIZE_MAX, SIZE_MAX - ps + 1, SIZE_MAX - 2 * ps + 1};
    for (size_t size : sizes) {
        bool threw = false;
        try {
            (void)allocAlignedMemory(size, ps);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        REQUIRE(threw, 1);
    }
}

static void a_failed_realloc_leaves_the_old_block_valid() {
    size_t ps = getOSMemoryPageSize();
    uint8_t* old = allocAlignedMemory(ps, ps);
    memset(old, 0x33, ps);
    bool threw = false;
    try {
        (void)reallocAlignedMemory(old, ps, SIZE_MAX, ps);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    REQUIRE(threw, 1);
    REQUIRE(mapped(old, ps), 2);
    REQUIRE(old[ps - 1] == 0x33, 3);
    freeAlignedMemory(old);
}

static void free_virtual_releases_what_alloc_virtual_reserved() {
    size_t ps = getOSMemoryPageSize();
    void* v = allocVirtualMemory(8 * ps);
    commitVirtualMemory(v, 8 * ps);
    memset(v, 0x22, 8 * ps);
    freeVirtualMemory(v, 8 * ps);
    REQUIRE(released(v, 8 * ps), 1);
}

int main() {
    run_case("freeAlignedMemory releases the block", free_aligned_releases_the_block);
    run_case("reallocAlignedMemory keeps the contents and releases the old block",
             realloc_keeps_the_contents_and_releases_the_old_block);
    run_case("freeAlignedMemory leaves neighbouring blocks mapped",
             free_aligned_leaves_neighbours_mapped);
    run_case("an unrepresentable aligned size throws std::bad_alloc",
             an_unrepresentable_size_throws);
    run_case("a failed reallocAlignedMemory leaves the old block valid",
             a_failed_realloc_leaves_the_old_block_valid);
    run_case("freeVirtualMemory releases what allocVirtualMemory reserved",
             free_virtual_releases_what_alloc_virtual_reserved);
    printf("test_warp_aot_host_mem: %d cases, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
