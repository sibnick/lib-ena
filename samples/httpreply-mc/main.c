/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unikraft HTTP Reply Multi-Core Benchmark Server (app-httpreply-mc).
 *
 * Micro-benchmark HTTP echo server running on native AWS ENA driver
 * and lwIP TCP/IP stack in multi-core run-to-completion mode.
 *
 * Each core executes an isolated run-to-completion loop. Each core
 * polls its assigned hardware queue, handles stack timers, accepts
 * incoming connections via SO_REUSEPORT, and transmits responses
 * on its dedicated TX queue. There are no locks, no inter-core queues,
 * and no shared state.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread.h>
#include <uk/schedcoop.h>
#include <uk/alloc.h>
#include <uk/lcpu.h>
#include <uk/lcpu/pm.h>
#include <uk/pcpuvar.h>
#include <uk/arch.h>
#include <uk/arch/x86_64.h>
#include <uk/intctlr.h>
#include <uk/print.h>
#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
#include <uk/allocbbuddy.h>

/*
 * These live in lib/ukboot. They are declared here directly
 * to avoid including uk/boot.h in the app target.
 */
unsigned int uk_percore_heap_count(void);
int uk_percore_heap_get(unsigned int i, __uptr *base, __sz *len);
#endif
#include <lwip/timeouts.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/tcp.h>
#include <lwip/memp.h>
#include "netif/uknetdev.h"
#include "lwip_percore.h"
#include "idlebackoff.h"

#if CONFIG_APPHTTPREPLYMC_NETLOG
#include <uk/console.h>
#include <uk/console/driver.h>
#include <uk/spinlock.h>
#endif

#define MC_MAX_WORKERS 2
#define MC_RECVBUF_SIZE 4096

#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#define LISTEN_PORT 80
#define BACKLOG 512
#define MAX_EVENTS 256
#define SOCK_BUF_SIZE 32768
#define MAX_TRACKED_FDS 2048

#define MC_STATS_INTERVAL 20000UL

static const char http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain; charset=utf-8\r\n"
	"Content-Length: 14\r\n"
	"Connection: keep-alive\r\n"
	"Server: Unikraft-ENA-Benchmark-MC\r\n"
	"\r\n"
	"Hello, World!\n";

static const size_t http_resp_len = sizeof(http_response) - 1;

struct worker_ctx {
	int worker_id;
	int epoll_fd;
	int listener_fd;
	int conn_count;
	char *recv_buf;
	uint32_t *resp_pending;
	unsigned long req_count;
	unsigned long byte_count;
};

static struct worker_ctx mc_workers[MC_MAX_WORKERS];
static int mc_nworkers;

#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
/* Highest core index with a bound per-core allocator (cores 1..N). */
static int mc_percore_ready;
#endif

/* One-shot alloc/free stress per core, run at worker start. */
static int mc_selftest_done[MC_MAX_WORKERS];

/*
 * Bounded alloc/free stress on this core's own allocator. Proves
 * that each core only touches its own region while all cores run
 * at the same time.
 */
static void mc_percore_alloc_selftest(unsigned int core_id)
{
	unsigned int i;
	unsigned int n = 4096;

	if (mc_selftest_done[core_id])
		return;
	mc_selftest_done[core_id] = 1;

	for (i = 0; i < n; i++) {
		size_t sz = 4096 + (i * 65536) % 262144;
		void *p = malloc(sz);

		if (!p) {
			printf("httpreply-mc: [ERR] core %u alloc self-test "
			       "failed at iteration %u (size %zu)\n",
			       (unsigned)core_id, i, sz);
			return;
		}
		memset(p, 0x5A, sz);
		free(p);
	}

	printf("httpreply-mc: core %u alloc self-test passed (%u iterations)\n",
	       (unsigned)core_id, n);
}

/*
 * Create a standalone bbuddy allocator on each per-core heap
 * partition and bind it to that core's per-CPU slot. Core i uses
 * partition i - 1. Core 0 keeps the default allocator.
 */
static void mc_percore_alloc_init(void)
{
#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
	unsigned int n = uk_percore_heap_count();
	unsigned int i;

	mc_percore_ready = 0;

	for (i = 1; i <= n && i < MC_MAX_WORKERS &&
	     i < CONFIG_UKPLAT_CPU_MAXCOUNT; i++) {
		__uptr base;
		__sz len;
		struct uk_alloc *a;

		if (uk_percore_heap_get(i - 1, &base, &len) < 0)
			break;

		a = uk_allocbbuddy_init((void *)base, len);
		if (!a)
			break;

		uk_pcpuvar_lval(i, uk_pcpuvar_percore_alloc) = a;
		mc_percore_ready = i;
		printf("httpreply-mc: core %u per-core allocator %p+%lu\n",
		       (unsigned)i, (void *)base, (unsigned long)len);
	}

	if (mc_percore_ready == 0)
		printf("httpreply-mc: [WARN] no per-core heap partitions available\n");
#endif
}

/*
 * A core may run only when its per-core allocator slot is bound.
 * Core 0 always runs on the default allocator.
 */
static int mc_core_has_percore_alloc(unsigned int core)
{
#if defined(CONFIG_LIBUKBOOT_PERCORE_HEAP) && CONFIG_LIBUKBOOT_PERCORE_HEAP
	return (core == 0) || (core <= (unsigned int)mc_percore_ready);
#else
	(void)core;
	return 1;
#endif
}

/*
 * Cross-core ARP synchronization.
 *
 * The ENA RSS hardware hashes IP traffic across RX queues, but it
 * delivers non-IP frames (including ARP requests and replies) only
 * to RX queue 0.
 *
 * Core 0 processes RX queue 0, so only core 0 resolves ARP for
 * both off-subnet gateways and in-subnet benchmark clients. A
 * secondary core never receives ARP replies. If a connection is
 * steered to a secondary core by RSS, the core creates a TCP PCB
 * and attempts to send a SYN-ACK, but its ARP entry for the client
 * stays PENDING forever.
 *
 * Core 0 publishes all resolved ARP entries (both gateway and local
 * subnet clients) in mc_arp_pub. Each secondary core checks the
 * version counter in its stack drive loop at a bounded 20 ms
 * interval. The worker loop runs at over 1 M iterations/s, but the
 * ARP tables change only at connect time. A per-iteration sync is
 * pure waste, so both cores run the sync at most every 20 ms.
 * [Ticket 06e687fe99]
 * When the version advances,
 * the core copies the resolved IP/MAC pairs into its own per-core ARP
 * table as static entries. This immediately flushes any queued SYN-ACKs,
 * allowing connections across all cores to establish.
 *
 * Additionally, if a secondary core has an unresolved PENDING ARP
 * entry, it posts the IP to mc_arp_req so core 0 can solicit an ARP
 * reply on RX queue 0.
 */
