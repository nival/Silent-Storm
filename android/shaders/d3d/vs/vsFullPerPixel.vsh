; vsFullPerPixel (id 69) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r2, v0, c10
mov oPos, r2
mov r3, r2
mul r3.xy, c7.zw, r3.w
mad r3.xy, r2.xy, c7.xy, r3.xy
mov oT1.xyzw, r3.xyww
mov oT2.xyzw, r3.xyww
mad r0.xyz, v1, c3.xxx, c3.yyy
mov oT0.xyz, r0
mul r1, v1.z, c17
add oD0, r1, c16

