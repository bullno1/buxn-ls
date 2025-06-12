#ifdef __linux__
#define _GNU_SOURCE
#include <sys/signalfd.h>
#include <signal.h>
#include <bio/file.h>
#endif

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
} ctrlc_ctx_t;

static void
ctrlc_handler(void* userdata) {
	ctrlc_ctx_t* ctx = userdata;

	sigset_t mask, old;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	pthread_sigmask(SIG_BLOCK, &mask, &old);

	int sigfd = signalfd(-1, &mask, SFD_CLOEXEC);
	bio_file_t sig_file;
	bio_fdopen(&sig_file, sigfd, NULL);
	struct signalfd_siginfo ssi;
	bio_fread(sig_file, &ssi, sizeof(ssi), NULL);
	BIO_INFO("Received signal: %d", ssi.ssi_signo);

	pthread_sigmask(SIG_SETMASK, &old, NULL);

	ctx->should_terminate = true;
	bio_net_close(ctx->server_sock, NULL);
	bio_fclose(sig_file, NULL);
}

static void
ls_wrapper(void* userdata) {
	args_t args = *(args_t*)userdata;
	bio_socket_t client = args.client;
	bio_raise_signal(args.ready_sig);

	bio_lsp_socket_conn_t conn;
	buxn_ls(bio_lsp_init_socket_conn(&conn, client), args.pool);
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

	ctrlc_ctx_t ctrlc = {
		.server_sock = server_sock,
	};
	(void)ctrlc_handler;
	bio_coro_t ctrlc_handler_coro = bio_spawn(ctrlc_handler, &ctrlc);

	server_ctx_t ctx = { 0 };
	bhash_init_set(&ctx.clients, bhash_config_default());

	BIO_INFO("Waiting for connection");
	while (!ctrlc.should_terminate) {
		bio_socket_t client;
		if (!bio_net_accept(server_sock, &client, &error)) {
			if (!ctrlc.should_terminate) {
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

	bio_join(ctrlc_handler_coro);
	barena_pool_cleanup(&pool);

	return 0;
}

int
buxn_ls_server(const char* socket_path) {
	return bio_enter(server_entry, (char*)socket_path);
}
