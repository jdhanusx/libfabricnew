/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2014 Intel Corporation, Inc. All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2020 Cray Inc. All rights reserved.
 */

/* Support for Restricted Nomatch Put.
 */


/****************************************************************************
 * Exported:
 *
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <sys/time.h>
#include <sys/types.h>

#include <ofi_list.h>
#include <ofi.h>
#include <fenv.h>
#include <xmmintrin.h>

#include "cxip.h"

#define	MAGIC	0x1776

#define CXIP_DBG(...) _CXIP_DBG(FI_LOG_EP_CTRL, \
		"COLL " __VA_ARGS__)
#define CXIP_INFO(...) _CXIP_INFO(FI_LOG_EP_CTRL, \
		"COLL " __VA_ARGS__)
#define CXIP_WARN(...) _CXIP_WARN(FI_LOG_EP_CTRL, \
		"COLL " __VA_ARGS__)

static bool _coll_arm_disable = false;

static ssize_t _coll_append_buffer(struct cxip_coll_pte *coll_pte,
				   struct cxip_coll_buf *buf);

/****************************************************************************
 * Collective cookie structure.
 *
 * mcast_id is not necessary given one PTE per multicast address: all request
 * structures used for posting receive buffers will receive events from
 * only that multicast. If underlying drivers are changed to allow a single PTE
 * to be mapped to multiple multicast addresses, the mcast_id field will be
 * needed to disambiguate packets.
 *
 * red_id is needed to disambiguate packets delivered for different concurrent
 * reductions.
 *
 * retry is a control bit that can be invoked by the hw root node to initiate a
 * retransmission of the data from the leaves, if packets are lost.
 *
 * magic is a magic number used to identify this packet as a reduction packet.
 * The basic send/receive code can be used for other kinds of restricted IDC
 * packets.
 */
union cxip_coll_cookie {
	struct {
		uint32_t mcast_id:13;
		uint32_t red_id:3;
		uint32_t magic: 15;
		uint32_t retry: 1;
	};
	uint32_t raw;
};

/**
 * Reduction packet for Cassini:
 *
 *  +----------------------------------------------------------+
 *  | BYTES | Mnemonic    | Definition                         |
 *  +----------------------------------------------------------+
 *  | 48:17 | RED_PAYLOAD | Reduction payload, always 32 bytes |
 *  | 16:5  | RED_HDR     | Reduction Header (below)           |
 *  | 4:0   | RED_PADDING | Padding                            |
 *  +----------------------------------------------------------+
 *
 *  Reduction header format, from Table 95 in CSDG 5.7.2, Table 24 in RSDG
 *  4.5.9.4:
 *  --------------------------------------------------------
 *  | Field          | Description              | Bit | Size (bits)
 *  --------------------------------------------------------
 *  | rt_seqno       | Sequence number          |  0  | 10 |
 *  | rt_arm         | Multicast arm command    | 10  |  1 |
 *  | rt_op          | Reduction operation      | 11  |  6 |
 *  | rt_count       | Number of contributions  | 17  | 20 |
 *  | rt_resno       | Result number            | 37  | 10 |
 *  | rt_rc          | result code              | 47  |  4 |
 *  | rt_repsum_m    | Reproducible sum M value | 51  |  8 |
 *  | rt_repsum_ovfl | Reproducible sum M ovfl  | 59  |  2 |
 *  | rt_pad         | Pad to 64 bits           | 61  |  3 |
 *  --------------------------------------------------------
 *  | rt_cookie      | Cookie value             | 64  | 32 |
 *  --------------------------------------------------------
 */
// TODO move to cxi_prov_hw.h (?)
struct red_pkt {
	uint8_t pad[5];
	union {
		struct {
			uint64_t seqno:10;
			uint64_t arm:1;
			uint64_t op:6;
			uint64_t redcnt:20;
			uint64_t resno:10;
			uint64_t rc:4;
			uint64_t repsum_m:8;
			uint64_t repsum_ovfl:2;
			uint64_t pad:3;
		} hdr;
		uint64_t redhdr;
	};
	uint32_t cookie;
	union cxip_coll_data data;
} __attribute__((packed));

/* Reduction rt_rc values.
 */
// TODO move to cxi_prov_hw.h (?)
enum pkt_rc {
	RC_SUCCESS = 0,
	RC_FLT_INEXACT = 1,
	RC_FLT_OVERFLOW = 3,
	RC_FLT_INVALID = 4,
	RC_REPSUM_INEXACT = 5,
	RC_INT_OVERFLOW = 6,
	RC_CONTR_OVERFLOW = 7,
	RC_OP_MISMATCH = 8,
};

__attribute__((unused))
static void _dump_red_data(const void *buf, const char *hdr)
{
	const union cxip_coll_data *data = (const union cxip_coll_data *)buf;
	int i;
	if (hdr)
		printf("%s\n", hdr);
	for (i = 0; i < 4; i++)
		printf("  ival[%d]     = %016lx\n", i, data->ival[i]);
}

__attribute__((unused))
static void _dump_red_pkt(struct red_pkt *pkt, char *dir)
{
	printf("---------------\n");
	printf("Reduction packet (%s):\n", dir);
	printf("  seqno       = %d\n", pkt->hdr.seqno);
	printf("  arm         = %d\n", pkt->hdr.arm);
	printf("  op          = %d\n", pkt->hdr.op);
	printf("  redcnt      = %d\n", pkt->hdr.redcnt);
	printf("  resno       = %d\n", pkt->hdr.resno);
	printf("  rc          = %d\n", pkt->hdr.rc);
	printf("  repsum_m    = %d\n", pkt->hdr.repsum_m);
	printf("  repsum_ovfl = %d\n", pkt->hdr.repsum_ovfl);
	printf("  cookie      = %08x\n", pkt->cookie);
	_dump_red_data(pkt->data.databuf, NULL);
	printf("---------------\n");
	fflush(stdout);
}

static inline void _hton_red_pkt(struct red_pkt *pkt)
{
	int i;

	pkt->redhdr = htobe64(pkt->redhdr);
	pkt->cookie = htobe32(pkt->cookie);
	for (i = 0; i < 4; i++)
		pkt->data.ival[i] = htobe64(pkt->data.ival[i]);
}

static inline void _ntoh_red_pkt(struct red_pkt *pkt)
{
	int i;

	pkt->redhdr = be64toh(pkt->redhdr);
	pkt->cookie = be32toh(pkt->cookie);
	for (i = 0; i < 4; i++)
		pkt->data.ival[i] = be64toh(pkt->data.ival[i]);
}

/****************************************************************************
 * SEND operation (restricted IDC Put to a remote PTE)
 */

static void _progress_coll(struct cxip_coll_reduction *reduction,
			   struct red_pkt *pkt);

/* Generate a dfa and index extension for a reduction.
 */
static int _gen_tx_dfa(struct cxip_coll_reduction *reduction,
		       int av_set_idx, union c_fab_addr *dfa,
		       uint8_t *index_ext, bool *is_mcast)
{
	struct cxip_ep_obj *ep_obj;
	struct cxip_av_set *av_set;
	struct cxip_addr dest_caddr;
	fi_addr_t dest_addr;
	int pid_bits;
	int idx_ext;
	int ret;

	ep_obj = reduction->mc_obj->ep_obj;
	av_set = reduction->mc_obj->av_set;

	/* Send address */
	switch (av_set->comm_key.type) {
	case COMM_KEY_MULTICAST:
		/* - dest_addr == multicast ID
		 * - idx_ext == 0
		 * - dfa == multicast destination
		 * - index_ext == 0
		 */
		if (is_netsim(ep_obj)) {
			CXIP_INFO("NETSIM does not support mcast\n");
			return -FI_EINVAL;
		}
		idx_ext = 0;
		cxi_build_mcast_dfa(av_set->comm_key.mcast.mcast_id,
				    reduction->red_id, idx_ext,
				    dfa, index_ext);
		*is_mcast = true;
		break;
	case COMM_KEY_UNICAST:
		/* - dest_addr == destination AV index
		 * - idx_ext == CXIP_PTL_IDX_COLL
		 * - dfa = remote nic
		 * - index_ext == CXIP_PTL_IDX_COLL
		 */
		if (av_set_idx >= av_set->fi_addr_cnt) {
			CXIP_INFO("av_set_idx out-of-range\n");
			return -FI_EINVAL;
		}
		dest_addr = av_set->fi_addr_ary[av_set_idx];
		ret = _cxip_av_lookup(ep_obj->av, dest_addr, &dest_caddr);
		if (ret != FI_SUCCESS)
			return ret;
		pid_bits = ep_obj->domain->iface->dev->info.pid_bits;
		cxi_build_dfa(dest_caddr.nic, dest_caddr.pid, pid_bits,
			      CXIP_PTL_IDX_COLL, dfa, index_ext);
		*is_mcast = false;
		break;
	case COMM_KEY_RANK:
		/* - dest_addr == multicast object index
		 * - idx_ext == multicast object index
		 * - dfa == source NIC
		 * - index_ext == idx_ext offset beyond RXCs
		 */
		if (av_set_idx >= av_set->fi_addr_cnt) {
			CXIP_INFO("av_set_idx out-of-range\n");
			return -FI_EINVAL;
		}
		dest_caddr = ep_obj->src_addr;
		pid_bits = ep_obj->domain->iface->dev->info.pid_bits;
		idx_ext = CXIP_PTL_IDX_COLL + av_set_idx;
		cxi_build_dfa(dest_caddr.nic, dest_caddr.pid, pid_bits,
			      idx_ext, dfa, index_ext);
		*is_mcast = false;
		break;
	default:
		CXIP_INFO("unexpected comm_key type: %d\n",
			  av_set->comm_key.type);
		return -FI_EINVAL;
	}
	return FI_SUCCESS;
}

