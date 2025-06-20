#include "completion.h"
#include "analyze.h"
#include <bmacro.h>

typedef enum {
	BUXN_LS_MATCH_ANY_SYMBOL,
	BUXN_LS_MATCH_ANY_LABEL,
	BUXN_LS_MATCH_ZERO_LABEL,
	BUXN_LS_MATCH_LOCAL_LABEL,
	BUXN_LS_MATCH_SUB_LABEL,
	// BUXN_LS_MATCH_NEARBY_LABEL,   // TODO: approximate label address around completion context
	BUXN_LS_MATCH_PRECEDING_LABEL,
} buxn_ls_sym_match_type_t;

typedef enum {
	BUXN_LS_FORMAT_FULL_NAME,
	BUXN_LS_FORMAT_LOCAL_NAME,
} buxn_ls_sym_format_type_t;

typedef struct {
	bool labels_only;
	bool preceding_labels;
	bio_lsp_position_t prefix_pos;
	uint16_t addr_min;
	uint16_t addr_max;
	buxn_ls_str_t prefix;
} buxn_ls_sym_filter_t;

typedef struct {
	buxn_ls_sym_filter_t filter;
	buxn_ls_sym_format_type_t format_type;
} buxn_ls_sym_visit_ctx_t;

static inline buxn_ls_str_t
buxn_ls_str_pop_front(buxn_ls_str_t str) {
	return (buxn_ls_str_t){
		.chars = str.chars + 1,
		.len = str.len - 1,
	};
}

static bool
buxn_ls_match_symbol(
	const buxn_ls_sym_node_t* def,
	const buxn_ls_sym_filter_t* filter
) {
	if (def->type == BUXN_ASM_SYM_LABEL) {
		if (
			filter->preceding_labels
			&& bio_lsp_cmp_pos(def->range.start, filter->prefix_pos) >= 0
		) {
			return false;
		}

		if (def->address < filter->addr_min || def->address > filter->addr_max) {
			return false;
		}
	} else {
		if (filter->labels_only) {
			return false;
		}

		// Label cannot be forward declared
		if (bio_lsp_cmp_pos(def->range.start, filter->prefix_pos) >= 0) {
			return false;
		}
	}

	size_t name_len = strlen(def->name);
	return name_len >= filter->prefix.len
		&& memcmp(def->name, filter->prefix.chars, filter->prefix.len) == 0;
}

static void
buxn_ls_visit_symbols(
	const buxn_ls_sym_visit_ctx_t* ctx,
	const buxn_ls_src_node_t* src_node
) {
	for (
		const buxn_ls_sym_node_t* def = src_node->definitions;
		def != NULL;
		def = def->next
	) {
		if (buxn_ls_match_symbol(def, &ctx->filter)) {
			buxn_ls_str_t suggestion = {
				.chars = def->name,
				.len = strlen(def->name),
			};
			switch (ctx->format_type) {
				case BUXN_LS_FORMAT_FULL_NAME:
					break;
				case BUXN_LS_FORMAT_LOCAL_NAME:
					for (size_t i = 0; i < suggestion.len; ++i) {
						char ch = def->name[i];
						if (ch == '/') {
							suggestion.chars += i + 1;
							suggestion.len -= (i + 1);
						}
					}
					break;
			}
			BIO_DEBUG(
				"Candidate: %s => %.*s",
				def->name,
				(int)suggestion.len, suggestion.chars
			);
		}
	}

	for (
		const buxn_ls_edge_t* edge = src_node->base.out_edges;
		edge != NULL;
		edge = edge->next_out
	) {
		buxn_ls_visit_symbols(ctx, BCONTAINER_OF(edge->to, buxn_ls_src_node_t, base));
	}
}

