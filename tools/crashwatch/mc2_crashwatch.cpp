//===========================================================================//
// mc2-crashwatch -- run the game under a debugger loop so a crash cannot get
// away without being described.
//
// WHY THIS EXISTS
//
// In-process crash handlers keep not running. Two separate mechanisms defeat
// them, and this project has now lost diagnostic information to both:
//
//  1. __fastfail. Heap corruption (0xC0000374) and /GS stack-buffer overrun
//     (0xC0000409) are raised through a path that deliberately bypasses every
//     user-mode handler -- no unhandled-exception filter, no vectored handler,
//     no crash-report.txt, no trailing output. That is the whole reason the
//     [teardown] breadcrumb census exists: a flushed trail was the ONLY way in.
//
//  2. On 2026-08-12 an ordinary unhandled 0xC0000005 in ucrtbase!strcpy was
//     recorded by Windows Error Reporting while DwExceptionFilter produced no
//     crash-report.txt at all. Why it was bypassed is still not established.
//
// A debugger sees both. Every exception in the debuggee is reported to the
// debugger BEFORE any user-mode handler gets it, and fast-fail is delivered as
// a debug event too. Capturing from outside the process also means the report
// does not depend on the debuggee's heap, stack or CRT still being usable --
// which is exactly the state a crashing process is in.
//
// WHAT IT PRODUCES, per crash, in the working directory:
//
//   crash-watch.txt        exception code, faulting module+offset, the access
//                          that failed, the full register file, a symbolised
//                          stack of the faulting thread, and the memory behind
//                          the pointer-passing registers
//   crash-<pid>-<n>.dmp    a minidump, openable in Visual Studio, for anything
//                          the text report does not answer
//
// WHAT IT COSTS, stated so it is not discovered later. The debuggee runs with a
// debugger attached. That is observable: `IsDebuggerPresent` returns true (this
// tree never calls it), timing changes slightly, and Windows would normally
// switch the process to the debug heap. `_NO_DEBUG_HEAP=1` is set in the child
// environment specifically to prevent that last one, because a different
// allocator would change the behaviour of the heap-corruption defect this is
// meant to catch.
//
// Usage:  mc2-crashwatch.exe <game.exe> [args...]
// The child inherits stdin/stdout/stderr, so redirects around this program
// apply to the game exactly as if it had been launched directly. The exit code
// is the child's, so playtest-exits.log keeps recording what it always did.
//===========================================================================//

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

namespace {

const int kMaxCaptures = 4;

std::FILE*  g_report   = nullptr;
int         g_captures = 0;
std::string g_symbolSearchPath;   // the directory holding the game exe and its PDB

void Say(const char* format, ...)
{
    char    line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputs(line, stderr);
    std::fflush(stderr);
    if (g_report)
    {
        std::fputs(line, g_report);
        std::fflush(g_report);   // a crash must not take the buffer with it
    }
}

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
        case 0xC0000374:                      return "HEAP_CORRUPTION (__fastfail)";
        case 0xC0000409:                      return "STACK_BUFFER_OVERRUN (__fastfail)";
        case 0xE06D7363:                      return "C++ exception";
        case 0x406D1388:                      return "thread name";
        default:                              return "unknown";
    }
}

// Codes worth stopping the world for. C++ throws and the thread-naming
// exception are excluded: both are normal traffic, not faults.
bool IsFatal(DWORD code)
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
        case 0xC0000374:
        case 0xC0000409:
            return true;
        default:
            return false;
    }
}

// __fastfail codes never get a second chance -- the OS tears the process down
// after the first. They have to be captured on sight.
bool NeverGetsSecondChance(DWORD code)
{
    return code == 0xC0000374 || code == 0xC0000409;
}

std::string ModuleAt(HANDLE process, DWORD64 address, DWORD64* moduleBase)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQueryEx(process, (LPCVOID)address, &mbi, sizeof(mbi))) return "";
    if (moduleBase) *moduleBase = (DWORD64)(ULONG_PTR)mbi.AllocationBase;
    char name[MAX_PATH] = "";
    if (!GetModuleFileNameExA(process, (HMODULE)mbi.AllocationBase, name, sizeof(name)))
        return "";
    const char* slash = std::strrchr(name, '\\');
    return slash ? slash + 1 : name;
}

