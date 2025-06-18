#include "common.h"
#include <stdlib.h>
#include <bio/logging/file.h>

typedef struct {
	void* userdata;
	bio_entry_fn_t entry;
	int exit_code;
} bio_entry_data_t;

static void
bio_entry_wrapper(void* userdata) {
	bio_set_coro_name("main");
	bio_entry_data_t* entry_data = userdata;

	bio_add_file_logger(
		BIO_LOG_LEVEL_TRACE,
		&(bio_file_logger_options_t){
			.file = BIO_STDERR,
			.with_colors = true,
		}
	);
	entry_data->exit_code = entry_data->entry(entry_data->userdata);
}

// TODO: Switch allocator on release
void*
buxn_ls_realloc(void* ptr, size_t size) {
	if (size == 0) {
		free(ptr);
		return NULL;
	} else {
		return realloc(ptr, size);
	}
}

static void*
buxn_ls_realloc_wrapper(void* ptr, size_t size, void* ctx) {
	(void)ctx;
	return buxn_ls_realloc(ptr, size);
}

int
bio_enter(bio_entry_fn_t entry, void* userdata) {
	bio_init(&(bio_options_t){
		.allocator.realloc = buxn_ls_realloc_wrapper,
		.log_options = {
			.current_filename = __FILE__,
			.current_depth_in_project = 1,
		},
	});

	bio_entry_data_t entry_data = {
		.entry = entry,
		.userdata = userdata,
	};
	bio_spawn(bio_entry_wrapper, &entry_data);

	bio_loop();

	bio_terminate();

	return entry_data.exit_code;
}

buxn_ls_line_slice_t
buxn_ls_split_file(buxn_ls_str_t content, barray(buxn_ls_str_t)* lines) {
	buxn_ls_str_t line = { .chars = content.chars, };
	int first_line_index = (int)barray_len(*lines);
	int num_lines = 0;
	size_t start_index = 0;
	for (size_t char_index = 0; char_index < content.len; ++char_index) {
		char ch = content.chars[char_index];
		if (ch == '\n') {
			line.len = char_index - start_index;
			barray_push(*lines, line, NULL);
			++num_lines;
			line.chars = content.chars + char_index + 1;
			start_index = char_index + 1;
		} else if (ch == '\r') {
			if (char_index < content.len - 1 && content.chars[char_index + 1] == '\n') {
				line.len = char_index - start_index;
				barray_push(*lines, line, NULL);
				++num_lines;
				line.chars = content.chars + char_index + 2;
				start_index = char_index + 2;
			} else {
				line.len = char_index - start_index;
				barray_push(*lines, line, NULL);
				++num_lines;
				line.chars = content.chars + char_index + 1;
				start_index = char_index + 1;
			}
		}
	}
	// Last line
	if (start_index < content.len) {
		line.len = content.len - start_index;
		barray_push(*lines, line, NULL);
		++num_lines;
	}

	return (buxn_ls_line_slice_t){
		.lines = &(*lines)[first_line_index],
		.num_lines = num_lines,
	};
}
