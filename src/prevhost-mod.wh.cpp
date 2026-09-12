// ==WindhawkMod==
// @id           lo-explorer-preview
// @name         LibreOffice documents in the normal Explorer Preview Handler
// @description  Lets the normal Explorer preview pane show .odt/.ods/.odp/.odg (and .odf) documents by converting them to PDF with LibreOffice and handing that PDF to the PDF preview handler Explorer already uses. No new preview app, no Office, no network.
// @version      0.5.0
// @author       LOPreview
// @include      prevhost.exe
// @compilerOptions -std=c++20 -lole32 -luuid -lshlwapi -lshell32 -ladvapi32
// ==/WindhawkMod==
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
#include "lop_core.h"  // inlined by tools/assemble.py
#include "lop_win.h"   // inlined by tools/assemble.py

namespace {

constexpr wchar_t kModVersion[] = L"0.5.0";
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

    // 6. last resort: ask the Shell for the handler registration of .pdf via
    //    AssocQueryString (covers handlers registered through less obvious
    //    places without hard-coding a CLSID).
    wchar_t value[256]{};
    DWORD size = sizeof(value);
    const std::wstring assocKey =
        std::wstring(L"SystemFileAssociations\\.pdf\\ShellEx\\") + kPreviewHandlerGuid;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLIDLIST, L".pdf", nullptr,
                                    value, &size)) &&
        ClsidFromRegistryText(value, clsid)) {
        g_pdfHandlerClsid = clsid;
        g_havePdfHandlerClsid = true;
        g_pdfHandlerHow = L"AssocQueryString";
        return true;
    }
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

    Wh_Log(L"LOPreview prevhost client %s starting (pid %u, integrity %s)", kModVersion,
           GetCurrentProcessId(), lopw::IntegrityLevelText().c_str());
    lopw::LogI(L"LOPreview " + std::wstring(kModVersion) + L" in prevhost.exe (pid " +
               lopw::Num(GetCurrentProcessId()) + L", integrity " + lopw::IntegrityLevelText() + L")");

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
