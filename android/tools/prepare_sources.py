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
import time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ANDROID_DIR = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(ANDROID_DIR)
DEFAULT_SRC = os.path.join(REPO_ROOT, "Soft", "Andy", "Jan03", "a5dll")
DEFAULT_OUT = os.path.join(ANDROID_DIR, "gen")

# Modules copied into the Android build tree.  Adding a module here is the first
# step of porting it; see docs/PORTING.md for the current status of each.
#  ADOFake provides the database-source stub the shipping game links instead of
#  ADOImport's COM/ADO code; ADOImport is staged for its BasicDB.h header only.
#  Main is staged (not built yet) because DBFormat includes two of its headers.
MODULES = ["Misc", "FileIO", "Script", "MiscDll", "Image", "DBFormat",
           "ADOFake", "ADOImport", "Main", "libpng"]

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
#  Mechanical pass: wide characters
# ---------------------------------------------------------------------------
#  The engine's wide strings are UTF-16 -- std::wstring is 16-bit on Win32, and
#  the on-disk format depends on it (CStructureSaver::DataChunkString reads
#  `nLength / 2` characters and writes `size() * 2` bytes).  Android's wchar_t is
#  32-bit, which would double every character and corrupt every string read out
#  of game.db.
#
#  -fshort-wchar is not an option: libc++ and bionic are built with 4-byte
#  wchar_t, and std::wstring's char_traits calls into wmemcpy/wmemcmp.  So the
#  staged sources move to char16_t/std::u16string, which is exactly UTF-16, and
#  compat/src/wide_char.cpp supplies the char16_t forms of the wide CRT.

WIDE_SUBSTITUTIONS = [
    (re.compile(r"\bwstring\b"), "u16string"),
    (re.compile(r"\bwchar_t\b"), "char16_t"),
    # L"..." / L'.' string and character literals become u"..." / u'.'
    (re.compile(r"(?<![A-Za-z0-9_])L(?=[\"'])"), "u"),
]

#  Constructs that cannot be mapped mechanically.  None appear in the modules
#  staged today; if one shows up, it needs a decision rather than a rewrite.
WIDE_UNSUPPORTED = re.compile(
    r"\b(wostream|wistream|wstringstream|wostringstream|wistringstream|"
    r"wofstream|wifstream|wfstream|wcout|wcerr|wcin|wclog|wbuffer_convert)\b" )


def rewrite_wide_characters(text, rel_path, warnings):
    unsupported = WIDE_UNSUPPORTED.search(text)
    if unsupported:
        warnings.append("%s uses %s, which has no char16_t equivalent in libc++"
                        % (rel_path, unsupported.group(1)))
        return text
    for pattern, replacement in WIDE_SUBSTITUTIONS:
        text, count = pattern.subn(replacement, text)
        stats["wide-char"] += count
    return text


# ---------------------------------------------------------------------------
#  Mechanical pass: forward-declared enums
# ---------------------------------------------------------------------------
#  The engine forward-declares enums freely (`enum EPose;`) and uses them as
#  fields.  MSVC allowed that because its unscoped enums are always int-sized;
#  ISO C++ only permits an opaque enum declaration when the underlying type is
#  fixed.  Spelling `enum EPose : int;` says exactly what MSVC assumed -- and the
#  *definition* has to carry the same `: int`, or clang rejects the mismatch.
#
#  So: every opaque declaration gets `: int`, and every definition of an enum
#  that is forward-declared anywhere in the tree gets it too.  Enums that are
#  never forward-declared are left alone.

ENUM_FORWARD_RE = re.compile(r"^(\s*)enum\s+([A-Za-z_]\w*)\s*;", re.MULTILINE)
# `enum Name` followed (possibly after a newline) by `{`, but not `: type` and
# not `class`/`struct`.  Only names in FORWARD_DECLARED_ENUMS are rewritten.
ENUM_DEFINITION_RE = re.compile(
    r"^(\s*)enum\s+([A-Za-z_]\w*)(\s*)(?=\{|\n\s*\{)", re.MULTILINE)


