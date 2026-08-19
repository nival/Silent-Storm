; vsBumpedFresnel (id 63) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r4, v0, c10
mov oPos, r4
mul r5.xy, c7.zw, r4.w
mad r4.xy, r4.xy, c7.xy, r5.xy
mov oT1.xyzw, r4.xyww
mad r1.xyz, v1, c3.xxx, c3.yyy
mad r2.xyz, v4, c3.xxx, c3.yyy
mad r3.xyz, v5, c3.xxx, c3.yyy
add r0.xyz, c9.xyz, -v0.xyz
dp3 r0.w, r0, r0
rsq r5.w, r0.w
mul r0.xyz, r0.xyz, r5.w
dp3 oT2.x, r2, r0
dp3 oT2.y, r3, r0
dp3 oT2.z, r1, r0
mul oT0.xy, v3, c6.xx

