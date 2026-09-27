/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "nvme/nvme_pcie_common.c"
#include "common/lib/test_env.c"

SPDK_LOG_REGISTER_COMPONENT(nvme)

pid_t g_spdk_nvme_pid;
DEFINE_STUB(nvme_ctrlr_get_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr, pid_t pid), NULL);

DEFINE_STUB(nvme_ctrlr_submit_admin_request, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_request *req), 0);

DEFINE_STUB_V(nvme_completion_poll_cb, (void *arg, const struct spdk_nvme_cpl *cpl));
DEFINE_STUB(nvme_wait_for_adminq_completion, int, (struct spdk_nvme_ctrlr *ctrlr,
		struct nvme_completion_poll_status *status, bool release), 0);

DEFINE_STUB(nvme_completion_is_retry, bool, (const struct spdk_nvme_cpl *cpl), false);

DEFINE_STUB_V(spdk_nvme_qpair_print_command, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cmd *cmd));

DEFINE_STUB_V(spdk_nvme_qpair_print_completion, (struct spdk_nvme_qpair *qpair,
		struct spdk_nvme_cpl *cpl));

DEFINE_STUB_V(nvme_qpair_deinit, (struct spdk_nvme_qpair *qpair));

DEFINE_STUB(nvme_ctrlr_get_current_process, struct spdk_nvme_ctrlr_process *,
	    (struct spdk_nvme_ctrlr *ctrlr), NULL);

DEFINE_STUB(spdk_nvme_qpair_process_completions, int32_t,
	    (struct spdk_nvme_qpair *qpair, uint32_t max_completions), 0);

DEFINE_STUB(nvme_request_check_timeout, int, (struct nvme_request *req, uint16_t cid,
		struct spdk_nvme_ctrlr_process *active_proc, uint64_t now_tick), 0);
DEFINE_STUB(spdk_strerror, const char *, (int errnum), NULL);

DEFINE_STUB(spdk_nvme_xds_translate, int,
	    (const char *peer_bdf, const void *addr, uint64_t len,
	     uint64_t *dma_addr, uint64_t *dma_len), -ENOTSUP);

DEFINE_STUB_V(nvme_ctrlr_disable, (struct spdk_nvme_ctrlr *ctrlr));

DEFINE_STUB(nvme_ctrlr_disable_poll, int, (struct spdk_nvme_ctrlr *ctrlr), 0);

DEFINE_STUB_V(nvme_transport_ctrlr_disconnect_qpair_done, (struct spdk_nvme_qpair *qpair));
DEFINE_STUB(spdk_nvme_ctrlr_get_numa_id, int32_t, (struct spdk_nvme_ctrlr *ctrlr),
	    SPDK_ENV_NUMA_ID_ANY);
DEFINE_STUB(nvme_qpair_state_string, const char *, (enum nvme_qpair_state state), NULL);

int
nvme_qpair_init(struct spdk_nvme_qpair *qpair, uint16_t id,
		struct spdk_nvme_ctrlr *ctrlr,
		enum spdk_nvme_qprio qprio,
		uint32_t num_requests, bool async)
{
	qpair->id = id;
	qpair->qprio = qprio;
	qpair->ctrlr = ctrlr;
	qpair->async = async;

	return 0;
}

static void
test_nvme_pcie_ctrlr_alloc_cmb(void)
{

	struct nvme_pcie_ctrlr pctrlr = {};
	void *vaddr = NULL;
	uint64_t alignment;
	uint64_t phys_addr_var;
	uint64_t size;

	size = 64;
	alignment = 4096;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	pctrlr.cmb.size = 1 << 16;

	/* Allocate CMB */
	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == (void *)0xF9001000);
	CU_ASSERT(phys_addr_var == 0xF8001000);
	CU_ASSERT(pctrlr.cmb.current_offset == 4160);

	/* CMB size overload */
	size = 0x1000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	SPDK_CU_ASSERT_FATAL(vaddr == NULL);

	/* BAR is mapped for data */
	pctrlr.cmb.mem_register_addr = (void *)0xF0000000;

	vaddr = nvme_pcie_ctrlr_alloc_cmb(&pctrlr.ctrlr, size, alignment, &phys_addr_var);
	CU_ASSERT(vaddr == NULL);
}

