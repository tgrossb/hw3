/**
 * Do not submit your assignment with a main function in this file.
 * If you submit with a main function in this file, you will get a zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "sfmm.h"
#include "helpers.h"
#include "errno.h"

#define ROW_SIZE 8
#define MIN_BLOCK_SIZE 32
#define BLOCK_ALIGN 16

// Get the size of a block from an sf_header or block
#define GET_SIZE(header) (sf_size_t)((((header ^ MAGIC) << 32) >> 36) << 4)
#define GET_B_SIZE(block) GET_SIZE(block->header)

// Isolate the prev allocated bit from a block
#define GET_PREV_ALLOCD(block) ((block->header ^ MAGIC) & PREV_BLOCK_ALLOCATED)

// Return a boolean representing if a block has the allocated bit set
#define IS_ALLOCD(block) (((block->header ^ MAGIC) & THIS_BLOCK_ALLOCATED) > 0)

// Returns a pointer to the next block in the heap
#define NEXT_BLOCK_S(block, size) ((sf_block*)((void*)block + size))
#define NEXT_BLOCK(block) NEXT_BLOCK_S(block, GET_SIZE(block->header))

// Returns a pointer to the previous block in the heap
#define PREV_BLOCK_S(block, size) ((sf_block*)((void*)block - size))
#define PREV_BLOCK(block) PREV_BLOCK_S(block, GET_SIZE(block->prev_footer))

// Coalesce is the only one that needs to be declared before use (bc of insertIntoQuickList)
sf_block* coalesce(sf_block*);

/**
 * Converts the size of a block to the minimum free list index
 * that a block this size or greater would be found at.
 *
 * Free lists are partitioned into bins (M is the minimum block size)
 *	M, (M, 2M], (2M, 4M], (4M, 8M], ..., (nM, inf)
 *
 * If the index hits the max index we are in the last range,
 * which goes to infinity, so return that at most.
 *
 * @param size The effective size of the block to be allocated
 * @return The index of the free list that contains block of that size or greater
 */
int bytesToFreeListIndex(sf_size_t size){
	sf_size_t binMax = MIN_BLOCK_SIZE;
	int index = 0;
	// Increment index (up to a max of NUM_FREE_LISTS-1), doubling binMax each time
	// Stop when the binMax is large enough to fit the block
	for (; size > binMax && index < NUM_FREE_LISTS-1; index++)
		binMax = binMax << 1;
	return index;
}


/**
 * Traverses the heap summing the payload of all allocated blocks, and keeps track of the maximum.
 */
double maxAggPayload = 0;
void updateMaxAggPayload(){
	double aggPayload = 0;

	if (sf_mem_start() == sf_mem_end())
		return;

	// Get pointers to the first block and epilogue, these are our start and stop
	sf_block* currentBlock = (sf_block*)(sf_mem_start() + 4*ROW_SIZE);
	sf_block* epilogue = (sf_block*)(sf_mem_end() - 2*ROW_SIZE);

	// Step through the blocks until we hit the epilogue
	while (currentBlock < epilogue){
		// Check if the current block is allocated and not in the quick list
		if (IS_ALLOCD(currentBlock) && ((currentBlock->header ^ MAGIC) & IN_QUICK_LIST) == 0){
			// If this is the case, get the payload
			sf_size_t payloadSize = (sf_size_t)((currentBlock->header^MAGIC) >> 32);

			// Increase the aggregate payload
			aggPayload += payloadSize;
		}

		// Advance the current block to the next block
		currentBlock = NEXT_BLOCK(currentBlock);
	}

	if (aggPayload > maxAggPayload)
		maxAggPayload = aggPayload;
}

/**
 * Sets the PREV_BLOCK_ALLOCATED bit of the block to either 1 or 0 depending on bit.
 * Also updates the prev_footer of the next block.
 *
 * @param block A pointer to the block to be updated
 * @param bit The value of the bit to be set, either 1 or 0 (default)
 */
