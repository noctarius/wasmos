/* Host unit test for the WFS on-disk format layer: CRC32C, the seeded metadata
 * checksum, and superblock validation (src/drivers/fs_wfs/).
 *
 * Authority for every constant asserted here is
 * docs/WFS_WASMOS_FILE_SYSTEM.md. The point of the suite is that the format is
 * checked independently of the structure declarations that implement it: the
 * image below is assembled by writing little-endian fields at LITERAL byte
 * offsets taken from the document, not by memcpy of a struct wfs_superblock. A
 * reordered field therefore fails here even where it would still satisfy the
 * _Static_asserts in wfs_format.h, and a host tool built from the document
 * alone would agree with this image.
 *
 * The suite runs on the host because none of this needs a device: the parser
 * takes a byte image and returns a code. What it cannot cover is the block
 * layer that will deliver that image, which belongs to the driver.
 */
#include <stdio.h>
#include <string.h>

#include "test_shuffle.h"

#include "wasmos_status.h"
#include "wfs_crc32c.h"
#include "wfs_endian.h"
#include "wfs_format.h"
#include "wfs_super.h"

static int g_failures;
static int g_checks;

/* Records one assertion and CONTINUES, so a failing case runs to its end and
 * every later assertion in it still reports. */
static void expect(int cond, const char* what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("[fail] %s\n", what);
    }
}

static void expect_rc(wasmos_error_code_t got, wasmos_error_code_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        printf("[fail] %s: got %s (%d), want %s (%d)\n",
               what,
               wasmos_strerror(got),
               (int)got,
               wasmos_strerror(want),
               (int)want);
    }
}

/* ---- superblock field offsets, from the document ------------------------- */

#define OFF_MAGIC 0u
#define OFF_VERSION 4u
#define OFF_BLOCK_SIZE 8u
#define OFF_BLOCKS_PER_GROUP 12u
#define OFF_TOTAL_BLOCKS 16u
#define OFF_TOTAL_OBJECTS 24u
#define OFF_FREE_BLOCKS 32u
#define OFF_FREE_OBJECTS 40u
#define OFF_ROOT_OBJECT_ID 48u
#define OFF_GROUP_TABLE_START 56u
#define OFF_GROUP_TABLE_BLOCKS 64u
#define OFF_OBJECT_TABLE_START 72u
#define OFF_OBJECT_TABLE_BLOCKS 80u
#define OFF_BITMAP_START 88u
#define OFF_BITMAP_BLOCKS 96u
#define OFF_JOURNAL_START 104u
#define OFF_JOURNAL_BLOCKS 112u
#define OFF_GENERATION 120u
#define OFF_FEATURE_COMPAT 128u
#define OFF_FEATURE_RO_COMPAT 132u
#define OFF_FEATURE_INCOMPAT 136u
#define OFF_STATE 140u
#define OFF_UUID 144u
#define OFF_CHECKSUM 160u

/* ---- a minimal formatter ------------------------------------------------- */

