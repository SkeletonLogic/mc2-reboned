//---------------------------------------------------------------------------//
// M8-B7: see page_guard.h for why this exists and what it can prove.
//---------------------------------------------------------------------------//

#include "page_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mc2mem
{
    namespace
    {
#if defined(_WIN32)

        struct Region
        {
            char*       base;       // start of the reservation
            std::size_t total;      // reserved bytes, guard page included
            std::size_t commit;     // committed bytes; the rest is the guard
            bool        freed;      // kept OUT of the block itself -- see PageGuardFree
        };

        // Blocks are never reused, so this only ever grows. A long mission is
        // tens of thousands of EString buffers; at 64 bytes of bookkeeping each
        // that is a few MB, which is the cheapest part of this mode.
        Region*     g_regions   = nullptr;
        std::size_t g_count     = 0;
        std::size_t g_capacity  = 0;

        char*       g_low       = (char*)~(uintptr_t)0;
        char*       g_high      = nullptr;

        std::size_t g_live      = 0;
        std::size_t g_freed     = 0;
        std::size_t g_bytes     = 0;   // reserved, cumulative
        std::size_t g_committed = 0;   // currently committed, i.e. real memory
        std::size_t g_fallbacks = 0;
        int         g_selfGuardPage = -1;   // -1 not run, 0 failed, 1 passed
        int         g_selfFreedBlock = -1;
        std::size_t g_reserved  = 0;   // reservation currently held, not cumulative
        std::size_t g_recycled  = 0;   // freed regions handed back to the OS
        std::size_t g_recycleScan = 0; // oldest-first cursor, so this stays O(1) amortised

        // Ceiling on RESERVED address space, separate from the commit budget.
        // Committed memory is what costs RAM; reservation is what runs out over a
        // long session, and it was measured at 55 GB in 26,000 ticks.
        std::size_t ReserveCapBytes()
        {
            static const std::size_t cap = []() -> std::size_t {
                const char* mb = std::getenv("MC2_PAGE_GUARD_RESERVE_MB");
                const long v = mb && *mb ? std::atol(mb) : 0;
                return (std::size_t)(v > 0 ? v : 8192) * 1024u * 1024u;
            }();
            return cap;
        }

        void RehashAllPages();

        // Give the oldest freed reservations back, oldest first, because a
        // use-after-free almost always touches memory freed recently -- the far
        // past is the cheapest thing to stop watching. Releases down to 80% of
        // the cap in one batch so the rehash below is amortised rather than run
        // on every free once the ceiling is reached.
        void RecycleOldestFreed()
        {
            const std::size_t cap = ReserveCapBytes();
            if (g_reserved <= cap) return;

            const std::size_t target = cap - cap / 5;
            bool releasedAny = false;
            while (g_reserved > target && g_recycleScan < g_count)
            {
                Region& r = g_regions[g_recycleScan];
                if (!r.freed || r.base == nullptr) { ++g_recycleScan; continue; }

                VirtualFree(r.base, 0, MEM_RELEASE);
                g_reserved -= r.total;
                r.base = nullptr;
                ++g_recycled;
                ++g_recycleScan;
                releasedAny = true;
            }

            // If the scan ran out of freed regions the cap cannot be met yet;
            // start again from the beginning next time rather than never.
            if (g_recycleScan >= g_count) g_recycleScan = 0;
            if (releasedAny) RehashAllPages();
        }

        std::size_t PageSize()
        {
            static std::size_t size = []() -> std::size_t {
                SYSTEM_INFO si;
                GetSystemInfo(&si);
                return si.dwPageSize ? si.dwPageSize : 4096;
            }();
            return size;
        }

        // Guarding every allocation in the game is only survivable with a cap:
        // each block costs a page plus a reserved guard page, so an unbudgeted
        // run exhausts memory long before it finds anything. Past the budget we
        // fall back to plain malloc and count it, so the report says how much of
        // the run was actually covered rather than implying full coverage.
        std::size_t BudgetBytes()
        {
            static const std::size_t budget = []() -> std::size_t {
                const char* mb = std::getenv("MC2_PAGE_GUARD_MB");
                const long  n  = (mb && *mb) ? std::atol(mb) : 512;
                return (std::size_t)((n > 0 ? n : 512)) * 1024u * 1024u;
            }();
            return budget;
        }

        // EVERY PAGE of a block maps to its region, so a free is O(1) on the
        // pointer's page rather than a walk of every block ever allocated. That
        // walk was survivable while only EString was guarded (~1,200 blocks);
        // with global new/delete it is every allocation in the game and would
        // make each delete O(n).
        //
        // Open addressing, power-of-two capacity, never deleted from: address
        // space is never reused here, so an entry stays valid for the life of
        // the process.
        struct Slot { char* page; std::size_t region; };
        Slot*       g_slots    = nullptr;
        std::size_t g_slotMask = 0;
        std::size_t g_slotUsed = 0;

        std::size_t HashPage(const char* page)
        {
            uintptr_t h = (uintptr_t)page >> 12;
            h *= 0x9E3779B97F4A7C15ULL;
            return (std::size_t)(h >> 32);
        }

        bool GrowSlots();

        void MapPage(char* page, std::size_t region)
        {
            if ((g_slotUsed + 1) * 4 >= (g_slotMask + 1) * 3 && !GrowSlots()) return;
            std::size_t i = HashPage(page) & g_slotMask;
            while (g_slots[i].page) i = (i + 1) & g_slotMask;
            g_slots[i].page   = page;
            g_slots[i].region = region;
            ++g_slotUsed;
        }

        bool GrowSlots()
        {
            const std::size_t grown = g_slots ? (g_slotMask + 1) * 2 : 65536;
            Slot* fresh = (Slot*)std::calloc(grown, sizeof(Slot));
            if (!fresh) return false;
            Slot* old = g_slots;
            const std::size_t oldCap = g_slots ? g_slotMask + 1 : 0;
            g_slots = fresh; g_slotMask = grown - 1; g_slotUsed = 0;
            for (std::size_t i = 0; i < oldCap; ++i)
                if (old[i].page) MapPage(old[i].page, old[i].region);
            std::free(old);
            return true;
        }

        // -1 when this page was never handed out by us.
        // Rebuild the page map from the regions that are still reserved. Called
        // only after a recycle batch.
        void RehashAllPages()
        {
            if (!g_slots) return;
            std::memset(g_slots, 0, (g_slotMask + 1) * sizeof(g_slots[0]));
            g_slotUsed = 0;
            const std::size_t page = PageSize();
            for (std::size_t i = 0; i < g_count; ++i)
            {
                Region& r = g_regions[i];
                if (!r.base) continue;
                for (std::size_t off = 0; off < r.total; off += page)
                    MapPage(r.base + off, i);
            }
        }

        std::size_t FindRegion(const char* p)
        {
            if (!g_slots) return (std::size_t)-1;
            char* page = (char*)((uintptr_t)p & ~(uintptr_t)(PageSize() - 1));
            std::size_t i = HashPage(page) & g_slotMask;
            while (g_slots[i].page)
            {
                if (g_slots[i].page == page)
                {
                    // A recycled region keeps no claim on its old addresses.
                    const std::size_t idx = g_slots[i].region;
                    return g_regions[idx].base ? idx : (std::size_t)-1;
                }
                i = (i + 1) & g_slotMask;
            }
            return (std::size_t)-1;
        }

        bool Record(char* base, std::size_t total, std::size_t commit)
        {
            if (g_count == g_capacity)
            {
                const std::size_t grown = g_capacity ? g_capacity * 2 : 4096;
                Region* bigger = (Region*)std::realloc(g_regions, grown * sizeof(Region));
                if (!bigger) return false;
                g_regions  = bigger;
                g_capacity = grown;
            }
            g_regions[g_count].base   = base;
            g_regions[g_count].total  = total;
            g_regions[g_count].commit = commit;
            g_regions[g_count].freed  = false;
            const std::size_t index = g_count;
            ++g_count;
            for (char* pg = base; pg < base + commit; pg += PageSize())
                MapPage(pg, index);
            g_reserved += total;
            RecycleOldestFreed();
            if (base < g_low)          g_low  = base;
            if (base + total > g_high) g_high = base + total;
            return true;
        }

#endif // _WIN32
    } // namespace

    bool PageGuardGlobalNewEnabled()
    {
#if defined(_WIN32)
        static const bool on = []() -> bool {
            if (!PageGuardEnabled()) return false;
            const char* env = std::getenv("MC2_PAGE_GUARD_GLOBAL");
            const bool set = env && *env && std::strcmp(env, "0") != 0;
            if (set)
                std::fprintf(stderr,
                    "[page-guard] GLOBAL new/delete band armed. THIS IS THE BAND THAT\n"
                    "[page-guard] DESTABILISED DAWN on 2026-08-17 -- it routes the graphics\n"
                    "[page-guard] runtime's own allocations through page-granular VirtualAlloc.\n"
                    "[page-guard] A crash in shader compilation under this switch is the\n"
                    "[page-guard] instrument, not the defect.\n");
            return set;
        }();
        return on;
#else
        return false;
#endif
    }

    bool PageGuardEnabled()
    {
#if defined(_WIN32)
        static const bool on = []() -> bool {
            const char* env = std::getenv("MC2_PAGE_GUARD");
            const bool set = env && *env && std::strcmp(env, "0") != 0;
            if (set)
            {
                std::fprintf(stderr,
                    "[page-guard] ARMED over gos_Malloc blocks. Each gets its own page run\n"
                    "[page-guard] with an inaccessible guard page after it, and a freed block\n"
                    "[page-guard] is decommitted and never reused. An overrun or a use-after-free\n"
                    "[page-guard] now faults AT THE WRITING INSTRUCTION -- on the crash stack THAT\n"
                    "[page-guard] frame is the culprit, not the innocent free() below it.\n"
                    "[page-guard] Address space is never released, physical memory is. Diagnostic\n"
                    "[page-guard] mode: slower and hungrier than the real allocator.\n");
                std::fflush(stderr);
            }
            return set;
        }();
        return on;
#else
        return false;
#endif
    }

#if defined(_WIN32)

    void* PageGuardAlloc(std::size_t bytes)
    {
        if (!PageGuardEnabled() || bytes == 0) return nullptr;

        const std::size_t page = PageSize();
        // The block gets whole pages; an inaccessible guard page follows it.
        const std::size_t need    = bytes;
        const std::size_t commit  = ((need + page - 1) / page) * page;
        const std::size_t total   = commit + page;

        if (g_committed + commit > BudgetBytes()) { ++g_fallbacks; return nullptr; }

        char* base = (char*)VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_NOACCESS);
        if (!base) { ++g_fallbacks; return nullptr; }

        if (!VirtualAlloc(base, commit, MEM_COMMIT, PAGE_READWRITE))
        {
            VirtualFree(base, 0, MEM_RELEASE);
            ++g_fallbacks;
            return nullptr;
        }
        // The tail page stays reserved-and-inaccessible. Reserved is enough:
        // touching it faults exactly as a committed PAGE_NOACCESS page would.

        if (!Record(base, total, commit))
        {
            VirtualFree(base, 0, MEM_RELEASE);
            ++g_fallbacks;
            return nullptr;
        }

        // Push the user block right, flush against the guard page, so an
        // overrun has no slack to land in before it hits the guard.
        char* user = base + commit - bytes;
        user = (char*)((uintptr_t)user & ~(uintptr_t)15);
        if (user < base) user = base;

        ++g_live;
        g_bytes += total;
        g_committed += commit;
        return user;
    }

    bool PageGuardOwns(const void* p)
    {
        if (!PageGuardEnabled() || !p) return false;
        // The low/high range test this used to do was safe while only EString
        // was guarded, because everything in that range came from here. With
        // global new/delete a malloc'd pointer can land between two of our
        // reservations, and answering "yes" to that would send it to the wrong
        // deallocator. Ask the page map, which cannot be wrong.
        const char* c = (const char*)p;
        if (c < g_low || c >= g_high) return false;      // cheap reject first
        return FindRegion(c) != (std::size_t)-1;
    }

    void PageGuardFree(void* p)
    {
        if (!p) return;

        const std::size_t index = FindRegion((const char*)p);
        if (index != (std::size_t)-1)
        {
            Region& r = g_regions[index];

            // The freed flag lives HERE, in the bookkeeping table, and not in a
            // header inside the block -- the block is about to become
            // unreadable, and a double-free check that has to touch poisoned
            // memory to run is no check at all.
            if (r.freed)
            {
                std::fprintf(stderr, "[page-guard] *** DOUBLE FREE at %p ***\n", p);
                std::fflush(stderr);
                return;
            }
            r.freed = true;

            // DECOMMIT, not just protect. The reservation stays, so the address
            // can never be handed out again and any stale pointer still faults
            // at the instruction that touched it -- but the physical pages go
            // back, which is what makes a whole mission affordable.
            VirtualFree(r.base, r.commit, MEM_DECOMMIT);
            RecycleOldestFreed();

            --g_live;
            ++g_freed;
            g_committed -= r.commit;
            return;
        }

        std::fprintf(stderr,
            "[page-guard] *** free of %p, which this allocator never handed out ***\n", p);
        std::fflush(stderr);
    }

    int PageGuardAddressInaccessible(const void* p, std::size_t offset)
    {
#if defined(_WIN32)
        if (!p) return -1;
        const void* target = (const char*)p + offset;
        MEMORY_BASIC_INFORMATION info;
        std::memset(&info, 0, sizeof(info));
        if (VirtualQuery(target, &info, sizeof(info)) != sizeof(info)) return -1;
        if (info.State != MEM_COMMIT) return 1;
        const DWORD blocked = PAGE_NOACCESS | PAGE_GUARD;
        return (info.Protect & blocked) ? 1 : 0;
#else
        (void)p; (void)offset;
        return -1;
#endif
    }

    void PageGuardNoteSelfTest(int guardPageBlocked, int freedBlockBlocked)
    {
#if defined(_WIN32)
        g_selfGuardPage  = guardPageBlocked;
        g_selfFreedBlock = freedBlockBlocked;
#else
        (void)guardPageBlocked; (void)freedBlockBlocked;
#endif
    }

    // Short enough to sit beside the FPS counter. Anything other than "ok"
    // means the run cannot prove what it looks like it is proving:
    //   SELFTEST?  the self-test has not run
    //   SELFTEST!  a class of access is NOT being caught
    //   fb=N>0     N allocations went UNGUARDED past the budget
    //   rec=N      N freed regions handed back, no longer watched for UAF
    void PageGuardStatusLine(char* out, std::size_t outBytes)
    {
        if (!out || outBytes == 0) return;
#if defined(_WIN32)
        if (!PageGuardEnabled())
        {
            std::snprintf(out, outBytes, "GUARD off");
            return;
        }
        const char* self = "ok";
        if (g_selfGuardPage < 0 || g_selfFreedBlock < 0) self = "SELFTEST?";
        else if (g_selfGuardPage != 1 || g_selfFreedBlock != 1) self = "SELFTEST!";

        char rec[32];
        if (g_recycled >= 1000000)
            std::snprintf(rec, sizeof(rec), "%.1fM", (double)g_recycled / 1000000.0);
        else if (g_recycled >= 1000)
            std::snprintf(rec, sizeof(rec), "%zuk", g_recycled / 1000);
        else
            std::snprintf(rec, sizeof(rec), "%zu", g_recycled);

        std::snprintf(out, outBytes, "GUARD %s fb=%zu rec=%s", self, g_fallbacks, rec);
#else
        std::snprintf(out, outBytes, "GUARD n/a");
#endif
    }

    void PageGuardReport()
    {
        if (!PageGuardEnabled()) return;
        std::fprintf(stderr,
            "[page-guard] blocks live=%zu freed=%zu, committed %.1f MB of %.0f MB budget, "
            "reserved %.1f MB held, %zu regions RECYCLED (no longer watched for use-after-free), UNGUARDED fallbacks=%zu\n",
            g_live, g_freed,
            (double)g_committed / (1024.0 * 1024.0),
            (double)BudgetBytes() / (1024.0 * 1024.0),
            (double)g_reserved / (1024.0 * 1024.0), g_recycled, g_fallbacks);
        std::fflush(stderr);
    }

#else  // not _WIN32

    void* PageGuardAlloc(std::size_t)      { return nullptr; }
    bool  PageGuardOwns(const void*)       { return false; }
    void  PageGuardFree(void*)             {}
    int   PageGuardAddressInaccessible(const void*, std::size_t) { return -1; }
    void  PageGuardNoteSelfTest(int, int)  {}
    void  PageGuardStatusLine(char* out, std::size_t outBytes)
    {
        if (out && outBytes) std::snprintf(out, outBytes, "GUARD n/a");
    }
    void  PageGuardReport()                {}

#endif
} // namespace mc2mem