void DescribeAddress(HANDLE process, DWORD64 address, const char* label)
{
    DWORD64 base = 0;
    const std::string module = ModuleAt(process, address, &base);

    char         storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {0};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen   = MAX_SYM_NAME;
    DWORD64    displacement = 0;
    const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol) != FALSE;

    IMAGEHLP_LINE64 lineInfo = {0};
    lineInfo.SizeOfStruct = sizeof(lineInfo);
    DWORD      lineDelta = 0;
    const bool haveLine = SymGetLineFromAddr64(process, address, &lineDelta, &lineInfo) != FALSE;

    Say("[crashwatch] %s 0x%016llX  %s+0x%llX", label, (unsigned long long)address,
        module.empty() ? "?" : module.c_str(),
        (unsigned long long)(base ? address - base : 0));
    if (haveSymbol) Say("  %s+0x%llX", symbol->Name, (unsigned long long)displacement);
    if (haveLine)   Say("  (%s:%lu)", lineInfo.FileName, lineInfo.LineNumber);
    Say("\n");
}

// For a fault inside a CRT string or memory routine the interesting evidence is
// what it was handed. On x64 the first four integer arguments are RCX, RDX, R8,
// R9, so printing the bytes behind them usually shows the source string that
// had no terminator, or the destination that was never allocated.
void DumpPointerArgument(HANDLE process, DWORD64 value, const char* name)
{
    if (!value)
    {
        Say("[crashwatch]   %s = 0x%016llX  (null)\n", name, (unsigned long long)value);
        return;
    }

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQueryEx(process, (LPCVOID)value, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT)
    {
        Say("[crashwatch]   %s = 0x%016llX  (not committed memory)\n",
            name, (unsigned long long)value);
        return;
    }

    unsigned char bytes[64] = {0};
    SIZE_T        read = 0;
    if (!ReadProcessMemory(process, (LPCVOID)value, bytes, sizeof(bytes) - 1, &read) || !read)
    {
        Say("[crashwatch]   %s = 0x%016llX  (unreadable)\n", name, (unsigned long long)value);
        return;
    }

    // Printable prefix, then a hex tail, then whether a terminator was found at
    // all -- which is the question that matters for a strcpy fault.
    char text[80];
    size_t out = 0;
    for (size_t i = 0; i < read && out < sizeof(text) - 1; ++i)
    {
        const unsigned char c = bytes[i];
        if (c == 0) break;
        text[out++] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    text[out] = 0;

    bool terminated = false;
    for (SIZE_T i = 0; i < read; ++i) if (bytes[i] == 0) { terminated = true; break; }

    Say("[crashwatch]   %s = 0x%016llX  \"%s\"%s  (%llu bytes readable, %s)\n",
        name, (unsigned long long)value, text,
        out == sizeof(text) - 1 ? "..." : "",
        (unsigned long long)read,
        terminated ? "NUL found within 63 bytes"
                   : "NO NUL in the first 63 bytes -- unterminated source is possible");
}

void WriteMiniDump(HANDLE process, DWORD pid, DWORD threadId,
                   EXCEPTION_RECORD* record, CONTEXT* context, int index)
{
    char fileName[MAX_PATH];
    std::snprintf(fileName, sizeof(fileName), "crash-%lu-%d.dmp", pid, index);

    HANDLE file = CreateFileA(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        Say("[crashwatch] could not create %s (%lu)\n", fileName, GetLastError());
        return;
    }

    EXCEPTION_POINTERS pointers = { record, context };
    MINIDUMP_EXCEPTION_INFORMATION info = {0};
    info.ThreadId          = threadId;
    info.ExceptionPointers = &pointers;
    info.ClientPointers    = FALSE;   // the records live in THIS process

    // Not MiniDumpWithFullMemory: that is the whole address space, gigabytes for
    // a loaded mission.
    //
    // The richest flag combination is tried first and the ladder falls back on
    // failure, because the first version of this returned ERROR_INVALID_USER_BUFFER
    // and wrote a 15 KB stub -- a dump that exists but is useless is worse than
    // one that is honestly reported as absent. Whichever rung succeeds is named
    // in the report, so the dump is never over-trusted either.
    // MiniDumpWithPrivateReadWriteMemory is the one that matters and it was
    // missing from the first version. The 08/12 shop crash needed two heap
    // values -- the shape's node count and the block behind the destination
    // pointer -- and NEITHER was in the dump: indirect+scan captured the stack
    // and what it pointed at, not the game's own heaps. A dump that cannot
    // answer the question it was taken for is not worth the disk it saved.
    // Private R/W pages ARE the heap, so this costs size and buys the answer.
    struct Rung { MINIDUMP_TYPE type; const char* name; };
    const Rung ladder[] = {
        { (MINIDUMP_TYPE)(MiniDumpWithPrivateReadWriteMemory |
                          MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                          MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo |
                          MiniDumpWithUnloadedModules | MiniDumpWithDataSegs |
                          MiniDumpWithHandleData),
          "privateRW+indirect+scan+threaddata+datasegs+handles" },
        { (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                          MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo |
                          MiniDumpWithUnloadedModules | MiniDumpWithDataSegs |
                          MiniDumpWithHandleData),
          "indirect+scan+threaddata+datasegs+handles" },
        { (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                          MiniDumpWithDataSegs | MiniDumpWithHandleData),
          "threadinfo+datasegs+handles" },
        { (MINIDUMP_TYPE)(MiniDumpWithDataSegs), "datasegs" },
        { MiniDumpNormal, "normal (stacks only)" },
    };

    BOOL        ok       = FALSE;
    const char* usedName = "";
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); ++i)
    {
        SetFilePointer(file, 0, NULL, FILE_BEGIN);
        SetEndOfFile(file);
        ok = MiniDumpWriteDump(process, pid, file, ladder[i].type, &info, NULL, NULL);
        if (ok) { usedName = ladder[i].name; break; }
        Say("[crashwatch] minidump flags '%s' rejected (%lu); falling back\n",
            ladder[i].name, GetLastError());
    }

    // Last resort, and a bisect: no MINIDUMP_EXCEPTION_INFORMATION at all. If
    // this one succeeds where every rung above failed, the rejection is in the
    // exception record rather than the flags, and the report says so instead of
    // leaving a silent zero-byte file.
    if (!ok)
    {
        SetFilePointer(file, 0, NULL, FILE_BEGIN);
        SetEndOfFile(file);
        ok = MiniDumpWriteDump(process, pid, file, MiniDumpNormal, NULL, NULL, NULL);
        if (ok)
        {
            usedName = "normal, WITHOUT exception info -- the exception record was "
                       "what dbghelp rejected; the faulting thread is named above";
        }
    }
    CloseHandle(file);

    if (ok)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        double megabytes = 0.0;
        if (GetFileAttributesExA(fileName, GetFileExInfoStandard, &attributes))
            megabytes = ((double)attributes.nFileSizeLow +
                         (double)attributes.nFileSizeHigh * 4294967296.0) / 1048576.0;
        Say("[crashwatch] minidump written: %s (%.1f MB, flags: %s) -- open it in Visual Studio\n",
            fileName, megabytes, usedName);
    }
    else
    {
        Say("[crashwatch] MiniDumpWriteDump failed (%lu)\n", GetLastError());
    }
}

