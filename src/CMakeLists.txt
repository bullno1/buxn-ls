set(SOURCES
	"main.c"
	"common.c"
	"server.c"
	"shim.c"
	"ls.c"
	"lsp.c"
	"libs.c"
)
add_executable(buxn-ls ${SOURCES})
target_link_libraries(
	buxn-ls
	PRIVATE
	bio
	blibs
	yyjson
)
