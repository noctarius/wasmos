/* fat_util.h - small shared helpers for the FAT backend (logging, char/string,
 * IPC name (un)packing).  No mutable state. */
#ifndef FS_FAT_FAT_UTIL_H
#define FS_FAT_FAT_UTIL_H

#include <stdint.h>

/* Write a "[fat] "-tagged line to the console (diagnostics only). */
void fat_log(const char* msg);

char fat_to_upper(char c);
int32_t fat_str_len(const char* s);

/* Unpack up to 16 name bytes packed little-endian across arg0..arg3 into out
 * (NUL-terminated, upper-cased), stopping at the first NUL. */
void fat_unpack_name(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, char* out,
                     uint32_t out_len);

/* Find "key" (e.g. "unit=") in a space-separated startup-arg string and return a
 * pointer to the value that follows, or NULL. */
const char* fat_find_token_value(const char* args, const char* key);

#endif /* FS_FAT_FAT_UTIL_H */
