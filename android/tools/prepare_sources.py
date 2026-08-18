#!/usr/bin/env python3
"""
prepare_sources.py -- stage the 2003 engine sources for the Android/clang build.

The original tree under Soft/Andy/Jan03/a5dll is left byte-for-byte untouched.
This script copies the modules we build into android/gen/ and applies a set of
*named, documented* rewrites on the way through.  Every rewrite is either

  (a) mechanical and unavoidable  -- MSVC accepted `#include "..\\Misc\\Geom.h"`
      and case-insensitive filenames; clang on a case-sensitive filesystem does
      not; or
  (b) a genuine language/ISA incompatibility -- x86 inline assembly, or MSVC-only
      C++ that clang rejects outright.

Running with --report prints what fired without writing anything, so the delta
against the historical sources always stays auditable.
"""

import argparse
import os
import re
import shutil
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ANDROID_DIR = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(ANDROID_DIR)
DEFAULT_SRC = os.path.join(REPO_ROOT, "Soft", "Andy", "Jan03", "a5dll")
DEFAULT_OUT = os.path.join(ANDROID_DIR, "gen")

# Modules copied into the Android build tree.  Adding a module here is the first
# step of porting it; see docs/PORTING.md for the current status of each.
MODULES = ["Misc", "FileIO", "Script", "MiscDll", "Image", "DBFormat"]

COPY_EXTENSIONS = {".cpp", ".c", ".h", ".hpp", ".inl", ".txt"}

stats = defaultdict(int)


# ---------------------------------------------------------------------------
#  Rule 1 (mechanical): include paths
# ---------------------------------------------------------------------------
INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*)"([^"]+)"(.*)$')


def build_case_index(src_root):
    """lowercase relative path -> real relative path, for the whole source tree."""
    index = {}
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel_dir = os.path.relpath(dirpath, src_root)
        for name in filenames:
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            index[rel.replace("\\", "/").lower()] = rel.replace("\\", "/")
    return index


def fix_include_path(raw, module, src_root, case_index):
    """Translate one #include target to something clang can find on Linux."""
    path = raw.replace("\\", "/")

    # Resolve relative to the including module directory so we can correct case.
    candidate = os.path.normpath(os.path.join(module, path)).replace("\\", "/")
    real = case_index.get(candidate.lower())
    if real and real != candidate:
        # Rewrite to the on-disk spelling, keeping the "../Module/file.h" shape.
        rel = os.path.relpath(real, module).replace("\\", "/")
        return rel
    return path


def rewrite_includes(text, module, src_root, case_index, path_for_log):
    out_lines = []
    for line in text.split("\n"):
        m = INCLUDE_RE.match(line)
        if m:
            prefix, target, suffix = m.groups()
            fixed = fix_include_path(target, module, src_root, case_index)
            if fixed != target:
                stats["include-path"] += 1
                line = '%s"%s"%s' % (prefix, fixed, suffix)
        out_lines.append(line)
    return "\n".join(out_lines)


# ---------------------------------------------------------------------------
#  Rule 2..n (targeted): MSVC-only constructs and x86 assembly
# ---------------------------------------------------------------------------
#  Each entry is (relative path, description, old text, new text).  Exact string
#  matching -- if a source ever changes upstream the rule fails loudly rather
#  than silently mangling something.