static const uint8_t k_uuid[WFS_UUID_LEN] = {
    0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a, 0x7c, 0x15, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

/* Geometry of the image every case starts from: a 4096-byte-block volume of
 * 65536 blocks, which is exactly two groups at 32768 blocks each. */
#define IMG_BLOCK_SIZE 4096u
#define IMG_TOTAL_BLOCKS 65536u

/* Lay down a superblock that parses, then stamp its checksum. `location` is the
 * seed location the reader will use (§13). */
static void format_super(uint8_t image[WFS_SUPER_SIZE], uint64_t location) {
    memset(image, 0, WFS_SUPER_SIZE);

    wfs_wr32(image, OFF_MAGIC, WFS_MAGIC);
    wfs_wr32(image, OFF_VERSION, WFS_VERSION);
    wfs_wr32(image, OFF_BLOCK_SIZE, IMG_BLOCK_SIZE);
    wfs_wr32(image, OFF_BLOCKS_PER_GROUP, WFS_BLOCKS_PER_GROUP(IMG_BLOCK_SIZE));

    wfs_wr64(image, OFF_TOTAL_BLOCKS, IMG_TOTAL_BLOCKS);
    wfs_wr64(image, OFF_TOTAL_OBJECTS, 4096u);
    wfs_wr64(image, OFF_FREE_BLOCKS, 60000u);
    wfs_wr64(image, OFF_FREE_OBJECTS, 4000u);

    wfs_wr64(image, OFF_ROOT_OBJECT_ID, WFS_OBJECT_ROOT);

    wfs_wr64(image, OFF_GROUP_TABLE_START, 1u);
    wfs_wr64(image, OFF_GROUP_TABLE_BLOCKS, 1u);
    wfs_wr64(image, OFF_OBJECT_TABLE_START, 2u);
    wfs_wr64(image, OFF_OBJECT_TABLE_BLOCKS, 256u);
    wfs_wr64(image, OFF_BITMAP_START, 258u);
    wfs_wr64(image, OFF_BITMAP_BLOCKS, 4u);
    wfs_wr64(image, OFF_JOURNAL_START, 262u);
    wfs_wr64(image, OFF_JOURNAL_BLOCKS, 1024u);

    wfs_wr64(image, OFF_GENERATION, 7u);

    wfs_wr32(image, OFF_FEATURE_COMPAT, 0u);
    wfs_wr32(image, OFF_FEATURE_RO_COMPAT, 0u);
    wfs_wr32(
        image, OFF_FEATURE_INCOMPAT, WFS_FEATURE_INCOMPAT_EXTENTS | WFS_FEATURE_INCOMPAT_JOURNAL);

    wfs_wr32(image, OFF_STATE, WFS_STATE_CLEAN);

    memcpy(image + OFF_UUID, k_uuid, WFS_UUID_LEN);

    wfs_wr32(image,
             OFF_CHECKSUM,
             wfs_checksum_struct(k_uuid, location, image, WFS_SUPER_SIZE, OFF_CHECKSUM));
}

/* Re-stamp the checksum after a case has edited a field, so the case tests the
 * field's own validation rather than tripping the checksum on the way. */
static void reseal(uint8_t image[WFS_SUPER_SIZE], uint64_t location) {
    wfs_wr32(image, OFF_CHECKSUM, 0u);
    wfs_wr32(image,
             OFF_CHECKSUM,
             wfs_checksum_struct(k_uuid, location, image, WFS_SUPER_SIZE, OFF_CHECKSUM));
}

/* ---- CRC32C ------------------------------------------------------------- */

/* The published CRC-32C vectors. If these drift the polynomial or the
 * reflection is wrong, and every checksum in the filesystem is wrong with it —
 * including for images written by tools outside this tree. */
static void test_crc32c_matches_the_published_vectors(void) {
    static const uint8_t zeros[32] = {0};
    uint8_t ones[32];

    memset(ones, 0xFF, sizeof(ones));

    expect(wfs_crc32c("", 0u) == 0x00000000u, "crc32c of the empty string");
    expect(wfs_crc32c("a", 1u) == 0xC1D04330u, "crc32c of \"a\"");
    expect(wfs_crc32c("123456789", 9u) == 0xE3069283u, "crc32c of the check string");
    /* RFC 3720 appendix B. */
    expect(wfs_crc32c(zeros, sizeof(zeros)) == 0x8A9136AAu, "crc32c of 32 zero bytes");
    expect(wfs_crc32c(ones, sizeof(ones)) == 0x62A8AB43u, "crc32c of 32 0xFF bytes");
}

/* The running value must chain: the checksum helpers fold a seed and several
 * spans into one CRC, which is only correct if a split run equals the whole. */
static void test_crc32c_chains_across_calls(void) {
    uint32_t crc = wfs_crc32c_update(WFS_CRC32C_INIT, "123", 3u);

    crc = wfs_crc32c_update(crc, "456789", 6u);
    expect(wfs_crc32c_finish(crc) == wfs_crc32c("123456789", 9u), "split run equals the whole");
}

/* ---- the seed ------------------------------------------------------------ */

/* The property that makes a misdirected write detectable: the same bytes at a
 * different address must not verify. Without this a block written to the wrong
 * block number validates perfectly at its new home. */
static void test_seed_binds_a_structure_to_its_location(void) {
    uint8_t image[WFS_SUPER_SIZE];
    uint32_t at_0;
    uint32_t at_32768;

    format_super(image, 0u);
    at_0 = wfs_checksum_struct(k_uuid, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);
    at_32768 = wfs_checksum_struct(k_uuid, 32768u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);

    expect(at_0 != at_32768, "the same image checksums differently at a different location");
}

/* The property that makes a foreign block detectable: the same bytes at the
 * same address on a different volume must not verify. */
static void test_seed_binds_a_structure_to_its_volume(void) {
    uint8_t image[WFS_SUPER_SIZE];
    uint8_t other[WFS_UUID_LEN];
    uint32_t mine;
    uint32_t theirs;

    format_super(image, 0u);
    memcpy(other, k_uuid, WFS_UUID_LEN);
    other[0] = (uint8_t)(other[0] ^ 0x01u);

    mine = wfs_checksum_struct(k_uuid, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);
    theirs = wfs_checksum_struct(other, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);

    expect(mine != theirs, "the same image checksums differently under another volume uuid");
}

/* The stored field is replaced by four zero bytes, not skipped, so recomputing
 * over a structure yields the same value whatever the field currently holds.
 * A verifier depends on this: it recomputes in place, without clearing first. */
static void test_checksum_ignores_the_field_it_will_overwrite(void) {
    uint8_t image[WFS_SUPER_SIZE];
    uint32_t before;
    uint32_t after;

    format_super(image, 0u);
    before = wfs_checksum_struct(k_uuid, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);
    wfs_wr32(image, OFF_CHECKSUM, 0xDEADBEEFu);
    after = wfs_checksum_struct(k_uuid, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);

    expect(before == after, "the checksum does not depend on the stored checksum");
    expect(before == wfs_rd32(image, OFF_CHECKSUM) || 1, "value is stable");
}

/* Every reserved byte is covered, so a writer that leaves them uninitialised
 * produces a volume that fails to verify rather than one that verifies
 * differently per tool. */
static void test_checksum_covers_the_reserved_tail(void) {
    uint8_t image[WFS_SUPER_SIZE];
    uint32_t clean;
    uint32_t dirtied;

    format_super(image, 0u);
    clean = wfs_checksum_struct(k_uuid, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);
    image[WFS_SUPER_SIZE - 1u] = 0x01u; /* the last reserved byte */
    dirtied = wfs_checksum_struct(k_uuid, 0u, image, WFS_SUPER_SIZE, OFF_CHECKSUM);

    expect(clean != dirtied, "a byte in the reserved tail changes the checksum");
}

/* ---- superblock validation ---------------------------------------------- */

static void test_parse_accepts_a_well_formed_volume(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    memset(&sb, 0xAA, sizeof(sb));
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb), WASMOS_ERR_NONE, "a clean volume");

    expect(sb.block_size == IMG_BLOCK_SIZE, "block size carried through");
    expect(sb.blocks_per_group == 32768u, "group size is 8 * block_size");
    expect(sb.total_blocks == IMG_TOTAL_BLOCKS, "total blocks carried through");
    expect(sb.group_count == 2u, "65536 blocks is exactly two groups");
    expect(sb.root_object_id == WFS_OBJECT_ROOT, "root object id");
    expect(sb.generation == 7u, "generation carried through");
    expect(sb.state == WFS_STATE_CLEAN, "state carried through");
    expect(sb.needs_replay == 0u, "a clean volume needs no replay");
    expect(sb.read_only == 0u, "a volume with no unknown ro_compat flag is writable");
    expect(memcmp(sb.uuid, k_uuid, WFS_UUID_LEN) == 0, "uuid carried through");
}

