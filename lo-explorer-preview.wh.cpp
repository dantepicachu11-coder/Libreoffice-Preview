// ==WindhawkMod==
// @id           lo-explorer-preview
// @name         LibreOffice documents in the normal Explorer Preview Handler
// @description  Lets the normal Explorer preview pane show .odt/.ods/.odp/.odg (and .odf) documents by converting them to PDF with LibreOffice and handing that PDF to the PDF preview handler Explorer already uses. No new preview app, no Office, no network.
// @version      0.5.1
// @author       LOPreview
// @include      prevhost.exe
// @compilerOptions -std=c++20 -lole32 -luuid -lshlwapi -lshell32 -ladvapi32 -luser32
// ==/WindhawkMod==

// ---------------------------------------------------------------------------
// GENERATED FILE - do not edit here.
// Assembled by tools/assemble.py from src/prevhost-mod.wh.cpp, src/lop_core.h and
// src/lop_win.h.  Edit those files and re-run the assembler instead.
// ---------------------------------------------------------------------------

// LOPreview 0.5.1 - fixes the ASSOCSTR_SHELLIDLIST compile error of 0.5.0.
// Stale-copy check: the string ASSOCSTR_SHELLIDLIST must NOT appear in this file,
// and the header above must say 0.5.1.  See docs/TESTING.md section 0.
//
// How it works (see docs/ARCHITECTURE.md for the full picture):
//
//   Explorer -> preview pane -> PDF preview handler (existing, untouched)
//                                          ^
//                                          | this proxy hands it an IStream
//                                          | containing a PDF
//   ODF IStream -> prevhost proxy (low integrity)
//                     |  cache hit?  -> use %LOCALAPPDATA%\LOPreview\cache\<key>.pdf
//                     |  otherwise   -> request file in the low scratch folder
//                     v
//                  broker in explorer.exe (medium integrity)
//                     |  copies the document into its own temp folder,
//                     |  runs soffice --headless --convert-to pdf
//                     v
//                  %LOCALAPPDATA%\LOPreview\cache\<key>.pdf
//
// If the broker is not running (Explorer not injected, Windhawk disabled, ...)
// the mod falls back to running LibreOffice itself, and if that is not possible
// either, it renders a diagnostic PDF so the preview pane explains the problem
// instead of staying blank.  Explorer is never left without a preview handler
// response: every failure still returns a valid PDF or a clean error.

#include <windows.h>
#include <shobjidl.h>
#include <objidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "windhawk_api.h"
#include "windhawk_utils.h"

// ==== inlined from src/lop_core.h ====
// LOPreview portable core.
//
// This header is deliberately free of Windows APIs so that it can be compiled
// and unit-tested on any platform (see tests/test_core.cpp).  Everything in
// here is pure logic: string/encoding helpers, ODF type sniffing, cache keys,
// the prevhost<->broker wire protocol, INI parsing, file URL building and a
// dependency-free PDF writer used to render diagnostics inside the preview
// pane when a conversion fails.
//
// NOTE: this file is inlined into the Windhawk mods by tools/assemble.py
// (and installer/build-mod.ps1).  Do not use `#pragma once` / include guards
// that depend on being compiled as a separate translation unit - the
// assembler strips include-guard directives for internal headers.


#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace lop {

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

inline bool IsAsciiAlphaNum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

inline std::string ToLowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

inline std::wstring ToLowerAscii(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return r;
}

