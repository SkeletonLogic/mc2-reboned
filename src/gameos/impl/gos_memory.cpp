//===========================================================================//
// GameOS memory manager -- clean-room reimplementation.
//
// Written against the contracts documented in Source/GameOS/include/GameOS.HPP
// (lines ~1028-1100) and nothing else. No decompiled GameOS source was read.
//
// This is subsystem 1 of the GameOS backend and it is on the critical path:
// MCLib's UserHeap now routes every allocation through gos_Malloc/gos_Free
// (see Heap.h), because UserHeap's own allocator was hand-written x86 assembly
// that no 64-bit toolchain can build.
//
// PORTABILITY: this file talks to malloc/free, the C++ library, and the
// declaration-only platform seam in platform/platform_backend.h -- no
// windows.h, no SDL. The seam is reached only from the env-gated B1-1 probe
// below, and both backends implement it, so the web build still needs no
// second copy of this file.
//
//===========================================================================//
// WHY THERE IS NO HEADER IN FRONT OF THE USER POINTER
//
// The obvious implementation puts per-block metadata immediately before the
// returned pointer. This one deliberately does not, and that is a correctness
// decision rather than a stylistic one.
//
// With a prefix header the pointer handed to the caller is NOT the pointer
// malloc returned. Any code that frees a GameOS allocation with plain free() or
// delete then hands the CRT an address some bytes inside a block, and the debug
// CRT aborts:
//
//     debug_heap.cpp(904) : Assertion failed: _CrtIsValidHeapPointer(block)
//
// That is exactly what happened during M2 bring-up, inside mission->destroy().
// Worse, the assertion raises a MODAL DIALOG, so the failure presented as a
// process blocked on a syscall with ~0 CPU and no output -- indistinguishable
// from a deadlock. It cost several debugging cycles chasing thread and timer
// theories before the dialog itself was identified.
//
// MC2 is 322k lines of 2001-era code mixing malloc/free, new/delete, its own
// UserHeap, and gos_Malloc -- and MCLib's heaps now all route through GameOS.
// Mismatched alloc/free pairs are therefore not hypothetical; there will be
// more, in paths not yet reached.
//
// So: THE USER POINTER IS THE MALLOC POINTER, and metadata lives in a side
// table. Consequences, all of them right for a port:
//   * free()/delete on a GameOS block is harmless instead of fatal.
//   * gos_Free on a foreign pointer is DETECTED by a table miss, rather than
//     guessed at from a magic number read out of memory the CRT may already
//     have recycled -- the previous check was itself a use-after-free.
//   * Accounting can drift if a block is freed behind our back. That surfaces
//     as a leak in gos_WalkMemoryHeap, not as corruption.
//
// Cost is one hash lookup per free. gos_Malloc/gos_Free are not the hot path --
// gos_GetCurrentHeap is, at 219 call sites, and it is untouched by this.
//===========================================================================//

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gameos.hpp>

#include "gos_memory_internal.h"
#include "platform/platform_backend.h"
#include "mem/page_guard.h"

