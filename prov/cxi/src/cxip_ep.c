/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2013-2014 Intel Corporation. All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2017 DataDirect Networks, Inc. All rights reserved.
 * Copyright (c) 2018 Cray Inc. All rights reserved.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "ofi_util.h"
#include "cxip.h"

#define CXIP_LOG_DBG(...) _CXIP_LOG_DBG(FI_LOG_EP_CTRL, __VA_ARGS__)
#define CXIP_LOG_ERROR(...) _CXIP_LOG_ERROR(FI_LOG_EP_CTRL, __VA_ARGS__)

extern struct fi_ops_rma cxip_ep_rma;
extern struct fi_ops_msg cxip_ep_msg_ops;
extern struct fi_ops_tagged cxip_ep_tagged_ops;
extern struct fi_ops_atomic cxip_ep_atomic;

extern struct fi_ops_cm cxip_ep_cm_ops;
extern struct fi_ops_ep cxip_ep_ops;
extern struct fi_ops cxip_ep_fi_ops;
extern struct fi_ops_ep cxip_ctx_ep_ops;

/*
 * cxip_rdzv_id_alloc() - Allocate a rendezvous ID.
 */
int cxip_rdzv_id_alloc(struct cxip_ep_obj *ep_obj)
{
	unsigned int block;
	int bit;
	int id;

	fastlock_acquire(&ep_obj->rdzv_id_lock);

	/* TODO Start search at different cachelines based on TXC ID. */
	for (block = 0; block < CXIP_RDZV_ID_BLOCKS; block++) {
		if (ep_obj->rdzv_ids[block]) {
			bit = ffsl(ep_obj->rdzv_ids[block]);
			ep_obj->rdzv_ids[block] &= ~(1ULL << --bit);
			fastlock_release(&ep_obj->rdzv_id_lock);

			id = block * __BITS_PER_LONG + bit;
			CXIP_LOG_DBG("Allocated ID: %d\n", id);
			return id;
		}
	}

	fastlock_release(&ep_obj->rdzv_id_lock);

	return -FI_ENOSPC;
}

/*
 * cxip_rdzv_id_free() - Free a rendezvous ID.
 */
int cxip_rdzv_id_free(struct cxip_ep_obj *ep_obj, int id)
{
	unsigned int block = id / __BITS_PER_LONG;
	int bit = id % __BITS_PER_LONG;

	if (id < 0 || id >= CXIP_RDZV_IDS)
		return -FI_EINVAL;

	fastlock_acquire(&ep_obj->rdzv_id_lock);
	ep_obj->rdzv_ids[block] |= (1ULL << bit);
	fastlock_release(&ep_obj->rdzv_id_lock);

	CXIP_LOG_DBG("Freed ID: %d\n", id);

	return FI_SUCCESS;
}

static int cxip_ep_cm_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct cxip_ep *cxip_ep;
	size_t len;

	len = MIN(*addrlen, sizeof(struct cxip_addr));

	switch (fid->fclass) {
	case FI_CLASS_EP:
	case FI_CLASS_SEP:
		cxip_ep = container_of(fid, struct cxip_ep, ep.fid);
		if (!cxip_ep->ep_obj->enabled)
			return -FI_EOPBADSTATE;

		CXIP_LOG_DBG("NIC: 0x%x PID: %u\n",
			     cxip_ep->ep_obj->src_addr.nic,
			     cxip_ep->ep_obj->src_addr.pid);

		memcpy(addr, &cxip_ep->ep_obj->src_addr, len);
		break;
	default:
		CXIP_LOG_ERROR("Invalid argument\n");
		return -FI_EINVAL;
	}

	*addrlen = sizeof(struct cxip_addr);
	return (len == sizeof(struct cxip_addr)) ? FI_SUCCESS : -FI_ETOOSMALL;
}

struct fi_ops_cm cxip_ep_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = fi_no_setname,
	.getname = cxip_ep_cm_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
	.join = fi_no_join,
};

/**
 * Close TX context.
 *
 * Support TX/RX context fi_close().
 *
 * @param txc : TX context to close
 */
static void cxip_txc_close(struct cxip_txc *txc)
{
	if (txc->send_cq)
		ofi_atomic_dec32(&txc->send_cq->ref);

	if (txc->send_cntr)
		ofi_atomic_dec32(&txc->send_cntr->ref);

	if (txc->read_cntr)
		ofi_atomic_dec32(&txc->read_cntr->ref);

	if (txc->write_cntr)
		ofi_atomic_dec32(&txc->write_cntr->ref);
}

/**
 * Close RX context.
 *
 * Support TX/RX context fi_close().
 *
 * @param rxc : RX context to close
 */
static void cxip_rxc_close(struct cxip_rxc *rxc)
{
	if (rxc->recv_cq)
		ofi_atomic_dec32(&rxc->recv_cq->ref);

	if (rxc->recv_cntr)
		ofi_atomic_dec32(&rxc->recv_cntr->ref);
}

