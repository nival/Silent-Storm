/*
 *  input_android.cpp -- the engine's NInput interface (Input/Input.h) from
 *  Android events.
 *
 *  The original (Input/Input.cpp) is DirectInput: it enumerates devices,
 *  gives every key/axis/button an action id, and PumpMessages() drains the
 *  buffered device data into SMessage records that Input/Bind.cpp maps onto
 *  named commands (input.cfg: `bind leftbutton_down 'MOUSE_BUTTON0'`).  This
 *  file keeps that contract -- the same control names, the same message
 *  semantics -- and feeds it from touch and key events instead:
 *
 *   - touch down/up          -> MOUSE_BUTTON0 pressed/released
 *   - two-finger tap         -> MOUSE_BUTTON1 (right button)
 *   - touch position         -> absolute cursor position (a5_set_pointer_position);
 *                               the engine's cursor reads it (see the Cursor.cpp
 *                               rule) instead of integrating MOUSE_AXIS deltas
 *   - hardware keys          -> the named key (ESC, ENTER, arrows, letters...)
 *   - two-finger vertical drag -> MOUSE_AXIS_Z (wheel) for zoom
 *
 *  Nothing here knows about the game; a5_input_* is what android_main calls.
 */
/* Main's own prologue: the same environment its files compile in */
#include "Main/StdAfx.h"
#include "FileIO/Streams.h"
#include "Input/Input.h"
#include "a5_input.h"

#include <android/keycodes.h>
#include <deque>
#include <string>

