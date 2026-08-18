; vsBumpedMirror (id 62) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r2.xyz, v1, c3.xxx, c3.yyy
mad r3.xyz, v4, c3.xxx, c3.yyy
mad r4.xyz, v5, c3.xxx, c3.yyy
add r0.xyz, c9.xyz, -v0.xyz
dp3 r0.w, r0, r0
rsq r6.w, r0.w
mul r0.xyz, r0.xyz, r6.w
mul oT0.xy, v3, c6.xx
mov r1.x, r3.x
mov r1.y, r4.x
mov r1.z, r2.x
mov r1.w, r0.x
mov oT1, r1
mov r1.x, r3.y
mov r1.y, r4.y
mov r1.z, r2.y
mov r1.w, r0.y
mov oT2, r1
mov r1.x, r3.z
mov r1.y, r4.z
mov r1.z, r2.z
mov r1.w, r0.z
mov oT3, r1

