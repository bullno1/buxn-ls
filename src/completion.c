#include "completion.h"

typedef enum {
	BUXN_LS_ANY_SYMBOL,
	BUXN_LS_ANY_LABEL,
	BUXN_LS_ZERO_LABEL,
} buxn_ls_sym_filter_type_t;

typedef enum {
	BUXN_LS_FORMAT_FULL_NAME,
	BUXN_LS_FORMAT_LOCAL_NAME,
} buxn_ls_sym_format_type_t;

typedef struct {
	buxn_ls_sym_filter_type_t filter;
	buxn_ls_sym_format_type_t format;
	buxn_ls_str_t prefix;
} buxn_ls_sym_filter_t;

struct yyjson_mut_val*
buxn_ls_build_completion_list(
	const buxn_ls_completion_ctx_t* ctx,
	struct yyjson_mut_doc* response
) {
	return NULL;
}
