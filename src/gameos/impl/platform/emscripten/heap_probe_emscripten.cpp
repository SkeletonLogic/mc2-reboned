//===========================================================================//
// Host-allocator probe helpers for B1-1 (M8-B7), browser build.
//
// A leaf translation unit, mirroring heap_probe_win32.cpp -- see the note
// there for why these must not live in the platform backend proper.
//===========================================================================//

#include <cstdio>

#include "platform/platform_backend.h"

// PORT: same limitation per block. dlmalloc cannot be asked whether one
// address is currently allocated without walking its arenas, so the browser
// build answers 'cannot tell'. The probe treats -1 as no information and stays
// silent, rather than reporting every block as suspect.
int MC2_PlatformHeapBlockAllocated(const void* /*p*/)
{
    return -1;
}

int MC2_PlatformReadMemory(const void* /*address*/, void* /*out*/, unsigned int /*bytes*/)
{
    return 0;
}

// PORT: no cheap in-process unwinder here. Returning zero frames makes the
// probe print its report without an allocation site rather than not print.
int MC2_PlatformCaptureBacktrace(void** /*frames*/, int /*maxFrames*/)
{
    return 0;
}

void MC2_PlatformDescribeAddress(const void* address, char* out, unsigned int outBytes)
{
    if (!out || outBytes == 0) return;
    std::snprintf(out, outBytes, "%p", address);
}
