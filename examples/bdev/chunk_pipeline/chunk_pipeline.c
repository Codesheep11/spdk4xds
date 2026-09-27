/*   SPDX-License-Identifier: BSD-3-Clause
 */

#include "spdk/stdinc.h"

#include <cuda_runtime.h>
#include <getopt.h>

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#include "spdk/string.h"

#define DEFAULT_JSON_CONFIG "/home/yangxc/workspace/xDS/configs/bdev/Raid0_2.json"
#define DEFAULT_BDEV_NAME "Raid0_2"
#define DEFAULT_REACTOR_MASK "0x3"
#define DEFAULT_TOTAL_SIZE (48ULL * 1024ULL * 1024ULL)
#define DEFAULT_CHUNK_SIZES "256K,512K,1M,2M,4M,8M,16M,48M"
#define STAGING_SLOT_COUNT 2
#define GIB (1024.0 * 1024.0 * 1024.0)

enum run_kind {
	RUN_SERIAL = 0,
	RUN_PIPELINE,
};

struct app_context;

struct run_result {
	uint64_t total_ns;
	uint64_t ssd_ns;
	uint64_t h2d_ns;
	uint64_t stall_ns;
	uint64_t chunks;
	int status;
};

struct run_state {
	enum run_kind kind;
	uint64_t chunk_size;
	uint64_t chunk_count;
	uint64_t next_read;
	uint64_t reads_completed;
	uint64_t copies_completed;
	uint64_t start_ns;
	uint64_t ssd_ns;
	uint64_t h2d_ns;
	uint64_t stall_ns;
	int status;
};

struct staging_slot {
	struct app_context *app;
	uint32_t slot_id;
	void *cpu_buf;
	bool host_registered;
	bool copy_pending;
	uint64_t chunk_idx;
	uint64_t bytes;
	uint64_t read_start_ns;
	cudaEvent_t copy_start;
	cudaEvent_t copy_done;
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

struct app_context {
	const char *bdev_name;
	uint64_t total_size;
	uint64_t offset_bytes;
	uint32_t cuda_device;

	uint64_t *chunk_sizes;
	uint32_t chunk_size_count;
	uint32_t chunk_size_index;
	uint64_t max_chunk_size;

	struct spdk_bdev *bdev;
	struct spdk_bdev_desc *bdev_desc;
	struct spdk_io_channel *bdev_io_channel;
	uint32_t block_size;
	uint32_t buf_align;
	uint64_t block_count;

	void *gpu_buf;
	cudaStream_t copy_stream;
	struct staging_slot slots[STAGING_SLOT_COUNT];

	struct run_state run;
	struct run_result serial_result;
	struct run_result pipeline_result;

