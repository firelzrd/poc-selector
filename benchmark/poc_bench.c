// SPDX-License-Identifier: GPL-2.0
/*
 * POC Selector Microbenchmark
 *
 * Demonstrates the effect of the Piece-Of-Cake (POC) fast idle CPU selector
 * by measuring wakeup latency with partial CPU saturation (background load
 * forces select_idle_cpu() to scan past busy CPUs, where POC's TZCNT fast
 * path provides the largest measurable improvement).
 *
 * Usage:
 *   sudo ./poc_bench [-i ITERS] [-t THREADS] [-b BACKGROUND]
 *                    [-w WARMUP] [--no-compare]
 *
 * Copyright (C) 2026 — for use with BORE scheduler + POC Selector
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <getopt.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define DEFAULT_ITERATIONS  100000
#define DEFAULT_WARMUP      5000
#define COMPARE_ROUNDS      3
#define SYSCTL_PATH         "/proc/sys/kernel/sched_poc_selector"
#define NS_PER_US           1000ULL
#define NS_PER_SEC          1000000000ULL

/* ------------------------------------------------------------------ */
/*  Utility: high-resolution timing                                    */
/* ------------------------------------------------------------------ */

static inline uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/*  Utility: statistics                                                */
/* ------------------------------------------------------------------ */

struct stats {
	uint64_t *samples;
	int       n;
};

static int cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;
	return (va > vb) - (va < vb);
}

struct stat_result {
	double   mean;
	double   stddev;
	uint64_t min;
	uint64_t max;
	uint64_t p50;
	uint64_t p99;
};

static struct stat_result stats_compute(struct stats *s)
{
	struct stat_result r = {0};
	if (s->n == 0)
		return r;

	qsort(s->samples, s->n, sizeof(uint64_t), cmp_u64);

	r.min = s->samples[0];
	r.max = s->samples[s->n - 1];
	r.p50 = s->samples[s->n / 2];
	r.p99 = s->samples[(int)((s->n - 1) * 0.99)];

	double sum = 0;
	for (int i = 0; i < s->n; i++)
		sum += (double)s->samples[i];
	r.mean = sum / s->n;

	double var = 0;
	for (int i = 0; i < s->n; i++) {
		double d = (double)s->samples[i] - r.mean;
		var += d * d;
	}
	r.stddev = sqrt(var / s->n);

	return r;
}

static struct stat_result stats_merge(struct stat_result *results, int n)
{
	struct stat_result m = {0};
	if (n == 0)
		return m;

	double sum_mean = 0, sum_stddev_sq = 0;
	uint64_t global_min = UINT64_MAX, global_max = 0;
	double sum_p50 = 0, sum_p99 = 0;

	for (int i = 0; i < n; i++) {
		sum_mean += results[i].mean;
		sum_stddev_sq += results[i].stddev * results[i].stddev;
		if (results[i].min < global_min)
			global_min = results[i].min;
		if (results[i].max > global_max)
			global_max = results[i].max;
		sum_p50 += (double)results[i].p50;
		sum_p99 += (double)results[i].p99;
	}

	m.mean = sum_mean / n;
	m.stddev = sqrt(sum_stddev_sq / n);
	m.min = global_min;
	m.max = global_max;
	m.p50 = (uint64_t)(sum_p50 / n);
	m.p99 = (uint64_t)(sum_p99 / n);

	return m;
}

static void stats_print(const char *label, struct stat_result *r, int n_iters)
{
	double ops_sec = (double)n_iters / (r->mean * n_iters / 1e9);
	printf("  %-8s  mean: %8.1f ns  p50: %7lu ns  p99: %7lu ns  "
	       "min: %7lu ns  max: %7lu ns  stddev: %7.1f ns  [%.0f ops/s]\n",
	       label, r->mean,
	       (unsigned long)r->p50, (unsigned long)r->p99,
	       (unsigned long)r->min, (unsigned long)r->max,
	       r->stddev, ops_sec);
}