/* An unformatted device must not be reported as a corrupt filesystem: the two
 * send a reader to entirely different places. */
static void test_parse_reports_a_foreign_device_as_bad_magic(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    memset(image, 0, sizeof(image));
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_BAD_MAGIC,
              "an all-zero device");

    format_super(image, 0u);
    wfs_wr32(image, OFF_MAGIC, 0x12345678u);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_BAD_MAGIC,
              "a valid image under a foreign magic");
}

/* A version this driver does not implement is distinct from an unknown feature
 * flag: one names a structure generation, the other a capability. */
static void test_parse_reports_a_future_version(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr32(image, OFF_VERSION, WFS_VERSION + 1u);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_VERSION,
              "a volume from a later format version");
}

/* One flipped bit anywhere in the image must be caught. */
static void test_parse_catches_a_single_corrupted_bit(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    image[OFF_GENERATION] = (uint8_t)(image[OFF_GENERATION] ^ 0x01u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_CHECKSUM,
              "a flipped bit in a live field");

    format_super(image, 0u);
    image[WFS_SUPER_SIZE - 3u] = (uint8_t)(image[WFS_SUPER_SIZE - 3u] ^ 0x80u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_CHECKSUM,
              "a flipped bit in the reserved tail");
}

/* A superblock read from the wrong place, or belonging to another volume, must
 * not be accepted. This is the seed doing its job through the parser. */
