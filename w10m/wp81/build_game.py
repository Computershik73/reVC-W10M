"""Build reVC for Windows Phone 8.1 and pack it: w10m/work/wp81/packages/reVC81_<version>_ARM.appx.

    python w10m/wp81/build_game.py                    every step, ARM
    python w10m/wp81/build_game.py --arch x86         for the Windows 10 Mobile emulator, which
                                                      runs 8.1 packages (a check of the app, not
                                                      of 8.1)
    python w10m/wp81/build_game.py build stage pack   only these
    python w10m/wp81/build_game.py --arch x86 --test REVC_PICKED=testsrc --test-src DIR stage pack
                                                      a test package: settings (env.NAME=value
                                                      files in testenv/, read as LocalState's are)
                                                      and a game folder of its own (testsrc/), for
                                                      the emulator, where nobody taps the folder
                                                      picker and ISETool's uploads are not the app's

A build of its own, apart from the Windows 10 Mobile one (w10m/CMakeLists.txt, cmake/,
tools/build.py, w10m/work/build, w10m/builds), which it neither uses nor changes: its own
CMakeLists.txt and toolchain-wp81.cmake here, its own folders under w10m/work/wp81. The two
share the sources; where they differ for 8.1 the sources say REVC_WP81. Built the way the 8.1
trial is (trial.cpp, README.md here): the C runtime, OpenAL Soft and mpg123 linked into one
program (/MT), and nothing linked against but the Windows Phone 8.1 kit, so a call 8.1 lacks is
an unresolved external, not an app that will not start; stubs.cpp and stubs_game.cpp have those.

Steps:
    configure  CMake with toolchain-wp81.cmake into w10m/work/wp81/game/build-<arch>
    build      reVC.exe
    stage      w10m/work/wp81/game/stage-<arch>: reVC.exe, gamefiles/, tiles at 8.1's sizes,
               an 8.1 manifest (Identity reVC.WP81), the third-party notice for this package
    imports    what the program imports from the system: w10m/work/wp81/game/imports-<arch>.txt
    pack       the Windows 8.1 SDK's makeappx (it knows 8.1's manifest), signtool; ARM into
               w10m/work/wp81/packages with the linker map and TESTER.txt, x86 and test
               packages into w10m/work/wp81/game

OpenAL Soft and mpg123 are the sources tools/build.py fetches and patches into w10m/work/src
(its "sources patch" steps, which build nothing), as vendor/librw is patched there.
"""
import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import env  # noqa: E402  (where the compiler, the SDKs and the certificate are)

PF86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
SDK81_BIN = os.environ.get("REVC_WP81_SDK_BIN", os.path.join(PF86, "Windows Kits", "8.1", "bin", "x86"))
ROOT = os.path.join(env.WORK, "wp81", "game")
PACKAGES = os.path.join(env.WORK, "wp81", "packages")
IDENTITY = "reVC.WP81"
ARCH = "arm"
TEST_ENV = []     # --test NAME=VALUE
TEST_SRC = None   # --test-src DIR


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        print(r.stdout[-8000:], r.stderr[-4000:])
        sys.exit(f"failed: {os.path.basename(cmd[0])} {' '.join(cmd[1:4])}")
    return r.stdout


def version():
    """1.0.<month day>.<hour minute>, as the Windows 10 Mobile packages are numbered"""
    now = datetime.datetime.now()
    return f"1.0.{now.month * 100 + now.day}.{now.hour * 100 + now.minute}"


