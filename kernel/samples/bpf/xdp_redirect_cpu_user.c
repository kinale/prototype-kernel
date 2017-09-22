/* GPLv2 Copyright(c) 2017 Jesper Dangaard Brouer, Red Hat, Inc.
 */
static const char *__doc__=
 " XDP redirect with a CPU-map type \"BPF_MAP_TYPE_CPUMAP\" (EXPERIMENTAL)";

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <sys/resource.h>
#include <getopt.h>
#include <net/if.h>
#include <time.h>

#include <arpa/inet.h>
#include <linux/if_link.h>

#define MAX_CPUS 12 /* WARNING - sync with _kern.c */

/* Wanted to get rid of bpf_load.h and fake-"libbpf.h" (and instead
 * use bpf/libbpf.h), but cannot as (currently) needed for XDP
 * attaching to a device via set_link_xdp_fd()
 */
#include "libbpf.h"
#include "bpf_load.h"

#include "bpf_util.h"

static int ifindex = -1;
static char ifname_buf[IF_NAMESIZE];
static char *ifname = NULL;
static __u32 xdp_flags = 0;

/* Exit return codes */
#define EXIT_OK			0
#define EXIT_FAIL		1
#define EXIT_FAIL_OPTION	2
#define EXIT_FAIL_XDP		3
#define EXIT_FAIL_BPF		4
#define EXIT_FAIL_MEM		5

static const struct option long_options[] = {
	{"help",	no_argument,		NULL, 'h' },
	{"dev",		required_argument,	NULL, 'd' },
	{"skb-mode", 	no_argument,		NULL, 'S' },
	{"debug",	no_argument,		NULL, 'D' },
	{"sec", 	required_argument,	NULL, 's' },
	{"prognum", 	required_argument,	NULL, 'p' },
	{"qsize", 	required_argument,	NULL, 'q' },
	{0, 0, NULL,  0 }
};

static void int_exit(int sig)
{
	fprintf(stderr,
		"Interrupted: Removing XDP program on ifindex:%d device:%s\n",
		ifindex, ifname);
	if (ifindex > -1)
		set_link_xdp_fd(ifindex, -1, xdp_flags);
	exit(EXIT_OK);
}

static void usage(char *argv[])
{
	int i;
	printf("\nDOCUMENTATION:\n%s\n", __doc__);
	printf("\n");
	printf(" Usage: %s (options-see-below)\n",
	       argv[0]);
	printf(" Listing options:\n");
	for (i = 0; long_options[i].name != 0; i++) {
		printf(" --%-12s", long_options[i].name);
		if (long_options[i].flag != NULL)
			printf(" flag (internal value:%d)",
			       *long_options[i].flag);
		else
			printf(" short-option: -%c",
			       long_options[i].val);
		printf("\n");
	}
	printf("\n");
}

/* gettime returns the current time of day in nanoseconds.
 * Cost: clock_gettime (ns) => 26ns (CLOCK_MONOTONIC)
 *       clock_gettime (ns) =>  9ns (CLOCK_MONOTONIC_COARSE)
 */
#define NANOSEC_PER_SEC 1000000000 /* 10^9 */
__u64 gettime(void)
{
	struct timespec t;
	int res;

	res = clock_gettime(CLOCK_MONOTONIC, &t);
	if (res < 0) {
		fprintf(stderr, "Error with gettimeofday! (%i)\n", res);
		exit(EXIT_FAIL);
	}
	return (__u64) t.tv_sec * NANOSEC_PER_SEC + t.tv_nsec;
}

struct datarec {
	__u64 processed;
	__u64 dropped;
};
struct record {
	__u64 timestamp;
	struct datarec total;
	struct datarec *cpu;
};
struct stats_record {
	struct record rx_cnt;
	struct record redir_err;
	struct record kthread;
	struct record enq[MAX_CPUS];
};

