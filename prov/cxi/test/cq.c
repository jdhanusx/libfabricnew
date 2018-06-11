/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2018 Cray Inc. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>

#include <criterion/criterion.h>
#include <criterion/parameterized.h>

#include <ofi.h>

#include "cxi_prov.h"
#include "cxi_test_common.h"

TestSuite(cq, .init = cxit_setup_cq, .fini = cxit_teardown_cq);

/* Test basic CQ creation */
Test(cq, simple)
{
	cxit_create_cqs();
	cr_assert(cxit_tx_cq != NULL);
	cr_assert(cxit_rx_cq != NULL);

	cxit_destroy_cqs();
}

static void req_populate(struct cxi_req *req, fi_addr_t *addr)
{
	*addr = 0xabcd0;
	req->flags = FI_SEND;
	req->context = 0xabcd2;
	req->addr = 0xabcd3;
	req->data = 0xabcd4;
	req->tag = 0xabcd5;
	req->buf = 0xabcd6;
	req->data_len = 0xabcd7;
}

Test(cq, read_fmt_context)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_entry entry;
	fi_addr_t req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_read(cxit_tx_cq, &entry, 1);
	cr_assert(ret == 1);
	cr_assert((uint64_t)entry.op_context == req.context);

	cxit_destroy_cqs();
}

Test(cq, read_fmt_msg)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_msg_entry entry;
	fi_addr_t req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_MSG;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_read(cxit_tx_cq, &entry, 1);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(entry.flags == req.flags);
	cr_assert(entry.len == req.data_len);

	cxit_destroy_cqs();
}

Test(cq, read_fmt_data)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_data_entry entry;
	fi_addr_t req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_DATA;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_read(cxit_tx_cq, &entry, 1);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(entry.flags == req.flags);
	cr_assert(entry.len == req.data_len);
	cr_assert((uint64_t)entry.buf == req.buf);
	cr_assert(entry.data == req.data);

	cxit_destroy_cqs();
}

Test(cq, read_fmt_tagged)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_tagged_entry entry;
	fi_addr_t req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_TAGGED;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_read(cxit_tx_cq, &entry, 1);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(entry.flags == req.flags);
	cr_assert(entry.len == req.data_len);
	cr_assert((uint64_t)entry.buf == req.buf);
	cr_assert(entry.data == req.data);
	cr_assert(entry.tag == req.tag);

	cxit_destroy_cqs();
}

Test(cq, readfrom_fmt_context)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_entry entry;
	fi_addr_t addr = 0, req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_readfrom(cxit_tx_cq, &entry, 1, &addr);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(addr == req_addr);

	cxit_destroy_cqs();
}

Test(cq, readfrom_fmt_msg)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_msg_entry entry;
	fi_addr_t addr = 0, req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_MSG;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_readfrom(cxit_tx_cq, &entry, 1, &addr);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(entry.flags == req.flags);
	cr_assert(entry.len == req.data_len);
	cr_assert(addr == req_addr);

	cxit_destroy_cqs();
}

Test(cq, readfrom_fmt_data)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_data_entry entry;
	fi_addr_t addr = 0, req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_DATA;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_readfrom(cxit_tx_cq, &entry, 1, &addr);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(entry.flags == req.flags);
	cr_assert(entry.len == req.data_len);
	cr_assert((uint64_t)entry.buf == req.buf);
	cr_assert(entry.data == req.data);
	cr_assert(addr == req_addr);

	cxit_destroy_cqs();
}

Test(cq, readfrom_fmt_tagged)
{
	int ret;
	struct cxi_cq *cxi_cq;
	struct cxi_req req;
	struct fi_cq_tagged_entry entry;
	fi_addr_t addr = 0, req_addr;

	cxit_tx_cq_attr.format = FI_CQ_FORMAT_TAGGED;
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	req_populate(&req, &req_addr);

	cxi_cq->report_completion(cxi_cq, req_addr, &req);
	ret = fi_cq_readfrom(cxit_tx_cq, &entry, 1, &addr);
	cr_assert(ret == 1);

	cr_assert((uint64_t)entry.op_context == req.context);
	cr_assert(entry.flags == req.flags);
	cr_assert(entry.len == req.data_len);
	cr_assert((uint64_t)entry.buf == req.buf);
	cr_assert(entry.data == req.data);
	cr_assert(entry.tag == req.tag);
	cr_assert(addr == req_addr);

	cxit_destroy_cqs();
}

