/* irq_route_deny - negative half of the IRQ routing capability test.
 *
 * The counterpart to irq_route_allow: this app holds no irq.route capability,
 * so a routing attempt must be refused outright. It asks for line 1 — the very
 * line irq_route_allow is permitted — which is what makes the deny attributable
 * to the missing capability rather than to a line policy.
 *
 * Prints "ok" and exits 0 when the call returns WASMOS_ERR_IRQ_NOT_AUTHORIZED,
 * otherwise exits 1.
 *
 * Precondition: the app must remain absent from the irq.route capability grants
 * in src/kernel/policy.c; granting it turns the expected deny into a success
 * and this app then reports failure. */
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
