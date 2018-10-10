/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2014 Intel Corporation, Inc. All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2018 Cray Inc. All rights reserved.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <ofi_list.h>
#include <ofi.h>

#include "cxip.h"

#define CXIP_LOG_DBG(...) _CXIP_LOG_DBG(FI_LOG_EP_DATA, __VA_ARGS__)
#define CXIP_LOG_ERROR(...) _CXIP_LOG_ERROR(FI_LOG_EP_DATA, __VA_ARGS__)

enum cxip_rma_op {
	CXIP_RMA_READ,
	CXIP_RMA_WRITE,
};

static void cxip_rma_cb(struct cxip_req *req, const union c_event *event)
{
	int ret;
	int event_rc;

	ret = cxil_unmap(req->cq->domain->dev_if->if_lni, &req->rma.local_md);
	if (ret != FI_SUCCESS)
		CXIP_LOG_ERROR("Failed to free MD: %d\n", ret);

	event_rc = event->init_short.return_code;
	if (event_rc == C_RC_OK) {
		ret = req->cq->report_completion(req->cq, FI_ADDR_UNSPEC, req);
		if (ret != req->cq->cq_entry_size)
			CXIP_LOG_ERROR("Failed to report completion: %d\n",
				       ret);
	} else {
		ret = cxip_cq_report_error(req->cq, req, 0, FI_EIO, event_rc,
					   NULL, 0);
		if (ret != FI_SUCCESS)
			CXIP_LOG_ERROR("Failed to report error: %d\n", ret);
	}

	cxip_cq_req_free(req);
}

