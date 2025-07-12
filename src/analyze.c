#include "analyze.h"
#include "workspace.h"
#include "common.h"
#include "lsp.h"
#include <bmacro.h>
#include <buxn/asm/asm.h>
#include <buxn/asm/annotation.h>
#include <buxn/asm/chess.h>
#include <bio/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>

typedef enum {
	BUXN_LS_ANNO_DOC,
	BUXN_LS_ANNO_BUXN_DEVICE,
	BUXN_LS_ANNO_BUXN_MEMORY,
	BUXN_LS_ANNO_BUXN_ENUM,
} buxn_ls_anno_type_t;

struct buxn_asm_ctx_s {
	buxn_ls_src_node_t* entry_node;
	buxn_ls_analyzer_t* analyzer;
	buxn_ls_workspace_t* workspace;
	buxn_asm_sym_t previous_sym;
	buxn_ls_str_t enum_scope;
	buxn_anno_spec_t anno_spec;
	buxn_ls_sym_node_t* current_sym_node;
	buxn_chess_t* chess;
	barena_t chess_arena;
	bool rom_is_empty;
	uint8_t rom[UINT16_MAX + 1 - 256];
};

static int
buxn_ls_cmp_diagnostic(const void* lhs, const void* rhs) {
	const buxn_ls_diagnostic_t* dlhs = lhs;
	const buxn_ls_diagnostic_t* drhs = rhs;
	return strcmp(dlhs->location.uri, drhs->location.uri);
}

buxn_ls_line_slice_t
buxn_ls_analyzer_split_file(buxn_ls_analyzer_t* analyzer, const char* filename) {
	bhash_index_t file_index = bhash_find(&analyzer->files, filename);
	if (!bhash_is_valid(file_index)) {  // Invalid file
		return (buxn_ls_line_slice_t){
			.lines = NULL,
			.num_lines = 0,
		};
	}

	buxn_ls_file_t* file = &analyzer->files.values[file_index];
	if (file->first_line_index >= 0) {
		return (buxn_ls_line_slice_t){
			.lines = &analyzer->lines[file->first_line_index],
			.num_lines = file->num_lines,
		};
	}

	BIO_DEBUG("Splitting file %s", filename);

	buxn_ls_str_t content = analyzer->files.values[file_index].content;
	file->first_line_index = (int)barray_len(analyzer->lines);
	buxn_ls_line_slice_t slice = buxn_ls_split_file(content, &analyzer->lines);
	file->num_lines = slice.num_lines;

	return slice;
}

static bio_lsp_position_t
buxn_ls_convert_position(
	buxn_ls_analyzer_t* analyzer,
	const char* filename,
	buxn_asm_file_pos_t basm_pos
) {
	buxn_ls_line_slice_t line_slice = buxn_ls_analyzer_split_file(analyzer, filename);
	bio_lsp_position_t lsp_pos = { .line = basm_pos.line - 1 };

	if (lsp_pos.line >= line_slice.num_lines) {
		lsp_pos.line = line_slice.num_lines - 1;
		lsp_pos.character = 0;
		return lsp_pos;
	}

	buxn_ls_str_t line = line_slice.lines[lsp_pos.line];
	lsp_pos.character = (int)bio_lsp_utf16_offset_from_byte_offset(
		line.chars, line.len, basm_pos.col - 1
	);

	return lsp_pos;
}

static bio_lsp_range_t
buxn_ls_convert_range(
	buxn_ls_analyzer_t* analyzer,
	const char* filename,
	buxn_asm_file_range_t basm_range
) {
	return (bio_lsp_range_t){
		.start = buxn_ls_convert_position(analyzer, filename, basm_range.start),
		.end = buxn_ls_convert_position(analyzer, filename, basm_range.end),
	};
}

static bio_lsp_location_t
buxn_ls_convert_region(
	buxn_ls_analyzer_t* analyzer,
	buxn_asm_source_region_t basm_region
) {
	bio_lsp_location_t location = {
		.range = buxn_ls_convert_range(
			analyzer, basm_region.filename, basm_region.range
		),
	};

	bhash_index_t src_node_index = bhash_find(
		&analyzer->current_ctx->sources,
		basm_region.filename
	);
	if (bhash_is_valid(src_node_index)) {
		location.uri = analyzer->current_ctx->sources.values[src_node_index]->uri;
	}

	return location;
}

