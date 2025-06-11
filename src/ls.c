#include "ls.h"
#include "lsp.h"
#include "common.h"
#include "resources.h"
#include "workspace.h"
#include "analyze.h"
#include "bmacro.h"
#include <string.h>
#include <yyjson.h>
#include <yuarel.h>
#include <bio/timer.h>

static const bio_time_t BUXN_LS_ANALYZE_DELAY_MS = 200;

typedef BHASH_SET(char*) buxn_ls_str_set_t;

typedef struct {
	bio_lsp_conn_t* conn;
	bool should_terminate;
	yyjson_alc json_allocator;
	char name_buf[sizeof("ls:2147483647")];
	buxn_ls_workspace_t workspace;
	barena_t request_arena;

	bio_timer_t analyze_delay_timer;
	buxn_ls_analyzer_t analyzer;
	buxn_ls_str_set_t diag_file_set_a;
	buxn_ls_str_set_t diag_file_set_b;
	buxn_ls_str_set_t* currently_diagnosed_files;
	buxn_ls_str_set_t* previously_diagnosed_files;
} buxn_ls_ctx_t;

static void*
buxn_ls_json_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size) {
	(void)ctx;
	(void)old_size;
	return buxn_ls_realloc(ptr, new_size);
}

static inline void*
buxn_ls_json_malloc(void* ctx, size_t size) {
	return buxn_ls_realloc(NULL, size);
}

static inline void
buxn_ls_json_free(void* ctx, void* ptr) {
	buxn_ls_realloc(ptr, 0);
}

static bio_lsp_out_msg_t
buxn_ls_begin_msg(buxn_ls_ctx_t* ctx, bio_lsp_msg_type_t type, const bio_lsp_in_msg_t* in_msg) {
	bio_lsp_out_msg_t out_msg = {
		.type = type,
		.doc = yyjson_mut_doc_new(&ctx->json_allocator),
	};
	switch (type) {
		case BIO_LSP_MSG_RESULT:
		case BIO_LSP_MSG_ERROR:
			out_msg.original_id = in_msg->id;
			break;
		case BIO_LSP_MSG_NOTIFICATION:
			break;
		case BIO_LSP_MSG_REQUEST:  // This isn't really used
			break;
	}

	return out_msg;
}