	int rc;
	bool stopping;
};

static struct app_context g_app = {
	.bdev_name = DEFAULT_BDEV_NAME,
	.total_size = DEFAULT_TOTAL_SIZE,
	.cuda_device = 0,
};

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const char *
run_kind_name(enum run_kind kind)
{
	return kind == RUN_PIPELINE ? "pipeline" : "serial";
}

static double
ns_to_ms(uint64_t ns)
{
	return (double)ns / 1000000.0;
}

static int
parse_size(const char *text, uint64_t *out)
{
	char *end = NULL;
	double value;
	uint64_t multiplier = 1;

	if (!text || !out) {
		return -EINVAL;
	}

	errno = 0;
	value = strtod(text, &end);
	if (errno != 0 || end == text || value <= 0) {
		return -EINVAL;
	}

	if (*end != '\0') {
		if (strcasecmp(end, "k") == 0 || strcasecmp(end, "kb") == 0) {
			multiplier = 1024ULL;
		} else if (strcasecmp(end, "m") == 0 || strcasecmp(end, "mb") == 0) {
			multiplier = 1024ULL * 1024ULL;
		} else if (strcasecmp(end, "g") == 0 || strcasecmp(end, "gb") == 0) {
			multiplier = 1024ULL * 1024ULL * 1024ULL;
		} else {
			return -EINVAL;
		}
	}

	*out = (uint64_t)(value * (double)multiplier);
	return 0;
}

static void
free_chunk_sizes(struct app_context *app)
{
	free(app->chunk_sizes);
	app->chunk_sizes = NULL;
	app->chunk_size_count = 0;
	app->max_chunk_size = 0;
}

static int
append_chunk_size(struct app_context *app, uint64_t size)
{
	uint64_t *sizes;

	sizes = realloc(app->chunk_sizes,
			(size_t)(app->chunk_size_count + 1) * sizeof(*sizes));
	if (!sizes) {
		return -ENOMEM;
	}
	app->chunk_sizes = sizes;
	app->chunk_sizes[app->chunk_size_count++] = size;
	if (size > app->max_chunk_size) {
		app->max_chunk_size = size;
	}
	return 0;
}

static int
set_single_chunk_size(struct app_context *app, const char *text)
{
	uint64_t size;
	int rc;

	rc = parse_size(text, &size);
	if (rc != 0) {
		return rc;
	}
	free_chunk_sizes(app);
	return append_chunk_size(app, size);
}

static int
set_chunk_size_list(struct app_context *app, const char *text)
{
	char *copy;
	char *saveptr = NULL;
	char *token;
	int rc = 0;

	if (!text) {
		return -EINVAL;
	}

	copy = strdup(text);
	if (!copy) {
		return -ENOMEM;
	}

	free_chunk_sizes(app);
	for (token = strtok_r(copy, ",", &saveptr); token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		uint64_t size;

		while (isspace((unsigned char)*token)) {
			token++;
		}
		rc = parse_size(token, &size);
		if (rc != 0) {
			break;
		}
		rc = append_chunk_size(app, size);
		if (rc != 0) {
			break;
		}
	}
	free(copy);

	if (rc != 0) {
		free_chunk_sizes(app);
		return rc;
	}
	return app->chunk_size_count > 0 ? 0 : -EINVAL;
}

static int
cuda_check(cudaError_t err, const char *op)
{
	if (err == cudaSuccess) {
		return 0;
	}
	fprintf(stderr, "%s failed: %s (%d)\n", op, cudaGetErrorString(err), err);
	return -EIO;
}

static int
cuda_check_nonfatal(cudaError_t err, const char *op)
{
	if (err == cudaSuccess) {
		return 0;
	}
	fprintf(stderr, "warning: %s failed: %s (%d); continuing\n",
		op, cudaGetErrorString(err), err);
	return -EIO;
}

static void cleanup_and_stop(struct app_context *app, int rc);

static int
wait_for_slot_copy(struct staging_slot *slot, bool count_stall)
{
	struct app_context *app = slot->app;
	cudaError_t err;
	float elapsed_ms = 0.0f;
	uint64_t stall_start;

	if (!slot->copy_pending) {
		return 0;
	}

	if (count_stall) {
		err = cudaEventQuery(slot->copy_done);
		if (err == cudaErrorNotReady) {
			stall_start = now_ns();
			err = cudaEventSynchronize(slot->copy_done);
			app->run.stall_ns += now_ns() - stall_start;
		}
	} else {
		err = cudaEventSynchronize(slot->copy_done);
	}
	if (cuda_check(err, "cudaEventSynchronize") != 0) {
		return -EIO;
	}
	if (cuda_check(cudaEventElapsedTime(&elapsed_ms, slot->copy_start,
					    slot->copy_done),
		       "cudaEventElapsedTime") != 0) {
		return -EIO;
	}

	app->run.h2d_ns += (uint64_t)((double)elapsed_ms * 1000000.0);
	app->run.copies_completed++;
	slot->copy_pending = false;
	return 0;
}

static int
start_slot_copy(struct staging_slot *slot)
{
	struct app_context *app = slot->app;
	char *dst = (char *)app->gpu_buf + slot->chunk_idx * app->run.chunk_size;

	if (cuda_check(cudaEventRecord(slot->copy_start, app->copy_stream),
		       "cudaEventRecord(copy_start)") != 0) {
		return -EIO;
	}
	if (cuda_check(cudaMemcpyAsync(dst, slot->cpu_buf, (size_t)slot->bytes,
				       cudaMemcpyHostToDevice, app->copy_stream),
		       "cudaMemcpyAsync(H2D)") != 0) {
		return -EIO;
	}
	if (cuda_check(cudaEventRecord(slot->copy_done, app->copy_stream),
		       "cudaEventRecord(copy_done)") != 0) {
		return -EIO;
	}

	slot->copy_pending = true;
	return 0;
}

static uint64_t
chunk_bytes(const struct app_context *app, uint64_t chunk_idx, uint64_t chunk_size)
{
	uint64_t offset = chunk_idx * chunk_size;
	uint64_t remaining = app->total_size - offset;

	return remaining < chunk_size ? remaining : chunk_size;
}

static void read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void submit_read_wait_cb(void *arg);

static int
submit_read_chunk(struct app_context *app, uint64_t chunk_idx,
		  struct staging_slot *slot)
{
	uint64_t byte_offset = app->offset_bytes + chunk_idx * app->run.chunk_size;
	uint64_t bytes = chunk_bytes(app, chunk_idx, app->run.chunk_size);
	uint64_t lba = byte_offset / app->block_size;
	uint64_t blocks = bytes / app->block_size;
	int rc;

