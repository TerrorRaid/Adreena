# ADREENA — State of Decay 2 single-player mod menu (source code)

ADREENA is an in-game menu for **State of Decay 2** (Steam, single-player). It is made of two programs:

| Program | Built from | What it is |
|---|---|---|
| `ADREENA_Loader.exe` | `adreena_loader.cpp` (+ embedded resources) | The file players download. A single, self-contained Windows program that prepares `Documents\ADREENA`, unpacks the menu DLL and image presets it carries inside itself, starts the game through Steam and loads the menu into it. |
| `ADREENA.dll` | `adreena_internal.cpp` (+ banner resource) | The menu itself (Direct3D 11 overlay drawn over the game). It is **built into** `ADREENA_Loader.exe`; it is not distributed separately. |

There is no third-party code or library in this project. Everything is built with Microsoft's C++ compiler against libraries that come with the Windows SDK.

---

## Repository layout and versions

Each version is a complete, independent source folder. Build a version from inside its own folder.

| Folder | Version | Status |
|---|---|---|
| `v0.1.46-original-submission/` | v0.1.46 | Source of the version originally submitted to Nexus Mods. |
| `v0.1.48-fixed/` | v0.1.48 | Updated, fixed version; not yet uploaded to Nexus Mods (see *Changes between the two versions* below). |
| `v0.1.46-to-v0.1.48.diff` | — | Exact line-by-line difference between the two source folders. |

Files in each version folder:

| File | Used by | Purpose |
|---|---|---|
| `adreena_internal.cpp` | both scripts | Source of `ADREENA.dll` (the menu). |
| `adreena_loader.cpp` | `build_loader.bat` | Source of `ADREENA_Loader.exe`. |
| `adreena_launcher.cpp` | `build_adreena.bat` | Source of `ADREENA_Launcher.exe`, a small console launcher used only for development builds (not distributed). |
| `build_loader.bat` | — | **Release build.** Produces the distributable `ADREENA_Loader.exe`. |
| `build_adreena.bat` | — | Development build (see below). |
| `adreena_banner.rc` | both scripts | Resource script: embeds `ADREENA_Banner.bmp` (the menu's default header image) into `ADREENA.dll`. |
| `adreena_icon.rc` | `build_adreena.bat` | Resource script: icon for the development launcher. |
| `ADREENA.ico` | both scripts | Program icon. |
| `ADREENA_Banner.bmp` | both scripts | Default menu header image (680×94). |
| `loader_bg.jpg` | `build_loader.bat` | Background artwork of the loader window. |
| `Presets\*.png` (8 files) | `build_loader.bat` | Menu theme preset images, embedded into the loader and unpacked to `Documents\ADREENA\Presets`. |

---

## Requirements

**Operating system:** Windows 10 or Windows 11, 64-bit.

**Compiler:** Microsoft Visual C++ (MSVC), **x64** toolset, with C++17 support. Both scripts compile with `cl /std:c++17` and must be run from an **x64** Visual Studio command prompt (State of Decay 2 is a 64-bit game, so a 32-bit build will not work).

> **Build environment:** Visual Studio 2026 Community (18.11.2), MSVC 19.51.36260 (x64), Windows SDK 10.0.26100.0.

**Visual Studio components** (any edition, including the free Community edition or *Build Tools for Visual Studio*), install the workload **Desktop development with C++**, which includes:
- *MSVC … C++ x64/x86 build tools* (the compiler `cl.exe` and linker)
- *Windows 11 SDK* or *Windows 10 SDK* (provides the headers and import libraries listed below, and the resource compiler `rc.exe`)

**Libraries** — all are import libraries that ship with the Windows SDK; nothing needs to be downloaded separately:

| Program | Libraries linked |
|---|---|
| `ADREENA.dll` | `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`, `d2d1.lib`, `dwrite.lib`, `user32.lib`, `gdi32.lib`, `gdiplus.lib` |
| `ADREENA_Loader.exe` | `user32.lib`, `gdi32.lib`, `shell32.lib`, `ole32.lib`, `uuid.lib`, `gdiplus.lib`, `shlwapi.lib`, `advapi32.lib`, `bcrypt.lib` |
| `ADREENA_Launcher.exe` (development only) | `shell32.lib`, `user32.lib` |

**Runtime dependencies of the release build:** none beyond Windows itself. The release DLL and the loader are linked with the static C++ runtime (`/MT`), so the Visual C++ Redistributable is **not** required. At runtime the menu uses system DLLs that are part of Windows 10/11 (`d3d11.dll`, `dxgi.dll`, `d3dcompiler_47.dll`, `d2d1.dll`, `dwrite.dll`, `gdiplus.dll`). To *run* it you also need State of Decay 2 installed through Steam.

---

## Setting up the build environment

1. Download and install **Visual Studio** (Community edition or *Build Tools for Visual Studio*) from Microsoft.
2. In the Visual Studio Installer, select the **Desktop development with C++** workload. Make sure *MSVC … x64/x86 build tools* and a *Windows 11 SDK* (or *Windows 10 SDK*) are ticked in the installation details, then install.
3. Get the source: clone this repository (`git clone <repository URL>`) or download it as a ZIP and extract it.
4. Open the Start menu and launch **x64 Native Tools Command Prompt for VS** (the exact name includes your Visual Studio version, e.g. *x64 Native Tools Command Prompt for VS 2022*). Do **not** use the "x86" or "x64_x86 Cross" prompts.
5. Check the tools are available in that prompt:
   ```
   where cl
   where rc
   cl
   ```
   `where` must print a path for both. The `cl` banner must end in **"for x64"**.

---

## Building

### Release build — the distributable `ADREENA_Loader.exe`

In the x64 Native Tools Command Prompt:

```
cd /d "<path to repository>\v0.1.48-fixed"
build_loader.bat
```

(Use `v0.1.46-original-submission` instead to build the original version.)

The script prints `[1/4]` … `[4/4]` and ends with:

```
Build complete: ADREENA_Loader.exe (single file, nothing else needed).
```

The finished program is `ADREENA_Loader.exe` in that same folder. It is the only file a player needs.

### Development build (optional)

```
cd /d "<path to repository>\v0.1.48-fixed"
build_adreena.bat
```

Ends with `Build complete. ADREENA_Launcher.exe and ADREENA.dll must stay together.`

---

## What each build script does

### `build_loader.bat` (release)

1. Changes to the script's own folder, and stops with a message if `cl` or `rc` cannot be found (i.e. not run from a Visual Studio command prompt).
2. **[1/4]** `rc` compiles `adreena_banner.rc` → `adreena_banner.res` (embeds `ADREENA_Banner.bmp`).
3. **[2/4]** `cl` compiles `adreena_internal.cpp` + `adreena_banner.res` into **`ADREENA.dll`** with:
   `/std:c++17 /EHsc /O2 /MT /DNDEBUG /GS /LD /DUNICODE /D_UNICODE` and linker options `/PDB:NONE /INCREMENTAL:NO` (no debug symbols).
4. **[3/4]** Generates two temporary files:
   - `adreena_loader_manifest.txt` — the list of embedded files and where the loader should unpack them;
   - `adreena_loader_res.rc` — a resource script that embeds the icon, the freshly built `ADREENA.dll` (resource 100), that manifest (101), `loader_bg.jpg` (102) and every `Presets\*.png` (200, 201, …, in alphabetical order);
   then compiles it with `rc` → `adreena_loader.res`.
5. **[4/4]** `cl` compiles `adreena_loader.cpp` + `adreena_loader.res` into **`ADREENA_Loader.exe`** with
   `/std:c++17 /EHsc /O2 /MT /DNDEBUG /GS /DUNICODE /D_UNICODE` and `/SUBSYSTEM:WINDOWS /PDB:NONE /INCREMENTAL:NO`.
6. Deletes intermediate files (`*.obj *.exp *.lib *.ilk *.pdb` and the two generated files).

Because the DLL is embedded in step 5, **any change to `adreena_internal.cpp` requires running `build_loader.bat` again** to get an updated loader.

### `build_adreena.bat` (development only — not used for releases)

1. Same `cl` / `rc` checks.
2. Compiles `adreena_banner.rc` → `adreena_banner.res`.
3. Builds `ADREENA.dll` with `/DADREENA_ALLOW_STANDALONE` and **without** `/MT` (dynamic C++ runtime: needs the Visual C++ 2015–2022 Redistributable, x64).
   `ADREENA_ALLOW_STANDALONE` disables the release check described under *Launch gate* below, so a developer can load the DLL without the loader.
4. Compiles `adreena_icon.rc` → `adreena_icon.res` and builds `ADREENA_Launcher.exe` (console program) from `adreena_launcher.cpp`.
5. Deletes `*.obj *.exp *.lib *.ilk *.pdb`.

---

## Output files

After `build_loader.bat`, in the version folder:

| File | Kind | Notes |
|---|---|---|
| **`ADREENA_Loader.exe`** | **final output** | The distributable. Self-contained single file. |
| `ADREENA.dll` | intermediate | Release DLL; an identical copy is embedded in the loader. |
| `adreena_banner.res`, `adreena_loader.res` | intermediate | Compiled resources. |

After `build_adreena.bat`: `ADREENA.dll` (development variant), `ADREENA_Launcher.exe`, `adreena_banner.res`, `adreena_icon.res`.

**Note:** both scripts write `ADREENA.dll` to the same place, so the last script you ran decides which variant is in the folder. `build_loader.bat` always rebuilds the release DLL before embedding it.

Where the loader puts files on a player's PC when it runs (`%USERPROFILE%\Documents\ADREENA`, resolved through the Windows "Documents" known folder):

```
Documents\ADREENA\
  Core\ADREENA.dll        menu DLL, unpacked from the loader (replaced at loader start if it differs from the embedded copy)
  Presets\*.png           theme presets, unpacked from the loader (only if missing)
  Profiles\               saved menu profiles (*.json) and Bookmarks.json
  Banners\                optional user header images
  Logs\                   ADREENA_loader.log, ADREENA_debug.log
  Cache\                  loader settings and one-time import marker
```

---

## Manual build without the batch files

The batch files are the verified build. The commands below are exactly what `build_loader.bat` runs; type them in the x64 Native Tools Command Prompt from inside the version folder.

```
rc /nologo /fo adreena_banner.res adreena_banner.rc

cl /nologo /std:c++17 /EHsc /O2 /MT /DNDEBUG /GS /LD /DUNICODE /D_UNICODE adreena_internal.cpp adreena_banner.res /link /PDB:NONE /INCREMENTAL:NO /OUT:ADREENA.dll d3d11.lib dxgi.lib d3dcompiler.lib d2d1.lib dwrite.lib user32.lib gdi32.lib gdiplus.lib
```

Create `adreena_loader_manifest.txt` with this content:

```
100|Core\ADREENA.dll|core
200|Presets\crimson_moon.png|asset
201|Presets\moonlit_sti.png|asset
202|Presets\schoolhouse.png|asset
203|Presets\skulls.png|asset
204|Presets\surreal_collage.png|asset
205|Presets\tagged.png|asset
206|Presets\undead_labs.png|asset
207|Presets\underwater_temple.png|asset
```

Create `adreena_loader_res.rc` with this content:

```
1 ICON "ADREENA.ico"
100 RCDATA "ADREENA.dll"
101 RCDATA "adreena_loader_manifest.txt"
102 RCDATA "loader_bg.jpg"
200 RCDATA "Presets\\crimson_moon.png"
201 RCDATA "Presets\\moonlit_sti.png"
202 RCDATA "Presets\\schoolhouse.png"
203 RCDATA "Presets\\skulls.png"
204 RCDATA "Presets\\surreal_collage.png"
205 RCDATA "Presets\\tagged.png"
206 RCDATA "Presets\\undead_labs.png"
207 RCDATA "Presets\\underwater_temple.png"
```

Then:

```
rc /nologo /fo adreena_loader.res adreena_loader_res.rc

cl /nologo /std:c++17 /EHsc /O2 /MT /DNDEBUG /GS /DUNICODE /D_UNICODE adreena_loader.cpp adreena_loader.res /link /SUBSYSTEM:WINDOWS /PDB:NONE /INCREMENTAL:NO /OUT:ADREENA_Loader.exe user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib gdiplus.lib shlwapi.lib advapi32.lib bcrypt.lib
```

### Visual Studio IDE (optional)

There is no `.sln`/`.vcxproj` in this repository; the project has only been built with the command lines above. To reproduce them in the IDE, create two empty C++ projects and set these properties (configuration **Release**, platform **x64**) so they match the command lines:

| Setting | `ADREENA.dll` project | `ADREENA_Loader.exe` project |
|---|---|---|
| Source files | `adreena_internal.cpp`, `adreena_banner.rc` | `adreena_loader.cpp`, `adreena_loader_res.rc` (from above, with the manifest file beside it) |
| General → Configuration Type | Dynamic Library (.dll) | Application (.exe) |
| C/C++ → Language → C++ Language Standard | ISO C++17 (`/std:c++17`) | ISO C++17 |
| C/C++ → Language → Conformance mode | No (the scripts do not pass `/permissive-`) | No |
| C/C++ → General → SDL checks | No (the scripts do not pass `/sdl`) | No |
| C/C++ → Code Generation → Runtime Library | Multi-threaded (`/MT`) | Multi-threaded (`/MT`) |
| C/C++ → Preprocessor Definitions | `NDEBUG;UNICODE;_UNICODE` | `NDEBUG;UNICODE;_UNICODE` |
| C/C++ → Precompiled Headers | Not Using | Not Using |
| Linker → System → SubSystem | (default) | Windows (`/SUBSYSTEM:WINDOWS`) |
| Linker → Debugging → Generate Debug Info | No | No |
| Linker → Input → Additional Dependencies | libraries listed under *Requirements* | libraries listed under *Requirements* |

Build the DLL project first and copy its `ADREENA.dll` next to `adreena_loader_res.rc` before building the loader project, because the loader embeds it.

---

## What the programs do (for security review)

**ADREENA_Loader.exe**
- Creates `Documents\ADREENA` and its subfolders and unpacks its embedded files. At every start it compares `Core\ADREENA.dll` byte-for-byte with the embedded copy and replaces it if it differs; presets are written only if missing. It never overwrites user data (profiles, banners, bookmarks); on first run it copies (never moves) profiles/banners/bookmarks from an older install found beside the loader, without overwriting existing files.
- Reads one registry value: `HKEY_CURRENT_USER\Software\Valve\Steam\SteamPath` (read-only, to check Steam is installed). It writes nothing to the registry.
- Starts the game with the Steam URL `steam://rungameid/495420` (State of Decay 2).
- Right before loading the menu, compares the SHA-256 hash (Windows BCrypt) of `Core\ADREENA.dll` with the copy embedded in the loader. If they differ, it refuses to launch and asks the user to reopen the loader (which repairs the file as described above).
- Loads `ADREENA.dll` into the game process (`StateOfDecay2-Win64-Shipping.exe`) using the standard `CreateRemoteThread` + `LoadLibraryW` method.
- Records a session in a named shared-memory block (`Local\ADREENA_LoaderSession`) holding the loader's and the game's process IDs (see *Launch gate*). After the game closes, it returns to the "ready" state.
- The only other things it opens are the `Documents\ADREENA` folder and its log files, when the user clicks the matching menu items.

**ADREENA.dll**
- Hooks the game's Direct3D 11 `Present` call to draw the menu, and subclasses the game window to receive keyboard/mouse input while the menu is open (PgDn toggles it).
- Implements menu features by patching code and data **inside the game process only** and by calling the game's own Unreal Engine functions. It does not modify any game files on disk.
- Writes only inside `Documents\ADREENA` (logs, profiles, bookmarks).

**Both programs** contain no networking code (no sockets, HTTP or download functions). The only external call is the `steam://` URL handed to Windows.

**Launch gate.** The release DLL refuses to start unless the game process was launched by a running `ADREENA_Loader.exe` (it checks the session record above and that the loader process is still alive). Development builds defined with `ADREENA_ALLOW_STANDALONE` skip this check.

**Hidden strings.** A few names are stored lightly obfuscated (XOR with a position-based key, `ObStr`/`HSTR` near the top of both `.cpp` files) so they do not appear as plain text in the binaries: the game's executable name, `ADREENA.dll`, the Steam URL, the Steam registry key/value and the session name. This only hides them from a quick `strings` scan; it is **not** encryption and the plain text is visible in the source at every `HSTR("…")` use.

**Reproducibility.** MSVC embeds build timestamps in the executable header, so a rebuilt `ADREENA_Loader.exe` will not be byte-for-byte identical to the distributed one even from identical source.

---

## Changes between the two versions

`v0.1.46-original-submission` → `v0.1.48-fixed` (full diff: `v0.1.46-to-v0.1.48.diff`). Only `adreena_internal.cpp` and `adreena_loader.cpp` differ.

`adreena_internal.cpp` (menu DLL):
1. **Black-screen fix (display mode checks).** v0.1.46 asked the display driver for the current display mode every 2 seconds while the menu was open (to size the menu). v0.1.48 only asks when the window or back-buffer size actually changes.
2. **Near Clip toggle.** Turning the Near Clip option off now sends `r.SetNearClipPlane 10` (Unreal Engine's default) instead of `0`.
3. **Graphics state restore.** After drawing the menu, the game's output-merger state is restored including its unordered-access views (`OMGetRenderTargetsAndUnorderedAccessViews` / `OMSetRenderTargetsAndUnorderedAccessViews`), not only its render targets.
4. Version string `v0.1.46` → `v0.1.48`.

`adreena_loader.cpp` (loader):
1. Version string `v0.1.46` → `v0.1.48` and a new v0.1.48 entry in the in-app change log.