void setPrevAllocd(sf_block* block, int bit){
	// Set the prevAllocd bit of the header by first anding with ~PREV_ALLOCD to destroy that bit
	// Then potentially or with PREV_ALLOCD to set it if bit == 1
	block->header = (block->header ^ MAGIC) & ~PREV_BLOCK_ALLOCATED;
	if (bit == 1)
		block->header = block->header | PREV_BLOCK_ALLOCATED;
	block->header = block->header ^ MAGIC;

	// Set the prev_footer of the next block to be the new header
	if (!IS_ALLOCD(block))
		NEXT_BLOCK(block)->prev_footer = block->header;
}


/**
 * Initializes the specified free list if it has not been already.  Initializing being setting
 * the dummy head of the list's pointers back to itself to be a circular doubly linked list
 *
 * @param c The index of the free list to initialize
 */
void verifyFreeListLinks(int c){
	if (sf_free_list_heads[c].body.links.next == NULL)
		sf_free_list_heads[c].body.links.next = &sf_free_list_heads[c];
	if (sf_free_list_heads[c].body.links.prev == NULL)
		sf_free_list_heads[c].body.links.prev = &sf_free_list_heads[c];
}


/**
 * Inserts a block into the quick list, flushing it if necessary.
 *
 * @param block A pointer to the block to be added
 * @param blockSize The size of the block to be added (has been calculated already)
 * @param quickListI The index of the quick list the block should be added too (also calculated already)
 */
void insertIntoQuickList(sf_block* block, sf_size_t blockSize, int quickListI){
	// Adjust the block header to be a quick list block
	block->header = (blockSize | THIS_BLOCK_ALLOCATED | GET_PREV_ALLOCD(block) | IN_QUICK_LIST) ^ MAGIC;

	// Set the next block's prev footer to match
	NEXT_BLOCK_S(block, blockSize)->prev_footer = block->header;

	// If the list is not at capacity yet, just insert it
	if (sf_quick_lists[quickListI].length <= QUICK_LIST_MAX){
		sf_block* oldFirst = sf_quick_lists[quickListI].first;

		// Point first to the new block, link in, and increase length
		sf_quick_lists[quickListI].first = block;
		if (oldFirst != NULL){
			block->body.links.next = oldFirst;
			oldFirst->body.links.prev = block;
			sf_quick_lists[quickListI].length += 1;
		}
		else
			sf_quick_lists[quickListI].length = 1;
	}
	// Otherwise, we need to flush the quick list
	else {
		// Follow the pointers from the start to the end
		sf_block* current = sf_quick_lists[quickListI].first;
		for (int c=0; c<sf_quick_lists[quickListI].length; c++){
			// Free the current block
			current->header = (blockSize | GET_PREV_ALLOCD(current)) ^ MAGIC;
			NEXT_BLOCK_S(current, blockSize)->prev_footer = current->header;
			sf_block* next = current->body.links.next;
			coalesce(current);
			current = next;
		}

		// Insert the new block into the now empty quick list
		sf_quick_lists[quickListI].first = block;
		sf_quick_lists[quickListI].length = 1;
	}
}

/**
 * Removes a block from a circular doubly linked list (free list).
 *
 * @param block A pointer to the block to be removed
 */
void removeFromFreeList(sf_block *block){
	// If the block is not linked in, there is nothing to do
	if (block->body.links.prev == NULL || block->body.links.next == NULL)
		return;

	// Set the next pointer of the previous block to the block's next
	(block->body.links.prev)->body.links.next = block->body.links.next;

	// Set the prev pointer of the next block to the block's prev
	(block->body.links.next)->body.links.prev = block->body.links.prev;

	// Set the prev and next pointers to null
	block->body.links.prev = NULL;
	block->body.links.next = NULL;
}


/**
 * Inserts a block at the front of the specified free list.
 *
 * @param block A pointer to the block to insert
 * @param blockSize The size of the block that is being inserted
 */
