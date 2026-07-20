/* net_stack_ifcfg.h - parser for the /boot/system/net/interfaces config file.
 *
 * Minimal Debian /etc/network/interfaces subset: a single `iface <name> inet
 * <dhcp|static>` stanza, plus indented `address`/`netmask`/`gateway` lines for
 * the static case. Pure (no lwIP / no libsys dependency) so it can be unit
 * tested on the host.
 */
#ifndef NET_STACK_IFCFG_H
#define NET_STACK_IFCFG_H

#include <stdint.h>

typedef struct {
    uint8_t valid; /* 1 if a usable `iface ... inet` stanza was parsed */
    uint8_t dhcp;  /* 1 = dhcp, 0 = static */
    uint8_t addr[4];
    uint8_t mask[4];
    uint8_t gw[4];
    char name[16];
} net_ifcfg_t;

/* Parse the first `iface <name> inet <dhcp|static>` stanza from `text` (of
 * `len` bytes; NUL-termination not required). Returns 1 and fills *out on
 * success, 0 otherwise. For a static stanza, `address`/`netmask`/`gateway`
 * lines up to the next `iface` or EOF fill out->addr/mask/gw; an `address
 * a.b.c.d/prefix` form also derives the mask when no `netmask` line is given
 * (default mask /24, default gateway 0.0.0.0). */
int net_ifcfg_parse(const char* text, uint32_t len, net_ifcfg_t* out);

#endif /* NET_STACK_IFCFG_H */
