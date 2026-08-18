#!/usr/bin/env python3
"""
extract_shaders.py -- recover the shader assembly from Main/GfxShaders.cpp.

GfxShaders.cpp holds 77 vertex and 78 pixel shaders as compiled Direct3D 9
bytecode (`static DWORD dwXxx[] = {...}`).  Each blob carries a DBUG comment
chunk with the original assembly text, so the source of every shader is
recoverable.  This tool writes them out as text -- one file per shader -- plus
a JSON index with, for each pixel shader, the render states its descriptor
requires (alpha test on/off, function, reference), which are not part of the
shader text.

    python3 tools/extract_shaders.py            # writes android/shaders/d3d/
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ANDROID = os.path.dirname(HERE)
SRC = os.path.join(ANDROID, "gen", "Main", "GfxShaders.cpp")
OUT = os.path.join(ANDROID, "shaders", "d3d")

ARRAY_RE = re.compile(r"static DWORD (dw\w+)\[\] =\s*\{([^}]*)\};")
VS_DECL_RE = re.compile(r"SVShader (vs\w+)\(\s*(\d+),\s*(dw\w+)\);")
PS_DECL_RE = re.compile(
    r"SPShader (ps\w+)\(\s*(\d+),\s*(dw\w+),\s*(dw\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\s*\);")
RS_TABLE_RE = re.compile(r"static SRenderState (\w+)\[\] = \{(.*?)\};", re.DOTALL)
RS_ENTRY_RE = re.compile(r"\{\s*(D3DRS_\w+),\s*([^}]+?)\}")


def fnv1a64(data):
    """FNV-1a over the raw source bytes; the C++ shim computes the same hash
    over the text it finds in the bytecode at CreateVertexShader/CreatePixelShader
    time and looks the GLSL up by it (see compat/d3d9gles/)."""
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xffffffffffffffff
    return h


def blob_source(dwords):
    """The assembly text inside a D3D9 shader blob's DBUG comment."""
    raw = b"".join(struct.pack("<I", d & 0xffffffff) for d in dwords)
    # The DBUG chunk: comment token, 'DBUG', 40-byte header, then the source
    # text NUL-terminated.  Locate the text by its version line instead of
    # trusting offsets.
    for tag in (b"vs.1.1", b"vs_1_1", b"ps.1.1", b"ps.1.4", b"ps_1_1", b"ps_1_4"):
        i = raw.find(tag)
        if i >= 0:
            end = raw.find(b"\0", i)
            return raw[i:end].decode("latin-1")
    return None


def main():
    text = open(SRC, encoding="utf-8").read()
    arrays = {}
    for m in ARRAY_RE.finditer(text):
        name, body = m.groups()
        dwords = [int(x, 0) for x in body.replace("\n", " ").split(",") if x.strip()]
        arrays[name] = dwords

    rs_tables = {}
    for m in RS_TABLE_RE.finditer(text):
        name, body = m.groups()
        rs_tables[name] = [(k, v.strip()) for k, v in RS_ENTRY_RE.findall(body)]

    os.makedirs(os.path.join(OUT, "vs"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "ps"), exist_ok=True)

    index = {"vertex": [], "pixel": []}
    for m in VS_DECL_RE.finditer(text):
        name, nid, arr = m.groups()
        src = blob_source(arrays[arr])
        with open(os.path.join(OUT, "vs", name + ".vsh"), "w") as f:
            f.write("; %s (id %s) -- recovered from GfxShaders.cpp DBUG chunk\n" % (name, nid))
            f.write(src + "\n")
        index["vertex"].append({"id": int(nid), "name": name,
                                "hash": "%016x" % fnv1a64(src.encode("latin-1"))})

    for m in PS_DECL_RE.finditer(text):
        name, nid, arr11, arr14, rssha, tsssha, rsstate, tssstate = m.groups()
        src11 = blob_source(arrays[arr11])
        src14 = blob_source(arrays[arr14])
        with open(os.path.join(OUT, "ps", name + ".psh"), "w") as f:
            f.write("; %s (id %s) -- recovered from GfxShaders.cpp DBUG chunk\n" % (name, nid))
            f.write(src11 + "\n")
            if src14 != src11:
                f.write("\n; ---- ps.1.4 variant ----\n")
                f.write(src14 + "\n")
        states = dict(rs_tables.get(rsstate, []))
        # The engine reports ps.1.4 hardware and hands the shim the ps.1.4 blob,
        # so the hash the shim will look up is that blob's text.
        index["pixel"].append({
            "id": int(nid), "name": name,
            "hash": "%016x" % fnv1a64(src14.encode("latin-1")),
            "hash11": "%016x" % fnv1a64(src11.encode("latin-1")),
            "ps14_differs": src14 != src11,
            "alpha_test": states.get("D3DRS_ALPHATESTENABLE", "FALSE") == "TRUE",
            "alpha_func": states.get("D3DRS_ALPHAFUNC"),
            "alpha_ref": states.get("D3DRS_ALPHAREF"),
        })

    with open(os.path.join(OUT, "index.json"), "w") as f:
        json.dump(index, f, indent=1)
    print("%d vertex, %d pixel shaders -> %s" % (len(index["vertex"]), len(index["pixel"]), OUT))
    n14 = sum(1 for p in index["pixel"] if p["ps14_differs"])
    nat = sum(1 for p in index["pixel"] if p["alpha_test"])
    print("  ps.1.4 variants that differ from ps.1.1: %d;  alpha-tested pixel shaders: %d" % (n14, nat))


if __name__ == "__main__":
    main()
