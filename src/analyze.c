#include "analyze.h"
#include "workspace.h"
#include "common.h"
#include <buxn/asm/asm.h>
#include <bio/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <utf8proc.h>

struct buxn_asm_ctx_s {
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

static void
buxn_ls_convert_position(buxn_ls_analyzer_t* analyzer, bio_lsp_position_t* pos) {
	size_t num_lines = barray_len(analyzer->lines);
	pos->line -= 1;
	if (pos->line >= (int)num_lines) {
		pos->line = num_lines - 1;
		pos->character = 0;
		return;
	}

	const buxn_ls_str_t* line = &analyzer->lines[pos->line];
	int utf8_offset = pos->character - 1;
	utf8proc_ssize_t byte_offset = 0;
	utf8proc_ssize_t line_len = line->len;
	int code_unit_offset = 0;
	while (true) {
		if (byte_offset >= utf8_offset || byte_offset >= line_len) { break; }

		utf8proc_int32_t codepoint;
		utf8proc_ssize_t num_bytes = utf8proc_iterate(
			(const utf8proc_uint8_t*)line->chars + byte_offset,
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

void*
buxn_asm_alloc(buxn_asm_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(&ctx->analyzer->arena, size, alignment);
}

void
buxn_asm_report(buxn_asm_ctx_t* ctx, buxn_asm_report_type_t type, const buxn_asm_report_t* report) {
	buxn_ls_analyzer_t* analyzer = ctx->analyzer;

	// Only save reports about source regions, not top level reports
	if (report->region->range.start.line == 0) { return; }

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
	(void)ctx;
	(void)addr;
	(void)sym;
}

buxn_asm_file_t*
buxn_asm_fopen(buxn_asm_ctx_t* ctx, const char* filename) {
	buxn_ls_analyzer_t* analyzer = ctx->analyzer;
	bhash_index_t file_index = bhash_find(&analyzer->files, filename);
	buxn_ls_str_t content;
	if (bhash_is_valid(file_index)) {  // File is already read
		content = analyzer->files.values[file_index];
	} else {  // New file
		bhash_index_t doc_index = bhash_find(&ctx->workspace->docs, (char*){ (char*)filename });
		if (bhash_is_valid(doc_index)) {  // File is managed
			content = (buxn_ls_str_t){
				.chars = ctx->workspace->docs.values[doc_index].content,
				.len = ctx->workspace->docs.values[doc_index].size,
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

			char* read_buf = barena_memalign(&ctx->analyzer->arena, stat.size, _Alignof(char));
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
		bhash_put(&analyzer->files, filename, content);
	}

	buxn_asm_file_t* file = buxn_ls_malloc(sizeof(buxn_asm_file_t));
	*file = (buxn_asm_file_t){ .content = content };
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

void
buxn_ls_analyzer_init(buxn_ls_analyzer_t* analyzer, barena_pool_t* pool) {
	barena_init(&analyzer->arena, pool);
	bhash_init(&analyzer->files, bhash_config_default());
}

void
buxn_ls_analyzer_cleanup(buxn_ls_analyzer_t* analyzer) {
	barray_free(NULL, analyzer->diagnostics);
	barray_free(NULL, analyzer->lines);
	bhash_cleanup(&analyzer->files);
	barena_reset(&analyzer->arena);
}

void
buxn_ls_analyze(buxn_ls_analyzer_t* analyzer, buxn_ls_workspace_t* workspace) {
	if (!bhash_is_valid(workspace->last_updated_doc)) { return; }

	barena_reset(&analyzer->arena);
	barray_clear(analyzer->diagnostics);
	bhash_clear(&analyzer->files);
	buxn_asm_ctx_t ctx = {
		.analyzer = analyzer,
		.workspace = workspace,
	};
	buxn_asm(&ctx, workspace->docs.keys[workspace->last_updated_doc]);

	// Coalesce reports into LSP diagnostic
	size_t num_diags = barray_len(analyzer->diagnostics);
	if (num_diags == 0) { return; }
	qsort(
		analyzer->diagnostics,
		num_diags, sizeof(analyzer->diagnostics[0]),
		buxn_ls_cmp_diagnostic
	);

	const char* previous_filename = NULL;
	char* doc_uri = NULL;
	for (size_t diag_index = 0; diag_index < num_diags; ++diag_index) {
		buxn_ls_diagnostic_t* diag = &analyzer->diagnostics[diag_index];

		if (diag->location.uri == previous_filename) {
			diag->location.uri = doc_uri;
			diag->related_location.uri = doc_uri;
		} else {
			const char* path = diag->location.uri;
			size_t uri_len = strlen(path) + workspace->root_dir_len + sizeof("file://");
			doc_uri = barena_memalign(&analyzer->arena, uri_len, _Alignof(char));
			snprintf(doc_uri, uri_len, "file://%s%s", workspace->root_dir, path);
			diag->location.uri = doc_uri;
			previous_filename = path;

			// Split file content into lines to convert into LSP questionable
			// position format
			barray_clear(analyzer->lines);
			bhash_index_t file_index = bhash_find(&analyzer->files, path);
			if (!bhash_is_valid(file_index)) { continue; }

			buxn_ls_str_t content = analyzer->files.values[file_index];
			buxn_ls_str_t line = { .chars = content.chars, };
			size_t start_index = 0;
			for (size_t char_index = 0; char_index < content.len; ++char_index) {
				char ch = content.chars[char_index];
				if (ch == '\n') {
					line.len = char_index - start_index;
					barray_push(analyzer->lines, line, NULL);
					line.chars = content.chars + char_index + 1;
					start_index = char_index + 1;
				} else if (ch == '\r') {
					if (char_index < content.len - 1 && content.chars[char_index + 1] == '\n') {
						line.len = char_index - start_index;
						barray_push(analyzer->lines, line, NULL);
						line.chars = content.chars + char_index + 2;
						start_index = char_index + 2;
					} else {
						line.len = char_index - start_index;
						barray_push(analyzer->lines, line, NULL);
						line.chars = content.chars + char_index + 1;
						start_index = char_index + 1;
					}
				}
			}
			// Last line
			if (start_index < content.len) {
				line.len = content.len - start_index;
				barray_push(analyzer->lines, line, NULL);
			}
		}

		buxn_ls_convert_position(analyzer, &diag->location.range.start);
		buxn_ls_convert_position(analyzer, &diag->location.range.end);
		if (diag->related_message != NULL) {
			buxn_ls_convert_position(analyzer, &diag->related_location.range.start);
			buxn_ls_convert_position(analyzer, &diag->related_location.range.end);
		}
	}
}