static buxn_ls_src_node_t*
buxn_ls_alloc_node(
	buxn_ls_analyzer_t* analyzer,
	buxn_ls_workspace_t* workspace,
	const char* filename
) {
	buxn_ls_src_node_t* node = barena_memalign(
		&analyzer->current_ctx->arena,
		sizeof(buxn_ls_src_node_t), _Alignof(buxn_ls_src_node_t)
	);

	size_t uri_len = (sizeof("file://") - 1) + workspace->root_dir_len + strlen(filename) + 1;
	char* uri = barena_memalign(&analyzer->current_ctx->arena, uri_len, _Alignof(char));
	// TODO: do we need to url encode?
	snprintf(uri, uri_len, "file://%s%s", workspace->root_dir, filename);
	*node = (buxn_ls_src_node_t){
		.filename = uri + (sizeof("file://") - 1) + workspace->root_dir_len,
		.uri = uri,
	};
	return node;
}

static void
buxn_ls_do_queue_file(
	buxn_ls_analyzer_t* analyzer,
	buxn_ls_workspace_t* workspace,
	const char* filename
) {
	buxn_ls_src_node_t* node = buxn_ls_alloc_node(analyzer, workspace, filename);
	bhash_put(&analyzer->current_ctx->sources, node->filename, node);
	barray_push(analyzer->analyze_queue, node, NULL);
}

static void
buxn_ls_maybe_queue_node(
	buxn_ls_analyzer_t* analyzer,
	buxn_ls_workspace_t* workspace,
	buxn_ls_src_node_t* node
) {
	bhash_index_t node_index = bhash_find(&analyzer->current_ctx->sources, node->filename);
	if (bhash_is_valid(node_index)) { return; }  // Already visited

	bhash_index_t doc_index = bhash_find(&workspace->docs, (char*){ (char*)node->filename });
	if (bhash_is_valid(doc_index)) {  // The document is opened
		buxn_ls_do_queue_file(analyzer, workspace, node->filename);
	}

	// Queue all children
	for (
		buxn_ls_edge_t* edge = node->base.out_edges;
		edge != NULL;
		edge = edge->next_out
	) {
		buxn_ls_maybe_queue_node(
			analyzer,
			workspace,
			BCONTAINER_OF(edge->to, buxn_ls_src_node_t, base)
		);
	}
}

static void
buxn_ls_queue_from_root(
	buxn_ls_analyzer_t* analyzer,
	buxn_ls_workspace_t* workspace,
	buxn_ls_src_node_t* node
) {
	if (node->base.in_edges != NULL) {
		for (
			buxn_ls_edge_t* edge = node->base.in_edges;
			edge != NULL;
			edge = edge->next_in
		) {
			buxn_ls_queue_from_root(
				analyzer,
				workspace,
				BCONTAINER_OF(edge->from, buxn_ls_src_node_t, base)
			);
		}
	} else {
		buxn_ls_maybe_queue_node(analyzer, workspace, node);
	}
}

static buxn_ls_sym_node_t*
buxn_ls_make_sym_node(
	buxn_ls_analyzer_t* analyzer,
	const buxn_asm_sym_t* sym
) {
	bhash_index_t src_node_index = bhash_find(
		&analyzer->current_ctx->sources,
		sym->region.filename
	);
	assert(bhash_is_valid(src_node_index) && "Symbol comes from unopened file");
	buxn_ls_src_node_t* src_node = analyzer->current_ctx->sources.values[src_node_index];

	buxn_ls_sym_node_t* sym_node = barena_memalign(
		&analyzer->current_ctx->arena,
		sizeof(buxn_ls_sym_node_t), _Alignof(buxn_ls_sym_node_t)
	);
	*sym_node = (buxn_ls_sym_node_t){
		.type = sym->type,
		.name = {
			.chars = sym->name,
			.len = strlen(sym->name),
		},
		.source = src_node,
		.byte_offset = sym->region.range.start.byte,
		.range = buxn_ls_convert_range(
			analyzer, sym->region.filename, sym->region.range
		),
	};
	return sym_node;
}

static buxn_ls_file_t*
buxn_ls_find_file(buxn_ls_analyzer_t* analyzer, const char* filename) {
	bhash_index_t file_index = bhash_find(&analyzer->files, filename);
	if (bhash_is_valid(file_index)) {
		return &analyzer->files.values[file_index];
	} else {
		return NULL;
	}
}

