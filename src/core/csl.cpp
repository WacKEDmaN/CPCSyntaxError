// CPCSyntaxError — CSL script player with SSM screenshots. See csl.h.
#include "csl.h"
#include "emulator.h"
#include "video.h"
#include "z80.h"
#include "crtc.h"
#include "memory.h"
#include "gate_array.h"
#include "gate_array_model.h"
#include "keyboard.h"
#include "fdc.h"
#include "ay.h"
#include "tape.h"
#include "ppi.h"
#include "sna.h"
#include "cpr_loader.h"
#include "v9990.h"
#include "monitor_model.h"
#include "monitor_renderer.h"
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace cpcse {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Bytes readBytes(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end); auto n = f.tellg(); f.seekg(0);
    Bytes b((size_t)std::max<std::streamoff>(0, n));
    if (n > 0) f.read((char*)b.data(), n);
    ok = true;
    return b;
}

bool isFile(const std::string& path) { std::error_code ec; return !path.empty() && fs::is_regular_file(path, ec); }
bool isDir(const std::string& path) { std::error_code ec; return !path.empty() && fs::is_directory(path, ec); }

// A directory instruction is a PREFIX: "it will be concatenated before the name". A
// folder written without its closing slash still means that folder.
// Scripts are written on Windows: names in either case and with '\' separators. Where the
// path as written does not exist (a case-sensitive file system), each part is looked up in
// its folder ignoring case, as a Windows host would have found it.
static std::string resolveHostCase(std::string path) {
#ifndef _WIN32
    for (char& c : path) if (c == '\\') c = '/';
    std::error_code ec;
    if (path.empty() || fs::exists(path, ec)) return path;
    auto lowerOf = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    fs::path p(path), out = p.has_root_path() ? p.root_path() : fs::path(".");
    for (const fs::path& part : p.relative_path()) {
        fs::path next = out / part;
        if (!fs::exists(next, ec) && fs::is_directory(out, ec)) {
            const std::string want = lowerOf(part.string());
            for (auto it = fs::directory_iterator(out, ec); it != fs::directory_iterator(); it.increment(ec))
                if (lowerOf(it->path().filename().string()) == want) { next = it->path(); break; }
        }
        out = next;
    }
    return p.has_root_path() ? out.string() : out.lexically_normal().string();
#else
    return path;
#endif
}

std::string joinPrefix(const std::string& prefix, const std::string& name) {
    if (prefix.empty()) return resolveHostCase(name);
    char last = prefix.back();
    if (last == '/' || last == '\\' || !isDir(prefix)) return resolveHostCase(prefix + name);
    return resolveHostCase(prefix + "/" + name);
}

// The typographic quotes a word processor puts in a script (the standard's own examples
// carry them) become the plain ones.
std::string plainQuotes(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80) {
            unsigned char d = (unsigned char)s[i + 2];
            if (d == 0x98 || d == 0x99) { o += '\''; i += 2; continue; }
            if (d == 0x9C || d == 0x9D) { o += '"'; i += 2; continue; }
        }
        if (c == 0x91 || c == 0x92) { o += '\''; continue; }   // Windows-1252
        if (c == 0x93 || c == 0x94) { o += '"'; continue; }
        o += (char)c;
    }
    return o;
}

// One CSL line: the instruction and its arguments, with the comment removed. A quoted
// argument is one argument whatever it holds; inside it a \(...) escape is opaque, so
// \(') does not end the string.
void splitLine(const std::string& lineIn, std::string& instruction, std::vector<std::string>& args, std::string& text) {
    const std::string line = plainQuotes(lineIn);
    std::vector<std::string> tokens;
    size_t i = 0, end = line.size();
    size_t textEnd = 0;
    while (i < end) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') { i++; continue; }
        if (c == ';') break;
        if (c == '\'' || c == '"') {
            std::string tok;
            size_t j = i + 1;
            while (j < end && line[j] != c) {
                if (line[j] == '\\' && j + 1 < end && line[j + 1] == '(') {
                    size_t close = line.find(')', j + 3);
                    if (close == std::string::npos) close = end - 1;
                    tok += line.substr(j, close - j + 1);
                    j = close + 1;
                    continue;
                }
                tok += line[j++];
            }
            tokens.push_back(tok);
            i = j < end ? j + 1 : end;
            textEnd = i;
            continue;
        }
        size_t j = i;
        while (j < end && line[j] != ' ' && line[j] != '\t' && line[j] != '\r' && line[j] != '\n'
               && line[j] != ',' && line[j] != ';') j++;
        tokens.push_back(line.substr(i, j - i));
        i = j;
        textEnd = j;
    }
    text = trim(line.substr(0, textEnd));
    instruction = tokens.empty() ? "" : lower(tokens[0]);
    args.assign(tokens.empty() ? tokens.end() : tokens.begin() + 1, tokens.end());
}

bool parseNumber(const std::string& s, double& v) {
    std::string t = trim(s);
    if (t.empty()) return false;
    char* e = nullptr;
    if (t[0] == '&' || t[0] == '#') v = (double)std::strtol(t.c_str() + 1, &e, 16);
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) v = (double)std::strtol(t.c_str() + 2, &e, 16);
    else v = std::strtod(t.c_str(), &e);
    // "1e999", "inf" and "nan" are numbers to strtod; as a time or a model they would be
    // converted to whole numbers that cannot hold them (undefined). 1e15 usec is 31 years.
    return e && *e == 0 && std::isfinite(v) && std::fabs(v) <= 1e15;
}

// The CSL annex's special keys, on the CPC's matrix (row, bit).
bool specialKey(const std::string& code, std::pair<int, int>& cell) {
    static const std::map<std::string, std::pair<int, int>> keys = {
        {"ESC", {8, 2}}, {"TAB", {8, 4}}, {"CAP", {8, 6}}, {"SHI", {2, 5}}, {"CTR", {2, 7}},
        {"COP", {1, 1}}, {"CLR", {2, 0}}, {"DEL", {9, 7}}, {"RET", {2, 2}}, {"ENT", {0, 6}},
        {"ARL", {1, 0}}, {"ARR", {0, 1}}, {"ARU", {0, 0}}, {"ARD", {0, 2}},
        {"FN0", {1, 7}}, {"FN1", {1, 5}}, {"FN2", {1, 6}}, {"FN3", {0, 5}}, {"FN4", {2, 4}},
        {"FN5", {1, 4}}, {"FN6", {0, 4}}, {"FN7", {1, 2}}, {"FN8", {1, 3}}, {"FN9", {0, 3}},
    };
    auto it = keys.find(upper(code));
    if (it == keys.end()) return false;
    cell = it->second;
    return true;
}