inline std::string TrimAscii(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

// Extension of a path (or bare file name), lower-cased, including the dot.
// Returns "" when there is no extension.  Works on UTF-8 or UTF-16 text.
template <typename CharT>
inline std::basic_string<CharT> ExtensionOf(const std::basic_string<CharT>& path) {
    // Strip a trailing directory component first so that "dir.d/file" has no
    // extension.  Both separators are handled because the caller may hand us
    // either a file name or a full Windows path.
    size_t slash = path.find_last_of(static_cast<CharT>('/'));
    size_t bslash = path.find_last_of(static_cast<CharT>('\\'));
    if (bslash != std::basic_string<CharT>::npos &&
        (slash == std::basic_string<CharT>::npos || bslash > slash)) {
        slash = bslash;
    }
    const size_t start = (slash == std::basic_string<CharT>::npos) ? 0 : slash + 1;
    size_t dot = path.find_last_of(static_cast<CharT>('.'));
    if (dot == std::basic_string<CharT>::npos || dot < start) return {};
    return ToLowerAscii(path.substr(dot));
}

inline std::string ExtensionOf(const std::string& path) { return ExtensionOf<char>(path); }
inline std::wstring ExtensionOf(const std::wstring& path) { return ExtensionOf<wchar_t>(path); }

// File name (without directory part) of a path.
template <typename CharT>
inline std::basic_string<CharT> FileNameOf(const std::basic_string<CharT>& path) {
    size_t slash = path.find_last_of(static_cast<CharT>('/'));
    size_t bslash = path.find_last_of(static_cast<CharT>('\\'));
    if (bslash != std::basic_string<CharT>::npos &&
        (slash == std::basic_string<CharT>::npos || bslash > slash)) {
        slash = bslash;
    }
    if (slash == std::basic_string<CharT>::npos) return path;
    return path.substr(slash + 1);
}

inline std::string FileNameOf(const std::string& path) { return FileNameOf<char>(path); }
inline std::wstring FileNameOf(const std::wstring& path) { return FileNameOf<wchar_t>(path); }

template <typename CharT>
inline std::basic_string<CharT> DirectoryOf(const std::basic_string<CharT>& path) {
    size_t slash = path.find_last_of(static_cast<CharT>('/'));
    size_t bslash = path.find_last_of(static_cast<CharT>('\\'));
    if (bslash != std::basic_string<CharT>::npos &&
        (slash == std::basic_string<CharT>::npos || bslash > slash)) {
        slash = bslash;
    }
    if (slash == std::basic_string<CharT>::npos) return {};
    return path.substr(0, slash);
}

inline std::string DirectoryOf(const std::string& path) { return DirectoryOf<char>(path); }
inline std::wstring DirectoryOf(const std::wstring& path) { return DirectoryOf<wchar_t>(path); }

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16/32 conversion (portable)
// ---------------------------------------------------------------------------

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Returns the code point and advances `i`.  Invalid sequences decode to the
// replacement character so that a malformed name can never break a request.
inline uint32_t NextUtf8(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) -> bool {
        return i + k < s.size() &&
               (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    if (c < 0x80) {
        ++i;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && cont(1)) {
        uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        uint32_t cp = ((c & 0x0Fu) << 12) |
                      ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        uint32_t cp = ((c & 0x07u) << 18) |
                      ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                      ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                      (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return 0xFFFD;
}

inline std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = NextUtf8(s, i);
        if (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            } else if (cp > 0xFFFF) {
                out.push_back(0xFFFD);
            } else {
                out.push_back(static_cast<wchar_t>(cp));
            }
        } else {
            out.push_back(static_cast<wchar_t>(cp <= 0x10FFFF ? cp : 0xFFFD));
        }
    }
    return out;
}

inline std::string WideToUtf8(const std::wstring& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp;
        if (sizeof(wchar_t) == 2) {
            const uint32_t w = static_cast<uint32_t>(s[i]);
            if (w >= 0xD800 && w <= 0xDBFF && i + 1 < s.size() &&
                s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                cp = 0x10000 + ((w - 0xD800) << 10) +
                     (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
                ++i;
            } else if (w >= 0xD800 && w <= 0xDFFF) {
                cp = 0xFFFD;  // lone surrogate
            } else {
                cp = w;
            }
        } else {
            cp = static_cast<uint32_t>(s[i]);
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hex encoding (used for every string field on the wire so that paths/Unicode
// can never confuse the line based protocol)
// ---------------------------------------------------------------------------

inline std::string HexEncode(const std::string& raw) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

inline int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool HexDecode(const std::string& hex, std::string& out) {
    out.clear();
    if (hex.size() % 2) return false;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexNibble(hex[i]);
        const int lo = HexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

// ---------------------------------------------------------------------------
// FNV-1a 64 bit - cache keys and content hashes.
// ---------------------------------------------------------------------------

inline uint64_t Fnv1a64(const void* data, size_t len, uint64_t seed = 14695981039346656037ULL) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

inline uint64_t Fnv1a64(const std::string& s, uint64_t seed = 14695981039346656037ULL) {
    return Fnv1a64(s.data(), s.size(), seed);
}

inline uint64_t Fnv1a64(const std::wstring& s, uint64_t seed = 14695981039346656037ULL) {
    return Fnv1a64(s.data(), s.size() * sizeof(wchar_t), seed);
}

inline std::string Hex64(uint64_t v) {
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[v & 0xF];
        v >>= 4;
    }
    return out;
}

// Cache key for a document.  Uses the canonical (lower-cased) full path plus
// the file size and last-write time; that is exactly what the requirements ask
// for and avoids reading gigabytes of document data in the common case.
// `contentHash` is only non-zero in the fallback case where no path is known
// (the stream itself had to be hashed anyway).
inline std::string CacheKeyHex(const std::string& utf8FullPathLower, uint64_t size,
                               uint64_t mtimeUnixMs, uint64_t contentHash = 0) {
    std::string material = utf8FullPathLower;
    material.push_back('\n');
    material += Hex64(size);
    material.push_back('\n');
    material += Hex64(mtimeUnixMs);
    if (contentHash) {
        material.push_back('\n');
        material += Hex64(contentHash);
    }
    return Hex64(Fnv1a64(material));
}

inline std::string ContentKeyHex(uint64_t contentHash) {
    std::string material = "content\n";
    material += Hex64(contentHash);
    return Hex64(Fnv1a64(material));
}

// ---------------------------------------------------------------------------
// ODF detection
// ---------------------------------------------------------------------------

// Extension list the mod acts on.  ".odf" is LibreOffice Math; the remaining
// ODF family members are recognised by content sniffing so that a renamed file
// still works, and can be enabled through config.ini.
inline const char* const kDefaultExtensions = ".odt .ods .odp .odg .odf";

inline bool IsOdfExtension(const std::string& extLower) {
    static const char* kOdf[] = {".odt", ".ods", ".odp", ".odg", ".odf", ".odc",
                                 ".odi", ".odm", ".odb", ".oth", ".ott", ".ots",
                                 ".otp", ".otg", ".otf", ".otc", ".oti", ".otm"};
    for (const char* e : kOdf) {
        if (extLower == e) return true;
    }
    return false;
}

// Flat (single-file XML) ODF variants need the flat extension when they are
// handed to LibreOffice, otherwise the import filter cannot be selected.
inline bool IsFlatOdfExtension(const std::string& extLower) {
    return extLower == ".fodt" || extLower == ".fods" || extLower == ".fodp" ||
           extLower == ".fodg";
}

inline std::string MimetypeToExtension(const std::string& mime) {
    struct Map {
        const char* mime;
        const char* ext;
    };
    static const Map kMap[] = {
        {"application/vnd.oasis.opendocument.text", ".odt"},
        {"application/vnd.oasis.opendocument.text-template", ".ott"},
        {"application/vnd.oasis.opendocument.text-master", ".odm"},
        {"application/vnd.oasis.opendocument.text-web", ".oth"},
        {"application/vnd.oasis.opendocument.spreadsheet", ".ods"},
        {"application/vnd.oasis.opendocument.spreadsheet-template", ".ots"},
        {"application/vnd.oasis.opendocument.presentation", ".odp"},
        {"application/vnd.oasis.opendocument.presentation-template", ".otp"},
        {"application/vnd.oasis.opendocument.graphics", ".odg"},
        {"application/vnd.oasis.opendocument.graphics-template", ".otg"},
        {"application/vnd.oasis.opendocument.formula", ".odf"},
        {"application/vnd.oasis.opendocument.formula-template", ".otf"},
        {"application/vnd.oasis.opendocument.chart", ".odc"},
        {"application/vnd.oasis.opendocument.chart-template", ".otc"},
        {"application/vnd.oasis.opendocument.image", ".odi"},
        {"application/vnd.oasis.opendocument.image-template", ".oti"},
        {"application/vnd.oasis.opendocument.database", ".odb"},
    };
    for (const Map& m : kMap) {
        if (mime == m.mime) return m.ext;
    }
    return {};
}

// ZIP local file header offsets.
constexpr size_t kZipLocalHeaderSize = 30;

inline uint16_t ReadU16LE(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t ReadU32LE(const unsigned char* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                                 (static_cast<uint32_t>(p[3]) << 24));
}

// ODF documents must store the "mimetype" entry first and uncompressed (ODF
// 1.2 part 3), which makes it readable from the first bytes of the stream
// without a ZIP implementation.  We accept a flexible but strict subset:
// entry name must be exactly "mimetype", method 0 (stored) and the payload must
// fit in the sampled header.
inline bool ZipMimetype(const unsigned char* head, size_t n, std::string& mimeOut) {
    mimeOut.clear();
    if (n < kZipLocalHeaderSize + 8) return false;
    if (ReadU32LE(head) != 0x04034b50u) return false;  // local file header
    const uint16_t method = ReadU16LE(head + 8);
    const uint16_t nameLen = ReadU16LE(head + 26);
    const uint16_t extraLen = ReadU16LE(head + 28);
    const uint32_t compSize = ReadU32LE(head + 18);
    const uint32_t uncompSize = ReadU32LE(head + 22);
    if (method != 0) return false;  // ODF mandates stored
    if (nameLen != 8) return false;  // "mimetype"
    const size_t nameOff = kZipLocalHeaderSize;
    if (nameOff + nameLen > n) return false;
    if (std::memcmp(head + nameOff, "mimetype", 8) != 0) return false;
    const size_t dataOff = nameOff + nameLen + extraLen;
    if (dataOff >= n) return false;
    uint32_t metaLen = compSize;
    if (metaLen == 0 && uncompSize != 0) metaLen = uncompSize;
    if (metaLen == 0xFFFFFFFFu) return false;  // ZIP64 descriptor, not supported
    if (metaLen == 0 || metaLen > 128) return false;
    if (dataOff + metaLen > n) return false;
    mimeOut.assign(reinterpret_cast<const char*>(head + dataOff), metaLen);
    // Trim any stray white space/NUL padding.
    while (!mimeOut.empty() &&
           (mimeOut.back() == '\0' || mimeOut.back() == '\n' || mimeOut.back() == '\r' ||
            mimeOut.back() == ' ')) {
        mimeOut.pop_back();
    }
    return !mimeOut.empty();
}

// Flat ODF: <?xml ...><office:document ... office:mimetype="...">.
inline bool FlatXmlMimetype(const unsigned char* head, size_t n, std::string& mimeOut) {
    mimeOut.clear();
    if (n < 16) return false;
    if (std::memcmp(head, "<?xml", 5) != 0 && std::memcmp(head, "<?Xml", 5) != 0) return false;
    std::string text(reinterpret_cast<const char*>(head), n);
    const char* key = "office:mimetype";
    size_t at = text.find(key);
    if (at == std::string::npos) return false;
    at = text.find('=', at);
    if (at == std::string::npos) return false;
    size_t q = text.find_first_of("\"'", at);
    if (q == std::string::npos) return false;
    const char quote = text[q];
    size_t end = text.find(quote, q + 1);
    if (end == std::string::npos) return false;
    mimeOut = text.substr(q + 1, end - q - 1);
    return !mimeOut.empty();
}

enum class SniffKind {
    Unknown,   // not recognised - pass through to the inner handler
    Pdf,       // already a PDF (pass through)
    ZipOther,  // a ZIP that is not an ODF document (pass through)
    XmlOther,  // XML that is not flat ODF (pass through)
    Odf,       // ODF payload, `ext`/`mime` filled in
    TooBig,    // caller could not provide enough header bytes
};

struct SniffResult {
    SniffKind kind = SniffKind::Unknown;
    std::string mime;
    std::string ext;  // ".odt" for ZIP ODF, ".fodt" for flat ODF
    bool flat = false;
    bool Recognised() const { return kind == SniffKind::Odf; }
};

// `extHintLower` is the extension taken from the file name (may be empty).
// The hint is used for flat XML files whose mimetype attribute is missing.
inline SniffResult SniffDocument(const unsigned char* head, size_t n,
                                 const std::string& extHintLower = std::string()) {
    SniffResult r;
    if (!head || n < 4) {
        r.kind = SniffKind::TooBig;
        return r;
    }
    if (n >= 5 && std::memcmp(head, "%PDF-", 5) == 0) {
        r.kind = SniffKind::Pdf;
        return r;
    }
    if (ReadU32LE(head) == 0x04034b50u) {
        std::string mime;
        if (ZipMimetype(head, n, mime)) {
            const std::string ext = MimetypeToExtension(mime);
            if (!ext.empty()) {
                r.kind = SniffKind::Odf;
                r.mime = mime;
                r.ext = ext;
                return r;
            }
            r.kind = SniffKind::ZipOther;
            r.mime = mime;
            return r;
        }
        r.kind = SniffKind::ZipOther;
        return r;
    }
    std::string mime;
    if (FlatXmlMimetype(head, n, mime)) {
        const std::string ext = MimetypeToExtension(mime);
        if (!ext.empty()) {
            r.kind = SniffKind::Odf;
            r.mime = mime;
            r.flat = true;
            r.ext = std::string(".f") + ext.substr(1);  // .odt -> .fodt
            return r;
        }
        r.kind = SniffKind::XmlOther;
        r.mime = mime;
        return r;
    }
    if (IsFlatOdfExtension(extHintLower)) {
        // Flat ODF with a stripped mimetype attribute: still worth trying.
        r.kind = SniffKind::Odf;
        r.flat = true;
        r.ext = extHintLower;
        return r;
    }
    if (head[0] == '<') {
        r.kind = SniffKind::XmlOther;
        return r;
    }
    r.kind = SniffKind::Unknown;
    return r;
}

// A minimal but complete validity check for a PDF produced by LibreOffice (or
// by our own diagnostic writer).  Deliberately cheap: headers plus EOF marker.
inline bool LooksLikePdf(const unsigned char* data, size_t n, std::string* errOut = nullptr) {
    auto fail = [&](const char* msg) {
        if (errOut) *errOut = msg;
        return false;
    };
    if (!data || n < 64) return fail("file too small");
    if (n < 5 || std::memcmp(data, "%PDF-", 5) != 0) return fail("missing %PDF header");
    const size_t tail = 1024;
    const size_t from = n > tail ? n - tail : 0;
    bool eof = false;
    for (size_t i = from; i + 4 < n; ++i) {
        if (std::memcmp(data + i, "%%EOF", 5) == 0) {
            eof = true;
            break;
        }
    }
    if (!eof) return fail("missing %%EOF trailer");
    return true;
}

// ---------------------------------------------------------------------------
// Scratch file names (never trust a path coming from the other side)
// ---------------------------------------------------------------------------

// The client sends only a *bare* file name; both sides compose the full path
// from their own known-good directory.  Path traversal is therefore impossible
// by construction.  This is the single validator used by both sides.
inline bool IsSafeScratchName(const std::string& nameUtf8) {
    if (nameUtf8.empty() || nameUtf8.size() > 96) return false;
    if (nameUtf8 == "." || nameUtf8 == "..") return false;
    for (unsigned char c : nameUtf8) {
        if (c <= 0x20 || c == 0x7F) return false;             // control/space
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||  // separators,
            c == '?' || c == '"' || c == '<' || c == '>' ||   // wildcards,
            c == '|') {                                       // ADS
            return false;
        }
        if (c > 0x7F) return false;  // ASCII only: we generate these names
    }
    // ".." can only appear inside the name now, but reject it anyway.
    if (nameUtf8.find("..") != std::string::npos) return false;
    const std::string ext = ExtensionOf(nameUtf8);
    // Only ODF documents ever appear in the scratch directory, so anything
    // else (.exe, .dll, .bat, ...) is rejected outright.
    return IsOdfExtension(ext) || IsFlatOdfExtension(ext);
}

// Absolute local drive path ("C:\dir\file.odt").  Rejects UNC (\\.\ and
// \\server\share - the requirements forbid network access), device paths and
// relative names.  The broker additionally canonicalises the path and checks
// the volume type before using it.
inline bool IsAbsoluteLocalPath(const std::string& p) {
    if (p.size() < 4) return false;
    if (!IsAsciiAlphaNum(p[0]) || p[1] != ':') return false;
    if (p[2] != '\\' && p[2] != '/') return false;
    return true;
}

// Names we generate for scratch copies and cached PDFs.
inline std::string ScratchBaseName(uint64_t id, const std::string& ext) {
    return Hex64(id).substr(8) + ext;  // 8 hex chars + extension
}

// ---------------------------------------------------------------------------
// INI configuration
// ---------------------------------------------------------------------------

struct Ini {
    std::vector<std::pair<std::string, std::string>> items;

    const std::string* Find(const std::string& keyLower) const {
        for (const auto& kv : items) {
            if (kv.first == keyLower) return &kv.second;
        }
        return nullptr;
    }
    bool Has(const std::string& key) const { return Find(ToLowerAscii(key)) != nullptr; }
    std::string Get(const std::string& key, const std::string& def) const {
        const std::string* v = Find(ToLowerAscii(key));
        return v ? *v : def;
    }
    int GetInt(const std::string& key, int def) const {
        const std::string* v = Find(ToLowerAscii(key));
        if (!v || v->empty()) return def;
        int sign = 1;
        size_t i = 0;
        if ((*v)[0] == '-') {
            sign = -1;
            i = 1;
        }
        int out = 0;
        bool any = false;
        for (; i < v->size(); ++i) {
            const char c = (*v)[i];
            if (c < '0' || c > '9') break;
            out = out * 10 + (c - '0');
            any = true;
            if (out > 100000000) return def;
        }
        return any ? out * sign : def;
    }
    bool GetBool(const std::string& key, bool def) const {
        const std::string* v = Find(ToLowerAscii(key));
        if (!v) return def;
        const std::string s = ToLowerAscii(TrimAscii(*v));
        if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
        if (s == "0" || s == "false" || s == "no" || s == "off") return false;
        return def;
    }
};

// key=value per line; ';' or '#' starts a comment; a UTF-8 BOM is skipped.
inline Ini ParseIni(const std::string& text) {
    Ini ini;
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        const std::string line = TrimAscii(body.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ToLowerAscii(TrimAscii(line.substr(0, eq)));
        const std::string value = TrimAscii(line.substr(eq + 1));
        if (key.empty()) continue;
        bool replaced = false;
        for (auto& kv : ini.items) {
            if (kv.first == key) {  // last one wins
                kv.second = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) ini.items.emplace_back(key, value);
    }
    return ini;
}

// ---------------------------------------------------------------------------
// file:/// URL for -env:UserInstallation
// ---------------------------------------------------------------------------

inline std::string FileUrlFromPath(const std::string& utf8Path) {
    std::string p = utf8Path;
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    if (p.rfind("//", 0) == 0) {
        // UNC: LibreOffice accepts file://server/share.  Rejecting UNC paths is
        // handled by the caller; here we only need a syntactically valid URL.
        return "file:" + p;
    }
    if (!p.empty() && p[0] == '/') return "file://" + p;
    return "file:///" + p;
}

inline bool NeedsUrlEscape(unsigned char c) {
    if (c >= 'a' && c <= 'z') return false;
    if (c >= 'A' && c <= 'Z') return false;
    if (c >= '0' && c <= '9') return false;
    switch (c) {
        case '-':
        case '_':
        case '.':
        case '~':
        case '/':
        case ':':
            return false;
        default:
            return true;
    }
}

inline std::string PercentEncodeUrl(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (!NeedsUrlEscape(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// UTF-8 in, escaped file:/// URL out.
inline std::string FileUrlFromPathEscaped(const std::string& utf8Path) {
    return PercentEncodeUrl(FileUrlFromPath(utf8Path));
}

// ---------------------------------------------------------------------------
// prevhost <-> broker wire protocol
//
// Line oriented UTF-8 text; every string field is hex encoded so that paths
// with spaces/Unicode/any punctuation can never break framing.  A message ends
// with an empty line.  One WriteFile emits a whole message.
// ---------------------------------------------------------------------------

constexpr int kProtocolVersion = 1;

struct Request {
    uint32_t id = 0;            // random per request, echoed back
    uint32_t seq = 0;           // per client, increasing (used for cancellation)
    uint32_t clientPid = 0;
    std::string mode;           // "path" (full document path) or "scratch" (bare name)
    std::string nameUtf8;       // full path (mode=path) or bare name (mode=scratch)
    std::string extLower;       // ".odt" / ".fodt" ... extension LO must see
    uint64_t size = 0;
    uint64_t mtimeUnixMs = 0;
    std::string keyHex;         // cache key computed by the client (may be empty)
    bool convertOriginal = false;  // broker may read `nameUtf8` directly
};

inline void AppendField(std::string& out, const char* key, const std::string& value) {
    out += key;
    out += '=';
    out += value;
    out += '\n';
}

inline std::string BuildRequest(const Request& r) {
    std::string out = "LOPREQ " + std::to_string(kProtocolVersion) + "\n";
    AppendField(out, "id", Hex64(r.id));
    AppendField(out, "seq", std::to_string(r.seq));
    AppendField(out, "pid", std::to_string(r.clientPid));
    AppendField(out, "mode", r.mode);
    AppendField(out, "name", HexEncode(r.nameUtf8));
    AppendField(out, "ext", r.extLower);
    AppendField(out, "size", Hex64(r.size));
    AppendField(out, "mtime", Hex64(r.mtimeUnixMs));
    AppendField(out, "key", r.keyHex);
    AppendField(out, "original", r.convertOriginal ? "1" : "0");
    out += '\n';
    return out;
}

inline bool ParseHexU64(const std::string& s, uint64_t& out) {
    out = 0;
    if (s.empty() || s.size() > 16) return false;
    for (char c : s) {
        const int nib = HexNibble(c);
        if (nib < 0) return false;
        out = (out << 4) | static_cast<uint64_t>(nib);
    }
    return true;
}

inline bool ParseDecU64(const std::string& s, uint64_t& out) {
    out = 0;
    if (s.empty() || s.size() > 20) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<uint64_t>(c - '0');
    }
    return true;
}

inline bool ParseDecU32(const std::string& s, uint32_t& out) {
    uint64_t wide = 0;
    if (!ParseDecU64(s, wide) || wide > 0xFFFFFFFFull) return false;
    out = static_cast<uint32_t>(wide);
    return true;
}

inline bool ParseDecU32Legacy(const std::string& s, uint32_t& out) {
    out = 0;
    if (s.empty() || s.size() > 10) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<uint32_t>(c - '0');
    }
    return true;
}

inline bool ParseRequest(const std::string& text, Request& r, std::string& err) {
    r = Request{};
    size_t pos = 0;
    bool first = true;
    bool sawTerminator = false;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            err = "unterminated message";
            return false;
        }
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (first) {
            first = false;
            if (line != "LOPREQ " + std::to_string(kProtocolVersion)) {
                err = "bad magic/version";
                return false;
            }
            continue;
        }
        if (line.empty()) {
            sawTerminator = true;
            break;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = "malformed field";
            return false;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "id") {
            uint64_t v = 0;
            if (!ParseHexU64(value, v)) { err = "bad id"; return false; }
            r.id = static_cast<uint32_t>(v);
        } else if (key == "seq") {
            if (!ParseDecU32(value, r.seq)) { err = "bad seq"; return false; }
        } else if (key == "pid") {
            if (!ParseDecU32(value, r.clientPid)) { err = "bad pid"; return false; }
        } else if (key == "mode") {
            if (value != "path" && value != "scratch") { err = "bad mode"; return false; }
            r.mode = value;
        } else if (key == "name") {
            if (value.size() > 8192 || !HexDecode(value, r.nameUtf8)) {
                err = "bad name";
                return false;
            }
        } else if (key == "ext") {
            r.extLower = ToLowerAscii(value);
        } else if (key == "size") {
            if (!ParseHexU64(value, r.size)) { err = "bad size"; return false; }
        } else if (key == "mtime") {
            if (!ParseHexU64(value, r.mtimeUnixMs)) { err = "bad mtime"; return false; }
        } else if (key == "key") {
            // The key is used as a file name by both sides; only 16 hex digits
            // are ever produced, so anything else is rejected outright rather
            // than sanitised (this is the whole path traversal defence).
            if (!value.empty()) {
                if (value.size() != 16) {
                    err = "bad key length";
                    return false;
                }
                for (char c : value) {
                    if (HexNibble(c) < 0) {
                        err = "bad key characters";
                        return false;
                    }
                }
            }
            r.keyHex = value;
        } else if (key == "original") {
            r.convertOriginal = (value == "1");
        }
        // Unknown keys are ignored so that a newer client stays compatible.
    }
    if (!sawTerminator) {
        err = "missing terminator";
        return false;
    }
    if (r.mode.empty()) {
        err = "missing mode";
        return false;
    }
    if (r.nameUtf8.empty()) {
        err = "missing name";
        return false;
    }
    if (r.mode == "path") {
        if (r.nameUtf8.size() > 4096) {
            err = "name too long";
            return false;
        }
        if (!IsAbsoluteLocalPath(r.nameUtf8)) {
            err = "path mode requires an absolute local path";
            return false;
        }
    } else {
        if (!IsSafeScratchName(r.nameUtf8)) {
            err = "unsafe scratch name";
            return false;
        }
    }
    if (r.extLower.size() < 4 || r.extLower.size() > 5 || r.extLower[0] != '.') {
        err = "bad ext";
        return false;
    }
    for (size_t i = 1; i < r.extLower.size(); ++i) {
        if (!IsAsciiAlphaNum(r.extLower[i])) {
            err = "bad ext";
            return false;
        }
    }
    return true;
}

constexpr int kStatusOk = 0;
constexpr int kStatusFail = 1;
constexpr int kStatusBusy = 2;
constexpr int kStatusCancelled = 3;

struct Response {
    uint32_t id = 0;
    int status = kStatusFail;
    std::string pdfFullPathUtf8;  // valid when status == kStatusOk
    uint64_t pdfSize = 0;
    std::string messageUtf8;      // human readable detail (shown in diagnostics)
};

inline std::string BuildResponse(const Response& r) {
    std::string out = "LOPRES " + std::to_string(kProtocolVersion) + "\n";
    AppendField(out, "id", Hex64(r.id));
    AppendField(out, "status", std::to_string(r.status));
    AppendField(out, "pdf", HexEncode(r.pdfFullPathUtf8));
    AppendField(out, "size", Hex64(r.pdfSize));
    AppendField(out, "msg", HexEncode(r.messageUtf8));
    out += '\n';
    return out;
}

inline bool ParseResponse(const std::string& text, Response& r, std::string& err) {
    r = Response{};
    size_t pos = 0;
    bool first = true;
    bool sawTerminator = false;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            err = "unterminated response";
            return false;
        }
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (first) {
            first = false;
            if (line != "LOPRES " + std::to_string(kProtocolVersion)) {
                err = "bad response magic";
                return false;
            }
            continue;
        }
        if (line.empty()) {
            sawTerminator = true;
            break;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = "malformed response field";
            return false;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "id") {
            uint64_t v = 0;
            if (!ParseHexU64(value, v)) { err = "bad id"; return false; }
            r.id = static_cast<uint32_t>(v);
        } else if (key == "status") {
            uint32_t v = 0;
            if (!ParseDecU32(value, v)) { err = "bad status"; return false; }
            r.status = static_cast<int>(v);
        } else if (key == "pdf") {
            if (!HexDecode(value, r.pdfFullPathUtf8)) { err = "bad pdf field"; return false; }
        } else if (key == "size") {
            if (!ParseHexU64(value, r.pdfSize)) { err = "bad size"; return false; }
        } else if (key == "msg") {
            if (!HexDecode(value, r.messageUtf8)) { err = "bad msg field"; return false; }
        }
    }
    if (!sawTerminator) {
        err = "missing terminator";
        return false;
    }
    if (r.status == kStatusOk && r.pdfFullPathUtf8.empty()) {
        err = "ok without pdf";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostic PDF writer
//
// Every failure path renders a readable page inside the normal preview pane
// instead of leaving the user with an empty pane.  This is intentionally a
// tiny, dependency free PDF writer; tests/test_core.cpp validates the xref
// offsets it produces.
// ---------------------------------------------------------------------------

inline std::vector<std::string> WrapText(const std::string& text, size_t columns) {
    std::vector<std::string> lines;
    if (columns < 8) columns = 8;
    std::string current;
    size_t i = 0;
    auto flush = [&]() {
        if (!current.empty() || !text.empty()) lines.push_back(current);
        current.clear();
    };
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') {
            flush();
            ++i;
            continue;
        }
        if (c == '\r') {
            ++i;
            continue;
        }
        if (c == ' ') {
            // Look ahead: does the next word fit?
            size_t j = i + 1;
            while (j < text.size() && text[j] == ' ') ++j;
            size_t wordEnd = j;
            while (wordEnd < text.size() && text[wordEnd] != ' ' && text[wordEnd] != '\n' &&
                   text[wordEnd] != '\r') {
                ++wordEnd;
            }
            const size_t word = wordEnd - j;
            if (!current.empty() && current.size() + 1 + word > columns) {
                lines.push_back(current);
                current.clear();
                i = j;
                continue;
            }
            if (!current.empty()) current.push_back(' ');
            i = j;
            continue;
        }
        // Hard-wrap very long tokens.
        if (current.size() >= columns) {
            lines.push_back(current);
            current.clear();
        }
        current.push_back(c);
        ++i;
    }
    if (!current.empty()) lines.push_back(current);
    if (lines.empty()) lines.push_back("");
    return lines;
}

inline std::string EscapePdfText(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '(':
            case ')':
            case '\\':
                out.push_back('\\');
                out.push_back(static_cast<char>(c));
                break;
            case '\t':
                out.push_back(' ');
                break;
            default:
                if (c < 0x20 || c > 0x7E) {
                    out.push_back('?');  // WinAnsi safety: diagnostics are ASCII
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

// Builds a one page PDF (A4, Helvetica) containing `lines`, plus an optional
// numbered footer.  Layout: 60pt margins, 12pt leading.
inline std::string BuildTextPdf(const std::vector<std::string>& lines) {
    std::string content = "BT\n/F1 11 Tf\n12 TL\n60 780 Td\n";
    bool first = true;
    for (const std::string& line : lines) {
        const std::string escaped = EscapePdfText(line);
        if (!first) content += "T*\n";
        first = false;
        content += "(" + escaped + ") Tj\n";
    }
    content += "ET\n";
    content += "BT\n/F2 8 Tf\n60 40 Td\n(" +
               EscapePdfText("LOPreview preview diagnostic page") + ") Tj\nET\n";

    // Object numbers are referenced from the dictionaries above, so the order
    // is fixed: 1 catalog, 2 pages, 3 page, 4 Helvetica, 5 contents,
    // 6 Courier, 7 info.
    std::vector<std::pair<std::string, std::string>> objects;
    objects.emplace_back("Catalog", "<< /Type /Catalog /Pages 2 0 R >>");
    objects.emplace_back("Pages", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.emplace_back("Page",
                         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
                         "/Resources << /Font << /F1 4 0 R /F2 6 0 R >> >> "
                         "/Contents 5 0 R >>");
    objects.emplace_back("FontHelvetica", "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    objects.emplace_back("Contents",
                         "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" +
                             content + "endstream");
    objects.emplace_back("FontCourier", "<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>");
    objects.emplace_back("Producer",
                         "<< /Producer (LOPreview) /Title (Preview diagnostics) >>");

    std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    offsets.reserve(objects.size() + 1);
    offsets.push_back(0);  // object 0 is free
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i].second + "\nendobj\n";
    }
    const size_t startxref = pdf.size();
    const size_t count = objects.size() + 1;
    pdf += "xref\n0 " + std::to_string(count) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t i = 1; i < count; ++i) {
        std::string off = std::to_string(offsets[i]);
        while (off.size() < 10) off.insert(off.begin(), '0');
        pdf += off + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + std::to_string(count) + " /Root 1 0 R /Info 7 0 R >>\n";
    pdf += "startxref\n" + std::to_string(startxref) + "\n%%EOF\n";
    return pdf;
}

// Deep structural validation (used by the unit tests; the runtime uses the
// cheaper LooksLikePdf).
inline bool ValidatePdfStructure(const std::string& pdf, std::string& err) {
    auto fail = [&](const std::string& m) {
        err = m;
        return false;
    };
    if (pdf.size() < 64) return fail("too small");
    if (pdf.compare(0, 8, "%PDF-1.4") != 0) return fail("bad header");
    if (pdf.compare(pdf.size() - 6, 5, "%%EOF") != 0) return fail("bad eof");
    const size_t xref = pdf.rfind("startxref");
    if (xref == std::string::npos) return fail("no startxref");
    size_t p = xref + 9;
    while (p < pdf.size() && (pdf[p] == '\n' || pdf[p] == '\r')) ++p;
    size_t numEnd = p;
    while (numEnd < pdf.size() && pdf[numEnd] >= '0' && pdf[numEnd] <= '9') ++numEnd;
    if (numEnd == p) return fail("bad startxref value");
    const size_t claimed = static_cast<size_t>(std::stoul(pdf.substr(p, numEnd - p)));
    if (claimed >= pdf.size()) return fail("startxref out of range");
    if (pdf.compare(claimed, 4, "xref") != 0) return fail("startxref does not point at xref");
    // Verify every in-use entry points at "<n> 0 obj".
    p = claimed + 4;
    while (p < pdf.size() && (pdf[p] == '\n' || pdf[p] == '\r')) ++p;
    size_t headerEnd = pdf.find('\n', p);
    if (headerEnd == std::string::npos) return fail("truncated xref header");
    const std::string header = TrimAscii(pdf.substr(p, headerEnd - p));
    const size_t sp = header.find(' ');
    if (sp == std::string::npos) return fail("bad xref header");
    const size_t firstObj = static_cast<size_t>(std::stoul(header.substr(0, sp)));
    const size_t nObjs = static_cast<size_t>(std::stoul(header.substr(sp + 1)));
    if (firstObj != 0) return fail("unexpected first object number");
    p = headerEnd + 1;
    // Skip the object 0 free entry, then verify every in-use entry.
    {
        size_t freeEnd = pdf.find('\n', p);
        if (freeEnd == std::string::npos) return fail("truncated free xref entry");
        p = freeEnd + 1;
    }
    for (size_t i = 1; i < nObjs; ++i) {
        size_t lineEnd = pdf.find('\n', p);
        if (lineEnd == std::string::npos) return fail("truncated xref table");
        const std::string entry = pdf.substr(p, lineEnd - p);
        if (entry.size() < 18) return fail("short xref entry");
        const size_t off = static_cast<size_t>(std::stoul(entry.substr(0, 10)));
        if (off >= pdf.size()) return fail("xref offset out of range");
        const std::string expect = std::to_string(i) + " 0 obj";
        if (pdf.compare(off, expect.size(), expect) != 0) {
            return fail("xref entry " + std::to_string(i) + " points at the wrong object");
        }
        p = lineEnd + 1;
    }
    // /Length must match the stream payload.
    const size_t streamPos = pdf.find("stream\n");
    if (streamPos == std::string::npos) return fail("no content stream");
    const size_t lenPos = pdf.rfind("/Length ", streamPos);
    if (lenPos == std::string::npos) return fail("no /Length");
    const size_t lenStart = lenPos + 8;
    const size_t lenEnd = pdf.find_first_not_of("0123456789", lenStart);
    const size_t declared = static_cast<size_t>(std::stoul(pdf.substr(lenStart, lenEnd - lenStart)));
    const size_t dataStart = streamPos + 7;
    const size_t endStream = pdf.find("endstream", dataStart);
    if (endStream == std::string::npos) return fail("no endstream");
    if (endStream - dataStart < declared) return fail("/Length longer than stream");
    err.clear();
    return true;
}

}  // namespace lop


// ==== inlined from src/lop_win.h ====
// LOPreview Windows helpers, shared by the prevhost client mod and the
// Explorer broker mod.
//
// Inlined into the generated mods by tools/assemble.py together with
// lop_core.h.  Everything here is called from plain worker threads, never from
// DllMain, and must never throw: Windhawk modules live inside important
// processes (explorer.exe!) so every failure is reported by return value and
// logged instead.
//
// Design notes that matter for correctness:
//
//  * prevhost.exe runs at LOW integrity.  It cannot write to %LOCALAPPDATA%,
//    and a process it starts also runs at low integrity.  It *can* write to
//    %USERPROFILE%\AppData\LocalLow (that folder carries an inheritable low
//    mandatory label), which is where the request hand-off lives.
//
//  * The broker runs inside explorer.exe at the user's normal (medium)
//    integrity level, so it is the only side that runs LibreOffice with a
//    normal profile, and the only side that writes the PDF cache.
//
//  * Hand-off is file based on purpose: a low integrity process cannot create
//    a named pipe that a medium integrity process may write to (no-write-up),
//    and inventing a reverse-direction pipe would be fragile.  Files in the
//    low scratch folder are trivially readable/writable in both directions:
//    low->medium (read down) and medium->low (write down).


#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#include "windhawk_api.h"

namespace lopw {

using lop::Ini;
using lop::Request;
using lop::Response;

// ---------------------------------------------------------------------------
// Tiny string helpers (no printf: Wh_Log("%s") with pre-built strings is both
// safer and compiler-independent)
// ---------------------------------------------------------------------------

inline std::wstring Widen(const char* s) {
    if (!s) return {};
    return lop::Utf8ToWide(std::string(s));
}

inline std::wstring Num(unsigned long long value, int base = 10) {
    if (value == 0) return L"0";
    const wchar_t* digits = base == 16 ? L"0123456789abcdef" : L"0123456789";
    std::wstring out;
    while (value) {
        out.insert(out.begin(), digits[value % static_cast<unsigned>(base)]);
        value /= static_cast<unsigned>(base);
    }
    return out;
}

inline std::wstring HexPtr(const void* p) {
    return L"0x" + Num(reinterpret_cast<unsigned long long>(p), 16);
}

inline std::wstring HrText(HRESULT hr) {
    return L"0x" + Num(static_cast<unsigned long>(hr), 16);
}

inline std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring out = a;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    size_t start = 0;
    while (start < b.size() && (b[start] == L'\\' || b[start] == L'/')) ++start;
    out += b.substr(start);
    return out;
}

// ---------------------------------------------------------------------------
// Logging
//
// Two sinks: Wh_Log (Console/DebugView, always available) and a UTF-8 log file.
// The file sink is disabled automatically when the current process may not
// write it (low integrity prevhost cannot touch %LOCALAPPDATA%, in that case we
// fall back to the low scratch folder).
// ---------------------------------------------------------------------------

enum LogLevel { kLogError = 0, kLogWarn = 1, kLogInfo = 2, kLogDebug = 3 };

struct LoggerState {
    CRITICAL_SECTION cs;
    bool csInit = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::wstring role;
    std::wstring version;
    int level = kLogInfo;
    unsigned long long bytesWritten = 0;
};

inline LoggerState& LogState() {
    static LoggerState s;  // function-local static: initialised on first use
    return s;
}

// `version` is stamped onto every line so that a log excerpt always says which
// build produced it.  (Version 0.4 and 0.5 share the same Windhawk mod id, so
// without this a pasted log could not be attributed to a build.)
inline void LogInit(const std::wstring& role, const std::wstring& version = std::wstring()) {
    LoggerState& s = LogState();
    if (!s.csInit) {
        InitializeCriticalSection(&s.cs);
        s.csInit = true;
    }
    s.role = role;
    s.version = version;
}

inline void LogSetLevel(int level) {
    LogState().level = level < 0 ? 0 : (level > 3 ? 3 : level);
}

inline bool LogSetFile(const std::wstring& path) {
    LoggerState& s = LogState();
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
    s.path = path;
    if (path.empty()) return false;
    s.file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(s.file, &size) && size.QuadPart > 2u * 1024u * 1024u) {
        // Simple rotation: keep one previous file.
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
        std::wstring old = path + L".1";
        DeleteFileW(old.c_str());
        MoveFileW(path.c_str(), old.c_str());
        s.file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (s.file != INVALID_HANDLE_VALUE) {
        s.bytesWritten = 0;
        std::string mark = "\xEF\xBB\xBF--- LOPreview log opened (";
        mark += s.version.empty() ? "unknown version" : lop::WideToUtf8(s.version);
        mark += " ";
        mark += s.role.empty() ? "unknown role" : lop::WideToUtf8(s.role);
        mark += ") ---\r\n";
        DWORD written = 0;
        WriteFile(s.file, mark.data(), static_cast<DWORD>(mark.size()), &written, nullptr);
    }
    return s.file != INVALID_HANDLE_VALUE;
}

inline std::wstring TimestampText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wstring out;
    auto two = [&](unsigned v) {
        std::wstring t = Num(v);
        if (t.size() < 2) t.insert(t.begin(), L'0');
        return t;
    };
    out += two(st.wHour) + L":" + two(st.wMinute) + L":" + two(st.wSecond) + L"." +
           two(st.wMilliseconds);
    return out;
}

inline void LogLine(int level, const std::wstring& text) {
    LoggerState& s = LogState();
    if (level > s.level) return;
    const wchar_t* tag = level == kLogError ? L"E" : (level == kLogWarn ? L"W" : L"I");
    std::wstring prefix = s.version;
    if (!prefix.empty() && !s.role.empty()) prefix += L" ";
    prefix += s.role;
    std::wstring line = std::wstring(L"[") + tag;
    if (!prefix.empty()) line += L" " + prefix;
    line += L"] " + text;
    if (level == kLogError) {
        Wh_Log(L"ERROR %s", line.c_str());
    } else if (level == kLogWarn) {
        Wh_Log(L"WARN  %s", line.c_str());
    } else {
        Wh_Log(L"%s", line.c_str());
    }
    if (s.file != INVALID_HANDLE_VALUE && s.csInit) {
        const std::string utf8 = lop::WideToUtf8(line);
        std::string record = lop::WideToUtf8(TimestampText()) + " " + utf8 + "\r\n";
        EnterCriticalSection(&s.cs);
        DWORD written = 0;
        WriteFile(s.file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr);
        s.bytesWritten += record.size();
        LeaveCriticalSection(&s.cs);
    }
}

inline void LogI(const std::wstring& text) { LogLine(kLogInfo, text); }
inline void LogD(const std::wstring& text) { LogLine(kLogDebug, text); }
inline void LogWarn(const std::wstring& text) { LogLine(kLogWarn, text); }
inline void LogErr(const std::wstring& text) { LogLine(kLogError, text); }

// ---------------------------------------------------------------------------
// Known folders / directories
// ---------------------------------------------------------------------------

inline std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* fallbackEnv) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) {
        std::wstring out(raw);
        CoTaskMemFree(raw);
        if (!out.empty()) return out;
    }
    if (fallbackEnv) {
        wchar_t buf[MAX_PATH * 2]{};
        const DWORD n = GetEnvironmentVariableW(fallbackEnv, buf, ARRAYSIZE(buf));
        if (n > 0 && n < ARRAYSIZE(buf)) return std::wstring(buf, n);
    }
    return {};
}