namespace {

thread_local bool g_workerAllocationProbe = false;
std::atomic<unsigned long long> g_workerAllocationCount(0);

// A trailing guard word, so gos_WalkMemoryHeap can detect the overrun the
// GameOS header explicitly promises to catch.
const uint32_t kBackGuard = 0xDEADBEEFu;

struct BlockInfo
{
    gos_Heap* heap;
    size_t    userBytes;
};

typedef std::unordered_map<void*, BlockInfo> BlockTable;

BlockTable& blocks()
{
    static BlockTable* t = new BlockTable();
    return *t;
}

inline void writeBackGuard(void* p, size_t userBytes)
{
    uint32_t g = kBackGuard;
    std::memcpy(static_cast<uint8_t*>(p) + userBytes, &g, sizeof(g));
}

inline bool backGuardIntact(const void* p, size_t userBytes)
{
    uint32_t g = 0;
    std::memcpy(&g, static_cast<const uint8_t*>(p) + userBytes, sizeof(g));
    return g == kBackGuard;
}

// PORT: the lock this allocator never had (M7 A1b).
//
// The block table is a global unordered_map mutated on every alloc and free, and
// its own nodes come from malloc. Two threads inside it corrupts the CRT heap
// itself, which lands on whatever free the main thread reaches next -- a random
// crash site with a heap that validates clean moments earlier. That is exactly
// the A1b signature, at three different sites so far.
//
// Whether anything actually races here is still being measured (see the thread
// census below). The lock goes in regardless: an unlocked allocator in a process
// that runs game code from a multimedia timer callback is a defect on its own
// terms, and the header's claim that this is cheap is the reason it is cheap to
// fix -- gos_Malloc/gos_Free are NOT the hot path. gos_GetCurrentHeap is, at 219
// call sites, and it stays lock-free and untouched.
std::mutex& tableLock()
{
    static std::mutex m;
    return m;
}

// PORT: which threads call in here? (M7 A1b)
//
// Kept after the lock went in, because it answers a different question: the lock
// makes a race safe, this says whether one was ever happening. If the crash
// stops AND this still reports one thread, the lock was not the fix and the
// search moves on rather than declaring victory.
//
// No allocation and no locking of its own -- a fixed array and a linear scan
// over at most a handful of ids, all under the table lock already held.
const int kMaxSeenThreads = 8;

std::thread::id g_seenThread[kMaxSeenThreads];
int             g_seenThreadCount = 0;
size_t          g_foreignEvents   = 0;

// Returns true if this call came from a thread other than the first one seen.
bool noteCallingThread()
{
    const std::thread::id self = std::this_thread::get_id();
    for (int i = 0; i < g_seenThreadCount; ++i)
        if (g_seenThread[i] == self) return i != 0;

    if (g_seenThreadCount < kMaxSeenThreads)
        g_seenThread[g_seenThreadCount++] = self;
    return g_seenThreadCount > 1;
}

// PORT: a ring of recently freed addresses, for the M7 A1b teardown probe.
//
// "Not in the live table" is a WEAK signal -- anything that never came from
// gos_Malloc was never in it, and a healthy run already reports four of the
// teardown globals that way. This is the strong one: if an address is in here,
// it was definitely allocated by us and definitely freed, and freeing it again
// is a double free rather than a guess.
//
// A ring rather than a set, because this must not grow without bound on a path
// that frees tens of thousands of blocks per mission. 4096 covers the whole of a
// teardown, which is the only window it is read in.
const int kFreedRing = 4096;

struct FreedBlock
{
    const void* address;
    long        serial;     // which free this was, so the order is visible
};

FreedBlock g_freed[kFreedRing];
int        g_freedNext = 0;
long       g_freeSerial = 0;

void noteFreed(const void* p)
{
    g_freed[g_freedNext].address = p;
    g_freed[g_freedNext].serial  = ++g_freeSerial;
    g_freedNext = (g_freedNext + 1) % kFreedRing;
}

//---------------------------------------------------------------------------//
// PORT: the B1-1 double-free probe (M8-B7).
//
// WHAT THE EVIDENCE SAYS, and why this is the shape of the instrument.
//
// The 2026-08-18 crash faulted inside RtlFreeHeap, reached from the free loop
// below, one line after checkpoint 882 reported BOTH gosCorrupt=0 (every
// trailing guard in this allocator intact) AND hostHeap=ok (HeapValidate over
// every heap in the process). An overrun should have failed one of those two
// walks. A pointer that is not a live heap block fails neither, and then
// __fastfails at the free -- which is what was observed, at five sites now.
//
// This file's own design note describes the mechanism that produces exactly
// that: free()/delete on a GameOS block is called harmless instead of fatal,
// and accounting is allowed to drift if a block is freed behind our back.
// Harmless is true of that free itself. It is NOT true afterwards -- the side
// table still holds the pointer, so teardown frees it a second time. The freed
// ring cannot catch it either: a free that never came through gos_Free was
// never rung.
//
// So the probe asks the HOST allocator, per pointer, immediately before the
// free, and on a negative answer reports the block and DOES NOT FREE IT. A leak
// is survivable and a double free is not: skipping keeps the process alive so
// one play session can report every bad block instead of only the first.
//
//   MC2_GOS_FREE_PROBE=1  check every free, report and skip bad ones
//   MC2_GOS_FREE_PROBE=2  also record an allocation stack per block, so the
//                         report names WHO allocated the block that was freed
//                         behind our back. Costs a backtrace per gos_Malloc.
//
// A SILENT RUN IS A RESULT: it says every pointer is still a block the host
// considers allocated, which moves the search off the double-free explanation.
// Read the limit on MC2_PlatformHeapBlockAllocated before believing a silence.
//---------------------------------------------------------------------------//
// Three of these are eaten by the allocator itself (noteAllocSite, gos_Malloc,
// and MCLib's UserHeap wrapper above it), which the self-test made visible.
const int kProbeFrames = 12;

int probeLevel()
{
    static const int level = []() -> int {
        const char* env = std::getenv("MC2_GOS_FREE_PROBE");
        const int v = (env && *env) ? std::atoi(env) : 0;
        if (v > 0)
        {
            std::fprintf(stderr,
                "[gos-free-probe] ARMED at level %d. Every gos_Malloc block is checked\n"
                "[gos-free-probe] against the host allocator immediately before it is freed.\n"
                "[gos-free-probe] A block the host does not consider allocated is REPORTED AND\n"
                "[gos-free-probe] NOT FREED, so a double free becomes a named leak instead of a\n"
                "[gos-free-probe] __fastfail. Silence means every pointer looked allocated.\n", v);
            std::fflush(stderr);
        }
        return v < 0 ? 0 : v;
    }();
    return level;
}

// Allocation stacks, kept OUT of BlockInfo on purpose: at level 0 this table is
// never touched, so an unarmed run allocates exactly what it always did and the
// strict bar cannot move.
struct AllocSite
{
    void* frames[kProbeFrames];
    int   count;
};

typedef std::unordered_map<const void*, AllocSite> SiteTable;

SiteTable& sites()
{
    static SiteTable* t = new SiteTable();
    return *t;
}

void noteAllocSite(const void* p)
{
    if (probeLevel() < 2) return;
    AllocSite site;
    site.count = MC2_PlatformCaptureBacktrace(site.frames, kProbeFrames);
    sites()[p] = site;
}

void forgetAllocSite(const void* p)
{
    if (probeLevel() < 2) return;
    sites().erase(p);
}

// PORT: B1-1 pre-free cursor, added after the 2026-08-18 sweep run.
//
// That run completed a per-block sweep at checkpoint 1305, then RtlFreeHeap
// fast-failed while destroyRecursive freed the sole AblStackHeap block. The
// preserved texture crash dump had anomalous bytes in the current block's LFH
// header, but it was captured after RtlFreeHeap entered its corrupt path. This
// cursor distinguishes damage that already exists after HeapValidate from
// bytes changed by forgetAllocSite or RtlFreeHeap itself.
//
// MC2_GOS_FREE_CURSOR_MAX=<MaximumSize> limits the writes to one named heap by
// its configured capacity. 511999 is AblStackHeap. The file is overwritten at
// the start of each matching free and unbuffered, so the first complete record
// survives __fastfail. Off means byte-for-byte unchanged normal runs.
size_t destroyFreeCursorMax()
{
    static const size_t target = []() -> size_t {
        const char* env = std::getenv("MC2_GOS_FREE_CURSOR_MAX");
        if (!env || !*env) return 0;
        const unsigned long long value = std::strtoull(env, NULL, 10);
        return static_cast<size_t>(value);
    }();
    return target;
}

std::FILE* destroyFreeCursorFile()
{
    static std::FILE* f = []() -> std::FILE* {
        if (destroyFreeCursorMax() == 0) return NULL;
        std::FILE* h = std::fopen("gos-free-cursor.txt", "w");
        if (h) std::setvbuf(h, NULL, _IONBF, 0);
        return h;
    }();
    return f;
}

void printCursorBytes(std::FILE* f, const char* label, const void* address,
                      unsigned int bytes)
{
    uint8_t data[32] = {};
    if (bytes > sizeof(data)) bytes = sizeof(data);
    std::fprintf(f, "%s address=%p bytes=%u", label, address, bytes);
    if (!MC2_PlatformReadMemory(address, data, bytes))
    {
        std::fprintf(f, " <unreadable>\n");
        return;
    }
    for (unsigned int i = 0; i < bytes; ++i) std::fprintf(f, " %02X", data[i]);
    std::fprintf(f, "\n");
}

bool cursorMatches(const gos_Heap* heap)
{
    const size_t target = destroyFreeCursorMax();
    return target != 0 && heap && heap->maxBytes == target;
}

void recordDestroyFreeCursorValidated(const gos_Heap* heap, void* p,
                                      size_t userBytes, size_t index, size_t total)
{
    if (!cursorMatches(heap)) return;
    std::FILE* f = destroyFreeCursorFile();
    if (!f) return;

    std::rewind(f);
    std::fprintf(f,
        "BEGIN CURRENT RECORD stage=validated heap=%s maxBytes=%zu block=%zu/%zu "
        "ptr=%p userBytes=%zu\n",
        heap->name, heap->maxBytes, index + 1, total, p, userBytes);
    printCursorBytes(f, "front-16", static_cast<const uint8_t*>(p) - 16, 16);
    printCursorBytes(f, "tail+32", static_cast<const uint8_t*>(p) + userBytes, 32);

    SiteTable::const_iterator site = sites().find(p);
    if (site != sites().end())
    {
        std::fprintf(f, "allocation-frames=%d", site->second.count);
        for (int i = 0; i < site->second.count; ++i)
            std::fprintf(f, " %p", site->second.frames[i]);
        std::fprintf(f, "\n");
    }
    else
    {
        std::fprintf(f, "allocation-frames=0\n");
    }
}

void recordDestroyFreeCursorPreFree(const gos_Heap* heap, void* p, size_t userBytes)
{
    if (!cursorMatches(heap)) return;
    std::FILE* f = destroyFreeCursorFile();
    if (!f) return;

    std::fprintf(f, "stage=immediately-before-std-free\n");
    printCursorBytes(f, "front-16", static_cast<const uint8_t*>(p) - 16, 16);
    printCursorBytes(f, "tail+32", static_cast<const uint8_t*>(p) + userBytes, 32);
    std::fprintf(f, "END CURRENT RECORD\n");
}

// Returns true if the block is safe to free. Reports and returns false if the
// host says it is not allocated. An unanswerable host (-1) is treated as safe:
// the browser build must not start leaking every block it tears down.
// Set only while the startup self-test runs, so its deliberate report is
// labelled. An armed log contains exactly one of these; a second one is real.
bool g_probeSelfTesting = false;

void reportBadBlock(const char* headline, const gos_Heap* heap, void* p,
                    size_t userBytes, size_t index, size_t total)
{
    const long freedAt = gos_memory_FreedAt(p);

    std::fprintf(stderr,
        "[gos-free-probe] *** %s ***%s\n"
        "[gos-free-probe]   heap=%s  block %zu of %zu  ptr=%p  userBytes=%zu\n"
        "[gos-free-probe]   backGuard=%s\n",
        headline,
        g_probeSelfTesting ? "  (SELF-TEST, expected)" : "",
        heap ? heap->name : "<none>", index + 1, total, p, userBytes,
        backGuardIntact(p, userBytes) ? "intact" : "BROKEN");

    if (freedAt >= 0)
        std::fprintf(stderr,
            "[gos-free-probe]   it WAS freed through gos_Free, serial %ld -- so this is a\n"
            "[gos-free-probe]   double free inside this allocator, not a foreign one.\n", freedAt);
    else
        std::fprintf(stderr,
            "[gos-free-probe]   never freed through gos_Free, so something released it with\n"
            "[gos-free-probe]   plain free()/delete and left the side table holding it.\n");

    if (probeLevel() >= 2)
    {
        SiteTable::const_iterator it = sites().find(p);
        if (it != sites().end() && it->second.count > 0)
        {
            std::fprintf(stderr, "[gos-free-probe]   allocated by:\n");
            for (int f = 0; f < it->second.count; ++f)
            {
                char text[512];
                MC2_PlatformDescribeAddress(it->second.frames[f], text, sizeof(text));
                std::fprintf(stderr, "[gos-free-probe]     #%d %s\n", f, text);
            }
        }
        else
        {
            std::fprintf(stderr,
                "[gos-free-probe]   no allocation stack recorded (allocated before the probe\n"
                "[gos-free-probe]   was armed, or the host has no unwinder).\n");
        }
    }

    std::fflush(stderr);
}

// PORT: B1-1. A guarded block is NOT a CRT heap block, so it goes back to the
// guard, and it must never be shown to the host-allocator checks -- those
// would answer 'not allocated' for every guarded block and the probe would
// refuse every free. PageGuardOwns is an address-range test, so this costs
// one comparison when the guard is off.
void releaseBlock(void* p)
{
    if (mc2mem::PageGuardOwns(p)) { mc2mem::PageGuardFree(p); return; }
    std::free(p);
}

// Returns true if the block is safe to free.
bool probeBlockBeforeFree(const gos_Heap* heap, void* p, size_t userBytes,
                          size_t index, size_t total)
{
    if (probeLevel() < 1) return true;
    if (mc2mem::PageGuardOwns(p)) return true;   // the guard owns it, not the CRT
    if (MC2_PlatformHeapBlockAllocated(p) != 0) return true;
    reportBadBlock("NOT AN ALLOCATED BLOCK -- SKIPPING THE FREE", heap, p,
                   userBytes, index, total);
    return false;
}

//---------------------------------------------------------------------------//
// PORT: the whole-table sweep (M8-B7, 2026-08-18).
//
// WHY A SWEEP, when there is already a check on every free. The owner's run
// had the per-free check APPROVE the pointer and RtlFreeHeap __fastfail on it
// immediately afterwards, in MC_TextureManager::flush. Two controls explain
// how that is possible and say what to do about it:
//
//   * The single-block check DOES see a block whose own header was smashed.
//     (Measured: overrun a 64-byte block's neighbour, ask about the neighbour,
//     answer 0.)
//   * The WHOLE-HEAP walk -- the hostHeap=ok every checkpoint in this project
//     prints -- was BLIND to that same smash and reported intact. So every
//     'the heap validated clean moments earlier' in the B1-1 record is weaker
//     than it reads, INCLUDING the framing of B1-1 itself.
//
// So the detector to run is the per-block check over EVERY live block, not the
// whole-heap walk. This does that, and reports each bad block once with its
// allocation stack at level 2.
//
// IT CAN DIE MID-SWEEP. Measured: on 200 / 5000 / 300000-byte blocks with a
// smashed successor header, HeapValidate itself __fastfails rather than
// returning FALSE. That is still a finding -- crashwatch will show the stack
// inside here -- but it does not say WHICH block. Set MC2_GOS_SWEEP_CURSOR=1
// to record the candidate to a file before each check; it costs one unbuffered
// write per block, so it is off by default.
//---------------------------------------------------------------------------//
std::FILE* sweepCursorFile()
{
    static std::FILE* f = []() -> std::FILE* {
        const char* want = std::getenv("MC2_GOS_SWEEP_CURSOR");
        if (!want || !*want || std::strcmp(want, "0") == 0) return NULL;
        std::FILE* h = std::fopen("sweep-cursor.txt", "w");
        if (h) std::setvbuf(h, NULL, _IONBF, 0);   // must survive a __fastfail
        return h;
    }();
    return f;
}

size_t g_sweepBad = 0;
bool   sweepSelfTestExpected = false;

// Caller must already hold tableLock().
void sweepAllBlocksLocked(const char* why)
{
    if (probeLevel() < 1) return;

    const size_t total = blocks().size();
    size_t index = 0;
    size_t bad = 0;

    for (BlockTable::const_iterator it = blocks().begin(); it != blocks().end();
         ++it, ++index)
    {
        if (std::FILE* cursor = sweepCursorFile())
        {
            std::rewind(cursor);
            std::fprintf(cursor, "sweep '%s' block %zu of %zu ptr=%p heap=%s bytes=%zu\n",
                         why ? why : "(unnamed)", index + 1, total, it->first,
                         it->second.heap ? it->second.heap->name : "<none>",
                         it->second.userBytes);
        }

        if (mc2mem::PageGuardOwns(it->first)) continue;   // guarded, not CRT
        if (MC2_PlatformHeapBlockAllocated(it->first) != 0) continue;

        ++bad;
        reportBadBlock("SWEEP FOUND A BLOCK THE HOST NO LONGER HOLDS",
                       it->second.heap, it->first, it->second.userBytes, index, total);
    }

    if (bad)
    {
        g_sweepBad += bad;
        std::fprintf(stderr,
            "[gos-free-probe] SWEEP at '%s': %zu of %zu live blocks are bad.%s\n",
            why ? why : "(unnamed)", bad, total,
            sweepSelfTestExpected ? "  (SELF-TEST, expected)" : "");
        std::fflush(stderr);
    }
}

// How many gos_Free calls between sweeps. 0 disables the free-driven sweep;
// the checkpoint-driven one still runs.
size_t sweepEvery()
{
    static const size_t every = []() -> size_t {
        const char* env = std::getenv("MC2_GOS_SWEEP_EVERY");
        const long v = (env && *env) ? std::atol(env) : 0;
        return v > 0 ? (size_t)v : 0;
    }();
    return every;
}

// The heap stack. gos_GetCurrentHeap has 219 call sites and push/pop 295, so
// this is the hottest path in the whole port: a fixed array plus a depth index,
// no allocation and no locking. Overflow is a hard error rather than a growth
// event -- an unbounded push/pop imbalance is a bug, not a capacity problem.
const int kMaxHeapDepth = 64;

struct MemoryState
{
    gos_Heap*  stack[kMaxHeapDepth];
    int        depth;
    gos_Heap*  root;
    bool       initialized;