static buxn_ls_str_t
buxn_ls_slice_file(buxn_ls_analyzer_t* analyzer, const buxn_asm_source_region_t* region) {
	buxn_ls_file_t* file = buxn_ls_find_file(analyzer, region->filename);
	if (file == NULL) {
		return (buxn_ls_str_t){ 0 };
	} else {
		return (buxn_ls_str_t){
			.chars = file->content.chars + region->range.start.byte,
				.len = region->range.end.byte - region->range.start.byte,
		};
	}
}

static void
buxn_ls_handle_annotation(
	void* anno_ctx,
	uint16_t addr,
	const buxn_asm_sym_t* sym,
	const buxn_anno_t* annotation,
	const buxn_asm_source_region_t* region
) {
	buxn_asm_ctx_t* ctx = anno_ctx;

	if (annotation != NULL) {
		buxn_ls_file_t* file = buxn_ls_find_file(ctx->analyzer, region->filename);
		assert((file != NULL) && "Annotation comes from unopened file");

		switch ((buxn_ls_anno_type_t)(annotation - ctx->anno_spec.annotations)) {
			case BUXN_LS_ANNO_DOC:
				ctx->current_sym_node->documentation = buxn_ls_slice_file(ctx->analyzer, region);
				break;
			case BUXN_LS_ANNO_BUXN_DEVICE:
				file->zero_page_semantics = BUXN_LS_SYMBOL_AS_DEVICE_PORT;
				break;
			case BUXN_LS_ANNO_BUXN_MEMORY:
				file->zero_page_semantics = BUXN_LS_SYMBOL_AS_VARIABLE;
				break;
			case BUXN_LS_ANNO_BUXN_ENUM:
				ctx->current_sym_node->semantics = BUXN_LS_SYMBOL_AS_ENUM;
				ctx->enum_scope = buxn_ls_label_scope(ctx->current_sym_node->name);
				break;
		}
	} else {
		ctx->current_sym_node->semantics = BUXN_LS_SYMBOL_AS_SUBROUTINE;
		ctx->current_sym_node->signature = buxn_ls_slice_file(ctx->analyzer, region);
	}
}

static void
buxn_ls_init_analyzer_ctx(buxn_ls_analyzer_ctx_t* ctx, barena_pool_t* pool) {
	barena_init(&ctx->arena, pool);

	bhash_config_t hash_config = bhash_config_default();
	hash_config.eq = buxn_ls_str_eq;
	hash_config.hash = buxn_ls_str_hash;
	hash_config.removable = false;
	bhash_init(&ctx->sources, hash_config);
}

static void
buxn_ls_reset_analyzer_ctx(buxn_ls_analyzer_ctx_t* ctx) {
	barena_reset(&ctx->arena);
	bhash_clear(&ctx->sources);
}

static void
buxn_ls_cleanup_analyzer_ctx(buxn_ls_analyzer_ctx_t* ctx) {
	bhash_cleanup(&ctx->sources);
	barena_reset(&ctx->arena);
}

void
buxn_ls_analyzer_init(buxn_ls_analyzer_t* analyzer, barena_pool_t* pool) {
	analyzer->arena_pool = pool;
	buxn_ls_init_analyzer_ctx(&analyzer->ctx_a, pool);
	buxn_ls_init_analyzer_ctx(&analyzer->ctx_b, pool);
	analyzer->current_ctx = &analyzer->ctx_a;
	analyzer->previous_ctx = &analyzer->ctx_b;

	bhash_config_t hash_config = bhash_config_default();
	hash_config.removable = false;
	bhash_init(&analyzer->label_defs, hash_config);

	hash_config.eq = buxn_ls_str_eq;
	hash_config.hash = buxn_ls_str_hash;
	bhash_init(&analyzer->files, hash_config);
}

void
buxn_ls_analyzer_cleanup(buxn_ls_analyzer_t* analyzer) {
	barray_free(NULL, analyzer->macro_defs);
	barray_free(NULL, analyzer->references);
	barray_free(NULL, analyzer->diagnostics);
	barray_free(NULL, analyzer->lines);
	barray_free(NULL, analyzer->analyze_queue);

	bhash_cleanup(&analyzer->label_defs);
	bhash_cleanup(&analyzer->files);
	buxn_ls_cleanup_analyzer_ctx(&analyzer->ctx_a);
	buxn_ls_cleanup_analyzer_ctx(&analyzer->ctx_b);
}

