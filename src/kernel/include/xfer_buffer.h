#ifndef WASMOS_XFER_BUFFER_H
#define WASMOS_XFER_BUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file xfer_buffer.h
 * @brief Object/owner/borrow xfer-buffer model.
 *
 * <p>This is the real xfer-buffer subsystem surface. A xfer buffer is a
 * first-class object with a single current owner. Ownership can be transferred,
 * the owner can lend the object to any number of borrowers, and each borrow is
 * an independent first-class handle with its own rights, lifetime, reborrow
 * tree, and optional DMA attachment.
 *
 * <p>Core model:
 *
 * <ul>
 * <li>A xfer buffer is a distinct object with one current owner.</li>
 * <li>The owner may borrow the object to other contexts without giving up
 *     ownership. A borrower can hold multiple simultaneous borrows to distinct
 *     objects; there is no one-borrow-per-context slot.</li>
 * <li>A borrower may reborrow only from an active borrow it holds, and may not
 *     amplify rights beyond what it already has.</li>
 * <li>Ownership may also be transferred. After transfer, the recipient becomes
 *     the new owner and the previous owner is no longer the owner.</li>
 * <li>Only the current owner may destroy or release the buffer object itself.
 *     Borrowers release borrow handles; they never destroy the object.</li>
 * </ul>
 *
 * <p>Kind rules:
 *
 * <ul>
 * <li>{@code BUFFER_KIND_TRANSFER} is a generic transferable bulk-data
 *     buffer.</li>
 * <li>{@code BUFFER_KIND_FRAMEBUFFER} is a special local-only backing class:
 *     it may only be borrowed by its own owner, may not be reborrowed, and may
 *     not be transferred between owners.</li>
 * </ul>
 *
 * <p>DMA rules:
 *
 * <ul>
 * <li>DMA attaches either to one owned buffer object or to one specific active
 *     borrow handle, never to "the borrower context as a whole".</li>
 * <li>DMA attached through a borrow must never outlive that borrow.</li>
 * <li>Owner-side release is rejected while owner-side DMA is still mapped.</li>
 * <li>Borrow-side unborrow is rejected while that borrow's DMA is still
 *     mapped.</li>
 * <li>DMA direction requirements are derived from access rights: read required
 *     for {@code TO_DEVICE}, write required for {@code FROM_DEVICE}, both
 *     required for bidirectional DMA.</li>
 * </ul>
 */

/* Transfer-buffer kinds. */
#define BUFFER_KIND_TRANSFER    1u
#define BUFFER_KIND_FRAMEBUFFER 2u

/* Borrow access rights. */
#define BUFFER_BORROW_READ  0x1u
#define BUFFER_BORROW_WRITE 0x2u

/**
 * Result codes for xfer-buffer operations.
 *
 * <p>All operations returning {@code int} return {@code XFER_BUFFER_OK} (0) on
 * success and a distinct negative {@code XFER_BUFFER_ERR_*} value naming the
 * specific failure. Callers should compare against {@code XFER_BUFFER_OK}
 * rather than testing for a bare {@code -1}.
 */
