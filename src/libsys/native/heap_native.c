/* heap_native.c - slab allocator for native (ring-0) WASMOS services.
 *
 * Small allocations use fixed size-class slabs; large allocations get their own
 * anonymous page mapping. Backed by the driver_api vm_map/vm_unmap hooks (kernel
 * pfa pages returned as higher-half pointers). Each native service links its own
 * copy, so the allocator state is per-service and single-threaded (net-stack's
 * reactor); add a lock before using it concurrently. Provides the standard
 * malloc/free/calloc/realloc so mbedTLS and other code can use them directly.
 *
 * Adapted from a correctness-first design; double-free detection is best-effort
 * (a freed slab block's header is partially overwritten by the free-list link),
 * and a reaped service's slabs are reclaimed by the kernel, not here.
 */
#include <stddef.h>
#include <stdint.h>

#include "wasmos_native_driver.h"

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#define HEAP_PAGE_SIZE 4096u
#define HEAP_SLAB_SIZE (64u * 1024u)
#define HEAP_MAX_SMALL_ALLOCATION 4096u
#define HEAP_ALIGN 16u

#define HEAP_ALLOCATION_MAGIC 0xA110CA7Eu
#define HEAP_FREED_MAGIC 0xFEE1DEADu
#define HEAP_SLAB_MAGIC 0x51AB51ABu
#define HEAP_LARGE_MAGIC 0x1A46E001u
#define HEAP_KEEP_EMPTY_SLABS 1u

/* --- driver_api page source ------------------------------------------------ */

static wasmos_driver_api_t* g_heap_api = 0;

/* Called once at service startup (before the first allocation). */
void wasmos_native_heap_init(wasmos_driver_api_t* api) {
    g_heap_api = api;
}

static void* os_vm_map(size_t size) {
    if (g_heap_api == 0 || g_heap_api->vm_map == 0) {
        return 0;
    }
    return g_heap_api->vm_map((uint32_t)size);
}

static void os_vm_unmap(void* address, size_t size) {
    if (g_heap_api != 0 && g_heap_api->vm_unmap != 0) {
        g_heap_api->vm_unmap(address, (uint32_t)size);
    }
}

static void* heap_memcpy(void* destination, const void* source, size_t size) {
    unsigned char* dst = destination;
    const unsigned char* src = source;
    for (size_t i = 0; i < size; ++i) {
        dst[i] = src[i];
    }
    return destination;
}

static void* heap_memset(void* destination, int value, size_t size) {
    unsigned char* dst = destination;
    unsigned char byte = (unsigned char)value;
    for (size_t i = 0; i < size; ++i) {
        dst[i] = byte;
    }
    return destination;
}

/* Allocator invariant broken (corruption / double free): a service that trips
 * this cannot safely continue. Report the reason and terminate the service.
 * Overridable via a strong definition. */
__attribute__((weak)) void heap_corruption_detected(const char* reason, const void* pointer) {
    (void)pointer;
    if (g_heap_api != 0 && g_heap_api->console_write != 0) {
        static const char prefix[] = "[heap] corruption detected: ";
        g_heap_api->console_write(prefix, (int)(sizeof(prefix) - 1));
        if (reason != 0) {
            int len = 0;
            while (reason[len] != '\0') {
                len++;
            }
            g_heap_api->console_write(reason, len);
        }
        g_heap_api->console_write("\n", 1);
    }
    if (g_heap_api != 0 && g_heap_api->proc_exit != 0) {
        g_heap_api->proc_exit(-1);
    }
    /* proc_exit is unavailable or returned unexpectedly: fail hard. */
    __builtin_trap();
}

/* --- helpers --------------------------------------------------------------- */

static int heap_add_overflow(size_t left, size_t right, size_t* result) {
    if (left > SIZE_MAX - right) {
        return 1;
    }
    *result = left + right;
    return 0;
}

static int heap_multiply_overflow(size_t left, size_t right, size_t* result) {
    if (left != 0 && right > SIZE_MAX / left) {
        return 1;
    }
    *result = left * right;
    return 0;
}

static size_t heap_align_up(size_t value, size_t alignment) {
    size_t mask = alignment - 1u;
    return (value + mask) & ~mask;
}

static uintptr_t heap_align_up_pointer(uintptr_t value, size_t alignment) {
    uintptr_t mask = (uintptr_t)alignment - 1u;
    return (value + mask) & ~mask;
}

/* --- metadata -------------------------------------------------------------- */

typedef enum {
    HEAP_ALLOCATION_SLAB = 1,
    HEAP_ALLOCATION_LARGE = 2,
} HeapAllocationKind;