def commit():
    r = subprocess.run(["git", "-C", env.ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() or "unknown"


def imports_of(path):
    """{dll (lower case): [names]}"""
    out = run([os.path.join(env.VC, "bin", "Hostx64", "x64", "dumpbin.exe"), "/nologo", "/imports", path])
    result, dll = {}, None
    for line in out.splitlines():
        if line.strip() == "Summary":
            break
        m = re.match(r"^    (\S+\.(?:dll|DLL))\s*$", line)
        if m:
            dll = m.group(1).lower()
            result.setdefault(dll, [])
            continue
        m = re.match(r"^\s+(?:[0-9A-F]+\s+)?([0-9A-F]+)\s+(\S+)\s*$", line)
        if m and dll:
            result[dll].append(m.group(2))
            continue
        m = re.match(r"^\s+Ordinal\s+(\d+)\s*$", line)
        if m and dll:
            result[dll].append("#" + m.group(1))
    return result


def kit():
    return os.environ.get("REVC_WP81_KIT") or os.path.join(PF86, "Windows Phone Kits", "8.1", "lib",
                                                          "ARM" if ARCH == "arm" else ARCH)


def build_dir():
    return os.path.join(ROOT, "build-" + ARCH)


def stage_dir():
    return os.path.join(ROOT, "stage-" + ARCH)


def benv():
    e = env.build_env()
    e["W10M_ARCH"] = ARCH
    e["REVC_WP81_KIT"] = kit()
    return e


def step_configure():
    if not os.path.isfile(os.path.join(env.OPENAL, "alc", "alc.cpp")) or not os.path.isdir(env.MPG123):
        sys.exit("OpenAL Soft and mpg123 are not there: python w10m/tools/build.py sources patch")
    os.makedirs(build_dir(), exist_ok=True)
    # this folder's own CMakeLists.txt, not the Windows 10 Mobile one
    cmd = ["cmake", "-G", "Ninja", "-S", HERE, "-B", build_dir(),
           "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(HERE, "toolchain-wp81.cmake").replace("\\", "/"),
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
           "-DOPENAL_SOURCE_DIR=" + env.OPENAL.replace("\\", "/"),
           "-DMPG123_SOURCE_DIR=" + env.MPG123.replace("\\", "/"),
           "-DREVC_PYTHON=" + sys.executable.replace("\\", "/")]
    if subprocess.run(cmd, env=benv()).returncode != 0:
        sys.exit("failed: cmake")


def step_build():
    if subprocess.run(["cmake", "--build", build_dir(), "--target", "reVC"], env=benv()).returncode != 0:
        sys.exit("failed: the build")
    exe = os.path.join(build_dir(), "bin", "reVC.exe")
    print(f"built reVC.exe ({os.path.getsize(exe) / 2**20:.1f} MB)")


def manifest(version):
    phone_id = uuid.uuid5(uuid.NAMESPACE_URL, "revc-wp81")
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!-- reVC (Grand Theft Auto: Vice City, reverse-engineered) for Windows Phone 8.1: the 2010
     manifest with 8.1's additions (m2, m3) and the phone's (mp), as Visual Studio 2013 made them.
     No capabilities: the game's folder comes through the system's folder picker. -->
<Package
    xmlns="http://schemas.microsoft.com/appx/2010/manifest"
    xmlns:m2="http://schemas.microsoft.com/appx/2013/manifest"
    xmlns:m3="http://schemas.microsoft.com/appx/2014/manifest"
    xmlns:mp="http://schemas.microsoft.com/appx/2014/phone/manifest">

  <Identity Name="{IDENTITY}" Publisher="{env.PUBLISHER}" Version="{version}" ProcessorArchitecture="{ARCH}" />

  <mp:PhoneIdentity PhoneProductId="{phone_id}" PhonePublisherId="00000000-0000-0000-0000-000000000000" />

  <Properties>
    <DisplayName>reVC</DisplayName>
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
    <Application Id="App" Executable="reVC.exe" EntryPoint="reVC.App">
      <m3:VisualElements
          DisplayName="reVC"
          Square150x150Logo="Assets\\Square150x150Logo.png"
          Square44x44Logo="Assets\\Square44x44Logo.png"
          Description="Grand Theft Auto: Vice City, reverse-engineered (reVC), for Windows Phone 8.1. Needs your own copy of the game."
          ForegroundText="light"
          BackgroundColor="#14091C">
        <m3:DefaultTile Wide310x150Logo="Assets\\Wide310x150Logo.png" Square71x71Logo="Assets\\Square71x71Logo.png" ShortName="reVC" />
        <m3:SplashScreen Image="Assets\\SplashScreen.png" BackgroundColor="#14091C" />
        <m3:InitialRotationPreference>
          <m3:Rotation Preference="landscape" />
          <m3:Rotation Preference="landscapeFlipped" />
        </m3:InitialRotationPreference>
      </m3:VisualElements>
    </Application>
  </Applications>
</Package>
"""


def third_party():
    """app/THIRD-PARTY.txt with this package's libraries: the Windows 10 Mobile package's are DLLs
    of their own, this one has them linked into reVC.exe."""
    with open(os.path.join(env.W10M, "app", "THIRD-PARTY.txt"), encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")
    start, end = text.index("\nOpenAL32.dll\n"), text.index("\ngamefiles\\\n")
    ours = """
reVC.exe has these linked into it on Windows Phone 8.1, which has no C runtime of today's
compiler to share between DLLs (w10m/wp81/README.md in this port's repository):

OpenAL Soft
    OpenAL Soft 1.24.3 (https://github.com/kcat/openal-soft), GNU Library General Public Licence
    version 2 or later, with an XAudio2 output backend added by this port. Its source, the
    backend, the rest of the program's source and the build that links them
    (w10m/wp81/build_game.py) are in this port's repository, so the program can be linked
    again with another build of the library. It includes {fmt}, (c) Victor Zverovich and
    contributors, MIT licence.

mpg123
    mpg123 1.32.10 (https://www.mpg123.de), (c) 1995-2020 Michael Hipp and others, GNU Lesser
    General Public Licence version 2.1, built from the unchanged release (w10m/deps/mpg123),
    linked in as OpenAL Soft is, and with the same source and build to link it again.

The C and C++ runtime
    Microsoft's Visual C++ runtime, linked in statically, under Microsoft's terms for it.
"""
    return text[:start] + ours + text[end:]


def make_assets(folder):
    """The package's tiles at the sizes an 8.1 manifest names, and a portrait splash screen
    (8.1 shows it over the whole phone: 480x800)."""
    import make_assets as assets  # tools/make_assets.py; needs Pillow
    from PIL import Image
    logo = Image.open(assets.LOGO).convert("RGBA")
    sizes = dict(assets.SIZES)
    sizes["SplashScreen.png"] = (480, 800, 0.6)
    del sizes["Square310x310Logo.png"]
    for name, (w, h, fraction) in sizes.items():
        assets.render(logo, w, h, fraction, 2 if name == "SplashScreen.png" else 4).save(os.path.join(folder, name))


def step_stage():
    stage = stage_dir()
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "Assets"))
    shutil.copy2(os.path.join(build_dir(), "bin", "reVC.exe"), stage)
    shutil.copytree(os.path.join(env.ROOT, "gamefiles"), os.path.join(stage, "gamefiles"))
    make_assets(os.path.join(stage, "Assets"))
    shutil.copy2(os.path.join(env.ROOT, "LICENSE.md"), stage)
    with open(os.path.join(stage, "THIRD-PARTY.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(third_party())
    v = version()
    with open(os.path.join(stage, "AppxManifest.xml"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(manifest(v))
    with open(os.path.join(stage, "build.txt"), "w", encoding="utf-8") as f:
        f.write(f"reVC for Windows Phone 8.1 {v} ({ARCH}), commit {commit()}\n")
    if TEST_ENV or TEST_SRC:
        os.makedirs(os.path.join(stage, "testenv"))
        for setting in TEST_ENV:
            open(os.path.join(stage, "testenv", "env." + setting), "w").close()
        if TEST_SRC:
            shutil.copytree(TEST_SRC, os.path.join(stage, "testsrc"),
                            ignore=shutil.ignore_patterns("movies", "mss", "*.exe", "*.dll", "userfiles"))
        with open(os.path.join(stage, "test.txt"), "w", encoding="utf-8") as f:
            f.write("A TEST package: " + ", ".join(TEST_ENV) + (f"; testsrc from {TEST_SRC}" if TEST_SRC else "") + "\n")
        print("a TEST package:", ", ".join(TEST_ENV), "testsrc" if TEST_SRC else "")
    print(f"staged {sum(len(fs) for _, _, fs in os.walk(stage))} files, version {v}")


def step_imports():
    imports = imports_of(os.path.join(stage_dir(), "reVC.exe"))
    report = os.path.join(ROOT, f"imports-{ARCH}.txt")
    with open(report, "w", encoding="utf-8") as f:
        f.write("What reVC.exe for Windows Phone 8.1 imports from the system: all of it through the 8.1 kit.\n\n")
        for dll in sorted(imports):
            f.write(f"{dll}\n    {', '.join(sorted(set(imports[dll])))}\n")
    by_name = sorted(d for d in imports if not d.startswith("api-ms-win-"))
    print(f"imports: {len(imports)} DLLs ({report}); by name: {', '.join(by_name)}")
    if any(d.startswith(("ext-ms-win-", "ucrtbase", "vcruntime", "msvcp", "api-ms-win-crt-")) for d in imports):
        sys.exit("the program imports a C runtime or an extension api-set: 8.1 has neither")


def step_pack():
    stage = stage_dir()
    with open(os.path.join(stage, "AppxManifest.xml"), encoding="utf-8") as f:
        v = re.search(r'<Identity\b[^>]*?Version="([^"]+)"', f.read(), re.S).group(1)
    test = os.path.isfile(os.path.join(stage, "test.txt"))
    out = PACKAGES if ARCH == "arm" and not test else ROOT
    os.makedirs(out, exist_ok=True)
    appx = os.path.join(out, f"reVC81_{v}_{ARCH.upper()}{'_TEST' if test else ''}.appx")
    run([os.path.join(SDK81_BIN, "makeappx.exe"), "pack", "/o", "/d", stage, "/p", appx])
    run([os.path.join(SDK81_BIN, "signtool.exe"), "sign", "/fd", "SHA256", "/f", env.PFX, "/p", env.PFX_PASSWORD, appx])
    maps = os.path.join(out, "maps")
    os.makedirs(maps, exist_ok=True)
    shutil.copy2(os.path.join(build_dir(), "bin", "reVC.map"), os.path.join(maps, f"reVC81_{v}_{ARCH}.map"))
    if out == PACKAGES:
        # what a tester is asked to do and send back, with this package's name in it
        with open(os.path.join(HERE, "TESTER.txt"), encoding="utf-8") as f:
            text = f.read().replace("@PACKAGE@", os.path.basename(appx))
        with open(os.path.join(out, "TESTER.txt"), "w", encoding="utf-8", newline="\r\n") as f:
            f.write(text)
    print(f"package: {appx} ({os.path.getsize(appx) / 2**20:.1f} MB), version {v}")


STEPS = [("configure", step_configure), ("build", step_build), ("stage", step_stage), ("imports", step_imports),
         ("pack", step_pack)]


def main():
    global ARCH
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("steps", nargs="*")
    ap.add_argument("--arch", choices=["arm", "x86"], default="arm")
    ap.add_argument("--test", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--test-src", metavar="DIR")
    a = ap.parse_args()
    ARCH = a.arch
    global TEST_SRC
    TEST_ENV.extend(a.test)
    TEST_SRC = a.test_src
    names = [n for n, _ in STEPS]
    unknown = [s for s in a.steps if s not in names]
    if unknown:
        sys.exit(f"no such step: {', '.join(unknown)} (steps: {', '.join(names)})")
    for name, step in STEPS:
        if not a.steps or name in a.steps:
            print(f"=== {name} ({ARCH}) ===")
            step()


if __name__ == "__main__":
    main()
