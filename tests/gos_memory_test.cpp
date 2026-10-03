//===========================================================================//
// Unit harness for the GameOS memory manager.
//
// WHY THIS EXISTS: the game cannot link yet (73 source files still fail to
// compile, and seven other GameOS subsystems are unwritten), so there is no
// path to executing this code inside MC2 for a long while. Meanwhile MCLib's
// UserHeap now routes every allocation through it. Without this harness the
// allocator's first execution would be inside a 322-translation-unit binary
// during engine init, where an attribution or alignment bug shows up as
// arbitrary corruption far from its cause.
//
// Deliberately dependency-free: no gtest, no framework, just a CHECK macro and
// a nonzero exit code, so it runs anywhere the port builds.
//===========================================================================//

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>

#include <gameos.hpp>
#include "gos_memory_internal.h"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, (msg));    \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define SECTION(name) std::printf("[ %s ]\n", name)

using namespace gos_memory_test;

// gos_system.cpp owns gos_TerminateApplication as well as the viewport API and
// therefore references the main-loop quit flag.  The unit harness supplies the
// flag so linking that object does not pull the production process entry point
// out of gos_platform.cpp.
bool g_gosQuitRequested = false;

// ---------------------------------------------------------------- basics
static void test_alloc_free_roundtrip()
{
    SECTION("alloc / free round trip");
    ResetCounters();

    HGOSHEAP root = RootHeap();
    const size_t before = LiveBytes(root);

    void* p = gos_Malloc(100);
    CHECK(p != nullptr, "gos_Malloc returned null");
    CHECK(LiveBytes(root) == before + 100, "live bytes not charged to current heap");
    CHECK(LiveCount(root) >= 1, "live count not incremented");

    // The whole user region must be writable -- catches an undersized malloc.
    std::memset(p, 0xAB, 100);

    gos_Free(p);
    CHECK(LiveBytes(root) == before, "live bytes not credited back on free");
    CHECK(CorruptionEvents() == 0, "spurious corruption report");
}

static void test_zero_byte_and_null()
{
    SECTION("zero-byte alloc and gos_Free(NULL)");
    ResetCounters();

    void* z = gos_Malloc(0);
    CHECK(z != nullptr, "zero-byte alloc should still return a freeable pointer");
    gos_Free(z);

    gos_Free(nullptr); // must be a silent no-op
    CHECK(CorruptionEvents() == 0, "gos_Free(NULL) must not report corruption");
}

static void test_alignment()
{
    SECTION("returned pointers are max-aligned");
    ResetCounters();

    // The allocator returns the malloc pointer unchanged (no prefix header), so
    // alignment is malloc's guarantee. Checked anyway across a range of sizes,
    // because that property is what makes free()/delete on a GameOS block safe
    // and it must not silently regress.
    const size_t align = alignof(max_align_t);

    void* ptrs[16];
    for (int i = 0; i < 16; ++i)
    {
        ptrs[i] = gos_Malloc(static_cast<size_t>(i) * 7 + 1);
        CHECK(ptrs[i] != nullptr, "alloc failed during alignment sweep");
        CHECK(reinterpret_cast<uintptr_t>(ptrs[i]) % align == 0,
              "returned pointer is not suitably aligned");
    }
    for (int i = 0; i < 16; ++i) gos_Free(ptrs[i]);
}

static void test_viewport_returns_pixel_scalars()
{
    SECTION("viewport returns pixel-space transform");
    gos_SetScreenMode(1024, 768, 32, 0, false, false, false, false, 0, true, false, 0);
    gos_SetupViewport(true, 1.0f, true, 0, 0.0f, 0.0f, 1.0f, 1.0f);
    float mulX = 0.0f, mulY = 0.0f, addX = -1.0f, addY = -1.0f;
    gos_GetViewport(&mulX, &mulY, &addX, &addY);
    CHECK(mulX == 1024.0f, "full viewport X multiplier is not pixel width");
    CHECK(mulY == 768.0f, "full viewport Y multiplier is not pixel height");
    CHECK(addX == 0.0f && addY == 0.0f, "full viewport offset is not zero");

    gos_SetupViewport(false, 1.0f, false, 0, 0.25f, 0.25f, 0.75f, 0.75f);
    gos_GetViewport(&mulX, &mulY, &addX, &addY);
    CHECK(mulX == 512.0f && mulY == 384.0f && addX == 256.0f && addY == 192.0f,
          "partial viewport did not scale normalized bounds into pixels");
}

