; vsNHCalcBumped (id 29) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
mad r4.xyz, v1, c3.xxx, c3.yyy
mad r5.xyz, v4, c3.xxx, c3.yyy
mad r6.xyz, v5, c3.xxx, c3.yyy
add r0.xyz, c9.xyz, -v0.xyz
dp3 r0.w, r0, r0
rsq r8.w, r0.w
mul r0.xyz, r0.xyz, r8.w
mad r1.xyz, -v0.xyz, c15.www, c15.xyz
dp3 r1.w, r1, r1
rsq r7.w, r1.w
mul r1.xyz, r1.xyz, r7.w
add r2, r1, r0
dp3 r3.x, r5, r2
dp3 r3.y, r6, r2
dp3 r3.z, r4, r2
mov oT1.xyz, r3
dp3 r3.x, r5, r1
dp3 r3.y, r6, r1
dp3 r3.z, r4, r1
dp3 r3.w, r3, r3
rsq r7.w, r3.w
mul r3.xyz, r3.xyz, r7.w
mad oT2.xyz, r3, c2.xxx, c2.xxx
mul oT0.xy, v3, c6.xx
mov r3.xy, c0
mov r3.z, c1

