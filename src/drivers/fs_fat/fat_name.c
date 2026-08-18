/* fat_name.c - pure FAT name-handling implementation (see fat_name.h).
 * LFN reassembly state is an explicit fat_lfn_t owned by the caller, never a
 * file-scope global, so each scan context accumulates into its own buffer and
 * nothing leaks between scans or between ops. */
#include "fat_name.h"

#include <stdint.h>
#include "ctype.h"
#include "string.h"

#include "fat_util.h"

int fat_name_eq(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    return strcasecmp(a, b) == 0;
}

void fat_lfn_reset(fat_lfn_t* lfn) {
    lfn->unit_count = 0;
    lfn->total = 0;
    lfn->seen = 0;
    lfn->valid = 0;
    for (uint32_t i = 0; i < sizeof(lfn->buf); ++i) {
        lfn->buf[i] = '\0';
    }
}

/* Store one UTF-16LE character from an LFN entry into the ASCII accumulation buffer.
 * 0x0000 = end-of-name sentinel; 0xFFFF = unused padding slot.  A character with
 * a non-zero high byte is outside ASCII and becomes '?': name handling here is
 * ASCII-only, and fat_validate_lfn_name refuses to create such names. */
/* Encode one code point as UTF-8.  Returns the byte count, or 0 when it does
 * not fit in `cap`. */
static uint32_t fat_utf8_encode(uint32_t cp, char* out, uint32_t cap) {
    if (cp < 0x80u) {
        if (cap < 1u) {
            return 0;
        }
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        if (cap < 2u) {
            return 0;
        }
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cap < 3u) {
            return 0;
        }
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cap < 4u) {
        return 0;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* Decode one UTF-8 sequence.  Returns the bytes consumed and writes the code
 * point, or 0 for a malformed sequence -- including an overlong encoding or a
 * surrogate, both of which must be refused rather than re-encoded. */
static uint32_t fat_utf8_decode(const char* s, uint32_t* out_cp) {
    unsigned char c0 = (unsigned char)s[0];
    unsigned char c1;
    unsigned char c2;
    unsigned char c3;
    uint32_t cp;

    if (c0 < 0x80u) {
        *out_cp = c0;
        return 1;
    }
    if ((c0 & 0xE0u) == 0xC0u) {
        c1 = (unsigned char)s[1];
        if ((c1 & 0xC0u) != 0x80u) {
            return 0;
        }
        cp = ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(c1 & 0x3Fu);
        return cp < 0x80u ? 0u : (*out_cp = cp, 2u);
    }
    if ((c0 & 0xF0u) == 0xE0u) {
        c1 = (unsigned char)s[1];
        if ((c1 & 0xC0u) != 0x80u) {
            return 0;
        }
        c2 = (unsigned char)s[2];
        if ((c2 & 0xC0u) != 0x80u) {
            return 0;
        }
        cp =
            ((uint32_t)(c0 & 0x0Fu) << 12) | ((uint32_t)(c1 & 0x3Fu) << 6) | (uint32_t)(c2 & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return 0;
        }
        *out_cp = cp;
        return 3;
    }
    if ((c0 & 0xF8u) == 0xF0u) {
        c1 = (unsigned char)s[1];
        c2 = (unsigned char)s[2];
        c3 = (unsigned char)s[3];
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
            return 0;
        }
        cp = ((uint32_t)(c0 & 0x07u) << 18) | ((uint32_t)(c1 & 0x3Fu) << 12) |
             ((uint32_t)(c2 & 0x3Fu) << 6) | (uint32_t)(c3 & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) {
            return 0;
        }
        *out_cp = cp;
        return 4;
    }
    return 0; /* a continuation byte or an invalid lead */
}

static void fat_lfn_store_char(fat_lfn_t* lfn, uint32_t pos, uint16_t ch) {
    if (pos >= FAT_LFN_MAX) {
        return;
    }
    if (ch == 0x0000 || ch == 0xFFFF) {
        /* End-of-name sentinel and padding: everything from here is absent. */
        if (pos < lfn->unit_count) {
            lfn->unit_count = pos;
        }
        lfn->units[pos] = 0;
        return;
    }
    lfn->units[pos] = ch;
    if (pos + 1u > lfn->unit_count) {
        lfn->unit_count = pos + 1u;
    }
}