void insertIntoFreeList(sf_block* block){
	// Require that the block is not in any free list yet
	if (block->body.links.prev != NULL || block->body.links.next != NULL)
		return;

	sf_size_t blockSize = GET_SIZE(block->header);
	int c = bytesToFreeListIndex(blockSize);

	// Ensure that the list is initialized
	verifyFreeListLinks(c);

	// Set the new block's prev to the dummy head
	block->body.links.prev = &sf_free_list_heads[c];
	// Set the new block's next to the old first (head.next)
	block->body.links.next = sf_free_list_heads[c].body.links.next;

	// Now the block's pointers are set, link it into the list
	// Set the old first's prev to the block
	(sf_free_list_heads[c].body.links.next)->body.links.prev = block;
	// Set the head's next to the block
	sf_free_list_heads[c].body.links.next = block;
}

/**
 * Checks the quick lists for an available block of the proper size,
 * deletes it from the quick lists, and returns a pointer to it.
 *
 * @param size The effective size of the block to be allocated
 * @return The address of the block to be allocated or null if one was not found.
 */
sf_block* getQuickListBlock(sf_size_t size){
	int index = (size-MIN_BLOCK_SIZE) / BLOCK_ALIGN;
	if (index >= NUM_QUICK_LISTS)
		return NULL;

	if (sf_quick_lists[index].length <= 0)
		return NULL;

	// As the quick list is maintained by inserting into the front, the first is what we return
	sf_block* toAllocate = sf_quick_lists[index].first;

	// If the first is not null (it shouldn't be), redirect list.first to list.first.next (might be NULL)
	// Deletes the block from the quick list
	if (toAllocate != NULL){
		sf_quick_lists[index].first = toAllocate->body.links.next;
		sf_quick_lists[index].length -= 1;
	}

	return toAllocate;
}


/**
 * Checks the free lists for an available sufficiently large block.
 * If one is located, remove it from the free list and return it.
 *
 * @param size The effective size of the block to be allocated
 * @return A pointer to a sufficiently large block or NULL if none was found.
 */
sf_block* getFreeListBlock(sf_size_t size){
	int freeListI = bytesToFreeListIndex(size);
	// Starting from the calculated min free list i, go through all free lists until a hit
	for (; freeListI < NUM_FREE_LISTS; freeListI++){
		// Ensure that initial free list links are set up
		verifyFreeListLinks(freeListI);

		// Use the first fitting block in the free list
		// The start block is a dummy senteniel, just use it as reference
		sf_block* start = &sf_free_list_heads[freeListI];
		sf_block* current = start->body.links.next;
		while (current != start) {
			// If the current block is too small, move on to the next and continue
			if (GET_B_SIZE(current) < size){
				current = current->body.links.next;
				continue;
			}

			// Otherwise we have found a match
			// Remove it from the free list and return it
			removeFromFreeList(current);
			return current;
		}
	}

	// If we've made it to here the free lists do not contain a fitting block
	// Indicate this with a null pointer
	return NULL;
}


/**
 * Coalesces a free block with any adjacent free blocks.  Assumes that free blocks
 * are coalesced immediately so it only checks neighbours, not neighbours of neighbours, etc.
 * Expects that block is not already added to a free list, adds everything needed to a free list.
 *
 * @param block A pointer to the block to be coalesced
 * @return A pointer to the (potentially) new block that has been allocated
 */
