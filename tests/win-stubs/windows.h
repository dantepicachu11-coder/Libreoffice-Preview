// Minimal Win32/COM stub - see README.md in this folder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

#define WINAPI
#define STDMETHODCALLTYPE
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#define WINBASEAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef int INT;
typedef wchar_t WCHAR;
typedef WCHAR* LPWSTR;
typedef const WCHAR* LPCWSTR;
typedef LPWSTR LPOLESTR;
typedef LPCWSTR LPCOLESTR;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef void* HANDLE;
typedef DWORD* LPDWORD;
typedef LONG* LPLONG;
typedef BOOL* LPBOOL;
typedef UINT* LPUINT;
typedef size_t SIZE_T;
typedef unsigned long long DWORD_PTR;
typedef long long LONG_PTR;
typedef long long INT_PTR;
typedef unsigned long long UINT_PTR;
typedef DWORD_PTR ULONG_PTR_T;
typedef void* PVOID;
typedef LPWSTR PWSTR;
typedef void* HWND;
typedef void* HKEY;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HGLOBAL;
typedef void* HLOCAL;
typedef void* LPSECURITY_ATTRIBUTES;
typedef DWORD_PTR* PDWORD_PTR;
typedef void* PSID;
typedef unsigned char UCHAR;
typedef UCHAR* PUCHAR;
typedef WCHAR* LPWCH;
typedef long LSTATUS;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long DWORDLONG;
typedef long HRESULT;
typedef WORD ATOM;
typedef HANDLE HKEY_;

#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_DISK_FULL 112L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_CANCELLED 1223L
#define ERROR_MORE_DATA 234L
#define WAIT_OBJECT_0 0
#define WAIT_ABANDONED 0x80
#define WAIT_TIMEOUT 258
#define WAIT_FAILED 0xFFFFFFFF
#define INFINITE 0xFFFFFFFF

struct GUID {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
};
typedef GUID IID;
typedef GUID CLSID;
typedef const GUID& REFGUID;
typedef const GUID& REFIID;
typedef const GUID& REFCLSID;
typedef const GUID& REFKNOWNFOLDERID;
typedef GUID KNOWNFOLDERID;
typedef const CLSID& REFCLSID_;

inline bool operator==(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }

struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
};
struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
struct LARGE_INTEGER {
    union {
        struct {
            DWORD LowPart;
            LONG HighPart;
        };
        long long QuadPart;
    };
};
struct ULARGE_INTEGER {
    union {
        struct {
            DWORD LowPart;
            DWORD HighPart;
        };
        unsigned long long QuadPart;
    };
};
struct RECT {
    LONG left, top, right, bottom;
};
struct MSG {
    HWND hwnd;
    UINT message;
    DWORD_PTR wParam, lParam;
    DWORD time;
};
struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
};
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};
struct STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    BYTE* lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
};
typedef STARTUPINFOW STARTUPINFO;
struct PROCESS_INFORMATION {
    HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId;
};
struct LPPROC_THREAD_ATTRIBUTE_LIST_ { void* p; };
typedef LPPROC_THREAD_ATTRIBUTE_LIST_* LPPROC_THREAD_ATTRIBUTE_LIST;
struct STARTUPINFOEXW {
    STARTUPINFOW StartupInfo;
    LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList;
};
typedef STARTUPINFOEXW STARTUPINFOEX;
typedef STARTUPINFOW* LPSTARTUPINFOW;
typedef PROCESS_INFORMATION* LPPROCESS_INFORMATION;
struct CRITICAL_SECTION {
    void* opaque[5];
};
typedef DWORD_PTR* LPULONG_PTR;
typedef ULONG_PTR_T ULONG_PTR;

#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ZeroMemory(p, n) std::memset((p), 0, (n))
#define RtlZeroMemory(p, n) std::memset((p), 0, (n))
#define MAKELONG(a, b) 0
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) \
                                : ((HRESULT)(((x) & 0x0000FFFF) | 0x80070000)))
#define MAKE_HRESULT(s, f) ((HRESULT)(((DWORD)(s) << 31) | (DWORD)(f)))