#define MC_ARP_SYNC_MAX ARP_TABLE_SIZE

struct mc_arp_pub_entry {
	ip4_addr_t ipaddr;
	struct eth_addr mac;
};

struct mc_arp_pub {
	struct mc_arp_pub_entry entries[MC_ARP_SYNC_MAX];
	volatile uint32_t count;
	volatile uint32_t version;
} __align(64);

struct mc_arp_req {
	ip4_addr_t ipaddr;
	volatile uint32_t version;
} __align(64);

static struct mc_arp_pub mc_arp_pub;
static struct mc_arp_req mc_arp_req;
static uint32_t mc_arp_seen[LWIP_CORE_MAX < MC_MAX_WORKERS ?
			      MC_MAX_WORKERS : LWIP_CORE_MAX];
static uint32_t mc_arp_req_served;

/*
 * Bounded interval for the cross-core ARP sync. The worker loop
 * runs at over 1 M iterations/s. The sync only needs to react to
 * ARP changes, which happen at connect time. 20 ms adds far less
 * delay than one TCP retransmission. [Ticket 06e687fe99]
 */
#define MC_ARP_SYNC_INTERVAL_NS ukarch_time_msec_to_nsec(20)
static __nsec mc_arp_sync_last[MC_MAX_WORKERS];

/*
 * Numeric mirror of enum etharp_state from lwIP etharp.c. The
 * enumerator is internal to lwIP and the public headers do not
 * expose it.
 */
#define MC_ARP_STATE_EMPTY       0
#define MC_ARP_STATE_PENDING      1
#define MC_ARP_STATE_STABLE      2
#define MC_ARP_STATE_RREQ_1      3
#define MC_ARP_STATE_RREQ_2      4
#define MC_ARP_STATE_STATIC      5

static void mc_arp_publish(void)
{
	struct lwip_core_state *cs;
	unsigned int i, j;
	bool changed = false;

	if (!netif_default)
		return;

	/* Service any ARP resolution requests from secondary cores */
	if (mc_arp_req.version != mc_arp_req_served) {
		mc_arp_req_served = mc_arp_req.version;
		uk_arch_rmb();
		if (!ip4_addr_isany_val(mc_arp_req.ipaddr))
			etharp_request(netif_default, &mc_arp_req.ipaddr);
	}

	cs = lwip_get_core_state();

	for (i = 0; i < ARP_TABLE_SIZE; i++) {
		struct etharp_entry *e = &cs->arp_table[i];

		if (e->state < MC_ARP_STATE_STABLE ||
		    ip4_addr_isany_val(e->ipaddr))
			continue;

		bool found = false;
		for (j = 0; j < mc_arp_pub.count; j++) {
			if (ip4_addr_cmp(&mc_arp_pub.entries[j].ipaddr, &e->ipaddr)) {
				found = true;
				if (memcmp(&mc_arp_pub.entries[j].mac, &e->ethaddr,
					   sizeof(struct eth_addr)) != 0) {
					mc_arp_pub.entries[j].mac = e->ethaddr;
					changed = true;
				}
				break;
			}
		}

		if (!found && mc_arp_pub.count < MC_ARP_SYNC_MAX) {
			mc_arp_pub.entries[mc_arp_pub.count].ipaddr = e->ipaddr;
			mc_arp_pub.entries[mc_arp_pub.count].mac = e->ethaddr;
			mc_arp_pub.count++;
			changed = true;
		}
	}

	if (changed) {
		uk_arch_wmb();
		mc_arp_pub.version++;
	}
}

static void mc_arp_adopt(unsigned int core_id, __nsec now)
{
	struct lwip_core_state *cs;
	uint32_t count, i;
	struct mc_arp_pub_entry entries[MC_ARP_SYNC_MAX];

	if (!netif_default)
		return;

	/* Check if any entry in our local ARP table is PENDING */
	static __nsec last_arp_req;

	cs = lwip_get_core_state();
	for (i = 0; i < ARP_TABLE_SIZE; i++) {
		struct etharp_entry *e = &cs->arp_table[i];

		if (e->state == MC_ARP_STATE_PENDING &&
		    !ip4_addr_isany_val(e->ipaddr)) {
			if (!ip4_addr_cmp(&mc_arp_req.ipaddr, &e->ipaddr) ||
			    (now - last_arp_req >= ukarch_time_msec_to_nsec(500))) {
				last_arp_req = now;
				mc_arp_req.ipaddr = e->ipaddr;
				uk_arch_wmb();
				mc_arp_req.version++;
			}
			break;
		}
	}

	if (mc_arp_pub.version == mc_arp_seen[core_id])
		return;
	mc_arp_seen[core_id] = mc_arp_pub.version;

	uk_arch_rmb();
	count = mc_arp_pub.count;
	if (count > MC_ARP_SYNC_MAX)
		count = MC_ARP_SYNC_MAX;

	for (i = 0; i < count; i++)
		entries[i] = mc_arp_pub.entries[i];

	for (i = 0; i < count; i++) {
		if (ip4_addr_isany_val(entries[i].ipaddr))
			continue;

		if (etharp_add_static_entry(&entries[i].ipaddr, &entries[i].mac) != ERR_OK) {
			printf("httpreply-mc: [WARN] core %u failed to adopt ARP for %s\n",
			       core_id, ip4addr_ntoa(&entries[i].ipaddr));
			continue;
		}

		printf("httpreply-mc: core %u adopted ARP %s MAC "
		       "%02x:%02x:%02x:%02x:%02x:%02x\n",
		       core_id, ip4addr_ntoa(&entries[i].ipaddr),
		       entries[i].mac.addr[0], entries[i].mac.addr[1], entries[i].mac.addr[2],
		       entries[i].mac.addr[3], entries[i].mac.addr[4], entries[i].mac.addr[5]);
	}
}