inline std::wstring LocalAppData() {
    return KnownFolder(FOLDERID_LocalAppData, L"LOCALAPPDATA");
}

inline std::wstring UserProfileDir() {
    return KnownFolder(FOLDERID_Profile, L"USERPROFILE");
}

// %LOCALAPPDATA%\LOPreview  - medium integrity (broker, cache, logs)
inline std::wstring RootDir() {
    const std::wstring lad = LocalAppData();
    if (lad.empty()) return {};
    return JoinPath(lad, L"LOPreview");
}

inline std::wstring CacheDir() { return JoinPath(RootDir(), L"cache"); }
inline std::wstring LogsDir() { return JoinPath(RootDir(), L"logs"); }
inline std::wstring BrokerTmpDir() { return JoinPath(RootDir(), L"tmp"); }
inline std::wstring ConfigPath() { return JoinPath(RootDir(), L"config.ini"); }
inline std::wstring HeartbeatPath() { return JoinPath(RootDir(), L"broker.json"); }
inline std::wstring CachePdfPath(const std::string& keyHexUtf8) {
    return JoinPath(CacheDir(), lop::Utf8ToWide(keyHexUtf8) + L".pdf");
}

// %USERPROFILE%\AppData\LocalLow\LOPreview\scratch - low integrity writable.
inline std::wstring LowRootDir() {
    std::wstring low = KnownFolder(FOLDERID_LocalAppDataLow, nullptr);
    if (low.empty()) {
        const std::wstring profile = UserProfileDir();
        if (profile.empty()) return {};
        low = JoinPath(JoinPath(profile, L"AppData"), L"LocalLow");
    }
    return JoinPath(low, L"LOPreview");
}