/**
 * Issue a restricted IDC Put to the destination address.
 *
 * Exported for unit testing.
 *
 * @param reduction - reduction object
 * @param av_set_idx - index of address in av_set
 * @param buffer - buffer containing data to send
 * @param buflen - byte count of data in buffer
 *
 * @return int - return code
 */
int cxip_coll_send(struct cxip_coll_reduction *reduction,
		   int av_set_idx, const void *buffer, size_t buflen)
{
	struct cxip_ep_obj *ep_obj;
	struct cxip_cmdq *cmdq;
	union c_fab_addr dfa;
	uint8_t index_ext;
	uint8_t pid_bits;
	bool is_mcast;
	int ret;

	if (buflen && !buffer) {
		CXIP_INFO("no buffer\n");
		return -FI_EINVAL;
	}

	ep_obj = reduction->mc_obj->ep_obj;

	ret = _gen_tx_dfa(reduction, av_set_idx, &dfa, &index_ext, &is_mcast);
	if (ret)
		return ret;

	/* pid_bits needed to obtain initiator address */
	pid_bits = ep_obj->domain->iface->dev->info.pid_bits;
	cmdq = ep_obj->coll.tx_cmdq;

	union c_cmdu cmd = {};
	cmd.c_state.event_send_disable = 1;
	cmd.c_state.event_success_disable = 1;
	cmd.c_state.restricted = 1;
	cmd.c_state.reduction = is_mcast;
	cmd.c_state.index_ext = index_ext;
	cmd.c_state.eq = ep_obj->coll.tx_cq->evtq->eqn;
	cmd.c_state.initiator = CXI_MATCH_ID(pid_bits, ep_obj->src_addr.pid,
					     ep_obj->src_addr.nic);

	fastlock_acquire(&cmdq->lock);
	ret = cxip_cmdq_emit_c_state(cmdq, &cmd.c_state);
	if (ret)
		goto err_unlock;

	memset(&cmd.idc_put, 0, sizeof(cmd.idc_put));
	cmd.idc_put.idc_header.dfa = dfa;
	ret = cxi_cq_emit_idc_put(cmdq->dev_cmdq, &cmd.idc_put,
				  buffer, buflen);
	if (ret) {
		/* Return error according to Domain Resource Management
		 */
		ret = -FI_EAGAIN;
		goto err_unlock;
	}

	cxi_cq_ring(cmdq->dev_cmdq);
	ret = FI_SUCCESS;

	ofi_atomic_inc32(&reduction->mc_obj->send_cnt);

err_unlock:
	fastlock_release(&cmdq->lock);
	return ret;
}

/****************************************************************************
 * RECV operation (restricted IDC Put to a local PTE)
 */

/* Report success/error results of an RX event through RX CQ / counters, and
 * roll over the buffers if appropriate.
 *
 * NOTE: req may be invalid after this call.
 */
static void _coll_rx_req_report(struct cxip_req *req)
{
	size_t overflow;
	int err, ret;

	req->flags &= (FI_RECV | FI_COMPLETION | FI_COLLECTIVE);

	/* Interpret results */
	overflow = req->coll.hw_req_len - req->data_len;
	if (req->coll.rc == C_RC_OK && req->coll.isred && !overflow) {
		/* success */
		if (req->flags & FI_COMPLETION) {
			/* failure means progression is hung */
			ret = cxip_cq_req_complete(req);
			if (ret)
				CXIP_FATAL(
				    "cxip_cq_req_complete failed: %d\n", ret);
		}

		if (req->coll.coll_pte->ep_obj->coll.rx_cntr) {
			/* failure means counts cannot be trusted */
			ret = cxip_cntr_mod(
				req->coll.coll_pte->ep_obj->coll.rx_cntr, 1,
				false, false);
			if (ret)
				CXIP_WARN(
					"Failed success cxip_cntr_mod: %d\n",
					ret);
		}
	} else {
		/* failure */
		if (req->coll.rc != C_RC_OK) {
			/* real network error of some sort */
			err = FI_EIO;
			CXIP_WARN("Request error: %p (err: %d, %s)\n",
				  req, err, cxi_rc_to_str(req->coll.rc));
		} else if (overflow) {
			/* can only happen on very large packet (> 64 bytes) */
			err = FI_EMSGSIZE;
			CXIP_WARN("Request truncated: %p (err: %d, %s)\n",
				  req, err, cxi_rc_to_str(req->coll.rc));
		} else {
			/* non-reduction packet */
			err = FI_ENOMSG;
			CXIP_INFO("Not reduction pkt: %p (err: %d, %s)\n",
				  req, err, cxi_rc_to_str(req->coll.rc));
		}

		/* failure means progression is hung */
		ret = cxip_cq_req_error(req, overflow, err, req->coll.rc,
					NULL, 0);
		if (ret)
			CXIP_FATAL("cxip_cq_req_error: %d\n", ret);

		if (req->coll.coll_pte->ep_obj->coll.rx_cntr) {
			/* failure means counts cannot be trusted */
			ret = cxip_cntr_mod(
				req->coll.coll_pte->ep_obj->coll.rx_cntr, 1,
				false, true);
			if (ret)
				CXIP_WARN("cxip_cntr_mod: %d\n", ret);
		}
	}

	/* manage buffer rollover */
	if (req->coll.mrecv_space <
	    req->coll.coll_pte->ep_obj->coll.min_multi_recv) {
		struct cxip_coll_pte *coll_pte = req->coll.coll_pte;
		struct cxip_coll_buf *buf = req->coll.coll_buf;

		/* Will be re-incremented when LINK is received */
		ofi_atomic_dec32(&coll_pte->buf_cnt);
		ofi_atomic_inc32(&coll_pte->buf_swap_cnt);

		/* Re-use this buffer in the hardware */
		ret = _coll_append_buffer(coll_pte, buf);
		if (ret != FI_SUCCESS)
			CXIP_WARN("Re-link buffer failed: %d\n", ret);

		/* Hardware has silently unlinked this */
		cxip_cq_req_free(req);
	}
}

/* Evaluate PUT request to see if this is a reduction packet.
 */
static void _coll_rx_progress(struct cxip_req *req,
			      const union c_event *event)
{
	struct cxip_coll_mc *mc_obj;
	union cxip_coll_cookie cookie;
	struct cxip_coll_reduction *reduction;
	struct red_pkt *pkt;

	mc_obj = req->coll.coll_pte->mc_obj;
	ofi_atomic_inc32(&mc_obj->recv_cnt);

	/* If not the right size, don't swap bytes */
	if (req->data_len != sizeof(struct red_pkt)) {
		CXIP_INFO("Bad coll packet size: %ld\n", req->data_len);
		return;
	}

	/* If swap doesn't look like reduction packet, swap back */
	pkt = (struct red_pkt *)req->buf;
	_ntoh_red_pkt(pkt);
	cookie.raw = pkt->cookie;
	if (cookie.magic != MAGIC)
	{
		CXIP_INFO("Bad coll MAGIC: %x\n", cookie.magic);
		_hton_red_pkt(pkt);
		return;
	}

	/* Treat as a reduction packet */
	req->coll.isred = true;
	ofi_atomic_inc32(&mc_obj->pkt_cnt);
	reduction = &mc_obj->reduction[cookie.red_id];
	_progress_coll(reduction, pkt);
}

/* Event-handling callback for posted receive buffers.
 */
static int _coll_recv_cb(struct cxip_req *req, const union c_event *event)
{
	req->coll.rc = cxi_tgt_event_rc(event);
	switch (event->hdr.event_type) {
	case C_EVENT_LINK:
		/* Enabled */
		if (req->coll.rc != C_RC_OK) {
			CXIP_WARN("LINK error rc: %d\n", req->coll.rc);
			break;
		}
		CXIP_DBG("LINK event seen\n");
		ofi_atomic_inc32(&req->coll.coll_pte->buf_cnt);
		break;
	case C_EVENT_UNLINK:
		/* Normally disabled, errors only */
		req->coll.rc = cxi_tgt_event_rc(event);
		if (req->coll.rc != C_RC_OK) {
			CXIP_WARN("UNLINK error rc: %d\n", req->coll.rc);
			break;
		}
		CXIP_DBG("UNLINK event seen\n");
		break;
	case C_EVENT_PUT:
		req->coll.isred = false;
		req->coll.rc = cxi_tgt_event_rc(event);
		if (req->coll.rc != C_RC_OK) {
			CXIP_WARN("PUT error rc: %d\n", req->coll.rc);
			break;
		}
		CXIP_DBG("PUT event seen\n");
		req->buf = (uint64_t)(CXI_IOVA_TO_VA(
					req->coll.coll_buf->cxi_md->md,
					event->tgt_long.start));
		req->coll.mrecv_space -= event->tgt_long.mlength;
		req->coll.hw_req_len = event->tgt_long.rlength;
		req->data_len = event->tgt_long.mlength;
		_coll_rx_progress(req, event);
		_coll_rx_req_report(req);
		break;
	default:
		req->coll.rc = cxi_tgt_event_rc(event);
		CXIP_WARN("Unexpected event type %d, rc: %d\n",
			  event->hdr.event_type, req->coll.rc);
		break;
	}

	return FI_SUCCESS;
}

/* Inject a hardware LE append. Does not generate HW LINK event unless error.
 */