RULES = [
    # ---- Misc/StdAfx.h : the shared 67-line prologue, copied into every module -
    (
        "*/StdAfx.h",
        "Remove the MSVC6 for-scope workaround; clang already scopes loop "
        "variables correctly and the macro breaks any modern for statement.",
        "#define for if(false); else for",
        "// [android] `#define for if(false); else for` removed: it existed to force\n"
        "// standard for-scoping on MSVC6.  clang is already conformant.",
    ),
    (
        "*/StdAfx.h",
        "Drop the STLport configuration include; the Android build uses libc++ "
        "(compat/include/stl/_config.h keeps the spelling valid for any leftovers).",
        '#include "stl_user_config.h"',
        '// [android] STLport replaced by libc++; see compat/include/hash_map.\n'
        '//#include "stl_user_config.h"',
    ),
    # ---- FileIO/Streams.h : MSVC-only in-class explicit specialisation ------
    (
        "FileIO/Streams.h",
        "In-class explicit template specialisation is an MSVC extension that "
        "clang rejects; plain overloads have identical semantics here.",
        """	template<class T>
		CDataStream& operator>>( T &res ) { Read( &res, sizeof(res) ); return *this; }
	template<class T>
		CDataStream& operator<<( const T &res ) { Write( &res, sizeof(res) ); return *this; }
	template<>
		CDataStream& operator>>( std::string &res ) { ReadString( res ); return *this; }
	template<>
		CDataStream& operator<<( const std::string &res ) { WriteString( res ); return *this; }""",
        """	template<class T>
		CDataStream& operator>>( T &res ) { Read( &res, sizeof(res) ); return *this; }
	template<class T>
		CDataStream& operator<<( const T &res ) { Write( &res, sizeof(res) ); return *this; }
	// [android] were in-class template<> specialisations (MSVC extension)
	CDataStream& operator>>( std::string &res ) { ReadString( res ); return *this; }
	CDataStream& operator<<( const std::string &res ) { WriteString( res ); return *this; }""",
    ),
    (
        "FileIO/Streams.h",
        "Same in-class specialisation problem in CBitStream.",
        """	template <class T>
		inline void Write( const T &a ) { Write( &a, sizeof(a) ); }
	template <class T>
		inline void Read( T &a ) { Read( &a, sizeof(a) ); }
	template<> 
		inline void Write<std::string>( const std::string &a ) { WriteCString( a.c_str() ); }
	template<> 
		inline void Read<std::string>( std::string &a ) { ReadCString( a ); }""",
        """	template <class T>
		inline void Write( const T &a ) { Write( &a, sizeof(a) ); }
	template <class T>
		inline void Read( T &a ) { Read( &a, sizeof(a) ); }
	// [android] were in-class template<> specialisations (MSVC extension)
	inline void Write( const std::string &a ) { WriteCString( a.c_str() ); }
	inline void Read( std::string &a ) { ReadCString( a ); }""",
    ),
    # ---- FileIO/Streams.cpp : path handling --------------------------------
    (
        "FileIO/Streams.cpp",
        "Route file opens through the compat resolver so backslash paths and "
        "case-insensitive game data work on Android's filesystem.",
        "pFile = fopen( pszFName, pszMode );",
        "pFile = a5_fopen( pszFName, pszMode );  // [android] separator + case resolution",
    ),
    # ---- FileIO/FilesPackage.cpp : shipped data uses a later signature ------
    (
        "FileIO/FilesPackage.cpp",
        "The shipped .res files carry signature 0x96948A22 -- the format was "
        "revised after this January 2003 snapshot.  Accept both.",
        "const DWORD DW_PACKAGE_SIGNATURE = 0x95938921;",
        "const DWORD DW_PACKAGE_SIGNATURE = 0x95938921;\n"
        "// [android] The retail data in Complete/*.res is stamped one revision later.\n"
        "const DWORD DW_PACKAGE_SIGNATURE_V2 = 0x96948A22;",
    ),
    (
        "FileIO/FilesPackage.cpp",
        "Accept either package signature when reading a header.",
        """		if ( nSignature != DW_PACKAGE_SIGNATURE )
			throw SFileIOError( "wrong signature" );""",
        """		if ( nSignature != DW_PACKAGE_SIGNATURE && nSignature != DW_PACKAGE_SIGNATURE_V2 )
			throw SFileIOError( "wrong signature" );""",
    ),
]



# ---------------------------------------------------------------------------
#  Rule set 3: x86 inline assembly in Misc/Tools.h and Misc/HPTimer.cpp
# ---------------------------------------------------------------------------
#  These are MSVC `_asm` blocks -- clang rejects the *syntax* regardless of the
#  target ISA, so each has to be re-expressed in C++.  The replacements below
#  preserve the original semantics exactly, including the non-obvious ones
#  (Float2Int rounds, it does not truncate).  Written as regexes because the
#  originals are tab-indented with inconsistent trailing whitespace.