typedef enum {
    XFER_BUFFER_OK = 0,
    /* A required pointer argument was NULL. */
    XFER_BUFFER_ERR_NULL_ARG = -1,
    /* Unknown or unsupported buffer kind. */
    XFER_BUFFER_ERR_INVALID_KIND = -2,
    /* A context id argument was zero. */
    XFER_BUFFER_ERR_INVALID_CONTEXT = -3,
    /* A requested size was zero. */
    XFER_BUFFER_ERR_INVALID_SIZE = -4,
    /* Requested size exceeds the kind's intrinsic capacity. */
    XFER_BUFFER_ERR_CAPACITY_EXCEEDED = -5,
    /* No physical backing could be obtained for the object. */
    XFER_BUFFER_ERR_NO_BACKING = -6,
    /* Registry storage could not be initialized or grown. */
    XFER_BUFFER_ERR_INTERNAL = -7,
    /* The referenced object does not exist (stale or destroyed). */
    XFER_BUFFER_ERR_NOT_FOUND = -8,
    /* The binding does not match the object's current owner. */
    XFER_BUFFER_ERR_NOT_OWNER = -9,
    /* Requested access flags are empty or contain invalid bits. */
    XFER_BUFFER_ERR_INVALID_FLAGS = -10,
    /* A transfer object cannot be borrowed by its own owner. */
    XFER_BUFFER_ERR_SELF_BORROW = -11,
    /* The borrower already holds an active borrow on this object. */
    XFER_BUFFER_ERR_ALREADY_BORROWED = -12,
    /* The kind may not be borrowed by the requested borrower. */
    XFER_BUFFER_ERR_KIND_NOT_BORROWABLE = -13,
    /* The kind does not support ownership transfer. */
    XFER_BUFFER_ERR_KIND_NOT_TRANSFERABLE = -14,
    /* The object still has active borrows. */
    XFER_BUFFER_ERR_ACTIVE_BORROWS = -15,
    /* DMA is still mapped and blocks this operation. */
    XFER_BUFFER_ERR_DMA_MAPPED = -16,
    /* Ownership transfer to the current owner is a no-op and rejected. */
    XFER_BUFFER_ERR_SAME_OWNER = -17,
    /* Downstream rights would exceed the upstream borrow's rights. */
    XFER_BUFFER_ERR_RIGHTS_AMPLIFICATION = -18,
    /* The kind may not be reborrowed. */
    XFER_BUFFER_ERR_NOT_REBORROWABLE = -19,
    /* The referenced borrow is inactive, stale, or forged. */
    XFER_BUFFER_ERR_INACTIVE_BORROW = -20,
    /* An offset/length subrange lies outside its bounds. */
    XFER_BUFFER_ERR_RANGE = -21,
    /* The DMA direction is not permitted by the access rights. */
    XFER_BUFFER_ERR_DIRECTION = -22,
    /* A DMA mapping is already active on this object or borrow. */
    XFER_BUFFER_ERR_DMA_ACTIVE = -23,
    /* The DMA mapping is inactive. */
    XFER_BUFFER_ERR_INACTIVE_MAPPING = -24,
    /* The object exists but the context is neither its owner nor a borrower. */
    XFER_BUFFER_ERR_NO_ACCESS = -25
} xfer_buffer_status_t;

/**
 * Stable descriptor of a buffer object, independent of its current owner or
 * borrow graph. {@code size_bytes} is the intrinsic capacity of the object and
 * the upper bound for borrow access, transfers, and DMA subranges.
 */
typedef struct {
    uint32_t kind;
    uint32_t buffer_id;
    uint32_t size_bytes;
} xfer_buffer_t;

/**
 * Current owner binding for a buffer object. Ownership grants implicit
 * read/write access and the right to borrow, transfer, and destroy the object.
 */
typedef struct {
    xfer_buffer_t buffer;
    uint32_t owner_context_id;
} xfer_buffer_owner_t;

/**
 * One active borrow edge in the borrow graph: a specific access grant from one
 * lender context (owner or an upstream borrower) to one borrower context over
 * one buffer object.
 */
typedef struct {
    xfer_buffer_t buffer;
    uint32_t lender_context_id;
    uint32_t borrower_context_id;
    uint32_t flags;
    uint32_t borrow_id;
} xfer_buffer_borrow_t;

/**
 * Active DMA attachment, either to an owner binding or to a specific active
 * borrow. {@code attached_via_borrow} distinguishes the two modes.
 */
typedef struct {
    xfer_buffer_t buffer;
    uint32_t owner_context_id;
    uint32_t borrow_id;
    uint32_t offset;
    uint32_t length;
    uint32_t direction_flags;
    uint64_t device_addr;
    uint8_t attached_via_borrow;
    uint8_t active;
} xfer_buffer_dma_mapping_t;

/**
 * Intrinsic capacity of a buffer kind in bytes, or 0 for unknown kinds.
 */
uint32_t xfer_buffer_size(uint32_t kind);

