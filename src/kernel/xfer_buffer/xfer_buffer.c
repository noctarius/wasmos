/* xfer_buffer/object.c - real object/owner/borrow xfer-buffer implementation.
 *
 * A xfer buffer is a first-class object with one current owner. The owner can
 * lend the object to any number of borrowers simultaneously, each borrow is an
 * independent handle with its own rights and reborrow tree, and DMA attaches to
 * a single owned object or a single borrow handle. This replaces the old
 * one-active-borrow-per-(kind, context) slot model, which could not represent a
 * borrower holding multiple grants to distinct owners at once.
 */
#include "xfer_buffer.h"

#include "framebuffer.h"
#include "list.h"
#include "physmem.h"
#include "sync/spinlock.h"
#include "wasmos_driver_abi.h"

/* Intrinsic capacity of the generic transfer buffer kind. Mirrors the kernel's
 * PM_XFER_BUFFER_SIZE; reconciled to a single source of truth when the object
 * model is wired into the process manager. */
/* Standard TRANSFER buffer size reported by xfer_buffer_size()/the
 * wasmos_xfer_buffer_size() hostcall. Callers use it as the chunk size for FS
 * transfers, so it is kept stable. Individual buffers are right-sized to the
 * requested minimum_size (page-rounded) at acquire time; this is just the
 * conventional/default size. */
#define XFER_TRANSFER_CAPACITY (2u * 1024u * 1024u)
/* Hard upper bound a single TRANSFER buffer may request. Sized to admit a
 * full-resolution compositor backbuffer (e.g. 1280x800x4 = 4 MiB) with headroom. */
#define XFER_TRANSFER_MAX_SIZE (16u * 1024u * 1024u)
#define XFER_PAGE_SIZE 4096u

typedef struct {
    uint8_t active;
    uint32_t kind;
    uint32_t buffer_id;
    uint32_t size_bytes;
    uint32_t owner_context_id;
    uint64_t phys_base;
    uint8_t dma_active;
    uint32_t dma_offset;
    uint32_t dma_length;
    uint32_t dma_direction_flags;
} object_slot_t;

typedef struct {
    uint8_t active;
    uint32_t borrow_id;
    uint32_t parent_borrow_id;
    uint32_t buffer_id;
    uint32_t kind;
    uint32_t lender_context_id;
    uint32_t borrower_context_id;
    uint32_t flags;
    uint8_t dma_active;
    uint32_t dma_offset;
    uint32_t dma_length;
    uint32_t dma_direction_flags;
} borrow_slot_t;

static list_t g_objects;
static list_t g_borrows;
static uint8_t g_initialized;
static uint32_t g_next_buffer_id = 1u;
static uint32_t g_next_borrow_id = 1u;

/* SMP guard for the whole object/borrow registry (g_objects, g_borrows, the id
 * counters, and the init-once flag). The kernel runs multi-core and every
 * spawn/reap now touches the registry (each process owns a spawn-info buffer),
 * so acquire/release/borrow/drop_context run concurrently across CPUs. Every
 * public xfer_buffer_* entry takes this lock via its wrapper; the internal
 * *_locked cores and helpers assume it is already held. Zero-initialized state
 * (== unlocked) is a valid starting value, so no explicit init is required. */
static ksync_spinlock_t g_xfer_lock;

static int registry_init_once(void) {
    if (g_initialized) {
        return 0;
    }
    if (list_init(&g_objects, (uint32_t)sizeof(object_slot_t), LIST_IMPL_LINKED, 0u) != 0) {
        return -1;
    }
    if (list_init(&g_borrows, (uint32_t)sizeof(borrow_slot_t), LIST_IMPL_LINKED, 0u) != 0) {
        return -1;
    }
    g_initialized = 1u;
    return 0;
}

static int valid_flags(uint32_t flags) {
    const uint32_t allowed = BUFFER_BORROW_READ | BUFFER_BORROW_WRITE;

    return flags != 0u && (flags & allowed) == flags;
}

static object_slot_t* object_alloc(void) {
    list_iter_t it;
    object_slot_t* slot = 0;

    /* Reuse a freed slot before growing the list to keep the store bounded. */
    slot = (object_slot_t*)list_first(&g_objects, &it);
    while (slot) {
        if (!slot->active) {
            return slot;
        }
        slot = (object_slot_t*)list_next(&it);
    }
    return (object_slot_t*)list_alloc(&g_objects);
}