RULES += [
    (
        "Misc/Tools.h",
        "Sign<int>: replace the setne/sar bit trick with a portable comparison.",
        re.compile(
            r"template <>\ninline int Sign<int>\( const int nVal \)\n\{\n"
            r"\tint nRes;\n\t_asm\n\t\{.*?\n\t\}\n\treturn nRes;\n\}",
            re.DOTALL),
        "template <>\n"
        "inline int Sign<int>( const int nVal )\n"
        "{\n"
        "\t// Original: x86 `test`/`setne`/`sar`/`or`, yielding -1, 0 or 1.\n"
        "\treturn ( nVal > 0 ) - ( nVal < 0 );\n"
        "}",
    ),
    (
        "Misc/Tools.h",
        "Sign<short int>: same bit trick, 16-bit variant.",
        re.compile(
            r"template <>\ninline short int Sign<short int>\( const short int nVal \)\n\{\n"
            r"\tshort int nRes;\n\t_asm\n\t\{.*?\n\t\}\n\treturn nRes;\n\}",
            re.DOTALL),
        "template <>\n"
        "inline short int Sign<short int>( const short int nVal )\n"
        "{\n"
        "\treturn (short int)( ( nVal > 0 ) - ( nVal < 0 ) );\n"
        "}",
    ),
    (
        "Misc/Tools.h",
        "MemSetDWord: replace `rep stosd` with a loop the compiler vectorises.",
        re.compile(
            r"inline void MemSetDWord\( void \*lpData, const DWORD value, const int nCount \)\n"
            r"\{\n\t_asm\n\t\{.*?\n\t\}\n\}",
            re.DOTALL),
        "inline void MemSetDWord( void *lpData, const DWORD value, const int nCount )\n"
        "{\n"
        "\t// Original: `rep stosd`.  clang emits an equivalent NEON/scalar store loop.\n"
        "\tDWORD *pOut = static_cast< DWORD * >( lpData );\n"
        "\tfor ( int i = 0; i < nCount; ++i )\n"
        "\t\tpOut[ i ] = value;\n"
        "}",
    ),
    (
        "Misc/Tools.h",
        "Float2Int: x87 fld/fistp -> lrintf (rounds, like the original; a cast "
        "would truncate and shift results by one).",
        re.compile(
            r"int __forceinline Float2Int\( const float fpVar \)\n\{\n"
            r"\tint nRet;\n\t__asm\s*\n\t\{.*?\n\t\}\n\treturn nRet;\n\}",
            re.DOTALL),
        "int __forceinline Float2Int( const float fpVar )\n"
        "{\n"
        "\t// Original: `fld dword ptr fpVar` / `fistp nRet`, which rounds using the\n"
        "\t// current x87 rounding mode (nearest-even by default) rather than\n"
        "\t// truncating.  lrintf() has exactly those semantics; a (int) cast does not.\n"
        "\treturn (int)lrintf( fpVar );\n"
        "}",
    ),
    (
        "Misc/Tools.h",
        "Min<float>: fcomp/fnstsw branchless select -> plain comparison.",
        re.compile(
            r"template<>\ninline const float Min<float>\( const float a, const float b \)\n\{\n"
            r"\tfloat fpRet;\n\t_asm\n\t\{.*?\n\t\}\n\treturn fpRet;\n\}",
            re.DOTALL),
        "template<>\n"
        "inline const float Min<float>( const float a, const float b )\n"
        "{\n"
        "\t// Original: branchless x87 select on the C0 flag ( b < a ? b : a ).\n"
        "\treturn b < a ? b : a;\n"
        "}",
    ),
    (
        "Misc/Tools.h",
        "Max<float>: same branchless select, opposite sense.",
        re.compile(
            r"template<>\ninline const float Max<float>\( const float a, const float b \)\n\{\n"
            r"\tfloat fpRet;\n\t_asm\n\t\{.*?\n\t\}\n\treturn fpRet;\n\}",
            re.DOTALL),
        "template<>\n"
        "inline const float Max<float>( const float a, const float b )\n"
        "{\n"
        "\t// Original: branchless x87 select on the C0 flag ( a < b ? b : a ).\n"
        "\treturn a < b ? b : a;\n"
        "}",
    ),
    (
        "Misc/Tools.h",
        "GetCPUID: x86 feature probe has no ARM meaning; report no MMX/SSE so "
        "every caller takes the portable path.",
        re.compile(
            r"#define GET_CPUID __asm _emit 0x0f __asm _emit 0xa2\n"
            r"inline DWORD GetCPUID\(\)\n\{\n\tDWORD dwRes;\n\t_asm\n\t\{.*?\n\t\}\n"
            r"\treturn dwRes;\n\}\n#undef GET_CPUID",
            re.DOTALL),
        "inline DWORD GetCPUID()\n"
        "{\n"
        "\t// Original: CPUID leaf 1, returning the EDX feature bits so callers could\n"
        "\t// pick MMX/SSE code paths.  ARM has neither, so report no features and the\n"
        "\t// engine falls back to its portable implementations.\n"
        "\treturn 0;\n"
        "}",
    ),
    (
        "Misc/HPTimer.cpp",
        "GetCounter: `rdtsc` -> CLOCK_MONOTONIC nanoseconds.",
        re.compile(
            r"static inline void GetCounter\( int64 \*pTime \)\n\{\n\t__asm\n\t\{.*?\n\t\}\n\}",
            re.DOTALL),
        "static inline void GetCounter( int64 *pTime )\n"
        "{\n"
        "\t// Original: `rdtsc`, a raw CPU cycle counter.  ARM's equivalent\n"
        "\t// (CNTVCT_EL0) is not reliably readable from userspace across devices,\n"
        "\t// and CLOCK_MONOTONIC is a vDSO call -- cheap enough for a frame timer.\n"
        "\tstruct timespec ts;\n"
        "\tclock_gettime( CLOCK_MONOTONIC, &ts );\n"
        "\t*pTime = (int64)ts.tv_sec * 1000000000LL + (int64)ts.tv_nsec;\n"
        "}",
    ),
    (
        "Misc/HPTimer.cpp",
        "InitHPTimer: the rdtsc-vs-QPC calibration loop is meaningless once the "
        "counter is already in nanoseconds; set the scale directly.",
        re.compile(
            r"static void InitHPTimer\(\)\n\{.*?\n\}\n"
            r"////////+\n"
            r"// [^\n]*\n"
            r"struct SHPTimerInit",
            re.DOTALL),
        "static void InitHPTimer()\n"
        "{\n"
        "\t// Original: spin-calibrated rdtsc against QueryPerformanceCounter to learn\n"
        "\t// the CPU clock rate.  GetCounter() now returns nanoseconds directly, so\n"
        "\t// the conversion factor is exact and needs no measurement.\n"
        "\tfProcFreq1 = 1e-9;\n"
        "}\n"
        "////////////////////////////////////////////////////////////////////////////////////////////////////\n"
        "struct SHPTimerInit",
    ),
    (
        "Misc/HPTimer.cpp",
        "Add the <time.h> include the new GetCounter needs.",
        '#include "StdAfx.h"\n#include "HPTimer.h"',
        '#include "StdAfx.h"\n#include "HPTimer.h"\n#include <time.h>  // [android] clock_gettime',
    ),
    # ---- Misc/RandomGen.cpp -------------------------------------------------
    (
        "Misc/RandomGen.cpp",
        "Seed ISAAC from /dev/urandom.  The original walked C:\\ recursively and "
        "hashed bytes out of a randomly chosen file; on Android that path never "
        "resolves and FillRandRsl would spin forever.",
        re.compile(
            r"void CRandomGenerator::FillRandRsl\(\)\n\{\n.*?\n\}\n"
            r"////////+",
            re.DOTALL),
        "void CRandomGenerator::FillRandRsl()\n"
        "{\n"
        "\t// Original: pick a pseudo-random file somewhere under C:\\ and read bytes\n"
        "\t// out of it for entropy.  There is no such path on Android, and the\n"
        "\t// original loops until it finds one -- so seed from the kernel CSPRNG\n"
        "\t// instead, which is what that code was approximating.\n"
        "\tbool bSeeded = false;\n"
        "\tFILE *pRandom = fopen( \"/dev/urandom\", \"rb\" );\n"
        "\tif ( pRandom )\n"
        "\t{\n"
        "\t\tbSeeded = fread( randrsl, 1, sizeof( randrsl ), pRandom ) == sizeof( randrsl );\n"
        "\t\tfclose( pRandom );\n"
        "\t}\n"
        "\tif ( !bSeeded )\n"
        "\t{\n"
        "\t\t// Last resort: clock plus address-space layout.\n"
        "\t\tsrand( (unsigned int)( GetTickCount() ^ (unsigned int)(uintptr_t)&bSeeded ) );\n"
        "\t\tfor ( int i = 0; i < RANDSIZ; ++i )\n"
        "\t\t\trandrsl[ i ] = ( (unsigned int)rand() << 16 ) ^ (unsigned int)rand();\n"
        "\t}\n"
        "}\n"
        "////////////////////////////////////////////////////////////////////////////////////////////////////",
    ),
    (
        "Misc/RandomGen.cpp",
        "Drop RecFindFile, the C:\\ directory walker that only FillRandRsl used.",
        re.compile(
            r"BOOL CRandomGenerator::RecFindFile\( std::string &szFoundName, const char \*pszBaseMask, "
            r"int nToFind, int\* pnTotFinded \)\n\{\n.*?\n\treturn FALSE;\n\}",
            re.DOTALL),
        "// [android] CRandomGenerator::RecFindFile removed -- it recursively scanned\n"
        "// C:\\ looking for a file to harvest entropy from.  See FillRandRsl below.",
    ),
]