typedef struct AllocationHeader {
    uint32_t magic;
    uint16_t kind;
    uint16_t class_index;
    size_t requested_size;
    void* owner; /* Slab* for slab blocks, LargeAllocationHeader* for large */
} AllocationHeader;

typedef struct FreeBlock {
    struct FreeBlock* next;
} FreeBlock;

typedef struct Slab {
    uint32_t magic;
    uint16_t class_index;
    uint16_t reserved;
    struct Slab* previous;
    struct Slab* next;
    FreeBlock* free_list;
    size_t mapping_size;
    size_t block_size;
    size_t total_blocks;
    size_t free_blocks;
} Slab;

/* `allocation` MUST be the last member: free()/realloc() recover the header as
 * (user_ptr - sizeof(AllocationHeader)), and the returned user pointer is
 * &large[1] (= base + sizeof(LargeAllocationHeader)). Placing the AllocationHeader
 * anywhere but immediately before the user data makes that recovered pointer miss
 * the real header (a garbage magic -> spurious "invalid free"). */
typedef struct LargeAllocationHeader {
    uint32_t large_magic;
    uint32_t reserved;
    size_t mapping_size;
    AllocationHeader allocation;
} LargeAllocationHeader;

typedef struct SizeClass {
    size_t user_size;
    size_t block_size;
    Slab* partial_slabs;
    Slab* full_slabs;
    Slab* empty_slabs;
    size_t empty_slab_count;
} SizeClass;

static const size_t heap_class_sizes[] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096,
};

#define HEAP_SIZE_CLASS_COUNT (sizeof(heap_class_sizes) / sizeof(heap_class_sizes[0]))

static SizeClass heap_classes[HEAP_SIZE_CLASS_COUNT];
static int heap_initialized;

/* --- slab lists ------------------------------------------------------------ */

static void slab_list_insert(Slab** head, Slab* slab) {
    slab->previous = 0;
    slab->next = *head;
    if (*head != 0) {
        (*head)->previous = slab;
    }
    *head = slab;
}

static void slab_list_remove(Slab** head, Slab* slab) {
    if (slab->previous != 0) {
        slab->previous->next = slab->next;
    } else {
        *head = slab->next;
    }
    if (slab->next != 0) {
        slab->next->previous = slab->previous;
    }
    slab->previous = 0;
    slab->next = 0;
}

/* --- init ------------------------------------------------------------------ */

static void heap_ensure_initialized(void) {
    if (heap_initialized) {
        return;
    }
    for (size_t i = 0; i < HEAP_SIZE_CLASS_COUNT; ++i) {
        SizeClass* size_class = &heap_classes[i];
        size_class->user_size = heap_class_sizes[i];
        size_class->block_size =
            heap_align_up(sizeof(AllocationHeader) + size_class->user_size, HEAP_ALIGN);
        size_class->partial_slabs = 0;
        size_class->full_slabs = 0;
        size_class->empty_slabs = 0;
        size_class->empty_slab_count = 0;
    }
    heap_initialized = 1;
}

/* --- slab lifecycle -------------------------------------------------------- */

static Slab* slab_create(uint16_t class_index, size_t block_size) {
    if (class_index >= HEAP_SIZE_CLASS_COUNT) {
        return 0;
    }
    void* mapping = os_vm_map(HEAP_SLAB_SIZE);
    if (mapping == 0) {
        return 0;
    }
    Slab* slab = mapping;
    slab->magic = HEAP_SLAB_MAGIC;
    slab->class_index = class_index;
    slab->reserved = 0;
    slab->previous = 0;
    slab->next = 0;
    slab->free_list = 0;
    slab->mapping_size = HEAP_SLAB_SIZE;
    slab->block_size = block_size;

    uintptr_t mapping_start = (uintptr_t)mapping;
    uintptr_t data_start = heap_align_up_pointer(mapping_start + sizeof(Slab), HEAP_ALIGN);
    size_t metadata_size = (size_t)(data_start - mapping_start);
    if (metadata_size >= HEAP_SLAB_SIZE) {
        os_vm_unmap(mapping, HEAP_SLAB_SIZE);
        return 0;
    }
    size_t available = HEAP_SLAB_SIZE - metadata_size;
    slab->total_blocks = available / block_size;
    slab->free_blocks = slab->total_blocks;
    if (slab->total_blocks == 0) {
        os_vm_unmap(mapping, HEAP_SLAB_SIZE);
        return 0;
    }
    for (size_t i = 0; i < slab->total_blocks; ++i) {
        FreeBlock* block = (FreeBlock*)(data_start + i * block_size);
        block->next = slab->free_list;
        slab->free_list = block;
    }
    return slab;
}

