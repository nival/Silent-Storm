; vsDynLMPDTexture (id 18) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r0.xyz, v1, c3.xxx, c3.yyy
max r3.xyz, r0, c0
mul r2.xyz, c25, r3.x
mad r2.xyz, c27, r3.y, r2
mad r2.xyz, c29, r3.z, r2
max r4.xyz, -r0, c0
mad r2.xyz, c26, r4.x, r2
mad r2.xyz, c28, r4.y, r2
mad oD0.xyz, c30, r4.z, r2
mul oT0.xy, v3, c6.xx

