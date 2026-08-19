/*  game_entry.h -- the game's start-up sequence and frame step, for the
 *  Android main loop.  A port of Game/Main.cpp's WinMain: everything it did
 *  before its message loop is a5_game_init(), one iteration of the loop is
 *  a5_game_step(), and the tail is a5_game_shutdown(). */
#ifndef A5_GAME_ENTRY_H
#define A5_GAME_ENTRY_H
#ifdef __cplusplus
extern "C" {
#endif
/*  Returns 0 on success; on failure pszError (static storage) says what. */
int         a5_game_init( const char **ppszError );
/*  One frame.  Returns 0 when the game asks to exit. */
int         a5_game_step( int bActive );
void        a5_game_shutdown( void );
/*  How many interfaces (screens) the game has on its stack; -1 before start. */
int         a5_game_interface_depth( void );
#ifdef __cplusplus
}
#endif
#endif