inline std::wstring LowScratchDir() { return JoinPath(LowRootDir(), L"scratch"); }
inline std::wstring ScratchPath(const std::string& nameUtf8) {
    return JoinPath(LowScratchDir(), lop::Utf8ToWide(nameUtf8));
}

inline bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    // Create the parents first (small depth, no recursion needed beyond 3).
    const std::wstring parent = lop::DirectoryOf(dir);
    if (!parent.empty() && parent.size() < dir.size()) EnsureDirectory(parent);
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

inline bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline uint64_t NowUnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    // FILETIME is 100ns ticks since 1601-01-01; convert to Unix milliseconds.
    const unsigned long long ticks =
        (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const unsigned long long kEpochDelta = 116444736000000000ULL;
    if (ticks < kEpochDelta) return 0;
    return (ticks - kEpochDelta) / 10000ULL;
}

inline bool FileInfo(const std::wstring& path, uint64_t& size, uint64_t& mtimeUnixMs) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    const unsigned long long ticks =
        (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    mtimeUnixMs = ticks < 116444736000000000ULL
                      ? 0
                      : (ticks - 116444736000000000ULL) / 10000ULL;
    return true;
}

inline bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out,
                          uint64_t maxBytes = 64ull * 1024ull * 1024ull) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, static_cast<DWORD>(out.size() - done), &got, nullptr) ||
            got == 0) {
            CloseHandle(h);
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

inline bool ReadTextFile(const std::wstring& path, std::string& out, uint64_t maxBytes = 1024 * 1024) {
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(path, bytes, maxBytes)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

// Writes through "<name>.tmp" + MoveFileEx so that readers only ever observe
// complete files (this is how the broker publishes PDFs and status files).
inline bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size) {
    static volatile LONG counter = 0;
    const std::wstring tmp =
        path + L"." + Num(static_cast<unsigned long>(GetCurrentProcessId())) + L"." +
        Num(static_cast<unsigned long long>(InterlockedIncrement(&counter))) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(
            (size - done) > 1u * 1024u * 1024u ? 1u * 1024u * 1024u : (size - done));
        if (!WriteFile(h, static_cast<const unsigned char*>(data) + done, chunk, &wrote, nullptr) ||
            wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    if (ok) ok = FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

inline bool WriteTextAtomic(const std::wstring& path, const std::string& utf8) {
    return WriteFileAtomic(path, utf8.data(), utf8.size());
}

inline bool DeleteQuiet(const std::wstring& path) {
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) != FALSE;
}

// Deletes files in `dir` that are older than maxAgeMs.  If maxTotalBytes is
// non-zero and the surviving set is larger, the oldest files are removed until
// the limit is honoured.  Never recurses into subdirectories.
inline void CleanupDirByAge(const std::wstring& dir, uint64_t maxAgeMs, uint64_t maxTotalBytes,
                            const wchar_t* const* extensions, size_t extensionCount,
                            uint64_t minKeepMs = 0) {
    if (dir.empty() || !IsDirectory(dir)) return;
    const uint64_t now = NowUnixMs();
    struct Entry {
        std::wstring path;
        uint64_t size;
        uint64_t mtime;
    };
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        bool wanted = extensionCount == 0;
        if (!wanted) {
            const std::wstring ext = lop::ExtensionOf(std::wstring(fd.cFileName));
            for (size_t i = 0; i < extensionCount; ++i) {
                if (ext == extensions[i]) {
                    wanted = true;
                    break;
                }
            }
        }
        if (!wanted) continue;
        Entry e;
        e.path = JoinPath(dir, fd.cFileName);
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        const unsigned long long ticks =
            (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
            fd.ftLastWriteTime.dwLowDateTime;
        e.mtime = ticks < 116444736000000000ULL ? 0 : (ticks - 116444736000000000ULL) / 10000ULL;
        entries.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::vector<size_t> survivors;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (maxAgeMs && entries[i].mtime && now > entries[i].mtime &&
            now - entries[i].mtime > maxAgeMs) {
            // Age based expiry is safe even for freshly used files because the
            // threshold is large (days), but never touch very recent files.
            if (minKeepMs && now - entries[i].mtime < minKeepMs) {
                survivors.push_back(i);
                continue;
            }
            DeleteQuiet(entries[i].path);
            continue;
        }
        survivors.push_back(i);
    }
    if (!maxTotalBytes) return;
    uint64_t total = 0;
    for (size_t i : survivors) total += entries[i].size;
    if (total <= maxTotalBytes) return;
    std::sort(survivors.begin(), survivors.end(),
              [&](size_t a, size_t b) { return entries[a].mtime < entries[b].mtime; });
    for (size_t i : survivors) {
        if (total <= maxTotalBytes) break;
        if (minKeepMs && now > entries[i].mtime && now - entries[i].mtime < minKeepMs) continue;
        if (DeleteQuiet(entries[i].path)) total -= entries[i].size;
    }
}

// Removes a directory tree, bounded in depth.  Used for stale LibreOffice work
// directories only, never for anything the user owns.
inline bool DeleteDirectoryRecursive(const std::wstring& dir, int depth = 0) {
    if (dir.empty() || depth > 8) return false;
    if (!IsDirectory(dir)) return RemoveDirectoryW(dir.c_str()) != FALSE;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring child = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteDirectoryRecursive(child, depth + 1);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

// Config
inline Ini LoadConfig() {
    std::string text;
    const std::wstring path = ConfigPath();
    if (!ReadTextFile(path, text)) {
        return lop::ParseIni(std::string());
    }
    return lop::ParseIni(text);
}

// Writable check used before we hand a directory to LibreOffice: a low
// integrity prevhost can only write to the low scratch folder, so probing is
// the difference between "works" and "silently does nothing".
inline bool ProbeWritableDir(const std::wstring& dir) {
    if (!EnsureDirectory(dir)) return false;
    const std::wstring probe =
        JoinPath(dir, L"probe-" + Num(static_cast<unsigned long>(GetCurrentProcessId())) + L".tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, "x", 1, &wrote, nullptr) != FALSE;
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// Integrity level detection (diagnostics + choosing what to promise)
// ---------------------------------------------------------------------------

inline std::wstring IntegrityLevelText() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"unknown";
    DWORD needed = 0;
    std::vector<unsigned char> buf(sizeof(TOKEN_MANDATORY_LABEL) + 64);
    std::wstring out = L"unknown";
    if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(),
                           static_cast<DWORD>(buf.size()), &needed)) {
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        if (label->Label.Sid) {
            const DWORD rid = *GetSidSubAuthority(
                label->Label.Sid, static_cast<DWORD>(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
            if (rid < SECURITY_MANDATORY_MEDIUM_RID) {
                out = L"low (" + Num(rid) + L")";
            } else if (rid < SECURITY_MANDATORY_HIGH_RID) {
                out = L"medium (" + Num(rid) + L")";
            } else {
                out = L"high+ (" + Num(rid) + L")";
            }
        }
    }
    CloseHandle(token);
    return out;
}

inline std::wstring ModuleNameOfCurrentProcess() {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    if (n == 0) return {};
    return lop::FileNameOf(std::wstring(buf, n));
}

// explorer.exe hosts the shell window; verifying that we really are that
// process protects against a wide process inclusion list ("*").
inline bool IsShellProcess() {
    HWND shell = GetShellWindow();
    if (!shell) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------------------
// Registry helpers
// ---------------------------------------------------------------------------

inline bool RegReadString(HKEY root, const std::wstring& subKey, const wchar_t* value,
                          std::wstring& out) {
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = RegGetValueW(root, subKey.c_str(), value,
                                    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buf, &size);
    if (st != ERROR_SUCCESS) return false;
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[1024]{};
        if (ExpandEnvironmentStringsW(buf, expanded, ARRAYSIZE(expanded))) {
            out.assign(expanded);
            return true;
        }
    }
    out.assign(buf);
    return true;
}

// ---------------------------------------------------------------------------
// LibreOffice discovery - never assume a single install path
// ---------------------------------------------------------------------------

inline bool IsSoffice(const std::wstring& path) {
    return !path.empty() && PathExists(path) && !IsDirectory(path);
}

inline std::wstring FindLibreOffice(const Ini& cfg, std::wstring& howFound) {
    howFound.clear();
    // 1. explicit configuration
    const std::wstring configured = lop::Utf8ToWide(cfg.Get("soffice_path", ""));
    if (!configured.empty()) {
        if (IsSoffice(configured)) {
            howFound = L"config.ini soffice_path";
            return configured;
        }
        howFound = L"config.ini soffice_path points at a missing file";
    }
    // 2. per-user / machine registry (written by the LibreOffice installer)
    const HKEY unoRoots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    const std::wstring unoKeys[] = {
        L"Software\\LibreOffice\\UNO\\InstallPath",
        L"SOFTWARE\\LibreOffice\\UNO\\InstallPath",
        L"SOFTWARE\\WOW6432Node\\LibreOffice\\UNO\\InstallPath",
        L"Software\\LibreOffice\\UNO\\InstallPath",
    };
    for (HKEY root : unoRoots) {
        for (const std::wstring& key : unoKeys) {
            std::wstring dir;
            if (!RegReadString(root, key, nullptr, dir) || dir.empty()) continue;
            const std::wstring exe = JoinPath(dir, L"soffice.exe");
            if (IsSoffice(exe)) {
                howFound = L"registry " + key;
                return exe;
            }
        }
    }
    // 3. standard install locations
    const wchar_t* envs[] = {L"ProgramW6432", L"ProgramFiles", L"ProgramFiles(x86)"};
    for (const wchar_t* env : envs) {
        wchar_t buf[MAX_PATH * 2]{};
        const DWORD n = GetEnvironmentVariableW(env, buf, ARRAYSIZE(buf));
        if (n == 0 || n >= ARRAYSIZE(buf)) continue;
        const std::wstring exe =
            JoinPath(std::wstring(buf, n), L"LibreOffice\\program\\soffice.exe");
        if (IsSoffice(exe)) {
            howFound = std::wstring(L"standard location under %") + env + L"%";
            return exe;
        }
    }
    // 4. PATH
    {
        wchar_t found[MAX_PATH * 2]{};
        if (SearchPathW(nullptr, L"soffice.exe", nullptr, ARRAYSIZE(found), found, nullptr) &&
            IsSoffice(found)) {
            howFound = L"PATH";
            return std::wstring(found);
        }
    }
    // 5. occasional manual/unpacked installs
    const std::wstring extra[] = {
        lop::Utf8ToWide("C:\\Program Files\\LibreOffice\\program\\soffice.exe"),
        lop::Utf8ToWide("C:\\Program Files (x86)\\LibreOffice\\program\\soffice.exe"),
        JoinPath(LocalAppData(), L"Programs\\LibreOffice\\program\\soffice.exe"),
        lop::Utf8ToWide("C:\\LibreOffice\\program\\soffice.exe"),
    };
    for (const std::wstring& exe : extra) {
        if (IsSoffice(exe)) {
            howFound = L"known location";
            return exe;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Process execution (job object + timeout + output capture)
// ---------------------------------------------------------------------------

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    bool jobAssigned = false;
    unsigned long startError = 0;
    unsigned long exitCode = 0xFFFFFFFF;
    std::string outputUtf8;
};

inline std::wstring BuildEnvironmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    auto add = [&](std::wstring name, const std::wstring& value) {
        for (auto& v : vars) {
            if (_wcsicmp(v.first.c_str(), name.c_str()) == 0) {
                v.second = value;
                return;
            }
        }
        vars.emplace_back(std::move(name), value);
    };
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (LPWCH p = block; *p;) {
            std::wstring entry(p);
            p += entry.size() + 1;
            const size_t eq = entry.find(L'=');
            if (eq == std::wstring::npos || eq == 0) continue;
            add(entry.substr(0, eq), entry.substr(eq + 1));
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& kv : overrides) add(kv.first, kv.second);
    std::wstring out;
    for (const auto& kv : vars) {
        out += kv.first + L"=" + kv.second;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

inline void DrainPipe(HANDLE pipe, std::string& sink, bool store) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return;
        if (available == 0) return;
        char buf[4096];
        DWORD got = 0;
        const DWORD want = available > sizeof(buf) ? sizeof(buf) : available;
        if (!ReadFile(pipe, buf, want, &got, nullptr) || got == 0) return;
        if (!store) continue;
        size_t keep = got;
        const size_t kMax = 32 * 1024;
        if (sink.size() + keep > kMax) {
            if (sink.size() >= kMax) keep = 0;
            else keep = kMax - sink.size();
        }
        sink.append(buf, keep);
    }
}

// Runs `exePath args` with a job object so that timeouts kill the whole tree
// (soffice.exe spawns soffice.bin).  Output is captured without ever blocking
// the child on a full pipe.
inline ProcessResult RunProcessCapture(
    const std::wstring& exePath, const std::wstring& args, const std::wstring& cwd,
    const std::vector<std::pair<std::wstring, std::wstring>>& envOverrides, DWORD timeoutMs,
    HANDLE cancelEvent, bool lowerPriority) {
    ProcessResult result;

    std::wstring commandLine = L"\"" + exePath + L"\"";
    if (!args.empty()) {
        commandLine += L" ";
        commandLine += args;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 64 * 1024)) {
        result.startError = GetLastError();
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdOutput = writePipe;
    si.StartupInfo.hStdError = writePipe;
    si.StartupInfo.hStdInput = nullptr;

    LPPROC_THREAD_ATTRIBUTE_LIST attrList = nullptr;
    bool useHandleList = false;
#ifdef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize) {
        attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attrSize));
        if (attrList && InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
            HANDLE inheritable[1] = {writePipe};
            useHandleList = UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                      inheritable, sizeof(inheritable), nullptr,
                                                      nullptr) != FALSE;
            si.lpAttributeList = attrList;
        }
    }
#endif

    std::wstring envBlock;
    LPVOID envPtr = nullptr;
    if (!envOverrides.empty()) {
        envBlock = BuildEnvironmentBlock(envOverrides);
        envPtr = const_cast<wchar_t*>(envBlock.data());
    }

    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                  (useHandleList ? EXTENDED_STARTUPINFO_PRESENT : 0) |
                  (lowerPriority ? BELOW_NORMAL_PRIORITY_CLASS : 0);

    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(0);

    PROCESS_INFORMATION pi{};
    const BOOL created =
        CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, useHandleList ? TRUE : FALSE,
                       flags, envPtr, cwd.empty() ? nullptr : cwd.c_str(),
                       &si.StartupInfo, &pi);
    const unsigned long createError = created ? 0 : GetLastError();

    if (useHandleList && attrList) {
        DeleteProcThreadAttributeList(attrList);
        HeapFree(GetProcessHeap(), 0, attrList);
    }
    CloseHandle(writePipe);

    if (!created) {
        CloseHandle(readPipe);
        result.startError = createError;
        return result;
    }
    result.started = true;

    // Job object: kills soffice.bin too, and guarantees no orphans if Explorer
    // or prevhost dies mid conversion.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        info.BasicLimitInformation.ActiveProcessLimit = 0;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (AssignProcessToJobObject(job, pi.hProcess)) result.jobAssigned = true;
        else LogWarn(L"AssignProcessToJobObject failed: " + Num(GetLastError()));
    } else {
        LogWarn(L"CreateJobObject failed: " + Num(GetLastError()));
    }

    ResumeThread(pi.hThread);

    const ULONGLONG deadline = GetTickCount64() + (timeoutMs ? timeoutMs : 120000);
    for (;;) {
        DrainPipe(readPipe, result.outputUtf8, true);
        const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) {
            result.cancelled = true;
            break;
        }
        if (timeoutMs && GetTickCount64() > deadline) {
            result.timedOut = true;
            break;
        }
    }

    if (result.timedOut || result.cancelled) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        // Give the pipe a last chance so diagnostics are not lost.
        DrainPipe(readPipe, result.outputUtf8, true);
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) result.exitCode = exitCode;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);  // KILL_ON_JOB_CLOSE: no stray soffice processes
    CloseHandle(readPipe);
    return result;
}

