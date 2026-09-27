/*   SPDX-License-Identifier: BSD-3-Clause
 */

#include "nvme_xds_registry.h"

#include <errno.h>
#include <stddef.h>

static spdk_nvme_xds_translate_cb g_xds_translate_cb;
static void *g_xds_translate_ctx;

void
spdk_nvme_xds_set_translate_cb(spdk_nvme_xds_translate_cb cb, void *ctx)
{
	__atomic_store_n(&g_xds_translate_ctx, ctx, __ATOMIC_RELEASE);
	__atomic_store_n(&g_xds_translate_cb, cb, __ATOMIC_RELEASE);
}

int
spdk_nvme_xds_translate(const char *peer_bdf, const void *addr, uint64_t len,
			uint64_t *dma_addr, uint64_t *dma_len)
{
	spdk_nvme_xds_translate_cb cb;
	void *ctx;

	cb = __atomic_load_n(&g_xds_translate_cb, __ATOMIC_ACQUIRE);
	if (!cb) {
		return -ENOENT;
	}

	ctx = __atomic_load_n(&g_xds_translate_ctx, __ATOMIC_ACQUIRE);
	return cb(ctx, peer_bdf, addr, len, dma_addr, dma_len);
}
