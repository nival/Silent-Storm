/*
 *  audio_null.cpp -- the engine's audio interface (NFMSound, FModSound/FMsound.h)
 *  implemented without FMOD.
 *
 *  Silent Storm played sound through FMOD 3.x, whose licence is not part of the
 *  source release; FModSound/FMSound.cpp is a thin wrapper over it.  FMsound.h is
 *  the seam: everything in Main talks to NFMSound::*, never to FMOD directly.
 *
 *  This is the silent implementation of that seam.  Every call succeeds and
 *  returns a live handle, streams and 3D sounds report themselves as finished
 *  immediately, and nothing is heard.  It exists so the game can run and be
 *  worked on before an audio back end lands.  The real one belongs in this same
 *  file's place: Oboe (or OpenSL ES) behind the same functions, decoding the
 *  engine's sample data -- Sounds/<id> assets are the raw PCM/OGG payloads --
 *  into a small mixer with the 3D attenuation the SPlayParams describe.
 *
 *  Handles are CObjectBase-derived because Main holds them in CObj<>/CPtr<>.
 */
#include "a5_engine_prologue.h"
#include "FModSound/FMsound.h"
#include "a5_log.h"

namespace NFMSound
{

/* The handle types are declared, not defined, in FMsound.h; the wrapper defined
 * them.  Main only ever holds them by smart pointer and calls back through the
 * NFMSound functions, so empty CObjectBase-derived definitions suffice. */
class CSample2D : public CObjectBase
{
    OBJECT_BASIC_METHODS( CSample2D );
};
class CSample3D : public CObjectBase
{
    OBJECT_BASIC_METHODS( CSample3D );
};
class CSound2D : public CObjectBase
{
    OBJECT_BASIC_METHODS( CSound2D );
};
class CSound3D : public CObjectBase, public ISound3D
{
    OBJECT_BASIC_METHODS( CSound3D );
public:
    virtual void SetPosition( const CVec3 & ) {}
};
class CStream : public CObjectBase
{
    OBJECT_NOCOPY_METHODS( CStream );
};

CDriversInfo drivers;

namespace
{
bool g_bInitialised = false;
bool g_bWarned = false;
void WarnOnce()
{
    if ( g_bWarned )
        return;
    g_bWarned = true;
    a5_log( A5_PRIORITY_WARN, "audio: null back end (silent) - see platform/audio_null.cpp" );
}
}  // namespace

bool SearchDevices()
{
    drivers.clear();
    SDriverInfo info;
    info.sName = "null (silent)";
    info.isHardware3DAccelerated = false;
    info.supportEAXReverb = false;
    info.supportA3DOcclusions = false;
    info.supportA3DReflections = false;
    info.supportReverb = false;
    drivers.push_back( info );
    return true;
}

bool Init( const SStartInfo & )
{
    WarnOnce();
    g_bInitialised = true;
    return true;
}

void Done()                        { g_bInitialised = false; }
bool IsInitialized()               { return g_bInitialised; }
void Update( const SListener & )   {}

CSample2D *LoadSample2D( const void *, int )                          { return new CSample2D; }
CSample3D *LoadSample3D( const void *, int, float, float, int )       { return new CSample3D; }
CSample3D *GetDefault3DSound()
{
    static CObj< CSample3D > pDefault = new CSample3D;
    return pDefault;
}

CSound2D *PlaySound( CSample2D * )                                    { return new CSound2D; }
CSound3D *Play3DSound( const SPlayParams & )                          { return new CSound3D; }
CStream  *PlayStream( const char *, bool )                            { return new CStream; }
CStream  *SwitchStream( CStream *, const char *, bool )               { return new CStream; }

bool IsPlaying( CStream * )                                           { return false; }
bool IsPlaying( CSound2D * )                                          { return false; }
bool IsPlaying( CSound3D * )                                          { return false; }

void FadeOut( CStream *, float )                                      {}
void CancelFadeOut( CStream * )                                       {}
void SetSFXMasterVolume( int )                                        {}
void SetMusicMasterVolume( int )                                      {}
void SetSpeakerType( ESpeakerType )                                   {}

}  // namespace NFMSound

BASIC_REGISTER_CLASS( NFMSound::CSample2D );
BASIC_REGISTER_CLASS( NFMSound::CSample3D );
BASIC_REGISTER_CLASS( NFMSound::CSound2D );
BASIC_REGISTER_CLASS( NFMSound::CSound3D );
BASIC_REGISTER_CLASS( NFMSound::CStream );
