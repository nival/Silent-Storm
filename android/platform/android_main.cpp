/*
 *  android_main.cpp -- the Android entry point.
 *
 *  Replaces Game/Main.cpp's WinMain and Game/WinFrame.cpp's window class: the
 *  NativeActivity lifecycle drives EGL surface creation, and touch replaces the
 *  DirectInput mouse.  When the D3D renderer is ported this file keeps its
 *  shape -- the EGL context it creates is the one the GLES backend will render
 *  into (see docs/PORTING.md).
 */
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <android/window.h>
#include <jni.h>
#include <math.h>
#include <mutex>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_harness.h"
#include "gles_present.h"
#include "windows.h"
#include "d3d9.h"
#include "a5_input.h"
#ifdef A5_HAVE_MAIN
#include "game_entry.h"
#endif
#ifdef A5_HAVE_AUDIO
#include "audio_android.h"
#endif

#define LOG_TAG "SilentStorm"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace {

/*  Mode of the activity: the boot console (engine core checks on screen) or
 *  the game itself.  The game runs when Main is linked and the boot checks
 *  passed with game data mounted; otherwise the console stays up and says why. */
enum ERunMode { RUN_CONSOLE, RUN_GAME };

/*  What a two-or-more-finger touch has turned out to be.  UNDECIDED until the
 *  movement clears a threshold (a release there is the two-finger tap = right
 *  click); PANZOOM and TWIST are two-finger modes, ORBIT is the three-finger
 *  drag.  One mode per gesture: the classification never changes mid-gesture
 *  except UNDECIDED -> decided and PANZOOM/TWIST <-> ORBIT on the third
 *  finger, because the pan (button 2 + axes) and the rotation (button 1 +
 *  axes) must not hold their buttons at the same time -- with both held one
 *  axis message would drive both camera binds at once. */
enum EGestureMode { GM_UNDECIDED, GM_PANZOOM, GM_TWIST, GM_ORBIT };

struct SEngineState
{
    android_app *pApp        = 0;
    ERunMode     mode        = RUN_CONSOLE;
    bool         bGameRunning = false;
    bool         bSurfaceAlive = false;

    EGLDisplay display       = EGL_NO_DISPLAY;
    EGLSurface surface       = EGL_NO_SURFACE;
    EGLContext context       = EGL_NO_CONTEXT;
    int        nWidth        = 0;
    int        nHeight       = 0;

    CBootConsole console;
    SBootReport  report;
    bool         bHarnessRun = false;

    /* Touch scrolling (console) / pointer (game). */
    bool  bTouching          = false;
    float fLastTouchY        = 0.0f;
    int   nTouchCount        = 0;
    /*  The game's 1024x768 image is letterboxed into the window, so on a
     *  2520x1080 screen ~40% of it is black border.  A tap there must not
     *  press the mouse button at the cursor's last position -- that clicks
     *  whatever the cursor happens to be over.  False for the whole of a
     *  gesture that began outside the image. */
    bool  bPressInside       = false;

    /*  One finger is the mouse; two fingers are a camera gesture -- pinch is
     *  the wheel (camera_zoom), drag holds the middle button and feeds
     *  MOUSE_AXIS deltas (the PC middle-drag pan), twist holds the right
     *  button and feeds MOUSE_AXIS_X (camera_rotate), a quick tap is the
     *  right click; a third finger switches to orbit -- the full PC
     *  right-button drag (horizontal rotates, vertical tilts).  So that the
     *  first finger's press does not land as a phantom left click the moment
     *  a pinch starts, the press is held back for F_PRESS_DELAY seconds (or
     *  until it moves / lifts) -- a second finger arriving within that window
     *  turns the touch into a gesture with no click ever delivered. */
    bool   bPressPending     = false;    /* left press seen, not yet delivered */
    bool   bPressSent        = false;    /* left button currently held in the engine */
    double fPressTime        = 0.0;
    float  fPressX           = 0.0f;     /* window coords of the pending press */
    float  fPressY           = 0.0f;
    bool   bGesture          = false;    /* two tracked fingers on screen */
    EGestureMode eGestureMode = GM_UNDECIDED;
    bool   bPanning          = false;    /* middle button (2) held in the engine */
    bool   bRotating         = false;    /* right button (1) held in the engine */
    int    nGestureId0       = -1;       /* pointer ids of the two tracked fingers */
    int    nGestureId1       = -1;
    float  fGestX0 = 0, fGestY0 = 0, fGestX1 = 0, fGestY1 = 0;
    float  fGestTravel       = 0.0f;     /* movement so far, for the tap slop */
    float  fGestAngle0       = 0.0f;     /* finger-line angle at gesture start */
    float  fWheelAccum       = 0.0f;     /* fractional wheel param not yet pushed */
    float  fPanAccumX        = 0.0f;     /* fractional axis deltas not yet pushed */
    float  fPanAccumY        = 0.0f;
    float  fRotAccum         = 0.0f;     /* fractional twist axis-X not yet pushed */

    /*  Soft keyboard: raised while the engine draws a focused edit box
     *  (a5_edit_was_active), lowered when the focus goes away. */
    bool   bKeyboardShown    = false;

