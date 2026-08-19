/*
 *  boot_harness.cpp -- see boot_harness.h.
 *
 *  Every check below runs unmodified 2003 engine code.  The point is not the
 *  reporting; it is that CObjectBase refcounting, the chunk serialiser, the .res
 *  package reader and the Lua 4 VM all execute correctly on ARM64/Android.
 */
/*  a5_engine_prologue.h must come first: engine headers are written assuming
 *  their module's StdAfx.h has already been read. */
#include "a5_engine_prologue.h"

#include "boot_harness.h"
#include "data_mount.h"
#include "a5_package_api.h"

#include "a5_log.h"

#include <stdio.h>

/* Engine headers.  These are the staged originals under gen/. */
#include "Misc/HPTimer.h"
#include "Misc/StrProc.h"
#include "FileIO/Streams.h"
#include "FileIO/BasicChunk1.h"
#include "FileIO/FilesPackage.h"
#include "Script/Script.h"
#include "ADOImport/BasicDB.h"
#include "DBFormat/DataFormat.h"
#include "DBFormat/DataInterface.h"
#include "DBFormat/DataMap.h"
#include "DBFormat/DataObject.h"
#include "DBFormat/DataRPG.h"
#include "DBFormat/DataSound.h"
#include "Misc/RandomGen.h"
#include "Main/GPixelFormat.h"
#include "Image/ImageMMP.h"
#include "dxt_decode.h"
#ifdef __ANDROID__
#include "d3d_selftest.h"
#endif

#define LOGI( ... ) a5_log( A5_PRIORITY_INFO,  __VA_ARGS__ )
#define LOGE( ... ) a5_log( A5_PRIORITY_ERROR, __VA_ARGS__ )

namespace {

class CReport
{
public:
    SBootReport report;

    CReport()
    {
        report.nPassed = report.nFailed = report.nWarnings = 0;
        report.bDataMounted = false;
    }

    void Add( EBootStatus status, double fSeconds, const char *pszFormat, ... )
    {
        char szBuffer[ 512 ];
        va_list args;
        va_start( args, pszFormat );
        vsnprintf( szBuffer, sizeof( szBuffer ), pszFormat, args );
        va_end( args );

        SBootLine line;
        line.status   = status;
        line.szText   = szBuffer;
        line.fSeconds = fSeconds;
        report.lines.push_back( line );

        switch ( status )
        {
            case BOOT_OK:   ++report.nPassed;   LOGI( "[ ok ] %s", szBuffer ); break;
            case BOOT_WARN: ++report.nWarnings; LOGI( "[warn] %s", szBuffer ); break;
            case BOOT_FAIL: ++report.nFailed;   LOGE( "[FAIL] %s", szBuffer ); break;
            case BOOT_HEADING:                  LOGI( "== %s", szBuffer );     break;
            case BOOT_DETAIL:                   LOGI( "       %s", szBuffer ); break;
        }
    }
};

/* A trivial serialisable object, used to exercise CObjectBase + CStructureSaver
 * round-tripping the same way every real game object does. */
class CProbeObject : public CObjectBase
{
    OBJECT_BASIC_METHODS( CProbeObject );
public:
    int              nValue;
    float            fValue;
    std::string      szValue;
    std::vector<int> numbers;

    CProbeObject() : nValue( 0 ), fValue( 0 ) {}

