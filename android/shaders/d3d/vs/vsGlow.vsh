; vsGlow (id 2) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
mad r1.xyz, v1, c3.xxx, c3.yyy
mul r0, r1.xyzz, c14.xxxw
add r0, v0, r0
m4x4 oPos, r0, c10
mov oD0, c15