// ---------------------------------------------------------------- heap stack
static void test_heap_stack()
{
    SECTION("heap stack push / pop / current");
    ResetCounters();

    HGOSHEAP root = RootHeap();
    const int  d0 = StackDepth();
    CHECK(gos_GetCurrentHeap() == root, "current heap should start at the root");

    HGOSHEAP a = gos_CreateMemoryHeap(const_cast<char*>("A"));
    HGOSHEAP b = gos_CreateMemoryHeap(const_cast<char*>("B"));

    gos_PushCurrentHeap(a);
    CHECK(gos_GetCurrentHeap() == a, "push did not change the current heap");
    CHECK(StackDepth() == d0 + 1, "depth did not increase");

    gos_PushCurrentHeap(b);
    CHECK(gos_GetCurrentHeap() == b, "nested push failed");

    gos_PopCurrentHeap();
    CHECK(gos_GetCurrentHeap() == a, "pop did not restore the previous heap");

    gos_PopCurrentHeap();
    CHECK(gos_GetCurrentHeap() == root, "pop did not return to the root");
    CHECK(StackDepth() == d0, "depth not restored");

    gos_DestroyMemoryHeap(a, true);
    gos_DestroyMemoryHeap(b, true);
    CHECK(StackErrorEvents() == 0, "unexpected heap-stack error");
}

static void test_stack_underflow_is_survivable()
{
    SECTION("heap stack underflow is reported, not fatal");
    ResetCounters();

    while (StackDepth() > 0) gos_PopCurrentHeap();
    gos_PopCurrentHeap(); // one too many

    CHECK(StackErrorEvents() == 1, "underflow should be reported exactly once");
    CHECK(gos_GetCurrentHeap() != nullptr, "current heap must stay valid after underflow");
}

// ------------------------------------------------------- attribution / tree
static void test_allocation_attribution()
{
    SECTION("allocations are charged to the right heap");
    ResetCounters();

    HGOSHEAP h = gos_CreateMemoryHeap(const_cast<char*>("Attribution"));
    CHECK(LiveBytes(h) == 0, "new heap should start empty");

    gos_PushCurrentHeap(h);
    void* p = gos_Malloc(256);
    gos_PopCurrentHeap();

    CHECK(LiveBytes(h) == 256, "allocation not charged to the pushed heap");

    // Explicit-heap form must bypass the stack entirely.
    void* q = gos_Malloc(64, h);
    CHECK(LiveBytes(h) == 320, "explicit-heap allocation not charged correctly");

    gos_Free(p);
    gos_Free(q);
    CHECK(LiveBytes(h) == 0, "heap not empty after freeing everything");

    gos_DestroyMemoryHeap(h, true);
    CHECK(LeakEvents() == 0, "clean heap wrongly reported as leaking");
}

static void test_parent_totals_include_children()
{
    SECTION("parent totals include child heaps");
    ResetCounters();

    HGOSHEAP parent = gos_CreateMemoryHeap(const_cast<char*>("Parent"));
    HGOSHEAP child  = gos_CreateMemoryHeap(const_cast<char*>("Child"), 0, parent);

    void* p = gos_Malloc(500, child);
    CHECK(LiveBytes(child)  == 500, "child total wrong");
    CHECK(LiveBytes(parent) == 500, "parent total should include the child's bytes");

    gos_Free(p);
    CHECK(LiveBytes(parent) == 0, "parent total not credited back");

    gos_DestroyMemoryHeap(parent, true);
}

static void test_destroy_kills_children()
{
    SECTION("destroying a heap destroys its children");
    ResetCounters();

    HGOSHEAP parent = gos_CreateMemoryHeap(const_cast<char*>("P"));
    HGOSHEAP child  = gos_CreateMemoryHeap(const_cast<char*>("C"), 0, parent);

    gos_Malloc(128, child);   // deliberately leaked

    // shouldBeEmpty == true must notice the child's outstanding block.
    gos_DestroyMemoryHeap(parent, true);
    CHECK(LeakEvents() >= 1, "non-empty child should have been reported on destroy");
}

static void test_destroy_silent_when_not_required_empty()
{
    SECTION("shouldBeEmpty == false silences the leak report");
    ResetCounters();

    HGOSHEAP h = gos_CreateMemoryHeap(const_cast<char*>("Quiet"));
    gos_Malloc(32, h);
    gos_DestroyMemoryHeap(h, false);

    CHECK(LeakEvents() == 0, "leak reported despite shouldBeEmpty == false");
}

static void test_destroy_cursor_control_heap()
{
    SECTION("destroy-path cursor control heap");
    ResetCounters();

    // MC2_GOS_FREE_CURSOR_MAX=424242 selects only this heap. The ordinary
    // test asserts the destroy path remains healthy; the instrument's positive
    // control run additionally inspects gos-free-cursor.txt for both snapshots.
    HGOSHEAP h = gos_CreateMemoryHeap(const_cast<char*>("CursorControl"), 424242);
    void* p = gos_Malloc(53, h);
    CHECK(p != nullptr, "cursor control allocation failed");
    gos_DestroyMemoryHeap(h, false);
    CHECK(CorruptionEvents() == 0, "cursor control destroy reported corruption");
}

// ---------------------------------------------------------------- limits
static void test_maximum_size_asserts()
{
    SECTION("MaximumSize is enforced");
    ResetCounters();

    HGOSHEAP h = gos_CreateMemoryHeap(const_cast<char*>("Capped"), 1000);

    void* a = gos_Malloc(600, h);
    CHECK(!ExceededMax(h), "under the cap should not trip the limit");

    void* b = gos_Malloc(600, h); // now 1200 > 1000
    CHECK(ExceededMax(h), "exceeding MaximumSize should be flagged");
    CHECK(OverflowEvents() >= 1, "overflow not counted");

    gos_Free(a);
    gos_Free(b);
    gos_DestroyMemoryHeap(h, true);
}