void
buxn_ls_analyze(buxn_ls_analyzer_t* analyzer, buxn_ls_workspace_t* workspace) {
	{
		buxn_ls_reset_analyzer_ctx(analyzer->previous_ctx);
		buxn_ls_analyzer_ctx_t* tmp = analyzer->current_ctx;
		analyzer->current_ctx = analyzer->previous_ctx;
		analyzer->previous_ctx = tmp;
	}
	barray_clear(analyzer->analyze_queue);

	// Based on dependency of files in the previous run, try to figure out in
	// what order the files should be compiled.
	bhash_index_t num_docs = bhash_len(&workspace->docs);
	for (bhash_index_t doc_index = 0 ; doc_index < num_docs; ++doc_index) {
		const char* filename = workspace->docs.keys[doc_index];
		bhash_index_t current_node_index = bhash_find(&analyzer->current_ctx->sources, filename);
		if (bhash_is_valid(current_node_index)) {  // File is already added
			continue;
		}

		bhash_index_t previous_node_index = bhash_find(&analyzer->previous_ctx->sources, filename);
		if (bhash_is_valid(previous_node_index)) {  // We saw this file before
			buxn_ls_queue_from_root(
				analyzer,
				workspace,
				analyzer->previous_ctx->sources.values[previous_node_index]
			);
		} else {  // This was never seen before
			buxn_ls_do_queue_file(analyzer, workspace, filename);
		}
	}

	// Analyze files in order
	barray_clear(analyzer->lines);
	bhash_clear(&analyzer->files);
	barray_clear(analyzer->diagnostics);
	for (size_t i = 0; i < barray_len(analyzer->analyze_queue); ++i) {
		buxn_ls_src_node_t* node = analyzer->analyze_queue[i];
		if (!node->analyzed) {
			BIO_INFO("Analyzing %s", node->filename);

			barray_clear(analyzer->macro_defs);
			bhash_clear(&analyzer->label_defs);
			barray_clear(analyzer->references);

			buxn_anno_t annotations[] = {
				[BUXN_LS_ANNO_DOC] = {
					.type = BUXN_ANNOTATION_PREFIX,
					.name = "doc",
				},
				[BUXN_LS_ANNO_BUXN_DEVICE] = {
					.type = BUXN_ANNOTATION_IMMEDIATE,
					.name = "buxn:device",
				},
				[BUXN_LS_ANNO_BUXN_MEMORY] = {
					.type = BUXN_ANNOTATION_IMMEDIATE,
					.name = "buxn:memory",
				},
				[BUXN_LS_ANNO_BUXN_ENUM] = {
					.type = BUXN_ANNOTATION_PREFIX,
					.name = "buxn:enum",
				},
			};
			buxn_asm_ctx_t ctx = {
				.entry_node = node,
				.analyzer = analyzer,
				.workspace = workspace,
				.anno_spec = {
					.annotations = annotations,
					.num_annotations = BCOUNT_OF(annotations),
					.ctx = &ctx,
					.handler = buxn_ls_handle_annotation,
				},
			};
			barena_init(&ctx.chess_arena, analyzer->arena_pool);
			ctx.chess = buxn_chess_begin(&ctx);
			bool success = buxn_asm(&ctx, node->filename);
			if (success && !ctx.rom_is_empty) {
				buxn_chess_end(ctx.chess);
			}
			barena_reset(&ctx.chess_arena);

			// Bring forward old symbols in files with error to have some degree
			// of error tolerance
			bhash_index_t num_files = bhash_len(&analyzer->files);
			for (bhash_index_t file_index = 0; file_index < num_files; ++file_index) {
				buxn_ls_file_t* file = &analyzer->files.values[file_index];
				if (!file->has_error) { continue; }

				bhash_index_t previous_src_index = bhash_find(
					&analyzer->previous_ctx->sources, analyzer->files.keys[file_index]
				);
				if (!bhash_is_valid(previous_src_index)) { continue; }
				buxn_ls_src_node_t* previous_src_node = analyzer->previous_ctx->sources.values[previous_src_index];

				bhash_index_t current_src_index = bhash_find(
					&analyzer->current_ctx->sources, analyzer->files.keys[file_index]
				);
				if (!bhash_is_valid(current_src_index)) { continue; }
				buxn_ls_src_node_t* current_src_node = analyzer->current_ctx->sources.values[current_src_index];

				for (
					buxn_ls_sym_node_t* sym_node = previous_src_node->definitions;
					sym_node != NULL;
					sym_node = sym_node->next
				) {
					if (sym_node->byte_offset <= file->last_symbol_byte) {
						continue;
					}

					// Symbol appears after error
					buxn_ls_sym_node_t* sym_copy = barena_memalign(
						&analyzer->current_ctx->arena,
						sizeof(buxn_ls_sym_node_t), _Alignof(buxn_ls_sym_node_t)
					);
					*sym_copy = (buxn_ls_sym_node_t){
						.name = buxn_ls_arena_cstrcpy(
							&analyzer->current_ctx->arena,
							sym_node->name
						),
						.documentation = buxn_ls_arena_cstrcpy(
							&analyzer->current_ctx->arena,
							sym_node->documentation
						),
						.signature = buxn_ls_arena_cstrcpy(
							&analyzer->current_ctx->arena,
							sym_node->signature
						),
						.source = current_src_node,
						.type = sym_node->type,
						.semantics = sym_node->semantics,
						.byte_offset = sym_node->byte_offset,
						.range = sym_node->range,
						.address = sym_node->address,
					};

					sym_copy->next = sym_copy->source->definitions;
					sym_copy->source->definitions = sym_copy;
				}
			}

			// Connect references to definitions
			size_t num_refs = barray_len(analyzer->references);
			for (size_t sym_index = 0; sym_index < num_refs; ++sym_index) {
				const buxn_asm_sym_t* sym = &analyzer->references[sym_index];

				buxn_ls_sym_node_t* def_node = NULL;
				if (sym->type == BUXN_ASM_SYM_MACRO_REF) {
					// Macros cannot be forward declared so references can only
					// be resolved when a macro is already declared
					def_node = analyzer->macro_defs[sym->id - 1];
				} else if (sym->type == BUXN_ASM_SYM_LABEL_REF) {
					bhash_index_t def_index = bhash_find(&analyzer->label_defs, sym->id);
					if (bhash_is_valid(def_index)) {
						def_node = analyzer->label_defs.values[def_index];
					}
				}

				if (def_node == NULL) {  // Unresolved reference
					continue;
				}

				buxn_ls_sym_node_t* ref_node = buxn_ls_make_sym_node(analyzer, sym);
				ref_node->next = ref_node->source->references;
				ref_node->source->references = ref_node;
				buxn_ls_graph_add_edge(
					&analyzer->current_ctx->arena,
					&ref_node->base,
					&def_node->base
				);
				/*BIO_TRACE(*/
					/*"Connecting reference for %s"*/
					/*" from %s:%d:%d:%d:%d"*/
					/*" to %s:%d:%d:%d:%d",*/
					/*sym->name,*/

					/*ref_node->source->uri,*/
					/*ref_node->range.start.line, ref_node->range.start.character,*/
					/*ref_node->range.end.line, ref_node->range.end.character,*/

					/*def_node->source->uri,*/
					/*def_node->range.start.line, def_node->range.start.character,*/
					/*def_node->range.end.line, def_node->range.end.character*/
				/*);*/
			}
		} else {
			BIO_INFO("Skipping %s", node->filename);
		}
	}

	// Sort diagnostics so that messages for the same file are grouped together
	size_t num_diags = barray_len(analyzer->diagnostics);
	if (num_diags > 0) {
		qsort(
			analyzer->diagnostics,
			num_diags, sizeof(analyzer->diagnostics[0]),
			buxn_ls_cmp_diagnostic
		);
	}
}

