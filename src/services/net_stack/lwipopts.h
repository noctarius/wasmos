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