struct yyjson_mut_val*
buxn_ls_build_completion_list(
	const buxn_ls_completion_ctx_t* ctx,
	struct yyjson_mut_doc* response
) {
	if (ctx->prefix.len == 0) { return NULL; }

	BIO_DEBUG("Completion prefix: %.*s", (int)ctx->prefix.len, ctx->prefix.chars);

	buxn_ls_sym_filter_t filter = {
		.prefix_pos = ctx->lsp_range.start,
		.labels_only = true,
		.addr_min = 0,
		.addr_max = 0xffff,
	};
	buxn_ls_sym_match_type_t match_type;
	buxn_ls_sym_format_type_t format_type;
	int text_edit_start;

	char prefix_rune = ctx->prefix.chars[0];
	if (
		prefix_rune == ','
		|| prefix_rune == '_'
		|| prefix_rune == '!'
		|| prefix_rune == '?'
	) {
		// TODO: Implement this somehow
		// The address of the completion context could be approximated
		// We can widen the search range to +/-128 bytes from the addresses of
		// the preceding and following labels
		// filter = BUXN_LS_MATCH_NEARBY_LABEL;
		match_type = BUXN_LS_MATCH_LOCAL_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
	} else if (prefix_rune == '.' || prefix_rune == '-') {
		match_type = BUXN_LS_MATCH_ZERO_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
	} else if (prefix_rune == ';' || prefix_rune == '=') {
		match_type = BUXN_LS_MATCH_ANY_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
	} else if (prefix_rune == '/' || prefix_rune == '&') {
		match_type = BUXN_LS_MATCH_SUB_LABEL;
		format_type = BUXN_LS_FORMAT_LOCAL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
	} else if (prefix_rune == '|' || prefix_rune == '$') {
		match_type = BUXN_LS_MATCH_PRECEDING_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
	} else {
		match_type = BUXN_LS_MATCH_ANY_SYMBOL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = ctx->prefix;
		text_edit_start = ctx->prefix_start_byte;
	}

	if (
		match_type != BUXN_LS_MATCH_ANY_SYMBOL
		&& ctx->prefix.len >= 2
		&& (ctx->prefix.chars[1] == '&' || ctx->prefix.chars[1] == '/')
	) {
		match_type = BUXN_LS_MATCH_SUB_LABEL;
		format_type = BUXN_LS_FORMAT_LOCAL_NAME;
		filter.prefix = buxn_ls_str_pop_front(filter.prefix);
		text_edit_start = ctx->prefix_start_byte + 2;
	} else {
		// Forward slash (/) in name
		for (int i = ctx->prefix_start_byte + 1; i < ctx->prefix_end_byte; ++i) {
			char ch = ctx->line_content.chars[i];
			if (ch == '/') {
				// The filter type is not changed because:
				// * The prefix rune could have restricted the search list
				// * a/b is a legal macro name
				// Only the formatting is affected
				format_type = BUXN_LS_FORMAT_LOCAL_NAME;
				text_edit_start = i + 1;
				break;
			}
		}
	}

	switch (match_type) {
		case BUXN_LS_MATCH_ANY_SYMBOL:
			filter.labels_only = false;
			break;
		case BUXN_LS_MATCH_LOCAL_LABEL:
		case BUXN_LS_MATCH_SUB_LABEL: {
			// Find the most recently defined label
			const buxn_ls_sym_node_t* most_recent_label = NULL;
			bio_lsp_position_t pos = {
				.line = -1,
				.character = -1,
			};
			for (
				const buxn_ls_sym_node_t* def = ctx->source->definitions;
				def != NULL;
				def = def->next
			) {
				if (def->type != BUXN_ASM_SYM_LABEL) { continue; }
				bio_lsp_position_t sym_start = def->range.start;
				// Find the symbol with the greatest position that is defined before
				// the completion prefix
				if (
					bio_lsp_cmp_pos(sym_start, pos) > 0
					&& bio_lsp_cmp_pos(sym_start, ctx->lsp_range.start) < 0
				) {
					most_recent_label = def;
					pos = def->range.start;
				}
			}

			if (most_recent_label == NULL) { return NULL; }

			// TODO: use buxn_ls_str instead
			int name_len = (int)strlen(most_recent_label->name);
			int i;
			for (i = 0; i < name_len; ++i) {
				char ch = most_recent_label->name[i];
				if (ch == '/') {
					break;
				}
			}
			// TODO: construct a prefix that is a combination of:
			//
			// * The most recent parent scope
			// * The completion prefix
			filter.prefix = (buxn_ls_str_t) {
				.chars = most_recent_label->name,
				// Local label includes the bare parent label (e.g: parent)
				// Sub label only includes the child label (e.g: parent/child)
				// Including the slash filters out the parent
				.len = i + (match_type == BUXN_LS_MATCH_LOCAL_LABEL ? 0 : 1),
			};
		} break;
		case BUXN_LS_MATCH_ANY_LABEL:
			break;
		case BUXN_LS_MATCH_ZERO_LABEL:
			filter.addr_max = 0x00ff;
			break;
		// BUXN_LS_MATCH_NEARBY_LABEL,   // TODO: approximate label address around completion context
		case BUXN_LS_MATCH_PRECEDING_LABEL:
			filter.preceding_labels = true;
			break;
	}

	buxn_ls_visit_symbols(
		&(buxn_ls_sym_visit_ctx_t){
			.filter = filter,
			.format_type = format_type,
		},
		ctx->source
	);
	(void)text_edit_start;

	return NULL;
}
