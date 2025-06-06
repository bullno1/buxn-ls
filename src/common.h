#ifndef BUXN_LS_COMMON_H
#define BUXN_LS_COMMON_H

#include <bio/bio.h>
#include <string.h>
#include <bhash.h>

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

static inline char*
buxn_ls_strcpy(const char* str) {
	size_t len = strlen(str);
	char* copy = buxn_ls_malloc(len + 1);
	memcpy(copy, str, len + 1);
	return copy;
}

static inline bhash_hash_t
buxn_ls_str_hash(const void* key, size_t size) {
	(void)size;
	return bhash_hash(*(const char**)key, strlen(*(const char**)key));
}

static inline bool
buxn_ls_str_eq(const void* lhs, const void* rhs, size_t size) {
	(void)size;
	return strcmp(*(const char**)lhs, *(const char**)rhs) == 0;
}

#endif