void Capture(HANDLE process, HANDLE thread, DWORD pid, DWORD threadId,
             EXCEPTION_RECORD* record, bool firstChance)
{
    ++g_captures;
    const int index = g_captures;

    if (!g_report) g_report = std::fopen("crash-watch.txt", "w");

    // Symbols are initialised HERE, not at startup.
    //
    // The first working version called SymInitialize right after CreateProcess,
    // where the debuggee is still stopped at the loader breakpoint with nothing
    // but its own image mapped. fInvadeProcess had nothing to enumerate, the
    // call failed, SymRefreshModuleList then returned ERROR_INVALID_HANDLE, and
    // every frame came back as module+offset with no name. At capture time the
    // module list is complete, and this is the only moment symbols are needed.
    static bool symbolsReady = false;
    if (!symbolsReady)
    {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (SymInitialize(process, g_symbolSearchPath.empty() ? NULL
                                                              : g_symbolSearchPath.c_str(),
                          TRUE))
            symbolsReady = true;
        else
            Say("[crashwatch] SymInitialize failed (%lu) -- frames will be "
                "module+offset only\n", GetLastError());
    }
    else if (!SymRefreshModuleList(process))
    {
        Say("[crashwatch] SymRefreshModuleList failed (%lu); names may be missing\n",
            GetLastError());
    }

    Say("\n[crashwatch] ==================== capture %d of at most %d ====================\n",
        index, kMaxCaptures);
    Say("[crashwatch] code 0x%08lX (%s), %s chance, pid %lu thread %lu\n",
        record->ExceptionCode, ExceptionCodeName(record->ExceptionCode),
        firstChance ? "first" : "second", pid, threadId);

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        Say("[crashwatch] tried to %s address 0x%016llX\n",
            kind == 0 ? "READ" : (kind == 1 ? "WRITE" : "EXECUTE"),
            (unsigned long long)record->ExceptionInformation[1]);
    }

    CONTEXT context = {0};
    context.ContextFlags = CONTEXT_ALL;
    const bool haveContext = GetThreadContext(thread, &context) != FALSE;
    if (!haveContext)
        Say("[crashwatch] GetThreadContext failed (%lu) -- no stack or registers\n",
            GetLastError());

    DescribeAddress(process, (DWORD64)(ULONG_PTR)record->ExceptionAddress, "faulted at");

