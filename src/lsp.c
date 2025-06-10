#include "lsp.h"
#include <yyjson.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

#define BIO_CONTAINER_OF(ptr, type, member) \
	((type *)((char *)(1 ? (ptr) : &((type *)0)->member) - offsetof(type, member)))

typedef enum {
	BIO_LSP_BAD_HEADER,
	BIO_LSP_BAD_JSON,
	BIO_LSP_BAD_JSONRPC,
	BIO_LSP_CONNECTIN_CLOSED,
} bio_lsp_error_t;

static const bio_tag_t BIO_LSP_ERROR = BIO_TAG_INIT("bio.lsp.error");

static const char*
bio_lsp_format_error(int error) {
	switch ((bio_lsp_error_t)error) {
		case BIO_LSP_BAD_HEADER:
			return "Bad header";
		case BIO_LSP_BAD_JSON:
			return "Bad JSON";
		case BIO_LSP_BAD_JSONRPC:
			return "Bad JSON-RPC message";
		case BIO_LSP_CONNECTIN_CLOSED:
			return "Connection closed";
	}
	return "Unknown error";
}

static void
bio_lsp_set_error(bio_error_t* error, int code, const char* file, int line) {
	if (error != NULL) {
		error->tag = &BIO_LSP_ERROR;
		error->code = code;
		error->strerror = bio_lsp_format_error;
		error->file = file;
		error->line = line;
	}
}

#define bio_lsp_set_error(error, code) bio_lsp_set_error(error, code, __FILE__, __LINE__)

static inline size_t
bio_lsp_recv(bio_lsp_conn_t* conn, void* buf, size_t size, bio_error_t* error) {
	return conn->recv(conn, buf, size, error);
}

static inline size_t
bio_lsp_send(bio_lsp_conn_t* conn, const void* buf, size_t size, bio_error_t* error) {
	return conn->send(conn, buf, size, error);
}

static inline bool
bio_lsp_recv_exactly(bio_lsp_conn_t* conn, void* buf, size_t size, bio_error_t* error) {
	char* recv_buf = buf;
	while (size > 0) {
		size_t bytes_received = bio_lsp_recv(conn, recv_buf, size, error);
		if (bio_has_error(error)) { return false; }

		size -= bytes_received;
		recv_buf += bytes_received;
	}
	return true;
}

static inline bool
bio_lsp_send_exactly(bio_lsp_conn_t* conn, const void* buf, size_t size, bio_error_t* error) {
	const char* send_buf = buf;
	while (size > 0) {
		size_t bytes_sent = bio_lsp_send(conn, send_buf, size, error);
		if (bio_has_error(error)) { return false; }

		size -= bytes_sent;
		send_buf += bytes_sent;
	}
	return true;
}

static const char*
bio_lsp_recv_line(bio_lsp_msg_reader_t* reader, bio_error_t* error) {
	if (reader->line_end != NULL) {
		size_t line_len = reader->line_end - reader->line_buf;
		memmove(reader->line_buf, reader->line_end, reader->buf_size - line_len);
		reader->buf_size -= line_len;
		reader->line_end = NULL;
	}

	while (true) {
		for (int i = 0; i < (int)reader->buf_size - 1; ++i) {
			if (
				reader->line_buf[i] == '\r'
				&& reader->line_buf[i + 1] == '\n'
			) {
				reader->line_buf[i] = '\0';
				reader->line_end = reader->line_buf + i + 2;
				return reader->line_buf;
			}
		}

		size_t bytes_recv = bio_lsp_recv(
			reader->conn,
			reader->line_buf + reader->buf_size,
			sizeof(reader->line_buf) - reader->buf_size,
			error
		);
		if (bytes_recv == 0) {
			if (!bio_has_error(error)) {
				bio_lsp_set_error(error, BIO_LSP_CONNECTIN_CLOSED);
			}
			return NULL;
		}
		reader->buf_size += bytes_recv;
	}
}

