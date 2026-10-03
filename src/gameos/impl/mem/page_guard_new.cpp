//---------------------------------------------------------------------------//
// M8-B7: route the WHOLE game's C++ allocations through the page guard.
//
// Guarding EString alone was not enough -- the 2026-08-17 armed replay crashed
// in MC_TextureManager::update with the guard silent, which proved the bad write
// is not into a string buffer but said nothing about where it IS.
//
// Replacing global operator new/delete widens the net in one step: every `new`
// in game code, MCLib and the statically linked Dawn goes through the guard.
// What it still cannot see is anything that calls malloc() directly and MC2's
// own UserHeap, which is a separate VirtualAlloc arena.
//
// THIS FILE MUST BE COMPILED INTO THE EXECUTABLE, not into a static library. A
// replacement operator new sitting in a .lib is only pulled in if something
// already references it, so it silently does nothing.
//
// A BUDGET IS MANDATORY HERE, not a nicety. Every guarded block costs at least
// one 4 KiB page plus a reserved guard page, so guarding a whole mission's
// allocations unbudgeted would exhaust memory long before it found anything.
// Past the budget this falls back to plain malloc and counts it, so the report
// says how much of the run was actually covered.
//---------------------------------------------------------------------------//

#include "mem/page_guard.h"

#include <cstdlib>
#include <new>

void* operator new(std::size_t bytes)
{
    if (bytes == 0) bytes = 1;
    if (mc2mem::PageGuardGlobalNewEnabled())
        if (void* guarded = mc2mem::PageGuardAlloc(bytes)) return guarded;
    void* p = std::malloc(bytes);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t bytes)                       { return ::operator new(bytes); }
void* operator new(std::size_t b, const std::nothrow_t&) noexcept
{
    if (b == 0) b = 1;
    if (mc2mem::PageGuardGlobalNewEnabled())
        if (void* g = mc2mem::PageGuardAlloc(b)) return g;
    return std::malloc(b);
}
void* operator new[](std::size_t b, const std::nothrow_t& t) noexcept { return ::operator new(b, t); }

void operator delete(void* p) noexcept
{
    if (!p) return;
    if (mc2mem::PageGuardOwns(p)) { mc2mem::PageGuardFree(p); return; }
    std::free(p);
}

void operator delete[](void* p) noexcept                      { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept           { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept         { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
