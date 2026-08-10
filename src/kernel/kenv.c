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

void kenv_reset(void) {
    for (int i = 0; i < KENV_MAX_ENTRIES; i++) {
        g_kenv[i].in_use = 0;
    }
}