static void slab_destroy(Slab* slab) {
    if (slab == 0 || slab->magic != HEAP_SLAB_MAGIC) {
        heap_corruption_detected("invalid slab destruction", slab);
        return;
    }
    size_t mapping_size = slab->mapping_size;
    slab->magic = 0;
    os_vm_unmap(slab, mapping_size);
}

static int heap_find_size_class(size_t size) {
    for (size_t i = 0; i < HEAP_SIZE_CLASS_COUNT; ++i) {
        if (size <= heap_classes[i].user_size) {
            return (int)i;
        }
    }
    return -1;
}

/* --- small allocation ------------------------------------------------------ */

static void* slab_allocate_block(Slab* slab, size_t requested_size) {
    if (slab == 0 || slab->magic != HEAP_SLAB_MAGIC || slab->free_list == 0 ||
        slab->free_blocks == 0) {
        return 0;
    }
    FreeBlock* block = slab->free_list;
    slab->free_list = block->next;
    slab->free_blocks--;

    AllocationHeader* header = (AllocationHeader*)block;
    header->magic = HEAP_ALLOCATION_MAGIC;
    header->kind = HEAP_ALLOCATION_SLAB;
    header->class_index = slab->class_index;
    header->requested_size = requested_size;
    header->owner = slab;
    return header + 1;
}

static void* heap_allocate_small(size_t size) {
    int class_index = heap_find_size_class(size);
    if (class_index < 0) {
        return 0;
    }
    SizeClass* size_class = &heap_classes[class_index];
    Slab* slab = size_class->partial_slabs;
    if (slab == 0 && size_class->empty_slabs != 0) {
        slab = size_class->empty_slabs;
        slab_list_remove(&size_class->empty_slabs, slab);
        size_class->empty_slab_count--;
        slab_list_insert(&size_class->partial_slabs, slab);
    }
    if (slab == 0) {
        slab = slab_create((uint16_t)class_index, size_class->block_size);
        if (slab == 0) {
            return 0;
        }
        slab_list_insert(&size_class->partial_slabs, slab);
    }
    void* pointer = slab_allocate_block(slab, size);
    if (pointer == 0) {
        heap_corruption_detected("partial slab has no free blocks", slab);
        return 0;
    }
    if (slab->free_blocks == 0) {
        slab_list_remove(&size_class->partial_slabs, slab);
        slab_list_insert(&size_class->full_slabs, slab);
    }
    return pointer;
}

/* --- large allocation ------------------------------------------------------ */

static void* heap_allocate_large(size_t size) {
    size_t total_size;
    if (heap_add_overflow(sizeof(LargeAllocationHeader), size, &total_size)) {
        return 0;
    }
    size_t mapping_size = heap_align_up(total_size, HEAP_PAGE_SIZE);
    LargeAllocationHeader* large = os_vm_map(mapping_size);
    if (large == 0) {
        return 0;
    }
    large->allocation.magic = HEAP_ALLOCATION_MAGIC;
    large->allocation.kind = HEAP_ALLOCATION_LARGE;
    large->allocation.class_index = (uint16_t)0xFFFFu;
    large->allocation.requested_size = size;
    large->allocation.owner = large;
    large->large_magic = HEAP_LARGE_MAGIC;
    large->reserved = 0;
    large->mapping_size = mapping_size;
    return &large[1];
}

/* --- malloc / free / calloc / realloc -------------------------------------- */

void* malloc(size_t size) {
    heap_ensure_initialized();
    if (size == 0) {
        size = 1;
    }
    if (size <= HEAP_MAX_SMALL_ALLOCATION) {
        return heap_allocate_small(size);
    }
    return heap_allocate_large(size);
}

static void heap_trim_empty_slabs(SizeClass* size_class) {
    while (size_class->empty_slab_count > HEAP_KEEP_EMPTY_SLABS) {
        Slab* slab = size_class->empty_slabs;
        if (slab == 0) {
            heap_corruption_detected("invalid empty slab count", size_class);
            return;
        }
        slab_list_remove(&size_class->empty_slabs, slab);
        size_class->empty_slab_count--;
        slab_destroy(slab);
    }
}

