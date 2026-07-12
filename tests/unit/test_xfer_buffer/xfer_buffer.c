#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wasmos_driver_abi.h"
#include "xfer_buffer.h"

typedef struct {
    uint32_t passed;
    uint32_t failed;
} test_stats_t;

static void
check(test_stats_t *stats, int condition, const char *name)
{
    if (condition) {
        stats->passed++;
        printf("[pass] %s\n", name);
    } else {
        stats->failed++;
        printf("[fail] %s\n", name);
    }
}

static void
test_acquire_validation(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t owner_two = {0};
    xfer_buffer_owner_t framebuffer_owner = {0};
    uint32_t transfer_size = xfer_buffer_size(BUFFER_KIND_TRANSFER);
    uint32_t framebuffer_size = xfer_buffer_size(BUFFER_KIND_FRAMEBUFFER);

    check(stats,
          xfer_buffer_acquire(99u, 10u, 1u, &owner) != 0,
          "acquire rejects invalid kind");
    check(stats,
          xfer_buffer_acquire(0u, 10u, 1u, &owner) != 0,
          "acquire rejects kind zero");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 0u, 1u, &owner) != 0,
          "acquire rejects zero owner context");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 10u, 0u, &owner) != 0,
          "acquire rejects zero minimum size");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 10u, 1u, 0) != 0,
          "acquire rejects null out_owner");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 10u, transfer_size + 1u, &owner) != 0,
          "acquire rejects request larger than transfer capacity");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 10u, UINT32_MAX, &owner) != 0,
          "acquire rejects absurd transfer minimum size");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 11u, transfer_size, &owner) == 0,
          "acquire accepts exact transfer capacity");
    check(stats,
          owner.buffer.kind == BUFFER_KIND_TRANSFER &&
          owner.owner_context_id == 11u &&
          owner.buffer.size_bytes >= transfer_size,
          "acquire returns owner binding with object descriptor");
    check(stats,
          owner.buffer.buffer_id != 0u,
          "acquire returns nonzero buffer id");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 11u, 1u, &owner_two) == 0,
          "acquire allows multiple transfer objects for one owner");
    check(stats,
          owner_two.buffer.buffer_id != owner.buffer.buffer_id,
          "acquire returns distinct object ids for separate objects");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER, 12u, 1u, &framebuffer_owner) == 0,
          "acquire allows framebuffer object");
    check(stats,
          framebuffer_owner.buffer.kind == BUFFER_KIND_FRAMEBUFFER &&
          framebuffer_owner.buffer.size_bytes == framebuffer_size,
          "framebuffer acquire returns intrinsic framebuffer capacity");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER,
                                       13u,
                                       framebuffer_size + 1u,
                                       &framebuffer_owner) != 0,
          "framebuffer acquire rejects request larger than intrinsic capacity");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "acquire validation cleanup releases first transfer object");
    check(stats,
          xfer_buffer_release_owned(&owner_two) == 0,
          "acquire validation cleanup releases second transfer object");
    check(stats,
          xfer_buffer_release_owned(&framebuffer_owner) == 0,
          "acquire validation cleanup releases framebuffer object");
}

static void
test_get_owned_and_release_owned_validation(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t fetched = {0};
    xfer_buffer_owner_t forged = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 20u, 128u, &owner) == 0,
          "setup owner object for get_owned validation");
    check(stats,
          xfer_buffer_get_owned(0, 20u, &fetched) != 0,
          "get_owned rejects null buffer");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 20u, 0) != 0,
          "get_owned rejects null out_owner");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 21u, &fetched) != 0,
          "get_owned rejects non-owner context");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 20u, &fetched) == 0,
          "get_owned returns binding for actual owner");
    check(stats,
          fetched.owner_context_id == 20u &&
          fetched.buffer.buffer_id == owner.buffer.buffer_id &&
          fetched.buffer.kind == owner.buffer.kind &&
          fetched.buffer.size_bytes == owner.buffer.size_bytes,
          "get_owned returns matching owner descriptor");
    fetched.buffer.kind = BUFFER_KIND_FRAMEBUFFER;
    check(stats,
          xfer_buffer_get_owned(&fetched.buffer, 20u, &fetched) != 0,
          "get_owned rejects kind-mismatched descriptor");

    memset(&forged, 0, sizeof(forged));
    forged.buffer = owner.buffer;
    forged.owner_context_id = 21u;
    check(stats,
          xfer_buffer_release_owned(0) != 0,
          "release_owned rejects null owner binding");
    check(stats,
          xfer_buffer_release_owned(&forged) != 0,
          "release_owned rejects forged non-owner binding");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "release_owned accepts real owner binding");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 20u, &fetched) != 0,
          "released object no longer resolves as owned");
    check(stats,
          xfer_buffer_release_owned(&owner) != 0,
          "release_owned rejects already-released object");
}

static void
test_transfer_ownership_lifecycle(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t new_owner = {0};
    xfer_buffer_owner_t third_owner = {0};
    xfer_buffer_owner_t old_owner_fetch = {0};
    xfer_buffer_owner_t framebuffer_owner = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 30u, 64u, &owner) == 0,
          "setup transfer-capable owned object");
    check(stats,
          xfer_buffer_transfer_ownership(0, 31u) != 0,
          "transfer_ownership rejects null owner");
    check(stats,
          xfer_buffer_transfer_ownership(&owner, 0u) != 0,
          "transfer_ownership rejects zero new owner");
    check(stats,
          xfer_buffer_transfer_ownership(&owner, 30u) != 0,
          "transfer_ownership rejects transfer to same owner");
    check(stats,
          xfer_buffer_transfer_ownership(&owner, 31u) == 0,
          "transfer_ownership succeeds for transfer buffer");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 30u, &old_owner_fetch) != 0,
          "old owner cannot retrieve ownership after transfer");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 31u, &new_owner) == 0,
          "new owner can retrieve ownership after transfer");
    check(stats,
          new_owner.owner_context_id == 31u &&
          new_owner.buffer.buffer_id == owner.buffer.buffer_id &&
          new_owner.buffer.kind == owner.buffer.kind &&
          new_owner.buffer.size_bytes == owner.buffer.size_bytes,
          "transfer preserves object identity while changing owner");
    check(stats,
          xfer_buffer_transfer_ownership(&new_owner, 34u) == 0,
          "new owner can transfer ownership onward");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 34u, &third_owner) == 0,
          "third owner can retrieve ownership after chained transfer");
    check(stats,
          xfer_buffer_release_owned(&new_owner) != 0,
          "stale transferred owner binding cannot release object");
    check(stats,
          xfer_buffer_release_owned(&third_owner) == 0,
          "current owner can release transferred object");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER, 32u, 64u, &framebuffer_owner) == 0,
          "setup framebuffer owned object");
    check(stats,
          xfer_buffer_transfer_ownership(&framebuffer_owner, 33u) != 0,
          "framebuffer ownership transfer is rejected");
    check(stats,
          xfer_buffer_release_owned(&framebuffer_owner) == 0,
          "framebuffer transfer test cleanup releases object");
}

static void
test_transfer_rejects_active_borrows(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t new_owner = {0};
    xfer_buffer_borrow_t borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 35u, 64u, &owner) == 0,
          "setup owned object for transfer-with-borrow rejection");
    check(stats,
          xfer_buffer_borrow(&owner, 36u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup active borrow before ownership transfer");
    check(stats,
          xfer_buffer_transfer_ownership(&owner, 37u) != 0,
          "transfer_ownership rejects transfer while borrows are active");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 35u, &new_owner) == 0,
          "original owner remains owner after rejected transfer");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "active borrow can be removed before ownership transfer");
    check(stats,
          xfer_buffer_transfer_ownership(&owner, 37u) == 0,
          "transfer_ownership succeeds once borrows are gone");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 37u, &new_owner) == 0,
          "new owner can retrieve ownership after delayed transfer");
    check(stats,
          xfer_buffer_release_owned(&new_owner) == 0,
          "delayed transfer test cleanup releases object");
}

