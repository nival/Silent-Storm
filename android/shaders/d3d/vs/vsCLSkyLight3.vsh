; vsCLSkyLight3 (id 26) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r1.xyz, v1, c3.xxx, c3.yyy
dp3 r0.x, r1, c31
dp3 r0.y, r1, c32
dp3 r0.z, r1, c33
mul oD1, c14, r0.xyzz
dp4 oD0.x, v0, c16
dp4 oT0.x, v0, c17
dp4 oT0.y, v0, c18
dp4 oD0.y, v0, c25
dp4 oT1.x, v0, c26
dp4 oT1.y, v0, c27
dp4 oD0.z, v0, c28
dp4 oT2.x, v0, c29
dp4 oT2.y, v0, c30

