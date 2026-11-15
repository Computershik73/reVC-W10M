# reVC for Windows 10 Mobile: the game, its libraries and the UWP shell, for 32-bit ARM.
#
# Configure with cmake/toolchain-arm-msvc.cmake and Ninja; tools/build.py does that, and then
# stages, packs and signs the package. What is built:
#
#   reVC.exe       the UWP shell (app/), the game (../src), librw (../vendor/librw) and
#                  GLFW's API over ANGLE and the shell (glfw/)
#   OpenAL32.dll   OpenAL Soft with an XAudio2 backend (openal/)
#   libmpg123.dll  the MP3 decoder
#
# ANGLE (libEGL.dll, libGLESv2.dll) is not built here: butterscotch-w10m builds it from
# gecko-w10m's sources (tools/build_angle.py there), and its binaries are linked and packaged.
cmake_minimum_required(VERSION 3.20)
project(reVC_W10M LANGUAGES C CXX)

if(NOT REVC_W10M)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm-msvc.cmake (or run tools/build.py)")
endif()

get_filename_component(REVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(W10M_SOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/src" CACHE PATH "Where tools/build.py unpacks openal-soft and mpg123")
set(OPENAL_SOURCE_DIR "${W10M_SOURCES_DIR}/openal-soft" CACHE PATH "OpenAL Soft 1.24.3 sources")
set(MPG123_SOURCE_DIR "${W10M_SOURCES_DIR}/mpg123-1.32.10" CACHE PATH "mpg123 1.32.10 sources")
set(ANGLE_INCLUDE_DIR "" CACHE PATH "ANGLE's include directory (EGL/egl.h)")
set(ANGLE_LIB_DIR "" CACHE PATH "Directory with ANGLE's libEGL.lib and libGLESv2.lib for ARM")

if(NOT EXISTS "${ANGLE_INCLUDE_DIR}/EGL/egl.h")
    message(FATAL_ERROR "ANGLE_INCLUDE_DIR (${ANGLE_INCLUDE_DIR}) has no EGL/egl.h")
endif()
if(NOT EXISTS "${ANGLE_LIB_DIR}/libEGL.lib" OR NOT EXISTS "${ANGLE_LIB_DIR}/libGLESv2.lib")
    message(FATAL_ERROR "ANGLE_LIB_DIR (${ANGLE_LIB_DIR}) has no libEGL.lib and libGLESv2.lib")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ===[ ANGLE ]===

add_library(angle INTERFACE)
target_include_directories(angle INTERFACE "${ANGLE_INCLUDE_DIR}")
target_link_libraries(angle INTERFACE "${ANGLE_LIB_DIR}/libEGL.lib" "${ANGLE_LIB_DIR}/libGLESv2.lib")

# ===[ GLFW's API over ANGLE and the shell ]===

add_library(glfw STATIC glfw/glfw_w10m.cpp)
target_include_directories(glfw PUBLIC glfw/include PRIVATE app glthread)
target_link_libraries(glfw PUBLIC angle)
target_compile_definitions(glfw PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_target_properties(glfw PROPERTIES CXX_STANDARD 17)

# ===[ librw ]===
# Not its own CMakeLists: on Windows that insists on desktop OpenGL (opengl32).

if(NOT EXISTS "${REVC_ROOT}/vendor/librw/src/gl/gl3device.cpp")
    message(FATAL_ERROR "vendor/librw is empty: git submodule update --init vendor/librw")
endif()
file(STRINGS "${REVC_ROOT}/vendor/librw/src/gl/gl3device.cpp" _librw_es30 REGEX "shaderDecl300es" LIMIT_COUNT 1)
if(NOT _librw_es30)
    message(FATAL_ERROR "vendor/librw is not patched for OpenGL ES 3.0: git -C vendor/librw apply ../../w10m/patches/librw/0001-gl3-OpenGL-ES-3.0-and-ANGLE.patch (tools/build.py does it)")
endif()

set(_librw "${REVC_ROOT}/vendor/librw/src")
file(GLOB LIBRW_SOURCES CONFIGURE_DEPENDS
    "${_librw}/*.cpp" "${_librw}/d3d/*.cpp" "${_librw}/gl/*.cpp" "${_librw}/ps2/*.cpp")
# glthread: OpenGL ES on a thread of its own. It lives on glad's table of pointers, which is
# librw's, and the GLFW layer starts it.
add_library(librw STATIC ${LIBRW_SOURCES} "${_librw}/gl/glad/glad.c" "${_librw}/lodepng/lodepng.cpp"
    glthread/glthread.cpp)
set_source_files_properties(glthread/glthread.cpp PROPERTIES INCLUDE_DIRECTORIES "${_librw}/gl")
target_include_directories(librw INTERFACE "${REVC_ROOT}/vendor/librw")
target_compile_definitions(librw PUBLIC RW_GL3 PRIVATE LODEPNG_NO_COMPILE_CPP _CRT_SECURE_NO_WARNINGS)
target_compile_options(librw PRIVATE /W2 /wd4996 /wd4244 /wd4267 /wd4305 /wd4838)
target_link_libraries(librw PUBLIC glfw)
set_target_properties(librw PROPERTIES PREFIX "")
add_library(librw::librw ALIAS librw)

# ===[ mpg123 ]===

add_subdirectory(deps/mpg123 mpg123)

# ===[ OpenAL Soft ]===

if(NOT EXISTS "${OPENAL_SOURCE_DIR}/alc/alc.cpp")
    message(FATAL_ERROR "OpenAL Soft sources not found in ${OPENAL_SOURCE_DIR} (tools/build.py fetches them)")
endif()
file(STRINGS "${OPENAL_SOURCE_DIR}/alc/alc.cpp" _openal_xaudio2 REGEX "XAudio2BackendFactory" LIMIT_COUNT 1)
if(NOT _openal_xaudio2)
    message(FATAL_ERROR "${OPENAL_SOURCE_DIR} is not patched: git apply patches/openal-soft/*.patch there (tools/build.py does it)")
endif()

# Its UWP flavour (the configuration in the app's roaming folder, no shell folders or registry),
# without a Windows Store toolchain: the names are set here instead of being found.
set(ALSOFT_UWP TRUE)
add_library(Microsoft::CppWinRT INTERFACE IMPORTED)  # C++/WinRT is in the Windows SDK's headers
set(LIBTYPE "SHARED" CACHE STRING "" FORCE)
foreach(_off IN ITEMS
        ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_INSTALL_CONFIG ALSOFT_INSTALL_HRTF_DATA
        ALSOFT_INSTALL_AMBDEC_PRESETS ALSOFT_INSTALL_EXAMPLES ALSOFT_INSTALL_UTILS ALSOFT_UPDATE_BUILD_VERSION
        ALSOFT_EAX ALSOFT_DLOPEN ALSOFT_EMBED_HRTF_DATA
        # ActivateAudioInterfaceAsync from a thread that is not the UI thread faults in
        # MMDevAPI.dll on 15063 phones: XAudio2 plays instead (openal/xaudio2_backend.cpp)
        ALSOFT_BACKEND_WASAPI ALSOFT_BACKEND_WAVE ALSOFT_BACKEND_SDL2 ALSOFT_BACKEND_SDL3
        ALSOFT_BACKEND_PORTAUDIO ALSOFT_BACKEND_JACK ALSOFT_BACKEND_PIPEWIRE ALSOFT_BACKEND_PULSEAUDIO
        ALSOFT_BACKEND_OPENSL ALSOFT_BACKEND_OBOE ALSOFT_BACKEND_COREAUDIO)
    set(${_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
# What its configure step finds out by linking a test program: this toolchain does not link
# them (CMAKE_TRY_COMPILE_TARGET_TYPE), so each such test "passes". None of these exists here.
foreach(_absent IN ITEMS HAVE_LIBATOMIC HAVE_LIBM HAVE_LIBRT HAVE_LIBDL HAVE_CPUID_INTRINSIC HAVE_GCC_GET_CPUID)
    set(${_absent} FALSE CACHE INTERNAL "")
endforeach()
add_subdirectory("${OPENAL_SOURCE_DIR}" openal EXCLUDE_FROM_ALL)

# Its FFT has NEON code only for compilers that define __ARM_NEON, and says so with #warning,
# which this compiler does not have in C++17. The FFT is for effects the game does not use.
target_compile_definitions(alsoft.common PRIVATE PFFFT_SIMD_DISABLE)

target_sources(OpenAL PRIVATE openal/xaudio2_backend.cpp openal/xaudio2_backend.h)
target_include_directories(OpenAL PRIVATE openal "${OPENAL_SOURCE_DIR}/alc")
target_compile_definitions(OpenAL PRIVATE HAVE_XAUDIO2=1)
# as for the executable below: C++/WinRT's error paths (alc/alconfig.cpp) are all that needs it
target_link_libraries(OpenAL PRIVATE delayimp.lib)
target_link_options(OpenAL PRIVATE /DELAYLOAD:oleaut32.dll "/MAP:${CMAKE_BINARY_DIR}/bin/OpenAL32.map")
# a map for the decoder too: the phone logs a fault as module+offset
target_link_options(mpg123 PRIVATE "/MAP:${CMAKE_BINARY_DIR}/bin/libmpg123.map")

# ===[ The game ]===

list(APPEND CMAKE_MODULE_PATH "${REVC_ROOT}/cmake")
include(GetGitRevisionDescription)
get_git_head_revision(GIT_REFSPEC GIT_SHA1 "ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR")
configure_file("${REVC_ROOT}/src/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp" @ONLY)

file(GLOB_RECURSE REVC_SOURCES CONFIGURE_DEPENDS "${REVC_ROOT}/src/*.cpp")
# every directory with a header is an include directory, as in src/CMakeLists.txt
file(GLOB_RECURSE _revc_headers "${REVC_ROOT}/src/*.h")
set(REVC_INCLUDES "")
foreach(_header IN LISTS _revc_headers)
    get_filename_component(_dir "${_header}" DIRECTORY)
    list(APPEND REVC_INCLUDES "${_dir}")
endforeach()
list(REMOVE_DUPLICATES REVC_INCLUDES)

add_library(revc_game STATIC ${REVC_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp")
target_include_directories(revc_game PRIVATE "${REVC_ROOT}" ${REVC_INCLUDES} app)
target_compile_definitions(revc_game PRIVATE
    GTA_W10M LIBRW AUDIO_OAL CMAKE_BUILD CMAKE_NO_AUTOLINK USE_OUR_VERSIONING
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
# /Zc:sizedDealloc- as src/CMakeLists.txt has it for MSVC; the warnings are the game's own style
target_compile_options(revc_game PRIVATE /Zc:sizedDealloc- /w /bigobj)
target_link_libraries(revc_game PUBLIC librw OpenAL::OpenAL MPG123::libmpg123)

# ===[ The shell and the executable ]===

add_executable(reVC WIN32
    app/App.cpp
    app/pch.h app/host_api.h app/TouchPad.h app/GameFiles.h app/Gamepad.h)
target_include_directories(reVC PRIVATE app)
target_compile_definitions(reVC PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(reVC PRIVATE /GR- /bigobj /permissive-)
set_target_properties(reVC PROPERTIES CXX_STANDARD 20)
target_link_libraries(reVC PRIVATE revc_game angle delayimp.lib)
# oleaut32 only on C++/WinRT's error paths, and not every Windows 10 Mobile build has it:
# delay-loaded, a phone without it still starts (gecko-w10m)
target_link_options(reVC PRIVATE
    /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /DELAYLOAD:oleaut32.dll
    "/MAP:${CMAKE_BINARY_DIR}/bin/reVC.map")
add_dependencies(reVC OpenAL mpg123)
