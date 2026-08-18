; vsAlienEffect (id 77) -- recovered from GfxShaders.cpp DBUG chunk
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
mov r0.xyzw, r4.xyww
mad r2.xyz, v1, c3.xxx, c3.yyy
add r4.xyz, c9.xyz, -v0.xyz
dp3 r4.w, r4, r4
rsq r7.w, r4.w
mul r4.xyz, r4.xyz, r7.w
dp3 r4.w, r4, r2
add r5.w, c1.x, -r4.w
mul r5.w, r5.w, r5.w
mul r5.w, r5.w, r5.w
mul r4.w, r4.w, c2.z
mad oD0.w, r5.w, c14.z, c14.w
mad oT1.xyz, r4.w, r2, -r4.xyz
add r3.xyz, c9.xyz, -v0.xyz
dp3 r3.w, r3, r3
rsq r5.w, r3.w
mul r3.xyz, r3.xyz, r5.w
dp3 r3.w, r3, r3
rsq r4.w, r3.w
mul r3.xyz, r3.xyz, r4.w
dp3 r1.z, r3, r2
mad r2.xyz, r3, -r1.z, r2
dp3 r1.x, r2, c16
dp3 r1.y, r2, c17
mad r0.xy, r1.xy, r0.w, r0.xy
mov oT0.xyzw, r0

