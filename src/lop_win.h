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

#ifndef LOPREVIEW_WIN_H
#define LOPREVIEW_WIN_H

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#include "windhawk_api.h"
#include "lop_core.h"

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
    int level = kLogInfo;
    unsigned long long bytesWritten = 0;
};

inline LoggerState& LogState() {
    static LoggerState s;  // function-local static: initialised on first use
    return s;
}

inline void LogInit(const std::wstring& role) {
    LoggerState& s = LogState();
    if (!s.csInit) {
        InitializeCriticalSection(&s.cs);
        s.csInit = true;
    }
    s.role = role;
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
        const char* mark = "\xEF\xBB\xBF--- LOPreview log opened ---\r\n";
        DWORD written = 0;
        WriteFile(s.file, mark, 39, &written, nullptr);
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
    std::wstring line = std::wstring(L"[") + tag + L"] " + text;
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

#endif  // LOPREVIEW_WIN_H
