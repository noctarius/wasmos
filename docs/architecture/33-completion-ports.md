# Completion Ports

> **Documentation status: Design proposal.** Completion queues are not
> implemented. This document specifies a kernel-owned completion transport that
> feeds the proposed user-space coroutine, future, and promise runtime.

## Purpose and Scope

A completion queue (CQ) is a kernel-owned, bounded queue of terminal
asynchronous-operation results. It is a transport and batching primitive: a
user-space future/promise runtime maps each completion to its local promise and
decides which coroutine or continuation to wake.

The CQ storage is fully kernel-owned. `cq_dequeue()` copies completion records
to a caller-supplied output array. This avoids a shared-ring registration,
pinning, memory-ordering, and consumer-index ownership ABI.

### Kernel-level API

```c
typedef uint32_t cq_handle_t;
typedef uint32_t endpoint_t;
typedef uint32_t operation_token_t;
typedef uint64_t deadline_t; /* absolute scheduler ticks */

enum cq_completion_flags {
    CQ_COMPLETION_NONE      = 0,
    CQ_COMPLETION_CANCELLED = 1u << 0,
    CQ_COMPLETION_TIMED_OUT = 1u << 1,
    CQ_COMPLETION_PEER_DIED = 1u << 2,
};

typedef struct cq_completion {
    operation_token_t operation_token;
    uintptr_t cookie;       /* opaque client value, never interpreted by kernel */
    int32_t status;
    uint32_t result_length;
    uint32_t flags;
} cq_completion_t;

typedef struct cq_async_submission {
    cq_handle_t queue_handle;
    endpoint_t endpoint_handle; /* target service/driver message endpoint */

    uint32_t buffer_id;
    uint32_t borrow_id;
    uint32_t request_length;
    deadline_t deadline;         /* 0 means no deadline */

    uintptr_t cookie;
    uint32_t flags;
} cq_async_submission_t;

/* Metadata delivered to the selected service in IPC_MESSAGE_TYPE_CQ_OPERATION. */
typedef struct cq_async_operation {
    operation_token_t operation_token;
    uint32_t buffer_id;
    uint32_t borrow_id;
    uint32_t request_length;
    deadline_t deadline;
    uint32_t flags;
} cq_async_operation_t;

/* Kernel allocates a bounded CQ and its private notification endpoint. */
int cq_create(uint32_t capacity, cq_handle_t *out_queue);
int cq_destroy(cq_handle_t queue);
int cq_notification_endpoint(cq_handle_t queue, endpoint_t *out_endpoint);

/* Returns CQ_ERR_FULL before sending when the queue cannot reserve one
 * non-droppable terminal completion slot. */
int cq_async_submit(const cq_async_submission_t *submission,
                    operation_token_t *out_token);

/* Copies up to capacity records; returns the number copied or a negative error. */
int cq_dequeue(cq_handle_t queue, cq_completion_t *out, uint32_t capacity);

/* Only the service endpoint selected at submission may complete this opaque,
 * generation-tagged 32-bit token.  The first terminal transition wins. */
int cq_operation_complete(operation_token_t token, int32_t status,
                          uint32_t result_length, uint32_t flags);
int cq_operation_cancel(operation_token_t token);
```

An `operation_token_t` is an opaque 32-bit kernel handle, implemented as a
generation-tagged table slot.  It is never a pointer, is bound to the submitting
context, CQ, and selected service endpoint, and is not reused until the old
operation is retired.  A stale, duplicated, or unauthorised completion fails;
it can never resolve another live operation.

### Notification and queue semantics

Each CQ owns a notification endpoint.  A notification is a counter-based,
payload-free doorbell, not an IPC message: consumers use `ipc_wait()` to consume
it and must not call `ipc_message_read_last()`.

The kernel coalesces notifications.  Under the CQ lock it enqueues a completion
and, only when transitioning from empty to non-empty, increments the notification
endpoint.  The dequeue path drains records and clears this signalled state under
the same CQ lock.  A producer which subsequently makes the queue non-empty sends
the next doorbell.  This is the lost-wakeup protocol; callers do not arm,
disarm, or test the CQ themselves.

For use in an IPC select set, notification endpoints must publish readiness to
select watchers just as message endpoints do.  The current `ipc_notify_from()`
increments and wakes direct waiters but does not yet call `poll_notify()`; CQ
implementation must add that readiness publication before this example is
usable with `ipc_select_wait()`.

### Client/application-level use

