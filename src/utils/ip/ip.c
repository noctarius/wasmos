/* ip - minimal interface-address CLI for the native net-stack.
 *
 * Usage (the whole command line arrives as one string via wasmos_startup_args;
 * a bare `ip` or `ip addr` is treated as `ip addr show`):
 *   ip addr show                         list interfaces and their addresses
 *   ip addr add <a.b.c.d>/<prefix> dev <name>   set an interface address
 *   ip addr del dev <name>               clear an interface address
 *   ip dev <name> up|down                administrative interface state
 *   ip dhcp <name> on|off                start/stop the DHCP client
 *   ip dns show                          list the configured resolvers
 *   ip dns set <ip> [<ip2>]              replace the resolver list
 *   ip dns del <ip>                      drop one resolver from the list
 *
 * `show` and `list` are interchangeable for both `ip addr` and `ip dns`, and a
 * bare `ip dns` means `ip dns show`. `ip addr add` defaults the gateway to the
 * network's .1 host and the prefix to /24 when it is omitted. `ip dns set`
 * replaces the whole list, so it is also how the list is cleared to one entry;
 * `ip dns del` re-reads the list, drops the named address, and writes back what
 * is left. At most two resolvers exist, since the reply carries them in two
 * message words.
 *
 * `<name>` is ethN/enN; its trailing digits are the interface index. Talks to the
 * `net.stack` service via NET_IPC_IFADDR_ADD/DEL/LIST, NET_IPC_IF_SET_STATE,
 * NET_IPC_DHCP_SET, and NET_IPC_DNS_SET/LIST.
 *
 * Every subcommand prints one `[ip] ...` line and returns 0 on success, 1 on
 * any failure (bad arguments, no net.stack, or a non-RESP reply).
 */
#include <stdint.h>

#include "stdio.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/startup.h"
#include "wasmos_driver_abi.h"

/* Upper bound for `ip addr show`; net-stack caps its interface table at 8. */
#define IP_MAX_IFACES 8u
/* Wire size of one net_ifaddr_record_v1_t (wasmos_driver_abi.h): six 32-bit
 * little-endian words, packed.  This tool reads the fields by byte offset rather
 * than through the struct, so this must track that layout: 0=version,
 * 4=if_index, 8=address, 12=netmask, 16=gateway, 20=flags, with every IPv4 word
 * in network byte order (first octet in the low byte). */
#define IP_IFADDR_RECORD_BYTES 24u

static void put_u32(uint8_t* out, uint32_t off, uint32_t v) {
    out[off] = (uint8_t)v;
    out[off + 1u] = (uint8_t)(v >> 8u);
    out[off + 2u] = (uint8_t)(v >> 16u);
    out[off + 3u] = (uint8_t)(v >> 24u);
}

static uint32_t get_u32(const uint8_t* in, uint32_t off) {
    return (uint32_t)in[off] | ((uint32_t)in[off + 1u] << 8u) | ((uint32_t)in[off + 2u] << 16u) |
           ((uint32_t)in[off + 3u] << 24u);
}

static int str_eq(const char* a, const char* b) {
    uint32_t i = 0u;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        if (a[i] != b[i])
            return 0;
    }
    return a[i] == b[i];
}

/* Split `s` in place into whitespace-delimited tokens. Returns the count. */
static int tokenize(char* s, char** tok, int max) {
    int n = 0;
    uint32_t i = 0u;
    while (s[i] != '\0' && n < max) {
        while (s[i] == ' ' || s[i] == '\t')
            s[i++] = '\0';
        if (s[i] == '\0')
            break;
        tok[n++] = &s[i];
        while (s[i] != '\0' && s[i] != ' ' && s[i] != '\t')
            i++;
    }
    return n;
}

/* Parse decimal digits until a non-digit; stores the value and returns the
 * number of characters consumed (0 if none). */
static uint32_t parse_dec(const char* s, uint32_t* out) {
    uint32_t v = 0u;
    uint32_t i = 0u;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10u + (uint32_t)(s[i] - '0');
        i++;
    }
    *out = v;
    return i;
}