constexpr std::pair<int, int> KEY_SHIFT{2, 5}, KEY_RETURN{2, 2}, KEY_SPACE{5, 7}, KEY_TAB{8, 4};

// The SSM standard's neutral bytes, and #FE/#FF, which only its reserved codes use.
bool ssmByte(int b) {
    return (b >= 0x00 && b <= 0x3F) || (b >= 0x7F && b <= 0x9F) || (b >= 0xA4 && b <= 0xA7) ||
           (b >= 0xAC && b <= 0xAF) || (b >= 0xB4 && b <= 0xB7) || (b >= 0xBC && b <= 0xBF) ||
           (b >= 0xC0 && b <= 0xFD) || b == 0xFE || b == 0xFF;
}

std::string withExtension(const std::string& name, const std::string& ext) {
    if (name.size() >= ext.size() && lower(name.substr(name.size() - ext.size())) == ext) return name;
    return name + ext;
}

// ERRATA: published scripts that cannot work as written, corrected in place. Returns a
// description of each change made to `lines`.
//
// SHAKE27A-0..4.CSL, test U (AU, "R4 & R9 CHECK"): the script waits 14 s after the key
// and then presses SPACE to leave the result screen. SHAKER 2.7 added a 9th sub-test to AU
// ("UPD R9=0, UPD R4=0 WHEN C4=1 & C9=7", not in 2.6's binary) that checks ten positions,
// each a 344-line frame and seven more counted by SHAKER's own VSYNC loops -- about 1.6 s
// more. The wait and its comment (11803953+1007210) are 2.6's, unchanged in the 2.7
// files. AU then ends 14.2-14.7 s after the key (14.23 on CRTC 1, 14.72 on CRTC 2; the same
// machine runs 2.6's AU in 12.48 s against the comment's 12.81). The script's SPACE comes
// 14.54 s after the key (press, gap, the 400000 after the key_output, the wait), so on
// CRTC 0 (14.62) and 2 (14.72) it lands while the test is still running, the next key
// ('I') is spent leaving the result screen, and the five AI screens (#008D-#0091) are never
// reached. The wait becomes 16 s.
std::vector<std::string> cslErrata(const std::string& path, std::vector<std::string>& lines) {
    std::vector<std::string> made;
    std::string name = upper(fs::path(path).filename().string());
    const bool shake27a = name.size() == 14 && name.rfind("SHAKE27A-", 0) == 0 && name.substr(10) == ".CSL"
                          && name[9] >= '0' && name[9] <= '4';
    if (!shake27a) return made;
    for (size_t n = 0; n + 1 < lines.size(); n++) {
        std::string ins, text, nextIns, nextText;
        std::vector<std::string> a, b;
        splitLine(lines[n], ins, a, text);
        if (ins != "key_output" || a.empty() || a[0] != "U") continue;
        size_t w = n + 1;
        while (w < lines.size()) {                       // the next instruction, past comments
            splitLine(lines[w], nextIns, b, nextText);
            if (!nextIns.empty()) break;
            w++;
        }
        if (w < lines.size() && nextIns == "wait" && b.size() == 1 && b[0] == "14000000") {
            lines[w] = "wait 16000000 ; erratum: SHAKER 2.7's AU runs a 9th sub-test (was: " + trim(lines[w]) + ")";
            made.push_back("line " + std::to_string(w + 1) + ": wait after key 'U' 14000000 -> 16000000 "
                           "(SHAKER 2.7 added a 9th AU sub-test; the script's wait is 2.6's)");
        }
    }
    return made;
}

const char* modelName(int model) {
    switch (model) {
        case 0: return "CPC 464"; case 1: return "CPC 664"; case 2: return "CPC 6128";
        case 4: return "6128 Plus"; case 5: return "464 Plus"; case 6: return "GX4000";
        default: return "?";
    }
}

} // namespace

bool writeVideoBmp(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t dataSize = (uint32_t)(w * h * 4), fileSize = 54 + dataSize;
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xff)); f.put((char)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) f.put((char)((v >> (8 * i)) & 0xff)); };
    f.put('B'); f.put('M'); u32(fileSize); u16(0); u16(0); u32(54);
    u32(40); u32((uint32_t)w); u32((uint32_t)(-h)); u16(1); u16(32);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    std::vector<uint8_t> row((size_t)w * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            uint8_t* d = &row[(size_t)x * 4];
            d[0] = (uint8_t)((p >> 16) & 0xff); d[1] = (uint8_t)((p >> 8) & 0xff); d[2] = (uint8_t)(p & 0xff); d[3] = 0xff;
        }
        f.write((char*)row.data(), (std::streamsize)row.size());
    }
    f.close();   // the last of it is written here
    return (bool)f;
}

CslPlayer::CslPlayer(GX4000& e, CpcVideo& v, CslSettings s) : emu(e), video(v), settings(std::move(s)) {
    emu.cpu->onUnwiredEd = [this](int suffix, int pcAfter) { onSsm(suffix, pcAfter); };
}

CslPlayer::~CslPlayer() {
    emu.cpu->onUnwiredEd = nullptr;
    if (log) std::fclose(log);
}

void CslPlayer::say(const char* format, ...) {
    char buffer[2048];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    if (log) { std::fputs(buffer, log); std::fputc('\n', log); std::fflush(log); }
    if (settings.echo) std::printf("%s\n", buffer);
}

double CslPlayer::seconds() const { return (double)(emu.machineCycles - origin) / 4000000.0; }

// ---------------------------------------------------------------- the script

bool CslPlayer::run(const std::string& path) {
    report.clear();
    if (!settings.logPath.empty()) {
        std::error_code ec;
        fs::path lp(settings.logPath);
        if (lp.has_parent_path()) fs::create_directories(lp.parent_path(), ec);
        if (log) std::fclose(log);
        log = std::fopen(settings.logPath.c_str(), "w");
    }
    screenshotDir.clear(); diskDir.clear(); tapeDir.clear(); snapshotDir.clear();
    say("CSL player (CSL %s, SSM 1.1): %s", SUPPORTED_VERSION, path.c_str());
    bool ok = runFile(path);
    if (ok) say("done: %d screenshot(s), %d snapshot(s)", shotCount, snapCount);
    else say("%s", report.c_str());
    if (log) { std::fclose(log); log = nullptr; }
    return ok;
}

