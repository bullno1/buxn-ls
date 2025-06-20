#ifndef BIO_LSP_H
#define BIO_LSP_H

#include <bio/bio.h>
#include <bio/net.h>
#include <bio/file.h>
#include <bio/buffering.h>

#define BIO_LSP_LIT_STRLEN(X) (sizeof(X "") - 1)

#define BIO_LSP_JSON_GET_LIT(OBJ, KEY) \
	yyjson_obj_getn(OBJ, KEY, BIO_LSP_LIT_STRLEN(KEY))

#define BIO_LSP_JSON_LIT_STR(DOC, STR) \
	yyjson_mut_strn((DOC), STR, BIO_LSP_LIT_STRLEN(STR))

#define BIO_LSP_STR_STARTS_WITH(STR, PREFIX) \
	(strncmp((STR), PREFIX, BIO_LSP_LIT_STRLEN(PREFIX)) == 0)

struct yyjson_alc;

typedef enum {
	BIO_LSP_MSG_REQUEST,
	BIO_LSP_MSG_RESULT,
	BIO_LSP_MSG_ERROR,
	BIO_LSP_MSG_NOTIFICATION,
} bio_lsp_msg_type_t;

typedef struct bio_lsp_in_msg_s {
	bio_lsp_msg_type_t type;
	struct yyjson_doc* doc;
	struct yyjson_val* id;
	const char* method;
	struct yyjson_val* value;
} bio_lsp_in_msg_t;

typedef struct {
	bio_lsp_msg_type_t type;
	struct yyjson_mut_doc* doc;
	union {
		struct yyjson_val* original_id;
		struct yyjson_mut_val* new_id;
	};
	const char* method;
	struct yyjson_mut_val* value;
} bio_lsp_out_msg_t;

typedef struct {
	int line;
	int character;
} bio_lsp_position_t;

typedef struct {
	bio_lsp_position_t start;
	bio_lsp_position_t end;
} bio_lsp_range_t;

typedef struct {
	const char* uri;
	bio_lsp_range_t range;
} bio_lsp_location_t;

size_t
bio_lsp_recv_msg_header(bio_io_buffer_t in_buf, bio_error_t* error);

bool
bio_lsp_parse_msg(
	char* buf, size_t content_length,
	bio_lsp_in_msg_t* msg,
	bio_error_t* error
);

bool
bio_lsp_send_msg(
	bio_io_buffer_t out_buf,
	struct yyjson_alc* alc,
	const bio_lsp_out_msg_t* msg,
	bio_error_t* error
);

ptrdiff_t
bio_lsp_utf16_offset_from_byte_offset(const char* utf8str, size_t str_size, ptrdiff_t byte_offset);

ptrdiff_t
bio_lsp_byte_offset_from_utf16_offset(const char* utf8str, size_t str_size, ptrdiff_t utf16_offset);

static inline int
bio_lsp_cmp_pos(bio_lsp_position_t lhs, bio_lsp_position_t rhs) {
	if (lhs.line == rhs.line) {
		return lhs.character - rhs.character;
	} else {
		return lhs.line - rhs.line;
	}
}

#endif