static int _hw_coll_recv(struct cxip_coll_pte *coll_pte, struct cxip_req *req)
{
	uint32_t le_flags;
	uint64_t recv_iova;
	int ret;

	/* Always set manage_local in Receive LEs. This makes Cassini ignore
	 * initiator remote_offset in all Puts.
	 */
	le_flags = C_LE_EVENT_UNLINK_DISABLE | C_LE_OP_PUT | C_LE_MANAGE_LOCAL;

	recv_iova = CXI_VA_TO_IOVA(req->coll.coll_buf->cxi_md->md,
				   (uint64_t)req->coll.coll_buf->buffer);

	ret = cxip_pte_append(coll_pte->pte,
			      recv_iova,
			      req->coll.coll_buf->bufsiz,
			      req->coll.coll_buf->cxi_md->md->lac,
			      C_PTL_LIST_PRIORITY,
			      req->req_id,
			      0, 0, 0,
			      coll_pte->ep_obj->min_multi_recv,
			      le_flags, coll_pte->ep_obj->coll.rx_cntr,
			      coll_pte->ep_obj->coll.rx_cmdq,
			      true);
	if (ret != FI_SUCCESS) {
		CXIP_WARN("PTE append inject failed: %d\n", ret);
		return ret;
	}

	return FI_SUCCESS;
}

/* Append a receive buffer to the PTE, with callback to handle receives.
 */
static ssize_t _coll_append_buffer(struct cxip_coll_pte *coll_pte,
				   struct cxip_coll_buf *buf)
{
	struct cxip_req *req;
	int ret;

	if (buf->bufsiz && !buf->buffer) {
		CXIP_INFO("no buffer\n");
		return -FI_EINVAL;
	}

	/* Allocate and populate a new request
	 * Sets:
	 * - req->cq
	 * - req->req_id to request index
	 * - req->req_ctx to passed context (buf)
	 * - req->discard to false
	 * - Inserts into the cq->req_list
	 */
	req = cxip_cq_req_alloc(coll_pte->ep_obj->coll.rx_cq, 1, buf);
	if (!req) {
		ret = -FI_ENOMEM;
		goto recv_unmap;
	}

	/* CQ event fields, set according to fi_cq.3
	 *   - set by provider
	 *   - returned to user in completion event
	 * uint64_t context;	// operation context
	 * uint64_t flags;	// operation flags
	 * uint64_t data_len;   // received data length
	 * uint64_t buf;	// receive buf offset
	 * uint64_t data;	// receive REMOTE_CQ_DATA
	 * uint64_t tag;	// receive tag value on matching interface
	 * fi_addr_t addr;	// sender address (if known) ???
	 */

	/* Request parameters */
	req->type = CXIP_REQ_COLL;
	req->flags = (FI_RECV | FI_COMPLETION | FI_COLLECTIVE);
	req->cb = _coll_recv_cb;
	req->triggered = false;
	req->trig_thresh = 0;
	req->trig_cntr = NULL;
	req->context = (uint64_t)buf;
	req->data_len = 0;
	req->buf = (uint64_t)buf->buffer;
	req->data = 0;
	req->tag = 0;
	req->coll.coll_pte = coll_pte;
	req->coll.coll_buf = buf;
	req->coll.mrecv_space = req->coll.coll_buf->bufsiz;

	/* Returns FI_SUCCESS or FI_EAGAIN */
	ret = _hw_coll_recv(coll_pte, req);
	if (ret != FI_SUCCESS)
		goto recv_dequeue;

	return FI_SUCCESS;

recv_dequeue:
	cxip_cq_req_free(req);

recv_unmap:
	cxip_unmap(buf->cxi_md);
	return ret;
}

/****************************************************************************
 * PTE management functions.
 */

/* PTE state-change callback.
 */
static void _coll_pte_cb(struct cxip_pte *pte, const union c_event *event)
{
	switch (pte->state) {
	case C_PTLTE_ENABLED:
	case C_PTLTE_DISABLED:
		break;
	default:
		CXIP_FATAL("Unexpected state received: %u\n", pte->state);
	}
}

/* Enable a collective PTE. Wait for completion.
 */
static inline int _coll_pte_enable(struct cxip_coll_pte *coll_pte,
				   uint32_t drop_count)
{
	return cxip_pte_set_state_wait(coll_pte->pte,
				       coll_pte->ep_obj->coll.rx_cmdq,
				       coll_pte->ep_obj->coll.rx_cq,
				       C_PTLTE_ENABLED, drop_count);
}

/* Disable a collective PTE. Wait for completion.
 */
static inline int _coll_pte_disable(struct cxip_coll_pte *coll_pte)
{
	return cxip_pte_set_state_wait(coll_pte->pte,
				       coll_pte->ep_obj->coll.rx_cmdq,
				       coll_pte->ep_obj->coll.rx_cq,
				       C_PTLTE_DISABLED, 0);
}

/* Destroy and unmap all buffers used by the collectives PTE.
 */
static void _coll_destroy_buffers(struct cxip_coll_pte *coll_pte)
{
	struct dlist_entry *list = &coll_pte->buf_list;
	struct cxip_coll_buf *buf;

	while (!dlist_empty(list)) {
		dlist_pop_front(list, struct cxip_coll_buf, buf, buf_entry);
		cxip_unmap(buf->cxi_md);
		free(buf);
	}
}

/* Adds 'count' buffers of 'size' bytes to the collecives PTE. This succeeds
 * fully, or it fails and removes all buffers.
 */
static int _coll_add_buffers(struct cxip_coll_pte *coll_pte, size_t size,
			     size_t count)
{
	struct cxip_coll_buf *buf;
	int ret, i;

	if (count < CXIP_COLL_MIN_RX_BUFS) {
		CXIP_INFO("Buffer count %ld < minimum (%d)\n",
			  count, CXIP_COLL_MIN_RX_BUFS);
		return -FI_EINVAL;
	}

	if (size < CXIP_COLL_MIN_RX_SIZE) {
		CXIP_INFO("Buffer size %ld < minimum (%d)\n",
			  size, CXIP_COLL_MIN_RX_SIZE);
		return -FI_EINVAL;
	}

	CXIP_DBG("Adding %ld buffers of size %ld\n", count, size);
	for (i = 0; i < count; i++) {
		buf = calloc(1, sizeof(*buf) + size);
		if (!buf) {
			ret = -FI_ENOMEM;
			goto out;
		}
		ret = cxip_map(coll_pte->ep_obj->domain, (void *)buf->buffer,
			       size, &buf->cxi_md);
		if (ret)
			goto del_msg;
		buf->bufsiz = size;
		dlist_insert_tail(&buf->buf_entry, &coll_pte->buf_list);

		ret = _coll_append_buffer(coll_pte, buf);
		if (ret) {
			CXIP_WARN("Add buffer %d of %ld: %d\n",
				  i, count, ret);
			goto out;
		}
	}
	do {
		sched_yield();
		cxip_cq_progress(coll_pte->ep_obj->coll.rx_cq);
	} while (ofi_atomic_get32(&coll_pte->buf_cnt) < count);

	return FI_SUCCESS;
del_msg:
	free(buf);
out:
	_coll_destroy_buffers(coll_pte);
	return ret;
}

/****************************************************************************
 * Initialize, configure, enable, disable, and close the collective PTE.
 */

/**
 * Initialize the collectives structures.
 *
 * Must be done during EP initialization.
 *
 * @param ep_obj - EP object
 *
 * @return int - FI return code
 */
int cxip_coll_init(struct cxip_ep_obj *ep_obj)
{
	cxip_coll_populate_opcodes();

	ep_obj->coll.rx_cmdq = NULL;
	ep_obj->coll.tx_cmdq = NULL;
	ep_obj->coll.rx_cntr = NULL;
	ep_obj->coll.tx_cntr = NULL;
	ep_obj->coll.rx_cq = NULL;
	ep_obj->coll.tx_cq = NULL;
	ep_obj->coll.min_multi_recv = CXIP_COLL_MIN_FREE;
	ep_obj->coll.buffer_count = CXIP_COLL_MIN_RX_BUFS;
	ep_obj->coll.buffer_size = CXIP_COLL_MIN_RX_SIZE;

	ofi_atomic_initialize32(&ep_obj->coll.mc_count, 0);
	fastlock_init(&ep_obj->coll.lock);

	return FI_SUCCESS;
}

/**
 * Enable collectives.
 *
 * Must be preceded by cxip_coll_init(), called from STD EP enable.
 *
 * There is only one collectives object associated with an EP. It can be safely
 * enabled multiple times.
 *
 * @param ep_obj - EP object
 *
 * @return int - FI return code
 */
int cxip_coll_enable(struct cxip_ep_obj *ep_obj)
{
	if (ep_obj->coll.enabled)
		return FI_SUCCESS;

	/* A read-only or write-only endpoint is legal */
	if (!(ofi_recv_allowed(ep_obj->rxcs[0]->attr.caps) &&
	      ofi_send_allowed(ep_obj->txcs[0]->attr.caps))) {
		CXIP_INFO("EP not recv/send, collectives not enabled\n");
		return FI_SUCCESS;
	}

	/* Sanity checks */
	if (ep_obj->coll.buffer_size == 0)
		return -FI_EINVAL;
	if (ep_obj->coll.buffer_count == 0)
		return -FI_EINVAL;
	if (ep_obj->coll.min_multi_recv == 0)
		return -FI_EINVAL;
	if (ep_obj->coll.min_multi_recv >= ep_obj->coll.buffer_size)
		return -FI_EINVAL;

	/* Bind all STD EP objects to the coll object */
	ep_obj->coll.rx_cmdq = ep_obj->rxcs[0]->rx_cmdq;
	ep_obj->coll.tx_cmdq = ep_obj->txcs[0]->tx_cmdq;
	ep_obj->coll.rx_cntr = ep_obj->rxcs[0]->recv_cntr;
	ep_obj->coll.tx_cntr = ep_obj->txcs[0]->send_cntr;
	ep_obj->coll.rx_cq = ep_obj->rxcs[0]->recv_cq;
	ep_obj->coll.tx_cq = ep_obj->txcs[0]->send_cq;

	ep_obj->coll.enabled = true;

	return FI_SUCCESS;
}

