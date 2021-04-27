/*
 * Copyright (c) 2017-2020 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <sys/types.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_trigger.h>

#include <ofi.h>
#include <ofi_enosys.h>
#include <ofi_rbuf.h>
#include <ofi_list.h>
#include <ofi_signal.h>
#include <ofi_util.h>
#include <ofi_proto.h>
#include <ofi_net.h>

#ifndef _TCP_H_
#define _TCP_H_


#define TCPX_MAX_INJECT		128
#define MAX_POLL_EVENTS		100
#define TCPX_MIN_MULTI_RECV	16384
#define TCPX_PORT_MAX_RANGE	(USHRT_MAX)

extern struct fi_provider	tcpx_prov;
extern struct util_prov		tcpx_util_prov;
extern struct fi_info		tcpx_info;
extern struct tcpx_port_range	port_range;
extern int tcpx_nodelay;
extern int tcpx_staging_sbuf_size;
extern int tcpx_prefetch_rbuf_size;

struct tcpx_xfer_entry;
struct tcpx_ep;


/*
 * Wire protocol structures and definitions
 */

#define TCPX_CTRL_HDR_VERSION	3

enum {
	TCPX_MAX_CM_DATA_SIZE = (1 << 8)
};

struct tcpx_cm_msg {
	struct ofi_ctrl_hdr hdr;
	char data[TCPX_MAX_CM_DATA_SIZE];
};

#define TCPX_HDR_VERSION	3

enum {
	TCPX_IOV_LIMIT = 4
};

enum tcpx_op_code {
	TCPX_OP_MSG_SEND,
	TCPX_OP_MSG_RECV,
	TCPX_OP_MSG_RESP,
	TCPX_OP_WRITE,
	TCPX_OP_REMOTE_WRITE,
	TCPX_OP_READ_REQ,
	TCPX_OP_READ_RSP,
	TCPX_OP_REMOTE_READ,
	TCPX_OP_CODE_MAX,
};

/* Flags */
#define TCPX_REMOTE_CQ_DATA	(1 << 0)
/* not used TCPX_TRANSMIT_COMPLETE	(1 << 1) */
#define TCPX_DELIVERY_COMPLETE	(1 << 2)
#define TCPX_COMMIT_COMPLETE	(1 << 3)
#define TCPX_TAGGED		(1 << 7)

struct tcpx_base_hdr {
	uint8_t			version;
	uint8_t			op;
	uint16_t		flags;
	uint8_t			op_data;
	uint8_t			rma_iov_cnt;
	uint8_t			payload_off;
	uint8_t			rsvd;
	uint64_t		size;
};

struct tcpx_tag_hdr {
	struct tcpx_base_hdr	base_hdr;
	uint64_t		tag;
};

struct tcpx_cq_data_hdr {
	struct tcpx_base_hdr 	base_hdr;
	uint64_t		cq_data;
};

struct tcpx_tag_data_hdr {
	struct tcpx_cq_data_hdr	cq_data_hdr;
	uint64_t		tag;
};

/* Maximum header is scatter RMA with CQ data */
#define TCPX_MAX_HDR (sizeof(struct tcpx_cq_data_hdr) + \
		     sizeof(struct ofi_rma_iov) * TCPX_IOV_LIMIT)

/*
 * End wire protocol definitions
 */


enum tcpx_cm_state {
	TCPX_CM_LISTENING,
	TCPX_CM_CONNECTING,
	TCPX_CM_WAIT_REQ,
	TCPX_CM_REQ_SENT,
	TCPX_CM_REQ_RVCD,
	TCPX_CM_RESP_READY,
	/* CM context is freed once connected */
};

struct tcpx_cm_context {
	fid_t			fid;
	enum tcpx_cm_state	state;
	size_t			cm_data_sz;
	struct tcpx_cm_msg	msg;
};

struct tcpx_port_range {
	int high;
	int low;
};

struct tcpx_conn_handle {
	struct fid		handle;
	struct tcpx_pep		*pep;
	SOCKET			sock;
	bool			endian_match;
};

struct tcpx_pep {
	struct util_pep 	util_pep;
	struct fi_info		*info;
	SOCKET			sock;
	struct tcpx_cm_context	cm_ctx;
};

enum tcpx_state {
	TCPX_IDLE,
	TCPX_CONNECTING,
	TCPX_RCVD_REQ,
	TCPX_ACCEPTING,
	TCPX_CONNECTED,
	TCPX_DISCONNECTED,
};

struct tcpx_cur_rx_msg {
	union {
		struct tcpx_base_hdr	base_hdr;
		uint8_t			max_hdr[TCPX_MAX_HDR];
	} hdr;
	size_t			hdr_len;
	size_t			done_len;
};

struct tcpx_rx_ctx {
	struct fid_ep		rx_fid;
	struct slist		rx_queue;
	struct ofi_bufpool	*buf_pool;
	uint64_t		op_flags;
	fastlock_t		lock;
};

typedef int (*tcpx_rx_process_fn_t)(struct tcpx_xfer_entry *rx_entry);

