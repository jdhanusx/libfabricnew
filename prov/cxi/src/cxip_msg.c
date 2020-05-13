/*
 * SPDX-License-Identifier: GPL-2.0
 *
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

static int cxip_recv_req_queue(struct cxip_req *req);
static int cxip_recv_req_dropped(struct cxip_req *req);
static void cxip_recv_req_dequeue_nolock(struct cxip_req *req);
static void cxip_recv_req_dequeue(struct cxip_req *req);
static ssize_t _cxip_recv_req(struct cxip_req *req, bool restart_seq);

static int cxip_send_req_dropped(struct cxip_txc *txc, struct cxip_req *req);
static void cxip_send_req_dequeue(struct cxip_txc *txc, struct cxip_req *req);

/*
 * init_ux_send() - Initialize the unexpected Send record using a target event.
 */
void init_ux_send(struct cxip_ux_send *ux_send, const union c_event *event)
{
	union cxip_match_bits mb = {
		.raw = event->tgt_long.match_bits
	};

	ux_send->start = event->tgt_long.start;
	ux_send->initiator = event->tgt_long.initiator.initiator.process;
	ux_send->rdzv_id = event->tgt_long.rendezvous_id;
	ux_send->src_offset = event->tgt_long.remote_offset;
	ux_send->rlen = event->tgt_long.rlength;
	ux_send->mlen = event->tgt_long.mlength;
	ux_send->mb.raw = event->tgt_long.match_bits;
	ux_send->data = event->tgt_long.header_data;

	if (event->tgt_long.rendezvous)
		ux_send->rdzv_id = event->tgt_long.rendezvous_id;
	else
		ux_send->rdzv_id = mb.rdzv_id_hi;
	ux_send->rdzv_lac = mb.rdzv_lac;
}

/*
 * match_ux_send() - Search for an unexpected Put that matches a Put Overflow
 * event.
 *
 * Caller must hold rxc->rx_lock.
 */
static struct cxip_ux_send *
match_ux_send(struct cxip_rxc *rxc, const union c_event *event)
{
	struct cxip_ux_send *ux_send;

	if (event->tgt_long.rendezvous) {
		uint32_t process = event->tgt_long.initiator.initiator.process;
		uint32_t ev_rdzv_id = event->tgt_long.rendezvous_id;

		/* Rendezvous events are correlated using rendezvous_id and
		 * initiator.
		 */
		dlist_foreach_container(&rxc->ux_rdzv_sends,
					struct cxip_ux_send, ux_send,
					ux_entry) {
			if ((ux_send->rdzv_id == ev_rdzv_id) &&
			    (ux_send->initiator == process))
				return ux_send;
		}
	} else {
		/* All other events are correlated using start address.
		 *
		 * TODO this assumes all overflow buffers use the same AC so
		 * all start pointers are unique.
		 */
		dlist_foreach_container(&rxc->ux_sends, struct cxip_ux_send,
					ux_send, ux_entry) {
			if (ux_send->start == event->tgt_long.start)
				return ux_send;
		}
	}

	return NULL;
}

/*
 * match_ux_recv() - Search for a previously matched request that matches an
 * unexpected Put event.
 *
 * Caller must hold rxc->rx_lock.
 */
static struct cxip_req *
match_ux_recv(struct cxip_rxc *rxc, const union c_event *event)
{
	struct cxip_req *req;

	if (event->tgt_long.rendezvous) {
		uint32_t process = event->tgt_long.initiator.initiator.process;
		uint32_t ev_rdzv_id = event->tgt_long.rendezvous_id;

		/* Rendezvous events are correlated using rendezvous_id and
		 * initiator.
		 */
		dlist_foreach_container(&rxc->ux_rdzv_recvs, struct cxip_req,
					req, recv.rxc_entry) {
			if ((req->recv.rdzv_id == ev_rdzv_id) &&
			    (req->recv.initiator == process))
				return req;
		}
	} else {
		/* All other events are correlated using start address.
		 *
		 * TODO this assumes all overflow buffers use the same AC so
		 * all start pointers are unique.
		 */
		dlist_foreach_container(&rxc->ux_recvs, struct cxip_req, req,
					recv.rxc_entry) {
			if (req->recv.oflow_start == event->tgt_long.start)
				return req;
		}
	}

	return NULL;
}

/*
 * recv_req_src_addr() - Translate request source address to FI address.
 */
static fi_addr_t recv_req_src_addr(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;

	/* If the FI_SOURCE capability is enabled, convert the initiator's
	 * address to an FI address to be reported in a CQ event. If
	 * application AVs are symmetric, the match_id in the EQ event is
	 * logical and translation is not needed. Otherwise, translate the
	 * physical address in the EQ event to logical FI address.
	 */
	if (rxc->attr.caps & FI_SOURCE) {
		uint32_t pid_bits = rxc->domain->iface->dev->info.pid_bits;
		uint32_t nic;
		uint32_t pid;

		if (req->recv.init_logical)
			return req->recv.initiator;

		nic = CXI_MATCH_ID_EP(pid_bits, req->recv.initiator);
		pid = CXI_MATCH_ID_PID(pid_bits, req->recv.initiator);

		return _cxip_av_reverse_lookup(rxc->ep_obj->av, nic, pid);
	}

	return FI_ADDR_NOTAVAIL;
}

/*
 * recv_req_complete() - Complete receive request.
 */
static void recv_req_complete(struct cxip_req *req)
{
	assert(dlist_empty(&req->recv.children));

	if (req->recv.recv_md)
		cxip_unmap(req->recv.recv_md);
	ofi_atomic_dec32(&req->recv.rxc->orx_reqs);
	cxip_cq_req_free(req);
}

/*
 * recv_req_report() - Report the completion of a receive operation.
 */
static void recv_req_report(struct cxip_req *req)
{
	int ret;
	int truncated;
	int err;
	fi_addr_t src_addr;
	int success_event = (req->flags & FI_COMPLETION);

	req->flags &= (FI_MSG | FI_TAGGED | FI_RECV);

	if (req->recv.parent) {
		struct cxip_req *parent = req->recv.parent;

		parent->recv.mrecv_bytes -= req->data_len;
		CXIP_LOG_DBG("Putting %lu mrecv bytes (req: %p left: %lu addr: %#lx)\n",
			      req->data_len, parent, parent->recv.mrecv_bytes,
			      req->buf);
		if (parent->recv.mrecv_bytes < req->recv.rxc->min_multi_recv) {
			CXIP_LOG_DBG("Freeing parent: %p\n", req->recv.parent);
			recv_req_complete(req->recv.parent);

			req->flags |= FI_MULTI_RECV;
		}
	}

	truncated = req->recv.rlen - req->data_len;
	if (req->recv.rc == C_RC_OK && !truncated) {
		CXIP_LOG_DBG("Request success: %p\n", req);

		if (success_event) {
			src_addr = recv_req_src_addr(req);
			ret = cxip_cq_req_complete_addr(req, src_addr);
			if (ret != FI_SUCCESS)
				CXIP_LOG_ERROR("Failed to report completion: %d\n",
					       ret);
		}

		if (req->recv.rxc->recv_cntr) {
			ret = cxip_cntr_mod(req->recv.rxc->recv_cntr, 1, false,
					    false);
			if (ret)
				CXIP_LOG_ERROR("cxip_cntr_mod returned: %d\n",
					       ret);
		}
	} else {
		if (req->recv.rc == C_RC_CANCELED) {
			err = FI_ECANCELED;
			if (req->recv.multi_recv)
				req->flags |= FI_MULTI_RECV;
			CXIP_LOG_DBG("Request cancelled: %p (err: %d, %s)\n",
				     req, err, cxi_rc_to_str(req->recv.rc));
		} else if (truncated) {
			err = FI_EMSGSIZE;
			CXIP_LOG_DBG("Request truncated: %p (err: %d, %s)\n",
				     req, err, cxi_rc_to_str(req->recv.rc));
		} else {
			err = FI_EIO;
			CXIP_LOG_ERROR("Request error: %p (err: %d, %s)\n",
				       req, err, cxi_rc_to_str(req->recv.rc));
		}

		ret = cxip_cq_req_error(req, truncated, err, req->recv.rc,
					NULL, 0);
		if (ret != FI_SUCCESS)
			CXIP_LOG_ERROR("Failed to report error: %d\n", ret);

		if (req->recv.rxc->recv_cntr) {
			ret = cxip_cntr_mod(req->recv.rxc->recv_cntr, 1, false,
					    true);
			if (ret)
				CXIP_LOG_ERROR("cxip_cntr_mod returned: %d\n",
					       ret);
		}
	}
}

/*
 * recv_req_tgt_event() - Update common receive request fields
 *
 * Populate a receive request with information found in all receive event
 * types.
 */
static void
recv_req_tgt_event(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	union cxip_match_bits mb = {
		.raw = event->tgt_long.match_bits
	};
	uint32_t pid_bits = rxc->domain->iface->dev->info.pid_bits;

	assert(event->hdr.event_type == C_EVENT_PUT ||
	       event->hdr.event_type == C_EVENT_PUT_OVERFLOW ||
	       event->hdr.event_type == C_EVENT_RENDEZVOUS);

	/* Rendezvous events contain the wrong match bits. */
	if (event->hdr.event_type != C_EVENT_RENDEZVOUS)
		req->tag = mb.tag;

	/* remote_offset is not provided in Overflow events. */
	if (event->hdr.event_type != C_EVENT_PUT_OVERFLOW)
		req->recv.src_offset = event->tgt_long.remote_offset;

	/* Only need one event to set remaining fields. */
	if (req->recv.tgt_event)
		return;
	req->recv.tgt_event = true;

	/* rlen is used to detect truncation. */
	req->recv.rlen = event->tgt_long.rlength;

	/* RC is used when generating completion events. */
	req->recv.rc = cxi_tgt_event_rc(event);

	/* Header data is provided in all completion events. */
	req->data = event->tgt_long.header_data;

	/* rdzv_id is used to correlate Put and Put Overflow events when using
	 * offloaded RPut. Otherwise, Overflow buffer start address is used to
	 * correlate events.
	 */
	if (event->tgt_long.rendezvous) {
		req->recv.rdzv_id = event->tgt_long.rendezvous_id;
	} else {
		req->recv.oflow_start = event->tgt_long.start;
		req->recv.rdzv_id = mb.rdzv_id_hi;
	}
	req->recv.rdzv_lac = mb.rdzv_lac;
	req->recv.rdzv_mlen = event->tgt_long.mlength;

	/* Initiator is provided in completion events. */
	if (event->hdr.event_type == C_EVENT_RENDEZVOUS) {
		uint32_t dfa = event->tgt_long.initiator.initiator.process;

		req->recv.initiator = cxi_dfa_to_init(dfa, pid_bits);
	} else {
		uint32_t init = event->tgt_long.initiator.initiator.process;

		if (rxc->ep_obj->av->attr.flags & FI_SYMMETRIC) {
			/* Take PID out of logical address. */
			req->recv.initiator = CXI_MATCH_ID_EP(pid_bits, init);
			req->recv.init_logical = true;
		} else {
			req->recv.initiator = init;
		}
	}

	/* data_len must be set uniquely for each protocol! */
}

/*
 * rdzv_mrecv_req_lookup() - Search for a matching rendezvous, multi-receive
 * child request.
 */
static struct cxip_req *rdzv_mrecv_req_lookup(struct cxip_req *req,
					      const union c_event *event,
					      uint32_t *initiator,
					      uint32_t *rdzv_id)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t pid_bits = rxc->domain->iface->dev->info.pid_bits;
	struct cxip_req *child_req;
	uint32_t ev_init;
	uint32_t ev_rdzv_id;

	if (event->hdr.event_type == C_EVENT_REPLY) {
		struct cxi_rdzv_user_ptr *user_ptr;

		/* Events for software-issued operations will return a
		 * reference to the correct request.
		 */
		if (!event->init_short.rendezvous)
			return req;

		user_ptr = (struct cxi_rdzv_user_ptr *)
				&event->init_short.user_ptr;

		ev_init = CXI_MATCH_ID(pid_bits, user_ptr->src_pid,
					user_ptr->src_nid);
		ev_rdzv_id = user_ptr->rendezvous_id;
	} else if (event->hdr.event_type == C_EVENT_RENDEZVOUS) {
		struct cxip_rxc *rxc = req->recv.rxc;
		uint32_t pid_bits = rxc->domain->iface->dev->info.pid_bits;
		uint32_t dfa = event->tgt_long.initiator.initiator.process;

		ev_init = cxi_dfa_to_init(dfa, pid_bits);
		ev_rdzv_id = event->tgt_long.rendezvous_id;
	} else {
		ev_init = event->tgt_long.initiator.initiator.process;
		ev_rdzv_id = event->tgt_long.rendezvous_id;
	}

	*initiator = ev_init;
	*rdzv_id = ev_rdzv_id;

	/* Events for hardware-issued operations will return a rendezvous_id
	 * and initiator data. Use these fields to find a matching child
	 * request.
	 */
	dlist_foreach_container(&req->recv.children,
				struct cxip_req, child_req,
				recv.children) {
		if (child_req->recv.rdzv_id == ev_rdzv_id &&
		    child_req->recv.initiator == ev_init) {
			return child_req;
		}
	}

	return NULL;
}

/*
 * mrecv_req_dup() - Create a new request using an event targeting a
 * multi-recv buffer.
 *
 * @mrecv_req: A previously posted multi-recv buffer request.
 */
static struct cxip_req *mrecv_req_dup(struct cxip_req *mrecv_req)
{
	struct cxip_rxc *rxc = mrecv_req->recv.rxc;
	struct cxip_req *req;

	req = cxip_cq_req_alloc(rxc->recv_cq, 0, rxc);
	if (!req)
		return NULL;

	/* Duplicate the parent request. */
	req->cb = mrecv_req->cb;
	req->flags = mrecv_req->flags;
	req->recv = mrecv_req->recv;

	/* Update fields specific to this Send */
	req->recv.parent = mrecv_req;

	/* Start pointer and data_len must be set elsewhere! */

	return req;
}

/*
 * rdzv_mrecv_req_event() - Look up a multi-recieve child request using an
 * event and multi-recv request.
 *
 * Each rendezvous Put transaction targeting a multi-receive buffer is tracked
 * using a separate child request. A child request is uniquely identified by
 * rendezvous ID and source address. Return a reference to a child request
 * which matches the event. Allocate a new child request, if necessary.
 */
static struct cxip_req *
rdzv_mrecv_req_event(struct cxip_req *mrecv_req, const union c_event *event)
{
	uint32_t ev_init;
	uint32_t ev_rdzv_id;
	struct cxip_req *req;

	assert(event->hdr.event_type == C_EVENT_REPLY ||
	       event->hdr.event_type == C_EVENT_PUT ||
	       event->hdr.event_type == C_EVENT_PUT_OVERFLOW ||
	       event->hdr.event_type == C_EVENT_RENDEZVOUS);

	req = rdzv_mrecv_req_lookup(mrecv_req, event, &ev_init, &ev_rdzv_id);
	if (!req) {
		req = mrecv_req_dup(mrecv_req);
		if (!req)
			return NULL;

		/* Store event initiator and rdzv_id for matching. */
		if (event->hdr.event_type == C_EVENT_REPLY) {
			req->recv.rdzv_id = ev_rdzv_id;
			req->recv.initiator = ev_init;
		}
		dlist_insert_tail(&req->recv.children,
				  &mrecv_req->recv.children);

		CXIP_LOG_DBG("New child: %p parent: %p event: %s\n",
			     req, mrecv_req, cxi_event_to_str(event));
	} else {
		CXIP_LOG_DBG("Found child: %p parent: %p event: %s\n",
			     req, mrecv_req, cxi_event_to_str(event));
	}

	/* Populate common fields with target events. */
	if (event->hdr.event_type != C_EVENT_REPLY)
		recv_req_tgt_event(req, event);

	return req;
}

/*
 * rdzv_recv_req_event() - Count a rendezvous event.
 *
 * Call for each target rendezvous event generated on a user receive buffer.
 * After three events, a rendezvous receive is complete. The three events could
 * be either:
 *   -Put, Rendezvous, Reply -- or
 *   -Put Overflow, Rendezvous, Reply
 *
 * In either case, the events could be generated in any order. As soon as three
 * events are processed, the request is complete.
 */
static void rdzv_recv_req_event(struct cxip_req *req)
{
	if (++req->recv.rdzv_events == 3) {
		if (req->recv.multi_recv) {
			dlist_remove(&req->recv.children);
			recv_req_report(req);
			cxip_cq_req_free(req);
		} else {
			recv_req_report(req);
			recv_req_complete(req);
		}
	}
}

/*
 * oflow_buf_free() - Free an Overflow buffer.
 *
 * Caller must hold rxc->rx_lock.
 */
static void oflow_buf_free(struct cxip_oflow_buf *oflow_buf)
{
	dlist_remove(&oflow_buf->list);
	ofi_atomic_dec32(&oflow_buf->rxc->oflow_bufs_in_use);

	cxip_unmap(oflow_buf->md);
	free(oflow_buf->buf);
	free(oflow_buf);
}

/*
 * oflow_req_put_bytes() - Consume bytes in the Overflow buffer.
 *
 * An Overflow buffer is freed when all bytes are consumed by the NIC.
 *
 * Caller must hold rxc->rx_lock.
 */
static void oflow_req_put_bytes(struct cxip_req *req, size_t bytes)
{
	CXIP_LOG_DBG("Putting %lu bytes: %p\n", bytes, req);
	req->oflow.oflow_buf->min_bytes -= bytes;
	if (req->oflow.oflow_buf->min_bytes < 0) {
		oflow_buf_free(req->oflow.oflow_buf);
		cxip_cq_req_free(req);
	}
}

