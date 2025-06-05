#include "text_document.h"
#include "common.h"
#include "lsp.h"
#include <yyjson.h>
#include <yuarel.h>

static bhash_hash_t
buxn_ls_str_hash(const void* key, size_t size) {
	(void)size;
	return bhash_hash(*(const char**)key, strlen(*(const char**)key));
}

static bool
buxn_ls_str_eq(const void* lhs, const void* rhs, size_t size) {
	(void)size;
	return strcmp(*(const char**)lhs, *(const char**)rhs) == 0;
}

static char*
buxn_ls_doc_resolve_path(buxn_ls_docs_t* docs, char* uri) {
	struct yuarel url;
	if (yuarel_parse(&url, uri) != 0) {
		BIO_WARN("Invalid document uri");
		return NULL;
	}

	if (strncmp(docs->root_dir, url.path, docs->root_dir_len) == 0) {
		return url.path + docs->root_dir_len;
	} else {
		BIO_WARN("Document is outside of root path: %s", url.path);
		return NULL;
	}
}

void
buxn_ls_text_document_init(buxn_ls_docs_t* docs, const char* root_dir) {
	size_t root_dir_len = strlen(root_dir);
	if (root_dir_len > 0 && root_dir[root_dir_len - 1] != '/') {
		docs->root_dir = buxn_ls_malloc(root_dir_len + 2);
		memcpy(docs->root_dir, root_dir, root_dir_len);
		docs->root_dir[root_dir_len] = '/';
		docs->root_dir[root_dir_len + 1] = '\0';
		docs->root_dir_len = root_dir_len + 1;
	} else {
		docs->root_dir = buxn_ls_strcpy(root_dir);
		docs->root_dir_len = root_dir_len;
	}

	bhash_config_t config = bhash_config_default();
	config.hash = buxn_ls_str_hash;
	config.eq = buxn_ls_str_eq;
	bhash_init(&docs->docs, config);
}

void
buxn_ls_text_document_cleanup(buxn_ls_docs_t* docs) {
	for (bhash_index_t i = 0; i < bhash_len(&docs->docs); ++i) {
		buxn_ls_free(docs->docs.keys[i]);
		buxn_ls_free(docs->docs.values[i].content);
	}
	bhash_cleanup(&docs->docs);
	buxn_ls_free(docs->root_dir);
}

void
buxn_ls_handle_text_document_msg(buxn_ls_docs_t* docs, const struct bio_lsp_in_msg_s* msg) {
	if (msg->type == BIO_LSP_MSG_NOTIFICATION) {
		yyjson_val* text_document = BIO_LSP_JSON_GET_LIT(msg->value, "textDocument");
		const char* uri = yyjson_get_str(BIO_LSP_JSON_GET_LIT(text_document, "uri"));
		char* path = buxn_ls_doc_resolve_path(docs, (char*)uri);
		if (path == NULL) { return; }

		if (strcmp(msg->method, "textDocument/didOpen") == 0) {
			const char* content = NULL;
			size_t content_size = 0;
			yyjson_val* json_text = BIO_LSP_JSON_GET_LIT(text_document, "text");
			content = yyjson_get_str(json_text);
			if (content != NULL) {
				content_size = yyjson_get_len(json_text);
			}

			BIO_INFO("Registering %s", path);

			bhash_alloc_result_t alloc_result = bhash_alloc(&docs->docs, path);
			buxn_ls_doc_t* doc;
			if (alloc_result.is_new) {
				docs->docs.keys[alloc_result.index] = buxn_ls_strcpy(path);
				doc = &docs->docs.values[alloc_result.index];
			} else {
				BIO_WARN("Document is already opened");
				doc = &docs->docs.values[alloc_result.index];
				buxn_ls_free(doc->content);
			}

			if (content_size > 0) {
				doc->content = buxn_ls_malloc(content_size);
				memcpy(doc->content, content, content_size);
			} else {
				doc->content= NULL;
			}
			doc->size = content_size;
		} else if (strcmp(msg->method, "textDocument/didChange") == 0) {
			// TODO: support incremental sync
			const char* content = NULL;
			size_t content_size = 0;
			yyjson_val* changes = BIO_LSP_JSON_GET_LIT(text_document, "contentChanges");
			yyjson_val* last_change = yyjson_arr_get_last(changes);
			yyjson_val* json_text = BIO_LSP_JSON_GET_LIT(last_change, "text");
			content = yyjson_get_str(json_text);
			if (content != NULL) {
				content_size = yyjson_get_len(json_text);
			}

			BIO_INFO("Updating %s", path);

			bhash_index_t index = bhash_find(&docs->docs, path);
			buxn_ls_doc_t* doc;
			if (bhash_is_valid(index)) {
				doc = &docs->docs.values[index];
				buxn_ls_free(doc->content);
			} else {
				BIO_WARN("Document was not opened");
				bhash_alloc_result_t alloc_result = bhash_alloc(&docs->docs, path);
				docs->docs.keys[alloc_result.index] = buxn_ls_strcpy(path);
				doc = &docs->docs.values[alloc_result.index];
			}

			if (content_size > 0) {
				doc->content = buxn_ls_malloc(content_size);
				memcpy(doc->content, content, content_size);
			} else {
				doc->content= NULL;
			}
			doc->size = content_size;
		} else if (strcmp(msg->method, "textDocument/didClose") == 0) {
			BIO_INFO("Closing %s", path);
			bhash_index_t index = bhash_remove(&docs->docs, path);
			if (bhash_is_valid(index)) {
				buxn_ls_free(docs->docs.keys[index]);
				buxn_ls_free(docs->docs.values[index].content);
			} else {
				BIO_WARN("Document was not opened");
			}
		} else {
			BIO_WARN("Dropped notification: %s", msg->method);
		}
	}
}

bool
buxn_ls_is_document_managed(buxn_ls_docs_t* docs, const char* path);