static bool
buxn_ls_end_msg(buxn_ls_ctx_t* ctx, const bio_lsp_out_msg_t* msg) {
	bio_error_t error = { 0 };
	bool success = bio_lsp_send_msg(ctx->conn, &ctx->json_allocator, msg, &error);
	yyjson_mut_doc_free(msg->doc);

	if (!success) {
		BIO_ERROR("Error while sending reply: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
	}

	return success;
}

static bool
buxn_ls_initialize(
	buxn_ls_ctx_t* ctx,
	barena_pool_t* pool,
	const bio_lsp_in_msg_t* msg
) {
	int pid = yyjson_get_int(BIO_LSP_JSON_GET_LIT(msg->value, "processId"));
	snprintf(ctx->name_buf, sizeof(ctx->name_buf), "ls:%d", pid);
	bio_set_coro_name(ctx->name_buf);
	BIO_INFO("Initializing");

	barena_init(&ctx->request_arena, pool);
	buxn_ls_analyzer_init(&ctx->analyzer, pool);

	bhash_config_t hash_config = bhash_config_default();
	hash_config.eq = buxn_ls_str_eq;
	hash_config.hash = buxn_ls_str_hash;
	bhash_init_set(&ctx->diag_file_set_a, hash_config);
	bhash_init_set(&ctx->diag_file_set_b, hash_config);
	ctx->currently_diagnosed_files = &ctx->diag_file_set_a;
	ctx->previously_diagnosed_files = &ctx->diag_file_set_b;

	// Find root dir
	// From workspaceFolders
	yyjson_val* workspace_folders = BIO_LSP_JSON_GET_LIT(msg->value, "workspaceFolders");
	size_t num_folders = yyjson_arr_size(workspace_folders);
	const char* root_dir = NULL;
	if (num_folders >= 1) {
		if (num_folders > 1) { BIO_WARN("Picking the first workspace folder as root"); }

		const char* workspace = yyjson_get_str(
			BIO_LSP_JSON_GET_LIT(yyjson_arr_get_first(workspace_folders), "uri")
		);
		struct yuarel url;
		if (
			workspace != NULL
			&& yuarel_parse(&url, (char*)workspace) == 0
			&& strcmp(url.scheme, "file") == 0
		) {
			BIO_INFO("Root dir: %s", url.path);
			root_dir = url.path;
		}
	}

	// From rootUri
	if (root_dir == NULL) {
		const char* root_uri = yyjson_get_str(BIO_LSP_JSON_GET_LIT(msg->value, "rootUri"));
		struct yuarel url;
		if (
			root_uri != NULL
			&& yuarel_parse(&url, (char*)root_uri) == 0
			&& strcmp(url.scheme, "file") == 0
		) {
			BIO_INFO("Root dir: %s", url.path);
			root_dir = url.path;
		}
	}

	// From rootPath
	if (root_dir == NULL) {
		const char* root_path = yyjson_get_str(BIO_LSP_JSON_GET_LIT(msg->value, "rootPath"));
		if (root_path != NULL) {
			BIO_INFO("Root dir: %s", root_path);
			root_dir = root_path;
		}
	}

	if (root_dir == NULL) {
		bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_ERROR, msg);
		reply.value = yyjson_mut_obj(reply.doc);
		yyjson_mut_obj_add_int(reply.doc, reply.value, "code", -32602);
		yyjson_mut_obj_add_str(reply.doc, reply.value, "message", "Root path was not provided");
		buxn_ls_end_msg(ctx, &reply);
		return false;
	}

	buxn_ls_workspace_init(&ctx->workspace, root_dir);

	xincbin_data_t initialize_json = XINCBIN_GET(initialize_json);
	// Can't do in-situ as multiple instances in server mode share the same
	// initialize.json
	size_t initialize_mem_size = yyjson_read_max_memory_usage(initialize_json.size, YYJSON_READ_NOFLAG);
	void* initialize_mem = buxn_ls_malloc(initialize_mem_size);
	yyjson_alc initialize_pool;
	yyjson_alc_pool_init(&initialize_pool, initialize_mem, initialize_mem_size);
	yyjson_doc* initialize_doc = yyjson_read_opts(
		(char*)initialize_json.data, initialize_json.size,
		YYJSON_READ_NOFLAG,
		&initialize_pool,
		NULL
	);

	bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_RESULT, msg);
	reply.value = yyjson_val_mut_copy(reply.doc, yyjson_doc_get_root(initialize_doc));
	buxn_ls_end_msg(ctx, &reply);

	buxn_ls_free(initialize_mem);
	return true;
}

static void
buxn_ls_cleanup(buxn_ls_ctx_t* ctx) {
	bio_cancel_timer(ctx->analyze_delay_timer);

	for (bhash_index_t i = 0; i < bhash_len(ctx->previously_diagnosed_files); ++i) {
		char* uri = ctx->previously_diagnosed_files->keys[i];
		buxn_ls_free(uri);
	}

	bhash_cleanup(&ctx->diag_file_set_a);
	bhash_cleanup(&ctx->diag_file_set_b);
	buxn_ls_workspace_cleanup(&ctx->workspace);
	buxn_ls_analyzer_cleanup(&ctx->analyzer);
	barena_reset(&ctx->request_arena);
}

static void
buxn_ls_serialize_lsp_position(
	yyjson_mut_doc* doc,
	yyjson_mut_val* json,
	const bio_lsp_position_t* position
) {
	yyjson_mut_obj_add_int(doc, json, "line", position->line);
	yyjson_mut_obj_add_int(doc, json, "character", position->character);
}

static void
buxn_ls_serialize_lsp_range(
	yyjson_mut_doc* doc,
	yyjson_mut_val* json,
	const bio_lsp_range_t* range
) {
	buxn_ls_serialize_lsp_position(doc, yyjson_mut_obj_add_obj(doc, json, "start"), &range->start);
	buxn_ls_serialize_lsp_position(doc, yyjson_mut_obj_add_obj(doc, json, "end"), &range->end);
}

