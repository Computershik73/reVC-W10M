# librw on ANGLE through the port's GLFW layer, for the PC that builds the port (x64):
# render_test.cpp says what is checked, tools/test_render.py configures, builds and runs it at
# each Direct3D feature level. ANGLE is butterscotch-w10m's x64 build of the package's sources
# (python tools/build_angle.py --arch x64 there).
cmake_minimum_required(VERSION 3.20)
project(revc_w10m_render_test LANGUAGES C CXX)

get_filename_component(REVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(W10M "${REVC_ROOT}/w10m")
set(ANGLE_INCLUDE_DIR "" CACHE PATH "ANGLE's include directory (EGL/egl.h)")
set(ANGLE_LIB_DIR "" CACHE PATH "Directory with ANGLE's x64 libEGL and libGLESv2 (.lib and .dll)")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_CXX_STANDARD 17)

add_library(glfw STATIC "${W10M}/glfw/glfw_w10m.cpp")
target_include_directories(glfw PUBLIC "${W10M}/glfw/include" "${ANGLE_INCLUDE_DIR}" PRIVATE "${W10M}/app" "${W10M}/glthread")
target_link_libraries(glfw PUBLIC "${ANGLE_LIB_DIR}/libEGL.lib" "${ANGLE_LIB_DIR}/libGLESv2.lib")
target_compile_definitions(glfw PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

set(_librw "${REVC_ROOT}/vendor/librw/src")
file(GLOB LIBRW_SOURCES "${_librw}/*.cpp" "${_librw}/d3d/*.cpp" "${_librw}/gl/*.cpp" "${_librw}/ps2/*.cpp")
add_library(librw STATIC ${LIBRW_SOURCES} "${_librw}/gl/glad/glad.c" "${_librw}/lodepng/lodepng.cpp"
    "${W10M}/glthread/glthread.cpp")
set_source_files_properties("${W10M}/glthread/glthread.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_librw}/gl")
target_include_directories(librw INTERFACE "${REVC_ROOT}/vendor/librw")
target_compile_definitions(librw PUBLIC RW_GL3 PRIVATE LODEPNG_NO_COMPILE_CPP _CRT_SECURE_NO_WARNINGS)
target_compile_options(librw PRIVATE /W2 /wd4996 /wd4244 /wd4267 /wd4305 /wd4838)
target_link_libraries(librw PUBLIC glfw)

add_executable(render_test render_test.cpp)
# the shader sources, where librw and the game include them from
target_include_directories(render_test PRIVATE "${W10M}/app" "${W10M}/glthread" "${_librw}/gl" "${REVC_ROOT}/src/extras")
target_compile_definitions(render_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(render_test PRIVATE librw)
add_custom_command(TARGET render_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ANGLE_LIB_DIR}/libEGL.dll" "${ANGLE_LIB_DIR}/libGLESv2.dll" "$<TARGET_FILE_DIR:render_test>")