bool CslPlayer::runFile(const std::string& pathIn) {
    std::string path = pathIn;
    if (!isFile(path)) {
        for (const char* ext : {".csl", ".CSL"}) if (isFile(pathIn + ext)) { path = pathIn + ext; break; }
    }
    std::ifstream in(path, std::ios::binary);
    Script script;
    script.path = path;
    script.dir = fs::path(path).has_parent_path() ? fs::path(path).parent_path().string() : std::string();
    if (!in) {
        if (report.empty()) {
            std::string where = stack.empty() ? path : stack.back().path + " line " + std::to_string(stack.back().line);
            report = "CSL error: cannot open the script " + path + " (" + where + ")";
        }
        return false;
    }
    if (stack.size() >= 16) { report = "CSL error: csl_load nested more than 16 deep at " + path; return false; }

    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l); ) lines.push_back(l);
    std::vector<std::string> corrected;
    if (settings.errata) corrected = cslErrata(path, lines);

    // The time budget: a hung program must not hold the run forever. From the script's
    // own waits, doubled, plus a minute for the loader and one for each open-ended wait.
    double cap = settings.timeCapSeconds;
    if (cap <= 0) {
        double waits = 0; int openEnded = 0;
        for (const auto& l : lines) {
            std::string ins, text; std::vector<std::string> a;
            splitLine(l, ins, a, text);
            double v = 0;
            if (ins == "wait" && !a.empty() && parseNumber(a[0], v)) waits += v;
            else if (ins == "wait_ssm0000" || ins == "wait_ssm" || ins == "wait_driveonoff" || ins == "wait_vsyncoffon") openEnded++;
        }
        cap = waits / 1e6 * 2.0 + 60.0 + 60.0 * openEnded;
    }
    cap = std::min(cap, 1e9);   // held where cap x 4 MHz still fits a long long (a script of huge waits)
    const long long savedCap = capAt;
    capAt = emu.machineCycles + (long long)(cap * 4000000.0);

    stack.push_back(script);
    say("script %s (time cap %.0f s)", path.c_str(), cap);
    for (const auto& c : corrected) say("  erratum applied, %s", c.c_str());
    bool ok = true;
    for (size_t n = 0; n < lines.size() && ok; n++) {
        std::string instruction, text;
        std::vector<std::string> args;
        splitLine(lines[n], instruction, args, text);
        if (instruction.empty()) continue;
        stack.back().line = (int)n + 1;
        stack.back().instruction = text;
        std::string why;
        // The readers throw for a file they cannot take (a disc, tape, snapshot or cartridge
        // the script names): that is the script's error, not the end of the program.
        try { ok = execute(instruction, args, text, why); }
        catch (const std::exception& ex) { ok = false; why = ex.what(); }
        if (!ok && report.empty()) {
            const Script& s = stack.back();
            report = "CSL error\n"
                     "  script:            " + s.path + "\n"
                     "  line:              " + std::to_string(s.line) + "\n"
                     "  instruction:       " + s.instruction + "\n"
                     "  reason:            " + why + "\n"
                     "  script version:    " + (s.versionText.empty() ? std::string("(not given)") : s.versionText) + "\n"
                     "  supported version: " + SUPPORTED_VERSION;
        }
    }
    stack.pop_back();
    capAt = savedCap;
    return ok;
}

