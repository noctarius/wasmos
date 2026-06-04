#include "user_mutex.h"

#include "memory.h"
#include "spinlock.h"

static spinlock_t g_user_mutex_lock;

static int
user_mutex_access(uint32_t context_id,
                  uint64_t user_addr,
                  uint32_t tid,
                  int (*op)(user_mutex_state_t *state, uint32_t tid),
                  user_mutex_state_t *out_state)
{
    user_mutex_state_t state = {0};
    int rc = -1;

    if (context_id == 0u || user_addr == 0u || !op) {
        return -1;
    }
    if ((user_addr & (uint64_t)(sizeof(uint32_t) - 1u)) != 0u) {
        return -1;
    }

    spinlock_lock(&g_user_mutex_lock);
    if (mm_copy_from_user(context_id, &state, user_addr, (uint64_t)sizeof(state)) == 0) {
        rc = op(&state, tid);
        if (rc >= 0 &&
            mm_copy_to_user(context_id, user_addr, &state, (uint64_t)sizeof(state)) != 0) {
            rc = -1;
        }
    }
    spinlock_unlock(&g_user_mutex_lock);

    if (out_state) {
        *out_state = state;
    }
    return rc;
}

int
user_mutex_user_try_lock(uint32_t context_id,
                         uint64_t user_addr,
                         uint32_t tid,
                         user_mutex_state_t *out_state)
{
    return user_mutex_access(context_id, user_addr, tid, user_mutex_state_try_lock, out_state);
}

int
user_mutex_user_unlock(uint32_t context_id,
                       uint64_t user_addr,
                       uint32_t tid,
                       user_mutex_state_t *out_state)
{
    return user_mutex_access(context_id, user_addr, tid, user_mutex_state_unlock, out_state);
}
