#pragma once

#include <cstddef>

//---------------------------------------------------------------------------//
// M8-B7: page heap, in-process, opt-in, and OURS.
//
// The evidence gap for this defect is that the corruption is only detected at a
// LATER free() -- ntdll raises 0xC0000374 while walking a heap that some earlier
// instruction damaged, so the stack names the innocent caller. Four crashes
// across four runs have each pointed at a different delete site.
//
// The standard instrument for that gap is Windows page heap via App Verifier,
// which needs machine-wide Image File Execution Options and elevation. The owner
// ruled on 2026-08-17 that this project does not ask anyone to change
// machine-wide settings. It does not have to: EString allocates its buffers with
// malloc() in our own source, so we can do the same job here.
//
// HOW IT CATCHES THE WRITE. Each block gets its own committed page run with a
// PAGE_NOACCESS guard page immediately after it, and the returned pointer is
// pushed as far right as alignment allows so the block ENDS flush against that
// guard. An overrun of even one byte touches the guard page and takes an access
// violation AT THE INSTRUCTION THAT WROTE IT -- which is the thing no amount of
// free()-time checking can tell us.
//
// It also catches use-after-free: a freed block is DECOMMITTED but its address
// space is never released and never reused, so reading or writing through a
// stale pointer faults at the offending instruction rather than silently rotting
// whatever would have been handed the recycled memory. Decommitting rather than
// merely protecting is what makes a full mission affordable -- protecting alone
// leaves every buffer the session ever freed resident until it exits.
//
// COST, stated plainly: one 4 KiB page minimum per live allocation plus a
// reserved guard page, and address space that is never given back. This is a
// diagnostic mode for hunting one bug, not a shipping allocator. It is OFF
// unless MC2_PAGE_GUARD is set, and when off every function here is a no-op
// costing one already-resolved bool.
//---------------------------------------------------------------------------//

namespace mc2mem
{
    // Read once, from MC2_PAGE_GUARD. Off unless set to something other than 0.
    //
    // WHAT THIS NOW MEANS, changed 2026-08-18. Enabling the guard covers
    // gos_Malloc blocks and NOTHING ELSE. Three bands have been tried:
    //
    //   EString buffers only (b88920c) -- too narrow. EString is one of the
    //     seven crash sites; the other six allocate through UserHeap.
    //   every global new/delete (b2668d5) -- too wide. It routed Dawn's own
    //     allocations through page-granular VirtualAlloc and killed the run in
    //     shader compilation, an artifact that cost a whole session.
    //   gos_Malloc (this) -- the band the victims actually live in. ABL stack,
    //     texture nodes, TG shapes and the heap-destroy path all reach memory
    //     through UserHeap -> gos_Malloc. Dawn does not.
    bool PageGuardEnabled();

    // The global operator new/delete override, on its own switch
    // (MC2_PAGE_GUARD_GLOBAL) so that arming the guard does not silently
    // re-enable the band that destabilised Dawn. Requires PageGuardEnabled().
    bool PageGuardGlobalNewEnabled();

    // Null when the guard is off, or if the reservation fails -- callers fall
    // back to malloc, so a failure degrades to today's behaviour.
    void* PageGuardAlloc(std::size_t bytes);

    // True if this pointer came from PageGuardAlloc. Cheap: an address-range
    // test, not a lookup. Callers need it because a build can be started with
    // the guard off and blocks then come from malloc.
    bool PageGuardOwns(const void* p);

    // PAGE_NOACCESS the block and keep it. Never unmapped, so a later touch
    // faults instead of corrupting whatever would have been reused.
    void PageGuardFree(void* p);

    // Is the byte at p+offset inaccessible to this process?
    //
    // Returns 1 for a page that is PAGE_NOACCESS, guarded, or not committed;
    // 0 for a page that could be written; -1 if the host cannot say.
    //
    // This asks VirtualQuery rather than performing the write. An earlier
    // version DID perform it inside __try/__except and that was a mistake:
    // measured 2026-08-18, the handler did not catch the access violation and
    // the self-test killed the process at startup, before the run could reach
    // anything worth catching. A control must not be able to end the
    // experiment.
    int PageGuardAddressInaccessible(const void* p, std::size_t offset);

    // Record what the startup self-test found, so the HUD can show it.
    void PageGuardNoteSelfTest(int guardPageBlocked, int freedBlockBlocked);

    // One short line for the HUD, e.g. "GUARD ok fb=0 rec=0". Never empty.
    void PageGuardStatusLine(char* out, std::size_t outBytes);

    // For the census line at shutdown: how much this cost and what it caught.
    void PageGuardReport();
}