/*
 * Get the scheduler that owns vCPU idx by walking uk_sched_head,
 * the linked list of all per-LCPU schedulers. Index 0 is the BSP
 * (vCPU 0) and index 1 is vCPU 1.
 */
static struct uk_sched *mc_get_sched(int idx)
{
	struct uk_sched *s;
	int i;

	s = uk_sched_head;
	for (i = 0; i < idx; i++) {
		if (s == NULL)
			return NULL;
		s = s->next;
	}

	return s;
}

static void configure_socket_options(int fd)
{
	int opt = 1;
	int buf_size = SOCK_BUF_SIZE;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
	ioctl(fd, FIONBIO, &opt);
}

void ena_netdev_dump_queue(struct uk_netdev *dev, uint16_t qid);
unsigned long ena_netdev_rxq_pkts(struct uk_netdev *dev, uint16_t qid);
uint32_t ena_netdev_rx_posted(struct uk_netdev *dev, uint16_t qid);
void ena_netdev_aen_report(struct uk_netdev *dev, uint32_t *count,
			   uint64_t *rx_drops, uint64_t *tx_drops);
uint64_t ena_netdev_rx_refill_err(struct uk_netdev *dev, uint16_t qid);
uint64_t ena_netdev_rx_dropped(struct uk_netdev *dev, uint16_t qid);
void ena_netdev_rearm_cq_intr(struct uk_netdev *dev, uint16_t qid);
unsigned long ena_netdev_txq_pkts(struct uk_netdev *dev, uint16_t qid);
/* Armed MSI-X vector count. Zero means software polling. */
uint32_t ena_plat_msix_state(void);
uint32_t ena_plat_msix_count_get(void);

#if CONFIG_APPHTTPREPLYMC_CONSOLE_STATS
static unsigned int mc_count_memp_free(void *head)
{
	unsigned int count = 0;
	void **cur = (void **)head;

	while (cur && count < 65536) {
		count++;
		cur = (void **)*cur;
	}
	return count;
}
#endif

/*
 * Drive the local core network stack: receive packets from dedicated
 * RX queue and process local stack timers.
 */
static void drive_core_stack(int core_id)
{
	static __nsec last_hb[MC_MAX_WORKERS];
	static unsigned long poll_cnt[MC_MAX_WORKERS];
	__nsec now = ukplat_monotonic_clock();

	poll_cnt[core_id]++;
	if (now - last_hb[core_id] >= ukarch_time_sec_to_nsec(2)) {
		last_hb[core_id] = now;

		/*
		 * MSI-X safety net: while the device is armed but no MSI
		 * has ever been observed, rewrite the completion-queue
		 * unmask registers. This is functional, not diagnostic, so
		 * it runs even when console stats are off.
		 */
		if (core_id == 0) {
			struct uk_netdev *dev = uk_netdev_get(0);

			if (dev && ena_plat_msix_state() != 0 &&
			    ena_plat_msix_count_get() == 0) {
				ena_netdev_rearm_cq_intr(dev, 0);
				ena_netdev_rearm_cq_intr(dev, 1);
			}
		}

#if CONFIG_APPHTTPREPLYMC_CONSOLE_STATS
		struct lwip_core_state *cs = lwip_get_core_state();
		unsigned int active_pcbs = 0;
		struct tcp_pcb *p;
		unsigned int m_pbuf = 0, m_pcb = 0, m_seg = 0;
		struct uk_netdev *dev = uk_netdev_get(0);
		unsigned long rxpkts = 0, txpkts = 0;

		for (p = cs->tcp_active_pcbs; p != NULL; p = p->next)
			active_pcbs++;

		if (cs->memp_tabs[MEMP_PBUF_POOL])
			m_pbuf = mc_count_memp_free(cs->memp_tabs[MEMP_PBUF_POOL]);
		if (cs->memp_tabs[MEMP_TCP_PCB])
			m_pcb = mc_count_memp_free(cs->memp_tabs[MEMP_TCP_PCB]);
		if (cs->memp_tabs[MEMP_TCP_SEG])
			m_seg = mc_count_memp_free(cs->memp_tabs[MEMP_TCP_SEG]);

		if (dev) {
			rxpkts = ena_netdev_rxq_pkts(dev, (uint16_t)core_id);
			txpkts = ena_netdev_txq_pkts(dev, (uint16_t)core_id);
		}

		{
			uint32_t aen_n = 0;
			uint64_t aen_rx = 0, aen_tx = 0;

			if (dev)
				ena_netdev_aen_report(dev, &aen_n, &aen_rx, &aen_tx);
			printf("httpreply-mc: core %d heartbeat (polls=%lu, rx=%lu, tx=%lu, active=%u, rxpost=%u refill=%llu rxdrop=%llu, aen=%u rxdrop=%llu txdrop=%llu, free: pbuf=%u pcb=%u seg=%u)\n",
			       core_id, poll_cnt[core_id], rxpkts, txpkts,
			       active_pcbs,
			       dev ? ena_netdev_rx_posted(dev, (uint16_t)core_id) : 0xFFFFFFFFu,
			       dev ? (unsigned long long)ena_netdev_rx_refill_err(dev, (uint16_t)core_id) : 0ULL,
			       dev ? (unsigned long long)ena_netdev_rx_dropped(dev, (uint16_t)core_id) : 0ULL,
			       aen_n,
			       (unsigned long long)aen_rx,
			       (unsigned long long)aen_tx,
			       m_pbuf, m_pcb, m_seg);


		}

		/*
		 * pbuf pool exhaustion warning. When the free pbuf count falls
		 * below 4 the stack cannot allocate receive buffers. send()
		 * returns ENOBUFS and connections stall.
		 * [Ticket ba82aec88b]
		 */
		if (m_pbuf < 4)
			printf("httpreply-mc: [WARN] core %d pbuf pool near-empty "
			       "(free=%u) — send() will fail with ENOBUFS\n",
			       core_id, m_pbuf);

		/*
		 * Stall probe: this core has open connections but its RX
		 * counter did not move in the last interval. Dump every
		 * active TCP control block to find why they are stuck.
		 * [Ticket 1152cbcaca]
		 */
		{
			if (active_pcbs > 0) {
				for (p = cs->tcp_active_pcbs; p != NULL;
				     p = p->next) {
					unsigned int qlen;

					if (p->nrtx == 0 && p->rtime == 0)
						continue;
					qlen = p->snd_queuelen;
					printf("httpreply-mc: core %d STALL pcb %u->%u state=%d nrtx=%u rtime=%d snd.wnd=%u rcv.wnd=%u snd.qlen=%u flags=%04x\n",
					       core_id,
					       (unsigned int)p->local_port,
					       (unsigned int)p->remote_port,
					       (int)p->state,
					       (unsigned int)p->nrtx,
					       (int)p->rtime,
					       (unsigned int)p->snd_wnd,
					       (unsigned int)p->rcv_wnd,
					       qlen,
					       (unsigned int)p->flags);
				}
			}
		}
#endif
	}
	uknetdev_poll_rxqueue((uint16_t)core_id);
	sys_check_timeouts();

	/*
	 * Synchronize ARP entries across cores. Core 0 receives ARP replies
	 * from the network and publishes resolved MACs. The other cores
	 * adopt the MACs in their own ARP tables.
	 *
	 * Run the sync at most every MC_ARP_SYNC_INTERVAL_NS. The loop
	 * rate is over 1 M iterations/s; the ARP tables change only at
	 * connect time. Reuse the timestamp from the top of this function
	 * so the check costs no extra clock read. [Ticket 06e687fe99]
	 */
	if (now - mc_arp_sync_last[core_id] >= MC_ARP_SYNC_INTERVAL_NS) {
		mc_arp_sync_last[core_id] = now;
		if (core_id == 0)
			mc_arp_publish();
		else
			mc_arp_adopt((unsigned int)core_id, now);
	}
}

