#include "analyze.h"
#include "workspace.h"
#include "common.h"
#include <buxn/asm/asm.h>
#include <bio/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <utf8proc.h>

struct buxn_asm_ctx_s {
	buxn_ls_node_t* entry_node;
	buxn_ls_analyzer_t* analyzer;
	buxn_ls_workspace_t* workspace;
};

static bio_lsp_location_t
buxn_source_region_to_lsp_location(const buxn_asm_source_region_t* region) {
	return (bio_lsp_location_t){
		.uri = region->filename,
		.range = {
			.start = {
				.line = region->range.start.line,
				.character = region->range.start.col,
			},
			.end = {
				.line = region->range.end.line,
				.character = region->range.end.col,
			},
		},
	};
}

static int
buxn_ls_cmp_diagnostic(const void* lhs, const void* rhs) {
	const buxn_ls_diagnostic_t* dlhs = lhs;
	const buxn_ls_diagnostic_t* drhs = rhs;
	return strcmp(dlhs->location.uri, drhs->location.uri);
}

buxn_ls_line_slice_t
buxn_ls_split_file(buxn_ls_analyzer_t* analyzer, const char* filename) {
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
	buxn_ls_str_t line = { .chars = content.chars, };
	int first_line_index = (int)barray_len(analyzer->lines);
	int num_lines = 0;
	size_t start_index = 0;
	for (size_t char_index = 0; char_index < content.len; ++char_index) {
		char ch = content.chars[char_index];
		if (ch == '\n') {
			line.len = char_index - start_index;
			barray_push(analyzer->lines, line, NULL);
			++num_lines;
			line.chars = content.chars + char_index + 1;
			start_index = char_index + 1;
		} else if (ch == '\r') {
			if (char_index < content.len - 1 && content.chars[char_index + 1] == '\n') {
				line.len = char_index - start_index;
				barray_push(analyzer->lines, line, NULL);
				++num_lines;
				line.chars = content.chars + char_index + 2;
				start_index = char_index + 2;
			} else {
				line.len = char_index - start_index;
				barray_push(analyzer->lines, line, NULL);
				++num_lines;
				line.chars = content.chars + char_index + 1;
				start_index = char_index + 1;
			}
		}
	}
	// Last line
	if (start_index < content.len) {
		line.len = content.len - start_index;
		barray_push(analyzer->lines, line, NULL);
		++num_lines;
	}

	file->first_line_index = first_line_index;
	file->num_lines = num_lines;

	return (buxn_ls_line_slice_t){
		.lines = &analyzer->lines[first_line_index],
		.num_lines = num_lines,
	};
}

static void
buxn_ls_convert_position(
	buxn_ls_analyzer_t* analyzer,
	const char* filename,
	bio_lsp_position_t* pos
) {
	buxn_ls_line_slice_t line_slice = buxn_ls_split_file(analyzer, filename);

	pos->line -= 1;
	if (pos->line >= line_slice.num_lines) {
		pos->line = line_slice.num_lines - 1;
		pos->character = 0;
		return;
	}

	buxn_ls_str_t line = line_slice.lines[pos->line];
	int utf8_offset = pos->character - 1;
	utf8proc_ssize_t byte_offset = 0;
	utf8proc_ssize_t line_len = line.len;
	int code_unit_offset = 0;
	while (true) {
		if (byte_offset >= utf8_offset || byte_offset >= line_len) { break; }

		utf8proc_int32_t codepoint;
		utf8proc_ssize_t num_bytes = utf8proc_iterate(
			(const utf8proc_uint8_t*)line.chars + byte_offset,
			line_len - byte_offset,
			&codepoint
		);

		if (num_bytes < 0) {
			BIO_WARN("Invalid codepoint encountered on line %d", pos->line + 1);
			break;
		}

		byte_offset += num_bytes;
		code_unit_offset += (codepoint <= 0xFFFF ? 1 : 2);
	}
	pos->character = code_unit_offset;
}

static void
buxn_ls_convert_range(
	buxn_ls_analyzer_t* analyzer,
	const char* filename,
	bio_lsp_range_t* range
) {
	buxn_ls_convert_position(analyzer, filename, &range->start);
	buxn_ls_convert_position(analyzer, filename, &range->end);
}