static void
test_borrow_validation_and_owner_access(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};
    xfer_buffer_owner_t forged_owner = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 40u, 256u, &owner) == 0,
          "setup owner object for borrow validation");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 40u, BUFFER_BORROW_READ),
          "owner has implicit read access");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 40u, BUFFER_BORROW_WRITE),
          "owner has implicit write access");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 41u, BUFFER_BORROW_READ),
          "unrelated context has no access before borrow");
    check(stats,
          xfer_buffer_borrow(0, 41u, BUFFER_BORROW_READ, &borrow) != 0,
          "borrow rejects null owner binding");
    check(stats,
          xfer_buffer_borrow(&owner, 0u, BUFFER_BORROW_READ, &borrow) != 0,
          "borrow rejects zero borrower context");
    check(stats,
          xfer_buffer_borrow(&owner, 41u, 0u, &borrow) != 0,
          "borrow rejects zero flags");
    check(stats,
          xfer_buffer_borrow(&owner, 41u, 0x4u, &borrow) != 0,
          "borrow rejects invalid flags");
    check(stats,
          xfer_buffer_borrow(&owner, 41u, BUFFER_BORROW_READ, 0) != 0,
          "borrow rejects null out_borrow");
    check(stats,
          xfer_buffer_borrow(&owner, 41u, BUFFER_BORROW_READ, &borrow) == 0,
          "borrow creates read-only borrow handle");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 41u, BUFFER_BORROW_READ),
          "borrower gains requested read access");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 41u, BUFFER_BORROW_WRITE),
          "read-only borrower does not gain write access");
    check(stats,
          xfer_buffer_same_object(&owner.buffer, 41u, 40u),
          "borrow preserves underlying object identity");

    forged_owner.buffer = owner.buffer;
    forged_owner.owner_context_id = 41u;
    check(stats,
          xfer_buffer_release_owned(&forged_owner) != 0,
          "borrower cannot release owned object");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "borrower can unborrow its borrow handle");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 41u, BUFFER_BORROW_READ),
          "borrower loses access after unborrow");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "owner can release object once no borrows remain");
}

static void
test_framebuffer_borrow_policy(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER, 50u, 128u, &owner) == 0,
          "setup framebuffer owned object");
    check(stats,
          xfer_buffer_borrow(&owner, 51u, BUFFER_BORROW_WRITE, &borrow) != 0,
          "framebuffer borrow rejects foreign borrower");
    check(stats,
          xfer_buffer_borrow(&owner, 50u, BUFFER_BORROW_WRITE, &borrow) == 0,
          "framebuffer borrow allows local borrower");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "framebuffer local borrow can be unborrowed");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "framebuffer owner can release local object");
}

static void
test_reborrow_rules(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t upstream = {0};
    xfer_buffer_borrow_t downstream = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 60u, 256u, &owner) == 0,
          "setup owner object for reborrow");
    check(stats,
          xfer_buffer_borrow(&owner, 61u, BUFFER_BORROW_READ, &upstream) == 0,
          "setup upstream read-only borrow");
    check(stats,
          xfer_buffer_reborrow(0, 62u, BUFFER_BORROW_READ, &downstream) != 0,
          "reborrow rejects null upstream borrow");
    check(stats,
          xfer_buffer_reborrow(&upstream, 62u, 0u, &downstream) != 0,
          "reborrow rejects zero flags");
    check(stats,
          xfer_buffer_reborrow(&upstream, 62u, BUFFER_BORROW_WRITE, &downstream) != 0,
          "reborrow rejects rights amplification");
    check(stats,
          xfer_buffer_reborrow(&upstream, 62u, BUFFER_BORROW_READ, &downstream) == 0,
          "reborrow creates downstream borrow within upstream rights");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 62u, BUFFER_BORROW_READ),
          "downstream borrower gains reborrowed access");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 62u, BUFFER_BORROW_WRITE),
          "downstream borrower does not gain amplified rights");
    check(stats,
          xfer_buffer_same_object(&owner.buffer, 62u, 60u),
          "reborrow preserves original object identity");
    check(stats,
          xfer_buffer_unborrow(&upstream) == 0,
          "upstream unborrow cascades and revokes downstream reborrow");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 61u, BUFFER_BORROW_READ),
          "upstream borrower loses access after cascade unborrow");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 62u, BUFFER_BORROW_READ),
          "downstream borrower loses access after cascade unborrow");
    check(stats,
          xfer_buffer_unborrow(&downstream) != 0,
          "downstream reborrow handle is stale after cascade unborrow");
    check(stats,
          xfer_buffer_unborrow(&upstream) != 0,
          "upstream handle is stale after cascade unborrow");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "owner can release object after reborrow chain ends");
}

static void
test_release_owned_with_active_borrow_rejected(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 70u, 128u, &owner) == 0,
          "setup owner object for active-borrow release rejection");
    check(stats,
          xfer_buffer_borrow(&owner, 71u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup active borrow for release rejection");
    check(stats,
          xfer_buffer_release_owned(&owner) != 0,
          "owner cannot release object while borrow is active");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "active borrow can be removed before release");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "owner can release object after borrow removal");
}

static void
test_owner_side_dma_contract(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_dma_mapping_t mapping = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 80u, 256u, &owner) == 0,
          "setup owner object for owner-side DMA");
    check(stats,
          xfer_buffer_dma_map_owned(0, 0u, 64u, WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_owned rejects null owner");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 0u, WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_owned rejects zero length");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, owner.buffer.size_bytes, 1u,
                                             WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_owned rejects out-of-range offset");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 64u, WASMOS_DMA_DIR_TO_DEVICE, &mapping) == 0,
          "owner-side DMA map succeeds for valid owned object");
    check(stats,
          mapping.active == 1u && mapping.attached_via_borrow == 0u,
          "owner-side DMA mapping records owner attachment");
    check(stats,
          xfer_buffer_release_owned(&owner) != 0,
          "release_owned rejects owner-side DMA while mapping is active");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 0u, 64u) == 0,
          "owner-side DMA sync succeeds for mapped range");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 0u, 65u) != 0,
          "owner-side DMA sync rejects out-of-range subrange");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "owner-side DMA unmap succeeds");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 0u, 64u) != 0,
          "owner-side DMA sync rejects inactive mapping");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "owner can release object after owner-side DMA unmap");
}

static void
test_borrow_dma_contract(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t read_borrow = {0};
    xfer_buffer_borrow_t write_borrow = {0};
    xfer_buffer_borrow_t rw_borrow = {0};
    xfer_buffer_dma_mapping_t mapping = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 90u, 512u, &owner) == 0,
          "setup owner object for borrow-side DMA");

    check(stats,
          xfer_buffer_borrow(&owner, 91u, BUFFER_BORROW_READ, &read_borrow) == 0,
          "setup read-only borrow for DMA");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) == 0,
          "borrow DMA allows TO_DEVICE on read access");
    check(stats,
          xfer_buffer_unborrow(&read_borrow) != 0,
          "borrow cannot unborrow while DMA mapping is active");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "borrow DMA unmap succeeds");
    check(stats,
          xfer_buffer_unborrow(&read_borrow) == 0,
          "borrow can unborrow after DMA unmap");

    check(stats,
          xfer_buffer_borrow(&owner, 92u, BUFFER_BORROW_READ, &read_borrow) == 0,
          "setup second read-only borrow for negative DMA direction");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_FROM_DEVICE, &mapping) != 0,
          "borrow DMA rejects FROM_DEVICE on read-only access");
    check(stats,
          xfer_buffer_unborrow(&read_borrow) == 0,
          "second read-only borrow can unborrow");

    check(stats,
          xfer_buffer_borrow(&owner, 93u, BUFFER_BORROW_WRITE, &write_borrow) == 0,
          "setup write-only borrow for DMA");
    check(stats,
          xfer_buffer_dma_map_borrow(&write_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "borrow DMA rejects TO_DEVICE on write-only access");
    check(stats,
          xfer_buffer_dma_map_borrow(&write_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_FROM_DEVICE, &mapping) == 0,
          "borrow DMA allows FROM_DEVICE on write-only access");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "write-only borrow DMA unmap succeeds");
    check(stats,
          xfer_buffer_unborrow(&write_borrow) == 0,
          "write-only borrow can unborrow");

    check(stats,
          xfer_buffer_borrow(&owner, 94u,
                                      BUFFER_BORROW_READ | BUFFER_BORROW_WRITE,
                                      &rw_borrow) == 0,
          "setup read-write borrow for bidirectional DMA");
    check(stats,
          xfer_buffer_dma_map_borrow(&rw_borrow, 32u, 64u,
                                              WASMOS_DMA_DIR_BIDIR, &mapping) == 0,
          "borrow DMA allows bidirectional mapping on read-write access");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 0u, 64u) == 0,
          "borrow DMA sync succeeds on active mapping");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 0u, 65u) != 0,
          "borrow DMA sync rejects out-of-range subrange");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "read-write borrow DMA unmap succeeds");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 94u, BUFFER_BORROW_READ),
          "borrow access remains after DMA unmap");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 94u, BUFFER_BORROW_WRITE),
          "borrow write access remains after DMA unmap");
    check(stats,
          xfer_buffer_unborrow(&rw_borrow) == 0,
          "read-write borrow can unborrow after DMA unmap");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "owner can release DMA-tested object after all borrows end");
}

