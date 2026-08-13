/* kallsyms.h - Address-to-symbol lookup for panic backtraces, backed by a table
 * generated from the linked image by scripts/gen_kernel_kallsyms.py and linked
 * in a second pass. A first-pass kernel links against an empty weak table, so
 * every lookup simply misses. */
#pragma once

#include <stdint.h>

/* Resolve addr to the last symbol starting at or below it, writing *name and
 * *sym_addr (either may be NULL). Returns 1 when a symbol was found and 0 when
 * the table is empty or addr precedes every symbol -- a found/not-found flag,
 * NOT a 0-means-success status. The match has no upper bound, so an address
 * past the end of the last function still resolves to that function. */
int kpanic_symbolize(uint64_t addr, const char** name, uint64_t* sym_addr);
