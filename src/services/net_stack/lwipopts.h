/* lwipopts.h - lwIP configuration for the wasmos native ring-0 net-stack.
 *
 * Native NO_SYS service configuration for the virtio.net-backed Ethernet
 * netif. Socket payload rings and TCP callback delivery remain deferred.
 *
 * Single-threaded NO_SYS build using the lwIP raw API. All timing/tick
 * hooks live in src/services/net_stack/port.c; compiler abstraction lives
 * in src/services/net_stack/arch/cc.h.
 */
#ifndef WASMOS_NET_STACK_LWIPOPTS_H
#define WASMOS_NET_STACK_LWIPOPTS_H

/* --- Threading / OS model ------------------------------------------------ */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0 /* NO_SYS single-thread: no protection */
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0 /* raw API only */

/* --- Memory model -------------------------------------------------------- */
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE (64 * 1024)

/* --- Pools / PCB counts -------------------------------------------------- */
#define PBUF_POOL_SIZE 128
#define MEMP_NUM_UDP_PCB 16
#define MEMP_NUM_TCP_PCB 16
#define MEMP_NUM_TCP_SEG 64

/* --- TCP tuning ---------------------------------------------------------- */
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)

/* --- Protocols ----------------------------------------------------------- */
#define LWIP_ARP 1
#define LWIP_IPV4 1
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_IPV6 0
#define LWIP_DHCP 1

/* DNS resolver. The DHCP client requests DNS servers (option 6) and installs
 * them via dns_setserver; ifcfg `dns-nameservers` and `ip dns` can override at
 * runtime. Resolution is exposed to clients as the async NET_IPC_RESOLVE op. */
/* Application-layer TCP abstraction: net-stack drives sockets through altcp so a
 * plain TCP connection and a TLS connection share one code path. Plaintext today
 * (altcp_tcp); altcp_tls is layered on later. */
#define LWIP_ALTCP 1

/* TLS via mbedTLS behind the altcp API (milestone B). A TLS stream socket is
 * created with altcp_tls_new() instead of altcp_tcp_new; every other tcp_/altcp_
 * call in the socket path is unchanged. */
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1
/* Milestone B is NO-verify: skip certificate chain / hostname validation so an
 * encrypted handshake + GET works against any server (verification is milestone
 * C). altcp_tls_mbedtls.c passes this to mbedtls_ssl_conf_authmode(); the token
 * resolves at its use site, after mbedtls/ssl.h is included. */
#define ALTCP_MBEDTLS_AUTHMODE MBEDTLS_SSL_VERIFY_NONE

#define LWIP_DNS 1
/* Static local name resolution, consulted before any network query. Maps
 * "localhost" to the IPv4 loopback so name lookups have a reliable, offline
 * baseline (and a hermetic resolver path independent of the SLIRP resolver). */
#define DNS_LOCAL_HOSTLIST 1
#define DNS_LOCAL_HOSTLIST_INIT \
    {DNS_LOCAL_HOSTLIST_ELEM("localhost", IPADDR4_INIT_BYTES(127, 0, 0, 1))}

/* --- Callbacks ----------------------------------------------------------- */
#define LWIP_NETIF_STATUS_CALLBACK 1

/* --- Freestanding libc surface ------------------------------------------ */
/* Use lwIP's self-contained ctype range checks instead of <ctype.h>. The
 * kernel's freestanding <ctype.h> does not provide the full set (islower,
 * isupper, ...), so avoid depending on it entirely. */
#define LWIP_NO_CTYPE_H 1

/* --- Randomness ---------------------------------------------------------- */
/* lwIP wants a PRNG for e.g. TCP ISN / ephemeral ports. Provided by port.c.
 * Declared with a fixed-width type here because arch/cc.h (which defines the
 * lwIP u32_t typedef) is pulled in after lwipopts.h by lwip/opt.h. */
#include <stdint.h>
uint32_t lwip_port_rand(void);
void lwip_port_seed(uint32_t seed);
#define LWIP_RAND() (lwip_port_rand())

#endif /* WASMOS_NET_STACK_LWIPOPTS_H */
