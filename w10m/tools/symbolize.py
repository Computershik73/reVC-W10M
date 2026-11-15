"""Name the functions behind the module+0xOFFSET pairs of a log from the phone.

    python w10m/tools/symbolize.py                      w10m/work/phone_revc.log, the newest maps
    python w10m/tools/symbolize.py LOG [--version V]    another log; the maps of build V (1.0.1114.1235)
    python w10m/tools/symbolize.py reVC.exe+0x1D8419    one address (or several)

The phone writes no dumps for a sideloaded app; the shell logs a fault as module+offset and the
return addresses on the faulting thread's stack the same way (App.cpp, StackNote). The linker
maps of each build are kept in w10m/builds/maps: reVC, OpenAL32 and libmpg123. Offsets into
system DLLs stay as they are.

The stack lines are every word of the stack that points into code, not an unwound backtrace:
callers appear in order, with leftovers of earlier calls among them.
"""
import argparse
import bisect
import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

MODULES = {"revc.exe": "reVC", "openal32.dll": "OpenAL32", "libmpg123.dll": "libmpg123"}
PAIR = re.compile(r"([A-Za-z0-9_.\-]+\.(?:exe|dll))\+0x([0-9A-Fa-f]+)")


def load(path):
    """[(offset, name, object)] sorted by offset, from a linker map's "Publics by Value"."""
    base = 0
    symbols = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*Preferred load address is ([0-9A-Fa-f]+)", line)
            if m:
                base = int(m.group(1), 16)
                continue
            m = re.match(r"\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{8})\s+(?:f\s+)?(?:i\s+)?(\S+)", line)
            if m and int(m.group(2), 16) >= base > 0:
                symbols.append((int(m.group(2), 16) - base, m.group(1), m.group(3)))
    symbols.sort()
    return symbols


def undecorate(names):
    """MSVC's undname, when it is there: ?Lock@Mutex@@QAAXXZ reads badly."""
    undname = os.path.join(env.VC, "bin", "Hostx64", "x64", "undname.exe")
    if not os.path.isfile(undname):
        return {}
    try:
        out = subprocess.run([undname, "0x29FFF"] + names, capture_output=True, text=True, errors="replace").stdout
    except OSError:
        return {}
    found = {}
    for m in re.finditer(r'Undecoration of :- "([^"]+)"\s+is :- "([^"]+)"', out):
        found[m.group(1)] = m.group(2)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="*", help="a log file, or module+0xOFFSET pairs")
    ap.add_argument("--version", help="the build the log is from (default: the newest maps)")
    a = ap.parse_args()

    maps_dir = os.path.join(env.OUT, "maps")
    version = a.version
    if not version:
        newest = sorted(glob.glob(os.path.join(maps_dir, "reVC_*.map")), key=os.path.getmtime)
        if not newest:
            sys.exit(f"no maps in {maps_dir}: python w10m/tools/build.py")
        version = os.path.basename(newest[-1])[len("reVC_"):-len(".map")]

    if a.what and all(PAIR.fullmatch(w) for w in a.what):
        lines = [" ".join(a.what)]
    else:
        log = a.what[0] if a.what else os.path.join(env.WORK, "phone_revc.log")
        with open(log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        if not a.version:
            # the log's first line names the build it is from
            m = re.search(r"reVC for Windows 10 Mobile (\d+\.\d+\.\d+\.\d+)", "\n".join(lines[:5]))
            if m:
                version = m.group(1)

    tables = {}
    for module, stem in MODULES.items():
        path = os.path.join(maps_dir, f"{stem}_{version}.map")
        if os.path.isfile(path):
            tables[module] = load(path)
    if not tables:
        sys.exit(f"no maps of build {version} in {maps_dir}")
    print(f"maps of build {version}: {', '.join(sorted(tables))}")

    wanted = []
    for line in lines:
        pairs = [(m.group(1), int(m.group(2), 16)) for m in PAIR.finditer(line)]
        if any(module.lower() in tables for module, _ in pairs):
            wanted.append((line, pairs))
    resolved = {}
    for _, pairs in wanted:
        for module, offset in pairs:
            table = tables.get(module.lower())
            if table is None:
                continue
            i = bisect.bisect_right(table, (offset | 1, "￿", "")) - 1  # Thumb: the address is odd, the symbol too
            if i >= 0:
                resolved[(module, offset)] = (table[i][1], offset - (table[i][0] & ~1), table[i][2])
    plain = undecorate(sorted({name for name, _, _ in resolved.values() if name.startswith("?")}))
    for line, pairs in wanted:
        print()
        print(line.strip()[:160])
        for module, offset in pairs:
            hit = resolved.get((module, offset))
            if hit:
                name, inside, obj = hit
                print(f"    {module}+0x{offset:X}  {plain.get(name, name)} +0x{inside & ~1:X}  [{obj}]")
            else:
                print(f"    {module}+0x{offset:X}")


if __name__ == "__main__":
    main()
