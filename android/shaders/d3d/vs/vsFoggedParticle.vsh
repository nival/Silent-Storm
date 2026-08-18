; vsFoggedParticle (id 52) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 r0, v0, c10
mov oPos, r0
mov r0.z, v0.z
mul oT0.xy, r0.zw, c14.xy
mul oT1.xy, v3, c6.xx