/**
 * Close EP TX/RX context.
 *
 * Provider fi_close() implementation for TX/RX contexts.
 *
 * This can be used on Scalable EP or Shared contexts.
 *
 * @param fid : TX/RX context fid
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_close(struct fid *fid)
{
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;

	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		txc = container_of(fid, struct cxip_txc, fid.ctx.fid);
		ofi_atomic_dec32(&txc->ep_obj->num_txc);
		ofi_atomic_dec32(&txc->domain->ref);
		cxip_txc_close(txc);
		cxip_txc_free(txc);
		break;

	case FI_CLASS_RX_CTX:
		rxc = container_of(fid, struct cxip_rxc, ctx.fid);
		ofi_atomic_dec32(&rxc->ep_obj->num_rxc);
		ofi_atomic_dec32(&rxc->domain->ref);
		cxip_rxc_close(rxc);
		cxip_rxc_free(rxc);
		break;

	case FI_CLASS_STX_CTX:
		txc = container_of(fid, struct cxip_txc, fid.stx.fid);
		ofi_atomic_dec32(&txc->domain->ref);
		cxip_txc_free(txc);
		break;

	case FI_CLASS_SRX_CTX:
		rxc = container_of(fid, struct cxip_rxc, ctx.fid);
		ofi_atomic_dec32(&rxc->domain->ref);
		cxip_rxc_free(rxc);
		break;

	default:
		CXIP_LOG_ERROR("Invalid fid\n");
		return -FI_EINVAL;
	}

	return 0;
}

/**
 * Bind TX/RX context to CQ.
 *
 * Support TX/RX context bind().
 *
 * @param fid : TX/RX context fid
 * @param bfid : CQ fid
 * @param flags : options
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_bind_cq(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct cxip_cq *cxi_cq;
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;

	if ((flags | CXIP_EP_CQ_FLAGS) != CXIP_EP_CQ_FLAGS) {
		CXIP_LOG_ERROR("Invalid cq flag\n");
		return -FI_EINVAL;
	}
	cxi_cq = container_of(bfid, struct cxip_cq, util_cq.cq_fid.fid);
	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		txc = container_of(fid, struct cxip_txc, fid.ctx);
		if (flags & FI_SEND) {
			txc->send_cq = cxi_cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				txc->selective_completion = 1;
		}

		ofi_atomic_inc32(&cxi_cq->ref);
		break;

	case FI_CLASS_RX_CTX:
		rxc = container_of(fid, struct cxip_rxc, ctx.fid);
		if (flags & FI_RECV) {
			rxc->recv_cq = cxi_cq;
			if (flags & FI_SELECTIVE_COMPLETION)
				rxc->selective_completion = 1;
		}

		ofi_atomic_inc32(&cxi_cq->ref);
		break;

	default:
		CXIP_LOG_ERROR("Invalid fid\n");
		return -FI_EINVAL;
	}

	return 0;
}

/**
 * Bind a TX/RX context to a CNTR.
 *
 * Support TX/RX context bind().
 *
 * @param fid : TX/RX context fid
 * @param bfid : CNTR fid
 * @param flags : options
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_bind_cntr(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct cxip_cntr *cntr;
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;

	if ((flags | CXIP_EP_CNTR_FLAGS) != CXIP_EP_CNTR_FLAGS) {
		CXIP_LOG_ERROR("Invalid cntr flag\n");
		return -FI_EINVAL;
	}

	cntr = container_of(bfid, struct cxip_cntr, cntr_fid.fid);
	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		txc = container_of(fid, struct cxip_txc, fid.ctx.fid);
		if (flags & FI_SEND) {
			txc->send_cntr = cntr;
			ofi_atomic_inc32(&cntr->ref);
		}

		if (flags & FI_READ) {
			txc->read_cntr = cntr;
			ofi_atomic_inc32(&cntr->ref);
		}

		if (flags & FI_WRITE) {
			txc->write_cntr = cntr;
			ofi_atomic_inc32(&cntr->ref);
		}
		break;

	case FI_CLASS_RX_CTX:
		rxc = container_of(fid, struct cxip_rxc, ctx.fid);
		if (flags & FI_RECV) {
			rxc->recv_cntr = cntr;
			ofi_atomic_inc32(&cntr->ref);
		}
		break;

	default:
		CXIP_LOG_ERROR("Invalid fid\n");
		return -FI_EINVAL;
	}

	return 0;
}

/**
 * Bind TX/RX context to CQ or CNTR.
 *
 * Provider bind() implementation for TX/RX contexts.
 *
 * This can be used only on Scalable EP contexts.
 *
 * @param fid : TX/RX context fid
 * @param bfid : CQ or CNTR fid
 * @param flags : options
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	switch (bfid->fclass) {
	case FI_CLASS_CQ:
		return cxip_ctx_bind_cq(fid, bfid, flags);

	case FI_CLASS_CNTR:
		return cxip_ctx_bind_cntr(fid, bfid, flags);

	case FI_CLASS_MR:
		return 0;

	default:
		CXIP_LOG_ERROR("Invalid bind()\n");
		return -FI_EINVAL;
	}
}

/**
 * Utility routine to enable EP or SEP.
 *
 * This is called when enabling any SEP TX/RX context, or when enabling an
 * standard EP. It checks for CQs and AV, then increments the reference
 * count on the CXIL interface and domain (allocating it if necessary).
 *
 * Note that a standard EP will increment the reference counts by one for each
 * EP. A scalable EP will increment the reference counts by one for each TX and
 * each RX, but not for the SEP itself.
 *
 * @param ep_obj : EP or SEP object
 *
 * @return int 0 on success, -errno on failure
 */
static int ep_enable(struct cxip_ep_obj *ep_obj)
{
	int ret = FI_SUCCESS;

	fastlock_acquire(&ep_obj->lock);

	if (ep_obj->enabled)
		goto unlock;

	if (!ep_obj->av) {
		CXIP_LOG_DBG("enable EP without AV\n");
		ret = -FI_ENOAV;
		goto unlock;
	}

	/* Assign resources to the libfabric domain. */
	ret = cxip_domain_enable(ep_obj->domain);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("cxip_domain_enable returned: %d\n", ret);
		goto unlock;
	}

	/* Get reference to a CXI domain. */
	ret = cxip_get_if_domain(ep_obj->domain->dev_if,
				 ep_obj->vni,
				 ep_obj->src_addr.pid,
				 &ep_obj->if_dom);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("Failed to get IF Domain: %d\n", ret);
		goto unlock;
	}

	/* Store PID in case it was automatically assigned. */
	CXIP_LOG_DBG("EP assigned PID: %u\n", ep_obj->if_dom->pid);
	ep_obj->src_addr.pid = ep_obj->if_dom->pid;

	if (getenv("RDZV_OFFLOAD")) {
		ep_obj->rdzv_offload = 1;
		fprintf(stderr, "Rendezvous offload enabled\n");
	}

	ep_obj->enabled = true;

	fastlock_release(&ep_obj->lock);

	return FI_SUCCESS;

unlock:
	fastlock_release(&ep_obj->lock);

	return ret;
}

/**
 * Enable a TX/RX context.
 *
 * Support TX/RX control(FI_ENABLE).
 *
 * @param ep
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_enable(struct fid_ep *ep)
{
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;
	int ret;

	switch (ep->fid.fclass) {
	case FI_CLASS_RX_CTX:
		rxc = container_of(ep, struct cxip_rxc, ctx.fid);

		ret = ep_enable(rxc->ep_obj);
		if (ret != FI_SUCCESS)
			return ret;

		ret = cxip_rxc_enable(rxc);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_DBG("cxip_rxc_enable returned: %d\n",
				     ret);
			return ret;
		}
		return 0;

	case FI_CLASS_TX_CTX:
		txc = container_of(ep, struct cxip_txc, fid.ctx.fid);

		ret = ep_enable(txc->ep_obj);
		if (ret != FI_SUCCESS)
			return ret;

		ret = cxip_txc_enable(txc);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_DBG("cxip_txc_enable returned: %d\n",
				     ret);
			return ret;
		}
		return 0;

	default:
		CXIP_LOG_ERROR("Invalid CTX\n");
		break;
	}

	return -FI_EINVAL;
}

/**
 * Get TX/RX option flags.
 *
 * Support TX/RX context control(FI_GETOPSFLAG).
 *
 * @param tx_attr : TX attributes, or NULL
 * @param rx_attr : RX attributes, or NULL
 * @param flags : storage for returned flags
 *
 * @return int : 0 on success, -errno on failure
 */
int cxip_getopflags(struct fi_tx_attr *tx_attr, struct fi_rx_attr *rx_attr,
		    uint64_t *flags)
{
	if ((*flags & FI_TRANSMIT) && (*flags & FI_RECV)) {
		CXIP_LOG_ERROR("Both Tx/Rx flags cannot be specified\n");
		return -FI_EINVAL;
	} else if (tx_attr && (*flags & FI_TRANSMIT)) {
		*flags = tx_attr->op_flags;
	} else if (rx_attr && (*flags & FI_RECV)) {
		*flags = rx_attr->op_flags;
	} else {
		CXIP_LOG_ERROR("Tx/Rx flags not specified\n");
		return -FI_EINVAL;
	}

	return 0;
}