static void
test_nvme_pcie_qpair_construct_destroy(void)
{
	struct spdk_nvme_io_qpair_opts opts = {};
	struct nvme_pcie_ctrlr pctrlr = {};
	struct spdk_nvme_cpl cpl[2] = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	size_t page_align = sysconf(_SC_PAGESIZE);
	uint64_t cmb_offset;
	int rc;

	opts.sq.paddr = 0xDEADBEEF;
	opts.cq.paddr = 0xDBADBEEF;
	opts.sq.vaddr = (void *)0xDCADBEEF;
	opts.cq.vaddr = cpl;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.transport_retry_count = 1;
	pctrlr.cmb.mem_register_addr = NULL;
	pctrlr.cmb.bar_va = (void *)0xF9000000;
	pctrlr.cmb.bar_pa = 0xF8000000;
	pctrlr.cmb.current_offset = 0x10;
	cmb_offset = pctrlr.cmb.current_offset;
	/* Make sure that CMB size is big enough and includes page alignment */
	pctrlr.cmb.size = (1 << 16) + page_align;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;

	/* Allocate memory for destroying. */
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	/* Enable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = true;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == true);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == (((pctrlr.cmb.bar_pa + cmb_offset) + page_align - 1) & ~
					   (page_align - 1)));
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	CU_ASSERT(pctrlr.cmb.current_offset == (uintptr_t)pqpair->cmd + (pqpair->num_entries * sizeof(
				struct spdk_nvme_cmd)) - (uintptr_t)pctrlr.cmb.bar_va);
	cmb_offset = pctrlr.cmb.current_offset;
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, &opts);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->sq_vaddr == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cq_vaddr == cpl);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd == (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDEADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000C);
	CU_ASSERT(pqpair->flags.phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);

	/* Disable submission queue in controller memory buffer, sq_vaddr and cq_vaddr invalid. */
	pctrlr.ctrlr.opts.use_cmb_sqs = false;
	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL,
			      SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_SHARE);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	pqpair->qpair.ctrlr = &pctrlr.ctrlr;
	pqpair->num_entries = 2;
	pqpair->qpair.id = 1;
	pqpair->cpl = cpl;
	MOCK_SET(spdk_vtophys, 0xDAADBEEF);

	rc = nvme_pcie_qpair_construct(&pqpair->qpair, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(pqpair->retry_count == 1);
	CU_ASSERT(pqpair->max_completions_cap == 1);
	CU_ASSERT(pqpair->cmd != NULL && pqpair->cmd != (void *)0xDCADBEEF);
	CU_ASSERT(pqpair->sq_in_cmb == false);
	CU_ASSERT(pqpair->cmd_bus_addr == 0xDAADBEEF);
	CU_ASSERT(pqpair->sq_tdbl == (void *)0xF7000008);
	CU_ASSERT(pqpair->cq_hdbl == (void *)0xF700000c);
	CU_ASSERT(pqpair->flags.phase == 1);
	CU_ASSERT(pqpair->tr != NULL);
	CU_ASSERT(pqpair->tr == TAILQ_FIRST(&pqpair->free_tr));
	nvme_pcie_qpair_destroy(&pqpair->qpair);
	MOCK_CLEAR(spdk_vtophys);
}

