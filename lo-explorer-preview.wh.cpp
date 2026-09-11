// ==WindhawkMod==
// @id           lo-explorer-preview
// @name         LibreOffice documents in the normal Explorer Preview Handler
// @description  Converts ODF streams to PDF and forwards them to the existing PDF Preview Handler.
// @version      0.4.0
// @author       OpenAI
// @include      prevhost.exe
// @compilerOptions -std=c++20 -lole32 -luuid -lshlwapi -lshell32
// ==/WindhawkMod==

#include <windows.h>
#include <shobjidl.h>
#include <objidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <algorithm>
#include <string>
#include <vector>
#include <new>
#include <cwctype>

#include "windhawk_api.h"
#include "windhawk_utils.h"

namespace {
constexpr wchar_t kPreviewGuid[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
using CoCreateInstance_t = decltype(&CoCreateInstance);
CoCreateInstance_t CoCreateInstance_Original = nullptr;
CLSID g_pdfHandlerClsid{};
bool g_havePdfHandlerClsid = false;

std::wstring Lower(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c){ return (wchar_t)towlower(c); }); return s; }
std::wstring ExtOf(const std::wstring& p) { size_t d=p.find_last_of(L'.'); return d==std::wstring::npos?L"":Lower(p.substr(d)); }
bool IsOdfExt(const std::wstring& e) { return e==L".odt"||e==L".ods"||e==L".odp"||e==L".odg"||e==L".odf"; }

std::wstring StreamName(IStream* s) {
    if (!s) return {};
    STATSTG st{};
    if (FAILED(s->Stat(&st, STATFLAG_DEFAULT))) return {};
    std::wstring r = st.pwcsName ? st.pwcsName : L"";
    if (st.pwcsName) CoTaskMemFree(st.pwcsName);
    return r;
}

bool CopyStreamToFile(IStream* in, const std::wstring& path) {
    if (!in) return false;
    LARGE_INTEGER z{}; in->Seek(z, STREAM_SEEK_SET, nullptr);
    HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (h==INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> b(1024*1024); ULONG got=0; bool ok=true;
    do { HRESULT hr=in->Read(b.data(),(ULONG)b.size(),&got); if (FAILED(hr)) {ok=false;break;} if (got) { DWORD wr=0; if (!WriteFile(h,b.data(),got,&wr,nullptr)||wr!=got){ok=false;break;} } } while(got);
    CloseHandle(h); return ok;
}

std::wstring FindLibreOffice() {
    wchar_t pf[MAX_PATH]{};
    const wchar_t* candidates[] = {
      L"C:\\Program Files\\LibreOffice\\program\\soffice.exe",
      L"C:\\Program Files (x86)\\LibreOffice\\program\\soffice.exe"
    };
    for (auto p:candidates) if (GetFileAttributesW(p)!=INVALID_FILE_ATTRIBUTES) return p;
    return {};
}

std::wstring MakeTempDir() {
    wchar_t t[MAX_PATH]{}; GetTempPathW(MAX_PATH,t);
    wchar_t n[MAX_PATH]{}; GetTempFileNameW(t,L"LOP",0,n); DeleteFileW(n);
    if (!CreateDirectoryW(n,nullptr)) return {};
    return n;
}

bool RunConvert(const std::wstring& input, const std::wstring& outDir, const std::wstring& profile) {
    auto soffice=FindLibreOffice();
    if (soffice.empty()) { Wh_Log(L"LibreOffice not found"); return false; }
    std::wstring cmd=L"\""+soffice+L"\" --headless --nologo --nodefault --nofirststartwizard --nolockcheck -env:UserInstallation=file:///"+profile+
      L" --convert-to pdf --outdir \""+outDir+L"\" \""+input+L"\"";
    std::vector<wchar_t> buf(cmd.begin(),cmd.end()); buf.push_back(0);
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,buf.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,outDir.c_str(),&si,&pi)) { Wh_Log(L"CreateProcess failed: %lu",GetLastError()); return false; }
    DWORD w=WaitForSingleObject(pi.hProcess,30000); DWORD ec=1; GetExitCodeProcess(pi.hProcess,&ec); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if(w!=WAIT_OBJECT_0 || ec!=0) { Wh_Log(L"LibreOffice failed wait=%lu exit=%lu",w,ec); return false; }
    return true;
}