bool CslPlayer::execute(const std::string& ins, const std::vector<std::string>& a, const std::string& text, std::string& why) {
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::string(); };
    auto need = [&](size_t n) {
        if (a.size() >= n) return true;
        why = "missing argument";
        return false;
    };
    auto number = [&](size_t i, double& v) {
        if (i < a.size() && parseNumber(a[i], v)) return true;
        why = i < a.size() ? "not a number: " + a[i] : "missing argument";
        return false;
    };
    // Every instruction but the plain waits goes in the log with the emulated time it ran
    // at: a key that reached a program too early or too late is read off these.
    if (ins != "csl_version" && ins != "wait" && ins != "key_delay") say("  [%8.3fs] %s", seconds(), text.c_str());

    // ---- miscellaneous
    if (ins == "csl_version") { stack.back().versionText = arg(0); return true; }
    if (ins == "reset") {
        std::string kind = lower(arg(0));
        if (kind.empty() || kind == "hard") return hardReset(why);
        if (kind == "soft") return softReset(why);
        why = "reset takes 'soft' or 'hard'";
        return false;
    }

    // ---- machine configuration: takes effect at the next reset (or first use)
    if (ins == "crtc_select") {
        if (!need(1)) return false;
        std::string c = upper(a[0]);
        int t = c == "0" ? 0 : c == "1" || c == "1A" ? 1 : c == "1B" ? 5 : c == "2" ? 2 : c == "3" ? 3 : c == "4" ? 4 : -1;
        if (t < 0) { why = "unknown CRTC '" + a[0] + "' (0, 1, 1A, 1B, 2, 3 and 4 are emulated)"; return false; }
        crtcSelected = t;
        configChanged = true;
        return true;
    }
    if (ins == "gate_array") {
        double v = 0;
        if (!number(0, v)) return false;
        int g = (int)v;
        if (g != 40007 && g != 40008 && g != 40010) { why = "unknown Gate Array " + a[0] + " (40007, 40008, 40010)"; return false; }
        gateArray = g;
        configChanged = true;
        return true;
    }
    if (ins == "cpc_model") {
        double v = 0;
        if (!number(0, v)) return false;
        int m = (int)v;
        if (m < 0 || m > 6 || m == 3) { why = "unknown CPC model " + a[0] + " (0=464 1=664 2=6128 4=6128+ 5=464+ 6=GX4000)"; return false; }
        model = m;
        configChanged = true;
        return true;
    }
    if (ins == "memory_exp") {
        double v = 0;
        if (!number(0, v)) return false;
        int x = (int)v;
        if (x < 0 || x > 4) { why = "unknown memory expansion " + a[0] + " (0 to 4)"; return false; }
        memoryExpansion = x;
        configChanged = true;
        return true;
    }
    if (ins == "rom_dir") { if (!need(1)) return false; romDirPrefix = a[0]; configChanged = true; return true; }
    if (ins == "rom_config") {
        if (!need(3)) return false;
        std::string type = upper(a[0]);
        double v = 0;
        if (!number(1, v)) return false;
        int slot = (int)v;
        if (type == "L") { if (slot != 0) { why = "a lower ROM is number 0"; return false; } lowerRomFile = a[2]; }
        else if (type == "U") { if (slot < 0 || slot > 255) { why = "upper ROM number out of range"; return false; } upperRomFiles[slot] = a[2]; }
        else if (type == "C") { if (slot != 0) { why = "a cartridge is number 0"; return false; } cartridgeFile = a[2]; }
        else if (type == "M") { why = "the Multiface 2 is not emulated"; return false; }
        else { why = "unknown ROM type '" + a[0] + "' (U, L, C or M)"; return false; }
        configChanged = true;
        return true;
    }

    // ---- directories
    if (ins == "disk_dir") { if (!need(1)) return false; diskDir = a[0]; return true; }
    if (ins == "tape_dir") { if (!need(1)) return false; tapeDir = a[0]; return true; }
    // Where a script SAVES: a folder below the one it is run in, never an absolute path or
    // one climbing out with '..' -- a script from elsewhere must not write over files
    // anywhere the user can write.
    auto staysInside = [&](const std::string& pIn) {
        // '\' as a separator everywhere: joinPrefix turns it into '/' on Linux, where
        // fs::path would otherwise read "..\..\x" as one name and let it through
        std::string p = pIn;
        std::replace(p.begin(), p.end(), '\\', '/');
        const fs::path path(p);
        if (path.has_root_path() || path.has_root_name()) { why = "a script may not save to an absolute path: " + p; return false; }
        for (const auto& part : path) if (part == "..") { why = "a script may not save outside its folder ('..'): " + p; return false; }
        return true;
    };
    if (ins == "snapshot_dir") { if (!need(1) || !staysInside(a[0])) return false; snapshotDir = a[0]; return true; }
    if (ins == "screenshot_dir") { if (!need(1) || !staysInside(a[0])) return false; screenshotDir = a[0]; return true; }
    if (ins == "screenshot_name") { if (!need(1) || !staysInside(a[0])) return false; screenshotName = a[0]; return true; }
    if (ins == "snapshot_name") { if (!need(1) || !staysInside(a[0])) return false; snapshotName = a[0]; return true; }
    if (ins == "snapshot_version") {
        double v = 0;
        if (!number(0, v)) return false;
        if ((int)v < 1 || (int)v > 3) { why = "snapshot version must be 1, 2 or 3"; return false; }
        snapshotVersion = (int)v;
        return true;
    }
    if (ins == "key_delay") {
        double p = 0, g = 0, r = 0;
        if (!number(0, p) || !number(1, g)) return false;
        r = g;
        if (a.size() > 2 && !number(2, r)) return false;
        keyPress = p; keyGap = g; keyAfterOutput = r;
        return true;
    }
    if (ins == "csl_load") {
        if (!need(1)) return false;
        if (!settings.followCslLoad) { say("  csl_load %s: not followed (this run plays one script)", a[0].c_str()); return true; }
        std::string target = a[0];
        std::string p = findFile("", target, stack.back().dir);
        if (p.empty()) p = findFile("", target + ".csl", stack.back().dir);
        if (p.empty()) p = findFile("", target + ".CSL", stack.back().dir);
        if (p.empty()) { why = "cannot find the script '" + target + "'"; return false; }
        return runFile(p);
    }

    // Everything below uses the machine.
    if (!ensureMachine(why)) return false;

    // ---- media
    if (ins == "disk_insert") {
        if (!need(1)) return false;
        int drive = 0;
        std::string name = a[0];
        if (a.size() >= 2) {
            std::string d = upper(a[0]);
            if (d != "A" && d != "B") { why = "drive must be A or B"; return false; }
            drive = d == "B" ? 1 : 0;
            name = a[1];
        }
        if (!emu.hasFdc) { why = std::string("the ") + modelName(model >= 0 ? model : settings.defaultModel) + " has no disc drive"; return false; }
        std::string ext = lower(fs::path(name).extension().string());
        if (ext != ".dsk" && ext != ".edsk") { why = "disc format '" + ext + "' is not supported (.dsk, .edsk)"; return false; }
        std::string p = findFile(diskDir, name, settings.diskDir);
        if (p.empty()) { why = "disc image not found: " + name; return false; }
        bool ok = false;
        Bytes d = readBytes(p, ok);
        if (!ok || d.empty() || !emu.fdc->mount(d, drive)) { why = "not a DSK/EDSK image: " + p; return false; }
        say("  disc %c: %s", drive ? 'B' : 'A', p.c_str());
        return true;
    }
    if (ins == "tape_insert") {
        if (!need(1)) return false;
        if (model == 6) { why = "the GX4000 has no tape"; return false; }
        std::string p = findFile(tapeDir, a[0], settings.tapeDir);
        if (p.empty()) { why = "tape image not found: " + a[0]; return false; }
        bool ok = false;
        Bytes d = readBytes(p, ok);
        if (!ok || d.empty() || !emu.tape->load(d, fs::path(p).filename().string())) { why = "not a tape image the emulator reads (.cdt, .tzx, .csw, .wav): " + p; return false; }
        say("  tape: %s", p.c_str());
        return true;
    }
    if (ins == "tape_play" || ins == "tape_stop") {
        if (!emu.tape->loaded) { why = "no tape inserted"; return false; }
        bool wantPlaying = ins == "tape_play";
        if (emu.tape->playing != wantPlaying) emu.tape->togglePlay();
        return true;
    }
    if (ins == "tape_rewind") {
        if (!emu.tape->loaded) { why = "no tape inserted"; return false; }
        emu.tape->rewind();
        return true;
    }
    if (ins == "snapshot_load") {
        if (!need(1)) return false;
        std::string p = findFile(snapshotDir, a[0], settings.snapshotDir);
        if (p.empty()) { why = "snapshot not found: " + a[0]; return false; }
        bool ok = false;
        Bytes d = readBytes(p, ok);
        Snapshot snap = ok ? parseSna(d) : Snapshot{};
        if (snap.ram.empty()) { why = "not an SNA snapshot: " + p; return false; }
        applySna(&emu, snap);
        crtc = emu.crtc->type;
        say("  snapshot loaded: %s (v%d)", p.c_str(), snap.version);
        return true;
    }

    // ---- keys
    if (ins == "key_output" || ins == "key_from_file") {
        if (!need(1)) return false;
        std::string textToType = a[0];
        if (ins == "key_from_file") {
            std::string p = findFile("", a[0], stack.back().dir);
            if (p.empty()) { why = "file not found: " + a[0]; return false; }
            bool ok = false;
            Bytes d = readBytes(p, ok);
            textToType.assign(d.begin(), d.end());
        }
        std::vector<Stroke> strokes;
        if (!parseKeys(textToType, ins == "key_from_file", strokes, why)) return false;
        return typeStrokes(strokes, why);
    }
    if (ins == "keyboard_write") {
        if (a.size() != 10) { why = "keyboard_write takes the 10 rows of the matrix"; return false; }
        for (int row = 0; row < 10; row++) {
            double v = 0;
            if (!number((size_t)row, v)) return false;
            int bits = (int)v & 0xff;
            for (int bit = 0; bit < 8; bit++) emu.keyboard->setMatrixDirect(row, bit, ((bits >> bit) & 1) == 0);
        }
        return true;
    }

    // ---- synchronisation
    if (ins == "wait") {
        double us = 0;
        if (!number(0, us)) return false;
        return wait(us, why);
    }
    if (ins == "wait_driveonoff") {
        double n = 1;
        if (!a.empty() && !number(0, n)) return false;
        int left = std::max(1, (int)n);
        bool wasOn = emu.fdc->motor;
        bool ok = advanceUntil([&]() {
            bool on = emu.fdc->motor;
            if (wasOn && !on) left--;
            wasOn = on;
            return left <= 0;
        }, why);
        if (!ok) return false;
        say("  [%8.3fs] drive motor switched off", seconds());
        return true;
    }
    if (ins == "wait_vsyncoffon") {
        bool was = emu.ppi->vsyncProvider();
        return advanceUntil([&]() {
            bool on = emu.ppi->vsyncProvider();
            bool rose = on && !was;
            was = on;
            return rose;
        }, why);
    }
    if (ins == "wait_ssm0000") {
        ssm0000Seen = false;
        if (!advanceUntil([&]() { return ssm0000Seen; }, why)) return false;
        say("  [%8.3fs] SSM #0000", seconds());
        return true;
    }
    // CSL 1.5: "wait_ssm <ssm code> -- Wait for SSM Code <ssm code>, expressed in
    // hexadecimal prefixed by '0x'" -- "a better method than waiting for a delay, as some
    // emulators can respond more or less quickly". A code that asks for a screenshot has it
    // taken on the picture the program was drawing (handlePicture); the wait ends once that
    // is written, so the script's next key cannot land in the picture it is waiting for.
    if (ins == "wait_ssm") {
        std::string a = lower(arg(0));
        if (a.rfind("0x", 0) == 0) a = a.substr(2);
        else if (!a.empty() && (a[0] == '#' || a[0] == '&')) a = a.substr(1);
        char* end = nullptr;
        const long code = a.empty() ? -1 : std::strtol(a.c_str(), &end, 16);
        if (a.empty() || !end || *end || code < 0 || code > 0xFFFF) {
            why = "wait_ssm needs a 16-bit SSM code in hexadecimal, e.g. wait_ssm 0xABCD";
            return false;
        }
        const bool ok = advanceUntil([&]() {
            bool seen = false;
            for (int c : ssmSinceWait) if (c == code) seen = true;
            if (!seen) return false;
            for (int pending : pendingShots) if (pending == code) return false;   // its picture first
            return true;
        }, why);
        ssmSinceWait.clear();
        if (!ok) return false;
        say("  [%8.3fs] SSM #%04lX received", seconds(), code);
        return true;
    }

    // ---- exports
    if (ins == "screenshot" || ins == "snapshot") {
        bool atVsync = lower(arg(0)) == "vsync";
        if (!arg(0).empty() && !atVsync) { why = "the only option is 'vsync'"; return false; }
        if (atVsync) {
            bool was = emu.ppi->vsyncProvider();
            if (!advanceUntil([&]() { bool on = emu.ppi->vsyncProvider(); bool rose = on && !was; was = on; return rose; }, why))
                return false;
        }
        if (ins == "screenshot") return takeScreenshot(-1, why);
        std::string name = snapshotName.empty() ? ssmName(0xFFFF) : snapshotName;
        snapshotName.clear();
        return takeSnapshot(resolveOut(snapshotDir.empty() ? settings.snapshotDir : snapshotDir, withExtension(name, ".sna")), why);
    }

    why = "unknown instruction";
    return false;
}

