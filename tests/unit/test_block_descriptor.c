/* test_block_descriptor.c — the block-device descriptor ABI
 * (wasmos_block_descriptor_t) and the class-instance fingerprint derived from
 * its canonical id, both in src/drivers/include/wasmos_driver_abi.h.
 *
 * Nothing is linked in: the header is self-contained, and the fingerprint is a
 * static inline function over a string. The subject is a LAYOUT and a VALUE
 * MAPPING, so the cases assert offsets, sizes and hash outputs rather than
 * behaviour of any driver.
 *
 * Why the offsets are pinned here as well as by the _Static_assert in the
 * header: that assertion only pins the total size, which several different field
 * orders satisfy. src/libc/zig/driver.zig mirrors this struct as a Zig `extern
 * struct` and pins the same offsets in a comptime block, so the two sides
 * describe the same bytes only if BOTH sets of numbers hold. A field inserted in
 * the middle of one and not the other keeps the size and moves everything after
 * it -- exactly the failure no field-by-field review catches.
 *
 * The fingerprint vectors are pinned on both sides for a sharper reason: the
 * fingerprint is the `block` class instance, i.e. the ADDRESS a filesystem
 * driver uses to find its disk. A C backend and a Zig backend that disagree by
 * one bit leave the filesystem waiting forever on an instance nobody registered,
 * with no error anywhere.
 *
 * The cases report through assert(), not a failure counter: the first failure
 * aborts the process, and main() prints "ok" only if every case ran to
 * completion. The suite is compiled without -DNDEBUG, which those asserts
 * depend on.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos_driver_abi.h"

/* Every offset src/libc/zig/driver.zig pins in its comptime block. Kept as one
 * list so the two are read side by side. */
static void test_descriptor_layout(void) {
    assert(sizeof(wasmos_block_descriptor_t) == 296u);
    assert(offsetof(wasmos_block_descriptor_t, version) == 0u);
    assert(offsetof(wasmos_block_descriptor_t, backend) == 4u);
    assert(offsetof(wasmos_block_descriptor_t, unit) == 8u);
    assert(offsetof(wasmos_block_descriptor_t, partition) == 12u);
    assert(offsetof(wasmos_block_descriptor_t, scheme) == 16u);
    assert(offsetof(wasmos_block_descriptor_t, fs_type) == 20u);
    assert(offsetof(wasmos_block_descriptor_t, sector_bytes) == 24u);
    assert(offsetof(wasmos_block_descriptor_t, flags) == 28u);
    assert(offsetof(wasmos_block_descriptor_t, lba_start) == 32u);
    assert(offsetof(wasmos_block_descriptor_t, lba_count) == 40u);
    assert(offsetof(wasmos_block_descriptor_t, type_guid) == 48u);
    assert(offsetof(wasmos_block_descriptor_t, part_guid) == 64u);
    assert(offsetof(wasmos_block_descriptor_t, mbr_type) == 80u);
    assert(offsetof(wasmos_block_descriptor_t, label) == 88u);
    assert(offsetof(wasmos_block_descriptor_t, canonical_id) == 232u);
}

/* The packed C layout and the naturally-aligned Zig mirror agree only because
 * every field sits at its natural alignment and the total is a multiple of 8.
 * Assert that property directly: it is the reason the two may differ in
 * `packed`-ness and still describe the same bytes, and it is what a new field
 * would break. */
static void test_layout_needs_no_padding(void) {
    assert(sizeof(wasmos_block_descriptor_t) % 8u == 0u);
    assert(offsetof(wasmos_block_descriptor_t, lba_start) % 8u == 0u);
    assert(offsetof(wasmos_block_descriptor_t, lba_count) % 8u == 0u);
}

/* Capacities come from abi/constants.yaml; the struct must use those and not a
 * literal that happens to match today. */
static void test_descriptor_capacities(void) {
    wasmos_block_descriptor_t desc;
    assert(sizeof(desc.label) == (size_t)BLOCK_DESCRIPTOR_LABEL_MAX);
    assert(sizeof(desc.canonical_id) == (size_t)BLOCK_DESCRIPTOR_ID_MAX);
    /* A GPT name is 36 UTF-16 code units. The worst case in UTF-8 is 3 bytes per
     * code unit, not 4: a code point that needs 4 bytes is outside the BMP and
     * costs TWO code units, so it buys 4 bytes for the price of two units rather
     * than 8. 36 * 3 + NUL is therefore the bound the field must clear. */
    assert(sizeof(desc.label) >= (36u * 3u) + 1u);
}

/* A descriptor survives the round trip through a byte buffer unchanged --
 * the way it actually travels, since both IDENTIFY and the inventory publish
 * carry it as raw bytes in a transfer buffer. */
