#ifndef BUXN_LS_WORKSPACE_H
#define BUXN_LS_WORKSPACE_H

#include <bhash.h>
#include "common.h"

struct bio_lsp_in_msg_s;

typedef struct buxn_ls_workspace_s {
	char* root_dir;
	size_t root_dir_len;
	BHASH_TABLE(char*, buxn_ls_str_t) docs;
} buxn_ls_workspace_t;

void
buxn_ls_workspace_init(buxn_ls_workspace_t* workspace, const char* root_dir);

void
buxn_ls_workspace_cleanup(buxn_ls_workspace_t* workspace);

void
buxn_ls_workspace_update(buxn_ls_workspace_t* workspace, const struct bio_lsp_in_msg_s* msg);

char*
buxn_ls_workspace_resolve_path(buxn_ls_workspace_t* workspace, char* uri);

#endif