static void
buxn_ls_analyze_workspace(void* userdata) {
	buxn_ls_ctx_t* ctx = userdata;
	buxn_ls_analyzer_t* analyzer = &ctx->analyzer;

	BIO_INFO("Analyzing");
	buxn_ls_analyze(analyzer, &ctx->workspace);
	BIO_INFO("Done");

	const char* last_uri = NULL;
	size_t num_diags = barray_len(analyzer->diagnostics);
	bio_lsp_out_msg_t msg = { 0 };
	yyjson_mut_val* diag_arr = NULL;
	for (size_t i = 0; i < num_diags; ++i) {
		const buxn_ls_diagnostic_t* diag = &analyzer->diagnostics[i];

		if (diag->location.uri != last_uri) {  // Next file encountered
			// Move uri to the set of diagnosed files
			bhash_index_t remove_index = bhash_remove(
				ctx->previously_diagnosed_files,
				(char*){ (char*)diag->location.uri }
			);
			if (bhash_is_valid(remove_index)) {
				bhash_put_key(
					ctx->currently_diagnosed_files,
					ctx->previously_diagnosed_files->keys[remove_index]
				);
			} else {
				char* uri_copy = buxn_ls_strcpy(diag->location.uri);
				bhash_put_key(ctx->currently_diagnosed_files, uri_copy);
			}

			// Send currently building diagnostic
			if (msg.method != NULL) {
				BIO_DEBUG("Sending diagnostic for: %s", last_uri);
				bool success = buxn_ls_end_msg(ctx, &msg);
				msg.method = NULL;
				diag_arr = NULL;
				if (!success) { break; }
			}

			// Build a new one
			msg = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_NOTIFICATION, NULL);
			msg.method = "textDocument/publishDiagnostics";
			msg.value = yyjson_mut_obj(msg.doc);
			yyjson_mut_obj_add_str(msg.doc, msg.value, "uri", diag->location.uri);
			diag_arr = yyjson_mut_obj_add_arr(msg.doc, msg.value, "diagnostics");

			last_uri = diag->location.uri;
		}

		yyjson_mut_val* diag_obj = yyjson_mut_arr_add_obj(msg.doc, diag_arr);
		yyjson_mut_obj_add_str(msg.doc, diag_obj, "source", diag->source);
		yyjson_mut_obj_add_str(msg.doc, diag_obj, "message", diag->message);
		yyjson_mut_obj_add_int(msg.doc, diag_obj, "severity", diag->severity);
		buxn_ls_serialize_lsp_range(
			msg.doc,
			yyjson_mut_obj_add_obj(msg.doc, diag_obj, "range"),
			&diag->location.range
		);
		// TODO: convert related_message to information diagnostic if client
		// does not have the capability
		if (diag->related_message) {
			yyjson_mut_val* related_arr = yyjson_mut_obj_add_arr(msg.doc, diag_obj, "relatedInformation");
			yyjson_mut_val* related_obj = yyjson_mut_arr_add_obj(msg.doc, related_arr);
			yyjson_mut_obj_add_str(msg.doc, related_obj, "message", diag->related_message);
			yyjson_mut_val* location_obj = yyjson_mut_obj_add_obj(msg.doc, related_obj, "location");
			yyjson_mut_obj_add_str(msg.doc, location_obj, "uri", diag->location.uri);
			buxn_ls_serialize_lsp_range(
				msg.doc,
				yyjson_mut_obj_add_obj(msg.doc, location_obj, "range"),
				&diag->related_location.range
			);
		}
	}
	// Last message
	if (msg.method != NULL) {
		BIO_DEBUG("Sending diagnostic for: %s", last_uri);
		buxn_ls_end_msg(ctx, &msg);
	}

	// Unpublish from files with no diagnostic
	for (bhash_index_t i = 0; i < bhash_len(ctx->previously_diagnosed_files); ++i) {
		char* uri = ctx->previously_diagnosed_files->keys[i];
		BIO_DEBUG("Clearing diagnostic for: %s", uri);
		msg = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_NOTIFICATION, NULL);
		msg.method = "textDocument/publishDiagnostics";
		msg.value = yyjson_mut_obj(msg.doc);
		yyjson_mut_obj_add_str(msg.doc, msg.value, "uri", uri);
		yyjson_mut_obj_add_arr(msg.doc, msg.value, "diagnostics");
		buxn_ls_end_msg(ctx, &msg);

		buxn_ls_free(uri);
	}
	bhash_clear(ctx->previously_diagnosed_files);

	// Swap current and previous set
	buxn_ls_str_set_t* tmp = ctx->previously_diagnosed_files;
	ctx->previously_diagnosed_files = ctx->currently_diagnosed_files;
	ctx->currently_diagnosed_files = tmp;
}