/**
 * Disable collectives.
 *
 * @param ep_obj - EP object
 *
 * @return int - FI return code
 */
int cxip_coll_disable(struct cxip_ep_obj *ep_obj)
{
	if (!ep_obj->coll.enabled)
		return FI_SUCCESS;

	ep_obj->coll.enabled = false;

	return FI_SUCCESS;
}

/**
 * Closes collectives and cleans up.
 *
 * Must be done during EP close.
 *
 * @param ep_obj - EP object
 */
int cxip_coll_close(struct cxip_ep_obj *ep_obj)
{
	if (ofi_atomic_get32(&ep_obj->coll.mc_count) != 0)
		return -FI_EBUSY;

	fastlock_destroy(&ep_obj->coll.lock);

	return FI_SUCCESS;
}

/* Write a Join Complete event to the endpoint EQ
 */
static int _post_join_complete(struct cxip_coll_mc *mc_obj, void *context)
{
	struct fi_eq_entry entry = {};
	int ret;

	entry.fid = &mc_obj->mc_fid.fid;
	entry.context = context;

	ret = ofi_eq_write(&mc_obj->ep_obj->eq->util_eq.eq_fid,
			   FI_JOIN_COMPLETE, &entry,
			   sizeof(entry), FI_COLLECTIVE);
	if (ret < 0)
		return ret;

	return FI_SUCCESS;
}

/****************************************************************************
 * Reduction packet management.
 */

static inline bool is_hw_root(struct cxip_coll_mc *mc_obj)
{
	return (mc_obj->hwroot_index == mc_obj->mynode_index);
}

static inline int _advance_seqno(struct cxip_coll_reduction *reduction)
{
	reduction->seqno = (reduction->seqno + 1) & CXIP_COLL_SEQNO_MASK;
	return reduction->seqno;
}

static inline uint64_t _generate_cookie(uint32_t mcast_id, uint32_t red_id,
					bool retry)
{
	union cxip_coll_cookie cookie = {
		.mcast_id = mcast_id,
		.red_id = red_id,
		.retry = retry,
		.magic = MAGIC,
	};
	return cookie.raw;
}

static inline void _zcopy_pkt_data(void *tgt, const void *src, int len)
{
	if (tgt) {
		if (src)
			memcpy(tgt, src, len);
		else
			len = 0;
		memset((uint8_t *)tgt + len, 0, CXIP_COLL_MAX_TX_SIZE - len);
	}
}

/* Simulated unicast send of multiple packets as root node to leaf nodes.
 */
static ssize_t _send_pkt_as_root(struct cxip_coll_reduction *reduction,
					bool retry)
{
	int i, ret;

	for (i = 0; i < reduction->mc_obj->av_set->fi_addr_cnt; i++) {
		if (i == reduction->mc_obj->mynode_index &&
		    reduction->mc_obj->av_set->fi_addr_cnt > 1)
			continue;
		ret = cxip_coll_send(reduction, i,
				     reduction->tx_msg,
				     sizeof(struct red_pkt));
		if (ret)
			return ret;
	}
	return FI_SUCCESS;
}

/* Simulated unicast send of single packet as leaf node to root node.
 */
static inline ssize_t _send_pkt_as_leaf(struct cxip_coll_reduction *reduction,
					bool retry)
{
	int ret;

	ret = cxip_coll_send(reduction, reduction->mc_obj->hwroot_index,
			     reduction->tx_msg, sizeof(struct red_pkt));
	return ret;
}

/* Multicast send of single packet from root or leaf node.
 */
static inline ssize_t _send_pkt_mc(struct cxip_coll_reduction *reduction,
				   bool retry)
{
	return cxip_coll_send(reduction, 0,
			      reduction->tx_msg,
			      sizeof(struct red_pkt));
}

/* Send packet from root or leaf node as appropriate.
 */
static inline ssize_t _send_pkt(struct cxip_coll_reduction *reduction,
				bool retry)
{
	int ret;

	if (reduction->mc_obj->av_set->comm_key.type == COMM_KEY_MULTICAST) {
		ret = _send_pkt_mc(reduction, retry);
	} else if (is_hw_root(reduction->mc_obj)) {
		ret = _send_pkt_as_root(reduction, retry);
	} else {
		ret = _send_pkt_as_leaf(reduction, retry);
	}
	return ret;
}

/**
 * Called to prevent setting the ARM bit on a root packet. Self-clearing.
 *
 * This is used in testing to suppress Rosetta collective operations. It is of
 * no use in production.
 */
void cxip_coll_arm_disable_once(void)
{
	_coll_arm_disable = true;
}

/**
 * Send a reduction packet.
 *
 * Exported for unit testing.
 *
 * @param reduction - reduction object pointer
 * @param redcnt - reduction count needed
 * @param op - operation code
 * @param data - pointer to send buffer
 * @param len - length of send data in bytes
 * @param retry - retry flag
 *
 * @return int - return code
 */
int cxip_coll_send_red_pkt(struct cxip_coll_reduction *reduction,
			   size_t redcnt, int op, const void *data,
			   int len, bool retry)
{
	struct red_pkt *pkt;
	int seqno, arm, ret;

	if (len > CXIP_COLL_MAX_TX_SIZE) {
		CXIP_INFO("length too large: %d\n", len);
		return -FI_EINVAL;
	}

	pkt = (struct red_pkt *)reduction->tx_msg;

	if (is_hw_root(reduction->mc_obj)) {
		seqno = _advance_seqno(reduction);
		arm = (_coll_arm_disable) ? 0 : 1;
		_coll_arm_disable = false;
	} else {
		seqno = reduction->seqno;
		arm = 0;
	}
	pkt->hdr.seqno = seqno;
	pkt->hdr.resno = seqno;
	pkt->hdr.arm = arm;
	pkt->hdr.redcnt = redcnt;
	pkt->hdr.op = op;
	pkt->cookie = _generate_cookie(reduction->mc_obj->mc_unique,
				       reduction->red_id,
				       retry);
	_zcopy_pkt_data(pkt->data.databuf, data, len);
	_hton_red_pkt(pkt);

	/* -FI_EAGAIN means HW queue is full, should self-clear */
	do {
		ret = _send_pkt(reduction, retry);
	} while (ret == -FI_EAGAIN);

	return ret;
}

/****************************************************************************
 * Reduction operations.
 */

/**
 * Opcodes for collective operations supported by Rosetta.
 *
 * Opcode implies data type.
 */

#define	COLL_OPCODE_BARRIER		0x00
#define	COLL_OPCODE_BIT_AND		0x01
#define	COLL_OPCODE_BIT_OR		0x02
#define	COLL_OPCODE_BIT_XOR		0x03
#define	COLL_OPCODE_INT_MIN		0x10
#define	COLL_OPCODE_INT_MAX		0x11
#define	COLL_OPCODE_INT_MINMAXLOC	0x12
#define	COLL_OPCODE_INT_SUM		0x14
#define	COLL_OPCODE_FLT_MIN		0x20
#define	COLL_OPCODE_FLT_MAX		0x21
#define	COLL_OPCODE_FLT_MINMAXLOC	0x22
#define	COLL_OPCODE_FLT_MINNUM		0x24
#define	COLL_OPCODE_FLT_MAXNUM		0x25
#define	COLL_OPCODE_FLT_MINMAXNUMLOC	0x26
#define	COLL_OPCODE_FLT_SUM_NOFTZ_RND0	0x28
#define	COLL_OPCODE_FLT_SUM_NOFTZ_RND1	0x29
#define	COLL_OPCODE_FLT_SUM_NOFTZ_RND2	0x2a
#define	COLL_OPCODE_FLT_SUM_NOFTZ_RND3	0x2b
#define	COLL_OPCODE_FLT_SUM_FTZ_RND0	0x2c
#define	COLL_OPCODE_FLT_SUM_FTZ_RND1	0x2d
#define	COLL_OPCODE_FLT_SUM_FTZ_RND2	0x2e
#define	COLL_OPCODE_FLT_SUM_FTZ_RND3	0x2f
#define	COLL_OPCODE_FLT_REPSUM		0x30

/* Convert exported op values to Rosetta opcodes
 */
static unsigned int _int8_16_32_op_to_opcode[CXI_FI_OP_LAST];
static unsigned int _int64_op_to_opcode[CXI_FI_OP_LAST];
static unsigned int _flt_op_to_opcode[CXI_FI_OP_LAST];

/* One-time dynamic initialization of FI to CXI opcode.
 *
 * The array lookup is faster than a switch. Non-static initialization makes
 * this adaptive to changes in header files (e.g. new opcodes in FI).
 */