#if defined(_M_X64)
    if (haveContext)
    {
        Say("[crashwatch] registers:\n");
        Say("[crashwatch]   rip=%016llX rsp=%016llX rbp=%016llX flags=%08lX\n",
            context.Rip, context.Rsp, context.Rbp, context.EFlags);
        Say("[crashwatch]   rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n",
            context.Rax, context.Rbx, context.Rcx, context.Rdx);
        Say("[crashwatch]   rsi=%016llX rdi=%016llX r8 =%016llX r9 =%016llX\n",
            context.Rsi, context.Rdi, context.R8, context.R9);
        Say("[crashwatch]   r10=%016llX r11=%016llX r12=%016llX r13=%016llX\n",
            context.R10, context.R11, context.R12, context.R13);
        Say("[crashwatch]   r14=%016llX r15=%016llX\n", context.R14, context.R15);

        Say("[crashwatch] the x64 argument registers, read as strings:\n");
        DumpPointerArgument(process, context.Rcx, "rcx (arg 1)");
        DumpPointerArgument(process, context.Rdx, "rdx (arg 2)");
        DumpPointerArgument(process, context.R8,  "r8  (arg 3)");
        DumpPointerArgument(process, context.R9,  "r9  (arg 4)");

        Say("[crashwatch] stack of the faulting thread:\n");
        CONTEXT      walk  = context;
        STACKFRAME64 frame = {0};
        frame.AddrPC.Offset    = walk.Rip;
        frame.AddrFrame.Offset = walk.Rbp;
        frame.AddrStack.Offset = walk.Rsp;
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 64; ++i)
        {
            // A null ReadMemoryRoutine makes StackWalk64 use ReadProcessMemory
            // against the handle given, which is the debuggee -- so this walk
            // never touches the crashing process's own heap or CRT.
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walk,
                             NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            if (!frame.AddrPC.Offset) break;
            char label[32];
            std::snprintf(label, sizeof(label), "  #%-2d", i);
            DescribeAddress(process, frame.AddrPC.Offset, label);
        }
    }
#endif

    if (haveContext)
        WriteMiniDump(process, pid, threadId, record, &context, index);

    Say("[crashwatch] ================== end of capture %d ==================\n\n", index);
}

} // namespace


