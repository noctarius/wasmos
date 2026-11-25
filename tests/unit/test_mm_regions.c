/* test_mm_regions.c — a context's region table under concurrent use
 * (src/kernel/memory.c).
 *
 * A thread of a process can fault on one CPU while another thread of the same
 * process grows or rebinds its linear memory on another. The fault path reads
 * a region's bounds and its backing and maps one page from them; the rebind
 * path changes the backing and the bounds. The contract: a fault maps a page
 * of the backing the region describes at that moment, never a page computed
 * from one binding's bounds and another binding's backing.
 *
 * Regression: 2026-11-25-mm-region-torn-read -- the region table had no lock.
 * mm_handle_page_fault checked the faulting address against region->size and
 * then read region->phys_base, while mm_context_rebind_wasm_linear stored
 * phys_base and then size. A fault that read the old, larger size and the new
 * phys_base mapped an offset past the end of the new backing: a frame the
 * process was never given. The same window let a fault map the old backing
 * after the rebind had unmapped and freed it.
 *
 * Here pthreads stand in for the CPUs. paging_map_4k_in_root is a stub that
 * checks every mapping against the two backings the writer alternates between;
 * a page past the end of the one-page backing is the torn read. memory.c is
 * linked in whole; the paging and frame-allocator calls it makes are stubs.
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "arch/arch_context.h"
#include "boot.h"
#include "cpu_local.h"
#include "klog.h"
#include "memory.h"
#include "paging.h"
#include "physmem.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("  [FAIL] %s (%s:%d)\n", (msg), __FILE__, __LINE__);                            \
        }                                                                                          \
    } while (0)

#define PAGE 4096ull

_Thread_local cpu_local_t* g_host_cpu_local;
cpu_local_t g_cpus[WASMOS_MAX_CPUS];

/* --- Stubs for what memory.c calls outside itself. ------------------------ */

void klog_write(const char* s) {
    (void)s;
}

void klog_printf(const char* fmt, ...) {
    (void)fmt;
}

/* The user-copy helpers' stack switch; nothing here copies. */
uint64_t arch_call_on_stack(uintptr_t stack_top, arch_stack_fn_t fn, uint64_t a0, uint64_t a1,
                            uint64_t a2) {
    (void)stack_top;
    uint64_t (*call)(uint64_t, uint64_t, uint64_t) = (uint64_t (*)(uint64_t, uint64_t, uint64_t))fn;
    return call(a0, a1, a2);
}

static uint64_t g_next_frame = 0x10000000ull;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;

void pfa_init(const boot_info_t* boot_info) {
    (void)boot_info;
}

uint64_t pfa_alloc_pages(uint64_t pages) {
    pthread_mutex_lock(&g_frame_lock);
    uint64_t phys = g_next_frame;
    g_next_frame += pages * PAGE;
    pthread_mutex_unlock(&g_frame_lock);
    return phys;
}

void pfa_free_pages(uint64_t phys, uint64_t pages) {
    (void)phys;
    (void)pages;
}

int paging_init(void) {
    return 0;
}

uint64_t paging_get_root_table(void) {
    return 0x1000;
}

uint64_t paging_get_current_root_table(void) {
    return 0x1000;
}

uint64_t paging_get_higher_half_base(void) {
    return 0xFFFF800000000000ull;
}

int paging_switch_root(uint64_t root_table) {
    (void)root_table;
    return 0;
}

int paging_create_address_space(uint64_t* out_root_table) {
    *out_root_table = pfa_alloc_pages(1);
    return 0;
}

void paging_destroy_address_space(uint64_t root_table) {
    (void)root_table;
}

int paging_verify_user_root(uint64_t root_table, int log_failures) {
    (void)root_table;
    (void)log_failures;
    return 0;
}

void paging_dump_user_root_kernel_mappings(uint64_t root_table) {
    (void)root_table;
}

uint64_t paging_virt_to_phys(uint64_t virt) {
    (void)virt;
    return 0;
}

uint64_t paging_virt_to_phys_in_root(uint64_t root_table, uint64_t virt) {
    (void)root_table;
    (void)virt;
    return 0;
}