def collect_forward_declared_enums(src_root, modules):
    names = set()
    for module in modules:
        module_dir = os.path.join(src_root, module)
        if not os.path.isdir(module_dir):
            continue
        for name in os.listdir(module_dir):
            if os.path.splitext(name)[1].lower() not in (".h", ".hpp", ".cpp"):
                continue
            with open(os.path.join(module_dir, name), "rb") as f:
                raw = f.read()
            try:
                text = raw.decode("cp1251")
            except UnicodeDecodeError:
                text = raw.decode("latin-1")
            for m in ENUM_FORWARD_RE.finditer(text):
                names.add(m.group(2))
    return names


def rewrite_enum_forward_declarations(text, forward_declared):
    def fix_forward(m):
        stats["enum-forward"] += 1
        return "%senum %s : int;" % (m.group(1), m.group(2))

    def fix_definition(m):
        if m.group(2) not in forward_declared:
            return m.group(0)
        stats["enum-definition"] += 1
        return "%senum %s : int%s" % (m.group(1), m.group(2), m.group(3))

    text = ENUM_FORWARD_RE.sub(fix_forward, text)
    text = ENUM_DEFINITION_RE.sub(fix_definition, text)
    return text


# ---------------------------------------------------------------------------
#  Mechanical pass: `typename` on dependent iterator types
# ---------------------------------------------------------------------------
#  Inside a template, `vector<T>::iterator i;` needs `typename` in ISO C++
#  because the compiler cannot know that ::iterator names a type until T is
#  bound.  MSVC 7 resolved it lazily and never asked.  The engine writes this
#  form throughout (118 sites in Main alone), so it is handled here rather
#  than rule by rule.
#
#  The pass is conservative: it only touches a `Container<...>::iterator`
#  declaration when a template parameter of the *innermost enclosing template*
#  appears inside the angle brackets.  Non-dependent uses (`vector<int>::
#  iterator`) are left alone, where `typename` would be a (harmless) noise word.

TEMPLATE_HEADER_RE = re.compile(r"template\s*<([^<>]*(?:<[^<>]*>[^<>]*)*)>")
ITERATOR_DECL_RE = re.compile(
    r"(?<![\w:])((?:std::)?(?:vector|list|map|multimap|set|multiset|hash_map|hash_set|"
    r"hash_multimap|deque)\s*<((?:[^<>]|<(?:[^<>]|<[^<>]*>)*>)*)>\s*::\s*"
    r"(?:const_)?(?:reverse_)?iterator)\b(?=\s+[A-Za-z_])")


def template_parameters(header_text):
    """Names declared in a template<...> parameter list."""
    names = set()
    for part in header_text.split(","):
        tokens = re.findall(r"[A-Za-z_]\w*", part)
        if tokens:
            # `class T`, `typename T`, `int N`, `class T = Foo` -> the declared name
            # is the last identifier before any '=' default.
            before_default = part.split("=")[0]
            ids = re.findall(r"[A-Za-z_]\w*", before_default)
            if ids:
                names.add(ids[-1])
    return names


def rewrite_dependent_iterators(text):
    # Walk the file, tracking the most recent template<...> header seen; a
    # template body ends well before the next header, so "most recent" is a
    # sound approximation for the engine's declaration style.
    out = []
    pos = 0
    current_params = set()
    events = sorted(
        [(m.start(), "tmpl", m) for m in TEMPLATE_HEADER_RE.finditer(text)] +
        [(m.start(), "iter", m) for m in ITERATOR_DECL_RE.finditer(text)])
    for start, kind, m in events:
        if kind == "tmpl":
            current_params = template_parameters(m.group(1))
            continue
        inner = m.group(2)
        if not current_params:
            continue
        if not any(re.search(r"\b%s\b" % re.escape(p), inner) for p in current_params):
            continue
        # Already qualified?
        if text[max(0, start - 9):start].rstrip().endswith("typename"):
            continue
        out.append(text[pos:start])
        out.append("typename ")
        pos = start
        stats["typename"] += 1
    out.append(text[pos:])
    return "".join(out)


# ---------------------------------------------------------------------------
#  Mechanical pass: condition declarations with parenthesised initialisers
# ---------------------------------------------------------------------------
#  `if ( CDynamicCast<T> p( expr ) )` -- a declaration in an if condition with a
#  direct-init parenthesised initialiser.  ISO C++ only allows `= expr` (or a
#  braced initialiser) there; MSVC 7 accepted the parenthesised form.  The
#  engine uses this idiom for every downcast (~110 sites in Main).  Rewriting to
#  `if ( CDynamicCast<T> p = expr )` is exactly equivalent: CDynamicCast's
#  constructors are implicit, so copy-initialisation picks the same one.
#
#  A tiny parser is used rather than a regex so nested parentheses inside the
#  initialiser (`p( Create( a, b ) )`) are balanced correctly.