void cxip_coll_populate_opcodes(void)
{
	int rnd, ftz, i;

	if ((int)CXI_FI_MINMAXLOC < (int)FI_ATOMIC_OP_LAST) {
		CXIP_FATAL("Invalid CXI_FMINMAXLOC value\n");
	}
	for (i = 0; i < CXI_FI_OP_LAST; i++) {
		_int8_16_32_op_to_opcode[i] = -FI_EOPNOTSUPP;
		_int64_op_to_opcode[i] = -FI_EOPNOTSUPP;
		_flt_op_to_opcode[i] = -FI_EOPNOTSUPP;
	}
	/* operations supported by 32, 16, and 8 bit integer operands */
	/* NOTE: executed as packed 64-bit quantities */
	_int8_16_32_op_to_opcode[FI_BOR] = COLL_OPCODE_BIT_OR;
	_int8_16_32_op_to_opcode[FI_BAND] = COLL_OPCODE_BIT_AND;
	_int8_16_32_op_to_opcode[FI_BXOR] = COLL_OPCODE_BIT_XOR;
	_int8_16_32_op_to_opcode[CXI_FI_BARRIER] = COLL_OPCODE_BARRIER;

	/* operations supported by 64 bit integer operands */
	_int64_op_to_opcode[FI_MIN] = COLL_OPCODE_INT_MIN;
	_int64_op_to_opcode[FI_MAX] = COLL_OPCODE_INT_MAX;
	_int64_op_to_opcode[FI_SUM] = COLL_OPCODE_INT_SUM;
	_int64_op_to_opcode[FI_BOR] = COLL_OPCODE_BIT_OR;
	_int64_op_to_opcode[FI_BAND] = COLL_OPCODE_BIT_AND;
	_int64_op_to_opcode[FI_BXOR] = COLL_OPCODE_BIT_XOR;
	_int64_op_to_opcode[CXI_FI_MINMAXLOC] = COLL_OPCODE_INT_MINMAXLOC;
	_int64_op_to_opcode[CXI_FI_BARRIER] = COLL_OPCODE_BARRIER;

	/* operations supported by 64 bit double operands */
	_flt_op_to_opcode[FI_MIN] = COLL_OPCODE_FLT_MIN;
	_flt_op_to_opcode[FI_MAX] = COLL_OPCODE_FLT_MAX;
	_flt_op_to_opcode[CXI_FI_MINMAXLOC] = COLL_OPCODE_FLT_MINMAXLOC;
	_flt_op_to_opcode[CXI_FI_MINNUM] = COLL_OPCODE_FLT_MINNUM;
	_flt_op_to_opcode[CXI_FI_MAXNUM] = COLL_OPCODE_FLT_MAXNUM;
	_flt_op_to_opcode[CXI_FI_MINMAXNUMLOC] = COLL_OPCODE_FLT_MINMAXNUMLOC;
	_flt_op_to_opcode[CXI_FI_REPSUM] = COLL_OPCODE_FLT_REPSUM;
	_flt_op_to_opcode[CXI_FI_BARRIER] = COLL_OPCODE_BARRIER;

	/* SUM operations supported by 64 bit double operands */
	rnd = fegetround();
	ftz = _MM_GET_FLUSH_ZERO_MODE();
	switch (rnd) {
	case FE_UPWARD:
		_flt_op_to_opcode[FI_SUM] = (ftz) ?
			COLL_OPCODE_FLT_SUM_FTZ_RND1 :
			COLL_OPCODE_FLT_SUM_NOFTZ_RND1;
		break;
	case FE_DOWNWARD:
		_flt_op_to_opcode[FI_SUM] = (ftz) ?
			COLL_OPCODE_FLT_SUM_FTZ_RND2 :
			COLL_OPCODE_FLT_SUM_NOFTZ_RND2;
		break;
	case FE_TOWARDZERO:
		_flt_op_to_opcode[FI_SUM] = (ftz) ?
			COLL_OPCODE_FLT_SUM_FTZ_RND3 :
			COLL_OPCODE_FLT_SUM_NOFTZ_RND3;
		break;
	case FE_TONEAREST:
		_flt_op_to_opcode[FI_SUM] = (ftz) ?
			COLL_OPCODE_FLT_SUM_FTZ_RND0 :
			COLL_OPCODE_FLT_SUM_NOFTZ_RND0;
		break;
	default:
		CXIP_FATAL("Invalid fegetround() return = %d\n", rnd);
	}
}

int cxip_fi2cxi_opcode(int op, int datatype)
{
	int opcode;

	switch (datatype) {
	case FI_UINT8:
	case FI_UINT16:
	case FI_UINT32:
		opcode = _int8_16_32_op_to_opcode[op];
		break;
	case FI_UINT64:
		opcode = _int64_op_to_opcode[op];
		break;
	case FI_DOUBLE:
		opcode = _flt_op_to_opcode[op];
		break;
	default:
		opcode = -FI_EOPNOTSUPP;
		break;
	}
	return opcode;
}

/* Determine datatype size */
static inline int _get_cxi_datasize(enum fi_datatype datatype, size_t count)
{
	int size;

	switch (datatype) {
	case FI_UINT8:
		size = sizeof(uint8_t);
		break;
	case FI_UINT16:
		size = sizeof(uint16_t);
		break;
	case FI_UINT32:
		size = sizeof(uint32_t);
		break;
	case FI_UINT64:
		size = sizeof(uint64_t);
		break;
	case FI_DOUBLE:
		size = sizeof(double);
		break;
	default:
		return -FI_EOPNOTSUPP;
	}
	size *= count;
	if (size > CXIP_COLL_MAX_TX_SIZE)
		return -FI_EINVAL;
	return size;
}

/* Find NIC address and convert to index in av_set.
 */
static int _nic_to_idx(struct cxip_av_set *av_set, uint32_t nic,
		       unsigned int *set_idx)
{
	struct cxip_addr addr;
	size_t size = sizeof(addr);
	int i, ret;

	for (i = 0; i < av_set->fi_addr_cnt; i++) {
		ret = fi_av_lookup(&av_set->cxi_av->av_fid,
				   av_set->fi_addr_ary[i],
				   &addr, &size);
		if (ret)
			return ret;
		if (nic == addr.nic) {
			*set_idx = i;
			return FI_SUCCESS;
		}
	}
	return -FI_EADDRNOTAVAIL;
}

/* Post a reduction completion request to the collective TX CQ.
 */
static void _post_coll_complete(struct cxip_coll_reduction *reduction)
{
	struct cxip_req *req;
	int ret;

	/* Indicates collective completion by writing to the endpoint TX CQ */
	req = reduction->op_inject_req;
	reduction->op_inject_req = NULL;
	if (req) {
		reduction->in_use = false;
		if (reduction->red_rc) {
			ret = cxip_cq_req_error(req, 0, FI_EIO,
						reduction->red_rc, NULL, 0);
		} else {
			ret = cxip_cq_req_complete(req);
		}
		if (ret < 0) {
			CXIP_WARN("Collective complete post: %d\n", ret);
		}
	}
}

/* Record only the first of multiple errors.
 */
static inline void _set_reduce_error(struct cxip_coll_reduction *reduction,
				     enum pkt_rc rc)
{
	if (!reduction->red_rc)
		reduction->red_rc = rc;
}

/* Perform a reduction on the root in software.
 */
static int _root_reduce(struct cxip_coll_reduction *reduction,
			struct red_pkt *pkt, uint32_t exp_count)
{
	union cxip_coll_data *red_data;
	int i;

	/* first packet to arrive (root or leaf) sets up the reduction */
	red_data = (union cxip_coll_data *)reduction->red_data;
	if (!reduction->red_init) {
		_zcopy_pkt_data(reduction->red_data, pkt->data.databuf,
				reduction->op_data_len);
		reduction->red_op = pkt->hdr.op;
		reduction->red_rc = pkt->hdr.rc;
		reduction->red_cnt = pkt->hdr.redcnt;
		reduction->red_init = true;
		goto out;
	}

	reduction->red_cnt += pkt->hdr.redcnt;
	if (pkt->hdr.rc != RC_SUCCESS) {
		_set_reduce_error(reduction, pkt->hdr.rc);
		goto out;
	}

	if (pkt->hdr.op != reduction->red_op) {
		_set_reduce_error(reduction, RC_OP_MISMATCH);
		goto out;
	}

	if (reduction->red_cnt > exp_count) {
		_set_reduce_error(reduction, RC_CONTR_OVERFLOW);
		goto out;
	}

