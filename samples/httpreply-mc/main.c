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
 * version counter in its stack drive loop. When the version advances,
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

static void mc_arp_adopt(unsigned int core_id)
{
	struct lwip_core_state *cs;
	uint32_t count, i;
	struct mc_arp_pub_entry entries[MC_ARP_SYNC_MAX];

	if (!netif_default)
		return;

	/* Check if any entry in our local ARP table is PENDING */
	static __nsec last_arp_req;
	__nsec now = ukplat_monotonic_clock();

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

/*
 * Update the interest set of an fd registered with worker epoll instance.
 */
static int set_epoll_events(int epfd, int fd, uint32_t events)
{
	struct epoll_event ev;

	ev.events = events;
	ev.data.fd = fd;

	return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

void ena_netdev_dump_queue(struct uk_netdev *dev, uint16_t qid);
unsigned long ena_netdev_rxq_pkts(struct uk_netdev *dev, uint16_t qid);
unsigned long ena_netdev_txq_pkts(struct uk_netdev *dev, uint16_t qid);

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

		printf("httpreply-mc: core %d heartbeat (polls=%lu, rx=%lu, tx=%lu, active=%u, free: pbuf=%u pcb=%u seg=%u)\n",
		       core_id, poll_cnt[core_id], rxpkts, txpkts,
		       active_pcbs, m_pbuf, m_pcb, m_seg);

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
	}
	uknetdev_poll_rxqueue((uint16_t)core_id);
	sys_check_timeouts();

	/*
	 * Synchronize ARP entries across cores. Core 0 receives ARP replies
	 * from the network and publishes resolved MACs. The other cores
	 * adopt the MACs in their own ARP tables.
	 */
	if (core_id == 0)
		mc_arp_publish();
	else
		mc_arp_adopt((unsigned int)core_id);
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

	if (pending > 0) {
		if (set_epoll_events(w->epoll_fd, fd, base_events | EPOLLOUT) < 0)
			return -1;
		return 1;
	}

	if (set_epoll_events(w->epoll_fd, fd, base_events) < 0)
		return -1;

	return 0;
}

/*
 * Drop a connection: clear its pending bytes, remove it from epoll,
 * and close the socket descriptor.
 */
static void drop_connection(struct worker_ctx *w, int fd)
{
	if (fd >= 0 && fd < MAX_TRACKED_FDS)
		w->resp_pending[fd] = 0;

	if (fd >= 0) {
		epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
		close(fd);
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

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		printf("httpreply-mc: [ERR] socket failed: errno %d\n", errno);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEADDR failed: errno %d\n", errno);
		close(fd);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
		printf("httpreply-mc: [ERR] SO_REUSEPORT failed: errno %d\n", errno);
		close(fd);
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
		return -1;
	}

	if (listen(fd, BACKLOG) < 0) {
		printf("httpreply-mc: [ERR] listen failed: errno %d\n", errno);
		close(fd);
		return -1;
	}

	l = lwip_get_core_state()->tcp_listen_pcbs.listen_pcbs;
	printf("httpreply-mc: core %d listener fd=%d (lwip_core %u, listen_pcb=%p)\n",
	       core_id, fd, lwip_current_core_id(), (void *)l);

	return fd;
}

/*
 * Sleep this vCPU for at most ns nanoseconds.
 *
 * The KVM platform implements this with a real halt: it arms a
 * one-shot timer interrupt for the wake, then executes sti; hlt. The
 * vCPU leaves the run queue for the whole sleep, so an idle core
 * uses no host CPU. The wake is the timer interrupt or any earlier
 * interrupt.
 * [Ticket 597c1b9731]
 */
static inline void
mc_idle_sleep(uint64_t ns)
{
	unsigned long flags;
	__nsec now;

	flags = uk_lcpu_save_irqf();
	uk_lcpu_disable_irq();
	now = ukplat_monotonic_clock();
	uk_lcpu_halt_irq_until((uint64_t)(now + ns));
	uk_lcpu_irqs_handle_pending();
	uk_lcpu_restore_irqf(flags);
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
	if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
		printf("httpreply-mc: [ERR] core %d failed to add listener to epoll: errno %d\n",
		       core_id, errno);
		close(server_fd);
		for (;;) {
			uk_sched_yield();
		}
	}

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

					cfd = accept4(server_fd,
						      (struct sockaddr *)&client_addr,
						      &client_len, SOCK_NONBLOCK);
					if (cfd < 0)
						break;

					/*
					 * Log the cumulative RX queue packet counter alongside
					 * the accept. This shows whether accepted connections
					 * follow the RSS queue distribution. [Ticket ba82aec88b]
					 * Rate-limited to prevent serial port blocking under load.
					 */
					if (cfd <= 10 || (cfd % 50) == 0) {
						struct uk_netdev *_dev = uk_netdev_get(0);
						unsigned long _rxpkts = 0;

						if (_dev)
							_rxpkts = ena_netdev_rxq_pkts(_dev,
										      (uint16_t)core_id);
						printf("httpreply-mc: core %d ACCEPTED fd=%d from %s:%d"
						       " (rxq%d_pkts=%lu)\n",
						       core_id, cfd,
						       ip4addr_ntoa((const ip4_addr_t *)&client_addr.sin_addr),
						       ntohs(client_addr.sin_port),
						       core_id, _rxpkts);
					}

					configure_socket_options(cfd);

					ev.events = EPOLLIN | EPOLLRDHUP;
					ev.data.fd = cfd;
					if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD,
						      cfd, &ev) < 0) {
						close(cfd);
						break;
					}
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
				ssize_t r = recv(fd, w->recv_buf, MC_RECVBUF_SIZE - 1, 0);

				if (r > 0) {
					w->recv_buf[r] = '\0';
					w->req_count++;
					w->byte_count += http_resp_len;
					if ((w->req_count % MC_STATS_INTERVAL) == 0)
						printf("httpreply-mc: [stats] core %d: req=%lu bytes=%lu\n",
						       core_id,
						       (unsigned long)w->req_count,
						       (unsigned long)w->byte_count);

					if (fd < MAX_TRACKED_FDS) {
						w->resp_pending[fd] = (uint32_t)http_resp_len;
						if (send_pending_response(w, fd, base_events) < 0)
							drop_connection(w, fd);
					} else {
						send(fd, http_response, http_resp_len, 0);
					}

					if (strstr(w->recv_buf, "Connection: close") != NULL ||
					    strstr(w->recv_buf, "connection: close") != NULL)
						drop_connection(w, fd);
				} else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
					drop_connection(w, fd);
				}
			}
		}

		/*
		 * Idle backoff. This iteration found work if an epoll
		 * event fired or a packet moved on this core's queues.
		 * The policy (idlebackoff.h) decides whether to sleep
		 * and for how long. [Ticket 597c1b9731]
		 */
		work = (n > 0);

		if (dev) {
			unsigned long rx_now, tx_now;

			rx_now = ena_netdev_rxq_pkts(dev, (uint16_t)core_id);
			tx_now = ena_netdev_txq_pkts(dev, (uint16_t)core_id);
			work |= (rx_now > rx_prev) || (tx_now > tx_prev);
			rx_prev = rx_now;
			tx_prev = tx_now;
		}

		sleep_ns = ib_tick(&ib, ukplat_monotonic_clock(), work);
		if (sleep_ns)
			mc_idle_sleep(sleep_ns);
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
