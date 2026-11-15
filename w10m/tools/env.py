"""Paths shared by the build scripts. Every one can be overridden with an environment variable."""
import glob
import os
import re

W10M = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(W10M)
GITHUB = os.path.dirname(ROOT)  # the sibling ports this one borrows from are next to it

WORK = os.environ.get("REVC_WORK", os.path.join(W10M, "work"))
SOURCES = os.path.join(WORK, "src")      # openal-soft, mpg123
BUILD = os.path.join(WORK, "build")      # CMake/Ninja
STAGE = os.path.join(WORK, "stage")      # the package's files
OUT = os.environ.get("REVC_OUT", os.path.join(W10M, "builds"))

OPENAL_TAG = "1.24.3"
OPENAL_URL = "https://github.com/kcat/openal-soft"
OPENAL = os.path.join(SOURCES, "openal-soft")
MPG123_VERSION = "1.32.10"
MPG123_URL = f"https://www.mpg123.de/download/mpg123-{MPG123_VERSION}.tar.bz2"
MPG123_SHA256 = "87b2c17fe0c979d3ef38eeceff6362b35b28ac8589fbf1854b5be75c9ab6557c"
MPG123 = os.path.join(SOURCES, f"mpg123-{MPG123_VERSION}")


def _newest_vc():
    """The newest Visual Studio 2022 toolset that still has the 32-bit ARM compiler and libraries
    (17.14, MSVC 14.44, is the last)."""
    def key(path):
        return [int(n) for n in re.findall(r"\d+", os.path.basename(path))]
    found = [d for d in glob.glob(r"C:\Program Files\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*")
             if os.path.isfile(os.path.join(d, "bin", "Hostx64", "arm", "cl.exe"))
             and os.path.isfile(os.path.join(d, "lib", "arm", "msvcrt.lib"))]
    return max(found, key=key) if found else ""


# MSVC with the 32-bit ARM tools. cl.exe, not clang: clang has no SEH on ARM32 Windows, and
# lld-link emits a Control Flow Guard table for ARM32 that the phone's loader rejects (both
# learned in gecko-w10m).
VC = os.environ.get("W10M_VC") or _newest_vc()
SDK = os.environ.get("W10M_SDK", r"C:\Program Files (x86)\Windows Kits\10")
# Headers and tools from the newest SDK that still has ARM32 libraries and C++/WinRT (26100
# dropped ARM32); import libraries from 1607, so that the api-sets the binaries import exist
# on every Windows 10 Mobile build.
SDK_VERSION = os.environ.get("W10M_SDK_VERSION", "10.0.22621.0")
SDK_LIB_VERSION = os.environ.get("W10M_SDK_LIB_VERSION", "10.0.14393.0")
SDK_BIN = os.path.join(SDK, "bin", SDK_VERSION, "x64")

# ANGLE (OpenGL ES on Direct3D 11, presenting into a XAML SwapChainPanel, down to feature level
# 9_1): butterscotch-w10m builds it with MSVC from gecko-w10m's sources
# (python tools/build_angle.py there). Its binaries and gecko-w10m's headers are used as they are.
BUTTERSCOTCH = os.environ.get("REVC_BUTTERSCOTCH", os.path.join(GITHUB, "butterscotch-w10m"))
GECKO = os.environ.get("REVC_GECKO", os.path.join(GITHUB, "gecko-w10m"))
ANGLE_BIN = os.environ.get("REVC_ANGLE_BIN", os.path.join(BUTTERSCOTCH, "work", "angle", "arm", "bin"))
ANGLE_INCLUDE = os.environ.get(
    "REVC_ANGLE_INCLUDE", os.path.join(GECKO, "engine", "firefox", "third_party", "angle", "include"))
# The UWP C runtime of VCLibs (ARM) under the plain names every binary here and ANGLE import:
# vcruntime140.dll and msvcp140.dll, from gecko-w10m's packaging stage.
CRT_DIR = os.environ.get("REVC_CRT_DIR", os.path.join(GECKO, "app", "GeckoW10m", "AppPackages", "stage"))
CRT = ["vcruntime140.dll", "msvcp140.dll"]

# The certificate the phones of this family of ports already trust (Publisher CN=Computershik)
PFX = os.environ.get("REVC_PFX", os.path.join(GECKO, "app", "GeckoW10m", "Gecko.pfx"))
PFX_PASSWORD = os.environ.get("REVC_PFX_PASSWORD", "gecko_w10m")
PUBLISHER = os.environ.get("REVC_PUBLISHER", "CN=Computershik")

IDENTITY = "reVC.W10M"
EXE = "reVC.exe"


def build_env():
    """The environment for CMake and Ninja: the toolchain file reads these, and the cross
    compiler loads its DLLs (mspdbcore...) from the x64 host directory."""
    env = dict(os.environ)
    env["W10M_VC"] = VC
    env["W10M_SDK"] = SDK
    env["W10M_SDK_VERSION"] = SDK_VERSION
    env["W10M_SDK_LIB_VERSION"] = SDK_LIB_VERSION
    env["PATH"] = os.path.join(VC, "bin", "Hostx64", "x64") + os.pathsep + SDK_BIN + os.pathsep + env.get("PATH", "")
    env["VSLANG"] = "1033"  # the compiler's messages in English, whatever Visual Studio's language
    # nothing of a developer prompt this may have been started from
    for name in ("INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_"):
        env.pop(name, None)
    return env


def check():
    """What must be there before anything is built, as a list of what is missing."""
    missing = []
    if not VC:
        missing.append("Visual Studio 2022 with the MSVC v143 ARM build tools (W10M_VC)")
    for version in (SDK_VERSION, SDK_LIB_VERSION):
        if not os.path.isfile(os.path.join(SDK, "Lib", version, "um", "arm", "WindowsApp.lib")):
            missing.append(f"Windows SDK {version} with ARM libraries (W10M_SDK, W10M_SDK_VERSION, W10M_SDK_LIB_VERSION)")
    if not os.path.isfile(os.path.join(ANGLE_INCLUDE, "EGL", "egl.h")):
        missing.append(f"ANGLE's headers in {ANGLE_INCLUDE} (REVC_ANGLE_INCLUDE)")
    for name in ("libEGL.dll", "libEGL.lib", "libGLESv2.dll", "libGLESv2.lib"):
        if not os.path.isfile(os.path.join(ANGLE_BIN, name)):
            missing.append(f"{name} in {ANGLE_BIN} (REVC_ANGLE_BIN; python tools/build_angle.py in butterscotch-w10m)")
    for name in CRT:
        if not os.path.isfile(os.path.join(CRT_DIR, name)):
            missing.append(f"{name} in {CRT_DIR} (REVC_CRT_DIR)")
    return missing