static int send_pending_response(struct worker_ctx *w, int fd,
				 uint32_t base_events);

#if CONFIG_APPHTTPREPLYMC_NETLOG
/*
 * Network log sink. The EC2 serial console is lossy and lags, so it
 * drops the bbuddy free-path guard and the fd-lock warnings that we
 * need to localize the c=100 fault. Register a stdout console device
 * that copies every log line into a ring, and answer GET /__log with
 * the most recent bytes. The out callback runs under the ukconsole
 * device lock, so it only does a bounded copy under our own leaf lock.
 * The /__log handler must not print, or it would feed itself.
 * [Ticket a9c6945c21]
 */
#define MC_NETLOG_SIZE   CONFIG_APPHTTPREPLYMC_NETLOG_SIZE
#define MC_NETLOG_SEND   65536

static char mc_netlog_buf[MC_NETLOG_SIZE];
static size_t mc_netlog_w;
static struct uk_spinlock mc_netlog_lock = UK_SPINLOCK_INITIALIZER();

static __ssz mc_netlog_out(struct uk_console *dev, const char *buf, __sz len)
{
	__sz i;

	(void)dev;
	uk_spin_lock(&mc_netlog_lock);
	for (i = 0; i < len; i++) {
		mc_netlog_buf[mc_netlog_w & (MC_NETLOG_SIZE - 1)] = buf[i];
		mc_netlog_w++;
	}
	uk_spin_unlock(&mc_netlog_lock);
	return len;
}

static const struct uk_console_ops mc_netlog_ops = { .out = mc_netlog_out };
static struct uk_console mc_netlog_dev;

static void mc_netlog_init(void)
{
	uk_console_init(&mc_netlog_dev, "netlog", &mc_netlog_ops,
			UK_CONSOLE_FLAG_STDOUT, UK_CONSOLE_CLASS_NONE);
	uk_console_register(&mc_netlog_dev);
}

/* Copy the most recent up-to-cap bytes of the ring into out. */
static size_t mc_netlog_snapshot(char *out, size_t cap)
{
	size_t w, have, n, s, i;

	uk_spin_lock(&mc_netlog_lock);
	w = mc_netlog_w;
	have = w < (size_t)MC_NETLOG_SIZE ? w : (size_t)MC_NETLOG_SIZE;
	n = have < cap ? have : cap;
	s = w - n;
	for (i = 0; i < n; i++)
		out[i] = mc_netlog_buf[(s + i) & (MC_NETLOG_SIZE - 1)];
	uk_spin_unlock(&mc_netlog_lock);
	return n;
}

static char mc_netlog_body[MC_NETLOG_SEND];

/* Serve the log ring on this diagnostic connection, then close it. */
static void mc_netlog_send_response(struct worker_ctx *w, int fd)
{
	char hdr[128];
	size_t blen, hlen;
	size_t off;

	blen = mc_netlog_snapshot(mc_netlog_body, sizeof(mc_netlog_body));
	hlen = (size_t)snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/plain; charset=utf-8\r\n"
		"Content-Length: %u\r\n"
		"Connection: close\r\n\r\n", (unsigned int)blen);

	for (off = 0; off < hlen; ) {
		ssize_t n = send(fd, hdr + off, hlen - off, 0);
		if (n > 0) { off += (size_t)n; continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			drive_core_stack(w->worker_id);
			continue;
		}
		return;
	}
	for (off = 0; off < blen; ) {
		ssize_t n = send(fd, mc_netlog_body + off, blen - off, 0);
		if (n > 0) { off += (size_t)n; continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			drive_core_stack(w->worker_id);
			continue;
		}
		return;
	}
}
#endif /* CONFIG_APPHTTPREPLYMC_NETLOG */

/*
 * Read one request from a connection and queue the response.
 * Returns -1 when the connection must be dropped.
 */
static int handle_readable(struct worker_ctx *w, int fd)
{
	ssize_t r = recv(fd, w->recv_buf, MC_RECVBUF_SIZE - 1, 0);

	if (r > 0) {
		w->recv_buf[r] = '\0';
#if CONFIG_APPHTTPREPLYMC_NETLOG
		if (strncmp(w->recv_buf, "GET /__log", 10) == 0) {
			mc_netlog_send_response(w, fd);
			return -1;
		}
#endif
		w->req_count++;
		w->byte_count += http_resp_len;
#if CONFIG_APPHTTPREPLYMC_CONSOLE_STATS
		if ((w->req_count % MC_STATS_INTERVAL) == 0)
			printf("httpreply-mc: [stats] core %d: req=%lu bytes=%lu\n",
			       w->worker_id,
			       (unsigned long)w->req_count,
			       (unsigned long)w->byte_count);
#endif

		if (fd < MAX_TRACKED_FDS) {
			w->resp_pending[fd] = (uint32_t)http_resp_len;
			if (send_pending_response(w, fd, EPOLLIN | EPOLLRDHUP) < 0)
				return -1;
		} else {
			send(fd, http_response, http_resp_len, 0);
		}

		if (strstr(w->recv_buf, "Connection: close") != NULL ||
		    strstr(w->recv_buf, "connection: close") != NULL)
			return -1;
		return 0;
	}

	if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
		return -1;
	return 0;
}