static buxn_ls_node_t*
buxn_ls_alloc_node(buxn_ls_analyzer_t* analyzer, const char* filename) {
	buxn_ls_node_t* node = barena_memalign(
		&analyzer->current_ctx->arena,
		sizeof(buxn_ls_node_t),
		_Alignof(buxn_ls_node_t)
	);
	*node = (buxn_ls_node_t){
		.filename = buxn_ls_arena_strcpy(&analyzer->current_ctx->arena, filename),
	};
	return node;
}

static void
buxn_ls_do_queue_file(buxn_ls_analyzer_t* analyzer, const char* filename) {
	buxn_ls_node_t* node = buxn_ls_alloc_node(analyzer, filename);
	bhash_put(&analyzer->current_ctx->nodes, node->filename, node);
	barray_push(analyzer->analyze_queue, node, NULL);
}

static void
buxn_ls_maybe_queue_node(
	buxn_ls_analyzer_t* analyzer,
	buxn_ls_workspace_t* workspace,
	buxn_ls_node_t* node
) {
	bhash_index_t node_index = bhash_find(&analyzer->current_ctx->nodes, node->filename);
	if (bhash_is_valid(node_index)) { return; }  // Already visited

	bhash_index_t doc_index = bhash_find(&workspace->docs, (char*){ (char*)node->filename });
	if (bhash_is_valid(doc_index)) {  // The document is opened
		buxn_ls_do_queue_file(analyzer, node->filename);
	}

	// Queue all children
	for (
		buxn_ls_edge_t* edge = node->out_edges;
		edge != NULL;
		edge = edge->next_out
	) {
		buxn_ls_maybe_queue_node(analyzer, workspace, edge->to);
	}
}

static void
buxn_ls_queue_from_root(
	buxn_ls_analyzer_t* analyzer,
	buxn_ls_workspace_t* workspace,
	buxn_ls_node_t* node
) {
	if (node->in_edges != NULL) {
		for (
			buxn_ls_edge_t* edge = node->in_edges;
			edge != NULL;
			edge = edge->next_in
		) {
			buxn_ls_queue_from_root(analyzer, workspace, edge->from);
		}
	} else {
		buxn_ls_maybe_queue_node(analyzer, workspace, node);
	}
}

static void
buxn_ls_init_analyzer_ctx(buxn_ls_analyzer_ctx_t* ctx, barena_pool_t* pool) {
	barena_init(&ctx->arena, pool);

	bhash_config_t hash_config = bhash_config_default();
	hash_config.eq = buxn_ls_str_eq;
	hash_config.hash = buxn_ls_str_hash;
	bhash_init(&ctx->nodes, hash_config);
}

static void
buxn_ls_reset_analyzer_ctx(buxn_ls_analyzer_ctx_t* ctx) {
	barena_reset(&ctx->arena);
	bhash_clear(&ctx->nodes);
}

static void
buxn_ls_cleanup_analyzer_ctx(buxn_ls_analyzer_ctx_t* ctx) {
	bhash_cleanup(&ctx->nodes);
	barena_reset(&ctx->arena);
}

