#ifndef BUXN_LS_ANALYZE_H
#define BUXN_LS_ANALYZE_H

#include <barena.h>
#include <barray.h>
#include <bhash.h>
#include <buxn/asm/asm.h>
#include "lsp.h"
#include "common.h"
#include "graph.h"

struct buxn_ls_workspace_s;

typedef enum {
	BIO_LSP_DIAGNOSTIC_ERROR = 1,
	BIO_LSP_DIAGNOSTIC_WARNING = 2,
	BIO_LSP_DIAGNOSTIC_INFORMATION = 3,
	BIO_LSP_DIAGNOSTIC_HINT = 4,
} bio_lsp_diagnostic_severity_t;

typedef enum {
	BUXN_LS_SYMBOL_AS_VARIABLE,
	BUXN_LS_SYMBOL_AS_SUBROUTINE,
	BUXN_LS_SYMBOL_AS_DEVICE_PORT,
	BUXN_LS_SYMBOL_AS_ENUM,
} buxn_ls_symbol_semantics_t;

typedef struct {
	bio_lsp_location_t location;
	bio_lsp_location_t related_location;

	bio_lsp_diagnostic_severity_t severity;
	const char* source;
	const char* message;
	const char* related_message;
} buxn_ls_diagnostic_t;

typedef struct buxn_ls_src_node_s buxn_ls_src_node_t;
typedef struct buxn_ls_sym_node_s buxn_ls_sym_node_t;

struct buxn_ls_sym_node_s {
	buxn_ls_sym_node_t* next;

	buxn_ls_str_t name;
	buxn_ls_str_t documentation;
	buxn_ls_str_t signature;
	buxn_ls_src_node_t* source;
	buxn_asm_sym_type_t type;
	buxn_ls_symbol_semantics_t semantics;
	bio_lsp_range_t range;
	uint16_t address;

	buxn_ls_node_base_t base;
};

struct buxn_ls_src_node_s {
	const char* filename;
	const char* uri;
	buxn_ls_sym_node_t* references;
	buxn_ls_sym_node_t* definitions;
	bool analyzed;

	buxn_ls_node_base_t base;
};

struct buxn_asm_file_s {
	buxn_ls_str_t content;
	size_t offset;
};

typedef struct {
	barena_t arena;
	BHASH_TABLE(const char*, buxn_ls_src_node_t*) sources;
} buxn_ls_analyzer_ctx_t;

typedef struct {
	buxn_ls_str_t content;
	buxn_ls_symbol_semantics_t zero_page_semantics;

	int first_line_index;
	int num_lines;
	int first_error_byte;
} buxn_ls_file_t;

typedef struct buxn_ls_analyzer_s {
	buxn_ls_analyzer_ctx_t ctx_a;
	buxn_ls_analyzer_ctx_t ctx_b;
	buxn_ls_analyzer_ctx_t* current_ctx;
	buxn_ls_analyzer_ctx_t* previous_ctx;

	barray(buxn_ls_diagnostic_t) diagnostics;
	BHASH_TABLE(const char*, buxn_ls_file_t) files;
	barray(buxn_ls_str_t) lines;
	barray(buxn_ls_src_node_t*) analyze_queue;

	barray(buxn_ls_sym_node_t*) macro_defs;
	BHASH_TABLE(uint16_t, buxn_ls_sym_node_t*) label_defs;
	barray(buxn_asm_sym_t) references;
} buxn_ls_analyzer_t;

void
buxn_ls_analyzer_init(buxn_ls_analyzer_t* analyzer, barena_pool_t* pool);

void
buxn_ls_analyzer_cleanup(buxn_ls_analyzer_t* analyzer);

void
buxn_ls_analyze(buxn_ls_analyzer_t* analyzer, struct buxn_ls_workspace_s* workspace);

buxn_ls_line_slice_t
buxn_ls_analyzer_split_file(buxn_ls_analyzer_t* analyzer, const char* filename);

#endif