/**
 * Set TX/RX option flags.
 *
 * Support TX/RX control(FI_SETOPSFLAG).
 *
 * @param tx_attr : TX attributes, or NULL
 * @param rx_attr : RX attributes, or NULL
 * @param flags : flags to set
 *
 * @return int : 0 on success, -errno on failure
 */
int cxip_setopflags(struct fi_tx_attr *tx_attr, struct fi_rx_attr *rx_attr,
		    uint64_t flags)
{
	if ((flags & FI_TRANSMIT) && (flags & FI_RECV)) {
		CXIP_LOG_ERROR("Both Tx/Rx flags cannot be specified\n");
		return -FI_EINVAL;
	} else if (tx_attr && (flags & FI_TRANSMIT)) {
		tx_attr->op_flags = flags;
		tx_attr->op_flags &= ~FI_TRANSMIT;
		if (!(flags & (FI_INJECT_COMPLETE | FI_TRANSMIT_COMPLETE |
			       FI_DELIVERY_COMPLETE)))
			tx_attr->op_flags |= FI_TRANSMIT_COMPLETE;
	} else if (rx_attr && (flags & FI_RECV)) {
		rx_attr->op_flags = flags;
		rx_attr->op_flags &= ~FI_RECV;
	} else {
		CXIP_LOG_ERROR("Tx/Rx flags not specified\n");
		return -FI_EINVAL;
	}

	return 0;
}

/**
 * Control TX/RX context.
 *
 * Provider control() implementation for TX/RX contexts.
 *
 * @param fid : TX/RX context fid
 * @param command : control operation code
 * @param arg : optional argument
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_control(struct fid *fid, int command, void *arg)
{
	struct fid_ep *ep;
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;
	int ret;

	switch (fid->fclass) {
	case FI_CLASS_TX_CTX:
		switch (command) {
		case FI_GETOPSFLAG:
			txc = container_of(fid, struct cxip_txc,
					      fid.ctx.fid);
			ret = cxip_getopflags(&txc->attr, NULL,
					      (uint64_t *)arg);
			if (ret)
				return -FI_EINVAL;
			break;
		case FI_SETOPSFLAG:
			txc = container_of(fid, struct cxip_txc,
					      fid.ctx.fid);
			ret = cxip_setopflags(&txc->attr, NULL,
					      *(uint64_t *)arg);
			if (ret)
				return -FI_EINVAL;
			break;
		case FI_ENABLE:
			ep = container_of(fid, struct fid_ep, fid);
			return cxip_ctx_enable(ep);
		default:
			return -FI_ENOSYS;
		}
		break;

	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		switch (command) {
		case FI_GETOPSFLAG:
			rxc = container_of(fid, struct cxip_rxc, ctx.fid);
			ret = cxip_getopflags(NULL, &rxc->attr,
					      (uint64_t *)arg);
			if (ret)
				return -FI_EINVAL;
			break;
		case FI_SETOPSFLAG:
			rxc = container_of(fid, struct cxip_rxc, ctx.fid);
			ret = cxip_setopflags(NULL, &rxc->attr,
					      *(uint64_t *)arg);
			if (ret)
				return -FI_EINVAL;
			break;
		case FI_ENABLE:
			ep = container_of(fid, struct fid_ep, fid);
			return cxip_ctx_enable(ep);
		default:
			return -FI_ENOSYS;
		}
		break;

	default:
		return -FI_ENOSYS;
	}

	return 0;
}

static struct fi_ops cxip_ctx_ops = {
	.size = sizeof(struct fi_ops),
	.close = cxip_ctx_close,
	.bind = cxip_ctx_bind,
	.control = cxip_ctx_control,
	.ops_open = fi_no_ops_open,
};

/*=======================================*/

/**
 * Get TX/RX context options
 *
 * Provider getopt() implementation for TX/RX contexts
 *
 * @param fid : TX/RX context fid
 * @param level : option level (must be FI_OPT_ENDPOINT)
 * @param optname : option name
 * @param optval : returned option
 * @param optlen : returned length
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_getopt(fid_t fid, int level, int optname, void *optval,
			   size_t *optlen)
{
	struct cxip_rxc *rxc;

	rxc = container_of(fid, struct cxip_rxc, ctx.fid);

	if (level != FI_OPT_ENDPOINT)
		return -FI_ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		if (*optlen < sizeof(size_t))
			return -FI_ETOOSMALL;
		*(size_t *)optval = rxc->min_multi_recv;
		*optlen = sizeof(size_t);
		break;
	default:
		return -FI_ENOPROTOOPT;
	}

	return 0;
}

/**
 * Set TX/RX options
 *
 * Provider setopt() implementation for TX/RX contexts
 *
 * @param fid : TX/RX context fid
 * @param level : option level (must be FI_OPT_ENDPOINT)
 * @param optname : option name
 * @param optval : option value to set
 * @param optlen : option length
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ctx_setopt(fid_t fid, int level, int optname,
			   const void *optval, size_t optlen)
{
	struct cxip_rxc *rxc;

	rxc = container_of(fid, struct cxip_rxc, ctx.fid);

	if (level != FI_OPT_ENDPOINT)
		return -FI_ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:
		/* TODO: this has a maximum value based on min_free_shift, which
		 * is a configurable cxi-driver kernel parameter. Currently
		 * incomplete implementation, limit when complete.
		 */
		rxc->min_multi_recv = *(size_t *)optval;
		break;
	default:
		return -FI_ENOPROTOOPT;
	}

	return 0;
}

/**
 * Cancel RX operation
 *
 * Support TX/RX context cancel().
 *
 * Searches the RX queue for a pending async operation with the specified
 * 'context', and cancels it if still pending.
 *
 * @param rxc : RX context to search
 * @param context : user context pointer to search for
 *
 * @return ssize_t : 0 on success, -errno on failure
 */
static ssize_t cxip_rxc_cancel(struct cxip_rxc *rxc, void *context)
{
	if (!rxc->enabled)
		return -FI_EOPBADSTATE;

	return cxip_cq_req_cancel(rxc->recv_cq, rxc, context, true);
}

/**
 * Cancel TX/RX operation
 *
 * Provider cancel() implementation for TX/RX contexts
 *
 * @param fid : EP, TX ctx, or RX ctx fid
 * @param context : user-specified context
 *
 * @return ssize_t : 0 on success, -errno on failure
 */
static ssize_t cxip_ep_cancel(fid_t fid, void *context)
{
	struct cxip_rxc *rxc = NULL;
	struct cxip_ep *cxi_ep;

	switch (fid->fclass) {
	case FI_CLASS_EP:
		cxi_ep = container_of(fid, struct cxip_ep, ep.fid);
		rxc = cxi_ep->ep_obj->rxcs[0];
		break;

	case FI_CLASS_RX_CTX:
	case FI_CLASS_SRX_CTX:
		rxc = container_of(fid, struct cxip_rxc, ctx.fid);
		break;

	case FI_CLASS_TX_CTX:
	case FI_CLASS_STX_CTX:
		return -FI_ENOENT;

	default:
		CXIP_LOG_ERROR("Invalid ep type\n");
		return -FI_EINVAL;
	}

	return cxip_rxc_cancel(rxc, context);
}