FILEIO_FACADE = """

////////////////////////////////////////////////////////////////////////////////////////////////////
// [android] Package access facade.
//
// IFilesPackage is declared in FilesPackage.h but *defined* in this .cpp, so no
// other translation unit can hold one or read its file table.  On Windows that
// was fine -- the engine only ever used packages through CPackageStream inside
// this DLL.  The Android boot harness needs to enumerate a package to show what
// it loaded, so this facade exposes the table through a plain C API.
//
// Declared in compat/include/a5_package_api.h.
////////////////////////////////////////////////////////////////////////////////////////////////////
#include "a5_package_api.h"

namespace {
struct SPackageHandle
{
    CPtr<IFilesPackage> pPackage;
};
}

extern "C" void *A5PackageOpen( const char *pszFileName )
{
    IFilesPackage *pPackage = OpenFilesPackage( pszFileName );
    if ( !pPackage )
        return 0;
    SPackageHandle *pHandle = new SPackageHandle;
    pHandle->pPackage = pPackage;
    return pHandle;
}

extern "C" void A5PackageClose( void *pOpaque )
{
    delete static_cast<SPackageHandle*>( pOpaque );
}

extern "C" int A5PackageGetFileCount( void *pOpaque )
{
    SPackageHandle *pHandle = static_cast<SPackageHandle*>( pOpaque );
    if ( !pHandle || !pHandle->pPackage )
        return 0;
    return (int)pHandle->pPackage->GetFileTable().size();
}

extern "C" int A5PackageGetFileIDs( void *pOpaque, int *pnOut, int nMaxCount )
{
    SPackageHandle *pHandle = static_cast<SPackageHandle*>( pOpaque );
    if ( !pHandle || !pHandle->pPackage )
        return 0;
    int nCount = 0;
    // `auto` avoids naming CFileInfoHash, which is protected inside IFilesPackage.
    auto &files = pHandle->pPackage->GetFileTable();
    for ( auto it = files.begin(); it != files.end() && nCount < nMaxCount; ++it )
        pnOut[ nCount++ ] = it->first;
    return nCount;
}

extern "C" int A5PackageGetFileSize( void *pOpaque, int nFileID )
{
    SPackageHandle *pHandle = static_cast<SPackageHandle*>( pOpaque );
    if ( !pHandle || !pHandle->pPackage )
        return -1;
    SFileInfo *pInfo = pHandle->pPackage->GetFileInfo( nFileID );
    return pInfo ? (int)pInfo->nLength : -1;
}

extern "C" int A5PackageReadFile( void *pOpaque, int nFileID, void *pDest, int nMaxSize )
{
    SPackageHandle *pHandle = static_cast<SPackageHandle*>( pOpaque );
    if ( !pHandle || !pHandle->pPackage )
        return -1;
    try
    {
        CPackageStream stream( pHandle->pPackage, nFileID );
        int nSize = stream.GetSize();
        if ( nSize > nMaxSize )
            nSize = nMaxSize;
        stream.Seek( 0 );
        stream.Read( pDest, nSize );
        return nSize;
    }
    catch ( ... )
    {
        return -1;
    }
}
"""