// ---------------------------------------------------------------- integrity
static void test_overrun_is_detected()
{
    SECTION("gos_WalkMemoryHeap detects a buffer overrun");
    ResetCounters();

    HGOSHEAP h = gos_CreateMemoryHeap(const_cast<char*>("Guarded"));
    unsigned char* p = static_cast<unsigned char*>(gos_Malloc(16, h));

    gos_WalkMemoryHeap(h, false);
    CHECK(CorruptionEvents() == 0, "clean heap should walk without complaint");

    // Deliberately scribble one byte past the user region. This is the exact
    // condition GameOS.HPP promises to catch.
    p[16] = 0xFF;

    gos_WalkMemoryHeap(h, false);
    CHECK(CorruptionEvents() >= 1, "overrun past the user region was not detected");

    // The guard is a 4-byte word, so a single byte cannot be "repaired" back to
    // it -- an earlier version of this test pretended to and silently relied on
    // a >= 1 assertion to stay green. Instead, assert the free path detects the
    // damage independently of the walk, which is a stronger claim.
    ResetCounters();
    gos_Free(p);
    CHECK(CorruptionEvents() >= 1, "free path did not independently detect the overrun");

    gos_DestroyMemoryHeap(h, true);
}

static void test_double_free_is_detected_reliably()
{
    SECTION("double free is detected reliably via the side table");
    ResetCounters();

    void* p = gos_Malloc(48);
    gos_Free(p);

    // Now RELIABLE, and that is a direct benefit of dropping the prefix header.
    // The previous design had to read a magic number out of the block on the
    // second free -- itself a use-after-free, so detection depended on the CRT
    // not having recycled the memory yet. The side table answers definitively
    // without touching freed memory at all.
    gos_Free(p);

    CHECK(ForeignFreeEvents() == 1, "double free not detected exactly once");
}

static void test_crt_free_of_gos_block_is_survivable()
{
    SECTION("plain free() on a GameOS block does not corrupt the CRT heap");
    ResetCounters();

    // THIS IS THE REGRESSION TEST FOR THE M2 BRING-UP BUG. With a prefix header
    // the pointer handed out was not malloc's, so this exact sequence tripped
    //     debug_heap.cpp(904): Assertion failed: _CrtIsValidHeapPointer(block)
    // inside mission->destroy() -- and the assertion's modal dialog made it look
    // like a deadlock. MC2 mixes malloc/free, new/delete, UserHeap and
    // gos_Malloc, so this has to be survivable rather than fatal.
    void* p = gos_Malloc(64);
    CHECK(p != nullptr, "allocation failed");
    std::memset(p, 0x5A, 64);

    free(p);   // deliberately the wrong deallocator

    // The accounting entry is now stale, which is the accepted trade: it shows
    // up as a leak in gos_WalkMemoryHeap rather than as heap corruption.
    // Reaching this line at all is the assertion.
    CHECK(true, "survived free() of a GameOS block");
}

// ---------------------------------------------------------------- volume
static void test_high_volume_churn()
{
    SECTION("high-volume churn keeps accounting exact");
    ResetCounters();

    HGOSHEAP h = gos_CreateMemoryHeap(const_cast<char*>("Churn"));
    const int N = 2000;
    void* ptrs[N];

    size_t expected = 0;
    for (int i = 0; i < N; ++i)
    {
        size_t sz = static_cast<size_t>((i * 37) % 512) + 1;
        ptrs[i] = gos_Malloc(sz, h);
        expected += sz;
    }
    CHECK(LiveBytes(h) == expected, "bytes drifted during bulk allocation");
    CHECK(LiveCount(h) == static_cast<size_t>(N), "count drifted during bulk allocation");

    // Free in an interleaved order to exercise the live-list unlink paths.
    for (int i = 0; i < N; i += 2) gos_Free(ptrs[i]);
    for (int i = 1; i < N; i += 2) gos_Free(ptrs[i]);

    CHECK(LiveBytes(h) == 0, "bytes not zero after freeing everything");
    CHECK(LiveCount(h) == 0, "count not zero after freeing everything");
    CHECK(CorruptionEvents() == 0, "corruption reported during churn");

    gos_DestroyMemoryHeap(h, true);
}

// ---------------------------------------------------------------- driver
int main()
{
    std::printf("=== GameOS memory manager unit tests ===\n");

    test_alloc_free_roundtrip();
    test_zero_byte_and_null();
    test_alignment();
    test_viewport_returns_pixel_scalars();
    test_heap_stack();
    test_stack_underflow_is_survivable();
    test_allocation_attribution();
    test_parent_totals_include_children();
    test_destroy_kills_children();
    test_destroy_silent_when_not_required_empty();
    test_destroy_cursor_control_heap();
    test_maximum_size_asserts();
    test_overrun_is_detected();
    test_double_free_is_detected_reliably();
    test_crt_free_of_gos_block_is_survivable();
    test_high_volume_churn();

    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