/*
 * issue_rdzv_get() - Perform a Get to pull source data from the Initiator of a
 * Send operation.
 */
static int issue_rdzv_get(struct cxip_req *req)
{
	union c_cmdu cmd = {};
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t pid_bits = rxc->domain->iface->dev->info.pid_bits;
	uint32_t pid_idx = rxc->domain->iface->dev->info.rdzv_get_idx;
	uint8_t idx_ext;
	union cxip_match_bits mb = {};
	int ret;
	uint32_t nic;
	uint32_t pid;
	union c_fab_addr dfa;

	if (req->recv.init_logical) {
		struct cxip_addr caddr;

		CXIP_LOG_DBG("Translating inititiator: %x, req: %p\n",
			     req->recv.initiator, req);

		ret = _cxip_av_lookup(rxc->ep_obj->av, req->recv.initiator,
				      &caddr);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_ERROR("Failed to look up FI addr: %d\n",
				       ret);
			abort();
		}
		nic = caddr.nic;
		pid = caddr.pid;
	} else {
		nic = CXI_MATCH_ID_EP(pid_bits, req->recv.initiator);
		pid = CXI_MATCH_ID_PID(pid_bits, req->recv.initiator);
	}

	cmd.full_dma.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.full_dma.command.opcode = C_CMD_GET;
	cmd.full_dma.lac = req->recv.recv_md->md->lac;
	cmd.full_dma.event_send_disable = 1;
	cmd.full_dma.eq = rxc->recv_cq->evtq->eqn;

	mb.rdzv_lac = req->recv.rdzv_lac;
	mb.rdzv_id_lo = req->recv.rdzv_id;
	cmd.full_dma.match_bits = mb.raw;

	cmd.full_dma.user_ptr = (uint64_t)req;
	cmd.full_dma.remote_offset = req->recv.src_offset;

	cxi_build_dfa(nic, pid, pid_bits, pid_idx, &dfa, &idx_ext);
	cmd.full_dma.dfa = dfa;
	cmd.full_dma.index_ext = idx_ext;

	if (req->data_len < req->recv.rdzv_mlen)
		cmd.full_dma.request_len = 0;
	else
		cmd.full_dma.request_len = req->data_len - req->recv.rdzv_mlen;

	cmd.full_dma.local_addr = CXI_VA_TO_IOVA(req->recv.recv_md->md,
						 req->recv.recv_buf);
	cmd.full_dma.local_addr += req->recv.rdzv_mlen;

	fastlock_acquire(&rxc->tx_cmdq->lock);

	/* Issue Rendezvous Get command */
	ret = cxi_cq_emit_dma_f(rxc->tx_cmdq->dev_cmdq, &cmd.full_dma);
	if (ret) {
		CXIP_LOG_ERROR("Failed to queue GET command: %d\n", ret);

		ret = -FI_EAGAIN;
		goto unlock;
	}

	cxi_cq_ring(rxc->tx_cmdq->dev_cmdq);

	ret = FI_SUCCESS;
unlock:
	fastlock_release(&rxc->tx_cmdq->lock);
	return ret;
}

/*
 * cxip_notify_match_cb() - Callback function for match complete notifiction
 * Ack events.
 */
static int
cxip_notify_match_cb(struct cxip_req *req, const union c_event *event)
{
	CXIP_LOG_DBG("Match complete: %p\n", req);

	recv_req_report(req);

	if (req->recv.multi_recv)
		cxip_cq_req_free(req);
	else
		recv_req_complete(req);

	return FI_SUCCESS;
}

/*
 * cxip_notify_match() - Notify the initiator of a Send that the match is
 * complete at the target.
 *
 * A transaction ID corresponding to the matched Send request is sent back to
 * the initiator in the match_bits field of a zero-byte Put.
 */
static int cxip_notify_match(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t pid_bits = rxc->domain->iface->dev->info.pid_bits;
	uint32_t pid_idx = rxc->domain->iface->dev->info.rdzv_get_idx;
	uint32_t init = event->tgt_long.initiator.initiator.process;
	uint32_t nic = CXI_MATCH_ID_EP(pid_bits, init);
	uint32_t pid = CXI_MATCH_ID_PID(pid_bits, init);
	union c_fab_addr dfa;
	uint8_t idx_ext;
	union cxip_match_bits mb = {
		.le_type = CXIP_LE_TYPE_ZBP,
	};
	union cxip_match_bits event_mb;
	union c_cmdu cmd = {};
	int ret;

	event_mb.raw = event->tgt_long.match_bits;
	mb.tx_id = event_mb.tx_id;

	cxi_build_dfa(nic, pid, pid_bits, pid_idx, &dfa, &idx_ext);

	cmd.c_state.event_send_disable = 1;
	cmd.c_state.index_ext = idx_ext;
	cmd.c_state.eq = rxc->recv_cq->evtq->eqn;

	fastlock_acquire(&rxc->tx_cmdq->lock);

	if (memcmp(&rxc->tx_cmdq->c_state, &cmd.c_state,
		   sizeof(cmd.c_state))) {
		/* Update TXQ C_STATE */
		rxc->tx_cmdq->c_state = cmd.c_state;

		ret = cxi_cq_emit_c_state(rxc->tx_cmdq->dev_cmdq,
					  &cmd.c_state);
		if (ret) {
			CXIP_LOG_DBG("Failed to issue C_STATE command: %d\n",
				     ret);

			/* Return error according to Domain Resource
			 * Management
			 */
			ret = -FI_EAGAIN;
			goto err_unlock;
		}

		CXIP_LOG_DBG("Updated C_STATE: %p\n", req);
	}

	memset(&cmd.idc_msg, 0, sizeof(cmd.idc_msg));
	cmd.idc_msg.dfa = dfa;
	cmd.idc_msg.match_bits = mb.raw;

	cmd.idc_msg.user_ptr = (uint64_t)req;

	ret = cxi_cq_emit_idc_msg(rxc->tx_cmdq->dev_cmdq, &cmd.idc_msg,
				  NULL, 0);
	if (ret) {
		CXIP_LOG_DBG("Failed to write IDC: %d\n", ret);

		/* Return error according to Domain Resource Management
		 */
		ret = -FI_EAGAIN;
		goto err_unlock;
	}

	cxi_cq_ring(rxc->tx_cmdq->dev_cmdq);

	fastlock_release(&rxc->tx_cmdq->lock);

	req->cb = cxip_notify_match_cb;

	CXIP_LOG_DBG("Queued match completion message: %p\n", req);

	return FI_SUCCESS;

err_unlock:
	fastlock_release(&rxc->tx_cmdq->lock);

	return ret;
}

/*
 * cxip_oflow_sink_cb() - Process an Overflow buffer event the sink buffer.
 *
 * The sink buffer matches all unexpected long eager sends. The sink buffer
 * truncates all send data and is never exhausted. See cxip_oflow_cb() for more
 * details about Overflow buffer event handling.
 */
static int
cxip_oflow_sink_cb(struct cxip_req *req, const union c_event *event)
{
	int ret;
	struct cxip_rxc *rxc = req->oflow.rxc;
	struct cxip_req *ux_recv;
	struct cxip_ux_send *ux_send;

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		/* TODO Handle append errors. */
		assert(cxi_event_rc(event) == C_RC_OK);

		ofi_atomic_inc32(&rxc->sink_le_linked);
		return FI_SUCCESS;
	case C_EVENT_UNLINK:
		/* TODO Handle append errors. */
		assert(cxi_event_rc(event) == C_RC_OK);

		/* Long sink buffer was manually unlinked. */
		ofi_atomic_dec32(&rxc->sink_le_linked);

		/* Clean up overflow buffers */
		cxip_cq_req_free(req);
		return FI_SUCCESS;
	case C_EVENT_PUT:
		/* Put event handling is complicated. Handle below. */
		break;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
		return FI_SUCCESS;
	}

	/* Handle Put events */
	fastlock_acquire(&rxc->rx_lock);

	/* Check for a previously received Put Overflow event */
	ux_recv = match_ux_recv(rxc, event);
	if (!ux_recv) {
		/* A Put Overflow event is pending. Store a record of this
		 * unexpected Put event for lookup when the event arrives.
		 */

		/* TODO make fast allocator for ux_sends */
		ux_send = malloc_f(sizeof(struct cxip_ux_send));
		if (!ux_send) {
			CXIP_LOG_ERROR("Failed to malloc ux_send\n");
			goto err_put;
		}

		/* Use start pointer for matching. */
		ux_send->req = req;
		init_ux_send(ux_send, event);

		dlist_insert_tail(&ux_send->ux_entry, &rxc->ux_sends);

		fastlock_release(&rxc->rx_lock);

		CXIP_LOG_DBG("Queued ux_send: %p\n", ux_send);

		return FI_SUCCESS;
	}

	CXIP_LOG_DBG("Matched ux_recv, data: 0x%lx\n",
		     ux_recv->recv.oflow_start);

	/* src_offset must come from a Put event. */
	ux_recv->recv.src_offset = event->tgt_long.remote_offset;

	/* For long eager messages, issue a Get to retrieve data
	 * from the initiator.
	 */

	ret = issue_rdzv_get(ux_recv);
	if (ret != FI_SUCCESS)
		goto err_put;

	dlist_remove(&ux_recv->recv.rxc_entry);

	fastlock_release(&rxc->rx_lock);

	CXIP_LOG_ERROR("Overflow beat Put event: %p\n", req);

	return FI_SUCCESS;

err_put:
	fastlock_release(&rxc->rx_lock);

	return -FI_EAGAIN;
}

/*
 * cxip_oflow_rdzv_cb() - Progress an Overflow buffer rendezvous event.
 *
 * All target events which are related to a offloaded rendezvous Put operation
 * have the rendezvous bit set. Handle all rendezvous events from an Overflow
 * buffer. See cxip_oflow_cb() for more details about Overflow buffer event
 * handling.
 */
static int
cxip_oflow_rdzv_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->oflow.rxc;
	struct cxip_oflow_buf *oflow_buf = req->oflow.oflow_buf;
	struct cxip_req *ux_recv;
	struct cxip_ux_send *ux_send;
	void *oflow_va;
	size_t oflow_bytes;

	if (event->hdr.event_type != C_EVENT_PUT) {
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
		return FI_SUCCESS;
	}

	/* Handle Put events */
	fastlock_acquire(&rxc->rx_lock);

	/* Check for a previously received Put Overflow event */
	ux_recv = match_ux_recv(rxc, event);
	if (!ux_recv) {
		/* A Put Overflow event is pending. Store a record of this
		 * unexpected Put event for lookup when the event arrives.
		 */

		/* TODO make fast allocator for ux_sends */
		ux_send = malloc_f(sizeof(struct cxip_ux_send));
		if (!ux_send) {
			CXIP_LOG_ERROR("Failed to malloc ux_send\n");
			goto err_put;
		}

		ux_send->req = req;
		init_ux_send(ux_send, event);

		dlist_insert_tail(&ux_send->ux_entry, &rxc->ux_rdzv_sends);

		fastlock_release(&rxc->rx_lock);

		CXIP_LOG_DBG("Queued ux_send: %p\n", ux_send);

		return FI_SUCCESS;
	}

	CXIP_LOG_DBG("Matched ux_recv, data: 0x%lx\n",
		     ux_recv->recv.oflow_start);

	/* A matching Put Overflow event arrived earlier. Data is
	 * waiting in the overflow buffer.
	 */

	oflow_va = (void *)CXI_IOVA_TO_VA(oflow_buf->md->md,
					  event->tgt_long.start);
	oflow_bytes = MIN(event->tgt_long.mlength, ux_recv->data_len);
	memcpy(ux_recv->recv.recv_buf, oflow_va, oflow_bytes);
	oflow_req_put_bytes(req, event->tgt_long.mlength);

	dlist_remove(&ux_recv->recv.rxc_entry);

	fastlock_release(&rxc->rx_lock);

	CXIP_LOG_ERROR("Overflow beat Put event: %p\n", ux_recv);

	return FI_SUCCESS;

err_put:
	fastlock_release(&rxc->rx_lock);

	return -FI_EAGAIN;
}

int cxip_rxc_eager_replenish(struct cxip_rxc *rxc);

/*
 * cxip_oflow_cb() - Process an Overflow buffer event.
 *
 * Overflow buffers are used to land unexpected Send data. Link, Unlink and Put
 * events are expected from Overflow buffers.
 *
 * A Link event indicates that a new buffer has been appended to the Overflow
 * list.
 *
 * An Unlink event indicates that buffer space was exhausted. Overflow buffers
 * are configured to use locally managed LEs. When enough Puts match in an
 * Overflow buffer, consuming its space, the NIC automatically unlinks the LE.
 * An automatic Unlink event is generated before the final Put which caused
 * buffer space to become exhausted.
 *
 * An Unlink may also be generated by an Unlink command. Overflow buffers are
 * manually unlinked in this way during teardown. When an LE is manually
 * unlinked the auto_unlinked field in the corresponding event is zero. In this
 * case, the request is freed immediately.
 *
 * A Put event is generated for each Put that matches the Overflow buffer LE.
 * This event indicates that data is available in the Overflow buffer. This
 * event must be correlated to a Put Overflow event from a user receive buffer
 * LE. The Put Overflow event may arrive before or after the Put event.
 *
 * When each Put event arrives, check for the existence of a previously posted
 * receive buffer which generated a matching Put Overflow event. If such a
 * buffer exists, copy data from the Overflow buffer to the user receive
 * buffer. Otherwise, store a record of the Put event for matching once a user
 * posts a new buffer that matches the unexpected Put.
 *
 * If data will remain in the Overflow buffer, take a reference to it to
 * prevent it from being freed. If a sequence of Unlink-Put events is detected,
 * drop a reference to the Overflow buffer so it is automatically freed once
 * all user data is copied out.
 */
static int cxip_oflow_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->oflow.rxc;
	struct cxip_oflow_buf *oflow_buf = req->oflow.oflow_buf;
	struct cxip_req *ux_recv;
	struct cxip_ux_send *ux_send;
	void *oflow_va;
	union cxip_match_bits mb;
	int ret;

	if (event->tgt_long.rendezvous)
		return cxip_oflow_rdzv_cb(req, event);

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		/* TODO Handle append errors. */
		assert(cxi_event_rc(event) == C_RC_OK);

		CXIP_LOG_DBG("Eager buffer linked: %p\n", req);
		ofi_atomic_inc32(&rxc->oflow_bufs_linked);
		return FI_SUCCESS;
	case C_EVENT_UNLINK:
		/* TODO Handle append errors. */
		assert(cxi_event_rc(event) == C_RC_OK);

		CXIP_LOG_DBG("Eager buffer unlinked (%s): %p\n",
			     event->tgt_long.auto_unlinked ? "auto" : "manual",
			     req);

		fastlock_acquire(&rxc->rx_lock);

		ofi_atomic_dec32(&rxc->oflow_bufs_submitted);
		ofi_atomic_dec32(&rxc->oflow_bufs_linked);

		if (!event->tgt_long.auto_unlinked) {
			uint64_t bytes = rxc->oflow_buf_size -
					(event->tgt_long.start -
					 CXI_VA_TO_IOVA(oflow_buf->md->md,
							oflow_buf->buf));
			oflow_req_put_bytes(req, bytes);
		} else {
			/* Replace the eager overflow buffer */
			cxip_rxc_eager_replenish(rxc);
		}

		fastlock_release(&rxc->rx_lock);

		return FI_SUCCESS;
	case C_EVENT_PUT:
		/* Put event handling is complicated. Handle below. */
		break;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
		return FI_SUCCESS;
	}

	/* Drop all unexpected 0-byte Put events. */
	if (!event->tgt_long.rlength)
		return FI_SUCCESS;

	/* Handle Put events */
	fastlock_acquire(&rxc->rx_lock);

	/* Check for a previously received Put Overflow event */
	ux_recv = match_ux_recv(rxc, event);
	if (!ux_recv) {
		/* A Put Overflow event is pending. Store a record of this
		 * unexpected Put event for lookup when the event arrives.
		 */

		/* TODO make fast allocator for ux_sends */
		ux_send = malloc_f(sizeof(struct cxip_ux_send));
		if (!ux_send) {
			CXIP_LOG_ERROR("Failed to malloc ux_send\n");
			goto err_put;
		}

		ux_send->req = req;
		init_ux_send(ux_send, event);

		dlist_insert_tail(&ux_send->ux_entry, &rxc->ux_sends);

		fastlock_release(&rxc->rx_lock);

		CXIP_LOG_DBG("Queued ux_send: %p\n", ux_send);

		return FI_SUCCESS;
	}

	/* A matching Put Overflow event arrived earlier. Data is
	 * waiting in the overflow buffer.
	 */

	CXIP_LOG_DBG("Matched ux_recv, data: 0x%lx\n",
		     ux_recv->recv.oflow_start);

	/* Copy data out of overflow buffer. */
	oflow_va = (void *)CXI_IOVA_TO_VA(oflow_buf->md->md,
					  event->tgt_long.start);
	memcpy(ux_recv->recv.recv_buf, oflow_va, ux_recv->data_len);
	oflow_req_put_bytes(req, event->tgt_long.mlength);

	dlist_remove(&ux_recv->recv.rxc_entry);

	fastlock_release(&rxc->rx_lock);

	CXIP_LOG_ERROR("Overflow beat Put event: %p\n", ux_recv);

	/* Check if the initiator requires match completion guarantees.
	 * If so, notify the initiator that the match is now complete.
	 * Delay the Receive event until the notification is complete.
	 * This only applies to unexpected eager messages.
	 */
	mb.raw = event->tgt_long.match_bits;
	if (mb.match_comp) {
		ret = cxip_notify_match(ux_recv, event);
		if (ret != FI_SUCCESS)
			return -FI_EAGAIN;

		return FI_SUCCESS;
	}

	recv_req_report(ux_recv);

	if (ux_recv->recv.multi_recv)
		cxip_cq_req_free(ux_recv);
	else
		recv_req_complete(ux_recv);

	return FI_SUCCESS;

