#include <stdio.h>
#include "sfmm.h"

int main(int argc, char const *argv[]) {
//	printf("32: %d\n", bytesToFreeListIndex(32));
//	printf("64: %d\n", bytesToFreeListIndex(64));
//	printf("128: %d\n", bytesToFreeListIndex(128));
//	printf("256: %d\n", bytesToFreeListIndex(256));
//	printf("4096: %d\n", bytesToFreeListIndex(4096));

	size_t sz_w = 8, sz_x = 200, sz_y = 300, sz_z = 4;
	/* void *w = */ sf_malloc(sz_w);
	void *x = sf_malloc(sz_x);
	void *y = sf_malloc(sz_y);
	/* void *z = */ sf_malloc(sz_z);

	sf_show_heap();

	sf_free(y);
	sf_free(x);

    sf_show_heap();

//    sf_free(ptr);

//	sf_show_heap();

    return EXIT_SUCCESS;
}