Test(cq, cq_open_null_attr)
{
	int ret;
	struct fid_cq *cxi_open_cq = NULL;
	struct cxi_cq *cxi_cq = NULL;

	/* Open a CQ with a NULL attribute object pointer */
	ret = fi_cq_open(cxit_domain, NULL, &cxi_open_cq, NULL);
	cr_assert(ret == FI_SUCCESS, "fi_cq_open with NULL attr");
	cr_assert_not_null(cxi_open_cq);

	/* Validate that the default attributes were set */
	cxi_cq = container_of(cxi_open_cq, struct cxi_cq, cq_fid);
	cr_assert_eq(cxi_cq->attr.size, CXI_CQ_DEF_SZ);
	cr_assert_eq(cxi_cq->attr.flags, 0);
	cr_assert_eq(cxi_cq->attr.format, FI_CQ_FORMAT_CONTEXT);
	cr_assert_eq(cxi_cq->attr.wait_obj, FI_WAIT_FD);
	cr_assert_eq(cxi_cq->attr.signaling_vector, 0);
	cr_assert_eq(cxi_cq->attr.wait_cond, FI_CQ_COND_NONE);
	cr_assert_null((void *)cxi_cq->attr.wait_set);

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert(ret == FI_SUCCESS);
	cxi_open_cq = NULL;
}

struct cq_format_attr_params {
	enum fi_cq_format in_format;
	enum fi_cq_format out_format;
	int status;
};

ParameterizedTestParameters(cq, cq_attr_format)
{
	size_t param_sz;

	static struct cq_format_attr_params params[] = {
		{.in_format = FI_CQ_FORMAT_CONTEXT,
		 .out_format = FI_CQ_FORMAT_CONTEXT,
		 .status = FI_SUCCESS},
		{.in_format = FI_CQ_FORMAT_MSG,
		 .out_format = FI_CQ_FORMAT_MSG,
		 .status = FI_SUCCESS},
		{.in_format = FI_CQ_FORMAT_DATA,
		 .out_format = FI_CQ_FORMAT_DATA,
		 .status = FI_SUCCESS},
		{.in_format = FI_CQ_FORMAT_TAGGED,
		 .out_format = FI_CQ_FORMAT_TAGGED,
		 .status = FI_SUCCESS},
		{.in_format = FI_CQ_FORMAT_UNSPEC,
		 .out_format = FI_CQ_FORMAT_CONTEXT,
		 .status = FI_SUCCESS},
		{.in_format = FI_CQ_FORMAT_UNSPEC - 1,
		 .out_format = -1, /* Unchecked in failure case */
		 .status = -FI_ENOSYS}
	};

	param_sz = ARRAY_SIZE(params);
	return cr_make_param_array(struct cq_format_attr_params, params,
				   param_sz);
}

ParameterizedTest(struct cq_format_attr_params *param, cq, cq_attr_format)
{
	int ret;
	struct fid_cq *cxi_open_cq = NULL;
	struct fi_cq_attr cxit_cq_attr = {0};
	struct cxi_cq *cxi_cq = NULL;

	cxit_cq_attr.format = param->in_format;
	cxit_cq_attr.wait_obj = FI_WAIT_NONE; /* default */
	cxit_cq_attr.size = 0; /* default */

	/* Open a CQ with a NULL attribute object pointer */
	ret = fi_cq_open(cxit_domain, &cxit_cq_attr, &cxi_open_cq, NULL);
	cr_assert_eq(ret, param->status,
		     "fi_cq_open() status mismatch %d != %d with format %d. %s",
		     ret, param->status, cxit_cq_attr.format,
		     fi_strerror(-ret));

	if (ret != FI_SUCCESS) {
		/* Test Complete */
		return;
	}

	/* Validate that the format attribute */
	cr_assert_not_null(cxi_open_cq,
			   "fi_cq_open() cxi_open_cq is NULL with format %d",
			   cxit_cq_attr.format);
	cxi_cq = container_of(cxi_open_cq, struct cxi_cq, cq_fid);
	cr_assert_eq(cxi_cq->attr.format, param->out_format);

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert(ret == FI_SUCCESS);
}

