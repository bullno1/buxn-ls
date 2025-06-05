#ifndef BUXN_LS_TEXT_DOCUMENT
#define BUXN_LS_TEXT_DOCUMENT

#include <bhash.h>

struct bio_lsp_in_msg_s;

typedef struct {
	char* content;
	size_t size;
} buxn_ls_doc_t;

typedef BHASH_TABLE(char*, buxn_ls_doc_t) buxn_ls_doc_map_t;

typedef struct buxn_ls_docs_s {
	char* root_dir;
	size_t root_dir_len;
	buxn_ls_doc_map_t docs;
} buxn_ls_docs_t;

void
buxn_ls_text_document_init(buxn_ls_docs_t* docs, const char* root_dir);

void
buxn_ls_text_document_cleanup(buxn_ls_docs_t* docs);

void
buxn_ls_handle_text_document_msg(buxn_ls_docs_t* docs, const struct bio_lsp_in_msg_s* msg);

bool
buxn_ls_is_document_managed(buxn_ls_docs_t* docs, const char* path);

#endif