struct fi_ops_ep cxip_ctx_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = cxip_ep_cancel,
	.getopt = cxip_ctx_getopt,
	.setopt = cxip_ctx_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

/*=======================================*/

/**
 * Enable the EP.
 *
 * Support EP fi_control(FI_ENABLE).
 *
 * Enabling a Standard EP will also enable the TX and RX contexts.
 *
 * Enabling a Scalable EP will only enable the EP. It is expected that the TX/RX
 * contexts will be explicitly enabled using fi_control(FI_ENABLE) on each
 * context, after enabling the SEP.
 *
 * @param fid : EP/SEP fid
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_enable(struct fid_ep *ep)
{
	int ret;
	struct cxip_ep *cxi_ep;
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;

	cxi_ep = container_of(ep, struct cxip_ep, ep);
	txc = cxi_ep->ep_obj->txcs[0];
	rxc = cxi_ep->ep_obj->rxcs[0];

	if (cxi_ep->ep_obj->fclass == FI_CLASS_EP) {
		/* Check for shared TX/RX not bound */
		if (!txc) {
			CXIP_LOG_DBG("enable EP with no TXC\n");
			return -FI_EINVAL;
		}
		if (!rxc) {
			CXIP_LOG_DBG("enable EP with no RXC\n");
			return -FI_EINVAL;
		}

		ret = ep_enable(cxi_ep->ep_obj);
		if (ret != FI_SUCCESS)
			return ret;

		ret = cxip_txc_enable(txc);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_DBG("cxip_txc_enable returned: %d\n",
				     ret);
			return ret;
		}

		ret = cxip_rxc_enable(rxc);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_DBG("cxip_rxc_enable returned: %d\n", ret);
			return ret;
		}
	} else {
		ret = ep_enable(cxi_ep->ep_obj);
		if (ret != FI_SUCCESS)
			return ret;
	}

	return FI_SUCCESS;
}

/**
 * Disable the EP/SEP if not disabled already.
 *
 * This simply drops a reference on the if_domain.
 *
 * @param cxi_ep : EP/SEP
 */
static void cxip_ep_disable(struct cxip_ep *cxi_ep)
{
	if (cxi_ep->ep_obj->enabled) {
		cxip_put_if_domain(cxi_ep->ep_obj->if_dom);
		cxi_ep->ep_obj->enabled = false;
	}
}

/**
 * Close (destroy) the EP.
 *
 * Provider fi_close() implementation for EP.
 *
 * @param fid : EP fid
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_close(struct fid *fid)
{
	struct cxip_ep *cxi_ep;

	switch (fid->fclass) {
	case FI_CLASS_EP:
	case FI_CLASS_SEP:
		cxi_ep = container_of(fid, struct cxip_ep, ep.fid);
		break;

	default:
		return -FI_EINVAL;
	}

	if (cxi_ep->is_alias) {
		ofi_atomic_dec32(&cxi_ep->ep_obj->ref);
		return 0;
	}

	/* If FI_CLASS_SEP, num_*_ctx > 0 until all CTX removed.
	 * Each MR bound increments ref, so MRs must be removed.
	 */
	if (ofi_atomic_get32(&cxi_ep->ep_obj->ref) ||
	    ofi_atomic_get32(&cxi_ep->ep_obj->num_rxc) ||
	    ofi_atomic_get32(&cxi_ep->ep_obj->num_txc))
		return -FI_EBUSY;

	if (cxi_ep->ep_obj->av) {
		ofi_atomic_dec32(&cxi_ep->ep_obj->av->ref);

		fastlock_acquire(&cxi_ep->ep_obj->av->list_lock);
		fid_list_remove(&cxi_ep->ep_obj->av->ep_list,
				&cxi_ep->ep_obj->lock,
				&cxi_ep->ep.fid);
		fastlock_release(&cxi_ep->ep_obj->av->list_lock);
	}

	if (cxi_ep->ep_obj->fclass == FI_CLASS_EP) {
		if (!cxi_ep->ep_obj->tx_shared) {
			cxip_txc_close(cxi_ep->ep_obj->txcs[0]);
			cxip_txc_free(cxi_ep->ep_obj->txcs[0]);
		}
		if (!cxi_ep->ep_obj->rx_shared) {
			cxip_rxc_close(cxi_ep->ep_obj->rxcs[0]);
			cxip_rxc_free(cxi_ep->ep_obj->rxcs[0]);
		}
	}

	cxip_ep_disable(cxi_ep);

	free(cxi_ep->ep_obj->txcs);
	free(cxi_ep->ep_obj->rxcs);

	ofi_atomic_dec32(&cxi_ep->ep_obj->domain->ref);
	fastlock_destroy(&cxi_ep->ep_obj->lock);
	free(cxi_ep->ep_obj);
	free(cxi_ep);

	return 0;
}

