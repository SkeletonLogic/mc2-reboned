//===========================================================================//
// GameOS platform layer -- process entry point, the main loop, and the
// platform globals GameOS is expected to own.
//
// Clean-room reimplementation from Source/GameOS/include/*.
//
// THIS FILE IS THE ARCHITECTURE. Phase 0's central structural finding was that
// MC2 does not own its main loop: GameOS does, and drives the game through four
// callbacks the game installs in the Environment block. `main` below is that
// loop, and Milestone 1 is defined precisely as running it end to end:
//
//     GetGameOSEnvironment(cmdLine)      game fills in Environment + callbacks
//     Environment.InitializeGameEngine() once, before the loop
//     Environment.DoGameLogic()          one tick, no drawing
//     Environment.UpdateRenderers()      drawing only
//     Environment.TerminateGameEngine()  once, after the loop
//
// Because the game never calls gos_RunMainLoop itself, the Emscripten version of
// this file is emscripten_set_main_loop(tick) rather than a while loop -- and
// nothing in the game changes. M5-5 enables Asyncify only beneath this seam so
// synchronous legacy file calls can use the browser's asynchronous OPFS API.
//
// M1 BRING-UP: the loop is bounded by default so the process exits cleanly and
// can be scripted. Set MC2_MAX_TICKS=0 for an unbounded run.
//===========================================================================//

#include <cstdio>
#include <chrono>   // M8-B13 slow-frame recorder
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#if defined(_WIN32)
#include <stdlib.h>     // _set_error_mode
#include <crtdbg.h>     // _CrtSetReportMode / _CrtSetReportFile
#endif

#include <gameos.hpp>
#include <mc2_port.h>

#include "audio/audio_backend.h"
#include "gos_backend.h"
#include "gos_memory_internal.h"
#include "mem/page_guard.h"
#include "platform/platform_backend.h"
#include "renderer/renderer_backend.h"
#include "renderer/renderer_image_decode.h"
#include "vfs/vfs_backend.h"
#include "vfs/vfs_resolve.h"


// Set by gos_TerminateApplication. Ends the loop cleanly so TerminateGameEngine
// still runs -- exiting the process there would skip the whole shutdown path.
bool g_gosQuitRequested = false;

//---------------------------------------------------------------------------//
// PORT: teardown census (M7 A1b). Contract is on the declaration in
// gameos/include/mc2_port.h; this is the implementation.
//
// It lives here rather than in gos_memory.cpp because it needs the platform
// heap validator, and gos_memory.cpp is deliberately free of platform headers
// so the Emscripten build inherits it unchanged.
//
// Two independent instruments per checkpoint, because they see different
// memory and the corruption is only known to be in one of them:
//   gosCorrupt  -- GameOS trailing guards, gos_Malloc blocks only
//   hostHeap    -- every heap in the process, which is where __fastfail fires
// The live-block delta is there so a phase that frees nothing, or frees far
// more than expected, is visible without a second run.
//---------------------------------------------------------------------------//
// One arming decision shared by both entry points below, cached so a disabled
// checkpoint stays one predictable branch on a path that runs every mission.
static bool TeardownCensusArmed()
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char* want = std::getenv("MC2_TEARDOWN_CENSUS");
        enabled = (want && *want && std::strcmp(want, "0") != 0) ? 1 : 0;
        if (enabled)
            std::fprintf(stderr, "[teardown] census armed: every checkpoint validates "
                                 "the GameOS guards AND every host heap\n");
    }
    return enabled != 0;
}

// PORT: contract is on the declaration in gameos/include/mc2_port.h.
void MC2_TeardownExpectLive(const char* what, const void* p)
{
    if (!TeardownCensusArmed()) return;

    // Two signals, and they are NOT equally strong. "not a gos block" is weak:
    // a healthy run already reports four of these that way, because they never
    // came from gos_Malloc and so were never in the table. "ALREADY FREED" is
    // strong -- that address was ours and we let it go, so deleting it again is
    // a double free rather than an inference.
    const long freedAt = gos_memory_FreedAt(p);
    const char* verdict = !p                        ? "null (already cleared)"
                        : freedAt >= 0              ? "*** ALREADY FREED -- DOUBLE DELETE ***"
                        : gos_memory_IsLiveBlock(p) ? "live"
                                                    : "not a gos block (weak signal; normal for several of these)";
    if (freedAt >= 0)
        std::fprintf(stderr, "[teardown-ptr] %-34s %p  %s (it was free #%ld)\n",
                     what ? what : "(unnamed)", p, verdict, freedAt);
    else
        std::fprintf(stderr, "[teardown-ptr] %-34s %p  %s\n", what ? what : "(unnamed)", p, verdict);
    std::fflush(stderr);
}