#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_ABORT ((HRESULT)0x80004004L)
#define STG_E_INVALIDFUNCTION ((HRESULT)0x80030001L)
#define STG_E_ACCESSDENIED ((HRESULT)0x80030005L)
#define STG_E_INVALIDPOINTER ((HRESULT)0x80030009L)
#define STG_E_INUSE ((HRESULT)0x8003001EL)

#define STGM_READ 0x00000000L
#define STGM_WRITE 0x00000001L
#define STGM_READWRITE 0x00000002L
#define STGM_SHARE_DENY_NONE 0x00000040L
#define STGM_CREATE 0x00001000L
#define STREAM_SEEK_SET 0
#define STREAM_SEEK_CUR 1
#define STREAM_SEEK_END 2
#define STGTY_STREAM 2
#define STATFLAG_DEFAULT 0
#define STATFLAG_NONAME 1
#define LOCK_WRITE 1

struct STATSTG {
    LPOLESTR pwcsName;
    DWORD type;
    ULARGE_INTEGER cbSize;
    FILETIME mtime, ctime, atime;
    DWORD grfMode, grfLocksSupported;
    CLSID clsid;
    DWORD grfStateBits, reserved;
};

struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
typedef IUnknown* LPUNKNOWN;
struct ISequentialStream : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) = 0;
    virtual HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) = 0;
};
struct IStream : public ISequentialStream {
    virtual HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                                          ULARGE_INTEGER* plibNewPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyTo(IStream* pstm, ULARGE_INTEGER cb,
                                             ULARGE_INTEGER* pcbRead,
                                             ULARGE_INTEGER* pcbWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE Commit(DWORD grfCommitFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Revert() = 0;
    virtual HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb,
                                                 DWORD dwLockType) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb,
                                                   DWORD dwLockType) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) = 0;
    virtual HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) = 0;
};
struct IPreviewHandler : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetWindow(HWND hwnd, const RECT* prc) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRect(const RECT* prc) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoPreview() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unload() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryFocus(HWND* phwnd) = 0;
    virtual HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG* pmsg) = 0;
};
struct IInitializeWithStream : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD grfMode) = 0;
};
struct IInitializeWithFile : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD grfMode) = 0;
};
struct IObjectWithSite : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) = 0;
};
enum SIGDN {
    SIGDN_NORMALDISPLAY = 0,
    SIGDN_PARENTRELATIVEPARSING = 0x80018001,
    SIGDN_DESKTOPABSOLUTEPARSING = 0x80028000,
    SIGDN_FILESYSPATH = 0x80058000,
};
struct IShellItem : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BindToHandler(void*, REFGUID, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParent(IShellItem**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN sigdnName, LPWSTR* ppszName) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAttributes(ULONG sfgaoMask, ULONG* psfgaoAttribs) = 0;
    virtual HRESULT STDMETHODCALLTYPE Compare(IShellItem* psi, DWORD hint, int* piOrder) = 0;
};

extern const GUID IID_IUnknown;
extern const GUID IID_IStream;
extern const GUID IID_ISequentialStream;
extern const GUID IID_IPreviewHandler;
extern const GUID IID_IInitializeWithStream;
extern const GUID IID_IInitializeWithFile;
extern const GUID IID_IObjectWithSite;
extern const GUID IID_IShellItem;
extern const GUID CLSID_ShellItem;
extern const KNOWNFOLDERID FOLDERID_LocalAppData;
extern const KNOWNFOLDERID FOLDERID_LocalAppDataLow;
extern const KNOWNFOLDERID FOLDERID_Profile;

template <typename T>
const GUID& StubIidOf(T**);
template <>
inline const GUID& StubIidOf<IUnknown>(IUnknown**) { return IID_IUnknown; }
template <>
inline const GUID& StubIidOf<IStream>(IStream**) { return IID_IStream; }
template <>
inline const GUID& StubIidOf<IPreviewHandler>(IPreviewHandler**) { return IID_IPreviewHandler; }
template <>
inline const GUID& StubIidOf<IInitializeWithStream>(IInitializeWithStream**) {
    return IID_IInitializeWithStream;
}
template <>
inline const GUID& StubIidOf<IInitializeWithFile>(IInitializeWithFile**) {
    return IID_IInitializeWithFile;
}
template <>
inline const GUID& StubIidOf<IObjectWithSite>(IObjectWithSite**) { return IID_IObjectWithSite; }
template <>
inline const GUID& StubIidOf<IShellItem>(IShellItem**) { return IID_IShellItem; }
#define IID_PPV_ARGS(ppType) StubIidOf(ppType), reinterpret_cast<void**>(ppType)

