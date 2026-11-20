# reVC for Windows Phone 8.1 (32-bit ARM; x86 for the Windows 10 Mobile emulator): a build of
# its own. The Windows 10 Mobile build (w10m/CMakeLists.txt, cmake/, tools/build.py) is neither
# used nor changed by it: the two share the sources alone, and the few places those differ for
# 8.1 are marked REVC_WP81.
#
# Configure with toolchain-wp81.cmake; build_game.py does that, and stages, packs and signs.
# One program, reVC.exe, with everything linked into it: the shell (app/), the game (src/),
# librw with the Direct3D 11 renderer (rwd3d11/), GLFW's API for the port (glfw/), OpenAL Soft
# with the XAudio2 backend (openal/), mpg123, the C runtime, and what 8.1's kit lacks (wp81/).
#
# OpenAL Soft and mpg123 are the sources tools/build.py fetches and patches into
# w10m/work/src (its sources and patch steps; they build nothing), as vendor/librw is patched.
cmake_minimum_required(VERSION 3.20)
project(reVC_WP81 LANGUAGES C CXX)

if(NOT REVC_WP81)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=toolchain-wp81.cmake (or run build_game.py)")
endif()

set(WP81 "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(W10M "${WP81}/.." ABSOLUTE)
get_filename_component(REVC_ROOT "${W10M}/.." ABSOLUTE)
set(OPENAL_SOURCE_DIR "${W10M}/work/src/openal-soft" CACHE PATH "OpenAL Soft 1.24.3 sources, patched")
set(MPG123_SOURCE_DIR "${W10M}/work/src/mpg123-1.32.10" CACHE PATH "mpg123 1.32.10 sources")
set(REVC_PYTHON "python" CACHE FILEPATH "Python, for tools/make_shaders.py")
set(REVC_FXC "" CACHE FILEPATH "fxc.exe of the Windows SDK (left empty: the one tools/env.py names)")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ===[ GLFW's API for the port, and librw with the Direct3D 11 renderer ]===
# librw without its OpenGL renderer: of gl/ only what calls no OpenGL; rwd3d11/ in its place.

add_library(glfw STATIC "${W10M}/glfw/glfw_w10m.cpp")
target_include_directories(glfw PUBLIC "${W10M}/glfw/include" PRIVATE "${W10M}/app" "${W10M}/glthread" "${W10M}/rwd3d11")
target_compile_definitions(glfw PRIVATE REVC_D3D11 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_target_properties(glfw PROPERTIES CXX_STANDARD 17)

set(_librw "${REVC_ROOT}/vendor/librw/src")
if(NOT EXISTS "${_librw}/gl/rwgl3.h")
    message(FATAL_ERROR "vendor/librw is empty: git submodule update --init vendor/librw")
endif()
file(STRINGS "${_librw}/gl/rwgl3.h" _librw_d3d11 REGEX "RW_D3D11" LIMIT_COUNT 1)
if(NOT _librw_d3d11)
    message(FATAL_ERROR "vendor/librw is not patched for the Direct3D 11 renderer: python w10m/tools/build.py patch")
endif()
file(GLOB LIBRW_SOURCES CONFIGURE_DEPENDS "${_librw}/*.cpp" "${_librw}/d3d/*.cpp" "${_librw}/ps2/*.cpp")
file(GLOB RWD3D11_SOURCES CONFIGURE_DEPENDS "${W10M}/rwd3d11/*.cpp")
file(GLOB RWD3D11_HLSL CONFIGURE_DEPENDS "${W10M}/rwd3d11/shaders/*.hlsl" "${W10M}/rwd3d11/shaders/*.hlsli")

# the renderer's shaders, compiled by the Windows SDK's fxc (tools/make_shaders.py)
set(_shaders "${CMAKE_BINARY_DIR}/rwd3d11-shaders")
set(_fxc "")
if(REVC_FXC)
    set(_fxc --fxc "${REVC_FXC}")
endif()
add_custom_command(
    OUTPUT "${_shaders}/d3d11_shaders.inc"
    COMMAND "${REVC_PYTHON}" "${W10M}/tools/make_shaders.py" --out "${_shaders}" ${_fxc}
    DEPENDS ${RWD3D11_HLSL} "${W10M}/tools/make_shaders.py"
    COMMENT "Compiling the Direct3D 11 renderer's shaders"
    VERBATIM)

add_library(librw STATIC ${LIBRW_SOURCES}
    "${_librw}/gl/gl3.cpp" "${_librw}/gl/gl3matfx.cpp" "${_librw}/gl/wdgl.cpp"
    "${_librw}/lodepng/lodepng.cpp"
    ${RWD3D11_SOURCES} "${_shaders}/d3d11_shaders.inc")
set_source_files_properties("${_shaders}/d3d11_shaders.inc" PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(librw
    PUBLIC "${W10M}/rwd3d11"
    INTERFACE "${REVC_ROOT}/vendor/librw"
    PRIVATE "${_librw}" "${_librw}/gl" "${W10M}/app" "${W10M}/glthread" "${_shaders}")
target_compile_definitions(librw PUBLIC RW_GL3 RW_D3D11 PRIVATE LODEPNG_NO_COMPILE_CPP _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(librw PRIVATE /W2 /wd4996 /wd4244 /wd4267 /wd4305 /wd4838)
target_link_libraries(librw PUBLIC glfw d3d11.lib dxgi.lib dxguid.lib)

# ===[ mpg123, a static library ]===
# The release's own sources (not ports/cmake: its configure step decides by linking test
# programs). Linked into the program with the one C runtime; LGPL: the program's source and this
# build are here to link it again with another build of it.

if(NOT EXISTS "${MPG123_SOURCE_DIR}/src/libmpg123/libmpg123.c")
    message(FATAL_ERROR "mpg123 sources not found in ${MPG123_SOURCE_DIR}: python w10m/tools/build.py sources")
endif()
set(_mpg "${MPG123_SOURCE_DIR}/src")
set(_mpglib "${_mpg}/libmpg123")
add_library(mpg123 STATIC
    "${_mpglib}/parse.c" "${_mpglib}/frame.c" "${_mpglib}/format.c" "${_mpglib}/dct64.c" "${_mpglib}/equalizer.c"
    "${_mpglib}/id3.c" "${_mpglib}/optimize.c" "${_mpglib}/readers.c" "${_mpglib}/tabinit.c"
    "${_mpglib}/libmpg123.c" "${_mpglib}/index.c" "${_mpglib}/icy.c" "${_mpglib}/icy2utf8.c"
    "${_mpglib}/layer1.c" "${_mpglib}/layer2.c" "${_mpglib}/layer3.c" "${_mpglib}/ntom.c"
    "${_mpglib}/synth_8bit.c" "${_mpglib}/synth.c" "${_mpglib}/synth_s32.c" "${_mpglib}/synth_real.c"
    "${_mpglib}/stringbuf.c" "${_mpglib}/feature.c" "${_mpglib}/lfs_wrap.c"
    "${_mpg}/compat/compat.c" "${_mpg}/compat/compat_str.c")
# config.h and the rest of what the release's configure would write: the port's own (deps/mpg123)
target_include_directories(mpg123
    PRIVATE "${W10M}/deps/mpg123" "${_mpg}" "${_mpg}/compat" "${_mpglib}"
    PUBLIC "${_mpg}/include")
target_compile_definitions(mpg123 PRIVATE OPT_GENERIC REAL_IS_FLOAT _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(mpg123 PRIVATE /W2 /wd4244 /wd4267 /wd4018 /wd4146 /wd4305 /wd4996)

# ===[ OpenAL Soft, a static library, with the XAudio2 backend ]===

if(NOT EXISTS "${OPENAL_SOURCE_DIR}/alc/alc.cpp")
    message(FATAL_ERROR "OpenAL Soft sources not found in ${OPENAL_SOURCE_DIR}: python w10m/tools/build.py sources patch")
endif()
file(STRINGS "${OPENAL_SOURCE_DIR}/alc/alc.cpp" _openal_xaudio2 REGEX "XAudio2BackendFactory" LIMIT_COUNT 1)
if(NOT _openal_xaudio2)
    message(FATAL_ERROR "${OPENAL_SOURCE_DIR} is not patched: python w10m/tools/build.py patch")
endif()

# Its UWP flavour (the configuration in the app's roaming folder, no shell folders or registry),
# without a Windows Store toolchain: the names are set here instead of being found.
set(ALSOFT_UWP TRUE)
add_library(Microsoft::CppWinRT INTERFACE IMPORTED)  # C++/WinRT is in the Windows SDK's headers
set(LIBTYPE "STATIC" CACHE STRING "" FORCE)
foreach(_off IN ITEMS
        ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_INSTALL_CONFIG ALSOFT_INSTALL_HRTF_DATA
        ALSOFT_INSTALL_AMBDEC_PRESETS ALSOFT_INSTALL_EXAMPLES ALSOFT_INSTALL_UTILS ALSOFT_UPDATE_BUILD_VERSION
        ALSOFT_EAX ALSOFT_DLOPEN ALSOFT_EMBED_HRTF_DATA
        # XAudio2 plays (openal/xaudio2_backend.cpp), as on Windows 10 Mobile
        ALSOFT_BACKEND_WASAPI ALSOFT_BACKEND_WAVE ALSOFT_BACKEND_SDL2 ALSOFT_BACKEND_SDL3
        ALSOFT_BACKEND_PORTAUDIO ALSOFT_BACKEND_JACK ALSOFT_BACKEND_PIPEWIRE ALSOFT_BACKEND_PULSEAUDIO
        ALSOFT_BACKEND_OPENSL ALSOFT_BACKEND_OBOE ALSOFT_BACKEND_COREAUDIO)
    set(${_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
# What its configure step finds out by linking a test program: the toolchain does not link them
# (CMAKE_TRY_COMPILE_TARGET_TYPE), so each such test "passes". None of these exists here...
foreach(_absent IN ITEMS HAVE_LIBATOMIC HAVE_LIBM HAVE_LIBRT HAVE_LIBDL HAVE_CPUID_INTRINSIC HAVE_GCC_GET_CPUID)
    set(${_absent} FALSE CACHE INTERNAL "")
endforeach()
# ...but for the emulator's x86 MSVC's __cpuid, which tells SSE's levels (without it OpenAL Soft
# assumes them with #warning, which this compiler stops at)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "X86")
    set(HAVE_CPUID_INTRINSIC TRUE CACHE INTERNAL "")
endif()
add_subdirectory("${OPENAL_SOURCE_DIR}" openal EXCLUDE_FROM_ALL)

# Its FFT has NEON code only for compilers that define __ARM_NEON, and says so with #warning,
# which this compiler does not have in C++17. The FFT is for effects the game does not use. The
# static library compiles the common sources itself; and in one program with the shell its
# C++/WinRT coroutines (alc/alconfig.cpp) must be C++20's, as the shell's are.
target_compile_definitions(alsoft.common PRIVATE PFFFT_SIMD_DISABLE)
target_compile_definitions(OpenAL PRIVATE PFFFT_SIMD_DISABLE)
set_target_properties(OpenAL PROPERTIES CXX_STANDARD 20)

target_sources(OpenAL PRIVATE "${W10M}/openal/xaudio2_backend.cpp" "${W10M}/openal/xaudio2_backend.h")
target_include_directories(OpenAL PRIVATE "${W10M}/openal" "${OPENAL_SOURCE_DIR}/alc")
target_compile_definitions(OpenAL PRIVATE HAVE_XAUDIO2=1)

# ===[ The game ]===

list(APPEND CMAKE_MODULE_PATH "${REVC_ROOT}/cmake")
include(GetGitRevisionDescription)
get_git_head_revision(GIT_REFSPEC GIT_SHA1 "ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR")
configure_file("${REVC_ROOT}/src/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp" @ONLY)

file(GLOB_RECURSE REVC_SOURCES CONFIGURE_DEPENDS "${REVC_ROOT}/src/*.cpp")
file(GLOB_RECURSE _revc_headers "${REVC_ROOT}/src/*.h")
set(REVC_INCLUDES "")
foreach(_header IN LISTS _revc_headers)
    get_filename_component(_dir "${_header}" DIRECTORY)
    list(APPEND REVC_INCLUDES "${_dir}")
endforeach()
list(REMOVE_DUPLICATES REVC_INCLUDES)

# bench/: what a draw costs (REVC_BENCH=1); its D3DCompile is wp81/d3dcompile.cpp's
add_library(revc_game STATIC ${REVC_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp" "${W10M}/bench/d3dbench.cpp")
target_include_directories(revc_game PRIVATE "${REVC_ROOT}" ${REVC_INCLUDES} "${W10M}/app")
target_compile_definitions(revc_game PRIVATE
    GTA_W10M LIBRW AUDIO_OAL CMAKE_BUILD CMAKE_NO_AUTOLINK USE_OUR_VERSIONING
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(revc_game PRIVATE /Zc:sizedDealloc- /w /bigobj)
target_link_libraries(revc_game PUBLIC librw OpenAL::OpenAL mpg123 d3d11.lib)

# ===[ The shell and the program ]===

add_executable(reVC WIN32
    "${W10M}/app/App.cpp"
    "${W10M}/app/pch.h" "${W10M}/app/host_api.h" "${W10M}/app/TouchPad.h" "${W10M}/app/GameFiles.h" "${W10M}/app/Gamepad.h"
    "${WP81}/stubs.cpp" "${WP81}/stubs_game.cpp" "${WP81}/d3dcompile.cpp" "${WP81}/BackKey81.h")
target_include_directories(reVC PRIVATE "${W10M}/app" "${WP81}")
target_compile_definitions(reVC PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS REVC_D3D11)
target_compile_options(reVC PRIVATE /GR- /bigobj /permissive-)
set_target_properties(reVC PROPERTIES CXX_STANDARD 20)
target_link_libraries(reVC PRIVATE revc_game)
# the subsystem's version is the linker's own (6.02): 8.1 loads nothing marked 10.0
target_link_options(reVC PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup "/MAP:${CMAKE_BINARY_DIR}/bin/reVC.map")
