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

#ifndef LOPREVIEW_CORE_H
#define LOPREVIEW_CORE_H

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
        std::string line = TrimAscii(body.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        // Trailing comments: ';' or '#' preceded by whitespace (the generated
        // config.ini documents every value this way).  A separator glued to
        // the value (e.g. in a path) is kept.
        for (size_t i = 1; i < line.size(); ++i) {
            if ((line[i] == ';' || line[i] == '#') &&
                (line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line = TrimAscii(line.substr(0, i));
                break;
            }
        }
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

#endif  // LOPREVIEW_CORE_H
