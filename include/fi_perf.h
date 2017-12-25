/*
 * Copyright (c) 2015-2017 Intel Corporation, Inc.  All rights reserved.
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

#ifndef _FI_PERF_H_
#define _FI_PERF_H_

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <inttypes.h>
#include <stdio.h>

/* Generic logic */

#define FI_PERF_SLOT_COUNT_MAX       (8)
#define FI_PERF_SLOT_NAME_LENGTH_MAX (256)
#define FI_PERF_PRIVATE_AREA_SIZE    (64)

struct fi_perf_slot {
	uint64_t begin;
	uint64_t sum;
	uint64_t count;
	uint64_t is_active;
};

struct fi_perf_handle {
	struct fi_perf_slot slots[FI_PERF_SLOT_COUNT_MAX];
	char slot_names[FI_PERF_SLOT_COUNT_MAX][FI_PERF_SLOT_NAME_LENGTH_MAX];
	int count;
	uint8_t private[FI_PERF_PRIVATE_AREA_SIZE];
};

#ifdef FI_PERF_RDPMC

/*
 * The following is a modification/adaptation of a code from Andi Kleen
 * pmu-tools project:
 *  https://github.com/andikleen/pmu-tools/blob/master/jevents/rdpmc.c
*/

/*
 * Copyright (c) 2012,2013 Intel Corporation
 * Author: Andi Kleen
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/* RDPMC specific logic */

#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdlib.h>
#include <asm/unistd.h>

struct fi_perf_rdpmc_context {
	int fd;
	struct perf_event_mmap_page *buf;
	int32_t  type;
	uint64_t config;
};

#define fi_perf_rdpmc_rmb() asm volatile("" ::: "memory")

/* Default configuration */

#define FI_PERF_RDPMC_DEFAULT_TYPE   (PERF_TYPE_HARDWARE)
#define FI_PERF_RDPMC_DEFAULT_CONFIG (PERF_COUNT_HW_CPU_CYCLES)
#define FI_PERF_RDPMC_ALIGNMENT      (4096)

static inline int fi_perf_init(struct fi_perf_handle ** handle_ptr)
{
	struct fi_perf_handle * handle = *handle_ptr;

	int result = 0;

	result = posix_memalign((void **)&handle, FI_PERF_RDPMC_ALIGNMENT,
				sizeof(struct fi_perf_handle));
	if (result != 0) {
		/* Error handling */
		goto exit_err_0;
	}

	memset(handle, 0, sizeof(struct fi_perf_handle));
	handle->count = FI_PERF_SLOT_COUNT_MAX;

	struct fi_perf_rdpmc_context * context =
		(struct fi_perf_rdpmc_context *)handle->private;

	char * param_config_str = NULL;

	fi_param_define(NULL, "perf_event_type", FI_PARAM_INT,
			"Specify perf event type: "
			"0 (HW), 1 (SW), 2 (TRACEPOINT), 3 (HW_CACHE), 4 (RAW) "
			"(default: 0)");
	fi_param_define(NULL, "perf_event_config", FI_PARAM_STRING,
			"Specify perf event config in hex: "
			"event type specific "
			"(default: 0)");

	context->type   = FI_PERF_RDPMC_DEFAULT_TYPE;
	context->config = FI_PERF_RDPMC_DEFAULT_CONFIG;

	fi_param_get_int(NULL, "perf_event_type", &context->type);

	if (fi_param_get_str(NULL,
			     "perf_event_config",
			     &param_config_str) == FI_SUCCESS) {
		context->config = (uint64_t)strtoull(param_config_str,
						     NULL,
						     16);
	}

	struct perf_event_attr attr = {
		.type = context->type,
		.size = sizeof(struct perf_event_attr),
		.config = context->config,
		.sample_type = PERF_SAMPLE_READ,
	};

	context->fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
	if (context->fd < 0) {
		perror("perf_event_open");
		/* Error handling */
		goto exit_err_1;
	}

	context->buf = mmap(NULL, sysconf(_SC_PAGESIZE),
			    PROT_READ, MAP_SHARED, context->fd, 0);
	if (context->buf == MAP_FAILED) {
		perror("mmap on perf fd");
		/* Error handling */
		goto exit_err_2;
	}

 exit:
	*handle_ptr = handle;
	return result;

	/* Error handling */
 exit_err_2:
	close(context->fd);
 exit_err_1:
	free(handle);
 exit_err_0:
	handle = NULL;

	result = -1;
	goto exit;
}

static inline void fi_perf_finalize(struct fi_perf_handle * handle)
{
	assert(handle);

	struct fi_perf_rdpmc_context * context =
		(struct fi_perf_rdpmc_context *)handle->private;
	munmap(context->buf, sysconf(_SC_PAGESIZE));
	close(context->fd);
}

static inline void fi_perf_reset(struct fi_perf_handle * handle, int slot_id)
{
	assert(handle);
	assert(slot_id < handle->count);

	handle->slots[slot_id].begin = 0;
	handle->slots[slot_id].sum   = 0;
	handle->slots[slot_id].count = 0;
}

static inline void fi_perf_reset_all(struct fi_perf_handle * handle)
{
	assert(handle);

	int i;
	for (i = 0; i < handle->count; i++) {
		fi_perf_reset(handle, i);
	}
}