RULES += [
    (
        "FileIO/FilesPackage.cpp",
        "Expose the package file table through a C facade so code outside this "
        "translation unit can enumerate a .res (IFilesPackage is defined here, "
        "not in the header).",
        None,
        FILEIO_FACADE,
    ),
    (
        "FileIO/FilesPackage.cpp",
        "Give IFilesPackage an accessor for its file table (it was private to "
        "the class before; nothing outside could enumerate a package).",
        """	CFileInfoHash files;
public:""",
        """	CFileInfoHash files;
public:
	// [android] added for the package facade at the bottom of this file
	CFileInfoHash& GetFileTable() { return files; }""",
    ),
]

RULES += [
    (
        "Misc/Tools.h",
        "Drop the hand-written float overloads of fabs/cos/sin/acos/asin.  MSVC's "
        "2003 <math.h> only had the double forms; libc++ declares the float ones, "
        "so redefining them here is a hard conflict.",
        re.compile(
            r"inline float fabs\( float x \)\n\{\n\treturn fabsf\( x \);\n\}",
            re.DOTALL),
        "// [android] float fabs() removed -- libc++ already declares it.",
    ),
    (
        "Misc/Tools.h",
        "Same for the float trigonometric wrappers.",
        re.compile(
            r"inline float cos\( float fVal \)[^\n]*\n"
            r"inline float sin\( float fVal \)[^\n]*\n"
            r"inline float acos\( float fVal \)[^\n]*\n"
            r"inline float asin\( float fVal \)[^\n]*\n"),
        "// [android] float cos/sin/acos/asin removed -- libc++ already declares them.\n",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 4: C++ that MSVC 7 accepted and clang does not
# ---------------------------------------------------------------------------
RULES += [
    (
        "Misc/Basic2.h",
        "Name members of the dependent base explicitly.  MSVC looked into "
        "dependent bases during template definition; ISO C++ (and clang) do not, "
        "so CPtr/CObj/CMObj could not see CPtrBase::Set.",
        """	typedef CPtrBase< T, TRef > CBase;                                                        \\
public:                                                                                     \\
	typedef T CDestType;                                                                      \\""",
        """	typedef CPtrBase< T, TRef > CBase;                                                        \\
	/* [android] pull the dependent base's members into scope; without these     */\\
	/* clang cannot find Set/SetObject/Get when the template is defined.         */\\
protected:                                                                                  \\
	using CBase::SetObject;                                                                   \\
	using CBase::Get;                                                                         \\
public:                                                                                     \\
	using CBase::Set;                                                                         \\
	typedef T CDestType;                                                                      \\""",
    ),
    (
        "Misc/Basic2.h",
        "Qualify Get() on the other operand for the same reason.",
        re.compile( r"a\.Get\(\)" ),
        "a.CBase::Get()",
    ),
    (
        "FileIO/BasicChunk1.h",
        "Add the 'typename' ISO C++ requires before a dependent type name "
        "(std::list<T1,T2>::iterator).",
        "for ( std::list<T1,T2>::iterator k = data.begin();",
        "for ( typename std::list<T1,T2>::iterator k = data.begin();",
    ),
    (
        "FileIO/BasicChunk1.h",
        "Same for std::hash_map<...>::iterator.",
        "for ( std::hash_map<T1,T2,T3,T4>::iterator pos = data.begin();",
        "for ( typename std::hash_map<T1,T2,T3,T4>::iterator pos = data.begin();",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "list::push_back() with no argument was an MSVC extension; resize() "
        "default-constructs the new element the same way.",
        re.compile( r"chunks\.push_back\(\);" ),
        "chunks.resize( chunks.size() + 1 );  // [android] was push_back() with no argument",
    ),
    (
        "Script/lparser.cpp",
        "vector::insert(pos) with no value was an MSVC extension; grow the "
        "vector and take a reference to the new element instead.",
        """	LocVar &res = *f->locvars.insert( f->locvars.end() );""",
        """	// [android] was: *f->locvars.insert( f->locvars.end() ) -- MSVC's
	// single-argument insert default-constructed the element.
	f->locvars.resize( f->locvars.size() + 1 );
	LocVar &res = f->locvars.back();""",
    ),
]

RULES += [
    (
        "Script/Script.cpp",
        "`using Script::Object;` inside a function is not a legal using-"
        "declaration (it names a class member); a typedef says the same thing.",
        "\tusing Script::Object;",
        "\ttypedef Script::Object Object;  // [android] was `using Script::Object;`",
    ),
    (
        "Script/lstring.cpp",
        "Compare the raw pointers explicitly.  p->pPtr is a CPtr<> and p->pObj a "
        "CObj<>, and comparing either against a bare CObjectBase* is ambiguous "
        "under ISO overload resolution (the smart pointer offers both an "
        "operator== and an implicit conversion to T*).",
        "( p->pPtr == pData || p->pObj == pData )",
        "( p->pPtr.GetPtr() == pData || p->pObj.GetPtr() == pData )",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 5: 64-bit correctness
# ---------------------------------------------------------------------------
#  The engine was written for 32-bit Windows, where a pointer fits in an int.
#  arm64-v8a is 64-bit, and a few places do pointer arithmetic through int.
RULES += [
    (
        "FileIO/Streams.cpp",
        "CBufferedStream::SetNewBufferSize truncated a pointer difference to "
        "int.  Two heap blocks on a 64-bit system are routinely more than 2GB "
        "apart, so the buffer-relocation fixup produced garbage pointers and the "
        "next read segfaulted.  Also compute the delta before freeing the old "
        "block rather than after.",
        """	unsigned char *pNewBuf = dbgnew unsigned char [ nNewSize ];
	if ( pBuffer )
	{
		memcpy( pNewBuf, pBuffer, pReservedEnd - pBuffer );
		delete[] pBuffer;
	}
	int nFixup = pNewBuf - pBuffer;
	pCurrent += nFixup;
	pFileEnd += nFixup;""",
        """	unsigned char *pNewBuf = dbgnew unsigned char [ nNewSize ];
	// [android] was `int nFixup = pNewBuf - pBuffer;` computed after the delete.
	// On 64-bit the difference between two allocations does not fit in an int,
	// and the truncated value corrupted pCurrent/pFileEnd.  ptrdiff_t is the
	// type this arithmetic actually has.
	ptrdiff_t nFixup = pNewBuf - pBuffer;
	if ( pBuffer )
	{
		memcpy( pNewBuf, pBuffer, pReservedEnd - pBuffer );
		delete[] pBuffer;
	}
	pCurrent += nFixup;
	pFileEnd += nFixup;""",
    ),
]

RULES += [
    (
        "FileIO/Streams.cpp",
        "CBufferedStream::LoadBufferForced adjusted two pointers by "
        "`nBufferStart - nPos`, computed in unsigned int.  When nPos is the "
        "larger value that expression is a huge positive number; on 32-bit "
        "Windows adding it wrapped the pointer around to the intended negative "
        "offset, but a 64-bit pointer just moves 4GB away.  This is what made "
        "every .res package fault on arm64.",
        """	pCurrent += nBufferStart - nPos;
	pFileEnd += nBufferStart - nPos;
	nBufferStart = nPos;""",
        """	// [android] was `pCurrent += nBufferStart - nPos;` in unsigned arithmetic,
	// which relied on 32-bit pointer wraparound.  Do the subtraction in a
	// signed, pointer-sized type so the shift is genuinely negative.
	const ptrdiff_t nShift = (ptrdiff_t)nBufferStart - (ptrdiff_t)nPos;
	pCurrent += nShift;
	pFileEnd += nShift;
	nBufferStart = nPos;""",
    ),
    (
        "FileIO/Streams.h",
        "GetSize/GetPosition mix a pointer difference with an unsigned offset; "
        "make the arithmetic explicitly signed and pointer-sized before it is "
        "narrowed to the int the callers expect.",
        """	int GetSize() { FixupSize(); return pFileEnd - pBuffer + nBufferStart; }
	int GetPosition() { return pCurrent - pBuffer + nBufferStart; }""",
        """	// [android] the additions below were unsigned; on 64-bit that turns a
	// negative intermediate into a huge value.  File offsets are still int.
	int GetSize() { FixupSize(); return (int)( ( pFileEnd - pBuffer ) + (ptrdiff_t)nBufferStart ); }
	int GetPosition() { return (int)( ( pCurrent - pBuffer ) + (ptrdiff_t)nBufferStart ); }""",
    ),
    (
        "FileIO/Streams.cpp",
        "CBufferedStream::Seek builds the new position with the same "
        "unsigned-offset pattern; compute it as a signed pointer offset.",
        """	unsigned char *pNewCurrent = pBuffer + nPos - nBufferStart; """,
        """	// [android] signed, pointer-sized arithmetic (see LoadBufferForced)
	unsigned char *pNewCurrent = pBuffer + ( (ptrdiff_t)nPos - (ptrdiff_t)nBufferStart ); """,
    ),
]


def apply_rules(text, rel_path, applied, unmatched):
    """Apply every rule whose file pattern matches.

    A rule is either an exact string swap or, when `old` is a compiled regex, a
    single regex substitution.  Rules that match no text are reported as
    unmatched rather than silently skipped -- that is the signal that an
    assumption about the historical source no longer holds.
    """
    for pattern, description, old, new in RULES:
        if pattern.startswith("*/"):
            if not rel_path.endswith(pattern[1:]):
                continue
        elif pattern != rel_path:
            continue

        if old is None:
            text = text + new
            applied.append((rel_path, description))
            continue

        if hasattr(old, "search"):
            text, count = old.subn(new, text)
            if count:
                applied.append((rel_path, description))
            else:
                unmatched.append((rel_path, description))
        elif old in text:
            text = text.replace(old, new)
            applied.append((rel_path, description))
        else:
            unmatched.append((rel_path, description))
    return text


# ---------------------------------------------------------------------------
#  Driver
# ---------------------------------------------------------------------------
def prepare(src_root, out_root, report_only=False):
    if not os.path.isdir(src_root):
        sys.exit("source tree not found: %s" % src_root)

    case_index = build_case_index(src_root)
    applied = []
    unmatched = []

    if not report_only and os.path.isdir(out_root):
        shutil.rmtree(out_root)

    for module in MODULES:
        module_src = os.path.join(src_root, module)
        if not os.path.isdir(module_src):
            sys.exit("module not found: %s" % module_src)
        module_out = os.path.join(out_root, module)
        if not report_only:
            os.makedirs(module_out, exist_ok=True)

        for name in sorted(os.listdir(module_src)):
            path = os.path.join(module_src, name)
            if not os.path.isfile(path):
                continue
            if os.path.splitext(name)[1].lower() not in COPY_EXTENSIONS:
                continue

            rel_path = "%s/%s" % (module, name)
            # The sources are CP1251 (Russian comments); decode leniently and
            # write back as UTF-8 so clang never chokes on a stray byte.
            with open(path, "rb") as f:
                raw = f.read()
            try:
                text = raw.decode("cp1251")
                stats["decoded-cp1251"] += 1
            except UnicodeDecodeError:
                text = raw.decode("latin-1")
                stats["decoded-latin1"] += 1
            # Normalise CRLF so the rewrite rules below can be written with \n.
            text = text.replace("\r\n", "\n")

            text = rewrite_includes(text, module, src_root, case_index, rel_path)
            text = apply_rules(text, rel_path, applied, unmatched)

            if not report_only:
                with open(os.path.join(module_out, name), "w", encoding="utf-8") as f:
                    f.write(text)
            stats["files"] += 1

    return applied, unmatched


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", default=DEFAULT_SRC, help="original a5dll source tree")
    parser.add_argument("--out", default=DEFAULT_OUT, help="generated build tree")
    parser.add_argument("--report", action="store_true", help="print rewrites without writing")
    args = parser.parse_args()

    applied, unmatched = prepare(args.src, args.out, report_only=args.report)

    print("prepare_sources: %d files from %s" % (stats["files"], args.src))
    print("  include paths rewritten : %d" % stats["include-path"])
    print("  targeted source rules   : %d applications" % len(applied))
    for rel_path, description in applied:
        print("    %-28s %s" % (rel_path, description))
    if unmatched:
        print("  WARNING: %d rule(s) matched nothing -- the source may have moved:" % len(unmatched))
        for rel_path, description in unmatched:
            print("    %-28s %s" % (rel_path, description))
        return 1
    if not args.report:
        print("  output: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