// ---------------------------------------------------------------------------
// LibreOffice profile bootstrap (macro execution off, no update checks, no
// outbound proxy) - applied to a throw-away profile, never to the user's.
// ---------------------------------------------------------------------------

inline void SeedLibreOfficeProfile(const std::wstring& profileDir) {
    const std::wstring userDir = JoinPath(profileDir, L"user");
    if (!EnsureDirectory(userDir)) return;
    const std::wstring path = JoinPath(userDir, L"registrymodifications.xcu");
    if (PathExists(path)) return;  // already seeded
    static const char* kXcu =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<oor:items xmlns:oor=\"http://openoffice.org/2001/registry\" "
        "xmlns:xs=\"http://www.w3.org/2001/XMLSchema\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
        " <item oor:path=\"/org.openoffice.Office.Common/Security/Scripting\">"
        "<prop oor:name=\"MacroSecurityLevel\" oor:op=\"fuse\"><value>3</value></prop></item>\n"
        " <item oor:path=\"/org.openoffice.Office.Common/Security/Scripting\">"
        "<prop oor:name=\"DisableMacrosExecution\" oor:op=\"fuse\"><value>true</value></prop></item>\n"
        " <item oor:path=\"/org.openoffice.Office.Common/Internet/Proxy\">"
        "<prop oor:name=\"ProxyMode\" oor:op=\"fuse\"><value>1</value></prop></item>\n"
        " <item oor:path=\"/org.openoffice.Office.Common/Internet/Proxy\">"
        "<prop oor:name=\"HttpProxyName\" oor:op=\"fuse\"><value>127.0.0.1</value></prop></item>\n"
        " <item oor:path=\"/org.openoffice.Office.Common/Internet/Proxy\">"
        "<prop oor:name=\"HttpProxyPort\" oor:op=\"fuse\"><value>9</value></prop></item>\n"
        " <item oor:path=\"/org.openoffice.Office.Jobs/Jobs/"
        "org.openoffice.Office.Jobs:Job['UpdateCheck']/Arguments\">"
        "<prop oor:name=\"AutoCheckEnabled\" oor:op=\"fuse\"><value>false</value></prop></item>\n"
        " <item oor:path=\"/org.openoffice.Office.Common/Misc\">"
        "<prop oor:name=\"FirstRun\" oor:op=\"fuse\"><value>false</value></prop></item>\n"
        "</oor:items>\n";
    WriteTextAtomic(path, std::string(kXcu));
}

// ---------------------------------------------------------------------------
// The conversion itself - one implementation, used by the broker (medium
// integrity, normal profile) and by the direct prevhost fallback (low
// integrity, everything inside the low scratch folder).
// ---------------------------------------------------------------------------

struct ConvertRequest {
    std::wstring inputPath;    // ODF file to read (already validated)
    std::wstring workDir;      // scratch dir for this conversion (created)
    std::wstring outputDir;    // where LibreOffice writes <base>.pdf
    std::wstring profileDir;   // -env:UserInstallation
    std::wstring tempDir;      // TEMP/TMP override (may be empty)
    std::wstring sofficePath;
    std::wstring filterOptions;  // config pdf_filter, e.g. "pdf" or "pdf:writer_pdf_Export"
    DWORD timeoutMs = 120000;
    HANDLE cancelEvent = nullptr;
    bool lowerPriority = true;
};

struct ConvertResult {
    bool ok = false;
    std::wstring pdfPath;
    std::wstring error;
    std::wstring log;
    DWORD exitCode = 0;
};

// Cheap, allocation-free PDF check used on every cache hit and after every
// conversion: 5 header bytes plus a 1 KiB tail sample (a 200 MB PDF is not read
// into memory just to be validated).
inline bool ValidatePdfFileQuick(const std::wstring& path, uint64_t size, std::wstring& err) {
    if (size < 64) {
        err = L"file is too small (" + Num(size) + L" bytes)";
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"cannot open file (error " + Num(GetLastError()) + L")";
        return false;
    }
    char head[5]{};
    DWORD got = 0;
    const bool headerOk = ReadFile(h, head, sizeof(head), &got, nullptr) && got == 5 &&
                          std::memcmp(head, "%PDF-", 5) == 0;
    char tail[1024]{};
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(size > sizeof(tail) ? size - sizeof(tail) : 0);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    DWORD tailGot = 0;
    const bool tailRead = ReadFile(h, tail, sizeof(tail), &tailGot, nullptr) != FALSE;
    CloseHandle(h);
    bool eofSeen = false;
    if (tailRead) {
        for (DWORD i = 0; i + 5 <= tailGot; ++i) {
            if (std::memcmp(tail + i, "%%EOF", 5) == 0) {
                eofSeen = true;
                break;
            }
        }
    }
    if (!headerOk) {
        err = L"missing %PDF header";
        return false;
    }
    if (!eofSeen) {
        err = L"missing %%EOF trailer (incomplete file?)";
        return false;
    }
    return true;
}

// LibreOffice replaces only the last extension when naming the PDF it writes.
inline std::wstring PdfNameFor(const std::wstring& inputPath) {
    const std::wstring ext = lop::ExtensionOf(inputPath);
    std::wstring base = lop::FileNameOf(inputPath);
    if (!ext.empty() && base.size() > ext.size()) {
        base = base.substr(0, base.size() - ext.size());
    }
    return base + L".pdf";
}

inline ConvertResult ConvertOdfToPdf(const ConvertRequest& req) {
    ConvertResult out;
    if (req.sofficePath.empty()) {
        out.error = L"LibreOffice (soffice.exe) was not found";
        return out;
    }
    if (!EnsureDirectory(req.outputDir)) {
        out.error = L"cannot create output directory " + req.outputDir + L": " + Num(GetLastError());
        return out;
    }
    if (!EnsureDirectory(req.profileDir)) {
        out.error = L"cannot create profile directory " + req.profileDir + L": " + Num(GetLastError());
        return out;
    }
    SeedLibreOfficeProfile(req.profileDir);

    // -env:UserInstallation wants a file URL, percent-encoded.
    const std::string profileUrl = lop::FileUrlFromPathEscaped(lop::WideToUtf8(req.profileDir));

    std::wstring args;
    args += L"--headless --nologo --nodefault --nofirststartwizard --norestore --nolockcheck";
    args += L" -env:UserInstallation=" + lop::Utf8ToWide(profileUrl);
    {
        const std::wstring filter = req.filterOptions.empty() ? std::wstring(L"pdf") : req.filterOptions;
        // A filter with no spaces is passed as-is; anything else is quoted so it
        // stays one argument for LibreOffice.
        args += L" --convert-to " +
                (filter.find(L' ') == std::wstring::npos ? filter : L"\"" + filter + L"\"");
    }
    args += L" --outdir \"" + req.outputDir + L"\"";
    args += L" \"" + req.inputPath + L"\"";

    std::vector<std::pair<std::wstring, std::wstring>> env;
    if (!req.tempDir.empty()) {
        env.emplace_back(L"TEMP", req.tempDir);
        env.emplace_back(L"TMP", req.tempDir);
    }

    LogI(L"running soffice: " + args);
    const ProcessResult pr = RunProcessCapture(req.sofficePath, args, req.workDir, env,
                                               req.timeoutMs, req.cancelEvent, req.lowerPriority);
    out.exitCode = pr.exitCode;
    if (!pr.started) {
        out.error = L"CreateProcess failed for " + req.sofficePath + L" (error " +
                    Num(pr.startError) + L")";
        return out;
    }
    const std::string trimmed = lop::TrimAscii(pr.outputUtf8);
    if (!trimmed.empty()) {
        out.log = lop::Utf8ToWide(trimmed.substr(0, 2048));
        LogI(L"soffice output: " + out.log);
    }
    if (pr.cancelled) {
        out.error = L"conversion cancelled";
        return out;
    }
    if (pr.timedOut) {
        out.error = L"conversion timed out after " + Num(req.timeoutMs / 1000) + L" s";
        return out;
    }

    const std::wstring pdf = JoinPath(req.outputDir, PdfNameFor(req.inputPath));
    uint64_t size = 0;
    uint64_t mtime = 0;
    if (!FileInfo(pdf, size, mtime) || size < 64) {
        // LibreOffice sometimes exits 0 without producing anything (password
        // protected documents, unsupported filter, corrupt file).
        out.error = L"LibreOffice did not produce a PDF (exit code " + Num(pr.exitCode) + L")";
        return out;
    }
    std::wstring pdfErr;
    if (!ValidatePdfFileQuick(pdf, size, pdfErr)) {
        out.error = L"generated file is not a valid PDF: " + pdfErr;
        DeleteQuiet(pdf);
        return out;
    }
    out.ok = true;
    out.pdfPath = pdf;
    return out;
}

}  // namespace lopw