static void
test_nvme_pcie_ctrlr_cmd_create_delete_io_queue(void)
{
	struct spdk_nvme_ctrlr ctrlr = {};
	struct nvme_pcie_qpair pqpair = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req = {};
	int rc;

	ctrlr.adminq = &adminq;
	STAILQ_INIT(&ctrlr.adminq->free_req);
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;

	rc = nvme_pcie_ctrlr_cmd_create_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0xDEADBEEF);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req.cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req.cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req.cmd.dptr.prp.prp1 == 0xDDADBEEF);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	/* No free request available */
	rc = nvme_pcie_ctrlr_cmd_create_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = nvme_pcie_ctrlr_cmd_create_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	/* Delete cq or sq */
	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	CU_ASSERT(req.cmd.cdw10_bits.delete_io_q.qid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	memset(&req, 0, sizeof(req));
	STAILQ_INSERT_HEAD(&ctrlr.adminq->free_req, &req, stailq);

	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req.cmd.opc == SPDK_NVME_OPC_DELETE_IO_SQ);
	CU_ASSERT(req.cmd.cdw10_bits.delete_io_q.qid == 1);
	CU_ASSERT(STAILQ_EMPTY(&ctrlr.adminq->free_req));

	/* No free request available */
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);

	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(&ctrlr, &pqpair.qpair, NULL, NULL);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_nvme_pcie_ctrlr_connect_qpair(void)
{
	struct nvme_pcie_ctrlr	pctrlr = {};
	struct nvme_pcie_qpair	pqpair = {};
	struct spdk_nvme_transport_poll_group poll_group = {};
	struct spdk_nvme_cpl cpl = {};
	struct spdk_nvme_qpair adminq = {};
	struct nvme_request req[3] = {};
	int rc;

	pqpair.cpl = &cpl;
	pqpair.num_entries = 1;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	pctrlr.ctrlr.page_size = 4096;

	/* Shadow doorbell available */
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.shadow_doorbell = spdk_zmalloc(pctrlr.ctrlr.page_size, pctrlr.ctrlr.page_size,
				       NULL, SPDK_ENV_LCORE_ID_ANY,
				       SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	pctrlr.ctrlr.eventidx = spdk_zmalloc(pctrlr.ctrlr.page_size, pctrlr.ctrlr.page_size,
					     NULL, SPDK_ENV_LCORE_ID_ANY,
					     SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE);
	pctrlr.ctrlr.adminq = &adminq;
	STAILQ_INIT(&pctrlr.ctrlr.adminq->free_req);
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[0].cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req[0].cmd.dptr.prp.prp1 == 0xDEADBEEF);

	/* Complete the first request, which triggers the second. */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	/* doorbell stride and qid are 1 */
	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == pctrlr.ctrlr.shadow_doorbell + 2);
	CU_ASSERT(pqpair.shadow_doorbell.cq_hdbl == pctrlr.ctrlr.shadow_doorbell + 3);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == pctrlr.ctrlr.eventidx + 2);
	CU_ASSERT(pqpair.shadow_doorbell.cq_eventidx == pctrlr.ctrlr.eventidx + 3);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 1);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	spdk_free(pctrlr.ctrlr.shadow_doorbell);
	spdk_free(pctrlr.ctrlr.eventidx);
	pctrlr.ctrlr.shadow_doorbell = NULL;
	pctrlr.ctrlr.eventidx = NULL;

	/* Shadow doorbell 0 */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[0].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[0].cmd.cdw11_bits.create_io_cq.pc == 1);
	CU_ASSERT(req[0].cmd.dptr.prp.prp1 == 0xDEADBEEF);

	/* Complete the first request, which triggers the second. */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qid == 1);
	CU_ASSERT(req[1].cmd.cdw10_bits.create_io_q.qsize == 0);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.pc == 1);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.qprio == SPDK_NVME_QPRIO_HIGH);
	CU_ASSERT(req[1].cmd.cdw11_bits.create_io_sq.cqid == 1);
	CU_ASSERT(req[1].cmd.dptr.prp.prp1 == 0xDDADBEEF);

	pqpair.qpair.state = NVME_QPAIR_CONNECTING;
	/* Complete the second request */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_READY);
	/* State is still CONNECTING until the thread is polled again. */
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_CONNECTING);

	CU_ASSERT(pqpair.shadow_doorbell.sq_tdbl == NULL);
	CU_ASSERT(pqpair.shadow_doorbell.sq_eventidx == NULL);
	CU_ASSERT(pqpair.flags.has_shadow_doorbell == 0);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	/* Completion error for CQ */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;
	/* Modify cpl such that CQ fails */
	pqpair.cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	for (int i = 0; i < 2; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);

	/* Request to complete callback in async operation */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);

	/* Remove unused request */
	STAILQ_REMOVE_HEAD(&pctrlr.ctrlr.adminq->free_req, stailq);
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));

	/* Completion error for SQ */
	memset(req, 0, sizeof(struct nvme_request) * 3);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	pqpair.qpair.poll_group = &poll_group;
	for (int i = 0; i < 3; i++) {
		STAILQ_INSERT_TAIL(&pctrlr.ctrlr.adminq->free_req, &req[i], stailq);
	}

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == 0);
	CU_ASSERT(req[0].cmd.opc == SPDK_NVME_OPC_CREATE_IO_CQ);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_CQ);

	/* Request to complete cq callback in async operation */
	req[0].cb_fn(req[0].cb_arg, &cpl);
	CU_ASSERT(req[1].cmd.opc == SPDK_NVME_OPC_CREATE_IO_SQ);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_WAIT_FOR_SQ);
	/* Modify cpl such that SQ fails */
	pqpair.cpl->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;

	/* Request to complete sq callback in async operation */
	req[1].cb_fn(req[1].cb_arg, &cpl);
	CU_ASSERT(req[2].cmd.opc == SPDK_NVME_OPC_DELETE_IO_CQ);
	/* Modify cpl back to success */
	pqpair.cpl->status.sc = SPDK_NVME_SC_SUCCESS;
	pqpair.cpl->status.sct = SPDK_NVME_SCT_GENERIC;
	req[2].cb_fn(req[2].cb_arg, &cpl);
	CU_ASSERT(pqpair.pcie_state == NVME_PCIE_QPAIR_FAILED);
	CU_ASSERT(pqpair.qpair.state == NVME_QPAIR_DISCONNECTED);
	/* No need to remove unused requests here */
	CU_ASSERT(STAILQ_EMPTY(&pctrlr.ctrlr.adminq->free_req));


	/* No available request used */
	memset(req, 0, sizeof(struct nvme_request) * 2);
	memset(&pqpair, 0, sizeof(pqpair));
	pqpair.cpl = &cpl;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pqpair.qpair.id = 1;
	pqpair.num_entries = 1;
	pqpair.cpl_bus_addr = 0xDEADBEEF;
	pqpair.cmd_bus_addr = 0xDDADBEEF;
	pqpair.qpair.qprio = SPDK_NVME_QPRIO_HIGH;
	pqpair.stat = NULL;
	pqpair.qpair.poll_group = &poll_group;

	rc = nvme_pcie_ctrlr_connect_qpair(&pctrlr.ctrlr, &pqpair.qpair);
	CU_ASSERT(rc == -ENOMEM);
}