    std::string szExternalFilesDir;
    std::string szInternalFilesDir;
};

/*  getExternalFilesDir(null) and getFilesDir() through JNI.  These are the only
 *  two paths an app can rely on without runtime permissions, and the port needs
 *  them to know where the user put the game data. */
std::string GetActivityDirectory( android_app *pApp, const char *pszMethod,
                                  bool bTakesArgument )
{
    JNIEnv *pEnv = 0;
    pApp->activity->vm->AttachCurrentThread( &pEnv, 0 );

    std::string szResult;
    jclass activityClass = pEnv->GetObjectClass( pApp->activity->clazz );
    jmethodID method = pEnv->GetMethodID(
        activityClass, pszMethod,
        bTakesArgument ? "(Ljava/lang/String;)Ljava/io/File;" : "()Ljava/io/File;" );

    if ( method )
    {
        jobject file = bTakesArgument
            ? pEnv->CallObjectMethod( pApp->activity->clazz, method, (jstring)0 )
            : pEnv->CallObjectMethod( pApp->activity->clazz, method );
        if ( file )
        {
            jclass fileClass = pEnv->GetObjectClass( file );
            jmethodID getPath = pEnv->GetMethodID( fileClass, "getAbsolutePath",
                                                   "()Ljava/lang/String;" );
            jstring path = (jstring)pEnv->CallObjectMethod( file, getPath );
            if ( path )
            {
                const char *pszPath = pEnv->GetStringUTFChars( path, 0 );
                szResult = pszPath ? pszPath : "";
                pEnv->ReleaseStringUTFChars( path, pszPath );
            }
        }
    }

    pApp->activity->vm->DetachCurrentThread();
    return szResult;
}

/*  The Android soft keyboard, through InputMethodManager.  NativeActivity has
 *  no editable view, so `showSoftInput` on the decor view can be refused by
 *  the IME; the deprecated `toggleSoftInput` is the standing NDK fallback.
 *  Key presses come back as normal key events (the IME has no InputConnection
 *  to talk to, so it falls back to sending them) and reach a5_input_key. */
void ShowSoftKeyboard( android_app *pApp, bool bShow )
{
    JNIEnv *pEnv = 0;
    pApp->activity->vm->AttachCurrentThread( &pEnv, 0 );

    jobject activity = pApp->activity->clazz;
    jclass activityClass = pEnv->GetObjectClass( activity );

    jclass contextClass = pEnv->FindClass( "android/content/Context" );
    jfieldID fidService = pEnv->GetStaticFieldID( contextClass, "INPUT_METHOD_SERVICE", "Ljava/lang/String;" );
    jobject szService = pEnv->GetStaticObjectField( contextClass, fidService );
    jmethodID midGetSystemService = pEnv->GetMethodID( activityClass, "getSystemService",
                                                       "(Ljava/lang/String;)Ljava/lang/Object;" );
    jobject imm = pEnv->CallObjectMethod( activity, midGetSystemService, szService );

    jmethodID midGetWindow = pEnv->GetMethodID( activityClass, "getWindow", "()Landroid/view/Window;" );
    jobject window = pEnv->CallObjectMethod( activity, midGetWindow );
    jclass windowClass = pEnv->FindClass( "android/view/Window" );
    jmethodID midGetDecorView = pEnv->GetMethodID( windowClass, "getDecorView", "()Landroid/view/View;" );
    jobject decorView = pEnv->CallObjectMethod( window, midGetDecorView );

    jclass immClass = pEnv->FindClass( "android/view/inputmethod/InputMethodManager" );
    if ( imm && decorView )
    {
        if ( bShow )
        {
            jmethodID midShow = pEnv->GetMethodID( immClass, "showSoftInput", "(Landroid/view/View;I)Z" );
            const jboolean bShown = pEnv->CallBooleanMethod( imm, midShow, decorView, 0 );
            if ( !bShown )
            {
                jmethodID midToggle = pEnv->GetMethodID( immClass, "toggleSoftInput", "(II)V" );
                pEnv->CallVoidMethod( imm, midToggle, 2 /* SHOW_FORCED */, 0 );
            }
            LOGI( "keyboard: show (showSoftInput=%d)", (int)bShown );
        }
        else
        {
            jclass viewClass = pEnv->FindClass( "android/view/View" );
            jmethodID midGetToken = pEnv->GetMethodID( viewClass, "getWindowToken", "()Landroid/os/IBinder;" );
            jobject token = pEnv->CallObjectMethod( decorView, midGetToken );
            jmethodID midHide = pEnv->GetMethodID( immClass, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z" );
            pEnv->CallBooleanMethod( imm, midHide, token, 0 );
            LOGI( "keyboard: hide" );
        }
    }
    if ( pEnv->ExceptionCheck() )
    {
        pEnv->ExceptionDescribe();
        pEnv->ExceptionClear();
    }
    pApp->activity->vm->DetachCurrentThread();
}

SEngineState *g_pState = 0;
int  HookWindowWidth()  { return g_pState ? g_pState->nWidth : 0; }
int  HookWindowHeight() { return g_pState ? g_pState->nHeight : 0; }
static int g_nPresents = 0;
void HookPresent()
{
    ++g_nPresents;
    if ( g_pState && g_pState->display != EGL_NO_DISPLAY )
        eglSwapBuffers( g_pState->display, g_pState->surface );
}
int  HookSurfaceAlive() { return g_pState && g_pState->bSurfaceAlive ? 1 : 0; }

bool InitDisplay( SEngineState *pState )
{
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };

    EGLDisplay display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
    eglInitialize( display, 0, 0 );

    EGLConfig config;
    EGLint    nConfigs = 0;
    if ( !eglChooseConfig( display, attributes, &config, 1, &nConfigs ) || nConfigs < 1 )
    {
        LOGE( "eglChooseConfig found no ES2 config" );
        return false;
    }

    /* ANativeWindow must use the format the chosen config expects. */
    EGLint nFormat = 0;
    eglGetConfigAttrib( display, config, EGL_NATIVE_VISUAL_ID, &nFormat );
    ANativeWindow_setBuffersGeometry( pState->pApp->window, 0, 0, nFormat );

    EGLSurface surface = eglCreateWindowSurface( display, config, pState->pApp->window, 0 );

    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext( display, config, EGL_NO_CONTEXT, contextAttributes );

    if ( eglMakeCurrent( display, surface, surface, context ) == EGL_FALSE )
    {
        LOGE( "eglMakeCurrent failed" );
        return false;
    }

    EGLint nWidth = 0, nHeight = 0;
    eglQuerySurface( display, surface, EGL_WIDTH, &nWidth );
    eglQuerySurface( display, surface, EGL_HEIGHT, &nHeight );

    pState->display = display;
    pState->surface = surface;
    pState->context = context;
    pState->nWidth  = nWidth;
    pState->nHeight = nHeight;

    LOGI( "EGL surface %dx%d", nWidth, nHeight );
    pState->bSurfaceAlive = true;

    if ( !pState->console.Init() )
        return false;
    pState->console.SetViewport( nWidth, nHeight );
    return true;
}

