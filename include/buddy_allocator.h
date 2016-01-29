/*
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2015 Cray Inc.  All rights reserved.
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

#ifndef _BUDDY_ALLOCATOR_H_
#define _BUDDY_ALLOCATOR_H_

#include "fi_list.h"
#include "gnix_bitmap.h"
#include "gnix_util.h"
#include "gnix.h"
#include <stdlib.h>

#define MIN_BLOCK_SIZE 16

/* The following table was taken from:
 * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogDeBruijn
 */
static const uint32_t MultiplyDeBruijnBitPosition[32] = {
	0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
	8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
};

/* The following log2 function was taken from:
 * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogDeBruijn.
 *
 * Note: this function always truncates the result.
 */
static inline uint32_t buddy_log2(uint32_t v)
{
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;

	return MultiplyDeBruijnBitPosition[(uint32_t)(v * 0x07C4ACDDU) >> 27];
}

/* evaluates to zero if X is not a power of two, otherwise evaluates to X - 1 */
#define IS_NOT_POW_TWO(X) (((X) & (~(X) + 1)) ^ (X))

/* Find the block size (in bytes) required for allocating LEN bytes */
#define BLOCK_SIZE(LEN, MIN_LEN) ((LEN) <= (MIN_LEN) ? (MIN_LEN) :\
			      (IS_NOT_POW_TWO(LEN)) ? (((LEN) << 1) & ~(LEN)) :\
			      (LEN))

/* Find the bitmap index for block X */
#define BITMAP_INDEX(X, BASE, MIN_LEN, LEN) ((size_t) ((X) - (BASE)) /\
					     (MIN_LEN) + 2 * buddy_log2\
					     ((LEN) / (MIN_LEN)))

/* Find the address of X's buddy block:
 * If the "index" of block X is even then the buddy must be to the right of X,
 * otherwise the buddy is to the left of X.
 */
#define BUDDY(X, LEN, BASE) (void *) ((((size_t) (BASE) - (size_t) (X)) /\
				       (LEN)) % 2 ? (size_t) (X) - (LEN) :\
				      (size_t) (X) + (LEN))

/* Calculate the offset of a free block, OFFSET = MIN_LEN * 2^MULT. */
#define OFFSET(MIN_LEN, MULT) ((MIN_LEN) * (1 << (MULT)))

/* Find the index into the free list with block size LEN. */
#define LIST_INDEX(LEN, MIN_LEN)  (buddy_log2((LEN) / (MIN_LEN)))

/**
 * Structure representing a buddy allocator.
 *
 * @var base		The base address of the buffer being managed.
 * @var len		The length of the buffer the buddy allocator is
 * managing.
 * @var max		The largest chunk of memory that can be allocated.
 *
 * @var nlists		The number of free lists.
 * @var lists		The array of free lists ordered from smallest block
 * size.
 * at index 0 to largest block size at index nlists - 1.
 *
 * @var bitmap		Each bit is 1 if the block is allocated or split,
 * otherwise the bit is 0.
 *
 * @var lock		The buddy alloc handle lock.
 */
typedef struct gnix_buddy_alloc_handle {
	void *base;
	uint32_t len;
	uint32_t max;

	uint32_t nlists;
	struct dlist_entry *lists;

	gnix_bitmap_t bitmap;

	fastlock_t lock;
} gnix_buddy_alloc_handle_t;

/**
 * Creates a buddy allocator
 *
 * @param[in] base		Base address of buffer to be managed by
 * allocator.
 *
 * @param[in] len		Size of the buffer to be managed by allocator
 * (must be a multiple of max).
 *
 * @param[in] max		Maximum amount of memory that can be allocated
 * by a single call to buddy_alloc (power 2).
 *
 * @param[in/out] alloc_handle	Handle to be used for when allocating/freeing
 * memory managed by the buddy allocator.
 *
 * @return FI_SUCCESS		Upon successfully creating an allocator.
 *
 * @return -FI_EINVAL		Upon an invalid parameter.
 *
 * @return -FI_ENOMEM		Upon failure to allocate memory to create the
 * buddy allocator.
 */
int buddy_allocator_create(void *base, uint32_t len, uint32_t max,
				 gnix_buddy_alloc_handle_t **alloc_handle);

/**
 * Releases all resources associated with a buddy allocator handle.
 *
 * @param[in] alloc_handle	Buddy alloc handle to destroy.
 *
 * @return FI_SUCCESS	 	Upon successfully destroying an allocator.
 *
 * @return -FI_EINVAL 		Upon an invalid parameter.
 */
int buddy_allocator_destroy(gnix_buddy_alloc_handle_t *alloc_handle);

/**
 * Allocate a buffer from the buddy allocator
 *
 * @param[in] alloc_handle 	Previously allocated GNI buddy_alloc_handle to
 * use as allocator.
 *
 * @param[in/out] ptr		Pointer to an address where the address of the
 * allocated buffer will be returned.
 *
 * @param[in] len		Size of buffer to allocate in bytes.
 *
 * @return FI_SUCCESS		Upon successfully allocating a buffer.
 *
 * @return -FI_ENOMEM 		Upon not being able to allocate a buffer of the
 * requested size.
 *
 * @return -FI_EINVAL		Upon an invalid parameter.
 */
int buddy_alloc(gnix_buddy_alloc_handle_t *alloc_handle, void **ptr,
		      uint32_t len);

/**
 * Free a previously allocated buffer
 *
 * @param[in] alloc_handle 	Previously allocated GNI buddy_alloc_handle to
 * use as allocator.
 *
 * @param[in/out] ptr		Pointer to the previously allocated block.
 *
 * @param[in] len		Size of the previously allocated block.
 *
 * @return FI_SUCCESS		Upon successfully freeing a block.
 *
 * @return -FI_EINVAL		Upon an invalid parameter.
 */
int buddy_free(gnix_buddy_alloc_handle_t *alloc_handle, void *ptr,
		     uint32_t len);
#endif /* _BUDDY_ALLOCATOR_H_ */