static void
test_nvme_pcie_ctrlr_construct_admin_qpair(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair *pqpair = NULL;
	int rc = 0;

	pctrlr.ctrlr.trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr.ctrlr.opts.admin_queue_size = 32;
	pctrlr.doorbell_base = (void *)0xF7000000;
	pctrlr.doorbell_stride_u32 = 1;
	pctrlr.ctrlr.flags = 0;
	pctrlr.ctrlr.free_io_qids = NULL;
	pctrlr.ctrlr.is_resetting = false;
	pctrlr.ctrlr.is_failed = false;
	pctrlr.ctrlr.is_destructed = false;
	pctrlr.ctrlr.outstanding_aborts = 0;
	pctrlr.ctrlr.ana_log_page = NULL;
	pctrlr.ctrlr.ana_log_page_size = 0;

	TAILQ_INIT(&pctrlr.ctrlr.active_io_qpairs);
	STAILQ_INIT(&pctrlr.ctrlr.queued_aborts);
	TAILQ_INIT(&pctrlr.ctrlr.active_procs);

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr.ctrlr, 32);
	CU_ASSERT(rc == 0);
	pqpair = nvme_pcie_qpair(pctrlr.ctrlr.adminq);
	SPDK_CU_ASSERT_FATAL(pqpair != NULL);
	CU_ASSERT(pqpair->num_entries == 32);
	CU_ASSERT(pqpair->flags.delay_cmd_submit == 0);
	CU_ASSERT(pqpair->qpair.id == 0);
	CU_ASSERT(pqpair->qpair.qprio == SPDK_NVME_QPRIO_URGENT);
	CU_ASSERT(pqpair->qpair.ctrlr == &pctrlr.ctrlr);
	CU_ASSERT(pqpair->stat != NULL);

	rc = nvme_pcie_qpair_destroy(pctrlr.ctrlr.adminq);
	CU_ASSERT(rc == 0);
}

