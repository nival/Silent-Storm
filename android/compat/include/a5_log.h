/*
 *  a5_log.h -- one logging call that works on device and on the host.
 *
 *  On Android this is logcat (adb logcat -s SilentStorm).  The port also builds
 *  a headless host target for debugging engine code without a device, where the
 *  same calls go to stderr.
 */
#ifndef A5_LOG_H
#define A5_LOG_H

#ifdef __ANDROID__
#  include <android/log.h>
#  define A5_LOG_TAG "SilentStorm"
#  define A5_PRIORITY_INFO  ANDROID_LOG_INFO
#  define A5_PRIORITY_WARN  ANDROID_LOG_WARN
#  define A5_PRIORITY_ERROR ANDROID_LOG_ERROR
#  define A5_PRIORITY_DEBUG ANDROID_LOG_DEBUG
#else
#  define A5_PRIORITY_DEBUG 3
#  define A5_PRIORITY_INFO  4
#  define A5_PRIORITY_WARN  5
#  define A5_PRIORITY_ERROR 6
#endif

#ifdef __cplusplus
extern "C" {
#endif

void a5_log( int nPriority, const char *pszFormat, ... )
#ifdef __GNUC__
    __attribute__( ( format( printf, 2, 3 ) ) )
#endif
    ;
void a5_log_write( int nPriority, const char *pszMessage );

#ifdef __cplusplus
}
#endif

#endif