static borrow_slot_t* borrow_alloc(void) {
    list_iter_t it;
    borrow_slot_t* slot = 0;

    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (!slot->active) {
            return slot;
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
    return (borrow_slot_t*)list_alloc(&g_borrows);
}

static object_slot_t* object_find(const xfer_buffer_t* buffer) {
    list_iter_t it;
    object_slot_t* slot = 0;

    if (!buffer || buffer->buffer_id == 0u || registry_init_once() != 0) {
        return 0;
    }
    slot = (object_slot_t*)list_first(&g_objects, &it);
    while (slot) {
        if (slot->active && slot->buffer_id == buffer->buffer_id && slot->kind == buffer->kind) {
            return slot;
        }
        slot = (object_slot_t*)list_next(&it);
    }
    return 0;
}

static object_slot_t* object_find_by_id(uint32_t buffer_id) {
    list_iter_t it;
    object_slot_t* slot = 0;

    if (buffer_id == 0u || registry_init_once() != 0) {
        return 0;
    }
    slot = (object_slot_t*)list_first(&g_objects, &it);
    while (slot) {
        if (slot->active && slot->buffer_id == buffer_id) {
            return slot;
        }
        slot = (object_slot_t*)list_next(&it);
    }
    return 0;
}

static borrow_slot_t* borrow_find(uint32_t borrow_id) {
    list_iter_t it;
    borrow_slot_t* slot = 0;

    if (borrow_id == 0u || registry_init_once() != 0) {
        return 0;
    }
    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (slot->active && slot->borrow_id == borrow_id) {
            return slot;
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
    return 0;
}

/* Active borrow held by borrower_context_id on a specific object, if any. A
 * borrower holds at most one active borrow per object. */
static borrow_slot_t* borrow_find_for(uint32_t buffer_id, uint32_t borrower_context_id) {
    list_iter_t it;
    borrow_slot_t* slot = 0;

    if (registry_init_once() != 0) {
        return 0;
    }
    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (slot->active && slot->buffer_id == buffer_id &&
            slot->borrower_context_id == borrower_context_id) {
            return slot;
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
    return 0;
}

static int object_has_active_borrow(uint32_t buffer_id) {
    list_iter_t it;
    borrow_slot_t* slot = 0;

    if (registry_init_once() != 0) {
        return 0;
    }
    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (slot->active && slot->buffer_id == buffer_id) {
            return 1;
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
    return 0;
}

/* Deactivate every active borrow of an object (top-level borrows and all their
 * reborrows share the object's buffer_id). Used by owner-side release as a
 * "transient unborrow": the owner tears the whole borrow tree down when it
 * destroys the object. DMA state on each revoked borrow is cleared. */
static void object_revoke_all_borrows(uint32_t buffer_id) {
    list_iter_t it;
    borrow_slot_t* slot = 0;

    if (registry_init_once() != 0) {
        return;
    }
    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (slot->active && slot->buffer_id == buffer_id) {
            slot->active = 0u;
            slot->dma_active = 0u;
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
}

/* Deactivate a borrow and cascade-revoke every downstream reborrow rooted in
 * it. DMA state attached to any revoked borrow is cleared. */
static void borrow_revoke_tree(uint32_t borrow_id) {
    list_iter_t it;
    borrow_slot_t* slot = 0;

    if (registry_init_once() != 0) {
        return;
    }
    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (slot->active && slot->parent_borrow_id == borrow_id) {
            borrow_revoke_tree(slot->borrow_id);
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
    slot = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (slot) {
        if (slot->active && slot->borrow_id == borrow_id) {
            slot->active = 0u;
            slot->dma_active = 0u;
            return;
        }
        slot = (borrow_slot_t*)list_next(&it);
    }
}

static uint32_t transfer_capacity(void) {
    return XFER_TRANSFER_CAPACITY;
}

static uint32_t framebuffer_capacity(void) {
    framebuffer_info_t fb_info = {0};

    if (framebuffer_get_info(&fb_info) != 0) {
        return 0u;
    }
    if (fb_info.framebuffer_size > 0xFFFFFFFFu) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)fb_info.framebuffer_size;
}

uint32_t xfer_buffer_size(uint32_t kind) {
    if (kind == BUFFER_KIND_TRANSFER) {
        return transfer_capacity();
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        return framebuffer_capacity();
    }
    return 0u;
}

static uint64_t xfer_buffer_object_phys_locked(const xfer_buffer_t* buffer) {
    object_slot_t* slot = object_find(buffer);

    return slot ? slot->phys_base : 0u;
}

static int xfer_buffer_describe_locked(uint32_t buffer_id, uint32_t kind, uint32_t context_id,
                                       xfer_buffer_t* out) {
    xfer_buffer_t key;
    object_slot_t* slot = 0;

    if (!out) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (buffer_id == 0u) {
        /* Ids are issued from 1; 0 never names a live object. */
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    key.kind = kind;
    key.buffer_id = buffer_id;
    key.size_bytes = 0u;
    slot = object_find(&key);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (slot->owner_context_id != context_id && !borrow_find_for(slot->buffer_id, context_id)) {
        return WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    }
    out->kind = slot->kind;
    out->buffer_id = slot->buffer_id;
    out->size_bytes = slot->size_bytes;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_get_borrowed_locked(uint32_t borrow_id, uint32_t context_id,
                                           xfer_buffer_borrow_t* out_borrow,
                                           xfer_buffer_dma_mapping_t* out_mapping) {
    borrow_slot_t* slot = 0;
    object_slot_t* object = 0;

    if (!out_borrow) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (borrow_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    slot = borrow_find(borrow_id);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    if (slot->borrower_context_id != context_id) {
        return WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    }
    object = object_find_by_id(slot->buffer_id);
    if (!object) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }

    out_borrow->buffer.kind = slot->kind;
    out_borrow->buffer.buffer_id = slot->buffer_id;
    out_borrow->buffer.size_bytes = object->size_bytes;
    out_borrow->lender_context_id = slot->lender_context_id;
    out_borrow->borrower_context_id = slot->borrower_context_id;
    out_borrow->flags = slot->flags;
    out_borrow->borrow_id = slot->borrow_id;

    if (out_mapping) {
        if (slot->dma_active) {
            out_mapping->buffer.kind = object->kind;
            out_mapping->buffer.buffer_id = object->buffer_id;
            out_mapping->buffer.size_bytes = object->size_bytes;
            out_mapping->owner_context_id = 0u;
            out_mapping->borrow_id = slot->borrow_id;
            out_mapping->offset = slot->dma_offset;
            out_mapping->length = slot->dma_length;
            out_mapping->direction_flags = slot->dma_direction_flags;
            out_mapping->device_addr = object->phys_base + (uint64_t)slot->dma_offset;
            out_mapping->attached_via_borrow = 1u;
            out_mapping->active = 1u;
        } else {
            out_mapping->buffer.kind = 0u;
            out_mapping->buffer.buffer_id = 0u;
            out_mapping->buffer.size_bytes = 0u;
            out_mapping->owner_context_id = 0u;
            out_mapping->borrow_id = 0u;
            out_mapping->offset = 0u;
            out_mapping->length = 0u;
            out_mapping->direction_flags = 0u;
            out_mapping->device_addr = 0u;
            out_mapping->attached_via_borrow = 0u;
            out_mapping->active = 0u;
        }
    }
    return WASMOS_ERR_NONE;
}

static uint64_t object_alloc_backing(uint32_t kind, uint32_t size_bytes) {
    if (kind == BUFFER_KIND_TRANSFER) {
        uint64_t pages = ((uint64_t)size_bytes + XFER_PAGE_SIZE - 1u) / XFER_PAGE_SIZE;
        return pfa_alloc_pages(pages);
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        framebuffer_info_t fb_info = {0};
        if (framebuffer_get_info(&fb_info) != 0) {
            return 0u;
        }
        return fb_info.framebuffer_base;
    }
    return 0u;
}

static void object_free_backing(const object_slot_t* slot) {
    if (slot->kind == BUFFER_KIND_TRANSFER && slot->phys_base != 0u) {
        uint64_t pages = ((uint64_t)slot->size_bytes + XFER_PAGE_SIZE - 1u) / XFER_PAGE_SIZE;
        pfa_free_pages(slot->phys_base, pages);
    }
}

/* Range [offset, offset+length) fully inside [0, size). */
static int range_within(uint32_t size, uint32_t offset, uint32_t length) {
    if (length == 0u || offset > size) {
        return 0;
    }
    return length <= size - offset;
}

static int dma_direction_allowed(uint32_t flags, uint32_t direction_flags) {
    if (direction_flags == 0u) {
        return 0;
    }
    if ((direction_flags & WASMOS_DMA_DIR_TO_DEVICE) != 0u && (flags & BUFFER_BORROW_READ) == 0u) {
        return 0;
    }
    if ((direction_flags & WASMOS_DMA_DIR_FROM_DEVICE) != 0u &&
        (flags & BUFFER_BORROW_WRITE) == 0u) {
        return 0;
    }
    return 1;
}

static int xfer_buffer_acquire_locked(uint32_t kind, uint32_t owner_context_id,
                                      uint32_t minimum_size, xfer_buffer_owner_t* out_owner) {
    uint32_t size = 0u;
    uint64_t phys_base = 0u;
    object_slot_t* slot = 0;

    if (!out_owner) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (kind != BUFFER_KIND_TRANSFER && kind != BUFFER_KIND_FRAMEBUFFER) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_KIND;
    }
    if (owner_context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (minimum_size == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_SIZE;
    }
    if (registry_init_once() != 0) {
        return WASMOS_ERR_XFER_BUFFER_INTERNAL;
    }
    if (kind == BUFFER_KIND_FRAMEBUFFER) {
        /* The FRAMEBUFFER object always spans the whole hardware framebuffer. */
        size = framebuffer_capacity();
        if (size == 0u) {
            return WASMOS_ERR_XFER_BUFFER_NO_BACKING;
        }
        if (minimum_size > size) {
            return WASMOS_ERR_XFER_BUFFER_CAPACITY_EXCEEDED;
        }
    } else {
        /* TRANSFER buffers are right-sized to the request, rounded up to a whole
         * number of pages (backing is always mapped page-by-page), bounded by
         * XFER_TRANSFER_MAX_SIZE. */
        if (minimum_size > XFER_TRANSFER_MAX_SIZE) {
            return WASMOS_ERR_XFER_BUFFER_CAPACITY_EXCEEDED;
        }
        size = (minimum_size + (XFER_PAGE_SIZE - 1u)) & ~(XFER_PAGE_SIZE - 1u);
    }
    phys_base = object_alloc_backing(kind, size);
    /* Object backing is mapped as whole pages by callers, so the base must be
     * page-aligned. DMA device addresses (base + offset) may be sub-page. */
    if (phys_base == 0u || (phys_base & (XFER_PAGE_SIZE - 1u)) != 0u) {
        return WASMOS_ERR_XFER_BUFFER_NO_BACKING;
    }
    slot = object_alloc();
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_INTERNAL;
    }
    slot->active = 1u;
    slot->kind = kind;
    slot->buffer_id = g_next_buffer_id++;
    slot->size_bytes = size;
    slot->owner_context_id = owner_context_id;
    slot->phys_base = phys_base;
    slot->dma_active = 0u;
    slot->dma_offset = 0u;
    slot->dma_length = 0u;
    slot->dma_direction_flags = 0u;

    out_owner->buffer.kind = kind;
    out_owner->buffer.buffer_id = slot->buffer_id;
    out_owner->buffer.size_bytes = size;
    out_owner->owner_context_id = owner_context_id;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_get_owned_locked(const xfer_buffer_t* buffer, uint32_t context_id,
                                        xfer_buffer_owner_t* out_owner) {
    object_slot_t* slot = 0;

    if (!buffer || !out_owner) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    slot = object_find(buffer);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (slot->owner_context_id != context_id) {
        return WASMOS_ERR_XFER_BUFFER_NOT_OWNER;
    }
    out_owner->buffer.kind = slot->kind;
    out_owner->buffer.buffer_id = slot->buffer_id;
    out_owner->buffer.size_bytes = slot->size_bytes;
    out_owner->owner_context_id = context_id;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_release_owned_locked(const xfer_buffer_owner_t* owner) {
    object_slot_t* slot = 0;

    if (!owner) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    slot = object_find(&owner->buffer);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (slot->owner_context_id != owner->owner_context_id) {
        return WASMOS_ERR_XFER_BUFFER_NOT_OWNER;
    }
    if (slot->dma_active) {
        /* Owner-side DMA must be unmapped first (a device may still be reading
         * the object). Borrow-side DMA is force-cleared by the cascade below. */
        return WASMOS_ERR_XFER_BUFFER_DMA_MAPPED;
    }
    /* Transient unborrow: destroying the object cascade-revokes every borrow of
     * it. The owner holds the lifecycle (owner-push), so release alone tears the
     * whole borrow tree down; borrowers need not have unborrowed first. */
    object_revoke_all_borrows(slot->buffer_id);
    object_free_backing(slot);
    slot->active = 0u;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_transfer_ownership_locked(const xfer_buffer_owner_t* current_owner,
                                                 uint32_t new_owner_context_id) {
    object_slot_t* slot = 0;

    if (!current_owner) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    slot = object_find(&current_owner->buffer);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (slot->owner_context_id != current_owner->owner_context_id) {
        return WASMOS_ERR_XFER_BUFFER_NOT_OWNER;
    }
    if (new_owner_context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (new_owner_context_id == current_owner->owner_context_id) {
        return WASMOS_ERR_XFER_BUFFER_SAME_OWNER;
    }
    if (slot->kind != BUFFER_KIND_TRANSFER) {
        return WASMOS_ERR_XFER_BUFFER_KIND_NOT_TRANSFERABLE;
    }
    if (object_has_active_borrow(slot->buffer_id)) {
        return WASMOS_ERR_XFER_BUFFER_ACTIVE_BORROWS;
    }
    slot->owner_context_id = new_owner_context_id;
    return WASMOS_ERR_NONE;
}

static int attach_borrow(object_slot_t* object, uint32_t parent_borrow_id,
                         uint32_t lender_context_id, uint32_t borrower_context_id, uint32_t flags,
                         xfer_buffer_borrow_t* out_borrow) {
    borrow_slot_t* slot = borrow_alloc();

    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_INTERNAL;
    }
    slot->active = 1u;
    slot->borrow_id = g_next_borrow_id++;
    slot->parent_borrow_id = parent_borrow_id;
    slot->buffer_id = object->buffer_id;
    slot->kind = object->kind;
    slot->lender_context_id = lender_context_id;
    slot->borrower_context_id = borrower_context_id;
    slot->flags = flags;
    slot->dma_active = 0u;
    slot->dma_offset = 0u;
    slot->dma_length = 0u;
    slot->dma_direction_flags = 0u;

    out_borrow->buffer.kind = object->kind;
    out_borrow->buffer.buffer_id = object->buffer_id;
    out_borrow->buffer.size_bytes = object->size_bytes;
    out_borrow->lender_context_id = lender_context_id;
    out_borrow->borrower_context_id = borrower_context_id;
    out_borrow->flags = flags;
    out_borrow->borrow_id = slot->borrow_id;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_borrow_locked(const xfer_buffer_owner_t* owner, uint32_t borrower_context_id,
                                     uint32_t flags, xfer_buffer_borrow_t* out_borrow) {
    object_slot_t* slot = 0;

    if (!owner || !out_borrow) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (borrower_context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (!valid_flags(flags)) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_FLAGS;
    }
    slot = object_find(&owner->buffer);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (slot->owner_context_id != owner->owner_context_id) {
        return WASMOS_ERR_XFER_BUFFER_NOT_OWNER;
    }
    if (slot->kind == BUFFER_KIND_FRAMEBUFFER) {
        /* Framebuffer is local-only: only the owner may borrow it, and only one
         * borrow may be active at a time. */
        if (borrower_context_id != slot->owner_context_id) {
            return WASMOS_ERR_XFER_BUFFER_KIND_NOT_BORROWABLE;
        }
    } else {
        /* A transfer object may not be borrowed by its own owner. */
        if (borrower_context_id == slot->owner_context_id) {
            return WASMOS_ERR_XFER_BUFFER_SELF_BORROW;
        }
    }
    /* A borrower holds at most one active borrow per object. */
    if (borrow_find_for(slot->buffer_id, borrower_context_id)) {
        return WASMOS_ERR_XFER_BUFFER_ALREADY_BORROWED;
    }
    return attach_borrow(slot, 0u, slot->owner_context_id, borrower_context_id, flags, out_borrow);
}

static int xfer_buffer_reborrow_locked(const xfer_buffer_borrow_t* upstream,
                                       uint32_t borrower_context_id, uint32_t flags,
                                       xfer_buffer_borrow_t* out_borrow) {
    borrow_slot_t* up = 0;
    object_slot_t* object = 0;

    if (!upstream || !out_borrow) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (borrower_context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (!valid_flags(flags)) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_FLAGS;
    }
    up = borrow_find(upstream->borrow_id);
    if (!up) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    if ((flags & up->flags) != flags) {
        return WASMOS_ERR_XFER_BUFFER_RIGHTS_AMPLIFICATION;
    }
    if (up->kind == BUFFER_KIND_FRAMEBUFFER) {
        return WASMOS_ERR_XFER_BUFFER_NOT_REBORROWABLE;
    }
    object = object_find_by_id(up->buffer_id);
    if (!object) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    /* A borrower holds at most one active borrow per object. */
    if (borrow_find_for(up->buffer_id, borrower_context_id)) {
        return WASMOS_ERR_XFER_BUFFER_ALREADY_BORROWED;
    }
    return attach_borrow(object, up->borrow_id, up->borrower_context_id, borrower_context_id, flags,
                         out_borrow);
}

static int xfer_buffer_unborrow_locked(const xfer_buffer_borrow_t* borrow) {
    borrow_slot_t* slot = 0;

    if (!borrow) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    slot = borrow_find(borrow->borrow_id);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    /* The DMA window is the mapper's transient, operational concern (scoped to
     * the device access); a borrow can't be torn down while its DMA is still
     * mapped — the mapper must unmap it first. Buffer/borrow lifecycle (this
     * unborrow) is separate and the lender's. */
    if (slot->dma_active) {
        return WASMOS_ERR_XFER_BUFFER_DMA_MAPPED;
    }
    borrow_revoke_tree(slot->borrow_id);
    return WASMOS_ERR_NONE;
}

/* Resolve a borrow binding for its GRANTOR (owner-push): authorizes the context
 * that created the borrow — its lender (the owner for a top-level borrow, or the
 * upstream borrower for a reborrow). Sibling of xfer_buffer_get_borrowed (which
 * authorizes the borrower, e.g. for DMA); feed the result to
 * xfer_buffer_unborrow so the grantor — and only the grantor — may drop it. */
static int xfer_buffer_get_lent_locked(uint32_t borrow_id, uint32_t lender_context_id,
                                       xfer_buffer_borrow_t* out_borrow) {
    borrow_slot_t* slot = 0;
    object_slot_t* object = 0;

    if (!out_borrow) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (lender_context_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INVALID_CONTEXT;
    }
    if (borrow_id == 0u) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    slot = borrow_find(borrow_id);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    if (slot->lender_context_id != lender_context_id) {
        return WASMOS_ERR_XFER_BUFFER_NO_ACCESS;
    }
    object = object_find_by_id(slot->buffer_id);
    if (!object) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    out_borrow->buffer.kind = slot->kind;
    out_borrow->buffer.buffer_id = slot->buffer_id;
    out_borrow->buffer.size_bytes = object->size_bytes;
    out_borrow->lender_context_id = slot->lender_context_id;
    out_borrow->borrower_context_id = slot->borrower_context_id;
    out_borrow->flags = slot->flags;
    out_borrow->borrow_id = slot->borrow_id;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_can_access_locked(const xfer_buffer_t* buffer, uint32_t accessor_context_id,
                                         uint32_t requested_flags) {
    object_slot_t* slot = object_find(buffer);
    borrow_slot_t* borrow = 0;

    if (!slot || accessor_context_id == 0u || !valid_flags(requested_flags)) {
        return 0;
    }
    if (slot->owner_context_id == accessor_context_id) {
        return 1;
    }
    borrow = borrow_find_for(slot->buffer_id, accessor_context_id);
    if (!borrow) {
        return 0;
    }
    return (borrow->flags & requested_flags) == requested_flags;
}

static int xfer_buffer_same_object_locked(const xfer_buffer_t* buffer, uint32_t accessor_context_id,
                                          uint32_t owner_context_id) {
    object_slot_t* slot = object_find(buffer);

    if (!slot || accessor_context_id == 0u || owner_context_id == 0u) {
        return 0;
    }
    if (slot->owner_context_id != owner_context_id) {
        return 0;
    }
    if (accessor_context_id == slot->owner_context_id) {
        return 1;
    }
    return borrow_find_for(slot->buffer_id, accessor_context_id) != 0;
}

static int xfer_buffer_dma_map_owned_locked(const xfer_buffer_owner_t* owner, uint32_t offset,
                                            uint32_t length, uint32_t direction_flags,
                                            xfer_buffer_dma_mapping_t* out_mapping) {
    object_slot_t* slot = 0;

    if (!owner || !out_mapping) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    slot = object_find(&owner->buffer);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (slot->owner_context_id != owner->owner_context_id) {
        return WASMOS_ERR_XFER_BUFFER_NOT_OWNER;
    }
    if (!range_within(slot->size_bytes, offset, length)) {
        return WASMOS_ERR_XFER_BUFFER_RANGE;
    }
    /* Owner has implicit read/write, so any nonzero direction is permitted. */
    if (direction_flags == 0u) {
        return WASMOS_ERR_XFER_BUFFER_DIRECTION;
    }
    if (slot->dma_active) {
        return WASMOS_ERR_XFER_BUFFER_DMA_ACTIVE;
    }
    slot->dma_active = 1u;
    slot->dma_offset = offset;
    slot->dma_length = length;
    slot->dma_direction_flags = direction_flags;

    out_mapping->buffer.kind = slot->kind;
    out_mapping->buffer.buffer_id = slot->buffer_id;
    out_mapping->buffer.size_bytes = slot->size_bytes;
    out_mapping->owner_context_id = slot->owner_context_id;
    out_mapping->borrow_id = 0u;
    out_mapping->offset = offset;
    out_mapping->length = length;
    out_mapping->direction_flags = direction_flags;
    out_mapping->device_addr = slot->phys_base + (uint64_t)offset;
    out_mapping->attached_via_borrow = 0u;
    out_mapping->active = 1u;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_dma_map_borrow_locked(const xfer_buffer_borrow_t* borrow, uint32_t offset,
                                             uint32_t length, uint32_t direction_flags,
                                             xfer_buffer_dma_mapping_t* out_mapping) {
    borrow_slot_t* slot = 0;
    object_slot_t* object = 0;

    if (!borrow || !out_mapping) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    slot = borrow_find(borrow->borrow_id);
    if (!slot) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_BORROW;
    }
    object = object_find_by_id(slot->buffer_id);
    if (!object) {
        return WASMOS_ERR_XFER_BUFFER_NOT_FOUND;
    }
    if (!range_within(object->size_bytes, offset, length)) {
        return WASMOS_ERR_XFER_BUFFER_RANGE;
    }
    if (!dma_direction_allowed(slot->flags, direction_flags)) {
        return WASMOS_ERR_XFER_BUFFER_DIRECTION;
    }
    if (slot->dma_active) {
        return WASMOS_ERR_XFER_BUFFER_DMA_ACTIVE;
    }
    slot->dma_active = 1u;
    slot->dma_offset = offset;
    slot->dma_length = length;
    slot->dma_direction_flags = direction_flags;

    out_mapping->buffer.kind = object->kind;
    out_mapping->buffer.buffer_id = object->buffer_id;
    out_mapping->buffer.size_bytes = object->size_bytes;
    out_mapping->owner_context_id = 0u;
    out_mapping->borrow_id = slot->borrow_id;
    out_mapping->offset = offset;
    out_mapping->length = length;
    out_mapping->direction_flags = direction_flags;
    out_mapping->device_addr = object->phys_base + (uint64_t)offset;
    out_mapping->attached_via_borrow = 1u;
    out_mapping->active = 1u;
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_dma_sync_locked(const xfer_buffer_dma_mapping_t* mapping, uint32_t offset,
                                       uint32_t length) {
    if (!mapping) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (!mapping->active) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_MAPPING;
    }
    if (!range_within(mapping->length, offset, length)) {
        return WASMOS_ERR_XFER_BUFFER_RANGE;
    }
    return WASMOS_ERR_NONE;
}

static int xfer_buffer_dma_unmap_locked(xfer_buffer_dma_mapping_t* mapping) {
    if (!mapping) {
        return WASMOS_ERR_XFER_BUFFER_NULL_ARG;
    }
    if (!mapping->active) {
        return WASMOS_ERR_XFER_BUFFER_INACTIVE_MAPPING;
    }
    if (mapping->attached_via_borrow) {
        borrow_slot_t* slot = borrow_find(mapping->borrow_id);
        if (slot) {
            slot->dma_active = 0u;
        }
    } else {
        object_slot_t* slot = object_find_by_id(mapping->buffer.buffer_id);
        if (slot) {
            slot->dma_active = 0u;
        }
    }
    mapping->active = 0u;
    return WASMOS_ERR_NONE;
}

static void xfer_buffer_drop_context_locked(uint32_t context_id) {
    list_iter_t it;
    borrow_slot_t* borrow = 0;
    object_slot_t* object = 0;

    if (context_id == 0u || registry_init_once() != 0) {
        return;
    }
    /* Revoke every borrow this context issued (as lender) or holds (as
     * borrower), cascading through downstream reborrows. */
    borrow = (borrow_slot_t*)list_first(&g_borrows, &it);
    while (borrow) {
        if (borrow->active && (borrow->lender_context_id == context_id ||
                               borrow->borrower_context_id == context_id)) {
            borrow_revoke_tree(borrow->borrow_id);
        }
        borrow = (borrow_slot_t*)list_next(&it);
    }
    /* Destroy objects owned by this context, revoking any borrows still rooted
     * in them and freeing their backing. */
    object = (object_slot_t*)list_first(&g_objects, &it);
    while (object) {
        if (object->active && object->owner_context_id == context_id) {
            list_iter_t bit;
            borrow_slot_t* b = (borrow_slot_t*)list_first(&g_borrows, &bit);
            while (b) {
                if (b->active && b->buffer_id == object->buffer_id) {
                    borrow_revoke_tree(b->borrow_id);
                }
                b = (borrow_slot_t*)list_next(&bit);
            }
            object_free_backing(object);
            object->active = 0u;
        }
        object = (object_slot_t*)list_next(&it);
    }
}

/* -------------------------------------------------------------------------
 * Public API: SMP-locking wrappers around the *_locked registry cores.
 * Each takes g_xfer_lock for the whole operation so concurrent acquire/
 * release/borrow/drop_context across CPUs cannot corrupt the object/borrow
 * lists. xfer_buffer_size() is intentionally lock-free (it touches no
 * registry state).
 * ---------------------------------------------------------------------- */

int xfer_buffer_acquire(uint32_t kind, uint32_t owner_context_id, uint32_t minimum_size,
                        xfer_buffer_owner_t* out_owner) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_acquire_locked(kind, owner_context_id, minimum_size, out_owner);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_get_owned(const xfer_buffer_t* buffer, uint32_t context_id,
                          xfer_buffer_owner_t* out_owner) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_get_owned_locked(buffer, context_id, out_owner);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_release_owned(const xfer_buffer_owner_t* owner) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_release_owned_locked(owner);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_transfer_ownership(const xfer_buffer_owner_t* current_owner,
                                   uint32_t new_owner_context_id) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_transfer_ownership_locked(current_owner, new_owner_context_id);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_borrow(const xfer_buffer_owner_t* owner, uint32_t borrower_context_id,
                       uint32_t flags, xfer_buffer_borrow_t* out_borrow) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_borrow_locked(owner, borrower_context_id, flags, out_borrow);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_reborrow(const xfer_buffer_borrow_t* upstream, uint32_t borrower_context_id,
                         uint32_t flags, xfer_buffer_borrow_t* out_borrow) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_reborrow_locked(upstream, borrower_context_id, flags, out_borrow);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_unborrow(const xfer_buffer_borrow_t* borrow) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_unborrow_locked(borrow);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_get_lent(uint32_t borrow_id, uint32_t lender_context_id,
                         xfer_buffer_borrow_t* out_borrow) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_get_lent_locked(borrow_id, lender_context_id, out_borrow);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_can_access(const xfer_buffer_t* buffer, uint32_t accessor_context_id,
                           uint32_t requested_flags) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_can_access_locked(buffer, accessor_context_id, requested_flags);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_same_object(const xfer_buffer_t* buffer, uint32_t accessor_context_id,
                            uint32_t owner_context_id) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_same_object_locked(buffer, accessor_context_id, owner_context_id);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_dma_map_owned(const xfer_buffer_owner_t* owner, uint32_t offset, uint32_t length,
                              uint32_t direction_flags, xfer_buffer_dma_mapping_t* out_mapping) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_dma_map_owned_locked(owner, offset, length, direction_flags, out_mapping);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_dma_map_borrow(const xfer_buffer_borrow_t* borrow, uint32_t offset, uint32_t length,
                               uint32_t direction_flags, xfer_buffer_dma_mapping_t* out_mapping) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc =
        xfer_buffer_dma_map_borrow_locked(borrow, offset, length, direction_flags, out_mapping);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_dma_sync(const xfer_buffer_dma_mapping_t* mapping, uint32_t offset,
                         uint32_t length) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_dma_sync_locked(mapping, offset, length);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_dma_unmap(xfer_buffer_dma_mapping_t* mapping) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_dma_unmap_locked(mapping);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

void xfer_buffer_drop_context(uint32_t context_id) {
    ksync_spinlock_lock(&g_xfer_lock);
    xfer_buffer_drop_context_locked(context_id);
    ksync_spinlock_unlock(&g_xfer_lock);
}

uint64_t xfer_buffer_object_phys(const xfer_buffer_t* buffer) {
    ksync_spinlock_lock(&g_xfer_lock);
    uint64_t phys = xfer_buffer_object_phys_locked(buffer);
    ksync_spinlock_unlock(&g_xfer_lock);
    return phys;
}

int xfer_buffer_describe(uint32_t buffer_id, uint32_t kind, uint32_t context_id,
                         xfer_buffer_t* out) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_describe_locked(buffer_id, kind, context_id, out);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}

int xfer_buffer_get_borrowed(uint32_t borrow_id, uint32_t context_id,
                             xfer_buffer_borrow_t* out_borrow,
                             xfer_buffer_dma_mapping_t* out_mapping) {
    ksync_spinlock_lock(&g_xfer_lock);
    int rc = xfer_buffer_get_borrowed_locked(borrow_id, context_id, out_borrow, out_mapping);
    ksync_spinlock_unlock(&g_xfer_lock);
    return rc;
}