// ---------------- attributes / flags ----------------
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define GENERIC_READ 0x80000000L
#define FILE_APPEND_DATA 0x00000004L
#define GENERIC_WRITE 0x40000000L
#define GENERIC_ALL 0x10000000L
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
#define CREATE_NO_WINDOW 0x08000000
#define CREATE_SUSPENDED 0x00000004
#define CREATE_UNICODE_ENVIRONMENT 0x00000400
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define STARTF_USESHOWWINDOW 1
#define STARTF_USESTDHANDLES 0x100
#define SW_HIDE 0
#define HANDLE_FLAG_INHERIT 1
#define PIPE_ACCESS_DUPLEX 3
#define PIPE_TYPE_BYTE 0
#define PIPE_READMODE_BYTE 0
#define PIPE_WAIT 0
#define FILE_NOTIFY_CHANGE_FILE_NAME 1
#define FILE_NOTIFY_CHANGE_LAST_WRITE 16
#define FILE_NOTIFY_CHANGE_SIZE 8
#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#define JobObjectExtendedLimitInformation 9
#define TOKEN_QUERY 0x0008
#define TokenIntegrityLevel 25
#define SECURITY_MANDATORY_LOW_RID 0x1000
#define SECURITY_MANDATORY_MEDIUM_RID 0x2000
#define SECURITY_MANDATORY_HIGH_RID 0x3000
#define RRF_RT_REG_SZ 2
#define RRF_RT_REG_EXPAND_SZ 4
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define HKEY_CURRENT_USER ((HKEY)(LONG_PTR)-2147483647LL)
#define HKEY_LOCAL_MACHINE ((HKEY)(LONG_PTR)-2147483646LL)
#define HKEY_CLASSES_ROOT ((HKEY)(LONG_PTR)-2147483648LL)
#define CLSCTX_INPROC_SERVER 1
#define CLSCTX_LOCAL_SERVER 4
#define CLSCTX_ALL 0x17
#define COINIT_APARTMENTTHREADED 2
// ASSOCF / ASSOCSTR mirror the Windows SDK exactly.  This stub once
// *invented* ASSOCSTR_SHELLIDLIST, which let a real compile error (clang:
// "use of undeclared identifier") slip past the local compile check - so the
// members below are the complete, real ones and nothing else may be added.
enum ASSOCF {
    ASSOCF_NONE = 0x00000000,
    ASSOCF_INIT_NOREMAPCLSID = 0x00000001,
    ASSOCF_INIT_BYEXENAME = 0x00000002,
    ASSOCF_OPEN_BYEXENAME = 0x00000002,
    ASSOCF_INIT_DEFAULTTOSTAR = 0x00000004,
    ASSOCF_INIT_DEFAULTTOFOLDER = 0x00000008,
    ASSOCF_NOUSERSETTINGS = 0x00000010,
    ASSOCF_NOTRUNCATE = 0x00000020,
    ASSOCF_VERIFY = 0x00000040,
    ASSOCF_REMAPRUNDLL = 0x00000080,
    ASSOCF_NOFIXUPS = 0x00000100,
    ASSOCF_IGNOREBASECLASS = 0x00000200,
    ASSOCF_INIT_IGNOREUNKNOWN = 0x00000400,
    ASSOCF_INIT_FIXED_PROGID = 0x00000800,
    ASSOCF_IS_PROTOCOL = 0x00001000,
    ASSOCF_INIT_FOR_FILE = 0x00002000,
    ASSOCF_IS_FILETYPE = 0x00004000,
    ASSOCF_TRUSTED = 0x00008000,
    ASSOCF_NOREMAPCLSID = 0x00010000,
    ASSOCF_DISALLOWEXEC = 0x00020000
};
enum ASSOCSTR {
    ASSOCSTR_COMMAND = 1,
    ASSOCSTR_EXECUTABLE,
    ASSOCSTR_FRIENDLYDOCNAME,
    ASSOCSTR_FRIENDLYAPPNAME,
    ASSOCSTR_NOOPEN,
    ASSOCSTR_SHELLNEWVALUE,
    ASSOCSTR_DDECOMMAND,
    ASSOCSTR_DDEIFEXEC,
    ASSOCSTR_DDEAPPLICATION,
    ASSOCSTR_DDETOPIC,
    ASSOCSTR_INFOTIP,
    ASSOCSTR_QUICKTIP,
    ASSOCSTR_TILEINFO,
    ASSOCSTR_CONTENTTYPE,
    ASSOCSTR_DEFAULTICON,
    ASSOCSTR_SHELLEXTENSION,
    ASSOCSTR_DROPTARGET,
    ASSOCSTR_DELEGATEEXECUTE,
    ASSOCSTR_SUPPORTED_URI_PROTOCOLS,
    ASSOCSTR_PROGID,
    ASSOCSTR_APPID,
    ASSOCSTR_APPPUBLISHER,
    ASSOCSTR_APPICONREFERENCE,
    ASSOCSTR_MAX
};