void MC2_TeardownCheckpoint(const char* phase)
{
    if (!TeardownCensusArmed()) return;

    static int    sequence  = 0;
    static size_t lastBlocks = 0;

    const size_t blocks = gos_memory_TotalLiveBlocks();
    const size_t bytes  = gos_memory_TotalLiveBytes();
    const long   delta  = static_cast<long>(blocks) - static_cast<long>(lastBlocks);
    lastBlocks = blocks;

    // Announce the phase BEFORE validating. If the walk itself faults on the
    // damage it is looking for, the phase name is already out.
    std::fprintf(stderr, "[teardown] %02d %-34s blocks=%zu (%+ld) bytes=%zu\n",
                 ++sequence, phase ? phase : "(unnamed)", blocks, delta, bytes);
    std::fflush(stderr);

    // PORT: B1-1. hostHeap below is a WHOLE-HEAP walk, and that was measured
    // blind to a smashed block header the per-block check caught. Run the
    // per-block sweep here too, or this checkpoint keeps printing 'ok' over a
    // heap that is already broken -- which is what it did for five sites.
    gos_memory_SweepBlocks(phase ? phase : "(unnamed)");
    const int gosCorrupt = gos_memory_CountCorruptBlocks();
    const int hostHeap   = MC2_PlatformHeapIntact();

    // PORT: threads is here because this allocator has NO LOCKING. Anything
    // other than threads=1 foreign=0 means two threads can be inside a global
    // unordered_map whose nodes come from malloc -- which corrupts the CRT heap
    // rather than the bookkeeping, lands on whatever free the main thread is
    // doing (hence three different crash sites), and leaves HeapValidate clean
    // at every checkpoint. Printed on every line so it cannot be missed.
    const size_t threads = gos_memory_ThreadsSeen();
    const size_t foreign = gos_memory_ForeignEvents();
    std::fprintf(stderr, "[teardown] %02d %-34s gosCorrupt=%d hostHeap=%s threads=%zu foreign=%zu%s\n",
                 sequence, phase ? phase : "(unnamed)", gosCorrupt,
                 hostHeap == 1 ? "ok" : (hostHeap == 0 ? "CORRUPT" : "unknown"),
                 threads, foreign,
                 foreign ? "   *** UNLOCKED ALLOCATOR REACHED FROM MORE THAN ONE THREAD ***" : "");
    std::fflush(stderr);
}
//---------------------------------------------------------------------------//
// PORT: in-mission heap census for the M7 A1b recurrence. The owner's latest
// failure happened during salvage, after an earlier Mission::destroy completed
// with both validators clean. Keep this separate from teardown numbering so the
// two events cannot be mistaken for one chronology in split stdout/stderr logs.
//---------------------------------------------------------------------------//
void MC2_RuntimeHeapCheckpoint(const char* phase)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char* want = std::getenv("MC2_RUNTIME_HEAP_CENSUS");
        enabled = (want && *want && std::strcmp(want, "0") != 0) ? 1 : 0;
        if (enabled)
            std::fprintf(stderr, "[runtime-heap] census armed: sparse mission heartbeats "
                                 "and salvage state changes\n");
    }
    if (!enabled) return;
    static unsigned long sequence = 0;
    const size_t blocks = gos_memory_TotalLiveBlocks();
    const size_t bytes = gos_memory_TotalLiveBytes();
    std::fprintf(stderr, "[runtime-heap] %04lu %-34s blocks=%zu bytes=%zu\n",
                 ++sequence, phase ? phase : "(unnamed)", blocks, bytes);
    std::fflush(stderr);
    // PORT: B1-1. hostHeap below is a WHOLE-HEAP walk, and that was measured
    // blind to a smashed block header the per-block check caught. Run the
    // per-block sweep here too, or this checkpoint keeps printing 'ok' over a
    // heap that is already broken -- which is what it did for five sites.
    gos_memory_SweepBlocks(phase ? phase : "(unnamed)");
    const int gosCorrupt = gos_memory_CountCorruptBlocks();
    const int hostHeap = MC2_PlatformHeapIntact();
    std::fprintf(stderr, "[runtime-heap] %04lu %-34s gosCorrupt=%d hostHeap=%s\n",
                 sequence, phase ? phase : "(unnamed)", gosCorrupt,
                 hostHeap == 1 ? "ok" : (hostHeap == 0 ? "CORRUPT" : "unknown"));
    std::fflush(stderr);
}

//---------------------------------------------------------------------------//
// Platform globals GameOS owns.
//
// These are declared extern across the GameOS headers and referenced directly
// by game code, so they must exist even where this backend has nothing
// meaningful to put in them yet.
//---------------------------------------------------------------------------//

// Win32 window handle. Null until a real window exists (the renderer is not
// written yet); game code only ever passes it back into GameOS.
struct HWND__* hWindow = nullptr;

// Legacy DirectDraw front buffer. Permanently null in this port -- the renderer
// target is WebGPU, and the one game file that touches DirectDraw (MCLib/mouse.cpp)
// checks this for null.
struct IDirectDrawSurface7* FrontBufferSurface = nullptr;

// PORT (M8-B49): present ONE frame from inside the blocking mission load.
//
// The loading bar has never appeared in this port. Retail draws it from the mouse
// timer straight into a locked DirectDraw front buffer, and FrontBufferSurface
// above is null, so that callback never fires. Drawing it as ordinary GUI quads
// fixed the drawing, and the instrument then showed why that was still not enough:
//
//     [loadbar] first draw: loadProgress=100.0 showing=1 backShown=1 fillShown=1
//
// The very first time the draw is reached, the load is already DONE. The mission
// load is one blocking call inside DoGameLogic, so not a single frame is presented
// while progress climbs from 0 to 100.
//
// WHY THIS IS NOT THE PUMP THAT WAS REVERTED. That one called the load screen's
// own render directly -- BeginFrame, draw the screen, EndFrame -- which skipped
// Environment.UpdateRenderers() and left everything else in the frame undrawn.
// That is the black box the owner reported. This runs exactly what the real frame
// loop runs, minus the logic tick:
//
//     Renderer().BeginFrame(); Environment.UpdateRenderers(); Renderer().EndFrame();
//
// Safe to call from inside DoGameLogic because the loop only opens its frame AFTER
// the logic tick returns, so no frame is in progress here and there is no nesting.
// Inert under the null backend, so the strict bar cannot move.
extern bool MC2_RendererIsNull();

