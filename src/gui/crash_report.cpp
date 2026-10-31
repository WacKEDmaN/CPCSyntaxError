// CPCSyntaxError GUI — the crash report (crash_report.h).
//
// At crash time the heap may be broken: the report is put together in static buffers with
// snprintf and written with the system's own calls, and nothing is allocated. One crash is
// reported; a second one (in the reporter, or another thread at the same time) ends the
// program at once.
#include "crash_report.h"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>

#include "version.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <SDL.h>
#endif

namespace cpcse {

namespace {

char reportDir[1024];
char reportPath[1200];
char report[32768];
size_t reportLength = 0;
// the context: two buffers, the one to read named by `contextAt` (written whole first)
char context[2][2048];
std::atomic<int> contextAt { 0 };
std::atomic<int> crashing { 0 };

void add(const char* fmt, ...) {
    if (reportLength >= sizeof report - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(report + reportLength, sizeof report - reportLength, fmt, ap);
    va_end(ap);
    if (n > 0) reportLength = std::min(sizeof report - 1, reportLength + (size_t)n);
}

void header(const char* what) {
    reportLength = 0;
    const std::time_t now = std::time(nullptr);
    char when[64] = "";
    if (const std::tm* t = std::localtime(&now)) std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", t);
    add("CPCSyntaxError %s crashed, %s\n\n", CPCSE_VERSION, when);
    add("What happened: %s\n", what);
}

void contextPart() {
    add("\nWhat the emulator was doing:\n%s\n", context[contextAt.load() & 1]);
}

void namePath() {
    const std::time_t now = std::time(nullptr);
    char stamp[32] = "now";
    if (const std::tm* t = std::localtime(&now)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", t);
#if defined(_WIN32)
    const unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    std::snprintf(reportPath, sizeof reportPath, "%scrash-%s-%lu.txt", reportDir, stamp, pid);   // two in a second: their own files
}

bool noDialog() {
    const char* v = std::getenv("CPCSE_CRASH_NO_DIALOG");
    return v && *v && *v != '0';
}

#if defined(_WIN32)

void module(void* address) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &m) && m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char* base = std::strrchr(name, '\\');
        add("%p  %s+0x%llx\n", address, base ? base + 1 : name, (unsigned long long)((const char*)address - (const char*)m));
    } else {
        add("%p  (no module)\n", address);
    }
}

void stack() {
    void* frames[62];
    const USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    add("\nThe stack (innermost first; the reporter's own frames come first):\n");
    for (USHORT i = 0; i < n; i++) { add("  #%-2u ", (unsigned)i); module(frames[i]); }
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation (memory read or written where there is none)";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
        case EXCEPTION_INT_OVERFLOW: return "integer overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
        case EXCEPTION_IN_PAGE_ERROR: return "page in error (a file mapped into memory could not be read)";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point division by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "invalid floating-point operation";
        case 0xC0000374: return "heap corruption (memory written past what was allocated)";
        case 0xC0000409: return "stack buffer overrun";
        default: return "an exception";
    }
}

