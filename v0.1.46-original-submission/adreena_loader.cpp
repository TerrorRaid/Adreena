// ADREENA_Loader: single-file loader. Prepares Documents\ADREENA, extracts the embedded components, shows a loading
// window and (once ready) a Launch button. ADREENA.dll stays an injected in-game menu; this program never starts the game
// by itself. Phase 1: folders + extraction + window (step 2); the Launch button runs the existing Steam start + DLL injection (step 3).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>
using std::min;using std::max;
#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>
using namespace Gdiplus;
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"bcrypt.lib")
#include <bcrypt.h>

// ---- hidden strings: a handful of names (the DLL, the game process, the Steam launch URI, the loader's own session
// name, the registry keys it reads) are kept out of the binary as plain text. This only defeats a quick look with
// "strings" or a hex editor - it is not a cryptographic protection, and the roadmap is explicit that nothing here is.
template<size_t N> struct ObStr{
    wchar_t buf[N]{};
    constexpr ObStr(const wchar_t (&s)[N]){for(size_t i=0;i<N;++i)buf[i]=(wchar_t)(s[i]^(0x53A1u+i*0x9Du));}
};
template<size_t N> static std::wstring deob(const ObStr<N>& o){
    std::wstring r(N-1,0);for(size_t i=0;i<N-1;++i)r[i]=(wchar_t)(o.buf[i]^(0x53A1u+i*0x9Du));return r;
}
#define HSTR(lit) ([]()->const std::wstring&{static constexpr ObStr<sizeof(L##lit)/sizeof(wchar_t)> o(L##lit);static const std::wstring v=deob(o);return v;}())

// ---- file integrity: SHA-256 via Windows' own crypto (BCrypt), so an ADREENA.dll swapped in after the loader last
// verified it (while the window sits open and Ready) is caught before it gets injected into the game.
static bool sha256(const void* data,size_t len,std::vector<BYTE>& out){
    BCRYPT_ALG_HANDLE alg=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    DWORD hashLen=0,cb=0;BCryptGetProperty(alg,BCRYPT_HASH_LENGTH,(PUCHAR)&hashLen,sizeof(hashLen),&cb,0);
    BCRYPT_HASH_HANDLE h=nullptr;bool ok=false;
    if(hashLen&&BCryptCreateHash(alg,&h,nullptr,0,nullptr,0,0)>=0){
        ok=BCryptHashData(h,(PUCHAR)data,(ULONG)len,0)>=0;
        if(ok){out.resize(hashLen);ok=BCryptFinishHash(h,out.data(),hashLen,0)>=0;}
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg,0);return ok;
}

static const wchar_t* LOADER_VERSION=L"v0.1.46";
static const UINT WM_STATE=WM_APP+1;

enum State{ST_LOADING,ST_READY,ST_FAILED,ST_LAUNCHING,ST_LAUNCH_FAILED,ST_DONE};
static CRITICAL_SECTION g_cs;
static State g_state=ST_LOADING;
static std::wstring g_status=L"Starting...",g_note;
static int g_prog=0;                 // 0..1000
static std::wstring g_root;          // Documents\ADREENA
static HWND g_hwnd=nullptr;
// Session record the DLL checks on attach (see launchedByLoader in adreena_internal.cpp): only a game started by this loader gets the menu.
struct Session{DWORD magic,loaderPid,gamePid;};
static Session* g_session=nullptr;
static void openSession(){
    if(g_session)return;
    HANDLE m=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Session),HSTR("Local\\ADREENA_LoaderSession").c_str());
    if(!m)return;
    g_session=(Session*)MapViewOfFile(m,FILE_MAP_WRITE,0,0,sizeof(Session));   // handle stays open for the life of the loader
    if(g_session){g_session->magic=0x4E524441;g_session->loaderPid=GetCurrentProcessId();g_session->gamePid=0;}
}
static double g_scale=1.0;
static int S(int v){return (int)(v*g_scale+0.5);}

// ---- logging (Logs\ADREENA_loader.log) -------------------------------------------------------------------------
static std::string toUtf8(const std::wstring& w){
    if(w.empty())return "";
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,0);WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,nullptr,nullptr);return s;
}
static std::wstring fromUtf8(const std::string& s){
    if(s.empty())return L"";
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),nullptr,0);
    std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n);return w;
}
// One logger feeds the debug window and Logs\ADREENA_loader.log. "detail" entries show only in Detailed mode.
enum Lvl{LV_INFO=0,LV_OK,LV_WARN,LV_ERR};
struct DbgEntry{SYSTEMTIME t;int lvl;std::wstring text;bool detail;bool file;};
static std::vector<DbgEntry> g_dbg;
static size_t g_dbgWritten=0;
static CRITICAL_SECTION g_dbgCs;
static bool g_detailMode=false;
static HWND g_dbgHwnd=nullptr;
static const UINT WM_DBGLOG=WM_APP+2;
static DWORD findGame();
static const wchar_t* tagOf(const DbgEntry& e){
    if(e.lvl==LV_OK)return L"SUCCESS";if(e.lvl==LV_WARN)return L"WARNING";if(e.lvl==LV_ERR)return L"ERROR";
    return e.detail?L"DEBUG":L"INFO";
}
static void flushLogLocked(){
    if(g_root.empty()||g_dbgWritten>=g_dbg.size())return;
    std::string out;
    for(size_t i=g_dbgWritten;i<g_dbg.size();++i){
        const DbgEntry& e=g_dbg[i];if(!e.file)continue;
        char head[64];snprintf(head,sizeof(head),"[%04d-%02d-%02d %02d:%02d:%02d] ",e.t.wYear,e.t.wMonth,e.t.wDay,e.t.wHour,e.t.wMinute,e.t.wSecond);
        std::wstring tag=tagOf(e);while(tag.size()<8)tag+=L' ';
        out+=std::string(head)+toUtf8(tag)+toUtf8(e.text)+"\r\n";
    }
    g_dbgWritten=g_dbg.size();
    if(out.empty())return;
    HANDLE h=CreateFileW((g_root+L"\\Logs\\ADREENA_loader.log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return;
    DWORD w=0;WriteFile(h,out.data(),(DWORD)out.size(),&w,nullptr);CloseHandle(h);
}
static void addEntry(int lvl,const std::wstring& text,bool detail,bool file=true){
    EnterCriticalSection(&g_dbgCs);
    DbgEntry e;GetLocalTime(&e.t);e.lvl=lvl;e.text=text;e.detail=detail;e.file=file;g_dbg.push_back(e);
    flushLogLocked();
    if(g_dbg.size()>2000){const size_t cut=500;g_dbg.erase(g_dbg.begin(),g_dbg.begin()+cut);g_dbgWritten=g_dbgWritten>cut?g_dbgWritten-cut:0;}
    LeaveCriticalSection(&g_dbgCs);
    if(g_dbgHwnd)PostMessageW(g_dbgHwnd,WM_DBGLOG,0,0);
}
static void say(int lvl,const std::wstring& text){addEntry(lvl,text,false);}      // plain-language message, shown in Normal mode
static void logLine(const std::wstring& text){addEntry(LV_INFO,text,true);}        // technical detail, shown in Detailed mode
static void setStatus(const std::wstring& text,int prog){
    EnterCriticalSection(&g_cs);g_status=text;g_prog=prog;LeaveCriticalSection(&g_cs);
    if(g_hwnd)PostMessageW(g_hwnd,WM_STATE,0,0);
}

// ---- file helpers ----------------------------------------------------------------------------------------------
static bool readWhole(const std::wstring& path,std::vector<char>& out){
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER sz{};GetFileSizeEx(h,&sz);
    if(sz.QuadPart<0||sz.QuadPart>(1LL<<30)){CloseHandle(h);return false;}
    out.assign((size_t)sz.QuadPart,0);
    size_t done=0;
    while(done<out.size()){DWORD got=0;if(!ReadFile(h,&out[done],(DWORD)(out.size()-done),&got,nullptr)||!got)break;done+=got;}
    CloseHandle(h);return done==out.size();
}
static bool fileExists(const std::wstring& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;}
// Writes to <path>.tmp then replaces <path>. locked is set when the target is in use (for example the DLL inside a running game).
static bool writeAtomic(const std::wstring& path,const void* data,size_t n,bool& locked){
    locked=false;
    const std::wstring tmp=path+L".tmp";
    HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    const char* p=(const char*)data;size_t done=0;bool ok=true;
    while(done<n){DWORD w=0;if(!WriteFile(h,p+done,(DWORD)(n-done),&w,nullptr)||!w){ok=false;break;}done+=w;}
    CloseHandle(h);
    if(!ok){DeleteFileW(tmp.c_str());return false;}
    if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        const DWORD e=GetLastError();
        locked=(e==ERROR_ACCESS_DENIED||e==ERROR_SHARING_VIOLATION||e==ERROR_LOCK_VIOLATION);
        DeleteFileW(tmp.c_str());return false;
    }
    return true;
}
static const char* embedded(int id,DWORD& size){
    HRSRC r=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);if(!r)return nullptr;
    HGLOBAL g=LoadResource(nullptr,r);if(!g)return nullptr;
    size=SizeofResource(nullptr,r);return (const char*)LockResource(g);
}