void MC2_PumpLoadFrame()
{
    if (MC2_RendererIsNull() || !Environment.UpdateRenderers)
        return;
    Renderer().BeginFrame();
    Environment.UpdateRenderers();
    Renderer().EndFrame();
}



float         frameRate      = 0.0f;    // updated by the loop below
float         ProcessorSpeed = 0.0f;    // MHz; informational only
unsigned long NumDevices     = 1;       // one render device
bool          gNoDialogs     = true;    // never show modal dialogs; this is not an interactive shell

struct _FontInfo* FontHandle = nullptr; // set once the font subsystem exists

// Offset of the client area within the window, used to convert mouse
// coordinates. Zero while running windowless.
struct tagPOINT { long x; long y; };
struct tagPOINT clientToScreen = { 0, 0 };

// Flags the GameOS mouse timer and display code toggle. Declared volatile
// because the original updated them from a multimedia timer callback.
volatile bool mc2IsInMouseTimer        = false;
volatile bool mc2IsInDisplayBackBuffer = false;
volatile bool mc2DisplayHasFlipped     = false;
volatile bool mc2HasLostFocus          = false;

//---------------------------------------------------------------------------//
// Modal dialog replacement.
//
// MC2 calls Win32 MessageBox in 13 places, several on the data-loading path.
// Every one is fatal to a headless run, and ffile.cpp:112 shows why it is worse
// than "a dialog appears":
//
//     while (handle == NULL) {
//         result = MessageBox(..., MB_OKCANCEL);
//         if (result == IDCANCEL) { ExitGameOS(); }
//         handle = fopen(actualPath, "r");     // retries the SAME failing open
//     }
//
// IDOK therefore loops forever reopening a file that is not there. A run that
// looked like a hang -- 617 seconds wall clock, 0.9s CPU, no output -- was this.
//
// Returning IDCANCEL turns that hang into a clean, diagnosable exit, and the
// message is printed so the real cause (which file) is visible instead of
// trapped in a dialog nobody is watching.
//
// Required rather than merely convenient: Emscripten has no MessageBox, so all
// 13 sites had to be redirected for the web build regardless.
//---------------------------------------------------------------------------//
// Signature mirrors Win32 MessageBox, HWND first, so the call sites are a
// straight textual substitution.
int MC2_PortMessageBox(void* /*hwnd*/, const char* text, const char* caption, unsigned int /*type*/)
{
    std::fprintf(stderr, "\n[GameOS dialog] %s\n              : %s\n",
                 (caption && *caption) ? caption : "(no title)",
                 (text && *text) ? text : "(no text)");
    std::fflush(stderr);
    return 2;   // IDCANCEL -- give up rather than retry forever
}

//---------------------------------------------------------------------------//
// Screen mode / window state
//
// The platform backend owns the real transition; Environment remains the
// portable state the game queries.
//---------------------------------------------------------------------------//
void __cdecl EnterWindowMode()
{
    Environment.fullScreen = false;
    Platform().SetFullscreen(false);
}

void __cdecl EnterFullScreenMode()
{
    Environment.fullScreen = true;
    Platform().SetFullscreen(true);
}

//---------------------------------------------------------------------------//
// Debugger / statistics hooks.
//
// GameOS shipped an in-process debug overlay that games registered counters and
// menu items with. There is no overlay here, so these accept and discard. They
// are called during startup, so they must exist and must not fail.
//---------------------------------------------------------------------------//
void __cdecl AddStatistic(char* /*Name*/, char* /*Units*/, enum gosType /*Type*/,
                          void* /*Value*/, unsigned long /*Flags*/)
{
}

void __cdecl StatisticFormat(char* /*String*/)
{
}

void __cdecl AddDebuggerMenuItem(char* /*Name*/,
                                 bool  (__cdecl* /*IsAvailable*/)(void),
                                 void  (__cdecl* /*Execute*/)(void),
                                 bool  (__cdecl* /*IsChecked*/)(void),
                                 unsigned long (__cdecl* /*GetText*/)(char*, unsigned long))
{
}

//---------------------------------------------------------------------------//
// Diagnostics
//---------------------------------------------------------------------------//

// Resolved a code address to a symbol name for crash reports, using the
// debug-info machinery GameOS carried. Reports the raw address instead.
char* __cdecl DecodeAddress(unsigned long address, bool /*includeOffset*/)
{
    static char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", address);
    return buffer;
}

// Structured-exception filter. Nothing is caught here: letting a fault reach the
// debugger is more useful during bring-up than swallowing it.
//
// PORT: on Win32 the real reporter lives in
// impl/platform/win32/crash_report_win32.cpp -- it names the exception code,
// the faulting address and walks the stack. It cannot live here because this
// file defines its own tagPOINT and so cannot include windows.h.
#if !defined(_WIN32) || defined(__EMSCRIPTEN__)
long __cdecl ProcessException(struct _EXCEPTION_POINTERS* /*ExceptionPointers*/)
{
    std::fprintf(stderr, "[GameOS] unhandled exception reached ProcessException\n");
    std::fflush(stderr);
    return 0;   // EXCEPTION_CONTINUE_SEARCH
}
#endif

