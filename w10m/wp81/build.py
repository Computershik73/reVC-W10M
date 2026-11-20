"""Builds the Windows Phone 8.1 trial (trial.cpp): an ARM appx for an unlocked 8.1 phone.

    python w10m/wp81/build.py [--arch arm|x86] [compile] [link] [stage] [pack]   (all four if none)

--arch x86 makes the same app for the Windows 10 Mobile emulator (x86; Windows 10 Mobile runs
Windows Phone 8.1 packages): a check of the app itself, not of 8.1.

Today's compiler (MSVC v143 for ARM, as the game) and today's headers (SDK 22621), but the C
runtime linked into the program (/MT) and nothing but the Windows Phone 8.1 kit's import
libraries to link against: a function 8.1 does not have is an unresolved external here, not an
app that will not start there. stubs.cpp has the few the kit lacks.

Everything goes to w10m/work/wp81 (w10m/work/wp81/x86 for x86): obj/, the program and its map,
stage/, and reVC81Trial_<version>_<arch>.appx signed with gecko-w10m's certificate
(CN=Computershik).
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import env  # noqa: E402

PF86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
ARCH = "arm"
if "--arch" in sys.argv:
    at = sys.argv.index("--arch")
    ARCH = sys.argv[at + 1].lower()
    del sys.argv[at:at + 2]
if ARCH not in ("arm", "x86"):
    sys.exit("--arch arm or x86")
KIT81 = os.environ.get("REVC_WP81_KIT", os.path.join(PF86, "Windows Phone Kits", "8.1", "lib", ARCH.upper() if ARCH == "arm" else ARCH))
SDK81_BIN = os.environ.get("REVC_WP81_SDK_BIN", os.path.join(PF86, "Windows Kits", "8.1", "bin", "x86"))
WORK = os.path.join(env.WORK, "wp81") if ARCH == "arm" else os.path.join(env.WORK, "wp81", ARCH)
OBJ = os.path.join(WORK, "obj")
STAGE = os.path.join(WORK, "stage")
EXE = "reVC81Trial.exe"
IDENTITY = "reVC.WP81Trial"
PHONE_PRODUCT_ID = "6f0b9a52-3c1e-4d7a-9b8e-81a0c0de0801"

# The version: 1.0.<month><day>.<minutes since midnight / 2>, as tools/build.py numbers the game's
_now = time.localtime()
VERSION = f"1.0.{_now.tm_mon}{_now.tm_mday:02d}.{(_now.tm_hour * 60 + _now.tm_min) // 2}"

SOURCES = [
    os.path.join(HERE, "trial.cpp"),
    os.path.join(HERE, "stubs.cpp"),
    os.path.join(env.W10M, "bench", "d3dbench.cpp"),
]


def run(args, **kw):
    print(">", " ".join(f'"{a}"' if " " in a else a for a in args[:3]), "..." if len(args) > 3 else "")
    result = subprocess.run(args, env=env.build_env(), **kw)
    if result.returncode != 0:
        sys.exit(f"failed ({result.returncode})")


def includes():
    inc = os.path.join(env.SDK, "Include", env.SDK_VERSION)
    return [os.path.join(env.VC, "include")] + [os.path.join(inc, d) for d in ("ucrt", "shared", "um", "winrt", "cppwinrt")] + [HERE]


def step_compile():
    os.makedirs(OBJ, exist_ok=True)
    cl = os.path.join(env.VC, "bin", "Hostx64", ARCH, "cl.exe")
    flags = ["/nologo", "/c", "/MT", "/O2", "/Ob2", "/Zi", "/EHsc", "/std:c++20", "/permissive-", "/bigobj", "/utf-8",
             "/Gy", "/Gw", "/guard:cf-", "/W3", "/wd4996",
             "/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE", "/DNDEBUG",
             "/D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1", "/D_WIN32_WINNT=0x0603", "/DWINVER=0x0603",
             f'/DTRIAL_VERSION="{VERSION}"', f"/Fd{os.path.join(OBJ, 'trial.pdb')}", f"/Fo{OBJ}\\"]
    flags += [f"/I{d}" for d in includes()]
    run([cl] + flags + SOURCES)


def step_link():
    link = os.path.join(env.VC, "bin", "Hostx64", ARCH, "link.exe")
    objs = [os.path.join(OBJ, os.path.splitext(os.path.basename(s))[0] + ".obj") for s in SOURCES]
    libpaths = [os.path.join(env.VC, "lib", ARCH), os.path.join(env.SDK, "Lib", env.SDK_LIB_VERSION, "ucrt", ARCH), KIT81]
    # The static C runtime, then only the 8.1 kit (not a library of Windows 10's SDK)
    libs = ["libcmt.lib", "libcpmt.lib", "libvcruntime.lib", "libucrt.lib",
            "WindowsPhoneCore.lib", "runtimeobject.lib", "d3d11.lib", "dxgi.lib", "dxguid.lib", "uuid.lib", "oleaut32.lib"]
    args = [link, "/nologo", f"/OUT:{os.path.join(WORK, EXE)}", f"/MAP:{os.path.join(WORK, 'reVC81Trial.map')}",
            f"/PDB:{os.path.join(WORK, 'reVC81Trial.pdb')}", "/DEBUG", "/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup",
            "/APPCONTAINER", "/DYNAMICBASE", "/NXCOMPAT", f"/MACHINE:{ARCH.upper()}", "/OPT:REF", "/OPT:ICF", "/guard:no",
            "/NODEFAULTLIB", "/IGNORE:4099"]
    args += [f"/LIBPATH:{p}" for p in libpaths] + objs + libs
    run(args)


def manifest():
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!-- reVC's Windows Phone 8.1 trial (w10m/wp81). A Windows Phone 8.1 app package: the 2010
     manifest with the 2013 and 2014 additions, as Visual Studio 2013 made them. -->
<Package
    xmlns="http://schemas.microsoft.com/appx/2010/manifest"
    xmlns:m2="http://schemas.microsoft.com/appx/2013/manifest"
    xmlns:m3="http://schemas.microsoft.com/appx/2014/manifest"
    xmlns:mp="http://schemas.microsoft.com/appx/2014/phone/manifest">

  <Identity Name="{IDENTITY}" Publisher="{env.PUBLISHER}" Version="{VERSION}" ProcessorArchitecture="{ARCH}" />

  <mp:PhoneIdentity PhoneProductId="{PHONE_PRODUCT_ID}" PhonePublisherId="00000000-0000-0000-0000-000000000000" />

  <Properties>
    <DisplayName>reVC 8.1 trial</DisplayName>
    <PublisherDisplayName>Computershik</PublisherDisplayName>
    <Logo>Assets\\StoreLogo.png</Logo>
  </Properties>

  <Prerequisites>
    <OSMinVersion>6.3.1</OSMinVersion>
    <OSMaxVersionTested>6.3.1</OSMaxVersionTested>
  </Prerequisites>

  <Resources>
    <Resource Language="en-US" />
  </Resources>

  <Applications>
    <Application Id="App" Executable="{EXE}" EntryPoint="reVC81Trial.App">
      <m3:VisualElements
          DisplayName="reVC 8.1 trial"
          Square150x150Logo="Assets\\Square150x150Logo.png"
          Square44x44Logo="Assets\\Square44x44Logo.png"
          Description="Does what reVC's Windows 10 Mobile port is made of run on Windows Phone 8.1?"
          ForegroundText="light"
          BackgroundColor="#14091C">
        <m3:DefaultTile Wide310x150Logo="Assets\\Wide310x150Logo.png" Square71x71Logo="Assets\\Square71x71Logo.png" />
        <m3:SplashScreen Image="Assets\\SplashScreen.png" BackgroundColor="#14091C" />
      </m3:VisualElements>
    </Application>
  </Applications>
</Package>
"""