// ---------------------------------------------------------------- the machine

bool CslPlayer::ensureMachine(std::string& why) {
    if (machineBuilt && !configChanged) return true;
    return hardReset(why);
}

std::string CslPlayer::findFile(const std::string& prefix, const std::string& name, const std::string& fallbackDir) const {
    std::vector<std::string> tries;
    if (!prefix.empty()) tries.push_back(joinPrefix(prefix, name));
    tries.push_back(name);
    if (!stack.empty() && !stack.back().dir.empty()) tries.push_back((fs::path(stack.back().dir) / name).string());
    if (!fallbackDir.empty()) tries.push_back((fs::path(fallbackDir) / name).string());
    for (const auto& t : tries) if (isFile(t)) return t;
    return "";
}

// A firmware file by keyword, as the front end finds them: every space-separated token
// must be in the lower-cased name, a token starting with '!' must not.
std::string CslPlayer::findFirmware(const std::string& keyword) const {
    std::vector<std::string> need, avoid;
    {
        std::string cur;
        auto flush = [&]() { if (!cur.empty()) { if (cur[0] == '!') avoid.push_back(cur.substr(1)); else need.push_back(cur); cur.clear(); } };
        for (char ch : lower(keyword)) { if (ch == ' ') flush(); else cur += ch; }
        flush();
    }
    std::vector<std::string> dirs;
    if (!romDirPrefix.empty() && isDir(romDirPrefix)) dirs.push_back(romDirPrefix);
    dirs.push_back(settings.romDir);
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!isDir(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string n = lower(e.path().filename().string());
            bool ok = true;
            for (const auto& t : need) if (n.find(t) == std::string::npos) { ok = false; break; }
            if (ok) for (const auto& t : avoid) if (n.find(t) != std::string::npos) { ok = false; break; }
            if (ok) return e.path().string();
        }
    }
    return "";
}

std::string CslPlayer::findSystemCartridge() const {
    std::vector<std::string> dirs;
    if (!romDirPrefix.empty() && isDir(romDirPrefix)) dirs.push_back(romDirPrefix);
    dirs.push_back(settings.romDir);
    dirs.push_back("roms");
    dirs.push_back("media");
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!isDir(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string n = lower(e.path().filename().string());
            if (lower(e.path().extension().string()) == ".cpr" && n.find("burning") != std::string::npos
                && n.find("rubber") != std::string::npos) return e.path().string();
        }
    }
    return "";
}