sf_block* coalesce(sf_block* block){
	// First check that the goal block is free
	if (IS_ALLOCD(block))
		return NULL;
	// Remove the block from the free list just in case (won't do anything if not in one yet)
	removeFromFreeList(block);

	// Otherwise, check if the previous and next blocks are allocated
	sf_block* prevBlock = PREV_BLOCK(block);
	sf_block* nextBlock = NEXT_BLOCK(block);

	int prevAllocd = IS_ALLOCD(prevBlock);
	int nextAllocd = IS_ALLOCD(nextBlock);

	if (!prevAllocd)
		removeFromFreeList(prevBlock);
	if (!nextAllocd)
		removeFromFreeList(nextBlock);

	// If both prev and next are allocated there is nothing to do
	if (prevAllocd && nextAllocd){
		// Add block to the free list
		insertIntoFreeList(block);
		return block;
	}
	// If only the next block is free expand the current block to include it
	else if (prevAllocd && !nextAllocd){
		// Update the current block's header to be expanded
		block->header = ((GET_B_SIZE(block) + GET_B_SIZE(nextBlock)) | GET_PREV_ALLOCD(block)) ^ MAGIC;
		NEXT_BLOCK(block)->prev_footer = block->header;
		// Add block to the proper free list
		insertIntoFreeList(block);
		return block;
	}
	// If only the prev block is free expand the prev block to include the current
	else if (!prevAllocd && nextAllocd){
		// Update the prev block's header to be expanded
		prevBlock->header = ((GET_B_SIZE(prevBlock) + GET_B_SIZE(block)) | GET_PREV_ALLOCD(prevBlock)) ^ MAGIC;
		nextBlock->prev_footer = prevBlock->header;

		// Reinsert prevBlock to force an index update
		insertIntoFreeList(prevBlock);

		return prevBlock;
	}
	// If both blocks are free expand the prev block to include the current and next
	else {
		// Update the prev block's header to be expanded
		prevBlock->header = ((GET_B_SIZE(prevBlock) + GET_B_SIZE(block) + GET_B_SIZE(nextBlock)) | GET_PREV_ALLOCD(prevBlock)) ^ MAGIC;
		// Remove block and next block from any free lists
		NEXT_BLOCK(prevBlock)->prev_footer = prevBlock->header;

		// Reinsert prevBlock to force an index update
		insertIntoFreeList(prevBlock);

		return prevBlock;
	}

	// If we've gotten to here theres an error
	fprintf(stderr, "Unreachable code point reached :(");
	return NULL;
}


/**
 * Initializes the heap by calling the first use of mem_grow, building a prologue, epilogue, and first
 * free block that makes up the beginning of the free lists.
 *
 * @return -1 on error (no memory) or 1 if all goes properly.
 */
int initializeHeap(){
	void* heapStart = sf_mem_grow();
	if (heapStart == NULL)
		return -1;

	// Build the prologue, no need to add to heapStart because prev_footer will be the empty padding row
	sf_block* prologue = (sf_block*)heapStart;
	prologue->header = (MIN_BLOCK_SIZE | THIS_BLOCK_ALLOCATED ) ^ MAGIC;

	// The first free block is 4 rows past the start of the heap (not 5 because of prev_footer)
	sf_block* firstBlock = (sf_block*)(heapStart + 4*ROW_SIZE);
	firstBlock->prev_footer = prologue->header;
	// Block size is the heap size - 5 rows (prologue) - 1 row (epilogue)
	sf_size_t blockSize = sf_mem_end() - heapStart - 6*ROW_SIZE;
	firstBlock->header = (blockSize | PREV_BLOCK_ALLOCATED) ^ MAGIC;
	firstBlock->body.links.prev = NULL;
	firstBlock->body.links.next = NULL;

	// Insert firstBlock into its appropriate free list (no need to coalesce)
	insertIntoFreeList(firstBlock);

	// Build the epilogue
	sf_block* epilogue = (sf_block*)(sf_mem_end() - 2*ROW_SIZE);
	epilogue->header = THIS_BLOCK_ALLOCATED ^ MAGIC;
	epilogue->prev_footer = firstBlock->header;
	return 1;
}

/**
 * Extends the heap by one page of memory.  Uses mem_grow, converts the old epilogue into a new free
 * block, and adds an epilogue to the new end of the heap.
 *
 * @return -1 on error (no memory) or 1 if all goes properly
 */