err_put:
	fastlock_release(&rxc->rx_lock);

	return -FI_EAGAIN;
}

/*
 * eager_buf_add() - Append a Locally Managed LE to the Overflow list to match
 * eager Sends.
 *
 * Caller must hold rxc->rx_lock.
 */
static int eager_buf_add(struct cxip_rxc *rxc)
{
	struct cxip_domain *dom;
	int ret;
	struct cxip_oflow_buf *oflow_buf;
	struct cxip_req *req;
	uint32_t le_flags;

	/* Match all eager, long sends */
	union cxip_match_bits mb = {
		.le_type = CXIP_LE_TYPE_RX
	};
	union cxip_match_bits ib = {
		.tag = ~0,
		.tx_id = ~0,
		.tagged = 1,
		.match_comp = 1,
	};

	dom = rxc->domain;

	/* Create an overflow buffer structure */
	oflow_buf = calloc(1, sizeof(*oflow_buf));
	if (!oflow_buf) {
		CXIP_LOG_ERROR("Unable to allocate oflow buffer structure\n");
		return -FI_ENOMEM;
	}

	/* Allocate overflow data buffer */
	oflow_buf->buf = calloc(1, rxc->oflow_buf_size);
	if (!oflow_buf->buf) {
		CXIP_LOG_ERROR("Unable to allocate oflow buffer\n");
		ret = -FI_ENOMEM;
		goto free_oflow;
	}

	/* Map overflow data buffer */
	ret = cxip_map(dom, (void *)oflow_buf->buf, rxc->oflow_buf_size,
		       &oflow_buf->md);
	if (ret) {
		CXIP_LOG_DBG("Failed to map oflow buffer: %d\n", ret);
		goto free_buf;
	}

	/* Populate request */
	req = cxip_cq_req_alloc(rxc->recv_cq, 1, NULL);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate request\n");
		ret = -FI_ENOMEM;
		goto oflow_unmap;
	}

	req->cb = cxip_oflow_cb;
	req->oflow.rxc = rxc;
	req->oflow.oflow_buf = oflow_buf;

	le_flags = C_LE_MANAGE_LOCAL | C_LE_NO_TRUNCATE |
		   C_LE_UNRESTRICTED_BODY_RO | C_LE_UNRESTRICTED_END_RO |
		   C_LE_OP_PUT;

	/* Issue Append command */
	ret = cxip_pte_append(rxc->rx_pte,
			      CXI_VA_TO_IOVA(oflow_buf->md->md,
					     oflow_buf->buf),
			      rxc->oflow_buf_size, oflow_buf->md->md->lac,
			      C_PTL_LIST_OVERFLOW, req->req_id, mb.raw, ib.raw,
			      CXI_MATCH_ID_ANY, rxc->rdzv_threshold, le_flags,
			      NULL, rxc->rx_cmdq, true);
	if (ret) {
		CXIP_LOG_DBG("Failed to write Append command: %d\n", ret);
		goto oflow_req_free;
	}

	/* Initialize oflow_buf structure */
	dlist_insert_tail(&oflow_buf->list, &rxc->oflow_bufs);
	oflow_buf->rxc = rxc;
	oflow_buf->min_bytes = rxc->oflow_buf_size - rxc->rdzv_threshold;
	oflow_buf->buffer_id = req->req_id;
	oflow_buf->type = CXIP_LE_TYPE_RX;

	ofi_atomic_inc32(&rxc->oflow_bufs_submitted);
	ofi_atomic_inc32(&rxc->oflow_bufs_in_use);
	CXIP_LOG_DBG("Eager buffer created: %p\n", req);

	return FI_SUCCESS;

oflow_req_free:
	cxip_cq_req_free(req);
oflow_unmap:
	cxip_unmap(oflow_buf->md);
free_buf:
	free(oflow_buf->buf);
free_oflow:
	free(oflow_buf);

	return ret;
}

/*
 * cxip_rxc_eager_replenish() - Replenish RXC eager overflow buffers.
 *
 * Caller must hold rxc->rx_lock.
 */
int cxip_rxc_eager_replenish(struct cxip_rxc *rxc)
{
	int ret = FI_SUCCESS;

	while (ofi_atomic_get32(&rxc->oflow_bufs_submitted) <
	       rxc->oflow_bufs_max) {
		ret = eager_buf_add(rxc);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_ERROR("Failed to append oflow buffer: %d\n",
				       ret);
			break;
		}
	}

	return ret;
}

/*
 * cxip_rxc_eager_fini() - Free RXC eager overflow buffers.
 */
static int cxip_rxc_eager_fini(struct cxip_rxc *rxc)
{
	int ret = FI_SUCCESS;
	struct cxip_oflow_buf *oflow_buf;

	/* Manually unlink each overflow buffer */
	dlist_foreach_container(&rxc->oflow_bufs, struct cxip_oflow_buf,
				oflow_buf, list) {
		ret = cxip_pte_unlink(rxc->rx_pte, C_PTL_LIST_OVERFLOW,
				      oflow_buf->buffer_id, rxc->rx_cmdq);
		if (ret != FI_SUCCESS) {
			/* TODO handle error */
			CXIP_LOG_ERROR("Failed to enqueue Unlink: %d\n",
				       ret);
			break;
		}
	}

	return ret;
}

/*
 * cxip_rxc_sink_init() - Initialize RXC sink buffer.
 *
 * The sink buffer is used for matching long eager sends in the Overflow list.
 * The sink buffer matches all long eager sends that do not match in the
 * priority list and truncates all data. The sink buffer is not used with the
 * off-loaded rendezvous protocol.
 */
static int cxip_rxc_sink_init(struct cxip_rxc *rxc)
{
	int ret;
	struct cxip_req *req;
	uint32_t le_flags;

	/* Match all eager, long sends */
	union cxip_match_bits mb = {
		.le_type = CXIP_LE_TYPE_SINK,
	};
	union cxip_match_bits ib = {
		.tag = ~0,
		.tx_id = ~0,
		.tagged = 1,
		.match_comp = 1,
	};

	/* Populate request */
	req = cxip_cq_req_alloc(rxc->recv_cq, 1, NULL);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate ux request\n");
		return -FI_ENOMEM;
	}

	le_flags = C_LE_MANAGE_LOCAL | C_LE_UNRESTRICTED_BODY_RO |
		   C_LE_UNRESTRICTED_END_RO | C_LE_OP_PUT;

	ret = cxip_pte_append(rxc->rx_pte, 0, 0, 0,
			      C_PTL_LIST_OVERFLOW, req->req_id, mb.raw, ib.raw,
			      CXI_MATCH_ID_ANY, 0,  le_flags, NULL,
			      rxc->rx_cmdq, true);
	if (ret) {
		CXIP_LOG_DBG("Failed to write UX Append command: %d\n", ret);
		goto req_free;
	}

	/* Initialize oflow_buf structure */
	rxc->sink_le.type = CXIP_LE_TYPE_SINK;
	rxc->sink_le.rxc = rxc;
	rxc->sink_le.buffer_id = req->req_id;

	req->oflow.rxc = rxc;
	req->oflow.oflow_buf = &rxc->sink_le;
	req->cb = cxip_oflow_sink_cb;

	return FI_SUCCESS;

req_free:
	cxip_cq_req_free(req);

	return ret;
}

/*
 * cxip_rxc_sink_fini() - Tear down RXC sink buffer.
 */
static int cxip_rxc_sink_fini(struct cxip_rxc *rxc)
{
	int ret;

	ret = cxip_pte_unlink(rxc->rx_pte, C_PTL_LIST_OVERFLOW,
			      rxc->sink_le.buffer_id, rxc->rx_cmdq);
	if (ret) {
		/* TODO handle error */
		CXIP_LOG_ERROR("Failed to enqueue Unlink: %d\n", ret);
	}

	return ret;
}

static void report_send_completion(struct cxip_req *req, bool sw_cntr);

/*
 * cxip_zbp_cb() - Process zero-byte Put events.
 *
 * Zero-byte Puts (ZBP) are used to transfer small messages without consuming
 * buffers outside of the EQ. ZBPs are currently only used for match complete
 * messages.
 */
static int
cxip_zbp_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_txc *txc = req->oflow.txc;
	struct cxip_req *put_req;
	union cxip_match_bits mb;
	int event_rc;

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		/* TODO Handle append errors. */
		assert(cxi_event_rc(event) == C_RC_OK);

		ofi_atomic_inc32(&txc->zbp_le_linked);
		return FI_SUCCESS;
	case C_EVENT_UNLINK:
		/* TODO Handle append errors. */
		assert(cxi_event_rc(event) == C_RC_OK);

		/* Zero-byte Put LE was manually unlinked. */
		ofi_atomic_dec32(&txc->zbp_le_linked);

		/* Clean up overflow buffers */
		cxip_cq_req_free(req);
		return FI_SUCCESS;
	case C_EVENT_PUT:
		mb.raw = event->tgt_long.match_bits;
		put_req = cxip_tx_id_lookup(txc->ep_obj, mb.tx_id);
		if (!put_req) {
			CXIP_LOG_ERROR("Failed to find TX ID: %d\n",
					mb.tx_id);
			return FI_SUCCESS;
		}

		event_rc = cxi_tgt_event_rc(event);
		if (event_rc != C_RC_OK)
			CXIP_LOG_ERROR("ZBP error: %p rc: %s\n",
				       put_req, cxi_rc_to_str(event_rc));
		else
			CXIP_LOG_DBG("ZBP received: %p rc: %s\n",
				     put_req, cxi_rc_to_str(event_rc));

		cxip_tx_id_free(txc->ep_obj, mb.tx_id);

		/* The unexpected message has been matched. Generate a
		 * completion event. The ZBP event is guaranteed to arrive
		 * after the eager Send Ack, so the transfer is always done at
		 * this point.
		 *
		 * If MATCH_COMPLETE was requested, software must manage
		 * counters.
		 */
		report_send_completion(put_req, true);

		cxip_send_req_dequeue(put_req->send.txc, put_req);

		ofi_atomic_dec32(&put_req->send.txc->otx_reqs);
		cxip_cq_req_free(put_req);

		return FI_SUCCESS;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
		return FI_SUCCESS;
	}
}

/*
 * cxip_txc_zbp_init() - Initialize zero-byte Put LE.
 */
int cxip_txc_zbp_init(struct cxip_txc *txc)
{
	int ret;
	struct cxip_req *req;
	uint32_t le_flags;
	union cxip_match_bits mb = {
		.le_type = CXIP_LE_TYPE_ZBP,
	};
	union cxip_match_bits ib = {
		.tag = ~0,
		.tx_id = ~0,
		.tagged = 1,
		.match_comp = 1,
	};

	/* Populate request */
	req = cxip_cq_req_alloc(txc->send_cq, 1, NULL);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate request\n");
		return -FI_ENOMEM;
	}

	le_flags = C_LE_UNRESTRICTED_BODY_RO | C_LE_UNRESTRICTED_END_RO |
		   C_LE_OP_PUT;

	ret = cxip_pte_append(txc->rdzv_pte, 0, 0, 0,
			      C_PTL_LIST_PRIORITY, req->req_id, mb.raw, ib.raw,
			      CXI_MATCH_ID_ANY, 0, le_flags, NULL,
			      txc->rx_cmdq, true);
	if (ret) {
		CXIP_LOG_DBG("Failed to write Append command: %d\n", ret);
		goto req_free;
	}

	/* Initialize oflow_buf structure */
	txc->zbp_le.type = CXIP_LE_TYPE_ZBP;
	txc->zbp_le.txc = txc;
	txc->zbp_le.buffer_id = req->req_id;

	req->oflow.txc = txc;
	req->oflow.oflow_buf = &txc->zbp_le;
	req->cb = cxip_zbp_cb;

	/* Wait for link */
	do {
		sched_yield();
		cxip_cq_progress(txc->send_cq);
	} while (!ofi_atomic_get32(&txc->zbp_le_linked));

	CXIP_LOG_DBG("ZBP LE linked: %p\n", txc);

	return FI_SUCCESS;

req_free:
	cxip_cq_req_free(req);

	return ret;
}

/*
 * cxip_txc_zbp_fini() - Tear down zero-byte Put LE.
 */
int cxip_txc_zbp_fini(struct cxip_txc *txc)
{
	int ret;

	ret = cxip_pte_unlink(txc->rdzv_pte, C_PTL_LIST_PRIORITY,
			      txc->zbp_le.buffer_id, txc->rx_cmdq);
	if (ret) {
		/* TODO handle error */
		CXIP_LOG_ERROR("Failed to enqueue Unlink: %d\n", ret);
	}

	/* Wait for unlink */
	do {
		sched_yield();
		cxip_cq_progress(txc->send_cq);
	} while (ofi_atomic_get32(&txc->zbp_le_linked));

	CXIP_LOG_DBG("ZBP LE unlinked: %p\n", txc);

	return ret;
}

/*
 * cxip_rxc_oflow_init() - Initialize overflow buffers used for messaging.
 *
 * Must be called with the RX PtlTE disabled.
 */
int cxip_rxc_oflow_init(struct cxip_rxc *rxc)
{
	int ret;

	ret = cxip_rxc_eager_replenish(rxc);
	if (ret) {
		CXIP_LOG_ERROR("cxip_rxc_eager_replenish failed: %d\n", ret);
		return ret;
	}

	ret = cxip_rxc_sink_init(rxc);
	if (ret) {
		CXIP_LOG_ERROR("cxip_rxc_sink_init failed: %d\n", ret);
		cxip_rxc_eager_fini(rxc);
		return ret;
	}

	/* Wait for Overflow buffers to be linked. */
	do {
		sched_yield();
		cxip_cq_progress(rxc->recv_cq);
	} while (ofi_atomic_get32(&rxc->oflow_bufs_linked) <
		 rxc->oflow_bufs_max ||
		 !ofi_atomic_get32(&rxc->sink_le_linked));

	return FI_SUCCESS;
}

/*
 * cxip_rxc_oflow_fini() - Finalize overflow buffers used for messaging.
 *
 * Must be called with the RX PtlTE disabled.
 */
void cxip_rxc_oflow_fini(struct cxip_rxc *rxc)
{
	int ret;
	struct cxip_ux_send *ux_send;
	struct dlist_entry *tmp;
	int ux_sends = 0;

	/* Clean up unexpected Put records. The PtlTE is disabled, so no more
	 * events can be expected.
	 */
	dlist_foreach_container_safe(&rxc->ux_sends, struct cxip_ux_send,
				     ux_send, ux_entry, tmp) {
		/* Dropping the last reference will cause the oflow_buf to be
		 * removed from the RXC list and freed.
		 */
		if (ux_send->req->oflow.oflow_buf->type == CXIP_LE_TYPE_RX)
			oflow_req_put_bytes(ux_send->req, ux_send->mlen);

		dlist_remove(&ux_send->ux_entry);
		free(ux_send);
		ux_sends++;
	}

	if (ux_sends)
		CXIP_LOG_DBG("Freed %d UX Send(s)\n", ux_sends);

	ret = cxip_rxc_sink_fini(rxc);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_ERROR("cxip_rxc_sink_fini() returned: %d\n", ret);
		return;
	}

	ret = cxip_rxc_eager_fini(rxc);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_ERROR("cxip_rxc_eager_fini() returned: %d\n", ret);
		return;
	}

	/* Wait for all overflow buffers to be unlinked */
	do {
		sched_yield();
		cxip_cq_progress(rxc->recv_cq);
	} while (ofi_atomic_get32(&rxc->oflow_bufs_linked) ||
		 ofi_atomic_get32(&rxc->sink_le_linked));

	if (ofi_atomic_get32(&rxc->oflow_bufs_in_use))
		CXIP_LOG_ERROR("Leaked %d overflow buffers\n",
			       ofi_atomic_get32(&rxc->oflow_bufs_in_use));
}

/*
 * mrecv_req_oflow_event() - Set start and length uniquely for an unexpected
 * mrecv request.
 *
 * Overflow buffer events contain a start address representing the offset into
 * the Overflow buffer where data was written. When a unexpected header is
 * later matched to a multi-receive buffer in the priority list, The Put
 * Overflow event does not contain the offset into the Priority list buffer
 * where data should be copied. Software must track the the Priority list
 * buffer offset using ordered Put Overflow events.
 */
static void
mrecv_req_oflow_event(struct cxip_req *req, uint32_t rlen)
{
	struct cxip_req *parent = req->recv.parent;
	uintptr_t rtail;
	uintptr_t mrecv_tail;

	req->recv.recv_buf = (uint8_t *)parent->recv.recv_buf +
			parent->recv.start_offset;
	req->buf = (uint64_t)req->recv.recv_buf;

	rtail = req->buf + rlen;
	mrecv_tail = (uint64_t)parent->recv.recv_buf + parent->recv.ulen;

	req->data_len = rlen;
	if (rtail > mrecv_tail)
		req->data_len -= rtail - mrecv_tail;

	parent->recv.start_offset += req->data_len;
}

