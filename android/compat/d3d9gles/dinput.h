/*  dinput.h -- DirectInput 8 header stand-in.  Input/Bind.cpp includes it but
 *  uses nothing from it (the action-binding layer is API-neutral); Input.cpp,
 *  the actual DirectInput device code, is never built -- platform/ implements
 *  the NInput interface from Android events instead. */
#ifndef A5_D3D9GLES_DINPUT_H
#define A5_D3D9GLES_DINPUT_H
#include "windows.h"
#endif