CONDITION_DECL_RE = re.compile(
    r"\b(if|while)\s*\(\s*(CDynamicCast\s*<[^<>]*(?:<[^<>]*>[^<>]*)*>\s*)([A-Za-z_]\w*)\s*\(")


def rewrite_condition_declarations(text):
    out = []
    pos = 0
    for m in CONDITION_DECL_RE.finditer(text):
        if m.start() < pos:
            continue
        # m.end() is just past the initialiser's opening '('.  Find its match.
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            i += 1
        if depth:
            continue  # unbalanced -- leave it alone
        initialiser = text[m.end():i - 1].strip()
        out.append(text[pos:m.start()])
        out.append("%s ( %s%s = %s" % (m.group(1), m.group(2), m.group(3), initialiser))
        pos = i
        stats["condition-decl"] += 1
    out.append(text[pos:])
    return "".join(out)


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

RULES += [
    (
        "Misc/Geom.h",
        "SHMatrix's anonymous union holds CVec3/CVec4 members that declare "
        "their own (empty) constructors.  ISO C++ then deletes SHMatrix's "
        "implicit default constructor; MSVC 7 did not.  Declare one, empty like "
        "the originals, so `SHMatrix m;` stays valid and uninitialised.",
        """	bool HomogeneousInverse( const SHMatrix &m );
	const CVec3 GetTranslation() const { return CVec3( _14, _24, _34 ); }
};""",
        """	bool HomogeneousInverse( const SHMatrix &m );
	const CVec3 GetTranslation() const { return CVec3( _14, _24, _34 ); }
	// [android] anonymous-union members with constructors delete the implicit
	// default constructor under ISO C++; the original relied on MSVC accepting it.
	SHMatrix() {}
};""",
    ),
    (
        "Misc/Geom.h",
        "SFBTransform holds two SHMatrix; same fix.",
        """struct SFBTransform
{
	SHMatrix forward, backward;
};""",
        """struct SFBTransform
{
	SHMatrix forward, backward;
	SFBTransform() {}  // [android] see SHMatrix
};""",
    ),
]

RULES += [
    (
        "Misc/BasicFactory.h",
        "CClassFactory::GetTypeID<TT>() evaluated typeid(TT) with TT often only "
        "forward-declared at the call site (NDatabase::ImportField<CSound> in "
        "DataAck.cpp, etc.).  MSVC allowed typeid on an incomplete class; ISO C++ "
        "does not.  typeid(TT*) is always well-formed and identifies TT just as "
        "uniquely, so keep a second index keyed by the pointer type, filled at "
        "RegisterType time, and look that up instead.  RegisterTypeSafe (runtime "
        "registration from an object) cannot fill it and still uses the "
        "class-typed index, which GetTypeID falls back to.",
        """	template < class TT >
		void RegisterType( int nTypeID, newFunc func, TT* ) { RegisterTypeBase( nTypeID, func, &typeid(TT) ); }""",
        """	template < class TT >
		void RegisterType( int nTypeID, newFunc func, TT* )
		{
			RegisterTypeBase( nTypeID, func, &typeid(TT) );
			// [android] also index by the pointer type; see GetTypeID below
			typeIndexByPointer[ &typeid(TT*) ] = nTypeID;
		}""",
    ),
    (
        "Misc/BasicFactory.h",
        "GetTypeID: look up by typeid(TT*) instead of typeid(TT).  Every type "
        "the engine asks about is registered statically through REGISTER_CLASS, "
        "which fills the pointer-typed index; a type only registered at runtime "
        "(RegisterTypeSafe) would need to be complete here, as before.",
        """	template<class TT>
		int GetTypeID( TT *p = 0 ) { return VFT2TypeID( &typeid(TT) ); }""",
        """	template<class TT>
		int GetTypeID( TT *p = 0 )
		{
			// [android] typeid(TT) requires a complete type; typeid(TT*) does not.
			// The pointer-typed index is filled by RegisterType (see above).
			return PointerType2TypeID( &typeid(TT*) );
		}
private:
	int PointerType2TypeID( VFT t )
	{
		CTypeIndexHash::const_iterator i = typeIndexByPointer.find( t );
		if ( i != typeIndexByPointer.end() )
			return i->second;
		for ( i = typeIndexByPointer.begin(); i != typeIndexByPointer.end(); ++i )
		{
			if ( *i->first == *t )
			{
				typeIndexByPointer[t] = i->second;
				return i->second;
			}
		}
		return -1;
	}
public:""",
    ),
    (
        "Misc/BasicFactory.h",
        "Declare the pointer-typed index next to the existing one.",
        """	CTypeIndexHash typeIndex;
	CTypeNewHash typeInfo;""",
        """	CTypeIndexHash typeIndex;
	CTypeIndexHash typeIndexByPointer;   // [android] keyed by typeid(TT*)
	CTypeNewHash typeInfo;""",
    ),
]

