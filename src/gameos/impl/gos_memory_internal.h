//===========================================================================//
// Internal definition of the memory manager's heap object, plus test-only
// introspection.
//
// GameOS.HPP deliberately leaves the type opaque:
//     typedef struct gos_Heap* HGOSHEAP;
// so the concrete layout is ours. Nothing outside the GameOS backend includes
// this header -- game code only ever holds an HGOSHEAP.
//===========================================================================//

#pragma once

#include <cstddef>

struct gos_Heap
{
    char       name[64];

    gos_Heap*  parent;
    gos_Heap*  firstChild;
    gos_Heap*  nextSibling;

    size_t     maxBytes;    // 0 == unlimited (GameOS.HPP: MaximumSize)
    size_t     liveBytes;   // includes descendants
    size_t     liveCount;
    size_t     peakBytes;
    size_t     totalAllocs;

    bool       exceededMax;
};

//---------------------------------------------------------------------------//
// Diagnostics for the teardown census (M7 A1b).
//
// gos_WalkMemoryHeap prints a whole heap summary and returns void, so a caller
// that only wants to know "is anything overrun right now" cannot ask. These
// answer exactly that, so a checkpoint can run thirty times in a row without
// thirty summaries burying the one line that matters.
//
// CountCorruptBlocks scans every gos_Malloc block's trailing guard, reports each
// broken one by heap name the way gos_WalkMemoryHeap does, and returns the
// count. It covers GameOS blocks only -- the host heap is MC2_PlatformHeapIntact.
//---------------------------------------------------------------------------//
int    gos_memory_CountCorruptBlocks();
size_t gos_memory_TotalLiveBlocks();
size_t gos_memory_TotalLiveBytes();

// Is this address a block that is allocated RIGHT NOW?
//
// The side table exists because the user pointer is the malloc pointer (see the
// header of gos_memory.cpp), and that turns out to buy something the design did
// not set out to buy: the allocator can answer "is this pointer still live"
// without dereferencing it. A freed pointer is simply absent from the table.
//
// That is the one question a heap validator CANNOT answer. HeapValidate walks
// block headers and free lists, so it reports intact right up until the moment
// someone frees an already-freed pointer -- which is exactly what both M7 A1b
// crashes look like. Ask this BEFORE the delete and the answer arrives while the
// process is still alive.
//
// False negatives are possible and are not evidence of a bug: anything that did
// not come from gos_Malloc was never in the table. Read a "not live" as "look
// here", and compare against a run that survived the same delete.
bool gos_memory_IsLiveBlock(const void* p);

// User size of a live gos_Malloc block, or 0 if this address is not the start
// of one (M8-B7, 2026-08-18). Lets a caller check a copy length against the
// buffer it is about to read or write -- the ABL array assignment at
// Ablxstmt.cpp:304 copies by TYPE size and checks neither end.
size_t gos_memory_BlockSize(const void* p);

// Find the live gos_Malloc block CONTAINING this address, not merely one that
// starts at it (M8-B7, 2026-08-18). ABL array values live inside a larger
// record allocation, so a start-only lookup answers 0 for exactly the case
// worth reporting. Returns false if no live block contains the address.
//
// This scans the block table, so it is for diagnostics only -- never a hot
// path.
bool gos_memory_BlockExtent(const void* p, const void** base, size_t* userBytes);

// The STRONG signal. Returns the free serial number if this exact address was
// freed recently (within the last few thousand frees), or -1 if it was not.
//
// A hit is not ambiguous the way "not live" is: the address was allocated by
// gos_Malloc and then freed, so deleting it again is a double free, full stop.
long gos_memory_FreedAt(const void* p);

// How many DISTINCT threads have called gos_Malloc/gos_Free, and how many of
// those calls came from something other than the first thread seen.
//
// This allocator has no locking, so anything other than 1 and 0 is a defect --
// the block table is a global unordered_map whose own nodes come from malloc,
// and two threads inside it at once corrupts the CRT heap rather than merely
// the bookkeeping. That is the signature M7 A1b shows: a random crash site and
// a heap that validates clean moments earlier.
// Prove the B1-1 double-free probe is actually able to fire, in this process,
// on this run (M8-B7).
//
// A probe that never reports and a probe that cannot report produce identical
// logs, and the owner pays for the difference in playtime. This deliberately
// commits the suspected defect -- free() behind the allocator's back, then a
// gos_Free of the same pointer -- so an armed run carries its own proof that
// detection, reporting and symbolisation all worked. No-op when unarmed.
// Check EVERY live block against the host allocator (M8-B7, 2026-08-18).
//
// Not the same question as MC2_PlatformHeapIntact. That walks whole heaps, and
// it was MEASURED BLIND to a smashed block header that the per-block check
// caught -- so 'hostHeap=ok' does not mean the heap is sound, and the B1-1
// record should be read with that in mind. This runs the per-block check,
// which is the one that sees it. No-op unless MC2_GOS_FREE_PROBE is set.
void gos_memory_SweepBlocks(const char* why);

// Prove the PAGE GUARD can fault, independently of the free probe (M8-B7).
// It lived inside the probe self-test and was therefore skipped whenever the
// probe was off -- an armed guard run then proved nothing about a write.
void gos_memory_GuardSelfTest();

void gos_memory_ProbeSelfTest();

size_t gos_memory_ThreadsSeen();
size_t gos_memory_ForeignEvents();

//---------------------------------------------------------------------------//
// Test-only introspection.
//
// The GameOS API exposes no way to observe accounting, so the unit harness
// reads it through these. Keeping them in a separate namespace and header makes
// it obvious they are not part of the GameOS surface the game links against.
//---------------------------------------------------------------------------//
namespace gos_memory_test {

HGOSHEAP RootHeap();
int      StackDepth();

size_t   LiveBytes(HGOSHEAP h);
size_t   LiveCount(HGOSHEAP h);
bool     ExceededMax(HGOSHEAP h);

size_t   OverflowEvents();
size_t   CorruptionEvents();
size_t   LeakEvents();
size_t   StackErrorEvents();
size_t   ForeignFreeEvents();

void     ResetCounters();

} // namespace gos_memory_test