static void print_comparison(struct stat_result *on, struct stat_result *off,
			     int n_iters)
{
	double ops_on  = 1e9 / on->mean;
	double ops_off = 1e9 / off->mean;

	printf("  %-18s %12s %12s\n", "", "POC ON", "POC OFF");
	printf("  %-18s %10.1f ns %10.1f ns\n", "mean",
	       on->mean, off->mean);
	printf("  %-18s %10lu ns %10lu ns\n", "p50",
	       (unsigned long)on->p50, (unsigned long)off->p50);
	printf("  %-18s %10lu ns %10lu ns\n", "p99",
	       (unsigned long)on->p99, (unsigned long)off->p99);
	printf("  %-18s %10.0f    %10.0f     %+.1f%%\n", "ops/sec",
	       ops_on, ops_off, (ops_on - ops_off) / ops_off * 100);
}

/* ------------------------------------------------------------------ */
/*  sysctl helpers                                                     */
/* ------------------------------------------------------------------ */

static int poc_selector_read(void)
{
	FILE *f = fopen(SYSCTL_PATH, "r");
	if (!f)
		return -1;
	int val = -1;
	if (fscanf(f, "%d", &val) != 1)
		val = -1;
	fclose(f);
	return val;
}

static int poc_selector_write(int val)
{
	FILE *f = fopen(SYSCTL_PATH, "w");
	if (!f)
		return -1;
	fprintf(f, "%d\n", val);
	fclose(f);
	/* Allow kernel state to settle */
	usleep(50000);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Benchmark: Burst wakeup with background CPU load                   */
/* ------------------------------------------------------------------ */

static atomic_int bg_stop;

static void *bg_burn_fn(void *arg)
{
	int cpu = *(int *)arg;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);

	/* Busy loop */
	while (!atomic_load_explicit(&bg_stop, memory_order_relaxed)) {
		for (volatile int i = 0; i < 10000; i++)
			;
	}
	return NULL;
}

struct burst_worker_ctx {
	int       iterations;
	int       warmup;
	int       efd;
	uint64_t *latencies;
	uint64_t *ts_wake;
	atomic_int ready;
};

static void *burst_worker_fn(void *arg)
{
	struct burst_worker_ctx *w = arg;
	int total = w->warmup + w->iterations;

	atomic_store(&w->ready, 1);

	for (int i = 0; i < total; i++) {
		uint64_t val;
		if (read(w->efd, &val, sizeof(val)) != (ssize_t)sizeof(val))
			break;
		uint64_t t1 = now_ns();
		uint64_t t0 = __atomic_load_n(&w->ts_wake[i], __ATOMIC_ACQUIRE);
		if (i >= w->warmup)
			w->latencies[i - w->warmup] = t1 - t0;
		/* Brief computation to simulate real work */
		volatile int x = 0;
		for (int j = 0; j < 100; j++)
			x += j;
	}
	return NULL;
}