RULES += [
    (
        "Misc/Basic2.h",
        "CPtr<T> == T* was ambiguous under ISO overload resolution: the member "
        "operator==(const T*) needs a qualification conversion on the argument, "
        "while the built-in T*==T* needs the user conversion operator T*() on "
        "the left -- a tie.  MSVC 7 preferred the member.  Making the member a "
        "template on the argument's pointee type gives it an exact match, which "
        "wins.  Same result, no more ambiguity, and it also accepts derived-class "
        "pointers the way the built-in comparison did.",
        """	inline bool operator==( const TPtrName &a ) const { return Get() == a.CBase::Get(); }            \\
	inline bool operator==( const T *a ) const { return Get() == a; }                         \\
	inline bool operator!=( const TPtrName &a ) const { return Get() != a.CBase::Get(); }            \\
	inline bool operator!=( const T *a ) const { return Get() != a; }                         \\""",
        """	inline bool operator==( const TPtrName &a ) const { return Get() == a.CBase::Get(); }            \\
	inline bool operator!=( const TPtrName &a ) const { return Get() != a.CBase::Get(); }            \\
	/* [android] templated on the pointee so the member is an exact match; see */\\
	/* the porting rule in tools/prepare_sources.py.  Was: operator==(const T*) */\\
	template<class TOther>                                                                    \\
	inline bool operator==( TOther *a ) const { return Get() == a; }                          \\
	template<class TOther>                                                                    \\
	inline bool operator!=( TOther *a ) const { return Get() != a; }                          \\""",
    ),
]

RULES += [
    (
        "ADOImport/BasicDB.h",
        "CDBPtr derives from the CPtrBase template and, like CPtr, needs the "
        "dependent base's members named explicitly under ISO two-phase lookup.",
        """	typedef CPtrBase<T, CDBRecord::SRef> CBase;
public:
	CDBPtr() {}""",
        """	typedef CPtrBase<T, CDBRecord::SRef> CBase;
	// [android] see the same note on BASIC_PTR_DECLARE in Misc/Basic2.h
protected:
	using CBase::SetObject;
	using CBase::Get;
public:
	using CBase::Set;
	using CBase::GetPtr;
	CDBPtr() {}""",
    ),
]