int extendHeap(){
	// Locate the old epilogue
	sf_block* oldEpilogue = (sf_block*)(sf_mem_end() - 2*ROW_SIZE);

	// Allocate a new page of memory, returning -1 if that fails
	void* nextPageStart = sf_mem_grow();
	if (nextPageStart == NULL)
		return -1;

	// Locate the new epilogue and set it's header (inserting a new free block so no prev allocated)
	sf_block* newEpilogue = (sf_block*)(sf_mem_end() - 2*ROW_SIZE);
	newEpilogue->header = THIS_BLOCK_ALLOCATED ^ MAGIC;

	// Turn the old epilogue into a new free block
	// Flip the allocated bit and calculate the new block size
	sf_size_t blockSize = ((void*) newEpilogue) - ((void*) oldEpilogue);
	oldEpilogue->header = (blockSize | GET_PREV_ALLOCD(oldEpilogue)) ^ MAGIC;
	oldEpilogue->body.links.prev = NULL;
	oldEpilogue->body.links.next = NULL;

	// Set the new epilogue's prev header temporarily to the new free block, will have to reset it after coalescing
	newEpilogue->prev_footer = oldEpilogue->header;

	// Coalesce the new free block with any adjacent free blocks, which will insert it into the free list
	sf_block* freeBlock = coalesce(oldEpilogue);

	// Copy the new free block's header to the prevFooter of newEpilogue
	newEpilogue->prev_footer = freeBlock->header;

	return 1;
}


/**
 * Splits a block into an allocated chunk and an unallocated chunk (with the unallocated being the higher in heap chunk).
 *
 * @param block A pointer to the whole block to split.
 * @param payloadSize The size of the payload for the allocated chunk (needed for the header)
 * @param effectiveSize The size of the block to be allocated
 * @param remainderSize The size of the block that will remain unallocated
 */
void splitBlock(sf_block* block, sf_header payloadSize, sf_size_t effectiveSize, sf_size_t remainderSize){
		// Redefine the current block's header to only include the effective size
		block->header = (payloadSize | effectiveSize | THIS_BLOCK_ALLOCATED | GET_PREV_ALLOCD(block)) ^ MAGIC;

		// Remove the block from any free lists
		removeFromFreeList(block);

		// Turn the remainder into a new fragment
		sf_block* frag = NEXT_BLOCK_S(block, effectiveSize);
		frag->header = (remainderSize | PREV_BLOCK_ALLOCATED) ^ MAGIC;
		frag->prev_footer = block->header;
		frag->body.links.next = NULL;
		frag->body.links.prev = NULL;

		// Coalesce and insert the fragment into its proper free list
		frag = coalesce(frag);

		// Link the next block's prev_footer to frag's header
		sf_block* pastFrag = NEXT_BLOCK_S(frag, remainderSize);
		pastFrag->prev_footer = frag->header;
}

void *sf_malloc(sf_size_t size) {
	if (size == 0)
		return NULL;

	// Initialize all the free list dummy head links
	for (size_t c = 0; c<NUM_FREE_LISTS; c++)
		verifyFreeListLinks(c);

	// If the mem_start and mem_end are the same, we have not intialized the heap yet
	if (sf_mem_start() == sf_mem_end()){
		if (initializeHeap() < 0){
			sf_errno = ENOMEM;
			return NULL;
		}
	}

	// Effective size is size+header rounded up to the nearest multiple of 16
	// Must be a minimum of 32 bytes to allow it to become a free list block
	sf_size_t effectiveSize = size + 8;

	effectiveSize += (BLOCK_ALIGN - (effectiveSize % BLOCK_ALIGN)) % BLOCK_ALIGN;
	if (effectiveSize < MIN_BLOCK_SIZE)
		effectiveSize = MIN_BLOCK_SIZE;

	// If the effective size is less than the size, there has been a rollover
	// This happens when the size is too close to the sf_size_t max (32 bits)
	if (effectiveSize < size)
		return NULL;

	sf_header payloadSize = ((sf_header) size) << 32;

	// Check quick lists to see if there is a block of the proper size
	sf_block* block = getQuickListBlock(effectiveSize);

	// If we found a quicklist block, allocate it and return it
	if (block != NULL){
		block->header = (payloadSize | effectiveSize | THIS_BLOCK_ALLOCATED | GET_PREV_ALLOCD(block)) ^ MAGIC;

		// Link the next block's prev_footer to block's header
		sf_block* pastBlock = NEXT_BLOCK_S(block, effectiveSize);
		pastBlock->prev_footer = block->header;

		// Set the next block's prevAllocated to true
		setPrevAllocd(pastBlock, 1);

		// Update the max aggregate payload
		updateMaxAggPayload();

		return ((void*) block) + 2*sizeof(sf_header);
	}

	// If the block is null (not in quick list), we have to check the free lists
	block = getFreeListBlock(effectiveSize);

	// For as long as the block is null extend the heap and check for a new free list block
	while (block == NULL){
		if (extendHeap() < 0){
			sf_errno = ENOMEM;
			return NULL;
		}

		// Try again to get a block from the new freelists
		block = getFreeListBlock(effectiveSize);
	}

	// Now that the block has been created, time to split it if we need
	sf_size_t blockSize = GET_B_SIZE(block);
	sf_size_t remainderSize = blockSize - effectiveSize;
	if (remainderSize >= MIN_BLOCK_SIZE)
		splitBlock(block, payloadSize, effectiveSize, remainderSize);

	// Otherwise allocate the entire block
	else {
		block->header = (payloadSize | blockSize | THIS_BLOCK_ALLOCATED | GET_PREV_ALLOCD(block)) ^ MAGIC;

		// Remove the block from any free lists
		removeFromFreeList(block);

		// Link the next block's prev_footer to block's header
		sf_block* pastBlock = NEXT_BLOCK_S(block, blockSize);
		pastBlock->prev_footer = block->header;

		// Set the next block's prevAllocated to true
		setPrevAllocd(pastBlock, 1);
	}

	// Update the max aggregate payload
	updateMaxAggPayload();

	return ((void*) block) + 2*sizeof(sf_header);
}