static bool map_collect_percpu(int fd, __u32 key, struct record* rec)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = bpf_num_possible_cpus();
	struct datarec values[nr_cpus];
	__u64 sum_processed = 0;
	__u64 sum_dropped = 0;
	int i;

	if ((bpf_map_lookup_elem(fd, &key, values)) != 0) {
		fprintf(stderr,
			"ERR: bpf_map_lookup_elem failed key:0x%X\n", key);
		return false;
	}
	/* Get time as close as possible to reading map contents */
	rec->timestamp = gettime();

	/* Record and sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		rec->cpu[i].processed = values[i].processed;
		sum_processed        += values[i].processed;
		rec->cpu[i].dropped = values[i].dropped;
		sum_dropped        += values[i].dropped;
	}
	rec->total.processed = sum_processed;
	rec->total.dropped   = sum_dropped;
	return true;
}

struct datarec *alloc_record_per_cpu(void)
{
	unsigned int nr_cpus = bpf_num_possible_cpus();
	struct datarec *array;
	size_t size;

	size = sizeof(struct datarec) * nr_cpus;
	array = malloc(size);
	memset(array, 0, size);
	if (!array) {
		fprintf(stderr, "Mem alloc error (nr_cpus:%u)\n", nr_cpus);
		exit(EXIT_FAIL_MEM);
	}
	return array;
}

struct stats_record* alloc_stats_record(void)
{
	struct stats_record* rec;
	int i;

	rec = malloc(sizeof(*rec));
	memset(rec, 0, sizeof(*rec));
	if (!rec) {
		fprintf(stderr, "Mem alloc error\n");
		exit(EXIT_FAIL_MEM);
	}
	rec->rx_cnt.cpu    = alloc_record_per_cpu();
	rec->redir_err.cpu = alloc_record_per_cpu();
	rec->kthread.cpu   = alloc_record_per_cpu();
	for (i = 0; i < MAX_CPUS; i++)
		rec->enq[i].cpu = alloc_record_per_cpu();

	return rec;
}

void free_stats_record(struct stats_record* r)
{
	int i;

	for (i = 0; i < MAX_CPUS; i++)
		free(r->enq[i].cpu);
	free(r->kthread.cpu);
	free(r->redir_err.cpu);
	free(r->rx_cnt.cpu);
	free(r);
}

static double calc_period(struct record *r, struct record *p)
{
	double period_ = 0;
	__u64 period  = 0;

	period = r->timestamp - p->timestamp;
	if (period > 0) {
		period_ = ((double) period / NANOSEC_PER_SEC);
	}
	return period_;
}

static __u64 calc_pps(struct datarec *r, struct datarec *p, double period_)
{
	__u64 packets = 0;
	__u64 pps = 0;

	if (period_ > 0) {
		packets = r->processed - p->processed;
		pps = packets / period_;
	}
	return pps;
}

static __u64 calc_drop_pps(struct datarec *r, struct datarec *p, double period_)
{
	__u64 packets = 0;
	__u64 pps = 0;

	if (period_ > 0) {
		packets = r->dropped - p->dropped;
		pps = packets / period_;
	}
	return pps;
}

static void stats_print(struct stats_record *stats_rec,
			struct stats_record *stats_prev)
{
	unsigned int nr_cpus = bpf_num_possible_cpus();
	struct record *rec, *prev;
	double pps = 0, drop = 0;
	int to_cpu;
	double t;
	int i;

	/* Header */
	printf("%-15s %-7s %-10s %-18s %-12s %-9s\n",
	       "XDP-cpumap", "CPU:to", "pps ", "pps-human-readable",
	       "drop-pps", "period");

	/* XDP rx_cnt */
	rec  = &stats_rec->rx_cnt;
	prev = &stats_prev->rx_cnt;
	t = calc_period(rec, prev);
	for (i = 0; i < nr_cpus; i++) {
		struct datarec *r = &rec->cpu[i];
		struct datarec *p = &prev->cpu[i];
		pps = calc_pps(r, p, t);
		if (pps > 0)
			printf("%-15s %-7d %-10.0f %'-18.0f %-12s %f\n",
			       "XDP-RX", i, pps, pps, "(nan)", t);
	}
	pps = calc_pps(&rec->total, &prev->total, t);
	printf("%-15s %-7s %-10.0f %'-18.0f %-12s %f\n",
	       "XDP-RX", "total", pps, pps, "(nan)", t);

	/* cpumap enqueue stats */
	for (to_cpu = 0; to_cpu < MAX_CPUS; to_cpu++) {
		rec  =  &stats_rec->enq[to_cpu];
		prev = &stats_prev->enq[to_cpu];
		t = calc_period(rec, prev);
		for (i = 0; i < nr_cpus; i++) {
			struct datarec *r = &rec->cpu[i];
			struct datarec *p = &prev->cpu[i];
			pps = calc_pps(r, p, t);
			drop = calc_drop_pps(r, p, t);
			if (pps > 0)
			printf("%-15s %3d:%-3d %-10.0f %'-18.0f %'-12.0f %f\n",
			       "cpumap-enqueue", i, to_cpu, pps, pps, drop, t);
		}
		pps = calc_pps(&rec->total, &prev->total, t);
		if (pps > 0) {
			drop = calc_drop_pps(&rec->total, &prev->total, t);
			printf("%-15s %3s:%-3d %-10.0f %'-18.0f %'-12.0f %f\n",
			       "cpumap-enqueue", "sum", to_cpu,pps,pps,drop, t);
		}
	}

	/* cpumap kthread stats */
	rec  = &stats_rec->kthread;
	prev = &stats_prev->kthread;
	t = calc_period(rec, prev);
	for (i = 0; i < nr_cpus; i++) {
		struct datarec *r = &rec->cpu[i];
		struct datarec *p = &prev->cpu[i];
		pps  = calc_pps(r, p, t);
		drop = calc_drop_pps(r, p, t);
		if (pps > 0)
			printf("%-15s %-7d %-10.0f %'-18.0f %'-12.0f %f\n",
			       "cpumap_kthread", i, pps, pps, drop, t);
	}
	pps = calc_pps(&rec->total, &prev->total, t);
	drop = calc_drop_pps(&rec->total, &prev->total, t);
	printf("%-15s %-7s %-10.0f %'-18.0f %'-12.0f %f\n",
	       "cpumap_kthread", "total", pps, pps, drop, t);

	/* XDP redirect err tracepoints (very unlikely) */
	rec  = &stats_rec->redir_err;
	prev = &stats_prev->redir_err;
	t = calc_period(rec, prev);
	for (i = 0; i < nr_cpus; i++) {
		struct datarec *r = &rec->cpu[i];
		struct datarec *p = &prev->cpu[i];
		pps  = calc_pps(r, p, t);
		drop = calc_drop_pps(r, p, t);
		if (pps > 0)
			printf("%-15s %-7d %-10.0f %'-18.0f %'-12.0f %f\n",
			       "redirect_err", i, pps, pps, drop, t);
	}
	pps = calc_pps(&rec->total, &prev->total, t);
	drop = calc_drop_pps(&rec->total, &prev->total, t);
	printf("%-15s %-7s %-10.0f %'-18.0f %'-12.0f %f\n",
	       "redirect_err", "total", pps, pps, drop, t);

	printf("\n");
	fflush(stdout);
}

