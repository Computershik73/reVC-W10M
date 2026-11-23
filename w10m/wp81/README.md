# reVC on Windows Phone 8.1

The game builds for Windows Phone 8.1 from the same sources as the Windows 10 Mobile package,
as `w10m/work/wp81/packages/reVC81_<version>_ARM.appx`: one program, `reVC.exe`, with the C
runtime, OpenAL Soft and mpg123 linked into it. The trial below asked a phone first whether that could
work, and an HTC One M8 with 8.1 said yes.

This port is the branch `wp81`: the Windows 10 Mobile branch `w10m` with it. `w10m` has none of
it, neither this folder nor the sources' `REVC_WP81` parts; changes to `w10m` are merged into
`wp81`, not the other way round.

## How it is built

```bash
python w10m/wp81/build_game.py
```

A build of its own: the Windows 10 Mobile build (`w10m/CMakeLists.txt`, `cmake/`,
`tools/build.py`, `w10m/work/build`, `w10m/builds`) is neither used nor changed by it. The two
share the sources alone, and where those differ for 8.1 they say `REVC_WP81` (`app/App.cpp`,
`app/GameFiles.h`, `openal/xaudio2_backend.cpp`). `build_game.py` configures this folder's own
[`CMakeLists.txt`](CMakeLists.txt) with [`toolchain-wp81.cmake`](toolchain-wp81.cmake) into
`w10m/work/wp81/game/build-<arch>`; OpenAL Soft and mpg123 are the sources `tools/build.py
sources patch` fetches and patches into `w10m/work/src` (those steps build nothing), as
`vendor/librw` is patched by it:

- today's MSVC (v143) and SDK headers, as for Windows 10 Mobile, but the C runtime linked in
  (`/MT`): 8.1 has no `ucrtbase.dll`, and the package carries no DLLs at all; OpenAL Soft and
  mpg123 are static libraries there, with one C runtime among them all;
- nothing linked against but that runtime and the Windows Phone 8.1 kit's import libraries
  (`/NODEFAULTLIB`). What 8.1 lacks is an unresolved external at the link, and
  [`stubs.cpp`](stubs.cpp) (the C runtime's console, command line and environment, C++/WinRT's
  `LoadLibraryW`) and [`stubs_game.cpp`](stubs_game.cpp) have all of it: the current directory,
  semaphores by their old names, `LocalAlloc`, anonymous pipes, `CopyFileW`,
  `AreFileApisANSI`, and the thread pool behind C++/WinRT's `resume_background`. Where the kit
  has no entry the phone may still have the function: those stubs look in `kernelbase.dll`
  and `kernel32.dll` first, the current directory in `ntdll.dll` after that, and do it
  themselves only where the phone has nothing. The log's third line says what was found where;
- `_WIN32_WINNT` is 8.1's, so XAudio2's header is 2.8's, and the sound backend loads
  `XAudio2_8.dll` first;
- `REVC_WP81`, for the shell's few differences, and an 8.1 manifest (Identity `reVC.WP81`, so
  it is an app apart from the Windows 10 Mobile one), tiles at 8.1's sizes, a portrait splash
  screen; packed by the Windows 8.1 SDK's `makeappx`, which knows 8.1's manifest.

`reVC.exe` imports `d3d11.dll`, `oleaut32.dll` and 22 api-sets, all through the kit
(`w10m/work/wp81/game/imports-arm.txt`). `d3dcompiler_47.dll` is looked for only when the
bench asks for it ([`d3dcompile.cpp`](d3dcompile.cpp)).

## What differs on the phone

- **The game's folder.** 8.1 has no `PickSingleFolderAsync`: the folder picker takes the phone
  from the app (`PickFolderAndContinue`), the app is suspended, and the folder comes back as an
  activation of its own (`App::OnActivated`, then `GameFiles::Take`). If the phone ended the
  app meanwhile, that activation is its start, with the files screen and not the game. The
  copy into the app is the Windows 10 Mobile one. `TryGetItemAsync` is Windows 10's too: the
  check for `models\gta3.img` and `data\gta_vc.dat` asks for each and catches what is not there.
