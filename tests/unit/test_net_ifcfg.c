#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_stack_ifcfg.h"

static void test_dhcp_stanza(void) {
    net_ifcfg_t cfg;
    const char* text = "# comment\n"
                       "auto eth0\n"
                       "iface eth0 inet dhcp\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.valid == 1u);
    assert(cfg.dhcp == 1u);
    assert(strcmp(cfg.name, "eth0") == 0);
}

static void test_static_dotted(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "    address 10.0.2.15\n"
                       "    netmask 255.255.255.0\n"
                       "    gateway 10.0.2.2\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.valid == 1u);
    assert(cfg.dhcp == 0u);
    assert(cfg.addr[0] == 10u && cfg.addr[1] == 0u && cfg.addr[2] == 2u && cfg.addr[3] == 15u);
    assert(cfg.mask[0] == 255u && cfg.mask[1] == 255u && cfg.mask[2] == 255u && cfg.mask[3] == 0u);
    assert(cfg.gw[0] == 10u && cfg.gw[1] == 0u && cfg.gw[2] == 2u && cfg.gw[3] == 2u);
}

static void test_static_prefix_form(void) {
    net_ifcfg_t cfg;
    /* address carries the prefix; no explicit netmask line. */
    const char* text = "iface en0 inet static\n"
                       "  address 192.168.1.50/24\n"
                       "  gateway 192.168.1.1\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.dhcp == 0u);
    assert(cfg.addr[0] == 192u && cfg.addr[3] == 50u);
    assert(cfg.mask[0] == 255u && cfg.mask[1] == 255u && cfg.mask[2] == 255u && cfg.mask[3] == 0u);
    assert(cfg.gw[0] == 192u && cfg.gw[3] == 1u);
}

static void test_prefix_30(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "address 10.1.2.3/30\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    /* /30 -> 255.255.255.252 */
    assert(cfg.mask[0] == 255u && cfg.mask[1] == 255u && cfg.mask[2] == 255u &&
           cfg.mask[3] == 252u);
}

static void test_first_stanza_wins(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "address 10.0.0.1\n"
                       "iface eth1 inet dhcp\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.dhcp == 0u); /* first stanza (static) wins, not the later dhcp */
    assert(cfg.addr[0] == 10u && cfg.addr[3] == 1u);
    assert(strcmp(cfg.name, "eth0") == 0);
}

static void test_static_missing_address_is_invalid(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "gateway 10.0.2.2\n";
    /* No address -> not usable. */
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 0);
    assert(cfg.valid == 0u);
}

static void test_empty_and_comments_only(void) {
    net_ifcfg_t cfg;
    const char* text = "# only comments\n\n   \n# iface eth0 inet dhcp (commented)\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 0);
}

static void test_rejects_bad_octet(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "address 10.0.2.999\n";
    /* 999 > 255 -> address rejected -> stanza invalid. */
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 0);
}

static void test_no_trailing_newline(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet dhcp"; /* no '\n' */
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.dhcp == 1u);
}

static void test_no_dns_defaults_empty(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "address 10.0.2.15\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.dns_count == 0u);
}

static void test_static_with_dns(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet static\n"
                       "    address 10.0.2.15\n"
                       "    gateway 10.0.2.2\n"
                       "    dns-nameservers 10.0.2.3 8.8.8.8\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.valid == 1u && cfg.dhcp == 0u);
    assert(cfg.dns_count == 2u);
    assert(cfg.dns[0][0] == 10u && cfg.dns[0][1] == 0u && cfg.dns[0][2] == 2u &&
           cfg.dns[0][3] == 3u);
    assert(cfg.dns[1][0] == 8u && cfg.dns[1][1] == 8u && cfg.dns[1][2] == 8u &&
           cfg.dns[1][3] == 8u);
}

static void test_dhcp_with_dns_override(void) {
    net_ifcfg_t cfg;
    /* DHCP for addressing, but an explicit resolver overrides the leased one. */
    const char* text = "iface eth0 inet dhcp\n"
                       "    dns-nameserver 1.1.1.1\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    assert(cfg.valid == 1u && cfg.dhcp == 1u);
    assert(cfg.dns_count == 1u);
    assert(cfg.dns[0][0] == 1u && cfg.dns[0][3] == 1u);
}

static void test_dns_caps_at_max(void) {
    net_ifcfg_t cfg;
    const char* text = "iface eth0 inet dhcp\n"
                       "dns-nameservers 1.1.1.1 2.2.2.2 3.3.3.3\n";
    assert(net_ifcfg_parse(text, (uint32_t)strlen(text), &cfg) == 1);
    /* Third server beyond NET_IFCFG_MAX_DNS is dropped. */
    assert(cfg.dns_count == NET_IFCFG_MAX_DNS);
}

int main(void) {
    test_dhcp_stanza();
    test_static_dotted();
    test_static_prefix_form();
    test_prefix_30();
    test_first_stanza_wins();
    test_static_missing_address_is_invalid();
    test_empty_and_comments_only();
    test_rejects_bad_octet();
    test_no_trailing_newline();
    test_no_dns_defaults_empty();
    test_static_with_dns();
    test_dhcp_with_dns_override();
    test_dns_caps_at_max();
    printf("test_net_ifcfg: ok\n");
    return 0;
}
