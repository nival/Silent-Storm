; vsPerspeciveDepth (id 49) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r0, v0, c10
mov oPos, r0
rcp r0.w, r0.w
mul r0.z, r0.z, r0.w
add oD0, c1.x, -r0.z

