#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "xfer_buffer.h"
#include "xfer_buffer/policy.h"

static void
test_transfer_borrow_accepts_external_read_write_source(void)
{
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_TRANSFER,
                                                         11u,
                                                         22u,
                                                         BUFFER_BORROW_READ) == 0);
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_TRANSFER,
                                                         11u,
                                                         22u,
                                                         BUFFER_BORROW_READ | BUFFER_BORROW_WRITE) == 0);
}

static void
test_transfer_borrow_rejects_missing_or_self_source(void)
{
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_TRANSFER,
                                                         11u,
                                                         0u,
                                                         BUFFER_BORROW_READ) != 0);
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_TRANSFER,
                                                         11u,
                                                         11u,
                                                         BUFFER_BORROW_WRITE) != 0);
}

static void
test_framebuffer_borrow_accepts_local_source_only(void)
{
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_FRAMEBUFFER,
                                                         33u,
                                                         0u,
                                                         BUFFER_BORROW_WRITE) == 0);
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_FRAMEBUFFER,
                                                         33u,
                                                         44u,
                                                         BUFFER_BORROW_WRITE) != 0);
}

static void
test_borrow_policy_rejects_invalid_flags_and_kinds(void)
{
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_TRANSFER,
                                                         11u,
                                                         22u,
                                                         0u) != 0);
    assert(xfer_buffer_policy_validate_borrow(BUFFER_KIND_FRAMEBUFFER,
                                                         33u,
                                                         0u,
                                                         0x4u) != 0);
    assert(xfer_buffer_policy_validate_borrow(99u,
                                                         11u,
                                                         22u,
                                                         BUFFER_BORROW_READ) != 0);
}

static void
test_allowed_flags_match_supported_read_write_mask(void)
{
    assert(xfer_buffer_policy_allowed_flags(BUFFER_KIND_TRANSFER) ==
           (BUFFER_BORROW_READ | BUFFER_BORROW_WRITE));
    assert(xfer_buffer_policy_allowed_flags(BUFFER_KIND_FRAMEBUFFER) ==
           (BUFFER_BORROW_READ | BUFFER_BORROW_WRITE));
    assert(xfer_buffer_policy_allowed_flags(99u) == 0u);
}

int
main(void)
{
    test_transfer_borrow_accepts_external_read_write_source();
    test_transfer_borrow_rejects_missing_or_self_source();
    test_framebuffer_borrow_accepts_local_source_only();
    test_borrow_policy_rejects_invalid_flags_and_kinds();
    test_allowed_flags_match_supported_read_write_mask();
    printf("test_xfer_buffer_policy: ok\n");
    return 0;
}
