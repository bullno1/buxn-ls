#ifndef BUXN_LS_ANALYZE_H
#define BUXN_LS_ANALYZE_H

#include <barena.h>
#include <barray.h>
#include <bhash.h>
#include "lsp.h"

struct buxn_ls_workspace_s;

typedef enum {
	BIO_LSP_DIAGNOSTIC_ERROR = 1,
	BIO_LSP_DIAGNOSTIC_WARNING = 2,
	BIO_LSP_DIAGNOSTIC_INFORMATION = 3,
	BIO_LSP_DIAGNOSTIC_HINT = 4,
} bio_lsp_diagnostic_severity_t;

typedef struct {
	bio_lsp_location_t location;
	bio_lsp_location_t related_location;

	bio_lsp_diagnostic_severity_t severity;
	const char* source;
	const char* message;
	const char* related_message;
} buxn_ls_diagnostic_t;

typedef struct {
	const char* chars;
	size_t len;
} buxn_ls_str_t;

struct buxn_asm_file_s {
	buxn_ls_str_t content;
	size_t offset;
	bool in_use;
};

typedef struct {
	barena_t arena;
	barray(buxn_ls_diagnostic_t) diagnostics;
	BHASH_TABLE(const char*, struct buxn_asm_file_s) files;
	barray(buxn_ls_str_t) lines;
} buxn_ls_analyzer_t;

void
buxn_ls_analyzer_init(buxn_ls_analyzer_t* analyzer, barena_pool_t* pool);

void
buxn_ls_analyzer_cleanup(buxn_ls_analyzer_t* analyzer);

void
buxn_ls_analyze(buxn_ls_analyzer_t* analyzer, struct buxn_ls_workspace_s* workspace);

#endif
