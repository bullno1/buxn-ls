# buxn
add_library(buxn INTERFACE)
target_include_directories(buxn INTERFACE "buxn/include")

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