namespace {

constexpr wchar_t kModVersion[] = L"0.5.1";
// Replaced by tools/assemble.py (and installer/build-mod.ps1) with a short
// digest of the sources this file was assembled from, so a log line can be
// matched against the exact source revision.
constexpr wchar_t kBuildId[] = L"490c01f2";
constexpr wchar_t kPreviewHandlerGuid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";

using CoCreateInstance_t = decltype(&CoCreateInstance);
CoCreateInstance_t CoCreateInstance_Original = nullptr;

CLSID g_pdfHandlerClsid{};
bool g_havePdfHandlerClsid = false;
std::wstring g_pdfHandlerHow;
volatile LONG g_seq = 0;

// ---------------------------------------------------------------------------
// Configuration (read once, re-read is possible by reloading the mod)
// ---------------------------------------------------------------------------

struct RuntimeConfig {
    lop::Ini ini;
    bool loaded = false;
};
RuntimeConfig g_cfg;
volatile LONG g_cfgState = 0;  // 0 = not loaded, 1 = loading, 2 = ready

const RuntimeConfig& Cfg() {
    if (InterlockedCompareExchange(&g_cfgState, 1, 0) == 0) {
        g_cfg.ini = lopw::LoadConfig();
        g_cfg.loaded = true;
        InterlockedExchange(&g_cfgState, 2);
    }
    while (InterlockedCompareExchange(&g_cfgState, 2, 2) != 2) Sleep(1);
    return g_cfg;
}

bool CfgBool(const char* key, bool def) { return Cfg().ini.GetBool(key, def); }
int CfgInt(const char* key, int def) { return Cfg().ini.GetInt(key, def); }

bool ExtensionEnabled(const std::string& extLower) {
    const std::string list = Cfg().ini.Get("extensions", lop::kDefaultExtensions);
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find_first_of(" ;,", pos);
        if (end == std::string::npos) end = list.size();
        const std::string token = lop::ToLowerAscii(lop::TrimAscii(list.substr(pos, end - pos)));
        if (!token.empty() && token == extLower) return true;
        pos = end + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PDF preview handler discovery: never hard-code a vendor CLSID, and follow the
// same order Explorer uses to pick the handler for .pdf
// ---------------------------------------------------------------------------

bool ClsidFromRegistryText(const std::wstring& text, CLSID& out) {
    if (text.empty() || text[0] != L'{') return false;
    return SUCCEEDED(CLSIDFromString(text.c_str(), &out));
}

bool ReadHandlerForProgId(HKEY root, const std::wstring& progId, CLSID& out) {
    const std::wstring key = progId + L"\\ShellEx\\" + kPreviewHandlerGuid;
    std::wstring value;
    if (lopw::RegReadString(root, key, nullptr, value) && ClsidFromRegistryText(value, out)) return true;
    // Some handlers register a "PreviewHandler" named value in a subkey.
    if (lopw::RegReadString(root, key, L"PreviewHandler", value) && ClsidFromRegistryText(value, out)) {
        return true;
    }
    return false;
}

bool DiscoverPdfPreviewHandler() {
    CLSID clsid{};
    const std::wstring dotPdf = L".pdf";

    // 1. per-user explicit association
    if (ReadHandlerForProgId(HKEY_CURRENT_USER, L"Software\\Classes\\" + dotPdf, clsid)) {
        g_pdfHandlerHow = L"HKCU\\Software\\Classes\\.pdf";
    }
    // 2. the user's chosen default app (UserChoice -> ProgId -> handler)
    if (g_pdfHandlerHow.empty()) {
        std::wstring progId;
        const std::wstring userChoice =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice";
        if (lopw::RegReadString(HKEY_CURRENT_USER, userChoice, L"ProgId", progId) &&
            !progId.empty()) {
            if (ReadHandlerForProgId(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId, clsid) ||
                ReadHandlerForProgId(HKEY_CLASSES_ROOT, progId, clsid)) {
                g_pdfHandlerHow = L"UserChoice(" + progId + L")";
            }
        }
    }
    // 3. SystemFileAssociations (Adobe and friends register here)
    if (g_pdfHandlerHow.empty() &&
        ReadHandlerForProgId(HKEY_LOCAL_MACHINE,
                             L"SOFTWARE\\Classes\\SystemFileAssociations\\" + dotPdf, clsid)) {
        g_pdfHandlerHow = L"HKLM SystemFileAssociations\\.pdf";
    }
    // 4. machine wide extension association
    if (g_pdfHandlerHow.empty() &&
        ReadHandlerForProgId(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\" + dotPdf, clsid)) {
        g_pdfHandlerHow = L"HKLM\\SOFTWARE\\Classes\\.pdf";
    }
    // 5. the shell's own fallback: the file type's ProgId handler
    if (g_pdfHandlerHow.empty() && ReadHandlerForProgId(HKEY_CLASSES_ROOT, dotPdf, clsid)) {
        g_pdfHandlerHow = L"HKCR\\.pdf";
    }
    if (!g_pdfHandlerHow.empty()) {
        g_pdfHandlerClsid = clsid;
        g_havePdfHandlerClsid = true;
        return true;
    }

    // Steps 1-5 cover every place Windows and the common PDF readers register a
    // preview handler for .pdf: per-user classes, the user's chosen ProgID,
    // SystemFileAssociations, machine-wide classes and the merged HKCR view.
    // There is deliberately no AssocQueryString fallback - the ASSOCSTR enum
    // has no member that returns a preview-handler CLSID, so such a call would
    // be wrong by construction (and one earlier revision got that wrong).
    return false;
}

// ---------------------------------------------------------------------------
// In-memory IStream used for the diagnostic PDF (and as a fallback when the
// scratch folder is not usable).  Implemented here instead of SHCreateMemStream
// so that Stat()/Clone() behaviour is fully under our control.
// ---------------------------------------------------------------------------

class MemoryStream final : public IStream {
   public:
    MemoryStream(std::string data, std::wstring name)
        : data_(std::move(data)), name_(std::move(name)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
            *ppv = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* read) override {
        if (read) *read = 0;
        if (!pv && cb) return STG_E_INVALIDPOINTER;
        const size_t left = pos_ < data_.size() ? data_.size() - pos_ : 0;
        const ULONG take = static_cast<ULONG>(left < cb ? left : cb);
        if (take) {
            std::memcpy(pv, data_.data() + pos_, take);
            pos_ += take;
        }
        if (read) *read = take;
        // Short read = end of stream: return S_FALSE so that handlers which
        // loop on S_OK cannot spin forever.
        return take < cb ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
        long long base = 0;
        switch (origin) {
            case STREAM_SEEK_SET:
                base = 0;
                break;
            case STREAM_SEEK_CUR:
                base = static_cast<long long>(pos_);
                break;
            case STREAM_SEEK_END:
                base = static_cast<long long>(data_.size());
                break;
            default:
                return STG_E_INVALIDFUNCTION;
        }
        const long long target = base + move.QuadPart;
        if (target < 0) return STG_E_INVALIDFUNCTION;
        pos_ = static_cast<size_t>(target);
        if (newPos) newPos->QuadPart = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* dst, ULARGE_INTEGER cb, ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override {
        if (!dst) return STG_E_INVALIDPOINTER;
        if (read) read->QuadPart = 0;
        if (written) written->QuadPart = 0;
        const size_t left = pos_ < data_.size() ? data_.size() - pos_ : 0;
        const size_t take = static_cast<size_t>(cb.QuadPart < left ? cb.QuadPart : left);
        ULONG got = 0;
        const HRESULT hr = dst->Write(data_.data() + pos_, static_cast<ULONG>(take), &got);
        if (SUCCEEDED(hr)) {
            pos_ += got;
            if (read) read->QuadPart = got;
            if (written) written->QuadPart = got;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstat, DWORD grfStatFlag) override {
        if (!pstat) return STG_E_INVALIDPOINTER;
        std::memset(pstat, 0, sizeof(*pstat));
        pstat->type = STGTY_STREAM;
        pstat->cbSize.QuadPart = data_.size();
        pstat->grfMode = STGM_READ;
        if (!(grfStatFlag & STATFLAG_NONAME) && !name_.empty()) {
            const size_t bytes = (name_.size() + 1) * sizeof(wchar_t);
            pstat->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
            if (pstat->pwcsName) std::memcpy(pstat->pwcsName, name_.c_str(), bytes);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override {
        if (!ppstm) return STG_E_INVALIDPOINTER;
        auto* copy = new (std::nothrow) MemoryStream(data_, name_);
        if (!copy) return E_OUTOFMEMORY;
        copy->pos_ = pos_;
        *ppstm = copy;
        return S_OK;
    }

   private:
    ~MemoryStream() = default;
    LONG refs_ = 1;
    std::string data_;
    std::wstring name_;
    size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Document identification
// ---------------------------------------------------------------------------

struct DocIdentity {
    std::wstring fullPath;         // empty when the host did not tell us
    std::string extFromName;       // extension of the file name, lower case
    std::string extForConversion;  // extension LibreOffice must see
    std::string mime;
    uint64_t size = 0;
    uint64_t mtime = 0;
    std::string keyHex;            // cache key (path based, or content based)
    bool isOdf = false;
    bool sniffed = false;
    bool needsStreamCopy = false;  // no path: the client must spool the stream
    std::wstring displayName;      // for diagnostics
};

std::wstring StreamName(IStream* stream) {
    if (!stream) return {};
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_DEFAULT))) return {};
    std::wstring name;
    if (st.pwcsName) {
        name.assign(st.pwcsName);
        CoTaskMemFree(st.pwcsName);
    }
    return name;
}

// Reads up to `max` bytes from the beginning of the stream and restores the
// stream position; the shell hands us seekable streams.
size_t ReadStreamHead(IStream* stream, unsigned char* buffer, size_t max) {
    if (!stream) return 0;
    LARGE_INTEGER zero{};
    ULARGE_INTEGER saved{};
    const bool hadPos = SUCCEEDED(stream->Seek(zero, STREAM_SEEK_CUR, &saved));
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return 0;
    size_t total = 0;
    while (total < max) {
        ULONG got = 0;
        const DWORD want = static_cast<DWORD>(max - total);
        const HRESULT hr = stream->Read(buffer + total, want, &got);
        total += got;
        if (FAILED(hr) || got < want) break;
    }
    if (hadPos) {
        LARGE_INTEGER back{};
        back.QuadPart = static_cast<LONGLONG>(saved.QuadPart);
        stream->Seek(back, STREAM_SEEK_SET, nullptr);
    } else {
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    }
    return total;
}

// The preview handler host exposes the selected item through the site object.
bool PathFromSite(IUnknown* site, std::wstring& path) {
    path.clear();
    if (!site) return false;
    IShellItem* item = nullptr;
    if (FAILED(site->QueryInterface(IID_IShellItem, reinterpret_cast<void**>(&item))) || !item) {
        return false;
    }
    LPWSTR display = nullptr;
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &display);
    if (FAILED(hr) || !display) {
        hr = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &display);
    }
    if (SUCCEEDED(hr) && display) {
        path.assign(display);
        CoTaskMemFree(display);
    }
    item->Release();
    return !path.empty();
}

// ---------------------------------------------------------------------------
// Diagnostic PDF: a failed preview shows what went wrong, in the preview pane.
// ---------------------------------------------------------------------------

std::wstring ScratchBaseForKey(const std::string& keyHex, const std::string& ext) {
    return lop::Utf8ToWide(keyHex + ext);
}

std::string BuildDiagnosticPdf(const DocIdentity& id, const std::vector<std::wstring>& reasons) {
    std::vector<std::string> lines;
    lines.push_back("LOPreview could not produce a PDF for this document.");
    lines.push_back("");
    lines.push_back("Document: " + lop::WideToUtf8(lop::FileNameOf(
                                     id.displayName.empty() ? L"(unknown)" : id.displayName)));
    if (!id.fullPath.empty()) lines.push_back("Path: " + lop::WideToUtf8(id.fullPath));
    if (!id.mime.empty()) lines.push_back("Detected type: " + id.mime);
    lines.push_back("");
    lines.push_back("What was tried:");
    if (reasons.empty()) lines.push_back("  (no details recorded)");
    for (const std::wstring& r : reasons) {
        for (const std::string& l : lop::WrapText("* " + lop::WideToUtf8(r), 96)) {
            lines.push_back("  " + l);
        }
    }
    lines.push_back("");
    lines.push_back("Log files:");
    lines.push_back("  " + lop::WideToUtf8(lopw::JoinPath(lopw::LogsDir(), L"lopreview.log")));
    lines.push_back("  " + lop::WideToUtf8(lopw::JoinPath(lopw::LowScratchDir(), L"prevhost.log")));
    lines.push_back("");
    lines.push_back("Checklist: LibreOffice installed, Windhawk mod lo-explorer-preview enabled for "
                    "prevhost.exe and lo-preview-broker enabled for explorer.exe, and the installer "
                    "run once to register the file associations.");
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    return lop::BuildTextPdf(lop::WrapText(out, 96));
}

// ---------------------------------------------------------------------------
// Broker hand-off (file based, see docs/ARCHITECTURE.md)
// ---------------------------------------------------------------------------

struct BrokerStatus {
    bool alive = false;
    uint64_t tick = 0;
    uint32_t pid = 0;
    std::wstring sofficePath;   // from the heartbeat: what the broker will use
    bool sofficeFound = false;
};

BrokerStatus ReadBrokerStatus(uint64_t maxAgeMs) {
    BrokerStatus status;
    std::string text;
    if (!lopw::ReadTextFile(lopw::HeartbeatPath(), text, 64 * 1024)) return status;
    const lop::Ini ini = lop::ParseIni(text);
    lop::ParseDecU64(ini.Get("tick", ""), status.tick);
    uint32_t pidValue = 0;
    lop::ParseDecU32(ini.Get("pid", ""), pidValue);
    status.pid = pidValue;
    status.sofficePath = lop::Utf8ToWide(ini.Get("soffice", ""));
    status.sofficeFound = ini.Get("soffice_found", "0") == "1";
    const uint64_t now = lopw::NowUnixMs();
    status.alive = status.tick != 0 && now > status.tick && now - status.tick < maxAgeMs;
    return status;
}

// Writes the request atomically; the broker only ever sees complete files.
bool WriteRequestFile(const lop::Request& req, std::wstring& error) {
    if (!lopw::EnsureDirectory(lopw::LowScratchDir())) {
        error = L"cannot create the scratch folder " + lopw::LowScratchDir() + L" (error " +
                lopw::Num(GetLastError()) + L")";
        return false;
    }
    const std::wstring path =
        lopw::ScratchPath(lop::Hex64(req.id) + ".req");
    if (!lopw::WriteTextAtomic(path, lop::BuildRequest(req))) {
        error = L"cannot write the request file " + path + L" (error " + lopw::Num(GetLastError()) +
                L")";
        return false;
    }
    return true;
}

enum class SpoolResult { Ok, Failed };

// Copies the document stream into the low scratch folder (used when the host
// did not give us a file path).  Also computes the content hash for the key.
SpoolResult SpoolStream(IStream* stream, const std::wstring& target, uint64_t& hashOut,
                        std::wstring& error) {
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        error = L"the document stream is not seekable";
        return SpoolResult::Failed;
    }
    HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"cannot create " + target + L" (error " + lopw::Num(GetLastError()) + L")";
        return SpoolResult::Failed;
    }
    const uint64_t maxBytes =
        static_cast<uint64_t>(CfgInt("max_document_mb", 512)) * 1024ull * 1024ull;
    std::vector<unsigned char> buffer(1 << 20);
    uint64_t hash = 14695981039346656037ULL;
    uint64_t total = 0;
    bool ok = true;
    for (;;) {
        ULONG got = 0;
        const HRESULT hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &got);
        if (FAILED(hr)) {
            error = L"reading the document stream failed (" + lopw::HrText(hr) + L")";
            ok = false;
            break;
        }
        if (got == 0) break;
        total += got;
        if (total > maxBytes) {
            error = L"document is larger than max_document_mb";
            ok = false;
            break;
        }
        hash = lop::Fnv1a64(buffer.data(), got, hash);
        DWORD written = 0;
        if (!WriteFile(h, buffer.data(), got, &written, nullptr) || written != got) {
            const unsigned long err = GetLastError();
            error = L"writing the document copy failed (error " + lopw::Num(err) + L")";
            if (err == ERROR_DISK_FULL) error = L"not enough free disk space";
            ok = false;
            break;
        }
    }
    CloseHandle(h);
    if (!ok) lopw::DeleteQuiet(target);
    hashOut = hash;
    return ok ? SpoolResult::Ok : SpoolResult::Failed;
}

// Waits for the broker's answer: the cached PDF or a .err status file.
enum class WaitOutcome { PdfReady, Reported, Cancelled, TimedOut, BrokerGone };

struct WaitResult {
    WaitOutcome outcome = WaitOutcome::TimedOut;
    std::wstring message;
};

WaitResult WaitForBrokerResult(const std::string& keyHex, HANDLE cancelEvent, DWORD timeoutMs,
                              const std::wstring& pdfPath) {
    WaitResult result;
    const std::wstring statusPath = lopw::ScratchPath(keyHex + ".err");
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    ULONGLONG lastBrokerCheck = 0;
    uint64_t lastSize = 0;
    uint64_t lastMtime = 0;
    bool brokerKnown = true;
    for (;;) {
        uint64_t size = 0;
        uint64_t mtime = 0;
        std::wstring err;
        if (lopw::FileInfo(pdfPath, size, mtime)) {
            if (size != lastSize || mtime != lastMtime) {
                lastSize = size;
                lastMtime = mtime;
                if (lopw::ValidatePdfFileQuick(pdfPath, size, err)) {
                    result.outcome = WaitOutcome::PdfReady;
                    return result;
                }
            }
        } else {
            lastSize = 0;
            lastMtime = 0;
        }
        if (lopw::PathExists(statusPath)) {
            std::string text;
            if (lopw::ReadTextFile(statusPath, text, 64 * 1024)) {
                result.message = lop::Utf8ToWide(text);
            } else {
                result.message = L"the broker reported a failure (status file unreadable)";
            }
            lopw::DeleteQuiet(statusPath);
            result.outcome = WaitOutcome::Reported;
            return result;
        }
        if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0) {
            result.outcome = WaitOutcome::Cancelled;
            return result;
        }
        if (GetTickCount64() > deadline) {
            result.outcome = WaitOutcome::TimedOut;
            return result;
        }
        const ULONGLONG now = GetTickCount64();
        if (brokerKnown && now - lastBrokerCheck > 5000) {
            lastBrokerCheck = now;
            if (!ReadBrokerStatus(20000).alive) {
                brokerKnown = false;
                result.outcome = WaitOutcome::BrokerGone;
                return result;
            }
        }
        Sleep(50);
    }
}

// ---------------------------------------------------------------------------
// Direct fallback: run LibreOffice from prevhost itself.
//
// prevhost is low integrity, so everything (profile, TEMP, output) has to live
// inside the low scratch folder.  Whether this works depends on the machine;
// the log records the exact reason either way.
// ---------------------------------------------------------------------------