/*
 * Send as many bytes of pending response as the socket accepts.
 */
static int send_pending_response(struct worker_ctx *w, int fd, uint32_t base_events)
{
	uint32_t pending;
	ssize_t n;

	if (fd < 0 || fd >= MAX_TRACKED_FDS)
		return -1;

	pending = w->resp_pending[fd];

	while (pending > 0) {
		n = send(fd, http_response + (http_resp_len - pending), pending, 0);
		if (n > 0) {
			pending -= (uint32_t)n;
			continue;
		}

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			      errno == ENOBUFS || errno == EBUSY)) {
			drive_core_stack(w->worker_id);
			n = send(fd, http_response + (http_resp_len - pending), pending, 0);
			if (n > 0) {
				pending -= (uint32_t)n;
				continue;
			}
			break;
		}

		return -1;
	}

	w->resp_pending[fd] = pending;

	/*
	 * Do not toggle EPOLLOUT here. Under load the epoll poll-chain
	 * update re-enters itself and trips the pollq _tag assert on
	 * UK 0.21. The worker loop retries pending responses every
	 * iteration instead.
	 */
	(void)base_events;
	return pending > 0 ? 1 : 0;
}

/*
 * Drop a connection: clear its pending bytes, remove it from epoll,
 * and close the socket descriptor.
 */
/*
 * The POSIX fd table is shared by all cores and has no internal
 * lock. Concurrent accept (fd alloc) and close (fd free) corrupt
 * the table and the file objects behind it. Serialize every fd
 * lifecycle operation with this lock. Data-path calls (recv,
 * send, epoll_wait) stay outside the lock.
 *
 * The lock stores the owning core id (0 means free). A waiter that
 * finds the holder idle for longer than MC_FD_LOCK_STUCK_NS prints
 * one warning naming both cores. Without this, a core that faults
 * or spins inside a corrupted allocator while holding the lock
 * leaves the other core spinning with no console output, which is
 * the "hang with no banner" symptom. [Ticket a9c6945c21]
 */
static volatile uint32_t mc_fd_lock;
static volatile __nsec mc_fd_lock_acquired_at;

#define MC_FD_LOCK_STUCK_NS ukarch_time_sec_to_nsec(2)

/*
 * Per-core record of what a core is doing while it holds the fd lock, so
 * a waiter that sees a stuck holder can name the blocked operation and
 * the fd. The lock is only ever held around accept4 and close.
 * [Ticket a9c6945c21]
 */
enum mc_fd_op { MC_FD_OP_NONE = 0, MC_FD_OP_ACCEPT, MC_FD_OP_CLOSE,
		MC_FD_OP_LISTEN };
static volatile int mc_fd_op[MC_MAX_WORKERS];
static volatile __nsec mc_fd_op_at[MC_MAX_WORKERS];
static volatile int mc_fd_op_fd[MC_MAX_WORKERS];
/* Return address of the call site that last acquired the lock, per core.
 * Names which acquire site holds a stuck lock (the "unknown" holder). */
static volatile unsigned long mc_fd_lock_acq_site[MC_MAX_WORKERS];

static uint32_t mc_this_core(void)
{
	return (uint32_t)uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
}

static void mc_fd_lock_acquire(void)
{
	uint32_t want = mc_this_core() + 1;
	uint32_t expected;
	int warned = 0;

	for (;;) {
		expected = 0;
		if (__atomic_compare_exchange_n(&mc_fd_lock, &expected, want,
						false, __ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED)) {
			mc_fd_lock_acquired_at = ukplat_monotonic_clock();
			mc_fd_lock_acq_site[want - 1] =
				(unsigned long)__builtin_return_address(0);
			return;
		}

		__nsec now = ukplat_monotonic_clock();
		__nsec held = now - mc_fd_lock_acquired_at;

		if (!warned && held >= MC_FD_LOCK_STUCK_NS && expected >= 1) {
			uint32_t holder = expected - 1;
			int op = __atomic_load_n(&mc_fd_op[holder],
						 __ATOMIC_RELAXED);
			__nsec op_at = __atomic_load_n(&mc_fd_op_at[holder],
						       __ATOMIC_RELAXED);
			int ofd = mc_fd_op_fd[holder];
			unsigned long acq = __atomic_load_n(
				&mc_fd_lock_acq_site[holder],
				__ATOMIC_RELAXED);
			const char *opname =
				(op == MC_FD_OP_ACCEPT) ? "accept4" :
				(op == MC_FD_OP_CLOSE) ? "close" :
				(op == MC_FD_OP_LISTEN) ? "listen-setup" :
				"unknown";

			warned = 1;
			printf("httpreply-mc: [WARN] core %u waiting on fd lock "
			       "held by core %u for %lld ms; holder is in %s "
			       "on fd %d for %lld ms; acquired at 0x%lx\n",
			       (unsigned)(want - 1), (unsigned)holder,
			       (long long)(held / 1000000LL), opname, ofd,
			       op_at ? (long long)((now - op_at) / 1000000LL)
				     : -1LL,
			       acq);
		}
		uk_sched_yield();
	}
}

static void mc_fd_lock_release(void)
{
	mc_fd_lock_acquired_at = 0;
	__atomic_store_n(&mc_fd_lock, 0, __ATOMIC_RELEASE);
}

/*
 * Helpers that record what a core is doing while it holds the fd lock.
 * The state itself (mc_fd_op*) is declared above mc_fd_lock_acquire so
 * the waiter can read it. [Ticket a9c6945c21]
 */
static void mc_fd_mark(int core, enum mc_fd_op op, int fd)
{
	mc_fd_op_fd[core] = fd;
	__atomic_store_n(&mc_fd_op[core], (int)op, __ATOMIC_RELAXED);
	__atomic_store_n(&mc_fd_op_at[core], ukplat_monotonic_clock(),
			 __ATOMIC_RELAXED);
}

