/*  a5_input.h -- what android_main pushes into the engine's NInput layer.  */
#ifndef A5_INPUT_H
#define A5_INPUT_H
#ifdef __cplusplus
extern "C" {
#endif
void a5_input_key( int nAndroidKeyCode, int bDown );
void a5_input_mouse_button( int nButton, int bDown );    /* 0 left, 1 right, 2 middle */
void a5_input_wheel( int nDelta );                       /* +120 per notch, like WM_MOUSEWHEEL */
void a5_input_axis( int nAxis, int nDelta );             /* relative mouse delta: 0 = X, 1 = Y,
                                                            in back-buffer pixels (~DirectInput counts) */
#ifdef __cplusplus
}
#endif
#endif
