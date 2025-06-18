#include "lsp.h"
#include <yyjson.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <utf8proc.h>

typedef enum {
	BIO_LSP_BAD_HEADER,
	BIO_LSP_BAD_JSON,
	BIO_LSP_BAD_JSONRPC,
	BIO_LSP_CONNECTION_CLOSED,
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
		case BIO_LSP_CONNECTION_CLOSED:
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

static bool
bio_lsp_get_char(bio_io_buffer_t in_buf, char* out, bio_error_t* error) {
	return bio_buffered_read(in_buf, out, sizeof(char), error) == 1;
}

static const char*
bio_lsp_recv_line(
	bio_io_buffer_t in_buf,
	char* line_buf, size_t buf_size,
	bio_error_t* error
) {
	char* end = line_buf + buf_size;
	char* current = line_buf;

	while (true) {
		if (current >= end) {
			bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
			return NULL;
		}

		if (!bio_lsp_get_char(in_buf, current, error)) {
			return NULL;
		}

		if (*current == '\r') {
			++current;
			if (current >= end) {
				bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
				return NULL;
			}
			if (!bio_lsp_get_char(in_buf, current, error)) {
				return NULL;
			}

			if (*current != '\n') {
				bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
				return NULL;
			}

			current[-1] = '\0';
			return line_buf;
		} else {
			++current;
		}
	}
}

size_t
bio_lsp_recv_msg_header(bio_io_buffer_t in_buf, bio_error_t* error) {
	size_t content_length = 0;
	char line_buf[1024];

	while (true) {
		const char* line = bio_lsp_recv_line(in_buf, line_buf, sizeof(line_buf), error);
		if (line == NULL) {
			return 0;
		} else if (line[0] == '\0') {
			if (content_length == 0) {
				bio_lsp_set_error(error, BIO_LSP_BAD_HEADER);
				return 0;
			}

			return content_length;
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
bio_lsp_parse_msg(
	char* buf, size_t content_length,
	bio_lsp_in_msg_t* msg,
	bio_error_t* error
) {
	yyjson_read_err yyjson_err = { 0 };
	yyjson_alc alc;
	yyjson_alc_pool_init(
		&alc,
		buf + content_length,
		yyjson_read_max_memory_usage(content_length, YYJSON_READ_INSITU)
	);
	msg->doc = yyjson_read_opts(
		buf, content_length,
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
	bio_io_buffer_t out_buf,
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
	if (!bio_buffered_write_exactly(out_buf, header, (size_t)header_len, error)) {
		goto end;
	}

	if (!bio_buffered_write_exactly(out_buf, body, content_length, error)) {
		goto end;
	}

	if (!bio_flush_buffer(out_buf, error)) {
		goto end;
	}

	success = true;
end:
	alc->free(alc->ctx, body);
	return success;
}

ptrdiff_t
bio_lsp_utf16_offset_from_byte_offset(const char* utf8str, size_t str_size, ptrdiff_t byte_offset) {
	utf8proc_ssize_t itr = 0;
	utf8proc_ssize_t line_len = (utf8proc_ssize_t)str_size;
	ptrdiff_t code_unit_offset = 0;
	while (true) {
		if (itr >= byte_offset || itr >= line_len) { break; }

		utf8proc_int32_t codepoint;
		utf8proc_ssize_t num_bytes = utf8proc_iterate(
			(const utf8proc_uint8_t*)utf8str + itr,
			line_len - itr,
			&codepoint
		);

		if (num_bytes < 0) {
			// If an invalid codepoint is encountered, skip the byte
			itr += 1;
		} else {
			itr += num_bytes;
			code_unit_offset += (codepoint <= 0xFFFF ? 1 : 2);
		}
	}

	return code_unit_offset;
}

ptrdiff_t
bio_lsp_byte_offset_from_utf16_offset(const char* utf8str, size_t str_size, ptrdiff_t utf16_offset) {
	utf8proc_ssize_t itr = 0;
	utf8proc_ssize_t line_len = (utf8proc_ssize_t)str_size;
	ptrdiff_t code_unit_offset = 0;
	while (true) {
		if (code_unit_offset >= utf16_offset || itr >= line_len) { break; }

		utf8proc_int32_t codepoint;
		utf8proc_ssize_t num_bytes = utf8proc_iterate(
			(const utf8proc_uint8_t*)utf8str + itr,
			line_len - itr,
			&codepoint
		);

		if (num_bytes < 0) {
			// If an invalid codepoint is encountered, skip the byte
			itr += 1;
		} else {
			itr += num_bytes;
			code_unit_offset += (codepoint <= 0xFFFF ? 1 : 2);
		}
	}

	return itr;
}
