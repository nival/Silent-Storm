; vsPointPPSpecular (id 66) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
mad r0.xyz, v1, c3.xxx, c3.yyy
mad r1.xyz, v4, c3.xxx, c3.yyy
mad r2.xyz, v5, c3.xxx, c3.yyy
m4x4 r6, v0, c10
mov oPos, r6
mul r7.xy, c7.zw, r6.w
mad r6.xy, r6.xy, c7.xy, r7.xy
mov oT0.xyzw, r6.xyww
sub r3.xyz, c15, v0
dp3 r4.x, r3, r0
mul r4.y, r4.x, c21.z
mul r4.y, r4.y, c4.z
mul r4.y, r4.y, r4.y
add r4.w, r4.y, c1.x
rcp r4.w, r4.w
rsq r4.z, r4.w
mul r4.y, r4.y, r4.y
mad r4.y, r4.y, r4.y, c4.w
rcp r4.y, r4.y
mul r4.y, r4.y, c4.w
mul r4.w, r4.y, r4.w
mul oD0, r4.w, c17
dp3 r6.x, r1, r3
dp3 r6.y, r2, r3
mul r6.w, r4.z, c2.z
mad r6.xy, r6.xy, c21.z, r4.zz
mov oT2.xyzw, r6.xyww