/**
 * Checks that a pointer is a valid pointer to be freed acording to the document specifications.
 *
 * @param pp The pointer that will be checked
 * @return A pointer to the block interpreted from this void* pointer or NULL if the heap is uninitialized
 */
sf_block* verifyPointer(void* pp){
	// Check for null pointer and block alignment
	if (pp == NULL)
		abort();
	if (((uintptr_t) pp) % BLOCK_ALIGN != 0)
		abort();

	// Check if the heap is empty, as this will cause other checks to fail
	if (sf_mem_start() == sf_mem_end())
		return NULL;

	// Check for before the start and after the end of the heap
	void* heapStart = sf_mem_start() + 4*ROW_SIZE;
	sf_block* block = (sf_block*)(pp - 2*sizeof(sf_header));
	if ((void*) block < heapStart)
		abort();

	void* blockEnd = ((void*) block) + sizeof(sf_block);
	if (blockEnd >= sf_mem_end()-2*ROW_SIZE)
		abort();


	// Check for block size
	sf_size_t blockSize = GET_B_SIZE(block);
	if (blockSize < MIN_BLOCK_SIZE)
		abort();
	if (blockSize % 16 != 0)
		abort();

	// Check for allocated bit
	if (!IS_ALLOCD(block))
	    abort();

	// Check if PREV_ALLOC is not set (prev free) but header of prev block indicates allocd
	sf_block* prevBlock = PREV_BLOCK(block);
	if (GET_PREV_ALLOCD(block) == 0 && IS_ALLOCD(prevBlock))
		abort();

	return block;
}

void sf_free(void *pp) {
	sf_block* block = verifyPointer(pp);
	if (block == NULL)
		abort();

	sf_size_t blockSize = GET_B_SIZE(block);
	// Now all error conditions are checked time to free the block
	// If the quick list index is in range it can be added to a quick list
	int quickListI = (blockSize-MIN_BLOCK_SIZE) / BLOCK_ALIGN;
	if (quickListI < NUM_QUICK_LISTS){
		insertIntoQuickList(block, blockSize, quickListI);
		return;
	}

	// Otherwise, we need to add it to the proper free list
	block->header = (blockSize | GET_PREV_ALLOCD(block)) ^ MAGIC;
	block->body.links.next = NULL;
	block->body.links.prev = NULL;

	// Update the prev_footer and prevAllocd bit of the next block
	sf_block* nextBlock = NEXT_BLOCK_S(block, blockSize);
	nextBlock->prev_footer = block->header;
	setPrevAllocd(nextBlock, 0);

	nextBlock = coalesce(block);

	// Set the prev_footer again
	nextBlock->prev_footer = block->header;
}