static struct stat_result bench_burst(int n_workers, int n_background,
				      int iterations, int warmup)
{
	int ncpus = get_nprocs();
	int total = warmup + iterations;

	/* Clamp background threads to available CPUs */
	if (n_background > ncpus - 1)
		n_background = ncpus - 1;
	if (n_background < 0)
		n_background = 0;

	/* Start background load threads pinned to specific CPUs */
	atomic_store(&bg_stop, 0);
	int *bg_cpus = calloc(n_background, sizeof(int));
	pthread_t *bg_threads = calloc(n_background, sizeof(pthread_t));
	if (!bg_cpus || !bg_threads) {
		perror("calloc");
		exit(1);
	}
	for (int i = 0; i < n_background; i++) {
		bg_cpus[i] = i;  /* Pin to CPUs 0..n_background-1 */
		pthread_create(&bg_threads[i], NULL, bg_burn_fn, &bg_cpus[i]);
	}

	/* Let background threads settle */
	usleep(50000);

	/* Start worker threads (not pinned — let scheduler choose) */
	struct burst_worker_ctx *workers = calloc(n_workers, sizeof(*workers));
	pthread_t *worker_threads = calloc(n_workers, sizeof(pthread_t));
	if (!workers || !worker_threads) {
		perror("calloc");
		exit(1);
	}
	for (int i = 0; i < n_workers; i++) {
		workers[i].efd = eventfd(0, EFD_SEMAPHORE);
		if (workers[i].efd < 0) {
			perror("eventfd");
			exit(1);
		}
		workers[i].iterations = iterations;
		workers[i].warmup = warmup;
		workers[i].latencies = calloc(iterations, sizeof(uint64_t));
		workers[i].ts_wake = calloc(total, sizeof(uint64_t));
		atomic_init(&workers[i].ready, 0);
		if (!workers[i].latencies || !workers[i].ts_wake) {
			perror("calloc");
			exit(1);
		}
		pthread_create(&worker_threads[i], NULL, burst_worker_fn,
			       &workers[i]);
	}

	for (int i = 0; i < n_workers; i++)
		while (!atomic_load(&workers[i].ready))
			usleep(100);

	usleep(10000);

	/* Dispatch wakeups */
	uint64_t wval = 1;
	for (int i = 0; i < total; i++) {
		uint64_t t0 = now_ns();
		for (int w = 0; w < n_workers; w++) {
			__atomic_store_n(&workers[w].ts_wake[i], t0,
					 __ATOMIC_RELEASE);
			if (write(workers[w].efd, &wval, sizeof(wval)) !=
			    (ssize_t)sizeof(wval))
				break;
		}
		struct timespec ts = { .tv_nsec = 1000 };
		nanosleep(&ts, NULL);
	}

	for (int i = 0; i < n_workers; i++)
		pthread_join(worker_threads[i], NULL);

	/* Stop background load */
	atomic_store(&bg_stop, 1);
	for (int i = 0; i < n_background; i++)
		pthread_join(bg_threads[i], NULL);

	/* Aggregate */
	int n_total = iterations * n_workers;
	uint64_t *all = calloc(n_total, sizeof(uint64_t));
	if (!all) {
		perror("calloc");
		exit(1);
	}
	int idx = 0;
	for (int w = 0; w < n_workers; w++)
		for (int i = 0; i < iterations; i++)
			all[idx++] = workers[w].latencies[i];

	struct stats st = { .samples = all, .n = n_total };
	struct stat_result r = stats_compute(&st);

	for (int i = 0; i < n_workers; i++) {
		close(workers[i].efd);
		free(workers[i].latencies);
		free(workers[i].ts_wake);
	}
	free(workers);
	free(worker_threads);
	free(bg_cpus);
	free(bg_threads);
	free(all);
	return r;
}

/* ------------------------------------------------------------------ */
/*  Runner: execute a scenario with POC ON/OFF comparison              */
/* ------------------------------------------------------------------ */

struct run_config {
	const char *name;
	int iterations;
	int warmup;
	int n_threads;
	int n_background;
	bool compare;
};

static void run_scenario(const char *title, struct run_config *cfg,
			 struct stat_result (*fn)(struct run_config *))
{
	int orig_poc = poc_selector_read();

	if (!cfg->compare || orig_poc < 0) {
		printf("\n--- %s (%d iterations, %d warmup) ---\n", title,
		       cfg->iterations, cfg->warmup);
		if (orig_poc < 0)
			printf("  [sysctl not available — running single measurement]\n");
		struct stat_result r = fn(cfg);
		stats_print("result", &r, cfg->iterations);
		return;
	}

	/* Verify sysctl is writable */
	if (poc_selector_write(orig_poc) < 0) {
		printf("\n--- %s (%d iterations, %d warmup) ---\n", title,
		       cfg->iterations, cfg->warmup);
		printf("  [cannot toggle sysctl (need root?) — running single measurement]\n");
		struct stat_result r = fn(cfg);
		stats_print("result", &r, cfg->iterations);
		return;
	}

	int total_rounds = COMPARE_ROUNDS + 1;  /* +1 for discard */
	printf("\n--- %s (%d iters x %d rounds, %d warmup) ---\n", title,
	       cfg->iterations, COMPARE_ROUNDS, cfg->warmup);

