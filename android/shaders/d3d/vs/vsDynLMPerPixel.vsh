; vsDynLMPerPixel (id 74) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r1, v0, c10
mov oPos, r1
mul r2.xy, c7.zw, r1.w
mad r1.xy, r1.xy, c7.xy, r2.xy
mov oT1.xyzw, r1.xyww
mad r0.xyz, v1, c3.xxx, c3.yyy
mov oT0.xyz, r0
max r2.xyz, r0, c0
mul r1.xyz, c25, r2.x
mad r1.xyz, c27, r2.y, r1
mad r1.xyz, c29, r2.z, r1
max r3.xyz, -r0, c0
mad r1.xyz, c26, r3.x, r1
mad r1.xyz, c28, r3.y, r1
mad oD0.xyz, c30, r3.z, r1