static const buxn_ls_sym_node_t*
buxn_ls_find_definition(buxn_ls_ctx_t* ctx, yyjson_val* text_document_position) {
	const char* uri = yyjson_get_str(
		BIO_LSP_JSON_GET_LIT(BIO_LSP_JSON_GET_LIT(text_document_position, "textDocument"), "uri")
	);
	if (uri == NULL) { return NULL; }

	const char* path = buxn_ls_workspace_resolve_path(&ctx->workspace, (char*)uri);
	if (path == NULL) { return NULL; }

	bhash_index_t node_index = bhash_find(&ctx->analyzer.current_ctx->sources, path);
	if (!bhash_is_valid(node_index)) { return NULL; }

	yyjson_val* position = BIO_LSP_JSON_GET_LIT(text_document_position, "position");
	int line = yyjson_get_int(BIO_LSP_JSON_GET_LIT(position, "line"));
	int character = yyjson_get_int(BIO_LSP_JSON_GET_LIT(position, "character"));

	const buxn_ls_src_node_t* node = ctx->analyzer.current_ctx->sources.values[node_index];
	for (buxn_ls_sym_node_t* ref = node->references; ref != NULL; ref = ref->next) {
		if (
			(ref->range.start.line <= line && line <= ref->range.end.line)
			&& (ref->range.start.character <= character && character < ref->range.end.character)
		) {
			if (ref->base.out_edges == NULL) { return NULL; }
			return BCONTAINER_OF(ref->base.out_edges->to, buxn_ls_sym_node_t, base);
		}
	}

	return NULL;
}

static void
buxn_ls_handle_find_definition(
	buxn_ls_ctx_t* ctx,
	const bio_lsp_in_msg_t* request,
	bio_lsp_out_msg_t* response
) {
	const buxn_ls_sym_node_t* def = buxn_ls_find_definition(ctx, request->value);
	if (def != NULL) {
		response->value = yyjson_mut_obj(response->doc);
		yyjson_mut_obj_add_str(response->doc, response->value, "uri", def->source->uri);
		yyjson_mut_val* range = yyjson_mut_obj_add_obj(response->doc, response->value, "range");
		buxn_ls_serialize_lsp_range(response->doc, range, &def->range);
	} else {
		response->value = yyjson_mut_null(response->doc);
	}
}

static void
buxn_ls_handle_hover(
	buxn_ls_ctx_t* ctx,
	const bio_lsp_in_msg_t* request,
	bio_lsp_out_msg_t* response
) {
	const buxn_ls_sym_node_t* def = buxn_ls_find_definition(ctx, request->value);
	if (def == NULL) {
		response->value = yyjson_mut_null(response->doc);
		return;
	}

	char* uri = buxn_ls_arena_strcpy(&ctx->request_arena, def->source->uri);
	const char* path = buxn_ls_workspace_resolve_path(&ctx->workspace, uri);
	if (path == NULL) {
		response->value = yyjson_mut_null(response->doc);
		return;
	}

	buxn_ls_line_slice_t slice = buxn_ls_split_file(&ctx->analyzer, path);
	if (def->range.start.line >= slice.num_lines) {
		response->value = yyjson_mut_null(response->doc);
		return;
	}

	buxn_ls_str_t line = slice.lines[def->range.start.line];
	response->value = yyjson_mut_obj(response->doc);
	yyjson_mut_obj_add_strn(response->doc, response->value, "contents", line.chars, line.len);
	yyjson_mut_val* range = yyjson_mut_obj_add_obj(response->doc, response->value, "range");
	buxn_ls_serialize_lsp_range(response->doc, range, &def->range);
}

