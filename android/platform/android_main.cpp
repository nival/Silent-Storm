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

#include "boot_harness.h"
#include "gles_present.h"
#include "windows.h"

#define LOG_TAG "SilentStorm"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace {

struct SEngineState
{
    android_app *pApp        = 0;

    EGLDisplay display       = EGL_NO_DISPLAY;
    EGLSurface surface       = EGL_NO_SURFACE;
    EGLContext context       = EGL_NO_CONTEXT;
    int        nWidth        = 0;
    int        nHeight       = 0;

    CBootConsole console;
    SBootReport  report;
    bool         bHarnessRun = false;

    /* Touch scrolling. */
    bool  bTouching          = false;
    float fLastTouchY        = 0.0f;

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

    if ( !pState->console.Init() )
        return false;
    pState->console.SetViewport( nWidth, nHeight );
    return true;
}

void TerminateDisplay( SEngineState *pState )
{
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

int32_t HandleInput( android_app *pApp, AInputEvent *pEvent )
{
    SEngineState *pState = (SEngineState *)pApp->userData;
    if ( AInputEvent_getType( pEvent ) != AINPUT_EVENT_TYPE_MOTION )
        return 0;

    const int32_t nAction = AMotionEvent_getAction( pEvent ) & AMOTION_EVENT_ACTION_MASK;
    const float   fY      = AMotionEvent_getY( pEvent, 0 );

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
    LOGI( "internal files dir: %s", state.szInternalFilesDir.c_str() );

    while ( true )
    {
        int                  nEvents;
        android_poll_source *pSource;

        /* Block when idle: the console is static, so there is no reason to spin.
         * A real game loop would poll with a 0 timeout and render continuously. */
        while ( ALooper_pollOnce( -1, 0, &nEvents, (void **)&pSource ) >= 0 )
        {
            if ( pSource )
                pSource->process( pApp, pSource );
            if ( pApp->destroyRequested )
            {
                TerminateDisplay( &state );
                return;
            }
            if ( state.bTouching )
                break;  /* redraw promptly while dragging */
        }
        DrawFrame( &state );
    }
}
