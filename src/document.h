#ifndef BIO_LSP_DOC_H
#define BIO_LSP_DOC_H

#include <stddef.h>

typedef struct bio_lsp_doc_s bio_lsp_doc_t;

typedef struct {
	int line;
	int character;
} bio_lsp_doc_pos_t;

typedef struct {
	bio_lsp_doc_pos_t start;
	bio_lsp_doc_pos_t end;
} bio_lsp_doc_range_t;

typedef struct {
	bio_lsp_doc_range_t range;
	const char* content;
	size_t content_len;
} bio_lsp_doc_edit_t;

bio_lsp_doc_t*
bio_lsp_doc_new(const char* content, size_t len);

void
bio_lsp_doc_destroy(bio_lsp_doc_t* doc);

void
bio_lsp_doc_edit(bio_lsp_doc_t* doc, const bio_lsp_doc_edit_t* edit);

const char*
bio_lsp_doc_content(bio_lsp_doc_t* doc, size_t* len);

#endif
