// ==WindhawkMod==
// @id           lo-preview-broker
// @name         LibreOffice Preview Broker
// @description  Converts ODF documents to PDF for the Explorer preview pane. Runs inside explorer.exe (medium integrity) so LibreOffice gets a normal user token; the prevhost mod talks to it through the low-integrity scratch folder.
// @version      0.5.1
// @author       LOPreview
// @include      explorer.exe
// @compilerOptions -std=c++20 -lole32 -luuid -lshlwapi -lshell32 -ladvapi32 -luser32
// ==/WindhawkMod==

// ---------------------------------------------------------------------------
// GENERATED FILE - do not edit here.
// Assembled by tools/assemble.py from src/broker-mod.wh.cpp, src/lop_core.h and
// src/lop_win.h.  Edit those files and re-run the assembler instead.
// ---------------------------------------------------------------------------

// LOPreview 0.5.1 - fixes the ASSOCSTR_SHELLIDLIST compile error of 0.5.0.
// Stale-copy check: the string ASSOCSTR_SHELLIDLIST must NOT appear in this file,
// and the header above must say 0.5.1.  See docs/TESTING.md section 0.
//
// Why this mod exists
// -------------------
// The Windows shell hosts preview handlers in prevhost.exe, which runs at LOW
// integrity.  A process started from there inherits that token: it cannot write
// to the user profile, so LibreOffice cannot create its profile, cannot write
// temp files and cannot produce a PDF.  (Launching soffice from prevhost is
// technically possible, but the conversion fails - and if it did not fail, the
// preview would depend on a low integrity process writing to the user's disk.)
//
// explorer.exe runs at the user's normal integrity level, is always present,
// and is already where the shell does privileged work.  This mod is therefore a
// tiny, bounded job runner: it watches the low-integrity scratch folder for
// request files, converts them with LibreOffice into the PDF cache, and writes
// back a status file.  See docs/ARCHITECTURE.md.

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
constexpr wchar_t kBuildId[] = L"b7091ce4";
constexpr int kQueueDepth = 16;
constexpr DWORD kScanIntervalMs = 1000;       // safety net poll
constexpr DWORD kHeartbeatIntervalMs = 5000;  // broker.json refresh
constexpr DWORD kMaintenanceIntervalMs = 60000;
constexpr DWORD kRequestMaxAgeMs = 300000;  // stale requests are dropped
constexpr DWORD kScratchMaxAgeMs = 3600000; // low scratch: 1 hour
constexpr DWORD kCacheMinKeepMs = 600000;   // never evict a fresh PDF

// ---------------------------------------------------------------------------
// Job queue and worker pool
// ---------------------------------------------------------------------------

struct Job {
    lop::Request req;
    std::wstring reqPath;
    ULONGLONG enqueuedTick = 0;
    HANDLE cancelEvent = nullptr;  // per job, signalled when superseded
    int workerSlot = 0;            // set by the worker that picks the job up
};

struct Broker {
    CRITICAL_SECTION cs{};
    bool csInit = false;
    HANDLE workEvent = nullptr;   // manual reset, signalled when queue non-empty
    HANDLE stopEvent = nullptr;   // signalled by Wh_ModUninit
    HANDLE thread = nullptr;
    HANDLE workers[4]{};
    int workerCount = 0;
    Job queue[kQueueDepth];
    int queueCount = 0;
    std::vector<std::string> inFlightKeys;
    std::vector<std::pair<DWORD, DWORD>> latestSeq;  // client pid -> highest seq seen
    std::wstring soffice;
    std::wstring sofficeHow;
    lop::Ini cfg;
    volatile LONG running = 0;
    volatile LONG exitNow = 0;
    // statistics for the heartbeat file
    volatile LONG completed = 0;
    volatile LONG failed = 0;
    volatile LONG runningJobs = 0;
    ULONGLONG startTick = 0;
};

Broker g_broker;

