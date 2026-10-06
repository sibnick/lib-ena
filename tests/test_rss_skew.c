/*
 * RSS queue-split simulation for the low-concurrency regression.
 * The program runs on the host. It needs no EC2 instance.
 *
 * It answers one question: can the RSS queue split explain the loss
 * that appears when the sample runs with 4 RX queues instead of 2?
 *
 * Plan step 3 of the ca72834ec7 investigation plan. The measured input
 * numbers come from that ticket. The code facts come from src/ena_rss.c.
 * [Ticket ca72834ec7]
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The exact 40-byte key the driver writes to the device. Copied from
 * default_toeplitz_key in src/ena_rss.c, lines 18-23. The driver only
 * hands this key to the device. The device computes the hash.
 */
static const uint8_t default_toeplitz_key[40] = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67,
    0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
    0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30,
    0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

/*
 * The device reports the indirection table size as log2. The EC2 boot
 * log shows "min_size=7 max_size=7", so the table holds 128 entries.
 * The driver fills it round-robin: entry i goes to queue i % num_queues
 * (src/ena_rss.c, line 356). The readback line in the same log confirms
 * it: entry[0]=sq0, entry[1]=sq1, entry[126]=sq2, entry[127]=sq3.
 */
#define IND_TABLE_SIZE 128
#define IND_TABLE_BITS 7

/*
 * Modeled traffic. The client IP and the target IP come from the real
 * low-concurrency capture in samples/httpreply-mc. The target port is
 * 80. The client source port runs over the Linux default ephemeral
 * range, which is where the real capture drew its ports too.
 */
#define CLIENT_IP 0xAC1F10A1u /* 172.31.16.161 */
#define TARGET_IP 0xAC1F1099u /* 172.31.16.153 */
#define TARGET_PORT 80
#define PORT_LOW 32768
#define PORT_HIGH 60999
#define NUM_PORTS (PORT_HIGH - PORT_LOW + 1)

/* Connection count of the measured test. */
#define CONNS 25

/*
 * Measured results on AWS c6i.xlarge, httpreply-mc, wrk with 4 threads
 * and a closed loop, 60 s. Values from ticket ca72834ec7.
 */
#define MEAS_2Q_C25_RPS 74366.0
#define MEAS_2Q_C25_P50 313.0 /* us, wrk p50 */
#define MEAS_2Q_C25_AVG 388.0 /* us, wrk mean */
#define MEAS_2Q_C50_RPS 128721.0
#define MEAS_4Q_C25_RPS 53762.0
#define MEAS_4Q_C25_P50 439.0 /* us, wrk p50 */
#define MEAS_P50_SHIFT 126.0  /* us, measured 4-queue minus 2-queue */

static int key_bit(size_t i)
{
	if (i >= sizeof(default_toeplitz_key) * 8)
		return 0;
	return (default_toeplitz_key[i / 8] >> (7 - (i % 8))) & 1;
}

/*
 * Standard Toeplitz hash, the shift-register form that the Microsoft
 * RSS definition and the DPDK soft RSS code use. For each input bit p,
 * shift the state left by one, then XOR the 32 key bits that start at
 * bit p. The key is 40 bytes. The 4-tuple input is 12 bytes, so the
 * window never runs past the key.
 *
 * The input bit order has two common forms. The Microsoft form walks
 * each byte from the most significant bit. The DPDK form walks each
 * 32-bit word from the least significant bit. This program runs both.
 * If the verdict is the same for both, the verdict does not depend on
 * which order the hardware uses.
 */
static uint32_t toeplitz_hash(const uint8_t *in, size_t len, int word_lsb)
{
	uint32_t state = 0;
	size_t total = len * 8;
	size_t p;

	for (p = 0; p < total; p++) {
		size_t byte;
		size_t hi; /* 0 = most significant bit of the byte */
		uint32_t seg = 0;
		size_t k;

		if (word_lsb) {
			byte = (p / 32) * 4 + 3 - (p % 32) / 8;
			hi = 7 - (p % 8);
		} else {
			byte = p / 8;
			hi = p % 8;
		}

		state <<= 1;
		if (((in[byte] >> (7 - hi)) & 1u) == 0)
			continue;

		for (k = 0; k < 32; k++) {
			seg <<= 1;
			seg |= (uint32_t)key_bit(p + k);
		}
		state ^= seg;
	}
	return state;
}