static inline void fi_perf_set_slot_name(struct fi_perf_handle * handle,
					 int slot_id, char * slot_name)
{
	assert(handle);
	assert(slot_id < handle->count);

	strncpy(handle->slot_names[slot_id], slot_name,
		FI_PERF_SLOT_NAME_LENGTH_MAX - 1);
	handle->slot_names[slot_id][FI_PERF_SLOT_NAME_LENGTH_MAX - 1] = '\0';

	handle->slots[slot_id].is_active = 1; /* set name activates the slot */
}

#if defined(__ICC) || defined(__INTEL_COMPILER)
#include "immintrin.h"
#endif

static inline uint64_t fi_perf_rdpmc_get_value(struct fi_perf_handle * handle)
{
	/* There are no handle and any other checks on a critical path */
	uint64_t value;
	unsigned seq;
	uint64_t offset;

	struct fi_perf_rdpmc_context * context =
		(struct fi_perf_rdpmc_context *)handle->private;
	struct perf_event_mmap_page * buf = context->buf;

	do {
		seq = buf->lock;
		fi_perf_rdpmc_rmb();
#if defined(__ICC) || defined(__INTEL_COMPILER)
		value = _rdpmc(buf->index - 1);
#else /* GCC */
		value = __builtin_ia32_rdpmc(buf->index - 1);
#endif
		offset = buf->offset;
		fi_perf_rdpmc_rmb();
	} while (buf->lock != seq);

	return value + offset;
}

static inline void fi_perf_begin(struct fi_perf_handle * handle, int slot_id)
{
	assert(handle);
	assert(slot_id < handle->count);

	handle->slots[slot_id].begin = fi_perf_rdpmc_get_value(handle);
}

static inline void fi_perf_end(struct fi_perf_handle * handle, int slot_id)
{
	assert(handle);
	assert(slot_id < handle->count);

	/* There is no counter wrapping handling */
	handle->slots[slot_id].sum +=
	    (fi_perf_rdpmc_get_value(handle) - handle->slots[slot_id].begin);
	handle->slots[slot_id].count++;
}

static inline void fi_perf_dump(struct fi_perf_handle * handle,
				int slot_id, FILE * stream)
{
	assert(handle);
	assert(slot_id < handle->count);
	assert(handle->slots[slot_id].is_active);

	struct fi_perf_rdpmc_context * context =
		(struct fi_perf_rdpmc_context *)handle->private;
	fprintf(stream, "FI_PERF_RDPMC [%s] (%d, %04" PRIX64 ") average = %g"
		" (%" PRIu64 " times)\n",
		handle->slot_names[slot_id], context->type, context->config,
		((double)handle->slots[slot_id].sum /
		 handle->slots[slot_id].count),
		handle->slots[slot_id].count);
	fflush(stream);
}

static inline void fi_perf_dump_all(struct fi_perf_handle * handle,
				    FILE * stream)
{
	assert(handle);

	int slot_id;
	for (slot_id = 0; slot_id < handle->count; slot_id++) {
		if (handle->slots[slot_id].is_active) {
			fi_perf_dump(handle, slot_id, stream);
		}
	}
}

#else /* FI_PERF_RDPMC */

/* Generic code is a stub */

static inline int fi_perf_init(struct fi_perf_handle ** handle)
{
	*handle = NULL;
	return 0;
}

static inline void fi_perf_finalize(struct fi_perf_handle * handle) {}

static inline void fi_perf_reset(struct fi_perf_handle * handle, int slot_id) {}
static inline void fi_perf_reset_all(struct fi_perf_handle * handle) {}

static inline void fi_perf_set_slot_name(struct fi_perf_handle * handle,
					 int slot_id, char * slot_name) {}

static inline void fi_perf_begin(struct fi_perf_handle * handle, int slot_id) {}
static inline void fi_perf_end(struct fi_perf_handle * handle, int slot_id) {}

static inline void fi_perf_dump(struct fi_perf_handle * handle,
				int slot_id, FILE * stream) {}
static inline void fi_perf_dump_all(struct fi_perf_handle * handle,
				    FILE * stream) {}
#endif /* FI_PERF_RDPMC */

/*

1. Usage example:

#define FI_PERF_RDPMC
#include <fi_perf.h>

struct fi_perf_handle * perf_handle = NULL;

...

void my_init_function() {
...
	(void)fi_perf_init(&perf_handle);
	fi_perf_set_slot_name(perf_handle, 0, "My important code block");
...
}

void my_finalize_function() {
...
	fi_perf_dump_all(perf_handle, stderr);
	fi_perf_finalize(perf_handle);
...
}

void my_critical_path_function() {
...
	fi_perf_begin(perf_handle, 0);

	my_important_code_line_0;
	my_important code line_1;
	...
	my_important_code_line_n;

	fi_perf_end(perf_handle, 0);
...
}

2. Runtime control variables (instruction count example):

export FI_PERF_EVENT_TYPE=4
export FI_PERF_EVENT_CONFIG=00C0

The env vars are controls for perf_event_attr structure initialization

3. Output example:

FI_PERF_RDPMC [My important code block] (4, 00c0) average = 86.2605 (2019 times)

*/

#endif /* _FI_PERF_H_ */
