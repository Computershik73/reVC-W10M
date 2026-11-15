"""Build reVC for Windows 10 Mobile (ARM32) and pack it: builds/reVC_<version>_ARM.appx.

    python w10m/tools/build.py                 every step
    python w10m/tools/build.py --list          the steps
    python w10m/tools/build.py build pack      only these
    python w10m/tools/build.py --from build    this one and the ones after it

Steps:
    sources    fetch OpenAL Soft and mpg123 into w10m/work/src (pinned: a commit, a SHA-256)
    patch      apply w10m/patches to OpenAL Soft and to vendor/librw (does nothing when applied)
    configure  CMake with cmake/toolchain-arm-msvc.cmake, Ninja, into w10m/work/build
    build      reVC.exe, OpenAL32.dll, libmpg123.dll
    stage      the package's files in w10m/work/stage: the binaries, ANGLE, the C runtime,
               the game files reVC adds to Vice City (gamefiles/), the tiles, the manifest
    shims      a forwarder DLL for each api-set name the package imports, for phones whose
               api-set schema does not know the name
    imports    every import of every staged binary: from a DLL in the package, it must be
               exported there; from the system, it is listed in work/imports.txt
    pack       makeappx, signtool -> w10m/builds, with the certificate and INSTALL.txt

No game data is involved anywhere: the app asks for the player's own copy of the game.
What the build needs and where it looks for it is in tools/env.py.
"""
import argparse
import datetime
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

OPENAL_COMMIT = "dc7d7054a5b4f3bec1dc23a42fd616a0847af948"  # the 1.24.3 tag
PATCHES = [
    # (the tree, its patches)
    (env.OPENAL, os.path.join(env.W10M, "patches", "openal-soft")),
    (os.path.join(env.ROOT, "vendor", "librw"), os.path.join(env.W10M, "patches", "librw")),
]
BINARIES = ["reVC.exe", "OpenAL32.dll", "libmpg123.dll"]
ANGLE = ["libEGL.dll", "libGLESv2.dll"]


def run(cmd, **kw):
    """Run and return the output; on failure print its end and stop."""
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)
    if r.returncode != 0:
        print(r.stdout[-8000:], r.stderr[-4000:])
        sys.exit(f"failed: {os.path.basename(cmd[0])} {' '.join(cmd[1:4])}")
    return r.stdout


def git(tree, *args, check=True):
    r = subprocess.run(["git", "-C", tree] + list(args), capture_output=True, text=True, errors="replace")
    if check and r.returncode != 0:
        print(r.stdout, r.stderr)
        sys.exit(f"failed: git {' '.join(args)} in {tree}")
    return r


# ===[ sources ]===

