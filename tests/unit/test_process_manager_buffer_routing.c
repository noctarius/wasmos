#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "process_manager_buffer_routing.h"

static void
test_plain_client_keeps_own_context(void)
{
    assert(process_manager_fs_buffer_target_context(0, 41u, 0u, 0, 0u, 0) == 41u);
}

static void
test_fs_manager_follows_borrow_source(void)
{
    assert(process_manager_fs_buffer_target_context(1, 20u, 77u, 0, 0u, 0) == 77u);
}

static void
test_fs_manager_without_borrow_keeps_own_context(void)
{
    assert(process_manager_fs_buffer_target_context(1, 20u, 0u, 0, 0u, 0) == 20u);
}

static void
test_client_redirects_to_backend_peer_context(void)
{
    assert(process_manager_fs_buffer_target_context(0, 41u, 0u, 1, 88u, 0) == 88u);
}

static void
test_client_reply_from_fs_manager_does_not_redirect(void)
{
    assert(process_manager_fs_buffer_target_context(0, 41u, 0u, 1, 20u, 1) == 41u);
}

static void
test_invalid_peer_does_not_redirect(void)
{
    assert(process_manager_fs_buffer_target_context(0, 41u, 0u, 1, 0u, 0) == 41u);
}

int
main(void)
{
    test_plain_client_keeps_own_context();
    test_fs_manager_follows_borrow_source();
    test_fs_manager_without_borrow_keeps_own_context();
    test_client_redirects_to_backend_peer_context();
    test_client_reply_from_fs_manager_does_not_redirect();
    test_invalid_peer_does_not_redirect();
    printf("test_process_manager_buffer_routing: ok\n");
    return 0;
}
