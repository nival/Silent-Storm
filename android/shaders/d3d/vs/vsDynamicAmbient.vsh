; vsDynamicAmbient (id 14) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r0.xyz, v1, c3.xxx, c3.yyy
max r2.xyz, r0, c0
mul r1.xyz, c25, r2.x
mad r1.xyz, c27, r2.y, r1
mad r1.xyz, c29, r2.z, r1
max r3.xyz, -r0, c0
mad r1.xyz, c26, r3.x, r1
mad r1.xyz, c28, r3.y, r1
mad oD0.xyz, c30, r3.z, r1

