// CPCSyntaxError GUI — C programs with SDCC and CPCtelera. See c_build.h.
#include "c_build.h"
#include "cpct_assets.h"
#include "cpct_media.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cpcse {

// ---------------------------------------------------------------- running a tool

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
// One argument as the C runtime's command-line parser reads it back: quoted when it holds a
// space, a tab or a quote; backslashes doubled only before a quote.
static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { slashes++; continue; }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q.push_back(c);
    }
    q.append(slashes * 2, L'\\');
    q.push_back(L'"');
    return q;
}
#endif

// Runs a program; its standard output and error, together, into `output`. The exit code,
// or -1 when it could not be started.
int runProgram(const std::vector<std::string>& args, const std::string& cwd, std::string& output) {
#ifdef _WIN32
    std::wstring line;
    for (const std::string& a : args) { if (!line.empty()) line += L' '; line += quoteArg(widen(a)); }
    SECURITY_ATTRIBUTES sa { sizeof sa, nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmd(line.begin(), line.end());
    cmd.push_back(0);
    const std::wstring wcwd = widen(cwd);
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        cwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (!started) { CloseHandle(readEnd); return -1; }
    CloseHandle(pi.hThread);
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buf, sizeof buf, &got, nullptr) && got)
        for (DWORD i = 0; i < got; i++) if (buf[i] != '\r') output.push_back(buf[i]);
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    int fd[2];
    if (pipe(fd) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        dup2(fd[1], 1); dup2(fd[1], 2);
        close(fd[0]); close(fd[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fd[1]);
    char buf[4096];
    ssize_t got;
    while ((got = read(fd[0], buf, sizeof buf)) > 0) output.append(buf, (size_t)got);
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// SDCC's preprocessor starts its cc1 by the PATH (sdcpp: "cannot execute 'cc1'" otherwise):
// SDCC's bin goes first on this program's PATH, which its children inherit.
static void putFirstOnPath(const std::string& bin) {
#ifdef _WIN32
    std::wstring path;
    const DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (n) { path.resize(n); path.resize(GetEnvironmentVariableW(L"PATH", path.data(), n)); }
    const std::wstring wbin = widen(fs::path(bin).make_preferred().string());
    if (path.rfind(wbin + L";", 0) == 0) return;
    SetEnvironmentVariableW(L"PATH", (wbin + L";" + path).c_str());
#else
    const char* old = std::getenv("PATH");
    const std::string path = old ? old : "";
    if (path.rfind(bin + ":", 0) == 0) return;
    setenv("PATH", (bin + ":" + path).c_str(), 1);
#endif
}

// ---------------------------------------------------------------- finding the tools

#ifdef _WIN32
static const char* EXE = ".exe";
#else
static const char* EXE = "";
#endif

static bool hasSdcc(const fs::path& bin) {
    std::error_code ec;
    for (const char* tool : { "sdcc", "sdasz80", "sdar" })
        if (!fs::exists(bin / (std::string(tool) + EXE), ec)) return false;
    return true;
}

std::string findSdccBin(const std::string& folder) {
    std::vector<fs::path> look;
    if (!folder.empty()) { look.push_back(fs::u8path(folder)); look.push_back(fs::u8path(folder) / "bin"); }
#ifdef _WIN32
    look.push_back("C:/Program Files/SDCC/bin");
    look.push_back("C:/Program Files (x86)/SDCC/bin");
#else
    look.push_back("/usr/local/bin");
    look.push_back("/usr/bin");
    look.push_back("/opt/homebrew/bin");
#endif
    if (const char* p = std::getenv("PATH")) {
        std::string s = p;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t a = 0;
        while (a <= s.size()) {
            size_t b = s.find(sep, a);
            if (b == std::string::npos) b = s.size();
            if (b > a) look.push_back(fs::u8path(s.substr(a, b - a)));
            a = b + 1;
        }
    }
    for (const fs::path& p : look) if (hasSdcc(p)) return p.u8string();
    return "";
}

std::string findCpctelera(const std::string& folder) {
    if (folder.empty()) return "";
    std::error_code ec;
    // absolute: the library is assembled from inside the CPCtelera folder, where a relative
    // include path would point nowhere
    const fs::path f = fs::absolute(fs::u8path(folder), ec);
    if (fs::exists(f / "src" / "cpctelera.h", ec)) return f.u8string();
    if (fs::exists(f / "cpctelera" / "src" / "cpctelera.h", ec)) return (f / "cpctelera").u8string();
    return "";
}

std::string findZ88dk(const std::string& folder) {
    std::error_code ec;
    auto isRoot = [&](const fs::path& p) {
        return fs::exists(p / "bin" / (std::string("zcc") + EXE), ec) && fs::exists(p / "lib" / "config", ec);
    };
    std::vector<fs::path> look;
    if (!folder.empty()) {
        const fs::path f = fs::absolute(fs::u8path(folder), ec);
        look.push_back(f);
        look.push_back(f.parent_path());              // its bin folder given
        look.push_back(f / "z88dk");
    }
#ifdef _WIN32
    look.push_back("C:/z88dk");
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        const fs::path here = fs::path(std::wstring(exe, n)).parent_path();
        look.push_back(here / "z88dk");
        look.push_back(here.parent_path() / "z88dk");
    }
#else
    look.push_back("/opt/z88dk");
    look.push_back("/usr/local/share/z88dk");
#endif
    for (const fs::path& p : look) if (isRoot(p)) return p.u8string();
    // zcc on the PATH: its folder's parent
    if (const char* p = std::getenv("PATH")) {
        std::string s = p;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        size_t a = 0;
        while (a <= s.size()) {
            size_t b = s.find(sep, a);
            if (b == std::string::npos) b = s.size();
            if (b > a) { const fs::path bin = fs::u8path(s.substr(a, b - a)); if (isRoot(bin.parent_path())) return bin.parent_path().u8string(); }
            a = b + 1;
        }
    }
    return "";
}

static void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// ---------------------------------------------------------------- reading what the tools say

// "file:12: error 20: text", "file:5: warning 112: text", "file:21: Error: <i> text"; a
// Windows path keeps its drive's colon. The linker's "?ASlink-Error-..." has no line.
bool parseToolMessage(const std::string& raw, CBuildMessage& out) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) return false;
    if (line.rfind("?ASlink-", 0) == 0) {
        out = CBuildMessage{};
        out.text = line;
        out.error = line.find("Error") != std::string::npos;
        return true;
    }
    const size_t from = (line.size() > 2 && std::isalpha((unsigned char)line[0]) && line[1] == ':') ? 2 : 0;
    for (size_t c = line.find(':', from); c != std::string::npos; c = line.find(':', c + 1)) {
        size_t d = c + 1;
        while (d < line.size() && std::isdigit((unsigned char)line[d])) d++;
        if (d == c + 1 || d >= line.size() || line[d] != ':') continue;
        // the preprocessor adds a column: "main.c:21:10: fatal error: img/x.h: No such file"
        size_t e = d + 1;
        while (e < line.size() && std::isdigit((unsigned char)line[e])) e++;
        const size_t restFrom = (e > d + 1 && e < line.size() && line[e] == ':') ? e + 1 : d + 1;
        std::string rest = line.substr(restFrom);
        while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        std::string lower = rest;
        for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
        const bool isError = lower.rfind("error", 0) == 0 || lower.rfind("syntax error", 0) == 0 || lower.rfind("fatal", 0) == 0;
        const bool isWarning = lower.rfind("warning", 0) == 0;
        if (!isError && !isWarning) return false;
        out = CBuildMessage{};
        out.file = line.substr(0, c);
        out.line = std::atoi(line.substr(c + 1, d - c - 1).c_str());
        out.text = rest;
        out.error = isError;
        return true;
    }
    return false;
}

