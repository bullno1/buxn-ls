#include "completion.h"
#include "analyze.h"
#include "lsp.h"
#include <bmacro.h>
#include <yyjson.h>
#include <stdarg.h>

typedef enum {
	BUXN_LS_MATCH_ANY_SYMBOL,
	BUXN_LS_MATCH_ANY_LABEL,
	BUXN_LS_MATCH_ZERO_LABEL,
	BUXN_LS_MATCH_SUB_LABEL,
	BUXN_LS_MATCH_PRECEDING_LABEL,
} buxn_ls_sym_match_type_t;

typedef enum {
	BUXN_LS_FORMAT_FULL_NAME,
	BUXN_LS_FORMAT_LOCAL_NAME,
} buxn_ls_sym_format_type_t;

typedef struct {
	bool labels_only;
	bool preceding_labels;
	bool subroutine_only;
	bio_lsp_position_t prefix_pos;
	uint16_t addr_min;
	uint16_t addr_max;
	buxn_ls_str_t prefix;
} buxn_ls_sym_filter_t;

typedef struct {
	buxn_ls_sym_filter_t filter;
	buxn_ls_str_t current_scope;
	buxn_ls_completion_map_t* completion_items;
	bool group_symbols;
} buxn_ls_sym_visit_ctx_t;

static inline buxn_ls_str_t
buxn_ls_str_pop_front(buxn_ls_str_t str) {
	return (buxn_ls_str_t){
		.chars = str.chars + 1,
		.len = str.len - 1,
	};
}