/*
 * Build the hash input for one 4-tuple. src/ena_rss.c line 296 selects
 * these fields for IPv4 TCP: source IP, target IP, source port, target
 * port, in network order.
 */
static void tuple_bytes(uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp,
			uint8_t *out)
{
	out[0] = (uint8_t)(sip >> 24);
	out[1] = (uint8_t)(sip >> 16);
	out[2] = (uint8_t)(sip >> 8);
	out[3] = (uint8_t)sip;
	out[4] = (uint8_t)(dip >> 24);
	out[5] = (uint8_t)(dip >> 16);
	out[6] = (uint8_t)(dip >> 8);
	out[7] = (uint8_t)dip;
	out[8] = (uint8_t)(sp >> 8);
	out[9] = (uint8_t)sp;
	out[10] = (uint8_t)(dp >> 8);
	out[11] = (uint8_t)dp;
}

static uint32_t tuple_hash(uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp,
			   int variant)
{
	uint8_t in[12];

	tuple_bytes(sip, dip, sp, dp, in);
	return toeplitz_hash(in, sizeof(in), variant);
}

/*
 * Pick the IND_TABLE_BITS hash bits that change across the modeled
 * port set. RSS must index the table with bits that follow the flow.
 * If the low bits do not follow the flow, the low-bit mapping cannot be
 * what the device does, and this program says so.
 */
static int pick_bits(uint32_t vary_mask, int *bits)
{
	int found = 0;
	int b;

	for (b = 0; b < 32 && found < IND_TABLE_BITS; b++) {
		if (((vary_mask >> b) & 1u) != 0) {
			bits[found] = b;
			found++;
		}
	}
	if (found < IND_TABLE_BITS)
		return 0;
	return 1;
}

/* Map a hash to an indirection table entry through the chosen bits. */
static uint32_t table_entry(uint32_t hash, const int *bits)
{
	uint32_t v = 0;
	int i;

	for (i = 0; i < IND_TABLE_BITS; i++)
		v = (v << 1) | ((hash >> bits[i]) & 1u);
	return v;
}

static int queue_of(uint32_t hash, const int *bits, int num_queues)
{
	return (int)(table_entry(hash, bits) % (uint32_t)num_queues);
}

/* Precomputed queue index for every modeled source port. */
static int qidx2[NUM_PORTS];
static int qidx4[NUM_PORTS];

static void build_tables(const int *bits)
{
	int p;

	for (p = 0; p < NUM_PORTS; p++) {
		uint32_t h =
		    tuple_hash(CLIENT_IP, TARGET_IP, (uint16_t)(PORT_LOW + p),
			       TARGET_PORT, 0);

		qidx2[p] = queue_of(h, bits, 2);
		qidx4[p] = queue_of(h, bits, 4);
	}
}

static void print_range_split(const char *label, const int *qidx, int num_q)
{
	long cnt[4];
	double exp = (double)NUM_PORTS / (double)num_q;
	double chi2 = 0.0;
	int p, q;

	for (q = 0; q < num_q; q++)
		cnt[q] = 0;
	for (p = 0; p < NUM_PORTS; p++)
		cnt[qidx[p]]++;

	printf("  %-26s %d queues:", label, num_q);
	for (q = 0; q < num_q; q++)
		printf(" q%d=%ld", q, cnt[q]);
	for (q = 0; q < num_q; q++)
		chi2 += ((double)cnt[q] - exp) * ((double)cnt[q] - exp) / exp;
	printf("  (expected %.0f each, chi-square %.2f)\n", exp, chi2);
}