static ssize_t _cxip_rma_op(enum cxip_rma_op op, struct fid_ep *ep,
			   const struct iovec *iov, size_t iov_count,
			   const struct fi_rma_iov *rma, size_t rma_count,
			   fi_addr_t addr, void *desc, uint64_t data,
			   uint64_t flags, void *context)
{
	struct cxip_ep *cxi_ep;
	struct cxip_tx_ctx *txc;
	struct cxip_domain *dom;
	int ret;
	struct cxi_iova mem_desc;
	struct cxip_req *req;
	union c_cmdu cmd = {};
	struct cxip_addr caddr;
	union c_fab_addr dfa;
	uint32_t idx_ext;
	uint32_t pid_granule;
	uint32_t pid_idx;
	uint32_t map_flags = CXI_MAP_PIN | CXI_MAP_NTA;

	if (!iov || !rma)
		return -FI_EINVAL;

	/* Technically (rma_count > 0 && rma_count <= MAX), where MAX == 1. If
	 * vectors are ever supported, this entire function must be modified,
	 * since rma[0] is hardcoded throughout.
	 */
	if (rma_count != 1) {
		CXIP_LOG_DBG("rma_count = %ld, must be 1\n", rma_count);
		return -FI_ENOSYS;
	}

	if (rma[0].key >= CXIP_EP_MAX_MR_CNT) {
		CXIP_LOG_DBG("rma key = %lu, must be < %d\n", rma[0].key,
			     CXIP_EP_MAX_MR_CNT);
		return -FI_EINVAL;
	}

	/* The input FID could be a standard endpoint (containing a TX
	 * context), or a TX context itself.
	 */
	switch (ep->fid.fclass) {
	case FI_CLASS_EP:
		cxi_ep = container_of(ep, struct cxip_ep, ep);
		txc = cxi_ep->ep_obj->tx_ctx;
		break;

	case FI_CLASS_TX_CTX:
		txc = container_of(ep, struct cxip_tx_ctx, fid.ctx);
		break;

	default:
		CXIP_LOG_ERROR("Invalid EP type\n");
		return -FI_EINVAL;
	}

	dom = txc->domain;

	/* Look up target CXI address */
	ret = _cxip_av_lookup(txc->av, addr, &caddr);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("Failed to look up FI addr: %d\n", ret);
		return ret;
	}

	/* Map local buffer */
	map_flags |= (op == CXIP_RMA_READ ? CXI_MAP_WRITE : CXI_MAP_READ);
	ret = cxil_map(dom->dev_if->if_lni, iov[0].iov_base, iov[0].iov_len,
		       map_flags, &mem_desc);
	if (ret) {
		CXIP_LOG_DBG("Failed to map buffer: %d\n", ret);
		return ret;
	}

	req = cxip_cq_req_alloc(txc->comp.send_cq, 0);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate request\n");
		ret = -FI_ENOMEM;
		goto unmap_op;
	}

	/* Populate request */
	req->context = (uint64_t)context;
	req->data_len = 0;
	req->buf = 0;
	req->data = 0;
	req->tag = 0;
	req->cb = cxip_rma_cb;
	req->flags = FI_RMA | (op == CXIP_RMA_READ ? FI_READ : FI_WRITE);
	req->rma.local_md = mem_desc;

	/* Generate the destination fabric address */
	pid_granule = dom->dev_if->if_pid_granule;
	pid_idx = CXIP_ADDR_MR_IDX(pid_granule, rma[0].key);
	cxi_build_dfa(caddr.nic, caddr.pid, pid_granule, pid_idx, &dfa,
		      &idx_ext);

	/* Populate command descriptor */
	cmd.full_dma.command.opcode =
		(op == CXIP_RMA_READ ? C_CMD_GET : C_CMD_PUT);
	cmd.full_dma.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.full_dma.index_ext = idx_ext;
	cmd.full_dma.lac = mem_desc.lac;
	cmd.full_dma.event_send_disable = 1;
	cmd.full_dma.restricted = 1;
	cmd.full_dma.dfa = dfa;
	cmd.full_dma.remote_offset = rma[0].addr;
	cmd.full_dma.local_addr = CXI_VA_TO_IOVA(&mem_desc, iov[0].iov_base);
	cmd.full_dma.request_len = rma[0].len;
	cmd.full_dma.eq = txc->comp.send_cq->evtq->eqn;
	cmd.full_dma.user_ptr = (uint64_t)req;

	/* Issue command */

	fastlock_acquire(&txc->lock);

	ret = cxi_cq_emit_dma(txc->tx_cmdq, &cmd.full_dma);
	if (ret) {
		CXIP_LOG_DBG("Failed to write DMA command: %d\n", ret);

		/* Return error according to Domain Resource Management */
		ret = -FI_EAGAIN;
		goto unlock_op;
	}

	cxi_cq_ring(txc->tx_cmdq);

	/* TODO take reference on EP or context for the outstanding request */
	fastlock_release(&txc->lock);

	return FI_SUCCESS;

unlock_op:
	fastlock_release(&txc->lock);
	cxip_cq_req_free(req);
unmap_op:
	cxil_unmap(dom->dev_if->if_lni, &mem_desc);

	return ret;
}

static ssize_t cxip_rma_write(struct fid_ep *ep, const void *buf, size_t len,
			      void *desc, fi_addr_t dest_addr, uint64_t addr,
			      uint64_t key, void *context)
{
	struct fi_rma_iov rma = {
		.addr = addr,
		.key = key,
		.len = len,
	};
	struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = len,
	};

	return _cxip_rma_op(CXIP_RMA_WRITE, ep, &iov, 1, &rma, 1, dest_addr,
			    desc, 0, 0, context);
}

static ssize_t cxip_rma_writev(struct fid_ep *ep, const struct iovec *iov,
			       void **desc, size_t count, fi_addr_t dest_addr,
			       uint64_t addr, uint64_t key, void *context)
{
	void *write_desc = NULL;
	struct fi_rma_iov rma = {
		.addr = addr,
		.key = key,
	};

	if (count > CXIP_RMA_MAX_IOV)
		return -FI_EINVAL;

	if (desc)
		write_desc = desc[0];

	for (size_t i = 0; i < count; i++)
		rma.len += iov[i].iov_len;

	return _cxip_rma_op(CXIP_RMA_WRITE, ep, iov, count, &rma, 1, dest_addr,
			    write_desc, 0, 0, context);
}