void parseRstLines(const std::string& text, std::vector<CSourceLine>& out) {
    std::istringstream in(text);
    std::string l, file;
    int line = 0;
    bool open = false;                   // the last run belongs to the current C line
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        bool hasAddress = l.size() >= 12 && l.compare(0, 4, "    ") == 0 && (l.size() == 12 || l[12] == ' ');
        for (size_t k = 4; hasAddress && k < 12; k++) hasAddress = std::isxdigit((unsigned char)l[k]) != 0;
        if (!hasAddress) {
            // ";F:\dir\main.c:43: cpct_drawSolidBox(...)": a new C line
            const size_t semi = l.find(';');
            if (semi != std::string::npos) {
                const std::string c = l.substr(semi + 1);
                for (size_t k = 2; k < c.size(); k++) {
                    if (c[k] != ':') continue;
                    size_t d = k + 1;
                    while (d < c.size() && std::isdigit((unsigned char)c[d])) d++;
                    if (d > k + 1 && d < c.size() && c[d] == ':') {
                        file = c.substr(0, k);
                        line = std::atoi(c.substr(k + 1, d - k - 1).c_str());
                        open = false;
                        break;
                    }
                }
            }
            // another area (data, initializers) ends the C line's code
            if (l.find(".area") != std::string::npos) { line = 0; open = false; }
            continue;
        }
        if (l.find(".area") != std::string::npos) { line = 0; open = false; continue; }
        if (line == 0) continue;
        // a table SDCC put after the function's code (const data in _CODE): not the line's code
        bool data = false;
        for (const char* d : { "\t.db", "\t.dw", "\t.ascii", "\t.asciz", "\t.ds", "\t.byte", "\t.word" }) data = data || l.find(d) != std::string::npos;
        if (data) { open = false; continue; }
        const int address = (int)(std::strtoul(l.substr(4, 8).c_str(), nullptr, 16) & 0xffff);
        int bytes = 0;
        std::istringstream field(l.size() > 13 ? l.substr(13, 17) : std::string());
        std::string w;
        while (field >> w) if (w.size() == 2 && std::isxdigit((unsigned char)w[0]) && std::isxdigit((unsigned char)w[1])) bytes++;
        if (bytes == 0) continue;            // a label
        if (open && !out.empty() && out.back().end == address) out.back().end = address + bytes;
        else { out.push_back({ address, address + bytes, file, line }); open = true; }
    }
}

// One line of a z80asm listing: "  335  0000  210000   \tld hl,..." -> its offset, its byte
// count and the source part; a line with no code gives offset -1.
static void lisLine(const std::string& l, int& offset, int& bytes, std::string& source) {
    offset = -1; bytes = 0;
    size_t i = 0;
    while (i < l.size() && l[i] == ' ') i++;
    size_t j = i;
    while (j < l.size() && std::isdigit((unsigned char)l[j])) j++;
    if (j == i) { source.clear(); return; }               // not a listing line (a bare label, bytes going on)
    size_t k = j;
    while (k < l.size() && l[k] == ' ') k++;
    size_t m = k;
    while (m < l.size() && std::isxdigit((unsigned char)l[m])) m++;
    if (m - k == 4 && m + 2 < l.size() && l[m] == ' ' && l[m + 1] == ' ' && std::isxdigit((unsigned char)l[m + 2])) {
        offset = (int)std::strtol(l.substr(k, 4).c_str(), nullptr, 16);
        size_t b = m + 2, e = b;
        while (e < l.size() && std::isxdigit((unsigned char)l[e])) e++;
        bytes = (int)(e - b) / 2;
        source = l.substr(e);
    } else {
        source = l.substr(k);
    }
}

