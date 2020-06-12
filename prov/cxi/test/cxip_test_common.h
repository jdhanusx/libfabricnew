/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2018,2020 Cray Inc. All rights reserved.
 */

#ifndef _CXIP_TEST_COMMON_H_
#define _CXIP_TEST_COMMON_H_

#include "cxip.h"

#define CXIT_DEFAULT_TIMEOUT 10

extern struct fi_info *cxit_fi_hints;
extern struct fi_info *cxit_fi;
extern struct fid_fabric *cxit_fabric;
extern struct fid_domain *cxit_domain;
extern struct fi_cxi_dom_ops *dom_ops;
extern struct fid_ep *cxit_ep;
extern struct cxip_addr cxit_ep_addr;
extern fi_addr_t cxit_ep_fi_addr;
extern struct fid_ep *cxit_sep;
extern struct fid_eq *cxit_eq;
extern struct fi_cq_attr cxit_tx_cq_attr, cxit_rx_cq_attr;
extern uint64_t cxit_tx_cq_bind_flags;
extern uint64_t cxit_rx_cq_bind_flags;
extern struct fid_cq *cxit_tx_cq, *cxit_rx_cq;
extern struct fi_cntr_attr cxit_cntr_attr;
extern struct fid_cntr *cxit_send_cntr, *cxit_recv_cntr;
extern struct fid_cntr *cxit_read_cntr, *cxit_write_cntr;
extern struct fid_cntr *cxit_rem_cntr;
extern struct fi_av_attr cxit_av_attr;
extern struct fid_av *cxit_av;
extern char *cxit_node, *cxit_service;
extern uint64_t cxit_flags;
extern int cxit_n_ifs;

void cxit_create_fabric_info(void);
void cxit_destroy_fabric_info(void);
void cxit_create_fabric(void);
void cxit_destroy_fabric(void);
void cxit_create_domain(void);
void cxit_destroy_domain(void);
void cxit_create_ep(void);
void cxit_destroy_ep(void);
void cxit_create_sep(void);
void cxit_destroy_sep(void);
void cxit_create_eq(void);
void cxit_destroy_eq(void);
void cxit_create_cqs(void);
void cxit_destroy_cqs(void);
void cxit_bind_cqs(void);
void cxit_create_cntrs(void);
void cxit_destroy_cntrs(void);
void cxit_bind_cntrs(void);
void cxit_create_av(void);
void cxit_destroy_av(void);
void cxit_bind_av(void);

void cxit_setup_getinfo(void);
void cxit_teardown_getinfo(void);
void cxit_setup_fabric(void);
void cxit_teardown_fabric(void);
void cxit_setup_domain(void);
void cxit_teardown_domain(void);
void cxit_setup_ep(void);
void cxit_teardown_ep(void);
#define cxit_setup_eq cxit_setup_ep
#define cxit_teardown_eq cxit_teardown_ep
#define cxit_setup_cq cxit_setup_ep
#define cxit_teardown_cq cxit_teardown_ep
#define cxit_setup_av cxit_setup_ep
#define cxit_teardown_av cxit_teardown_ep
void cxit_setup_enabled_ep(void);
void cxit_setup_rma(void);
#define cxit_setup_tagged cxit_setup_rma
#define cxit_setup_msg cxit_setup_rma
void cxit_teardown_rma(void);
#define cxit_teardown_tagged cxit_teardown_rma
#define cxit_teardown_msg cxit_teardown_rma
int cxit_await_completion(struct fid_cq *cq, struct fi_cq_tagged_entry *cqe);
void validate_tx_event(struct fi_cq_tagged_entry *cqe, uint64_t flags,
		       void *context);
void validate_rx_event(struct fi_cq_tagged_entry *cqe, void *context,
		       size_t len, uint64_t flags, void *buf, uint64_t data,
		       uint64_t tag);
void validate_multi_recv_rx_event(struct fi_cq_tagged_entry *cqe,
				  void *context, size_t len, uint64_t flags,
				  uint64_t data, uint64_t tag);

struct mem_region {
	uint8_t *mem;
	struct fid_mr *mr;
};

int mr_create(size_t len, uint64_t access, uint8_t seed, uint64_t key,
	      struct mem_region *mr);
void mr_destroy(struct mem_region *mr);

#endif