static void mc_fd_unmark(int core)
{
	__atomic_store_n(&mc_fd_op[core], MC_FD_OP_NONE, __ATOMIC_RELAXED);
	mc_fd_op_fd[core] = -1;
}

static void drop_connection(struct worker_ctx *w, int fd)
{
	if (w->conn_count > 0)
		w->conn_count--;

	if (fd >= 0 && fd < MAX_TRACKED_FDS)
		w->resp_pending[fd] = 0;

	if (fd >= 0) {
		mc_fd_lock_acquire();
		mc_fd_mark(mc_this_core(), MC_FD_OP_CLOSE, fd);
		epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
		close(fd);
		mc_fd_unmark(mc_this_core());
		mc_fd_lock_release();
	}
}

/*
 * Create an independent listener socket on LISTEN_PORT with SO_REUSEPORT.
 * Each core creates its own listener inside its local lwIP state.
 */
static int create_core_listener(int core_id)
{
	struct sockaddr_in addr;
	int opt = 1;
	int fd;
	struct tcp_pcb_listen *l;

	printf("httpreply-mc: core %d create_core_listener starting (lwip_core %u, pcpu_idx %lu)\n",
	       core_id, lwip_current_core_id(),
	       (unsigned long)uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx));

	mc_fd_lock_acquire();
	mc_fd_mark(mc_this_core(), MC_FD_OP_LISTEN, -1);
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		printf("httpreply-mc: [ERR] socket failed: errno %d\n", errno);
		mc_fd_unmark(mc_this_core());
		mc_fd_lock_release();
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEADDR failed: errno %d\n", errno);
		close(fd);
		mc_fd_unmark(mc_this_core());
		mc_fd_lock_release();
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEPORT failed: errno %d\n", errno);
		close(fd);
		mc_fd_unmark(mc_this_core());
		mc_fd_lock_release();
		return -1;
	}

	configure_socket_options(fd);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(LISTEN_PORT);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("httpreply-mc: [ERR] bind failed: errno %d\n", errno);
		close(fd);
		mc_fd_unmark(mc_this_core());
		mc_fd_lock_release();
		return -1;
	}

	if (listen(fd, BACKLOG) < 0) {
		printf("httpreply-mc: [ERR] listen failed: errno %d\n", errno);
		close(fd);
		mc_fd_unmark(mc_this_core());
		mc_fd_lock_release();
		return -1;
	}

	l = lwip_get_core_state()->tcp_listen_pcbs.listen_pcbs;
	printf("httpreply-mc: core %d listener fd=%d (lwip_core %u, listen_pcb=%p)\n",
	       core_id, fd, lwip_current_core_id(), (void *)l);

	mc_fd_unmark(mc_this_core());
	mc_fd_lock_release();
	return fd;
}

/*
 * Sleep this vCPU for at most ns nanoseconds.
 *
 * The KVM platform implements this with a real halt: it arms this
 * core's own LAPIC one-shot timer for the wake, then executes
 * sti; hlt. The vCPU leaves the run queue for the whole sleep, so
 * an idle core uses no host CPU. The wake is the core's own timer
 * interrupt or its queue MSI, whichever comes first.
 * [Ticket 597c1b9731, Ticket a5b91e7993]
 */
static inline void
mc_idle_sleep(int core_id, uint64_t ns)
{
	unsigned long flags;
	__nsec now;

	/*
	 * Re-arm this core's queue interrupt before halting. A
	 * completion that posted while the queue was masked does not
	 * fire an MSI later. Writing the unmask register here makes
	 * the device signal any pending completion now, so the halt
	 * below either does not happen or ends immediately.
	 */
	{
		struct uk_netdev *dev = uk_netdev_get(0);

		if (dev)
			ena_netdev_rearm_cq_intr(dev, (uint16_t)core_id);
	}

	flags = uk_lcpu_save_irqf();
	uk_lcpu_disable_irq();
	now = ukplat_monotonic_clock();
	uk_lcpu_halt_irq_until((uint64_t)(now + ns));
	uk_lcpu_irqs_handle_pending();
	uk_lcpu_restore_irqf(flags);
}


/*
 * Report whether this core's lwIP instance still owns an active TCP
 * connection. A tracked fd can close (drop_connection) while its pcb
 * stays in the retransmit queue waiting for an ACK. The fd count then
 * reads zero but the flow is still live, so the core must keep
 * polling its queue. [Ticket 1152cbcaca]
 */
static int mc_core_has_pcb(void)
{
	struct lwip_core_state *cs = lwip_get_core_state();

	if (cs == NULL)
		return 0;

	return cs->tcp_active_pcbs != NULL;
}

/*
 * Run-to-completion worker engine. Bound to a single dedicated core
 * and its assigned hardware queue pair.
 */
