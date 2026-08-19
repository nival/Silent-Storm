/*  stl/_config.h -- STLport's configuration header.
 *
 *  The engine's StdAfx.h includes "stl_user_config.h" (which only sets _STLP_*
 *  macros) and then <stl/_config.h>.  We build against libc++ instead of
 *  STLport, so this file only has to neutralise the STLport spellings the engine
 *  headers reference.
 */
#ifndef A5_COMPAT_STL_CONFIG_H
#define A5_COMPAT_STL_CONFIG_H

#define _STLP_BEGIN_NAMESPACE  namespace std {
#define _STLP_END_NAMESPACE    }
#define _STLP_STD              std
#define _STLP_CALL
#define _STLP_TEMPLATE_NULL    template<>
#define _STLP_CLASS_DECLSPEC
#define __STL_BEGIN_NAMESPACE  namespace std {
#define __STL_END_NAMESPACE    }

#endif
