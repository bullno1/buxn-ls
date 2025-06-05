#include "ls.h"
#include "lsp.h"
#include "common.h"
#include "resources.h"
#include <string.h>
#include <yyjson.h>

typedef struct {
	bio_lsp_conn_t* conn;
	bool should_terminate;
	yyjson_alc json_allocator;
	char name_buf[sizeof("ls:2147483647")];
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
buxn_ls_begin_reply(buxn_ls_ctx_t* ctx, bio_lsp_msg_type_t type, const bio_lsp_in_msg_t* in_msg) {
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
buxn_ls_end_reply(buxn_ls_ctx_t* ctx, const bio_lsp_out_msg_t* msg) {
	bio_error_t error = { 0 };
	bool success = bio_lsp_send_msg(ctx->conn, &ctx->json_allocator, msg, &error);
	yyjson_mut_doc_free(msg->doc);

	if (!success) {
		BIO_ERROR("Error while sending reply: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
	}

	return success;
}

static void
buxn_ls_initialize(buxn_ls_ctx_t* ctx, const bio_lsp_in_msg_t* msg) {
	int pid = yyjson_get_int(BIO_LSP_JSON_GET_LIT(msg->value, "processId"));
	snprintf(ctx->name_buf, sizeof(ctx->name_buf), "ls:%d", pid);
	bio_set_coro_name(ctx->name_buf);

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

	bio_lsp_out_msg_t reply = buxn_ls_begin_reply(ctx, BIO_LSP_MSG_RESULT, msg);
	reply.value = yyjson_val_mut_copy(reply.doc, yyjson_doc_get_root(initialize_doc));
	buxn_ls_end_reply(ctx, &reply);

	buxn_ls_free(initialize_mem);
}

static void
buxn_ls_cleanup(buxn_ls_ctx_t* ctx) {
}

static void
buxn_ls_handle_msg(buxn_ls_ctx_t* ctx, const bio_lsp_in_msg_t* in_msg) {
	switch (in_msg->type) {
		case BIO_LSP_MSG_REQUEST:
			if (strcmp(in_msg->method, "shutdown") == 0) {
				BIO_INFO("shutdown received");
				bio_lsp_out_msg_t reply = buxn_ls_begin_reply(ctx, BIO_LSP_MSG_RESULT, in_msg);
				reply.value = yyjson_mut_null(reply.doc);
				buxn_ls_end_reply(ctx, &reply);
			} else {
				bio_lsp_out_msg_t reply = buxn_ls_begin_reply(ctx, BIO_LSP_MSG_ERROR, in_msg);
				reply.value = yyjson_mut_obj(reply.doc);
				yyjson_mut_obj_add_int(reply.doc, reply.value, "code", -32601);
				yyjson_mut_obj_add_str(reply.doc, reply.value, "message", "Method not found");
				buxn_ls_end_reply(ctx, &reply);
			}
			break;
		case BIO_LSP_MSG_NOTIFICATION:
			if (strcmp(in_msg->method, "exit") == 0) {
				BIO_INFO("exit received");
				ctx->should_terminate = true;
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
buxn_ls(bio_lsp_conn_t* conn) {
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
					buxn_ls_initialize(&ctx, &in_msg);
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
	return buxn_ls(bio_lsp_init_file_conn(&conn, BIO_STDIN, BIO_STDOUT));
}