RULES += [
    (
        "ADOImport/BasicDB.h",
        "REGISTER_DATABASE_CLASS goes through NDatabase::AddTable -> "
        "RegisterTypeSafe, which registers a table by the *dynamic* type_info of "
        "a freshly created record.  The port's GetTypeID looks up by typeid(T*) "
        "(see Misc/BasicFactory.h), so the static type has to reach the factory "
        "too.  The macro has it: pass a typed null pointer through an overload "
        "of AddTable that records both keys.",
        """#define REGISTER_DATABASE_CLASS( N, table, name ) NDatabase::AddTable( N, table, \\
(NDatabase::RecordCreateFunc)name##::New##name );
#define REGISTER_DATABASE_CLASS_TEMPL( N, table, name,className ) NDatabase::AddTable( N, table, \\
(NDatabase::RecordCreateFunc)name##::New##className );""",
        """// [android] the macros also hand the factory the static record type, so the
// pointer-typed index GetTypeID<T>() consults is filled for every table.
#define REGISTER_DATABASE_CLASS( N, table, name ) NDatabase::AddTable( N, table, \\
(NDatabase::RecordCreateFunc)name##::New##name, (name*)0 );
#define REGISTER_DATABASE_CLASS_TEMPL( N, table, name,className ) NDatabase::AddTable( N, table, \\
(NDatabase::RecordCreateFunc)name##::New##className, (name*)0 );""",
    ),
    (
        "ADOImport/BasicDB.h",
        "Declare the typed AddTable overload used by the macros above.",
        """	void AddTable( int nTableID, const char *pszTableName, RecordCreateFunc newf );""",
        """	void AddTable( int nTableID, const char *pszTableName, RecordCreateFunc newf );
	// [android] typed variant: registers the pointer-typed key as well
	template<class T>
	void AddTable( int nTableID, const char *pszTableName, RecordCreateFunc newf, T * )
	{
		AddTable( nTableID, pszTableName, newf );
		GetRecordTypes().RegisterPointerType( nTableID, (T*)0 );
	}""",
    ),
    (
        "Misc/BasicFactory.h",
        "Factory: allow registering the pointer-typed key on its own, for types "
        "whose main registration happens at runtime (RegisterTypeSafe).",
        """	void RegisterTypeSafe( int nTypeID, newFunc func ) """,
        """	// [android] pointer-typed key only; pairs with RegisterTypeSafe below
	template < class TT >
		void RegisterPointerType( int nTypeID, TT* ) { typeIndexByPointer[ &typeid(TT*) ] = nTypeID; }
	void RegisterTypeSafe( int nTypeID, newFunc func ) """,
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 6: 32-bit object references in the chunk serialiser
# ---------------------------------------------------------------------------
#  CStructureSaver stores a cross-object reference as the object's *address at
#  save time*, written as 4 bytes, and rebuilds the graph on load by mapping
#  those 4-byte values back to freshly created objects.  The values are opaque
#  IDs as far as the file is concerned -- but the code keeps them in void*
#  variables and hash_map<void*,...>, so on a 64-bit target a 4-byte read leaves
#  half the pointer unwritten and a 4-byte write drops half the address (and two
#  live objects can collide in the low 32 bits).  Every reference in game.db
#  resolved to nothing and the database loaded empty.
#
#  The fix keeps the on-disk format byte-for-byte: references are handled as
#  uint32 IDs throughout, and on write each stored object gets a dense sequence
#  number instead of its address.
RULES += [
    (
        "FileIO/BasicChunk1.h",
        "Object-reference maps: key by the 32-bit on-disk ID, not by void*.",
        """	typedef std::hash_map<void*,CPtr<CObjectBase>,SDefaultPtrHash> CObjectsHash;
	CObjectsHash objects;
	typedef std::hash_map<void*,bool,SDefaultPtrHash> CPObjectsHash;
	CPObjectsHash storedObjects;
	std::list<CObjectBase*> toStore;""",
        """	// [android] the file stores 32-bit save-time addresses as reference IDs.
	// They are keyed as the uint32 they are on disk; on write, objects are
	// numbered densely (see StoreObject) rather than by truncated address.
	typedef unsigned int TObjectRef;
	typedef std::hash_map<TObjectRef,CPtr<CObjectBase> > CObjectsHash;
	CObjectsHash objects;
	typedef std::hash_map<const void*,TObjectRef,SDefaultPtrHash> CPObjectsHash;
	CPObjectsHash storedObjects;
	std::list<CObjectBase*> toStore;
	TObjectRef nNextObjectRef;""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "StoreObject: write a dense 32-bit ID for the object, assigned on first "
        "sight, instead of 4 bytes of its address.",
        """void CStructureSaver::StoreObject( CObjectBase *pObject )
{
	if ( pObject != 0 && storedObjects.find( pObject ) == storedObjects.end() )
	{
		toStore.push_back( pObject );
		storedObjects[pObject] = true; // важно присвоить хоть что-нибудь
	}
	RawData( &pObject, 4 );
}""",
        """void CStructureSaver::StoreObject( CObjectBase *pObject )
{
	// [android] was RawData( &pObject, 4 ) -- the low 32 bits of the address.
	TObjectRef nRef = 0;
	if ( pObject != 0 )
	{
		CPObjectsHash::iterator it = storedObjects.find( pObject );
		if ( it == storedObjects.end() )
		{
			nRef = nNextObjectRef++;
			toStore.push_back( pObject );
			storedObjects[pObject] = nRef;
		}
		else
			nRef = it->second;
	}
	RawData( &nRef, 4 );
}""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "LoadObject: read the 32-bit ID into a uint32, not into half a pointer.",
        """CObjectBase* CStructureSaver::LoadObject()
{
	void *pServerPtr = 0;
	RawData( &pServerPtr, 4 );
	if ( pServerPtr != 0 )
	{
		CObjectsHash::iterator pFound = objects.find( pServerPtr );""",
        """CObjectBase* CStructureSaver::LoadObject()
{
	TObjectRef nRef = 0;   // [android] was `void *pServerPtr` read 4 bytes at a time
	RawData( &nRef, 4 );
	if ( nRef != 0 )
	{
		CObjectsHash::iterator pFound = objects.find( nRef );""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "Start(): read the object table's reference IDs as uint32.",
        """			int nTypeID = 0;
			void *pServer = 0;
			bool bValid;
			obj.Read( &nTypeID, 4 );
			obj.Read( &pServer, 4 );
			obj.Read( &bValid,1 );""",
        """			int nTypeID = 0;
			TObjectRef pServer = 0;   // [android] 32-bit reference ID, was void*
			bool bValid;
			obj.Read( &nTypeID, 4 );
			obj.Read( &pServer, 4 );
			obj.Read( &bValid,1 );""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "Start(): per-object data chunks are keyed by the same 32-bit ID.",
        """			void *pServer = 0;
			CObjectBase *pObject;
			StartChunk( (chunk_id) 1, i + 1 );
			DataChunk( 0, &pServer, 4, 1 );""",
        """			TObjectRef pServer = 0;   // [android] was void*
			CObjectBase *pObject;
			StartChunk( (chunk_id) 1, i + 1 );
			DataChunk( 0, &pServer, 4, 1 );""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "Start(): reset the write-side reference counter.",
        """	chunks.clear();
	obj.Clear();
	data.Clear();
	chunks.resize( chunks.size() + 1 );  // [android] was push_back() with no argument
	bIsReading = bRead;""",
        """	chunks.clear();
	obj.Clear();
	data.Clear();
	chunks.resize( chunks.size() + 1 );  // [android] was push_back() with no argument
	bIsReading = bRead;
	nNextObjectRef = 1;   // [android] 0 is the null reference""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "Finish(): write each object's assigned ID, and its data chunk keyed by "
        "that ID, instead of 4 bytes of its address.",
        """			int nTypeID = pSSClasses->GetObjectTypeID( pObject );
			bool bValid = IsValid( pObject );
			ASSERT( nTypeID != -1 );
			obj.Write( &nTypeID, 4 );
			obj.Write( &pObject, 4 );
			obj.Write( &bValid, 1 );
			// save object data
			StartChunk( (chunk_id) 1, nObject );
			DataChunk( 0, &pObject, 4, 1 );""",
        """			int nTypeID = pSSClasses->GetObjectTypeID( pObject );
			bool bValid = IsValid( pObject );
			ASSERT( nTypeID != -1 );
			// [android] the reference ID assigned in StoreObject, not the address
			TObjectRef nRef = storedObjects[pObject];
			obj.Write( &nTypeID, 4 );
			obj.Write( &nRef, 4 );
			obj.Write( &bValid, 1 );
			// save object data
			StartChunk( (chunk_id) 1, nObject );
			DataChunk( 0, &nRef, 4, 1 );""",
    ),
]

RULES += [
    (
        "FileIO/BasicChunk1.cpp",
        "Count object-table entries whose type is not registered, instead of "
        "silently mapping them to null.  Lets a loader tell 'the file is a "
        "later format' apart from 'the file is empty'.",
        """			CObjectBase *pObject = pSSClasses->CreateObject( nTypeID );
			ASSERT( pObject );""",
        """			CObjectBase *pObject = pSSClasses->CreateObject( nTypeID );
			ASSERT( pObject );
			if ( !pObject )   // [android] see a5_serializer_unknown_types()
				RecordUnknownType( nTypeID );""",
    ),
    (
        "FileIO/BasicChunk1.cpp",
        "Implement the unknown-type tally next to the class factory global.",
        """void StartRegisterSaveload()
{
	if ( !pSSClasses )
		pSSClasses = new CClassFactory<CObjectBase>;
}""",
        """void StartRegisterSaveload()
{
	if ( !pSSClasses )
		pSSClasses = new CClassFactory<CObjectBase>;
}
// [android] Diagnostics for CStructureSaver::Start(): which type IDs in a file
// had no registered class.  Reset by a5_serializer_reset_unknown_types().
static int g_nUnknownTypeCount = 0;
static int g_nLastUnknownType = 0;
static void RecordUnknownType( int nTypeID ) { ++g_nUnknownTypeCount; g_nLastUnknownType = nTypeID; }
extern "C" int a5_serializer_unknown_types( int *pnLastTypeID )
{
	if ( pnLastTypeID ) *pnLastTypeID = g_nLastUnknownType;
	return g_nUnknownTypeCount;
}
extern "C" void a5_serializer_reset_unknown_types() { g_nUnknownTypeCount = 0; g_nLastUnknownType = 0; }""",
    ),
]

RULES += [
    (
        "libpng/png.h",
        "libpng 1.0.9 includes the vendored zlib by a relative Windows path; the "
        "Android build uses the NDK's zlib (same API, and libpng only needs the "
        "public one).",
        # (the include-path pass has already turned the backslashes into '/')
        '#include "../zlib/zlib.h"',
        '#include <zlib.h>  // [android] NDK zlib instead of the vendored copy',
    ),
]

RULES += [
    (
        "libpng/pngconf.h",
        "libpng 1.0.9's `MACOS` branch means classic Mac OS with CodeWarrior's "
        "<fp.h>.  A modern macOS host defines MACOS through the compat layer's "
        "toolchain and has <math.h> like everyone else; take that branch.",
        "#  if defined(MACOS)\n     /* We need to check that <math.h> hasn't already been included earlier",
        "#  if defined(MACOS) && !defined(__APPLE__)  /* [android] classic Mac OS only */\n     /* We need to check that <math.h> hasn't already been included earlier",
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
    forward_declared_enums = collect_forward_declared_enums(src_root, MODULES)
    applied = []
    unmatched = []
    warnings = []

    if not report_only and os.path.isdir(out_root):
        # macOS occasionally fails rmtree with "directory not empty" when
        # Spotlight or the Finder touches the tree mid-delete; retry rather
        # than leaving the output half-removed.
        for attempt in range(3):
            try:
                shutil.rmtree(out_root)
                break
            except OSError:
                if attempt == 2:
                    raise
                time.sleep(0.2)

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
            text = rewrite_wide_characters(text, rel_path, warnings)
            text = rewrite_enum_forward_declarations(text, forward_declared_enums)
            text = rewrite_dependent_iterators(text)
            text = rewrite_condition_declarations(text)
            text = apply_rules(text, rel_path, applied, unmatched)

            if not report_only:
                with open(os.path.join(module_out, name), "w", encoding="utf-8") as f:
                    f.write(text)
            stats["files"] += 1

    return applied, unmatched, warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", default=DEFAULT_SRC, help="original a5dll source tree")
    parser.add_argument("--out", default=DEFAULT_OUT, help="generated build tree")
    parser.add_argument("--report", action="store_true", help="print rewrites without writing")
    args = parser.parse_args()

    applied, unmatched, warnings = prepare(args.src, args.out, report_only=args.report)

    print("prepare_sources: %d files from %s" % (stats["files"], args.src))
    print("  include paths rewritten : %d" % stats["include-path"])
    print("  wide-char substitutions : %d" % stats["wide-char"])
    print("  enum forward decls      : %d declarations, %d definitions given ': int'"
          % (stats["enum-forward"], stats["enum-definition"]))
    print("  typename on dependent   : %d iterator declarations" % stats["typename"])
    print("  if-condition declarations: %d rewritten to '= expr'" % stats["condition-decl"])
    print("  targeted source rules   : %d applications" % len(applied))
    for rel_path, description in applied:
        print("    %-28s %s" % (rel_path, description))
    for warning in warnings:
        print("  WARNING: %s" % warning)
    if unmatched:
        print("  WARNING: %d rule(s) matched nothing -- the source may have moved:" % len(unmatched))
        for rel_path, description in unmatched:
            print("    %-28s %s" % (rel_path, description))
        return 1
    if not args.report:
        print("  output: %s" % args.out)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        # Piping into head/grep closes stdout early.  Without this the exception
        # surfaces after the output tree has already been rewritten, which looks
        # like a staging failure when nothing actually went wrong.
        try:
            sys.stdout.close()
        finally:
            os._exit(0)
