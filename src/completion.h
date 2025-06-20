#ifndef BUXN_LS_COMPLETION_H
#define BUXN_LS_COMPLETION_H

#include "common.h"
#include "lsp.h"
#include <buxn/asm/asm.h>
#include <bhash.h>
#include <barray.h>

struct yyjson_mut_val;
struct yyjson_val;
struct yyjson_mut_doc;
struct buxn_ls_analyzer_s;
struct buxn_ls_src_node_s;
struct buxn_ls_sym_node_s;

typedef struct {
	struct barena_s* arena;
	struct buxn_ls_analyzer_s* analyzer;
	struct buxn_ls_src_node_s* source;
	buxn_ls_str_t line_content;
	buxn_ls_str_t prefix;
	bio_lsp_range_t lsp_range;
	int line_number;
	int prefix_start_byte;  // From start of line
	int prefix_end_byte;  // Exclusive
} buxn_ls_completion_ctx_t;

typedef struct {
	const struct buxn_ls_sym_node_s* sym;
	int size;  //  For collapsed item
	bool is_local;
} buxn_ls_completion_item_t;

typedef BHASH_TABLE(buxn_ls_str_t, buxn_ls_completion_item_t) buxn_ls_completion_map_t;

typedef struct {
	buxn_ls_completion_map_t completion_items;
} buxn_ls_completer_t;

void
buxn_ls_completer_init(buxn_ls_completer_t* completer);

void
buxn_ls_completer_cleanup(buxn_ls_completer_t* completer);

struct yyjson_mut_val*
buxn_ls_build_completion_list(
	buxn_ls_completer_t* completer,
	const buxn_ls_completion_ctx_t* ctx,
	struct yyjson_mut_doc* response
);

#endif
