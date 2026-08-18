; vsFastFullBumpLight (id 71) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r7, v0, c10
mov oPos, r7
mov r8, r7
mul r8.xy, c7.zw, r8.w
mad r8.xy, r7.xy, c7.xy, r8.xy
mov oT2.xyzw, r8.xyww
mov oT3.xyzw, r8.xyww
mad r1.xyz, v1, c3.xxx, c3.yyy
mad r2.xyz, v4, c3.xxx, c3.yyy
mad r3.xyz, v5, c3.xxx, c3.yyy
dp3 r0.x, r2, c15
dp3 r0.y, r3, c15
dp3 r0.z, r1, c15
mov r5.x, r2.z
mov r5.y, r3.z
mov r5.z, r1.z
dp3 r0.w, r0, r0
rsq r7.w, r0.w
mul r0.xyz, r0.xyz, r7.w
dp3 r5.w, r5, r5
rsq r7.w, r5.w
mul r5.xyz, r5.xyz, r7.w
dp3 r6.x, r1, c15
sge oD1.w, r6.x, c0.x
mad oD1.xyz, r5.xyz, c2.xxx, c2.xxx
mad oD0.xyz, r0.xyz, c2.xxx, c2.xxx
mul oT0.xy, v3, c6.xx
mul oT1.xy, v3, c6.xx

