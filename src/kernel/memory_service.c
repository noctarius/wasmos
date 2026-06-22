/* memory_service.c - Userspace memory allocation service client stub.
 * Wraps IPC calls to the in-kernel memory service endpoint so that WASM
 * host-call handlers can request heap pages without touching the physical
 * allocator directly.  Retries up to MEM_SVC_SEND_RETRY_LIMIT times. */
#include "memory_service.h"
#include "memory.h"

#define MEM_SVC_SEND_RETRY_LIMIT 4096

static uint32_t g_mem_service_context;
static uint32_t g_mem_service_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_mem_service_reply_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_mem_service_request_id = 1;

void
memory_service_register(uint32_t context_id, uint32_t endpoint, uint32_t reply_endpoint)
{
    g_mem_service_context = context_id;
    g_mem_service_endpoint = endpoint;
    g_mem_service_reply_endpoint = reply_endpoint;
    g_mem_service_request_id = 1;
}

static int
memory_service_handle_request(const ipc_message_t *req, ipc_message_t *reply)
{
    if (!req || !reply) {
        return -1;
    }
    if (req->type != IPC_MEM_FAULT) {
        return -1;
    }

    uint64_t addr = ((uint64_t)req->arg1 << 32) | req->arg0;
    uint64_t error_code = req->arg2;
    uint32_t context_id = req->arg3;
    uint64_t mapped_base = 0;
    int status = mm_handle_page_fault(context_id, addr, error_code, &mapped_base);

    reply->type = IPC_MEM_FAULT_REPLY;
    reply->source = g_mem_service_endpoint;
    reply->destination = req->source;
    reply->request_id = req->request_id;
    reply->arg0 = (uint32_t)status;
    reply->arg1 = (uint32_t)mapped_base;
    reply->arg2 = (uint32_t)(mapped_base >> 32);
    reply->arg3 = 0;
    return status;
}

int
memory_service_process_once(void)
{
    if (g_mem_service_endpoint == IPC_ENDPOINT_NONE ||
        g_mem_service_context == 0) {
        return -1;
    }

    ipc_message_t req;
    int rc = ipc_recv_for(g_mem_service_context, g_mem_service_endpoint, &req);
    if (rc == IPC_EMPTY) {
        return 1;
    }
    if (rc != IPC_OK) {
        return -1;
    }

    ipc_message_t reply;
    int status = memory_service_handle_request(&req, &reply);
    int send_rc;
    int tries = 0;
    do {
        send_rc = ipc_send_from(g_mem_service_context, reply.destination, &reply);
    } while (send_rc == IPC_ERR_FULL && ++tries < MEM_SVC_SEND_RETRY_LIMIT);
    if (send_rc != IPC_OK) {
        return -1;
    }
    return status == 0 ? 0 : -1;
}

int
memory_service_handle_fault_ipc(uint32_t fault_context_id, uint64_t fault_addr, uint64_t error_code)
{
    /* Resolve the fault directly rather than round-tripping through the
     * mem-service IPC endpoint.  memory_service_handle_request() only ever
     * calls mm_handle_page_fault() for IPC_MEM_FAULT, and that endpoint is also
     * drained by the mem-service worker thread.  Under SMP the worker (on
     * another CPU) can consume the just-sent IPC_MEM_FAULT before this inline
     * recv does, so the inline path sees IPC_EMPTY and reports the fault as
     * unhandled — turning a recoverable demand-fault into a kernel panic
     * (observed in the pagefault selftest on wasm3+SMP).  Calling
     * mm_handle_page_fault() directly removes both the indirection and the
     * dual-consumer race; it is an in-kernel routine, so no IPC is needed. */
    uint64_t mapped_base = 0;
    return mm_handle_page_fault(fault_context_id, fault_addr, error_code, &mapped_base);
}

process_run_result_t
memory_service_entry(process_t *process, void *arg)
{
    (void)arg;
    if (!process) {
        return PROCESS_RUN_IDLE;
    }

    int rc = memory_service_process_once();
    if (rc == 0) {
        return PROCESS_RUN_YIELDED;
    }
    if (rc == 1) {
        process_block_on_ipc(process);
        return PROCESS_RUN_BLOCKED;
    }
    return PROCESS_RUN_IDLE;
}