// JPEG decode used by packed mission color maps. The selected image decoder peer
// owns the host codec; this wrapper preserves GameOS allocation and ABI contracts.
void* __cdecl DecodeJPG(const char* fileName, unsigned char* data,
                        unsigned long size, unsigned long* width,
                        unsigned long* height, bool /*textureLoad*/, void* dest)
{
    if (!data || !size || !width || !height) return nullptr;
    mc2gfx::JpegImage image;
    if (!mc2gfx::DecodeJpeg(data, size, image))
    {
        std::fprintf(stderr, "[GameOS] JPEG decode failed for '%s'\n",
                     fileName ? fileName : "");
        return nullptr;
    }
    void* pixels = dest ? dest : gos_Malloc(image.rgba.size());
    if (!pixels) return nullptr;
    std::memcpy(pixels, image.rgba.data(), image.rgba.size());
    *width = image.width;
    *height = image.height;
#if defined(__EMSCRIPTEN__)
    static unsigned long browserJpegDecodes = 0;
    ++browserJpegDecodes;
    std::printf("[GameOS] browser libjpeg decode[%lu] '%s': %lux%lu sourceBytes=%lu\n",
                browserJpegDecodes, fileName ? fileName : "",
                image.width, image.height, size);
#endif
    return pixels;
}

//---------------------------------------------------------------------------//
// Entry point and main loop
//---------------------------------------------------------------------------//
namespace {

// Bounded by default so M1 bring-up terminates and can be scripted.
long ticksFromEnvironment()
{
    const char* v = std::getenv("MC2_MAX_TICKS");
    if (!v || !*v) return 100;
    return std::strtol(v, nullptr, 10);   // 0 == run until the game exits
}

double fixedFrameSecondsFromEnvironment()
{
    const char* value = std::getenv("MC2_FIXED_FPS");
    if (!value || !*value) return 0.0;
    char* end = nullptr;
    const double fps = std::strtod(value, &end);
    return end != value && *end == '\0' && fps > 0.0 ? 1.0 / fps : 0.0;
}

bool windowRequested()
{
    const char* value = std::getenv("MC2_WINDOW");
    return value && *value && std::strcmp(value, "0") != 0;
}

#if defined(__EMSCRIPTEN__)
struct BrowserLoopState
{
    long maxTicks = 0;
    double fixedFrameSeconds = 0.0;
    long tickProgressInterval = 0;
    long traceTick = 0;
    long traceFromTick = 0;
    bool timingStats = false;
    long ticksRun = 0;
    double frameSecondsTotal = 0.0;
    double frameSecondsMin = std::numeric_limits<double>::max();
    double frameSecondsMax = 0.0;
};

void finishBrowserLoop(BrowserLoopState& state)
{
    emscripten_cancel_main_loop();
    std::printf("[GameOS] ran %ld tick(s)%s\n", state.ticksRun,
                g_gosQuitRequested ? " (game requested quit)" : "");
    if (state.timingStats && state.ticksRun > 0) {
        const double averageSeconds = state.frameSecondsTotal / static_cast<double>(state.ticksRun);
        std::printf("[GameOS timing] frames=%ld minMs=%.3f maxMs=%.3f avgMs=%.3f avgFps=%.3f\n",
                    state.ticksRun, state.frameSecondsMin * 1000.0,
                    state.frameSecondsMax * 1000.0, averageSeconds * 1000.0,
                    averageSeconds > 0.0 ? 1.0 / averageSeconds : 0.0);
    }
    std::printf("[GameOS] main loop finished\n");
    const char* memoryStats = std::getenv("MC2_MEMORY_STATS");
    if (memoryStats && *memoryStats && std::strcmp(memoryStats, "0") != 0)
        gos_WalkMemoryHeap(nullptr, false);
    gosRenderNull_DumpStats();
    gosAudioNull_DumpStats();
    VfsDumpStats();
    Platform().DumpStats();
    // PORT (M8-B7): silent unless MC2_PAGE_GUARD is set. Its counts are the
    // proof that the guard actually took the allocations -- an armed banner
    // with zero blocks would mean every request fell back to malloc and the
    // run diagnosed nothing.
    mc2mem::PageGuardReport();
    std::printf("[GameOS] TerminateGameEngine ...\n");
    Environment.TerminateGameEngine();
    std::printf("[GameOS] TerminateGameEngine returned -- clean exit\n");
    mc2audio::AudioShutdown();
    Platform().Destroy();
}

void browserMainLoop(void* context)
{
    auto& state = *static_cast<BrowserLoopState*>(context);
    // PORT: entry marker, before ANY work. Without it "no tick output" cannot be
    // told apart from "the callback is never scheduled", which is the difference
    // between a scheduler bug and a hang inside the first frame.
    if (state.traceFromTick > 0 && state.ticksRun + 1 >= state.traceFromTick)
        std::printf("[GameOS] trace tick %ld: enter\n", state.ticksRun + 1);
    if (g_gosQuitRequested || (state.maxTicks != 0 && state.ticksRun >= state.maxTicks)) {
        finishBrowserLoop(state);
        return;
    }
    gosInput_BeginFrame();
    Platform().SetGameScreenSize(Environment.screenWidth, Environment.screenHeight);
    if (Platform().HasWindow() && !Platform().PumpEvents()) {
        g_gosQuitRequested = true;
        finishBrowserLoop(state);
        return;
    }
    int clientWidth = 0, clientHeight = 0;
    Platform().ClientSize(&clientWidth, &clientHeight);
    Renderer().SetWindow(Platform().NativeWindowHandle(), Platform().NativeDisplayHandle(),
                         clientWidth, clientHeight);
    const double frameSeconds = gosTiming_BeginFrame(state.fixedFrameSeconds);
    frameRate = frameSeconds > 0.0 ? static_cast<float>(1.0 / frameSeconds) : 0.0f;
    state.frameSecondsTotal += frameSeconds;
    if (frameSeconds < state.frameSecondsMin) state.frameSecondsMin = frameSeconds;
    if (frameSeconds > state.frameSecondsMax) state.frameSecondsMax = frameSeconds;
    const long tick = state.ticksRun + 1;
    const bool trace = state.traceTick == tick ||
        (state.traceFromTick > 0 && tick >= state.traceFromTick);
    if (trace) std::printf("[GameOS] trace tick %ld: logic begin\n", tick);
    Environment.DoGameLogic();
    if (trace) std::printf("[GameOS] trace tick %ld: logic end\n", tick);
    Renderer().BeginFrame();
    if (trace) std::printf("[GameOS] trace tick %ld: render begin\n", tick);
    Environment.UpdateRenderers();
    if (trace) std::printf("[GameOS] trace tick %ld: render end\n", tick);
    Renderer().EndFrame();
    ++state.ticksRun;
    if (state.tickProgressInterval > 0 && state.ticksRun % state.tickProgressInterval == 0)
        std::printf("[GameOS] tick progress: %ld/%ld\n", state.ticksRun, state.maxTicks);
    if (g_gosQuitRequested || (state.maxTicks != 0 && state.ticksRun >= state.maxTicks))
        finishBrowserLoop(state);
}
#endif

} // namespace

