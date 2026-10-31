// CPCSyntaxError — a program run with pipes (see host_process.h).
#include "host_process.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cpcse {

struct HostProcess::Impl {
    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<unsigned char>> toWrite;
    bool inputClosing = false, stopping = false, outputEnded = false;
    std::vector<unsigned char> out;
    size_t outAt = 0;
    std::string err;
    std::thread writer, reader, errReader;
    size_t cap = 4u << 20;
#ifdef _WIN32
    HANDLE process = nullptr, in = nullptr, outR = nullptr, errR = nullptr;
    static bool put(HANDLE h, const unsigned char* p, size_t n) {
        while (n) {
            DWORD done = 0;
            if (!WriteFile(h, p, (DWORD)std::min<size_t>(n, 1 << 16), &done, nullptr) || done == 0) return false;
            p += done; n -= done;
        }
        return true;
    }
    static long get(HANDLE h, unsigned char* p, size_t n) {
        DWORD done = 0;
        return ReadFile(h, p, (DWORD)n, &done, nullptr) ? (long)done : -1;
    }
    void closeIn() { if (in) { CloseHandle(in); in = nullptr; } }
#else
    pid_t pid = -1;
    int in = -1, outR = -1, errR = -1;
    static bool put(int fd, const unsigned char* p, size_t n) {
        while (n) {
            const ssize_t done = ::write(fd, p, n);
            if (done <= 0) return false;
            p += done; n -= (size_t)done;
        }
        return true;
    }
    static long get(int fd, unsigned char* p, size_t n) { return (long)::read(fd, p, n); }
    void closeIn() { if (in >= 0) { ::close(in); in = -1; } }
#endif

    void startThreads(bool withInput) {
        if (withInput)
            writer = std::thread([this] {
                for (;;) {
                    std::vector<unsigned char> chunk;
                    {
                        std::unique_lock<std::mutex> l(m);
                        cv.wait(l, [&] { return stopping || inputClosing || !toWrite.empty(); });
                        if (stopping) break;
                        if (toWrite.empty()) break;    // closing, all written
                        chunk = std::move(toWrite.front());
                        toWrite.pop_front();
                    }
                    if (!put(in, chunk.data(), chunk.size())) break;
                }
                std::lock_guard<std::mutex> l(m);
                closeIn();
            });
        reader = std::thread([this] {
            std::vector<unsigned char> buf(1 << 16);
            for (;;) {
                const long n = get(outR, buf.data(), buf.size());
                if (n <= 0) break;
                std::unique_lock<std::mutex> l(m);
                if (outAt > (1u << 20)) { out.erase(out.begin(), out.begin() + (long)outAt); outAt = 0; }
                out.insert(out.end(), buf.begin(), buf.begin() + n);
                cv.wait(l, [&] { return stopping || out.size() - outAt < cap; });
                if (stopping) break;
            }
            std::lock_guard<std::mutex> l(m);
            outputEnded = true;
        });
        errReader = std::thread([this] {
            unsigned char buf[4096];
            for (;;) {
                const long n = get(errR, buf, sizeof buf);
                if (n <= 0) break;
                std::lock_guard<std::mutex> l(m);
                if (err.size() < 65536) err.append((const char*)buf, (size_t)n);
            }
        });
    }
};

HostProcess::HostProcess() : impl(std::make_unique<Impl>()) {}
HostProcess::~HostProcess() { stop(); }

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
// one argument as the C runtime reads it back: quoted when it holds a space or a quote
static std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\'); else q.append(slashes, L'\\');
        slashes = 0;
        q.push_back(c);
    }
    q.append(slashes * 2, L'\\');
    q.push_back(L'"');
    return q;
}
#endif

