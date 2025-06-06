#include "analyze.h"
#include "workspace.h"
#include <buxn/asm/asm.h>
#include <bio/file.h>
#include <stdio.h>

struct buxn_asm_ctx_s {
	buxn_ls_analyzer_t* analyzer;
	buxn_ls_workspace_t* workspace;
};

void*
buxn_asm_alloc(buxn_asm_ctx_t* ctx, size_t size, size_t alignment) {
	return barena_memalign(&ctx->analyzer->arena, size, alignment);
}

void
buxn_asm_report(buxn_asm_ctx_t* ctx, buxn_asm_report_type_t type, const buxn_asm_report_t* report) {
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
	if (bhash_is_valid(file_index) && !analyzer->files.values[file_index].in_use) {
		buxn_asm_file_t* file = &analyzer->files.values[file_index];
		file->in_use = true;
		file->offset = 0;
		return file;
	}

	bhash_index_t doc_index = bhash_find(&ctx->workspace->docs, (char*){ (char*)filename });
	buxn_asm_file_t file;
	if (bhash_is_valid(doc_index)) {
		file = (buxn_asm_file_t){
			.content = ctx->workspace->docs.values[doc_index].content,
			.size = ctx->workspace->docs.values[doc_index].size,
			.in_use = true,
		};
	} else {
		bio_file_t fd;
		bio_error_t error = { 0 };
		if (!bio_fopen(&fd, filename, "r", &error)) {
			BIO_ERROR("Could not open %s: " BIO_ERROR_FMT, filename, BIO_ERROR_FMT_ARGS(&error));
			return NULL;
		}

		bio_stat_t stat;
		if (!bio_fstat(fd, &stat,&error)) {
			BIO_ERROR("Could not stat %s: " BIO_ERROR_FMT, filename, BIO_ERROR_FMT_ARGS(&error));
			return NULL;
		}

		char* content = barena_memalign(&ctx->analyzer->arena, stat.size, _Alignof(char));
		if (bio_fread_exactly(fd, content, stat.size, &error) != stat.size) {
			BIO_ERROR("Error while reading %s: " BIO_ERROR_FMT, filename, BIO_ERROR_FMT_ARGS(&error));
			return NULL;
		}

		file = (buxn_asm_file_t){
			.content = content,
			.size = stat.size,
			.in_use = true,
		};
	}

	bhash_alloc_result_t alloc_result = bhash_alloc(&analyzer->files, filename);
	if (alloc_result.is_new) {
		analyzer->files.keys[alloc_result.index] = filename;
		analyzer->files.values[alloc_result.index] = file;
		return &analyzer->files.values[alloc_result.index];
	} else {
		buxn_asm_file_t* new_instance = barena_memalign(
			&analyzer->arena,
			sizeof(buxn_asm_file_t),
			_Alignof(buxn_asm_file_t)
		);
		*new_instance = file;
		return new_instance;
	}
}

void
buxn_asm_fclose(buxn_asm_ctx_t* ctx, buxn_asm_file_t* file) {
	(void)ctx;
	file->in_use = false;
}

int
buxn_asm_fgetc(buxn_asm_ctx_t* ctx, buxn_asm_file_t* file) {
	if (file->offset < file->size) {
		return (int)file->content[file->offset++];
	} else {
		return EOF;
	}
}

void
buxn_ls_analyzer_init(buxn_ls_analyzer_t* analyzer, barena_pool_t* pool) {
	BIO_TRACE("%p", (void*)pool);
	barena_init(&analyzer->arena, pool);
	bhash_init(&analyzer->files, bhash_config_default());
}

void
buxn_ls_analyzer_cleanup(buxn_ls_analyzer_t* analyzer) {
	barray_free(NULL, analyzer->diagnostics);
	bhash_cleanup(&analyzer->files);
	barena_reset(&analyzer->arena);
}

void
buxn_ls_analyze(buxn_ls_analyzer_t* analyzer, buxn_ls_workspace_t* workspace) {
	if (bhash_is_valid(workspace->last_updated_doc)) {
		barena_reset(&analyzer->arena);
		barray_clear(analyzer->diagnostics);
		bhash_clear(&analyzer->files);
		buxn_asm_ctx_t ctx = {
			.analyzer = analyzer,
			.workspace = workspace,
		};
		buxn_asm(&ctx, workspace->docs.keys[workspace->last_updated_doc]);
	}
}
