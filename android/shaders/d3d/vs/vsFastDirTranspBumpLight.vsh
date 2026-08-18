; vsFastDirTranspBumpLight (id 31) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r1.xyz, v1, c3.xxx, c3.yyy
mad r2.xyz, v4, c3.xxx, c3.yyy
mad r3.xyz, v5, c3.xxx, c3.yyy
dp3 r0.x, r2, c15
dp3 r0.y, r3, c15
dp3 r0.z, r1, c15
dp3 r0.w, r0, r0
rsq r4.w, r0.w
mul r0.xyz, r0.xyz, r4.w
mov oD1.xyz, c14.xyz
mad oD0.xyz, r0.xyz, c2.xxx, c2.xxx
mul oT0.xy, v3, c6.xx
mul oT1.xy, v3, c6.xx

