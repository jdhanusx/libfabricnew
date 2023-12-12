/*
 * Copyright (C) 2023 by Cornelis Networks.
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
#ifndef _FI_PROV_OPX_HMEM_H_
#define _FI_PROV_OPX_HMEM_H_

#include <assert.h>
#include "rdma/opx/fi_opx_compiler.h"
#include "ofi_hmem.h"

struct fi_opx_hmem_info {
	uint64_t			device;
	enum fi_hmem_iface		iface;
	uint32_t			unused;
} __attribute__((__packed__)) __attribute__((aligned(8)));

OPX_COMPILE_TIME_ASSERT((sizeof(struct fi_opx_hmem_info) & 0x7) == 0,
			"sizeof(fi_opx_hmem_info) should be a multiple of 8");

__OPX_FORCE_INLINE__
enum fi_hmem_iface fi_opx_hmem_get_iface(const void *ptr,
					 const struct fi_opx_mr *desc,
					 uint64_t *device)
{
#ifdef OPX_HMEM
	if (desc) {
		switch (desc->attr.iface) {
			case FI_HMEM_CUDA:
				*device = desc->attr.device.cuda;
				break;
			case FI_HMEM_ZE:
				*device = desc->attr.device.ze;
				break;
			default:
				*device = 0ul;
		}
		return desc->attr.iface;
	}

	return ofi_get_hmem_iface(ptr, device, NULL);
#else
	*device = 0ul;
	return FI_HMEM_SYSTEM;
#endif
}

__OPX_FORCE_INLINE__
unsigned fi_opx_hmem_is_managed(const void *ptr, const enum fi_hmem_iface iface)
{
#if defined(OPX_HMEM) && HAVE_CUDA
	if (iface == FI_HMEM_CUDA) {
		unsigned is_hmem_managed;
		CUresult __attribute__((unused)) cu_result =
			ofi_cuPointerGetAttribute(&is_hmem_managed,
						CU_POINTER_ATTRIBUTE_IS_MANAGED,
						(CUdeviceptr)ptr);
		assert(cu_result == CUDA_SUCCESS);
		return is_hmem_managed;
	}
#endif
	return 0;
}

__OPX_FORCE_INLINE__
unsigned fi_opx_hmem_iov_init(const void *buf,
				const size_t len,
				const void *desc,
				struct fi_opx_hmem_iov *iov)
{
	iov->buf = (uintptr_t) buf;
	iov->len = len;
#ifdef OPX_HMEM
	uint64_t hmem_device;
	enum fi_hmem_iface hmem_iface = fi_opx_hmem_get_iface(buf, desc, &hmem_device);
	iov->iface = hmem_iface;
	iov->device = hmem_device;
	return (hmem_iface != FI_HMEM_SYSTEM);
#else
	iov->iface = FI_HMEM_SYSTEM;
	iov->device = 0ul;
	return 0;
#endif
}

#ifdef OPX_HMEM
#define OPX_HMEM_COPY_FROM(dst, src, len, src_iface, src_device)			\
	do {										\
		if (src_iface == FI_HMEM_SYSTEM) {					\
			memcpy(dst, src, len);						\
		} else {								\
			ofi_copy_from_hmem(src_iface, src_device, dst, src, len);	\
		}									\
	} while (0)

#define OPX_HMEM_COPY_TO(dst, src, len, dst_iface, dst_device)				\
	do {										\
		if (dst_iface == FI_HMEM_SYSTEM) {					\
			memcpy(dst, src, len);						\
		} else {								\
			ofi_copy_to_hmem(dst_iface, dst_device, dst, src, len);		\
		}									\
	} while (0)

#else

#define OPX_HMEM_COPY_FROM(dst, src, len, src_iface, src_device)			\
	do {										\
		memcpy(dst, src, len);							\
		(void)src_iface;							\
	} while (0)

#define OPX_HMEM_COPY_TO(dst, src, len, dst_iface, dst_device)				\
	do {										\
		memcpy(dst, src, len);							\
		(void)dst_iface;							\
	} while (0)

#endif // OPX_HMEM

#endif