/**
 * Bind an EP.
 *
 * Provider fi_bind() implementation for EP.
 *
 * For Scalable EP binding a CQ or CNTR, all of the TX/RX contexts will be bound
 * to the specified CQ or CNTR.
 *
 * @param fid : EP fid
 * @param bfid : fid of object to bind EP to
 * @param flags : options
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	int ret;
	size_t i;
	struct cxip_ep *ep;
	struct cxip_eq *eq;
	struct cxip_cq *cq;
	struct cxip_av *av;
	struct cxip_cntr *cntr;
	struct cxip_txc *txc;
	struct cxip_rxc *rxc;

	ret = ofi_ep_bind_valid(&cxip_prov, bfid, flags);
	if (ret)
		return ret;

	switch (fid->fclass) {
	case FI_CLASS_EP:	/* Standard EP */
	case FI_CLASS_SEP:	/* Scalable EP */
		ep = container_of(fid, struct cxip_ep, ep.fid);
		break;

	default:
		return -FI_EINVAL;
	}

	switch (bfid->fclass) {
	case FI_CLASS_EQ:
		eq = container_of(bfid, struct cxip_eq, eq.fid);
		ep->ep_obj->eq = eq;
		break;

	case FI_CLASS_CQ:
		cq = container_of(bfid, struct cxip_cq, util_cq.cq_fid.fid);
		if (ep->ep_obj->domain != cq->domain)
			return -FI_EINVAL;

		// TODO: prevent re-binding
		if (flags & FI_SEND) {
			for (i = 0; i < ep->ep_obj->ep_attr.tx_ctx_cnt; i++) {
				txc = ep->ep_obj->txcs[i];

				if (!txc)
					continue;

				ret = cxip_ctx_bind_cq(&txc->fid.ctx.fid,
						       bfid, flags);
				if (ret)
					return ret;
			}
		}

		// TODO: prevent re-binding
		if (flags & FI_RECV) {
			for (i = 0; i < ep->ep_obj->ep_attr.rx_ctx_cnt; i++) {
				rxc = ep->ep_obj->rxcs[i];

				if (!rxc)
					continue;

				ret = cxip_ctx_bind_cq(&rxc->ctx.fid, bfid,
						       flags);
				if (ret)
					return ret;
			}
		}
		break;

	case FI_CLASS_CNTR:
		cntr = container_of(bfid, struct cxip_cntr, cntr_fid.fid);
		if (ep->ep_obj->domain != cntr->domain)
			return -FI_EINVAL;

		// TODO: prevent re-binding
		if (flags & FI_SEND || flags & FI_WRITE || flags & FI_READ) {
			for (i = 0; i < ep->ep_obj->ep_attr.tx_ctx_cnt; i++) {
				txc = ep->ep_obj->txcs[i];

				if (!txc)
					continue;

				ret = cxip_ctx_bind_cntr(&txc->fid.ctx.fid,
							 bfid, flags);
				if (ret)
					return ret;
			}
		}

		// TODO: prevent re-binding
		if (flags & FI_RECV || flags & FI_REMOTE_READ ||
		    flags & FI_REMOTE_WRITE) {
			for (i = 0; i < ep->ep_obj->ep_attr.rx_ctx_cnt; i++) {
				rxc = ep->ep_obj->rxcs[i];

				if (!rxc)
					continue;

				ret = cxip_ctx_bind_cntr(&rxc->ctx.fid, bfid,
							 flags);
				if (ret)
					return ret;
			}
		}
		break;

	case FI_CLASS_AV:
		av = container_of(bfid, struct cxip_av, av_fid.fid);
		if (ep->ep_obj->domain != av->domain)
			return -FI_EINVAL;

		ep->ep_obj->av = av;
		ofi_atomic_inc32(&av->ref);

		fastlock_acquire(&av->list_lock);
		ret = fid_list_insert(&av->ep_list, &ep->ep_obj->lock,
				      &ep->ep.fid);
		fastlock_release(&av->list_lock);
		if (ret) {
			CXIP_LOG_ERROR("Error in adding fid in the EP list\n");
			return ret;
		}
		break;

	case FI_CLASS_STX_CTX:	/* shared TX context */
		// TODO: should verify EP attr against CTX attr
		// TODO: must not be SEP
		txc = container_of(bfid, struct cxip_txc, fid.stx.fid);

		ep->ep_obj->txcs[0]->use_shared = 1;
		ep->ep_obj->txcs[0]->stx = txc;
		break;

	case FI_CLASS_SRX_CTX:	/* shared RX context */
		// TODO: should verify EP attr against CTX attr
		// TODO: must not be SEP
		rxc = container_of(bfid, struct cxip_rxc, ctx);

		ep->ep_obj->rxcs[0]->use_shared = 1;
		ep->ep_obj->rxcs[0]->srx = rxc;
		break;

	default:
		return -FI_EINVAL;
	}

	return 0;
}

/**
 * Control EP.
 *
 * Provider control() implementation for EP
 *
 * @param fid : EP fid
 * @param command : control operation
 * @param arg : control argument
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_control(struct fid *fid, int command, void *arg)
{
	int ret;
	struct fid_ep *ep_fid;
	struct fi_alias *alias;
	struct cxip_ep *cxi_ep, *new_ep;

	switch (fid->fclass) {
	case FI_CLASS_EP:
	case FI_CLASS_SEP:
		cxi_ep = container_of(fid, struct cxip_ep, ep.fid);
		break;

	default:
		return -FI_EINVAL;
	}

	switch (command) {
	case FI_ALIAS:
		if (!arg)
			return -FI_EINVAL;
		alias = (struct fi_alias *)arg;
		if (!alias->fid)
			return -FI_EINVAL;
		new_ep = calloc(1, sizeof(*new_ep));
		if (!new_ep)
			return -FI_ENOMEM;

		memcpy(&new_ep->tx_attr, &cxi_ep->tx_attr,
		       sizeof(struct fi_tx_attr));
		memcpy(&new_ep->rx_attr, &cxi_ep->rx_attr,
		       sizeof(struct fi_rx_attr));
		ret = cxip_setopflags(&new_ep->tx_attr, &new_ep->rx_attr,
				      alias->flags);
		if (ret) {
			free(new_ep);
			return -FI_EINVAL;
		}
		new_ep->ep_obj = cxi_ep->ep_obj;
		new_ep->is_alias = 1;
		memcpy(&new_ep->ep, &cxi_ep->ep, sizeof(struct fid_ep));
		*alias->fid = &new_ep->ep.fid;
		ofi_atomic_inc32(&new_ep->ep_obj->ref);
		break;
	case FI_GETOPSFLAG:
		if (!arg)
			return -FI_EINVAL;
		ret = cxip_getopflags(&cxi_ep->tx_attr, &cxi_ep->rx_attr,
				      (uint64_t *)arg);
		if (ret)
			return -FI_EINVAL;
		break;
	case FI_SETOPSFLAG:
		if (!arg)
			return -FI_EINVAL;
		ret = cxip_setopflags(&cxi_ep->tx_attr, &cxi_ep->rx_attr,
				      *(uint64_t *)arg);
		if (ret)
			return -FI_EINVAL;
		break;
	case FI_ENABLE:
		ep_fid = container_of(fid, struct fid_ep, fid);
		return cxip_ep_enable(ep_fid);
	default:
		return -FI_EINVAL;
	}

	return FI_SUCCESS;
}

struct fi_ops cxip_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = cxip_ep_close,
	.bind = cxip_ep_bind,
	.control = cxip_ep_control,
	.ops_open = fi_no_ops_open,
};

/*=======================================*/