bool CslPlayer::hardReset(std::string& why) {
    const int crtcWanted = settings.forceCrtc >= 0 ? settings.forceCrtc : crtcSelected;
    // The CRTC 3 is the 6128 Plus's ASIC, so selecting it with no model named builds that
    // machine. The CRTC 4 is the cost-down 6128's ASIC -- a classic CPC -- so it keeps the
    // default model.
    const int m = model >= 0 ? model : (crtcWanted == 3 && !settings.classicCrtc3 ? 4 : settings.defaultModel);
    const bool plus = m >= 4;
    auto romPath = [&](const std::string& file) {
        std::string p = findFile(romDirPrefix, file, settings.romDir);
        if (p.empty()) why = "ROM not found: " + joinPrefix(romDirPrefix, file);
        return p;
    };
    auto load = [&](const std::string& path, Bytes& out) {
        bool ok = false;
        out = readBytes(path, ok);
        if (!ok || out.empty()) { why = "cannot read " + path; return false; }
        return true;
    };

    // memory_exp: 0 = 128K (C4..C7), 1 = 256K (C4..DF), 2 = the 256K silicon disc (E4..FF),
    // 3 = 4M, 4 = 512K (dk'tronics). Expansion page n is the 64K selected by &7Fxx's
    // bits 5..3; the silicon disc answers pages 4..7 only, beside a 6128's own page 0.
    int ram = (m == 2 || m == 4) ? 128 : 64;
    int pages = 0xff;
    switch (memoryExpansion) {
        case 0: ram = 128; break;
        case 1: ram = 320; break;
        case 2: ram = 576; pages = (m == 2 || m == 4) ? 0xF1 : 0xF0; break;
        case 3: ram = 4160; break;
        case 4: ram = 576; break;
        default: break;
    }

    plusMenu = false;
    if (plus) {
        if (crtcWanted >= 0 && crtcWanted != 3) {
            why = std::string("the ") + modelName(m) + "'s CRTC is its ASIC, the CRTC 3; CRTC " + std::to_string(crtcWanted) + " cannot be fitted";
            return false;
        }
        if (gateArray) { why = std::string("the ") + modelName(m) + " has no separate Gate Array (the ASIC is one)"; return false; }
        std::string cartPath;
        if (!cartridgeFile.empty()) { cartPath = romPath(cartridgeFile); if (cartPath.empty()) return false; }
        else {
            cartPath = findSystemCartridge();
            if (cartPath.empty()) { why = "the Plus system cartridge (a .cpr named \"...Burning Rubber...\") is not in " + settings.romDir; return false; }
        }
        Bytes data;
        if (!load(cartPath, data)) return false;
        Cartridge cart = parseCartridge(data);
        if (cart.banks.empty()) { why = "not a CPR cartridge: " + cartPath; return false; }
        GX4000::LoadCartridgeOptions o;
        o.plusComputer = m != 6;
        o.ramKiB = ram;
        o.ram128 = ram >= 128;
        emu.loadCartridge(cart, o);
        emu.hasFdc = m == 4 || upperRomFiles.count(7) != 0;
        crtc = 3;
        plusMenu = o.plusComputer && cartridgeFile.empty();
        say("  machine: %s, %dK, CRTC 3 (ASIC), cartridge %s", modelName(m), ram, fs::path(cartPath).filename().string().c_str());
    } else {
        const std::string k = m == 0 ? "464" : m == 1 ? "664" : "6128";
        Bytes os, basic, amsdos;
        std::string osPath = lowerRomFile.empty() ? findFirmware(k + " os !+") : romPath(lowerRomFile);
        if (osPath.empty()) { if (why.empty()) why = "no " + k + " OS ROM in " + settings.romDir; return false; }
        std::string basicPath = upperRomFiles.count(0) ? romPath(upperRomFiles[0]) : findFirmware(k + " basic !+");
        if (basicPath.empty()) { if (why.empty()) why = "no " + k + " BASIC ROM in " + settings.romDir; return false; }
        if (!load(osPath, os) || !load(basicPath, basic)) return false;
        if (m != 0 || upperRomFiles.count(7)) {
            std::string amsPath = upperRomFiles.count(7) ? romPath(upperRomFiles[7]) : findFirmware("amsdos");
            if (amsPath.empty()) { if (why.empty()) why = "no AMSDOS ROM in " + settings.romDir; return false; }
            if (!load(amsPath, amsdos)) return false;
        }
        crtc = crtcWanted >= 0 ? crtcWanted : 1;
        if (gateArray && (crtc == 3 || crtc == 4)) {
            why = "with the CRTC " + std::to_string(crtc) + " the Gate Array is part of the same ASIC; gate_array cannot choose another";
            return false;
        }
        GX4000::LoadClassicOptions o;
        o.lowerRom = os; o.basicRom = basic; o.amsdosRom = amsdos;
        o.ramKiB = ram; o.ram128 = ram >= 128; o.crtcType = crtc;
        o.model = std::string("Amstrad ") + modelName(m);
        emu.loadClassicFirmware(o);
        if (gateArray == 40007) emu.gateArray->model = gateArrayModel40007();
        else if (gateArray == 40008) emu.gateArray->model = gateArrayModel40008();
        else if (gateArray == 40010) emu.gateArray->model = gateArrayModel40010();
        emu.hasFdc = !amsdos.empty();
        say("  machine: %s, %dK, CRTC %s%s", modelName(m), ram, crtc == 5 ? "1-B" : std::to_string(crtc).c_str(),
            gateArray ? (", Gate Array " + std::to_string(gateArray)).c_str() : "");
    }
    for (const auto& [slot, file] : upperRomFiles) {
        if (!plus && (slot == 0 || slot == 7)) continue;          // the firmware's own, fitted above
        std::string p = romPath(file);
        Bytes rom;
        if (p.empty() || !load(p, rom)) return false;
        emu.memory->setUpperRom(slot, rom);
    }
    emu.memory->expansionPages = pages;
    emu.ay->setOutputSampleRate(44100);
    emu.keyboard->setRegion("uk");
    // The set the machine came with (ACCC 15.1's centred pairing): the core takes it at the
    // reset that ended the build; the renderer's tube follows it.
    video.setMonitorModel(emu.monitorRenderer->model);
    machineBuilt = true;
    configChanged = false;
    if (onMachineBuilt) onMachineBuilt();
    machineStarted();
    return true;
}

