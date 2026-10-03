# Port structure — current state and the seams that still need cutting

Written 2026-07-29 after M1; **updated 2026-08-02** after the native platform,
present, and VFS seams landed. Records how the tree is organised, what is
genuinely modular, and what is not yet but has to be before SDL2 and Emscripten
arrive.

## Layout

```
mc2-port/
  CMakeLists.txt          one file; targets, no build logic in scripts
  build.ps1               configure+build wrapper (vcvars, clang-cl vs cl)
  cmake/
    sources-*.cmake       GENERATED from the original VS2005 .vcproj files
  docs/
    PROGRESS.md           the % tracker
    ARCHITECTURE.md       this file
    GAMEOS_REQUIRED_SYMBOLS.txt   linker-derived backend surface
  src/
    game/                 MC2 sources, mirroring the original Code/MCLib/GUI
    gameos/
      include/            Microsoft's GameOS headers + mc2_port.h
      impl/               the clean-room backend
  tests/                  standalone unit targets
  tools/                  one script per concern
```

### What is already right

**No monolith.** The backend is split by subsystem, none of it large:

| file | responsibility |
|---|---|
| `gos_memory.cpp` | heap tree, heap stack, guarded blocks |
| `gos_file.cpp` | thin GameOS file ABI and application-hook policy |
| `gos_system.cpp` | machine info, screen mode, registry |
| `gos_platform.cpp` | entry point/main loop; dispatches through the platform seam |
| `gos_input.cpp` | input, IME stubs, resource strings |
| `gos_core.cpp` | Environment, timing, rand, stop/exit |
| `renderer/renderer_backend.h` | the `RendererBackend` interface (~18 ops) |
| `renderer/renderer_dispatch.cpp` | `gos_*` forwarding only, zero logic |
| `renderer/renderer_state.h` | the fixed-function state machine + census, shared by all backends |
| `renderer/renderer_image_decode.h` | host-selected JPEG decode contract |
| `renderer/win32/renderer_jpeg_wic.cpp` | Windows Imaging Component JPEG peer |
| `renderer/renderer_select.cpp` | which backend runs, and the fallback policy |
| `renderer/renderer_null.cpp` | null backend, a peer |
| `renderer/webgpu/renderer_webgpu.cpp` | Dawn backend factory, a peer |
| `renderer/webgpu/renderer_webgpu_internal.h` | private WebGPU implementation types and ownership |
| `renderer/webgpu/renderer_webgpu_backend.cpp` | device bring-up, frame lifecycle, diagnostics |
| `renderer/webgpu/renderer_webgpu_targets.cpp` | offscreen colour/depth targets and PPM readback |
| `renderer/webgpu/renderer_webgpu_pipelines.cpp` | shaders, pipeline/sampler caches, draw-state binding |
| `renderer/webgpu/renderer_webgpu_geometry.cpp` | frame arenas, quad expansion, draw submission |
| `renderer/webgpu/renderer_webgpu_textures.cpp` | texture creation, staging, decode, upload, bind groups |
| `renderer/webgpu/renderer_webgpu_text.cpp` | font ownership, layout, clipping, glyph emission |
| `renderer/webgpu/renderer_webgpu_surface.cpp` | native surface configure, acquire, blit, present |
| `platform/platform_backend.h` | native window/event interface; opaque handles only |
| `platform/platform_dispatch.cpp` | platform selection and forwarding |
| `platform/win32/platform_win32.cpp` | Win32 window, client input, resize/fullscreen |
| `vfs/vfs_backend.h` | storage interface; no path-resolution policy |
| `vfs/vfs_dispatch.cpp` | native backend selection, zero policy |
| `vfs/vfs_resolve.cpp` | shared separators, case fallback, and find filtering |
| `vfs/native/vfs_native.cpp` | native stdio, stat, directory, and enumeration peer |
| `audio/audio_null.cpp` | silent audio |

**Source lists are generated, not hand-maintained.** `tools/gen-source-lists.ps1`
reads the original `.vcproj` files, so the build cannot silently drift from what
Microsoft actually shipped, and a re-sync of the upstream drop is a re-run.

**Game edits are marked and diffable.** Every change to MC2 source carries a
`// PORT:` comment explaining what and why, and commit 1 is the untouched
import, so `git diff d8c8d80 -- src/game` is the complete set of modifications.

**Tools are one-per-concern**, 31–230 lines each, all dry-run by default.

**Two headers now carry cross-module contracts** (added 2026-07-29):
`gameos/include/mc2_port.h` is the only thing game code includes for port shims;
`gameos/impl/gos_backend.h` is how backend modules talk to each other. Before
this, `MC2_PortMessageBox` was hand-declared in six separate files.

## Seams that are now CUT

### The renderer — done (2026-07-29 interface, 2026-07-30 second implementation)

