//===========================================================================//
// Win32 crash reporter.
//
// GameOS hands an unhandled fault to ProcessException. That used to print one
// line naming nothing, which is exactly what a real playtest crash reduced to:
// "unhandled exception reached ProcessException", exit code 0xC0000005, and no
// way to tell what faulted. The EXCEPTION_POINTERS were already being passed
// in and thrown away.
//
// This lives here rather than in gos_platform.cpp because that file defines its
// own tagPOINT and so cannot include windows.h. The win32 platform peer already
// links dbghelp, so the stack walk adds no dependency.
//
// Symbol NAMES require a build carrying debug info:
//     .\build.ps1 -WebGPU -Symbols
// Without one, every frame still reports module+offset, which can be resolved
// afterwards against a matching build. That is what a player's crash produces,
// and it is far more than the single line it replaces.
//===========================================================================//

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>
#include <dbghelp.h>

namespace {

const char* ExceptionCodeName(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case 0xC0000409:                      return "STACK_BUFFER_OVERRUN";
        default:                              return "unknown";
    }
}

// Every line goes to both sinks: stderr for whoever is watching, and a file,
// because a crash usually takes the console window with it.
void CrashPrint(std::FILE* file, const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputs(line, stderr);
    std::fflush(stderr);
    if (file)
    {
        std::fputs(line, file);
        // PORT: flush EVERY line. A real crash killed the process midway through
        // the stack walk and the buffered file was left zero bytes, while stderr
        // -- unbuffered -- kept everything. The file is the copy that survives a
        // closed console, so it cannot be the lossy one.
        std::fflush(file);
    }
}

void ReportFrame(std::FILE* out, HANDLE process, DWORD64 address, int frameIndex)
{
    char    moduleName[MAX_PATH] = "";
    DWORD64 moduleBase = SymGetModuleBase64(process, address);
    if (moduleBase)
    {
        HMODULE module = reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(moduleBase));
        if (GetModuleFileNameA(module, moduleName, sizeof(moduleName)))
        {
            const char* slash = std::strrchr(moduleName, '\\');
            if (slash) std::memmove(moduleName, slash + 1, std::strlen(slash + 1) + 1);
        }
    }

    // SYMBOL_INFO is variable length -- the name is written past the struct.
    char         storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen   = MAX_SYM_NAME;

    DWORD64    displacement = 0;
    const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol) != FALSE;

    IMAGEHLP_LINE64 lineInfo = {0};
    lineInfo.SizeOfStruct = sizeof(lineInfo);
    DWORD      lineDisplacement = 0;
    const bool haveLine =
        SymGetLineFromAddr64(process, address, &lineDisplacement, &lineInfo) != FALSE;

    CrashPrint(out, "[GameOS]   #%-2d 0x%016llX  %s+0x%llX  %s",
               frameIndex, (unsigned long long)address,
               moduleName[0] ? moduleName : "?",
               (unsigned long long)(moduleBase ? address - moduleBase : 0),
               haveSymbol ? symbol->Name : "<no symbol -- build with -Symbols>");
    if (haveSymbol && displacement)
        CrashPrint(out, "+0x%llX", (unsigned long long)displacement);
    if (haveLine)
        CrashPrint(out, "  (%s:%lu)", lineInfo.FileName, lineInfo.LineNumber);
    CrashPrint(out, "\n");
}

void WriteCrashReport(struct _EXCEPTION_POINTERS* ExceptionPointers,
                      const char* fileName, const char* mode, const char* banner)
{
    // A fault inside the reporter must not loop back into it. This is a NESTING
    // depth, not a one-shot latch: the vectored handler and the unhandled filter
    // are two separate entry points and each must still be able to report.
    static LONG reporting = 0;
    if (InterlockedIncrement(&reporting) != 1)
    {
        InterlockedDecrement(&reporting);
        return;
    }

    std::FILE* out = std::fopen(fileName, mode);

    CrashPrint(out, "%s", banner);

    if (!ExceptionPointers || !ExceptionPointers->ExceptionRecord)
    {
        CrashPrint(out, "[GameOS] no exception record was supplied\n");
        std::fflush(stderr);
        if (out) std::fclose(out);
        InterlockedDecrement(&reporting);
        return;
    }

    const EXCEPTION_RECORD* record = ExceptionPointers->ExceptionRecord;
    CrashPrint(out, "[GameOS] code 0x%08lX (%s) at 0x%016llX\n",
               record->ExceptionCode, ExceptionCodeName(record->ExceptionCode),
               (unsigned long long)(ULONG_PTR)record->ExceptionAddress);

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        CrashPrint(out, "[GameOS] %s address 0x%016llX\n",
                   kind == 0 ? "read from" : (kind == 1 ? "write to" : "execute at"),
                   (unsigned long long)record->ExceptionInformation[1]);
    }

    // PORT: walk the stack WITHOUT the heap first.
    //
    // A real crash (0xC0000374, heap corruption) killed the process inside
    // StackWalk64 -- dbghelp allocates, and the heap was the thing that was
    // broken. The report died after printing "stack:" and named nothing.
    //
    // RtlLookupFunctionEntry + RtlVirtualUnwind are the x64 unwinder ntdll uses
    // itself and touch no heap, and VirtualQuery/GetModuleFileNameA resolve the
    // module without allocating either. So the addresses land on disk even when
    // the allocator is unusable; symbol names are attempted afterwards, as a
    // bonus rather than a prerequisite.