/* Parse "a.b.c.d" into a network-order word (octet a in the low byte, matching
 * lwIP's ip4_addr_get/set_u32). Returns 0 on success. */
static int parse_ipv4_no(const char* s, uint32_t* out) {
    uint32_t oct[4];
    uint32_t idx = 0u;
    uint32_t pos = 0u;
    for (idx = 0u; idx < 4u; ++idx) {
        uint32_t consumed = parse_dec(s + pos, &oct[idx]);
        if (consumed == 0u || oct[idx] > 255u)
            return -1;
        pos += consumed;
        if (idx < 3u) {
            if (s[pos] != '.')
                return -1;
            pos++;
        }
    }
    if (s[pos] != '\0')
        return -1;
    *out = oct[0] | (oct[1] << 8u) | (oct[2] << 16u) | (oct[3] << 24u);
    return 0;
}

static uint32_t prefix_to_mask_no(uint32_t prefix) {
    uint8_t m[4] = {0u, 0u, 0u, 0u};
    uint32_t b;
    if (prefix > 32u)
        prefix = 32u;
    for (b = 0u; b < prefix; ++b)
        m[b / 8u] |= (uint8_t)(0x80u >> (b % 8u));
    return (uint32_t)m[0] | ((uint32_t)m[1] << 8u) | ((uint32_t)m[2] << 16u) |
           ((uint32_t)m[3] << 24u);
}

static uint32_t mask_prefix(uint32_t mask_no) {
    uint32_t count = 0u;
    for (uint32_t i = 0u; i < 32u; ++i) {
        if ((mask_no >> i) & 1u)
            count++;
    }
    return count;
}

/* Append a decimal number to buf at *n (bounded by cap). */
static void app_dec(char* buf, int cap, int* n, uint32_t v) {
    char tmp[10];
    int t = 0;
    if (v == 0u) {
        if (*n < cap)
            buf[(*n)++] = '0';
        return;
    }
    while (v > 0u && t < (int)sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (t > 0 && *n < cap)
        buf[(*n)++] = tmp[--t];
}

static void app_str(char* buf, int cap, int* n, const char* s) {
    for (uint32_t i = 0u; s[i] != '\0' && *n < cap; ++i)
        buf[(*n)++] = s[i];
}

static void app_ipv4(char* buf, int cap, int* n, uint32_t addr_no) {
    for (int i = 0; i < 4; ++i) {
        if (i != 0)
            app_str(buf, cap, n, ".");
        app_dec(buf, cap, n, (addr_no >> (i * 8)) & 0xFFu);
    }
}

/* Map an interface name like "eth0"/"en1" to its index (trailing digits). */
static uint32_t name_to_index(const char* name) {
    uint32_t i = 0u;
    uint32_t idx = 0u;
    while (name[i] != '\0' && (name[i] < '0' || name[i] > '9'))
        i++;
    (void)parse_dec(name + i, &idx);
    return idx;
}

/* Block on `ep` until the reply carrying `request_id` arrives, discarding up to
 * 16 messages that carry a different id. Returns 0 with *message filled, or -1
 * if the endpoint errors or 16 mismatched messages arrive first. */
static int recv_reply(int32_t ep, int32_t request_id, wasmos_ipc_message_t* message) {
    for (int rounds = 0; rounds < 16; ++rounds) {
        int got = 0;
        for (int spin = 0; spin < 300000; ++spin) {
            if (wasmos_ipc_select_one(ep) == 1) {
                wasmos_ipc_message_read_last(message);
                got = 1;
                break;
            }
            (void)wasmos_sched_yield();
        }
        if (!got)
            return -1;
        if (message->request_id == request_id)
            return 0;
    }
    return -1;
}

static void usage(void) {
    puts("[ip] usage: ip addr show | ip addr add <a.b.c.d>/<prefix> dev <name> | ip addr del dev "
         "<name> | ip dev <name> up|down | ip dhcp <name> on|off | ip dns show | ip dns set <ip> "
         "[<ip2>] | ip dns del <ip>");
}

static int cmd_dhcp(int32_t stack_ep, int32_t reply_ep, int32_t* rid, const char* dev,
                    uint32_t on) {
    wasmos_ipc_message_t message;
    if (wasmos_ipc_send(
            stack_ep, reply_ep, NET_IPC_DHCP_SET, *rid, name_to_index(dev), on, 0u, 0) != 0 ||
        recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts(on ? "[ip] dhcp on failed" : "[ip] dhcp off failed");
        return 1;
    }
    puts(on ? "[ip] dhcp on ok" : "[ip] dhcp off ok");
    return 0;
}

static int dns_query(int32_t stack_ep, int32_t reply_ep, int32_t* rid, uint32_t servers[2],
                     uint32_t* out_count) {
    wasmos_ipc_message_t message;
    if (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_DNS_LIST, *rid, 0, 0, 0, 0) != 0 ||
        recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP) {
        return 1;
    }
    *out_count = message.arg1 > 2u ? 2u : message.arg1;
    servers[0] = message.arg2;
    servers[1] = message.arg3;
    return 0;
}

