// The one translation unit that defines doctest's runner. Every test file includes doctest.h
// without the define.
#if defined(_WIN32)
// doctest's own handler turns a crash into "Unhandled SEH exception caught" and nothing else. Ours
// (below) says what the exception was, where, and the stack, which is all a CI run has to go on.
#define DOCTEST_CONFIG_NO_WINDOWS_SEH
#endif
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>

namespace {
    // Prints the exception and the stack of the thread that raised it, then ends the process (the exit
    // code is the exception's, so ctest reports a failure). Symbols come from the PDB beside the binary.
    LONG WINAPI reportCrash(EXCEPTION_POINTERS* info) {
        const EXCEPTION_RECORD& record = *info->ExceptionRecord;
        std::fprintf(stderr, "\nCRASH: exception 0x%08lX at %p on thread %lu\n", record.ExceptionCode,
                     record.ExceptionAddress, GetCurrentThreadId());
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
            const ULONG_PTR kind = record.ExceptionInformation[0];
            std::fprintf(stderr, "CRASH: access violation, %s address %p\n",
                         kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                         reinterpret_cast<void*>(record.ExceptionInformation[1]));
        }
        HANDLE process = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        for (int depth = 0; depth < 48; depth++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                break;
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512];
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != 0;
            const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != 0;
            std::fprintf(stderr, "CRASH:  #%-2d %p  %s", depth, reinterpret_cast<void*>(frame.AddrPC.Offset),
                         named ? symbol->Name : "?");
            if (lined) std::fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
        TerminateProcess(process, record.ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Installed before main, so before doctest runs anything.
    const struct InstallCrashReport {
        InstallCrashReport() {
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
            SetUnhandledExceptionFilter(reportCrash);
        }
    } installCrashReport;
}
#endif