int CfgInt(const char* key, int def) { return g_broker.cfg.GetInt(key, def); }
bool CfgBool(const char* key, bool def) { return g_broker.cfg.GetBool(key, def); }

// ---------------------------------------------------------------------------
// Heartbeat: lets the prevhost side know whether a broker exists without
// blocking on a dead hand-off.
// ---------------------------------------------------------------------------

void WriteHeartbeat() {
    const std::string text =
        std::string("LOPBROKER 1\n") +
        "version=" + lop::WideToUtf8(kModVersion) + "\n" +
        "pid=" + std::to_string(GetCurrentProcessId()) + "\n" +
        "tick=" + std::to_string(lopw::NowUnixMs()) + "\n" +
        "started_tick=" + std::to_string(g_broker.startTick) + "\n" +
        "in_flight=" + std::to_string(g_broker.runningJobs) + "\n" +
        "completed=" + std::to_string(g_broker.completed) + "\n" +
        "failed=" + std::to_string(g_broker.failed) + "\n" +
        "soffice=" + lop::WideToUtf8(g_broker.soffice) + "\n" +
        "soffice_found=" + std::string(g_broker.soffice.empty() ? "0" : "1") + "\n";
    if (!lopw::WriteTextAtomic(lopw::HeartbeatPath(), text)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            lopw::LogWarn(L"cannot write heartbeat " + lopw::HeartbeatPath() + L" (error " +
                          lopw::Num(GetLastError()) + L")");
        }
    }
}

// ---------------------------------------------------------------------------
// Path validation for mode=path requests
// ---------------------------------------------------------------------------

struct ValidatedInput {
    bool ok = false;
    std::wstring path;
    std::wstring error;
    uint64_t size = 0;
    uint64_t mtime = 0;
};

ValidatedInput ValidateIncomingPath(const lop::Request& req) {
    ValidatedInput out;
    // Canonicalise: this collapses "..", short names and alternate separators.
    wchar_t full[32768]{};
    if (!GetFullPathNameW(lop::Utf8ToWide(req.nameUtf8).c_str(), ARRAYSIZE(full), full, nullptr)) {
        out.error = L"GetFullPathName failed (error " + lopw::Num(GetLastError()) + L")";
        return out;
    }
    out.path.assign(full);
    if (out.path.size() < 4 || out.path[0] == L'\\' || out.path[1] != L':') {
        out.error = L"refusing to open a non-local path";
        return out;
    }
    // No network access: reject UNC, and reject volumes that are not local.
    const UINT driveType = GetDriveTypeW(out.path.substr(0, 3).c_str());
    if (driveType == DRIVE_REMOTE || driveType == DRIVE_NO_ROOT_DIR) {
        out.error = L"refusing to open a path on a remote/offline volume";
        return out;
    }
    if (lopw::IsDirectory(out.path)) {
        out.error = L"path is a directory";
        return out;
    }
    if (!lopw::FileInfo(out.path, out.size, out.mtime)) {
        out.error = L"file not found or not readable (error " + lopw::Num(GetLastError()) + L")";
        return out;
    }
    if (out.size == 0) {
        out.error = L"file is empty";
        return out;
    }
    const unsigned long long maxBytes =
        static_cast<unsigned long long>(CfgInt("max_document_mb", 512)) * 1024ull * 1024ull;
    if (out.size > maxBytes) {
        out.error = L"document is larger than max_document_mb (" + lopw::Num(out.size / (1024 * 1024)) +
                    L" MB)";
        return out;
    }
    const std::string actualExt = lop::ExtensionOf(lop::WideToUtf8(out.path));
    if (!lop::IsOdfExtension(actualExt) && !lop::IsFlatOdfExtension(actualExt)) {
        out.error = L"unexpected extension '" + lop::Utf8ToWide(actualExt) + L"'";
        return out;
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Job execution
// ---------------------------------------------------------------------------

std::wstring ScratchRequestPath(const lop::Request& req) {
    return lopw::ScratchPath(lop::Hex64(req.id) + ".req");
}

void WriteErrorStatus(const std::string& keyHex, const std::wstring& message) {
    const std::wstring path = lopw::ScratchPath(keyHex + ".err");
    const std::string text = lop::WideToUtf8(message);
    if (!lopw::WriteTextAtomic(path, text)) {
        lopw::LogWarn(L"cannot write status file " + path + L" (error " + lopw::Num(GetLastError()) +
                      L")");
    }
}

// Copies the source document into the job's work directory.  LibreOffice is
// never pointed at the user's own folder: it creates .~lock.<name># files next
// to whatever it opens, and preview must not modify the user's directories.
bool CopyDocumentToWorkDir(const std::wstring& source, const std::wstring& target,
                           std::wstring& error) {
    HANDLE in = CreateFileW(source.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        error = L"cannot read the document (error " + lopw::Num(GetLastError()) + L")";
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            error = L"the document is locked by another program";
        }
        return false;
    }
    HANDLE out = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        const unsigned long err = GetLastError();
        CloseHandle(in);
        error = L"cannot create the work copy (error " + lopw::Num(err) + L")";
        if (err == ERROR_DISK_FULL) error = L"not enough free disk space";
        return false;
    }
    std::vector<char> buffer(1 << 20);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            error = L"read failed (error " + lopw::Num(GetLastError()) + L")";
            ok = false;
            break;
        }
        if (got == 0) break;
        DWORD written = 0;
        if (!WriteFile(out, buffer.data(), got, &written, nullptr) || written != got) {
            const unsigned long err = GetLastError();
            error = L"write failed (error " + lopw::Num(err) + L")";
            if (err == ERROR_DISK_FULL) error = L"not enough free disk space";
            ok = false;
            break;
        }
    }
    CloseHandle(in);
    CloseHandle(out);
    if (!ok) lopw::DeleteQuiet(target);
    return ok;
}

