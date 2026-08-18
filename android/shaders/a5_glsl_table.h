/*  a5_glsl_table.h -- the generated GLSL for the engine's shaders.
 *  See tools/d3dasm2glsl.py; the table lives in glsl_table.cpp (generated). */
#ifndef A5_GLSL_TABLE_H
#define A5_GLSL_TABLE_H
#include <stdint.h>

struct A5GlslEntry
{
    uint64_t    hash;        /* FNV-1a-64 of the D3D assembly text in the bytecode */
    int         id;          /* the engine's shader id (SVShader/SPShader::nID)     */
    const char *name;        /* vsXxx / psXxx                                        */
    int         isVertex;
    const char *glsl;        /* full source, "#version 300 es" first line           */
};
extern const A5GlslEntry a5GlslTable[];
extern const int a5GlslTableSize;
#endif
