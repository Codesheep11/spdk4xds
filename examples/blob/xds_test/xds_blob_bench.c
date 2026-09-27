/*   SPDX-License-Identifier: BSD-3-Clause
 */

#include <getopt.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cuda_runtime.h>

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#include "nvme_xds_registry.h"
#include "xds/dma_registry.h"

#define DEFAULT_JSON_CONFIG "/home/yangxc/project/KVCache/spdk_blob/json/Raid0Numa1x2.json"
#define DEFAULT_BDEV_NAME "Raid0Numa1x2"
#define DEFAULT_REACTOR_MASK "0xf000000000000"
#define DEFAULT_PEER_BDFS "0000:da:00.0,0000:d9:00.0"
#define DEFAULT_MAX_BLOB_BYTES (64ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_MAX_GPU_BYTES (36ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_MIN_CLUSTER_SIZE (1024ULL * 1024ULL)
#define DEFAULT_MAX_CLUSTER_SIZE (1024ULL * 1024ULL * 1024ULL)
#define FILL_BYTE 0xA5

#define CUDA_CHECK(call) do { \
	cudaError_t _e = (call); \
	if (_e != cudaSuccess) { \
		fprintf(stderr, "cuda error %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(_e)); \
		app_stop_with_rc(1); \
		return; \
	} \
} while (0)

enum bench_mode {
	MODE_RANDOM_READ = 0,
	MODE_APPEND_WRITE,
	MODE_SEQUENTIAL_READ,
};

enum pass_kind {
	PASS_PREFILL = 0,
	PASS_WARMUP,
	PASS_MEASURE,
};

enum buffer_kind {
	BUFFER_GPU = 0,
	BUFFER_HOST,
};

struct app_ctx;
struct worker_ctx;

struct io_slot {
	struct app_ctx *app;
	struct worker_ctx *worker;
	uint32_t slot_id;
	uint64_t order_idx;
	double submit_ts;
	bool in_use;
};

struct worker_ctx {
	struct app_ctx *app;
	uint32_t worker_id;
	uint32_t core;
	uint32_t qd;
	uint32_t slot_base;
	struct spdk_thread *thread;
	struct spdk_io_channel *channel;
	struct io_slot *slots;

	uint64_t pass_begin;
	uint64_t pass_ios;
	uint64_t next_submit;
	uint64_t completed;
	uint32_t inflight;
	double latency_sum_sec;
	int rc;
	bool pass_done;
};

struct app_ctx {
	const char *json_config_file;
	const char *bdev_name;
	const char *reactor_mask;
	const char *peer_bdfs;
	const char *output_json;
	int main_core;
	int cuda_device;
	uint32_t seed;
	enum buffer_kind buffer_kind;

	enum bench_mode mode;
	uint64_t io_size;
	uint32_t qd;
	uint64_t total_ios;
	uint64_t warmup_ios;
	uint64_t blob_count;
	uint64_t max_blob_bytes;
	uint64_t max_gpu_bytes;
	uint64_t requested_cluster_size;
	bool share_gpu_buffer;

	struct spdk_bs_dev *bs_dev;
	struct spdk_blob_store *bs;
	struct spdk_blob **blobs;
	spdk_blob_id *blob_ids;
	uint64_t next_blob;

	uint64_t io_unit_size;
	uint64_t cluster_size;
	uint64_t io_units_per_io;
	uint64_t blob_clusters;
	uint64_t gpu_buffer_bytes;

	void *gpu_buffer;
	void *host_buffer;
	struct xds_dma_buffer *xds_handle;
	bool xds_inited;

	uint64_t *order;
	struct spdk_thread *main_thread;
	struct worker_ctx *workers;
	uint32_t num_workers;
	uint32_t active_workers;
	uint32_t workers_ready;
	uint32_t workers_done;
	uint32_t workers_cleaned;
	bool cleanup_started;
	enum pass_kind pass;
	uint64_t pass_ios;
	uint64_t completed;
	double pass_start_sec;
	double latency_sum_sec;

	int rc;
	bool stopping;
};

static struct app_ctx g_app;

static void app_stop_with_rc(int rc);
static void start_prefill(struct app_ctx *app);

static void
bs_unload_done(void *arg, int bserrno)
{
	int rc = (int)(intptr_t)arg;

	if (bserrno != 0 && rc == 0) {
		rc = bserrno;
	}
	spdk_app_stop(rc);
}

static double
now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *
mode_name(enum bench_mode mode)
{
	switch (mode) {
	case MODE_RANDOM_READ:
		return "random-read";
	case MODE_APPEND_WRITE:
		return "append-write";
	case MODE_SEQUENTIAL_READ:
		return "sequential-read";
	default:
		return "unknown";
	}
}

static int
parse_mode(const char *s, enum bench_mode *mode)
{
	if (strcmp(s, "random-read") == 0) {
		*mode = MODE_RANDOM_READ;
		return 0;
	}
	if (strcmp(s, "append-write") == 0) {
		*mode = MODE_APPEND_WRITE;
		return 0;
	}
	if (strcmp(s, "sequential-read") == 0) {
		*mode = MODE_SEQUENTIAL_READ;
		return 0;
	}
	return -EINVAL;
}

static int
parse_buffer_kind(const char *s, enum buffer_kind *kind)
{
	if (strcmp(s, "gpu") == 0) {
		*kind = BUFFER_GPU;
		return 0;
	}
	if (strcmp(s, "host") == 0) {
		*kind = BUFFER_HOST;
		return 0;
	}
	return -EINVAL;
}

static const char *
buffer_kind_name(enum buffer_kind kind)
{
	return kind == BUFFER_HOST ? "host" : "gpu";
}

static uint64_t
parse_size_or_die(const char *s)
{
	char *end = NULL;
	uint64_t value;
	uint64_t scale = 1;

	value = strtoull(s, &end, 10);
	if (end == s) {
		fprintf(stderr, "invalid size: %s\n", s);
		exit(1);
	}

	if (strcasecmp(end, "b") == 0 || *end == '\0') {
		scale = 1;
	} else if (strcasecmp(end, "kb") == 0 || strcasecmp(end, "kib") == 0) {
		scale = 1024ULL;
	} else if (strcasecmp(end, "mb") == 0 || strcasecmp(end, "mib") == 0) {
		scale = 1024ULL * 1024ULL;
	} else if (strcasecmp(end, "gb") == 0 || strcasecmp(end, "gib") == 0) {
		scale = 1024ULL * 1024ULL * 1024ULL;
	} else {
		fprintf(stderr, "invalid size suffix: %s\n", s);
		exit(1);
	}

	return value * scale;
}

static void
format_size(uint64_t bytes, char *buf, size_t len)
{
	if (bytes >= 1024ULL * 1024ULL * 1024ULL && bytes % (1024ULL * 1024ULL * 1024ULL) == 0) {
		snprintf(buf, len, "%" PRIu64 "GiB", (uint64_t)(bytes / (1024ULL * 1024ULL * 1024ULL)));
	} else if (bytes >= 1024ULL * 1024ULL && bytes % (1024ULL * 1024ULL) == 0) {
		snprintf(buf, len, "%" PRIu64 "MiB", (uint64_t)(bytes / (1024ULL * 1024ULL)));
	} else if (bytes >= 1024ULL && bytes % 1024ULL == 0) {
		snprintf(buf, len, "%" PRIu64 "KiB", (uint64_t)(bytes / 1024ULL));
	} else {
		snprintf(buf, len, "%" PRIu64 "B", bytes);
	}
}

static uint64_t
ceil_div_u64(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
}

static uint64_t
min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static uint32_t
xorshift32(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x ? x : 1;
	return *state;
}

static void
build_order(struct app_ctx *app)
{
	uint64_t i;
	uint32_t state = app->seed ? app->seed : 1;

	app->order = calloc(app->total_ios, sizeof(*app->order));
	if (!app->order) {
		fprintf(stderr, "failed to allocate order\n");
		app_stop_with_rc(1);
		return;
	}

	for (i = 0; i < app->total_ios; i++) {
		app->order[i] = i % app->blob_count;
	}

	if (app->mode == MODE_RANDOM_READ) {
		for (i = app->total_ios - 1; i > 0; i--) {
			uint64_t j = xorshift32(&state) % (i + 1);
			uint64_t tmp = app->order[i];
			app->order[i] = app->order[j];
			app->order[j] = tmp;
		}
	}
}

static char *
trim_ascii(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		end--;
	}
	*end = '\0';
	return s;
}

static int
register_gpu_buffer(struct app_ctx *app)
{
	const char **peer_bdfs;
	char *copy;
	char *saveptr = NULL;
	char *tok;
	size_t capacity = 1;
	size_t count = 0;
	const char *p;
	int rc;

	for (p = app->peer_bdfs; *p; p++) {
		if (*p == ',') {
			capacity++;
		}
	}

	peer_bdfs = calloc(capacity, sizeof(*peer_bdfs));
	copy = strdup(app->peer_bdfs);
	if (!peer_bdfs || !copy) {
		free(peer_bdfs);
		free(copy);
		return -ENOMEM;
	}

	for (tok = strtok_r(copy, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
		tok = trim_ascii(tok);
		if (*tok == '\0') {
			free(peer_bdfs);
			free(copy);
			return -EINVAL;
		}
		peer_bdfs[count++] = tok;
	}

	rc = xds_dma_registry_register(app->gpu_buffer, app->gpu_buffer_bytes,
				       &app->xds_handle);
	if (rc == 0) {
		rc = xds_dma_registry_map_peers(app->xds_handle, peer_bdfs, count);
		if (rc != 0) {
			xds_dma_registry_unregister(app->xds_handle);
			app->xds_handle = NULL;
		}
	}
	free(peer_bdfs);
	free(copy);
	return rc;
}

static void
write_json_result(struct app_ctx *app, double seconds, double gibps, double avg_us)
{
	FILE *fp;

	if (!app->output_json) {
		return;
	}

	fp = fopen(app->output_json, "w");
	if (!fp) {
		fprintf(stderr, "failed to open output json: %s\n", app->output_json);
		return;
	}

	fprintf(fp,
		"{\n"
		"  \"mode\": \"%s\",\n"
		"  \"io_size\": %" PRIu64 ",\n"
		"  \"qd\": %u,\n"
		"  \"total_ios\": %" PRIu64 ",\n"
		"  \"total_bytes\": %" PRIu64 ",\n"
		"  \"seconds\": %.9f,\n"
		"  \"bandwidth_gibps\": %.9f,\n"
		"  \"avg_latency_us\": %.9f,\n"
		"  \"blob_count\": %" PRIu64 ",\n"
		"  \"workers\": %u,\n"
		"  \"buffer\": \"%s\",\n"
		"  \"share_gpu_buffer\": %s\n"
		"}\n",
		mode_name(app->mode), app->io_size, app->qd, app->total_ios,
		app->io_size * app->total_ios, seconds, gibps, avg_us,
		app->blob_count, app->active_workers, buffer_kind_name(app->buffer_kind),
		app->share_gpu_buffer ? "true" : "false");
	fclose(fp);
}

static void close_next_blob(void *arg, int bserrno);

static void
worker_cleanup_done_msg(void *arg)
{
	struct app_ctx *app = arg;

	app->workers_cleaned++;
	if (app->workers_cleaned == app->active_workers) {
		close_next_blob(app, 0);
	}
}

static void
worker_cleanup_msg(void *arg)
{
	struct worker_ctx *worker = arg;

	if (worker->channel) {
		spdk_bs_free_io_channel(worker->channel);
		worker->channel = NULL;
	}
	free(worker->slots);
	worker->slots = NULL;
	if (worker->thread) {
		spdk_thread_exit(worker->thread);
		worker->thread = NULL;
	}
	spdk_thread_send_msg(worker->app->main_thread, worker_cleanup_done_msg, worker->app);
}

static void
cleanup_workers_or_continue(struct app_ctx *app)
{
	uint32_t i;

	if (app->cleanup_started || app->active_workers == 0) {
		if (app->active_workers == 0 || app->workers_cleaned == app->active_workers) {
			close_next_blob(app, 0);
		}
		return;
	}

	app->cleanup_started = true;
	app->workers_cleaned = 0;
	for (i = 0; i < app->active_workers; i++) {
		if (app->workers[i].thread) {
			spdk_thread_send_msg(app->workers[i].thread, worker_cleanup_msg, &app->workers[i]);
		} else {
			app->workers_cleaned++;
		}
	}
	if (app->workers_cleaned == app->active_workers) {
		close_next_blob(app, 0);
	}
}

static void
close_next_blob(void *arg, int bserrno)
{
	struct app_ctx *app = arg;

	(void)bserrno;
	while (app->next_blob < app->blob_count && app->blobs[app->next_blob] == NULL) {
		app->next_blob++;
	}
	if (app->next_blob < app->blob_count) {
		struct spdk_blob *blob = app->blobs[app->next_blob];
		app->blobs[app->next_blob] = NULL;
		app->next_blob++;
		spdk_blob_close(blob, close_next_blob, app);
		return;
	}

	if (app->xds_handle) {
		xds_dma_registry_unregister(app->xds_handle);
		app->xds_handle = NULL;
	}
	if (app->xds_inited) {
		spdk_nvme_xds_set_translate_cb(NULL, NULL);
		xds_dma_registry_fini();
		app->xds_inited = false;
	}
	if (app->gpu_buffer) {
		cudaFree(app->gpu_buffer);
		app->gpu_buffer = NULL;
	}
	if (app->host_buffer) {
		spdk_dma_free(app->host_buffer);
		app->host_buffer = NULL;
	}
	free(app->order);
	free(app->blob_ids);
	free(app->blobs);
	free(app->workers);
	app->order = NULL;
	app->blob_ids = NULL;
	app->blobs = NULL;
	app->workers = NULL;

	if (app->bs) {
		spdk_bs_unload(app->bs, bs_unload_done, (void *)(intptr_t)app->rc);
		app->bs = NULL;
	} else {
		spdk_app_stop(app->rc);
	}
}

static void
app_stop_with_rc(int rc)
{
	struct app_ctx *app = &g_app;

	if (rc != 0 && app->rc == 0) {
		app->rc = rc;
	}
	if (app->stopping) {
		return;
	}
	app->stopping = true;
	app->next_blob = 0;
	cleanup_workers_or_continue(app);
}

static void
finish_pass(struct app_ctx *app)
{
	if (app->pass == PASS_PREFILL) {
		if (app->warmup_ios > 0) {
			app->pass = PASS_WARMUP;
		} else {
			app->pass = PASS_MEASURE;
		}
	} else if (app->pass == PASS_WARMUP) {
		app->pass = PASS_MEASURE;
	} else {
		double seconds = now_sec() - app->pass_start_sec;
		uint64_t total_bytes = app->io_size * app->total_ios;
		double gibps = seconds > 0.0 ? ((double)total_bytes / (1024.0 * 1024.0 * 1024.0)) / seconds : 0.0;
		double avg_us = app->completed ? app->latency_sum_sec * 1000000.0 / (double)app->completed : 0.0;
		char size_buf[32];

		format_size(app->io_size, size_buf, sizeof(size_buf));
		printf("mode=%s io_size=%s qd=%u ios=%" PRIu64 " total=%.3f GiB\n",
		       mode_name(app->mode), size_buf, app->qd, app->total_ios,
		       (double)total_bytes / (1024.0 * 1024.0 * 1024.0));
		printf("bandwidth=%.4f GiB/s avg_io_latency=%.3f us elapsed=%.6f s\n",
		       gibps, avg_us, seconds);
		write_json_result(app, seconds, gibps, avg_us);
		app_stop_with_rc(0);
		return;
	}

	start_prefill(app);
}

static uint64_t
blob_index_for_order(struct app_ctx *app, uint64_t order_idx)
{
	if (app->pass == PASS_PREFILL) {
		return order_idx % app->blob_count;
	}
	return app->order[order_idx];
}

static void worker_submit_more(struct worker_ctx *worker);

static void
worker_pass_done_msg(void *arg)
{
	struct worker_ctx *worker = arg;
	struct app_ctx *app = worker->app;

	if (worker->pass_done) {
		return;
	}
	worker->pass_done = true;

	if (worker->rc != 0) {
		if (app->rc == 0) {
			app->rc = worker->rc;
		}
	}

	app->completed += worker->completed;
	app->latency_sum_sec += worker->latency_sum_sec;
	app->workers_done++;

	if (app->workers_done == app->active_workers) {
		if (app->rc != 0) {
			app_stop_with_rc(app->rc);
		} else {
			finish_pass(app);
		}
	}
}

static void
io_complete(void *arg, int bserrno)
{
	struct io_slot *slot = arg;
	struct worker_ctx *worker = slot->worker;
	struct app_ctx *app = worker->app;

	if (bserrno != 0) {
		fprintf(stderr, "I/O failed rc=%d\n", bserrno);
		if (worker->rc == 0) {
			worker->rc = bserrno;
		}
	}

	if (worker->rc != 0) {
		slot->in_use = false;
		if (worker->inflight > 0) {
			worker->inflight--;
		}
		if (worker->inflight == 0) {
			spdk_thread_send_msg(app->main_thread, worker_pass_done_msg, worker);
		}
		return;
	}

	if (app->pass == PASS_MEASURE) {
		worker->latency_sum_sec += now_sec() - slot->submit_ts;
	}
	slot->in_use = false;
	worker->inflight--;
	worker->completed++;

	if (worker->completed == worker->pass_ios) {
		spdk_thread_send_msg(app->main_thread, worker_pass_done_msg, worker);
		return;
	}

	worker_submit_more(worker);
}

static void
worker_submit_one(struct worker_ctx *worker, struct io_slot *slot)
{
	struct app_ctx *app = worker->app;
	uint64_t order_idx = worker->pass_begin + worker->next_submit++;
	uint64_t blob_idx = blob_index_for_order(app, order_idx);
	void *payload;
	bool is_read;

	if (app->buffer_kind == BUFFER_HOST) {
		payload = (void *)((uintptr_t)app->host_buffer +
				   ((uint64_t)(worker->slot_base + slot->slot_id) * app->io_size));
	} else {
		payload = app->share_gpu_buffer ? app->gpu_buffer :
			  (void *)((uintptr_t)app->gpu_buffer +
				   ((uint64_t)(worker->slot_base + slot->slot_id) * app->io_size));
	}
	slot->order_idx = order_idx;
	slot->submit_ts = now_sec();
	slot->in_use = true;
	worker->inflight++;

	is_read = (app->pass != PASS_PREFILL && app->mode != MODE_APPEND_WRITE);
	if (is_read) {
		spdk_blob_io_read(app->blobs[blob_idx], worker->channel, payload, 0,
				  app->io_units_per_io, io_complete, slot);
	} else {
		spdk_blob_io_write(app->blobs[blob_idx], worker->channel, payload, 0,
				   app->io_units_per_io, io_complete, slot);
	}
}

static void
worker_submit_more(struct worker_ctx *worker)
{
	uint32_t i;

	if (worker->rc != 0 || worker->app->stopping) {
		return;
	}

	for (i = 0; i < worker->qd && worker->next_submit < worker->pass_ios; i++) {
		if (!worker->slots[i].in_use) {
			worker_submit_one(worker, &worker->slots[i]);
		}
	}
}

static void
worker_start_pass_msg(void *arg)
{
	struct worker_ctx *worker = arg;

	if (worker->pass_ios == 0) {
		spdk_thread_send_msg(worker->app->main_thread, worker_pass_done_msg, worker);
		return;
	}
	worker_submit_more(worker);
}

static void
start_prefill(struct app_ctx *app)
{
	const char *name;
	uint32_t i;

	if (app->pass == PASS_PREFILL) {
		app->pass_ios = app->blob_count;
		name = "prefill";
	} else if (app->pass == PASS_WARMUP) {
		app->pass_ios = min_u64(app->warmup_ios, app->total_ios);
		name = "warmup";
	} else {
		app->pass_ios = app->total_ios;
		name = "measured";
		app->pass_start_sec = now_sec();
	}

	app->completed = 0;
	app->latency_sum_sec = 0.0;
	app->workers_done = 0;
	printf("starting %s pass: ios=%" PRIu64 " workers=%u global_qd=%u\n",
	       name, app->pass_ios, app->active_workers, app->qd);

	for (i = 0; i < app->active_workers; i++) {
		struct worker_ctx *worker = &app->workers[i];
		uint64_t begin = (app->pass_ios * i) / app->active_workers;
		uint64_t end = (app->pass_ios * (i + 1)) / app->active_workers;

		worker->pass_begin = begin;
		worker->pass_ios = end - begin;
		worker->next_submit = 0;
		worker->completed = 0;
		worker->inflight = 0;
		worker->latency_sum_sec = 0.0;
		worker->rc = 0;
		worker->pass_done = false;
		for (uint32_t j = 0; j < worker->qd; j++) {
			worker->slots[j].in_use = false;
		}
		spdk_thread_send_msg(worker->thread, worker_start_pass_msg, worker);
	}
}

static void
open_blob_done(void *arg, struct spdk_blob *blob, int bserrno);
static void create_next_blob(struct app_ctx *app);

static int
discover_workers(struct app_ctx *app)
{
	uint32_t core;
	uint32_t available = 0;
	uint32_t idx = 0;
	uint32_t slot_base = 0;

	app->main_thread = spdk_get_thread();
	SPDK_ENV_FOREACH_CORE(core) {
		if (core != (uint32_t)app->main_core) {
			available++;
		}
	}
	if (available == 0) {
		fprintf(stderr, "reactor mask must include at least one worker core besides main core %d\n",
			app->main_core);
		return -EINVAL;
	}

	app->active_workers = app->qd < available ? app->qd : available;
	app->num_workers = app->active_workers;
	app->workers = calloc(app->active_workers, sizeof(*app->workers));
	if (!app->workers) {
		return -ENOMEM;
	}

	SPDK_ENV_FOREACH_CORE(core) {
		if (core == (uint32_t)app->main_core || idx == app->active_workers) {
			continue;
		}

		app->workers[idx].app = app;
		app->workers[idx].worker_id = idx;
		app->workers[idx].core = core;
		app->workers[idx].qd = app->qd / app->active_workers +
				       (idx < (app->qd % app->active_workers) ? 1 : 0);
		app->workers[idx].slot_base = slot_base;
		slot_base += app->workers[idx].qd;
		idx++;
	}

	return 0;
}

static void
worker_init_done_msg(void *arg)
{
	struct worker_ctx *worker = arg;
	struct app_ctx *app = worker->app;

	app->workers_ready++;
	if (app->workers_ready == app->active_workers) {
		app->next_blob = 0;
		create_next_blob(app);
	}
}

static void
worker_init_failed_msg(void *arg)
{
	struct worker_ctx *worker = arg;

	app_stop_with_rc(worker->rc != 0 ? worker->rc : 1);
}

static void
worker_init_msg(void *arg)
{
	struct worker_ctx *worker = arg;
	struct app_ctx *app = worker->app;
	uint32_t i;

	worker->channel = spdk_bs_alloc_io_channel(app->bs);
	if (!worker->channel) {
		worker->rc = -ENOMEM;
		spdk_thread_send_msg(app->main_thread, worker_init_failed_msg, worker);
		return;
	}

	worker->slots = calloc(worker->qd, sizeof(*worker->slots));
	if (!worker->slots) {
		worker->rc = -ENOMEM;
		spdk_thread_send_msg(app->main_thread, worker_init_failed_msg, worker);
		return;
	}
	for (i = 0; i < worker->qd; i++) {
		worker->slots[i].app = app;
		worker->slots[i].worker = worker;
		worker->slots[i].slot_id = i;
	}

	spdk_thread_send_msg(app->main_thread, worker_init_done_msg, worker);
}

static void
start_workers(struct app_ctx *app)
{
	uint32_t i;

	for (i = 0; i < app->active_workers; i++) {
		struct spdk_cpuset cpumask = {};
		char thread_name[32];

		spdk_cpuset_zero(&cpumask);
		spdk_cpuset_set_cpu(&cpumask, app->workers[i].core, true);
		snprintf(thread_name, sizeof(thread_name), "xds_worker_%u", app->workers[i].core);
		app->workers[i].thread = spdk_thread_create(thread_name, &cpumask);
		if (!app->workers[i].thread) {
			app_stop_with_rc(-ENOMEM);
			return;
		}
		spdk_thread_send_msg(app->workers[i].thread, worker_init_msg, &app->workers[i]);
	}
}

static void
create_blob_done(void *arg, spdk_blob_id blobid, int bserrno)
{
	struct app_ctx *app = arg;

	if (bserrno != 0) {
		fprintf(stderr, "create blob failed rc=%d\n", bserrno);
		app_stop_with_rc(1);
		return;
	}

	app->blob_ids[app->next_blob] = blobid;
	spdk_bs_open_blob(app->bs, blobid, open_blob_done, app);
}

static void
create_next_blob(struct app_ctx *app)
{
	struct spdk_blob_opts opts;

	if (app->next_blob == app->blob_count) {
		app->next_blob = 0;
		build_order(app);
		if (app->stopping) {
			return;
		}
		if (app->mode == MODE_APPEND_WRITE) {
			app->pass = app->warmup_ios > 0 ? PASS_WARMUP : PASS_MEASURE;
		} else {
			app->pass = PASS_PREFILL;
		}
		start_prefill(app);
		return;
	}

	spdk_blob_opts_init(&opts, sizeof(opts));
	opts.num_clusters = app->blob_clusters;
	spdk_bs_create_blob_ext(app->bs, &opts, create_blob_done, app);
}

static void
open_blob_done(void *arg, struct spdk_blob *blob, int bserrno)
{
	struct app_ctx *app = arg;

	if (bserrno != 0) {
		fprintf(stderr, "open blob failed rc=%d\n", bserrno);
		app_stop_with_rc(1);
		return;
	}

	app->blobs[app->next_blob] = blob;
	app->next_blob++;
	create_next_blob(app);
}

static void
bs_init_done(void *arg, struct spdk_blob_store *bs, int bserrno)
{
	struct app_ctx *app = arg;
	int rc;
	uint64_t required_buffer_bytes;
	char io_size_buf[32], data_buf[32], blob_buf[32], cluster_buf[32];

	if (bserrno != 0) {
		fprintf(stderr, "blobstore init failed rc=%d\n", bserrno);
		app_stop_with_rc(1);
		return;
	}

	app->bs = bs;
	app->io_unit_size = spdk_bs_get_io_unit_size(bs);
	app->cluster_size = spdk_bs_get_cluster_size(bs);
	if (app->io_size % app->io_unit_size != 0) {
		fprintf(stderr, "io_size must be multiple of io_unit_size=%" PRIu64 "\n", app->io_unit_size);
		app_stop_with_rc(1);
		return;
	}

	app->io_units_per_io = app->io_size / app->io_unit_size;
	app->blob_clusters = ceil_div_u64(app->io_size, app->cluster_size);
	required_buffer_bytes = (app->buffer_kind == BUFFER_GPU && app->share_gpu_buffer) ?
				app->io_size : app->io_size * app->qd;
	if (app->buffer_kind == BUFFER_GPU && required_buffer_bytes > app->max_gpu_bytes) {
		fprintf(stderr, "gpu buffer exceeds limit\n");
		app_stop_with_rc(1);
		return;
	}
	app->gpu_buffer_bytes = required_buffer_bytes;

	app->blobs = calloc(app->blob_count, sizeof(*app->blobs));
	app->blob_ids = calloc(app->blob_count, sizeof(*app->blob_ids));
	if (!app->blobs || !app->blob_ids) {
		fprintf(stderr, "allocation failed\n");
		app_stop_with_rc(1);
		return;
	}

	if (app->buffer_kind == BUFFER_HOST) {
		app->host_buffer = spdk_dma_zmalloc(app->gpu_buffer_bytes, 4096, NULL);
		if (!app->host_buffer) {
			fprintf(stderr, "spdk_dma_zmalloc failed\n");
			app_stop_with_rc(1);
			return;
		}
		memset(app->host_buffer, FILL_BYTE, app->gpu_buffer_bytes);
	} else {
		cudaSetDevice(app->cuda_device);
		CUDA_CHECK(cudaMalloc(&app->gpu_buffer, app->gpu_buffer_bytes));
		CUDA_CHECK(cudaMemset(app->gpu_buffer, FILL_BYTE, app->gpu_buffer_bytes));
		CUDA_CHECK(cudaDeviceSynchronize());

		rc = xds_dma_registry_init(XDS_DEVICE_BACKEND_NVIDIA, NULL);
		if (rc != 0) {
			fprintf(stderr, "xds_dma_registry_init failed rc=%d\n", rc);
			app_stop_with_rc(1);
			return;
		}
		spdk_nvme_xds_set_translate_cb(xds_dma_registry_translate, NULL);
		app->xds_inited = true;

		rc = register_gpu_buffer(app);
		if (rc != 0) {
			fprintf(stderr, "xds_dma_registry_register/map failed rc=%d\n", rc);
			app_stop_with_rc(1);
			return;
		}
	}

	format_size(app->io_size, io_size_buf, sizeof(io_size_buf));
	format_size(app->gpu_buffer_bytes, data_buf, sizeof(data_buf));
	format_size(app->io_size * app->blob_count, blob_buf, sizeof(blob_buf));
	format_size(app->cluster_size, cluster_buf, sizeof(cluster_buf));
	printf("mode=%s io_size=%s qd=%u total_ios=%" PRIu64 " blob_count=%" PRIu64 " buffer=%s peers=%s\n",
	       mode_name(app->mode), io_size_buf, app->qd, app->total_ios, app->blob_count,
	       buffer_kind_name(app->buffer_kind), app->peer_bdfs);
	printf("logical_blob_data=%s io_buffer=%s shared_gpu=%s cluster_size=%s io_unit=%" PRIu64 "\n",
	       blob_buf, data_buf, app->share_gpu_buffer ? "true" : "false",
	       cluster_buf, app->io_unit_size);

	rc = discover_workers(app);
	if (rc != 0) {
		fprintf(stderr, "discover_workers failed rc=%d\n", rc);
		app_stop_with_rc(1);
		return;
	}
	printf("workers=%u global_qd=%u qd_per_worker=", app->active_workers, app->qd);
	for (uint32_t i = 0; i < app->active_workers; i++) {
		printf("%s%u@core%u", i == 0 ? "" : ",", app->workers[i].qd, app->workers[i].core);
	}
	printf("\n");
	start_workers(app);
}

static void
base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	(void)type;
	(void)bdev;
	(void)event_ctx;
}

static void
app_start(void *arg)
{
	struct app_ctx *app = arg;
	struct spdk_bs_opts opts;
	int rc;

	rc = spdk_bdev_create_bs_dev_ext(app->bdev_name, base_bdev_event_cb, NULL, &app->bs_dev);
	if (rc != 0) {
		fprintf(stderr, "spdk_bdev_create_bs_dev_ext failed rc=%d\n", rc);
		spdk_app_stop(rc);
		return;
	}

	spdk_bs_opts_init(&opts, sizeof(opts));
	if (app->requested_cluster_size != 0) {
		opts.cluster_sz = app->requested_cluster_size;
	}
	spdk_bs_init(app->bs_dev, &opts, bs_init_done, app);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  --mode random-read|append-write|sequential-read\n"
		"  --io-size SIZE              default 4KB\n"
		"  --qd N                      default 64; global QD split across worker cores\n"
		"  --total-ios N               default 65536\n"
		"  --warmup-ios N              default 4096\n"
		"  --blob-count N              default min(total_ios,16384,max_blob_bytes/io_size)\n"
		"  --max-blob-bytes SIZE       default 64GB\n"
		"  --max-gpu-bytes SIZE        default 36GB\n"
		"  --share-gpu-buffer\n"
		"  --buffer gpu|host          default gpu; host skips xDS translation\n"
		"  --bdev-name NAME            default Raid0Numa1x2\n"
		"  --json-config-file PATH\n"
		"  --reactor-mask MASK         default 0xf000000000000\n"
		"  --main-core N               default 48\n"
		"  --peer-bdfs LIST            default 0000:da:00.0,0000:d9:00.0\n"
		"  --cuda-device N             default 0\n"
		"  --cluster-size SIZE         default clamp(io_size,1MB,1GB)\n"
		"  --output-json PATH\n"
		"  --seed N                    default 1\n",
		prog);
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	uint64_t auto_blob_count;
	int op;

	enum {
		OPT_MODE = 1000,
		OPT_IO_SIZE,
		OPT_QD,
		OPT_TOTAL_IOS,
		OPT_WARMUP_IOS,
		OPT_BLOB_COUNT,
		OPT_MAX_BLOB_BYTES,
		OPT_MAX_GPU_BYTES,
		OPT_SHARE_GPU_BUFFER,
		OPT_BUFFER,
		OPT_BDEV_NAME,
		OPT_JSON_CONFIG_FILE,
		OPT_REACTOR_MASK,
		OPT_MAIN_CORE,
		OPT_PEER_BDFS,
		OPT_CUDA_DEVICE,
		OPT_CLUSTER_SIZE,
		OPT_OUTPUT_JSON,
		OPT_SEED,
	};
	static const struct option long_opts[] = {
		{"mode", required_argument, NULL, OPT_MODE},
		{"io-size", required_argument, NULL, OPT_IO_SIZE},
		{"qd", required_argument, NULL, OPT_QD},
		{"total-ios", required_argument, NULL, OPT_TOTAL_IOS},
		{"warmup-ios", required_argument, NULL, OPT_WARMUP_IOS},
		{"blob-count", required_argument, NULL, OPT_BLOB_COUNT},
		{"max-blob-bytes", required_argument, NULL, OPT_MAX_BLOB_BYTES},
		{"max-gpu-bytes", required_argument, NULL, OPT_MAX_GPU_BYTES},
		{"share-gpu-buffer", no_argument, NULL, OPT_SHARE_GPU_BUFFER},
		{"buffer", required_argument, NULL, OPT_BUFFER},
		{"bdev-name", required_argument, NULL, OPT_BDEV_NAME},
		{"json-config-file", required_argument, NULL, OPT_JSON_CONFIG_FILE},
		{"reactor-mask", required_argument, NULL, OPT_REACTOR_MASK},
		{"main-core", required_argument, NULL, OPT_MAIN_CORE},
		{"peer-bdfs", required_argument, NULL, OPT_PEER_BDFS},
		{"cuda-device", required_argument, NULL, OPT_CUDA_DEVICE},
		{"cluster-size", required_argument, NULL, OPT_CLUSTER_SIZE},
		{"output-json", required_argument, NULL, OPT_OUTPUT_JSON},
		{"seed", required_argument, NULL, OPT_SEED},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	memset(&g_app, 0, sizeof(g_app));
	g_app.json_config_file = DEFAULT_JSON_CONFIG;
	g_app.bdev_name = DEFAULT_BDEV_NAME;
	g_app.reactor_mask = DEFAULT_REACTOR_MASK;
	g_app.peer_bdfs = DEFAULT_PEER_BDFS;
	g_app.main_core = 48;
	g_app.cuda_device = 0;
	g_app.seed = 1;
	g_app.buffer_kind = BUFFER_GPU;
	g_app.mode = MODE_RANDOM_READ;
	g_app.io_size = 4 * 1024ULL;
	g_app.qd = 64;
	g_app.total_ios = 65536;
	g_app.warmup_ios = 4096;
	g_app.max_blob_bytes = DEFAULT_MAX_BLOB_BYTES;
	g_app.max_gpu_bytes = DEFAULT_MAX_GPU_BYTES;

	while ((op = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
		switch (op) {
		case OPT_MODE:
			if (parse_mode(optarg, &g_app.mode) != 0) {
				usage(argv[0]);
				return 1;
			}
			break;
		case OPT_IO_SIZE:
			g_app.io_size = parse_size_or_die(optarg);
			break;
		case OPT_QD:
			g_app.qd = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case OPT_TOTAL_IOS:
			g_app.total_ios = strtoull(optarg, NULL, 10);
			break;
		case OPT_WARMUP_IOS:
			g_app.warmup_ios = strtoull(optarg, NULL, 10);
			break;
		case OPT_BLOB_COUNT:
			g_app.blob_count = strtoull(optarg, NULL, 10);
			break;
		case OPT_MAX_BLOB_BYTES:
			g_app.max_blob_bytes = parse_size_or_die(optarg);
			break;
		case OPT_MAX_GPU_BYTES:
			g_app.max_gpu_bytes = parse_size_or_die(optarg);
			break;
		case OPT_SHARE_GPU_BUFFER:
			g_app.share_gpu_buffer = true;
			break;
		case OPT_BUFFER:
			if (parse_buffer_kind(optarg, &g_app.buffer_kind) != 0) {
				usage(argv[0]);
				return 1;
			}
			break;
		case OPT_BDEV_NAME:
			g_app.bdev_name = optarg;
			break;
		case OPT_JSON_CONFIG_FILE:
			g_app.json_config_file = optarg;
			break;
		case OPT_REACTOR_MASK:
			g_app.reactor_mask = optarg;
			break;
		case OPT_MAIN_CORE:
			g_app.main_core = (int)strtol(optarg, NULL, 10);
			break;
		case OPT_PEER_BDFS:
			g_app.peer_bdfs = optarg;
			break;
		case OPT_CUDA_DEVICE:
			g_app.cuda_device = (int)strtol(optarg, NULL, 10);
			break;
		case OPT_CLUSTER_SIZE:
			g_app.requested_cluster_size = parse_size_or_die(optarg);
			break;
		case OPT_OUTPUT_JSON:
			g_app.output_json = optarg;
			break;
		case OPT_SEED:
			g_app.seed = (uint32_t)strtoul(optarg, NULL, 10);
			break;
		case 'h':
		default:
			usage(argv[0]);
			return op == 'h' ? 0 : 1;
		}
	}

	if (g_app.io_size == 0 || g_app.qd == 0 || g_app.total_ios == 0 ||
	    g_app.max_blob_bytes < g_app.io_size) {
		usage(argv[0]);
		return 1;
	}
	if (g_app.requested_cluster_size == 0) {
		g_app.requested_cluster_size = min_u64(DEFAULT_MAX_CLUSTER_SIZE,
						       spdk_max(g_app.io_size, DEFAULT_MIN_CLUSTER_SIZE));
	}
	if (g_app.requested_cluster_size > UINT32_MAX) {
		fprintf(stderr, "cluster size must fit uint32\n");
		return 1;
	}

	auto_blob_count = min_u64(g_app.total_ios, 16384);
	auto_blob_count = min_u64(auto_blob_count, g_app.max_blob_bytes / g_app.io_size);
	if (auto_blob_count == 0) {
		auto_blob_count = 1;
	}
	if (g_app.blob_count == 0) {
		g_app.blob_count = auto_blob_count;
	}

	if (!g_app.share_gpu_buffer && g_app.io_size * g_app.qd > g_app.max_gpu_bytes) {
		fprintf(stderr, "requested GPU ring exceeds max-gpu-bytes; use --share-gpu-buffer\n");
		return 1;
	}

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "xds_blob_bench";
	opts.json_config_file = g_app.json_config_file;
	opts.reactor_mask = g_app.reactor_mask;
	opts.main_core = g_app.main_core;
	opts.rpc_addr = NULL;
	opts.iova_mode = "pa";

	int rc = spdk_app_start(&opts, app_start, &g_app);
	spdk_app_fini();
	return rc != 0 ? rc : g_app.rc;
}
