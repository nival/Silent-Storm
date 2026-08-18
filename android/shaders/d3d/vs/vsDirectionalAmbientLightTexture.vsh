; vsDirectionalAmbientLightTexture (id 27) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r0.xyz, v1, c3.xxx, c3.yyy
dp3 r1.x, r0, c15
max r1.x, r1.x, c0.x
mul r1, c14, r1.x
add oD0, r1, c16
mul oT0.xy, v3, c6.xx

