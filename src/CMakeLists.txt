set(SOURCES
	"main.c"
	"common.c"
	"server.c"
	"shim.c"
	"ls.c"
	"lsp.c"
	"analyze.c"
	"completion.c"
	"workspace.c"
	"libs.c"
)

if (WIN32)
	set(SOURCES ${SOURCES} "resources.rc")
endif ()
add_executable(buxn-ls ${SOURCES})

target_link_libraries(
	buxn-ls
	PRIVATE
	bio
	blibs
	yyjson
	libyuarel
	buxn-asm
	buxn-asm-annotation
	buxn-asm-chess
	utf8proc
)

if (LINUX)
	target_link_options(buxn-ls PRIVATE $<$<CONFIG:RelWithDebInfo>:-static>)
elseif (BSD)
	target_link_options(buxn-ls PRIVATE $<$<CONFIG:RelWithDebInfo>:-static> $<$<CONFIG:RelWithDebInfo>:-pthread>)
elseif (WIN32)
	set_property(TARGET buxn-ls PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()
