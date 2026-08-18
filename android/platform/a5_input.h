/*  a5_input.h -- what android_main pushes into the engine's NInput layer.  */
#ifndef A5_INPUT_H
#define A5_INPUT_H
#ifdef __cplusplus
extern "C" {
#endif
void a5_input_key( int nAndroidKeyCode, int bDown );
void a5_input_mouse_button( int nButton, int bDown );    /* 0 left, 1 right */
void a5_input_wheel( int nDelta );                       /* +120 per notch, like WM_MOUSEWHEEL */
#ifdef __cplusplus
}
#endif
#endif
