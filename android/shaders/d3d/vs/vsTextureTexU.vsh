; vsTextureTexU (id 6) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mul oT0.xy, v3, c6.xx
mad r0.xyz, v4, c3.xxx, c3.yyy
mad r1.xyz, v5, c3.xxx, c3.yyy
dp3 oD0, r0, r1