static int dns_apply(int32_t stack_ep, int32_t reply_ep, int32_t* rid, uint32_t count, uint32_t s0,
                     uint32_t s1) {
    wasmos_ipc_message_t message;
    return (wasmos_ipc_send(stack_ep, reply_ep, NET_IPC_DNS_SET, *rid, count, s0, s1, 0) != 0 ||
            recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP)
               ? 1
               : 0;
}

static int cmd_dns_show(int32_t stack_ep, int32_t reply_ep, int32_t* rid) {
    uint32_t servers[2];
    uint32_t count = 0u;
    if (dns_query(stack_ep, reply_ep, rid, servers, &count) != 0) {
        puts("[ip] dns show failed");
        return 1;
    }
    if (count == 0u) {
        puts("[ip] dns: (none)");
        return 0;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        char line[32];
        int n = 0;
        app_str(line, (int)sizeof(line), &n, "[ip] dns ");
        app_ipv4(line, (int)sizeof(line), &n, servers[i]);
        if (n < (int)sizeof(line))
            line[n] = '\0';
        puts(line);
    }
    return 0;
}

static int cmd_dns_set(int32_t stack_ep, int32_t reply_ep, int32_t* rid, const char* ip1,
                       const char* ip2) {
    uint32_t s0 = 0u;
    uint32_t s1 = 0u;
    uint32_t count = 0u;
    if (ip1 != 0 && parse_ipv4_no(ip1, &s0) == 0) {
        count = 1u;
    } else if (ip1 != 0) {
        puts("[ip] bad dns address");
        return 1;
    }
    if (ip2 != 0) {
        if (parse_ipv4_no(ip2, &s1) != 0) {
            puts("[ip] bad dns address");
            return 1;
        }
        count = 2u;
    }
    if (dns_apply(stack_ep, reply_ep, rid, count, s0, s1) != 0) {
        puts("[ip] dns set failed");
        return 1;
    }
    puts("[ip] dns set ok");
    return 0;
}

static int cmd_dns_del(int32_t stack_ep, int32_t reply_ep, int32_t* rid, const char* ip) {
    uint32_t target = 0u;
    uint32_t servers[2];
    uint32_t count = 0u;
    uint32_t keep[2] = {0u, 0u};
    uint32_t kept = 0u;
    uint32_t i;
    if (parse_ipv4_no(ip, &target) != 0) {
        puts("[ip] bad dns address");
        return 1;
    }
    if (dns_query(stack_ep, reply_ep, rid, servers, &count) != 0) {
        puts("[ip] dns del failed");
        return 1;
    }
    for (i = 0u; i < count; ++i) {
        if (servers[i] != target)
            keep[kept++] = servers[i];
    }
    if (dns_apply(stack_ep, reply_ep, rid, kept, keep[0], keep[1]) != 0) {
        puts("[ip] dns del failed");
        return 1;
    }
    puts("[ip] dns del ok");
    return 0;
}

static int cmd_dev_state(int32_t stack_ep, int32_t reply_ep, int32_t* rid, const char* dev,
                         uint32_t up) {
    wasmos_ipc_message_t message;
    if (wasmos_ipc_send(
            stack_ep, reply_ep, NET_IPC_IF_SET_STATE, *rid, name_to_index(dev), up, 0u, 0) != 0 ||
        recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts(up ? "[ip] dev up failed" : "[ip] dev down failed");
        return 1;
    }
    puts(up ? "[ip] dev up ok" : "[ip] dev down ok");
    return 0;
}

