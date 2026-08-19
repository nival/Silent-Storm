; vsPrecisePointBump (id 43) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r3.xyz, v1, c3.xxx, c3.yyy
mad r4.xyz, v4, c3.xxx, c3.yyy
mad r5.xyz, v5, c3.xxx, c3.yyy
sub r0.xyz, c15, v0
dp3 r1.x, r0, r3
mul r1.y, r1.x, c21.z
mul r1.y, r1.y, c4.z
mul r1.y, r1.y, r1.y
add r1.w, r1.y, c1.x
rcp r1.w, r1.w
rsq r1.z, r1.w
mul r1.y, r1.y, r1.y
mad r1.y, r1.y, r1.y, c4.w
rcp r1.y, r1.y
mul r1.y, r1.y, c4.w
mul r1.w, r1.y, r1.w
dp3 r6.x, r3, r0
sge r6.x, r6.x, c0.x
mul r6.x, r6.x, r1.w
mul oD0, r6.x, c14
dp3 r8.x, r4, r0
dp3 r8.y, r5, r0
mul r8.w, r1.z, c2.z
mad r8.xy, r8.xy, c21.z, r1.zz
mov oT0.xyzw, r8.xyww
dp3 r7.x, r4, r0
dp3 r7.y, r5, r0
dp3 r7.z, r3, r0
dp3 r7.w, r7, r7
rsq r8.w, r7.w
mul r7.xyz, r7.xyz, r8.w
mov oT1.xyz, r7
mul oT2.xy, v3, c6.xx
mul oT3.xy, v3, c6.xx

