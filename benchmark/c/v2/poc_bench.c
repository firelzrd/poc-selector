// SPDX-License-Identifier: GPL-2.0
/*
 * poc_bench.c - POC Selector Performance Benchmark
 *
 * Measures scheduling wakeup latency with the POC (Piece-Of-Cake) idle CPU
 * selector enabled vs disabled, providing real-time terminal visualization
 * of latency changes when toggling.
 *
 * Workers perform rapid nanosleep cycles to stress the select_idle_sibling()
 * path that POC optimizes. Wakeup latency is measured as the difference
 * between actual elapsed time and requested sleep duration.
 *
 * Requires root for toggling /proc/sys/kernel/sched_poc_selector.
 *
 * Usage:
 *   sudo ./poc_bench --mode ab --duration 60
 *   sudo ./poc_bench --mode auto-toggle --interval 5
 *   sudo ./poc_bench --mode manual
 *
 * Copyright (C) 2026 Masahito Suzuki
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <termios.h>
#include <sys/prctl.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define POC_BENCH_VERSION	"1.0.0"

#define RING_CAPACITY		(1 << 16)	/* 65536 samples per worker */
#define RING_MASK		(RING_CAPACITY - 1)

#define DEFAULT_WORKERS		0		/* 0 = auto (2 * nprocs) */
#define DEFAULT_DURATION_SEC	60
#define DEFAULT_TOGGLE_SEC	5
#define DEFAULT_SLEEP_US	50
#define DEFAULT_WARMUP_SEC	3
#define DEFAULT_WINDOW_MS	1000

#define TOGGLE_GRACE_MS		100		/* discard samples around toggle */

#define HIST_BUCKETS		8
#define MAX_BAR_WIDTH		40

#define MAX_CSTATES		8

#define SYSCTL_POC_PATH		"/proc/sys/kernel/sched_poc_selector"
#define SYSCTL_POC_L2_PATH	"/proc/sys/kernel/sched_poc_l2_cluster_search"
#define SYSFS_STATUS_ACTIVE	"/sys/kernel/poc_selector/status/active"
#define SYSFS_STATUS_VERSION	"/sys/kernel/poc_selector/status/version"
#define SYSFS_COUNTER_HIT	"/sys/kernel/poc_selector/counters/hit"
#define SYSFS_COUNTER_FALL	"/sys/kernel/poc_selector/counters/fallthrough"
#define SYSFS_COUNTER_L2	"/sys/kernel/poc_selector/counters/l2_hit"
#define SYSFS_COUNTER_LLC	"/sys/kernel/poc_selector/counters/llc_hit"
#define SYSFS_COUNTER_RESET	"/sys/kernel/poc_selector/counters/reset"

/* ANSI escape codes */
#define ANSI_RESET	"\033[0m"
#define ANSI_BOLD	"\033[1m"
#define ANSI_DIM	"\033[2m"
#define ANSI_GREEN	"\033[32m"
#define ANSI_RED	"\033[31m"
#define ANSI_YELLOW	"\033[33m"
#define ANSI_CYAN	"\033[36m"
#define ANSI_CLEAR	"\033[2J"
#define ANSI_HOME	"\033[H"
#define ANSI_ERASE_LINE	"\033[2K"
#define ANSI_HIDE_CURSOR "\033[?25l"
#define ANSI_SHOW_CURSOR "\033[?25h"

/* ============================================================
 * Data structures
 * ============================================================ */

enum bench_mode {
	MODE_AB = 0,
	MODE_AUTO_TOGGLE,
	MODE_MANUAL,
};

struct wakeup_sample {
	uint64_t latency_ns;
	uint64_t timestamp_ns;
	uint16_t cpu_before;
	uint16_t cpu_after;
};

struct sample_ring {
	struct wakeup_sample	buf[RING_CAPACITY];
	atomic_uint_fast64_t	head __attribute__((aligned(64)));
	atomic_uint_fast64_t	tail __attribute__((aligned(64)));
};

struct window_stats {
	uint64_t count;
	uint64_t min_ns;
	uint64_t max_ns;
	uint64_t sum_ns;
	uint64_t p50_ns;
	uint64_t p95_ns;
	uint64_t p99_ns;
	uint64_t p999_ns;
	double   stddev_ns;
	int      poc_state;
	uint64_t timestamp;		/* seconds since start */
	uint64_t wakeups_per_sec;
	uint64_t migrations;		/* samples where cpu_before != cpu_after */
	double   migration_pct;		/* migrations / count * 100 */
	/* Per-category stats: same-CPU vs migrated */
	uint64_t same_count;
	uint64_t same_p50_ns, same_p95_ns, same_p99_ns;
	uint64_t migr_count;
	uint64_t migr_p50_ns, migr_p95_ns, migr_p99_ns;
};

struct poc_counters {
	uint64_t hit;
	uint64_t fallthrough;
	uint64_t l2_hit;
	uint64_t llc_hit;
};

struct worker_ctx {
	int              id;
	pthread_t        thread;
	struct sample_ring *ring;
	atomic_int       should_stop;
};

struct bench_config {
	int		nr_workers;
	int		nr_cpus;
	int		duration_sec;
	int		toggle_interval_sec;
	int		sleep_ns;
	int		warmup_sec;
	int		window_ms;
	enum bench_mode	mode;
	bool		has_debug_counters;
	bool		no_viz;
	bool		csv_output;
	int		max_cstate;		/* -1 = no limit */
	long		timer_slack_ns;		/* -1 = system default */
	bool		spin_wait;		/* busy-wait instead of nanosleep */
};

/* Histogram boundaries (nanoseconds): 0-500, 500-1k, 1k-2k, 2k-4k, 4k-8k, 8k-16k, 16k-32k, >32k */
static const uint64_t hist_bounds[HIST_BUCKETS] = {
	500, 1000, 2000, 4000, 8000, 16000, 32000, UINT64_MAX
};

static const char *hist_labels[HIST_BUCKETS] = {
	"  0-0.5us", "0.5-1.0us", "1.0-2.0us", "2.0-4.0us",
	"4.0-8.0us", " 8.0-16us", "  16-32us ", "    >32us "
};

/* ============================================================
 * Globals
 * ============================================================ */

static volatile sig_atomic_t g_should_stop;
static int g_original_poc_state = -1;
static struct bench_config g_cfg;
static struct worker_ctx *g_workers;
static struct termios g_orig_termios;
static bool g_termios_saved;

/* Aggregated stats for final report */
#define MAX_WINDOWS	3600
static struct window_stats g_history[MAX_WINDOWS];
static int g_history_count;

/* Toggle grace period tracking */
static atomic_uint_fast64_t g_toggle_timestamp_ns;

/* ============================================================
 * Timing helpers
 * ============================================================ */

static inline uint64_t time_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void ns_to_timespec(uint64_t ns, struct timespec *ts)
{
	ts->tv_sec = ns / 1000000000ULL;
	ts->tv_nsec = ns % 1000000000ULL;
}

/* ============================================================
 * SPSC ring buffer (lock-free, single-producer single-consumer)
 * ============================================================ */

static struct sample_ring *ring_alloc(void)
{
	struct sample_ring *r = calloc(1, sizeof(*r));
	if (!r) {
		perror("calloc ring");
		exit(1);
	}
	atomic_store(&r->head, 0);
	atomic_store(&r->tail, 0);
	return r;
}