int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr,
                     "usage: mc2-crashwatch.exe <game.exe> [args...]\n"
                     "Runs the game under a debugger loop and describes any crash.\n");
        return 2;
    }

    // Rebuild the command line, quoting each argument, because CreateProcess
    // takes one string and paths here contain spaces.
    std::string commandLine;
    for (int i = 1; i < argc; ++i)
    {
        if (i > 1) commandLine += ' ';
        commandLine += '"';
        commandLine += argv[i];
        commandLine += '"';
    }

    // Without this Windows gives a debugged process the debug heap, which has
    // different behaviour and different timing from the real one -- and heap
    // behaviour is precisely what several open defects here turn on.
    SetEnvironmentVariableA("_NO_DEBUG_HEAP", "1");

    STARTUPINFOA        si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);

    std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back('\0');

    if (!CreateProcessA(NULL, mutableCommandLine.data(), NULL, NULL, TRUE,
                        DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi))
    {
        std::fprintf(stderr, "[crashwatch] could not start %s (%lu)\n",
                     argv[1], GetLastError());
        return 3;
    }

    std::fprintf(stderr, "[crashwatch] watching pid %lu -- %s\n", pi.dwProcessId, argv[1]);
    std::fflush(stderr);

    // Symbols are initialised lazily, at the moment of a crash -- see Capture().
    // Doing it here does not work: the debuggee is still at the loader
    // breakpoint with nothing mapped. Record where its PDB lives instead.
    {
        char full[MAX_PATH] = "";
        if (GetFullPathNameA(argv[1], sizeof(full), full, NULL))
        {
            char* slash = std::strrchr(full, '\\');
            if (slash) { *slash = 0; g_symbolSearchPath = full; }
        }
    }

    std::map<DWORD, HANDLE> threads;
    threads[pi.dwThreadId] = pi.hThread;

    DWORD exitCode = 0;
    bool  running  = true;
    while (running)
    {
        DEBUG_EVENT event = {0};
        if (!WaitForDebugEvent(&event, INFINITE)) break;

        DWORD continueStatus = DBG_CONTINUE;

        switch (event.dwDebugEventCode)
        {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (event.u.CreateProcessInfo.hFile)
                    CloseHandle(event.u.CreateProcessInfo.hFile);
                threads[event.dwThreadId] = event.u.CreateProcessInfo.hThread;
                break;

            case CREATE_THREAD_DEBUG_EVENT:
                threads[event.dwThreadId] = event.u.CreateThread.hThread;
                break;

            case EXIT_THREAD_DEBUG_EVENT:
                threads.erase(event.dwThreadId);
                break;

            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
                break;

            case EXCEPTION_DEBUG_EVENT:
            {
                EXCEPTION_RECORD* record = &event.u.Exception.ExceptionRecord;
                const bool firstChance = event.u.Exception.dwFirstChance != 0;

                // The loader breakpoint: expected, and ours to swallow.
                if (record->ExceptionCode == EXCEPTION_BREAKPOINT && firstChance)
                    break;

                if (IsFatal(record->ExceptionCode) && g_captures < kMaxCaptures)
                {
                    // Second chance means no user-mode handler took it, which is
                    // the definitive moment. Fast-fail never reaches a second
                    // chance, so it is captured on sight instead.
                    const bool capture =
                        !firstChance || NeverGetsSecondChance(record->ExceptionCode);
                    if (capture)
                    {
                        std::map<DWORD, HANDLE>::iterator found = threads.find(event.dwThreadId);
                        if (found != threads.end())
                            Capture(pi.hProcess, found->second, event.dwProcessId,
                                    event.dwThreadId, record, firstChance);
                        else
                            Say("[crashwatch] fault on unknown thread %lu -- no handle\n",
                                event.dwThreadId);
                    }
                }
                // Always hand it back unhandled: the game's own handlers still
                // run, and a genuinely unhandled fault still kills the process
                // with the same exit code it always had.
                continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                break;
            }

            case EXIT_PROCESS_DEBUG_EVENT:
                exitCode = event.u.ExitProcess.dwExitCode;
                running  = false;
                break;

            default:
                break;
        }

        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continueStatus))
            break;
    }

    if (g_captures) SymCleanup(pi.hProcess);
    if (g_report) std::fclose(g_report);

    std::fprintf(stderr, "[crashwatch] child exited with %lu (0x%08lX); %d crash%s captured\n",
                 exitCode, exitCode, g_captures, g_captures == 1 ? "" : "es");
    std::fflush(stderr);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exitCode;
}
