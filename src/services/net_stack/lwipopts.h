/* lwipopts.h - lwIP configuration for the wasmos native ring-0 net-stack.
 *
 * Native NO_SYS service configuration for the virtio.net-backed Ethernet netif.
 *
 * Single-threaded NO_SYS build using the lwIP raw API. All timing/tick
 * hooks live in src/services/net_stack/port.c; compiler abstraction lives
 * in src/services/net_stack/arch/cc.h.
 */
#ifndef WASMOS_NET_STACK_LWIPOPTS_H
#define WASMOS_NET_STACK_LWIPOPTS_H

/* --- Threading / OS model ------------------------------------------------
 * All four deviate from upstream's defaults, and together they define the port:
 * NO_SYS=1 removes the OS abstraction layer (no threads, no mboxes, no
 * semaphores), which in turn makes the sequential APIs unavailable, so
 * LWIP_NETCONN/LWIP_SOCKET are off and the service drives the raw/callback API
 * directly.  With a single thread there is nothing to protect against, so
 * SYS_LIGHTWEIGHT_PROT is off as well.  The reactor calls sys_check_timeouts()
 * itself; see port.c. */
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0 /* NO_SYS single-thread: no protection */
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0 /* raw API only */

/* --- Memory model --------------------------------------------------------
 * lwIP keeps its own fixed heap and pools rather than borrowing the service's
 * allocator: MEM_LIBC_MALLOC/MEMP_MEM_MALLOC stay at the upstream default of 0,
 * so lwIP never calls malloc and its footprint is bounded at link time.
 * MEM_SIZE is raised far above the 1600-byte upstream default because that heap
 * has to hold outbound TCP/TLS payload copies as well as every non-pool
 * allocation; note that the mbedTLS heap is deliberately NOT in here (see
 * net_stack_mbedtls_config.h, which binds mbedtls_calloc to the growable native
 * slab precisely to keep it out of this fixed 64 KiB). */
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE (64 * 1024)

/* --- Pools / PCB counts --------------------------------------------------
 * All four are raised over upstream (16 / 4 / 5 / 16).  The pcb counts bound how
 * many sockets can be live at once and are sized against NET_SOCKET_MAX (32 in
 * socket.h); the pbuf pool and segment count are what absorb an inbound burst
 * from virtio-net between reactor turns — running out shows up as dropped
 * frames rather than an error. */
#define PBUF_POOL_SIZE 128
#define MEMP_NUM_UDP_PCB 16
#define MEMP_NUM_TCP_PCB 16
#define MEMP_NUM_TCP_SEG 64

/* --- TCP tuning ----------------------------------------------------------
 * TCP_WND is upstream's default (4 * TCP_MSS); TCP_SND_BUF doubles upstream's
 * (2 * TCP_MSS) so a socket can keep more unacknowledged data in flight —
 * altcp_sndbuf is what the socket TX path uses for backpressure, and a
 * one-segment send buffer would stall the ring drain after every segment.
 * TCP_MSS itself is left at upstream's 536. */
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)

/* --- Protocols -----------------------------------------------------------
 * IPv4 only: LWIP_IPV6 stays at upstream's 0 and nothing in the service builds
 * an IPv6 address.  LWIP_DHCP is the deviation (upstream default 0) — the boot
 * interface config may ask for DHCP (see net_stack_ifcfg.h). */
#define LWIP_ARP 1
#define LWIP_IPV4 1
#define LWIP_ICMP 1
#define LWIP_UDP 1
#define LWIP_TCP 1
#define LWIP_IPV6 0
#define LWIP_DHCP 1

/* Application-layer TCP abstraction: net-stack drives sockets through altcp so a
 * plain TCP connection and a TLS connection share one code path. */
#define LWIP_ALTCP 1

/* TLS via mbedTLS behind the altcp API. A TLS stream socket is created with
 * altcp_tls_new() instead of altcp_tcp_new; every other tcp_/altcp_ call in the
 * socket path is unchanged. */
#define LWIP_ALTCP_TLS 1
#define LWIP_ALTCP_TLS_MBEDTLS 1
/* The client requires a certificate chain that validates to the CA trust store
 * loaded from /boot/system/net/certificates/ca-certs.pem, and net-stack
 * additionally sets a per-connection hostname (mbedtls_ssl_set_hostname) so the
 * server certificate CN/SAN is checked. altcp_tls_mbedtls.c passes this token to
 * mbedtls_ssl_conf_authmode(); it resolves at its use site, after mbedtls/ssl.h
 * is included. REQUIRED aborts the handshake when the chain or hostname does not
 * verify, so an untrusted server certificate is rejected. */
#define ALTCP_MBEDTLS_AUTHMODE MBEDTLS_SSL_VERIFY_REQUIRED

/* DNS resolver. The DHCP client requests DNS servers (option 6) and installs
 * them via dns_setserver; ifcfg `dns-nameservers` and `ip dns` can override at
 * runtime. Resolution is exposed to clients as the async NET_IPC_RESOLVE op. */
#define LWIP_DNS 1
/* Static local name resolution, consulted before any network query. Maps
 * "localhost" to the IPv4 loopback so name lookups have a reliable, offline
 * baseline (and a hermetic resolver path independent of the SLIRP resolver). */
#define DNS_LOCAL_HOSTLIST 1
#define DNS_LOCAL_HOSTLIST_INIT                                                                    \
    {DNS_LOCAL_HOSTLIST_ELEM("localhost", IPADDR4_INIT_BYTES(127, 0, 0, 1))}

/* --- Callbacks -----------------------------------------------------------
 * Off upstream.  Enabled so net_stack.c learns when a netif gains or loses an
 * address (the DHCP lease landing is the case that matters) instead of polling
 * the netif. */
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
/* Return the next xorshift value, seeding from the tick counter on first use if
 * lwip_port_seed has not been called.  Never returns while the state is 0. */
uint32_t lwip_port_rand(void);
/* Install a new PRNG seed; bit 0 is forced set, so seed 0 is not a dead state.
 * net_stack.c calls this with hardware entropy from the `hrng` service once that
 * provider registers. */
void lwip_port_seed(uint32_t seed);
#define LWIP_RAND() (lwip_port_rand())

#endif /* WASMOS_NET_STACK_LWIPOPTS_H */
