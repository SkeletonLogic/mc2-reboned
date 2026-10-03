// Sampling stack-walker: attach to a live PID, suspend every thread, walk every
// stack and symbolise it. This is how the 2026-08-04 mission-start hang was found
// after two sessions of reasoning from source that reads correct.
//
// WHY THIS EXISTS: there is no debugger installed on this host, and dbghelp.dll
// ships with Windows, so this needs nothing but a compiler. It answers the only
// question that matters about a hang -- which function is executing right now.
//
// Build (needs vcvars64 for the Windows SDK headers/libs):
//
//   cl /nologo /EHsc /Zi /Femc2stack.exe mc2stack.cpp dbghelp.lib
//
// Use:
//
//   .\build.ps1 -WebGPU -Symbols        # REQUIRED -- see below
//   mc2stack.exe <pid> [samples] [interval_ms]
//   mc2stack.exe 19392 5 800            # 5 samples, 800 ms apart
//
// YOU MUST RUN THE -Symbols BUILD. The shipped RelWithDebInfo config compiles
// without /Zi and links without /DEBUG, and bin\MechCmd2.pdb is a stale artefact
// matching no binary -- against it every frame prints "<no symbol>" and the tool
// looks broken when it is working perfectly. -Symbols adds /Z7 and /DEBUG in a
// separate tree with codegen otherwise unchanged.
//
// Take several samples. One sample tells you where it is; five identical samples
// tell you it is stuck there rather than passing through.

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static void walkThread(HANDLE hProc, HANDLE hThread, DWORD tid)
{
    if (SuspendThread(hThread) == (DWORD)-1) return;

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(hThread, &ctx)) { ResumeThread(hThread); return; }

    STACKFRAME64 f;
    memset(&f, 0, sizeof(f));
    f.AddrPC.Offset    = ctx.Rip; f.AddrPC.Mode    = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;

    printf("  thread %lu:\n", tid);

    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;

    for (int depth = 0; depth < 64; ++depth)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, hThread, &f, &ctx,
                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (f.AddrPC.Offset == 0) break;

        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = MAX_SYM_NAME;

        DWORD64 disp = 0;
        const char* name = "<no symbol>";
        if (SymFromAddr(hProc, f.AddrPC.Offset, &disp, sym)) name = sym->Name;

        IMAGEHLP_LINE64 line; memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        if (SymGetLineFromAddr64(hProc, f.AddrPC.Offset, &ldisp, &line))
            printf("    %2d  %s + 0x%llx   [%s:%lu]\n", depth, name,
                   (unsigned long long)disp, line.FileName, line.LineNumber);
        else
            printf("    %2d  %s + 0x%llx   (0x%llx)\n", depth, name,
                   (unsigned long long)disp, (unsigned long long)f.AddrPC.Offset);
    }
    ResumeThread(hThread);
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: mc2stack <pid> [samples] [interval_ms]\n"); return 2; }
    DWORD pid      = (DWORD)strtoul(argv[1], NULL, 10);
    int   samples  = argc > 2 ? atoi(argv[2]) : 1;
    int   interval = argc > 3 ? atoi(argv[3]) : 500;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                               PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!hProc) { printf("OpenProcess failed: %lu\n", GetLastError()); return 1; }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(hProc, NULL, TRUE))
    { printf("SymInitialize failed: %lu\n", GetLastError()); return 1; }

    for (int s = 0; s < samples; ++s)
    {
        printf("=== sample %d (pid %lu) ===\n", s, pid);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            if (Thread32First(snap, &te))
            {
                do {
                    if (te.th32OwnerProcessID != pid) continue;
                    HANDLE hT = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (!hT) continue;
                    walkThread(hProc, hT, te.th32ThreadID);
                    CloseHandle(hT);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        fflush(stdout);
        if (s + 1 < samples) Sleep(interval);
    }

    SymCleanup(hProc);
    CloseHandle(hProc);
    return 0;
}