    // Diagnostics surfaced to the test harness; never consulted by game code.
    size_t     overflowEvents;
    size_t     corruptionEvents;
    size_t     leakEvents;
    size_t     stackErrorEvents;
    size_t     foreignFreeEvents;
};

MemoryState g_state;

void report(const char* what, const char* detail)
{
    std::fprintf(stderr, "[gos_memory] %s: %s\n", what, detail ? detail : "");
}

// Charge/credit an allocation to a heap and every ancestor, so a parent's
// totals include its children -- which is what makes the heap tree useful.
void chargeUp(gos_Heap* heap, size_t bytes)
{
    for (gos_Heap* h = heap; h; h = h->parent)
    {
        h->liveBytes   += bytes;
        h->liveCount   += 1;
        h->totalAllocs += 1;
        if (h->liveBytes > h->peakBytes) h->peakBytes = h->liveBytes;
        if (h->maxBytes != 0 && h->liveBytes > h->maxBytes)
        {
            h->exceededMax = true;
            g_state.overflowEvents++;
            report("heap exceeded MaximumSize", h->name);
        }
    }
}

void creditUp(gos_Heap* heap, size_t bytes)
{
    for (gos_Heap* h = heap; h; h = h->parent)
    {
        if (h->liveBytes >= bytes) h->liveBytes -= bytes; else h->liveBytes = 0;
        if (h->liveCount > 0)      h->liveCount -= 1;
    }
}

gos_Heap* newHeapNode(const char* name, size_t maxBytes, gos_Heap* parent)
{
    gos_Heap* h = static_cast<gos_Heap*>(std::calloc(1, sizeof(gos_Heap)));
    if (!h) return nullptr;

    if (name)
    {
        std::strncpy(h->name, name, sizeof(h->name) - 1);
        h->name[sizeof(h->name) - 1] = '\0';
    }
    else
    {
        std::strcpy(h->name, "<unnamed>");
    }

    h->maxBytes = maxBytes;
    h->parent   = parent;
    if (parent)
    {
        h->nextSibling     = parent->firstChild;
        parent->firstChild = h;
    }
    return h;
}

void ensureInit()
{
    if (g_state.initialized) return;
    g_state.initialized = true;
    g_state.root = newHeapNode("ClientHeap", 0, nullptr);
    g_state.depth = 0;
    g_state.stack[0] = g_state.root;
}

bool isDescendantOf(const gos_Heap* node, const gos_Heap* ancestor)
{
    for (const gos_Heap* h = node; h; h = h->parent)
        if (h == ancestor) return true;
    return false;
}

// Walks every live block belonging to `heap` or its descendants. With the side
// table this is a scan rather than a per-heap list walk; heap walking is a
// diagnostic operation, so trading its cost for mismatch tolerance is right.
int walkHeapRecursive(gos_Heap* heap, bool vociferous)
{
    int bad = 0;
    for (BlockTable::const_iterator it = blocks().begin(); it != blocks().end(); ++it)
    {
        if (!isDescendantOf(it->second.heap, heap)) continue;

        if (!backGuardIntact(it->first, it->second.userBytes))
        {
            report("buffer overrun detected in heap", it->second.heap->name);
            g_state.corruptionEvents++;
            bad++;
        }
        if (vociferous)
        {
            std::fprintf(stderr, "[gos_memory]   %s: block %p, %zu bytes\n",
                         it->second.heap->name, it->first, it->second.userBytes);
        }
    }
    return bad;
}

// Frees a heap's blocks and its children's, depth first.
void destroyRecursive(gos_Heap* heap, bool shouldBeEmpty)
{
    gos_Heap* c = heap->firstChild;
    while (c)
    {
        gos_Heap* nextSib = c->nextSibling;
        destroyRecursive(c, shouldBeEmpty);
        c = nextSib;
    }

    // Collect this heap's blocks first; erasing while iterating is not safe.
    std::vector<void*> mine;
    for (BlockTable::const_iterator it = blocks().begin(); it != blocks().end(); ++it)
        if (it->second.heap == heap) mine.push_back(it->first);

    if (shouldBeEmpty && !mine.empty())
    {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "%s (%zu block(s), %zu bytes still allocated)",
                      heap->name, mine.size(), heap->liveBytes);
        report("heap destroyed while not empty", msg);
        g_state.leakEvents++;
    }