	slot->chunk_idx = chunk_idx;
	slot->bytes = bytes;
	slot->read_start_ns = now_ns();

	rc = spdk_bdev_read_blocks(app->bdev_desc, app->bdev_io_channel,
				   slot->cpu_buf, lba, blocks,
				   read_complete, slot);
	if (rc == 0) {
		app->run.next_read = chunk_idx + 1;
		return 0;
	}
	if (rc == -ENOMEM) {
		slot->bdev_io_wait.bdev = app->bdev;
		slot->bdev_io_wait.cb_fn = submit_read_wait_cb;
		slot->bdev_io_wait.cb_arg = slot;
		rc = spdk_bdev_queue_io_wait(app->bdev, app->bdev_io_channel,
					     &slot->bdev_io_wait);
		if (rc == 0) {
			return 0;
		}
	}

	SPDK_ERRLOG("spdk_bdev_read_blocks failed for chunk %" PRIu64 ": %d\n",
		    chunk_idx, rc);
	return rc;
}

static void
finish_run(struct app_context *app, int rc)
{
	struct run_result result = {
		.total_ns = now_ns() - app->run.start_ns,
		.ssd_ns = app->run.ssd_ns,
		.h2d_ns = app->run.h2d_ns,
		.stall_ns = app->run.stall_ns,
		.chunks = app->run.chunk_count,
		.status = rc,
	};

	if (rc != 0) {
		cleanup_and_stop(app, rc);
		return;
	}

	if (app->run.kind == RUN_SERIAL) {
		app->serial_result = result;
	} else {
		double serial_ms;
		double pipeline_ms;
		double speedup;
		double gib_per_sec;

		app->pipeline_result = result;
		serial_ms = ns_to_ms(app->serial_result.total_ns);
		pipeline_ms = ns_to_ms(app->pipeline_result.total_ns);
		speedup = pipeline_ms > 0.0 ? serial_ms / pipeline_ms : 0.0;
		gib_per_sec = ((double)app->total_size / GIB) /
			      ((double)app->pipeline_result.total_ns / 1000000000.0);

		printf("chunk_pipeline_result bdev=%s total_size=%" PRIu64
		       " chunk_size=%" PRIu64 " chunks=%" PRIu64
		       " serial_ms=%.3f pipeline_ms=%.3f speedup=%.3f"
		       " pipeline_gib_per_sec=%.3f"
		       " serial_ssd_ms=%.3f serial_h2d_ms=%.3f"
		       " pipeline_ssd_ms=%.3f pipeline_h2d_ms=%.3f"
		       " pipeline_stall_ms=%.3f\n",
		       app->bdev_name, app->total_size, app->run.chunk_size,
		       app->pipeline_result.chunks, serial_ms, pipeline_ms, speedup,
		       gib_per_sec, ns_to_ms(app->serial_result.ssd_ns),
		       ns_to_ms(app->serial_result.h2d_ns),
		       ns_to_ms(app->pipeline_result.ssd_ns),
		       ns_to_ms(app->pipeline_result.h2d_ns),
		       ns_to_ms(app->pipeline_result.stall_ns));
	}

	if (app->run.kind == RUN_SERIAL) {
		app->run.kind = RUN_PIPELINE;
	} else {
		app->chunk_size_index++;
		app->run.kind = RUN_SERIAL;
	}

	if (app->chunk_size_index >= app->chunk_size_count) {
		cleanup_and_stop(app, 0);
		return;
	}
}

static int
flush_pending_copies(struct app_context *app, bool count_stall)
{
	for (uint32_t i = 0; i < STAGING_SLOT_COUNT; i++) {
		int rc = wait_for_slot_copy(&app->slots[i], count_stall);

		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int start_current_run(struct app_context *app);

static void
read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct staging_slot *slot = cb_arg;
	struct app_context *app = slot->app;
	int rc;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		SPDK_ERRLOG("read failed for chunk %" PRIu64 "\n", slot->chunk_idx);
		cleanup_and_stop(app, -EIO);
		return;
	}

	app->run.ssd_ns += now_ns() - slot->read_start_ns;
	app->run.reads_completed++;

	rc = start_slot_copy(slot);
	if (rc != 0) {
		cleanup_and_stop(app, rc);
		return;
	}

	if (app->run.kind == RUN_SERIAL) {
		rc = wait_for_slot_copy(slot, false);
		if (rc != 0) {
			cleanup_and_stop(app, rc);
			return;
		}
		if (app->run.next_read < app->run.chunk_count) {
			rc = submit_read_chunk(app, app->run.next_read, &app->slots[0]);
			if (rc != 0) {
				cleanup_and_stop(app, rc);
			}
			return;
		}
		finish_run(app, 0);
		rc = start_current_run(app);
		if (rc != 0) {
			cleanup_and_stop(app, rc);
		}
		return;
	}

	if (app->run.next_read < app->run.chunk_count) {
		struct staging_slot *next_slot =
			&app->slots[app->run.next_read % STAGING_SLOT_COUNT];

		rc = wait_for_slot_copy(next_slot, true);
		if (rc != 0) {
			cleanup_and_stop(app, rc);
			return;
		}
		rc = submit_read_chunk(app, app->run.next_read, next_slot);
		if (rc != 0) {
			cleanup_and_stop(app, rc);
		}
		return;
	}

	if (app->run.reads_completed == app->run.chunk_count) {
		rc = flush_pending_copies(app, false);
		if (rc != 0) {
			cleanup_and_stop(app, rc);
			return;
		}
		finish_run(app, 0);
		rc = start_current_run(app);
		if (rc != 0) {
			cleanup_and_stop(app, rc);
		}
	}
}

static void
submit_read_wait_cb(void *arg)
{
	struct staging_slot *slot = arg;
	struct app_context *app = slot->app;
	int rc;

	if (slot->read_start_ns == 0) {
		return;
	}

	rc = spdk_bdev_read_blocks(app->bdev_desc, app->bdev_io_channel,
				   slot->cpu_buf,
				   (app->offset_bytes +
				    slot->chunk_idx * app->run.chunk_size) /
					   app->block_size,
				   slot->bytes / app->block_size,
				   read_complete, slot);
	if (rc == 0) {
		app->run.next_read = slot->chunk_idx + 1;
		return;
	}
	if (rc == -ENOMEM) {
		slot->bdev_io_wait.bdev = app->bdev;
		slot->bdev_io_wait.cb_fn = submit_read_wait_cb;
		slot->bdev_io_wait.cb_arg = slot;
		rc = spdk_bdev_queue_io_wait(app->bdev, app->bdev_io_channel,
					     &slot->bdev_io_wait);
		if (rc == 0) {
			return;
		}
	}
	cleanup_and_stop(app, rc);
}

static int
start_current_run(struct app_context *app)
{
	struct run_state *run = &app->run;
	enum run_kind kind = run->kind;
	uint64_t chunk_size;
	int rc;

	if (app->chunk_size_index >= app->chunk_size_count) {
		return 0;
	}

	rc = flush_pending_copies(app, false);
	if (rc != 0) {
		return rc;
	}

	chunk_size = app->chunk_sizes[app->chunk_size_index];
	memset(run, 0, sizeof(*run));
	run->kind = kind;
	run->chunk_size = chunk_size;
	run->chunk_count = (app->total_size + chunk_size - 1) / chunk_size;
	run->start_ns = now_ns();

	printf("chunk_pipeline_start mode=%s chunk_size=%" PRIu64
	       " chunks=%" PRIu64 "\n",
	       run_kind_name(run->kind), run->chunk_size, run->chunk_count);

	return submit_read_chunk(app, 0, &app->slots[0]);
}

static int
validate_sizes(struct app_context *app)
{
	uint64_t bdev_bytes;

	if (app->total_size == 0 || app->chunk_size_count == 0 ||
	    app->max_chunk_size == 0) {
		return -EINVAL;
	}
	if (app->offset_bytes % app->block_size != 0 ||
	    app->total_size % app->block_size != 0) {
		fprintf(stderr, "offset and total-size must be multiples of bdev block size %u\n",
			app->block_size);
		return -EINVAL;
	}
	for (uint32_t i = 0; i < app->chunk_size_count; i++) {
		if (app->chunk_sizes[i] == 0 ||
		    app->chunk_sizes[i] > app->total_size ||
		    app->chunk_sizes[i] % app->block_size != 0) {
			fprintf(stderr, "invalid chunk size %" PRIu64
				"; it must be >0, <= total-size, and block aligned\n",
				app->chunk_sizes[i]);
			return -EINVAL;
		}
	}

	bdev_bytes = app->block_count * (uint64_t)app->block_size;
	if (app->offset_bytes > bdev_bytes ||
	    app->total_size > bdev_bytes - app->offset_bytes) {
		fprintf(stderr, "requested range offset=%" PRIu64 " size=%" PRIu64
			" exceeds bdev size=%" PRIu64 "\n",
			app->offset_bytes, app->total_size, bdev_bytes);
		return -EINVAL;
	}
	return 0;
}

static int
alloc_buffers(struct app_context *app)
{
	int rc;

	rc = cuda_check(cudaSetDevice((int)app->cuda_device), "cudaSetDevice");
	if (rc != 0) {
		return rc;
	}
	rc = cuda_check(cudaStreamCreateWithFlags(&app->copy_stream,
						   cudaStreamNonBlocking),
			"cudaStreamCreateWithFlags");
	if (rc != 0) {
		return rc;
	}
	rc = cuda_check(cudaMalloc(&app->gpu_buf, (size_t)app->total_size),
			"cudaMalloc");
	if (rc != 0) {
		return rc;
	}

	for (uint32_t i = 0; i < STAGING_SLOT_COUNT; i++) {
		struct staging_slot *slot = &app->slots[i];

		slot->app = app;
		slot->slot_id = i;
		slot->cpu_buf = spdk_dma_zmalloc(app->max_chunk_size,
						 app->buf_align, NULL);
		if (!slot->cpu_buf) {
			return -ENOMEM;
		}
		if (cuda_check_nonfatal(cudaHostRegister(slot->cpu_buf,
							 (size_t)app->max_chunk_size,
							 cudaHostRegisterDefault),
					"cudaHostRegister(staging slot)") == 0) {
			slot->host_registered = true;
		}
		rc = cuda_check(cudaEventCreate(&slot->copy_start),
				"cudaEventCreate(copy_start)");
		if (rc != 0) {
			return rc;
		}
		rc = cuda_check(cudaEventCreate(&slot->copy_done),
				"cudaEventCreate(copy_done)");
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static void
free_buffers(struct app_context *app)
{
	for (uint32_t i = 0; i < STAGING_SLOT_COUNT; i++) {
		struct staging_slot *slot = &app->slots[i];

		if (slot->copy_pending && slot->copy_done) {
			cudaEventSynchronize(slot->copy_done);
		}
		if (slot->copy_start) {
			cudaEventDestroy(slot->copy_start);
		}
		if (slot->copy_done) {
			cudaEventDestroy(slot->copy_done);
		}
		if (slot->host_registered) {
			cudaHostUnregister(slot->cpu_buf);
		}
		if (slot->cpu_buf) {
			spdk_dma_free(slot->cpu_buf);
		}
		memset(slot, 0, sizeof(*slot));
	}
	if (app->gpu_buf) {
		cudaFree(app->gpu_buf);
		app->gpu_buf = NULL;
	}
	if (app->copy_stream) {
		cudaStreamDestroy(app->copy_stream);
		app->copy_stream = NULL;
	}
	free_chunk_sizes(app);
}

static void
bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
	      void *event_ctx)
{
	SPDK_NOTICELOG("bdev event type %d for %s\n", type, spdk_bdev_get_name(bdev));
}

static void
cleanup_and_stop(struct app_context *app, int rc)
{
	if (app->stopping) {
		return;
	}
	app->stopping = true;
	app->rc = rc;
	if (app->bdev_io_channel) {
		spdk_put_io_channel(app->bdev_io_channel);
		app->bdev_io_channel = NULL;
	}
	if (app->bdev_desc) {
		spdk_bdev_close(app->bdev_desc);
		app->bdev_desc = NULL;
	}
	spdk_app_stop(rc);
}

static void
pipeline_start(void *arg)
{
	struct app_context *app = arg;
	int rc;

	rc = spdk_bdev_open_ext(app->bdev_name, false, bdev_event_cb, NULL,
				&app->bdev_desc);
	if (rc != 0) {
		SPDK_ERRLOG("could not open bdev %s: %d\n", app->bdev_name, rc);
		cleanup_and_stop(app, rc);
		return;
	}
	app->bdev = spdk_bdev_desc_get_bdev(app->bdev_desc);
	app->bdev_io_channel = spdk_bdev_get_io_channel(app->bdev_desc);
	if (!app->bdev_io_channel) {
		cleanup_and_stop(app, -ENODEV);
		return;
	}

	app->block_size = spdk_bdev_get_block_size(app->bdev);
	app->block_count = spdk_bdev_get_num_blocks(app->bdev);
	app->buf_align = spdk_bdev_get_buf_align(app->bdev);

	rc = validate_sizes(app);
	if (rc != 0) {
		cleanup_and_stop(app, rc);
		return;
	}
	rc = alloc_buffers(app);
	if (rc != 0) {
		cleanup_and_stop(app, rc);
		return;
	}

	printf("chunk_pipeline_config bdev=%s total_size=%" PRIu64
	       " offset=%" PRIu64 " block_size=%u cuda_device=%u"
	       " max_chunk_size=%" PRIu64 " slots=%u host_registered=%s\n",
	       app->bdev_name, app->total_size, app->offset_bytes,
	       app->block_size, app->cuda_device, app->max_chunk_size,
	       STAGING_SLOT_COUNT,
	       (app->slots[0].host_registered && app->slots[1].host_registered)
		       ? "true"
		       : "false");

	app->run.kind = RUN_SERIAL;
	rc = start_current_run(app);
	if (rc != 0) {
		cleanup_and_stop(app, rc);
	}
}

static void
pipeline_usage(void)
{
	printf(" -b, --bdev NAME             bdev name [Raid0_2]\n");
	printf(" -T, --total-size SIZE       total transfer size [48M]\n");
	printf(" -C, --chunk-size SIZE       single chunk size to test\n");
	printf("     --chunk-sizes LIST      comma-separated chunk sizes [%s]\n",
	       DEFAULT_CHUNK_SIZES);
	printf(" -G, --cuda-device N         CUDA device [0]\n");
	printf(" -O, --offset SIZE           bdev byte offset [0]\n");
	printf("SPDK options: use -c PATH for JSON config; default is %s\n",
	       DEFAULT_JSON_CONFIG);
}

enum {
	OPT_CHUNK_SIZES = 1000,
};

static int
pipeline_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'b':
		g_app.bdev_name = arg;
		break;
	case 'T':
		if (parse_size(arg, &g_app.total_size) != 0) {
			return -EINVAL;
		}
		break;
	case 'C':
		if (set_single_chunk_size(&g_app, arg) != 0) {
			return -EINVAL;
		}
		break;
	case 'G':
		g_app.cuda_device = (uint32_t)strtoul(arg, NULL, 0);
		break;
	case 'O':
		if (parse_size(arg, &g_app.offset_bytes) != 0) {
			return -EINVAL;
		}
		break;
	case OPT_CHUNK_SIZES:
		if (set_chunk_size_list(&g_app, arg) != 0) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	const struct option long_opts[] = {
		{"bdev", required_argument, NULL, 'b'},
		{"total-size", required_argument, NULL, 'T'},
		{"chunk-size", required_argument, NULL, 'C'},
		{"chunk-sizes", required_argument, NULL, OPT_CHUNK_SIZES},
		{"cuda-device", required_argument, NULL, 'G'},
		{"offset", required_argument, NULL, 'O'},
		{NULL, 0, NULL, 0},
	};
	int rc;

	rc = set_chunk_size_list(&g_app, DEFAULT_CHUNK_SIZES);
	if (rc != 0) {
		return 1;
	}

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "chunk_pipeline";
	opts.json_config_file = DEFAULT_JSON_CONFIG;
	opts.reactor_mask = DEFAULT_REACTOR_MASK;
	opts.rpc_addr = NULL;

	rc = spdk_app_parse_args(argc, argv, &opts, "b:T:C:G:O:", long_opts,
				 pipeline_parse_arg, pipeline_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		free_chunk_sizes(&g_app);
		return rc;
	}

	rc = spdk_app_start(&opts, pipeline_start, &g_app);
	free_buffers(&g_app);
	spdk_app_fini();
	return rc;
}