static __noreturn void run_to_completion_worker(int core_id)
{
	mc_percore_alloc_selftest(core_id);
	struct worker_ctx *w = &mc_workers[core_id];
	struct epoll_event events[MAX_EVENTS];
	struct epoll_event ev;
	struct uk_netdev *dev;
	struct ib_state ib;
	unsigned long rx_prev, tx_prev;
	int server_fd;
	int n, i;

	server_fd = create_core_listener(core_id);
	if (server_fd < 0) {
		printf("httpreply-mc: [ERR] core %d failed to create listener\n",
		       core_id);
		for (;;) {
			uk_sched_yield();
		}
	}
	w->listener_fd = server_fd;

	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	mc_fd_lock_acquire();
	if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
		printf("httpreply-mc: [ERR] core %d failed to add listener to epoll: errno %d\n",
		       core_id, errno);
		close(server_fd);
		mc_fd_lock_release();
		for (;;) {
			uk_sched_yield();
		}
	}
	mc_fd_lock_release();

	printf("httpreply-mc: [INFO] core %d listening on port %d (queue pair %d)\n",
	       core_id, LISTEN_PORT, core_id);

	/*
	 * Idle backoff state for this core. Work is detected from the
	 * cumulative packet counters of this core's hardware queues, so
	 * an iteration counts as work even when it produces no epoll
	 * event (for example a timer-driven retransmit).
	 */
	dev = uk_netdev_get(0);
	ib_init(&ib, ukplat_monotonic_clock());
	rx_prev = dev ? ena_netdev_rxq_pkts(dev, (uint16_t)core_id) : 0UL;
	tx_prev = dev ? ena_netdev_txq_pkts(dev, (uint16_t)core_id) : 0UL;

	for (;;) {
		int work;
		uint64_t sleep_ns;

		drive_core_stack(core_id);

		n = epoll_wait(w->epoll_fd, events, MAX_EVENTS, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			printf("httpreply-mc: [ERR] core %d epoll_wait errno %d\n",
			       core_id, errno);
			continue;
		}

		for (i = 0; i < n; i++) {
			uint32_t base_events = EPOLLIN | EPOLLRDHUP;
			int fd = events[i].data.fd;

			if (fd == server_fd) {
				for (;;) {
					struct sockaddr_in client_addr;
					socklen_t client_len = sizeof(client_addr);
					int cfd;

					mc_fd_lock_acquire();
					mc_fd_mark(mc_this_core(),
						   MC_FD_OP_ACCEPT, server_fd);
					cfd = accept4(server_fd,
						      (struct sockaddr *)&client_addr,
						      &client_len, SOCK_NONBLOCK);
					if (cfd < 0) {
						mc_fd_unmark(mc_this_core());
						mc_fd_lock_release();
						break;
					}

					configure_socket_options(cfd);

					ev.events = EPOLLIN | EPOLLRDHUP;
					ev.data.fd = cfd;
					if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD,
						      cfd, &ev) < 0) {
						close(cfd);
						mc_fd_unmark(mc_this_core());
						mc_fd_lock_release();
						break;
					}
					mc_fd_unmark(mc_this_core());
					mc_fd_lock_release();
					w->conn_count++;
				}
				continue;
			}

			if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				drop_connection(w, fd);
				continue;
			}

			if (fd < MAX_TRACKED_FDS && w->resp_pending[fd] > 0) {
				if (events[i].events & EPOLLOUT) {
					if (send_pending_response(w, fd, base_events) < 0)
						drop_connection(w, fd);
				}
				continue;
			}

			if (events[i].events & (EPOLLIN | EPOLLRDNORM)) {
				if (handle_readable(w, fd) < 0)
					drop_connection(w, fd);
			}
		}

		/*
		 * Idle backoff. This iteration found work if an epoll
		 * event fired or a packet moved on this core's queues.
		 * The policy (idlebackoff.h) decides whether to sleep
		 * and for how long. [Ticket 597c1b9731]
		 */
		work = (n > 0);

		/*
		 * Retry any response that a previous send() left
		 * partial. The EPOLLOUT path is avoided (see
		 * send_pending_response), so drive retries from the
		 * loop directly.
		 */
		{
			int fd2;

			for (fd2 = 0; fd2 < MAX_TRACKED_FDS; fd2++) {
				int rc;

				if (w->resp_pending[fd2] > 0) {
					rc = send_pending_response(w, fd2,
								   EPOLLIN | EPOLLRDHUP);
					if (rc < 0) {
						drop_connection(w, fd2);
					} else {
						work = true;
						/*
						 * The response fully went out.
						 * The EPOLLIN guard above skips
						 * reads while a response is
						 * pending, and the event for data
						 * that arrived meanwhile may not
						 * repeat. Drain the socket now.
						 * [Ticket 1152cbcaca]
						 */
						if (rc == 0 &&
						    handle_readable(w, fd2) < 0)
							drop_connection(w, fd2);
					}
				}
			}
		}

		/*
		 * Keep the active (busy) backoff regime while this
		 * core has open connections or a live pcb. The device
		 * delays TX completion for a queue whose core sleeps,
		 * so a connected flow must stay polled. A pcb can
		 * outlive its tracked fd while it retransmits, so
		 * check both. Idle cores with no flow still fall
		 * through to deep sleep.
		 */
		work |= (w->conn_count > 0) || mc_core_has_pcb();



		if (dev) {
			unsigned long rx_now, tx_now;

			rx_now = ena_netdev_rxq_pkts(dev, (uint16_t)core_id);
			tx_now = ena_netdev_txq_pkts(dev, (uint16_t)core_id);
			work |= (rx_now > rx_prev) || (tx_now > tx_prev);
			rx_prev = rx_now;
			tx_prev = tx_now;
		}

		sleep_ns = ib_tick(&ib, ukplat_monotonic_clock(), work);

		if (sleep_ns) {
			mc_idle_sleep(core_id, sleep_ns);
		}
	}
}

static __noreturn void worker_thread(void *arg)
{
	struct worker_ctx *w = (struct worker_ctx *)arg;
	run_to_completion_worker(w->worker_id);
}

static __noreturn void mc_secondary_entry(void *arg)
{
	struct uk_lcpu *this_lcpu = (struct uk_lcpu *)arg;
	struct uk_alloc *a = uk_alloc_get_default();
	struct uk_sched *sec_s;
	int r;

	r = uk_lcpu_init(this_lcpu);
	if (unlikely(r))
		uk_lcpu_halt();

	uk_lcpu_enable_irq();

	sec_s = uk_schedcoop_create(a, a, a, a);
	if (unlikely(!sec_s))
		uk_lcpu_halt();

	uk_sched_register(sec_s);
	uk_sched_start(sec_s);

	while (1) {
		uk_sched_yield();
	}
}