/* NUL-terminate the reassembled LFN name after all ordinal entries have been
 * collected.  Each LFN entry holds 13 UTF-16LE characters; lfn->total gives
 * the number of entries present.  If fat_lfn_store_char already wrote a NUL
 * (end-of-name sentinel), the loop exits early. */
void fat_lfn_finalize(fat_lfn_t* lfn) {
    uint32_t out = 0;
    uint32_t i = 0;

    if (!lfn->valid || lfn->total == 0) {
        return;
    }
    /* UTF-16 to UTF-8, pairing surrogates.  A name that does not fit the output
     * buffer invalidates the accumulator rather than truncating: the caller then
     * uses the 8.3 short name, which is a name that can actually be opened. */
    while (i < lfn->unit_count) {
        uint16_t unit = lfn->units[i];
        uint32_t cp = unit;
        uint32_t n;

        if (unit >= 0xD800u && unit <= 0xDBFFu) {
            if (i + 1u >= lfn->unit_count) {
                lfn->valid = 0; /* unpaired high surrogate */
                return;
            }
            {
                uint16_t low = lfn->units[i + 1u];
                if (low < 0xDC00u || low > 0xDFFFu) {
                    lfn->valid = 0;
                    return;
                }
                cp = 0x10000u + (((uint32_t)(unit - 0xD800u) << 10) | (uint32_t)(low - 0xDC00u));
                i++;
            }
        } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
            lfn->valid = 0; /* unpaired low surrogate */
            return;
        }
        n = fat_utf8_encode(cp, &lfn->buf[out], (uint32_t)sizeof(lfn->buf) - 1u - out);
        if (n == 0) {
            lfn->valid = 0; /* does not fit; fall back to the short name */
            return;
        }
        out += n;
        i++;
    }
    lfn->buf[out] = '\0';
}

/* Accumulate one 32-byte FAT Long File Name directory entry into lfn->buf.
 *
 * FAT LFN layout: ordinal byte[0] (1-based, 0x40 flag on the last/highest
 * entry in the sequence), attr=0x0F[11], checksum[13], cluster=0[26:27],
 * then 13 UTF-16LE characters spread across three non-contiguous byte ranges:
 *   [1..10]  characters 1-5   (name1, 5 chars)
 *   [14..25] characters 6-11  (name2, 6 chars)
 *   [28..31] characters 12-13 (name3, 2 chars)
 *
 * Entries appear on disk in reverse ordinal order (highest first), so the
 * first entry seen has bit 0x40 set and declares lfn->total.  Each entry's
 * characters are written at base = (ordinal-1)*13 so they land in forward
 * order in lfn->buf regardless of disk order. */
void fat_lfn_collect(fat_lfn_t* lfn, const uint8_t* ent) {
    uint8_t ord = ent[0];
    if (ord == 0xE5) {
        /* 0xE5 marks a deleted entry — discard any partial LFN state. */
        fat_lfn_reset(lfn);
        return;
    }
    ord &= 0x1Fu;
    if (ord == 0) {
        fat_lfn_reset(lfn);
        return;
    }
    if (ent[0] & 0x40) {
        /* 0x40 flag: this is the last (highest-ordinal) LFN entry — it's
         * also the first one encountered on disk.  Reset and start fresh. */
        fat_lfn_reset(lfn);
        lfn->valid = 1;
        lfn->total = ord;
    }
    if (!lfn->valid) {
        return;
    }
    if (ord > lfn->total) {
        fat_lfn_reset(lfn);
        return;
    }

    uint32_t base = (uint32_t)(ord - 1u) * 13u;
    /* name1: bytes 1-10, 5 UTF-16LE characters */
    for (uint32_t i = 0; i < 5; ++i) {
        uint16_t ch = (uint16_t)ent[1 + i * 2] | ((uint16_t)ent[2 + i * 2] << 8);
        fat_lfn_store_char(lfn, base + i, ch);
    }
    /* name2: bytes 14-25, 6 UTF-16LE characters */
    for (uint32_t i = 0; i < 6; ++i) {
        uint16_t ch = (uint16_t)ent[14 + i * 2] | ((uint16_t)ent[15 + i * 2] << 8);
        fat_lfn_store_char(lfn, base + 5 + i, ch);
    }
    /* name3: bytes 28-31, 2 UTF-16LE characters */
    for (uint32_t i = 0; i < 2; ++i) {
        uint16_t ch = (uint16_t)ent[28 + i * 2] | ((uint16_t)ent[29 + i * 2] << 8);
        fat_lfn_store_char(lfn, base + 11 + i, ch);
    }
    lfn->seen++;
}