struct cq_wait_attr_params {
	enum fi_wait_obj in_wo;
	enum fi_wait_obj out_wo;
	int status;
};

ParameterizedTestParameters(cq, cq_attr_wait)
{
	size_t param_sz;

	static struct cq_wait_attr_params params[] = {
		{.in_wo = FI_WAIT_NONE,
		 .out_wo = FI_WAIT_NONE,
		 .status = FI_SUCCESS},
		{.in_wo = FI_WAIT_FD,
		 .out_wo = FI_WAIT_FD,
		 .status = FI_SUCCESS},
		{.in_wo = FI_WAIT_SET,
		 .out_wo = -1, /* Unchecked in failure case */
		 .status = -FI_ENOSYS},
		{.in_wo = FI_WAIT_MUTEX_COND,
		 .out_wo = FI_WAIT_MUTEX_COND,
		 .status = FI_SUCCESS},
		{.in_wo = FI_WAIT_UNSPEC,
		 .out_wo = FI_WAIT_FD,
		 .status = FI_SUCCESS},
		{.in_wo = FI_WAIT_NONE - 1,
		 .out_wo = -1, /* Unchecked in failure case */
		 .status = -FI_ENOSYS}
	};

	param_sz = ARRAY_SIZE(params);
	return cr_make_param_array(struct cq_wait_attr_params, params,
				   param_sz);
}

ParameterizedTest(struct cq_wait_attr_params *param, cq, cq_attr_wait)
{
	int ret;
	struct fid_cq *cxi_open_cq = NULL;
	struct fi_cq_attr cxit_cq_attr = {0};
	struct cxi_cq *cxi_cq = NULL;

	cxit_cq_attr.wait_obj = param->in_wo;
	cxit_cq_attr.format = FI_CQ_FORMAT_UNSPEC; /* default */
	cxit_cq_attr.size = 0; /* default */

	/* Open a CQ with a NULL attribute object pointer */
	ret = fi_cq_open(cxit_domain, &cxit_cq_attr, &cxi_open_cq, NULL);
	cr_assert_eq(ret, param->status,
		     "fi_cq_open() status mismatch %d != %d with wait obj %d. %s",
		     ret, param->status, cxit_cq_attr.wait_obj,
		     fi_strerror(-ret));

	if (ret != FI_SUCCESS) {
		/* Test Complete */
		return;
	}

	/* Validate that the wait_obj attribute */
	cr_assert_not_null(cxi_open_cq);
	cxi_cq = container_of(cxi_open_cq, struct cxi_cq, cq_fid);
	cr_assert_eq(cxi_cq->attr.wait_obj, param->out_wo);

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert(ret == FI_SUCCESS);
}

struct cq_size_attr_params {
	size_t in_sz;
	size_t out_sz;
};

ParameterizedTestParameters(cq, cq_attr_size)
{
	size_t param_sz;

	static struct cq_size_attr_params params[] = {
		{.in_sz = 0,
		 .out_sz = CXI_CQ_DEF_SZ},
		{.in_sz = 1 << 9,
		 .out_sz = 1 << 9},
		{.in_sz = 1 << 6,
		 .out_sz = 1 << 6}
	};

	param_sz = ARRAY_SIZE(params);
	return cr_make_param_array(struct cq_size_attr_params, params,
				   param_sz);
}