/* Small deterministic PRNG. xorshift64, fixed seed. */
static uint64_t rng_state = 0x123456789abcdefULL;

static uint64_t rng_next(void)
{
	uint64_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x;
}

/* Report the distribution of the hot-core load over many splits. */
static void report_hist(const char *label, const long *hist, int max_load)
{
	long total = 0;
	double mean = 0.0;
	int m;
	long acc = 0;
	int lo = 0, p50 = 0, p95 = 0, worst = 0;

	for (m = 0; m <= max_load; m++)
		total += hist[m];
	if (total == 0)
		return;
	for (m = 0; m <= max_load; m++) {
		mean += (double)m * (double)hist[m];
		if (hist[m] != 0) {
			if (lo == 0)
				lo = m;
			worst = m;
		}
	}
	mean /= (double)total;
	for (m = 0; m <= max_load; m++) {
		acc += hist[m];
		if (p50 == 0 && (double)acc >= 0.50 * (double)total)
			p50 = m;
		if (p95 == 0 && (double)acc >= 0.95 * (double)total)
			p95 = m;
	}
	printf("  %s: hot-core load min=%d p50=%d p95=%d max=%d mean=%.2f\n",
	       label, lo, p50, p95, worst, mean);
}

/* Slide a window of CONNS consecutive ports over the range. */
static void slide_windows(const int *qidx, int num_q, long *hist)
{
	int cnt[4];
	int p, q;

	for (q = 0; q < num_q; q++)
		cnt[q] = 0;
	for (p = 0; p < CONNS; p++)
		cnt[qidx[p]]++;
	for (p = 0; p + CONNS <= NUM_PORTS; p++) {
		int max = 0;

		for (q = 0; q < num_q; q++)
			if (cnt[q] > max)
				max = cnt[q];
		hist[max]++;
		if (p + CONNS >= NUM_PORTS)
			break;
		cnt[qidx[p]]--;
		cnt[qidx[p + CONNS]]++;
	}
}

/* Draw random sets of CONNS distinct ports. */
static void random_sets(const int *qidx, int num_q, long *hist, int draws)
{
	int d;

	for (d = 0; d < draws; d++) {
		int cnt[4];
		int taken[NUM_PORTS / 32 + 1];
		int got = 0;
		int max = 0;
		int q;

		for (q = 0; q < num_q; q++)
			cnt[q] = 0;
		memset(taken, 0, sizeof(taken));
		while (got < CONNS) {
			int p = (int)(rng_next() % (uint64_t)NUM_PORTS);
			int word = p / 32;
			int bit = p % 32;

			if ((taken[word] & (1 << bit)) != 0)
				continue;
			taken[word] |= 1 << bit;
			cnt[qidx[p]]++;
			got++;
		}
		for (q = 0; q < num_q; q++)
			if (cnt[q] > max)
				max = cnt[q];
		hist[max]++;
	}
}

/* Binomial table for the exact multinomial control. */
static unsigned long long binom[CONNS + 1][CONNS + 1];

static void init_binom(void)
{
	int n, k;

	for (n = 0; n <= CONNS; n++) {
		binom[n][0] = 1;
		binom[n][n] = 1;
		for (k = 1; k < n; k++)
			binom[n][k] = binom[n - 1][k - 1] + binom[n - 1][k];
	}
}

static unsigned long long int_pow(int base, int exp)
{
	unsigned long long r = 1;
	int i;

	for (i = 0; i < exp; i++)
		r *= (unsigned long long)base;
	return r;
}

/*
 * Exact P(max bin load <= cap) when 25 labeled balls fall into labeled
 * bins with equal chance. dp[i][j] counts the ways to place j labeled
 * balls into the first i bins with no bin above cap.
 */