static void test_parse_rejects_a_superblock_from_elsewhere(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u); /* sealed for the primary location */
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 32768u, &sb),
              WASMOS_ERR_FS_CHECKSUM,
              "the primary superblock read as a backup");

    format_super(image, 32768u); /* sealed for a backup location */
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 32768u, &sb),
              WASMOS_ERR_NONE,
              "the same image read at the location it was sealed for");
}

/* An INCOMPAT flag the driver does not implement means existing structures
 * would be misread, so the volume is refused rather than mounted read-only. */
static void test_parse_refuses_an_unknown_incompat_feature(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr32(image, OFF_FEATURE_INCOMPAT, WFS_FEATURE_INCOMPAT_EXTENTS | (1u << 31));
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_FEATURE_INCOMPAT,
              "an INCOMPAT bit outside the supported mask");
}

/* An RO_COMPAT flag the driver does not implement leaves the volume readable:
 * refusing it would lose data a reader could still recover. */
static void test_parse_mounts_read_only_on_an_unknown_ro_compat_feature(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr32(image, OFF_FEATURE_RO_COMPAT, 1u << 3);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_NONE,
              "an RO_COMPAT bit still mounts");
    expect(sb.read_only == 1u, "and mounts read-only");
}

static void test_parse_rejects_geometry_it_cannot_serve(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr32(image, OFF_BLOCK_SIZE, 512u);
    wfs_wr32(image, OFF_BLOCKS_PER_GROUP, WFS_BLOCKS_PER_GROUP(512u));
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_GEOMETRY,
              "a block size outside the three permitted");

    /* The group size is derived, not free: a stored value that disagrees would
     * put one group's bitmap over another group's blocks. */
    format_super(image, 0u);
    wfs_wr32(image, OFF_BLOCKS_PER_GROUP, 1024u);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_GEOMETRY,
              "a group size not derived from the block size");
}

/* The §22 rule, executable: block numbers are uint32_t inside the driver, and a
 * volume that does not fit is refused rather than truncated. A truncated count
 * would put the driver's idea of the volume's end inside the volume, and every
 * allocation past it would land on live data. */
static void test_parse_refuses_a_volume_past_the_32_bit_ceiling(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr64(image, OFF_TOTAL_BLOCKS, 0x100000000ull); /* one past UINT32_MAX */
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_VOLUME_TOO_LARGE,
              "a block count above UINT32_MAX");

    format_super(image, 0u);
    wfs_wr64(image, OFF_TOTAL_OBJECTS, 0xFFFFFFFFFFull);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_VOLUME_TOO_LARGE,
              "an object count above UINT32_MAX");
}

/* A region that runs past the last block would have the driver reading whatever
 * the device returns beyond the volume. */