ParameterizedTest(struct cq_size_attr_params *param, cq, cq_attr_size)
{
	int ret;
	struct fid_cq *cxi_open_cq = NULL;
	struct fi_cq_attr cxit_cq_attr = {0};
	struct cxi_cq *cxi_cq = NULL;

	cxit_cq_attr.format = FI_CQ_FORMAT_UNSPEC; /* default */
	cxit_cq_attr.wait_obj = FI_WAIT_NONE; /* default */
	cxit_cq_attr.size = param->in_sz;

	/* Open a CQ with a NULL attribute object pointer */
	ret = fi_cq_open(cxit_domain, &cxit_cq_attr, &cxi_open_cq, NULL);
	cr_assert_eq(ret, FI_SUCCESS,
		     "fi_cq_open() status mismatch %d != %d with size %ld. %s",
		     ret, FI_SUCCESS, cxit_cq_attr.size,
		     fi_strerror(-ret));
	cr_assert_not_null(cxi_open_cq);

	/* Validate that the size attribute */
	cxi_cq = container_of(cxi_open_cq, struct cxi_cq, cq_fid);
	cr_assert_eq(cxi_cq->attr.size, param->out_sz);
	cr_assert_eq(cxi_cq->cq_rbfd.rb.size,
		     param->out_sz * cxi_cq->cq_entry_size);
	cr_assert_eq(cxi_cq->addr_rb.size,
		     roundup_power_of_two(param->out_sz * sizeof(fi_addr_t)));
	cr_assert_eq(cxi_cq->cqerr_rb.size,
		     roundup_power_of_two(param->out_sz *
					  sizeof(struct fi_cq_err_entry)));

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert(ret == FI_SUCCESS);
}

Test(cq, cq_open_null_domain, .signal = SIGSEGV)
{
	struct fid_cq *cxi_open_cq = NULL;

	/*
	 * Attempt to open a CQ with a NULL domain pointer
	 * Expect a SIGSEGV since the fi_cq_open implementation attempts to
	 * use the domain pointer before checking.
	 */
	fi_cq_open(NULL, NULL, &cxi_open_cq, NULL);
}

Test(cq, cq_open_null_cq)
{
	/* Attempt to open a CQ with a NULL cq pointer */
	int ret;

	ret = fi_cq_open(cxit_domain, NULL, NULL, NULL);
	cr_assert(ret == -FI_EINVAL, "fi_cq_open with NULL cq");
}

Test(cq, strerror_str_termination)
{
	const char *err_str;
	char *test_str = NULL, *orig_str = NULL;
	int err_num = -FI_EOVERRUN;
	size_t orig_len = 0, test_len;
	int ret;
	struct fid_cq *cxi_open_cq = NULL;

	/* Open a CQ with a NULL attribute object pointer */
	ret = fi_cq_open(cxit_domain, NULL, &cxi_open_cq, NULL);
	cr_assert(ret == FI_SUCCESS, "fi_cq_open with NULL attr");
	cr_assert_not_null(cxi_open_cq);

	/* Get an error string without a buffer */
	err_str = fi_cq_strerror(cxi_open_cq, err_num, NULL, NULL, 0);
	cr_assert_not_null(err_str, "Original error string must not be NULL");

	orig_len = strlen(err_str);
	cr_assert_gt(orig_len, 0,
		     "Original error string must be greater than zero");

	/* Make a copy of the original error string */
	orig_str = malloc(orig_len + 1);
	cr_assert_not_null(orig_str, "Unable to malloc a buffer");
	strcpy(orig_str, err_str);

	/* Get a buffer larger than the original str for boundary testing */
	test_len = orig_len + 10;
	test_str = malloc(test_len);
	cr_expect_not_null(test_str, "Unable to malloc a test buffer");
	if (!test_str)
		goto str_term_cleanup;

	/* Walk through the range of lengths before and after string end */
	for (int i = 3; i > -4; i--) {
		/* Initialize and terminate the buffer */
		memset(test_str, 0x55, test_len);
		test_str[test_len - 1] = '\0';

		err_str = fi_cq_strerror(cxi_open_cq, err_num, NULL, test_str,
					 orig_len + i);
		cr_expect_eq(err_str, test_str,
			     "Returned pointer does not match buffer. i %d Expected %p Got %p",
			     i, test_str, err_str);

		if (i > 0) {
			/* Expect the whole string returned */
			if (strcmp(test_str, orig_str)) {
				cr_expect_fail(
					"Strings do not match. i %d Len %zd '%s' != '%s'",
					i, orig_len + i, test_str, orig_str);
			}
		} else {
			/* Expect most of the string returned */
			if (strncmp(test_str, orig_str, strlen(test_str))) {
				cr_expect_fail(
					"Strings do not match. i %d Len %zd '%s' != '%s'",
					i, orig_len + i - 1, test_str,
					orig_str);
			}
		}
	}

str_term_cleanup:
	if (orig_str)
		free(orig_str);
	if (test_str)
		free(test_str);

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert(ret == FI_SUCCESS);
}