void TerminateDisplay( SEngineState *pState )
{
    pState->bSurfaceAlive = false;
    pState->console.Shutdown();
    if ( pState->display != EGL_NO_DISPLAY )
    {
        eglMakeCurrent( pState->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        if ( pState->context != EGL_NO_CONTEXT )
            eglDestroyContext( pState->display, pState->context );
        if ( pState->surface != EGL_NO_SURFACE )
            eglDestroySurface( pState->display, pState->surface );
        eglTerminate( pState->display );
    }
    pState->display = EGL_NO_DISPLAY;
    pState->context = EGL_NO_CONTEXT;
    pState->surface = EGL_NO_SURFACE;
}

void DrawFrame( SEngineState *pState )
{
    if ( pState->display == EGL_NO_DISPLAY || !pState->console.IsReady() )
        return;
    pState->console.Render( 0.0 );
    eglSwapBuffers( pState->display, pState->surface );
}

double NowSeconds()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/*  Gesture tuning.  The slop and the pinch step scale with the window so the
 *  gestures feel the same across screen densities. */
const double F_PRESS_DELAY = 0.09;       /* seconds a left press is held back */
float GestureSlop( const SEngineState *pState )
{
    const int nMax = pState->nWidth > pState->nHeight ? pState->nWidth : pState->nHeight;
    return 0.012f * nMax;                /* ~30 px on a 2520-wide screen */
}
float PinchPixelsPerNotch( const SEngineState *pState )
{
    const int nMax = pState->nWidth > pState->nHeight ? pState->nWidth : pState->nHeight;
    return 0.03f * nMax;                 /* ~75 px of finger spread per wheel notch */
}
/*  Twist: how far the line between the two fingers must turn before the
 *  gesture is a rotation rather than a pan/zoom, and how many back-buffer
 *  pixels of MOUSE_AXIS_X one degree of twist feeds into the camera_rotate
 *  bind ('MOUSE_BUTTON1' + 'MOUSE_AXIS_X', the PC right-button drag). */
const float F_TWIST_DECIDE_RAD  = 0.17f;   /* ~10 degrees */
const float F_TWIST_PX_PER_DEG  = 6.0f;
float WrapAngle( float fA )                /* into (-pi, pi] */
{
    while ( fA >  (float)M_PI ) fA -= 2.0f * (float)M_PI;
    while ( fA <= -(float)M_PI ) fA += 2.0f * (float)M_PI;
    return fA;
}

/*  Deliver the held-back left press: called once the touch is known to be a
 *  single-finger press (it moved, lifted, or outlived the delay), never when
 *  a second finger made it a gesture. */
void FlushPendingPress( SEngineState *pState )
{
    if ( !pState->bPressPending )
        return;
    pState->bPressPending = false;
    float fBackX = 0, fBackY = 0;
    if ( !A5D3DWindowToBackBuffer( pState->fPressX, pState->fPressY, &fBackX, &fBackY ) )
        return;
    a5_set_pointer_position( (long)fBackX, (long)fBackY );
    a5_input_mouse_button( 0, 1 );
    pState->bPressSent = true;
}

/*  A gesture MOVE: zoom by the change of the finger distance, pan by the
 *  movement of the midpoint, rotate by the turn of the finger line (twist) or
 *  by the midpoint drag when a third finger is down (orbit).  Nothing is fed
 *  until the movement decides the mode, so a two-finger tap stays a tap. */
void UpdateGesture( SEngineState *pState, AInputEvent *pEvent )
{
    const size_t nPointers = AMotionEvent_getPointerCount( pEvent );
    float fX0 = 0, fY0 = 0, fX1 = 0, fY1 = 0;
    int nFound = 0;
    for ( size_t i = 0; i < nPointers; ++i )
    {
        const int nId = AMotionEvent_getPointerId( pEvent, i );
        if ( nId == pState->nGestureId0 )
        {
            fX0 = AMotionEvent_getX( pEvent, i );
            fY0 = AMotionEvent_getY( pEvent, i );
            nFound |= 1;
        }
        else if ( nId == pState->nGestureId1 )
        {
            fX1 = AMotionEvent_getX( pEvent, i );
            fY1 = AMotionEvent_getY( pEvent, i );
            nFound |= 2;
        }
    }
    if ( nFound != 3 )
        return;

    const float fOldDist = hypotf( pState->fGestX1 - pState->fGestX0, pState->fGestY1 - pState->fGestY0 );
    const float fNewDist = hypotf( fX1 - fX0, fY1 - fY0 );
    const float fDeltaDist = fNewDist - fOldDist;
    const float fDeltaCX = ( fX0 + fX1 - pState->fGestX0 - pState->fGestX1 ) * 0.5f;
    const float fDeltaCY = ( fY0 + fY1 - pState->fGestY0 - pState->fGestY1 ) * 0.5f;
    const float fOldAngle = atan2f( pState->fGestY1 - pState->fGestY0, pState->fGestX1 - pState->fGestX0 );
    const float fNewAngle = atan2f( fY1 - fY0, fX1 - fX0 );
    const float fDeltaAngle = WrapAngle( fNewAngle - fOldAngle );
    pState->fGestX0 = fX0; pState->fGestY0 = fY0;
    pState->fGestX1 = fX1; pState->fGestY1 = fY1;

    if ( pState->eGestureMode == GM_UNDECIDED )
    {
        pState->fGestTravel += fabsf( fDeltaDist ) + fabsf( fDeltaCX ) + fabsf( fDeltaCY );
        /*  A twist barely moves the midpoint or the distance, so the two
         *  tests rarely race; the angle needs a baseline -- with the fingers
         *  close together it is all noise.  Movement up to the decision is
         *  dropped so the camera does not jump. */
        if ( fNewDist > 3.0f * GestureSlop( pState ) &&
             fabsf( WrapAngle( fNewAngle - pState->fGestAngle0 ) ) > F_TWIST_DECIDE_RAD )
            pState->eGestureMode = GM_TWIST;
        else if ( pState->fGestTravel > GestureSlop( pState ) )
            pState->eGestureMode = GM_PANZOOM;
        return;
    }

    if ( pState->eGestureMode == GM_TWIST )
    {
        /* twist -> the horizontal part of the PC right-button drag
         * (input.cfg: -camera_rotate 'MOUSE_BUTTON1' + 'MOUSE_AXIS_X') */
        if ( !pState->bRotating )
        {
            a5_input_mouse_button( 1, 1 );
            pState->bRotating = true;
        }
        pState->fRotAccum += fDeltaAngle * ( 180.0f / (float)M_PI ) * F_TWIST_PX_PER_DEG;
        const int nRot = (int)pState->fRotAccum;
        if ( nRot != 0 ) { pState->fRotAccum -= nRot; a5_input_axis( 0, nRot ); }
        return;
    }

    if ( pState->eGestureMode == GM_ORBIT )
    {
        /* three fingers -> the whole PC right-button drag: horizontal turns
         * (camera_rotate), vertical tilts (camera_pitch).  The button goes
         * down only once the drag clears the slop, so three fingers set down
         * and lifted do not right-click. */
        if ( !pState->bRotating )
        {
            pState->fGestTravel += fabsf( fDeltaCX ) + fabsf( fDeltaCY );
            if ( pState->fGestTravel <= GestureSlop( pState ) )
                return;
            a5_input_mouse_button( 1, 1 );
            pState->bRotating = true;
            return;
        }
        float fScaleX = 1.0f, fScaleY = 1.0f;
        A5D3DBackBufferScale( &fScaleX, &fScaleY );
        pState->fPanAccumX += fDeltaCX * fScaleX;
        pState->fPanAccumY += fDeltaCY * fScaleY;
        const int nDX = (int)pState->fPanAccumX;
        const int nDY = (int)pState->fPanAccumY;
        if ( nDX != 0 ) { pState->fPanAccumX -= nDX; a5_input_axis( 0, nDX ); }
        if ( nDY != 0 ) { pState->fPanAccumY -= nDY; a5_input_axis( 1, nDY ); }
        return;
    }

    /* pinch -> wheel (input.cfg: -camera_zoom 'MOUSE_AXIS_Z'; spreading the
     * fingers is wheel-up = zoom in) */
    pState->fWheelAccum += fDeltaDist * ( 120.0f / PinchPixelsPerNotch( pState ) );
    const int nWheel = (int)pState->fWheelAccum;
    if ( nWheel != 0 )
    {
        pState->fWheelAccum -= nWheel;
        a5_input_wheel( nWheel );
    }

    /* midpoint drag -> the PC middle-button pan (input.cfg: +camera_forward /
     * -camera_strafe on 'MOUSE_BUTTON2' + axis); deltas go in back-buffer
     * pixels so the speed matches the PC mouse at 1024x768 */
    if ( !pState->bPanning )
    {
        a5_input_mouse_button( 2, 1 );
        pState->bPanning = true;
    }
    float fScaleX = 1.0f, fScaleY = 1.0f;
    A5D3DBackBufferScale( &fScaleX, &fScaleY );
    pState->fPanAccumX += fDeltaCX * fScaleX;
    pState->fPanAccumY += fDeltaCY * fScaleY;
    const int nDX = (int)pState->fPanAccumX;
    const int nDY = (int)pState->fPanAccumY;
    if ( nDX != 0 ) { pState->fPanAccumX -= nDX; a5_input_axis( 0, nDX ); }
    if ( nDY != 0 ) { pState->fPanAccumY -= nDY; a5_input_axis( 1, nDY ); }
}

void EndGesture( SEngineState *pState, bool bAllowTap )
{
    if ( !pState->bGesture )
        return;
    pState->bGesture = false;
    if ( pState->bPanning )
    {
        a5_input_mouse_button( 2, 0 );
        pState->bPanning = false;
    }
    if ( pState->bRotating )
    {
        a5_input_mouse_button( 1, 0 );
        pState->bRotating = false;
    }
    if ( bAllowTap && pState->eGestureMode == GM_UNDECIDED )
    {
        /* two-finger tap: a right click where the first finger sat */
        float fBackX = 0, fBackY = 0;
        if ( A5D3DWindowToBackBuffer( pState->fGestX0, pState->fGestY0, &fBackX, &fBackY ) )
        {
            a5_set_pointer_position( (long)fBackX, (long)fBackY );
            a5_input_mouse_button( 1, 1 );
            a5_input_mouse_button( 1, 0 );
        }
    }
    pState->eGestureMode = GM_UNDECIDED;
}

int32_t HandleGameTouch( SEngineState *pState, AInputEvent *pEvent, int32_t nAction )
{
    const float fX = AMotionEvent_getX( pEvent, 0 );
    const float fY = AMotionEvent_getY( pEvent, 0 );

    switch ( nAction )
    {
        case AMOTION_EVENT_ACTION_DOWN:
        {
            pState->nTouchCount = 1;
            float fBackX = 0, fBackY = 0;
            pState->bPressInside = A5D3DWindowToBackBuffer( fX, fY, &fBackX, &fBackY ) != 0;
            if ( pState->bPressInside )
                a5_set_pointer_position( (long)fBackX, (long)fBackY );
            pState->bPressPending = pState->bPressInside;
            pState->bPressSent    = false;
            pState->fPressTime    = NowSeconds();
            pState->fPressX = fX;
            pState->fPressY = fY;
            break;
        }

        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            ++pState->nTouchCount;
            if ( pState->nTouchCount == 2 )
            {
                /* a gesture, not a click: swallow the pending press, or let
                 * go of the button if it already went out */
                pState->bPressPending = false;
                if ( pState->bPressSent )
                {
                    a5_input_mouse_button( 0, 0 );
                    pState->bPressSent = false;
                }
                pState->bGesture      = true;
                pState->eGestureMode  = GM_UNDECIDED;
                pState->fGestTravel   = 0;
                pState->fWheelAccum   = 0;
                pState->fPanAccumX    = 0;
                pState->fPanAccumY    = 0;
                pState->fRotAccum     = 0;
                pState->nGestureId0 = AMotionEvent_getPointerId( pEvent, 0 );
                pState->nGestureId1 = AMotionEvent_getPointerId( pEvent, 1 );
                pState->fGestX0 = AMotionEvent_getX( pEvent, 0 );
                pState->fGestY0 = AMotionEvent_getY( pEvent, 0 );
                pState->fGestX1 = AMotionEvent_getX( pEvent, 1 );
                pState->fGestY1 = AMotionEvent_getY( pEvent, 1 );
                pState->fGestAngle0 = atan2f( pState->fGestY1 - pState->fGestY0,
                                              pState->fGestX1 - pState->fGestX0 );
            }
            else if ( pState->nTouchCount == 3 && pState->bGesture )
            {
                /* a third finger: rotate/tilt (orbit).  The pan button must be
                 * up before the rotate button goes down (see EGestureMode). */
                if ( pState->bPanning )
                {
                    a5_input_mouse_button( 2, 0 );
                    pState->bPanning = false;
                }
                pState->eGestureMode = GM_ORBIT;
                pState->fGestTravel  = 0;
                pState->fWheelAccum  = 0;
                pState->fPanAccumX   = 0;
                pState->fPanAccumY   = 0;
            }
            break;

        case AMOTION_EVENT_ACTION_MOVE:
            if ( pState->bGesture )
            {
                UpdateGesture( pState, pEvent );
                break;
            }
            if ( pState->bPressPending &&
                 ( hypotf( fX - pState->fPressX, fY - pState->fPressY ) > GestureSlop( pState ) ||
                   NowSeconds() - pState->fPressTime > F_PRESS_DELAY ) )
                FlushPendingPress( pState );   /* a drag or a hold, not a nascent gesture */
            {
                float fBackX = 0, fBackY = 0;
                if ( A5D3DWindowToBackBuffer( fX, fY, &fBackX, &fBackY ) )
                    a5_set_pointer_position( (long)fBackX, (long)fBackY );
            }
            break;

        case AMOTION_EVENT_ACTION_POINTER_UP:
        {
            const int nIndex = ( AMotionEvent_getAction( pEvent ) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK )
                               >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            const int nId = AMotionEvent_getPointerId( pEvent, nIndex );
            if ( pState->bGesture && ( nId == pState->nGestureId0 || nId == pState->nGestureId1 ) )
                EndGesture( pState, true );
            else if ( pState->bGesture && pState->eGestureMode == GM_ORBIT )
            {
                /* the third finger left: back to two-finger pan/zoom */
                if ( pState->bRotating )
                {
                    a5_input_mouse_button( 1, 0 );
                    pState->bRotating = false;
                }
                pState->eGestureMode = GM_PANZOOM;
                pState->fWheelAccum  = 0;
                pState->fPanAccumX   = 0;
                pState->fPanAccumY   = 0;
            }
            --pState->nTouchCount;
            break;
        }

        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            EndGesture( pState, nAction == AMOTION_EVENT_ACTION_UP );
            if ( nAction == AMOTION_EVENT_ACTION_CANCEL )
                pState->bPressPending = false;
            /*  A tap while an edit box has focus re-arms the keyboard check:
             *  if the user dismissed the keyboard (Back) and taps the field
             *  again, the game loop shows it again.  When the keyboard is
             *  already up this re-show is a no-op. */
            if ( nAction == AMOTION_EVENT_ACTION_UP && a5_edit_was_active( 400 ) )
                pState->bKeyboardShown = false;
            FlushPendingPress( pState );       /* a quick tap: press and release together */
            if ( pState->bPressSent )
            {
                a5_input_mouse_button( 0, 0 );
                pState->bPressSent = false;
            }
            pState->nTouchCount  = 0;
            pState->bPressInside = false;
            break;

        default:
            break;
    }
    return 1;
}

/*  Runs on the input thread (see StartInputThread); the game loop reads the
 *  touch state machine under the same mutex. */
std::mutex g_inputMutex;

int32_t HandleInput( android_app *pApp, AInputEvent *pEvent )
{
    std::lock_guard< std::mutex > lock( g_inputMutex );
    SEngineState *pState = (SEngineState *)pApp->userData;
    if ( AInputEvent_getType( pEvent ) == AINPUT_EVENT_TYPE_KEY )
    {
        const int32_t nKeyAction = AKeyEvent_getAction( pEvent );
        const int32_t nKey = AKeyEvent_getKeyCode( pEvent );
        if ( pState->mode == RUN_GAME && ( nKeyAction == AKEY_EVENT_ACTION_DOWN || nKeyAction == AKEY_EVENT_ACTION_UP ) )
        {
            /* Back is the game's Escape */
            a5_input_key( nKey == AKEYCODE_BACK ? AKEYCODE_ESCAPE : nKey, nKeyAction == AKEY_EVENT_ACTION_DOWN );
            return 1;
        }
        return 0;
    }
    if ( AInputEvent_getType( pEvent ) != AINPUT_EVENT_TYPE_MOTION )
        return 0;

    const int32_t nAction = AMotionEvent_getAction( pEvent ) & AMOTION_EVENT_ACTION_MASK;
    const float   fX      = AMotionEvent_getX( pEvent, 0 );
    const float   fY      = AMotionEvent_getY( pEvent, 0 );

    if ( pState->mode == RUN_GAME )
        return HandleGameTouch( pState, pEvent, nAction );

    switch ( nAction )
    {
        case AMOTION_EVENT_ACTION_DOWN:
            pState->bTouching   = true;
            pState->fLastTouchY = fY;
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            if ( pState->bTouching )
            {
                pState->console.Scroll( pState->fLastTouchY - fY );
                pState->fLastTouchY = fY;
            }
            return 1;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            pState->bTouching = false;
            return 1;
    }
    return 0;
}

/* ---- input thread --------------------------------------------------------
 *  native_app_glue attaches the AInputQueue to the game thread's looper, so a
 *  long engine call (mission generation, a pass-calc job) leaves touches
 *  unconsumed and after 10s Android declares the app unresponsive ("Input
 *  dispatching timed out" ANR).  The queue is re-attached to this thread
 *  instead (HandleCommand, APP_CMD_INPUT_CHANGED): events are consumed and
 *  queued for the engine immediately, whatever the game thread is doing. */
ALooper *g_pInputLooper = 0;
sem_t    g_inputLooperReady;

int InputQueueCallback( int, int, void *pData )
{
    android_app *pApp   = (android_app *)pData;
    AInputQueue *pQueue = pApp->inputQueue;
    if ( !pQueue )
        return 1;
    AInputEvent *pEvent = 0;
    while ( AInputQueue_getEvent( pQueue, &pEvent ) >= 0 )
    {
        if ( AInputQueue_preDispatchEvent( pQueue, pEvent ) )
            continue;   /* the IME took it */
        const int32_t nHandled = HandleInput( pApp, pEvent );
        AInputQueue_finishEvent( pQueue, pEvent, nHandled );
    }
    /*  When the console is idle the game loop blocks in its looper; wake it
     *  so a touch scrolls the console without waiting for another command. */
    ALooper_wake( pApp->looper );
    return 1;   /* keep the callback installed */
}

void *InputThreadMain( void * )
{
    g_pInputLooper = ALooper_prepare( ALOOPER_PREPARE_ALLOW_NON_CALLBACKS );
    sem_post( &g_inputLooperReady );
    for ( ;; )
        ALooper_pollOnce( -1, 0, 0, 0 );   /* callbacks dispatch inside */
    return 0;
}

void StartInputThread()
{
    sem_init( &g_inputLooperReady, 0, 0 );
    pthread_t thread;
    if ( pthread_create( &thread, 0, InputThreadMain, 0 ) != 0 )
    {
        LOGE( "input: thread failed to start - input stays on the game thread" );
        return;   /* g_pInputLooper stays 0; the glue's path keeps working */
    }
    pthread_detach( thread );
    sem_wait( &g_inputLooperReady );
}

void StartGameIfPossible( SEngineState *pState )
{
#ifdef A5_HAVE_MAIN
    if ( pState->report.nFailed != 0 || !pState->report.bDataMounted )
    {
        LOGI( "game: not starting (checks failed: %d, data mounted: %d) - console stays up",
              pState->report.nFailed, (int)pState->report.bDataMounted );
        return;
    }
    g_pState = pState;
    A5D3DPlatformHooks hooks = { HookWindowWidth, HookWindowHeight, HookPresent, HookSurfaceAlive };
    A5D3DSetPlatformHooks( &hooks );
    /* the game renders through the D3D shim into its own FBO; the console's
     * program state must not leak into it, and vice versa */
    const char *pszError = 0;
    const int nResult = a5_game_init( &pszError );
    if ( nResult != 0 )
    {
        LOGE( "game: init failed (%d): %s", nResult, pszError ? pszError : "?" );
        SBootLine line;
        line.status = BOOT_FAIL;
        line.szText = std::string( "game init failed: " ) + ( pszError ? pszError : "?" );
        line.fSeconds = 0;
        pState->report.lines.push_back( line );
        ++pState->report.nFailed;
        return;
    }
    pState->mode = RUN_GAME;
    pState->bGameRunning = true;
    LOGI( "game: running" );
#else
    (void)pState;
#endif
}

void HandleCommand( android_app *pApp, int32_t nCommand )
{
    SEngineState *pState = (SEngineState *)pApp->userData;

    switch ( nCommand )
    {
        case APP_CMD_INIT_WINDOW:
            if ( pApp->window )
            {
                if ( InitDisplay( pState ) )
                {
                    if ( !pState->bHarnessRun )
                    {
                        /* Run the engine bring-up once, on first window. */
                        pState->report = RunBootHarness(
                            pState->szExternalFilesDir.c_str(),
                            pState->szInternalFilesDir.c_str() );
                        pState->bHarnessRun = true;
                        StartGameIfPossible( pState );
                    }
                    pState->console.SetReport( pState->report );
                    DrawFrame( pState );
                }
            }
            break;

        case APP_CMD_TERM_WINDOW:
            TerminateDisplay( pState );
            break;

        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if ( pState->display != EGL_NO_DISPLAY )
            {
                EGLint nWidth = 0, nHeight = 0;
                eglQuerySurface( pState->display, pState->surface, EGL_WIDTH, &nWidth );
                eglQuerySurface( pState->display, pState->surface, EGL_HEIGHT, &nHeight );
                pState->nWidth  = nWidth;
                pState->nHeight = nHeight;
                pState->console.SetViewport( nWidth, nHeight );
            }
            break;

        case APP_CMD_GAINED_FOCUS:
        case APP_CMD_LOST_FOCUS:
            break;

        /*  The glue has just attached the (new) queue to this thread's looper;
         *  move it to the input thread so touches are consumed even while the
         *  engine holds this thread (see the input-thread comment). */
        case APP_CMD_INPUT_CHANGED:
            if ( pApp->inputQueue && g_pInputLooper )
            {
                AInputQueue_detachLooper( pApp->inputQueue );
                AInputQueue_attachLooper( pApp->inputQueue, g_pInputLooper,
                                          0, InputQueueCallback, pApp );
            }
            break;

        /* Background: silence the mixer's output stream (nothing advances while
         * we are away, so sounds resume where they were). */
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
#ifdef A5_HAVE_AUDIO
            a5_audio_set_active( 0 );
#endif
            break;
        case APP_CMD_RESUME:
#ifdef A5_HAVE_AUDIO
            a5_audio_set_active( 1 );
#endif
            break;
    }
}

}  // namespace

