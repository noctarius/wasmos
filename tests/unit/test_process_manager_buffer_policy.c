#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "process_manager_buffer.h"
#include "process_manager_buffer_policy.h"

static void
test_filesystem_borrow_accepts_external_read_write_source(void)
{
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FILESYSTEM,
                                                         11u,
                                                         22u,
                                                         PM_BUFFER_BORROW_READ) == 0);
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FILESYSTEM,
                                                         11u,
                                                         22u,
                                                         PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE) == 0);
}

static void
test_filesystem_borrow_rejects_missing_or_self_source(void)
{
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FILESYSTEM,
                                                         11u,
                                                         0u,
                                                         PM_BUFFER_BORROW_READ) != 0);
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FILESYSTEM,
                                                         11u,
                                                         11u,
                                                         PM_BUFFER_BORROW_WRITE) != 0);
}

static void
test_framebuffer_borrow_accepts_local_source_only(void)
{
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FRAMEBUFFER,
                                                         33u,
                                                         0u,
                                                         PM_BUFFER_BORROW_WRITE) == 0);
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FRAMEBUFFER,
                                                         33u,
                                                         44u,
                                                         PM_BUFFER_BORROW_WRITE) != 0);
}

static void
test_borrow_policy_rejects_invalid_flags_and_kinds(void)
{
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FILESYSTEM,
                                                         11u,
                                                         22u,
                                                         0u) != 0);
    assert(process_manager_buffer_policy_validate_borrow(PM_BUFFER_KIND_FRAMEBUFFER,
                                                         33u,
                                                         0u,
                                                         0x4u) != 0);
    assert(process_manager_buffer_policy_validate_borrow(99u,
                                                         11u,
                                                         22u,
                                                         PM_BUFFER_BORROW_READ) != 0);
}

static void
test_allowed_flags_match_supported_read_write_mask(void)
{
    assert(process_manager_buffer_policy_allowed_flags(PM_BUFFER_KIND_FILESYSTEM) ==
           (PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE));
    assert(process_manager_buffer_policy_allowed_flags(PM_BUFFER_KIND_FRAMEBUFFER) ==
           (PM_BUFFER_BORROW_READ | PM_BUFFER_BORROW_WRITE));
    assert(process_manager_buffer_policy_allowed_flags(99u) == 0u);
}

int
main(void)
{
    test_filesystem_borrow_accepts_external_read_write_source();
    test_filesystem_borrow_rejects_missing_or_self_source();
    test_framebuffer_borrow_accepts_local_source_only();
    test_borrow_policy_rejects_invalid_flags_and_kinds();
    test_allowed_flags_match_supported_read_write_mask();
    printf("test_process_manager_buffer_policy: ok\n");
    return 0;
}
