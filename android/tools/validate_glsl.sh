#!/usr/bin/env bash
# Validate the generated GLSL ES 3.00 with the NDK's glslc.  glslc only speaks
# the SPIR-V-targeting dialects, so each shader is checked as an ES 3.10 copy
# with explicit layout locations added -- a syntax/type check, not a byte-exact
# one.  The real test is compiling on a device (the boot harness does that).
set -u
GLSLC="${GLSLC:-$(ls -d "${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"/ndk/*/shader-tools/*/glslc | sort -V | tail -1)}"
HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
fail=0; total=0
for f in "$HERE"/../shaders/glsl/vs/*.vert "$HERE"/../shaders/glsl/ps/*.frag; do
  total=$((total+1))
  case $f in *.vert) st=vert;; *) st=frag;; esac
  python3 - "$f" > /tmp/validate_glsl.$st <<'PY'
import sys, re
s = open(sys.argv[1]).read().replace("#version 300 es", "#version 310 es")
s = re.sub(r"^uniform vec4 vc\[96\];", "layout(location=0) uniform vec4 vc[96];", s, flags=re.M)
s = re.sub(r"^uniform vec4 posFixup;", "layout(location=96) uniform vec4 posFixup;", s, flags=re.M)
s = re.sub(r"^uniform vec4 pc\[8\];", "layout(location=0) uniform vec4 pc[8];", s, flags=re.M)
s = re.sub(r"^uniform float alphaRef;", "layout(location=8) uniform float alphaRef;", s, flags=re.M)
s = re.sub(r"^uniform int alphaFunc;", "layout(location=9) uniform int alphaFunc;", s, flags=re.M)
s = re.sub(r"^uniform sampler(\w+) s(\d);", r"layout(binding=\2) uniform sampler\1 s\2;", s, flags=re.M)
s = re.sub(r"^out vec4 fragColor;", "layout(location=0) out vec4 fragColor;", s, flags=re.M)
for i, n in enumerate(["vD0", "vD1", "vT0", "vT1", "vT2", "vT3"]):
    s = re.sub(r"^(in|out) vec4 %s;" % n, r"layout(location=%d) \1 vec4 %s;" % (i, n), s, flags=re.M)
sys.stdout.write(s)
PY
  if ! "$GLSLC" --target-env=opengl -fshader-stage=$st -c /tmp/validate_glsl.$st -o /dev/null 2>/tmp/validate_glsl.err; then
    fail=$((fail+1)); echo "== $f"; grep -i error /tmp/validate_glsl.err | head -3
  fi
done
echo "glsl validation: $((total-fail))/$total pass"
[ $fail -eq 0 ]