std::wstring ResolveScratchDir(std::wstring& why) {
    const std::wstring low = lopw::LowScratchDir();
    if (lopw::ProbeWritableDir(low)) return low;
    why += L"the low integrity scratch folder is not writable (" + low + L"); ";
    const std::wstring temp = lopw::JoinPath(lopw::LowRootDir(), L"temp");
    if (lopw::ProbeWritableDir(temp)) return temp;
    why += L"the low temp folder is not writable; ";
    return {};
}

bool DirectConvert(const DocIdentity& id, const std::wstring& spooledDocument,
                   HANDLE cancelEvent, std::wstring& pdfPathOut, std::wstring& why) {
    std::wstring scratchWhy;
    const std::wstring scratch = ResolveScratchDir(scratchWhy);
    if (scratch.empty()) {
        why += scratchWhy;
        return false;
    }
    std::wstring error;
    std::wstring input = spooledDocument;
    if (input.empty()) {
        if (id.fullPath.empty()) {
            why += L"no document path and no spooled copy; ";
            return false;
        }
        // Copy the original into the scratch folder: LibreOffice writes lock
        // files next to whatever it opens, and the user's folder must stay clean.
        input = lopw::JoinPath(scratch, lop::Utf8ToWide(id.keyHex + id.extForConversion));
        if (!CopyFileW(id.fullPath.c_str(), input.c_str(), FALSE)) {
            why += L"cannot copy the document into the scratch folder (error " +
                    lopw::Num(GetLastError()) + L"); ";
            return false;
        }
    }
    std::wstring howFound;
    const std::wstring soffice = lopw::FindLibreOffice(Cfg().ini, howFound);
    if (soffice.empty()) {
        why += L"LibreOffice was not found (registry, %ProgramFiles%, PATH and known locations); ";
        return false;
    }
    lopw::LogI(L"direct conversion using " + soffice + L" (" + howFound + L") at integrity " +
               lopw::IntegrityLevelText());

    lopw::ConvertRequest creq;
    creq.inputPath = input;
    creq.workDir = scratch;
    creq.outputDir = lopw::JoinPath(scratch, L"out");
    creq.profileDir = lopw::JoinPath(scratch, L"lo-profile");
    creq.tempDir = scratch;
    creq.sofficePath = soffice;
    creq.filterOptions = lop::Utf8ToWide(Cfg().ini.Get("pdf_filter", "pdf"));
    creq.timeoutMs = static_cast<DWORD>(CfgInt("timeout_seconds", 120)) * 1000u;
    creq.cancelEvent = cancelEvent;
    creq.lowerPriority = CfgBool("lower_priority", true);

    const lopw::ConvertResult r = lopw::ConvertOdfToPdf(creq);
    if (!r.ok) {
        why += r.error + L" (direct, low integrity); ";
        if (!r.log.empty()) why += L"soffice said: " + r.log + L"; ";
        return false;
    }
    pdfPathOut = r.pdfPath;
    return true;
}

// ---------------------------------------------------------------------------
// The proxy handed to the shell in place of the real preview handler
// ---------------------------------------------------------------------------

class StreamProxy final : public IInitializeWithStream,
                          public IInitializeWithFile,
                          public IPreviewHandler,
                          public IObjectWithSite {
   public:
    StreamProxy(IUnknown* inner, REFCLSID createdClsid, bool createdIsPdfHandler)
        : innerUnknown_(inner), createdClsid_(createdClsid), createdIsPdfHandler_(createdIsPdfHandler) {
        if (inner) {
            inner->AddRef();
            inner->QueryInterface(IID_PPV_ARGS(&innerInitStream_));
            inner->QueryInterface(IID_PPV_ARGS(&innerInitFile_));
            inner->QueryInterface(IID_PPV_ARGS(&innerPreview_));
            inner->QueryInterface(IID_PPV_ARGS(&innerSite_));
        }
        cancelEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IInitializeWithFile) {
            *ppv = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IPreviewHandler) {
            *ppv = static_cast<IPreviewHandler*>(this);
        } else if (riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else {
            return innerUnknown_ ? innerUnknown_->QueryInterface(riid, ppv) : E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (left == 0) delete this;
        return left;
    }

    // --- IInitializeWithStream / IInitializeWithFile -------------------------
    HRESULT STDMETHODCALLTYPE Initialize(IStream* stream, DWORD grfMode) override;
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR filePath, DWORD grfMode) override;
    HRESULT InitializeInternal(IStream* stream, LPCWSTR pathHint, DWORD grfMode);

    // --- IPreviewHandler ----------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd, const RECT* rect) override {
        return innerPreview_ ? innerPreview_->SetWindow(hwnd, rect) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE SetRect(const RECT* rect) override {
        return innerPreview_ ? innerPreview_->SetRect(rect) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE DoPreview() override {
        return innerPreview_ ? innerPreview_->DoPreview() : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Unload() override {
        // The shell calls this when the selection changes or the pane closes:
        // use it to abort a still-running hand-off.
        if (cancelEvent_) SetEvent(cancelEvent_);
        HRESULT hr = innerPreview_ ? innerPreview_->Unload() : S_OK;
        if (pdfStream_) {
            pdfStream_->Release();
            pdfStream_ = nullptr;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        return innerPreview_ ? innerPreview_->SetFocus() : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE QueryFocus(HWND* phwnd) override {
        return innerPreview_ ? innerPreview_->QueryFocus(phwnd) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG* msg) override {
        return innerPreview_ ? innerPreview_->TranslateAccelerator(msg) : S_FALSE;
    }

    // --- IObjectWithSite ----------------------------------------------------
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (site_) {
            site_->Release();
            site_ = nullptr;
        }
        site_ = site;
        if (site_) site_->AddRef();
        return innerSite_ ? innerSite_->SetSite(site) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) override {
        return innerSite_ ? innerSite_->GetSite(riid, ppv) : E_NOINTERFACE;
    }

   private:
    ~StreamProxy();

    // Replaces the wrapped handler with the PDF preview handler.  Only used
    // when Explorer handed us a LibreOffice document but routed it to a
    // different handler (i.e. the association is missing): the preview then
    // still works through the normal PDF handler.
    bool AdoptPdfHandler(const std::wstring& why);

    HRESULT PreviewDocument(IStream* stream, DocIdentity& id, DWORD grfMode);
    bool BuildIdentity(IStream* stream, const std::wstring& nameHint, DocIdentity& id,
                       std::vector<std::wstring>& notes);
    HRESULT HandOff(const std::string& data, const std::wstring& name);

    LONG refs_ = 1;
    IUnknown* innerUnknown_ = nullptr;
    IInitializeWithStream* innerInitStream_ = nullptr;
    IInitializeWithFile* innerInitFile_ = nullptr;
    IPreviewHandler* innerPreview_ = nullptr;
    IObjectWithSite* innerSite_ = nullptr;
    IUnknown* site_ = nullptr;
    IStream* pdfStream_ = nullptr;  // kept alive for the handler's lifetime
    HANDLE cancelEvent_ = nullptr;
    CLSID createdClsid_{};
    bool createdIsPdfHandler_ = false;
    bool initialized_ = false;
};

StreamProxy::~StreamProxy() {
    if (pdfStream_) pdfStream_->Release();
    if (cancelEvent_) CloseHandle(cancelEvent_);
    if (site_) site_->Release();
    if (innerSite_) innerSite_->Release();
    if (innerPreview_) innerPreview_->Release();
    if (innerInitFile_) innerInitFile_->Release();
    if (innerInitStream_) innerInitStream_->Release();
    if (innerUnknown_) innerUnknown_->Release();
}

bool StreamProxy::AdoptPdfHandler(const std::wstring& why) {
    if (!g_havePdfHandlerClsid) {
        lopw::LogWarn(L"cannot adopt the PDF preview handler: none discovered (" + why + L")");
        return false;
    }
    if (IsEqualCLSID(createdClsid_, g_pdfHandlerClsid)) return true;
    IUnknown* obj = nullptr;
    HRESULT hr = CoCreateInstance_Original(g_pdfHandlerClsid, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_IUnknown, reinterpret_cast<void**>(&obj));
    if (FAILED(hr) || !obj) {
        lopw::LogWarn(L"CoCreateInstance for the PDF preview handler failed (" + lopw::HrText(hr) +
                      L", " + why + L")");
        return false;
    }
    bool ok = false;
    IInitializeWithStream* init = nullptr;
    if (SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&init))) && init) {
        // Swap: keep the new object, drop the old one.
        if (innerInitStream_) innerInitStream_->Release();
        if (innerInitFile_) innerInitFile_->Release();
        if (innerPreview_) innerPreview_->Release();
        if (innerSite_) innerSite_->Release();
        if (innerUnknown_) innerUnknown_->Release();
        innerInitStream_ = init;
        innerInitFile_ = nullptr;
        obj->QueryInterface(IID_PPV_ARGS(&innerPreview_));
        obj->QueryInterface(IID_PPV_ARGS(&innerSite_));
        innerUnknown_ = obj;  // reference transferred from the AddRef above
        createdIsPdfHandler_ = true;
        ok = true;
    }
    if (!ok) obj->Release();
    if (ok) {
        lopw::LogI(L"routed an ODF document to the PDF preview handler because " + why);
    }
    return ok;
}

bool StreamProxy::BuildIdentity(IStream* stream, const std::wstring& nameHint, DocIdentity& id,
                                std::vector<std::wstring>& notes) {
    std::wstring name = nameHint;
    bool pathFromSite = false;
    if (name.empty()) {
        std::wstring sitePath;
        if (PathFromSite(site_, sitePath)) {
            name = sitePath;
            pathFromSite = true;
        }
    }
    if (name.empty()) {
        const std::wstring streamName = StreamName(stream);
        if (!streamName.empty()) name = streamName;
    }
    id.displayName = name;

    unsigned char head[8192]{};
    const size_t headLen = ReadStreamHead(stream, head, sizeof(head));
    id.sniffed = headLen > 0;

    // A file name that is a real path lets us avoid touching the stream again.
    std::wstring candidatePath;
    if (pathFromSite) {
        candidatePath = name;
    } else if (name.size() > 3 && name[1] == L':' && (name[2] == L'\\' || name[2] == L'/')) {
        candidatePath = name;
    }
    // UNC paths are never used: the requirements forbid network access, and the
    // broker rejects them as well.

    std::string nameExt;
    if (!name.empty()) nameExt = lop::ExtensionOf(lop::WideToUtf8(lop::FileNameOf(name)));

    const lop::SniffResult sniff =
        lop::SniffDocument(head, headLen, lop::ToLowerAscii(nameExt));
    id.mime = sniff.mime;

    const bool extEnabled = !nameExt.empty() && ExtensionEnabled(nameExt);
    if (sniff.kind == lop::SniffKind::Pdf) {
        // Already a PDF: the wrapped handler gets the stream untouched.
        return false;
    }
    if (sniff.Recognised()) {
        if (!ExtensionEnabled(sniff.ext) && !extEnabled) {
            notes.push_back(L"detected an ODF document of type '" + lop::Utf8ToWide(sniff.ext) +
                            L"' which is not in the configured extensions list");
            return false;
        }
        id.isOdf = true;
        id.extForConversion = sniff.ext;
    } else if (extEnabled) {
        // The name says LibreOffice document but the content is not recognisable
        // (corrupt file, unusual ZIP layout, ...).  Try the conversion anyway so
        // the user gets a meaningful diagnostic instead of "no preview".
        id.isOdf = true;
        id.extForConversion = nameExt;
        notes.push_back(L"the document content could not be identified; using the extension " +
                        lop::Utf8ToWide(nameExt));
    } else {
        return false;
    }
    id.extFromName = nameExt;

    if (!candidatePath.empty()) {
        uint64_t size = 0;
        uint64_t mtime = 0;
        if (lopw::FileInfo(candidatePath, size, mtime)) {
            id.fullPath = candidatePath;
            id.size = size;
            id.mtime = mtime;
            id.keyHex = lop::CacheKeyHex(lop::ToLowerAscii(lop::WideToUtf8(candidatePath)), size, mtime);
        } else {
            notes.push_back(L"the document path is not readable (" + candidatePath + L")");
        }
    }
    if (id.fullPath.empty()) {
        id.needsStreamCopy = true;
        if (notes.empty()) {
            notes.push_back(L"the preview host did not provide a file path; the stream is spooled "
                            L"to the scratch folder instead");
        }
    }
    return true;
}

HRESULT StreamProxy::HandOff(const std::string& data, const std::wstring& name) {
    if (!innerInitStream_) {
        lopw::LogWarn(L"the wrapped preview handler has no IInitializeWithStream");
        return E_NOINTERFACE;
    }
    auto* stream = new (std::nothrow) MemoryStream(data, name);
    if (!stream) return E_OUTOFMEMORY;
    const HRESULT hr = innerInitStream_->Initialize(stream, STGM_READ);
    if (SUCCEEDED(hr)) {
        if (pdfStream_) pdfStream_->Release();
        pdfStream_ = stream;  // the handler may keep reading after Initialize
    } else {
        stream->Release();
        lopw::LogWarn(L"IInitializeWithStream::Initialize failed on the generated PDF (" +
                      lopw::HrText(hr) + L")");
    }
    return hr;
}