/**
 * Get EP options
 *
 * Provider getopt() implementation for EPs
 *
 * @param fid : EP fid
 * @param level : option level (must be FI_OPT_ENDPOINT)
 * @param optname : option name
 * @param optval : returned option
 * @param optlen : returned length
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_getopt(fid_t fid, int level, int optname, void *optval,
			  size_t *optlen)
{
	struct cxip_ep *cxi_ep;

	cxi_ep = container_of(fid, struct cxip_ep, ep.fid);

	if (level != FI_OPT_ENDPOINT)
		return -FI_ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:

		if (!optval || !optlen)
			return -FI_EINVAL;

		*(size_t *)optval = cxi_ep->ep_obj->min_multi_recv;
		*optlen = sizeof(size_t);
		break;

	default:
		return -FI_ENOPROTOOPT;
	}

	return 0;
}

/**
 * Set EP options
 *
 * Provider setopt() implementation for EP
 *
 * @param fid : EP fid
 * @param level : option level (must be FI_OPT_ENDPOINT)
 * @param optname : option name
 * @param optval : option value to set
 * @param optlen : option length
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_setopt(fid_t fid, int level, int optname, const void *optval,
			  size_t optlen)
{
	size_t i;
	struct cxip_ep *cxi_ep;

	cxi_ep = container_of(fid, struct cxip_ep, ep.fid);

	if (level != FI_OPT_ENDPOINT)
		return -FI_ENOPROTOOPT;

	switch (optname) {
	case FI_OPT_MIN_MULTI_RECV:

		if (!optval)
			return -FI_EINVAL;

		cxi_ep->ep_obj->min_multi_recv = *(size_t *)optval;
		for (i = 0; i < cxi_ep->ep_obj->ep_attr.rx_ctx_cnt; i++) {
			if (cxi_ep->ep_obj->rxcs[i] != NULL) {
				cxi_ep->ep_obj->rxcs[i]->min_multi_recv =
					cxi_ep->ep_obj->min_multi_recv;
			}
		}
		break;

	default:
		return -FI_ENOPROTOOPT;
	}

	return 0;
}

/**
 * Create SEP TX context.
 *
 * Provider fi_tx_context() implementation.
 *
 * The index value must be less than the SEP num_txc value.
 *
 * Attributes default to the SEP TX attributes if attr is NULL.
 *
 * Note that this returns a struct fid_ep*, implying that this object is an
 * endpoint, rather than a TX context. It is not actually an endpoint, and the
 * functions it supports differ from an EP or SEP.
 *
 * @param ep : SEP fid
 * @param index : SEP TX index
 * @param attr : override attributes for TX context
 * @param tx_ep : return value of TX context
 * @param context : user-specified opaque context
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_txc(struct fid_ep *ep, int index, struct fi_tx_attr *attr,
		       struct fid_ep **tx_ep, void *context)
{
	struct fi_tx_attr *ep_attr;
	struct cxip_ep *cxi_ep;
	struct cxip_txc *txc;

	cxi_ep = container_of(ep, struct cxip_ep, ep);
	if (cxi_ep->ep_obj->fclass != FI_CLASS_SEP || !tx_ep ||
	    index >= cxi_ep->ep_obj->ep_attr.tx_ctx_cnt || index < 0)
		return -FI_EINVAL;

	/* Cannot reuse index */
	if (cxi_ep->ep_obj->txcs[index])
		return -FI_EADDRINUSE;

	/* parent SEP attributes */
	ep_attr = &cxi_ep->tx_attr;

	if (attr) {
		/* validate and use the supplied attributes */
		if (ofi_check_tx_attr(&cxip_prov, ep_attr, attr, 0) ||
		    ofi_check_attr_subset(&cxip_prov, ep_attr->caps,
					  attr->caps))
			return -FI_ENODATA;
		txc = cxip_txc_alloc(attr, context, 0);
	} else {
		/* use the SEP TX attr */
		txc = cxip_txc_alloc(ep_attr, context, 0);
	}
	if (!txc)
		return -FI_ENOMEM;

	txc->tx_id = index;
	txc->ep_obj = cxi_ep->ep_obj;
	txc->domain = cxi_ep->ep_obj->domain;

	txc->fid.ctx.fid.ops = &cxip_ctx_ops;
	txc->fid.ctx.ops = &cxip_ctx_ep_ops;
	txc->fid.ctx.msg = &cxip_ep_msg_ops;
	txc->fid.ctx.tagged = &cxip_ep_tagged_ops;
	txc->fid.ctx.rma = &cxip_ep_rma;
	txc->fid.ctx.atomic = &cxip_ep_atomic;

	*tx_ep = &txc->fid.ctx;
	cxi_ep->ep_obj->txcs[index] = txc;
	ofi_atomic_inc32(&cxi_ep->ep_obj->num_txc);
	ofi_atomic_inc32(&cxi_ep->ep_obj->domain->ref);

	return 0;
}

/**
 * Create SEP RX context.
 *
 * Provider fi_rx_context() implementation.
 *
 * The index value must be less than the SEP num_rxc value.
 *
 * Attributes default to the SEP RX attributes if attr is NULL.
 *
 * Note that this returns a struct fid_ep*, implying that this object is an
 * endpoint, rather than an RX context. It is not actually an endpoint, and the
 * functions it supports differ from an EP or SEP.
 *
 * @param ep : SEP fid
 * @param index : SEP RX index
 * @param attr : override attributes for RX context
 * @param tx_ep : return value of RX context
 * @param context : user-specified opaque context
 *
 * @return int : 0 on success, -errno on failure
 */
static int cxip_ep_rxc(struct fid_ep *ep, int index, struct fi_rx_attr *attr,
			  struct fid_ep **rx_ep, void *context)
{
	struct fi_rx_attr *ep_attr;
	struct cxip_ep *cxi_ep;
	struct cxip_rxc *rxc;

	cxi_ep = container_of(ep, struct cxip_ep, ep);
	if (cxi_ep->ep_obj->fclass != FI_CLASS_SEP || !rx_ep ||
	    index >= cxi_ep->ep_obj->ep_attr.rx_ctx_cnt || index < 0)
		return -FI_EINVAL;

	/* Cannot reuse index */
	if (cxi_ep->ep_obj->rxcs[index])
		return -FI_EADDRINUSE;

	/* parent SEP attributes */
	ep_attr = &cxi_ep->rx_attr;

	if (attr) {
		/* We need a partial info structure for check */
		struct fi_domain_attr dom_attr = {
			/* This suppresses a warning message */
			.resource_mgmt = FI_RM_ENABLED,
		};
		struct fi_info info = {
			.domain_attr = &dom_attr,
			.rx_attr = ep_attr,
		};

		if (ofi_check_rx_attr(&cxip_prov, &info, attr, 0) ||
		    ofi_check_attr_subset(&cxip_prov, ep_attr->caps,
					  attr->caps))
			return -FI_ENODATA;
		rxc = cxip_rxc_alloc(attr, context, 0);
	} else {
		rxc = cxip_rxc_alloc(ep_attr, context, 0);
	}
	if (!rxc)
		return -FI_ENOMEM;

	rxc->rx_id = index;
	rxc->ep_obj = cxi_ep->ep_obj;
	rxc->domain = cxi_ep->ep_obj->domain;

	rxc->ctx.fid.ops = &cxip_ctx_ops;
	rxc->ctx.ops = &cxip_ctx_ep_ops;
	rxc->ctx.msg = &cxip_ep_msg_ops;
	rxc->ctx.tagged = &cxip_ep_tagged_ops;

	rxc->min_multi_recv = cxi_ep->ep_obj->min_multi_recv;
	*rx_ep = &rxc->ctx;
	cxi_ep->ep_obj->rxcs[index] = rxc;
	ofi_atomic_inc32(&cxi_ep->ep_obj->num_rxc);
	ofi_atomic_inc32(&cxi_ep->ep_obj->domain->ref);

	return 0;
}

struct fi_ops_ep cxip_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = cxip_ep_cancel,
	.getopt = cxip_ep_getopt,
	.setopt = cxip_ep_setopt,
	.tx_ctx = cxip_ep_txc,
	.rx_ctx = cxip_ep_rxc,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

/*=======================================*/