static void stats_collect(struct stats_record *rec)
{
	int fd, i;

	fd = map_fd[1]; /* map: rx_cnt */
	map_collect_percpu(fd, 0, &rec->rx_cnt);

	fd = map_fd[2]; /* map: redirect_err_cnt */
	map_collect_percpu(fd, 1, &rec->redir_err);

	fd = map_fd[3]; /* map: cpumap_enqueue_cnt */
	for (i = 0; i < MAX_CPUS; i++) {
		map_collect_percpu(fd, i, &rec->enq[i]);
	}

	fd = map_fd[4]; /* map: cpumap_kthread_cnt */
	map_collect_percpu(fd, 0, &rec->kthread);

}


/* Pointer swap trick */
static inline void swap(struct stats_record **a, struct stats_record **b)
{
	struct stats_record *tmp;

	tmp = *a;
	*a = *b;
	*b = tmp;
}

static void stats_poll(int interval)
{
	struct stats_record *record, *prev;

	record = alloc_stats_record();
	prev   = alloc_stats_record();
	stats_collect(record);

	/* Trick to pretty printf with thousands separators use %' */
	setlocale(LC_NUMERIC, "en_US");

	while (1) {
		swap(&prev, &record);
		stats_collect(record);
		stats_print(record, prev);
		sleep(interval);
	}

	free_stats_record(record);
	free_stats_record(prev);
}