/*
 * cxip_recv_rdzv_cb() - Progress rendezvous receive events.
 *
 * Handle rendezvous target events. All target events which are related to an
 * offloaded rendezvous Put operation have the rendezvous field set.
 *
 * Note that Reply events that were generated from a SW-issued Get will not
 * have the rendezvous bit set.
 *
 * There is some complexity in how the receive buffer start pointer (for
 * multi-receives) and receive length are set when using the rendezvous
 * protocol. The method for calculating these for each scenario is below.
 *
 * Expected Receives:
 *	Calculate receive length using Rendezvous event. It needs to be
 *	available for SW issued Gets.
 *
 * Unexpected Receives:
 *	Calculate receive length using Put Overflow event. It needs to be
 *	available for copying eager data into the user buffer. Note that
 *	receive length is set twice for a UX receive using both Rendezvous and
 *	Put Overflow events.
 *
 * Expected Multi-Receives:
 *	Use start, mlength and rlength in the Rendezvous event.
 *
 * Unexpected Multi-Receives:
 *	Track user buffer offset in software using the order of Put Overflow
 *	events.
 */
static int cxip_recv_rdzv_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_ux_send *ux_send;
	struct cxip_oflow_buf *oflow_buf;
	void *oflow_va;
	size_t oflow_bytes;

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		assert(cxi_tgt_event_rc(event) == C_RC_NO_SPACE);

		cxip_recv_req_dropped(req);
		return FI_SUCCESS;
	case C_EVENT_UNLINK:
		/* TODO Handle unlink errors. */
		assert(cxi_event_rc(event) == C_RC_OK);
		return FI_SUCCESS;
	case C_EVENT_SEND:
		/* TODO Handle Send event errors. */
		assert(cxi_event_rc(event) == C_RC_OK);
		return FI_SUCCESS;
	case C_EVENT_PUT_OVERFLOW:
		/* We matched an unexpected header */

		fastlock_acquire(&rxc->rx_lock);

		/* Check for a previously received unexpected Put event */
		ux_send = match_ux_send(rxc, event);
		if (!ux_send) {
			/* An unexpected Put event is pending.  Link this
			 * request to the pending list for lookup when the
			 * event arrives.
			 */
			if (req->recv.multi_recv) {
				req = rdzv_mrecv_req_event(req, event);
				if (!req) {
					fastlock_release(&rxc->rx_lock);
					return -FI_EAGAIN;
				}

				/* Set start and length uniquely for an
				 * unexpected mrecv request.
				 */
				mrecv_req_oflow_event(req,
						event->tgt_long.rlength);

				/* The Child request is placed on the
				 * ux_rdzv_recvs list. Don't look up a child
				 * request when the Put event arrives.
				 */
			} else {
				recv_req_tgt_event(req, event);

				req->data_len = event->tgt_long.rlength;
				if (req->data_len > req->recv.ulen)
					req->data_len = req->recv.ulen;
			}
			cxip_recv_req_dequeue_nolock(req);
			dlist_insert_tail(&req->recv.rxc_entry,
					  &rxc->ux_rdzv_recvs);

			CXIP_LOG_DBG("Queued recv req, data: 0x%lx\n",
				     req->recv.oflow_start);

			fastlock_release(&rxc->rx_lock);

			/* Count the rendezvous event. */
			rdzv_recv_req_event(req);

			return FI_SUCCESS;
		}

		CXIP_LOG_DBG("Matched ux_send: %p\n", ux_send);

		if (req->recv.multi_recv) {
			req = rdzv_mrecv_req_event(req, event);
			if (!req) {
				fastlock_release(&rxc->rx_lock);
				return -FI_EAGAIN;
			}

			/* Set start and length uniquely for an unexpected
			 * mrecv request.
			 */
			mrecv_req_oflow_event(req,
					event->tgt_long.rlength);
		} else {
			recv_req_tgt_event(req, event);

			req->data_len = event->tgt_long.rlength;
			if (req->data_len > req->recv.ulen)
				req->data_len = req->recv.ulen;
		}

		oflow_buf = ux_send->req->oflow.oflow_buf;

		/* Copy data out of overflow buffer. */
		oflow_va = (void *)CXI_IOVA_TO_VA(oflow_buf->md->md,
						  ux_send->start);
		oflow_bytes = MIN(event->tgt_long.mlength, req->data_len);
		memcpy(req->recv.recv_buf, oflow_va, oflow_bytes);
		oflow_req_put_bytes(ux_send->req, event->tgt_long.mlength);

		dlist_remove(&ux_send->ux_entry);
		free(ux_send);

		cxip_recv_req_dequeue_nolock(req);

		fastlock_release(&rxc->rx_lock);

		/* Count the rendezvous event. */
		rdzv_recv_req_event(req);

		return FI_SUCCESS;
	case C_EVENT_PUT:
		/* Eager data was delivered directly to the user buffer. */
		cxip_recv_req_dequeue(req);

		if (req->recv.multi_recv) {
			req = rdzv_mrecv_req_event(req, event);
			if (!req)
				return -FI_EAGAIN;

			/* Set start pointer and data_len using Rendezvous or
			 * Put Overflow event (depending on if message was
			 * unexpected).
			 */
		} else {
			recv_req_tgt_event(req, event);
		}

		/* Count the rendezvous event. */
		rdzv_recv_req_event(req);
		return FI_SUCCESS;
	case C_EVENT_RENDEZVOUS:
		if (req->recv.multi_recv) {
			req = rdzv_mrecv_req_event(req, event);
			if (!req)
				return -FI_EAGAIN;

			/* Use Rendezvous event to set start pointer and
			 * data_len for expected Sends.
			 */
			struct cxip_req *parent = req->recv.parent;
			uintptr_t rtail;
			uintptr_t mrecv_tail;

			req->buf = CXI_IOVA_TO_VA(
					parent->recv.recv_md->md,
					event->tgt_long.start) -
					event->tgt_long.mlength;
			req->recv.recv_buf = (void *)req->buf;
			rtail = req->buf + event->tgt_long.rlength;
			mrecv_tail = (uint64_t)parent->recv.recv_buf +
				parent->recv.ulen;

			req->data_len = event->tgt_long.rlength;
			if (rtail > mrecv_tail)
				req->data_len -= rtail - mrecv_tail;
		} else {
			recv_req_tgt_event(req, event);

			req->data_len = event->tgt_long.rlength;
			if (req->data_len > req->recv.ulen)
				req->data_len = req->recv.ulen;
		}

		if (!event->tgt_long.get_issued) {
			int ret = issue_rdzv_get(req);
			if (ret != FI_SUCCESS) {
				/* Undo multi-recv event processing. */
				if (req->recv.multi_recv &&
				    !req->recv.rdzv_events) {
					dlist_remove(&req->recv.children);
					cxip_cq_req_free(req);
				}
				return -FI_EAGAIN;
			}

			CXIP_LOG_DBG("Software issued Get, req: %p\n", req);
		}

		/* Count the rendezvous event. */
		rdzv_recv_req_event(req);
		return FI_SUCCESS;
	case C_EVENT_REPLY:
		/* If mrecv, look up the correct child request. */
		if (req->recv.multi_recv) {
			req = rdzv_mrecv_req_event(req, event);
			if (!req)
				return -FI_EAGAIN;
		}

		/* Rendezvous Get completed. Complete the request. */
		req->recv.rc = cxi_init_event_rc(event);

		/* Count the rendezvous event. */
		rdzv_recv_req_event(req);
		return FI_SUCCESS;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
	}

	return FI_SUCCESS;
}

/*
 * cxip_recv_cb() - Process a user receive buffer event.
 *
 * A user receive buffer is described by an LE linked to the Priority list.
 * Link, Unlink, Put, Put Overflow, and Reply events are expected from a user
 * receive buffer.
 *
 * A Link event indicates that a new user buffer has been linked to the
 * priority list. Successful Link events may be suppressed.
 *
 * An Unlink event indicates that a user buffer has been unlinked. Normally, a
 * receive is used once and unlinked when it is matched with a Send. In this
 * case, a successful Unlink event may be suppressed.
 *
 * For expected, eager Sends, a Put will be matched to a user receive buffer by
 * the NIC. Send data is copied directly to the user buffer. A Put event is
 * generated describing the match.
 *
 * For unexpected, eager Sends, a Put will first match a buffer in the Overflow
 * list. See cxip_oflow_cb() for details on Overflow event handling. Once a
 * matching user receive buffer is appended to the Priority list, a Put
 * Overflow event is generated. Put and Put Overflow events for an unexpected,
 * eager Send must be correlated. These events may arrive in any order. Once
 * both events are accounted, data is copied from the Overflow buffer to the
 * user receive buffer.
 *
 * Unexpected, eager Sends that are longer than the eager threshold have their
 * data truncated to zero. This is to avoid long messages consuming too much
 * Overflow buffer space at the target. Once a match is made with a user
 * receive buffer, data is re-read from the initiator using a Get.
 *
 * Rendezvous receive events are handled by cxip_recv_rdzv_cb().
 */
static int cxip_recv_cb(struct cxip_req *req, const union c_event *event)
{
	int ret;
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_ux_send *ux_send;
	struct cxip_oflow_buf *oflow_buf;
	void *oflow_va;
	bool rdzv = false;
	union cxip_match_bits mb;

	/* All events related to an offloaded rendezvous receive will be
	 * handled by cxip_recv_rdzv_cb(). Those events are identified by the
	 * event rendezvous field. One exception is a Reply event generated
	 * from a SW-issued Get. When such an event is generated, the request
	 * will have already processed a Rendezvous event. If the rendezvous
	 * field is not set, but the rdzv_events count is elevated, this must
	 * be a SW-issued Reply event.
	 */
	if (event->hdr.event_type == C_EVENT_REPLY)
		rdzv = (event->init_short.rendezvous || req->recv.rdzv_events);
	else
		rdzv = event->tgt_long.rendezvous;

	if (rdzv)
		return cxip_recv_rdzv_cb(req, event);

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		assert(cxi_tgt_event_rc(event) == C_RC_NO_SPACE);

		cxip_recv_req_dropped(req);
		return FI_SUCCESS;
	case C_EVENT_UNLINK:
		if (!event->tgt_long.auto_unlinked) {
			req->recv.rc = C_RC_CANCELED;
			recv_req_report(req);
			recv_req_complete(req);
		} else {
			assert(cxi_event_rc(event) == C_RC_OK);
		}
		return FI_SUCCESS;
	case C_EVENT_SEND:
		/* TODO Handle Send event errors. */
		assert(cxi_event_rc(event) == C_RC_OK);
		return FI_SUCCESS;
	case C_EVENT_PUT_OVERFLOW:
		/* We matched an unexpected header */

		fastlock_acquire(&rxc->rx_lock);

		/* Unexpected 0-byte Put events are dropped. Skip matching. */
		if (!event->tgt_long.rlength) {
			recv_req_tgt_event(req, event);
			req->data_len = 0;
			goto zbp;
		}

		/* Check for a previously received unexpected Put event */
		ux_send = match_ux_send(rxc, event);
		if (!ux_send) {
			/* An unexpected Put event is pending. Link this
			 * request to the pending list for lookup when the
			 * event arrives.
			 */
			if (req->recv.multi_recv) {
				req = mrecv_req_dup(req);
				if (!req) {
					fastlock_release(&rxc->rx_lock);
					return -FI_EAGAIN;
				}
				recv_req_tgt_event(req, event);

				/* Set start and length uniquely for an
				 * unexpected mrecv request.
				 */
				mrecv_req_oflow_event(req,
						event->tgt_long.rlength);

				/* The Child request is placed on the ux_recvs
				 * list. Don't look up a child request when the
				 * Put event arrives.
				 */
			} else {
				recv_req_tgt_event(req, event);

				req->data_len = event->tgt_long.rlength;
				if (req->data_len > req->recv.ulen)
					req->data_len = req->recv.ulen;
			}
			cxip_recv_req_dequeue_nolock(req);
			dlist_insert_tail(&req->recv.rxc_entry, &rxc->ux_recvs);

			CXIP_LOG_DBG("Queued recv req, data: 0x%lx\n",
				     req->recv.oflow_start);

			fastlock_release(&rxc->rx_lock);

			return FI_SUCCESS;
		}

		/* A matching unexpected-Put event arrived earlier. */

		CXIP_LOG_DBG("Matched ux_send: %p\n", ux_send);

		oflow_buf = ux_send->req->oflow.oflow_buf;

		if (req->recv.multi_recv) {
			req = mrecv_req_dup(req);
			if (!req)
				return -FI_EAGAIN;
			recv_req_tgt_event(req, event);

			/* Set start and length uniquely for an unexpected
			 * mrecv request.
			 */
			mrecv_req_oflow_event(req,
					event->tgt_long.rlength);
		} else {
			recv_req_tgt_event(req, event);

			req->data_len = ux_send->rlen;
			if (req->data_len > req->recv.ulen)
				req->data_len = req->recv.ulen;
		}

		if (oflow_buf->type == CXIP_LE_TYPE_SINK) {
			/* src_offset must come from a Put event. */
			req->recv.src_offset = ux_send->src_offset;

			/* For unexpected, long, eager messages, issue a Get to
			 * retrieve data from the initiator.
			 */
			ret = issue_rdzv_get(req);
			if (ret == FI_SUCCESS) {
				dlist_remove(&ux_send->ux_entry);
				free(ux_send);

				CXIP_LOG_DBG("Issued Get, req: %p\n", req);
			} else if (req->recv.multi_recv) {
				/* Undo multi-recv event processing. */
				req->recv.parent->recv.start_offset -=
						req->data_len;
				cxip_cq_req_free(req);
			}

			cxip_recv_req_dequeue_nolock(req);
			fastlock_release(&rxc->rx_lock);
			return ret;
		}

		/* Copy data out of overflow buffer. */
		oflow_va = (void *)CXI_IOVA_TO_VA(oflow_buf->md->md,
				event->tgt_long.start);
		memcpy(req->recv.recv_buf, oflow_va, req->data_len);
		oflow_req_put_bytes(ux_send->req, event->tgt_long.mlength);

		dlist_remove(&ux_send->ux_entry);
		free(ux_send);

zbp:
		mb.raw = event->tgt_long.match_bits;
		if (!mb.match_comp)
			cxip_recv_req_dequeue_nolock(req);

		fastlock_release(&rxc->rx_lock);

		/* Check if the initiator requires match completion guarantees.
		 * If so, notify the initiator that the match is now complete.
		 * Delay the Receive event until the notification is complete.
		 */
		if (mb.match_comp) {
			ret = cxip_notify_match(req, event);
			if (ret != FI_SUCCESS) {
				if (req->recv.multi_recv)
					cxip_cq_req_free(req);

				return -FI_EAGAIN;
			}

			cxip_recv_req_dequeue(req);
			return FI_SUCCESS;
		}

		recv_req_report(req);

		if (req->recv.multi_recv)
			cxip_cq_req_free(req);
		else
			recv_req_complete(req);

		return FI_SUCCESS;
	case C_EVENT_PUT:
		/* Data was delivered directly to the user buffer. Complete the
		 * request.
		 */
		cxip_recv_req_dequeue(req);

		if (req->recv.multi_recv) {
			req = mrecv_req_dup(req);
			if (!req)
				return -FI_EAGAIN;
			recv_req_tgt_event(req, event);

			req->buf = (uint64_t)(CXI_IOVA_TO_VA(
					req->recv.recv_md->md,
					event->tgt_long.start));
			req->data_len = event->tgt_long.mlength;

			recv_req_report(req);
			cxip_cq_req_free(req);
		} else {
			req->data_len = event->tgt_long.mlength;
			recv_req_tgt_event(req, event);
			recv_req_report(req);
			recv_req_complete(req);
		}
		return FI_SUCCESS;
	case C_EVENT_REPLY:
		/* Long-send Get completed. Complete the request. */
		req->recv.rc = cxi_init_event_rc(event);

		if (req->recv.multi_recv) {
			recv_req_report(req);
			cxip_cq_req_free(req);
		} else {
			/* Complete receive request. */
			recv_req_report(req);
			recv_req_complete(req);
		}
		return FI_SUCCESS;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
	}

	return FI_SUCCESS;
}

/*
 * cxip_recv_cancel() - Cancel outstanding receive request.
 */
int cxip_recv_cancel(struct cxip_req *req)
{
	int ret;
	struct cxip_rxc *rxc = req->recv.rxc;

	ret = cxip_pte_unlink(rxc->rx_pte, C_PTL_LIST_PRIORITY,
			      req->req_id, rxc->rx_cmdq);
	if (ret == FI_SUCCESS)
		req->recv.canceled = true;

	return ret;
}

/*
 * cxip_recv_reenable() - Attempt to re-enable the RX queue.
 *
 * Called by disabled EP ready to re-enable.
 *
 * Determine if the RX queue can be re-enabled and perform a state change
 * command if necessary. The Endpoint must receive dropped Send notifications
 * from all peers who experienced drops before re-enabling the RX queue.
 *
 * Caller must hold rxc->lock.
 */
int cxip_recv_reenable(struct cxip_rxc *rxc)
{
	int total_drops = -1;
	struct cxi_pte_status pte_status = {};
	struct cxip_fc_drops *fc_drops;
	int ret;

	if (rxc->pte_state == C_PTLTE_ENABLED || rxc->enable_pending)
		return FI_SUCCESS;

	/* Check if we're ready to re-enable the RX queue */
	dlist_foreach_container(&rxc->fc_drops, struct cxip_fc_drops,
				fc_drops, rxc_entry) {
		total_drops += fc_drops->drops;
	}

	ret = cxil_pte_status(rxc->rx_pte->pte, &pte_status);
	assert(!ret);

	CXIP_LOG_DBG("Processed %d/%d drops\n",
		     total_drops+1, pte_status.drop_count+1);

	if (total_drops != pte_status.drop_count)
		return -FI_EAGAIN;

	CXIP_LOG_DBG("Re-enabling PTE\n");

	ret = cxip_rxc_msg_enable(rxc, total_drops);
	assert(ret == FI_SUCCESS);

	return FI_SUCCESS;
}