static void
buxn_ls_handle_list_doc_symbols(
	buxn_ls_ctx_t* ctx,
	const bio_lsp_in_msg_t* request,
	bio_lsp_out_msg_t* response
) {
	const char* uri = yyjson_get_str(
		BIO_LSP_JSON_GET_LIT(BIO_LSP_JSON_GET_LIT(request->value, "textDocument"), "uri")
	);
	if (uri == NULL) {
		response->value = yyjson_mut_null(response->doc);
		return;
	}

	const char* path = buxn_ls_workspace_resolve_path(&ctx->workspace, (char*)uri);
	if (path == NULL) {
		response->value = yyjson_mut_null(response->doc);
		return;
	}

	bhash_index_t src_node_index = bhash_find(&ctx->analyzer.current_ctx->sources, path);
	if (!bhash_is_valid(src_node_index)) {
		response->value = yyjson_mut_null(response->doc);
		return;
	}

	response->value = yyjson_mut_arr(response->doc);
	const buxn_ls_src_node_t* src_node = ctx->analyzer.current_ctx->sources.values[src_node_index];
	for (
		buxn_ls_sym_node_t* sym = src_node->definitions;
		sym != NULL;
		sym = sym->next
	) {
		yyjson_mut_val* sym_obj = yyjson_mut_arr_add_obj(response->doc, response->value);
		yyjson_mut_obj_add_str(response->doc, sym_obj, "name", sym->name);

		// TODO: use comment to change symbol type
		int kind;
		switch (sym->type) {
			case BUXN_ASM_SYM_MACRO:
				kind = 12;  // Function
				break;
			case BUXN_ASM_SYM_LABEL:
				kind = 13;  // Variable
				break;
			default:
				kind = 13;
				break;
		}
		yyjson_mut_obj_add_int(response->doc, sym_obj, "kind", kind);

		// TODO: Add signature
		buxn_ls_serialize_lsp_range(
			response->doc,
			yyjson_mut_obj_add_obj(response->doc, sym_obj, "range"),
			&sym->range
		);
		buxn_ls_serialize_lsp_range(
			response->doc,
			yyjson_mut_obj_add_obj(response->doc, sym_obj, "selectionRange"),
			&sym->range
		);
	}
}

static void
buxn_ls_handle_msg(buxn_ls_ctx_t* ctx, const bio_lsp_in_msg_t* in_msg) {
	switch (in_msg->type) {
		case BIO_LSP_MSG_REQUEST:
			barena_reset(&ctx->request_arena);
			if (strcmp(in_msg->method, "shutdown") == 0) {
				BIO_INFO("shutdown received");
				bio_cancel_timer(ctx->analyze_delay_timer);
				bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_RESULT, in_msg);
				reply.value = yyjson_mut_null(reply.doc);
				buxn_ls_end_msg(ctx, &reply);
			} else if (strcmp(in_msg->method, "textDocument/definition") == 0) {
				bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_RESULT, in_msg);
				buxn_ls_handle_find_definition(ctx, in_msg, &reply);
				buxn_ls_end_msg(ctx, &reply);
			} else if (strcmp(in_msg->method, "textDocument/hover") == 0) {
				bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_RESULT, in_msg);
				buxn_ls_handle_hover(ctx, in_msg, &reply);
				buxn_ls_end_msg(ctx, &reply);
			} else if (strcmp(in_msg->method, "textDocument/documentSymbol") == 0) {
				bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_RESULT, in_msg);
				buxn_ls_handle_list_doc_symbols(ctx, in_msg, &reply);
				buxn_ls_end_msg(ctx, &reply);
			} else {
				BIO_WARN("Client called an unimplemented method: %s", in_msg->method);
				bio_lsp_out_msg_t reply = buxn_ls_begin_msg(ctx, BIO_LSP_MSG_ERROR, in_msg);
				reply.value = yyjson_mut_obj(reply.doc);
				yyjson_mut_obj_add_int(reply.doc, reply.value, "code", -32601);
				yyjson_mut_obj_add_str(reply.doc, reply.value, "message", "Method not found");
				buxn_ls_end_msg(ctx, &reply);
			}
			break;
		case BIO_LSP_MSG_NOTIFICATION:
			if (strcmp(in_msg->method, "exit") == 0) {
				BIO_INFO("exit received");
				ctx->should_terminate = true;
			} else if (BIO_LSP_STR_STARTS_WITH(in_msg->method, "textDocument/")) {
				buxn_ls_workspace_update(&ctx->workspace, in_msg);
				if (bio_is_timer_pending(ctx->analyze_delay_timer)) {
					bio_reset_timer(ctx->analyze_delay_timer, BUXN_LS_ANALYZE_DELAY_MS);
				} else {
					ctx->analyze_delay_timer = bio_create_timer(
						BIO_TIMER_ONESHOT,
						BUXN_LS_ANALYZE_DELAY_MS,
						buxn_ls_analyze_workspace, ctx
					);
				}
			} else {
				BIO_WARN("Dropped notification: %s", in_msg->method);
			}
			break;
		default:
			BIO_WARN("Dropped message");
			break;
	}
}

