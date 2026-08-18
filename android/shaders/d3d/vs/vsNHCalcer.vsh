; vsNHCalcer (id 28) -- recovered from GfxShaders.cpp DBUG chunk
vs.1.1
dcl_position v0;
dcl_normal v1;
dcl_texcoord0 v3;
dcl_tangent0 v4;
dcl_tangent1 v5;
dcl_texcoord1 v6;
m4x4 oPos, v0, c10
add r0.xyz, c9.xyz, -v0.xyz
dp3 r0.w, r0, r0
rsq r4.w, r0.w
mul r0.xyz, r0.xyz, r4.w
mad r1.xyz, -v0.xyz, c15.www, c15.xyz
dp3 r1.w, r1, r1
rsq r3.w, r1.w
mul r1.xyz, r1.xyz, r3.w
add r2.xyz, r0.xyz, r1.xyz
mad oT0.xyz, v1, c3.xxx, c3.yyy
mov oT1.xyz, r2
mad oT2.xyz, r1, c2.xxx, c2.xxx

