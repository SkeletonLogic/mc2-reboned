//===========================================================================//
// GameOS core -- environment, timing, rand, and the stop/exit path.
//
// Clean-room reimplementation from the contracts documented in
// Source/GameOS/include/*. No decompiled GameOS source was consulted.
//
// This covers the symbols the linker demanded that are not part of a larger
// subsystem: the gosEnvironment global the game reads constantly, the timing
// family, gos_rand/srand, and InternalFunctionStop/Pause which sit behind every
// STOP() and PAUSE() macro in the codebase.
//
// PORTABILITY: std C++ plus <windows.h> only where a Win32 handle type is
// unavoidable (hWindow). The timing path uses std::chrono rather than
// QueryPerformanceCounter so it carries to Emscripten unchanged.
//===========================================================================//

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <chrono>

#include <gameos.hpp>

// Declared in GameOS's ErrorHandler.hpp rather than GameOS.HPP. Forward-declared
// here instead of including that header, which drags in Win32 error-reporting
// machinery this module does not need.
void __stdcall ExitGameOS();

//---------------------------------------------------------------------------//
// The environment block. GameOS.HPP declares `extern gosEnvironment
// Environment;` and the game both reads and writes it -- screenWidth and
// screenHeight in particular are read on nearly every UI path.
//
// Zero-initialised, then given the defaults GameOS.HPP documents in its field
// comments. InitializeGameEngine overwrites most of these during startup.
//---------------------------------------------------------------------------//
gosEnvironment Environment = {};

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point g_startTime;
Clock::time_point g_lastFrameTime;
bool              g_timeInitialized = false;
unsigned int      g_randState = 1;
double            g_gameTime = 0.0;
double            g_realTime = 0.0;

void ensureTimeInit()
{
    if (g_timeInitialized) return;
    g_timeInitialized = true;
    g_startTime = Clock::now();
    g_lastFrameTime = g_startTime;

    // Defaults from the GameOS.HPP field comments; the game may override them.
    if (Environment.MaxTimeDelta    == 0.0f) Environment.MaxTimeDelta    = 1.0f;
    if (Environment.MinimumTimeDelta == 0.0f) Environment.MinimumTimeDelta = 1.0f / 30.0f;
}

} // namespace

//---------------------------------------------------------------------------//
// Timing
//---------------------------------------------------------------------------//

// GameOS.HPP: game time starts at zero and is updated once per game loop. Keep
// sampling out of this public getter so repeated calls within one tick observe
// the same value instead of consuming progressively smaller deltas.
void gosTiming_Reset()
{
    ensureTimeInit();
    g_lastFrameTime = Clock::now();
    g_gameTime = 0.0;
    g_realTime = 0.0;
}

// PORT (M8-B18): see mc2_port.h. Defined in GameOS so the platform layer
// always has it, and written by the game once per Mission::update.
long MC2_PublishedTurn = -1;

double gosTiming_BeginFrame(double fixedDeltaSeconds)
{
    ensureTimeInit();

    const Clock::time_point now = Clock::now();
    const double realDelta = std::chrono::duration<double>(now - g_lastFrameTime).count();
    g_lastFrameTime = now;

    double gameDelta = fixedDeltaSeconds > 0.0 ? fixedDeltaSeconds : realDelta;
    if (fixedDeltaSeconds <= 0.0 && Environment.MaxTimeDelta > 0.0f &&
        gameDelta > (double)Environment.MaxTimeDelta)
        gameDelta = (double)Environment.MinimumTimeDelta;

    g_realTime += realDelta;
    g_gameTime += gameDelta;
    return gameDelta;
}

double __stdcall gos_GetElapsedTime(int RealTime)
{
    ensureTimeInit();

    // There is no modal-script pause machinery yet, so both clocks advance on
    // every frame. The real clock deliberately bypasses MaxTimeDelta.
    return RealTime ? g_realTime : g_gameTime;
}