void android_main( android_app *pApp )
{
    /*  Member initialisers rather than memset: this struct holds std::string and
     *  std::vector members, which must not be overwritten with zeroes. */
    SEngineState state;
    state.pApp = pApp;

    pApp->userData     = &state;
    pApp->onAppCmd     = HandleCommand;
    pApp->onInputEvent = HandleInput;

    /*  Fullscreen, like the original game.  Without this the status bar sits on
     *  top of the surface and overlaps the first lines of output. */
    ANativeActivity_setWindowFlags( pApp->activity, AWINDOW_FLAG_FULLSCREEN, 0 );

    state.szExternalFilesDir = GetActivityDirectory( pApp, "getExternalFilesDir", true );
    state.szInternalFilesDir = GetActivityDirectory( pApp, "getFilesDir", false );
    LOGI( "external files dir: %s", state.szExternalFilesDir.c_str() );
    /*  Debug switches: <external files>/env.txt, one NAME=VALUE per line, goes
     *  into the environment (A5_D3D_TRACE, A5_DB_DUMP, ...). */
    if ( FILE *pEnv = fopen( ( state.szExternalFilesDir + "/env.txt" ).c_str(), "r" ) )
    {
        char szLine[ 512 ];
        while ( fgets( szLine, sizeof( szLine ), pEnv ) )
        {
            char *pEq = strchr( szLine, '=' );
            if ( !pEq || szLine[ 0 ] == '#' ) continue;
            *pEq = 0;
            char *pVal = pEq + 1;
            pVal[ strcspn( pVal, "\r\n" ) ] = 0;
            setenv( szLine, pVal, 1 );
            LOGI( "env.txt: %s=%s", szLine, pVal );
        }
        fclose( pEnv );
    }
    LOGI( "internal files dir: %s", state.szInternalFilesDir.c_str() );

    while ( true )
    {
        int                  nEvents;
        android_poll_source *pSource;

        /* Console: block when idle (it is static).  Game: poll and step. */
        const int nTimeout = state.mode == RUN_GAME && state.bSurfaceAlive ? 0 : -1;
        while ( ALooper_pollOnce( nTimeout, 0, &nEvents, (void **)&pSource ) >= 0 )
        {
            if ( pSource )
                pSource->process( pApp, pSource );
            if ( pApp->destroyRequested )
            {
#ifdef A5_HAVE_MAIN
                if ( state.bGameRunning )
                    a5_game_shutdown();
#endif
                TerminateDisplay( &state );
                return;
            }
            if ( state.bTouching || state.mode == RUN_GAME )
                break;
        }
        if ( state.mode == RUN_GAME )
        {
#ifdef A5_HAVE_MAIN
            if ( state.bSurfaceAlive && state.bGameRunning )
            {
                /*  A stationary finger sends no MOVE events, so the held-back
                 *  left press (see SEngineState) is aged out here: after the
                 *  delay a press-and-hold reaches the engine as one. */
                if ( state.bPressPending && NowSeconds() - state.fPressTime > F_PRESS_DELAY )
                    FlushPendingPress( &state );
                /*  Soft keyboard follows the focused-edit-box beacon (a CEdit
                 *  with input focus pings it every frame it draws). */
                const bool bWantKeyboard = a5_edit_was_active( 400 ) != 0;
                if ( bWantKeyboard != state.bKeyboardShown )
                {
                    state.bKeyboardShown = bWantKeyboard;
                    ShowSoftKeyboard( pApp, bWantKeyboard );
                }
                static int nSteps = 0;
                static double fLastLog = 0;
                ++nSteps;
                {
                    struct timespec ts; clock_gettime( CLOCK_MONOTONIC, &ts );
                    const double fNow = ts.tv_sec + ts.tv_nsec * 1e-9;
                    if ( fNow - fLastLog > 5.0 )
                    {
                        fLastLog = fNow;
                        A5D3DFrameStats st;
                        A5D3DGetFrameStats( &st, 1 );
                        LOGI( "game: %d steps, %d presents, interface depth %d; since last: %d draws (%d without program), %d clears",
                              nSteps, g_nPresents, a5_game_interface_depth(), st.nDraws, st.nDrawsNoProgram, st.nClears );
                        if ( getenv( "A5_D3D_SHADERS" ) )
                            LOGI( "game: draws by shader: %s", A5D3DDrawsByShader( 1 ) );
#ifdef A5_HAVE_AUDIO
                        a5_audio_log_stats();
#endif
                    }
                }
                if ( !a5_game_step( 1 ) )
                {
                    LOGI( "game: asked to exit" );
                    a5_game_shutdown();
                    state.bGameRunning = false;
                    state.mode = RUN_CONSOLE;
                    state.console.SetReport( state.report );
                }
            }
#endif
        }
        else
            DrawFrame( &state );
    }
}
