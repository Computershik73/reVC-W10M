"""Make w10m/glthread/calls.inc from librw's glad.h.

    python w10m/tools/gen_glthread.py

glthread.cpp puts a function of its own in the place of every pointer in glad's table, so that
the call is made on the drawing thread. This writes those functions, one for every OpenGL
function glad has (478: whatever librw or the game comes to call is covered), of two kinds:

    queued    no answer, and nothing but numbers for arguments: the arguments are copied into
              the queue and the caller goes on
    waited    anything else (an answer, a pointer to fill or to read from, a string): the
              caller waits until the drawing thread has made the call. Always right, never fast

The few that hand over data and are called every frame (uniforms, vertices, pixels, names to
delete or to make, draws from the caller's memory) are written by hand in glthread.cpp; BY_HAND
below names them. The file is
kept in the repository: run this again only when glad.h changes.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import env  # noqa: E402

GLAD = os.path.join(env.ROOT, "vendor", "librw", "src", "gl", "glad", "glad.h")
OUT = os.path.join(env.W10M, "glthread", "calls.inc")

BY_HAND = {
    "glUniform1fv", "glUniform2fv", "glUniform3fv", "glUniform4fv",
    "glUniform1iv", "glUniform2iv", "glUniform3iv", "glUniform4iv",
    "glUniformMatrix2fv", "glUniformMatrix3fv", "glUniformMatrix4fv",
    "glBufferData", "glBufferSubData",
    "glDeleteBuffers", "glDeleteTextures", "glDeleteVertexArrays", "glDeleteFramebuffers", "glDeleteRenderbuffers",
    "glGenBuffers", "glGenTextures", "glGenVertexArrays",
    "glTexImage2D", "glTexSubImage2D", "glCompressedTexImage2D", "glCompressedTexSubImage2D",
    "glPixelStorei", "glBindBuffer",
    # vertices and indices in the caller's memory: what is bound and enabled is followed
    "glBindVertexArray", "glEnableVertexAttribArray", "glDisableVertexAttribArray",
    "glVertexAttribPointer", "glDrawArrays", "glDrawElements",
}
# A pointer that is an offset into the buffer bound at the time, which is how librw and the
# game use these (vertices and indices are always in buffers): a number like the others.
OFFSETS = {
    ("glVertexAttribIPointer", 4), ("glDrawRangeElements", 5), ("glDrawElementsInstanced", 3),
}
# Nothing but numbers, and still the caller means to wait.
WAITED = {"glFinish"}

TYPEDEF = re.compile(r"^typedef (.+?) ?\(APIENTRYP (PFNGL\w+PROC)\)\((.*)\);\s*$")
POINTER = re.compile(r"^GLAPI (PFNGL\w+PROC) glad_(gl\w+);\s*$")


def functions():
    """[(name, pointer type, return type, [argument types])] in glad.h's order"""
    types, found, seen = {}, [], set()
    with open(GLAD, encoding="utf-8") as f:
        for line in f:
            m = TYPEDEF.match(line)
            if m:
                ret, proc, args = m.group(1).strip(), m.group(2), m.group(3).strip()
                arg_types = []
                if args != "void":
                    for arg in args.split(","):
                        t = re.match(r"^(.*?)(\w+)$", arg.strip())
                        arg_types.append(t.group(1).strip())
                types[proc] = (ret, arg_types)
                continue
            m = POINTER.match(line)
            if m and m.group(2) not in seen:
                seen.add(m.group(2))
                ret, arg_types = types[m.group(1)]
                found.append((m.group(2), m.group(1), ret, arg_types))
    return found


def main():
    out, queued, waited = [], 0, 0
    out.append("// Made by tools/gen_glthread.py from librw's glad.h: not to be edited.\n")
    every = functions()
    for name, proc, ret, args in every:
        out.append(f"static {proc} r_{name};\n")
    out.append("\n")
    for name, proc, ret, args in every:
        if name in BY_HAND:
            continue
        params = ", ".join(f"{t} p{i}" for i, t in enumerate(args)) or "void"
        passed = ", ".join(f"p{i}" for i in range(len(args)))
        numbers = all("*" not in t or (name, i) in OFFSETS for i, t in enumerate(args))
        if ret == "void" and numbers and name not in WAITED:
            queued += 1
            fields = "".join(f" {t} p{i};" for i, t in enumerate(args))
            taken = ", ".join(f"c->p{i}" for i in range(len(args)))
            copies = "".join(f" c->p{i} = p{i};" for i in range(len(args)))
            out.append(
                f"static void APIENTRY w_{name}({params}) {{\n"
                f"    struct C {{ Cmd cmd;{fields} }};\n"
                f"    C* c = (C*)Reserve(Round(sizeof(C)));\n"
                f"    c->cmd.run = [](Cmd* x) {{ C* c = (C*)x; (void)c; r_{name}({taken}); }};\n"
                f"   {copies}\n"
                f"    Publish(&c->cmd);\n"
                f"}}\n")
        elif ret == "void":
            waited += 1
            out.append(
                f"static void APIENTRY w_{name}({params}) {{\n"
                f"    Wait([&] {{ r_{name}({passed}); }});\n"
                f"}}\n")
        else:
            waited += 1
            out.append(
                f"static {ret} APIENTRY w_{name}({params}) {{\n"
                f"    {ret} r;\n"
                f"    Wait([&] {{ r = r_{name}({passed}); }});\n"
                f"    return r;\n"
                f"}}\n")
    missing = BY_HAND - {name for name, _, _, _ in every}
    if missing:
        sys.exit("glad.h has no " + ", ".join(sorted(missing)))
    out.append("\n#define GLT_EACH(X)")
    for i, (name, _, _, _) in enumerate(every):
        out.append((" \\\n   " if i % 4 == 0 else "") + f" X({name})")
    out.append("\n")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))
    print(f"{OUT}: {len(every)} functions: {queued} queued, {waited} waited for, {len(BY_HAND)} by hand")


if __name__ == "__main__":
    main()