/**
 * Acquire or create a buffer object owned by a context.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code out_owner} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_KIND} - {@code kind} is neither
 *     {@code BUFFER_KIND_TRANSFER} nor {@code BUFFER_KIND_FRAMEBUFFER}.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_CONTEXT} - {@code owner_context_id} is
 *     zero.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_SIZE} - {@code minimum_size} is zero.</li>
 * <li>{@code XFER_BUFFER_ERR_CAPACITY_EXCEEDED} - {@code minimum_size} exceeds
 *     the kind's intrinsic capacity.</li>
 * <li>{@code XFER_BUFFER_ERR_NO_BACKING} - the kind has no capacity available
 *     or no physical backing could be obtained for the object.</li>
 * <li>{@code XFER_BUFFER_ERR_INTERNAL} - the object registry could not be
 *     initialized or a slot could not be allocated.</li>
 * </ul>
 */
int xfer_buffer_acquire(uint32_t kind,
                        uint32_t owner_context_id,
                        uint32_t minimum_size,
                        xfer_buffer_owner_t *out_owner);

/**
 * Retrieve the current owner binding for a buffer object as seen by one
 * context. Does not claim ownership and does not modify state.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code buffer} or {@code out_owner} is
 *     NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_CONTEXT} - {@code context_id} is zero.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - no such object exists (stale or
 *     destroyed descriptor, or mismatched kind).</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_OWNER} - {@code context_id} is not the object's
 *     current owner.</li>
 * </ul>
 */
int xfer_buffer_get_owned(const xfer_buffer_t *buffer,
                          uint32_t context_id,
                          xfer_buffer_owner_t *out_owner);

/**
 * Destroy an owned buffer object. Only the current owner may destroy it.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code owner} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - the object does not exist (stale or
 *     already-destroyed binding).</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_OWNER} - the binding does not match the
 *     object's current owner (for example a stale pre-transfer owner).</li>
 * <li>{@code XFER_BUFFER_ERR_ACTIVE_BORROWS} - the object still has one or more
 *     active borrows; release those first.</li>
 * <li>{@code XFER_BUFFER_ERR_DMA_MAPPED} - owner-side DMA is still mapped on the
 *     object; unmap it first.</li>
 * </ul>
 */
int xfer_buffer_release_owned(const xfer_buffer_owner_t *owner);

/**
 * Transfer ownership of a buffer object to another context.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code current_owner} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - the object does not exist (stale or
 *     destroyed binding, or mismatched kind).</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_OWNER} - the binding does not match the
 *     object's current owner.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_CONTEXT} - {@code new_owner_context_id} is
 *     zero.</li>
 * <li>{@code XFER_BUFFER_ERR_SAME_OWNER} - {@code new_owner_context_id} is
 *     already the current owner.</li>
 * <li>{@code XFER_BUFFER_ERR_KIND_NOT_TRANSFERABLE} - the object kind (for
 *     example {@code BUFFER_KIND_FRAMEBUFFER}) cannot change owners.</li>
 * <li>{@code XFER_BUFFER_ERR_ACTIVE_BORROWS} - the object still has active
 *     borrows; transfer is only allowed once all borrows are gone.</li>
 * </ul>
 */
int xfer_buffer_transfer_ownership(const xfer_buffer_owner_t *current_owner,
                                   uint32_t new_owner_context_id);

/**
 * Borrow a buffer object from its current owner. A borrower may hold multiple
 * simultaneous borrows to distinct objects, but at most one active borrow per
 * object.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code owner} or {@code out_borrow} is
 *     NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_CONTEXT} - {@code borrower_context_id} is
 *     zero.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_FLAGS} - {@code flags} is empty or contains
 *     bits outside {@code BUFFER_BORROW_READ | BUFFER_BORROW_WRITE}.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - the object does not exist (stale or
 *     destroyed owner binding).</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_OWNER} - the binding does not match the
 *     object's current owner.</li>
 * <li>{@code XFER_BUFFER_ERR_KIND_NOT_BORROWABLE} - a framebuffer object was
 *     borrowed by a context other than its owner (framebuffers are
 *     local-only).</li>
 * <li>{@code XFER_BUFFER_ERR_SELF_BORROW} - a transfer object's own owner tried
 *     to borrow it.</li>
 * <li>{@code XFER_BUFFER_ERR_ALREADY_BORROWED} - the borrower already holds an
 *     active borrow on this object.</li>
 * <li>{@code XFER_BUFFER_ERR_INTERNAL} - a borrow slot could not be
 *     allocated.</li>
 * </ul>
 */