#if defined(_M_X64)
    if (ExceptionPointers->ContextRecord)
    {
        CONTEXT walk = *ExceptionPointers->ContextRecord;
        CrashPrint(out, "[GameOS] stack, module+offset (heap-free unwind):\n");
        for (int frame = 0; frame < 64 && walk.Rip; ++frame)
        {
            char  moduleName[MAX_PATH] = "";
            void* moduleBase = 0;
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((LPCVOID)walk.Rip, &mbi, sizeof(mbi)) == sizeof(mbi))
            {
                moduleBase = mbi.AllocationBase;
                if (moduleBase &&
                    GetModuleFileNameA((HMODULE)moduleBase, moduleName, sizeof(moduleName)))
                {
                    const char* slash = std::strrchr(moduleName, '\\');
                    if (slash) std::memmove(moduleName, slash + 1, std::strlen(slash + 1) + 1);
                }
            }
            CrashPrint(out, "[GameOS]   #%-2d 0x%016llX  %s+0x%llX\n",
                       frame, (unsigned long long)walk.Rip,
                       moduleName[0] ? moduleName : "?",
                       (unsigned long long)(moduleBase ? (ULONG_PTR)walk.Rip - (ULONG_PTR)moduleBase : 0));

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(walk.Rip, &imageBase, NULL);
            if (!function)
            {
                // Leaf function: the return address is on top of the stack.
                if (!walk.Rsp) break;
                walk.Rip = *(DWORD64*)walk.Rsp;
                walk.Rsp += 8;
            }
            else
            {
                PVOID  handlerData = NULL;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, function,
                                 &walk, &handlerData, &establisherFrame, NULL);
            }
            if (!walk.Rip) break;
        }
    }
#endif

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // Already initialised is NOT a failure. The B1-1 probe's report path calls
    // SymInitialize at startup, and a second call then returns FALSE with
    // ERROR_INVALID_PARAMETER -- which silently cost this report its symbols on
    // 2026-08-18. Only clean up what we actually opened.
    const BOOL symOpened = SymInitialize(process, NULL, TRUE);
    const bool symReady  = symOpened || GetLastError() == ERROR_INVALID_PARAMETER;
    if (symReady && ExceptionPointers->ContextRecord)
    {
        // StackWalk64 modifies the context it is given, so it gets a copy.
        CONTEXT      context = *ExceptionPointers->ContextRecord;
        STACKFRAME64 frame   = {0};
#if defined(_M_X64)
        const DWORD machine    = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset    = context.Rip;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
        const DWORD machine    = IMAGE_FILE_MACHINE_ARM64;
        frame.AddrPC.Offset    = context.Pc;
        frame.AddrFrame.Offset = context.Fp;
        frame.AddrStack.Offset = context.Sp;
#else
        const DWORD machine    = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset    = context.Eip;
        frame.AddrFrame.Offset = context.Ebp;
        frame.AddrStack.Offset = context.Esp;
#endif
        frame.AddrPC.Mode    = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Mode = AddrModeFlat;

        CrashPrint(out, "[GameOS] symbolised stack (dbghelp -- may fail if the heap is corrupt):\n");
        for (int i = 0; i < 64; ++i)
        {
            if (!StackWalk64(machine, process, thread, &frame, &context, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            if (frame.AddrPC.Offset == 0)
                break;
            ReportFrame(out, process, frame.AddrPC.Offset, i);
        }
        if (symOpened) SymCleanup(process);
    }
    else
    {
        CrashPrint(out, "[GameOS] no symbol handler and no context -- no stack available (SymInitialize %lu)\n",
                   GetLastError());
    }

    CrashPrint(out, "[GameOS] full report also written to %s\n", fileName);
    std::fflush(stderr);
    if (out) std::fclose(out);
    InterlockedDecrement(&reporting);
}

// PORT: the unhandled-exception filter is not a reliable net.
//
// On 2026-08-12 the owner's session died with 0xC0000005 in ucrtbase!strcpy.
// Windows Error Reporting logged the fault, so the process really did take an
// unhandled access violation -- and DwExceptionFilter never ran: every one of
// its paths calls ProcessException, and no crash-report.txt was written. The
// filter is installed exactly once, from InitDW, and nothing else in the tree
// calls SetUnhandledExceptionFilter, so WHY it was bypassed is NOT established.
//
// A vectored handler does not depend on that filter, nor on any of Dr Watson's
// shared-memory setup succeeding. It runs first-chance, before SEH unwinding,
// so its report reaches disk whatever happens afterwards.
//
// It reports only codes this tree never raises deliberately. The game contains
// no __try/__except at all, so nothing here generates a legitimate first-chance
// access violation. Reports are numbered and capped, so a driver that does use
// faults as control flow shows up as a run of numbered reports instead of
// silently overwriting the one that matters. It always returns
// EXCEPTION_CONTINUE_SEARCH: it observes, it never changes what happens next.
bool IsFatalExceptionCode(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case 0xC0000374:            // heap corruption
        case 0xC0000409:            // stack buffer overrun
            return true;
        default:
            return false;           // C++ throws, breakpoints, thread names, ...
    }
}

