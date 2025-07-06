#include <barg.h>
#include <string.h>
#include "ls.h"
#include "lsp.h"
#include "common.h"

typedef enum {
	BUXN_LS_STDIO,
	BUXN_LS_SERVER,
	BUXN_LS_SHIM,
	BUXN_LS_HYBRID,
} launch_mode_t;

extern int
buxn_ls_server(const char* socket_path);

extern int
buxn_ls_shim(const char* socket_path, bool fallback);

static inline const char*
parse_mode(void* userdata, const char* str) {
	launch_mode_t* mode = userdata;
	if (strcmp(str, "stdio") == 0) {
		*mode = BUXN_LS_STDIO;
		return NULL;
	} else if (strcmp(str, "server") == 0) {
		*mode = BUXN_LS_SERVER;
		return NULL;
	} else if (strcmp(str, "shim") == 0) {
		*mode = BUXN_LS_SHIM;
		return NULL;
	} else if (strcmp(str, "hybrid") == 0) {
		*mode = BUXN_LS_HYBRID;
		return NULL;
	} else {
		return "Invalid mode";
	}
}

int
main(int argc, const char* argv[]) {
	launch_mode_t mode = BUXN_LS_STDIO;
	const char* socket_path = "@buxn/ls";
	barg_opt_t opts[] = {
		{
			.name = "mode",
			.value_name = "mode",
			.parser = {
				.parse = parse_mode,
				.userdata = &mode,
			},
			.summary = "The mode to start in",
			.description =
				"Default value: stdio\n"
				"Available modes:\n\n"
				"* stdio: Communicate through stdin and stdout\n"
				"* server: Listens for incoming connection\n"
				"* shim: Connect to a server and forward stdio to that server\n"
				"* hybrid: Same as shim but fallback to stdio if the connection failed\n",
		},
		{
			.name = "socket",
			.value_name = "path",
			.parser = barg_str(&socket_path),
			.summary = "The socket to connect or listen to",
			.description =
				"Default value: @buxn/ls\n"
				"This is only valid for server or shim mode"
		},
		barg_opt_help(),
	};
	barg_t barg = {
		.usage = "buxn-ls server [options]",
		.summary = "Start the language server",
		.opts = opts,
		.num_opts = sizeof(opts) / sizeof(opts[0]),
	};

	barg_result_t result = barg_parse(&barg, argc, argv);
	if (result.status != BARG_OK) {
		barg_print_result(&barg, result, stderr);
		return result.status == BARG_PARSE_ERROR;
	}

	switch (mode) {
		case BUXN_LS_STDIO:
			return bio_enter(buxn_ls_stdio, NULL);
		case BUXN_LS_SERVER:
			return buxn_ls_server(socket_path);
		case BUXN_LS_SHIM:
			return buxn_ls_shim(socket_path, false);
		case BUXN_LS_HYBRID:
			return buxn_ls_shim(socket_path, true);
	}
}

#define XINCBIN_IMPLEMENTATION
#include "resources.h"