bool CslPlayer::softReset(std::string& why) {
    if (!machineBuilt || configChanged) return hardReset(why);
    // "memory cleared by the rom and only concerns the 64K of central ram": the machine
    // is reset, and every byte of RAM survives it for the ROM to clear what it clears.
    Bytes ram = emu.memory->ram;
    emu.reset();
    emu.memory->ram = ram;
    say("  soft reset");
    machineStarted();
    return true;
}

// The 6128 Plus and 464 Plus start in their system cartridge's menu; BASIC is F1 on it.
// The menu is up after about half a second, so F1 goes down at one second, for a tenth.
void CslPlayer::plusMenuToBasic() {
    const long long start = emu.machineCycles;
    auto runFor = [&](double usec) {
        const long long until = emu.machineCycles + (long long)(usec * 4.0);
        while (emu.machineCycles < until) stepOne();
    };
    runFor(1000000);
    emu.keyboard->setMatrixDirect(1, 5, true);
    runFor(100000);
    emu.keyboard->setMatrixDirect(1, 5, false);
    say("  Plus menu: F1 (BASIC) after %.1f s", (double)(emu.machineCycles - start) / 4000000.0);
}

void CslPlayer::machineStarted() {
    pendingLow = -1; pendingNextAddress = -1;
    pendingShots.clear(); pendingSnapshot = false; ssmSinceWait.clear();
    if (plusMenu) plusMenuToBasic();
    origin = emu.machineCycles;       // the script's clock starts here
    scriptT = -1;
}

// ---------------------------------------------------------------- time

long CslPlayer::picture() const {
    return emu.classicMonitorFrame;
}

// One instruction, with what GX4000::runFrame does between two pictures, so a script
// sees the same machine whether it waits for a second or a microsecond.
void CslPlayer::stepOne() {
    const long before = picture();
    if (emu.breakpoints) emu.debuggerBreakpointHit();      // a host's PC trace hangs off this
    if (onStep) {
        const int pc = emu.cpu->pc & 0xffff;
        const long long start = emu.machineCycles;
        emu.stepInstruction();
        onStep(pc, emu.machineCycles - start);
    } else emu.stepInstruction();
    if (pendingSnapshot) {
        pendingSnapshot = false;
        std::string name = snapshotName.empty() ? ssmName(0xFFFF) : snapshotName;
        snapshotName.clear();
        std::string why;
        if (!takeSnapshot(resolveOut(snapshotDir.empty() ? settings.snapshotDir : snapshotDir, withExtension(name, ".sna")), why))
            say("  SSM #FFFF: %s", why.c_str());
    }
    if (picture() != before) {
        if (!emu.plusHardware) emu.videoCaptureRegisters = emu.crtc->registers;
        handlePicture();
    }
}

// An SSM screenshot shows the picture the program was drawing when it asked: the one
// that completes after the code.
void CslPlayer::handlePicture() {
    if (pendingShots.empty()) { if (onPicture) onPicture(); return; }
    std::vector<int> codes;
    codes.swap(pendingShots);
    for (int code : codes) {
        std::string why;
        if (!takeScreenshot(code, why)) say("  SSM #%04X: %s", code, why.c_str());
    }
}

bool CslPlayer::advanceTo(long long target, std::string& why) {
    while (emu.machineCycles - origin < target) {
        stepOne();
        if (emu.machineCycles > capAt) { why = "time cap reached (the program did not get there)"; return false; }
    }
    return true;
}

bool CslPlayer::advanceUntil(const std::function<bool()>& done, std::string& why) {
    while (!done()) {
        stepOne();
        if (emu.machineCycles > capAt) { why = "time cap reached while waiting"; return false; }
    }
    reanchor();
    return true;
}

// The script's clock: every wait is measured from where the previous one ENDED on the
// script's timeline, not from wherever the last instruction happened to stop, so the
// error of stopping on an instruction boundary never accumulates.
bool CslPlayer::wait(double usec, std::string& why) {
    if (scriptT < 0) scriptT = emu.machineCycles - origin;
    scriptT += (long long)(usec * 4.0);
    return advanceTo(scriptT, why);
}

void CslPlayer::reanchor() { scriptT = emu.machineCycles - origin; }

// ---------------------------------------------------------------- SSM

void CslPlayer::onSsm(int suffix, int pcAfter) {
    if (!ssmByte(suffix)) { pendingLow = -1; return; }
    const int start = (pcAfter - 2) & 0xffff;
    if (pendingLow < 0 || start != pendingNextAddress) {
        pendingLow = suffix;
        pendingNextAddress = (start + 2) & 0xffff;
        return;
    }
    const int code = suffix << 8 | pendingLow;
    const int low = pendingLow;
    pendingLow = -1;
    if (ssmSinceWait.size() < 256) ssmSinceWait.push_back(code);   // CSL 1.5 wait_ssm
    if (low >= 0xFE && suffix != 0xFF) return;                  // #FE/#FF are only for #FFxx
    if (code == 0x0000) { ssm0000Seen = true; return; }
    if (code == 0xFFFF) { pendingSnapshot = true; return; }
    if (code == 0xFFFE) { pendingShots.push_back(code); return; }
    if (code == 0xFFFD || code == 0xFFFC) {
        say("  [%8.3fs] SSM #%04X (Sikoview event log %s: no inspector attached)", seconds(), code, code == 0xFFFD ? "start" : "breakpoint");
        return;
    }
    if ((code & 0xFF00) == 0xFF00) { say("  [%8.3fs] SSM #%04X reserved, ignored", seconds(), code); return; }
    pendingShots.push_back(code);
}

std::string CslPlayer::ssmName(int code) const {
    const int n = (crtc == 5 && settings.crtcTag.empty()) ? 1 : crtc;   // a 1-B is a CRTC 1
    char b[160];
    std::snprintf(b, sizeof(b), "%s_%s%d_%04X", settings.emulatorName.c_str(), settings.crtcTag.c_str(), n, code & 0xffff);
    return b;
}

std::string CslPlayer::resolveOut(const std::string& prefix, const std::string& name) const {
    std::string path = prefix.empty() ? name : joinPrefix(prefix, name);
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    return path;
}

