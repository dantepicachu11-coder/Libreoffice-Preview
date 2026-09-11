// ==WindhawkMod==
// @id           lo-preview-broker
// @name         LibreOffice Preview Broker
// @description  Converts ODF documents to PDF for the Explorer preview pane. Runs inside explorer.exe (medium integrity) so LibreOffice gets a normal user token; the prevhost mod talks to it through the low-integrity scratch folder.
// @version      0.5.0
// @author       LOPreview
// @include      explorer.exe
// @compilerOptions -std=c++20 -lole32 -luuid -lshlwapi -lshell32 -ladvapi32
// ==/WindhawkMod==
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
#include "lop_core.h"  // inlined by tools/assemble.py
#include "lop_win.h"   // inlined by tools/assemble.py

namespace {

constexpr wchar_t kModVersion[] = L"0.5.0";
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
    lopw::LogInit(L"broker");
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

    lopw::LogI(L"LOPreview broker " + std::wstring(kModVersion) + L" starting in explorer.exe (pid " +
               lopw::Num(GetCurrentProcessId()) + L", integrity " + lopw::IntegrityLevelText() + L")");

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
    lopw::LogInit(L"broker");
    lopw::LogSetLevel(lopw::kLogInfo);
    // config.ini is read on the broker thread only; touching it here would race
    // with the thread we are about to start.
    Wh_Log(L"LOPreview broker %s loading in explorer.exe", kModVersion);

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
