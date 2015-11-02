/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
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
#include <rdma/rdma_cma.h>
#include <netinet/in.h>
#include <infiniband/ib.h>

#include <prov/verbs/src/verbs_ep.h>

#include <prov/verbs/src/fi_verbs.h>
#include <prov/verbs/src/verbs_checks.h>

extern struct fi_provider fi_ibv_prov;
extern struct fi_ops fi_ibv_msg_ep_ops;
extern struct fi_ops_ep fi_ibv_msg_ep_base_ops;
extern struct fi_ops_msg fi_ibv_msg_ep_msg_ops;
extern struct fi_ops_cm fi_ibv_msg_ep_cm_ops;
extern struct fi_ops_rma fi_ibv_msg_ep_rma_ops;
extern struct fi_ops_atomic fi_ibv_msg_ep_atomic_ops;

static const char *local_node = "localhost";

struct fi_info *fi_ibv_search_verbs_info(const char *fabric_name,
		const char *domain_name);

struct fi_ibv_msg_ep *fi_ibv_alloc_msg_ep(struct fi_info *info)
{
	struct fi_ibv_msg_ep *ep;

	ep = calloc(1, sizeof *ep);
	if (!ep)
		return NULL;

	ep->info = fi_dupinfo(info);
	if (!ep->info)
		goto err;

	return ep;
err:
	free(ep);
	return NULL;
}

void fi_ibv_free_msg_ep(struct fi_ibv_msg_ep *ep)
{
	if (ep->id)
		rdma_destroy_ep(ep->id);
	fi_freeinfo(ep->info);
	free(ep);
}

int
fi_ibv_open_ep(struct fid_domain *domain, struct fi_info *info,
	    struct fid_ep **ep, void *context)
{
	struct fi_ibv_domain *dom;
	struct fi_ibv_msg_ep *_ep;
	struct fi_ibv_connreq *connreq;
	struct fi_ibv_pep *pep;
	struct fi_info *fi;
	int ret;

	dom = container_of(domain, struct fi_ibv_domain, domain_fid);
	if (strcmp(dom->verbs->device->name, info->domain_attr->name)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_DOMAIN, "Invalid info->domain_attr->name\n");
		return -FI_EINVAL;
	}

	fi = fi_ibv_search_verbs_info(NULL, info->domain_attr->name);
	if (!fi) {
		FI_INFO(&fi_ibv_prov, FI_LOG_DOMAIN, "Unable to find matching verbs_info\n");
		return -FI_EINVAL;
	}

	if (info->ep_attr) {
		ret = fi_ibv_check_ep_attr(info->ep_attr, fi);
		if (ret)
			return ret;
	}

	if (info->tx_attr) {
		ret = fi_ibv_check_tx_attr(info->tx_attr, info, fi);
		if (ret)
			return ret;
	}

	if (info->rx_attr) {
		ret = fi_ibv_check_rx_attr(info->rx_attr, info, fi);
		if (ret)
			return ret;
	}

	_ep = fi_ibv_alloc_msg_ep(info);
	if (!_ep)
		return -FI_ENOMEM;

	if (!info->handle) {
		ret = fi_ibv_create_ep(NULL, NULL, 0, info, NULL, &_ep->id);
		if (ret)
			goto err;
	} else if (info->handle->fclass == FI_CLASS_CONNREQ) {
		connreq = container_of(info->handle, struct fi_ibv_connreq, handle);
		_ep->id = connreq->id;
        } else if (info->handle->fclass == FI_CLASS_PEP) {
		pep = container_of(info->handle, struct fi_ibv_pep, pep_fid.fid);
		_ep->id = pep->id;
		pep->id = NULL;

		if (rdma_resolve_addr(_ep->id, info->src_addr, info->dest_addr, VERBS_RESOLVE_TIMEOUT)) {
			ret = -errno;
			FI_INFO(&fi_ibv_prov, FI_LOG_DOMAIN, "Unable to rdma_resolve_addr\n");
			goto err;
		}

		if (rdma_resolve_route(_ep->id, VERBS_RESOLVE_TIMEOUT)) {
			ret = -errno;
			FI_INFO(&fi_ibv_prov, FI_LOG_DOMAIN, "Unable to rdma_resolve_route\n");
			goto err;
		}
	} else {
		ret = -FI_ENOSYS;
		goto err;
	}

	_ep->id->context = &_ep->ep_fid.fid;

	_ep->ep_fid.fid.fclass = FI_CLASS_EP;
	_ep->ep_fid.fid.context = context;
	_ep->ep_fid.fid.ops = &fi_ibv_msg_ep_ops;
	_ep->ep_fid.ops = &fi_ibv_msg_ep_base_ops;
	_ep->ep_fid.msg = &fi_ibv_msg_ep_msg_ops;
	_ep->ep_fid.cm = &fi_ibv_msg_ep_cm_ops;
	_ep->ep_fid.rma = &fi_ibv_msg_ep_rma_ops;
	_ep->ep_fid.atomic = &fi_ibv_msg_ep_atomic_ops;

	*ep = &_ep->ep_fid;

	return 0;