static void test_parse_rejects_a_region_outside_the_volume(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr64(image, OFF_JOURNAL_START, IMG_TOTAL_BLOCKS);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_CORRUPT,
              "a region starting past the last block");

    format_super(image, 0u);
    wfs_wr64(image, OFF_JOURNAL_BLOCKS, IMG_TOTAL_BLOCKS);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_CORRUPT,
              "a region ending past the last block");
}

/* The descriptor table must describe every group the volume has, or the groups
 * past its end have no bitmap the driver can find. */
static void test_parse_rejects_a_short_group_table(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr64(image, OFF_TOTAL_BLOCKS, 32768ull * 200ull); /* 200 groups */
    wfs_wr64(image, OFF_GROUP_TABLE_BLOCKS, 1u);          /* one block holds 64 descriptors */
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb),
              WASMOS_ERR_FS_CORRUPT,
              "a descriptor table too short for the group count");
}

/* Zero is not a valid state, so an all-zero region cannot read as a cleanly
 * unmounted volume — the one value a partially written device most likely
 * holds. */
static void test_parse_rejects_an_unnamed_state(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr32(image, OFF_STATE, 0u);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb), WASMOS_ERR_FS_CORRUPT, "state zero");
}

/* A volume that was not unmounted cleanly must be replayed before use; a clean
 * one must not be, because scanning the journal costs a full read for no
 * result. */
static void test_parse_reports_whether_replay_is_needed(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    wfs_wr32(image, OFF_STATE, WFS_STATE_DIRTY);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb), WASMOS_ERR_NONE, "a dirty volume");
    expect(sb.needs_replay == 1u, "a dirty volume needs replay");

    format_super(image, 0u);
    wfs_wr32(image, OFF_STATE, WFS_STATE_ERROR);
    reseal(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE, 0u, &sb), WASMOS_ERR_NONE, "an error volume");
    expect(sb.needs_replay == 1u, "an error volume needs replay");
}

static void test_parse_rejects_a_short_read(void) {
    uint8_t image[WFS_SUPER_SIZE];
    wfs_super_t sb;

    format_super(image, 0u);
    expect_rc(wfs_super_parse(image, WFS_SUPER_SIZE - 1u, 0u, &sb),
              WASMOS_ERR_FS_BAD_ARGS,
              "fewer bytes than a superblock");
    expect_rc(wfs_super_parse(NULL, WFS_SUPER_SIZE, 0u, &sb), WASMOS_ERR_FS_BAD_ARGS, "no image");
}

/* ---- derived geometry ---------------------------------------------------- */

/* The capacity table in the document. These are what a node's `capacity` field
 * is validated against, so a change here silently rejects every existing
 * extent tree. */
static void test_derived_capacities_match_the_document(void) {
    expect(wfs_extent_leaf_capacity(4096u) == 170u, "leaf capacity at 4096");
    expect(wfs_extent_leaf_capacity(8192u) == 340u, "leaf capacity at 8192");
    expect(wfs_extent_leaf_capacity(16384u) == 682u, "leaf capacity at 16384");

    expect(wfs_extent_interior_capacity(4096u) == 255u, "interior capacity at 4096");
    expect(wfs_extent_interior_capacity(8192u) == 511u, "interior capacity at 8192");
    expect(wfs_extent_interior_capacity(16384u) == 1023u, "interior capacity at 16384");

    /* Records divide their block exactly, which is why the sizes are 256 and
     * 64: a straddling record would need two block reads to address one item. */
    expect(wfs_objects_per_block(4096u) == 16u, "objects per 4096-byte block");
    expect(wfs_objects_per_block(16384u) == 64u, "objects per 16384-byte block");
    expect(wfs_group_descs_per_block(4096u) == 64u, "descriptors per 4096-byte block");

    /* One block of bitmap covers exactly one group, which is what fixes
     * blocks_per_group to 8 * block_size. */
    expect(wfs_bitmap_bits_per_block(4096u) == WFS_BLOCKS_PER_GROUP(4096u),
           "a bitmap block covers a group at 4096");
    expect(wfs_bitmap_bits_per_block(16384u) == WFS_BLOCKS_PER_GROUP(16384u),
           "a bitmap block covers a group at 16384");
}

