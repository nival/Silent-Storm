; vsFullPPSpecular (id 72) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r0, v0, c10
mov oPos, r0
mov r1, r0
mul r1.xy, c7.zw, r1.w
mad r1.xy, r0.xy, c7.xy, r1.xy
mov oT0.xyzw, r1.xyww
mov oT2.xyzw, r1.xyww
mov oD0, c17