/*
 * cxip_fc_resume_cb() - Process FC resume completion events.
 */
int cxip_fc_resume_cb(struct cxip_ctrl_req *req, const union c_event *event)
{
	struct cxip_fc_drops *fc_drops = container_of(req,
			struct cxip_fc_drops, req);

	switch (event->hdr.event_type) {
	case C_EVENT_ACK:
		/* TODO handle error. Drops can happen. */
		assert(cxi_event_rc(event) == C_RC_OK);

		free(fc_drops);
		break;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
	}

	return FI_SUCCESS;
}

/*
 * cxip_fc_process_drops() - Process a dropped Send notification from a peer.
 *
 * Called by disabled EP waiting to re-enable.
 *
 * When a peer detects dropped Sends it follows up by sending a message to the
 * disabled Endpoint indicating the number of drops experienced. The disabled
 * Endpoint peer must count all drops before re-enabling its RX queue.
 */
int cxip_fc_process_drops(struct cxip_ep_obj *ep_obj, uint8_t rxc_id,
			  uint32_t nic_addr, uint32_t pid, uint8_t txc_id,
			  uint16_t drops)
{
	struct cxip_rxc *rxc = ep_obj->rxcs[rxc_id];
	struct cxip_fc_drops *fc_drops;
	int ret;

	fc_drops = calloc(1, sizeof(*fc_drops));
	if (!fc_drops) {
		CXIP_LOG_DBG("Failed to allocate drops\n");
		return -FI_ENOMEM;
	}

	fc_drops->rxc = rxc;
	fc_drops->nic_addr = nic_addr;
	fc_drops->pid = pid;
	fc_drops->txc_id = txc_id;
	fc_drops->rxc_id = rxc_id;
	fc_drops->drops = drops;

	fc_drops->req.send.nic_addr = nic_addr;
	fc_drops->req.send.pid = pid;
	fc_drops->req.send.mb.txc_id = txc_id;
	fc_drops->req.send.mb.rxc_id = rxc_id;
	fc_drops->req.send.mb.drops = drops;

	fc_drops->req.send.mb.ctrl_le_type = CXIP_CTRL_LE_TYPE_CTRL_MSG;
	fc_drops->req.send.mb.ctrl_msg_type = CXIP_CTRL_MSG_FC_RESUME;
	fc_drops->req.cb = cxip_fc_resume_cb;
	fc_drops->req.ep_obj = rxc->ep_obj;

	fastlock_acquire(&rxc->lock);

	dlist_insert_tail(&fc_drops->rxc_entry, &rxc->fc_drops);

	CXIP_LOG_DBG("Processed drops: %d NIC: %#x TXC: %d RXC: %p\n",
		     drops, nic_addr, txc_id, rxc);

	ret = cxip_recv_reenable(rxc);
	assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);

	fastlock_release(&rxc->lock);

	return FI_SUCCESS;
}

/*
 * cxip_recv_replay() - Replay dropped Receive requests.
 *
 * When no LE is available while processing an Append command, the command is
 * dropped and future appends are disabled. After all outstanding commands are
 * dropped and resources are recovered, replayed all Receive requests in order.
 *
 * Caller must hold rxc->lock.
 */
static int cxip_recv_replay(struct cxip_rxc *rxc)
{
	struct cxip_req *req;
	struct dlist_entry *tmp;
	bool restart_seq = true;
	int ret;

	/* Wait until all outstanding Receives complete before replaying. */
	if (!dlist_empty(&rxc->msg_queue))
		return -FI_EAGAIN;

	rxc->append_disabled = false;

	dlist_foreach_container_safe(&rxc->replay_queue,
				     struct cxip_req, req,
				     recv.rxc_entry, tmp) {
		dlist_remove(&req->recv.rxc_entry);

		CXIP_LOG_DBG("Replaying: %p\n", req);

		ret = cxip_recv_req_queue(req);

		/* Match made in software? */
		if (ret == -FI_EALREADY)
			continue;

		assert(ret == FI_SUCCESS);

		ret = _cxip_recv_req(req, restart_seq);
		assert(ret == FI_SUCCESS);

		restart_seq = false;
	}

	return FI_SUCCESS;
}

/*
 * cxip_recv_resume() - Send a resume message to all peers who reported dropped
 * Sends.
 *
 * Called by disabled EP after re-enable.
 *
 * After counting all dropped sends targeting a disabled RX queue and
 * re-enabling the queue, notify all peers who experienced dropped Sends so
 * they can be replayed.
 *
 * Caller must hold rxc->lock.
 */
int cxip_recv_resume(struct cxip_rxc *rxc)
{
	struct cxip_fc_drops *fc_drops;
	struct dlist_entry *tmp;
	int ret;

	dlist_foreach_container_safe(&rxc->fc_drops,
				     struct cxip_fc_drops, fc_drops,
				     rxc_entry, tmp) {
		ret = cxip_ctrl_msg_send(&fc_drops->req);
		assert(ret == FI_SUCCESS);

		dlist_remove(&fc_drops->rxc_entry);
	}

	return FI_SUCCESS;
}

/*
 * cxip_ux_onload_cb() - Process SEARCH_AND_DELETE command events.
 */
static int cxip_ux_onload_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_rxc *rxc = req->search.rxc;
	struct cxip_ux_send *ux_send;
	int ret;

	fastlock_acquire(&rxc->lock);

	switch (event->hdr.event_type) {
	case C_EVENT_PUT_OVERFLOW:
		assert(cxi_event_rc(event) == C_RC_OK);
		assert(rxc->searches_pending);

		ux_send = match_ux_send(rxc, event);
		if (!ux_send) {
			CXIP_LOG_ERROR("Matching Put event not found\n");
			abort();
		}
		dlist_remove(&ux_send->ux_entry);

		dlist_insert_tail(&ux_send->ux_entry, &rxc->sw_ux_list);
		rxc->sw_ux_list_len++;

		CXIP_LOG_DBG("Onloaded ux_send: %p\n", ux_send);

		break;
	case C_EVENT_SEARCH:
		assert(rxc->searches_pending);

		CXIP_LOG_DBG("UX list empty (sw_ux_list_len: %d): %p\n",
			     rxc->sw_ux_list_len, rxc);

		ofi_atomic_dec32(&rxc->orx_reqs);
		cxip_cq_req_free(req);
		rxc->searches_pending--;

		/* Check if the RX queue can be re-enabled now */
		ret = cxip_recv_reenable(rxc);
		assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);

		ret = cxip_recv_replay(rxc);
		assert(ret == FI_SUCCESS || ret == -FI_EAGAIN);

		break;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
	}

	fastlock_release(&rxc->lock);

	return FI_SUCCESS;
}

/*
 * cxip_ux_onload() - Issue SEARCH_AND_DELETE command to on-load unexpected
 * Send headers queued on the RXC message queue.
 *
 * Caller must hold rxc->lock.
 */
static int cxip_ux_onload(struct cxip_rxc *rxc)
{
	struct cxip_req *req;
	union c_cmdu cmd = {};
	int ret;

	/* Populate request */
	req = cxip_cq_req_alloc(rxc->recv_cq, 1, NULL);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate request\n");
		ret = -FI_ENOMEM;
		return ret;
	}
	ofi_atomic_inc32(&rxc->orx_reqs);

	req->cb = cxip_ux_onload_cb;
	req->search.rxc = rxc;

	cmd.command.opcode = C_CMD_TGT_SEARCH_AND_DELETE;
	cmd.target.ptl_list = C_PTL_LIST_UNEXPECTED;
	cmd.target.ptlte_index = rxc->rx_pte->pte->ptn;
	cmd.target.buffer_id = req->req_id;
	cmd.target.length = -1U;
	cmd.target.ignore_bits = -1UL;
	cmd.target.match_id = CXI_MATCH_ID_ANY;

	fastlock_acquire(&rxc->rx_cmdq->lock);

	ret = cxi_cq_emit_target(rxc->rx_cmdq->dev_cmdq, &cmd);
	if (ret) {
		CXIP_LOG_DBG("Failed to write Search command: %d\n", ret);

		ofi_atomic_dec32(&rxc->orx_reqs);
		cxip_cq_req_free(req);

		fastlock_release(&rxc->rx_cmdq->lock);

		return -FI_EAGAIN;
	}

	cxi_cq_ring(rxc->rx_cmdq->dev_cmdq);

	fastlock_release(&rxc->rx_cmdq->lock);

	rxc->searches_pending++;

	return FI_SUCCESS;
}

/*
 * cxip_recv_pte_cb() - Process receive PTE state change events.
 */
void cxip_recv_pte_cb(struct cxip_pte *pte, enum c_ptlte_state state)
{
	struct cxip_rxc *rxc = (struct cxip_rxc *)pte->ctx;
	int ret;

	fastlock_acquire(&rxc->lock);

	switch (state) {
	case C_PTLTE_ENABLED:
		assert(rxc->pte_state == C_PTLTE_DISABLED);
		assert(rxc->enable_pending);

		rxc->enable_pending = false;
		rxc->pte_state = C_PTLTE_ENABLED;

		CXIP_LOG_DBG("Enabled Receive PTE: %p\n", rxc);

		ret = cxip_recv_resume(rxc);
		assert(ret == FI_SUCCESS);

		break;
	case C_PTLTE_DISABLED:
		assert(rxc->pte_state == C_PTLTE_ENABLED);

		rxc->pte_state = C_PTLTE_DISABLED;

		if (!rxc->disabling) {
			CXIP_LOG_DBG("Flow control detected: %p\n", rxc);

			ret = cxip_ux_onload(rxc);
			assert(ret == FI_SUCCESS);
		}

		break;
	default:
		CXIP_LOG_ERROR("Unexpected state received: %u\n", state);
	}

	fastlock_release(&rxc->lock);
}

#if 1
/*
 * tag_match() - Compare UX Send tag and Receive tags in SW.
 */
static bool tag_match(uint64_t init_mb, uint64_t mb, uint64_t ib)
{
	return !((init_mb ^ mb) & ~ib);
}

/*
 * tag_match() - Compare UX Send initiator and Receive initiator in SW.
 */
static bool init_match(uint32_t init, uint32_t match_id)
{
	if (match_id == CXI_MATCH_ID_ANY)
		return true;

	return init == match_id;
}

/*
 * recv_req_sw_match() - Fill Receive request fields for a SW match
 *
 * Substitute for recv_req_tgt_event() during SW matches.
 */
static void recv_req_sw_match(struct cxip_req *req,
			      struct cxip_ux_send *ux_send)
{
	req->recv.rlen = ux_send->rlen;
	req->recv.rc = C_RC_OK;
	req->tag = ux_send->mb.tag;
	req->data = ux_send->data;
	req->recv.src_offset = ux_send->src_offset;
	req->recv.initiator = ux_send->initiator;
	req->recv.rdzv_lac = ux_send->rdzv_lac;
	req->recv.rdzv_id = ux_send->rdzv_id;
	req->recv.rdzv_mlen = ux_send->mlen;
}

/*
 * cxip_recv_sw_match() - Progress the SW Receive match.
 *
 * Progress the operation which matched in SW.
 */
static int cxip_recv_sw_match(struct cxip_req *req,
			      struct cxip_ux_send *ux_send)
{
	struct cxip_oflow_buf *oflow_buf;
	void *oflow_va;
	int ret;

	oflow_buf = ux_send->req->oflow.oflow_buf;

	if (req->recv.multi_recv) {
		req = mrecv_req_dup(req);
		if (!req)
			return -FI_EAGAIN;

		recv_req_sw_match(req, ux_send);

		/* Set start and length uniquely for an unexpected
		 * mrecv request.
		 */
		mrecv_req_oflow_event(req, ux_send->rlen);
	} else {
		recv_req_sw_match(req, ux_send);

		req->data_len = ux_send->rlen;
		if (req->data_len > req->recv.ulen)
			req->data_len = req->recv.ulen;
	}

	/* TODO support long Send SW matching */
	assert(ux_send->rlen < req->recv.rxc->rdzv_threshold);

	oflow_va = (void *)CXI_IOVA_TO_VA(oflow_buf->md->md,
					  ux_send->start);
	memcpy(req->recv.recv_buf, oflow_va, req->data_len);
	oflow_req_put_bytes(ux_send->req, ux_send->mlen);

	CXIP_LOG_DBG("Software match, req: %p ux_send: %p (sw_ux_list_len: %u)\n",
		     req, ux_send, req->recv.rxc->sw_ux_list_len);

	recv_req_report(req);

	ret = FI_SUCCESS;
	if (req->recv.multi_recv) {
		if ((req->recv.mrecv_bytes - req->data_len) >=
				req->recv.rxc->min_multi_recv)
			ret = -FI_EINPROGRESS;

		cxip_cq_req_free(req);
	} else {
		recv_req_complete(req);
	}

	return ret;
}

/*
 * cxip_recv_sw_matcher() - Attempt to match the Receive in SW.
 *
 * Loop through all onloaded UX Sends looking for a match for the Receive
 * request. If a match is found, progress the operation.
 *
 * Caller must hold req->recv.rxc->lock.
 */
static int cxip_recv_sw_matcher(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	struct cxip_ux_send *ux_send;
	struct dlist_entry *tmp;
	int ret = FI_ENOMSG;

	if (dlist_empty(&rxc->sw_ux_list))
		return -FI_ENOMSG;

	dlist_foreach_container_safe(&rxc->sw_ux_list, struct cxip_ux_send,
				     ux_send, ux_entry, tmp) {
		if (req->recv.tagged) {
			if (!ux_send->mb.tagged)
				continue;

			if (!tag_match(ux_send->mb.tag, req->recv.tag,
				       req->recv.ignore))
				continue;

			if (!init_match(ux_send->initiator, req->recv.match_id))
				continue;
		} else {
			if (ux_send->mb.tagged)
				continue;

			if (!init_match(ux_send->initiator, req->recv.match_id))
				continue;
		}

		ret = cxip_recv_sw_match(req, ux_send);
		if (ret == -FI_EAGAIN) {
			/* Couldn't process match, try again */
			break;
		}

		dlist_remove(&ux_send->ux_entry);
		free(ux_send);
		rxc->sw_ux_list_len--;

		if (ret == -FI_EINPROGRESS) {
			/* Multi-recv, keep matching */
			ret = -FI_ENOMSG;
			continue;
		}

		ret = FI_SUCCESS;
		break;
	}

	return ret;
}

/*
 * cxip_recv_req_dropped() - Mark the Received request dropped.
 *
 * If HW does not have sufficient LEs to perform an append, the command is
 * dropped. Queue the request for replay. When all outstanding append commands
 * complete, replay all Receives.
 */
static int cxip_recv_req_dropped(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	int ret;

	fastlock_acquire(&rxc->lock);

	rxc->append_disabled = true;

	dlist_remove(&req->recv.rxc_entry);
	dlist_insert_tail(&req->recv.rxc_entry, &rxc->replay_queue);

	CXIP_LOG_DBG("Receive dropped: %p\n", req);

	if (dlist_empty(&rxc->msg_queue)) {
		ret = cxip_ux_onload(rxc);
		assert(ret == FI_SUCCESS);
	}

	fastlock_release(&rxc->lock);

	return FI_SUCCESS;
}

/*
 * cxip_recv_req_queue() - Queue Receive request on RXC.
 *
 * Place the Receive request in an ordered SW queue. There are a couple
 * temporary conditions that prevent queuing new Receives.
 *
 * 1. Appends have been disabled by HW. Previous Receives must be replayed
 *    first.
 * 2. SW is in the process of onloading UX Sends.
 *
 * Before appending a new Receive request to a HW list, attempt to match the
 * Receive to any onloaded UX Sends.
 */
static int cxip_recv_req_queue(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	int ret;

	/* Try to match against onloaded Sends first. */
	ret = cxip_recv_sw_matcher(req);

	if (ret == FI_SUCCESS) {
		ret = -FI_EALREADY;
		return ret;
	}

	if (ret != -FI_ENOMSG)
		return ret;

	/* Don't accept new Receives while there are message that need to be
	 * replayed.
	 */
	if (rxc->append_disabled)
		return -FI_EAGAIN;

	/* Matching can't be performed while in the intermediate onloading
	 * state. Wait until software completes building the UX Send list.
	 */
	if (rxc->searches_pending)
		return -FI_EAGAIN;

	dlist_insert_tail(&req->recv.rxc_entry, &rxc->msg_queue);

	return FI_SUCCESS;
}

/*
 * cxip_recv_req_dequeue_nolock() - Dequeue Receive request from RXC.
 *
 * Caller must hold req->recv.rxc->lock.
 */
static void cxip_recv_req_dequeue_nolock(struct cxip_req *req)
{
	dlist_remove(&req->recv.rxc_entry);
}

/*
 * cxip_recv_req_dequeue() - Dequeue Receive request from RXC.
 *
 * A Receive request may be dequeued from the RXC as soon as there is evidence
 * that the append command has been accepted.
 */
static void cxip_recv_req_dequeue(struct cxip_req *req)
{
	struct cxip_rxc *rxc = req->recv.rxc;

	fastlock_acquire(&rxc->lock);

	cxip_recv_req_dequeue_nolock(req);

	fastlock_release(&rxc->lock);
}
#else
static int cxip_recv_req_dropped(struct cxip_req *req) {return FI_SUCCESS;}
static int cxip_recv_req_queue(struct cxip_req *req)  {return FI_SUCCESS;}
static void cxip_recv_req_dequeue_nolock(struct cxip_req *req) {}
static void cxip_recv_req_dequeue(struct cxip_req *req) {}
#endif

/*
 * _cxip_recv_req() - Submit Receive request to hardware.
 */