// ---- settings, pre-flight checks and the menu-log follower ------------------------------------------------------
static void loadSettings(){
    std::vector<char> v;g_detailMode=false;
    if(readWhole(g_root+L"\\Cache\\loader_settings.txt",v)){const std::string t(v.begin(),v.end());g_detailMode=t.find("detail=1")!=std::string::npos;}
}
static void saveSettings(){
    if(g_root.empty())return;
    const char* t=g_detailMode?"detail=1\r\n":"detail=0\r\n";bool l=false;writeAtomic(g_root+L"\\Cache\\loader_settings.txt",t,strlen(t),l);
}
static std::wstring steamPath(){
    wchar_t buf[512]{};DWORD sz=sizeof(buf)-2;
    if(RegGetValueW(HKEY_CURRENT_USER,HSTR("Software\\Valve\\Steam").c_str(),HSTR("SteamPath").c_str(),RRF_RT_REG_SZ,nullptr,buf,&sz)!=ERROR_SUCCESS)return L"";
    std::wstring p=buf;for(auto& c:p)if(c==L'/')c=L'\\';return p;
}
// Information only: nothing here ever blocks launching.
static void preflight(){
    const std::wstring steam=steamPath();
    if(steam.empty()){say(LV_WARN,L"Steam was not found. Launching needs Steam installed and signed in.");}
    else{
        logLine(L"Steam path: "+steam);say(LV_OK,L"Steam found");
        std::vector<std::wstring> libs{steam};
        std::vector<char> v;
        if(readWhole(steam+L"\\steamapps\\libraryfolders.vdf",v)){
            const std::string t(v.begin(),v.end());size_t pos=0;
            while((pos=t.find("\"path\"",pos))!=std::string::npos){
                pos+=6;const size_t a=t.find('"',pos);if(a==std::string::npos)break;
                std::string val;size_t i=a+1;
                for(;i<t.size()&&t[i]!='"';++i){if(t[i]=='\\'&&i+1<t.size()&&t[i+1]=='\\'){val+='\\';++i;}else val+=t[i];}
                if(!val.empty())libs.push_back(fromUtf8(val));
                pos=i;
            }
        }
        bool found=false;
        for(const std::wstring& l:libs)if(fileExists(l+L"\\steamapps\\appmanifest_495420.acf")){found=true;logLine(L"State of Decay 2 found in Steam library: "+l);break;}
        if(found)say(LV_OK,L"State of Decay 2 is installed");
        else say(LV_WARN,L"State of Decay 2 was not found in your Steam libraries.");
    }
    const DWORD pid=findGame();
    logLine(pid?L"Game process is running (pid "+std::to_wstring(pid)+L")":std::wstring(L"Game is not running"));
}
static ULONGLONG g_dllOff=0;static bool g_dllInit=false;
static void dllLine(const std::string& raw){
    std::string t=raw;while(!t.empty()&&(t.back()=='\r'||t.back()==' '))t.pop_back();
    const size_t hr=t.find(" HRESULT=0x");if(hr!=std::string::npos)t.erase(hr);
    if(t.empty())return;
    std::string lo=t;for(char& c:lo)c=(char)tolower((unsigned char)c);
    int lvl=LV_INFO;
    if(lo.find("crash during")!=std::string::npos||lo.find("exception")!=std::string::npos||lo.find("access violation")!=std::string::npos)lvl=LV_ERR;
    else if(lo.find("failed")!=std::string::npos||lo.find("could not")!=std::string::npos||lo.find("unavailable")!=std::string::npos||lo.find("not found")!=std::string::npos||lo.find("missing")!=std::string::npos||lo.find("unexpected")!=std::string::npos||lo.find("timed out")!=std::string::npos)lvl=LV_WARN;
    else if(lo.find("ready")!=std::string::npos||lo.find("installed")!=std::string::npos||lo.find("verified")!=std::string::npos)lvl=LV_OK;
    addEntry(lvl,L"[menu] "+fromUtf8(t),lvl==LV_INFO||lvl==LV_OK,false);   // warnings and errors from the menu show in Normal mode too
}
static void pollDllLog(){
    if(g_root.empty())return;
    const std::wstring path=g_root+L"\\Logs\\ADREENA_debug.log";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&fa)){g_dllOff=0;g_dllInit=true;return;}
    const ULONGLONG size=((ULONGLONG)fa.nFileSizeHigh<<32)|fa.nFileSizeLow;
    if(!g_dllInit){g_dllOff=size;g_dllInit=true;return;}   // only lines written from now on
    if(size<g_dllOff)g_dllOff=0;
    if(size==g_dllOff)return;
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return;
    LARGE_INTEGER pos;pos.QuadPart=(LONGLONG)g_dllOff;SetFilePointerEx(h,pos,nullptr,FILE_BEGIN);
    std::string buf((size_t)(std::min)((ULONGLONG)262144,size-g_dllOff),'\0');DWORD got=0;
    ReadFile(h,&buf[0],(DWORD)buf.size(),&got,nullptr);CloseHandle(h);buf.resize(got);
    const size_t last=buf.rfind('\n');if(last==std::string::npos)return;   // wait for a complete line
    size_t a=0;
    while(a<=last){size_t e=buf.find('\n',a);dllLine(buf.substr(a,e-a));a=e+1;}
    g_dllOff+=last+1;
}

struct Entry{int id;std::wstring rel;bool core;};   // core: always kept in sync; otherwise written only if missing
static bool parseManifest(std::vector<Entry>& out){
    DWORD n=0;const char* m=embedded(101,n);if(!m||!n)return false;
    std::string text(m,n);size_t pos=0;
    while(pos<text.size()){
        size_t e=text.find('\n',pos);if(e==std::string::npos)e=text.size();
        std::string line=text.substr(pos,e-pos);pos=e+1;
        while(!line.empty()&&(line.back()=='\r'||line.back()==' '))line.pop_back();
        if(line.empty())continue;
        const size_t a=line.find('|'),b=line.find('|',a==std::string::npos?0:a+1);
        if(a==std::string::npos||b==std::string::npos)continue;
        Entry en;en.id=atoi(line.substr(0,a).c_str());
        const std::string rel=line.substr(a+1,b-a-1);
        en.rel=std::wstring(rel.begin(),rel.end());
        en.core=line.substr(b+1)=="core";
        out.push_back(en);
    }
    return !out.empty();
}

// ---- import from an older install (ADREENA files sitting beside the loader exe), copy-only and once ----------------
static int copyMissingFolder(const std::wstring& from,const std::wstring& to,bool skipTxt){
    int copied=0;WIN32_FIND_DATAW fd{};
    HANDLE h=FindFirstFileW((from+L"\\*").c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE)return 0;
    do{
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        const std::wstring name=fd.cFileName;
        if(skipTxt&&name.size()>4&&_wcsicmp(name.c_str()+name.size()-4,L".txt")==0)continue;
        if(CopyFileW((from+L"\\"+name).c_str(),(to+L"\\"+name).c_str(),TRUE)){++copied;logLine(L"imported: "+to.substr(g_root.size()+1)+L"\\"+name);}   // TRUE: never overwrite
    }while(FindNextFileW(h,&fd));
    FindClose(h);return copied;
}
static int importLegacy(){
    const std::wstring marker=g_root+L"\\Cache\\import_done.txt";
    if(fileExists(marker))return 0;
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    std::wstring dir=exe;const size_t cut=dir.find_last_of(L'\\');if(cut==std::wstring::npos)return 0;
    dir.resize(cut);
    if(_wcsicmp(dir.c_str(),g_root.c_str())==0)return 0;   // running from inside the ADREENA folder itself
    const bool found=fileExists(dir+L"\\ADREENA.dll")||fileExists(dir+L"\\Profiles")||fileExists(dir+L"\\Banners")||fileExists(dir+L"\\Bookmarks.json");
    if(!found)return 0;
    logLine(L"older install found beside the loader: "+dir);
    int n=0;
    n+=copyMissingFolder(dir+L"\\Profiles",g_root+L"\\Profiles",false);
    n+=copyMissingFolder(dir+L"\\Banners",g_root+L"\\Banners",true);
    n+=copyMissingFolder(dir+L"\\Presets",g_root+L"\\Presets",true);
    if(CopyFileW((dir+L"\\Bookmarks.json").c_str(),(g_root+L"\\Profiles\\Bookmarks.json").c_str(),TRUE)){++n;logLine(L"imported: Profiles\\Bookmarks.json");}
    bool locked=false;const char done[]="import finished\r\n";writeAtomic(marker,done,sizeof(done)-1,locked);
    logLine(L"import finished, "+std::to_wstring(n)+L" file(s) copied (originals left untouched)");
    return n;
}