static bool
buxn_ls_is_subroutine(const buxn_ls_sym_node_t* def) {
	if (def->semantics == BUXN_LS_SYMBOL_AS_SUBROUTINE) {
		// Has stack comment
		return true;
	} else {
		// Local label whose name starts with '>'
		for (size_t i = 0; i < def->name.len; ++i) {
			char ch = def->name.chars[i];
			if (ch == '/') {
				return i < def->name.len - 1 && def->name.chars[i + 1] == '>';
			}
		}

		return false;
	}
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

		if (filter->subroutine_only && !buxn_ls_is_subroutine(def)) {
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

	return def->name.len >= filter->prefix.len
		&& memcmp(def->name.chars, filter->prefix.chars, filter->prefix.len) == 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static void
buxn_ls_add_detail_fmt(
	barena_t* arena,
	yyjson_mut_doc* doc,
	yyjson_mut_val* item,
	const char* fmt,
	...
) {
	// TODO: use a reusable buffer and copy to arena instead of double formatting
	va_list args, args_copy;

	va_start(args, fmt);

	va_copy(args_copy, args);
	int strlen = vsnprintf(NULL, 0, fmt, args_copy);
	va_end(args_copy);

	char* str = barena_memalign(arena, strlen + 1, _Alignof(char));
	vsnprintf(str, strlen + 1, fmt, args);
	yyjson_mut_obj_add_strn(doc, item, "detail", str, strlen);

	va_end(args);
}

static bool
buxn_ls_cstr_eq(const void* lhs, const void* rhs, size_t size) {
	(void)size;
	const buxn_ls_str_t* lstr = lhs;
	const buxn_ls_str_t* rstr = rhs;
	return lstr->len == rstr->len
		&& memcmp(lstr->chars, rstr->chars, lstr->len) == 0;
}

static bhash_hash_t
buxn_ls_cstr_hash(const void* key, size_t size) {
	(void)size;
	const buxn_ls_str_t* str = key;
	return bhash_hash(str->chars, str->len);
}

static buxn_ls_str_t
buxn_ls_label_scope(buxn_ls_str_t name) {
	int i;
	for (i = 0; i < (int)name.len; ++i) {
		if (name.chars[i] == '/') { break; }
	}
	return (buxn_ls_str_t){
		.chars = name.chars,
		.len = i,
	};
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
			buxn_ls_str_t scope = buxn_ls_label_scope(def->name);
			bool is_local = buxn_ls_cstr_eq(&scope, &ctx->current_scope, 0);

			if (ctx->group_symbols) {
				buxn_ls_str_t key = is_local ? def->name : scope;
				bhash_alloc_result_t alloc_result = bhash_alloc(ctx->completion_items, key);
				if (alloc_result.is_new) {
					ctx->completion_items->keys[alloc_result.index] = key;
					ctx->completion_items->values[alloc_result.index] = (buxn_ls_completion_item_t){
						.sym = def,
						.size = 1,
						.is_local = is_local,
					};
				} else {
					ctx->completion_items->values[alloc_result.index].size += 1;
				}
			} else {
				buxn_ls_str_t key = def->name;
				buxn_ls_completion_item_t value = {
					.sym = def,
					.size = 1,
					.is_local = is_local,
				};
				bhash_put(ctx->completion_items, key, value);
			}
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

void
buxn_ls_completer_init(buxn_ls_completer_t* completer) {
	bhash_config_t config = bhash_config_default();
	config.eq = buxn_ls_cstr_eq;
	config.hash = buxn_ls_cstr_hash;
	bhash_init(&completer->completion_items, config);
}

void
buxn_ls_completer_cleanup(buxn_ls_completer_t* completer) {
	bhash_cleanup(&completer->completion_items);
}

struct yyjson_mut_val*
buxn_ls_build_completion_list(
	buxn_ls_completer_t* completer,
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
	bool group_symbols = false;
	int text_edit_start;

	char prefix_rune = ctx->prefix.chars[0];
	if (
		prefix_rune == ','
		|| prefix_rune == '_'
		|| prefix_rune == '!'
		|| prefix_rune == '?'
	) {
		match_type = BUXN_LS_MATCH_ANY_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
		filter.subroutine_only = prefix_rune == '!' || prefix_rune == '?';
		group_symbols = true;
	} else if (prefix_rune == '.' || prefix_rune == '-') {
		match_type = BUXN_LS_MATCH_ZERO_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
		group_symbols = true;
	} else if (prefix_rune == ';' || prefix_rune == '=') {
		match_type = BUXN_LS_MATCH_ANY_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
		group_symbols = true;
	} else if (prefix_rune == '/' || prefix_rune == '&') {
		match_type = BUXN_LS_MATCH_SUB_LABEL;
		format_type = BUXN_LS_FORMAT_LOCAL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
		group_symbols = false;
	} else if (prefix_rune == '|' || prefix_rune == '$') {
		match_type = BUXN_LS_MATCH_PRECEDING_LABEL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = buxn_ls_str_pop_front(ctx->prefix);
		text_edit_start = ctx->prefix_start_byte + 1;
		group_symbols = true;
	} else {
		match_type = BUXN_LS_MATCH_ANY_SYMBOL;
		format_type = BUXN_LS_FORMAT_FULL_NAME;
		filter.prefix = ctx->prefix;
		text_edit_start = ctx->prefix_start_byte;
		filter.subroutine_only = true;
		group_symbols = true;
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
		group_symbols = false;
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
				group_symbols = false;
				break;
			}
		}
	}

	buxn_ls_str_t current_scope;
	{
		// Find the most recently defined label to determine the current scope
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

		if (most_recent_label == NULL) {
			current_scope = (buxn_ls_str_t){
				.chars = "RESET",
				.len = sizeof("RESET") - 1,
			};
		} else {
			current_scope = buxn_ls_label_scope(most_recent_label->name);
		}
	}

	// Adjust filter based on match type
	switch (match_type) {
		case BUXN_LS_MATCH_ANY_SYMBOL:
			filter.labels_only = false;
			break;
		case BUXN_LS_MATCH_SUB_LABEL: {
			// When we have this scenario:
			//
			// ```
			// @parent
			// ,& <-- completion
			// &child
			// ```
			//
			// The most recent label is `parent` but it does not possess a trailing
			// slash.
			// So the safe thing to do is to just copy the scope then insert
			// a slash of or own if needed.
			char* parent_prefix = barena_memalign(ctx->arena, current_scope.len + 1, _Alignof(char));
			memcpy(parent_prefix, current_scope.chars, current_scope.len);
			parent_prefix[current_scope.len] = '/';

			filter.prefix = (buxn_ls_str_t) {
				.chars = parent_prefix,
				// Local label includes the bare parent label (e.g: parent)
				// Sub label only includes the child label (e.g: parent/child)
				// Including the slash filters out the parent
				.len = current_scope.len + 1,
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

	BIO_DEBUG("match_type = %d", match_type);
	BIO_DEBUG("format_type = %d", format_type);
	BIO_DEBUG("prefix = %.*s", (int)filter.prefix.len, filter.prefix.chars);
	BIO_DEBUG("current_scope = %.*s", (int)current_scope.len, current_scope.chars);
	BIO_DEBUG("group_symbols = %s", group_symbols ? "true" : "false");

	// Collect candidates
	bhash_clear(&completer->completion_items);
	buxn_ls_visit_symbols(
		&(buxn_ls_sym_visit_ctx_t){
			.filter = filter,
			.current_scope = current_scope,
			.completion_items = &completer->completion_items,
			.group_symbols = group_symbols,
		},
		ctx->source
	);

	// Format result
	int lsp_text_edit_start = (int)bio_lsp_utf16_offset_from_byte_offset(
		ctx->line_content.chars, ctx->line_content.len, text_edit_start
	);
	bio_lsp_range_t edit_range = {
		.start = {
			.line = ctx->line_number,
			.character = lsp_text_edit_start,
		},
		.end = ctx->lsp_range.end,
	};

	yyjson_mut_val* completion_list_obj = yyjson_mut_obj(response);
	yyjson_mut_obj_add_bool(response, completion_list_obj, "isIncomplete", false);
	yyjson_mut_val* completion_items_arr = yyjson_mut_obj_add_arr(response, completion_list_obj, "items");
	bhash_index_t num_candidates = bhash_len(&completer->completion_items);
	for (
		bhash_index_t candidate_index = 0;
		candidate_index < num_candidates;
		++candidate_index
	) {
		const buxn_ls_completion_item_t* item = &completer->completion_items.values[candidate_index];
		const buxn_ls_sym_node_t* sym = item->sym;
		buxn_ls_str_t label;
		switch (format_type) {
			case BUXN_LS_FORMAT_FULL_NAME:
				if (item->is_local) {
					buxn_ls_str_t scope = buxn_ls_label_scope(sym->name);
					label = (buxn_ls_str_t){
						.chars = sym->name.chars + scope.len,
						.len = sym->name.len - scope.len,
					};
				} else {
					if (item->size == 1) {
						// Use full name if this is the only symbol of the scope
						label = sym->name;
					} else {
						// Use scope name if there are multiple
						label = completer->completion_items.keys[candidate_index];
					}
				}
				break;
			case BUXN_LS_FORMAT_LOCAL_NAME: {
				buxn_ls_str_t scope = buxn_ls_label_scope(sym->name);
				if (scope.len < sym->name.len) {
					label = (buxn_ls_str_t){
						// Skip the '/'
						.chars = sym->name.chars + (scope.len + 1),
						.len = sym->name.len - (scope.len + 1),
					};
				} else {
					label = (buxn_ls_str_t){ 0 };
				}
			} break;
		}
		if (label.len == 0) { continue; }
		BIO_DEBUG(
			"Candidate: '%.*s' => '%.*s'<%d>",
			(int)sym->name.len, sym->name.chars,
			(int)label.len, label.chars, (int)label.len
		);

		yyjson_mut_val* item_obj = yyjson_mut_arr_add_obj(response, completion_items_arr);
		yyjson_mut_obj_add_strn(response, item_obj, "label", label.chars, label.len);
		yyjson_mut_obj_add_int(response, item_obj, "insertTextFormat", 1);  // PlainText
		yyjson_mut_obj_add_int(response, item_obj, "insertTextMode", 1);  // asIs

		if (item->size == 1) {
			// Format a lone symbol as itself
			switch (sym->semantics) {
				case BUXN_LS_SYMBOL_AS_VARIABLE:
					yyjson_mut_obj_add_int(response, item_obj, "kind", 6);  // Variable
					buxn_ls_add_detail_fmt(ctx->arena, response, item_obj, "|0x%04x", sym->address);
					break;
				case BUXN_LS_SYMBOL_AS_SUBROUTINE: {
					yyjson_mut_obj_add_int(response, item_obj, "kind", 3);  // Function
					buxn_ls_line_slice_t slice = buxn_ls_analyzer_split_file(ctx->analyzer, sym->source->filename);
					if (sym->range.start.line < slice.num_lines) {
						buxn_ls_str_t line_content = slice.lines[sym->range.start.line];
						// TODO: cache these?
						ptrdiff_t offset = bio_lsp_byte_offset_from_utf16_offset(
							line_content.chars,
							line_content.len,
							sym->range.end.character
						);
						buxn_ls_str_t signature = {
							.chars = line_content.chars + offset,
							.len = line_content.len - offset,
						};
						yyjson_mut_obj_add_strn(response, item_obj, "detail", signature.chars, signature.len);
					}
				} break;
				case BUXN_LS_SYMBOL_AS_DEVICE_PORT:
					yyjson_mut_obj_add_int(response, item_obj, "kind", 21);  // Constant
					buxn_ls_add_detail_fmt(ctx->arena, response, item_obj, "|0x%02x", sym->address);
					break;
				case BUXN_LS_SYMBOL_AS_ENUM:
					yyjson_mut_obj_add_int(response, item_obj, "kind", 20);  // Enum member
					buxn_ls_add_detail_fmt(ctx->arena, response, item_obj, "|0x%04x", sym->address);
					break;
			}
		} else {
			// Format a group as a "module"
			yyjson_mut_obj_add_int(response, item_obj, "kind", 9);  // Module
			buxn_ls_add_detail_fmt(ctx->arena, response, item_obj, "( %d symbols )", item->size);
		}
		yyjson_mut_val* text_edit = yyjson_mut_obj_add_obj(response, item_obj, "textEdit");
		{
			yyjson_mut_obj_add_strn(response, text_edit, "newText", label.chars, label.len);
			buxn_ls_serialize_lsp_range(
				response,
				yyjson_mut_obj_add_obj(response, text_edit, "range"),
				&edit_range
			);
		}
	}

	bio_yield();
	return completion_list_obj;
}