static ssize_t _cxip_recv_req(struct cxip_req *req, bool restart_seq)
{
	struct cxip_rxc *rxc = req->recv.rxc;
	uint32_t le_flags;
	union cxip_match_bits mb = {};
	union cxip_match_bits ib = {
		.tx_id = ~0,
		.match_comp = 1,
		.le_type = ~0,
	};
	int ret;
	struct cxip_md *recv_md = req->recv.recv_md;
	uint64_t recv_iova = 0;

	if (req->recv.tagged) {
		mb.tagged = 1;
		mb.tag = req->recv.tag;
		ib.tag = req->recv.ignore;
	}

	/* Always set manage_local in Receive LEs. This makes Cassini ignore
	 * initiator remote_offset in all Puts. With this, remote_offset in Put
	 * events can be used by the initiator for protocol data. The behavior
	 * of use_once is not impacted by manage_local.
	 */
	le_flags = C_LE_EVENT_LINK_DISABLE | C_LE_EVENT_UNLINK_DISABLE |
		   C_LE_MANAGE_LOCAL |
		   C_LE_UNRESTRICTED_BODY_RO | C_LE_UNRESTRICTED_END_RO |
		   C_LE_OP_PUT;
	if (!req->recv.multi_recv)
		le_flags |= C_LE_USE_ONCE;
	if (restart_seq)
		le_flags |= C_LE_RESTART_SEQ;

	if (recv_md)
		recv_iova = CXI_VA_TO_IOVA(recv_md->md,
					   (uint64_t)req->recv.recv_buf +
					   req->recv.start_offset);

	/* Issue Append command */
	ret = cxip_pte_append(rxc->rx_pte, recv_iova,
			      req->recv.ulen - req->recv.start_offset,
			      recv_md ? recv_md->md->lac : 0,
			      C_PTL_LIST_PRIORITY, req->req_id,
			      mb.raw, ib.raw, req->recv.match_id,
			      rxc->min_multi_recv,
			      le_flags, NULL, rxc->rx_cmdq,
			      !(req->recv.flags & FI_MORE));
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("Failed to write Append command: %d\n", ret);
		return ret;
	}

	return FI_SUCCESS;
}

/*
 * _cxip_recv() - Common message receive function. Used for tagged and untagged
 * sends of all sizes.
 */
static ssize_t _cxip_recv(struct cxip_rxc *rxc, void *buf, size_t len,
			  void *desc, fi_addr_t src_addr, uint64_t tag,
			  uint64_t ignore, void *context, uint64_t flags,
			  bool tagged)
{
	struct cxip_domain *dom = rxc->domain;
	int ret;
	struct cxip_md *recv_md = NULL;
	struct cxip_req *req;
	struct cxip_addr caddr;
	uint32_t match_id;
	uint32_t pid_bits;

	if (len && !buf)
		return -FI_EINVAL;

	if (!rxc->enabled)
		return -FI_EOPBADSTATE;

	if (!ofi_recv_allowed(rxc->attr.caps))
		return -FI_ENOPROTOOPT;

	/* If FI_DIRECTED_RECV and a src_addr is specified, encode the address
	 * in the LE for matching. If application AVs are symmetric, use
	 * logical FI address for matching. Otherwise, use physical address.
	 */
	pid_bits = dom->iface->dev->info.pid_bits;
	if (rxc->attr.caps & FI_DIRECTED_RECV &&
	    src_addr != FI_ADDR_UNSPEC) {
		if (rxc->ep_obj->av->attr.flags & FI_SYMMETRIC) {
			match_id = CXI_MATCH_ID(pid_bits, caddr.pid, src_addr);
		} else {
			ret = _cxip_av_lookup(rxc->ep_obj->av, src_addr,
					      &caddr);
			if (ret != FI_SUCCESS) {
				CXIP_LOG_DBG("Failed to look up FI addr: %d\n",
					     ret);
				return -FI_EINVAL;
			}

			match_id = CXI_MATCH_ID(pid_bits, caddr.pid, caddr.nic);
		}
	} else {
		match_id = CXI_MATCH_ID_ANY;
	}

	/* Map local buffer */
	if (len) {
		ret = cxip_map(dom, (void *)buf, len, &recv_md);
		if (ret) {
			CXIP_LOG_DBG("Failed to map recv buffer: %d\n", ret);
			return ret;
		}
	}

	/* Populate request */
	req = cxip_cq_req_alloc(rxc->recv_cq, 1, rxc);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate request\n");
		ret = -FI_ENOMEM;
		goto recv_unmap;
	}
	ofi_atomic_inc32(&rxc->orx_reqs);

	/* req->data_len, req->tag, req->data must be set later. req->buf may
	 * be overwritten later.
	 */
	req->context = (uint64_t)context;

	req->flags = FI_RECV | (flags & FI_COMPLETION);
	if (tagged)
		req->flags |= FI_TAGGED;
	else
		req->flags |= FI_MSG;

	req->buf = 0;
	req->cb = cxip_recv_cb;

	req->recv.rxc = rxc;
	req->recv.recv_buf = buf;
	req->recv.recv_md = recv_md;
	req->recv.ulen = len;
	req->recv.match_id = match_id;
	req->recv.tag = tag & CXIP_TAG_MASK;
	req->recv.ignore = ignore & CXIP_TAG_MASK;
	req->recv.flags = flags;
	req->recv.tagged = tagged;
	req->recv.start_offset = 0;
	req->recv.multi_recv = (flags & FI_MULTI_RECV ? true : false);
	req->recv.mrecv_bytes = len;
	req->recv.parent = NULL;
	dlist_init(&req->recv.children);

	/* Count Put, Rendezvous, and Reply events during offloaded RPut. */
	req->recv.rdzv_events = 0;

	fastlock_acquire(&rxc->lock);
	ret = cxip_recv_req_queue(req);
	fastlock_release(&rxc->lock);

	/* Match made in software? */
	if (ret == -FI_EALREADY)
		return FI_SUCCESS;

	/* RXC busy (onloading Sends or full CQ)? */
	if (ret != FI_SUCCESS)
		goto req_free;

	ret = _cxip_recv_req(req, false);
	if (ret != FI_SUCCESS)
		goto req_dequeue;

	CXIP_LOG_DBG("req: %p buf: %p len: %lu src_addr: %ld tag(%c): 0x%lx ignore: 0x%lx context: %p\n",
		     req, buf, len, src_addr, tagged ? '*' : '-', tag, ignore,
		     context);

	return FI_SUCCESS;

req_dequeue:
	cxip_recv_req_dequeue(req);
req_free:
	ofi_atomic_dec32(&rxc->orx_reqs);
	cxip_cq_req_free(req);
recv_unmap:
	if (recv_md)
		cxip_unmap(recv_md);

	return ret;
}

/*
 * cxip_txc_fi_addr() - Return the FI address of the TXC.
 */
static fi_addr_t _txc_fi_addr(struct cxip_txc *txc)
{
	if (txc->ep_obj->fi_addr == FI_ADDR_NOTAVAIL) {
		txc->ep_obj->fi_addr =
				_cxip_av_reverse_lookup(
						txc->ep_obj->av,
						txc->ep_obj->src_addr.nic,
						txc->ep_obj->src_addr.pid);
		CXIP_LOG_DBG("Found EP FI Addr: %lu\n", txc->ep_obj->fi_addr);
	}

	return txc->ep_obj->fi_addr;
}

/*
 * cxip_msg_match_id() - Return the TXC's initiator address used to transmit a
 * message.
 *
 * By default, the physical address of the TXC is returned. This address is
 * sent along with message data and is used for source address matching at the
 * target. When the target receives a message, the physical ID is translated to
 * a logical FI address. Translation adds overhead to the receive path.
 *
 * As an optimization, if rendezvous offload is not being used and the process
 * is part of a job with symmetric AVs, a logical FI address is returned. This
 * way, there is no source address translation overhead involved in the
 * receive.
 */
static uint32_t cxip_msg_match_id(struct cxip_txc *txc)
{
	int pid_bits = txc->domain->iface->dev->info.pid_bits;

	if (txc->ep_obj->av->attr.flags & FI_SYMMETRIC)
		return CXI_MATCH_ID(pid_bits, txc->ep_obj->src_addr.pid,
				    _txc_fi_addr(txc));

	return CXI_MATCH_ID(pid_bits, txc->ep_obj->src_addr.pid,
			    txc->ep_obj->src_addr.nic);
}

/*
 * report_send_completion() - Report the completion of a send operation.
 */
static void report_send_completion(struct cxip_req *req, bool sw_cntr)
{
	int ret;
	int success_event = (req->flags & FI_COMPLETION);

	req->flags &= (FI_MSG | FI_TAGGED | FI_SEND);

	if (req->send.rc == C_RC_OK) {
		CXIP_LOG_DBG("Request success: %p\n", req);

		if (success_event) {
			ret = cxip_cq_req_complete(req);
			if (ret != FI_SUCCESS)
				CXIP_LOG_ERROR("Failed to report completion: %d\n",
					       ret);
		}

		if (sw_cntr && req->send.txc->send_cntr) {
			ret = cxip_cntr_mod(req->send.txc->send_cntr, 1, false,
					    false);
			if (ret)
				CXIP_LOG_ERROR("cxip_cntr_mod returned: %d\n",
					       ret);
		}
	} else {
		CXIP_LOG_ERROR("Request error: %p (err: %d, %s)\n",
			       req, FI_EIO, cxi_rc_to_str(req->send.rc));

		ret = cxip_cq_req_error(req, 0, FI_EIO, req->send.rc, NULL, 0);
		if (ret != FI_SUCCESS)
			CXIP_LOG_ERROR("Failed to report error: %d\n", ret);

		if (sw_cntr && req->send.txc->send_cntr) {
			ret = cxip_cntr_mod(req->send.txc->send_cntr, 1, false,
					    true);
			if (ret)
				CXIP_LOG_ERROR("cxip_cntr_mod returned: %d\n",
					       ret);
		}
	}
}

/*
 * rdzv_send_req_complete() - Complete long send request.
 */
static void rdzv_send_req_complete(struct cxip_req *req)
{
	cxip_rdzv_id_free(req->send.txc->ep_obj, req->send.rdzv_id);

	cxip_unmap(req->send.send_md);

	report_send_completion(req, true);

	ofi_atomic_dec32(&req->send.txc->otx_reqs);
	cxip_cq_req_free(req);
}

/*
 * long_send_req_event() - Count a long send event.
 *
 * Call for each initiator event. The events could be generated in any order.
 * Once all expected events are received, complete the request.
 *
 * A successful long Send generates two events: Ack and Get. That applies to
 * both the offloaded rendezvous protocol and long eager protocol.
 *
 * Note: a Get is not used for a long eager Send if the Put matches in the
 * Priority list.
 */
static void long_send_req_event(struct cxip_req *req)
{
	if (++req->send.long_send_events == 2)
		rdzv_send_req_complete(req);
}

/*
 * cxip_send_long_cb() - Long send callback.
 *
 * Progress a long send operation to completion.
 */
static int cxip_send_long_cb(struct cxip_req *req, const union c_event *event)
{
	int event_rc;
	int ret;

	switch (event->hdr.event_type) {
	case C_EVENT_ACK:
		/* The source Put completed. */
		event_rc = cxi_init_event_rc(event);
		if (event_rc == C_RC_OK)
			CXIP_LOG_DBG("Put Acked (%s): %p\n",
				     cxi_ptl_list_to_str(event->init_short.ptl_list),
				     req);
		else
			CXIP_LOG_ERROR("Ack error: %p rc: %s\n",
				       req, cxi_rc_to_str(event_rc));

		/* If the message was dropped, mark the peer as disabled. Do
		 * not generate a completion. Free associated resources. Do not
		 * free the request (it will be used to replay the Send).
		 */
		if (event_rc == C_RC_PT_DISABLED) {
			ret = cxip_send_req_dropped(req->send.txc, req);
			if (ret != FI_SUCCESS)
				ret = -FI_EAGAIN;

			return ret;
		}

		/* Message was accepted by the peer. Match order is preserved.
		 * The request can be dequeued from the SW message queue. This
		 * allows flow-control recovery to be performed before
		 * outstanding long Send operations have completed.
		 */
		cxip_send_req_dequeue(req->send.txc, req);

		/* The transaction is complete if:
		 * 1. The Put failed
		 * 2. Using the eager long protocol and data landed in the
		 *    Priority list
		 */
		if (event_rc != C_RC_OK ||
		    (!req->send.txc->ep_obj->rdzv_offload &&
		     event->init_short.ptl_list == C_PTL_LIST_PRIORITY)) {
			req->send.rc = event_rc;
			rdzv_send_req_complete(req);
		} else {
			/* Count the event, another may be expected. */
			long_send_req_event(req);
		}
		return FI_SUCCESS;
	default:
		CXIP_LOG_ERROR("Unexpected event received: %s\n",
			       cxi_event_to_str(event));
		return FI_SUCCESS;
	}
}

/*
 * rdzv_src_cb() - Process rendezvous source buffer events.
 *
 * A Get event is generated for each rendezvous Send indicating Send completion.
 */
static int rdzv_src_cb(struct cxip_req *req, const union c_event *event)
{
	struct cxip_txc *txc = req->rdzv_src.txc;
	struct cxip_req *get_req;
	union cxip_match_bits mb;
	int event_rc = cxi_tgt_event_rc(event);

	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		if (event_rc != C_RC_OK)
			CXIP_LOG_ERROR("%s error: %p rc: %s\n",
				       cxi_event_to_str(event), req,
				       cxi_rc_to_str(event_rc));
		else
			CXIP_LOG_ERROR("%s received: %p rc: %s\n",
				       cxi_event_to_str(event), req,
				       cxi_rc_to_str(event_rc));

		req->rdzv_src.rc = cxi_tgt_event_rc(event);
		return FI_SUCCESS;
	case C_EVENT_UNLINK:
		dlist_remove(&req->rdzv_src.list);
		cxip_cq_req_free(req);
		ofi_atomic_sub32(&txc->rdzv_src_lacs, 1 << req->rdzv_src.lac);

		CXIP_LOG_DBG("RDZV source window unlinked (LAC: %u)\n",
			     req->rdzv_src.lac);
		return FI_SUCCESS;
	case C_EVENT_GET:
		mb.raw = event->tgt_long.match_bits;
		get_req = cxip_rdzv_id_lookup(txc->ep_obj, mb.rdzv_id_lo);
		if (!get_req) {
			CXIP_LOG_ERROR("Failed to find RDZV ID: %d\n",
					mb.rdzv_id_lo);
			return FI_SUCCESS;
		}

		event_rc = cxi_tgt_event_rc(event);
		if (event_rc != C_RC_OK)
			CXIP_LOG_ERROR("Get error: %p rc: %s\n",
					get_req, cxi_rc_to_str(event_rc));
		else
			CXIP_LOG_DBG("Get received: %p rc: %s\n",
					get_req, cxi_rc_to_str(event_rc));

		get_req->send.rc = event_rc;

		/* Count the event, another may be expected. */
		long_send_req_event(get_req);

		return FI_SUCCESS;
	default:
		CXIP_LOG_ERROR("Unexpected event received: %s\n",
			       cxi_event_to_str(event));
		return FI_SUCCESS;
	}
}

/*
 * cxip_txc_prep_rdzv_src() - Prepare an LAC for use with the rendezvous
 * protocol.
 *
 * Synchronously append an LE describing every address in the specified LAC.
 * Each rendezvous Send uses this LE to access source buffer data.
 */
static int cxip_txc_prep_rdzv_src(struct cxip_txc *txc, unsigned int lac)
{
	int ret;
	struct cxip_req *req;
	uint32_t le_flags;
	union cxip_match_bits mb = {};
	union cxip_match_bits ib = { .raw = ~0 };
	uint32_t lac_mask = 1 << lac;

	if (ofi_atomic_get32(&txc->rdzv_src_lacs) & lac_mask)
		return FI_SUCCESS;

	fastlock_acquire(&txc->rdzv_src_lock);

	if (ofi_atomic_get32(&txc->rdzv_src_lacs) & lac_mask) {
		ret = FI_SUCCESS;
		goto unlock;
	}

	req = cxip_cq_req_alloc(txc->send_cq, 1, NULL);
	if (!req) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	req->cb = rdzv_src_cb;
	req->rdzv_src.txc = txc;
	req->rdzv_src.lac = lac;
	req->rdzv_src.rc = 0;

	mb.rdzv_lac = lac;
	ib.rdzv_lac = 0;

	le_flags = C_LE_UNRESTRICTED_BODY_RO | C_LE_UNRESTRICTED_END_RO |
		   C_LE_OP_GET;

	ret = cxip_pte_append(txc->rdzv_pte, 0, -1ULL, lac,
			      C_PTL_LIST_PRIORITY, req->req_id,
			      mb.raw, ib.raw, CXI_MATCH_ID_ANY, 0,
			      le_flags, NULL, txc->rx_cmdq, true);
	if (ret != FI_SUCCESS) {
		ret = -FI_EAGAIN;
		goto req_free;
	}

	do {
		sched_yield();
		cxip_cq_progress(txc->send_cq);
	} while (!req->rdzv_src.rc);

	if (req->rdzv_src.rc == C_RC_OK) {
		ofi_atomic_add32(&txc->rdzv_src_lacs, lac_mask);
		dlist_insert_tail(&req->rdzv_src.list, &txc->rdzv_src_reqs);

		CXIP_LOG_DBG("RDZV source window linked (LAC: %u)\n", lac);
	} else {
		ret = -FI_EAGAIN;
		goto req_free;
	}

	fastlock_release(&txc->rdzv_src_lock);

	return FI_SUCCESS;

req_free:
	cxip_cq_req_free(req);
unlock:
	fastlock_release(&txc->rdzv_src_lock);

	return ret;
}

