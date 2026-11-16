#ifndef WASMOS_KERNEL_KMSG_H
#define WASMOS_KERNEL_KMSG_H

#include <stdint.h>

/*
 * kmsg.h — the kernel log ring.
 *
 * Every kernel-log write, from the first line of kmain onward, is appended
 * here as plain text with '\n' line ends (no CR). Console output of a process
 * with a controlling slot is not kernel log and never reaches the ring.
 *
 * Each byte has a sequence number: the count of bytes appended before it. The
 * ring holds the newest KMSG_SIZE bytes, so the oldest retained byte is
 * `next - KMSG_SIZE` once the ring has wrapped and 0 before. Sequence numbers
 * are 64-bit and never wrap, which is what lets a reader tell an overwritten
 * range from a retained one: a reader that remembers where it stopped can
 * resume there, or learn exactly how many bytes it lost.
 *
 * The ring holds no lock. The kernel serializes appends and kmsg_read under
 * the serial lock (serial.c); kmsg_size and kmsg_copy are also called without
 * it and can then observe a concurrent append.
 */

#define KMSG_SIZE 65536u /* a power of two */

typedef struct {
    uint64_t next; /* sequence number of the next byte appended */
    uint8_t data[KMSG_SIZE];
} kmsg_t;

/* Appends the NUL-terminated string s. NULL is ignored. */
void kmsg_append(kmsg_t* k, const char* s);

/* Sequence number of the oldest retained byte. */
uint64_t kmsg_first(const kmsg_t* k);

/* Number of retained bytes, at most KMSG_SIZE. */
uint32_t kmsg_size(const kmsg_t* k);

/* Copies up to len retained bytes starting `offset` bytes after the oldest one
 * into dst. len is clamped to what is retained; an offset at or past the end,
 * or a NULL dst, copies nothing. */
void kmsg_copy(const kmsg_t* k, uint8_t* dst, uint32_t offset, uint32_t len);

/* Copies up to len bytes starting at sequence number *seq into dst and stores
 * in *seq the sequence number of the first byte copied.
 *
 * A *seq older than the oldest retained byte starts at the oldest one instead,
 * so `*seq` on return minus `*seq` on entry is the number of bytes the reader
 * lost to overwrite; 0 therefore means "from the oldest retained byte". A *seq
 * equal to the next sequence number copies nothing.
 *
 * Returns the number of bytes copied, or WASMOS_INVAL for a NULL seq or dst, or
 * a *seq past the next sequence number (one the log never reached). */
int32_t kmsg_read(const kmsg_t* k, uint64_t* seq, uint8_t* dst, uint32_t len);

#endif
