#ifndef HELPERS_H
#define HELPERS_H

#define GET_SIZE_D(header) (sf_size_t)((((header ^ MAGIC) << 32) >> 36) << 4)
#define NEXT_BLOCK_D(block) ((sf_block*)((void*)block + GET_SIZE_D(block->header)))

#include "sfmm.h"
int bytesToFreeListIndex(sf_size_t);

#endif