struct SID_AND_ATTRIBUTES {
    PSID Sid;
    DWORD Attributes;
};
struct TOKEN_MANDATORY_LABEL {
    SID_AND_ATTRIBUTES Label;
};
struct JOBOBJECT_BASIC_LIMIT_INFORMATION {
    LARGE_INTEGER PerProcessUserTimeLimit;
    LARGE_INTEGER PerJobUserTimeLimit;
    DWORD LimitFlags;
    SIZE_T MinimumWorkingSetSize;
    SIZE_T MaximumWorkingSetSize;
    DWORD ActiveProcessLimit;
    DWORD_PTR Affinity;
    DWORD PriorityClass;
    DWORD SchedulingClass;
};
struct IO_COUNTERS {
    ULONGLONG ReadOperationCount, WriteOperationCount, OtherOperationCount;
    ULONGLONG ReadTransferCount, WriteTransferCount, OtherTransferCount;
};
struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    IO_COUNTERS IoInfo;
    SIZE_T ProcessMemoryLimit;
    SIZE_T JobMemoryLimit;
    SIZE_T PeakProcessMemoryUsed;
    SIZE_T PeakJobMemoryUsed;
};

// ---------------- functions ----------------
DWORD GetLastError();
void SetLastError(DWORD);
void Sleep(DWORD);
ULONGLONG GetTickCount64();
HANDLE GetCurrentProcess();
DWORD GetCurrentProcessId();
void GetSystemTimeAsFileTime(FILETIME*);
void GetLocalTime(SYSTEMTIME*);
BOOL CloseHandle(HANDLE);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR);
BOOL SetEvent(HANDLE);
BOOL ResetEvent(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
DWORD WaitForMultipleObjects(DWORD, const HANDLE*, BOOL, DWORD);
HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, LPVOID);
BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPVOID);
BOOL FlushFileBuffers(HANDLE);
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, DWORD);
DWORD GetFileAttributesW(LPCWSTR);
BOOL GetFileAttributesExW(LPCWSTR, GET_FILEEX_INFO_LEVELS, WIN32_FILE_ATTRIBUTE_DATA*);
BOOL SetFileAttributesW(LPCWSTR, DWORD);
BOOL DeleteFileW(LPCWSTR);
BOOL MoveFileW(LPCWSTR, LPCWSTR);
BOOL MoveFileExW(LPCWSTR, LPCWSTR, DWORD);
BOOL CopyFileW(LPCWSTR, LPCWSTR, BOOL);
BOOL CreateDirectoryW(LPCWSTR, LPSECURITY_ATTRIBUTES);
BOOL RemoveDirectoryW(LPCWSTR);
HANDLE FindFirstFileW(LPCWSTR, WIN32_FIND_DATAW*);
BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW*);
BOOL FindClose(HANDLE);
BOOL SetHandleInformation(HANDLE, DWORD, DWORD);
BOOL CreatePipe(HANDLE*, HANDLE*, LPSECURITY_ATTRIBUTES, DWORD);
BOOL PeekNamedPipe(HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD);
BOOL CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                    LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
BOOL InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, SIZE_T*);
void DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST);
BOOL UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T,
                               PVOID, SIZE_T*);
