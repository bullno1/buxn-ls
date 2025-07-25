# buxn
add_library(buxn INTERFACE)
target_include_directories(buxn INTERFACE "buxn/include")

add_library(buxn-asm STATIC "buxn/src/asm/asm.c")
target_link_libraries(buxn-asm PUBLIC buxn)

add_library(buxn-asm-annotation STATIC "buxn/src/asm/annotation.c")
target_link_libraries(buxn-asm-annotation PUBLIC buxn)

add_library(buxn-asm-chess STATIC "buxn/src/asm/chess.c")
target_link_libraries(buxn-asm-chess PUBLIC buxn blibs)

# bio
include(bio/deps/CMakeLists.txt)
set(BUILD_DEPS OFF CACHE BOOL "bio should not build deps")
add_subdirectory(bio EXCLUDE_FROM_ALL)

# yyjson
set(YYJSON_DISABLE_INCR_READER OFF CACHE BOOL "")
set(YYJSON_DISABLE_UTILS OFF CACHE BOOL "")
set(YYJSON_DISABLE_FAST_FP_CONV OFF CACHE BOOL "")
set(YYJSON_DISABLE_NON_STANDARD OFF CACHE BOOL "")
set(YYJSON_DISABLE_UTF8_VALIDATION OFF CACHE BOOL "")
set(YYJSON_DISABLE_UNALIGNED_MEMORY_ACCESS OFF CACHE BOOL "")
add_subdirectory(yyjson EXCLUDE_FROM_ALL)

# libyuarel
add_library(libyuarel STATIC "libyuarel/yuarel.c")
target_include_directories(libyuarel PUBLIC "libyuarel")

# utf8proc
add_library(utf8proc STATIC "utf8proc/utf8proc.c")
target_include_directories(utf8proc PUBLIC "utf8proc")
target_compile_definitions(utf8proc PUBLIC "UTF8PROC_STATIC")