    size_t skipped = 0;
    for (size_t i = 0; i < mine.size(); ++i)
    {
        BlockTable::iterator it = blocks().find(mine[i]);
        if (it == blocks().end()) continue;
        const size_t userBytes = it->second.userBytes;
        if (heap->parent) creditUp(heap->parent, userBytes);
        blocks().erase(it);

        // PORT: B1-1. A table entry does NOT mean the host still holds the
        // block; see the probe's note above. Ask before freeing -- and ask
        // BEFORE forgetAllocSite, which would otherwise erase the allocation
        // stack the report is printed from. (The self-test caught that.)
        const bool safe = probeBlockBeforeFree(heap, mine[i], userBytes, i, mine.size());
        recordDestroyFreeCursorValidated(heap, mine[i], userBytes, i, mine.size());
        forgetAllocSite(mine[i]);
        if (!safe)
        {
            ++skipped;
            continue;
        }
        recordDestroyFreeCursorPreFree(heap, mine[i], userBytes);
        releaseBlock(mine[i]);
    }

    if (skipped)
    {
        std::fprintf(stderr,
            "[gos-free-probe] heap %s: %zu of %zu blocks were NOT freed because the host\n"
            "[gos-free-probe] did not consider them allocated. The process is still alive.\n",
            heap->name, skipped, mine.size());
        std::fflush(stderr);
    }

