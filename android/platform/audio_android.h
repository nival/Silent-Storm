/*
 *  audio_android.h -- the platform layer's hooks into the audio back end
 *  (platform/audio_android.cpp).  The engine itself talks to NFMSound
 *  (FModSound/FMsound.h); these are the few things Android has to tell the
 *  mixer that the engine never did on Windows.
 */
#ifndef A5_AUDIO_ANDROID_H
#define A5_AUDIO_ANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

/*  Foreground/background.  While inactive the output stream is stopped and
 *  nothing advances, so a sound that was playing when the app went to the
 *  background resumes where it was. */
void a5_audio_set_active( int bActive );

/*  Diagnostics for logcat: voices playing, streams, output rate, underruns. */
void a5_audio_log_stats( void );

#ifdef __cplusplus
}
#endif

#endif