int paging_unmap_4k_in_root(uint64_t root_table, uint64_t virt) {
    (void)root_table;
    (void)virt;
    return 0;
}

/* The two backings the writer alternates between: A covers two pages, B one. */
#define BACKING_A 0x40000000ull
#define BACKING_B 0x50000000ull
static uint64_t g_linear_base;
static volatile int g_torn;
static volatile uint64_t g_torn_phys;

int paging_map_4k_in_root(uint64_t root_table, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)root_table;
    (void)flags;
    uint64_t offset = virt - g_linear_base;
    int in_a = phys >= BACKING_A && phys < BACKING_A + 2 * PAGE;
    int in_b = phys >= BACKING_B && phys < BACKING_B + PAGE;
    /* A mapping must be the page at `offset` of one whole backing. */
    if (!((in_a && phys - BACKING_A == offset) || (in_b && phys - BACKING_B == offset))) {
        g_torn_phys = phys;
        g_torn = 1;
    }
    return 0;
}

int paging_map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    return paging_map_4k_in_root(0, virt, phys, flags);
}

int paging_unmap_4k(uint64_t virt) {
    (void)virt;
    return 0;
}

/* --- The race. -------------------------------------------------------------- */

#define CONTEXT_ID 7u
static volatile int g_stop;

static void* rebind_writer(void* arg) {
    (void)arg;
    g_host_cpu_local = &g_cpus[1];
    while (!g_stop) {
        (void)mm_context_rebind_wasm_linear(CONTEXT_ID, BACKING_A, 2 * PAGE);
        (void)mm_context_rebind_wasm_linear(CONTEXT_ID, BACKING_B, PAGE);
    }
    return 0;
}

static void* faulting_reader(void* arg) {
    (void)arg;
    g_host_cpu_local = &g_cpus[2];
    /* The second page exists only while backing A is bound. */
    uint64_t addr = g_linear_base + PAGE;
    while (!g_stop && !g_torn) {
        uint64_t mapped = 0;
        (void)mm_handle_page_fault(CONTEXT_ID, addr, MM_FAULT_USER | MM_FAULT_WRITE, &mapped);
    }
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void s_a_fault_never_maps_past_the_backing_a_rebind_installs(void) {
    mm_context_t* ctx = mm_context_create(CONTEXT_ID);
    CHECK(ctx != 0, "a context was created");
    if (!ctx) {
        return;
    }
    mem_region_t linear;
    CHECK(mm_context_region_for_type(ctx, MEM_REGION_WASM_LINEAR, &linear) == 0,
          "the context has a WASM_LINEAR region");
    g_linear_base = linear.base;
    /* Off the placeholder backing mm_context_create gave the region, so every
     * fault from here on maps a page of A or B. */
    CHECK(mm_context_rebind_wasm_linear(CONTEXT_ID, BACKING_A, 2 * PAGE) == 0,
          "the region was bound to backing A");

    pthread_t writer;
    pthread_t reader;
    pthread_create(&writer, 0, rebind_writer, 0);
    pthread_create(&reader, 0, faulting_reader, 0);
    /* The window is a few instructions wide; a few seconds of contention finds
     * it reliably when it is open. */
    double deadline = now_s() + 3.0;
    while (now_s() < deadline && !g_torn) {
        struct timespec nap = {0, 10L * 1000L * 1000L};
        nanosleep(&nap, 0);
    }
    g_stop = 1;
    pthread_join(writer, 0);
    pthread_join(reader, 0);
    if (g_torn) {
        printf("  torn mapping: phys %llx for offset %llx\n",
               (unsigned long long)g_torn_phys,
               (unsigned long long)PAGE);
    }
    CHECK(!g_torn, "every fault mapped a page of the backing bound at that moment");
}

int main(void) {
    g_host_cpu_local = &g_cpus[0];
    static boot_info_t boot_info;
    mm_init(&boot_info);
    printf("test_mm_regions\n");
    s_a_fault_never_maps_past_the_backing_a_rebind_installs();
    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