/*
 * cxip_txc_rdzv_src_fini() - Unlink rendezvous source LEs.
 */
int cxip_txc_rdzv_src_fini(struct cxip_txc *txc)
{
	struct cxip_req *req;
	int ret;

	dlist_foreach_container(&txc->rdzv_src_reqs, struct cxip_req, req,
				     rdzv_src.list) {
		ret = cxip_pte_unlink(txc->rdzv_pte, C_PTL_LIST_PRIORITY,
				      req->req_id, txc->rx_cmdq);
		if (ret) {
			CXIP_LOG_ERROR("Failed to enqueue Unlink: %d\n", ret);
			return ret;
		}
	}

	/* Wait for unlink events */
	do {
		sched_yield();
		cxip_cq_progress(txc->send_cq);
	} while (ofi_atomic_get32(&txc->rdzv_src_lacs));

	return FI_SUCCESS;
}

/*
 * _cxip_send_long() - Initiate a long send operation.
 *
 * There are two long send protocols implemented: an eager (long) protocol and
 * an offloaded rendezvous protocol.
 *
 * The eager (long) protocol works as follows:
 *
 * 1. The Initiator performs a Put of the entire source buffer.
 * 2. An Ack event is generated indicating the Put completed. The Ack indicates
 *    whether it matched in the Priority or Overflow list at the target.
 * 3a. If the Put matched in the Priority list, the entire payload was copied
 *     directly to a receive buffer at the target. The operation is complete.
 * 3b. If the Put matched in the Overflow list, the payload was truncated to
 *     zero. The Target receives events describing the Put attempt.
 * 4b. The Target performs a Get of the entire source buffer using the source
 *     buffer LE.
 *
 * The rendezvous protocol works as follows:
 *
 * 1. The Initiator performs a Rendezvous Put command which includes a portion
 *    of the source buffer data.
 * 2. Once the Put is matched to a user receive buffer (in the Priority list),
 *    a Get of the remaining source data is performed.
 */
static ssize_t _cxip_send_long(struct cxip_req *req)
{
	struct cxip_txc *txc = req->send.txc;
	struct cxip_domain *dom;
	struct cxip_md *send_md;
	union c_fab_addr dfa;
	uint8_t idx_ext;
	uint32_t pid_bits;
	uint32_t pid_idx;
	struct c_full_dma_cmd cmd = {};
	union cxip_match_bits put_mb = {};
	int rdzv_id;
	int ret;

	if (req->send.flags & FI_INJECT) {
		CXIP_LOG_DBG("Invalid inject\n");
		return -FI_EMSGSIZE;
	}

	dom = txc->domain;

	/* Calculate DFA */
	pid_bits = dom->iface->dev->info.pid_bits;
	pid_idx = CXIP_PTL_IDX_RXC(req->send.rxc_id);
	cxi_build_dfa(req->send.caddr.nic, req->send.caddr.pid, pid_bits,
		      pid_idx, &dfa, &idx_ext);

	/* Map local buffer */
	ret = cxip_map(dom, req->send.buf, req->send.len, &send_md);
	if (ret) {
		CXIP_LOG_DBG("Failed to map send buffer: %d\n", ret);
		return ret;
	}
	req->send.send_md = send_md;

	/* Prepare rendezvous source buffer */
	ret = cxip_txc_prep_rdzv_src(txc, send_md->md->lac);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("Failed to prepare source window: %d\n",
			     ret);
		goto err_unmap;
	}

	/* Allocate rendezvous ID */
	rdzv_id = cxip_rdzv_id_alloc(txc->ep_obj, req);
	if (rdzv_id < 0)
		goto err_unmap;

	/* Build match bits */
	if (req->send.tagged) {
		put_mb.tagged = 1;
		put_mb.tag = req->send.tag;
	}

	req->send.rdzv_id = rdzv_id;
	req->cb = cxip_send_long_cb;
	req->send.long_send_events = 0;

	/* Build Put command descriptor */
	cmd.command.cmd_type = C_CMD_TYPE_DMA;
	cmd.index_ext = idx_ext;
	cmd.lac = send_md->md->lac;
	cmd.event_send_disable = 1;
	cmd.restricted = 0;
	cmd.dfa = dfa;
	cmd.local_addr = CXI_VA_TO_IOVA(send_md->md, req->send.buf);
	cmd.request_len = req->send.len;
	cmd.eq = txc->send_cq->evtq->eqn;
	cmd.user_ptr = (uint64_t)req;
	cmd.initiator = cxip_msg_match_id(txc);
	cmd.header_data = req->send.data;
	cmd.remote_offset = CXI_VA_TO_IOVA(send_md->md, req->send.buf);

	fastlock_acquire(&txc->tx_cmdq->lock);

	if (txc->ep_obj->rdzv_offload) {
		cmd.command.opcode = C_CMD_RENDEZVOUS_PUT;
		cmd.eager_length = txc->rdzv_threshold;
		cmd.use_offset_for_get = 1;

		put_mb.rdzv_lac = send_md->md->lac;
		put_mb.le_type = CXIP_LE_TYPE_RX;
		cmd.match_bits = put_mb.raw;

		/* RPut rdzv ID goes in command */
		cmd.rendezvous_id = rdzv_id;

		ret = cxi_cq_emit_dma(txc->tx_cmdq->dev_cmdq, &cmd);
	} else {
		cmd.command.opcode = C_CMD_PUT;

		/* Match sink buffer */
		put_mb.le_type = CXIP_LE_TYPE_SINK;
		/* Use match bits for rdzv_id */
		put_mb.rdzv_id_hi = rdzv_id;
		cmd.match_bits = put_mb.raw;

		ret = cxi_cq_emit_dma(txc->tx_cmdq->dev_cmdq, &cmd);
	}

	if (ret) {
		CXIP_LOG_ERROR("Failed to enqueue Put: %d\n", ret);
		goto err_unlock;
	}

	if (!(req->send.flags & FI_MORE))
		cxi_cq_ring(txc->tx_cmdq->dev_cmdq);

	fastlock_release(&txc->tx_cmdq->lock);

	return FI_SUCCESS;

err_unlock:
	fastlock_release(&txc->tx_cmdq->lock);
	cxip_rdzv_id_free(txc->ep_obj, rdzv_id);
err_unmap:
	cxip_unmap(send_md);

	return -FI_EAGAIN;
}

/*
 * cxip_send_eager_cb() - Eager send callback. Used for both tagged and
 * untagged messages.
 */
static int cxip_send_eager_cb(struct cxip_req *req,
			      const union c_event *event)
{
	int match_complete = req->flags & FI_MATCH_COMPLETE;
	int ret;

	/* IDCs don't have an MD */
	if (req->send.send_md) {
		cxip_unmap(req->send.send_md);
		req->send.send_md = NULL;
	}

	req->send.rc = cxi_init_event_rc(event);

	/* If MATCH_COMPLETE was requested and the the Put did not match a user
	 * buffer, do not generate a completion event until the target notifies
	 * the initiator that the match is complete.
	 */
	if (match_complete) {
		if (req->send.rc == C_RC_OK &&
		    event->init_short.ptl_list != C_PTL_LIST_PRIORITY) {
			CXIP_LOG_DBG("Waiting for match complete: %p\n", req);
			return FI_SUCCESS;
		}

		CXIP_LOG_DBG("Match complete with Ack: %p\n", req);
		cxip_tx_id_free(req->send.txc->ep_obj, req->send.tx_id);
	}

	/* If the message was dropped, mark the peer as disabled. Do not
	 * generate a completion. Free associated resources. Do not free the
	 * request (it will be used to replay the Send).
	 */
	if (req->send.rc == C_RC_PT_DISABLED) {
		ret = cxip_send_req_dropped(req->send.txc, req);
		if (ret != FI_SUCCESS)
			ret = -FI_EAGAIN;

		return ret;
	}

	/* If MATCH_COMPLETE was requested, software must manage counters. */
	report_send_completion(req, match_complete);

	cxip_send_req_dequeue(req->send.txc, req);

	ofi_atomic_dec32(&req->send.txc->otx_reqs);
	cxip_cq_req_free(req);

	return FI_SUCCESS;
}

/*
 * _cxip_send_eager() - Enqueue eager send command.
 */
static ssize_t _cxip_send_eager(struct cxip_req *req)
{
	struct cxip_txc *txc = req->send.txc;
	struct cxip_domain *dom;
	struct cxip_md *send_md = NULL;
	union c_fab_addr dfa;
	uint8_t idx_ext;
	uint32_t pid_bits;
	uint32_t pid_idx;
	union cxip_match_bits mb = {
		.le_type = CXIP_LE_TYPE_RX
	};
	int idc;
	int ret;
	int match_complete = req->send.flags & FI_MATCH_COMPLETE;
	int tx_id;

	/* Always use IDCs when the payload fits */
	idc = (req->send.len <= CXIP_INJECT_SIZE);

	if ((req->send.flags & FI_INJECT) && !idc) {
		CXIP_LOG_DBG("Invalid inject\n");
		return -FI_EMSGSIZE;
	}

	dom = txc->domain;

	/* Calculate DFA */
	pid_bits = dom->iface->dev->info.pid_bits;
	pid_idx = CXIP_PTL_IDX_RXC(req->send.rxc_id);
	cxi_build_dfa(req->send.caddr.nic, req->send.caddr.pid, pid_bits,
		      pid_idx, &dfa, &idx_ext);

	/* Map local buffer */
	if (!idc) {
		ret = cxip_map(dom, req->send.buf, req->send.len, &send_md);
		if (ret != FI_SUCCESS) {
			CXIP_LOG_DBG("Failed to map send buffer: %d\n", ret);
			return ret;
		}
	}
	req->send.send_md = send_md;

	/* Build match bits */
	if (req->send.tagged) {
		mb.tagged = 1;
		mb.tag = req->send.tag;
	}

	/* Allocate a TX ID if match completion guarantees are required */
	if (match_complete) {
		tx_id = cxip_tx_id_alloc(txc->ep_obj, req);
		if (tx_id < 0) {
			CXIP_LOG_DBG("Failed to allocate TX ID: %d\n", ret);
			goto err_unmap;
		}

		req->send.tx_id = tx_id;

		mb.match_comp = 1;
		mb.tx_id = tx_id;
	}

	req->cb = cxip_send_eager_cb;

	/* Submit command */
	fastlock_acquire(&txc->tx_cmdq->lock);

	if (idc) {
		union c_cmdu cmd = {};

		cmd.c_state.event_send_disable = 1;
		cmd.c_state.index_ext = idx_ext;
		cmd.c_state.eq = txc->send_cq->evtq->eqn;
		cmd.c_state.initiator = cxip_msg_match_id(txc);

		/* If MATCH_COMPLETE was requested, software must manage
		 * counters.
		 */
		if (txc->send_cntr && !match_complete) {
			cmd.c_state.event_ct_ack = 1;
			cmd.c_state.ct = txc->send_cntr->ct->ctn;
		}

		if (memcmp(&txc->tx_cmdq->c_state, &cmd.c_state,
			   sizeof(cmd.c_state))) {
			/* Update TXQ C_STATE */
			txc->tx_cmdq->c_state = cmd.c_state;

			ret = cxi_cq_emit_c_state(txc->tx_cmdq->dev_cmdq,
						  &cmd.c_state);
			if (ret) {
				CXIP_LOG_DBG("Failed to issue C_STATE command: %d\n",
					     ret);

				/* Return error according to Domain Resource
				 * Management
				 */
				ret = -FI_EAGAIN;
				goto err_unlock;
			}

			CXIP_LOG_DBG("Updated C_STATE: %p\n", req);
		}

		memset(&cmd.idc_msg, 0, sizeof(cmd.idc_msg));
		cmd.idc_msg.dfa = dfa;
		cmd.idc_msg.match_bits = mb.raw;
		cmd.idc_msg.header_data = req->send.data;
		cmd.idc_msg.user_ptr = (uint64_t)req;

		ret = cxi_cq_emit_idc_msg(txc->tx_cmdq->dev_cmdq, &cmd.idc_msg,
					  req->send.buf, req->send.len);
		if (ret) {
			CXIP_LOG_DBG("Failed to write IDC: %d\n", ret);

			/* Return error according to Domain Resource Management
			 */
			ret = -FI_EAGAIN;
			goto err_unlock;
		}
	} else {
		struct c_full_dma_cmd cmd = {};

		cmd.command.cmd_type = C_CMD_TYPE_DMA;
		cmd.command.opcode = C_CMD_PUT;
		cmd.index_ext = idx_ext;
		cmd.lac = send_md->md->lac;
		cmd.event_send_disable = 1;
		cmd.restricted = 0;
		cmd.dfa = dfa;
		cmd.remote_offset = 0;
		cmd.local_addr = CXI_VA_TO_IOVA(send_md->md, req->send.buf);
		cmd.request_len = req->send.len;
		cmd.eq = txc->send_cq->evtq->eqn;
		cmd.user_ptr = (uint64_t)req;
		cmd.initiator = cxip_msg_match_id(txc);
		cmd.match_bits = mb.raw;
		cmd.header_data = req->send.data;

		/* If MATCH_COMPLETE was requested, software must manage
		 * counters.
		 */
		if (txc->send_cntr && !match_complete) {
			cmd.event_ct_ack = 1;
			cmd.ct = txc->send_cntr->ct->ctn;
		}

		/* Issue Eager Put command */
		ret = cxi_cq_emit_dma(txc->tx_cmdq->dev_cmdq, &cmd);
		if (ret) {
			CXIP_LOG_DBG("Failed to write DMA command: %d\n", ret);

			/* Return error according to Domain Resource Mgmt */
			ret = -FI_EAGAIN;
			goto err_unlock;
		}
	}

	if (!(req->send.flags & FI_MORE))
		cxi_cq_ring(txc->tx_cmdq->dev_cmdq);

	fastlock_release(&txc->tx_cmdq->lock);

	return FI_SUCCESS;

err_unlock:
	fastlock_release(&txc->tx_cmdq->lock);
	if (match_complete)
		cxip_tx_id_free(txc->ep_obj, req->send.tx_id);
err_unmap:
	if (!idc)
		cxip_unmap(send_md);

	return ret;
}

static ssize_t _cxip_send_req(struct cxip_req *req)
{
	if (req->send.len > req->send.txc->rdzv_threshold)
		return _cxip_send_long(req);
	else
		return _cxip_send_eager(req);
}

/*
 * cxip_fc_peer_lookup() - Check if a peer is disabled.
 *
 * Look up disabled peer state and return it, if available.
 *
 * Caller must hold txc->lock.
 */
static struct cxip_fc_peer *cxip_fc_peer_lookup(struct cxip_txc *txc,
						struct cxip_addr caddr,
						uint8_t rxc_id)
{
	struct cxip_fc_peer *peer;

	dlist_foreach_container(&txc->fc_peers, struct cxip_fc_peer,
				peer, txc_entry) {
		if (CXIP_ADDR_EQUAL(peer->caddr, caddr) &&
		    peer->rxc_id == rxc_id) {
			return peer;
		}
	}

	return NULL;
}

/*
 * cxip_fc_peer_put() - Account for completion of an outstanding Send targeting
 * a disabled peer.
 *
 * Drop a reference to a disabled peer. When the last reference is dropped,
 * attempt flow-control recovery.
 *
 * Caller must hold txc->lock.
 */
static void cxip_fc_peer_put(struct cxip_fc_peer *peer)
{
	int ret;

	/* Account for the completed Send */
	if (!--peer->pending) {
		peer->req.send.mb.drops = peer->dropped;

		ret = cxip_ctrl_msg_send(&peer->req);
		if (ret)
			abort();

		CXIP_LOG_DBG("Notified disabled peer, TXC: %p NIC: %#x PID: %u dropped: %u\n",
			     peer->txc, peer->caddr.nic, peer->caddr.pid,
			     peer->dropped);
	}
}

/*
 * cxip_fc_notify_cb() - Process FC notify completion events.
 */
int cxip_fc_notify_cb(struct cxip_ctrl_req *req, const union c_event *event)
{
	switch (event->hdr.event_type) {
	case C_EVENT_ACK:
		/* TODO handle error. Drops can happen. */
		assert(cxi_event_rc(event) == C_RC_OK);
		break;
	default:
		CXIP_LOG_ERROR("Unexpected event type: %d\n",
			       event->hdr.event_type);
		return FI_SUCCESS;
	}

	return FI_SUCCESS;
}

/*
 * cxip_fc_peer_init() - Mark a peer as disabled.
 *
 * Called by sending EP after experiencing first dropped Send to a peer.
 *
 * Allocate state to track the disabled peer. Locate all outstanding Sends
 * targeting the peer.
 *
 * Caller must hold txc->lock.
 */
static int cxip_fc_peer_init(struct cxip_txc *txc, struct cxip_addr caddr,
			     uint8_t rxc_id, struct cxip_fc_peer **peer)
{
	struct cxip_fc_peer *p;
	struct cxip_req *req;
	struct dlist_entry *tmp;

	p = calloc(1, sizeof(*p));
	if (!p) {
		CXIP_LOG_ERROR("Failed to allocate FC Peer\n");
		return -FI_ENOMEM;
	}

	p->caddr = caddr;
	p->rxc_id = rxc_id;
	p->txc = txc;
	dlist_init(&p->msg_queue);
	dlist_insert_tail(&p->txc_entry, &txc->fc_peers);

	p->req.send.nic_addr = caddr.nic;
	p->req.send.pid = caddr.pid;
	p->req.send.mb.txc_id = txc->tx_id;
	p->req.send.mb.rxc_id = rxc_id;

	p->req.send.mb.ctrl_le_type = CXIP_CTRL_LE_TYPE_CTRL_MSG;
	p->req.send.mb.ctrl_msg_type = CXIP_CTRL_MSG_FC_NOTIFY;
	p->req.cb = cxip_fc_notify_cb;
	p->req.ep_obj = txc->ep_obj;

	/* Queue all Sends to the FC'ed peer */
	dlist_foreach_container_safe(&txc->msg_queue, struct cxip_req,
				     req, send.txc_entry, tmp) {
		if (CXIP_ADDR_EQUAL(req->send.caddr, caddr) &&
		     req->send.rxc_id == rxc_id) {
			dlist_remove(&req->send.txc_entry);
			dlist_insert_tail(&req->send.txc_entry, &p->msg_queue);
			p->pending++;
			req->send.fc_peer = p;
		}
	}

	*peer = p;

	return FI_SUCCESS;
}

