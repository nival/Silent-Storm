; vsPointLight (id 41) -- recovered from GfxShaders.cpp DBUG chunk
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
sub r3.xyz, c15, v0
dp3 r4.x, r3, r0
mul r4.y, r4.x, c21.z
mul r4.y, r4.y, c4.z
mul r4.y, r4.y, r4.y
add r4.w, r4.y, c1.x
rcp r4.w, r4.w
mul r4.y, r4.y, r4.y
mad r4.y, r4.y, r4.y, c4.w
rcp r4.y, r4.y
mul r4.y, r4.y, c4.w
mul r4.w, r4.y, r4.w
sge r3.w, r4.x, c0.x
mul r3.w, r3.w, r4.w
mul oD0.xyz, r3.w, c16
dp3 r5.x, r1, r3
dp3 r5.y, r2, r3
mul r5.w, r4.x, c4.z
mul r5.z, r5.w, c2.x
mad r5.xy, r5.xy, c2.xx, r5.zz
mov oT0.xyzw, r5.xyww