int xfer_buffer_borrow(const xfer_buffer_owner_t *owner,
                       uint32_t borrower_context_id,
                       uint32_t flags,
                       xfer_buffer_borrow_t *out_borrow);

/**
 * Reborrow a buffer object from an existing active borrow. Downstream rights
 * may not exceed the upstream borrow's rights.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code upstream} or {@code out_borrow}
 *     is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_CONTEXT} - {@code borrower_context_id} is
 *     zero.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_FLAGS} - {@code flags} is empty or contains
 *     bits outside {@code BUFFER_BORROW_READ | BUFFER_BORROW_WRITE}.</li>
 * <li>{@code XFER_BUFFER_ERR_INACTIVE_BORROW} - the upstream borrow is inactive,
 *     stale, or forged.</li>
 * <li>{@code XFER_BUFFER_ERR_RIGHTS_AMPLIFICATION} - {@code flags} requests
 *     rights the upstream borrow does not hold.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_REBORROWABLE} - the upstream borrow's kind (for
 *     example {@code BUFFER_KIND_FRAMEBUFFER}) may not be reborrowed.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - the underlying object no longer
 *     exists.</li>
 * <li>{@code XFER_BUFFER_ERR_ALREADY_BORROWED} - the borrower already holds an
 *     active borrow on this object.</li>
 * <li>{@code XFER_BUFFER_ERR_INTERNAL} - a borrow slot could not be
 *     allocated.</li>
 * </ul>
 */
int xfer_buffer_reborrow(const xfer_buffer_borrow_t *upstream,
                         uint32_t borrower_context_id,
                         uint32_t flags,
                         xfer_buffer_borrow_t *out_borrow);

/**
 * Remove one active borrow handle. Cascade-revokes all downstream reborrows
 * derived from it. Does not destroy the buffer object.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code borrow} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INACTIVE_BORROW} - the handle is inactive, stale,
 *     forged, or was already cascade-revoked by an upstream unborrow.</li>
 * <li>{@code XFER_BUFFER_ERR_DMA_MAPPED} - this borrow still has DMA mapped;
 *     unmap it first.</li>
 * </ul>
 */
int xfer_buffer_unborrow(const xfer_buffer_borrow_t *borrow);

/**
 * Whether a context currently holds at least the requested access rights to a
 * specific buffer object. The owner satisfies any valid request; borrowers
 * satisfy only their granted rights.
 *
 * @return 1 when access is currently allowed; 0 otherwise.
 */
int xfer_buffer_can_access(const xfer_buffer_t *buffer,
                           uint32_t accessor_context_id,
                           uint32_t requested_flags);

/**
 * Whether an accessor and an owner both currently resolve to the same
 * underlying buffer object (identity is preserved across borrow/reborrow).
 *
 * @return 1 when both resolve to the same object; 0 otherwise.
 */
int xfer_buffer_same_object(const xfer_buffer_t *buffer,
                            uint32_t accessor_context_id,
                            uint32_t owner_context_id);

/**
 * Attach DMA state to one owned buffer object (owner-initiated). The owner has
 * implicit read/write rights, so any nonzero direction is permitted.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code owner} or {@code out_mapping} is
 *     NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - the object does not exist (stale or
 *     destroyed owner binding).</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_OWNER} - the binding does not match the
 *     object's current owner.</li>
 * <li>{@code XFER_BUFFER_ERR_RANGE} - {@code length} is zero or the
 *     {@code [offset, offset+length)} range falls outside the object.</li>
 * <li>{@code XFER_BUFFER_ERR_DIRECTION} - {@code direction_flags} is zero.</li>
 * <li>{@code XFER_BUFFER_ERR_DMA_ACTIVE} - the object already has an active
 *     owner-side DMA mapping.</li>
 * </ul>
 */
int xfer_buffer_dma_map_owned(const xfer_buffer_owner_t *owner,
                              uint32_t offset,
                              uint32_t length,
                              uint32_t direction_flags,
                              xfer_buffer_dma_mapping_t *out_mapping);