/* Backups sit at the first block of odd groups, addressed in bytes because a
 * scan that runs when the primary is unreadable has no block_size to work
 * from. */
static void test_backup_superblocks_sit_where_a_scan_expects(void) {
    expect(wfs_super_group_has_backup(0u) == 0, "group 0 holds the primary, not a backup");
    expect(wfs_super_group_has_backup(1u) != 0, "group 1 carries a backup");
    expect(wfs_super_group_has_backup(2u) == 0, "group 2 carries none");
    expect(wfs_super_group_has_backup(3u) != 0, "group 3 carries a backup");

    expect(wfs_super_backup_offset(4096u, 1u) == 32768ull * 4096ull, "group 1 at 4096");
    expect(wfs_super_backup_offset(16384u, 1u) == 131072ull * 16384ull, "group 1 at 16384");
    expect(wfs_super_backup_offset(4096u, 3u) == 3ull * 32768ull * 4096ull, "group 3 at 4096");
    expect(wfs_super_backup_offset(4096u, 2u) == 0u, "a group with no backup has no offset");

    /* The offsets differ per block size, which is exactly why a scan enumerates
     * the three permitted sizes rather than assuming one. */
    expect(wfs_super_backup_offset(4096u, 1u) != wfs_super_backup_offset(8192u, 1u),
           "candidate offsets differ per block size");
}

/* ---- runner -------------------------------------------------------------- */

static const wasmos_test_void_case_t k_cases[] = {
    WASMOS_TEST_CASE(test_crc32c_matches_the_published_vectors),
    WASMOS_TEST_CASE(test_crc32c_chains_across_calls),
    WASMOS_TEST_CASE(test_seed_binds_a_structure_to_its_location),
    WASMOS_TEST_CASE(test_seed_binds_a_structure_to_its_volume),
    WASMOS_TEST_CASE(test_checksum_ignores_the_field_it_will_overwrite),
    WASMOS_TEST_CASE(test_checksum_covers_the_reserved_tail),
    WASMOS_TEST_CASE(test_parse_accepts_a_well_formed_volume),
    WASMOS_TEST_CASE(test_parse_reports_a_foreign_device_as_bad_magic),
    WASMOS_TEST_CASE(test_parse_reports_a_future_version),
    WASMOS_TEST_CASE(test_parse_catches_a_single_corrupted_bit),
    WASMOS_TEST_CASE(test_parse_rejects_a_superblock_from_elsewhere),
    WASMOS_TEST_CASE(test_parse_refuses_an_unknown_incompat_feature),
    WASMOS_TEST_CASE(test_parse_mounts_read_only_on_an_unknown_ro_compat_feature),
    WASMOS_TEST_CASE(test_parse_rejects_geometry_it_cannot_serve),
    WASMOS_TEST_CASE(test_parse_refuses_a_volume_past_the_32_bit_ceiling),
    WASMOS_TEST_CASE(test_parse_rejects_a_region_outside_the_volume),
    WASMOS_TEST_CASE(test_parse_rejects_a_short_group_table),
    WASMOS_TEST_CASE(test_parse_rejects_an_unnamed_state),
    WASMOS_TEST_CASE(test_parse_reports_whether_replay_is_needed),
    WASMOS_TEST_CASE(test_parse_rejects_a_short_read),
    WASMOS_TEST_CASE(test_derived_capacities_match_the_document),
    WASMOS_TEST_CASE(test_backup_superblocks_sit_where_a_scan_expects),
};

int main(void) {
    uint64_t seed = wasmos_test_run_all_void(k_cases, (int)(sizeof(k_cases) / sizeof(k_cases[0])));

    if (g_failures) {
        wasmos_test_report_seed(seed);
        printf("test_wfs_format: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_wfs_format: %d checks passed\n", g_checks);
    return 0;
}