HRESULT StreamProxy::PreviewDocument(IStream* stream, DocIdentity& id, DWORD grfMode) {
    std::vector<std::wstring> reasons;
    // A new preview starts fresh: a cancel that arrived before this call (for
    // example an Unload of a previously previewed item) must not abort it.
    if (cancelEvent_) ResetEvent(cancelEvent_);

    // Fast path: an up-to-date cached PDF from a previous preview.
    if (!id.keyHex.empty()) {
        const std::wstring cached = lopw::CachePdfPath(id.keyHex);
        uint64_t size = 0;
        uint64_t mtime = 0;
        std::wstring err;
        if (lopw::FileInfo(cached, size, mtime) && lopw::ValidatePdfFileQuick(cached, size, err)) {
            IStream* fileStream = nullptr;
            const HRESULT hr = SHCreateStreamOnFileEx(cached.c_str(),
                                                     STGM_READ | STGM_SHARE_DENY_NONE,
                                                     FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                                     &fileStream);
            if (SUCCEEDED(hr) && fileStream) {
                lopw::LogI(L"cache hit: " + cached);
                const HRESULT initHr = innerInitStream_->Initialize(fileStream, grfMode);
                if (SUCCEEDED(initHr)) {
                    if (pdfStream_) pdfStream_->Release();
                    pdfStream_ = fileStream;
                    return initHr;
                }
                fileStream->Release();
                lopw::LogWarn(L"cached PDF was rejected by the handler (" + lopw::HrText(initHr) +
                              L"), regenerating");
            } else {
                lopw::LogWarn(L"cannot open the cached PDF (" + lopw::HrText(hr) + L")");
            }
        }
    }

    // Spool the stream when the host gave us no path (also produces the key).
    std::wstring spooled;
    if (id.needsStreamCopy) {
        std::wstring error;
        uint64_t hash = 0;
        if (!lopw::EnsureDirectory(lopw::LowScratchDir())) {
            reasons.push_back(L"the scratch folder " + lopw::LowScratchDir() + L" cannot be created");
        } else {
            // The name is derived from the hash, so hash first into a temp file
            // and rename afterwards.
            // Scratch paths are composed from a bare UTF-8 name by both sides
            // so that a name can never carry path separators.
            const std::string spoolName =
                "spool-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(InterlockedIncrement(&g_seq)) + id.extForConversion;
            const std::wstring tmp = lopw::ScratchPath(spoolName);
            if (SpoolStream(stream, tmp, hash, error) == SpoolResult::Ok) {
                const std::string contentKey = lop::ContentKeyHex(hash);
                const std::wstring finalName = lop::Utf8ToWide(contentKey + id.extForConversion);
                const std::wstring finalPath = lopw::ScratchPath(lop::WideToUtf8(finalName));
                lopw::DeleteQuiet(finalPath);
                if (MoveFileW(tmp.c_str(), finalPath.c_str())) {
                    spooled = finalPath;
                    id.keyHex = contentKey;
                    // A previous run may have produced this exact content.
                    const std::wstring cached = lopw::CachePdfPath(id.keyHex);
                    uint64_t csize = 0, cmtime = 0;
                    std::wstring cerr;
                    if (lopw::FileInfo(cached, csize, cmtime) &&
                        lopw::ValidatePdfFileQuick(cached, csize, cerr)) {
                        IStream* fileStream = nullptr;
                        const HRESULT hr = SHCreateStreamOnFileEx(
                            cached.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL,
                            FALSE, nullptr, &fileStream);
                        if (SUCCEEDED(hr) && fileStream) {
                            const HRESULT initHr = innerInitStream_->Initialize(fileStream, grfMode);
                            if (SUCCEEDED(initHr)) {
                                if (pdfStream_) pdfStream_->Release();
                                pdfStream_ = fileStream;
                                lopw::LogI(L"cache hit (content key): " + cached);
                                return initHr;
                            }
                            fileStream->Release();
                        }
                    }
                } else {
                    reasons.push_back(L"cannot finalise the document copy (" +
                                      lopw::Num(GetLastError()) + L")");
                }
            } else {
                reasons.push_back(error);
            }
        }
    }

    // Broker hand-off.  The direct fallback below is only worth attempting when
    // the broker was *unavailable*: if it answered (even with a failure) another
    // LibreOffice run would fail the same way, and after a timeout the broker is
    // most likely still working and will publish the PDF for the next selection.
    bool tryDirectFallback = true;
    if (id.isOdf && !id.keyHex.empty() && CfgBool("use_broker", true)) {
        const uint64_t maxAge = static_cast<uint64_t>(CfgInt("broker_max_age_ms", 20000));
        const BrokerStatus broker = ReadBrokerStatus(maxAge);
        if (broker.alive) {
            lopw::LogI(L"asking the Explorer broker (pid " + lopw::Num(broker.pid) + L") to convert " +
                       (id.fullPath.empty() ? L"a spooled document" : id.fullPath));
            lop::Request req;
            req.id = static_cast<uint32_t>(InterlockedIncrement(&g_seq)) ^
                     (GetCurrentProcessId() << 12);
            req.seq = static_cast<uint32_t>(InterlockedIncrement(&g_seq));
            req.clientPid = GetCurrentProcessId();
            req.keyHex = id.keyHex;
            req.extLower = id.extForConversion;
            req.size = id.size;
            req.mtimeUnixMs = id.mtime;
            req.convertOriginal = CfgBool("convert_original", false);
            if (id.fullPath.empty() && !spooled.empty()) {
                req.mode = "scratch";
                req.nameUtf8 = lop::WideToUtf8(lop::FileNameOf(spooled));
            } else {
                req.mode = "path";
                req.nameUtf8 = lop::WideToUtf8(id.fullPath);
            }
            std::wstring error;
            if (!WriteRequestFile(req, error)) {
                reasons.push_back(error);
            } else {
                const std::wstring cached = lopw::CachePdfPath(id.keyHex);
                const DWORD waitMs = static_cast<DWORD>(CfgInt("client_wait_ms", 60000));
                const WaitResult waited = WaitForBrokerResult(id.keyHex, cancelEvent_, waitMs, cached);
                switch (waited.outcome) {
                    case WaitOutcome::PdfReady: {
                        IStream* fileStream = nullptr;
                        const HRESULT hr = SHCreateStreamOnFileEx(
                            cached.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL,
                            FALSE, nullptr, &fileStream);
                        if (SUCCEEDED(hr) && fileStream) {
                            const HRESULT initHr = innerInitStream_->Initialize(fileStream, grfMode);
                            if (SUCCEEDED(initHr)) {
                                if (pdfStream_) pdfStream_->Release();
                                pdfStream_ = fileStream;
                                lopw::LogI(L"broker produced the preview PDF: " + cached);
                                return initHr;
                            }
                            fileStream->Release();
                            reasons.push_back(L"the PDF preview handler rejected the converted PDF (" +
                                              lopw::HrText(initHr) + L")");
                        } else {
                            reasons.push_back(L"cannot open the converted PDF (" + lopw::HrText(hr) +
                                              L")");
                        }
                        break;
                    }
                    case WaitOutcome::Reported:
                        reasons.push_back(L"the Explorer broker reported: " + waited.message);
                        tryDirectFallback = false;
                        break;
                    case WaitOutcome::BrokerGone:
                        reasons.push_back(L"the Explorer broker stopped responding; is the "
                                          L"lo-preview-broker mod enabled for explorer.exe?");
                        break;
                    case WaitOutcome::Cancelled:
                        lopw::LogI(L"preview cancelled by the shell while converting");
                        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    case WaitOutcome::TimedOut:
                        reasons.push_back(L"the Explorer broker did not answer within " +
                                          lopw::Num(waitMs / 1000) + L" s; the conversion may "
                                          L"still complete in the background and be cached");
                        tryDirectFallback = false;
                        break;
                }
            }
        } else {
            reasons.push_back(L"the Explorer broker is not running (enable the lo-preview-broker mod "
                              L"for explorer.exe)");
        }
    }

    // Direct fallback inside prevhost (low integrity).
    if (id.isOdf && tryDirectFallback && CfgBool("direct_fallback", true)) {
        std::wstring why;
        std::wstring pdfPath;
        if (DirectConvert(id, spooled, cancelEvent_, pdfPath, why)) {
            IStream* fileStream = nullptr;
            const HRESULT hr = SHCreateStreamOnFileEx(pdfPath.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                                     FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                                     &fileStream);
            if (SUCCEEDED(hr) && fileStream) {
                const HRESULT initHr = innerInitStream_->Initialize(fileStream, grfMode);
                if (SUCCEEDED(initHr)) {
                    if (pdfStream_) pdfStream_->Release();
                    pdfStream_ = fileStream;
                    lopw::LogI(L"direct conversion produced the preview PDF: " + pdfPath);
                    return initHr;
                }
                fileStream->Release();
                reasons.push_back(L"the PDF preview handler rejected the directly converted PDF (" +
                                  lopw::HrText(initHr) + L")");
            } else {
                reasons.push_back(L"cannot open the directly converted PDF (" + lopw::HrText(hr) + L")");
            }
        } else if (!why.empty()) {
            reasons.push_back(L"direct conversion failed: " + why);
        } else {
            reasons.push_back(L"direct conversion is disabled in config.ini");
        }
    }

    // Nothing worked: still hand the shell a valid PDF that explains why.
    lopw::LogWarn(L"no preview PDF could be produced for " + id.displayName);
    for (const std::wstring& r : reasons) lopw::LogWarn(L"  reason: " + r);
    const std::string diagnostic = BuildDiagnosticPdf(id, reasons);
    std::wstring name = id.displayName.empty() ? std::wstring(L"LOPreview.pdf")
                                               : lop::FileNameOf(id.displayName);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name = name.substr(0, dot);
    name += L".pdf";
    return HandOff(diagnostic, name);
}

HRESULT StreamProxy::Initialize(IStream* stream, DWORD grfMode) {
    return InitializeInternal(stream, nullptr, grfMode);
}

HRESULT StreamProxy::InitializeInternal(IStream* stream, LPCWSTR pathHint, DWORD grfMode) {
    if (!stream) return E_INVALIDARG;
    lopw::LogI(std::wstring(pathHint ? L"IInitializeWithFile::Initialize('" +
                                         std::wstring(pathHint) + L"')"
                                     : L"IInitializeWithStream::Initialize") +
               L" stream " + lopw::HexPtr(stream) + L", mode " + lopw::Num(grfMode));
    if (!CfgBool("enabled", true)) {
        return innerInitStream_ ? innerInitStream_->Initialize(stream, grfMode) : E_FAIL;
    }
    DocIdentity id;
    std::vector<std::wstring> notes;
    if (!BuildIdentity(stream, pathHint ? std::wstring(pathHint) : std::wstring(), id, notes)) {
        for (const std::wstring& n : notes) lopw::LogD(L"passthrough: " + n);
        if (notes.empty()) lopw::LogD(L"passthrough: not a LibreOffice document");
        return innerInitStream_ ? innerInitStream_->Initialize(stream, grfMode) : E_FAIL;
    }
    for (const std::wstring& n : notes) lopw::LogI(L"identification: " + n);
    lopw::LogI(L"LibreOffice document detected: name='" + id.displayName + L"' type='" +
               lop::Utf8ToWide(id.extForConversion) + L"' key=" + lop::Utf8ToWide(id.keyHex));

    if (!createdIsPdfHandler_ && CfgBool("redirect_any_handler", true)) {
        AdoptPdfHandler(L"Explorer routed it to a different preview handler (" +
                        lopw::HexPtr(stream) + L")");
    }
    if (!innerInitStream_) {
        lopw::LogErr(L"the wrapped handler does not support IInitializeWithStream; the document "
                     L"cannot be previewed through it");
        return E_NOINTERFACE;
    }
    initialized_ = true;
    const HRESULT hr = PreviewDocument(stream, id, grfMode);
    if (FAILED(hr)) {
        lopw::LogWarn(L"preview failed (" + lopw::HrText(hr) + L")");
    }
    return hr;
}

HRESULT StreamProxy::Initialize(LPCWSTR filePath, DWORD grfMode) {
    if (!filePath) return E_INVALIDARG;
    if (!CfgBool("enabled", true)) {
        return innerInitFile_ ? innerInitFile_->Initialize(filePath, grfMode) : E_FAIL;
    }
    IStream* stream = nullptr;
    const HRESULT hr = SHCreateStreamOnFileEx(filePath, STGM_READ | STGM_SHARE_DENY_NONE,
                                              FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (FAILED(hr) || !stream) {
        lopw::LogWarn(L"IInitializeWithFile: cannot open '" + std::wstring(filePath) + L"' (" +
                      lopw::HrText(hr) + L")");
        return innerInitFile_ ? innerInitFile_->Initialize(filePath, grfMode) : hr;
    }
    // Prefer the stream based path so that both initialisers behave identically.
    if (!innerInitStream_) {
        stream->Release();
        return innerInitFile_ ? innerInitFile_->Initialize(filePath, grfMode) : E_NOINTERFACE;
    }
    const HRESULT init = InitializeInternal(stream, filePath, grfMode);
    stream->Release();
    return init;
}

// ---------------------------------------------------------------------------
// COM interception
// ---------------------------------------------------------------------------

// True when the object really is a preview handler: this keeps the hook away
// from unrelated COM objects even with a wide inclusion list.
bool LooksLikePreviewHandler(IUnknown* obj) {
    if (!obj) return false;
    IPreviewHandler* preview = nullptr;
    const bool hasPreview = SUCCEEDED(obj->QueryInterface(IID_PPV_ARGS(&preview))) && preview;
    if (preview) preview->Release();
    return hasPreview;
}

HRESULT WINAPI CoCreateInstance_Hook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext,
                                     REFIID riid, LPVOID* ppv) {
    const HRESULT hr = CoCreateInstance_Original(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (FAILED(hr) || !ppv || !*ppv || pUnkOuter) return hr;
    if (!CfgBool("enabled", true)) return hr;
    const bool isPdfHandler = g_havePdfHandlerClsid && IsEqualCLSID(rclsid, g_pdfHandlerClsid);
    if (!isPdfHandler && !CfgBool("redirect_any_handler", true)) return hr;
    auto* inner = static_cast<IUnknown*>(*ppv);
    if (!LooksLikePreviewHandler(inner)) return hr;
    const void* innerForLog = inner;
    auto* proxy = new (std::nothrow) StreamProxy(inner, rclsid, isPdfHandler);
    if (!proxy) return hr;
    void* result = nullptr;
    if (riid == IID_IUnknown) result = static_cast<IInitializeWithStream*>(proxy);
    else if (riid == IID_IInitializeWithStream) result = static_cast<IInitializeWithStream*>(proxy);
    else if (riid == IID_IInitializeWithFile) result = static_cast<IInitializeWithFile*>(proxy);
    else if (riid == IID_IPreviewHandler) result = static_cast<IPreviewHandler*>(proxy);
    else if (riid == IID_IObjectWithSite) result = static_cast<IObjectWithSite*>(proxy);
    else {
        proxy->Release();
        return hr;
    }
    inner->Release();  // the proxy owns the reference the caller would have got
    *ppv = result;
    lopw::LogD(L"wrapped preview handler " + lopw::HexPtr(innerForLog) + L" (clsid known=" +
               (isPdfHandler ? L"pdf" : L"other") + L")");
    return hr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Windhawk entry points
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    const std::wstring module = lopw::ModuleNameOfCurrentProcess();
    if (_wcsicmp(module.c_str(), L"prevhost.exe") != 0) {
        return TRUE;  // nothing to do in other processes
    }
    lopw::LogInit(L"prevhost", kModVersion);
    lopw::LogSetLevel(CfgInt("log_level", 2));

    // The shell's own log is preferred, but a low integrity prevhost cannot
    // write there - in that case log next to the low scratch folder.
    lopw::EnsureDirectory(lopw::LowScratchDir());
    if (!lopw::LogSetFile(lopw::JoinPath(lopw::LogsDir(), L"lopreview.log"))) {
        // Expected: prevhost is low integrity and cannot write to %LOCALAPPDATA%.
        lopw::LogSetFile(lopw::JoinPath(lopw::LowRootDir(), L"prevhost.log"));
    }

    Wh_Log(L"LOPreview prevhost client %s build %s starting (pid %u, integrity %s)", kModVersion,
           kBuildId, GetCurrentProcessId(), lopw::IntegrityLevelText().c_str());
    lopw::LogI(L"LOPreview " + std::wstring(kModVersion) + L" build " + kBuildId +
               L" in prevhost.exe (pid " + lopw::Num(GetCurrentProcessId()) + L", integrity " +
               lopw::IntegrityLevelText() + L")");

    if (DiscoverPdfPreviewHandler()) {
        wchar_t text[64]{};
        StringFromGUID2(g_pdfHandlerClsid, text, ARRAYSIZE(text));
        lopw::LogI(L"PDF preview handler: " + std::wstring(text) + L" (found via " + g_pdfHandlerHow +
                   L")");
    } else {
        lopw::LogWarn(L"no PDF preview handler is registered for .pdf: previews will fall back to "
                      L"the diagnostic page");
    }

    const BrokerStatus broker = ReadBrokerStatus(20000);
    lopw::LogI(L"Explorer broker: " + std::wstring(broker.alive ? L"running" : L"not running") +
               (broker.pid ? L" (pid " + lopw::Num(broker.pid) + L")" : L"") +
               (broker.sofficePath.empty() ? L""
                                           : L", soffice " + broker.sofficePath +
                                                 (broker.sofficeFound ? L"" : L" (not found!)")));

    if (!WindhawkUtils::SetFunctionHook(CoCreateInstance, CoCreateInstance_Hook,
                                        &CoCreateInstance_Original)) {
        Wh_Log(L"failed to hook CoCreateInstance");
        lopw::LogErr(L"failed to hook CoCreateInstance");
        return FALSE;
    }
    lopw::LogI(L"CoCreateInstance hook installed");
    return TRUE;
}

void Wh_ModUninit() {
    lopw::LogI(L"prevhost client unloading");
}
