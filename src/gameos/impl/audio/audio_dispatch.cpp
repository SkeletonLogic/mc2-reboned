//===========================================================================//
// Audio backend selection -- selection only, no policy beyond the fallback.
//
// Mirrors renderer_select.cpp: MC2_AUDIO picks the backend, and ANY failure
// falls back to null rather than failing the run. Audio is never allowed to be
// fatal -- a silent game is a working game.
//
// THE DEFAULT IS SILENT, DELIBERATELY, AND IT MUST STAY THAT WAY.
// Every headless invocation in this project -- including the strict fixed-60
// regression, whose published hashes cover every byte of stdout -- runs without
// setting MC2_AUDIO. Defaulting to the device changes those runs twice over: it
// prints a backend line, and, worse, a device that really finishes a sound makes
// gosAudio_GetChannelPlayMode start returning Stop, which the game reads and
// acts on. Measured: the 200-tick strict stdout moved from 01420ac4546ac781 to
// 89adc835f2c37f50 with the device on. So sound is opt-in --
// tools/playtest/Play-MC2.cmd sets MC2_AUDIO=win32 for players, and every
// regression path keeps the silence it has always had.
//===========================================================================//

#include "audio/audio_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mc2audio {

namespace {
AudioBackend* g_backend = nullptr;
}

void AudioSelect()
{
    if (g_backend) return;

    const char* want = std::getenv("MC2_AUDIO");
    const bool  askedNull  = want && std::strcmp(want, "null")  == 0;
    const bool  askedWin32 = want && std::strcmp(want, "win32") == 0;
    const bool  askedWeb   = want && std::strcmp(want, "web")   == 0;

    (void)askedNull;
    (void)askedWeb;
#if defined(__EMSCRIPTEN__)
    // PORT (C2): in the browser the device is the DEFAULT, not opt-in. The
    // reason the native side is opt-in does not apply here -- the strict
    // fixed-60 regression whose published hashes cover every byte of stdout
    // runs natively, never under Emscripten, so nothing this choice affects is
    // hashed. MC2_AUDIO=null still forces silence.
    const bool preferDevice = !askedNull;
#else
    const bool preferDevice = askedWin32;   // opt-in only; see the note above
#endif

    if (preferDevice)
    {
#if defined(__EMSCRIPTEN__)
        AudioBackend* device = MakeWebAudioBackend();
        if (device->Open()) { g_backend = device; std::printf("[audio] backend = %s\n", device->Name()); return; }
        std::printf("[audio] %s unavailable; falling back to null\n", device->Name());
#elif defined(_WIN32)
        AudioBackend* device = MakeWin32AudioBackend();
        if (device->Open()) { g_backend = device; std::printf("[audio] backend = %s\n", device->Name()); return; }
        std::printf("[audio] %s unavailable; falling back to null\n", device->Name());
#else
        if (askedWin32) std::printf("[audio] win32 backend not built on this host; using null\n");
#endif
    }

    g_backend = MakeNullAudioBackend();
    g_backend->Open();
    // Deliberately quiet on the null path: the strict fixed-60 regression runs
    // here and its published hashes cover every byte of stdout.
}

AudioBackend& Audio()
{
    if (!g_backend) AudioSelect();
    return *g_backend;
}

void AudioShutdown()
{
    if (!g_backend) return;
    g_backend->Close();
    g_backend = nullptr;
}

} // namespace mc2audio