err:
	fi_ibv_free_msg_ep(_ep);
	return ret;
}

static int fi_ibv_fi_to_rai(const struct fi_info *fi, uint64_t flags, struct rdma_addrinfo *rai)
{
	memset(rai, 0, sizeof *rai);
	if (flags & FI_SOURCE)
		rai->ai_flags = RAI_PASSIVE;
	if (flags & FI_NUMERICHOST)
		rai->ai_flags |= RAI_NUMERICHOST;

	rai->ai_qp_type = IBV_QPT_RC;
	rai->ai_port_space = RDMA_PS_TCP;

	if (!fi)
		return 0;

	switch(fi->addr_format) {
	case FI_SOCKADDR_IN:
		rai->ai_family = AF_INET;
		rai->ai_flags |= RAI_FAMILY;
		break;
	case FI_SOCKADDR_IN6:
		rai->ai_family = AF_INET6;
		rai->ai_flags |= RAI_FAMILY;
		break;
	case FI_SOCKADDR_IB:
		rai->ai_family = AF_IB;
		rai->ai_flags |= RAI_FAMILY;
		break;
	case FI_SOCKADDR:
		if (fi->src_addrlen) {
			rai->ai_family = ((struct sockaddr *)fi->src_addr)->sa_family;
			rai->ai_flags |= RAI_FAMILY;
		} else if (fi->dest_addrlen) {
			rai->ai_family = ((struct sockaddr *)fi->dest_addr)->sa_family;
			rai->ai_flags |= RAI_FAMILY;
		}
		break;
	case FI_FORMAT_UNSPEC:
		break;
	default:
		VERBS_INFO(FI_LOG_FABRIC, "Unknown fi->addr_format\n");
	}

	if (fi->src_addrlen) {
		if (!(rai->ai_src_addr = malloc(fi->src_addrlen)))
			return -FI_ENOMEM;
		memcpy(rai->ai_src_addr, fi->src_addr, fi->src_addrlen);
		rai->ai_src_len = fi->src_addrlen;
	}
	if (fi->dest_addrlen) {
		if (!(rai->ai_dst_addr = malloc(fi->dest_addrlen)))
			return -FI_ENOMEM;
		memcpy(rai->ai_dst_addr, fi->dest_addr, fi->dest_addrlen);
		rai->ai_dst_len = fi->dest_addrlen;
	}

	return 0;
}

int
fi_ibv_create_ep(const char *node, const char *service,
		 uint64_t flags, const struct fi_info *hints,
		 struct rdma_addrinfo **rai, struct rdma_cm_id **id)
{
	struct rdma_addrinfo rai_hints, *_rai;
	struct rdma_addrinfo **rai_current;
	int ret;

	ret = fi_ibv_fi_to_rai(hints, flags, &rai_hints);
	if (ret)
		goto out;

	if (!node && !rai_hints.ai_dst_addr) {
		if (!rai_hints.ai_src_addr && !service) {
			node = local_node;
		}
		rai_hints.ai_flags |= RAI_PASSIVE;
	}

	ret = rdma_getaddrinfo((char *) node, (char *) service,
				&rai_hints, &_rai);
	if (ret) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "rdma_getaddrinfo", errno);
		ret = -errno;
		goto out;
	}

	/*
	 * If caller requested rai, remove ib_rai entries added by IBACM to
	 * prevent wrong ib_connect_hdr from being sent in connect request.
	 */
	if (rai && hints && (hints->addr_format != FI_SOCKADDR_IB)) {
		for (rai_current = &_rai; *rai_current;) {
			struct rdma_addrinfo *rai_next;
			if ((*rai_current)->ai_family == AF_IB) {
				rai_next = (*rai_current)->ai_next;
				(*rai_current)->ai_next = NULL;
				rdma_freeaddrinfo(*rai_current);
				*rai_current = rai_next;
				continue;
			}
			rai_current = &(*rai_current)->ai_next;
		}
	}

	ret = rdma_create_ep(id, _rai, NULL, NULL);
	if (ret) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "rdma_create_ep", errno);
		ret = -errno;
		goto err;
	}

	if (rai) {
		*rai = _rai;
		goto out;
	}
err:
	rdma_freeaddrinfo(_rai);
out:
	if (rai_hints.ai_src_addr)
		free(rai_hints.ai_src_addr);
	if (rai_hints.ai_dst_addr)
		free(rai_hints.ai_dst_addr);
	return ret;
}