static void
test_nvme_pcie_poll_group_get_stats(void)
{
	int rc = 0;
	struct nvme_pcie_poll_group *pgroup = NULL;
	struct spdk_nvme_transport_poll_group *tgroup = NULL;
	struct spdk_nvme_transport_poll_group_stat *tgroup_stat = NULL;

	tgroup = nvme_pcie_poll_group_create();
	CU_ASSERT(tgroup != NULL);
	pgroup = SPDK_CONTAINEROF(tgroup, struct nvme_pcie_poll_group, group);
	CU_ASSERT(pgroup != NULL);

	/* Invalid group pointer, expect fail and return -EINVAL */
	rc = nvme_pcie_poll_group_get_stats(NULL, &tgroup_stat);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(tgroup_stat == NULL);

	/* Invalid stats, expect fail and return -EINVAL */
	rc = nvme_pcie_poll_group_get_stats(tgroup, NULL);
	CU_ASSERT(rc == -EINVAL);

	/* Get state success */
	rc = nvme_pcie_poll_group_get_stats(tgroup, &tgroup_stat);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	CU_ASSERT(tgroup_stat != NULL);
	CU_ASSERT(tgroup_stat->trtype == SPDK_NVME_TRANSPORT_PCIE);
	CU_ASSERT(memcmp(&tgroup_stat->pcie, &pgroup->stats, sizeof(struct spdk_nvme_pcie_stat)) == 0);

	nvme_pcie_poll_group_free_stats(tgroup, tgroup_stat);
	rc = nvme_pcie_poll_group_destroy(tgroup);
	CU_ASSERT(rc == 0);
}

