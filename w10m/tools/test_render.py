"""Start librw on ANGLE through the port's GLFW layer on this PC (x64), at each Direct3D level.

    python w10m/tools/test_render.py [--levels none,10_0,9_3,9_1]

w10m/test/render has what is built and what is checked: the EGL context, librw's start, every
shader program librw and the game make, one clear read back. "none" is the PC's own level
(OpenGL ES 3.0); 9_3 is what most Windows phones have (OpenGL ES 2.0). Needs the MSVC x64 tools,
the Windows SDK's d3dcompiler_47.dll (it is in Windows) and butterscotch-w10m's x64 ANGLE
(REVC_ANGLE_BIN_X64; python tools/build_angle.py --arch x64 there).
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402
import test_audio  # noqa: E402  (the x64 environment)

TEST = os.path.join(env.W10M, "test", "render")
BUILD = os.path.join(env.WORK, "test-render")
ANGLE_X64 = os.environ.get("REVC_ANGLE_BIN_X64", os.path.join(env.BUTTERSCOTCH, "work", "angle", "x64", "bin"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--levels", default="none,10_0,9_3,9_1", help="Direct3D feature levels to cap ANGLE at")
    a = ap.parse_args()
    if not os.path.isfile(os.path.join(ANGLE_X64, "libGLESv2.dll")):
        sys.exit(f"no x64 ANGLE in {ANGLE_X64} (REVC_ANGLE_BIN_X64)")
    e = test_audio.host_env()
    cl = os.path.join(env.VC, "bin", "Hostx64", "x64", "cl.exe").replace("\\", "/")
    steps = [
        ["cmake", "-G", "Ninja", "-S", TEST, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_C_COMPILER=" + cl, "-DCMAKE_CXX_COMPILER=" + cl,
         "-DANGLE_INCLUDE_DIR=" + env.ANGLE_INCLUDE.replace("\\", "/"),
         "-DANGLE_LIB_DIR=" + ANGLE_X64.replace("\\", "/")],
        ["cmake", "--build", BUILD, "--target", "render_test"],
    ]
    for cmd in steps:
        r = subprocess.run(cmd, env=e, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print((r.stdout + r.stderr)[-8000:])
            sys.exit(f"failed: {' '.join(cmd[:3])}")
    failed = []
    for level in a.levels.split(","):
        level = level.strip()
        print(f"=== Direct3D feature level: {level} ===", flush=True)
        run_env = dict(e)
        run_env.pop("REVC_ANGLE_FL", None)
        if level != "none":
            run_env["REVC_ANGLE_FL"] = level
        # twice: the first run compiles the programs and leaves them in a folder, the second
        # must find every one of them there (librw's programCacheDir)
        kept = os.path.join(BUILD, "programs-" + level)
        shutil.rmtree(kept, ignore_errors=True)
        os.makedirs(kept)
        run_env["REVC_TEST_PROGRAM_CACHE"] = kept
        for again in (False, True):
            if again:
                # and with OpenGL on the drawing thread from librw's start on (w10m/glthread):
                # each level is run both ways
                print(f"--- {level}, again, from the {len(os.listdir(kept))} programs kept, "
                      "on the drawing thread ---", flush=True)
                run_env["REVC_TEST_PROGRAMS_KEPT"] = "1"
                run_env["REVC_TEST_GL_THREAD"] = "1"
            r = subprocess.run([os.path.join(BUILD, "bin", "render_test.exe")], env=run_env, cwd=os.path.join(BUILD, "bin"))
            if r.returncode != 0:
                failed.append(level + (" (kept programs)" if again else ""))
    print("failed at: " + ", ".join(failed) if failed else "every level passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