static inline bool ring_push(struct sample_ring *r, const struct wakeup_sample *s)
{
	uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
	if (h - t >= RING_CAPACITY)
		return false; /* full, drop sample */
	r->buf[h & RING_MASK] = *s;
	atomic_store_explicit(&r->head, h + 1, memory_order_release);
	return true;
}

static inline bool ring_pop(struct sample_ring *r, struct wakeup_sample *s)
{
	uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
	if (t >= h)
		return false; /* empty */
	*s = r->buf[t & RING_MASK];
	atomic_store_explicit(&r->tail, t + 1, memory_order_release);
	return true;
}

/* ============================================================
 * sysctl / sysfs helpers
 * ============================================================ */

static int sysfs_read_int(const char *path)
{
	char buf[64];
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return atoi(buf);
}

static uint64_t sysfs_read_u64(const char *path)
{
	char buf[64];
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return strtoull(buf, NULL, 10);
}

static int sysfs_read_str(const char *path, char *out, size_t sz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	int n = read(fd, out, sz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	out[n] = '\0';
	/* strip trailing newline */
	if (n > 0 && out[n - 1] == '\n')
		out[n - 1] = '\0';
	return 0;
}

static int sysfs_write_int(const char *path, int val)
{
	char buf[16];
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int n = snprintf(buf, sizeof(buf), "%d", val);
	int ret = (write(fd, buf, n) == n) ? 0 : -1;
	close(fd);
	return ret;
}

static int poc_get_enabled(void)
{
	return sysfs_read_int(SYSCTL_POC_PATH);
}

static int poc_set_enabled(int val)
{
	return sysfs_write_int(SYSCTL_POC_PATH, val);
}

static bool poc_debug_counters_available(void)
{
	return access(SYSFS_COUNTER_HIT, R_OK) == 0;
}

static struct poc_counters poc_read_counters(void)
{
	struct poc_counters c = {0};
	c.hit = sysfs_read_u64(SYSFS_COUNTER_HIT);
	c.fallthrough = sysfs_read_u64(SYSFS_COUNTER_FALL);
	c.l2_hit = sysfs_read_u64(SYSFS_COUNTER_L2);
	c.llc_hit = sysfs_read_u64(SYSFS_COUNTER_LLC);
	return c;
}

static void poc_reset_counters(void)
{
	sysfs_write_int(SYSFS_COUNTER_RESET, 1);
}

/* ============================================================
 * cpuidle C-state monitoring
 * ============================================================ */

struct cstate_info {
	char name[16];
	int  latency_us;
};

struct cpuidle_snapshot {
	uint64_t usage[MAX_CSTATES];	/* sum of all CPUs' usage per state */
};

static int g_nr_cstates;
static struct cstate_info g_cstates[MAX_CSTATES];

static void cpuidle_detect(int nr_cpus)
{
	char path[256];
	g_nr_cstates = 0;
	for (int s = 0; s < MAX_CSTATES; s++) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cpuidle/state%d/name", s);
		if (sysfs_read_str(path, g_cstates[s].name,
				   sizeof(g_cstates[s].name)) < 0)
			break;
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cpuidle/state%d/latency", s);
		g_cstates[s].latency_us = sysfs_read_int(path);
		g_nr_cstates++;
	}
	(void)nr_cpus;
}

static struct cpuidle_snapshot cpuidle_read(int nr_cpus)
{
	struct cpuidle_snapshot snap = {0};
	char path[256];
	for (int s = 0; s < g_nr_cstates; s++) {
		for (int c = 0; c < nr_cpus; c++) {
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/usage",
				 c, s);
			snap.usage[s] += sysfs_read_u64(path);
		}
	}
	return snap;
}

static void cpuidle_delta(const struct cpuidle_snapshot *before,
			  const struct cpuidle_snapshot *after,
			  uint64_t *out_delta)
{
	for (int s = 0; s < g_nr_cstates; s++)
		out_delta[s] = after->usage[s] - before->usage[s];
}

/* ============================================================
 * C-state limiting (for diagnosis)
 * ============================================================ */

static int  g_orig_cstate_disable[MAX_CSTATES];	/* per-state from cpu0 */
static bool g_cstate_limited;

static void cstate_limit_apply(int max_cstate, int nr_cpus)
{
	char path[256];

	/* Save original disable values from cpu0 */
	for (int s = 0; s < g_nr_cstates; s++) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu0/cpuidle/state%d/disable", s);
		g_orig_cstate_disable[s] = sysfs_read_int(path);
	}

	/* Disable states > max_cstate on all CPUs */
	for (int c = 0; c < nr_cpus; c++) {
		for (int s = 0; s < g_nr_cstates; s++) {
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable",
				 c, s);
			sysfs_write_int(path, (s > max_cstate) ? 1 : 0);
		}
	}
	g_cstate_limited = true;
}

static void cstate_limit_restore(int nr_cpus)
{
	char path[256];

	if (!g_cstate_limited)
		return;
	for (int c = 0; c < nr_cpus; c++) {
		for (int s = 0; s < g_nr_cstates; s++) {
			if (g_orig_cstate_disable[s] < 0)
				continue;
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/disable",
				 c, s);
			sysfs_write_int(path, g_orig_cstate_disable[s]);
		}
	}
	g_cstate_limited = false;
}

/* ============================================================
 * Worker thread
 * ============================================================ */

static void *worker_func(void *arg)
{
	struct worker_ctx *ctx = arg;
	struct timespec req;
	ns_to_timespec(g_cfg.sleep_ns, &req);

	if (g_cfg.timer_slack_ns >= 0)
		prctl(PR_SET_TIMERSLACK, (unsigned long)g_cfg.timer_slack_ns);

	while (!atomic_load_explicit(&ctx->should_stop, memory_order_relaxed)) {
		int cpu_before = sched_getcpu();
		uint64_t before = time_now_ns();
		if (g_cfg.spin_wait) {
			uint64_t deadline = before + (uint64_t)g_cfg.sleep_ns;
			while (time_now_ns() < deadline)
				;  /* busy-wait: no sleep, no scheduler, no hrtimer */
		} else {
			nanosleep(&req, NULL);
		}
		uint64_t after = time_now_ns();
		int cpu_after = sched_getcpu();

		uint64_t elapsed = after - before;
		uint64_t latency = (elapsed > (uint64_t)g_cfg.sleep_ns) ?
				   elapsed - (uint64_t)g_cfg.sleep_ns : 0;

		struct wakeup_sample sample = {
			.latency_ns = latency,
			.timestamp_ns = after,
			.cpu_before = (uint16_t)cpu_before,
			.cpu_after = (uint16_t)cpu_after,
		};
		ring_push(ctx->ring, &sample);
	}
	return NULL;
}

/* ============================================================
 * Statistics engine
 * ============================================================ */

static int cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

