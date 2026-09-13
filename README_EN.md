# SA:MP Launcher

A modern, clean-room **C++ / Win32 / Dear ImGui** launcher for
**San Andreas Multiplayer 0.3.7 / 0.3.DL**.

Fully rewritten without VCL / RAD Studio. Builds with **MinGW-w64** and
**CMake** (Ninja). Renders its own custom UI with Dear ImGui (D3D11 backend).

> This project is a community reimplementation of the classic SA:MP launcher.
> It is **not** affiliated with Rockstar Games, Take-Two Interactive, or the
> SA:MP team. Please read `DISCLAIMER.md` and `LICENSE.txt` before use.

---

## Features

- Full server browser UI (master server list, favorites, hosted list) drawn
  with Dear ImGui — no VCL, no OS chrome
- Async, multithreaded server queries (ping, info, players, rules)
- Custom search + filters: name / IP / mode / map / **language (dropdown
  combobox with live filter, ~100 languages and country variants)**
- Show/hide toggles: full games, empty games, password-protected servers
- Right-click context menu on servers: Connect, Server Properties,
  Copy Server Info, Refresh Server
- Server Properties dialog with Connect button
- Favorites list (save / load), import & export
- RCON console support
- Custom accent-color / theme settings, persisted to `samp_c.ini`
- Nickname history (persisted to `nickhistory.xml`)
- Full High-DPI support, fixed-size window (no resize / fullscreen)
- SAMP window icon on the title bar and taskbar
- Static (no runtime DLLs) or dynamic MinGW build

---

## Requirements to run

- Microsoft Windows (7 SP1 / 8 / 8.1 / 10 / 11), 64-bit recommended
- **A legitimate copy of Grand Theft Auto: San Andreas (2004)** — required,
  the launcher does **not** ship or download the game
- **SA:MP client files**, most importantly **`samp.dll`**, present in the
  folder next to `gta_sa.exe`
  - The launcher **does not bundle** `samp.dll`.
  - Without `samp.dll` you **cannot join servers** — the launcher shows an
    error dialog and aborts the launch.
  - Get SA:MP client files only from trusted sources.

---

## Build (Windows)

### Toolchain

- **CMake** ≥ 3.16 (tested with the CMake shipped with MinGW-w64)
- **MinGW-w64** (GCC / G++) — tested with `C:\MinGW64\mingw64`
- **Ninja** build tool (`ninja.exe` on `PATH`)

### Configure (dynamic build)

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_MAKE_PROGRAM="C:\Path\To\ninja.exe" `
  -DCMAKE_C_COMPILER="C:\MinGW64\mingw64\bin\gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:\MinGW64\mingw64\bin\g++.exe" `
  -DCMAKE_BUILD_TYPE=Release
```

### Build

```powershell
cmake --build build --config Release
```

Output: `build\samp.exe`

### Static build (no MinGW runtime DLLs needed)

Add `-DSAMP_STATIC=ON` when configuring:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_MAKE_PROGRAM="C:\Path\To\ninja.exe" `
  -DCMAKE_C_COMPILER="C:\MinGW64\mingw64\bin\gcc.exe" `
  -DCMAKE_CXX_COMPILER="C:\MinGW64\mingw64\bin\g++.exe" `
  -DCMAKE_BUILD_TYPE=Release `
  -DSAMP_STATIC=ON
cmake --build build --config Release
```

In a static build `samp.exe` links the MinGW runtimes statically, so you get a
single self-contained `samp.exe` with no `libstdc++-6.dll`,
`libgcc_s_seh-1.dll` or `libwinpthread-1.dll` dependencies.

### Also included

Two convenience batch scripts that assume a typical local toolchain layout:

- `dynamic_build.bat` — configure + build into `build\`
- `static_build.bat` — configure + build with `-DSAMP_STATIC=ON`

Both write a log into `logs\`.

---

## Installing & running

1. Build `samp.exe` (see above) **or** download a prebuilt release.
2. Place `samp.exe` anywhere handy.
3. Make sure your **legally owned** copy of GTA: San Andreas is installed and
   that `samp.dll` (SA:MP client) sits next to `gta_sa.exe`.
4. Run `samp.exe`, pick a server, press **Connect**.

> The first launch creates `nickhistory.xml` and `samp_c.ini` next to the
> launcher.

---

## Screenshots

![Screenshot 1](screenshots/launcher-1.png)

![Screenshot 2](screenshots/launcher-2.png)

---

## Repository structure

```text
launcher/          C++ source (main.cpp, core.cpp/h, languages.h)
imgui/             Vendored Dear ImGui (MIT; imgui/LICENSE.txt)
resource/          Icon, manifest, version resource, PNG icons
screenshots/       Interface screenshots
CMakeLists.txt     CMake build script
dynamic_build.bat  One-click dynamic build (MinGW paths preconfigured)
static_build.bat   One-click static build
LICENSE.txt        GNU GPL v3 — full text
DISCLAIMER.md      Trademarks / ownership / third-party rights
```

---

## Credits & licenses

- **This launcher** — written from scratch in C++/Win32/Dear ImGui
  (no VCL). Licensed under **GPL-3.0**, see `LICENSE.txt`.
- Build/drop-in concepts and project layout reference the open-source project
  [`1therealcloud/samp-launcher`](https://github.com/1therealcloud/samp-launcher)
  (GPL-3.0).
- **Dear ImGui** — by Omar Cornut and contributors, MIT license
  (`imgui/LICENSE.txt`).
- **SA:MP** — community multiplayer mod, not affiliated with the game's
  publishers. See `DISCLAIMER.md`.
- **Grand Theft Auto: San Andreas** (2004) — copyright Rockstar North /
  Rockstar Games / Take-Two Interactive. This project is not endorsed or
  sponsored by them. See `DISCLAIMER.md`.

---

## Disclaimer (short)

This software is provided "as is", **without any warranty**. Use it at your
own risk. It is not affiliated with, endorsed by, or sponsored by Rockstar
Games, Take-Two Interactive, or the SA:MP team. See `DISCLAIMER.md` and
`LICENSE.txt` for full legal notices.