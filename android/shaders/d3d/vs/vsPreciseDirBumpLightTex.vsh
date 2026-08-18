; vsPreciseDirBumpLightTex (id 32) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r0.xyz, v1, c3.xxx, c3.yyy
mad r1.xyz, v4, c3.xxx, c3.yyy
mad r2.xyz, v5, c3.xxx, c3.yyy
dp3 r3.x, r1, c15
dp3 r3.y, r2, c15
dp3 r3.z, r0, c15
dp3 r3.w, r3, r3
rsq r5.w, r3.w
mul r3.xyz, r3.xyz, r5.w
mov oT0.xyz, r3
dp3 r4.x, r0, c15
sge r4.x, r4.x, c0.x
mul oD0, r4.x, c14
mul oT1.xy, v3, c6.xx
mul oT2.xy, v3, c6.xx

