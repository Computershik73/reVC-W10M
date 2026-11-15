# The XAudio2 backend of w10m/openal, on the PC that builds the port: OpenAL Soft for x64 with
# that backend as its only output, and a program that plays through it (silently) and checks
# that the mix is consumed at the rate it should be. tools/test_audio.py configures, builds and
# runs it. A phone's XAudio2 is the same API; what this cannot show is how a phone behaves.
cmake_minimum_required(VERSION 3.20)
project(revc_w10m_openal_test LANGUAGES C CXX)

set(OPENAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../work/src/openal-soft" CACHE PATH "OpenAL Soft 1.24.3, patched")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(LIBTYPE "SHARED" CACHE STRING "" FORCE)
foreach(_off IN ITEMS
        ALSOFT_UTILS ALSOFT_EXAMPLES ALSOFT_TESTS ALSOFT_INSTALL ALSOFT_UPDATE_BUILD_VERSION ALSOFT_EAX
        ALSOFT_EMBED_HRTF_DATA ALSOFT_BACKEND_WASAPI ALSOFT_BACKEND_WINMM ALSOFT_BACKEND_DSOUND ALSOFT_BACKEND_WAVE
        ALSOFT_BACKEND_SDL2 ALSOFT_BACKEND_SDL3 ALSOFT_BACKEND_PORTAUDIO ALSOFT_BACKEND_JACK)
    set(${_off} OFF CACHE BOOL "" FORCE)
endforeach()
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
add_subdirectory("${OPENAL_SOURCE_DIR}" openal EXCLUDE_FROM_ALL)

target_sources(OpenAL PRIVATE ../../openal/xaudio2_backend.cpp ../../openal/xaudio2_backend.h)
target_include_directories(OpenAL PRIVATE ../../openal "${OPENAL_SOURCE_DIR}/alc")
target_compile_definitions(OpenAL PRIVATE HAVE_XAUDIO2=1)

add_executable(xaudio2_test xaudio2_test.cpp)
target_link_libraries(xaudio2_test PRIVATE OpenAL::OpenAL)
set_target_properties(xaudio2_test PROPERTIES CXX_STANDARD 17)