#if 0
/* shared contexts */
int cxip_stx(struct fid_domain *domain, struct fi_tx_attr *attr,
		 struct fid_stx **stx, void *context)
{
	struct cxip_domain *dom;
	struct cxip_txc *txc;

	if ((attr && cxip_verify_tx_attr(attr)) || !stx)
		return -FI_EINVAL;

	dom = container_of(domain, struct cxip_domain, util_domain.domain_fid);

	txc = cxip_stx_alloc(attr ? attr : &cxip_stx_attr, context);
	if (!txc)
		return -FI_ENOMEM;

	txc->domain = dom;

	txc->fid.stx.fid.ops = &cxip_ctx_ops;
	txc->fid.stx.ops = &cxip_ep_ops;
	ofi_atomic_inc32(&dom->ref);

	*stx = &txc->fid.stx;

	return 0;
}

/* shared contexts */
int cxip_srx(struct fid_domain *domain, struct fi_rx_attr *attr,
		 struct fid_ep **srx, void *context)
{
	struct cxip_domain *dom;
	struct cxip_rxc *rxc;

	if ((attr && cxip_verify_rx_attr(attr)) || !srx)
		return -FI_EINVAL;

	dom = container_of(domain, struct cxip_domain, util_domain.domain_fid);
	rxc = cxip_rxc_alloc(attr ? attr : &cxip_srx_attr, context, 0);
	if (!rxc)
		return -FI_ENOMEM;

	rxc->domain = dom;
	rxc->ctx.fid.fclass = FI_CLASS_SRX_CTX;

	rxc->ctx.fid.ops = &cxip_ctx_ops;
	rxc->ctx.ops = &cxip_ctx_ep_ops;
	rxc->ctx.msg = &cxip_ep_msg_ops;
	rxc->ctx.tagged = &cxip_ep_tagged_ops;
	rxc->enabled = true;

	/* default config */
	rxc->min_multi_recv = CXIP_EP_MIN_MULTI_RECV;
	*srx = &rxc->ctx;
	ofi_atomic_inc32(&dom->ref);

	return 0;
}
#endif

/**
 * Allocate endpoint.
 *
 * Support function for:
 * - cxip_rdm_ep()  : implements fi_endpoint()
 * - cxip_rdm_sep() : implements fi_scalable_ep()
 *
 * The Standard EP (FI_CLASS_EP) automatically creates one TX context and one RX
 * context.
 *
 * The Scalable EP (FI_CLASS_SEP) creates space for the TX and RX contexts as
 * specified by the respective counts in the info structure, but does not create
 * the contexts. They must be explicitly be created by calling fi_tx_context()
 * or fi_rx_context().
 *
 * @param domain : target domain fid
 * @param hints : fabric endpoint hints from fi_getinfo()
 * @param ep : return allocated endpoint
 * @param context : user-defined context
 * @param fclass : FI_CLASS_EP or FI_CLASS_SEP
 *
 * @return int : 0 on success, -errno on failure
 */
static int
cxip_alloc_endpoint(struct fid_domain *domain, struct fi_info *hints,
		    struct cxip_ep **ep, void *context, size_t fclass)
{
	int ret;
	struct cxip_domain *cxi_dom;
	struct cxip_ep *cxi_ep = NULL;
	struct cxip_txc *txc = NULL;
	struct cxip_rxc *rxc = NULL;
	uint32_t nic;
	uint32_t pid;

	if (!domain || !hints || !hints->ep_attr || !hints->tx_attr ||
	    !hints->rx_attr)
		return -FI_EINVAL;

	if (fclass == FI_CLASS_SEP &&
	    (ofi_send_allowed(hints->tx_attr->caps) ||
	     ofi_recv_allowed(hints->tx_attr->caps))) {
		CXIP_LOG_ERROR("Scalable EPs do not support messaging.\n");
		return -FI_ENOPROTOOPT;
	}

	ret = ofi_prov_check_info(&cxip_util_prov, CXIP_FI_VERSION, hints);
	if (ret != FI_SUCCESS)
		return -FI_ENOPROTOOPT;

	/* domain, info, info->ep_attr, and ep are != NULL */
	cxi_dom = container_of(domain, struct cxip_domain,
			       util_domain.domain_fid);

	nic = cxi_dom->nic_addr;
	if (hints->src_addr) {
		struct cxip_addr *src = hints->src_addr;
		if (src->nic != nic) {
			CXIP_LOG_ERROR("bad src_addr NIC value\n");
			ret = -FI_EINVAL;
			goto err;
		}
		pid = src->pid;
	} else {
		pid = C_PID_ANY;
	}

	/* Allocate memory for the EP */
	cxi_ep = calloc(1, sizeof(*cxi_ep));
	if (!cxi_ep) {
		ret = -FI_ENOMEM;
		goto err;
	}

	cxi_ep->ep_obj = calloc(1, sizeof(struct cxip_ep_obj));
	if (!cxi_ep->ep_obj) {
		ret = -FI_ENOMEM;
		goto err;
	}

	/* Copy EP attributes from hints */
	cxi_ep->ep_obj->ep_attr = *hints->ep_attr;
	cxi_ep->tx_attr = *hints->tx_attr;
	cxi_ep->rx_attr = *hints->rx_attr;

	/* Complete EP fid initialization */
	cxi_ep->ep.fid.fclass = fclass;
	cxi_ep->ep.fid.context = context;
	cxi_ep->ep.fid.ops = &cxip_ep_fi_ops;

	/* Initialize object */
	ofi_atomic_initialize32(&cxi_ep->ep_obj->ref, 0);
	ofi_atomic_initialize32(&cxi_ep->ep_obj->num_txc, 0);
	ofi_atomic_initialize32(&cxi_ep->ep_obj->num_rxc, 0);
	fastlock_init(&cxi_ep->ep_obj->lock);

	cxi_ep->ep_obj->fclass = fclass;
	cxi_ep->ep_obj->domain = cxi_dom;
	cxi_ep->ep_obj->min_multi_recv = CXIP_EP_MIN_MULTI_RECV;
	cxi_ep->ep_obj->src_addr.nic = nic;
	cxi_ep->ep_obj->src_addr.pid = pid;
	cxi_ep->ep_obj->src_addr.valid = 1;
	cxi_ep->ep_obj->fi_addr = FI_ADDR_NOTAVAIL;
	memset(cxi_ep->ep_obj->rdzv_ids, 0xFF,
	       sizeof(cxi_ep->ep_obj->rdzv_ids));
	fastlock_init(&cxi_ep->ep_obj->rdzv_id_lock);

	switch (fclass) {
	case FI_CLASS_EP:
		/* standard EP */
		cxi_ep->ep.ops = &cxip_ep_ops;
		cxi_ep->ep.cm = &cxip_ep_cm_ops;
		cxi_ep->ep.msg = &cxip_ep_msg_ops;
		cxi_ep->ep.rma = &cxip_ep_rma;
		cxi_ep->ep.tagged = &cxip_ep_tagged_ops;
		cxi_ep->ep.atomic = &cxip_ep_atomic;
		break;

	case FI_CLASS_SEP:
		/* scalable EP */
		cxi_ep->ep.ops = &cxip_ep_ops;
		cxi_ep->ep.cm = &cxip_ep_cm_ops;
		/* msg, rma, tagged, atomic must use TX context */
		break;

	default:
		ret = -FI_EINVAL;
		goto err;
	}