void*
buxn_asm_alloc(buxn_asm_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(&ctx->analyzer->current_ctx->arena, size, alignment);
}

void
buxn_asm_report(buxn_asm_ctx_t* ctx, buxn_asm_report_type_t type, const buxn_asm_report_t* report) {
	buxn_ls_analyzer_t* analyzer = ctx->analyzer;

	// Only save reports about source regions, not top level reports
	if (report->region->range.start.line == 0) { return; }

	if (type == BUXN_ASM_REPORT_ERROR) {
		bhash_index_t file_index = bhash_find(&analyzer->files, report->region->filename);
		if (bhash_is_valid(file_index)) {
			buxn_ls_file_t* file = &analyzer->files.values[file_index];
			file->has_error = true;
		}
	}

	buxn_ls_diagnostic_t diag = {
		.location = buxn_ls_convert_region(ctx->analyzer, *report->region),
		.message = buxn_ls_arena_strcpy(&ctx->analyzer->current_ctx->arena, report->message),
		.source = "buxn-asm",
	};
	switch (type) {
		case BUXN_ASM_REPORT_WARNING:
			diag.severity = BIO_LSP_DIAGNOSTIC_WARNING;
			break;
		case BUXN_ASM_REPORT_ERROR:
			diag.severity = BIO_LSP_DIAGNOSTIC_ERROR;
			break;
		default:
			diag.severity = BIO_LSP_DIAGNOSTIC_INFORMATION;
			break;
	}
	if (
		report->related_message != NULL
		&& report->related_region->filename == report->region->filename
	) {
		diag.related_location = buxn_ls_convert_region(ctx->analyzer, *report->region);
		diag.related_message = buxn_ls_arena_strcpy(&ctx->analyzer->current_ctx->arena, report->related_message);
	}

	barray_push(analyzer->diagnostics, diag, NULL);
}