    if (heap->parent)
    {
        gos_Heap** link = &heap->parent->firstChild;
        while (*link && *link != heap) link = &(*link)->nextSibling;
        if (*link == heap) *link = heap->nextSibling;
    }
    std::free(heap);
}

} // namespace

//---------------------------------------------------------------------------//
// The ParentClientHeap global declared by GameOS.HPP. It is the default parent
// for new heaps and the bottom of the heap stack.
//---------------------------------------------------------------------------//
HGOSHEAP ParentClientHeap = nullptr;

//---------------------------------------------------------------------------//

// PORT: B1-1 probe self-test. See gos_memory_internal.h for why this runs.
//
// It reproduces the suspected defect exactly: a gos_Malloc block released
// with plain free() behind the allocator's back, leaving the side table
// holding a pointer the host no longer considers allocated. Without the probe
// the gos_Free below would be the __fastfail being hunted; with it, the run
// prints the report and carries on.
//
// NOT gated behind a second switch on purpose. An armed run that does not
// contain this proof did not have a working instrument, and that has to be
// visible in the log rather than assumed.
void gos_memory_SweepBlocks(const char* why)
{
    std::lock_guard<std::mutex> guard(tableLock());
    sweepAllBlocksLocked(why);
}

void gos_memory_GuardSelfTest()
{
    // PORT: B1-1. If the page guard is armed, prove IT can fault too. A band
    // that silently fell back to malloc looks exactly like a band that found
    // nothing, and the difference is a whole playthrough.
    if (!mc2mem::PageGuardEnabled()) return;
    {
        void* g = gos_Malloc(32, NULL);
        if (!mc2mem::PageGuardOwns(g))
        {
            mc2mem::PageGuardNoteSelfTest(0, 0);
            std::fprintf(stderr,
                "[page-guard] SELF-TEST FAILED: a fresh gos_Malloc block is not\n"
                "[page-guard] guarded. The band is NOT covering gos_Malloc. This run\n"
                "[page-guard] cannot prove anything about a write.\n");
        }
        else
        {
            // Ask about the NEXT PAGE, not about block end + 1. The guard is
            // flush to alignment rather than to the byte, so a small block has
            // slack before its guard page -- measured, and a short overrun
            // within that slack is NOT caught. Report the slack so the limit is
            // visible instead of implied.
            const size_t pageSize = 4096;
            const size_t slack = pageSize - ((size_t)(uintptr_t)g + 32 + sizeof(uint32_t)) % pageSize;
            const int overrun = mc2mem::PageGuardAddressInaccessible(
                g, 32 + sizeof(uint32_t) + slack);
            gos_Free(g);
            const int afterFree = mc2mem::PageGuardAddressInaccessible(g, 0);
            std::fprintf(stderr,
                "[page-guard] SELF-TEST: guard page blocked=%d  freed block blocked=%d\n"
                "[page-guard] Both must be 1. A 0 means that class of access would go\n"
                "[page-guard] through unnoticed and a silent run proves nothing about it.\n",
                overrun, afterFree);
            mc2mem::PageGuardNoteSelfTest(overrun, afterFree);
            std::fprintf(stderr,
                "[page-guard] SELF-TEST: %zu slack bytes sit between a 32-byte block and\n"
                "[page-guard] its guard page, so an overrun shorter than that is NOT caught.\n",
                slack);
        }
        std::fflush(stderr);
    }
}