static void compute_stats(uint64_t *samples, int n, struct window_stats *out)
{
	memset(out, 0, sizeof(*out));
	if (n == 0)
		return;

	qsort(samples, n, sizeof(uint64_t), cmp_u64);

	out->count = n;
	out->min_ns = samples[0];
	out->max_ns = samples[n - 1];
	out->p50_ns = samples[n * 50 / 100];
	out->p95_ns = samples[n * 95 / 100];
	out->p99_ns = samples[n * 99 / 100];
	out->p999_ns = samples[(uint64_t)n * 999 / 1000];

	double sum = 0.0, sum_sq = 0.0;
	for (int i = 0; i < n; i++) {
		double v = (double)samples[i];
		sum += v;
		sum_sq += v * v;
	}
	out->sum_ns = (uint64_t)sum;
	double mean = sum / n;
	double variance = sum_sq / n - mean * mean;
	out->stddev_ns = (variance > 0.0) ? sqrt(variance) : 0.0;
}

static void compute_histogram(uint64_t *samples, int n, uint64_t *hist_out)
{
	memset(hist_out, 0, sizeof(uint64_t) * HIST_BUCKETS);
	for (int i = 0; i < n; i++) {
		for (int b = 0; b < HIST_BUCKETS; b++) {
			if (samples[i] <= hist_bounds[b]) {
				hist_out[b]++;
				break;
			}
		}
	}
}

/* ============================================================
 * Terminal visualization
 * ============================================================ */

static void format_ns(uint64_t ns, char *buf, size_t sz)
{
	if (ns < 1000)
		snprintf(buf, sz, "%llu ns", (unsigned long long)ns);
	else if (ns < 1000000)
		snprintf(buf, sz, "%.1f us", ns / 1000.0);
	else
		snprintf(buf, sz, "%.2f ms", ns / 1000000.0);
}

static void print_header(int poc_state, uint64_t elapsed_sec)
{
	int minutes = elapsed_sec / 60;
	int seconds = elapsed_sec % 60;
	const char *mode_str;
	switch (g_cfg.mode) {
	case MODE_AB:		mode_str = "ab"; break;
	case MODE_AUTO_TOGGLE:	mode_str = "auto-toggle"; break;
	case MODE_MANUAL:	mode_str = "manual"; break;
	default:		mode_str = "?"; break;
	}

	printf(ANSI_BOLD "POC Bench v%s" ANSI_RESET " | POC: ", POC_BENCH_VERSION);
	if (poc_state > 0)
		printf(ANSI_GREEN ANSI_BOLD "[ON ]" ANSI_RESET);
	else if (poc_state == 0)
		printf(ANSI_RED ANSI_BOLD "[OFF]" ANSI_RESET);
	else
		printf(ANSI_YELLOW "[???]" ANSI_RESET);
	printf(" | Workers: %d | %d:%02d elapsed\n", g_cfg.nr_workers, minutes, seconds);

	printf("Mode: %s", mode_str);
	if (g_cfg.mode == MODE_AUTO_TOGGLE)
		printf(" (%ds)", g_cfg.toggle_interval_sec);
	printf(" | CPUs: %d | Sleep: %dus", g_cfg.nr_cpus, g_cfg.sleep_ns / 1000);
	if (g_cfg.max_cstate >= 0)
		printf(" | " ANSI_YELLOW "max-cstate=%d" ANSI_RESET, g_cfg.max_cstate);
	if (g_cfg.timer_slack_ns >= 0)
		printf(" | " ANSI_YELLOW "slack=%ldns" ANSI_RESET, g_cfg.timer_slack_ns);
	if (g_cfg.spin_wait)
		printf(" | " ANSI_YELLOW "SPIN" ANSI_RESET);
	if (g_cfg.mode == MODE_MANUAL)
		printf(" | Press " ANSI_BOLD "t" ANSI_RESET " to toggle, " ANSI_BOLD "q" ANSI_RESET " to quit");
	printf("\n");
}

static void print_table_header(void)
{
	printf(ANSI_DIM "%-6s %10s %10s %10s %10s  %-3s %10s %6s" ANSI_RESET "\n",
	       "Time", "p50", "p95", "p99", "max", "POC", "Wakeups/s", "Migr%");
}

static void print_window_row(const struct window_stats *w)
{
	char p50[16], p95[16], p99[16], maxs[16];
	format_ns(w->p50_ns, p50, sizeof(p50));
	format_ns(w->p95_ns, p95, sizeof(p95));
	format_ns(w->p99_ns, p99, sizeof(p99));
	format_ns(w->max_ns, maxs, sizeof(maxs));

	const char *color = w->poc_state ? ANSI_GREEN : ANSI_RED;
	const char *state = w->poc_state ? "ON " : "OFF";

	int minutes = w->timestamp / 60;
	int seconds = w->timestamp % 60;

	printf("%s%02d:%02d  %10s %10s %10s %10s  %s  %10llu %5.1f%%" ANSI_RESET "\n",
	       color, minutes, seconds,
	       p50, p95, p99, maxs, state,
	       (unsigned long long)w->wakeups_per_sec,
	       w->migration_pct);
}

static void print_toggle_marker(int new_state)
{
	const char *color = new_state ? ANSI_GREEN : ANSI_RED;
	const char *state = new_state ? "ON" : "OFF";
	printf("%s --- POC toggled %s --- " ANSI_RESET "\n", color, state);
}

static void print_csv_header(void)
{
	printf("timestamp,count,min_ns,p50_ns,p95_ns,p99_ns,p999_ns,max_ns,avg_ns,stddev_ns,poc_state,wakeups_per_sec,migrations,migration_pct,same_count,same_p50,same_p95,same_p99,migr_count,migr_p50,migr_p95,migr_p99\n");
}

static void print_csv_row(const struct window_stats *w)
{
	uint64_t avg = (w->count > 0) ? w->sum_ns / w->count : 0;
	printf("%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.1f,%d,%llu,%llu,%.1f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
	       (unsigned long long)w->timestamp,
	       (unsigned long long)w->count,
	       (unsigned long long)w->min_ns,
	       (unsigned long long)w->p50_ns,
	       (unsigned long long)w->p95_ns,
	       (unsigned long long)w->p99_ns,
	       (unsigned long long)w->p999_ns,
	       (unsigned long long)w->max_ns,
	       (unsigned long long)avg,
	       w->stddev_ns,
	       w->poc_state,
	       (unsigned long long)w->wakeups_per_sec,
	       (unsigned long long)w->migrations,
	       w->migration_pct,
	       (unsigned long long)w->same_count,
	       (unsigned long long)w->same_p50_ns,
	       (unsigned long long)w->same_p95_ns,
	       (unsigned long long)w->same_p99_ns,
	       (unsigned long long)w->migr_count,
	       (unsigned long long)w->migr_p50_ns,
	       (unsigned long long)w->migr_p95_ns,
	       (unsigned long long)w->migr_p99_ns);
}

/* ============================================================
 * Final report
 * ============================================================ */

struct aggregate_stats {
	uint64_t total_samples;
	uint64_t total_sum;
	uint64_t total_migrations;
	uint64_t min_p50, max_p50;
	double   avg_p50, avg_p95, avg_p99, avg_p999, avg_max;
	double   avg_stddev;
	double   avg_migration_pct;
	double   avg_same_p50, avg_same_p95, avg_same_p99;
	double   avg_migr_p50, avg_migr_p95, avg_migr_p99;
	int      windows;
	int      migr_windows;	/* windows that had any migrations */
};

