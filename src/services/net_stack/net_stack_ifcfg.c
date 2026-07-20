/* net_stack_ifcfg.c - see net_stack_ifcfg.h. Dependency-free by design. */
#include <stddef.h>

#include "net_stack_ifcfg.h"

typedef struct {
    const char* p;
    uint32_t n;
} ifcfg_token_t;

static int ifcfg_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int ifcfg_tok_eq(ifcfg_token_t t, const char* lit) {
    uint32_t i = 0u;
    for (; i < t.n; ++i) {
        if (lit[i] == '\0' || t.p[i] != lit[i]) {
            return 0;
        }
    }
    return lit[i] == '\0';
}

/* Split a line [p, p+n) into up to `max` whitespace-delimited tokens. Returns
 * the token count. */
static uint32_t ifcfg_tokenize(const char* p, uint32_t n, ifcfg_token_t* out, uint32_t max) {
    uint32_t count = 0u;
    uint32_t i = 0u;
    while (i < n && count < max) {
        while (i < n && ifcfg_is_space(p[i])) {
            i++;
        }
        if (i >= n) {
            break;
        }
        uint32_t start = i;
        while (i < n && !ifcfg_is_space(p[i])) {
            i++;
        }
        out[count].p = p + start;
        out[count].n = i - start;
        count++;
    }
    return count;
}

/* Parse a decimal number from [p, p+n). Returns 0 on success. */
static int ifcfg_parse_u32(const char* p, uint32_t n, uint32_t* out) {
    uint32_t value = 0u;
    uint32_t i = 0u;
    if (n == 0u) {
        return -1;
    }
    for (; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') {
            return -1;
        }
        value = value * 10u + (uint32_t)(p[i] - '0');
    }
    *out = value;
    return 0;
}

/* Parse dotted-quad "a.b.c.d" from a token into octets. Returns 0 on success. */
static int ifcfg_parse_ipv4(const char* p, uint32_t n, uint8_t out[4]) {
    uint32_t field = 0u;
    uint32_t start = 0u;
    uint32_t i = 0u;
    for (i = 0u; i <= n; ++i) {
        if (i == n || p[i] == '.') {
            uint32_t v = 0u;
            if (field >= 4u || ifcfg_parse_u32(p + start, i - start, &v) != 0 || v > 255u) {
                return -1;
            }
            out[field] = (uint8_t)v;
            field++;
            start = i + 1u;
        }
    }
    return field == 4u ? 0 : -1;
}

static void ifcfg_prefix_to_mask(uint32_t prefix, uint8_t mask[4]) {
    uint32_t bit = 0u;
    if (prefix > 32u) {
        prefix = 32u;
    }
    mask[0] = mask[1] = mask[2] = mask[3] = 0u;
    for (bit = 0u; bit < prefix; ++bit) {
        mask[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
    }
}

static void ifcfg_copy_name(char* dst, uint32_t cap, const char* p, uint32_t n) {
    uint32_t i = 0u;
    if (cap == 0u) {
        return;
    }
    for (; i < n && i + 1u < cap; ++i) {
        dst[i] = p[i];
    }
    dst[i] = '\0';
}

int net_ifcfg_parse(const char* text, uint32_t len, net_ifcfg_t* out) {
    uint32_t pos = 0u;
    int in_static = 0;
    int have_addr = 0;
    int have_mask = 0;

    if (text == NULL || out == NULL) {
        return 0;
    }
    for (uint32_t i = 0u; i < sizeof(*out); ++i) {
        ((uint8_t*)out)[i] = 0u;
    }
    /* Sensible static defaults; overridden by parsed fields. */
    out->mask[0] = 255u;
    out->mask[1] = 255u;
    out->mask[2] = 255u;
    out->mask[3] = 0u;

    while (pos < len) {
        uint32_t line_start = pos;
        uint32_t line_end;
        ifcfg_token_t tok[4];
        uint32_t ntok;
        while (pos < len && text[pos] != '\n') {
            pos++;
        }
        line_end = pos;
        if (pos < len) {
            pos++; /* consume '\n' */
        }
        ntok = ifcfg_tokenize(text + line_start, line_end - line_start, tok, 4u);
        if (ntok == 0u || tok[0].p[0] == '#') {
            continue; /* blank or comment */
        }

        if (ifcfg_tok_eq(tok[0], "iface")) {
            if (in_static || out->valid) {
                break; /* only the first stanza is honored */
            }
            /* iface <name> inet <dhcp|static> */
            if (ntok < 4u || !ifcfg_tok_eq(tok[2], "inet")) {
                continue;
            }
            ifcfg_copy_name(out->name, sizeof(out->name), tok[1].p, tok[1].n);
            if (ifcfg_tok_eq(tok[3], "dhcp")) {
                out->dhcp = 1u;
                out->valid = 1u;
                break;
            }
            if (ifcfg_tok_eq(tok[3], "static")) {
                in_static = 1;
            }
            continue;
        }

        if (!in_static) {
            continue;
        }
        if (ifcfg_tok_eq(tok[0], "address") && ntok >= 2u) {
            /* Accept "a.b.c.d" or "a.b.c.d/prefix". */
            uint32_t slash = 0u;
            while (slash < tok[1].n && tok[1].p[slash] != '/') {
                slash++;
            }
            if (ifcfg_parse_ipv4(tok[1].p, slash, out->addr) == 0) {
                have_addr = 1;
            }
            if (slash < tok[1].n && !have_mask) {
                uint32_t prefix = 0u;
                if (ifcfg_parse_u32(tok[1].p + slash + 1u, tok[1].n - slash - 1u, &prefix) == 0) {
                    ifcfg_prefix_to_mask(prefix, out->mask);
                }
            }
        } else if (ifcfg_tok_eq(tok[0], "netmask") && ntok >= 2u) {
            uint8_t m[4];
            if (ifcfg_parse_ipv4(tok[1].p, tok[1].n, m) == 0) {
                out->mask[0] = m[0];
                out->mask[1] = m[1];
                out->mask[2] = m[2];
                out->mask[3] = m[3];
                have_mask = 1;
            } else {
                uint32_t prefix = 0u;
                if (ifcfg_parse_u32(tok[1].p, tok[1].n, &prefix) == 0) {
                    ifcfg_prefix_to_mask(prefix, out->mask);
                    have_mask = 1;
                }
            }
        } else if (ifcfg_tok_eq(tok[0], "gateway") && ntok >= 2u) {
            (void)ifcfg_parse_ipv4(tok[1].p, tok[1].n, out->gw);
        }
    }

    if (out->valid) {
        return 1; /* dhcp stanza */
    }
    if (in_static && have_addr) {
        out->valid = 1u;
        return 1;
    }
    return 0;
}