static void
test_xds_nvme_pcie_profile_core(void)
{
	struct nvme_pcie_qpair pqpair = {};
	struct spdk_nvme_pcie_stat stat = {};
	struct xds_nvme_pcie_profile *profile;
	struct xds_nvme_pcie_sample *sample;
	uint32_t i;
	int rc;

	pqpair.num_entries = 128;
	pqpair.stat = &stat;

	rc = xds_nvme_pcie_profile_init(&pqpair, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT_PTR_NULL(pqpair.xds_profile);
	CU_ASSERT(pqpair.flags.xds_profile_enabled == 0);

	rc = xds_nvme_pcie_profile_init(&pqpair, "12");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(pqpair.xds_profile != NULL);
	profile = pqpair.xds_profile;
	CU_ASSERT(pqpair.flags.xds_profile_enabled == 1);
	CU_ASSERT(profile->shift == 12);
	CU_ASSERT(profile->free_count == XDS_NVME_PCIE_SAMPLE_POOL_SIZE);
	CU_ASSERT(profile->next_sample > 0);
	CU_ASSERT(profile->next_sample < 4096);
	for (i = 0; i < pqpair.num_entries; i++) {
		CU_ASSERT(profile->sample_by_cid[i] == -1);
	}

	profile->eligible = profile->next_sample;
	sample = xds_nvme_pcie_profile_try_begin(&pqpair, 7, 100);
	SPDK_CU_ASSERT_FATAL(sample != NULL);
	CU_ASSERT(sample->cid == 7);
	CU_ASSERT(sample->sq_staged_tsc == 100);
	CU_ASSERT(profile->sample_by_cid[7] >= 0);
	CU_ASSERT(profile->pending_count == 1);
	CU_ASSERT_PTR_NULL(xds_nvme_pcie_profile_try_begin(&pqpair, 8, 110));

	xds_nvme_pcie_profile_fini(&pqpair);
	CU_ASSERT_PTR_NULL(pqpair.xds_profile);
	CU_ASSERT(pqpair.flags.xds_profile_enabled == 0);
}

static void
test_xds_nvme_pcie_profile_rotates_sample_phase(void)
{
	struct nvme_pcie_qpair pqpair = {};
	struct xds_nvme_pcie_sample *sample;
	int rc;

	pqpair.num_entries = 128;
	rc = xds_nvme_pcie_profile_init(&pqpair, "5");
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* The first block starts at a randomized nonzero steady-state offset. */
	CU_ASSERT(pqpair.xds_profile->next_sample > 0);
	CU_ASSERT(pqpair.xds_profile->next_sample < 32);
	pqpair.xds_profile->eligible = pqpair.xds_profile->next_sample;
	sample = xds_nvme_pcie_profile_try_begin(&pqpair, 7, 100);
	SPDK_CU_ASSERT_FATAL(sample != NULL);
	xds_nvme_pcie_profile_release(pqpair.xds_profile, sample);

	/* The next block must choose a nonzero in-block phase. */
	CU_ASSERT(pqpair.xds_profile->next_sample > 32);
	CU_ASSERT(pqpair.xds_profile->next_sample < 64);
	pqpair.xds_profile->eligible = pqpair.xds_profile->next_sample - 1;
	CU_ASSERT_PTR_NULL(xds_nvme_pcie_profile_try_begin(&pqpair, 8, 110));
	sample = xds_nvme_pcie_profile_try_begin(&pqpair, 9, 120);
	SPDK_CU_ASSERT_FATAL(sample != NULL);
	xds_nvme_pcie_profile_release(pqpair.xds_profile, sample);

	xds_nvme_pcie_profile_fini(&pqpair);
}

static void
test_xds_nvme_pcie_profile_doorbell_and_completion(void)
{
	struct nvme_pcie_qpair pqpair = {};
	struct spdk_nvme_pcie_stat stat = {};
	struct xds_nvme_pcie_profile *profile;
	struct xds_nvme_pcie_sample *first;
	struct xds_nvme_pcie_sample *second;
	int rc;

	pqpair.num_entries = 128;
	pqpair.stat = &stat;
	pqpair.qpair.queue_depth = 32;
	rc = xds_nvme_pcie_profile_init(&pqpair, "0");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	profile = pqpair.xds_profile;

	first = xds_nvme_pcie_profile_try_begin(&pqpair, 7, 100);
	second = xds_nvme_pcie_profile_try_begin(&pqpair, 8, 110);
	SPDK_CU_ASSERT_FATAL(first != NULL);
	SPDK_CU_ASSERT_FATAL(second != NULL);
	CU_ASSERT(profile->pending_count == 2);

	xds_nvme_pcie_profile_mark_doorbell_at(&pqpair, 150);
	CU_ASSERT(first->doorbell_tsc == 150);
	CU_ASSERT(second->doorbell_tsc == 150);
	CU_ASSERT(first->pending_doorbell == false);
	CU_ASSERT(second->pending_doorbell == false);
	CU_ASSERT(profile->pending_count == 0);

	stat.polls = 12;
	stat.idle_polls = 9;
	stat.completions = 4;
	pqpair.qpair.queue_depth = 31;
	rc = xds_nvme_pcie_profile_complete_at(&pqpair, 7, false, 250);
	CU_ASSERT(rc == 0);
	CU_ASSERT(profile->completed == 1);
	CU_ASSERT(profile->sq_wait_ticks == 50);
	CU_ASSERT(profile->doorbell_to_cqe_ticks == 100);
	CU_ASSERT(profile->total_ticks == 150);
	CU_ASSERT(profile->polls_delta == 12);
	CU_ASSERT(profile->idle_polls_delta == 9);
	CU_ASSERT(profile->completions_delta == 4);
	CU_ASSERT(profile->qd_submit_sum == 32);
	CU_ASSERT(profile->qd_cqe_sum == 31);
	CU_ASSERT(profile->sample_by_cid[7] == -1);

	rc = xds_nvme_pcie_profile_complete_at(&pqpair, 8, true, 260);
	CU_ASSERT(rc == -EIO);
	CU_ASSERT(profile->error_drops == 1);
	CU_ASSERT(profile->completed == 1);
	CU_ASSERT(profile->sample_by_cid[8] == -1);

	SPDK_CU_ASSERT_FATAL(xds_nvme_pcie_profile_try_begin(&pqpair, 9, 300) != NULL);
	rc = xds_nvme_pcie_profile_complete_at(&pqpair, 9, false, 350);
	CU_ASSERT(rc == -EINVAL);
	CU_ASSERT(profile->invalid == 1);
	CU_ASSERT(profile->sample_by_cid[9] == -1);
	xds_nvme_pcie_profile_mark_doorbell_at(&pqpair, 360);
	CU_ASSERT(profile->pending_count == 0);

	xds_nvme_pcie_profile_fini(&pqpair);
}

static void
test_xds_nvme_pcie_profile_actual_doorbell_hook(void)
{
	struct nvme_pcie_ctrlr pctrlr = {};
	struct nvme_pcie_qpair pqpair = {};
	struct spdk_nvme_pcie_stat stat = {};
	struct xds_nvme_pcie_sample *sample;
	uint32_t doorbell = 0;
	int rc;

	pqpair.num_entries = 128;
	pqpair.stat = &stat;
	pqpair.sq_tdbl = &doorbell;
	pqpair.sq_tail = 9;
	pqpair.qpair.ctrlr = &pctrlr.ctrlr;
	pctrlr.doorbell_base = &doorbell;
	rc = xds_nvme_pcie_profile_init(&pqpair, "0");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	sample = xds_nvme_pcie_profile_try_begin(&pqpair, 7, 1);
	SPDK_CU_ASSERT_FATAL(sample != NULL);

	nvme_pcie_qpair_ring_sq_doorbell(&pqpair.qpair);
	CU_ASSERT(sample->pending_doorbell == false);
	CU_ASSERT(doorbell == 9);

	xds_nvme_pcie_profile_fini(&pqpair);
}

static void
test_xds_nvme_pcie_profile_format(void)
{
	struct xds_nvme_pcie_profile profile = {};
	char line[1024];
	int rc;

	profile.shift = 12;
	profile.eligible = 8192;
	profile.completed = 2;
	profile.sq_wait_ticks = 100;
	profile.doorbell_to_cqe_ticks = 300;
	profile.total_ticks = 400;
	profile.polls_delta = 20;
	profile.idle_polls_delta = 12;
	profile.completions_delta = 8;
	profile.qd_submit_sum = 64;
	profile.qd_cqe_sum = 62;

	rc = xds_nvme_pcie_profile_format(line, sizeof(line),
				     "0000:aa:00.0", 32, 1, &profile,
				     1000000);
	CU_ASSERT(rc > 0);
	CU_ASSERT_PTR_NOT_NULL(strstr(line,
		"XDS_NVME_PCIE_STAGE_PROFILE traddr=0000:aa:00.0 core=32 qid=1 shift=12"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "eligible=8192 samples=2"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "sq_wait_us=50.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "doorbell_to_cqe_us=150.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "transport_total_us=200.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "polls_per_sample=10.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "idle_polls_per_sample=6.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "completions_per_sample=4.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "qd_submit=32.000 qd_cqe=31.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line,
		"qd_time_0_pct=0.000 qd_time_1_7_pct=0.000 "
		"qd_time_8_15_pct=0.000 qd_time_16_23_pct=0.000 "
		"qd_time_24_27_pct=0.000 qd_time_28_31_pct=0.000 "
		"qd_time_32_plus_pct=0.000 low_qd_episodes=0 "
		"low_qd_max_us=0.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "qd_time_total_us=0.000"));
}

static void
test_xds_nvme_pcie_profile_qd_residency(void)
{
	struct xds_nvme_pcie_profile profile = {};

	CU_ASSERT(xds_nvme_pcie_qd_bucket(0) == 0);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(1) == 1);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(7) == 1);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(8) == 2);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(15) == 2);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(16) == 3);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(23) == 3);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(24) == 4);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(27) == 4);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(28) == 5);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(31) == 5);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(32) == 6);
	CU_ASSERT(xds_nvme_pcie_qd_bucket(UINT16_MAX) == 6);

	xds_nvme_pcie_profile_qd_transition_at(&profile, 32, 100);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 27, 110);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 24, 130);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 28, 145);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 7, 150);
	xds_nvme_pcie_profile_qd_flush_at(&profile, 180);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 0, 190);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 32, 200);

	CU_ASSERT(profile.qd_residency_ticks[0] == 10);
	CU_ASSERT(profile.qd_residency_ticks[1] == 40);
	CU_ASSERT(profile.qd_residency_ticks[2] == 0);
	CU_ASSERT(profile.qd_residency_ticks[3] == 0);
	CU_ASSERT(profile.qd_residency_ticks[4] == 35);
	CU_ASSERT(profile.qd_residency_ticks[5] == 5);
	CU_ASSERT(profile.qd_residency_ticks[6] == 10);
	CU_ASSERT(profile.low_qd_episodes == 2);
	CU_ASSERT(profile.low_qd_max_ticks == 50);
	CU_ASSERT(profile.low_qd_start_tsc == 0);
	CU_ASSERT(profile.qd_last == 32);
	CU_ASSERT(profile.qd_last_tsc == 200);
}

