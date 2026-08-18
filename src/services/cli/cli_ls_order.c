/* cli_ls_order.c - see cli_ls_order.h. */
#include "cli_ls_order.h"

int cli_ls_name_cmp(const char* a, const char* b) {
    if (!a || !b) {
        return a == b ? 0 : (a ? 1 : -1);
    }
    for (;;) {
        char ca = *a;
        char cb = *b;

        /* A trailing '/' is the directory marker, not part of the name. */
        if (ca == '/' && a[1] == '\0') {
            ca = '\0';
        }
        if (cb == '/' && b[1] == '\0') {
            cb = '\0';
        }
        if (ca == '\0' || cb == '\0') {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            uint32_t va = 0;
            uint32_t vb = 0;

            /* Saturating, so a pathologically long digit run cannot overflow
             * the accumulator; past the bound the runs compare equal and the
             * scan continues with the characters after them. */
            while (*a >= '0' && *a <= '9') {
                va = va <= 100000000u ? va * 10u + (uint32_t)(*a - '0') : va;
                a++;
            }
            while (*b >= '0' && *b <= '9') {
                vb = vb <= 100000000u ? vb * 10u + (uint32_t)(*b - '0') : vb;
                b++;
            }
            if (va != vb) {
                return va < vb ? -1 : 1;
            }
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return (int)(unsigned char)ca - (int)(unsigned char)cb;
        }
        a++;
        b++;
    }
}

void cli_ls_sort(const char* pool, uint16_t* offsets, uint32_t count) {
    uint32_t i;
    uint32_t j;

    if (!pool || !offsets) {
        return;
    }
    for (i = 1; i < count; ++i) {
        uint16_t key = offsets[i];

        j = i;
        while (j > 0 && cli_ls_name_cmp(&pool[offsets[j - 1]], &pool[key]) > 0) {
            offsets[j] = offsets[j - 1];
            j--;
        }
        offsets[j] = key;
    }
}
