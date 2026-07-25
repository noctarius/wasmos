/* curl - minimal HTTP/1.0 GET over the net-stack TCP ring helper.
 *
 * Usage: curl <host>[:port][/path] [-o <file>]
 *
 * Resolves the host through net.stack DNS, opens a TCP stream socket (zero-copy
 * rings via wasmos_net_tcp_*), sends an HTTP/1.0 GET, strips the response
 * headers, and writes the body to stdout or, with -o, to a file.
 */
#include <stdint.h>

#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "wasmos/api.h"
#include "wasmos/ipc.h"
#include "wasmos/net.h"
#include "wasmos/startup.h"

#define RING_CAP 16384u
#define RECV_BUF 2048

/* Parse a dotted-decimal IPv4 literal into a network-order word (octet a in the
 * low byte, the form wasmos_net_resolve yields). Returns 1 on success, 0 if
 * `s` is not a bare IPv4 literal (caller then falls back to DNS). */
static int parse_ipv4_literal(const char* s, uint32_t* out) {
    uint32_t word = 0u;
    int i = 0;
    for (int octet = 0; octet < 4; ++octet) {
        uint32_t v = 0u;
        int digits = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            v = v * 10u + (uint32_t)(s[i++] - '0');
            digits++;
        }
        if (digits == 0 || v > 255u) {
            return 0;
        }
        word |= v << (octet * 8);
        if (octet < 3) {
            if (s[i] != '.') {
                return 0;
            }
            i++;
        }
    }
    if (s[i] != '\0') {
        return 0;
    }
    *out = word;
    return 1;
}

/* Append the NUL-terminated `src` to dst[*len], bounded by cap. */
static void sappend(char* dst, int cap, int* len, const char* src) {
    for (int i = 0; src[i] != '\0' && *len < cap - 1; ++i) {
        dst[(*len)++] = src[i];
    }
    dst[*len] = '\0';
}

/* Split "arg" (a whitespace-run of the command line) into up to `max` tokens in
 * place (NUL-terminating each); returns the token count. */
static int tokenize(char* arg, char** out, int max) {
    int n = 0;
    int i = 0;
    while (arg[i] != '\0' && n < max) {
        while (arg[i] == ' ' || arg[i] == '\t') {
            arg[i++] = '\0';
        }
        if (arg[i] == '\0') {
            break;
        }
        out[n++] = &arg[i];
        while (arg[i] != '\0' && arg[i] != ' ' && arg[i] != '\t') {
            i++;
        }
    }
    return n;
}

