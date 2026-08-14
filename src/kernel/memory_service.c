/* memory_service.c - In-kernel page-fault service thread.
 * Owns an IPC endpoint that accepts IPC_MEM_FAULT requests, resolves each
 * through mm_handle_page_fault(), and answers with IPC_MEM_FAULT_REPLY carrying
 * the status and the mapped base.  The reply send is retried up to
 * MEM_SVC_SEND_RETRY_LIMIT times while the peer queue is full. */
#include "memory_service.h"
#include "memory.h"

#define MEM_SVC_SEND_RETRY_LIMIT 4096

static uint32_t g_mem_service_context;
static uint32_t g_mem_service_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_mem_service_reply_endpoint = IPC_ENDPOINT_NONE;
static uint32_t g_mem_service_request_id = 1;
static uint32_t g_mem_service_select_id = 0;
static uint8_t g_mem_service_select_ready = 0;

/* Lazily create the select set watching the request endpoint. Select-style
 * (rather than a bare single-endpoint blocking recv) keeps this service on the
 * same wait pattern as process-manager and other multi-endpoint consumers. */
static int memory_service_select_setup(void) {
    if (g_mem_service_select_ready) {
        return 0;
    }
    if (g_mem_service_endpoint == IPC_ENDPOINT_NONE || g_mem_service_context == 0) {
        return -1;
    }
    uint32_t eps[1] = {g_mem_service_endpoint};
    if (ipc_select_listen(g_mem_service_context, eps, 1, &g_mem_service_select_id) != IPC_OK) {
        return -1;
    }
    g_mem_service_select_ready = 1;
    return 0;
}

/* Records the identity the service runs under.  There is exactly one such
 * registration in the kernel; a second call replaces it wholesale and the
 * previous endpoints are simply forgotten.
 *
 * The endpoints are borrowed ids, not created here — the caller owns their
 * lifetime.  Call it before memory_service_serve_one, which refuses to build its
 * select set without them.  The select set is not rebuilt on re-registration, so
 * changing the endpoint after the first serve leaves the service listening on
 * the old one. */
void memory_service_register(uint32_t context_id, uint32_t endpoint, uint32_t reply_endpoint) {
    g_mem_service_context = context_id;
    g_mem_service_endpoint = endpoint;
    g_mem_service_reply_endpoint = reply_endpoint;
    g_mem_service_request_id = 1;
}

static int memory_service_handle_request(const ipc_message_t* req, ipc_message_t* reply) {
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

/* BLOCKS on the service's select set until one IPC_MEM_FAULT arrives, resolves
 * it, and sends the reply.
 *
 * Returns 0 when the fault was resolved and the reply sent, 1 on a spurious wake
 * with nothing to receive (the caller should simply loop), and -1 when the
 * service is unregistered, the receive failed, the request could not be resolved,
 * or the reply could not be delivered.  A -1 does not distinguish "fault refused"
 * from "transport broken"; the requester learns the fault status from the reply's
 * arg0 either way.
 *
 * The reply is retried while the peer's queue is full, up to
 * MEM_SVC_SEND_RETRY_LIMIT times, spinning rather than parking. */
int memory_service_serve_one(void) {
    if (memory_service_select_setup() != 0) {
        return -1;
    }

    ipc_message_t req;
    uint32_t ready_ep = IPC_ENDPOINT_NONE;
    /* Block on the select set until the request endpoint has a message (no
     * timeout — mem-service has no periodic work, so it sleeps until woken). */
    int rc = ipc_select_recv(g_mem_service_select_id, g_mem_service_context, &ready_ep, &req, 0);
    if (rc == IPC_EMPTY) {
        return 1; /* spurious wake / lost race — caller loops and re-blocks */
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

/* Resolves a fault synchronously on the faulting CPU, for the exception handler
 * to call directly.  Returns 0 when the page was mapped and -1 when the fault is
 * not one the region table can satisfy.  Despite the name it sends no IPC and
 * does not block; the mapped base is discarded. */
int memory_service_handle_fault_ipc(uint32_t fault_context_id, uint64_t fault_addr,
                                    uint64_t error_code) {
    /* Resolves the fault in-line instead of round-tripping through the
     * mem-service endpoint, which the worker thread also drains: under SMP the
     * worker can consume the just-sent IPC_MEM_FAULT first, leaving an inline
     * receive with IPC_EMPTY and a recoverable demand fault reported as
     * unhandled.  mm_handle_page_fault() is an in-kernel routine and is all the
     * request handler would have called anyway, so no IPC is involved. */
    uint64_t mapped_base = 0;
    return mm_handle_page_fault(fault_context_id, fault_addr, error_code, &mapped_base);
}

/* Scheduler entry point for the service process: serves at most one request per
 * dispatch and yields, so the scheduler regains control between requests rather
 * than the service owning a loop.  arg is unused.
 *
 * Returns PROCESS_RUN_YIELDED normally and PROCESS_RUN_IDLE for a NULL process.
 * The serve result is deliberately ignored — a failed request must not stop the
 * service from being dispatched again. */
process_run_result_t memory_service_entry(process_t* process, void* arg) {
    (void)arg;
    if (!process) {
        return PROCESS_RUN_IDLE;
    }

    /* memory_service_serve_one() blocks on the select set between requests
     * rather than busy-polling, so the service sleeps on its endpoint event and
     * is only re-dispatched when a request actually arrives. */
    (void)memory_service_serve_one();
    return PROCESS_RUN_YIELDED;
}
