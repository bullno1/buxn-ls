#include "common.h"

static inline void*
buxn_dbg_blib_realloc(void* ptr, size_t size, void* ctx) {
	return buxn_ls_realloc(ptr, size);
}

#define BLIB_REALLOC buxn_dbg_blib_realloc
#define BLIB_IMPLEMENTATION
#include <barg.h>
#include <bhash.h>