- **The Back key** is `Windows.Phone.UI.Input.HardwareButtons` ([`BackKey81.h`](BackKey81.h):
  the SDK's C++/WinRT has no projection of that phone class, so its two interfaces are declared
  from the 8.1 kit's ABI header). As on Windows 10 Mobile: Escape in the game, out of the app on
  the files screen.
- **Full screen**: 8.1 has none. The game's window goes under the status bar (hidden) and the
  navigation bar (`SetDesiredBoundsMode(UseCoreWindow)`).
- **No gamepad**: `Windows.Gaming.Input` is Windows 10's. The pad on the screen and a keyboard.
- **The phone in the log**: its maker, model and firmware (`EasClientDeviceInformation`; 8.1
  has no `AnalyticsInfo`), the memory an app may have, where the missing functions came from.
- **The log, sent from the phone**: 8.1 has no Device Portal. *Send the log* on the files screen
  and in the game's *About the Port* (Options) opens the phone's share screen with `revc.log`
  and `revc.prev.log` (the run before, which is the one that failed when the app is opened
  again after a crash) as files, and their ends as text.
- **Memory**: a phone with 512 MB gives an app about 185 MB, and the game needs about 220 MB.
  The files screen says so on such a phone; 1 GB or more is what the game is for.
- **The renderer** (`rwd3d11/device.cpp`, `REVC_WP81`), from a Lumia 920:
  - the depth buffer is not told away after each frame (`DiscardView`). With it, an Adreno 225
    on 8.10.12393 drew the radar empty and smeared the picture behind a fast car in every other
    frame (the radar's round mask is drawn into the depth buffer). The picture before a frame
    is still told away: keeping it as well (`REVC_D3D_KEEP=1`) cost a third of the frames.
    `REVC_D3D_DISCARD_DEPTH=1` discards the depth buffer again;
  - what differs between the draws of geometry goes with the vertices (the instance stream)
    only on an Adreno of the 300s or later; on any other GPU by constant buffers. The trial's
    bench, a draw with a matrix of its own with the GPU's part: Adreno 330 (HTC One M8) 8.6
    microseconds with the vertices and 28.2 by a constant buffer, Adreno 225 (Lumia 920, whose
    driver calls it "Software Adapter") 92.6 and 29.8. `REVC_D3D_INSTANCE=1` or `0` says
    otherwise. The log says what the GPU is called (`d3d: the GPU is ...`).

## What has been checked, and what not

- **The link** (1.0.1121.1616, ARM and x86, from this folder's own CMakeLists.txt): the whole
  game, OpenAL Soft and mpg123 against the 8.1 kit alone, with the stubs. The ARM program is
  marked for subsystem 6.02 and an app container, and imports what the kit has, nothing else.
- **The Windows 10 Mobile build** is as committed in its build files; with the sources as they
  are now (8.1's differences under `REVC_WP81`) it was configured from scratch into a folder of
  its own and built as before: `reVC.exe`, `OpenAL32.dll`, `libmpg123.dll`, of the same sizes.
- **On the Windows 10 Mobile emulator** (15254, 1 GB, x86 build, WARP for a GPU; it runs 8.1
  packages, so this checks the app and not 8.1) with test packages:
  - a first start: the files screen, the Back key taken (through the 8.1 class), the log;
  - a folder handed in as the picker would (`REVC_PICKED`), first a tiny fake game: the check,
    the copy leaving out `movies` and `.exe`, `game.part` becoming `game`, reVC's files over it,
    the game started (and stopped by the fake files, as it should);
  - the PC copy of the game, 1428 files and 1647 MB copied in 25 to 28 s, the game's menu up
    30 s after the start (58 MB, 62 frames a second), then with `REVC_KEYS` a new game, its
    first cutscene and the drive: 29 to 30 frames a second, 223 to 255 MB of the 390 MB the
    emulator gives an app. That is why a 512 MB phone (185 MB) will not do. The same again
    (1.0.1120.1420) once the build had become one of its own: the copy, the menu, a new game
    and the drive, up to 259 MB (the frame rate of a WARP emulator just started is not a
    phone's: 17 to 23).
- **On Windows Phone 8.1 itself**, a Lumia 920 (Snapdragon S4 Plus, Adreno 225, 1 GB,
  768x1280, 8.10.12393.890, 2026-11-21), on USB with the 8.1 SDK's `AppDeployCmd` and `ISETool`
  (test packages with `REVC_LOG_MIRROR`):
  - the functions the kit lacks all came from the phone's own `kernelbase.dll`;
  - the folder picker's real way back: the player picked `Documents\Grand Theft Auto Vice
    City`, the app was suspended, came back through `OnActivated` and copied 1417 files, 1463 MB,
    in 212 s;
  - the menu 8 s after the game's start, a new game and the drive by itself (`REVC_KEYS` with
    `d`), at 1043x626 (the 626 lines a level-9 GPU gets); suspended and resumed twice; up to
    302 MB of the 390 MB an app may have there;
  - frames a second driving, by draws a frame (`w10m/work/elite_windows.py`): first 21.8 at 300
    draws and 15.4 at 450, with the instance stream; 28.7 and 26.9 by constant buffers, which is
    what 1.0.1121.1616 chooses on that phone by itself (28.9 and 26.9);
  - the radar's and the fast car's flicker, gone with the depth buffer kept (the player's eyes
    on 1.0.1121.1549, after a video of the one before).
- **Not checked yet**: the Back key, a turn of the phone, sound, the share screen and the
  status and navigation bars on 8.1 (the Lumia 920 has its keys below the screen); another 8.1
  phone with the game (an Adreno 305 phone, a Lumia 520 to 630, would take the instance stream).

## Settings for a test package

`build_game.py --test NAME=VALUE --test-src DIR` makes a test package: `testenv\env.NAME=value`
files read as LocalState's are, and a game folder of its own (`testsrc`). For the emulator, where
nobody taps the folder picker and what ISETool uploads into LocalState is not the app's any more.

| Setting | |
|---|---|
| `REVC_PICKED=testsrc` | as if that folder (LocalState's, or else the package's) had come back from the folder picker |
| `REVC_LOG_MIRROR=1` | the log in TempState as well: ISETool copies nothing of LocalState while the game holds a file of it open, but takes TempState alone (`ts appxfolder:Temp`) |

The Windows 10 Mobile ones (`w10m/README.md`) work the same where 8.1 has what they need.

## The trial

Would what reVC's Windows 10 Mobile port is made of run on Windows Phone 8.1? This small app
(`trial.cpp`) asked a phone before the game was built for it.

The port is built with today's compiler (MSVC v143 for 32-bit ARM) and its C runtime, which
Windows 10 has and 8.1 does not. The trial is built the same way, with that C runtime linked
into it (`/MT`) and against nothing but the Windows Phone 8.1 kit's import libraries, so a
call 8.1 does not have is an unresolved external at the link, not an app that will not start.
On ARM there were 15 such calls (the C runtime's console, command line and environment block,
`LocalFree`, and C++/WinRT's `LoadLibraryW`); [`stubs.cpp`](stubs.cpp) has them.

On the phone it goes through five steps and shows what it finds: a summary at the top, the
whole log below it, and LocalState\trial.log.

1. The phone: its model, the memory it gives an app, its processors and screen.
2. The C and C++ runtime: formatting, C++ and structured exceptions, threads, a lock and a
   condition variable, 88 MB of heap, a file in LocalState, time.
3. A swap chain made for composition in a XAML `SwapChainPanel`, from a code-only C++/WinRT
   app, as the port's shell has it: frames a second of a cleared picture, with and without
   waiting for the screen. The strip in the middle changes colour while it runs.
4. [`../bench/d3dbench.cpp`](../bench/d3dbench.cpp), unchanged, as `REVC_BENCH=1` runs it on
   Windows 10 Mobile: what this Direct3D has, what a draw costs with one thing changed before
   it, and whether calls can be recorded on one thread for another (a deferred context). The
   summary sets the Lumia 650's numbers beside the phone's.
5. The same bench's `fill`: what a layer of pixels and a grid of triangles cost the GPU.

At the end the strip is green (no problems) or red, and the summary says "done". The button
at the bottom shares the summary and the whole log (mail, messaging, OneNote...).

### What has been checked, and what not

- The link (1.0.1118.115): ARM and x86 link against the 8.1 kit alone, with the stubs; the
  ARM program imports nothing but the kit's api-sets, `d3d11.dll`, `oleaut32.dll` (by
  ordinal) and `api-ms-win-core-winrt-l1-1-0.dll`. x86's C++ library wants eight thread-pool
  wrappers more, which ARM's does not.
- The app, on the Windows 10 Mobile emulator (15254, 512 MB, x86, WARP for a GPU), which runs
  8.1 packages: all five steps in 7.5 seconds, no problems; 185 MB for an app, 64 frames a
  second in the panel with vsync. Its bench numbers are WARP's, not a phone's.
- On Windows Phone 8.1 itself (1.0.1118.115, 2026-11-20, a player's phone): an HTC One M8 for
  Windows (HTC6995LVW, Snapdragon 801, Adreno 330, 2 GB, 1080x1920, firmware 2061.30.10602.605).
  All five steps in 10.3 seconds, no problems: the C runtime of 2022, C++/WinRT's XAML, a
  swap chain for composition (55.7 frames a second with vsync), Direct3D 11 at level 9_3, a
  deferred context, 825 MB for an app. Its log is `w10m/work/wp81/htc-one-m8-wp81-trial-115.log`.
  Beside the Windows 10 Mobile phones' rows of the same bench (`w10m/work/lumia-bench3.log`,
  `lumia-fill2.log`, `elite2-1550-level11-bench.log`, `elite-fill.log`; microseconds a draw
  with what the GPU had left to do, milliseconds for a further layer of 1280x720):

  | | HTC One M8, 8.1, 9_3 | Lumia 650, 9_3 | HP Elite x3, 11_0 |
  |---|---|---|---|
  | a draw, nothing changed (the bench's second row) | 5.4 | 19.1 | 3.1 |
  | a matrix in a constant buffer of its own | 28.2 | 86.7 | 9.1 |
  | the matrix with the vertices, from an instance stream | 8.6 | 32.2 | |
  | vertices further along a buffer, nothing overwritten | 6.7 | 25.0 | 5.0 |
  | the buffer discarded for each draw | 77.9 | 199.0 | 134.5 |
  | the other shaders | 38.5 | 156.9 | 17.7 |
  | a layer: one colour | 0.39 | 2.03 | 0.43 |
  | a layer: the renderer's shader, no alpha test | 0.51 | 2.72 | 0.86 |
  | a layer: from an atlas, with the alpha test | 0.76 | 5.20 | 1.23 |
  | 100 thousand triangles over the picture, the renderer's shader | 3.10 | 19.61 | 8.23 |
  | the same with eight lights, none lit | 15.36 | 58.37 | 14.66 |

  What the port does on a level-9 GPU (the instance stream, buffers written further along,
  no lights loop) holds on 8.1's driver as on the Lumia's. The phone is three to four times
  the Lumia 650 per draw and five to seven times per pixel; the Elite x3's fill rows were
  measured on a phone that may have been warm. The bench's first row (153 microseconds a
  draw) paid for what the GPU still had from the step before; the summary of 1.0.1118.115
  showed it, from 1.0.1120.395 on it shows the second row, as the Lumia's 19 beside it is.
  The phone's clock said 2014: the package installed and ran all the same.

### Installing it

The phone must be developer-unlocked (Windows Phone Developer Registration 8.1, or the
interop unlocks that replaced it) and on USB. With the Windows Phone 8.1 SDK:

```bash
"/c/Program Files (x86)/Microsoft SDKs/Windows Phone/v8.1/Tools/AppDeploy/AppDeployCmd.exe" /installlaunch reVC81Trial_<version>_ARM.appx /targetdevice:de
```

or the same SDK's *Windows Phone Application Deployment 8.1* window. The log can also be
taken off the phone without sharing it:

```bash
"/c/Program Files (x86)/Microsoft SDKs/Windows Phone/v8.1/Tools/IsolatedStorageExplorerTool/ISETool.exe" ts de 6f0b9a52-3c1e-4d7a-9b8e-81a0c0de0801 <folder>
```

`de` is the phone on USB: with a Windows 10 Mobile phone plugged in as well, pick the 8.1 one
by its number (`/EnumerateDevices`).

### Building it

```bash
python w10m/wp81/build.py
```

Visual Studio 2022's ARM tools and the SDKs as for the game (`tools/env.py`), and the
Windows Phone 8.1 kit (`C:\Program Files (x86)\Windows Phone Kits\8.1`) and Windows 8.1 SDK
(its `makeappx`, which knows 8.1's manifest). Into `w10m/work/wp81`:
`reVC81Trial_<version>_ARM.appx`, signed with gecko-w10m's certificate like the game.
`--arch x86` makes it for the Windows 10 Mobile emulator (`w10m/work/wp81/x86`), which runs
Windows Phone 8.1 packages: a check of the app, not of 8.1.
