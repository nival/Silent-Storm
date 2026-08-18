; vsTexturedFog (id 54) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r0, v0, c10
mov oPos, r0
mad oT0.xy, v0.xy, c16.xy, c16.zw
mul oT1.xyz, v0.wwz, c15.zwx
mul oT2.xyz, r0.www, c14.zzy

