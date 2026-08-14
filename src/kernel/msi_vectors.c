/* msi_vectors.c - MSI vector bookkeeping. See msi_vectors.h for the model and
 * why it is so much smaller than the INTx one. Pure logic: the vector table and
 * delivery are supplied by the caller, so this compiles and is unit-testable on
 * the host. */
#include "msi_vectors.h"

void msi_vectors_init(msi_vector_t* vectors, uint32_t count) {
    if (!vectors) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        vectors[i].in_use = 0;
        vectors[i].owner_context_id = 0;
        vectors[i].endpoint = 0;
    }
}

/* Claims the lowest free vector for context_id and records the endpoint that
 * will receive its notifications.  Returns 0 on success with the index in
 * *out_index, or the packed WASMOS_ERR_MSI_NO_VECTORS for a NULL argument and
 * for a table with nothing free.  No validation is done on context_id or
 * endpoint; a zero owner is indistinguishable from a released slot's owner
 * field, so allocating for context 0 is not meaningful. */
int msi_vectors_alloc(msi_vector_t* vectors, uint32_t count, uint32_t context_id, uint32_t endpoint,
                      uint32_t* out_index) {
    if (!vectors || !out_index) {
        return WASMOS_ERR_MSI_NO_VECTORS;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (vectors[i].in_use) {
            continue;
        }
        vectors[i].in_use = 1;
        vectors[i].owner_context_id = context_id;
        vectors[i].endpoint = endpoint;
        *out_index = i;
        return 0;
    }
    return WASMOS_ERR_MSI_NO_VECTORS;
}

/* Releases a vector, but only to its owner.  Returns 0 on success,
 * WASMOS_ERR_MSI_BAD_VECTOR for a NULL table, an out-of-range index or a vector
 * that is already free, and WASMOS_ERR_MSI_NOT_OWNER when context_id is not the
 * recorded owner — so a double free is reported as a bad vector rather than
 * silently accepted.  Only the table entry is cleared; the device is not
 * reprogrammed and can still raise the vector. */
int msi_vectors_free(msi_vector_t* vectors, uint32_t count, uint32_t index, uint32_t context_id) {
    if (!vectors || index >= count || !vectors[index].in_use) {
        return WASMOS_ERR_MSI_BAD_VECTOR;
    }
    if (vectors[index].owner_context_id != context_id) {
        return WASMOS_ERR_MSI_NOT_OWNER;
    }
    vectors[index].in_use = 0;
    vectors[index].owner_context_id = 0;
    vectors[index].endpoint = 0;
    return 0;
}

/* Frees every vector owned by context_id, for teardown of a dying driver.
 * Unlike msi_vectors_free this performs no ownership check beyond the match
 * itself and reports nothing, so it is safe to call for a context that owns
 * none. */
void msi_vectors_release_context(msi_vector_t* vectors, uint32_t count, uint32_t context_id) {
    if (!vectors) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (vectors[i].in_use && vectors[i].owner_context_id == context_id) {
            vectors[i].in_use = 0;
            vectors[i].owner_context_id = 0;
            vectors[i].endpoint = 0;
        }
    }
}

/* Delivers a notification for one vector through the caller-supplied ops.  Note
 * the polarity: 1 means DELIVERED, 0 means not — the opposite of the 0-on-success
 * convention used by alloc/free, and of ops->deliver's own return, which this
 * inverts.
 *
 * 0 covers every reason for not delivering: a NULL table or ops, a missing
 * deliver hook, an out-of-range index, an unallocated vector, and a deliver that
 * itself failed.  Nothing is logged; a spurious interrupt on a free vector is
 * expected to be silent. */
int msi_vectors_dispatch(msi_vector_t* vectors, uint32_t count, uint32_t index,
                         const msi_vector_ops_t* ops) {
    if (!vectors || index >= count || !vectors[index].in_use || !ops || !ops->deliver) {
        return 0;
    }
    return ops->deliver(vectors[index].endpoint, index) == 0 ? 1 : 0;
}

/* 1 when the vector is allocated, 0 when it is free or the arguments are out of
 * range.  A predicate, not a status code. */
int msi_vectors_in_use(const msi_vector_t* vectors, uint32_t count, uint32_t index) {
    if (!vectors || index >= count) {
        return 0;
    }
    return vectors[index].in_use ? 1 : 0;
}