void
buxn_asm_put_rom(buxn_asm_ctx_t* ctx, uint16_t addr, uint8_t value) {
	ctx->rom[addr - 256] = value;
	ctx->rom_is_empty = false;
}

void
buxn_asm_put_symbol(buxn_asm_ctx_t* ctx, uint16_t addr, const buxn_asm_sym_t* sym) {
	buxn_chess_handle_symbol(ctx->chess, addr, sym);

	// When an address reference is 16 bit, there will be two identical symbols
	// emitted for both bytes.
	// We should only consider the first symbol.
	if (
		sym->type == ctx->previous_sym.type
		&& sym->id == ctx->previous_sym.id
		&& sym->region.filename == ctx->previous_sym.region.filename
		&& sym->region.range.start.byte == ctx->previous_sym.region.range.start.byte
		&& sym->region.range.end.byte == ctx->previous_sym.region.range.end.byte
	) {
		return;
	}

	buxn_ls_analyzer_t* analyzer = ctx->analyzer;
	switch (sym->type) {
		case BUXN_ASM_SYM_MACRO:
		case BUXN_ASM_SYM_LABEL: {
			buxn_ls_file_t* file = buxn_ls_find_file(analyzer, sym->region.filename);
			assert((file != NULL) && "Symbol comes from unopened file");
			if (sym->region.range.start.byte > file->last_symbol_byte) {
				file->last_symbol_byte = sym->region.range.start.byte;
			}

			if (!sym->name_is_generated) {
				buxn_ls_sym_node_t* sym_node = buxn_ls_make_sym_node(analyzer, sym);
				sym_node->next = sym_node->source->definitions;
				sym_node->source->definitions = sym_node;
				if (sym->type == BUXN_ASM_SYM_LABEL) {
					sym_node->address = addr;

					if (addr <= 0x00ff) {  // Zero-page
						buxn_ls_str_t scope = buxn_ls_label_scope(sym_node->name);
						if (
							ctx->enum_scope.len > 0
							&& buxn_ls_cstr_eq(&scope, &ctx->enum_scope, 0)
						) {
							sym_node->semantics = BUXN_LS_SYMBOL_AS_ENUM;
						} else {
							sym_node->semantics = file->zero_page_semantics;
							ctx->enum_scope.len = 0;
						}
					}

					uint16_t id = sym->id;
					bhash_put(&analyzer->label_defs, id, sym_node);
				} else {
					sym_node->semantics = BUXN_LS_SYMBOL_AS_SUBROUTINE;
					barray_push(analyzer->macro_defs, sym_node, NULL);
				}
				ctx->current_sym_node = sym_node;
			}
		} break;
		case BUXN_ASM_SYM_MACRO_REF:
		case BUXN_ASM_SYM_LABEL_REF:
			barray_push(analyzer->references, *sym, NULL);
			break;
		default:
			break;
	}
	ctx->previous_sym = *sym;

	buxn_anno_handle_symbol(&ctx->anno_spec, addr, sym);
}