def step_sources():
    os.makedirs(env.SOURCES, exist_ok=True)
    if not os.path.isfile(os.path.join(env.OPENAL, "alc", "alc.cpp")):
        print(f"cloning OpenAL Soft {env.OPENAL_TAG}")
        run(["git", "clone", "--quiet", "--depth", "1", "--branch", env.OPENAL_TAG, env.OPENAL_URL, env.OPENAL])
    head = git(env.OPENAL, "rev-parse", "HEAD").stdout.strip()
    if head != OPENAL_COMMIT:
        sys.exit(f"{env.OPENAL} is at {head}, not at {env.OPENAL_TAG} ({OPENAL_COMMIT})")

    if not os.path.isfile(os.path.join(env.MPG123, "src", "libmpg123", "libmpg123.c")):
        archive = os.path.join(env.WORK, "dl", os.path.basename(env.MPG123_URL))
        os.makedirs(os.path.dirname(archive), exist_ok=True)
        if not os.path.isfile(archive):
            print(f"downloading {env.MPG123_URL}")
            with urllib.request.urlopen(env.MPG123_URL, timeout=120) as r, open(archive + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(archive + ".part", archive)
        with open(archive, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        if digest != env.MPG123_SHA256:
            sys.exit(f"{archive}: SHA-256 {digest}, expected {env.MPG123_SHA256}")
        print(f"unpacking {os.path.basename(archive)}")
        with tarfile.open(archive) as tar:
            tar.extractall(env.SOURCES, filter="data")  # nothing outside, no links out, no devices
    print("sources: OpenAL Soft", env.OPENAL_TAG, "and mpg123", env.MPG123_VERSION)


# ===[ patch ]===

def step_patch():
    for tree, folder in PATCHES:
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".patch"):
                continue
            patch = os.path.join(folder, name)
            # applied already when it can be taken off again
            if git(tree, "apply", "--reverse", "--check", patch, check=False).returncode == 0:
                print(f"patch: {name} is applied")
                continue
            r = git(tree, "apply", "--whitespace=nowarn", patch, check=False)
            if r.returncode != 0:
                print(r.stdout, r.stderr)
                sys.exit(f"{name} does not apply to {tree}: is the tree at the expected commit?")
            print(f"patch: applied {name}")


# ===[ configure, build ]===

def step_configure():
    os.makedirs(env.BUILD, exist_ok=True)
    cmd = ["cmake", "-G", "Ninja", "-S", env.W10M, "-B", env.BUILD,
           "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(env.W10M, "cmake", "toolchain-arm-msvc.cmake").replace("\\", "/"),
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
           "-DW10M_SOURCES_DIR=" + env.SOURCES.replace("\\", "/"),
           "-DOPENAL_SOURCE_DIR=" + env.OPENAL.replace("\\", "/"),
           "-DMPG123_SOURCE_DIR=" + env.MPG123.replace("\\", "/"),
           "-DANGLE_INCLUDE_DIR=" + env.ANGLE_INCLUDE.replace("\\", "/"),
           "-DANGLE_LIB_DIR=" + env.ANGLE_BIN.replace("\\", "/")]
    r = subprocess.run(cmd, env=env.build_env())
    if r.returncode != 0:
        sys.exit("failed: cmake")


def step_build(jobs=None):
    cmd = ["cmake", "--build", env.BUILD, "--target", "reVC"]
    if jobs:
        cmd += ["--parallel", str(jobs)]
    r = subprocess.run(cmd, env=env.build_env())
    if r.returncode != 0:
        sys.exit("failed: the build")
    for name in BINARIES:
        path = os.path.join(env.BUILD, "bin", name)
        print(f"built {name} ({os.path.getsize(path) / 2**20:.1f} MB)")


# ===[ stage ]===

def version():
    """1.0.<month day>.<hour minute>: each package is newer than the last, and says when it was built."""
    now = datetime.datetime.now()
    return f"1.0.{now.month * 100 + now.day}.{now.hour * 100 + now.minute}"


def commit():
    r = git(env.ROOT, "rev-parse", "--short", "HEAD", check=False)
    return r.stdout.strip() or "unknown"


def step_stage():
    if os.path.isdir(env.STAGE):
        shutil.rmtree(env.STAGE)
    os.makedirs(env.STAGE)
    for name in BINARIES:
        shutil.copy2(os.path.join(env.BUILD, "bin", name), os.path.join(env.STAGE, name))
    for name in ANGLE:
        shutil.copy2(os.path.join(env.ANGLE_BIN, name), os.path.join(env.STAGE, name))
    for name in env.CRT:
        shutil.copy2(os.path.join(env.CRT_DIR, name), os.path.join(env.STAGE, name))
    # What reVC adds to and replaces in the game's own files: the app copies these over the
    # player's copy of the game (app/GameFiles.h).
    shutil.copytree(os.path.join(env.ROOT, "gamefiles"), os.path.join(env.STAGE, "gamefiles"))
    shutil.copytree(os.path.join(env.W10M, "app", "Assets"), os.path.join(env.STAGE, "Assets"))
    for name in ("LICENSE.md",):
        shutil.copy2(os.path.join(env.ROOT, name), os.path.join(env.STAGE, name))
    shutil.copy2(os.path.join(env.W10M, "app", "THIRD-PARTY.txt"), os.path.join(env.STAGE, "THIRD-PARTY.txt"))

    v = version()
    with open(os.path.join(env.W10M, "app", "Package.appxmanifest"), encoding="utf-8") as f:
        manifest = (f.read().replace("@VERSION@", v).replace("@IDENTITY@", env.IDENTITY)
                    .replace("@PUBLISHER@", env.PUBLISHER)
                    # stable, so that an update replaces the same app on the phone
                    .replace("@PHONEID@", str(uuid.uuid5(uuid.NAMESPACE_URL, "revc-w10m"))))
    with open(os.path.join(env.STAGE, "AppxManifest.xml"), "w", encoding="utf-8") as f:
        f.write(manifest)
    with open(os.path.join(env.STAGE, "build.txt"), "w", encoding="utf-8") as f:
        f.write(f"reVC for Windows 10 Mobile {v}, commit {commit()}\n")
    count = sum(len(files) for _, _, files in os.walk(env.STAGE))
    print(f"staged {count} files, version {v}")
    return v


def staged_version():
    with open(os.path.join(env.STAGE, "AppxManifest.xml"), encoding="utf-8") as f:
        return re.search(r'<Identity\b[^>]*?Version="([^"]+)"', f.read(), re.S).group(1)


# ===[ shims ]===

def host_of(apiset):
    """The DLL that hosts an api-set family on Windows 10 Mobile (as venetium-w10m maps them),
    or None where that is not known."""
    if apiset.startswith("api-ms-win-crt-"):
        return "ucrtbase"
    if apiset.startswith(("api-ms-win-core-com-", "api-ms-win-core-winrt")):
        return "combase"
    if apiset.startswith("api-ms-win-core-"):
        return "kernelbase"
    return None


def step_shims():
    """A forwarder DLL for every api-set name the package imports.

    The import libraries are the 1607 SDK's, whose umbrella libraries are the desktop's; a
    phone's api-set schema is a subset of them, and one name outside it fails the whole load.
    A DLL the loader cannot resolve through the schema it looks for in the package: so the
    package carries, under each api-set name, a DLL that forwards the imported functions to
    where they live. Where the schema knows the name the file is never opened (learned in
    venetium-w10m and gecko-w10m, whose tools/gen-apiset-shims.py this follows)."""
    work = os.path.join(env.WORK, "shims")
    os.makedirs(work, exist_ok=True)
    wanted = {}
    for name in sorted(os.listdir(env.STAGE)):
        lower = name.lower()
        if not lower.endswith((".dll", ".exe")) or lower.startswith(("api-ms-win-", "ext-ms-win-")):
            continue
        for dll, names in imports_of(os.path.join(env.STAGE, name)).items():
            if dll.startswith(("api-ms-win-", "ext-ms-win-")):
                wanted.setdefault(dll[:-4], set()).update(n for n in names if not n.startswith("#"))
    benv = env.build_env()
    source, obj = os.path.join(work, "empty.c"), os.path.join(work, "empty.obj")
    with open(source, "w") as f:
        f.write("int revc_w10m_apiset_shim;\n")
    run([os.path.join(env.VC, "bin", "Hostx64", "arm", "cl.exe"), "/nologo", "/c", "/GS-", source, "/Fo" + obj],
        env=benv, cwd=work)
    made, skipped = 0, []
    for apiset in sorted(wanted):
        host = host_of(apiset)
        if not host or not wanted[apiset]:
            skipped.append(apiset)
            continue
        definition = os.path.join(work, apiset + ".def")
        with open(definition, "w") as f:
            f.write(f"LIBRARY {apiset}\nEXPORTS\n")
            for symbol in sorted(wanted[apiset]):
                f.write(f"  {symbol}={host}.{symbol}\n")
        # In two steps: given the .def itself, link.exe looks for each forwarded function among
        # its inputs and fails; lib.exe writes the exports, forwards and all, into an .exp.
        run([os.path.join(env.VC, "bin", "Hostx64", "arm", "lib.exe"), "/nologo", "/MACHINE:ARM",
             "/DEF:" + definition, "/OUT:" + os.path.join(work, apiset + ".lib")], env=benv, cwd=work)
        run([os.path.join(env.VC, "bin", "Hostx64", "arm", "link.exe"), "/nologo", obj,
             os.path.join(work, apiset + ".exp"), "/DLL", "/NOENTRY", "/APPCONTAINER", "/MACHINE:ARM",
             "/NODEFAULTLIB", "/OUT:" + os.path.join(env.STAGE, apiset + ".dll")],
            env=benv, cwd=work)
        made += 1
    print(f"shims: {made} api-set forwarders for the {len(wanted)} names the package imports")
    if skipped:
        print("shims: none for " + ", ".join(skipped) + " (host not known: they must be in the phone's schema)")


# ===[ imports ]===

def dumpbin(*args):
    return run([os.path.join(env.VC, "bin", "Hostx64", "x64", "dumpbin.exe"), "/nologo"] + list(args),
               env=env.build_env())


def imports_of(path):
    """{dll (lower case): [names]}, load-time and delay-loaded alike"""
    result, dll = {}, None
    for line in dumpbin("/imports", path).splitlines():
        if line.strip() == "Summary":  # the sections' sizes follow, shaped like imports
            break
        m = re.match(r"^    (\S+\.(?:dll|exe|DLL|EXE))\s*$", line)
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


def exports_of(path):
    return set(re.findall(r"^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)", dumpbin("/exports", path), re.M))


def step_imports():
    staged = {name.lower(): os.path.join(env.STAGE, name) for name in os.listdir(env.STAGE)
              if name.lower().endswith((".dll", ".exe"))}
    exports, system, failed = {}, {}, []
    for name, path in sorted(staged.items()):
        if name.startswith(("api-ms-win-", "ext-ms-win-")):
            continue  # the forwarders of step_shims: they import nothing
        for dll, names in imports_of(path).items():
            if dll.startswith(("api-ms-win-", "ext-ms-win-")):
                # the phone's own, through its schema; the forwarder in the package is the fallback
                system.setdefault(dll, {}).setdefault(name, []).extend(names)
                continue
            if dll in staged:
                if dll not in exports:
                    exports[dll] = exports_of(staged[dll])
                for symbol in names:
                    if not symbol.startswith("#") and symbol not in exports[dll]:
                        failed.append(f"{name} imports {symbol} from {dll}, which does not export it")
            else:
                system.setdefault(dll, {}).setdefault(name, []).extend(names)
    report = os.path.join(env.WORK, "imports.txt")
    with open(report, "w", encoding="utf-8") as f:
        f.write("System DLLs the package's binaries import, and what from each.\n"
                "api-ms-win-* and ext-ms-win-* are api-sets; anything else is a DLL by name.\n\n")
        for dll in sorted(system):
            f.write(f"{dll}\n")
            for name in sorted(system[dll]):
                f.write(f"    {name}: {', '.join(sorted(set(system[dll][name])))}\n")
    by_name = sorted(d for d in system if not d.startswith(("api-ms-win-", "ext-ms-win-")))
    print(f"imports: {len(system)} system DLLs ({report}); by name: {', '.join(by_name) or 'none'}")
    extensions = sorted(d for d in system if d.startswith("ext-ms-win-"))
    if extensions:
        print("imports: extension api-sets, which a phone may not have: " + ", ".join(extensions))
    for line in failed:
        print("imports:", line)
    if failed:
        sys.exit("the package would not load: see above")


# ===[ pack ]===

def step_pack():
    os.makedirs(env.OUT, exist_ok=True)
    v = staged_version()
    appx = os.path.join(env.OUT, f"reVC_{v}_ARM.appx")
    run([os.path.join(env.SDK_BIN, "makeappx.exe"), "pack", "/o", "/d", env.STAGE, "/p", appx])
    if not os.path.isfile(env.PFX):
        sys.exit(f"packed {appx}, but there is no certificate to sign it with: {env.PFX} (REVC_PFX)")
    run([os.path.join(env.SDK_BIN, "signtool.exe"), "sign", "/fd", "SHA256", "/f", env.PFX, "/p", env.PFX_PASSWORD, appx])
    # what goes out with the package: the certificate to trust and the instructions
    cer = os.path.join(env.OUT, "Computershik.cer")
    run(["powershell", "-NoProfile", "-Command",
         "$c = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($env:REVC_P, $env:REVC_W); "
         "[IO.File]::WriteAllBytes($env:REVC_C, $c.Export('Cert'))"],
        env=dict(os.environ, REVC_P=env.PFX, REVC_W=env.PFX_PASSWORD, REVC_C=cer))
    shutil.copy2(os.path.join(env.W10M, "app", "INSTALL.txt"), os.path.join(env.OUT, "INSTALL.txt"))
    # the linker map of this very build: the phone logs a crash as module+offset
    maps = os.path.join(env.OUT, "maps")
    os.makedirs(maps, exist_ok=True)
    for module in ("reVC", "OpenAL32", "libmpg123"):
        shutil.copy2(os.path.join(env.BUILD, "bin", module + ".map"), os.path.join(maps, f"{module}_{v}.map"))
    print(f"package: {appx} ({os.path.getsize(appx) / 2**20:.1f} MB), version {v}")
    return appx


STEPS = [
    ("sources", step_sources),
    ("patch", step_patch),
    ("configure", step_configure),
    ("build", step_build),
    ("stage", step_stage),
    ("shims", step_shims),
    ("imports", step_imports),
    ("pack", step_pack),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("steps", nargs="*", help="steps to run (default: all)")
    ap.add_argument("--from", dest="start", help="run this step and every one after it")
    ap.add_argument("--list", action="store_true", help="name the steps and stop")
    a = ap.parse_args()
    names = [name for name, _ in STEPS]
    if a.list:
        print("\n".join(names))
        return
    unknown = [s for s in a.steps + ([a.start] if a.start else []) if s not in names]
    if unknown:
        sys.exit(f"no such step: {', '.join(unknown)} (steps: {', '.join(names)})")
    chosen = a.steps or names
    if a.start:
        chosen = names[names.index(a.start):]
    missing = env.check()
    if missing:
        sys.exit("missing:\n  " + "\n  ".join(missing))
    for name, step in STEPS:
        if name in chosen:
            print(f"=== {name} ===")
            step()


if __name__ == "__main__":
    main()
