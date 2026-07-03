#ifndef SPDK_NVME_XDS_REGISTRY_H
#define SPDK_NVME_XDS_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

struct spdk_nvme_xds_handle;

#if defined(__GNUC__) && !defined(SPDK_NVME_XDS_REGISTRY_DEFINE)
#define SPDK_NVME_XDS_OPTIONAL __attribute__((weak))
#else
#define SPDK_NVME_XDS_OPTIONAL
#endif

int spdk_nvme_xds_init(const char *devnode);
void spdk_nvme_xds_fini(void);

int spdk_nvme_xds_register(void *gpu_ptr, uint64_t len,
			    const char *const *peer_bdfs, size_t count,
			    struct spdk_nvme_xds_handle **out);
int spdk_nvme_xds_unregister(struct spdk_nvme_xds_handle *h);

int spdk_nvme_xds_translate(const char *peer_bdf, const void *addr, uint64_t len,
			     uint64_t *dma_addr, uint64_t *dma_len) SPDK_NVME_XDS_OPTIONAL;

#undef SPDK_NVME_XDS_OPTIONAL

#endif
