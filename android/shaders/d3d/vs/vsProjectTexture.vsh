; vsProjectTexture (id 55) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
dp4 r0.x, v0, c14
dp4 r0.y, v0, c15
dp4 r0.w, v0, c17
mov oT0.xyzw, r0.xyww