	switch (reduction->red_op) {
	case COLL_OPCODE_BARRIER:
		break;
	case COLL_OPCODE_BIT_AND:
		for (i = 0; i < 4; i++)
			red_data->ival[i] &= pkt->data.ival[i];
		break;
	case COLL_OPCODE_BIT_OR:
		for (i = 0; i < 4; i++)
			red_data->ival[i] |= pkt->data.ival[i];
		break;
	case COLL_OPCODE_BIT_XOR:
		for (i = 0; i < 4; i++)
			red_data->ival[i] ^= pkt->data.ival[i];
		break;
	case COLL_OPCODE_INT_MIN:
		for (i = 0; i < 4; i++)
			if (red_data->ival[i] > pkt->data.ival[i])
				red_data->ival[i] = pkt->data.ival[i];
		break;
	case COLL_OPCODE_INT_MAX:
		for (i = 0; i < 4; i++)
			if (red_data->ival[i] < pkt->data.ival[i])
				red_data->ival[i] = pkt->data.ival[i];
		break;
	case COLL_OPCODE_INT_MINMAXLOC:
		if (red_data->ival[0] > pkt->data.ival[0]) {
			red_data->ival[0] = pkt->data.ival[0];
			red_data->ival[1] = pkt->data.ival[1];
		}
		if (red_data->ival[2] < pkt->data.ival[2]) {
			red_data->ival[2] = pkt->data.ival[2];
			red_data->ival[3] = pkt->data.ival[3];
		}
		break;
	case COLL_OPCODE_INT_SUM:
		for (i = 0; i < 4; i++)
			red_data->ival[i] += pkt->data.ival[i];
		break;
	case COLL_OPCODE_FLT_MIN:
		for (i = 0; i < 4; i++)
			if (red_data->fval[i] > pkt->data.fval[i])
				red_data->fval[i] = pkt->data.fval[i];
		break;
	case COLL_OPCODE_FLT_MAX:
		for (i = 0; i < 4; i++)
			if (red_data->fval[i] < pkt->data.fval[i])
				red_data->fval[i] = pkt->data.fval[i];
		break;
	case COLL_OPCODE_FLT_MINMAXLOC:
		if (red_data->fval[0] > pkt->data.fval[0]) {
			red_data->fval[0] = pkt->data.fval[0];
			red_data->fval[1] = pkt->data.fval[1];
		}
		if (red_data->fval[2] < pkt->data.fval[2]) {
			red_data->fval[2] = pkt->data.fval[2];
			red_data->fval[3] = pkt->data.fval[3];
		}
		break;
	case COLL_OPCODE_FLT_MINNUM:
		// TODO
		break;
	case COLL_OPCODE_FLT_MAXNUM:
		// TODO
		break;
	case COLL_OPCODE_FLT_MINMAXNUMLOC:
		// TODO
		break;
	case COLL_OPCODE_FLT_SUM_NOFTZ_RND0:
	case COLL_OPCODE_FLT_SUM_NOFTZ_RND1:
	case COLL_OPCODE_FLT_SUM_NOFTZ_RND2:
	case COLL_OPCODE_FLT_SUM_NOFTZ_RND3:
	case COLL_OPCODE_FLT_SUM_FTZ_RND0:
	case COLL_OPCODE_FLT_SUM_FTZ_RND1:
	case COLL_OPCODE_FLT_SUM_FTZ_RND2:
	case COLL_OPCODE_FLT_SUM_FTZ_RND3:
		/* Rosetta opcode has been chosen according to the current
		 * rounding mode for this application, so all we need to do is
		 * add the numbers.
		 */
		for (i = 0; i < 4; i++)
			red_data->fval[i] += pkt->data.fval[i];
		break;
	case COLL_OPCODE_FLT_REPSUM:
		// TODO
		break;
	}
out:
	return (reduction->red_cnt < exp_count) ? -1 : 0;
}

/****************************************************************************
 * Collective State Machine
 *
 * The basic flow is:
 *   - all nodes reach a reduction call (at different times)
 *   - leaf nodes send their data, to be reduced, and block, polling CQ
 *   - root node prepares for the reduction, and blocks, polling CQ
 *   - root node receives leaf packets and reduces them, until all received
 *   - root node sends Arm Packet with results, and unblocks
 *   - leaf nodes receive Arm Packet with results, and unblock
 *
 * The Rosetta acceleration comes from the Arm Packet, which speculatively
 * arms the Rosetta tree for the NEXT operation. This persists until a
 * timeout expires. The timeout is specified when the multicast tree is created
 * by the Rosetta configuration service.
 *
 * If the next collective operation occurs within the timeout, the leaf results
 * will be reduced in reduction engines by Rosetta as they move up the tree,
 * reducing the number of packets received by the root.
 *
 * If the reduction engine times out with partial results, it forwards the
 * partial results, and all subsequent results are passed directly to the next
 * Rosetta.
 *
 * There are eight reduction_id values, which can be used to acquire and use
 * up to eight independent reduction engines (REs) at each upstream port of each
 * Rosetta switch.
 *
 * We use a round-robin selection of reduction id values.
 * Consider:
 *
 *   MPI_Ireduce(..., &req0) // uses reduction id 0
 *   sleep(random)
 *   MPI_Ireduce(..., &req1) // uses reduction id 1
 *   sleep(random)
 *   MPI_Wait(..., &req0)
 *   MPI_Ireduce(..., &req3)
 *
 * On all nodes, these calls must use the SAME reduction id for each reduction.
 * There is no libfabric syntax for specifying this on the reduction call, short
 * of having a separate multicast object for each reduction ID. In principle, we
 * know that req3 cannot begin on any node until it has verified that req0 has
 * completed on that node: thus, we could reuse reduction id 0.
 *
 * But we have no way of knowing in libfabric that MPI_Wait() was even called,
 * much less whether it was called on req0 or req1. With that knowledge, we
 * could safely re-use reduction id 0. Without that knowledge, we have to
 * continue to advance the reduction id.
 *
 * We cannot rely upon our internal detection of req0 completion. Although this
 * is broadcast from the root to all leaves with a single write, there will be
 * delays in each Rosetta due to packet retransmission, so every leaf will
 * receive the completion packet at slightly different times, potentially
 * resulting in req3 using different reduction ID values.
 */

static inline bool _root_retry_required(struct cxip_coll_reduction *reduction)
{
	// TODO
	return false;
}

static inline
struct red_pkt * _copy_user_to_pkt(void *packet,
				   struct cxip_coll_reduction *reduction)
{
	struct red_pkt *rootpkt = (struct red_pkt *)packet;

	rootpkt->hdr.redcnt = 1;
	rootpkt->hdr.seqno = reduction->seqno;
	rootpkt->hdr.resno = reduction->seqno;
	rootpkt->hdr.rc = RC_SUCCESS;
	rootpkt->hdr.op = reduction->op_code;
	_zcopy_pkt_data(rootpkt->data.databuf, reduction->op_send_data,
			reduction->op_data_len);

	return rootpkt;
}

static inline
void _copy_pkt_to_user(struct cxip_coll_reduction *reduction,
		       struct red_pkt *pkt)
{
	if (reduction->op_rslt_data && reduction->op_data_len) {
		memcpy(reduction->op_rslt_data, pkt->data.databuf,
		       reduction->op_data_len);
	}
}

static inline
void _copy_result_to_user(struct cxip_coll_reduction *reduction)
{
	if (reduction->op_rslt_data && reduction->op_data_len) {
		memcpy(reduction->op_rslt_data, reduction->red_data,
		       reduction->op_data_len);
	}
}

/* Root node state machine.
 * !pkt means this is progressing from injection call (e.g. fi_reduce())
 *  pkt means this is progressing from event callback (leaf packet)
 */
static void _progress_root(struct cxip_coll_reduction *reduction,
			   struct red_pkt *pkt)
{
	struct red_pkt *rootpkt = (struct red_pkt *)reduction->tx_msg;
	ssize_t ret;

	/* Drop packets until root is initialized. */
	if (reduction->op_state == CXIP_COLL_STATE_NONE)
		return;

	/* Initial state for root is BLOCKED. The root makes the transition from
	 * BLOCKED to READY to BLOCKED in a single pass through this code, so we
	 * do not set or check the state.
	 */

	if (!pkt) {
		/* 'Receive' data packet with initial root data */
		pkt = _copy_user_to_pkt(reduction->tx_msg, reduction);
	} else {
		/* Drop old packets */
		if (pkt->hdr.resno != reduction->seqno) {
			ofi_atomic_inc32(&reduction->mc_obj->seq_err_cnt);
			return;
		}

		/* process a retry request from CQ polling */
		if (_root_retry_required(reduction)) {
			CXIP_DBG("RETRY collective packet\n");

			/* empty retry packet auto-advances the seqno */
			ret = cxip_coll_send_red_pkt(reduction, 0, 0, NULL, 0,
						     true);
			if (ret) {
				/* fatal send error, collectives broken */
				CXIP_WARN("Collective send: %ld\n", ret);
				reduction->red_rc = ret;
				_post_coll_complete(reduction);
				reduction->op_state = CXIP_COLL_STATE_NONE;
				return;
			}

			/* start reduction over */
			reduction->red_init = false;
			pkt = _copy_user_to_pkt(reduction->tx_msg, reduction);
		}
	}

	/* initialize or add to reduction */
	ret = _root_reduce(reduction, pkt,
			   reduction->mc_obj->av_set->fi_addr_cnt);

	/* return != 0 means more work to do, wait for new packets */
	if (ret != 0)
		return;

	/* reduction->op_state = CXIP_COLL_STATE_READY, not necessary */
	rootpkt->hdr.rc = reduction->red_rc;

	/* copy reduction result to root user response buffer */
	_copy_result_to_user(reduction);

	/* send reduction result to leaves */
	ret = cxip_coll_send_red_pkt(reduction,
				     rootpkt->hdr.redcnt,
				     reduction->op_code,
				     reduction->op_rslt_data,
				     reduction->op_data_len,
				     false);
	if (ret) {
		/* fatal send error, leaves are hung */
		CXIP_WARN("Collective send: %ld\n", ret);
		reduction->red_rc = ret;
		_post_coll_complete(reduction);
		reduction->op_state = CXIP_COLL_STATE_NONE;
		return;
	}

	/* Done with reduction, will re-initialize on next use */
	reduction->red_init = false;

	/* Reduction completed on root */
	_post_coll_complete(reduction);

	/* reduction->op_state = CXIP_COLL_STATE_BLOCKED, not necessary */
}

/* Leaf node state machine.
 * !pkt means this is progressing from injection call (e.g. fi_reduce())
 *  pkt means this is progressing from event callback (receipt of packet)
 */
static void _progress_leaf(struct cxip_coll_reduction *reduction,
			   struct red_pkt *pkt)
{
	union cxip_coll_cookie cookie;
	int ret;

	if (reduction->op_state == CXIP_COLL_STATE_NONE)
		return;

	/* initial state for leaf is always READY */