Test(cq, cq_readerr_null_cq, .signal = SIGSEGV)
{
	struct fi_cq_err_entry err_entry;

	/* Attempt to read an err with a CQ with a NULL cq pointer */
	fi_cq_readerr(NULL, &err_entry, (uint64_t)0);
}

Test(cq, cq_readerr_null_buff)
{
	int ret;
	struct fid_cq *cxi_open_cq = NULL;

	/* Open a CQ */
	ret = fi_cq_open(cxit_domain, NULL, &cxi_open_cq, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "fi_cq_open with NULL attr");
	cr_assert_not_null(cxi_open_cq);

	/* Attempt to read an err with a CQ with a NULL buff pointer */
	ret = fi_cq_readerr(cxi_open_cq, NULL, (uint64_t)0);
	cr_assert_eq(ret, -FI_EINVAL, "fi_cq_open with NULL buff returned %d",
		     ret);

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert_eq(ret, FI_SUCCESS);
}

Test(cq, cq_readerr_no_errs)
{
	int ret;
	struct fid_cq *cxi_open_cq = NULL;
	struct fi_cq_err_entry err_entry;

	/* Open a CQ */
	ret = fi_cq_open(cxit_domain, NULL, &cxi_open_cq, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "fi_cq_open with NULL attr");
	cr_assert_not_null(cxi_open_cq);

	/* Attempt to read an err with a CQ with a NULL buff pointer */
	ret = fi_cq_readerr(cxi_open_cq, &err_entry, (uint64_t)0);
	/* Expect no completions to be available */
	cr_assert_eq(ret, -FI_EAGAIN, "fi_cq_readerr returned %d - %s", ret,
		     fi_cq_strerror(cxi_open_cq, ret, NULL, NULL, 0));

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert_eq(ret, FI_SUCCESS);
}

void err_entry_comp(struct fi_cq_err_entry *a,
		   struct fi_cq_err_entry *b,
		   size_t size)
{
	uint8_t *data_a, *data_b;

	data_a = (uint8_t *)a;
	data_b = (uint8_t *)b;

	for (int i = 0; i < size; i++)
		if (data_a[i] != data_b[i])
			cr_expect_fail("Mismatch at offset %d. %02X - %02X",
				       i, data_a[i], data_b[i]);
}

