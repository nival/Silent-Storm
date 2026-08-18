; vsTextureAndMirror (id 58) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r0.xyz, v1, c3.xxx, c3.yyy
add r1.xyz, c9.xyz, -v0.xyz
dp3 r1.w, r1, r1
rsq r4.w, r1.w
mul r1.xyz, r1.xyz, r4.w
dp3 r1.w, r1, r0
add r2.w, c1.x, -r1.w
mul r2.w, r2.w, r2.w
mul r2.w, r2.w, r2.w
mul r1.w, r1.w, c2.z
mad oD0.w, r2.w, c14.z, c14.w
mad oT1.xyz, r1.w, r0, -r1.xyz
mov oD0.xyz, c1
mul oT0.xy, v3, c6.xx