static struct aggregate_stats aggregate_windows(int poc_state)
{
	struct aggregate_stats a = {0};
	a.min_p50 = UINT64_MAX;
	a.max_p50 = 0;
	double sum_p50 = 0, sum_p95 = 0, sum_p99 = 0, sum_p999 = 0;
	double sum_max = 0, sum_stddev = 0, sum_migpct = 0;
	double sum_sp50 = 0, sum_sp95 = 0, sum_sp99 = 0;
	double sum_mp50 = 0, sum_mp95 = 0, sum_mp99 = 0;

	for (int i = 0; i < g_history_count; i++) {
		struct window_stats *w = &g_history[i];
		if (w->poc_state != poc_state || w->count == 0)
			continue;
		a.windows++;
		a.total_samples += w->count;
		a.total_sum += w->sum_ns;
		a.total_migrations += w->migrations;
		if (w->p50_ns < a.min_p50) a.min_p50 = w->p50_ns;
		if (w->p50_ns > a.max_p50) a.max_p50 = w->p50_ns;
		sum_p50 += (double)w->p50_ns;
		sum_p95 += (double)w->p95_ns;
		sum_p99 += (double)w->p99_ns;
		sum_p999 += (double)w->p999_ns;
		sum_max += (double)w->max_ns;
		sum_stddev += w->stddev_ns;
		sum_migpct += w->migration_pct;
		if (w->same_count > 0) {
			sum_sp50 += (double)w->same_p50_ns;
			sum_sp95 += (double)w->same_p95_ns;
			sum_sp99 += (double)w->same_p99_ns;
		}
		if (w->migr_count > 0) {
			sum_mp50 += (double)w->migr_p50_ns;
			sum_mp95 += (double)w->migr_p95_ns;
			sum_mp99 += (double)w->migr_p99_ns;
			a.migr_windows++;
		}
	}

	if (a.windows > 0) {
		a.avg_p50 = sum_p50 / a.windows;
		a.avg_p95 = sum_p95 / a.windows;
		a.avg_p99 = sum_p99 / a.windows;
		a.avg_p999 = sum_p999 / a.windows;
		a.avg_max = sum_max / a.windows;
		a.avg_stddev = sum_stddev / a.windows;
		a.avg_migration_pct = sum_migpct / a.windows;
		a.avg_same_p50 = sum_sp50 / a.windows;
		a.avg_same_p95 = sum_sp95 / a.windows;
		a.avg_same_p99 = sum_sp99 / a.windows;
	}
	if (a.migr_windows > 0) {
		a.avg_migr_p50 = sum_mp50 / a.migr_windows;
		a.avg_migr_p95 = sum_mp95 / a.migr_windows;
		a.avg_migr_p99 = sum_mp99 / a.migr_windows;
	}
	return a;
}

static void print_delta(double on_val, double off_val)
{
	if (off_val == 0.0) {
		printf("%10s", "N/A");
		return;
	}
	double pct = (on_val - off_val) / off_val * 100.0;
	const char *color = (pct < 0) ? ANSI_GREEN : ANSI_RED;
	printf("%s%+9.1f%%%s", color, pct, ANSI_RESET);
}

static void print_report_row(const char *label, double on_val, double off_val)
{
	char on_str[32], off_str[32];
	format_ns((uint64_t)on_val, on_str, sizeof(on_str));
	format_ns((uint64_t)off_val, off_str, sizeof(off_str));
	printf("  %-18s %12s %12s  ", label, on_str, off_str);
	print_delta(on_val, off_val);
	printf("\n");
}

static void print_final_report(void)
{
	struct aggregate_stats on = aggregate_windows(1);
	struct aggregate_stats off = aggregate_windows(0);

	if (on.windows == 0 && off.windows == 0) {
		printf("\nNo measurement data collected.\n");
		return;
	}

	char version[64] = "N/A";
	sysfs_read_str(SYSFS_STATUS_VERSION, version, sizeof(version));

	struct utsname {
		char sysname[65], nodename[65], release[65];
		char version[65], machine[65];
	};

	printf("\n" ANSI_BOLD "═══════════════════════════════════════════════════════════════\n");
	printf(" POC Selector Benchmark Report\n");
	printf("═══════════════════════════════════════════════════════════════" ANSI_RESET "\n");
	printf("  POC Version:    %s\n", version);
	printf("  CPUs:           %d\n", g_cfg.nr_cpus);
	printf("  Workers:        %d\n", g_cfg.nr_workers);
	printf("  Duration:       %ds", g_cfg.duration_sec);
	if (on.windows > 0 && off.windows > 0)
		printf(" (%ds on + %ds off)", on.windows, off.windows);
	printf("\n");
	printf("  Sleep interval: %dus\n", g_cfg.sleep_ns / 1000);
	printf("  Window:         %dms\n", g_cfg.window_ms);
	if (g_cfg.max_cstate >= 0)
		printf("  Max C-state:    %d\n", g_cfg.max_cstate);
	if (g_cfg.timer_slack_ns >= 0)
		printf("  Timer slack:    %ld ns\n", g_cfg.timer_slack_ns);
	if (g_cfg.spin_wait)
		printf("  Wait method:    spin (busy-wait)\n");

	if (on.windows > 0 && off.windows > 0) {
		printf("\n" ANSI_BOLD "%-20s %12s %12s %11s" ANSI_RESET "\n",
		       "", "POC ON", "POC OFF", "Delta");
		printf("  ────────────────────────────────────────────────────────\n");
		printf("  %-18s %12llu %12llu\n", "Samples",
		       (unsigned long long)on.total_samples,
		       (unsigned long long)off.total_samples);
		printf("  %-18s %12d %12d\n", "Windows",
		       on.windows, off.windows);
		print_report_row("Avg p50 latency", on.avg_p50, off.avg_p50);
		print_report_row("Avg p95 latency", on.avg_p95, off.avg_p95);
		print_report_row("Avg p99 latency", on.avg_p99, off.avg_p99);
		print_report_row("Avg p99.9 latency", on.avg_p999, off.avg_p999);
		print_report_row("Avg max latency", on.avg_max, off.avg_max);

		double on_avg = (on.total_samples > 0) ?
				(double)on.total_sum / on.total_samples : 0;
		double off_avg = (off.total_samples > 0) ?
				 (double)off.total_sum / off.total_samples : 0;
		print_report_row("Mean latency", on_avg, off_avg);
		print_report_row("Avg stddev", on.avg_stddev, off.avg_stddev);
		printf("  ────────────────────────────────────────────────────────\n");
		printf("  %-18s %11.1f%% %11.1f%%  ",
		       "Avg migration %", on.avg_migration_pct, off.avg_migration_pct);
		print_delta(on.avg_migration_pct, off.avg_migration_pct);
		printf("\n");
		printf("  %-18s %12llu %12llu\n", "Total migrations",
		       (unsigned long long)on.total_migrations,
		       (unsigned long long)off.total_migrations);

		printf("\n" ANSI_BOLD "  %-18s %12s %12s %11s" ANSI_RESET "\n",
		       "Same-CPU", "POC ON", "POC OFF", "Delta");
		printf("  ────────────────────────────────────────────────────────\n");
		print_report_row("  p50 latency", on.avg_same_p50, off.avg_same_p50);
		print_report_row("  p95 latency", on.avg_same_p95, off.avg_same_p95);
		print_report_row("  p99 latency", on.avg_same_p99, off.avg_same_p99);

		if (on.migr_windows > 0 || off.migr_windows > 0) {
			printf("\n" ANSI_BOLD "  %-18s %12s %12s %11s" ANSI_RESET "\n",
			       "Migrated", "POC ON", "POC OFF", "Delta");
			printf("  ────────────────────────────────────────────────────────\n");
			print_report_row("  p50 latency", on.avg_migr_p50, off.avg_migr_p50);
			print_report_row("  p95 latency", on.avg_migr_p95, off.avg_migr_p95);
			print_report_row("  p99 latency", on.avg_migr_p99, off.avg_migr_p99);
		}
	} else {
		struct aggregate_stats *s = (on.windows > 0) ? &on : &off;
		const char *state = (on.windows > 0) ? "ON" : "OFF";
		printf("\n  POC %s only:\n", state);
		printf("  Samples: %llu, Windows: %d\n",
		       (unsigned long long)s->total_samples, s->windows);
		char buf[32];
		format_ns((uint64_t)s->avg_p50, buf, sizeof(buf));
		printf("  Avg p50: %s\n", buf);
		format_ns((uint64_t)s->avg_p95, buf, sizeof(buf));
		printf("  Avg p95: %s\n", buf);
		format_ns((uint64_t)s->avg_p99, buf, sizeof(buf));
		printf("  Avg p99: %s\n", buf);
		printf("  Avg migration: %.1f%%\n", s->avg_migration_pct);
	}
}