Test(cq, cq_readerr_err)
{
	int ret;
	size_t avail;
	struct fid_cq *cxi_open_cq = NULL;
	struct fi_cq_err_entry err_entry, fake_entry;
	struct cxi_cq *cxi_cq;
	uint8_t *data_fake, *data_err;

	/* initialize the entries with data */
	data_fake = (uint8_t *)&fake_entry;
	data_err = (uint8_t *)&err_entry;
	for (int i = 0; i < sizeof(fake_entry); i++) {
		data_fake[i] = (uint8_t)i;
		data_err[i] = (uint8_t)0xa5;
	}
	fake_entry.err_data = err_entry.err_data = NULL;
	fake_entry.err_data_size = err_entry.err_data_size = 0;

	/* Open a CQ */
	ret = fi_cq_open(cxit_domain, NULL, &cxi_open_cq, NULL);
	cr_assert_eq(ret, FI_SUCCESS, "fi_cq_open with NULL attr");
	cr_assert_not_null(cxi_open_cq);

	/* Add a fake error to the CQ's error ringbuffer */
	cxi_cq = container_of(cxi_open_cq, struct cxi_cq, cq_fid);
	fastlock_acquire(&cxi_cq->lock);
	avail = ofi_rbavail(&cxi_cq->cqerr_rb);
	cr_assert_geq(avail, sizeof(fake_entry),
		      "Not enough space in error ring buffer. %zd", avail);
	ofi_rbwrite(&cxi_cq->cqerr_rb, &fake_entry, sizeof(fake_entry));
	ofi_rbcommit(&cxi_cq->cqerr_rb);
	fastlock_release(&cxi_cq->lock);

	/* Attempt to read an err with a CQ with a NULL buff pointer */
	ret = fi_cq_readerr(cxi_open_cq, &err_entry, (uint64_t)0);
	/* Expect 1 completion to be available */
	cr_assert_eq(ret, 1, "fi_cq_readerr returned %d - %s", ret,
		     fi_cq_strerror(cxi_open_cq, ret, NULL, NULL, 0));
	/* Expect the data to match the fake entry */
	err_entry_comp(&err_entry, &fake_entry, sizeof(fake_entry));

	ret = fi_close(&cxi_open_cq->fid);
	cr_assert_eq(ret, FI_SUCCESS);
}

Test(cq, cq_readerr_reperr)
{
	int ret;
	struct fi_cq_err_entry err_entry = {0};
	struct cxi_req req = {0};
	size_t olen, err_data_size;
	int err, prov_errno;
	void *err_data;
	struct cxi_cq *cxi_cq;
	uint8_t err_buff[32] = {0};

	/* initialize the input data */
	req.flags = 0x12340987abcd5676;
	req.context = 0xa5a5a5a5a5a5a5a5;
	req.data_len = 0xabcdef0123456789;
	req.data = 0xbadcfe1032547698;
	req.tag = 0xefcdab0192837465;
	olen = 0x4545121290907878;
	err = -3;
	prov_errno = -2;
	err_data = (void *)err_buff;
	err_data_size = ARRAY_SIZE(err_buff);

	/* Open a CQ */
	cxit_create_cqs();
	cxi_cq = container_of(cxit_tx_cq, struct cxi_cq, cq_fid);

	/* Add an error to the CQ's error ringbuffer */
	ret = cxi_cq_report_error(cxi_cq, &req, olen, err, prov_errno,
				  err_data, err_data_size);
	cr_assert_eq(ret, 0, "cxi_cq_report_error() error %d", ret);

	/* Attempt to read an err with a CQ with a NULL buff pointer */
	ret = fi_cq_readerr(cxit_tx_cq, &err_entry, (uint64_t)0);
	cr_assert_eq(ret, 1, "fi_cq_readerr returned %d - %s", ret,
		     fi_cq_strerror(cxit_tx_cq, ret, NULL, NULL, 0));

	/* Expect the data to match the fake entry */
	cr_assert_eq(err_entry.err, err);
	cr_assert_eq(err_entry.olen, olen);
	cr_assert_eq(err_entry.len, req.data_len);
	cr_assert_eq(err_entry.prov_errno, prov_errno);
	cr_assert_eq(err_entry.flags, req.flags);
	cr_assert_eq(err_entry.data, req.data);
	cr_assert_eq(err_entry.tag, req.tag);
	cr_assert_eq(err_entry.op_context, (void *)(uintptr_t)req.context);
	cr_assert_eq(err_entry.err_data, err_data);
	cr_assert_leq(err_entry.err_data_size, err_data_size,
		      "Size mismatch. %zd, %zd",
		      err_entry.err_data_size, err_data_size);

	cxit_destroy_cqs();
}