// ---- startup preparation (worker thread) -----------------------------------------------------------------------
static bool prepare(std::wstring& err){
    setStatus(L"Locating your Documents folder...",20);
    PWSTR docs=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&docs))||!docs){err=L"Could not find your Documents folder.";return false;}
    g_root=std::wstring(docs)+L"\\ADREENA";CoTaskMemFree(docs);
    if(!CreateDirectoryW(g_root.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){err=L"Could not create "+g_root+L" (check folder permissions).";g_root.clear();return false;}
    static const wchar_t* dirs[]={L"Core",L"Logs",L"Profiles",L"Presets",L"Banners",L"Cache"};
    for(const wchar_t* d:dirs){
        const std::wstring p=g_root+L"\\"+d;
        if(!CreateDirectoryW(p.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){err=L"Could not create the "+std::wstring(d)+L" folder.";return false;}
    }
    {   // keep the loader log from growing forever: roll it over at 1 MB
        WIN32_FILE_ATTRIBUTE_DATA fa{};const std::wstring lp=g_root+L"\\Logs\\ADREENA_loader.log";
        if(GetFileAttributesExW(lp.c_str(),GetFileExInfoStandard,&fa)&&(fa.nFileSizeHigh||fa.nFileSizeLow>1024*1024))MoveFileExW(lp.c_str(),(lp+L".old").c_str(),MOVEFILE_REPLACE_EXISTING);
    }
    loadSettings();
    const bool existing=fileExists(g_root+L"\\Core\\"+HSTR("ADREENA.dll"));
    say(LV_INFO,existing?L"Checking your ADREENA installation":L"Preparing ADREENA for first use");
    logLine(L"---- loader "+std::wstring(LOADER_VERSION)+L" start ("+(existing?L"existing installation":L"first run")+L") root="+g_root);
    std::vector<Entry> items;
    if(!parseManifest(items)){err=L"This loader is missing its embedded file list.";logLine(err);return false;}
    setStatus(existing?L"Checking your installation...":L"Preparing ADREENA for first use...",100);
    int idx=0;const int total=(int)items.size();
    for(const Entry& en:items){
        ++idx;
        setStatus((existing?L"Checking ":L"Preparing ")+en.rel,100+(idx*800)/(total+1));
        const std::wstring target=g_root+L"\\"+en.rel;
        const size_t cut=en.rel.find_last_of(L'\\');
        if(cut!=std::wstring::npos)CreateDirectoryW((g_root+L"\\"+en.rel.substr(0,cut)).c_str(),nullptr);
        DWORD n=0;const char* data=embedded(en.id,n);
        if(!data){err=L"Embedded file missing: "+en.rel;logLine(err);return false;}
        if(!en.core){
            if(fileExists(target)){logLine(L"kept (exists): "+en.rel);continue;}
            bool locked=false;
            if(!writeAtomic(target,data,n,locked)){err=L"Could not write "+en.rel;logLine(err);return false;}
            logLine(L"written: "+en.rel);continue;
        }
        std::vector<char> cur;
        if(readWhole(target,cur)&&cur.size()==n&&memcmp(cur.data(),data,n)==0){logLine(L"up to date: "+en.rel);continue;}
        bool locked=false;
        if(writeAtomic(target,data,n,locked)){logLine(L"updated: "+en.rel);continue;}
        if(locked&&existing){
            EnterCriticalSection(&g_cs);g_note=L"Core files are in use (is the game running?). Using the installed version.";LeaveCriticalSection(&g_cs);
            say(LV_WARN,L"ADREENA's core file is in use (is the game running?). Using the installed version.");
            logLine(L"core file in use, kept installed copy: "+en.rel);continue;
        }
        err=L"Could not write "+en.rel+(locked?L" (file in use).":L".");logLine(err);return false;
    }
    say(LV_OK,L"ADREENA files verified");
    setStatus(L"Checking for an older install...",940);
    const int imported=importLegacy();
    if(imported>0){
        EnterCriticalSection(&g_cs);g_note=L"Imported "+std::to_wstring(imported)+L" file(s) from your previous install.";LeaveCriticalSection(&g_cs);
        say(LV_OK,L"Imported "+std::to_wstring(imported)+L" file(s) from your previous install");
    }
    setStatus(L"Checking Steam and the game...",980);
    preflight();
    logLine(L"preparation finished");
    return true;
}
static DWORD WINAPI prepareThread(LPVOID){
    std::wstring err;const bool ok=prepare(err);
    EnterCriticalSection(&g_cs);
    if(ok){g_state=ST_READY;g_status=L"Ready. Click Launch to start State of Decay 2 with ADREENA.";g_prog=1000;}
    else{g_state=ST_FAILED;g_status=err.empty()?L"Preparation failed.":err;}
    LeaveCriticalSection(&g_cs);
    if(ok)say(LV_OK,L"Ready to launch");else say(LV_ERR,err.empty()?L"Preparation failed.":err);
    if(g_hwnd)PostMessageW(g_hwnd,WM_STATE,0,0);
    return 0;
}
static void startPrepare(){
    EnterCriticalSection(&g_cs);g_state=ST_LOADING;g_status=L"Starting...";g_note.clear();g_prog=0;LeaveCriticalSection(&g_cs);
    HANDLE t=CreateThread(nullptr,0,prepareThread,nullptr,0,nullptr);if(t)CloseHandle(t);
}

// ---- launch: the existing Steam start + DLL injection (same sequence and timings as the original launcher) -------
static DWORD findGame(){
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(s==INVALID_HANDLE_VALUE)return 0;
    PROCESSENTRY32W e{};e.dwSize=sizeof(e);DWORD pid=0;
    if(Process32FirstW(s,&e))do{
        if(_wcsicmp(e.szExeFile,HSTR("StateOfDecay2-Win64-Shipping.exe").c_str())==0){pid=e.th32ProcessID;break;}
    }while(Process32NextW(s,&e));
    CloseHandle(s);return pid;
}
static bool moduleLoaded(DWORD pid,const wchar_t* dllName){
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(s==INVALID_HANDLE_VALUE)return false;
    MODULEENTRY32W e{};e.dwSize=sizeof(e);bool found=false;
    if(Module32FirstW(s,&e))do{
        if(_wcsicmp(e.szModule,dllName)==0){found=true;break;}
    }while(Module32NextW(s,&e));
    CloseHandle(s);return found;
}
static std::wstring num(unsigned long v){return std::to_wstring(v);}
static DWORD g_launchedPid=0;   // the game process the last successful launch attached to (watched until it exits)
// Returns true on success; msg is the final status text either way.
static bool launchSequence(std::wstring& msg){
    const std::wstring dll=g_root+L"\\Core\\"+HSTR("ADREENA.dll");
    if(!fileExists(dll)){msg=L"ADREENA.dll is missing from Documents\\ADREENA\\Core. Close and reopen the loader to repair it.";logLine(msg);return false;}
    {   // integrity check: the file on disk must still match what this loader shipped, in case something replaced it
        // while the window sat open (prepare() only verifies this once, at startup)
        std::vector<char> disk;DWORD en=0;const char* emb=embedded(100,en);
        std::vector<BYTE> h1,h2;
        const bool ok=emb&&en&&readWhole(dll,disk)&&sha256(emb,en,h1)&&sha256(disk.data(),disk.size(),h2)&&h1==h2;
        if(!ok){msg=L"ADREENA.dll failed its integrity check (it may have been modified). Close and reopen the loader to repair it.";say(LV_ERR,msg);return false;}
        logLine(L"ADREENA.dll integrity check passed");
    }
    openSession();
    if(!g_session){msg=L"Could not create the launch session (error "+num(GetLastError())+L").";logLine(msg);return false;}
    g_session->gamePid=0;
    DWORD pid=findGame();
    if(pid){
        // ADREENA is only available in a game started by this loader, so a game that is already running is never injected.
        if(moduleLoaded(pid,HSTR("ADREENA.dll").c_str())){msg=L"ADREENA is already loaded. Switch to the game and press PgDn.";logLine(msg);g_launchedPid=pid;return true;}
        msg=L"State of Decay 2 is already running. Close the game, then launch it from here so ADREENA can start with it.";logLine(msg);return false;
    }
    {
        setStatus(L"Starting State of Decay 2 through Steam...",50);say(LV_INFO,L"Starting State of Decay 2 through Steam");
        HINSTANCE launched=ShellExecuteW(nullptr,L"open",HSTR("steam://rungameid/495420").c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        if((INT_PTR)launched<=32){msg=L"Could not start the game through Steam. Is Steam installed and signed in?";logLine(msg);return false;}
        for(int i=0;i<240&&!pid;i++){
            Sleep(1000);pid=findGame();
            if(!pid&&i%3==0)setStatus(L"Waiting for State of Decay 2 to start... ("+num(i+1)+L" s)",50+(i*250)/240);
        }
        if(!pid){msg=L"State of Decay 2 did not start within four minutes.";logLine(msg);return false;}
    }
    logLine(L"game process found, pid "+num(pid));
    g_session->gamePid=pid;   // authorizes exactly this game process
    say(LV_OK,L"State of Decay 2 started");
    setStatus(L"Game found. Letting it finish starting...",300);
    for(int i=0;i<10;i++){Sleep(1000);setStatus(L"Game found. Letting it finish starting...",300+(i+1)*30);}   // Unreal needs time to initialize before the attach
    HANDLE process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!process){msg=L"Could not open the game process (error "+num(GetLastError())+L").";logLine(msg);return false;}
    setStatus(L"Loading ADREENA into the game...",650);say(LV_INFO,L"Loading ADREENA into the game");
    const SIZE_T bytes=(dll.size()+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(process,nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!remote){msg=L"Could not prepare the game process (error "+num(GetLastError())+L").";logLine(msg);CloseHandle(process);return false;}
    SIZE_T written=0;
    if(!WriteProcessMemory(process,remote,dll.c_str(),bytes,&written)||written!=bytes){
        msg=L"Could not write to the game process (error "+num(GetLastError())+L").";logLine(msg);
        VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);return false;
    }
    auto load=(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    if(!load){msg=L"Could not find LoadLibraryW.";logLine(msg);VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);return false;}
    HANDLE thread=CreateRemoteThread(process,nullptr,0,load,remote,0,nullptr);
    if(!thread){msg=L"Could not start the loading thread in the game (error "+num(GetLastError())+L").";logLine(msg);VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);return false;}
    const DWORD wait=WaitForSingleObject(thread,15000);
    bool ok=false;
    if(wait==WAIT_OBJECT_0){
        Sleep(500);   // verify through the module list rather than the truncated x64 thread exit code
        ok=moduleLoaded(pid,HSTR("ADREENA.dll").c_str());
        VirtualFreeEx(process,remote,0,MEM_RELEASE);
        msg=ok?L"ADREENA is loaded. Switch to the game and press PgDn to open the menu.":L"The game did not load ADREENA. See Logs\\ADREENA_loader.log and ADREENA_debug.log.";
    }else msg=L"Loading ADREENA timed out. The game may still be starting - try again in a moment.";   // remote path kept for safety
    logLine(msg);
    CloseHandle(thread);CloseHandle(process);
    if(ok)g_launchedPid=pid;
    return ok;
}
// After a successful launch: wait for that game process to close, then offer Launch again.
static void waitForGameExit(DWORD pid){
    if(!pid)return;
    HANDLE h=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(h){WaitForSingleObject(h,INFINITE);CloseHandle(h);}
    else{while(findGame()==pid)Sleep(2000);}   // fallback if the process can't be opened for waiting
}
static DWORD WINAPI launchThread(LPVOID){
    g_launchedPid=0;
    std::wstring msg;const bool ok=launchSequence(msg);
    say(ok?LV_OK:LV_ERR,msg);
    EnterCriticalSection(&g_cs);g_state=ok?ST_DONE:ST_LAUNCH_FAILED;g_status=msg;g_prog=ok?1000:g_prog;LeaveCriticalSection(&g_cs);
    if(g_hwnd)PostMessageW(g_hwnd,WM_STATE,0,0);
    if(ok&&g_launchedPid){
        waitForGameExit(g_launchedPid);
        if(g_session)g_session->gamePid=0;   // that game is gone; the next launch authorizes a new one
        const std::wstring done=L"State of Decay 2 was closed. Click Launch to play again.";
        logLine(L"game process "+num(g_launchedPid)+L" exited");say(LV_INFO,done);
        g_launchedPid=0;
        EnterCriticalSection(&g_cs);g_state=ST_READY;g_status=done;g_prog=1000;LeaveCriticalSection(&g_cs);
        if(g_hwnd)PostMessageW(g_hwnd,WM_STATE,0,0);
    }
    return 0;
}
static void startLaunch(){
    EnterCriticalSection(&g_cs);g_state=ST_LAUNCHING;g_status=L"Starting...";g_note.clear();g_prog=20;LeaveCriticalSection(&g_cs);
    say(LV_INFO,L"Launch pressed");
    HANDLE t=CreateThread(nullptr,0,launchThread,nullptr,0,nullptr);if(t)CloseHandle(t);
    if(g_hwnd)PostMessageW(g_hwnd,WM_STATE,0,0);
}

