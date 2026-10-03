//===========================================================================//
// Native platform seam. Implementations own windows and event translation;
// GameOS and renderers see only portable operations and opaque native handles.
//===========================================================================//

#pragma once

class PlatformBackend
{
public:
    virtual ~PlatformBackend() {}
    virtual const char* Name() const = 0;
    virtual bool Create(const char* title, int clientWidth, int clientHeight) = 0;
    virtual void Destroy() = 0;
    virtual bool PumpEvents() = 0;
    virtual bool HasWindow() const = 0;
    virtual void* NativeWindowHandle() const = 0;
    virtual void* NativeDisplayHandle() const = 0;
    virtual void ClientSize(int* width, int* height) const = 0;
    virtual void ClientScreenOffset(long* x, long* y) const = 0;
    virtual void SetGameScreenSize(int width, int height) = 0;
    virtual void SetFullscreen(bool fullscreen) = 0;
    virtual void SetMouseConfined(bool confined) = 0;
    virtual void DumpStats() const = 0;
};

PlatformBackend& Platform();
PlatformBackend* CreateWin32PlatformBackend();
PlatformBackend* CreateEmscriptenPlatformBackend();

// PORT: ask the host allocator whether its own bookkeeping is still intact.
//
// The teardown corruption (M7 A1b) is detected by the CRT, not by GameOS: the
// CRT heap raises __fastfail(FAST_FAIL_HEAP_CORRUPTION), which bypasses the
// exception filter, so there is no stack and no message by design. GameOS's
// trailing guards cannot see it either -- they cover gos_Malloc blocks only,
// and the block that gets overrun may be a plain new/malloc one.
//
// HeapValidate walks the host heap's block headers and free lists, which is the
// same bookkeeping __fastfail is complaining about, so calling it at checkpoints
// turns "the process vanished somewhere in teardown" into "the heap was intact
// at checkpoint N and broken at N+1".
//
// Returns 1 intact, 0 corrupt, -1 the host cannot answer.
int MC2_PlatformHeapIntact();

// PORT: is THIS address a block the host allocator currently holds allocated?
//
// MC2_PlatformHeapIntact answers a different question. It walks headers and free
// lists, so it reports intact right up to the moment someone frees a pointer that
// was already freed -- which is what every B1-1 crash looks like: a checkpoint
// says the heap is fine, and the very next free __fastfails. A whole-heap walk
// cannot see that, because the heap really is fine; it is the POINTER that is
// wrong.
//
// This asks about one block, and it cannot fault: HeapValidate with an address
// returns FALSE for a block that is not allocated rather than raising.
//
// Returns 1 allocated, 0 NOT allocated, -1 the host cannot answer.
//
// LIMIT, and it matters when reading a silent run: the low-fragmentation heap
// can leave a freed block inside a valid subsegment, so a 0 is strong evidence
// and a 1 is weak. This probe is designed to come back negative; a run in which
// it never fires has narrowed the field, not cleared it.
int MC2_PlatformHeapBlockAllocated(const void* p);

// PORT: copy a small diagnostic memory range without turning an unreadable
// range into a second crash. Used only by the env-gated B1-1 free cursor.
// Returns 1 when all requested bytes were copied, 0 otherwise.
int MC2_PlatformReadMemory(const void* address, void* out, unsigned int bytes);

// PORT: capture the current call stack, cheaply enough to run on every
// allocation while a diagnostic switch is armed.
//
// Return addresses alone are what a double-free report is missing: the freeing
// site is already on the crash stack, and it is the ALLOCATING site that names
// the subsystem holding the stale pointer.
//
// Frame 0 is the immediate caller. Returns how many frames were written.
int MC2_PlatformCaptureBacktrace(void** frames, int maxFrames);

// PORT: turn one captured return address into text, for the probe's report.
//
// Writes 'name+0xOFF (file:line)' when symbols are loaded and 'module+0xRVA'
// when they are not, so the line is still usable against a map file. Never
// fails: on any error the address itself is written.
void MC2_PlatformDescribeAddress(const void* address, char* out, unsigned int outBytes);

// PORT: size of the primary display in pixels, so gos_Info_ValidMode can answer
// honestly instead of accepting every mode. The options screen builds its
// resolution list by dropping modes this query rejects, and applyPrefs falls
// back when the saved pair is rejected -- both of which are no-ops while the
// answer is an unconditional yes.
//
// Returns false when the host cannot say (headless, browser). A caller that
// gets false must ACCEPT the mode: refusing on "don't know" would empty the
// options list and break the headless regression runs.
bool MC2_PlatformDisplaySize(int* width, int* height);