void parseLisLines(const std::string& text, int base, std::vector<CSourceLine>& out) {
    std::istringstream in(text);
    std::string l, file, source;
    int line = 0;
    bool code = false, open = false;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        int offset, bytes;
        lisLine(l, offset, bytes, source);
        const size_t section = source.find("SECTION");
        if (section != std::string::npos) {
            std::istringstream w(source.substr(section + 7));
            std::string name;
            w >> name;
            code = name == "code_compiler";
            open = false;
            continue;
        }
        if (offset < 0) {
            size_t s = 0;
            while (s < source.size() && (source[s] == ' ' || source[s] == '\t')) s++;
            if (s < source.size() && source[s] == ';') {
                const std::string c = source.substr(s + 1);
                for (size_t k = 2; k < c.size(); k++) {
                    if (c[k] != ':') continue;
                    size_t d = k + 1;
                    while (d < c.size() && std::isdigit((unsigned char)c[d])) d++;
                    if (d > k + 1 && d < c.size() && c[d] == ':') {
                        file = c.substr(0, k);
                        line = std::atoi(c.substr(k + 1, d - k - 1).c_str());
                        open = false;
                        break;
                    }
                }
            }
            continue;
        }
        if (!code || line == 0 || bytes == 0) continue;
        const int address = (base + offset) & 0xffff;
        if (open && !out.empty() && out.back().end == address) out.back().end = address + bytes;
        else { out.push_back({ address, address + bytes, file, line }); open = true; }
    }
}

void parseListingSizes(const std::string& text, std::map<std::string, int>& out) {
    // an .rst: its code lines carry an 8-digit address in columns 4-11, the source from
    // column 40; else a z80asm .lis (lisLine)
    auto rstAddress = [](const std::string& l) {
        if (l.size() < 13 || l.compare(0, 4, "    ") != 0 || l[12] != ' ') return false;
        for (size_t k = 4; k < 12; k++) if (!std::isxdigit((unsigned char)l[k])) return false;
        return true;
    };
    bool rst = false;
    {
        std::istringstream probe(text);
        std::string l;
        for (int n = 0; n < 400 && !rst && std::getline(probe, l); n++) rst = rstAddress(l);
    }
    std::istringstream in(text);
    std::string l, label;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        std::string source;
        int bytes = 0;
        if (rst) {
            source = l.size() > 40 ? l.substr(40) : "";
            if (rstAddress(l)) {
                std::istringstream field(l.size() > 13 ? l.substr(13, 17) : std::string());
                std::string w;
                while (field >> w) if (w.size() == 2 && std::isxdigit((unsigned char)w[0]) && std::isxdigit((unsigned char)w[1])) bytes++;
            }
        } else {
            int offset;
            lisLine(l, offset, bytes, source);
            if (offset < 0) bytes = 0;
        }
        size_t s = 0;
        while (s < source.size() && (source[s] == ' ' || source[s] == '\t')) s++;
        const std::string src = source.substr(s);
        if (src.empty() || src[0] == ';') continue;
        std::string lower = src;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.find(".area") != std::string::npos || lower.rfind("section", 0) == 0) { label.clear(); continue; }
        // "name:" or "name::" (SDCC's own "00102$:" labels inside code are not things)
        size_t e = 0;
        while (e < src.size() && (std::isalnum((unsigned char)src[e]) || src[e] == '_' || src[e] == '.' || src[e] == '$')) e++;
        if (e > 0 && e < src.size() && src[e] == ':' && src[e - 1] != '$' && s == 0) {
            label = src.substr(0, e);
            out[label] = 0;
            continue;
        }
        if (label.empty()) continue;
        // ".ds N" (SDCC) or "defs N" (z80asm): N bytes kept
        for (const char* ds : { ".ds", "defs" }) {
            if (lower.rfind(ds, 0) == 0) {
                char* end = nullptr;
                const long n = std::strtol(src.c_str() + std::strlen(ds), &end, 0);
                if (end && n > 0 && n <= 0x10000) out[label] += (int)n;
                bytes = 0;
            }
        }
        out[label] += bytes;
    }
}

bool readIntelHex(const std::string& text, std::vector<uint8_t>& bytes, int& lowest) {
    auto hex = [](const std::string& s, size_t at, size_t n, int& v) {
        if (at + n > s.size()) return false;
        v = 0;
        for (size_t i = 0; i < n; i++) {
            const char c = s[at + i];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = v * 16 + d;
        }
        return true;
    };
    std::vector<int> image(0x10000, -1);
    int lo = 0x10000, hi = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != ':') continue;
        int n = 0, addr = 0, type = 0;
        if (!hex(line, 1, 2, n) || !hex(line, 3, 4, addr) || !hex(line, 7, 2, type)) return false;
        if (type == 1) break;
        if (type != 0) continue;
        for (int i = 0; i < n; i++) {
            int v = 0;
            if (!hex(line, 9 + (size_t)i * 2, 2, v)) return false;
            const int a = (addr + i) & 0xffff;
            image[(size_t)a] = v;
            lo = std::min(lo, a);
            hi = std::max(hi, a + 1);
        }
    }
    if (hi <= lo) return false;
    bytes.assign((size_t)(hi - lo), 0);
    for (int a = lo; a < hi; a++) if (image[(size_t)a] >= 0) bytes[(size_t)(a - lo)] = (uint8_t)image[(size_t)a];
    lowest = lo;
    return true;
}

// ---------------------------------------------------------------- the build

static std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

namespace {
struct Job { std::vector<std::string> args; std::string cwd, output; int code = 0; };

// The jobs on all the machine's threads; each job's output kept with it.
void runJobs(std::vector<Job>& jobs, const std::function<void(const std::string&)>& progress, const char* what) {
    std::atomic<size_t> next { 0 }, done { 0 };
    const unsigned n = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), 16));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; t++)
        pool.emplace_back([&]() {
            for (size_t i = next++; i < jobs.size(); i = next++) {
                jobs[i].code = runProgram(jobs[i].args, jobs[i].cwd, jobs[i].output);
                const size_t d = ++done;
                if (progress && (d % 25 == 0 || d == jobs.size()))
                    progress(std::string(what) + " " + std::to_string(d) + " / " + std::to_string(jobs.size()));
            }
        });
    for (std::thread& t : pool) t.join();
}

std::string commandLine(const std::vector<std::string>& args) {
    std::string s;
    for (const std::string& a : args) s += (s.empty() ? "" : " ") + (a.find(' ') == std::string::npos ? a : "\"" + a + "\"");
    return s;
}

uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// CPCtelera's cfg/build_config.mk: "NAME := value" lines (values that use make's $(...) are
// left to the defaults, except the flags: buildC fills in the variables they name).
struct ProjectConfig { std::string name, srcDir, objDir, flags; int codeLoc = -1; };
ProjectConfig readBuildConfig(const fs::path& file) {
    ProjectConfig cfg;
    std::istringstream in(readText(file));
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        size_t eq = line.find(":=");
        size_t skip = 2;
        if (eq == std::string::npos) { eq = line.find('='); skip = 1; }
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
            size_t a = 0;
            while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
            return s.substr(a);
        };
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + skip));
        if (key == "Z80CCFLAGS") { cfg.flags = value; continue; }   // its $(...) are filled in by buildC
        if (value.find('$') != std::string::npos) continue;
        if (key == "PROJNAME") cfg.name = value;
        else if (key == "SRCDIR") cfg.srcDir = value;
        else if (key == "OBJDIR") cfg.objDir = value;
        else if (key == "Z80CODELOC") cfg.codeLoc = (int)std::strtol(value.c_str(), nullptr, 0);
    }
    return cfg;
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}
} // namespace

// z88dk: one zcc command builds the whole program (CToolchain's comment); the results read
// back as from SDCC: the binary, the map's symbols, the listings' lines; then the program
// as a disc, a tape and a snapshot (cpct_media.h) beside the objects.
static CBuildResult buildZ88dk(const CToolchain& tools, const std::string& mainPath, const std::function<void(const std::string&)>& progress) {
    CBuildResult r;
    std::error_code ec;
    auto fail = [&](const std::string& text) { CBuildMessage m; m.text = text; r.messages.push_back(m); r.ok = false; return r; };
    auto note = [&](const std::string& s) { r.log += s + "\n"; if (progress) progress(s); };

    const std::string root = findZ88dk(tools.z88dkFolder);
    if (root.empty()) return fail("z88dk was not found. Unpack z88dk (z88dk.org) and give its folder in C settings.");
    const fs::path zroot = fs::u8path(root);
    setEnv("ZCCCFG", (zroot / "lib" / "config").make_preferred().u8string());
    putFirstOnPath((zroot / "bin").u8string());
    const std::string zcc = (zroot / "bin" / (std::string("zcc") + EXE)).u8string();

    const fs::path main = fs::absolute(fs::u8path(mainPath), ec);
    if (mainPath.empty() || !fs::exists(main, ec)) return fail("Save the C file first: z88dk compiles files on disc.");
    const fs::path srcDir = main.parent_path();
    const fs::path projRoot = srcDir.filename() == "src" ? srcDir.parent_path() : srcDir;
    const fs::path objDir = projRoot / "obj";
    fs::create_directories(objDir, ec);
    r.project = main.stem().u8string();

    std::vector<fs::path> sources;
    for (auto it = fs::recursive_directory_iterator(srcDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec) && fs::equivalent(it->path(), objDir, ec)) { it.disable_recursion_pending(); continue; }
        const std::string ext = it->path().extension().u8string();
        if (it->is_regular_file(ec) && (ext == ".c" || ext == ".asm")) sources.push_back(it->path());
    }
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) return fail("No .c or .asm files in " + srcDir.u8string());

    const bool zsdcc = !tools.z88dkSccz80;
    const fs::path binFile = objDir / (r.project + ".bin");
    std::vector<std::string> args = { zcc, "+cpc" };
    if (zsdcc) args.push_back("-compiler=sdcc");
    for (const char* a : { "-lndos", "-m", "--list", "-create-app" }) args.push_back(a);
    if (zsdcc) args.push_back("--c-code-in-asm");
    args.push_back("-zorg=" + std::to_string(tools.codeLoc & 0xffff));
    for (const std::string& f : splitWords(tools.extraFlags)) args.push_back(f);
    args.push_back("-o");
    args.push_back(binFile.u8string());
    for (const fs::path& s : sources) args.push_back(s.generic_u8string());
    std::string version;
    runProgram({ zcc }, "", version);
    note("z88dk: " + version.substr(0, version.find('\n')) + (zsdcc ? " (zsdcc)" : " (sccz80)"));
    note(commandLine(args));
    fs::remove(binFile, ec);                      // this build's own output, so a stale one is not taken
    std::string out;
    const int code = runProgram(args, objDir.u8string(), out);
    r.log += out;
    {
        std::istringstream in(out);
        std::string line;
        while (std::getline(in, line)) { CBuildMessage m; if (parseToolMessage(line, m)) r.messages.push_back(m); }
    }
    // the listings zcc wrote beside the sources go to the objects
    std::vector<std::pair<fs::path, std::string>> listings;
    for (const fs::path& s : sources) {
        const fs::path lis = fs::path(s.native() + fs::path(".lis").native());
        if (!fs::exists(lis, ec)) continue;
        if (s.extension() == ".c") listings.emplace_back(s, readText(lis));
        fs::rename(lis, objDir / lis.filename(), ec);
    }
    if (code != 0 || !fs::exists(binFile, ec)) {
        bool error = false;
        for (const CBuildMessage& m : r.messages) error = error || m.error;
        if (!error) { CBuildMessage m; m.text = "zcc failed (exit " + std::to_string(code) + "); the Build log has its output."; r.messages.push_back(m); }
        return r;
    }
    {
        std::ifstream f(binFile, std::ios::binary);
        r.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    r.binPath = binFile.u8string();

    // the map: "name   = $1480 ; addr, public, , module, section, file:line"
    std::map<std::string, int> addressOf;
    {
        std::istringstream map(readText(fs::path(binFile).replace_extension(".map")));
        std::string line;
        while (std::getline(map, line)) {
            const size_t eq = line.find("= $");
            if (eq == std::string::npos) continue;
            std::string name = line.substr(0, eq);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            const int value = (int)(std::strtoul(line.c_str() + eq + 3, nullptr, 16) & 0xffff);
            if (name.rfind("__C_LINE", 0) == 0 || name.rfind("__CDBINFO", 0) == 0) continue;
            addressOf[name] = value;
            if (line.find("; addr,") != std::string::npos) r.symbols.emplace_back(name, value);
        }
    }
    r.loadAddress = addressOf.count("__head") ? addressOf["__head"] : addressOf.count("CRT_ORG_CODE") ? addressOf["CRT_ORG_CODE"] : tools.codeLoc;
    r.runAddress = r.loadAddress;                 // the crt0, which calls main
    // the lines (zsdcc): each listing's code_compiler offsets put at the address the map
    // gives its first function
    for (const auto& [source, text] : listings) {
        std::istringstream in(text);
        std::string l, src;
        bool code = false;
        std::string label;
        int base = -1;
        while (base < 0 && std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            int offset, bytes;
            lisLine(l, offset, bytes, src);
            if (src.find("SECTION") != std::string::npos) { code = src.find("code_compiler") != std::string::npos; label.clear(); continue; }
            if (!code) continue;
            if (offset < 0) {
                size_t s = 0;
                while (s < src.size() && (src[s] == ' ' || src[s] == '\t')) s++;
                size_t e = s;
                while (e < src.size() && (std::isalnum((unsigned char)src[e]) || src[e] == '_')) e++;
                if (e > s && e < src.size() && src[e] == ':' && addressOf.count(src.substr(s, e - s))) label = src.substr(s, e - s);
            } else if (!label.empty()) {
                base = addressOf[label] - offset;
            }
        }
        if (base >= 0) parseLisLines(text, base, r.lines);
        parseListingSizes(text, r.symbolSizes);
    }
    std::sort(r.lines.begin(), r.lines.end(), [](const CSourceLine& a, const CSourceLine& b) { return a.start < b.start; });
    if (!zsdcc) note("sccz80 builds have no C line table (its line markers come a line late): choose zsdcc to step by line");

    // the program on a disc, a tape and a snapshot, as the CPCtelera build makes them
    std::shared_ptr<Disk> disc = cpctNewDisc();
    std::string why = cpctDiscImport(*disc, r.project + ".bin", r.bytes, 1, r.loadAddress, r.runAddress);
    if (why.empty()) {
        const std::vector<uint8_t> image = cpctDiscImage(*disc);
        r.dskPath = (objDir / (r.project + ".dsk")).u8string();
        std::ofstream(fs::u8path(r.dskPath), std::ios::binary).write((const char*)image.data(), (std::streamsize)image.size());
    }
    std::vector<uint8_t> cdt;
    std::string tapeName = r.project.substr(0, 16);
    for (char& ch : tapeName) ch = (char)std::toupper((unsigned char)ch);
    cpctTapeAddFile(cdt, r.bytes, tapeName, "cpc", r.loadAddress, r.runAddress, 3000);
    r.cdtPath = (objDir / (r.project + ".cdt")).u8string();
    std::ofstream(fs::u8path(r.cdtPath), std::ios::binary).write((const char*)cdt.data(), (std::streamsize)cdt.size());
    std::vector<uint8_t> sna;
    std::string snaWhy;
    if (tools.makeSnapshot && tools.makeSnapshot(r.bytes, r.loadAddress, r.runAddress, sna, snaWhy)) {
        r.snaPath = (objDir / (r.project + ".sna")).u8string();
        std::ofstream(fs::u8path(r.snaPath), std::ios::binary).write((const char*)sna.data(), (std::streamsize)sna.size());
    }
    char text[96];
    std::snprintf(text, sizeof text, "%d bytes at &%04X, run &%04X (z88dk's crt0)", (int)r.bytes.size(), r.loadAddress & 0xffff, r.runAddress & 0xffff);
    note(text);
    r.ok = true;
    return r;
}