bool CslPlayer::takeScreenshot(int code, std::string& why) {
    std::string name;
    if (code == -1 || code == 0xFFFE) {
        if (!screenshotName.empty()) { name = screenshotName; screenshotName.clear(); }
        else if (code == 0xFFFE) name = ssmName(0xFFFE);
        else {
            char b[160];
            std::snprintf(b, sizeof(b), "%s_%s%d_SHOT%04d", settings.emulatorName.c_str(), settings.crtcTag.c_str(),
                          (crtc == 5 && settings.crtcTag.empty()) ? 1 : crtc, ++unnamedShots);
            name = b;
        }
    } else {
        name = ssmName(code);
    }
    const std::string path = resolveOut(screenshotDir.empty() ? settings.screenshotDir : screenshotDir, withExtension(name, ".bmp"));
    if (beforeScreenshot) beforeScreenshot(code);
    video.render();
    if (!writeVideoBmp(path, video.pixels, video.width, video.height)) { why = "cannot write " + path; return false; }
    shotCount++;
    if (code >= 0) say("  [SSM %04X] -> %s  t=%.3fs", code & 0xffff, path.c_str(), seconds());
    else say("  [screenshot] -> %s  t=%.3fs", path.c_str(), seconds());
    if (onScreenshot) onScreenshot(code, path);
    return true;
}

bool CslPlayer::takeSnapshot(const std::string& path, std::string& why) {
    Bytes data = createSna(&emu, false);
    if (snapshotVersion < 3 && data.size() >= 256) {
        // A version 1 or 2 file: the header up to what that version defines, and 64K or
        // 128K of RAM -- the only dumps those versions know.
        const size_t ramBytes = std::min<size_t>(emu.memory->ram.size(), 128 * 1024);
        Bytes h(data.begin(), data.begin() + 256);
        h[0x10] = (uint8_t)snapshotVersion;
        for (size_t i = snapshotVersion == 1 ? 0x6d : 0x76; i < 256; i++) h[i] = 0;
        h[0x6b] = (uint8_t)((ramBytes / 1024) & 0xff); h[0x6c] = (uint8_t)((ramBytes / 1024) >> 8);
        Bytes out(h);
        out.insert(out.end(), emu.memory->ram.begin(), emu.memory->ram.begin() + (std::ptrdiff_t)ramBytes);
        data.swap(out);
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) { why = "cannot write " + path; return false; }
    f.write((const char*)data.data(), (std::streamsize)data.size());
    f.close();
    if (!f) { why = "could not write all of " + path + " (disc full?)"; return false; }
    snapCount++;
    say("  [snapshot v%d] -> %s  t=%.3fs", snapshotVersion, path.c_str(), seconds());
    return true;
}

// ---------------------------------------------------------------- keys

// key_output's text as strokes: one key, a {group} held together, or a \(code). A
// character the keyboard cannot type is skipped, as the standard says.
bool CslPlayer::parseKeys(const std::string& text, bool fromFile, std::vector<Stroke>& out, std::string& why) const {
    Stroke* group = nullptr;
    Stroke groupStroke;
    auto add = [&](const std::vector<std::pair<int, int>>& cells, bool isReturn) {
        if (group) { group->cells.insert(group->cells.end(), cells.begin(), cells.end()); group->isReturn |= isReturn; return; }
        Stroke s; s.cells = cells; s.isReturn = isReturn;
        out.push_back(s);
    };
    auto addChar = [&](char c) {
        if (c == '\n') { add({KEY_RETURN}, true); return; }
        if (c == '\r') return;
        if (c == ' ') { add({KEY_SPACE}, false); return; }
        if (c == '\t') { add({KEY_TAB}, false); return; }
        auto cell = cpcCharacterMapping(std::string(1, c), "", "uk");
        if (!cell) return;
        std::vector<std::pair<int, int>> cells{{(*cell)[0], (*cell)[1]}};
        if ((*cell)[2]) cells.push_back(KEY_SHIFT);
        add(cells, false);
    };
    for (size_t i = 0; i < text.size(); ) {
        char c = text[i];
        if (!fromFile && c == '\\' && i + 1 < text.size() && text[i + 1] == '(') {
            size_t close = text.find(')', i + 3);
            if (close == std::string::npos) { why = "unterminated \\( code"; return false; }
            std::string code = text.substr(i + 2, close - i - 2);
            i = close + 1;
            std::pair<int, int> cell;
            if (upper(code) == "KOF") {
                if (group) group->noDelayAfter = true;
                else if (!out.empty()) out.back().noDelayAfter = true;
            } else if (specialKey(code, cell)) {
                add({cell}, upper(code) == "RET" || upper(code) == "ENT");
            } else if (code == "{" || code == "}" || code == "\\" || code == "'") {
                addChar(code[0]);
            } else {
                why = "unknown key code \\(" + code + ")";
                return false;
            }
            continue;
        }
        if (!fromFile && c == '{') {
            if (group) { why = "a { group inside a { group"; return false; }
            groupStroke = Stroke{};
            group = &groupStroke;
            i++;
            continue;
        }
        if (!fromFile && c == '}') {
            if (!group) { why = "a } with no {"; return false; }
            out.push_back(groupStroke);
            group = nullptr;
            i++;
            continue;
        }
        addChar(c);
        i++;
    }
    if (group) { why = "a { group is not closed"; return false; }
    return true;
}

// Each stroke: its keys down for the press delay, up, then the gap before the next (none
// after a stroke marked \(KOF)). And when the whole key_output is sent, key_delay's third
// value -- the time "after sending a CR", which is how each key_output line ends a command.
//
// That last wait is what the published scripts are timed for, and they cannot work
// without it. SHAKER's own comments give the time from the key to its SSM code: AI's second
// screen takes 2315008 usec and the script sends the next key 2200000 usec after its
// key_output, which with only the press and the gap (70000 + 70000) leaves 25 ms -- and
// several comments (865.8 ms, 845.8 ms) are longer than the `wait 800000` they sit on.
// With the 400000 after each key_output every one of them has room. Reading the third value
// as a gap for RETURN strokes only lost screens on every chip (AI/B-E, AU's successor).
bool CslPlayer::typeStrokes(const std::vector<Stroke>& strokes, std::string& why) {
    for (const auto& s : strokes) {
        for (const auto& [row, bit] : s.cells) emu.keyboard->setMatrixDirect(row, bit, true);
        if (!wait(keyPress, why)) return false;
        for (const auto& [row, bit] : s.cells) emu.keyboard->setMatrixDirect(row, bit, false);
        if (s.noDelayAfter) continue;
        if (!wait(keyGap, why)) return false;
    }
    return wait(keyAfterOutput, why);
}

} // namespace cpcse