int fat_entry_name_from_dirent(fat_lfn_t* lfn, const uint8_t* ent, char* out, uint32_t out_len) {
    uint32_t pos = 0;

    if (!ent || !out || out_len == 0) {
        return -1;
    }

    /* `unit_count` is the accumulated-name test now: buf is produced BY
     * fat_lfn_finalize, so testing buf[0] first would always fail.  Validity is
     * re-tested after it, because a name that does not fit invalidates there and
     * the entry falls back to its 8.3 short name. */
    if (lfn->valid && lfn->seen == lfn->total && lfn->unit_count > 0) {
        fat_lfn_finalize(lfn);
    }
    if (lfn->valid && lfn->seen == lfn->total && lfn->unit_count > 0 && lfn->buf[0]) {
        while (lfn->buf[pos] && pos + 1 < out_len) {
            out[pos] = lfn->buf[pos];
            pos++;
        }
        out[pos] = '\0';
        return 0;
    }

    for (uint32_t i = 0; i < 8 && pos + 1 < out_len; ++i) {
        if (ent[i] != ' ') {
            out[pos++] = (char)ent[i];
        }
    }
    if (ent[8] != ' ' && pos + 1 < out_len) {
        out[pos++] = '.';
        for (uint32_t i = 0; i < 3 && pos + 1 < out_len; ++i) {
            if (ent[8 + i] != ' ') {
                out[pos++] = (char)ent[8 + i];
            }
        }
    }
    out[pos] = '\0';
    return 0;
}

int fat_utf8_to_utf16(const char* name, uint16_t* out, uint32_t out_cap, uint32_t* out_len) {
    uint32_t pos = 0;
    uint32_t units = 0;

    if (!name || name[0] == '\0') {
        return -1;
    }
    while (name[pos]) {
        uint32_t cp = 0;
        uint32_t n = fat_utf8_decode(&name[pos], &cp);

        if (n == 0) {
            return -1; /* malformed UTF-8 */
        }
        /* The characters FAT forbids, plus the control range.  Everything above
         * ASCII is now allowed: it is a name, not a command. */
        if (cp < 0x20u || cp == '"' || cp == '*' || cp == '/' || cp == ':' || cp == '<' ||
            cp == '>' || cp == '?' || cp == '\\' || cp == '|') {
            return -1;
        }
        if (cp < 0x10000u) {
            if (units + 1u > out_cap) {
                return -1;
            }
            if (out) {
                out[units] = (uint16_t)cp;
            }
            units += 1u;
        } else {
            if (units + 2u > out_cap) {
                return -1;
            }
            if (out) {
                cp -= 0x10000u;
                out[units] = (uint16_t)(0xD800u + (cp >> 10));
                out[units + 1u] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
            }
            units += 2u;
        }
        pos += n;
    }
    if (out_len) {
        *out_len = units;
    }
    return 0;
}

int fat_validate_lfn_name(const char* name, uint32_t* out_len) {
    return fat_utf8_to_utf16(name, NULL, FAT_LFN_MAX, out_len);
}

int fat_encode_short_name(const char* name, uint8_t out[11]) {
    uint32_t base_len = 0;
    uint32_t ext_len = 0;
    uint32_t i = 0;
    uint32_t dot_pos = 0xFFFFFFFFu;

    if (!name || !out || name[0] == '\0') {
        return -1;
    }

    while (name[i]) {
        char c = name[i];
        if (c == '.') {
            if (dot_pos != 0xFFFFFFFFu || i == 0 || name[i + 1] == '\0') {
                return -1;
            }
            dot_pos = i;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '_' || c == '-')) {
            return -1;
        }
        i++;
    }

    if (dot_pos == 0xFFFFFFFFu) {
        base_len = i;
    } else {
        base_len = dot_pos;
        ext_len = i - dot_pos - 1u;
    }
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return -1;
    }

    for (i = 0; i < 11; ++i) {
        out[i] = ' ';
    }
    for (i = 0; i < base_len; ++i) {
        out[i] = (uint8_t)fat_to_upper(name[i]);
    }
    for (i = 0; i < ext_len; ++i) {
        out[8 + i] = (uint8_t)fat_to_upper(name[dot_pos + 1u + i]);
    }

    return 0;
}