buxn_asm_file_t*
buxn_asm_fopen(buxn_asm_ctx_t* ctx, const char* filename) {
	buxn_ls_analyzer_t* analyzer = ctx->analyzer;
	bhash_index_t file_index = bhash_find(&analyzer->files, filename);
	buxn_ls_str_t content;
	if (bhash_is_valid(file_index)) {  // File is already read
		content = analyzer->files.values[file_index].content;
	} else {  // New file
		bhash_index_t doc_index = bhash_find(&ctx->workspace->docs, (char*){ (char*)filename });
		if (bhash_is_valid(doc_index)) {  // File is managed
			// Make a copy so that even if workspace gets updated, we analyze
			// based on the current content
			buxn_ls_str_t content_str = ctx->workspace->docs.values[doc_index];
			char* content_copy = barena_memalign(
				&ctx->analyzer->current_ctx->arena,
				content_str.len,
				_Alignof(char)
			);
			memcpy(content_copy, content_str.chars, content_str.len);
			content = (buxn_ls_str_t){
				.chars = content_copy,
				.len = content_str.len,
			};
		} else {  // File is unmanaged
			bio_file_t fd;
			bio_error_t error = { 0 };
			char full_path[1024];
			snprintf(full_path, sizeof(full_path), "%s%s", ctx->workspace->root_dir, filename);
			if (!bio_fopen(&fd, full_path, "r", &error)) {
				BIO_ERROR("Could not open %s: " BIO_ERROR_FMT, full_path, BIO_ERROR_FMT_ARGS(&error));
				return NULL;
			}

			bio_stat_t stat;
			if (!bio_fstat(fd, &stat,&error)) {
				BIO_ERROR("Could not stat %s: " BIO_ERROR_FMT, full_path, BIO_ERROR_FMT_ARGS(&error));
				bio_fclose(fd, NULL);
				return NULL;
			}

			char* read_buf = barena_memalign(&ctx->analyzer->current_ctx->arena, stat.size, _Alignof(char));
			if (bio_fread_exactly(fd, read_buf, stat.size, &error) != stat.size) {
				BIO_ERROR("Error while reading %s: " BIO_ERROR_FMT, full_path, BIO_ERROR_FMT_ARGS(&error));
				bio_fclose(fd, NULL);
				return NULL;
			}

			content = (buxn_ls_str_t){
				.chars = read_buf,
				.len = stat.size,
			};
			bio_fclose(fd, NULL);
		}

		buxn_ls_file_t file = {
			.content = content,
			.zero_page_semantics = BUXN_LS_SYMBOL_AS_VARIABLE,
			.first_line_index = -1,
			.has_error = false,
		};
		bhash_put(&analyzer->files, filename, file);
	}

	buxn_asm_file_t* file = buxn_ls_malloc(sizeof(buxn_asm_file_t));
	*file = (buxn_asm_file_t){ .content = content };

	bhash_alloc_result_t alloc_result = bhash_alloc(&analyzer->current_ctx->sources, filename);
	buxn_ls_src_node_t* node;
	if (alloc_result.is_new) {
		node = buxn_ls_alloc_node(analyzer, ctx->workspace, filename);
		analyzer->current_ctx->sources.keys[alloc_result.index] = node->filename;
		analyzer->current_ctx->sources.values[alloc_result.index] = node;
	} else {
		node = analyzer->current_ctx->sources.values[alloc_result.index];
	}
	node->analyzed = true;

	if (node != ctx->entry_node) {
		buxn_ls_graph_add_edge(
			&analyzer->current_ctx->arena,
			&ctx->entry_node->base,
			&node->base
		);
	}

	return file;
}

void
buxn_asm_fclose(buxn_asm_ctx_t* ctx, buxn_asm_file_t* file) {
	(void)ctx;
	buxn_ls_free(file);
}

int
buxn_asm_fgetc(buxn_asm_ctx_t* ctx, buxn_asm_file_t* file) {
	if (file->offset < file->content.len) {
		return (int)file->content.chars[file->offset++];
	} else {
		return EOF;
	}
}

void*
buxn_chess_alloc(buxn_asm_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(&ctx->chess_arena, size, alignment);
}

void*
buxn_chess_begin_mem_region(buxn_asm_ctx_t* ctx) {
	return (void*)barena_snapshot(&ctx->chess_arena);
}