static void mc_boot_secondary_cores(void)
{
#if CONFIG_HAVE_SMP && (CONFIG_UKPLAT_CPU_MAXCOUNT > 1)
	struct uk_alloc *a = uk_alloc_get_default();
	__u64 ap_idx[MC_MAX_WORKERS];
	__uptr ap_sp[MC_MAX_WORKERS];
	__uptr ap_entry[MC_MAX_WORKERS];
	unsigned int num_aps = 0;
	unsigned int i;
	int rc;

	for (i = 1; i < MC_MAX_WORKERS && i < CONFIG_UKPLAT_CPU_MAXCOUNT; i++) {
		void *stk;

		if (!mc_core_has_percore_alloc(i)) {
			printf("httpreply-mc: [INFO] core %u not started: "
			       "no per-core allocator bound\n", (unsigned)i);
			continue;
		}

		stk = uk_malloc(a, 16384);
		if (!stk)
			continue;
		ap_idx[num_aps] = i;
		ap_sp[num_aps] = (__uptr)stk + 16384;
		ap_entry[num_aps] = (__uptr)mc_secondary_entry;
		num_aps++;
	}

	if (num_aps > 0) {
		unsigned int started = num_aps;
		int j;

		rc = uk_lcpu_start(ap_idx, &started, ap_sp, ap_entry, 0);
		if (rc != 0)
			printf("httpreply-mc: [ERR] uk_lcpu_start failed: %d\n",
			       rc);

		for (j = 0; j < (int)started; j++) {
			unsigned long spins;
			int online = 0;

			/* Bounded wait for the AP to come online */
			for (spins = 0; spins < 200000000UL; spins++) {
				if (uk_lcpu_state_is_online(
					    uk_pcpuvar_lval(ap_idx[j],
							       uk_lcpus).state)) {
					online = 1;
					break;
				}
				__asm__ __volatile__("pause");
			}
			if (!online) {
				printf("httpreply-mc: [WARN] lcpu %u did not come online\n",
				       (unsigned)ap_idx[j]);
				continue;
			}

			/* Wait for secondary scheduler to register on uk_sched_head */
			for (spins = 0; spins < 200000000UL; spins++) {
				unsigned int count = 0;
				struct uk_sched *sch;
				for (sch = uk_sched_head; sch != NULL; sch = sch->next)
					count++;
				if (count > (unsigned int)j + 1)
					break;
				__asm__ __volatile__("pause");
			}
		}
	}
#endif
}

int main(int argc, char **argv)
{
	int i;
	struct uk_sched *s;

	(void)argc;
	(void)argv;

#if CONFIG_APPHTTPREPLYMC_NETLOG
	mc_netlog_init();
#endif

	printf("\n=============================================\n");
	printf(" Unikraft HTTP Benchmark Server (lib-ena-mc)\n");
	printf(" Port: %d (TCP)\n", LISTEN_PORT);
	printf(" Mode: Shared-nothing run-to-completion (NO_SYS)\n");
	printf(" Driver: AWS ENA multi-queue\n");
	printf(" Stack: lwIP per-core state (SO_REUSEPORT)\n");
	printf("=============================================\n\n");

	mc_percore_alloc_init();

	mc_boot_secondary_cores();

	mc_nworkers = 0;
	for (s = uk_sched_head; s != NULL && mc_nworkers < MC_MAX_WORKERS; s = s->next)
		mc_nworkers++;

	if (mc_nworkers < 1)
		mc_nworkers = 1;

	printf("httpreply-mc: detected %d worker cores\n", mc_nworkers);
	if (netif_default) {
		printf("httpreply-mc: [INFO] netif %c%c%u IP %s gw %s\n",
		       netif_default->name[0], netif_default->name[1],
		       netif_default->num,
		       ip4addr_ntoa(netif_ip4_addr(netif_default)),
		       ip4addr_ntoa(netif_ip4_gw(netif_default)));

		if (!ip4_addr_isany_val(*netif_ip4_gw(netif_default))) {
			const ip4_addr_t *gw = netif_ip4_gw(netif_default);
			struct lwip_core_state *cs0;
			int resolved = 0;
			unsigned long spins;

			/*
			 * Best-effort early resolution. With DHCP the gateway
			 * is unknown at this point and resolution is deferred
			 * to runtime: core 0 resolves the gateway on its first
			 * off-subnet transmit and the sync in
			 * drive_core_stack() carries the MAC to the other
			 * cores.
			 */
			printf("httpreply-mc: resolving gateway %s ARP...\n",
			       ip4addr_ntoa(gw));
			etharp_request(netif_default, gw);

			for (spins = 0; spins < 20000000UL; spins++) {
				drive_core_stack(0);
				cs0 = lwip_get_core_state_by_id(0);
				for (i = 0; i < ARP_TABLE_SIZE; i++) {
					if (cs0->arp_table[i].state >= MC_ARP_STATE_STABLE &&
					    cs0->arp_table[i].ipaddr.addr == gw->addr) {
						resolved = 1;
						break;
					}
				}
				if (resolved)
					break;
				if ((spins % 2000000UL) == 0 && spins > 0)
					etharp_request(netif_default, gw);
			}

			if (resolved) {
				printf("httpreply-mc: gateway ARP resolved to %02x:%02x:%02x:%02x:%02x:%02x\n",
				       cs0->arp_table[i].ethaddr.addr[0],
				       cs0->arp_table[i].ethaddr.addr[1],
				       cs0->arp_table[i].ethaddr.addr[2],
				       cs0->arp_table[i].ethaddr.addr[3],
				       cs0->arp_table[i].ethaddr.addr[4],
				       cs0->arp_table[i].ethaddr.addr[5]);
				/*
				 * Mark the core 0 entry static so the table
				 * recycling never evicts it, and publish the
				 * MAC. Each worker adopts the MAC in its own
				 * ARP table during its first stack drive.
				 */
				if (etharp_add_static_entry(gw,
						&cs0->arp_table[i].ethaddr) != ERR_OK)
					printf("httpreply-mc: [WARN] failed to "
					       "make gateway entry static\n");
				mc_arp_publish();
			} else {
				printf("httpreply-mc: [WARN] gateway ARP resolution timed out\n");
			}
		}
	}

	for (i = 0; i < mc_nworkers; i++) {
		mc_workers[i].worker_id = i;
		mc_workers[i].epoll_fd = epoll_create1(0);
		if (mc_workers[i].epoll_fd < 0) {
			printf("httpreply-mc: [ERR] epoll_create1 failed for worker %d\n", i);
			return 1;
		}
		mc_workers[i].recv_buf = malloc(MC_RECVBUF_SIZE);
		mc_workers[i].resp_pending = calloc(MAX_TRACKED_FDS, sizeof(uint32_t));
		mc_workers[i].conn_count = 0;
		mc_workers[i].req_count = 0;
		mc_workers[i].byte_count = 0;
	}

	for (i = 1; i < mc_nworkers; i++) {
		struct uk_sched *ws = mc_get_sched(i);
		struct uk_thread *th;

		if (ws == NULL)
			ws = uk_sched_current();

		th = uk_sched_thread_create(ws, worker_thread, &mc_workers[i], "worker");
		if (th == NULL) {
			printf("httpreply-mc: [ERR] failed to start worker %d\n", i);
			return 1;
		}
	}

	run_to_completion_worker(0);

	return 0;
}
