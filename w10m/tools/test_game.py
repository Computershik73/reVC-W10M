"""Run the game as the port builds it on this PC (x64), without the shell and without a screen.

    python w10m/tools/test_game.py [--game DIR] [--frames N] [--seconds S] [--level 9_3] [--press FRAME:BUTTON ...]
                                   [--key FRAME:VK ...] [--timeout SECONDS]

The game's own code with the port's changes (GTA_W10M), the port's GLFW layer over ANGLE (drawing
into a pbuffer), OpenAL Soft with the XAudio2 backend and mpg123 are built for x64 and started in
--game: the folder of your own copy of Grand Theft Auto: Vice City with reVC's gamefiles copied
over it (as the app keeps it on the phone). After --frames frames the window "closes" and the
game is expected to shut down; --press holds a pad button (host_api.h's index: 0 is the cross,
7 Start...) from a frame on, to get through the menus, and --key presses a key (a virtual-key
code: 123 is F12, with which the game saves a screenshot into its folder: the only way to see
what it drew). --level caps Direct3D (9_3: OpenGL ES 2.0, what most Windows phones have).

Without --game it runs in a folder that has only reVC's own gamefiles and an empty archive of
models: the game starts its engine through the GLFW layer there and then stops at the first
file of the real game it needs. That much is what can be checked without a copy of the game.

Sound is not silenced: with a game folder the menu's music plays for as long as the test runs.
The output is the game's own, a line at a time, also written to w10m/work/test-game/game.log.
A PC is not a phone: the same code, another processor, driver and file system.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402
import test_audio  # noqa: E402  (the x64 environment)

TEST = os.path.join(env.W10M, "test", "game")
BUILD = os.path.join(env.WORK, "test-game")
ANGLE_X64 = os.environ.get("REVC_ANGLE_BIN_X64", os.path.join(env.BUTTERSCOTCH, "work", "angle", "x64", "bin"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="the game's folder (default: a folder with only reVC's gamefiles)")
    ap.add_argument("--frames", type=int, default=300,
                    help="polls of the event queue: more than one a frame where the game waits for its frame's time")
    ap.add_argument("--seconds", type=float, default=0, help="close the window after so many seconds instead (--frames 0)")
    ap.add_argument("--level", default="none", help="cap Direct3D at this feature level (9_3, 9_1, 10_0)")
    ap.add_argument("--press", action="append", default=[], metavar="FRAME:BUTTON")
    ap.add_argument("--key", action="append", default=[], metavar="FRAME:VK")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--renderer", default="angle", choices=["angle", "d3d11"],
                    help="angle: OpenGL ES through ANGLE; d3d11: the Direct3D 11 renderer (w10m/rwd3d11)")
    a = ap.parse_args()
    global BUILD
    if a.renderer == "d3d11":
        BUILD = os.path.join(env.WORK, "test-game-d3d11")
    elif not os.path.isfile(os.path.join(ANGLE_X64, "libGLESv2.dll")):
        sys.exit(f"no x64 ANGLE in {ANGLE_X64} (REVC_ANGLE_BIN_X64)")
    e = test_audio.host_env()
    cl = os.path.join(env.VC, "bin", "Hostx64", "x64", "cl.exe").replace("\\", "/")
    steps = [
        ["cmake", "-G", "Ninja", "-S", TEST, "-B", BUILD, "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DCMAKE_C_COMPILER=" + cl, "-DCMAKE_CXX_COMPILER=" + cl,
         "-DOPENAL_SOURCE_DIR=" + env.OPENAL.replace("\\", "/"),
         "-DMPG123_SOURCE_DIR=" + env.MPG123.replace("\\", "/"),
         "-DANGLE_INCLUDE_DIR=" + env.ANGLE_INCLUDE.replace("\\", "/"),
         "-DANGLE_LIB_DIR=" + ANGLE_X64.replace("\\", "/"),
         "-DREVC_RENDERER=" + a.renderer, "-DREVC_PYTHON=" + sys.executable.replace("\\", "/")],
        ["cmake", "--build", BUILD, "--target", "game_test"],
    ]
    for cmd in steps:
        r = subprocess.run(cmd, env=e, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print((r.stdout + r.stderr)[-8000:])
            sys.exit(f"failed: {' '.join(cmd[:3])}")
    if a.build_only:
        return

    game = a.game
    if not game:
        # reVC's own files and nothing else: a fresh copy each time, the game writes into it
        game = os.path.join(BUILD, "gamefiles-only")
        if os.path.isdir(game):
            shutil.rmtree(game)
        shutil.copytree(os.path.join(env.ROOT, "gamefiles"), game)
        # An empty archive of models, so that the game's streaming has something to open: it
        # waits forever on a read from a file that is not there. Zeros, not game data.
        with open(os.path.join(game, "models", "gta3.img"), "wb") as f:
            f.write(bytes(2048 * 4))
        open(os.path.join(game, "models", "gta3.dir"), "wb").close()
    run_env = dict(e, ALSOFT_LOGLEVEL="2")
    run_env.pop("REVC_ANGLE_FL", None)
    if a.level != "none":
        run_env["REVC_ANGLE_FL"] = a.level
    run_env["PATH"] = os.path.join(BUILD, "bin") + os.pathsep + run_env["PATH"]
    cmd = [os.path.join(BUILD, "bin", "game_test.exe"), "--frames", str(0 if a.seconds else a.frames)]
    if a.seconds:
        cmd += ["--seconds", str(a.seconds)]
    for press in a.press:
        cmd += ["--press", press]
    for key in a.key:
        cmd += ["--key", key]
    log = os.path.join(BUILD, "game.log")
    print(f"running in {game}, {a.frames} frames, Direct3D level {a.level}; the log is {log}", flush=True)
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        try:
            r = subprocess.run(cmd, env=run_env, cwd=game, stdout=f, stderr=subprocess.STDOUT, timeout=a.timeout)
            code = r.returncode
        except subprocess.TimeoutExpired:
            code = None
    with open(log, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    print("\n".join(lines[-40:]))
    print(f"--- {len(lines)} lines; " + ("timed out" if code is None else f"exit code {code}"))
    if not a.game:
        # no game to run: what counts is that the engine started on the port's GLFW layer
        started = any("glfw: OpenGL ES" in line or "d3d: Direct3D 11 at feature level" in line for line in lines)
        print("the engine started through the GLFW layer" if started else "the engine did NOT start")
        sys.exit(0 if started else 1)
    sys.exit(0 if code == 0 else 1)


if __name__ == "__main__":
    main()
