#include "rdma/bgq/fi_bgq.h"
#include <fi_enosys.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int fi_bgq_close_fabric(struct fid *fid)
{
	int ret;
	struct fi_bgq_fabric *bgq_fabric =
		container_of(fid, struct fi_bgq_fabric, fabric_fid);

	ret = fi_bgq_fid_check(fid, FI_CLASS_FABRIC, "fabric");
	if (ret)
		return ret;

	ret = fi_bgq_ref_finalize(&bgq_fabric->ref_cnt, "fabric");
	if (ret)
		return ret;

	free(bgq_fabric);
	return 0;
}

static struct fi_ops fi_bgq_fi_ops = {
	.size		= sizeof(struct fi_ops),
	.close		= fi_bgq_close_fabric,
	.bind		= fi_no_bind,
	.control	= fi_no_control,
	.ops_open	= fi_no_ops_open
};

static struct fi_ops_fabric fi_bgq_ops_fabric = {
	.size		= sizeof(struct fi_ops_fabric),
	.domain		= fi_bgq_domain,
	.passive_ep	= fi_no_passive_ep,
	.eq_open	= fi_no_eq_open
};

int fi_bgq_check_fabric_attr(struct fi_fabric_attr *attr)
{
	if (attr->name) {
		if (strcmp(attr->name, FI_BGQ_FABRIC_NAME)) {
			FI_LOG(fi_bgq_global.prov, FI_LOG_DEBUG, FI_LOG_FABRIC,
					"attr->name (%s) doesn't match fabric (%s)\n",
					attr->name, FI_BGQ_FABRIC_NAME);
			errno = FI_EINVAL;
			return -errno;
		}
	}
	if (attr->prov_name) {
		if (strcmp(attr->prov_name, FI_BGQ_PROVIDER_NAME)) {
			FI_LOG(fi_bgq_global.prov, FI_LOG_DEBUG, FI_LOG_FABRIC,
					"attr->prov_name (%s) doesn't match prov (%s)\n",
					attr->prov_name, FI_BGQ_PROVIDER_NAME);
			errno = FI_EINVAL;
			return -errno;
		}
	}
	if (attr->prov_version) {
		if (attr->prov_version != FI_BGQ_PROVIDER_VERSION) {
			FI_LOG(fi_bgq_global.prov, FI_LOG_DEBUG, FI_LOG_FABRIC,
					"attr->prov_version (%u) doesn't match prov (%u) "
					"backward/forward compatibility support not implemented\n",
					attr->prov_version, FI_BGQ_PROVIDER_VERSION);
			errno = FI_ENOSYS;
			return -errno;
		}
	}
	return 0;
}

int fi_bgq_fabric(struct fi_fabric_attr *attr,
		struct fid_fabric **fabric, void *context)
{
	int ret;
	struct fi_bgq_fabric *bgq_fabric;

	if (attr) {
		if (attr->fabric) {
			FI_LOG(fi_bgq_global.prov, FI_LOG_DEBUG, FI_LOG_FABRIC,
				"attr->fabric only valid on getinfo\n");
			errno = FI_EINVAL;
			return -errno;
		}

		ret = fi_bgq_check_fabric_attr(attr);
		if (ret)
			return ret;
	}

	bgq_fabric = calloc(1, sizeof(*bgq_fabric));
	if (!bgq_fabric)
		goto err;

	bgq_fabric->fabric_fid.fid.fclass = FI_CLASS_FABRIC;
	bgq_fabric->fabric_fid.fid.context = context;
	bgq_fabric->fabric_fid.fid.ops = &fi_bgq_fi_ops;
	bgq_fabric->fabric_fid.ops = &fi_bgq_ops_fabric;

	ret = fi_bgq_node_init(&bgq_fabric->node);
        if (ret) {
                FI_LOG(fi_bgq_global.prov, FI_LOG_DEBUG, FI_LOG_FABRIC,
                                "error initializing the bgq node manager\n");
                errno = FI_EOTHER;
                goto err;
        }

	fi_bgq_mu_checks();

	*fabric = &bgq_fabric->fabric_fid;

	fi_bgq_ref_init(&bgq_fabric->node, &bgq_fabric->ref_cnt, "fabric");

	return 0;
err:
	errno = FI_ENOMEM;
	return -errno;
}