// Profile pool: LibreOffice cannot share one user profile between concurrent
// processes, and re-creating a profile for every preview costs seconds.
std::wstring AcquireProfile(int slot) {
    if (slot < 0) slot = 0;
    if (slot > 3) slot = 3;
    const std::wstring dir = lopw::JoinPath(
        lopw::JoinPath(lopw::BrokerTmpDir(), L"lo-profile"), L"p" + lopw::Num(slot));
    lopw::EnsureDirectory(dir);
    lopw::SeedLibreOfficeProfile(dir);
    return dir;
}

void RunJob(Job& job) {
    const std::string keyHex = job.req.keyHex.size() == 16 ? job.req.keyHex
                                                           : lop::CacheKeyHex(
                                                                 lop::ToLowerAscii(job.req.nameUtf8),
                                                                 job.req.size, job.req.mtimeUnixMs);
    const std::wstring cachePath = lopw::CachePdfPath(keyHex);
    const std::wstring statusPath = lopw::ScratchPath(keyHex + ".err");
    const std::wstring workDir = lopw::JoinPath(lopw::BrokerTmpDir(), lop::Utf8ToWide(keyHex));
    const std::wstring scratchCopy = lopw::ScratchPath(job.req.nameUtf8);

    auto finish = [&](bool ok, const std::wstring& message) {
        if (ok) {
            InterlockedIncrement(&g_broker.completed);
            lopw::DeleteQuiet(statusPath);  // a previous failure is now stale
        } else {
            InterlockedIncrement(&g_broker.failed);
            WriteErrorStatus(keyHex, message);
        }
        lopw::DeleteQuiet(job.reqPath);
    };

    // A cached PDF that is already valid wins over any conversion.
    {
        uint64_t size = 0, mtime = 0;
        std::wstring err;
        if (lopw::FileInfo(cachePath, size, mtime) && lopw::ValidatePdfFileQuick(cachePath, size, err)) {
            lopw::LogD(L"cache already present for " + lop::Utf8ToWide(keyHex) + L", skipping");
            finish(true, L"");
            return;
        }
    }

    if (GetTickCount64() - job.enqueuedTick > kRequestMaxAgeMs) {
        lopw::LogWarn(L"dropping stale request " + job.reqPath);
        lopw::DeleteQuiet(statusPath);
        lopw::DeleteQuiet(job.reqPath);
        return;
    }

    lopw::EnsureDirectory(workDir);
    lopw::EnsureDirectory(lopw::JoinPath(workDir, L"out"));

    // 1. Where does the ODF document come from?
    std::wstring source;
    std::wstring error;
    if (job.req.mode == "scratch") {
        if (!lop::IsSafeScratchName(job.req.nameUtf8)) {
            finish(false, L"rejected unsafe scratch name");
            return;
        }
        source = scratchCopy;
        if (!lopw::PathExists(source)) {
            finish(false, L"the client's document copy is missing (" +
                              lop::Utf8ToWide(job.req.nameUtf8) + L")");
            return;
        }
    } else {
        const ValidatedInput in = ValidateIncomingPath(job.req);
        if (!in.ok) {
            finish(false, in.error);
            return;
        }
        source = in.path;
        if (in.mtime != job.req.mtimeUnixMs && job.req.mtimeUnixMs) {
            lopw::LogD(L"document changed since the client read it (client mtime " +
                       lopw::Num(job.req.mtimeUnixMs) + L", disk mtime " + lopw::Num(in.mtime) +
                       L") - converting the current content");
        }
    }

    // 2. Work on a private copy, with the extension LibreOffice must see (the
    //    sniffed one, not necessarily the one from the file name).
    std::wstring inputPath;
    if (CfgBool("convert_original", false) && job.req.convertOriginal && job.req.mode == "path") {
        inputPath = source;
    } else {
        const std::wstring ext = lop::Utf8ToWide(job.req.extLower.empty() ? ".odt" : job.req.extLower);
        inputPath = lopw::JoinPath(workDir, L"input" + ext);
        if (!CopyDocumentToWorkDir(source, inputPath, error)) {
            finish(false, error);
            return;
        }
    }

    // 3. Convert.
    lopw::ConvertRequest creq;
    creq.inputPath = inputPath;
    creq.workDir = workDir;
    creq.outputDir = lopw::JoinPath(workDir, L"out");
    creq.profileDir = AcquireProfile(job.workerSlot);
    creq.tempDir = workDir;
    creq.sofficePath = g_broker.soffice;
    creq.filterOptions = lop::Utf8ToWide(g_broker.cfg.Get("pdf_filter", "pdf"));
    creq.timeoutMs = static_cast<DWORD>(CfgInt("timeout_seconds", 120)) * 1000u;
    creq.cancelEvent = job.cancelEvent;
    creq.lowerPriority = CfgBool("lower_priority", true);

    lopw::ConvertResult result = lopw::ConvertOdfToPdf(creq);

    if (!result.ok) {
        lopw::LogWarn(L"conversion failed for " + source + L": " + result.error);
        finish(false, result.error);
        return;
    }

    // 4. Publish atomically into the cache.
    if (!lopw::EnsureDirectory(lopw::CacheDir())) {
        finish(false, L"cannot create the cache directory");
        return;
    }
    lopw::DeleteQuiet(cachePath);
    if (!MoveFileExW(result.pdfPath.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Cross-volume or locked: fall back to a copy.
        std::wstring copyError;
        if (!CopyDocumentToWorkDir(result.pdfPath, cachePath, copyError)) {
            finish(false, L"cannot publish the generated PDF: " + copyError);
            return;
        }
    }
    uint64_t pdfSize = 0, pdfMtime = 0;
    std::wstring verifyErr;
    if (!lopw::FileInfo(cachePath, pdfSize, pdfMtime) ||
        !lopw::ValidatePdfFileQuick(cachePath, pdfSize, verifyErr)) {
        lopw::LogErr(L"published PDF failed validation: " + verifyErr);
        lopw::DeleteQuiet(cachePath);
        finish(false, L"the generated PDF did not validate: " + verifyErr);
        return;
    }
    lopw::LogI(L"converted " + source + L" -> " + cachePath + L" (" + lopw::Num(pdfSize) + L" bytes)");
    finish(true, L"ok");
}

// ---------------------------------------------------------------------------
// Queue handling
// ---------------------------------------------------------------------------

DWORD WINAPI WorkerThread(LPVOID param) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int slot = static_cast<int>(reinterpret_cast<INT_PTR>(param));
    lopw::LogD(L"worker " + lopw::Num(slot) + L" started");

    for (;;) {
        Job job{};
        bool have = false;
        EnterCriticalSection(&g_broker.cs);
        if (g_broker.queueCount > 0) {
            job = g_broker.queue[0];
            for (int i = 1; i < g_broker.queueCount; ++i) g_broker.queue[i - 1] = g_broker.queue[i];
            g_broker.queueCount--;
            have = true;
            if (g_broker.queueCount == 0) ResetEvent(g_broker.workEvent);
        }
        LeaveCriticalSection(&g_broker.cs);
        if (!have) {
            if (InterlockedCompareExchange(&g_broker.exitNow, 0, 0) ||
                WaitForSingleObject(g_broker.stopEvent, 250) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        // Superseded? (the user moved on to another file in the same preview
        // host - do not spend a LibreOffice process on it)
        bool superseded = false;
        EnterCriticalSection(&g_broker.cs);
        for (const auto& kv : g_broker.latestSeq) {
            if (kv.first == job.req.clientPid && job.req.seq < kv.second) superseded = true;
        }
        LeaveCriticalSection(&g_broker.cs);

        // Each worker owns one LibreOffice profile: two concurrent soffice.exe
        // instances must never share -env:UserInstallation.
        job.workerSlot = slot;
        InterlockedIncrement(&g_broker.runningJobs);
        if (superseded) {
            lopw::LogI(L"skipping superseded request " + job.reqPath);
            lopw::DeleteQuiet(job.reqPath);
        } else {
            RunJob(job);
        }
        InterlockedDecrement(&g_broker.runningJobs);

        EnterCriticalSection(&g_broker.cs);
        for (size_t i = 0; i < g_broker.inFlightKeys.size(); ++i) {
            if (g_broker.inFlightKeys[i] == job.req.keyHex) {
                g_broker.inFlightKeys.erase(g_broker.inFlightKeys.begin() + i);
                break;
            }
        }
        LeaveCriticalSection(&g_broker.cs);
        if (job.cancelEvent) CloseHandle(job.cancelEvent);
    }
    CoUninitialize();
    return 0;
}

bool EnqueueJob(const Job& job) {
    bool enqueued = false;
    EnterCriticalSection(&g_broker.cs);
    if (g_broker.queueCount < kQueueDepth) {
        g_broker.queue[g_broker.queueCount++] = job;
        enqueued = true;
    }
    LeaveCriticalSection(&g_broker.cs);
    if (enqueued) SetEvent(g_broker.workEvent);
    return enqueued;
}

// Reads one request file and hands it to the worker(s).
void ProcessRequestFile(const std::wstring& path) {
    std::string text;
    if (!lopw::ReadTextFile(path, text, 32 * 1024)) {
        lopw::LogWarn(L"cannot read request " + path);
        return;
    }
    lop::Request req;
    std::string parseError;
    if (!lop::ParseRequest(text, req, parseError)) {
        lopw::LogWarn(L"rejecting malformed request " + path + L": " +
                      lop::Utf8ToWide(parseError));
        // The client cannot be answered because we do not know its key; drop it.
        lopw::DeleteQuiet(path);
        return;
    }
    if (req.keyHex.size() != 16) {
        req.keyHex = lop::CacheKeyHex(lop::ToLowerAscii(req.nameUtf8), req.size, req.mtimeUnixMs);
    }

    const std::wstring cachePath = lopw::CachePdfPath(req.keyHex);
    const std::wstring statusPath = lopw::ScratchPath(req.keyHex + ".err");
    uint64_t size = 0, mtime = 0;
    std::wstring err;
    if (lopw::FileInfo(cachePath, size, mtime) && lopw::ValidatePdfFileQuick(cachePath, size, err)) {
        lopw::LogD(L"request satisfied from cache: " + path);
        lopw::DeleteQuiet(statusPath);
        lopw::DeleteQuiet(path);
        return;
    }

    // Remember the newest sequence per client so that older, still-running
    // conversions can be cancelled (rapid selection changes).
    EnterCriticalSection(&g_broker.cs);
    bool seenPid = false;
    for (auto& kv : g_broker.latestSeq) {
        if (kv.first == req.clientPid) {
            seenPid = true;
            if (req.seq > kv.second) kv.second = req.seq;
        }
    }
    if (!seenPid) g_broker.latestSeq.emplace_back(req.clientPid, req.seq);
    while (g_broker.latestSeq.size() > 32) g_broker.latestSeq.erase(g_broker.latestSeq.begin());

    bool duplicate = false;
    for (const std::string& k : g_broker.inFlightKeys) {
        if (k == req.keyHex) duplicate = true;
    }
    if (!duplicate) g_broker.inFlightKeys.push_back(req.keyHex);
    LeaveCriticalSection(&g_broker.cs);

    if (duplicate) {
        lopw::LogI(L"a conversion for this document is already running: " + path);
        return;  // the running job deletes the request file when it finishes
    }

    Job job{};
    job.req = req;
    job.reqPath = path;
    job.enqueuedTick = GetTickCount64();
    job.cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!EnqueueJob(job)) {
        EnterCriticalSection(&g_broker.cs);
        g_broker.inFlightKeys.clear();
        LeaveCriticalSection(&g_broker.cs);
        if (job.cancelEvent) CloseHandle(job.cancelEvent);
        lopw::LogWarn(L"queue is full, rejecting " + path);
        WriteErrorStatus(req.keyHex,
                         L"the preview queue is busy; try again in a moment");
        lopw::DeleteQuiet(path);
        return;
    }
    if (g_broker.queueCount == 1 && g_broker.runningJobs == 0) {
        lopw::LogD(L"queued " + path);
    }
}

void ScanScratchDir() {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = lopw::JoinPath(lopw::LowScratchDir(), L"*.req");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = lopw::JoinPath(lopw::LowScratchDir(), fd.cFileName);
        ProcessRequestFile(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RunMaintenance() {
    const wchar_t* scratchExts[] = {L".req", L".tmp", L".odt", L".pdf", L".err", L".odg", L".odp",
                                    L".ods", L".odf"};
    lopw::CleanupDirByAge(lopw::LowScratchDir(), kScratchMaxAgeMs, 256ull * 1024ull * 1024ull,
                          scratchExts, ARRAYSIZE(scratchExts));

    const wchar_t* cacheExts[] = {L".pdf"};
    const unsigned long long cacheMaxBytes =
        static_cast<unsigned long long>(CfgInt("cache_max_mb", 512)) * 1024ull * 1024ull;
    const unsigned long long cacheMaxAge =
        static_cast<unsigned long long>(CfgInt("cache_max_age_days", 30)) * 24ull * 3600ull * 1000ull;
    lopw::CleanupDirByAge(lopw::CacheDir(), cacheMaxAge, cacheMaxBytes, cacheExts,
                          ARRAYSIZE(cacheExts), kCacheMinKeepMs);

    // Work directories of crashed/abandoned conversions are removed after a day.
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = lopw::JoinPath(lopw::BrokerTmpDir(), L"*");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    const uint64_t now = lopw::NowUnixMs();
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring dir = lopw::JoinPath(lopw::BrokerTmpDir(), fd.cFileName);
        // Never touch a directory a live job could still be using.
        if (g_broker.runningJobs) continue;
        const uint64_t ticks = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                               fd.ftLastWriteTime.dwLowDateTime;
        const uint64_t mtimeMs =
            ticks < 116444736000000000ULL ? 0 : (ticks - 116444736000000000ULL) / 10000ULL;
        if (mtimeMs && now > mtimeMs && now - mtimeMs > 24ull * 3600ull * 1000ull) {
            lopw::LogD(L"removing stale work directory " + dir);
            lopw::DeleteDirectoryRecursive(dir);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

DWORD WINAPI BrokerThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_broker.startTick = GetTickCount64();
    lopw::LogInit(L"broker", kModVersion);
    g_broker.cfg = lopw::LoadConfig();
    lopw::LogSetLevel(CfgInt("log_level", 2));

    if (!CfgBool("enabled", true) || !CfgBool("use_broker", true)) {
        lopw::LogI(L"broker disabled by config.ini; not starting");
        CoUninitialize();
        return 0;
    }

    // Log file: medium integrity, so %LOCALAPPDATA% is fine.
    lopw::EnsureDirectory(lopw::LogsDir());
    lopw::LogSetFile(lopw::JoinPath(lopw::LogsDir(), L"lopreview.log"));

    lopw::EnsureDirectory(lopw::RootDir());
    lopw::EnsureDirectory(lopw::CacheDir());
    lopw::EnsureDirectory(lopw::BrokerTmpDir());
    lopw::EnsureDirectory(lopw::LowScratchDir());

    lopw::LogI(L"LOPreview broker " + std::wstring(kModVersion) + L" build " + kBuildId +
               L" starting in explorer.exe (pid " + lopw::Num(GetCurrentProcessId()) + L", integrity " +
               lopw::IntegrityLevelText() + L")");

    g_broker.soffice = lopw::FindLibreOffice(g_broker.cfg, g_broker.sofficeHow);
    if (g_broker.soffice.empty()) {
        lopw::LogErr(L"LibreOffice was not found. Checked config.ini soffice_path, the LibreOffice "
                     L"registry keys, %ProgramFiles%\\LibreOffice, PATH and the usual install "
                     L"locations.");
    } else {
        lopw::LogI(L"LibreOffice: " + g_broker.soffice + L" (found via " + g_broker.sofficeHow + L")");
    }

    const int maxConcurrent = CfgInt("max_concurrent", 2) < 1 ? 1 : (CfgInt("max_concurrent", 2) > 4 ? 4 : CfgInt("max_concurrent", 2));
    g_broker.workEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_broker.stopEvent = g_broker.stopEvent ? g_broker.stopEvent : CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int i = 0; i < maxConcurrent; ++i) {
        HANDLE t = CreateThread(nullptr, 0, WorkerThread,
                                reinterpret_cast<LPVOID>(static_cast<INT_PTR>(i)), 0, nullptr);
        if (t) {
            EnterCriticalSection(&g_broker.cs);
            if (g_broker.workerCount < 4) g_broker.workers[g_broker.workerCount++] = t;
            LeaveCriticalSection(&g_broker.cs);
        }
    }

    WriteHeartbeat();
    ScanScratchDir();  // pick up requests that arrived while we were not running

    HANDLE change = FindFirstChangeNotificationW(
        lopw::LowScratchDir().c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);

    ULONGLONG lastHeartbeat = GetTickCount64();
    ULONGLONG lastMaintenance = 0;
    while (!InterlockedCompareExchange(&g_broker.exitNow, 0, 0)) {
        HANDLE waits[3] = {g_broker.stopEvent, change, nullptr};
        const DWORD count = change ? 2u : 1u;
        const DWORD wait = WaitForMultipleObjects(count, waits, FALSE, kScanIntervalMs);
        if (wait == WAIT_OBJECT_0) break;  // stop requested
        if (wait == WAIT_OBJECT_0 + 1 && change) {
            ScanScratchDir();
            if (!FindNextChangeNotification(change)) {
                FindCloseChangeNotification(change);
                change = FindFirstChangeNotificationW(
                    lopw::LowScratchDir().c_str(), FALSE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                        FILE_NOTIFY_CHANGE_SIZE);
            }
        } else {
            ScanScratchDir();  // safety net
        }
        const ULONGLONG now = GetTickCount64();
        if (now - lastHeartbeat >= kHeartbeatIntervalMs) {
            lastHeartbeat = now;
            WriteHeartbeat();
        }
        if (now - lastMaintenance >= kMaintenanceIntervalMs) {
            lastMaintenance = now;
            RunMaintenance();
        }
    }
    if (change) FindCloseChangeNotification(change);

    InterlockedExchange(&g_broker.exitNow, 1);
    SetEvent(g_broker.stopEvent);
    // Give running conversions a moment to notice cancellation.
    for (int i = 0; i < 40; ++i) {
        if (g_broker.runningJobs == 0 && g_broker.queueCount == 0) break;
        Sleep(50);
    }
    lopw::LogI(L"broker stopped");
    CoUninitialize();
    return 0;
}

BOOL Wh_ModInit() {
    // Wide process inclusion lists ("*") are a common Windhawk setup; make sure
    // this mod only ever acts inside the real shell process.
    const std::wstring module = lopw::ModuleNameOfCurrentProcess();
    if (_wcsicmp(module.c_str(), L"explorer.exe") != 0) {
        return TRUE;
    }
    if (!lopw::IsShellProcess()) {
        Wh_Log(L"not the shell process (%s); broker stays idle", module.c_str());
        return TRUE;
    }
    lopw::LogInit(L"broker", kModVersion);
    lopw::LogSetLevel(lopw::kLogInfo);
    // config.ini is read on the broker thread only; touching it here would race
    // with the thread we are about to start.
    Wh_Log(L"LOPreview broker %s build %s loading in explorer.exe", kModVersion, kBuildId);

    InitializeCriticalSection(&g_broker.cs);
    g_broker.csInit = true;
    g_broker.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_broker.thread = CreateThread(nullptr, 0, BrokerThread, nullptr, 0, nullptr);
    if (!g_broker.thread) {
        Wh_Log(L"failed to start the broker thread: %lu", GetLastError());
        return FALSE;
    }
    InterlockedExchange(&g_broker.running, 1);
    return TRUE;
}

void Wh_ModUninit() {
    if (!InterlockedCompareExchange(&g_broker.running, 0, 1)) return;
    InterlockedExchange(&g_broker.exitNow, 1);
    if (g_broker.stopEvent) SetEvent(g_broker.stopEvent);
    if (g_broker.thread) {
        WaitForSingleObject(g_broker.thread, 5000);
        CloseHandle(g_broker.thread);
        g_broker.thread = nullptr;
    }
    // Every worker must be gone before Windhawk unloads this module: a thread
    // still executing mod code after unmapping would kill explorer.exe.
    for (int i = 0; i < g_broker.workerCount; ++i) {
        if (g_broker.workers[i]) {
            const DWORD wait = WaitForSingleObject(g_broker.workers[i], 8000);
            if (wait != WAIT_OBJECT_0) {
                Wh_Log(L"worker %d did not stop in time", i);
            }
            CloseHandle(g_broker.workers[i]);
            g_broker.workers[i] = nullptr;
        }
    }
    g_broker.workerCount = 0;
    if (g_broker.workEvent) CloseHandle(g_broker.workEvent);
    if (g_broker.stopEvent) CloseHandle(g_broker.stopEvent);
    if (g_broker.csInit) DeleteCriticalSection(&g_broker.cs);
    lopw::LogSetFile(L"");
}

}  // namespace