static size_t
bio_lsp_socket_conn_recv(bio_lsp_conn_t* conn, void* buf, size_t size, bio_error_t* error) {
	bio_lsp_socket_conn_t* socket_conn = BIO_CONTAINER_OF(conn, bio_lsp_socket_conn_t, conn);
	return bio_net_recv(socket_conn->socket, buf, size, error);
}

static size_t
bio_lsp_socket_conn_send(bio_lsp_conn_t* conn, const void* buf, size_t size, bio_error_t* error) {
	bio_lsp_socket_conn_t* socket_conn = BIO_CONTAINER_OF(conn, bio_lsp_socket_conn_t, conn);
	return bio_net_send(socket_conn->socket, buf, size, error);
}

static size_t
bio_lsp_file_conn_recv(bio_lsp_conn_t* conn, void* buf, size_t size, bio_error_t* error) {
	bio_lsp_file_conn_t* file_conn = BIO_CONTAINER_OF(conn, bio_lsp_file_conn_t, conn);
	return bio_fread(file_conn->in, buf, size, error);
}

static size_t
bio_lsp_file_conn_send(bio_lsp_conn_t* conn, const void* buf, size_t size, bio_error_t* error) {
	bio_lsp_file_conn_t* file_conn = BIO_CONTAINER_OF(conn, bio_lsp_file_conn_t, conn);
	return bio_fwrite(file_conn->out, buf, size, error);
}

bio_lsp_conn_t*
bio_lsp_init_socket_conn(bio_lsp_socket_conn_t* conn, bio_socket_t socket) {
	conn->conn.recv = bio_lsp_socket_conn_recv;
	conn->conn.send = bio_lsp_socket_conn_send;
	conn->socket = socket;
	return &conn->conn;
}

bio_lsp_conn_t*
bio_lsp_init_file_conn(bio_lsp_file_conn_t* conn, bio_file_t input, bio_file_t output) {
	conn->conn.recv = bio_lsp_file_conn_recv;
	conn->conn.send = bio_lsp_file_conn_send;
	conn->in = input;
	conn->out = output;
	return &conn->conn;
}

size_t
bio_lsp_recv_msg_header(bio_lsp_msg_reader_t* reader, bio_error_t* error) {
	size_t content_length = 0;

	while (true) {
		const char* line = bio_lsp_recv_line(reader, error);
		if (line == NULL) {
			return 0;
		} else if (line[0] == '\0') {
			if (content_length == 0) {
				bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
				return 0;
			}

			reader->next_msg_size = content_length;
			return yyjson_read_max_memory_usage(content_length, YYJSON_READ_INSITU) + content_length;
		} else if (strncmp(line, "Content-Length: ", BIO_LSP_LIT_STRLEN("Content-Length: ")) == 0) {
			char *endptr = NULL;
			errno = 0;
			unsigned long long val = strtoull(
				line + BIO_LSP_LIT_STRLEN("Content-Length: "), &endptr, 10
			);
			if (errno == ERANGE || val > SIZE_MAX) {
				bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
				return 0;
			}

			if (*endptr != '\0') {
				bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
				return 0;
			}

			content_length = val;
		}
	}
}

