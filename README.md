# MechCommander 2 - REBONED (source)

A 64-bit port of Microsoft's MechCommander 2 source release to modern Windows and the web
browser. The original game code is kept, and every change made to it is marked with a
`// PORT:` comment explaining why. Windows, input, audio and video sit behind a rewritten,
clean-room GameOS layer; rendering is WebGPU, through Google's Dawn.

- Game page and download: <https://skeletonlogic.github.io/MacroslopBinbows/mc2/>
- Play in the browser: <https://skeletonlogic.github.io/mc2-play/>

**This repository contains source code only -- no game data.** To play a build you need
MechCommander 2's data files: either the
[Release 1.0 download](https://github.com/SkeletonLogic/MacroslopBinbows/releases/latest/download/MC2-REBONED-1.0.zip)
or your own copy of the game.

## Layout

The build expects the third-party libraries in a `third_party` folder **beside** this
repository, not inside it. Keep `some-folder` on a short path (for example `C:\src`):
the build's object paths are long and Windows' path limit will stop CMake otherwise.

```
some-folder/
  mc2-reboned/       this repository
  third_party/
    zlib/            required
    dawn/            required for the WebGPU renderer and the browser build
    ffmpeg/          optional, for in-game movies on Windows
```

## Windows build

Prerequisites:

- Visual Studio 2022 (Community or Build Tools) with **Desktop development with C++** and
  **C++ Clang Compiler for Windows** (that provides `clang-cl`).
- CMake 3.21 or newer, Ninja, Python 3, Git. All four must be on `PATH`.
- Windows 10/11 SDK (installed with the C++ workload; it supplies `d3dcompiler_47.dll`).

Fetch the libraries (from `some-folder`, in a "x64 Native Tools Command Prompt for VS 2022"):

```bat
git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git third_party\zlib

git clone https://dawn.googlesource.com/dawn third_party\dawn
cd third_party\dawn
git checkout 1e897275172a23f27b0022fa6beae3084ed54a9b
python tools\fetch_dawn_dependencies.py
cmake -S . -B out\Release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
      -DDAWN_FETCH_DEPENDENCIES=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF ^
      -DTINT_BUILD_TESTS=OFF -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
cmake --build out\Release --target webgpu_dawn
cd ..\..
```

Optional, for movies: unpack an **LGPL shared** FFmpeg 7.1 build for win64 (for example
`ffmpeg-n7.1-latest-win64-lgpl-shared-7.1` from <https://github.com/BtbN/FFmpeg-Builds/releases>)
so that `third_party\ffmpeg\include`, `lib` and `bin` exist. Without it the game builds and
runs, but skips movies.

Build the game (from this repository, in PowerShell):

```powershell
.\build.ps1 -WebGPU
```

The result is `build-clang-cl-RelWithDebInfo-webgpu\bin\MechCmd2.exe`, with
`d3dcompiler_47.dll` and the FFmpeg DLLs copied beside it. Copy those files into a folder
holding the MechCommander 2 game data (for example the unpacked Release 1.0 download) and
run `MechCmd2.exe` from there.

Headless check, run from the game-data folder:

```powershell
$env:MC2_RENDERER = "null"; $env:MC2_MAX_TICKS = "200"
.\MechCmd2.exe -mission mc2_01
```

It loads the first mission, runs 200 ticks without a window and exits with code 0.

Other options are documented at the top of [`build.ps1`](build.ps1) (`-Symbols` for a
debuggable build, `-Compiler cl` to cross-check with MSVC).

## Browser build

The browser build uses the Emscripten SDK that Dawn's dependency fetch places in
`third_party\dawn\third_party\emsdk`. Install and activate it once:

```powershell
cd ..\third_party\dawn\third_party\emsdk
.\emsdk install 5.0.6
.\emsdk activate 5.0.6
```

Then, from this repository:

```powershell
.\build.ps1 -Emscripten -Config Release
```

The page and engine land in `build-emscripten-Release\bin`. Game data is packaged for the
browser by [`tools/package-browser-data.mjs`](tools/package-browser-data.mjs) (Node.js), and
[`tools/mc2_browser_server.py`](tools/mc2_browser_server.py) serves the result locally.

## Repository map

- `src/game` -- Microsoft's game code, with `// PORT:` edits.
- `src/gameos` -- the rewritten GameOS platform layer (renderer, audio, input, files, video).
- `cmake/` -- source lists generated from the original Visual Studio projects by
  [`tools/gen-source-lists.ps1`](tools/gen-source-lists.ps1).
- `tests/` -- unit tests (`ctest` in the build folder).
- `docs/` -- [architecture](docs/ARCHITECTURE.md) and [widescreen](docs/WIDESCREEN.md) notes.

## Licence and credit

MechCommander 2 was created by FASA Interactive and published by Microsoft, who released
its source code under the Shared Source Limited Permissive License. This port is
distributed under that same licence -- see [`LICENSE.md`](LICENSE.md). Its art assets may
not be distributed commercially. Third-party libraries are listed in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

MechCommander is a trademark of Microsoft. This project is not affiliated with or endorsed
by Microsoft.
