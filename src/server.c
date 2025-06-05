#include "common.h"
#include <string.h>
#include <bio/net.h>
#include "ls.h"
#include "lsp.h"

typedef struct {
	bio_socket_t client;
	bio_signal_t ready_sig;
} args_t;

static void
ls_wrapper(void* userdata) {
	args_t* args = userdata;
	bio_socket_t client = args->client;
	bio_raise_signal(args->ready_sig);

	bio_lsp_socket_conn_t conn;
	buxn_ls(bio_lsp_init_socket_conn(&conn, client));
	bio_net_close(client, NULL);
}

static int
server_entry(void* userdata) {
	const char* socket_path = userdata;

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

	while (true) {
		bio_socket_t client;
		if (!bio_net_accept(server_sock, &client, &error)) {
			BIO_ERROR(
				"Could not accept connection: " BIO_ERROR_FMT,
				BIO_ERROR_FMT_ARGS(&error)
			);
			break;
		}

		args_t args = {
			.client = client,
			.ready_sig = bio_make_signal(),
		};
		BIO_DEBUG("New client connected, spawning wrapper");
		bio_spawn(ls_wrapper, &args);
		bio_wait_for_one_signal(args.ready_sig);
	}

	bio_net_close(server_sock, NULL);
	return 0;
}

int
buxn_ls_server(const char* socket_path) {
	return bio_enter(server_entry, (char*)socket_path);
}
