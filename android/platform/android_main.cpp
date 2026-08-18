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

#define LOG_TAG "SilentStorm"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace {

/*  Mode of the activity: the boot console (engine core checks on screen) or
 *  the game itself.  The game runs when Main is linked and the boot checks
 *  passed with game data mounted; otherwise the console stays up and says why. */
enum ERunMode { RUN_CONSOLE, RUN_GAME };

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

int32_t HandleInput( android_app *pApp, AInputEvent *pEvent )
{
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
    {
        /* Touch -> the engine's cursor (absolute, in back-buffer pixels) and
         * mouse buttons: one finger = left button, a second finger = right. */
        float fBackX = 0, fBackY = 0;
        const bool bInside = A5D3DWindowToBackBuffer( fX, fY, &fBackX, &fBackY ) != 0;
        if ( bInside )
            a5_set_pointer_position( (long)fBackX, (long)fBackY );
        switch ( nAction )
        {
            case AMOTION_EVENT_ACTION_DOWN:
                pState->nTouchCount = 1;
                a5_input_mouse_button( 0, 1 );
                break;
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                ++pState->nTouchCount;
                if ( pState->nTouchCount == 2 )
                {
                    a5_input_mouse_button( 0, 0 );      /* the first finger becomes a right click */
                    a5_input_mouse_button( 1, 1 );
                }
                break;
            case AMOTION_EVENT_ACTION_POINTER_UP:
                if ( pState->nTouchCount == 2 )
                    a5_input_mouse_button( 1, 0 );
                --pState->nTouchCount;
                break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_CANCEL:
                if ( pState->nTouchCount == 1 )
                    a5_input_mouse_button( 0, 0 );
                pState->nTouchCount = 0;
                break;
            default:
                break;
        }
        return 1;
    }

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
