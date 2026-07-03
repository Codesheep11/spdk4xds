/*   SPDX-License-Identifier: BSD-3-Clause
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"

#include "xds_p2p_user.h"
#define SPDK_NVME_XDS_REGISTRY_DEFINE
#include "nvme_xds_registry.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SPDK_NVME_XDS_MAX_BDF_LEN 32

struct spdk_nvme_xds_dma_extent {
	uint64_t start_page;
	uint64_t page_count;
	uint64_t dma_addr;
};

struct spdk_nvme_xds_dma_table {
	uint64_t page_size;
	uint32_t segment_count;
	uint32_t extent_count;
	uint32_t refcnt;
	struct spdk_nvme_xds_dma_extent *extents;
};

struct spdk_nvme_xds_peer_mapping {
	struct spdk_pci_addr peer_addr;
	char peer_bdf[SPDK_NVME_XDS_MAX_BDF_LEN];
	struct xds_p2p_handle h;
	struct spdk_nvme_xds_dma_table *table;
};

struct spdk_nvme_xds_peer_map_array {
	uint32_t count;
	struct spdk_nvme_xds_peer_mapping *entries[];
};

struct spdk_nvme_xds_handle {
	void *gpu_ptr;
	uint64_t len;
	struct spdk_nvme_xds_peer_map_array *peer_maps;
	struct spdk_nvme_xds_handle *next;
};

struct spdk_nvme_xds_index_entry {
	uint64_t base;
	uint64_t end;
	struct spdk_nvme_xds_handle *handle;
};

struct spdk_nvme_xds_index {
	uint32_t count;
	struct spdk_nvme_xds_index_entry entries[];
};

struct spdk_nvme_xds_registry {
	pthread_mutex_t lock;
	struct xds_p2p_ctx ctx;
	bool initialized;
	struct spdk_nvme_xds_handle *head;
	struct spdk_nvme_xds_index *index;
	unsigned int current_gen;
	uint32_t active_readers[2];
};

static struct spdk_nvme_xds_registry g_xds = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

static bool
spdk_nvme_xds_u64_add_overflow(uint64_t a, uint64_t b, uint64_t *out)
{
	return __builtin_add_overflow(a, b, out);
}

static bool
spdk_nvme_xds_u64_mul_overflow(uint64_t a, uint64_t b, uint64_t *out)
{
	return __builtin_mul_overflow(a, b, out);
}

static uint64_t
spdk_nvme_xds_min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static int
spdk_nvme_xds_parse_peer_addr(const char *peer_bdf, struct spdk_pci_addr *peer_addr)
{
	if (!peer_bdf || peer_bdf[0] == '\0' || !peer_addr) {
		return -EINVAL;
	}

	return spdk_pci_addr_parse(peer_addr, peer_bdf);
}

static int
spdk_nvme_xds_format_peer_addr(const struct spdk_pci_addr *peer_addr,
				char *peer_bdf, size_t peer_bdf_size)
{
	if (!peer_addr || !peer_bdf) {
		return -EINVAL;
	}

	return spdk_pci_addr_fmt(peer_bdf, peer_bdf_size, peer_addr);
}

static bool
spdk_nvme_xds_peer_addr_equal(const struct spdk_pci_addr *lhs,
			      const struct spdk_pci_addr *rhs)
{
	if (!lhs || !rhs) {
		return false;
	}

	return spdk_pci_addr_compare(lhs, rhs) == 0;
}

static bool
spdk_nvme_xds_ranges_overlap(uint64_t base1, uint64_t len1, uint64_t base2, uint64_t len2)
{
	uint64_t end1, end2;

	if (spdk_nvme_xds_u64_add_overflow(base1, len1, &end1) ||
	    spdk_nvme_xds_u64_add_overflow(base2, len2, &end2)) {
		return true;
	}

	return base1 < end2 && base2 < end1;
}

static int
spdk_nvme_xds_index_entry_cmp(const void *lhs, const void *rhs)
{
	const struct spdk_nvme_xds_index_entry *a = lhs;
	const struct spdk_nvme_xds_index_entry *b = rhs;

	if (a->base < b->base) {
		return -1;
	}
	if (a->base > b->base) {
		return 1;
	}
	return 0;
}

static int
spdk_nvme_xds_build_index_locked(struct spdk_nvme_xds_index **out)
{
	struct spdk_nvme_xds_handle *h;
	struct spdk_nvme_xds_index *index;
	size_t alloc_size;
	uint32_t count = 0;
	uint32_t i = 0;
	uint64_t end;

	if (!out) {
		return -EINVAL;
	}

	for (h = g_xds.head; h != NULL; h = h->next) {
		count++;
	}

	if (count == 0) {
		*out = NULL;
		return 0;
	}

	alloc_size = sizeof(*index) + ((size_t)count * sizeof(index->entries[0]));
	index = calloc(1, alloc_size);
	if (!index) {
		return -ENOMEM;
	}

	index->count = count;
	for (h = g_xds.head; h != NULL; h = h->next) {
		if (spdk_nvme_xds_u64_add_overflow((uint64_t)(uintptr_t)h->gpu_ptr, h->len, &end)) {
			free(index);
			return -ERANGE;
		}

		index->entries[i].base = (uint64_t)(uintptr_t)h->gpu_ptr;
		index->entries[i].end = end;
		index->entries[i].handle = h;
		i++;
	}

	qsort(index->entries, index->count, sizeof(index->entries[0]),
	      spdk_nvme_xds_index_entry_cmp);
	*out = index;
	return 0;
}

static struct spdk_nvme_xds_handle *
spdk_nvme_xds_index_find(const struct spdk_nvme_xds_index *index, uint64_t addr)
{
	uint32_t left = 0;
	uint32_t right;

	if (!index || index->count == 0) {
		return NULL;
	}

	right = index->count;
	while (left < right) {
		uint32_t mid = left + ((right - left) / 2);
		const struct spdk_nvme_xds_index_entry *entry = &index->entries[mid];

		if (addr < entry->base) {
			right = mid;
		} else if (addr >= entry->end) {
			left = mid + 1;
		} else {
			return entry->handle;
		}
	}

	return NULL;
}

static unsigned int
spdk_nvme_xds_reader_acquire(const struct spdk_nvme_xds_index **index_out)
{
	unsigned int gen;

	for (;;) {
		gen = __atomic_load_n(&g_xds.current_gen, __ATOMIC_ACQUIRE) & 1U;
		__atomic_add_fetch(&g_xds.active_readers[gen], 1, __ATOMIC_ACQ_REL);

		if (gen == (__atomic_load_n(&g_xds.current_gen, __ATOMIC_ACQUIRE) & 1U)) {
			if (index_out) {
				*index_out = __atomic_load_n(&g_xds.index, __ATOMIC_ACQUIRE);
			}
			return gen;
		}

		__atomic_sub_fetch(&g_xds.active_readers[gen], 1, __ATOMIC_ACQ_REL);
	}
}

static void
spdk_nvme_xds_reader_release(unsigned int gen)
{
	__atomic_sub_fetch(&g_xds.active_readers[gen & 1U], 1, __ATOMIC_ACQ_REL);
}

static unsigned int
spdk_nvme_xds_flip_generation_locked(void)
{
	unsigned int old_gen = g_xds.current_gen & 1U;
	unsigned int new_gen = old_gen ^ 1U;

	__atomic_store_n(&g_xds.current_gen, new_gen, __ATOMIC_RELEASE);
	return old_gen;
}

static void
spdk_nvme_xds_wait_readers(unsigned int gen)
{
	while (__atomic_load_n(&g_xds.active_readers[gen & 1U], __ATOMIC_ACQUIRE) != 0) {
		sched_yield();
	}
}

static struct spdk_nvme_xds_peer_mapping *
spdk_nvme_xds_find_peer_mapping(const struct spdk_nvme_xds_peer_map_array *maps,
				 const char *peer_bdf,
				 const struct spdk_pci_addr *peer_addr)
{
	uint32_t i;
	struct spdk_nvme_xds_peer_mapping *mapping;

	if (!maps || (!peer_bdf && !peer_addr)) {
		return NULL;
	}

	for (i = 0; i < maps->count; i++) {
		mapping = maps->entries[i];

		if (peer_bdf && strcmp(mapping->peer_bdf, peer_bdf) == 0) {
			return mapping;
		}

		if (peer_addr && spdk_nvme_xds_peer_addr_equal(&mapping->peer_addr, peer_addr)) {
			return mapping;
		}
	}

	return NULL;
}

static struct spdk_nvme_xds_dma_table *
spdk_nvme_xds_alloc_dma_table(uint64_t page_size, uint32_t segment_count,
			       const uint64_t *dma_addrs)
{
	struct spdk_nvme_xds_dma_table *table;
	struct spdk_nvme_xds_dma_extent *extents;
	uint32_t i, extent_count;
	size_t bytes;
	uint64_t expected_next;

	if (page_size == 0 || segment_count == 0 || !dma_addrs) {
		return NULL;
	}

	table = calloc(1, sizeof(*table));
	if (!table) {
		return NULL;
	}

	bytes = (size_t)segment_count * sizeof(extents[0]);
	extents = calloc(segment_count, sizeof(extents[0]));
	if (!extents) {
		free(table);
		return NULL;
	}

	extent_count = 1;
	extents[0].start_page = 0;
	extents[0].page_count = 1;
	extents[0].dma_addr = dma_addrs[0];

	for (i = 1; i < segment_count; i++) {
		if (!spdk_nvme_xds_u64_add_overflow(dma_addrs[i - 1], page_size, &expected_next) &&
		    dma_addrs[i] == expected_next) {
			extents[extent_count - 1].page_count++;
			continue;
		}

		extents[extent_count].start_page = i;
		extents[extent_count].page_count = 1;
		extents[extent_count].dma_addr = dma_addrs[i];
		extent_count++;
	}

	if (extent_count < segment_count) {
		struct spdk_nvme_xds_dma_extent *tmp;

		bytes = (size_t)extent_count * sizeof(extents[0]);
		tmp = realloc(extents, bytes);
		if (tmp) {
			extents = tmp;
		}
	}

	table->page_size = page_size;
	table->segment_count = segment_count;
	table->extent_count = extent_count;
	table->extents = extents;
	table->refcnt = 1;
	return table;
}

static void
spdk_nvme_xds_dma_table_get(struct spdk_nvme_xds_dma_table *table)
{
	if (!table) {
		return;
	}

	table->refcnt++;
}

static void
spdk_nvme_xds_dma_table_put(struct spdk_nvme_xds_dma_table *table)
{
	if (!table) {
		return;
	}

	assert(table->refcnt > 0);
	table->refcnt--;
	if (table->refcnt == 0) {
		free(table->extents);
		free(table);
	}
}

static bool
spdk_nvme_xds_dma_tables_equal(const struct spdk_nvme_xds_dma_table *lhs,
				const struct spdk_nvme_xds_dma_table *rhs)
{
	if (!lhs || !rhs) {
		return false;
	}

	if (lhs->page_size != rhs->page_size ||
	    lhs->segment_count != rhs->segment_count ||
	    lhs->extent_count != rhs->extent_count) {
		return false;
	}

	return memcmp(lhs->extents, rhs->extents,
		      (size_t)lhs->extent_count * sizeof(lhs->extents[0])) == 0;
}

static struct spdk_nvme_xds_dma_table *
spdk_nvme_xds_find_shared_dma_table(const struct spdk_nvme_xds_peer_map_array *maps,
				     const struct spdk_nvme_xds_dma_table *candidate)
{
	uint32_t i;

	if (!maps || !candidate) {
		return NULL;
	}

	for (i = 0; i < maps->count; i++) {
		struct spdk_nvme_xds_peer_mapping *mapping = maps->entries[i];

		if (spdk_nvme_xds_dma_tables_equal(mapping->table, candidate)) {
			return mapping->table;
		}
	}

	return NULL;
}

static const struct spdk_nvme_xds_dma_extent *
spdk_nvme_xds_find_extent(const struct spdk_nvme_xds_dma_table *table,
			   uint64_t page_index)
{
	uint32_t left = 0;
	uint32_t right;

	if (!table || !table->extents || table->extent_count == 0) {
		return NULL;
	}

	right = table->extent_count;
	while (left < right) {
		uint32_t mid = left + ((right - left) / 2);
		const struct spdk_nvme_xds_dma_extent *extent = &table->extents[mid];
		uint64_t end_page;

		if (spdk_nvme_xds_u64_add_overflow(extent->start_page, extent->page_count,
						    &end_page)) {
			return NULL;
		}

		if (page_index < extent->start_page) {
			right = mid;
		} else if (page_index >= end_page) {
			left = mid + 1;
		} else {
			return extent;
		}
	}

	return NULL;
}

static int
spdk_nvme_xds_translate_cached(const struct spdk_nvme_xds_handle *buf,
				const struct spdk_nvme_xds_peer_mapping *mapping,
				const void *addr,
				uint64_t len,
				uint64_t *dma_addr,
				uint64_t *dma_len)
{
	const struct spdk_nvme_xds_dma_table *table;
	const struct spdk_nvme_xds_dma_extent *extent;
	uint64_t base, offset, page_index, page_off, remaining, max_len, span_len;
	uint64_t extent_page_off, extent_byte_off, extent_remaining_pages, extent_remaining_bytes, dma_base;

	if (!buf || !mapping || !addr || !dma_addr || !dma_len) {
		return -EINVAL;
	}

	if (xds_p2p_handle_invalidated(&mapping->h)) {
		return -EIO;
	}

	table = mapping->table;
	if (!table || table->page_size == 0 || table->segment_count == 0 ||
	    table->extent_count == 0 || !table->extents) {
		return -EINVAL;
	}

	base = (uint64_t)(uintptr_t)buf->gpu_ptr;
	offset = (uint64_t)(uintptr_t)addr - base;
	if (offset >= buf->len) {
		return -ERANGE;
	}

	page_index = offset / table->page_size;
	page_off = offset % table->page_size;
	if (page_index >= table->segment_count) {
		return -ERANGE;
	}

	extent = spdk_nvme_xds_find_extent(table, page_index);
	if (!extent) {
		return -ERANGE;
	}

	extent_page_off = page_index - extent->start_page;
	extent_remaining_pages = extent->page_count - extent_page_off;
	if (spdk_nvme_xds_u64_mul_overflow(extent_page_off, table->page_size, &extent_byte_off) ||
	    spdk_nvme_xds_u64_add_overflow(extent->dma_addr, extent_byte_off, &dma_base) ||
	    spdk_nvme_xds_u64_add_overflow(dma_base, page_off, dma_addr)) {
		return -ERANGE;
	}

	remaining = buf->len - offset;
	max_len = spdk_nvme_xds_min_u64(remaining, len);
	if (spdk_nvme_xds_u64_mul_overflow(extent_remaining_pages, table->page_size,
					    &extent_remaining_bytes) ||
	    extent_remaining_bytes < page_off) {
		return -ERANGE;
	}

	span_len = spdk_nvme_xds_min_u64(extent_remaining_bytes - page_off, max_len);

	*dma_len = span_len;
	return 0;
}

static void
spdk_nvme_xds_free_peer_mapping_locked(struct spdk_nvme_xds_peer_mapping *mapping)
{
	int rc;

	if (!mapping) {
		return;
	}

	rc = xds_p2p_unregister(&g_xds.ctx, &mapping->h);
	if (rc != 0) {
		SPDK_ERRLOG("xDS backend unregister(%s) failed rc=%d (%s)\n",
			    mapping->peer_bdf, rc, xds_p2p_strerror(rc));
	}

	spdk_nvme_xds_dma_table_put(mapping->table);
	free(mapping);
}

static int
spdk_nvme_xds_free_handle_locked(struct spdk_nvme_xds_handle *handle)
{
	struct spdk_nvme_xds_peer_map_array *maps;
	uint32_t i;
	int rc;
	int first_rc = 0;

	if (!handle) {
		return 0;
	}

	maps = handle->peer_maps;
	if (maps) {
		for (i = 0; i < maps->count; i++) {
			rc = xds_p2p_unregister(&g_xds.ctx, &maps->entries[i]->h);
			if (rc != 0 && first_rc == 0) {
				first_rc = rc;
				SPDK_ERRLOG("xDS backend unregister(%s) failed rc=%d (%s)\n",
					    maps->entries[i]->peer_bdf, rc, xds_p2p_strerror(rc));
			}

			spdk_nvme_xds_dma_table_put(maps->entries[i]->table);
			free(maps->entries[i]);
		}

		free(maps);
	}

	free(handle);
	return first_rc;
}

static int
spdk_nvme_xds_add_peer_mapping_locked(struct spdk_nvme_xds_handle *handle,
				       const char *peer_bdf,
				       struct spdk_nvme_xds_peer_mapping **out)
{
	struct spdk_nvme_xds_peer_map_array *old_maps, *new_maps;
	struct spdk_nvme_xds_peer_mapping *mapping;
	struct spdk_pci_addr peer_addr;
	size_t alloc_size;
	uint32_t old_count, i;
	int rc;

	if (!handle || !peer_bdf || !out || peer_bdf[0] == '\0') {
		return -EINVAL;
	}

	rc = spdk_nvme_xds_parse_peer_addr(peer_bdf, &peer_addr);
	if (rc != 0) {
		return rc;
	}

	old_maps = handle->peer_maps;
	mapping = spdk_nvme_xds_find_peer_mapping(old_maps, peer_bdf, &peer_addr);
	if (mapping) {
		*out = mapping;
		return 0;
	}

	mapping = calloc(1, sizeof(*mapping));
	if (!mapping) {
		return -ENOMEM;
	}

	mapping->peer_addr = peer_addr;
	rc = spdk_nvme_xds_format_peer_addr(&mapping->peer_addr, mapping->peer_bdf,
					     sizeof(mapping->peer_bdf));
	if (rc != 0) {
		free(mapping);
		return rc;
	}

	rc = xds_p2p_register_nvidia(&g_xds.ctx, handle->gpu_ptr, handle->len, mapping->peer_bdf, 0, &mapping->h);
	if (rc != 0) {
		SPDK_ERRLOG("xDS backend register(%s) failed rc=%d (%s)\n",
			    mapping->peer_bdf, rc, xds_p2p_strerror(rc));
		free(mapping);
		return rc;
	}

	mapping->table = spdk_nvme_xds_alloc_dma_table(mapping->h.page_size,
							 mapping->h.segment_count,
							 mapping->h.dma_addrs);
	if (!mapping->table) {
		spdk_nvme_xds_free_peer_mapping_locked(mapping);
		return -ENOMEM;
	}

	free(mapping->h.dma_addrs);
	mapping->h.dma_addrs = NULL;

	{
		struct spdk_nvme_xds_dma_table *shared;

		shared = spdk_nvme_xds_find_shared_dma_table(old_maps, mapping->table);
		if (shared) {
			spdk_nvme_xds_dma_table_get(shared);
			spdk_nvme_xds_dma_table_put(mapping->table);
			mapping->table = shared;
		}
	}

	old_count = old_maps ? old_maps->count : 0;
	alloc_size = sizeof(*new_maps) + ((size_t)(old_count + 1) * sizeof(new_maps->entries[0]));
	new_maps = calloc(1, alloc_size);
	if (!new_maps) {
		spdk_nvme_xds_free_peer_mapping_locked(mapping);
		return -ENOMEM;
	}

	new_maps->count = old_count + 1;
	for (i = 0; i < old_count; i++) {
		new_maps->entries[i] = old_maps->entries[i];
	}
	new_maps->entries[old_count] = mapping;

	handle->peer_maps = new_maps;
	free(old_maps);

	*out = mapping;
	return 0;
}

int
spdk_nvme_xds_init(const char *devnode)
{
	int rc;

	pthread_mutex_lock(&g_xds.lock);
	if (g_xds.initialized) {
		pthread_mutex_unlock(&g_xds.lock);
		return 0;
	}

	rc = xds_p2p_open(&g_xds.ctx, XDS_P2P_BACKEND_NVIDIA, devnode);
	if (rc == 0) {
		g_xds.initialized = true;
		g_xds.head = NULL;
		g_xds.index = NULL;
		g_xds.current_gen = 0;
		g_xds.active_readers[0] = 0;
		g_xds.active_readers[1] = 0;
	}

	pthread_mutex_unlock(&g_xds.lock);
	return rc;
}

void
spdk_nvme_xds_fini(void)
{
	struct spdk_nvme_xds_handle *head;
	struct spdk_nvme_xds_handle *next;
	struct spdk_nvme_xds_index *old_index;
	unsigned int drain_gen;

	pthread_mutex_lock(&g_xds.lock);
	if (!g_xds.initialized) {
		pthread_mutex_unlock(&g_xds.lock);
		return;
	}

	head = g_xds.head;
	g_xds.head = NULL;

	old_index = g_xds.index;
	__atomic_store_n(&g_xds.index, NULL, __ATOMIC_RELEASE);
	drain_gen = spdk_nvme_xds_flip_generation_locked();
	spdk_nvme_xds_wait_readers(drain_gen);
	free(old_index);
	g_xds.index = NULL;
	g_xds.initialized = false;
	pthread_mutex_unlock(&g_xds.lock);

	for (; head != NULL; head = next) {
		next = head->next;
		pthread_mutex_lock(&g_xds.lock);
		(void)spdk_nvme_xds_free_handle_locked(head);
		pthread_mutex_unlock(&g_xds.lock);
	}

	xds_p2p_close(&g_xds.ctx);
}

int
spdk_nvme_xds_register(void *gpu_ptr, uint64_t len,
			const char *const *peer_bdfs, size_t count,
			struct spdk_nvme_xds_handle **out)
{
	struct spdk_nvme_xds_handle *handle, *it;
	struct spdk_nvme_xds_peer_mapping *mapping;
	struct spdk_nvme_xds_index *new_index, *old_index;
	unsigned int drain_gen;
	uint64_t base, end;
	size_t i;
	int rc;

	if (!gpu_ptr || len == 0 || !out || !peer_bdfs || count == 0) {
		return -EINVAL;
	}

	base = (uint64_t)(uintptr_t)gpu_ptr;
	if (spdk_nvme_xds_u64_add_overflow(base, len, &end)) {
		return -ERANGE;
	}

	handle = calloc(1, sizeof(*handle));
	if (!handle) {
		return -ENOMEM;
	}

	handle->gpu_ptr = gpu_ptr;
	handle->len = len;

	pthread_mutex_lock(&g_xds.lock);
	if (!g_xds.initialized) {
		pthread_mutex_unlock(&g_xds.lock);
		free(handle);
		return -EINVAL;
	}

	for (it = g_xds.head; it != NULL; it = it->next) {
		if (spdk_nvme_xds_ranges_overlap(base, len,
						  (uint64_t)(uintptr_t)it->gpu_ptr, it->len)) {
			pthread_mutex_unlock(&g_xds.lock);
			free(handle);
			return -EEXIST;
		}
	}

	for (i = 0; i < count; i++) {
		if (!peer_bdfs[i] || peer_bdfs[i][0] == '\0') {
			pthread_mutex_unlock(&g_xds.lock);
			free(handle);
			return -EINVAL;
		}

		rc = spdk_nvme_xds_add_peer_mapping_locked(handle, peer_bdfs[i], &mapping);
		if (rc != 0) {
			(void)spdk_nvme_xds_free_handle_locked(handle);
			pthread_mutex_unlock(&g_xds.lock);
			return rc;
		}
	}

	handle->next = g_xds.head;
	g_xds.head = handle;

	rc = spdk_nvme_xds_build_index_locked(&new_index);
	if (rc != 0) {
		g_xds.head = handle->next;
		(void)spdk_nvme_xds_free_handle_locked(handle);
		pthread_mutex_unlock(&g_xds.lock);
		return rc;
	}

	old_index = g_xds.index;
	__atomic_store_n(&g_xds.index, new_index, __ATOMIC_RELEASE);
	drain_gen = spdk_nvme_xds_flip_generation_locked();
	spdk_nvme_xds_wait_readers(drain_gen);
	free(old_index);
	g_xds.index = new_index;

	pthread_mutex_unlock(&g_xds.lock);

	*out = handle;
	return 0;
}

int
spdk_nvme_xds_unregister(struct spdk_nvme_xds_handle *handle)
{
	struct spdk_nvme_xds_handle **pp;
	struct spdk_nvme_xds_index *new_index, *old_index;
	unsigned int drain_gen;
	int rc;

	if (!handle) {
		return -EINVAL;
	}

	pthread_mutex_lock(&g_xds.lock);
	if (!g_xds.initialized) {
		pthread_mutex_unlock(&g_xds.lock);
		return -EINVAL;
	}

	for (pp = &g_xds.head; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == handle) {
			break;
		}
	}

	if (*pp == NULL) {
		pthread_mutex_unlock(&g_xds.lock);
		return -ENOENT;
	}

	*pp = handle->next;

	rc = spdk_nvme_xds_build_index_locked(&new_index);
	if (rc != 0) {
		handle->next = g_xds.head;
		g_xds.head = handle;
		pthread_mutex_unlock(&g_xds.lock);
		return rc;
	}

	old_index = g_xds.index;
	__atomic_store_n(&g_xds.index, new_index, __ATOMIC_RELEASE);
	drain_gen = spdk_nvme_xds_flip_generation_locked();
	spdk_nvme_xds_wait_readers(drain_gen);
	free(old_index);
	g_xds.index = new_index;

	rc = spdk_nvme_xds_free_handle_locked(handle);
	pthread_mutex_unlock(&g_xds.lock);
	return rc;
}

int
spdk_nvme_xds_translate(const char *peer_bdf, const void *addr, uint64_t len,
			 uint64_t *dma_addr, uint64_t *dma_len)
{
	const struct spdk_nvme_xds_index *index;
	const struct spdk_nvme_xds_peer_map_array *maps;
	struct spdk_nvme_xds_handle *handle;
	struct spdk_nvme_xds_peer_mapping *mapping;
	unsigned int gen;
	uint64_t vaddr;
	int rc;

	if (!peer_bdf || peer_bdf[0] == '\0' || !addr || !dma_addr || !dma_len || len == 0) {
		return -EINVAL;
	}

	if (!__atomic_load_n(&g_xds.initialized, __ATOMIC_ACQUIRE)) {
		return -ENOENT;
	}

	vaddr = (uint64_t)(uintptr_t)addr;
	gen = spdk_nvme_xds_reader_acquire(&index);
	handle = spdk_nvme_xds_index_find(index, vaddr);
	if (!handle) {
		spdk_nvme_xds_reader_release(gen);
		return -ENOENT;
	}

	maps = handle->peer_maps;
	mapping = spdk_nvme_xds_find_peer_mapping(maps, peer_bdf, NULL);
	if (!mapping) {
		struct spdk_pci_addr peer_addr;

		if (spdk_nvme_xds_parse_peer_addr(peer_bdf, &peer_addr) == 0) {
			mapping = spdk_nvme_xds_find_peer_mapping(maps, NULL, &peer_addr);
		}
	}
	if (mapping) {
		rc = spdk_nvme_xds_translate_cached(handle, mapping, addr, len, dma_addr, dma_len);
		spdk_nvme_xds_reader_release(gen);
		return rc;
	}

	spdk_nvme_xds_reader_release(gen);
	return -ENOENT;
}
