#ifndef BUXN_LS_COMPLETION_H
#define BUXN_LS_COMPLETION_H

#include "common.h"
#include "lsp.h"
#include <buxn/asm/asm.h>

struct yyjson_mut_val;
struct yyjson_val;
struct yyjson_mut_doc;
struct buxn_ls_analyzer_s;
struct buxn_ls_src_node_s;

typedef struct {
	struct buxn_ls_src_node_s* source;
	buxn_ls_str_t line;
	bio_lsp_range_t lsp_prefix_range;
	buxn_asm_file_range_t buxn_prefix_range;
	buxn_ls_str_t prefix;
} buxn_ls_completion_ctx_t;

struct yyjson_mut_val*
buxn_ls_build_completion_list(
	const buxn_ls_completion_ctx_t* ctx,
	struct yyjson_mut_doc* response
);

#endif
