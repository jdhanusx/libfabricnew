/*
 * Copyright (c) 2017 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
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

#include "tcpx.h"


static struct fi_ops_domain tcpx_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = ip_av_create,
	.cq_open = tcpx_cq_open,
	.endpoint = tcpx_endpoint,
	.scalable_ep = fi_no_scalable_ep,
	.cntr_open = fi_no_cntr_open,
	.poll_open = fi_poll_create,
	.stx_ctx = fi_no_stx_context,
	.srx_ctx = fi_no_srx_context,
	.query_atomic = fi_no_query_atomic,
};

static int tcpx_domain_close(fid_t fid)
{
	struct tcpx_domain *tcpx_domain;
	int ret;

	tcpx_domain = container_of(fid, struct tcpx_domain,
				   util_domain.domain_fid.fid);

	ret = ofi_domain_close(&tcpx_domain->util_domain);
	if (ret)
		return ret;

	ret = tcpx_progress_close(tcpx_domain);
	if (ret)
		return ret;

	free(tcpx_domain->progress);
	free(tcpx_domain);
	return 0;
}

static struct fi_ops tcpx_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = tcpx_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int tcpx_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **domain, void *context)
{
	struct tcpx_domain *tcpx_domain;
	int ret;

	ret = ofi_prov_check_info(&tcpx_util_prov, fabric->api_version, info);
	if (ret)
		return ret;

	tcpx_domain = calloc(1, sizeof(*tcpx_domain));
	if (!tcpx_domain)
		return -FI_ENOMEM;

	ret = ofi_domain_init(fabric, info, &tcpx_domain->util_domain, context);
	if (ret)
		goto err1;

	*domain = &tcpx_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &tcpx_domain_fi_ops;
	(*domain)->ops = &tcpx_domain_ops;


	tcpx_domain->progress = calloc(1, sizeof(*tcpx_domain->progress));
	if (!tcpx_domain->progress)
		goto err2;

	ret = tcpx_progress_init(tcpx_domain, tcpx_domain->progress);
	if (ret)
		goto err3;

	return 0;
err3:
	free(tcpx_domain->progress);
err2:
	ofi_domain_close(&tcpx_domain->util_domain);
err1:
	free(tcpx_domain);
	return ret;
}