static void
test_xds_nvme_pcie_profile_qd_resets_after_long_idle(void)
{
	struct xds_nvme_pcie_profile profile = {
		.qd_idle_reset_ticks = 100,
	};

	xds_nvme_pcie_profile_qd_transition_at(&profile, 1, 10);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 32, 20);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 0, 30);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 1, 200);

	for (uint32_t i = 0; i < SPDK_COUNTOF(profile.qd_residency_ticks); i++) {
		CU_ASSERT(profile.qd_residency_ticks[i] == 0);
	}
	CU_ASSERT(profile.low_qd_episodes == 0);
	CU_ASSERT(profile.low_qd_max_ticks == 0);
	CU_ASSERT(profile.low_qd_start_tsc == 200);
	CU_ASSERT(profile.qd_last == 1);
	CU_ASSERT(profile.qd_last_tsc == 200);
}

static void
test_xds_nvme_pcie_profile_qd_ignores_trailing_long_idle(void)
{
	struct xds_nvme_pcie_profile profile = {
		.qd_idle_reset_ticks = 100,
	};

	xds_nvme_pcie_profile_qd_transition_at(&profile, 32, 10);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 0, 20);
	xds_nvme_pcie_profile_qd_flush_at(&profile, 200);

	CU_ASSERT(profile.qd_residency_ticks[0] == 0);
	CU_ASSERT(profile.qd_residency_ticks[6] == 10);
	CU_ASSERT(profile.low_qd_episodes == 0);
	CU_ASSERT(profile.low_qd_max_ticks == 0);
}

