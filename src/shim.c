#include "common.h"
#include <bio/net.h>
#include <bio/file.h>
#include <string.h>

#define BUF_SIZE 1024

static void
shim_stdin(void* userdata) {
	bio_set_coro_name("stdin");
	bio_socket_t sock = *(bio_socket_t*)userdata;
	char buf[BUF_SIZE];

	bio_error_t error = { 0 };
	while (true) {
		size_t bytes_read = bio_fread(BIO_STDIN, buf, sizeof(buf), &error);
		if (bytes_read == 0) {
			BIO_ERROR("Error while reading: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			break;
		}

		if (bio_net_send_exactly(sock, buf, bytes_read, &error) != bytes_read) {
			BIO_ERROR("Error while forwarding: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			break;
		}
	}
}

static void
shim_stdout(void* userdata) {
	bio_set_coro_name("stdout");
	bio_socket_t sock = *(bio_socket_t*)userdata;
	char buf[BUF_SIZE];

	bio_error_t error = { 0 };
	while (true) {
		size_t bytes_recv = bio_net_recv(sock, buf, sizeof(buf), NULL);
		if (bytes_recv == 0) {
			BIO_ERROR("Error while receiving: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			break;
		}

		if (bio_fwrite_exactly(BIO_STDOUT, buf, bytes_recv, NULL) != bytes_recv) {
			BIO_ERROR("Error while forwarding: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
			break;
		}
	}
}

static int
shim_entry(void* userdata) {
	const char* socket_path = userdata;

	bio_socket_t sock;
	bio_addr_t addr = { .type = BIO_ADDR_NAMED };
	addr.named.len = strlen(socket_path);
	memcpy(addr.named.name, socket_path, addr.named.len);
	bio_error_t error = { 0 };
	if (!bio_net_connect(
		BIO_SOCKET_STREAM,
		&addr, BIO_PORT_ANY,
		&sock,
		&error
	)) {
		BIO_ERROR("Could not connect to server: " BIO_ERROR_FMT, BIO_ERROR_FMT_ARGS(&error));
		return 1;
	}

	bio_coro_t stdin_handler = bio_spawn(shim_stdin, &sock);
	bio_coro_t stdout_handler = bio_spawn(shim_stdout, &sock);
	bio_signal_t exit_sig = bio_make_signal();
	bio_monitor(stdin_handler, exit_sig);
	bio_monitor(stdout_handler, exit_sig);
	bio_wait_for_one_signal(exit_sig);

	bio_net_close(sock, NULL);
	return 0;
}

int
buxn_ls_shim(const char* socket_path) {
	return bio_enter(shim_entry, (char*)socket_path);
}