// ---- window: borderless, art-backed interface (design space 1672 x 941, scaled to the window) -------------------------
struct RF{float x0,y0,x1,y1;bool has(float x,float y)const{return x>=x0&&x<x1&&y>=y0&&y<y1;}};
enum Hit{H_NONE=0,H_GEAR,H_MIN,H_CLOSE,H_TAB_HOME,H_TAB_LOG,H_LAUNCH,H_CARD_VER,H_CARD_DATE,H_SCROLL,H_SETBLOCK,H_SET1=20,H_LOGROW=40};
static const float DW=1672.f,DH=941.f;
static const RF R_GEAR{1482,10,1522,50},R_MIN{1548,10,1588,50},R_CLOSE{1614,10,1654,50};
static const RF R_TAB_HOME{962,388,1276,456},R_TAB_LOG{1310,388,1634,456};
static const RF R_LAUNCH{964,498,1634,602},R_PROG{964,540,1634,556},R_MSG{964,624,1634,800},R_PANEL{962,486,1634,900};
static const RF R_CARD{36,754,454,916};
static const RF R_SET{1290,56,1654,56+4*56+14};
static const RF R_LOGVIEW{972,560,1612,892},R_TRACK{1616,560,1628,892};
static const RF R_CVER{56,812,446,860},R_CDATE{56,858,446,904};
static int g_tab=0;                      // 0 Home, 1 Change Log
static bool g_setOpen=false;             // settings dropdown (drawn inside the loader)
static float g_logScroll=0.f,g_logContentH=0.f;
static unsigned g_logOpen=1;             // bit i: change-log entry i is expanded (newest starts open)
static bool g_dragScroll=false;static float g_dragGrab=0.f;
static RF g_logRow[8];static int g_logRowIdx[8];static int g_logRowN=0;   // header rects of the last render (design space, already scrolled and clipped)
static RF g_thumb{0,0,0,0};
static int g_hoverId=0,g_downId=0;
static float g_k=1.f;                    // design units -> pixels
static int g_cw=1000,g_ch=563;
static Bitmap* g_bg=nullptr;             // original art
static Bitmap* g_bgScaled=nullptr;       // art scaled to the window
static ULONG_PTR g_gdiToken=0;

