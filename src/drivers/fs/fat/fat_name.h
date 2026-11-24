/* fat_name.h - pure FAT name-handling: short/long name encoding, matching,
 * LFN accumulation, and directory-entry name (de)serialization.  These encode
 * the FAT on-disk name rules and carry no block I/O or global mutable state
 * (the LFN accumulator is passed explicitly as a fat_lfn_t). */
#ifndef FS_FAT_FAT_NAME_H
#define FS_FAT_FAT_NAME_H

#include <stdint.h>
#include "fat_types.h"

/* Case-insensitive equality of two NUL-terminated names (NULL-safe). */
int fat_name_eq(const char* a, const char* b);

/* Reset an LFN accumulator to empty before a directory scan / after a match. */
void fat_lfn_reset(fat_lfn_t* lfn);

/* NUL-terminate the reassembled LFN name.  Call it only once every ordinal entry
 * has been collected — it terminates at total * 13 characters and cannot tell a
 * short name from a partially gathered one, so the caller checks seen == total. */
void fat_lfn_finalize(fat_lfn_t* lfn);

/* Accumulate one 32-byte FAT LFN directory entry into the accumulator. */
void fat_lfn_collect(fat_lfn_t* lfn, const uint8_t* ent);

/* Extract a display name from a 32-byte short entry, preferring the completed
 * LFN in `lfn` when valid (finalizing it in place, hence the non-const `lfn`);
 * writes a NUL-terminated, possibly truncated name into out[out_len].  0 on
 * success, -1 on bad arguments. */
int fat_entry_name_from_dirent(fat_lfn_t* lfn, const uint8_t* ent, char* out, uint32_t out_len);

/* Validate a candidate long file name (ASCII, no reserved chars, <= FAT_LFN_MAX)
 * and report its length; returns 0 on success. */
/* Convert a UTF-8 name to UTF-16 code units, refusing malformed input and the
 * characters FAT forbids.  Pass out=NULL to validate and count only.  Returns 0,
 * or -1; *out_len is the number of UTF-16 units, which is what sizes the LFN
 * chain (13 units per entry). */
int fat_utf8_to_utf16(const char* name, uint16_t* out, uint32_t out_cap, uint32_t* out_len);

int fat_validate_lfn_name(const char* name, uint32_t* out_len);

/* Encode `name` as a strict 8.3 short name into out[11]; returns 0 only when the
 * name fits 8.3 exactly (else -1, meaning an alias is required). */
int fat_encode_short_name(const char* name, uint8_t out[11]);

/* Compute the FAT short-name checksum used to bind LFN entries to their 8.3. */
uint8_t fat_short_name_checksum(const uint8_t short_name[11]);

/* Build a "BASE~N" 8.3 alias for a long name into out[11]: the base is folded to
 * upper case, stripped of characters 8.3 disallows and truncated to 6 so "~N"
 * fits.  ordinal must be 1..9 (single digit); 0 on success, -1 otherwise. */
int fat_build_short_alias(const char* name, uint32_t ordinal, uint8_t out[11]);

/* Construct one 32-byte FAT Long File Name directory entry for writing. */
void fat_fill_lfn_entry(uint8_t* entry, const uint16_t* units, uint32_t name_len, uint32_t ordinal,
                        uint32_t total, uint8_t checksum);

#endif /* FS_FAT_FAT_NAME_H */