static void
test_xds_nvme_pcie_profile_qd_keeps_longest_activity_window(void)
{
	struct xds_nvme_pcie_profile profile = {
		.qd_idle_reset_ticks = 100,
	};

	/* Main benchmark activity window. */
	xds_nvme_pcie_profile_qd_transition_at(&profile, 32, 10);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 31, 1010);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 0, 2010);

	/* A tiny shutdown I/O window after a long idle must not replace it. */
	xds_nvme_pcie_profile_qd_transition_at(&profile, 1, 5010);
	xds_nvme_pcie_profile_qd_transition_at(&profile, 0, 5020);
	xds_nvme_pcie_profile_qd_finalize_at(&profile, 6020);

	CU_ASSERT(profile.qd_residency_ticks[0] == 0);
	CU_ASSERT(profile.qd_residency_ticks[1] == 0);
	CU_ASSERT(profile.qd_residency_ticks[5] == 1000);
	CU_ASSERT(profile.qd_residency_ticks[6] == 1000);
}

static void
test_xds_nvme_pcie_profile_latency_format(void)
{
	struct xds_nvme_pcie_profile profile = {};
	uint64_t latency_ticks[] = {10, 100, 20, 30};
	char line[1024];
	int rc;

	profile.completed = 4;
	profile.latency_ticks = latency_ticks;
	profile.latency_stored = SPDK_COUNTOF(latency_ticks);
	profile.latency_max_polls = 17;
	profile.latency_max_idle_polls = 15;
	profile.latency_max_completions = 2;
	profile.latency_max_qd_submit = 31;
	profile.latency_max_qd_cqe = 30;

	rc = xds_nvme_pcie_profile_latency_format(
		line, sizeof(line), "0000:aa:00.0", 32, 1, &profile, 1000000);
	CU_ASSERT(rc > 0);
	CU_ASSERT_PTR_NOT_NULL(strstr(line,
		"XDS_NVME_PCIE_LATENCY_PROFILE traddr=0000:aa:00.0 core=32 qid=1"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "samples=4 stored=4 truncated=0"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "doorbell_to_cqe_p50_us=20.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "doorbell_to_cqe_p95_us=100.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "doorbell_to_cqe_p99_us=100.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "doorbell_to_cqe_max_us=100.000"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line,
		"over_100us=0 over_1000us=0 over_10000us=0"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line,
		"max_polls=17 max_idle_polls=15 max_completions=2"));
	CU_ASSERT_PTR_NOT_NULL(strstr(line, "max_qd_submit=31 max_qd_cqe=30"));
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvme_pcie_common", NULL, NULL);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_alloc_cmb);
	CU_ADD_TEST(suite, test_nvme_pcie_qpair_construct_destroy);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_cmd_create_delete_io_queue);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_connect_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_ctrlr_construct_admin_qpair);
	CU_ADD_TEST(suite, test_nvme_pcie_poll_group_get_stats);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_core);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_rotates_sample_phase);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_doorbell_and_completion);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_actual_doorbell_hook);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_format);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_qd_residency);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_qd_resets_after_long_idle);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_qd_ignores_trailing_long_idle);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_qd_keeps_longest_activity_window);
	CU_ADD_TEST(suite, test_xds_nvme_pcie_profile_latency_format);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