void
buxn_ls_analyzer_init(buxn_ls_analyzer_t* analyzer, barena_pool_t* pool) {
	buxn_ls_init_analyzer_ctx(&analyzer->ctx_a, pool);
	buxn_ls_init_analyzer_ctx(&analyzer->ctx_b, pool);
	analyzer->current_ctx = &analyzer->ctx_a;
	analyzer->previous_ctx = &analyzer->ctx_b;

	bhash_config_t hash_config = bhash_config_default();
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
		bhash_index_t current_node_index = bhash_find(&analyzer->current_ctx->nodes, filename);
		if (bhash_is_valid(current_node_index)) {  // File is already added
			continue;
		}

		bhash_index_t previous_node_index = bhash_find(&analyzer->previous_ctx->nodes, filename);
		if (bhash_is_valid(previous_node_index)) {  // We saw this file before
			buxn_ls_queue_from_root(
				analyzer,
				workspace,
				analyzer->previous_ctx->nodes.values[previous_node_index]
			);
		} else {  // This was never seen before
			buxn_ls_do_queue_file(analyzer, filename);
		}
	}

	// Analyze files in order
	barray_clear(analyzer->lines);
	bhash_clear(&analyzer->files);
	barray_clear(analyzer->diagnostics);
	for (size_t i = 0; i < barray_len(analyzer->analyze_queue); ++i) {
		buxn_ls_node_t* node = analyzer->analyze_queue[i];
		if (!node->analyzed) {
			BIO_INFO("Analyzing %s", node->filename);

			barray_clear(analyzer->macro_defs);
			bhash_clear(&analyzer->label_defs);
			barray_clear(analyzer->references);

			buxn_asm_ctx_t ctx = {
				.entry_node = node,
				.analyzer = analyzer,
				.workspace = workspace,
			};
			buxn_asm(&ctx, node->filename);

			size_t num_refs = barray_len(analyzer->references);
			for (size_t sym_index = 0; sym_index < num_refs; ++sym_index) {
				const buxn_asm_sym_t* sym = &analyzer->references[sym_index];
				const char* filename = sym->region.filename;

				const buxn_asm_source_region_t* def_region = NULL;
				if (sym->type == BUXN_ASM_SYM_MACRO_REF) {
					// Macros cannot be forward declared so references can only
					// be resolved when a macro is already declared
					def_region = &analyzer->macro_defs[sym->id - 1].region;
				} else if (sym->type == BUXN_ASM_SYM_LABEL_REF) {
					bhash_index_t def_index = bhash_find(&analyzer->label_defs, sym->id);
					if (bhash_is_valid(def_index)) {
						def_region = &analyzer->label_defs.values[def_index].region;
					}
				}

				if (def_region == NULL) {
					continue;
				}

				bhash_index_t file_index = bhash_find(&analyzer->files, def_region->filename);
				assert(bhash_is_valid(file_index) && "Defining file is never opened");
				buxn_ls_file_t* def_file = &analyzer->files.values[file_index];

				buxn_ls_reference_t* reference = barena_memalign(
					&analyzer->current_ctx->arena,
					sizeof(buxn_ls_reference_t), _Alignof(buxn_ls_reference_t)
				);
				*reference = (buxn_ls_reference_t) {
					.range = buxn_source_region_to_lsp_location(&sym->region).range,
					.definition_location = {
						.uri = def_file->uri,
						.range = buxn_source_region_to_lsp_location(def_region).range,
					},
				};
				buxn_ls_convert_range(
					analyzer,
					filename,
					&reference->range
				);
				buxn_ls_convert_range(
					analyzer,
					def_region->filename,
					&reference->definition_location.range
				);

				bhash_alloc_result_t alloc_result = bhash_alloc(&analyzer->current_ctx->nodes, filename);
				buxn_ls_node_t* ref_node;
				if (alloc_result.is_new) {
					ref_node = barena_memalign(
						&analyzer->current_ctx->arena,
						sizeof(buxn_ls_node_t), _Alignof(buxn_ls_node_t)
					);
					analyzer->current_ctx->nodes.keys[alloc_result.index] = filename;
					analyzer->current_ctx->nodes.values[alloc_result.index] = node;
				} else {
					ref_node = analyzer->current_ctx->nodes.values[alloc_result.index];
				}

				reference->next = ref_node->references;
				ref_node->references = reference;
				/*BIO_TRACE(*/
					/*"Indexed reference to %s "*/
					/*" from %s:%d:%d:%d:%d to %s:%d:%d:%d:%d ",*/
					/*sym->name,*/

					/*filename,*/
					/*reference->range.start.line, reference->range.start.character,*/
					/*reference->range.end.line, reference->range.end.character,*/

					/*reference->definition_location.uri,*/
					/*reference->definition_location.range.start.line, reference->definition_location.range.start.character,*/
					/*reference->definition_location.range.end.line, reference->definition_location.range.end.character*/
				/*);*/
			}
		} else {
			BIO_INFO("Skipping %s", node->filename);
		}
	}

	// Coalesce reports into LSP diagnostic
	size_t num_diags = barray_len(analyzer->diagnostics);
	if (num_diags == 0) { return; }
	qsort(
		analyzer->diagnostics,
		num_diags, sizeof(analyzer->diagnostics[0]),
		buxn_ls_cmp_diagnostic
	);

	const char* current_filename = NULL;
	const char* doc_uri = NULL;
	for (size_t diag_index = 0; diag_index < num_diags; ++diag_index) {
		buxn_ls_diagnostic_t* diag = &analyzer->diagnostics[diag_index];

		if (diag->location.uri == current_filename) {
			diag->location.uri = doc_uri;
			diag->related_location.uri = doc_uri;
		} else {
			const char* path = diag->location.uri;

			bhash_index_t file_index = bhash_find(&analyzer->files, path);
			assert(bhash_is_valid(file_index) && "Diagnostic comes from unvisited file");

			buxn_ls_file_t* file = &analyzer->files.values[file_index];
			doc_uri = diag->location.uri = file->uri;
			current_filename = path;
		}

		buxn_ls_convert_position(analyzer, current_filename, &diag->location.range.start);
		buxn_ls_convert_position(analyzer, current_filename, &diag->location.range.end);
		if (diag->related_message != NULL) {
			buxn_ls_convert_position(analyzer, current_filename, &diag->related_location.range.start);
			buxn_ls_convert_position(analyzer, current_filename, &diag->related_location.range.end);
		}
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
			file->first_error_byte = report->region->range.start.byte < file->first_error_byte
				? report->region->range.start.byte
				: file->first_error_byte;
		}
	}

	// Collect all reports first
	// They will be sorted and converted to LSP format later
	buxn_ls_diagnostic_t diag = {
		.location = buxn_source_region_to_lsp_location(report->region),
		.message = report->message,
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
		diag.related_location = buxn_source_region_to_lsp_location(report->related_region);
		diag.related_message = report->related_message;
	}

	barray_push(analyzer->diagnostics, diag, NULL);
}