// Writes the report, says so on stderr and in a message box. Returns the program's end code.
void finish() {
    namePath();
    HANDLE f = CreateFileA(reportPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool written = false;
    if (f != INVALID_HANDLE_VALUE) {
        DWORD done = 0;
        written = WriteFile(f, report, (DWORD)reportLength, &done, nullptr) && done == reportLength;
        CloseHandle(f);
    }
    std::fwrite(report, 1, reportLength, stderr);
    std::fprintf(stderr, "\n[crash] report %s%s\n", reportPath, written ? "" : " (could not be written)");
    std::fflush(stderr);
    if (noDialog()) return;
    static char box[2048];
    const char* first = std::strstr(report, "What happened: ");
    char line[400] = "";
    if (first) {
        first += 15;
        const char* end = std::strchr(first, '\n');
        std::snprintf(line, sizeof line, "%.*s", (int)(end ? end - first : (long)std::strlen(first)), first);
    }
    std::snprintf(box, sizeof box,
                  "CPCSyntaxError has crashed and must close.\n\n%s\n\n%s\n%s\n\nPlease send this file with what you were doing.",
                  line, written ? "The report was saved as:" : "The report could not be saved; it was printed on stderr. It would have been:",
                  reportPath);
    MessageBoxA(nullptr, box, "CPCSyntaxError crashed", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_TOPMOST);
}

LONG WINAPI onException(EXCEPTION_POINTERS* ep) {
    if (crashing.exchange(1)) TerminateProcess(GetCurrentProcess(), 3);
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    char what[256];
    std::snprintf(what, sizeof what, "%s (code 0x%08lX)", exceptionName(code), (unsigned long)code);
    header(what);
    if (ep && ep->ExceptionRecord) {
        add("Where: ");
        module(ep->ExceptionRecord->ExceptionAddress);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
            add("It %s address %p\n", kind == 0 ? "read" : kind == 1 ? "wrote" : "executed", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        }
    }
#if defined(_M_X64) || defined(__x86_64__)
    if (ep && ep->ContextRecord) {
        const CONTEXT& c = *ep->ContextRecord;
        add("Registers: RIP %016llx RSP %016llx RBP %016llx\n           RAX %016llx RBX %016llx RCX %016llx RDX %016llx\n",
            (unsigned long long)c.Rip, (unsigned long long)c.Rsp, (unsigned long long)c.Rbp, (unsigned long long)c.Rax,
            (unsigned long long)c.Rbx, (unsigned long long)c.Rcx, (unsigned long long)c.Rdx);
    }
#endif
    contextPart();
    stack();
    finish();
    return EXCEPTION_EXECUTE_HANDLER;            // the program ends, with the exception's code
}

void reportAndEnd(const char* what, int code) {
    if (crashing.exchange(1)) TerminateProcess(GetCurrentProcess(), 3);
    header(what);
    contextPart();
    stack();
    finish();
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

#else   // POSIX

void stack() {
    void* frames[64];
    const int n = backtrace(frames, 64);
    add("\nThe stack (innermost first; the reporter's own frames come first):\n");
    char** names = backtrace_symbols(frames, n);          // may fail with a broken heap: addresses then
    for (int i = 0; i < n; i++) {
        if (names) add("  #%-2d %s\n", i, names[i]);
        else add("  #%-2d %p\n", i, frames[i]);
    }
}

void finish() {
    namePath();
    bool written = false;
    const int f = open(reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { written = write(f, report, reportLength) == (ssize_t)reportLength; close(f); }
    if (write(2, report, reportLength) < 0) {}
    std::fprintf(stderr, "\n[crash] report %s%s\n", reportPath, written ? "" : " (could not be written)");
    std::fflush(stderr);
    if (noDialog()) return;
    static char box[2048];
    std::snprintf(box, sizeof box, "CPCSyntaxError has crashed and must close.\n\n%s\n%s\n\nPlease send this file with what you were doing.",
                  written ? "The report was saved as:" : "The report could not be saved; it was printed on stderr. It would have been:",
                  reportPath);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CPCSyntaxError crashed", box, nullptr);
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "segmentation fault (memory read or written where there is none, or a stack overflow)";
        case SIGBUS: return "bus error";
        case SIGFPE: return "arithmetic error (a division by zero)";
        case SIGILL: return "illegal instruction";
        case SIGABRT: return "abort()";
        default: return "a signal";
    }
}

void onSignal(int sig, siginfo_t* info, void*) {
    if (crashing.exchange(1)) _exit(128 + sig);
    char what[256];
    std::snprintf(what, sizeof what, "%s (signal %d)", signalName(sig), sig);
    header(what);
    if (info && sig != SIGABRT) add("Where it went: address %p\n", info->si_addr);
    contextPart();
    stack();
    finish();
    _exit(128 + sig);
}

void reportAndEnd(const char* what, int code) {
    if (crashing.exchange(1)) _exit(code);
    header(what);
    contextPart();
    stack();
    finish();
    _exit(code);
}

#endif

// A C++ exception nobody caught (std::terminate): its message in the report.
void onTerminate() {
    static char what[600];
    std::snprintf(what, sizeof what, "std::terminate (an exception nobody caught, or a thread left running)");
    if (std::exception_ptr e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) { std::snprintf(what, sizeof what, "a C++ exception nobody caught: %s", ex.what()); }
        catch (...) { std::snprintf(what, sizeof what, "a C++ exception nobody caught (not a std::exception)"); }
    }
    reportAndEnd(what, 3);
}

#if defined(_WIN32)
void onAbort(int) { reportAndEnd("abort() (an assertion or a library gave up)", 3); }
#endif

} // namespace

void installCrashReporter(const std::string& dir) {
    std::snprintf(reportDir, sizeof reportDir, "%s", dir.c_str());
    std::snprintf(context[0], sizeof context[0], "(starting up)");
    std::set_terminate(onTerminate);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(onException);
    ULONG guarantee = 64 * 1024;                 // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   // our box, not Windows'
    std::signal(SIGABRT, onAbort);
#else
    static char altStack[64 * 1024];             // a stack overflow's handler runs here
    stack_t ss {};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT }) sigaction(sig, &sa, nullptr);
#endif
}

void setCrashContext(const std::string& text) {
    const int next = (contextAt.load() + 1) & 1;
    std::snprintf(context[next], sizeof context[next], "%s", text.c_str());
    contextAt.store(next);
}

namespace {
volatile int crashZero = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
int recurse(int n) { volatile char pad[4096]; pad[0] = (char)n; return recurse(n + 1) + pad[0]; }   // the stack overflow, on purpose
#pragma GCC diagnostic pop
}

void crashOnPurpose(const std::string& kind) {
    if (kind == "segv") { volatile int* p = nullptr; *p = 1; }
    else if (kind == "abort") std::abort();
    else if (kind == "throw") throw std::runtime_error("a test exception (--crash-test throw)");
    else if (kind == "stack") std::printf("%d\n", recurse(0));
    else if (kind == "divide") std::printf("%d\n", (std::rand() % 7 + 100) / crashZero);   // not 1/x: GCC makes that a compare
    std::fprintf(stderr, "--crash-test %s: no crash happened\n", kind.c_str());
    std::exit(2);                                // never on into the program
}

} // namespace cpcse
