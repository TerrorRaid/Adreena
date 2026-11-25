// ADREENA v0.9 - D3D11-native overlay; no Direct2D/DXGI surface render target.
// Windows x64, offline single-player. PgDn toggles the menu after renderer initialization.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <tlhelp32.h>
#include <malloc.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>
#include <initializer_list>
#include <algorithm>
#include <random>
#include <set>
#include <array>
#include <map>
// ---- hidden strings: a few names (the game process, this loader's session name) are kept out of the binary as plain
// text. This only defeats a quick look with "strings" or a hex editor - it is not a cryptographic protection.
template<size_t N> struct ObStr{
    wchar_t buf[N]{};
    constexpr ObStr(const wchar_t (&s)[N]){for(size_t i=0;i<N;++i)buf[i]=(wchar_t)(s[i]^(0x7C3Fu+i*0x51u));}
};
template<size_t N> static std::wstring deob(const ObStr<N>& o){
    std::wstring r(N-1,0);for(size_t i=0;i<N-1;++i)r[i]=(wchar_t)(o.buf[i]^(0x7C3Fu+i*0x51u));return r;
}
#define HSTR(lit) ([]()->const std::wstring&{static constexpr ObStr<sizeof(L##lit)/sizeof(wchar_t)> o(L##lit);static const std::wstring v=deob(o);return v;}())
#include <climits>
#include <cwchar>
#include <string>
#include <cwctype>
#include <gdiplus.h>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dwrite.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")

namespace {
template<class T> void drop(T*& p){if(p){p->Release();p=nullptr;}}
HMODULE dllModule{};
// User banner images are loaded from Banners next to ADREENA.dll.
std::vector<std::wstring> bannerFiles;
// Menu state is touched by the game window thread (menuWndProc) and the render thread (hookedPresent/drawOverlay).
// Everything that reads or writes it must hold menuLock. Recursive: menuWndProc calls helpers that lock again.
std::recursive_mutex menuLock;
int selectedBanner=0; // -1 means None (transparent header).
bool bannerDropdownOpen=false;
int bannerDropdownScroll=0;
constexpr int BANNER_VISIBLE_ROWS=6;
constexpr int BANNER_ROW_HEIGHT=25;
constexpr int BANNER_MAX_HEIGHT=200;
int bannerDropdownRows(){return (std::min)(BANNER_VISIBLE_ROWS,(int)bannerFiles.size()+1);}
int bannerDropdownTop(int bannerY){return bannerY+28;} // Dropdown opens below the field.
void clampBannerScroll(int rows){bannerDropdownScroll=(std::max)(0,(std::min)(bannerDropdownScroll,(int)bannerFiles.size()+1-rows));}
std::wstring bannerWarning;
int bannerRevision=0;
int bannerLoadedRevision=-1;
ID3D11ShaderResourceView* standaloneBannerView=nullptr;
int standaloneBannerW=0,standaloneBannerH=0;
std::wstring bannerFolder;

// Official presets live in Presets next to ADREENA.dll, not in the custom Banners dropdown.
struct BannerPreset { const wchar_t* file; const wchar_t* label; COLORREF bg, accent, text; };
const BannerPreset officialPresets[]={
    {L"crimson_moon.png",L"Crimson Moon",RGB(23,18,24),RGB(178,45,65),RGB(244,221,216)},
    {L"moonlit_sti.png",L"Moonlit STI",RGB(20,19,34),RGB(174,88,220),RGB(239,227,251)},
    {L"skulls.png",L"Skulls",RGB(20,18,43),RGB(55,160,234),RGB(235,223,255)},
    {L"underwater_temple.png",L"Underwater Temple",RGB(11,24,44),RGB(104,91,225),RGB(220,238,255)},
    {L"schoolhouse.png",L"Schoolhouse",RGB(38,35,42),RGB(222,90,170),RGB(255,244,237)},
    {L"undead_labs.png",L"Undead Labs",RGB(39,42,12),RGB(171,188,13),RGB(255,255,233)},
    {L"surreal_collage.png",L"Surreal Collage",RGB(26,24,33),RGB(138,82,143),RGB(236,224,233)},
    {L"tagged.png",L"Tagged",RGB(8,38,24),RGB(0,222,107),RGB(243,230,232)},
};
constexpr int PRESET_COUNT=sizeof(officialPresets)/sizeof(officialPresets[0]);
int selectedPreset=0; // Default to Crimson Moon; -1 = custom banner. Saved profiles can override.
// Settings is one continuous page; the preset gallery has no nested scrolling.
int settingsScroll=0;
bool draggingSettingsScroll=false;
int settingsDragStartY=0,settingsDragStartScroll=0;
constexpr int SETTINGS_PRESET_ROW=108;
constexpr int SETTINGS_PRESET_CELL_H=94;
constexpr int SETTINGS_BANNER_OFF=234;   // Settings: banner dropdown, relative to the page top
constexpr int SETTINGS_PRESET_TOP_OFFSET=338;   // room for the Profiles block between the banner chooser and the preset gallery
int settingsMaxScroll();
// Data root = the folder that holds Presets, Banners and Profiles. Normally the folder the DLL sits in; when the DLL sits in a
// folder named "Core" (ADREENA loader layout) it is that folder's parent, and logs/bookmarks use Logs\ and Profiles\.
std::wstring adreenaRootDir(bool* coreLayout=nullptr){
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(dllModule,path,MAX_PATH);
    wchar_t* slash=wcsrchr(path,L'\\');if(slash)*(slash+1)=0;
    std::wstring dir=path;bool core=false;
    std::wstring t=dir;if(!t.empty()&&t.back()==L'\\')t.pop_back();
    const size_t p=t.find_last_of(L'\\');
    if(p!=std::wstring::npos&&_wcsicmp(t.substr(p+1).c_str(),L"Core")==0){dir=t.substr(0,p+1);core=true;}
    if(coreLayout)*coreLayout=core;
    return dir;
}
std::wstring presetFolder;
void initPresetFolder(){
    if(!presetFolder.empty())return;
    presetFolder=adreenaRootDir()+L"Presets\\";
}

ULONG_PTR bannerGdiplusToken=0;
std::wstring weaponFolder;
void initWeaponFolder(){
    if(!weaponFolder.empty())return;
    weaponFolder=adreenaRootDir()+L"WeaponImages\\";   // feature removed from the menu: the folder is no longer created
}
std::wstring weaponBaseName(const std::wstring& id){std::wstring b=id;if(b.size()>2&&b.compare(b.size()-2,2,L"_C")==0)b.resize(b.size()-2);return b;}
// Loads WeaponImages\<weapon>.png (also .jpg/.jpeg/.bmp); the weapon id is the game's class name, with or without the trailing _C.
HBITMAP loadWeaponBitmap(const std::wstring& id,int& w,int& h){
    initWeaponFolder();w=h=0;if(id.empty())return nullptr;
    if(!bannerGdiplusToken){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&bannerGdiplusToken,&input,nullptr);}
    if(!bannerGdiplusToken)return nullptr;
    const std::wstring names[2]={weaponBaseName(id),id};
    for(const std::wstring& n:names)for(const wchar_t* ext:{L".png",L".jpg",L".jpeg",L".bmp"}){
        Gdiplus::Bitmap image((weaponFolder+n+ext).c_str());
        if(image.GetLastStatus()!=Gdiplus::Ok)continue;
        HBITMAP result=nullptr;
        if(image.GetHBITMAP(Gdiplus::Color(255,42,37,46),&result)==Gdiplus::Ok&&result){w=(int)image.GetWidth();h=(int)image.GetHeight();return result;}
    }
    return nullptr;
}
void initBannerFolder(){
    if(!bannerFolder.empty())return;
    bannerFolder=adreenaRootDir()+L"Banners\\";
    CreateDirectoryW(bannerFolder.c_str(),nullptr);
}
void refreshBannerFiles(){
    initBannerFolder();
    std::wstring previous=selectedBanner>=0&&selectedBanner<(int)bannerFiles.size()?bannerFiles[selectedBanner]:L"";
    bannerFiles.clear();
    WIN32_FIND_DATAW fd{};
    HANDLE h=FindFirstFileW((bannerFolder+L"*").c_str(),&fd);
    if(h!=INVALID_HANDLE_VALUE){
        do{
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            std::wstring name=fd.cFileName;
            size_t dot=name.find_last_of(L'.');if(dot==std::wstring::npos)continue;
            std::wstring ext=name.substr(dot);
            for(auto& c:ext)if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';
            if(ext==L".png"||ext==L".jpg"||ext==L".jpeg"||ext==L".bmp")bannerFiles.push_back(name);
        }while(FindNextFileW(h,&fd));
        FindClose(h);
    }
    std::sort(bannerFiles.begin(),bannerFiles.end());
    // Keep None selected across folder refreshes, and preserve an existing selection.
    if(selectedBanner==-1)return;
    selectedBanner=0;
    for(int i=0;i<(int)bannerFiles.size();++i)if(bannerFiles[i]==previous){selectedBanner=i;break;}
}
HBITMAP loadSelectedBanner(){
    if(selectedPreset>=0&&selectedPreset<PRESET_COUNT){
        initPresetFolder();
        if(!bannerGdiplusToken){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&bannerGdiplusToken,&input,nullptr);}
        Gdiplus::Bitmap image((presetFolder+officialPresets[selectedPreset].file).c_str());
        HBITMAP result=nullptr;
        if(image.GetLastStatus()==Gdiplus::Ok&&image.GetHBITMAP(Gdiplus::Color(0,0,0),&result)==Gdiplus::Ok)return result;
        return nullptr; // Do not silently show an unrelated custom banner.
    }
    if(selectedBanner==-1)return nullptr;
    if(bannerFiles.empty())refreshBannerFiles();
    if(selectedBanner>=0&&selectedBanner<(int)bannerFiles.size()){
        if(!bannerGdiplusToken){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&bannerGdiplusToken,&input,nullptr);}
        if(bannerGdiplusToken){
            Gdiplus::Bitmap image((bannerFolder+bannerFiles[selectedBanner]).c_str());
            if(image.GetLastStatus()==Gdiplus::Ok){
                HBITMAP result=nullptr;
                if(image.GetHBITMAP(Gdiplus::Color(0,0,0),&result)==Gdiplus::Ok)return result;
            }
        }
    }
    return LoadBitmapW(dllModule,MAKEINTRESOURCEW(102));
}

std::atomic<bool> running{false},graphicsReady{false},showMenu{false},health{false},stamina{false},clip{false},meleeWeight{false},weaponDurability{false},noFatigue{false},noRecoil{false},noSway{false},infiniteResources{false},infiniteCarFuel{false};
std::atomic<bool> aimbotFlag{false};
std::atomic<bool> infiniteAmmo{false},vehicleHealthLock{false},durHookFallback{false};
volatile uintptr_t ammoItemClassPtr=0,ammoLastOther=0;volatile unsigned ammoKeptCount=0;   // Infinite Ammo: AmmoItem class, last non-ammo class removed, reloads kept
volatile uintptr_t fuelLatchObj=0;volatile unsigned fuelLatchVal=0;   // Vehicle Fuel Lock: the fuel object and the value it is frozen at   // feature ids 3 and 46 (keep-ticker, game thread)   // Weapons > Aimbot (feature id 45)
HWND gameWindow{};
std::atomic<bool> giveAutoStop{false};   // Give Item: the automatic start-up scan has finished (or is no longer needed)
WNDPROC oldWndProc{};
using PresentFn=HRESULT(__stdcall*)(IDXGISwapChain*,UINT,UINT);
PresentFn originalPresent{};
void** presentSlot{};
ID3D11Device* device{};
ID3D11DeviceContext* context{};
IDXGISwapChain* activeSwap{};
ID3D11VertexShader* vertexShader{};
ID3D11PixelShader* pixelShader{};
ID3D11InputLayout* inputLayout{};
ID3D11Buffer* vertexBuffer{};
ID3D11Texture2D* uiTexture{};
ID3D11ShaderResourceView* uiView{};
ID3D11Texture2D* bannerListTexture{};
ID3D11ShaderResourceView* bannerListView{};
ID3D11Texture2D* cursorTexture{};
ID3D11ShaderResourceView* cursorView{};
ID3D11SamplerState* sampler{};
ID3D11BlendState* blendState{};
ID3D11DepthStencilState* depthState{};
ID3D11RasterizerState* rasterState{};
constexpr int MW=680,MH=520; // LOGICAL layout size: every layout, hit-test and font coordinate below is in these units.
// ---- Resolution-independent scaling -------------------------------------------------
// uiScale converts logical units to real screen pixels. The D3D texture is created at
// exactly MW*uiScale x MH*uiScale and the on-screen quad is the same pixel size, so the
// texture is sampled 1:1 (no resampling blur). Layout code never needs to know the scale.
// It follows the game window size automatically; there are no scale hotkeys.
float uiScale=1.0f;        // logical -> pixels (currently applied)
int TW=MW,TH=MH;           // current texture size == on-screen menu size in pixels
inline int toLogical(int px){return (int)floorf((float)px/uiScale);}
constexpr int TOP=122, ROW=30, HEADER_H=94, CAT_TOP=124, CAT_ROW=43;
int settingsMaxScroll(){
    const int top=HEADER_H+53;
    const int contentEnd=top+SETTINGS_PRESET_TOP_OFFSET+((PRESET_COUNT+1)/2)*SETTINGS_PRESET_ROW+20;
    return (std::max)(0,contentEnd-(MH-34));
}
constexpr int SIDE_MIN=46, SIDE_MAX=190;
int SIDE=SIDE_MIN; // Live sidebar width (starts collapsed); independent of the banner and gameplay features.
bool resizingSidebar=false;
int sidebarDragStartX=0,sidebarDragStartWidth=0;
int cachedSidebarWidth=-1;
int menuX=-1, menuY=-1, activeCategory=0, scrollOffset=0;
// Cache the expensive 3x GDI rasterization. The D3D texture remains valid
// between changes; the overlay quad itself is still drawn every Present.
bool uiCacheValid=false;
int cachedCategory=-1,cachedScroll=-1,cachedFeatureMask=-1;
POINT cachedCursor{-9999,-9999};
ULONGLONG lastCursorRaster=0;
bool dragging=false; POINT dragStart{}; int dragOriginX=0,dragOriginY=0;
constexpr int categoryCount=7;
int themeBg[3]={23,18,24};
int themeAccent[3]={178,45,65};
int themeText[3]={244,221,216};
int bgOpacity=100; // 100% opaque by default; affects flat background pixels only.
bool draggingOpacity=false;
// UI values. Engine bindings require verified game signatures/pointer chain.
std::atomic<float> characterTimeDilation{1.0f};
std::atomic<float> visualTimeValue{1600.0f};
float worldTimeScale=1.0f;
float gravityScaleValue=1.0f;   // player gravity scale (Z-SPWN > Fun)
std::atomic<bool> worldToggleFlags[3]{};   // 0 night vision, 1 ghost mode, 2 enclave lighting (feature ids 42..44)
std::atomic<bool> gfxFlags[5]{};   // 0 no motion blur, 1 no film grain, 2 no anti-aliasing, 3 no tonemapper effects, 4 near clip (World > Advanced)
// World tools run on the game thread: wParam 0 = time scale (lParam float bits), 1 = gravity scale (lParam float bits), 2 = graphics toggle (lParam = index<<1 | on)
constexpr UINT WM_ADREENA_WORLD=WM_APP+0x2AF;
constexpr UINT WM_ADREENA_COMM=WM_APP+0x2B4;     // community resources / combine items / mission timer (game thread)
constexpr UINT WM_ADREENA_HORDE=WM_APP+0x2B7;    // horde spawn: wParam 0..2 start small/medium/massive, 3 clear area, 10 tick (game thread)
constexpr UINT WM_ADREENA_KEEP=WM_APP+0x2B8;     // keep-ticker: infinite ammo / durability top-up, vehicle health lock (game thread)
constexpr UINT WM_ADREENA_AIM=WM_APP+0x2B6;      // aimbot tick (game thread)
constexpr UINT WM_ADREENA_TELE=WM_APP+0x2B3;     // teleport to the map marker (game thread)
constexpr UINT WM_ADREENA_VOICE=WM_APP+0x2B2;    // wParam 0: load voice list, 1: apply entry lParam (game thread)
constexpr UINT WM_ADREENA_APPEAR=WM_APP+0x2B1;   // wParam 0: load appearance list, 1: apply entry lParam (game thread)
constexpr UINT WM_ADREENA_CHAR=WM_APP+0x2B0;   // wParam: character action (0 body,1 hat,2 sickness,3 trauma,4 unlearn,5 body back,6 hat back,7 hat reset,8 body reset); lParam: steps to undo for 7/8
inline void postWorldFloat(WPARAM kind,float v){uint32_t u=0;memcpy(&u,&v,4);if(gameWindow)PostMessageW(gameWindow,WM_ADREENA_WORLD,kind,(LPARAM)u);}
std::atomic<bool> visualTimeLock{false};
// New CT features default OFF. Skill trigger is armed briefly by its button.
std::atomic<bool> unlimitedWeight{false},noBrokenBone{false},maxAllSkills{false};
std::atomic<bool> meleeStatFlags[7]{};
// Seven new CT-derived features, independent and OFF by default.
std::atomic<bool> extraFlags[7]{};
std::atomic<bool> plannedFlags[3]{}; // No Blood Plague, Instant Radio Cooldown, Perfect Accuracy
bool plannedReady[3]{};
std::atomic<bool> noSpread{false}, accuracySharedActive{false};
bool noSpreadReady=false, accuracySharesNoSpread=false;
void syncAccuracyShared(){accuracySharedActive.store(plannedFlags[2].load() || (accuracySharesNoSpread && noSpread.load()),std::memory_order_release);}
int skillEditorMode=0; // 0=base, 1=specialized; editor pending survivor pointer validation
bool extraReady[7]{};
bool weightPatchReady=false,bonePatchReady=false,skillHookReady=false,meleeStatsReady=false;
std::atomic<ULONGLONG> skillAutoOffAt{0};
std::atomic<ULONGLONG> skillButtonPressedAt{0};
int draggingFeatureSlider=-1; // 0 character, 1 visual time, 2 world time scale

COLORREF themeTextColor(){return RGB(themeText[0],themeText[1],themeText[2]);}
COLORREF themeMutedText(){return RGB((themeText[0]*65)/100,(themeText[1]*65)/100,(themeText[2]*65)/100);}
COLORREF themeLineColor(){return RGB((themeAccent[0]*75)/100,(themeAccent[1]*75)/100,(themeAccent[2]*75)/100);}
int themeRevision=0;
// Settings: compact HEX fields and a native in-overlay HSV color picker.
int editingHex=-1, pickerColor=-1, pickerDrag=-1;
wchar_t hexDraft[3][8]={{0},{0}};
float pickerHue=0.f,pickerSat=0.f,pickerValue=0.f;
constexpr int HEX_INPUT_W=84, HEX_INPUT_H=23;
constexpr int PICKER_SIZE=158, PICKER_GAP=13, PICKER_HUE_W=19; // Picker approximately 25% larger; compact controls unchanged.
void rgbToHsv(const int* rgb,float& h,float& sat,float& value){
    float r=rgb[0]/255.f,g=rgb[1]/255.f,b=rgb[2]/255.f;
    float hi=(std::max)(r,(std::max)(g,b)),lo=(std::min)(r,(std::min)(g,b)),d=hi-lo;
    value=hi;sat=hi>0.f?d/hi:0.f;h=0.f;
    if(d>0.f){if(hi==r)h=(g-b)/d+(g<b?6.f:0.f);else if(hi==g)h=(b-r)/d+2.f;else h=(r-g)/d+4.f;h/=6.f;}
}
COLORREF hsvColor(float h,float sat,float value){
    h=h-floorf(h);float k=h*6.f;int seg=(int)k;float f=k-seg;
    float p=value*(1.f-sat),q=value*(1.f-sat*f),t=value*(1.f-sat*(1.f-f));
    float r=0,g=0,b=0;
    switch(seg){case 0:r=value;g=t;b=p;break;case 1:r=q;g=value;b=p;break;case 2:r=p;g=value;b=t;break;case 3:r=p;g=q;b=value;break;case 4:r=t;g=p;b=value;break;default:r=value;g=p;b=q;break;}
    return RGB((int)(r*255.f+.5f),(int)(g*255.f+.5f),(int)(b*255.f+.5f));
}
void colorHex(int i,wchar_t out[8]){const int* c=i==0?themeBg:(i==1?themeAccent:themeText);swprintf_s(out,8,L"#%02X%02X%02X",c[0],c[1],c[2]);}
void syncHex(int i){colorHex(i,hexDraft[i]);}
void setThemeColor(int i,COLORREF c){int* v=i==0?themeBg:(i==1?themeAccent:themeText);
    int r=GetRValue(c),g=GetGValue(c),b=GetBValue(c);
    if(v[0]!=r||v[1]!=g||v[2]!=b){v[0]=r;v[1]=g;v[2]=b;++themeRevision;uiCacheValid=false;}
    syncHex(i);
}
void selectOfficialPreset(int index){
    if(index<0||index>=PRESET_COUNT)return;
    selectedPreset=index;
    const BannerPreset& p=officialPresets[index];
    setThemeColor(0,p.bg);setThemeColor(1,p.accent);setThemeColor(2,p.text);
    bannerDropdownOpen=false;pickerColor=-1;editingHex=-1;
    ++bannerRevision;uiCacheValid=false;
}
void openPicker(int i){bannerDropdownOpen=false;pickerDrag=-1;pickerColor=i;editingHex=-1;rgbToHsv(i==0?themeBg:(i==1?themeAccent:themeText),pickerHue,pickerSat,pickerValue);uiCacheValid=false;}
int hexDigit(wchar_t c){if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'A'&&c<=L'F')return c-L'A'+10;if(c>=L'a'&&c<=L'f')return c-L'a'+10;return -1;}
bool commitHex(int i){const wchar_t* d=hexDraft[i];if(wcslen(d)!=7||d[0]!=L'#')return false;
    int v=0;for(int j=1;j<7;j++){int n=hexDigit(d[j]);if(n<0)return false;v=(v<<4)|n;}
    setThemeColor(i,RGB((v>>16)&255,(v>>8)&255,v&255));if(pickerColor==i)rgbToHsv(i==0?themeBg:(i==1?themeAccent:themeText),pickerHue,pickerSat,pickerValue);return true;
}
int settingsColW(){return (MW-SIDE-40-28)/2;}
int settingsRightX(){return SIDE+20+settingsColW()+28;}   // left edge of the right-hand Settings column
int bannerDropW(){return (std::min)(255,settingsColW()-12);}
int hexX(){return SIDE+20+104;}   // Settings colours: label on the left, hex field, swatch (and the opacity slider on the first row) on the right
int hexY(int i){return HEADER_H+53+36+i*32;}
// Keep the floating HSV picker fully within the menu content area.
// These coordinates are shared by drawing, mouse hit-testing, and dragging.
int pickerX(){
    const int popupWidth = PICKER_SIZE + PICKER_GAP + PICKER_HUE_W + 22;
    const int preferred = hexX() + HEX_INPUT_W + 10 + 23 + 16;
    return (std::max)(SIDE+20+10, (std::min)(preferred, MW-popupWidth-12)) + 10;
}
int pickerY(){
    const int preferred = hexY(pickerColor>=0?pickerColor:0) + 12;
    const int minY = HEADER_H+53+31;
    const int maxY = MH-34-PICKER_SIZE-11;
    return (std::max)(minY, (std::min)(preferred,maxY));
}
int opacitySliderX(){return hexX();}
constexpr int OPACITY_SLIDER_W=63;
void setOpacityFromLogicalX(int lx){
    int value=(std::max)(0,(std::min)(100,(int)lroundf((lx-opacitySliderX())*100.f/OPACITY_SLIDER_W)));
    if(bgOpacity!=value){bgOpacity=value;++themeRevision;uiCacheValid=false;}
}
void pickerUpdate(int lx,int ly){if(pickerColor<0||pickerDrag<0)return;
    int px=pickerX(),py=pickerY();
    if(pickerDrag==0){pickerSat=(std::max)(0.f,(std::min)(1.f,(float)(lx-px)/(PICKER_SIZE-1)));
        pickerValue=1.f-(std::max)(0.f,(std::min)(1.f,(float)(ly-py)/(PICKER_SIZE-1)));}
    else {
        pickerHue=(std::max)(0.f,(std::min)(1.f,(float)(ly-py)/(PICKER_SIZE-1)));
        // Background defaults to neutral gray (S=0). Hue changes are invisible
        // on gray until saturation is raised in the square. Make the first hue
        // interaction meaningful, without altering the other color pickers.
        if(pickerColor==0 && pickerSat<0.005f){
            pickerSat=0.75f;
            pickerValue=(std::max)(pickerValue,0.50f);
        }
    }
    setThemeColor(pickerColor,hsvColor(pickerHue,pickerSat,pickerValue));
}
// Independent scroll positions. A section stops at its own limit; wheel input never
// propagates to the page when hovering that section.
float pageScroll[7]{};
// Settings page glide (it used to jump 56 px per wheel notch with no animation).
float settingsScrollF=0.f,settingsScrollTarget=0.f;int settingsScrollApplied=0;
float sectionScroll[7][7]{};
float pageScrollTarget[7]{};
float sectionScrollTarget[7][7]{};
// Scrollbar drag state: -1 none, -2 page, 0..3 section.
int draggingScrollbar=-1;
int scrollbarDragGrab=0;
int scrollbarDragCategory=-1;

// Fractional scrolling lets the next row peek into the viewport while wheel
// input glides towards its target rather than jumping by an entire item.
int sectionRevision=0,cachedSectionRevision=-1;
void animateScroll(){
    // Framerate-independent exponential glide: ~230ms to settle to 95% of target.
    // Mouse dragging bypasses this animation and tracks the thumb directly.
    static ULONGLONG lastTick=0;
    const ULONGLONG now=GetTickCount64();
    const float dt=lastTick?(float)(std::min)(now-lastTick,(ULONGLONG)50):16.f;
    lastTick=now;
    const float alpha=1.f-expf(-dt/78.f);
    bool moving=false;
    for(int c=0;c<7;c++){
        auto advance=[&](float& current,float target){
            const float delta=target-current;
            if(fabsf(delta)<0.015f){if(current!=target){current=target;moving=true;}}
            else {
                float next=current+delta*alpha;
                // Avoid rebuilding the entire text texture for changes smaller
                // than one physical pixel; prevents apparent glyph shimmer.
                if(fabsf(next-current)>=0.5f/(uiScale*29.f)){current=next;moving=true;}
                else if(fabsf(delta)<=0.5f/(uiScale*29.f)){current=target;moving=true;}
            }
        };
        advance(pageScroll[c],pageScrollTarget[c]);
        for(int k=0;k<7;k++)advance(sectionScroll[c][k],sectionScrollTarget[c][k]);
    }
    {
        // Settings page: same glide as the feature tabs. Writes made elsewhere (scrollbar drag, tab change)
        // change settingsScroll directly; detect that and re-sync the glide state to it.
        if(settingsScroll!=settingsScrollApplied){settingsScrollF=settingsScrollTarget=(float)settingsScroll;}
        const float d=settingsScrollTarget-settingsScrollF;
        if(fabsf(d)>0.01f)settingsScrollF+=(fabsf(d)<0.5f)?d:d*alpha;
        const int v=(int)lroundf(settingsScrollF);
        if(v!=settingsScroll){settingsScroll=v;moving=true;}
        settingsScrollApplied=settingsScroll;
    }
    if(moving){++sectionRevision;uiCacheValid=false;}
}
constexpr int SECTION_VIEW_ROWS=3;
constexpr int SECTION_ROW_H=29;
constexpr int PAGE_TOP=HEADER_H+53;
constexpr int PAGE_BOTTOM=MH-34;
constexpr int SECTION_STRIDE=168;
constexpr int SECTION_COUNT=6; // Maximum groups per tab; extra groups appear below the original four.
int sectionCount(int category){return category==0?6:category==1?5:category==2?4:(category==3?4:(category==4||category==5?6:4));}
constexpr int TALL_SECTION_H=250;   // Spawn Vehicle (with Bounty Delivery underneath) is taller than a normal 149-unit section
constexpr int SKILL_ROWS=5;          // Skill Editor shows five skill rows (survivors never have more per tab)
constexpr int SKILL_SECTION_H=272;   // Skill Editor: tabs, refresh, 5 skill rows, status, Max button
constexpr int VEHICLE_SECTION_H=222;  // Vehicle: two toggles + "Repair and Refuel" heading, two buttons, status
constexpr int ZOMBIE_SECTION_H=522;  // Zombie Spawner: full button list + heart stats, laid out in the tab (no inner scrolling)
int sectionHeight(int category,int k){
    if(category==4&&k==4)return TALL_SECTION_H;
    if(category==4&&k==0)return VEHICLE_SECTION_H;
    if(category==4&&k==3)return 0;   // old Refuel / Repair subsection removed (its controls live in Vehicle)
    if(category==4&&(k==1||k==2))return 0;   // Engine and Handling subsections removed
    if(category==3&&k==0)return 98;   // Time: just the two slider rows
    if(category==0&&k==4)return SKILL_SECTION_H;
    if(category==2&&k==0)return 315;   // 8 resource rows (see RES_ROWS)   // Set Resources: selector, amount + Set, status, Infinite Resources toggle
    if(category==2&&k==1)return 124;   // Management: two toggle rows
    if(category==2&&k==2)return 124;   // Mission Timer: seconds box, Subtract / Add, status
    if(category==2&&k==3)return 298;   // Give Item: category dropdown, search, 5 result rows, page buttons, amount + Give, status
    if(category==3&&k==1)return 372;   // Teleport: button + status + hotkey row, then the 5 position bookmarks (no separate heading)
    if(category==0&&k==0)return 156;   // Protection spans both columns: four toggle rows plus the Recovery buttons
    if(category==1&&(k==1||k==2))return 96;   // Ammunition and Weapon Condition: just two toggle rows each
    if(category==1&&k==0)return 0;   // Weapon Preview removed   // Weapon Preview: image box + name, Prev / Next / Equipped / Refresh, status
    if(category==0&&k==5)return 178;   // Survivor Name: first / last boxes, Load current + Apply, status
    if(category==0&&k==2)return 450;   // Character / Clothing: hat, outfit, appearance (gender + group + variant dropdowns), voice selector, status
    if(category==5&&k==0)return ZOMBIE_SECTION_H;
    if(category==5&&k==1)return 122;   // Fun: Gravity Scale slider, Haven Protocol button, temporary result line
    if(category==5&&k==3)return 0;     // Utilities removed (Unlimited Spawning moved to Hordes, Haven Protocol to Fun)
    if(category==5&&k==5)return 204;   // Hordes: Unlimited Spawning toggle, three spawn buttons (Small / Medium / Massive), clear, status
    if(category==5&&k==4)return 0;   // unused section slot (hidden)
    if(category==5&&k==2)return 0;   // hidden placeholder section
    return 149;
}
// Top of a section relative to the page top: rows stack by their tallest section (+19 gap, the old 168 stride for 149-high rows).
// Column of a section: -1 spans both columns, 0 left, 1 right. Only the Player tab deviates from the plain left/right alternation.
int sectionCol(int category,int k){
    if(category==0){static const int c[6]={-1,0,1,1,0,0};return k>=0&&k<6?c[k]:0;}   // Protection | Movement + Skill Editor | Character + Utilities
    if(category==1)return k==0?-1:(k+1)%2;   // Weapon Preview spans both columns; the rest keep their old left/right places
    if(category==3)return k==1?1:0;   // World: Time, Exploration, Advanced stacked on the left; Teleport (with bookmarks) on the right
    return k%2;
}
int sectionTopOffset(int category,int k){
    // Each column stacks independently; a full-width section pushes both columns down.
    int top=0;const int ck=sectionCol(category,k);
    for(int j=0;j<k;j++){const int h=sectionHeight(category,j);if(h<=0)continue;const int cj=sectionCol(category,j);if(cj==-1||ck==-1||cj==ck)top+=h+19;}
    return top;
}
constexpr int SECTION_GAP_X=28;
int sectionAvail(){return MW-SIDE-40;}
int sectionHalfW(){return (sectionAvail()-SECTION_GAP_X)/2;}
int sectionX(int category,int k){return sectionCol(category,k)==1?SIDE+20+sectionHalfW()+SECTION_GAP_X:SIDE+20;}
int sectionW(int category,int k){return sectionCol(category,k)==-1?sectionAvail():sectionHalfW();}
int pageMaxScroll(){
    int extent=0;
    for(int k=0;k<sectionCount(activeCategory);k++)extent=(std::max)(extent,sectionTopOffset(activeCategory,k)+sectionHeight(activeCategory,k)+1);
    return (std::max)(0,PAGE_TOP+extent-PAGE_BOTTOM);
}
int sectionItemCount(int category,int k); // Derived from the shared menu mapping below.
float sectionMaxScroll(int category,int k){
    // Three full rows and a hint of the fourth; the last row remains fully reachable.
    if(category==0&&k==0)return 0.f;   // Protection shows all four rows
    return (std::max)(0.f,(float)sectionItemCount(category,k)-3.f);
}
int featureSliderId(int category,int section,int index){
    if(category==0&&section==1&&index==2)return 0;
    if(category==3&&section==0&&index==0)return 1;
    if(category==3&&section==0&&index==1)return 2;
    if(category==5&&section==1&&index==0)return 3;   // Gravity Scale
    return -1;
}
void setFeatureSlider(int id,int x,int start,int width){
    float t=(std::max)(0.f,(std::min)(1.f,(float)(x-start)/width));
    if(id==0)characterTimeDilation=0.1f+roundf(t*58.f)*0.05f;
    if(id==1)visualTimeValue=roundf(t*240.f)*10.f;
    if(id==2){const float v=(std::max)(0.1f,roundf(t*40.f)*0.1f);if(v!=worldTimeScale){worldTimeScale=v;postWorldFloat(0,v);}}
    if(id==3){const float v=0.1f+roundf(t*29.f)*0.1f;if(v!=gravityScaleValue){gravityScaleValue=v;postWorldFloat(1,v);}}
    uiCacheValid=false;
}


int themeHue[2]={0,345};
int cachedThemeRevision=-1;
COLORREF hueRgb(int hue,int brightness){
    float h=(hue%360)/60.0f;int segment=(int)h;float f=h-segment;
    float r=0,g=0,b=0;
    switch(segment){case 0:r=1;g=f;break;case 1:r=1-f;g=1;break;case 2:g=1;b=f;break;case 3:g=1-f;b=1;break;case 4:r=f;b=1;break;default:r=1;b=1-f;break;}
    return RGB((int)(r*brightness),(int)(g*brightness),(int)(b*brightness));
}
void setHue(int index,int mouseX){
    int start=SIDE+24, length=MW-SIDE-62;
    if(length<=0)return;
    int v=(mouseX-start)*359/length;if(v<0)v=0;if(v>359)v=359;
    if(themeHue[index]==v)return;
    themeHue[index]=v;COLORREF c=hueRgb(v,index==0?48:225);
    int* dest=index==0?themeBg:themeAccent;
    dest[0]=GetRValue(c);dest[1]=GetGValue(c);dest[2]=GetBValue(c);
    ++themeRevision;uiCacheValid=false;
}

COLORREF themeAccentColor(){return RGB(themeAccent[0],themeAccent[1],themeAccent[2]);}
void updateThemeSlider(int index,int mouseX){
    if(index<0||index>=6)return;
    constexpr int sx=MW-205, sw=165;
    int v=(mouseX-sx)*255/sw;
    if(v<0)v=0;if(v>255)v=255;
    int* channel=index<3?&themeBg[index]:&themeAccent[index-3];
    if(*channel!=v){*channel=v;++themeRevision;uiCacheValid=false;}
}

// Single version source for the footer. Bump this with every build (v31 was the last build before this constant existed).
constexpr const wchar_t* ADREENA_VERSION=L"v0.1.48";
std::atomic<ULONGLONG> lastWndProcTick{0};   // refreshed by every message our window procedure sees
const wchar_t* categoryNames[]={L"PLAYER",L"WEAPONS",L"COMMUNITY",L"WORLD",L"VEHICLE",L"Z-SPWN",L"SETTINGS"};
struct FeatureInfo {const wchar_t* label; int id;};
// Feature IDs 0..4 are the original ADREENA controls; 5..10 are CT-based additions.
const FeatureInfo playerItems[]={{L"God Mode",0},{L"Infinite Stamina",1},{L"No Infection",-1},{L"Movement Speed",-1},{L"Unlimited Weight",12},{L"No Fatigue",6},{L"Character Time Dilation",-1},{L"No Broken Bones",11},{L"Instant Max Skill Level (all)",20},{L"Quick Search",21},{L"No Blood Plague",28},{L"Instant Radio Cooldown",29}};
const FeatureInfo weaponItems[]={{L"Infinite Ammo",3},{L"No Reload / Clip Lock",2},{L"Infinite Durability",5},{L"No Recoil",7},{L"Perfect Accuracy",30},{L"No Spread",32},{L"Max Dismember",13},{L"Max Impact",14},{L"Max Knockdown",15},{L"Max Lethality",16},{L"Max Quietness (Melee)",17},{L"Max Speed",18},{L"Max Ease Of Use",19},{L"Max Fire Rate",24},{L"Silent Weapons",25},{L"No Sway",31},{L"Aimbot",45}};
const FeatureInfo communityItems[]={{L"Infinite Resources",9},{L"Duplicate Locker Items",27},{L"Instant Construction & Upgrades",26}};
const FeatureInfo worldItems[]={{L"Time of Day Lock",-1},{L"Time Scale",-1},{L"Quick Survey",22},{L"Zombie Radar",23},{L"No Motion Blur",33},{L"No Film Grain",34},{L"No Anti-Aliasing",35},{L"No Tonemapper Effects",37},{L"Near Clip",38},{L"Night Vision",42},{L"Ghost Mode",43},{L"Enclave Lighting",44}};
const FeatureInfo vehicleItems[]={{L"Vehicle Health Lock",46},{L"Vehicle Fuel Lock",10},{L"Vehicle Damage Scale",-1}};
const FeatureInfo funItems[]={{L"Gravity Scale",-1}};
struct FeatureList {const FeatureInfo* items;int count;};
const FeatureList featureLists[]={ {playerItems,(int)(sizeof(playerItems)/sizeof(playerItems[0]))},{weaponItems,(int)(sizeof(weaponItems)/sizeof(weaponItems[0]))},{communityItems,(int)(sizeof(communityItems)/sizeof(communityItems[0]))},{worldItems,(int)(sizeof(worldItems)/sizeof(worldItems[0]))},{vehicleItems,(int)(sizeof(vehicleItems)/sizeof(vehicleItems[0]))},{funItems,(int)(sizeof(funItems)/sizeof(funItems[0]))},{nullptr,0} };
// One authoritative feature-to-section mapping for rendering, hit-testing and scrolling.
// Append indices here to grow a section; there are no fixed eight-item slots.
using SectionIndices = std::vector<int>;
static const std::vector<std::vector<SectionIndices>> menuSections = {
    { {0,7,10,5}, {1,4,6}, {}, {9,11}, {8}, {} }, // Player (last section: Survivor Name, custom)
    { {}, {0,1}, {2,14}, {3,4,15,13,5,16}, {6,7,8,9,10,11,12} }, // Weapons: Weapon Preview (custom, full width) | Ammunition | Weapon Condition | Accuracy | Melee
    { {}, {1,2,0}, {}, {} }, // Community: Set Resources (custom) | Community | Inventory (custom) | Mission Timer (custom) | Building
    { {0,1}, {}, {2,3,9,10,11}, {4,5,6,7,8} }, // World: Time | Teleport (custom, with bookmarks) | Exploration | Advanced (graphics toggles): Time | Advanced (graphics toggles) | Exploration | Teleport (custom)
    { {0,1}, {}, {}, {}, {}, {} }, // Vehicle (Engine / Handling removed)
    { {}, {0}, {}, {}, {}, {} } // Z-SPWN: Zombie Spawner (custom, left) | Fun (right; Gravity Scale + Haven Protocol) | hidden | removed (Utilities) | hidden | Hordes
};
int sectionItemCount(int category,int k){
    if(category<0 || category>=(int)menuSections.size() || k<0 || k>=(int)menuSections[category].size())return 0;
    // Skill Editor has a custom layout, with one actionable row.
    if(category==0 && (k==2||k==4||k==5))return 1;
    if(category==1 && k==0)return 1;   // Weapon Preview: custom layout
    if(category==3 && k==1)return 1;   // Teleport / Position Bookmarks: custom layouts
    if(category==2 && (k==0||k==2||k==3))return 1;   // Community: Set Resources / Inventory / Mission Timer custom layouts
    if(category==5 && (k==0||k==3||k==4||k==5))return 1; // Zombie Spawner / Utilities: custom layouts
    if(category==4 && (k==4||k==5))return 1; // Spawn Vehicle (+ Bounty Delivery) / Trunk Size: custom layouts
    const auto& fl=featureLists[category];
    int count=0;
    for(int ix:menuSections[category][k])if(ix>=0 && ix<fl.count)++count;
    return count;
}
bool swayHookReady=false,ammoPatchReady=false,durPatchReady=false;
bool ctFeatureReady[6]{}; // feature IDs 5..10; only activate when every required hook is installed
std::atomic<bool>* featureFlag(int id){
    switch(id){
        case 0:return &health;case 1:return &stamina;case 2:return &clip;
        case 3:return &infiniteAmmo;case 4:return &meleeWeight;
        case 5:return &weaponDurability;case 6:return &noFatigue;
        case 7:return &noRecoil;
        case 9:return &infiniteResources;case 10:return &infiniteCarFuel;
        case 11:return &noBrokenBone;case 12:return &unlimitedWeight;
        case 13:case 14:case 15:case 16:case 17:case 18:case 19:return &meleeStatFlags[id-13];
        case 21:case 22:case 23:case 24:case 25:case 26:case 27:return &extraFlags[id-21];
        case 28:case 29:case 30:return &plannedFlags[id-28];
        case 31:return &noSway;case 32:return &noSpread;
        case 33:case 34:case 35:return &gfxFlags[id-33];case 37:return &gfxFlags[3];case 38:return &gfxFlags[4];
        case 42:case 43:case 44:return &worldToggleFlags[id-42];
        case 45:return &aimbotFlag;case 46:return &vehicleHealthLock;
        default:return nullptr;
    }
}
bool enabled(int id){auto* f=featureFlag(id);return f&&f->load();}
bool featureReady(int id){
    if(id<0)return false;
    if(id==3)return ammoPatchReady;
    if(id>=5&&id<=10)return ctFeatureReady[id-5];
    if(id==11)return bonePatchReady;
    if(id==12)return weightPatchReady;
    if(id>=13&&id<=19)return meleeStatsReady;
    if(id==20)return skillHookReady;
    if(id>=21&&id<=27)return extraReady[id-21];
    if(id>=28&&id<=30)return plannedReady[id-28];
    if(id==31)return swayHookReady;
    if(id==32)return noSpreadReady;
    return true;
}
void toggle(int id){if(!featureReady(id))return;auto* f=featureFlag(id);if(f)f->store(!f->load());if(id==10)fuelLatchObj=0;if(id==30||id==32)syncAccuracyShared();
    if(id>=42&&id<=44){if(gameWindow)PostMessageW(gameWindow,WM_ADREENA_WORLD,(WPARAM)(3+(id-42)),(LPARAM)(worldToggleFlags[id-42].load()?1:0));}
    if((id>=33&&id<=35)||id==37||id==38){const int gi=id==37?3:(id==38?4:id-33);if(gameWindow)PostMessageW(gameWindow,WM_ADREENA_WORLD,2,(LPARAM)((gi<<1)|(gfxFlags[gi].load()?1:0)));}}
void logMessage(const char* msg,HRESULT hr=S_OK);
void activateMaxSkills(){
    skillButtonPressedAt.store(GetTickCount64(),std::memory_order_release);
    uiCacheValid=false;
    if(!skillHookReady){logMessage("Max Skills: hook unavailable; activation refused");return;}
    logMessage("Max Skills: one-shot hook armed (close/reopen character menu to refresh)");
    maxAllSkills.store(true,std::memory_order_release);
    skillAutoOffAt.store(GetTickCount64()+5000,std::memory_order_release);
    uiCacheValid=false;
}
// ---- Coordinate spaces -------------------------------------------------------------------------------------------
// The menu is drawn in the game's BACK BUFFER pixels (menuX/menuY/TW/TH). Mouse messages arrive in window CLIENT pixels.
// They are the same unless the game renders at a different size than its window (render scaling, some borderless
// modes); clientToBuffer() maps one onto the other so clicks always land where the menu is drawn.
std::atomic<int> g_bbW{0},g_bbH{0};   // current back buffer size, set every frame by drawOverlay
bool bufferSize(int& w,int& h){
    w=g_bbW.load(std::memory_order_relaxed);h=g_bbH.load(std::memory_order_relaxed);
    if(w>0&&h>0)return true;
    if(!gameWindow)return false;
    RECT rc{};GetClientRect(gameWindow,&rc);w=rc.right;h=rc.bottom;return w>0&&h>0;
}
void clientToBuffer(POINT& p){
    if(!gameWindow)return;
    const int bw=g_bbW.load(std::memory_order_relaxed),bh=g_bbH.load(std::memory_order_relaxed);
    RECT rc{};GetClientRect(gameWindow,&rc);
    if(bw<=0||bh<=0||rc.right<=0||rc.bottom<=0||(rc.right==bw&&rc.bottom==bh))return;
    p.x=MulDiv(p.x,bw,rc.right);p.y=MulDiv(p.y,bh,rc.bottom);
}
// Saved menu position is a FRACTION of the free space (0 = left/top edge, 1 = right/bottom edge), so it adapts when the
// resolution or menu scale changes and the menu can never end up off screen.
float menuFx=-1.f,menuFy=-1.f;   // -1 = not known yet
bool menuApplyFrac=false;       // a profile supplied fractions: place the menu from them on the next menuPosition()
void menuPosition(){
    int W=0,H=0;if(!bufferSize(W,H))return;
    static int lastW=0,lastH=0,lastTW=0,lastTH=0;
    const bool sizeChanged=lastW>0&&(W!=lastW||H!=lastH||TW!=lastTW||TH!=lastTH);
    const int freeW=W-TW,freeH=H-TH;
    if(menuX<0||menuY<0){
        menuX=(W-TW)-(int)(75*uiScale)-21;   // default: right side, vertically centred (as before)
        menuY=(H-TH)/2-23;
        menuApplyFrac=false;
    }else if((menuApplyFrac||sizeChanged)&&menuFx>=0.f&&menuFy>=0.f){
        if(freeW>0)menuX=(int)lroundf(menuFx*freeW);
        if(freeH>0)menuY=(int)lroundf(menuFy*freeH);
        menuApplyFrac=false;
    }
    // Keep the whole menu on screen, but never shuffle it while minimized.
    if(W>=TW&&menuX>W-TW)menuX=W-TW;
    if(H>=TH&&menuY>H-TH)menuY=H-TH;
    menuX=(menuX<0?0:menuX);menuY=(menuY<0?0:menuY);
    if(freeW>0)menuFx=(std::min)(1.f,(float)menuX/(float)freeW);
    if(freeH>0)menuFy=(std::min)(1.f,(float)menuY/(float)freeH);
    lastW=W;lastH=H;lastTW=TW;lastTH=TH;
}

void logMessage(const char* msg,HRESULT hr){
    bool core=false;std::wstring path=adreenaRootDir(&core);
    if(core){path+=L"Logs\\";CreateDirectoryW(path.c_str(),nullptr);}
    path+=L"ADREENA_debug.log";
    FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"a")==0 && f){fprintf(f,"%s HRESULT=0x%08lX\n",msg,(unsigned long)hr);fclose(f);}
}
// ===== Zombie spawner =====================================================================================
// Spawns zombies by calling the game's own GameplayStatics functions through UObject::ProcessEvent, found at
// runtime by walking GObjects. The four RVAs below are for THIS game build (from Dumper-7) and WILL change
// after a game update. Everything is validated at runtime; on any doubt the spawner refuses and says why
// (menu status line + ADREENA_debug.log). It only ever runs on the game's main thread, from menuWndProc.
constexpr UINT WM_ADREENA_SPAWN=WM_APP+0x2A1;
struct ZombieDef{const wchar_t* label;const char* cls;const char* pkg;};
const ZombieDef zombieDefs[]={
    {L"Fast Zombie","Zombie_Fast_C","/Game/Characters/Zombie/Presets/Zombie_Fast"},
    {L"Slow Zombie","Zombie_Slow_C","/Game/Characters/Zombie/Presets/Zombie_Slow"},
    {L"BP Bloater","BloodBloater_C","/Game/Characters/Bloater/BloodBloater"},
    {L"Plague Zombie","Zombie_Plague_C","/Game/Characters/Zombie/Presets/Zombie_Plague"},
    {L"Juggernaut","JuggernautCharacter_C","/Game/Characters/Juggernaut/JuggernautCharacter"},
    {L"BP Juggernaut","BloodPlagueJuggCharacter_C","/Game/Characters/Juggernaut/BloodPlagueJuggCharacter"},
    {L"Plague Heart","BloodPlagueNode_C","/Game/Characters/BloodPlagueNode/BloodPlagueNode"},
    {L"Plague Wall","BloodPlagueWall_C","/Game/Characters/BloodPlagueWall/BloodPlagueWall"},
    {L"Feral","FeralCharacter_C","/Game/Characters/Feral/FeralCharacter"},
    {L"BP Feral","BloodFeral_C","/Game/Characters/Feral/BloodFeral"},
    {L"BP Screamer","BloodScreamer_C","/Game/Characters/Screamer/BloodScreamer"},
    {L"Screamer","ScreamerCharacter_C","/Game/Characters/Screamer/ScreamerCharacter"},
    {L"Armored Zombie","Zombie_Armored_C","/Game/Characters/Zombie/Presets/Zombie_Armored"},
    {L"Black Plague Heart","BlackPlagueNode_C","/Game/Characters/BlackPlagueNode/BlackPlagueNode"}
};
constexpr int zombieCount=(int)(sizeof(zombieDefs)/sizeof(zombieDefs[0]));
int spawnSelected=0;                       // guarded by menuLock
std::wstring hordeStatus=L"Ready";
std::wstring spawnStatus=L"Ready";         // guarded by menuLock
// Zombie spawner extras (all guarded by menuLock)
bool zombieCustom=false;                   // apply custom health / damage multipliers to spawned zombies
int zombieHP=100,zombieMeleeT=10,zombieRangedT=10;   // multipliers are stored in tenths (10 = x1.0)
bool zombieLoadedKnown=false;unsigned char zombieLoadedFlags[zombieCount]{};
bool zombieMaskRequested=false,zombiePollActive=false;int zombieLoadPending=-1;
bool unlimitedSpawn=false;std::wstring unlimitedStatus=L"Off";   // guarded by menuLock
constexpr UINT WM_ADREENA_UNLIM=WM_APP+0x2AB;   // wParam 1/0: set DynamicPawnSpawner.bIsUnlimitedSpawningEnabled (game thread)
// Button order in the Z-SPWN list (indices into zombieDefs): normal types first, plague hearts last.
constexpr int zombieOrder[14]={0,1,12,3,2,4,5,8,9,11,10,7,6,13};
int zombieBtnW[14]{};   // button widths measured while drawing (guarded by menuLock); click handler reuses them
constexpr int zombieRowTop(int i){return i<12?33+i*25:338+(i-12)*25;}   // relative to section top
std::wstring skillStatus=L"Ready";   // guarded by menuLock
std::wstring charStatus=L"Ready";    // Character / Clothing section status, guarded by menuLock
std::vector<std::wstring> appearanceNames;std::vector<unsigned long long> appearanceRaw;std::vector<std::vector<int>> appearanceGroups[2];std::vector<std::wstring> appearanceGroupNames[2];   // per gender: 0 male, 1 female
int appearanceGender=1,appearanceGroupSel=0,appearanceVarSel=0;
int dropOpen=0,dropScroll=0;          // open dropdown: 0 none, 1 appearance group, 2 appearance variant, 3 Give Item category
int itemCat=0;
const wchar_t* const itemCatNames[9]={L"Ranged weapons",L"Melee weapons",L"Close combat",L"Consumables",L"Ammo",L"Miscellaneous",L"Backpacks",L"Resources",L"Weapon mods"};
RECT dropField[5]{};bool dropFieldSet[5]{};   // field rectangles from the last draw (popups anchor to them)
// ---- Profiles (menu layout + settings saved as JSON) and position bookmarks. State is guarded by menuLock.
constexpr UINT WM_ADREENA_PROFILE=WM_APP+0x2B5;   // wParam 1: apply the startup profile's layout, 2: apply its gameplay settings (game thread)
std::vector<std::wstring> profileNames;int profileSel=-1;
wchar_t profileNameBuf[28]{};
std::wstring profileStatus=L"Type a name and press Save";
bool profDropOpen=false;int profDropScroll=0;RECT profDropField{};
bool profileDelArm=false;
std::wstring profileAuto;   // profile loaded automatically at startup ("" = none)
struct Bookmark{std::wstring name;float x=0,y=0,z=0;};
std::vector<Bookmark> bookmarks;int bookmarkSel=-1;
wchar_t bookmarkNameBuf[28]{};
std::wstring bookmarkStatus=L"Type a name and press Save";
std::wstring bookmarkPendingName;bool bookmarkDelArm=false;
void profilesInit();void profilesRefresh();void profileDoSave();void profileDoLoad();void profileDoDelete();void profileDoAuto();
void bookmarkDoSave();void bookmarkDoTeleport();void bookmarkDoDelete();void bookmarkAdd(float x,float y,float z);
bool profileLoadByName(const std::wstring& name,int phases,std::wstring& msg);
std::vector<std::wstring> voiceNames;std::vector<unsigned long long> voiceRaw;std::vector<std::vector<int>> voiceGroups;std::vector<std::wstring> voiceGroupNames;int voiceGroupSel=0,voiceVarSel=0;   // voices grouped by character (guarded by menuLock)

// Appearance helpers (names are "HumanFemaleArmy_v_13": gender prefix + group + variant tail).
std::wstring appearanceVariantLabel(const std::wstring& full,const std::wstring& group){
    std::wstring rest=full;
    if(rest.compare(0,11,L"HumanFemale")==0)rest=rest.substr(11);else if(rest.compare(0,9,L"HumanMale")==0)rest=rest.substr(9);
    std::wstring tail=rest.compare(0,group.size(),group)==0?rest.substr(group.size()):rest;
    if(tail.empty())return L"Default";
    if(tail.compare(0,3,L"_v_")==0&&tail.size()>3&&tail.find_first_not_of(L"0123456789",3)==std::wstring::npos)return L"Variant "+tail.substr(3);
    while(!tail.empty()&&tail[0]==L'_')tail.erase(0,1);
    return tail.empty()?std::wstring(L"Default"):tail;
}
int dropTotal(){
    if(dropOpen==3)return 9;
    if(dropOpen==4)return (int)bookmarks.size();
    const auto& AG=appearanceGroups[appearanceGender];if(AG.empty())return 0;
    if(dropOpen==1)return (int)AG.size();
    if(dropOpen==2)return (int)AG[appearanceGroupSel%(int)AG.size()].size();
    return 0;
}
int itemInternalCount(int cat);
std::wstring dropItemText(int item){
    if(dropOpen==4)return (item>=0&&item<(int)bookmarks.size())?bookmarks[item].name:std::wstring();
    if(dropOpen==3){
        if(item<0||item>=9)return std::wstring();
        std::wstring t=itemCatNames[item];const int n=itemInternalCount(item);
        if(n>0)t+=L"  ("+std::to_wstring(n)+L" internal)";
        return t;
    }
    const auto& AG=appearanceGroups[appearanceGender];const auto& AN=appearanceGroupNames[appearanceGender];
    if(AG.empty()||item<0)return L"";
    const int gs=appearanceGroupSel%(int)AG.size();
    if(dropOpen==1)return item<(int)AN.size()?AN[item]:L"";
    const auto& mem=AG[gs];if(item>=(int)mem.size())return L"";
    return appearanceVariantLabel(appearanceNames[mem[item]],AN[gs]);
}
int dropSelected(){return dropOpen==4?bookmarkSel:dropOpen==3?itemCat:dropOpen==1?appearanceGroupSel:appearanceVarSel;}
bool dropActive(){return dropOpen&&((dropOpen<=2&&activeCategory==0)||(dropOpen==3&&activeCategory==2)||(dropOpen==4&&activeCategory==3));}
// Popup rectangle for the open dropdown; opens below its field and flips above when there is no room.
bool dropPopupGeom(RECT& r,int& rows,int& total){
    if(dropOpen<1||dropOpen>4||!dropFieldSet[dropOpen])return false;
    total=dropTotal();if(total<=0)return false;
    rows=(std::min)(dropOpen==3?9:(dropOpen==4?6:8),total);
    const RECT& f=dropField[dropOpen];const int h=rows*BANNER_ROW_HEIGHT;
    int top=f.bottom+1;
    if(top+h>MH-34)top=f.top-h-1;
    if(top<HEADER_H+53)top=HEADER_H+53;
    r=RECT{f.left,top,f.right,top+h};return true;
}
// human definitions (guarded by menuLock)
std::wstring teleStatus=L"Place a map marker first";   // World > Teleport status, guarded by menuLock
// Community tab state (guarded by menuLock)
std::vector<std::wstring> resLabels;std::vector<float> resAmts;bool resLoaded=false;
constexpr int RES_ROWS=8;
int resAmtVals[RES_ROWS]{};bool resAmtInit=false;int timerVal=60;
const wchar_t* const resUiNames[RES_ROWS]={L"Food",L"Materials",L"Ammo",L"Medicine",L"Fuel",L"Influence",L"Parts",L"Prestige"};   // rows shown in Set Resources
float resAmountFor(const wchar_t* name){for(size_t i=0;i<resLabels.size()&&i<resAmts.size();++i)if(resLabels[i]==name)return resAmts[i];return -1.f;}
int editingNum=-1;wchar_t numDraft[8]{};   // 0 = resource amount, 1 = mission timer seconds
std::vector<std::wstring> wpnList;int wpnSel=0;std::wstring wpnStatus=L"Open this tab with a weapon equipped";
HBITMAP wpnBmp=nullptr;std::wstring wpnBmpFor;int wpnBmpW=0,wpnBmpH=0;
struct ItemEntry{std::wstring name;int cat;bool internal=false;};
std::vector<ItemEntry> itemAll;int itemSel=-1,itemPage=0,itemCount=1;wchar_t itemFilter[24]{};
int itemInternalCount(int cat){int n=0;for(const ItemEntry& e:itemAll)if(e.cat==cat&&e.internal)++n;return n;}
std::wstring itemStatus=L"Press Load to read the item list";
constexpr int ITEM_ROWS=5;
std::vector<int> itemFiltered(){
    std::vector<int> out;std::wstring f=itemFilter;for(auto& ch:f)ch=(wchar_t)towlower(ch);
    for(int i=0;i<(int)itemAll.size();++i){
        if(itemAll[i].cat!=itemCat)continue;
        if(!f.empty()){std::wstring n=itemAll[i].name;for(auto& ch:n)ch=(wchar_t)towlower(ch);if(n.find(f)==std::wstring::npos)continue;}
        out.push_back(i);
    }
    return out;
}
wchar_t nameFirst[24]{},nameLast[24]{},nameNick[24]{};std::wstring nameStatus=L"Load current, edit, then Apply";
std::wstring havenStatus;ULONGLONG havenStatusUntil=0;   // Haven result line under the button: shown while working, then for 5 s (0 = hidden)
std::wstring resStatus=L"Set amount \u2192 Set",timerStatus=L"Adds or removes time on active missions (all)";
void commitNum(){
    if(editingNum<0)return;
    int v=0;for(const wchar_t* c=numDraft;*c;++c)v=v*10+(*c-L'0');
    if(editingNum>=0&&editingNum<RES_ROWS)resAmtVals[editingNum]=(std::max)(0,(std::min)(9999,v));
    else if(editingNum==100)timerVal=(std::max)(1,(std::min)(9999,v));
    else if(editingNum==101)itemCount=(std::max)(1,(std::min)(999,v));
    editingNum=-1;
}
void beginNum(int which){
    commitNum();editingNum=which;numDraft[0]=0;
}

// Opening the Weapons / Community tabs asks the game thread to rescan (weapon list, resources, every item blueprint). Those scans take
// tens to hundreds of ms and caused a hitch each time you switched tabs while moving, so they are throttled now.
// The Weapon Preview "Refresh" button still forces a weapon rescan.
void tabEnterRefresh(int cat){
    // Community tab: the Give Item list is read automatically the first time the tab is opened (one scan only; Set Resources keeps its
    // own Load button so the two scans never run back to back). The Load button next to the list stays as a manual Reload.
    static bool giveAutoDone=false;
    if(cat==2&&!giveAutoDone&&itemAll.empty()&&gameWindow){
        giveAutoDone=true;giveAutoStop=true;itemStatus=L"Loading item list...";
        PostMessageW(gameWindow,WM_ADREENA_COMM,10,0);
    }
}
// ===== Menu-wide search =====
struct SearchEntry{std::wstring label,where,hay;int cat,sec,row;};   // row -1 = whole section; sec -1 + cat 6 = Settings entry (row = y offset)
std::vector<SearchEntry> searchEntries;
bool searchOpen=false;
wchar_t searchBuf[32]{};
int searchScroll=0;
std::vector<int> searchResults;
int searchHLCat=-1,searchHLSec=-1,searchHLRow=-1;ULONGLONG searchHLStart=0;
const wchar_t* const sectionNameTable[6][7]={
    {L"Protection",L"Movement",L"Character / Clothing",L"Utilities",L"Skill Editor",L"Survivor Name"},
    {L"",L"Ammunition",L"Weapon Condition",L"Accuracy",L"Melee",L""},
    {L"Set Resources",L"Management",L"Mission Timer",L"Give Item",L"",L""},
    {L"Time",L"Teleport",L"Exploration",L"Advanced",L"",L""},
    {L"Vehicle",L"",L"",L"",L"Spawn Vehicle",L"Trunk Size"},
    {L"Zombie Spawner",L"Fun",L"",L"",L"",L"Hordes"}
};
std::wstring searchLower(const std::wstring& a){std::wstring r=a;for(auto& c:r)c=(wchar_t)towlower(c);return r;}
void searchBuildIndex(){
    if(!searchEntries.empty())return;
    auto add=[&](const std::wstring& label,int cat,int sec,int row,const wchar_t* extra){
        SearchEntry e;e.label=label;e.cat=cat;e.sec=sec;e.row=row;
        e.where=cat==6?L"Settings":categoryNames[cat];
        e.hay=searchLower(label+L" "+(extra?extra:L"")+L" "+e.where);
        searchEntries.push_back(e);
    };
    for(int cat=0;cat<6;cat++){
        const auto& fl=featureLists[cat];
        for(int k=0;k<(int)menuSections[cat].size()&&k<6;k++){
            if(sectionHeight(cat,k)<=0)continue;
            const wchar_t* nm=sectionNameTable[cat][k];
            if(nm&&nm[0]){
                const wchar_t* hint=nullptr;
                if(cat==3&&k==1)hint=L"bookmark position waypoint location save";
                else if(cat==2&&k==3)hint=L"spawn item add give weapon";
                else if(cat==0&&k==4)hint=L"skills level specialization";
                else if(cat==0&&k==2)hint=L"outfit hat clothes appearance voice gender";
                else if(cat==2&&k==0)hint=L"resources food ammo fuel meds materials";
                else if(cat==2&&k==2)hint=L"timer mission countdown";
                else if(cat==0&&k==5)hint=L"name rename nickname";
                else if(cat==4&&k==4)hint=L"car spawn bounty";
                else if(cat==5&&k==0)hint=L"zombies spawn";
                else if(cat==5&&k==4)hint=L"endless one-time ring population swarm surround";
                else if(cat==5&&k==5)hint=L"horde small medium massive clear zombies group unlimited spawning";
                else if(cat==5&&k==1)hint=L"haven protocol expose gravity";
                add(nm,cat,k,-1,hint);
            }
            if(sectionItemCount(cat,k)==1&&!(menuSections[cat][k].size()==1))continue;   // custom sections: their name is the entry
            int n=0;
            for(int ix:menuSections[cat][k]){
                if(ix<0||ix>=fl.count)continue;
                add(fl.items[ix].label,cat,k,n,nm);
                n++;
            }
        }
    }
    add(L"Position Bookmarks",3,1,-1,L"teleport save slots");
    add(L"Teleport Hotkey",3,1,-1,L"key bind");
    add(L"Outfit / Hat",0,2,-1,L"clothes");
    add(L"Voice",0,2,-1,L"");
    add(L"Background / Opacity",6,-1,36,L"color colour theme");
    add(L"Accent Colour",6,-1,68,L"color theme");
    add(L"Text Colour",6,-1,100,L"color theme");
    add(L"Reset Colors",6,-1,170,L"default");
    add(L"Banner",6,-1,198,L"image header");
    add(L"Profiles",6,-1,0,L"save load autoload json");
    add(L"Official Presets",6,-1,309,L"theme");
}
void searchRebuildResults(){
    searchBuildIndex();
    searchResults.clear();searchScroll=0;
    std::wstring q=searchLower(searchBuf);
    std::vector<std::wstring> words;
    {std::wstring w;for(wchar_t c:q){if(c==L' '){if(!w.empty())words.push_back(w);w.clear();}else w+=c;}if(!w.empty())words.push_back(w);}
    if(words.empty())return;
    std::vector<std::pair<int,int>> sc;
    for(int i=0;i<(int)searchEntries.size();i++){
        const SearchEntry& e=searchEntries[i];bool ok=true;
        for(auto& w:words)if(e.hay.find(w)==std::wstring::npos){ok=false;break;}
        if(!ok)continue;
        std::wstring lab=searchLower(e.label);
        int score=lab.compare(0,words[0].size(),words[0])==0?0:(lab.find(words[0])!=std::wstring::npos?1:2);
        sc.push_back({score,i});
    }
    std::stable_sort(sc.begin(),sc.end(),[](const std::pair<int,int>& a,const std::pair<int,int>& b){return a.first<b.first;});
    for(size_t i=0;i<sc.size()&&i<40;i++)searchResults.push_back(sc[i].second);
}
RECT searchIconRect(){return RECT{MW-40,HEADER_H+8,MW-14,HEADER_H+32};}
RECT searchFieldRect(){return RECT{MW-40-6-170,HEADER_H+8,MW-40-6,HEADER_H+32};}
int searchPopupRowsShown(){return searchResults.empty()?1:(std::min)(7,(int)searchResults.size());}
RECT searchPopupRect(){const int rows=searchPopupRowsShown();return RECT{MW-14-250,HEADER_H+35,MW-14,HEADER_H+35+rows*BANNER_ROW_HEIGHT+2};}
bool searchPopupVisible(){return searchOpen&&searchBuf[0];}
void searchClose(){searchOpen=false;if(editingNum==207)editingNum=-1;uiCacheValid=false;}
bool searchHLActive(){return searchHLCat>=0&&GetTickCount64()-searchHLStart<2600;}
bool searchHLBlinkOn(){return ((GetTickCount64()-searchHLStart)/260)%2==0;}
void searchGo(int ri){
    if(ri<0||ri>=(int)searchResults.size())return;
    const SearchEntry e=searchEntries[searchResults[ri]];
    activeCategory=e.cat;dropOpen=0;profDropOpen=false;bannerDropdownOpen=false;scrollOffset=0;
    tabEnterRefresh(e.cat);
    if(e.cat==6){
        const int v=(std::max)(0,(std::min)(settingsMaxScroll(),e.row-6));
        settingsScroll=v;settingsScrollTarget=(float)v;
        searchHLCat=6;searchHLSec=-1;searchHLRow=e.row;
    }else{
        settingsScroll=0;
        pageScrollTarget[e.cat]=(float)(std::max)(0,(std::min)(pageMaxScroll(),sectionTopOffset(e.cat,e.sec)));
        if(e.row>=3)sectionScrollTarget[e.cat][e.sec]=(std::max)(0.f,(std::min)(sectionMaxScroll(e.cat,e.sec),(float)(e.row-1)));
        else sectionScrollTarget[e.cat][e.sec]=0.f;
        searchHLCat=e.cat;searchHLSec=e.sec;searchHLRow=e.row;
    }
    searchHLStart=GetTickCount64();
    searchClose();
}
// Returns true when the click belonged to the search UI.
bool searchClick(int lx,int ly){
    auto in=[&](const RECT& r){return lx>=r.left&&lx<r.right&&ly>=r.top&&ly<r.bottom;};
    if(in(searchIconRect())){
        if(searchOpen)searchClose();
        else{commitNum();searchOpen=true;searchBuf[0]=0;searchResults.clear();searchScroll=0;editingNum=207;editingHex=-1;pickerColor=-1;dropOpen=0;profDropOpen=false;bannerDropdownOpen=false;uiCacheValid=false;}
        return true;
    }
    if(!searchOpen)return false;
    if(in(searchFieldRect())){editingNum=207;uiCacheValid=false;return true;}
    if(searchPopupVisible()){
        const RECT pr=searchPopupRect();
        if(in(pr)){
            if(!searchResults.empty()){
                const int row=searchScroll+(ly-pr.top-1)/BANNER_ROW_HEIGHT;
                searchGo(row);
            }
            return true;
        }
    }
    searchClose();
    return false;
}
std::atomic<int> teleHotVk{0};          // teleport hotkey: virtual-key code (0 = none)
std::atomic<int> teleHotMods{0};        // 1 = Ctrl, 2 = Shift, 4 = Alt
bool teleHotCapturing=false;            // waiting for the next key press (guarded by menuLock)
int currentKeyMods(){return ((GetKeyState(VK_CONTROL)&0x8000)?1:0)|((GetKeyState(VK_SHIFT)&0x8000)?2:0)|((GetKeyState(VK_MENU)&0x8000)?4:0);}
std::wstring hotkeyName(int vk,int mods){
    std::wstring n;if(mods&1)n+=L"Ctrl+";if(mods&2)n+=L"Shift+";if(mods&4)n+=L"Alt+";
    LONG sc=(LONG)MapVirtualKeyW((UINT)vk,MAPVK_VK_TO_VSC)<<16;
    switch(vk){case VK_INSERT:case VK_DELETE:case VK_HOME:case VK_END:case VK_PRIOR:case VK_NEXT:case VK_LEFT:case VK_RIGHT:case VK_UP:case VK_DOWN:case VK_DIVIDE:case VK_NUMLOCK:sc|=1<<24;break;default:break;}
    wchar_t buf[64]{};
    if(GetKeyNameTextW(sc,buf,64)>0)n+=buf;else{wchar_t t[16];swprintf(t,16,L"Key %d",vk);n+=t;}
    return n;
}
int hatOffset=0,bodyOffset=0;        // net hat / outfit steps from where the menu started tracking (guarded by menuLock)
constexpr UINT WM_ADREENA_SKDUMP=WM_APP+0x2AC;   // read-only diagnostic: log how the player's survivor stores skills (game thread)
// Skill Editor (guarded by menuLock)
struct SkillRow{std::wstring name;int level=0,max=7;bool base=true;};
std::vector<SkillRow> skillRows;
constexpr UINT WM_ADREENA_SKREFRESH=WM_APP+0x2AD;   // read the player's skills (game thread)
constexpr UINT WM_ADREENA_SKREM=WM_APP+0x2C0;       // wParam: row index (remove that skill / specialization) (game thread)
std::wstring skillArmedName;                        // row waiting for a second click on its X (guarded by menuLock)
constexpr UINT WM_ADREENA_SKSET=WM_APP+0x2AE;       // wParam: row index, lParam: target level (game thread)
int skillVisibleIndex(int r){   // r-th row shown in the current tab (Base / Specialized), or -1
    int n=0;
    for(size_t i=0;i<skillRows.size();++i){if(skillRows[i].base!=(skillEditorMode==0))continue;if(n==r)return (int)i;++n;}
    return -1;
}
constexpr UINT WM_ADREENA_ZMASK=WM_APP+0x2A9;   // re-check which zombie classes are loaded (game thread)
constexpr UINT WM_ADREENA_ZLOAD=WM_APP+0x2AA;   // wParam: zombie index; ask the game to load its class
constexpr UINT WM_ADREENA_VEHICLE=WM_APP+0x2A2; // wParam: 1 = refuel, 2 = repair
std::wstring vehicleStatus=L"Ready";       // guarded by menuLock

// ---- Vehicle spawner / bounty delivery data (from the Hades menu's lists) ----
constexpr UINT WM_ADREENA_VSPAWN=WM_APP+0x2A3;  // wParam: vehicle index, lParam: 1 = register as owned vehicle
constexpr UINT WM_ADREENA_VCHECK=WM_APP+0x2A5;  // wParam: spawned actor pointer, lParam: its object index (delayed re-check)
constexpr UINT WM_ADREENA_BOUNTY=WM_APP+0x2A4;  // wParam: bounty vehicle index
struct VehicleDef{const wchar_t* variant;const char* pkg;const char* cls;};
struct VehicleFamily{const wchar_t* name;int first;int count;};
const VehicleDef vehicleDefs[]={
    {L"GolfCart DLC2","/Game/Art/Driveables/GolfCart_DLC2/GolfCart_DLC2","GolfCart_DLC2_C"},
    {L"Muscle Apoc Jugg 01","/Game/Art/Driveables/Muscle_Apoc_Jugg_01/Muscle_Apoc_Jugg_01","Muscle_Apoc_Jugg_01_C"},
    {L"Standard","/Game/Art/Driveables/4wheeler_Apoc/Vehicle_4wheeler_Apoc","Vehicle_4wheeler_Apoc_C"},
    {L"Bayonette DLC1","/Game/Art/Driveables/4wheeler_Apoc/Vehicle_4wheeler_Apoc_Bayonette_DLC1","Vehicle_4wheeler_Apoc_Bayonette_DLC1_C"},
    {L"grey","/Game/Art/Driveables/4wheeler_Apoc/Vehicle_4wheeler_Apoc_grey","Vehicle_4wheeler_Apoc_grey_C"},
    {L"orange","/Game/Art/Driveables/4wheeler_Apoc/Vehicle_4wheeler_Apoc_orange","Vehicle_4wheeler_Apoc_orange_C"},
    {L"tan","/Game/Art/Driveables/4wheeler_Apoc/Vehicle_4wheeler_Apoc_tan","Vehicle_4wheeler_Apoc_tan_C"},
    {L"white","/Game/Art/Driveables/4wheeler_Apoc/Vehicle_4wheeler_Apoc_white","Vehicle_4wheeler_Apoc_white_C"},
    {L"Standard","/Game/Art/Driveables/4wheeler_Basic/Vehicle_4wheeler_Basic","Vehicle_4wheeler_Basic_C"},
    {L"black","/Game/Art/Driveables/4wheeler_Basic/Vehicle_4wheeler_Basic_black","Vehicle_4wheeler_Basic_black_C"},
    {L"green","/Game/Art/Driveables/4wheeler_Basic/Vehicle_4wheeler_Basic_green","Vehicle_4wheeler_Basic_green_C"},
    {L"grey","/Game/Art/Driveables/4wheeler_Basic/Vehicle_4wheeler_Basic_grey","Vehicle_4wheeler_Basic_grey_C"},
    {L"white","/Game/Art/Driveables/4wheeler_Basic/Vehicle_4wheeler_Basic_white","Vehicle_4wheeler_Basic_white_C"},
    {L"OffraderJ RetailerPI Package3","/Game/Art/Driveables/4wheeler_Basic/Vehicle_OffraderJ_RetailerPI_Package3","Vehicle_OffraderJ_RetailerPI_Package3_C"},
    {L"Standard","/Game/Art/Driveables/4wheeler_Sport/Vehicle_4wheeler_Sport","Vehicle_4wheeler_Sport_C"},
    {L"black","/Game/Art/Driveables/4wheeler_Sport/Vehicle_4wheeler_Sport_black","Vehicle_4wheeler_Sport_black_C"},
    {L"green","/Game/Art/Driveables/4wheeler_Sport/Vehicle_4wheeler_Sport_green","Vehicle_4wheeler_Sport_green_C"},
    {L"red","/Game/Art/Driveables/4wheeler_Sport/Vehicle_4wheeler_Sport_red","Vehicle_4wheeler_Sport_red_C"},
    {L"teal","/Game/Art/Driveables/4wheeler_Sport/Vehicle_4wheeler_Sport_teal","Vehicle_4wheeler_Sport_teal_C"},
    {L"Standard","/Game/Art/Driveables/Hatchback_Apoc/Vehicle_Hatchback_Apoc","Vehicle_Hatchback_Apoc_C"},
    {L"blue","/Game/Art/Driveables/Hatchback_Apoc/Vehicle_Hatchback_Apoc_blue","Vehicle_Hatchback_Apoc_blue_C"},
    {L"green","/Game/Art/Driveables/Hatchback_Apoc/Vehicle_Hatchback_Apoc_green","Vehicle_Hatchback_Apoc_green_C"},
    {L"grey","/Game/Art/Driveables/Hatchback_Apoc/Vehicle_Hatchback_Apoc_grey","Vehicle_Hatchback_Apoc_grey_C"},
    {L"yellow","/Game/Art/Driveables/Hatchback_Apoc/Vehicle_Hatchback_Apoc_yellow","Vehicle_Hatchback_Apoc_yellow_C"},
    {L"Hatchback Apoc","/Game/Art/Driveables/Hatchback_Apoc_P40_DLC1/Vehicle_Hatchback_Apoc","Vehicle_Hatchback_Apoc_C"},
    {L"Standard","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_Basic","Vehicle_Hatchback_Basic_C"},
    {L"black","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_Basic_black","Vehicle_Hatchback_Basic_black_C"},
    {L"green","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_Basic_green","Vehicle_Hatchback_Basic_green_C"},
    {L"grey","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_Basic_grey","Vehicle_Hatchback_Basic_grey_C"},
    {L"orange","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_Basic_orange","Vehicle_Hatchback_Basic_orange_C"},
    {L"Hatchback Retailer Anime Def","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_Retailer_Anime_Def","Vehicle_Hatchback_Retailer_Anime_Def_C"},
    {L"Hatchback RetailerPI Package1","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_RetailerPI_Package1","Vehicle_Hatchback_RetailerPI_Package1_C"},
    {L"Hatchback RetailerPI Package8","/Game/Art/Driveables/Hatchback_Basic/Vehicle_Hatchback_RetailerPI_Package8","Vehicle_Hatchback_RetailerPI_Package8_C"},
    {L"Standard","/Game/Art/Driveables/Hatchback_RoadTrip/Vehicle_Hatchback_RoadTrip","Vehicle_Hatchback_RoadTrip_C"},
    {L"gold","/Game/Art/Driveables/Hatchback_RoadTrip/Vehicle_Hatchback_RoadTrip_gold","Vehicle_Hatchback_RoadTrip_gold_C"},
    {L"mint","/Game/Art/Driveables/Hatchback_RoadTrip/Vehicle_Hatchback_RoadTrip_mint","Vehicle_Hatchback_RoadTrip_mint_C"},
    {L"red","/Game/Art/Driveables/Hatchback_RoadTrip/Vehicle_Hatchback_RoadTrip_red","Vehicle_Hatchback_RoadTrip_red_C"},
    {L"teal","/Game/Art/Driveables/Hatchback_RoadTrip/Vehicle_Hatchback_RoadTrip_teal","Vehicle_Hatchback_RoadTrip_teal_C"},
    {L"Standard","/Game/Art/Driveables/Hatchback_Scout/Vehicle_Hatchback_Scout","Vehicle_Hatchback_Scout_C"},
    {L"black","/Game/Art/Driveables/Hatchback_Scout/Vehicle_Hatchback_Scout_black","Vehicle_Hatchback_Scout_black_C"},
    {L"green","/Game/Art/Driveables/Hatchback_Scout/Vehicle_Hatchback_Scout_green","Vehicle_Hatchback_Scout_green_C"},
    {L"grey","/Game/Art/Driveables/Hatchback_Scout/Vehicle_Hatchback_Scout_grey","Vehicle_Hatchback_Scout_grey_C"},
    {L"orange","/Game/Art/Driveables/Hatchback_Sport/Vehicle_Hatchback_Sport_orange","Vehicle_Hatchback_Sport_orange_C"},
    {L"Standard","/Game/Art/Driveables/Hatchback_Sport/Vehicle_Hatchback_Sport","Vehicle_Hatchback_Sport_C"},
    {L"blue","/Game/Art/Driveables/Hatchback_Sport/Vehicle_Hatchback_Sport_blue","Vehicle_Hatchback_Sport_blue_C"},
    {L"grey","/Game/Art/Driveables/Hatchback_Sport/Vehicle_Hatchback_Sport_grey","Vehicle_Hatchback_Sport_grey_C"},
    {L"orange","/Game/Art/Driveables/Hatchback_Sport/Vehicle_Hatchback_Sport_orange","Vehicle_Hatchback_Sport_orange_C"},
    {L"white","/Game/Art/Driveables/Hatchback_Sport/Vehicle_Hatchback_Sport_white","Vehicle_Hatchback_Sport_white_C"},
    {L"Standard","/Game/Art/Driveables/Muscle_Apoc_SoT_Meg_01/Vehicle_Muscle_Apoc_SoT_Meg_01","Vehicle_Muscle_Apoc_SoT_Meg_01_C"},
    {L"Standard","/Game/Art/Driveables/Muscle_Basic/Vehicle_Muscle_Basic","Vehicle_Muscle_Basic_C"},
    {L"black","/Game/Art/Driveables/Muscle_Basic/Vehicle_Muscle_Basic_black","Vehicle_Muscle_Basic_black_C"},
    {L"blue","/Game/Art/Driveables/Muscle_Basic/Vehicle_Muscle_Basic_blue","Vehicle_Muscle_Basic_blue_C"},
    {L"orange","/Game/Art/Driveables/Muscle_Basic/Vehicle_Muscle_Basic_orange","Vehicle_Muscle_Basic_orange_C"},
    {L"white","/Game/Art/Driveables/Muscle_Basic/Vehicle_Muscle_Basic_white","Vehicle_Muscle_Basic_white_C"},
    {L"Standard","/Game/Art/Driveables/Muscle_Maximized/Vehicle_Muscle_Maximized","Vehicle_Muscle_Maximized_C"},
    {L"black","/Game/Art/Driveables/Muscle_Maximized/Vehicle_Muscle_Maximized_black","Vehicle_Muscle_Maximized_black_C"},
    {L"blue","/Game/Art/Driveables/Muscle_Maximized/Vehicle_Muscle_Maximized_blue","Vehicle_Muscle_Maximized_blue_C"},
    {L"orange","/Game/Art/Driveables/Muscle_Maximized/Vehicle_Muscle_Maximized_orange","Vehicle_Muscle_Maximized_orange_C"},
    {L"purple","/Game/Art/Driveables/Muscle_Maximized/Vehicle_Muscle_Maximized_purple","Vehicle_Muscle_Maximized_purple_C"},
    {L"Standard","/Game/Art/Driveables/RV_Basic/Vehicle_RV_basic","Vehicle_RV_basic_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Iconic_Apoc/Vehicle_Sedan_Iconic_Apoc","Vehicle_Sedan_Iconic_Apoc_C"},
    {L"blue","/Game/Art/Driveables/Sedan_Iconic_Apoc/Vehicle_Sedan_Iconic_Apoc_blue","Vehicle_Sedan_Iconic_Apoc_blue_C"},
    {L"brown","/Game/Art/Driveables/Sedan_Iconic_Apoc/Vehicle_Sedan_Iconic_Apoc_brown","Vehicle_Sedan_Iconic_Apoc_brown_C"},
    {L"green","/Game/Art/Driveables/Sedan_Iconic_Apoc/Vehicle_Sedan_Iconic_Apoc_green","Vehicle_Sedan_Iconic_Apoc_green_C"},
    {L"grey","/Game/Art/Driveables/Sedan_Iconic_Apoc/Vehicle_Sedan_Iconic_Apoc_grey","Vehicle_Sedan_Iconic_Apoc_grey_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Iconic_Basic/Vehicle_Sedan_Iconic_Basic","Vehicle_Sedan_Iconic_Basic_C"},
    {L"beige","/Game/Art/Driveables/Sedan_Iconic_Basic/Vehicle_Sedan_Iconic_Basic_beige","Vehicle_Sedan_Iconic_Basic_beige_C"},
    {L"black","/Game/Art/Driveables/Sedan_Iconic_Basic/Vehicle_Sedan_Iconic_Basic_black","Vehicle_Sedan_Iconic_Basic_black_C"},
    {L"blue","/Game/Art/Driveables/Sedan_Iconic_Basic/Vehicle_Sedan_Iconic_Basic_blue","Vehicle_Sedan_Iconic_Basic_blue_C"},
    {L"grey","/Game/Art/Driveables/Sedan_Iconic_Basic/Vehicle_Sedan_Iconic_Basic_grey","Vehicle_Sedan_Iconic_Basic_grey_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Iconic_Police/Vehicle_Sedan_Iconic_Police","Vehicle_Sedan_Iconic_Police_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Iconic_Taxi/Vehicle_Sedan_Iconic_Taxi","Vehicle_Sedan_Iconic_Taxi_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Modern_Apoc/Vehicle_Sedan_Modern_Apoc","Vehicle_Sedan_Modern_Apoc_C"},
    {L"blue","/Game/Art/Driveables/Sedan_Modern_Apoc/Vehicle_Sedan_Modern_Apoc_blue","Vehicle_Sedan_Modern_Apoc_blue_C"},
    {L"green","/Game/Art/Driveables/Sedan_Modern_Apoc/Vehicle_Sedan_Modern_Apoc_green","Vehicle_Sedan_Modern_Apoc_green_C"},
    {L"white","/Game/Art/Driveables/Sedan_Modern_Apoc/Vehicle_Sedan_Modern_Apoc_white","Vehicle_Sedan_Modern_Apoc_white_C"},
    {L"yellow","/Game/Art/Driveables/Sedan_Modern_Apoc/Vehicle_Sedan_Modern_Apoc_yellow","Vehicle_Sedan_Modern_Apoc_yellow_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_Basic","Vehicle_Sedan_Modern_Basic_C"},
    {L"black","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_Basic_black","Vehicle_Sedan_Modern_Basic_black_C"},
    {L"green","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_Basic_green","Vehicle_Sedan_Modern_Basic_green_C"},
    {L"grey","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_Basic_grey","Vehicle_Sedan_Modern_Basic_grey_C"},
    {L"silver","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_Basic_silver","Vehicle_Sedan_Modern_Basic_silver_C"},
    {L"Sedan Modern RetailerPI Package2","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_RetailerPI_Package2","Vehicle_Sedan_Modern_RetailerPI_Package2_C"},
    {L"Sedan Modern RetailerPI Package7","/Game/Art/Driveables/Sedan_Modern_Basic/Vehicle_Sedan_Modern_RetailerPI_Package7","Vehicle_Sedan_Modern_RetailerPI_Package7_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Modern_Sport/Vehicle_Sedan_Modern_Sport","Vehicle_Sedan_Modern_Sport_C"},
    {L"black","/Game/Art/Driveables/Sedan_Modern_Sport/Vehicle_Sedan_Modern_Sport_black","Vehicle_Sedan_Modern_Sport_black_C"},
    {L"orange","/Game/Art/Driveables/Sedan_Modern_Sport/Vehicle_Sedan_Modern_Sport_orange","Vehicle_Sedan_Modern_Sport_orange_C"},
    {L"red","/Game/Art/Driveables/Sedan_Modern_Sport/Vehicle_Sedan_Modern_Sport_red","Vehicle_Sedan_Modern_Sport_red_C"},
    {L"silver","/Game/Art/Driveables/Sedan_Modern_Sport/Vehicle_Sedan_Modern_Sport_silver","Vehicle_Sedan_Modern_Sport_silver_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Old_Apoc/Vehicle_Sedan_Old_Apoc","Vehicle_Sedan_Old_Apoc_C"},
    {L"blue","/Game/Art/Driveables/Sedan_Old_Apoc/Vehicle_Sedan_Old_Apoc_blue","Vehicle_Sedan_Old_Apoc_blue_C"},
    {L"gold","/Game/Art/Driveables/Sedan_Old_Apoc/Vehicle_Sedan_Old_Apoc_gold","Vehicle_Sedan_Old_Apoc_gold_C"},
    {L"green","/Game/Art/Driveables/Sedan_Old_Apoc/Vehicle_Sedan_Old_Apoc_green","Vehicle_Sedan_Old_Apoc_green_C"},
    {L"PI","/Game/Art/Driveables/Sedan_Old_Apoc/Vehicle_Sedan_Old_Apoc_PI","Vehicle_Sedan_Old_Apoc_PI_C"},
    {L"white","/Game/Art/Driveables/Sedan_Old_Apoc/Vehicle_Sedan_Old_Apoc_white","Vehicle_Sedan_Old_Apoc_white_C"},
    {L"Standard","/Game/Art/Driveables/Sedan_Old_Basic/Vehicle_Sedan_Old_Basic","Vehicle_Sedan_Old_Basic_C"},
    {L"blue","/Game/Art/Driveables/Sedan_Old_Basic/Vehicle_Sedan_Old_Basic_blue","Vehicle_Sedan_Old_Basic_blue_C"},
    {L"gold","/Game/Art/Driveables/Sedan_Old_Basic/Vehicle_Sedan_Old_Basic_gold","Vehicle_Sedan_Old_Basic_gold_C"},
    {L"green","/Game/Art/Driveables/Sedan_Old_Basic/Vehicle_Sedan_Old_Basic_green","Vehicle_Sedan_Old_Basic_green_C"},
    {L"white","/Game/Art/Driveables/Sedan_Old_Basic/Vehicle_Sedan_Old_Basic_white","Vehicle_Sedan_Old_Basic_white_C"},
    {L"Small Plane Basic","/Game/Art/Driveables/Plane_SmallProp/Vehicle_Small_Plane_Basic","Vehicle_Small_Plane_Basic_C"},
    {L"Standard","/Game/Art/Driveables/Sport_Coupe_Apoc/Vehicle_sport_coupe_apoc","Vehicle_sport_coupe_apoc_C"},
    {L"black","/Game/Art/Driveables/Sport_Coupe_Apoc/Vehicle_sport_coupe_apoc_black","Vehicle_sport_coupe_apoc_black_C"},
    {L"blue","/Game/Art/Driveables/Sport_Coupe_Apoc/Vehicle_sport_coupe_apoc_blue","Vehicle_sport_coupe_apoc_blue_C"},
    {L"rust","/Game/Art/Driveables/Sport_Coupe_Apoc/Vehicle_sport_coupe_apoc_rust","Vehicle_sport_coupe_apoc_rust_C"},
    {L"white","/Game/Art/Driveables/Sport_Coupe_Apoc/Vehicle_sport_coupe_apoc_white","Vehicle_sport_coupe_apoc_white_C"},
    {L"Standard","/Game/Art/Driveables/Sport_Coupe_Basic/Vehicle_sport_coupe_basic","Vehicle_sport_coupe_basic_C"},
    {L"black","/Game/Art/Driveables/Sport_Coupe_Basic/Vehicle_sport_coupe_basic_black","Vehicle_sport_coupe_basic_black_C"},
    {L"grey","/Game/Art/Driveables/Sport_Coupe_Basic/Vehicle_sport_coupe_basic_grey","Vehicle_sport_coupe_basic_grey_C"},
    {L"red","/Game/Art/Driveables/Sport_Coupe_Basic/Vehicle_sport_coupe_basic_red","Vehicle_sport_coupe_basic_red_C"},
    {L"white","/Game/Art/Driveables/Sport_Coupe_Basic/Vehicle_sport_coupe_basic_white","Vehicle_sport_coupe_basic_white_C"},
    {L"Standard","/Game/Art/Driveables/Sport_Coupe_Racer/Vehicle_sport_coupe_racer","Vehicle_sport_coupe_racer_C"},
    {L"black","/Game/Art/Driveables/Sport_Coupe_Racer/Vehicle_sport_coupe_racer_black","Vehicle_sport_coupe_racer_black_C"},
    {L"blue","/Game/Art/Driveables/Sport_Coupe_Racer/Vehicle_sport_coupe_racer_blue","Vehicle_sport_coupe_racer_blue_C"},
    {L"silver","/Game/Art/Driveables/Sport_Coupe_Racer/Vehicle_sport_coupe_racer_silver","Vehicle_sport_coupe_racer_silver_C"},
    {L"yellow","/Game/Art/Driveables/Sport_Coupe_Racer/Vehicle_sport_coupe_racer_yellow","Vehicle_sport_coupe_racer_yellow_C"},
    {L"Standard","/Game/Art/Driveables/SUV_Modern_Apoc/Vehicle_SUV_Modern_apoc","Vehicle_SUV_Modern_apoc_C"},
    {L"blue","/Game/Art/Driveables/SUV_Modern_Apoc/Vehicle_SUV_Modern_Apoc_blue","Vehicle_SUV_Modern_Apoc_blue_C"},
    {L"green","/Game/Art/Driveables/SUV_Modern_Apoc/Vehicle_SUV_Modern_Apoc_green","Vehicle_SUV_Modern_Apoc_green_C"},
    {L"grey","/Game/Art/Driveables/SUV_Modern_Apoc/Vehicle_SUV_Modern_Apoc_grey","Vehicle_SUV_Modern_Apoc_grey_C"},
    {L"tan","/Game/Art/Driveables/SUV_Modern_Apoc/Vehicle_SUV_Modern_Apoc_tan","Vehicle_SUV_Modern_Apoc_tan_C"},
    {L"Standard","/Game/Art/Driveables/SUV_Modern_Basic/Vehicle_SUV_Modern_Basic","Vehicle_SUV_Modern_Basic_C"},
    {L"black","/Game/Art/Driveables/SUV_Modern_Basic/Vehicle_SUV_Modern_Basic_black","Vehicle_SUV_Modern_Basic_black_C"},
    {L"blue","/Game/Art/Driveables/SUV_Modern_Basic/Vehicle_SUV_Modern_Basic_blue","Vehicle_SUV_Modern_Basic_blue_C"},
    {L"red","/Game/Art/Driveables/SUV_Modern_Basic/Vehicle_SUV_Modern_Basic_red","Vehicle_SUV_Modern_Basic_red_C"},
    {L"RetailerPI Package6","/Game/Art/Driveables/SUV_Modern_Basic/Vehicle_SUV_Modern_Basic_RetailerPI_Package6","Vehicle_SUV_Modern_Basic_RetailerPI_Package6_C"},
    {L"white","/Game/Art/Driveables/SUV_Modern_Basic/Vehicle_SUV_Modern_Basic_white","Vehicle_SUV_Modern_Basic_white_C"},
    {L"SUV Modern Military","/Game/Art/Driveables/SUV_Modern_RedTalon/Vehicle_SUV_Modern_Military","Vehicle_SUV_Modern_Military_C"},
    {L"Standard","/Game/Art/Driveables/SUV_Modern_RedTalon/Vehicle_SUV_Modern_RedTalon","Vehicle_SUV_Modern_RedTalon_C"},
    {L"Standard","/Game/Art/Driveables/SUV_Modern_Police/Vehicle_SUV_Modern_Police","Vehicle_SUV_Modern_Police_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Classic_Apoc/Vehicle_Truck_Classic_Apoc","Vehicle_Truck_Classic_Apoc_C"},
    {L"black","/Game/Art/Driveables/Truck_Classic_Apoc/Vehicle_Truck_Classic_Apoc_black","Vehicle_Truck_Classic_Apoc_black_C"},
    {L"blue","/Game/Art/Driveables/Truck_Classic_Apoc/Vehicle_Truck_Classic_Apoc_blue","Vehicle_Truck_Classic_Apoc_blue_C"},
    {L"red","/Game/Art/Driveables/Truck_Classic_Apoc/Vehicle_Truck_Classic_Apoc_red","Vehicle_Truck_Classic_Apoc_red_C"},
    {L"Resto black","/Game/Art/Driveables/Truck_Classic_Apoc/Vehicle_Truck_Classic_Apoc_Resto_black","Vehicle_Truck_Classic_Apoc_Resto_black_C"},
    {L"yellow","/Game/Art/Driveables/Truck_Classic_Apoc/Vehicle_Truck_Classic_Apoc_yellow","Vehicle_Truck_Classic_Apoc_yellow_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Classic_Basic/Vehicle_Truck_Classic_Basic","Vehicle_Truck_Classic_Basic_C"},
    {L"green","/Game/Art/Driveables/Truck_Classic_Basic/Vehicle_Truck_Classic_Basic_green","Vehicle_Truck_Classic_Basic_green_C"},
    {L"grey","/Game/Art/Driveables/Truck_Classic_Basic/Vehicle_Truck_Classic_Basic_grey","Vehicle_Truck_Classic_Basic_grey_C"},
    {L"red","/Game/Art/Driveables/Truck_Classic_Basic/Vehicle_Truck_Classic_Basic_red","Vehicle_Truck_Classic_Basic_red_C"},
    {L"yellow","/Game/Art/Driveables/Truck_Classic_Basic/Vehicle_Truck_Classic_Basic_yellow","Vehicle_Truck_Classic_Basic_yellow_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Military_Apoc/Vehicle_Truck_Military_Apoc","Vehicle_Truck_Military_Apoc_C"},
    {L"blue","/Game/Art/Driveables/Truck_Military_Apoc/Vehicle_Truck_Military_Apoc_blue","Vehicle_Truck_Military_Apoc_blue_C"},
    {L"grey","/Game/Art/Driveables/Truck_Military_Apoc/Vehicle_Truck_Military_Apoc_grey","Vehicle_Truck_Military_Apoc_grey_C"},
    {L"tan","/Game/Art/Driveables/Truck_Military_Apoc/Vehicle_Truck_Military_Apoc_tan","Vehicle_Truck_Military_Apoc_tan_C"},
    {L"yellow","/Game/Art/Driveables/Truck_Military_Apoc/Vehicle_Truck_Military_Apoc_yellow","Vehicle_Truck_Military_Apoc_yellow_C"},
    {L"Basic","/Game/Art/Driveables/Truck_Military/Vehicle_Truck_Military_Basic","Vehicle_Truck_Military_Basic_C"},
    {L"Basic Black","/Game/Art/Driveables/Truck_Military/Vehicle_Truck_Military_Basic_Black","Vehicle_Truck_Military_Basic_Black_C"},
    {L"Basic Grey","/Game/Art/Driveables/Truck_Military/Vehicle_Truck_Military_Basic_Grey","Vehicle_Truck_Military_Basic_Grey_C"},
    {L"Basic Tan","/Game/Art/Driveables/Truck_Military/Vehicle_Truck_Military_Basic_Tan","Vehicle_Truck_Military_Basic_Tan_C"},
    {L"Basic White","/Game/Art/Driveables/Truck_Military/Vehicle_Truck_Military_Basic_White","Vehicle_Truck_Military_Basic_White_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Modern_Apoc/Vehicle_Truck_Modern_Apoc","Vehicle_Truck_Modern_Apoc_C"},
    {L"black","/Game/Art/Driveables/Truck_Modern_Apoc/Vehicle_Truck_Modern_Apoc_black","Vehicle_Truck_Modern_Apoc_black_C"},
    {L"green","/Game/Art/Driveables/Truck_Modern_Apoc/Vehicle_Truck_Modern_Apoc_green","Vehicle_Truck_Modern_Apoc_green_C"},
    {L"tan","/Game/Art/Driveables/Truck_Modern_Apoc/Vehicle_Truck_Modern_Apoc_tan","Vehicle_Truck_Modern_Apoc_tan_C"},
    {L"white","/Game/Art/Driveables/Truck_Modern_Apoc/Vehicle_Truck_Modern_Apoc_white","Vehicle_Truck_Modern_Apoc_white_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Modern_Basic/Vehicle_Truck_Modern_Basic","Vehicle_Truck_Modern_Basic_C"},
    {L"grey","/Game/Art/Driveables/Truck_Modern_Basic/Vehicle_Truck_Modern_Basic_grey","Vehicle_Truck_Modern_Basic_grey_C"},
    {L"red","/Game/Art/Driveables/Truck_Modern_Basic/Vehicle_Truck_Modern_Basic_red","Vehicle_Truck_Modern_Basic_red_C"},
    {L"RetailerPI Package5","/Game/Art/Driveables/Truck_Modern_Basic/Vehicle_Truck_Modern_Basic_RetailerPI_Package5","Vehicle_Truck_Modern_Basic_RetailerPI_Package5_C"},
    {L"white","/Game/Art/Driveables/Truck_Modern_Basic/Vehicle_Truck_Modern_Basic_white","Vehicle_Truck_Modern_Basic_white_C"},
    {L"yellow","/Game/Art/Driveables/Truck_Modern_Basic/Vehicle_Truck_Modern_Basic_yellow","Vehicle_Truck_Modern_Basic_yellow_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Modern_MidSize/Vehicle_Truck_Modern_Midsize","Vehicle_Truck_Modern_Midsize_C"},
    {L"Black","/Game/Art/Driveables/Truck_Modern_MidSize/Vehicle_Truck_Modern_Midsize_Black","Vehicle_Truck_Modern_Midsize_Black_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Modern_MidSize_DLC1/Vehicle_Truck_Modern_Midsize_DLC1","Vehicle_Truck_Modern_Midsize_DLC1_C"},
    {L"Standard","/Game/Art/Driveables/Truck_Modern_Utility/Vehicle_Truck_Modern_Utility","Vehicle_Truck_Modern_Utility_C"},
    {L"Standard","/Game/Art/Driveables/Van_Ambulance/Vehicle_van_ambulance","Vehicle_van_ambulance_C"},
    {L"Standard","/Game/Art/Driveables/Van_Apoc/Vehicle_van_apoc","Vehicle_van_apoc_C"},
    {L"blue","/Game/Art/Driveables/Van_Apoc/vehicle_van_apoc_blue","vehicle_van_apoc_blue_C"},
    {L"green","/Game/Art/Driveables/Van_Apoc/vehicle_van_apoc_green","vehicle_van_apoc_green_C"},
    {L"grey","/Game/Art/Driveables/Van_Apoc/vehicle_van_apoc_grey","vehicle_van_apoc_grey_C"},
    {L"rust","/Game/Art/Driveables/Van_Apoc/vehicle_van_apoc_rust","vehicle_van_apoc_rust_C"},
    {L"Standard","/Game/Art/Driveables/Van_Cargo/Vehicle_van_cargo","Vehicle_van_cargo_C"},
    {L"black","/Game/Art/Driveables/Van_Cargo/Vehicle_van_cargo_black","Vehicle_van_cargo_black_C"},
    {L"blue","/Game/Art/Driveables/Van_Cargo/Vehicle_van_cargo_blue","Vehicle_van_cargo_blue_C"},
    {L"brown","/Game/Art/Driveables/Van_Cargo/Vehicle_van_cargo_brown","Vehicle_van_cargo_brown_C"},
    {L"red","/Game/Art/Driveables/Van_Cargo/Vehicle_van_cargo_red","Vehicle_van_cargo_red_C"},
    {L"RetailerPI Package4","/Game/Art/Driveables/Van_Cargo/Vehicle_van_cargo_RetailerPI_Package4","Vehicle_van_cargo_RetailerPI_Package4_C"},
    {L"Standard","/Game/Art/Driveables/Van_Cargo_Meat_DLC1/Vehicle_van_cargo_meat_DLC1","Vehicle_van_cargo_meat_DLC1_C"},
    {L"Standard","/Game/Art/Driveables/Van_Handyman/Vehicle_van_handyman","Vehicle_van_handyman_C"},
    {L"Standard","/Game/Art/Driveables/Van_News/Vehicle_van_news","Vehicle_van_news_C"},
    {L"Standard","/Game/Art/Driveables/Van_Passenger/Vehicle_van_passenger","Vehicle_van_passenger_C"},
    {L"beige","/Game/Art/Driveables/Van_Passenger/Vehicle_van_passenger_beige","Vehicle_van_passenger_beige_C"},
    {L"black","/Game/Art/Driveables/Van_Passenger/Vehicle_van_passenger_black","Vehicle_van_passenger_black_C"},
    {L"green","/Game/Art/Driveables/Van_Passenger/Vehicle_van_passenger_green","Vehicle_van_passenger_green_C"},
    {L"white","/Game/Art/Driveables/Van_Passenger/Vehicle_van_passenger_white","Vehicle_van_passenger_white_C"},
};
const VehicleFamily vehicleFamilies[]={
    {L"GolfCart DLC2",0,1},
    {L"Muscle Apoc Jugg 01",1,1},
    {L"4wheeler Apoc",2,6},
    {L"4wheeler Basic",8,6},
    {L"4wheeler Sport",14,5},
    {L"Hatchback Apoc",19,5},
    {L"Hatchback Apoc P40 DLC1",24,1},
    {L"Hatchback Basic",25,8},
    {L"Hatchback RoadTrip",33,5},
    {L"Hatchback Scout",38,4},
    {L"Hatchback Sport",42,6},
    {L"Muscle Apoc SoT Meg 01",48,1},
    {L"Muscle Basic",49,5},
    {L"Muscle Maximized",54,5},
    {L"RV Basic",59,1},
    {L"Sedan Iconic Apoc",60,5},
    {L"Sedan Iconic Basic",65,5},
    {L"Sedan Iconic Police",70,1},
    {L"Sedan Iconic Taxi",71,1},
    {L"Sedan Modern Apoc",72,5},
    {L"Sedan Modern Basic",77,7},
    {L"Sedan Modern Sport",84,5},
    {L"Sedan Old Apoc",89,6},
    {L"Sedan Old Basic",95,5},
    {L"Plane SmallProp",100,1},
    {L"Sport Coupe Apoc",101,5},
    {L"Sport Coupe Basic",106,5},
    {L"Sport Coupe Racer",111,5},
    {L"SUV Modern Apoc",116,5},
    {L"SUV Modern Basic",121,6},
    {L"SUV Modern RedTalon",127,2},
    {L"SUV Modern Police",129,1},
    {L"Truck Classic Apoc",130,6},
    {L"Truck Classic Basic",136,5},
    {L"Truck Military Apoc",141,5},
    {L"Truck Military",146,5},
    {L"Truck Modern Apoc",151,5},
    {L"Truck Modern Basic",156,6},
    {L"Truck Modern MidSize",162,2},
    {L"Truck Modern MidSize DLC1",164,1},
    {L"Truck Modern Utility",165,1},
    {L"Van Ambulance",166,1},
    {L"Van Apoc",167,5},
    {L"Van Cargo",172,6},
    {L"Van Cargo Meat DLC1",178,1},
    {L"Van Handyman",179,1},
    {L"Van News",180,1},
    {L"Van Passenger",181,5},
};
constexpr int vehicleCount=(int)(sizeof(vehicleDefs)/sizeof(vehicleDefs[0]));
constexpr int vehicleFamilyCount=(int)(sizeof(vehicleFamilies)/sizeof(vehicleFamilies[0]));
struct BountyDef{const wchar_t* label;const char* pkg;const char* cls;};
const BountyDef bountyDefs[]={
    {L"Mad Norma","/Game/Items/VehicleDeliveryItems/Apoc_Norma_VehicleDeliveryItem","Apoc_Norma_VehicleDeliveryItem_C"},
    {L"Megladon","/Game/Items/VehicleDeliveryItems/Apoc_Meg_SoT_VehicleDeliveryItem","Apoc_Meg_SoT_VehicleDeliveryItem_C"},
    {L"Trumble 4x4","/Game/Items/VehicleDeliveryItems/Truck_Modern_MidSize_VehicleDeliveryItem","Truck_Modern_MidSize_VehicleDeliveryItem_C"}
};
constexpr int bountyCount=(int)(sizeof(bountyDefs)/sizeof(bountyDefs[0]));
int vehFamilySel=0,vehVariantSel=0,bountySel=0;   // guarded by menuLock
bool vehRegisterNew=false;                          // guarded by menuLock
std::wstring vehSpawnStatus=L"Ready",bountyStatus=L"Ready"; // guarded by menuLock
constexpr UINT WM_ADREENA_TRUNK=WM_APP+0x2A8;   // wParam: new trunk size, lParam: 1 = only read the current size
bool trunkOnSpawn=false;   // guarded by menuLock: apply the trunk size to vehicles spawned from the Spawn Vehicle section
int trunkValue=100;std::wstring trunkStatus=L"Only applies to spawned vehicles.";  // guarded by menuLock
constexpr UINT WM_ADREENA_VMASK=WM_APP+0x2A6;   // re-check which vehicle classes are loaded (game thread)
constexpr UINT WM_ADREENA_VLOAD=WM_APP+0x2A7;   // wParam: vehicle index; ask the game to load its class
bool vehLoadedKnown=false;                       // guarded by menuLock
unsigned char vehLoadedFlags[vehicleCount]{};    // guarded by menuLock: 1 = class is in memory
bool vehMaskRequested=false,vehPollActive=false; // guarded by menuLock
int vehLoadPending=-1;                           // guarded by menuLock: vehicle index being loaded, or -1
// UI animation state (render thread, menuLock held).
bool uiMarqueeSeen=false;int uiMarqueeMs=1000;       // scrolling status text: redraw at most every uiMarqueeMs (not every frame)
bool uiAnimSeen=false;                           // set while drawing if something on screen is animating
ULONGLONG lastAnimDraw=0;
int flashId=0;ULONGLONG flashUntil=0;             // brief "pressed" look after a click
// ===== JSON helpers (profiles, bookmarks) =====
struct JVal{
    enum Type{NUL,BOOL,NUM,STR,ARR,OBJ};
    Type t=NUL;bool b=false;double n=0;std::string s;
    std::vector<JVal> a;std::vector<std::pair<std::string,JVal>> o;
    const JVal* get(const std::string& k)const{for(const auto& p:o)if(p.first==k)return &p.second;return nullptr;}
};
struct JParser{
    const std::string& src;size_t i=0;bool ok=true;
    explicit JParser(const std::string& s):src(s){}
    void ws(){while(i<src.size()&&(src[i]==' '||src[i]=='\t'||src[i]=='\r'||src[i]=='\n'))++i;}
    bool lit(const char* w){const size_t n=strlen(w);if(i<=src.size()&&src.compare(i,n,w)==0){i+=n;return true;}return false;}
    std::string str(){
        std::string out;++i;   // opening quote
        while(i<src.size()&&src[i]!='"'){
            const char c=src[i++];
            if(c=='\\'&&i<src.size()){
                const char e=src[i++];
                switch(e){
                    case 'n':out+='\n';break;case 't':out+='\t';break;case 'r':out+='\r';break;case 'b':out+='\b';break;case 'f':out+='\f';break;
                    case 'u':{unsigned v=0;for(int k=0;k<4&&i<src.size();++k){const char h=src[i++];v=v*16+(unsigned)(h>='0'&&h<='9'?h-'0':((h|32)>='a'&&(h|32)<='f'?(h|32)-'a'+10:0));}out+=v<128?(char)v:'?';break;}
                    default:out+=e;
                }
            }else out+=c;
        }
        if(i<src.size())++i;else ok=false;
        return out;
    }
    JVal val(int depth){
        JVal v;ws();
        if(i>=src.size()||depth>16){ok=false;return v;}
        const char c=src[i];
        if(c=='{'){
            v.t=JVal::OBJ;++i;ws();
            if(i<src.size()&&src[i]=='}'){++i;return v;}
            for(;;){
                ws();if(i>=src.size()||src[i]!='"'){ok=false;return v;}
                std::string k=str();ws();
                if(i>=src.size()||src[i]!=':'){ok=false;return v;}
                ++i;JVal x=val(depth+1);if(!ok)return v;
                v.o.emplace_back(k,std::move(x));ws();
                if(i<src.size()&&src[i]==','){++i;continue;}
                if(i<src.size()&&src[i]=='}'){++i;return v;}
                ok=false;return v;
            }
        }
        if(c=='['){
            v.t=JVal::ARR;++i;ws();
            if(i<src.size()&&src[i]==']'){++i;return v;}
            for(;;){
                JVal x=val(depth+1);if(!ok)return v;
                v.a.push_back(std::move(x));ws();
                if(i<src.size()&&src[i]==','){++i;continue;}
                if(i<src.size()&&src[i]==']'){++i;return v;}
                ok=false;return v;
            }
        }
        if(c=='"'){v.t=JVal::STR;v.s=str();return v;}
        if(lit("true")){v.t=JVal::BOOL;v.b=true;return v;}
        if(lit("false")){v.t=JVal::BOOL;return v;}
        if(lit("null"))return v;
        char* end=nullptr;const char* from=src.c_str()+i;
        v.n=strtod(from,&end);const size_t used=(size_t)(end-from);
        if(used==0){ok=false;return v;}
        i+=used;v.t=JVal::NUM;return v;
    }
};
bool jsonParse(const std::string& text,JVal& out){JParser p(text);out=p.val(0);p.ws();return p.ok&&out.t==JVal::OBJ;}
std::string jsonEscape(const std::string& s){
    std::string o;
    for(unsigned char c:s){
        switch(c){
            case '"':o+="\\\"";break;case '\\':o+="\\\\";break;case '\n':o+="\\n";break;case '\r':o+="\\r";break;case '\t':o+="\\t";break;
            default:if(c<32){char b[8];snprintf(b,sizeof(b),"\\u%04x",(unsigned)c);o+=b;}else o+=(char)c;
        }
    }
    return o;
}
std::string jsonNum(double v){char b[40];snprintf(b,sizeof(b),"%.6g",v);return b;}
// ===== end JSON helpers =====
namespace spawner{
constexpr uintptr_t RVA_GOBJECTS=0x46326B0,RVA_GWORLD=0x4726808,RVA_PROCESSEVENT=0x11FF660,RVA_APPENDSTRING=0x11394D0;
constexpr int VT_PROCESSEVENT=0x35;
constexpr int O_CLASS=0x10,O_NAME=0x18,O_OUTER=0x20,O_CHILDREN=0x38,O_NEXT=0x28,O_SIZE=0x40,O_PROPOFF=0x50;
constexpr size_t ITEM_SIZE=0x18;
struct FStr{wchar_t* data;int num;int max;};
using AppendFn=void(__fastcall*)(const void*,FStr*);
using PEFn=void(__fastcall*)(void*,void*,void*);
struct Fn{uintptr_t ptr=0;int size=0;std::unordered_map<std::string,int> off;};

bool safeRead(uintptr_t src,void* dst,size_t n){__try{memcpy(dst,(const void*)src,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
template<class T> bool rd(uintptr_t a,T& v){return a&&safeRead(a,&v,sizeof(T));}
bool safeWriteFloat(uintptr_t dst,float v){__try{*(volatile float*)dst=v;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool safeWriteInt(uintptr_t dst,int v){__try{*(volatile int*)dst=v;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool callAppend(AppendFn f,const void* n,FStr* s){__try{f(n,s);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool callPE(PEFn f,uintptr_t obj,uintptr_t fn,void* params){__try{f((void*)obj,(void*)fn,params);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}

uintptr_t base=0,objs=0;int numObjs=0;AppendFn appendName=nullptr;PEFn processEvent=nullptr;
bool ready=false,dead=false;std::string deadWhy;
Fn fBegin,fFinish,fPawn,fLoc,fFwd;uintptr_t cdoStatics=0;
uintptr_t zClass[zombieCount]{};int zIndex[zombieCount]{};

void logf(const char* fmt,unsigned long long a=0,unsigned long long b=0,unsigned long long c=0){
    char m[256];snprintf(m,sizeof(m),fmt,a,b,c);logMessage(m);
}
std::wstring widen(const std::string& s){return std::wstring(s.begin(),s.end());}
void logStatus(const char* what,const std::wstring& w){
    std::string n;for(wchar_t c:w)n.push_back(c<128?(char)c:'?');
    logMessage((std::string(what)+": "+n).c_str());
}

bool objName(uintptr_t obj,std::string& out){
    unsigned char fname[8];
    if(!obj||!safeRead(obj+O_NAME,fname,8))return false;
    static wchar_t buf[1024];buf[0]=0;FStr s{buf,0,1024};
    if(!callAppend(appendName,fname,&s))return false;
    if(s.data!=buf||s.num<=0||s.num>1024)return false; // engine reallocated or garbage: do not trust
    out.clear();
    for(int i=0;i<s.num&&buf[i];++i)out.push_back(buf[i]<128?(char)buf[i]:'?');
    return !out.empty();
}
bool onMainThread(){
    const DWORD cur=GetCurrentThreadId(),pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snap==INVALID_HANDLE_VALUE)return false;
    THREADENTRY32 te{};te.dwSize=sizeof(te);
    ULONGLONG best=~0ull;DWORD bestId=0;
    if(Thread32First(snap,&te)){
        do{
            if(te.th32OwnerProcessID!=pid)continue;
            HANDLE th=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,te.th32ThreadID);
            if(!th)continue;
            FILETIME c{},e{},k{},u{};
            if(GetThreadTimes(th,&c,&e,&k,&u)){
                ULONGLONG t=((ULONGLONG)c.dwHighDateTime<<32)|c.dwLowDateTime;
                if(t<best){best=t;bestId=te.th32ThreadID;}
            }
            CloseHandle(th);
        }while(Thread32Next(snap,&te));
    }
    CloseHandle(snap);
    logf("Spawner: current thread %llu, main (oldest) thread %llu",cur,bestId);
    return bestId!=0&&bestId==cur;
}
uintptr_t objArrayAddr=0;
// The object count grows as the game loads things; re-read it so new actors/classes are visible.
void refreshCount(){
    int n=0,mx=0;
    if(objArrayAddr&&rd(objArrayAddr+8,mx)&&rd(objArrayAddr+12,n)&&n>=numObjs&&n<=mx)numObjs=n;
}
bool locateObjects(){
    for(uintptr_t extra:{(uintptr_t)0,(uintptr_t)0x10}){
        const uintptr_t arr=base+RVA_GOBJECTS+extra;
        uintptr_t p=0;int mx=0,n=0;
        if(!rd(arr,p)||!rd(arr+8,mx)||!rd(arr+12,n))continue;
        if(p&&n>1000&&n<=mx&&mx<20000000){objs=p;numObjs=n;objArrayAddr=arr;logf("Spawner: GObjects ok, %llu objects (layout +0x%llX)",(unsigned long long)n,extra);return true;}
    }
    return false;
}
struct Hit{uintptr_t obj;int index;std::string cls,outer;};
// One pass over GObjects, collecting only objects whose name is in 'wanted'.
std::unordered_map<std::string,std::vector<Hit>> scan(const std::unordered_set<std::string>& wanted,int& namesOk,int& namesTried){
    std::unordered_map<std::string,std::vector<Hit>> hits;
    namesOk=namesTried=0;refreshCount();
    for(int i=0;i<numObjs;++i){
        uintptr_t o=0;
        if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
        std::string nm;
        const bool probe=namesTried<2000; // health check: how many of the first 2000 names could be read
        if(probe)++namesTried;
        if(!objName(o,nm))continue;
        if(probe)++namesOk;
        if(!wanted.count(nm))continue;
        Hit h{o,i,"",""};uintptr_t cls=0,outer=0;
        if(rd(o+O_CLASS,cls))objName(cls,h.cls);
        if(rd(o+O_OUTER,outer))objName(outer,h.outer);
        hits[nm].push_back(h);
    }
    return hits;
}
bool readFn(Fn& f,uintptr_t fn,std::initializer_list<const char*> required){
    int sz=0;
    if(!rd(fn+O_SIZE,sz)||sz<0||sz>4096)return false;   // size 0 is valid: functions without parameters
    f.ptr=fn;f.size=sz;f.off.clear();
    uintptr_t c=0;rd(fn+O_CHILDREN,c);
    for(int g=0;c&&g<64;++g){
        std::string n;int off=-1;
        if(objName(c,n)&&rd(c+O_PROPOFF,off)&&off>=0&&off<sz)f.off[n]=off;
        uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
    }
    for(const char* r:required)if(!f.off.count(r)){logf("Spawner: a required parameter is missing from a function");return false;}
    return true;
}
const uintptr_t* findHit(const std::unordered_map<std::string,std::vector<Hit>>& hits,const char* name,const char* cls,const char* outer,uintptr_t& out){
    auto it=hits.find(name);if(it==hits.end())return nullptr;
    for(const Hit& h:it->second){
        if(cls&&h.cls!=cls)continue;
        if(outer&&h.outer!=outer)continue;
        out=h.obj;return &out;
    }
    return nullptr;
}
bool init(std::string& why){
    if(ready)return true;
    if(dead){why=deadWhy;return false;}
    auto fail=[&](const char* w){dead=true;deadWhy=w;why=w;logMessage((std::string("Spawner disabled: ")+w).c_str());return false;};
    base=(uintptr_t)GetModuleHandleW(HSTR("StateOfDecay2-Win64-Shipping.exe").c_str());
    if(!base)return fail("game module not found");
    if(!onMainThread())return fail("menu is not running on the game's main thread");
    appendName=(AppendFn)(base+RVA_APPENDSTRING);processEvent=(PEFn)(base+RVA_PROCESSEVENT);
    if(!locateObjects())return fail("GObjects offset invalid for this game build");
    std::unordered_set<std::string> wanted={"Default__GameplayStatics","BeginDeferredActorSpawnFromClass","FinishSpawningActor","GetPlayerPawn","K2_GetActorLocation","GetActorForwardVector"};
    int ok=0,tried=0;
    auto hits=scan(wanted,ok,tried);
    if(tried>=100&&ok*10<tried*9)return fail("cannot read object names (AppendString offset wrong?)");
    uintptr_t cdo=0,fb=0,ff=0,fp=0,fl=0,fw=0;
    if(!findHit(hits,"Default__GameplayStatics","GameplayStatics",nullptr,cdo))return fail("GameplayStatics not found (offsets wrong for this build?)");
    if(!findHit(hits,"BeginDeferredActorSpawnFromClass","Function","GameplayStatics",fb)||
       !findHit(hits,"FinishSpawningActor","Function","GameplayStatics",ff)||
       !findHit(hits,"GetPlayerPawn","Function","GameplayStatics",fp)||
       !findHit(hits,"K2_GetActorLocation","Function","Actor",fl)||
       !findHit(hits,"GetActorForwardVector","Function","Actor",fw))return fail("a required engine function was not found");
    // Verify the ProcessEvent RVA against the object's real vtable before ever calling it.
    uintptr_t vt=0,peSlot=0;
    if(!rd(cdo,vt)||!rd(vt+8*(uintptr_t)VT_PROCESSEVENT,peSlot))return fail("cannot read vtable");
    if(peSlot!=base+RVA_PROCESSEVENT){logf("Spawner: vtable ProcessEvent 0x%llX != expected 0x%llX",(unsigned long long)(peSlot-base),RVA_PROCESSEVENT);return fail("ProcessEvent offset mismatch");}
    if(!readFn(fBegin,fb,{"WorldContextObject","ActorClass","SpawnTransform","ReturnValue"})||
       !readFn(fFinish,ff,{"Actor","SpawnTransform"})||
       !readFn(fPawn,fp,{"WorldContextObject","PlayerIndex","ReturnValue"})||
       !readFn(fLoc,fl,{"ReturnValue"})||!readFn(fFwd,fw,{"ReturnValue"}))return fail("function parameter layout not as expected");
    cdoStatics=cdo;ready=true;
    logMessage("Spawner ready: functions resolved, ProcessEvent verified");
    return true;
}
// Diagnostic for classes that are not loaded: logs similarly named objects and any engine "Load" functions.
bool icontains(const std::string& h,const std::string& n){
    if(n.empty()||h.size()<n.size())return false;
    for(size_t i=0;i+n.size()<=h.size();++i){
        size_t j=0;for(;j<n.size();++j)if(tolower((unsigned char)h[i+j])!=tolower((unsigned char)n[j]))break;
        if(j==n.size())return true;
    }
    return false;
}
void diagnose(int idx){
    static bool done[zombieCount]{};
    if(done[idx])return;done[idx]=true;
    std::string stem=zombieDefs[idx].cls;
    if(stem.size()>2&&stem.compare(stem.size()-2,2,"_C")==0)stem.resize(stem.size()-2);
    std::string key=stem;
    if(stem.find("Bloater")!=std::string::npos)key="Bloater";
    else if(stem.find("BlackPlague")!=std::string::npos)key="BlackPlague";
    logMessage((std::string("Spawner diag: ")+zombieDefs[idx].cls+" not loaded; searching for '"+key+"' and Load functions").c_str());
    int shown=0,loadFns=0;
    for(int i=0;i<numObjs&&(shown<80||loadFns<40);++i){
        uintptr_t o=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
        std::string nm;if(!objName(o,nm))continue;
        const bool nameHit=shown<80&&icontains(nm,key);
        const bool loadHit=loadFns<40&&icontains(nm,"load")&&(icontains(nm,"asset")||icontains(nm,"class")||icontains(nm,"blocking"));
        if(!nameHit&&!loadHit)continue;
        uintptr_t cls=0,outer=0;std::string cn,on;
        if(rd(o+O_CLASS,cls))objName(cls,cn);
        if(rd(o+O_OUTER,outer))objName(outer,on);
        if(loadHit&&cn=="Function"){
            ++loadFns;
            logMessage((std::string("Spawner diag: LOADFN ")+on+"."+nm).c_str());
        }else if(nameHit){
            ++shown;
            logMessage((std::string("Spawner diag: ")+nm+"  [class "+cn+", outer "+on+"]").c_str());
        }
    }
    logMessage("Spawner diag: done");
}
// Class pointers are cached with their GObjects slot and re-validated every use (a freed class must never be used).
// Which zombie classes are in memory right now (1 = loaded). Game thread only.
void computeLoadedZombies(unsigned char* out){
    std::unordered_set<std::string> wanted;
    for(int i=0;i<zombieCount;++i)wanted.insert(zombieDefs[i].cls);
    int ok=0,t=0;auto hits=scan(wanted,ok,t);
    for(int i=0;i<zombieCount;++i){
        out[i]=0;
        auto it=hits.find(zombieDefs[i].cls);if(it==hits.end())continue;
        for(const Hit& h:it->second)if(h.cls.find("Class")!=std::string::npos){out[i]=1;break;}
    }
}
std::wstring applyZombieStats(uintptr_t actor,int hp,float melee,float ranged);
std::wstring setUnlimitedSpawning(bool on);
uintptr_t classFor(int idx){
    uintptr_t cur=0;
    if(zClass[idx]&&rd(objs+(size_t)zIndex[idx]*ITEM_SIZE,cur)&&cur==zClass[idx]){
        std::string nm;if(objName(cur,nm)&&nm==zombieDefs[idx].cls)return cur;
    }
    zClass[idx]=0;
    std::unordered_set<std::string> wanted={zombieDefs[idx].cls};int ok=0,tried=0;
    auto hits=scan(wanted,ok,tried);
    auto it=hits.find(zombieDefs[idx].cls);if(it==hits.end())return 0;
    for(const Hit& h:it->second){
        if(h.cls.find("Class")==std::string::npos)continue; // BlueprintGeneratedClass / Class, never the Default__ instance
        zClass[idx]=h.obj;zIndex[idx]=h.index;return h.obj;
    }
    return 0;
}
struct Buf{
    void* p;size_t n;
    explicit Buf(size_t sz):n(sz+32){p=_aligned_malloc(n,16);if(p)memset(p,0,n);}
    ~Buf(){if(p)_aligned_free(p);}
    template<class T> void put(const Fn& f,const char* name,T v){auto it=f.off.find(name);if(p&&it!=f.off.end()&&(size_t)it->second+sizeof(T)<=n)memcpy((char*)p+it->second,&v,sizeof(T));}
    void putBytes(const Fn& f,const char* name,const void* src,size_t len){auto it=f.off.find(name);if(p&&it!=f.off.end()&&(size_t)it->second+len<=n)memcpy((char*)p+it->second,src,len);}
    template<class T> bool get(const Fn& f,const char* name,T& v){auto it=f.off.find(name);if(!p||it==f.off.end()||(size_t)it->second+sizeof(T)>n)return false;memcpy(&v,(char*)p+it->second,sizeof(T));return true;}
};
// Starts the game's own asynchronous asset load for a blueprint class; returns a short status ("" on success).
std::wstring requestLoad(const char* pkg,const char* cls);
// Call only on the game's main thread.
std::wstring spawn(int idx,bool custom=false,int hp=0,float melee=1.f,float ranged=1.f){
    if(idx<0||idx>=zombieCount)return L"Bad selection";
    std::string why;
    if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;
    if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t cls=classFor(idx);
    if(!cls){
        diagnose(idx);
        const std::wstring r=requestLoad(zombieDefs[idx].pkg,zombieDefs[idx].cls);
        if(r.empty())return std::wstring(L"Loading ")+zombieDefs[idx].label+L" zombie class into memory...";
        return L"Class not loaded in memory yet; loader: "+r;
    }
    // Player pawn, its location and facing.
    uintptr_t pawn=0;
    {Buf b(fPawn.size);b.put(fPawn,"WorldContextObject",world);b.put(fPawn,"PlayerIndex",(int)0);
     if(!callPE(processEvent,cdoStatics,fPawn.ptr,b.p))return L"Engine call faulted (GetPlayerPawn)";
     b.get(fPawn,"ReturnValue",pawn);}
    if(!pawn)return L"No player pawn (are you in game?)";
    float loc[3]{},fwd[3]{};
    {Buf b(fLoc.size);if(!callPE(processEvent,pawn,fLoc.ptr,b.p))return L"Engine call faulted (location)";
     if(!b.get(fLoc,"ReturnValue",loc))return L"Could not read location";}
    {Buf b(fFwd.size);if(!callPE(processEvent,pawn,fFwd.ptr,b.p))return L"Engine call faulted (forward)";
     if(!b.get(fFwd,"ReturnValue",fwd))return L"Could not read facing";}
    for(float v:{loc[0],loc[1],loc[2],fwd[0],fwd[1],fwd[2]})if(!(v==v)||v>1e9f||v<-1e9f)return L"Bad player location";
    // FTransform (UE4, single precision): rotation quat, translation, scale; 48 bytes.
    float xf[12]={0,0,0,1, loc[0]+fwd[0]*350.f,loc[1]+fwd[1]*350.f,loc[2]+fwd[2]*350.f+40.f,0, 1,1,1,0};
    uintptr_t actor=0;
    {Buf b(fBegin.size);
     b.put(fBegin,"WorldContextObject",world);b.put(fBegin,"ActorClass",cls);b.putBytes(fBegin,"SpawnTransform",xf,sizeof(xf));
     b.put(fBegin,"CollisionHandlingOverride",(unsigned char)2);
     if(!callPE(processEvent,cdoStatics,fBegin.ptr,b.p))return L"Engine call faulted (spawn begin)";
     b.get(fBegin,"ReturnValue",actor);}
    if(!actor)return L"Spawn returned nothing";
    {Buf b(fFinish.size);b.put(fFinish,"Actor",actor);b.putBytes(fFinish,"SpawnTransform",xf,sizeof(xf));
     if(!callPE(processEvent,cdoStatics,fFinish.ptr,b.p))return L"Engine call faulted (spawn finish)";}
    logMessage((std::string("Spawned: ")+zombieDefs[idx].cls).c_str());
    std::wstring result=std::wstring(L"Spawned ")+zombieDefs[idx].label;
    if(custom)result+=applyZombieStats(actor,hp,melee,ranged);
    return result;
}

// ---- Vehicle actions: refuel / repair the vehicle you are in, or the nearest one -------------------------------
// Same call the Hades menu makes: Player.ServerUseItemOnVehicle(vehicle, <item default object>) with the game's
// own "refuel vehicle" / "repair vehicle (advanced)" items. Does NOT touch the Vehicle Fuel Lock hook.
constexpr int O_SUPER=0x30;
bool vehReady=false,vehDead=false;std::string vehWhy;
Fn fGetVeh,fRegister,fTele,fGetTf;uintptr_t cdoVehLib=0,clsVehicle=0;std::vector<uintptr_t> useCandidates;
struct Cached{uintptr_t obj=0;int index=0;};
Cached cRefuel,cRepair;
bool isA(uintptr_t cls,uintptr_t target){
    for(int g=0;cls&&g<16;++g){
        if(cls==target)return true;
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))return false;cls=sup;
    }
    return false;
}
std::vector<std::pair<std::string,int>> sortedParams(const Fn& f){
    std::vector<std::pair<std::string,int>> v;
    for(const auto& kv:f.off)if(kv.first!="ReturnValue")v.push_back({kv.first,kv.second});
    std::sort(v.begin(),v.end(),[](const std::pair<std::string,int>& a,const std::pair<std::string,int>& b){return a.second<b.second;});
    return v;
}
bool initVehicle(std::string& why){
    if(vehReady)return true;
    if(vehDead){why=vehWhy;return false;}
    if(!init(why))return false;
    auto fail=[&](const char* w){vehDead=true;vehWhy=w;why=w;logMessage((std::string("Vehicle actions disabled: ")+w).c_str());return false;};
    std::unordered_set<std::string> wanted={"ServerUseItemOnVehicle","GetActorVehicle","RegisterSpawnedVehicle","K2_TeleportTo","GetTransform","Default__VehicleFunctionLibrary","DaytonVehicle"};
    int ok=0,tried=0;auto hits=scan(wanted,ok,tried);
    uintptr_t lib=0,gv=0,cls=0;
    if(!findHit(hits,"Default__VehicleFunctionLibrary","VehicleFunctionLibrary",nullptr,lib))return fail("VehicleFunctionLibrary not found");
    if(!findHit(hits,"GetActorVehicle","Function","VehicleFunctionLibrary",gv))return fail("GetActorVehicle not found");
    if(!findHit(hits,"DaytonVehicle","Class",nullptr,cls))return fail("DaytonVehicle class not found");
    auto it=hits.find("ServerUseItemOnVehicle");if(it==hits.end())return fail("ServerUseItemOnVehicle not found");
    useCandidates.clear();
    for(const Hit& h:it->second)if(h.cls=="Function")useCandidates.push_back(h.obj);
    if(useCandidates.empty())return fail("ServerUseItemOnVehicle not found");
    if(!readFn(fGetVeh,gv,{"ReturnValue"})||sortedParams(fGetVeh).size()!=1)return fail("GetActorVehicle layout not as expected");
    {uintptr_t rg=0;fRegister=Fn();
     if(findHit(hits,"RegisterSpawnedVehicle","Function","VehicleFunctionLibrary",rg)&&(!readFn(fRegister,rg,{})||sortedParams(fRegister).size()!=1))fRegister=Fn();}
    {uintptr_t tp=0;fTele=Fn();
     if(findHit(hits,"K2_TeleportTo","Function","Actor",tp)&&!readFn(fTele,tp,{"DestLocation","DestRotation"}))fTele=Fn();}
    {uintptr_t gt=0;fGetTf=Fn();
     if(findHit(hits,"GetTransform","Function","Actor",gt)&&(!readFn(fGetTf,gt,{"ReturnValue"})||fGetTf.size<48))fGetTf=Fn();}
    cdoVehLib=lib;clsVehicle=cls;vehReady=true;
    logMessage("Vehicle actions ready");
    return true;
}
// Find a default object by exact name, caching it with its GObjects slot (re-validated on every use).
uintptr_t findCdo(const char* name,const char* clsName,Cached& c){
    uintptr_t cur=0;
    if(c.obj&&rd(objs+(size_t)c.index*ITEM_SIZE,cur)&&cur==c.obj){std::string nm;if(objName(cur,nm)&&nm==name)return cur;}
    c.obj=0;
    std::unordered_set<std::string> wanted={name};int ok=0,tried=0;
    auto hits=scan(wanted,ok,tried);
    auto it=hits.find(name);if(it==hits.end())return 0;
    for(const Hit& h:it->second)if(h.cls==clsName){c.obj=h.obj;c.index=h.index;return h.obj;}
    return 0;
}
bool actorLocation(uintptr_t actor,float out[3]){
    Buf b(fLoc.size);
    return callPE(processEvent,actor,fLoc.ptr,b.p)&&b.get(fLoc,"ReturnValue",out);
}
// Nearest live actor whose class derives from 'targetCls', within maxDist of point pl. Skips default objects,
// archetypes, pending-kill objects, and anything that is not a real actor placed in a level.
uintptr_t nearestActor(uintptr_t targetCls,const float pl[3],float maxDist){
    std::unordered_map<uintptr_t,bool> memo;
    uintptr_t best=0;float bestD=maxDist*maxDist;refreshCount();
    for(int i=0;i<numObjs;++i){
        uintptr_t o=0,cls=0;
        if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o||!rd(o+O_CLASS,cls)||!cls)continue;
        auto m=memo.find(cls);
        bool match;
        if(m==memo.end()){match=isA(cls,targetCls);memo[cls]=match;}else match=m->second;
        if(!match)continue;
        int fl=0,ifl=0;
        if(!rd(o+8,fl)||(fl&0x30)||(fl&0x20000000))continue;   // class default object / archetype / pending kill
        if(rd(objs+(size_t)i*ITEM_SIZE+8,ifl)&&(ifl&0x20000000))continue;
        std::string nm;if(!objName(o,nm))continue;
        if(nm.compare(0,9,"Default__")==0||nm.compare(0,7,"REINST_")==0||nm.compare(0,5,"SKEL_")==0)continue;
        uintptr_t outer=0,oc=0;std::string on;
        if(!rd(o+O_OUTER,outer)||!rd(outer+O_CLASS,oc)||!objName(oc,on)||on!="Level")continue;
        float p[3]{};
        if(!actorLocation(o,p))continue;
        const float dx=p[0]-pl[0],dy=p[1]-pl[1],dz=p[2]-pl[2],d=dx*dx+dy*dy+dz*dz;
        if(d<bestD){bestD=d;best=o;}
    }
    return best;
}
// Call only on the game's main thread. refuel=true: refuel item, false: advanced repair item.
std::wstring vehicleAction(bool refuel){
    std::string why;
    if(!initVehicle(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;
    if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    uintptr_t pawn=0;
    {Buf b(fPawn.size);b.put(fPawn,"WorldContextObject",world);b.put(fPawn,"PlayerIndex",(int)0);
     if(!callPE(processEvent,cdoStatics,fPawn.ptr,b.p))return L"Engine call faulted (GetPlayerPawn)";
     b.get(fPawn,"ReturnValue",pawn);}
    if(!pawn)return L"No player pawn (are you in game?)";
    const char* itemName=refuel?"Default__MiscItemRefuelVehicle_C":"Default__MiscItemRepairVehicleAdvanced_C";
    const char* itemCls=refuel?"MiscItemRefuelVehicle_C":"MiscItemRepairVehicleAdvanced_C";
    const uintptr_t item=findCdo(itemName,itemCls,refuel?cRefuel:cRepair);
    if(!item)return refuel?L"Refuel item not loaded in memory yet":L"Repair item not loaded in memory yet";
    uintptr_t pc=0;if(!rd(pawn+O_CLASS,pc))return L"Could not read player class";
    float pl[3]{};
    if(!actorLocation(pawn,pl))return L"Could not read player location";
    // While driving, the player's pawn is the vehicle itself; a survivor character must make the call instead.
    const bool driving=isA(pc,clsVehicle);
    Fn use;uintptr_t caller=0;
    for(uintptr_t cand:useCandidates){
        uintptr_t outer=0;if(!rd(cand+O_OUTER,outer))continue;
        uintptr_t who=0;
        if(driving)who=nearestActor(outer,pl,3000.f);
        else if(isA(pc,outer))who=pawn;
        if(who&&readFn(use,cand,{})){caller=who;break;}
    }
    if(!caller){
        std::string pn;objName(pc,pn);
        logMessage((std::string("Vehicle actions: no character can call ServerUseItemOnVehicle (pawn class ")+pn+(driving?", driving)":", on foot)")).c_str());
        return driving?L"No survivor character found to use the item (see log)":L"Player cannot use items on vehicles (function mismatch)";
    }
    const auto ps=sortedParams(use);
    if(ps.size()!=2){logf("Vehicle actions: ServerUseItemOnVehicle has %llu parameters, expected 2",(unsigned long long)ps.size());return L"Unexpected function layout (see log)";}
    // Vehicle: the one being driven, else the one the player is in, else the nearest.
    uintptr_t veh=0;
    if(driving)veh=pawn;
    else{
        {Buf b(fGetVeh.size);const auto gp=sortedParams(fGetVeh);b.put(fGetVeh,gp[0].first.c_str(),pawn);
         if(callPE(processEvent,cdoVehLib,fGetVeh.ptr,b.p))b.get(fGetVeh,"ReturnValue",veh);}
        if(veh){uintptr_t vc=0;if(!rd(veh+O_CLASS,vc)||!isA(vc,clsVehicle))veh=0;}
        if(!veh)veh=nearestActor(clsVehicle,pl,5000.f);
    }
    if(!veh)return L"No vehicle nearby (get in one or stand next to it)";
    {Buf b(use.size);b.put(use,ps[0].first.c_str(),veh);b.put(use,ps[1].first.c_str(),item);
     if(!callPE(processEvent,caller,use.ptr,b.p))return L"Engine call faulted (use item)";}
    logMessage(refuel?"Vehicle refuel applied":"Vehicle repair applied");
    return refuel?L"Refuel applied":L"Repair applied";
}

// ---- Vehicle spawning and bounty delivery (Hades: spawn from a class list; request a delivery item) ----------
std::string lowerStr(std::string s){for(char& c:s)c=(char)tolower((unsigned char)c);return s;}
// Case-insensitive variant of scan(): names in 'wantedLower' must already be lower case; hits are keyed lower case.
std::unordered_map<std::string,std::vector<Hit>> scanLower(const std::unordered_set<std::string>& wantedLower){
    std::unordered_map<std::string,std::vector<Hit>> hits;refreshCount();
    for(int i=0;i<numObjs;++i){
        uintptr_t o=0;
        if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
        std::string nm;
        if(!objName(o,nm))continue;
        const std::string lo=lowerStr(nm);
        if(!wantedLower.count(lo))continue;
        Hit h{o,i,"",""};uintptr_t cls=0,outer=0;
        if(rd(o+O_CLASS,cls))objName(cls,h.cls);
        if(rd(o+O_OUTER,outer))objName(outer,h.outer);
        hits[lo].push_back(h);
    }
    return hits;
}
Cached vehCache[vehicleCount];
float g_vehWant[3]{};
// A loaded blueprint class by exact package path + name (case-insensitive), cached with its GObjects slot.
uintptr_t blueprintClass(const char* pkg,const char* cls,Cached* cache){
    uintptr_t cur=0;
    if(cache&&cache->obj&&rd(objs+(size_t)cache->index*ITEM_SIZE,cur)&&cur==cache->obj){
        std::string nm;if(objName(cur,nm)&&lowerStr(nm)==lowerStr(cls))return cur;
    }
    if(cache)cache->obj=0;
    std::unordered_set<std::string> wanted={lowerStr(cls)};
    auto hits=scanLower(wanted);
    auto it=hits.find(lowerStr(cls));if(it==hits.end())return 0;
    for(const Hit& h:it->second){
        if(h.cls.find("Class")==std::string::npos)continue;      // BlueprintGeneratedClass, never the Default__ object
        if(lowerStr(h.outer)!=lowerStr(pkg))continue;            // several vehicles share a class name; the package tells them apart
        if(cache){cache->obj=h.obj;cache->index=h.index;}
        return h.obj;
    }
    return 0;
}
void logLoadedVehicles(){
    std::unordered_set<std::string> wanted;
    for(int i=0;i<vehicleCount;++i)wanted.insert(lowerStr(vehicleDefs[i].cls));
    auto hits=scanLower(wanted);
    int loaded=0;std::string names;
    for(int i=0;i<vehicleCount;++i){
        auto it=hits.find(lowerStr(vehicleDefs[i].cls));if(it==hits.end())continue;
        for(const Hit& h:it->second)
            if(h.cls.find("Class")!=std::string::npos&&lowerStr(h.outer)==lowerStr(vehicleDefs[i].pkg)){++loaded;if(names.size()<1500)names+=std::string(vehicleDefs[i].cls)+" ";break;}
    }
    logf("Vehicle spawner: %llu of %llu vehicle classes are loaded right now",(unsigned long long)loaded,(unsigned long long)vehicleCount);
    if(!names.empty())logMessage((std::string("Vehicle spawner: loaded = ")+names).c_str());
}
// Which of the vehicle classes are in memory right now (1 = loaded). Game thread only.
void computeLoadedVehicles(unsigned char* out){
    std::unordered_set<std::string> wanted;
    for(int i=0;i<vehicleCount;++i)wanted.insert(lowerStr(vehicleDefs[i].cls));
    auto hits=scanLower(wanted);
    for(int i=0;i<vehicleCount;++i){
        out[i]=0;
        auto it=hits.find(lowerStr(vehicleDefs[i].cls));if(it==hits.end())continue;
        for(const Hit& h:it->second)
            if(h.cls.find("Class")!=std::string::npos&&lowerStr(h.outer)==lowerStr(vehicleDefs[i].pkg)){out[i]=1;break;}
    }
}
uintptr_t playerPawn(uintptr_t world){
    uintptr_t pawn=0;
    Buf b(fPawn.size);b.put(fPawn,"WorldContextObject",world);b.put(fPawn,"PlayerIndex",(int)0);
    if(callPE(processEvent,cdoStatics,fPawn.ptr,b.p))b.get(fPawn,"ReturnValue",pawn);
    return pawn;
}
// Logs whether a spawned actor still exists and where it is. Safe on stale pointers (re-validated through GObjects).
int propOffset(uintptr_t cls,const char* name);
// Log whether an actor has a root component (without one it has no position) and where it is.
void probeActor(const char* tag,uintptr_t actor){
    char m[300];
    uintptr_t cls=0,root=0,rc=0;std::string cn,rn="-";
    if(!rd(actor+O_CLASS,cls)){snprintf(m,sizeof(m),"%s: cannot read actor",tag);logMessage(m);return;}
    objName(cls,cn);
    const int off=propOffset(cls,"RootComponent");
    if(off>=0){rd(actor+(uintptr_t)off,root);if(root&&rd(root+O_CLASS,rc))objName(rc,rn);}
    float p[3]{};const bool got=actorLocation(actor,p);
    snprintf(m,sizeof(m),"%s: %s RootComponent offset %d ptr %llx (%s) location %d %d %d%s",tag,cn.c_str(),off,(unsigned long long)root,rn.c_str(),(int)p[0],(int)p[1],(int)p[2],got?"":" [unreadable]");
    logMessage(m);
}
void describeActor(const char* tag,uintptr_t actor,int index,const float want[3]){
    char m[320];
    uintptr_t cur=0;int fl=0,ifl=0;refreshCount();
    if(index<0||index>=numObjs||!rd(objs+(size_t)index*ITEM_SIZE,cur)||cur!=actor){
        snprintf(m,sizeof(m),"%s: actor %llx is gone (object slot %d no longer holds it)",tag,(unsigned long long)actor,index);logMessage(m);return;}
    rd(actor+8,fl);rd(objs+(size_t)index*ITEM_SIZE+8,ifl);
    std::string cn;uintptr_t cls=0;if(rd(actor+O_CLASS,cls))objName(cls,cn);
    float p[3]{};const bool got=actorLocation(actor,p);
    snprintf(m,sizeof(m),"%s: actor %llx class %s flags %x itemflags %x pendingKill=%d",tag,(unsigned long long)actor,cn.c_str(),(unsigned)fl,(unsigned)ifl,((fl|ifl)&0x20000000)?1:0);logMessage(m);
    if(got)snprintf(m,sizeof(m),"%s: location %d %d %d (asked for %d %d %d)",tag,(int)p[0],(int)p[1],(int)p[2],(int)want[0],(int)want[1],(int)want[2]);
    else snprintf(m,sizeof(m),"%s: could not read location",tag);
    logMessage(m);
}
// ---- Class loading: KismetSystemLibrary.LoadAssetClass (the game has no *_Blocking variant, so it is asynchronous) ----
std::wstring requestLoad(const char* pkg,const char* cls){
    static bool tried=false,okInit=false;static std::string whyNot;
    static uintptr_t cdoKsl=0;static Fn fLoad;static std::vector<std::pair<std::string,int>> ps;static int assetSize=0;
    static int uuid=0x41445245;static std::vector<std::wstring*> keep;
    if(!tried){
        tried=true;
        std::unordered_set<std::string> wanted={"Default__KismetSystemLibrary","LoadAssetClass"};
        int ok=0,t=0;auto hits=scan(wanted,ok,t);
        uintptr_t fn=0;
        if(!findHit(hits,"Default__KismetSystemLibrary","KismetSystemLibrary",nullptr,cdoKsl))whyNot="KismetSystemLibrary not found";
        else if(!findHit(hits,"LoadAssetClass","Function","KismetSystemLibrary",fn)||!readFn(fLoad,fn,{}))whyNot="LoadAssetClass not found";
        else{
            ps=sortedParams(fLoad);
            if(ps.size()!=4)whyNot="LoadAssetClass has an unexpected parameter list";
            else{
                assetSize=ps[2].second-ps[1].second;
                const int latentSize=fLoad.size-ps[3].second;
                char m[200];snprintf(m,sizeof(m),"Class loader: LoadAssetClass found, soft class pointer is %d bytes, latent info %d bytes",assetSize,latentSize);logMessage(m);
                if(assetSize!=32)whyNot="unsupported soft pointer layout (see log)";
                else if(latentSize<24)whyNot="unsupported latent info layout (see log)";
                else okInit=true;
            }
        }
        if(!okInit)logMessage((std::string("Class loader unavailable: ")+whyNot).c_str());
    }
    if(!okInit)return widen(whyNot);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"no game world";
    uintptr_t target=playerPawn(world);if(!target)target=cdoStatics;
    // Soft class pointer (older engine layout): weak ptr (8) + tag (4) + FString asset path at +16.
    std::wstring* path=new std::wstring(widen(std::string(pkg)+"."+cls));
    keep.push_back(path);
    Buf b(fLoad.size);
    b.put(fLoad,ps[0].first.c_str(),world);
    {
        unsigned char soft[32]{};
        const uintptr_t data=(uintptr_t)path->c_str();const int num=(int)path->size()+1;
        memcpy(soft+16,&data,8);memcpy(soft+24,&num,4);memcpy(soft+28,&num,4);
        b.putBytes(fLoad,ps[1].first.c_str(),soft,sizeof(soft));
    }
    {
        // FLatentActionInfo: Linkage, UUID, ExecutionFunction (None), CallbackTarget. Only the target and UUID matter here.
        unsigned char li[24]{};
        const int link=0,id=uuid++;
        memcpy(li+0,&link,4);memcpy(li+4,&id,4);memcpy(li+16,&target,8);
        b.putBytes(fLoad,ps[3].first.c_str(),li,sizeof(li));
    }
    if(!callPE(processEvent,cdoKsl,fLoad.ptr,b.p))return L"engine call faulted";
    logMessage((std::string("Class loader: requested load of ")+pkg+"."+cls).c_str());
    return L"";
}
// ---- Trunk size: Hades sets VehicleDefinition.InventorySize on the vehicle's definition ---------------
bool writeInt(uintptr_t a,int v){__try{*(int*)a=v;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
std::wstring trunkAction(int newSize,bool readOnly,int& current){
    current=-1;
    std::string why;if(!initVehicle(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;
    if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);
    if(!pawn)return L"No player pawn (are you in game?)";
    uintptr_t pc=0;if(!rd(pawn+O_CLASS,pc))return L"Could not read player class";
    float pl[3]{};if(!actorLocation(pawn,pl))return L"Could not read player location";
    // Vehicle: the one being driven, else the one the player is in, else the nearest.
    uintptr_t veh=0;
    if(isA(pc,clsVehicle))veh=pawn;
    else{
        {Buf b(fGetVeh.size);const auto gp=sortedParams(fGetVeh);b.put(fGetVeh,gp[0].first.c_str(),pawn);
         if(callPE(processEvent,cdoVehLib,fGetVeh.ptr,b.p))b.get(fGetVeh,"ReturnValue",veh);}
        if(veh){uintptr_t vc=0;if(!rd(veh+O_CLASS,vc)||!isA(vc,clsVehicle))veh=0;}
        if(!veh)veh=nearestActor(clsVehicle,pl,5000.f);
    }
    if(!veh)return L"No vehicle nearby (get in one or stand next to it)";
    uintptr_t vcls=0;if(!rd(veh+O_CLASS,vcls))return L"Could not read vehicle class";
    std::unordered_set<std::string> wanted={"GetDefinition"};int ok=0,t=0;
    auto hits=scan(wanted,ok,t);
    auto it=hits.find("GetDefinition");if(it==hits.end())return L"GetDefinition not found";
    Fn gd;bool have=false;
    for(const Hit& h:it->second){
        uintptr_t outer=0;
        if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(vcls,outer)&&readFn(gd,h.obj,{"ReturnValue"})){have=true;break;}
    }
    if(!have)return L"Vehicle definition function not found for this vehicle";
    uintptr_t def=0;
    {Buf b(gd.size);if(!callPE(processEvent,veh,gd.ptr,b.p))return L"Engine call faulted (GetDefinition)";b.get(gd,"ReturnValue",def);}
    if(!def)return L"This vehicle has no definition";
    uintptr_t dcls=0;if(!rd(def+O_CLASS,dcls))return L"Could not read definition class";
    const int off=propOffset(dcls,"InventorySize");
    std::string vn,dn;objName(vcls,vn);objName(dcls,dn);
    if(off<0){logMessage((std::string("Trunk: InventorySize not found on ")+dn).c_str());return L"Trunk size property not found (see log)";}
    int cur=0;if(!rd(def+(uintptr_t)off,cur))return L"Could not read trunk size";
    current=cur;
    {char m[320];snprintf(m,sizeof(m),"Trunk: vehicle %s, definition %s, InventorySize at +0x%X = %d",vn.c_str(),dn.c_str(),(unsigned)off,cur);logMessage(m);}
    if(readOnly)return L"Current trunk size: "+std::to_wstring(cur);
    if(newSize<1||newSize>1000)return L"Size out of range (1-1000)";
    if(!writeInt(def+(uintptr_t)off,newSize))return L"Could not write trunk size";
    int back=0;rd(def+(uintptr_t)off,back);
    {char m[160];snprintf(m,sizeof(m),"Trunk: size changed %d -> %d (read back %d)",cur,newSize,back);logMessage(m);}
    if(back!=newSize)return L"Write did not stick";
    current=back;
    return L"Trunk size set to "+std::to_wstring(back)+L" (was "+std::to_wstring(cur)+L"). Re-enter the vehicle / reopen the trunk.";
}
// Set InventorySize on the definition of a vehicle actor (also usable before FinishSpawningActor).
bool setActorTrunk(uintptr_t actor,int size,int& oldSize,std::string& why){
    uintptr_t vcls=0;if(!rd(actor+O_CLASS,vcls)){why="could not read vehicle class";return false;}
    std::unordered_set<std::string> wanted={"GetDefinition"};int ok=0,t=0;
    auto hits=scan(wanted,ok,t);
    auto it=hits.find("GetDefinition");if(it==hits.end()){why="GetDefinition not found";return false;}
    Fn gd;bool have=false;
    for(const Hit& h:it->second){
        uintptr_t outer=0;
        if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(vcls,outer)&&readFn(gd,h.obj,{"ReturnValue"})){have=true;break;}
    }
    if(!have){why="definition function not found for this vehicle";return false;}
    uintptr_t def=0;
    {Buf b(gd.size);if(!callPE(processEvent,actor,gd.ptr,b.p)){why="engine call faulted (GetDefinition)";return false;}b.get(gd,"ReturnValue",def);}
    if(!def){why="vehicle has no definition yet";return false;}
    uintptr_t dcls=0;if(!rd(def+O_CLASS,dcls)){why="could not read definition class";return false;}
    const int off=propOffset(dcls,"InventorySize");
    if(off<0){why="InventorySize not found";return false;}
    int cur=0;if(!rd(def+(uintptr_t)off,cur)){why="could not read trunk size";return false;}
    if(!writeInt(def+(uintptr_t)off,size)){why="could not write trunk size";return false;}
    int back=0;rd(def+(uintptr_t)off,back);
    if(back!=size){why="write did not stick";return false;}
    oldSize=cur;
    std::string dn;objName(dcls,dn);
    char m[200];snprintf(m,sizeof(m),"Trunk (spawn): %s InventorySize %d -> %d",dn.c_str(),cur,size);logMessage(m);
    return true;
}
// Load button: ask the game to load this vehicle's class (the class itself is checked first).
uintptr_t propObj(uintptr_t cls,const char* name){
    for(int g=0;cls&&g<16;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<1024;++h){
            std::string n;if(objName(c,n)&&n==name)return c;
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    return 0;
}
bool writeByte(uintptr_t a,unsigned char v){__try{*(volatile unsigned char*)a=v;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// Hades: GetAllActorsOfClass(DynamicPawnSpawner) -> set bIsUnlimitedSpawningEnabled. We find the live spawner object(s) through GObjects.
std::wstring setUnlimitedSpawning(bool on){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    char m[300];
    std::unordered_set<std::string> w={"DynamicPawnSpawner"};int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t dcls=0;auto it=hits.find("DynamicPawnSpawner");
    if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="Class"){dcls=h.obj;break;}
    if(!dcls){logMessage("Unlimited spawning: DynamicPawnSpawner class not found");return L"Spawner class not found";}
    const uintptr_t prop=propObj(dcls,"bIsUnlimitedSpawningEnabled");
    int off=-1;if(prop)rd(prop+O_PROPOFF,off);
    if(!prop||off<0){logMessage("Unlimited spawning: property bIsUnlimitedSpawningEnabled not found");return L"Property not found";}
    // Bool properties: find FieldSize/ByteOffset/ByteMask/FieldMask in the property object. Native bool = 01 00 01 FF (full byte, as in the log); bitfield = mask bit.
    unsigned char pb[0x60]{};for(int i=0;i<0x60;++i)rd(prop+0x58+(uintptr_t)i,pb[i]);
    { char hx[0x60*3+8]{};for(int i=0;i<0x30;++i)snprintf(hx+i*3,4,"%02X ",pb[i]);
      snprintf(m,sizeof(m),"Unlimited spawning: prop=0x%llX offset=0x%X bytes@+58: %s",(unsigned long long)prop,off,hx);logMessage(m); }
    unsigned char fmask=0;
    for(int i=0;i+3<0x60;++i){
        if(pb[i]==1&&pb[i+1]==0&&pb[i+2]!=0&&pb[i+3]!=0){fmask=pb[i+3];
            snprintf(m,sizeof(m),"Unlimited spawning: bool mask 0x%02X found at property +0x%X",fmask,0x58+i);logMessage(m);break;}
    }
    if(!fmask){logMessage("Unlimited spawning: could not determine the bool mask - nothing written");return L"Could not read bool layout (see log)";}
    refreshCount();int found=0,wrote=0;
    for(int i=0;i<numObjs;++i){
        uintptr_t o=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
        uintptr_t c=0;if(!rd(o+O_CLASS,c)||!c)continue;
        if(c!=dcls&&!isA(c,dcls))continue;
        std::string nm;if(!objName(o,nm)||nm.compare(0,9,"Default__")==0)continue;
        unsigned flags=0;if(rd(o+8,flags)&&(flags&0x20000000u))continue;   // pending kill
        ++found;
        unsigned char b=0;if(!rd(o+(uintptr_t)off,b))continue;
        const unsigned char nb=on?(fmask==0xFF?1:(unsigned char)(b|fmask)):(fmask==0xFF?0:(unsigned char)(b&~fmask));
        unsigned char back=0;
        if(writeByte(o+(uintptr_t)off,nb)&&rd(o+(uintptr_t)off,back)&&back==nb){
            ++wrote;snprintf(m,sizeof(m),"Unlimited spawning: %s 0x%llX byte %02X -> %02X",nm.c_str(),(unsigned long long)o,b,nb);logMessage(m);
        }
    }
    snprintf(m,sizeof(m),"Unlimited spawning %s: spawner objects found=%d updated=%d",on?"ON":"OFF",found,wrote);logMessage(m);
    if(!found)return L"No spawner in the world yet (load into a save, then toggle again)";
    if(wrote!=found)return L"Only partly applied (see log)";
    return on?L"Unlimited spawning ON":L"Unlimited spawning OFF";
}
extern volatile LONG statsTrace;LONG CALLBACK statsCrashLogger(PEXCEPTION_POINTERS ep);
// ---- Skill Editor diagnostic (read-only) ---------------------------------------------------------------------
// Player pawn -> GetDaytonCharacter() -> GetSkills() (as in Hades). Logs the skill objects, their properties and the
// skill/level related functions, so the editor can be built on what the game really exposes.
std::wstring dumpSkills(){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    static void* veh2=AddVectoredExceptionHandler(1,statsCrashLogger);(void)veh2;
    struct TG{TG(){InterlockedIncrement(&statsTrace);}~TG(){InterlockedDecrement(&statsTrace);}} tg;
    char m[420];
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn (are you in game?)";
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    std::unordered_set<std::string> w={"GetDaytonCharacter","GetSkills","GetCurrentLevel","GetMaxLevel","GetExperienceFraction","AwardExperience","CanGainExperience","HasSkillAtLevel","PickSpecialization","OnLevelUp","GetChosenSpecialization","UnlearnSpecializationOfBaseSkill"};int ok=0,t=0;auto hits=scan(w,ok,t);
    auto findFn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    { std::string cn;objName(pcls,cn);snprintf(m,sizeof(m),"Skill dump: pawn=0x%llX class=%s",(unsigned long long)pawn,cn.c_str());logMessage(m); }
    Fn fDC,fSk;
    if(!findFn("GetDaytonCharacter",pcls,fDC)){logMessage("Skill dump: GetDaytonCharacter not found for the pawn class");return L"GetDaytonCharacter not found (see log)";}
    uintptr_t dc=0;
    {Buf b(fDC.size);if(!callPE(processEvent,pawn,fDC.ptr,b.p)){logMessage("Skill dump: GetDaytonCharacter call faulted");return L"Engine call faulted (GetDaytonCharacter)";}b.get(fDC,"ReturnValue",dc);}
    if(!dc){logMessage("Skill dump: GetDaytonCharacter returned null");return L"Survivor object is null";}
    uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
    { std::string cn;objName(dcls,cn);snprintf(m,sizeof(m),"Skill dump: survivor object=0x%llX class=%s",(unsigned long long)dc,cn.c_str());logMessage(m); }
    // Lists functions on a class chain whose names look skill / level / xp related.
    auto listFns=[&](uintptr_t cls,const char* tag){
        std::string line;
        for(int g=0;cls&&g<10;++g){
            std::string cn;objName(cls,cn);if(cn=="Object")break;
            uintptr_t c=0;rd(cls+O_CHILDREN,c);
            for(int h=0;c&&h<512;++h){
                std::string n,pt;uintptr_t pc=0;rd(c+O_CLASS,pc);objName(c,n);objName(pc,pt);
                if(pt=="Function"){
                    std::string lo=n;for(char& ch:lo)ch=(char)tolower((unsigned char)ch);
                    if(lo.find("skill")!=std::string::npos||lo.find("level")!=std::string::npos||lo.find("xp")!=std::string::npos||
                       lo.find("experience")!=std::string::npos||lo.find("special")!=std::string::npos||lo.find("learn")!=std::string::npos||lo.find("trait")!=std::string::npos){
                        line+=cn+"::"+n+"  ";
                        if(line.size()>300){snprintf(m,sizeof(m),"Skill dump: %s functions: %s",tag,line.c_str());logMessage(m);line.clear();}
                    }
                }
                uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
            }
            uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
        }
        if(!line.empty()){snprintf(m,sizeof(m),"Skill dump: %s functions: %s",tag,line.c_str());logMessage(m);}
    };
    listFns(dcls,"survivor");
    if(!findFn("GetSkills",dcls,fSk)){logMessage("Skill dump: GetSkills not found for the survivor class");return L"GetSkills not found (see log)";}
    struct TArr{uintptr_t data;int num,max;};TArr arr{};
    {Buf b(fSk.size);if(!callPE(processEvent,dc,fSk.ptr,b.p)){logMessage("Skill dump: GetSkills call faulted");return L"Engine call faulted (GetSkills)";}
     auto rv=fSk.off.find("ReturnValue");if(rv==fSk.off.end()||(size_t)rv->second+sizeof(arr)>b.n){logMessage("Skill dump: GetSkills has no ReturnValue");return L"GetSkills layout unexpected (see log)";}
     memcpy(&arr,(char*)b.p+rv->second,sizeof(arr));}
    snprintf(m,sizeof(m),"Skill dump: GetSkills -> data=0x%llX num=%d max=%d",(unsigned long long)arr.data,arr.num,arr.max);logMessage(m);
    if(!arr.data||arr.num<=0||arr.num>64)return L"Skill list empty or unexpected (see log)";
    // Raw look at the first element in case the array holds structs instead of object pointers.
    { unsigned char raw[32]{};for(int k=0;k<32;++k)rd(arr.data+(uintptr_t)k,raw[k]);char hx[100]{};for(int k=0;k<32;++k)snprintf(hx+k*3,4,"%02X ",raw[k]);
      snprintf(m,sizeof(m),"Skill dump: first 32 array bytes: %s",hx);logMessage(m); }
    bool fnsListed=false;int dumped=0;std::vector<uintptr_t> sk;
    for(int i=0;i<arr.num&&i<20;++i){
        uintptr_t o=0;if(!rd(arr.data+(uintptr_t)i*8,o)||!o){snprintf(m,sizeof(m),"Skill[%d]: unreadable pointer",i);logMessage(m);continue;}
        uintptr_t cls=0;if(!rd(o+O_CLASS,cls)||!cls){snprintf(m,sizeof(m),"Skill[%d]: 0x%llX is not an object",i,(unsigned long long)o);logMessage(m);continue;}
        std::string cn,nm,def="-";objName(cls,cn);objName(o,nm);
        const int doff=propOffset(cls,"SkillDefinition");
        if(doff>=0){uintptr_t dp=0;if(rd(o+(uintptr_t)doff,dp)&&dp)objName(dp,def);}
        snprintf(m,sizeof(m),"Skill[%d]: obj=0x%llX name=%s class=%s definition=%s",i,(unsigned long long)o,nm.c_str(),cn.c_str(),def.c_str());logMessage(m);
        if(!fnsListed){listFns(cls,"skill object");fnsListed=true;}
        int shown=0;uintptr_t cc=cls;
        for(int g=0;cc&&g<8&&shown<60;++g){
            std::string ccn;objName(cc,ccn);if(ccn=="Object")break;
            uintptr_t c=0;rd(cc+O_CHILDREN,c);
            for(int h=0;c&&h<256&&shown<60;++h){
                std::string n,pt;uintptr_t pc=0;rd(c+O_CLASS,pc);objName(c,n);objName(pc,pt);
                if(pt!="Function"){
                    int off=-1;rd(c+O_PROPOFF,off);
                    if(off>=0&&off<0x4000){
                        unsigned char raw[8]{};for(int k=0;k<8;++k)rd(o+(uintptr_t)off+(uintptr_t)k,raw[k]);
                        int iv=0;float fv=0;memcpy(&iv,raw,4);memcpy(&fv,raw,4);
                        snprintf(m,sizeof(m),"  [%d] %s.%s (%s) +0x%X raw=%02X %02X %02X %02X %02X %02X %02X %02X int=%d float=%g",i,ccn.c_str(),n.c_str(),pt.c_str(),off,
                            raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7],iv,fv);
                        logMessage(m);++shown;
                    }
                }
                uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
            }
            uintptr_t sup=0;if(!rd(cc+O_SUPER,sup))break;cc=sup;
        }
        ++dumped;sk.push_back(o);
    }
    // ---- step 2: signatures, current levels, and a search for where the level is stored ----
    auto sig=[&](const char* label,const Fn& f){
        std::string line=std::string("Skill dump: signature ")+label+" (parms size="+std::to_string(f.size)+"):";
        uintptr_t c=0;rd(f.ptr+O_CHILDREN,c);
        for(int g=0;c&&g<32;++g){
            std::string n,pt;uintptr_t pc=0;rd(c+O_CLASS,pc);objName(c,n);objName(pc,pt);int off=-1;rd(c+O_PROPOFF,off);
            line+=" "+n+":"+pt+"@"+std::to_string(off);
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        logMessage(line.c_str());
    };
    uintptr_t scls=0;if(!sk.empty())rd(sk[0]+O_CLASS,scls);
    Fn fLvl,fMax,fXp,fAward,fCan,fPending,fLvlUp,fChosen;Fn dAward,dHas,dPick,dUnl;
    const bool hLvl=scls&&findFn("GetCurrentLevel",scls,fLvl);
    const bool hMax=scls&&findFn("GetMaxLevel",scls,fMax);
    const bool hXp=scls&&findFn("GetExperienceFraction",scls,fXp);
    if(scls&&findFn("AwardExperience",scls,fAward))sig("CharacterSkill::AwardExperience",fAward);
    if(scls&&findFn("CanGainExperience",scls,fCan))sig("CharacterSkill::CanGainExperience",fCan);
    if(scls&&findFn("OnLevelUp",scls,fLvlUp))sig("CharacterSkill::OnLevelUp",fLvlUp);
    if(scls&&findFn("GetChosenSpecialization",scls,fChosen))sig("CharacterSkill::GetChosenSpecialization",fChosen);
    if(hLvl)sig("CharacterSkill::GetCurrentLevel",fLvl);
    if(hMax)sig("CharacterSkill::GetMaxLevel",fMax);
    if(hXp)sig("CharacterSkill::GetExperienceFraction",fXp);
    if(findFn("AwardExperience",dcls,dAward)&&dAward.ptr!=fAward.ptr)sig("DaytonCharacter::AwardExperience",dAward);
    if(findFn("HasSkillAtLevel",dcls,dHas))sig("DaytonCharacter::HasSkillAtLevel",dHas);
    if(findFn("PickSpecialization",dcls,dPick))sig("DaytonCharacter::PickSpecialization",dPick);
    if(findFn("UnlearnSpecializationOfBaseSkill",dcls,dUnl))sig("DaytonCharacter::UnlearnSpecializationOfBaseSkill",dUnl);
    std::vector<int> lv(sk.size(),-1);
    for(size_t i=0;i<sk.size();++i){
        int L=-1,M=-1;float X=-1;
        if(hLvl){Buf b(fLvl.size);if(callPE(processEvent,sk[i],fLvl.ptr,b.p)){auto r=fLvl.off.find("ReturnValue");if(r!=fLvl.off.end())memcpy(&L,(char*)b.p+r->second,4);if(L<0||L>255)L&=0xFF;}}
        if(hMax){Buf b(fMax.size);if(callPE(processEvent,sk[i],fMax.ptr,b.p)){auto r=fMax.off.find("ReturnValue");if(r!=fMax.off.end())memcpy(&M,(char*)b.p+r->second,4);if(M<0||M>255)M&=0xFF;}}
        if(hXp){Buf b(fXp.size);if(callPE(processEvent,sk[i],fXp.ptr,b.p)){auto r=fXp.off.find("ReturnValue");if(r!=fXp.off.end())memcpy(&X,(char*)b.p+r->second,4);}}
        lv[i]=L;
        std::string def="-";const int doff=propOffset(scls,"SkillDefinition");
        if(doff>=0){uintptr_t dp=0;if(rd(sk[i]+(uintptr_t)doff,dp)&&dp)objName(dp,def);}
        snprintf(m,sizeof(m),"Skill level[%zu] %s: level=%d max=%d xpFraction=%g",i,def.c_str(),L,M,(double)X);logMessage(m);
    }
    // Raw object bytes in the gap that has no reflected properties (+0xA8..+0x110), first three skills.
    for(size_t i=0;i<sk.size()&&i<3;++i){
        for(int row=0;row<7;++row){
            unsigned char raw[16]{};for(int k=0;k<16;++k)rd(sk[i]+(uintptr_t)(0xA8+row*16+k),raw[k]);
            char hx[60]{};for(int k=0;k<16;++k)snprintf(hx+k*3,4,"%02X ",raw[k]);
            snprintf(m,sizeof(m),"Skill raw[%zu] +0x%X: %s",i,0xA8+row*16,hx);logMessage(m);
        }
    }
    // Which byte holds the level? Look for a spot that matches GetCurrentLevel for EVERY skill (in the object, or behind a pointer in it).
    bool varied=false;for(size_t i=1;i<lv.size();++i)if(lv[i]!=lv[0])varied=true;
    snprintf(m,sizeof(m),"Skill dump: level search (levels %s)",varied?"vary - good":"all equal - results unreliable");logMessage(m);
    int cand=0;
    for(int B=0x28;B<0x140&&cand<12;++B){
        bool all=!sk.empty();
        for(size_t i=0;i<sk.size()&&all;++i){unsigned char v=0;if(!rd(sk[i]+(uintptr_t)B,v)||(int)v!=lv[i])all=false;}
        if(all){snprintf(m,sizeof(m),"Skill dump: level candidate: byte at object+0x%X",B);logMessage(m);++cand;}
    }
    for(int S2=0x28;S2<0x140&&cand<24;S2+=8){
        for(int K=0;K<0x40&&cand<24;++K){
            bool all=!sk.empty();
            for(size_t i=0;i<sk.size()&&all;++i){
                uintptr_t pv=0;if(!rd(sk[i]+(uintptr_t)S2,pv)||pv<0x10000||pv>0x7FFFFFFFFFFFull){all=false;break;}
                unsigned char v=0;if(!rd(pv+(uintptr_t)K,v)||(int)v!=lv[i])all=false;
            }
            if(all){snprintf(m,sizeof(m),"Skill dump: level candidate: byte at [object+0x%X]+0x%X",S2,K);logMessage(m);++cand;}
        }
    }
    snprintf(m,sizeof(m),"Skill dump: level search done, %d candidate(s)",cand);logMessage(m);
    // ---- step 3: where does GetCurrentLevel really read the level? Raw native fields of every skill + the function's code ----
    for(size_t i=0;i<sk.size();++i){
        for(int row=0;row<5;++row){
            unsigned char raw[16]{};for(int k=0;k<16;++k)rd(sk[i]+(uintptr_t)(0x28+row*16+k),raw[k]);
            char hx[60]{};for(int k=0;k<16;++k)snprintf(hx+k*3,4,"%02X ",raw[k]);
            snprintf(m,sizeof(m),"Skill native[%zu] L%d +0x%X: %s",i,lv[i],0x28+row*16,hx);logMessage(m);
        }
    }
    if(hLvl){
        auto code=[&](const char* tag,uintptr_t addr,int n){
            for(int row=0;row*16<n;++row){
                unsigned char raw[16]{};for(int k=0;k<16;++k)rd(addr+(uintptr_t)(row*16+k),raw[k]);
                char hx[60]{};for(int k=0;k<16;++k)snprintf(hx+k*3,4,"%02X ",raw[k]);
                snprintf(m,sizeof(m),"Skill code %s +0x%X: %s",tag,row*16,hx);logMessage(m);
            }
        };
        for(int off=0x40;off<0x140;off+=8){
            uintptr_t v=0;if(!rd(fLvl.ptr+(uintptr_t)off,v))continue;
            if(v<base+0x1000||v>=base+0x4000000)continue;
            snprintf(m,sizeof(m),"Skill dump: GetCurrentLevel UFunction+0x%X -> module+0x%llX",off,(unsigned long long)(v-base));logMessage(m);
            code("thunk",v,0x60);
            for(int k=0;k<0x5B;++k){
                unsigned char b=0;rd(v+(uintptr_t)k,b);
                if(b==0xE8){
                    int rel=0;rd(v+(uintptr_t)k+1,rel);
                    const uintptr_t tgt=v+(uintptr_t)k+5+(intptr_t)rel;
                    if(tgt>=base+0x1000&&tgt<base+0x4000000){
                        snprintf(m,sizeof(m),"Skill dump: thunk call at +0x%X -> module+0x%llX",k,(unsigned long long)(tgt-base));logMessage(m);
                        code("callee",tgt,0x80);break;
                    }
                }
            }
        }
    }
    logMessage("Skill dump: finished");
    return L"Skill dump written to ADREENA_debug.log ("+std::to_wstring(dumped)+L" skills)";
}
// ---- Skill Editor ---------------------------------------------------------------------------------------------
struct SkillCtx{uintptr_t dc=0,scls=0;std::vector<uintptr_t> objs;Fn lvl,mx,xp,award;};
bool skillContext(SkillCtx& c,std::wstring& err){
    std::string why;if(!init(why)){err=L"Unavailable: "+widen(why);return false;}
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world){err=L"No game world yet (load into a save)";return false;}
    const uintptr_t pawn=playerPawn(world);if(!pawn){err=L"No player pawn (are you in game?)";return false;}
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    std::unordered_set<std::string> w={"GetDaytonCharacter","GetSkills","GetCurrentLevel","GetMaxLevel","GetExperienceFraction","AwardExperience"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    auto findFn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    Fn fDC,fSk;
    if(!findFn("GetDaytonCharacter",pcls,fDC)){err=L"GetDaytonCharacter not found";return false;}
    {Buf b(fDC.size);if(!callPE(processEvent,pawn,fDC.ptr,b.p)){err=L"Engine call faulted (GetDaytonCharacter)";return false;}b.get(fDC,"ReturnValue",c.dc);}
    if(!c.dc){err=L"Survivor object is null";return false;}
    uintptr_t dcls=0;rd(c.dc+O_CLASS,dcls);
    if(!findFn("GetSkills",dcls,fSk)){err=L"GetSkills not found";return false;}
    struct TArr{uintptr_t data;int num,max;};TArr arr{};
    {Buf b(fSk.size);if(!callPE(processEvent,c.dc,fSk.ptr,b.p)){err=L"Engine call faulted (GetSkills)";return false;}
     auto rv=fSk.off.find("ReturnValue");if(rv==fSk.off.end()||(size_t)rv->second+sizeof(arr)>b.n){err=L"GetSkills layout unexpected";return false;}
     memcpy(&arr,(char*)b.p+rv->second,sizeof(arr));}
    if(!arr.data||arr.num<=0||arr.num>32){err=L"No skills found on this survivor";return false;}
    for(int i=0;i<arr.num;++i){uintptr_t o=0;if(rd(arr.data+(uintptr_t)i*8,o)&&o)c.objs.push_back(o);}
    if(c.objs.empty()){err=L"Skill list unreadable";return false;}
    rd(c.objs[0]+O_CLASS,c.scls);
    if(!c.scls||!findFn("GetCurrentLevel",c.scls,c.lvl)||!findFn("GetMaxLevel",c.scls,c.mx)||!findFn("GetExperienceFraction",c.scls,c.xp)||!findFn("AwardExperience",c.scls,c.award)){
        err=L"Skill functions not found (see log)";return false;}
    return true;
}
int skillByte(uintptr_t o,const Fn& f){
    Buf b(f.size);if(!callPE(processEvent,o,f.ptr,b.p))return -1;
    auto r=f.off.find("ReturnValue");if(r==f.off.end())return -1;return *((unsigned char*)b.p+r->second);
}
float skillXp(uintptr_t o,const Fn& f){
    Buf b(f.size);if(!callPE(processEvent,o,f.ptr,b.p))return -1.f;
    auto r=f.off.find("ReturnValue");if(r==f.off.end())return -1.f;float v=-1.f;memcpy(&v,(char*)b.p+r->second,4);return v;
}
std::wstring skillDefName(uintptr_t scls,uintptr_t o){
    std::string def="?";const int doff=propOffset(scls,"SkillDefinition");
    if(doff>=0){uintptr_t dp=0;if(rd(o+(uintptr_t)doff,dp)&&dp)objName(dp,def);}
    const std::string suf="Definition";
    if(def.size()>suf.size()&&def.compare(def.size()-suf.size(),suf.size(),suf)==0)def.erase(def.size()-suf.size());
    return widen(def);
}
std::wstring readSkills(std::vector<SkillRow>& rows){
    SkillCtx c;std::wstring err;if(!skillContext(c,err))return err;
    const int aoff=propOffset(c.scls,"AvailableSpecializations");
    rows.clear();
    for(uintptr_t o:c.objs){
        SkillRow r;r.name=skillDefName(c.scls,o);
        r.level=skillByte(o,c.lvl);r.max=skillByte(o,c.mx);
        uintptr_t ap=0;if(aoff>=0)rd(o+(uintptr_t)aoff,ap);
        r.base=ap!=0;   // specialized skills have no list of specializations of their own
        if(r.level<0)r.level=0;if(r.max<=0)r.max=7;
        rows.push_back(r);
    }
    return L"Loaded "+std::to_wstring(rows.size())+L" skills";
}
// GetCurrentLevel is `sub rsp,28h; call H; movzx eax,[rax+8]; ...` : H(skill) returns the skill's level record, level at +8
// (the same +8 byte the cheat-table Max Skills hook edits). Find H at runtime from that code so it survives rebuilds.
uintptr_t callHelper(uintptr_t fn,uintptr_t self){__try{return ((uintptr_t(*)(void*))fn)((void*)self);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
uintptr_t findLevelHelper(uintptr_t fnPtr){
    for(int off=0x40;off<0x140;off+=8){
        uintptr_t v=0;if(!rd(fnPtr+(uintptr_t)off,v)||v<base+0x1000||v>=base+0x4000000)continue;
        for(int k=0;k<0x5B;++k){
            unsigned char b=0;rd(v+(uintptr_t)k,b);if(b!=0xE8)continue;
            int rel=0;rd(v+(uintptr_t)k+1,rel);
            const uintptr_t callee=v+(uintptr_t)k+5+(intptr_t)rel;
            if(callee<base+0x1000||callee>=base+0x4000000)break;
            unsigned char cb[14]{};for(int i=0;i<14;++i)rd(callee+(uintptr_t)i,cb[i]);
            if(cb[0]==0x48&&cb[1]==0x83&&cb[2]==0xEC&&cb[3]==0x28&&cb[4]==0xE8&&cb[9]==0x0F&&cb[10]==0xB6&&cb[11]==0x40&&cb[12]==0x08){
                int r2=0;rd(callee+5,r2);return callee+9+(uintptr_t)(intptr_t)r2;
            }
            break;
        }
    }
    return 0;
}
// Raise through the game's own AwardExperience (so level-up effects run); lower by writing the level bytes the dump found.
std::wstring setSkillLevel(const std::wstring& name,int target){
    SkillCtx c;std::wstring err;if(!skillContext(c,err))return err;
    static void* veh3=AddVectoredExceptionHandler(1,statsCrashLogger);(void)veh3;
    struct TG{TG(){InterlockedIncrement(&statsTrace);}~TG(){InterlockedDecrement(&statsTrace);}} tg;
    char m[300];
    uintptr_t o=0;for(uintptr_t obj:c.objs)if(skillDefName(c.scls,obj)==name){o=obj;break;}
    if(!o)return L"Skill not found on the current survivor (press Refresh)";
    const int mx=skillByte(o,c.mx),cur0=skillByte(o,c.lvl);
    if(mx<0||cur0<0)return L"Could not read the skill level";
    if(target>mx)target=mx;if(target<0)target=0;
    if(target==cur0)return name+L" is already level "+std::to_wstring(cur0);
    auto award=[&](float amt){Buf b(c.award.size);b.put(c.award,"Experience",amt);return callPE(processEvent,o,c.award.ptr,b.p);};
    int cur=cur0;std::wstring note;
    snprintf(m,sizeof(m),"Skill edit: %ls level %d -> %d",name.c_str(),cur0,target);logMessage(m);
    if(target>cur){
        for(int pass=0;pass<12&&cur<target;++pass){
            const float f0=skillXp(o,c.xp);
            if(!award(1.0f)){note=L" (engine call faulted)";break;}
            const int l1=skillByte(o,c.lvl);
            if(l1>cur){cur=l1;continue;}
            const float f1=skillXp(o,c.xp);const float d=f1-f0;
            if(d<=1e-9f){note=L" (this skill cannot gain experience right now)";break;}
            const float need=(1.0f-f1)/d;   // experience still needed for the next level
            snprintf(m,sizeof(m),"Skill edit: pass %d level=%d xpFrac=%.4f need~%.2f",pass,cur,(double)f1,(double)need);logMessage(m);
            award(need*1.02f+0.01f);
            int l2=skillByte(o,c.lvl);
            if(l2<=cur){award(need*2.f+1.f);l2=skillByte(o,c.lvl);}
            if(l2<=cur){note=L" (level did not advance)";break;}
            cur=l2;
        }
    }else{
        const uintptr_t helper=findLevelHelper(c.lvl.ptr);
        const uintptr_t rec=helper?callHelper(helper,o):0;
        unsigned char old=0;bool done=false;
        snprintf(m,sizeof(m),"Skill edit: level record helper=module+0x%llX record=0x%llX",(unsigned long long)(helper?helper-base:0),(unsigned long long)rec);logMessage(m);
        if(rec&&rd(rec+8,old)&&old==(unsigned char)cur0){
            writeByte(rec+8,(unsigned char)target);
            if(skillByte(o,c.lvl)==target)done=true;else writeByte(rec+8,old);
        }
        if(!done)note=helper?L" (could not lower safely - restored)":L" (level record not found)";
        snprintf(m,sizeof(m),"Skill edit: lower %s (record byte was %02X)",done?"ok":"failed",old);logMessage(m);
    }
    const int now=skillByte(o,c.lvl);
    snprintf(m,sizeof(m),"Skill edit: result level=%d",now);logMessage(m);
    return name+L": level "+std::to_wstring(cur0)+L" -> "+std::to_wstring(now<0?cur:now)+note;
}
// ---- Skill removal --------------------------------------------------------------------------------------------
// GetSkills hands back a COPY of the survivor's skill array, so removing a base skill means finding the real TArray inside the
// game's objects (same pointers, same count) and taking the element out of it. Specialized skills are removed through the game's
// own UnlearnSpecializationOfBaseSkill on the base skill that owns them.
bool safeMoveMem(uintptr_t dst,uintptr_t src,size_t n){__try{memmove((void*)dst,(void*)src,n);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool findSkillArray(uintptr_t owner,const std::vector<uintptr_t>& objs,uintptr_t span,uintptr_t& arrAddr){
    for(uintptr_t off=0;off<span;off+=8){
        uintptr_t d=0;int num=0,mx=0;
        if(!rd(owner+off,d)||!d||(d&7))continue;
        if(!rd(owner+off+8,num)||num!=(int)objs.size())continue;
        if(!rd(owner+off+12,mx)||mx<num||mx>4096)continue;
        bool all=true;
        for(uintptr_t want:objs){
            bool hit=false;
            for(int i=0;i<num&&!hit;++i){uintptr_t o=0;if(rd(d+(uintptr_t)i*8,o)&&o==want)hit=true;}
            if(!hit){all=false;break;}
        }
        if(all){arrAddr=owner+off;return true;}
    }
    return false;
}
// Takes the skill object out of the survivor's real skill array. Verified through GetSkills; restored if it does not take.
std::wstring removeFromSkillArray(const SkillCtx& c,uintptr_t skill,const std::wstring& name,bool& removed){
    removed=false;char m[260];
    uintptr_t world=0;rd(base+RVA_GWORLD,world);const uintptr_t pawn=playerPawn(world);
    std::vector<uintptr_t> owners={c.dc};
    uintptr_t dcls=0;rd(c.dc+O_CLASS,dcls);
    uintptr_t arr=0,owner=0;
    if(findSkillArray(c.dc,c.objs,0x1800,arr))owner=c.dc;
    else for(uintptr_t off=0x28;off<0xC00;off+=8){   // objects the survivor points at (components) can hold the array too
        uintptr_t p=0;if(!rd(c.dc+off,p)||!p||(p&7)||p<0x10000)continue;
        uintptr_t pc=0;if(!rd(p+O_CLASS,pc)||!pc||(pc&7))continue;
        owners.push_back(p);if(owners.size()>300)break;
    }
    if(pawn)owners.push_back(pawn);
    if(!owner)for(uintptr_t ow:owners){
        if(findSkillArray(ow,c.objs,ow==c.dc?0x1800:0x600,arr)){owner=ow;break;}
    }
    if(!arr){logMessage("Skill remove: could not locate the survivor's real skill array");return L"Could not locate the survivor's skill list in memory (see log)";}
    snprintf(m,sizeof(m),"Skill remove: real array at owner+0x%llX (owner %s), %d entries",(unsigned long long)(arr-owner),owner==c.dc?"survivor":"sub-object",(int)c.objs.size());logMessage(m);
    uintptr_t data=0;int num=0;rd(arr,data);rd(arr+8,num);
    int idx=-1;for(int i=0;i<num;++i){uintptr_t o=0;rd(data+(uintptr_t)i*8,o);if(o==skill){idx=i;break;}}
    if(idx<0)return L"Skill not found in the survivor's skill list";
    std::vector<uintptr_t> backup(num);for(int i=0;i<num;++i)rd(data+(uintptr_t)i*8,backup[i]);
    if(num-1-idx>0&&!safeMoveMem(data+(uintptr_t)idx*8,data+(uintptr_t)(idx+1)*8,(size_t)(num-1-idx)*8))return L"Could not edit the skill list";
    if(!safeWriteInt(arr+8,num-1)){for(int i=0;i<num;++i)safeMoveMem(data+(uintptr_t)i*8,(uintptr_t)&backup[i],8);return L"Could not edit the skill list";}
    SkillCtx c2;std::wstring err;bool still=true;
    if(skillContext(c2,err)){still=false;for(uintptr_t o:c2.objs)if(o==skill){still=true;break;}}
    else{still=true;}
    if(still){   // restore
        for(int i=0;i<num;++i)safeMoveMem(data+(uintptr_t)i*8,(uintptr_t)&backup[i],8);
        safeWriteInt(arr+8,num);
        logMessage("Skill remove: array edit did not take effect - restored");
        return L"Could not remove "+name+L" safely - restored";
    }
    removed=true;logMessage("Skill remove: array edit verified");
    return L"";
}
// baseSkill selects the removal kind (true: remove the base skill itself; false: remove one specialization, keeping its base skill).
std::wstring removeSkill(const std::wstring& name,bool baseSkill){
    SkillCtx c;std::wstring err;if(!skillContext(c,err))return err;
    static void* veh4=AddVectoredExceptionHandler(1,statsCrashLogger);(void)veh4;
    struct TG{TG(){InterlockedIncrement(&statsTrace);}~TG(){InterlockedDecrement(&statsTrace);}} tg;
    char m[300];
    uintptr_t o=0;for(uintptr_t obj:c.objs)if(skillDefName(c.scls,obj)==name){o=obj;break;}
    if(!o)return L"Skill not found on the current survivor (press Refresh)";
    const int aoff=propOffset(c.scls,"AvailableSpecializations");
    uintptr_t ap=0;if(aoff>=0)rd(o+(uintptr_t)aoff,ap);
    const bool isBase=ap!=0;
    if(isBase!=baseSkill)return L"That skill is not in this tab (press Refresh)";
    snprintf(m,sizeof(m),"Skill remove: %ls (%s)",name.c_str(),baseSkill?"base":"specialization");logMessage(m);
    if(baseSkill){
        return L"Base skills cannot be removed";   // the game keeps them in save data; a live list edit does not stick
    }
    // Specialized: find the base skill that chose it, then use the game's own unlearn on that base skill.
    uintptr_t world=0;rd(base+RVA_GWORLD,world);const uintptr_t pawn=playerPawn(world);
    std::unordered_set<std::string> w={"GetCharacterComponent","UnlearnSpecializationOfBaseSkill","GetChosenSpecialization"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    auto findFn=[&](const char* nm,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(nm);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    uintptr_t dcls=0;rd(c.dc+O_CLASS,dcls);
    uintptr_t comp=0;{Fn fComp;if(findFn("GetCharacterComponent",dcls,fComp)){Buf b(fComp.size);if(callPE(processEvent,c.dc,fComp.ptr,b.p))b.get(fComp,"ReturnValue",comp);}}
    Fn fUn,fChosen;uintptr_t target=0;
    {uintptr_t tg2[3]={c.dc,comp,pawn};
     for(int i=0;i<3&&!target;++i){if(!tg2[i])continue;uintptr_t tc=0;rd(tg2[i]+O_CLASS,tc);if(findFn("UnlearnSpecializationOfBaseSkill",tc,fUn))target=tg2[i];}}
    const bool haveChosen=findFn("GetChosenSpecialization",c.scls,fChosen);
    uintptr_t owner=0;
    if(haveChosen){
        for(uintptr_t sk:c.objs){
            uintptr_t bp=0;if(aoff>=0)rd(sk+(uintptr_t)aoff,bp);if(!bp)continue;   // base skills only
            Buf b(fChosen.size);if(!callPE(processEvent,sk,fChosen.ptr,b.p))continue;
            uintptr_t got=0;b.get(fChosen,"ReturnValue",got);
            snprintf(m,sizeof(m),"Skill remove: base %ls chose 0x%llX (looking for 0x%llX)",skillDefName(c.scls,sk).c_str(),(unsigned long long)got,(unsigned long long)o);logMessage(m);
            if(got==o){owner=sk;break;}
        }
    }else logMessage("Skill remove: GetChosenSpecialization not found");
    if(owner&&target){
        const auto ps=sortedParams(fUn);
        if(ps.size()==1){
            Buf b(fUn.size);b.put(fUn,ps[0].first.c_str(),owner);
            const bool called=callPE(processEvent,target,fUn.ptr,b.p);
            unsigned char r=0;b.get(fUn,"ReturnValue",r);
            snprintf(m,sizeof(m),"Skill remove: unlearn call=%d returned=%d",called?1:0,(int)r);logMessage(m);
            SkillCtx c2;std::wstring e2;bool still=true;
            if(skillContext(c2,e2)){still=false;for(uintptr_t x:c2.objs)if(x==o){still=true;break;}}
            if(!still)return name+L" removed (base skill kept)";
            logMessage("Skill remove: specialization still listed after unlearn");
        }
    }
    logMessage("Skill remove: falling back to editing the skill list directly");
    bool removed=false;const std::wstring r=removeFromSkillArray(c,o,name,removed);
    return removed?name+L" removed (list edit)":r;
}
std::wstring loadZombieClass(int idx){
    if(idx<0||idx>=zombieCount)return L"Bad selection";
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    if(classFor(idx))return L"ALREADY";
    const std::wstring r=requestLoad(zombieDefs[idx].pkg,zombieDefs[idx].cls);
    if(r.empty())return std::wstring(L"Loading ")+zombieDefs[idx].label+L" zombie class into memory...";
    return L"Could not start loading: "+r;
}
std::wstring vehicleName(int idx){
    for(int f=0;f<vehicleFamilyCount;++f){
        const VehicleFamily& vf=vehicleFamilies[f];
        if(idx>=vf.first&&idx<vf.first+vf.count)return std::wstring(vf.name)+L" ("+vehicleDefs[idx].variant+L")";
    }
    return widen(vehicleDefs[idx].cls);
}
std::wstring loadVehicleClass(int idx){
    if(idx<0||idx>=vehicleCount)return L"Bad selection";
    std::string why;if(!initVehicle(why))return L"Unavailable: "+widen(why);
    if(blueprintClass(vehicleDefs[idx].pkg,vehicleDefs[idx].cls,&vehCache[idx]))return L"ALREADY";
    const std::wstring r=requestLoad(vehicleDefs[idx].pkg,vehicleDefs[idx].cls);
    if(r.empty())return L"Loading "+vehicleName(idx)+L" vehicle class into memory...";
    return L"Could not start loading: "+r;
}
void postVehicleRecheck(uintptr_t actor,int index){
    HWND w=gameWindow;
    std::thread([w,actor,index]{Sleep(2500);if(w)PostMessageW(w,WM_ADREENA_VCHECK,(WPARAM)actor,(LPARAM)index);}).detach();
}
std::wstring spawnVehicle(int idx,bool registerIt,int trunkSize=0){
    if(idx<0||idx>=vehicleCount)return L"Bad selection";
    std::string why;if(!initVehicle(why))return L"Unavailable: "+widen(why);
    static bool listed=false;if(!listed){listed=true;logLoadedVehicles();}
    uintptr_t world=0;
    if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t cls=blueprintClass(vehicleDefs[idx].pkg,vehicleDefs[idx].cls,&vehCache[idx]);
    if(!cls){
        const std::wstring r=requestLoad(vehicleDefs[idx].pkg,vehicleDefs[idx].cls);
        if(r.empty())return L"Loading "+vehicleName(idx)+L" vehicle class into memory...";
        return L"Vehicle class not loaded in memory yet; loader: "+r;
    }
    const uintptr_t pawn=playerPawn(world);
    if(!pawn)return L"No player pawn (are you in game?)";
    float loc[3]{},fwd[3]{};
    if(!actorLocation(pawn,loc))return L"Could not read player location";
    {Buf b(fFwd.size);if(!callPE(processEvent,pawn,fFwd.ptr,b.p)||!b.get(fFwd,"ReturnValue",fwd))return L"Could not read facing";}
    for(float v:{loc[0],loc[1],loc[2],fwd[0],fwd[1],fwd[2]})if(!(v==v)||v>1e9f||v<-1e9f)return L"Bad player location";
    // 7 m ahead and 1.5 m up, so the vehicle drops in front of you rather than onto you.
    float xf[12]={0,0,0,1, loc[0]+fwd[0]*700.f,loc[1]+fwd[1]*700.f,loc[2]+fwd[2]*700.f+150.f,0, 1,1,1,0};
    // Hades spawns with the player's own transform (rotation and scale included) moved 250 units along Y.
    if(fGetTf.ptr){
        Buf b(fGetTf.size);
        if(callPE(processEvent,pawn,fGetTf.ptr,b.p)){
            float t[12]{};
            if(b.get(fGetTf,"ReturnValue",t)){bool fin=true;for(float v:t)if(!(v==v)||v>1e9f||v<-1e9f)fin=false;
                if(fin){t[5]-=250.f;memcpy(xf,t,sizeof(xf));logMessage("Vehicle placement: using the player's transform (like Hades)");}}
        }
    }
    {uintptr_t existing=nearestActor(clsVehicle,loc,1e9f);if(existing)probeActor("Vehicle probe (existing vehicle)",existing);else logMessage("Vehicle probe: no existing vehicle found");}
    uintptr_t actor=0;
    {Buf b(fBegin.size);
     b.put(fBegin,"WorldContextObject",world);b.put(fBegin,"ActorClass",cls);b.putBytes(fBegin,"SpawnTransform",xf,sizeof(xf));
     b.put(fBegin,"CollisionHandlingOverride",(unsigned char)2);
     if(!callPE(processEvent,cdoStatics,fBegin.ptr,b.p))return L"Engine call faulted (spawn begin)";
     b.get(fBegin,"ReturnValue",actor);}
    if(!actor)return L"Spawn returned nothing";
    // Trunk size: set it on the vehicle's definition before the vehicle finishes spawning, so its inventory is created with it.
    bool trunkDone=false;std::string trunkWhy;int trunkOld=0;
    if(trunkSize>0)trunkDone=setActorTrunk(actor,trunkSize,trunkOld,trunkWhy);
    {Buf b(fFinish.size);b.put(fFinish,"Actor",actor);b.putBytes(fFinish,"SpawnTransform",xf,sizeof(xf));
     if(!callPE(processEvent,cdoStatics,fFinish.ptr,b.p))return L"Engine call faulted (spawn finish)";}
    if(trunkSize>0&&!trunkDone){
        trunkDone=setActorTrunk(actor,trunkSize,trunkOld,trunkWhy);   // some vehicles only have a definition once spawned
        if(trunkDone)logMessage("Trunk (spawn): set after the vehicle finished spawning (may only apply to the next vehicle of this type)");
    }
    if(trunkSize>0&&!trunkDone)logMessage((std::string("Trunk (spawn) failed: ")+trunkWhy).c_str());
    std::wstring result=L"Spawned vehicle";
    if(trunkSize>0)result+=trunkDone?(L" with "+std::to_wstring(trunkSize)+L" trunk slots"):L" (trunk size not changed)";
    // Vehicles came out of the spawn at the world origin (0,0,0), so place them explicitly.
    if(fTele.ptr){
        Buf b(fTele.size);const float loc3[3]={xf[4],xf[5],xf[6]};const float rot3[3]={0,0,0};
        b.putBytes(fTele,"DestLocation",loc3,12);b.putBytes(fTele,"DestRotation",rot3,12);
        if(!callPE(processEvent,actor,fTele.ptr,b.p))logMessage("Vehicle placement: teleport call faulted");
        else{bool okTp=false;b.get(fTele,"ReturnValue",okTp);logMessage(okTp?"Vehicle placement: teleport ok":"Vehicle placement: teleport returned false");}
    }else logMessage("Vehicle placement: K2_TeleportTo not found");
    probeActor("Vehicle probe (spawned)",actor);
    int actorIndex=-1;rd(actor+0xC,actorIndex);
    {const float want[3]={xf[4],xf[5],xf[6]};describeActor("Vehicle spawn check (now)",actor,actorIndex,want);
     g_vehWant[0]=want[0];g_vehWant[1]=want[1];g_vehWant[2]=want[2];}
    if(registerIt){
        if(!fRegister.ptr)result+=L" (register function not found)";
        else{
            uintptr_t veh=0;
            {Buf b(fGetVeh.size);const auto gp=sortedParams(fGetVeh);b.put(fGetVeh,gp[0].first.c_str(),actor);
             if(callPE(processEvent,cdoVehLib,fGetVeh.ptr,b.p))b.get(fGetVeh,"ReturnValue",veh);}
            if(!veh)result+=L" (not registered: no vehicle component)";
            else{
                Buf b(fRegister.size);const auto rp=sortedParams(fRegister);b.put(fRegister,rp[0].first.c_str(),veh);
                result+=callPE(processEvent,cdoVehLib,fRegister.ptr,b.p)?L" and registered":L" (register call faulted)";
            }
        }
    }
    {const float want[3]={xf[4],xf[5],xf[6]};describeActor("Vehicle spawn check (after register)",actor,actorIndex,want);}
    logMessage((std::string("Spawned vehicle: ")+vehicleDefs[idx].cls).c_str());
    postVehicleRecheck(actor,actorIndex);
    return result;
}
// Byte offset of a named property on a class (searching its parents), or -1.
int propOffset(uintptr_t cls,const char* name){
    for(int g=0;cls&&g<16;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<1024;++h){
            std::string n;
            if(objName(c,n)&&n==name){int off=-1;if(rd(c+O_PROPOFF,off)&&off>=0)return off;return -1;}
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    return -1;
}
volatile LONG statsTrace=0;   // >0 while applyZombieStats runs; lets the crash logger below record what failed
LONG CALLBACK statsCrashLogger(PEXCEPTION_POINTERS ep){
    if(statsTrace>0&&ep&&ep->ExceptionRecord){
        const DWORD c=ep->ExceptionRecord->ExceptionCode;
        if(c!=0x406D1388&&c!=0x40010006&&c!=0xE06D7363&&(c&0xF0000000)==0xC0000000){   // serious errors only (not C++/debug noise)
            char m[300];HMODULE mod=nullptr;char mn[MAX_PATH]="?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)ep->ExceptionRecord->ExceptionAddress,&mod);
            if(mod)GetModuleFileNameA(mod,mn,MAX_PATH);
            const char* sl=strrchr(mn,'\\');
            snprintf(m,sizeof(m),"CRASH during stats/diagnostic step: code=0x%08lX addr=0x%llX module=%s +0x%llX",(unsigned long)c,
                (unsigned long long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,sl?sl+1:mn,
                (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress-(uintptr_t)mod));
            logMessage(m);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
bool writeFloat(uintptr_t a,float v){__try{*(float*)a=v;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
// Hades: SetHealth(value) and BloodPlagueActionComponent.damageMult_Melee / damageMult_Ranged on the spawned zombie.
std::wstring applyZombieStats(uintptr_t actor,int hp,float melee,float ranged){
    std::wstring out;char m[240];
    uintptr_t acls=0;if(!rd(actor+O_CLASS,acls))return L" (stats not applied: could not read class)";
    static void* veh=AddVectoredExceptionHandler(1,statsCrashLogger);(void)veh;
    struct TraceGuard{TraceGuard(){InterlockedIncrement(&statsTrace);}~TraceGuard(){InterlockedDecrement(&statsTrace);}} traceGuard;
    { std::string cn;objName(acls,cn);
      if(cn.find("PlagueNode")==std::string::npos){
          snprintf(m,sizeof(m),"Zombie stats: skipped - %s is not a Plague Heart (Hades only sets these on hearts)",cn.c_str());logMessage(m);
          return L" (heart stats skipped: only Plague Hearts have them)";
      } }
    { std::string cn;objName(acls,cn);snprintf(m,sizeof(m),"Zombie stats: begin actor=0x%llX class=%s hp=%d melee=%.2f ranged=%.2f",
        (unsigned long long)actor,cn.c_str(),hp,melee,ranged);logMessage(m); }
    // --- health ---
    {
        bool done=false;std::string why="SetHealth not found for this zombie";
        std::unordered_set<std::string> wanted={"SetHealth"};int ok=0,t=0;auto hits=scan(wanted,ok,t);
        auto it=hits.find("SetHealth");
        if(it!=hits.end())for(const Hit& h:it->second){
            uintptr_t outer=0;Fn fn;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(acls,outer)||!readFn(fn,h.obj,{}))continue;
            const auto ps=sortedParams(fn);if(ps.empty()){why="SetHealth has no parameters";continue;}
            { std::string on;objName(outer,on);snprintf(m,sizeof(m),"Zombie stats: SetHealth candidate fn=0x%llX outer=%s params=%d size=%d first=%s",
                (unsigned long long)fn.ptr,on.c_str(),(int)ps.size(),fn.size,ps[0].first.c_str());logMessage(m); }
            Buf b(fn.size);b.put(fn,ps[0].first.c_str(),(float)hp);
            logMessage("Zombie stats: calling SetHealth...");
            const bool called=callPE(processEvent,actor,fn.ptr,b.p);
            if(called){logMessage("Zombie stats: SetHealth returned");done=true;break;}
            why="engine call faulted (SetHealth)";
        }
        if(done)snprintf(m,sizeof(m),"Zombie stats: health set to %d",hp);else snprintf(m,sizeof(m),"Zombie stats: health NOT set (%s)",why.c_str());
        logMessage(m);
        out+=done?(L" HP "+std::to_wstring(hp)):L" (health not set)";
    }
    // --- damage multipliers ---
    {
        logMessage("Zombie stats: looking up BloodPlagueActionComponent");
        const int coff=propOffset(acls,"BloodPlagueActionComponent");
        snprintf(m,sizeof(m),"Zombie stats: component offset=%d",coff);logMessage(m);
        uintptr_t comp=0;if(coff>=0)rd(actor+(uintptr_t)coff,comp);
        uintptr_t ccls=0;if(comp)rd(comp+O_CLASS,ccls);
        int okCount=0;
        if(ccls){
            const struct{const char* name;float v;}mult[2]={{"damageMult_Melee",melee},{"damageMult_Ranged",ranged}};
            for(const auto& mm:mult){
                const int off=propOffset(ccls,mm.name);
                if(off>=0&&writeFloat(comp+(uintptr_t)off,mm.v)){++okCount;snprintf(m,sizeof(m),"Zombie stats: %s = %.2f",mm.name,mm.v);}
                else snprintf(m,sizeof(m),"Zombie stats: %s not set",mm.name);
                logMessage(m);
            }
        }else logMessage("Zombie stats: this zombie type has no BloodPlagueActionComponent");
        char t2[64];snprintf(t2,sizeof(t2)," melee x%.1f ranged x%.1f",melee,ranged);
        out+=okCount==2?widen(t2):L" (damage multipliers not available for this type)";
    }
    return out;
}
// Hades: PlayerController.BountyVehicleDeliveryProtocol.ServerRequestVehicleDelivery(<delivery item class>, 0)
std::wstring bountyDeliver(int idx){
    if(idx<0||idx>=bountyCount)return L"Bad selection";
    std::string why;if(!initVehicle(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;
    if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    std::unordered_set<std::string> wanted={"GetPlayerController","ServerRequestVehicleDelivery"};
    int ok=0,tried=0;auto hits=scan(wanted,ok,tried);
    uintptr_t gpc=0;Fn fGetPC;
    if(!findHit(hits,"GetPlayerController","Function","GameplayStatics",gpc)||!readFn(fGetPC,gpc,{"WorldContextObject","PlayerIndex","ReturnValue"}))
        return L"GetPlayerController not found";
    uintptr_t pc=0;
    {Buf b(fGetPC.size);b.put(fGetPC,"WorldContextObject",world);b.put(fGetPC,"PlayerIndex",(int)0);
     if(!callPE(processEvent,cdoStatics,fGetPC.ptr,b.p))return L"Engine call faulted (GetPlayerController)";
     b.get(fGetPC,"ReturnValue",pc);}
    if(!pc)return L"No player controller yet";
    uintptr_t pcClass=0;if(!rd(pc+O_CLASS,pcClass))return L"Could not read controller class";
    const int off=propOffset(pcClass,"BountyVehicleDeliveryProtocol");
    if(off<0)return L"Delivery system not found on this build";
    uintptr_t proto=0;rd(pc+(uintptr_t)off,proto);
    if(!proto)return L"Delivery system not ready (load into a save)";
    uintptr_t protoClass=0;if(!rd(proto+O_CLASS,protoClass))return L"Could not read delivery system class";
    const uintptr_t item=blueprintClass(bountyDefs[idx].pkg,bountyDefs[idx].cls,nullptr);
    if(!item){
        const std::wstring r=requestLoad(bountyDefs[idx].pkg,bountyDefs[idx].cls);
        if(r.empty())return std::wstring(L"Loading ")+bountyDefs[idx].label+L" delivery item class into memory - press Request again in a few seconds.";
        return L"Delivery item class not loaded in memory yet; loader: "+r;
    }
    auto it=hits.find("ServerRequestVehicleDelivery");
    if(it==hits.end())return L"ServerRequestVehicleDelivery not found";
    Fn use;bool have=false;
    for(const Hit& h:it->second){
        uintptr_t outer=0;
        if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(protoClass,outer)&&readFn(use,h.obj,{})){have=true;break;}
    }
    if(!have)return L"Delivery function does not match the delivery system";
    const auto ps=sortedParams(use);
    if(ps.size()!=2){logf("Bounty delivery: ServerRequestVehicleDelivery has %llu parameters, expected 2",(unsigned long long)ps.size());return L"Unexpected function layout (see log)";}
    {Buf b(use.size);b.put(use,ps[0].first.c_str(),item);b.put(use,ps[1].first.c_str(),(int)0);
     if(!callPE(processEvent,proto,use.ptr,b.p))return L"Engine call faulted (request delivery)";}
    logMessage((std::string("Bounty delivery requested: ")+bountyDefs[idx].cls).c_str());
    return std::wstring(L"Delivery requested: ")+bountyDefs[idx].label;
}
// ---- World tools (v34) -------------------------------------------------------------------------------------
// Time Scale = GameplayStatics.SetGlobalTimeDilation; graphics toggles = KismetSystemLibrary.ExecuteConsoleCommand (as in Hades);
// Gravity Scale = the player pawn's CharacterMovement.GravityScale. All run on the game thread.
Fn fTimeDil,fExecCmd;uintptr_t cdoKSL=0;bool worldScanned=false;
void initWorldFns(){
    if(worldScanned)return;worldScanned=true;
    std::unordered_set<std::string> w={"Default__KismetSystemLibrary","ExecuteConsoleCommand","SetGlobalTimeDilation"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t fe=0,ft=0;
    if(findHit(hits,"SetGlobalTimeDilation","Function","GameplayStatics",ft)&&!readFn(fTimeDil,ft,{"WorldContextObject","TimeDilation"}))fTimeDil=Fn();
    if(findHit(hits,"Default__KismetSystemLibrary","KismetSystemLibrary",nullptr,cdoKSL)&&findHit(hits,"ExecuteConsoleCommand","Function","KismetSystemLibrary",fe)&&!readFn(fExecCmd,fe,{"WorldContextObject","Command"}))fExecCmd=Fn();
    char m[200];snprintf(m,sizeof(m),"World tools: SetGlobalTimeDilation %s, ExecuteConsoleCommand %s",fTimeDil.ptr?"found":"MISSING",(fExecCmd.ptr&&cdoKSL)?"found":"MISSING");logMessage(m);
}
std::wstring setTimeScale(float v){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    initWorldFns();
    if(!fTimeDil.ptr)return L"SetGlobalTimeDilation not found";
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    if(v<0.1f)v=0.1f;if(v>4.f)v=4.f;
    Buf b(fTimeDil.size);b.put(fTimeDil,"WorldContextObject",world);b.put(fTimeDil,"TimeDilation",v);
    if(!callPE(processEvent,cdoStatics,fTimeDil.ptr,b.p))return L"Engine call faulted (time scale)";
    char m[120];snprintf(m,sizeof(m),"Time scale set to %.2f",v);logMessage(m);
    return L"Time scale applied";
}
std::wstring runConsoleCommand(const wchar_t* cmd){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    initWorldFns();
    if(!fExecCmd.ptr||!cdoKSL)return L"ExecuteConsoleCommand not found";
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    static wchar_t cbuf[160];size_t n=wcslen(cmd);if(n>150)return L"Command too long";
    memcpy(cbuf,cmd,(n+1)*sizeof(wchar_t));
    FStr fs{cbuf,(int)n+1,160};
    Buf b(fExecCmd.size);b.put(fExecCmd,"WorldContextObject",world);b.putBytes(fExecCmd,"Command",&fs,sizeof(fs));
    if(!callPE(processEvent,cdoKSL,fExecCmd.ptr,b.p))return L"Engine call faulted (console command)";
    std::string nm;for(size_t i=0;i<n;++i)nm.push_back(cmd[i]<128?(char)cmd[i]:'?');
    logMessage(("Console command sent: "+nm).c_str());
    return L"Applied";
}
std::wstring setGraphicsToggle(int idx,bool on){
    static const wchar_t* onCmd[5]={L"r.MotionBlurQuality 0",L"r.Tonemapper.GrainQuantization 0",L"r.DefaultFeature.AntiAliasing 0",L"r.Tonemapper.Quality 0",L"r.SetNearClipPlane 300"};
    static const wchar_t* offCmd[5]={L"r.MotionBlurQuality 4",L"r.Tonemapper.GrainQuantization 1",L"r.DefaultFeature.AntiAliasing 2",L"r.Tonemapper.Quality 5",L"r.SetNearClipPlane 10"};   // engine defaults (10 = Unreal's default near plane; 0 would be a degenerate camera)
    if(idx<0||idx>4)return L"Bad toggle";
    return runConsoleCommand(on?onCmd[idx]:offCmd[idx]);
}
std::wstring setGravityScale(float g){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    uintptr_t pawn=0;
    {Buf b(fPawn.size);b.put(fPawn,"WorldContextObject",world);b.put(fPawn,"PlayerIndex",(int)0);
     if(!callPE(processEvent,cdoStatics,fPawn.ptr,b.p))return L"Engine call faulted (GetPlayerPawn)";
     b.get(fPawn,"ReturnValue",pawn);}
    if(!pawn)return L"No player pawn (are you in game?)";
    uintptr_t pcls=0;if(!rd(pawn+O_CLASS,pcls)||!pcls)return L"Cannot read pawn class";
    const int moff=propOffset(pcls,"CharacterMovement");
    uintptr_t mov=0;if(moff<0||!rd(pawn+(uintptr_t)moff,mov)||!mov)return L"Pawn has no CharacterMovement";
    uintptr_t mcls=0;if(!rd(mov+O_CLASS,mcls)||!mcls)return L"Cannot read movement class";
    const int goff=propOffset(mcls,"GravityScale");
    if(goff<0)return L"GravityScale property not found";
    if(!writeFloat(mov+(uintptr_t)goff,g))return L"Write failed";
    float back=0;rd(mov+(uintptr_t)goff,back);
    char m[160];snprintf(m,sizeof(m),"Gravity scale set to %.2f (read back %.2f)",g,back);logMessage(m);
    return back==g?L"Gravity scale applied":L"Gravity write did not stick";
}
// ---- Night Vision / Ghost Mode / Enclave Lighting (v35) ----------------------------------------------------------
// Night Vision and Ghost Mode are Hades' timed buff sets: CharacterComponent.AddTimedBuffSet(buff names, set id, duration, 0, text, text, false).
// FNames and FTexts are produced by the engine's own Conv_StringToName / Conv_NameToText through ProcessEvent.
struct FNameRaw{unsigned char b[8];};
bool callStatic(uintptr_t cdo,const Fn& f,Buf& b){return callPE(processEvent,cdo,f.ptr,b.p);}
void logBuffFunctionsGlobal();
std::wstring logBuffState(int which,const char* tag);
// Resolved once and reused, so toggling no longer walks the whole object table each time (that was the hitch).
struct BuffCache{bool valid=false;uintptr_t pcls=0,dcls=0,ccls=0,cdoStr=0,cdoTxt=0;Fn fDC,fComp,fAdd,fS2N,fN2T;};
static bool resolveBuffFns(BuffCache& bc,uintptr_t pawn,uintptr_t pcls,std::wstring& err){
    std::unordered_set<std::string> w={"GetDaytonCharacter","GetCharacterComponent","AddTimedBuffSet","Default__KismetStringLibrary","Conv_StringToName","Default__KismetTextLibrary","Conv_NameToText"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    auto findFn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    bc=BuffCache();bc.pcls=pcls;
    if(!findFn("GetDaytonCharacter",pcls,bc.fDC)){err=L"GetDaytonCharacter not found";return false;}
    uintptr_t dc=0;{Buf b(bc.fDC.size);if(!callPE(processEvent,pawn,bc.fDC.ptr,b.p)){err=L"Engine call faulted (GetDaytonCharacter)";return false;}b.get(bc.fDC,"ReturnValue",dc);}
    if(!dc){err=L"Survivor object is null";return false;}
    rd(dc+O_CLASS,bc.dcls);
    if(!findFn("GetCharacterComponent",bc.dcls,bc.fComp)){err=L"GetCharacterComponent not found";return false;}
    uintptr_t comp=0;{Buf b(bc.fComp.size);if(!callPE(processEvent,dc,bc.fComp.ptr,b.p)){err=L"Engine call faulted (GetCharacterComponent)";return false;}b.get(bc.fComp,"ReturnValue",comp);}
    if(!comp){err=L"Character component is null";return false;}
    rd(comp+O_CLASS,bc.ccls);
    if(!findFn("AddTimedBuffSet",bc.ccls,bc.fAdd)){err=L"AddTimedBuffSet not found";return false;}
    auto cdoOf=[&](const char* n,uintptr_t& out){auto it=hits.find(n);if(it==hits.end())return false;for(const Hit& h:it->second)if(h.cls!="Function"){out=h.obj;return true;}return false;};
    if(!cdoOf("Default__KismetStringLibrary",bc.cdoStr)||!cdoOf("Default__KismetTextLibrary",bc.cdoTxt)){err=L"String/Text library not found";return false;}
    {auto it=hits.find("Conv_StringToName");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="Function"&&readFn(bc.fS2N,h.obj,{"ReturnValue"}))break;
     auto it2=hits.find("Conv_NameToText");if(it2!=hits.end())for(const Hit& h:it2->second)if(h.cls=="Function"&&readFn(bc.fN2T,h.obj,{"ReturnValue"}))break;}
    if(!bc.fS2N.ptr||!bc.fN2T.ptr){err=L"Name/Text conversion functions not found";return false;}
    bc.valid=true;logMessage("Buff functions resolved and cached");
    return true;
}
std::wstring setNightVisionGhost(int which,bool on,bool& okRun){
    okRun=false;
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn (are you in game?)";
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    static BuffCache bc;
    std::wstring err;
    if(!bc.valid||bc.pcls!=pcls){if(!resolveBuffFns(bc,pawn,pcls,err)){bc.valid=false;return err;}}
    uintptr_t dc=0;{Buf b(bc.fDC.size);if(!callPE(processEvent,pawn,bc.fDC.ptr,b.p))return L"Engine call faulted (GetDaytonCharacter)";b.get(bc.fDC,"ReturnValue",dc);}
    if(!dc)return L"Survivor object is null";
    uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
    if(dcls!=bc.dcls){bc.valid=false;if(!resolveBuffFns(bc,pawn,pcls,err))return err;rd(dc+O_CLASS,dcls);}
    uintptr_t comp=0;{Buf b(bc.fComp.size);if(!callPE(processEvent,dc,bc.fComp.ptr,b.p))return L"Engine call faulted (GetCharacterComponent)";b.get(bc.fComp,"ReturnValue",comp);}
    if(!comp)return L"Character component is null";
    uintptr_t ccls=0;rd(comp+O_CLASS,ccls);
    if(ccls!=bc.ccls){bc.valid=false;return L"Character component changed; try again";}
    const Fn& fAdd=bc.fAdd;const Fn& fS2N=bc.fS2N;const Fn& fN2T=bc.fN2T;
    static const char* setIds[2]={"Night Vision","GhostMode"};
    auto toName=[&](const char* s,FNameRaw& out)->bool{
        static wchar_t wb[96];size_t n=strlen(s);if(n>90)return false;for(size_t i=0;i<=n;++i)wb[i]=(wchar_t)(unsigned char)s[i];
        FStr fs{wb,(int)n+1,96};const auto ps=sortedParams(fS2N);if(ps.size()!=1)return false;
        Buf b(fS2N.size);b.putBytes(fS2N,ps[0].first.c_str(),&fs,sizeof(fs));
        if(!callStatic(bc.cdoStr,fS2N,b))return false;
        return b.get(fS2N,"ReturnValue",out);
    };
    FNameRaw setId{};if(!toName(setIds[which],setId))return L"Could not make FName";
    const auto ps=sortedParams(fAdd);
    if(ps.size()!=7)return L"AddTimedBuffSet layout unexpected (see log)";
    const int tsz=ps[5].second-ps[4].second;
    if(ps[1].second-ps[0].second!=16||ps[2].second-ps[1].second!=8||(tsz!=16&&tsz!=24))return L"AddTimedBuffSet layout unexpected (see log)";
    unsigned char txt[32]{};
    {const auto pn=sortedParams(fN2T);if(pn.size()!=1)return L"Conv_NameToText layout unexpected";
     Buf b(fN2T.size);b.putBytes(fN2T,pn[0].first.c_str(),&setId,sizeof(setId));
     if(!callStatic(bc.cdoTxt,fN2T,b))return L"Engine call faulted (name to text)";
     auto rv=fN2T.off.find("ReturnValue");if(rv==fN2T.off.end()||(size_t)rv->second+tsz>b.n)return L"Text result unreadable";
     memcpy(txt,(char*)b.p+rv->second,(size_t)tsz);}
    static const char* nv[]={"dlc4_nightvision_high","skill_wits_aggressive_detection_range","skill_wits_scouting_scoutrange","skill_wits_scouting_containervisibility","skill_wits_scouting_range_perk2","skill_wits_scouting_perk2"};
    static const char* gh[]={"consumable_zombievisibility_reduction_major","consumable_locomotion_noise_reduction_major","consumable_locomotion_noise_reduction_major"};
    const char** ids=which==0?nv:gh;const int cnt=which==0?6:3;
    static FNameRaw arr[8];
    for(int i=0;i<cnt;++i)if(!toName(ids[i],arr[i]))return L"Could not make FName";
    struct TArr{void* data;int num,max;}ta{arr,cnt,8};
    // The game has no buff-remove function. ON adds the set for ~2 billion seconds (stacking 0); OFF re-adds the same set with a
    // 0.5 s duration under stacking mode 1, which replaces the running one (verified in the log: "in effect = 0" afterwards).
    const float dur=on?2000000000.0f:0.5f;const int stacking=on?0:1;
    Buf b(fAdd.size);
    b.putBytes(fAdd,ps[0].first.c_str(),&ta,sizeof(ta));
    b.putBytes(fAdd,ps[1].first.c_str(),&setId,sizeof(setId));
    b.put(fAdd,ps[2].first.c_str(),dur);
    b.put(fAdd,ps[3].first.c_str(),(int)stacking);
    b.putBytes(fAdd,ps[4].first.c_str(),txt,(size_t)tsz);
    b.putBytes(fAdd,ps[5].first.c_str(),txt,(size_t)tsz);
    if(!callPE(processEvent,comp,fAdd.ptr,b.p))return L"Engine call faulted (AddTimedBuffSet)";
    unsigned char rv=0;int got=b.get(fAdd,"ReturnValue",rv)?(int)rv:-2;
    char m[160];snprintf(m,sizeof(m),"%s %s (duration %.1f, stacking %d) returned %d",which==0?"Night Vision":"Ghost Mode",on?"ON":"OFF",dur,stacking,got);logMessage(m);
    okRun=true;
    if(on)return which==0?L"Night Vision applied":L"Ghost Mode applied";
    return L"Turning off (takes a second or two)";
}
// Logs every function whose name mentions TimedBuff / BuffSet anywhere in the game, with its owning class (finds a remover on another object).
void logBuffFunctionsGlobal(){
    static bool done=false;if(done)return;done=true;
    refreshCount();int shown=0;
    for(int i=0;i<numObjs&&shown<80;++i){
        uintptr_t o=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
        std::string nm;if(!objName(o,nm))continue;
        if(!(icontains(nm,"timedbuff")||icontains(nm,"buffset")||icontains(nm,"removebuff")||icontains(nm,"clearbuff")))continue;
        uintptr_t kc=0;std::string k;if(!rd(o+O_CLASS,kc)||!objName(kc,k)||k!="Function")continue;
        uintptr_t oc=0;std::string on;rd(o+O_OUTER,oc);objName(oc,on);
        logMessage(("Global buff function: "+on+"::"+nm).c_str());++shown;
    }
}
// Is the set still in effect? (IsTimedBuffWithProviderIdInEffect on the character component.)
std::wstring logBuffState(int which,const char* tag){
    std::string why;if(!init(why))return L"Unavailable";
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No world";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No pawn";
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    std::unordered_set<std::string> w={"GetDaytonCharacter","GetCharacterComponent","IsTimedBuffWithProviderIdInEffect","Default__KismetStringLibrary","Conv_StringToName"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    auto findFn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    Fn fDC,fComp,fIn,fS2N;uintptr_t cdoStr=0;
    if(!findFn("GetDaytonCharacter",pcls,fDC))return L"GetDaytonCharacter not found";
    uintptr_t dc=0;{Buf b(fDC.size);if(!callPE(processEvent,pawn,fDC.ptr,b.p))return L"faulted";b.get(fDC,"ReturnValue",dc);}
    if(!dc)return L"no survivor";
    uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
    if(!findFn("GetCharacterComponent",dcls,fComp))return L"GetCharacterComponent not found";
    uintptr_t comp=0;{Buf b(fComp.size);if(!callPE(processEvent,dc,fComp.ptr,b.p))return L"faulted";b.get(fComp,"ReturnValue",comp);}
    if(!comp)return L"no component";
    uintptr_t ccls=0;rd(comp+O_CLASS,ccls);
    if(!findFn("IsTimedBuffWithProviderIdInEffect",ccls,fIn))return L"state check not found";
    {auto it=hits.find("Default__KismetStringLibrary");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls!="Function"){cdoStr=h.obj;break;}
     auto it2=hits.find("Conv_StringToName");if(it2!=hits.end())for(const Hit& h:it2->second)if(h.cls=="Function"&&readFn(fS2N,h.obj,{"ReturnValue"}))break;}
    if(!cdoStr||!fS2N.ptr)return L"name conversion not found";
    static const char* setIds[2]={"Night Vision","GhostMode"};
    static wchar_t wb[64];const char* sid=setIds[which==1?1:0];size_t n=strlen(sid);for(size_t i=0;i<=n;++i)wb[i]=(wchar_t)(unsigned char)sid[i];
    FStr fs{wb,(int)n+1,64};const auto ps1=sortedParams(fS2N);if(ps1.size()!=1)return L"layout";
    FNameRaw nmv{};{Buf b(fS2N.size);b.putBytes(fS2N,ps1[0].first.c_str(),&fs,sizeof(fs));if(!callPE(processEvent,cdoStr,fS2N.ptr,b.p))return L"faulted";b.get(fS2N,"ReturnValue",nmv);}
    const auto ps2=sortedParams(fIn);if(ps2.size()!=1)return L"state check layout";
    Buf b(fIn.size);b.putBytes(fIn,ps2[0].first.c_str(),&nmv,sizeof(nmv));
    if(!callPE(processEvent,comp,fIn.ptr,b.p))return L"faulted";
    unsigned char r=0;b.get(fIn,"ReturnValue",r);
    char m[200];snprintf(m,sizeof(m),"Buff state [%s] %s: in effect = %d",tag,sid,(int)r);logMessage(m);
    return r?L"in effect":L"not in effect";
}
// Hades: GameMode.ScoutingManagerNative.SiteManager.SetCheatEnclaveLighting(bool)
std::wstring setEnclaveLighting(bool on,bool& okRun){
    okRun=false;
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    std::unordered_set<std::string> w={"GetGameMode","SetCheatEnclaveLighting"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    Fn fGM,fLight;uintptr_t gmFn=0;
    if(!findHit(hits,"GetGameMode","Function","GameplayStatics",gmFn)||!readFn(fGM,gmFn,{"WorldContextObject","ReturnValue"}))return L"GetGameMode not found";
    uintptr_t gm=0;{Buf b(fGM.size);b.put(fGM,"WorldContextObject",world);if(!callPE(processEvent,cdoStatics,fGM.ptr,b.p))return L"Engine call faulted (GetGameMode)";b.get(fGM,"ReturnValue",gm);}
    if(!gm)return L"No game mode";
    uintptr_t gcls=0;rd(gm+O_CLASS,gcls);
    const int o1=propOffset(gcls,"ScoutingManagerNative");uintptr_t sm=0;
    if(o1<0||!rd(gm+(uintptr_t)o1,sm)||!sm)return L"ScoutingManagerNative not found";
    uintptr_t scls=0;rd(sm+O_CLASS,scls);
    const int o2=propOffset(scls,"SiteManager");uintptr_t site=0;
    if(o2<0||!rd(sm+(uintptr_t)o2,site)||!site)return L"SiteManager not found";
    uintptr_t sitecls=0;rd(site+O_CLASS,sitecls);
    auto it=hits.find("SetCheatEnclaveLighting");
    if(it!=hits.end())for(const Hit& h:it->second){uintptr_t outer=0;
        if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(sitecls,outer)&&readFn(fLight,h.obj,{}))break;}
    if(!fLight.ptr)return L"SetCheatEnclaveLighting not found";
    const auto ps=sortedParams(fLight);if(ps.size()!=1)return L"Lighting function layout unexpected";
    {char m[200];snprintf(m,sizeof(m),"Enclave lighting: param %s@%d, function size %d",ps[0].first.c_str(),ps[0].second,fLight.size);logMessage(m);}
    Buf b(fLight.size);b.put(fLight,ps[0].first.c_str(),(unsigned char)(on?1:0));
    if(!callPE(processEvent,site,fLight.ptr,b.p))return L"Engine call faulted (enclave lighting)";
    okRun=true;logMessage(on?"Enclave lighting ON":"Enclave lighting OFF");
    return on?L"Enclave lighting ON":L"Enclave lighting OFF";
}

// Character actions (Player > Character / Clothing). Call only on the game's main thread.
// 0 Cycle Body, 1 Cycle Hat, 2 Cure Sickness, 3 Cure Trauma, 4 Unlearn Specializations.
std::wstring characterAction(int action,bool& okRun,int count=0){
    okRun=false;
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn (are you in game?)";
    static const char* fnNames[6]={"CheatCycleBody","CheatCycleHat","CheatRemoveSickness","CheatRemoveTrauma","UnlearnSpecializationOfBaseSkill","TryRemoveTraitWithTag"};
    // 0/5/8 body, 1/6/7 hat; 2 sickness, 3 trauma, 4 unlearn
    const int fIdx=(action==0||action==5||action==8)?0:((action==1||action==6||action==7)?1:((action>=2&&action<=4)?action:(action==9?5:0)));
    std::unordered_set<std::string> w={"GetDaytonCharacter","GetCharacterComponent",fnNames[fIdx]};
    if(action==9)w.insert("Injury_C");
    if(action==2)for(const char* a:{"RemoveSickness","CureSickness","CheatCureSickness","CheatRemoveIllness","CheatCureAllSickness"})w.insert(a);
    int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    auto findFnOn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    Fn fDC,fComp;
    if(!findFnOn("GetDaytonCharacter",pcls,fDC))return L"GetDaytonCharacter not found";
    uintptr_t dc=0;{Buf b(fDC.size);if(!callPE(processEvent,pawn,fDC.ptr,b.p))return L"Engine call faulted (GetDaytonCharacter)";b.get(fDC,"ReturnValue",dc);}
    if(!dc)return L"Survivor object is null";
    uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
    uintptr_t comp=0;
    if(findFnOn("GetCharacterComponent",dcls,fComp)){Buf b(fComp.size);if(callPE(processEvent,dc,fComp.ptr,b.p))b.get(fComp,"ReturnValue",comp);}
    // The cheat functions live on the character, its component or the pawn; call each on whichever object owns it.
    uintptr_t targets[3]={dc,comp,pawn};
    const char* fname=fnNames[fIdx];
    Fn fn;uintptr_t target=0;
    for(int i=0;i<3&&!target;++i){
        if(!targets[i])continue;uintptr_t tc=0;rd(targets[i]+O_CLASS,tc);
        if(findFnOn(fname,tc,fn))target=targets[i];
    }
    if(!target&&action==2){
        // CheatRemoveSickness is not on the survivor, component or pawn: try known alternative names, then log what exists.
        for(const char* alt:{"RemoveSickness","CureSickness","CheatCureSickness","CheatRemoveIllness","CheatCureAllSickness"}){
            for(int i=0;i<3&&!target;++i){
                if(!targets[i])continue;uintptr_t tc=0;rd(targets[i]+O_CLASS,tc);
                if(findFnOn(alt,tc,fn)){target=targets[i];fname=alt;}
            }
            if(target)break;
        }
        {auto it=hits.find("CheatRemoveSickness");char m[200];
         snprintf(m,sizeof(m),"Sickness: CheatRemoveSickness hits=%d",it==hits.end()?0:(int)it->second.size());logMessage(m);
         if(it!=hits.end())for(const Hit& h:it->second){snprintf(m,sizeof(m),"Sickness: hit cls=%s outer=%s",h.cls.c_str(),h.outer.c_str());logMessage(m);}}
        {char m[200];std::string n1,n2,n3;uintptr_t c1=0;
         rd(dc+O_CLASS,c1);objName(c1,n1);snprintf(m,sizeof(m),"Sickness: survivor class %s",n1.c_str());logMessage(m);
         if(comp){rd(comp+O_CLASS,c1);objName(c1,n2);snprintf(m,sizeof(m),"Sickness: component class %s",n2.c_str());logMessage(m);}}
        if(!target){
            refreshCount();int logged=0;
            for(int i=0;i<numObjs&&logged<80;++i){
                uintptr_t o=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
                std::string nm;if(!objName(o,nm))continue;
                if(!(icontains(nm,"sick")||icontains(nm,"plague")||icontains(nm,"cure")||icontains(nm,"illness")))continue;
                uintptr_t cls=0,outer=0;std::string cn,on;
                if(!rd(o+O_CLASS,cls)||!objName(cls,cn)||cn!="Function")continue;
                if(rd(o+O_OUTER,outer))objName(outer,on);
                char m[260];snprintf(m,sizeof(m),"Sickness: function %s.%s",on.c_str(),nm.c_str());logMessage(m);++logged;
            }
        }
    }
    if(!target){char m[160];snprintf(m,sizeof(m),"Character action: %s not found",fname);logMessage(m);return L"Function not found in this game build (see log)";}
    {char m[200];snprintf(m,sizeof(m),"Character action %d: %s on %p, size %d",action,fname,(void*)target,fn.size);logMessage(m);}
    if(fIdx<=1){
        const bool isHat=fIdx==1;
        const auto ps=sortedParams(fn);
        const char* pn=ps.size()==1?ps[0].first.c_str():nullptr;
        {char m[160];snprintf(m,sizeof(m),"Cycle function %s: %d parameter(s), first=%s",fname,(int)ps.size(),pn?pn:"-");logMessage(m);}
        bool rev=(action==5||action==6);
        int times=1;
        if(action==7||action==8){rev=count>0;times=count>0?count:-count;if(times>200)times=200;}
        if(rev&&!pn)return L"Reverse cycling not available (see log)";
        for(int i=0;i<times;++i){
            Buf b(fn.size);if(rev)b.put(fn,pn,(unsigned char)1);
            if(!callPE(processEvent,target,fn.ptr,b.p))return L"Engine call faulted";
        }
        okRun=true;
        if(action==7||action==8)return isHat?L"Hat reset":L"Outfit reset";
        return isHat?(rev?L"Previous hat":L"Next hat"):(rev?L"Previous outfit":L"Next outfit");
    }
    if(action==2){
        Buf b(fn.size);
        if(!callPE(processEvent,target,fn.ptr,b.p))return L"Engine call faulted";
        okRun=true;return L"Sickness cured";
    }
    if(action==3){
        const auto ps=sortedParams(fn);if(ps.size()!=1)return L"Trauma function layout unexpected (see log)";
        Buf b(fn.size);b.put(fn,ps[0].first.c_str(),2000000000.0f);
        if(!callPE(processEvent,target,fn.ptr,b.p))return L"Engine call faulted";
        okRun=true;return L"Trauma cured";
    }
    if(action==9){
        // Remove every injury trait: TryRemoveTraitWithTag(Injury tag class) until it reports nothing left to remove.
        uintptr_t injCls=0;int nHits=0;
        {auto it=hits.find("Injury_C");
         if(it!=hits.end())for(const Hit& h:it->second){++nHits;
             if(h.cls.find("Class")!=std::string::npos&&h.outer.size()>=6&&h.outer.compare(h.outer.size()-6,6,"Injury")==0){injCls=h.obj;break;}}}
        {char m[160];snprintf(m,sizeof(m),"Injury tag class: %d name hit(s), class found=%d",nHits,injCls?1:0);logMessage(m);}
        if(!injCls)return L"Injury tag class is not loaded (see log)";
        const auto ps=sortedParams(fn);if(ps.size()!=1)return L"Trait function layout unexpected (see log)";
        int removed=0;
        for(int i=0;i<40;++i){
            Buf b(fn.size);b.put(fn,ps[0].first.c_str(),injCls);
            if(!callPE(processEvent,target,fn.ptr,b.p))return L"Engine call faulted";
            unsigned char r=0;if(!b.get(fn,"ReturnValue",r)||!r)break;
            ++removed;
        }
        okRun=true;
        if(!removed)return L"No injuries to remove";
        wchar_t r[64];swprintf(r,64,L"Removed %d injur%ls",removed,removed==1?L"y":L"ies");return r;
    }
    // action 4: unlearn specializations for every skill the survivor has
    const auto ps=sortedParams(fn);if(ps.size()!=1)return L"Unlearn function layout unexpected (see log)";
    SkillCtx c;std::wstring err;
    if(!skillContext(c,err))return err;
    int done=0,okCount=0;
    for(uintptr_t sk:c.objs){
        Buf b(fn.size);b.put(fn,ps[0].first.c_str(),sk);
        if(callPE(processEvent,target,fn.ptr,b.p)){++done;unsigned char r=0;if(b.get(fn,"ReturnValue",r)&&r)++okCount;}
    }
    {char m[160];snprintf(m,sizeof(m),"Unlearn specializations: %d calls, %d returned true",done,okCount);logMessage(m);}
    if(!done)return L"Engine call faulted";
    okRun=true;
    wchar_t r[96];swprintf(r,96,L"Specializations unlearned (%d skills)",okCount);return r;
}

// ---- Appearance (human definition) list and apply. Game thread only.
bool fnameToString(unsigned long long raw,std::string& out){
    unsigned char fname[8];memcpy(fname,&raw,8);
    static wchar_t buf[1024];buf[0]=0;FStr s{buf,0,1024};
    if(!callAppend(appendName,fname,&s))return false;
    if(s.data!=buf||s.num<=0||s.num>1024)return false;
    out.clear();for(int i=0;i<s.num&&buf[i];++i)out.push_back(buf[i]<128?(char)buf[i]:'?');
    return !out.empty();
}
std::wstring loadAppearanceNames(std::vector<std::string>& names,std::vector<unsigned long long>& raws){
    names.clear();raws.clear();
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    std::unordered_set<std::string> w={"GetGameInstance","GetHumanDefinitionNames"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    Fn fGI,fNames;uintptr_t giFn=0;
    if(!findHit(hits,"GetGameInstance","Function","GameplayStatics",giFn)||!readFn(fGI,giFn,{"WorldContextObject","ReturnValue"}))return L"GetGameInstance not found";
    uintptr_t gi=0;{Buf b(fGI.size);b.put(fGI,"WorldContextObject",world);if(!callPE(processEvent,cdoStatics,fGI.ptr,b.p))return L"Engine call faulted (GetGameInstance)";b.get(fGI,"ReturnValue",gi);}
    if(!gi)return L"No game instance";
    uintptr_t gcls=0;rd(gi+O_CLASS,gcls);
    const int off=propOffset(gcls,"CharacterDefinitions");uintptr_t cd=0;
    if(off<0||!rd(gi+(uintptr_t)off,cd)||!cd)return L"CharacterDefinitions not found";
    uintptr_t cdcls=0;rd(cd+O_CLASS,cdcls);
    {auto it=hits.find("GetHumanDefinitionNames");
     if(it!=hits.end())for(const Hit& h:it->second){uintptr_t outer=0;
         if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(cdcls,outer)&&readFn(fNames,h.obj,{"ReturnValue"}))break;}}
    if(!fNames.ptr)return L"GetHumanDefinitionNames not found";
    struct TArr{uintptr_t data;int num,max;}arr{};
    {Buf b(fNames.size);if(!callPE(processEvent,cd,fNames.ptr,b.p))return L"Engine call faulted (GetHumanDefinitionNames)";
     auto rv=fNames.off.find("ReturnValue");if(rv==fNames.off.end()||(size_t)rv->second+sizeof(arr)>b.n)return L"Name list layout unexpected";
     memcpy(&arr,(char*)b.p+rv->second,sizeof(arr));}
    if(!arr.data||arr.num<=0||arr.num>2000)return L"No appearances found";
    for(int i=0;i<arr.num;++i){
        unsigned long long raw=0;if(!safeRead(arr.data+(uintptr_t)i*8,&raw,8))continue;
        std::string nm;if(!fnameToString(raw,nm))continue;
        names.push_back(nm);raws.push_back(raw);
    }
    if(names.empty())return L"Appearance names unreadable";
    {char m[120];snprintf(m,sizeof(m),"Appearance list: %d entries (first %s)",(int)names.size(),names[0].c_str());logMessage(m);}
    // One-time diagnostic for the voice feature: what voice-related functions and objects exist.
    static bool voiceDiag=false;
    if(!voiceDiag){
        voiceDiag=true;refreshCount();int logged=0;
        for(int i=0;i<numObjs&&logged<120;++i){
            uintptr_t o=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
            std::string nm;if(!objName(o,nm)||!icontains(nm,"voice"))continue;
            uintptr_t cls=0,outer=0;std::string cn,on;
            if(rd(o+O_CLASS,cls))objName(cls,cn);
            if(cn=="Function"||cn=="BlueprintGeneratedClass"||cn=="DataTable"||cn=="DataAsset"||cn.find("Voice")!=std::string::npos||cn.find("Property")!=std::string::npos){
                if(rd(o+O_OUTER,outer))objName(outer,on);
                char m[300];snprintf(m,sizeof(m),"Voice: %s %s.%s",cn.c_str(),on.c_str(),nm.c_str());logMessage(m);++logged;
            }
        }
    }
    wchar_t r[64];swprintf(r,64,L"Loaded %d appearances",(int)names.size());return r;
}
std::wstring applyAppearance(unsigned long long raw){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn (are you in game?)";
    std::unordered_set<std::string> w={"GetDaytonCharacter","GetCharacterComponent","CheatSetHumanDefinition"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    auto findFnOn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    Fn fDC,fComp,fSet;
    if(!findFnOn("GetDaytonCharacter",pcls,fDC))return L"GetDaytonCharacter not found";
    uintptr_t dc=0;{Buf b(fDC.size);if(!callPE(processEvent,pawn,fDC.ptr,b.p))return L"Engine call faulted (GetDaytonCharacter)";b.get(fDC,"ReturnValue",dc);}
    if(!dc)return L"Survivor object is null";
    uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
    uintptr_t comp=0;
    if(findFnOn("GetCharacterComponent",dcls,fComp)){Buf b(fComp.size);if(callPE(processEvent,dc,fComp.ptr,b.p))b.get(fComp,"ReturnValue",comp);}
    uintptr_t targets[3]={comp,dc,pawn},target=0;
    for(int i=0;i<3&&!target;++i){if(!targets[i])continue;uintptr_t tc=0;rd(targets[i]+O_CLASS,tc);if(findFnOn("CheatSetHumanDefinition",tc,fSet))target=targets[i];}
    if(!target)return L"CheatSetHumanDefinition not found (see log)";
    const auto ps=sortedParams(fSet);if(ps.size()!=1)return L"Appearance function layout unexpected (see log)";
    Buf b(fSet.size);b.putBytes(fSet,ps[0].first.c_str(),&raw,8);
    if(!callPE(processEvent,target,fSet.ptr,b.p))return L"Engine call faulted";
    std::string nm;fnameToString(raw,nm);{char m[200];snprintf(m,sizeof(m),"Appearance applied: %s",nm.c_str());logMessage(m);}
    return L"Appearance applied";
}

// ---- Voices: names come from the game's Voices data table; apply = CheatSetVoice on the survivor + ApplyCharacterVoice on the pawn.
std::wstring loadVoiceNames(std::vector<std::string>& names,std::vector<unsigned long long>& raws){
    names.clear();raws.clear();
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    std::unordered_set<std::string> w={"Voices","GetDataTableRowNames","Default__DataTableFunctionLibrary"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t table=0;
    {auto it=hits.find("Voices");int n=0;
     if(it!=hits.end())for(const Hit& h:it->second){++n;if(h.cls=="DataTable"){table=h.obj;break;}}
     char m[120];snprintf(m,sizeof(m),"Voices table: %d name hit(s), DataTable found=%d",n,table?1:0);logMessage(m);}
    if(!table)return L"Voices table not loaded (see log)";
    uintptr_t cdo=0,fnObj=0;Fn fRows;
    if(!findHit(hits,"Default__DataTableFunctionLibrary","DataTableFunctionLibrary",nullptr,cdo))return L"DataTable library not found";
    if(!findHit(hits,"GetDataTableRowNames","Function","DataTableFunctionLibrary",fnObj)||!readFn(fRows,fnObj,{}))return L"GetDataTableRowNames not found";
    const auto ps=sortedParams(fRows);
    if(ps.size()!=2){char m[120];snprintf(m,sizeof(m),"GetDataTableRowNames has %d params",(int)ps.size());logMessage(m);return L"Row-name function layout unexpected (see log)";}
    Buf b(fRows.size);b.put(fRows,ps[0].first.c_str(),table);
    if(!callPE(processEvent,cdo,fRows.ptr,b.p))return L"Engine call faulted (GetDataTableRowNames)";
    struct TArr{uintptr_t data;int num,max;}arr{};
    if((size_t)ps[1].second+sizeof(arr)>b.n)return L"Row-name result unreadable";
    memcpy(&arr,(char*)b.p+ps[1].second,sizeof(arr));
    if(!arr.data||arr.num<=0||arr.num>5000)return L"No voices found";
    for(int i=0;i<arr.num;++i){
        unsigned long long raw=0;if(!safeRead(arr.data+(uintptr_t)i*8,&raw,8))continue;
        std::string nm;if(!fnameToString(raw,nm))continue;
        names.push_back(nm);raws.push_back(raw);
    }
    if(names.empty())return L"Voice names unreadable";
    {char m[200];snprintf(m,sizeof(m),"Voice list: %d entries (first %s, last %s)",(int)names.size(),names[0].c_str(),names.back().c_str());logMessage(m);}
    wchar_t r[64];swprintf(r,64,L"Loaded %d voices",(int)names.size());return r;
}
std::wstring applyVoice(unsigned long long raw){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn (are you in game?)";
    std::unordered_set<std::string> w={"GetDaytonCharacter","CheatSetVoice","ApplyCharacterVoice"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
    auto findFnOn=[&](const char* name,uintptr_t objCls,Fn& fn)->bool{
        auto it=hits.find(name);if(it==hits.end())return false;
        for(const Hit& h:it->second){uintptr_t outer=0;
            if(h.cls!="Function"||!rd(h.obj+O_OUTER,outer)||!isA(objCls,outer))continue;
            if(readFn(fn,h.obj,{}))return true;}
        return false;
    };
    Fn fDC,fSet,fApply;
    if(!findFnOn("GetDaytonCharacter",pcls,fDC))return L"GetDaytonCharacter not found";
    uintptr_t dc=0;{Buf b(fDC.size);if(!callPE(processEvent,pawn,fDC.ptr,b.p))return L"Engine call faulted (GetDaytonCharacter)";b.get(fDC,"ReturnValue",dc);}
    if(!dc)return L"Survivor object is null";
    uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
    if(!findFnOn("CheatSetVoice",dcls,fSet))return L"CheatSetVoice not found";
    const auto ps=sortedParams(fSet);if(ps.size()!=1)return L"Voice function layout unexpected (see log)";
    {Buf b(fSet.size);b.putBytes(fSet,ps[0].first.c_str(),&raw,8);
     if(!callPE(processEvent,dc,fSet.ptr,b.p))return L"Engine call faulted (CheatSetVoice)";}
    bool applied=false;
    if(findFnOn("ApplyCharacterVoice",pcls,fApply)){Buf b(fApply.size);applied=callPE(processEvent,pawn,fApply.ptr,b.p);}
    std::string nm;fnameToString(raw,nm);{char m[200];snprintf(m,sizeof(m),"Voice set: %s (ApplyCharacterVoice %s)",nm.c_str(),applied?"ran":"not run");logMessage(m);}
    return applied?L"Voice applied":L"Voice set (apply step unavailable)";
}

// ---- Teleport research: log how the game stores map markers / waypoints (MapStateComponent on the game state).
static void hexLog(const char* tag,uintptr_t addr,int n){
    unsigned char b[128]{};if(n>128)n=128;
    if(!safeRead(addr,b,(size_t)n)){char m[160];snprintf(m,sizeof(m),"%s: unreadable at %llx",tag,(unsigned long long)addr);logMessage(m);return;}
    char hx[128*3+8]{};for(int i=0;i<n;++i)snprintf(hx+i*3,4,"%02X ",b[i]);
    char m[600];snprintf(m,sizeof(m),"%s @%llx: %s",tag,(unsigned long long)addr,hx);logMessage(m);
}
std::wstring teleportResearch(){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);
    if(pawn){float p[3]{};if(actorLocation(pawn,p)){char m[160];snprintf(m,sizeof(m),"Map: player location %.1f %.1f %.1f",p[0],p[1],p[2]);logMessage(m);}}
    std::unordered_set<std::string> w={"GetGameState"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    Fn fGS;uintptr_t gsFn=0;
    if(!findHit(hits,"GetGameState","Function","GameplayStatics",gsFn)||!readFn(fGS,gsFn,{"WorldContextObject","ReturnValue"}))return L"GetGameState not found";
    uintptr_t gs=0;{Buf b(fGS.size);b.put(fGS,"WorldContextObject",world);if(!callPE(processEvent,cdoStatics,fGS.ptr,b.p))return L"Engine call faulted (GetGameState)";b.get(fGS,"ReturnValue",gs);}
    if(!gs)return L"No game state";
    uintptr_t gcls=0;rd(gs+O_CLASS,gcls);{std::string n;objName(gcls,n);char m[160];snprintf(m,sizeof(m),"Map: game state class %s",n.c_str());logMessage(m);}
    const int off=propOffset(gcls,"MapStateComponent");uintptr_t comp=0;
    if(off<0||!rd(gs+(uintptr_t)off,comp)||!comp)return L"MapStateComponent not found (see log)";
    uintptr_t ccls=0;rd(comp+O_CLASS,ccls);
    int logged=0;
    for(uintptr_t cls=ccls;cls&&logged<260;){
        std::string cn;objName(cls,cn);
        {char m[160];snprintf(m,sizeof(m),"Map: class %s",cn.c_str());logMessage(m);}
        if(cn=="ActorComponent"||cn=="Object"||cn=="SceneComponent")break;
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int g=0;c&&g<400&&logged<260;++g,++logged){
            std::string nm,tn;uintptr_t tcls=0;objName(c,nm);if(rd(c+O_CLASS,tcls))objName(tcls,tn);
            char m[300];
            if(tn=="Function"){
                Fn f;std::string ps;
                if(readFn(f,c,{})){for(const auto& pr:sortedParams(f)){ps+=pr.first;ps+="@"+std::to_string(pr.second)+" ";}}
                snprintf(m,sizeof(m),"Map:   fn %s(%s)",nm.c_str(),ps.c_str());
            }else{
                int po=-1;rd(c+O_PROPOFF,po);
                snprintf(m,sizeof(m),"Map:   %s %s offset %d",tn.c_str(),nm.c_str(),po);
                if(po>=0&&(icontains(nm,"waypoint")||icontains(nm,"marker")||icontains(nm,"pin")||icontains(nm,"destination")||icontains(nm,"location"))){
                    logMessage(m);
                    if(tn=="ArrayProperty"){
                        struct TArr{uintptr_t data;int num,max;}a{};
                        if(safeRead(comp+(uintptr_t)po,&a,sizeof(a))){char h[120];snprintf(h,sizeof(h),"Map:     array %s num=%d",nm.c_str(),a.num);logMessage(h);if(a.data&&a.num>0)hexLog("Map:     first element",a.data,96);}
                    }else hexLog(("Map:     value "+nm).c_str(),comp+(uintptr_t)po,48);
                    uintptr_t nx2=0;if(!rd(c+O_NEXT,nx2))break;c=nx2;continue;
                }
            }
            logMessage(m);
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    // Other objects whose names mention waypoints / map markers.
    refreshCount();int n2=0;
    for(int i=0;i<numObjs&&n2<120;++i){
        uintptr_t o=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o)continue;
        std::string nm;if(!objName(o,nm))continue;
        if(!(icontains(nm,"waypoint")||icontains(nm,"mapmarker")||icontains(nm,"playermarker")))continue;
        uintptr_t cls=0,outer=0;std::string cn,on;
        if(rd(o+O_CLASS,cls))objName(cls,cn);
        if(cn=="Function"||cn.find("Property")!=std::string::npos||cn=="ScriptStruct"||cn=="Class"||cn=="BlueprintGeneratedClass"){
            if(rd(o+O_OUTER,outer))objName(outer,on);
            char m[300];snprintf(m,sizeof(m),"Waypoint: %s %s.%s",cn.c_str(),on.c_str(),nm.c_str());logMessage(m);++n2;
        }
    }
    return L"Map data logged - research build, no teleport yet";
}

// ---- Teleport to the map marker. Uses the game's own DaytonCheatManager.ULTeleportToWaypoint when a cheat manager exists;
// otherwise logs what a manual teleport would need (marker position, world bounds, player location) without moving anything.

// ---- Community resources, Combine All Items, Mission Timer ------------------------------------------------------------
struct CommResult{std::wstring status;bool haveNames=false;std::wstring first,last,nick;bool haveNick=false;bool haveList=false;std::vector<std::wstring> labels;std::vector<float> amts;};
bool resolveFnOn(const std::unordered_map<std::string,std::vector<Hit>>& hits,const char* name,uintptr_t objCls,Fn& f){
    auto it=hits.find(name);if(it==hits.end())return false;
    for(const Hit& h:it->second){uintptr_t outer=0;
        if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(objCls,outer)&&readFn(f,h.obj,{}))return true;}
    return false;
}
uintptr_t findLiveInstance(uintptr_t cls){
    refreshCount();
    for(int i=0;i<numObjs;++i){
        uintptr_t o=0,c=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o||!rd(o+O_CLASS,c)||c!=cls)continue;
        std::string nm;if(!objName(o,nm)||nm.compare(0,9,"Default__")==0)continue;
        return o;
    }
    return 0;
}
// Log every data property of a class chain (once per run) so field names can be checked in the log.
void dumpClassProps(const char* tag,uintptr_t cls,uintptr_t inst){
    char m[240];
    for(int g=0;cls&&g<8;++g){
        std::string cn;objName(cls,cn);
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<256;++h){
            std::string n,tn;uintptr_t pc=0;objName(c,n);rd(c+O_CLASS,pc);objName(pc,tn);int off=-1;rd(c+O_PROPOFF,off);
            if(tn!="Function"){
                float fv=0;int iv=0;
                if(inst&&off>=0&&tn=="FloatProperty"){safeRead(inst+off,&fv,4);snprintf(m,sizeof(m),"%s: %s.%s [%s @%d] = %.3f",tag,cn.c_str(),n.c_str(),tn.c_str(),off,fv);}
                else if(inst&&off>=0&&tn=="IntProperty"){safeRead(inst+off,&iv,4);snprintf(m,sizeof(m),"%s: %s.%s [%s @%d] = %d",tag,cn.c_str(),n.c_str(),tn.c_str(),off,iv);}
                else snprintf(m,sizeof(m),"%s: %s.%s [%s @%d]",tag,cn.c_str(),n.c_str(),tn.c_str(),off);
                logMessage(m);
            }
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
}
// Offset of the float property holding a resource's stock, or -1.
int findAmountOffset(uintptr_t cls){
    static std::unordered_map<uintptr_t,int> cache;
    auto ci=cache.find(cls);if(ci!=cache.end())return ci->second;
    std::vector<std::pair<std::string,int>> fl;
    uintptr_t k=cls;
    for(int g=0;k&&g<8;++g){
        uintptr_t c=0;rd(k+O_CHILDREN,c);
        for(int h=0;c&&h<256;++h){
            std::string n,tn;uintptr_t pc=0;objName(c,n);rd(c+O_CLASS,pc);objName(pc,tn);int off=-1;rd(c+O_PROPOFF,off);
            if(tn=="FloatProperty"&&off>=0)fl.push_back({n,off});
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(k+O_SUPER,sup))break;k=sup;
    }
    static const char* want[]={"Supply","Amount","CurrentAmount","StockpileAmount","Stockpile","Quantity","Count","CurrentValue","Current","Value","Stock"};
    int found=-1;std::string fname;
    for(const char* w:want){for(const auto& pr:fl)if(pr.first==w){found=pr.second;fname=pr.first;break;}if(found>=0)break;}
    if(found<0)for(const auto& pr:fl){std::string lo=lowerStr(pr.first);
        if(lo.find("amount")!=std::string::npos||lo.find("stock")!=std::string::npos||lo.find("current")!=std::string::npos){found=pr.second;fname=pr.first;break;}}
    char m[200];snprintf(m,sizeof(m),"Community: resource amount property %s @%d",found>=0?fname.c_str():"NOT FOUND",found);logMessage(m);
    cache[cls]=found;return found;
}
std::wstring friendlyResName(uintptr_t obj,int idx){
    std::string nm,cn;objName(obj,nm);uintptr_t c=0;rd(obj+O_CLASS,c);objName(c,cn);
    const std::string lo=lowerStr(nm+" "+cn);
    if(lo.find("food")!=std::string::npos)return L"Food";
    if(lo.find("meds")!=std::string::npos)return L"Medicine";
    if(lo.find("influence")!=std::string::npos)return L"Influence";
    if(lo.find("prestige")!=std::string::npos)return L"Prestige";
    if(lo.find("parts")!=std::string::npos)return L"Parts";
    if(lo.find("fuel")!=std::string::npos)return L"Fuel";
    if(lo.find("medic")!=std::string::npos)return L"Medicine";
    if(lo.find("ammo")!=std::string::npos)return L"Ammo";
    if(lo.find("material")!=std::string::npos||lo.find("build")!=std::string::npos)return L"Materials";
    std::string cl=cn;if(cl.size()>2&&cl.compare(cl.size()-2,2,"_C")==0)cl.resize(cl.size()-2);
    wchar_t buf[96];swprintf(buf,96,L"%ls (%d)",widen(cl).c_str(),idx);return buf;
}
// Resource objects of the player's community; false + err on failure.
bool getResources(const std::unordered_map<std::string,std::vector<Hit>>& hits,uintptr_t world,std::vector<uintptr_t>& list,std::wstring& err){
    Fn fPC;uintptr_t pcFn=0,pc=0;
    if(findHit(hits,"GetPlayerController","Function","GameplayStatics",pcFn)&&readFn(fPC,pcFn,{"WorldContextObject","ReturnValue"})){
        Buf b(fPC.size);b.put(fPC,"WorldContextObject",world);b.put(fPC,"PlayerIndex",(int)0);
        if(callPE(processEvent,cdoStatics,fPC.ptr,b.p))b.get(fPC,"ReturnValue",pc);
    }
    if(!pc){err=L"No player controller yet";return false;}
    uintptr_t pcls=0;rd(pc+O_CLASS,pcls);
    Fn fCC;if(!resolveFnOn(hits,"GetCommunityComponent",pcls,fCC)){err=L"GetCommunityComponent not found";return false;}
    uintptr_t comp=0;{Buf b(fCC.size);if(callPE(processEvent,pc,fCC.ptr,b.p))b.get(fCC,"ReturnValue",comp);}
    if(!comp){err=L"No community yet (load into a save)";return false;}
    uintptr_t ccls=0;rd(comp+O_CLASS,ccls);const int ro=propOffset(ccls,"Resources");
    if(ro<0){err=L"Resources list not found";return false;}
    struct TArr{uintptr_t data;int num,max;}a{};
    if(!safeRead(comp+(uintptr_t)ro,&a,sizeof(a))||!a.data||a.num<=0||a.num>64){err=L"No resources found";return false;}
    for(int i=0;i<a.num;++i){uintptr_t o=0;if(safeRead(a.data+(uintptr_t)i*8,&o,8)&&o)list.push_back(o);}
    if(list.empty()){err=L"No resources found";return false;}
    return true;
}
float readAmount(uintptr_t obj){
    uintptr_t cls=0;rd(obj+O_CLASS,cls);const int off=findAmountOffset(cls);
    float v=-1.f;if(off>=0)safeRead(obj+(uintptr_t)off,&v,4);return v;
}
void fillResourceList(const std::vector<uintptr_t>& list,CommResult& r){
    static bool dumped=false;
    if(!dumped){dumped=true;char m[200];snprintf(m,sizeof(m),"Community: %llu resources",(unsigned long long)list.size());logMessage(m);
        for(size_t i=0;i<list.size();++i){std::string nm,cn;uintptr_t c=0;objName(list[i],nm);rd(list[i]+O_CLASS,c);objName(c,cn);
            snprintf(m,sizeof(m),"Community: resource %llu = %s (%s)",(unsigned long long)i,nm.c_str(),cn.c_str());logMessage(m);}
        uintptr_t c0=0;rd(list[0]+O_CLASS,c0);dumpClassProps("Community props",c0,list[0]);}
    for(size_t i=0;i<list.size();++i){r.labels.push_back(friendlyResName(list[i],(int)i));r.amts.push_back(readAmount(list[i]));}
    r.haveList=true;
}
// Property object (not its offset) by name along a class chain.
uintptr_t findPropObj(uintptr_t cls,const char* name){
    for(int g=0;cls&&g<16;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<1024;++h){
            std::string n;if(objName(c,n)&&n==name)return c;
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    return 0;
}
// The ScriptStruct a StructProperty points at (searches the property's tail for a pointer to a ScriptStruct).
uintptr_t structOfProp(uintptr_t prop){
    for(int off=0x58;off<=0x98;off+=8){
        uintptr_t sp=0,sc=0;std::string cn;
        if(rd(prop+(uintptr_t)off,sp)&&sp&&rd(sp+O_CLASS,sc)&&sc&&objName(sc,cn)&&cn=="ScriptStruct")return sp;
    }
    return 0;
}
// Log the fields of a struct instance at 'base' with their values.
void dumpStructValues(const char* tag,uintptr_t structCls,uintptr_t base){
    char m[300];
    for(int g=0;structCls&&g<4;++g){
        std::string sn;objName(structCls,sn);
        uintptr_t c=0;rd(structCls+O_CHILDREN,c);
        for(int h=0;c&&h<128;++h){
            std::string n,tn;uintptr_t pc=0;objName(c,n);rd(c+O_CLASS,pc);objName(pc,tn);int off=-1;rd(c+O_PROPOFF,off);
            if(off>=0&&tn!="Function"){
                if(tn=="IntProperty"){int v=0;safeRead(base+off,&v,4);snprintf(m,sizeof(m),"%s: %s.%s [int @%d] = %d",tag,sn.c_str(),n.c_str(),off,v);}
                else if(tn=="FloatProperty"){float v=0;safeRead(base+off,&v,4);snprintf(m,sizeof(m),"%s: %s.%s [float @%d] = %.3f",tag,sn.c_str(),n.c_str(),off,v);}
                else if(tn=="BoolProperty"||tn=="ByteProperty"){unsigned char v=0;safeRead(base+off,&v,1);snprintf(m,sizeof(m),"%s: %s.%s [%s @%d] = %d",tag,sn.c_str(),n.c_str(),tn.c_str(),off,(int)v);}
                else snprintf(m,sizeof(m),"%s: %s.%s [%s @%d]",tag,sn.c_str(),n.c_str(),tn.c_str(),off);
                logMessage(m);
            }
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(structCls+O_SUPER,sup))break;structCls=sup;
    }
}
// Log every function of a class chain with its parameters.
// Read-only: log the value of every property of a class/struct instance at `base`, descending into structs.
static bool textToStr(uintptr_t cdoTxt,const Fn& fT2S,const std::vector<std::pair<std::string,int>>& tps,uintptr_t addr,std::string& out){
    if(!cdoTxt||!fT2S.ptr||tps.size()!=1)return false;
    unsigned char raw[32]{};if(!safeRead(addr,raw,24))return false;
    Buf b(fT2S.size);b.putBytes(fT2S,tps[0].first.c_str(),raw,24);
    if(!callStatic(cdoTxt,fT2S,b))return false;
    FStr fs{};if(!b.get(fT2S,"ReturnValue",fs)||!fs.data||fs.num<=0||fs.num>300)return false;
    wchar_t tmp[310]{};if(!safeRead((uintptr_t)fs.data,tmp,(size_t)fs.num*2))return false;
    tmp[fs.num<309?fs.num:309]=0;out.clear();for(const wchar_t* q=tmp;*q;++q)out.push_back(*q<128?(char)*q:'?');return true;
}
void dumpValues(const char* tag,const std::string& path,uintptr_t base,uintptr_t cls,int depth,uintptr_t cdoTxt,const Fn& fT2S,const std::vector<std::pair<std::string,int>>& tps){
    for(int g=0;cls&&g<8;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<256;++h){
            std::string n,tn;uintptr_t pc=0;objName(c,n);rd(c+O_CLASS,pc);objName(pc,tn);int off=-1;rd(c+O_PROPOFF,off);
            if(off>=0&&tn!="Function"){
                const uintptr_t a=base+(uintptr_t)off;std::string val;char t[96];
                if(tn=="TextProperty"){std::string v;val=textToStr(cdoTxt,fT2S,tps,a,v)?"\""+v+"\"":"(unreadable)";}
                else if(tn=="StrProperty"){FStr fs{};std::string v;if(safeRead(a,&fs,sizeof(fs))&&fs.data&&fs.num>0&&fs.num<=200){wchar_t tmp[210]{};if(safeRead((uintptr_t)fs.data,tmp,(size_t)fs.num*2)){tmp[fs.num<209?fs.num:209]=0;for(const wchar_t* q=tmp;*q;++q)v.push_back(*q<128?(char)*q:'?');}}val="\""+v+"\"";}
                else if(tn=="IntProperty"){int v=0;safeRead(a,&v,4);val=std::to_string(v);}
                else if(tn=="FloatProperty"){float v=0;safeRead(a,&v,4);snprintf(t,sizeof(t),"%.3f",v);val=t;}
                else if(tn=="ByteProperty"||tn=="BoolProperty"||tn=="EnumProperty"){unsigned char v=0;safeRead(a,&v,1);val=std::to_string((int)v);}
                else if(tn=="NameProperty"){unsigned int v[2]{};safeRead(a,v,8);val="name idx "+std::to_string(v[0]);}
                else if(tn=="ObjectProperty"||tn=="ClassProperty"){uintptr_t v=0;std::string vn="(null)";if(safeRead(a,&v,8)&&v){vn.clear();objName(v,vn);}val=vn;}
                else if(tn=="ArrayProperty"){int num=0;safeRead(a+8,&num,4);val="array of "+std::to_string(num);}
                else if(tn=="StructProperty"){
                    const uintptr_t st=structOfProp(c);std::string sn;if(st)objName(st,sn);
                    val="struct "+sn;
                    snprintf(t,sizeof(t),"%s: %s%s @%d = %s",tag,path.c_str(),n.c_str(),off,val.c_str());logMessage(t);
                    if(st&&depth<3)dumpValues(tag,path+n+".",a,st,depth+1,cdoTxt,fT2S,tps);
                    uintptr_t nx2=0;if(!rd(c+O_NEXT,nx2))break;c=nx2;continue;
                }else{unsigned char v[16]{};safeRead(a,v,16);snprintf(t,sizeof(t),"raw %02X%02X%02X%02X%02X%02X%02X%02X",v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);val=t;}
                std::string ln=std::string(tag)+": "+path+n+" ["+tn+" @"+std::to_string(off)+"] = "+val;logMessage(ln.c_str());
            }
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
}
void dumpFunctions(const char* tag,uintptr_t cls){
    char m[400];
    for(int g=0;cls&&g<6;++g){
        std::string cn;objName(cls,cn);
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<512;++h){
            std::string n,tn;uintptr_t pc=0;objName(c,n);rd(c+O_CLASS,pc);objName(pc,tn);
            if(tn=="Function"){
                std::string ps;uintptr_t p=0;rd(c+O_CHILDREN,p);
                for(int q=0;p&&q<16;++q){std::string pn,pt;uintptr_t ppc=0;objName(p,pn);rd(p+O_CLASS,ppc);objName(ppc,pt);int po=-1;rd(p+O_PROPOFF,po);
                    if(pt.size()>8&&pt.compare(pt.size()-8,8,"Property")==0)pt.resize(pt.size()-8);
                    ps+=pn+":"+pt+"@"+std::to_string(po)+" ";
                    uintptr_t nx=0;if(!rd(p+O_NEXT,nx))break;p=nx;}
                snprintf(m,sizeof(m),"%s: %s::%s(%s)",tag,cn.c_str(),n.c_str(),ps.c_str());logMessage(m);
            }
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
}
// ===== Aimbot feasibility probe (READ-ONLY: only reads values and calls harmless getters; changes nothing in the game) =====
static const char* const kwState[]={"dead","alive","dying","death","health","knock","ragdoll","incap","downed","stun","ground","fallen"};
static const char* const kwAim[]={"aim","zoom","scope","sight","iron","ads_"};
static const char* const kwRot[]={"rotation","lookat","viewpoint","aimpoint","yawinput","pitchinput"};
static bool kwHas(const std::string& name,const char* const* k,int n){const std::string l=lowerStr(name);for(int i=0;i<n;++i)if(l.find(k[i])!=std::string::npos)return true;return false;}
static void plog(const std::string& s){for(size_t i=0;i<s.size();i+=300){const std::string part=s.substr(i,300);logMessage(part.c_str());}}
static std::string chainStr(uintptr_t cls,int maxN){
    std::string out;
    for(int g=0;cls&&g<maxN;++g){std::string n;objName(cls,n);out+=(g?" > ":"")+n;uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;}
    return out;
}
static std::string rawHex(uintptr_t addr,int len){
    unsigned char b[16]{};if(len>16)len=16;std::string out;
    if(!addr||!safeRead(addr,b,len))return "??";
    char t[4];for(int i=0;i<len;++i){snprintf(t,sizeof(t),"%02x",b[i]);out+=t;}
    return out;
}
// Function objects (by name) in a class chain.
static uintptr_t chainFindFn(uintptr_t cls,const char* name){
    for(int g=0;cls&&g<16;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<1024;++h){
            std::string n,tn;uintptr_t pc=0;objName(c,n);rd(c+O_CLASS,pc);objName(pc,tn);
            if(tn=="Function"&&n==name)return c;
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    return 0;
}
static std::string fnSig(uintptr_t fn){
    std::string ps;uintptr_t p=0;rd(fn+O_CHILDREN,p);
    for(int q=0;p&&q<16;++q){std::string pn,pt;uintptr_t ppc=0;objName(p,pn);rd(p+O_CLASS,ppc);objName(ppc,pt);int po=-1;rd(p+O_PROPOFF,po);
        if(pt.size()>8&&pt.compare(pt.size()-8,8,"Property")==0)pt.resize(pt.size()-8);
        ps+=pn+":"+pt+"@"+std::to_string(po)+" ";
        uintptr_t nx=0;if(!rd(p+O_NEXT,nx))break;p=nx;}
    std::string n;objName(fn,n);return n+"("+ps+")";
}
// Functions in a class chain whose name has a keyword. getters=true: only no-input Is/Get/Has/Can functions (safe to call).
static std::vector<uintptr_t> chainFns(uintptr_t cls,const char* const* k,int n,bool getters){
    std::vector<uintptr_t> out;
    for(int g=0;cls&&g<16;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<1024&&out.size()<80;++h){
            std::string nm,tn;uintptr_t pc=0;objName(c,nm);rd(c+O_CLASS,pc);objName(pc,tn);
            if(tn=="Function"&&kwHas(nm,k,n)){
                bool ok=true;
                if(getters){
                    ok=nm.compare(0,2,"Is")==0||nm.compare(0,3,"Get")==0||nm.compare(0,3,"Has")==0||nm.compare(0,3,"Can")==0;
                    uintptr_t p=0;rd(c+O_CHILDREN,p);
                    for(int q=0;ok&&p&&q<16;++q){std::string pn;objName(p,pn);if(pn!="ReturnValue")ok=false;uintptr_t nx=0;if(!rd(p+O_NEXT,nx))break;p=nx;}
                }
                if(ok)out.push_back(c);
            }
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    return out;
}
static bool callGetter(uintptr_t obj,uintptr_t fn,unsigned char out[8]){
    Fn f;if(!readFn(f,fn,{})||!f.off.count("ReturnValue"))return false;
    Buf b(f.size);if(!callPE(processEvent,obj,f.ptr,b.p))return false;
    memset(out,0,8);const int off=f.off["ReturnValue"];
    memcpy(out,(char*)b.p+off,(size_t)(std::min)(8,(std::max)(0,f.size-off)));return true;
}
static std::string hex8(const unsigned char* b){char t[24];snprintf(t,sizeof(t),"%02x%02x%02x%02x%02x%02x%02x%02x",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);return t;}
// Data properties in a class chain whose name has a keyword: (name, offset).
static std::vector<std::pair<std::string,int>> chainProps(uintptr_t cls,const char* const* k,int n,std::vector<std::string>* types=nullptr){
    std::vector<std::pair<std::string,int>> out;
    for(int g=0;cls&&g<16;++g){
        uintptr_t c=0;rd(cls+O_CHILDREN,c);
        for(int h=0;c&&h<1024&&out.size()<60;++h){
            std::string nm,tn;uintptr_t pc=0;objName(c,nm);rd(c+O_CLASS,pc);objName(pc,tn);int off=-1;rd(c+O_PROPOFF,off);
            if(off>=0&&tn!="Function"&&tn.size()>8&&tn.compare(tn.size()-8,8,"Property")==0&&kwHas(nm,k,n)){out.push_back({nm,off});if(types)types->push_back(tn);}
            uintptr_t nx=0;if(!rd(c+O_NEXT,nx))break;c=nx;
        }
        uintptr_t sup=0;if(!rd(cls+O_SUPER,sup))break;cls=sup;
    }
    return out;
}
static bool rootPos(uintptr_t actor,uintptr_t cls,float out[3]){
    const int ro=propOffset(cls,"RootComponent");uintptr_t root=0;
    if(ro>=0)rd(actor+(uintptr_t)ro,root);
    return root&&safeRead(root+0x170,out,12);
}
struct ProbeZ{uintptr_t obj=0,cls=0;int idx=-1;float d=0;float p[3]{};};
static std::vector<ProbeZ> collectZombies(const float pl[3],float maxD,bool verbose=true,size_t cap=12){
    std::vector<ProbeZ> out;static std::unordered_map<uintptr_t,bool> memo;refreshCount();
    for(int i=0;i<numObjs;++i){
        uintptr_t o=0,cls=0;
        if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o||!rd(o+O_CLASS,cls)||!cls)continue;
        auto m=memo.find(cls);bool match;
        if(m==memo.end()){
            bool z=false,bad=false;uintptr_t c=cls;
            for(int g=0;c&&g<16;++g){std::string n;objName(c,n);const std::string l=lowerStr(n);
                if(l=="daytonzombiecharacter")z=true;
                if(l.find("controller")!=std::string::npos||l.find("spawner")!=std::string::npos||l.find("component")!=std::string::npos||l.find("horde")!=std::string::npos)bad=true;
                uintptr_t sup=0;if(!rd(c+O_SUPER,sup))break;c=sup;}
            match=z&&!bad;memo[cls]=match;
        }else match=m->second;
        if(!match)continue;
        int fl=0,ifl=0;
        if(!rd(o+8,fl)||(fl&0x30)||(fl&0x20000000))continue;
        if(rd(objs+(size_t)i*ITEM_SIZE+8,ifl)&&(ifl&0x20000000))continue;
        std::string nm;if(!objName(o,nm))continue;
        if(nm.compare(0,9,"Default__")==0||nm.compare(0,7,"REINST_")==0||nm.compare(0,5,"SKEL_")==0)continue;
        uintptr_t outer=0,oc=0;std::string on;
        if(!rd(o+O_OUTER,outer)||!rd(outer+O_CLASS,oc)||!objName(oc,on)||on!="Level")continue;
        ProbeZ z;z.obj=o;z.cls=cls;z.idx=i;
        if(!rootPos(o,cls,z.p))continue;
        const float dx=z.p[0]-pl[0],dy=z.p[1]-pl[1],dz=z.p[2]-pl[2];z.d=sqrtf(dx*dx+dy*dy+dz*dz);
        if(z.d<=maxD)out.push_back(z);
    }
    std::sort(out.begin(),out.end(),[](const ProbeZ& a,const ProbeZ& b){return a.d<b.d;});
    if(verbose){std::map<std::string,int> tally;for(const ProbeZ& z:out){std::string n;objName(z.cls,n);tally[n]++;}
     std::string l="Aimbot probe: zombie classes within range (all, not just nearest):";for(const auto& kv:tally)l+=" "+kv.first+"x"+std::to_string(kv.second);plog(l);}
    if(out.size()>cap)out.resize(cap);
    return out;
}
static std::string fnameToStr(uintptr_t cdoStr,const Fn& fN2S,const unsigned char name8[8]){
    Buf b(fN2S.size);b.putBytes(fN2S,"InName",name8,8);
    if(!callPE(processEvent,cdoStr,fN2S.ptr,b.p))return "?";
    FStr fs{};if(!b.get(fN2S,"ReturnValue",fs)||!fs.data||fs.num<=0||fs.num>128)return "";
    wchar_t t[130]{};if(!safeRead((uintptr_t)fs.data,t,(size_t)fs.num*2))return "?";
    std::string o;for(int i=0;i<fs.num&&t[i];++i)o+=(t[i]<128?(char)t[i]:'?');return o;
}
// stage 1: everything static (one pass). stage 2: one compact snapshot line (called once a second while the user aims). stage 3: done.
struct CamTest{uintptr_t pc=0,pcCls=0,pawn=0,fSet=0,fAddC=0,fAddP=0;int rotOff=-1;float saved[3]{};};
static CamTest g_cam;
std::wstring aimProbe(int stage,int keys,uintptr_t world){
    char m[400];
    if(stage>=5&&stage<=9){   // camera test steps: use cached pointers only (fast, no scan)
        CamTest& c=g_cam;uintptr_t cc=0;
        if(!c.pc||!rd(c.pc+O_CLASS,cc)||cc!=c.pcCls)return L"Camera test: not initialised";
        float now[3]{};safeRead(c.pc+(uintptr_t)c.rotOff,now,12);
        auto addInput=[&](uintptr_t fn,uintptr_t obj,float val){Fn f;if(!fn||!readFn(f,fn,{}))return false;Buf b(f.size);b.put(f,"Val",val);return callPE(processEvent,obj,f.ptr,b.p);};
        auto setRot=[&](const float r[3]){Fn f;if(!readFn(f,c.fSet,{}))return false;Buf b(f.size);b.putBytes(f,"NewRotation",r,12);return callPE(processEvent,c.pc,f.ptr,b.p);};
        static const char* const labels[]={"baseline","after AddYawInput(controller)","after AddControllerYawInput(pawn)","during SetControlRotation spam","after spam stopped","after restore"};
        if(stage==6){snprintf(m,sizeof(m),"Camera test [%s]: ControlRotation %.1f %.1f %.1f (start %.1f %.1f %.1f)",labels[(std::min)(5,keys)],now[0],now[1],now[2],c.saved[0],c.saved[1],c.saved[2]);logMessage(m);return L"Camera test running...";}
        if(stage==7||stage==8){
            const bool ok=stage==7?addInput(c.fAddC,c.pc,(float)keys):addInput(c.fAddP,c.pawn,(float)keys);
            float after[3]{};safeRead(c.pc+(uintptr_t)c.rotOff,after,12);
            snprintf(m,sizeof(m),"Camera test: %s(%d) call %s; yaw %.1f -> %.1f",stage==7?"AddYawInput(controller)":"AddControllerYawInput(pawn)",keys,ok?"ok":"FAILED",now[1],after[1]);logMessage(m);
            return L"Camera test running...";
        }
        if(stage==9){float t[3]={c.saved[0],c.saved[1]+30.f,c.saved[2]};setRot(t);return L"Camera test running...";}
        if(stage==5){setRot(c.saved);snprintf(m,sizeof(m),"Camera test: restored saved rotation %.1f %.1f %.1f",c.saved[0],c.saved[1],c.saved[2]);logMessage(m);return L"Camera test finished";}
    }
    const uintptr_t pawn=playerPawn(world);
    if(!pawn)return L"Probe: no player pawn (are you in game?)";
    uintptr_t pawnCls=0;rd(pawn+O_CLASS,pawnCls);
    std::unordered_set<std::string> w={"GetPlayerController","Default__KismetStringLibrary","Conv_NameToString"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    Fn fPC;uintptr_t pcFn=0,pc=0;
    if(findHit(hits,"GetPlayerController","Function","GameplayStatics",pcFn)&&readFn(fPC,pcFn,{"WorldContextObject","ReturnValue"})){
        Buf b(fPC.size);b.put(fPC,"WorldContextObject",world);b.put(fPC,"PlayerIndex",(int)0);
        if(callPE(processEvent,cdoStatics,fPC.ptr,b.p))b.get(fPC,"ReturnValue",pc);
    }
    uintptr_t pcCls=0;if(pc)rd(pc+O_CLASS,pcCls);
    const int rotOff=pcCls?propOffset(pcCls,"ControlRotation"):-1;
    float cr[3]{};if(pc&&rotOff>=0)safeRead(pc+(uintptr_t)rotOff,cr,12);
    float pl[3]{};if(!rootPos(pawn,pawnCls,pl))actorLocation(pawn,pl);
    const auto aimP=chainProps(pawnCls,kwAim,6);const auto aimPc=pcCls?chainProps(pcCls,kwAim,6):std::vector<std::pair<std::string,int>>();
    const auto aimF=chainFns(pawnCls,kwAim,6,true);
    if(stage==4){   // camera test: initialise (cache everything so later steps need no scan)
        CamTest& c=g_cam;c=CamTest();
        c.pc=pc;c.pcCls=pcCls;c.pawn=pawn;c.rotOff=rotOff;
        c.fSet=pcCls?chainFindFn(pcCls,"SetControlRotation"):0;
        c.fAddC=pcCls?chainFindFn(pcCls,"AddYawInput"):0;
        c.fAddP=chainFindFn(pawnCls,"AddControllerYawInput");
        if(!pc||rotOff<0||!c.fSet){c.pc=0;return L"Camera test: controller functions not found";}
        safeRead(pc+(uintptr_t)rotOff,c.saved,12);
        snprintf(m,sizeof(m),"Camera test: init ok, saved ControlRotation %.1f %.1f %.1f; AddYawInput(ctrl) %s, AddControllerYawInput(pawn) %s",c.saved[0],c.saved[1],c.saved[2],c.fAddC?"found":"MISSING",c.fAddP?"found":"MISSING");logMessage(m);
        return L"Camera test running - keep the mouse still";
    }
    if(stage==2){
        std::string line="Aimbot probe tick: RMB="+std::to_string(keys&1)+" LMB="+std::to_string((keys>>1)&1)+" ControlRot="+std::to_string((int)cr[0])+","+std::to_string((int)cr[1])+","+std::to_string((int)cr[2]);
        for(const auto& pr:aimP)line+=" P."+pr.first+"="+rawHex(pawn+(uintptr_t)pr.second,4);
        for(const auto& pr:aimPc)line+=" C."+pr.first+"="+rawHex(pc+(uintptr_t)pr.second,4);
        for(uintptr_t fn:aimF){unsigned char o[8];std::string n;objName(fn,n);line+=" F."+n+"="+(callGetter(pawn,fn,o)?hex8(o):std::string("err"));}
        plog(line);return L"Probe running...";
    }
    if(stage==3){logMessage("Aimbot probe: finished");return L"Probe finished - please send the log";}
    logMessage("===== Aimbot probe: start =====");
    plog("Aimbot probe: pawn class chain: "+chainStr(pawnCls,8));
    plog("Aimbot probe: controller class chain: "+(pcCls?chainStr(pcCls,8):std::string("(none)")));
    snprintf(m,sizeof(m),"Aimbot probe: player at %d %d %d; ControlRotation offset %d value %.1f %.1f %.1f (pitch/yaw/roll)",(int)pl[0],(int)pl[1],(int)pl[2],rotOff,cr[0],cr[1],cr[2]);logMessage(m);
    // can the view be turned? which candidate functions exist
    {std::string line="Aimbot probe: view functions: ";
     struct Q{const char* name;uintptr_t cls;const char* on;};
     const Q qs[]={{"SetControlRotation",pcCls,"controller"},{"GetControlRotation",pcCls,"controller"},{"GetViewRotation",pcCls,"controller"},{"GetPlayerViewPoint",pcCls,"controller"},
                   {"AddYawInput",pcCls,"controller"},{"AddPitchInput",pcCls,"controller"},{"K2_SetActorRotation",pawnCls,"pawn"},{"GetBaseAimRotation",pawnCls,"pawn"},{"AddControllerYawInput",pawnCls,"pawn"},{"AddControllerPitchInput",pawnCls,"pawn"},{"SetActorRotation",pawnCls,"pawn"}};
     for(const Q& q:qs){const uintptr_t fn=q.cls?chainFindFn(q.cls,q.name):0;line+=std::string(q.name)+"("+q.on+")="+(fn?fnSig(fn):std::string("MISSING"))+"; ";}
     plog(line);}
    for(uintptr_t fn:chainFns(pcCls,kwRot,6,false))plog("Aimbot probe: controller fn: "+fnSig(fn));
    for(uintptr_t fn:chainFns(pawnCls,kwAim,6,false))plog("Aimbot probe: pawn aim fn: "+fnSig(fn));
    for(uintptr_t fn:chainFns(pawnCls,kwRot,6,false))plog("Aimbot probe: pawn rotation fn: "+fnSig(fn));
    {std::string l="Aimbot probe: pawn aim-like properties:";for(const auto& pr:aimP)l+=" "+pr.first+"@"+std::to_string(pr.second);plog(l);}
    {std::string l="Aimbot probe: controller aim-like properties:";for(const auto& pr:aimPc)l+=" "+pr.first+"@"+std::to_string(pr.second);plog(l);}
    {   // where could the real view rotation live? list rotation-like properties on the controller, pawn and camera manager
        static const char* const kwR[]={"rot","yaw","pitch","look","camera"};
        auto dumpR=[&](const char* tag,uintptr_t obj,uintptr_t cls){
            std::vector<std::string> ty;const auto ps=cls?chainProps(cls,kwR,5,&ty):std::vector<std::pair<std::string,int>>();
            std::string l=std::string("Aimbot probe: rotation-like properties on ")+tag+":";
            for(size_t i=0;i<ps.size();++i)l+=" "+ps[i].first+"("+ty[i]+")@"+std::to_string(ps[i].second)+"="+rawHex(obj+(uintptr_t)ps[i].second,4);
            plog(l);};
        dumpR("controller",pc,pcCls);dumpR("pawn",pawn,pawnCls);
        const int cmo=pcCls?propOffset(pcCls,"PlayerCameraManager"):-1;uintptr_t cam=0;if(cmo>=0)rd(pc+(uintptr_t)cmo,cam);
        uintptr_t camCls=0;if(cam)rd(cam+O_CLASS,camCls);
        plog("Aimbot probe: camera manager chain: "+(camCls?chainStr(camCls,6):std::string("(none)")));
        dumpR("camera manager",cam,camCls);
        const uintptr_t fEye=chainFindFn(pawnCls,"GetActorEyesViewPoint");
        if(fEye){Fn f;if(readFn(f,fEye,{})){Buf b(f.size);if(callPE(processEvent,pawn,f.ptr,b.p)){float el[3]{},er[3]{};b.get(f,"OutLocation",el);b.get(f,"OutRotation",er);
            snprintf(m,sizeof(m),"Aimbot probe: pawn eyes viewpoint location %d %d %d rotation %.1f %.1f %.1f (player root %d %d %d)",(int)el[0],(int)el[1],(int)el[2],er[0],er[1],er[2],(int)pl[0],(int)pl[1],(int)pl[2]);logMessage(m);}}}
    }
    // zombies
    const auto zs=collectZombies(pl,8000.f);
    snprintf(m,sizeof(m),"Aimbot probe: %d zombie-like actors within 80 m (nearest first, max 12)",(int)zs.size());logMessage(m);
    std::vector<std::string> ptypes;
    const auto stateP=zs.empty()?std::vector<std::pair<std::string,int>>():chainProps(zs[0].cls,kwState,12,&ptypes);
    const auto stateF=zs.empty()?std::vector<uintptr_t>():chainFns(zs[0].cls,kwState,12,true);
    {std::string l="Aimbot probe: state-like properties on zombie class:";for(size_t i=0;i<stateP.size();++i)l+=" "+stateP[i].first+"("+ptypes[i]+")@"+std::to_string(stateP[i].second);plog(l);}
    {std::string l="Aimbot probe: state-like getter functions on zombie class:";for(uintptr_t fn:stateF)l+=" "+fnSig(fn);plog(l);}
    for(size_t zi=0;zi<zs.size();++zi){
        const ProbeZ& z=zs[zi];std::string cn;objName(z.cls,cn);
        snprintf(m,sizeof(m),"Aimbot probe: zombie #%d dist %d at %d %d %d class %s",(int)zi,(int)z.d,(int)z.p[0],(int)z.p[1],(int)z.p[2],cn.c_str());logMessage(m);
        plog("Aimbot probe: zombie #"+std::to_string(zi)+" chain: "+chainStr(z.cls,6));
        std::string l="Aimbot probe: zombie #"+std::to_string(zi)+" props:";
        for(const auto& pr:stateP)l+=" "+pr.first+"="+rawHex(z.obj+(uintptr_t)pr.second,4);
        plog(l);
        l="Aimbot probe: zombie #"+std::to_string(zi)+" getters:";
        for(uintptr_t fn:stateF){unsigned char o[8];std::string n;objName(fn,n);l+=" "+n+"="+(callGetter(z.obj,fn,o)?hex8(o):std::string("err"));}
        plog(l);
    }
    // head bone: read from the first two zombies' skeletal mesh
    uintptr_t cdoStr=0;{auto it=hits.find("Default__KismetStringLibrary");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls!="Function"){cdoStr=h.obj;break;}}
    Fn fN2S;{auto it=hits.find("Conv_NameToString");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="Function"&&readFn(fN2S,h.obj,{"InName","ReturnValue"}))break;}
    std::array<unsigned char,8> headName{};bool haveHead=false;
    for(size_t zi=0;zi<zs.size()&&zi<3;++zi){
        const ProbeZ& z=zs[zi];
        const int mo=propOffset(z.cls,"Mesh");uintptr_t mesh=0;if(mo>=0)rd(z.obj+(uintptr_t)mo,mesh);
        if(!mesh){snprintf(m,sizeof(m),"Aimbot probe: zombie #%d has no readable Mesh property (offset %d)",(int)zi,mo);logMessage(m);continue;}
        uintptr_t mcls=0;rd(mesh+O_CLASS,mcls);
        plog("Aimbot probe: zombie #"+std::to_string(zi)+" mesh chain: "+chainStr(mcls,6));
        const uintptr_t fBoneName=chainFindFn(mcls,"GetBoneName"),fBoneLoc=chainFindFn(mcls,"GetBoneLocation"),fSockLoc=chainFindFn(mcls,"GetSocketLocation"),fCompLoc=chainFindFn(mcls,"K2_GetComponentLocation");
        plog("Aimbot probe: mesh fns: GetBoneName="+(fBoneName?fnSig(fBoneName):std::string("MISSING"))+"; GetBoneLocation="+(fBoneLoc?fnSig(fBoneLoc):std::string("MISSING"))+"; GetSocketLocation="+(fSockLoc?fnSig(fSockLoc):std::string("MISSING"))+"; K2_GetComponentLocation="+(fCompLoc?fnSig(fCompLoc):std::string("MISSING")));
        if(!fBoneName||!cdoStr||!fN2S.ptr){logMessage("Aimbot probe: cannot list bones (GetBoneName or Conv_NameToString missing)");continue;}
        Fn fBn;readFn(fBn,fBoneName,{});
        std::vector<std::pair<std::string,std::array<unsigned char,8>>> bones;std::string all;int blanks=0;
        for(int bi=0;bi<400&&blanks<3;++bi){
            Buf b(fBn.size);b.put(fBn,"BoneIndex",bi);
            if(!callPE(processEvent,mesh,fBn.ptr,b.p))break;
            std::array<unsigned char,8> nm{};if(!b.get(fBn,"ReturnValue",nm))break;
            const std::string s2=fnameToStr(cdoStr,fN2S,nm.data());
            if(s2.empty()||s2=="None"){++blanks;continue;}
            blanks=0;bones.push_back({s2,nm});all+=std::to_string(bi)+":"+s2+" ";
        }
        snprintf(m,sizeof(m),"Aimbot probe: zombie #%d skeleton has %d bones",(int)zi,(int)bones.size());logMessage(m);
        plog("Aimbot probe: bones: "+all);
        float cl[3]{};
        if(fCompLoc){Fn f;if(readFn(f,fCompLoc,{})){Buf b(f.size);if(callPE(processEvent,mesh,f.ptr,b.p))b.get(f,"ReturnValue",cl);}}
        snprintf(m,sizeof(m),"Aimbot probe: zombie #%d actor %d %d %d, mesh component %d %d %d",(int)zi,(int)z.p[0],(int)z.p[1],(int)z.p[2],(int)cl[0],(int)cl[1],(int)cl[2]);logMessage(m);
        for(const auto& bn:bones){
            if(bn.first=="Head"){headName=bn.second;haveHead=true;}
            const std::string l=lowerStr(bn.first);
            if(l.find("head")==std::string::npos&&l!="neck"&&l.find("neck")==std::string::npos)continue;
            float bl[3]{},sl[3]{};bool g1=false,g2=false;
            if(fBoneLoc){Fn f;if(readFn(f,fBoneLoc,{})){Buf b(f.size);b.putBytes(f,"BoneName",bn.second.data(),8);b.put(f,"Space",(unsigned char)0);if(callPE(processEvent,mesh,f.ptr,b.p))g1=b.get(f,"ReturnValue",bl);}}
            if(fSockLoc){Fn f;if(readFn(f,fSockLoc,{})){Buf b(f.size);b.putBytes(f,"InSocketName",bn.second.data(),8);if(callPE(processEvent,mesh,f.ptr,b.p))g2=b.get(f,"ReturnValue",sl);}}
            snprintf(m,sizeof(m),"Aimbot probe: zombie #%d bone '%s': GetBoneLocation %s %d %d %d | GetSocketLocation %s %d %d %d",(int)zi,bn.first.c_str(),g1?"ok":"fail",(int)bl[0],(int)bl[1],(int)bl[2],g2?"ok":"fail",(int)sl[0],(int)sl[1],(int)sl[2]);logMessage(m);
        }
    }
    if(haveHead){
        for(size_t zi=0;zi<zs.size();++zi){
            const ProbeZ& z=zs[zi];const int mo=propOffset(z.cls,"Mesh");uintptr_t mesh=0;if(mo>=0)rd(z.obj+(uintptr_t)mo,mesh);
            uintptr_t mcls=0;if(mesh)rd(mesh+O_CLASS,mcls);
            const uintptr_t fSock=mcls?chainFindFn(mcls,"GetSocketLocation"):0;
            float hp[3]{};bool g=false;
            if(fSock){Fn f;if(readFn(f,fSock,{})){Buf b(f.size);b.putBytes(f,"InSocketName",headName.data(),8);if(callPE(processEvent,mesh,f.ptr,b.p))g=b.get(f,"ReturnValue",hp);}}
            std::string cn;objName(z.cls,cn);
            snprintf(m,sizeof(m),"Aimbot probe: zombie #%d %s dist %d root %d %d %d head %s %d %d %d (head-root dz %d)",(int)zi,cn.c_str(),(int)z.d,(int)z.p[0],(int)z.p[1],(int)z.p[2],g?"ok":"fail",(int)hp[0],(int)hp[1],(int)hp[2],(int)(hp[2]-z.p[2]));logMessage(m);
        }
    }else logMessage("Aimbot probe: no skeleton with a 'Head' bone was found among the nearest zombies");
    logMessage("===== Aimbot probe: static part done; now sampling aim state once a second =====");
    return L"Probe running: close the menu, aim at zombies, release";
}
// ===== Aimbot: while aiming, steers the view onto the nearest living zombie's head (single player). =====
// The view is turned with the game's own AddYawInput / AddPitchInput (what the mouse uses): measured 2.5 degrees per unit of input.
std::atomic<bool> g_aimPending{false},g_keepPending{false};
struct AimCache{
    uintptr_t pcCls=0,pawnCls=0,camCls=0,cdoStr=0;
    Fn fPC,fYaw,fPitch,fCamLoc,fN2S;uintptr_t fEyes=0;
    bool resolved=false;ULONGLONG lastResolveTry=0;
    int rotOff=-1,aimOff=-1,camOff=-1;
    std::unordered_map<uintptr_t,uintptr_t> aliveFn,delayFn,sockFn;
    std::unordered_map<uintptr_t,int> meshOff;
    std::array<unsigned char,8> headName{};bool haveHead=false;ULONGLONG lastHeadTry=0;
    uintptr_t target=0;int targetIdx=-1;
    std::vector<ProbeZ> cands;ULONGLONG lastRefresh=0;
    float pitchSign=-1.f;int badPitch=0;float lastPitchErr=0,lastPitchCmd=0;
    ULONGLONG lastCmd=0;int idle=0;int logs=0;int timings=0;bool wasAiming=false;
    uintptr_t pawnPtr=0,pawnPtrCls=0;ULONGLONG pawnTime=0;      // cached player pawn for the cheap "not aiming" path
    Fn fTrace;int losProbes=0;bool traceObjs=false;uintptr_t ksl=0;int hrOff=24;bool traceTried=false;int losLogs=0;
    struct LosEntry{ULONGLONG t=0,lastClear=0;bool clear=true;};
    std::unordered_map<uintptr_t,LosEntry> los;
    std::unordered_map<uintptr_t,int> aimOffByCls;               // pawn class -> offset of IsAiming (-1 = none, e.g. vehicles)
};
static AimCache g_ac;
static bool aimSlotOk(const ProbeZ& z){
    uintptr_t cur=0,cls=0;int fl=0,ifl=0;
    if(z.idx<0||z.idx>=numObjs||!rd(objs+(size_t)z.idx*ITEM_SIZE,cur)||cur!=z.obj)return false;
    if(!rd(z.obj+O_CLASS,cls)||cls!=z.cls)return false;
    if(!rd(z.obj+8,fl)||(fl&0x20000000))return false;
    if(rd(objs+(size_t)z.idx*ITEM_SIZE+8,ifl)&&(ifl&0x20000000))return false;
    return true;
}
static float norm180(float a){while(a>180.f)a-=360.f;while(a<=-180.f)a+=360.f;return a;}
static void aimLog(const char* msg){if(g_ac.logs<60){++g_ac.logs;logMessage(msg);}}
static void aimResolve(AimCache& g,uintptr_t pcCls,uintptr_t pawnCls,uintptr_t pc){
    std::unordered_set<std::string> w={"GetPlayerController","Default__KismetStringLibrary","Conv_NameToString","GetCameraLocation","LineTraceSingle","LineTraceSingleForObjects","Default__KismetSystemLibrary","HitResult"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    uintptr_t pcFn=0;
    if(findHit(hits,"GetPlayerController","Function","GameplayStatics",pcFn))readFn(g.fPC,pcFn,{"WorldContextObject","ReturnValue"});
    {auto it=hits.find("Default__KismetStringLibrary");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls!="Function"){g.cdoStr=h.obj;break;}}
    {auto it=hits.find("Conv_NameToString");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="Function"&&readFn(g.fN2S,h.obj,{"InName","ReturnValue"}))break;}
    {   // line-of-sight trace (the same engine call the teleport uses to find the ground)
        uintptr_t tFn=0,oFn=0;
        {auto it=hits.find("Default__KismetSystemLibrary");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="KismetSystemLibrary"){g.ksl=h.obj;break;}}
        const bool haveO=g.ksl&&findHit(hits,"LineTraceSingleForObjects","Function","KismetSystemLibrary",oFn);
        const bool haveS=g.ksl&&findHit(hits,"LineTraceSingle","Function","KismetSystemLibrary",tFn);
        // Preferred: trace against static/dynamic WORLD objects only (walls, buildings, terrain) so zombies and the player never block the ray.
        if(haveO&&readFn(g.fTrace,oFn,{"WorldContextObject","Start","End","ObjectTypes","OutHit"}))g.traceObjs=true;
        else if(haveS&&readFn(g.fTrace,tFn,{"WorldContextObject","Start","End","OutHit"}))g.traceObjs=false;
        else g.fTrace=Fn();
        {char lm[200];snprintf(lm,sizeof(lm),"Aimbot: trace lookup: ksl %d, LineTraceSingleForObjects %d, LineTraceSingle %d",g.ksl?1:0,haveO?1:0,haveS?1:0);logMessage(lm);
         if(g.fTrace.ptr){std::string pl="Aimbot: trace params:";for(const auto& kv:g.fTrace.off)pl+=" "+kv.first+"@"+std::to_string(kv.second);logMessage(pl.c_str());}}
        {auto it=hits.find("HitResult");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="ScriptStruct"){const int o=propOffset(h.obj,"ImpactPoint");if(o>=0)g.hrOff=o;break;}}
        char lm[160];snprintf(lm,sizeof(lm),"Aimbot: line-of-sight trace %s (ImpactPoint@%d)",(g.ksl&&g.fTrace.ptr)?"ready":"UNAVAILABLE",g.hrOff);logMessage(lm);
    }
    g.pcCls=pcCls;g.pawnCls=pawnCls;
    g.rotOff=propOffset(pcCls,"ControlRotation");g.aimOff=propOffset(pawnCls,"IsAiming");g.camOff=propOffset(pcCls,"PlayerCameraManager");
    const uintptr_t fy=chainFindFn(pcCls,"AddYawInput"),fp=chainFindFn(pcCls,"AddPitchInput");
    if(fy)readFn(g.fYaw,fy,{"Val"});if(fp)readFn(g.fPitch,fp,{"Val"});
    uintptr_t cam=0;if(g.camOff>=0)rd(pc+(uintptr_t)g.camOff,cam);
    if(cam){rd(cam+O_CLASS,g.camCls);const uintptr_t fc=chainFindFn(g.camCls,"GetCameraLocation");if(fc)readFn(g.fCamLoc,fc,{});}
    g.fEyes=chainFindFn(pawnCls,"GetActorEyesViewPoint");
    g.resolved=g.fPC.ptr&&g.fYaw.ptr&&g.fPitch.ptr&&g.rotOff>=0&&g.aimOff>=0;
    char m[300];snprintf(m,sizeof(m),"Aimbot: resolved=%d (GetPlayerController %d, AddYawInput %d, AddPitchInput %d, ControlRotation@%d, IsAiming@%d, camera location fn %d, eyes fn %d)",
        g.resolved?1:0,g.fPC.ptr?1:0,g.fYaw.ptr?1:0,g.fPitch.ptr?1:0,g.rotOff,g.aimOff,g.fCamLoc.ptr?1:0,g.fEyes?1:0);logMessage(m);
}
// Finds the world position of a zombie's head (Head bone via the mesh socket); falls back to root + 70.
static bool aimHeadPos(AimCache& g,const ProbeZ& z,const float root[3],float out[3]){
    int mo;{auto it=g.meshOff.find(z.cls);if(it==g.meshOff.end())it=g.meshOff.emplace(z.cls,propOffset(z.cls,"Mesh")).first;mo=it->second;}
    uintptr_t mesh=0;if(mo>=0)rd(z.obj+(uintptr_t)mo,mesh);
    if(mesh){
        uintptr_t mcls=0;rd(mesh+O_CLASS,mcls);
        if(!g.haveHead&&g.cdoStr&&g.fN2S.ptr&&GetTickCount64()-g.lastHeadTry>2000){   // learn the FName "Head" once, from this zombie's skeleton
            g.lastHeadTry=GetTickCount64();
            const uintptr_t fBn=chainFindFn(mcls,"GetBoneName");Fn fb;
            if(fBn&&readFn(fb,fBn,{})){
                for(int bi=0;bi<80&&!g.haveHead;++bi){
                    Buf b(fb.size);b.put(fb,"BoneIndex",bi);std::array<unsigned char,8> nm{};
                    if(!callPE(processEvent,mesh,fb.ptr,b.p)||!b.get(fb,"ReturnValue",nm))break;
                    if(fnameToStr(g.cdoStr,g.fN2S,nm.data())=="Head"){g.headName=nm;g.haveHead=true;aimLog("Aimbot: head bone name learned");}
                }
            }
        }
        if(g.haveHead){
            uintptr_t fs=0;{auto it=g.sockFn.find(mcls);if(it==g.sockFn.end())it=g.sockFn.emplace(mcls,chainFindFn(mcls,"GetSocketLocation")).first;fs=it->second;}
            Fn f;
            if(fs&&readFn(f,fs,{})){
                Buf b(f.size);b.putBytes(f,"InSocketName",g.headName.data(),8);
                float hp[3]{};
                if(callPE(processEvent,mesh,f.ptr,b.p)&&b.get(f,"ReturnValue",hp)&&fabsf(hp[0]-root[0])<400.f&&fabsf(hp[1]-root[1])<400.f&&fabsf(hp[2]-root[2])<400.f){memcpy(out,hp,12);return true;}
            }
        }
    }
    out[0]=root[0];out[1]=root[1];out[2]=root[2]+70.f;return true;
}
// Line of sight: one LineTraceSingle from the player's eyes to the zombie's head. A hit well before the head means something solid is in the way.
// Fails open (counts as clear) if the trace cannot be run, so the aimbot never gets worse than it was without the option.
// One trace against the given EObjectTypeQuery values. Returns false if the call itself failed.
static bool aimTraceOnce(AimCache& g,uintptr_t world,uintptr_t ignoreActor,const float st[3],const float en[3],const unsigned char* types,int ntypes,bool& hit,unsigned char& flags,float ip[3]){
    Buf b(g.fTrace.size);
    b.put(g.fTrace,"WorldContextObject",world);b.putBytes(g.fTrace,"Start",st,12);b.putBytes(g.fTrace,"End",en,12);
    if(g.traceObjs){struct TArr{void* data;int num,max;}ar{(void*)types,ntypes,ntypes};b.putBytes(g.fTrace,"ObjectTypes",&ar,sizeof(ar));}
    else b.put(g.fTrace,"TraceChannel",(unsigned char)0);
    uintptr_t ign[1]={ignoreActor};
    if(ignoreActor){struct TArr{void* data;int num,max;}ai{ign,1,1};b.putBytes(g.fTrace,"ActorsToIgnore",&ai,sizeof(ai));}
    b.put(g.fTrace,"bTraceComplex",(unsigned char)0);b.put(g.fTrace,"bIgnoreSelf",(unsigned char)1);
    if(!callPE(processEvent,g.ksl,g.fTrace.ptr,b.p))return false;
    unsigned char r=0;b.get(g.fTrace,"ReturnValue",r);hit=(r&1)!=0;flags=0;ip[0]=ip[1]=ip[2]=0;
    auto oi=g.fTrace.off.find("OutHit");
    if(hit&&oi!=g.fTrace.off.end()&&(size_t)oi->second+g.hrOff+12<=b.n){flags=*((unsigned char*)b.p+oi->second);memcpy(ip,(char*)b.p+oi->second+g.hrOff,12);}
    return true;
}
// Line of sight: a trace from the player's eyes to the zombie's head against WORLD STATIC objects only (rocks, buildings, terrain).
// Fails open (counts as clear) if the trace cannot be run, so the aimbot never gets worse than it was without the option.
static bool aimLosClear(AimCache& g,uintptr_t world,uintptr_t ignoreActor,const float from[3],const float head[3],bool dbg){
    if(!g.ksl||!g.fTrace.ptr)return true;
    const float dx=head[0]-from[0],dy=head[1]-from[1],dz=head[2]-from[2];
    const float total=sqrtf(dx*dx+dy*dy+dz*dz);if(total<150.f)return true;
    char m[260];
    static const unsigned char worldStatic[1]={0};
    bool hit=false;unsigned char fl=0;float ip[3]{};
    if(!aimTraceOnce(g,world,ignoreActor,from,head,worldStatic,1,hit,fl,ip))return true;
    if(!hit){if(dbg&&g.losLogs<30){++g.losLogs;snprintf(m,sizeof(m),"Aimbot LOS: no hit of %d -> clear",(int)total);logMessage(m);}return true;}
    const float ox=ip[0]-from[0],oy=ip[1]-from[1],oz=ip[2]-from[2];
    const float hd=sqrtf(ox*ox+oy*oy+oz*oz);
    if(fl&2){   // the ray starts inside a static object: cannot judge
        if(dbg&&g.losLogs<30){++g.losLogs;snprintf(m,sizeof(m),"Aimbot LOS: start inside a static object (flags %02X) -> clear",(int)fl);logMessage(m);}
        return true;
    }
    const bool blocked=hd<total-110.f;
    if(dbg&&g.losLogs<30){++g.losLogs;snprintf(m,sizeof(m),"Aimbot LOS: hit at %d of %d (flags %02X) -> %s",(int)hd,(int)total,(int)fl,blocked?"blocked":"clear");logMessage(m);}
    return !blocked;
}
void aimTick(){
    AimCache& g=g_ac;
    const ULONGLONG t0=GetTickCount64();
    // Cheap path while NOT aiming: the player pawn is cached (re-fetched twice a second) and IsAiming is a plain memory read,
    // so no engine call, scan or log happens. Pawns without an aim state (vehicles) stop here silently.
    uintptr_t pawn=g.pawnPtr,pawnCls=0,world=0;
    if(!(pawn&&rd(pawn+O_CLASS,pawnCls)&&pawnCls==g.pawnPtrCls&&t0-g.pawnTime<500)){
        std::string why0;if(!init(why0))return;
        if(!rd(base+RVA_GWORLD,world)||!world)return;
        pawn=playerPawn(world);g.pawnTime=t0;g.pawnPtr=pawn;g.pawnPtrCls=0;
        if(!pawn||!rd(pawn+O_CLASS,pawnCls)||!pawnCls){g.pawnPtr=0;return;}
        g.pawnPtrCls=pawnCls;
    }
    int aimOff;{auto it=g.aimOffByCls.find(pawnCls);if(it==g.aimOffByCls.end())it=g.aimOffByCls.emplace(pawnCls,propOffset(pawnCls,"IsAiming")).first;aimOff=it->second;}
    if(aimOff<0){if(g.wasAiming){g.wasAiming=false;g.target=0;g.targetIdx=-1;}return;}
    unsigned char aimByte=0;safeRead(pawn+(uintptr_t)aimOff,&aimByte,1);
    if(!(aimByte&1)){
        if(g.wasAiming){g.wasAiming=false;g.target=0;g.targetIdx=-1;}
        return;
    }
    // Aiming: from here on it is the same as before. Use a freshly fetched pawn rather than the cached one.
    std::string why;if(!init(why))return;
    if(!world&&(!rd(base+RVA_GWORLD,world)||!world))return;
    pawn=playerPawn(world);if(!pawn)return;
    {uintptr_t pc1=0;if(!rd(pawn+O_CLASS,pc1)||pc1!=pawnCls)return;}
    if(!g.resolved){
        if(GetTickCount64()-g.lastResolveTry<3000)return;
        g.lastResolveTry=GetTickCount64();
        Buf b(8);uintptr_t pc0=0;   // need a controller to resolve; use a throw-away lookup through the cached function if present
        if(g.fPC.ptr){Buf bb(g.fPC.size);bb.put(g.fPC,"WorldContextObject",world);bb.put(g.fPC,"PlayerIndex",(int)0);if(callPE(processEvent,cdoStatics,g.fPC.ptr,bb.p))bb.get(g.fPC,"ReturnValue",pc0);}
        if(!pc0){   // first time: resolve GetPlayerController through a scan
            std::unordered_set<std::string> w={"GetPlayerController"};int ok=0,t=0;auto hits=scan(w,ok,t);uintptr_t pcFn=0;
            if(findHit(hits,"GetPlayerController","Function","GameplayStatics",pcFn)&&readFn(g.fPC,pcFn,{"WorldContextObject","ReturnValue"})){
                Buf bb(g.fPC.size);bb.put(g.fPC,"WorldContextObject",world);bb.put(g.fPC,"PlayerIndex",(int)0);if(callPE(processEvent,cdoStatics,g.fPC.ptr,bb.p))bb.get(g.fPC,"ReturnValue",pc0);}
        }
        uintptr_t pcCls0=0;if(pc0)rd(pc0+O_CLASS,pcCls0);
        if(!pc0||!pcCls0)return;
        aimResolve(g,pcCls0,pawnCls,pc0);
        if(!g.resolved)return;
    }
    if(g.pawnCls!=pawnCls){g.pawnCls=pawnCls;g.fEyes=chainFindFn(pawnCls,"GetActorEyesViewPoint");}
    // aiming: controller, view state
    uintptr_t pc=0;
    {Buf bb(g.fPC.size);bb.put(g.fPC,"WorldContextObject",world);bb.put(g.fPC,"PlayerIndex",(int)0);if(callPE(processEvent,cdoStatics,g.fPC.ptr,bb.p))bb.get(g.fPC,"ReturnValue",pc);}
    uintptr_t cc=0;if(!pc||!rd(pc+O_CLASS,cc)||cc!=g.pcCls)return;
    g.wasAiming=true;
    float plRoot[3]{};if(!rootPos(pawn,pawnCls,plRoot))return;
    float rot[3]{};if(!safeRead(pc+(uintptr_t)g.rotOff,rot,12))return;
    // camera position (where the crosshair ray starts)
    float cam[3]={plRoot[0],plRoot[1],plRoot[2]+60.f};
    {bool got=false;
     if(g.fCamLoc.ptr&&g.camOff>=0){uintptr_t co=0;rd(pc+(uintptr_t)g.camOff,co);
        if(co){Buf b(g.fCamLoc.size);float v[3]{};if(callPE(processEvent,co,g.fCamLoc.ptr,b.p)&&b.get(g.fCamLoc,"ReturnValue",v)&&fabsf(v[0]-plRoot[0])<2000.f&&fabsf(v[1]-plRoot[1])<2000.f){memcpy(cam,v,12);got=true;}}}
     if(!got&&g.fEyes){Fn f;if(readFn(f,g.fEyes,{})){Buf b(f.size);float el[3]{};if(callPE(processEvent,pawn,f.ptr,b.p)&&b.get(f,"OutLocation",el)){memcpy(cam,el,12);}}}}
    const ULONGLONG now=GetTickCount64();
    float eye[3]={plRoot[0],plRoot[1],plRoot[2]+60.f};   // where the line-of-sight ray starts (the player's eyes, not the camera behind the shoulder)
    if(g.fEyes){Fn f;if(readFn(f,g.fEyes,{})){Buf b(f.size);float el[3]{};if(callPE(processEvent,pawn,f.ptr,b.p)&&b.get(f,"OutLocation",el)&&fabsf(el[0]-plRoot[0])<500.f&&fabsf(el[1]-plRoot[1])<500.f&&fabsf(el[2]-plRoot[2])<500.f)memcpy(eye,el,12);}}
    if(g.los.size()>200)g.los.clear();
    const float RANGE=5000.f,KEEP=RANGE*1.12f;   // 50 m
    // refresh the candidate list (slow scan) at most every 1.5 s while aiming
    if(now-g.lastRefresh>1500||(g.cands.empty()&&now-g.lastRefresh>500)){
        const ULONGLONG t0=GetTickCount64();
        g.cands=collectZombies(plRoot,RANGE*1.4f,false,48);g.lastRefresh=now;
        if(g.timings<4){++g.timings;char m[200];snprintf(m,sizeof(m),"Aimbot: zombie scan took %d ms, %d candidates within %.0f",(int)(GetTickCount64()-t0),(int)g.cands.size(),RANGE*1.4f);logMessage(m);}
    }
    auto eligible=[&](const ProbeZ& z,float lim,float pos[3],float& dist)->bool{
        if(!aimSlotOk(z)||!rootPos(z.obj,z.cls,pos))return false;
        const float dx=pos[0]-plRoot[0],dy=pos[1]-plRoot[1],dz=pos[2]-plRoot[2];dist=sqrtf(dx*dx+dy*dy+dz*dz);
        if(dist>lim)return false;
        uintptr_t fa=0,fd=0;
        {auto it=g.aliveFn.find(z.cls);if(it==g.aliveFn.end())it=g.aliveFn.emplace(z.cls,chainFindFn(z.cls,"IsAlive")).first;fa=it->second;}
        {auto it=g.delayFn.find(z.cls);if(it==g.delayFn.end())it=g.delayFn.emplace(z.cls,chainFindFn(z.cls,"GetIsInDelayedDeath")).first;fd=it->second;}
        unsigned char o[8]{};
        if(!fa||!callGetter(z.obj,fa,o)||!(o[0]&1))return false;     // dead (a knocked-down zombie is still alive)
        if(fd&&callGetter(z.obj,fd,o)&&(o[0]&1))return false;        // dying
        {   // line of sight is part of the Aimbot toggle
            AimCache::LosEntry& e=g.los[z.obj];
            if(now-e.t>150||e.t==0){
                float hp[3]{};aimHeadPos(g,z,pos,hp);
                e.clear=aimLosClear(g,world,pawn,eye,hp,true);e.t=now;if(e.clear)e.lastClear=now;
            }
            if(!e.clear){
                const bool isCurrent=(z.obj==g.target);
                if(!(isCurrent&&now-e.lastClear<350))return false;   // the current target gets a short grace so a passing obstacle does not flick the aim away
            }
        }
        return true;
    };
    float tpos[3]{};float tdist=0;const ProbeZ* tz=nullptr;
    if(g.target){for(const ProbeZ& z:g.cands)if(z.obj==g.target){if(eligible(z,KEEP,tpos,tdist))tz=&z;break;}}
    if(!tz){
        g.target=0;g.targetIdx=-1;
        std::vector<std::pair<float,size_t>> order;
        for(size_t i=0;i<g.cands.size();++i){float p[3]{};if(rootPos(g.cands[i].obj,g.cands[i].cls,p)&&aimSlotOk(g.cands[i])){const float dx=p[0]-plRoot[0],dy=p[1]-plRoot[1],dz=p[2]-plRoot[2];order.push_back({dx*dx+dy*dy+dz*dz,i});}}
        std::sort(order.begin(),order.end());
        for(const auto& oi:order){
            if(oi.first>RANGE*RANGE)break;
            const ProbeZ& z=g.cands[oi.second];
            if(eligible(z,RANGE,tpos,tdist)){tz=&z;g.target=z.obj;g.targetIdx=z.idx;
                std::string cn;objName(z.cls,cn);char m[200];snprintf(m,sizeof(m),"Aimbot: target %s at %d",cn.c_str(),(int)tdist);aimLog(m);break;}
        }
    }
    if(!tz)return;   // nothing eligible: leave the camera alone
    float head[3]{};aimHeadPos(g,*tz,tpos,head);
    const float dx=head[0]-cam[0],dy=head[1]-cam[1],dz=head[2]-cam[2];
    const float hd=sqrtf(dx*dx+dy*dy);
    const float yawT=atan2f(dy,dx)*57.29578f,pitchT=atan2f(dz,hd)*57.29578f;
    const float dYaw=norm180(yawT-norm180(rot[1])),pitchErr=pitchT-norm180(rot[0]);
    // learn the pitch direction: if the error keeps growing after we pushed against it, flip the sign
    if(fabsf(g.lastPitchCmd)>0.3f){
        if(fabsf(pitchErr)>fabsf(g.lastPitchErr)+0.2f*fabsf(g.lastPitchCmd)){if(++g.badPitch>=4){g.pitchSign=-g.pitchSign;g.badPitch=0;aimLog("Aimbot: pitch direction flipped");}}
        else g.badPitch=0;
    }
    g.lastPitchErr=pitchErr;g.lastPitchCmd=0;
    if(fabsf(dYaw)<0.2f&&fabsf(pitchErr)<0.2f)return;
    if(now-g.lastCmd<14)return;   // at most about one command per frame so pending input is not double counted
    g.lastCmd=now;
    const float GAIN=0.45f,DEG_PER_UNIT=2.5f;
    float yawIn=GAIN*dYaw/DEG_PER_UNIT,pitchIn=g.pitchSign*GAIN*pitchErr/DEG_PER_UNIT;
    if(fabsf(dYaw)<0.2f)yawIn=0.f;if(fabsf(pitchErr)<0.2f)pitchIn=0.f;
    auto addIn=[&](const Fn& f,float v){if(v==0.f)return;Buf b(f.size);b.put(f,"Val",v);callPE(processEvent,pc,f.ptr,b.p);};
    addIn(g.fYaw,yawIn);addIn(g.fPitch,pitchIn);
    g.lastPitchCmd=pitchIn!=0.f?GAIN*pitchErr:0.f;
}
// ---- Hordes (Part 1): a loose group in front of the player ----
// Flow: pick the zombies -> ask the game to load any class not in memory -> wait until all are loaded (max 15 s)
// -> spawn the group in batches of 5 every ~120 ms so the game thread never has to do 50 spawns in one frame.
struct HordeJob{bool active=false;int stage=0;std::vector<int> plan;size_t next=0;ULONGLONG t0=0,lastCheck=0;int sizeIdx=0;int spawned=0,failed=0;
    float center[3]{},fwd[2]{1,0};float spread=600.f;float loud=1.f;uintptr_t lastActor=0;ULONGLONG t1=0;std::wstring summary;uintptr_t cls[zombieCount]{};bool clsOk=false;
    };
static HordeJob g_hj;
std::atomic<bool> g_hordeActive{false},g_hordePending{false};
static std::mt19937& hrng(){static std::mt19937 r((unsigned)GetTickCount64()^0x9E3779B9u);return r;}
static float hrand(){return std::uniform_real_distribution<float>(0.f,1.f)(hrng());}
static std::vector<int> hordePlan(int n,bool juggernaut){
    // idx into zombieDefs: 0 Fast, 1 Slow, 12 Armored, 3 Plague Zombie (common); 8 Feral, 11 Screamer, 2 Bloater (rare, capped); 4 Juggernaut (massive only)
    struct W{int idx,w;bool rare;};
    const W pool[]={{0,40,false},{1,8,false},{12,25,false},{3,22,false},{8,2,true},{11,2,true},{2,1,true}};
    const int rareCap=n/12+1;int rareCount[14]{};
    std::vector<int> out;const int picks=juggernaut?n-1:n;
    while((int)out.size()<picks){
        int total=0;for(const W& p:pool)total+=p.w;
        int r=(int)(hrand()*total);if(r>=total)r=total-1;
        const W* ch=&pool[0];for(const W& p:pool){if(r<p.w){ch=&p;break;}r-=p.w;}
        if(ch->rare){if(rareCount[ch->idx]>=rareCap)continue;rareCount[ch->idx]++;}
        out.push_back(ch->idx);
    }
    if(juggernaut)out.push_back(4);
    std::shuffle(out.begin(),out.end(),hrng());
    return out;
}
static const char* const kwAggro[]={"noise","alert","aggro","notice","sense","target","investigat","wake","agitat","detect","perceiv","hear","attention","aware","chase","hunt","stimul"};
// After a horde lands the zombies just stand there. Experiment: make the player "make a noise" (the engine's own MakeNoise),
// and (once per session) write the zombie's AI-related functions/properties to the log so a real aggro call can be chosen.
static void hordeAggro(uintptr_t world,uintptr_t zombie,float loud,float range=8000.f){
    const uintptr_t pawn=playerPawn(world);
    static bool dumped=true;   // the AI dump was captured in v90; set false to log it again
    if(!dumped&&zombie){
        dumped=true;
        uintptr_t zc=0;rd(zombie+O_CLASS,zc);
        if(zc){
            plog("Horde AI: zombie class chain: "+chainStr(zc,8));
            std::string l="Horde AI: zombie functions: ";for(uintptr_t fn:chainFns(zc,kwAggro,17,false))l+=fnSig(fn)+"; ";plog(l);
            l="Horde AI: zombie properties: ";for(const auto& pr:chainProps(zc,kwAggro,17))l+=pr.first+"@"+std::to_string(pr.second)+" ";plog(l);
            static const char* const kwCtl[]={"controller","brain","perception","blackboard","behavior"};
            l="Horde AI: zombie controller/brain properties: ";for(const auto& pr:chainProps(zc,kwCtl,5))l+=pr.first+"@"+std::to_string(pr.second)+" ";plog(l);
            const int co=propOffset(zc,"Controller");uintptr_t ctl=0;
            if(co>=0&&rd(zombie+(uintptr_t)co,ctl)&&ctl){
                uintptr_t cc=0;rd(ctl+O_CLASS,cc);
                if(cc){
                    plog("Horde AI: controller class chain: "+chainStr(cc,8));
                    l="Horde AI: controller functions: ";for(uintptr_t fn:chainFns(cc,kwAggro,17,false))l+=fnSig(fn)+"; ";plog(l);
                    l="Horde AI: controller properties: ";for(const auto& pr:chainProps(cc,kwAggro,17))l+=pr.first+"@"+std::to_string(pr.second)+" ";plog(l);
                }
            }else logMessage("Horde AI: zombie has no readable Controller");
        }
    }
    if(!pawn){logMessage("Horde AI: no player pawn for noise");return;}
    uintptr_t pc=0;rd(pawn+O_CLASS,pc);
    const uintptr_t fn=pc?chainFindFn(pc,"MakeNoise"):0;
    Fn f;if(!fn||!readFn(f,fn,{})){logMessage("Horde AI: MakeNoise not found on the player");return;}
    float loc[3]{};
    {Buf b(fLoc.size);if(!callPE(processEvent,pawn,fLoc.ptr,b.p)||!b.get(fLoc,"ReturnValue",loc)){logMessage("Horde AI: could not read player location");return;}}
    Buf b(f.size);
        b.put(f,"Loudness",loud);b.put(f,"NoiseInstigator",pawn);b.putBytes(f,"NoiseLocation",loc,12);b.put(f,"MaxRange",range);
    const bool ok=callPE(processEvent,pawn,f.ptr,b.p);
    {char m[120];snprintf(m,sizeof(m),"Horde AI: MakeNoise sent (%s) loudness %.1f",ok?"ok":"faulted",loud);logMessage(m);}
}

// ---- Keep-ticker: Infinite Durability (freeze), Infinite Ammo (probe), Vehicle Health Lock, Fuel Lock reset ----------------
// Durability: instead of putting durability back, the wear itself is switched off: the weapon data's
// DurabilityLossPerShot / PerHit / PerFinisher Min+Max values are set to 0 while the toggle is on (originals are restored when off).
static bool keepFn(uintptr_t cls,const char* name,Fn& out){
    static std::map<std::pair<uintptr_t,std::string>,std::pair<bool,Fn>> cache;
    const auto key=std::make_pair(cls,std::string(name));
    auto it=cache.find(key);
    if(it==cache.end()){Fn f;const uintptr_t fn=chainFindFn(cls,name);const bool ok=fn&&readFn(f,fn,{});it=cache.emplace(key,std::make_pair(ok,f)).first;}
    if(!it->second.first)return false;
    out=it->second.second;return true;
}
static bool keepBoolMask(uintptr_t prop,unsigned char& mask){
    unsigned char pb[0x60]{};for(int i=0;i<0x60;++i)rd(prop+0x58+(uintptr_t)i,pb[i]);
    for(int i=0;i+3<0x60;++i)if(pb[i]==1&&pb[i+1]==0&&pb[i+2]!=0&&pb[i+3]!=0){mask=pb[i+3];return true;}
    return false;
}
struct KeepVeh{uintptr_t obj=0;int idx=0;unsigned char orig=0;};
static void keepTick(){
    static ULONGLONG lastSlow=0;static bool vehWas=false,fuelWas=false;
    static std::vector<KeepVeh> locked;
    std::string why;if(!init(why))return;
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return;
    const uintptr_t pawn=playerPawn(world);
    const bool vehOn=vehicleHealthLock.load(),fuelOn=infiniteCarFuel.load();
    const ULONGLONG now=GetTickCount64();
    const bool slow=now-lastSlow>=250;if(slow)lastSlow=now;
    // ---- fuel lock: forget the frozen value whenever the toggle changes ----
    if(fuelOn!=fuelWas){fuelLatchObj=0;fuelWas=fuelOn;}
    // ---- infinite ammo: resolve the AmmoItem class for the reserve hook, report what it did ----
    if(infiniteAmmo.load()){
        if(!ammoItemClassPtr){
            std::unordered_set<std::string> w={"AmmoItem"};int ok=0,t=0;auto hits=scan(w,ok,t);
            auto it=hits.find("AmmoItem");
            if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="Class"){ammoItemClassPtr=h.obj;break;}
            logMessage(ammoItemClassPtr?"Infinite Ammo: AmmoItem class resolved":"Infinite Ammo: AmmoItem class not found - reserve ammo will behave normally");
        }
        static unsigned seenKept=0;static uintptr_t seenOther=0;
        if(ammoKeptCount!=seenKept){seenKept=ammoKeptCount;char m[100];snprintf(m,sizeof(m),"Infinite Ammo: reserve kept on %u reloads so far",seenKept);logMessage(m);}
        if(ammoLastOther&&ammoLastOther!=seenOther){seenOther=ammoLastOther;std::string cn;objName(seenOther,cn);logMessage((std::string("Infinite Ammo: other item removal left alone: ")+cn).c_str());}
    }
    // ---- vehicle health lock ----
    if(vehOn&&slow&&pawn){
        std::string vw;
        if(initVehicle(vw)){
            uintptr_t pc=0,veh=0;rd(pawn+O_CLASS,pc);
            if(pc&&isA(pc,clsVehicle))veh=pawn;
            else{Buf b(fGetVeh.size);const auto gp=sortedParams(fGetVeh);
                if(!gp.empty()){b.put(fGetVeh,gp[0].first.c_str(),pawn);if(callPE(processEvent,cdoVehLib,fGetVeh.ptr,b.p))b.get(fGetVeh,"ReturnValue",veh);}
                if(veh){uintptr_t vc=0;if(!rd(veh+O_CLASS,vc)||!isA(vc,clsVehicle))veh=0;}}
            if(veh){
                static uintptr_t cachedCls=0;static int off=-1;static unsigned char mask=0;
                if(cachedCls!=clsVehicle){cachedCls=clsVehicle;off=-1;mask=0;
                    const uintptr_t prop=propObj(clsVehicle,"bDisableDamage");
                    if(prop&&rd(prop+O_PROPOFF,off)&&off>=0&&keepBoolMask(prop,mask)){char m[120];snprintf(m,sizeof(m),"Vehicle Health Lock: bDisableDamage at +0x%X mask 0x%02X",off,mask);logMessage(m);}
                    else{off=-1;logMessage("Vehicle Health Lock: bDisableDamage property not found");}}
                if(off>=0&&mask){
                    unsigned char cur=0;
                    if(rd(veh+(uintptr_t)off,cur)){
                        bool known=false;for(const KeepVeh& k:locked)if(k.obj==veh){known=true;break;}
                        if(!known){KeepVeh k;k.obj=veh;rd(veh+0xC,k.idx);k.orig=cur;locked.push_back(k);
                            char m[120];snprintf(m,sizeof(m),"Vehicle Health Lock: vehicle locked (flag byte %02X)",cur);logMessage(m);}
                        const unsigned char nb=mask==0xFF?1:(unsigned char)(cur|mask);
                        if(cur!=nb)writeByte(veh+(uintptr_t)off,nb);
                    }
                }
            }
        }
    }
    if(!vehOn&&vehWas){   // switched off: put the flag back on every vehicle we changed
        std::string vw;
        if(initVehicle(vw)){
            const uintptr_t prop=propObj(clsVehicle,"bDisableDamage");int off=-1;unsigned char mask=0;
            if(prop&&rd(prop+O_PROPOFF,off)&&off>=0&&keepBoolMask(prop,mask)){
                for(const KeepVeh& k:locked){
                    uintptr_t cur=0;if(!rd(objs+(size_t)k.idx*ITEM_SIZE,cur)||cur!=k.obj)continue;
                    unsigned char b=0;if(!rd(k.obj+(uintptr_t)off,b))continue;
                    const unsigned char nb=mask==0xFF?(unsigned char)(k.orig?1:0):(unsigned char)((b&~mask)|(k.orig&mask));
                    writeByte(k.obj+(uintptr_t)off,nb);
                }
            }
        }
        locked.clear();logMessage("Vehicle Health Lock: off, vehicle flags restored");
    }
    vehWas=vehOn;
}
std::wstring hordeStart(int sizeIdx){
    if(sizeIdx<0||sizeIdx>2)return L"Bad horde size";
    if(!unlimitedSpawn)return L"Turn on Unlimited spawning first (top of Hordes)";
    if(g_hj.active)return L"A horde is already being spawned";
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    static const int counts[3]={15,25,50};static const float dists[3]={1500.f,2500.f,3000.f};static const float spreads[3]={500.f,700.f,900.f};
    HordeJob j;j.active=true;j.sizeIdx=sizeIdx;j.t0=GetTickCount64();
    j.spread=spreads[sizeIdx];{static const float louds[3]={1.0f,1.5f,3.5f};j.loud=louds[sizeIdx];}
    j.plan=hordePlan(counts[sizeIdx],sizeIdx==2);
    j.center[0]=dists[sizeIdx];   // temporarily holds the distance; resolved against the player when spawning starts
    std::set<int> need(j.plan.begin(),j.plan.end());
    int started=0;
    for(int idx:need){const std::wstring r=loadZombieClass(idx);if(r!=L"ALREADY"){++started;logMessage((std::string("Horde: loading ")+zombieDefs[idx].cls).c_str());}}
    g_hj=j;g_hordeActive=true;g_hordePending=false;
    HWND gw=gameWindow;
    std::thread([gw]{while(g_hordeActive.load()){Sleep(120);if(g_hordeActive.load()&&gw&&!g_hordePending.exchange(true))PostMessageW(gw,WM_ADREENA_HORDE,10,0);}}).detach();
    char m[160];snprintf(m,sizeof(m),"Horde: started size %d (%d zombies, %d classes, %d need loading)",sizeIdx,(int)j.plan.size(),(int)need.size(),started);logMessage(m);
    return started?L"Loading zombie classes for the horde...":L"Spawning horde...";
}
static void hordeFinish(const wchar_t* why){g_hj.active=false;g_hordeActive=false;(void)why;}
std::wstring hordeTick(){
    HordeJob& j=g_hj;
    if(!j.active){g_hordeActive=false;return L"";}
    std::string why;if(!init(why)){hordeFinish(L"");return L"Unavailable: "+widen(why);}
    const ULONGLONG now=GetTickCount64();
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world){hordeFinish(L"");return L"Horde cancelled: no game world";}
    if(j.stage==0){
        if(now-j.lastCheck<500)return L"";
        j.lastCheck=now;
        unsigned char flags[zombieCount]{};computeLoadedZombies(flags);
        std::set<int> need(j.plan.begin(),j.plan.end());int have=0;
        for(int idx:need)if(flags[idx])++have;
        const bool timeout=now-j.t0>15000;
        if(have<(int)need.size()&&!timeout){
            wchar_t b[96];swprintf(b,96,L"Loading zombie classes... %d / %d",have,(int)need.size());return b;
        }
        // Anything that never loaded is replaced by a class that did.
        std::vector<int> loaded;for(int idx:{0,1,12,3})if(flags[idx])loaded.push_back(idx);
        if(loaded.empty()){hordeFinish(L"");logMessage("Horde: no common zombie class loaded, giving up");return L"Horde failed: zombie classes did not load";}
        int swapped=0;
        for(int& idx:j.plan)if(!flags[idx]){idx=loaded[(size_t)(hrand()*loaded.size())%loaded.size()];++swapped;}
        if(swapped){char m[100];snprintf(m,sizeof(m),"Horde: %d zombies swapped to loaded classes",swapped);logMessage(m);}
        for(int idx:std::set<int>(j.plan.begin(),j.plan.end()))j.cls[idx]=classFor(idx);
        // Player position and facing; the group's centre is fixed now so it does not drift while it streams in.
        const uintptr_t pawn=playerPawn(world);
        float loc[3]{},fw[3]{};bool okp=pawn!=0;
        if(okp){Buf b(fLoc.size);okp=callPE(processEvent,pawn,fLoc.ptr,b.p)&&b.get(fLoc,"ReturnValue",loc);}
        if(okp){Buf b(fFwd.size);okp=callPE(processEvent,pawn,fFwd.ptr,b.p)&&b.get(fFwd,"ReturnValue",fw);}
        if(!okp){hordeFinish(L"");return L"Horde failed: could not read player position";}
        float fl=sqrtf(fw[0]*fw[0]+fw[1]*fw[1]);if(!(fl>0.001f)){fw[0]=1;fw[1]=0;fl=1;}
        j.fwd[0]=fw[0]/fl;j.fwd[1]=fw[1]/fl;
        const float dist=j.center[0];
        j.center[0]=loc[0]+j.fwd[0]*dist;j.center[1]=loc[1]+j.fwd[1]*dist;j.center[2]=loc[2]+120.f;
        j.stage=1;
    }
    // stage 1: spawn up to 5 per tick
    int batch=0;
    while(j.stage==1&&batch<5&&j.next<j.plan.size()){
        const int idx=j.plan[j.next++];++batch;
        const uintptr_t cls=j.cls[idx];
        if(!cls){++j.failed;continue;}
        float px,py,yaw;
        const float r=j.spread*sqrtf(hrand()),th=hrand()*6.2831853f;
        const float ax=cosf(th)*r,ay=sinf(th)*r;   // along forward / sideways
        px=j.center[0]+j.fwd[0]*ax-j.fwd[1]*ay;py=j.center[1]+j.fwd[1]*ax+j.fwd[0]*ay;
        yaw=atan2f(-j.fwd[1],-j.fwd[0])+(hrand()-0.5f)*1.2f;   // roughly facing the player
        float xf[12]={0,0,sinf(yaw*0.5f),cosf(yaw*0.5f), px,py,j.center[2],0, 1,1,1,0};
        uintptr_t actor=0;
        {Buf b(fBegin.size);
         b.put(fBegin,"WorldContextObject",world);b.put(fBegin,"ActorClass",cls);b.putBytes(fBegin,"SpawnTransform",xf,sizeof(xf));
         b.put(fBegin,"CollisionHandlingOverride",(unsigned char)2);
         if(callPE(processEvent,cdoStatics,fBegin.ptr,b.p))b.get(fBegin,"ReturnValue",actor);}
        if(!actor){++j.failed;continue;}
        {Buf b(fFinish.size);b.put(fFinish,"Actor",actor);b.putBytes(fFinish,"SpawnTransform",xf,sizeof(xf));
         if(!callPE(processEvent,cdoStatics,fFinish.ptr,b.p)){++j.failed;continue;}}
        ++j.spawned;j.lastActor=actor;
    }
    if(j.stage==1&&j.next>=j.plan.size()){
        char m[160];snprintf(m,sizeof(m),"Horde: done, %d spawned, %d failed, took %d ms",j.spawned,j.failed,(int)(GetTickCount64()-j.t0));logMessage(m);
        wchar_t sb[120];
        if(j.failed)swprintf(sb,120,L"Horde spawned: %d of %d (%d failed)",j.spawned,(int)j.plan.size(),j.failed);else swprintf(sb,120,L"Horde spawned: %d zombies",j.spawned);
        j.summary=sb;j.stage=2;j.t1=GetTickCount64();
        return j.summary;
    }
    if(j.stage==2){
        if(GetTickCount64()-j.t1<800)return L"";
        hordeAggro(world,j.lastActor,j.loud,8000.f);
        j.stage=3;j.t1=GetTickCount64();
        return L"";
    }
    if(j.stage==3){   // second noise a little later, for zombies that were still waking up
        if(GetTickCount64()-j.t1<1500)return L"";
        hordeAggro(world,j.lastActor,j.loud,8000.f);
        const std::wstring sm=j.summary;
        hordeFinish(L"");
        return sm;
    }
    wchar_t b[96];swprintf(b,96,L"Spawning horde... %d / %d",j.spawned+j.failed,(int)j.plan.size());return b;
}
std::wstring hordeClear(){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    if(g_hj.active){hordeFinish(L"");logMessage("Horde: spawning cancelled by Clear");}
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn";
    uintptr_t pcls=0;float pl[3]{};
    if(!rd(pawn+O_CLASS,pcls)||!rootPos(pawn,pcls,pl))return L"Could not read player position";
    const auto zs=collectZombies(pl,10000.f,false,4000);
    if(zs.empty())return L"No zombies within 100 m";
    const uintptr_t fn=chainFindFn(zs.front().cls,"K2_DestroyActor");
    Fn f;if(!fn||!readFn(f,fn,{}))return L"Destroy function not found";
    int n=0;
    for(const ProbeZ& z:zs){Buf b(f.size);if(callPE(processEvent,z.obj,f.ptr,b.p))++n;}
    char m[100];snprintf(m,sizeof(m),"Horde: cleared %d of %d zombies within 100 m",n,(int)zs.size());logMessage(m);
    wchar_t b[110];swprintf(b,110,L"Cleared %d zombies within 100 m",n);return b;
}
// Give Item start-up scan: true once the game has finished loading its object table (count large and unchanged for 3 checks, one per second).
bool itemScanReady(){
    std::string why;if(!init(why))return false;
    refreshCount();
    static int last=-1,stable=0;
    if(numObjs==last)++stable;else{stable=0;last=numObjs;}
    return numObjs>300000&&stable>=3;
}
CommResult commAction(int act,int p,const std::wstring& s1=L"",const std::wstring& s2=L"",const std::wstring& s3=L""){
    CommResult r;
    std::string why;if(!init(why)){r.status=L"Unavailable: "+widen(why);return r;}
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world){r.status=L"No game world yet (load into a save)";return r;}
    if(act==13){r.status=aimProbe(p&0xFF,(p>>8)&0xFF,world);return r;}   // read-only aimbot feasibility probe (p: stage | key bits<<8)
    std::unordered_set<std::string> w={"GetPlayerController","GetCommunityComponent","ForceProduce","Consume","GetGameMode","GetActiveMissions","AlterMissionTimer","CombineAllItems"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    char m[300];
    if(act==0||act==1){   // 0 = load list, 1 = set amount (p = index<<16 | amount)
        std::vector<uintptr_t> list;std::wstring err;
        if(!getResources(hits,world,list,err)){r.status=err;return r;}
        if(act==1){
            const int uiRow=(p>>16)&0xFFFF,amt=(std::max)(0,(std::min)(9999,p&0xFFFF));
            int idx=-1;
            if(uiRow>=0&&uiRow<RES_ROWS)for(size_t i=0;i<list.size();++i)if(friendlyResName(list[i],(int)i)==resUiNames[uiRow]){idx=(int)i;break;}
            if(idx<0){r.status=L"That resource was not found";fillResourceList(list,r);return r;}
            const uintptr_t res=list[idx];uintptr_t rcls=0;rd(res+O_CLASS,rcls);
            Fn fP,fC;
            if(!resolveFnOn(hits,"ForceProduce",rcls,fP)){r.status=L"ForceProduce not found";fillResourceList(list,r);return r;}
            const bool haveC=resolveFnOn(hits,"Consume",rcls,fC);
            const auto pp=sortedParams(fP);const auto cp=haveC?sortedParams(fC):std::vector<std::pair<std::string,int>>();
            if(pp.size()<1){r.status=L"Unexpected ForceProduce layout";fillResourceList(list,r);return r;}
            const float cur=readAmount(res);
            auto produce=[&](float v){Buf b(fP.size);b.put(fP,pp[0].first.c_str(),v);
                if(pp.size()>1)b.put(fP,pp[1].first.c_str(),(unsigned char)8);
                if(pp.size()>2)b.put(fP,pp[2].first.c_str(),(unsigned char)1);
                return callPE(processEvent,res,fP.ptr,b.p);};
            auto consume=[&](float v){Buf b(fC.size);b.put(fC,cp[0].first.c_str(),v);
                return callPE(processEvent,res,fC.ptr,b.p);};
            bool okc=true;
            snprintf(m,sizeof(m),"Community: set resource %d to %d (current %.1f, Consume %s)",idx,amt,cur,haveC?"available":"missing");logMessage(m);
            if(cur>=0.f){
                const float d=(float)amt-cur;
                if(d>0.5f)okc=produce(d);
                else if(d<-0.5f){if(!haveC||cp.empty()){r.status=L"Consume not found - cannot lower";fillResourceList(list,r);return r;}okc=consume(-d);}
            }else{
                r.status=L"Could not read the current amount";fillResourceList(list,r);return r;
            }
            if(!okc){r.status=L"Engine call faulted (resource)";fillResourceList(list,r);return r;}
            float after=readAmount(res);
            snprintf(m,sizeof(m),"Community: resource %d now %.1f",idx,after);logMessage(m);
            bool direct=false;
            if(cur>=0.f&&after>=0.f&&fabsf(after-cur)<0.5f&&fabsf(after-(float)amt)>1.5f){
                // the game ignored the call (some resources are not producible this way): write the stock value directly
                const int so=findAmountOffset(rcls);
                if(so>=0&&safeWriteFloat(res+(uintptr_t)so,(float)amt)){direct=true;after=readAmount(res);snprintf(m,sizeof(m),"Community: resource %d direct write -> %.1f",idx,after);logMessage(m);}
            }
            wchar_t sb[160];
            if(after>=0.f&&fabsf(after-(float)amt)>1.5f)swprintf(sb,160,L"Requested %d, game allows %d",amt,(int)(after+0.5f));
            else swprintf(sb,160,direct?L"Set to %d (direct)":L"Set to %d",amt);
            r.status=sb;
        }else r.status=L"Resources loaded";
        fillResourceList(list,r);
        return r;
    }
    if(act==10){   // Give Item: list every loaded item blueprint (leaf classes of the known item families)
        const ULONGLONG giveT0=GetTickCount64();
        std::unordered_map<uintptr_t,bool> memoBP;std::unordered_set<uintptr_t> parents;
        struct Cand{uintptr_t obj;std::string name;int cat;};std::vector<Cand> cands;
        refreshCount();
        for(int i=0;i<numObjs;++i){
            uintptr_t o=0,c=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o||!rd(o+O_CLASS,c)||!c)continue;
            auto mm=memoBP.find(c);bool bp;
            if(mm==memoBP.end()){std::string cn;bp=objName(c,cn)&&cn=="BlueprintGeneratedClass";memoBP[c]=bp;}else bp=mm->second;
            if(!bp)continue;
            std::string nm;if(!objName(o,nm)||nm.size()<3||nm.compare(nm.size()-2,2,"_C")!=0||nm.compare(0,6,"REINST")==0||nm.compare(0,5,"SKEL_")==0)continue;
            int cat=-1;uintptr_t sup0=0;rd(o+O_SUPER,sup0);
            uintptr_t k=sup0;
            for(int g=0;k&&g<14&&cat<0;++g){
                std::string n;if(!objName(k,n))break;
                if(n=="RangedWeaponItem")cat=0;else if(n=="MeleeWeaponItem")cat=1;else if(n=="CloseCombatItem")cat=2;else if(n=="ConsumableItem")cat=3;else if(n=="AmmoItem")cat=4;else if(n.find("MiscellaneousItem")!=std::string::npos)cat=5;else if(n.find("Backpack")!=std::string::npos)cat=6;else if(n=="ResourceItem"||n.find("ResourceItem")==0)cat=7;else if(n.find("WeaponMod")!=std::string::npos)cat=8;
                uintptr_t sp=0;if(!rd(k+O_SUPER,sp))break;k=sp;
            }
            if(cat<0)continue;
            {   // real items always descend from the base "item" class; loot tables and other data classes do not
                bool isItem=false;uintptr_t q=sup0;
                for(int g=0;q&&g<16;++g){std::string n;if(!objName(q,n))break;if(n=="item"){isItem=true;break;}uintptr_t sp3=0;if(!rd(q+O_SUPER,sp3))break;q=sp3;}
                if(!isItem)continue;
            }
            parents.insert(sup0);cands.push_back({o,nm,cat});
        }
        // placeholder detection: the game stores the display name on each class default object; unfinished items read "DNL - INTERNAL ITEM"
        std::unordered_set<std::string> internalNames;
        {
            std::unordered_map<uintptr_t,uintptr_t> cdoOf;std::unordered_set<uintptr_t> leafCls;
            for(const Cand& cd:cands)if(!parents.count(cd.obj))leafCls.insert(cd.obj);
            for(int i=0;i<numObjs;++i){
                uintptr_t o=0,c=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o||!rd(o+O_CLASS,c)||!c)continue;
                if(!leafCls.count(c)||cdoOf.count(c))continue;
                std::string on;if(objName(o,on)&&on.compare(0,9,"Default__")==0)cdoOf[c]=o;
            }
            std::unordered_set<std::string> wt={"Default__KismetTextLibrary","Conv_TextToString"};int ot=0,tt=0;auto ht=scan(wt,ot,tt);
            uintptr_t cdoTxt=0;{auto it=ht.find("Default__KismetTextLibrary");if(it!=ht.end())for(const Hit& h:it->second)if(h.cls!="Function"){cdoTxt=h.obj;break;}}
            Fn fT2S;{auto it=ht.find("Conv_TextToString");if(it!=ht.end())for(const Hit& h:it->second)if(h.cls=="Function"&&readFn(fT2S,h.obj,{"ReturnValue"}))break;}
            const auto tps=fT2S.ptr?sortedParams(fT2S):std::vector<std::pair<std::string,int>>();
            int offDI=-1,offDN=-1;
            if(cdoTxt&&fT2S.ptr&&tps.size()==1){
                for(const Cand& cd:cands){
                    if(parents.count(cd.obj))continue;
                    auto ci=cdoOf.find(cd.obj);if(ci==cdoOf.end())continue;
                    if(offDI<0){
                        offDI=propOffset(cd.obj,"DisplayInfo");const uintptr_t dp=findPropObj(cd.obj,"DisplayInfo");const uintptr_t ds=dp?structOfProp(dp):0;
                        offDN=ds?propOffset(ds,"DisplayName"):-1;
                        if(offDI<0||offDN<0){offDI=-2;break;}
                    }
                    unsigned char raw[32]{};if(!safeRead(ci->second+(uintptr_t)offDI+(uintptr_t)offDN,raw,24))continue;
                    Buf b(fT2S.size);b.putBytes(fT2S,tps[0].first.c_str(),raw,24);
                    if(!callStatic(cdoTxt,fT2S,b))continue;
                    FStr fs{};if(!b.get(fT2S,"ReturnValue",fs)||!fs.data||fs.num<=0||fs.num>64)continue;
                    wchar_t tmp[70]{};if(!safeRead((uintptr_t)fs.data,tmp,(size_t)fs.num*2))continue;
                    tmp[fs.num<69?fs.num:69]=0;std::string v;for(const wchar_t* q=tmp;*q;++q)v.push_back(*q<128?(char)toupper((int)*q):'?');
                    if(v.find("DNL - INTERNAL")!=std::string::npos||v.find("INTERNAL ITEM")!=std::string::npos)internalNames.insert(cd.name);
                }
            }
            snprintf(m,sizeof(m),"Give Item: %d placeholder (internal) items tagged, %d class defaults read%s",(int)internalNames.size(),(int)cdoOf.size(),offDI==-2?" (DisplayName field not found)":"");logMessage(m);
            {int pc[9]{};for(const Cand& cd:cands){if(parents.count(cd.obj))continue;if(internalNames.count(cd.name)&&cd.cat>=0&&cd.cat<9)++pc[cd.cat];}
             snprintf(m,sizeof(m),"Give Item: internal per category (ranged %d, melee %d, close combat %d, consumable %d, ammo %d, misc %d, backpack %d, resource %d, weapon mod %d)",pc[0],pc[1],pc[2],pc[3],pc[4],pc[5],pc[6],pc[7],pc[8]);logMessage(m);}
        }
        std::vector<std::pair<std::wstring,int>> out;
        for(const Cand& cd:cands){if(parents.count(cd.obj))continue;std::wstring w;for(char ch:cd.name)w.push_back((wchar_t)(unsigned char)ch);out.emplace_back(w,cd.cat+(internalNames.count(cd.name)?100:0));}
        std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());
        int per[9]{};for(auto& e:out)++per[e.second%100];
        snprintf(m,sizeof(m),"Give Item: %d item classes found (ranged %d, melee %d, close combat %d, consumable %d, ammo %d, misc %d, backpack %d, resource %d, weapon mod %d)",(int)out.size(),per[0],per[1],per[2],per[3],per[4],per[5],per[6],per[7],per[8]);logMessage(m);
        for(auto& e:out){r.labels.push_back(e.first);r.amts.push_back((float)e.second);}
        r.haveList=true;
        wchar_t sb[96];swprintf(sb,96,L"%d items available",(int)out.size());r.status=sb;
        {char tm[100];snprintf(tm,sizeof(tm),"Give Item: list scan took %d ms",(int)(GetTickCount64()-giveT0));logMessage(tm);}
        return r;
    }
    if(act==11){   // Give Item: create the item with the game's own helper, then TryAddItem on the player's inventory (as Hades does)
        const int cat=p&0xFF;int count=p>>8;if(count<1)count=1;if(count>999)count=999;
        if(s1.empty()||cat<0||cat>8){r.status=L"Pick an item first";return r;}
        std::string cname;for(wchar_t ch:s1)cname.push_back((char)ch);
        static const char* fnames[9]={"CreateRangedWeaponInstance","CreateMeleeWeaponInstance","CreateCloseCombatInstance","CreateConsumableInstance","CreateAmmoInstance","CreateMiscellaneousInstance","CreateBackpackInstance","CreateResourceInstance","CreateRangedWeaponModInstance"};
        std::unordered_set<std::string> w11={cname,fnames[cat],"TryAddItem","GetInventoryComponent","GetInventory","Default__InventoryComponent","Default__ItemBlueprintHelpers","Default__"+cname};
        int o11=0,t11=0;auto h11=scan(w11,o11,t11);
        uintptr_t itemCls=0;{auto it=h11.find(cname);if(it!=h11.end())for(const Hit& h:it->second)if(h.cls=="BlueprintGeneratedClass"){itemCls=h.obj;break;}}
        if(!itemCls){snprintf(m,sizeof(m),"Give Item: class %s not found",cname.c_str());logMessage(m);r.status=L"Item class not found";return r;}
        uintptr_t helpers=0;{auto it=h11.find("Default__ItemBlueprintHelpers");if(it!=h11.end())for(const Hit& h:it->second)if(h.cls=="ItemBlueprintHelpers"){helpers=h.obj;break;}}
        if(!helpers){r.status=L"Item helper not found";return r;}
        uintptr_t hcls=0;rd(helpers+O_CLASS,hcls);Fn fC;
        if(!resolveFnOn(h11,fnames[cat],hcls,fC)){snprintf(m,sizeof(m),"Give Item: %s not found",fnames[cat]);logMessage(m);r.status=L"Create function not found";return r;}
        const auto cp=sortedParams(fC);
        if(cp.empty()){r.status=L"Unexpected create function layout";return r;}
        const bool stackable=cp.size()>=2;
        const uintptr_t pawn=playerPawn(world);if(!pawn){r.status=L"No player pawn";return r;}
        uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
        Fn fI;uintptr_t inv=0;
        if(resolveFnOn(h11,"GetInventory",pcls,fI)){Buf b(fI.size);if(callPE(processEvent,pawn,fI.ptr,b.p))b.get(fI,"ReturnValue",inv);}
        if(!inv){
            uintptr_t cdo=0;{auto it=h11.find("Default__InventoryComponent");if(it!=h11.end())for(const Hit& h:it->second)if(h.cls=="InventoryComponent"){cdo=h.obj;break;}}
            uintptr_t gcFn=0;Fn fGC;
            if(cdo&&findHit(h11,"GetInventoryComponent","Function","InventoryComponent",gcFn)&&readFn(fGC,gcFn,{})){
                const auto gp=sortedParams(fGC);uintptr_t comp=0;
                if(!gp.empty()){Buf b(fGC.size);b.put(fGC,gp[0].first.c_str(),pawn);if(callPE(processEvent,cdo,fGC.ptr,b.p))b.get(fGC,"ReturnValue",comp);}
                if(comp){uintptr_t ccls=0;rd(comp+O_CLASS,ccls);Fn g2;
                    if(resolveFnOn(h11,"GetInventory",ccls,g2)){Buf b(g2.size);if(callPE(processEvent,comp,g2.ptr,b.p))b.get(g2,"ReturnValue",inv);}}
            }
        }
        if(!inv){r.status=L"Inventory not reachable";return r;}
        uintptr_t icls=0;rd(inv+O_CLASS,icls);Fn fT;
        if(!resolveFnOn(h11,"TryAddItem",icls,fT)){r.status=L"TryAddItem not found";return r;}
        const auto tp=sortedParams(fT);
        if(tp.size()<2){r.status=L"Unexpected TryAddItem layout";return r;}
        const char* failStep="none";uintptr_t lastInst=0;
        auto addOne=[&](int stack)->bool{
            Buf b(fC.size);b.put(fC,cp[0].first.c_str(),itemCls);
            if(stackable)b.put(fC,cp[1].first.c_str(),(int)stack);
            if(!callStatic(helpers,fC,b)){failStep="create call faulted";return false;}
            uintptr_t inst=0;if(!b.get(fC,"ReturnValue",inst)||!inst){failStep="create returned no item";return false;}
            lastInst=inst;
            Buf t(fT.size);t.put(fT,tp[0].first.c_str(),inst);t.put(fT,tp[1].first.c_str(),(unsigned char)8);
            if(tp.size()>=3)t.put(fT,tp[2].first.c_str(),(unsigned char)0);
            if(!callPE(processEvent,inv,fT.ptr,t.p)){failStep="TryAddItem call faulted";return false;}
            unsigned char ok=0;t.get(fT,"ReturnValue",ok);if(!ok){failStep="TryAddItem refused the item";return false;}return true;
        };
        int added=0;
        if(stackable){
            int want=count;
            while(want>0&&added==0){if(addOne(want)){added=want;break;}want/=2;}   // stack limit: retry with a smaller stack
        }else{
            const int n=(std::min)(count,10);for(int i=0;i<n;++i){if(addOne(1))++added;else break;}
        }
        snprintf(m,sizeof(m),"Give Item: %s x%d requested -> %d added (%s)",cname.c_str(),count,added,fnames[cat]);logMessage(m);
        wchar_t sb[120];
        if(added>0&&added<count)swprintf(sb,120,L"Added %d (limit reached)",added);
        else if(added>0)swprintf(sb,120,L"Added %d to your inventory",added);
        else if(!strcmp(failStep,"create returned no item")||!strcmp(failStep,"create call faulted"))swprintf(sb,120,L"Not a giveable item (the game made nothing)");
        else if(!strcmp(failStep,"TryAddItem refused the item"))swprintf(sb,120,L"Inventory refused it (full or too heavy)");
        else swprintf(sb,120,L"Not added (%hs)",failStep);
        r.status=sb;return r;
    }
    if(act==12){   // read the player's current position (position bookmarks)
        const uintptr_t pawn=playerPawn(world);if(!pawn){r.status=L"No player pawn (are you in game?)";return r;}
        uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
        const int roff=propOffset(pcls,"RootComponent");uintptr_t root=0;if(roff>=0)rd(pawn+(uintptr_t)roff,root);
        float cur[3]{};if(!root||!safeRead(root+0x170,cur,12)){r.status=L"Could not read player location";return r;}
        r.amts={cur[0],cur[1],cur[2]};r.haveList=true;r.status=L"Position read";return r;
    }
    if(act==9){   // weapon preview: list the equipped weapon first, then the other weapons in the inventory (read only)
        std::unordered_set<std::string> w9={"GetDaytonCharacter","GetEquipmentComponent","GetRangedWeaponItemInstance","GetOtherRangedWeaponItemInstance","GetMeleeWeaponInstance","GetCloseCombatInstance","GetInventoryComponent","GetInventory","Default__InventoryComponent","Default__ItemBlueprintHelpers"};int o9=0,t9=0;auto h9=scan(w9,o9,t9);
        const uintptr_t pawn=playerPawn(world);if(!pawn){r.status=L"No player pawn";return r;}
        uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);uintptr_t dc=pawn;
        {Fn f;if(resolveFnOn(h9,"GetDaytonCharacter",pcls,f)){Buf b(f.size);uintptr_t v=0;if(callPE(processEvent,pawn,f.ptr,b.p)&&b.get(f,"ReturnValue",v)&&v)dc=v;}}
        auto classIdOf=[&](uintptr_t inst,std::string& out)->bool{
            uintptr_t c=0;if(!inst||!rd(inst+O_CLASS,c)||!c)return false;
            for(const char* pn:{"RangedWeaponItemClass","MeleeWeaponItemClass","CloseCombatItemClass"}){
                const int off=propOffset(c,pn);if(off<0)continue;
                uintptr_t v=0;if(rd(inst+(uintptr_t)off,v)&&v&&objName(v,out)&&!out.empty())return true;
            }
            std::string cn;objName(c,cn);   // melee / close combat instances: take the first class reference they hold
            if(cn.find("CloseCombat")!=std::string::npos||cn.find("Melee")!=std::string::npos){
                for(uintptr_t k=c;k;){
                    uintptr_t ch=0;rd(k+O_CHILDREN,ch);
                    for(int h=0;ch&&h<256;++h){std::string tn,pnm;uintptr_t pc=0;rd(ch+O_CLASS,pc);objName(pc,tn);objName(ch,pnm);int off=-1;rd(ch+O_PROPOFF,off);
                        if(tn=="ClassProperty"&&off>=0){uintptr_t v=0;if(rd(inst+(uintptr_t)off,v)&&v&&objName(v,out)&&!out.empty())return true;}
                        uintptr_t nx=0;if(!rd(ch+O_NEXT,nx))break;ch=nx;}
                    uintptr_t sup=0;if(!rd(k+O_SUPER,sup))break;k=sup;
                }
            }
            return false;
        };
        std::vector<std::string> ids;
        auto addId=[&](const std::string& n){for(const auto& e:ids)if(e==n)return;ids.push_back(n);};
        {   // equipped
            uintptr_t dcls=0;rd(dc+O_CLASS,dcls);uintptr_t eq=0;
            Fn f;if(resolveFnOn(h9,"GetEquipmentComponent",dcls,f)){Buf b(f.size);if(callPE(processEvent,dc,f.ptr,b.p))b.get(f,"ReturnValue",eq);}
            if(eq){uintptr_t ecls=0;rd(eq+O_CLASS,ecls);Fn g;uintptr_t inst=0;
                if(resolveFnOn(h9,"GetRangedWeaponItemInstance",ecls,g)){Buf b(g.size);if(callPE(processEvent,eq,g.ptr,b.p))b.get(g,"ReturnValue",inst);}
                std::string nm;if(classIdOf(inst,nm))addId(nm);
                for(const char* fn:{"GetOtherRangedWeaponItemInstance","GetMeleeWeaponInstance","GetCloseCombatInstance"}){
                    Fn h;uintptr_t oi=0;
                    if(!resolveFnOn(h9,fn,ecls,h))continue;
                    {Buf b(h.size);if(callPE(processEvent,eq,h.ptr,b.p))b.get(h,"ReturnValue",oi);}
                    std::string nm2;if(classIdOf(oi,nm2))addId(nm2);
                }}
        }
        const bool haveEquipped=!ids.empty();
        {   // inventory
            Fn fI;uintptr_t inv=0;
            if(resolveFnOn(h9,"GetInventory",pcls,fI)){Buf b(fI.size);if(callPE(processEvent,pawn,fI.ptr,b.p))b.get(fI,"ReturnValue",inv);}
            if(!inv){
                uintptr_t cdo=0;{auto it=h9.find("Default__InventoryComponent");if(it!=h9.end())for(const Hit& h:it->second)if(h.cls=="InventoryComponent"){cdo=h.obj;break;}}
                uintptr_t gcFn=0;Fn fGC;
                if(cdo&&findHit(h9,"GetInventoryComponent","Function","InventoryComponent",gcFn)&&readFn(fGC,gcFn,{})){
                    const auto gp=sortedParams(fGC);uintptr_t comp=0;
                    if(!gp.empty()){Buf b(fGC.size);b.put(fGC,gp[0].first.c_str(),pawn);if(callPE(processEvent,cdo,fGC.ptr,b.p))b.get(fGC,"ReturnValue",comp);}
                    if(comp){uintptr_t ccls=0;rd(comp+O_CLASS,ccls);Fn g2;
                        if(resolveFnOn(h9,"GetInventory",ccls,g2)){Buf b(g2.size);if(callPE(processEvent,comp,g2.ptr,b.p))b.get(g2,"ReturnValue",inv);}}
                }
            }
            if(inv){
                uintptr_t icls=0;rd(inv+O_CLASS,icls);const int io=propOffset(icls,"Items");
                struct TArr{uintptr_t data;int num,max;}a{};
                if(io>=0&&safeRead(inv+(uintptr_t)io,&a,sizeof(a))&&a.data&&a.num>0&&a.num<=2000){
                    for(int i=0;i<a.num;++i){uintptr_t it=0;if(!safeRead(a.data+(uintptr_t)i*8,&it,8)||!it)continue;std::string nm;if(classIdOf(it,nm))addId(nm);}
                }
            }
        }
        snprintf(m,sizeof(m),"Weapon preview: %d weapon(s) found, equipped %s",(int)ids.size(),haveEquipped?ids[0].c_str():"(none)");logMessage(m);
        for(const auto& id:ids){std::wstring w;for(char ch:id)w.push_back((wchar_t)(unsigned char)ch);r.labels.push_back(w);}
        r.haveList=true;r.amts.assign(1,haveEquipped?1.f:0.f);
        r.status=ids.empty()?L"No weapon found (equip or carry one)":(haveEquipped?L"Showing equipped weapon":L"Carried weapons listed (none equipped)");
        return r;
    }
    if(act==6||act==7){   // survivor name: 6 = read current, 7 = apply (s1 first name, s2 last name)
        std::unordered_set<std::string> w6={"GetDaytonCharacter","Default__KismetTextLibrary","Conv_StringToText","Conv_TextToString"};int o6=0,t6=0;auto h6=scan(w6,o6,t6);
        const uintptr_t pawn=playerPawn(world);if(!pawn){r.status=L"No player pawn";return r;}
        uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);
        uintptr_t dc=pawn;
        {Fn fDC;if(resolveFnOn(h6,"GetDaytonCharacter",pcls,fDC)){Buf b(fDC.size);uintptr_t v=0;if(callPE(processEvent,pawn,fDC.ptr,b.p)&&b.get(fDC,"ReturnValue",v)&&v)dc=v;}}
        uintptr_t dcls=0;rd(dc+O_CLASS,dcls);
        const int recOff=propOffset(dcls,"CharacterRecord");const uintptr_t recProp=findPropObj(dcls,"CharacterRecord");
        const uintptr_t recStruct=recProp?structOfProp(recProp):0;
        if(recOff<0||!recStruct){logMessage("Name: CharacterRecord not found on the survivor");r.status=L"Survivor record not found (logged)";return r;}
        const int offF=propOffset(recStruct,"FirstName"),offL=propOffset(recStruct,"LastName"),offN=propOffset(recStruct,"NickName");
        snprintf(m,sizeof(m),"Name: record @%d, FirstName @%d, LastName @%d, NickName @%d",recOff,offF,offL,offN);logMessage(m);
        if(offF<0||offL<0){r.status=L"Name fields not found (logged)";return r;}
        uintptr_t cdoTxt=0;{auto it=h6.find("Default__KismetTextLibrary");if(it!=h6.end())for(const Hit& h:it->second)if(h.cls!="Function"){cdoTxt=h.obj;break;}}
        Fn fS2T,fT2S;
        {auto it=h6.find("Conv_StringToText");if(it!=h6.end())for(const Hit& h:it->second)if(h.cls=="Function"&&readFn(fS2T,h.obj,{"ReturnValue"}))break;}
        {auto it=h6.find("Conv_TextToString");if(it!=h6.end())for(const Hit& h:it->second)if(h.cls=="Function"&&readFn(fT2S,h.obj,{"ReturnValue"}))break;}
        if(!cdoTxt||!fS2T.ptr||!fT2S.ptr){r.status=L"Text conversion functions not found";return r;}
        const auto sp=sortedParams(fS2T),tp=sortedParams(fT2S);
        if(sp.size()!=1||tp.size()!=1){r.status=L"Unexpected text function layout";return r;}
        auto rvS=fS2T.off.find("ReturnValue");
        if(rvS==fS2T.off.end()){r.status=L"Unexpected text function layout";return r;}
        const int tsz=fS2T.size-rvS->second>=24?24:16;
        const uintptr_t rec=dc+(uintptr_t)recOff;
        auto readName=[&](int off,std::wstring& out)->bool{
            Buf b(fT2S.size);unsigned char raw[32]{};
            if(!safeRead(rec+(uintptr_t)off,raw,(size_t)tsz))return false;
            b.putBytes(fT2S,tp[0].first.c_str(),raw,(size_t)tsz);
            if(!callStatic(cdoTxt,fT2S,b))return false;
            FStr fs{};if(!b.get(fT2S,"ReturnValue",fs)||!fs.data||fs.num<=0||fs.num>256)return false;
            wchar_t tmp[260]{};if(!safeRead((uintptr_t)fs.data,tmp,(size_t)(fs.num)*2))return false;
            tmp[fs.num<259?fs.num:259]=0;out=tmp;return true;
        };
        auto writeName=[&](int off,const std::wstring& val)->bool{
            static wchar_t wb[2][64];static int slot=0;wchar_t* buf=wb[slot^=1];
            const size_t n=(std::min)((size_t)40,val.size());for(size_t i=0;i<n;++i)buf[i]=val[i];buf[n]=0;
            FStr fs{buf,(int)n+1,64};
            Buf b(fS2T.size);b.putBytes(fS2T,sp[0].first.c_str(),&fs,sizeof(fs));
            if(!callStatic(cdoTxt,fS2T,b))return false;
            unsigned char raw[32]{};memcpy(raw,(char*)b.p+rvS->second,(size_t)tsz);
            DWORD old=0;if(!VirtualProtect((void*)(rec+(uintptr_t)off),(size_t)tsz,PAGE_READWRITE,&old))return false;
            memcpy((void*)(rec+(uintptr_t)off),raw,(size_t)tsz);VirtualProtect((void*)(rec+(uintptr_t)off),(size_t)tsz,old,&old);
            return true;
        };
        std::wstring f0,l0,n0;
        const bool okr=readName(offF,f0)&&readName(offL,l0);
        const bool okn=offN>=0&&readName(offN,n0);
        snprintf(m,sizeof(m),"Name: nickname %s: \"%ls\"",okn?"read ok":"not available",n0.c_str());logMessage(m);
        snprintf(m,sizeof(m),"Name: read %s: \"%ls\" \"%ls\" (text size %d)",okr?"ok":"FAILED",f0.c_str(),l0.c_str(),tsz);logMessage(m);
        if(act==6){
            if(!okr){r.status=L"Could not read the name (logged)";return r;}
            r.haveNames=true;r.first=f0;r.last=l0;r.haveNick=okn;r.nick=n0;r.status=okn?L"Loaded current name":L"Loaded name (no nickname field found)";return r;
        }
        if(!okr){r.status=L"Could not read the current name - not changed";return r;}
        std::wstring nf=s1.empty()?f0:s1,nl=s2.empty()?l0:s2;
        std::wstring nn=s3.empty()?n0:s3;
        if(!writeName(offF,nf)||!writeName(offL,nl)){r.status=L"Write failed (logged)";return r;}
        if(okn&&!writeName(offN,nn)){r.status=L"Nickname write failed (logged)";return r;}
        std::wstring f1,l1,n1;bool okr2=readName(offF,f1)&&readName(offL,l1);
        if(okn){r.haveNick=readName(offN,n1);r.nick=n1;if(!r.haveNick)okr2=false;}
        snprintf(m,sizeof(m),"Name: now \"%ls\" \"%ls\" nick \"%ls\" (read-back %s)",f1.c_str(),l1.c_str(),n1.c_str(),okr2?"ok":"failed");logMessage(m);
        r.haveNames=okr2;r.first=f1;r.last=l1;
        r.status=(okr2&&f1==nf&&l1==nl&&(!okn||n1==nn))?L"Name changed - save the game to keep it":L"Written, but read-back differs (logged)";
        return r;
    }
    if(act==4||act==5){   // Haven protocol: SetExposedToHavenProtocol(flag, none) on every zombie character (as Hades does)
        std::unordered_set<std::string> w5={"DaytonZombieCharacter","SetExposedToHavenProtocol"};int o5=0,t5=0;auto h5=scan(w5,o5,t5);
        uintptr_t zcls=0;{auto it=h5.find("DaytonZombieCharacter");if(it!=h5.end())for(const Hit& h:it->second)if(h.cls=="Class"){zcls=h.obj;break;}}
        if(!zcls){r.status=L"Zombie class not found";return r;}
        Fn fH;if(!resolveFnOn(h5,"SetExposedToHavenProtocol",zcls,fH)){r.status=L"SetExposedToHavenProtocol not found";return r;}
        const auto hp=sortedParams(fH);
        if(hp.empty()){r.status=L"Unexpected function layout";return r;}
        int applied=0,seen=0;std::unordered_map<uintptr_t,bool> memo;refreshCount();
        for(int i=0;i<numObjs;++i){
            uintptr_t o=0,c=0;if(!rd(objs+(size_t)i*ITEM_SIZE,o)||!o||!rd(o+O_CLASS,c)||!c)continue;
            auto mm=memo.find(c);bool match;if(mm==memo.end()){match=isA(c,zcls);memo[c]=match;}else match=mm->second;
            if(!match)continue;
            int fl=0,ifl=0;if(!rd(o+8,fl)||(fl&0x30)||(fl&0x20000000))continue;
            if(rd(objs+(size_t)i*ITEM_SIZE+8,ifl)&&(ifl&0x20000000))continue;
            std::string nm;if(!objName(o,nm)||nm.compare(0,9,"Default__")==0||nm.compare(0,7,"REINST_")==0||nm.compare(0,5,"SKEL_")==0)continue;
            uintptr_t outer=0,oc=0;std::string on;if(!rd(o+O_OUTER,outer)||!rd(outer+O_CLASS,oc)||!objName(oc,on)||on!="Level")continue;
            ++seen;
            Buf b(fH.size);b.put(fH,hp[0].first.c_str(),(unsigned char)(act==4?1:0));
            if(callPE(processEvent,o,fH.ptr,b.p))++applied;
        }
        snprintf(m,sizeof(m),"Haven protocol: %s on %d of %d zombies",act==4?"set":"cleared",applied,seen);logMessage(m);
        wchar_t sb[120];
        if(seen==0)swprintf(sb,120,L"No zombies loaded right now");else swprintf(sb,120,act==4?L"Exposed %d zombie(s)":L"Cleared on %d zombie(s)",applied);
        r.status=sb;return r;
    }
    if(act==3){   // Mission timer: p = signed seconds
        Fn fGM;uintptr_t gmFn=0,gm=0;
        if(!findHit(hits,"GetGameMode","Function","GameplayStatics",gmFn)||!readFn(fGM,gmFn,{"WorldContextObject","ReturnValue"})){r.status=L"GetGameMode not found";return r;}
        {Buf b(fGM.size);b.put(fGM,"WorldContextObject",world);if(callPE(processEvent,cdoStatics,fGM.ptr,b.p))b.get(fGM,"ReturnValue",gm);}
        if(!gm){r.status=L"No game mode yet";return r;}
        uintptr_t gcls=0;rd(gm+O_CLASS,gcls);const int mo=propOffset(gcls,"MissionModeComponentNative");
        uintptr_t mc=0;if(mo>=0)rd(gm+(uintptr_t)mo,mc);
        if(!mc){r.status=L"Mission system not found";return r;}
        uintptr_t mcls=0;rd(mc+O_CLASS,mcls);
        Fn fGA,fAl;
        if(!resolveFnOn(hits,"GetActiveMissions",mcls,fGA)){r.status=L"GetActiveMissions not found";return r;}
        if(!resolveFnOn(hits,"AlterMissionTimer",mcls,fAl)){r.status=L"AlterMissionTimer not found";return r;}
        const auto ap=sortedParams(fAl);
        if(ap.size()<2){r.status=L"Unexpected AlterMissionTimer layout";return r;}
        struct TArr{uintptr_t data;int num,max;}a{};
        {Buf b(fGA.size);if(!callPE(processEvent,mc,fGA.ptr,b.p)){r.status=L"Engine call faulted (missions)";return r;}
         auto oi=fGA.off.find("ReturnValue");if(oi!=fGA.off.end()&&(size_t)oi->second+sizeof(a)<=b.n)memcpy(&a,(char*)b.p+oi->second,sizeof(a));}
        if(!a.data||a.num<=0||a.num>32){r.status=L"No active mission";return r;}
        int done=0;
        for(int i=0;i<a.num;++i){
            uintptr_t ms=0;if(!safeRead(a.data+(uintptr_t)i*8,&ms,8)||!ms)continue;
            Buf b(fAl.size);b.put(fAl,ap[0].first.c_str(),(float)p);b.put(fAl,ap[1].first.c_str(),ms);
            if(callPE(processEvent,mc,fAl.ptr,b.p))++done;
        }
        snprintf(m,sizeof(m),"Mission timer: %+d applied to %d of %d active missions",p,done,a.num);logMessage(m);
        wchar_t sb[120];swprintf(sb,120,L"%+d applied to %d mission(s)",p,done);r.status=done?sb:L"Engine call faulted (timer)";
        return r;
    }
    r.status=L"Unknown action";return r;
}

// Staged teleport: move to the marker at a safe height, hold there while the ground streams in, then drop onto it.
struct TeleState{uintptr_t actor=0,root=0,ksl=0,mover=0;Fn fSet,fTrace,fStop;int hrOff=24;float tx=0,ty=0,hoverZ=0;int ticks=0;bool vehicle=false;float tz=0;bool useZ=false;};
TeleState g_tp;std::atomic<bool> g_tpActive{false};std::atomic<bool> g_tpBookmark{false};
inline const wchar_t* tpDone(){return g_tp.vehicle?L"Teleported with vehicle":(g_tp.useZ?L"Teleported to bookmark":L"Teleported to marker");}std::atomic<bool> g_tpThread{false};std::atomic<bool> g_tpPending{false};
bool teleTrace(float x,float y,float& zOut,bool diag){
    if(!g_tp.ksl||!g_tp.fTrace.ptr)return false;
    static const unsigned char combos[4][2]={{0,0},{0,1},{1,0},{1,1}};   // {TraceChannel, bTraceComplex}
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return false;
    for(int c=0;c<4;++c){
        float st[3]={x,y,30000.f},en[3]={x,y,-20000.f};
        Buf b(g_tp.fTrace.size);b.put(g_tp.fTrace,"WorldContextObject",world);b.putBytes(g_tp.fTrace,"Start",st,12);b.putBytes(g_tp.fTrace,"End",en,12);
        b.put(g_tp.fTrace,"TraceChannel",combos[c][0]);b.put(g_tp.fTrace,"bTraceComplex",combos[c][1]);b.put(g_tp.fTrace,"bIgnoreSelf",(unsigned char)1);
        if(!callPE(processEvent,g_tp.ksl,g_tp.fTrace.ptr,b.p))continue;
        unsigned char hit=0;b.get(g_tp.fTrace,"ReturnValue",hit);
        auto oi=g_tp.fTrace.off.find("OutHit");float ip[3]{};
        if(oi!=g_tp.fTrace.off.end()&&(size_t)oi->second+g_tp.hrOff+12<=b.n)memcpy(ip,(char*)b.p+oi->second+g_tp.hrOff,12);
        if(diag){char m[200];snprintf(m,sizeof(m),"Teleport: trace ch=%d complex=%d hit=%d impactZ=%.1f",(int)combos[c][0],(int)combos[c][1],(int)hit,ip[2]);logMessage(m);}
        if(hit&&ip[2]>-19000.f&&ip[2]<31000.f){zOut=ip[2];return true;}
    }
    return false;
}
bool teleSet(float x,float y,float z){
    Buf b(g_tp.fSet.size);float nl[3]={x,y,z};b.putBytes(g_tp.fSet,"NewLocation",nl,12);b.put(g_tp.fSet,"bSweep",(unsigned char)0);b.put(g_tp.fSet,"bTeleport",(unsigned char)1);
    if(!callPE(processEvent,g_tp.actor,g_tp.fSet.ptr,b.p))return false;
    if(g_tp.mover&&g_tp.fStop.ptr){Buf s(g_tp.fStop.size);callPE(processEvent,g_tp.mover,g_tp.fStop.ptr,s.p);}   // cancel falling speed
    return true;
}
// Move straight down by 'step' with collision; returns true if it moved the whole way (nothing below yet).
bool teleSweepDown(float step,float& zNow){
    float cur[3]{};if(!safeRead(g_tp.root+0x170,cur,12))return false;
    Buf b(g_tp.fSet.size);float nl[3]={cur[0],cur[1],cur[2]-step};
    b.putBytes(g_tp.fSet,"NewLocation",nl,12);b.put(g_tp.fSet,"bSweep",(unsigned char)1);b.put(g_tp.fSet,"bTeleport",(unsigned char)0);
    if(!callPE(processEvent,g_tp.actor,g_tp.fSet.ptr,b.p)){zNow=cur[2];return false;}
    float after[3]{};safeRead(g_tp.root+0x170,after,12);zNow=after[2];
    if(g_tp.mover&&g_tp.fStop.ptr){Buf s(g_tp.fStop.size);callPE(processEvent,g_tp.mover,g_tp.fStop.ptr,s.p);}
    return (cur[2]-after[2])>=step-2.f;
}
// Called every ~150 ms on the game thread while a teleport is pending. more=false when finished.
std::wstring teleportTick(bool& more){
    more=false;g_tpPending=false;
    if(!g_tpActive.load())return L"";
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world){g_tpActive=false;return L"Teleport cancelled (no world)";}
    if(playerPawn(world)!=g_tp.actor){g_tpActive=false;logMessage("Teleport: player changed during teleport, stopped");return L"Teleport stopped (character changed)";}
    ++g_tp.ticks;
    float gz=0;
    if(teleTrace(g_tp.tx,g_tp.ty,gz,false)){
        const float z=g_tp.useZ?g_tp.tz+20.f:gz+150.f;teleSet(g_tp.tx,g_tp.ty,z);g_tpActive=false;
        char m[200];snprintf(m,sizeof(m),"Teleport: ground found after %d ticks, placed at z=%.1f",g_tp.ticks,z);logMessage(m);
        return tpDone();
    }
    if(g_tp.ticks>=1000){g_tpActive=false;logMessage("Teleport: ground never loaded, released");return L"Ground did not load - released (you may fall)";}
    if(g_tp.ticks>=30){   // no trace result: lower with collision until something solid stops us
        float zNow=0;const bool free=teleSweepDown(250.f,zNow);
        if(!free){g_tpActive=false;char m[200];snprintf(m,sizeof(m),"Teleport: landed by sweep at z=%.1f after %d ticks",zNow,g_tp.ticks);logMessage(m);
            return tpDone();}
        if(zNow<-15000.f){g_tpActive=false;logMessage("Teleport: sweep found nothing, stopped");return L"No ground found - stopped";}
        more=true;return L"Lowering to the ground...";
    }
    teleSet(g_tp.tx,g_tp.ty,g_tp.hoverZ);   // brief hold at hover height while the area streams in
    more=true;return L"Waiting for ground to load...";
}
std::wstring teleportToMarker(bool bookmark=false,float bx=0,float by=0,float bz=0){
    std::string why;if(!init(why))return L"Unavailable: "+widen(why);
    uintptr_t world=0;if(!rd(base+RVA_GWORLD,world)||!world)return L"No game world yet (load into a save)";
    const uintptr_t pawn=playerPawn(world);if(!pawn)return L"No player pawn (are you in game?)";
    std::unordered_set<std::string> w={"GetGameState","K2_SetActorLocation","LineTraceSingle","Default__KismetSystemLibrary","HitResult","StopMovementImmediately"};
    int ok=0,t=0;auto hits=scan(w,ok,t);
    char m[300];
    float wp[3]{},mn[3]{},mx[3]{};TeleState st;
    if(bookmark){st.tx=bx;st.ty=by;st.tz=bz;st.useZ=true;}
    else{
    Fn fGS;uintptr_t gsFn=0,gs=0;
    if(!findHit(hits,"GetGameState","Function","GameplayStatics",gsFn)||!readFn(fGS,gsFn,{"WorldContextObject","ReturnValue"}))return L"GetGameState not found";
    {Buf b(fGS.size);b.put(fGS,"WorldContextObject",world);if(callPE(processEvent,cdoStatics,fGS.ptr,b.p))b.get(fGS,"ReturnValue",gs);}
    if(!gs)return L"No game state yet";
    uintptr_t comp=0;{uintptr_t gcls=0;rd(gs+O_CLASS,gcls);const int off=propOffset(gcls,"MapStateComponent");if(off>=0)rd(gs+(uintptr_t)off,comp);}
    if(!comp)return L"Map state not found";
    struct TArr{uintptr_t data;int num,max;}a{};
    safeRead(comp+632,&a,sizeof(a));safeRead(comp+2028,mn,12);safeRead(comp+2040,mx,12);
    if(!a.data||a.num<=0)return L"Place a map marker first";
    safeRead(a.data,wp,12);
    if(!(wp[0]>=0.f&&wp[0]<=1.f&&wp[1]>=0.f&&wp[1]<=1.f)||!(mx[0]>mn[0])||!(mx[1]>mn[1]))return L"Map marker data looks invalid";
    st.tx=mn[0]+wp[0]*(mx[0]-mn[0]);st.ty=mn[1]+wp[1]*(mx[1]-mn[1]);
    }
    uintptr_t pcls=0;rd(pawn+O_CLASS,pcls);std::string pn;objName(pcls,pn);
    {std::string vw;initVehicle(vw);}
    st.vehicle=clsVehicle&&isA(pcls,clsVehicle);   // while driving the pawn is the vehicle itself, so it carries the player
    const int roff=propOffset(pcls,"RootComponent");uintptr_t root=0;if(roff>=0)rd(pawn+(uintptr_t)roff,root);
    float cur[3]{};if(!root||!safeRead(root+0x170,cur,12))return L"Could not read player location";
    st.actor=pawn;st.root=root;
    {auto it=hits.find("Default__KismetSystemLibrary");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="KismetSystemLibrary"){st.ksl=h.obj;break;}}
    uintptr_t tFn=0;
    {char dm[200];snprintf(dm,sizeof(dm),"Teleport: trace setup: ksl=%s",st.ksl?"found":"MISSING");logMessage(dm);}
    if(st.ksl&&findHit(hits,"LineTraceSingle","Function","KismetSystemLibrary",tFn))readFn(st.fTrace,tFn,{"WorldContextObject","Start","End","OutHit"});
    {auto it=hits.find("HitResult");if(it!=hits.end())for(const Hit& h:it->second)if(h.cls=="ScriptStruct"){const int o=propOffset(h.obj,"ImpactPoint");if(o>=0)st.hrOff=o;break;}}
    {auto it=hits.find("LineTraceSingle");size_t n=it==hits.end()?0:it->second.size();std::string d;
     if(it!=hits.end())for(const Hit& h:it->second){std::string on;uintptr_t ou=0,oc=0;if(rd(h.obj+O_OUTER,ou)&&ou){std::string x;objName(ou,x);on=x;}d+=h.cls+"/"+on+" ";}
     char dm[400];snprintf(dm,sizeof(dm),"Teleport: LineTraceSingle hits=%llu fn=%s: %s",(unsigned long long)n,st.fTrace.ptr?"ok":"unresolved",d.c_str());logMessage(dm);}
    {auto it=hits.find("K2_SetActorLocation");
     if(it!=hits.end())for(const Hit& h:it->second){uintptr_t outer=0;
         if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(pcls,outer)&&readFn(st.fSet,h.obj,{"NewLocation"}))break;}}
    if(!st.fSet.ptr)return L"K2_SetActorLocation not found";
    // movement component (to cancel falling speed while holding): character pawns only
    if(!st.vehicle){
        const int mo=propOffset(pcls,"CharacterMovement");uintptr_t mc=0;if(mo>=0)rd(pawn+(uintptr_t)mo,mc);
        if(mc){uintptr_t mcls=0;rd(mc+O_CLASS,mcls);
            auto it=hits.find("StopMovementImmediately");
            if(it!=hits.end())for(const Hit& h:it->second){uintptr_t outer=0;
                if(h.cls=="Function"&&rd(h.obj+O_OUTER,outer)&&isA(mcls,outer)&&readFn(st.fStop,h.obj,{})){st.mover=mc;break;}}}
    }
    st.hoverZ=(cur[2]>3500.f?cur[2]:3500.f)+6000.f;
    snprintf(m,sizeof(m),"Teleport: marker uv=(%.4f, %.4f) -> target x=%.1f y=%.1f; now %.1f %.1f %.1f; moving %s; mover %s, trace %s",wp[0],wp[1],st.tx,st.ty,cur[0],cur[1],cur[2],pn.c_str(),st.mover?"yes":"no",st.fTrace.ptr?"yes":"no");logMessage(m);
    g_tp=st;
    {   // diagnostic: does the trace work at all? try under the player's own feet
        float z0=0;const bool h0=teleTrace(cur[0],cur[1],z0,true);
        snprintf(m,sizeof(m),"Teleport: trace at current position %s (z=%.1f, player z=%.1f)",h0?"hit":"MISS",z0,cur[2]);logMessage(m);
    }
    float gz=0;
    if(teleTrace(g_tp.tx,g_tp.ty,gz,false)){   // target already loaded: go straight to the ground
        const float zPlace=g_tp.useZ?g_tp.tz+20.f:gz+150.f;
        if(!teleSet(g_tp.tx,g_tp.ty,zPlace))return L"Engine call faulted (teleport)";
        snprintf(m,sizeof(m),"Teleport: direct placement at z=%.1f",zPlace);logMessage(m);
        return tpDone();
    }
    if(!teleSet(g_tp.tx,g_tp.ty,g_tp.hoverZ))return L"Engine call faulted (teleport)";
    logMessage("Teleport: moved to hover height, waiting for ground");
    g_tp.ticks=0;g_tpActive=true;
    return L"Waiting for ground to load...";
}
} // namespace spawner

bool accessible(const void* p,size_t n,bool write=false){
    if(!p||!n)return false;MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery(p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&PAGE_GUARD))return false;
    DWORD flags=m.Protect&0xff;if(flags==PAGE_NOACCESS)return false;
    if(write&&flags!=PAGE_READWRITE&&flags!=PAGE_WRITECOPY&&flags!=PAGE_EXECUTE_READWRITE&&flags!=PAGE_EXECUTE_WRITECOPY)return false;
    auto a=(uintptr_t)p,b=(uintptr_t)m.BaseAddress;return a>=b&&n<=m.RegionSize-(a-b);
}
template<size_t N> uintptr_t resolve(uintptr_t module,uintptr_t offset,const uintptr_t (&chain)[N]){
    if(module>UINTPTR_MAX-offset)return 0;uintptr_t at=module+offset;
    if(!accessible((void*)at,sizeof(uintptr_t)))return 0;
    uintptr_t p=*(uintptr_t*)at;
    for(size_t i=0;i+1<N;i++){
        if(!p||p>UINTPTR_MAX-chain[i])return 0;
        at=p+chain[i];if(!accessible((void*)at,sizeof(uintptr_t)))return 0;p=*(uintptr_t*)at;
    }
    return (!p||p>UINTPTR_MAX-chain[N-1])?0:p+chain[N-1];
}
// Current-build stamina writer hook. Only installed if the original five bytes match.
// The detour is a 5-byte relative JMP to a nearby executable trampoline.
constexpr uintptr_t STAMINA_WRITE_RVA=0x1B9593;
const unsigned char STAMINA_WRITE_ORIGINAL[5]={0xF3,0x0F,0x11,0x73,0x08};
void* staminaTrampoline=nullptr;
bool staminaHookInstalled=false;

void* allocateNear(uintptr_t target,size_t size){
    SYSTEM_INFO si{};GetSystemInfo(&si);
    uintptr_t gran=si.dwAllocationGranularity;
    uintptr_t center=target & ~(gran-1);
    constexpr uintptr_t maxDistance=0x70000000;
    for(uintptr_t dist=0;dist<maxDistance;dist+=gran){
        for(int sign=0;sign<2;sign++){
            if(sign==1&&dist==0)continue;
            uintptr_t candidate=sign==0?center+dist:center-dist;
            if((sign==0&&candidate<center)||(sign==1&&center<dist))continue;
            auto mem=VirtualAlloc((void*)candidate,size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
            if(mem)return mem;
        }
    }
    return nullptr;
}

bool relativeJump(unsigned char* out,uintptr_t from,uintptr_t to);

// CT-derived, opt-in hooks. Scan only the game's executable image; refuse ambiguous AOBs.
// Each trampoline executes the original instruction while OFF, and the CT's
// additional operation while ON. No new game-memory writes occur on disabling.
struct CtHookSpec {
    const char* name;
    const unsigned char* pattern; size_t patternSize;
    size_t stolenSize; const unsigned char* enabledCode; size_t enabledSize;
    bool enabledAfterOriginal; std::atomic<bool>* flag;
};
struct CtHookRecord {uintptr_t address; size_t size; unsigned char original[16]; void* cave;};
std::vector<CtHookRecord> ctInstalled;
void appendBytes(std::vector<unsigned char>& out,const unsigned char* bytes,size_t n){out.insert(out.end(),bytes,bytes+n);}
void appendRel32(std::vector<unsigned char>& out,uintptr_t caveBase,uintptr_t destination){
    const auto from=caveBase+out.size();
    const int64_t displacement=(int64_t)destination-(int64_t)(from+5);
    out.push_back(0xE9);
    const int32_t d=(int32_t)displacement;
    appendBytes(out,(const unsigned char*)&d,4);
}
uintptr_t findUniqueCtPattern(uintptr_t module,const unsigned char* pat,size_t n){
    auto* dos=(IMAGE_DOS_HEADER*)module;
    if(!accessible(dos,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
    auto* nt=(IMAGE_NT_HEADERS64*)(module+dos->e_lfanew);
    if(!accessible(nt,sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE)return 0;
    uintptr_t found=0;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++){
        const auto& sec=sections[i];
        if(!(sec.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        const uintptr_t start=module+sec.VirtualAddress;
        const size_t size=sec.Misc.VirtualSize;
        if(size<n||!accessible((void*)start,size))continue;
        for(size_t j=0;j<=size-n;j++){
            if(memcmp((void*)(start+j),pat,n)==0){
                if(found)return 0; // fail closed on multiple matches
                found=start+j;
            }
        }
    }
    return found;
}
// Match CT AOBs with explicit wildcards (-1); reject missing or ambiguous results.
uintptr_t findUniqueMaskedPattern(uintptr_t module,const int* signature,size_t count){
    auto* dos=(IMAGE_DOS_HEADER*)module;
    if(!accessible(dos,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
    auto* nt=(IMAGE_NT_HEADERS64*)(module+dos->e_lfanew);
    if(!accessible(nt,sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE)return 0;
    uintptr_t found=0;
    auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++){
        const auto& sec=sections[i];
        if(!(sec.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        uintptr_t start=module+sec.VirtualAddress;size_t size=sec.Misc.VirtualSize;
        if(size<count||!accessible((void*)start,size))continue;
        for(size_t pos=0;pos<=size-count;pos++){
            const unsigned char* bytes=(const unsigned char*)(start+pos);
            size_t j=0;for(;j<count;j++)if(signature[j]>=0&&bytes[j]!=(unsigned char)signature[j])break;
            if(j==count){if(found)return 0;found=start+pos;}
        }
    }
    return found;
}
template<size_t N> uintptr_t findMasked(uintptr_t module,const int (&sig)[N]){
    return findUniqueMaskedPattern(module,sig,N);
}
bool installCtHook(uintptr_t module,const CtHookSpec& spec,uintptr_t targetOverride=0){
    if(spec.stolenSize<5||spec.stolenSize>16||spec.patternSize<spec.stolenSize)return false;
    const uintptr_t target=targetOverride?targetOverride:findUniqueCtPattern(module,spec.pattern,spec.patternSize);
    if(targetOverride&&(!accessible((void*)target,spec.stolenSize)||memcmp((void*)target,spec.pattern,spec.stolenSize)))return false;
    if(!target){logMessage(spec.name);return false;}
    for(const auto& h:ctInstalled){
        if(target<h.address+h.size && h.address<target+spec.stolenSize){
            logMessage("CT hook conflict: overlapping patch; skipped");return false;
        }
    }
    auto* cave=(unsigned char*)allocateNear(target,256);
    if(!cave){logMessage("CT hook: nearby allocation failed");return false;}
    std::vector<unsigned char> code;
    // pushfq; push rax; mov rax, &atomic_flag; cmp byte ptr [rax],0
    const unsigned char prolog[]={0x9C,0x50,0x48,0xB8};
    appendBytes(code,prolog,sizeof(prolog));
    uintptr_t flagAddress=(uintptr_t)spec.flag;
    appendBytes(code,(unsigned char*)&flagAddress,8);
    const unsigned char cmp[]={0x80,0x38,0x00,0x0F,0x84};
    appendBytes(code,cmp,sizeof(cmp));
    size_t jeDisp=code.size();code.insert(code.end(),4,0);
    const unsigned char restore[]={0x58,0x9D};
    appendBytes(code,restore,2);
    if(!spec.enabledAfterOriginal)appendBytes(code,spec.enabledCode,spec.enabledSize);
    appendBytes(code,spec.pattern,spec.stolenSize);
    if(spec.enabledAfterOriginal)appendBytes(code,spec.enabledCode,spec.enabledSize);
    // Jump past disabled path. E9 rel32.
    const size_t skipAt=code.size();code.insert(code.end(),5,0);
    const size_t disabledAt=code.size();
    appendBytes(code,restore,2);
    appendBytes(code,spec.pattern,spec.stolenSize);
    const size_t returnAt=code.size();
    code.insert(code.end(),5,0);
    int64_t je=(int64_t)disabledAt-(int64_t)(jeDisp+4);
    int64_t skip=(int64_t)returnAt-(int64_t)(skipAt+5);
    int64_t back=(int64_t)(target+spec.stolenSize)-(int64_t)((uintptr_t)cave+returnAt+5);
    if(je<INT32_MIN||je>INT32_MAX||skip<INT32_MIN||skip>INT32_MAX||back<INT32_MIN||back>INT32_MAX){
        VirtualFree(cave,0,MEM_RELEASE);return false;
    }
    int32_t d=(int32_t)je;memcpy(code.data()+jeDisp,&d,4);
    code[skipAt]=0xE9;d=(int32_t)skip;memcpy(code.data()+skipAt+1,&d,4);
    code[returnAt]=0xE9;d=(int32_t)back;memcpy(code.data()+returnAt+1,&d,4);
    if(code.size()>256){VirtualFree(cave,0,MEM_RELEASE);return false;}
    unsigned char patch[16]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){
        VirtualFree(cave,0,MEM_RELEASE);return false;
    }
    for(size_t i=5;i<spec.stolenSize;i++)patch[i]=0x90;
    memcpy(cave,code.data(),code.size());
    FlushInstructionCache(GetCurrentProcess(),cave,code.size());
    DWORD old{};
    if(!VirtualProtect((void*)target,spec.stolenSize,PAGE_EXECUTE_READWRITE,&old)){
        VirtualFree(cave,0,MEM_RELEASE);return false;
    }
    CtHookRecord record{};record.address=target;record.size=spec.stolenSize;record.cave=cave;
    memcpy(record.original,(void*)target,spec.stolenSize);
    memcpy((void*)target,patch,spec.stolenSize);
    FlushInstructionCache(GetCurrentProcess(),(void*)target,spec.stolenSize);
    DWORD ignored{};VirtualProtect((void*)target,spec.stolenSize,old,&ignored);
    ctInstalled.push_back(record);
    logMessage(spec.name);
    return true;
}
// Live controls ported from the supplied steam_u37_2 Cheat Engine table.
// Fail closed if a signature is missing/ambiguous or a trampoline cannot be placed nearby.
std::atomic<uintptr_t> capturedCharacter{0};
bool characterHookReady=false,visualTimeHookReady=false;
static bool installDirectTrampoline(uintptr_t module,const unsigned char* signature,size_t signatureSize,
                                    size_t stolen,const std::vector<unsigned char>& prefix,
                                    const char* label){
    uintptr_t target=findUniqueCtPattern(module,signature,signatureSize);
    if(!target){logMessage(label);return false;}
    for(const auto& h:ctInstalled)
        if(target<h.address+h.size&&h.address<target+stolen)return false;
    auto* cave=(unsigned char*)allocateNear(target,256);
    if(!cave){logMessage("Live slider: nearby allocation failed");return false;}
    std::vector<unsigned char> code=prefix;
    // prefix ends with the original instruction's replacement; jump to after stolen bytes.
    int64_t delta=(int64_t)(target+stolen)-(int64_t)((uintptr_t)cave+code.size()+5);
    if(delta<INT32_MIN||delta>INT32_MAX){VirtualFree(cave,0,MEM_RELEASE);return false;}
    code.push_back(0xE9);int32_t disp=(int32_t)delta;
    appendBytes(code,(unsigned char*)&disp,4);
    unsigned char patch[16]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){
        VirtualFree(cave,0,MEM_RELEASE);return false;
    }
    for(size_t i=5;i<stolen;i++)patch[i]=0x90;
    memcpy(cave,code.data(),code.size());FlushInstructionCache(GetCurrentProcess(),cave,code.size());
    DWORD old{};
    if(!VirtualProtect((void*)target,stolen,PAGE_EXECUTE_READWRITE,&old)){
        VirtualFree(cave,0,MEM_RELEASE);return false;
    }
    CtHookRecord record{};record.address=target;record.size=stolen;record.cave=cave;
    memcpy(record.original,(void*)target,stolen);
    memcpy((void*)target,patch,stolen);FlushInstructionCache(GetCurrentProcess(),(void*)target,stolen);
    DWORD ignored{};VirtualProtect((void*)target,stolen,old,&ignored);
    ctInstalled.push_back(record);logMessage(label);return true;
}
void installLiveTimeControls(uintptr_t module){
    // CT Character Pointer: mov rax,[rsi+390]; capture RSI, then replay instruction.
    const unsigned char characterSig[]={0x48,0x8B,0x86,0x90,0x03,0x00,0x00,0x48,0x89};
    std::vector<unsigned char> characterCode={0x50,0x48,0xB8}; // push rax; mov rax,imm64
    uintptr_t characterAddress=(uintptr_t)&capturedCharacter;
    appendBytes(characterCode,(unsigned char*)&characterAddress,8);
    const unsigned char capture[]={0x48,0x89,0x30,0x58,0x48,0x8B,0x86,0x90,0x03,0x00,0x00};
    appendBytes(characterCode,capture,sizeof(capture));
    characterHookReady=installDirectTrampoline(module,characterSig,sizeof(characterSig),7,characterCode,
                                              "Character Pointer hook installed (Time Dilation +C8)");
    // CT Set Visual Day Time: movss [rbx+120],xmm0; preserve original write when OFF.
    // A conditional branch in the trampoline selects the original store or the slider float.
    const unsigned char visualSig[]={0xF3,0x0F,0x11,0x83,0x20,0x01,0x00,0x00,
                                     0x48,0x83,0xC4,0x20,0x5B,0xC3};
    std::vector<unsigned char> visualCode={0x50,0x48,0xB8}; // push rax; mov rax, lock flag
    uintptr_t lockAddr=(uintptr_t)&visualTimeLock;
    appendBytes(visualCode,(unsigned char*)&lockAddr,8);
    // cmp byte ptr [rax],0 ; je originalStore (relative 8-bit patched below)
    const unsigned char check[]={0x80,0x38,0x00,0x74,0x00};
    appendBytes(visualCode,check,sizeof(check));size_t branch=visualCode.size()-1;
    // mov rax, &visualTimeValue ; mov eax,[rax] ; mov [rbx+120],eax ; jmp done
    const unsigned char valueOp[]={0x48,0xB8};appendBytes(visualCode,valueOp,sizeof(valueOp));
    uintptr_t valueAddr=(uintptr_t)&visualTimeValue;
    appendBytes(visualCode,(unsigned char*)&valueAddr,8);
    const unsigned char store[]={0x8B,0x00,0x89,0x83,0x20,0x01,0x00,0x00,0xEB,0x00};
    appendBytes(visualCode,store,sizeof(store));size_t skip=visualCode.size()-1;
    size_t originalAt=visualCode.size();
    appendBytes(visualCode,visualSig,8); // replay original movss when lock OFF
    size_t done=visualCode.size();visualCode.push_back(0x58); // pop rax
    if(originalAt-(branch+1)<=127 && done-(skip+1)<=127){
        visualCode[branch]=(unsigned char)(originalAt-(branch+1));
        visualCode[skip]=(unsigned char)(done-(skip+1));
        visualTimeHookReady=installDirectTrampoline(module,visualSig,sizeof(visualSig),8,visualCode,
                                                   "Visual Day Time hook installed (slider enabled by lock)");
    }
}
void applyCharacterTimeDilation(){
    if(!characterHookReady)return;
    uintptr_t base=capturedCharacter.load(std::memory_order_relaxed);
    if(!base||base>UINTPTR_MAX-0xC8)return;
    float* field=(float*)(base+0xC8);
    if(!accessible(field,sizeof(float)))return;
    const float requested=characterTimeDilation.load(std::memory_order_relaxed);
    if(requested>=0.1f&&requested<=3.0f&&std::isfinite(requested)){
        // Only write after the pointer has been captured by the game's own code.
        *field=requested;
    }
}

// Literal AOBs and effects from the user-supplied SoD2.CT. All hooks are
// installed inert (OFF), and only enabled by the corresponding ADREENA toggle.
void installCtFeatures(uintptr_t module){
    static const unsigned char dur1[]={0xF3,0x41,0x0F,0x10,0x86,0x9C,0x16,0x00,0x00};
    static const unsigned char dur2[]={0xF3,0x41,0x0F,0x10,0x46,0x60,0xF3,0x44};
    static const unsigned char fatigue[]={0xF3,0x44,0x0F,0x10,0xA7,0x78,0x04,0x00,0x00};
    static const unsigned char recoil1[]={0xF3,0x44,0x0F,0x59,0x83,0x24,0x01,0x00,0x00};
    static const unsigned char recoil2[]={0xF3,0x0F,0x59,0x83,0x24,0x01,0x00,0x00,0x40};
    static const unsigned char resources[]={0xF3,0x0F,0x10,0x81,0xF8,0x00,0x00,0x00,0xC3};
    static const unsigned char fuel[]={0xF3,0x0F,0x10,0x81,0x78,0x04,0x00,0x00};
    // mov dword ptr [r14+169C], 100.0f / [r14+60], 100.0f
    static const unsigned char dur1On[]={0x41,0xC7,0x86,0x9C,0x16,0x00,0x00,0x00,0x00,0xC8,0x42};
    static const unsigned char dur2On[]={0x41,0xC7,0x46,0x60,0x00,0x00,0xC8,0x42};
    static const unsigned char fatigueOn[]={0xC7,0x87,0x78,0x04,0x00,0x00,0,0,0,0};
    static const unsigned char recoil1On[]={0x45,0x0F,0x57,0xC0}; // xorps xmm8,xmm8
    static const unsigned char recoil2On[]={0x0F,0x57,0xC0};      // xorps xmm0,xmm0
    static const unsigned char resourcesOn[]={0xC7,0x81,0xF8,0x00,0x00,0x00,0x00,0x24,0x74,0x49}; // 1000000.0f
    static const unsigned char fuelOn[]={0xC7,0x81,0x78,0x04,0x00,0x00,0x00,0x00,0xC8,0x42};
    // Vehicle Fuel Lock: freeze at the current value. First read of a fuel object latches its value; later reads put that value back.
    static unsigned char fuelLatch[96];size_t fuelN=0;
    {
        auto put=[&](std::initializer_list<unsigned char> b){for(unsigned char c:b)fuelLatch[fuelN++]=c;};
        auto put64=[&](uintptr_t v){memcpy(fuelLatch+fuelN,&v,8);fuelN+=8;};
        const uintptr_t aObj=(uintptr_t)&fuelLatchObj,aVal=(uintptr_t)&fuelLatchVal;
        put({0x9C,0x50,0x53});                       // pushfq; push rax; push rbx
        put({0x48,0xB8});put64(aObj);                // mov rax,&latchObj
        put({0x48,0x8B,0x18});                       // mov rbx,[rax]
        put({0x48,0x39,0xCB});                       // cmp rbx,rcx
        put({0x74,0x00});const size_t jeAt=fuelN-1;     // je restore
        put({0x48,0x89,0x08});                       // mov [rax],rcx
        put({0x8B,0x99,0x78,0x04,0x00,0x00});        // mov ebx,[rcx+478]
        put({0x48,0xB8});put64(aVal);                // mov rax,&latchVal
        put({0x89,0x18});                            // mov [rax],ebx
        put({0xEB,0x00});const size_t jmpAt=fuelN-1;    // jmp done
        const size_t restoreAt=fuelN;
        put({0x48,0xB8});put64(aVal);                // restore: mov rax,&latchVal
        put({0x8B,0x18});                            // mov ebx,[rax]
        put({0x89,0x99,0x78,0x04,0x00,0x00});        // mov [rcx+478],ebx
        const size_t doneAt=fuelN;
        put({0x5B,0x58,0x9D});                       // done: pop rbx; pop rax; popfq
        fuelLatch[jeAt]=(unsigned char)(restoreAt-(jeAt+1));
        fuelLatch[jmpAt]=(unsigned char)(doneAt-(jmpAt+1));
    }
    const CtHookSpec specs[]={
        {"CT durability hook 1",dur1,sizeof(dur1),9,dur1On,sizeof(dur1On),false,&durHookFallback},
        {"CT durability hook 2",dur2,sizeof(dur2),6,dur2On,sizeof(dur2On),false,&durHookFallback},
        {"CT fatigue hook",fatigue,sizeof(fatigue),9,fatigueOn,sizeof(fatigueOn),false,&noFatigue},
        {"CT recoil hook 1",recoil1,sizeof(recoil1),9,recoil1On,sizeof(recoil1On),true,&noRecoil},
        {"CT recoil hook 2",recoil2,sizeof(recoil2),8,recoil2On,sizeof(recoil2On),true,&noRecoil},
        {"CT resources hook",resources,sizeof(resources),8,resourcesOn,sizeof(resourcesOn),false,&infiniteResources},
        {"CT fuel hook",fuel,sizeof(fuel),8,fuelLatch,fuelN,false,&infiniteCarFuel},
    };
    // Do not expose partially installed multi-hook features as active.
    bool ok[7]{};
    for(int i=0;i<7;i++)ok[i]=installCtHook(module,specs[i]);
    ctFeatureReady[0]=ok[0]&&ok[1];
    ctFeatureReady[1]=ok[2];
    ctFeatureReady[2]=ok[3]&&ok[4];
    ctFeatureReady[3]=false; // Retired No Spread ID; Perfect Accuracy is the sole accuracy hook.
    ctFeatureReady[4]=ok[5];
    ctFeatureReady[5]=ok[6];
    // No Sway: CT steam_u37_2 script, +677C7F. Install inert until toggled.
    // Scan the CT's wildcard signature and verify the actual 8-byte instruction.
    static const int swaySig[]={0xF3,0x0F,-1,-1,-1,-1,-1,-1,
        0xF3,0x0F,-1,-1,0xF3,0x0F,-1,-1,0xF3,0x0F,-1,-1,
        0xF3,0x0F,-1,-1,-1,-1,-1,-1,0xF3,0x0F,-1,-1,
        0xF3,0x41,-1,-1,-1,0x44,0x0F};
    static const unsigned char swayOrig[]={0xF3,0x0F,0x59,0xB1,0xE4,0x00,0x00,0x00};
    static const unsigned char swayOn[]={0xC7,0x81,0xE4,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    uintptr_t swaySite=findMasked(module,swaySig);
    if(swaySite && memcmp((void*)swaySite,swayOrig,sizeof(swayOrig))==0){
        const CtHookSpec sway={"No Sway hook",swayOrig,sizeof(swayOrig),8,swayOn,sizeof(swayOn),false,&noSway};
        swayHookReady=installCtHook(module,sway,swaySite);
    }
    logMessage(swayHookReady?"No Sway: hook ready":"No Sway: signature missing, ambiguous, or hook unavailable");
}

bool relativeJump(unsigned char* out,uintptr_t from,uintptr_t to){
    auto displacement=(int64_t)to-(int64_t)(from+5);
    if(displacement<INT32_MIN||displacement>INT32_MAX)return false;
    out[0]=0xE9;int32_t rel=(int32_t)displacement;
    memcpy(out+1,&rel,4);return true;
}

// EXPERIMENTAL: zero the melee item-weight return value without editing item data.
// Verified current-build reader: exe+528887: movss xmm0,[rax+74] (5 bytes).
// The toggle is OFF by default. The original read runs when disabled.
constexpr uintptr_t MELEE_WEIGHT_READ_RVA=0x528887;
const unsigned char MELEE_WEIGHT_READ_ORIGINAL[5]={0xF3,0x0F,0x10,0x40,0x74};
void* meleeWeightTrampoline=nullptr;
bool meleeWeightHookInstalled=false;
bool installMeleeWeightHook(uintptr_t module){
    uintptr_t target=module+MELEE_WEIGHT_READ_RVA;
    if(!accessible((void*)target,5)||
       memcmp((void*)target,MELEE_WEIGHT_READ_ORIGINAL,5)!=0){
        logMessage("Melee weight hook skipped: current game bytes differ");return false;
    }
    auto cave=(unsigned char*)allocateNear(target,128);
    if(!cave){logMessage("Melee weight hook: nearby allocation failed");return false;}
    // Preserve GPR and flags; only xmm0 changes, just as with the original read.
    // pushfq; push rax; mov rax,&meleeWeight; cmp byte ptr [rax],0
    // je original; xorps xmm0,xmm0; jmp finish; original: movss xmm0,[rax+74]
    // finish: pop rax; popfq; jmp target+5
    unsigned char safeCode[]={
        0x9C,0x50,                          // pushfq; push rax
        0x48,0xB8,0,0,0,0,0,0,0,0,         // mov rax, &meleeWeight
        0x80,0x38,0x00,                    // cmp byte ptr [rax],0
        0x74,0x07,                         // je disabled (offset 24)
        0x58,0x9D,                         // enabled: pop rax; popfq
        0x0F,0x57,0xC0,                    // xorps xmm0,xmm0
        0xEB,0x09,                         // jmp return jump (offset 33)
        0x58,0x9D,                         // disabled: pop rax; popfq
        0xF3,0x0F,0x10,0x40,0x74,         // movss xmm0,[rax+74]
        0x90,0x90,                         // padding
        0xE9,0,0,0,0                      // jmp target+5
    };
    uintptr_t flag=(uintptr_t)&meleeWeight;
    memcpy(safeCode+4,&flag,8);
    if(!relativeJump(safeCode+33,(uintptr_t)cave+33,target+5)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Melee weight hook: return jump out of range");return false;
    }
    unsigned char patch[5]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Melee weight hook: detour out of range");return false;
    }
    memcpy(cave,safeCode,sizeof(safeCode));FlushInstructionCache(GetCurrentProcess(),cave,sizeof(safeCode));
    DWORD old{};
    if(!VirtualProtect((void*)target,5,PAGE_EXECUTE_READWRITE,&old)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Melee weight hook: protection change failed");return false;
    }
    memcpy((void*)target,patch,5);FlushInstructionCache(GetCurrentProcess(),(void*)target,5);
    DWORD ignored{};VirtualProtect((void*)target,5,old,&ignored);
    meleeWeightTrampoline=cave;meleeWeightHookInstalled=true;
    logMessage("Melee weight hook installed (experimental +528887)");return true;
}

bool installStaminaHook(uintptr_t module){
    uintptr_t target=module+STAMINA_WRITE_RVA;
    if(!accessible((void*)target,5)||memcmp((void*)target,STAMINA_WRITE_ORIGINAL,5)!=0){
        logMessage("Stamina hook skipped: current game bytes differ");return false;
    }
    auto cave=(unsigned char*)allocateNear(target,128);
    if(!cave){logMessage("Stamina hook: nearby trampoline allocation failed");return false;}
    // mov rax, &stamina; cmp byte ptr [rax],0; je +5
    // movss xmm6,[rbx+0C] (max); original movss [rbx+08],xmm6; jmp return
    unsigned char code[]={
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0x80,0x38,0x00,
        0x74,0x05,
        0xF3,0x0F,0x10,0x73,0x0C,
        0xF3,0x0F,0x11,0x73,0x08,
        0xE9,0,0,0,0
    };
    uintptr_t flag=(uintptr_t)&stamina;
    memcpy(code+2,&flag,8);
    if(!relativeJump(code+25,(uintptr_t)cave+25,target+5)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Stamina hook: return jump out of range");return false;
    }
    unsigned char patch[5]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Stamina hook: detour jump out of range");return false;
    }
    memcpy(cave,code,sizeof(code));FlushInstructionCache(GetCurrentProcess(),cave,sizeof(code));
    DWORD old{};
    if(!VirtualProtect((void*)target,5,PAGE_EXECUTE_READWRITE,&old)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Stamina hook: protection change failed");return false;
    }
    memcpy((void*)target,patch,5);FlushInstructionCache(GetCurrentProcess(),(void*)target,5);
    DWORD ignored{};VirtualProtect((void*)target,5,old,&ignored);
    staminaTrampoline=cave;staminaHookInstalled=true;
    logMessage("Stamina hook installed (current build; max value at +0C)");return true;
}

// Current-build health writer hook, modeled after the working stamina hook.
// exe+1B9619: F3 0F 11 49 08 = movss [rcx+08],xmm1
constexpr uintptr_t HEALTH_WRITE_RVA=0x1B9619;
const unsigned char HEALTH_WRITE_ORIGINAL[5]={0xF3,0x0F,0x11,0x49,0x08};
void* healthTrampoline=nullptr;
bool healthHookInstalled=false;
bool installHealthHook(uintptr_t module){
    uintptr_t target=module+HEALTH_WRITE_RVA;
    if(!accessible((void*)target,5)||memcmp((void*)target,HEALTH_WRITE_ORIGINAL,5)!=0){
        logMessage("Health hook skipped: current game bytes differ");return false;
    }
    auto cave=(unsigned char*)allocateNear(target,128);
    if(!cave){logMessage("Health hook: nearby trampoline allocation failed");return false;}
    // mov rax,&health; cmp byte ptr [rax],0; je +5
    // enabled: movss xmm1,[rcx+0C] (presumed maximum)
    // original: movss [rcx+08],xmm1; jmp back
    unsigned char code[]={
        0x48,0xB8,0,0,0,0,0,0,0,0,
        0x80,0x38,0x00,
        0x74,0x05,
        0xF3,0x0F,0x10,0x49,0x0C,
        0xF3,0x0F,0x11,0x49,0x08,
        0xE9,0,0,0,0
    };
    uintptr_t flag=(uintptr_t)&health;
    memcpy(code+2,&flag,8);
    if(!relativeJump(code+25,(uintptr_t)cave+25,target+5)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Health hook: return jump out of range");return false;
    }
    unsigned char patch[5]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Health hook: detour jump out of range");return false;
    }
    memcpy(cave,code,sizeof(code));FlushInstructionCache(GetCurrentProcess(),cave,sizeof(code));
    DWORD old{};
    if(!VirtualProtect((void*)target,5,PAGE_EXECUTE_READWRITE,&old)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Health hook: protection change failed");return false;
    }
    memcpy((void*)target,patch,5);FlushInstructionCache(GetCurrentProcess(),(void*)target,5);
    DWORD ignored{};VirtualProtect((void*)target,5,old,&ignored);
    healthTrampoline=cave;healthHookInstalled=true;
    logMessage("Health hook installed (current build; presumed max at +0C)");return true;
}

// Current-build ammo writer verified by CE: one write per shot.
// exe+4FF1A7: 89 41 50 = mov [rcx+50],eax
// Next complete instruction: 48 83 C1 58 = add rcx,58
// The detour replaces BOTH instructions (7 bytes) to avoid splitting one.
constexpr uintptr_t CLIP_WRITE_RVA=0x4FF1A7;
const unsigned char CLIP_WRITE_ORIGINAL[7]={0x89,0x41,0x50,0x48,0x83,0xC1,0x58};
void* clipTrampoline=nullptr;
bool clipHookInstalled=false;

bool installClipHook(uintptr_t module){
    uintptr_t target=module+CLIP_WRITE_RVA;
    if(!accessible((void*)target,sizeof(CLIP_WRITE_ORIGINAL)) ||
       memcmp((void*)target,CLIP_WRITE_ORIGINAL,sizeof(CLIP_WRITE_ORIGINAL))!=0){
        logMessage("Clip hook skipped: current-build ammo writer bytes differ");return false;
    }
    auto cave=(unsigned char*)allocateNear(target,128);
    if(!cave){logMessage("Clip hook: nearby trampoline allocation failed");return false;}
    // Preserve RAX and incoming RFLAGS during toggle check.
    // Enabled: skip ammo write, execute original add rcx,58.
    // Disabled: execute both original instructions.
    // This does not distinguish shooting from reloading/other ammo operations.
    unsigned char code[]={
        0x9C,0x50,                         // pushfq; push rax
        0x48,0xB8,0,0,0,0,0,0,0,0,       // mov rax, &clip
        0x80,0x38,0x00,                   // cmp byte ptr [rax],0
        0x58,                             // pop rax
        0x74,0x0A,                        // je disabled (+10)
        0x9D,                             // enabled: popfq
        0x48,0x83,0xC1,0x58,              // add rcx,58
        0xE9,0,0,0,0,                    // jmp return
        0x9D,                             // disabled: popfq
        0x89,0x41,0x50,                   // mov [rcx+50],eax
        0x48,0x83,0xC1,0x58,              // add rcx,58
        0xE9,0,0,0,0                     // jmp return
    };
    uintptr_t flag=(uintptr_t)&clip;memcpy(code+4,&flag,8);
    // Verify the short branch lands on the disabled path.
    static_assert(sizeof(code)==41,"Unexpected clip trampoline layout");
    if(!relativeJump(code+23,(uintptr_t)cave+23,target+7)||
       !relativeJump(code+36,(uintptr_t)cave+36,target+7)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Clip hook: return jump out of range");return false;
    }
    unsigned char patch[7]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Clip hook: detour jump out of range");return false;
    }
    patch[5]=0x90;patch[6]=0x90;
    memcpy(cave,code,sizeof(code));FlushInstructionCache(GetCurrentProcess(),cave,sizeof(code));
    DWORD old{};
    if(!VirtualProtect((void*)target,sizeof(patch),PAGE_EXECUTE_READWRITE,&old)){
        VirtualFree(cave,0,MEM_RELEASE);logMessage("Clip hook: protection change failed");return false;
    }
    memcpy((void*)target,patch,sizeof(patch));FlushInstructionCache(GetCurrentProcess(),(void*)target,sizeof(patch));
    DWORD ignored{};VirtualProtect((void*)target,sizeof(patch),old,&ignored);
    clipTrampoline=cave;clipHookInstalled=true;
    logMessage("Clip hook installed (current build +4FF1A7; freeze clip writes)");return true;
}


// Infinite Ammo is intentionally unavailable. The experimental reserve-ammo
// writer hook was removed pending a verified, inventory-safe implementation.

void releaseRenderer(){
    uiCacheValid=false;
    graphicsReady=false;showMenu=false;
    drop(standaloneBannerView);bannerLoadedRevision=-1;
    drop(cursorView);drop(cursorTexture);drop(bannerListView);drop(bannerListTexture);drop(uiView);drop(uiTexture);drop(vertexBuffer);drop(inputLayout);
    drop(vertexShader);drop(pixelShader);drop(sampler);drop(blendState);drop(depthState);drop(rasterState);
    drop(activeSwap);drop(context);drop(device);
}
struct Vertex {float x,y,u,v;};
static const char* shaderSource=R"(
struct VSIn { float2 pos:POSITION; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
VSOut VSMain(VSIn v) { VSOut o; o.pos=float4(v.pos,0,1); o.uv=v.uv; return o; }
Texture2D tex:register(t0); SamplerState samp:register(s0);
float4 PSMain(VSOut i):SV_TARGET { return tex.Sample(samp,i.uv); }
)";
HRESULT compile(const char* entry,const char* profile,ID3DBlob** out){
    ID3DBlob* errors=nullptr;
    HRESULT hr=D3DCompile(shaderSource,strlen(shaderSource),nullptr,nullptr,nullptr,entry,profile,0,0,out,&errors);
    if(errors){if(FAILED(hr))logMessage("Shader compilation failed",hr);errors->Release();}
    return hr;
}
bool setupRenderer(IDXGISwapChain* swap){
    releaseRenderer();
    HRESULT hr=swap->GetDevice(__uuidof(ID3D11Device),(void**)&device);
    if(FAILED(hr)){logMessage("Swapchain is not D3D11",hr);return false;}
    device->GetImmediateContext(&context);
    ID3DBlob *vs=nullptr,*ps=nullptr;
    hr=compile("VSMain","vs_4_0",&vs);
    if(SUCCEEDED(hr))hr=compile("PSMain","ps_4_0",&ps);
    if(SUCCEEDED(hr))hr=device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertexShader);
    if(SUCCEEDED(hr))hr=device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixelShader);
    D3D11_INPUT_ELEMENT_DESC elements[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if(SUCCEEDED(hr))hr=device->CreateInputLayout(elements,2,vs->GetBufferPointer(),vs->GetBufferSize(),&inputLayout);
    drop(vs);drop(ps);
    D3D11_BUFFER_DESC vb{};vb.ByteWidth=sizeof(Vertex)*6;vb.Usage=D3D11_USAGE_DYNAMIC;vb.BindFlags=D3D11_BIND_VERTEX_BUFFER;vb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if(SUCCEEDED(hr))hr=device->CreateBuffer(&vb,nullptr,&vertexBuffer);
    // The menu texture is created lazily by ensureUiTexture() because its size depends on the game resolution.
    // Independent cursor texture: draw over the entire game viewport, not inside the menu texture.
    // 16x22 white arrow with a dark outline, transparent background.
    if(SUCCEEDED(hr)){
        constexpr int CW=16,CH=22;
        uint32_t pixels[CW*CH]{};
        // Full-width arrow silhouette, with a solid black outline and white interior.
        // The previous x<=y/2 shape was only about 8 pixels wide and looked cut in half.
        static const char* arrow[CH]={
            "X...............",
            "XX..............",
            "XOX.............",
            "XOOX............",
            "XOOOX...........",
            "XOOOOX..........",
            "XOOOOOX.........",
            "XOOOOOOX........",
            "XOOOOOOOX.......",
            "XOOOOOOOOX......",
            "XOOOOOOOOOX.....",
            "XOOOOOOOOOOX....",
            "XOOOOOOOOOOOX...",
            "XOOOOOOOOOOOOX..",
            "XOOOOOOXXXXXXX..",
            "XOOOXOOX........",
            "XOOX.XOOX.......",
            "XXX..XOOX.......",
            ".....XOOX.......",
            "......XX........",
            "................",
            "................"
        };
        for(int y=0;y<CH;y++)for(int x=0;x<CW;x++){
            char px=arrow[y][x];
            pixels[y*CW+x]=(px=='X')?0xFF101014u:(px=='O'?0xFFFFFFFFu:0u);
        }
        D3D11_TEXTURE2D_DESC cd{};cd.Width=CW;cd.Height=CH;cd.MipLevels=1;cd.ArraySize=1;
        cd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;cd.SampleDesc.Count=1;
        cd.Usage=D3D11_USAGE_IMMUTABLE;cd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=pixels;initial.SysMemPitch=CW*4;
        hr=device->CreateTexture2D(&cd,&initial,&cursorTexture);
        if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(cursorTexture,nullptr,&cursorView);
    }
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    if(SUCCEEDED(hr))hr=device->CreateSamplerState(&sd,&sampler);
    D3D11_BLEND_DESC bd{};auto& rt=bd.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if(SUCCEEDED(hr))hr=device->CreateBlendState(&bd,&blendState);
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;dd.StencilEnable=FALSE;
    if(SUCCEEDED(hr))hr=device->CreateDepthStencilState(&dd,&depthState);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=FALSE;
    if(SUCCEEDED(hr))hr=device->CreateRasterizerState(&rd,&rasterState);
    if(FAILED(hr)){logMessage("D3D11 renderer initialization failed",hr);releaseRenderer();return false;}
    activeSwap=swap;activeSwap->AddRef();graphicsReady=true;
    logMessage("D3D11 renderer initialized successfully");return true;
}
// Pick the UI scale for a back buffer of the given size (resolution-aware, works the same for every user).
// Reference: the original look, 680x520 logical units at 1.10x = 748x572 pixels on a 1920x1080 desktop (windowed).
// Goal: the menu covers the same share of the MONITOR at any resolution (1080p 1.10x, 1440p ~1.47x, 4K ~2.2x),
// in windowed, borderless and exclusive fullscreen:
//   desktopH  = the monitor's desktop height (its saved mode; exclusive fullscreen does not change it)
//   shownH    = how many desktop pixels tall the back buffer appears on the monitor:
//               the window's client height, stretched by desktop/current display mode when exclusive fullscreen
//               switched the monitor to another mode (e.g. 1280x720 on a 1080p monitor shows at 1.5x)
//   scale     = 1.10 * (desktopH / 1080) * (bbH / shownH)
// Then capped so the whole menu fits inside the game's picture (small displays / small windows).
// Re-checked only when the back buffer or window size changes (resize, mode switch); logged on change.
float pickUiScale(UINT bbW,UINT bbH){
    constexpr float BASE_SCALE=1.10f,REF_HEIGHT=1080.f,MIN_SCALE=0.50f;
    static float cached=BASE_SCALE;static UINT lastW=0,lastH=0;static int lastCW=-1,lastCH=-1;
    if(!gameWindow||!bbW||!bbH){cached=BASE_SCALE;return cached;}
    RECT cr{};GetClientRect(gameWindow,&cr);   // cheap window query; the display driver is only asked when a size changes
    const int clientW=cr.right-cr.left,clientH=cr.bottom-cr.top;
    if(clientW<=0||clientH<=0)return cached;   // minimized: keep the current size
    if(bbW==lastW&&bbH==lastH&&clientW==lastCW&&clientH==lastCH)return cached;
    lastW=bbW;lastH=bbH;lastCW=clientW;lastCH=clientH;
    MONITORINFOEXW mi{};mi.cbSize=sizeof(mi);
    DEVMODEW cur{},desk{};cur.dmSize=desk.dmSize=sizeof(DEVMODEW);
    bool haveCur=false,haveDesk=false;
    if(GetMonitorInfoW(MonitorFromWindow(gameWindow,MONITOR_DEFAULTTONEAREST),&mi)){
        haveCur=EnumDisplaySettingsW(mi.szDevice,ENUM_CURRENT_SETTINGS,&cur)!=FALSE&&cur.dmPelsHeight>0;
        haveDesk=EnumDisplaySettingsW(mi.szDevice,ENUM_REGISTRY_SETTINGS,&desk)!=FALSE&&desk.dmPelsHeight>0;
    }
    const float desktopH=haveDesk?(float)desk.dmPelsHeight:(haveCur?(float)cur.dmPelsHeight:REF_HEIGHT);
    const bool modeChanged=haveCur&&haveDesk&&cur.dmPelsHeight!=desk.dmPelsHeight;
    const float shownH=modeChanged?(float)clientH*desktopH/(float)cur.dmPelsHeight:(float)clientH;
    float scale=BASE_SCALE*(desktopH/REF_HEIGHT)*((float)bbH/shownH);
    const float fitScale=(std::min)(((float)bbW-8.f)/(float)MW,((float)bbH-8.f)/(float)MH);   // whole menu must fit
    const bool capped=scale>fitScale;
    if(capped)scale=fitScale;
    if(scale<MIN_SCALE)scale=MIN_SCALE;
    scale=roundf(scale*100.f)/100.f;   // stable steps (1080p stays exactly 1.10)
    static std::string lastLine;
    char m[480];
    snprintf(m,sizeof(m),"Menu scale %.2f%s: back buffer %ux%u, window client %dx%d, display mode %lux%lu, desktop %lux%lu (%s)",
        scale,capped?" (capped to fit)":"",bbW,bbH,clientW,clientH,
        haveCur?cur.dmPelsWidth:0UL,haveCur?cur.dmPelsHeight:0UL,haveDesk?desk.dmPelsWidth:0UL,haveDesk?desk.dmPelsHeight:0UL,
        modeChanged?"exclusive fullscreen, display mode differs from desktop":"windowed/borderless");
    if(lastLine!=m){lastLine=m;logMessage(m);}
    cached=scale;return cached;
}
// (Re)create the menu texture when the scale changes. Texture pixels == on-screen pixels.
bool ensureUiTexture(float scale){
    int w=(int)lroundf(MW*scale),h=(int)lroundf(MH*scale);
    if(uiTexture&&uiView&&w==TW&&h==TH)return true;
    drop(uiView);drop(uiTexture);
    D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DYNAMIC;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    HRESULT hr=device->CreateTexture2D(&td,nullptr,&uiTexture);
    if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(uiTexture,nullptr,&uiView);
    if(FAILED(hr)){
        drop(uiView);drop(uiTexture);
        static ULONGLONG lastLog=0;ULONGLONG now=GetTickCount64();
        if(now-lastLog>5000){lastLog=now;logMessage("UI texture creation failed",hr);}
        return false;
    }
    uiScale=scale;TW=w;TH=h;uiCacheValid=false;
    return true;
}
// Geometric pens follow the GDI world transform (a width-1 cosmetic pen would stay 1 device pixel).
HPEN mkPen(int width,COLORREF color,bool flatCaps=false){
    LOGBRUSH lb{};lb.lbStyle=BS_SOLID;lb.lbColor=color;
    HPEN p=ExtCreatePen(PS_GEOMETRIC|PS_SOLID|(flatCaps?PS_ENDCAP_FLAT:PS_ENDCAP_ROUND)|PS_JOIN_ROUND,(DWORD)width,&lb,0,nullptr);
    return p?p:CreatePen(PS_SOLID,width,color);
}
void rectPixels(uint32_t* pixels,int x,int y,int w,int h,uint32_t rgba){
    for(int yy=(y<0?0:y);yy<y+h&&yy<MH;yy++)for(int xx=(x<0?0:x);xx<x+w&&xx<MW;xx++)pixels[yy*MW+xx]=rgba;
}
// DirectWrite text rendered into the existing 680x520 GDI-compatible surface.
// Keeps the proven texture dimensions, layout coordinates and gameplay code intact.
// If Direct2D initialization or a particular draw fails, fall back to GDI.
ID2D1Factory* textD2dFactory=nullptr;
IDWriteFactory* textWriteFactory=nullptr;
ID2D1DCRenderTarget* textDcTarget=nullptr;
IDWriteTextFormat* textFormats[3]{};
float textFormatScale=0.0f;
bool textRendererTried=false;
// Fonts are created at their FINAL pixel size (logical size * uiScale). Direct2D itself runs with an
// identity transform, so the only scaling that happens is baked into the font size and the rectangles.
void buildTextFormats(float scale){
    if(!textWriteFactory)return;
    for(auto& f:textFormats)drop(f);
    const FLOAT sizes[]={14.0f,13.0f,18.0f};
    const DWRITE_FONT_WEIGHT weights[]={DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_WEIGHT_SEMI_BOLD};
    for(int i=0;i<3;i++){
        if(FAILED(textWriteFactory->CreateTextFormat(L"Segoe UI",nullptr,weights[i],
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,sizes[i]*scale,L"en-us",&textFormats[i])))continue;
        textFormats[i]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormats[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    textFormatScale=scale;
}
void initTextRenderer(){
    if(textRendererTried)return;
    textRendererTried=true;
    if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&textD2dFactory)))return;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown**)&textWriteFactory)))return;
    D2D1_RENDER_TARGET_PROPERTIES props=D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),
        96.0f,96.0f,D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
    if(FAILED(textD2dFactory->CreateDCRenderTarget(&props,&textDcTarget)))return;
    buildTextFormats(uiScale);
}
void shutdownTextRenderer(){
    for(auto& f:textFormats)drop(f);
    drop(textDcTarget);drop(textWriteFactory);drop(textD2dFactory);
    textRendererTried=false;
}
HFONT uiNormalFont=nullptr,uiSmallFont=nullptr,uiHeadingFont=nullptr;
// DirectWrite renders with an identity transform; track Settings scroll separately.
int settingsTextScrollOffset=0;
float textXFrac=0.f; // sub-pixel horizontal shift (logical units) for smooth scrolling text; normally 0
void textGdi(HDC dc,HFONT font,const wchar_t* str,int x,int y,int w,int h,COLORREF rgb){
    // Direct2D's BindDC establishes its own coordinate origin. Binding each
    // label's small rectangle while drawing in menu-global coordinates made
    // DrawTextW clip every glyph outside that rectangle.
    // Bind the whole menu instead; retain GDI if any Direct2D step fails.
    bool rendered=false;
    if(str&&*str&&w>0&&h>0){
        initTextRenderer();
        if(textFormatScale!=uiScale)buildTextFormats(uiScale);
        int index=font==uiSmallFont?1:(font==uiHeadingFont?2:0);
        IDWriteTextFormat* format=textFormats[index];
        if(textDcTarget&&format){
            // Direct2D's DC target reads the DC's GDI world transform, which made text scale twice
            // (once from GDI, once from us). Give Direct2D an identity DC for the call, then restore it.
            XFORM savedXf{};GetWorldTransform(dc,&savedXf);
            ModifyWorldTransform(dc,nullptr,MWT_IDENTITY);
            // Logical layout rectangle -> pixel rectangle (identity DC, so this is device space).
            const D2D1_RECT_F area=D2D1::RectF((x+textXFrac)*uiScale,(y-settingsTextScrollOffset)*uiScale,
                (x+w+textXFrac)*uiScale,(y+h-settingsTextScrollOffset)*uiScale);
            // PERFORMANCE: bind only the pixels this label can touch. BindDC/EndDraw copy the whole bound
            // rectangle in and out of the DIB, and binding the entire menu for each of ~40 labels meant
            // copying hundreds of MB per rebuild (the main cause of scroll lag). The target's origin moves to
            // the bound rectangle's top-left, so a translation keeps all drawing in menu coordinates.
            RECT bind{(LONG)floorf(area.left)-2,(LONG)floorf(area.top)-2,(LONG)ceilf(area.right)+2,(LONG)ceilf(area.bottom)+2};
            if(bind.left<0)bind.left=0;if(bind.top<0)bind.top=0;
            if(bind.right>TW)bind.right=TW;if(bind.bottom>TH)bind.bottom=TH;
            RECT clipRect{};
            const int clipType=GetClipBox(dc,&clipRect);
            bool nothingVisible=(clipType==NULLREGION);
            if(clipType!=ERROR&&clipType!=NULLREGION){ // the GDI clip is always a rectangle here; intersect with it
                if(clipRect.left>bind.left)bind.left=clipRect.left;
                if(clipRect.top>bind.top)bind.top=clipRect.top;
                if(clipRect.right<bind.right)bind.right=clipRect.right;
                if(clipRect.bottom<bind.bottom)bind.bottom=clipRect.bottom;
            }
            if(bind.right<=bind.left||bind.bottom<=bind.top)nothingVisible=true;
            if(nothingVisible){
                rendered=true; // fully clipped away: nothing to draw, and no GDI fallback either
            }else if(SUCCEEDED(textDcTarget->BindDC(dc,&bind))){
                textDcTarget->BeginDraw();
                textDcTarget->SetTransform(D2D1::Matrix3x2F::Translation((FLOAT)-bind.left,(FLOAT)-bind.top));
                textDcTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                ID2D1SolidColorBrush* brush=nullptr;
                const D2D1_COLOR_F color=D2D1::ColorF(GetRValue(rgb)/255.f,GetGValue(rgb)/255.f,GetBValue(rgb)/255.f,1.f);
                if(SUCCEEDED(textDcTarget->CreateSolidColorBrush(color,&brush))){
                    textDcTarget->DrawTextW(str,(UINT32)wcslen(str),format,area,brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP,DWRITE_MEASURING_MODE_NATURAL);
                    brush->Release();
                    rendered=SUCCEEDED(textDcTarget->EndDraw());
                }else{
                    textDcTarget->EndDraw();
                }
            }
            SetWorldTransform(dc,&savedXf);
        }
    }
    if(!rendered){
        SelectObject(dc,font);SetTextColor(dc,rgb);RECT r{x,y,x+w,y+h};
        DrawTextW(dc,str,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_NOPREFIX);
    }
}
// Word-wrapped status text (up to maxLines lines, last line ends with "..." if it still does not fit).
// Static, so it adds no per-frame redraw cost. Width is estimated from the character count.
void statusLines(HDC dc,HFONT font,const std::wstring& s,int x,int y,int w,int maxLines,COLORREF col){
    const size_t maxChars=(size_t)(std::max)(8,(int)(w/6.6f));
    std::vector<std::wstring> lines;std::wstring cur,word;
    auto flushWord=[&](){
        if(word.empty())return;
        if(!cur.empty()&&cur.size()+1+word.size()>maxChars){lines.push_back(cur);cur.clear();}
        while(word.size()>maxChars){lines.push_back(word.substr(0,maxChars));word.erase(0,maxChars);}
        if(!cur.empty())cur+=L' ';
        cur+=word;word.clear();
    };
    for(wchar_t c:s){if(c==L' ')flushWord();else word.push_back(c);}
    flushWord();if(!cur.empty())lines.push_back(cur);
    if((int)lines.size()>maxLines){
        lines.resize(maxLines);
        std::wstring& last=lines.back();
        if(last.size()+3>maxChars)last.resize(maxChars>3?maxChars-3:0);
        last+=L"...";
    }
    for(size_t i=0;i<lines.size();++i)textGdi(dc,font,lines[i].c_str(),x,y+(int)i*15,w,16,col);
}
// ---- Buttons and scrolling status text ----------------------------------------------------------
COLORREF shadeColor(COLORREF c,int pct){ // pct > 0 lightens toward white, pct < 0 darkens
    int r=GetRValue(c),g=GetGValue(c),b=GetBValue(c);
    if(pct>=0){r+=(255-r)*pct/100;g+=(255-g)*pct/100;b+=(255-b)*pct/100;}
    else{r=r*(100+pct)/100;g=g*(100+pct)/100;b=b*(100+pct)/100;}
    return RGB(r,g,b);
}
void flashButton(int id){flashId=id;flashUntil=GetTickCount64()+140;}
// A raised, bevelled button that visibly sinks for a moment after being clicked.
constexpr int BTN_RADIUS=4;   // one corner radius for every rectangular button (rectangles, not pills)
int measureTextW(HDC dc,HFONT font,const wchar_t* str){
    SIZE ts{};HGDIOBJ pf=SelectObject(dc,font);
    GetTextExtentPoint32W(dc,str,(int)wcslen(str),&ts);
    SelectObject(dc,pf);
    return ts.cx;
}
void drawButton(HDC dc,HFONT font,const RECT& r,const wchar_t* label,int id,COLORREF base,COLORREF textCol){
    const bool down=flashId==id&&GetTickCount64()<flashUntil;
    if(down)uiAnimSeen=true;
    const int clipSaved=SaveDC(dc);
    BeginPath(dc);RoundRect(dc,r.left,r.top,r.right+1,r.bottom+1,BTN_RADIUS*2,BTN_RADIUS*2);EndPath(dc);
    SelectClipPath(dc,RGN_AND);   // all bevel / face fills below are cut to the rounded silhouette
    auto fillRect=[&](LONG l,LONG t,LONG rr,LONG b,COLORREF c){RECT q{l,t,rr,b};HBRUSH br=CreateSolidBrush(c);FillRect(dc,&q,br);DeleteObject(br);};
    fillRect(r.left,r.top,r.right,r.bottom,shadeColor(base,-55));                       // outline
    const LONG l=r.left+1,t=r.top+1,rt=r.right-1,b=r.bottom-1;
    fillRect(l,t,rt,b,down?shadeColor(base,-22):base);                                 // face
    const COLORREF hi=shadeColor(base,down?-40:32),lo=shadeColor(base,down?20:-38);
    fillRect(l,t,rt,t+1,hi);fillRect(l,t,l+1,b,hi);                                    // top / left edge
    fillRect(l,b-1,rt,b,lo);fillRect(rt-1,t,rt,b,lo);                                  // bottom / right edge
    RestoreDC(dc,clipSaved);
    int tw=measureTextW(dc,font,label);   // measured, not estimated, so the label is truly centred
    int tx=r.left+((r.right-r.left)-tw)/2;if(tx<r.left+3)tx=r.left+3;
    int ty=r.top+((r.bottom-r.top)-18)/2+1;
    if(down){tx+=1;ty+=1;}
    textGdi(dc,font,label,tx,ty,r.right-tx-2,18,textCol);
}
// Trunk Size section layout, shared by drawing and clicking: 0..4 = -10, -1, value, +1, +10; 5 = Read current; 6 = Apply; 7 = on-spawn toggle.
void trunkLayout(int x,int y,int cw,RECT r[8]){
    const int x0=x+4,x1=x+cw-12,bw=28,gp=2;
    r[0]={x0,y+33,x0+bw,y+55};
    r[1]={x0+bw+gp,y+33,x0+2*bw+gp,y+55};
    r[4]={x1-bw,y+33,x1,y+55};
    r[3]={x1-2*bw-gp,y+33,x1-bw-gp,y+55};
    r[2]={x0+2*bw+2*gp,y+33,x1-2*bw-2*gp,y+55};
    const int half=(x1-x0-4)/2;
    r[5]={x0,y+58,x0+half,y+80};
    r[6]={x0+half+4,y+58,x1,y+80};
    r[7]={x0,y+83,x1,y+105};
}
// Five-part stepper row: 0,1 = left buttons, 2 = value box, 3,4 = right buttons.
void stepperRow(int x,int cw,int top,RECT r[5],int bw=38){
    const int x0=x+4,x1=x+cw-12,gp=2;   // 38: room for "-100" / "+0.1" with signs; value box shrinks to compensate
    r[0]={x0,top,x0+bw,top+22};
    r[1]={x0+bw+gp,top,x0+2*bw+gp,top+22};
    r[2]={x0+2*bw+2*gp,top,x1-2*bw-2*gp,top+22};
    r[3]={x1-2*bw-gp,top,x1-bw-gp,top+22};
    r[4]={x1-bw,top,x1,top+22};
}
// Poll after a zombie class load was requested (same idea as the vehicle poll).
void startZombiePoll(int idx){
    zombieLoadPending=idx;
    if(zombiePollActive)return;
    zombiePollActive=true;
    HWND w=gameWindow;
    std::thread([w]{
        for(int i=0;i<16;++i){
            Sleep(1500);
            {std::lock_guard<std::recursive_mutex> g(menuLock);if(zombieLoadPending<0){zombiePollActive=false;return;}}
            if(w)PostMessageW(w,WM_ADREENA_ZMASK,0,0);
        }
        std::lock_guard<std::recursive_mutex> g(menuLock);
        if(zombieLoadPending>=0){spawnStatus=L"Still not loaded - press Load to try again.";zombieLoadPending=-1;uiCacheValid=false;}
        zombiePollActive=false;
    }).detach();
}
// One-line status text. If it does not fit it scrolls slowly, pauses at both ends and loops by itself.
struct MarqueeState{std::wstring text;ULONGLONG t0=0;};
MarqueeState marqueeStates[17];
bool statusIsLoading(const std::wstring& s){return s.find(L"oading")!=std::wstring::npos;}
void statusMarquee(HDC dc,HFONT font,const std::wstring& s,int x,int y,int w,COLORREF col,int slot){
    if(statusIsLoading(s))col=RGB(238,190,64); // a pending load is shown in yellow so it is not missed (errors keep their normal colour)
    MarqueeState& m=marqueeStates[slot];
    if(m.text!=s){m.text=s;m.t0=GetTickCount64();}
    const float textW=s.size()*6.9f;
    if(textW<=(float)w){textGdi(dc,font,s.c_str(),x,y,w,18,col);return;}
    const float dist=textW-(float)w+14.f;         // how far the text must travel so its end becomes visible
    // Scrolls ONCE after the text changes (3 s pause, then the scroll), then rests with the end of the text showing.
    // It used to loop forever, which forced a full menu redraw on every game frame (hundreds of FPS lost on tabs with long status lines).
    const double speed=28.0,holdStart=3.0;
    const double travel=dist/speed;
    const double t=(double)(GetTickCount64()-m.t0)/1000.0;
    double off;
    if(t>=holdStart+travel)off=(double)dist;          // finished: static, no more redraws needed
    else{
        off=t<holdStart?0.0:(t-holdStart)*speed;
        uiMarqueeSeen=true;
        const int ms=t<holdStart?100:33;               // pause phase: 10 redraws/s, scroll phase: about 30 redraws/s
        if(ms<uiMarqueeMs)uiMarqueeMs=ms;
    }
    const int save=SaveDC(dc);
    IntersectClipRect(dc,x,y,x+w,y+18);
    const double whole=floor(off);
    textXFrac=-(float)(off-whole);                  // sub-pixel remainder, so the motion is smooth instead of 1-px steps
    textGdi(dc,font,s.c_str(),x-(int)whole,y,(int)textW+24,18,col);
    textXFrac=0.f;
    RestoreDC(dc,save);
}
// Called with menuLock held when a vehicle class load has been requested: re-check which classes are in memory
// every 1.5 s (up to 24 s) until the pending one appears.
void startLoadPoll(int idx){
    vehLoadPending=idx;
    if(vehPollActive)return;
    vehPollActive=true;
    HWND w=gameWindow;
    std::thread([w]{
        for(int i=0;i<16;++i){
            Sleep(1500);
            {std::lock_guard<std::recursive_mutex> g(menuLock);if(vehLoadPending<0){vehPollActive=false;return;}}
            if(w)PostMessageW(w,WM_ADREENA_VMASK,0,0);
        }
        std::lock_guard<std::recursive_mutex> g(menuLock);
        if(vehLoadPending>=0){vehSpawnStatus=L"Still not loaded - press Load to try again.";vehLoadPending=-1;uiCacheValid=false;}
        vehPollActive=false;
    }).detach();
}
// ---- Anti-aliased vector layer ------------------------------------------------------------------
// GDI shapes (RoundRect/Ellipse/Arc/LineTo) are never anti-aliased, so toggles and icons looked jagged next to
// DirectWrite text. Shapes are recorded in logical units and drawn in ONE Direct2D pass (one bitmap round trip).
// If Direct2D is unavailable the same commands are replayed with GDI, so the menu never loses these controls.
enum VecType{V_RRECT,V_ELLIPSE_FILL,V_ELLIPSE_STROKE,V_LINE,V_ARC_BOTTOM};
struct VecCmd{int type;float a,b,c,d,r;COLORREF col;float w;};
// a,b,c,d: RRECT/ELLIPSE/ARC use a GDI-style bounding box (left,top,right,bottom); LINE uses x1,y1,x2,y2.
// ARC_BOTTOM is the lower half of the box's ellipse, left to right (what the old GDI Arc() calls produced).
void vecRRect(std::vector<VecCmd>& v,float l,float t,float r,float b,float radius,COLORREF col){v.push_back({V_RRECT,l,t,r,b,radius,col,0});}
void vecFillEllipse(std::vector<VecCmd>& v,float l,float t,float r,float b,COLORREF col){v.push_back({V_ELLIPSE_FILL,l,t,r,b,0,col,0});}
void vecEllipse(std::vector<VecCmd>& v,float l,float t,float r,float b,float w,COLORREF col){v.push_back({V_ELLIPSE_STROKE,l,t,r,b,0,col,w});}
void vecLine(std::vector<VecCmd>& v,float x1,float y1,float x2,float y2,float w,COLORREF col){v.push_back({V_LINE,x1,y1,x2,y2,0,col,w});}
void vecArcBottom(std::vector<VecCmd>& v,float l,float t,float r,float b,float w,COLORREF col){v.push_back({V_ARC_BOTTOM,l,t,r,b,0,col,w});}
bool flushVecD2D(HDC dc,const std::vector<VecCmd>& v){
    initTextRenderer();
    if(!textDcTarget||!textD2dFactory)return false;
    XFORM saved{};GetWorldTransform(dc,&saved);
    ModifyWorldTransform(dc,nullptr,MWT_IDENTITY); // Direct2D must see an identity DC (see textGdi)
    bool ok=false;
    // Bind only the area the shapes cover, intersected with the current GDI clip. Direct2D does not see GDI
    // clip regions, so without this a toggle in a half-scrolled row was drawn outside its section.
    float minX=1e9f,minY=1e9f,maxX=-1e9f,maxY=-1e9f;
    for(const auto& c:v){
        const float pad=c.w+2.0f;
        minX=(std::min)(minX,(std::min)(c.a,c.c)-pad);maxX=(std::max)(maxX,(std::max)(c.a,c.c)+pad);
        minY=(std::min)(minY,(std::min)(c.b,c.d)-pad);maxY=(std::max)(maxY,(std::max)(c.b,c.d)+pad);
    }
    RECT bind{(LONG)floorf(minX*uiScale),(LONG)floorf(minY*uiScale),(LONG)ceilf(maxX*uiScale),(LONG)ceilf(maxY*uiScale)};
    if(bind.left<0)bind.left=0;if(bind.top<0)bind.top=0;
    if(bind.right>TW)bind.right=TW;if(bind.bottom>TH)bind.bottom=TH;
    RECT clipRect{};
    const int clipType=GetClipBox(dc,&clipRect); // identity transform is active, so this is device space
    if(clipType!=ERROR&&clipType!=NULLREGION){
        if(clipRect.left>bind.left)bind.left=clipRect.left;
        if(clipRect.top>bind.top)bind.top=clipRect.top;
        if(clipRect.right<bind.right)bind.right=clipRect.right;
        if(clipRect.bottom<bind.bottom)bind.bottom=clipRect.bottom;
    }
    if(clipType==NULLREGION||bind.right<=bind.left||bind.bottom<=bind.top){SetWorldTransform(dc,&saved);return true;}
    if(SUCCEEDED(textDcTarget->BindDC(dc,&bind))){
        textDcTarget->BeginDraw();
        // logical -> pixels, then shift so the bound rectangle's top-left is the origin
        textDcTarget->SetTransform(D2D1::Matrix3x2F::Scale(uiScale,uiScale)*D2D1::Matrix3x2F::Translation((FLOAT)-bind.left,(FLOAT)-bind.top));
        textDcTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ID2D1SolidColorBrush* brush=nullptr;
        ID2D1StrokeStyle* round=nullptr;
        bool clipped=false;
        if(SUCCEEDED(textDcTarget->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1),&brush))){
            textD2dFactory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND),nullptr,0,&round);
            RECT clipRect{};int clipType=GetClipBox(dc,&clipRect);
            clipped=clipType!=ERROR&&clipType!=NULLREGION;
            if(clipped)textDcTarget->PushAxisAlignedClip(D2D1::RectF((FLOAT)clipRect.left/uiScale,(FLOAT)clipRect.top/uiScale,(FLOAT)clipRect.right/uiScale,(FLOAT)clipRect.bottom/uiScale),D2D1_ANTIALIAS_MODE_ALIASED);
            for(const auto& c:v){
                brush->SetColor(D2D1::ColorF(GetRValue(c.col)/255.f,GetGValue(c.col)/255.f,GetBValue(c.col)/255.f,1.f));
                const float cx=(c.a+c.c)*0.5f,cy=(c.b+c.d)*0.5f,rx=(c.c-c.a)*0.5f,ry=(c.d-c.b)*0.5f;
                switch(c.type){
                case V_RRECT:
                    textDcTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.a,c.b,c.c,c.d),c.r,c.r),brush);break;
                case V_ELLIPSE_FILL:
                    textDcTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),rx,ry),brush);break;
                case V_ELLIPSE_STROKE: // pen is centered on the outline, so pull the radius in half a unit like GDI did
                    textDcTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),rx-0.5f,ry-0.5f),brush,c.w,round);break;
                case V_LINE: // GDI lines run through pixel centers
                    textDcTarget->DrawLine(D2D1::Point2F(c.a+0.5f,c.b+0.5f),D2D1::Point2F(c.c+0.5f,c.d+0.5f),brush,c.w,round);break;
                case V_ARC_BOTTOM:{
                    ID2D1PathGeometry* geo=nullptr;
                    if(SUCCEEDED(textD2dFactory->CreatePathGeometry(&geo))){
                        ID2D1GeometrySink* sink=nullptr;
                        if(SUCCEEDED(geo->Open(&sink))){
                            const float arx=rx-0.5f,ary=ry-0.5f;
                            sink->BeginFigure(D2D1::Point2F(cx-arx,cy),D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx+arx,cy),D2D1::SizeF(arx,ary),0.0f,
                                D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();sink->Release();
                            textDcTarget->DrawGeometry(geo,brush,c.w,round);
                        }
                        geo->Release();
                    }
                    break;}
                }
            }
        }
        if(clipped)textDcTarget->PopAxisAlignedClip();
        drop(round);drop(brush);
        ok=SUCCEEDED(textDcTarget->EndDraw());
    }
    SetWorldTransform(dc,&saved);
    return ok;
}
// GDI fallback for the vector layer (aliased, but keeps every control visible).
void replayVecGdi(HDC dc,const std::vector<VecCmd>& v){
    for(const auto& c:v){
        const int a=(int)c.a,b=(int)c.b,r=(int)c.c,d=(int)c.d;
        if(c.type==V_RRECT||c.type==V_ELLIPSE_FILL){
            HBRUSH br=CreateSolidBrush(c.col);HGDIOBJ ob=SelectObject(dc,br),op=SelectObject(dc,GetStockObject(NULL_PEN));
            if(c.type==V_RRECT)RoundRect(dc,a,b,r,d,(int)(c.r*2),(int)(c.r*2));else Ellipse(dc,a,b,r,d);
            SelectObject(dc,op);SelectObject(dc,ob);DeleteObject(br);
        }else{
            HPEN pen=mkPen((int)c.w,c.col);HGDIOBJ op=SelectObject(dc,pen),ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
            if(c.type==V_ELLIPSE_STROKE)Ellipse(dc,a,b,r,d);
            else if(c.type==V_LINE){MoveToEx(dc,a,b,nullptr);LineTo(dc,r,d);}
            else{int cy=(b+d)/2;Arc(dc,a,b,r,d,a,cy,r,cy);}
            SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(pen);
        }
    }
}
void drawVecLayer(HDC dc,std::vector<VecCmd>& v){
    if(v.empty())return;
    if(!flushVecD2D(dc,v))replayVecGdi(dc,v);
    v.clear();
}
bool toggleAnimationActive=false;
// Footer status from real checks. 0 = green (all installed hooks/patches healthy), 1 = yellow (an internal component
// failed to install or the spawner marked itself unusable), 2 = red (our window procedure stopped receiving messages while the
// menu is open: input hook detached). Game lag never changes the colour. Note: if ADREENA were not injected there would be no UI
// to show this in - red is only reachable for faults the injected UI can still observe.
int adreenaHealthState(std::string& why){
    const ULONGLONG hb=lastWndProcTick.load(std::memory_order_relaxed);
    if(hb&&GetTickCount64()-hb>20000){why="window procedure silent for >20 s (input hook detached?)";return 2;}
    struct Chk{const char* name;bool ok;};
    const Chk checks[]={
        {"Max Skills hook",skillHookReady},{"No Sway hook",swayHookReady},{"No Spread hook",noSpreadReady},
        {"Unlimited Weight patch",weightPatchReady},{"No Broken Bone patch",bonePatchReady},{"Melee stat hook",meleeStatsReady},
        {"CT hook group 1",ctFeatureReady[0]},{"CT hook group 2",ctFeatureReady[1]},{"CT hook group 3",ctFeatureReady[2]},
        {"CT hook group 5",ctFeatureReady[4]},{"CT hook group 6",ctFeatureReady[5]},
        {"Quick Search",extraReady[0]},{"Quick Survey",extraReady[1]},{"Zombie Radar",extraReady[2]},{"Max Fire Rate",extraReady[3]},
        {"Silent Weapons",extraReady[4]},{"Instant Construction",extraReady[5]},{"Duplicate Locker Items",extraReady[6]},
        {"No Blood Plague",plannedReady[0]},{"Instant Radio Cooldown",plannedReady[1]},{"Perfect Accuracy",plannedReady[2]},
        {"Spawner (game update?)",!spawner::dead}
    };
    for(const Chk& c:checks)if(!c.ok){why=c.name;return 1;}
    why.clear();return 0;
}
// ---- Preset thumbnail cache --------------------------------------------------------------------
// The preset PNGs are 0.1-3 MB each. They used to be decoded from disk and bicubic-resampled on EVERY texture
// rebuild (i.e. every scroll frame on the Settings tab). Now each is decoded once, downscaled to its final pixel
// size, and kept as raw pixels; the per-frame cost is a row copy. Rebuilt only if the UI scale changes.
struct PresetThumb{float scale=0.f;int tw=0,th=0,dw=0,dh=0,pw=0,ph=0;bool valid=false;std::vector<uint32_t> pixels;};
PresetThumb presetThumbs[PRESET_COUNT];
const PresetThumb* getPresetThumb(int index,int targetW,int targetH,float S){
    if(index<0||index>=PRESET_COUNT)return nullptr;
    PresetThumb& t=presetThumbs[index];
    if(t.scale==S&&t.tw==targetW&&t.th==targetH)return t.valid?&t:nullptr; // also remembers failures: no retry per frame
    t=PresetThumb{};t.scale=S;t.tw=targetW;t.th=targetH;
    Gdiplus::Bitmap src((presetFolder+officialPresets[index].file).c_str());
    if(src.GetLastStatus()!=Gdiplus::Ok)return nullptr;
    const int iw=(int)src.GetWidth(),ih=(int)src.GetHeight();
    if(iw<=0||ih<=0)return nullptr;
    const float fit=(std::min)((float)targetW/iw,(float)targetH/ih);
    t.dw=(int)lroundf(iw*fit);t.dh=(int)lroundf(ih*fit);
    t.pw=(std::max)(1,(int)lroundf(t.dw*S));t.ph=(std::max)(1,(int)lroundf(t.dh*S));
    Gdiplus::Bitmap dst(t.pw,t.ph,PixelFormat32bppARGB);
    if(dst.GetLastStatus()!=Gdiplus::Ok)return nullptr;
    {
        Gdiplus::Graphics g(&dst);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.DrawImage(&src,Gdiplus::Rect(0,0,t.pw,t.ph));
    }
    Gdiplus::BitmapData bd{};Gdiplus::Rect r(0,0,t.pw,t.ph);
    if(dst.LockBits(&r,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&bd)!=Gdiplus::Ok)return nullptr;
    t.pixels.resize((size_t)t.pw*t.ph);
    for(int y=0;y<t.ph;y++)memcpy(&t.pixels[(size_t)y*t.pw],(const uint8_t*)bd.Scan0+(ptrdiff_t)y*bd.Stride,(size_t)t.pw*4);
    dst.UnlockBits(&bd);
    t.valid=true;
    return &t;
}
bool updateTexture(){
    toggleAnimationActive=false;uiAnimSeen=false;uiMarqueeSeen=false;uiMarqueeMs=1000;lastAnimDraw=GetTickCount64();
    // Draw text and controls directly at their final pixel size (no downsample pass).
    // Layout stays in logical 680x520 units; SS scales GDI (world transform) and Direct2D (SetTransform).
    const float SS=uiScale;
    const int RW=TW, RH=TH;
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=RW;bi.bmiHeader.biHeight=-RH;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);ReleaseDC(nullptr,screen);
    if(!dc)return false;
    HBITMAP bmp=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bmp||!bits){if(bmp)DeleteObject(bmp);DeleteDC(dc);return false;}
    HGDIOBJ oldBitmap=SelectObject(dc,bmp);uint32_t* px=(uint32_t*)bits;memset(px,0,RW*RH*4);
    // One continuous charcoal surface; no separate feature-row backgrounds.
    for(int k=0;k<RW*RH;k++)px[k]=0xFF000000|(themeBg[0]<<16)|(themeBg[1]<<8)|themeBg[2];
    // The header remains transparent. The selected banner is rendered as a
    // separate native-size D3D quad above the unchanged menu panel.
    SetGraphicsMode(dc,GM_ADVANCED);
    XFORM transform{SS,0.0f,0.0f,SS,0.0f,0.0f};
    SetWorldTransform(dc,&transform);
    const auto& list=featureLists[activeCategory];
    // Resizable sidebar divider and a one-click expand/collapse control.
    HPEN dividerPen=mkPen(1,themeLineColor(),true);
    HGDIOBJ prevDivider=SelectObject(dc,dividerPen);
    MoveToEx(dc,SIDE,HEADER_H+1,nullptr);LineTo(dc,SIDE,MH-33);
    SelectObject(dc,prevDivider);DeleteObject(dividerPen);
    std::vector<VecCmd> vec; // anti-aliased shapes, drawn in drawVecLayer()
    vecRRect(vec,(float)(SIDE-23),(float)(HEADER_H+3),(float)(SIDE-3),(float)(HEADER_H+17),2.5f,themeLineColor());
    // Directional chevron on the collapse/expand button.
    {
        const COLORREF chev=themeTextColor();
        float cx=(float)(SIDE-13),cyB=(float)(HEADER_H+10);
        if(SIDE<=SIDE_MIN){vecLine(vec,cx-3,cyB-4,cx+2,cyB,2,chev);vecLine(vec,cx+2,cyB,cx-3,cyB+4,2,chev);}
        else{vecLine(vec,cx+2,cyB-4,cx-3,cyB,2,chev);vecLine(vec,cx-3,cyB,cx+2,cyB+4,2,chev);}
    }
    SetBkMode(dc,TRANSPARENT);
    HFONT normal=CreateFontW(-14,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HFONT smallFont=CreateFontW(-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HFONT heading=CreateFontW(-18,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    uiNormalFont=normal;uiSmallFont=smallFont;uiHeadingFont=heading;
    HGDIOBJ oldFont=GetCurrentObject(dc,OBJ_FONT);
    // Consistent 24-unit vector icon paths (rendered by the existing anti-aliased D2D layer).
    // No font glyphs, GDI icon approximations, or external image assets.
    auto drawSidebarIcon=[&](int category,int ix,int iy){
        const COLORREF ic=themeTextColor();
        constexpr float stroke=1.8f;
        // Scale the 24-unit artwork to the original compact sidebar icon size.
        // Keep it centered on the same 23px sidebar column in both modes.
        constexpr float iconScale=0.94f;   // was 0.72: icons now fill the collapsed-sidebar box without crowding it
        const float ox=(float)ix+12.0f, oy=(float)iy+12.0f;
        auto sx=[&](float x){return ox+(x-12.0f)*iconScale;};
        auto sy=[&](float y){return oy+(y-12.0f)*iconScale;};
        auto line=[&](float x,float y,float xx,float yy){
            vecLine(vec,sx(x),sy(y),sx(xx),sy(yy),stroke*iconScale,ic);
        };
        // Data-driven artwork: every sidebar icon is a list of polylines / ellipses on the same 24-unit grid.
        // Final approved artwork (v33 icon set), generated from the SVG paths: n>0 = polyline of n points; -1 = stroked ellipse (cx,cy,rx,ry); -2 = filled ellipse; 0 = end.
        static const float iconData0[]={-1.f,12.f,8.f,3.6f,3.6f,21.f,4.8f,20.f,4.89f,18.83f,5.16f,17.74f,5.6f,16.72f,6.18f,15.81f,6.9f,15.f,7.74f,14.31f,8.68f,13.76f,9.72f,13.34f,10.83f,13.09f,12.f,13.f,13.17f,13.09f,14.28f,13.34f,15.32f,13.76f,16.26f,14.31f,17.1f,15.f,17.82f,15.81f,18.4f,16.72f,18.84f,17.74f,19.11f,18.83f,19.2f,20.f,0.f};  // Player
        static const float iconData1[]={5.f,1.99f,14.66f,6.15f,13.07f,7.65f,15.67f,4.96f,18.61f,1.99f,14.66f,4.f,6.15f,13.07f,15.16f,7.87f,16.56f,10.29f,7.55f,15.49f,2.f,15.61f,8.65f,20.11f,6.05f,2.f,9.8f,14.19f,11.14f,18.5f,3.f,12.92f,12.39f,16.36f,15.95f,18.44f,14.75f,2.f,8.57f,10.06f,12.03f,8.06f,0.f};  // Weapons
        static const float iconData2[]={-1.f,12.f,8.2f,3.f,3.f,21.f,6.8f,19.5f,6.87f,18.65f,7.06f,17.85f,7.38f,17.1f,7.8f,16.42f,8.31f,15.81f,8.92f,15.3f,9.6f,14.88f,10.35f,14.56f,11.15f,14.37f,12.f,14.3f,12.85f,14.37f,13.65f,14.56f,14.4f,14.88f,15.08f,15.3f,15.69f,15.81f,16.2f,16.42f,16.62f,17.1f,16.94f,17.85f,17.13f,18.65f,17.2f,19.5f,-1.f,4.9f,10.3f,2.1f,2.1f,11.f,1.8f,18.4f,1.84f,17.84f,1.96f,17.32f,2.15f,16.83f,2.4f,16.38f,2.71f,15.99f,3.07f,15.65f,3.48f,15.38f,3.92f,15.17f,4.4f,15.04f,4.9f,15.f,-1.f,19.1f,10.3f,2.1f,2.1f,11.f,22.2f,18.4f,22.16f,17.84f,22.04f,17.32f,21.85f,16.83f,21.6f,16.38f,21.29f,15.99f,20.93f,15.65f,20.52f,15.38f,20.08f,15.17f,19.6f,15.04f,19.1f,15.f,0.f};  // Community
        static const float iconData3[]={-1.f,12.f,12.f,9.f,9.f,-1.f,12.f,12.f,4.f,9.f,2.f,3.f,12.f,21.f,12.f,2.f,4.6f,7.4f,19.4f,7.4f,2.f,4.6f,16.6f,19.4f,16.6f,0.f};  // World
        static const float iconData4[]={48.f,2.6f,16.4f,2.6f,13.6f,2.61f,13.4f,2.66f,13.21f,2.73f,13.03f,2.82f,12.87f,2.94f,12.72f,3.08f,12.6f,3.23f,12.49f,3.41f,12.41f,3.6f,12.34f,3.8f,12.3f,6.3f,11.8f,8.5f,8.5f,8.63f,8.33f,8.76f,8.18f,8.91f,8.04f,9.07f,7.92f,9.24f,7.82f,9.41f,7.74f,9.6f,7.68f,9.79f,7.64f,9.99f,7.61f,10.2f,7.6f,14.8f,7.6f,15.01f,7.61f,15.21f,7.64f,15.4f,7.68f,15.59f,7.74f,15.76f,7.82f,15.93f,7.92f,16.09f,8.04f,16.24f,8.18f,16.37f,8.33f,16.5f,8.5f,18.7f,11.6f,20.9f,12.2f,21.07f,12.27f,21.22f,12.35f,21.36f,12.46f,21.48f,12.57f,21.58f,12.7f,21.66f,12.84f,21.72f,12.99f,21.76f,13.15f,21.79f,13.32f,21.8f,13.5f,21.8f,16.4f,5.f,7.7f,12.f,9.5f,9.3f,14.5f,9.3f,16.3f,12.f,7.7f,12.f,2.f,12.f,9.3f,12.f,12.f,2.f,8.6f,16.4f,15.4f,16.4f,-2.f,6.4f,16.6f,2.1f,2.1f,-2.f,17.6f,16.6f,2.1f,2.1f,0.f};  // Vehicle
        static const float iconData5[]={3.f,7.f,11.f,6.2f,8.1f,7.3f,5.4f,3.f,10.f,10.6f,9.5f,7.f,10.4f,3.6f,3.f,13.f,10.4f,13.5f,6.5f,12.5f,2.6f,3.f,16.f,10.8f,16.9f,7.5f,18.1f,5.f,3.f,6.f,13.6f,3.4f,11.4f,2.9f,8.8f,2.f,7.f,11.2f,9.6f,17.f,2.f,16.f,10.9f,14.4f,17.f,2.f,10.4f,17.f,10.4f,22.f,2.f,13.6f,17.f,13.6f,22.f,-2.f,7.f,11.f,1.15f,1.15f,-2.f,10.f,10.6f,1.15f,1.15f,-2.f,13.f,10.4f,1.15f,1.15f,-2.f,16.f,10.8f,1.15f,1.15f,0.f};  // Z-SPWN
        static const float iconData6[]={33.f,19.25f,10.53f,21.52f,10.76f,21.52f,13.24f,19.25f,13.47f,18.17f,16.09f,19.61f,17.85f,17.85f,19.61f,16.09f,18.17f,13.47f,19.25f,13.24f,21.52f,10.76f,21.52f,10.53f,19.25f,7.91f,18.17f,6.15f,19.61f,4.39f,17.85f,5.83f,16.09f,4.75f,13.47f,2.48f,13.24f,2.48f,10.76f,4.75f,10.53f,5.83f,7.91f,4.39f,6.15f,6.15f,4.39f,7.91f,5.83f,10.53f,4.75f,10.76f,2.48f,13.24f,2.48f,13.47f,4.75f,16.09f,5.83f,17.85f,4.39f,19.61f,6.15f,18.17f,7.91f,19.25f,10.53f,-1.f,12.f,12.f,3.f,3.f,0.f};  // Settings
        static const float* const iconTable[7]={iconData0,iconData1,iconData2,iconData3,iconData4,iconData5,iconData6};
        if(category<0||category>=7)return;
        const float* d=iconTable[category];
        const float sw=stroke*iconScale;
        while(true){
            const int n=(int)(*d);
            if(n==0)break;
            if(n>0){
                ++d;
                float px=d[0],py=d[1];
                for(int i=1;i<n;i++){line(px,py,d[2*i],d[2*i+1]);px=d[2*i];py=d[2*i+1];}
                d+=2*n;
            }else{
                const float cx=d[1],cy=d[2],rx=d[3]*iconScale,ry=d[4]*iconScale;
                if(n==-2)vecFillEllipse(vec,sx(cx)+0.5f-rx,sy(cy)+0.5f-ry,sx(cx)+0.5f+rx,sy(cy)+0.5f+ry,ic);
                else vecEllipse(vec,sx(cx)-rx,sy(cy)-ry,sx(cx)+rx+1.f,sy(cy)+ry+1.f,sw,ic);
                d+=5;
            }
        }
    };
    // Pass 1: selection highlight + icons (anti-aliased, drawn before any label text so the highlight stays behind it).
    for(int i=0;i<categoryCount;i++){
        int yy=CAT_TOP+i*CAT_ROW;
        if(i==activeCategory){
            // Collapsed mode: equal 8px left/right padding inside 46px sidebar.
            // Expanded mode: retain the original full-width label highlight.
            const float inset=(SIDE<=SIDE_MIN+8)?8.0f:5.0f;
            vecRRect(vec,inset,(float)(yy+3),(float)SIDE-inset,(float)(yy+35),4.5f,themeAccentColor());
        }
        drawSidebarIcon(i,11,yy+9);
    }
    drawVecLayer(dc,vec); // also flushes the collapse handle + chevron recorded above
    // Pass 2: sidebar labels.
    int savedSidebarDc=SaveDC(dc);
    IntersectClipRect(dc,0,CAT_TOP,SIDE-3,MH-34);
    for(int i=0;i<categoryCount;i++){
        int yy=CAT_TOP+i*CAT_ROW;
        if(SIDE>SIDE_MIN+8)textGdi(dc,smallFont,categoryNames[i],43,yy,SIDE-46,38,i==activeCategory?themeTextColor():themeMutedText());
    }
    RestoreDC(dc,savedSidebarDc);
    // Flat two-column layout: no oversized section cards or title bars.
    const int left=SIDE+20, available=MW-SIDE-40, gap=28, cw=(available-gap)/2;
    const int right=left+cw+gap, top=HEADER_H+53;
    textGdi(dc,heading,categoryNames[activeCategory],left,HEADER_H+10,available,27,themeTextColor());
    HPEN linePen=mkPen(1,themeLineColor(),true);
    auto rule=[&](int x,int y,int w){HGDIOBJ old=SelectObject(dc,linePen);MoveToEx(dc,x,y,nullptr);LineTo(dc,x+w,y);SelectObject(dc,old);};
    auto title=[&](int x,int y,const wchar_t* label){
        textGdi(dc,normal,label,x,y,cw-26,26,themeTextColor());
        // Short accent underline: only the rendered heading text, not the full section width.
        HGDIOBJ previousFont=SelectObject(dc,normal);
        SIZE headingSize={};
        GetTextExtentPoint32W(dc,label,(int)wcslen(label),&headingSize);
        SelectObject(dc,previousFont);
        int underlineWidth=headingSize.cx;
        if(underlineWidth>cw-26)underlineWidth=cw-26;
        HPEN underlinePen=mkPen(3,themeAccentColor(),true);
        HGDIOBJ previousPen=SelectObject(dc,underlinePen);
        MoveToEx(dc,x,y+25,nullptr);
        LineTo(dc,x+underlineWidth,y+25);
        SelectObject(dc,previousPen);
        DeleteObject(underlinePen);
    };
    // Visual-only 250ms ease-in-out toggle animation. The underlying feature
    // flag changes immediately; no hook or gameplay operation is delayed.
    struct ToggleVisual { bool initialized=false; bool target=false; float from=0.f; float value=0.f; ULONGLONG started=0; };
    static ToggleVisual toggleVisuals[48]{};   // ids 0..39 (No Sway = 31, No Spread = 32, Time of Day Lock = 36)
    auto togglePosition=[&](int id,bool on)->float{
        if(id<0||id>=48)return on?1.f:0.f;
        ToggleVisual& a=toggleVisuals[id];
        const ULONGLONG now=GetTickCount64();
        if(!a.initialized){a.initialized=true;a.target=on;a.from=a.value=on?1.f:0.f;a.started=now;}
        if(a.target!=on){a.from=a.value;a.target=on;a.started=now;}
        float t=(std::min)(1.f,float(now-a.started)/250.f);
        float eased=t*t*(3.f-2.f*t); // smoothstep, including mid-animation reversals
        a.value=a.from+((a.target?1.f:0.f)-a.from)*eased;
        if(t<1.f)toggleAnimationActive=true; // request next frame while moving
        return a.value;
    };
    auto toggleRow=[&](int x,int y,int w,const wchar_t* label,int id){
        bool ready=featureReady(id);
        textGdi(dc,smallFont,label,x+5,y,w-60,25,ready?themeTextColor():themeMutedText());
        if(id==20){
            // Raised control with hover and a short pressed feedback state.
            const int bx=x+w-67,by=y+2,bw=62,bh=23;
            POINT cursor{};GetCursorPos(&cursor);
            if(gameWindow){ScreenToClient(gameWindow,&cursor);clientToBuffer(cursor);}
            const int cx=toLogical(cursor.x-menuX),cy=toLogical(cursor.y-menuY);
            const bool hover=cx>=bx&&cx<bx+bw&&cy>=by&&cy<by+bh;
            const ULONGLONG elapsed=GetTickCount64()-skillButtonPressedAt.load(std::memory_order_acquire);
            const bool pressed=elapsed<160;
            if(pressed)toggleAnimationActive=true;
            const int dy=pressed?2:0;
            {   HBRUSH sb=CreateSolidBrush(RGB(17,15,20));HGDIOBJ sob=SelectObject(dc,sb),sop=SelectObject(dc,GetStockObject(NULL_PEN));
                RoundRect(dc,bx+1,by+3,bx+bw+2,by+bh+4,BTN_RADIUS*2,BTN_RADIUS*2);
                SelectObject(dc,sop);SelectObject(dc,sob);DeleteObject(sb);}
            HBRUSH br=nullptr;
            RECT body{bx,by+dy,bx+bw,by+bh+dy};
            COLORREF bg=!ready?RGB(65,62,70):(pressed?RGB(121,43,73):(hover?RGB(230,92,144):themeAccentColor()));
            br=CreateSolidBrush(bg);
            HPEN outline=mkPen(1,pressed?RGB(95,45,65):RGB(247,162,198),true);
            HGDIOBJ oldPen=SelectObject(dc,outline),oldBrush=SelectObject(dc,br);
            RoundRect(dc,body.left,body.top,body.right,body.bottom,BTN_RADIUS*2,BTN_RADIUS*2);
            SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(outline);DeleteObject(br);
            textGdi(dc,smallFont,pressed?L"...":L"MAX",bx+14,by+dy+3,bw-16,18,themeTextColor());
            return;
        }
        int tx=x+w-40;
        const bool on=ready&&enabled(id);
        // Anti-aliased pill track (34x16, fully rounded) and 12px knob; flushed once after all rows.
        float visual=togglePosition(id,on);
        COLORREF off=RGB(65,62,70),accent=themeAccentColor();
        auto blend=[&](int a,int b)->BYTE{return (BYTE)(a+(b-a)*visual+0.5f);};
        COLORREF track=RGB(blend(GetRValue(off),GetRValue(accent)),blend(GetGValue(off),GetGValue(accent)),blend(GetBValue(off),GetBValue(accent)));
        vecRRect(vec,(float)tx,(float)(y+5),(float)(tx+34),(float)(y+21),8.0f,track);
        float dx=(float)tx+3.f+16.f*visual;
        vecFillEllipse(vec,(float)dx,(float)(y+7),(float)(dx+12),(float)(y+19),themeTextColor());
    };
    auto featureSliderRow=[&](int x,int y,int w,int sliderId){
        const wchar_t* label=sliderId==0?L"Character Time Dilation":(sliderId==1?L"Time of Day Lock":(sliderId==3?L"Gravity Scale":L"Global Time Dilation"));
        float val=sliderId==0?characterTimeDilation.load(std::memory_order_relaxed):(sliderId==1?visualTimeValue.load(std::memory_order_relaxed):(sliderId==3?gravityScaleValue:worldTimeScale));
        float lo=(sliderId==0||sliderId==3)?0.1f:(sliderId==2?0.1f:0.f),hi=(sliderId==0||sliderId==3)?3.f:(sliderId==1?2400.f:4.f);
        wchar_t buf[64]{};
        if(sliderId==1)swprintf_s(buf,L"%.0f",val);else swprintf_s(buf,L"%.2fx",val);
        textGdi(dc,smallFont,label,x+5,y,w-85,19,themeTextColor());
        textGdi(dc,smallFont,buf,x+w-(sliderId==1?112:75),y,sliderId==1?67:70,19,themeTextColor());
        if(sliderId==1){
            int tx=x+w-40;
            // Same smooth on/off animation as the other toggles (visual only; visualTimeLock itself changes immediately).
            const float tv=togglePosition(36,visualTimeLock);
            const COLORREF offC=RGB(65,62,70),onC=themeAccentColor();
            auto mix=[&](int a,int b)->BYTE{return (BYTE)(a+(b-a)*tv+0.5f);};
            const COLORREF trk=RGB(mix(GetRValue(offC),GetRValue(onC)),mix(GetGValue(offC),GetGValue(onC)),mix(GetBValue(offC),GetBValue(onC)));
            vecRRect(vec,(float)tx,(float)(y+1),(float)(tx+34),(float)(y+15),7.f,trk);
            const float dx=(float)tx+3.f+16.f*tv;
            vecFillEllipse(vec,dx,(float)(y+2),dx+12.f,(float)(y+14),themeTextColor());
        }
        int sx=x+6,sw=w-12,sy=y+21;
        RECT bg{sx,sy,sx+sw,sy+8};HBRUSH b=CreateSolidBrush(RGB(63,58,66));FillRect(dc,&bg,b);DeleteObject(b);
        RECT fg{sx,sy,sx+(int)roundf(sw*(val-lo)/(hi-lo)),sy+8};
        b=CreateSolidBrush(themeAccentColor());FillRect(dc,&fg,b);DeleteObject(b);
    };
    if(activeCategory==6){
        // Scroll the whole Settings content while keeping the sidebar and footer fixed.
        int settingsDC=SaveDC(dc);
        IntersectClipRect(dc,SIDE+1,top-3,MW-8,MH-34);
        XFORM settingsTransform{SS,0.0f,0.0f,SS,0.0f,-SS*(float)settingsScroll};
        SetWorldTransform(dc,&settingsTransform);
        settingsTextScrollOffset=settingsScroll;
        const wchar_t* names[]={L"Background",L"Accent",L"Text / Icon"};
        title(left,top,L"Colors");
        for(int i=0;i<3;i++){
            int y=hexY(i),bx=hexX(),sw=23;
            textGdi(dc,normal,names[i],left+6,y,bx-left-12,HEX_INPUT_H,themeTextColor());
            RECT field{bx,y,bx+HEX_INPUT_W,y+HEX_INPUT_H};
            HBRUSH fieldBrush=CreateSolidBrush(RGB(35,32,38));FillRect(dc,&field,fieldBrush);DeleteObject(fieldBrush);
            HPEN borderPen=mkPen(1,editingHex==i?themeAccentColor():RGB(110,94,107),true);
            HGDIOBJ oldPen=SelectObject(dc,borderPen);HGDIOBJ oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Rectangle(dc,field.left,field.top,field.right,field.bottom);
            SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(borderPen);
            if(!hexDraft[i][0])syncHex(i);
            textGdi(dc,normal,hexDraft[i],bx+6,y+1,HEX_INPUT_W-8,HEX_INPUT_H-2,themeTextColor());
            int sx=bx+HEX_INPUT_W+10;
            RECT swatch{sx,y,sx+sw,y+HEX_INPUT_H};
            HBRUSH colorBrush=CreateSolidBrush(i==0?RGB(themeBg[0],themeBg[1],themeBg[2]):(i==1?themeAccentColor():themeTextColor()));
            FillRect(dc,&swatch,colorBrush);DeleteObject(colorBrush);
            FrameRect(dc,&swatch,(HBRUSH)GetStockObject(GRAY_BRUSH));
        }
        { // Background opacity: its own row under the three colours.
            {
                int y=hexY(3),ox=opacitySliderX(),oy=y+HEX_INPUT_H/2;
                RECT rail{ox,oy-4,ox+OPACITY_SLIDER_W,oy+4};
                HBRUSH b=CreateSolidBrush(RGB(63,58,66));FillRect(dc,&rail,b);DeleteObject(b);
                RECT fill{ox,oy-4,ox+(bgOpacity*OPACITY_SLIDER_W+50)/100,oy+4};
                b=CreateSolidBrush(themeAccentColor());FillRect(dc,&fill,b);DeleteObject(b);
                wchar_t opacityText[12];swprintf_s(opacityText,L"%d%%",bgOpacity);
                textGdi(dc,smallFont,opacityText,ox+OPACITY_SLIDER_W+6,y,39,HEX_INPUT_H,themeTextColor());
                textGdi(dc,normal,L"Opacity",left+6,y,ox-left-12,HEX_INPUT_H,themeTextColor());
            }
        }
        // One unobtrusive divider below both color fields, not between them.
        // No divider separating the compact color controls.
        // Compact, clearly bounded Reset Colors button; its hitbox matches the visual button.
        constexpr int RESET_BUTTON_X_OFFSET=6, RESET_BUTTON_Y_OFFSET=170;
        constexpr int RESET_BUTTON_W=142, RESET_BUTTON_H=27;
        RECT resetButton{left+RESET_BUTTON_X_OFFSET,top+RESET_BUTTON_Y_OFFSET,
                         left+RESET_BUTTON_X_OFFSET+RESET_BUTTON_W,top+RESET_BUTTON_Y_OFFSET+RESET_BUTTON_H};
        HBRUSH resetFill=CreateSolidBrush(RGB(42,38,45));
        HPEN resetPen=mkPen(1,themeAccentColor(),true);
        HGDIOBJ resetOldPen=SelectObject(dc,resetPen);
        HGDIOBJ resetOldBrush=SelectObject(dc,resetFill);
        RoundRect(dc,resetButton.left,resetButton.top,resetButton.right,resetButton.bottom,BTN_RADIUS*2,BTN_RADIUS*2);
        SelectObject(dc,resetOldBrush);SelectObject(dc,resetOldPen);DeleteObject(resetPen);DeleteObject(resetFill);
        textGdi(dc,normal,L"Reset Colors",resetButton.left+(RESET_BUTTON_W-measureTextW(dc,normal,L"Reset Colors"))/2,resetButton.top+2,RESET_BUTTON_W-4,RESET_BUTTON_H-4,themeTextColor());
        // Thin section rules, as on the other tabs: under Colors, under Profiles and under Banner.
        rule(left,top+207,cw);rule(right,top+184,cw);rule(right,top+286,cw);   // Colors rule sits 4 px lower (more room under Reset Colors)

        // Banner chooser: compact dropdown below the color controls.
        const int bannerY=top+SETTINGS_BANNER_OFF;
        const int rcol=right;   // right-hand Settings column
        title(rcol,top+198,L"Banner");
        int dropX=rcol+6,dropW=bannerDropW();
        RECT bannerField{dropX,bannerY,dropX+dropW,bannerY+26};
        HBRUSH bfill=CreateSolidBrush(RGB(35,32,38));FillRect(dc,&bannerField,bfill);DeleteObject(bfill);
        HPEN bp=mkPen(1,themeAccentColor(),true);HGDIOBJ bpOld=SelectObject(dc,bp);
        HGDIOBJ bbOld=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Rectangle(dc,bannerField.left,bannerField.top,bannerField.right,bannerField.bottom);
        SelectObject(dc,bbOld);SelectObject(dc,bpOld);DeleteObject(bp);
        const wchar_t* bannerName=selectedBanner==-1?L"None":(selectedBanner>=0&&selectedBanner<(int)bannerFiles.size()?bannerFiles[selectedBanner].c_str():L"Default Banner");
        textGdi(dc,smallFont,bannerName,dropX+7,bannerY+3,dropW-27,22,themeTextColor());
        textGdi(dc,smallFont,bannerDropdownOpen?L"v":L"v",dropX+dropW-17,bannerY+3,15,22,themeTextColor());
        // The expanded list is rendered as a separate D3D overlay after the menu.
        // Drawing it here would clip entries outside the 680x520 menu texture.

        if(!bannerWarning.empty()){
            textGdi(dc,smallFont,bannerWarning.c_str(),dropX,bannerY+30,cw-12,16,RGB(255,72,85));
        }
        // Two-column official preset gallery on the main Settings scroll page.
        // Each card has enough height for a readable, undistorted thumbnail.
        const int galleryX=left+6,galleryY=top+SETTINGS_PRESET_TOP_OFFSET;
        const int galleryW=available-12,galleryGap=12;
        const int cellW=(galleryW-galleryGap)/2,cellH=SETTINGS_PRESET_CELL_H;
        textGdi(dc,normal,L"Official Presets",galleryX,galleryY-29,galleryW,22,themeTextColor());
        initPresetFolder();
        if(!bannerGdiplusToken){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&bannerGdiplusToken,&input,nullptr);}
        for(int index=0;index<PRESET_COUNT;index++){
            const int cx=galleryX+(index%2)*(cellW+galleryGap);
            const int cy=galleryY+(index/2)*SETTINGS_PRESET_ROW;
            if(cy+cellH-settingsScroll<top+4||cy-settingsScroll>=MH-34)continue;
            RECT cell{cx,cy,cx+cellW,cy+cellH};
            HBRUSH bg=CreateSolidBrush(RGB(29,27,33));FillRect(dc,&cell,bg);DeleteObject(bg);
            {
                const int targetW=cellW-8,targetH=cellH-24;
                const PresetThumb* thumb=getPresetThumb(index,targetW,targetH,SS);
                if(thumb){
                    GdiFlush(); // make sure the GDI fills above are in the DIB before writing pixels directly
                    const int dx0=(int)lroundf((cx+4+(targetW-thumb->dw)/2)*SS);
                    const int dy0=(int)lroundf((cy+3+(targetH-thumb->dh)/2-settingsScroll)*SS);
                    // Same visible window as the Settings clip rectangle.
                    const int minX=(std::max)(0,(int)ceilf((SIDE+1)*SS)),maxX=(std::min)(RW,(int)floorf((MW-8)*SS));
                    const int minY=(std::max)(0,(int)ceilf((top-3)*SS)),maxY=(std::min)(RH,(int)floorf((MH-34)*SS));
                    for(int yy=(std::max)(minY,dy0);yy<(std::min)(maxY,dy0+thumb->ph);yy++){
                        const uint32_t* srow=&thumb->pixels[(size_t)(yy-dy0)*thumb->pw];
                        for(int xx=(std::max)(minX,dx0);xx<(std::min)(maxX,dx0+thumb->pw);xx++){
                            const uint32_t s=srow[xx-dx0],a=s>>24;
                            if(a==255)px[(size_t)yy*RW+xx]=0xFF000000u|(s&0x00FFFFFFu);
                            else if(a){ // blend over the card background RGB(29,27,33)
                                const uint32_t r=(((s>>16)&255u)*a+29u*(255u-a))/255u,g=(((s>>8)&255u)*a+27u*(255u-a))/255u,b=((s&255u)*a+33u*(255u-a))/255u;
                                px[(size_t)yy*RW+xx]=0xFF000000u|(r<<16)|(g<<8)|b;
                            }
                        }
                    }
                }
            }
            RECT tag{cx+2,cy+cellH-21,cx+cellW-2,cy+cellH-2};
            HBRUSH tagBrush=CreateSolidBrush(RGB(18,18,23));FillRect(dc,&tag,tagBrush);DeleteObject(tagBrush);
            textGdi(dc,smallFont,officialPresets[index].label,cx+8,cy+cellH-21,cellW-16,18,RGB(248,243,247));
            HPEN border=mkPen(selectedPreset==index?2:1,selectedPreset==index?themeAccentColor():RGB(95,90,103),true);
            HGDIOBJ oldPen=SelectObject(dc,border),oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Rectangle(dc,cell.left,cell.top,cell.right,cell.bottom);
            SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(border);
        }
        // ---- Profiles: the whole menu layout and settings saved as a JSON file (Profiles folder next to ADREENA.dll).
        {
            const int rcol=right,px0=rcol+6;
            title(rcol,top,L"Profiles");
            const COLORREF stepBase=RGB(74,66,80);
            const int fieldW=bannerDropW();
            RECT nb{px0,top+36,px0+fieldW-78,top+60};
            {HBRUSH nbr=CreateSolidBrush(editingNum==205?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&nb,nbr);DeleteObject(nbr);
             wchar_t nl[40];if(editingNum==205)swprintf(nl,40,L"%ls_",profileNameBuf);else if(profileNameBuf[0])swprintf(nl,40,L"%ls",profileNameBuf);else swprintf(nl,40,L"Profile name");
             textGdi(dc,smallFont,nl,nb.left+6,nb.top+3,nb.right-nb.left-8,20,(profileNameBuf[0]||editingNum==205)?themeTextColor():themeMutedText());}
            RECT svb{nb.right+8,top+36,px0+fieldW,top+60};
            drawButton(dc,smallFont,svb,L"Save",230,themeAccentColor(),themeTextColor());
            RECT lf{px0,top+68,px0+fieldW,top+94};
            profDropField=lf;
            {HBRUSH lfb=CreateSolidBrush(RGB(35,32,38));FillRect(dc,&lf,lfb);DeleteObject(lfb);
             HPEN lp2=mkPen(1,themeAccentColor(),true);HGDIOBJ lpOld=SelectObject(dc,lp2);HGDIOBJ lbOld=SelectObject(dc,GetStockObject(NULL_BRUSH));
             Rectangle(dc,lf.left,lf.top,lf.right,lf.bottom);SelectObject(dc,lbOld);SelectObject(dc,lpOld);DeleteObject(lp2);
             const wchar_t* lt=profileNames.empty()?L"No saved profiles yet":((profileSel>=0&&profileSel<(int)profileNames.size())?profileNames[profileSel].c_str():L"Select a profile");
             textGdi(dc,smallFont,lt,lf.left+7,lf.top+3,lf.right-lf.left-27,22,themeTextColor());
             textGdi(dc,smallFont,L"v",lf.right-17,lf.top+3,15,22,themeTextColor());}
            const int halfW=(fieldW-8)/2;
            RECT ldb{px0,top+100,px0+halfW,top+124},dlb{px0+halfW+8,top+100,px0+fieldW,top+124};
            drawButton(dc,smallFont,ldb,L"Load",231,stepBase,themeTextColor());
            drawButton(dc,smallFont,dlb,profileDelArm?L"Sure?":L"Delete",232,stepBase,themeTextColor());
            RECT aub{px0,top+130,px0+fieldW,top+154};
            {std::wstring al=profileAuto.empty()?std::wstring(L"Load on startup: Off"):(L"Load on startup: "+profileAuto);
             drawButton(dc,smallFont,aub,al.c_str(),233,profileAuto.empty()?stepBase:themeAccentColor(),themeTextColor());}
            textGdi(dc,smallFont,profileStatus.c_str(),px0,top+158,cw-12,20,themeMutedText());
        }
        // Floating picker renders above the banner dropdown and dividers.
        if(pickerColor>=0){
            int gx=pickerX(),gy=pickerY(),hx=gx+PICKER_SIZE+PICKER_GAP;
            // A compact floating panel. It does not reflow Settings or touch the footer.
            RECT popup{gx-10,gy-31,hx+PICKER_HUE_W+12,gy+PICKER_SIZE+11};
            HBRUSH panel=CreateSolidBrush(RGB(34,31,37));FillRect(dc,&popup,panel);DeleteObject(panel);
            HPEN frame=mkPen(1,RGB(95,65,82),true);HGDIOBJ oldP=SelectObject(dc,frame);
            HGDIOBJ oldB=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Rectangle(dc,popup.left,popup.top,popup.right,popup.bottom);
            SelectObject(dc,oldB);SelectObject(dc,oldP);DeleteObject(frame);
            textGdi(dc,smallFont,pickerColor==0?L"Background Color":(pickerColor==1?L"Accent Color":L"Text / Icon Color"),gx,gy-27,185,22,themeTextColor());
            // Render the HSV gradients at physical pixel resolution, bypassing GDI's
            // non-integer world-transform pixel expansion that created the visible grid.
            auto gradient=[&](int lx,int ly,int lw,int lh,bool hue){
                int x0=(int)lroundf(lx*SS),y0=(int)lroundf((ly-settingsScroll)*SS);
                int x1=(int)lroundf((lx+lw)*SS),y1=(int)lroundf((ly+lh-settingsScroll)*SS);
                for(int yy=(std::max)(0,y0);yy<(std::min)(RH,y1);yy++){
                    float v=(float)(yy-y0)/(float)((std::max)(1,y1-y0-1));
                    for(int xx=(std::max)(0,x0);xx<(std::min)(RW,x1);xx++){
                        float u=(float)(xx-x0)/(float)((std::max)(1,x1-x0-1));
                        COLORREF c=hue?hsvColor(v,1.f,1.f):hsvColor(pickerHue,u,1.f-v);
                        px[(size_t)yy*RW+xx]=0xFF000000u|((uint32_t)GetRValue(c)<<16)|((uint32_t)GetGValue(c)<<8)|GetBValue(c);
                    }
                }
            };
            gradient(gx,gy,PICKER_SIZE,PICKER_SIZE,false);
            gradient(hx,gy,PICKER_HUE_W,PICKER_SIZE,true);
            HPEN marker=mkPen(2,themeTextColor(),true);HGDIOBJ op=SelectObject(dc,marker);
            int mx=gx+(int)(pickerSat*(PICKER_SIZE-1)),my=gy+(int)((1.f-pickerValue)*(PICKER_SIZE-1));
            HGDIOBJ oldMarkerBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Ellipse(dc,mx-5,my-5,mx+6,my+6);SelectObject(dc,oldMarkerBrush);
            int hy=gy+(int)(pickerHue*(PICKER_SIZE-1));MoveToEx(dc,hx-3,hy,nullptr);LineTo(dc,hx+PICKER_HUE_W+3,hy);
            SelectObject(dc,op);DeleteObject(marker);
        }
        if(profDropOpen){   // saved-profile list, drawn last so it sits above everything on the page
            const int total=(int)profileNames.size();const int rows=(std::min)(6,(std::max)(1,total));
            RECT pr{profDropField.left,profDropField.bottom+1,profDropField.right,profDropField.bottom+1+rows*BANNER_ROW_HEIGHT};
            const int maxScroll=(std::max)(0,total-rows);if(profDropScroll>maxScroll)profDropScroll=maxScroll;if(profDropScroll<0)profDropScroll=0;
            for(int j=0;j<rows;j++){
                const int item=profDropScroll+j;
                RECT row{pr.left,pr.top+j*BANNER_ROW_HEIGHT,pr.right,pr.top+(j+1)*BANNER_ROW_HEIGHT};
                HBRUSH fill=CreateSolidBrush(item==profileSel?RGB(79,49,66):RGB(35,32,38));FillRect(dc,&row,fill);DeleteObject(fill);
                const std::wstring tx=total==0?std::wstring(L"No saved profiles yet"):(item<total?profileNames[item]:std::wstring());
                textGdi(dc,smallFont,tx.c_str(),row.left+8,row.top+3,row.right-row.left-(total>rows?22:14),22,total==0?themeMutedText():themeTextColor());
            }
            if(total>rows){
                const int hgt=rows*BANNER_ROW_HEIGHT,trackX=pr.right-6;
                const int thumbH=(std::max)(14,hgt*rows/total);
                const int thumbY=pr.top+(maxScroll?profDropScroll*(hgt-thumbH)/maxScroll:0);
                RECT track{trackX,pr.top,pr.right,pr.top+hgt};HBRUSH tb=CreateSolidBrush(RGB(68,61,70));FillRect(dc,&track,tb);DeleteObject(tb);
                RECT thumb{trackX,thumbY,pr.right,thumbY+thumbH};HBRUSH hb=CreateSolidBrush(themeAccentColor());FillRect(dc,&thumb,hb);DeleteObject(hb);
            }
            HPEN bp2=mkPen(1,themeAccentColor(),true);HGDIOBJ bpOld2=SelectObject(dc,bp2),bbOld2=SelectObject(dc,GetStockObject(NULL_BRUSH));
            Rectangle(dc,pr.left,pr.top,pr.right,pr.bottom);
            SelectObject(dc,bbOld2);SelectObject(dc,bpOld2);DeleteObject(bp2);
        }
        RestoreDC(dc,settingsDC);
        settingsTextScrollOffset=0;
        // One subtle scrollbar represents the full Settings page.
        if(settingsMaxScroll()>0){
            const int trackTop=top,trackH=MH-34-top;
            const int thumbH=(std::max)(22,(int)lroundf(trackH*(float)trackH/(trackH+settingsMaxScroll())));
            const int thumbY=trackTop+(int)lroundf((trackH-thumbH)*(float)settingsScroll/settingsMaxScroll());
            RECT rail{MW-12,trackTop,MW-5,trackTop+trackH};
            HBRUSH b=CreateSolidBrush(RGB(70,65,74));FillRect(dc,&rail,b);DeleteObject(b);
            RECT thumb{MW-12,thumbY,MW-5,thumbY+thumbH};
            b=CreateSolidBrush(themeAccentColor());FillRect(dc,&thumb,b);DeleteObject(b);
        }
    }else{
        const wchar_t* sectionNames[6][7]={
            {L"Protection",L"Movement",L"Character / Clothing",L"Utilities",L"Skill Editor",L"Survivor Name"},
            {L"",L"Ammunition",L"Weapon Condition",L"Accuracy",L"Melee",L""},
            {L"Set Resources",L"Management",L"Mission Timer",L"Give Item",L"",L""},
            {L"Time",L"Teleport",L"Exploration",L"Advanced",L"",L""},
            {L"Vehicle",L"",L"",L"",L"Spawn Vehicle",L"Trunk Size"},
            {L"Zombie Spawner",L"Fun",L"",L"",L"",L"Hordes"}
        };

        int pageClip=SaveDC(dc);
        IntersectClipRect(dc,SIDE+1,top-3,MW-8,MH-34);
        dropFieldSet[1]=dropFieldSet[2]=dropFieldSet[3]=false;
        for(int k=0;k<sectionCount(activeCategory);k++){
            int x=sectionX(activeCategory,k),y=top+sectionTopOffset(activeCategory,k)-pageScroll[activeCategory];
            const int cw=sectionW(activeCategory,k);   // full width for Protection, half width otherwise
            const int secH=sectionHeight(activeCategory,k);
            if(secH<=0)continue;
            if(y+secH+1<top-3||y>=MH-34)continue;
            title(x,y,sectionNames[activeCategory][k]);
            if(searchHLActive()&&searchHLCat==activeCategory&&searchHLSec==k&&searchHLRow<0){
                uiAnimSeen=true;
                if(searchHLBlinkOn()){HBRUSH hb=CreateSolidBrush(themeAccentColor());RECT hr{x-4,y-2,x+cw-8,y+28};FrameRect(dc,&hr,hb);DeleteObject(hb);}
            }
            // Content is independently clipped and scrollable. The title and rule stay fixed.
            const int contentTop=y+31, contentBottom=y+secH-4;
            const float offset=sectionScroll[activeCategory][k];
            int clipSave=SaveDC(dc);
            IntersectClipRect(dc,x,contentTop,x+cw,contentBottom);
            {
                if(activeCategory==0&&k==2){
                    // Character / Clothing: Cycle Hat and Cycle Outfit selectors (arrows, centred value, Default), then Unlearn + status.
                    const COLORREF stepBase=RGB(74,66,80),arrowBase=RGB(74,66,80);
                    const int boxW=26;
                    for(int g=0;g<2;g++){
                        const int base=y+32+g*80;
                        const wchar_t* lab=g==0?L"Cycle Hat":L"Cycle Outfit";
                        const int off=g==0?hatOffset:bodyOffset;
                        {const int tw=measureTextW(dc,smallFont,lab);textGdi(dc,smallFont,lab,x+4+((cw-16)-tw)/2,base,cw-8,19,themeTextColor());}   // centred over the controls (x+4 .. x+cw-12)
                        RECT lb{x+4,base+22,x+4+boxW,base+44},rb{x+cw-12-boxW,base+22,x+cw-12,base+44},nb{x+4+boxW+4,base+22,x+cw-12-boxW-4,base+44};
                        drawButton(dc,smallFont,lb,L"<",160+g*2,arrowBase,themeTextColor());
                        drawButton(dc,smallFont,rb,L">",161+g*2,arrowBase,themeTextColor());
                        HBRUSH nbr=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&nb,nbr);DeleteObject(nbr);
                        wchar_t vt[48];
                        if(off==0)swprintf(vt,48,g==0?L"Default Hat":L"Default Outfit");
                        else swprintf(vt,48,g==0?L"Hat %+d":L"Outfit %+d",off);
                        {const int tw=measureTextW(dc,smallFont,vt),bw2=nb.right-nb.left;textGdi(dc,smallFont,vt,nb.left+(tw<bw2?(bw2-tw)/2:2),nb.top+2,bw2-4,20,themeTextColor());}
                        RECT db{x+4,base+48,x+cw-12,base+70};
                        drawButton(dc,smallFont,db,L"Default",164+g,stepBase,themeTextColor());
                    }
                    {   // Appearance (group + variant selectors, Apply) and Voice (selector, Apply). Names come from the game's own lists.
                        auto fit=[&](const std::wstring& t,int maxW)->std::wstring{
                            if(measureTextW(dc,smallFont,t.c_str())<=maxW)return t;
                            std::wstring r=t;
                            while(r.size()>3){r.pop_back();const std::wstring c=r+L"..";if(measureTextW(dc,smallFont,c.c_str())<=maxW)return c;}
                            return r;
                        };
                        auto selectorRow=[&](int top,int idL,const std::wstring& text){
                            RECT lb{x+4,top,x+4+boxW,top+22},rb{x+cw-12-boxW,top,x+cw-12,top+22},nb{x+4+boxW+4,top,x+cw-12-boxW-4,top+22};
                            drawButton(dc,smallFont,lb,L"<",idL,arrowBase,themeTextColor());
                            drawButton(dc,smallFont,rb,L">",idL+1,arrowBase,themeTextColor());
                            HBRUSH nbr=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&nb,nbr);DeleteObject(nbr);
                            const int bw2=nb.right-nb.left;const std::wstring shown=fit(text,bw2-8);
                            const int tw=measureTextW(dc,smallFont,shown.c_str());
                            textGdi(dc,smallFont,shown.c_str(),nb.left+(tw<bw2?(bw2-tw)/2:2),nb.top+2,bw2-4,20,themeTextColor());
                        };
                        auto centred=[&](int top,const wchar_t* lab){const int tw=measureTextW(dc,smallFont,lab);textGdi(dc,smallFont,lab,x+4+((cw-16)-tw)/2,top,cw-8,19,themeTextColor());};
                        const int base=y+192;
                        centred(base,L"Appearance");
                        // Male / Female
                        {const int hw=(cw-16-4)/2;
                         for(int gdr=0;gdr<2;gdr++){
                             RECT gb{x+4+gdr*(hw+4),base+22,x+4+gdr*(hw+4)+hw,base+44};
                             drawButton(dc,smallFont,gb,gdr==0?L"Male":L"Female",174+gdr,appearanceGender==gdr?themeAccentColor():stepBase,themeTextColor());
                         }}
                        // Group and variant dropdown fields (the open list is drawn on top of the page afterwards).
                        const auto& AG=appearanceGroups[appearanceGender];const auto& AN=appearanceGroupNames[appearanceGender];
                        const bool haveA=!AG.empty();
                        std::wstring groupText=L"Press to load",varText=L"-";
                        if(haveA){
                            const int gs=appearanceGroupSel%(int)AG.size();
                            const auto& mem=AG[gs];
                            const int vs=appearanceVarSel%(int)mem.size();
                            groupText=L"Group: "+AN[gs];
                            wchar_t cnt[24];swprintf(cnt,24,L"  %d / %d",vs+1,(int)mem.size());
                            varText=appearanceVariantLabel(appearanceNames[mem[vs]],AN[gs])+cnt;
                        }
                        for(int fi=0;fi<2;fi++){
                            RECT fb{x+4,base+48+fi*26,x+cw-12,base+70+fi*26};
                            drawButton(dc,smallFont,fb,L"",177+fi,arrowBase,themeTextColor());
                            const std::wstring& tx=fi==0?groupText:varText;
                            textGdi(dc,smallFont,fit(tx,fb.right-fb.left-30).c_str(),fb.left+8,fb.top+2,fb.right-fb.left-30,20,themeTextColor());
                            textGdi(dc,smallFont,L"v",fb.right-17,fb.top+2,15,20,themeTextColor());
                            dropField[1+fi]=fb;dropFieldSet[1+fi]=true;
                        }
                        RECT ab{x+4,base+100,x+cw-12,base+122};
                        drawButton(dc,smallFont,ab,L"Apply",168,themeAccentColor(),themeTextColor());
                        const int vb=y+322;
                        centred(vb,L"Voice");
                        std::wstring vGroup=L"Press > to load",vVar=L"-";
                        if(!voiceGroups.empty()){
                            const int gs=voiceGroupSel%(int)voiceGroups.size();const auto& mem=voiceGroups[gs];
                            const int vs=voiceVarSel%(int)mem.size();
                            vGroup=voiceGroupNames[gs];
                            const std::wstring& full=voiceNames[mem[vs]];
                            std::wstring tail=full.size()>vGroup.size()+1&&full.compare(0,vGroup.size(),vGroup)==0?full.substr(vGroup.size()+1):std::wstring(L"Default");
                            wchar_t cnt[24];swprintf(cnt,24,L"  %d / %d",vs+1,(int)mem.size());
                            vVar=tail+cnt;
                        }
                        selectorRow(vb+22,171,vGroup);
                        selectorRow(vb+48,179,vVar);
                        RECT vab{x+4,vb+74,x+cw-12,vb+96};
                        drawButton(dc,smallFont,vab,L"Apply",173,themeAccentColor(),themeTextColor());
                    }
                    statusMarquee(dc,smallFont,charStatus,x+5,y+424,cw-14,themeMutedText(),7);
                }else if(activeCategory==3&&k==1){
                    const COLORREF stepBase=RGB(74,66,80);
                    RECT tb{x+4,y+34,x+cw-12,y+58};
                    drawButton(dc,smallFont,tb,L"Teleport to Map Marker",181,themeAccentColor(),themeTextColor());
                    statusMarquee(dc,smallFont,teleStatus,x+5,y+64,cw-14,themeMutedText(),8);
                    {   // hotkey row: [Add Hotkey / current key] [Reset Hotkey]
                        const int bw=cw-16,aw=bw*58/100;
                        RECT ab{x+4,y+90,x+4+aw,y+114};
                        RECT rb{x+4+aw+6,y+90,x+4+bw,y+114};
                        std::wstring lbl=L"Add Hotkey";
                        if(teleHotCapturing)lbl=L"Press a key... (Esc cancels)";
                        else if(teleHotVk.load())lbl=L"Hotkey: "+hotkeyName(teleHotVk.load(),teleHotMods.load());
                        drawButton(dc,smallFont,ab,lbl.c_str(),182,teleHotCapturing?themeAccentColor():stepBase,themeTextColor());
                        drawButton(dc,smallFont,rb,L"Reset Hotkey",183,stepBase,themeTextColor());
                    }
                    {   // Position bookmarks: 5 fixed slots under the Teleport controls
                        RECT nb{x+4,y+128,x+cw-12,y+150};HBRUSH nbr=CreateSolidBrush(editingNum==206?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&nb,nbr);DeleteObject(nbr);
                        wchar_t nl[40];if(editingNum==206)swprintf(nl,40,L"%ls_",bookmarkNameBuf);else if(bookmarkNameBuf[0])swprintf(nl,40,L"%ls",bookmarkNameBuf);else swprintf(nl,40,L"Bookmark name");
                        textGdi(dc,smallFont,nl,nb.left+6,nb.top+2,nb.right-nb.left-8,20,(bookmarkNameBuf[0]||editingNum==206)?themeTextColor():themeMutedText());
                        RECT sv{x+4,y+156,x+cw-12,y+180};drawButton(dc,smallFont,sv,L"Save current position",240,themeAccentColor(),themeTextColor());
                        for(int i=0;i<5;i++){
                            RECT row{x+4,y+188+i*24,x+cw-12,y+188+i*24+22};
                            HBRUSH rb2=CreateSolidBrush(i==bookmarkSel&&i<(int)bookmarks.size()?RGB(79,49,66):RGB(35,32,38));FillRect(dc,&row,rb2);DeleteObject(rb2);
                            const bool have=i<(int)bookmarks.size();
                            textGdi(dc,smallFont,have?bookmarks[i].name.c_str():L"(empty)",row.left+8,row.top+2,row.right-row.left-12,20,have?themeTextColor():themeMutedText());
                        }
                        const int bw=cw-16,tw=bw*60/100;
                        RECT tp{x+4,y+312,x+4+tw,y+336},dl{x+4+tw+6,y+312,x+4+bw,y+336};
                        drawButton(dc,smallFont,tp,L"Teleport",241,themeAccentColor(),themeTextColor());
                        drawButton(dc,smallFont,dl,bookmarkDelArm?L"Sure?":L"Delete",242,RGB(74,66,80),themeTextColor());
                        statusMarquee(dc,smallFont,bookmarkStatus,x+5,y+344,cw-14,themeMutedText(),14);
                    }
                }else if(activeCategory==0&&k==4){
                    // Skill Editor: Base / Specialized tabs, Refresh, one row per skill ([-] [+] [Max]), status, Max-all button.
                    const COLORREF stepBase=RGB(74,66,80);
                    const int tabW=(cw-12)/2;
                    for(int t=0;t<2;t++){
                        RECT tab{x+4+t*tabW,y+34,x+4+(t+1)*tabW-2,y+56};
                        drawButton(dc,smallFont,tab,t==0?L"Base Skills":L"Specialized",112+t,t==skillEditorMode?themeAccentColor():stepBase,themeTextColor());
                    }
                    RECT rf{x+4,y+60,x+cw-12,y+82};
                    drawButton(dc,smallFont,rf,L"Refresh skills",111,stepBase,themeTextColor());
                    bool anyRow=false;
                    for(int r=0;r<SKILL_ROWS;r++){
                        const int ri=skillVisibleIndex(r);if(ri<0)break;
                        anyRow=true;
                        const SkillRow& sr=skillRows[ri];
                        const int top=y+86+r*25,x1=x+cw-12;
                        const bool hasX=skillEditorMode==1;   // X only on the Specialized tab
                        RECT xb{x1-22,top,x1,top+22},mxb{hasX?xb.left-40:x1-38,top,hasX?xb.left-2:x1,top+22},plb{mxb.left-30,top,mxb.left-2,top+22},mnb{plb.left-30,top,plb.left-2,top+22};
                        const bool armed=!skillArmedName.empty()&&skillArmedName==sr.name;
                        wchar_t tv[64];swprintf(tv,64,L"%ls  %d/%d",sr.name.c_str(),sr.level,sr.max);
                        textGdi(dc,smallFont,tv,x+6,top+3,mnb.left-x-10,20,themeTextColor());
                        drawButton(dc,smallFont,mnb,L"-",120+r*3,stepBase,themeTextColor());
                        drawButton(dc,smallFont,plb,L"+",121+r*3,stepBase,themeTextColor());
                        drawButton(dc,smallFont,mxb,L"Max",122+r*3,themeAccentColor(),themeTextColor());
                        if(hasX)drawButton(dc,smallFont,xb,L"X",400+r,armed?RGB(170,50,50):stepBase,themeTextColor());
                    }
                    if(!anyRow)textGdi(dc,smallFont,skillRows.empty()?L"Press Refresh while in game":L"No skills in this tab",x+6,y+90,cw-14,20,themeMutedText());
                    statusMarquee(dc,smallFont,skillStatus,x+5,y+214,cw-14,themeMutedText(),6);
                    RECT mb{x+4,y+240,x+cw-12,y+264};
                    drawButton(dc,smallFont,mb,skillHookReady?L"Instant Max Skill Level (all)":L"Max Skill Level (unavailable)",110,skillHookReady?themeAccentColor():RGB(65,62,70),themeTextColor());
                }else if(activeCategory==4&&k==4){
                    // Spawn Vehicle: [<] family [>], [<] variant [>], Spawn / Load button + register toggle, status.
                    if(!vehMaskRequested&&gameWindow){vehMaskRequested=true;PostMessageW(gameWindow,WM_ADREENA_VMASK,0,0);}
                    const VehicleFamily& vf=vehicleFamilies[vehFamilySel];
                    const int boxW=26;
                    const COLORREF arrowBase=RGB(74,66,80);
                    for(int r=0;r<2;r++){
                        const int rowY=y+33+r*25;
                        RECT lb{x+4,rowY,x+4+boxW,rowY+22},rb{x+cw-12-boxW,rowY,x+cw-12,rowY+22},nb{x+4+boxW+4,rowY,x+cw-12-boxW-4,rowY+22};
                        drawButton(dc,smallFont,lb,L"<",20+r*2,arrowBase,themeTextColor());
                        drawButton(dc,smallFont,rb,L">",21+r*2,arrowBase,themeTextColor());
                        HBRUSH nbr=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&nb,nbr);DeleteObject(nbr);
                        if(r==0)textGdi(dc,smallFont,vf.name,nb.left+6,nb.top+2,nb.right-nb.left-10,20,themeTextColor());
                        else{wchar_t t[96];swprintf(t,96,L"%ls (%d/%d)",vehicleDefs[vf.first+vehVariantSel].variant,vehVariantSel+1,vf.count);
                             textGdi(dc,smallFont,t,nb.left+6,nb.top+2,nb.right-nb.left-10,20,themeTextColor());}
                    }
                    const bool needLoad=vehLoadedKnown&&!vehLoadedFlags[vf.first+vehVariantSel];
                    RECT sp{x+4,y+83,x+cw-12-86,y+105};
                    drawButton(dc,smallFont,sp,needLoad?L"Load":L"Spawn vehicle",24,needLoad?RGB(176,120,40):themeAccentColor(),themeTextColor());
                    RECT rg{x+cw-12-82,y+83,x+cw-12,y+105};
                    drawButton(dc,smallFont,rg,vehRegisterNew?L"Register: ON":L"Register: OFF",25,vehRegisterNew?themeAccentColor():RGB(74,66,80),themeTextColor());
                    statusMarquee(dc,smallFont,vehSpawnStatus,x+5,y+110,cw-14,themeMutedText(),0);
                    // ---- Bounty Delivery: a sub-section of Spawn Vehicle (bold heading, no underline) ----
                    {
                        const int by=y+130;
                        textGdi(dc,normal,L"Bounty Delivery",x,by,cw-26,26,themeTextColor());
                        const int bx=26;
                        RECT lb{x+4,by+30,x+4+bx,by+54},rb{x+cw-12-bx,by+30,x+cw-12,by+54},nb{x+4+bx+4,by+30,x+cw-12-bx-4,by+54};
                        drawButton(dc,smallFont,lb,L"<",30,RGB(74,66,80),themeTextColor());
                        drawButton(dc,smallFont,rb,L">",31,RGB(74,66,80),themeTextColor());
                        HBRUSH nbr=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&nb,nbr);DeleteObject(nbr);
                        textGdi(dc,smallFont,bountyDefs[bountySel].label,nb.left+6,nb.top+3,nb.right-nb.left-10,20,themeTextColor());
                        RECT btn{x+4,by+60,x+cw-12,by+86};
                        drawButton(dc,smallFont,btn,L"Request delivery",32,themeAccentColor(),themeTextColor());
                        statusMarquee(dc,smallFont,bountyStatus,x+5,by+93,cw-14,themeMutedText(),1);
                    }
                }else if(activeCategory==4&&k==5){
                    // Trunk Size: [-10][-1] value [+1][+10], Read current / Apply, "apply on spawn" toggle, status.
                    RECT tr[8];trunkLayout(x,y,cw,tr);
                    const COLORREF stepBase=RGB(74,66,80);
                    drawButton(dc,smallFont,tr[0],L"-10",60,stepBase,themeTextColor());
                    drawButton(dc,smallFont,tr[1],L"-1",61,stepBase,themeTextColor());
                    drawButton(dc,smallFont,tr[3],L"+1",62,stepBase,themeTextColor());
                    drawButton(dc,smallFont,tr[4],L"+10",63,stepBase,themeTextColor());
                    HBRUSH nbr=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&tr[2],nbr);DeleteObject(nbr);
                    wchar_t tv[48];swprintf(tv,48,L"Slots: %d",trunkValue);
                    textGdi(dc,smallFont,tv,tr[2].left+6,tr[2].top+2,tr[2].right-tr[2].left-8,20,themeTextColor());
                    drawButton(dc,smallFont,tr[5],L"Read current",64,stepBase,themeTextColor());
                    drawButton(dc,smallFont,tr[6],L"Apply to vehicle",65,themeAccentColor(),themeTextColor());
                    drawButton(dc,smallFont,tr[7],trunkOnSpawn?L"Use on spawned vehicles: ON":L"Use on spawned vehicles: OFF",66,trunkOnSpawn?themeAccentColor():stepBase,themeTextColor());
                    statusMarquee(dc,smallFont,trunkStatus,x+5,y+110,cw-14,themeMutedText(),4);
                }else if(activeCategory==5&&k==0){
                    // Zombie Spawner: one button per zombie (click = spawn, or load if the class is not in memory), hearts last, then heart stats.
                    if(!zombieMaskRequested&&gameWindow){zombieMaskRequested=true;PostMessageW(gameWindow,WM_ADREENA_ZMASK,0,0);}
                    const COLORREF stepBase=RGB(74,66,80);
                    {
                        HGDIOBJ prevFont=SelectObject(dc,smallFont);
                        for(int i=0;i<zombieCount;i++){
                            const int zi=zombieOrder[i];
                            const bool needLoad=zombieLoadedKnown&&!zombieLoadedFlags[zi];
                            std::wstring lab=zombieDefs[zi].label;if(needLoad)lab+=L"  (Load)";
                            SIZE ts{};GetTextExtentPoint32W(dc,lab.c_str(),(int)lab.size(),&ts);
                            int bw=ts.cx+2*14;   // label + consistent 14 px padding on each side
                            if(bw>cw-16)bw=cw-16;
                            zombieBtnW[i]=bw;
                            RECT zb{x+4,y+zombieRowTop(i),x+4+bw,y+zombieRowTop(i)+22};
                            drawButton(dc,smallFont,zb,lab.c_str(),100+i,needLoad?RGB(176,120,40):themeAccentColor(),themeTextColor());
                        }
                        SelectObject(dc,prevFont);
                    }
                    RECT tg{x+4,y+393,x+cw-12,y+415};
                    drawButton(dc,smallFont,tg,zombieCustom?L"Heart stats (hearts only): ON":L"Heart stats (hearts only): OFF",53,zombieCustom?themeAccentColor():stepBase,themeTextColor());
                    const wchar_t* hl[5]={L"-100",L"-10",L"",L"+10",L"+100"};
                    const wchar_t* ml[5]={L"-1",L"-0.1",L"",L"+0.1",L"+1"};
                    for(int row=0;row<3;row++){
                        RECT sr[5];stepperRow(x,cw,y+420+row*25,sr,row==0?38:32);
                        const wchar_t** lab=row==0?hl:ml;
                        for(int i=0;i<5;i++){
                            if(i==2)continue;
                            drawButton(dc,smallFont,sr[i],lab[i],70+row*4+(i<2?i:i-1),stepBase,themeTextColor());
                        }
                        HBRUSH vb=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&sr[2],vb);DeleteObject(vb);
                        wchar_t tv[48];
                        if(row==0)swprintf(tv,48,L"HP: %d",zombieHP);
                        else if(row==1)swprintf(tv,48,L"Melee x%d.%d",zombieMeleeT/10,zombieMeleeT%10);
                        else swprintf(tv,48,L"Ranged x%d.%d",zombieRangedT/10,zombieRangedT%10);
                        {   const int boxW2=sr[2].right-sr[2].left,tw2=measureTextW(dc,smallFont,tv);
                            const int tx2=sr[2].left+(tw2<boxW2?(boxW2-tw2)/2:2);   // centred; never clipped on the left
                            textGdi(dc,smallFont,tv,tx2,sr[2].top+2,boxW2-(tx2-sr[2].left)-1,20,zombieCustom?themeTextColor():themeMutedText());}
                    }
                    statusMarquee(dc,smallFont,spawnStatus,x+5,y+498,cw-14,themeMutedText(),3);
                }else if(activeCategory==5&&k==5){
                    const COLORREF stepBase=RGB(74,66,80);
                    {RECT ub{x+4,y+34,x+cw-12,y+58};   // Unlimited Spawning (moved here from Utilities; required by every horde button)
                     drawButton(dc,smallFont,ub,unlimitedSpawn?L"Unlimited spawning: ON":L"Unlimited spawning: OFF",90,unlimitedSpawn?themeAccentColor():stepBase,themeTextColor());}
                    const int hy=y+62;
                    const wchar_t* hl[3]={L"Spawn Small Horde (15)",L"Spawn Medium Horde (25)",L"Spawn Massive Horde (50)"};
                    for(int bi=0;bi<3;bi++){RECT hb{x+4,hy+bi*28,x+cw-12,hy+bi*28+24};drawButton(dc,smallFont,hb,hl[bi],260+bi,themeAccentColor(),themeTextColor());}
                    RECT cb{x+4,hy+3*28+4,x+cw-12,hy+3*28+28};
                    drawButton(dc,smallFont,cb,L"Clear Zombies (100 m)",264,stepBase,themeTextColor());
                    statusMarquee(dc,smallFont,hordeStatus,x+5,hy+3*28+36,cw-14,themeMutedText(),15);
}else if(activeCategory==1&&k==0){
                    const COLORREF stepBase=RGB(74,66,80);
                    const std::wstring curId=(wpnSel>=0&&wpnSel<(int)wpnList.size())?wpnList[wpnSel]:L"";
                    if(curId!=wpnBmpFor){if(wpnBmp){DeleteObject(wpnBmp);wpnBmp=nullptr;}wpnBmp=loadWeaponBitmap(curId,wpnBmpW,wpnBmpH);wpnBmpFor=curId;}
                    const int BTN_W=112,BTN_H=24,BTN_GAP=8,IMG_H=152;
                    RECT ib{x+4,y+34,x+cw-12-BTN_W-10,y+34+IMG_H};
                    {HBRUSH ibr=CreateSolidBrush(RGB(42,37,46));FillRect(dc,&ib,ibr);DeleteObject(ibr);}
                    if(wpnBmp&&wpnBmpW>0&&wpnBmpH>0){
                        const int bw=ib.right-ib.left-8,bh=ib.bottom-ib.top-8;
                        int dw=bw,dh=bw*wpnBmpH/wpnBmpW;if(dh>bh){dh=bh;dw=bh*wpnBmpW/wpnBmpH;}
                        HDC mdc=CreateCompatibleDC(dc);HGDIOBJ old=SelectObject(mdc,wpnBmp);
                        SetStretchBltMode(dc,HALFTONE);SetBrushOrgEx(dc,0,0,nullptr);
                        StretchBlt(dc,ib.left+4+(bw-dw)/2,ib.top+4+(bh-dh)/2,dw,dh,mdc,0,0,wpnBmpW,wpnBmpH,SRCCOPY);
                        SelectObject(mdc,old);DeleteDC(mdc);
                    }else textGdi(dc,smallFont,curId.empty()?L"No weapon":L"No image yet",ib.left+6,ib.top+40,ib.right-ib.left-12,20,themeMutedText());
                    const int rx=x+4,rw=cw-16;
                    wchar_t line[96];
                    if(curId.empty())swprintf(line,96,L"No weapon selected");else swprintf(line,96,L"%ls%ls",weaponBaseName(curId).c_str(),wpnSel==0?L"  (equipped)":L"");
                    textGdi(dc,smallFont,line,rx,y+34+IMG_H+6,rw,20,themeTextColor());
                    if(!curId.empty()){
                        if(wpnBmp)swprintf(line,96,L"Weapon %d of %d",wpnSel+1,(int)wpnList.size());
                        else swprintf(line,96,L"Add WeaponImages\\%ls.png",weaponBaseName(curId).c_str());
                        textGdi(dc,smallFont,line,rx,y+34+IMG_H+28,rw,20,themeMutedText());
                    }
                    {const wchar_t* lb[4]={L"< Prev",L"Next >",L"Equipped",L"Refresh"};
                     for(int b2=0;b2<4;b2++){RECT br{x+cw-12-BTN_W,y+34+b2*(BTN_H+BTN_GAP),x+cw-12,y+34+b2*(BTN_H+BTN_GAP)+BTN_H};drawButton(dc,smallFont,br,lb[b2],213+b2,b2==3?themeAccentColor():stepBase,themeTextColor());}}
                }else if(activeCategory==0&&k==5){
                    const COLORREF stepBase=RGB(74,66,80);
                    for(int i=0;i<3;i++){
                        RECT bx{x+4,y+34+i*28,x+cw-12,y+56+i*28};
                        const int id=201+i;wchar_t* tb=i==0?nameFirst:i==1?nameLast:nameNick;
                        HBRUSH ibr=CreateSolidBrush(editingNum==id?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&bx,ibr);DeleteObject(ibr);
                        wchar_t line[48];
                        if(editingNum==id)swprintf(line,48,L"%ls_",tb);
                        else if(tb[0])swprintf(line,48,L"%ls",tb);
                        else swprintf(line,48,i==0?L"First name":i==1?L"Last name":L"Nickname");
                        textGdi(dc,smallFont,line,bx.left+6,bx.top+2,bx.right-bx.left-8,20,(tb[0]||editingNum==id)?themeTextColor():themeMutedText());
                    }
                    {const int bw=cw-16,aw=bw*50/100;
                     RECT lb{x+4,y+122,x+4+aw-3,y+144},ab{x+4+aw+3,y+122,x+4+bw,y+144};
                     drawButton(dc,smallFont,lb,L"Load current",210,stepBase,themeTextColor());
                     drawButton(dc,smallFont,ab,L"Apply",211,themeAccentColor(),themeTextColor());}
                    statusMarquee(dc,smallFont,nameStatus,x+5,y+150,cw-14,themeMutedText(),13);
                }else if(activeCategory==2&&k==0){
                    {RECT lr{x+4,y+34,x+cw-12,y+58};
                     drawButton(dc,smallFont,lr,resLoaded?L"Reload Resources in Base":L"Load Resources in Base",225,resLoaded?RGB(74,66,80):RGB(176,120,40),themeTextColor());}
                    // Fixed rows: [name: current] [amount box] [Set]
                    for(int i=0;i<RES_ROWS;i++){
                        const int ry=y+64+i*28;
                        RECT ib{x+cw-118,ry,x+cw-62,ry+22},sb{x+cw-56,ry,x+cw-12,ry+22};
                        wchar_t rt[96];const float cur=resAmountFor(resUiNames[i]);
                        if(cur>=0.f)swprintf(rt,96,L"%ls: %d",resUiNames[i],(int)(cur+0.5f));else swprintf(rt,96,L"%ls",resUiNames[i]);
                        textGdi(dc,smallFont,rt,x+6,ry+2,ib.left-x-10,20,themeTextColor());
                        HBRUSH ibr=CreateSolidBrush(editingNum==i?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&ib,ibr);DeleteObject(ibr);
                        wchar_t at[24];if(editingNum==i)swprintf(at,24,L"%ls_",numDraft);else swprintf(at,24,L"%d",resAmtVals[i]);
                        textGdi(dc,smallFont,at,ib.left+5,ib.top+2,ib.right-ib.left-6,20,themeTextColor());
                        drawButton(dc,smallFont,sb,L"Set",190+i,themeAccentColor(),themeTextColor());
                    }
                    statusMarquee(dc,smallFont,resStatus,x+5,y+64+RES_ROWS*28+2,cw-14,themeMutedText(),9);
                }else if(activeCategory==2&&k==3){
                    const COLORREF stepBase=RGB(74,66,80);
                    {RECT cb{x+4,y+34,x+cw-12-58,y+58};
                     drawButton(dc,smallFont,cb,L"",220,stepBase,themeTextColor());
                     wchar_t cl[96];swprintf(cl,96,L"%ls",itemCatNames[itemCat]);
                     textGdi(dc,smallFont,cl,cb.left+8,cb.top+3,cb.right-cb.left-30,20,themeTextColor());
                     textGdi(dc,smallFont,L"v",cb.right-17,cb.top+3,15,20,themeTextColor());
                     dropField[3]=cb;dropFieldSet[3]=true;}
                    {RECT lb{x+cw-12-52,y+34,x+cw-12,y+58};drawButton(dc,smallFont,lb,L"Load",224,itemAll.empty()?RGB(176,120,40):stepBase,themeTextColor());}
                    {RECT fb{x+4,y+66,x+cw-12,y+88};HBRUSH fbr=CreateSolidBrush(editingNum==204?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&fb,fbr);DeleteObject(fbr);
                     wchar_t fl[48];if(editingNum==204)swprintf(fl,48,L"%ls_",itemFilter);else if(itemFilter[0])swprintf(fl,48,L"%ls",itemFilter);else swprintf(fl,48,L"Search items");
                     textGdi(dc,smallFont,fl,fb.left+6,fb.top+2,fb.right-fb.left-8,20,(itemFilter[0]||editingNum==204)?themeTextColor():themeMutedText());}
                    const std::vector<int> flt=itemFiltered();
                    const int pages=flt.empty()?1:((int)flt.size()+ITEM_ROWS-1)/ITEM_ROWS;
                    if(itemPage>=pages)itemPage=pages-1;if(itemPage<0)itemPage=0;
                    for(int i=0;i<ITEM_ROWS;i++){
                        RECT rb{x+4,y+96+i*22,x+cw-12,y+96+i*22+20};
                        const int fi=itemPage*ITEM_ROWS+i;
                        const bool have=fi<(int)flt.size();const bool sel=have&&flt[fi]==itemSel;
                        HBRUSH rbr=CreateSolidBrush(sel?themeAccentColor():RGB(42,37,46));FillRect(dc,&rb,rbr);DeleteObject(rbr);
                        if(have){std::wstring lbl=weaponBaseName(itemAll[flt[fi]].name);if(itemAll[flt[fi]].internal)lbl+=L"  (internal)";
                            textGdi(dc,smallFont,lbl.c_str(),rb.left+6,rb.top+1,rb.right-rb.left-8,18,itemAll[flt[fi]].internal&&!sel?themeMutedText():themeTextColor());}
                    }
                    {RECT pb1{x+4,y+210,x+4+64,y+232},pb2{x+cw-12-64,y+210,x+cw-12,y+232};
                     drawButton(dc,smallFont,pb1,L"< Page",221,stepBase,themeTextColor());
                     drawButton(dc,smallFont,pb2,L"Page >",222,stepBase,themeTextColor());
                     wchar_t pl[64];swprintf(pl,64,L"%d / %d  (%d)",itemPage+1,pages,(int)flt.size());
                     textGdi(dc,smallFont,pl,pb1.right+8,pb1.top+2,pb2.left-pb1.right-16,20,themeMutedText());}
                    {RECT ib{x+4,y+240,x+84,y+264};HBRUSH ibr=CreateSolidBrush(editingNum==101?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&ib,ibr);DeleteObject(ibr);
                     wchar_t at[24];if(editingNum==101)swprintf(at,24,L"%ls_",numDraft);else swprintf(at,24,L"%d",itemCount);
                     textGdi(dc,smallFont,at,ib.left+6,ib.top+3,ib.right-ib.left-8,20,themeTextColor());
                     RECT gb{x+cw-12-88,y+240,x+cw-12,y+264};drawButton(dc,smallFont,gb,L"Give",223,themeAccentColor(),themeTextColor());}
                    statusMarquee(dc,smallFont,itemStatus,x+5,y+272,cw-14,themeMutedText(),10);
                }else if(activeCategory==2&&k==2){
                    const COLORREF stepBase=RGB(74,66,80);
                    RECT ib{x+4,y+34,x+84,y+56};
                    {HBRUSH ibr=CreateSolidBrush(editingNum==100?RGB(58,50,66):RGB(42,37,46));FillRect(dc,&ib,ibr);DeleteObject(ibr);
                     wchar_t at[24];if(editingNum==100)swprintf(at,24,L"%ls_",numDraft);else swprintf(at,24,L"%d",timerVal);
                     textGdi(dc,smallFont,at,ib.left+6,ib.top+2,ib.right-ib.left-8,20,themeTextColor());}
                    textGdi(dc,smallFont,L"seconds",ib.right+8,ib.top+2,cw-110,20,themeMutedText());
                    const int half=(cw-16)/2;
                    RECT sbt{x+4,y+62,x+4+half-3,y+86},abt{x+4+half+3,y+62,x+cw-12,y+86};
                    drawButton(dc,smallFont,sbt,L"Subtract",206,stepBase,themeTextColor());
                    drawButton(dc,smallFont,abt,L"Add",207,themeAccentColor(),themeTextColor());
                    statusMarquee(dc,smallFont,timerStatus,x+5,y+92,cw-14,themeMutedText(),11);
                }else{
                int n=0;const auto& fl=featureLists[activeCategory];
                const int rowW=(activeCategory==0&&k==0)?sectionHalfW():cw;   // Protection toggles stay in the left half
                for(int ix:menuSections[activeCategory][k]){if(ix<0||ix>=fl.count)continue;
                    int rowY=y+32+(int)roundf((n-offset)*SECTION_ROW_H);
                    if(rowY+SECTION_ROW_H>contentTop&&rowY<contentBottom){
                        int sid=featureSliderId(activeCategory,k,n);
                        if(searchHLActive()&&searchHLCat==activeCategory&&searchHLSec==k&&searchHLRow==n){
                            uiAnimSeen=true;
                            if(searchHLBlinkOn()){HBRUSH hb=CreateSolidBrush(themeAccentColor());RECT hr{x,rowY,x+rowW-8,rowY+SECTION_ROW_H-2};FrameRect(dc,&hr,hb);DeleteObject(hb);}
                        }
                        if(sid>=0)featureSliderRow(x,rowY,rowW,sid);
                        else toggleRow(x,rowY,rowW,fl.items[ix].label,fl.items[ix].id);
                    }
                    n++;
                }
                if(!n)textGdi(dc,smallFont,L"Future options",x+5,y+36,cw-12,25,themeMutedText());
                if(activeCategory==0&&k==0){
                    // Recovery buttons on the right half of Protection.
                    const COLORREF stepBase=RGB(74,66,80);
                    const int rx=x+sectionHalfW()+SECTION_GAP_X;
                    RECT c1{rx,y+32,x+cw-12,y+56},c2{rx,y+62,x+cw-12,y+86},c3{rx,y+92,x+cw-12,y+116};
                    drawButton(dc,smallFont,c3,L"Heal Injuries",155,stepBase,themeTextColor());
                    drawButton(dc,smallFont,c1,L"Cure Sickness",152,stepBase,themeTextColor());
                    drawButton(dc,smallFont,c2,L"Cure Trauma",153,stepBase,themeTextColor());
                }
                if(activeCategory==5&&k==1){
                    // Haven Protocol (moved from Utilities): bottom button of Fun; its result shows briefly underneath.
                    RECT hb{x+4,y+68,x+cw-12,y+92};
                    drawButton(dc,smallFont,hb,L"Haven Protocol: Expose",208,themeAccentColor(),themeTextColor());
                    if(havenStatusUntil&&!havenStatus.empty())statusMarquee(dc,smallFont,havenStatus,x+5,y+97,cw-14,themeMutedText(),12);
                }
                if(activeCategory==4&&k==0){
                    // Repair and Refuel: bold heading (no underline), same controls as before.
                    const int hy=y+96;
                    textGdi(dc,normal,L"Repair and Refuel",x,hy,cw-26,26,themeTextColor());
                    for(int bi=0;bi<2;bi++){
                        RECT btn{x+4,hy+30+bi*30,x+cw-12,hy+30+bi*30+26};
                        drawButton(dc,smallFont,btn,bi==0?L"Refuel vehicle":L"Repair vehicle",40+bi,themeAccentColor(),themeTextColor());
                    }
                    statusMarquee(dc,smallFont,vehicleStatus,x+5,hy+93,cw-14,themeMutedText(),2);
                }
                }
            }
            // Draw the toggles while the content clip is active.
            drawVecLayer(dc,vec);
            RestoreDC(dc,clipSave);
            // Track and thumb are visible whenever this section has hidden rows.
            int count=sectionItemCount(activeCategory,k);
            if(count>SECTION_VIEW_ROWS&&sectionMaxScroll(activeCategory,k)>0.f){
                int trackX=x+cw-3,trackTop=contentTop,trackH=contentBottom-contentTop;
                RECT track{trackX,trackTop,trackX+3,contentBottom};
                HBRUSH brush=CreateSolidBrush(RGB(70,65,74));FillRect(dc,&track,brush);DeleteObject(brush);
                int thumbH=(std::max)(18,(int)roundf(trackH*3.f/count));
                int thumbY=trackTop+(int)roundf((trackH-thumbH)*offset/sectionMaxScroll(activeCategory,k));
                RECT thumb{trackX,thumbY,trackX+3,thumbY+thumbH};
                brush=CreateSolidBrush(themeAccentColor());FillRect(dc,&thumb,brush);DeleteObject(brush);
            }
            rule(x,y+secH,cw);
        }
        if(dropActive()){
            // Expanded appearance list, drawn over the page (same look as the banner list: 25 px rows, accent scrollbar thumb).
            RECT pr;int rows=0,total=0;
            if(dropPopupGeom(pr,rows,total)){
                const int maxScroll=(std::max)(0,total-rows);
                if(dropScroll>maxScroll)dropScroll=maxScroll;if(dropScroll<0)dropScroll=0;
                const int sel=dropSelected();
                for(int j=0;j<rows;j++){
                    const int item=dropScroll+j;
                    RECT row{pr.left,pr.top+j*BANNER_ROW_HEIGHT,pr.right,pr.top+(j+1)*BANNER_ROW_HEIGHT};
                    HBRUSH fill=CreateSolidBrush(item==sel?RGB(79,49,66):RGB(35,32,38));FillRect(dc,&row,fill);DeleteObject(fill);
                    const std::wstring tx=dropItemText(item);
                    textGdi(dc,smallFont,tx.c_str(),row.left+8,row.top+3,row.right-row.left-(total>rows?22:14),22,themeTextColor());
                }
                if(total>rows){
                    const int hgt=rows*BANNER_ROW_HEIGHT,trackX=pr.right-6;
                    const int thumbH=(std::max)(14,hgt*rows/total);
                    const int thumbY=pr.top+(maxScroll?dropScroll*(hgt-thumbH)/maxScroll:0);
                    RECT track{trackX,pr.top,pr.right,pr.top+hgt};HBRUSH tb=CreateSolidBrush(RGB(68,61,70));FillRect(dc,&track,tb);DeleteObject(tb);
                    RECT thumb{trackX,thumbY,pr.right,thumbY+thumbH};HBRUSH hb=CreateSolidBrush(themeAccentColor());FillRect(dc,&thumb,hb);DeleteObject(hb);
                }
                HPEN bp=mkPen(1,themeAccentColor(),true);HGDIOBJ bpOld=SelectObject(dc,bp),bbOld=SelectObject(dc,GetStockObject(NULL_BRUSH));
                Rectangle(dc,pr.left,pr.top,pr.right,pr.bottom);
                SelectObject(dc,bbOld);SelectObject(dc,bpOld);DeleteObject(bp);
            }
        }
        RestoreDC(dc,pageClip);
        // Global page scroll indicator, shown only when groups extend below the viewport.
        if(pageMaxScroll()>0){
            int trackTop=top,trackH=MH-34-top;
            int thumbH=(std::max)(22,(int)roundf(trackH*(float)trackH/(trackH+pageMaxScroll())));
            int thumbY=trackTop+(int)roundf((trackH-thumbH)*pageScroll[activeCategory]/pageMaxScroll());
            RECT track{MW-12,trackTop,MW-5,trackTop+trackH};   // same rail geometry as Settings (thumb unchanged)
            HBRUSH brush=CreateSolidBrush(RGB(70,65,74));FillRect(dc,&track,brush);DeleteObject(brush);
            RECT thumb{MW-12,thumbY,MW-5,thumbY+thumbH};
            brush=CreateSolidBrush(themeAccentColor());FillRect(dc,&thumb,brush);DeleteObject(brush);
        }
    }
    drawVecLayer(dc,vec); // settings controls, if any
    {   // ===== menu search: magnifier, field, results =====
        if(activeCategory==6&&searchHLActive()&&searchHLCat==6){
            uiAnimSeen=true;
            if(searchHLBlinkOn()){
                int sv=SaveDC(dc);IntersectClipRect(dc,SIDE+1,HEADER_H+50,MW-8,MH-34);
                HBRUSH hb=CreateSolidBrush(themeAccentColor());
                RECT hr{SIDE+14,HEADER_H+53+searchHLRow-settingsScroll-4,MW-20,HEADER_H+53+searchHLRow-settingsScroll+30};
                FrameRect(dc,&hr,hb);DeleteObject(hb);RestoreDC(dc,sv);
            }
        }
        const RECT ic=searchIconRect();
        if(searchOpen)vecRRect(vec,(float)ic.left,(float)ic.top,(float)ic.right,(float)ic.bottom,4.f,themeLineColor());
        vecEllipse(vec,(float)ic.left+5.5f,(float)ic.top+4.5f,(float)ic.left+16.5f,(float)ic.top+15.5f,1.8f,themeTextColor());
        vecLine(vec,(float)ic.left+15.2f,(float)ic.top+14.2f,(float)ic.left+19.5f,(float)ic.top+18.5f,2.2f,themeTextColor());
        drawVecLayer(dc,vec);
        if(searchOpen){
            const RECT fr=searchFieldRect();
            HBRUSH fb=CreateSolidBrush(RGB(58,50,66));FillRect(dc,&fr,fb);DeleteObject(fb);
            HBRUSH ab=CreateSolidBrush(themeAccentColor());FrameRect(dc,&fr,ab);DeleteObject(ab);
            wchar_t tl[40];
            if(searchBuf[0])swprintf(tl,40,L"%ls_",searchBuf);else wcscpy_s(tl,40,L"Search menu_");
            textGdi(dc,smallFont,tl,fr.left+6,fr.top+3,fr.right-fr.left-10,20,searchBuf[0]?themeTextColor():themeMutedText());
            if(searchPopupVisible()){
                const RECT pr=searchPopupRect();
                HBRUSH pb=CreateSolidBrush(RGB(36,32,40));FillRect(dc,&pr,pb);DeleteObject(pb);
                HBRUSH lb=CreateSolidBrush(themeLineColor());FrameRect(dc,&pr,lb);DeleteObject(lb);
                if(searchResults.empty())textGdi(dc,smallFont,L"No matches",pr.left+8,pr.top+4,pr.right-pr.left-16,20,themeMutedText());
                else{
                    const int rows=searchPopupRowsShown();
                    if(searchScroll>(int)searchResults.size()-rows)searchScroll=(std::max)(0,(int)searchResults.size()-rows);
                    for(int i=0;i<rows;i++){
                        const int ri=searchScroll+i;if(ri>=(int)searchResults.size())break;
                        const SearchEntry& e=searchEntries[searchResults[ri]];
                        RECT rr{pr.left+1,pr.top+1+i*BANNER_ROW_HEIGHT,pr.right-1,pr.top+1+(i+1)*BANNER_ROW_HEIGHT};
                        if(ri==0){HBRUSH hb=CreateSolidBrush(RGB(52,46,60));FillRect(dc,&rr,hb);DeleteObject(hb);}
                        textGdi(dc,smallFont,e.label.c_str(),rr.left+7,rr.top+3,150,20,themeTextColor());
                        textGdi(dc,smallFont,e.where.c_str(),rr.left+160,rr.top+3,rr.right-rr.left-166,20,themeMutedText());
                    }
                }
            }
        }
    }
    DeleteObject(linePen);
    HPEN footerPen=mkPen(1,themeLineColor(),true);HGDIOBJ oldFooter=SelectObject(dc,footerPen);
    MoveToEx(dc,0,MH-33,nullptr);LineTo(dc,MW,MH-33);SelectObject(dc,oldFooter);DeleteObject(footerPen);
    wchar_t footerLeft[96];swprintf(footerLeft,96,L"ADREENA  %ls",ADREENA_VERSION);
    textGdi(dc,smallFont,footerLeft,14,MH-30,320,25,themeTextColor());
    {   // divider closing the name + version block (same "|" glyph as the one after Build: Stable)
        const int lw=measureTextW(dc,smallFont,footerLeft);
        textGdi(dc,smallFont,L"|",14+lw+8,MH-30,12,25,themeTextColor());
    }
    {
        std::string whyH;const int hs=adreenaHealthState(whyH);
        static int lastHs=-1;
        if(hs!=lastHs){lastHs=hs;if(hs!=0){std::string msgH="Footer status "+std::string(hs==1?"WARNING: ":"DISCONNECTED: ")+whyH;logMessage(msgH.c_str());}}
        const wchar_t* stText=hs==0?L"Status: Injected":(hs==1?L"Status: Warning":L"Status: Disconnected");
        const COLORREF stCol=hs==0?RGB(80,205,120):(hs==1?RGB(238,190,64):RGB(228,72,72));
        const wchar_t* bt=L"Build: Stable  |";
        HGDIOBJ pf=SelectObject(dc,smallFont);SIZE s1{},s2{};
        GetTextExtentPoint32W(dc,stText,(int)wcslen(stText),&s1);GetTextExtentPoint32W(dc,bt,(int)wcslen(bt),&s2);
        SelectObject(dc,pf);
        const int sx=MW-14-15-s1.cx,cx=sx+s1.cx+6,bx=sx-8-s2.cx;
        textGdi(dc,smallFont,bt,bx,MH-30,s2.cx+8,25,themeTextColor());
        vecFillEllipse(vec,(float)cx,(float)(MH-21),(float)(cx+9),(float)(MH-12),stCol);
        drawVecLayer(dc,vec);
        textGdi(dc,smallFont,stText,sx,MH-30,s1.cx+10,25,themeTextColor());
    }
    // The surface is already at final pixel size: no resampling. GDI leaves alpha at 0, so force opaque.
    const size_t total=(size_t)RW*RH;
    for(size_t k=0;k<total;k++)px[k]|=0xFF000000u;
    // Preserve foreground text, icons, controls, and banner at full opacity.
    // Only exact flat-background pixels get adjustable alpha. GDI text edges
    // are left opaque to avoid accidentally fading labels or icon strokes.
    if(bgOpacity<100){
        const uint32_t bgRgb=((uint32_t)themeBg[0]<<16)|((uint32_t)themeBg[1]<<8)|(uint32_t)themeBg[2];
        const uint32_t bgAlpha=(uint32_t)((bgOpacity*255+50)/100)<<24;
        for(int yy=(int)ceilf(HEADER_H*SS);yy<RH;yy++)
            for(int xx=0;xx<RW;xx++){
                uint32_t& pixel=px[(size_t)yy*RW+xx];
                if((pixel&0x00FFFFFFu)==bgRgb)pixel=bgAlpha|bgRgb;
            }
    }
    // Anti-aliased rounded outer corners (12 logical px radius); transparent outside the silhouette.
    // Colors are not premultiplied (blend state uses SRC_ALPHA), so only alpha changes.
    const float R=12.0f*SS;
    const int ri=(int)ceilf(R);
    for(int cy=0;cy<ri;cy++)for(int cx=0;cx<ri;cx++){
        float dx=R-(cx+0.5f),dy=R-(cy+0.5f);
        float a=R-sqrtf(dx*dx+dy*dy)+0.5f;
        if(a>=1.0f)continue;
        int xs[2]={cx,RW-1-cx},ys[2]={cy,RH-1-cy};
        for(int iy=0;iy<2;iy++)for(int ix=0;ix<2;ix++){
            uint32_t& p=px[(size_t)ys[iy]*RW+xs[ix]];
            p=(a<=0.0f)?0u:(((uint32_t)(a*255.0f)<<24)|(p&0x00FFFFFFu));
        }
    }
    // None means the header is completely transparent, without moving the menu.
    // Apply this after all GDI drawing so no header decorations leave opaque pixels.
    {
        const int headerPixels=(std::min)(RH,(int)lroundf(HEADER_H*SS));
        for(int yy=0;yy<headerPixels;yy++)
            for(int xx=0;xx<RW;xx++)px[(size_t)yy*RW+xx]=0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};HRESULT hr=context->Map(uiTexture,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(SUCCEEDED(hr)){for(int y=0;y<RH;y++)memcpy((uint8_t*)mapped.pData+(size_t)y*mapped.RowPitch,px+(size_t)y*RW,(size_t)RW*4);context->Unmap(uiTexture,0);}
    SelectObject(dc,oldFont);DeleteObject(normal);DeleteObject(smallFont);DeleteObject(heading);
    uiNormalFont=uiSmallFont=uiHeadingFont=nullptr;
    SelectObject(dc,oldBitmap);DeleteObject(bmp);DeleteDC(dc);return SUCCEEDED(hr);
}
// Render the expanded banner list to an independent texture. Its quad can extend
// below the menu without enlarging or changing the menu itself.
bool updateBannerListTexture(int logicalWidth,int rows){
    if(rows<=0)return false;
    const int width=(int)lroundf(logicalWidth*uiScale);
    const int height=(int)lroundf(rows*BANNER_ROW_HEIGHT*uiScale);
    if(width<=0||height<=0)return false;
    static int cachedWidth=-1,cachedHeight=-1,cachedOffset=-1,cachedSelected=-999;
    static int cachedTheme=-1,cachedBannerRevision=-1;
    if(bannerListView&&cachedWidth==width&&cachedHeight==height&&
       cachedOffset==bannerDropdownScroll&&cachedSelected==selectedBanner&&
       cachedTheme==themeRevision&&cachedBannerRevision==bannerRevision)return true;
    D3D11_TEXTURE2D_DESC td{};
    td.Width=width;td.Height=height;td.MipLevels=1;td.ArraySize=1;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DYNAMIC;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    drop(bannerListView);drop(bannerListTexture);
    if(FAILED(device->CreateTexture2D(&td,nullptr,&bannerListTexture)))return false;
    if(FAILED(device->CreateShaderResourceView(bannerListTexture,nullptr,&bannerListView))){drop(bannerListTexture);return false;}
    BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth=width;bmi.bmiHeader.biHeight=-height;
    bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HDC dc=CreateCompatibleDC(nullptr);
    if(!dc)return false;
    HBITMAP bitmap=CreateDIBSection(dc,&bmi,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap||!pixels){if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return false;}
    HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
    SetBkMode(dc,TRANSPARENT);
    HFONT font=CreateFontW((int)lroundf(15*uiScale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
        DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
    HGDIOBJ oldFont=SelectObject(dc,font);
    for(int j=0;j<rows;j++){
        const int item=bannerDropdownScroll+j;
        const int y0=(int)lroundf(j*BANNER_ROW_HEIGHT*uiScale);
        const int y1=(int)lroundf((j+1)*BANNER_ROW_HEIGHT*uiScale);
        const bool selected=(item==0&&selectedBanner==-1)||(item>0&&item-1==selectedBanner);
        RECT row{0,y0,width,y1};
        HBRUSH fill=CreateSolidBrush(selected?RGB(79,49,66):RGB(35,32,38));
        FillRect(dc,&row,fill);DeleteObject(fill);
        const wchar_t* name=item==0?L"None":bannerFiles[item-1].c_str();
        RECT label{(int)lroundf(7*uiScale),y0,width-(int)lroundf(16*uiScale),y1};
        SetTextColor(dc,themeTextColor());
        DrawTextW(dc,name,-1,&label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    }
    if((int)bannerFiles.size()+1>rows){
        const int total=(int)bannerFiles.size()+1;
        const int trackX=width-(int)lroundf(6*uiScale);
        const int thumbH=(std::max)((int)lroundf(14*uiScale),height*rows/total);
        const int maxScroll=total-rows;
        const int thumbY=maxScroll?bannerDropdownScroll*(height-thumbH)/maxScroll:0;
        RECT track{trackX,0,width,height};HBRUSH tb=CreateSolidBrush(RGB(68,61,70));
        FillRect(dc,&track,tb);DeleteObject(tb);
        RECT thumb{trackX,thumbY,width,thumbY+thumbH};
        HBRUSH hb=CreateSolidBrush(themeAccentColor());FillRect(dc,&thumb,hb);DeleteObject(hb);
    }
    // GDI DIB alpha is zero; force opaque for correct blending.
    auto* data=static_cast<uint32_t*>(pixels);
    for(size_t i=0;i<(size_t)width*height;i++)data[i]|=0xFF000000u;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr=context->Map(bannerListTexture,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(SUCCEEDED(hr)){
        for(int y=0;y<height;y++)memcpy((uint8_t*)mapped.pData+(size_t)y*mapped.RowPitch,
            data+(size_t)y*width,(size_t)width*4);
        context->Unmap(bannerListTexture,0);
    }
    SelectObject(dc,oldFont);DeleteObject(font);
    SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
    if(SUCCEEDED(hr)){
        cachedWidth=width;cachedHeight=height;cachedOffset=bannerDropdownScroll;
        cachedSelected=selectedBanner;cachedTheme=themeRevision;cachedBannerRevision=bannerRevision;
    }
    return SUCCEEDED(hr);
}
// Actual popup bounds in logical menu coordinates, allowed below MH.
int bannerPopupRows(){
    int rows=bannerDropdownRows();
    int bw=0,bh=0;
    if(rows<=0||!bufferSize(bw,bh))return rows;
    const int logicalBottom=toLogical(bh-menuY);
    const int start=(HEADER_H+53)+SETTINGS_BANNER_OFF+28;
    const int fit=(std::max)(1,(logicalBottom-start-8)/BANNER_ROW_HEIGHT);
    return (std::min)(rows,fit);
}
// Native-resolution, independent banner texture. No upscaling or stretching.
void updateStandaloneBanner(){
    if(bannerLoadedRevision==bannerRevision)return;
    bannerLoadedRevision=bannerRevision;
    drop(standaloneBannerView);standaloneBannerW=standaloneBannerH=0;
    if(selectedBanner<0||!device)return;
    HBITMAP bitmap=loadSelectedBanner();if(!bitmap)return;
    BITMAP bm{};GetObjectW(bitmap,sizeof(bm),&bm);
    int bw=bm.bmWidth,bh=bm.bmHeight;
    if(bw<=0||bh<=0||bw>8192||bh>8192){DeleteObject(bitmap);return;}
    std::vector<uint32_t> pixels((size_t)bw*bh);
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=bw;bi.bmiHeader.biHeight=-bh;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    HDC dc=GetDC(nullptr);
    bool ok=dc&&GetDIBits(dc,bitmap,0,bh,pixels.data(),&bi,DIB_RGB_COLORS)==bh;
    if(dc)ReleaseDC(nullptr,dc);
    DeleteObject(bitmap);
    if(!ok)return;
    for(auto& pixel:pixels)pixel|=0xFF000000u;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=bw;desc.Height=bh;
    desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels.data();data.SysMemPitch=bw*4;
    ID3D11Texture2D* tex=nullptr;
    if(SUCCEEDED(device->CreateTexture2D(&desc,&data,&tex))){
        device->CreateShaderResourceView(tex,nullptr,&standaloneBannerView);
        drop(tex);
        if(standaloneBannerView){standaloneBannerW=bw;standaloneBannerH=bh;}
    }
}
void drawOverlay(IDXGISwapChain* swap){
    std::lock_guard<std::recursive_mutex> menuGuard(menuLock);
    if(!device||!context)return;
    // Choose the UI scale for this back buffer and (re)create the menu texture if it changed.
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if(FAILED(swap->GetDesc(&swapDesc)))return;
    g_bbW.store((int)swapDesc.BufferDesc.Width,std::memory_order_relaxed);g_bbH.store((int)swapDesc.BufferDesc.Height,std::memory_order_relaxed);
    if(!ensureUiTexture(pickUiScale(swapDesc.BufferDesc.Width,swapDesc.BufferDesc.Height)))return;
    // Menu artwork is cached; cursor is drawn independently every frame at its actual screen position.
    animateScroll();
    int mask=(health.load()?1:0)|(stamina.load()?2:0)|
             (clip.load()?4:0)|0|(meleeWeight.load()?16:0)|
             (weaponDurability.load()?32:0)|(noFatigue.load()?64:0)|
             (noRecoil.load()?128:0)|
             (infiniteResources.load()?512:0)|(infiniteCarFuel.load()?1024:0)|(infiniteAmmo.load()?2048:0)|(vehicleHealthLock.load()?4096:0);
    if(flashId&&GetTickCount64()>=flashUntil){flashId=0;uiCacheValid=false;}   // end of the "pressed" look
    if(havenStatusUntil&&GetTickCount64()>=havenStatusUntil){havenStatusUntil=0;uiCacheValid=false;}   // Haven result line times out
    const bool animTick=uiAnimSeen||(uiMarqueeSeen&&GetTickCount64()-lastAnimDraw>=(ULONGLONG)uiMarqueeMs);                                              // redraw every frame while text scrolls or a button is pressed (even frame pacing = smooth)
    bool stateChanged=animTick||toggleAnimationActive||!uiCacheValid||cachedSidebarWidth!=SIDE||cachedCategory!=activeCategory||
                      cachedScroll!=scrollOffset||cachedFeatureMask!=mask||cachedThemeRevision!=themeRevision||cachedSectionRevision!=sectionRevision;

    if(stateChanged){
        if(!updateTexture())return;
        uiCacheValid=true;cachedSidebarWidth=SIDE;cachedCategory=activeCategory;
        cachedScroll=scrollOffset;cachedFeatureMask=mask;cachedThemeRevision=themeRevision;cachedSectionRevision=sectionRevision;
    }
    updateStandaloneBanner();
    ID3D11Texture2D* backBuffer=nullptr;HRESULT hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&backBuffer);
    if(FAILED(hr))return;
    D3D11_TEXTURE2D_DESC td{};backBuffer->GetDesc(&td);
    ID3D11RenderTargetView* rtv=nullptr;hr=device->CreateRenderTargetView(backBuffer,nullptr,&rtv);drop(backBuffer);
    if(FAILED(hr))return;
    float w=(float)td.Width,h=(float)td.Height;
    menuPosition();float x=(float)menuX,y=(float)menuY;
    float l=2*x/w-1,r=2*(x+TW)/w-1,t=1-2*y/h,b=1-2*(y+TH)/h;
    const float headerPx=(float)lroundf(HEADER_H*uiScale);
    const float panelTop=1-2*(y+headerPx)/h;
    const float headerUv=headerPx/(float)TH;
    Vertex vertices[6]={{l,panelTop,0,headerUv},{r,panelTop,1,headerUv},{l,b,0,1},
                        {l,b,0,1},{r,panelTop,1,headerUv},{r,b,1,1}};
    D3D11_MAPPED_SUBRESOURCE mapped{};hr=context->Map(vertexBuffer,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(FAILED(hr)){drop(rtv);return;}memcpy(mapped.pData,vertices,sizeof(vertices));context->Unmap(vertexBuffer,0);
    // Save and restore the state that this draw changes, so the game's next frame is unaffected.
    ID3D11RenderTargetView* prevRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* prevDSV=nullptr;
    // Render targets AND the output-stage UAVs: restoring with OMSetRenderTargets alone would unbind the game's UAVs.
    ID3D11UnorderedAccessView* prevUAV[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    UINT uavStart=0,uavCount=0;
    {
        context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,prevRTV,&prevDSV,0,D3D11_PS_CS_UAV_REGISTER_COUNT,prevUAV);
        UINT firstRtvEnd=0;for(UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;i++)if(prevRTV[i])firstRtvEnd=i+1;
        for(UINT i=0;i<D3D11_PS_CS_UAV_REGISTER_COUNT;i++)if(prevUAV[i]){if(!uavCount)uavStart=i;uavCount=i-uavStart+1;}
        if(uavCount&&uavStart<firstRtvEnd){uavCount=0;}   // invalid combination: never seen at present time; fall back to plain restore
    }
    ID3D11BlendState* prevBlend=nullptr;FLOAT blendFactor[4]{};UINT sampleMask=0;context->OMGetBlendState(&prevBlend,blendFactor,&sampleMask);
    ID3D11DepthStencilState* prevDepth=nullptr;UINT stencilRef=0;context->OMGetDepthStencilState(&prevDepth,&stencilRef);
    ID3D11RasterizerState* prevRaster=nullptr;context->RSGetState(&prevRaster);
    UINT nvp=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};context->RSGetViewports(&nvp,vps);
    UINT nsc=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};context->RSGetScissorRects(&nsc,scissors);
    ID3D11InputLayout* prevLayout=nullptr;context->IAGetInputLayout(&prevLayout);
    D3D11_PRIMITIVE_TOPOLOGY prevTopology{};context->IAGetPrimitiveTopology(&prevTopology);
    ID3D11Buffer* prevVB=nullptr;UINT prevStride=0,prevOffset=0;context->IAGetVertexBuffers(0,1,&prevVB,&prevStride,&prevOffset);
    ID3D11VertexShader* prevVS=nullptr;ID3D11PixelShader* prevPS=nullptr;
    context->VSGetShader(&prevVS,nullptr,nullptr);context->PSGetShader(&prevPS,nullptr,nullptr);
    ID3D11ShaderResourceView* prevSRV=nullptr;context->PSGetShaderResources(0,1,&prevSRV);
    ID3D11SamplerState* prevSampler=nullptr;context->PSGetSamplers(0,1,&prevSampler);
    ID3D11GeometryShader* prevGS=nullptr;ID3D11HullShader* prevHS=nullptr;ID3D11DomainShader* prevDS=nullptr;
    context->GSGetShader(&prevGS,nullptr,nullptr);context->HSGetShader(&prevHS,nullptr,nullptr);context->DSGetShader(&prevDS,nullptr,nullptr);
    ID3D11Buffer* prevVSConstant=nullptr;context->VSGetConstantBuffers(0,1,&prevVSConstant);
    ID3D11Buffer* prevPSConstant=nullptr;context->PSGetConstantBuffers(0,1,&prevPSConstant);
    D3D11_VIEWPORT vp{0,0,w,h,0,1};UINT stride=sizeof(Vertex),offset=0;
    context->OMSetRenderTargets(1,&rtv,nullptr);context->OMSetBlendState(blendState,nullptr,0xFFFFFFFF);
    context->OMSetDepthStencilState(depthState,0);context->RSSetState(rasterState);context->RSSetViewports(1,&vp);
    context->IASetInputLayout(inputLayout);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers(0,1,&vertexBuffer,&stride,&offset);
    context->VSSetShader(vertexShader,nullptr,0);context->PSSetShader(pixelShader,nullptr,0);
    context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
    ID3D11Buffer* nullCB=nullptr;context->VSSetConstantBuffers(0,1,&nullCB);context->PSSetConstantBuffers(0,1,&nullCB);
    context->PSSetShaderResources(0,1,&uiView);context->PSSetSamplers(0,1,&sampler);
    context->Draw(6,0);
    // Draw the dropdown after the main menu, in a separate quad outside its texture.
    if(activeCategory==6&&bannerDropdownOpen){
        const int rows=bannerPopupRows();
        const int dropWidth=bannerDropW();
        clampBannerScroll(rows);
        if(updateBannerListTexture(dropWidth,rows)){
            const float px=x+(settingsRightX()+6)*uiScale;
            const float py=y+(bannerDropdownTop(HEADER_H+53+SETTINGS_BANNER_OFF)-settingsScroll)*uiScale;
            const float pw=dropWidth*uiScale;
            const float ph=rows*BANNER_ROW_HEIGHT*uiScale;
            const float dl=2*px/w-1,dr=2*(px+pw)/w-1;
            const float dt=1-2*py/h,db=1-2*(py+ph)/h;
            Vertex popupVertices[6]={{dl,dt,0,0},{dr,dt,1,0},{dl,db,0,1},
                                     {dl,db,0,1},{dr,dt,1,0},{dr,db,1,1}};
            D3D11_MAPPED_SUBRESOURCE popupMap{};
            if(SUCCEEDED(context->Map(vertexBuffer,0,D3D11_MAP_WRITE_DISCARD,0,&popupMap))){
                memcpy(popupMap.pData,popupVertices,sizeof(popupVertices));
                context->Unmap(vertexBuffer,0);
                context->PSSetShaderResources(0,1,&bannerListView);
                context->Draw(6,0);
                context->PSSetShaderResources(0,1,&uiView);
            }
        }
    }

    // Place the banner directly above the panel, centered horizontally.
    // Height grows upward, never changing the panel's dimensions or position.
    if(standaloneBannerView){
        const float srcW=(float)standaloneBannerW,srcH=(float)standaloneBannerH;
        // Fit within the header width and a provisional 200 logical-pixel height.
        // Preserve aspect ratio; never upscale narrower artwork.
        const float maxW=(float)TW;
        const float maxH=(float)lroundf(BANNER_MAX_HEIGHT*uiScale);
        float factor=(std::min)(1.0f,(std::min)(maxW/srcW,maxH/srcH));
        // The banner keeps its size when the menu is dragged to the top of the screen (it is simply cut off by the screen edge).
        float bw=srcW*factor,bh=srcH*factor;
        float vTop=0.f,vBottom=1.f;
        const float bx=x+((float)TW-bw)*0.5f;
        const float by=y+headerPx-bh;
        const float bl=2*bx/w-1,br=2*(bx+bw)/w-1;
        const float bt=1-2*by/h,bb=1-2*(by+bh)/h;
        Vertex bannerVertices[6]={{bl,bt,0,vTop},{br,bt,1,vTop},{bl,bb,0,vBottom},
                                  {bl,bb,0,vBottom},{br,bt,1,vTop},{br,bb,1,vBottom}};
        D3D11_MAPPED_SUBRESOURCE bannerMap{};
        if(SUCCEEDED(context->Map(vertexBuffer,0,D3D11_MAP_WRITE_DISCARD,0,&bannerMap))){
            memcpy(bannerMap.pData,bannerVertices,sizeof(bannerVertices));
            context->Unmap(vertexBuffer,0);
            context->PSSetShaderResources(0,1,&standaloneBannerView);
            context->Draw(6,0);
        }
    }
    // Draw a separate cursor quad after the menu, using client coordinates.
    // This remains visible even when the cursor is outside ADREENA's rectangle.
    if(cursorView && gameWindow){
        POINT cp{};
        if(GetCursorPos(&cp)&&ScreenToClient(gameWindow,&cp)&&(clientToBuffer(cp),true) &&
           cp.x>=0 && cp.y>=0 && cp.x<(LONG)w && cp.y<(LONG)h){
            // Whole-number cursor scale keeps the pixel-art arrow crisp with point sampling.
            const float ck=(float)(lroundf(uiScale)<1?1:lroundf(uiScale));
            const float cw=16.0f*ck,ch=22.0f*ck;
            float cl=2.0f*cp.x/w-1.0f,cr=2.0f*(cp.x+cw)/w-1.0f;
            float ct=1.0f-2.0f*cp.y/h,cb=1.0f-2.0f*(cp.y+ch)/h;
            Vertex cv[6]={{cl,ct,0,0},{cr,ct,1,0},{cl,cb,0,1},
                          {cl,cb,0,1},{cr,ct,1,0},{cr,cb,1,1}};
            D3D11_MAPPED_SUBRESOURCE cm{};
            if(SUCCEEDED(context->Map(vertexBuffer,0,D3D11_MAP_WRITE_DISCARD,0,&cm))){
                memcpy(cm.pData,cv,sizeof(cv));context->Unmap(vertexBuffer,0);
                context->PSSetShaderResources(0,1,&cursorView);
                context->Draw(6,0);
            }
        }
    }

    context->PSSetShaderResources(0,1,&prevSRV);context->PSSetSamplers(0,1,&prevSampler);
    context->VSSetShader(prevVS,nullptr,0);context->PSSetShader(prevPS,nullptr,0);
    context->GSSetShader(prevGS,nullptr,0);context->HSSetShader(prevHS,nullptr,0);context->DSSetShader(prevDS,nullptr,0);
    context->VSSetConstantBuffers(0,1,&prevVSConstant);context->PSSetConstantBuffers(0,1,&prevPSConstant);
    context->IASetInputLayout(prevLayout);context->IASetPrimitiveTopology(prevTopology);context->IASetVertexBuffers(0,1,&prevVB,&prevStride,&prevOffset);
    context->RSSetState(prevRaster);if(nvp)context->RSSetViewports(nvp,vps);if(nsc)context->RSSetScissorRects(nsc,scissors);
    context->OMSetBlendState(prevBlend,blendFactor,sampleMask);context->OMSetDepthStencilState(prevDepth,stencilRef);
    if(uavCount){
        UINT rtvN=0;for(UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;i++)if(prevRTV[i])rtvN=i+1;
        UINT initial[D3D11_PS_CS_UAV_REGISTER_COUNT];for(auto& v:initial)v=(UINT)-1;   // -1 = keep the hidden counters as they are
        context->OMSetRenderTargetsAndUnorderedAccessViews(rtvN,prevRTV,prevDSV,uavStart,uavCount,prevUAV+uavStart,initial);
    }else context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,prevRTV,prevDSV);
    for(auto& p:prevRTV)drop(p);
    for(auto& p:prevUAV)drop(p);
    drop(prevDSV);drop(prevBlend);drop(prevDepth);drop(prevRaster);drop(prevLayout);drop(prevVB);
    drop(prevVS);drop(prevPS);drop(prevSRV);drop(prevSampler);drop(prevGS);drop(prevHS);drop(prevDS);drop(prevVSConstant);drop(prevPSConstant);drop(rtv);
}
void clickMenu(int mx,int my){
    commitNum();   // a click anywhere finishes number entry (clicking the box again restarts it)
    menuPosition();int x=toLogical(mx-menuX),y=toLogical(my-menuY); // screen pixels -> logical menu units
    if(activeCategory==6&&bannerDropdownOpen){
        const int left=SIDE+20,available=MW-SIDE-40;
        const int dropX=settingsRightX()+6,dropW=bannerDropW();
        const int listTop=bannerDropdownTop(HEADER_H+53+SETTINGS_BANNER_OFF)-settingsScroll;
        const int rows=bannerPopupRows();
        if(x>=dropX&&x<dropX+dropW&&y>=listTop&&y<listTop+rows*BANNER_ROW_HEIGHT){
            const int item=bannerDropdownScroll+(y-listTop)/BANNER_ROW_HEIGHT;
            bannerWarning.clear();
            if(item==0){selectedPreset=-1;selectedBanner=-1;++bannerRevision;}
            else if(item>0&&item<=(int)bannerFiles.size()){
                const int candidate=item-1;
                if(!bannerGdiplusToken){Gdiplus::GdiplusStartupInput input;Gdiplus::GdiplusStartup(&bannerGdiplusToken,&input,nullptr);}
                Gdiplus::Bitmap probe((bannerFolder+bannerFiles[candidate]).c_str());
                if(probe.GetLastStatus()!=Gdiplus::Ok)bannerWarning=L"Banner could not be loaded.";
                else {selectedPreset=-1;selectedBanner=candidate;++bannerRevision;}
            }
            bannerDropdownOpen=false;uiCacheValid=false;return;
        }
    }
    if(dropActive()){
        RECT pr;int rows=0,total=0;
        if(dropPopupGeom(pr,rows,total)&&x>=pr.left&&x<pr.right&&y>=pr.top&&y<pr.bottom){
            const int item=dropScroll+(y-pr.top)/BANNER_ROW_HEIGHT;
            if(item>=0&&item<total){
                if(dropOpen==3){if(itemCat!=item){itemCat=item;itemPage=0;itemSel=-1;itemCount=1;}}
                else if(dropOpen==4){bookmarkSel=item;bookmarkDelArm=false;}else if(dropOpen==1){appearanceGroupSel=item;appearanceVarSel=0;}else appearanceVarSel=item;
            }
        }
        dropOpen=0;uiCacheValid=false;return;   // a click anywhere closes the list (and is consumed)
    }
    if(x<0||x>=MW||y<0||y>=MH)return;
    if(x<SIDE){for(int i=0;i<categoryCount;i++)if(y>=CAT_TOP+i*CAT_ROW&&y<CAT_TOP+(i+1)*CAT_ROW){activeCategory=i;dropOpen=0;profDropOpen=false;bannerDropdownOpen=false;scrollOffset=0;settingsScroll=0;uiCacheValid=false;tabEnterRefresh(i);return;}return;}
    int left=SIDE+20,available=MW-SIDE-40,gap=28,cw=(available-gap)/2;
    int right=left+cw+gap,top=HEADER_H+53;
    if(activeCategory==6){
        if(y<top-3||y>=MH-34)return;
        y+=settingsScroll;
        // Saved-profile list (open): a click on a row picks it, a click anywhere else just closes the list.
        if(profDropOpen){
            const int total=(int)profileNames.size();const int rows=(std::min)(6,(std::max)(1,total));
            const int pTop=profDropField.bottom+1,pBottom=pTop+rows*BANNER_ROW_HEIGHT;
            if(x>=profDropField.left&&x<profDropField.right&&y>=pTop&&y<pBottom){
                const int item=profDropScroll+(y-pTop)/BANNER_ROW_HEIGHT;
                if(item>=0&&item<total){profileSel=item;profileDelArm=false;wcsncpy(profileNameBuf,profileNames[item].c_str(),24);profileNameBuf[24]=0;}
            }
            profDropOpen=false;uiCacheValid=false;return;
        }
        // Prevent click-through into banner dropdown or other controls under picker.
        if(pickerColor>=0){
            const int px=pickerX(),py=pickerY(),hx=px+PICKER_SIZE+PICKER_GAP;
            if(y>=py-31&&y<py+PICKER_SIZE+11&&x>=px-10&&x<hx+PICKER_HUE_W+12){
                if(y>=py&&y<py+PICKER_SIZE){
                    if(x>=px&&x<px+PICKER_SIZE){pickerDrag=0;pickerUpdate(x,y);return;}
                    if(x>=hx&&x<hx+PICKER_HUE_W){pickerDrag=1;pickerUpdate(x,y);return;}
                }
                return;
            }
        }
        // Profiles block.
        if(!bannerDropdownOpen){
            const int fieldW=bannerDropW(),px0=right+6;
            const int nbL=px0,nbR=px0+fieldW-78;
            if(y>=top+36&&y<top+60){
                if(x>=nbL&&x<nbR){editingNum=205;editingHex=-1;pickerColor=-1;uiCacheValid=false;return;}
                if(x>=nbR+8&&x<px0+fieldW){flashButton(230);profileDoSave();return;}
            }
            if(y>=top+68&&y<top+94&&x>=px0&&x<px0+fieldW){
                profilesRefresh();profDropOpen=true;profDropScroll=0;pickerColor=-1;editingHex=-1;profileDelArm=false;
                settingsScrollTarget=(float)(std::max)(0,(std::min)(settingsMaxScroll(),(top+68)-(HEADER_H+53)-120));
                uiCacheValid=false;return;
            }
            if(y>=top+100&&y<top+124){
                const int halfW=(fieldW-8)/2;
                if(x>=px0&&x<px0+halfW){flashButton(231);profileDoLoad();return;}
                if(x>=px0+halfW+8&&x<px0+fieldW){flashButton(232);profileDoDelete();return;}
            }
            if(y>=top+130&&y<top+154&&x>=px0&&x<px0+fieldW){flashButton(233);profileDoAuto();return;}
        }
        // Two columns of independent preset thumbnails.
        const int gx=left+6,gy=top+SETTINGS_PRESET_TOP_OFFSET;
        const int gw=available-12,gap=12,cw=(gw-gap)/2;
        if(!bannerDropdownOpen&&y>=gy&&y<gy+((PRESET_COUNT+1)/2)*SETTINGS_PRESET_ROW){
            const int row=(y-gy)/SETTINGS_PRESET_ROW;
            const int column=(x>=gx+cw+gap&&x<gx+gw)?1:(x>=gx&&x<gx+cw?0:-1);
            const int index=row*2+column;
            if(column>=0&&index>=0&&index<PRESET_COUNT&&
               (y-gy)%SETTINGS_PRESET_ROW<SETTINGS_PRESET_CELL_H){selectOfficialPreset(index);return;}
        }
        // Banner list is rescanned whenever the dropdown opens.
        const int bannerY=top+SETTINGS_BANNER_OFF,dropX=right+6,dropW=bannerDropW();
        if(x>=dropX&&x<dropX+dropW&&y>=bannerY&&y<bannerY+26){
            if(!bannerDropdownOpen){refreshBannerFiles();bannerDropdownScroll=0;drop(bannerListView);drop(bannerListTexture);}
            bannerDropdownOpen=!bannerDropdownOpen;uiCacheValid=false;return;
        }
        if(bannerDropdownOpen){bannerDropdownOpen=false;uiCacheValid=false;return;}
        // Background opacity slider lives on the same line as its HEX field.
        if(y>=hexY(3)-7&&y<=hexY(3)+HEX_INPUT_H+7&&x>=opacitySliderX()-5&&x<=opacitySliderX()+OPACITY_SLIDER_W+5){
            setOpacityFromLogicalX(x);draggingOpacity=true;return;
        }
        for(int i=0;i<3;i++){
            int y0=hexY(i),bx=hexX();
            if(y>=y0&&y<=y0+HEX_INPUT_H&&x>=bx&&x<bx+HEX_INPUT_W){editingHex=i;pickerColor=-1;wcscpy_s(hexDraft[i],8,L"#");uiCacheValid=false;return;}
            if(y>=y0&&y<=y0+HEX_INPUT_H&&x>=bx+HEX_INPUT_W+10&&x<bx+HEX_INPUT_W+33){openPicker(i);return;}
        }
        if(pickerColor>=0){int px=pickerX(),py=pickerY();
            if(y>=py&&y<py+PICKER_SIZE&&x>=px&&x<px+PICKER_SIZE){pickerDrag=0;pickerUpdate(x,y);return;}
            int hx=px+PICKER_SIZE+PICKER_GAP;
            if(y>=py&&y<py+PICKER_SIZE&&x>=hx&&x<hx+PICKER_HUE_W){pickerDrag=1;pickerUpdate(x,y);return;}
        }
        if(y>=top+170&&y<top+170+27&&x>=left+6&&x<left+6+142){
            setThemeColor(0,RGB(27,27,27));setThemeColor(1,RGB(205,70,107));setThemeColor(2,RGB(245,228,236));bgOpacity=100;++themeRevision;uiCacheValid=false;
            if(pickerColor>=0)rgbToHsv(pickerColor==0?themeBg:(pickerColor==1?themeAccent:themeText),pickerHue,pickerSat,pickerValue);
            editingHex=-1;pickerColor=-1;uiCacheValid=false;return;
        }
        if(editingHex>=0){commitHex(editingHex);syncHex(editingHex);editingHex=-1;}
        pickerColor=-1;uiCacheValid=false;return;
    }

    if(y<top-3||y>=MH-34)return; // content viewport only
    for(int k=0;k<sectionCount(activeCategory);k++){
        int xx=sectionX(activeCategory,k), yy=top+sectionTopOffset(activeCategory,k)-pageScroll[activeCategory];
        const int cw=sectionW(activeCategory,k);
        if(x<xx||x>=xx+cw||y<yy||y>=yy+sectionHeight(activeCategory,k))continue;
        if(y<yy+29)return; // headings always remain expanded
        if(activeCategory==3&&k==1&&y>=yy+128){
            if(x<xx+4||x>=xx+cw-12)return;
            if(y<yy+150){commitNum();editingNum=206;uiCacheValid=false;return;}
            if(y>=yy+156&&y<yy+180){flashButton(240);bookmarkDelArm=false;bookmarkDoSave();return;}
            if(y>=yy+188&&y<yy+308){
                const int i=(y-(yy+188))/24;
                if(i>=0&&i<5&&i<(int)bookmarks.size()&&(y-(yy+188))%24<22){bookmarkSel=i;bookmarkDelArm=false;uiCacheValid=false;}
                return;
            }
            if(y>=yy+312&&y<yy+336){
                const int bw=cw-16,tw=bw*60/100;
                if(x<xx+4+tw){flashButton(241);bookmarkDelArm=false;bookmarkDoTeleport();}
                else if(x>=xx+4+tw+6&&x<xx+4+bw){flashButton(242);bookmarkDoDelete();}
            }
            return;
        }
        if(activeCategory==3&&k==1){
            if(y>=yy+34&&y<yy+58&&x>=xx+4&&x<xx+cw-12&&gameWindow){
                flashButton(181);teleStatus=L"Working...";uiCacheValid=false;
                PostMessageW(gameWindow,WM_ADREENA_TELE,0,0);
            }
            {
                const int bw=cw-16,aw=bw*58/100;
                if(y>=yy+90&&y<yy+114){
                    if(x>=xx+4&&x<xx+4+aw){flashButton(182);teleHotCapturing=!teleHotCapturing;uiCacheValid=false;}
                    else if(x>=xx+4+aw+6&&x<xx+4+bw){flashButton(183);teleHotVk=0;teleHotMods=0;teleHotCapturing=false;teleStatus=L"Hotkey cleared";uiCacheValid=false;}
                }
            }
            return;
        }
        if(activeCategory==0&&k==0&&x>=xx+sectionHalfW()){
            // Recovery buttons (right half of Protection)
            const int rx=xx+sectionHalfW()+SECTION_GAP_X;
            int act=-1;
            if(x>=rx&&x<xx+cw-12){if(y>=yy+32&&y<yy+56)act=2;else if(y>=yy+62&&y<yy+86)act=3;else if(y>=yy+92&&y<yy+116)act=9;}
            if(act>=0&&gameWindow){
                flashButton(act==9?155:150+act);charStatus=L"Working...";uiCacheValid=false;
                PostMessageW(gameWindow,WM_ADREENA_CHAR,(WPARAM)act,0);
            }
            return;
        }
        if(activeCategory==0&&k==2){
            const int boxW=26;
            for(int g=0;g<2;g++){
                const int base=yy+32+g*80;
                if(y>=base+22&&y<base+44){
                    const int dir=(x>=xx+4&&x<xx+4+boxW)?-1:((x>=xx+cw-12-boxW&&x<xx+cw-12)?1:0);
                    if(dir&&gameWindow){
                        flashButton(dir<0?160+g*2:161+g*2);charStatus=L"Working...";uiCacheValid=false;
                        // body: 0 forward / 5 back; hat: 1 forward / 6 back
                        PostMessageW(gameWindow,WM_ADREENA_CHAR,(WPARAM)(g==0?(dir>0?1:6):(dir>0?0:5)),0);
                    }
                    return;
                }
                if(y>=base+48&&y<base+70&&x>=xx+4&&x<xx+cw-12){
                    flashButton(164+g);charStatus=L"Working...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_CHAR,(WPARAM)(g==0?7:8),(LPARAM)(g==0?hatOffset:bodyOffset));
                    return;
                }
            }
            if(gameWindow){   // Appearance (group + variant + Apply) and Voice (selector + Apply)
                const int base=yy+192,vb=yy+322;
                auto dirAt=[&](int rowTop)->int{
                    if(y<rowTop||y>=rowTop+22)return 0;
                    return (x>=xx+4&&x<xx+4+boxW)?-1:((x>=xx+cw-12-boxW&&x<xx+cw-12)?1:0);
                };
                const int dvo=dirAt(vb+22),dvv=dirAt(vb+48);
                {   // Male / Female
                    const int hw=(cw-16-4)/2;
                    if(y>=base+22&&y<base+44)for(int gdr=0;gdr<2;gdr++){
                        const int l=xx+4+gdr*(hw+4);
                        if(x>=l&&x<l+hw){flashButton(174+gdr);appearanceGender=gdr;appearanceGroupSel=0;appearanceVarSel=0;dropOpen=0;uiCacheValid=false;return;}
                    }
                }
                for(int fi=0;fi<2;fi++){   // dropdown fields
                    if(y>=base+48+fi*26&&y<base+70+fi*26&&x>=xx+4&&x<xx+cw-12){
                        flashButton(177+fi);uiCacheValid=false;
                        if(appearanceGroups[appearanceGender].empty()){dropOpen=0;charStatus=L"Loading appearances...";PostMessageW(gameWindow,WM_ADREENA_APPEAR,0,0);return;}
                        if(dropOpen==1+fi){dropOpen=0;return;}
                        dropOpen=1+fi;
                        const int total=dropTotal(),rows=(std::min)(8,total);
                        dropScroll=(std::max)(0,(std::min)(total-rows,dropSelected()-2));
                        return;
                    }
                }
                if(y>=base+100&&y<base+122&&x>=xx+4&&x<xx+cw-12){
                    flashButton(168);uiCacheValid=false;
                    if(appearanceGroups[appearanceGender].empty()){charStatus=L"Loading appearances...";PostMessageW(gameWindow,WM_ADREENA_APPEAR,0,0);}
                    else{
                        const auto& AG=appearanceGroups[appearanceGender];
                        const auto& mem=AG[appearanceGroupSel%(int)AG.size()];
                        charStatus=L"Applying...";PostMessageW(gameWindow,WM_ADREENA_APPEAR,1,(LPARAM)mem[appearanceVarSel%(int)mem.size()]);
                    }
                    return;
                }
                if(dvo||dvv){
                    flashButton(dvo?(dvo<0?171:172):(dvv<0?179:180));
                    if(voiceGroups.empty()){charStatus=L"Loading voices...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_VOICE,0,0);}
                    else if(dvo){const int n=(int)voiceGroups.size();voiceGroupSel=(voiceGroupSel+n+dvo)%n;voiceVarSel=0;uiCacheValid=false;}
                    else{const int n=(int)voiceGroups[voiceGroupSel%(int)voiceGroups.size()].size();voiceVarSel=(voiceVarSel+n+dvv)%n;uiCacheValid=false;}
                    return;
                }
                if(y>=vb+74&&y<vb+96&&x>=xx+4&&x<xx+cw-12){
                    flashButton(173);uiCacheValid=false;
                    if(voiceGroups.empty()){charStatus=L"Loading voices...";PostMessageW(gameWindow,WM_ADREENA_VOICE,0,0);}
                    else{
                        const auto& mem=voiceGroups[voiceGroupSel%(int)voiceGroups.size()];
                        charStatus=L"Applying...";PostMessageW(gameWindow,WM_ADREENA_VOICE,1,(LPARAM)mem[voiceVarSel%(int)mem.size()]);
                    }
                    return;
                }
            }
            return;
        }
        if(activeCategory==0&&k==4){
            const int tabW=(cw-12)/2;
            if(y>=yy+34&&y<yy+56){
                for(int t=0;t<2;t++)if(x>=xx+4+t*tabW&&x<xx+4+(t+1)*tabW-2){flashButton(112+t);skillEditorMode=t;skillArmedName.clear();uiCacheValid=false;return;}
                return;
            }
            if(y>=yy+60&&y<yy+82&&x>=xx+4&&x<xx+cw-12){
                flashButton(111);skillArmedName.clear();skillStatus=L"Reading skills...";uiCacheValid=false;
                PostMessageW(gameWindow,WM_ADREENA_SKREFRESH,0,0);
                return;
            }
            for(int r=0;r<SKILL_ROWS;r++){
                const int top=yy+86+r*25;
                if(y<top||y>=top+22)continue;
                const int ri=skillVisibleIndex(r);if(ri<0)return;
                const bool hasX=skillEditorMode==1;
                const int x1=xx+cw-12,xL=x1-22,mxR=hasX?xL-2:x1,mxL=mxR-38,plL=mxL-30,mnL=plL-30;
                const SkillRow& sr=skillRows[ri];
                if(hasX&&x>=xL&&x<x1){
                    flashButton(400+r);
                    if(skillArmedName!=sr.name){skillArmedName=sr.name;skillStatus=L"Click X again to remove "+sr.name+(sr.base?L" completely":L" (base skill is kept)");uiCacheValid=false;return;}
                    skillArmedName.clear();skillStatus=L"Working...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_SKREM,(WPARAM)ri,0);
                    return;
                }
                skillArmedName.clear();
                int target=-1,bid=0;
                if(x>=mxL&&x<mxR){target=sr.max;bid=122+r*3;}
                else if(x>=plL&&x<mxL-2){target=sr.level+1;bid=121+r*3;}
                else if(x>=mnL&&x<plL-2){target=sr.level-1;bid=120+r*3;}
                if(target<0||target>sr.max)return;
                flashButton(bid);skillStatus=L"Working...";uiCacheValid=false;
                PostMessageW(gameWindow,WM_ADREENA_SKSET,(WPARAM)ri,(LPARAM)target);
                return;
            }
            if(y>=yy+240&&y<yy+264&&x>=xx+4&&x<xx+cw-12){
                flashButton(110);activateMaxSkills();
                skillStatus=skillHookReady?L"Armed - close and reopen the character menu to apply":L"Max Skills hook unavailable";
                uiCacheValid=false;return;
            }
            return;
        }

        if(activeCategory==4&&k==4){
            const int boxW=26;
            for(int r=0;r<2;r++){
                const int rowY=yy+33+r*25;
                if(y>=rowY&&y<rowY+22){
                    const int dir=x>=xx+4&&x<xx+4+boxW?-1:(x>=xx+cw-12-boxW&&x<xx+cw-12?1:0);
                    if(!dir)return;
                    flashButton(dir<0?20+r*2:21+r*2);
                    if(r==0){
                        vehFamilySel=(vehFamilySel+vehicleFamilyCount+dir)%vehicleFamilyCount;vehVariantSel=0;
                    }else{
                        const int n=vehicleFamilies[vehFamilySel].count;
                        vehVariantSel=(vehVariantSel+n+dir)%n;
                    }
                    uiCacheValid=false;return;
                }
            }
            if(y>=yy+83&&y<yy+105){
                if(x>=xx+cw-12-82&&x<xx+cw-12){flashButton(25);vehRegisterNew=!vehRegisterNew;uiCacheValid=false;return;}
                if(x>=xx+4&&x<xx+cw-12-86){
                    const int vi=vehicleFamilies[vehFamilySel].first+vehVariantSel;
                    flashButton(24);
                    if(vehLoadedKnown&&!vehLoadedFlags[vi]){
                        vehSpawnStatus=L"Requesting load...";uiCacheValid=false;
                        PostMessageW(gameWindow,WM_ADREENA_VLOAD,(WPARAM)vi,0);
                    }else{
                        vehSpawnStatus=L"Spawning...";uiCacheValid=false;
                        PostMessageW(gameWindow,WM_ADREENA_VSPAWN,(WPARAM)(vi|((trunkOnSpawn?trunkValue:0)<<16)),(LPARAM)(vehRegisterNew?1:0));
                    }
                    return;
                }
            }
            {   // Bounty Delivery sub-section
                const int byy=yy+130,bx=26;
                if(y>=byy+30&&y<byy+54){
                    const int dir=x>=xx+4&&x<xx+4+bx?-1:(x>=xx+cw-12-bx&&x<xx+cw-12?1:0);
                    if(dir){flashButton(dir<0?30:31);bountySel=(bountySel+bountyCount+dir)%bountyCount;uiCacheValid=false;}
                    return;
                }
                if(y>=byy+60&&y<byy+86&&x>=xx+4&&x<xx+cw-12){
                    flashButton(32);
                    bountyStatus=L"Requesting...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_BOUNTY,(WPARAM)bountySel,0);
                    return;
                }
            }
            return;
        }
        if(activeCategory==4&&k==5){
            RECT tr[8];trunkLayout(xx,yy,cw,tr);
            auto hit=[&](int i){return x>=tr[i].left&&x<tr[i].right&&y>=tr[i].top&&y<tr[i].bottom;};
            const int deltas[5]={-10,-1,0,1,10};
            for(int i=0;i<5;i++){
                if(i==2||!hit(i))continue;
                flashButton(60+(i<2?i:i-1));
                trunkValue=(std::max)(1,(std::min)(1000,trunkValue+deltas[i]));
                uiCacheValid=false;return;
            }
            if(hit(5)){flashButton(64);trunkStatus=L"Reading...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_TRUNK,(WPARAM)trunkValue,(LPARAM)1);return;}
            if(hit(6)){flashButton(65);trunkStatus=L"Applying...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_TRUNK,(WPARAM)trunkValue,(LPARAM)0);return;}
            if(hit(7)){flashButton(66);trunkOnSpawn=!trunkOnSpawn;uiCacheValid=false;return;}
            return;
        }
        if(activeCategory==4&&k==0&&y>=yy+96){
            for(int bi=0;bi<2;bi++){
                const int by=yy+96+30+bi*30;
                if(y>=by&&y<by+26&&x>=xx+4&&x<xx+cw-12){
                    flashButton(40+bi);
                    vehicleStatus=L"Working...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_VEHICLE,(WPARAM)(bi==0?1:2),0); // runs on the game thread via the window procedure
                    return;
                }
            }
            return;
        }
        if(activeCategory==5&&k==0){
            for(int i=0;i<zombieCount;i++){
                const int top=yy+zombieRowTop(i);
                if(y<top||y>=top+22||x<xx+4||x>=xx+4+(zombieBtnW[i]>0?zombieBtnW[i]:cw-16))continue;
                const int zi=zombieOrder[i];
                flashButton(100+i);spawnSelected=zi;
                if(zombieLoadedKnown&&!zombieLoadedFlags[zi]){
                    spawnStatus=L"Requesting load...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_ZLOAD,(WPARAM)zi,0);
                }else{
                    spawnStatus=L"Spawning...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_SPAWN,(WPARAM)zi,0); // runs on the game thread via the window procedure
                }
                return;
            }
            if(y>=yy+393&&y<yy+415&&x>=xx+4&&x<xx+cw-12){flashButton(53);zombieCustom=!zombieCustom;uiCacheValid=false;return;}
            for(int row=0;row<3;row++){
                const int top=yy+420+row*25;
                if(y<top||y>=top+22)continue;
                RECT sr[5];stepperRow(xx,cw,top,sr,row==0?38:32);
                const int dHP[5]={-100,-10,0,10,100},dM[5]={-10,-1,0,1,10};
                for(int i=0;i<5;i++){
                    if(i==2||x<sr[i].left||x>=sr[i].right)continue;
                    flashButton(70+row*4+(i<2?i:i-1));
                    if(row==0)zombieHP=(std::max)(1,(std::min)(100000,zombieHP+dHP[i]));
                    else if(row==1)zombieMeleeT=(std::max)(0,(std::min)(1000,zombieMeleeT+dM[i]));
                    else zombieRangedT=(std::max)(0,(std::min)(1000,zombieRangedT+dM[i]));
                    uiCacheValid=false;return;
                }
                return;
            }
            return;
        }
        if(activeCategory==1&&k==0){
            const int BTN_W=112,BTN_H=24,BTN_GAP=8;
            if(x>=xx+cw-12-BTN_W&&x<xx+cw-12){
                for(int b2=0;b2<4;b2++){
                    const int by=yy+34+b2*(BTN_H+BTN_GAP);if(y<by||y>=by+BTN_H)continue;
                    flashButton(213+b2);
                    const int n=(int)wpnList.size();
                    if(b2==0&&n>0)wpnSel=(wpnSel+n-1)%n;
                    else if(b2==1&&n>0)wpnSel=(wpnSel+1)%n;
                    else if(b2==2)wpnSel=0;
                    else if(b2==3&&gameWindow){wpnStatus=L"Working...";PostMessageW(gameWindow,WM_ADREENA_COMM,9,0);}
                    uiCacheValid=false;return;
                }
            }
            return;
        }
        if(activeCategory==0&&k==5){
            for(int i=0;i<3;i++)if(y>=yy+34+i*28&&y<yy+56+i*28&&x>=xx+4&&x<xx+cw-12){commitNum();editingNum=201+i;uiCacheValid=false;return;}
            if(y>=yy+122&&y<yy+144&&gameWindow){
                const int bw=cw-16,aw=bw*50/100;
                if(x>=xx+4&&x<xx+4+aw-3){flashButton(210);nameStatus=L"Working...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_COMM,6,0);}
                else if(x>=xx+4+aw+3&&x<xx+4+bw){flashButton(211);nameStatus=L"Working...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_COMM,7,0);}
            }
            return;
        }
        if(activeCategory==2&&k==0){
            if(y>=yy+34&&y<yy+58&&x>=xx+4&&x<xx+cw-12&&gameWindow){flashButton(225);resStatus=L"Loading resources...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_COMM,0,0);return;}
            for(int i=0;i<RES_ROWS;i++){
                const int ry=yy+64+i*28;
                if(y>=ry&&y<ry+22){
                    if(x>=xx+cw-118&&x<xx+cw-62)beginNum(i);
                    else if(x>=xx+cw-56&&x<xx+cw-12&&gameWindow){flashButton(190+i);resStatus=L"Working...";uiCacheValid=false;
                        PostMessageW(gameWindow,WM_ADREENA_COMM,1,(LPARAM)((i<<16)|resAmtVals[i]));}
                }
            }
            uiCacheValid=false;return;
        }
        if(activeCategory==2&&k==3){
            const std::vector<int> flt=itemFiltered();
            const int pages=flt.empty()?1:((int)flt.size()+ITEM_ROWS-1)/ITEM_ROWS;
            if(y>=yy+34&&y<yy+58){
                commitNum();
                if(x>=xx+cw-12-52){flashButton(224);dropOpen=0;itemStatus=L"Loading item list...";if(gameWindow)PostMessageW(gameWindow,WM_ADREENA_COMM,10,0);}
                else{flashButton(220);if(dropOpen==3)dropOpen=0;else{dropOpen=3;dropScroll=0;}}
            }
            else if(y>=yy+66&&y<yy+88){commitNum();editingNum=204;}
            else if(y>=yy+96&&y<yy+96+ITEM_ROWS*22){
                const int i=(y-(yy+96))/22;const int fi=itemPage*ITEM_ROWS+i;
                if(fi<(int)flt.size()){if(itemSel!=flt[fi])itemCount=1;itemSel=flt[fi];}
            }
            else if(y>=yy+210&&y<yy+232){
                if(x>=xx+4&&x<xx+4+64){flashButton(221);if(itemPage>0)--itemPage;}
                else if(x>=xx+cw-12-64&&x<xx+cw-12){flashButton(222);if(itemPage<pages-1)++itemPage;}
            }
            else if(y>=yy+240&&y<yy+264){
                if(x>=xx+4&&x<xx+84)beginNum(101);
                else if(x>=xx+cw-12-88&&x<xx+cw-12&&gameWindow){
                    commitNum();flashButton(223);
                    if(itemSel<0||itemSel>=(int)itemAll.size())itemStatus=L"Pick an item first";
                    else{itemStatus=L"Working...";PostMessageW(gameWindow,WM_ADREENA_COMM,11,(LPARAM)((itemCount<<8)|itemAll[itemSel].cat));}
                }
            }
            uiCacheValid=false;return;
        }
        if(activeCategory==2&&k==2){
            const int half=(cw-16)/2;
            if(y>=yy+34&&y<yy+56&&x>=xx+4&&x<xx+84)beginNum(100);
            else if(y>=yy+62&&y<yy+86&&gameWindow){
                if(x>=xx+4&&x<xx+4+half-3){flashButton(206);timerStatus=L"Working...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_COMM,3,(LPARAM)(-timerVal));}
                else if(x>=xx+4+half+3&&x<xx+cw-12){flashButton(207);timerStatus=L"Working...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_COMM,3,(LPARAM)timerVal);}
            }
            uiCacheValid=false;return;
        }
        if(activeCategory==5&&k==5){
            if(x>=xx+4&&x<xx+cw-12&&gameWindow){
                if(y>=yy+34&&y<yy+58){   // Unlimited Spawning: its result shows in the Hordes status line
                    flashButton(90);unlimitedSpawn=!unlimitedSpawn;
                    hordeStatus=L"Applying...";uiCacheValid=false;
                    PostMessageW(gameWindow,WM_ADREENA_UNLIM,(WPARAM)(unlimitedSpawn?1:0),0);
                    return;
                }
                const int hy=yy+62;
                for(int bi=0;bi<3;bi++)if(y>=hy+bi*28&&y<hy+bi*28+24){flashButton(260+bi);hordeStatus=L"Starting...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_HORDE,(WPARAM)bi,0);return;}
                if(y>=hy+3*28+4&&y<hy+3*28+28){flashButton(264);hordeStatus=L"Clearing...";uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_HORDE,3,0);return;}
            }
            return;
        }
        if(activeCategory==5&&k==1&&y>=yy+68&&y<yy+92&&x>=xx+4&&x<xx+cw-12){   // Haven Protocol (Fun)
            if(gameWindow){flashButton(208);havenStatus=L"Working...";havenStatusUntil=~0ULL;uiCacheValid=false;PostMessageW(gameWindow,WM_ADREENA_COMM,4,0);}
            return;
        }
        int n=0;const auto& fl=featureLists[activeCategory];
        for(int ix:menuSections[activeCategory][k]){if(ix<0||ix>=fl.count)continue;
            int rowY=yy+32+(int)roundf((n-sectionScroll[activeCategory][k])*SECTION_ROW_H);
            if(y>=yy+31&&y<yy+sectionHeight(activeCategory,k)-4&&y>=rowY&&y<rowY+SECTION_ROW_H){
                int sid=featureSliderId(activeCategory,k,n);
                if(sid>=0){
                    if(sid==1&&y<rowY+19&&x>=xx+cw-42){visualTimeLock=!visualTimeLock;uiCacheValid=false;return;}
                    if(y>=rowY+18){setFeatureSlider(sid,x,xx+6,cw-12);draggingFeatureSlider=sid;SetCapture(gameWindow);return;}
                    return;
                }
                if(fl.items[ix].id==20)activateMaxSkills();
                else toggle(fl.items[ix].id);
                uiCacheValid=false;return;
            }
            n++;
        }
        return;
    }
}
// Map mouse position to a scrollbar's scroll offset, retaining the thumb grab position.
void updateScrollbarDrag(int mouseY){
    if(draggingScrollbar==-1||scrollbarDragCategory<0)return;
    const int top=HEADER_H+53;
    if(draggingScrollbar==-2){
        int trackH=MH-34-top;
        float maximum=(float)pageMaxScroll();
        if(maximum<=0)return;
        int thumbH=(std::max)(22,(int)roundf(trackH*(float)trackH/(trackH+maximum)));
        int travel=trackH-thumbH;
        float value=travel>0?(float)(mouseY-scrollbarDragGrab-top)*maximum/travel:0.f;
        value=(std::max)(0.f,(std::min)(maximum,value));
        pageScroll[scrollbarDragCategory]=pageScrollTarget[scrollbarDragCategory]=value;
    }else{
        int k=draggingScrollbar;
        int y=top+sectionTopOffset(scrollbarDragCategory,k)-(int)pageScroll[scrollbarDragCategory];
        int trackTop=y+31,trackH=114;
        int count=sectionItemCount(scrollbarDragCategory,k);
        float maximum=sectionMaxScroll(scrollbarDragCategory,k);
        if(count<=SECTION_VIEW_ROWS||maximum<=0)return;
        int thumbH=(std::max)(18,(int)roundf(trackH*3.f/count));
        int travel=trackH-thumbH;
        float value=travel>0?(float)(mouseY-scrollbarDragGrab-trackTop)*maximum/travel:0.f;
        value=(std::max)(0.f,(std::min)(maximum,value));
        sectionScroll[scrollbarDragCategory][k]=sectionScrollTarget[scrollbarDragCategory][k]=value;
    }
    uiCacheValid=false;
}
// ---- Profiles and position bookmarks: definitions (folders, JSON files, apply) ----
std::wstring utf8ToWide(const std::string& s){
    if(s.empty())return std::wstring();
    const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    std::wstring w((size_t)(n>0?n:0),L'\0');
    if(n>0)MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],n);
    return w;
}
std::string wideToUtf8(const std::wstring& w){
    if(w.empty())return std::string();
    const int n=WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s((size_t)(n>0?n:0),'\0');
    if(n>0)WideCharToMultiByte(CP_UTF8,0,w.data(),(int)w.size(),&s[0],n,nullptr,nullptr);
    return s;
}
bool readTextFile(const std::wstring& path,std::string& out){
    FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"rb")!=0||!f)return false;
    std::string s;char buf[4096];size_t n;
    while((n=fread(buf,1,sizeof(buf),f))>0&&s.size()<(1u<<20))s.append(buf,n);
    fclose(f);out=s;return true;
}
bool writeTextFile(const std::wstring& path,const std::string& text){
    FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"wb")!=0||!f)return false;
    const bool ok=text.empty()||fwrite(text.data(),1,text.size(),f)==text.size();
    fclose(f);return ok;
}
std::wstring dllDir,profileDir;
void initProfileFolder(){
    if(!profileDir.empty())return;
    dllDir=adreenaRootDir();profileDir=dllDir+L"Profiles\\";
    CreateDirectoryW(profileDir.c_str(),nullptr);
}
std::wstring trimW(std::wstring s){
    while(!s.empty()&&(s.back()==L' '||s.back()==L'\r'||s.back()==L'\n'))s.pop_back();
    size_t a=0;while(a<s.size()&&s[a]==L' ')++a;
    return s.substr(a);
}
std::wstring bookmarksFile(){bool core=false;adreenaRootDir(&core);initProfileFolder();return core?profileDir+L"Bookmarks.json":dllDir+L"Bookmarks.json";}
void writeAutoFile(){initProfileFolder();writeTextFile(profileDir+L"_autoload.txt",wideToUtf8(profileAuto));}
void bookmarksSave(){
    initProfileFolder();
    std::string j="{\n  \"bookmarks\": [";
    for(size_t i=0;i<bookmarks.size();++i){
        const Bookmark& b=bookmarks[i];
        j+=std::string(i?",":"")+"\n    {\"name\": \""+jsonEscape(wideToUtf8(b.name))+"\", \"x\": "+jsonNum(b.x)+", \"y\": "+jsonNum(b.y)+", \"z\": "+jsonNum(b.z)+"}";
    }
    j+="\n  ]\n}\n";
    writeTextFile(bookmarksFile(),j);
}
void bookmarksLoad(){
    initProfileFolder();
    bookmarks.clear();
    std::string text;JVal root;
    if(!readTextFile(bookmarksFile(),text)||!jsonParse(text,root))return;
    const JVal* arr=root.get("bookmarks");if(!arr||arr->t!=JVal::ARR)return;
    for(const JVal& e:arr->a){
        if(e.t!=JVal::OBJ)continue;
        const JVal* nm=e.get("name");const JVal* x=e.get("x");const JVal* y=e.get("y");const JVal* z=e.get("z");
        if(!nm||nm->t!=JVal::STR||!x||!y||!z||x->t!=JVal::NUM||y->t!=JVal::NUM||z->t!=JVal::NUM)continue;
        Bookmark b;b.name=utf8ToWide(nm->s);b.x=(float)x->n;b.y=(float)y->n;b.z=(float)z->n;
        if(!b.name.empty()&&bookmarks.size()<5)bookmarks.push_back(b);
    }
}
void profilesRefresh(){
    initProfileFolder();
    const std::wstring keep=(profileSel>=0&&profileSel<(int)profileNames.size())?profileNames[profileSel]:std::wstring();
    profileNames.clear();
    WIN32_FIND_DATAW fd{};HANDLE h=FindFirstFileW((profileDir+L"*.json").c_str(),&fd);
    if(h!=INVALID_HANDLE_VALUE){
        do{
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            std::wstring n=fd.cFileName;
            if(n.size()<=5||_wcsicmp(n.c_str()+n.size()-5,L".json")!=0)continue;
            n.resize(n.size()-5);
            if(!n.empty()&&n[0]!=L'_'&&profileNames.size()<200)profileNames.push_back(n);
        }while(FindNextFileW(h,&fd));
        FindClose(h);
    }
    std::sort(profileNames.begin(),profileNames.end(),[](const std::wstring& a,const std::wstring& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    profileSel=-1;for(int i=0;i<(int)profileNames.size();++i)if(_wcsicmp(profileNames[i].c_str(),keep.c_str())==0){profileSel=i;break;}
}
void profilesInit(){
    initProfileFolder();
    profilesRefresh();
    std::string t;profileAuto.clear();
    if(readTextFile(profileDir+L"_autoload.txt",t)){
        const std::wstring nm=trimW(utf8ToWide(t));
        for(const std::wstring& n:profileNames)if(_wcsicmp(n.c_str(),nm.c_str())==0){profileAuto=n;break;}
    }
    bookmarksLoad();
    if(bookmarkSel>=(int)bookmarks.size())bookmarkSel=-1;
}
std::string featureKey(int id){
    for(const FeatureList& fl:featureLists)
        for(int i=0;i<fl.count;++i)
            if(fl.items[i].id==id)return wideToUtf8(fl.items[i].label)+" ["+std::to_string(id)+"]";
    return "feature ["+std::to_string(id)+"]";
}
bool profileSaveFile(const std::wstring& name,std::wstring& msg){
    initProfileFolder();menuPosition();
    std::vector<std::string> e;
    auto str=[&](const std::string& v){return "\""+jsonEscape(v)+"\"";};
    auto bol=[&](bool b){return std::string(b?"true":"false");};
    auto hex=[&](const int* c){char b[16];snprintf(b,sizeof(b),"#%02X%02X%02X",c[0],c[1],c[2]);return std::string(b);};
    auto kv=[&](const char* k,const std::string& v){e.push_back("  \""+std::string(k)+"\": "+v);};
    kv("adreena_profile","1");
    menuPosition();kv("menu_x",jsonNum(menuX));kv("menu_y",jsonNum(menuY));kv("menu_fx",jsonNum(menuFx));kv("menu_fy",jsonNum(menuFy));kv("sidebar_width",jsonNum(SIDE));
    kv("color_background",str(hex(themeBg)));kv("color_accent",str(hex(themeAccent)));kv("color_text",str(hex(themeText)));
    kv("background_opacity",jsonNum(bgOpacity));
    if(selectedBanner==-1)kv("banner_mode",str("none"));
    else if(selectedBanner>=0&&selectedBanner<(int)bannerFiles.size()){kv("banner_mode",str("file"));kv("banner_file",str(wideToUtf8(bannerFiles[selectedBanner])));}
    else kv("banner_mode",str("default"));
    kv("preset",jsonNum(selectedPreset));
    kv("teleport_hotkey_vk",jsonNum(teleHotVk.load()));kv("teleport_hotkey_mods",jsonNum(teleHotMods.load()));
    kv("character_time_dilation",jsonNum(characterTimeDilation.load()));
    kv("visual_time",jsonNum(visualTimeValue.load()));kv("visual_time_lock",bol(visualTimeLock.load()));
    kv("world_time_scale",jsonNum(worldTimeScale));kv("gravity_scale",jsonNum(gravityScaleValue));
    kv("zombie_custom",bol(zombieCustom));kv("zombie_health",jsonNum(zombieHP));kv("zombie_melee",jsonNum(zombieMeleeT));kv("zombie_ranged",jsonNum(zombieRangedT));
    kv("trunk_on_spawn",bol(trunkOnSpawn));kv("trunk_value",jsonNum(trunkValue));
    kv("vehicle_register_new",bol(vehRegisterNew));
    std::string feats="  \"features\": {";
    bool first=true;
    for(int id=0;id<=46;++id){
        auto* f=featureFlag(id);if(!f)continue;
        feats+=std::string(first?"":",")+"\n    \""+jsonEscape(featureKey(id))+"\": "+bol(f->load());first=false;
    }
    feats+="\n  }";
    e.push_back(feats);
    std::string j="{\n";
    for(size_t i=0;i<e.size();++i)j+=e[i]+(i+1<e.size()?",\n":"\n");
    j+="}\n";
    if(!writeTextFile(profileDir+name+L".json",j)){msg=L"Could not write the profile file";return false;}
    msg=L"Saved \""+name+L"\"";return true;
}
bool profileLoadByName(const std::wstring& name,int phases,std::wstring& msg){
    initProfileFolder();
    std::string text;
    if(!readTextFile(profileDir+name+L".json",text)){msg=L"Could not read \""+name+L"\"";return false;}
    JVal root;
    if(!jsonParse(text,root)){msg=L"\""+name+L"\" is not a valid profile file";return false;}
    auto getNum=[&](const char* k,double& v)->bool{const JVal* p=root.get(k);if(p&&p->t==JVal::NUM){v=p->n;return true;}return false;};
    auto getBool=[&](const char* k,bool& v)->bool{const JVal* p=root.get(k);if(p&&p->t==JVal::BOOL){v=p->b;return true;}return false;};
    auto getStr=[&](const char* k,std::string& v)->bool{const JVal* p=root.get(k);if(p&&p->t==JVal::STR){v=p->s;return true;}return false;};
    auto clampD=[](double v,double lo,double hi){return v<lo?lo:(v>hi?hi:v);};
    int skipped=0;double d=0;bool b=false;std::string sv;
    if(phases&1){
        if(getNum("sidebar_width",d))SIDE=(int)clampD(d,SIDE_MIN,SIDE_MAX);
        if(getNum("menu_x",d))menuX=(int)clampD(d,0,20000);
        if(getNum("menu_y",d))menuY=(int)clampD(d,0,20000);
        {   // proportional position (newer profiles): adapts to any resolution / menu scale; old profiles keep pixels
            double fx=-1,fy=-1;
            if(getNum("menu_fx",fx)&&getNum("menu_fy",fy)&&fx>=0&&fy>=0){
                menuFx=(float)clampD(fx,0,1);menuFy=(float)clampD(fy,0,1);menuApplyFrac=true;
                if(menuX<0)menuX=0;if(menuY<0)menuY=0;
            }
        }
        auto setHex=[&](const char* k,int idx){
            std::string h;if(!getStr(k,h)||h.size()!=7||h[0]!='#')return;
            int v=0;for(int i=1;i<7;i++){const int n=hexDigit((wchar_t)(unsigned char)h[i]);if(n<0)return;v=(v<<4)|n;}
            setThemeColor(idx,RGB((v>>16)&255,(v>>8)&255,v&255));
        };
        setHex("color_background",0);setHex("color_accent",1);setHex("color_text",2);
        if(getNum("background_opacity",d)){bgOpacity=(int)clampD(d,0,100);++themeRevision;}
        if(getStr("banner_mode",sv)){
            if(sv=="none")selectedBanner=-1;
            else if(sv=="default")selectedBanner=0;
            else if(sv=="file"){
                selectedBanner=0;
                std::string bf;
                if(getStr("banner_file",bf)){
                    refreshBannerFiles();
                    const std::wstring want=utf8ToWide(bf);
                    for(int i=0;i<(int)bannerFiles.size();++i)if(_wcsicmp(bannerFiles[i].c_str(),want.c_str())==0){selectedBanner=i;break;}
                }
            }
        }
        if(getNum("preset",d))selectedPreset=(int)clampD(d,-1,PRESET_COUNT-1);
        if(getNum("teleport_hotkey_vk",d)){teleHotVk=(int)clampD(d,0,255);if(getNum("teleport_hotkey_mods",d))teleHotMods=(int)clampD(d,0,7);teleHotCapturing=false;}
        bannerWarning.clear();++bannerRevision;++themeRevision;
        pickerColor=-1;editingHex=-1;bannerDropdownOpen=false;profDropOpen=false;
        menuPosition();uiCacheValid=false;
    }
    if(phases&2){
        if(getNum("character_time_dilation",d))characterTimeDilation=(float)clampD(d,0.1,3.0);
        if(getNum("visual_time",d))visualTimeValue=(float)clampD(d,0,2400);
        if(getBool("visual_time_lock",b))visualTimeLock=b;
        if(getNum("world_time_scale",d)){const float v=(float)clampD(d,0.1,4.0);if(v!=worldTimeScale){worldTimeScale=v;postWorldFloat(0,v);}}
        if(getNum("gravity_scale",d)){const float v=(float)clampD(d,0.1,3.0);if(v!=gravityScaleValue){gravityScaleValue=v;postWorldFloat(1,v);}}
        if(getBool("zombie_custom",b))zombieCustom=b;
        if(getNum("zombie_health",d))zombieHP=(int)clampD(d,0,100000);
        if(getNum("zombie_melee",d))zombieMeleeT=(int)clampD(d,0,1000);
        if(getNum("zombie_ranged",d))zombieRangedT=(int)clampD(d,0,1000);
        if(getBool("trunk_on_spawn",b))trunkOnSpawn=b;
        if(getNum("trunk_value",d))trunkValue=(int)clampD(d,0,1000000);
        if(getBool("vehicle_register_new",b))vehRegisterNew=b;
        const JVal* feats=root.get("features");
        if(feats&&feats->t==JVal::OBJ){
            for(const auto& kvp:feats->o){
                if(kvp.second.t!=JVal::BOOL)continue;
                const size_t lb=kvp.first.rfind('['),rb=kvp.first.rfind(']');
                if(lb==std::string::npos||rb==std::string::npos||rb<=lb+1)continue;
                const int id=atoi(kvp.first.substr(lb+1,rb-lb-1).c_str());
                auto* f=featureFlag(id);if(!f)continue;
                if(f->load()==kvp.second.b)continue;
                if(featureReady(id))toggle(id);else ++skipped;
            }
        }
        uiCacheValid=false;
    }
    msg=L"Loaded \""+name+L"\"";
    if(skipped>0)msg+=L" ("+std::to_wstring(skipped)+L" toggle(s) not available yet)";
    return true;
}
void profileDoSave(){
    std::wstring name=trimW(profileNameBuf);
    if(name.empty()){profileStatus=L"Type a name first";uiCacheValid=false;return;}
    std::wstring msg;
    if(profileSaveFile(name,msg)){
        profilesRefresh();profileSel=-1;
        for(int i=0;i<(int)profileNames.size();++i)if(_wcsicmp(profileNames[i].c_str(),name.c_str())==0){profileSel=i;break;}
    }
    profileStatus=msg;profileDelArm=false;uiCacheValid=false;
}
void profileDoLoad(){
    profileDelArm=false;
    if(profileSel<0||profileSel>=(int)profileNames.size()){profileStatus=L"Pick a profile first";uiCacheValid=false;return;}
    std::wstring msg;profileLoadByName(profileNames[profileSel],3,msg);
    profileStatus=msg;uiCacheValid=false;
}
void profileDoDelete(){
    if(profileSel<0||profileSel>=(int)profileNames.size()){profileStatus=L"Pick a profile first";profileDelArm=false;uiCacheValid=false;return;}
    const std::wstring nm=profileNames[profileSel];
    if(!profileDelArm){profileDelArm=true;profileStatus=L"Press Delete again to remove \""+nm+L"\"";uiCacheValid=false;return;}
    profileDelArm=false;
    if(DeleteFileW((profileDir+nm+L".json").c_str())){
        if(_wcsicmp(profileAuto.c_str(),nm.c_str())==0){profileAuto.clear();writeAutoFile();}
        profileSel=-1;profilesRefresh();profileStatus=L"Deleted \""+nm+L"\"";
    }else profileStatus=L"Could not delete the file";
    uiCacheValid=false;
}
void profileDoAuto(){
    profileDelArm=false;
    const bool haveSel=profileSel>=0&&profileSel<(int)profileNames.size();
    if(!profileAuto.empty()&&(!haveSel||_wcsicmp(profileAuto.c_str(),profileNames[profileSel].c_str())==0)){
        profileAuto.clear();profileStatus=L"Startup profile turned off";
    }else if(haveSel){
        profileAuto=profileNames[profileSel];profileStatus=L"\""+profileAuto+L"\" will load on startup";
    }else{profileStatus=L"Pick a profile first";uiCacheValid=false;return;}
    writeAutoFile();uiCacheValid=false;
}
void bookmarkAdd(float x,float y,float z){
    const std::wstring name=trimW(bookmarkPendingName);bookmarkPendingName.clear();
    if(name.empty())return;
    int idx=-1;for(int i=0;i<(int)bookmarks.size();++i)if(_wcsicmp(bookmarks[i].name.c_str(),name.c_str())==0){idx=i;break;}
    if(idx<0){
        if(bookmarks.size()>=5){bookmarkStatus=L"All 5 slots used - delete one first";return;}
        Bookmark b;b.name=name;b.x=x;b.y=y;b.z=z;bookmarks.push_back(b);idx=(int)bookmarks.size()-1;
        bookmarkStatus=L"Saved \""+name+L"\"";
    }else{bookmarks[idx].x=x;bookmarks[idx].y=y;bookmarks[idx].z=z;bookmarkStatus=L"Updated \""+bookmarks[idx].name+L"\"";}
    bookmarkSel=idx;bookmarksSave();uiCacheValid=false;
}
void bookmarkDoSave(){
    const std::wstring name=trimW(bookmarkNameBuf);
    if(name.empty()){bookmarkStatus=L"Type a name first";uiCacheValid=false;return;}
    if(!gameWindow)return;
    bookmarkPendingName=name;bookmarkStatus=L"Reading position...";uiCacheValid=false;
    PostMessageW(gameWindow,WM_ADREENA_COMM,12,0);
}
void bookmarkDoTeleport(){
    if(bookmarkSel<0||bookmarkSel>=(int)bookmarks.size()){bookmarkStatus=L"Select a bookmark first";uiCacheValid=false;return;}
    if(!gameWindow)return;
    bookmarkStatus=L"Working...";uiCacheValid=false;
    PostMessageW(gameWindow,WM_ADREENA_TELE,2,0);
}
void bookmarkDoDelete(){
    if(bookmarkSel<0||bookmarkSel>=(int)bookmarks.size()){bookmarkStatus=L"Select a bookmark first";bookmarkDelArm=false;uiCacheValid=false;return;}
    const std::wstring nm=bookmarks[bookmarkSel].name;
    if(!bookmarkDelArm){bookmarkDelArm=true;bookmarkStatus=L"Press Delete again to remove \""+nm+L"\"";uiCacheValid=false;return;}
    bookmarkDelArm=false;
    bookmarks.erase(bookmarks.begin()+bookmarkSel);bookmarkSel=-1;bookmarksSave();
    bookmarkStatus=L"Deleted \""+nm+L"\"";uiCacheValid=false;
}
void setMenuOpen(bool open){
    searchOpen=false;if(editingNum==207)editingNum=-1;
    showMenu=open;draggingSettingsScroll=false;draggingScrollbar=-1;scrollbarDragCategory=-1;dragging=false;resizingSidebar=false;draggingOpacity=false;draggingFeatureSlider=-1;pickerDrag=-1;editingHex=-1;pickerColor=-1;syncHex(0);syncHex(1);syncHex(2);
    if(open){menuPosition();ClipCursor(nullptr);SetCursor(LoadCursorW(nullptr,IDC_ARROW));}
}
LRESULT CALLBACK menuWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    lastWndProcTick.store(GetTickCount64(),std::memory_order_relaxed);
    const LPARAM rawLp=lp;   // the game's own handler always receives the original coordinates
    if(msg==WM_MOUSEMOVE||msg==WM_LBUTTONDOWN||msg==WM_LBUTTONUP||msg==WM_LBUTTONDBLCLK){
        POINT mp{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};clientToBuffer(mp);lp=MAKELPARAM((short)mp.x,(short)mp.y);
    }
    std::unique_lock<std::recursive_mutex> menuGuard(menuLock); // released before the game's own handler runs
    if(msg==WM_ADREENA_KEEP){
        menuGuard.unlock();
        spawner::keepTick();
        spawner::g_keepPending=false;
        menuGuard.lock();
        return 0;
    }
    if(msg==WM_ADREENA_AIM){
        menuGuard.unlock();
        if(aimbotFlag.load())spawner::aimTick();
        spawner::g_aimPending=false;
        menuGuard.lock();
        return 0;
    }
    if(msg==WM_ADREENA_VCHECK){
        menuGuard.unlock();
        spawner::describeActor("Vehicle spawn check (2.5 s later)",(uintptr_t)wp,(int)lp,spawner::g_vehWant);
        menuGuard.lock();
        return 0;
    }
    if(msg==WM_ADREENA_VSPAWN){
        const int which=(int)(wp&0xFFFF);const int trunkSize=(int)((wp>>16)&0xFFFF);const bool reg=lp!=0;
        menuGuard.unlock();
        const std::wstring result=spawner::spawnVehicle(which,reg,trunkSize);
        spawner::logStatus("Vehicle spawn result",result);
        menuGuard.lock();
        vehSpawnStatus=result;uiCacheValid=false;
        if(result.compare(0,7,L"Loading")==0&&which>=0&&which<vehicleCount){
            vehLoadedKnown=true;vehLoadedFlags[which]=0;startLoadPoll(which); // the button turns into Load until it appears
        }
        return 0;
    }
    if(msg==WM_ADREENA_TRUNK){
        const int size=(int)wp;const bool readOnly=lp!=0;
        menuGuard.unlock();
        int current=-1;
        const std::wstring result=spawner::trunkAction(size,readOnly,current);
        spawner::logStatus("Trunk result",result);
        menuGuard.lock();
        trunkStatus=result;
        if(current>0)trunkValue=current;
        uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_VMASK){
        menuGuard.unlock();
        unsigned char flags[vehicleCount]{};bool okScan=false;
        {std::string why;if(spawner::init(why)){spawner::computeLoadedVehicles(flags);okScan=true;}}
        menuGuard.lock();
        if(okScan){
            memcpy(vehLoadedFlags,flags,sizeof(flags));vehLoadedKnown=true;
            if(vehLoadPending>=0&&vehLoadPending<vehicleCount&&vehLoadedFlags[vehLoadPending]){
                vehSpawnStatus=L"Vehicle loaded - ready to spawn.";vehLoadPending=-1;
            }
            uiCacheValid=false;
        }
        return 0;
    }
    if(msg==WM_ADREENA_VLOAD){
        const int which=(int)wp;
        menuGuard.unlock();
        const std::wstring result=spawner::loadVehicleClass(which);
        spawner::logStatus("Vehicle load result",result);
        menuGuard.lock();
        if(result==L"ALREADY"){
            vehSpawnStatus=L"Vehicle is already loaded - ready to spawn.";
            if(which>=0&&which<vehicleCount)vehLoadedFlags[which]=1;
        }else{
            vehSpawnStatus=result;
            if(result.compare(0,7,L"Loading")==0)startLoadPoll(which);
        }
        uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_BOUNTY){
        const int which=(int)wp;
        menuGuard.unlock();
        const std::wstring result=spawner::bountyDeliver(which);
        spawner::logStatus("Bounty delivery result",result);
        menuGuard.lock();
        bountyStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_VEHICLE){
        const bool refuel=(int)wp==1;
        menuGuard.unlock();
        const std::wstring result=spawner::vehicleAction(refuel);
        spawner::logStatus(refuel?"Refuel result":"Repair result",result);
        menuGuard.lock();
        vehicleStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_SPAWN){
        // The window procedure runs on the game's main thread, the only safe place to call into the engine.
        const int which=(int)wp;
        const bool custom=zombieCustom;const int hp=zombieHP;const float melee=zombieMeleeT/10.f,ranged=zombieRangedT/10.f; // read while locked
        menuGuard.unlock();
        const std::wstring result=spawner::spawn(which,custom,hp,melee,ranged);
        spawner::logStatus("Spawn result",result);
        menuGuard.lock();
        spawnStatus=result;uiCacheValid=false;
        if(result.compare(0,7,L"Loading")==0&&which>=0&&which<zombieCount){
            zombieLoadedKnown=true;zombieLoadedFlags[which]=0;startZombiePoll(which);
        }
        return 0;
    }
    if(msg==WM_ADREENA_HORDE){
        const int w=(int)wp;
        menuGuard.unlock();
        std::wstring r;
        if(w==10){r=spawner::hordeTick();spawner::g_hordePending=false;}
        else if(w==3)r=spawner::hordeClear();
                else r=spawner::hordeStart(w);
        const bool finished=(w==10&&!spawner::g_hordeActive.load()&&!r.empty());
        menuGuard.lock();
        if(!r.empty()&&r!=hordeStatus){hordeStatus=r;uiCacheValid=false;}
        if(finished&&gameWindow)PostMessageW(gameWindow,WM_ADREENA_ZMASK,0,0);
        return 0;
    }
    if(msg==WM_ADREENA_SKREFRESH){
        menuGuard.unlock();
        std::vector<SkillRow> rows;
        const std::wstring result=spawner::readSkills(rows);
        spawner::logStatus("Skill refresh",result);
        menuGuard.lock();
        if(!rows.empty()){skillRows=rows;}
        skillStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_SKSET){
        const int ri=(int)wp;const int target=(int)lp;
        std::wstring name;if(ri>=0&&ri<(int)skillRows.size())name=skillRows[ri].name;
        menuGuard.unlock();
        std::wstring result=name.empty()?std::wstring(L"Press Refresh first"):spawner::setSkillLevel(name,target);
        std::vector<SkillRow> rows;if(!name.empty())spawner::readSkills(rows);
        spawner::logStatus("Skill edit result",result);
        menuGuard.lock();
        if(!rows.empty())skillRows=rows;
        skillStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_SKREM){
        const int ri=(int)wp;
        std::wstring name;bool isBase=true;
        if(ri>=0&&ri<(int)skillRows.size()){name=skillRows[ri].name;isBase=skillRows[ri].base;}
        menuGuard.unlock();
        std::wstring result=name.empty()?std::wstring(L"Press Refresh first"):spawner::removeSkill(name,isBase);
        std::vector<SkillRow> rows;std::wstring rr;if(!name.empty())rr=spawner::readSkills(rows);
        spawner::logStatus("Skill remove result",result);
        menuGuard.lock();
        if(!name.empty()&&!rows.empty())skillRows=rows;
        skillStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_SKDUMP){
        menuGuard.unlock();
        const std::wstring result=spawner::dumpSkills();
        spawner::logStatus("Skill dump result",result);
        menuGuard.lock();
        skillStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_COMM){
        int act=(int)wp;const int par=(int)lp;
        if(act==14){   // automatic Give Item scan at start-up (game menu / loading), once the game has settled
            if(giveAutoStop.load()||!itemAll.empty()){giveAutoStop=true;return 0;}
            menuGuard.unlock();const bool ready=spawner::itemScanReady();menuGuard.lock();
            if(!ready)return 0;
            giveAutoStop=true;act=10;itemStatus=L"Loading item list...";logMessage("Give Item: automatic start-up scan");
        }
        const std::wstring nf=nameFirst,nl=nameLast,nk=nameNick;
        const std::wstring ig=(itemSel>=0&&itemSel<(int)itemAll.size())?itemAll[itemSel].name:std::wstring();   // copied while the lock is still held
        menuGuard.unlock();
        spawner::CommResult r=spawner::commAction(act,par,act==11?ig:nf,nl,nk);
        menuGuard.lock();
        if(act==0||act==1){
            resStatus=r.status;
            if(r.haveList){resLabels=r.labels;resAmts=r.amts;resLoaded=true;
                if(!resAmtInit){resAmtInit=true;for(int i=0;i<RES_ROWS;i++){const float c=resAmountFor(resUiNames[i]);if(c>=0.f)resAmtVals[i]=(std::min)(9999,(int)(c+0.5f));}}}
        }else if(act==6||act==7){
            nameStatus=r.status;
            if(r.haveNames){wcsncpy(nameFirst,r.first.c_str(),20);nameFirst[20]=0;wcsncpy(nameLast,r.last.c_str(),20);nameLast[20]=0;if(r.haveNick){wcsncpy(nameNick,r.nick.c_str(),20);nameNick[20]=0;}}
        }else if(act==10||act==11){
            itemStatus=r.status;
            if(act==10&&r.haveList){
                giveAutoStop=true;
                std::wstring keep=(itemSel>=0&&itemSel<(int)itemAll.size())?itemAll[itemSel].name:L"";
                itemAll.clear();for(size_t i=0;i<r.labels.size()&&i<r.amts.size();++i)itemAll.push_back({r.labels[i],((int)r.amts[i])%100,((int)r.amts[i])>=100});
                itemSel=-1;if(!keep.empty())for(int i=0;i<(int)itemAll.size();++i)if(itemAll[i].name==keep){itemSel=i;break;}
                itemPage=0;
            }
        }else if(act==13){
            bookmarkStatus=r.status;uiCacheValid=false;
        }else if(act==12){
            bookmarkStatus=r.status;
            if(r.haveList&&r.amts.size()>=3&&!bookmarkPendingName.empty())bookmarkAdd(r.amts[0],r.amts[1],r.amts[2]);
        }else if(act==9){
            wpnStatus=r.status;
            std::wstring keep=(wpnSel>=0&&wpnSel<(int)wpnList.size())?wpnList[wpnSel]:L"";
            wpnList=r.labels;wpnSel=0;
            if(!keep.empty())for(int i=0;i<(int)wpnList.size();++i)if(wpnList[i]==keep){wpnSel=i;break;}
        }else if(act==4||act==5){havenStatus=r.status;havenStatusUntil=GetTickCount64()+5000;}
        else timerStatus=r.status;
        uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_PROFILE){   // startup profile: layout first, gameplay settings once a character exists
        if((wp==1||wp==2)&&!profileAuto.empty()){
            std::wstring msg2;const bool okp=profileLoadByName(profileAuto,wp==1?1:2,msg2);
            if(wp==2||!okp)profileStatus=msg2;
            if(!okp)logMessage("Startup profile could not be applied");
        }
        return 0;
    }
    if(msg==WM_ADREENA_TELE){
        float bmX=0,bmY=0,bmZ=0;bool bmOk=false;
        if(wp==2&&bookmarkSel>=0&&bookmarkSel<(int)bookmarks.size()){bmX=bookmarks[bookmarkSel].x;bmY=bookmarks[bookmarkSel].y;bmZ=bookmarks[bookmarkSel].z;bmOk=true;}
        menuGuard.unlock();
        std::wstring result;
        if(wp==1){bool more=false;result=spawner::teleportTick(more);
            if(!more)spawner::logStatus("Teleport result",result);}
        else{
            spawner::g_tpActive=false;   // a new press replaces any pending one
            spawner::g_tpBookmark=(wp==2);
            if(wp==2&&!bmOk)result=L"Select a bookmark first";
            else result=(wp==2)?spawner::teleportToMarker(true,bmX,bmY,bmZ):spawner::teleportToMarker();
            spawner::logStatus("Teleport result",result);
            if(spawner::g_tpActive.load()&&!spawner::g_tpThread.exchange(true)){
                HWND gw=gameWindow;
                std::thread([gw](){
                    while(spawner::g_tpActive.load()){Sleep(16);if(spawner::g_tpActive.load()&&gw&&!spawner::g_tpPending.exchange(true))PostMessageW(gw,WM_ADREENA_TELE,1,0);}
                    spawner::g_tpThread=false;
                }).detach();
            }
        }
        menuGuard.lock();
        if(!result.empty()){if(wp==2||(wp==1&&spawner::g_tpBookmark.load()))bookmarkStatus=result;else teleStatus=result;uiCacheValid=false;}
        return 0;
    }
    if(msg==WM_ADREENA_VOICE){
        const int mode=(int)wp,idx=(int)lp;
        unsigned long long raw=0;bool haveRaw=false;
        if(mode==1&&idx>=0&&idx<(int)voiceRaw.size()){raw=voiceRaw[idx];haveRaw=true;}
        menuGuard.unlock();
        std::wstring result;std::vector<std::string> names;std::vector<unsigned long long> raws;
        if(mode==0)result=spawner::loadVoiceNames(names,raws);
        else result=haveRaw?spawner::applyVoice(raw):std::wstring(L"Load the list first");
        spawner::logStatus("Voice result",result);
        menuGuard.lock();
        if(mode==0&&!names.empty()){
            voiceNames.clear();for(const auto& n:names)voiceNames.push_back(std::wstring(n.begin(),n.end()));
            voiceRaw=raws;
            // Group by character: "Ada_Normal" -> group "Ada", variant "Normal" (names without '_' form their own group).
            voiceGroups.clear();voiceGroupNames.clear();
            std::unordered_map<std::string,int> vix;
            for(size_t i=0;i<names.size();++i){
                const size_t us=names[i].find('_');
                const std::string key=(us==std::string::npos||us==0)?names[i]:names[i].substr(0,us);
                auto it=vix.find(key);
                if(it==vix.end()){it=vix.emplace(key,(int)voiceGroups.size()).first;voiceGroups.emplace_back();voiceGroupNames.push_back(std::wstring(key.begin(),key.end()));}
                voiceGroups[it->second].push_back((int)i);
            }
            voiceGroupSel=0;voiceVarSel=0;
        }
        charStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_APPEAR){
        const int mode=(int)wp,idx=(int)lp;
        unsigned long long raw=0;bool haveRaw=false;
        if(mode==1&&idx>=0&&idx<(int)appearanceRaw.size()){raw=appearanceRaw[idx];haveRaw=true;}   // idx = global entry index
        menuGuard.unlock();
        std::wstring result;std::vector<std::string> names;std::vector<unsigned long long> raws;
        if(mode==0)result=spawner::loadAppearanceNames(names,raws);
        else result=haveRaw?spawner::applyAppearance(raw):std::wstring(L"Load the list first");
        spawner::logStatus("Appearance result",result);
        menuGuard.lock();
        if(mode==0&&!names.empty()){
            appearanceNames.clear();for(const auto& n:names)appearanceNames.push_back(std::wstring(n.begin(),n.end()));
            appearanceRaw=raws;
            // Group per gender by name prefix: "HumanFemaleArmy_v_13" -> female / "Army"; names with no gender prefix appear under both.
            for(int g=0;g<2;++g){appearanceGroups[g].clear();appearanceGroupNames[g].clear();}
            std::unordered_map<std::string,int> gix[2];
            for(size_t i=0;i<names.size();++i){
                const std::string& n0=names[i];
                int gender=-1;std::string n=n0;
                if(n0.compare(0,11,"HumanFemale")==0){gender=1;n=n0.substr(11);}
                else if(n0.compare(0,9,"HumanMale")==0){gender=0;n=n0.substr(9);}
                std::string key=n;
                const size_t pv=n.rfind("_v_");
                if(pv!=std::string::npos&&pv>0&&pv+3<n.size()&&n.find_first_not_of("0123456789",pv+3)==std::string::npos)key=n.substr(0,pv);
                else{size_t e=n.size();while(e>0&&isdigit((unsigned char)n[e-1]))--e;while(e>0&&n[e-1]=='_')--e;if(e>0&&e<n.size())key=n.substr(0,e);}
                if(key.empty())key=n0;
                for(int g=0;g<2;++g){
                    if(gender>=0&&gender!=g)continue;
                    auto it=gix[g].find(key);
                    if(it==gix[g].end()){it=gix[g].emplace(key,(int)appearanceGroups[g].size()).first;appearanceGroups[g].emplace_back();appearanceGroupNames[g].push_back(std::wstring(key.begin(),key.end()));}
                    appearanceGroups[g][it->second].push_back((int)i);
                }
            }
            dropOpen=0;
            appearanceGroupSel=0;appearanceVarSel=0;
        }
        charStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_CHAR){
        const int action=(int)wp;
        menuGuard.unlock();
        bool okRun=false;
        const std::wstring result=spawner::characterAction(action,okRun,(int)lp);
        spawner::logStatus("Character action result",result);
        menuGuard.lock();
        if(action==4)skillStatus=result;else charStatus=result;   // Unlearn lives in the Skill Editor
        uiCacheValid=false;
        if(okRun){
            if(action==0)++bodyOffset;else if(action==5)--bodyOffset;else if(action==8)bodyOffset=0;
            else if(action==1)++hatOffset;else if(action==6)--hatOffset;else if(action==7)hatOffset=0;
        }
        return 0;
    }
    if(msg==WM_ADREENA_WORLD){
        const int kind=(int)wp;const uint32_t u=(uint32_t)lp;float fv=0;memcpy(&fv,&u,4);
        menuGuard.unlock();
        std::wstring r;
        if(kind==0)r=spawner::setTimeScale(fv);
        else if(kind==1)r=spawner::setGravityScale(fv);
        else if(kind==2)r=spawner::setGraphicsToggle((int)((lp>>1)&7),(lp&1)!=0);
        else if(kind==6){char tg[64];snprintf(tg,sizeof(tg),"2.5 s after OFF using stacking %d",(int)((lp>>4)&15));r=spawner::logBuffState((int)(lp&15),tg);}
        else if(kind>=3&&kind<=5){
            const bool on=(lp&1)!=0;bool okRun=false;
            r=kind==3?spawner::setNightVisionGhost(0,on,okRun):(kind==4?spawner::setNightVisionGhost(1,on,okRun):spawner::setEnclaveLighting(on,okRun));
            if(!okRun&&on){menuGuard.lock();worldToggleFlags[kind-3].store(false);uiCacheValid=false;menuGuard.unlock();}   // could not apply: show the toggle OFF again
        }
        spawner::logStatus("World tools result",r);
        menuGuard.lock();
        return 0;
    }
    if(msg==WM_ADREENA_UNLIM){
        const bool on=wp!=0;
        menuGuard.unlock();
        const std::wstring result=spawner::setUnlimitedSpawning(on);
        spawner::logStatus("Unlimited spawning result",result);
        menuGuard.lock();
        hordeStatus=result;uiCacheValid=false;
        return 0;
    }
    if(msg==WM_ADREENA_ZMASK){
        menuGuard.unlock();
        unsigned char flags[zombieCount]{};bool okScan=false;
        {std::string why;if(spawner::init(why)){spawner::computeLoadedZombies(flags);okScan=true;}}
        menuGuard.lock();
        if(okScan){
            memcpy(zombieLoadedFlags,flags,sizeof(flags));zombieLoadedKnown=true;
            if(zombieLoadPending>=0&&zombieLoadPending<zombieCount&&zombieLoadedFlags[zombieLoadPending]){
                spawnStatus=L"Zombie class loaded - ready to spawn.";zombieLoadPending=-1;
            }
            uiCacheValid=false;
        }
        return 0;
    }
    if(msg==WM_ADREENA_ZLOAD){
        const int which=(int)wp;
        menuGuard.unlock();
        const std::wstring result=spawner::loadZombieClass(which);
        spawner::logStatus("Zombie load result",result);
        menuGuard.lock();
        if(result==L"ALREADY"){
            spawnStatus=L"Zombie class is already loaded - ready to spawn.";
            if(which>=0&&which<zombieCount)zombieLoadedFlags[which]=1;
        }else{
            spawnStatus=result;
            if(result.compare(0,7,L"Loading")==0)startZombiePoll(which);
        }
        uiCacheValid=false;
        return 0;
    }
    if(teleHotCapturing&&(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN||msg==WM_KEYUP||msg==WM_SYSKEYUP||msg==WM_CHAR||msg==WM_SYSCHAR)){
        if(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN){
            const int vk=(int)wp;
            if(vk==VK_CONTROL||vk==VK_SHIFT||vk==VK_MENU||vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_LSHIFT||vk==VK_RSHIFT||vk==VK_LMENU||vk==VK_RMENU||vk==VK_LWIN||vk==VK_RWIN)return 0;   // wait for the main key
            if(vk==VK_ESCAPE){teleHotCapturing=false;teleStatus=L"Hotkey unchanged";}
            else if(vk==VK_NEXT){teleStatus=L"Page Down opens the menu - pick another key";}
            else{teleHotVk=vk;teleHotMods=currentKeyMods();teleHotCapturing=false;teleStatus=L"Hotkey set: "+hotkeyName(vk,teleHotMods.load());}
            uiCacheValid=false;
        }
        return 0;
    }
    if((msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)&&!(lp&0x40000000)&&teleHotVk.load()&&(int)wp==teleHotVk.load()&&currentKeyMods()==teleHotMods.load()&&graphicsReady.load()&&editingHex<0&&gameWindow){
        teleStatus=L"Working...";uiCacheValid=false;
        PostMessageW(gameWindow,WM_ADREENA_TELE,0,0);
        return 0;
    }
    if((msg==WM_KEYUP||msg==WM_SYSKEYUP)&&wp==VK_NEXT){if(graphicsReady.load())setMenuOpen(!showMenu.load());return 0;}
    if(showMenu.load()&&graphicsReady.load()){
        if(msg==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;}
        if(msg==WM_MOUSEWHEEL){
            if(searchPopupVisible()){
                POINT sp{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd,&sp);clientToBuffer(sp);menuPosition();
                const int sx=toLogical(sp.x-menuX),sy=toLogical(sp.y-menuY);
                const RECT pr=searchPopupRect();
                if(sx>=pr.left&&sx<pr.right&&sy>=pr.top&&sy<pr.bottom){
                    const int steps=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA;
                    searchScroll=(std::max)(0,(std::min)((std::max)(0,(int)searchResults.size()-searchPopupRowsShown()),searchScroll-steps));
                    uiCacheValid=false;return 0;
                }
            }
            if(activeCategory==6){
                POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd,&pt);clientToBuffer(pt);menuPosition();
                const int mx=toLogical(pt.x-menuX),my=toLogical(pt.y-menuY);
                const int steps=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA;
                if(profDropOpen){
                    const int total=(int)profileNames.size();const int rows=(std::min)(6,(std::max)(1,total));
                    const int pTop=profDropField.bottom+1-settingsScroll,pBottom=pTop+rows*BANNER_ROW_HEIGHT;
                    if(mx>=profDropField.left&&mx<profDropField.right&&my>=pTop&&my<pBottom)
                        profDropScroll=(std::max)(0,(std::min)((std::max)(0,total-rows),profDropScroll-steps*2));
                    else profDropOpen=false;
                    uiCacheValid=false;return 0;
                }
                if(bannerDropdownOpen){
                    const int top=HEADER_H+53,dropX=settingsRightX()+6;
                    const int dropW=bannerDropW();
                    const int listTop=bannerDropdownTop(top+SETTINGS_BANNER_OFF)-settingsScroll;
                    if(mx>=dropX&&mx<dropX+dropW&&my>=listTop&&my<listTop+bannerPopupRows()*BANNER_ROW_HEIGHT){
                        bannerDropdownScroll-=steps*2;
                        clampBannerScroll(bannerPopupRows());uiCacheValid=false;
                    }
                }else if(mx>=SIDE&&mx<MW&&my>=HEADER_H+53&&my<MH-34){
                    // Move the glide TARGET (fractional, so smooth/high-resolution wheels work); animateScroll()
                    // eases settingsScroll toward it each frame.
                    const float notches=(float)GET_WHEEL_DELTA_WPARAM(wp)/(float)WHEEL_DELTA;
                    settingsScrollTarget=(std::max)(0.f,(std::min)((float)settingsMaxScroll(),settingsScrollTarget-notches*56.f));
                    pickerColor=-1;pickerDrag=-1;editingHex=-1;
                    uiCacheValid=false;
                }
                return 0;
            }
            POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd,&pt);clientToBuffer(pt);menuPosition();
            int x=toLogical(pt.x-menuX),y=toLogical(pt.y-menuY);
            if(x<SIDE||x>=MW||y<HEADER_H+53||y>=MH-34)return 0;
            const int left=SIDE+20,available=MW-SIDE-40,gap=28,cw=(available-gap)/2;
            const int right=left+cw+gap,top=HEADER_H+53;
            if(dropActive()){
                RECT pr;int rows=0,total=0;
                if(dropPopupGeom(pr,rows,total)&&x>=pr.left&&x<pr.right&&y>=pr.top&&y<pr.bottom){
                    const int steps=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA;
                    dropScroll=(std::max)(0,(std::min)((std::max)(0,total-rows),dropScroll-steps*2));
                }else dropOpen=0;   // scrolling the page elsewhere closes the list
                uiCacheValid=false;return 0;
            }
            float wheelSteps=-(float)GET_WHEEL_DELTA_WPARAM(wp)/(float)WHEEL_DELTA;
            for(int k=0;k<sectionCount(activeCategory);k++){
                int xx=sectionX(activeCategory,k), yy=top+sectionTopOffset(activeCategory,k)-pageScroll[activeCategory];
                const int cw=sectionW(activeCategory,k);
                if(sectionMaxScroll(activeCategory,k)<=0.f)continue;   // custom / non-scrolling sections never trap the wheel
                if(x>=xx&&x<xx+cw&&y>=yy+31&&y<yy+145){
                    float& offset=sectionScrollTarget[activeCategory][k];
                    offset=(std::max)(0.f,(std::min)(sectionMaxScroll(activeCategory,k),offset+wheelSteps*0.85f));
                    uiCacheValid=false;return 0; // At limit: STOP. Never chain to page scroll.
                }
            }
            float& offset=pageScrollTarget[activeCategory];
            offset=(std::max)(0.f,(std::min)((float)pageMaxScroll(),offset+wheelSteps*SECTION_ROW_H));
            uiCacheValid=false;return 0;
        }
        if(msg==WM_INPUT||msg==WM_MOUSEHWHEEL)return 0; // prevent game raw mouse look
        if(msg==WM_LBUTTONDOWN){int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);menuPosition();
            int lx=toLogical(x-menuX),ly=toLogical(y-menuY);
            if(searchClick(lx,ly))return 0;
            if(ly>=HEADER_H+1&&ly<=HEADER_H+20&&lx>=SIDE-26&&lx<=SIDE){
                SIDE=(SIDE<=SIDE_MIN?160:SIDE_MIN);uiCacheValid=false;return 0;
            }
            if(ly>=HEADER_H+20&&ly<MH-8&&lx>=SIDE-6&&lx<=SIDE+6){
                resizingSidebar=true;sidebarDragStartX=x;sidebarDragStartWidth=SIDE;SetCapture(hwnd);return 0;
            }
            if(activeCategory==6&&settingsMaxScroll()>0&&lx>=MW-18&&lx<MW&&ly>=HEADER_H+53&&ly<MH-34){
                const int trackTop=HEADER_H+53,trackH=MH-34-trackTop;
                const int thumbH=(std::max)(22,(int)lroundf(trackH*(float)trackH/(trackH+settingsMaxScroll())));
                const int thumbY=trackTop+(int)lroundf((trackH-thumbH)*(float)settingsScroll/settingsMaxScroll());
                if(ly>=thumbY&&ly<=thumbY+thumbH){
                    draggingSettingsScroll=true;settingsDragStartY=ly;settingsDragStartScroll=settingsScroll;
                }else{
                    settingsScroll=(std::max)(0,(std::min)(settingsMaxScroll(),
                        (int)lroundf((ly-trackTop-thumbH*0.5f)*settingsMaxScroll()/(trackH-thumbH))));
                    draggingSettingsScroll=true;settingsDragStartY=ly;settingsDragStartScroll=settingsScroll;
                }
                pickerColor=-1;editingHex=-1;uiCacheValid=false;SetCapture(hwnd);return 0;
            }
            if(activeCategory==6&&lx>=SIDE&&ly>=HEADER_H){
                clickMenu(x,y);
                if(pickerDrag>=0||draggingOpacity){SetCapture(hwnd);}
                return 0;
            }
            // Sidebar navigation must be handled on mouse DOWN as well.
            // Otherwise clicks never reach clickMenu() outside the content area.
            if(lx>=0&&lx<SIDE&&ly>=CAT_TOP&&ly<CAT_TOP+categoryCount*CAT_ROW){
                clickMenu(x,y);
                return 0;
            }
            // Grab section scrollbar thumbs (or click their tracks to jump).
            if(activeCategory>=0&&activeCategory<6&&ly>=HEADER_H+53&&ly<MH-34){
                const int left=SIDE+20,available=MW-SIDE-40,gap=28,cw=(available-gap)/2;
                const int right=left+cw+gap,top=HEADER_H+53;
                for(int k=0;k<sectionCount(activeCategory);k++){
                    int xx=sectionX(activeCategory,k),yy=top+sectionTopOffset(activeCategory,k)-(int)pageScroll[activeCategory];
                    const int cw=sectionW(activeCategory,k);
                    int count=sectionItemCount(activeCategory,k);
                    if(count<=SECTION_VIEW_ROWS||sectionMaxScroll(activeCategory,k)<=0.f)continue;
                    int trackX=xx+cw-3,trackTop=yy+31,trackH=114;
                    if(lx<trackX-5||lx>trackX+6||ly<trackTop||ly>=trackTop+trackH)continue;
                    int thumbH=(std::max)(18,(int)roundf(trackH*3.f/count));
                    int thumbY=trackTop+(int)roundf((trackH-thumbH)*sectionScroll[activeCategory][k]/sectionMaxScroll(activeCategory,k));
                    draggingScrollbar=k;scrollbarDragCategory=activeCategory;
                    scrollbarDragGrab=(ly>=thumbY&&ly<thumbY+thumbH)?ly-thumbY:thumbH/2;
                    updateScrollbarDrag(ly);SetCapture(hwnd);return 0;
                }
                // Page scrollbar, on the far-right edge.
                if(pageMaxScroll()>0&&lx>=MW-12&&lx<=MW&&ly>=top){
                    int trackH=MH-34-top;
                    int thumbH=(std::max)(22,(int)roundf(trackH*(float)trackH/(trackH+pageMaxScroll())));
                    int thumbY=top+(int)roundf((trackH-thumbH)*pageScroll[activeCategory]/pageMaxScroll());
                    draggingScrollbar=-2;scrollbarDragCategory=activeCategory;
                    scrollbarDragGrab=(ly>=thumbY&&ly<thumbY+thumbH)?ly-thumbY:thumbH/2;
                    updateScrollbarDrag(ly);SetCapture(hwnd);return 0;
                }
            }
            // Start feature slider capture on mouse DOWN, not mouse UP.
            // This prevents the old click-once/move/click-again behavior.
            if(lx>=SIDE&&lx<MW&&ly>=HEADER_H+53&&ly<MH-34){
                clickMenu(x,y);
                if(draggingFeatureSlider>=0)SetCapture(hwnd);
                return 0;
            }
            if(lx>=0&&lx<MW&&ly>=0&&ly<HEADER_H){dragging=true;dragStart={x,y};dragOriginX=menuX;dragOriginY=menuY;SetCapture(hwnd);}return 0;}
        if(msg==WM_MOUSEMOVE){
            if(draggingSettingsScroll){
                const int trackH=MH-34-(HEADER_H+53);
                const int thumbH=(std::max)(22,(int)lroundf(trackH*(float)trackH/(trackH+settingsMaxScroll())));
                const int delta=toLogical(GET_Y_LPARAM(lp)-menuY)-settingsDragStartY;
                settingsScroll=(std::max)(0,(std::min)(settingsMaxScroll(),
                    settingsDragStartScroll+(int)lroundf(delta*(float)settingsMaxScroll()/(trackH-thumbH))));
                uiCacheValid=false;return 0;
            }
            if(draggingScrollbar!=-1){updateScrollbarDrag(toLogical(GET_Y_LPARAM(lp)-menuY));return 0;}
            if(resizingSidebar){
                int newWidth=sidebarDragStartWidth+(int)lroundf((float)(GET_X_LPARAM(lp)-sidebarDragStartX)/uiScale);
                if(newWidth<SIDE_MIN)newWidth=SIDE_MIN;
                if(newWidth>SIDE_MAX)newWidth=SIDE_MAX;
                if(SIDE!=newWidth){SIDE=newWidth;uiCacheValid=false;}
                return 0;
            }
            if(draggingOpacity){setOpacityFromLogicalX(toLogical(GET_X_LPARAM(lp)-menuX));return 0;}
            if(draggingFeatureSlider>=0){
                int left=SIDE+20,available=MW-SIDE-40,gap=28,cw=(available-gap)/2;
                int sx=(draggingFeatureSlider==3)?left+cw+gap:left;   // Character Time Dilation now sits in the left column
                setFeatureSlider(draggingFeatureSlider,toLogical(GET_X_LPARAM(lp)-menuX),sx+6,cw-12);return 0;
            }
            if(pickerDrag>=0){pickerUpdate(toLogical(GET_X_LPARAM(lp)-menuX),toLogical(GET_Y_LPARAM(lp)-menuY)+(activeCategory==6?settingsScroll:0));return 0;}if(dragging){menuX=dragOriginX+GET_X_LPARAM(lp)-dragStart.x;menuY=dragOriginY+GET_Y_LPARAM(lp)-dragStart.y;int bw=0,bh=0;bufferSize(bw,bh);if(menuX<0)menuX=0;if(menuY<0)menuY=0;if(menuX>bw-TW)menuX=bw-TW;if(menuY>bh-TH)menuY=bh-TH;if(menuX<0)menuX=0;if(menuY<0)menuY=0;menuPosition();}return 0;}
        if(msg==WM_LBUTTONUP){if(draggingSettingsScroll){draggingSettingsScroll=false;ReleaseCapture();return 0;}if(draggingScrollbar!=-1){draggingScrollbar=-1;scrollbarDragCategory=-1;ReleaseCapture();return 0;}if(draggingFeatureSlider>=0){draggingFeatureSlider=-1;ReleaseCapture();return 0;}if(resizingSidebar){resizingSidebar=false;ReleaseCapture();return 0;}if(activeCategory==6){if(draggingOpacity){draggingOpacity=false;ReleaseCapture();return 0;}if(pickerDrag>=0){pickerDrag=-1;ReleaseCapture();return 0;}if(dragging){dragging=false;ReleaseCapture();return 0;}if(toLogical(GET_X_LPARAM(lp)-menuX)<SIDE)clickMenu(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));return 0;}if(pickerDrag>=0){pickerDrag=-1;ReleaseCapture();return 0;}if(dragging){dragging=false;ReleaseCapture();}return 0;}
        if(editingHex>=0&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;int i=editingHex;size_t n=wcslen(hexDraft[i]);
            if(ch==L'\r'){if(commitHex(i)){editingHex=-1;}uiCacheValid=false;return 0;}
            if(ch==27){syncHex(i);editingHex=-1;uiCacheValid=false;return 0;}
            if(ch==L'\b'){if(n>0)hexDraft[i][n-1]=0;uiCacheValid=false;return 0;}
            if(hexDigit(ch)>=0&&n<7){hexDraft[i][n]=ch;hexDraft[i][n+1]=0;uiCacheValid=false;}
            return 0;
        }
        if(editingNum==204&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;size_t n=wcslen(itemFilter);
            if(ch==L'\r'||ch==27){editingNum=-1;uiCacheValid=false;return 0;}
            if(ch==L'\b'){if(n>0)itemFilter[n-1]=0;itemPage=0;uiCacheValid=false;return 0;}
            if(ch>=32&&ch!=127&&n<20){itemFilter[n]=ch;itemFilter[n+1]=0;itemPage=0;uiCacheValid=false;}
            return 0;
        }
        if(editingNum==207&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;size_t n=wcslen(searchBuf);
            if(ch==27){searchClose();return 0;}
            if(ch==L'\r'){if(!searchResults.empty())searchGo(0);else searchClose();return 0;}
            if(ch==L'\b'){if(n>0){searchBuf[n-1]=0;searchRebuildResults();}uiCacheValid=false;return 0;}
            if(n<24&&ch>=32&&ch<127){searchBuf[n]=ch;searchBuf[n+1]=0;searchRebuildResults();uiCacheValid=false;}
            return 0;
        }
        if(editingNum==205&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;size_t n=wcslen(profileNameBuf);
            if(ch==L'\r'||ch==27){editingNum=-1;uiCacheValid=false;return 0;}
            if(ch==L'\b'){if(n>0)profileNameBuf[n-1]=0;uiCacheValid=false;return 0;}
            if(n<24&&ch<128&&(iswalnum(ch)||ch==L' '||ch==L'-'||ch==L'_')){profileNameBuf[n]=ch;profileNameBuf[n+1]=0;uiCacheValid=false;}
            return 0;
        }
        if(editingNum==206&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;size_t n=wcslen(bookmarkNameBuf);
            if(ch==L'\r'||ch==27){editingNum=-1;uiCacheValid=false;return 0;}
            if(ch==L'\b'){if(n>0)bookmarkNameBuf[n-1]=0;uiCacheValid=false;return 0;}
            if(n<24&&ch>=32&&ch<127){bookmarkNameBuf[n]=ch;bookmarkNameBuf[n+1]=0;uiCacheValid=false;}
            return 0;
        }
        if((editingNum==201||editingNum==202||editingNum==203)&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;wchar_t* tb=editingNum==201?nameFirst:editingNum==202?nameLast:nameNick;size_t n=wcslen(tb);
            if(ch==L'\r'||ch==27){editingNum=-1;uiCacheValid=false;return 0;}
            if(ch==L'\b'){if(n>0)tb[n-1]=0;uiCacheValid=false;return 0;}
            if(ch>=32&&ch!=127&&n<20){tb[n]=ch;tb[n+1]=0;uiCacheValid=false;}
            return 0;
        }
        if(editingNum>=0&&msg==WM_CHAR){
            wchar_t ch=(wchar_t)wp;size_t n=wcslen(numDraft);
            if(ch==L'\r'){commitNum();uiCacheValid=false;return 0;}
            if(ch==27){editingNum=-1;uiCacheValid=false;return 0;}
            if(ch==L'\b'){if(n>0)numDraft[n-1]=0;uiCacheValid=false;return 0;}
            if(ch>=L'0'&&ch<=L'9'&&n<4u){numDraft[n]=ch;numDraft[n+1]=0;uiCacheValid=false;}
            return 0;
        }
        // Typing into a menu box: the game must not see those keys.
        if((editingNum>=0||editingHex>=0)&&(msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN||msg==WM_KEYUP||msg==WM_SYSKEYUP||msg==WM_CHAR||msg==WM_SYSCHAR))return 0;
        // The menu key itself never reaches the game.
        if((msg==WM_KEYDOWN||msg==WM_SYSKEYDOWN)&&wp==VK_NEXT)return 0;
        // Mouse stays locked to the menu while it is open; the keyboard (and controller) keep working in the game.
        if(msg==WM_RBUTTONDOWN||msg==WM_RBUTTONUP||msg==WM_MBUTTONDOWN||msg==WM_MBUTTONUP)return 0;
    }
    menuGuard.unlock();
    return oldWndProc?CallWindowProcW(oldWndProc,hwnd,msg,wp,rawLp):DefWindowProcW(hwnd,msg,wp,rawLp);
}
HRESULT __stdcall hookedPresent(IDXGISwapChain* swap,UINT sync,UINT flags){
    if(!originalPresent)return E_FAIL;
    if(running.load()){
        DXGI_SWAP_CHAIN_DESC desc{};
        if(SUCCEEDED(swap->GetDesc(&desc))&&desc.OutputWindow){
            if(!gameWindow)gameWindow=desc.OutputWindow;
            static ULONGLONG nextAttempt=0;
            ULONGLONG now=GetTickCount64();
            if((!graphicsReady.load()||activeSwap!=swap)&&now>=nextAttempt){
                nextAttempt=now+5000;{std::lock_guard<std::recursive_mutex> menuGuard(menuLock);setupRenderer(swap);}
            }
            if(graphicsReady.load()&&!oldWndProc){
                SetLastError(0);oldWndProc=(WNDPROC)SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,(LONG_PTR)menuWndProc);
                if(!oldWndProc)logMessage("WndProc subclass failed",HRESULT_FROM_WIN32(GetLastError()));
            }
            {   // saved startup profile: layout as soon as the overlay is ready, gameplay settings once a character exists
                static int stage=0;static ULONGLONG since=0;
                if(graphicsReady.load()&&oldWndProc&&gameWindow){
                    if(stage==0){
                        stage=1;
                        {std::lock_guard<std::recursive_mutex> pg(menuLock);profilesInit();}
                        {   // aimbot ticker: asks the game thread for a tick about every 8 ms, but only while the Aimbot toggle is on
                            HWND gw=gameWindow;
                            std::thread([gw]{   // keep-ticker: every 40 ms while Infinite Ammo / Infinite Durability / Vehicle Health Lock is on (one more tick after switching off, to restore)
                                bool was=false;
                                while(running.load()){Sleep(40);const bool on=infiniteAmmo.load()||vehicleHealthLock.load()||infiniteCarFuel.load();
                                    if((on||was)&&gw&&!spawner::g_keepPending.exchange(true))PostMessageW(gw,WM_ADREENA_KEEP,0,0);was=on;}
                            }).detach();
                            std::thread([gw]{while(running.load()){Sleep(8);if(aimbotFlag.load()&&gw&&!spawner::g_aimPending.exchange(true))PostMessageW(gw,WM_ADREENA_AIM,0,0);}}).detach();
                            std::thread([gw]{   // Give Item list: ask the game thread once a second until the game has settled and the list has been read
                                for(int i=0;i<300&&running.load()&&!giveAutoStop.load();++i){Sleep(1000);if(gw)PostMessageW(gw,WM_ADREENA_COMM,14,0);}
                            }).detach();
                        }
                        if(!profileAuto.empty())PostMessageW(gameWindow,WM_ADREENA_PROFILE,1,0);else stage=3;
                    }else if(stage==1&&capturedCharacter.load()){
                        const ULONGLONG t2=GetTickCount64();
                        if(!since)since=t2;
                        else if(t2-since>4000){stage=3;PostMessageW(gameWindow,WM_ADREENA_PROFILE,2,0);}
                    }
                }
            }
            if(showMenu.load()&&graphicsReady.load()&&activeSwap==swap&&oldWndProc)drawOverlay(swap);
        }
    }
    return originalPresent(swap,sync,flags);
}
bool installPresentHook(){
    HWND hwnd=CreateWindowExW(0,L"STATIC",L"ADREENA_D3D11_probe",WS_POPUP,0,0,4,4,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!hwnd)return false;
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Width=4;desc.BufferDesc.Height=4;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=hwnd;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* swap=nullptr;ID3D11Device* dev=nullptr;ID3D11DeviceContext* ctx=nullptr;D3D_FEATURE_LEVEL level{};
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested,2,D3D11_SDK_VERSION,&desc,&swap,&dev,&level,&ctx);
    if(SUCCEEDED(hr)){
        void** table=*(void***)swap;presentSlot=&table[8];originalPresent=(PresentFn)table[8];
        DWORD old{};
        if(VirtualProtect(presentSlot,sizeof(void*),PAGE_EXECUTE_READWRITE,&old)){
            InterlockedExchangePointer((PVOID volatile*)presentSlot,(PVOID)hookedPresent);
            DWORD ignored{};VirtualProtect(presentSlot,sizeof(void*),old,&ignored);
        }else originalPresent=nullptr;
    }else logMessage("Dummy D3D11 swapchain failed",hr);
    drop(ctx);drop(dev);drop(swap);DestroyWindow(hwnd);
    return originalPresent!=nullptr;
}

// New CT entries: fail closed if signatures are missing or already modified.
struct DirectPatch {uintptr_t at=0;unsigned char original[8]{};unsigned char changed[8]{};size_t size=0;bool active=false;};
DirectPatch weightDirect,boneDirect,durDirect;
bool prepareDirectPatch(uintptr_t module,DirectPatch& patch,const unsigned char* pattern,size_t n,size_t patchOffset,
                        const unsigned char* expected,const unsigned char* replacement,size_t patchSize){
    uintptr_t found=findUniqueCtPattern(module,pattern,n);
    if(!found||patchSize>8||!accessible((void*)(found+patchOffset),patchSize)||
       memcmp((void*)(found+patchOffset),expected,patchSize))return false;
    patch.at=found+patchOffset;patch.size=patchSize;
    memcpy(patch.original,expected,patchSize);memcpy(patch.changed,replacement,patchSize);
    return true;
}
template<size_t N> bool prepareMaskedPatch(uintptr_t module,DirectPatch& patch,const int (&sig)[N],
        size_t offset,const unsigned char* expected,const unsigned char* replacement,size_t count){
    uintptr_t address=findMasked(module,sig);
    if(!address||count>sizeof(patch.original)||!accessible((void*)(address+offset),count)||
       memcmp((void*)(address+offset),expected,count))return false;
    patch.at=address+offset;patch.size=count;
    memcpy(patch.original,expected,count);memcpy(patch.changed,replacement,count);
    return true;
}
void applyDirectPatch(DirectPatch& patch,bool enabled){
    if(!patch.at||patch.active==enabled)return;
    const unsigned char* expected=patch.active?patch.changed:patch.original;
    if(memcmp((void*)patch.at,expected,patch.size))return;
    DWORD old{};if(!VirtualProtect((void*)patch.at,patch.size,PAGE_EXECUTE_READWRITE,&old))return;
    memcpy((void*)patch.at,enabled?patch.changed:patch.original,patch.size);
    FlushInstructionCache(GetCurrentProcess(),(void*)patch.at,patch.size);
    DWORD ignored{};VirtualProtect((void*)patch.at,patch.size,old,&ignored);
    patch.active=enabled;
}
// Infinite Ammo (reserve): the game's "remove N of item class F from the inventory" routine does `sub ecx,r15d` at exe+4EF604
// (F = [rbp+0x6f], the UClass being removed). If F is the AmmoItem class or one of its children, the subtraction is skipped
// (reserve stays); any other class (food, medicine, ...) is subtracted as normal. Only active while the toggle is on.
bool installAmmoReserveHook(uintptr_t module){
    static const unsigned char expect[]={0x41,0x2B,0xCF,0x89,0x08,0x33,0xD2,0x41,0x8B,0xCF,0xEB,0x0C,0x75,0x03,0x41,0x8B,0xCF,0x33,0xD2,0x89,0x10};
    const uintptr_t target=module+0x4EF604;
    if(!accessible((void*)target,sizeof(expect))||memcmp((void*)target,expect,sizeof(expect))){logMessage("Infinite Ammo: reserve write site not found - toggle unavailable");return false;}
    auto* cave=(unsigned char*)allocateNear(target,512);
    if(!cave){logMessage("Infinite Ammo: nearby allocation failed");return false;}
    std::vector<unsigned char> c;
    auto b=[&](std::initializer_list<unsigned char> v){for(unsigned char x:v)c.push_back(x);};
    auto imm64=[&](uintptr_t v){appendBytes(c,(unsigned char*)&v,8);};
    struct Fix{size_t at;int label;};std::vector<Fix> fixes;size_t lab[5]{};   // 0 normal,1 other,2 skip,3 loop,4 back
    auto rel32=[&](int label){fixes.push_back({c.size(),label});c.insert(c.end(),4,0);};
    b({0x9C,0x50,0x52});                                   // pushfq; push rax; push rdx
    b({0x48,0xB8});imm64((uintptr_t)&infiniteAmmo);        // mov rax,&flag
    b({0x80,0x38,0x00});                                   // cmp byte [rax],0
    b({0x0F,0x84});rel32(0);                               // je normal
    b({0x48,0xBA});imm64((uintptr_t)&ammoItemClassPtr);    // mov rdx,&ammoClass
    b({0x48,0x8B,0x12});                                   // mov rdx,[rdx]
    b({0x48,0x85,0xD2});                                   // test rdx,rdx
    b({0x0F,0x84});rel32(0);                               // je normal
    b({0x48,0x8B,0x45,0x6F});                              // mov rax,[rbp+6F]   (class being removed)
    lab[3]=c.size();
    b({0x48,0x85,0xC0});                                   // loop: test rax,rax
    b({0x0F,0x84});rel32(1);                               // je other
    b({0x48,0x39,0xD0});                                   // cmp rax,rdx
    b({0x0F,0x84});rel32(2);                               // je skip
    b({0x48,0x8B,0x40,0x30});                              // mov rax,[rax+30]   (super class)
    b({0xEB});c.push_back((unsigned char)(lab[3]-(c.size()+1)));   // jmp loop
    lab[1]=c.size();                                       // other: remember the class, then subtract normally
    b({0x48,0x8B,0x55,0x6F});                              // mov rdx,[rbp+6F]
    b({0x48,0xB8});imm64((uintptr_t)&ammoLastOther);       // mov rax,&lastOther
    b({0x48,0x89,0x10});                                   // mov [rax],rdx
    lab[0]=c.size();                                       // normal:
    b({0x5A,0x58,0x9D});                                   // pop rdx; pop rax; popfq
    b({0x41,0x2B,0xCF,0x89,0x08});                         // sub ecx,r15d ; mov [rax],ecx
    b({0xE9});rel32(4);                                    // jmp back
    lab[2]=c.size();                                       // skip: count it, keep the reserve as it is
    b({0x48,0xB8});imm64((uintptr_t)&ammoKeptCount);       // mov rax,&kept
    b({0xFF,0x00});                                        // inc dword [rax]
    b({0x5A,0x58,0x9D});                                   // pop rdx; pop rax; popfq
    b({0x89,0x08});                                        // mov [rax],ecx   (value unchanged)
    b({0xE9});rel32(4);                                    // jmp back
    lab[4]=(size_t)((int64_t)(target+5)-(int64_t)(uintptr_t)cave);   // absolute return, as an offset from the cave
    for(const Fix& f:fixes){
        const int64_t rel=(int64_t)lab[f.label]-(int64_t)(f.at+4);
        if(rel<INT32_MIN||rel>INT32_MAX){VirtualFree(cave,0,MEM_RELEASE);return false;}
        const int32_t r=(int32_t)rel;memcpy(c.data()+f.at,&r,4);
    }
    if(c.size()>512){VirtualFree(cave,0,MEM_RELEASE);return false;}
    unsigned char patch[5]{};
    if(!relativeJump(patch,target,(uintptr_t)cave)){VirtualFree(cave,0,MEM_RELEASE);return false;}
    memcpy(cave,c.data(),c.size());FlushInstructionCache(GetCurrentProcess(),cave,c.size());
    DWORD old{};if(!VirtualProtect((void*)target,5,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(cave,0,MEM_RELEASE);return false;}
    CtHookRecord rec{};rec.address=target;rec.size=5;rec.cave=cave;memcpy(rec.original,(void*)target,5);
    memcpy((void*)target,patch,5);FlushInstructionCache(GetCurrentProcess(),(void*)target,5);
    DWORD ign{};VirtualProtect((void*)target,5,old,&ign);
    ctInstalled.push_back(rec);ammoPatchReady=true;
    logMessage("Infinite Ammo: reserve hook ready (+4EF604, ammunition only)");return true;
}
void installNewDirectPatches(uintptr_t module){
    // The CT weight AOB has wildcards; use the fully verified original instruction
    // plus the following epilogue bytes, and patch only the 3-byte movaps instruction.
    static const unsigned char weightSig[]={0x0F,0x28,0xC7,0x0F,0x28,0x7C,0x24,0x50,0x48,0x81,0xC4};
    static const unsigned char weightOrig[]={0x0F,0x28,0xC7},weightOn[]={0x0F,0x57,0xC0};
    weightPatchReady=prepareDirectPatch(module,weightDirect,weightSig,sizeof(weightSig),0,weightOrig,weightOn,3);
    static const unsigned char boneSig[]={0x40,0x57,0x41,0x55,0x48,0x83,0xEC,0x48,0x44,0x0F,0xB6,0xEA};
    static const unsigned char boneOrig[]={0x40,0x57},boneOn[]={0xC3,0x90};
    bonePatchReady=prepareDirectPatch(module,boneDirect,boneSig,sizeof(boneSig),0,boneOrig,boneOn,2);
    installAmmoReserveHook(module);
    {   // Infinite Durability (CT "infdurability", exe+4E2322): the store of the reduced durability is removed and the "went below zero" fix-up is jumped over
        static const unsigned char durSig[]={0xF3,0x41,0x0F,0x11,0x46,0x60,0x73,0x04,0x45,0x89,0x66,0x60};
        static const unsigned char durOrig[]={0xF3,0x41,0x0F,0x11,0x46,0x60,0x73,0x04},durOn[]={0x90,0x90,0x90,0x90,0x90,0x90,0xEB,0x04};
        durPatchReady=prepareDirectPatch(module,durDirect,durSig,sizeof(durSig),0,durOrig,durOn,8);
        ctFeatureReady[0]=durPatchReady;
        logMessage(durPatchReady?"Infinite Durability: wear patch ready (+4E2322)":"Infinite Durability: wear site not found - toggle unavailable");
    }
    logMessage(weightPatchReady?"Unlimited Weight patch ready":"Unlimited Weight pattern not unique/absent");
    logMessage(bonePatchReady?"No Broken Bone patch ready":"No Broken Bone pattern not unique/absent");
}
// Radar-specific scanner: require the CT wildcard pattern AND the exact bytes
// expected at the injection site. A broad AOB alone may have several matches.
// Never choose arbitrarily when more than one validated site remains.
template<size_t N, size_t M>
uintptr_t findRadarVerifiedSite(uintptr_t module,const int (&sig)[N],
                               const unsigned char (&expected)[M],
                               const char* label){
    auto* dos=(IMAGE_DOS_HEADER*)module;
    if(!accessible(dos,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
    auto* nt=(IMAGE_NT_HEADERS64*)(module+dos->e_lfanew);
    if(!accessible(nt,sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE)return 0;
    size_t matches=0,verified=0;uintptr_t result=0;
    auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++){
        const auto& sec=sections[i];
        if(!(sec.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        uintptr_t start=module+sec.VirtualAddress;size_t size=sec.Misc.VirtualSize;
        if(size<N||!accessible((void*)start,size))continue;
        for(size_t pos=0;pos<=size-N;pos++){
            const unsigned char* bytes=(const unsigned char*)(start+pos);
            size_t j=0;for(;j<N;j++)if(sig[j]>=0&&bytes[j]!=(unsigned char)sig[j])break;
            if(j!=N)continue;
            ++matches;
            if(M<=N&&!memcmp(bytes,expected,M)){++verified;result=start+pos;}
        }
    }
    char msg[192]{};
    sprintf_s(msg,"Zombie Radar %s: AOB matches=%zu, verified sites=%zu",label,matches,verified);
    logMessage(msg);
    return verified==1?result:0;
}
// Radar Site 2: require exactly one full CE AOB match in executable code.
// The signature fixes the cmp opcode and surrounding branch structure while
// allowing its four-byte displacement to vary, as the working CT script does.
template<size_t N>
uintptr_t findRadarWildcardSite(uintptr_t module,const int (&sig)[N],const char* label){
    auto* dos=(IMAGE_DOS_HEADER*)module;
    if(!accessible(dos,sizeof(*dos))||dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
    auto* nt=(IMAGE_NT_HEADERS64*)(module+dos->e_lfanew);
    if(!accessible(nt,sizeof(*nt))||nt->Signature!=IMAGE_NT_SIGNATURE)return 0;
    size_t matches=0,verified=0;uintptr_t result=0;
    auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++){
        const auto& sec=sections[i];
        if(!(sec.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        uintptr_t start=module+sec.VirtualAddress;size_t size=sec.Misc.VirtualSize;
        if(size<N||!accessible((void*)start,size))continue;
        for(size_t pos=0;pos<=size-N;pos++){
            const unsigned char* bytes=(const unsigned char*)(start+pos);
            size_t j=0;for(;j<N;j++)if(sig[j]>=0&&bytes[j]!=(unsigned char)sig[j])break;
            if(j!=N)continue;
            ++matches;
            // Six-byte cmp [rbx+disp32],al, followed by a two-byte JE.
            // This is the complete instruction layout needed for a +8 skip.
            if(bytes[0]==0x38&&bytes[1]==0x83&&bytes[6]==0x74){++verified;result=start+pos;}
        }
    }
    char msg[192]{};
    sprintf_s(msg,"Zombie Radar %s: AOB matches=%zu, verified sites=%zu",label,matches,verified);
    logMessage(msg);
    return matches==1&&verified==1?result:0;
}
void installPlannedHooks(uintptr_t module){
    // No Blood Plague: CT reads xmm7 from survivor plague state; zero xmm7 while ON.
    static const int plagueSig[]={0xF3,0x0F,-1,-1,-1,-1,-1,-1,0xF3,0x44,-1,-1,-1,-1,-1,-1,-1,0x40,0x84};
    static const unsigned char plagueOrig[]={0xF3,0x0F,0x10,0xBE,0x80,0x04,0x00,0x00};
    static const unsigned char plagueOn[]={0x0F,0x57,0xFF}; // xorps xmm7,xmm7
    uintptr_t p=findMasked(module,plagueSig);
    const CtHookSpec plague={"No Blood Plague",plagueOrig,8,8,plagueOn,sizeof(plagueOn),true,&plannedFlags[0]};
    plannedReady[0]=p&&installCtHook(module,plague,p);
    logMessage(plannedReady[0]?"No Blood Plague: ready":"No Blood Plague: signature missing/ambiguous");

    static const int radioSig[]={0xF3,0x0F,-1,-1,-1,0xE9,-1,-1,-1,-1,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x48,0x89,-1,-1,-1,0x55,0x56};
    static const unsigned char radioOrig[]={0xF3,0x0F,0x10,0x41,0x08};
    // mov dword ptr [rcx+08],0 ; original movss xmm0,[rcx+08] is replayed by hook.
    static const unsigned char radioOn[]={0xC7,0x41,0x08,0x00,0x00,0x00,0x00};
    uintptr_t r=findMasked(module,radioSig);
    const CtHookSpec radio={"Instant Radio Cooldown",radioOrig,5,5,radioOn,sizeof(radioOn),false,&plannedFlags[1]};
    plannedReady[1]=r&&installCtHook(module,radio,r);
    logMessage(plannedReady[1]?"Instant Radio Cooldown: ready":"Instant Radio Cooldown: signature missing/ambiguous");

    static const int accuracySig[]={0xF3,0x0F,-1,-1,-1,-1,-1,-1,0x48,0x8B,-1,-1,0xF3,0x0F,-1,-1,-1,-1,-1,-1,0x0F,0x2F};
    static const unsigned char accuracyOrig[]={0xF3,0x0F,0x11,0x83,0xFC,0x00,0x00,0x00};
    static const unsigned char accuracyOn[]={0x0F,0x57,0xC0}; // xorps xmm0,xmm0 before original store
    // Scan both CT scripts BEFORE installing either hook.  Their first sites
    // have the same eight instruction bytes, but different ninth bytes in the
    // supplied CT; matching instruction bytes alone do not prove overlap.
    static const int noSpread1Sig[]={0xF3,0x0F,0x11,0x83,0xFC,0x00,0x00,0x00,0x77};
    static const int noSpread2Sig[]={0xF3,0x0F,0x11,0x87,0xFC,0x00,0x00,0x00,0x0F};
    static const unsigned char noSpread2Orig[]={0xF3,0x0F,0x11,0x87,0xFC,0x00,0x00,0x00};
    uintptr_t a=findMasked(module,accuracySig);
    uintptr_t n1=findMasked(module,noSpread1Sig);
    uintptr_t n2=findMasked(module,noSpread2Sig);
    accuracySharesNoSpread=(a&&n1&&a==n1);
    // A shared site is installed once. Its condition is the OR of the two
    // menu switches. A distinct first site gets its own independently gated hook.
    const CtHookSpec accuracy={"Perfect Accuracy",accuracyOrig,8,8,accuracyOn,sizeof(accuracyOn),false,&accuracySharedActive};
    plannedReady[2]=a&&installCtHook(module,accuracy,a);
    logMessage(plannedReady[2]?"Perfect Accuracy: ready":"Perfect Accuracy: signature missing/ambiguous");
    bool firstReady=false;
    if(accuracySharesNoSpread){
        firstReady=plannedReady[2];
        logMessage("No Spread: first site shared with Perfect Accuracy");
    }else if(n1){
        const CtHookSpec first={"No Spread site 1",accuracyOrig,8,8,accuracyOn,sizeof(accuracyOn),false,&noSpread};
        firstReady=installCtHook(module,first,n1);
        logMessage(firstReady?"No Spread: first site ready (separate)":"No Spread: first site conflict/installation failed");
    }else logMessage("No Spread: first site signature missing/ambiguous");
    bool secondReady=false;
    if(n2){
        const CtHookSpec second={"No Spread site 2",noSpread2Orig,8,8,accuracyOn,sizeof(accuracyOn),false,&noSpread};
        secondReady=installCtHook(module,second,n2);
    }
    noSpreadReady=firstReady&&secondReady;
    logMessage(noSpreadReady?"No Spread: both sites ready":"No Spread: unavailable (first or second site missing/conflicting)");
    syncAccuracyShared();
}

void installSkillHook(uintptr_t module){
    static const unsigned char sig[]={0x44,0x0F,0xB6,0x68,0x08,0xE8};
    static const unsigned char setSeven[]={0xC6,0x40,0x08,0x07};
    const CtHookSpec spec={"Max Skills hook",sig,sizeof(sig),5,setSeven,sizeof(setSeven),false,&maxAllSkills};
    skillHookReady=installCtHook(module,spec);
    logMessage(skillHookReady?"Max Skills: hook installed and ready":"Max Skills: signature missing/ambiguous or installation failed");
}
// One shared CT Melee hook; each flag independently controls its two stat fields.
void installMeleeStatsHook(uintptr_t module){
    static const unsigned char sig[]={0x8B,0x48,0x34,0x41,0x89,0x08,0xC3,0xCC,0xCC,0xCC};
    uintptr_t target=findUniqueCtPattern(module,sig,sizeof(sig));
    if(!target)return;
    for(const auto& h:ctInstalled)if(target<h.address+h.size&&h.address<target+6)return;
    auto* cave=(unsigned char*)allocateNear(target,512);
    if(!cave)return;
    std::vector<unsigned char> code;
    auto emit=[&](std::initializer_list<unsigned char> b){code.insert(code.end(),b);};
    emit({0x9C,0x41,0x52}); // pushfq; push r10
    const unsigned char offsets[7][2]={{0x30,0x34},{0x40,0x44},{0x50,0x54},{0x60,0x64},{0x78,0x7C},{0x80,0x84},{0x88,0x8C}};
    const uint32_t huge=0x497423F0u; // 999999.0f
    for(int i=0;i<7;i++){
        emit({0x49,0xBA});uintptr_t flag=(uintptr_t)&meleeStatFlags[i];appendBytes(code,(unsigned char*)&flag,8);
        emit({0x41,0x80,0x3A,0x00,0x0F,0x84}); // cmp byte ptr [r10],0; je after writes
        size_t skipDisp=code.size();code.insert(code.end(),4,0);
        for(int j=0;j<2;j++){
            emit({0xC7,0x80}); // mov dword ptr [rax+disp32], immediate
            uint32_t offset=offsets[i][j];appendBytes(code,(unsigned char*)&offset,4);
            uint32_t bits=(i==4||i==6)?0u:huge;
            appendBytes(code,(unsigned char*)&bits,4);
        }
        int32_t skip=(int32_t)(code.size()-(skipDisp+4));
        memcpy(code.data()+skipDisp,&skip,4);
    }
    emit({0x41,0x5A,0x9D,0x8B,0x48,0x34,0x41,0x89,0x08});
    size_t jumpAt=code.size();code.insert(code.end(),5,0);
    int64_t back=(int64_t)(target+6)-(int64_t)((uintptr_t)cave+jumpAt+5);
    if(back<INT32_MIN||back>INT32_MAX||code.size()>512){VirtualFree(cave,0,MEM_RELEASE);return;}
    code[jumpAt]=0xE9;int32_t d=(int32_t)back;memcpy(code.data()+jumpAt+1,&d,4);
    unsigned char patch[6]{};if(!relativeJump(patch,target,(uintptr_t)cave)){VirtualFree(cave,0,MEM_RELEASE);return;}
    patch[5]=0x90;memcpy(cave,code.data(),code.size());FlushInstructionCache(GetCurrentProcess(),cave,code.size());
    DWORD old{};if(!VirtualProtect((void*)target,6,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(cave,0,MEM_RELEASE);return;}
    CtHookRecord rec{};rec.address=target;rec.size=6;rec.cave=cave;memcpy(rec.original,(void*)target,6);
    memcpy((void*)target,patch,6);FlushInstructionCache(GetCurrentProcess(),(void*)target,6);
    DWORD ignored{};VirtualProtect((void*)target,6,old,&ignored);ctInstalled.push_back(rec);
    meleeStatsReady=true;logMessage("Shared Melee stat hook installed");
}

// CT-derived additions: one-time hook installation and reversible code patches.
// When a signature is absent/ambiguous, the corresponding UI toggle stays disabled.
DirectPatch searchPatch,surveyPatch,radarSkipPatch,dupePatch;
void installSevenCtFeatures(uintptr_t module){
    // Quick Search: maxss -> minss, changing just the opcode (5F to 5D).
    static const int searchSig[]={0xF3,0x0F,-1,-1,-1,-1,-1,-1,0x0F,0x28,-1,0x0F,0x28,-1,-1,-1,0x48,0x83,0xC4,-1,0x5B,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,0x48,0x89};
    static const unsigned char searchOld[]={0x5F},searchNew[]={0x5D};
    extraReady[0]=prepareMaskedPatch(module,searchPatch,searchSig,2,searchOld,searchNew,1);
    logMessage(extraReady[0]?"Instant Search: patch ready":"Instant Search: CT signature missing/ambiguous");

    // Quick Survey: NOP comiss xmm1,xmm0; ja +16 (5 bytes).
    static const int surveySig[]={0x0F,0x2F,-1,0x77,-1,0xF3,0x0F,-1,-1,-1,-1,-1,-1,0x41,0xB0,-1,0x48,0x8B,-1,0x48,0x8B,-1,0xE8,-1,-1,-1,-1,0x8B,0x53,-1,0x0F,0x28};
    static const unsigned char surveyOld[]={0x0F,0x2F,0xC8,0x77,0x16},surveyNew[]={0x90,0x90,0x90,0x90,0x90};
    extraReady[1]=prepareMaskedPatch(module,surveyPatch,surveySig,0,surveyOld,surveyNew,5);
    logMessage(extraReady[1]?"Instant Survey: patch ready":"Instant Survey: CT signature missing/ambiguous");

    // Zombie Radar uses two sites: force sil=1 after original 8 bytes, and
    // bypass the follow-on comparison/conditional jump (same as CT vmap3+8).
    static const int radar1Sig[]={0x40,0x0F,-1,-1,0x48,0x8B,-1,-1,0xE8,-1,-1,-1,-1,0x0F,0xB6};
    static const unsigned char radar1Orig[]={0x40,0x0F,0x93,0xC6,0x48,0x8B,0x0C,0x0F};
    static const unsigned char radar1On[]={0x40,0xB6,0x01}; // mov sil,1
    // Resolve and validate both radar sites before installing either hook.
    // This isolates signature verification from any changes made by the first hook.
    static const int radar3Sig[]={0x38,0x83,-1,-1,-1,-1,0x74,-1,0xB0,-1,0xEB,-1,0x32,0xC0,0x40,0x84};
    // CE's vmap3 pattern intentionally wildcards the cmp displacement. Validate
    // the unique complete instruction sequence instead of demanding a fixed +898.
    // Save the six bytes actually present so the patch can restore them safely.
    static const unsigned char radarSkipNew[]={0xEB,0x06,0x90,0x90,0x90,0x90}; // short jump to site+8
    uintptr_t radarAt=findRadarVerifiedSite(module,radar1Sig,radar1Orig,"site 1");
    uintptr_t radarSkipAt=findRadarWildcardSite(module,radar3Sig,"site 2");
    const CtHookSpec radarHook={"Zombie Radar hook",radar1Orig,sizeof(radar1Orig),8,radar1On,sizeof(radar1On),true,&extraFlags[2]};
    if(!radarAt)logMessage("Zombie Radar: first signature missing or ambiguous");
    bool radarHookOk=radarAt&&installCtHook(module,radarHook,radarAt);
    if(radarAt&&!radarHookOk)logMessage("Zombie Radar: first hook rejected");
    bool radarSkipOk=radarSkipAt!=0;
    if(radarSkipOk){
        radarSkipPatch.at=radarSkipAt;radarSkipPatch.size=sizeof(radarSkipNew);
        memcpy(radarSkipPatch.original,(const void*)radarSkipAt,sizeof(radarSkipNew));
        memcpy(radarSkipPatch.changed,radarSkipNew,sizeof(radarSkipNew));
    }
    if(!radarSkipOk)logMessage("Zombie Radar: second site missing, ambiguous, or bytes differ");
    extraReady[2]=radarHookOk&&radarSkipOk;
    logMessage(extraReady[2]?"Zombie Radar: both sites ready":"Zombie Radar: missing one or both required sites");

    // Fire rate uses TWO weapon stat readers, both must install.
    static const int rate1Sig[]={0x8B,0x87,-1,-1,-1,-1,0xEB,-1,0x80,0x78,0x12};
    static const int rate2Sig[]={0x8B,0x81,-1,-1,-1,-1,0x41,0x89,-1,0xC3,0xCC,0x48,0x89,-1,-1,-1,0x57,0x48,0x83,0xEC,-1,0x48,0x8B};
    static const unsigned char rate1Orig[]={0x8B,0x87,0x8C,0x00,0x00,0x00};
    static const unsigned char rate2Orig[]={0x8B,0x81,0xC4,0x00,0x00,0x00};
    // The game's own stat is read as normal, then ONLY the returned value is replaced (mov eax, 8000.0f). The weapon's stored stat is never
    // written, so switching the toggle off immediately gives back the real fire rate (the old version overwrote the stored value for good).
    static const unsigned char rate1On[]={0xB8,0x00,0x00,0xFA,0x45}; // mov eax, 8000.0f
    static const unsigned char rate2On[]={0xB8,0x00,0x00,0xFA,0x45};
    const CtHookSpec rate1={"Max Fire Rate hook 1",rate1Orig,6,6,rate1On,sizeof(rate1On),true,&extraFlags[3]};
    const CtHookSpec rate2={"Max Fire Rate hook 2",rate2Orig,6,6,rate2On,sizeof(rate2On),true,&extraFlags[3]};
    uintptr_t r1=findMasked(module,rate1Sig),r2=findMasked(module,rate2Sig);
    bool ok1=r1&&installCtHook(module,rate1,r1),ok2=r2&&installCtHook(module,rate2,r2);
    extraReady[3]=ok1&&ok2;
    logMessage(extraReady[3]?"Max Fire Rate: both hooks ready":"Max Fire Rate: one or both hooks unavailable");

    // Silence Shot: set weapon stat at +2D8 to zero before original read.
    static const int silentSig[]={0x8B,0x81,-1,-1,-1,-1,0x41,0x89,-1,0xC3,0xCC,0x40,-1,0x48,0x83,0xEC,-1,0x48,0x8B,-1,-1,0x45,0x33,-1,0x48,0x85,-1,0x49,0x8B,-1,0x41,0x0F,-1,-1,0x4C,0x03,-1,0x4C,0x89,-1,-1,0x48,0x8D,-1,-1,-1,0xE8,-1,-1,-1,-1,0x48,0x8B,-1,0x48,0x89,-1,0x48,0x83,0xC4,-1,0x5B,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x40,-1,0x48,0x83,0xEC,-1,0x48,0x8B,-1,-1,0x45,0x33,-1,0x48,0x85,-1,0x49,0x8B,-1,0x41,0x0F,-1,-1,0x4C,0x03,-1,0x4C,0x89,-1,-1,0x48,0x8D,-1,-1,-1,0xE8,-1,-1,-1,-1,0x0F,0x10}; // full CT Silence shot signature
    static const unsigned char silentOrig[]={0x8B,0x81,0xD8,0x02,0x00,0x00};
    static const unsigned char silentOn[]={0xB8,0x00,0x00,0x00,0x00}; // mov eax,0 after the normal read: only the returned value is changed, nothing is stored in the weapon
    const CtHookSpec silentHook={"Silent Weapons hook",silentOrig,6,6,silentOn,sizeof(silentOn),true,&extraFlags[4]};
    uintptr_t sil=findMasked(module,silentSig);
    if(!sil)logMessage("Silent Weapons: CT signature missing or ambiguous");
    extraReady[4]=sil&&installCtHook(module,silentHook,sil);
    if(sil&&!extraReady[4])logMessage("Silent Weapons: hook rejected after signature match");
    logMessage(extraReady[4]?"Silent Weapons: hook ready":"Silent Weapons: hook unavailable");

    // Construction: overwrite remaining timer with xmm6, then original 7 bytes.
    static const int buildSig[]={0xF3,0x0F,-1,-1,0x0F,0x28,-1,0xEB,-1,0x0F,0x28,-1,0x48,0x8B,-1,-1,-1,0x48,0x8B,-1,-1,-1,0x0F,0x28,-1,-1,-1,0x0F,0x28,-1,-1,-1,0x48,0x83,0xC4,-1,0x5F,0xC3,0xCC};
    static const unsigned char buildOrig[]={0xF3,0x0F,0x5C,0x30,0x0F,0x28,0xC6};
    static const unsigned char buildOn[]={0xF3,0x0F,0x11,0x30}; // movss [rax],xmm6
    const CtHookSpec buildHook={"Instant Construction hook",buildOrig,7,7,buildOn,sizeof(buildOn),false,&extraFlags[5]};
    uintptr_t b=findMasked(module,buildSig);
    extraReady[5]=b&&installCtHook(module,buildHook,b);
    logMessage(extraReady[5]?"Instant Construction: hook ready":"Instant Construction: hook unavailable");

    // Locker stack duplication: skip the decrement but preserve 'sete al'.
    static const int dupeSig[]={0x29,0x08,0x0F,0x94,0xC0};
    static const unsigned char dupeOld[]={0x29,0x08,0x0F,0x94,0xC0};
    static const unsigned char dupeNew[]={0x90,0x90,0x0F,0x94,0xC0};
    extraReady[6]=prepareMaskedPatch(module,dupePatch,dupeSig,0,dupeOld,dupeNew,5);
    logMessage(extraReady[6]?"Duplicate Locker Items: patch ready":"Duplicate Locker Items: signature missing/ambiguous");
}
void updateSevenCtPatches(){
    if(extraReady[0])applyDirectPatch(searchPatch,extraFlags[0].load());
    if(extraReady[1])applyDirectPatch(surveyPatch,extraFlags[1].load());
    if(extraReady[2])applyDirectPatch(radarSkipPatch,extraFlags[2].load());
    if(extraReady[6])applyDirectPatch(dupePatch,extraFlags[6].load());
}

DWORD WINAPI worker(void*){
    uintptr_t module=(uintptr_t)GetModuleHandleW(HSTR("StateOfDecay2-Win64-Shipping.exe").c_str());
    if(!module)return 0;
    running=true;Sleep(3500);
    if(installPresentHook())logMessage("Present hook installed");else logMessage("Present hook installation failed");
    installStaminaHook(module);
    installHealthHook(module);
    installClipHook(module);
    installMeleeWeightHook(module);
    installCtFeatures(module);
    installLiveTimeControls(module);
    installNewDirectPatches(module);
    installSkillHook(module);
    installPlannedHooks(module);
    installMeleeStatsHook(module);
    installSevenCtFeatures(module);
    while(running){
        if(weightPatchReady)applyDirectPatch(weightDirect,unlimitedWeight.load());
        if(bonePatchReady)applyDirectPatch(boneDirect,noBrokenBone.load());
        if(durPatchReady)applyDirectPatch(durDirect,weaponDurability.load());
        updateSevenCtPatches();
        ULONGLONG stop=skillAutoOffAt.load();
        if(stop&&GetTickCount64()>=stop){maxAllSkills=false;skillAutoOffAt=0;}
        applyCharacterTimeDilation();
        // Existing hooks and CT-derived hooks remain installed; toggles default OFF.
        Sleep(40);
    }
    applyDirectPatch(weightDirect,false);applyDirectPatch(boneDirect,false);applyDirectPatch(durDirect,false);
    extraFlags[0]=false;extraFlags[1]=false;extraFlags[2]=false;extraFlags[3]=false;
    extraFlags[4]=false;extraFlags[5]=false;extraFlags[6]=false;
    updateSevenCtPatches();
    maxAllSkills=false;
    for(auto& flag:plannedFlags)flag=false;
    return 0;
}
}
extern "C" __declspec(dllexport) BOOL __cdecl ADREENA_IsRunning(){return running?TRUE:FALSE;}
extern "C" __declspec(dllexport) int __cdecl ADREENA_GetApiVersion(){return 4;}
#ifndef ADREENA_ALLOW_STANDALONE
// The menu only starts when the game was launched by ADREENA_Loader: the loader publishes a session record (its own pid and the
// pid of the game process it started) that this process must match. Development builds define ADREENA_ALLOW_STANDALONE.
static bool launchedByLoader(){
    HANDLE m=OpenFileMappingW(FILE_MAP_READ,FALSE,HSTR("Local\\ADREENA_LoaderSession").c_str());
    if(!m)return false;
    struct Session{DWORD magic,loaderPid,gamePid;};
    const Session* v=(const Session*)MapViewOfFile(m,FILE_MAP_READ,0,0,sizeof(Session));
    bool ok=false;
    if(v){
        if(v->magic==0x4E524441&&v->gamePid==GetCurrentProcessId()){
            HANDLE lp=OpenProcess(SYNCHRONIZE,FALSE,v->loaderPid);
            if(lp){ok=WaitForSingleObject(lp,0)==WAIT_TIMEOUT;CloseHandle(lp);}   // the loader must still be running
        }
        UnmapViewOfFile(v);
    }
    CloseHandle(m);return ok;
}
#endif
BOOL WINAPI DllMain(HMODULE h,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){dllModule=h;DisableThreadLibraryCalls(h);
#ifndef ADREENA_ALLOW_STANDALONE
        if(!launchedByLoader()){logMessage("ADREENA must be started from ADREENA_Loader (game was not launched through it) - not loading");return FALSE;}
#endif
        HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(!thread)return FALSE;CloseHandle(thread);
    }
    return TRUE;
}