void *sf_realloc(void *pp, sf_size_t rsize) {
	if (rsize == 0){
		sf_free(pp);
		return NULL;
	}

	sf_block* block = verifyPointer(pp);
	if (block == NULL)
		return NULL;

	// Initialize all the free list dummy head links
	for (size_t c = 0; c<NUM_FREE_LISTS; c++)
		verifyFreeListLinks(c);

	// Effective size is size+header rounded up to the nearest multiple of 16
	// Must be a minimum of 32 bytes to allow it to become a free list block
	sf_size_t reffectiveSize = rsize + 8;

	reffectiveSize += (BLOCK_ALIGN - (reffectiveSize % BLOCK_ALIGN)) % BLOCK_ALIGN;
	if (reffectiveSize < MIN_BLOCK_SIZE)
		reffectiveSize = MIN_BLOCK_SIZE;

	// If the effective size is less than the size, there has been a rollover
	// This happens when the size is too close to the sf_size_t max (32 bits)
	if (reffectiveSize < rsize)
		return NULL;

	sf_size_t blockSize = GET_B_SIZE(block);
	// If we are reallocating to a larger size it is easy
	if (reffectiveSize > blockSize){
		// Allocate a larger block
		void* payloadDst = sf_malloc(rsize);
		if (payloadDst == NULL)
			return NULL;

		// memcpy from old payload address to new
		memcpy(payloadDst, pp, rsize);

		// Free the old block
		sf_free(pp);

		// Return the pointer to the new dst
		return payloadDst;
	}

	sf_header payloadSize = ((sf_header) rsize) << 32;
	// If we are reallocating to the same size there is nothing to do except update payload size
	if (reffectiveSize == blockSize){
		block->header = (payloadSize | blockSize | THIS_BLOCK_ALLOCATED | GET_PREV_ALLOCD(block)) ^ MAGIC;
		return pp;
	}

	// If we are reallocating to a smaller size we might need to split
	else {
		sf_size_t remainderSize = blockSize - reffectiveSize;
		// If splitting would leave a splinter just update the payload size
		if (remainderSize < MIN_BLOCK_SIZE)
			block->header = (payloadSize | blockSize | THIS_BLOCK_ALLOCATED | GET_PREV_ALLOCD(block)) ^ MAGIC;
		else
			splitBlock(block, payloadSize, reffectiveSize, remainderSize);
		return pp;
	}
}

double sf_internal_fragmentation() {
	double payloadSum = 0;
	double blockSum = 0;

	if (sf_mem_start() == sf_mem_end())
		return 0;

	// Get pointers to the first block and epilogue, these are our start and stop
	sf_block* currentBlock = (sf_block*)(sf_mem_start() + 4*ROW_SIZE);
	sf_block* epilogue = (sf_block*)(sf_mem_end() - 2*ROW_SIZE);

	// Step through the blocks until we hit the epilogue
	while (currentBlock < epilogue){
		// Check if the current block is allocated and not in the quick list
		if (IS_ALLOCD(currentBlock) && ((currentBlock->header ^ MAGIC) & IN_QUICK_LIST) == 0){
			// If this is the case, get the payload and block size
			sf_size_t blockSize = GET_B_SIZE(currentBlock);
			sf_size_t payloadSize = (sf_size_t)((currentBlock->header^MAGIC) >> 32);

			// Increase the payload and block sums
			payloadSum += payloadSize;
			blockSum += blockSize;
		}

		// Advance the current block to the next block
		currentBlock = NEXT_BLOCK(currentBlock);
	}

	if (blockSum == 0)
		return 0;
	return payloadSum / blockSum;
}

double sf_peak_utilization() {
	// Get the heap size as the difference between the start and end of the heap
	double heapSize = sf_mem_end() - sf_mem_start();
	if (heapSize == 0)
		return 0;

	// Update the max aggregate payload once more
	updateMaxAggPayload();

	return maxAggPayload / heapSize;
}