static void
test_context_teardown_contract(test_stats_t *stats)
{
    xfer_buffer_owner_t owner_a = {0};
    xfer_buffer_owner_t owner_b = {0};
    xfer_buffer_borrow_t borrow_a = {0};
    xfer_buffer_borrow_t borrow_b = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 100u, 128u, &owner_a) == 0,
          "setup owner A for context teardown");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 101u, 128u, &owner_b) == 0,
          "setup owner B for context teardown");
    check(stats,
          xfer_buffer_borrow(&owner_a, 102u, BUFFER_BORROW_READ, &borrow_a) == 0,
          "setup borrow rooted in owner A");
    check(stats,
          xfer_buffer_borrow(&owner_b, 103u, BUFFER_BORROW_WRITE, &borrow_b) == 0,
          "setup borrow rooted in owner B");

    xfer_buffer_drop_context(100u);

    check(stats,
          !xfer_buffer_can_access(&owner_a.buffer, 102u, BUFFER_BORROW_READ),
          "context teardown revokes borrows rooted in torn-down owner");
    check(stats,
          xfer_buffer_can_access(&owner_b.buffer, 103u, BUFFER_BORROW_WRITE),
          "context teardown preserves unrelated borrows");
    check(stats,
          xfer_buffer_unborrow(&borrow_a) != 0,
          "revoked borrow handle cannot be unborrowed as active");
    check(stats,
          xfer_buffer_unborrow(&borrow_b) == 0,
          "unrelated borrow remains valid after foreign teardown");
    check(stats,
          xfer_buffer_release_owned(&owner_b) == 0,
          "unrelated owner can still release object after teardown");
}

static void
test_negative_handle_and_zero_context_cases(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t other_owner = {0};
    xfer_buffer_owner_t forged_owner = {0};
    xfer_buffer_owner_t fetched = {0};
    xfer_buffer_borrow_t borrow = {0};
    xfer_buffer_borrow_t reborrow = {0};
    xfer_buffer_borrow_t forged_borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 110u, 256u, &owner) == 0,
          "setup primary owner for negative-handle cases");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 111u, 256u, &other_owner) == 0,
          "setup secondary owner for negative-handle cases");

    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 0u, &fetched) != 0,
          "get_owned rejects context_id == 0");
    check(stats,
          xfer_buffer_get_owned(&other_owner.buffer, 110u, &fetched) != 0,
          "get_owned rejects request for non-owned buffer");

    forged_owner.buffer = other_owner.buffer;
    forged_owner.owner_context_id = owner.owner_context_id;
    check(stats,
          xfer_buffer_release_owned(&forged_owner) != 0,
          "release_owned rejects non-owned buffer");

    check(stats,
          xfer_buffer_transfer_ownership(&owner, 0u) != 0,
          "transfer_ownership rejects new_context_id == 0");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 0u, BUFFER_BORROW_READ),
          "can_access rejects accessor_context_id == 0");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 110u, 0u),
          "can_access rejects zero requested flags");
    check(stats,
          !xfer_buffer_can_access(0, 110u, BUFFER_BORROW_READ),
          "can_access rejects null buffer");
    check(stats,
          !xfer_buffer_same_object(0, 110u, 110u),
          "same_object rejects null buffer");
    check(stats,
          !xfer_buffer_same_object(&owner.buffer, 110u, 0u),
          "same_object rejects owner_context_id == 0");

    check(stats,
          xfer_buffer_borrow(&owner, 112u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup borrow for negative unborrow cases");
    check(stats,
          xfer_buffer_reborrow(&borrow, 113u, BUFFER_BORROW_READ, &reborrow) == 0,
          "setup reborrow for negative unborrow cases");

    memset(&forged_borrow, 0, sizeof(forged_borrow));
    forged_borrow = borrow;
    forged_borrow.borrow_id = 0u;
    check(stats,
          xfer_buffer_unborrow(&forged_borrow) != 0,
          "unborrow rejects non-borrowed handle with zero borrow id");

    forged_borrow = borrow;
    forged_borrow.borrow_id = 0xFFFFFFFFu;
    check(stats,
          xfer_buffer_unborrow(&forged_borrow) != 0,
          "unborrow rejects non-borrowed handle with forged borrow id");

    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "original borrower unborrow cascades through downstream reborrow");
    check(stats,
          xfer_buffer_unborrow(&reborrow) != 0,
          "reborrower cannot unborrow already-cascade-revoked handle");
    check(stats,
          xfer_buffer_unborrow(&borrow) != 0,
          "original borrower cannot unborrow already-unborrowed handle");

    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "primary owner can release after negative-handle checks");
    check(stats,
          xfer_buffer_release_owned(&other_owner) == 0,
          "secondary owner can release after negative-handle checks");
}

static void
test_stale_handle_and_identity_cases(test_stats_t *stats)
{
    xfer_buffer_owner_t first_owner = {0};
    xfer_buffer_owner_t second_owner = {0};
    xfer_buffer_owner_t stale_owner = {0};
    xfer_buffer_owner_t current_owner = {0};
    xfer_buffer_borrow_t stale_borrow = {0};
    xfer_buffer_borrow_t active_borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 120u, 64u, &first_owner) == 0,
          "setup first owned object for stale-handle cases");
    stale_owner = first_owner;
    check(stats,
          xfer_buffer_release_owned(&first_owner) == 0,
          "release first object for stale-handle cases");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 120u, 64u, &second_owner) == 0,
          "acquire second owned object after release");
    check(stats,
          second_owner.buffer.buffer_id != stale_owner.buffer.buffer_id,
          "released object id is not immediately reused");
    check(stats,
          xfer_buffer_get_owned(&stale_owner.buffer, 120u, &current_owner) != 0,
          "stale released descriptor no longer resolves");
    check(stats,
          xfer_buffer_release_owned(&stale_owner) != 0,
          "stale released owner binding cannot release again");
    check(stats,
          xfer_buffer_transfer_ownership(&second_owner, 121u) == 0,
          "transfer setup succeeds for stale old-owner checks");
    check(stats,
          xfer_buffer_borrow(&second_owner, 122u, BUFFER_BORROW_READ, &active_borrow) != 0,
          "stale old owner cannot borrow after ownership transfer");
    check(stats,
          xfer_buffer_get_owned(&second_owner.buffer, 121u, &current_owner) == 0,
          "current owner can retrieve binding after transfer");
    check(stats,
          xfer_buffer_borrow(&current_owner, 122u, BUFFER_BORROW_READ, &stale_borrow) == 0,
          "current owner can still borrow after transfer");
    check(stats,
          xfer_buffer_unborrow(&stale_borrow) == 0,
          "cleanup borrow after stale old-owner checks");
    check(stats,
          xfer_buffer_release_owned(&current_owner) == 0,
          "cleanup current owner after stale-handle checks");
}

static void
test_acquire_capacity_semantics(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t framebuffer_owner = {0};
    uint32_t transfer_size = xfer_buffer_size(BUFFER_KIND_TRANSFER);
    uint32_t framebuffer_size = xfer_buffer_size(BUFFER_KIND_FRAMEBUFFER);

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 200u, 1u, &owner) == 0,
          "acquire accepts a tiny minimum size");
    check(stats,
          owner.buffer.size_bytes == transfer_size,
          "small transfer request still yields full intrinsic capacity");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER, 201u, framebuffer_size,
                                       &framebuffer_owner) == 0,
          "framebuffer acquire accepts exact intrinsic capacity");
    check(stats,
          framebuffer_owner.buffer.size_bytes == framebuffer_size,
          "framebuffer exact-capacity acquire yields full intrinsic capacity");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "capacity-semantics cleanup releases transfer object");
    check(stats,
          xfer_buffer_release_owned(&framebuffer_owner) == 0,
          "capacity-semantics cleanup releases framebuffer object");
}

static void
test_get_release_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t fetched_a = {0};
    xfer_buffer_owner_t fetched_b = {0};
    xfer_buffer_owner_t forged_kind = {0};
    xfer_buffer_t nonexistent = {0};
    xfer_buffer_borrow_t borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 330u, 128u, &owner) == 0,
          "setup owner for extended get/release cases");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 330u, &fetched_a) == 0 &&
          xfer_buffer_get_owned(&owner.buffer, 330u, &fetched_b) == 0,
          "get_owned is repeatable and non-mutating");
    check(stats,
          fetched_a.buffer.buffer_id == fetched_b.buffer.buffer_id,
          "repeated get_owned returns the same object identity");

    nonexistent = owner.buffer;
    nonexistent.buffer_id = owner.buffer.buffer_id + 4242u;
    check(stats,
          xfer_buffer_get_owned(&nonexistent, 330u, &fetched_a) != 0,
          "get_owned rejects a nonexistent buffer id");

    check(stats,
          xfer_buffer_borrow(&owner, 331u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup borrower for get_owned-by-borrower case");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 331u, &fetched_a) != 0,
          "get_owned rejects a borrower that is not the owner");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "extended get/release cleanup unborrows helper borrow");

    forged_kind = owner;
    forged_kind.buffer.kind = BUFFER_KIND_FRAMEBUFFER;
    check(stats,
          xfer_buffer_release_owned(&forged_kind) != 0,
          "release_owned rejects a kind-mismatched owner binding");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "extended get/release cleanup releases owner object");
}