static void test_descriptor_round_trip(void) {
    wasmos_block_descriptor_t out;
    wasmos_block_descriptor_t in;
    uint8_t wire[sizeof(wasmos_block_descriptor_t)];

    memset(&out, 0, sizeof(out));
    out.version = BLOCK_DESCRIPTOR_VERSION;
    out.backend = BLOCK_BACKEND_VIRTIO_BLK;
    out.unit = 8u;
    out.partition = 3u;
    out.scheme = PARTITION_SCHEME_GPT;
    out.fs_type = FS_TYPE_FAT;
    out.sector_bytes = 512u;
    out.flags = BLOCK_DESCRIPTOR_FLAG_PRESENT | BLOCK_DESCRIPTOR_FLAG_READ_ONLY;
    /* Past 2^32 sectors on purpose: the 64-bit fields are the reason the
     * descriptor exists rather than four IPC argument words. */
    out.lba_start = 2048u;
    out.lba_count = 0x1FFFFFFFFull;
    out.type_guid[0] = 0x28u;
    out.type_guid[15] = 0x3Bu;
    out.part_guid[0] = 0xA1u;
    out.mbr_type = 0x0Cu;
    memcpy(out.label, "user", 5);
    memcpy(out.canonical_id, "block:virtio-blk:8p3", 21);

    memcpy(wire, &out, sizeof(wire));
    memcpy(&in, wire, sizeof(in));

    assert(memcmp(&in, &out, sizeof(in)) == 0);
    assert(in.lba_count == 0x1FFFFFFFFull);
    assert(in.lba_start == 2048u);
    assert(strcmp(in.canonical_id, "block:virtio-blk:8p3") == 0);
    assert(strcmp(in.label, "user") == 0);
    assert(in.mbr_type == 0x0Cu);
}

/* FNV-1a 32, pinned to the same numbers src/libc/zig/driver.zig asserts at
 * comptime. These are not arbitrary: they are the class instances the shipped
 * backends register under for their real canonical ids. */
static void test_fingerprint_vectors(void) {
    assert(wasmos_block_fingerprint("block:ata:0") == 4118534846u);
    assert(wasmos_block_fingerprint("block:ata:1") == 4135312465u);
    assert(wasmos_block_fingerprint("block:virtio-blk:8") == 2695290355u);
}

/* 0 is reserved for "no id", which is how a caller distinguishes an unusable
 * descriptor from a device whose fingerprint merely happens to be small. It is
 * also what BLOCK_IPC_IDENTIFY_REQ treats as "the only device you serve". */
static void test_fingerprint_rejects_empty(void) {
    assert(wasmos_block_fingerprint("") == 0u);
    assert(wasmos_block_fingerprint(NULL) == 0u);
}

/* Distinct ids must not collide, and — the case that matters — a TRUNCATED id
 * must not fingerprint to the same value as the full one. A truncation that
 * hashed alike would silently address a different disk, which is why every
 * producer refuses to truncate rather than letting it through. */
static void test_fingerprint_distinguishes_ids(void) {
    const uint32_t ata0 = wasmos_block_fingerprint("block:ata:0");
    const uint32_t ata1 = wasmos_block_fingerprint("block:ata:1");
    const uint32_t part = wasmos_block_fingerprint("block:ata:0p1");
    const uint32_t truncated = wasmos_block_fingerprint("block:ata:");

    assert(ata0 != ata1);
    assert(ata0 != part);
    assert(ata1 != part);
    assert(truncated != ata0);
    assert(truncated != ata1);
}

/* The hash must depend on order, not only on the multiset of bytes: FNV-1a does,
 * a plain sum would not, and a plain sum would map `block:ata:01` and
 * `block:ata:10` to one instance. */
static void test_fingerprint_is_order_sensitive(void) {
    assert(wasmos_block_fingerprint("ab") != wasmos_block_fingerprint("ba"));
    assert(wasmos_block_fingerprint("block:ata:01") != wasmos_block_fingerprint("block:ata:10"));
}

int main(void) {
    /* Randomized order: a case that leaks state must not be able to make its
     * neighbour pass. Replay a failure with WASMOS_TEST_SEED. */
    static const wasmos_test_void_case_t cases[] = {
        WASMOS_TEST_CASE(test_descriptor_layout),
        WASMOS_TEST_CASE(test_layout_needs_no_padding),
        WASMOS_TEST_CASE(test_descriptor_capacities),
        WASMOS_TEST_CASE(test_descriptor_round_trip),
        WASMOS_TEST_CASE(test_fingerprint_vectors),
        WASMOS_TEST_CASE(test_fingerprint_rejects_empty),
        WASMOS_TEST_CASE(test_fingerprint_distinguishes_ids),
        WASMOS_TEST_CASE(test_fingerprint_is_order_sensitive),
    };
    (void)wasmos_test_run_all_void(cases, (int)(sizeof(cases) / sizeof(cases[0])));
    printf("test_block_descriptor: ok\n");
    return 0;
}
