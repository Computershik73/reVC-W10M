# Windows 10 Mobile (32-bit ARM, UWP app container) with MSVC's own ARM compiler.
#
# cl.exe/link.exe, not clang: clang has no SEH for 32-bit ARM Windows, and lld-link writes a
# Control Flow Guard table for ARM32 that the phone's loader rejects (learned in gecko-w10m).
#
# Headers come from the newest SDK that still ships ARM32 libraries and C++/WinRT (22621), with
# the desktop partition visible: the game is ordinary Win32 code. Libraries come from an older
# SDK (14393 by default), so that every api-set the binaries import exists on every Windows 10
# Mobile build; a call the phone does not have is then an unresolved external here instead of
# a package that will not load there.
#
# Everything can be moved with the environment (tools/env.py sets the same names):
#   W10M_VC               MSVC toolset with bin/Hostx64/arm    newest VS 2022 one that has it
#   W10M_SDK              Windows Kits 10 root
#   W10M_SDK_VERSION      SDK for headers and tools            10.0.22621.0
#   W10M_SDK_LIB_VERSION  SDK for the import libraries         10.0.14393.0

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR ARM)
set(REVC_W10M ON CACHE BOOL "Windows 10 Mobile build" FORCE)

macro(_w10m_path var env default)
    if(DEFINED ENV{${env}} AND NOT "$ENV{${env}}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{${env}}" ${var})
    else()
        set(${var} "${default}")
    endif()
endmacro()

_w10m_path(W10M_SDK W10M_SDK "C:/Program Files (x86)/Windows Kits/10")
_w10m_path(W10M_SDK_VERSION W10M_SDK_VERSION "10.0.22621.0")
_w10m_path(W10M_SDK_LIB_VERSION W10M_SDK_LIB_VERSION "10.0.14393.0")
if(DEFINED ENV{W10M_VC} AND NOT "$ENV{W10M_VC}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{W10M_VC}" W10M_VC)
else()
    file(GLOB _w10m_vc_candidates "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*")
    list(SORT _w10m_vc_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(_candidate IN LISTS _w10m_vc_candidates)
        if(EXISTS "${_candidate}/bin/Hostx64/arm/cl.exe" AND EXISTS "${_candidate}/lib/arm/msvcrt.lib")
            set(W10M_VC "${_candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${W10M_VC}/bin/Hostx64/arm/cl.exe")
    message(FATAL_ERROR "No MSVC toolset with the 32-bit ARM compiler (set W10M_VC)")
endif()
if(NOT EXISTS "${W10M_SDK}/Lib/${W10M_SDK_LIB_VERSION}/um/arm/WindowsApp.lib")
    message(FATAL_ERROR "No ARM libraries in Windows SDK ${W10M_SDK_LIB_VERSION} (set W10M_SDK_LIB_VERSION)")
endif()

# try_compile() projects see the toolchain file again but not the cache
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES W10M_VC W10M_SDK W10M_SDK_VERSION W10M_SDK_LIB_VERSION)
# no linking in compiler checks: a test executable would need the whole app-container link line
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_w10m_bin "${W10M_VC}/bin/Hostx64/arm")
set(_w10m_sdk_bin "${W10M_SDK}/bin/${W10M_SDK_VERSION}/x64")
set(CMAKE_C_COMPILER "${_w10m_bin}/cl.exe")
set(CMAKE_CXX_COMPILER "${_w10m_bin}/cl.exe")
set(CMAKE_LINKER "${_w10m_bin}/link.exe")
set(CMAKE_AR "${_w10m_bin}/lib.exe")
set(CMAKE_RC_COMPILER "${_w10m_sdk_bin}/rc.exe")
set(CMAKE_MT "${_w10m_sdk_bin}/mt.exe")

set(_w10m_inc "${W10M_SDK}/Include/${W10M_SDK_VERSION}")
set(_w10m_includes
    "${W10M_VC}/include"
    "${_w10m_inc}/ucrt"
    "${_w10m_inc}/shared"
    "${_w10m_inc}/um"
    "${_w10m_inc}/winrt"
    "${_w10m_inc}/cppwinrt")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${_w10m_includes})
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${_w10m_includes})
set(CMAKE_RC_STANDARD_INCLUDE_DIRECTORIES ${_w10m_includes})

# The headers hide the desktop partition from ARM unless told the SDK has it. No Control Flow
# Guard: the UWP C runtime the package carries (and ANGLE) are built without it.
set(_w10m_flags "/D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DWIN32 /D_WINDOWS /guard:cf- /utf-8 /Gy /Gw")
set(CMAKE_C_FLAGS_INIT "${_w10m_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_w10m_flags} /EHsc")
foreach(_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_RELEASE_INIT "/O2 /Ob2 /DNDEBUG")
    set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO_INIT "/O2 /Ob2 /DNDEBUG /Zi")
endforeach()

# The plain-named (not "store") C runtime import libraries first: the package carries the UWP
# C runtime of VCLibs under vcruntime140.dll/msvcp140.dll, the names ANGLE imports too.
set(_w10m_sdk_lib "${W10M_SDK}/Lib/${W10M_SDK_LIB_VERSION}")
set(_w10m_libpaths
    "/LIBPATH:\"${W10M_VC}/lib/arm\""
    "/LIBPATH:\"${W10M_VC}/lib/arm/store\""
    "/LIBPATH:\"${_w10m_sdk_lib}/ucrt/arm\""
    "/LIBPATH:\"${_w10m_sdk_lib}/um/arm\"")
list(JOIN _w10m_libpaths " " _w10m_libpaths)
set(_w10m_link "${_w10m_libpaths} /APPCONTAINER /DYNAMICBASE /NXCOMPAT /MACHINE:ARM /OPT:REF /OPT:ICF /guard:no")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_w10m_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_w10m_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_w10m_link}")
set(CMAKE_STATIC_LINKER_FLAGS_INIT "/MACHINE:ARM")

# What every binary links: the UWP umbrella library first, so a call resolves to the api-set
# an app container may load, then OneCore for the Win32 calls the umbrella leaves out, then
# kernel32 for the few (AddVectoredExceptionHandler) only it exports to app containers.
set(_w10m_libs "WindowsApp.lib OneCoreUap.lib kernel32.lib")
set(CMAKE_C_STANDARD_LIBRARIES "${_w10m_libs}" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "${_w10m_libs}" CACHE STRING "" FORCE)

# /MD: one C runtime (the package's vcruntime140.dll/msvcp140.dll and the system's ucrtbase)
# shared by the executable and every DLL.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