/* ============================================================
 * Terminal raw mode for manual key input
 * ============================================================ */

static void terminal_raw_enable(void)
{
	if (!isatty(STDIN_FILENO))
		return;
	if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
		g_termios_saved = true;
		struct termios raw = g_orig_termios;
		raw.c_lflag &= ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
}

static void terminal_raw_disable(void)
{
	if (g_termios_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static int read_keypress(void)
{
	char ch;
	if (read(STDIN_FILENO, &ch, 1) == 1)
		return ch;
	return -1;
}

/* ============================================================
 * Signal handling
 * ============================================================ */

static void sig_handler(int sig)
{
	(void)sig;
	g_should_stop = 1;
}

static void cleanup(void)
{
	cstate_limit_restore(g_cfg.nr_cpus);
	if (g_original_poc_state >= 0)
		poc_set_enabled(g_original_poc_state);
	terminal_raw_disable();
	printf(ANSI_SHOW_CURSOR);
	fflush(stdout);
}

/* ============================================================
 * Command-line parsing
 * ============================================================ */

static void usage(const char *progname)
{
	printf("Usage: %s [OPTIONS]\n\n", progname);
	printf("POC Selector Performance Benchmark\n\n");
	printf("Modes:\n");
	printf("  --mode ab              A/B comparison (default)\n");
	printf("  --mode auto-toggle     Toggle POC at regular intervals\n");
	printf("  --mode manual          Toggle POC with 't' key\n\n");
	printf("Options:\n");
	printf("  -w, --workers N        Worker threads (default: 2*ncpus)\n");
	printf("  -d, --duration N       Duration in seconds (default: %d)\n", DEFAULT_DURATION_SEC);
	printf("  -i, --interval N       Auto-toggle interval sec (default: %d)\n", DEFAULT_TOGGLE_SEC);
	printf("  -s, --sleep N          Nanosleep duration in us (default: %d)\n", DEFAULT_SLEEP_US);
	printf("  -W, --warmup N         Warmup seconds (default: %d)\n", DEFAULT_WARMUP_SEC);
	printf("      --window N         Stats window in ms (default: %d)\n", DEFAULT_WINDOW_MS);
	printf("      --max-cstate N     Limit deepest C-state (default: no limit)\n");
	printf("      --timer-slack N    Set timer slack in ns (0 = minimum, default: system)\n");
	printf("      --spin             Use busy-wait instead of nanosleep (default: nanosleep)\n");
	printf("      --no-viz           Disable terminal visualization\n");
	printf("      --csv              CSV output format\n");
	printf("  -h, --help             Show this help\n\n");
	printf("Examples:\n");
	printf("  sudo %s --mode ab --duration 60\n", progname);
	printf("  sudo %s --mode auto-toggle --interval 3\n", progname);
	printf("  sudo %s --mode manual\n", progname);
	printf("  sudo %s --mode ab --no-viz --csv > results.csv\n", progname);
}

static void parse_args(int argc, char **argv)
{
	static struct option long_opts[] = {
		{"mode",	required_argument,	NULL, 'm'},
		{"workers",	required_argument,	NULL, 'w'},
		{"duration",	required_argument,	NULL, 'd'},
		{"interval",	required_argument,	NULL, 'i'},
		{"sleep",	required_argument,	NULL, 's'},
		{"warmup",	required_argument,	NULL, 'W'},
		{"window",	required_argument,	NULL, 0x100},
		{"max-cstate",	required_argument,	NULL, 0x103},
		{"timer-slack",	required_argument,	NULL, 0x104},
		{"spin",	no_argument,		NULL, 0x105},
		{"no-viz",	no_argument,		NULL, 0x101},
		{"csv",		no_argument,		NULL, 0x102},
		{"help",	no_argument,		NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	g_cfg.nr_workers = DEFAULT_WORKERS;
	g_cfg.duration_sec = DEFAULT_DURATION_SEC;
	g_cfg.toggle_interval_sec = DEFAULT_TOGGLE_SEC;
	g_cfg.sleep_ns = DEFAULT_SLEEP_US * 1000;
	g_cfg.warmup_sec = DEFAULT_WARMUP_SEC;
	g_cfg.window_ms = DEFAULT_WINDOW_MS;
	g_cfg.mode = MODE_AB;
	g_cfg.max_cstate = -1;
	g_cfg.timer_slack_ns = -1;
	g_cfg.spin_wait = false;
	g_cfg.nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	int c;
	while ((c = getopt_long(argc, argv, "m:w:d:i:s:W:h", long_opts, NULL)) != -1) {
		switch (c) {
		case 'm':
			if (strcmp(optarg, "ab") == 0)
				g_cfg.mode = MODE_AB;
			else if (strcmp(optarg, "auto-toggle") == 0)
				g_cfg.mode = MODE_AUTO_TOGGLE;
			else if (strcmp(optarg, "manual") == 0)
				g_cfg.mode = MODE_MANUAL;
			else {
				fprintf(stderr, "Unknown mode: %s\n", optarg);
				exit(1);
			}
			break;
		case 'w':
			g_cfg.nr_workers = atoi(optarg);
			break;
		case 'd':
			g_cfg.duration_sec = atoi(optarg);
			break;
		case 'i':
			g_cfg.toggle_interval_sec = atoi(optarg);
			break;
		case 's':
			g_cfg.sleep_ns = atoi(optarg) * 1000;
			break;
		case 'W':
			g_cfg.warmup_sec = atoi(optarg);
			break;
		case 0x100:
			g_cfg.window_ms = atoi(optarg);
			break;
		case 0x101:
			g_cfg.no_viz = true;
			break;
		case 0x102:
			g_cfg.csv_output = true;
			g_cfg.no_viz = true;
			break;
		case 0x103:
			g_cfg.max_cstate = atoi(optarg);
			break;
		case 0x104:
			g_cfg.timer_slack_ns = atol(optarg);
			break;
		case 0x105:
			g_cfg.spin_wait = true;
			break;
		case 'h':
			usage(argv[0]);
			exit(0);
		default:
			usage(argv[0]);
			exit(1);
		}
	}

	if (g_cfg.nr_workers <= 0)
		g_cfg.nr_workers = g_cfg.nr_cpus * 2;
	if (g_cfg.nr_workers < 1)
		g_cfg.nr_workers = 1;
}

/* ============================================================
 * Main loop
 * ============================================================ */

struct drain_result {
	uint64_t *same_buf;	/* latencies where cpu_before == cpu_after */
	int       same_count;
	int       same_cap;
	uint64_t *migr_buf;	/* latencies where cpu_before != cpu_after */
	int       migr_count;
	int       migr_cap;
};

static void drain_init(struct drain_result *dr)
{
	memset(dr, 0, sizeof(*dr));
}

static void drain_free(struct drain_result *dr)
{
	free(dr->same_buf);
	free(dr->migr_buf);
	memset(dr, 0, sizeof(*dr));
}

static void drain_append(uint64_t **buf, int *count, int *cap, uint64_t val)
{
	if (*count >= *cap) {
		*cap = *cap ? *cap * 2 : 65536;
		*buf = realloc(*buf, *cap * sizeof(uint64_t));
		if (!*buf) {
			perror("realloc");
			exit(1);
		}
	}
	(*buf)[(*count)++] = val;
}

static void drain_samples(struct drain_result *dr, uint64_t grace_until_ns)
{
	dr->same_count = 0;
	dr->migr_count = 0;

	for (int i = 0; i < g_cfg.nr_workers; i++) {
		struct wakeup_sample s;
		while (ring_pop(g_workers[i].ring, &s)) {
			if (s.timestamp_ns < grace_until_ns)
				continue;
			if (s.cpu_before != s.cpu_after)
				drain_append(&dr->migr_buf, &dr->migr_count,
					     &dr->migr_cap, s.latency_ns);
			else
				drain_append(&dr->same_buf, &dr->same_count,
					     &dr->same_cap, s.latency_ns);
		}
	}
}

int main(int argc, char **argv)
{
	parse_args(argc, argv);

	/* Verify POC sysctl exists */
	int poc_state = poc_get_enabled();
	if (poc_state < 0) {
		fprintf(stderr, "Error: Cannot read %s\n", SYSCTL_POC_PATH);
		fprintf(stderr, "Is the kernel compiled with CONFIG_SCHED_POC_SELECTOR?\n");
		return 1;
	}

	/* Check root for toggling */
	if (geteuid() != 0) {
		fprintf(stderr, "Error: Root privileges required for toggling POC selector.\n");
		fprintf(stderr, "Run with: sudo %s\n", argv[0]);
		return 1;
	}

	/* Save original state */
	g_original_poc_state = poc_state;
	atexit(cleanup);

	/* Signal handlers */
	struct sigaction sa = {.sa_handler = sig_handler};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* Detect debug counters */
	g_cfg.has_debug_counters = poc_debug_counters_available();

	/* Detect cpuidle states */
	cpuidle_detect(g_cfg.nr_cpus);

	/* Apply C-state limit if requested */
	if (g_cfg.max_cstate >= 0) {
		if (g_nr_cstates == 0) {
			fprintf(stderr, "Warning: No cpuidle states detected, "
				"--max-cstate ignored.\n");
		} else {
			cstate_limit_apply(g_cfg.max_cstate, g_cfg.nr_cpus);
			fprintf(stderr, "C-state limited to max state %d",
				g_cfg.max_cstate);
			for (int s = 0; s <= g_cfg.max_cstate && s < g_nr_cstates; s++)
				fprintf(stderr, " %s", g_cstates[s].name);
			fprintf(stderr, " (states %d-%d disabled)\n",
				g_cfg.max_cstate + 1, g_nr_cstates - 1);
		}
	}

	/* Allocate workers */
	g_workers = calloc(g_cfg.nr_workers, sizeof(struct worker_ctx));
	if (!g_workers) {
		perror("calloc workers");
		return 1;
	}
	for (int i = 0; i < g_cfg.nr_workers; i++) {
		g_workers[i].id = i;
		g_workers[i].ring = ring_alloc();
		atomic_store(&g_workers[i].should_stop, 0);
	}

	/* Sample buffers for stats (same-CPU and migrated, separate) */
	struct drain_result dr;
	drain_init(&dr);
	uint64_t hist_same[HIST_BUCKETS], hist_migr[HIST_BUCKETS];

	/* Terminal setup */
	bool is_tty = isatty(STDOUT_FILENO);
	if (g_cfg.no_viz)
		is_tty = false;
	if (g_cfg.csv_output)
		is_tty = false;

	if (g_cfg.mode == MODE_MANUAL && isatty(STDIN_FILENO))
		terminal_raw_enable();

	if (is_tty) {
		printf(ANSI_HIDE_CURSOR);
		printf(ANSI_CLEAR ANSI_HOME);
	}
	if (g_cfg.csv_output)
		print_csv_header();

	/* Start workers */
	for (int i = 0; i < g_cfg.nr_workers; i++) {
		if (pthread_create(&g_workers[i].thread, NULL, worker_func, &g_workers[i]) != 0) {
			perror("pthread_create");
			return 1;
		}
	}

	/* Warmup */
	if (!g_cfg.csv_output && !g_cfg.no_viz)
		printf("Warming up for %d seconds...\n", g_cfg.warmup_sec);
	{
		struct timespec warmup_ts;
		ns_to_timespec((uint64_t)g_cfg.warmup_sec * 1000000000ULL, &warmup_ts);
		nanosleep(&warmup_ts, NULL);
	}
	/* Drain warmup samples */
	drain_samples(&dr, UINT64_MAX);

	if (g_cfg.has_debug_counters)
		poc_reset_counters();

	/* cpuidle baseline snapshot */
	struct cpuidle_snapshot cidle_start = {0}, cidle_prev = {0};
	if (g_nr_cstates > 0) {
		cidle_start = cpuidle_read(g_cfg.nr_cpus);
		cidle_prev = cidle_start;
	}
	/* Accumulated cpuidle deltas per POC state */
	uint64_t cidle_on_total[MAX_CSTATES] = {0};
	uint64_t cidle_off_total[MAX_CSTATES] = {0};

	uint64_t start_ns = time_now_ns();
	uint64_t window_ns = (uint64_t)g_cfg.window_ms * 1000000ULL;
	uint64_t toggle_ns = (uint64_t)g_cfg.toggle_interval_sec * 1000000000ULL;
	uint64_t duration_ns = (uint64_t)g_cfg.duration_sec * 1000000000ULL;
	uint64_t grace_ns = (uint64_t)TOGGLE_GRACE_MS * 1000000ULL;

	uint64_t next_window_ns = start_ns + window_ns;
	uint64_t next_toggle_ns = 0;
	uint64_t grace_until_ns = 0;
	atomic_store(&g_toggle_timestamp_ns, 0);

	/* Mode-specific setup */
	switch (g_cfg.mode) {
	case MODE_AB:
		/* Start with POC ON, toggle to OFF at halfway */
		poc_set_enabled(1);
		poc_state = 1;
		next_toggle_ns = start_ns + duration_ns / 2;
		break;
	case MODE_AUTO_TOGGLE:
		poc_state = poc_get_enabled();
		next_toggle_ns = start_ns + toggle_ns;
		break;
	case MODE_MANUAL:
		poc_state = poc_get_enabled();
		break;
	}

	/*
	 * max_table_rows: how many time-series rows fit on screen.
	 * Reserve lines for: header(2) + blank(1) + table_header(1)
	 *                   + histogram(HIST_BUCKETS+2) + debug(2) + margin(1)
	 */
	int max_table_rows = 20;

	struct poc_counters prev_counters = {0};
	if (g_cfg.has_debug_counters)
		prev_counters = poc_read_counters();

	/* Track toggle events for display */
#define MAX_TOGGLE_EVENTS 256
	struct toggle_event {
		uint64_t timestamp;	/* seconds since start */
		int      new_state;
	};
	struct toggle_event toggle_events[MAX_TOGGLE_EVENTS];
	int toggle_event_count = 0;

	/* Full-screen redraw function (is_tty mode) */
#define REDRAW_SCREEN() do { \
	printf(ANSI_HOME); \
	print_header(poc_state, \
		     (time_now_ns() - start_ns) / 1000000000ULL); \
	printf(ANSI_ERASE_LINE "\n"); \
	print_table_header(); \
	/* Determine which history rows to show */ \
	int _show_start = g_history_count - max_table_rows; \
	if (_show_start < 0) _show_start = 0; \
	int _row = 0; \
	for (int _i = _show_start; _i < g_history_count; _i++) { \
		/* Check for toggle event before this window */ \
		for (int _t = 0; _t < toggle_event_count; _t++) { \
			if (toggle_events[_t].timestamp == g_history[_i].timestamp) { \
				printf(ANSI_ERASE_LINE); \
				print_toggle_marker(toggle_events[_t].new_state); \
				_row++; \
			} \
		} \
		printf(ANSI_ERASE_LINE); \
		print_window_row(&g_history[_i]); \
		_row++; \
	} \
	/* Pad remaining table rows with blank lines */ \
	for (int _i = _row; _i < max_table_rows; _i++) \
		printf(ANSI_ERASE_LINE "\n"); \
	/* Histogram: same-CPU */ \
	compute_histogram(dr.same_buf, dr.same_count, hist_same); \
	{ \
		int _total_n = dr.same_count + dr.migr_count; \
		double _same_pct = _total_n ? 100.0 * dr.same_count / _total_n : 0; \
		printf(ANSI_ERASE_LINE "\n" ANSI_DIM "Same CPU (%.1f%%):" ANSI_RESET \
		       "                         " ANSI_DIM "Migrated (%.1f%%):" ANSI_RESET "\n", \
		       _same_pct, 100.0 - _same_pct); \
	} \
	compute_histogram(dr.migr_buf, dr.migr_count, hist_migr); \
	{ \
		uint64_t _max_s = 0, _max_m = 0; \
		for (int _i = 0; _i < HIST_BUCKETS; _i++) { \
			if (hist_same[_i] > _max_s) _max_s = hist_same[_i]; \
			if (hist_migr[_i] > _max_m) _max_m = hist_migr[_i]; \
		} \
		int _bw2 = MAX_BAR_WIDTH / 2 - 2; \
		for (int _i = 0; _i < HIST_BUCKETS; _i++) { \
			double _sp = dr.same_count ? 100.0 * hist_same[_i] / dr.same_count : 0; \
			double _mp = dr.migr_count ? 100.0 * hist_migr[_i] / dr.migr_count : 0; \
			int _sw = (_max_s > 0) ? (int)(_bw2 * hist_same[_i] / _max_s) : 0; \
			int _mw = (_max_m > 0) ? (int)(_bw2 * hist_migr[_i] / _max_m) : 0; \
			printf(ANSI_ERASE_LINE "  %s ", hist_labels[_i]); \
			for (int _j = 0; _j < _sw; _j++) printf("\u2588"); \
			for (int _j = _sw; _j < _bw2; _j++) printf(" "); \
			printf(" %5.1f%%  \u2502 ", _sp); \
			for (int _j = 0; _j < _mw; _j++) printf("\u2588"); \
			for (int _j = _mw; _j < _bw2; _j++) printf(" "); \
			printf(" %5.1f%%\n", _mp); \
		} \
	} \
	/* Debug counters */ \
	if (g_cfg.has_debug_counters) { \
		struct poc_counters _cur = poc_read_counters(); \
		uint64_t _d_hit = _cur.hit - prev_counters.hit; \
		uint64_t _d_fall = _cur.fallthrough - prev_counters.fallthrough; \
		uint64_t _d_l2 = _cur.l2_hit - prev_counters.l2_hit; \
		uint64_t _d_llc = _cur.llc_hit - prev_counters.llc_hit; \
		uint64_t _total = _d_hit + _d_fall; \
		double _hr = (_total > 0) ? 100.0 * _d_hit / _total : 0.0; \
		printf(ANSI_ERASE_LINE ANSI_DIM \
		       "  POC: hit=%llu fall=%llu (%.1f%%) l2=%llu llc=%llu" \
		       ANSI_RESET "\n", \
		       (unsigned long long)_d_hit, (unsigned long long)_d_fall, \
		       _hr, (unsigned long long)_d_l2, (unsigned long long)_d_llc); \
		prev_counters = _cur; \
	} \
	/* cpuidle C-state deltas */ \
	if (g_nr_cstates > 0) { \
		struct cpuidle_snapshot _ci_now = cpuidle_read(g_cfg.nr_cpus); \
		uint64_t _ci_d[MAX_CSTATES]; \
		cpuidle_delta(&cidle_prev, &_ci_now, _ci_d); \
		uint64_t _ci_sum = 0; \
		for (int _i = 0; _i < g_nr_cstates; _i++) _ci_sum += _ci_d[_i]; \
		printf(ANSI_ERASE_LINE ANSI_DIM "  C-state: "); \
		for (int _i = 0; _i < g_nr_cstates; _i++) { \
			double _cp = _ci_sum ? 100.0 * _ci_d[_i] / _ci_sum : 0; \
			printf("%s=%.1f%% ", g_cstates[_i].name, _cp); \
		} \
		printf(ANSI_RESET "\n"); \
		/* Accumulate into ON/OFF totals */ \
		for (int _i = 0; _i < g_nr_cstates; _i++) { \
			if (poc_state) \
				cidle_on_total[_i] += _ci_d[_i]; \
			else \
				cidle_off_total[_i] += _ci_d[_i]; \
		} \
		cidle_prev = _ci_now; \
	} \
	/* Clear any leftover lines below */ \
	printf(ANSI_ERASE_LINE); \
	fflush(stdout); \
} while (0)

	if (is_tty)
		printf(ANSI_CLEAR);

	/* ---- Main measurement loop ---- */
	while (!g_should_stop) {
		uint64_t now = time_now_ns();

		/* Check duration */
		if (now - start_ns >= duration_ns)
			break;

		/* Check manual key input */
		if (g_cfg.mode == MODE_MANUAL) {
			int key = read_keypress();
			if (key == 't' || key == 'T') {
				poc_state = !poc_state;
				poc_set_enabled(poc_state);
				grace_until_ns = now + grace_ns;
				atomic_store(&g_toggle_timestamp_ns, now);
				if (toggle_event_count < MAX_TOGGLE_EVENTS) {
					toggle_events[toggle_event_count].timestamp =
						(now - start_ns) / 1000000000ULL + 1;
					toggle_events[toggle_event_count].new_state = poc_state;
					toggle_event_count++;
				}
			} else if (key == 'q' || key == 'Q') {
				break;
			}
		}

		/* Check auto-toggle */
		if ((g_cfg.mode == MODE_AUTO_TOGGLE || g_cfg.mode == MODE_AB) &&
		    next_toggle_ns > 0 && now >= next_toggle_ns) {
			if (g_cfg.mode == MODE_AB) {
				poc_state = 0;
				poc_set_enabled(0);
				next_toggle_ns = 0;
			} else {
				poc_state = !poc_state;
				poc_set_enabled(poc_state);
				next_toggle_ns = now + toggle_ns;
			}
			grace_until_ns = now + grace_ns;
			atomic_store(&g_toggle_timestamp_ns, now);
			if (toggle_event_count < MAX_TOGGLE_EVENTS) {
				toggle_events[toggle_event_count].timestamp =
					(now - start_ns) / 1000000000ULL + 1;
				toggle_events[toggle_event_count].new_state = poc_state;
				toggle_event_count++;
			}
		}

		/* Window stats collection */
		if (now >= next_window_ns) {
			drain_samples(&dr, grace_until_ns);

			int total_count = dr.same_count + dr.migr_count;

			/* Compute overall stats by merging both arrays */
			/* Build a merged latency array for overall percentiles */
			uint64_t *merged = NULL;
			if (total_count > 0) {
				merged = malloc(total_count * sizeof(uint64_t));
				if (merged) {
					if (dr.same_count > 0)
						memcpy(merged, dr.same_buf,
						       dr.same_count * sizeof(uint64_t));
					if (dr.migr_count > 0)
						memcpy(merged + dr.same_count, dr.migr_buf,
						       dr.migr_count * sizeof(uint64_t));
				}
			}

			struct window_stats w;
			compute_stats(merged, total_count, &w);
			w.poc_state = poc_state;
			w.timestamp = (now - start_ns) / 1000000000ULL;
			w.wakeups_per_sec = (uint64_t)total_count * 1000 / g_cfg.window_ms;
			w.migrations = dr.migr_count;
			w.migration_pct = (total_count > 0) ?
					  100.0 * dr.migr_count / total_count : 0.0;

			/* Same-CPU stats */
			w.same_count = dr.same_count;
			if (dr.same_count > 0) {
				struct window_stats tmp;
				compute_stats(dr.same_buf, dr.same_count, &tmp);
				w.same_p50_ns = tmp.p50_ns;
				w.same_p95_ns = tmp.p95_ns;
				w.same_p99_ns = tmp.p99_ns;
			}

			/* Migrated stats */
			w.migr_count = dr.migr_count;
			if (dr.migr_count > 0) {
				struct window_stats tmp;
				compute_stats(dr.migr_buf, dr.migr_count, &tmp);
				w.migr_p50_ns = tmp.p50_ns;
				w.migr_p95_ns = tmp.p95_ns;
				w.migr_p99_ns = tmp.p99_ns;
			}

			free(merged);

			/* Save to history */
			if (g_history_count < MAX_WINDOWS)
				g_history[g_history_count++] = w;

			/* Display */
			if (g_cfg.csv_output) {
				print_csv_row(&w);
				fflush(stdout);
			} else if (is_tty) {
				REDRAW_SCREEN();
			} else if (!g_cfg.no_viz) {
				char p50s[32], p99s[32];
				format_ns(w.p50_ns, p50s, sizeof(p50s));
				format_ns(w.p99_ns, p99s, sizeof(p99s));
				printf("[%3llus] POC=%s  p50=%s  p99=%s  migr=%.1f%%  wakeups=%llu/s\n",
				       (unsigned long long)w.timestamp,
				       poc_state ? "ON " : "OFF",
				       p50s, p99s,
				       w.migration_pct,
				       (unsigned long long)w.wakeups_per_sec);
				fflush(stdout);
			}

			next_window_ns = now + window_ns;
		}

		/* Sleep until next event */
		{
			uint64_t sleep_until = next_window_ns;
			if ((g_cfg.mode == MODE_AUTO_TOGGLE || g_cfg.mode == MODE_AB) &&
			    next_toggle_ns > 0 && next_toggle_ns < sleep_until)
				sleep_until = next_toggle_ns;

			uint64_t now2 = time_now_ns();
			if (sleep_until > now2) {
				uint64_t wait = sleep_until - now2;
				if (g_cfg.mode == MODE_MANUAL && wait > 50000000ULL)
					wait = 50000000ULL; /* 50ms max for key polling */
				struct timespec ts;
				ns_to_timespec(wait, &ts);
				nanosleep(&ts, NULL);
			}
		}
	}

	/* Stop workers */
	for (int i = 0; i < g_cfg.nr_workers; i++)
		atomic_store(&g_workers[i].should_stop, 1);
	for (int i = 0; i < g_cfg.nr_workers; i++)
		pthread_join(g_workers[i].thread, NULL);

	/* Terminal cleanup */
	if (is_tty) {
		printf(ANSI_CLEAR ANSI_HOME);
		printf(ANSI_SHOW_CURSOR);
	}

	/* Final report */
	print_final_report();

	/* Print debug counter summary if available */
	if (g_cfg.has_debug_counters) {
		struct poc_counters final = poc_read_counters();
		uint64_t total = final.hit + final.fallthrough;
		printf("\n  POC Debug Counter Summary (total):\n");
		printf("    Hit:         %12llu\n", (unsigned long long)final.hit);
		printf("    Fallthrough: %12llu\n", (unsigned long long)final.fallthrough);
		if (total > 0)
			printf("    Hit rate:    %11.1f%%\n", 100.0 * final.hit / total);
		printf("    L2 hit:      %12llu\n", (unsigned long long)final.l2_hit);
		printf("    LLC hit:     %12llu\n", (unsigned long long)final.llc_hit);
	}

	/* C-state usage comparison */
	if (g_nr_cstates > 0) {
		uint64_t on_sum = 0, off_sum = 0;
		for (int i = 0; i < g_nr_cstates; i++) {
			on_sum += cidle_on_total[i];
			off_sum += cidle_off_total[i];
		}
		printf("\n  C-state Entry Distribution:\n");
		printf("  %-8s %8s %8s  %8s %8s\n",
		       "State", "POC ON", "%", "POC OFF", "%");
		printf("  ────────────────────────────────────────────\n");
		for (int i = 0; i < g_nr_cstates; i++) {
			double on_pct = on_sum ? 100.0 * cidle_on_total[i] / on_sum : 0;
			double off_pct = off_sum ? 100.0 * cidle_off_total[i] / off_sum : 0;
			printf("  %-4s(%3dus) %8llu %6.1f%%  %8llu %6.1f%%\n",
			       g_cstates[i].name,
			       g_cstates[i].latency_us,
			       (unsigned long long)cidle_on_total[i], on_pct,
			       (unsigned long long)cidle_off_total[i], off_pct);
		}
	}

	printf("\n");

	/* Free resources */
	for (int i = 0; i < g_cfg.nr_workers; i++)
		free(g_workers[i].ring);
	free(g_workers);
	drain_free(&dr);

	return 0;
}
