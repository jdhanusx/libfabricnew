/*
 * Copyright (c) 2013-2017 Intel Corporation. All rights reserved.
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

#include <stdlib.h>
#include <string.h>

#include "smr.h"

int smr_cq_close(struct fid *fid)
{
	int ret;
	struct smr_cq *smr_cq;

	smr_cq = container_of(fid, struct smr_cq, util_cq.cq_fid.fid);

	ret = ofi_cq_cleanup(&smr_cq->util_cq);
	if (ret)
		return ret;

	free(smr_cq);
	return 0;
}

static ssize_t
smr_peer_cq_progress(struct fid_cq *cq, void *buf, size_t count)
{
	struct util_cq *util_cq;

	if (buf || count > 0)
		return -FI_EINVAL;

	util_cq = container_of(cq, struct util_cq, cq_fid);

	ofi_cq_progress(util_cq);

	return 0;
}

static struct fi_ops_cq smr_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = smr_peer_cq_progress,
	.readfrom = fi_no_cq_readfrom,
	.readerr = fi_no_cq_readerr,
	.sread = fi_no_cq_sread,
	.sreadfrom = fi_no_cq_sreadfrom,
	.signal = fi_no_cq_signal,
	.strerror = fi_no_cq_strerror,
};

static struct fi_ops smr_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = smr_cq_close,
	.bind = fi_no_bind,
	.control = ofi_cq_control,
	.ops_open = fi_no_ops_open,
};

int smr_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		struct fid_cq **cq_fid, void *context)
{
	struct smr_cq *smr_cq;
	int ret;

	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
		attr->wait_obj = FI_WAIT_YIELD;
		/* fall through */
	case FI_WAIT_NONE:
	case FI_WAIT_YIELD:
		break;
	default:
		FI_INFO(&smr_prov, FI_LOG_CQ, "CQ wait not yet supported\n");
		return -FI_ENOSYS;
	}

	smr_cq = calloc(1, sizeof(*smr_cq));
	if (!smr_cq)
		return -FI_ENOMEM;

	ret = ofi_cq_init(&smr_prov, domain, attr, &smr_cq->util_cq,
					  &ofi_cq_progress, context);
	if (ret)
		goto free;

	if (attr->flags & FI_PEER_CQ) {
		struct fi_peer_cq_context *cq_cntxt = context;
		if (!cq_cntxt)
			return -FI_EINVAL;

		smr_cq->peer_cq = cq_cntxt->cq;

		smr_cq->util_cq.cq_fid.fid.ops = &smr_cq_fi_ops;
		smr_cq->util_cq.cq_fid.ops = &smr_cq_ops;
		smr_cq->cq_cb.cq_comp = smr_peer_cq_comp;
		smr_cq->cq_cb.cq_comp_src = smr_peer_cq_comp_src;
		smr_cq->cq_cb.cq_err = smr_peer_cq_err;
	} else {
		smr_cq->cq_cb.cq_comp = smr_cq_comp;
		smr_cq->cq_cb.cq_comp_src = smr_cq_comp_src;
		smr_cq->cq_cb.cq_err = smr_cq_err;
	}

	(*cq_fid) = &smr_cq->util_cq.cq_fid;
	return 0;

free:
	free(smr_cq);
	return ret;
}