void gos_memory_ProbeSelfTest()
{
    gos_memory_GuardSelfTest();
    if (probeLevel() < 1) return;

    std::fprintf(stderr,
        "[gos-free-probe] SELF-TEST: releasing one block behind the allocator's back\n"
        "[gos-free-probe] and freeing it again. ONE report must follow. If it does not,\n"
        "[gos-free-probe] the probe is not working and this run proves nothing.\n");
    std::fflush(stderr);

    g_probeSelfTesting = true;
    void* p = gos_Malloc(64, NULL);
    if (!p)
    {
        g_probeSelfTesting = false;
        std::fprintf(stderr, "[gos-free-probe] SELF-TEST could not allocate. Probe UNPROVEN.\n");
        std::fflush(stderr);
        return;
    }

    std::free(p);       // the release the side table never learns about
    gos_Free(p);        // must be REPORTED and skipped, not performed
    g_probeSelfTesting = false;

    // The sweep needs its own control: the block above is erased from the table
    // by gos_Free, so it can never be found by a later sweep. Leave a second one
    // IN the table, sweep, and only then release it.
    void* q = gos_Malloc(48, NULL);
    if (q)
    {
        g_probeSelfTesting = true;
        std::free(q);                          // behind the allocator's back
        sweepSelfTestExpected = true;
        gos_memory_SweepBlocks("self-test");   // must find exactly this one
        sweepSelfTestExpected = false;
        gos_Free(q);                           // clears the stale table entry
        g_probeSelfTesting = false;
    }

    std::fprintf(stderr,
        "[gos-free-probe] SELF-TEST done. THREE reports above, all labelled\n"
        "[gos-free-probe] (SELF-TEST, expected), mean both detectors are live: the\n"
        "[gos-free-probe] per-free check and the whole-table sweep. Anything NOT so\n"
        "[gos-free-probe] labelled is a finding.\n");
    std::fflush(stderr);
}
void* __stdcall gos_Malloc(size_t bytes, HGOSHEAP Heap)
{
    if (g_workerAllocationProbe)
        g_workerAllocationCount.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(tableLock());   // PORT: A1b
    ensureInit();
    if (noteCallingThread()) ++g_foreignEvents;   // PORT: A1b thread census

    // "Not filling in the heap parameter ... allocate memory in the heap
    // currently on top of the heap stack."
    gos_Heap* heap = Heap ? Heap : g_state.stack[g_state.depth];
    if (!heap) heap = g_state.root;

    // A zero-byte request still returns a unique freeable pointer, matching
    // what callers expect from malloc.
    // PORT: B1-1. When the page guard is armed this block gets its own page
    // run ending flush against a PAGE_NOACCESS page, so a stray write past it
    // faults AT THE WRITING INSTRUCTION instead of being noticed by whoever
    // frees next -- which is the whole gap seven runs of victim-side evidence
    // could not close. Past the budget PageGuardAlloc returns null and counts a
    // fallback, so the run degrades to plain malloc rather than failing.
    const size_t totalBytes = bytes + sizeof(uint32_t);   // + trailing guard
    void* p = mc2mem::PageGuardAlloc(totalBytes);
    if (!p) p = std::malloc(totalBytes);
    if (!p)
    {
        report("out of memory", heap->name);
        return nullptr;
    }

    writeBackGuard(p, bytes);

    BlockInfo info;
    info.heap = heap;
    info.userBytes = bytes;
    blocks()[p] = info;
    noteAllocSite(p);   // PORT: B1-1 probe, level 2 only

    chargeUp(heap, bytes);
    return p;
}