void
buxn_asm_put_rom(buxn_asm_ctx_t* ctx, uint16_t addr, uint8_t value) {
	(void)ctx;
	(void)addr;
	(void)value;
}

void
buxn_asm_put_symbol(buxn_asm_ctx_t* ctx, uint16_t addr, const buxn_asm_sym_t* sym) {
	(void)addr;
	buxn_ls_analyzer_t* analyzer = ctx->analyzer;
	switch (sym->type) {
		case BUXN_ASM_SYM_MACRO:
			barray_push(analyzer->macro_defs, *sym, NULL);
			break;
		case BUXN_ASM_SYM_LABEL: {
			buxn_asm_sym_t sym_copy = *sym;
			uint16_t id = sym->id;
			bhash_put(&analyzer->label_defs, id, sym_copy);
		} break;
		case BUXN_ASM_SYM_MACRO_REF:
		case BUXN_ASM_SYM_LABEL_REF:
			barray_push(analyzer->references, *sym, NULL);
			break;
		default:
			break;
	}
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
			content = ctx->workspace->docs.values[doc_index];
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

		size_t uri_len = (sizeof("file://") - 1) + ctx->workspace->root_dir_len + strlen(filename) + 1;
		char* uri = barena_memalign(&analyzer->current_ctx->arena, uri_len, _Alignof(char));
		// TODO: do we need to url encode?
		snprintf(uri, uri_len, "file://%s%s", ctx->workspace->root_dir, filename);
		buxn_ls_file_t file = {
			.uri = uri,
			.content = content,
			.first_line_index = -1,
			.first_error_byte = INT_MAX,  // No error
		};
		bhash_put(&analyzer->files, filename, file);
	}

	buxn_asm_file_t* file = buxn_ls_malloc(sizeof(buxn_asm_file_t));
	*file = (buxn_asm_file_t){ .content = content };

	bhash_alloc_result_t alloc_result = bhash_alloc(&analyzer->current_ctx->nodes, filename);
	buxn_ls_node_t* node;
	if (alloc_result.is_new) {
		node = buxn_ls_alloc_node(analyzer, filename);
		analyzer->current_ctx->nodes.keys[alloc_result.index] = node->filename;
		analyzer->current_ctx->nodes.values[alloc_result.index] = node;
	} else {
		node = analyzer->current_ctx->nodes.values[alloc_result.index];
	}
	node->analyzed = true;

	if (node != ctx->entry_node) {
		buxn_ls_edge_t* edge = barena_memalign(
			&analyzer->current_ctx->arena,
			sizeof(buxn_ls_edge_t), _Alignof(buxn_ls_edge_t)
		);

		edge->from = ctx->entry_node;
		edge->to = node;

		edge->next_out = ctx->entry_node->out_edges;
		ctx->entry_node->out_edges = edge;
		edge->next_in = node->in_edges;
		node->in_edges = edge;
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
