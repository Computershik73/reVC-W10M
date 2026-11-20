# Windows Phone 8.1 (an app container, 32-bit ARM; x86 for the Windows 10 Mobile emulator) with
# today's MSVC, as cmake/toolchain-arm-msvc.cmake does it for Windows 10 Mobile, but:
#
# - the C runtime is linked into the program (/MT): 8.1 has no ucrtbase.dll, and the package
#   carries no runtime DLLs at all (OpenAL Soft and mpg123 are linked in as well)
# - nothing is linked against but that runtime and the Windows Phone 8.1 kit's import libraries
#   (/NODEFAULTLIB): a call 8.1 does not have is an unresolved external here, and
#   w10m/wp81/stubs.cpp has what had to be added
# - _WIN32_WINNT is 8.1's (0x0603): headers declare nothing newer (XAudio2 2.8, not 2.9)
# - REVC_WP81 is defined for the shell's few differences
#
# Set by w10m/wp81/build_game.py through the environment (tools/env.py's names):
#   W10M_VC, W10M_SDK, W10M_SDK_VERSION, W10M_SDK_LIB_VERSION  as for the W10M build
#   W10M_ARCH       arm (default) or x86
#   REVC_WP81_KIT   the kit's import libraries for that architecture

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 6.3)
set(REVC_WP81 ON CACHE BOOL "Windows Phone 8.1 build (wp81/CMakeLists.txt)" FORCE)

foreach(_name W10M_VC W10M_SDK W10M_SDK_VERSION W10M_SDK_LIB_VERSION REVC_WP81_KIT)
    if(NOT DEFINED ENV{${_name}} OR "$ENV{${_name}}" STREQUAL "")
        message(FATAL_ERROR "${_name} is not set (run w10m/wp81/build_game.py)")
    endif()
    file(TO_CMAKE_PATH "$ENV{${_name}}" ${_name})
endforeach()
set(W10M_ARCH "arm")
if(DEFINED ENV{W10M_ARCH} AND NOT "$ENV{W10M_ARCH}" STREQUAL "")
    set(W10M_ARCH "$ENV{W10M_ARCH}")
endif()
if(W10M_ARCH STREQUAL "arm")
    set(CMAKE_SYSTEM_PROCESSOR ARM)
    set(_machine ARM)
elseif(W10M_ARCH STREQUAL "x86")
    set(CMAKE_SYSTEM_PROCESSOR X86)
    set(_machine X86)
else()
    message(FATAL_ERROR "W10M_ARCH is arm or x86, not ${W10M_ARCH}")
endif()

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES W10M_VC W10M_SDK W10M_SDK_VERSION W10M_SDK_LIB_VERSION REVC_WP81_KIT W10M_ARCH)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_bin "${W10M_VC}/bin/Hostx64/${W10M_ARCH}")
set(_sdk_bin "${W10M_SDK}/bin/${W10M_SDK_VERSION}/x64")
set(CMAKE_C_COMPILER "${_bin}/cl.exe")
set(CMAKE_CXX_COMPILER "${_bin}/cl.exe")
set(CMAKE_LINKER "${_bin}/link.exe")
set(CMAKE_AR "${_bin}/lib.exe")
set(CMAKE_RC_COMPILER "${_sdk_bin}/rc.exe")
set(CMAKE_MT "${_sdk_bin}/mt.exe")

set(_inc "${W10M_SDK}/Include/${W10M_SDK_VERSION}")
set(_includes "${W10M_VC}/include" "${_inc}/ucrt" "${_inc}/shared" "${_inc}/um" "${_inc}/winrt" "${_inc}/cppwinrt")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${_includes})
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${_includes})
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES ${_includes})

set(_flags "/D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 /D_WIN32_WINNT=0x0603 /DWINVER=0x0603 /DNTDDI_VERSION=0x06030000 /DWIN32 /D_WINDOWS /DREVC_WP81 /guard:cf- /utf-8 /Gy /Gw")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags} /EHsc")
foreach(_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_RELEASE_INIT "/O2 /Ob2 /DNDEBUG")
    set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO_INIT "/O2 /Ob2 /DNDEBUG /Zi")
endforeach()

set(_libpaths
    "/LIBPATH:\"${W10M_VC}/lib/${W10M_ARCH}\""
    "/LIBPATH:\"${W10M_SDK}/Lib/${W10M_SDK_LIB_VERSION}/ucrt/${W10M_ARCH}\""
    "/LIBPATH:\"${REVC_WP81_KIT}\"")
list(JOIN _libpaths " " _libpaths)
set(_link "${_libpaths} /APPCONTAINER /DYNAMICBASE /NXCOMPAT /MACHINE:${_machine} /OPT:REF /OPT:ICF /guard:no /NODEFAULTLIB /IGNORE:4099")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_STATIC_LINKER_FLAGS_INIT "/MACHINE:${_machine}")

# The static C runtime, then the kit: nothing of Windows 10's SDK
set(_libs "libcmt.lib libcpmt.lib libvcruntime.lib libucrt.lib oldnames.lib WindowsPhoneCore.lib runtimeobject.lib d3d11.lib dxgi.lib dxguid.lib uuid.lib oleaut32.lib XAudio2.lib")
set(CMAKE_C_STANDARD_LIBRARIES "${_libs}" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "${_libs}" CACHE STRING "" FORCE)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