// A monotonically increasing cycle-ish counter. The game uses it only for
// relative profiling, so a nanosecond count is a faithful stand-in for rdtsc
// and, unlike rdtsc, is meaningful across cores and carries to Emscripten.
__int64 __stdcall GetCycles()
{
    ensureTimeInit();
    return (__int64)std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - g_startTime).count();
}

//---------------------------------------------------------------------------//
// Random numbers
//
// GameOS.HPP notes RAND_MAX == (1<<15)-1 on MSVC, and MCLib::RandomNumber
// depends on it: it computes (gos_rand()*range)>>15 and asserts the bound. So
// this must return 15-bit values regardless of the host CRT's RAND_MAX.
//---------------------------------------------------------------------------//
void __stdcall gos_srand(unsigned int seed)
{
    g_randState = seed ? seed : 1;
}

int __stdcall gos_rand()
{
    // Numerical Recipes LCG; the high bits are the well-behaved ones, so the
    // result is taken from bits 16..30 to yield the required 0..32767 range.
    g_randState = g_randState * 1664525u + 1013904223u;
    return (int)((g_randState >> 16) & 0x7FFFu);
}

//---------------------------------------------------------------------------//
// Stop / Pause
//
// STOP(x) and PAUSE(x) expand to these. They are NOT gated behind _ARMOR, so
// they are live in every configuration -- roughly 499 call sites across 120
// game files. The return value drives ENTER_DEBUGGER: returning zero means
// "do not break", which is what a non-interactive build wants.
//
// STOP is documented as not continuable, so it reports and terminates rather
// than returning into code that has already decided its invariants are broken.
//---------------------------------------------------------------------------//
int __cdecl InternalFunctionStop(const char* Message, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, Message);
    vsnprintf(buffer, sizeof(buffer), Message ? Message : "(null)", args);
    va_end(args);

    std::fprintf(stderr, "\n[GameOS STOP] %s\n", buffer);
    std::fflush(stderr);

#if defined(__EMSCRIPTEN__)
    // PORT: CONTAINMENT. On Win32 a STOP raised a dialog and a developer saw
    // it. Here ExitGameOS() calls std::exit(1): the wasm program stops, the
    // canvas keeps showing its last frame, and the player gets no message
    // anywhere -- indistinguishable from a hang. Measured 2026-08-24, a save
    // whose filename differed only in CASE took the entire game down this way,
    // and it read to the owner as "the game freezes on New Campaign".
    //
    // A STOP means an invariant is broken, which is not the same as
    // unrecoverable. Report it loudly -- shell log, the persistent fault record
    // that survives closing the tab, and a banner the player cannot miss --
    // then RETURN and let the game carry on. If it goes on to trap anyway, the
    // shell fault recorder catches that too, with a stack. Either outcome beats
    // a silent dead tab, and it is the difference between "it froze" and a
    // report that names the file.
    //
    // Rate-limited: a STOP inside a per-frame loop must not flood the page.
    static int reported = 0;
    if (reported < 16)
    {
        ++reported;
        EM_ASM({
            var text = UTF8ToString($0);
            var stack = "";
            try { stack = new Error().stack || ""; } catch (e) {}
            if (typeof window !== "undefined" && window.mc2RecordFault)
                window.mc2RecordFault("engine stop", text, stack);
            else if (typeof console !== "undefined")
                console.error("[GameOS STOP] " + text);
        }, buffer);
    }
    return 0;
#else
    ExitGameOS();
    return 0;
#endif
}

int __cdecl InternalFunctionPause(const char* Message, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, Message);
    vsnprintf(buffer, sizeof(buffer), Message ? Message : "(null)", args);
    va_end(args);

    // Documented as continuable: report and carry on.
    std::fprintf(stderr, "[GameOS PAUSE] %s\n", buffer);
    std::fflush(stderr);
    return 0;
}

//---------------------------------------------------------------------------//
// Shutdown
//---------------------------------------------------------------------------//
void __stdcall ExitGameOS()
{
    std::fflush(stdout);
    std::fflush(stderr);
    std::exit(1);
}
