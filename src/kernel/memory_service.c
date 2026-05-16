#include "memory_service.h"
#include "memory.h"

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
    if (ipc_send_from(g_mem_service_context, reply.destination, &reply) != IPC_OK) {
        return -1;
    }
    return status == 0 ? 0 : -1;
}

int
memory_service_handle_fault_ipc(uint32_t fault_context_id, uint64_t fault_addr, uint64_t error_code)
{
    if (g_mem_service_endpoint == IPC_ENDPOINT_NONE ||
        g_mem_service_reply_endpoint == IPC_ENDPOINT_NONE) {
        return -1;
    }

    ipc_message_t req;
    req.type = IPC_MEM_FAULT;
    req.source = g_mem_service_reply_endpoint;
    req.destination = g_mem_service_endpoint;
    req.request_id = g_mem_service_request_id++;
    if (g_mem_service_request_id == 0) {
        g_mem_service_request_id = 1;
    }
    req.arg0 = (uint32_t)fault_addr;
    req.arg1 = (uint32_t)(fault_addr >> 32);
    req.arg2 = (uint32_t)error_code;
    req.arg3 = fault_context_id;

    if (ipc_send_from(IPC_CONTEXT_KERNEL, g_mem_service_endpoint, &req) != IPC_OK) {
        return -1;
    }

    if (memory_service_process_once() != 0) {
        return -1;
    }

    ipc_message_t reply;
    int rc = ipc_recv_for(IPC_CONTEXT_KERNEL, g_mem_service_reply_endpoint, &reply);
    if (rc != IPC_OK) {
        return -1;
    }
    if (reply.type != IPC_MEM_FAULT_REPLY || reply.request_id != req.request_id) {
        return -1;
    }
    if ((int32_t)reply.arg0 != 0) {
        return -1;
    }
    return 0;
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