	/* Evaluate TX/RX counts.
	 *
	 * FI_CLASS_EP has one TX and one RX context, either or both of which
	 * may be shared. If shared, this must be created without that context.
	 * The txc or rxc field will be set to point to the shared context
	 * when this EP is bound to the shared context. No space is allocated
	 * for the context.
	 *
	 * If not shared, this allocates space for one context of each type,
	 * creates the context as txcs[0] and/or rxcs[0], and sets
	 * txc and/or rxc to point to the context. Note that the hint
	 * count is ignored, and treated as 1.
	 *
	 * FI_CLASS_SEP does not support shared contexts. It can have zero or
	 * more TX/RX contexts (they cannot both be zero), and allocates space
	 * for them, but does not create them. They must be created and used
	 * explicitly by the application. Creating them fills entries in the
	 * txcs[] or rxcs[] arrays. The txc and rxc fields will
	 * remain NULL.
	 */
	if (fclass == FI_CLASS_EP) {
		/* Standard EP has 1 TX and 1 RX, may be shared.
		 * Otherwise, ignore supplied value, substitute 1.
		 */
		if (cxi_ep->ep_obj->ep_attr.tx_ctx_cnt == FI_SHARED_CONTEXT) {
			cxi_ep->ep_obj->tx_shared = 1;
			cxi_ep->ep_obj->ep_attr.tx_ctx_cnt = 0;
		} else {
			cxi_ep->ep_obj->ep_attr.tx_ctx_cnt = 1;
		}
		if (cxi_ep->ep_obj->ep_attr.rx_ctx_cnt == FI_SHARED_CONTEXT) {
			cxi_ep->ep_obj->rx_shared = 1;
			cxi_ep->ep_obj->ep_attr.rx_ctx_cnt = 0;
		} else {
			cxi_ep->ep_obj->ep_attr.rx_ctx_cnt = 1;
		}
	} else {
		/* Scalable EP may not use shared CTX */
		if (cxi_ep->ep_obj->ep_attr.tx_ctx_cnt == FI_SHARED_CONTEXT ||
		    cxi_ep->ep_obj->ep_attr.rx_ctx_cnt == FI_SHARED_CONTEXT) {
			CXIP_LOG_ERROR("shared CTX incompatible with SEP\n");
			ret = -FI_EINVAL;
			goto err;
		}
		/* Scalable EP has a limit on the number of CTX */
		if (cxi_ep->ep_obj->ep_attr.tx_ctx_cnt > CXIP_EP_MAX_TX_CNT ||
		    cxi_ep->ep_obj->ep_attr.rx_ctx_cnt > CXIP_EP_MAX_RX_CNT) {
			CXIP_LOG_ERROR("too many CTX for SEP\n");
			ret = -FI_EINVAL;
			goto err;
		}
		/* Scalable EP must have at least one TX OR one RX CTX */
		if (!cxi_ep->ep_obj->ep_attr.tx_ctx_cnt &&
		    !cxi_ep->ep_obj->ep_attr.rx_ctx_cnt) {
			CXIP_LOG_ERROR("no CTX for SEP\n");
			ret = -FI_EINVAL;
			goto err;
		}
	}

	/* Allocate space for TX contexts */
	if (cxi_ep->ep_obj->ep_attr.tx_ctx_cnt) {
		cxi_ep->ep_obj->txcs =
			calloc(cxi_ep->ep_obj->ep_attr.tx_ctx_cnt,
			       sizeof(struct cxip_txc *));
		if (!cxi_ep->ep_obj->txcs) {
			ret = -FI_ENOMEM;
			goto err;
		}
	}

	/* Allocate space for RX contexts */
	if (cxi_ep->ep_obj->ep_attr.rx_ctx_cnt) {
		cxi_ep->ep_obj->rxcs =
			calloc(cxi_ep->ep_obj->ep_attr.rx_ctx_cnt,
			       sizeof(struct cxip_rxc *));
		if (!cxi_ep->ep_obj->rxcs) {
			ret = -FI_ENOMEM;
			goto err;
		}
	}

	/* Standard EP automatically creates TX/RX, unless shared */
	if (cxi_ep->ep_obj->fclass == FI_CLASS_EP) {
		if (!cxi_ep->ep_obj->tx_shared) {
			txc = cxip_txc_alloc(&cxi_ep->tx_attr, context,
						   cxi_ep->ep_obj->tx_shared);
			if (!txc) {
				ret = -FI_ENOMEM;
				goto err;
			}
			txc->ep_obj = cxi_ep->ep_obj;
			txc->domain = cxi_dom;
			txc->tx_id = 0;
			cxi_ep->ep_obj->txcs[0] = txc;
		}

		if (!cxi_ep->ep_obj->rx_shared) {
			rxc = cxip_rxc_alloc(&cxi_ep->rx_attr, context,
					     cxi_ep->ep_obj->rx_shared);
			if (!rxc) {
				ret = -FI_ENOMEM;
				goto err;
			}
			rxc->ep_obj = cxi_ep->ep_obj;
			rxc->domain = cxi_dom;
			rxc->rx_id = 0;
			cxi_ep->ep_obj->rxcs[0] = rxc;
		}
	}

	ofi_atomic_inc32(&cxi_dom->ref);

	*ep = cxi_ep;
	return 0;

err:
	if (rxc)
		cxip_rxc_free(rxc);
	if (txc)
		cxip_txc_free(txc);
	if (cxi_ep) {
		if (cxi_ep->ep_obj) {
			free(cxi_ep->ep_obj->rxcs);
			free(cxi_ep->ep_obj->txcs);
		}
		free(cxi_ep->ep_obj);
	}
	free(cxi_ep);
	return ret;
}

/*
 * cxip_endpoint() - Provider fi_endpoint() implementation.
 */
int cxip_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep, void *context)
{
	int ret;
	struct cxip_ep *cxip_ep;

	if (!ep)
		return -FI_EINVAL;

	ret = cxip_alloc_endpoint(domain, info, &cxip_ep, context,
				  FI_CLASS_EP);
	if (ret)
		return ret;

	*ep = &cxip_ep->ep;

	return FI_SUCCESS;
}

/*
 * cxip_scalable_ep() - Provider fi_scalable_ep() implementation.
 */
int cxip_scalable_ep(struct fid_domain *domain, struct fi_info *info,
		     struct fid_ep **sep, void *context)
{
	int ret;
	struct cxip_ep *cxip_ep;

	if (!sep)
		return -FI_EINVAL;

	ret = cxip_alloc_endpoint(domain, info, &cxip_ep, context,
				  FI_CLASS_SEP);
	if (ret)
		return ret;

	*sep = &cxip_ep->ep;

	return FI_SUCCESS;
}