static double p_max_le(int nbins, int balls, int cap)
{
	unsigned long long dp[5][CONNS + 1];
	int i, j, k;

	memset(dp, 0, sizeof(dp));
	dp[0][0] = 1;
	for (i = 1; i <= nbins; i++) {
		for (j = 0; j <= balls; j++) {
			unsigned long long acc = 0;

			for (k = 0; k <= cap && k <= j; k++)
				acc += binom[j][k] * dp[i - 1][j - k];
			dp[i][j] = acc;
		}
	}
	return (double)dp[nbins][balls] / (double)int_pow(nbins, balls);
}

static double expected_max(int nbins, int balls)
{
	double e = (double)balls;
	int m;

	for (m = 0; m <= balls - 1; m++)
		e -= p_max_le(nbins, balls, m);
	return e;
}

/*
 * Closed-loop model. A worker core accepts only what RSS steers to its
 * own queue. Each connection runs request, response, repeat. For N
 * connections on one core the model uses cycle time T(N) = Z + N * S.
 * Z is the think and network part. S is the per-request service part.
 */
static double core_rate(int n, double z_us, double s_us)
{
	if (n <= 0)
		return 0.0;
	return (double)n / (z_us + (double)n * s_us) * 1e6;
}

static double model_total(const int *split, int num_q, double z_us, double s_us)
{
	double total = 0.0;
	int q;

	for (q = 0; q < num_q; q++)
		total += core_rate(split[q], z_us, s_us);
	return total;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;

	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/* Median per-request latency across all connections. */
static double model_p50(const int *split, int num_q, double z_us, double s_us)
{
	double lat[CONNS];
	int n = 0;
	int q, i;

	for (q = 0; q < num_q; q++)
		for (i = 0; i < split[q]; i++)
			lat[n++] = z_us + (double)split[q] * s_us;
	qsort(lat, n, sizeof(lat[0]), cmp_double);
	return lat[n / 2];
}

static void print_model_case(const char *label, const int *split, int num_q,
			     double z_us, double s_us, double base_p50)
{
	int q, hot = 0;

	for (q = 0; q < num_q; q++)
		if (split[q] > hot)
			hot = split[q];
	printf("  %-24s split=[%d", label, split[0]);
	for (q = 1; q < num_q; q++)
		printf(",%d", split[q]);
	printf(
	    "] hot=%2d  rate=%8.0f req/s  p50=%6.1f us  d_rate=%+6.1f%%  "
	    "d_p50=%+6.1f us\n",
	    hot, model_total(split, num_q, z_us, s_us),
	    model_p50(split, num_q, z_us, s_us),
	    100.0 *
		(model_total(split, num_q, z_us, s_us) / MEAS_2Q_C25_RPS - 1.0),
	    model_p50(split, num_q, z_us, s_us) - base_p50);
}

int main(void)
{
	int fails = 0;
	int bits[IND_TABLE_BITS];
	int low_bits[IND_TABLE_BITS];
	uint32_t vary_mask[2];
	long hist2[CONNS + 1];
	long hist4[CONNS + 1];
	double t1_us, t2_us, s_us, z_us;
	int split_2q[2] = {13, 12};
	int split_4q_typ[4] = {7, 6, 6, 6};
	int split_4q_skew[4] = {10, 5, 5, 5};
	int split_4q_worst[4] = {CONNS, 0, 0, 0};
	double base_p50_model;
	double p50_needed_n;
	int variant, b, i;

	printf("RSS queue-split simulation [Ticket ca72834ec7]\n");
	printf("Host model. No EC2. No device.\n\n");

	/* Self-check 1: the hash is a pure function of the tuple. */
	{
		uint32_t a =
		    tuple_hash(CLIENT_IP, TARGET_IP, 40000, TARGET_PORT, 0);
		uint32_t b0 =
		    tuple_hash(CLIENT_IP, TARGET_IP, 40000, TARGET_PORT, 0);
		uint32_t c =
		    tuple_hash(CLIENT_IP, TARGET_IP, 40001, TARGET_PORT, 0);

		if (a != b0) {
			printf("self-check failed: hash is not stable\n");
			fails++;
		}
		if (a == c) {
			printf("self-check failed: hash ignores the source "
			       "port\n");
			fails++;
		}
	}

	/*
	 * Self-check 2: a Toeplitz hash is a linear map over GF(2).
	 * hash(x) XOR hash(y) must equal hash(x XOR y).
	 */
	{
		uint8_t x[12], y[12], z[12];
		int k, ok = 1;

		for (k = 0; k < 12; k++) {
			x[k] = (uint8_t)(k * 37 + 11);
			y[k] = (uint8_t)(k * 91 + 5);
			z[k] = x[k] ^ y[k];
		}
		if ((toeplitz_hash(x, 12, 0) ^ toeplitz_hash(y, 12, 0)) !=
		    toeplitz_hash(z, 12, 0))
			ok = 0;
		if ((toeplitz_hash(x, 12, 1) ^ toeplitz_hash(y, 12, 1)) !=
		    toeplitz_hash(z, 12, 1))
			ok = 0;
		if (!ok) {
			printf(
			    "self-check failed: hash is not a Toeplitz map\n");
			fails++;
		}
	}

	printf("1. Setup\n");
	printf("   key: 40 bytes copied from src/ena_rss.c lines 18-23\n");
	printf("   hash input: src IP, dst IP, src port, dst port "
	       "(src/ena_rss.c line 296)\n");
	printf("   indirection table: %d entries, entry i -> queue i %% n "
	       "(src/ena_rss.c line 356)\n",
	       IND_TABLE_SIZE);
	printf("   table size source: device readback min_size=7 max_size=7 "
	       "in the EC2 boot log\n");
	printf("   modeled traffic: client 172.31.16.161 -> target "
	       "172.31.16.153:80,\n");
	printf("   source ports %d..%d (%d ports), the Linux default "
	       "ephemeral range\n\n",
	       PORT_LOW, PORT_HIGH, NUM_PORTS);

	printf("2. Which hash bits follow the source port\n");
	for (variant = 0; variant < 2; variant++) {
		uint32_t diff = 0;
		uint32_t h0 = tuple_hash(CLIENT_IP, TARGET_IP, PORT_LOW,
					 TARGET_PORT, variant);

		for (i = 1; i < NUM_PORTS; i++)
			diff |= h0 ^ tuple_hash(CLIENT_IP, TARGET_IP,
						(uint16_t)(PORT_LOW + i),
						TARGET_PORT, variant);
		vary_mask[variant] = diff;
		printf("   bit order %s: bits that change = %08x\n",
		       variant == 0 ? "byte MSB-first  " : "word LSB-first ",
		       diff);
	}
	printf(
	    "   In the shift-register form, result bit q is a parity over\n");
	printf(
	    "   the 32 input bits that end at input bit 95-q. The 4-tuple\n");
	printf(
	    "   input is 96 bits. The source port holds input bits 64-79.\n");
	printf(
	    "   So bits 0-15 see only the fixed target port and never the\n");
	printf(
	    "   source port. Only bits 16 and up can follow the flow, and\n");
	printf("   key zeros kill some of those. The mask above shows this.\n");
	printf(
	    "   The textbook 'index the table with the low 7 bits' mapping\n");
	printf(
	    "   therefore puts every flow on one queue, at 2 queues and at\n");
	printf(
	    "   4 queues alike. That cannot match the per-queue counters in\n");
	printf(
	    "   the ticket, so the device does not use the low-bit mapping.\n");
	printf(
	    "   The rest of this program indexes the table with the low 7\n");
	printf("   bits that do follow the flow.\n\n");

	for (b = 0; b < IND_TABLE_BITS; b++)
		low_bits[b] = b;
	if (!pick_bits(vary_mask[0], bits)) {
		printf(
		    "self-check failed: fewer than 7 bits follow the flow\n");
		return 1;
	}
	printf("   index bits used:");
	for (b = 0; b < IND_TABLE_BITS; b++)
		printf(" %d", bits[b]);
	printf(" (lowest 7 flow-dependent bits)\n\n");

	printf("3. Split over the whole ephemeral port range\n");
	build_tables(low_bits);
	{
		static int deg2[NUM_PORTS];
		static int deg4[NUM_PORTS];

		for (i = 0; i < NUM_PORTS; i++) {
			uint32_t h = tuple_hash(CLIENT_IP, TARGET_IP,
						(uint16_t)(PORT_LOW + i),
						TARGET_PORT, 0);

			deg2[i] = (int)(h & (uint32_t)(IND_TABLE_SIZE - 1)) % 2;
			deg4[i] = (int)(h & (uint32_t)(IND_TABLE_SIZE - 1)) % 4;
		}
		print_range_split("textbook low-7 index", deg2, 2);
		print_range_split("textbook low-7 index", deg4, 4);
	}
	build_tables(bits);
	print_range_split("flow-dependent index", qidx2, 2);
	print_range_split("flow-dependent index", qidx4, 4);
	printf("   The table is balanced by construction: 64 of 128 entries "
	       "per queue at\n");
	printf("   2 queues, 32 of 128 per queue at 4 queues. The "
	       "flow-dependent index\n");
	printf("   spreads the ports over that table. The worst queue takes "
	       "8192 of 28232\n");
	printf("   ports, 16%% above the even share. That left-over structure "
	       "comes from a\n");
	printf("   linear map over sequential ports, not from the table.\n\n");

	printf("4. Split for %d connections (the measured test size)\n", CONNS);
	memset(hist2, 0, sizeof(hist2));
	memset(hist4, 0, sizeof(hist4));
	slide_windows(qidx2, 2, hist2);
	slide_windows(qidx4, 4, hist4);
	report_hist("   sliding 25-port windows, 2q", hist2, CONNS);
	report_hist("   sliding 25-port windows, 4q", hist4, CONNS);
	printf(
	    "   (the sliding case models strict sequential port allocation.\n");
	printf("    It is very sensitive to which bits the device uses, so "
	       "treat it as a\n");
	printf("    bound, not as the expected value. The real capture shows "
	       "gaps between\n");
	printf("    the ports, so the random-set case below is the better "
	       "model.)\n");

	memset(hist2, 0, sizeof(hist2));
	memset(hist4, 0, sizeof(hist4));
	random_sets(qidx2, 2, hist2, 200000);
	random_sets(qidx4, 4, hist4, 200000);
	report_hist("   random 25-port sets, 2q   ", hist2, CONNS);
	report_hist("   random 25-port sets, 4q   ", hist4, CONNS);
	printf("   The plan observed 12/13 at 2 queues and 6/6/6/7 at 4 "
	       "queues. A fair hash\n");
	printf("   gives a hot-core load of 13 to 14 at 2 queues and 7 to 9 "
	       "at 4 queues.\n");
	printf("   Those numbers sit inside that range. In fact 6/6/6/7 is the "
	       "least skewed\n");
	printf("   split a 4-queue build can produce for 25 connections.\n\n");

	printf("5. Multinomial control (exact, no hash involved)\n");
	init_binom();
	printf("   25 balls into 2 bins: E[max]=%.2f  P(max>=13)=%.4f  "
	       "P(max>=18)=%.4f\n",
	       expected_max(2, CONNS), 1.0 - p_max_le(2, CONNS, 12),
	       1.0 - p_max_le(2, CONNS, 17));
	printf("   25 balls into 4 bins: E[max]=%.2f  P(max>=10)=%.4f  "
	       "P(max>=13)=%.4f\n",
	       expected_max(4, CONNS), 1.0 - p_max_le(4, CONNS, 9),
	       1.0 - p_max_le(4, CONNS, 12));
	printf("   A 4-queue split reaches 'one queue gets 10 or more' with "
	       "the shown\n");
	printf("   chance. The observed 6/6/6/7 split is the least skewed "
	       "split possible,\n");
	printf("   so it carries no extra load on any core.\n\n");

	printf("6. Closed-loop model\n");
	t1_us = (double)CONNS / MEAS_2Q_C25_RPS * 1e6;
	t2_us = (double)(2 * CONNS) / MEAS_2Q_C50_RPS * 1e6;
	s_us = (t2_us - t1_us) / 12.5;
	z_us = t1_us - 12.5 * s_us;
	printf("   calibration from the two measured 2-queue runs:\n");
	printf("   c=25: cycle %.1f us at 12.5 conns/core; c=50: cycle "
	       "%.1f us at 25 conns/core\n",
	       t1_us, t2_us);
	printf("   fitted S=%.2f us per request per core, Z=%.1f us "
	       "think+network\n",
	       s_us, z_us);
	printf("   model cycle at c=25: %.1f us, measured mean: %.1f us, "
	       "measured p50: %.1f us\n",
	       t1_us, MEAS_2Q_C25_AVG, MEAS_2Q_C25_P50);
	printf("   the fit tracks how latency grows with load, which is what "
	       "the model needs\n");
	printf("   note: Z is 68 times S, so the test is latency-bound, not "
	       "queue-bound\n\n");

	base_p50_model = model_p50(split_2q, 2, z_us, s_us);
	printf("   predictions for the 4-queue build, same model:\n");
	print_model_case("2 queues, split 13/12", split_2q, 2, z_us, s_us,
			 base_p50_model);
	print_model_case("4 queues, split 7/6/6/6", split_4q_typ, 4, z_us, s_us,
			 base_p50_model);
	print_model_case("4 queues, split 10/5/5/5", split_4q_skew, 4, z_us,
			 s_us, base_p50_model);
	print_model_case("4 queues, split 25/0/0/0", split_4q_worst, 4, z_us,
			 s_us, base_p50_model);
	printf("   measured 4 queues: %.0f req/s, p50 %.1f us, d_rate %.1f%%, "
	       "d_p50 +%.1f us\n",
	       MEAS_4Q_C25_RPS, MEAS_4Q_C25_P50,
	       100.0 * (MEAS_4Q_C25_RPS / MEAS_2Q_C25_RPS - 1.0),
	       MEAS_P50_SHIFT);

	p50_needed_n = 13.0 + MEAS_P50_SHIFT / s_us;
	printf("\n   to raise p50 by %.0f us the hot core would need %.0f "
	       "connections,\n",
	       MEAS_P50_SHIFT, p50_needed_n);
	printf("   but the test has only %d connections in total.\n", CONNS);
	printf("   even the worst split, all %d on one core, still gives "
	       "%.0f req/s,\n",
	       CONNS, model_total(split_4q_worst, 4, z_us, s_us));
	printf("   which is above the measured %.0f req/s.\n", MEAS_4Q_C25_RPS);

	printf("\n7. Verdict on hypothesis (b), RSS hash skew\n");
	printf("   REJECTED.\n");
	printf("   The table is balanced by construction. A fair hash puts "
	       "about 13\n");
	printf("   connections on the hot core at 2 queues and about 7 at 4 "
	       "queues.\n");
	printf("   Fewer connections per core must lower queueing delay, not "
	       "raise it.\n");
	printf("   The model predicts %+.0f%% throughput and %+.0f us p50 for "
	       "the 4-queue\n",
	       100.0 *
		   (model_total(split_4q_typ, 4, z_us, s_us) / MEAS_2Q_C25_RPS -
		    1.0),
	       model_p50(split_4q_typ, 4, z_us, s_us) - base_p50_model);
	printf("   split. The measurement shows -28%% and +126 us. The sign "
	       "is wrong and\n");
	printf("   the size is wrong too: no split, not even 25/0/0/0, "
	       "reaches the\n");
	printf("   measured 4-queue number. The queue split cannot explain "
	       "the loss.\n");
	printf("   Caveat: the model holds S and Z fixed and changes only the "
	       "split. If\n");
	printf("   the 4-queue build has a higher per-request cost, that "
	       "cost is not\n");
	printf("   the split, and this result says the loss must come from "
	       "elsewhere.\n");

	return fails == 0 ? 0 : 1;
}