static void
test_borrow_rights_matrix(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t stale_owner = {0};
    xfer_buffer_borrow_t write_borrow = {0};
    xfer_buffer_borrow_t rw_borrow = {0};
    xfer_buffer_borrow_t borrow_a = {0};
    xfer_buffer_borrow_t borrow_b = {0};
    xfer_buffer_borrow_t second = {0};
    xfer_buffer_borrow_t reused = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 210u, 256u, &owner) == 0,
          "setup owner for borrow rights matrix");

    check(stats,
          xfer_buffer_borrow(&owner, 211u, BUFFER_BORROW_WRITE, &write_borrow) == 0,
          "write-only borrow succeeds");
    check(stats,
          write_borrow.lender_context_id == 210u &&
          write_borrow.borrower_context_id == 211u &&
          write_borrow.flags == BUFFER_BORROW_WRITE &&
          write_borrow.borrow_id != 0u,
          "borrow handle records lender, borrower, flags and nonzero id");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 211u, BUFFER_BORROW_WRITE),
          "write-only borrower gains write access");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 211u, BUFFER_BORROW_READ),
          "write-only borrower does not gain read access");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 210u, BUFFER_BORROW_READ) &&
          xfer_buffer_can_access(&owner.buffer, 210u, BUFFER_BORROW_WRITE),
          "owner keeps implicit read/write access while lent out");

    check(stats,
          xfer_buffer_borrow(&owner, 212u,
                                      BUFFER_BORROW_READ | BUFFER_BORROW_WRITE,
                                      &rw_borrow) == 0,
          "read-write borrow succeeds");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 212u,
                                          BUFFER_BORROW_READ | BUFFER_BORROW_WRITE),
          "read-write borrower gains combined access");

    check(stats,
          xfer_buffer_borrow(&owner, 210u, BUFFER_BORROW_READ, &second) != 0,
          "transfer owner cannot borrow its own object to itself");
    check(stats,
          xfer_buffer_borrow(&owner, 211u, BUFFER_BORROW_READ, &second) != 0,
          "second borrow by the same borrower context is rejected");
    check(stats,
          xfer_buffer_borrow(&owner, 213u, 0x8u, &second) != 0,
          "borrow rejects high invalid flag bit");
    check(stats,
          xfer_buffer_borrow(&owner, 213u, 0xFFFFFFFFu, &second) != 0,
          "borrow rejects all-bits flags");
    check(stats,
          xfer_buffer_borrow(&owner, 213u,
                                      BUFFER_BORROW_READ | BUFFER_BORROW_WRITE | 0x4u,
                                      &second) != 0,
          "borrow rejects valid flags mixed with an invalid bit");

    stale_owner = owner;
    stale_owner.buffer.buffer_id = owner.buffer.buffer_id + 9999u;
    check(stats,
          xfer_buffer_borrow(&stale_owner, 214u, BUFFER_BORROW_READ, &second) != 0,
          "borrow rejects an owner binding for a nonexistent object");

    check(stats,
          xfer_buffer_unborrow(&write_borrow) == 0,
          "borrow rights matrix cleanup unborrows write borrow");
    check(stats,
          xfer_buffer_unborrow(&rw_borrow) == 0,
          "borrow rights matrix cleanup unborrows read-write borrow");

    check(stats,
          xfer_buffer_borrow(&owner, 211u, BUFFER_BORROW_READ, &reused) == 0,
          "a borrower context can borrow again after releasing its prior borrow");
    check(stats,
          xfer_buffer_unborrow(&reused) == 0,
          "borrow rights matrix cleanup unborrows reused borrow");

    check(stats,
          xfer_buffer_borrow(&owner, 215u, BUFFER_BORROW_READ, &borrow_a) == 0,
          "owner fans out a first borrow to a distinct context");
    check(stats,
          xfer_buffer_borrow(&owner, 216u, BUFFER_BORROW_READ, &borrow_b) == 0,
          "owner fans out a second borrow to another distinct context");
    check(stats,
          borrow_a.borrow_id != borrow_b.borrow_id,
          "fan-out borrows get distinct borrow ids");
    check(stats,
          xfer_buffer_unborrow(&borrow_a) == 0,
          "one fan-out borrow can be released independently");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 216u, BUFFER_BORROW_READ),
          "the sibling fan-out borrow keeps access after the other is released");
    check(stats,
          xfer_buffer_unborrow(&borrow_b) == 0,
          "the remaining fan-out borrow can be released");

    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "borrow rights matrix cleanup releases owner object");
}

static void
test_simultaneous_multi_handle_borrowing_core(test_stats_t *stats)
{
    xfer_buffer_owner_t request_owner = {0};
    xfer_buffer_owner_t reply_owner = {0};
    xfer_buffer_owner_t third_owner = {0};
    xfer_buffer_borrow_t request_read = {0};
    xfer_buffer_borrow_t reply_write = {0};
    xfer_buffer_borrow_t third_read = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 500u, 256u, &request_owner) == 0,
          "setup request owner for simultaneous multi-handle borrowing");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 501u, 256u, &reply_owner) == 0,
          "setup reply owner for simultaneous multi-handle borrowing");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 502u, 256u, &third_owner) == 0,
          "setup third owner for simultaneous multi-handle borrowing");

    check(stats,
          xfer_buffer_borrow(&request_owner, 510u, BUFFER_BORROW_READ, &request_read) == 0,
          "borrower acquires first live read borrow from request owner");
    check(stats,
          xfer_buffer_borrow(&reply_owner, 510u, BUFFER_BORROW_WRITE, &reply_write) == 0,
          "same borrower can simultaneously acquire a write borrow from a second owner");
    check(stats,
          xfer_buffer_can_access(&request_owner.buffer, 510u, BUFFER_BORROW_READ),
          "borrower keeps read access to first borrowed object while second borrow is active");
    check(stats,
          !xfer_buffer_can_access(&request_owner.buffer, 510u, BUFFER_BORROW_WRITE),
          "first borrowed object does not gain write access from second borrow");
    check(stats,
          xfer_buffer_can_access(&reply_owner.buffer, 510u, BUFFER_BORROW_WRITE),
          "borrower gains write access to second borrowed object");
    check(stats,
          !xfer_buffer_can_access(&reply_owner.buffer, 510u, BUFFER_BORROW_READ),
          "second borrowed object does not gain read access when only write was granted");
    check(stats,
          xfer_buffer_same_object(&request_owner.buffer, 510u, 500u),
          "first simultaneous borrow preserves request object identity");
    check(stats,
          xfer_buffer_same_object(&reply_owner.buffer, 510u, 501u),
          "second simultaneous borrow preserves reply object identity");

    check(stats,
          xfer_buffer_borrow(&third_owner, 510u, BUFFER_BORROW_READ, &third_read) == 0,
          "same borrower can hold a third simultaneous borrow from another owner");
    check(stats,
          xfer_buffer_can_access(&third_owner.buffer, 510u, BUFFER_BORROW_READ),
          "third simultaneous borrow grants access to its own object");

    check(stats,
          xfer_buffer_unborrow(&request_read) == 0,
          "releasing one simultaneous borrow succeeds independently");
    check(stats,
          !xfer_buffer_can_access(&request_owner.buffer, 510u, BUFFER_BORROW_READ),
          "released first borrow loses access");
    check(stats,
          xfer_buffer_can_access(&reply_owner.buffer, 510u, BUFFER_BORROW_WRITE),
          "second simultaneous borrow stays active after releasing the first");
    check(stats,
          xfer_buffer_can_access(&third_owner.buffer, 510u, BUFFER_BORROW_READ),
          "third simultaneous borrow stays active after releasing the first");

    check(stats,
          xfer_buffer_unborrow(&reply_write) == 0,
          "releasing the second simultaneous borrow succeeds independently");
    check(stats,
          !xfer_buffer_can_access(&reply_owner.buffer, 510u, BUFFER_BORROW_WRITE),
          "released second borrow loses access");
    check(stats,
          xfer_buffer_can_access(&third_owner.buffer, 510u, BUFFER_BORROW_READ),
          "third simultaneous borrow stays active after releasing the second");

    check(stats,
          xfer_buffer_unborrow(&third_read) == 0,
          "releasing the last simultaneous borrow succeeds");
    check(stats,
          xfer_buffer_release_owned(&request_owner) == 0,
          "simultaneous multi-handle cleanup releases request owner object");
    check(stats,
          xfer_buffer_release_owned(&reply_owner) == 0,
          "simultaneous multi-handle cleanup releases reply owner object");
    check(stats,
          xfer_buffer_release_owned(&third_owner) == 0,
          "simultaneous multi-handle cleanup releases third owner object");
}

