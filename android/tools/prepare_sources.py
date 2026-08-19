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
import fnmatch
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
#  Input is staged for its headers: Input.h is a DirectInput-free interface that
#  the Android touch layer will implement, and Bind.h/Bind.cpp (action mapping)
#  are portable.  Input.cpp itself is DirectInput and is never built.
#  FModSound is staged for FMsound.h only: it is the engine's own audio
#  interface (NFMSound::*), and platform/audio_null.cpp implements it without
#  FMOD.  FMSound.cpp -- the FMOD 3 wrapper -- is never built.
MODULES = ["Misc", "FileIO", "Script", "MiscDll", "Image", "DBFormat",
           "ADOFake", "ADOImport", "Main", "libpng", "Input", "FModSound"]

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
#  A definition is `enum Name` followed by `{`, possibly after a comment and/or a
#  newline; `enum Name : type` and `enum class` are excluded by the lookahead.
ENUM_DEFINITION_RE = re.compile(
    r"^(\s*)enum\s+([A-Za-z_]\w*)(\s*)(?=(?://[^\n]*)?\n?\s*\{)", re.MULTILINE)


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

#  The engine also names container types through typedefs made inside the class
#  template (`typedef list< CObj<TPlayer> > TPlayerList;` then
#  `TPlayerList::iterator i`).  Those typedefs are dependent too.  Collect the
#  typedef names whose definition mentions a template parameter, and give their
#  ::iterator uses `typename` as well.
TYPEDEF_RE = re.compile(r"\btypedef\s+([^;{}]+?)\s+([A-Za-z_]\w*)\s*;")
TYPEDEF_ITERATOR_RE = re.compile(
    r"(?<![\w:.>])([A-Za-z_]\w*)\s*::\s*((?:const_)?(?:reverse_)?iterator)\b(?=\s+[A-Za-z_])")

#  Inside a template body, `typename` is permitted before *any* qualified name
#  (C++11 relaxed the dependent-only rule), so once we know we are inside one,
#  every `Something<...>::iterator x` / `Name::iterator x` declaration can take
#  it safely.  Names that resolve to something already complete just get a
#  redundant keyword.  We still skip the obvious non-type spellings.
ANY_ITERATOR_DECL_RE = re.compile(
    r"(?<![\w:.>])((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*(?:\s*<(?:[^<>]|<(?:[^<>]|<[^<>]*>)*>)*>)?)\s*::\s*"
    r"((?:const_)?(?:reverse_)?iterator)\b(?=\s+[A-Za-z_])")


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
    dependent_typedefs = set()   # typedef names whose definition uses a template parameter
    events = sorted(
        [(m.start(), "tmpl", m) for m in TEMPLATE_HEADER_RE.finditer(text)] +
        [(m.start(), "tdef", m) for m in TYPEDEF_RE.finditer(text)] +
        [(m.start(), "aiter", m) for m in ANY_ITERATOR_DECL_RE.finditer(text)])
    for start, kind, m in events:
        if kind == "tmpl":
            current_params = template_parameters(m.group(1))
            continue
        if kind == "tdef":
            definition, name = m.group(1), m.group(2)
            if current_params and any(re.search(r"\b%s\b" % re.escape(p), definition)
                                      for p in current_params):
                dependent_typedefs.add(name)
            continue
        if start < pos:
            continue
        if not current_params:
            continue
        # A template header's own scope ends at the next blank-line-separated
        # non-template declaration in practice; the engine keeps template
        # bodies contiguous, so "since the last template<...>" is a fair
        # approximation.  Guard against the one false positive that matters:
        # `std::` and `NStr::`-style namespace-qualified concrete iterators are
        # still fine to prefix inside a template.
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
#  Mechanical pass: single-argument insert()
# ---------------------------------------------------------------------------
#  `container.insert( container.end() )` -- MSVC's STL let insert() default-
#  construct the element when no value was given; ISO containers do not.
#  `*c.insert( c.end() )` becomes `( c.resize( c.size() + 1 ), c.back() )`,
#  which yields the same reference to a fresh default-constructed last element.
SINGLE_ARG_INSERT_RE = re.compile(
    r"\*\s*\(?\s*([A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*)\s*(\.|->)\s*insert\s*\(\s*\1\s*\2\s*end\s*\(\s*\)\s*\)\s*\)?")


def rewrite_single_arg_insert(text):
    def fix(m):
        c, op = m.group(1), m.group(2)
        stats["insert-end"] += 1
        return "( %s%sresize( %s%ssize() + 1 ), %s%sback() )" % (c, op, c, op, c, op)
    return SINGLE_ARG_INSERT_RE.sub(fix, text)


# ---------------------------------------------------------------------------
#  Mechanical pass: unqualified member-function names as arguments
# ---------------------------------------------------------------------------
#  `r1( this, OnShowBloodUpdated )` -- MSVC 7 accepted a bare member-function
#  name where a pointer-to-member is expected and formed &Class::Member itself.
#  ISO C++ requires the explicit form.  The engine uses this idiom for its event
#  registrations (`CEventRegister<C,E> r; ... r(this, OnE)`), always inside a
#  constructor initialiser or a member function of the class that owns the
#  method, so `&<enclosing class>::Method` is what MSVC formed.
#
#  Finding the enclosing class textually: the pass tracks `class X` / `struct X`
#  headers and `X::Method(` out-of-line definitions, and uses the innermost.
MEMBER_ARG_RE = re.compile(r"\(\s*this\s*,\s*([A-Z][A-Za-z0-9_]*)\s*\)")
CLASS_HEADER_RE = re.compile(r"\b(?:class|struct)\s+([A-Za-z_]\w*)\s*(?::[^{;]*)?\{")
OUT_OF_LINE_RE = re.compile(r"\b([A-Za-z_]\w*)\s*::\s*~?[A-Za-z_]\w*\s*\([^;{)]*\)\s*(?:const\s*)?(?::[^{;]*)?\{")


def rewrite_member_function_arguments(text):
    scopes = []   # (position, class name)
    for m in CLASS_HEADER_RE.finditer(text):
        scopes.append((m.start(), m.group(1)))
    for m in OUT_OF_LINE_RE.finditer(text):
        scopes.append((m.start(), m.group(1)))
    scopes.sort()

    def enclosing(pos):
        name = None
        for start, cls in scopes:
            if start > pos:
                break
            name = cls
        return name

    # A name defined as a free function in this file is not a member, however it
    # is passed (GView.cpp: `SetShadowsMode( this, IsNormalViewMode )` where
    # IsNormalViewMode is an inline free function).
    free_functions = set(re.findall(
        r"^\s*(?:static\s+|inline\s+)+[A-Za-z_][\w:<>]*[\s*&]+([A-Za-z_]\w*)\s*\(", text, re.MULTILINE))

    def fix(m):
        cls = enclosing(m.start())
        if not cls or m.group(1) in free_functions:
            return m.group(0)
        stats["member-arg"] += 1
        return "( this, &%s::%s )" % (cls, m.group(1))

    return MEMBER_ARG_RE.sub(fix, text)


# ---------------------------------------------------------------------------
#  Mechanical pass: address of a temporary
# ---------------------------------------------------------------------------
#  `f( &SRand() )`, `f( &vector<SLuaParams>() )` -- MSVC 7 materialised the
#  temporary and handed out its address for the duration of the call; ISO C++
#  forbids taking the address of a prvalue.  Rewrite: hoist the temporary into
#  a named local declared on the line before the statement (same lifetime as
#  far as the call is concerned: it lives to the end of the enclosing block,
#  which is at least the full expression), and pass its address.
#
#  Only handled where the statement is a whole line, which is every site in
#  the tree; anything else is left for a targeted rule.
#  Only the value types the engine actually spells this way.  A general
#  `&Name()` pattern would also match `T& Get()` declarations and calls of
#  functions returning references, which are legal and must stay untouched.
ADDRESS_OF_TEMP_RE = re.compile(r"&\s*(SRand|vector<SLuaParams>|vector<int>|SRandomSeed)\(\)")


