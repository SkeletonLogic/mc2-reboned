#pragma once
// PORT: Browser timer declarations; implementations live in the Emscripten
// platform peer so legacy mouse/timing callers do not own the host policy.
#include <windows.h>

using MMRESULT = UINT;
using LPTIMECALLBACK = void (CALLBACK*)(UINT, UINT, DWORD, DWORD, DWORD);
#define TIME_PERIODIC 1

extern "C" DWORD timeGetTime();
extern "C" MMRESULT timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback,
	DWORD user, UINT eventType);
extern "C" MMRESULT timeKillEvent(UINT timerId);
