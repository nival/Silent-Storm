; vsCLPointLightShadowed (id 48) -- recovered from GfxShaders.cpp DBUG chunk
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
sub r4.xyz, c15, v0
dp3 r5.x, r4, r0
mul r5.y, r5.x, c21.z
mul r5.y, r5.y, c4.z
mul r5.y, r5.y, r5.y
add r5.w, r5.y, c1.x
rcp r5.w, r5.w
mul r5.y, r5.y, r5.y
mad r5.y, r5.y, r5.y, c4.w
rcp r5.y, r5.y
mul r5.y, r5.y, c4.w
mul r5.w, r5.y, r5.w
sge r4.w, r5.x, c0.x
mul r4.w, r4.w, r5.w
mul oD0.xyz, r4.w, c14
dp3 r6.x, r1, r4
dp3 r6.y, r2, r4
mul r6.w, r5.x, c4.z
mul r6.z, r6.w, c2.x
mad r6.xy, r6.xy, c2.xx, r6.zz
mov oT0.xyzw, r6.xyww
add r3, v0, -c15
mad oD1.xyz, r3, c16.yyy, c16.xxx
mov oT1.xyz, r3
mov oT2.xyz, r3

