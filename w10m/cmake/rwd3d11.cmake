# The Direct3D 11 renderer (rwd3d11/) in the place of librw's OpenGL renderer and of ANGLE:
# the targets glfw (GLFW's API for the port, built without EGL) and librw (librw's own code,
# with rwd3d11/ where its gl/ would be). Included by the package's CMakeLists.txt and by the
# tests' where REVC_RENDERER is d3d11. Wants W10M and REVC_ROOT set.
#
# The shaders are compiled here, when the package is built: tools/make_shaders.py runs the
# Windows SDK's fxc over rwd3d11/shaders and writes the include that rwd3d11/shader.cpp takes
# them from.
set(REVC_PYTHON "python" CACHE FILEPATH "Python, for tools/make_shaders.py")
set(REVC_FXC "" CACHE FILEPATH "fxc.exe of the Windows SDK (left empty: the one tools/env.py names)")

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
    message(FATAL_ERROR "vendor/librw is not patched for the Direct3D 11 renderer: git -C vendor/librw apply ../../w10m/patches/librw/0001-gl3-OpenGL-ES-3.0-and-ANGLE.patch (tools/build.py does it)")
endif()

# librw without its OpenGL renderer: of gl/ only what calls no OpenGL (the driver's
# registration, the material effects' pipeline, the old wdgl format's reader)
file(GLOB LIBRW_SOURCES CONFIGURE_DEPENDS "${_librw}/*.cpp" "${_librw}/d3d/*.cpp" "${_librw}/ps2/*.cpp")
file(GLOB RWD3D11_SOURCES CONFIGURE_DEPENDS "${W10M}/rwd3d11/*.cpp")
file(GLOB RWD3D11_HLSL CONFIGURE_DEPENDS "${W10M}/rwd3d11/shaders/*.hlsl" "${W10M}/rwd3d11/shaders/*.hlsli")

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
# rwd3d11 is public: librw's rwgl3.h, which everything that uses librw includes, takes the
# few OpenGL names its interface is written with from there (rwd3d11gl.h)
target_include_directories(librw
    PUBLIC "${W10M}/rwd3d11"
    INTERFACE "${REVC_ROOT}/vendor/librw"
    PRIVATE "${_librw}" "${_librw}/gl" "${W10M}/app" "${W10M}/glthread" "${_shaders}")
target_compile_definitions(librw PUBLIC RW_GL3 RW_D3D11 PRIVATE LODEPNG_NO_COMPILE_CPP _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(librw PUBLIC glfw d3d11.lib dxgi.lib dxguid.lib)
set_target_properties(librw PROPERTIES PREFIX "")