int main(void) {
    char args[256];
    char* tok[8];
    const char* url = 0;
    const char* outfile = 0;
    char host[128];
    char path[160];
    uint32_t port = 80u;
    int32_t proc_ep = wasmos_startup_arg(0);
    int32_t reply_ep = wasmos_ipc_create_endpoint();
    int32_t rid = 1;
    int32_t stack_ep = -1;
    uint32_t addr = 0u;
    wasmos_net_tcp_t sock;
    char req[512];
    int rlen = 0;
    uint8_t buf[RECV_BUF];
    int fd = 1; /* stdout */
    int headers_done = 0;
    int match = 0;
    uint32_t body_len = 0u;
    static const char marker[4] = {'\r', '\n', '\r', '\n'};

    args[0] = '\0';
    (void)wasmos_startup_args(args, sizeof(args));
    int ntok = tokenize(args, tok, 8);
    for (int i = 0; i < ntok; ++i) {
        if (strcmp(tok[i], "-o") == 0 && i + 1 < ntok) {
            outfile = tok[++i];
        } else if (url == 0) {
            url = tok[i];
        }
    }
    if (proc_ep <= 0 || reply_ep < 0) {
        puts("[curl] setup failed");
        return 1;
    }
    if (url == 0) {
        puts("[curl] usage: curl <host>[:port][/path] [-o <file>]");
        return 1;
    }

    /* Parse url -> host, port, path (strip an optional http:// prefix). */
    if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    }
    {
        int hi = 0;
        int ui = 0;
        while (url[ui] != '\0' && url[ui] != '/' && url[ui] != ':' && hi < (int)sizeof(host) - 1) {
            host[hi++] = url[ui++];
        }
        host[hi] = '\0';
        if (url[ui] == ':') {
            uint32_t p = 0u;
            ui++;
            while (url[ui] >= '0' && url[ui] <= '9') {
                p = p * 10u + (uint32_t)(url[ui++] - '0');
            }
            if (p > 0u && p < 65536u) {
                port = p;
            }
            while (url[ui] != '\0' && url[ui] != '/') {
                ui++;
            }
        }
        if (url[ui] == '/') {
            int pi = 0;
            while (url[ui] != '\0' && pi < (int)sizeof(path) - 1) {
                path[pi++] = url[ui++];
            }
            path[pi] = '\0';
        } else {
            path[0] = '/';
            path[1] = '\0';
        }
    }
    if (host[0] == '\0') {
        puts("[curl] bad url");
        return 1;
    }

    for (int spin = 0; spin < 4096 && stack_ep < 0; ++spin) {
        stack_ep = wasmos_svc_lookup(proc_ep, reply_ep, "net.stack", rid++);
        if (stack_ep < 0) {
            (void)wasmos_sched_yield();
        }
    }
    if (stack_ep < 0) {
        puts("[curl] no net.stack");
        return 1;
    }
    if (!parse_ipv4_literal(host, &addr) &&
        wasmos_net_resolve(stack_ep, reply_ep, host, rid++, &addr) != 0) {
        puts("[curl] resolve failed");
        return 1;
    }
    if (wasmos_net_tcp_connect(&sock, stack_ep, reply_ep, addr, (uint16_t)port, RING_CAP, rid) != 0) {
        puts("[curl] connect failed");
        return 1;
    }

    sappend(req, (int)sizeof(req), &rlen, "GET ");
    sappend(req, (int)sizeof(req), &rlen, path);
    sappend(req, (int)sizeof(req), &rlen, " HTTP/1.0\r\nHost: ");
    sappend(req, (int)sizeof(req), &rlen, host);
    sappend(req, (int)sizeof(req), &rlen, "\r\nConnection: close\r\n\r\n");
    if (wasmos_net_tcp_send(&sock, req, rlen) != rlen) {
        puts("[curl] send failed");
        wasmos_net_tcp_close(&sock);
        return 1;
    }

    if (outfile != 0) {
        fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            puts("[curl] open output failed");
            wasmos_net_tcp_close(&sock);
            return 1;
        }
    }

    for (;;) {
        int32_t n = wasmos_net_tcp_recv(&sock, buf, (int32_t)sizeof(buf));
        int32_t start = 0;
        if (n <= 0) {
            break;
        }
        if (!headers_done) {
            int32_t i = 0;
            while (i < n) {
                char c = (char)buf[i];
                match = (c == marker[match]) ? (match + 1) : ((c == '\r') ? 1 : 0);
                i++;
                if (match == 4) {
                    headers_done = 1;
                    start = i;
                    break;
                }
            }
            if (!headers_done) {
                continue; /* still inside the header block */
            }
        }
        if (n - start > 0) {
            (void)write(fd, buf + start, (size_t)(n - start));
            body_len += (uint32_t)(n - start);
        }
    }

    wasmos_net_tcp_close(&sock);
    if (outfile != 0) {
        (void)close(fd);
        char line[192];
        int ll = 0;
        char num[12];
        int t = 0;
        uint32_t v = body_len;
        sappend(line, (int)sizeof(line), &ll, "[curl] wrote ");
        if (v == 0u) {
            num[t++] = '0';
        }
        while (v > 0u && t < (int)sizeof(num)) {
            num[t++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (t > 0 && ll < (int)sizeof(line) - 1) {
            line[ll++] = num[--t];
        }
        line[ll] = '\0';
        sappend(line, (int)sizeof(line), &ll, " bytes to ");
        sappend(line, (int)sizeof(line), &ll, outfile);
        puts(line);
    }
    return 0;
}
