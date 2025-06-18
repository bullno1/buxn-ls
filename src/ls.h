#ifndef BUXN_LS_H
#define BUXN_LS_H

#include <bio/buffering.h>

#define BUXN_LS_IO_BUF_SIZE 16384

struct barena_pool_s;

int
buxn_ls(
	bio_io_buffer_t in_buf,
	bio_io_buffer_t out_buf,
	struct barena_pool_s* pool
);

int
buxn_ls_stdio(void* userdata);

#endif