static void heap_free_small(AllocationHeader* header, void* user_pointer) {
    if (header->class_index >= HEAP_SIZE_CLASS_COUNT) {
        heap_corruption_detected("invalid allocation class", user_pointer);
        return;
    }
    Slab* slab = header->owner;
    if (slab == 0 || slab->magic != HEAP_SLAB_MAGIC || slab->class_index != header->class_index) {
        heap_corruption_detected("invalid allocation owner", user_pointer);
        return;
    }
    SizeClass* size_class = &heap_classes[header->class_index];
    int was_full = slab->free_blocks == 0;

    header->magic = HEAP_FREED_MAGIC;
    FreeBlock* block = (FreeBlock*)header;
    block->next = slab->free_list;
    slab->free_list = block;

    if (slab->free_blocks >= slab->total_blocks) {
        heap_corruption_detected("slab free count overflow", user_pointer);
        return;
    }
    slab->free_blocks++;

    if (was_full) {
        slab_list_remove(&size_class->full_slabs, slab);
        slab_list_insert(&size_class->partial_slabs, slab);
    }
    if (slab->free_blocks == slab->total_blocks) {
        slab_list_remove(&size_class->partial_slabs, slab);
        slab_list_insert(&size_class->empty_slabs, slab);
        size_class->empty_slab_count++;
        heap_trim_empty_slabs(size_class);
    }
}

static void heap_free_large(AllocationHeader* header, void* user_pointer) {
    LargeAllocationHeader* large = header->owner;
    if (large == 0 || &large->allocation != header || large->large_magic != HEAP_LARGE_MAGIC) {
        heap_corruption_detected("invalid large allocation", user_pointer);
        return;
    }
    size_t mapping_size = large->mapping_size;
    if (mapping_size == 0 || (mapping_size % HEAP_PAGE_SIZE) != 0) {
        heap_corruption_detected("invalid large mapping size", user_pointer);
        return;
    }
    large->allocation.magic = HEAP_FREED_MAGIC;
    large->large_magic = 0;
    os_vm_unmap(large, mapping_size);
}

void free(void* pointer) {
    if (pointer == 0) {
        return;
    }
    AllocationHeader* header = ((AllocationHeader*)pointer) - 1;
    if (header->magic == HEAP_FREED_MAGIC) {
        heap_corruption_detected("double free", pointer);
        return;
    }
    if (header->magic != HEAP_ALLOCATION_MAGIC) {
        heap_corruption_detected("invalid free", pointer);
        return;
    }
    switch ((HeapAllocationKind)header->kind) {
    case HEAP_ALLOCATION_SLAB:
        heap_free_small(header, pointer);
        return;
    case HEAP_ALLOCATION_LARGE:
        heap_free_large(header, pointer);
        return;
    default:
        heap_corruption_detected("invalid allocation kind", pointer);
        return;
    }
}

void* calloc(size_t count, size_t element_size) {
    size_t total_size;
    if (heap_multiply_overflow(count, element_size, &total_size)) {
        return 0;
    }
    void* pointer = malloc(total_size);
    if (pointer != 0) {
        heap_memset(pointer, 0, total_size);
    }
    return pointer;
}

void* realloc(void* pointer, size_t new_size) {
    if (pointer == 0) {
        return malloc(new_size);
    }
    if (new_size == 0) {
        free(pointer);
        return 0;
    }
    AllocationHeader* header = ((AllocationHeader*)pointer) - 1;
    if (header->magic != HEAP_ALLOCATION_MAGIC) {
        heap_corruption_detected("invalid realloc", pointer);
        return 0;
    }
    size_t old_size = header->requested_size;

    if (header->kind == HEAP_ALLOCATION_SLAB) {
        if (header->class_index >= HEAP_SIZE_CLASS_COUNT) {
            heap_corruption_detected("invalid realloc size class", pointer);
            return 0;
        }
        SizeClass* size_class = &heap_classes[header->class_index];
        if (new_size <= size_class->user_size) {
            header->requested_size = new_size;
            return pointer;
        }
    } else if (header->kind == HEAP_ALLOCATION_LARGE) {
        LargeAllocationHeader* large = header->owner;
        if (large == 0 || large->large_magic != HEAP_LARGE_MAGIC) {
            heap_corruption_detected("invalid large realloc", pointer);
            return 0;
        }
        size_t available_size = large->mapping_size - sizeof(LargeAllocationHeader);
        if (new_size <= available_size) {
            header->requested_size = new_size;
            return pointer;
        }
    } else {
        heap_corruption_detected("invalid realloc kind", pointer);
        return 0;
    }

    void* replacement = malloc(new_size);
    if (replacement == 0) {
        return 0;
    }
    size_t copy_size = old_size < new_size ? old_size : new_size;
    heap_memcpy(replacement, pointer, copy_size);
    free(pointer);
    return replacement;
}