void
buxn_chess_end_mem_region(buxn_asm_ctx_t* ctx, void* region) {
	barena_restore(&ctx->chess_arena, (barena_snapshot_t)region);
}

uint8_t
buxn_chess_get_rom(buxn_asm_ctx_t* ctx, uint16_t address) {
	return ctx->rom[address - 256];
}

void
buxn_chess_report(
	buxn_asm_ctx_t* ctx,
	buxn_chess_id_t trace_id,
	buxn_chess_report_type_t type,
	const buxn_asm_report_t* report
) {
	buxn_ls_analyzer_t* analyzer = ctx->analyzer;

	// Only save reports about source regions, not top level reports
	if (report->region->range.start.line == 0) { return; }

	buxn_ls_diagnostic_t diag = { 0 };

	switch (type) {
		case BUXN_CHESS_REPORT_WARNING:
			diag.severity = BIO_LSP_DIAGNOSTIC_WARNING;
			break;
		case BUXN_CHESS_REPORT_ERROR:
			diag.severity = BIO_LSP_DIAGNOSTIC_ERROR;
			break;
		default:
			return;
	}

	if (type == BUXN_CHESS_REPORT_ERROR) {
		bhash_index_t file_index = bhash_find(&analyzer->files, report->region->filename);
		if (bhash_is_valid(file_index)) {
			buxn_ls_file_t* file = &analyzer->files.values[file_index];
			file->has_error = true;
		}
	}

	if (trace_id != BUXN_CHESS_NO_TRACE) {
		diag = (buxn_ls_diagnostic_t){
			.location = buxn_ls_convert_region(ctx->analyzer, *report->region),
			.message = buxn_ls_arena_fmt(
				&ctx->analyzer->current_ctx->arena,
				"[%d] %s", trace_id, report->message
			).chars,
			.source = "buxn-chess",
		};
	} else {
		diag = (buxn_ls_diagnostic_t){
			.location = buxn_ls_convert_region(ctx->analyzer, *report->region),
			.message = buxn_ls_arena_strcpy(
				&ctx->analyzer->current_ctx->arena,
				report->message
			),
			.source = "buxn-chess",
		};
	}

	if (
		report->related_message != NULL
		&& report->related_region->filename == report->region->filename
	) {
		diag.related_location = buxn_ls_convert_region(ctx->analyzer, *report->region);
		diag.related_message = buxn_ls_arena_strcpy(&ctx->analyzer->current_ctx->arena, report->related_message);
	}

	barray_push(analyzer->diagnostics, diag, NULL);
}

void
buxn_chess_deo(
	buxn_asm_ctx_t* ctx,
	buxn_chess_id_t trace_id,
	const buxn_chess_vm_state_t* state,
	uint8_t value,
	uint8_t port
) {
	if (port == 0x0e && value == 0x2b) {
		void* mem_region = buxn_chess_begin_mem_region(ctx);

		buxn_chess_str_t wst_str = buxn_chess_format_stack(
			ctx->chess, state->wst.content, state->wst.len
		);
		buxn_chess_str_t rst_str = buxn_chess_format_stack(
			ctx->chess, state->rst.content, state->rst.len
		);

		buxn_ls_diagnostic_t diag = {
			.severity = BIO_LSP_DIAGNOSTIC_INFORMATION,
			.location = buxn_ls_convert_region(ctx->analyzer, state->src_region),
			.message = buxn_ls_arena_fmt(
				&ctx->analyzer->current_ctx->arena,
				"[%d] Stack:\nWST(%d):%.*s\nRST(%d):%.*s",
				trace_id,
				state->wst.size, wst_str.len, wst_str.chars,
				state->rst.size, rst_str.len, rst_str.chars
			).chars,
			.source = "buxn-chess",
		};
		barray_push(ctx->analyzer->diagnostics, diag, NULL);

		buxn_chess_end_mem_region(ctx, mem_region);
	}
}

void
buxn_chess_begin_trace(
	buxn_asm_ctx_t* ctx,
	buxn_chess_id_t trace_id,
	buxn_chess_id_t parent_id
) {
	(void)ctx;
	(void)trace_id;
	(void)parent_id;
}

extern void
buxn_chess_end_trace(
	buxn_asm_ctx_t* ctx,
	buxn_chess_id_t trace_id,
	bool success
) {
	(void)ctx;
	(void)trace_id;
	(void)success;
}
