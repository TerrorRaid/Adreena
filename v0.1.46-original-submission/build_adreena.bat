@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  echo Open this batch file from an x64 Native Tools Command Prompt for Visual Studio.
  pause
  exit /b 1
)
echo Compiling embedded ADREENA banner resource...
where rc >nul 2>nul
if errorlevel 1 (
  echo Windows SDK rc.exe is required to embed the header.
  goto :failed
)
rc /nologo /fo adreena_banner.res adreena_banner.rc
if errorlevel 1 goto :failed
echo Building ADREENA.dll...
cl /nologo /std:c++17 /EHsc /O2 /DNDEBUG /GS /LD /DADREENA_ALLOW_STANDALONE /DUNICODE /D_UNICODE adreena_internal.cpp adreena_banner.res /link /PDB:NONE /INCREMENTAL:NO /OUT:ADREENA.dll d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib user32.lib gdi32.lib gdiplus.lib
if errorlevel 1 goto :failed
echo Building ADREENA launcher icon...
where rc >nul 2>nul
if errorlevel 1 (
  echo Windows Resource Compiler rc.exe not found. Use the Visual Studio x64 Native Tools Command Prompt with Windows SDK installed.
  goto :failed
)
rc /nologo /fo adreena_icon.res adreena_icon.rc
if errorlevel 1 goto :failed
echo Building ADREENA_Launcher.exe...
cl /nologo /std:c++17 /EHsc /O2 /DNDEBUG /GS /DUNICODE /D_UNICODE adreena_launcher.cpp adreena_icon.res /link /PDB:NONE /INCREMENTAL:NO /OUT:ADREENA_Launcher.exe shell32.lib user32.lib
if errorlevel 1 goto :failed
echo.
del /q *.obj *.exp *.lib *.ilk *.pdb >nul 2>nul
echo Build complete. ADREENA_Launcher.exe and ADREENA.dll must stay together.
exit /b 0
:failed
echo Build failed. Copy the compiler errors for troubleshooting.
exit /b 1