def rewrite_address_of_temporaries(text):
    lines = text.split("\n")
    out = []
    counter = 0
    for line in lines:
        matches = list(ADDRESS_OF_TEMP_RE.finditer(line))
        if not matches or line.lstrip().startswith(("//", "*", "/*")):
            out.append(line)
            continue
        indent = line[:len(line) - len(line.lstrip())]
        hoisted = []
        for m in matches:
            counter += 1
            name = "a5Temp%d" % counter
            type_name = m.group(1)
            hoisted.append("%s%s %s;   // [android] was &%s() -- address of a temporary" % (indent, type_name, name, type_name))
            line = line.replace(m.group(0), "&" + name, 1)
            stats["addr-of-temp"] += 1
        out.extend(hoisted)
        out.append(line)
    return "\n".join(out)


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
			if ( SamePointerType( i->first, t ) )
			{
				typeIndexByPointer[t] = i->second;
				return i->second;
			}
		}
		return -1;
	}
	// The Itanium ABI gives type_info for a pointer to an *incomplete* class
	// internal linkage and a name starting with '*' ("compare by address"),
	// so typeid(NDb::CPlacableObject*) taken where the class is only
	// forward-declared (DataMap.cpp) never equals the one registered where it
	// is complete (DataFormat.cpp) -- and every such cross-reference in the
	// database import came out null.  Compare the mangled names instead.
	static bool SamePointerType( VFT a, VFT b )
	{
		if ( *a == *b )
			return true;
		const char *pa = a->name(), *pb = b->name();
		if ( *pa == '*' ) ++pa;
		if ( *pb == '*' ) ++pb;
		return strcmp( pa, pb ) == 0;
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
	inline bool operator!=( TOther *a ) const { return Get() != a; }                          \\
	/* `p == 0` / `p != 0`: a literal 0 is an int, which the pointer template   */\\
	/* cannot deduce; the original operator==(const T*) accepted it as a null   */\\
	/* pointer constant.  Keep that spelling working.                          */\\
	inline bool operator==( int nNull ) const { return Get() == (T*)(intptr_t)nNull; }        \\
	inline bool operator!=( int nNull ) const { return Get() != (T*)(intptr_t)nNull; }        \\""",
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

# ---------------------------------------------------------------------------
#  Rule set 7: dependent-base member access in class templates (Main)
# ---------------------------------------------------------------------------
#  MSVC 7 looked into dependent base classes during template definition; ISO
#  C++ two-phase lookup does not, so a template deriving from Base<T> must say
#  this->member or bring the name in with `using`.  These are the sites in the
#  engine; the CObjectBase macros are fixed at the macro so every template that
#  expands them is covered at once.
RULES += [
    (
        "Misc/Basic2.h",
        "OBJECT_BASIC_METHODS / OBJECT_NOCOPY_METHODS read CObjectBase's "
        "nRefData/nObjData.  Expanded inside a class template that derives from "
        "a *dependent* CObjectBase-derived base, those names need this-> under "
        "two-phase lookup.  this-> is correct in every context the macro is used.",
        re.compile(r"int nHoldRefs = nRefData, nHoldObjs = nObjData; ::new\(this\) classname; nRefData \+= nHoldRefs; nObjData \+= nHoldObjs;"),
        "int nHoldRefs = this->nRefData, nHoldObjs = this->nObjData; ::new(this) classname; this->nRefData += nHoldRefs; this->nObjData += nHoldObjs;",
    ),
    (
        "Main/Transform.h",
        "CMatrixStack43 / CFBMatrixStack derive from the dependent "
        "CBaseMatrixStack<N, T> and use its `matrices` / `nCurrentMatrix` "
        "unqualified; bring them into scope.",
        """template <int nMaxNumMatrices>
class CMatrixStack43: public CBaseMatrixStack<nMaxNumMatrices, SHMatrix>
{
public :""",
        """template <int nMaxNumMatrices>
class CMatrixStack43: public CBaseMatrixStack<nMaxNumMatrices, SHMatrix>
{
protected:   // [android] dependent-base members (ISO two-phase lookup)
	using CBaseMatrixStack<nMaxNumMatrices, SHMatrix>::matrices;
	using CBaseMatrixStack<nMaxNumMatrices, SHMatrix>::nCurrentMatrix;
public :""",
    ),
    (
        "Main/Transform.h",
        "Same for CFBMatrixStack.",
        """template <int nMaxNumMatrices>
class CFBMatrixStack: public CBaseMatrixStack<nMaxNumMatrices, SFBTransform>
{
protected:""",
        """template <int nMaxNumMatrices>
class CFBMatrixStack: public CBaseMatrixStack<nMaxNumMatrices, SFBTransform>
{
protected:   // [android] dependent-base members (ISO two-phase lookup)
	using CBaseMatrixStack<nMaxNumMatrices, SFBTransform>::matrices;
	using CBaseMatrixStack<nMaxNumMatrices, SFBTransform>::nCurrentMatrix;""",
    ),
    (
        "Main/Sync.h",
        "CSetSyncSrc<T> calls Add/Remove/Update of its dependent base CSyncSrc<T>.",
        """template<class T>
class CSetSyncSrc: public CSyncSrc<T>
{
	OBJECT_BASIC_METHODS( CSetSyncSrc );
	typedef CSyncSrc<T> TParent;""",
        """template<class T>
class CSetSyncSrc: public CSyncSrc<T>
{
	OBJECT_BASIC_METHODS( CSetSyncSrc );
	typedef CSyncSrc<T> TParent;
	using TParent::Add;      // [android] dependent-base members
	using TParent::Remove;
	using TParent::Update;""",
    ),
    (
        "Main/GResource.h",
        "CLazyResourceLoader reads pValue from its dependent base.",
        """	typedef CResourceLoader<TKey,TValue> TParent;
	CObj<CFileRequest> pRequest;
protected:
	virtual CFileRequest* CreateRequest() = 0;""",
        """	typedef CResourceLoader<TKey,TValue> TParent;
	CObj<CFileRequest> pRequest;
protected:
	using TParent::pValue;   // [android] dependent-base member
	virtual CFileRequest* CreateRequest() = 0;""",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 8: assorted MSVC 7 leniencies in Main
# ---------------------------------------------------------------------------
RULES += [
    (
        "Main/BuildingGrid.h",
        "`const ZSHIFT = 16;` -- implicit int, gone since C++98 but MSVC 7 "
        "still took it.",
        re.compile(r"^const ZSHIFT = 16;", re.MULTILINE),
        "const int ZSHIFT = 16;  // [android] implicit int",
    ),
    (
        "Main/aiVoxelRender.h",
        "`friend class CTParent;` where CTParent is a typedef: ISO C++ requires "
        "`friend CTParent;` (an elaborated-type-specifier cannot name a typedef).",
        re.compile(r"\tfriend class CTParent;"),
        "\tfriend CTParent;  // [android] was `friend class` on a typedef",
    ),
    (
        "Main/Cache.h",
        "typename on a nested dependent enum type.",
        re.compile(r"(?<!typename )TElement::ESplitType"),
        "typename TElement::ESplitType",
    ),
    (
        "Main/Sync.h",
        "typename on a nested dependent struct type.",
        re.compile(r"(?<![\w:])(?<!typename )CSyncSrc<T>::SObject\b"),
        "typename CSyncSrc<T>::SObject",
    ),
    (
        "Main/MapBuild.cpp",
        "typename on dependent T::reference.",
        re.compile(r"(?<!typename )\bT::reference\b"),
        "typename T::reference",
    ),
    (
        "Main/Cache.h",
        "typename on Alloc::pointer / CTracker::pointer typedefs.",
        re.compile(r"typedef Alloc::pointer pointer;"),
        "typedef typename Alloc::pointer pointer;",
    ),
    (
        "Main/Cache.h",
        "typename on CTracker::pointer.",
        re.compile(r"(?<!typename )\bCTracker::pointer\b"),
        "typename CTracker::pointer",
    ),
]

RULES += [
    (
        "Misc/EventsBase.h",
        "The event system keys handlers by typeid(TParam) and throws by "
        "typeid(T).  Handlers are registered from headers where the event class "
        "is only forward-declared (wDecal.h registers for NWorld::CShowBloodUpdated "
        "before it is defined), and ISO C++ rejects typeid on an incomplete type.  "
        "typeid(T*) is well-formed and identifies T just as uniquely; both the "
        "throw and the register side switch to it, so the registry stays "
        "internally consistent.",
        "\tThrowEventInner( typeid(T), &event ); ",
        "\tThrowEventInner( typeid(T*), &event );  // [android] pointer type: see rule",
    ),
    (
        "Misc/EventsBase.h",
        "Register side of the same change.",
        "\t\tRegisterEventHandler( this, typeid(TParam) );",
        "\t\tRegisterEventHandler( this, typeid(TParam*) );  // [android]",
    ),
    (
        "Misc/EventsBase.h",
        "Unregister side of the same change.",
        "\t\tUnregisterEventHandler( this, typeid(TParam) );",
        "\t\tUnregisterEventHandler( this, typeid(TParam*) );  // [android]",
    ),
    (
        "Main/Sync.h",
        "CBoolSyncSrc<T,TFunc> also calls Add/Remove/Update on its dependent base.",
        """template<class T, class TFunc>
class CBoolSyncSrc: public CSyncSrc<T>
{
	OBJECT_BASIC_METHODS( CBoolSyncSrc )
	typedef CBoolSyncSrc<T,TFunc> TThis;""",
        """template<class T, class TFunc>
class CBoolSyncSrc: public CSyncSrc<T>
{
	OBJECT_BASIC_METHODS( CBoolSyncSrc )
	typedef CBoolSyncSrc<T,TFunc> TThis;
	using CSyncSrc<T>::Add;      // [android] dependent-base members
	using CSyncSrc<T>::Remove;
	using CSyncSrc<T>::Update;""",
    ),
]

RULES += [
    (
        "Main/GCombiner.cpp",
        "SPartTransformer<T> derives from its parameter T and calls the transform "
        "helpers T provides; qualify them (this->) so two-phase lookup finds them.",
        """template<class T>
struct SPartTransformer : public T
{
	int DoTransform( IPart *p, T::TRes *pRes, const vector<CVec3> &transformed )
	{""",
        """template<class T>
struct SPartTransformer : public T
{
	// [android] dependent-base members (T is the template parameter)
	using T::CopyTransform;
	using T::SimpleTransform;
	using T::SimpleDiscreteTransform;
	using T::SingleSkinTransform;
	int DoTransform( IPart *p, typename T::TRes *pRes, const vector<CVec3> &transformed )
	{""",
    ),
    (
        "Main/GCombiner.cpp",
        "SGfxTnLTransformer<TTrans> calls DoTransform of its dependent base.",
        """template<class TTrans>
struct SGfxTnLTransformer : public SPartTransformer<TTrans>
{""",
        """template<class TTrans>
struct SGfxTnLTransformer : public SPartTransformer<TTrans>
{
	using SPartTransformer<TTrans>::DoTransform;   // [android] dependent base""",
    ),
]

RULES += [
    (
        "Main/aiPosition.h",
        "SMove is a union of two views over {SPathPlace, EMoveType}.  SPathPlace "
        "declares its own operator=, which under ISO C++ makes the union member "
        "non-trivial and deletes SMove's implicit copy/assignment; MSVC 7 "
        "generated a bitwise copy anyway.  Spell out that bitwise copy.",
        """struct SMove
{
	union
	{
		struct 
		{
			SPathPlace dest;
			EMoveType type;
		};
		struct 
		{
			SPathPlace first;
			EMoveType second;
		};
	};
};""",
        """struct SMove
{
	union
	{
		struct 
		{
			SPathPlace dest;
			EMoveType type;
		};
		struct 
		{
			SPathPlace first;
			EMoveType second;
		};
	};
	// [android] the anonymous union has a non-trivial member (SPathPlace has a
	// user-declared operator=), so ISO C++ deletes the implicit special members
	// MSVC generated.  They were bitwise copies of the 8 bytes; keep them so.
	SMove() : dest(), type() {}
	SMove( const SMove &a ) { memcpy( (void*)this, &a, sizeof( SMove ) ); }
	SMove& operator=( const SMove &a ) { memcpy( (void*)this, &a, sizeof( SMove ) ); return *this; }
};""",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 9: MMX skinning in Main/GCombiner.cpp
# ---------------------------------------------------------------------------
#  Three routines rotate a packed 8-bit unit vector (normal / tangent) by one,
#  two or three fixed-point 3x3 matrices, blend, renormalise through a lookup
#  table, and repack.  They are the *only* path for skinned normals, and they
#  are MMX inline assembly.  Below is the same fixed-point arithmetic written
#  out in C++ -- same 16-bit saturating adds, same shifts, same normalisation
#  table -- so the output is bit-comparable to the original, not merely close.
#  Nival's own commented-out check in TransformVertexT() shows the reference:
#  MMX result within 0.02 of a float rotate+normalise.
#
#  Layout reminder (GPixelFormat.h):
#    SCompactVector  { unsigned char z, y, x, w; }  -- w is 0, components 0..255
#    SMMXWord        { short nZ, nY, nX, nW; }
#    SCompactTransformer { SMMXWord a, b, c; }      -- rows in the packing order
#    fixups.normalFixup  = 0x8000 per lane (unpack bias), shiftedFixup = 0x8080

MMX_SCALAR_IMPL = """
////////////////////////////////////////////////////////////////////////////////////////////////////
// [android] Portable fixed-point equivalents of the MMX routines below.  See the
// rule in tools/prepare_sources.py for the derivation.  Lanes are (z, y, x, w).
////////////////////////////////////////////////////////////////////////////////////////////////////
namespace
{
	inline short SatAdd16( int a, int b )
	{
		int r = a + b;
		return (short)( r > 32767 ? 32767 : ( r < -32768 ? -32768 : r ) );
	}
	inline short MulHi16( int a, int b ) { return (short)( ( a * b ) >> 16 ); }   // pmulhw

	// pmulhw of a lane vector by a transformer row, all four lanes.
	inline void MulHiRow( short *pOut, const short *pIn, const NGfx::SMMXWord &row )
	{
		pOut[0] = MulHi16( pIn[0], row.nZ );
		pOut[1] = MulHi16( pIn[1], row.nY );
		pOut[2] = MulHi16( pIn[2], row.nX );
		pOut[3] = MulHi16( pIn[3], row.nW );
	}

	// One matrix: mm1 = v*a + rot16(v)*b + rot32(v)*c, where rot16 rotates the
	// lanes (z y x w) -> (x z y ?) and rot32 -> (y x z ?), exactly as the
	// psllq/psrlq/paddw sequences did.  Only lanes 0..2 matter afterwards.
	inline void RotateLanes( const short *v, short *r16, short *r32 )
	{
		// psllq 16 then paddw psrlq 32: lane0 <- v[2] (x), lane1 <- v[0]... derived
		// from the 64-bit shifts on (z,y,x,w) little-endian lanes:
		//   psllq mm2,16 : (0, z, y, x)     psrlq mm3,32 : (x, w, 0, 0)  sum: (x, z+w, y, x)
		//   psllq mm3,32 : (0, 0, z, y)     psrlq mm4,16 : (y, x, w, 0)  sum: (y, x, z+w, y)
		// w is always 0 for the input vector, so:
		r16[0] = v[2]; r16[1] = v[0]; r16[2] = v[1]; r16[3] = v[2];
		r32[0] = v[1]; r32[1] = v[2]; r32[2] = v[0]; r32[3] = v[1];
	}

	inline void ApplyTransformer( short *pAcc, const short *v, const NGfx::SCompactTransformer &t )
	{
		short r16[4], r32[4], m1[4], m2[4], m3[4];
		RotateLanes( v, r16, r32 );
		MulHiRow( m1, v,   t.a );
		MulHiRow( m2, r16, t.b );
		MulHiRow( m3, r32, t.c );
		for ( int i = 0; i < 4; ++i )
			pAcc[i] = SatAdd16( SatAdd16( m1[i], m2[i] ), m3[i] );
	}

	inline void Unpack( short *v, const NGfx::SCompactVector *pSrc, const SMMXFixups *pFixups )
	{
		// punpcklbw mm0, mm7 puts each byte in the high half of a 16-bit lane;
		// psubw the 0x8000 fixup recentres it around zero.
		v[0] = (short)( ( pSrc->z << 8 ) - (unsigned short)pFixups->normalFixup.nZ );
		v[1] = (short)( ( pSrc->y << 8 ) - (unsigned short)pFixups->normalFixup.nY );
		v[2] = (short)( ( pSrc->x << 8 ) - (unsigned short)pFixups->normalFixup.nX );
		v[3] = (short)( ( pSrc->w << 8 ) - (unsigned short)pFixups->normalFixup.nW );
	}

	inline void NormaliseAndPack( NGfx::SCompactVector *pRes, short *acc, const SMMXFixups *pFixups, int nPreShift )
	{
		// psllw acc, nPreShift  (5 for one matrix, 3 after the blend paths)
		for ( int i = 0; i < 4; ++i ) acc[i] = (short)( acc[i] << nPreShift );
		// pmaddwd + fold: sum of squares of the four lanes (w lane is 0)
		unsigned int nSq = (unsigned int)( acc[0]*acc[0] + acc[1]*acc[1] ) + (unsigned int)( acc[2]*acc[2] + acc[3]*acc[3] );
		unsigned int nIdx = nSq >> 18;
		if ( nIdx >= (unsigned int)ARRAY_SIZE( nNormalizeTable ) ) nIdx = ARRAY_SIZE( nNormalizeTable ) - 1;
		const short nScale = nNormalizeTable[ nIdx ];
		for ( int i = 0; i < 4; ++i )
			acc[i] = (short)( MulHi16( acc[i], nScale ) << 5 );
		// paddw shiftedFixup (0x8080: recentre and round), psrlw 8, packuswb
		unsigned char out[4];
		for ( int i = 0; i < 4; ++i )
		{
			const short *pFix = &pFixups->shiftedFixup.nZ;
			int t = ( (unsigned short)acc[i] + (unsigned short)pFix[i] ) & 0xffff;
			t >>= 8;
			out[i] = (unsigned char)( t > 255 ? 255 : t );
		}
		pRes->z = out[0]; pRes->y = out[1]; pRes->x = out[2]; pRes->w = out[3];
	}

	inline void BlendWeight( short *acc, int nWeight )
	{
		// psllw 4, pmulhw by mmxWeights[w] (= w << 6 in every lane)
		const int nW = mmxWeights[ nWeight & 0xff ].nX;
		for ( int i = 0; i < 4; ++i )
			acc[i] = MulHi16( (short)( acc[i] << 4 ), nW );
	}
}
static void MMXTransformVector( NGfx::SCompactVector *pRes, const NGfx::SCompactVector *pSrc, const SMMXFixups *pFixups,
	const NGfx::SCompactTransformer *pTrans )
{
	ASSERT( pSrc->w == 0 );
	short v[4], acc[4];
	Unpack( v, pSrc, pFixups );
	ApplyTransformer( acc, v, *pTrans );
	NormaliseAndPack( pRes, acc, pFixups, 5 );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
static void MMXTransformVector2( NGfx::SCompactVector *pRes, const NGfx::SCompactVector *pSrc, const SMMXFixups *pFixups,
	const NGfx::SCompactTransformer *pTrans, char w1,
	const NGfx::SCompactTransformer *pTrans2, char w2 )
{
	ASSERT( pSrc->w == 0 );
	short v[4], a1[4], a2[4];
	Unpack( v, pSrc, pFixups );
	ApplyTransformer( a1, v, *pTrans );
	ApplyTransformer( a2, v, *pTrans2 );
	BlendWeight( a1, (unsigned char)w1 );
	BlendWeight( a2, (unsigned char)w2 );
	for ( int i = 0; i < 4; ++i ) a1[i] = SatAdd16( a1[i], a2[i] );
	NormaliseAndPack( pRes, a1, pFixups, 3 );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
static void MMXTransformVector3( NGfx::SCompactVector *pRes, const NGfx::SCompactVector *pSrc, const SMMXFixups *pFixups,
	const NGfx::SCompactTransformer *pTrans, char w1,
	const NGfx::SCompactTransformer *pTrans2, char w2,
	const NGfx::SCompactTransformer *pTrans3, char w3 )
{
	ASSERT( pSrc->w == 0 );
	short v[4], a1[4], a2[4], a3[4];
	Unpack( v, pSrc, pFixups );
	ApplyTransformer( a1, v, *pTrans );
	ApplyTransformer( a2, v, *pTrans2 );
	ApplyTransformer( a3, v, *pTrans3 );
	BlendWeight( a1, (unsigned char)w1 );
	BlendWeight( a2, (unsigned char)w2 );
	BlendWeight( a3, (unsigned char)w3 );
	for ( int i = 0; i < 4; ++i ) a1[i] = SatAdd16( SatAdd16( a1[i], a2[i] ), a3[i] );
	NormaliseAndPack( pRes, a1, pFixups, 3 );
}
"""

RULES += [
    (
        "Main/GCombiner.cpp",
        "Replace the three MMX inline-assembly skinning routines with the same "
        "fixed-point arithmetic in portable C++.  See MMX_SCALAR_IMPL.",
        re.compile(
            r"// disable no emms warning, emms is placed after all mmx calcs\n"
            r"#pragma warning\( disable : 4799 \)\n"
            r"static void MMXTransformVector\(.*?"
            r"#pragma warning\( default : 4799 \)\n",
            re.DOTALL),
        MMX_SCALAR_IMPL.lstrip("\n"),
    ),
    (
        "Main/GCombiner.cpp",
        "`_asm emms;` clears the x87/MMX state after MMX use; there is none now.",
        re.compile(r"[ \t]*_asm emms;\n"),
        "",
    ),
]

RULES += [
    (
        "Misc/Basic2.h",
        "CPtr/CObj/CMObj converting constructors.  MSVC built `CPtr<Base> p = "
        "objOfDerived;` by chaining operator T*() and the pointer constructor -- "
        "two user-defined conversions, which ISO C++ does not allow implicitly.  "
        "Accepting any other smart pointer whose pointee converts to T* restores "
        "that in one step (Main relies on it in ~15 places).",
        "\tinline TPtrName( const TPtrName &a ): CBase( a ) {}                                       \\",
        "\tinline TPtrName( const TPtrName &a ): CBase( a ) {}                                       \\\n"
        "\t/* [android] converting ctor/assignment from any other CPtrBase, see rule */               \\\n"
        "\ttemplate<class TOther, class TOtherRef>                                                   \\\n"
        "\tinline TPtrName( const CPtrBase<TOther, TOtherRef> &a ): CBase( a.GetPtr() ) {}          \\\n"
        "\ttemplate<class TOther, class TOtherRef>                                                   \\\n"
        "\tinline TPtrName& operator=( const CPtrBase<TOther, TOtherRef> &a ) { Set( a.GetPtr() ); return *this; } \\",
    ),
    (
        "ADOImport/BasicDB.h",
        "CDBPtr: same converting constructor (wDebris.cpp assigns a CPtr<CTEffect> "
        "to a CDBPtr<CTEffect>).",
        "\tCDBPtr( T *_ptr ): CBase( _ptr ) {}",
        "\tCDBPtr( T *_ptr ): CBase( _ptr ) {}\n"
        "\t// [android] converting ctor from any other smart pointer, see Basic2.h rule\n"
        "\ttemplate<class TOther, class TOtherRef>\n"
        "\tCDBPtr( const CPtrBase<TOther, TOtherRef> &a ): CBase( a.GetPtr() ) {}\n"
        "\ttemplate<class TOther, class TOtherRef>\n"
        "\tCDBPtr& operator=( const CPtrBase<TOther, TOtherRef> &a ) { Set( a.GetPtr() ); return *this; }",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 10: one-off MSVC 7 leniencies in Main
# ---------------------------------------------------------------------------
RULES += [
    (
        "Main/RectPacker.cpp",
        "SRectOrder::operator() has no return type (implicit int) and is "
        "non-const; ISO needs `bool` and std::sort needs a const call operator.",
        """	SRectOrder( const vector<SRect> &_s ) : s(_s) {}
	operator()( int a, int b )
	{""",
        """	SRectOrder( const vector<SRect> &_s ) : s(_s) {}
	bool operator()( int a, int b ) const   // [android] implicit-int return, const for std::sort
	{""",
    ),
    (
        "Main/iMissionMovieUI.cpp",
        "`return false;` from a function returning a pointer -- MSVC converted the "
        "bool to a null pointer; ISO C++ does not.",
        re.compile(r"(if \( CDynamicCast<NWorld::CUICmd(?:Turn|Unit)> pTurn = pCmd \)\n\t\t)return false;"),
        r"\1return 0;   // [android] was `return false;` from a pointer-returning function",
    ),
    (
        "Main/iCommonUI.cpp",
        "String literals are const; a `WCHAR*` member cannot bind to u\"...\" "
        "under ISO C++ (MSVC allowed the deprecated conversion).",
        """			int nStringID;
			WCHAR* pszID;
		};
		SShootMode pShotModeNames[NDb::SM_MAXVALUE] =""",
        """			int nStringID;
			const WCHAR* pszID;   // [android] literals are const
		};
		SShootMode pShotModeNames[NDb::SM_MAXVALUE] =""",
    ),
    (
        "Main/GTerrainTexture.cpp",
        "GetGenericBuffer takes its colour by non-const reference but is called "
        "with temporaries; const& is what the callers mean.",
        "static NGfx::CTexture* GetGenericBuffer( CObj<NGfx::CTexture> *pBuf, NGfx::SPixel8888 &color )",
        "static NGfx::CTexture* GetGenericBuffer( CObj<NGfx::CTexture> *pBuf, const NGfx::SPixel8888 &color )  // [android] const&",
    ),
    (
        "Main/wMain.cpp",
        "GetDeployWithNumber is a file-static helper that names CWorld's private "
        "nested SWorldDeploySpot; MSVC 7 did not check access on nested types "
        "used in a parameter list.  Make the helper a friend via a forward "
        "declaration is intrusive; the minimal faithful fix is to make the "
        "nested struct public, which changes no behaviour.",
        None, None,   # placeholder replaced below
    ),
]
# drop the placeholder (kept the list readable); real wMain rule follows
RULES.pop()

#  MSVC 7 did not enforce access control on *nested types* named from outside
#  their class (a file-static helper taking vector<CWorld::SWorldDeploySpot>,
#  a derived class using CPathPlaceTable::SMove...).  ISO C++ does.  Making the
#  nested type public changes nothing at run time -- access control is a
#  compile-time property -- and is the smallest faithful fix.
RULES += [
    (
        "Main/wMain.h",
        "CWorld::SWorldDeploySpot is used by a file-static helper in wMain.cpp.",
        """class CWorld: public IWorld, public CTBSWorld<CUnitServer, CPlayer, CCommander>, public CDebrisController
{
	struct SWorldDeploySpot""",
        """class CWorld: public IWorld, public CTBSWorld<CUnitServer, CPlayer, CCommander>, public CDebrisController
{
public:   // [android] nested type named from a file-static helper (was private)
	struct SWorldDeploySpot""",
    ),
    (
        "Main/aiPathTable.h",
        "CPathPlaceTable::SMove and ::CMovesHash are named by CMultiMovesTable "
        "and by aiPath.cpp.",
        """class CPathPlaceTable
{
	typedef SMoveInfo<SPathPlace, WORD> SMove;
	typedef hash_map<SPathPlace, SMove, SPathPlaceHash> CMovesHash;""",
        """class CPathPlaceTable
{
public:   // [android] nested typedefs named from other classes (were private)
	typedef SMoveInfo<SPathPlace, WORD> SMove;
	typedef hash_map<SPathPlace, SMove, SPathPlaceHash> CMovesHash;
private:""",
    ),
    (
        "Main/GAnimParticles.h",
        "CATrailPath::STrailPoint is named by wBullet.cpp.",
        """class CATrailPath: public CAnimator
{
	OBJECT_BASIC_METHODS(CATrailPath);
private:
	struct STrailPoint""",
        """class CATrailPath: public CAnimator
{
	OBJECT_BASIC_METHODS(CATrailPath);
public:   // [android] nested type named from wBullet.cpp (was private)
	struct STrailPoint""",
    ),
    (
        "Main/GAnimParticles.h",
        "...and restore private for the members that follow it.",
        """	ZDATA_(CAnimator)
	int nTrailCount;
	STime sCast;
	vector<STrailPoint> trailpointsSet;""",
        """private:  // [android] see STrailPoint above
	ZDATA_(CAnimator)
	int nTrailCount;
	STime sCast;
	vector<STrailPoint> trailpointsSet;""",
    ),
    (
        "Main/aiColourer.h",
        "`friend class CLayerColorConstraints;` inside CColouredWaysCalcer does not "
        "introduce the name for later ordinary lookup under ISO C++ (it did under "
        "MSVC 7), so the CalcBestWays parameter that names it needs a forward "
        "declaration in the namespace.",
        """namespace NAI
{
enum ESpecialColor
{
	EC_LADDER_COLOR = 32000,
};""",
        """namespace NAI
{
class CLayerColorConstraints;   // [android] friend-declared below; ISO needs a real declaration
enum ESpecialColor
{
	EC_LADDER_COLOR = 32000,
};""",
    ),
]

#  Non-const reference parameters bound to temporaries.  MSVC 7 accepted these
#  (a well-known extension); ISO C++ does not.  Every one of these functions
#  only reads the parameter, so `const&` is what they meant.
RULES += [
    (
        "Main/wAnimation.h",
        "StandStill takes its position by non-const reference but is called with "
        "the temporary GetCPNoHeight() returns; it never writes it.",
        "\tvoid StandStill( CVec2 &pos, float fAngle, bool bNeedStrafe = false );",
        "\tvoid StandStill( const CVec2 &pos, float fAngle, bool bNeedStrafe = false );  // [android] const&",
    ),
    (
        "Main/wAnimation.cpp",
        "Definition of the above.",
        "void CUnitAnimator::StandStill( CVec2 &pos, float fAngle, bool bNeedStrafe )",
        "void CUnitAnimator::StandStill( const CVec2 &pos, float fAngle, bool bNeedStrafe )  // [android] const&",
    ),
    (
        "Main/aiMovesCalcer.h",
        "CreateGrid2GridCandidates: srcOrigin/dstOrigin are read-only, called with "
        "temporaries.",
        re.compile(r"(void CreateGrid2GridCandidates\( IAIMap \*pMap, \n[^\n]*?)CTPoint<int> &srcOrigin,\n([^\n]*?)CTPoint<int> &dstOrigin,"),
        r"\1const CTPoint<int> &srcOrigin,\n\2const CTPoint<int> &dstOrigin,",
    ),
    (
        "Main/aiMovesCalcer.cpp",
        "Definition of the above.",
        re.compile(r"(void CMovesCalcer::CreateGrid2GridCandidates\( IAIMap \*pMap, \n[^\n]*?)CTPoint<int> &srcOrigin,\n([^\n]*?)CTPoint<int> &dstOrigin,"),
        r"\1const CTPoint<int> &srcOrigin,\n\2const CTPoint<int> &dstOrigin,",
    ),
    (
        "Main/scFlowChart.cpp",
        "GetBestState is passed `CompareSize`, a member function, without the "
        "&Class:: that a pointer-to-member requires (the member-arg pass only "
        "handles the `(this, Method)` shape).",
        re.compile(r"GetBestState\( finalStates, CompareSize \)"),
        "GetBestState( finalStates, &CScenarioFlowChartPathFinder::CompareSize )",
    ),
    (
        "Main/wBuilding.cpp",
        "`extern FixSmallPieceID(...)` -- implicit int return type.",
        "\textern FixSmallPieceID( const NAI::CGeometryInfo::CPieceMap &pieces, int nPieceID );",
        "\textern int FixSmallPieceID( const NAI::CGeometryInfo::CPieceMap &pieces, int nPieceID );  // [android] implicit int",
    ),
    (
        "Main/iActionDecorator.h",
        "CActionDecorator<Type> derives from its parameter and calls the window "
        "methods Type provides; name them for two-phase lookup.",
        """template<class Type>
class CActionDecorator: public Type
{
private:""",
        """template<class Type>
class CActionDecorator: public Type
{
protected:   // [android] dependent-base members (Type is the parameter)
	using Type::GetStyle;
	using Type::GetInterface;
private:""",
    ),
]

#  Address of a temporary passed to a pointer parameter (`&SRand()`,
#  `&vector<SLuaParams>()`).  MSVC 7 materialised the temporary; ISO C++
#  forbids taking its address.  A named local with the same lifetime (the full
#  expression) is the equivalent.
RULES += [
]

# ---------------------------------------------------------------------------
#  Rule set 11: the last one-offs in Main
# ---------------------------------------------------------------------------
RULES += [
    (
        "Main/aiVoxelRender.h",
        "CTVoxelRenderer<TFinal,TRes> calls RasterNoClip of its dependent base "
        "CRasterizer<TFinal>.",
        """template <class TFinal, class TRes>
class CTVoxelRenderer : public CRasterizer<TFinal>
{
private:""",
        """template <class TFinal, class TRes>
class CTVoxelRenderer : public CRasterizer<TFinal>
{
protected:   // [android] dependent-base member
	using CRasterizer<TFinal>::RasterNoClip;
private:""",
    ),
    (
        "Main/aiPassCalcer.cpp",
        "MarkCandidatesToDisplace is a file-static helper naming the private "
        "nested CPassCalcer::STile; access on nested types was not enforced by "
        "MSVC 7.  Same fix as the others: make it public in the header.",
        None, None,
    ),
]
RULES.pop()
RULES += [
    (
        "Main/RodJunction.cpp",
        "`const iRnd = ...` -- implicit int.",
        "\tconst iRnd = pRand->Get( edges.size() );",
        "\tconst int iRnd = pRand->Get( edges.size() );  // [android] implicit int",
    ),
    (
        "Main/iCommonUI.cpp",
        "swprintf( ..., u\"%s\", <u16string> ) passes a std::basic_string through "
        "varargs, which is undefined -- MSVC 7 happened to pass the buffer "
        "pointer.  Pass .c_str() explicitly.",
        re.compile(r'swprintf\( wsBuffer, u"%s: --- ", GetDBString\( pShotModeNames\[nTemp\]\.nStringID \) \);'),
        'swprintf( wsBuffer, u"%s: --- ", GetDBString( pShotModeNames[nTemp].nStringID ).c_str() );  // [android] .c_str()',
    ),
    (
        "Main/iSaveManager.cpp",
        "MSVC's `struct _stat` / `_stat()` are POSIX stat under other names.",
        "\tstruct _stat sStat;\n\tint nRet = _stat( GetSlotFilePath( szName, S_SAVE_FILENAME ).c_str(), &sStat );",
        "\tstruct stat sStat;   // [android] MSVC _stat -> POSIX stat\n"
        "\tchar szResolved[ 1024 ];\n"
        "\ta5_resolve_path( GetSlotFilePath( szName, S_SAVE_FILENAME ).c_str(), szResolved, sizeof( szResolved ) );\n"
        "\tint nRet = stat( szResolved, &sStat );",
    ),
    (
        "Main/iSaveManager.cpp",
        "wcsftime on char16_t: format narrow, widen (the format is ASCII digits "
        "and separators).",
        "\twcsftime( wcBuffer, MAX_PATH, u\"%d/%m/%y\", pLocalTime );",
        "\t{   // [android] wcsftime has no char16_t form: format narrow, widen\n"
        "\t\tchar szNarrow[ 64 ];\n"
        "\t\tstrftime( szNarrow, sizeof( szNarrow ), \"%d/%m/%y\", pLocalTime );\n"
        "\t\tint i = 0;\n"
        "\t\tfor ( ; szNarrow[ i ] && i < MAX_PATH - 1; ++i ) wcBuffer[ i ] = (WCHAR)(unsigned char)szNarrow[ i ];\n"
        "\t\twcBuffer[ i ] = 0;\n"
        "\t}",
    ),
    (
        "Main/iSaveManager.cpp",
        "Add <sys/stat.h> for the stat() above.",
        '#include "StdAfx.h"',
        '#include "StdAfx.h"\n#include <sys/stat.h>   // [android] stat()',
    ),
]

RULES += [
    (
        "Main/aiPassCalcer.h",
        "CPassCalcer::STile is named by a file-static helper in aiPassCalcer.cpp; "
        "MSVC 7 did not enforce access on nested types.  Make the typedef public.",
        """class CPassCalcer
{
	typedef CNodesLayer::STile STile;
	struct SLadderInfo""",
        """class CPassCalcer
{
public:   // [android] nested typedef named from a file-static helper (was private)
	typedef CNodesLayer::STile STile;
private:
	struct SLadderInfo""",
    ),
]

RULES += [
    (
        "Main/GRenderLight.cpp",
        "`inline IsPointLightSupported(...)` -- implicit int (it returns a bool).",
        "inline IsPointLightSupported( ERenderPath renderPath )",
        "inline bool IsPointLightSupported( ERenderPath renderPath )  // [android] implicit int",
    ),
    (
        "Main/BuildingSchema.cpp",
        "`const nz = ...` -- implicit int.",
        "\t\tconst nz = pI->ptJ.z;",
        "\t\tconst int nz = pI->ptJ.z;  // [android] implicit int",
    ),
    (
        "Main/A5Script.cpp",
        "`{ (0,0) }` as an aggregate initialiser is a comma expression yielding "
        "0, which MSVC 7 accepted for the first (const char*) member; ISO C++ "
        "does not convert int to pointer.  The intent is a null terminator "
        "entry: `{ { 0, 0 } }`.",
        "Script::SRegFunction pLuaPtrTagFuncList[] = { (0,0) };",
        "Script::SRegFunction pLuaPtrTagFuncList[] = { { 0, 0 } };  // [android] was `{ (0,0) }`",
    ),
    (
        "Main/Cursor.cpp",
        "SystemParametersInfo(SPI_GETMOUSE) reads Windows' mouse acceleration "
        "thresholds; there is no equivalent for a touch screen.  Use the values "
        "the code already falls back to (no acceleration).",
        """	DWORD pdwParams[3];
	SystemParametersInfo( SPI_GETMOUSE, 0, pdwParams, 0 );

	fThreshold1 = pdwParams[0];
	fThreshold2 = pdwParams[1];
	fAcceleration = pdwParams[2];""",
        """	// [android] was SystemParametersInfo( SPI_GETMOUSE ): Windows mouse
	// acceleration thresholds.  A touch screen has none -- take the values the
	// original used when the query returned zeros.
	fThreshold1 = 6;
	fThreshold2 = 10;
	fAcceleration = 0;""",
    ),
    (
        "Main/Cursor.cpp",
        "GetCursorPos/ScreenToClient in the editor cursor: the pointer position "
        "comes from the input layer on Android, and this cursor is the map "
        "editor's, which does not run on the device.  Read the position from the "
        "compat pointer state so the code still compiles and behaves sanely.",
        """	POINT sPoint;
	GetCursorPos( &sPoint );
	ScreenToClient( NGfx::GetHWND(), &sPoint );""",
        """	POINT sPoint;
	a5_get_pointer_position( &sPoint.x, &sPoint.y );   // [android] was GetCursorPos + ScreenToClient""",
    ),
]

#  swscanf on char16_t.  libc has no char16_t scanf, and a generic narrow-and-
#  forward wrapper would be wrong for %s (it would write char into a WCHAR
#  buffer).  Only four call sites exist, with two formats; each is rewritten
#  to the explicit conversion it performs.
RULES += [
    (
        "Main/UIMLHandlers.h",
        "swscanf( \"%x\" ) -> strtol on the narrowed string.",
        "\t\tswscanf( wsColor.c_str(), u\"%x\", &sColor.color );",
        "\t\tsColor.color = (DWORD)a5_u16_strtol( wsColor.c_str(), 0, 16 );  // [android] was swscanf %x",
    ),
    (
        "Main/GText.cpp",
        "swscanf( \"%x\" ) -> strtol on the narrowed string.",
        "\t\tswscanf( iTemp->wsString.c_str(), u\"%x\", &sState.sColor.color );",
        "\t\tsState.sColor.color = (DWORD)a5_u16_strtol( iTemp->wsString.c_str(), 0, 16 );  // [android] was swscanf %x",
    ),
    (
        "Main/UIMLHandlers.h",
        "swscanf( \"%d%2s\" ): a number followed by an optional two-character "
        "modifier (e.g. \"12px\").  Parse the number with strtol and copy up to "
        "two following non-space characters, returning the same count swscanf "
        "would have.",
        "\t\t\t\t\tint nParams = swscanf( wsParam.c_str(), u\"%d%2s\", &sState.sFont.nSize, wsString );",
        "\t\t\t\t\tint nParams = a5_u16_scan_int_and_suffix( wsParam.c_str(), &sState.sFont.nSize, wsString, 2 );  // [android] was swscanf %d%2s",
    ),
    (
        "Main/GText.cpp",
        "Same %d%2s form.",
        "\t\t\t\tint nParams = swscanf( iTemp->wsString.c_str(), u\"%d%2s\", &sNewFont.nSize, wsString );",
        "\t\t\t\tint nParams = a5_u16_scan_int_and_suffix( iTemp->wsString.c_str(), &sNewFont.nSize, wsString, 2 );  // [android] was swscanf %d%2s",
    ),
    (
        "Main/DG.H",
        "CDGPtr<TFunc> is initialised from CObj<Derived>/CPtr<Derived> in three "
        "places (wOSBase.cpp, wAnimation.cpp, aiVolumeCalcer.cpp); MSVC chained "
        "operator T*() into the TFunc* constructor.  Add the converting "
        "constructor, as done for CPtr.",
        """	CDGPtr( TFunc *_pNode ): pNode(_pNode) { nVersion = 0; }""",
        """	CDGPtr( TFunc *_pNode ): pNode(_pNode) { nVersion = 0; }
	// [android] converting ctor/assignment from any smart pointer whose pointee
	// converts to TFunc* (both, or assignment becomes ambiguous between them)
	template<class TOther, class TOtherRef>
	CDGPtr( const CPtrBase<TOther, TOtherRef> &a ): pNode( a.GetPtr() ) { nVersion = 0; }
	template<class TOther, class TOtherRef>
	CDGPtr& operator=( const CPtrBase<TOther, TOtherRef> &a ) { pNode = a.GetPtr(); nVersion = 0; return *this; }""",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 12: the remaining x86 inline assembly in Main
# ---------------------------------------------------------------------------
#  Four MMX sites, all small pixel/vector arithmetic.  Each is replaced by the
#  same arithmetic in C++; where Nival left a C++ version in comments next to
#  the asm (2DSceneSW.cpp) that is the reference.

#  Bound.h -- an axis-aligned bounding-box accumulator that keeps min/max in
#  MMX registers across three calls (Start / Add x N / Store).  The compares
#  are integer pcmpgtd on float bit patterns, a trick valid for IEEE floats of
#  matching sign; the plain float min/max is what it computed.
RULES += [
    (
        "Main/Bound.h",
        "MMX bounding-box accumulator -> the same min/max with the state in a "
        "thread-local instead of MMX registers.",
        re.compile(
            r"// should not be mixed with fpu & mmx code from StartMMXBound to StoreMMXBoundResult\n"
            r"#pragma warning\( disable : 4799 \)\n"
            r"inline void StartMMXBound\( CVec3 \*pMin, CVec3 \*pMax \)\n\{.*?"
            r"inline void StoreMMXBoundResult\( CVec3 \*pMin, CVec3 \*pMax \)\n\{.*?\n\}\n"
            r"#pragma warning\( default : 4799 \)\n",
            re.DOTALL),
        """// [android] Was MMX: min/max held in mm4..mm7 between StartMMXBound and
// StoreMMXBoundResult, compared with pcmpgtd on the float bit patterns.  The
// same accumulator with the state in thread-local storage.  Callers pair the
// three calls within one function, so a single slot per thread is enough.
struct SBoundAccumulator { CVec3 vMin, vMax; };
inline SBoundAccumulator& GetBoundAccumulator() { static thread_local SBoundAccumulator acc; return acc; }
inline void StartMMXBound( CVec3 *pMin, CVec3 *pMax )
{
	SBoundAccumulator &a = GetBoundAccumulator();
	a.vMin = *pMin;
	a.vMax = *pMax;
}
inline void AddMMXBoundPoint( const CVec3 *p )
{
	SBoundAccumulator &a = GetBoundAccumulator();
	if ( p->x < a.vMin.x ) a.vMin.x = p->x;  if ( p->x > a.vMax.x ) a.vMax.x = p->x;
	if ( p->y < a.vMin.y ) a.vMin.y = p->y;  if ( p->y > a.vMax.y ) a.vMax.y = p->y;
	if ( p->z < a.vMin.z ) a.vMin.z = p->z;  if ( p->z > a.vMax.z ) a.vMax.z = p->z;
}
inline void StoreMMXBoundResult( CVec3 *pMin, CVec3 *pMax )
{
	SBoundAccumulator &a = GetBoundAccumulator();
	*pMin = a.vMin;
	*pMax = a.vMax;
}
""",
    ),
]

#  GSceneParticles.h -- particle colour: dwResColor = dwColor (*) dwPColor with
#  the alpha of dwColor replicated into a scale, all in 8-bit lanes.  Reading
#  the asm: per channel, ((c*p)>>8) is scaled by (a>>1) and >>5 with the
#  0x7f000000 alpha override -- i.e. modulate the particle colour by the
#  vertex colour and by its own alpha, alpha forced to 0x7f.
RULES += [
    (
        "Main/GSceneParticles.h",
        "MMX particle colour modulate -> per-channel arithmetic.",
        re.compile(
            r"\t\tDWORD dwResColor = dwPColor;\n\t\t__asm\n\t\t\{.*?movd dwResColor, mm0\n\t\t\}\n",
            re.DOTALL),
        """		DWORD dwResColor = dwPColor;
		{
			// [android] was MMX.  Lane-exact transcription of the instruction
			// sequence (verified bit-for-bit against an emulation of the asm):
			//   ebx = replicate(alpha7) in b,g,r, alpha lane 0x7f
			//   m0 = unpack(color) with 0xff low bytes, >>1 ;  m1 = same for pcolor
			//   m0 = pmulhw(m0, m1) ; m0 = pmulhw(m0, unpack(ebx)) ; >>5 ; packuswb
			const unsigned int nA7 = dwColor >> 25;
			const unsigned int nMask = ( nA7 | ( nA7 << 8 ) | ( nA7 << 16 ) ) | 0x7f000000u;
			unsigned int nOut = 0;
			for ( int lane = 0; lane < 4; ++lane )
			{
				const unsigned int c = ( dwColor >> ( lane * 8 ) ) & 0xff;
				const unsigned int p = ( dwPColor >> ( lane * 8 ) ) & 0xff;
				const unsigned int m = ( nMask >> ( lane * 8 ) ) & 0xff;
				const int m0 = (int)( ( ( c << 8 ) | 0xff ) >> 1 );          // punpcklbw with 0xffff, psrlw 1
				const int m1 = (int)( ( ( p << 8 ) | 0xff ) >> 1 );
				const int m6 = (int)( ( ( m << 8 ) | 0xff ) );               // punpcklbw with 0xffff
				int v = (int)(short)( ( (short)m0 * (short)m1 ) >> 16 );     // pmulhw
				v = (int)(short)( ( (short)v * (short)m6 ) >> 16 );          // pmulhw
				v = ( (unsigned short)v ) >> 5;                              // psrlw 5
				if ( v > 255 ) v = 255;                                      // packuswb
				nOut |= (unsigned int)v << ( lane * 8 );
			}
			dwResColor = nOut;
		}
""",
    ),
]

#  2DSceneSW.cpp -- masked alpha blend, from Nival's own commented C++:
#      dst.c = color.c + ( ( dst.c * ( 256 - a ) ) >> 8 )
RULES += [
    (
        "Main/2DSceneSW.cpp",
        "MMX 'over' blend -> the C++ Nival left in the adjacent comment.",
        re.compile(
            r"\t\t\t\t_asm\n\t\t\t\t\{\n\t\t\t\t\tpxor mm2, mm2\n\t\t\t\t\}\n"
            r"\t\t\t\tfor \( ; pDst < pFinish; \+\+pDst \)\n\t\t\t\t\{\n"
            r"\t\t\t\t\tDWORD color = tex\.Fetch\(\)\.color;\n"
            r"\t\t\t\t\t__asm\n\t\t\t\t\t\{.*?\t\t\t\t\t\}\n"
            r"(.*?)\t\t\t\t\}\n\t\t\t\t__asm emms\n",
            re.DOTALL),
        """				for ( ; pDst < pFinish; ++pDst )
				{
					// [android] was MMX; this is the C++ from the comment that followed it.
					const NGfx::SPixel8888 &color = tex.Fetch();
					NGfx::SPixel8888 &dst = *pDst;
					const int a = color.a;
					dst.r = (unsigned char)( color.r + ( ( dst.r * ( 256 - a ) ) >> 8 ) );
					dst.g = (unsigned char)( color.g + ( ( dst.g * ( 256 - a ) ) >> 8 ) );
					dst.b = (unsigned char)( color.b + ( ( dst.b * ( 256 - a ) ) >> 8 ) );
					dst.a = (unsigned char)( color.a + ( ( dst.a * ( 256 - a ) ) >> 8 ) );
				}
""",
    ),
]

#  SWTexture.cpp -- bilinear resample with 0.15 fixed-point weights.  The asm:
#  unpack four source pixels to 16-bit, halve, lerp horizontally by nXMul,
#  then vertically by nYMul/nYMul1, add a rounding constant, >>5, pack.
RULES += [
    (
        "Main/SWTexture.cpp",
        "MMX bilinear resample -> the same fixed-point lerp per channel.",
        re.compile(
            r"\t\t__asm\n\t\t\{\n\t\t\tmovd mm4, nYMul\n.*?\t\t\}\n"
            r"\t\tfor \( int x = 0; x < nXSize; \+\+x \)\n\t\t\{\n"
            r"\t\t\tint nXMul = \( nUPos & 0x7fff \);\n"
            r"\t\t\tNGfx::SPixel8888 \*pSrc = &pPictureSrc\[nUPos>>15\];\n"
            r"\t\t\t__asm\n\t\t\t\{.*?\t\t\t\}\n",
            re.DOTALL),
        """		for ( int x = 0; x < nXSize; ++x )
		{
			int nXMul = ( nUPos & 0x7fff );
			NGfx::SPixel8888 *pSrc = &pPictureSrc[nUPos>>15];
			{
				// [android] was MMX.  Bilinear lerp of the 2x2 block at pSrc with
				// 0.15 fixed-point weights, same shifts as the asm: halve to 7-bit
				// headroom, lerp horizontally, lerp vertically, +16 round, >>5.
				const unsigned char *p00 = (const unsigned char*)pSrc;
				const unsigned char *p01 = p00 + 4;
				const unsigned char *p10 = p00 + nNextY;
				const unsigned char *p11 = p10 + 4;
				unsigned char *pOut = (unsigned char*)pDst;
				for ( int c = 0; c < 4; ++c )
				{
					int a = ( p00[c] << 8 ) >> 1, b = ( p01[c] << 8 ) >> 1;   // punpcklbw + psrlw 1
					int d = ( p10[c] << 8 ) >> 1, e = ( p11[c] << 8 ) >> 1;
					int top = ( a >> 1 ) + ( ( ( b - a ) * nXMul ) >> 16 );  // psubw/psrlw/pmulhw/paddw
					int bot = ( d >> 1 ) + ( ( ( e - d ) * nXMul ) >> 16 );
					int v = ( ( top * nYMul1 ) >> 16 ) + ( ( bot * nYMul ) >> 16 );
					v = ( v + 0x10 ) >> 5;
					pOut[c] = (unsigned char)( v > 255 ? 255 : ( v < 0 ? 0 : v ) );
				}
			}
""",
    ),
    (
        "Main/SWTexture.cpp",
        "The trailing `_asm emms` and the now-unused MMX rounding constant.",
        re.compile(r"\t_asm emms\n"),
        "",
    ),
]

RULES += [
    (
        "Main/GfxEffects.cpp",
        "GfxEffects.cpp includes <D3D9.h> but uses nothing from it (it only "
        "talks to CRenderContext); drop the include so it builds against the "
        "GLES backend.",
        '#include "StdAfx.h"\n#include <D3D9.h>\n#include "GfxEffects.h"',
        '#include "StdAfx.h"\n// [android] <D3D9.h> include removed: nothing from it is used here\n#include "GfxEffects.h"',
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 13: the D3D9 renderer files, against compat/d3d9gles
# ---------------------------------------------------------------------------
RULES += [
    (
        "*/*.cpp",
        "The engine spells the header <D3D9.h>; on a case-sensitive filesystem "
        "the shim's file is d3d9.h.",
        re.compile(r'#include <D3D9\.h>'),
        '#include <d3d9.h>  // [android] compat/d3d9gles',
    ),
    (
        "Main/GfxBuffers.cpp",
        "ReallyFastShiftingTransfer widened 16-bit indices to 32 bits while "
        "adding the vertex-buffer base -- in MMX.  A loop the compiler "
        "vectorises does the same.",
        re.compile(
            r"static __forceinline void ReallyFastShiftingTransfer\( const unsigned short \*pSrc, int \*pDst, int nSize, int nShift \)\n\{\n\t_asm\n\t\{.*?\n\t\}\n\}",
            re.DOTALL),
        """static __forceinline void ReallyFastShiftingTransfer( const unsigned short *pSrc, int *pDst, int nSize, int nShift )
{
	// [android] was MMX (punpcklwd/paddd over four indices at a time)
	for ( int i = 0; i < nSize; ++i )
		pDst[i] = (int)pSrc[i] + nShift;
}""",
    ),
    (
        "Main/GfxBuffers.cpp",
        "The vertex-buffer sub-allocator locks the whole buffer and writes one "
        "element's range; tell the shim which range so Unlock uploads only that "
        "(see IDirect3DVertexBuffer9::MarkDirty in compat/d3d9gles/d3d9.h).",
        """	virtual void* Lock()
	{
		++nLocked;
		return pBuffer->Lock() + pBuffer->GetStride() * nStart;
	}""",
        """	virtual void* Lock()
	{
		++nLocked;
		unsigned char *pBase = pBuffer->Lock();
		// [android] dirty-range hint for the GLES buffer upload
		pBuffer->GetBuffer()->obj->MarkDirty( pBuffer->GetStride() * nStart, pBuffer->GetStride() * nBufSize );
		return pBase + pBuffer->GetStride() * nStart;
	}""",
    ),
    (
        "Main/GfxBuffers.cpp",
        "Same hint for the 32-bit dynamic index ring: it writes nToDraw "
        "triangles at nLast.",
        """			pDynamicTrisBuffer->Lock( dwFlags );
			S32Triangle *pTri = (S32Triangle*)pDynamicTrisBuffer->pLocked;
			pTri += nLast;
			ReallyFastShiftingTransfer( (const unsigned short*)&pSrcTris[ nSrcStart ], (int*)pTri, nToDraw * 3, nVBStart );""",
        """			pDynamicTrisBuffer->Lock( dwFlags );
			S32Triangle *pTri = (S32Triangle*)pDynamicTrisBuffer->pLocked;
			pTri += nLast;
			pDynamicTrisBuffer->obj->MarkDirty( nLast * sizeof(S32Triangle), nToDraw * sizeof(S32Triangle) );  // [android]
			ReallyFastShiftingTransfer( (const unsigned short*)&pSrcTris[ nSrcStart ], (int*)pTri, nToDraw * 3, nVBStart );""",
    ),
    (
        "Main/GfxBuffers.cpp",
        "Same hint for the 16-bit ring.",
        """			pDynamicTrisBuffer->Lock( dwFlags );
			S3DTriangle *pTri = (S3DTriangle*)pDynamicTrisBuffer->pLocked;
			pTri += nLast;
			// fill tris from source""",
        """			pDynamicTrisBuffer->Lock( dwFlags );
			S3DTriangle *pTri = (S3DTriangle*)pDynamicTrisBuffer->pLocked;
			pTri += nLast;
			pDynamicTrisBuffer->obj->MarkDirty( nLast * sizeof(S3DTriangle), nToDraw * sizeof(S3DTriangle) );  // [android]
			// fill tris from source""",
    ),
]

RULES += [
    (
        "Misc/Basic2.h",
        "OBJECT_NOCOPY_METHODS calls the destructor as `classname::~classname()`. "
        "Inside a class template whose destructor is implicit (GfxBuffers.cpp's "
        "CLinearBuffer, CIBFast) clang cannot find it by qualified name at "
        "definition time; `this->~classname()` resolves the same destructor -- "
        "DestroyContents is virtual and each class instantiates its own, so the "
        "dynamic type is classname -- and is accepted everywhere.",
        re.compile(r"virtual void DestroyContents\(\) \{ classname::~classname\(\);"),
        "virtual void DestroyContents() { this->~classname();",
    ),
    (
        "Main/GfxBuffers.cpp",
        "list::push_front() / insert(pos) with no value: MSVC extension.",
        "\t\tframes.push_front();",
        "\t\tframes.push_front( SBuffersPerFrame() );  // [android] no-argument push_front",
    ),
    (
        "Main/GfxBuffers.cpp",
        "Same for insert(pos).",
        "\t\tframes.insert( frames.begin() )->data.reserve( nReserve );",
        "\t\tframes.insert( frames.begin(), SBuffersPerFrame() )->data.reserve( nReserve );  // [android]",
    ),
    (
        "Main/GfxBuffers.cpp",
        "typename on nested dependent types of the cache template.",
        re.compile(r"(?<!typename )\bCCache::SCachePlace\b"),
        "typename CCache::SCachePlace",
    ),
    (
        "Main/GfxBuffers.cpp",
        "typename on CCache::SStatePlace.",
        re.compile(r"(?<!typename )\bCCache::SStatePlace\b"),
        "typename CCache::SStatePlace",
    ),
    (
        "Main/GfxRender.cpp",
        "`SRenderParam<SFBTransform> transformMode( SFBTransform() );` is the "
        "most-vexing parse -- a function declaration.  Brace-initialise.",
        "static SRenderParam<SFBTransform> transformMode( SFBTransform() );",
        "static SRenderParam<SFBTransform> transformMode( ( SFBTransform() ) );  // [android] most vexing parse",
    ),
]

RULES += [
    (
        "Main/Cursor.cpp",
        "A touch's DOWN/UP arrive in the same frame as the position; the "
        "interface reads the cursor position when it turns the button message "
        "into a UI event (CInterface::ProcessEvent), before the per-frame "
        "Update() would apply it.  Take the absolute position at the top of "
        "Update(), before its 'no time passed' early return, so an Update() "
        "call from ProcessEvent moves the cursor first.",
        """	STime sDelta = pTimer->GetValue() - sLastUpdateTime;
	sLastUpdateTime = pTimer->GetValue();
	if ( sDelta == 0 )
		return;""",
        """	STime sDelta = pTimer->GetValue() - sLastUpdateTime;
	sLastUpdateTime = pTimer->GetValue();
	{	// [android] absolute pointer, applied even when no time has passed
		LONG nAbsX = 0, nAbsY = 0;
		if ( a5_get_pointer_absolute( &nAbsX, &nAbsY ) )
		{
			vCursorPos.x = (float)nAbsX;
			vCursorPos.y = (float)nAbsY;
		}
	}
	if ( sDelta == 0 )
		return;""",
    ),
    (
        "Main/UIInterface.cpp",
        "Same touch problem from the interface side: bring the cursor up to "
        "date before turning a mouse-button message into a positioned UI event.",
        """	SPoint sPoint( pCursor->GetPos().x * 1024 / pView->GetViewportSize().x, pCursor->GetPos().y * 768 / pView->GetViewportSize().y  );
	if ( cmdLButtonUp.ProcessEvent( eEvent ) )""",
        """	if ( eEvent.mMessage.cType != NInput::CT_TIME )
		pCursor->Update();   // [android] touch: position and button arrive together
	SPoint sPoint( pCursor->GetPos().x * 1024 / pView->GetViewportSize().x, pCursor->GetPos().y * 768 / pView->GetViewportSize().y  );
	if ( cmdLButtonUp.ProcessEvent( eEvent ) )""",
    ),
    (
        "Main/Cursor.cpp",
        "The cursor integrates relative mouse deltas.  Touch is absolute: when "
        "the platform has published a pointer position (a5_get_pointer_position, "
        "set on every touch event in back-buffer coordinates) the cursor takes "
        "it directly.  With no touch yet, the original delta path runs unchanged.",
        """	const CVec2 &vSize = NGfx::GetScreenRect();
	vCursorPos.x += AccelerateAxis( bindX.GetDelta() * 250.0f, sDelta );
	vCursorPos.y += AccelerateAxis( bindY.GetDelta() * 250.0f, sDelta );""",
        """	const CVec2 &vSize = NGfx::GetScreenRect();
	// [android] absolute pointer (touch) takes precedence over integrated deltas
	{
		LONG nAbsX = 0, nAbsY = 0;
		const float fDX = bindX.GetDelta(), fDY = bindY.GetDelta();
		if ( a5_get_pointer_absolute( &nAbsX, &nAbsY ) )
		{
			vCursorPos.x = (float)nAbsX;
			vCursorPos.y = (float)nAbsY;
		}
		else
		{
			vCursorPos.x += AccelerateAxis( fDX * 250.0f, sDelta );
			vCursorPos.y += AccelerateAxis( fDY * 250.0f, sDelta );
		}
		if ( getenv( "A5_DEBUG_CURSOR" ) )
		{
			static int nCount = 0;
			if ( ( ++nCount % 30 ) == 0 || fDX != 0 || fDY != 0 )
			{
				char szBuf[ 160 ];
				sprintf( szBuf, "[android] cursor %p: pos %.1f %.1f abs %d (%ld %ld) deltas %.3f %.3f\\n", (void*)this, vCursorPos.x, vCursorPos.y, a5_get_pointer_absolute( 0, 0 ), (long)nAbsX, (long)nAbsY, fDX, fDY );
				OutputDebugString( szBuf );
			}
		}
	}""",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 14: link-time issues in Main
# ---------------------------------------------------------------------------
RULES += [
    (
        "Main/GParticleFormat.cpp",
        "GParticleFormat.h declares `template<class T> void Interpolate(...)`; the "
        ".cpp defines plain overloads for CVec3/CVec2/float/DWORD/short.  MSVC "
        "resolved calls to the template against those overloads at link time; "
        "ISO C++ needs them to be explicit specialisations of the template.",
        re.compile(r"^void Interpolate\( const (\w+) &v1, const \1 &v2, float fAlpha, \1 \*pRes \)", re.MULTILINE),
        r"template<> void Interpolate( const \1 &v1, const \1 &v2, float fAlpha, \1 *pRes )  // [android] explicit specialisation",
    ),
    (
        "Main/BuildingGrid.cpp",
        "CBuildingGrid::At is defined `inline` in the .cpp but declared without "
        "it in the header and called from other files; MSVC still emitted an "
        "out-of-line copy, the ISO linker does not.",
        "inline BYTE& CBuildingGrid::At( const SPoint3 &pt )",
        "BYTE& CBuildingGrid::At( const SPoint3 &pt )  // [android] was inline in the .cpp only",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 15: runtime diagnostics -- a failed allocation that the original
#  would have followed with a null dereference now says what it was.
# ---------------------------------------------------------------------------
RULES += [
    (
        "Main/GfxBuffers.cpp",
        "CTextureCache::Alloc returns 0 both when the cache has no backing "
        "texture and when the quad tree has no room; the caller (GTexture.cpp) "
        "dereferences the result.  Say which it was before that happens.",
        """		if ( !IsValid(pCache) )
			return 0;
		NCache::CQuadTreeElement elem;
		elem.nXSize = GetMSB( nXSize - 1 ) + 1;
		elem.nYSize = GetMSB( nYSize - 1 ) + 1;
		typename CCache::SCachePlace place;
		if ( !pCache->GetPlace( elem, &place ) )
			return 0;""",
        """		if ( !IsValid(pCache) )
		{
			OutputDebugString( "[android] texture cache: Alloc before Init (no cache texture)\\n" );  // [android]
			return 0;
		}
		NCache::CQuadTreeElement elem;
		elem.nXSize = GetMSB( nXSize - 1 ) + 1;
		elem.nYSize = GetMSB( nYSize - 1 ) + 1;
		typename CCache::SCachePlace place;
		if ( !pCache->GetPlace( elem, &place ) )
		{
			char szDiag[ 128 ];  // [android]
			sprintf( szDiag, "[android] texture cache: no place for %dx%d (log2 %dx%d) in %dx%d\\n", nXSize, nYSize, elem.nXSize, elem.nYSize, pBuffer->GetXSize(), pBuffer->GetYSize() );
			OutputDebugString( szDiag );
			return 0;
		}""",
    ),
    (
        "Main/GTexture.cpp",
        "MakeTexture( hdr, ... ) can return 0 (cache full) and RealLoadTexture "
        "then dereferences it.  Report the texture instead of crashing on it.",
        """static NGfx::CTexture* MakeTexture( const SMMPFileHeader &hdr, NGfx::ETextureUsage eUsage, NGfx::EWrap eWrap )
{
	return NGfx::MakeTexture( hdr.nSizeX, hdr.nSizeY, hdr.nNumMipLevels, 
		hdr.format, eUsage, eWrap );
}""",
        """static NGfx::CTexture* MakeTexture( const SMMPFileHeader &hdr, NGfx::ETextureUsage eUsage, NGfx::EWrap eWrap )
{
	NGfx::CTexture *pRes = NGfx::MakeTexture( hdr.nSizeX, hdr.nSizeY, hdr.nNumMipLevels, 
		hdr.format, eUsage, eWrap );
	// [android] diagnostics: the caller dereferences the result
	if ( !pRes || !CDynamicCast<NGfx::I2DBuffer>( pRes ).GetPtr() )
	{
		char szDiag[ 160 ];
		sprintf( szDiag, "[android] MakeTexture failed: %dx%d, %d mips, format %d, usage %d, wrap %d -> %p\\n",
			hdr.nSizeX, hdr.nSizeY, hdr.nNumMipLevels, hdr.format, (int)eUsage, (int)eWrap, (void*)pRes );
		OutputDebugString( szDiag );
	}
	return pRes;
}""",
    ),
    (
        "Misc/Basic2.h",
        "CDynamicCast casts an opaque (forward-declared) object pointer to "
        "CObjectBase* with a C-style cast, i.e. a reinterpret_cast, then "
        "dynamic_casts from there.  MSVC's RTTI locates the complete object from "
        "any vptr so that worked; the Itanium ABI looks for a CObjectBase "
        "subobject at that address, finds none (CObjectBase is a virtual base "
        "and lives at the end of the object) and returns null -- every "
        "CDynamicCast<I2DBuffer>( NGfx::CTexture* ) in Main failed.  "
        "a5_cast_opaque<> (compat) does what MSVC did: read the vptr, find the "
        "complete object and cast from its real type.",
        """		inline CDynamicCast( TT *_ptr ) { ptr = dynamic_cast<T*>((CObjectBase*)_ptr); }""",
        """		inline CDynamicCast( TT *_ptr ) { ptr = a5_cast_opaque<T>( _ptr ); }  // [android] was dynamic_cast<T*>((CObjectBase*)_ptr)""",
    ),
    (
        "Main/*.cpp",
        "Console commands and variables get `this` (an interface object) as a "
        "void* context and recover it with `(CObjectBase*)pContext` before a "
        "dynamic_cast.  Same MSVC-vs-Itanium RTTI difference as above: the "
        "reinterpreted pointer is not a CObjectBase subobject and the "
        "dynamic_cast yields null.  a5_cast_opaque<> reads the vptr and casts "
        "from the complete object.",
        re.compile(r"\(\s*CObjectBase\s*\*\s*\)\s*pContext\b"),
        "a5_cast_opaque<CObjectBase>( pContext )",
    ),
    (
        "MiscDll/LogStream.cpp",
        "The in-game console echoes every line with DebugTrace( \"%S\" ) -- MSVC's "
        "%S is a wchar_t (UTF-16) string; on Android %S means 32-bit wchar_t, so "
        "the echo came out empty.  Convert to UTF-8 and log it as such, so the "
        "console (command replies, script errors, 'file not found') shows in "
        "logcat.",
        """	DebugTrace( "%S\\n", sLine.szText.data() );""",
        """	{	// [android] %S was MSVC's UTF-16; log the console line as UTF-8
		char szUtf8[ 2048 ];
		DebugTrace( "console: %s\\n", a5_u16_to_utf8( sLine.szText.c_str(), szUtf8, sizeof( szUtf8 ) ) );
	}""",
    ),
]

# ---------------------------------------------------------------------------
#  Rule set 16: the retail data's UI templates differ from what this source's
#  interfaces expect.  Adapt the code to the data where the difference is
#  small and mechanical.
# ---------------------------------------------------------------------------
RULES += [
    (
        "Main/UIInterface.h",
        "Let template consumers ask whether a control exists instead of "
        "getting a zero-sized stand-in plus a console error.",
        """	const SWindowInfo& GetControl( const string &szID );
};""",
        """	const SWindowInfo& GetControl( const string &szID );
	bool HasControl( const string &szID ) const;   // [android]
};""",
    ),
    (
        "Main/UIInterface.cpp",
        "HasControl implementation.",
        """const SWindowInfo& CLoader::GetControl( const string &szID )
{""",
        """bool CLoader::HasControl( const string &szID ) const   // [android]
{
	for ( int nTemp = 0; nTemp < windowsSet.size(); nTemp++ )
		if ( windowsSet[nTemp].second.sInfo.szID == szID )
			return true;
	return false;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
const SWindowInfo& CLoader::GetControl( const string &szID )
{""",
    ),
    (
        "Main/iMainMenu.cpp",
        "The retail main-menu template (UI container 347) has no "
        "credits/options/campaign/load/quit buttons and no font-prefix strings "
        "10900/10901; it has a text line 'line_1' where the retail build laid "
        "its menu out.  When the named buttons are absent, build them along "
        "line_1 with the retail strings 10902..10906.  Their IDs are the bind "
        "names, so clicking them works exactly like the original buttons.",
        """			pLogo = new CFlashImage( sEvent.pLoader->GetControl( "logo" ) );

			pCredits = new CHoverButton( sEvent.pLoader->GetControl( "credits" ) );""",
        """			pLogo = new CFlashImage( sEvent.pLoader->GetControl( "logo" ) );

			// [android] retail template: lay the five buttons out along line_1
			if ( !sEvent.pLoader->HasControl( "credits" ) )
			{
				const char *pszIDs[5] = { "campaign", "load", "options", "credits", "quit" };
				const int nStrings[5] = { 10902, 10903, 10904, 10905, 10906 };
				CObj<CHoverButton> *ppButtons[5] = { &pCampaign, &pLoadGame, &pOptions, &pCredits, &pQuitGame };
				for ( int i = 0; i < 5; ++i )
				{
					CHoverButton *pButton = new CHoverButton( sEvent.pLoader->MakeLineControl( pszIDs[i], 1, i, 5 ) );
					pButton->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + u"<center>" + GetDBString( nStrings[i] ) );
					pButton->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + u"<center>" + GetDBString( nStrings[i] ) );
					*ppButtons[i] = pButton;
				}
				break;
			}

			pCredits = new CHoverButton( sEvent.pLoader->GetControl( "credits" ) );""",
    ),
    (
        "Main/UIWindow.h",
        "The retail templates name the 3D view 'view'; this source's menus ask "
        "for 'clientview'.  Fall back before inventing an empty window (which "
        "leaves the camera with a zero rect and no scene drawn).",
        """	CWindow* pChild = pContainer->GetChildByID( szID );
	if ( IsValid( pChild ) )""",
        """	CWindow* pChild = pContainer->GetChildByID( szID );
	if ( !IsValid( pChild ) && szID == "clientview" )   // [android] retail templates say "view"
		pChild = pContainer->GetChildByID( "view" );
	if ( IsValid( pChild ) )""",
    ),
    (
        "Main/UIInterface.h",
        "Helper for menus whose retail template lacks the named buttons this "
        "source expects: place a button along one of the template's text lines "
        "(line_1 / line_2), split nCount ways.",
        """	bool HasControl( const string &szID ) const;   // [android]
};""",
        """	bool HasControl( const string &szID ) const;   // [android]
	// [android] a stand-in for a missing control: slot nIndex of nCount along
	// the template's "line_<nLine>" text line, with szID as the window id
	SWindowInfo MakeLineControl( const string &szID, int nLine, int nIndex, int nCount );
};""",
    ),
    (
        "Main/UIInterface.cpp",
        "MakeLineControl implementation.",
        """bool CLoader::HasControl( const string &szID ) const   // [android]
{""",
        """SWindowInfo CLoader::MakeLineControl( const string &szID, int nLine, int nIndex, int nCount )   // [android]
{
	char szLine[ 16 ];
	sprintf( szLine, "line_%d", nLine );
	SWindowInfo sLine = HasControl( szLine ) ? GetControl( szLine ) : SWindowInfo( pParent, SPoint( 0, 700 + 40 * ( nLine - 1 ) ), SPoint( 1024, 32 ), szLine, STYLE_VISIBLE | STYLE_ENABLED );
	const int nWidth = sLine.sSize.x / Max( 1, nCount );
	return SWindowInfo( sLine.pParent, SPoint( sLine.sPosition.x + nIndex * nWidth, sLine.sPosition.y ), SPoint( nWidth, sLine.sSize.y ), szID, STYLE_VISIBLE | STYLE_ENABLED );
}
////////////////////////////////////////////////////////////////////////////////////////////////////
bool CLoader::HasControl( const string &szID ) const   // [android]
{""",
    ),
    (
        "Main/iSideMenu.cpp",
        "Retail side-selection template (UI container 353) has axis/allies but "
        "no next/cancel buttons and no 10900-style font strings; put NEXT and "
        "BACK on the template's text lines.  (The source labels the 'next' "
        "button ALLIES -- 11132 -- a copy-paste slip; retail string 16820 is "
        "NEXT.)",
        """				pBack = new CHoverButton( sEvent.pLoader->GetControl( "cancel" ) );
				pBack->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + GetDBString( 11174 ) );
				pBack->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + GetDBString( 11174 ) );""",
        """				// [android] retail template: NEXT on line_1, BACK on line_2
				if ( !sEvent.pLoader->HasControl( "cancel" ) )
				{
					pBack = new CHoverButton( sEvent.pLoader->MakeLineControl( "cancel", 2, 0, 1 ) );
					pBack->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + u"<center>" + GetDBString( 11174 ) );
					pBack->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + u"<center>" + GetDBString( 11174 ) );
					pNext = new CScriptHoverButton( sEvent.pLoader->MakeLineControl( "next", 1, 0, 1 ), pInterface );
					pNext->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + u"<center>" + GetDBString( 16820 ) );
					pNext->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + u"<center>" + GetDBString( 16820 ) );
					pAxis = new CScriptHoverButton( sEvent.pLoader->GetControl( "axis" ), pInterface );
					pAxis->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + GetDBString( 11131 ) );
					pAxis->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + GetDBString( 11131 ) );
					pAllies = new CScriptHoverButton( sEvent.pLoader->GetControl( "allies" ), pInterface );
					pAllies->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + GetDBString( 11132 ) );
					pAllies->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + GetDBString( 11132 ) );
					break;
				}
				pBack = new CHoverButton( sEvent.pLoader->GetControl( "cancel" ) );
				pBack->AddTextState( CHoverButton::STATE_HOVER, GetDBString( 11130 ) + GetDBString( 11174 ) );
				pBack->AddTextState( CHoverButton::STATE_NORMAL, GetDBString( 11129 ) + GetDBString( 11174 ) );""",
    ),
    (
        "Main/GTexture.cpp",
        "Diagnostics: a texture that falls back to the checkerboard says which "
        "one and why (missing file vs. unsupported format).",
        """		pValue = MakeTexture( hdr, eUsage, eWrap );
		if ( !RealLoadTexture( pValue.GetPtr(), file.GetStream(), hdr, 0, 0, hdr.nSizeX, hdr.nSizeY, hdr.nNumMipLevels ) )
		{
			ASSERT(0);
			CreateChecker();
		}
	}
	else
	{
		ASSERT(0);
		CreateChecker();
	}""",
        """		pValue = MakeTexture( hdr, eUsage, eWrap );
		if ( !RealLoadTexture( pValue.GetPtr(), file.GetStream(), hdr, 0, 0, hdr.nSizeX, hdr.nSizeY, hdr.nNumMipLevels ) )
		{
			ASSERT(0);
			DebugTrace( "[android] texture %d (file %d): unsupported format %d (%dx%d) - checkerboard\\n", pTex->GetRecordID(), GetRealTextureID( pTex ), (int)hdr.format, hdr.nSizeX, hdr.nSizeY );
			CreateChecker();
		}
	}
	else
	{
		ASSERT(0);
		DebugTrace( "[android] texture %d (file %d): file missing or empty - checkerboard\\n", pTex->GetRecordID(), GetRealTextureID( pTex ) );
		CreateChecker();
	}""",
    ),
    (
        "Main/SWTexture.cpp",
        "The software texture path (terrain painting) only reads A8R8G8B8 "
        "MMPs; the retail terrain textures are DXT-compressed, so every retail "
        "terrain tile came up as the checkerboard.  Decode DXT1/3/5 through the "
        "port's decoder (platform/dxt_decode.cpp).",
        """static void CreateChecker( CSWTextureData *pTexture )
{""",
        """// [android] DXT-compressed MMPs, decoded in software
static void LoadTextureDataDXT( CSWTextureData *pTexture, const SMMPFileHeader &hdr, CDataStream *pFile )
{
	const int nDxt = ( hdr.format == NGfx::CF_DXT1 ) ? 1 : ( hdr.format <= NGfx::CF_DXT3 ? 3 : 5 );
	int nXSize = hdr.nSizeX, nYSize = hdr.nSizeY;
	const int nMips = Max( (int)hdr.nNumMipLevels, 1 );
	pTexture->mips.resize( nMips );
	std::vector<uint8_t> in, out;
	for ( int nMip = 0; nMip < nMips; ++nMip )
	{
		const size_t nBytes = DxtLevelSize( nDxt, nXSize, nYSize );
		in.resize( nBytes );
		pFile->Read( &in[0], (int)nBytes );
		out.assign( (size_t)nXSize * nYSize * 4, 0 );
		DxtDecode( nDxt, &in[0], nBytes, nXSize, nYSize, &out[0] );
		CArray2D<NGfx::SPixel8888> &mip = pTexture->mips[nMip];
		mip.SetSizes( nXSize, nYSize );
		for ( int y = 0; y < nYSize; ++y )
			for ( int x = 0; x < nXSize; ++x )
			{
				const uint8_t *p = &out[ ( (size_t)y * nXSize + x ) * 4 ];
				mip[y][x] = NGfx::SPixel8888( p[0], p[1], p[2], p[3] );
			}
		nXSize = Max( 1, nXSize >> 1 );
		nYSize = Max( 1, nYSize >> 1 );
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////
static void CreateChecker( CSWTextureData *pTexture )
{""",
    ),
    (
        "Main/SWTexture.cpp",
        "Dispatch the DXT formats to the decoder above.",
        """		case NGfx::CF_A8R8G8B8: LoadTextureData<NGfx::SPixel8888>( pValue, hdr.nNumMipLevels, hdr.nSizeX, hdr.nSizeY, file.GetStream() ); break;
		default: ASSERT( 0 ); CreateChecker( pValue ); break;""",
        """		case NGfx::CF_A8R8G8B8: LoadTextureData<NGfx::SPixel8888>( pValue, hdr.nNumMipLevels, hdr.nSizeX, hdr.nSizeY, file.GetStream() ); break;
		case NGfx::CF_DXT1: case NGfx::CF_DXT2: case NGfx::CF_DXT3: case NGfx::CF_DXT4: case NGfx::CF_DXT5:   // [android]
			if ( getenv( "A5_DEBUG_TEXTURES" ) )
				DebugTrace( "[android] software texture %d: DXT%d %dx%d, %d mips, avg 0x%08X\\n", GetKey(), (int)hdr.format, hdr.nSizeX, hdr.nSizeY, hdr.nNumMipLevels, (unsigned)hdr.dwAverageColor );
			LoadTextureDataDXT( pValue, hdr, file.GetStream() ); break;
		default: ASSERT( 0 ); DebugTrace( "[android] software texture: unsupported MMP format %d - checkerboard\\n", (int)hdr.format ); CreateChecker( pValue ); break;""",
    ),
    (
        "Main/SWTexture.cpp",
        "Include the decoder.",
        '#include "SWTexture.h"\n#include "../Misc/StrProc.h"',
        '#include "SWTexture.h"\n#include "../Misc/StrProc.h"\n#include "dxt_decode.h"   // [android]',
    ),
    (
        "Main/GParticleFormat.h",
        "Particle effects are loaded as a memory image whose SParticle records "
        "carry 32-bit file offsets in the TKeyTrack::keys pointer fields, fixed "
        "up in place.  On a 64-bit target the pointer is 8 bytes and the "
        "in-memory SParticle no longer matches the file record; the records are "
        "now decoded into a separate array.",
        """	int nParticles;
	SParticle *particles;

	CParticlesInfo() { nBytes = 0; nParticles = 0; }""",
        """	int nParticles;
	SParticle *particles;
	std::vector<SParticle> particleStore;   // [android] decoded from the 32-bit file records

	CParticlesInfo() { nBytes = 0; nParticles = 0; particles = 0; }""",
    ),
    (
        "Main/GParticleFormat.cpp",
        "Decode the file's SParticle records (2+2 bytes of times, then five "
        "{short nKeys; uint32 offset} tracks, packed to 2) into the native "
        "SParticle array instead of reinterpreting the bytes.",
        """	pValue->particles = (SParticle*)p;

	for ( int nP = 0; nP < pValue->nParticles; ++nP )
	{
		SParticle &particle = pValue->particles[nP];
		particle.pos.keys = (TKey<CVec3>*)(pData + (int)particle.pos.keys);
		particle.rot.keys = (TKey<float>*)(pData + (int)particle.rot.keys);
		particle.scale.keys = (TKey<CVec2>*)(pData + (int)particle.scale.keys);
		particle.color.keys = (TKey<DWORD>*)(pData + (int)particle.color.keys);
		particle.sprite.keys = (TKey<short>*)(pData + (int)particle.sprite.keys);
	}
}""",
        """	// [android] the file record: 32-bit offsets where SParticle has pointers
#pragma pack( push, 2 )
	struct SFileTrack { short nKeys; unsigned int nOffset; };
	struct SFileParticle { short nTStart; short nTEnd; SFileTrack pos, rot, scale, color, sprite; };
#pragma pack( pop )
	const SFileParticle *pFile = (const SFileParticle*)p;
	pValue->particleStore.resize( pValue->nParticles );
	pValue->particles = pValue->nParticles ? &pValue->particleStore[0] : 0;

	for ( int nP = 0; nP < pValue->nParticles; ++nP )
	{
		SParticle &particle = pValue->particles[nP];
		const SFileParticle &f = pFile[nP];
		particle.nTStart = f.nTStart;
		particle.nTEnd = f.nTEnd;
		particle.pos.nKeys = f.pos.nKeys;       particle.pos.keys = (TKey<CVec3>*)(pData + f.pos.nOffset);
		particle.rot.nKeys = f.rot.nKeys;       particle.rot.keys = (TKey<float>*)(pData + f.rot.nOffset);
		particle.scale.nKeys = f.scale.nKeys;   particle.scale.keys = (TKey<CVec2>*)(pData + f.scale.nOffset);
		particle.color.nKeys = f.color.nKeys;   particle.color.keys = (TKey<DWORD>*)(pData + f.color.nOffset);
		particle.sprite.nKeys = f.sprite.nKeys; particle.sprite.keys = (TKey<short>*)(pData + f.sprite.nOffset);
	}
}""",
    ),
    (
        "Main/GRenderExecute.cpp",
        "Retail ambient lights have VapourSwitchTime = 0 (no vapour switching); "
        "this source divides the time by it, so the dynamic-fog constants become "
        "+/-inf and the fog lookup clamps to 'far' - the whole scene fogged over. "
        "Treat 0 as 'no switching'.",
        """				float fTest = fog.fTime / fog.fVapourSwitchTime;""",
        """				float fTest = fog.fVapourSwitchTime > 0 ? fog.fTime / fog.fVapourSwitchTime : 0;   // [android] retail data has 0 here""",
    ),
    (
        "Main/GRenderFactor.cpp",
        "Diagnostics: say what the fog lookup was built from (bring-up of the "
        "fog model against retail light data).",
        """	fog = _fog;
	NGfx::CTextureLock<NGfx::SPixel8888> lock( pFogLookup , 0, NGfx::INPLACE );""",
        """	fog = _fog;
	{	// [android] diagnostics
		char szDiag[ 256 ];
		sprintf( szDiag, "[android] fog lookup: camera h %.2f, vapour %.2f..%.2f density %.3f colour (%.2f %.2f %.2f), fog dist %.1f start %.1f colour (%.2f %.2f %.2f)\\n",
			fog.fCameraHeight, fog.fVapourHeightStart, fog.fHeight, fog.fDensity, fog.vWaterColor.x, fog.vWaterColor.y, fog.vWaterColor.z,
			fog.fDist, fog.fDistStart, fog.vFogColor.x, fog.vFogColor.y, fog.vFogColor.z );
		OutputDebugString( szDiag );
	}
	NGfx::CTextureLock<NGfx::SPixel8888> lock( pFogLookup , 0, NGfx::INPLACE );""",
    ),
    (
        "Main/GRenderFactor.cpp",
        "Vapour density units.  This source's lookup makes VapourDensity an "
        "opacity per world unit (alpha = density * distance inside the layer). "
        "The retail AmbientLights table has densities of 0.07..1 with the "
        "camera inside a 5-unit layer -- with per-unit density every map "
        "would be opaque within a few metres, and the main menu is.  Read the "
        "retail value as per 100 units (density 1 = opaque at F_FOG_DISTANCE), "
        "which is the only reading under which the shipped values make sense.",
        """			fAlpha *= fog.fDensity * fMult;""",
        """			fAlpha *= fog.fDensity * 0.01f * fMult;   // [android] retail VapourDensity is per 100 units""",
    ),
    (
        "ADOImport/BasicDB.h",
        "Declare the retail-data lookups platform/db_retail.cpp provides: "
        "records by authoring name, and the UI-texture id alias for the "
        "renumbered cursor textures.",
        """	void Serialize( CDataStream &file, CStructureSaver::EMode mode );""",
        """	void Serialize( CDataStream &file, CStructureSaver::EMode mode );
	// [android] retail game.db: record id by the table's UserName column (-1 if none),
	// and the cursor-texture alias (see platform/db_retail.cpp)
	int FindRecordByUserName( const char *pszTable, const char *pszUserName );
	int AliasUITexture( int nID );""",
    ),
    (
        "DBFormat/DataFormat.cpp",
        "Cursor UITexture ids are hard-coded in this source and renumbered in "
        "the retail table; resolve through the alias.",
        """CUITexture* GetUITexture( int nID ) { return Get<CUITexture>( nID ); }""",
        """CUITexture* GetUITexture( int nID ) { return Get<CUITexture>( NDatabase::AliasUITexture( nID ) ); }   // [android] retail ids""",
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
        if "*" in pattern:
            if not fnmatch.fnmatch(rel_path, pattern):
                continue
        elif pattern != rel_path:
            continue

        if old is None:
            text = text + new
            applied.append((rel_path, description))
            continue

        # A wildcard rule is expected to miss most files; only exact-path
        # rules count as unmatched when they find nothing.
        wildcard = "*" in pattern
        if hasattr(old, "search"):
            text, count = old.subn(new, text)
            if count:
                applied.append((rel_path, description))
            elif not wildcard:
                unmatched.append((rel_path, description))
        elif old in text:
            text = text.replace(old, new)
            applied.append((rel_path, description))
        elif not wildcard:
            unmatched.append((rel_path, description))
    return text


# ---------------------------------------------------------------------------
#  Driver
# ---------------------------------------------------------------------------
def vcproj_source_list(vcproj_path):
    """The .cpp files a Visual Studio 2003 project actually compiles.

    The Main/ directory holds a couple of sources that were dropped from the
    build (Interface.cpp, iPopupMenu.cpp reference types that no longer
    exist).  Main.vcproj is the authority on what MSVC built, so the Android
    build takes its list from there rather than globbing the directory."""
    with open(vcproj_path, "rb") as f:
        text = f.read().decode("cp1251", "replace")
    names = re.findall(r'RelativePath="(?:\.\\)?([^"\\]+\.(?:cpp|CPP|c))"', text)
    return sorted(set(names))


def write_source_list(src_root, out_root, module):
    vcproj = os.path.join(src_root, module, module + ".vcproj")
    if not os.path.isfile(vcproj):
        return
    names = vcproj_source_list(vcproj)
    with open(os.path.join(out_root, module, "SOURCES.cmake"), "w") as f:
        f.write("# Generated by tools/prepare_sources.py from %s.vcproj -- the files MSVC built.\n" % module)
        f.write("set(%s_VCPROJ_SOURCES\n" % module.upper())
        for name in names:
            f.write("    ${ENGINE_GEN}/%s/%s\n" % (module, name))
        f.write(")\n")
    stats["vcproj-lists"] += 1


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
            text = rewrite_single_arg_insert(text)
            text = rewrite_member_function_arguments(text)
            text = rewrite_address_of_temporaries(text)
            text = apply_rules(text, rel_path, applied, unmatched)

            if not report_only:
                with open(os.path.join(module_out, name), "w", encoding="utf-8") as f:
                    f.write(text)
            stats["files"] += 1

        if not report_only:
            write_source_list(src_root, out_root, module)

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
    print("  insert(end()) no-value  : %d rewritten" % stats["insert-end"])
    print("  member-fn args          : %d qualified as &Class::Method" % stats["member-arg"])
    print("  address-of-temporary    : %d hoisted into locals" % stats["addr-of-temp"])
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
