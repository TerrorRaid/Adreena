@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  echo Open this batch file from an x64 Native Tools Command Prompt for Visual Studio.
  pause
  exit /b 1
)
where rc >nul 2>nul
if errorlevel 1 (
  echo Windows SDK rc.exe is required.
  goto :failed
)
echo [1/4] Banner resource...
rc /nologo /fo adreena_banner.res adreena_banner.rc
if errorlevel 1 goto :failed
echo [2/4] ADREENA.dll (static runtime)...
cl /nologo /std:c++17 /EHsc /O2 /MT /DNDEBUG /GS /LD /DUNICODE /D_UNICODE adreena_internal.cpp adreena_banner.res /link /PDB:NONE /INCREMENTAL:NO /OUT:ADREENA.dll d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib user32.lib gdi32.lib gdiplus.lib
if errorlevel 1 goto :failed
echo [3/4] Embedding files...
set RCFILE=adreena_loader_res.rc
set MAN=adreena_loader_manifest.txt
>%RCFILE% echo 1 ICON "ADREENA.ico"
>>%RCFILE% echo 100 RCDATA "ADREENA.dll"
>>%RCFILE% echo 101 RCDATA "%MAN%"
>>%RCFILE% echo 102 RCDATA "loader_bg.jpg"
>%MAN% echo 100^|Core\ADREENA.dll^|core
set /a n=200
for %%F in (Presets\*.png) do (
  >>%RCFILE% echo !n! RCDATA "Presets\\%%~nxF"
  >>%MAN% echo !n!^|Presets\%%~nxF^|asset
  set /a n+=1
)
rc /nologo /fo adreena_loader.res %RCFILE%
if errorlevel 1 goto :failed
echo [4/4] ADREENA_Loader.exe...
cl /nologo /std:c++17 /EHsc /O2 /MT /DNDEBUG /GS /DUNICODE /D_UNICODE adreena_loader.cpp adreena_loader.res /link /SUBSYSTEM:WINDOWS /PDB:NONE /INCREMENTAL:NO /OUT:ADREENA_Loader.exe user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib gdiplus.lib shlwapi.lib advapi32.lib bcrypt.lib
if errorlevel 1 goto :failed
echo.
del /q *.obj *.exp *.lib *.ilk *.pdb %RCFILE% %MAN% >nul 2>nul
echo Build complete: ADREENA_Loader.exe (single file, nothing else needed).
exit /b 0
:failed
echo Build failed. Copy the compiler errors for troubleshooting.
exit /b 1