const long kMaxVectoredReports = 8;
long g_vectoredReports = 0;

long CALLBACK VectoredCrashHandler(struct _EXCEPTION_POINTERS* ExceptionPointers)
{
    if (!ExceptionPointers || !ExceptionPointers->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!IsFatalExceptionCode(ExceptionPointers->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    const long index = InterlockedIncrement(&g_vectoredReports);
    if (index > kMaxVectoredReports)
        return EXCEPTION_CONTINUE_SEARCH;

    char banner[256];
    std::snprintf(banner, sizeof(banner),
                  "[GameOS] vectored handler saw a first-chance fault (report %ld of "
                  "at most %ld; the process may or may not survive it)\n",
                  index, kMaxVectoredReports);
    WriteCrashReport(ExceptionPointers, "crash-vectored.txt",
                     index == 1 ? "w" : "a", banner);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace


// Returns EXCEPTION_CONTINUE_SEARCH, as before, so a debugger still sees the
// fault. The only change is that the fault is now described first.
long __cdecl ProcessException(struct _EXCEPTION_POINTERS* ExceptionPointers)
{
    WriteCrashReport(ExceptionPointers, "crash-report.txt", "w",
                     "[GameOS] unhandled exception reached ProcessException\n");
    return 0;   // EXCEPTION_CONTINUE_SEARCH
}


void InstallVectoredCrashReporter()
{
    // First = 1 so this runs ahead of any handler registered later. Nothing else
    // in the tree registers one, so the ordering only matters against Dawn.
    if (!AddVectoredExceptionHandler(1, VectoredCrashHandler))
        std::fprintf(stderr, "[GameOS] AddVectoredExceptionHandler failed (%lu)\n",
                     GetLastError());
    else
        std::fprintf(stderr, "[GameOS] vectored crash reporter installed\n");
    std::fflush(stderr);
}


// PORT: MC2_CRASH_SELFTEST=1 faults on purpose, right after the handlers are
// installed, and the process dies. That is the point.
//
// The recurring and most expensive mistake in this project is trusting a probe
// without checking what it actually proves. A crash reporter is the worst case
// of that: it is only ever exercised at the moment it is needed, and the
// 2026-08-12 session is the second time one has silently not run. This makes
// the whole pipeline -- handler installed, fault caught, stack walked, symbols
// resolved, file on disk -- checkable in one command, at any time, with a
// clearly negative outcome: no crash-vectored.txt, or one with no symbol names.
// MC2_CRASH_SELFTEST=1 or =av    a null store: an ordinary access violation,
//                                which both in-process handlers should catch.
// MC2_CRASH_SELFTEST=fastfail    __fastfail, the path heap corruption and /GS
//                                take. It bypasses EVERY user-mode handler by
//                                design, so this one is expected to produce NO
//                                crash-report.txt and NO crash-vectored.txt.
//                                That negative is the point: it is the case
//                                only mc2-crashwatch can describe, and running
//                                it with and without the watchdog is the proof.
void CrashReporterSelfTest()
{
    const char* wanted = std::getenv("MC2_CRASH_SELFTEST");
    if (!wanted || !*wanted || std::strcmp(wanted, "0") == 0) return;

    if (std::strcmp(wanted, "fastfail") == 0)
    {
        std::fprintf(stderr, "[GameOS] MC2_CRASH_SELFTEST=fastfail: raising __fastfail, "
                             "which bypasses every user-mode handler\n");
        std::fflush(stderr);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }

    std::fprintf(stderr, "[GameOS] MC2_CRASH_SELFTEST: raising a deliberate "
                         "access violation to prove the reporter works\n");
    std::fflush(stderr);

    // Volatile pointer through a volatile variable so this is a real store the
    // optimiser cannot fold into an unreachable trap.
    static int* volatile target = 0;
    *target = 1;
}
