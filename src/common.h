#ifndef BUXN_LS_COMMON_H
#define BUXN_LS_COMMON_H

#include "lsp.h"
#include <bio/bio.h>
#include <string.h>
#include <bhash.h>
#include <barena.h>
#include <barray.h>
#include <yyjson.h>

typedef int (*bio_entry_fn_t)(void* userdata);

typedef struct {
	const char* chars;
	size_t len;
} buxn_ls_str_t;

typedef struct {
	buxn_ls_str_t* lines;
	int num_lines;
} buxn_ls_line_slice_t;

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

static inline char*
buxn_ls_arena_strcpy(barena_t* arena, const char* str) {
	size_t len = strlen(str);
	char* copy = barena_memalign(arena, len + 1, _Alignof(char));
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

static inline buxn_ls_str_t
buxn_ls_label_scope(buxn_ls_str_t name) {
	int i;
	for (i = 0; i < (int)name.len; ++i) {
		if (name.chars[i] == '/') { break; }
	}
	return (buxn_ls_str_t){
		.chars = name.chars,
		.len = i,
	};
}

static inline bool
buxn_ls_cstr_eq(const void* lhs, const void* rhs, size_t size) {
	(void)size;
	const buxn_ls_str_t* lstr = lhs;
	const buxn_ls_str_t* rstr = rhs;
	return lstr->len == rstr->len
		&& memcmp(lstr->chars, rstr->chars, lstr->len) == 0;
}

static inline bhash_hash_t
buxn_ls_cstr_hash(const void* key, size_t size) {
	(void)size;
	const buxn_ls_str_t* str = key;
	return bhash_hash(str->chars, str->len);
}

static inline buxn_ls_str_t
buxn_ls_arena_vfmt(barena_t* arena, const char* fmt, va_list args) {
	// TODO: use a reusable buffer and copy to arena instead of double formatting
	va_list args_copy;
	va_copy(args_copy, args);
	int strlen = vsnprintf(NULL, 0, fmt, args_copy);
	va_end(args_copy);

	char* str = barena_memalign(arena, strlen + 1, _Alignof(char));
	vsnprintf(str, strlen + 1, fmt, args);

	return (buxn_ls_str_t){
		.chars = str,
		.len = strlen,
	};
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static inline buxn_ls_str_t
buxn_ls_arena_fmt(barena_t* arena, const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	buxn_ls_str_t str = buxn_ls_arena_vfmt(arena, fmt, args);
	va_end(args);

	return str;
}

static inline void
buxn_ls_serialize_lsp_position(
	yyjson_mut_doc* doc,
	yyjson_mut_val* json,
	const bio_lsp_position_t* position
) {
	yyjson_mut_obj_add_int(doc, json, "line", position->line);
	yyjson_mut_obj_add_int(doc, json, "character", position->character);
}

static inline void
buxn_ls_serialize_lsp_range(
	yyjson_mut_doc* doc,
	yyjson_mut_val* json,
	const bio_lsp_range_t* range
) {
	buxn_ls_serialize_lsp_position(doc, yyjson_mut_obj_add_obj(doc, json, "start"), &range->start);
	buxn_ls_serialize_lsp_position(doc, yyjson_mut_obj_add_obj(doc, json, "end"), &range->end);
}

buxn_ls_line_slice_t
buxn_ls_split_file(buxn_ls_str_t content, barray(buxn_ls_str_t)* lines);

#endif
