#include "workspace.h"
#include "common.h"
#include "lsp.h"
#include <yyjson.h>
#include <yuarel.h>

char*
buxn_ls_workspace_resolve_path(buxn_ls_workspace_t* workspace, char* uri) {
	struct yuarel url;
	if (yuarel_parse(&url, uri) != 0) {
		BIO_WARN("Invalid document uri");
		return NULL;
	}

	if (strncmp(workspace->root_dir, url.path, workspace->root_dir_len) == 0) {
		return url.path + workspace->root_dir_len;
	} else {
		BIO_WARN("Document is outside of root path: %s", url.path);
		return NULL;
	}
}

void
buxn_ls_workspace_init(buxn_ls_workspace_t* workspace, const char* root_dir) {
	size_t root_dir_len = strlen(root_dir);
	if (root_dir_len > 0 && root_dir[root_dir_len - 1] != '/') {
		workspace->root_dir = buxn_ls_malloc(root_dir_len + 2);
		memcpy(workspace->root_dir, root_dir, root_dir_len);
		workspace->root_dir[root_dir_len] = '/';
		workspace->root_dir[root_dir_len + 1] = '\0';
		workspace->root_dir_len = root_dir_len + 1;
	} else {
		workspace->root_dir = buxn_ls_strcpy(root_dir);
		workspace->root_dir_len = root_dir_len;
	}

	bhash_config_t config = bhash_config_default();
	config.hash = buxn_ls_str_hash;
	config.eq = buxn_ls_str_eq;
	bhash_init(&workspace->docs, config);
}

void
buxn_ls_workspace_cleanup(buxn_ls_workspace_t* workspace) {
	for (bhash_index_t i = 0; i < bhash_len(&workspace->docs); ++i) {
		buxn_ls_free(workspace->docs.keys[i]);
		buxn_ls_free((char*)workspace->docs.values[i].chars);
	}
	bhash_cleanup(&workspace->docs);
	buxn_ls_free(workspace->root_dir);
}

void
buxn_ls_workspace_update(buxn_ls_workspace_t* workspace, const struct bio_lsp_in_msg_s* msg) {
	if (msg->type == BIO_LSP_MSG_NOTIFICATION) {
		yyjson_val* text_document = BIO_LSP_JSON_GET_LIT(msg->value, "textDocument");
		const char* uri = yyjson_get_str(BIO_LSP_JSON_GET_LIT(text_document, "uri"));
		char* path = buxn_ls_workspace_resolve_path(workspace, (char*)uri);
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

			bhash_alloc_result_t alloc_result = bhash_alloc(&workspace->docs, path);
			buxn_ls_str_t* doc;
			if (alloc_result.is_new) {
				workspace->docs.keys[alloc_result.index] = buxn_ls_strcpy(path);
				doc = &workspace->docs.values[alloc_result.index];
			} else {
				BIO_WARN("Document is already opened");
				doc = &workspace->docs.values[alloc_result.index];
				buxn_ls_free((char*)doc->chars);
			}

			if (content_size > 0) {
				doc->chars = buxn_ls_malloc(content_size);
				memcpy((char*)doc->chars, content, content_size);
			} else {
				doc->chars = NULL;
			}
			doc->len = content_size;
		} else if (strcmp(msg->method, "textDocument/didChange") == 0) {
			// TODO: support incremental sync
			const char* content = NULL;
			size_t content_size = 0;
			yyjson_val* changes = BIO_LSP_JSON_GET_LIT(msg->value, "contentChanges");
			yyjson_val* last_change = yyjson_arr_get_last(changes);
			yyjson_val* json_text = BIO_LSP_JSON_GET_LIT(last_change, "text");
			content = yyjson_get_str(json_text);
			if (content != NULL) {
				content_size = yyjson_get_len(json_text);
			}

			BIO_INFO("Updating %s", path);

			bhash_index_t index = bhash_find(&workspace->docs, path);
			buxn_ls_str_t* doc;
			if (bhash_is_valid(index)) {
				doc = &workspace->docs.values[index];
				buxn_ls_free((char*)doc->chars);
			} else {
				BIO_WARN("Document was not opened");
				index = bhash_alloc(&workspace->docs, path).index;
				workspace->docs.keys[index] = buxn_ls_strcpy(path);
				doc = &workspace->docs.values[index];
			}

			if (content_size > 0) {
				doc->chars = buxn_ls_malloc(content_size);
				memcpy((char*)doc->chars, content, content_size);
			} else {
				doc->chars= NULL;
			}
			doc->len = content_size;
		} else if (strcmp(msg->method, "textDocument/didClose") == 0) {
			BIO_INFO("Closing %s", path);

			bhash_index_t index = bhash_remove(&workspace->docs, path);
			if (bhash_is_valid(index)) {
				buxn_ls_free(workspace->docs.keys[index]);
				buxn_ls_free((char*)workspace->docs.values[index].chars);
			} else {
				BIO_WARN("Document was not opened");
			}
		} else {
			BIO_WARN("Dropped notification: %s", msg->method);
		}
	}
}