static void
test_simultaneous_multi_handle_borrowing_broker_pattern(test_stats_t *stats)
{
    xfer_buffer_owner_t caller_payload = {0};
    xfer_buffer_owner_t broker_plan = {0};
    xfer_buffer_owner_t aux_payload = {0};
    xfer_buffer_borrow_t caller_read = {0};
    xfer_buffer_borrow_t plan_write = {0};
    xfer_buffer_borrow_t aux_read = {0};
    xfer_buffer_borrow_t downstream = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 520u, 512u, &caller_payload) == 0,
          "setup caller payload object for broker pattern");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 521u, 512u, &broker_plan) == 0,
          "setup broker plan object for broker pattern");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 522u, 512u, &aux_payload) == 0,
          "setup auxiliary payload object for broker pattern");

    check(stats,
          xfer_buffer_borrow(&caller_payload, 530u, BUFFER_BORROW_READ, &caller_read) == 0,
          "broker receives a read borrow for the caller payload");
    check(stats,
          xfer_buffer_borrow(&broker_plan, 530u, BUFFER_BORROW_WRITE, &plan_write) == 0,
          "broker simultaneously receives a write borrow for the plan buffer");
    check(stats,
          xfer_buffer_can_access(&caller_payload.buffer, 530u, BUFFER_BORROW_READ),
          "broker can read the caller payload while holding the plan write borrow");
    check(stats,
          xfer_buffer_can_access(&broker_plan.buffer, 530u, BUFFER_BORROW_WRITE),
          "broker can write the plan buffer while holding the caller read borrow");

    check(stats,
          xfer_buffer_borrow(&aux_payload, 530u, BUFFER_BORROW_READ, &aux_read) == 0,
          "broker can hold an additional simultaneous payload borrow");
    check(stats,
          xfer_buffer_can_access(&aux_payload.buffer, 530u, BUFFER_BORROW_READ),
          "broker gains access to the additional simultaneous payload borrow");

    check(stats,
          xfer_buffer_reborrow(&caller_read, 531u, BUFFER_BORROW_READ, &downstream) == 0,
          "broker can reborrow the caller payload downstream while still holding other borrows");
    check(stats,
          xfer_buffer_can_access(&caller_payload.buffer, 531u, BUFFER_BORROW_READ),
          "downstream component gains access to the reborrowed caller payload");
    check(stats,
          xfer_buffer_can_access(&broker_plan.buffer, 530u, BUFFER_BORROW_WRITE),
          "broker retains simultaneous write access to the plan buffer after reborrowing caller payload");
    check(stats,
          xfer_buffer_can_access(&aux_payload.buffer, 530u, BUFFER_BORROW_READ),
          "broker retains the auxiliary simultaneous borrow after reborrowing caller payload");

    check(stats,
          xfer_buffer_unborrow(&plan_write) == 0,
          "broker can release the plan write borrow independently");
    check(stats,
          !xfer_buffer_can_access(&broker_plan.buffer, 530u, BUFFER_BORROW_WRITE),
          "broker loses plan-buffer access after releasing the plan write borrow");
    check(stats,
          xfer_buffer_can_access(&caller_payload.buffer, 530u, BUFFER_BORROW_READ),
          "caller payload borrow remains live after releasing the plan write borrow");
    check(stats,
          xfer_buffer_can_access(&aux_payload.buffer, 530u, BUFFER_BORROW_READ),
          "auxiliary borrow remains live after releasing the plan write borrow");
    check(stats,
          xfer_buffer_can_access(&caller_payload.buffer, 531u, BUFFER_BORROW_READ),
          "downstream reborrow remains live after releasing the unrelated plan write borrow");

    check(stats,
          xfer_buffer_unborrow(&caller_read) == 0,
          "releasing the broker's caller-payload borrow cascades only along that borrow tree");
    check(stats,
          !xfer_buffer_can_access(&caller_payload.buffer, 530u, BUFFER_BORROW_READ),
          "broker loses caller-payload access after releasing that borrow");
    check(stats,
          !xfer_buffer_can_access(&caller_payload.buffer, 531u, BUFFER_BORROW_READ),
          "downstream reborrow is revoked when the broker releases the caller-payload borrow");
    check(stats,
          xfer_buffer_can_access(&aux_payload.buffer, 530u, BUFFER_BORROW_READ),
          "unrelated auxiliary borrow remains live after caller-payload cascade");

    check(stats,
          xfer_buffer_unborrow(&aux_read) == 0,
          "broker can release the remaining simultaneous auxiliary borrow");
    check(stats,
          xfer_buffer_release_owned(&caller_payload) == 0,
          "broker-pattern cleanup releases caller payload object");
    check(stats,
          xfer_buffer_release_owned(&broker_plan) == 0,
          "broker-pattern cleanup releases plan object");
    check(stats,
          xfer_buffer_release_owned(&aux_payload) == 0,
          "broker-pattern cleanup releases auxiliary object");
}

static void
test_borrow_rejects_released_object(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t stale = {0};
    xfer_buffer_borrow_t borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 217u, 128u, &owner) == 0,
          "setup owner for released-object borrow rejection");
    stale = owner;
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "release object before borrowing from a stale binding");
    check(stats,
          xfer_buffer_borrow(&stale, 218u, BUFFER_BORROW_READ, &borrow) != 0,
          "borrow rejects a released (destroyed) object");
    check(stats,
          xfer_buffer_transfer_ownership(&stale, 219u) != 0,
          "transfer rejects a released (destroyed) object");
}

static void
test_can_access_matrix(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t stale = {0};
    xfer_buffer_borrow_t read_borrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 220u, 256u, &owner) == 0,
          "setup owner for can_access matrix");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 220u,
                                          BUFFER_BORROW_READ | BUFFER_BORROW_WRITE),
          "owner satisfies a combined read/write access check");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 220u, 0x4u),
          "can_access rejects an invalid requested flag bit");

    check(stats,
          xfer_buffer_borrow(&owner, 221u, BUFFER_BORROW_READ, &read_borrow) == 0,
          "setup read-only borrow for can_access matrix");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 221u,
                                           BUFFER_BORROW_READ | BUFFER_BORROW_WRITE),
          "read-only borrower fails a combined read/write access check");
    check(stats,
          xfer_buffer_unborrow(&read_borrow) == 0,
          "can_access matrix cleanup unborrows read borrow");

    stale = owner;
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "release object for released-buffer can_access check");
    check(stats,
          !xfer_buffer_can_access(&stale.buffer, 220u, BUFFER_BORROW_READ),
          "can_access denies access on a released object");
}

static void
test_same_object_matrix(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 240u, 256u, &owner) == 0,
          "setup owner for same_object matrix");
    check(stats,
          xfer_buffer_same_object(&owner.buffer, 240u, 240u),
          "same_object holds for the owner against itself");
    check(stats,
          !xfer_buffer_same_object(&owner.buffer, 240u, 241u),
          "same_object rejects a wrong owner context id");
    check(stats,
          !xfer_buffer_same_object(&owner.buffer, 242u, 240u),
          "same_object rejects an unrelated accessor");
    check(stats,
          !xfer_buffer_same_object(&owner.buffer, 0u, 240u),
          "same_object rejects accessor_context_id == 0");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "same_object matrix cleanup releases owner object");
}

static void
test_framebuffer_borrow_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};
    xfer_buffer_borrow_t second = {0};
    xfer_buffer_borrow_t reborrow = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER, 260u, 128u, &owner) == 0,
          "setup framebuffer owner for extended policy");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 260u, BUFFER_BORROW_WRITE),
          "framebuffer owner has implicit access");
    check(stats,
          xfer_buffer_borrow(&owner, 260u, 0u, &borrow) != 0,
          "framebuffer local borrow rejects zero flags");
    check(stats,
          xfer_buffer_borrow(&owner, 260u, 0x4u, &borrow) != 0,
          "framebuffer local borrow rejects invalid flags");
    check(stats,
          xfer_buffer_borrow(&owner, 260u, BUFFER_BORROW_READ, &borrow) == 0,
          "framebuffer local borrow allows read flags");
    check(stats,
          xfer_buffer_borrow(&owner, 260u, BUFFER_BORROW_WRITE, &second) != 0,
          "framebuffer rejects a second concurrent local borrow");
    check(stats,
          xfer_buffer_reborrow(&borrow, 261u, BUFFER_BORROW_READ, &reborrow) != 0,
          "framebuffer borrow cannot be reborrowed");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "framebuffer extended cleanup unborrows local borrow");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "framebuffer extended cleanup releases owner object");
}

