#include "common.h"
#include <string.h>
#include <bio/net.h>
#include <barena.h>
#include <bhash.h>
#include "ls.h"
#include "lsp.h"

typedef struct {
	BHASH_SET(bio_coro_t) clients;
} server_ctx_t;

typedef struct {
	bio_socket_t client;
	bio_signal_t ready_sig;
	barena_pool_t* pool;
	server_ctx_t* server_ctx;
} args_t;

typedef struct {
	bio_socket_t server_sock;
	bool should_terminate;
} exit_ctx_t;

static void
exit_handler(void* userdata) {
	exit_ctx_t* ctx = userdata;
	bio_wait_for_exit();
	ctx->should_terminate = true;
	bio_net_close(ctx->server_sock, NULL);
}

static void
ls_wrapper(void* userdata) {
	args_t args = *(args_t*)userdata;
	bio_socket_t client = args.client;
	bio_raise_signal(args.ready_sig);

	bio_io_buffer_t in_buf = bio_make_socket_read_buffer(client, BUXN_LS_IO_BUF_SIZE);
	bio_io_buffer_t out_buf = bio_make_socket_write_buffer(client, BUXN_LS_IO_BUF_SIZE);

	buxn_ls(in_buf, out_buf, args.pool);

	bio_destroy_buffer(in_buf);
	bio_destroy_buffer(out_buf);

	bio_net_close(client, NULL);
	bio_coro_t self = bio_current_coro();
	bhash_remove(&args.server_ctx->clients, self);
}

static int
server_entry(void* userdata) {
	const char* socket_path = userdata;

	barena_pool_t pool;
	barena_pool_init(&pool, 1);

	bio_socket_t server_sock;
	bio_error_t error = { 0 };
	bio_addr_t addr = { .type = BIO_ADDR_NAMED };
	addr.named.len = strlen(socket_path);
	memcpy(addr.named.name, socket_path, addr.named.len);
	if (!bio_net_listen(
		BIO_SOCKET_STREAM,
		&addr, BIO_PORT_ANY,
		&server_sock,
		&error
	)) {
		BIO_ERROR(
			"Could not listen to %s: " BIO_ERROR_FMT,
			socket_path, BIO_ERROR_FMT_ARGS(&error)
		);
		return 1;
	}

	exit_ctx_t exit_ctx = { .server_sock = server_sock };
	bio_coro_t exit_handler_coro = bio_spawn(exit_handler, &exit_ctx);

	server_ctx_t ctx = { 0 };
	bhash_init_set(&ctx.clients, bhash_config_default());

	BIO_INFO("Waiting for connection");
	while (!exit_ctx.should_terminate) {
		bio_socket_t client;
		if (!bio_net_accept(server_sock, &client, &error)) {
			if (!exit_ctx.should_terminate) {
				BIO_ERROR(
					"Could not accept connection: " BIO_ERROR_FMT,
					BIO_ERROR_FMT_ARGS(&error)
				);
			}
			BIO_INFO("Shutting down");
			break;
		}

		args_t args = {
			.client = client,
			.ready_sig = bio_make_signal(),
			.pool = &pool,
			.server_ctx = &ctx,
		};
		BIO_INFO("New client connected, spawning wrapper");
		bio_coro_t client_coro = bio_spawn(ls_wrapper, &args);
		bhash_put_key(&ctx.clients, client_coro);
		bio_wait_for_one_signal(args.ready_sig);
	}

	bio_net_close(server_sock, NULL);

	bhash_index_t num_clients;
	while ((num_clients = bhash_len(&ctx.clients)) > 0) {
		BIO_INFO("Waiting for %d client(s) to shutdown", num_clients);
		bio_join(ctx.clients.keys[0]);
	}
	bhash_cleanup(&ctx.clients);

	bio_join(exit_handler_coro);
	barena_pool_cleanup(&pool);

	return 0;
}

int
buxn_ls_server(const char* socket_path) {
	return bio_enter(server_entry, (char*)socket_path);
}