static std::wstring buildDate(){          // "Nov 21, 2026" from the compiler's __DATE__ ("Nov 21 2026")
    const char* d=__DATE__;std::string mon(d,3),day(d+4,2),yr(d+7,4);
    if(!day.empty()&&day[0]==' ')day.erase(0,1);
    const std::string t=mon+" "+day+", "+yr;return std::wstring(t.begin(),t.end());
}
static const FontFamily* family(bool serif){
    static FontFamily* ser=nullptr;static FontFamily* sans=nullptr;
    if(!ser){
        ser=new FontFamily(L"Palatino Linotype");if(!ser->IsAvailable()){delete ser;ser=new FontFamily(L"Georgia");}
        if(!ser->IsAvailable()){delete ser;ser=new FontFamily(L"Times New Roman");}
        sans=new FontFamily(L"Segoe UI");if(!sans->IsAvailable()){delete sans;sans=new FontFamily(L"Arial");}
    }
    return serif?ser:sans;
}
static void drawStr(Graphics& g,const std::wstring& t,float size,Color c,const RF& r,int ha=1,int va=1,bool bold=false,bool serif=true,bool wrap=false){
    Font f(family(serif),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
    StringFormat sf;sf.SetAlignment(ha==0?StringAlignmentNear:(ha==1?StringAlignmentCenter:StringAlignmentFar));
    sf.SetLineAlignment(va==0?StringAlignmentNear:(va==1?StringAlignmentCenter:StringAlignmentFar));
    if(!wrap)sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(c);g.DrawString(t.c_str(),-1,&f,RectF(r.x0,r.y0,r.x1-r.x0,r.y1-r.y0),&sf,&b);
}
static float textW(Graphics& g,const std::wstring& t,float size,bool bold=false,bool serif=true){
    Font f(family(serif),size,bold?FontStyleBold:FontStyleRegular,UnitPixel);
    RectF out;g.MeasureString(t.c_str(),-1,&f,PointF(0,0),&out);return out.Width;
}
static void rrPath(GraphicsPath& p,const RF& r,float rad){
    const float d=rad*2;
    p.AddArc(r.x0,r.y0,d,d,180,90);p.AddArc(r.x1-d,r.y0,d,d,270,90);p.AddArc(r.x1-d,r.y1-d,d,d,0,90);p.AddArc(r.x0,r.y1-d,d,d,90,90);p.CloseFigure();
}
static int clampi(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static void crimsonButton(Graphics& g,const RF& r,bool hover,bool down,bool active=true){
    GraphicsPath path;rrPath(path,r,8);
    const int lift=down?-18:(hover?26:0);
    if(active){
        LinearGradientBrush fill(PointF(0,r.y0),PointF(0,r.y1),Color(255,clampi(124+lift,0,255),clampi(12+lift/3,0,255),clampi(24+lift/3,0,255)),Color(255,clampi(58+lift/2,0,255),5,clampi(12+lift/4,0,255)));
        g.FillPath(&fill,&path);
        Pen edge(Color(hover?255:210,200,34,48),2.f);g.DrawPath(&edge,&path);
        // bright top and bottom edge lines, as in the reference
        LinearGradientBrush top(PointF(r.x0,0),PointF(r.x1,0),Color(0,255,70,80),Color(0,255,70,80));
        Color mid[3]={Color(0,255,70,80),Color(220,255,76,86),Color(0,255,70,80)};REAL pos[3]={0.f,0.5f,1.f};
        top.SetInterpolationColors(mid,pos,3);
        Pen tp(&top,2.f);g.DrawLine(&tp,r.x0+14,r.y0+1,r.x1-14,r.y0+1);g.DrawLine(&tp,r.x0+14,r.y1-1,r.x1-14,r.y1-1);
    }else if(hover){
        SolidBrush fill(Color(60,200,34,48));g.FillPath(&fill,&path);
    }
}
static void iconHome(Graphics& g,float cx,float cy,float s,Color c){
    Pen p(c,2.4f);p.SetLineJoin(LineJoinRound);
    PointF roof[3]={PointF(cx-s,cy-s*0.05f),PointF(cx,cy-s*0.95f),PointF(cx+s,cy-s*0.05f)};g.DrawLines(&p,roof,3);
    PointF body[6]={PointF(cx-s*0.72f,cy-s*0.2f),PointF(cx-s*0.72f,cy+s*0.9f),PointF(cx-s*0.2f,cy+s*0.9f),PointF(cx-s*0.2f,cy+s*0.35f),PointF(cx+s*0.2f,cy+s*0.35f),PointF(cx+s*0.2f,cy+s*0.9f)};
    g.DrawLines(&p,body,6);g.DrawLine(&p,cx+s*0.2f,cy+s*0.9f,cx+s*0.72f,cy+s*0.9f);g.DrawLine(&p,cx+s*0.72f,cy+s*0.9f,cx+s*0.72f,cy-s*0.2f);
}
static void iconDoc(Graphics& g,float cx,float cy,float s,Color c){
    Pen p(c,2.4f);p.SetLineJoin(LineJoinRound);
    PointF o[5]={PointF(cx-s*0.7f,cy-s),PointF(cx+s*0.2f,cy-s),PointF(cx+s*0.7f,cy-s*0.5f),PointF(cx+s*0.7f,cy+s),PointF(cx-s*0.7f,cy+s)};
    g.DrawLines(&p,o,5);g.DrawLine(&p,cx-s*0.7f,cy+s,cx-s*0.7f,cy-s);
    g.DrawLine(&p,cx+s*0.2f,cy-s,cx+s*0.2f,cy-s*0.5f);g.DrawLine(&p,cx+s*0.2f,cy-s*0.5f,cx+s*0.7f,cy-s*0.5f);
    for(int i=0;i<3;i++)g.DrawLine(&p,cx-s*0.4f,cy-s*0.1f+i*s*0.42f,cx+s*0.4f,cy-s*0.1f+i*s*0.42f);
}
static void iconGear(Graphics& g,float cx,float cy,float s,Color c){
    Pen p(c,2.6f);p.SetLineJoin(LineJoinRound);
    const double pi=3.14159265358979;GraphicsPath path;PointF pts[32];
    for(int i=0;i<16;i++){   // 8 teeth
        const double a0=i*pi/8.0-pi/16.0;const bool tooth=(i%2==0);
        const float r=tooth?s:s*0.78f;
        pts[i*2]=PointF(cx+(float)cos(a0)*r,cy+(float)sin(a0)*r);pts[i*2+1]=PointF(cx+(float)cos(a0+pi/16.0)*r,cy+(float)sin(a0+pi/16.0)*r);
    }
    g.DrawPolygon(&p,pts,32);g.DrawEllipse(&p,cx-s*0.36f,cy-s*0.36f,s*0.72f,s*0.72f);
}
static void iconCheck(Graphics& g,float cx,float cy,float s,Color c,bool ok){
    Pen p(c,3.f);p.SetLineJoin(LineJoinRound);p.SetStartCap(LineCapRound);p.SetEndCap(LineCapRound);
    g.DrawEllipse(&p,cx-s,cy-s,s*2,s*2);
    if(ok){PointF t[3]={PointF(cx-s*0.45f,cy+s*0.02f),PointF(cx-s*0.1f,cy+s*0.4f),PointF(cx+s*0.5f,cy-s*0.38f)};g.DrawLines(&p,t,3);}
    else{g.DrawLine(&p,cx-s*0.4f,cy-s*0.4f,cx+s*0.4f,cy+s*0.4f);g.DrawLine(&p,cx+s*0.4f,cy-s*0.4f,cx-s*0.4f,cy+s*0.4f);}
}
struct LogEntry{const wchar_t* ver;int y,mo,d,h,mi;const wchar_t* bullets[5];};   // release time in UTC
static const LogEntry LOGS[]={
 {L"v0.1.46",2026,11,21,11,40,{L"Application protection: sensitive names (the DLL, the game process, the Steam link, the loader's session name) are no longer stored as plain text in either program",L"ADREENA.dll is verified against its known-good copy right before it is loaded into the game",L"Release builds no longer include debug symbols",L"The Launch button comes back after the game is closed",nullptr}},
 {L"v0.1.45",2026,11,21,11,5,{L"New debug window (gear menu): color-coded messages with timestamps, Normal and Detailed modes",L"Follows the menu's own debug log, copy all / open log file / clear",L"Steam and game install checks at startup (information only)",L"Loader log rolls over at 1 MB",nullptr}},
 {L"v0.1.44",2026,11,21,10,36,{L"Status card label shortened to \"Updated\"",nullptr,nullptr,nullptr,nullptr}},
 {L"v0.1.43",2026,11,21,10,40,{L"Change Log scrolls and each version opens to show what changed",L"Versions show how long ago they were released",L"Settings menu now opens inside the loader",nullptr,nullptr}},
 {L"v0.1.42",2026,11,21,10,18,{L"New loader interface with the ADREENA artwork",L"Home and Change Log tabs",L"Status messages shown under the Launch button",L"Settings menu with shortcuts to the folders and logs",nullptr}},
 {L"v0.1.41",2026,11,21,9,47,{L"Imports Profiles, Banners and Bookmarks from an older install (once, copy only)",L"Existing files are never overwritten",nullptr,nullptr,nullptr}},
 {L"v0.1.40",2026,11,21,9,31,{L"The menu only loads in a game that was started from this loader",L"A game that is already running is never injected",nullptr,nullptr,nullptr}},
 {L"v0.1.39",2026,11,21,9,19,{L"Launch starts State of Decay 2 through Steam and loads ADREENA",L"Clear messages for every failure",nullptr,nullptr,nullptr}},
 {L"v0.1.38",2026,11,21,8,55,{L"First-run setup creates the Documents\\ADREENA folders",L"All files are embedded in the single loader program",nullptr,nullptr,nullptr}},
 {L"v0.1.37",2026,11,21,8,45,{L"Skill Editor: remove an individual specialization with X",L"Version numbering changed to v0.1.x",nullptr,nullptr,nullptr}},
};
static const int LOGN=(int)(sizeof(LOGS)/sizeof(LOGS[0]));
static ULONGLONG ftNow(){FILETIME f;GetSystemTimeAsFileTime(&f);return ((ULONGLONG)f.dwHighDateTime<<32)|f.dwLowDateTime;}
static ULONGLONG ftOf(int y,int mo,int d,int h,int mi){SYSTEMTIME t{};t.wYear=(WORD)y;t.wMonth=(WORD)mo;t.wDay=(WORD)d;t.wHour=(WORD)h;t.wMinute=(WORD)mi;FILETIME f;SystemTimeToFileTime(&t,&f);return ((ULONGLONG)f.dwHighDateTime<<32)|f.dwLowDateTime;}
static ULONGLONG buildStamp(){   // when this loader was compiled (local time from __DATE__ / __TIME__) as UTC
    static const char* mons="JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* d=__DATE__;const char* t=__TIME__;
    SYSTEMTIME l{};for(int i=0;i<12;i++)if(strncmp(d,mons+i*3,3)==0)l.wMonth=(WORD)(i+1);
    l.wDay=(WORD)atoi(d+4);l.wYear=(WORD)atoi(d+7);l.wHour=(WORD)atoi(t);l.wMinute=(WORD)atoi(t+3);l.wSecond=(WORD)atoi(t+6);
    SYSTEMTIME u{};FILETIME f;if(!TzSpecificLocalTimeToSystemTime(nullptr,&l,&u))u=l;
    SystemTimeToFileTime(&u,&f);return ((ULONGLONG)f.dwHighDateTime<<32)|f.dwLowDateTime;
}
static std::wstring agoText(ULONGLONG then){
    const ULONGLONG now=ftNow();const long long secs=now>then?(long long)((now-then)/10000000ULL):0;
    if(secs<60)return L"just now";
    const long long mins=secs/60,hrs=mins/60,days=hrs/24;
    if(hrs<1)return std::to_wstring(mins)+(mins==1?L" minute ago":L" minutes ago");
    if(days<1)return std::to_wstring(hrs)+(hrs==1?L" hour ago":L" hours ago");
    return std::to_wstring(days)+(days==1?L" day ago":L" days ago");
}
static float textH(Graphics& g,const std::wstring& t,float size,float width,bool serif=false){
    Font f(family(serif),size,FontStyleRegular,UnitPixel);StringFormat sf;RectF out;
    g.MeasureString(t.c_str(),-1,&f,RectF(0,0,width,100000.f),&sf,&out);return out.Height;
}
static void clampScroll(){const float m=(max)(0.f,g_logContentH-(R_LOGVIEW.y1-R_LOGVIEW.y0));g_logScroll=(max)(0.f,(min)(g_logScroll,m));}

static Hit hitTest(float px,float py){
    const float x=px/g_k,y=py/g_k;
    State st;EnterCriticalSection(&g_cs);st=g_state;LeaveCriticalSection(&g_cs);
    if(g_setOpen){
        for(int i=0;i<4;i++)if(RF{R_SET.x0+8,R_SET.y0+8+i*56.f,R_SET.x1-8,R_SET.y0+8+(i+1)*56.f}.has(x,y))return (Hit)(H_SET1+i);
        if(R_GEAR.has(x,y))return H_GEAR;
        return H_SETBLOCK;   // any other click only closes the dropdown
    }
    if(R_GEAR.has(x,y))return H_GEAR;if(R_MIN.has(x,y))return H_MIN;if(R_CLOSE.has(x,y))return H_CLOSE;
    if(R_CVER.has(x,y))return H_CARD_VER;if(R_CDATE.has(x,y))return H_CARD_DATE;
    if(R_TAB_HOME.has(x,y))return H_TAB_HOME;if(R_TAB_LOG.has(x,y))return H_TAB_LOG;
    if(g_tab==1){
        if(g_thumb.y1>g_thumb.y0&&R_TRACK.has(x,y))return H_SCROLL;
        if(R_LOGVIEW.has(x,y))for(int i=0;i<g_logRowN;i++)if(g_logRow[i].has(x,y))return (Hit)(H_LOGROW+i);
    }
    if(g_tab==0&&(st==ST_READY||st==ST_FAILED||st==ST_LAUNCH_FAILED)&&R_LAUNCH.has(x,y))return H_LAUNCH;
    return H_NONE;
}
static void render(Graphics& g){
    State st;std::wstring status,note;int prog;
    EnterCriticalSection(&g_cs);st=g_state;status=g_status;note=g_note;prog=g_prog;LeaveCriticalSection(&g_cs);
    g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHalf);
    if(g_bgScaled)g.DrawImage(g_bgScaled,0,0);
    g.ScaleTransform(g_k,g_k);
    const bool bad=(st==ST_FAILED||st==ST_LAUNCH_FAILED);
    const Color white(255,236,232,240),soft(255,205,198,212),green(255,70,225,100),red(255,240,100,100),amber(255,232,184,84);

    // top-right window controls
    const Color ic(255,226,222,230),icHot(255,255,255,255);
    iconGear(g,(R_GEAR.x0+R_GEAR.x1)/2,(R_GEAR.y0+R_GEAR.y1)/2,12,g_hoverId==H_GEAR?icHot:ic);
    {Pen p(g_hoverId==H_MIN?icHot:ic,2.6f);const float cx=(R_MIN.x0+R_MIN.x1)/2,cy=(R_MIN.y0+R_MIN.y1)/2+2;g.DrawLine(&p,cx-11,cy,cx+11,cy);}
    {Pen p(g_hoverId==H_CLOSE?Color(255,255,110,110):ic,2.6f);const float cx=(R_CLOSE.x0+R_CLOSE.x1)/2,cy=(R_CLOSE.y0+R_CLOSE.y1)/2;g.DrawLine(&p,cx-10,cy-10,cx+10,cy+10);g.DrawLine(&p,cx+10,cy-10,cx-10,cy+10);}

    // tabs
    for(int t=0;t<2;t++){
        const RF& r=t==0?R_TAB_HOME:R_TAB_LOG;const Hit id=t==0?H_TAB_HOME:H_TAB_LOG;
        const bool active=g_tab==t;
        crimsonButton(g,r,g_hoverId==id,false,active);
        const std::wstring label=t==0?L"Home":L"Change Log";
        const float tw=textW(g,label,34),total=40+14+tw,sx=(r.x0+r.x1)/2-total/2,cy=(r.y0+r.y1)/2;
        const Color c=active?white:soft;
        if(t==0)iconHome(g,sx+20,cy,17,c);else iconDoc(g,sx+20,cy,17,c);
        drawStr(g,label,34,c,RF{sx+54,r.y0,sx+54+tw+30,r.y1},0,1);
    }

    if(g_tab==0){
        // Launch button (or progress while the loader is busy)
        if(st==ST_READY||st==ST_FAILED||st==ST_LAUNCH_FAILED){
            const bool hot=g_hoverId==H_LAUNCH,down=g_downId==H_LAUNCH&&hot;
            crimsonButton(g,R_LAUNCH,hot,down,true);
            const std::wstring label=st==ST_READY?L"Launch":(st==ST_LAUNCH_FAILED?L"Try Again":L"Retry");
            const float tw=textW(g,label,56),total=st==ST_READY?(46+22+tw):tw,sx=(R_LAUNCH.x0+R_LAUNCH.x1)/2-total/2,cy=(R_LAUNCH.y0+R_LAUNCH.y1)/2;
            if(st==ST_READY){SolidBrush tb(Color(255,232,226,236));PointF tri[3]={PointF(sx,cy-23),PointF(sx+40,cy),PointF(sx,cy+23)};g.FillPolygon(&tb,tri,3);}
            drawStr(g,label,56,white,RF{sx+(st==ST_READY?62.f:0.f),R_LAUNCH.y0,sx+total+40,R_LAUNCH.y1},0,1);
        }else if(st==ST_LOADING||st==ST_LAUNCHING){
            GraphicsPath tr;rrPath(tr,R_PROG,8);SolidBrush tb(Color(255,34,12,16));g.FillPath(&tb,&tr);
            const float w=(R_PROG.x1-R_PROG.x0)*(min)(prog,1000)/1000.f;
            if(w>6){GraphicsPath fp;rrPath(fp,RF{R_PROG.x0,R_PROG.y0,R_PROG.x0+w,R_PROG.y1},8);
                LinearGradientBrush fb(PointF(R_PROG.x0,0),PointF(R_PROG.x1,0),Color(255,130,14,28),Color(255,236,60,72));g.FillPath(&fb,&fp);}
            Pen bp(Color(120,200,34,48),1.5f);g.DrawPath(&bp,&tr);
        }
        // status messages go under the Launch button
        const Color mc=bad?red:((st==ST_READY||st==ST_DONE)?green:soft);
        drawStr(g,status,29,mc,R_MSG,1,0,false,false,true);
        if(!note.empty())drawStr(g,note,24,amber,RF{R_MSG.x0,R_MSG.y0+90,R_MSG.x1,R_MSG.y1+40},1,0,false,false,true);
    }else{
        // Change Log panel: scrollable, each version expands to its bullet list
        GraphicsPath pp;rrPath(pp,R_PANEL,10);SolidBrush pb(Color(150,8,6,10));g.FillPath(&pb,&pp);Pen pe(Color(90,200,34,48),1.5f);g.DrawPath(&pe,&pp);
        drawStr(g,L"Change Log",38,white,RF{R_PANEL.x0+30,R_PANEL.y0+14,R_PANEL.x1-20,R_PANEL.y0+68},0,1);
        const float vx=R_LOGVIEW.x0,vw=R_LOGVIEW.x1-R_LOGVIEW.x0,vh=R_LOGVIEW.y1-R_LOGVIEW.y0;
        // lay out
        float yy=0;g_logRowN=0;
        struct Item{int idx;float y,h;};Item items[16];int n=0;
        for(int i=0;i<LOGN&&n<16;i++){
            float h=60;
            if(g_logOpen&(1u<<i)){h+=6;for(int b=0;b<5&&LOGS[i].bullets[b];b++)h+=textH(g,LOGS[i].bullets[b],23,vw-96)+8;h+=10;}
            items[n++]=Item{i,yy,h};yy+=h+6;
        }
        g_logContentH=yy;clampScroll();
        RectF clip(R_LOGVIEW.x0-4,R_LOGVIEW.y0,vw+8,vh);g.SetClip(clip);
        for(int k=0;k<n;k++){
            const int i=items[k].idx;const float top=R_LOGVIEW.y0+items[k].y-g_logScroll,h=items[k].h;
            if(top+h<R_LOGVIEW.y0||top>R_LOGVIEW.y1)continue;
            const bool open=(g_logOpen&(1u<<i))!=0,hot=g_hoverId==H_LOGROW+g_logRowN;
            RF hdr{vx,top,vx+vw,top+60};
            if(g_logRowN<8){g_logRow[g_logRowN]=RF{hdr.x0,(max)(hdr.y0,R_LOGVIEW.y0),hdr.x1,(min)(hdr.y1,R_LOGVIEW.y1)};g_logRowIdx[g_logRowN]=i;}
            // card behind the entry
            GraphicsPath ep;rrPath(ep,RF{vx,top,vx+vw,top+h},8);SolidBrush eb(Color(hot?70:40,200,34,48));g.FillPath(&eb,&ep);
            // chevron
            {SolidBrush cb(Color(255,236,70,84));const float cx=vx+26,cy=top+30;
             if(open){PointF t[3]={PointF(cx-9,cy-5),PointF(cx+9,cy-5),PointF(cx,cy+8)};g.FillPolygon(&cb,t,3);}
             else{PointF t[3]={PointF(cx-5,cy-9),PointF(cx-5,cy+9),PointF(cx+8,cy)};g.FillPolygon(&cb,t,3);}}
            drawStr(g,LOGS[i].ver,30,Color(255,240,236,244),RF{vx+54,top,vx+260,top+60},0,1,true,true);
            drawStr(g,agoText(ftOf(LOGS[i].y,LOGS[i].mo,LOGS[i].d,LOGS[i].h,LOGS[i].mi)),23,soft,RF{vx+260,top,vx+vw-18,top+60},2,1,false,false);
            if(open){
                float by=top+64;
                for(int b=0;b<5&&LOGS[i].bullets[b];b++){
                    const float bh=textH(g,LOGS[i].bullets[b],23,vw-96);
                    SolidBrush dot(Color(255,236,70,84));g.FillEllipse(&dot,vx+58.f,by+10.f,8.f,8.f);
                    drawStr(g,LOGS[i].bullets[b],23,soft,RF{vx+78,by,vx+vw-18,by+bh+4},0,0,false,false,true);
                    by+=bh+8;
                }
            }
            if(g_logRowN<8)++g_logRowN;
        }
        g.ResetClip();
        // scroll bar
        g_thumb=RF{0,0,0,0};
        if(g_logContentH>vh+1){
            const float th=(max)(48.f,vh*vh/g_logContentH),range=vh-th,ty=R_TRACK.y0+(g_logScroll/(g_logContentH-vh))*range;
            GraphicsPath tp;rrPath(tp,R_TRACK,5);SolidBrush tb(Color(110,30,10,14));g.FillPath(&tb,&tp);
            g_thumb=RF{R_TRACK.x0,ty,R_TRACK.x1,ty+th};GraphicsPath th2;rrPath(th2,g_thumb,5);
            SolidBrush thb(Color(g_hoverId==H_SCROLL||g_dragScroll?255:200,200,34,48));g.FillPath(&thb,&th2);
        }
    }

    // status card (bottom left)
    {
        GraphicsPath cp;rrPath(cp,R_CARD,6);SolidBrush cb(Color(240,9,7,11));g.FillPath(&cb,&cp);
        Pen ce(Color(70,255,255,255),1.2f);g.DrawPath(&ce,&cp);
        SolidBrush bar(Color(255,226,20,32));g.FillRectangle(&bar,R_CARD.x0+3,R_CARD.y0+12,4.f,R_CARD.y1-R_CARD.y0-24);
        const float rows[3]={792,838,881};const Color lab(255,226,222,230);
        const wchar_t* labels[3]={L"Status:",L"Version:",L"Updated:"};
        for(int i=0;i<3;i++)drawStr(g,labels[i],27,lab,RF{R_CARD.x0+26,rows[i]-20,R_CARD.x0+230,rows[i]+20},0,1);
        const wchar_t* sv=L"Preparing";Color sc=amber;bool ok=false,showIcon=false;
        if(st==ST_READY){sv=L"Ready";sc=green;ok=true;showIcon=true;}
        else if(st==ST_DONE){sv=L"Running";sc=green;ok=true;showIcon=true;}
        else if(st==ST_LAUNCHING){sv=L"Launching";}
        else if(bad){sv=L"Error";sc=red;showIcon=true;}
        if(showIcon)iconCheck(g,241,rows[0],17,sc,ok);
        drawStr(g,sv,27,sc,RF{showIcon?272.f:226.f,rows[0]-20,440,rows[0]+20},0,1);
        const Color hv(255,255,255,255);
        drawStr(g,LOADER_VERSION,27,g_hoverId==H_CARD_VER?hv:lab,RF{240,rows[1]-20,440,rows[1]+20},0,1);
        drawStr(g,agoText(buildStamp()),27,g_hoverId==H_CARD_DATE?hv:lab,RF{240,rows[2]-20,440,rows[2]+20},0,1);
        if(g_hoverId==H_CARD_VER||g_hoverId==H_CARD_DATE){Pen up(Color(160,236,70,84),1.5f);const float yy2=rows[g_hoverId==H_CARD_VER?1:2]+19;g.DrawLine(&up,240.f,yy2,330.f,yy2);}
    }
    if(g_setOpen){
        GraphicsPath sp;rrPath(sp,R_SET,10);SolidBrush sb(Color(246,12,9,14));g.FillPath(&sb,&sp);Pen se(Color(200,200,34,48),1.6f);g.DrawPath(&se,&sp);
        const wchar_t* items[4]={L"Open debug window",L"Open ADREENA folder",L"Open logs folder",L"Open loader log"};
        for(int i=0;i<4;i++){
            const RF row{R_SET.x0+8,R_SET.y0+8+i*56.f,R_SET.x1-8,R_SET.y0+8+(i+1)*56.f};
            if(g_hoverId==H_SET1+i){GraphicsPath rp;rrPath(rp,row,6);SolidBrush hb(Color(90,200,34,48));g.FillPath(&hb,&rp);}
            drawStr(g,items[i],26,white,RF{row.x0+18,row.y0,row.x1-8,row.y1},0,1,false,false);
        }
    }
    g.ResetTransform();
    Pen bd(Color(140,150,20,32),2.f);g.DrawRectangle(&bd,1.f,1.f,(REAL)g_cw-2,(REAL)g_ch-2);
}
static void paint(HWND hwnd){
    PAINTSTRUCT ps;HDC wdc=BeginPaint(hwnd,&ps);
    {Bitmap back(g_cw,g_ch,PixelFormat32bppPARGB);Graphics g(&back);render(g);Graphics gw(wdc);gw.DrawImage(&back,0,0);}
    EndPaint(hwnd,&ps);
}
static void openPath(const std::wstring& p){ShellExecuteW(nullptr,L"open",p.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}
static void openDebugWindow();
static void onClick(HWND h,int id){
    State st;EnterCriticalSection(&g_cs);st=g_state;LeaveCriticalSection(&g_cs);
    switch(id){
    case H_GEAR:g_setOpen=!g_setOpen;break;
    case H_SETBLOCK:g_setOpen=false;break;
    case H_SET1:case H_SET1+1:case H_SET1+2:case H_SET1+3:
        g_setOpen=false;
        if(id==H_SET1)openDebugWindow();
        else if(!g_root.empty()){if(id==H_SET1+1)openPath(g_root);else if(id==H_SET1+2)openPath(g_root+L"\\Logs");else openPath(g_root+L"\\Logs\\ADREENA_loader.log");}
        break;
    case H_CARD_VER:case H_CARD_DATE:g_tab=1;g_logOpen|=1u;g_logScroll=0;break;
    case H_SCROLL:break;
    case H_MIN:ShowWindow(h,SW_MINIMIZE);break;
    case H_CLOSE:DestroyWindow(h);break;
    case H_TAB_HOME:g_tab=0;break;
    case H_TAB_LOG:g_tab=1;break;
    default:
        if(id>=H_LOGROW&&id<H_LOGROW+8&&id-H_LOGROW<g_logRowN)g_logOpen^=(1u<<g_logRowIdx[id-H_LOGROW]);   // toggle the entry whose header was clicked
        break;
    case H_LAUNCH:
        if(st==ST_FAILED)startPrepare();
        else if(st==ST_READY||st==ST_LAUNCH_FAILED)startLaunch();
        break;
    }
    InvalidateRect(h,nullptr,FALSE);
}
static LRESULT CALLBACK wndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_STATE:InvalidateRect(h,nullptr,FALSE);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:paint(h);return 0;
    case WM_NCHITTEST:{
        POINT pt{(int)(short)LOWORD(l),(int)(short)HIWORD(l)};ScreenToClient(h,&pt);
        return hitTest((float)pt.x,(float)pt.y)==H_NONE?HTCAPTION:HTCLIENT;}   // drag the window by its art
    case WM_MOUSEWHEEL:{
        if(g_tab==1&&!g_setOpen){g_logScroll-=(float)(short)HIWORD(w)/120.f*70.f;clampScroll();InvalidateRect(h,nullptr,FALSE);}
        return 0;}
    case WM_TIMER:if(w==2)pollDllLog();else InvalidateRect(h,nullptr,FALSE);return 0;   // keeps the "x hours ago" text current
    case WM_KEYDOWN:if(w==VK_ESCAPE&&g_setOpen){g_setOpen=false;InvalidateRect(h,nullptr,FALSE);}return 0;
    case WM_MOUSEMOVE:{
        if(g_dragScroll){
            const float th=g_thumb.y1-g_thumb.y0,range=(R_TRACK.y1-R_TRACK.y0)-th;
            if(range>1){g_logScroll=(((float)(short)HIWORD(l))/g_k-g_dragGrab-R_TRACK.y0)/range*(g_logContentH-(R_LOGVIEW.y1-R_LOGVIEW.y0));clampScroll();InvalidateRect(h,nullptr,FALSE);}
            return 0;
        }
        const int id=hitTest((float)(short)LOWORD(l),(float)(short)HIWORD(l));
        if(id!=g_hoverId){g_hoverId=id;InvalidateRect(h,nullptr,FALSE);}
        TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);return 0;}
    case WM_MOUSELEAVE:if(g_hoverId){g_hoverId=0;InvalidateRect(h,nullptr,FALSE);}return 0;
    case WM_NCMOUSEMOVE:if(g_hoverId){g_hoverId=0;InvalidateRect(h,nullptr,FALSE);}break;
    case WM_SETCURSOR:
        if(LOWORD(l)==HTCLIENT&&g_hoverId&&g_hoverId!=H_SETBLOCK){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        break;
    case WM_LBUTTONDOWN:{
        g_downId=hitTest((float)(short)LOWORD(l),(float)(short)HIWORD(l));
        if(g_downId==H_SCROLL){   // grab the thumb, or jump the thumb to the clicked spot
            const float my=(float)(short)HIWORD(l)/g_k,th=g_thumb.y1-g_thumb.y0;
            g_dragGrab=(my>=g_thumb.y0&&my<g_thumb.y1)?my-g_thumb.y0:th/2;g_dragScroll=true;g_downId=0;SetCapture(h);
            const float range=(R_TRACK.y1-R_TRACK.y0)-th;
            if(range>1){g_logScroll=(my-g_dragGrab-R_TRACK.y0)/range*(g_logContentH-(R_LOGVIEW.y1-R_LOGVIEW.y0));clampScroll();}
            InvalidateRect(h,nullptr,FALSE);return 0;
        }
        if(g_downId){SetCapture(h);InvalidateRect(h,nullptr,FALSE);}
        return 0;}
    case WM_LBUTTONUP:{
        if(g_dragScroll){g_dragScroll=false;if(GetCapture()==h)ReleaseCapture();InvalidateRect(h,nullptr,FALSE);return 0;}
        const int id=hitTest((float)(short)LOWORD(l),(float)(short)HIWORD(l));const int was=g_downId;g_downId=0;
        if(GetCapture()==h)ReleaseCapture();
        if(was&&was==id)onClick(h,id);else InvalidateRect(h,nullptr,FALSE);
        return 0;}
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
// ---- debug window ------------------------------------------------------------------------------------------------
enum DHit{D_NONE=0,D_MIN,D_CLOSE,D_NORMAL,D_DETAIL,D_COPY,D_OPENLOG,D_CLEAR,D_SCROLL,D_LATEST};
static const float DBW=920.f,DBH=600.f,DROW=26.f;
static const RF D_RMIN{820,12,856,44},D_RCLOSE{862,12,898,44};
static const RF D_RNORMAL{24,64,144,100},D_RDETAIL{144,64,264,100};
static const RF D_RCOPY{536,64,656,100},D_ROPEN{664,64,784,100},D_RCLEAR{792,64,896,100};
static const RF D_RLOG{24,112,880,578},D_RTRACK{884,112,896,578},D_RLATEST{690,536,872,568};
static int g_dHover=0,g_dDown=0,g_dW=920,g_dH=600;
static float g_dK=1.f,g_dScroll=0.f,g_dGrab=0.f,g_dMax=0.f;
static bool g_dFollow=true,g_dDrag=false;
static RF g_dThumb{0,0,0,0};

static void smallButton(Graphics& g,const RF& r,const std::wstring& label,bool hot){
    GraphicsPath p;rrPath(p,r,6);SolidBrush f(Color(hot?255:235,hot?70:40,hot?22:16,hot?32:24));g.FillPath(&f,&p);
    Pen e(Color(hot?255:170,200,34,48),1.5f);g.DrawPath(&e,&p);
    drawStr(g,label,18,Color(255,236,232,240),r,1,1,false,false);
}
static Color levelColor(const DbgEntry& e){
    if(e.lvl==LV_ERR)return Color(255,245,100,100);
    if(e.lvl==LV_WARN)return Color(255,240,190,80);
    if(e.lvl==LV_OK)return Color(255,90,225,120);
    return e.detail?Color(255,150,160,190):Color(255,170,190,235);
}
static void dbgRender(Graphics& g){
    g.Clear(Color(255,14,10,16));
    g.SetSmoothingMode(SmoothingModeAntiAlias);g.SetTextRenderingHint(TextRenderingHintAntiAlias);g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(g_dK,g_dK);
    const Color white(255,236,232,240),soft(255,205,198,212),muted(255,130,124,140);
    {SolidBrush bar(Color(255,190,40,60));g.FillRectangle(&bar,0.f,0.f,DBW,3.f);}
    drawStr(g,L"Debug Log",30,white,RF{24,6,400,58},0,1,false,true);
    {Pen p(g_dHover==D_MIN?Color(255,255,255,255):Color(255,226,222,230),2.6f);const float cx=(D_RMIN.x0+D_RMIN.x1)/2,cy=(D_RMIN.y0+D_RMIN.y1)/2+3;g.DrawLine(&p,cx-9,cy,cx+9,cy);}
    {Pen p(g_dHover==D_CLOSE?Color(255,255,110,110):Color(255,226,222,230),2.6f);const float cx=(D_RCLOSE.x0+D_RCLOSE.x1)/2,cy=(D_RCLOSE.y0+D_RCLOSE.y1)/2;g.DrawLine(&p,cx-9,cy-9,cx+9,cy+9);g.DrawLine(&p,cx+9,cy-9,cx-9,cy+9);}
    // Normal / Detailed switch
    for(int i=0;i<2;i++){
        const RF& r=i==0?D_RNORMAL:D_RDETAIL;const bool active=(i==1)==g_detailMode;
        crimsonButton(g,r,g_dHover==(i==0?D_NORMAL:D_DETAIL),false,active);
        drawStr(g,i==0?L"Normal":L"Detailed",19,active?white:soft,r,1,1,false,false);
    }
    smallButton(g,D_RCOPY,L"Copy all",g_dHover==D_COPY);smallButton(g,D_ROPEN,L"Open log file",g_dHover==D_OPENLOG);smallButton(g,D_RCLEAR,L"Clear",g_dHover==D_CLEAR);
    // log panel
    {GraphicsPath pp;rrPath(pp,D_RLOG,8);SolidBrush pb(Color(255,9,7,11));g.FillPath(&pb,&pp);Pen pe(Color(110,200,34,48),1.2f);g.DrawPath(&pe,&pp);}
    const float viewH=D_RLOG.y1-D_RLOG.y0-8,visRows=viewH/DROW;
    std::vector<DbgEntry> rows;int count=0;float first=0;
    EnterCriticalSection(&g_dbgCs);
    {
        std::vector<int> idx;
        for(size_t i=0;i<g_dbg.size();++i)if(g_detailMode||!g_dbg[i].detail)idx.push_back((int)i);
        count=(int)idx.size();
        g_dMax=(max)(0.f,count-visRows);
        if(g_dFollow)g_dScroll=g_dMax;
        g_dScroll=(max)(0.f,(min)(g_dScroll,g_dMax));
        first=g_dScroll;
        const int start=(int)first;
        for(int k=start;k<count&&k<start+(int)visRows+3;++k)rows.push_back(g_dbg[idx[k]]);
    }
    LeaveCriticalSection(&g_dbgCs);
    g.SetClip(RectF(D_RLOG.x0,D_RLOG.y0+2,D_RLOG.x1-D_RLOG.x0,D_RLOG.y1-D_RLOG.y0-4));
    if(count==0)drawStr(g,L"No messages yet.",18,muted,RF{D_RLOG.x0+16,D_RLOG.y0+10,D_RLOG.x1-16,D_RLOG.y0+40},0,1,false,false);
    const int start=(int)first;
    for(size_t r=0;r<rows.size();++r){
        const DbgEntry& e=rows[r];
        const float y=D_RLOG.y0+4+((float)(start+(int)r)-first)*DROW;
        if(((start+(int)r)&1)==0){SolidBrush alt(Color(14,255,255,255));g.FillRectangle(&alt,D_RLOG.x0+2,y,D_RLOG.x1-D_RLOG.x0-4,DROW);}
        wchar_t ts[16];swprintf(ts,16,L"%02d:%02d:%02d",e.t.wHour,e.t.wMinute,e.t.wSecond);
        drawStr(g,ts,15,muted,RF{D_RLOG.x0+12,y,D_RLOG.x0+92,y+DROW},0,1,false,false);
        drawStr(g,tagOf(e),13,levelColor(e),RF{D_RLOG.x0+98,y,D_RLOG.x0+172,y+DROW},0,1,true,false);
        const Color mc=(e.lvl==LV_INFO)?(e.detail?Color(255,150,146,160):soft):levelColor(e);
        drawStr(g,e.text,16,mc,RF{D_RLOG.x0+178,y,D_RLOG.x1-14,y+DROW},0,1,false,false);
    }
    g.ResetClip();
    // scroll bar
    g_dThumb=RF{0,0,0,0};
    if(count>visRows+0.01f){
        const float trackH=D_RTRACK.y1-D_RTRACK.y0,th=(max)(40.f,trackH*visRows/count),range=trackH-th;
        const float ty=D_RTRACK.y0+(g_dMax>0?g_dScroll/g_dMax:0.f)*range;
        GraphicsPath tp;rrPath(tp,D_RTRACK,5);SolidBrush tb(Color(110,30,10,14));g.FillPath(&tb,&tp);
        g_dThumb=RF{D_RTRACK.x0,ty,D_RTRACK.x1,ty+th};GraphicsPath th2;rrPath(th2,g_dThumb,5);
        SolidBrush thb(Color(g_dHover==D_SCROLL||g_dDrag?255:200,200,34,48));g.FillPath(&thb,&th2);
    }
    if(!g_dFollow&&count>0)smallButton(g,D_RLATEST,L"Jump to latest",g_dHover==D_LATEST);
    g.ResetTransform();
    Pen bd(Color(140,150,20,32),2.f);g.DrawRectangle(&bd,1.f,1.f,(REAL)g_dW-2,(REAL)g_dH-2);
}
static int dbgHit(float px,float py){
    const float x=px/g_dK,y=py/g_dK;
    if(D_RMIN.has(x,y))return D_MIN;if(D_RCLOSE.has(x,y))return D_CLOSE;
    if(D_RNORMAL.has(x,y))return D_NORMAL;if(D_RDETAIL.has(x,y))return D_DETAIL;
    if(D_RCOPY.has(x,y))return D_COPY;if(D_ROPEN.has(x,y))return D_OPENLOG;if(D_RCLEAR.has(x,y))return D_CLEAR;
    if(!g_dFollow&&D_RLATEST.has(x,y))return D_LATEST;
    if(g_dThumb.y1>g_dThumb.y0&&D_RTRACK.has(x,y))return D_SCROLL;
    return D_NONE;
}
static void copyAllLog(HWND h){
    std::wstring out;
    EnterCriticalSection(&g_dbgCs);
    for(const DbgEntry& e:g_dbg){
        if(!g_detailMode&&e.detail)continue;
        wchar_t ts[40];swprintf(ts,40,L"[%02d:%02d:%02d] ",e.t.wHour,e.t.wMinute,e.t.wSecond);
        std::wstring tag=tagOf(e);while(tag.size()<8)tag+=L' ';
        out+=ts;out+=tag;out+=e.text;out+=L"\r\n";
    }
    LeaveCriticalSection(&g_dbgCs);
    if(!OpenClipboard(h))return;
    EmptyClipboard();
    HGLOBAL m=GlobalAlloc(GMEM_MOVEABLE,(out.size()+1)*sizeof(wchar_t));
    if(m){wchar_t* p=(wchar_t*)GlobalLock(m);if(p){memcpy(p,out.c_str(),(out.size()+1)*sizeof(wchar_t));GlobalUnlock(m);if(!SetClipboardData(CF_UNICODETEXT,m))GlobalFree(m);}else GlobalFree(m);}
    CloseClipboard();
}
static void dbgClick(HWND h,int id){
    switch(id){
    case D_MIN:ShowWindow(h,SW_MINIMIZE);break;
    case D_CLOSE:DestroyWindow(h);return;
    case D_NORMAL:case D_DETAIL:g_detailMode=(id==D_DETAIL);saveSettings();g_dFollow=true;break;
    case D_COPY:copyAllLog(h);break;
    case D_OPENLOG:if(!g_root.empty())openPath(g_root+L"\\Logs\\ADREENA_loader.log");break;
    case D_CLEAR:EnterCriticalSection(&g_dbgCs);flushLogLocked();g_dbg.clear();g_dbgWritten=0;LeaveCriticalSection(&g_dbgCs);g_dScroll=0;g_dFollow=true;break;
    case D_LATEST:g_dFollow=true;break;
    }
    InvalidateRect(h,nullptr,FALSE);
}
static void dbgSetScrollFromY(float my){
    const float th=g_dThumb.y1-g_dThumb.y0,range=(D_RTRACK.y1-D_RTRACK.y0)-th;
    if(range<=1)return;
    g_dScroll=(my/g_dK-g_dGrab-D_RTRACK.y0)/range*g_dMax;
    g_dScroll=(max)(0.f,(min)(g_dScroll,g_dMax));
    g_dFollow=g_dScroll>=g_dMax-0.01f;
}
static LRESULT CALLBACK dbgProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_DBGLOG:InvalidateRect(h,nullptr,FALSE);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps;HDC wdc=BeginPaint(h,&ps);
        {Bitmap back(g_dW,g_dH,PixelFormat32bppPARGB);Graphics g(&back);dbgRender(g);Graphics gw(wdc);gw.DrawImage(&back,0,0);}
        EndPaint(h,&ps);return 0;}
    case WM_NCHITTEST:{
        POINT pt{(int)(short)LOWORD(l),(int)(short)HIWORD(l)};ScreenToClient(h,&pt);
        return dbgHit((float)pt.x,(float)pt.y)==D_NONE?HTCAPTION:HTCLIENT;}
    case WM_MOUSEMOVE:{
        if(g_dDrag){dbgSetScrollFromY((float)(short)HIWORD(l));InvalidateRect(h,nullptr,FALSE);return 0;}
        const int id=dbgHit((float)(short)LOWORD(l),(float)(short)HIWORD(l));
        if(id!=g_dHover){g_dHover=id;InvalidateRect(h,nullptr,FALSE);}
        TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);return 0;}
    case WM_MOUSELEAVE:if(g_dHover){g_dHover=0;InvalidateRect(h,nullptr,FALSE);}return 0;
    case WM_NCMOUSEMOVE:if(g_dHover){g_dHover=0;InvalidateRect(h,nullptr,FALSE);}break;
    case WM_SETCURSOR:
        if(LOWORD(l)==HTCLIENT&&g_dHover){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        break;
    case WM_MOUSEWHEEL:
        g_dScroll-=(float)(short)HIWORD(w)/120.f*3.f;g_dScroll=(max)(0.f,(min)(g_dScroll,g_dMax));
        g_dFollow=g_dScroll>=g_dMax-0.01f;InvalidateRect(h,nullptr,FALSE);return 0;
    case WM_LBUTTONDOWN:{
        const int id=dbgHit((float)(short)LOWORD(l),(float)(short)HIWORD(l));
        if(id==D_SCROLL){
            const float my=(float)(short)HIWORD(l)/g_dK;
            g_dGrab=(my>=g_dThumb.y0&&my<g_dThumb.y1)?my-g_dThumb.y0:(g_dThumb.y1-g_dThumb.y0)/2;
            g_dDrag=true;SetCapture(h);dbgSetScrollFromY((float)(short)HIWORD(l));InvalidateRect(h,nullptr,FALSE);return 0;
        }
        g_dDown=id;if(id){SetCapture(h);InvalidateRect(h,nullptr,FALSE);}
        return 0;}
    case WM_LBUTTONUP:{
        if(g_dDrag){g_dDrag=false;if(GetCapture()==h)ReleaseCapture();InvalidateRect(h,nullptr,FALSE);return 0;}
        const int id=dbgHit((float)(short)LOWORD(l),(float)(short)HIWORD(l));const int was=g_dDown;g_dDown=0;
        if(GetCapture()==h)ReleaseCapture();
        if(was&&was==id)dbgClick(h,id);else InvalidateRect(h,nullptr,FALSE);
        return 0;}
    case WM_DESTROY:g_dbgHwnd=nullptr;return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
static void openDebugWindow(){
    if(g_dbgHwnd){ShowWindow(g_dbgHwnd,SW_RESTORE);SetForegroundWindow(g_dbgHwnd);return;}
    RECT wa{0,0,1280,720};SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    int w=(int)(DBW*g_scale+0.5);const int maxW=(int)((wa.right-wa.left)*0.92),maxH=(int)((wa.bottom-wa.top)*0.92*DBW/DBH);
    w=(min)(w,(min)(maxW,maxH));
    g_dW=w;g_dH=(int)(w*DBH/DBW+0.5f);g_dK=g_dW/DBW;g_dFollow=true;g_dHover=0;g_dDown=0;g_dDrag=false;
    int x=wa.left+((wa.right-wa.left)-g_dW)/2+S(40),y=wa.top+((wa.bottom-wa.top)-g_dH)/2+S(40);
    if(x+g_dW>wa.right)x=wa.right-g_dW;if(y+g_dH>wa.bottom)y=wa.bottom-g_dH;
    HWND hw=CreateWindowExW(WS_EX_APPWINDOW,L"ADREENA_DebugWindow",L"ADREENA Debug Log",WS_POPUP|WS_MINIMIZEBOX|WS_SYSMENU,x,y,g_dW,g_dH,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!hw)return;
    g_dbgHwnd=hw;ShowWindow(hw,SW_SHOW);SetForegroundWindow(hw);
}

static Bitmap* loadArt(HINSTANCE){
    DWORD n=0;const char* d=embedded(102,n);if(!d||!n)return nullptr;
    IStream* st=SHCreateMemStream((const BYTE*)d,n);if(!st)return nullptr;
    Bitmap* b=Bitmap::FromStream(st);st->Release();
    if(b&&b->GetLastStatus()!=Ok){delete b;return nullptr;}
    return b;
}
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,PWSTR,int show){
    HANDLE once=CreateMutexW(nullptr,TRUE,L"Local\\ADREENA_Loader_Instance");
    if(GetLastError()==ERROR_ALREADY_EXISTS){
        if(HWND other=FindWindowW(L"ADREENA_LoaderWindow",nullptr)){ShowWindow(other,SW_RESTORE);SetForegroundWindow(other);}
        return 0;
    }
    SetProcessDPIAware();
    {HDC sd=GetDC(nullptr);g_scale=GetDeviceCaps(sd,LOGPIXELSX)/96.0;ReleaseDC(nullptr,sd);}
    InitializeCriticalSection(&g_cs);InitializeCriticalSection(&g_dbgCs);
    GdiplusStartupInput gi;GdiplusStartup(&g_gdiToken,&gi,nullptr);
    g_bg=loadArt(inst);
    RECT wa{0,0,1280,720};SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
    int cw=S(1000);const int maxW=(int)((wa.right-wa.left)*0.92),maxH=(int)((wa.bottom-wa.top)*0.92*DW/DH);
    cw=(min)(cw,(min)(maxW,maxH));
    g_cw=cw;g_ch=(int)(cw*DH/DW+0.5f);g_k=g_cw/DW;
    if(g_bg){g_bgScaled=new Bitmap(g_cw,g_ch,PixelFormat32bppPARGB);Graphics gg(g_bgScaled);gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);gg.SetPixelOffsetMode(PixelOffsetModeHalf);gg.DrawImage(g_bg,Rect(0,0,g_cw,g_ch));}
    WNDCLASSW wc{};wc.lpfnWndProc=wndProc;wc.hInstance=inst;wc.lpszClassName=L"ADREENA_LoaderWindow";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(inst,MAKEINTRESOURCEW(1));wc.hbrBackground=nullptr;
    RegisterClassW(&wc);
    {WNDCLASSW dc{};dc.lpfnWndProc=dbgProc;dc.hInstance=inst;dc.lpszClassName=L"ADREENA_DebugWindow";dc.hCursor=LoadCursorW(nullptr,IDC_ARROW);dc.hIcon=LoadIconW(inst,MAKEINTRESOURCEW(1));RegisterClassW(&dc);}
    g_hwnd=CreateWindowExW(WS_EX_APPWINDOW,L"ADREENA_LoaderWindow",L"ADREENA",WS_POPUP|WS_MINIMIZEBOX|WS_SYSMENU,
        wa.left+((wa.right-wa.left)-g_cw)/2,wa.top+((wa.bottom-wa.top)-g_ch)/2,g_cw,g_ch,nullptr,nullptr,inst,nullptr);
    if(!g_hwnd)return 1;
    ShowWindow(g_hwnd,show);UpdateWindow(g_hwnd);   // the window is up in its loading state before any work starts
    SetTimer(g_hwnd,1,30000,nullptr);SetTimer(g_hwnd,2,700,nullptr);
    startPrepare();
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    delete g_bgScaled;delete g_bg;GdiplusShutdown(g_gdiToken);
    CloseHandle(once);return 0;
}
