; vsSpotShadow (id 50) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
m4x4 r0, v0, c14
rcp r1.w, r0.w
mul r1.w, r0.z, r1.w
add oD0, c1.x, -r1.w
mov oT0.xyzw, r0.xyzw