int main(int argc, char** argv)
{
    // Rebuild a single command-line string; GetGameOSEnvironment parses it.
    char cmdLine[1024] = { 0 };
    for (int i = 1; i < argc; ++i)
    {
        if (cmdLine[0]) std::strncat(cmdLine, " ", sizeof(cmdLine) - std::strlen(cmdLine) - 1);
        std::strncat(cmdLine, argv[i], sizeof(cmdLine) - std::strlen(cmdLine) - 1);
    }

    // Unbuffered stdout. During bring-up the process is routinely killed while
    // blocked, and block-buffered output is then lost entirely -- which hid the
    // whole startup trace behind a wall of stderr messages and made a hang look
    // like it never started.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Route CRT assertions to stderr instead of a modal dialog.
    //
    // THIS IS NOT COSMETIC. A debug-CRT assertion (_CrtIsValidHeapPointer and
    // friends) raises a MessageBox, and a headless run then sits in it forever.
    // Every "hang" investigated during bring-up presented as blocked-on-syscall
    // with ~0 CPU and no output -- indistinguishable from a deadlock, and the
    // dialog was only visible to someone watching the desktop.
    //
    // With this, the same failure prints the assertion text and the process
    // keeps going (or dies) instead of stalling invisibly.
#if defined(_DEBUG) && defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN,   _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN,   _CRTDBG_FILE_STDERR);
#endif
    // Belt and braces: also suppress the Win32 error box for hard faults.
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
#endif

#if defined(__EMSCRIPTEN__)
    std::printf("[GameOS] MechCommander 2 -- portable runtime bring-up\n");
    // PORT: Mount the shell-populated origin-private filesystem before any
    // renderer or game initialization can resolve a data path.
    if (!InitializeEmscriptenVfs()) return 3;
#else
    std::printf("[GameOS] MechCommander 2 -- native port bring-up\n");
#endif

    // 0. Choose the renderer backend. This has to happen before
    //    InitializeGameEngine, not with the rest of the graphics setup: the game
    //    creates and locks textures during DATA LOAD, so a backend chosen later
    //    would miss every texture the engine builds while starting up.
    gosRenderer_Select();

    // 1. Let the game populate Environment and install its callbacks.
    GetGameOSEnvironment(cmdLine);

    if (!Environment.InitializeGameEngine || !Environment.DoGameLogic ||
        !Environment.UpdateRenderers || !Environment.TerminateGameEngine)
    {
        std::fprintf(stderr, "[GameOS] the game did not install all four loop callbacks\n");
        return 2;
    }

    std::printf("[GameOS] app='%s' version='%s' screen=%dx%d\n",
                Environment.applicationName ? Environment.applicationName : "(unset)",
                Environment.version ? Environment.version : "(unset)",
                Environment.screenWidth, Environment.screenHeight);

    const bool requestedWindow = windowRequested();
    if (requestedWindow)
    {
        if (!Platform().Create(Environment.applicationName,
                               Environment.screenWidth, Environment.screenHeight))
            std::fprintf(stderr, "[GameOS] window creation failed; continuing headless\n");
    }
    int clientWidth = 0, clientHeight = 0;
    Platform().ClientSize(&clientWidth, &clientHeight);
    Renderer().SetWindow(Platform().NativeWindowHandle(), Platform().NativeDisplayHandle(),
                         clientWidth, clientHeight);
    std::printf("[GameOS] platform=%s window=%s\n", Platform().Name(),
                Platform().HasWindow() ? "yes" : "no");

    // 2. One-time engine init.
    // PORT: B1-1 (M8-B7). Prove the double-free probe can fire BEFORE the
    // session that depends on it, so a silent log is a finding rather than an
    // unanswered question. No-op unless MC2_GOS_FREE_PROBE is set.
    gos_memory_ProbeSelfTest();

    std::printf("[GameOS] InitializeGameEngine ...\n");
    Environment.InitializeGameEngine();
    std::printf("[GameOS] InitializeGameEngine returned\n");
    Platform().SetGameScreenSize(Environment.screenWidth, Environment.screenHeight);
    Platform().ClientSize(&clientWidth, &clientHeight);
    Renderer().SetWindow(Platform().NativeWindowHandle(), Platform().NativeDisplayHandle(),
                         clientWidth, clientHeight);

    // GameOS owns the clock epoch and takes exactly one sample per loop. The
    // game can query cumulative elapsed time as often as it likes without
    // perturbing the next frame's delta.
    gosTiming_Reset();

    // 3. The loop. Logic and render are kept as separate calls exactly as the
    //    header specifies, because UpdateRenderers is the only place the game
    //    may draw -- which is what lets the renderer be skipped entirely here.
    const long maxTicks = ticksFromEnvironment();
    const double fixedFrameSeconds = fixedFrameSecondsFromEnvironment();
    long tickProgressInterval = 0;
    if (const char* progress = std::getenv("MC2_TICK_PROGRESS"))
        tickProgressInterval = std::strtol(progress, nullptr, 10);
    long traceTick = 0;
    if (const char* trace = std::getenv("MC2_TRACE_TICK"))
        traceTick = std::strtol(trace, nullptr, 10);
    long traceFromTick = 0;
    if (const char* trace = std::getenv("MC2_TRACE_FROM_TICK"))
        traceFromTick = std::strtol(trace, nullptr, 10);
    const char* timingStatsValue = std::getenv("MC2_TIMING_STATS");
    const bool timingStats = timingStatsValue && *timingStatsValue &&
        std::strcmp(timingStatsValue, "0") != 0;
    std::printf("[GameOS] entering main loop (maxTicks=%ld)\n", maxTicks);
    if (fixedFrameSeconds > 0.0)
        std::printf("[GameOS] fixed-step regression mode: %.3f FPS\n",
                    1.0 / fixedFrameSeconds);

#if defined(__EMSCRIPTEN__)
    // PORT: The browser owns the event loop. No Asyncify is required because
    // GameOS already owns the four game callbacks and can yield between ticks.
    static BrowserLoopState browserState;
    browserState.maxTicks = maxTicks;
    browserState.fixedFrameSeconds = fixedFrameSeconds;
    browserState.tickProgressInterval = tickProgressInterval;
    browserState.traceTick = traceTick;
    browserState.traceFromTick = traceFromTick;
    browserState.timingStats = timingStats;
    // PORT: rAF remains the production default (fps 0), because it reaches the
    // display's full refresh rate. The timer override selects setTimeout instead,
    // for runs with no visible surface.
    //
    // It MUST be expressed as the fps argument here, not as a follow-up call to
    // emscripten_set_main_loop_timing. Registration queues exactly one callback
    // using whichever scheduler is installed at that moment, and
    // emscripten_set_main_loop_timing only swaps the scheduler *function* -- it
    // never re-arms anything. Call it before registration and it is a no-op
    // ("Cannot set timing mode for main loop since a main loop does not exist!");
    // call it after and the loop is already parked on the single rAF that
    // registration queued. rAF is fully suspended in a hidden document, so either
    // way the loop never runs once and the failure is silent. Both orderings were
    // measured on 2026-08-04: 0 of 3 ticks, and the entry trace never printed.
    // Passing fps > 0 installs the setTimeout scheduler and kicks it in the same
    // call, which is the only variant that actually starts.
    int loopFps = 0;
    if (const char* scheduler = std::getenv("MC2_BROWSER_SCHEDULER"))
        if (std::strcmp(scheduler, "timer") == 0)
        {
            // 1 ms requested; browsers clamp hidden-tab timers to ~4-5 ms.
            loopFps = 1000;
            std::printf("[GameOS] browser scheduler override: timer\n");
        }
    emscripten_set_main_loop_arg(browserMainLoop, &browserState, loopFps, false);
    return 0;
#else
    long ticksRun = 0;
    double frameSecondsTotal = 0.0;
    double frameSecondsMin = std::numeric_limits<double>::max();
    double frameSecondsMax = 0.0;

    // M8-B13, 2026-08-20. Owner reports drops to 45 FPS and suspects smoke from
    // destroyed buildings, saying explicitly that they cannot tell for certain.
    // The session aggregate cannot settle it: 160,575 frames at avgMs 7.316 with
    // a single maxMs of 385.002 says a spike happened, not what was in it, and
    // not how often 22 ms frames occurred.
    //
    // SPLIT THE FRAME, because the two candidates sit on opposite sides of it.
    // Effect SIMULATION cost lands in DoGameLogic; effect DRAWING cost lands in
    // UpdateRenderers/EndFrame. A slow frame that is all logic and a slow frame
    // that is all render need different work, and the aggregate hides which.
    //
    // MC2_SLOW_FRAME_MS=<ms> arms it; unset or 0 is off and costs nothing --
    // no clock is read at all on a normal run, so the strict bar cannot move.
    // The histogram prints at exit whether or not any line was emitted, because
    // a threshold that was never crossed is itself the answer to how often.
    double slowFrameMs = 0.0;
    if (const char* slowEnv = std::getenv("MC2_SLOW_FRAME_MS"))
        slowFrameMs = std::atof(slowEnv);
    long slowFrames = 0, over60 = 0, over45 = 0, over30 = 0;
    double worstTotalMs = 0.0, worstLogicMs = 0.0, worstRenderMs = 0.0;
    long   worstTick = -1;
    double slowLogicMsTotal = 0.0, slowRenderMsTotal = 0.0;
    // PORT (M8-B18): the existing split only summed the frames that tripped
    // the threshold, so it could say what a SLOW frame looked like but not
    // where the AVERAGE frame goes. 3.65 ms of an 8.28 ms frame is render and
    // present, and had never been profiled. These accumulate every frame, and
    // cost two extra clock reads per frame.
    double allLogicMsTotal = 0.0, allBeginMsTotal = 0.0;
    double allDrawMsTotal = 0.0, allEndMsTotal = 0.0;
    long   allFrames = 0;
    // PORT (M8-B18): GROUND TRUTH. `frameSeconds` comes from
    // gosTiming_BeginFrame, which returns GAME time, not real time: when the
    // real delta exceeds Environment.MaxTimeDelta it substitutes
    // MinimumTimeDelta. Every fps figure this project quotes is derived from
    // that number, so it needs checking against a plain steady_clock delta
    // between loop iterations before any of it can be believed.
    double allWallMsTotal = 0.0;
    // THE LAST TICK IS AN OUTLIER AND IT WRECKED THE FIRST VERSION OF THIS
    // PROBE. Mission teardown runs inside DoGameLogic on the final tick: it
    // measured 2427 ms of "logic" in one frame, which alone put 6.07 ms on a
    // 10.8 ms mean. Worse, the wall sampler never sees that frame -- it times
    // the interval BETWEEN loop tops, so the last frame has no successor to
    // close it. The two sums therefore covered different frame sets and logic
    // "exceeded" total wall time, which is impossible and is what gave the
    // problem away. Both are now reported over frames 0..n-2.
    double lastLogicMs = 0.0, lastBeginMs = 0.0;
    double lastDrawMs = 0.0, lastEndMs = 0.0;
    std::chrono::steady_clock::time_point lastLoopTop;
    bool   haveLastLoopTop = false;
    for (long tick = 0; !g_gosQuitRequested && (maxTicks == 0 || tick < maxTicks); ++tick)
    {
        if (slowFrameMs > 0.0)
        {
            const std::chrono::steady_clock::time_point loopTop =
                std::chrono::steady_clock::now();
            if (haveLastLoopTop)
                allWallMsTotal += std::chrono::duration<double, std::milli>(
                    loopTop - lastLoopTop).count();
            lastLoopTop = loopTop;
            haveLastLoopTop = true;
        }
        gosInput_BeginFrame();
        Platform().SetGameScreenSize(Environment.screenWidth, Environment.screenHeight);
        if (Platform().HasWindow() && !Platform().PumpEvents()) break;
        Platform().ClientSize(&clientWidth, &clientHeight);
        Renderer().SetWindow(Platform().NativeWindowHandle(), Platform().NativeDisplayHandle(),
                             clientWidth, clientHeight);
        const double frameSeconds = gosTiming_BeginFrame(fixedFrameSeconds);
        frameRate = frameSeconds > 0.0
            ? static_cast<float>(1.0 / frameSeconds)
            : 0.0f;
        frameSecondsTotal += frameSeconds;
        if (frameSeconds < frameSecondsMin) frameSecondsMin = frameSeconds;
        if (frameSeconds > frameSecondsMax) frameSecondsMax = frameSeconds;
        const bool traceThisTick = traceTick == tick + 1 ||
            (traceFromTick > 0 && tick + 1 >= traceFromTick);
        if (traceThisTick) std::printf("[GameOS] trace tick %ld: logic begin\n", tick + 1);
        const std::chrono::steady_clock::time_point phase0 =
            slowFrameMs > 0.0 ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point();
        Environment.DoGameLogic();
        const std::chrono::steady_clock::time_point phase1 =
            slowFrameMs > 0.0 ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point();
        if (traceThisTick) std::printf("[GameOS] trace tick %ld: logic end\n", tick + 1);
        Renderer().BeginFrame();
        const std::chrono::steady_clock::time_point phaseBegin =
            slowFrameMs > 0.0 ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point();
        if (traceThisTick) std::printf("[GameOS] trace tick %ld: render begin\n", tick + 1);
        Environment.UpdateRenderers();
        const std::chrono::steady_clock::time_point phaseDraw =
            slowFrameMs > 0.0 ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point();
        if (traceThisTick) std::printf("[GameOS] trace tick %ld: render end\n", tick + 1);
        Renderer().EndFrame();
        if (slowFrameMs > 0.0)
        {
            const std::chrono::steady_clock::time_point phase2 =
                std::chrono::steady_clock::now();
            const double logicMs =
                std::chrono::duration<double, std::milli>(phase1 - phase0).count();
            const double renderMs =
                std::chrono::duration<double, std::milli>(phase2 - phase1).count();
            const double totalMs = logicMs + renderMs;
            ++allFrames;
            lastLogicMs = logicMs;
            lastBeginMs = std::chrono::duration<double, std::milli>(
                phaseBegin - phase1).count();
            lastDrawMs  = std::chrono::duration<double, std::milli>(
                phaseDraw - phaseBegin).count();
            lastEndMs   = std::chrono::duration<double, std::milli>(
                phase2 - phaseDraw).count();
            allLogicMsTotal += logicMs;
            allBeginMsTotal += std::chrono::duration<double, std::milli>(
                phaseBegin - phase1).count();
            allDrawMsTotal  += std::chrono::duration<double, std::milli>(
                phaseDraw - phaseBegin).count();
            allEndMsTotal   += std::chrono::duration<double, std::milli>(
                phase2 - phaseDraw).count();
            if (totalMs > 16.7) ++over60;
            if (totalMs > 22.2) ++over45;   // the number the owner reported
            if (totalMs > 33.3) ++over30;
            if (totalMs > worstTotalMs)
            {
                worstTotalMs = totalMs; worstLogicMs = logicMs;
                worstRenderMs = renderMs; worstTick = tick + 1;
            }
            if (totalMs >= slowFrameMs)
            {
                ++slowFrames;
                slowLogicMsTotal += logicMs;
                slowRenderMsTotal += renderMs;
                std::fprintf(stderr,
                    "[slow-frame] tick=%ld totalMs=%.2f logicMs=%.2f renderMs=%.2f\n",
                    tick + 1, totalMs, logicMs, renderMs);
            }
        }
        ++ticksRun;
        if (tickProgressInterval > 0 && ticksRun % tickProgressInterval == 0)
            std::printf("[GameOS] tick progress: %ld/%ld\n", ticksRun, maxTicks);
    }

    std::printf("[GameOS] ran %ld tick(s)%s\n", ticksRun,
                g_gosQuitRequested ? " (game requested quit)" : "");

    if (timingStats && ticksRun > 0)
    {
        const double averageSeconds = frameSecondsTotal / static_cast<double>(ticksRun);
        std::printf("[GameOS timing] frames=%ld minMs=%.3f maxMs=%.3f avgMs=%.3f avgFps=%.3f\n",
                    ticksRun, frameSecondsMin * 1000.0, frameSecondsMax * 1000.0,
                    averageSeconds * 1000.0,
                    averageSeconds > 0.0 ? 1.0 / averageSeconds : 0.0);
    }

    // M8-B13. Printed whenever the recorder was armed, even with zero slow
    // frames, because a report that only appears on a hit can never say that
    // the threshold was never crossed.
    if (slowFrameMs > 0.0)
    {
        if (allFrames > 1)
        {
            // Drop the final tick from every sum so the phases and the wall
            // clock describe the same frames. See the note at the declarations.
            const double n = (double)(allFrames - 1);
            const double logicMean = (allLogicMsTotal - lastLogicMs) / n;
            const double beginMean = (allBeginMsTotal - lastBeginMs) / n;
            const double drawMean  = (allDrawMsTotal  - lastDrawMs)  / n;
            const double endMean   = (allEndMsTotal   - lastEndMs)   / n;
            std::printf("[frame-split] over %ld frames, mean ms: logic=%.3f "
                        "BeginFrame=%.3f UpdateRenderers=%.3f EndFrame=%.3f "
                        "(render subtotal %.3f, accounted total %.3f)\n",
                        allFrames - 1, logicMean, beginMean, drawMean, endMean,
                        beginMean + drawMean + endMean,
                        logicMean + beginMean + drawMean + endMean);
            std::printf("[frame-split] final tick excluded as an outlier: "
                        "logic=%.1f ms (mission teardown)\n", lastLogicMs);
        }
        if (allFrames > 1)
            std::printf("[frame-split] REAL wall frame mean %.3f ms (%.1f fps) "
                        "vs reported %.3f ms; MaxTimeDelta=%.4f "
                        "MinimumTimeDelta=%.4f\n",
                        allWallMsTotal / (double)(allFrames - 1),
                        allWallMsTotal > 0.0
                            ? 1000.0 * (double)(allFrames - 1) / allWallMsTotal
                            : 0.0,
                        1000.0 * frameSecondsTotal / (double)ticksRun,
                        (double)Environment.MaxTimeDelta,
                        (double)Environment.MinimumTimeDelta);
        std::printf("[frame-split] RAW totals: frames=%ld logic=%.1f begin=%.1f "
                    "draw=%.1f end=%.1f wall=%.1f frameSecondsTotal=%.1f ms\n",
                    allFrames, allLogicMsTotal, allBeginMsTotal, allDrawMsTotal,
                    allEndMsTotal, allWallMsTotal, 1000.0 * frameSecondsTotal);
        std::printf("[slow-frame] armed at %.1f ms over %ld frames: "
                    "over16.7ms=%ld over22.2ms=%ld over33.3ms=%ld reported=%ld\n",
                    slowFrameMs, ticksRun, over60, over45, over30, slowFrames);
        if (worstTick >= 0)
            std::printf("[slow-frame] worst tick=%ld totalMs=%.2f logicMs=%.2f renderMs=%.2f\n",
                        worstTick, worstTotalMs, worstLogicMs, worstRenderMs);
        if (slowFrames > 0)
            std::printf("[slow-frame] mean of the reported frames: logicMs=%.2f renderMs=%.2f "
                        "-- whichever dominates names the half to look in\n",
                        slowLogicMsTotal / (double)slowFrames,
                        slowRenderMsTotal / (double)slowFrames);
    }

    std::printf("[GameOS] main loop finished\n");

    // Opt-in long-run memory proof. gos_WalkMemoryHeap validates every live
    // block guard and reports the root heap's current/peak accounting when
    // MC2_MEMORY_STATS is set; normal runs retain their existing quiet output.
    const char* memoryStats = std::getenv("MC2_MEMORY_STATS");
    if (memoryStats && *memoryStats && std::strcmp(memoryStats, "0") != 0)
        gos_WalkMemoryHeap(nullptr, false);

    // What the game actually asked the renderer to do. This is the measurement
    // that sizes M3: the real WebGPU backend has to reproduce exactly the render
    // states listed here, and nothing else.
    //
    // PRINTED BEFORE SHUTDOWN, DELIBERATELY. It used to run after
    // TerminateGameEngine, which meant it never ran at all -- teardown currently
    // faults on a heap buffer overrun in game code, so the one measurement the
    // renderer work is planned against was being lost to an unrelated bug. It
    // also belongs here on the merits: it describes what the RUNNING game asked
    // for, not what shutdown did.
    gosRenderNull_DumpStats();
    gosAudioNull_DumpStats();
    Platform().DumpStats();
    // PORT (M8-B7): silent unless MC2_PAGE_GUARD is set. There are TWO shutdown
    // paths in this file and this is the one the -mission runs take; the report
    // was added to the other one first and printed nothing, which is how that
    // was found.
    mc2mem::PageGuardReport();

    // 4. Shutdown.
    std::printf("[GameOS] TerminateGameEngine ...\n");
    Environment.TerminateGameEngine();
    std::printf("[GameOS] TerminateGameEngine returned -- clean exit\n");

    mc2audio::AudioShutdown();
    Platform().Destroy();

    return 0;
#endif
}
