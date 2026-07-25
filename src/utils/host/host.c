/* host - resolve a hostname to an IPv4 address via net.stack DNS.
 *
 * Usage: host <name>
 *
 * Looks up the `net.stack` service and issues NET_IPC_RESOLVE through the shared
 * wasmos_net_resolve() helper, then prints the resolved address.
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/net.h"
#include "wasmos/startup.h"

static void app_str(char* buf, int cap, int* n, const char* s) {
    for (int i = 0; s[i] != '\0' && *n < cap - 1; ++i) {
        buf[(*n)++] = s[i];
    }
}

static void app_dec(char* buf, int cap, int* n, uint32_t v) {
    char tmp[12];
    int t = 0;
    if (v == 0u && *n < cap - 1) {
        buf[(*n)++] = '0';
        return;
    }
    while (v > 0u && t < (int)sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (t > 0 && *n < cap - 1) {
        buf[(*n)++] = tmp[--t];
    }
}

/* Format a network-order IPv4 word (octet a in the low byte) as "a.b.c.d". */
static void app_ipv4(char* buf, int cap, int* n, uint32_t addr_no) {
    for (int i = 0; i < 4; ++i) {
        if (i != 0)
            app_str(buf, cap, n, ".");
        app_dec(buf, cap, n, (addr_no >> (i * 8)) & 0xFFu);
    }
}

/* Copy the first whitespace-delimited token of `src` into `dst` (bounded). */
static void first_token(const char* src, char* dst, int cap) {
    int i = 0;
    int j = 0;
    while (src[i] == ' ' || src[i] == '\t')
        i++;
    while (src[i] != '\0' && src[i] != ' ' && src[i] != '\t' && j < cap - 1)
        dst[j++] = src[i++];
    dst[j] = '\0';
}

int main(void) {
    char args[128];
    char host[128];
    char line[160];
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t rid = 1;
    int32_t stack_ep = -1;
    uint32_t addr = 0u;
    int n = 0;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    first_token(args, host, (int)sizeof(host));
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[host] setup failed");
        return 1;
    }
    if (host[0] == '\0') {
        puts("[host] usage: host <name>");
        return 1;
    }
    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", rid++);
        if (stack_ep < 0)
            (void)wasmos_sched_yield();
    }
    if (stack_ep < 0) {
        puts("[host] no net.stack");
        return 1;
    }
    if (wasmos_net_resolve(stack_ep, reply_ep, host, rid++, &addr) != 0) {
        app_str(line, (int)sizeof(line), &n, "[host] ");
        app_str(line, (int)sizeof(line), &n, host);
        app_str(line, (int)sizeof(line), &n, ": not found");
        if (n < (int)sizeof(line))
            line[n] = '\0';
        puts(line);
        return 1;
    }
    app_str(line, (int)sizeof(line), &n, "[host] ");
    app_str(line, (int)sizeof(line), &n, host);
    app_str(line, (int)sizeof(line), &n, " -> ");
    app_ipv4(line, (int)sizeof(line), &n, addr);
    if (n < (int)sizeof(line))
        line[n] = '\0';
    puts(line);
    return 0;
}
