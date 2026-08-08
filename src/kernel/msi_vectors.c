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

int msi_vectors_dispatch(msi_vector_t* vectors, uint32_t count, uint32_t index,
                         const msi_vector_ops_t* ops) {
    if (!vectors || index >= count || !vectors[index].in_use || !ops || !ops->deliver) {
        return 0;
    }
    return ops->deliver(vectors[index].endpoint, index) == 0 ? 1 : 0;
}

int msi_vectors_in_use(const msi_vector_t* vectors, uint32_t count, uint32_t index) {
    if (!vectors || index >= count) {
        return 0;
    }
    return vectors[index].in_use ? 1 : 0;
}