namespace NInput
{

namespace
{
struct SControl
{
    const char  *pszName;
    EControlType eType;
    int          nAndroidKey;     /* AKEYCODE_*, or -1 for mouse/synthetic */
};

/*  The names are the original table's (Input/Input.cpp) so input.cfg binds
 *  resolve; the ids are the row indices.  Keys the phone cannot produce are
 *  kept so config lines referring to them still parse. */
const SControl CONTROLS[] = {
    { "ESC", CT_KEY, AKEYCODE_ESCAPE },   { "1", CT_KEY, AKEYCODE_1 }, { "2", CT_KEY, AKEYCODE_2 },
    { "3", CT_KEY, AKEYCODE_3 }, { "4", CT_KEY, AKEYCODE_4 }, { "5", CT_KEY, AKEYCODE_5 },
    { "6", CT_KEY, AKEYCODE_6 }, { "7", CT_KEY, AKEYCODE_7 }, { "8", CT_KEY, AKEYCODE_8 },
    { "9", CT_KEY, AKEYCODE_9 }, { "0", CT_KEY, AKEYCODE_0 }, { "-", CT_KEY, AKEYCODE_MINUS },
    { "=", CT_KEY, AKEYCODE_EQUALS }, { "BACKSPACE", CT_KEY, AKEYCODE_DEL }, { "TAB", CT_KEY, AKEYCODE_TAB },
    { "Q", CT_KEY, AKEYCODE_Q }, { "W", CT_KEY, AKEYCODE_W }, { "E", CT_KEY, AKEYCODE_E }, { "R", CT_KEY, AKEYCODE_R },
    { "T", CT_KEY, AKEYCODE_T }, { "Y", CT_KEY, AKEYCODE_Y }, { "U", CT_KEY, AKEYCODE_U }, { "I", CT_KEY, AKEYCODE_I },
    { "O", CT_KEY, AKEYCODE_O }, { "P", CT_KEY, AKEYCODE_P }, { "[", CT_KEY, AKEYCODE_LEFT_BRACKET },
    { "]", CT_KEY, AKEYCODE_RIGHT_BRACKET }, { "ENTER", CT_KEY, AKEYCODE_ENTER }, { "LCTRL", CT_KEY, AKEYCODE_CTRL_LEFT },
    { "A", CT_KEY, AKEYCODE_A }, { "S", CT_KEY, AKEYCODE_S }, { "D", CT_KEY, AKEYCODE_D }, { "F", CT_KEY, AKEYCODE_F },
    { "G", CT_KEY, AKEYCODE_G }, { "H", CT_KEY, AKEYCODE_H }, { "J", CT_KEY, AKEYCODE_J }, { "K", CT_KEY, AKEYCODE_K },
    { "L", CT_KEY, AKEYCODE_L }, { ";", CT_KEY, AKEYCODE_SEMICOLON }, { "'", CT_KEY, AKEYCODE_APOSTROPHE },
    { "`", CT_KEY, AKEYCODE_GRAVE }, { "LSHIFT", CT_KEY, AKEYCODE_SHIFT_LEFT }, { "\\", CT_KEY, AKEYCODE_BACKSLASH },
    { "Z", CT_KEY, AKEYCODE_Z }, { "X", CT_KEY, AKEYCODE_X }, { "C", CT_KEY, AKEYCODE_C }, { "V", CT_KEY, AKEYCODE_V },
    { "B", CT_KEY, AKEYCODE_B }, { "N", CT_KEY, AKEYCODE_N }, { "M", CT_KEY, AKEYCODE_M }, { ",", CT_KEY, AKEYCODE_COMMA },
    { ".", CT_KEY, AKEYCODE_PERIOD }, { "/", CT_KEY, AKEYCODE_SLASH }, { "RSHIFT", CT_KEY, AKEYCODE_SHIFT_RIGHT },
    { "NUM_MULTIPLY", CT_KEY, AKEYCODE_NUMPAD_MULTIPLY }, { "LALT", CT_KEY, AKEYCODE_ALT_LEFT }, { "SPACE", CT_KEY, AKEYCODE_SPACE },
    { "CAPITAL", CT_KEY, AKEYCODE_CAPS_LOCK },
    { "F1", CT_KEY, AKEYCODE_F1 }, { "F2", CT_KEY, AKEYCODE_F2 }, { "F3", CT_KEY, AKEYCODE_F3 }, { "F4", CT_KEY, AKEYCODE_F4 },
    { "F5", CT_KEY, AKEYCODE_F5 }, { "F6", CT_KEY, AKEYCODE_F6 }, { "F7", CT_KEY, AKEYCODE_F7 }, { "F8", CT_KEY, AKEYCODE_F8 },
    { "F9", CT_KEY, AKEYCODE_F9 }, { "F10", CT_KEY, AKEYCODE_F10 }, { "NUM", CT_KEY, AKEYCODE_NUM_LOCK },
    { "SCROLL", CT_KEY, AKEYCODE_SCROLL_LOCK },
    { "NUM_7", CT_KEY, AKEYCODE_NUMPAD_7 }, { "NUM_8", CT_KEY, AKEYCODE_NUMPAD_8 }, { "NUM_9", CT_KEY, AKEYCODE_NUMPAD_9 },
    { "NUM_MINUS", CT_KEY, AKEYCODE_NUMPAD_SUBTRACT }, { "NUM_4", CT_KEY, AKEYCODE_NUMPAD_4 }, { "NUM_5", CT_KEY, AKEYCODE_NUMPAD_5 },
    { "NUM_6", CT_KEY, AKEYCODE_NUMPAD_6 }, { "NUM_PLUS", CT_KEY, AKEYCODE_NUMPAD_ADD }, { "NUM_1", CT_KEY, AKEYCODE_NUMPAD_1 },
    { "NUM_2", CT_KEY, AKEYCODE_NUMPAD_2 }, { "NUM_3", CT_KEY, AKEYCODE_NUMPAD_3 }, { "NUM_0", CT_KEY, AKEYCODE_NUMPAD_0 },
    { "NUM_PERIOD", CT_KEY, AKEYCODE_NUMPAD_DOT }, { "OEM_102", CT_KEY, -1 }, { "F11", CT_KEY, AKEYCODE_F11 },
    { "F12", CT_KEY, AKEYCODE_F12 }, { "F13", CT_KEY, -1 }, { "F14", CT_KEY, -1 }, { "F15", CT_KEY, -1 },
    { "KANA", CT_KEY, -1 }, { "ABNT_C1", CT_KEY, -1 }, { "CONVERT", CT_KEY, -1 }, { "NOCONVERT", CT_KEY, -1 },
    { "YEN", CT_KEY, -1 }, { "ABNT_C2", CT_KEY, -1 }, { "NUM_EQUALS", CT_KEY, AKEYCODE_NUMPAD_EQUALS },
    { "PREV_TRACK", CT_KEY, AKEYCODE_MEDIA_PREVIOUS }, { "AT", CT_KEY, AKEYCODE_AT }, { "COLON", CT_KEY, -1 },
    { "UNDERLINE", CT_KEY, -1 }, { "KANJI", CT_KEY, -1 }, { "STOP", CT_KEY, -1 }, { "AX", CT_KEY, -1 },
    { "UNLABELED", CT_KEY, -1 }, { "NEXT_TRACK", CT_KEY, AKEYCODE_MEDIA_NEXT }, { "NUM_ENTER", CT_KEY, AKEYCODE_NUMPAD_ENTER },
    { "RCTRL", CT_KEY, AKEYCODE_CTRL_RIGHT }, { "MUTE", CT_KEY, AKEYCODE_VOLUME_MUTE }, { "CALCULATOR", CT_KEY, -1 },
    { "PLAY", CT_KEY, AKEYCODE_MEDIA_PLAY_PAUSE }, { "MEDIA_STOP", CT_KEY, AKEYCODE_MEDIA_STOP },
    { "VOL_DOWN", CT_KEY, AKEYCODE_VOLUME_DOWN }, { "VOL_UP", CT_KEY, AKEYCODE_VOLUME_UP }, { "WEB_HOME", CT_KEY, -1 },
    { "NUM_COMMA", CT_KEY, AKEYCODE_NUMPAD_COMMA }, { "NUM_DIVIDE", CT_KEY, AKEYCODE_NUMPAD_DIVIDE }, { "SYSRQ", CT_KEY, AKEYCODE_SYSRQ },
    { "RALT", CT_KEY, AKEYCODE_ALT_RIGHT }, { "PAUSE", CT_KEY, AKEYCODE_BREAK }, { "HOME", CT_KEY, AKEYCODE_MOVE_HOME },
    { "UP", CT_KEY, AKEYCODE_DPAD_UP }, { "PG_UP", CT_KEY, AKEYCODE_PAGE_UP }, { "LEFT", CT_KEY, AKEYCODE_DPAD_LEFT },
    { "RIGHT", CT_KEY, AKEYCODE_DPAD_RIGHT }, { "END", CT_KEY, AKEYCODE_MOVE_END }, { "DOWN", CT_KEY, AKEYCODE_DPAD_DOWN },
    { "PG_DOWN", CT_KEY, AKEYCODE_PAGE_DOWN }, { "INSERT", CT_KEY, AKEYCODE_INSERT }, { "DELETE", CT_KEY, AKEYCODE_FORWARD_DEL },
    { "LWIN", CT_KEY, AKEYCODE_META_LEFT }, { "RWIN", CT_KEY, AKEYCODE_META_RIGHT }, { "APP_MENU", CT_KEY, AKEYCODE_MENU },
    { "POWER", CT_KEY, -1 }, { "SLEEP", CT_KEY, -1 }, { "WAKE", CT_KEY, -1 }, { "WEB_SEARCH", CT_KEY, -1 },
    { "WEB_FAVOR", CT_KEY, -1 }, { "WEB_REFRESH", CT_KEY, -1 }, { "WEB_STOP", CT_KEY, -1 }, { "WEB_FORWARD", CT_KEY, -1 },
    { "WEB_BACK", CT_KEY, -1 }, { "MYCOMPUTER", CT_KEY, -1 }, { "MAIL", CT_KEY, -1 }, { "MEDIA_SELECT", CT_KEY, -1 },
    /* mouse */
    { "MOUSE_AXIS_X", CT_AXIS, -1 }, { "MOUSE_AXIS_Y", CT_AXIS, -1 }, { "MOUSE_AXIS_Z", CT_AXIS, -1 },
    { "MOUSE_BUTTON0", CT_KEY, -1 }, { "MOUSE_BUTTON1", CT_KEY, -1 }, { "MOUSE_BUTTON2", CT_KEY, -1 },
    { "MOUSE_BUTTON3", CT_KEY, -1 }, { "MOUSE_BUTTON4", CT_KEY, -1 }, { "MOUSE_BUTTON5", CT_KEY, -1 },
    { "MOUSE_BUTTON6", CT_KEY, -1 }, { "MOUSE_BUTTON7", CT_KEY, -1 },
};
const int N_CONTROLS = (int)( sizeof( CONTROLS ) / sizeof( CONTROLS[ 0 ] ) );

int ControlByName( const char *pszName )
{
    for ( int i = 0; i < N_CONTROLS; ++i )
        if ( strcmp( CONTROLS[ i ].pszName, pszName ) == 0 )
            return i;
    return -1;
}
int ControlByAndroidKey( int nKey )
{
    for ( int i = 0; i < N_CONTROLS; ++i )
        if ( CONTROLS[ i ].nAndroidKey == nKey )
            return i;
    return -1;
}

std::deque< SMessage > g_pending;     /* filled by a5_input_*, drained by PumpMessages */
std::deque< SMessage > g_messages;    /* handed out by GetMessage */
bool g_bInitialised = false;
int  g_nMouseButton0, g_nMouseButton1, g_nAxisZ;

void Push( int nControl, bool bState, int nParam )
{
    if ( nControl < 0 )
        return;
    SMessage m;
    m.nAction = nControl;
    m.ePOVAxis = PA_UNKNOWN;
    m.cType = CONTROLS[ nControl ].eType;
    m.nParam = nParam;
    m.bState = bState;
    m.tTime = GetTickCount();
    g_pending.push_back( m );
}
}  // namespace

bool InitInput( HWND, bool, int )
{
    g_bInitialised = true;
    g_nMouseButton0 = ControlByName( "MOUSE_BUTTON0" );
    g_nMouseButton1 = ControlByName( "MOUSE_BUTTON1" );
    g_nAxisZ = ControlByName( "MOUSE_AXIS_Z" );
    return true;
}
bool DoneInput() { g_bInitialised = false; return true; }

void PumpMessages( bool bFocus )
{
    if ( !bFocus )
    {
        g_pending.clear();
        return;
    }
    while ( !g_pending.empty() )
    {
        g_messages.push_back( g_pending.front() );
        g_pending.pop_front();
    }
}
bool GetMessage( SMessage *pMsg )
{
    if ( g_messages.empty() )
    {
        /* Like the original: an empty queue yields a CT_TIME message stamped
         * with the current tick.  Bind's GetEvent hands it up as the last
         * (non-)event, and NMainLoop::StepApp takes currentTime from it -- so
         * without this the game clock only advanced when a real input event
         * arrived, and the scene sat still until the cursor moved. */
        pMsg->nAction = -1;
        pMsg->ePOVAxis = PA_UNKNOWN;
        pMsg->cType = CT_TIME;
        pMsg->nParam = 0;
        pMsg->bState = false;
        pMsg->tTime = GetTickCount();
        return false;
    }
    *pMsg = g_messages.front();
    g_messages.pop_front();
    return true;
}
bool GetCharForKey( int nVirtualKey, WCHAR *pwcChar )
{
    /* Text entry (save-game names).  Virtual keys here are the Win32 VK codes
     * the compat layer defines; letters and digits map to themselves. */
    if ( ( nVirtualKey >= '0' && nVirtualKey <= '9' ) || ( nVirtualKey >= 'A' && nVirtualKey <= 'Z' ) || nVirtualKey == ' ' )
    {
        *pwcChar = (WCHAR)nVirtualKey;
        return true;
    }
    return false;
}
bool GetKeyForMessage( const SMessage &mMsg, int *pnVirtualKey )
{
    *pnVirtualKey = 0;
    if ( mMsg.cType != CT_KEY || !mMsg.bState || mMsg.nAction < 0 || mMsg.nAction >= N_CONTROLS )
        return false;
    const char *pszName = CONTROLS[ mMsg.nAction ].pszName;
    if ( strlen( pszName ) == 1 )
        *pnVirtualKey = pszName[ 0 ];
    else if ( strcmp( pszName, "SPACE" ) == 0 ) *pnVirtualKey = VK_SPACE;
    else if ( strcmp( pszName, "ENTER" ) == 0 ) *pnVirtualKey = VK_RETURN;
    else if ( strcmp( pszName, "BACKSPACE" ) == 0 ) *pnVirtualKey = VK_BACK;
    else if ( strcmp( pszName, "ESC" ) == 0 ) *pnVirtualKey = VK_ESCAPE;
    else if ( strcmp( pszName, "TAB" ) == 0 ) *pnVirtualKey = VK_TAB;
    else if ( strcmp( pszName, "HOME" ) == 0 ) *pnVirtualKey = VK_HOME;
    else if ( strcmp( pszName, "END" ) == 0 ) *pnVirtualKey = VK_END;
    else if ( strcmp( pszName, "UP" ) == 0 ) *pnVirtualKey = VK_UP;
    else if ( strcmp( pszName, "DOWN" ) == 0 ) *pnVirtualKey = VK_DOWN;
    else if ( strcmp( pszName, "LEFT" ) == 0 ) *pnVirtualKey = VK_LEFT;
    else if ( strcmp( pszName, "RIGHT" ) == 0 ) *pnVirtualKey = VK_RIGHT;
    else if ( strcmp( pszName, "DELETE" ) == 0 ) *pnVirtualKey = VK_DELETE;
    else if ( strcmp( pszName, "PG_UP" ) == 0 ) *pnVirtualKey = VK_PRIOR;
    else if ( strcmp( pszName, "PG_DOWN" ) == 0 ) *pnVirtualKey = VK_NEXT;
    return *pnVirtualKey != 0;
}
int GetControlID( const string &sCommand ) { return ControlByName( sCommand.c_str() ); }
void GetControlInfo( int nAction, EControlType *pcType, float *pfGranularity )
{
    if ( nAction < 0 || nAction >= N_CONTROLS )
    {
        *pcType = CT_UNKNOWN;
        *pfGranularity = 1;
        return;
    }
    *pcType = CONTROLS[ nAction ].eType;
    *pfGranularity = 1.0f;
}
/* Input recording/playback (the original wrote DirectInput samples to a
 * stream).  Not offered on Android. */
void StartSaveInput( CDataStream * ) {}
void StopSaveInput() {}
void StartEmulateInput( CDataStream * ) {}
void StopEmulateInput() {}

}  // namespace NInput

/* ---- what android_main feeds -------------------------------------------- */
extern "C" void a5_input_key( int nAndroidKeyCode, int bDown )
{
    NInput::Push( NInput::ControlByAndroidKey( nAndroidKeyCode ), bDown != 0, 0 );
}
extern "C" void a5_input_mouse_button( int nButton, int bDown )
{
    NInput::Push( nButton == 0 ? NInput::g_nMouseButton0 : NInput::g_nMouseButton1, bDown != 0, 0 );
}
extern "C" void a5_input_wheel( int nDelta )
{
    NInput::Push( NInput::g_nAxisZ, true, nDelta );
}
