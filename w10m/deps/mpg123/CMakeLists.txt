# libmpg123 as a DLL (LGPL: a library of its own, replaceable in the package), from the
# release tarball in MPG123_SOURCE_DIR. Not ports/cmake: its configure step decides by linking
# test programs, and it turns on shlwapi path handling for every Windows build.
if(NOT EXISTS "${MPG123_SOURCE_DIR}/src/libmpg123/libmpg123.c")
    message(FATAL_ERROR "mpg123 sources not found in ${MPG123_SOURCE_DIR} (tools/build.py fetches them)")
endif()

set(_src "${MPG123_SOURCE_DIR}/src")
set(_lib "${_src}/libmpg123")
add_library(mpg123 SHARED
    "${_lib}/parse.c" "${_lib}/frame.c" "${_lib}/format.c" "${_lib}/dct64.c" "${_lib}/equalizer.c"
    "${_lib}/id3.c" "${_lib}/optimize.c" "${_lib}/readers.c" "${_lib}/tabinit.c"
    "${_lib}/libmpg123.c" "${_lib}/index.c" "${_lib}/icy.c" "${_lib}/icy2utf8.c"
    "${_lib}/layer1.c" "${_lib}/layer2.c" "${_lib}/layer3.c" "${_lib}/ntom.c"
    "${_lib}/synth_8bit.c" "${_lib}/synth.c" "${_lib}/synth_s32.c" "${_lib}/synth_real.c"
    "${_lib}/stringbuf.c" "${_lib}/feature.c" "${_lib}/lfs_wrap.c"
    "${_src}/compat/compat.c" "${_src}/compat/compat_str.c")
target_include_directories(mpg123
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_src}" "${_src}/compat" "${_lib}"
    PUBLIC "${_src}/include")
target_compile_definitions(mpg123
    PRIVATE BUILD_MPG123_DLL OPT_GENERIC REAL_IS_FLOAT _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
    INTERFACE LINK_MPG123_DLL)
target_compile_options(mpg123 PRIVATE /W2 /wd4244 /wd4267 /wd4018 /wd4146 /wd4305 /wd4996)
set_target_properties(mpg123 PROPERTIES OUTPUT_NAME "libmpg123" PREFIX "")
add_library(MPG123::libmpg123 ALIAS mpg123)
