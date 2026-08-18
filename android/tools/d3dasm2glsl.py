#!/usr/bin/env python3
"""
d3dasm2glsl.py -- translate the engine's vs.1.1 / ps.1.1 / ps.1.4 assembly to
GLSL ES 3.00.

Why a translator and not a hand port: there are 77 vertex and 78 pixel shaders,
their assembly is a small register language (12 VS opcodes, ~15 PS opcodes),
and the visual result depends on ps.1.x semantics that are easy to get subtly
wrong by hand -- the _x2/_x4/_bx2/_sat modifiers, the [-1,1] register range,
co-issued colour/alpha ops, texm3x2/texm3x3 dependent reads.  Encoding those
rules once, here, gives every shader the same faithful treatment and lets the
GLSL be regenerated whenever the rules improve.

Conventions the generated GLSL relies on (see docs/RENDERER.md):

  Vertex inputs (locations)  0 v0 position vec3      1 v1 normal  D3DCOLOR
                             3 v3 texcoord0 short2   4 v4 tangent0 D3DCOLOR
                             5 v5 tangent1 D3DCOLOR  6 v6 texcoord1 short2
    D3DCOLOR attributes are bound as 4 normalized ubytes read in memory order
    (b,g,r,a) and swizzled .zyxw here so v1.xyz is (x,y,z) as D3D saw it.
  Uniforms   vec4 vc[96]  vertex constants c0..c95
             vec4 pc[8]   pixel constants c0..c7
             vec4 posFixup  (yflip, unused, 1/vpW, -1/vpH)   D3D->GL clip fixup
             sampler2D/samplerCube s0..s3   -- type per stage decided by the
             caller (cube_mask), because ps.1.x `tex tN` samples whatever is
             bound and GLSL samplers are typed
  Varyings   vD0, vD1 (colour, clamped 0..1 as D3D vs outputs are), vT0..vT3

Usage:  python3 tools/d3dasm2glsl.py                # regenerate shaders/glsl/
        python3 tools/d3dasm2glsl.py --check NAME   # print one translation
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ANDROID = os.path.dirname(HERE)
D3D_DIR = os.path.join(ANDROID, "shaders", "d3d")
OUT_DIR = os.path.join(ANDROID, "shaders", "glsl")

# ps.1.x register range.  1.0 is what GeForce3-class hardware (the engine's
# HL_GFORCE3, the reference level) provided; Radeon 8500 gave 8.0.  Intermediate
# results are clamped to this after every arithmetic instruction.
PS_RANGE = 1.0

# ---------------------------------------------------------------------------
#  Parsing
# ---------------------------------------------------------------------------
def strip_comment(line):
    i = line.find("//")
    if i >= 0:
        line = line[:i]
    i = line.find(";")
    if i >= 0:
        line = line[:i]
    return line.strip()


def split_args(text):
    """Split 'r0.xyz, v1, c3.xxx' on top-level commas."""
    return [a.strip() for a in text.split(",") if a.strip()]


class Operand:
    """A source or destination operand: register, swizzle/mask, modifiers."""
    def __init__(self, text, is_dst=False):
        self.text = text
        t = text.strip()
        self.neg = False
        self.invert = False        # 1-x
        self.mod = None            # _bx2, _bias, _x2, _dw, _dz
        if not is_dst:
            if t.startswith("-"):
                self.neg = True
                t = t[1:].strip()
            elif t.startswith("1-"):
                self.invert = True
                t = t[2:].strip()
        # register[_mod][.swz]
        m = re.match(r"^([a-zA-Z]+\d*)(?:_(bx2|bias|x2|dw|dz))?(?:\.([xyzwrgba]{1,4}))?$", t)
        if not m:
            raise ValueError("cannot parse operand %r" % text)
        self.reg = m.group(1)
        self.mod = m.group(2)
        swz = m.group(3)
        if swz:
            swz = swz.replace("r", "x").replace("g", "y").replace("b", "z").replace("a", "w")
        self.swz = swz


class Instr:
    def __init__(self, line):
        self.coissue = line.startswith("+")
        if self.coissue:
            line = line[1:].strip()
        head, _, rest = line.partition(" ")
        parts = head.split("_")
        self.op = parts[0]
        self.mods = parts[1:]           # x2, x4, x8, d2, sat
        self.args = [Operand(a, is_dst=(i == 0)) for i, a in enumerate(split_args(rest))]

    @property
    def dst(self):
        return self.args[0]

    @property
    def srcs(self):
        return self.args[1:]


def parse(text):
    version = None
    instrs = []
    for raw in text.split("\n"):
        line = strip_comment(raw)
        if not line:
            continue
        if re.match(r"^(vs|ps)[._]\d[._]\d$", line):
            version = line.replace("_", ".")
            continue
        if line.startswith("dcl_"):
            continue
        if line == "phase":
            continue
        instrs.append(Instr(line))
    return version, instrs


# ---------------------------------------------------------------------------
#  Expression building
# ---------------------------------------------------------------------------
def swizzle_expr(base, swz):
    """Apply a D3D swizzle to a vec4 expression.  Fewer than 4 components
    replicate the last one (D3D rule)."""
    if not swz or swz == "xyzw":
        return base
    if len(swz) < 4:
        swz = swz + swz[-1] * (4 - len(swz))
    return "%s.%s" % (base, swz)


class Translator:
    def __init__(self, cube_mask=0):
        self.cube_mask = cube_mask
        self.lines = []
        self.temps = set()

    def emit(self, s):
        self.lines.append("    " + s)

    # ---- register naming ---------------------------------------------------
    def reg_name(self, reg):
        raise NotImplementedError

    def src_expr(self, o):
        base = self.reg_name(o.reg)
        e = swizzle_expr(base, o.swz)
        if o.mod == "bx2":
            e = "(%s * 2.0 - 1.0)" % e
        elif o.mod == "bias":
            e = "(%s - 0.5)" % e
        elif o.mod == "x2":
            e = "(%s * 2.0)" % e
        if o.invert:
            e = "(1.0 - %s)" % e
        if o.neg:
            e = "(-%s)" % e
        return e

    def write(self, dst, expr, clamp_lo=None, clamp_hi=None):
        """dst[.mask] = expr, with the write mask honoured."""
        name = self.reg_name(dst.reg)
        mask = dst.swz or "xyzw"
        # Ensure the mask is in canonical order for GLSL l-value swizzles.
        canon = "".join(c for c in "xyzw" if c in mask)
        if clamp_lo is not None:
            expr = "clamp(%s, %s, %s)" % (expr, clamp_lo, clamp_hi)
        if canon == "xyzw":
            self.emit("%s = %s;" % (name, expr))
        else:
            self.emit("%s.%s = (%s).%s;" % (name, canon, expr, canon))


# ---------------------------------------------------------------------------
#  Vertex shaders
# ---------------------------------------------------------------------------
class VertexTranslator(Translator):
    INPUTS = {"v0": "vec4(a_pos, 1.0)", "v1": "a_normal.zyxw", "v3": "vec4(a_tex0, 0.0, 1.0)",
              "v4": "a_tangent0.zyxw", "v5": "a_tangent1.zyxw", "v6": "vec4(a_tex1, 0.0, 1.0)"}
    OUTPUTS = {"oPos": "o_pos", "oD0": "vD0", "oD1": "vD1",
               "oT0": "vT0", "oT1": "vT1", "oT2": "vT2", "oT3": "vT3"}

    def reg_name(self, reg):
        if reg in self.INPUTS:
            return self.INPUTS[reg]
        if reg in self.OUTPUTS:
            return self.OUTPUTS[reg]
        if reg.startswith("c") and reg[1:].isdigit():
            return "vc[%d]" % int(reg[1:])
        if reg.startswith("r") and reg[1:].isdigit():
            self.temps.add(reg)
            return reg
        raise ValueError("unknown VS register %r" % reg)

    def translate(self, name, instrs):
        for ins in instrs:
            op = ins.op
            d = ins.dst
            s = [self.src_expr(x) for x in ins.srcs]
            if op == "mov":
                self.write(d, s[0])
            elif op == "mul":
                self.write(d, "(%s * %s)" % (s[0], s[1]))
            elif op == "add":
                self.write(d, "(%s + %s)" % (s[0], s[1]))
            elif op == "sub":
                self.write(d, "(%s - %s)" % (s[0], s[1]))
            elif op == "mad":
                self.write(d, "(%s * %s + %s)" % (s[0], s[1], s[2]))
            elif op == "dp3":
                self.write(d, "vec4(dot((%s).xyz, (%s).xyz))" % (s[0], s[1]))
            elif op == "dp4":
                self.write(d, "vec4(dot(%s, %s))" % (s[0], s[1]))
            elif op == "m4x4":
                # dst = src0 * [c(n)..c(n+3)] rows: dst.i = dot(src0, c(n+i))
                base = ins.srcs[1].reg
                n = int(base[1:])
                rows = ["dot(%s, vc[%d])" % (s[0], n + i) for i in range(4)]
                self.write(d, "vec4(%s)" % ", ".join(rows))
            elif op == "rsq":
                # scalar: the source swizzle selects one component (always given
                # in this codebase); replicate the result.
                self.write(d, "vec4(inversesqrt(abs((%s).x)))" % s[0])
            elif op == "rcp":
                self.write(d, "vec4(1.0 / (%s).x)" % s[0])
            elif op == "max":
                self.write(d, "max(%s, %s)" % (s[0], s[1]))
            elif op == "min":
                self.write(d, "min(%s, %s)" % (s[0], s[1]))
            elif op == "sge":
                self.write(d, "vec4(greaterThanEqual(%s, %s))" % (s[0], s[1]))
            elif op == "slt":
                self.write(d, "vec4(lessThan(%s, %s))" % (s[0], s[1]))
            else:
                raise ValueError("%s: unhandled VS op %r" % (name, op))

    def source(self, name, instrs):
        self.translate(name, instrs)
        body = "\n".join(self.lines)
        temps = "".join("    vec4 %s = vec4(0.0);\n" % t for t in sorted(self.temps))
        return VS_TEMPLATE % {"name": name, "temps": temps, "body": body}


VS_TEMPLATE = """#version 300 es
// %(name)s -- generated by tools/d3dasm2glsl.py from shaders/d3d/vs/%(name)s.vsh
precision highp float;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_normal;    // D3DCOLOR: memory b,g,r,a
layout(location = 3) in vec2 a_tex0;      // SHORT2, unnormalized
layout(location = 4) in vec4 a_tangent0;  // D3DCOLOR
layout(location = 5) in vec4 a_tangent1;  // D3DCOLOR
layout(location = 6) in vec2 a_tex1;      // SHORT2, unnormalized
uniform vec4 vc[96];
uniform vec4 posFixup;    // x: y flip (+1 / -1), z: 1/viewportW, w: -1/viewportH
out vec4 vD0;
out vec4 vD1;
out vec4 vT0;
out vec4 vT1;
out vec4 vT2;
out vec4 vT3;
void main()
{
    vec4 o_pos = vec4(0.0);
    vD0 = vec4(0.0); vD1 = vec4(0.0);
    vT0 = vec4(0.0); vT1 = vec4(0.0); vT2 = vec4(0.0); vT3 = vec4(0.0);
%(temps)s%(body)s
    // D3D vertex-shader colour outputs are saturated.
    vD0 = clamp(vD0, 0.0, 1.0);
    vD1 = clamp(vD1, 0.0, 1.0);
    // D3D clip space -> GL: z from [0,w] to [-w,w]; optional y flip when
    // rendering into a texture (so the image lands top-down like D3D's);
    // half-pixel offset so D3D9's pixel-centre convention lines up.
    o_pos.y *= posFixup.x;
    o_pos.xy += posFixup.zw * o_pos.w;
    o_pos.z = o_pos.z * 2.0 - o_pos.w;
    gl_Position = o_pos;
}
"""


# ---------------------------------------------------------------------------
#  Pixel shaders (ps.1.1 and ps.1.4)
# ---------------------------------------------------------------------------
class PixelTranslator(Translator):
    def __init__(self, cube_mask, version):
        Translator.__init__(self, cube_mask)
        self.version = version
        self.tex_loaded = set()      # tN registers that hold a sampled result (1.1)
        self.stages_used = set()

    def reg_name(self, reg):
        if reg in ("v0", "v1"):
            return "vD%s" % reg[1]
        if reg.startswith("t") and reg[1:].isdigit():
            n = int(reg[1:])
            # In ps.1.1 tN is the interpolated coordinate until `tex tN`
            # replaces it with the sampled colour; we keep both as separate
            # variables and pick by whether the tex happened.
            if reg in self.tex_loaded:
                return "t%d" % n
            return "vT%d" % n
        if reg.startswith("r") and reg[1:].isdigit():
            self.temps.add(reg)
            return reg
        if reg.startswith("c") and reg[1:].isdigit():
            return "pc[%d]" % int(reg[1:])
        raise ValueError("unknown PS register %r" % reg)

    def sampler(self, stage):
        self.stages_used.add(stage)
        return "s%d" % stage

    def is_cube(self, stage):
        return (self.cube_mask >> stage) & 1

    def sample(self, stage, coord_expr, projective=False):
        """A stage's sampler type is only known at draw time (ps.1.x `tex`
        samples whatever is bound).  The stored GLSL therefore declares each
        stage under #ifdef A5_CUBEn and samples through TEXn()/TEXPn() macros;
        the shim prepends the defines for the bound texture types."""
        self.stages_used.add(stage)
        if projective:
            return "TEXP%d(%s)" % (stage, coord_expr)
        return "TEX%d(%s)" % (stage, coord_expr)

    def result_clamp(self, mods):
        if "sat" in mods:
            return "0.0", "1.0"
        return "-%.1f" % PS_RANGE, "%.1f" % PS_RANGE

    def scale(self, expr, mods):
        for m in mods:
            if m == "x2":
                expr = "(%s * 2.0)" % expr
            elif m == "x4":
                expr = "(%s * 4.0)" % expr
            elif m == "x8":
                expr = "(%s * 8.0)" % expr
            elif m == "d2":
                expr = "(%s * 0.5)" % expr
        return expr

    def arith(self, ins, s):
        op = ins.op
        if op == "mov":
            return s[0]
        if op == "mul":
            return "(%s * %s)" % (s[0], s[1])
        if op == "add":
            return "(%s + %s)" % (s[0], s[1])
        if op == "sub":
            return "(%s - %s)" % (s[0], s[1])
        if op == "mad":
            return "(%s * %s + %s)" % (s[0], s[1], s[2])
        if op == "lrp":
            # lrp dst, s0, s1, s2 = s0*s1 + (1-s0)*s2
            return "mix(%s, %s, %s)" % (s[2], s[1], s[0])
        if op == "dp3":
            return "vec4(dot((%s).xyz, (%s).xyz))" % (s[0], s[1])
        if op == "dp4":
            return "vec4(dot(%s, %s))" % (s[0], s[1])
        if op == "cnd":
            # ps.1.1: cnd dst, r0.a, s1, s2 -> r0.a > 0.5 ? s1 : s2 (scalar test)
            # ps.1.4: per-component test of s0
            if self.version == "ps.1.1":
                return "((%s).x > 0.5 ? %s : %s)" % (s[0], s[1], s[2])
            return "mix(%s, %s, vec4(greaterThan(%s, vec4(0.5))))" % (s[2], s[1], s[0])
        if op == "cmp":
            return "mix(%s, %s, vec4(greaterThanEqual(%s, vec4(0.0))))" % (s[2], s[1], s[0])
        raise ValueError("unhandled PS op %r" % op)

    def translate(self, name, instrs):
        i = 0
        while i < len(instrs):
            ins = instrs[i]
            op = ins.op
            # ---- texture addressing ops -------------------------------
            if op == "tex":                       # ps.1.1: sample stage N at tN
                n = int(ins.dst.reg[1:])
                self.emit("vec4 t%d = %s;" % (n, self.sample(n, "vT%d" % n)))
                self.tex_loaded.add("t%d" % n)
            elif op == "texcoord":                # ps.1.1: tN = clamp(coord)
                n = int(ins.dst.reg[1:])
                self.emit("vec4 t%d = clamp(vT%d, 0.0, 1.0);" % (n, n))
                self.tex_loaded.add("t%d" % n)
            elif op == "texreg2ar":               # tN = sample N at (tM.a, tM.r)
                n = int(ins.dst.reg[1:])
                m = self.src_expr(ins.srcs[0])
                self.emit("vec4 t%d = %s;" % (n, self.sample(n, "vec4((%s).w, (%s).x, 0.0, 1.0)" % (m, m))))
                self.tex_loaded.add("t%d" % n)
            elif op == "texm3x2pad":
                # first row of a 3x2 matrix multiply: u = dot(tN.xyz, src.rgb)
                n = int(ins.dst.reg[1:])
                m = self.src_expr(ins.srcs[0])
                self.emit("float m3x2_u = dot(vT%d.xyz, (%s).xyz);" % (n, m))
            elif op == "texm3x2tex":
                n = int(ins.dst.reg[1:])
                m = self.src_expr(ins.srcs[0])
                self.emit("float m3x2_v = dot(vT%d.xyz, (%s).xyz);" % (n, m))
                self.emit("vec4 t%d = %s;" % (n, self.sample(n, "vec4(m3x2_u, m3x2_v, 0.0, 1.0)")))
                self.tex_loaded.add("t%d" % n)
            elif op == "texm3x3pad":
                n = int(ins.dst.reg[1:])
                m = self.src_expr(ins.srcs[0])
                # rows accumulate into m3x3_n and the eye vector from .w
                self.emit("float m3x3_%d = dot(vT%d.xyz, (%s).xyz);" % (n, n, m))
            elif op == "texm3x3vspec":
                n = int(ins.dst.reg[1:])
                m = self.src_expr(ins.srcs[0])
                self.emit("float m3x3_%d = dot(vT%d.xyz, (%s).xyz);" % (n, n, m))
                self.emit("vec3 m3x3_N = vec3(m3x3_%d, m3x3_%d, m3x3_%d);" % (n - 2, n - 1, n))
                self.emit("vec3 m3x3_E = vec3(vT%d.w, vT%d.w, vT%d.w);" % (n - 2, n - 1, n))
                self.emit("vec3 m3x3_R = 2.0 * dot(m3x3_N, m3x3_E) * m3x3_N - dot(m3x3_N, m3x3_N) * m3x3_E;")
                self.emit("vec4 t%d = %s;" % (n, self.sample(n, "vec4(m3x3_R, 1.0)")))
                self.tex_loaded.add("t%d" % n)
            elif op == "texld":                   # ps.1.4
                dst = ins.dst
                src = ins.srcs[0]
                n_stage = int(dst.reg[1:])        # texld rN samples stage N
                if src.reg.startswith("t"):
                    coord = "vT%s" % src.reg[1:]
                else:
                    coord = self.reg_name(src.reg)
                projective = (src.mod == "dw")
                self.temps.add(dst.reg)
                self.emit("%s = %s;" % (dst.reg, self.sample(n_stage, coord, projective)))
            elif op == "texcrd":                  # ps.1.4: rN.xyz = tM
                dst = ins.dst
                coord = "vT%s" % ins.srcs[0].reg[1:]
                self.temps.add(dst.reg)
                self.write(dst, coord)
            elif op == "texkill":
                self.emit("if (any(lessThan(%s.xyz, vec3(0.0)))) discard;" % self.reg_name(ins.dst.reg))
            # ---- arithmetic (with co-issue pairing) ------------------------
            else:
                pair = None
                if i + 1 < len(instrs) and instrs[i + 1].coissue:
                    pair = instrs[i + 1]
                if pair is None:
                    s = [self.src_expr(x) for x in ins.srcs]
                    lo, hi = self.result_clamp(ins.mods)
                    self.write(ins.dst, self.scale(self.arith(ins, s), ins.mods), lo, hi)
                else:
                    # Evaluate both results before writing either: co-issued
                    # instructions read their sources simultaneously.
                    s0 = [self.src_expr(x) for x in ins.srcs]
                    s1 = [self.src_expr(x) for x in pair.srcs]
                    lo0, hi0 = self.result_clamp(ins.mods)
                    lo1, hi1 = self.result_clamp(pair.mods)
                    self.emit("vec4 co_a = clamp(%s, %s, %s);" % (self.scale(self.arith(ins, s0), ins.mods), lo0, hi0))
                    self.emit("vec4 co_b = clamp(%s, %s, %s);" % (self.scale(self.arith(pair, s1), pair.mods), lo1, hi1))
                    self.write(ins.dst, "co_a")
                    self.write(pair.dst, "co_b")
                    # names must be unique per block
                    self.lines[-4] = self.lines[-4].replace("co_a", "co_a%d" % i)
                    self.lines[-3] = self.lines[-3].replace("co_b", "co_b%d" % i)
                    self.lines[-2] = self.lines[-2].replace("co_a", "co_a%d" % i)
                    self.lines[-1] = self.lines[-1].replace("co_b", "co_b%d" % i)
                    i += 1
            i += 1

    def source(self, name, instrs, alpha_test):
        self.translate(name, instrs)
        body = "\n".join(self.lines)
        temps = "".join("    vec4 %s = vec4(0.0);\n" % t for t in sorted(self.temps))
        samplers = ""
        for n in sorted(self.stages_used):
            samplers += SAMPLER_DECL % {"n": n}
        return PS_TEMPLATE % {"name": name, "version": self.version, "samplers": samplers,
                              "temps": temps, "body": body}


SAMPLER_DECL = """#ifdef A5_CUBE%(n)d
uniform samplerCube s%(n)d;
#define TEX%(n)d(c)  texture(s%(n)d, (c).xyz)
#define TEXP%(n)d(c) texture(s%(n)d, (c).xyz)
#else
uniform sampler2D s%(n)d;
#define TEX%(n)d(c)  texture(s%(n)d, (c).xy)
#define TEXP%(n)d(c) textureProj(s%(n)d, (c).xyw)
#endif
"""

PS_TEMPLATE = """#version 300 es
// %(name)s (%(version)s) -- generated by tools/d3dasm2glsl.py from shaders/d3d/ps/%(name)s.psh
precision mediump float;
uniform vec4 pc[8];
// D3DRS_ALPHATESTENABLE / ALPHAFUNC / ALPHAREF are device state in D3D, so
// they are uniforms here rather than baked per shader (the engine changes the
// reference at run time through SetAlphaRef).  alphaFunc uses D3DCMP values,
// 0 = disabled.
uniform int alphaFunc;
uniform float alphaRef;
%(samplers)sin vec4 vD0;
in vec4 vD1;
in vec4 vT0;
in vec4 vT1;
in vec4 vT2;
in vec4 vT3;
out vec4 fragColor;
bool a5AlphaTestFails(float a)
{
    switch (alphaFunc) {
        case 1: return true;                 // D3DCMP_NEVER
        case 2: return !(a <  alphaRef);     // LESS
        case 3: return !(a == alphaRef);     // EQUAL
        case 4: return !(a <= alphaRef);     // LESSEQUAL
        case 5: return !(a >  alphaRef);     // GREATER
        case 6: return !(a != alphaRef);     // NOTEQUAL
        case 7: return !(a >= alphaRef);     // GREATEREQUAL
        default: return false;               // 0 disabled, 8 ALWAYS
    }
}
void main()
{
%(temps)s%(body)s
    vec4 a5Out = clamp(r0, 0.0, 1.0);
    if (a5AlphaTestFails(a5Out.a)) discard;
    fragColor = a5Out;
}
"""


# ---------------------------------------------------------------------------
#  Driver
# ---------------------------------------------------------------------------
def load_ps_text(name):
    text = open(os.path.join(D3D_DIR, "ps", name + ".psh")).read()
    if "; ---- ps.1.4 variant ----" in text:
        # prefer the ps.1.4 form: it spells projective reads out (t0_dw)
        text = text.split("; ---- ps.1.4 variant ----")[1]
    return text


def translate_vs(name):
    text = open(os.path.join(D3D_DIR, "vs", name + ".vsh")).read()
    _, instrs = parse(text)
    return VertexTranslator().source(name, instrs)


def translate_ps(name):
    version, instrs = parse(load_ps_text(name))
    return PixelTranslator(0, version).source(name, instrs, False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", help="print one shader's translation (vs or ps name)")
    args = ap.parse_args()

    index = json.load(open(os.path.join(D3D_DIR, "index.json")))
    if args.check:
        n = args.check
        if n.startswith("vs"):
            print(translate_vs(n))
        else:
            print(translate_ps(n))
        return

    os.makedirs(os.path.join(OUT_DIR, "vs"), exist_ok=True)
    os.makedirs(os.path.join(OUT_DIR, "ps"), exist_ok=True)
    errors = 0
    for v in index["vertex"]:
        try:
            src = translate_vs(v["name"])
            open(os.path.join(OUT_DIR, "vs", v["name"] + ".vert"), "w").write(src)
        except Exception as e:
            errors += 1
            print("VS %s: %s" % (v["name"], e))
    for p in index["pixel"]:
        try:
            src = translate_ps(p["name"])
            open(os.path.join(OUT_DIR, "ps", p["name"] + ".frag"), "w").write(src)
        except Exception as e:
            errors += 1
            print("PS %s: %s" % (p["name"], e))
    print("translated %d vertex + %d pixel shaders -> %s (%d errors)"
          % (len(index["vertex"]), len(index["pixel"]), OUT_DIR, errors))
    if not errors:
        write_cpp_table(index)
    return 1 if errors else 0


def cpp_string_literal(text):
    out = []
    for line in text.split("\n"):
        out.append('"%s\\n"' % line.replace("\\", "\\\\").replace('"', '\\"'))
    return "\n    ".join(out)


def write_cpp_table(index):
    """shaders/glsl_table.cpp: the GLSL keyed by the FNV-1a hash of the D3D
    assembly text embedded in each bytecode blob.  The shim finds that text in
    the blob it is handed and looks the program up here."""
    path = os.path.join(ANDROID, "shaders", "glsl_table.cpp")
    with open(path, "w") as f:
        f.write("// Generated by tools/d3dasm2glsl.py -- do not edit.\n")
        f.write("// GLSL ES 3.00 for the engine's 155 shaders, keyed by the FNV-1a-64 hash of\n")
        f.write("// the vs.1.1/ps.1.x assembly text inside each D3D bytecode blob's DBUG chunk.\n")
        f.write("#include \"a5_glsl_table.h\"\n\n")
        f.write("const A5GlslEntry a5GlslTable[] = {\n")
        for v in index["vertex"]:
            src = open(os.path.join(OUT_DIR, "vs", v["name"] + ".vert")).read()
            f.write("  { 0x%sULL, %d, \"%s\", 1,\n    %s },\n" % (v["hash"], v["id"], v["name"], cpp_string_literal(src)))
        seen = set()
        for p in index["pixel"]:
            src = open(os.path.join(OUT_DIR, "ps", p["name"] + ".frag")).read()
            for h in (p["hash"], p["hash11"]):
                if h in seen:
                    continue
                seen.add(h)
                f.write("  { 0x%sULL, %d, \"%s\", 0,\n    %s },\n" % (h, p["id"], p["name"], cpp_string_literal(src)))
        f.write("};\n")
        f.write("const int a5GlslTableSize = sizeof(a5GlslTable) / sizeof(a5GlslTable[0]);\n")
    print("wrote %s" % path)


if __name__ == "__main__":
    sys.exit(main())