void MC2_SetWorkerAllocationProbe(bool active)
{
    g_workerAllocationProbe = active;
}

void MC2_ResetWorkerAllocationCount()
{
    g_workerAllocationCount.store(0, std::memory_order_relaxed);
}

unsigned long long MC2_GetWorkerAllocationCount()
{
    return g_workerAllocationCount.load(std::memory_order_relaxed);
}

void __stdcall gos_Free(void* ptr)
{
    if (!ptr) return;   // free(NULL) is a no-op and callers rely on that
    std::lock_guard<std::mutex> guard(tableLock());   // PORT: A1b
    if (noteCallingThread()) ++g_foreignEvents;   // PORT: A1b thread census

    BlockTable::iterator it = blocks().find(ptr);
    if (it == blocks().end())
    {
        // Not ours. This is now a RELIABLE determination, unlike the previous
        // magic-number scheme which had to read possibly-recycled memory.
        // The block was either already freed, or came from plain malloc/new and
        // was handed here by mistake. Both are bugs worth surfacing, and
        // neither is worth turning into a crash mid-port.
        g_state.foreignFreeEvents++;
        static int reported = 0;
        if (reported < 20)
        {
            ++reported;
            report("gos_Free on a pointer this allocator did not hand out "
                   "(double free, or malloc/new memory)", "");
        }
        return;
    }

    if (!backGuardIntact(ptr, it->second.userBytes))
    {
        report("buffer overrun detected at free, heap", it->second.heap->name);
        g_state.corruptionEvents++;
        // Freed regardless: leaking on top of a corruption report helps nobody.
    }

    const gos_Heap* freedFrom  = it->second.heap;
    const size_t    freedBytes = it->second.userBytes;
    // PORT: B1-1. The same question the teardown loop asks, at the ordinary
    // single-block free -- the release that matters does not have to be a
    // teardown.
    //
    // ORDER IS LOAD-BEARING, and the self-test is what proved it. Run this
    // BEFORE noteFreed, or gos_memory_FreedAt finds the address THIS call just
    // rang and every report claims the block was double-freed inside GameOS --
    // including the case where plain free() released it from outside, which is
    // the distinction the whole probe exists to draw. Before forgetAllocSite
    // too, or the allocation stack is gone by the time it is printed.
    const bool safe = probeBlockBeforeFree(freedFrom, ptr, freedBytes, 0, 1);

    creditUp(it->second.heap, it->second.userBytes);
    blocks().erase(it);
    forgetAllocSite(ptr);
    if (!safe) return;      // reported already; freeing it is the crash

    noteFreed(ptr);     // PORT: A1b double-free probe; see the ring above.
    releaseBlock(ptr);

    // PORT: B1-1 sweep. Bracketing the moment the heap goes bad is the whole
    // point -- the last clean sweep and the first dirty one name the window.
    if (const size_t every = sweepEvery())
    {
        static size_t sinceSweep = 0;
        if (++sinceSweep >= every)
        {
            sinceSweep = 0;
            sweepAllBlocksLocked("every-N-frees");
        }
    }
}

HGOSHEAP __stdcall gos_CreateMemoryHeap(char* HeapName, DWORD MaximumSize, HGOSHEAP parentHeap)
{
    ensureInit();
    gos_Heap* parent = parentHeap ? parentHeap : g_state.root;
    return newHeapNode(HeapName, static_cast<size_t>(MaximumSize), parent);
}

void __stdcall gos_DestroyMemoryHeap(HGOSHEAP Heap, bool shouldBeEmpty)
{
    if (!Heap) return;
    ensureInit();

    if (Heap == g_state.root)
    {
        report("refusing to destroy the root client heap", Heap->name);
        return;
    }

    // A heap being destroyed must not remain on the stack, or subsequent
    // allocations would be charged to freed memory.
    for (int i = g_state.depth; i >= 0; --i)
    {
        if (g_state.stack[i] == Heap)
        {
            report("heap destroyed while still on the heap stack", Heap->name);
            g_state.stackErrorEvents++;
            g_state.depth = (i > 0) ? i - 1 : 0;
            break;
        }
    }

    destroyRecursive(Heap, shouldBeEmpty);
}