static void
test_reborrow_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t rw_upstream = {0};
    xfer_buffer_borrow_t narrowed = {0};
    xfer_buffer_borrow_t level_a = {0};
    xfer_buffer_borrow_t level_b = {0};
    xfer_buffer_borrow_t level_c = {0};
    xfer_buffer_borrow_t fanout = {0};
    xfer_buffer_borrow_t bad = {0};
    xfer_buffer_borrow_t inactive_src = {0};
    xfer_buffer_borrow_t inactive_child = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 280u, 512u, &owner) == 0,
          "setup owner for extended reborrow cases");
    check(stats,
          xfer_buffer_borrow(&owner, 281u,
                                      BUFFER_BORROW_READ | BUFFER_BORROW_WRITE,
                                      &rw_upstream) == 0,
          "setup read-write upstream borrow for narrowing");
    check(stats,
          xfer_buffer_reborrow(&rw_upstream, 282u, 0x4u, &bad) != 0,
          "reborrow rejects invalid flag bits");
    check(stats,
          xfer_buffer_reborrow(&rw_upstream, 0u, BUFFER_BORROW_READ, &bad) != 0,
          "reborrow rejects zero borrower context");
    check(stats,
          xfer_buffer_reborrow(&rw_upstream, 282u, BUFFER_BORROW_READ, 0) != 0,
          "reborrow rejects null out_borrow");
    check(stats,
          xfer_buffer_reborrow(&rw_upstream, 282u, BUFFER_BORROW_READ, &narrowed) == 0,
          "reborrow narrows read-write upstream to read-only downstream");
    check(stats,
          narrowed.lender_context_id == 281u,
          "reborrow records the upstream borrower as the downstream lender");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 282u, BUFFER_BORROW_READ) &&
          !xfer_buffer_can_access(&owner.buffer, 282u, BUFFER_BORROW_WRITE),
          "narrowed downstream borrower holds only the narrowed right");
    check(stats,
          xfer_buffer_reborrow(&narrowed, 283u,
                                        BUFFER_BORROW_READ | BUFFER_BORROW_WRITE,
                                        &bad) != 0,
          "reborrow cannot amplify beyond a narrowed upstream borrow");
    check(stats,
          xfer_buffer_reborrow(&rw_upstream, 281u, BUFFER_BORROW_READ, &bad) != 0,
          "reborrow rejects a downstream context that already holds a borrow");

    check(stats,
          xfer_buffer_reborrow(&narrowed, 284u, BUFFER_BORROW_READ, &fanout) == 0,
          "one upstream reborrow can fan out to multiple downstream borrowers");
    check(stats,
          fanout.borrow_id != narrowed.borrow_id,
          "fan-out reborrow gets a distinct borrow id");

    check(stats,
          xfer_buffer_unborrow(&fanout) == 0,
          "reborrow extended cleanup unborrows fan-out downstream");
    check(stats,
          xfer_buffer_unborrow(&narrowed) == 0,
          "reborrow extended cleanup unborrows narrowed downstream");
    check(stats,
          xfer_buffer_unborrow(&rw_upstream) == 0,
          "reborrow extended cleanup unborrows upstream borrow");

    check(stats,
          xfer_buffer_borrow(&owner, 285u, BUFFER_BORROW_READ, &level_a) == 0,
          "setup multi-level chain level A");
    check(stats,
          xfer_buffer_reborrow(&level_a, 286u, BUFFER_BORROW_READ, &level_b) == 0,
          "setup multi-level chain level B");
    check(stats,
          xfer_buffer_reborrow(&level_b, 287u, BUFFER_BORROW_READ, &level_c) == 0,
          "setup multi-level chain level C (three levels deep)");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 287u, BUFFER_BORROW_READ),
          "deepest chain borrower gains reborrowed access");
    check(stats,
          xfer_buffer_same_object(&owner.buffer, 287u, 280u),
          "deep reborrow preserves original object identity");
    check(stats,
          xfer_buffer_unborrow(&level_a) == 0,
          "unborrowing the chain root cascades through all downstream levels");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 287u, BUFFER_BORROW_READ),
          "deepest chain borrower loses access after root cascade");

    check(stats,
          xfer_buffer_borrow(&owner, 288u, BUFFER_BORROW_READ, &inactive_src) == 0,
          "setup upstream for inactive/forged reborrow checks");
    inactive_child = inactive_src;
    inactive_child.borrow_id = inactive_src.borrow_id + 7777u;
    check(stats,
          xfer_buffer_reborrow(&inactive_child, 289u, BUFFER_BORROW_READ, &bad) != 0,
          "reborrow rejects a forged upstream borrow id");
    check(stats,
          xfer_buffer_unborrow(&inactive_src) == 0,
          "release upstream to make it inactive");
    check(stats,
          xfer_buffer_reborrow(&inactive_src, 289u, BUFFER_BORROW_READ, &bad) != 0,
          "reborrow rejects an inactive (already unborrowed) upstream");

    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "reborrow extended cleanup releases owner object");
}

static void
test_owner_dma_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t stale = {0};
    xfer_buffer_dma_mapping_t mapping = {0};
    xfer_buffer_dma_mapping_t second = {0};
    uint32_t size = 0;

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 350u, 4096u, &owner) == 0,
          "setup owner for extended owner-side DMA");
    size = owner.buffer.size_bytes;

    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 64u, WASMOS_DMA_DIR_TO_DEVICE, 0) != 0,
          "dma_map_owned rejects null out_mapping");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 64u, 0u, &mapping) != 0,
          "dma_map_owned rejects zero direction flags");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, size + 1u,
                                             WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_owned rejects length larger than the object");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, size - 64u, 128u,
                                             WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_owned rejects a range that overruns the object end");

    stale = owner;
    stale.buffer.buffer_id = owner.buffer.buffer_id + 5150u;
    check(stats,
          xfer_buffer_dma_map_owned(&stale, 0u, 64u,
                                             WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_owned rejects a nonexistent owner binding");

    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, size,
                                             WASMOS_DMA_DIR_FROM_DEVICE, &mapping) == 0,
          "owner-side DMA maps the full object range FROM_DEVICE");
    check(stats,
          mapping.offset == 0u && mapping.length == size &&
          mapping.direction_flags == WASMOS_DMA_DIR_FROM_DEVICE &&
          mapping.attached_via_borrow == 0u && mapping.device_addr != 0u,
          "owner-side full-range mapping records offset, length, direction and address");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 64u,
                                             WASMOS_DMA_DIR_TO_DEVICE, &second) != 0,
          "owner-side DMA rejects a second concurrent mapping");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "owner-side full-range DMA unmap succeeds");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, size - 64u, 64u,
                                             WASMOS_DMA_DIR_BIDIR, &mapping) == 0,
          "owner-side DMA remaps a boundary range bidirectionally after unmap");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "owner-side boundary DMA unmap succeeds");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "extended owner DMA cleanup releases owner object");
}

static void
test_borrow_dma_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t read_borrow = {0};
    xfer_buffer_borrow_t write_borrow = {0};
    xfer_buffer_borrow_t rw_borrow = {0};
    xfer_buffer_borrow_t forged = {0};
    xfer_buffer_dma_mapping_t mapping = {0};
    xfer_buffer_dma_mapping_t second = {0};
    uint32_t size = 0;

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 370u, 4096u, &owner) == 0,
          "setup owner for extended borrow-side DMA");
    size = owner.buffer.size_bytes;

    check(stats,
          xfer_buffer_borrow(&owner, 371u, BUFFER_BORROW_READ, &read_borrow) == 0,
          "setup read-only borrow for extended DMA checks");
    check(stats,
          xfer_buffer_dma_map_borrow(0, 0u, 64u, WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_borrow rejects null borrow handle");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_TO_DEVICE, 0) != 0,
          "dma_map_borrow rejects null out_mapping");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 64u, 0u, &mapping) != 0,
          "dma_map_borrow rejects zero direction flags");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 0u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_borrow rejects zero length");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, size + 1u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_borrow rejects length larger than the object");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, size - 64u, 128u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_borrow rejects a range that overruns the object end");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_BIDIR, &mapping) != 0,
          "borrow DMA rejects bidirectional mapping on read-only access");

    forged = read_borrow;
    forged.borrow_id = read_borrow.borrow_id + 6161u;
    check(stats,
          xfer_buffer_dma_map_borrow(&forged, 0u, 64u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) != 0,
          "dma_map_borrow rejects a forged borrow id");

    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, size,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) == 0,
          "borrow DMA maps the full object range on read access");
    check(stats,
          mapping.attached_via_borrow == 1u &&
          mapping.borrow_id == read_borrow.borrow_id &&
          mapping.length == size && mapping.device_addr != 0u,
          "borrow-side mapping records borrow attachment, id, length and address");
    check(stats,
          xfer_buffer_dma_map_borrow(&read_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &second) != 0,
          "borrow DMA rejects a second concurrent mapping on the same borrow");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "borrow DMA full-range unmap succeeds");
    check(stats,
          xfer_buffer_unborrow(&read_borrow) == 0,
          "extended borrow DMA cleanup unborrows read borrow");

    check(stats,
          xfer_buffer_borrow(&owner, 372u, BUFFER_BORROW_WRITE, &write_borrow) == 0,
          "setup write-only borrow for bidirectional rejection");
    check(stats,
          xfer_buffer_dma_map_borrow(&write_borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_BIDIR, &mapping) != 0,
          "borrow DMA rejects bidirectional mapping on write-only access");
    check(stats,
          xfer_buffer_unborrow(&write_borrow) == 0,
          "extended borrow DMA cleanup unborrows write borrow");

    check(stats,
          xfer_buffer_borrow(&owner, 373u,
                                      BUFFER_BORROW_READ | BUFFER_BORROW_WRITE,
                                      &rw_borrow) == 0,
          "setup read-write borrow for boundary DMA");
    check(stats,
          xfer_buffer_dma_map_borrow(&rw_borrow, size - 64u, 64u,
                                              WASMOS_DMA_DIR_BIDIR, &mapping) == 0,
          "borrow DMA maps a boundary range on read-write access");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "borrow DMA boundary unmap succeeds");
    check(stats,
          xfer_buffer_unborrow(&rw_borrow) == 0,
          "extended borrow DMA cleanup unborrows read-write borrow");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "extended borrow DMA cleanup releases owner object");
}

