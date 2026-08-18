; vsLMPDTexture (id 16) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r1, v0, c10
mov oPos, r1
mul r2.xy, c7.zw, r1.w
mad r1.xy, r1.xy, c7.xy, r2.xy
mov oT0.xyzw, r1.xyww
mul oT1.xy, v3, c6.xx
mul r0, v1.z, c17
add oD0, r0, c16

