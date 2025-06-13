#ifndef BIO_LSP_H
#define BIO_LSP_H

#include <bio/bio.h>
#include <bio/net.h>
#include <bio/file.h>

#define BIO_LSP_LIT_STRLEN(X) (sizeof(X "") - 1)

#define BIO_LSP_JSON_GET_LIT(OBJ, KEY) \
	yyjson_obj_getn(OBJ, KEY, BIO_LSP_LIT_STRLEN(KEY))

#define BIO_LSP_JSON_LIT_STR(DOC, STR) \
	yyjson_mut_strn((DOC), STR, BIO_LSP_LIT_STRLEN(STR))

#define BIO_LSP_STR_STARTS_WITH(STR, PREFIX) \
	(strncmp((STR), PREFIX, BIO_LSP_LIT_STRLEN(PREFIX)) == 0)

typedef struct bio_lsp_conn_s bio_lsp_conn_t;
struct yyjson_alc;

struct bio_lsp_conn_s {
	size_t (*recv)(bio_lsp_conn_t* conn, void* buf, size_t size, bio_error_t* error);
	size_t (*send)(bio_lsp_conn_t* conn, const void* buf, size_t size, bio_error_t* error);
};

typedef struct {
	bio_lsp_conn_t conn;
	bio_socket_t socket;
} bio_lsp_socket_conn_t;

typedef struct {
	bio_lsp_conn_t conn;
	bio_file_t in;
	bio_file_t out;
} bio_lsp_file_conn_t;

typedef struct {
	bio_lsp_conn_t* conn;
	size_t next_msg_size;
	char* line_end;
	char line_buf[1024];
	size_t buf_size;
} bio_lsp_msg_reader_t;

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

bio_lsp_conn_t*
bio_lsp_init_socket_conn(bio_lsp_socket_conn_t* conn, bio_socket_t socket);

bio_lsp_conn_t*
bio_lsp_init_file_conn(bio_lsp_file_conn_t* conn, bio_file_t input, bio_file_t output);

size_t
bio_lsp_recv_msg_header(bio_lsp_msg_reader_t* reader, bio_error_t* error);

bool
bio_lsp_recv_msg(
	bio_lsp_msg_reader_t* reader,
	char* recv_buf,
	bio_lsp_in_msg_t* msg,
	bio_error_t* error
);

bool
bio_lsp_send_msg(
	bio_lsp_conn_t* conn,
	struct yyjson_alc* alc,
	const bio_lsp_out_msg_t* msg,
	bio_error_t* error
);

ptrdiff_t
bio_lsp_utf16_offset_from_byte_offset(const char* utf8str, size_t str_size, ptrdiff_t byte_offset);

ptrdiff_t
bio_lsp_byte_offset_from_utf16_offset(const char* utf8str, size_t str_size, ptrdiff_t utf16_offset);

#endif
