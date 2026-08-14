#include "kenv.h"

#include "string.h"

typedef struct {
    uint8_t in_use;
    char key[KENV_KEY_MAX];
    char value[KENV_VAL_MAX];
} kenv_entry_t;

static kenv_entry_t g_kenv[KENV_MAX_ENTRIES];

static uint32_t kenv_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int kenv_find(const char* key) {
    for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
        if (g_kenv[i].in_use && strcmp(g_kenv[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

/* Copies a variable's value into the caller's buffer, always NUL-terminating.
 *
 * The VALUE is truncated to out_size - 1 bytes when it does not fit, and
 * *written reports how much was actually copied — so *written < the stored
 * length is the only signal of truncation, and WASMOS_OK is still returned.
 * The KEY is never shortened: an over-long key is refused outright, because a
 * shortened one would resolve to a different variable.
 *
 * Returns WASMOS_OK on a hit, WASMOS_INVAL for a NULL key or out or a zero
 * out_size, WASMOS_ERR_ENV_TOO_LONG for an empty or over-long key, and
 * WASMOS_ERR_ENV_NOT_FOUND when no such variable exists.  *written is zeroed
 * before any lookup, so it is defined on the failure paths that reach it. */
wasmos_error_code_t kenv_get(const char* key, char* out, uint32_t out_size, uint32_t* written) {
    if (!key || !out || out_size == 0) {
        return WASMOS_INVAL;
    }
    if (written) {
        *written = 0;
    }
    /* Refused, not shortened: a shortened key resolves to whichever variable
     * shares the surviving prefix, which is a different variable than the one
     * asked for -- and one the caller could not have created, since kenv_set
     * refuses the same length. */
    uint32_t key_len = kenv_strlen(key);
    if (key_len == 0 || key_len >= KENV_KEY_MAX) {
        return WASMOS_ERR_ENV_TOO_LONG;
    }

    int idx = kenv_find(key);
    if (idx < 0) {
        return WASMOS_ERR_ENV_NOT_FOUND;
    }
    uint32_t val_len = kenv_strlen(g_kenv[idx].value);
    if (val_len >= out_size) {
        val_len = out_size - 1u;
    }
    memcpy(out, g_kenv[idx].value, val_len);
    out[val_len] = '\0';
    if (written) {
        *written = val_len;
    }
    return WASMOS_OK;
}

/* Creates or overwrites a variable.  Both strings are copied into the fixed
 * table, so the caller's buffers are borrowed for the call only.
 *
 * Nothing is truncated: a key at or over KENV_KEY_MAX, a value at or over
 * KENV_VAL_MAX, and an empty key are all WASMOS_ERR_ENV_TOO_LONG.  A NULL
 * argument is WASMOS_INVAL, and a table with no free slot is
 * WASMOS_ERR_ENV_TABLE_FULL — the slot count is only consulted for a NEW key, so
 * overwriting an existing one always succeeds.  An empty value is allowed. */
wasmos_error_code_t kenv_set(const char* key, const char* value) {
    if (!key || !value) {
        return WASMOS_INVAL;
    }
    uint32_t key_len = kenv_strlen(key);
    uint32_t val_len = kenv_strlen(value);
    if (key_len == 0 || key_len >= KENV_KEY_MAX || val_len >= KENV_VAL_MAX) {
        return WASMOS_ERR_ENV_TOO_LONG;
    }

    int idx = kenv_find(key);
    if (idx < 0) {
        for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
            if (!g_kenv[i].in_use) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            return WASMOS_ERR_ENV_TABLE_FULL;
        }
        g_kenv[idx].in_use = 1;
        memcpy(g_kenv[idx].key, key, key_len + 1u);
    }
    memcpy(g_kenv[idx].value, value, val_len + 1u);
    return WASMOS_OK;
}

/* Removes a variable if it exists.  Returns WASMOS_OK whether or not anything
 * was removed, so it is idempotent and gives no way to test existence — use
 * kenv_get for that.  WASMOS_INVAL for a NULL key and WASMOS_ERR_ENV_TOO_LONG
 * for an empty or over-long one.
 *
 * The slot is only marked free; its key and value bytes stay in the table until
 * a later kenv_set reuses the slot. */
wasmos_error_code_t kenv_unset(const char* key) {
    if (!key) {
        return WASMOS_INVAL;
    }
    uint32_t key_len = kenv_strlen(key);
    if (key_len == 0 || key_len >= KENV_KEY_MAX) {
        return WASMOS_ERR_ENV_TOO_LONG;
    }
    int idx = kenv_find(key);
    if (idx >= 0) {
        g_kenv[idx].in_use = 0;
    }
    return WASMOS_OK;
}

uint32_t kenv_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
        if (g_kenv[i].in_use) {
            n++;
        }
    }
    return n;
}

/* Drops every variable by clearing the in-use flags; the stored bytes are left
 * behind, as in kenv_unset.  Intended for test setup — there is no lock, so it
 * must not race a concurrent kenv_set. */
void kenv_reset(void) {
    for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
        g_kenv[i].in_use = 0;
    }
}