bool ReadPdfHandlerClsid() {
    wchar_t buf[128]{}; DWORD cb=sizeof(buf); HKEY k=nullptr;
    std::wstring path=L"Software\\Classes\\.pdf\\shellex\\"+std::wstring(kPreviewGuid);
    if (RegGetValueW(HKEY_CURRENT_USER,path.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,buf,&cb)==ERROR_SUCCESS && SUCCEEDED(CLSIDFromString(buf,&g_pdfHandlerClsid))) return g_havePdfHandlerClsid=true;
    cb=sizeof(buf); path=L"SOFTWARE\\Classes\\.pdf\\shellex\\"+std::wstring(kPreviewGuid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,path.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,buf,&cb)==ERROR_SUCCESS && SUCCEEDED(CLSIDFromString(buf,&g_pdfHandlerClsid))) return g_havePdfHandlerClsid=true;
    return false;
}

struct StreamProxy final : IInitializeWithStream, IPreviewHandler, IObjectWithSite {
    LONG refs=1; IUnknown* innerUnknown=nullptr; IInitializeWithStream* innerInit=nullptr; IPreviewHandler* innerPreview=nullptr; IObjectWithSite* innerSite=nullptr;
    explicit StreamProxy(IUnknown* p):innerUnknown(p){ if(p)p->AddRef(); if(p)p->QueryInterface(IID_PPV_ARGS(&innerInit)); if(p)p->QueryInterface(IID_PPV_ARGS(&innerPreview)); if(p)p->QueryInterface(IID_PPV_ARGS(&innerSite)); }
    ~StreamProxy(){if(innerSite)innerSite->Release();if(innerPreview)innerPreview->Release();if(innerInit)innerInit->Release();if(innerUnknown)innerUnknown->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r,void**pp) override { if(!pp)return E_POINTER;*pp=nullptr; if(r==IID_IUnknown||r==IID_IInitializeWithStream)*pp=static_cast<IInitializeWithStream*>(this); else if(r==IID_IPreviewHandler)*pp=static_cast<IPreviewHandler*>(this); else if(r==IID_IObjectWithSite)*pp=static_cast<IObjectWithSite*>(this); else return innerUnknown?innerUnknown->QueryInterface(r,pp):E_NOINTERFACE; AddRef();return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);} ULONG STDMETHODCALLTYPE Release() override{ULONG r=InterlockedDecrement(&refs);if(!r)delete this;return r;}
    HRESULT STDMETHODCALLTYPE Initialize(IStream*,DWORD) override;
    HRESULT STDMETHODCALLTYPE SetWindow(HWND h,const RECT*r) override{return innerPreview?innerPreview->SetWindow(h,r):E_FAIL;} HRESULT STDMETHODCALLTYPE SetRect(const RECT*r) override{return innerPreview?innerPreview->SetRect(r):E_FAIL;} HRESULT STDMETHODCALLTYPE DoPreview() override{return innerPreview?innerPreview->DoPreview():E_FAIL;} HRESULT STDMETHODCALLTYPE Unload() override{return innerPreview?innerPreview->Unload():E_FAIL;} HRESULT STDMETHODCALLTYPE SetFocus() override{return innerPreview?innerPreview->SetFocus():E_FAIL;} HRESULT STDMETHODCALLTYPE QueryFocus(HWND* h) override{return innerPreview?innerPreview->QueryFocus(h):E_FAIL;} HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*m) override{return innerPreview?innerPreview->TranslateAccelerator(m):E_FAIL;}
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown*p) override{return innerSite?innerSite->SetSite(p):E_FAIL;} HRESULT STDMETHODCALLTYPE GetSite(REFIID r,void**p) override{return innerSite?innerSite->GetSite(r,p):E_FAIL;}
};

