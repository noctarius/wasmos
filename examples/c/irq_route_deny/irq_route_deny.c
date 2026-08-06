#include "stdio.h"
#include "wasmos/api.h"

int main(int argc, char** argv) {
    int32_t endpoint = 0;
    int32_t rc = 0;

    (void)argc;
    (void)argv;

    endpoint = wasmos_ipc_create_endpoint();
    if (endpoint <= 0) {
        puts("irq-route-deny: endpoint failed");
        return 1;
    }

    /* This app holds no irq.route capability at all, so the host call refuses it
     * before any per-line policy check — same reason code either way. */
    rc = wasmos_irq_route_ipc(1, endpoint);
    if (rc != WASMOS_ERR_IRQ_NOT_AUTHORIZED) {
        puts("irq-route-deny: expected deny");
        return 1;
    }

    puts("irq-route-deny: ok");
    return 0;
}