```c
cq_handle_t queue;
endpoint_t completion_notification;
int32_t selector;

if (cq_create(128, &queue) != 0 ||
    cq_notification_endpoint(queue, &completion_notification) != 0) {
    // handle error
}

selector = ipc_select_create();
if (selector < 0 ||
    ipc_select_add(selector, completion_notification) != 0) {
    // destroy selector/CQ and handle error
}

cq_async_submission_t submission = create_some_request();
operation_token_t token;
if (cq_async_submit(&submission, &token) != 0) {
    // CQ backpressure, invalid borrow/endpoint, or immediate submission error
}

for (;;) {
    if (ipc_select_wait(selector) != 0) {
        continue; /* timeout/spurious wake/error policy belongs to the runtime */
    }

    (void)ipc_wait(completion_notification); /* consume the payload-free doorbell */

    cq_completion_t completions[64];
    int count;
    while ((count = cq_dequeue(queue, completions, 64)) > 0) {
        for (int i = 0; i < count; ++i) {
            resolve_completion(&completions[i]);
        }
    }
}
```

`resolve_completion()` removes the token from the local operation table and
settles its promise.  It must tolerate a missing entry: that represents a
locally cancelled/retired operation or a duplicate/stale delivery, not memory
that may be dereferenced.

### Server/application-level use

The kernel posts `IPC_MESSAGE_TYPE_CQ_OPERATION` to the selected service
endpoint.  The message identifies the operation token; the server extracts the
validated operation metadata from the message and accesses the already-granted
transfer-buffer borrow.

```c
for (;;) {
    wasmos_ipc_message_t message;
    if (wasmos_sys_event_loop_poll(&loop, 16) <= 0) {
        continue;
    }

    /* Handler for IPC_MESSAGE_TYPE_CQ_OPERATION: */
    cq_async_operation_t request;
    if (extract_cq_operation(&message, &request)) {
        dispatch_request(&request);
    }
}

static void dispatch_request(const cq_async_operation_t *request)
{
    const struct fs_request_header *header = read_fs_request(
        request->buffer_id, request->borrow_id, request->request_length);

    switch (header->opcode) {
    case FS_READ:
        handle_read(request->operation_token,
                    (const struct fs_read_request *)header->payload);
        break;
    case FS_WRITE:
        handle_write(request->operation_token,
                     (const struct fs_write_request *)header->payload);
        break;
    default:
        (void)cq_operation_complete(request->operation_token, -ENOSYS, 0, 0);
        break;
    }
}
```

### Terminal ownership, cancellation, and backpressure

Terminal ownership means that one kernel operation record has exactly one winner:
normal service completion, deadline expiry, cancellation, service death, or
submitter/CQ teardown.  The winner atomically changes the record from `PENDING`
to one terminal state, releases the operation's kernel-side references, and
causes at most one CQ completion to be enqueued.  Every losing path observes an
already-terminal record and does nothing.  This prevents completion-after-timeout
and completion-after-cancel from producing a second result.

Cancellation has two distinct effects:

1. The caller's future becomes cancelled when cancellation wins the terminal
   transition.
2. The kernel may ask the service to stop work, but this is best-effort.  A
   service which has already started may finish; its late completion is rejected
   as stale.

The caller owns the transfer buffer and therefore determines when it may release
that object.  It must keep the buffer and the submitted borrow valid until it
has observed a terminal result (or has completed the documented cancellation /
teardown protocol).  CQ completion never transfers ownership of the buffer.

Terminal completions are never silently dropped.  `cq_async_submit()` reserves
the capacity needed for one terminal completion before it posts work to the
service.  If it cannot reserve that space it returns `CQ_ERR_FULL` and no
operation is sent.  This provides explicit caller-visible backpressure and
ensures a service can always publish the completion for accepted work.

### Coroutine and future interaction

The coroutine runtime remains necessary: it owns ready queues, `await`,
continuation scheduling, timeout/cancellation policy, and the local
future/promise state.  The CQ does not replace it.  Instead, it supplies an
efficient asynchronous source of promise resolution, particularly for the
networking path where many receive, transmit, and timer-adjacent operations may
complete in batches.

```c
void dispatch_completion(const cq_completion_t *completion)
{
    future_state_t *future = operation_table_remove(completion->operation_token);
    if (future == NULL) {
        return; /* cancelled, retired, stale, or duplicate completion */
    }

    future_complete_once(future, completion->status, completion->result_length,
                         completion->flags);
}
```

Ordinary non-blocking IPC should still resolve promises directly for normal
request/reply service RPC.  The CQ is not a reason to discard that working
transport; it is the stronger operation-completion primitive needed where
kernel-mediated execution, bounded outstanding work, and batched completion
delivery are useful.  Both paths feed the same future/promise contract.
