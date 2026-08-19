/*
 *  LifeStudioHeadAPI.h -- stub for the LifeStudio:HEAD facial-animation SDK.
 *
 *  Silent Storm used LifeStudio:HEAD (Lifemode Interactive) to animate the
 *  talking heads in dialogue close-ups: an IAnimator per head mesh, an
 *  ISequencer per phoneme/expression sequence, and a shared IMMTree of macro
 *  muscles loaded from tree.mma.  The SDK is proprietary; its headers and
 *  library are not in the source release (only LifeStudioHeadAPI.dll under
 *  bin/), so the engine cannot be built against it.
 *
 *  This stub presents the interface the engine calls (see Main/LSHead.cpp) and
 *  does nothing: Process() leaves the vertex positions exactly as loaded, so
 *  heads render in their neutral pose instead of animating.  Every method the
 *  engine touches is here; nothing else is claimed.
 *
 *  Replacing this with a real implementation means either licensing the SDK
 *  again or writing a macro-muscle deformer that reads the game's animator and
 *  sequencer streams -- a project of its own, and out of scope for the port.
 */
#ifndef A5_STUB_LIFESTUDIOHEADAPI_H
#define A5_STUB_LIFESTUDIOHEADAPI_H

#include <stddef.h>

namespace LifeStudioHeadAPI
{

class IMacroMuscle
{
public:
    virtual ~IMacroMuscle() {}
};

class IMMTree
{
public:
    static IMMTree *Create();
    virtual void Destroy();
    virtual bool Load( const char *pszFileName );
    virtual IMacroMuscle *RootMacroMuscle();
    virtual ~IMMTree() {}
};

class IAnimator
{
public:
    static IAnimator *Create();
    virtual void Destroy();
    /* Loads an animator stream (the game keeps them inside Heads/ assets). */
    virtual bool Load( const char *pData, int nSize );
    virtual void RegisterMacroMuscle( IMacroMuscle *pMuscle );
    virtual void ClearAllMacroMuscles();
    virtual void ComputePhysics();
    virtual void FillUnused( bool bFill );
    /* Writes the deformed vertex positions into pVertices, nStride floats
     * apart.  The stub leaves the caller's buffer untouched. */
    virtual void Process( float *pVertices, int nStride );
    virtual ~IAnimator() {}
};

class ISequencer
{
public:
    static ISequencer *Create();
    virtual void Destroy();
    virtual bool Load( const char *pData, int nSize );
    virtual void RegisterMMTree( IMMTree *pTree );
    virtual void RenderMacroMuscles( IAnimator *pAnimator, int nTimeMs );
    virtual int  SequenceTime();
    virtual ~ISequencer() {}
};

}  // namespace LifeStudioHeadAPI

#endif
