/*  lifestudio_stub.cpp -- see compat/include/thirdparty-stubs/LifeStudioHeadAPI.h */
#include "LifeStudioHeadAPI.h"
#include "a5_log.h"

namespace LifeStudioHeadAPI
{

namespace
{
struct SStubTree : IMMTree
{
    IMacroMuscle root;
};
struct SStubAnimator : IAnimator {};
struct SStubSequencer : ISequencer {};

bool g_bWarned = false;
void WarnOnce()
{
    if ( g_bWarned )
        return;
    g_bWarned = true;
    a5_log( A5_PRIORITY_WARN,
            "LifeStudio:HEAD is stubbed - dialogue heads will not animate (see "
            "compat/include/thirdparty-stubs/LifeStudioHeadAPI.h)" );
}
}  // namespace

IMMTree      *IMMTree::Create()                          { WarnOnce(); return new SStubTree; }
void          IMMTree::Destroy()                         { delete this; }
bool          IMMTree::Load( const char * )              { return true; }
IMacroMuscle *IMMTree::RootMacroMuscle()                 { return &static_cast< SStubTree * >( this )->root; }

IAnimator *IAnimator::Create()                           { WarnOnce(); return new SStubAnimator; }
void       IAnimator::Destroy()                          { delete this; }
bool       IAnimator::Load( const char *, int )          { return true; }
void       IAnimator::RegisterMacroMuscle( IMacroMuscle * ) {}
void       IAnimator::ClearAllMacroMuscles()             {}
void       IAnimator::ComputePhysics()                   {}
void       IAnimator::FillUnused( bool )                 {}
void       IAnimator::Process( float *, int )            {}

ISequencer *ISequencer::Create()                         { WarnOnce(); return new SStubSequencer; }
void        ISequencer::Destroy()                        { delete this; }
bool        ISequencer::Load( const char *, int )        { return true; }
void        ISequencer::RegisterMMTree( IMMTree * )      {}
void        ISequencer::RenderMacroMuscles( IAnimator *, int ) {}
int         ISequencer::SequenceTime()                   { return 0; }

}  // namespace LifeStudioHeadAPI
