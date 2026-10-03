//===========================================================================//
// mc2_port.h -- the ONE header game code includes for port shims.
//
// Anything the port substitutes for a platform facility the game used to call
// directly belongs here. Game code includes this instead of hand-copying
// `extern` declarations, which is what it was doing: MC2_PortMessageBox was
// declared identically in six separate .cpp files, so changing its signature
// meant editing six places and finding out at link time if one was missed.
//
// Kept deliberately small and dependency-free -- no windows.h, no gameos.hpp --
// so it can be included from anywhere in the game tree without ordering
// problems, and so the Emscripten build inherits it unchanged.
//===========================================================================//

#ifndef MC2_PORT_H
#define MC2_PORT_H

//---------------------------------------------------------------------------//
// The game's simulation clock, published for the platform layer.
//
// WHY THIS EXISTS. MC2_WINDOW_CAPTURE_FRAME counts WINDOW PUMPS from window
// creation, and pumps run during mission loading too. How many pumps loading
// takes depends on disk cache and machine load, so "pump 195" lands at a
// different point in the mission from one hour to the next. That is not a
// theory: the framebuffer gate's references were regenerated, passed three
// times with a max block delta of ZERO, and an hour later the same command on
// the same binary differed by 73.5% of the frame. It had already been mistaken
// once for a rendering regression and once for a present-mode defect, and both
// times it was this.
//
// `turn` advances once per Mission::update and therefore measures SIMULATION
// PROGRESS, which is what a reproducible screenshot has to be anchored to.
// MC2_CAPTURE_TURN uses it. -1 means the mission has not started.
//---------------------------------------------------------------------------//
extern long MC2_PublishedTurn;

//---------------------------------------------------------------------------//
// Modal dialog replacement.
//
// Signature mirrors Win32 MessageBox (HWND first) so call sites are a straight
// textual substitution. The implementation prints the message and returns
// IDCANCEL (2) rather than blocking:
//
//   * A modal dialog hangs a headless run outright.
//   * ffile.cpp:112 loops `while (handle == NULL) { MessageBox(...); fopen(...); }`
//     so returning IDOK retries a missing file FOREVER. Only IDCANCEL escapes.
//   * Emscripten has no MessageBox at all, so every call site had to move
//     regardless of the above.
//
// Implemented in gameos/impl/gos_platform.cpp.
//---------------------------------------------------------------------------//
extern int MC2_PortMessageBox(void* hwnd, const char* text, const char* caption,
                              unsigned int type);

//---------------------------------------------------------------------------//
// Teardown census breadcrumb (M7 A1b).
//
// Mission teardown ends in a heap corruption that the CRT detects with
// __fastfail(FAST_FAIL_HEAP_CORRUPTION). That bypasses the exception filter, so
// there is no stack, no crash report and no last-line-of-output -- by design,
// not by omission. The only way in is a breadcrumb trail that has already been
// flushed by the time the process dies.
//
// Call this between the phases of a teardown. Each call prints a numbered line
// naming the phase, the GameOS live-block delta since the previous checkpoint,
// GameOS's own overrun-guard count, and whether the HOST heap still validates
// -- then flushes. The last line in the log is the phase that survived; the
// corruption is in the phase after it.
//
// SILENT AND FREE unless MC2_TEARDOWN_CENSUS is set to something other than 0:
// the env lookup is cached, so a disabled checkpoint is one predictable branch.
// It has to be free, because it is placed in code that runs every mission.
//
// Implemented in gameos/impl/gos_platform.cpp.
//---------------------------------------------------------------------------//
extern void MC2_TeardownCheckpoint(const char* phase);

//---------------------------------------------------------------------------//
// Is this global still pointing at something that exists? (M7 A1b)
//
// Both teardown crashes so far die on `delete` of a long-lived global, and both
// come one statement after a checkpoint that reported the host heap INTACT.
// That combination rules out a buffer overrun: an overrun damages block headers
// and HeapValidate sees it. A stale POINTER validates clean right up to the free
// that trips over it, and that is the remaining explanation.
//
// So ask the allocator, before deleting: is this address a live block? Call it
// immediately above a `delete` of anything that outlives a mission. It prints
// and returns; it never changes what the caller does, because a diagnostic that
// alters control flow stops being a diagnostic.
//
// "not live" is a lead, not a verdict -- anything not allocated through
// gos_Malloc was never in the table. Compare against a run that survived the
// same delete before concluding.
//
// Silent unless MC2_TEARDOWN_CENSUS is set. Implemented in gos_platform.cpp.
//---------------------------------------------------------------------------//
extern void MC2_TeardownExpectLive(const char* what, const void* p);

//---------------------------------------------------------------------------//
// Runtime heap census breadcrumb (M7 A1b, in-mission recurrence).
//
// Unlike the teardown census, this is placed only at infrequent state changes
// and sparse heartbeats in live gameplay. Each call validates the same two
// independent domains and flushes immediately, so a salvage/recovery repro can
// identify the last operation that left every heap intact.
//
// SILENT unless MC2_RUNTIME_HEAP_CENSUS is non-zero.
//---------------------------------------------------------------------------//
extern void MC2_RuntimeHeapCheckpoint(const char* phase);

//---------------------------------------------------------------------------//
// Parallel-region allocation proof.
//
// A worker brackets only the code intended to be allocation-free. Every
// gos_Malloc reached inside that bracket increments a process-wide counter.
// The object manager resets and reads it at the phase barrier; zero is the
// acceptance condition, while the old whole-update path is the positive
// control that proves the instrument can fire.
//---------------------------------------------------------------------------//
extern void MC2_SetWorkerAllocationProbe(bool active);
extern void MC2_ResetWorkerAllocationCount();
extern unsigned long long MC2_GetWorkerAllocationCount();

//---------------------------------------------------------------------------//
// Fixed-layout logistics canvas.
//
// The retail menus are authored at 800x600. Keeping that coordinate system
// while the GameOS renderer presents the player's actual mode lets the port
// scale and centre the complete UI without rewriting thousands of FIT entries.
// Passing zeroes disables the canvas before entering the tactical renderer.
//---------------------------------------------------------------------------//
extern void MC2_SetVirtualCanvas(unsigned int width, unsigned int height);

// Read back what is currently set, so a caller that must draw OUTSIDE the canvas
// can turn it off and put it back exactly as it was.
//
// The mission's 3D is the one thing that must never go through the canvas: it is
// already submitted in real backbuffer coordinates. It normally does not, but
// during the loading-screen handoff the world and the 800x600 load screen are
// drawn in the SAME frame, and one global transform cannot serve both. Zero in
// both means no canvas is active.
extern void MC2_GetVirtualCanvas(unsigned int* width, unsigned int* height);
extern bool MC2_IsVirtualCanvasActive();

#endif // MC2_PORT_H