int
buxn_ls(bio_lsp_conn_t* conn, struct barena_pool_s* pool) {
	bio_lsp_msg_reader_t reader = { .conn = conn };

	int exit_code = 1;
	bio_error_t error = { 0 };
	void* recv_buf = NULL;
	size_t recv_buf_size = 0;
	size_t required_recv_buf_size = 0;
	bio_lsp_in_msg_t in_msg = { 0 };
	buxn_ls_ctx_t ctx = {
		.conn = conn,
		.json_allocator = {
			.realloc = buxn_ls_json_realloc,
			.malloc = buxn_ls_json_malloc,
			.free = buxn_ls_json_free,
		},
	};

	BIO_DEBUG("Waiting for client to call: initialize");

	bool initialized = false;
	while (!initialized) {
		if ((required_recv_buf_size = bio_lsp_recv_msg_header(&reader, &error)) == 0) {
			BIO_ERROR("Error while reading header: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			goto end;
		}
		if (required_recv_buf_size > recv_buf_size) {
			buxn_ls_free(recv_buf);
			recv_buf = buxn_ls_malloc(required_recv_buf_size);
			BIO_DEBUG("Resize recv buffer: %zu -> %zu", recv_buf_size, required_recv_buf_size);
			recv_buf_size = required_recv_buf_size;
		}

		if (!bio_lsp_recv_msg(&reader, recv_buf, &in_msg, &error)) {
			BIO_ERROR("Error while reading message: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			goto end;
		}

		switch (in_msg.type) {
			case BIO_LSP_MSG_NOTIFICATION:
				if (strcmp(in_msg.method, "exit") == 0) {
					exit_code = 0;
					goto end;
				}
				break;
			case BIO_LSP_MSG_REQUEST:
				if (strcmp(in_msg.method, "initialize") == 0) {
					if (!buxn_ls_initialize(&ctx, pool, &in_msg)) {
						goto end;
					}

					initialized = true;
				} else {
					BIO_ERROR("Client sent invalid message during initialization");
					goto end;
				}
				break;
			default:
				BIO_ERROR("Client sent invalid message during initialization");
				goto end;
		}
	}

	BIO_DEBUG("Waiting for client to send: initialized");

	initialized = false;
	while (!initialized) {
		if ((required_recv_buf_size = bio_lsp_recv_msg_header(&reader, &error)) == 0) {
			BIO_ERROR("Error while reading header: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			goto end;
		}
		if (required_recv_buf_size > recv_buf_size) {
			buxn_ls_free(recv_buf);
			recv_buf = buxn_ls_malloc(required_recv_buf_size);
			BIO_DEBUG("Resize recv buffer: %zu -> %zu", recv_buf_size, required_recv_buf_size);
			recv_buf_size = required_recv_buf_size;
		}

		if (!bio_lsp_recv_msg(&reader, recv_buf, &in_msg, &error)) {
			BIO_ERROR("Error while reading message: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			goto end;
		}

		switch (in_msg.type) {
			case BIO_LSP_MSG_NOTIFICATION:
				if (strcmp(in_msg.method, "exit") == 0) {
					exit_code = 0;
					goto end;
				} else if (strcmp(in_msg.method, "initialized") == 0) {
					initialized = true;
				}
				break;
			default:
				BIO_ERROR("Client sent invalid message during initialization");
				goto end;
		}
	}

	BIO_DEBUG("Initialized");

	while (!ctx.should_terminate) {
		if ((required_recv_buf_size = bio_lsp_recv_msg_header(&reader, &error)) == 0) {
			BIO_ERROR("Error while reading header: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			goto end;
		}
		if (required_recv_buf_size > recv_buf_size) {
			buxn_ls_free(recv_buf);
			recv_buf = buxn_ls_malloc(required_recv_buf_size);
			BIO_DEBUG("Resize recv buffer: %zu -> %zu", recv_buf_size, required_recv_buf_size);
			recv_buf_size = required_recv_buf_size;
		}

		if (!bio_lsp_recv_msg(&reader, recv_buf, &in_msg, &error)) {
			BIO_ERROR("Error while reading message: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			goto end;
		}

		buxn_ls_handle_msg(&ctx, &in_msg);
	}

	exit_code = 0;
end:
	buxn_ls_cleanup(&ctx);
	buxn_ls_free(recv_buf);

	BIO_DEBUG("Shutdown");
	return exit_code;
}

int
buxn_ls_stdio(void* userdata) {
	(void)userdata;
	bio_lsp_file_conn_t conn;
	barena_pool_t pool;
	barena_pool_init(&pool, 1);
	int exit_code = buxn_ls(bio_lsp_init_file_conn(&conn, BIO_STDIN, BIO_STDOUT), &pool);
	barena_pool_cleanup(&pool);
	return exit_code;
}