/*
 * cxip_fc_peer_fini() - Remove disabled peer state.
 *
 * Caller must hold txc->lock.
 */
static void cxip_fc_peer_fini(struct cxip_fc_peer *peer)
{
	assert(dlist_empty(&peer->msg_queue));
	dlist_remove(&peer->txc_entry);
	free(peer);
}

/*
 * cxip_fc_resume() - Replay dropped Sends.
 *
 * Called by sending EP after being notified disabled peer was re-enabled.
 *
 * Replay all dropped Sends in order.
 */
int cxip_fc_resume(struct cxip_ep_obj *ep_obj, uint8_t txc_id,
		   uint32_t nic_addr, uint32_t pid, uint8_t rxc_id)
{
	struct cxip_txc *txc = ep_obj->txcs[txc_id];
	struct cxip_fc_peer *peer;
	struct cxip_addr caddr = {
		.nic = nic_addr,
		.pid = pid,
	};
	struct cxip_req *req;
	struct dlist_entry *tmp;
	int ret;

	fastlock_acquire(&txc->lock);

	peer = cxip_fc_peer_lookup(txc, caddr, rxc_id);
	if (!peer) {
		CXIP_LOG_ERROR("FC peer not found: TXC: %u NIC: %#x PID: %d\n",
			       txc_id, nic_addr, pid);
		fastlock_release(&txc->lock);
		return -FI_ENOENT;
	}

	CXIP_LOG_DBG("Replaying dropped sends, TXC: %u NIC: %#x PID: %d\n",
		     txc_id, nic_addr, pid);

	dlist_foreach_container_safe(&peer->msg_queue, struct cxip_req,
				     req, send.txc_entry, tmp) {
		ret = _cxip_send_req(req);
		assert(ret == FI_SUCCESS);

		/* Move request back to the message queue. */
		dlist_remove(&req->send.txc_entry);
		req->send.fc_peer = NULL;
		dlist_insert_tail(&req->send.txc_entry, &txc->msg_queue);

		CXIP_LOG_DBG("Replayed %p\n", req);
	}

	cxip_fc_peer_fini(peer);

	fastlock_release(&txc->lock);

	return FI_SUCCESS;
}

/*
 * cxip_send_req_dropped() - Mark the Send request dropped.
 *
 * Mark the Send request dropped. Mark the target peer as disabled. Track all
 * outstanding Sends targeting the disabled peer. When all outstanding Sends
 * are completed, recovery will be performed.
 */
static int cxip_send_req_dropped(struct cxip_txc *txc, struct cxip_req *req)
{
	struct cxip_fc_peer *peer;
	int ret;

	fastlock_acquire(&txc->lock);

	/* Check if peer is already disabled */
	peer = cxip_fc_peer_lookup(txc, req->send.caddr, req->send.rxc_id);
	if (!peer) {
		ret = cxip_fc_peer_init(txc, req->send.caddr, req->send.rxc_id,
					&peer);
		if (ret != FI_SUCCESS) {
			fastlock_release(&txc->lock);
			return ret;
		}

		CXIP_LOG_DBG("Disabled peer detected, TXC: %p NIC: %#x PID: %u pending: %u\n",
			     txc, peer->caddr.nic, peer->caddr.pid,
			     peer->pending);
	}

	peer->dropped++;
	CXIP_LOG_DBG("Send dropped, req: %p NIC: %#x PID: %u pending: %u dropped: %u\n",
		     req, peer->caddr.nic, peer->caddr.pid, peer->pending,
		     peer->dropped);

	/* Account for the dropped message. */
	cxip_fc_peer_put(peer);

	fastlock_release(&txc->lock);

	return FI_SUCCESS;
}

/*
 * cxip_send_req_queue() - Queue Send request on TXC.
 *
 * Place the Send request in an ordered SW queue. Return error if the target
 * peer is disabled.
 */
static int cxip_send_req_queue(struct cxip_txc *txc, struct cxip_req *req)
{
	struct cxip_fc_peer *peer;

	fastlock_acquire(&txc->lock);

	if (!dlist_empty(&txc->fc_peers)) {
		peer = cxip_fc_peer_lookup(txc, req->send.caddr,
					   req->send.rxc_id);
		if (peer) {
			/* Peer is disabled */
			fastlock_release(&txc->lock);
			return -FI_EAGAIN;
		}
	}

	dlist_insert_tail(&req->send.txc_entry, &txc->msg_queue);

	fastlock_release(&txc->lock);

	return FI_SUCCESS;
}

/*
 * cxip_send_req_dequeue() - Dequeue Send request from TXC.
 *
 * Remove the Send requst from the ordered message queue. Update peer
 * flow-control state, if necessary.
 */
static void cxip_send_req_dequeue(struct cxip_txc *txc, struct cxip_req *req)
{
	fastlock_acquire(&txc->lock);

	if (req->send.fc_peer) {
		/* The peer was disabled after this message arrived. */
		CXIP_LOG_DBG("Send not dropped, req: %p NIC: %#x PID: %u pending: %u dropped: %u\n",
			     req, req->send.fc_peer->caddr.nic,
			     req->send.fc_peer->caddr.pid,
			     req->send.fc_peer->pending,
			     req->send.fc_peer->dropped);

		cxip_fc_peer_put(req->send.fc_peer);
	}

	dlist_remove(&req->send.txc_entry);

	fastlock_release(&txc->lock);
}

/*
 * _cxip_send() - Common message send function. Used for tagged and untagged
 * sends of all sizes.
 */
static ssize_t _cxip_send(struct cxip_txc *txc, const void *buf, size_t len,
			  void *desc, uint64_t data, fi_addr_t dest_addr,
			  uint64_t tag, void *context, uint64_t flags,
			  bool tagged)
{
	struct cxip_req *req;
	struct cxip_addr caddr;
	int ret;

	if (!txc->enabled)
		return -FI_EOPBADSTATE;

	if (!ofi_send_allowed(txc->attr.caps))
		return -FI_ENOPROTOOPT;

	if (len && !buf)
		return -FI_EINVAL;

	if (len > CXIP_EP_MAX_MSG_SZ)
		return -FI_EMSGSIZE;

	req = cxip_cq_req_alloc(txc->send_cq, false, txc);
	if (!req) {
		CXIP_LOG_DBG("Failed to allocate request\n");
		return -FI_EAGAIN;
	}
	ofi_atomic_inc32(&txc->otx_reqs);

	/* Save Send parameters to replay */
	req->send.txc = txc;
	req->send.buf = buf;
	req->send.len = len;
	req->send.tagged = tagged;
	req->send.tag = tag;
	req->send.data = data;
	req->send.flags = flags;

	/* Set completion parameters */
	req->context = (uint64_t)context;
	req->flags = FI_SEND | (flags & (FI_COMPLETION | FI_MATCH_COMPLETE));
	if (tagged)
		req->flags |= FI_TAGGED;
	else
		req->flags |= FI_MSG;

	req->data_len = 0;
	req->buf = 0;
	req->data = 0;
	req->tag = 0;

	/* Look up target CXI address */
	ret = _cxip_av_lookup(txc->ep_obj->av, dest_addr, &caddr);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("Failed to look up FI addr: %d\n", ret);
		goto req_free;
	}
	req->send.caddr = caddr;
	req->send.rxc_id = CXIP_AV_ADDR_RXC(txc->ep_obj->av, dest_addr);

	/* Check if target peer is disabled */
	ret = cxip_send_req_queue(req->send.txc, req);
	if (ret != FI_SUCCESS) {
		CXIP_LOG_DBG("Target peer disabled\n");
		goto req_free;
	}

	/* Try Send */
	ret = _cxip_send_req(req);
	if (ret != FI_SUCCESS)
		goto req_dequeue;

	CXIP_LOG_DBG("req: %p buf: %p len: %lu dest_addr: %ld tag(%c): 0x%lx context %#lx\n",
		     req, req->send.buf, req->send.len, dest_addr,
		     req->send.tagged ? '*' : '-', req->send.tag,
		     req->context);

	return FI_SUCCESS;

req_dequeue:
	cxip_send_req_dequeue(req->send.txc, req);
req_free:
	ofi_atomic_dec32(&txc->otx_reqs);
	cxip_cq_req_free(req);

	return ret;
}

/*
 * Libfabric APIs
 */

static ssize_t cxip_trecv(struct fid_ep *ep, void *buf, size_t len, void *desc,
			  fi_addr_t src_addr, uint64_t tag, uint64_t ignore,
			  void *context)
{
	struct cxip_rxc *rxc;

	if (cxip_fid_to_rxc(ep, &rxc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_recv(rxc, buf, len, desc, src_addr, tag, ignore, context,
			  rxc->attr.op_flags, true);
}

static ssize_t cxip_trecvv(struct fid_ep *ep, const struct iovec *iov,
			   void **desc, size_t count, fi_addr_t src_addr,
			   uint64_t tag, uint64_t ignore, void *context)
{
	struct cxip_rxc *rxc;

	if (!iov || count != 1)
		return -FI_EINVAL;

	if (cxip_fid_to_rxc(ep, &rxc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_recv(rxc, iov[0].iov_base, iov[0].iov_len,
			  desc ? desc[0] : NULL,
			  src_addr, tag, ignore, context, rxc->attr.op_flags,
			  true);
}

static ssize_t cxip_trecvmsg(struct fid_ep *ep, const struct fi_msg_tagged *msg,
			     uint64_t flags)
{
	struct cxip_rxc *rxc;

	if (!msg || !msg->msg_iov || msg->iov_count != 1)
		return -FI_EINVAL;

	if (flags & ~CXIP_RX_OP_FLAGS)
		return -FI_EBADFLAGS;

	if (cxip_fid_to_rxc(ep, &rxc) != FI_SUCCESS)
		return -FI_EINVAL;

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!rxc->selective_completion)
		flags |= FI_COMPLETION;

	return _cxip_recv(rxc, msg->msg_iov[0].iov_base,
			  msg->msg_iov[0].iov_len,
			  msg->desc ? msg->desc[0] : NULL, msg->addr,
			  msg->tag, msg->ignore, msg->context, flags, true);
}

static ssize_t cxip_tsend(struct fid_ep *ep, const void *buf, size_t len,
			  void *desc, fi_addr_t dest_addr, uint64_t tag,
			  void *context)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, desc, 0, dest_addr, tag, context,
			  txc->attr.op_flags, true);
}

static ssize_t cxip_tsendv(struct fid_ep *ep, const struct iovec *iov,
			   void **desc, size_t count, fi_addr_t dest_addr,
			   uint64_t tag, void *context)
{
	struct cxip_txc *txc;

	if (!iov || count != 1)
		return -FI_EINVAL;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, iov[0].iov_base, iov[0].iov_len,
			  desc ? desc[0] : NULL, 0,
			  dest_addr, tag, context, txc->attr.op_flags, true);
}

static ssize_t cxip_tsendmsg(struct fid_ep *ep,
			     const struct fi_msg_tagged *msg, uint64_t flags)
{
	struct cxip_txc *txc;

	if (!msg || !msg->msg_iov || msg->iov_count != 1)
		return -FI_EINVAL;

	if (flags & ~CXIP_TX_OP_FLAGS)
		return -FI_EBADFLAGS;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!txc->selective_completion)
		flags |= FI_COMPLETION;

	return _cxip_send(txc, msg->msg_iov[0].iov_base,
			  msg->msg_iov[0].iov_len,
			  msg->desc ? msg->desc[0] : NULL, msg->data,
			  msg->addr, msg->tag, msg->context, flags, true);
}

static ssize_t cxip_tinject(struct fid_ep *ep, const void *buf, size_t len,
			    fi_addr_t dest_addr, uint64_t tag)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, NULL, 0, dest_addr, tag, NULL,
			  FI_INJECT, true);
}

static ssize_t cxip_tsenddata(struct fid_ep *ep, const void *buf, size_t len,
			      void *desc, uint64_t data, fi_addr_t dest_addr,
			      uint64_t tag, void *context)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, desc, data, dest_addr, tag, context,
			  txc->attr.op_flags, true);
}

static ssize_t cxip_tinjectdata(struct fid_ep *ep, const void *buf, size_t len,
				uint64_t data, fi_addr_t dest_addr,
				uint64_t tag)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, NULL, data, dest_addr, tag, NULL,
			  FI_INJECT, true);
}

struct fi_ops_tagged cxip_ep_tagged_ops = {
	.size = sizeof(struct fi_ops_tagged),
	.recv = cxip_trecv,
	.recvv = cxip_trecvv,
	.recvmsg = cxip_trecvmsg,
	.send = cxip_tsend,
	.sendv = cxip_tsendv,
	.sendmsg = cxip_tsendmsg,
	.inject = cxip_tinject,
	.senddata = cxip_tsenddata,
	.injectdata = cxip_tinjectdata,
};

static ssize_t cxip_recv(struct fid_ep *ep, void *buf, size_t len, void *desc,
			 fi_addr_t src_addr, void *context)
{
	struct cxip_rxc *rxc;

	if (cxip_fid_to_rxc(ep, &rxc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_recv(rxc, buf, len, desc, src_addr, 0, 0, context,
			  rxc->attr.op_flags, false);
}

static ssize_t cxip_recvv(struct fid_ep *ep, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t src_addr,
			  void *context)
{
	struct cxip_rxc *rxc;

	if (!iov || count != 1)
		return -FI_EINVAL;

	if (cxip_fid_to_rxc(ep, &rxc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_recv(rxc, iov[0].iov_base, iov[0].iov_len,
			  desc ? desc[0] : NULL,
			  src_addr, 0, 0, context, rxc->attr.op_flags, false);
}

static ssize_t cxip_recvmsg(struct fid_ep *ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	struct cxip_rxc *rxc;

	if (!msg || !msg->msg_iov || msg->iov_count != 1)
		return -FI_EINVAL;

	if (flags & ~CXIP_RX_OP_FLAGS)
		return -FI_EBADFLAGS;

	if (cxip_fid_to_rxc(ep, &rxc) != FI_SUCCESS)
		return -FI_EINVAL;

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!rxc->selective_completion)
		flags |= FI_COMPLETION;

	return _cxip_recv(rxc, msg->msg_iov[0].iov_base,
			  msg->msg_iov[0].iov_len,
			  msg->desc ? msg->desc[0] : NULL, msg->addr, 0, 0,
			  msg->context, flags, false);
}

static ssize_t cxip_send(struct fid_ep *ep, const void *buf, size_t len,
			 void *desc, fi_addr_t dest_addr, void *context)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, desc, 0, dest_addr, 0, context,
			  txc->attr.op_flags, false);
}

static ssize_t cxip_sendv(struct fid_ep *ep, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t dest_addr,
			  void *context)
{
	struct cxip_txc *txc;

	if (!iov || count != 1)
		return -FI_EINVAL;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, iov[0].iov_base, iov[0].iov_len,
			  desc ? desc[0] : NULL, 0, dest_addr, 0, context,
			  txc->attr.op_flags, false);
}

static ssize_t cxip_sendmsg(struct fid_ep *ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	struct cxip_txc *txc;

	if (!msg || !msg->msg_iov || msg->iov_count != 1)
		return -FI_EINVAL;

	if (flags & ~CXIP_TX_OP_FLAGS)
		return -FI_EBADFLAGS;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	/* If selective completion is not requested, always generate
	 * completions.
	 */
	if (!txc->selective_completion)
		flags |= FI_COMPLETION;

	return _cxip_send(txc, msg->msg_iov[0].iov_base,
			  msg->msg_iov[0].iov_len,
			  msg->desc ? msg->desc[0] : NULL, msg->data,
			  msg->addr, 0, msg->context, flags, false);
}

static ssize_t cxip_inject(struct fid_ep *ep, const void *buf, size_t len,
			   fi_addr_t dest_addr)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, NULL, 0, dest_addr, 0, NULL,
			  FI_INJECT, false);
}

static ssize_t cxip_senddata(struct fid_ep *ep, const void *buf, size_t len,
			     void *desc, uint64_t data, fi_addr_t dest_addr,
			     void *context)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, desc, data, dest_addr, 0, context,
			  txc->attr.op_flags, false);
}

static ssize_t cxip_injectdata(struct fid_ep *ep, const void *buf, size_t len,
			       uint64_t data, fi_addr_t dest_addr)
{
	struct cxip_txc *txc;

	if (cxip_fid_to_txc(ep, &txc) != FI_SUCCESS)
		return -FI_EINVAL;

	return _cxip_send(txc, buf, len, NULL, data, dest_addr, 0, NULL,
			  FI_INJECT, false);
}

struct fi_ops_msg cxip_ep_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = cxip_recv,
	.recvv = cxip_recvv,
	.recvmsg = cxip_recvmsg,
	.send = cxip_send,
	.sendv = cxip_sendv,
	.sendmsg = cxip_sendmsg,
	.inject = cxip_inject,
	.senddata = cxip_senddata,
	.injectdata = cxip_injectdata,
};