struct tcpx_ep {
	struct util_ep		util_ep;
	struct ofi_bsock	bsock;
	struct tcpx_cur_rx_msg	cur_rx_msg;
	struct tcpx_xfer_entry	*cur_rx_entry;
	size_t			rem_rx_len;
	tcpx_rx_process_fn_t 	cur_rx_proc_fn;
	struct tcpx_xfer_entry	*cur_tx_entry;
	size_t			rem_tx_len;
	struct dlist_entry	ep_entry;
	struct slist		rx_queue;
	struct slist		tx_queue;
	struct slist		tx_rsp_pend_queue;
	struct slist		rma_read_queue;
	struct tcpx_rx_ctx	*srx_ctx;
	enum tcpx_state		state;
	/* lock for protecting tx/rx queues, rma list, state*/
	fastlock_t		lock;
	int (*start_op[ofi_op_write + 1])(struct tcpx_ep *ep);
	void (*hdr_bswap)(struct tcpx_base_hdr *hdr);
	size_t			min_multi_recv_size;
	bool			pollout_set;
};

struct tcpx_fabric {
	struct util_fabric	util_fabric;
};

#define TCPX_NEED_DYN_RBUF 	BIT_ULL(61)

struct tcpx_xfer_entry {
	struct slist_entry	entry;
	union {
		struct tcpx_base_hdr	base_hdr;
		struct tcpx_cq_data_hdr cq_data_hdr;
		struct tcpx_tag_data_hdr tag_data_hdr;
		struct tcpx_tag_hdr	tag_hdr;
		uint8_t		       	max_hdr[TCPX_MAX_HDR + TCPX_MAX_INJECT];
	} hdr;
	size_t			iov_cnt;
	struct iovec		iov[TCPX_IOV_LIMIT+1];
	struct tcpx_ep		*ep;
	uint64_t		flags;
	void			*context;
	void			*mrecv_msg_start;
};

struct tcpx_domain {
	struct util_domain		util_domain;
	struct ofi_ops_dynamic_rbuf	*dynamic_rbuf;
};

static inline struct ofi_ops_dynamic_rbuf *tcpx_dynamic_rbuf(struct tcpx_ep *ep)
{
	struct tcpx_domain *domain;

	domain = container_of(ep->util_ep.domain, struct tcpx_domain,
			      util_domain);
	return domain->dynamic_rbuf;
}

struct tcpx_buf_pool {
	struct ofi_bufpool	*pool;
	enum tcpx_op_code	op_type;
};

struct tcpx_cq {
	struct util_cq		util_cq;
	/* buf_pools protected by util.cq_lock */
	struct tcpx_buf_pool	buf_pools[TCPX_OP_CODE_MAX];
};

struct tcpx_eq {
	struct util_eq		util_eq;
	/*
	  The following lock avoids race between ep close
	  and connection management code.
	 */
	fastlock_t		close_lock;
};

int tcpx_create_fabric(struct fi_fabric_attr *attr,
		       struct fid_fabric **fabric,
		       void *context);

int tcpx_passive_ep(struct fid_fabric *fabric, struct fi_info *info,
		    struct fid_pep **pep, void *context);

int tcpx_set_port_range(void);

int tcpx_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **domain, void *context);


int tcpx_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep_fid, void *context);
void tcpx_ep_disable(struct tcpx_ep *ep, int cm_err);


int tcpx_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		 struct fid_cq **cq_fid, void *context);
void tcpx_cq_report_success(struct util_cq *cq,
			    struct tcpx_xfer_entry *xfer_entry);
void tcpx_cq_report_error(struct util_cq *cq,
			  struct tcpx_xfer_entry *xfer_entry,
			  int err);
void tcpx_get_cq_info(struct tcpx_xfer_entry *entry, uint64_t *flags,
		      uint64_t *data, uint64_t *tag);

ssize_t tcpx_recv_hdr(struct tcpx_ep *ep);
int tcpx_recv_msg_data(struct tcpx_ep *ep);
int tcpx_send_msg(struct tcpx_ep *ep);

struct tcpx_xfer_entry *tcpx_xfer_entry_alloc(struct tcpx_cq *cq,
					      enum tcpx_op_code type);
struct tcpx_xfer_entry *tcpx_srx_entry_alloc(struct tcpx_rx_ctx *srx_ctx,
					     struct tcpx_ep *ep);
void tcpx_xfer_entry_free(struct tcpx_cq *tcpx_cq,
			  struct tcpx_xfer_entry *xfer_entry);
void tcpx_srx_entry_free(struct tcpx_rx_ctx *srx_ctx,
			 struct tcpx_xfer_entry *xfer_entry);
void tcpx_rx_entry_free(struct tcpx_xfer_entry *rx_entry);
void tcpx_reset_rx(struct tcpx_ep *ep);

void tcpx_progress_tx(struct tcpx_ep *ep);
void tcpx_progress_rx(struct tcpx_ep *ep);
int tcpx_try_func(void *util_ep);

void tcpx_hdr_none(struct tcpx_base_hdr *hdr);
void tcpx_hdr_bswap(struct tcpx_base_hdr *hdr);

void tcpx_tx_queue_insert(struct tcpx_ep *tcpx_ep,
			  struct tcpx_xfer_entry *tx_entry);

void tcpx_conn_mgr_run(struct util_eq *eq);
int tcpx_eq_wait_try_func(void *arg);
int tcpx_eq_create(struct fid_fabric *fabric_fid, struct fi_eq_attr *attr,
		   struct fid_eq **eq_fid, void *context);

int tcpx_op_invalid(struct tcpx_ep *tcpx_ep);
int tcpx_op_msg(struct tcpx_ep *tcpx_ep);
int tcpx_op_read_req(struct tcpx_ep *tcpx_ep);
int tcpx_op_write(struct tcpx_ep *tcpx_ep);
int tcpx_op_read_rsp(struct tcpx_ep *tcpx_ep);

#endif //_TCP_H_
