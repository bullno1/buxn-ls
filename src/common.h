#ifndef BUXN_LS_COMMON_H
#define BUXN_LS_COMMON_H

#include <bio/bio.h>

typedef int (*bio_entry_fn_t)(void* userdata);

int
bio_enter(bio_entry_fn_t entry, void* userdata);

void*
buxn_ls_realloc(void* ptr, size_t size);

static inline void*
buxn_ls_malloc(size_t size) {
	return buxn_ls_realloc(NULL, size);
}

static inline void
buxn_ls_free(void* ptr) {
	buxn_ls_realloc(ptr, 0);
}

#endif
