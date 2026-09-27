#ifndef SPDK_NVME_XDS_REGISTRY_H
#define SPDK_NVME_XDS_REGISTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*spdk_nvme_xds_translate_cb)(void *ctx, const char *peer_bdf,
					  const void *addr, uint64_t len,
					  uint64_t *dma_addr,
					  uint64_t *dma_len);

void spdk_nvme_xds_set_translate_cb(spdk_nvme_xds_translate_cb cb, void *ctx);

int spdk_nvme_xds_translate(const char *peer_bdf, const void *addr,
			    uint64_t len, uint64_t *dma_addr,
			    uint64_t *dma_len);

#ifdef __cplusplus
}
#endif

#endif
