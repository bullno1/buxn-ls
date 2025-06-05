#ifndef BUXN_LS_H
#define BUXN_LS_H

struct bio_lsp_conn_s;

int
buxn_ls(struct bio_lsp_conn_s* conn);

int
buxn_ls_stdio(void* userdata);

#endif