def make_assets(folder):
    """The game's tiles at the sizes a Windows Phone 8.1 package names, and a portrait splash
    screen (8.1 shows it over the whole phone: 480x800)."""
    sys.path.insert(0, os.path.join(env.W10M, "tools"))
    import make_assets as assets  # noqa: E402
    from PIL import Image
    logo = Image.open(assets.LOGO).convert("RGBA")
    sizes = dict(assets.SIZES)
    sizes["SplashScreen.png"] = (480, 800, 0.6)
    del sizes["Square310x310Logo.png"]
    for name, (w, h, fraction) in sizes.items():
        oversample = 2 if name == "SplashScreen.png" else 4
        assets.render(logo, w, h, fraction, oversample).save(os.path.join(folder, name))


def step_stage():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    os.makedirs(os.path.join(STAGE, "Assets"))
    shutil.copy2(os.path.join(WORK, EXE), STAGE)
    with open(os.path.join(STAGE, "AppxManifest.xml"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(manifest())
    make_assets(os.path.join(STAGE, "Assets"))
    print("stage:", STAGE)


def step_pack():
    appx = os.path.join(WORK, f"reVC81Trial_{VERSION}_{ARCH.upper()}.appx")
    for old in os.listdir(WORK):
        if old.startswith("reVC81Trial_") and old.endswith(".appx"):
            os.remove(os.path.join(WORK, old))
    run([os.path.join(SDK81_BIN, "makeappx.exe"), "pack", "/o", "/d", STAGE, "/p", appx])
    run([os.path.join(SDK81_BIN, "signtool.exe"), "sign", "/fd", "SHA256", "/f", env.PFX, "/p", env.PFX_PASSWORD, appx])
    print("package:", appx)


STEPS = {"compile": step_compile, "link": step_link, "stage": step_stage, "pack": step_pack}

if __name__ == "__main__":
    wanted = sys.argv[1:] or list(STEPS)
    for name in wanted:
        if name not in STEPS:
            sys.exit(f"no step {name}: {', '.join(STEPS)}")
    os.makedirs(WORK, exist_ok=True)
    print("version", VERSION, ARCH)
    for name in wanted:
        STEPS[name]()