bool HostProcess::start(const std::vector<std::string>& args, bool withInput) {
    Impl& p = *impl;
    if (args.empty()) return false;
    p.cap = outputCap;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE inR = nullptr, outW = nullptr, errW = nullptr;
    if (withInput && !CreatePipe(&inR, &p.in, &sa, 1 << 16)) return false;
    if (!CreatePipe(&p.outR, &outW, &sa, 1 << 16) || !CreatePipe(&p.errR, &errW, &sa, 1 << 12)) return false;
    if (p.in) SetHandleInformation(p.in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p.outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p.errR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = withInput ? nullptr : CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = withInput ? inR : nul;
    si.hStdOutput = outW;
    si.hStdError = errW;
    std::wstring line;
    for (const std::string& a : args) { if (!line.empty()) line += L' '; line += quote(widen(a)); }
    std::vector<wchar_t> cmd(line.begin(), line.end());
    cmd.push_back(0);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (inR) CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        p.closeIn();
        CloseHandle(p.outR); p.outR = nullptr;
        CloseHandle(p.errR); p.errR = nullptr;
        return false;
    }
    CloseHandle(pi.hThread);
    p.process = pi.hProcess;
#else
    std::signal(SIGPIPE, SIG_IGN);                     // a decoder that quits: write() fails instead
    int inPipe[2] = { -1, -1 }, outPipe[2], errPipe[2];
    if ((withInput && pipe(inPipe) != 0) || pipe(outPipe) != 0 || pipe(errPipe) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (withInput) posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", 0, 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errPipe[1], 2);
    for (int fd : { inPipe[0], inPipe[1], outPipe[0], outPipe[1], errPipe[0], errPipe[1] })
        if (fd >= 0) posix_spawn_file_actions_addclose(&fa, fd);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const int rc = posix_spawnp(&p.pid, args[0].c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (withInput) ::close(inPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    p.in = withInput ? inPipe[1] : -1;
    p.outR = outPipe[0];
    p.errR = errPipe[0];
    if (rc != 0) { p.pid = -1; p.closeIn(); ::close(p.outR); ::close(p.errR); p.outR = p.errR = -1; return false; }
#endif
    p.startThreads(withInput);
    return true;
}

void HostProcess::write(const unsigned char* data, size_t n) {
    if (!n) return;
    {
        std::lock_guard<std::mutex> l(impl->m);
        if (impl->inputClosing || impl->stopping) return;
        // a program that stopped reading cannot make this grow without end: 8 MB waiting at most
        size_t queued = 0;
        for (const auto& c : impl->toWrite) queued += c.size();
        if (queued + n > (8u << 20)) return;
        impl->toWrite.emplace_back(data, data + n);
    }
    impl->cv.notify_all();
}

void HostProcess::closeInput() {
    { std::lock_guard<std::mutex> l(impl->m); impl->inputClosing = true; }
    impl->cv.notify_all();
}

size_t HostProcess::available() const {
    std::lock_guard<std::mutex> l(impl->m);
    return impl->out.size() - impl->outAt;
}

size_t HostProcess::read(unsigned char* to, size_t n) {
    size_t k;
    {
        std::lock_guard<std::mutex> l(impl->m);
        k = std::min(n, impl->out.size() - impl->outAt);
        if (k) std::memcpy(to, impl->out.data() + impl->outAt, k);   // (an empty buffer has no data(): UB even for 0)
        impl->outAt += k;
    }
    impl->cv.notify_all();                             // room again for the output thread
    return k;
}

bool HostProcess::finished() const {
    std::lock_guard<std::mutex> l(impl->m);
    return impl->outputEnded && impl->outAt >= impl->out.size();
}

std::string HostProcess::errorText() const {
    std::lock_guard<std::mutex> l(impl->m);
    return impl->err;
}

void HostProcess::stop() {
    Impl& p = *impl;
    { std::lock_guard<std::mutex> l(p.m); p.stopping = true; }
    p.cv.notify_all();
    // the program first: a writer held in a full pipe gets its error then
#ifdef _WIN32
    if (p.process) { TerminateProcess(p.process, 1); WaitForSingleObject(p.process, 5000); }
    if (p.writer.joinable()) p.writer.join();
    { std::lock_guard<std::mutex> l(p.m); p.closeIn(); }
    if (p.reader.joinable()) p.reader.join();
    if (p.errReader.joinable()) p.errReader.join();
    if (p.process) { CloseHandle(p.process); p.process = nullptr; }
    if (p.outR) { CloseHandle(p.outR); p.outR = nullptr; }
    if (p.errR) { CloseHandle(p.errR); p.errR = nullptr; }
#else
    if (p.pid > 0) { kill(p.pid, SIGTERM); }
    if (p.writer.joinable()) p.writer.join();
    { std::lock_guard<std::mutex> l(p.m); p.closeIn(); }
    if (p.reader.joinable()) p.reader.join();
    if (p.errReader.joinable()) p.errReader.join();
    if (p.pid > 0) { int st; waitpid(p.pid, &st, 0); p.pid = -1; }
    if (p.outR >= 0) { ::close(p.outR); p.outR = -1; }
    if (p.errR >= 0) { ::close(p.errR); p.errR = -1; }
#endif
}

} // namespace cpcse