uint8_t fat_short_name_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;

    for (uint32_t i = 0; i < 11; ++i) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

int fat_build_short_alias(const char* name, uint32_t ordinal, uint8_t out[11]) {
    char base[9];
    char ext[4];
    uint32_t base_len = 0;
    uint32_t ext_len = 0;
    uint32_t len = 0;
    uint32_t dot = 0xFFFFFFFFu;

    if (!name || !out || ordinal == 0 || ordinal > 9) {
        return -1;
    }
    while (name[len]) {
        if (name[len] == '.') {
            dot = len;
        }
        len++;
    }

    for (uint32_t i = 0; i < len; ++i) {
        char c = name[i];
        if (i == dot) {
            continue;
        }
        if (dot != 0xFFFFFFFFu && i > dot) {
            if (c == ' ' || c == '.') {
                continue;
            }
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-')) {
                c = '_';
            }
            if (ext_len < 3) {
                ext[ext_len++] = fat_to_upper(c);
            }
            continue;
        }
        if (c == ' ' || c == '.') {
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            c = '_';
        }
        if (base_len < 6) {
            base[base_len++] = fat_to_upper(c);
        }
    }
    if (base_len == 0) {
        base[base_len++] = 'F';
    }
    for (uint32_t i = 0; i < 11; ++i) {
        out[i] = ' ';
    }
    for (uint32_t i = 0; i < base_len; ++i) {
        out[i] = (uint8_t)base[i];
    }
    out[base_len++] = '~';
    out[base_len++] = (uint8_t)('0' + ordinal);
    for (uint32_t i = 0; i < ext_len; ++i) {
        out[8 + i] = (uint8_t)ext[i];
    }
    return 0;
}

/* Construct one 32-byte FAT Long File Name directory entry for writing.
 *
 * LFN entries store 13 UTF-16LE characters at three non-contiguous byte
 * offsets within the 32-byte slot.  positions[] maps character index 0..12
 * to the low byte of each UTF-16LE word in the entry:
 *   chars 0-4  → bytes [1,3,5,7,9]      (name1, 5 × 2 bytes)
 *   chars 5-10 → bytes [14,16,18,20,22,24] (name2, 6 × 2 bytes)
 *   chars 11-12 → bytes [28,30]          (name3, 2 × 2 bytes)
 *
 * Fixed fields per FAT spec:
 *   entry[0]  = ordinal (1-based); |= 0x40 on the last (highest) entry
 *   entry[11] = 0x0F (ATTR_LONG_NAME; marks as LFN, not a real 8.3 entry)
 *   entry[12] = 0x00 (type, must be 0)
 *   entry[13] = checksum of the matching 8.3 short-name entry
 *   entry[26:27] = 0x0000 (cluster field; must be 0 for LFN entries)
 *
 * Characters after the name end are filled with 0x0000 (first unused slot)
 * and 0xFFFF (all remaining padding slots). */
void fat_fill_lfn_entry(uint8_t* entry, const uint16_t* units, uint32_t name_len, uint32_t ordinal,
                        uint32_t total, uint8_t checksum) {
    /* byte offsets of the low byte of each UTF-16LE character in the entry */
    static const uint8_t positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint32_t base = (ordinal - 1u) * 13u;
    uint32_t ended = 0;

    for (uint32_t i = 0; i < 32; ++i) {
        entry[i] = 0;
    }
    entry[0] = (uint8_t)ordinal;
    if (ordinal == total) {
        entry[0] |= 0x40u; /* last/highest ordinal flag */
    }
    entry[11] = 0x0Fu; /* ATTR_LONG_NAME */
    entry[12] = 0x00u;
    entry[13] = checksum;
    entry[26] = 0x00u; /* cluster low (must be 0) */
    entry[27] = 0x00u; /* cluster high (must be 0) */

    for (uint32_t i = 0; i < 13; ++i) {
        uint16_t ch = 0xFFFFu; /* padding for unused slots beyond NUL */
        uint32_t pos = base + i;

        if (!ended) {
            if (pos < name_len) {
                ch = units[pos];
            } else {
                ch = 0x0000u; /* end-of-name sentinel */
                ended = 1;
            }
        }
        entry[positions[i]] = (uint8_t)(ch & 0xFFu);
        entry[positions[i] + 1u] = (uint8_t)((ch >> 8) & 0xFFu);
    }
}