	if (!pkt) {
		/* Application called a collective operation. We can't get here
		 * if reduction channel isn't in the READY state, because
		 * it would not have been possible to acquire a reduction
		 * channel. So no state check is required.
		 */
		ret = cxip_coll_send_red_pkt(reduction, 1,
					     reduction->op_code,
					     reduction->op_send_data,
					     reduction->op_data_len, false);
		if (ret) {
			/* fatal send error, root will time out and retry */
			CXIP_WARN("Collective send: %d\n", ret);
			return;
		}
		reduction->op_state = CXIP_COLL_STATE_BLOCKED;
	} else {
		/* Extract sequence number for next response */
		reduction->seqno = pkt->hdr.seqno;
		cookie.raw = pkt->cookie;
		if (cookie.retry) {
			// TODO -- this needs to be expanded, see design
			/* Send the previous packet with new seqno */
			CXIP_DBG("leaf sending retry packet\n");
			pkt = (struct red_pkt *)&reduction->tx_msg;
			pkt->redhdr = be64toh(pkt->redhdr);
			pkt->hdr.seqno = reduction->seqno;
			pkt->redhdr = htobe64(pkt->redhdr);
			_send_pkt(reduction, true);
			/* do not change state, wait for next ARM */
			return;
		}

		/* We can't get to here unless we are in the BLOCKED state. The
		 * root node can send unsolicited RETRY packets, but it cannot
		 * send unsolicited non-RETRY packets. A solicited packet means
		 * that all of the leaf nodes have sent data, including this
		 * one, meaning we must be in the BLOCKED state. So no state
		 * check is required.
		 */

		/* Capture final reduction data */
		reduction->red_rc = pkt->hdr.rc;
		_copy_pkt_to_user(reduction, pkt);

		/* Reduction completed on leaf */
		_post_coll_complete(reduction);
		reduction->op_state = CXIP_COLL_STATE_READY;
	}
}

/* Root or leaf progress state machine.
 */
static void _progress_coll(struct cxip_coll_reduction *reduction,
			   struct red_pkt *pkt)
{
	if (is_hw_root(reduction->mc_obj))
		_progress_root(reduction, pkt);
	else
		_progress_leaf(reduction, pkt);
}

/* Generic collective request injection.
 */
ssize_t cxip_coll_inject(struct cxip_coll_mc *mc_obj,
			 enum fi_datatype datatype, int cxi_opcode,
			 const void *op_send_data, void *op_rslt_data,
			 size_t op_count, void *context, int *reduction_id)
{
	struct cxip_coll_reduction *reduction;
	int red_id;
	int size;

	if (!mc_obj->is_joined)
		return -FI_EOPBADSTATE;

	size = _get_cxi_datasize(datatype, op_count);
	if (size < 0)
		return size;

	/* Acquire reduction ID */
	fastlock_acquire(&mc_obj->lock);
	red_id = mc_obj->next_red_id;
	if (mc_obj->reduction[red_id].in_use) {
		fastlock_release(&mc_obj->lock);
		return -FI_EBUSY;
	}
	mc_obj->reduction[red_id].in_use = true;
	mc_obj->next_red_id++;
	mc_obj->next_red_id &= 0x7;
	fastlock_release(&mc_obj->lock);

	/* Pass reduction parameters through the reduction structure */
	reduction = &mc_obj->reduction[red_id];
	reduction->op_code = cxi_opcode;
	reduction->op_send_data = op_send_data;
	reduction->op_rslt_data = op_rslt_data;
	reduction->op_data_len = size;
	reduction->op_context = context;
	reduction->op_inject_req = cxip_cq_req_alloc(mc_obj->ep_obj->coll.tx_cq,
						     1, reduction);
	if (! reduction->op_inject_req) {
		reduction->in_use = false;
		return -FI_ENOMEM;
	}
	reduction->op_inject_req->context = (uint64_t)context;

	if (reduction_id)
		*reduction_id = red_id;

	_progress_coll(reduction, NULL);
	return FI_SUCCESS;
}

ssize_t cxip_barrier(struct fid_ep *ep, fi_addr_t coll_addr, void *context)
{
	struct cxip_ep *cxi_ep;
	struct cxip_coll_mc *mc_obj;
	int ret;

	cxi_ep = container_of(ep, struct cxip_ep, ep.fid);
	mc_obj = (struct cxip_coll_mc *) ((uintptr_t) coll_addr);

	if (mc_obj->ep_obj != cxi_ep->ep_obj) {
		CXIP_INFO("bad coll_addr\n");
		return -FI_EINVAL;
	}

	/* Use special opcode of -1 for barrier */
	ret = cxip_coll_inject(mc_obj, FI_UINT64, COLL_OPCODE_BARRIER,
			       NULL, NULL, 0, context, NULL);

	return ret;
}

/* NOTE: root_addr is index of node in fi_av_set list, i.e. local rank */
ssize_t cxip_broadcast(struct fid_ep *ep, void *buf, size_t count,
		       void *desc, fi_addr_t coll_addr, fi_addr_t root_addr,
		       enum fi_datatype datatype, uint64_t flags,
		       void *context)
{
	struct cxip_ep *cxi_ep;
	struct cxip_coll_mc *mc_obj;
	uint8_t src[CXIP_COLL_MAX_TX_SIZE];
	int size;
	int ret;

	cxi_ep = container_of(ep, struct cxip_ep, ep.fid);
	mc_obj = (struct cxip_coll_mc *) ((uintptr_t) coll_addr);

	if (mc_obj->ep_obj != cxi_ep->ep_obj) {
		CXIP_INFO("bad coll_addr\n");
		return -FI_EINVAL;
	}

	size = _get_cxi_datasize(datatype, count);
	if (size < 0)
		return size;

	/* only root node contributes data */
	memset(src, 0, sizeof(src));
	if (root_addr == mc_obj->mynode_index)
		memcpy(src, buf, size);

	ret = cxip_coll_inject(mc_obj, datatype, COLL_OPCODE_BIT_OR, src, buf,
			       count, context, NULL);
	return ret;
}

/* NOTE: root_addr is index of node in fi_av_set list, i.e. local rank */
ssize_t cxip_reduce(struct fid_ep *ep, const void *buf, size_t count,
		    void *desc, void *result, void *result_desc,
		    fi_addr_t coll_addr, fi_addr_t root_addr,
		    enum fi_datatype datatype, enum fi_op op, uint64_t flags,
		    void *context)
{
	struct cxip_ep *cxi_ep;
	struct cxip_coll_mc *mc_obj;
	int cxi_opcode;
	int ret;

	cxi_ep = container_of(ep, struct cxip_ep, ep.fid);
	mc_obj = (struct cxip_coll_mc *) ((uintptr_t) coll_addr);

	if (mc_obj->ep_obj != cxi_ep->ep_obj) {
		CXIP_INFO("bad coll_addr\n");
		return -FI_EINVAL;
	}

	if (root_addr != mc_obj->mynode_index)
		result = NULL;

	cxi_opcode = cxip_fi2cxi_opcode(op, datatype);
	if (cxi_opcode < 0) {
		CXIP_INFO("bad opcode %d\n", op);
		return cxi_opcode;
	}

	ret = cxip_coll_inject(mc_obj, datatype, cxi_opcode, buf, result,
			       count, context, NULL);

	return ret;
}

ssize_t cxip_allreduce(struct fid_ep *ep, const void *buf, size_t count,
		       void *desc, void *result, void *result_desc,
		       fi_addr_t coll_addr, enum fi_datatype datatype,
		       enum fi_op op, uint64_t flags, void *context)
{
	struct cxip_ep *cxi_ep;
	struct cxip_coll_mc *mc_obj;
	int cxi_opcode;
	int ret;

	cxi_ep = container_of(ep, struct cxip_ep, ep.fid);
	mc_obj = (struct cxip_coll_mc *) ((uintptr_t) coll_addr);

	if (mc_obj->ep_obj != cxi_ep->ep_obj)
		return -FI_EINVAL;

	cxi_opcode = cxip_fi2cxi_opcode(op, datatype);
	if (cxi_opcode < 0) {
		CXIP_INFO("bad opcode %d\n", op);
		return cxi_opcode;
	}

	ret = cxip_coll_inject(mc_obj, datatype, cxi_opcode, buf, result,
			       count, context, NULL);

	return ret;
}

struct fi_ops_collective cxip_collective_ops = {
	.size = sizeof(struct fi_ops_collective),
	.barrier = cxip_barrier,
	.broadcast = cxip_broadcast,
	.alltoall = fi_coll_no_alltoall,
	.allreduce = cxip_allreduce,
	.allgather = fi_coll_no_allgather,
	.reduce_scatter = fi_coll_no_reduce_scatter,
	.reduce = cxip_reduce,
	.scatter = fi_coll_no_scatter,
	.gather = fi_coll_no_gather,
	.msg = fi_coll_no_msg,
};

struct fi_ops_collective cxip_no_collective_ops = {
	.size = sizeof(struct fi_ops_collective),
	.barrier = fi_coll_no_barrier,
	.broadcast = fi_coll_no_broadcast,
	.alltoall = fi_coll_no_alltoall,
	.allreduce = fi_coll_no_allreduce,
	.allgather = fi_coll_no_allgather,
	.reduce_scatter = fi_coll_no_reduce_scatter,
	.reduce = fi_coll_no_reduce,
	.scatter = fi_coll_no_scatter,
	.gather = fi_coll_no_gather,
	.msg = fi_coll_no_msg,
};

/****************************************************************************
 * Collective join operation.
 */

/* Close a multicast object.
 */
