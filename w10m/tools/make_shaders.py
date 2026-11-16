"""Compile the Direct3D 11 renderer's shaders (w10m/rwd3d11/shaders) into a C++ include.

    python w10m/tools/make_shaders.py --out DIR [--fxc FXC.EXE]

Every shader is compiled once, when the package is built, for Direct3D's feature level 9_3,
which a device of any higher level takes as well: nothing is compiled on the phone. What
comes out is DIR/d3d11_shaders.inc, which w10m/rwd3d11/shader.cpp includes: the bytecode of
each shader, where each of its uniforms lies in its constant buffers (read out of the
compiler's listing; the renderer sets uniforms by name, as librw's OpenGL renderer does),
which vertex attributes it reads and which samplers.

librw and the game ask for a shader by its GLSL source (Shader::create with the strings of
vendor/librw/src/gl/shaders and src/extras/shaders). So that those calls can stay as they are,
each shader here also says which GLSL source it stands for: the renderer looks the HLSL up by a
hash of the defines and of the body it was handed. A GLSL source that changes no longer finds
its HLSL, and says so when the game starts.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

SHADERS = os.path.join(env.W10M, "rwd3d11", "shaders")
LIBRW = os.path.join(env.ROOT, "vendor", "librw", "src", "gl", "shaders")
GAME = os.path.join(env.ROOT, "src", "extras", "shaders", "obj")

VS = "vs_4_0_level_9_3"
PS = "ps_4_0_level_9_3"
ALL_LIGHTS = ["DIRECTIONALS", "POINTLIGHTS", "SPOTLIGHTS"]
ALL_LIGHTS_GLSL = "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n"
DIRECTIONALS_GLSL = "#define DIRECTIONALS\n"
NO_ALPHATEST_GLSL = "#define NO_ALPHATEST\n"

# name, HLSL file, profile, its defines, and the GLSL it stands for: (defines as the call has
# them, the .inc file with the body, the body's variable)
MANIFEST = [
    ("default", "default_vs.hlsl", VS, [], [("", LIBRW + "/default_vs_gl.inc", "default_vert_src")]),
    ("default_fullLight", "default_vs.hlsl", VS, ALL_LIGHTS,
     [(ALL_LIGHTS_GLSL, LIBRW + "/default_vs_gl.inc", "default_vert_src")]),
    ("default_atlas", "default_vs.hlsl", VS, ["ATLAS"], []),
    ("default_atlas_fullLight", "default_vs.hlsl", VS, ["ATLAS"] + ALL_LIGHTS, []),
    ("im2d", "im2d_vs.hlsl", VS, [], [("", LIBRW + "/im2d_gl.inc", "im2d_vert_src")]),
    ("im3d", "im3d_vs.hlsl", VS, [], [("", LIBRW + "/im3d_gl.inc", "im3d_vert_src")]),
    ("skin", "skin_vs.hlsl", VS, [], [("", LIBRW + "/skin_gl.inc", "skin_vert_src")]),
    ("skin_fullLight", "skin_vs.hlsl", VS, ALL_LIGHTS, [(ALL_LIGHTS_GLSL, LIBRW + "/skin_gl.inc", "skin_vert_src")]),
    ("matfx_env", "matfx_env_vs.hlsl", VS, [], [("", LIBRW + "/matfx_gl.inc", "matfx_env_vert_src")]),
    ("matfx_env_fullLight", "matfx_env_vs.hlsl", VS, ALL_LIGHTS,
     [(ALL_LIGHTS_GLSL, LIBRW + "/matfx_gl.inc", "matfx_env_vert_src")]),
    ("im2d_game", "im2d_vs.hlsl", VS, ["GAME"], [("", GAME + "/im2d_vert.inc", "im2d_vert_src")]),
    ("im2d_UV2", "im2d_vs.hlsl", VS, ["GAME", "UV2"], [("", GAME + "/im2d_UV2_vert.inc", "im2d_UV2_vert_src")]),
    ("default_UV2", "default_vs.hlsl", VS, ["UV2"], [("", GAME + "/default_UV2_vert.inc", "default_UV2_vert_src")]),
    ("neoVehicle", "neoVehicle_vs.hlsl", VS, ["DIRECTIONALS"],
     [(DIRECTIONALS_GLSL, GAME + "/neoVehicle_vert.inc", "neoVehicle_vert_src")]),
    ("neoGloss", "neoGloss_vs.hlsl", VS, ["DIRECTIONALS"], [("", GAME + "/neoGloss_vert.inc", "neoGloss_vert_src")]),
    ("neoRim", "default_vs.hlsl", VS, ["RIM", "DIRECTIONALS"],
     [(DIRECTIONALS_GLSL, GAME + "/neoRim_vert.inc", "neoRim_vert_src")]),
    ("neoRimSkin", "skin_vs.hlsl", VS, ["RIM", "DIRECTIONALS"],
     [(DIRECTIONALS_GLSL, GAME + "/neoRimSkin_vert.inc", "neoRimSkin_vert_src")]),

    ("simple", "simple_ps.hlsl", PS, [],
     [("", LIBRW + "/simple_fs_gl.inc", "simple_frag_src"), ("", GAME + "/simple_frag.inc", "simple_frag_src")]),
    ("simple_noAT", "simple_ps.hlsl", PS, ["NO_ALPHATEST"],
     [(NO_ALPHATEST_GLSL, LIBRW + "/simple_fs_gl.inc", "simple_frag_src"),
      (NO_ALPHATEST_GLSL, GAME + "/simple_frag.inc", "simple_frag_src")]),
    ("atlas", "atlas_ps.hlsl", PS, [], []),
    ("matfx_env_ps", "matfx_env_ps.hlsl", PS, [], [("", LIBRW + "/matfx_gl.inc", "matfx_env_frag_src")]),
    ("matfx_env_ps_noAT", "matfx_env_ps.hlsl", PS, ["NO_ALPHATEST"],
     [(NO_ALPHATEST_GLSL, LIBRW + "/matfx_gl.inc", "matfx_env_frag_src")]),
    ("neoVehicle_ps", "neoVehicle_ps.hlsl", PS, [], [("", GAME + "/neoVehicle_frag.inc", "neoVehicle_frag_src")]),
    ("neoWorldVC", "neoWorldVC_ps.hlsl", PS, [], [("", GAME + "/neoWorldVC_frag.inc", "neoWorldVC_frag_src")]),
    ("neoGloss_ps", "neoGloss_ps.hlsl", PS, [], [("", GAME + "/neoGloss_frag.inc", "neoGloss_frag_src")]),
    ("colourfilterVC", "colourfilterVC_ps.hlsl", PS, [],
     [("", GAME + "/colourfilterVC_frag.inc", "colourfilterVC_frag_src")]),
    ("contrast", "contrast_ps.hlsl", PS, [], [("", GAME + "/contrast_frag.inc", "contrast_frag_src")]),
    ("screenDroplet", "screenDroplet_ps.hlsl", PS, [], [("", GAME + "/screenDroplet_frag.inc", "screenDroplet_frag_src")]),
]

# rw::gl3::ATTRIB_* by the semantic a vertex shader reads it under
ATTRIBS = {
    ("POSITION", 0): 0, ("NORMAL", 0): 1, ("COLOR", 0): 2, ("BLENDWEIGHT", 0): 3, ("BLENDINDICES", 0): 4,
    ("TEXCOORD", 0): 5, ("TEXCOORD", 1): 6, ("TEXCOORD", 2): 7, ("TEXCOORD", 3): 8, ("TEXCOORD", 4): 9,
    ("TEXCOORD", 5): 10, ("TEXCOORD", 6): 11, ("TEXCOORD", 7): 12,
}


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


_inc_cache = {}


def glsl_string(path, variable):
    """The string a generated .inc file gives its variable, as the C compiler reads it."""
    if path not in _inc_cache:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        found = {}
        for m in re.finditer(r"const char \*(\w+)\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)\s*;", text):
            parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(2))
            value = "".join(parts)
            value = re.sub(r"\\(.)", lambda e: {"n": "\n", "t": "\t", "\\": "\\", "\"": "\"", "r": "\r"}.get(e.group(1), e.group(1)), value)
            found[m.group(1)] = value
        _inc_cache[path] = found
    if variable not in _inc_cache[path]:
        sys.exit(f"{path} has no {variable}")
    return _inc_cache[path][variable]


def compile_one(fxc, out, entry):
    name, source, profile, defines, _ = entry
    cso = os.path.join(out, name + ".cso")
    lst = os.path.join(out, name + ".lst")
    cmd = [fxc, "/nologo", "/T", profile, "/E", "main", "/O3", "/Fo", cso, "/Fc", lst]
    for d in defines:
        cmd += ["/D", d + "=1"]
    cmd.append(os.path.join(SHADERS, source))
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", cwd=SHADERS)
    if r.returncode != 0 or not os.path.isfile(cso):
        return name, None, (r.stdout + r.stderr).strip()
    warnings = "\n".join(line for line in (r.stdout + r.stderr).splitlines() if "warning" in line)
    with open(cso, "rb") as f:
        code = f.read()
    with open(lst, encoding="utf-8", errors="replace") as f:
        listing = f.read()
    return name, (code, listing), warnings


def reflect(listing):
    """Constant buffers (name, slot, size, variables), samplers used, vertex attributes read."""
    buffers = {}
    for m in re.finditer(r"// cbuffer (\w+)\s*\n// \{\s*\n(.*?)\n// \}", listing, re.S):
        variables = []
        for v in re.finditer(r"//\s+[\w<>, ]+?\s+(\w+)(?:\[(\d+)\])?;\s*// Offset:\s*(\d+) Size:\s*(\d+)( \[unused\])?", m.group(2)):
            variables.append((v.group(1), int(v.group(3)), int(v.group(4)), bool(v.group(5))))
        buffers[m.group(1)] = variables
    slots = {}
    samplers = 0
    bindings = re.search(r"// Resource Bindings:\s*\n(.*?)\n//\s*\n//\s*\n", listing, re.S)
    if bindings:
        for line in bindings.group(1).splitlines():
            parts = line.strip("/ \t").split()
            if len(parts) < 5:
                continue
            kind = parts[1]
            where = parts[4]
            if kind == "cbuffer":
                slots[parts[0]] = int(re.sub(r"\D", "", where))
            elif kind == "sampler":
                samplers |= 1 << int(re.sub(r"\D", "", where))
    result = []
    for name, variables in buffers.items():
        if name not in slots:
            continue  # declared and not used
        used = [v for v in variables if not v[3]]
        size = max((v[1] + v[2] for v in variables), default=0)
        size = (size + 15) & ~15
        result.append((name, slots[name], size, [(v[0], v[1], v[2]) for v in used]))
    result.sort(key=lambda b: b[1])
    attribs = 0
    signature = re.search(r"// Input signature:\s*\n(.*?)\n//\s*\n//\s*\n// Output signature", listing, re.S)
    if signature:
        for line in signature.group(1).splitlines():
            parts = line.strip("/ \t").split()
            if len(parts) >= 6 and parts[1].isdigit() and (parts[0], int(parts[1])) in ATTRIBS:
                attribs |= 1 << ATTRIBS[(parts[0], int(parts[1]))]
    return result, samplers, attribs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fxc", default=os.path.join(env.SDK_BIN, "fxc.exe"))
    a = ap.parse_args()
    if not os.path.isfile(a.fxc):
        sys.exit(f"no HLSL compiler at {a.fxc} (--fxc)")
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda entry: compile_one(a.fxc, a.out, entry), MANIFEST))
    failed = [(name, message) for name, compiled, message in results if compiled is None]
    for name, message in failed:
        print(f"--- {name} did not compile:\n{message}")
    if failed:
        sys.exit(f"{len(failed)} of {len(MANIFEST)} shaders did not compile")
    for name, _, warnings in results:
        if warnings:
            print(f"--- {name}:\n{warnings}")

    lines = ["// Generated by w10m/tools/make_shaders.py from w10m/rwd3d11/shaders: do not edit.", ""]
    table = []
    for entry, (name, (code, listing), _) in zip(MANIFEST, results):
        _, source, profile, defines, glsl = entry
        buffers, samplers, attribs = reflect(listing)
        lines.append(f"static const unsigned char code_{name}[] = {{")
        for i in range(0, len(code), 24):
            lines.append("\t" + ",".join(str(b) for b in code[i:i + 24]) + ",")
        lines.append("};")
        for i, (_, _, _, variables) in enumerate(buffers):
            lines.append(f"static const ShaderVar vars_{name}_{i}[] = {{")
            for vname, offset, size in variables:
                lines.append(f"\t{{ \"{vname}\", {offset}, {size} }},")
            if not variables:
                lines.append("\t{ nil, 0, 0 },")
            lines.append("};")
        lines.append(f"static const ShaderBuffer bufs_{name}[] = {{")
        for i, (bname, slot, size, variables) in enumerate(buffers):
            lines.append(f"\t{{ \"{bname}\", {slot}, {size}, {len(variables)}, vars_{name}_{i} }},")
        if not buffers:
            lines.append("\t{ nil, 0, 0, 0, nil },")
        lines.append("};")
        hashes = []
        for glsl_defines, path, variable in glsl:
            hashes.append(fnv1a((glsl_defines + glsl_string(path, variable)).encode("utf-8")))
        lines.append(f"static const uint32 hashes_{name}[] = {{ " + ", ".join(f"0x{h:08X}u" for h in hashes or [0]) + " };")
        table.append(f"\t{{ \"{name}\", {1 if profile == VS else 0}, code_{name}, sizeof(code_{name}), {len(buffers)}, "
                     f"bufs_{name}, 0x{attribs:X}, 0x{samplers:X}, {len(hashes)}, hashes_{name} }},")
        lines.append("")
    lines.append("static const ShaderCode shaderCodes[] = {")
    lines += table
    lines.append("};")
    lines.append("")
    text = "\n".join(lines)
    out = os.path.join(a.out, "d3d11_shaders.inc")
    old = None
    if os.path.isfile(out):
        with open(out, encoding="utf-8") as f:
            old = f.read()
    if old != text:  # untouched when nothing changed: what includes it is not built again
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    total = sum(len(compiled[0]) for _, compiled, _ in results)
    print(f"{len(MANIFEST)} shaders, {total} bytes of bytecode -> {out}")


if __name__ == "__main__":
    main()
