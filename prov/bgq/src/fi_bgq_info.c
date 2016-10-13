#include "rdma/bgq/fi_bgq.h"


int fi_bgq_set_default_info()
{
	struct fi_info *fi, *prev_fi;
	uint32_t ppn = Kernel_ProcessCount();

	/*
	 * See: fi_bgq_stx_init() for the number of mu injection fifos
	 * allocated for each tx context. Each rx context uses one
	 * mu injection fifo and one mu reception fifo.
	 */
	const unsigned tx_ctx_cnt = (((BGQ_MU_NUM_INJ_FIFO_GROUPS-1) * BGQ_MU_NUM_INJ_FIFOS_PER_GROUP) / 3) / ppn;

	/*
	 * The number of rx contexts on a node is the minimum of:
	 * 1. number of mu injection fifos on the node not used by tx contexts
	 * 2. total number mu reception fifos on the node
	 */
	const unsigned rx_ctx_cnt = MIN((((BGQ_MU_NUM_INJ_FIFO_GROUPS-1) * BGQ_MU_NUM_INJ_FIFOS_PER_GROUP) - (tx_ctx_cnt * ppn)), ((BGQ_MU_NUM_REC_FIFO_GROUPS-1) * BGQ_MU_NUM_REC_FIFOS_PER_GROUP)) / ppn;

	fi = fi_dupinfo(NULL);
	if (!fi) {
		errno = FI_ENOMEM;
		return -errno;
	}

	fi_bgq_global.info = fi;

	*fi->tx_attr = (struct fi_tx_attr) {
		.caps		= FI_RMA | FI_ATOMIC | FI_TRANSMIT_COMPLETE,
		.mode		= FI_ASYNC_IOV,
		.op_flags	= FI_TRANSMIT_COMPLETE,
		.msg_order	= FI_ORDER_SAS | FI_ORDER_WAW | FI_ORDER_RAW | FI_ORDER_RAR,
		.comp_order	= FI_ORDER_NONE,
		.inject_size	= FI_BGQ_INJECT_SIZE,
		.size		= FI_BGQ_TX_SIZE,
		.iov_limit	= SIZE_MAX,
		.rma_iov_limit  = 0
	};
	
	*fi->rx_attr = (struct fi_rx_attr) {
		.caps		= FI_RMA | FI_ATOMIC | FI_NAMED_RX_CTX,
		.mode		= FI_ASYNC_IOV,
		.op_flags	= 0,
		.msg_order	= 0,
		.comp_order	= FI_ORDER_NONE,
		.total_buffered_recv = FI_BGQ_TOTAL_BUFFERED_RECV,
		.size		= FI_BGQ_RX_SIZE,
		.iov_limit	= SIZE_MAX
	};

	*fi->ep_attr = (struct fi_ep_attr) {
		.type			= FI_EP_RDM,
		.protocol		= FI_BGQ_PROTOCOL,
		.protocol_version	= FI_BGQ_PROTOCOL_VERSION,
		.max_msg_size		= FI_BGQ_MAX_MSG_SIZE,
		.msg_prefix_size	= FI_BGQ_MAX_PREFIX_SIZE,
		.max_order_raw_size	= FI_BGQ_MAX_ORDER_RAW_SIZE,
		.max_order_war_size	= FI_BGQ_MAX_ORDER_WAR_SIZE,
		.max_order_waw_size	= FI_BGQ_MAX_ORDER_WAW_SIZE,
		.mem_tag_format		= FI_BGQ_MEM_TAG_FORMAT,	
		.tx_ctx_cnt		= tx_ctx_cnt,
		.rx_ctx_cnt		= rx_ctx_cnt,
	};

	*fi->domain_attr = (struct fi_domain_attr) {
		.domain		= NULL,
		.name		= NULL, /* TODO: runtime query for name? */
		.threading	= FI_THREAD_FID,
		.control_progress = FI_PROGRESS_MANUAL,
		.data_progress	= FI_PROGRESS_AUTO, // + FI_PROGRESS_MANUAL ?
		.resource_mgmt	= FI_RM_DISABLED,
		.av_type	= FI_AV_MAP,
		.mr_mode	= FI_MR_SCALABLE,
		.mr_key_size	= 2,
		.cq_data_size	= 0,
		.cq_cnt		= 128 / ppn,
		.ep_cnt		= SIZE_MAX,
		.tx_ctx_cnt	= tx_ctx_cnt,
		.rx_ctx_cnt	= rx_ctx_cnt,

		.max_ep_tx_ctx	= ((BGQ_MU_NUM_INJ_FIFO_GROUPS-1) * BGQ_MU_NUM_INJ_FIFOS_PER_GROUP) / ppn / 2,
		.max_ep_rx_ctx	= ((BGQ_MU_NUM_REC_FIFO_GROUPS-1) * BGQ_MU_NUM_REC_FIFOS_PER_GROUP) / ppn,
		.max_ep_stx_ctx	= ((BGQ_MU_NUM_INJ_FIFO_GROUPS-1) * BGQ_MU_NUM_INJ_FIFOS_PER_GROUP) / ppn / 2,
		.max_ep_srx_ctx	= 0
	};

	*fi->fabric_attr = (struct fi_fabric_attr) {
		.fabric		= NULL,
		.name		= strdup(FI_BGQ_FABRIC_NAME),
		.prov_name	= strdup(FI_BGQ_PROVIDER_NAME),
		.prov_version	= FI_BGQ_PROVIDER_VERSION
	};

	fi->caps		= FI_RMA | FI_ATOMIC |
					FI_NAMED_RX_CTX | FI_TRANSMIT_COMPLETE;
	fi->mode		= FI_ASYNC_IOV;
	fi->addr_format		= FI_ADDR_BGQ;
	fi->src_addrlen		= 24; // includes null
	fi->dest_addrlen	= 24; // includes null

	prev_fi = fi;
	fi = fi_dupinfo(prev_fi);
	prev_fi->next = fi;

	return 0;
}