void __stdcall gos_PushCurrentHeap(HGOSHEAP Heap)
{
    ensureInit();
    if (g_state.depth + 1 >= kMaxHeapDepth)
    {
        report("heap stack overflow; push ignored", Heap ? Heap->name : "");
        g_state.stackErrorEvents++;
        return;
    }
    g_state.stack[++g_state.depth] = Heap ? Heap : g_state.root;
}

HGOSHEAP __stdcall gos_GetCurrentHeap()
{
    ensureInit();
    return g_state.stack[g_state.depth];
}

void __stdcall gos_PopCurrentHeap()
{
    ensureInit();
    if (g_state.depth == 0)
    {
        report("heap stack underflow; pop ignored", "");
        g_state.stackErrorEvents++;
        return;
    }
    --g_state.depth;
}

void __stdcall gos_WalkMemoryHeap(HGOSHEAP pHeap, bool vociferous)
{
    ensureInit();
    // "Passing a NULL (or 0) as pHeap will check ALL heaps."
    gos_Heap* heap = pHeap ? pHeap : g_state.root;
    const int corruptBlocks = walkHeapRecursive(heap, vociferous);
    const char* reportStats = std::getenv("MC2_MEMORY_STATS");
    if (reportStats && *reportStats && std::strcmp(reportStats, "0") != 0)
    {
        std::fprintf(stderr,
                     "[gos_memory] heap summary '%s': liveBytes=%zu peakBytes=%zu "
                     "liveBlocks=%zu totalAllocs=%zu corruptBlocks=%d\n",
                     heap->name, heap->liveBytes, heap->peakBytes,
                     heap->liveCount, heap->totalAllocs, corruptBlocks);
    }
}

//---------------------------------------------------------------------------//
// Teardown census support. Declared in gos_memory_internal.h; see the comment
// there for why these exist alongside gos_WalkMemoryHeap.
//---------------------------------------------------------------------------//
int gos_memory_CountCorruptBlocks()
{
    ensureInit();
    return walkHeapRecursive(g_state.root, false);
}

size_t gos_memory_TotalLiveBlocks()
{
    ensureInit();
    return g_state.root->liveCount;
}

size_t gos_memory_TotalLiveBytes()
{
    ensureInit();
    return g_state.root->liveBytes;
}

size_t gos_memory_ThreadsSeen()   { return (size_t)g_seenThreadCount; }
size_t gos_memory_ForeignEvents() { return g_foreignEvents; }

long gos_memory_FreedAt(const void* p)
{
    if (!p) return -1;
    for (int i = 0; i < kFreedRing; ++i)
        if (g_freed[i].address == p) return g_freed[i].serial;
    return -1;
}

bool gos_memory_BlockExtent(const void* p, const void** base, size_t* userBytes)
{
    if (!p) return false;
    std::lock_guard<std::mutex> guard(tableLock());
    const char* c = (const char*)p;
    for (BlockTable::const_iterator it = blocks().begin(); it != blocks().end(); ++it)
    {
        const char* b = (const char*)it->first;
        if (c >= b && c < b + it->second.userBytes)
        {
            if (base)      *base = it->first;
            if (userBytes) *userBytes = it->second.userBytes;
            return true;
        }
    }
    return false;
}

size_t gos_memory_BlockSize(const void* p)
{
    if (!p) return 0;
    std::lock_guard<std::mutex> guard(tableLock());
    BlockTable::const_iterator it = blocks().find(const_cast<void*>(p));
    return it == blocks().end() ? 0 : it->second.userBytes;
}

bool gos_memory_IsLiveBlock(const void* p)
{
    if (!p) return false;
    ensureInit();
    // A table hit means the block is allocated now. No dereference of p, which
    // matters: the whole point is to ask about a pointer that may already be
    // freed, and reading it would be the very thing we are trying to detect.
    return blocks().find(const_cast<void*>(p)) != blocks().end();
}

// GamePlatform maps new/delete onto the memory manager; GameOS.HPP declares the
// heap-targeted placement form, so it is provided here for completeness.
void* __cdecl operator new (size_t size, HGOSHEAP Heap)
{
    return gos_Malloc(size, Heap);
}

// M8-B75: without this, new(heap) T[n] binds to the standard placement
// operator new[](size_t, void*) and returns the heap pointer itself.
void* __cdecl operator new[] (size_t size, HGOSHEAP Heap)
{
    return gos_Malloc(size, Heap);
}

//---------------------------------------------------------------------------//
// Test-only introspection. Declared in gos_memory_internal.h, which game code
// never includes.
//---------------------------------------------------------------------------//
namespace gos_memory_test {

HGOSHEAP RootHeap()              { ensureInit(); return g_state.root; }
int      StackDepth()            { ensureInit(); return g_state.depth; }

size_t   LiveBytes(HGOSHEAP h)   { return h ? h->liveBytes : 0; }
size_t   LiveCount(HGOSHEAP h)   { return h ? h->liveCount : 0; }
bool     ExceededMax(HGOSHEAP h) { return h ? h->exceededMax : false; }

size_t   OverflowEvents()        { return g_state.overflowEvents; }
size_t   CorruptionEvents()      { return g_state.corruptionEvents; }
size_t   LeakEvents()            { return g_state.leakEvents; }
size_t   StackErrorEvents()      { return g_state.stackErrorEvents; }
size_t   ForeignFreeEvents()     { return g_state.foreignFreeEvents; }

void ResetCounters()
{
    g_state.overflowEvents    = 0;
    g_state.corruptionEvents  = 0;
    g_state.leakEvents        = 0;
    g_state.stackErrorEvents  = 0;
    g_state.foreignFreeEvents = 0;
}

} // namespace gos_memory_test
