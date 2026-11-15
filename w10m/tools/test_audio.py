"""Build OpenAL Soft with the port's XAudio2 backend for this PC (x64) and play through it.

    python w10m/tools/test_audio.py

w10m/test/openal has what is built and what is checked. The sound is played with the listener's
gain at zero: nothing is heard. Needs the MSVC x64 tools and an audio output device on the PC;
the sources are the ones tools/build.py fetched and patched.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

TEST = os.path.join(env.W10M, "test", "openal")
BUILD = os.path.join(env.WORK, "test-openal")


def host_env():
    """cl.exe for x64 with the same SDK the port's headers come from."""
    e = dict(os.environ)
    include = [os.path.join(env.VC, "include")] + [os.path.join(env.SDK, "Include", env.SDK_VERSION, d)
                                                    for d in ("ucrt", "shared", "um", "winrt", "cppwinrt")]
    lib = [os.path.join(env.VC, "lib", "x64"),
           os.path.join(env.SDK, "Lib", env.SDK_VERSION, "ucrt", "x64"),
           os.path.join(env.SDK, "Lib", env.SDK_VERSION, "um", "x64")]
    e["INCLUDE"] = ";".join(include)
    e["LIB"] = ";".join(lib)
    e["PATH"] = os.path.join(env.VC, "bin", "Hostx64", "x64") + os.pathsep + env.SDK_BIN + os.pathsep + e.get("PATH", "")
    return e


def main():
    if not os.path.isfile(os.path.join(env.OPENAL, "alc", "alc.cpp")):
        sys.exit("no OpenAL Soft sources: python w10m/tools/build.py sources patch")
    e = host_env()
    cl = os.path.join(env.VC, "bin", "Hostx64", "x64", "cl.exe").replace("\\", "/")
    steps = [
        ["cmake", "-G", "Ninja", "-S", TEST, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_C_COMPILER=" + cl, "-DCMAKE_CXX_COMPILER=" + cl,
         "-DOPENAL_SOURCE_DIR=" + env.OPENAL.replace("\\", "/")],
        ["cmake", "--build", BUILD, "--target", "xaudio2_test"],
    ]
    for cmd in steps:
        r = subprocess.run(cmd, env=e, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print((r.stdout + r.stderr)[-6000:])
            sys.exit(f"failed: {' '.join(cmd[:3])}")
    r = subprocess.run([os.path.join(BUILD, "bin", "xaudio2_test.exe")], env=dict(e, ALSOFT_LOGLEVEL="2"),
                       cwd=os.path.join(BUILD, "bin"))
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