`RendererBackend` is an abstract interface, `gos_*` is thin dispatch, and null and
WebGPU are peers selected by `renderer_select.cpp` (`MC2_RENDERER=null|webgpu`,
defaulting to WebGPU when compiled in and **falling back to null if the GPU device
cannot be created**). The WebGPU backend landing required **zero game-code
changes**, which is the seam doing its job.

Two things worth knowing about how it is arranged:

**The state machine is shared, not duplicated.** `renderer_state.h` holds the
render-state block, the push/pop stack and the census; both backends own one. If
each kept its own copy, a disagreement between them would be a bug in the
bookkeeping rather than evidence about WebGPU — and comparing them is the whole
reason the null backend still exists.

**The WebGPU backend is a separate CMake target, `mc2_renderer_webgpu`, at C++20.**
Not stylistic: Dawn's generated headers use `<span>`, `explicit(false)` and a
`requires` clause, so they cannot compile at C++14, and raising all 322 TUs of
2001-era code to C++20 would buy five standards' worth of removals for nothing.
Mixing standards is safe *here specifically* because the seam is pure virtuals
over GameOS POD types, declared in a header with no standard containers — no
`std::` type crosses the boundary, so there is no layout to disagree about. Dawn's
include roots are private to that one target so nothing else can see a WebGPU
header by accident.

This is also the pattern for the next backend: a subsystem needing different
compile settings goes one directory deeper and gets its own target. The `impl/`
globs are deliberately one level deep, not recursive, so that is enforced rather
than hoped for.

### The native platform and presentation path -- done (2026-07-31)

`PlatformBackend` owns window creation, event pumping, client dimensions,
fullscreen transitions, and opaque native window/display handles. The main loop
only sees that interface. The `mc2_platform_win32` implementation is a separate
target, so an SDL2 or Emscripten peer can replace it without leaking platform
types into GameOS or the renderer.

JPEG decoding follows the renderer's existing image-codec pattern rather than the
windowing seam. `renderer_image_decode.h` is the host-independent contract; the
selected Win32 peer owns WIC, COM, and its system libraries. `gos_platform.cpp`
retains only the legacy GameOS ABI/allocation wrapper. A future Emscripten build
selects a browser/WASM decoder peer without changing GameOS or mission code.

Windowing remains opt-in (`MC2_WINDOW=1`), preserving deterministic headless runs.
When enabled, Win32 events are translated into the existing GameOS input module,
close requests set the normal quit flag instead of terminating the process, and
client resizes are propagated every tick. `RendererBackend::SetWindow` carries
only opaque handles and dimensions; WebGPU builds/configures a Dawn surface from
them, presents the existing offscreen result, and reconfigures on resize or a
recoverable surface status. The null renderer ignores the call. Thus neither the
game draw pipelines nor headless PPM readback depend on a native window.

### The VFS -- interface cut (2026-08-02)

`VfsBackend` owns storage-only operations: open/read/write/close, existence, size,
stat, directory enumeration, permission changes, and directory creation. The
selected native peer contains the existing stdio and host-filesystem mechanics.
`vfs_resolve.cpp` remains above the seam and applies separator normalization,
case-insensitive component fallback, and find-pattern filtering once for every
backend. `gos_file.cpp` retains only the GameOS ABI and the documented
`HookGetFile`/`HookDoesFileExist` application policy.

The native backend is a separate CMake target so a MEMFS/OPFS peer can be selected
without changing game code or duplicating path policy. The seam cut itself made
zero `src/game` changes and deliberately left the known negative-result cache
optimization for a later measured change.

## What still needs cutting

### Audio has its own module but not yet an interface

`gosAudio_*` moved out of the renderer file into `impl/audio/audio_null.cpp`
(2026-07-29). It is a module boundary, not yet an interface; give it the
`RendererBackend` treatment when real audio starts, not before.

## A runtime dependency the layout has to carry

`d3dcompiler_47.dll` must sit **next to the executable**. Dawn's D3D12 backend
loads it with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, which rejects a bare filename
with `ERROR_INVALID_PARAMETER` (87), so the copy in `System32` is never found and
device creation fails while complaining about a DLL that is plainly installed.
CMake copies it beside the built binary; **anything that relocates the executable
must relocate the DLL too**, or WebGPU silently degrades to the null backend.

## Judgement

The structure is sound, and the parts that matter most for rebuilding — generated
source lists, marked and diffable game edits, subsystem-split backend,
one-concern tools — are in place. Nothing is being built into one file.

The renderer seam has now been **proven** rather than merely asserted: a second
implementation with a different language standard and a 110 MB dependency landed
behind it without touching one line of game code. That is the strongest evidence
so far that the seam discipline is worth its cost.

Remaining seam debt is audio: it is a module boundary, not yet an interface. Cut
it when real audio makes a second implementation concrete. Renderer, platform,
image decode, and VFS are already isolated for future Emscripten peers.