    int operator&( CStructureSaver &f )
    {
        f.Add( 1, &nValue );
        f.Add( 2, &fValue );
        f.Add( 3, &szValue );
        f.Add( 4, &numbers );
        return 0;
    }
};

/* ----- individual checks -------------------------------------------------- */

void CheckTiming( CReport *pReport )
{
    pReport->Add( BOOT_HEADING, 0, "Misc: timing and math" );

    NHPTimer::STime t;
    NHPTimer::GetTime( &t );
    Sleep( 20 );
    const double fMeasured = NHPTimer::GetTimePassed( &t );

    /* The original clock was calibrated rdtsc; ours is CLOCK_MONOTONIC.  If the
     * replacement were wrong this would read as zero or wildly off. */
    if ( fMeasured > 0.010 && fMeasured < 0.500 )
        pReport->Add( BOOT_OK, fMeasured, "HPTimer measures a 20ms sleep as %.1f ms",
                      fMeasured * 1000.0 );
    else
        pReport->Add( BOOT_FAIL, fMeasured, "HPTimer returned %.6f s for a 20ms sleep",
                      fMeasured );

    /* Float2Int replaced x87 fld/fistp with lrintf: it must round, not truncate. */
    const bool bRounds = Float2Int( 2.6f ) == 3 && Float2Int( -2.6f ) == -3 &&
                         Float2Int( 2.4f ) == 2;
    if ( bRounds )
        pReport->Add( BOOT_OK, 0, "Float2Int rounds like the x87 original (2.6 -> 3)" );
    else
        pReport->Add( BOOT_FAIL, 0, "Float2Int(2.6)=%d, expected 3", Float2Int( 2.6f ) );

    if ( Sign( -7 ) == -1 && Sign( 0 ) == 0 && Sign( 7 ) == 1 )
        pReport->Add( BOOT_OK, 0, "Sign<int> matches the replaced asm sequence" );
    else
        pReport->Add( BOOT_FAIL, 0, "Sign<int> is wrong" );
}

void CheckObjectSystem( CReport *pReport )
{
    pReport->Add( BOOT_HEADING, 0, "Misc: object model" );

    CPtr< CProbeObject > pObject = new CProbeObject;
    pObject->nValue = 1234;
    if ( IsValid( pObject ) && pObject->nValue == 1234 )
        pReport->Add( BOOT_OK, 0, "CObjectBase refcounting and CPtr<> work" );
    else
        pReport->Add( BOOT_FAIL, 0, "CPtr<> did not keep the object alive" );
}

void CheckSerialiser( CReport *pReport )
{
    pReport->Add( BOOT_HEADING, 0, "FileIO: chunk serialiser" );

    /* Engine objects have protected destructors on purpose: they are meant to
     * live on the heap under CPtr<>, which is exactly how the game holds them. */
    CPtr< CProbeObject > pWritten = new CProbeObject;
    pWritten->nValue  = 0x5A5A5A5A;
    pWritten->fValue  = 3.14159f;
    pWritten->szValue = "Silent Storm";
    for ( int i = 0; i < 8; ++i )
        pWritten->numbers.push_back( i * i );

    CMemoryStream stream;
    try
    {
        stream.SetWMode();
        {
            CStructureSaver saver( stream, CStructureSaver::WRITE );
            saver.Add( 1, pWritten.GetPtr() );
        }
        const int nBytes = stream.GetSize();

        stream.SetRMode();
        stream.Seek( 0 );
        CPtr< CProbeObject > pRead = new CProbeObject;
        {
            CStructureSaver loader( stream, CStructureSaver::READ );
            loader.Add( 1, pRead.GetPtr() );
        }

        const bool bMatch = pRead->nValue   == pWritten->nValue &&
                            pRead->fValue   == pWritten->fValue &&
                            pRead->szValue  == pWritten->szValue &&
                            pRead->numbers  == pWritten->numbers;
        if ( bMatch )
            pReport->Add( BOOT_OK, 0,
                          "CStructureSaver round-trip of %d bytes is byte-exact", nBytes );
        else
            pReport->Add( BOOT_FAIL, 0, "CStructureSaver round-trip lost data" );
    }
    catch ( const SFileIOError &error )
    {
        pReport->Add( BOOT_FAIL, 0, "serialiser threw: %s", error.szError.c_str() );
    }
    catch ( ... )
    {
        pReport->Add( BOOT_FAIL, 0, "serialiser threw an unknown exception" );
    }
}

void CheckPackages( CReport *pReport, const SDataMountResult &mount )
{
    pReport->Add( BOOT_HEADING, 0, "FileIO: game data packages" );

    if ( !mount.bMounted )
    {
        pReport->Add( BOOT_WARN, 0, "no game data mounted - package checks skipped" );
        return;
    }

    int nOpened = 0, nTotalFiles = 0;
    for ( size_t i = 0; i < mount.packagesFound.size(); ++i )
    {
        /*  Deliberately a bare, root-relative name: that is what engine code
         *  passes (Main/GResource.cpp builds "<resource dir>\\<name>.res"), and
         *  the compat layer resolves it against the mounted data root. */
        const std::string szPath = mount.packagesFound[ i ];

        NHPTimer::STime t;
        NHPTimer::GetTime( &t );
        void *pPackage = A5PackageOpen( szPath.c_str() );
        const double fElapsed = NHPTimer::GetTimePassed( &t );

        if ( !pPackage )
        {
            /*  A package that is only a signature carries no header at all --
             *  Complete/Effects.res ships that way.  That is empty content, not
             *  a broken reader, so it is a warning rather than a failure. */
            CFileStream probe;
            const bool bEmpty = probe.TryOpenRead( szPath.c_str() ) && probe.GetSize() < 8;
            if ( bEmpty )
                pReport->Add( BOOT_WARN, 0, "%s: empty package (signature only)",
                              mount.packagesFound[ i ].c_str() );
            else
                pReport->Add( BOOT_FAIL, 0, "%s: could not open",
                              mount.packagesFound[ i ].c_str() );
            continue;
        }

        const int nFiles = A5PackageGetFileCount( pPackage );
        ++nOpened;
        nTotalFiles += nFiles;
        pReport->Add( BOOT_OK, fElapsed, "%s: %d entries",
                      mount.packagesFound[ i ].c_str(), nFiles );

        /* Read the first entry back so the whole path -- header table, offset
         * lookup, CPackageStream buffering -- is actually exercised. */
        if ( nFiles > 0 )
        {
            int nFileID = 0;
            if ( A5PackageGetFileIDs( pPackage, &nFileID, 1 ) == 1 )
            {
                const int nSize = A5PackageGetFileSize( pPackage, nFileID );
                std::vector< unsigned char > buffer( nSize > 0 ? nSize : 1 );
                const int nRead = A5PackageReadFile( pPackage, nFileID, &buffer[ 0 ],
                                                     (int)buffer.size() );
                if ( nRead == nSize && nSize > 0 )
                    pReport->Add( BOOT_DETAIL, 0, "  entry %d: read %d bytes, first byte 0x%02X",
                                  nFileID, nRead, buffer[ 0 ] );
                else
                    pReport->Add( BOOT_FAIL, 0, "  entry %d: read %d of %d bytes",
                                  nFileID, nRead, nSize );
            }
        }
        A5PackageClose( pPackage );
    }

    if ( nOpened == 0 )
        pReport->Add( BOOT_WARN, 0, "no .res packages present in the data root" );
    else
        pReport->Add( BOOT_OK, 0, "%d packages opened, %d entries total",
                      nOpened, nTotalFiles );
}

void CheckLooseAssets( CReport *pReport, const SDataMountResult &mount )
{
    if ( !mount.bMounted || mount.assetDirsFound.empty() )
        return;

    pReport->Add( BOOT_HEADING, 0, "FileIO: loose asset directories" );

    /* The dev-mode data layout stores each asset as a file named after its
     * numeric ID.  Open one through CFileStream -- the same class the engine
     * uses -- to prove the path resolver and stream buffering work. */
    for ( size_t i = 0; i < mount.assetDirsFound.size() && i < 4; ++i )
    {
        const std::string szDirectory = mount.assetDirsFound[ i ];
        /* Scripts/ holds named .l sources rather than numbered assets; it gets
         * its own check further down. */
        if ( szDirectory == "Scripts" )
            continue;
        bool bRead = false;

        for ( int nFileID = 1; nFileID <= 64 && !bRead; ++nFileID )
        {
            char szPath[ 512 ];
            /* Deliberately a Windows-style path: this is what engine code builds. */
            snprintf( szPath, sizeof( szPath ), "%s\\%d", szDirectory.c_str(), nFileID );

            CFileStream file;
            if ( !file.TryOpenRead( szPath ) )
                continue;
            const int nSize = file.GetSize();
            if ( nSize <= 0 )
                continue;

            unsigned char header[ 8 ] = { 0 };
            file.Seek( 0 );
            file.Read( header, nSize < 8 ? nSize : 8 );
            pReport->Add( BOOT_OK, 0, "%s\\%d: %d bytes, chunk id %d",
                          szDirectory.c_str(), nFileID, nSize, (int)header[ 0 ] );
            bRead = true;
        }
        if ( !bRead )
            pReport->Add( BOOT_WARN, 0, "%s: no numbered files in 1..64",
                          szDirectory.c_str() );
    }
}

/*  Runs one of the game's own script files.  Complete/Scripts/*.l are plain
 *  Lua 4 sources (constants, helper functions) that the shipping game loads at
 *  start-up, so this is the first piece of real game *logic* the port executes. */
void CheckGameScripts( CReport *pReport, const SDataMountResult &mount )
{
    if ( !mount.bMounted )
        return;

    const char *SCRIPT_NAMES[] = { "Constants.l", "Common.l", "TriggersManager.l" };

    pReport->Add( BOOT_HEADING, 0, "Script: the game's own Lua sources" );

    for ( size_t i = 0; i < sizeof( SCRIPT_NAMES ) / sizeof( SCRIPT_NAMES[ 0 ] ); ++i )
    {
        char szPath[ 512 ];
        snprintf( szPath, sizeof( szPath ), "Scripts\\%s", SCRIPT_NAMES[ i ] );

        CFileStream file;
        if ( !file.TryOpenRead( szPath ) )
        {
            pReport->Add( BOOT_WARN, 0, "%s: not present", SCRIPT_NAMES[ i ] );
            continue;
        }

        const int nSize = file.GetSize();
        std::vector< char > source( nSize + 1 );
        file.Seek( 0 );
        file.Read( &source[ 0 ], nSize );
        source[ nSize ] = 0;

        NHPTimer::STime t;
        NHPTimer::GetTime( &t );

        Script script( true );
        const int nResult = script.DoString( &source[ 0 ] );
        script.ExecuteThreads();   /* see the note in CheckScripting */
        const double fElapsed = NHPTimer::GetTimePassed( &t );

        if ( nResult == LUA_NOERR )
            pReport->Add( BOOT_OK, fElapsed, "%s: %d bytes executed",
                          SCRIPT_NAMES[ i ], nSize );
        else
            pReport->Add( BOOT_FAIL, fElapsed, "%s: %s", SCRIPT_NAMES[ i ],
                          ErrorToString( nResult ) );

        /* Constants.l defines named constants the rest of the game reads back;
         * checking one proves the values really landed in the VM. */
        if ( nResult == LUA_NOERR && strcmp( SCRIPT_NAMES[ i ], "Constants.l" ) == 0 )
        {
            Script::Object pose = script.GetGlobal( "POSE_RUN" );
            if ( !pose.IsNil() && pose.GetInteger() == 3 )
                pReport->Add( BOOT_DETAIL, 0, "  POSE_RUN = %d, as defined in the file",
                              pose.GetInteger() );
            else
                pReport->Add( BOOT_FAIL, 0, "  POSE_RUN did not read back as 3" );
        }
    }
}

/*  The whole object database.  Game/Main.cpp does exactly this at start-up:
 *  open game.db, NDatabase::Serialize( f, READ ).  Every record class in
 *  DBFormat/ deserialises itself through operator&, and every cross-record
 *  reference is resolved through CDBPtr -- so this is DBFormat, db_retail,
 *  the chunk serialiser and the class factory all working together over
 *  3.3 MB (Data/) or 34 MB (Complete/) of real data. */
void CheckGameDatabase( CReport *pReport, const SDataMountResult &mount )
{
    pReport->Add( BOOT_HEADING, 0, "DBFormat: game.db object database" );

    if ( !mount.bMounted )
    {
        pReport->Add( BOOT_WARN, 0, "no game data mounted - database check skipped" );
        return;
    }
    if ( !mount.bHasGameDb )
    {
        pReport->Add( BOOT_WARN, 0, "game.db not present in the data root" );
        return;
    }

    /*  Format note.  This source snapshot (January 2003) has two database
     *  back ends and every game.db in the repository (Data/, Complete/,
     *  Versions/) is in neither's native layout: they were written by a later
     *  build as generic column-store tables (class 0xA1843130).  The Android
     *  build's platform/db_retail.cpp reads that layout and runs the record
     *  classes' own ADO-style Import() over it, so the load below is the
     *  real thing. */
    NHPTimer::STime t;
    NHPTimer::GetTime( &t );
    a5_serializer_reset_unknown_types();
    try
    {
        CFileStream file;
        file.OpenRead( "game.db" );          /* root-relative, like the original */
        const int nBytes = file.GetSize();
        NDatabase::Serialize( file, CStructureSaver::READ );
        const double fElapsed = NHPTimer::GetTimePassed( &t );
        pReport->Add( BOOT_OK, fElapsed, "game.db: %d bytes parsed", nBytes );
    }
    catch ( const SFileIOError &error )
    {
        pReport->Add( BOOT_FAIL, NHPTimer::GetTimePassed( &t ), "game.db: %s",
                      error.szError.c_str() );
        return;
    }
    catch ( ... )
    {
        pReport->Add( BOOT_FAIL, NHPTimer::GetTimePassed( &t ),
                      "game.db: unknown exception during load" );
        return;
    }

    /*  Count what was loaded, table by table.  The table registry maps a
     *  numeric ID to a table; there is no name index in the fake DB layer, so
     *  a handful of well-known IDs are named here for the report. */
    struct STableName { int nID; const char *pszName; };
    const STableName KNOWN[] = {
        { 45, "Strings" }, { 46, "Textures" }, { 40, "Sounds" }, { 30, "Units" },
    };
    int nTablesSeen = 0, nRecordsSeen = 0;
    for ( int nTableID = 0; nTableID < 200; ++nTableID )
    {
        CDBTableBase *pTable = NDatabase::GetTable( nTableID );
        if ( !pTable )
            continue;
        ++nTablesSeen;
        int nRecords = 0;
        /* CDBIteratorBase's constructor is protected; the typed iterator over
         * the CDBRecord base counts records regardless of the concrete type. */
        for ( CDBIterator<CDBRecord> it( *static_cast<CDBTable<CDBRecord>*>( pTable ) );
              it.MoveNext(); )
            ++nRecords;
        nRecordsSeen += nRecords;
        for ( size_t i = 0; i < sizeof( KNOWN ) / sizeof( KNOWN[ 0 ] ); ++i )
            if ( KNOWN[ i ].nID == nTableID )
                pReport->Add( BOOT_DETAIL, 0, "  table %d (%s): %d records",
                              nTableID, KNOWN[ i ].pszName, nRecords );
    }
    int nLastUnknownType = 0;
    const int nUnknown = a5_serializer_unknown_types( &nLastUnknownType );
    if ( nUnknown > 0 )
    {
        pReport->Add( BOOT_WARN, 0,
                      "%d objects in the file are of unregistered type 0x%08X - the "
                      "database was written by a later build than this source (see "
                      "docs/PORTING.md); %d tables registered, records not loadable",
                      nUnknown, (unsigned)nLastUnknownType, nTablesSeen );
        return;
    }
    if ( nTablesSeen > 0 && nRecordsSeen > 0 )
        pReport->Add( BOOT_OK, 0, "%d tables, %d records in memory",
                      nTablesSeen, nRecordsSeen );
    else
        pReport->Add( BOOT_FAIL, 0, "database parsed but holds no records" );

    /*  The Strings table is the localised game text, stored as UTF-16.  If the
     *  wide-string port is right these read back as text; if the character
     *  width were wrong they would be interleaved garbage.  Convert to UTF-8 for
     *  the log and check the result contains letters, not just punctuation. */
    CDBTable<NDb::CString> *pStrings = NDatabase::GetTable<NDb::CString>();
    if ( pStrings )
    {
        int nShown = 0, nNonAscii = 0, nTotal = 0;
        for ( CDBIterator<NDb::CString> it( *pStrings ); it.MoveNext(); )
        {
            const NDb::CString *pString = it.Get();
            if ( !pString )
                continue;
            ++nTotal;
            for ( size_t k = 0; k < pString->szStr.size(); ++k )
                if ( pString->szStr[ k ] >= 0x80 )
                    ++nNonAscii;
            if ( nShown < 3 && pString->szStr.size() >= 8 && pString->szStr.size() < 60 )
            {
                char szUtf8[ 256 ];
                const int n = WideCharToMultiByte( CP_UTF8, 0, pString->szStr.c_str(),
                                                   (int)pString->szStr.size(), szUtf8,
                                                   sizeof( szUtf8 ) - 1, 0, 0 );
                szUtf8[ n < 0 ? 0 : n ] = 0;
                pReport->Add( BOOT_DETAIL, 0, "  string %d: \"%s\"",
                              pString->GetRecordID(), szUtf8 );
                ++nShown;
            }
        }
        if ( nTotal > 0 )
            pReport->Add( BOOT_OK, 0,
                          "Strings table: %d entries, UTF-16 decoded (%d non-ASCII chars)",
                          nTotal, nNonAscii );
        else
            pReport->Add( BOOT_DETAIL, 0, "  Strings table present, no records" );
    }
    else
        pReport->Add( BOOT_FAIL, 0, "GetTable<CString>() returned null - type registry broken" );

    /*  The object chain a map is built from: TemplVariant -> FinalElement ->
     *  PlacableObject -> ObjectTemplate -> (random) Object -> ContainerModel
     *  -> Model -> Geometry.  Walk it for the main-menu variant (template 2425,
     *  iMainMenu.cpp) so a broken link shows up here rather than as an empty
     *  scene on the device. */
    const struct { int nID; const char *pszName; } TEMPLATES[] = { { 2425, "main menu" }, { 2999, "allies hero screen" } };
    for ( size_t t = 0; t < sizeof( TEMPLATES ) / sizeof( TEMPLATES[ 0 ] ); ++t )
    {
        NDb::CTemplate *pTemplate = NDb::GetTemplate( TEMPLATES[ t ].nID );
        NDb::CTemplVariant *pVar = 0;
        if ( pTemplate && !pTemplate->variants.empty() )
            pVar = pTemplate->variants[ 0 ];
        if ( !pVar )
            pReport->Add( BOOT_WARN, 0, "template %d (%s) has no variants", TEMPLATES[ t ].nID, TEMPLATES[ t ].pszName );
        else
        {
            int nElements = (int)pVar->pFinalElements.size(), nPlacable = 0, nTemplates = 0, nObjects = 0, nModels = 0, nGeometry = 0, nSkinned = 0;
            SRandomSeed seed;
            SRand rnd( seed );
            std::vector<int> noFlags;
            std::string szFirstMissing;
            for ( int i = 0; i < nElements; ++i )
            {
                NDb::CFinalElement *pFin = pVar->pFinalElements[ i ];
                if ( !pFin || !IsValid( pFin->pObject ) ) { if ( szFirstMissing.empty() ) szFirstMissing = "PlacableObject"; continue; }
                ++nPlacable;
                if ( !IsValid( pFin->pObject->pObject ) ) { if ( szFirstMissing.empty() ) szFirstMissing = "ObjectTemplate (PlacableID link)"; continue; }
                ++nTemplates;
                CPtr<NDb::CObject> pObject( pFin->pObject->pObject->CreateObject( &rnd, noFlags ) );
                if ( !pObject ) { if ( szFirstMissing.empty() ) szFirstMissing = "Object (ObjectTemplates.variants empty)"; continue; }
                ++nObjects;
                if ( !IsValid( pObject->pModels[ 0 ] ) ) { if ( szFirstMissing.empty() ) szFirstMissing = "ContainerModel (Objects.Model0)"; continue; }
                ++nModels;
                if ( IsValid( pObject->pModels[ 0 ]->pModel ) && IsValid( pObject->pModels[ 0 ]->pModel->pGeometry ) )
                {
                    ++nGeometry;
                    if ( IsValid( pObject->pModels[ 0 ]->pModel->pSkeleton ) )
                        ++nSkinned;
                }
                else if ( szFirstMissing.empty() ) szFirstMissing = "Model/Geometry (ContainerModels.ModelID -> Models.GeometryID)";
            }
            if ( nElements > 0 && nGeometry == nElements )
                pReport->Add( BOOT_OK, 0, "template %d (%s): %d final elements, all resolve to models with geometry (%d skinned)", TEMPLATES[ t ].nID, TEMPLATES[ t ].pszName, nElements, nSkinned );
            else
                pReport->Add( nElements > 0 ? BOOT_FAIL : BOOT_WARN, 0, "template %d (%s): %d final elements -> %d placable, %d templates, %d objects, %d container models, %d with geometry; first break: %s",
                              TEMPLATES[ t ].nID, TEMPLATES[ t ].pszName, nElements, nPlacable, nTemplates, nObjects, nModels, nGeometry, szFirstMissing.empty() ? "-" : szFirstMissing.c_str() );
        }
    }

    /*  Cross-references and the retail importer's per-row Import(): the main
     *  menu is UI container 347 (iMainMenu.cpp), and its controls attach
     *  themselves to it in CUIControl::Import() through the UIContainerID
     *  column.  The menu code then looks up controls by their IDText. */
    {
        NDb::CUIContainer *pMenu = NDb::GetUIContainer( 347 );
        if ( !pMenu )
            pReport->Add( BOOT_FAIL, 0, "UI container 347 (main menu) not in the database" );
        else
        {
            const char *NEEDED[] = { "credits", "options", "campaign", "load", "quit", "clientview" };
            int nFound = 0;
            std::string szFirst;
            for ( size_t i = 0; i < sizeof( NEEDED ) / sizeof( NEEDED[ 0 ] ); ++i )
                for ( size_t c = 0; c < pMenu->controls.size(); ++c )
                    if ( pMenu->controls[ c ] && pMenu->controls[ c ]->szID == NEEDED[ i ] ) { ++nFound; break; }
            for ( size_t c = 0; c < pMenu->controls.size() && szFirst.size() < 60; ++c )
                if ( pMenu->controls[ c ] ) { if ( !szFirst.empty() ) szFirst += ","; szFirst += pMenu->controls[ c ]->szID; }
            if ( nFound == 6 )
                pReport->Add( BOOT_OK, 0, "UI container 347 (main menu): %dx%d, %d controls, all 6 the menu needs by name",
                              pMenu->nWidth, pMenu->nHeight, (int)pMenu->controls.size() );
            else if ( pMenu->controls.empty() )
                pReport->Add( BOOT_FAIL, 0, "UI container 347 (main menu): %dx%d but no controls attached - UIControls import broken",
                              pMenu->nWidth, pMenu->nHeight );
            else
                /* the retail data's menu is laid out differently from what this
                 * source's iMainMenu.cpp expects (view/logo/lines/version instead
                 * of named buttons) - a data-vs-source difference, see PORTING.md */
                pReport->Add( BOOT_WARN, 0, "UI container 347 (main menu): %dx%d, %d controls (%s); %d/6 of the names iMainMenu.cpp expects - retail UI layout differs from this source",
                              pMenu->nWidth, pMenu->nHeight, (int)pMenu->controls.size(), szFirst.c_str(), nFound );
        }
    }
}

/*  Textures.  Every texture the game ships is an MMP container holding DXT
 *  mip levels; Textures/<id> files are the raw MMPs.  Load a few through the
 *  engine's own NImage::LoadImageMMP, then decode the top mip in software and
 *  compare its mean colour with the dwAverageColor the tools stored in the
 *  header -- if the loader or the decoder were wrong the two would not agree. */
void CheckTextures( CReport *pReport, const SDataMountResult &mount )
{
    if ( !mount.bMounted )
        return;
    bool bHaveTextures = false;
    for ( size_t i = 0; i < mount.assetDirsFound.size(); ++i )
        if ( mount.assetDirsFound[ i ] == "Textures" )
            bHaveTextures = true;
    if ( !bHaveTextures )
        return;

    pReport->Add( BOOT_HEADING, 0, "Image: MMP/DXT textures" );

    int nLoaded = 0, nDecoded = 0, nMatched = 0;
    for ( int nFileID = 1; nFileID <= 40 && nLoaded < 6; ++nFileID )
    {
        char szPath[ 64 ];
        snprintf( szPath, sizeof( szPath ), "Textures\\%d", nFileID );
        CFileStream file;
        if ( !file.TryOpenRead( szPath ) )
            continue;

        NHPTimer::STime t;
        NHPTimer::GetTime( &t );
        CObj< NImage::CImageMMP > pImage = NImage::LoadImageMMP( &file );
        const double fElapsed = NHPTimer::GetTimePassed( &t );
        if ( !pImage )
        {
            pReport->Add( BOOT_WARN, 0, "%s: not an MMP", szPath );
            continue;
        }
        ++nLoaded;

        const int nFormat = (int)pImage->GetFormat();
        const int nWidth = pImage->GetSizeX( 0 ), nHeight = pImage->GetSizeY( 0 );
        const int nDxt = ( nFormat == NGfx::CF_DXT1 ) ? 1 : ( nFormat == NGfx::CF_DXT3 ) ? 3
                       : ( nFormat == NGfx::CF_DXT5 ) ? 5 : 0;

        if ( !nDxt )
        {
            pReport->Add( BOOT_OK, fElapsed, "%s: %dx%d, format %d, %d mips (not DXT)",
                          szPath, nWidth, nHeight, nFormat, pImage->GetNumMipLevels() );
            continue;
        }

        std::vector< uint8_t > rgba( (size_t)nWidth * nHeight * 4 );
        if ( !DxtDecode( nDxt, (const uint8_t *)pImage->GetLFB( 0 ),
                         (size_t)pImage->GetLinearSize( 0 ), nWidth, nHeight, &rgba[ 0 ] ) )
        {
            pReport->Add( BOOT_FAIL, fElapsed, "%s: DXT%d decode failed", szPath, nDxt );
            continue;
        }
        ++nDecoded;

        unsigned long long r = 0, g = 0, b = 0;
        const size_t nPixels = (size_t)nWidth * nHeight;
        for ( size_t k = 0; k < nPixels; ++k )
        {
            r += rgba[ k * 4 ];
            g += rgba[ k * 4 + 1 ];
            b += rgba[ k * 4 + 2 ];
        }
        const int mr = (int)( r / nPixels ), mg = (int)( g / nPixels ), mb = (int)( b / nPixels );
        const DWORD dwAverage = pImage->GetAverageColor();
        const int hr = ( dwAverage >> 16 ) & 255, hg = ( dwAverage >> 8 ) & 255, hb = dwAverage & 255;
        const int nError = abs( mr - hr ) + abs( mg - hg ) + abs( mb - hb );
        const bool bMatch = nError <= 24;   /* 8 per channel: DXT quantisation */
        if ( bMatch )
            ++nMatched;

        pReport->Add( bMatch ? BOOT_OK : BOOT_WARN, fElapsed,
                      "%s: %dx%d DXT%d, %d mips; mean rgb(%d,%d,%d) vs header (%d,%d,%d)",
                      szPath, nWidth, nHeight, nDxt, pImage->GetNumMipLevels(),
                      mr, mg, mb, hr, hg, hb );
    }

    if ( nLoaded == 0 )
        pReport->Add( BOOT_WARN, 0, "no textures found among Textures\\1..40" );
    else if ( nDecoded > 0 && nMatched == nDecoded )
        pReport->Add( BOOT_OK, 0, "%d textures loaded, %d DXT levels decoded, all match "
                      "their stored average colour", nLoaded, nDecoded );
    else if ( nDecoded > 0 )
        pReport->Add( BOOT_WARN, 0, "%d textures loaded, %d decoded, %d matched",
                      nLoaded, nDecoded, nMatched );
}

extern "C" const char *a5_script_prelude( void );
static std::string g_szHarnessLuaOut;
static int HarnessLuaOut( lua_State *L )
{
    const char *psz = lua_tostring( L, 1 );
    if ( psz ) g_szHarnessLuaOut += psz;
    return 0;
}
static int HarnessLuaFalse( lua_State * ) { return 0; }
static int HarnessLuaRandom( lua_State *L ) { lua_pushnumber( L, 3 ); return 1; }   /* the engine registers random() itself */

void CheckScripting( CReport *pReport )
{
    pReport->Add( BOOT_HEADING, 0, "Script: Lua 4.0 virtual machine" );

    NHPTimer::STime t;
    NHPTimer::GetTime( &t );

    Script script( true );

    /* Exercise the parser, the VM, string handling and the C API round trip. */
    const char *pszProgram =
        "total = 0\n"
        "for i = 1, 100 do total = total + i end\n"
        "greeting = 'Silent Storm on ' .. 'Android'\n"
        "function square(x) return x * x end\n"
        "squared = square(12)\n";

    const int nResult = script.DoString( pszProgram );
    /*  lua_dobuffer() parses the chunk and *starts* it on a new Lua thread, but
     *  the lua_executeThreads() call that would run it to completion is
     *  commented out in ldo.cpp -- the engine pumps threads from its frame loop
     *  instead (Script::ExecuteThreads).  Without this the chunk never runs. */
    script.ExecuteThreads();
    const double fElapsed = NHPTimer::GetTimePassed( &t );

    if ( nResult != LUA_NOERR )
    {
        pReport->Add( BOOT_FAIL, fElapsed, "lua_dostring failed: %s",
                      ErrorToString( nResult ) );
        return;
    }

    Script::Object total    = script.GetGlobal( "total" );
    Script::Object squared  = script.GetGlobal( "squared" );
    Script::Object greeting = script.GetGlobal( "greeting" );

    const int nTotal   = total.GetInteger();
    const int nSquared = squared.GetInteger();
    const char *pszGreeting = greeting.GetString();

    if ( nTotal == 5050 && nSquared == 144 )
        pReport->Add( BOOT_OK, fElapsed, "Lua VM ran: sum(1..100)=%d, square(12)=%d",
                      nTotal, nSquared );
    else
        pReport->Add( BOOT_FAIL, fElapsed, "Lua produced total=%d squared=%d",
                      nTotal, nSquared );

    if ( pszGreeting && strcmp( pszGreeting, "Silent Storm on Android" ) == 0 )
        pReport->Add( BOOT_OK, 0, "Lua string concatenation returned \"%s\"", pszGreeting );
    else
        pReport->Add( BOOT_FAIL, 0, "Lua string handling returned \"%s\"",
                      pszGreeting ? pszGreeting : "(null)" );

    /*  The port's script prelude (platform/script_prelude.cpp): Lua 4 stand-ins
     *  for the retail script API.  Load it into a fresh VM with the two engine
     *  functions it uses (out, ObjectIsAction) faked, then call a few of the
     *  stand-ins -- a syntax slip there would silently abort every mission
     *  script on the device. */
    if ( const char *pszTest = getenv( "A5_LUA_TEST" ) )   /* ad-hoc Lua 4 experiments */
    {
        Script t( true );
        t.Register( "out", HarnessLuaOut );
        g_szHarnessLuaOut.clear();
        const int nT = t.DoString( pszTest );
        t.ExecuteThreads();
        pReport->Add( BOOT_DETAIL, 0, "  A5_LUA_TEST: %s; out: %s", ErrorToString( nT ), g_szHarnessLuaOut.c_str() );
    }
    {
        Script prelude( true );
        prelude.Register( "out", HarnessLuaOut );
        prelude.Register( "ObjectIsAction", HarnessLuaFalse );
        prelude.Register( "Sleep", HarnessLuaFalse );
        prelude.Register( "random", HarnessLuaRandom );
        g_szHarnessLuaOut.clear();
        const char *pszPrelude = a5_script_prelude();
        int n = prelude.DoBuffer( pszPrelude, strlen( pszPrelude ), "prelude" );
        prelude.ExecuteThreads();
        int n2 = LUA_NOERR;
        if ( n == LUA_NOERR )
        {
            n2 = prelude.DoString(
                "WaitForObject( 1 )\n"
                "r = Random( 1, 6 )\n"
                "PlaySound( 15691 ) PlaySound( 15691 )\n"
                "SetGlobalGameVar( 'x', 7 ) gv = GetGlobalGameVar( 'x' )\n"
                "n = TableGetSize( { 1, 2, 3 } )\n"
                "hp = ObjectGetHP( 1 )\n" );
            prelude.ExecuteThreads();
        }
        const int nR = prelude.GetGlobal( "r" ).GetInteger(), nGV = prelude.GetGlobal( "gv" ).GetInteger(),
                  nN = prelude.GetGlobal( "n" ).GetInteger(), nHP = prelude.GetGlobal( "hp" ).GetInteger();
        const bool bLoaded = g_szHarnessLuaOut.find( "prelude loaded" ) != std::string::npos;
        const bool bStubOnce = g_szHarnessLuaOut.find( "stub called: PlaySound" ) != std::string::npos
                            && g_szHarnessLuaOut.find( "stub called: PlaySound" ) == g_szHarnessLuaOut.rfind( "stub called: PlaySound" );
        if ( n == LUA_NOERR && n2 == LUA_NOERR && bLoaded && bStubOnce && nR >= 1 && nR <= 6 && nGV == 7 && nN == 3 && nHP == 100 )
            pReport->Add( BOOT_OK, 0, "script prelude: loads, stand-ins work (Random=%d, game var=%d, TableGetSize=%d, stub reported once)", nR, nGV, nN );
        else
            pReport->Add( BOOT_FAIL, 0, "script prelude: load %s, run %s, loaded-marker %d, stub-once %d, Random=%d gv=%d n=%d hp=%d; out: %s",
                          ErrorToString( n ), ErrorToString( n2 ), (int)bLoaded, (int)bStubOnce, nR, nGV, nN, nHP, g_szHarnessLuaOut.c_str() );
    }
}

#ifdef __ANDROID__
void D3DReportSink( void *pReporter, EBootStatus status, double fSeconds, const char *pszText )
{
    static_cast< CReport * >( pReporter )->Add( status, fSeconds, "%s", pszText );
}
#endif

}  // namespace

