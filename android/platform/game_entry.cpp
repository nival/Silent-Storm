/*
 *  game_entry.cpp -- Game/Main.cpp's WinMain, in three pieces the Android
 *  activity can drive.  The order and the calls are the original's; only the
 *  Win32 window (NWinFrame) is gone -- the NativeActivity is the window -- and
 *  the command line is fixed.
 */
/* Main's own prologue: the same environment its files compile in */
#include "Main/StdAfx.h"
#include "game_entry.h"
#include "a5_log.h"

#include "Main/GInit.h"
#include "Main/iMain.h"
#include "Main/GResource.h"
#include "Main/iInterMission.h"
#include "Main/iSaveManager.h"
#include "Main/Sound.h"
#include "Input/Bind.h"
#include "ADOImport/BasicDB.h"
#include "FileIO/Streams.h"
#include "FileIO/BasicChunk1.h"
#include "Misc/StrProc.h"
#include "MiscDll/Commands.h"

namespace
{
bool g_bStarted = false;
}

extern "C" int a5_game_init( const char **ppszError )
{
    *ppszError = 0;
    srand( GetTickCount() );

    NGScene::AddResourceDir( ".\\res" );
    NGScene::RunResourceLoadingThread();

    /* game.db: the whole object database */
    try
    {
        CFileStream f;
        f.OpenRead( "game.db" );
        NDatabase::Serialize( f, CStructureSaver::READ );
    }
    catch ( ... )
    {
        *ppszError = "game.db not found or unreadable";
        return 1;
    }

    /* subsystems.  The HWND is the compat layer's stand-in; nothing reads it. */
    HWND hWnd = (HWND)1;
    if ( !NGfx::Init3D( hWnd ) )
    {
        *ppszError = "NGfx::Init3D failed";
        return 2;
    }
    if ( !NSound::InitSound( hWnd ) )
    {
        *ppszError = "NSound::InitSound failed";
        return 3;
    }
    if ( !NInput::InitInput( hWnd ) )
    {
        *ppszError = "NInput::InitInput failed";
        return 4;
    }

    NGlobal::LoadConfig( ".\\cfg\\autoexec.cfg" );

    /* the original parsed lpCmdLine here; on Android there is none */
    string szCfg( "start.cfg" );

    if ( !NGScene::SetModeFromConfig() )
    {
        *ppszError = "NGScene::SetModeFromConfig failed (no display mode)";
        return 5;
    }
    if ( !NSound::SetModeFromConfig() )
    {
        *ppszError = "NSound::SetModeFromConfig failed";
        return 6;
    }
    NMainLoop::Command( new CICInterMission( szCfg ) );
    g_bStarted = true;
    a5_log( A5_PRIORITY_INFO, "game: initialised, first command queued (%s)", szCfg.c_str() );
    return 0;
}

extern "C" int a5_game_step( int bActive )
{
    if ( !g_bStarted )
        return 0;
    NInput::PumpMessages( bActive != 0 );
    return NMainLoop::StepApp( bActive != 0, bActive != 0 ) ? 1 : 0;
}

extern "C" int a5_game_interface_depth( void )
{
    return g_bStarted ? NMainLoop::GetInterfaceStackDepth() : -1;
}

extern "C" void a5_game_shutdown( void )
{
    if ( !g_bStarted )
        return;
    g_bStarted = false;
    NGlobal::SaveConfig( ".\\cfg\\config.cfg" );
    NMainLoop::DoneInterface();
    NGfx::Done3D();
    NInput::DoneInput();
    NSound::DoneSound();
}