/**
 * Attach DMA state to one active borrow (borrower-initiated). Direction
 * requirements are derived from the borrow's rights: {@code TO_DEVICE} requires
 * read, {@code FROM_DEVICE} requires write, bidirectional requires both.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code borrow} or {@code out_mapping}
 *     is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INACTIVE_BORROW} - the borrow handle is inactive,
 *     stale, or forged.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - the underlying object no longer
 *     exists.</li>
 * <li>{@code XFER_BUFFER_ERR_RANGE} - {@code length} is zero or the
 *     {@code [offset, offset+length)} range falls outside the object.</li>
 * <li>{@code XFER_BUFFER_ERR_DIRECTION} - {@code direction_flags} is zero or
 *     requests a direction the borrow's rights do not permit.</li>
 * <li>{@code XFER_BUFFER_ERR_DMA_ACTIVE} - this borrow already has an active DMA
 *     mapping.</li>
 * </ul>
 */
int xfer_buffer_dma_map_borrow(const xfer_buffer_borrow_t *borrow,
                               uint32_t offset,
                               uint32_t length,
                               uint32_t direction_flags,
                               xfer_buffer_dma_mapping_t *out_mapping);

/**
 * Synchronize a subrange of an active DMA mapping. {@code offset} and
 * {@code length} are relative to the mapped range, not the whole object.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code mapping} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INACTIVE_MAPPING} - the mapping is not active (for
 *     example already unmapped).</li>
 * <li>{@code XFER_BUFFER_ERR_RANGE} - {@code length} is zero or the subrange
 *     falls outside the mapped range.</li>
 * </ul>
 */
int xfer_buffer_dma_sync(const xfer_buffer_dma_mapping_t *mapping,
                         uint32_t offset,
                         uint32_t length);

/**
 * Remove DMA state from one active mapping. Never destroys the object and never
 * by itself drops the underlying borrow. On success the mapping is marked
 * inactive, so a second unmap of the same mapping is rejected.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code mapping} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INACTIVE_MAPPING} - the mapping is not active (for
 *     example already unmapped).</li>
 * </ul>
 */
int xfer_buffer_dma_unmap(xfer_buffer_dma_mapping_t *mapping);

/**
 * Tear down all xfer-buffer state rooted in one context: destroys owner
 * bindings owned by it, and cascade-revokes borrows it issued or holds
 * (including downstream reborrows and their DMA state).
 */
void xfer_buffer_drop_context(uint32_t context_id);

/**
 * Physical base address of a buffer object's backing, or 0 if the object does
 * not exist. Used by kernel code that must map or DMA the backing directly.
 */
uint64_t xfer_buffer_object_phys(const xfer_buffer_t *buffer);

/**
 * Resolve a bare {@code buffer_id} (as carried across the syscall/IPC boundary)
 * to its full descriptor, but only for a context that may actually touch it.
 *
 * <p>Userspace names buffers by their integer {@code buffer_id}. A syscall
 * handler passes the caller's {@code context_id} so this both recovers the
 * {@code kind}/{@code size_bytes} needed to bounds-check and enforces that the
 * caller is the object's owner or an active borrower. The specific right for an
 * operation is still checked with {@code xfer_buffer_can_access}.
 *
 * @return {@code XFER_BUFFER_OK} on success. On failure, one of:
 *
 * <ul>
 * <li>{@code XFER_BUFFER_ERR_NULL_ARG} - {@code out} is NULL.</li>
 * <li>{@code XFER_BUFFER_ERR_INVALID_CONTEXT} - {@code context_id} is zero.</li>
 * <li>{@code XFER_BUFFER_ERR_NOT_FOUND} - no live object has that id and
 *     kind.</li>
 * <li>{@code XFER_BUFFER_ERR_NO_ACCESS} - the object exists but the context is
 *     neither its owner nor an active borrower.</li>
 * </ul>
 */
int xfer_buffer_describe(uint32_t buffer_id,
                         uint32_t kind,
                         uint32_t context_id,
                         xfer_buffer_t *out);

#ifdef __cplusplus
}
#endif

#endif