DWORD ResumeThread(HANDLE);
BOOL GetExitCodeProcess(HANDLE, LPDWORD);
HANDLE CreateJobObjectW(LPSECURITY_ATTRIBUTES, LPCWSTR);
BOOL SetInformationJobObject(HANDLE, int, LPVOID, DWORD);
BOOL AssignProcessToJobObject(HANDLE, HANDLE);
BOOL TerminateJobObject(HANDLE, UINT);
BOOL TerminateProcess(HANDLE, UINT);
HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, DWORD (*)(LPVOID), LPVOID, DWORD, LPDWORD);
void InitializeCriticalSection(CRITICAL_SECTION*);
void EnterCriticalSection(CRITICAL_SECTION*);
void LeaveCriticalSection(CRITICAL_SECTION*);
void DeleteCriticalSection(CRITICAL_SECTION*);
LONG InterlockedIncrement(volatile LONG*);
LONG InterlockedDecrement(volatile LONG*);
LONG InterlockedExchange(volatile LONG*, LONG);
LONG InterlockedCompareExchange(volatile LONG*, LONG, LONG);
HANDLE GetProcessHeap();
LPVOID HeapAlloc(HANDLE, DWORD, SIZE_T);
BOOL HeapFree(HANDLE, DWORD, LPVOID);
DWORD GetEnvironmentVariableW(LPCWSTR, LPWSTR, DWORD);
LPWCH GetEnvironmentStringsW();
BOOL FreeEnvironmentStringsW(LPWCH);
DWORD ExpandEnvironmentStringsW(LPCWSTR, LPWSTR, DWORD);
DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
DWORD SearchPathW(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPWSTR, LPWSTR*);
DWORD GetFullPathNameW(LPCWSTR, DWORD, LPWSTR, LPWSTR*);
UINT GetDriveTypeW(LPCWSTR);
HWND GetShellWindow();
DWORD GetWindowThreadProcessId(HWND, LPDWORD);
LSTATUS RegGetValueW(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
HANDLE FindFirstChangeNotificationW(LPCWSTR, BOOL, DWORD);
BOOL FindNextChangeNotification(HANDLE);
BOOL FindCloseChangeNotification(HANDLE);
HRESULT CoInitializeEx(LPVOID, DWORD);
void CoUninitialize();
HRESULT CoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
HRESULT StringFromGUID2(REFGUID, LPOLESTR, int);
BOOL IsEqualCLSID(REFCLSID, REFCLSID);
BOOL IsEqualIID(REFIID, REFIID);
HRESULT CLSIDFromString(LPCWSTR, CLSID*);
LPVOID CoTaskMemAlloc(SIZE_T);
void CoTaskMemFree(LPVOID);
BOOL OpenProcessToken(HANDLE, DWORD, HANDLE*);
BOOL GetTokenInformation(HANDLE, int, LPVOID, DWORD, LPDWORD);
PUCHAR GetSidSubAuthorityCount(PSID);
DWORD* GetSidSubAuthority(PSID, DWORD);
HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
HRESULT SHCreateStreamOnFileEx(LPCWSTR, DWORD, DWORD, BOOL, IStream*, IStream**);
HRESULT AssocQueryStringW(ASSOCF, ASSOCSTR, LPCWSTR, LPCWSTR, LPWSTR, DWORD*);

// Wide string helpers from <string.h>/<wchar.h> that the real SDK pulls in.
extern "C" int _wcsicmp(const wchar_t*, const wchar_t*);
extern "C" int _wcsnicmp(const wchar_t*, const wchar_t*, size_t);
extern "C" size_t wcslen(const wchar_t*);
extern "C" int wcscmp(const wchar_t*, const wchar_t*);

// The GUID definitions are only needed when linking; -fsyntax-only never does.