static int cmd_show(int32_t stack_ep, int32_t reply_ep, int32_t* rid) {
    uint8_t records[IP_MAX_IFACES * IP_IFADDR_RECORD_BYTES];
    wasmos_ipc_message_t message;
    int32_t bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(records));
    int32_t grant;
    uint32_t count;
    if (bid < 0)
        return 1;
    grant = wasmos_xfer_buffer_borrow(
        stack_ep, bid, WASMOS_BUFFER_GRANT_READ | WASMOS_BUFFER_GRANT_WRITE);
    if (grant < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    if (wasmos_ipc_send(stack_ep,
                        reply_ep,
                        NET_IPC_IFADDR_LIST,
                        *rid,
                        bid,
                        grant,
                        (int32_t)sizeof(records),
                        0) != 0 ||
        recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP ||
        (int32_t)message.arg0 < 0) {
        puts("[ip] list failed");
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    count = message.arg0;
    if (count > IP_MAX_IFACES)
        count = IP_MAX_IFACES;
    if (wasmos_xfer_buffer_read(bid, addr_cast(int32_t, records), (int32_t)(count * 24u), 0) != 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const uint8_t* r = records + i * 24u;
        char line[80];
        int n = 0;
        app_str(line, (int)sizeof(line), &n, "[ip] eth");
        app_dec(line, (int)sizeof(line), &n, get_u32(r, 4u));
        app_str(line, (int)sizeof(line), &n, ": ");
        app_ipv4(line, (int)sizeof(line), &n, get_u32(r, 8u));
        app_str(line, (int)sizeof(line), &n, "/");
        app_dec(line, (int)sizeof(line), &n, mask_prefix(get_u32(r, 12u)));
        app_str(line, (int)sizeof(line), &n, " gw ");
        app_ipv4(line, (int)sizeof(line), &n, get_u32(r, 16u));
        app_str(line,
                (int)sizeof(line),
                &n,
                (get_u32(r, 20u) & NET_IFADDR_FLAG_ADMIN_UP) ? " state up" : " state down");
        app_str(line,
                (int)sizeof(line),
                &n,
                (get_u32(r, 20u) & NET_IFADDR_FLAG_LINK_UP) ? " link up" : " link down");
        if (get_u32(r, 20u) & NET_IFADDR_FLAG_DHCP)
            app_str(line, (int)sizeof(line), &n, " dhcp");
        if (n < (int)sizeof(line))
            line[n] = '\0';
        else
            line[sizeof(line) - 1] = '\0';
        puts(line);
    }
    (void)wasmos_xfer_buffer_release(bid);
    return 0;
}

static int cmd_add(int32_t stack_ep, int32_t reply_ep, int32_t* rid, const char* cidr,
                   const char* dev) {
    uint8_t record[24] = {0};
    char ipbuf[24];
    uint32_t addr_no;
    uint32_t prefix = 24u;
    uint32_t i = 0u;
    uint32_t slash = 0u;
    wasmos_ipc_message_t message;
    int32_t bid;
    int32_t grant;
    /* Split "a.b.c.d/prefix". */
    while (cidr[slash] != '\0' && cidr[slash] != '/')
        slash++;
    if (slash >= sizeof(ipbuf)) {
        usage();
        return 1;
    }
    for (i = 0u; i < slash; ++i)
        ipbuf[i] = cidr[i];
    ipbuf[slash] = '\0';
    if (parse_ipv4_no(ipbuf, &addr_no) != 0) {
        puts("[ip] bad address");
        return 1;
    }
    if (cidr[slash] == '/')
        (void)parse_dec(cidr + slash + 1u, &prefix);
    put_u32(record, 0u, NET_IFADDR_RECORD_VERSION);
    put_u32(record, 4u, name_to_index(dev));
    put_u32(record, 8u, addr_no);
    put_u32(record, 12u, prefix_to_mask_no(prefix));
    /* Default gateway to the network's .1 host for convenience. */
    put_u32(record, 16u, (addr_no & prefix_to_mask_no(prefix)) | (1u << 24u));
    bid = wasmos_xfer_buffer_acquire((int32_t)sizeof(record));
    if (bid < 0)
        return 1;
    if (wasmos_xfer_buffer_write(bid, addr_cast(int32_t, record), (int32_t)sizeof(record), 0) !=
        0) {
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    grant = wasmos_xfer_buffer_borrow(stack_ep, bid, WASMOS_BUFFER_GRANT_READ);
    if (grant < 0) {
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    if (wasmos_ipc_send(
            stack_ep, reply_ep, NET_IPC_IFADDR_ADD, *rid, bid, grant, (int32_t)sizeof(record), 0) !=
            0 ||
        recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts("[ip] addr add failed");
        (void)wasmos_xfer_buffer_release(bid);
        return 1;
    }
    (void)wasmos_xfer_buffer_release(bid);
    puts("[ip] addr add ok");
    return 0;
}

static int cmd_del(int32_t stack_ep, int32_t reply_ep, int32_t* rid, const char* dev) {
    wasmos_ipc_message_t message;
    if (wasmos_ipc_send(
            stack_ep, reply_ep, NET_IPC_IFADDR_DEL, *rid, name_to_index(dev), 0u, 0u, 0) != 0 ||
        recv_reply(reply_ep, (*rid)++, &message) != 0 || message.type != NET_IPC_RESP) {
        puts("[ip] addr del failed");
        return 1;
    }
    puts("[ip] addr del ok");
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    char args[128];
    char* tok[8];
    int n;
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t rid = 1;
    int32_t stack_ep = -1;

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    n = tokenize(args, tok, 8);
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[ip] setup failed");
        return 1;
    }
    if (n < 1 || (!str_eq(tok[0], "addr") && !str_eq(tok[0], "dev") && !str_eq(tok[0], "dhcp") &&
                  !str_eq(tok[0], "dns"))) {
        usage();
        return 1;
    }
    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", rid++);
        if (stack_ep < 0)
            (void)wasmos_sched_yield();
    }
    if (stack_ep < 0) {
        puts("[ip] no net.stack");
        return 1;
    }

    if (str_eq(tok[0], "dev")) {
        if (n >= 3 && str_eq(tok[2], "up"))
            return cmd_dev_state(stack_ep, reply_ep, &rid, tok[1], 1u);
        if (n >= 3 && str_eq(tok[2], "down"))
            return cmd_dev_state(stack_ep, reply_ep, &rid, tok[1], 0u);
        usage();
        return 1;
    }
    if (str_eq(tok[0], "dhcp")) {
        if (n >= 3 && str_eq(tok[2], "on"))
            return cmd_dhcp(stack_ep, reply_ep, &rid, tok[1], 1u);
        if (n >= 3 && str_eq(tok[2], "off"))
            return cmd_dhcp(stack_ep, reply_ep, &rid, tok[1], 0u);
        usage();
        return 1;
    }
    if (str_eq(tok[0], "dns")) {
        if (n == 1 || str_eq(tok[1], "show") || str_eq(tok[1], "list"))
            return cmd_dns_show(stack_ep, reply_ep, &rid);
        if (n >= 3 && str_eq(tok[1], "set"))
            return cmd_dns_set(stack_ep, reply_ep, &rid, tok[2], n >= 4 ? tok[3] : 0);
        if (n >= 3 && str_eq(tok[1], "del"))
            return cmd_dns_del(stack_ep, reply_ep, &rid, tok[2]);
        usage();
        return 1;
    }
    if (n >= 5 && str_eq(tok[1], "add") && str_eq(tok[3], "dev"))
        return cmd_add(stack_ep, reply_ep, &rid, tok[2], tok[4]);
    if (n >= 4 && str_eq(tok[1], "del") && str_eq(tok[2], "dev"))
        return cmd_del(stack_ep, reply_ep, &rid, tok[3]);
    if (n == 1 || str_eq(tok[1], "show") || str_eq(tok[1], "list"))
        return cmd_show(stack_ep, reply_ep, &rid);
    usage();
    return 1;
}
