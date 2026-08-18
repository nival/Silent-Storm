; vsDirectionalLightTexture (id 22) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r1.xyz, v1, c3.xxx, c3.yyy
dp3 r0.x, r1, c15
mul oD0, c14, r0.x
mul oT0.xy, v3, c6.xx