static void
test_dma_sync_unmap_negatives(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_dma_mapping_t mapping = {0};
    xfer_buffer_dma_mapping_t empty = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 400u, 4096u, &owner) == 0,
          "setup owner for DMA sync/unmap negatives");
    check(stats,
          xfer_buffer_dma_sync(0, 0u, 16u) != 0,
          "dma_sync rejects a null mapping");
    check(stats,
          xfer_buffer_dma_unmap(0) != 0,
          "dma_unmap rejects a null mapping");
    check(stats,
          xfer_buffer_dma_sync(&empty, 0u, 16u) != 0,
          "dma_sync rejects an inactive mapping");
    check(stats,
          xfer_buffer_dma_unmap(&empty) != 0,
          "dma_unmap rejects an inactive mapping");

    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 128u,
                                             WASMOS_DMA_DIR_BIDIR, &mapping) == 0,
          "setup owner DMA mapping for sync range checks");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 32u, 32u) == 0,
          "dma_sync accepts an interior sub-window of the mapped range");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 0u, 0u) != 0,
          "dma_sync rejects a zero-length sub-window");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 129u, 1u) != 0,
          "dma_sync rejects an offset past the mapped range");
    check(stats,
          xfer_buffer_dma_sync(&mapping, 64u, 128u) != 0,
          "dma_sync rejects a sub-window that overruns the mapped range");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "DMA sync/unmap negatives cleanup unmaps owner mapping");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) != 0,
          "dma_unmap rejects a double unmap of the same mapping");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "DMA sync/unmap negatives cleanup releases owner object");
}

static void
test_transfer_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t new_owner = {0};
    xfer_buffer_owner_t forged_kind = {0};
    xfer_buffer_owner_t nonexistent = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 310u, 128u, &owner) == 0,
          "setup owner for extended transfer cases");

    nonexistent = owner;
    nonexistent.buffer.buffer_id = owner.buffer.buffer_id + 3131u;
    check(stats,
          xfer_buffer_transfer_ownership(&nonexistent, 311u) != 0,
          "transfer rejects a nonexistent object binding");
    forged_kind = owner;
    forged_kind.buffer.kind = BUFFER_KIND_FRAMEBUFFER;
    check(stats,
          xfer_buffer_transfer_ownership(&forged_kind, 311u) != 0,
          "transfer rejects a kind-mismatched owner binding");

    check(stats,
          xfer_buffer_transfer_ownership(&owner, 311u) == 0,
          "transfer ownership to a new context succeeds");
    check(stats,
          xfer_buffer_can_access(&owner.buffer, 311u, BUFFER_BORROW_READ) &&
          xfer_buffer_can_access(&owner.buffer, 311u, BUFFER_BORROW_WRITE),
          "the new owner has implicit read/write access after transfer");
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 310u, BUFFER_BORROW_READ),
          "the previous owner loses access after transfer");
    check(stats,
          xfer_buffer_transfer_ownership(&owner, 312u) != 0,
          "the stale previous-owner binding cannot transfer the object again");

    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 311u, &new_owner) == 0,
          "extended transfer fetches the current owner for cleanup");
    check(stats,
          xfer_buffer_release_owned(&new_owner) == 0,
          "extended transfer cleanup releases the transferred object");
}

static void
test_context_teardown_extended(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t reacquired = {0};
    xfer_buffer_owner_t fetched = {0};
    xfer_buffer_borrow_t borrow = {0};
    xfer_buffer_dma_mapping_t mapping = {0};

    xfer_buffer_drop_context(0u);
    check(stats, 1, "drop_context(0) is a safe no-op");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 420u, 256u, &owner) == 0,
          "setup owner for teardown-of-borrower case");
    check(stats,
          xfer_buffer_borrow(&owner, 421u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup borrow held by the context to be torn down");
    xfer_buffer_drop_context(421u);
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 421u, BUFFER_BORROW_READ),
          "tearing down a borrower revokes the borrow it held");
    check(stats,
          xfer_buffer_unborrow(&borrow) != 0,
          "the torn-down borrower's handle is no longer active");
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 420u, &fetched) == 0,
          "the owner still owns its object after a borrower teardown");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "owner releases object after borrower teardown");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 422u, 256u, &owner) == 0,
          "setup owner for teardown-clears-DMA case");
    check(stats,
          xfer_buffer_borrow(&owner, 423u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup borrow for teardown-clears-DMA case");
    check(stats,
          xfer_buffer_dma_map_borrow(&borrow, 0u, 64u,
                                              WASMOS_DMA_DIR_TO_DEVICE, &mapping) == 0,
          "setup active borrow-side DMA before teardown");
    xfer_buffer_drop_context(423u);
    check(stats,
          !xfer_buffer_can_access(&owner.buffer, 423u, BUFFER_BORROW_READ),
          "teardown revokes the borrow whose DMA was active");
    check(stats,
          xfer_buffer_borrow(&owner, 423u, BUFFER_BORROW_READ, &borrow) == 0,
          "the buffer is reusable by a fresh borrow after DMA-holding teardown");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "teardown-clears-DMA cleanup unborrows the fresh borrow");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "teardown-clears-DMA cleanup releases owner object");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 424u, 256u, &owner) == 0,
          "setup owner for teardown-of-owner case");
    xfer_buffer_drop_context(424u);
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 424u, &fetched) != 0,
          "get_owned fails for an owner whose context was torn down");
    check(stats,
          xfer_buffer_release_owned(&owner) != 0,
          "release_owned fails for an owner whose context was torn down");
    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 424u, 256u, &reacquired) == 0,
          "a torn-down owner context can acquire a fresh object");
    check(stats,
          xfer_buffer_release_owned(&reacquired) == 0,
          "teardown-of-owner cleanup releases the re-acquired object");
}

static void
test_object_phys(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_owner_t other = {0};
    xfer_buffer_owner_t framebuffer = {0};
    xfer_buffer_owner_t current = {0};
    xfer_buffer_t stale = {0};
    xfer_buffer_t nul = {0};
    xfer_buffer_dma_mapping_t mapping = {0};
    uint64_t phys = 0;

    check(stats,
          xfer_buffer_object_phys(0) == 0u,
          "object_phys rejects a null buffer");
    check(stats,
          xfer_buffer_object_phys(&nul) == 0u,
          "object_phys returns 0 for a zero descriptor");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 600u, 4096u, &owner) == 0,
          "setup owned transfer object for object_phys");
    phys = xfer_buffer_object_phys(&owner.buffer);
    check(stats,
          phys != 0u,
          "object_phys returns nonzero backing for a live object");
    check(stats,
          (phys & 0xFFFu) == 0u,
          "transfer object phys base is page-aligned");

    stale = owner.buffer;
    stale.buffer_id = owner.buffer.buffer_id + 7000u;
    check(stats,
          xfer_buffer_object_phys(&stale) == 0u,
          "object_phys returns 0 for a nonexistent buffer id");
    stale = owner.buffer;
    stale.kind = BUFFER_KIND_FRAMEBUFFER;
    check(stats,
          xfer_buffer_object_phys(&stale) == 0u,
          "object_phys returns 0 for a kind-mismatched descriptor");

    check(stats,
          xfer_buffer_dma_map_owned(&owner, 0u, 64u, WASMOS_DMA_DIR_TO_DEVICE, &mapping) == 0,
          "map owner DMA at offset 0 to cross-check object_phys");
    check(stats,
          mapping.device_addr == phys,
          "DMA device_addr at offset 0 equals the object phys base");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "unmap offset-0 DMA cross-check");
    check(stats,
          xfer_buffer_dma_map_owned(&owner, 128u, 64u, WASMOS_DMA_DIR_TO_DEVICE, &mapping) == 0,
          "map owner DMA at a nonzero offset to cross-check object_phys");
    check(stats,
          mapping.device_addr == phys + 128u,
          "DMA device_addr equals the object phys base plus the offset");
    check(stats,
          (mapping.device_addr & 0xFFFu) != 0u,
          "DMA device_addr with a sub-page offset is intentionally not page-aligned");
    check(stats,
          xfer_buffer_dma_unmap(&mapping) == 0,
          "unmap offset DMA cross-check");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 601u, 4096u, &other) == 0,
          "setup a second transfer object for object_phys");
    check(stats,
          xfer_buffer_object_phys(&other.buffer) != 0u,
          "second transfer object has nonzero backing");
    check(stats,
          xfer_buffer_object_phys(&other.buffer) != phys,
          "distinct transfer objects have distinct backing");

    check(stats,
          xfer_buffer_transfer_ownership(&owner, 602u) == 0,
          "transfer ownership to check phys stability");
    check(stats,
          xfer_buffer_object_phys(&owner.buffer) == phys,
          "object phys is unchanged by ownership transfer");

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_FRAMEBUFFER, 603u, 64u, &framebuffer) == 0,
          "setup framebuffer object for object_phys");
    check(stats,
          xfer_buffer_object_phys(&framebuffer.buffer) != 0u,
          "framebuffer object has nonzero backing");
    check(stats,
          (xfer_buffer_object_phys(&framebuffer.buffer) & 0xFFFu) == 0u,
          "framebuffer object phys base is page-aligned");

    stale = owner.buffer;
    check(stats,
          xfer_buffer_get_owned(&owner.buffer, 602u, &current) == 0,
          "fetch current owner before releasing for object_phys stale check");
    check(stats,
          xfer_buffer_release_owned(&current) == 0,
          "release transfer object for object_phys stale check");
    check(stats,
          xfer_buffer_object_phys(&stale) == 0u,
          "object_phys returns 0 for a released object");

    check(stats,
          xfer_buffer_release_owned(&other) == 0,
          "object_phys cleanup releases the second transfer object");
    check(stats,
          xfer_buffer_release_owned(&framebuffer) == 0,
          "object_phys cleanup releases the framebuffer object");
}