#define CXIP_WRITEMSG_ALLOWED_FLAGS ( \
	FI_REMOTE_CQ_DATA | FI_COMPLETION | FI_MORE | FI_INJECT_COMPLETE | \
	FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE | FI_COMMIT_COMPLETE | \
	FI_FENCE)
static ssize_t cxip_rma_writemsg(struct fid_ep *ep,
				 const struct fi_msg_rma *msg, uint64_t flags)
{
	void *write_desc;

	if (!msg || msg->iov_count > CXIP_RMA_MAX_IOV)
		return -FI_EINVAL;

	/* Check for unsupported flags */
	if (flags & ~CXIP_WRITEMSG_ALLOWED_FLAGS)
		return -FI_EBADFLAGS;

	/* Check for unimplemented flags */
	if (flags & CXIP_WRITEMSG_ALLOWED_FLAGS)
		return -FI_EINVAL;

	write_desc = (msg->desc ? msg->desc[0] : NULL);

	return _cxip_rma_op(CXIP_RMA_WRITE, ep, msg->msg_iov, msg->iov_count,
			    msg->rma_iov, msg->rma_iov_count, msg->addr,
			    write_desc, msg->data, flags, msg->context);
}

static ssize_t cxip_rma_read(struct fid_ep *ep, void *buf, size_t len,
			     void *desc, fi_addr_t src_addr, uint64_t addr,
			     uint64_t key, void *context)
{
	struct fi_rma_iov rma = {
		.addr = addr,
		.key = key,
		.len = len,
	};
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = len,
	};

	return _cxip_rma_op(CXIP_RMA_READ, ep, &iov, 1, &rma, 1, src_addr, desc,
			    0, 0, context);
}

static ssize_t cxip_rma_readv(struct fid_ep *ep, const struct iovec *iov,
			      void **desc, size_t count, fi_addr_t src_addr,
			      uint64_t addr, uint64_t key, void *context)
{
	void *read_desc = NULL;
	struct fi_rma_iov rma = {
		.addr = addr,
		.key = key,
	};

	if (count > CXIP_RMA_MAX_IOV)
		return -FI_EINVAL;

	if (desc)
		read_desc = desc[0];

	for (size_t i = 0; i < count; i++)
		rma.len += iov[i].iov_len;

	return _cxip_rma_op(CXIP_RMA_READ, ep, iov, count, &rma, 1, src_addr,
			    read_desc, 0, 0, context);
}


#define CXIP_READMSG_ALLOWED_FLAGS (FI_COMPLETION | FI_MORE)
static ssize_t cxip_rma_readmsg(struct fid_ep *ep, const struct fi_msg_rma *msg,
				uint64_t flags)
{
	void *read_desc;

	if (!msg || msg->iov_count > CXIP_RMA_MAX_IOV)
		return -FI_EINVAL;

	/* Check for unsupported flags */
	if (flags & ~CXIP_READMSG_ALLOWED_FLAGS)
		return -FI_EBADFLAGS;

	/* Check for unimplemented flags */
	if (flags & CXIP_READMSG_ALLOWED_FLAGS)
		return -FI_EINVAL;

	read_desc = (msg->desc ? msg->desc[0] : NULL);

	return _cxip_rma_op(CXIP_RMA_READ, ep, msg->msg_iov, msg->iov_count,
			    msg->rma_iov, msg->rma_iov_count, msg->addr,
			    read_desc, msg->data, flags, msg->context);
}

struct fi_ops_rma cxip_ep_rma = {
	.size = sizeof(struct fi_ops_rma),
	.read = cxip_rma_read,
	.readv = cxip_rma_readv,
	.readmsg = cxip_rma_readmsg,
	.write = cxip_rma_write,
	.writev = cxip_rma_writev,
	.writemsg = cxip_rma_writemsg,
	.inject = fi_no_rma_inject,
	.injectdata = fi_no_rma_injectdata,
	.writedata = fi_no_rma_writedata,
};