static int _close_mc(struct fid *fid)
{
	struct cxip_coll_mc *mc_obj;
	int ret;

	mc_obj = container_of(fid, struct cxip_coll_mc, mc_fid.fid);

	do {
		ret = _coll_pte_disable(mc_obj->coll_pte);
	} while (ret == -FI_EAGAIN);

	_coll_destroy_buffers(mc_obj->coll_pte);
	cxip_pte_free(mc_obj->coll_pte->pte);
	free(mc_obj->coll_pte);

	mc_obj->av_set->mc_obj = NULL;
	ofi_atomic_dec32(&mc_obj->av_set->ref);
	ofi_atomic_dec32(&mc_obj->ep_obj->coll.mc_count);
	free(mc_obj);

	return FI_SUCCESS;
}

static struct fi_ops mc_ops = {
	.size = sizeof(struct fi_ops),
	.close = _close_mc,
};

/* Allocate a multicast object.
 */
static int _alloc_mc(struct cxip_ep_obj *ep_obj, struct cxip_av_set *av_set,
		     struct cxip_coll_mc **mc)
{
	static int unicast_idcode = 0;

	struct cxi_pt_alloc_opts pt_opts = {
		.use_long_event = 1,
		.do_space_check = 1,
		.en_restricted_unicast_lm = 1,
	};
	struct cxip_coll_mc *mc_obj;
	struct cxip_coll_pte *coll_pte;
	bool is_multicast;
	uint32_t mc_unique;
	uint64_t pid_idx;
	unsigned int hwroot_idx;
	unsigned int mynode_idx;
	int red_id;
	int state;
	int ret;

	/* remapping is not allowed */
	if (av_set->mc_obj) {
		CXIP_INFO("remap not allowed\n");
		return -FI_EINVAL;
	}

	/* PTE receive address */
	switch (av_set->comm_key.type) {
	case COMM_KEY_MULTICAST:
		if (is_netsim(ep_obj)) {
			CXIP_INFO("NETSIM does not support mcast\n");
			return -FI_EINVAL;
		}
		is_multicast = true;
		pid_idx = av_set->comm_key.mcast.mcast_id;
		mc_unique = av_set->comm_key.mcast.mcast_ref;
		hwroot_idx = av_set->comm_key.mcast.hwroot_idx;
		ret = _nic_to_idx(av_set, ep_obj->src_addr.nic, &mynode_idx);
		if (ret)
			return ret;
		break;
	case COMM_KEY_UNICAST:
		is_multicast = false;
		pid_idx = CXIP_PTL_IDX_COLL;
		fastlock_acquire(&ep_obj->lock);
		mc_unique = unicast_idcode++;
		fastlock_release(&ep_obj->lock);
		ret = _nic_to_idx(av_set, av_set->comm_key.ucast.hwroot_idx,
				  &hwroot_idx);
		if (ret)
			return ret;
		ret = _nic_to_idx(av_set, ep_obj->src_addr.nic, &mynode_idx);
		if (ret)
			return ret;
		break;
	case COMM_KEY_RANK:
		is_multicast = false;
		pid_idx = CXIP_PTL_IDX_COLL + av_set->comm_key.rank.rank;
		mc_unique = av_set->comm_key.rank.rank;
		hwroot_idx = av_set->comm_key.rank.hwroot_idx;
		if (hwroot_idx >= av_set->fi_addr_cnt) {
			CXIP_INFO("hwroot_idx out of range: %d\n",
				  hwroot_idx);
			return -FI_EINVAL;
		}
		mynode_idx = av_set->comm_key.rank.rank;
		if (mynode_idx >= av_set->fi_addr_cnt) {
			CXIP_INFO("mynode_idx out of range: %d\n",
				  mynode_idx);
			return -FI_EINVAL;
		}
		break;
	default:
		CXIP_INFO("unexpected comm_key type: %d\n",
			  av_set->comm_key.type);
		return -FI_EINVAL;
	}

	ret = -FI_ENOMEM;
	mc_obj = calloc(1, sizeof(*av_set->mc_obj));
	if (!mc_obj)
		return ret;

	coll_pte = calloc(1, sizeof(*coll_pte));
	if (!coll_pte)
		goto free_mc_obj;

	dlist_init(&coll_pte->buf_list);
	coll_pte->ep_obj = ep_obj;
	ofi_atomic_initialize32(&coll_pte->buf_cnt, 0);
	ofi_atomic_initialize32(&coll_pte->buf_swap_cnt, 0);

	ret = cxip_pte_alloc(ep_obj->if_dom[0], ep_obj->coll.rx_cq->evtq,
			     pid_idx, is_multicast, &pt_opts, _coll_pte_cb,
			     coll_pte, &coll_pte->pte);
	if (ret)
		goto free_coll_pte;

	ret = _coll_pte_enable(coll_pte, CXIP_PTE_IGNORE_DROPS);
	if (ret)
		goto coll_pte_destroy;

	ret = _coll_add_buffers(coll_pte,
				ep_obj->coll.buffer_size,
				ep_obj->coll.buffer_count);
	if (ret)
		goto coll_pte_disable;

	mc_obj->mc_fid.fid.fclass = FI_CLASS_MC;
	mc_obj->mc_fid.fid.context = mc_obj;
	mc_obj->mc_fid.fid.ops = &mc_ops;
	mc_obj->coll_pte = coll_pte;
	mc_obj->ep_obj = ep_obj;
	mc_obj->av_set = av_set;
	mc_obj->mc_unique = mc_unique;
	mc_obj->hwroot_index = hwroot_idx;
	mc_obj->mynode_index = mynode_idx;
	mc_obj->is_joined = true;
	state = is_hw_root(mc_obj) ?
		CXIP_COLL_STATE_BLOCKED :
		CXIP_COLL_STATE_READY;
	for (red_id = 0; red_id < CXIP_COLL_MAX_CONCUR; red_id++) {
		struct cxip_coll_reduction *reduction;

		reduction = &mc_obj->reduction[red_id];
		reduction->op_state = state;
		reduction->mc_obj = mc_obj;
		reduction->red_id = red_id;
		reduction->in_use = false;
	}
	fastlock_init(&mc_obj->lock);
	ofi_atomic_initialize32(&mc_obj->send_cnt, 0);
	ofi_atomic_initialize32(&mc_obj->recv_cnt, 0);
	ofi_atomic_initialize32(&mc_obj->pkt_cnt, 0);
	ofi_atomic_initialize32(&mc_obj->seq_err_cnt, 0);

	ofi_atomic_inc32(&ep_obj->coll.mc_count);
	ofi_atomic_inc32(&av_set->ref);
	av_set->mc_obj = mc_obj;
	coll_pte->mc_obj = mc_obj;

	*mc = av_set->mc_obj;

	return FI_SUCCESS;

coll_pte_disable:
	_coll_pte_disable(coll_pte);
coll_pte_destroy:
	cxip_pte_free(coll_pte->pte);
free_coll_pte:
	free(coll_pte);
free_mc_obj:
	free(mc_obj);
	return ret;
}

void cxip_coll_reset_mc_ctrs(struct cxip_coll_mc *mc_obj)
{
	fastlock_acquire(&mc_obj->lock);
	ofi_atomic_set32(&mc_obj->send_cnt, 0);
	ofi_atomic_set32(&mc_obj->recv_cnt, 0);
	ofi_atomic_set32(&mc_obj->pkt_cnt, 0);
	ofi_atomic_set32(&mc_obj->seq_err_cnt, 0);
	fastlock_release(&mc_obj->lock);
}

/**
 * fi_join_collective() implementation.
 *
 * @param ep - endpoint
 * @param addr - pointer to struct fi_collective_addr
 * @param flags - collective flags
 * @param mc - returned multicast object
 * @param context - user-defined context
 *
 * @return int - return code
 */
int cxip_join_collective(struct fid_ep *ep, fi_addr_t coll_addr,
			 const struct fid_av_set *coll_av_set,
			 uint64_t flags, struct fid_mc **mc, void *context)
{
	struct cxip_ep *cxi_ep;
	struct cxip_av_set *av_set;
	struct cxip_coll_mc *mc_obj;
	int ret;

	cxi_ep = container_of(ep, struct cxip_ep, ep.fid);
	av_set = container_of(coll_av_set, struct cxip_av_set, av_set_fid);

	if (!cxi_ep->ep_obj->coll.enabled)
		return -FI_EOPBADSTATE;

	if (coll_addr != FI_ADDR_NOTAVAIL) {
		CXIP_INFO("coll_addr is not FI_ADDR_NOTAVAIL\n");
		return -FI_EINVAL;
	} else if (av_set->comm_key.type == COMM_KEY_NONE) {
		CXIP_INFO("comm_key not specified\n");
		return -FI_EINVAL;
	}

	ret = _alloc_mc(cxi_ep->ep_obj, av_set, &mc_obj);
	if (ret)
		return ret;

	ret = _post_join_complete(mc_obj, context);
	if (ret)
		return ret;

	*mc = &mc_obj->mc_fid;

	return FI_SUCCESS;
}

/* Exported through fi_cxi_ext.h. Generates only MULTICAST comm_keys.
 */
size_t cxip_coll_init_mcast_comm_key(struct cxip_coll_comm_key *comm_key,
				     uint32_t mcast_ref,
				     uint32_t mcast_id,
				     uint32_t hwroot_idx)
{
	struct cxip_comm_key *key = (struct cxip_comm_key *)comm_key;

	key->type = COMM_KEY_MULTICAST;
	key->mcast.mcast_ref = mcast_ref;
	key->mcast.mcast_id = mcast_id;
	key->mcast.hwroot_idx = hwroot_idx;

	return sizeof(struct cxip_comm_key);
}