static void
test_describe(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};
    xfer_buffer_t desc = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 700u, 256u, &owner) == 0,
          "setup owned object for describe");

    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 700u, 0) != 0,
          "describe rejects null out");
    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 0u, &desc) != 0,
          "describe rejects zero context");
    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id + 8000u, BUFFER_KIND_TRANSFER, 700u, &desc) != 0,
          "describe rejects a nonexistent buffer id");
    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_FRAMEBUFFER, 700u, &desc) != 0,
          "describe rejects a kind mismatch");

    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 700u, &desc) == 0,
          "owner can describe its own object");
    check(stats,
          desc.buffer_id == owner.buffer.buffer_id &&
          desc.kind == owner.buffer.kind &&
          desc.size_bytes == owner.buffer.size_bytes,
          "describe returns the full descriptor for the owner");

    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 701u, &desc) != 0,
          "describe denies a context with no relationship to the object");

    check(stats,
          xfer_buffer_borrow(&owner, 701u, BUFFER_BORROW_READ, &borrow) == 0,
          "setup borrow for describe access check");
    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 701u, &desc) == 0,
          "an active borrower can describe the borrowed object");
    check(stats,
          desc.size_bytes == owner.buffer.size_bytes,
          "describe returns the full descriptor for a borrower");
    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "release borrow for describe access check");
    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 701u, &desc) != 0,
          "describe denies a former borrower after unborrow");

    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "describe cleanup releases the object");
    check(stats,
          xfer_buffer_describe(owner.buffer.buffer_id, BUFFER_KIND_TRANSFER, 700u, &desc) != 0,
          "describe rejects a released object");
}

static void
test_get_borrowed(test_stats_t *stats)
{
    xfer_buffer_owner_t owner = {0};
    xfer_buffer_borrow_t borrow = {0};
    xfer_buffer_borrow_t fetched = {0};
    xfer_buffer_dma_mapping_t map_from_map = {0};
    xfer_buffer_dma_mapping_t map_from_get = {0};

    check(stats,
          xfer_buffer_acquire(BUFFER_KIND_TRANSFER, 800u, 4096u, &owner) == 0,
          "setup owner for get_borrowed");
    check(stats,
          xfer_buffer_borrow(&owner, 801u, BUFFER_BORROW_READ | BUFFER_BORROW_WRITE, &borrow) == 0,
          "setup borrow for get_borrowed");

    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 801u, 0, 0) != 0,
          "get_borrowed rejects null out_borrow");
    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 0u, &fetched, 0) != 0,
          "get_borrowed rejects zero context");
    check(stats,
          xfer_buffer_get_borrowed(0u, 801u, &fetched, 0) != 0,
          "get_borrowed rejects zero borrow id");
    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id + 9000u, 801u, &fetched, 0) != 0,
          "get_borrowed rejects a forged borrow id");
    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 802u, &fetched, 0) != 0,
          "get_borrowed denies a context that is not the borrower");

    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 801u, &fetched, &map_from_get) == 0,
          "borrower can get its borrow binding by id");
    check(stats,
          fetched.borrow_id == borrow.borrow_id &&
          fetched.buffer.buffer_id == owner.buffer.buffer_id &&
          fetched.borrower_context_id == 801u &&
          fetched.lender_context_id == 800u &&
          fetched.flags == (BUFFER_BORROW_READ | BUFFER_BORROW_WRITE) &&
          fetched.buffer.size_bytes == owner.buffer.size_bytes,
          "get_borrowed reconstructs the full borrow binding");
    check(stats,
          map_from_get.active == 0u,
          "get_borrowed reports no active mapping before DMA");

    check(stats,
          xfer_buffer_dma_map_borrow(&borrow, 128u, 256u, WASMOS_DMA_DIR_BIDIR, &map_from_map) == 0,
          "map DMA on the borrow for get_borrowed round-trip");
    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 801u, &fetched, &map_from_get) == 0,
          "get_borrowed after DMA map succeeds");
    check(stats,
          map_from_get.active == 1u &&
          map_from_get.attached_via_borrow == 1u &&
          map_from_get.borrow_id == borrow.borrow_id &&
          map_from_get.offset == 128u &&
          map_from_get.length == 256u &&
          map_from_get.direction_flags == WASMOS_DMA_DIR_BIDIR &&
          map_from_get.device_addr == map_from_map.device_addr,
          "get_borrowed reconstructs the active mapping matching dma_map_borrow");

    /* The reconstructed mapping drives the existing sync/unmap ops. */
    check(stats,
          xfer_buffer_dma_sync(&map_from_get, 0u, 256u) == 0,
          "reconstructed mapping syncs a valid subrange");
    check(stats,
          xfer_buffer_dma_sync(&map_from_get, 0u, 257u) != 0,
          "reconstructed mapping rejects an out-of-range sync");
    check(stats,
          xfer_buffer_dma_unmap(&map_from_get) == 0,
          "reconstructed mapping unmaps the borrow DMA");
    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 801u, &fetched, &map_from_get) == 0 &&
          map_from_get.active == 0u,
          "get_borrowed reports no active mapping after unmap");

    check(stats,
          xfer_buffer_unborrow(&borrow) == 0,
          "unborrow the get_borrowed test borrow");
    check(stats,
          xfer_buffer_get_borrowed(borrow.borrow_id, 801u, &fetched, 0) != 0,
          "get_borrowed rejects a released borrow id");
    check(stats,
          xfer_buffer_release_owned(&owner) == 0,
          "get_borrowed cleanup releases the owner object");
}

int
main(void)
{
    test_stats_t stats = {0};

    test_acquire_validation(&stats);
    test_acquire_capacity_semantics(&stats);
    test_object_phys(&stats);
    test_describe(&stats);
    test_get_borrowed(&stats);
    test_get_owned_and_release_owned_validation(&stats);
    test_get_release_extended(&stats);
    test_transfer_ownership_lifecycle(&stats);
    test_transfer_rejects_active_borrows(&stats);
    test_transfer_extended(&stats);
    test_borrow_validation_and_owner_access(&stats);
    test_borrow_rights_matrix(&stats);
    test_simultaneous_multi_handle_borrowing_core(&stats);
    test_simultaneous_multi_handle_borrowing_broker_pattern(&stats);
    test_borrow_rejects_released_object(&stats);
    test_can_access_matrix(&stats);
    test_same_object_matrix(&stats);
    test_framebuffer_borrow_policy(&stats);
    test_framebuffer_borrow_extended(&stats);
    test_reborrow_rules(&stats);
    test_reborrow_extended(&stats);
    test_release_owned_with_active_borrow_rejected(&stats);
    test_owner_side_dma_contract(&stats);
    test_owner_dma_extended(&stats);
    test_borrow_dma_contract(&stats);
    test_borrow_dma_extended(&stats);
    test_dma_sync_unmap_negatives(&stats);
    test_context_teardown_contract(&stats);
    test_context_teardown_extended(&stats);
    test_negative_handle_and_zero_context_cases(&stats);
    test_stale_handle_and_identity_cases(&stats);

    printf("test_xfer_buffer_object: %u passed, %u failed\n",
           stats.passed,
           stats.failed);
    return stats.failed == 0 ? 0 : 1;
}
