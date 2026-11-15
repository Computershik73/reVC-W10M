# The game itself, as the port builds it (GTA_W10M: the GLFW skeleton over w10m/glfw, OpenAL
# with the XAudio2 backend, mpg123), for the PC that builds the port (x64) and without the
# shell: game_test.cpp stands in for it, with no panel (a pbuffer) and no input.
# tools/test_game.py configures, builds and runs it; what it can and cannot show is said there.
cmake_minimum_required(VERSION 3.20)
project(revc_w10m_game_test LANGUAGES C CXX)

get_filename_component(REVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(W10M "${REVC_ROOT}/w10m")
set(OPENAL_SOURCE_DIR "${W10M}/work/src/openal-soft" CACHE PATH "OpenAL Soft 1.24.3, patched")
set(MPG123_SOURCE_DIR "${W10M}/work/src/mpg123-1.32.10" CACHE PATH "mpg123 1.32.10")
set(ANGLE_INCLUDE_DIR "" CACHE PATH "ANGLE's include directory (EGL/egl.h)")
set(ANGLE_LIB_DIR "" CACHE PATH "Directory with ANGLE's x64 libEGL and libGLESv2 (.lib and .dll)")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_CXX_STANDARD 14)

add_library(glfw STATIC "${W10M}/glfw/glfw_w10m.cpp")
target_include_directories(glfw PUBLIC "${W10M}/glfw/include" "${ANGLE_INCLUDE_DIR}" PRIVATE "${W10M}/app" "${W10M}/glthread")
target_link_libraries(glfw PUBLIC "${ANGLE_LIB_DIR}/libEGL.lib" "${ANGLE_LIB_DIR}/libGLESv2.lib")
target_compile_definitions(glfw PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
set_target_properties(glfw PROPERTIES CXX_STANDARD 17)

set(_librw "${REVC_ROOT}/vendor/librw/src")
file(GLOB LIBRW_SOURCES "${_librw}/*.cpp" "${_librw}/d3d/*.cpp" "${_librw}/gl/*.cpp" "${_librw}/ps2/*.cpp")
add_library(librw STATIC ${LIBRW_SOURCES} "${_librw}/gl/glad/glad.c" "${_librw}/lodepng/lodepng.cpp"
    "${W10M}/glthread/glthread.cpp")
set_source_files_properties("${W10M}/glthread/glthread.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_librw}/gl")
target_include_directories(librw INTERFACE "${REVC_ROOT}/vendor/librw")
target_compile_definitions(librw PUBLIC RW_GL3 PRIVATE LODEPNG_NO_COMPILE_CPP _CRT_SECURE_NO_WARNINGS)
target_compile_options(librw PRIVATE /w)
target_link_libraries(librw PUBLIC glfw)

add_subdirectory("${W10M}/deps/mpg123" mpg123)

set(LIBTYPE "SHARED" CACHE STRING "" FORCE)
foreach(_off IN ITEMS
        ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_UPDATE_BUILD_VERSION ALSOFT_EAX
        ALSOFT_EMBED_HRTF_DATA ALSOFT_BACKEND_WASAPI ALSOFT_BACKEND_WINMM ALSOFT_BACKEND_DSOUND ALSOFT_BACKEND_WAVE
        ALSOFT_BACKEND_SDL2 ALSOFT_BACKEND_SDL3 ALSOFT_BACKEND_PORTAUDIO ALSOFT_BACKEND_JACK)
    set(${_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
add_subdirectory("${OPENAL_SOURCE_DIR}" openal EXCLUDE_FROM_ALL)
target_sources(OpenAL PRIVATE "${W10M}/openal/xaudio2_backend.cpp")
target_include_directories(OpenAL PRIVATE "${W10M}/openal" "${OPENAL_SOURCE_DIR}/alc")
target_compile_definitions(OpenAL PRIVATE HAVE_XAUDIO2=1)

set(GIT_SHA1 "test")
configure_file("${REVC_ROOT}/src/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp" @ONLY)
file(GLOB_RECURSE REVC_SOURCES "${REVC_ROOT}/src/*.cpp")
file(GLOB_RECURSE _revc_headers "${REVC_ROOT}/src/*.h")
set(REVC_INCLUDES "")
foreach(_header IN LISTS _revc_headers)
    get_filename_component(_dir "${_header}" DIRECTORY)
    list(APPEND REVC_INCLUDES "${_dir}")
endforeach()
list(REMOVE_DUPLICATES REVC_INCLUDES)

add_executable(game_test game_test.cpp ${REVC_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/GitSHA1.cpp")
target_include_directories(game_test PRIVATE "${REVC_ROOT}" ${REVC_INCLUDES} "${W10M}/app")
target_compile_definitions(game_test PRIVATE
    GTA_W10M LIBRW AUDIO_OAL CMAKE_BUILD CMAKE_NO_AUTOLINK USE_OUR_VERSIONING
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(game_test PRIVATE /Zc:sizedDealloc- /w /bigobj)
target_link_libraries(game_test PRIVATE librw OpenAL::OpenAL MPG123::libmpg123)
add_custom_command(TARGET game_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANGLE_LIB_DIR}/libEGL.dll" "${ANGLE_LIB_DIR}/libGLESv2.dll" "$<TARGET_FILE_DIR:game_test>")