HRESULT StreamProxy::Initialize(IStream* stream,DWORD mode) {
    std::wstring name=StreamName(stream); Wh_Log(L"Initialize stream name='%s'",name.c_str());
    auto ext=ExtOf(name); if(!IsOdfExt(ext)) return innerInit?innerInit->Initialize(stream,mode):E_FAIL;
    std::wstring dir=MakeTempDir(); if(dir.empty()) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring src=dir+L"\\source"+ext, out=dir+L"\\source.pdf", profile=dir+L"\\profile"; CreateDirectoryW(profile.c_str(),nullptr);
    if(!CopyStreamToFile(stream,src)){Wh_Log(L"CopyStreamToFile failed %lu",GetLastError());return E_FAIL;}
    Wh_Log(L"Converting '%s'",src.c_str());
    if(!RunConvert(src,dir,profile)) return E_FAIL;
    if(GetFileAttributesW(out.c_str())==INVALID_FILE_ATTRIBUTES){Wh_Log(L"Expected PDF missing '%s'",out.c_str());return E_FAIL;}
    LARGE_INTEGER sz{}; WIN32_FILE_ATTRIBUTE_DATA fad{}; if(!GetFileAttributesExW(out.c_str(),GetFileExInfoStandard,&fad)||fad.nFileSizeHigh||fad.nFileSizeLow==0)return E_FAIL;
    IStream* pdf=nullptr; HRESULT hr=SHCreateStreamOnFileEx(out.c_str(),STGM_READ|STGM_SHARE_DENY_NONE,FILE_ATTRIBUTE_NORMAL,FALSE,nullptr,&pdf);
    if(FAILED(hr)){Wh_Log(L"SHCreateStreamOnFileEx failed 0x%08lx",hr);return hr;}
    Wh_Log(L"Forwarding generated PDF '%s'",out.c_str()); hr=innerInit?innerInit->Initialize(pdf,STGM_READ):E_FAIL; pdf->Release(); return hr;
}

HRESULT WINAPI CoCreateInstance_Hook(REFCLSID c,LPUNKNOWN outer,DWORD ctx,REFIID riid,LPVOID*ppv){
    HRESULT hr=CoCreateInstance_Original(c,outer,ctx,riid,ppv); if(FAILED(hr)||!ppv||!*ppv||!g_havePdfHandlerClsid||!IsEqualCLSID(c,g_pdfHandlerClsid))return hr;
    if(riid==IID_IUnknown||riid==IID_IInitializeWithStream||riid==IID_IPreviewHandler||riid==IID_IObjectWithSite){IUnknown* inner=(IUnknown*)*ppv;auto* p=new(std::nothrow)StreamProxy(inner);if(!p)return hr;inner->Release();if(riid==IID_IUnknown)*ppv=(IUnknown*)(IInitializeWithStream*)p;else if(riid==IID_IInitializeWithStream)*ppv=(IInitializeWithStream*)p;else if(riid==IID_IPreviewHandler)*ppv=(IPreviewHandler*)p;else *ppv=(IObjectWithSite*)p;}return hr;
}
}
BOOL Wh_ModInit(){ReadPdfHandlerClsid();if(!g_havePdfHandlerClsid){Wh_Log(L"PDF preview handler not found");return TRUE;}Wh_Log(L"PDF Preview Handler found");if(!WindhawkUtils::SetFunctionHook(CoCreateInstance,CoCreateInstance_Hook,&CoCreateInstance_Original)){Wh_Log(L"Hook failed");return FALSE;}return TRUE;}
void Wh_ModUninit(){}
