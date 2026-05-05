#include "stdio.h"
#include "wasmos/api.h"

int
main(int argc, char **argv)
{
    int32_t endpoint = 0;
    int32_t rc = 0;

    (void)argc;
    (void)argv;

    endpoint = wasmos_ipc_create_endpoint();
    if (endpoint <= 0) {
        puts("irq-route-deny: endpoint failed");
        return 1;
    }

    rc = wasmos_irq_route(1, endpoint);
    if (rc != -1) {
        puts("irq-route-deny: expected deny");
        return 1;
    }

    puts("irq-route-deny: ok");
    return 0;
}
