//===========================================================================//
// gos_backend.h -- internal contracts BETWEEN GameOS backend modules.
//
// NOT part of the GameOS API. Game code never includes this; it only ever sees
// gameos.hpp (Microsoft's) and mc2_port.h (the port's shims). This header is
// how the backend's own translation units talk to each other, instead of
// scattering `extern` declarations inside function bodies where a signature
// change fails silently until link time.
//
// The split it documents is the one the port plan requires -- windowing/input/
// timing, audio, file I/O behind a VFS, renderer behind an interface -- so that
// swapping in SDL2, Dawn/WebGPU or Emscripten replaces a module rather than
// editing game code.
//===========================================================================//

#ifndef GOS_BACKEND_H
#define GOS_BACKEND_H

//---------------------------------------------------------------------------//
// gos_platform.cpp -- process entry, the GameOS-owned main loop, platform globals
//---------------------------------------------------------------------------//

// Provided by the GAME (Code/MechCmd2.cpp). GameOS calls it so the application
// can populate Environment and install its four loop callbacks.
extern void GetGameOSEnvironment(char* CommandLine);

// Set by gos_TerminateApplication (gos_system.cpp), read by the main loop.
// Ending the loop rather than calling exit() keeps TerminateGameEngine on the
// path -- which is the whole point of Milestone 1's shutdown leg.
extern bool g_gosQuitRequested;

//---------------------------------------------------------------------------//
// gos_core.cpp -- the one authoritative timing sample per GameOS frame.
//
// The public gos_GetElapsedTime API returns cumulative game time; the main loop
// owns clock sampling so repeated game calls cannot change the frame delta.
//---------------------------------------------------------------------------//
void gosTiming_Reset();
double gosTiming_BeginFrame(double fixedDeltaSeconds = 0.0);

//---------------------------------------------------------------------------//
// impl/renderer/ -- backend selection and diagnostics
//
// gosRenderer_Select picks the active backend (null or WebGPU) once at startup;
// the policy and its fallback live in renderer_select.cpp. It must run before
// InitializeGameEngine, because the game creates textures during data load.
//
// gosRenderNull_DumpStats prints what the game actually asked the renderer to do,
// via whichever backend is active. This is the measurement that sizes the real
// renderer: Phase 0 counted 691 gos_SetRenderState CALL SITES, but what a WebGPU
// backend must reproduce is the STATE SPACE, which at boot is 15 distinct states.
//---------------------------------------------------------------------------//
void gosRenderer_Select();
void gosRenderNull_DumpStats();

//---------------------------------------------------------------------------//
// audio/ -- the audio call census.
//
// PORT: M6-4 sizes the audio backend the same way M3 sized the renderer: by
// measuring what the game actually asks for rather than reading the 60-odd
// gosAudio_* declarations in the header. Gated on MC2_AUDIO_CENSUS, because the
// strict fixed-60 regression must stay byte-identical on the silent path.
//---------------------------------------------------------------------------//
void gosAudioNull_DumpStats();

//---------------------------------------------------------------------------//
// gos_input.cpp -- the backend feed.
//
// This is the ENTIRE seam a windowing layer (SDL2 today, browser events later)
// has to drive. Everything else in the input subsystem is derived state, so a
// new platform implements these three and nothing more.
//---------------------------------------------------------------------------//
void gosInput_SetMouse(float x, float y, int dx, int dy, int wheel, unsigned long buttons);
void gosInput_BeginFrame();
void gosInput_SetKey(int keyIndex, bool down);
void gosInput_PushKeyEvent(unsigned long key);

// PORT: display size, late-bound rather than called directly.
//
// gos_Info_ValidMode needs to know how big the display is, but gos_system.cpp
// must not REFERENCE the platform peer to find out: that pulls
// platform_win32.obj into every link, which pulls gos_platform.obj, which
// defines main() -- and the unit-test executables supply their own. This is a
// link error rather than a theory; it is exactly what happened when the query
// was written as a direct call.
//
// So the peer registers a provider when it is created and gos_system asks
// through it. No provider means "cannot say", which callers must read as
// "accept the mode" -- never as "reject".
typedef bool (*MC2_DisplaySizeFn)(int* width, int* height);
void MC2_SetDisplaySizeProvider(MC2_DisplaySizeFn provider);
bool MC2_QueryDisplaySize(int* width, int* height);

// Fixed-layout canvas transform shared by screen-mode, renderer and input.
bool gosCanvas_GetTransform(float* scale, float* offsetX, float* offsetY);
bool gosCanvas_Matches(unsigned int width, unsigned int height);
void gosCanvas_PhysicalToGame(float* x, float* y, int* dx, int* dy);
void gosCanvas_GameToPhysical(float* x, float* y);

#endif // GOS_BACKEND_H
