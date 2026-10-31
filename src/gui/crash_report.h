// CPCSyntaxError GUI — the crash report. Should the program crash (an access violation, a
// stack overflow, a division by zero, abort(), a C++ exception nobody caught), it does not
// just vanish: it writes a report (what went wrong, where: module and offset, each frame of
// the stack; what the emulator was doing) to crash-<date>-<time>-<pid>.txt in its settings
// folder and to stderr, and shows a message box naming that file. CPCSE_CRASH_NO_DIALOG=1
// leaves the box out (the checks).
#pragma once
#include <string>

namespace cpcse {

// As early as possible in main(): every thread's crashes are reported from then on.
// `reportDir` ends with a separator (or is "": the current folder).
void installCrashReporter(const std::string& reportDir);

// What the emulator is doing (the machine, its media, the PC): kept for a report, copied
// into a buffer of its own, so a crash needs no memory and no lock to read it.
void setCrashContext(const std::string& text);

// For the checks: crash on purpose ("segv", "abort", "throw", "stack", "divide").
void crashOnPurpose(const std::string& kind);

} // namespace cpcse