CBuildResult buildC(const CToolchain& tools, const std::string& mainPath, const std::string& cacheFolder,
                    const std::function<void(const std::string&)>& progress) {
    if (tools.compiler == "z88dk") return buildZ88dk(tools, mainPath, progress);
    CBuildResult r;
    std::error_code ec;
    auto fail = [&](const std::string& text) { CBuildMessage m; m.text = text; r.messages.push_back(m); r.ok = false; return r; };
    auto note = [&](const std::string& s) { r.log += s + "\n"; if (progress) progress(s); };
    auto takeOutput = [&](const std::string& output) {
        std::istringstream in(output);
        std::string line;
        while (std::getline(in, line)) {
            CBuildMessage m;
            if (parseToolMessage(line, m)) r.messages.push_back(m);
        }
    };

    const std::string bin = findSdccBin(tools.sdccFolder);
    if (bin.empty())
        return fail("SDCC was not found. Install SDCC (sdcc.sourceforge.net) and give its folder in C settings.");
    putFirstOnPath(bin);
    const std::string sdcc = (fs::u8path(bin) / (std::string("sdcc") + EXE)).u8string();
    const std::string sdasz80 = (fs::u8path(bin) / (std::string("sdasz80") + EXE)).u8string();
    const std::string sdar = (fs::u8path(bin) / (std::string("sdar") + EXE)).u8string();
    std::string version;
    runProgram({ sdcc, "--version" }, "", version);
    version = version.substr(0, version.find('\n'));
    note("SDCC: " + version);
    // SDCC 4.2 made __sdcccall(1) (arguments in registers) the default and added
    // --sdcccall; before it every call passed its arguments on the stack.
    int major = 0, minor = 0;
    for (size_t i = 0; i + 2 < version.size(); i++)
        if (std::isdigit((unsigned char)version[i]) && version[i + 1] == '.' && std::isdigit((unsigned char)version[i + 2]) &&
            (i == 0 || version[i - 1] == ' ')) { major = version[i] - '0'; minor = std::atoi(version.c_str() + i + 2); break; }
    const bool registerCalls = major > 4 || (major == 4 && minor >= 2);

    const fs::path main = fs::absolute(fs::u8path(mainPath), ec);
    if (mainPath.empty() || !fs::exists(main, ec)) return fail("Save the C file first: SDCC compiles files on disc.");

    // ---- the project: a CPCtelera project if cfg/build_config.mk is above the file
    fs::path root = main.parent_path();
    ProjectConfig cfg;
    bool cpctProject = false;
    // the file's folder (a project's root) or the one above it (the file in src/)
    for (const fs::path& p : { main.parent_path(), main.parent_path().parent_path() }) {
        if (!p.empty() && fs::exists(p / "cfg" / "build_config.mk", ec)) {
            root = p;
            cfg = readBuildConfig(p / "cfg" / "build_config.mk");
            cpctProject = true;
            break;
        }
    }
    if (!cpctProject && main.parent_path().filename() == "src") root = main.parent_path().parent_path();
    const fs::path srcDir = cpctProject && !cfg.srcDir.empty() ? root / cfg.srcDir : main.parent_path();
    const fs::path objDir = root / (cfg.objDir.empty() ? "obj" : cfg.objDir);
    r.project = !cfg.name.empty() ? cfg.name : main.stem().u8string();
    const int codeLoc = cfg.codeLoc >= 0 ? cfg.codeLoc : tools.codeLoc;
    std::string flags = cfg.flags;
    if (!tools.extraFlags.empty()) flags += (flags.empty() ? "" : " ") + tools.extraFlags;
    char locText[16];
    std::snprintf(locText, sizeof locText, "0x%04X", codeLoc & 0xffff);
    note(std::string(cpctProject ? "CPCtelera project " : "Project ") + r.project + ", code at " + locText);

    // ---- CPCtelera's library, built once into the cache
    const std::string cpct = findCpctelera(tools.cpcteleraFolder);
    std::string library;
    // Which calling convention the library's assembly expects. CPCtelera_next is written for
    // SDCC 4.6's default __sdcccall(1) (arguments in registers; its sources say so in 137
    // files). CPCtelera 1.4.2 and older were made for SDCC 3.6.8, which passes arguments on
    // the stack: their C must be compiled with --sdcccall 0, or every library call reads
    // the wrong registers and the program crashes. A library that never names sdcccall is
    // the old kind. Its headers' old-style "f()" declarations (warning 283) are not the
    // program's: that warning is left out for it.
    bool oldConvention = false;
    if (!tools.cpcteleraFolder.empty() && cpct.empty())
        return fail("No CPCtelera in " + tools.cpcteleraFolder + " (no src/cpctelera.h, or cpctelera/src/cpctelera.h).");
    if (!cpct.empty()) {
        const fs::path csrc = fs::u8path(cpct) / "src";
        std::vector<fs::path> sources;
        long long newest = 0;
        for (auto it = fs::recursive_directory_iterator(csrc, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".s") continue;
            sources.push_back(it->path());
            newest = std::max<long long>(newest, (long long)fs::last_write_time(it->path(), ec).time_since_epoch().count());
        }
        std::sort(sources.begin(), sources.end());
        oldConvention = true;
        for (const fs::path& s : sources)
            if (readText(s).find("sdcccall") != std::string::npos) { oldConvention = false; break; }
        if (!oldConvention && !registerCalls)
            return fail("This CPCtelera is written for __sdcccall(1) (CPCtelera_next: SDCC 4.6.0). " + version.substr(0, 40) +
                        " passes arguments on the stack: name SDCC 4.2 or later in C settings.");
        note(oldConvention ? (registerCalls ? "CPCtelera with the old calling convention (made for SDCC 3.6.8): C compiled with --sdcccall 0"
                                            : "CPCtelera with the old calling convention, as this SDCC's own")
                           : "CPCtelera with __sdcccall(1) (made for SDCC 4.6.0)");
        char key[24];
        std::snprintf(key, sizeof key, "%016llx", (unsigned long long)fnv(version + "|" + cpct + "|" + std::to_string(sources.size()) + "|" + std::to_string(newest)));
        const fs::path libDir = fs::u8path(cacheFolder) / (std::string("cpctelera-") + key);
        const fs::path lib = libDir / "cpctelera.lib";
        if (!fs::exists(lib, ec)) {
            note("Building the CPCtelera library (" + std::to_string(sources.size()) + " files, once)");
            // objects in a folder of this build's own: two builds at once (two windows, a
            // batch) never write the same object file; the finished library is renamed in
            static std::atomic<unsigned> serial { 0 };
            char own[48];
            std::snprintf(own, sizeof own, "obj-%llx-%u", (unsigned long long)std::hash<std::thread::id>()(std::this_thread::get_id()) ^
                          (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count(), serial++);
            const fs::path libObj = libDir / own;
            std::vector<Job> jobs;
            for (const fs::path& s : sources) {
                const fs::path rel = libObj / fs::relative(s, csrc, ec).replace_extension(".rel");
                fs::create_directories(rel.parent_path(), ec);
                Job j;
                // from the CPCtelera folder: its sources include "src/macros/..."
                j.args = { sdasz80, "-I" + csrc.u8string() + "/", "-l", "-o", "-s", rel.u8string(), s.u8string() };
                j.cwd = cpct;
                jobs.push_back(j);
            }
            runJobs(jobs, progress, "library");
            std::vector<std::string> rels;
            for (size_t i = 0; i < jobs.size(); i++) {
                if (jobs[i].code != 0) {
                    r.log += commandLine(jobs[i].args) + "\n" + jobs[i].output;
                    takeOutput(jobs[i].output);
                    return fail("The CPCtelera library did not build: " + sources[i].filename().u8string());
                }
                rels.push_back(fs::relative(jobs[i].args[5], libObj.u8string(), ec).u8string());
            }
            const fs::path partial = libObj / "cpctelera.lib";
            for (size_t i = 0; i < rels.size(); i += 100) {
                std::vector<std::string> args = { sdar, "rc", partial.u8string() };
                for (size_t k = i; k < std::min(rels.size(), i + 100); k++) args.push_back(rels[k]);
                std::string out;
                if (runProgram(args, libObj.u8string(), out) != 0) { r.log += out; return fail("sdar could not make the CPCtelera library."); }
            }
            fs::rename(partial, lib, ec);
            if (ec && !fs::exists(lib)) return fail("Could not write " + lib.u8string());   // else another build finished it first
            ec.clear();
            fs::remove_all(libObj, ec);                 // this build's own objects, and only them
        }
        library = lib.u8string();
    }

    // ---- a CPCtelera project's asset steps (its cfg/*.mk): images, maps, music, binaries
    if (cpctProject) {
        const AssetResult a = convertProjectAssets(root.u8string(), srcDir.u8string(), cpct, tools.forceAssets || tools.assetsOnly);
        r.log += a.log;
        r.messages.insert(r.messages.end(), a.messages.begin(), a.messages.end());
        note("Assets: " + std::to_string(a.converted) + " converted, " + std::to_string(a.upToDate) + " up to date");
        if (!a.ok) { r.ok = false; return r; }
        if (tools.assetsOnly) { r.ok = true; return r; }
    } else if (tools.assetsOnly) return fail("No cfg/build_config.mk beside the project: there are no CPCtelera asset steps to run.");

    // ---- the project's sources
    std::vector<fs::path> cfiles, sfiles;
    for (auto it = fs::recursive_directory_iterator(srcDir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec) && fs::equivalent(it->path(), objDir, ec)) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().u8string();
        if (ext == ".c") cfiles.push_back(it->path());
        else if (ext == ".s") sfiles.push_back(it->path());
    }
    std::sort(cfiles.begin(), cfiles.end());
    std::sort(sfiles.begin(), sfiles.end());
    if (cfiles.empty() && sfiles.empty()) return fail("No .c or .s files in " + srcDir.u8string());

    std::vector<Job> jobs;
    std::vector<std::string> objects;
    auto objectFor = [&](const fs::path& source) {
        fs::path rel = fs::relative(source, srcDir, ec);
        fs::path o = objDir / rel.replace_extension(".rel");
        fs::create_directories(o.parent_path(), ec);
        return o.u8string();
    };
    // The flags with CPCtelera's variables filled in ("-Wa -I$(CPCT_SRC)": the assembler of
    // the C files gets the library's include folder). A word naming any other variable is
    // left out, with a -Wa / -Wl / -Wp before it that would pass it on.
    std::vector<std::string> flagWords;
    for (std::string w : splitWords(flags)) {
        auto put = [&](const char* var, const std::string& value) {
            for (size_t at = w.find(var); at != std::string::npos; at = w.find(var)) w.replace(at, std::strlen(var), value);
        };
        for (const char* open : { "$(", "${" }) {
            const char close = open[1] == '(' ? ')' : '}';
            auto var = [&](const char* name) { return std::string(open) + name + close; };
            if (!cpct.empty()) {
                put(var("CPCT_SRC").c_str(), (fs::u8path(cpct) / "src").u8string());
                put(var("CPCT_PATH").c_str(), cpct + "/");
            }
            put(var("SRCDIR").c_str(), srcDir.u8string());
            put(var("OBJDIR").c_str(), objDir.u8string());
        }
        if (w.find("$(") != std::string::npos || w.find("${") != std::string::npos) {
            if (!flagWords.empty() && (flagWords.back() == "-Wa" || flagWords.back() == "-Wl" || flagWords.back() == "-Wp")) flagWords.pop_back();
            continue;
        }
        // A peephole file under an SDCC folder the CPCtelera does not have (easyoverscan names
        // tools/sdcc-4.5.0 in a CPCtelera that ships tools/sdcc-4.6.0): the file of that name
        // from the SDCC folder it has.
        if (!flagWords.empty() && flagWords.back() == "--peep-file" && !fs::exists(fs::u8path(w), ec) && !cpct.empty()) {
            const std::string name = fs::u8path(w).filename().u8string();
            std::string found;
            for (auto it = fs::directory_iterator(fs::u8path(cpct) / "tools", ec); it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) break;
                const fs::path candidate = it->path() / "peep" / name;
                if (it->path().filename().u8string().rfind("sdcc", 0) == 0 && fs::exists(candidate, ec)) { found = candidate.u8string(); break; }
            }
            note("--peep-file " + w + " is not there; " + (found.empty() ? std::string("left out") : "used " + found));
            if (found.empty()) { flagWords.pop_back(); continue; }
            w = found;
        }
        flagWords.push_back(w);
    }
    for (const fs::path& c : cfiles) {
        Job j;
        j.args = { sdcc };
        if (!cpct.empty()) j.args.push_back("-I" + (fs::u8path(cpct) / "src").u8string());
        j.args.push_back("-I" + srcDir.u8string());
        j.args.push_back("-mz80");
        if (oldConvention && registerCalls)   // the old library's headers: 283 "f()" declarations, 291 enum range
            for (const char* a : { "--sdcccall", "0", "--disable-warning", "283", "--disable-warning", "291", "--disable-warning", "296" })
                j.args.push_back(a);
        for (const std::string& w : flagWords) j.args.push_back(w);
        const std::string o = objectFor(c);
        for (const std::string& a : { std::string("-c"), c.u8string(), std::string("-o"), o }) j.args.push_back(a);
        j.cwd = root.u8string();
        jobs.push_back(j);
        objects.push_back(o);
    }
    for (const fs::path& s : sfiles) {
        Job j;
        const std::string o = objectFor(s);
        j.args = { sdasz80, "-l", "-o", "-s" };
        if (!cpct.empty()) j.args.push_back("-I" + (fs::u8path(cpct) / "src").u8string());
        j.args.push_back("-I" + srcDir.u8string());
        j.args.push_back(o);
        j.args.push_back(s.u8string());
        j.cwd = root.u8string();
        jobs.push_back(j);
        objects.push_back(o);
    }
    note("Compiling " + std::to_string(cfiles.size()) + " C and " + std::to_string(sfiles.size()) + " assembly file(s)");
    runJobs(jobs, nullptr, "compile");
    bool compiled = true;
    for (const Job& j : jobs) {
        r.log += commandLine(j.args) + "\n" + j.output;
        takeOutput(j.output);
        if (j.code != 0) compiled = false;
    }
    if (!compiled) { r.ok = false; return r; }

    // ---- link, as CPCtelera's project template does
    const fs::path ihx = objDir / (r.project + ".ihx");
    fs::remove(ihx, ec);
    std::vector<std::string> link = { sdcc, "-mz80", "--no-std-crt0", "-Wl-u", "--code-loc", locText, "--data-loc", "0" };
    if (!library.empty()) link.push_back("-l" + library);
    for (const std::string& o : objects) link.push_back(o);
    link.push_back("-o");
    link.push_back(ihx.u8string());
    std::string out;
    const int code = runProgram(link, root.u8string(), out);
    r.log += commandLine(link) + "\n" + out;
    takeOutput(out);
    // The old convention and SDCC's own library: the linker names each module built for
    // the other convention ('"-mz80 sdcccall(1)" in module "modunsigned"'). SDCC 4.2 and later
    // build that library (and its hand-written z80 helpers: .optsdcc sdcccall(1)) for
    // registers, so such a program would read the wrong registers in multiply, divide,
    // strlen ... and crash. It is refused with the reason, not run.
    if (oldConvention && registerCalls) {
        std::string modules;
        std::istringstream lines(out);
        std::string l;
        while (std::getline(lines, l)) {
            const size_t at = l.find("sdcccall(1)\" in module \"");
            if (at == std::string::npos) continue;
            const size_t from = at + std::strlen("sdcccall(1)\" in module \"");
            const size_t to = l.find('"', from);
            if (to != std::string::npos) modules += (modules.empty() ? "" : ", ") + l.substr(from, to - from);
        }
        if (!modules.empty()) {
            fs::remove(ihx, ec);
            return fail("This CPCtelera (1.4.x and older) is made for SDCC 3.6.8, which passes arguments on the stack. "
                        "The program also uses SDCC's own library (" + modules + "), which SDCC 4.2 and later build for "
                        "arguments in registers: it would crash. Use CPCtelera_next (made for SDCC 4.6.0), or name an "
                        "SDCC 4.1 or older in C settings.");
        }
    }
    if (code != 0 || !fs::exists(ihx, ec)) { CBuildMessage m; m.text = "The link failed."; r.messages.push_back(m); return r; }

    int lowest = 0;
    if (!readIntelHex(readText(ihx), r.bytes, lowest)) return fail("The linker's Intel HEX file could not be read.");
    r.loadAddress = lowest;
    // the link map: "     00004000  _main      main"
    std::istringstream map(readText(fs::path(ihx).replace_extension(".map")));
    std::string line;
    while (std::getline(map, line)) {
        size_t a = 0;
        while (a < line.size() && line[a] == ' ') a++;
        size_t b = a;
        while (b < line.size() && std::isxdigit((unsigned char)line[b])) b++;
        if (b - a != 8) continue;
        size_t c = b;
        while (c < line.size() && line[c] == ' ') c++;
        size_t d = c;
        while (d < line.size() && line[d] != ' ' && line[d] != '\t') d++;
        if (d == c) continue;
        const std::string name = line.substr(c, d - c);
        if (!(std::isalpha((unsigned char)name[0]) || name[0] == '_' || name[0] == '.')) continue;
        const int value = (int)(std::strtoul(line.substr(a, 8).c_str(), nullptr, 16) & 0xffff);
        r.symbols.emplace_back(name, value);
        if (name == "_main") r.runAddress = value;
    }
    if (r.runAddress < 0) r.runAddress = r.loadAddress;
    // the C lines' addresses, from each object's listing as the linker finished it (-Wl-u)
    for (const std::string& o : objects) {
        const std::string rst = readText(fs::u8path(o).replace_extension(".rst"));
        parseRstLines(rst, r.lines);
        parseListingSizes(rst, r.symbolSizes);
    }
    std::sort(r.lines.begin(), r.lines.end(), [](const CSourceLine& a, const CSourceLine& b) { return a.start < b.start; });
    const fs::path binFile = objDir / (r.project + ".bin");
    std::ofstream f(binFile, std::ios::binary);
    f.write((const char*)r.bytes.data(), (std::streamsize)r.bytes.size());
    f.close();
    r.binPath = binFile.u8string();

    // ---- the disc, tape and snapshot images the project's TARGET asks for
    if (cpctProject) {
        MediaResult m = makeProjectMedia(root.u8string(), cpct, r.project, r.bytes, r.loadAddress, r.runAddress);
        r.log += m.log;
        r.messages.insert(r.messages.end(), m.messages.begin(), m.messages.end());
        if (!m.ok) return r;
        r.dskPath = m.dskPath;
        r.cdtPath = m.cdtPath;
        r.discNeeded = m.discNeeded;
        if (m.wantSna) {
            std::vector<uint8_t> sna;
            std::string why = "no machine to make it";
            if (tools.makeSnapshot && tools.makeSnapshot(r.bytes, r.loadAddress, r.runAddress, sna, why)) {
                std::ofstream s(fs::u8path(m.snaPath), std::ios::binary);
                s.write((const char*)sna.data(), (std::streamsize)sna.size());
                if (s) { r.snaPath = m.snaPath; r.log += "snapshot: " + fs::u8path(m.snaPath).filename().u8string() + "\n"; }
                else { CBuildMessage w; w.text = "Could not write " + m.snaPath; r.messages.push_back(w); return r; }
            } else {
                CBuildMessage w; w.text = "The snapshot (" + fs::u8path(m.snaPath).filename().u8string() + ") was not made: " + why; w.error = false;
                r.messages.push_back(w);
            }
        }
        std::string made;
        for (const std::string* p : { &r.dskPath, &r.cdtPath, &r.snaPath }) if (!p->empty()) made += (made.empty() ? "" : ", ") + fs::u8path(*p).filename().u8string();
        if (!made.empty()) note("Images: " + made);
    }
    r.ok = true;
    return r;
}

} // namespace cpcse
