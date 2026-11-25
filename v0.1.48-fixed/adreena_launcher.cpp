// ADREENA v0.8: one launcher. Starts State of Decay 2 through Steam if needed,
// waits for game process, then loads ADREENA.dll once. Offline single-player only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <cwchar>
#pragma comment(lib,"shell32.lib")
static DWORD findGame(){
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(s==INVALID_HANDLE_VALUE)return 0;
    PROCESSENTRY32W e{};e.dwSize=sizeof(e);DWORD pid=0;
    if(Process32FirstW(s,&e))do{
        if(_wcsicmp(e.szExeFile,L"StateOfDecay2-Win64-Shipping.exe")==0){pid=e.th32ProcessID;break;}
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
int wmain(){
    std::wcout<<L"ADREENA v0.8 - State of Decay 2 in-game menu\n";
    wchar_t path[MAX_PATH]{};
    DWORD n=GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(!n||n>=MAX_PATH){std::wcerr<<L"Cannot locate launcher.\n";return 1;}
    wchar_t* slash=wcsrchr(path,L'\\');if(!slash)return 1;
    wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"ADREENA.dll");
    if(GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES){std::wcerr<<L"ADREENA.dll missing beside launcher.\n";return 1;}
    DWORD pid=findGame();
    if(!pid){
        std::wcout<<L"Starting State of Decay 2 through Steam...\n";
        HINSTANCE launched=ShellExecuteW(nullptr,L"open",L"steam://rungameid/495420",nullptr,nullptr,SW_SHOWNORMAL);
        if((INT_PTR)launched<=32){std::wcerr<<L"Could not launch Steam URI.\n";return 1;}
        for(int i=0;i<240 && !pid;i++){Sleep(1000);pid=findGame();}
        if(!pid){std::wcerr<<L"Game not found after four minutes.\n";return 1;}
    }
    std::wcout<<L"Game PID: "<<pid<<L"\n";
    if(moduleLoaded(pid,L"ADREENA.dll")){std::wcout<<L"ADREENA already loaded. Switch to game and press PgDn.\n";return 0;}
    // Give Unreal a chance to initialize before DLL attach.
    Sleep(10000);
    HANDLE process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!process){std::wcerr<<L"OpenProcess error: "<<GetLastError()<<L"\n";return 1;}
    SIZE_T bytes=(wcslen(path)+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(process,nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!remote){std::wcerr<<L"VirtualAllocEx error: "<<GetLastError()<<L"\n";CloseHandle(process);return 1;}
    SIZE_T written=0;
    if(!WriteProcessMemory(process,remote,path,bytes,&written)||written!=bytes){
        std::wcerr<<L"WriteProcessMemory error: "<<GetLastError()<<L"\n";
        VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);return 1;
    }
    auto load=(LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    // Like the previously tested loader, assumes matching x64 kernel32 mapping.
    if(!load){VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);return 1;}
    HANDLE thread=CreateRemoteThread(process,nullptr,0,load,remote,0,nullptr);
    if(!thread){std::wcerr<<L"CreateRemoteThread error: "<<GetLastError()<<L"\n";
        VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(process);return 1;}
    DWORD wait=WaitForSingleObject(thread,15000);
    if(wait==WAIT_OBJECT_0){
        // Verify module presence instead of relying on truncated x64 thread exit code.
        Sleep(500);
        bool found=moduleLoaded(pid,L"ADREENA.dll");
        std::wcout<<(found?L"ADREENA.dll found in game. Press PgDn to open menu.\n":L"WARNING: DLL not visible in module list. Check game and logs.\n");
        VirtualFreeEx(process,remote,0,MEM_RELEASE);
    }else std::wcerr<<L"Loader wait timed out; remote path retained for safety.\n";
    CloseHandle(thread);CloseHandle(process);
    return 0;
}