	struct stat_result results_on[COMPARE_ROUNDS];
	struct stat_result results_off[COMPARE_ROUNDS];

	for (int round = 0; round < total_rounds; round++) {
		bool on_first = (round % 2 == 0);
		int order[2] = { on_first ? 1 : 0, on_first ? 0 : 1 };
		const char *order_str = on_first ? "ON->OFF" : "OFF->ON";

		if (round == 0)
			printf("  Discard round (system warmup)...\n");
		else
			printf("  Round %d (%s)...\n", round, order_str);

		struct stat_result phase[2];
		for (int ph = 0; ph < 2; ph++) {
			poc_selector_write(order[ph]);
			phase[ph] = fn(cfg);
		}

		if (round > 0) {
			int idx = round - 1;
			struct stat_result *r_on, *r_off;
			if (on_first) {
				r_on  = &phase[0];
				r_off = &phase[1];
			} else {
				r_on  = &phase[1];
				r_off = &phase[0];
			}
			results_on[idx]  = *r_on;
			results_off[idx] = *r_off;
			printf("    ON mean=%8.1f ns, OFF mean=%8.1f ns\n",
			       r_on->mean, r_off->mean);
		}
	}

	/* Aggregate across rounds */
	struct stat_result r_on  = stats_merge(results_on,  COMPARE_ROUNDS);
	struct stat_result r_off = stats_merge(results_off, COMPARE_ROUNDS);

	/* Restore original setting */
	poc_selector_write(orig_poc);

	printf("\n");
	stats_print("POC ON", &r_on, cfg->iterations);
	stats_print("POC OFF", &r_off, cfg->iterations);
	printf("\n");
	print_comparison(&r_on, &r_off, cfg->iterations);
}

static struct stat_result run_burst(struct run_config *cfg)
{
	return bench_burst(cfg->n_threads, cfg->n_background,
			   cfg->iterations, cfg->warmup);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"  -i, --iterations <N>                    Iterations (default: %d)\n"
		"  -t, --threads <N>                       Worker threads (default: nproc)\n"
		"  -b, --background <N>                    Background threads (default: nproc/2)\n"
		"  -w, --warmup <N>                        Warmup iterations (default: %d)\n"
		"      --no-compare                        Skip POC ON/OFF comparison\n"
		"  -h, --help                              Show this help\n",
		prog, DEFAULT_ITERATIONS, DEFAULT_WARMUP);
}

int main(int argc, char *argv[])
{
	int ncpus = get_nprocs();
	int iterations = DEFAULT_ITERATIONS;
	int n_threads = ncpus;
	int n_background = ncpus / 2;
	int warmup = DEFAULT_WARMUP;
	bool compare = true;

	static struct option long_opts[] = {
		{"iterations", required_argument, NULL, 'i'},
		{"threads",    required_argument, NULL, 't'},
		{"background", required_argument, NULL, 'b'},
		{"warmup",     required_argument, NULL, 'w'},
		{"no-compare", no_argument,       NULL, 'C'},
		{"help",       no_argument,       NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "i:t:b:w:h", long_opts,
				  NULL)) != -1) {
		switch (opt) {
		case 'i': iterations = atoi(optarg); break;
		case 't': n_threads = atoi(optarg); break;
		case 'b': n_background = atoi(optarg); break;
		case 'w': warmup = atoi(optarg); break;
		case 'C': compare = false; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	/* Lock memory to avoid page-fault noise */
	mlockall(MCL_CURRENT | MCL_FUTURE);

	printf("=== POC Selector Microbenchmark ===\n");
	printf("System: %d CPUs\n", ncpus);

	int poc_val = poc_selector_read();
	if (poc_val >= 0)
		printf("sched_poc_selector: %d\n", poc_val);
	else
		printf("sched_poc_selector: not available (kernel may lack POC support)\n");

	struct run_config cfg = {
		.iterations   = iterations,
		.warmup       = warmup,
		.n_threads    = n_threads,
		.n_background = n_background,
		.compare      = compare,
	};

	run_scenario("Burst with Background Load", &cfg, run_burst);

	printf("\nDone.\n");
	return 0;
}