bool
bio_lsp_recv_msg(
	bio_lsp_msg_reader_t* reader,
	char* buf,
	bio_lsp_in_msg_t* msg,
	bio_error_t* error
) {
	if (reader->line_end != NULL) {
		size_t line_len = reader->line_end - reader->line_buf;
		memmove(reader->line_buf, reader->line_end, reader->buf_size - line_len);
		reader->buf_size -= line_len;
		reader->line_end = NULL;
	}

	size_t msg_size = reader->next_msg_size;
	size_t read_from_buf_size = reader->buf_size <= msg_size ? reader->buf_size : msg_size;
	memcpy(buf, reader->line_buf, read_from_buf_size);
	memmove(reader->line_buf, reader->line_buf + read_from_buf_size, reader->buf_size - read_from_buf_size);
	reader->buf_size -= read_from_buf_size;
	if (!bio_lsp_recv_exactly(
		reader->conn,
		buf + read_from_buf_size, msg_size - read_from_buf_size,
		error
	)) {
		return false;
	}

	reader->next_msg_size = 0;
	yyjson_read_err yyjson_err = { 0 };
	yyjson_alc alc;
	yyjson_alc_pool_init(
		&alc,
		buf + msg_size,
		yyjson_read_max_memory_usage(msg_size, YYJSON_READ_INSITU)
	);
	msg->doc = yyjson_read_opts(
		buf, msg_size,
		YYJSON_READ_INSITU,
		&alc,
		&yyjson_err
	);
	if (msg->doc == NULL) {
		bio_lsp_set_error(error, BIO_LSP_BAD_JSON);
		return false;
	}
	yyjson_val* root = yyjson_doc_get_root(msg->doc);
	msg->method = yyjson_get_str(BIO_LSP_JSON_GET_LIT(root, "method"));
	if (msg->method == NULL) {
		yyjson_val* value = BIO_LSP_JSON_GET_LIT(root, "result");
		if (value != NULL) {
			msg->type = BIO_LSP_MSG_RESULT;
			msg->value = value;
		} else {
			value = BIO_LSP_JSON_GET_LIT(root, "error");
			if (value == NULL) {
				bio_lsp_set_error(error, BIO_LSP_BAD_JSONRPC);
				return false;
			}

			msg->type = BIO_LSP_MSG_ERROR;
			msg->value = value;
		}
	} else {
		msg->id = BIO_LSP_JSON_GET_LIT(root, "id");
		msg->value = BIO_LSP_JSON_GET_LIT(root, "params");
		msg->type = msg->id == NULL ? BIO_LSP_MSG_NOTIFICATION : BIO_LSP_MSG_REQUEST;
	}

	return true;
}

bool
bio_lsp_send_msg(
	bio_lsp_conn_t* conn,
	struct yyjson_alc* alc,
	const bio_lsp_out_msg_t* msg,
	bio_error_t* error
) {
	yyjson_mut_doc* doc = msg->doc;
	yyjson_mut_val* root = yyjson_mut_obj(doc);
	yyjson_mut_obj_add(
		root,
		BIO_LSP_JSON_LIT_STR(doc, "jsonrpc"),
		BIO_LSP_JSON_LIT_STR(doc, "2.0")
	);
	switch (msg->type) {
		case BIO_LSP_MSG_REQUEST:
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "id"), msg->new_id);
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "method"), yyjson_mut_str(doc, msg->method));
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "params"), msg->value);
			break;
		case BIO_LSP_MSG_NOTIFICATION:
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "method"), yyjson_mut_str(doc, msg->method));
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "params"), msg->value);
			break;
		case BIO_LSP_MSG_RESULT:
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "id"), yyjson_val_mut_copy(doc, msg->original_id));
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "result"), msg->value);
			break;
		case BIO_LSP_MSG_ERROR:
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "id"), yyjson_val_mut_copy(doc, msg->original_id));
			yyjson_mut_obj_add(root, BIO_LSP_JSON_LIT_STR(doc, "error"), msg->value);
			break;
	}
	yyjson_mut_doc_set_root(doc, root);
	size_t content_length;
	char* body = yyjson_mut_write_opts(
		doc,
		YYJSON_WRITE_NOFLAG,
		alc,
		&content_length,
		NULL
	);
	bool success = false;
	/*BIO_TRACE("%.*s", (int)content_length, body);*/

	char header[32];
	int header_len = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", content_length);
	if (!bio_lsp_send_exactly(conn, header, (size_t)header_len, error)) {
		goto end;
	}

	if (!bio_lsp_send_exactly(conn, body, content_length, error)) {
		goto end;
	}

	success = true;
end:
	alc->free(alc->ctx, body);
	return success;
}