BASIC_REGISTER_CLASS( CProbeObject );

SBootReport RunBootHarness( const char *pszExternalFilesDir,
                            const char *pszInternalFilesDir,
                            const char *pszExplicitDataRoot )
{
    CReport report;

    /* ---- data ---------------------------------------------------------- */
    report.Add( BOOT_HEADING, 0, "Game data" );
    const SDataMountResult mount = MountGameData( pszExternalFilesDir, pszInternalFilesDir,
                                                  pszExplicitDataRoot );
    report.report.bDataMounted = mount.bMounted;
    report.report.szDataRoot   = mount.szRoot;

    if ( mount.bMounted )
    {
        report.Add( BOOT_OK, 0, "data root: %s", mount.szRoot.c_str() );
        report.Add( BOOT_DETAIL, 0, "  %d packages, %d asset directories%s",
                    (int)mount.packagesFound.size(), (int)mount.assetDirsFound.size(),
                    mount.bHasGameDb ? ", game.db present" : "" );
    }
    else
    {
        report.Add( BOOT_WARN, 0, "no game data found - engine checks still run" );
        for ( size_t i = 0; i < mount.candidates.size(); ++i )
            report.Add( BOOT_DETAIL, 0, "  looked in %s (%s)",
                        mount.candidates[ i ].szPath.c_str(),
                        mount.candidates[ i ].bExists ? "exists, no game data" : "absent" );
    }

    /* ---- engine -------------------------------------------------------- */
    CheckTiming( &report );
    CheckObjectSystem( &report );
    CheckSerialiser( &report );
    CheckPackages( &report, mount );
    CheckLooseAssets( &report, mount );
    CheckGameDatabase( &report, mount );
    CheckTextures( &report, mount );
    CheckScripting( &report );
    CheckGameScripts( &report, mount );
#ifdef __ANDROID__
    /* Needs a current GL context: android_main runs the harness after InitDisplay. */
    RunD3DSelfTest( &report, D3DReportSink );
#endif

    report.Add( BOOT_HEADING, 0, "Summary" );
    report.Add( report.report.nFailed ? BOOT_FAIL : BOOT_OK, 0,
                "%d passed, %d failed, %d warnings",
                report.report.nPassed, report.report.nFailed, report.report.nWarnings );

    return report.report;
}