int create_cpu_entry(__u32 cpu, __u32 queue_size)
{
	int ret;

	/* Add a CPU entry to map, as this allocate a cpu entry in
	 * the kernel for the cpu.
	 */
	ret = bpf_map_update_elem(map_fd[0], &cpu, &queue_size, 0);
	if (ret) {
		fprintf(stderr, "Create CPU entry failed\n");
		exit(EXIT_FAIL_BPF);
	}
	return 0;
}

/* How many xdp_progs are defined in _kern.c */
#define MAX_PROG 4

int main(int argc, char **argv)
{
	char filename[256];
	bool debug = false;
	int longindex = 0;
	int interval = 2;
	int prog_num = 0;
	__u32 qsize;
	int opt;

	/* Notice: choosing he queue size is very important with the
	 * ixgbe driver, because it's driver recycling trick is
	 * dependend on pages being returned quickly.  The number of
	 * out-standing packets in the system must be less-than 2x
	 * RX-ring size.
	 */
	qsize = 128+64;

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	/* Parse commands line args */
	while ((opt = getopt_long(argc, argv, "hSd:",
				  long_options, &longindex)) != -1) {
		switch (opt) {
		case 'd':
			if (strlen(optarg) >= IF_NAMESIZE) {
				fprintf(stderr, "ERR: --dev name too long\n");
				goto error;
			}
			ifname = (char *)&ifname_buf;
			strncpy(ifname, optarg, IF_NAMESIZE);
			ifindex = if_nametoindex(ifname);
			if (ifindex == 0) {
				fprintf(stderr,
					"ERR: --dev name unknown err(%d):%s\n",
					errno, strerror(errno));
				goto error;
			}
			break;
		case 's':
			interval = atoi(optarg);
			break;
		case 'S':
			xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'D':
			debug = true;
			break;
		case 'p':
			/* Selecting eBPF prog to load */
			prog_num = atoi(optarg);
			if (prog_num < 0 || prog_num >= MAX_PROG) {
				fprintf(stderr,
					"--prognum too large err(%d):%s\n",
					errno, strerror(errno));
				goto error;
			}
			break;
		case 'q':
			qsize = atoi(optarg);
			break;
		case 'h':
		error:
		default:
			usage(argv);
			return EXIT_FAIL_OPTION;
		}
	}
	/* Required options */
	if (ifindex == -1) {
		fprintf(stderr, "ERR: required option --dev missing");
		usage(argv);
		return EXIT_FAIL_OPTION;
	}

	if (load_bpf_file(filename)) {
		fprintf(stderr, "ERR in load_bpf_file(): %s", bpf_log_buf);
		return EXIT_FAIL;
	}

	if (!prog_fd[0]) {
		fprintf(stderr, "ERR: load_bpf_file: %s\n", strerror(errno));
		return EXIT_FAIL;
	}

	create_cpu_entry(0, qsize);
	create_cpu_entry(1, qsize);
	create_cpu_entry(2, qsize);
	create_cpu_entry(3, qsize);
	create_cpu_entry(4, qsize);

	/* Remove XDP program when program is interrupted */
	signal(SIGINT, int_exit);

	if (set_link_xdp_fd(ifindex, prog_fd[prog_num], xdp_flags) < 0) {
		fprintf(stderr, "link set xdp fd failed\n");
		return EXIT_FAIL_XDP;
	}

	if (debug) {
		printf("Debug-mode reading trace pipe (fix #define DEBUG)\n");
		read_trace_pipe();
	}

	stats_poll(interval);
	return EXIT_OK;
}