// CPCSyntaxError GUI — emulator host implementation.
#include "emuhost.h"
#include "../core/monitor_model.h"
#include "keymap.h"
#include "core/emulator.h"
#include "core/video.h"
#include "core/ay.h"
#include "core/keyboard.h"
#include "core/tape.h"
#include "core/fdc.h"
#include "core/dsk.h"
#include "core/cpr_loader.h"
#include "core/sna.h"
#include "core/memory.h"
#include "core/crtc.h"
#include "core/z80.h"
#include "core/v9990.h"
#include "core/m4.h"
#include "core/symbiface_mouse.h"
#include "core/sf3.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace cpcse {

static const int SCREEN_W = 768;
static const int SCREEN_H = 544;

// ----------------------------------------------------------------------------
// An M4 virtual drive backed by a real host folder. The M4 firmware sees the
// folder's contents as its SD card. File data is loaded into the base struct's
// `files`/`dirs` maps (which M4Board reads directly); mutations write back to
// the folder. Names are upper-cased FAT-style, keyed by absolute path.
// ----------------------------------------------------------------------------
namespace {

std::string m4Upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string m4BaseName(const std::string& full) { auto s = full.rfind('/'); return s == std::string::npos ? full : full.substr(s + 1); }
std::string m4ParentOf(const std::string& full) { auto s = full.rfind('/'); if (s == std::string::npos || s == 0) return "/"; return full.substr(0, s); }
std::string m4Join(const std::string& cwd, const std::string& name) {
    std::string n = name;
    for (char& c : n) if (c == '\\') c = '/';
    if (!n.empty() && n[0] == '/') return m4Upper(n);
    std::string r = cwd;
    if (r.empty() || r.back() != '/') r += '/';
    r += n;
    return m4Upper(r);
}

class HostFolderM4Drive : public M4Drive {
public:
    std::string root;
    explicit HostFolderM4Drive(const std::string& dir) : root(dir) {
        readOnly = false; limitBytes = 16 * 1024 * 1024; cwd = "/";
        rescan();
    }
    void rescan() {
        files.clear(); dirs.clear(); dirs.insert("/");
        namespace fs = std::filesystem; std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec)) return;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code rc;
            std::string rel = fs::relative(it->path(), root, rc).generic_string();
            if (rel.empty() || rel[0] == '.') continue;
            std::string full = m4Upper("/" + rel);
            if (it->is_directory(ec)) dirs.insert(full);
            else if (it->is_regular_file(ec)) {
                bool ok = false; Bytes data = EmuHost::readFile(it->path().string(), ok);
                files[full] = M4File{ full, m4BaseName(full), data, (int)data.size() };
            }
        }
    }
    std::string hostPathFor(const std::string& full) const {
        std::string p = full; if (!p.empty() && p[0] == '/') p = p.substr(1);
        return (std::filesystem::path(root) / p).string();
    }
    std::vector<M4DriveRow> list(const std::string& path) override {
        std::vector<M4DriveRow> rows;
        std::string p = path.empty() ? "/" : path;
        for (const auto& d : dirs) if (d != "/" && m4ParentOf(d) == p) rows.push_back({ m4BaseName(d), "dir", 0 });
        for (const auto& kv : files) if (m4ParentOf(kv.second.path) == p) rows.push_back({ kv.second.name, "file", kv.second.size });
        return rows;
    }
    M4File* find(const std::string& name) override {
        std::string full = m4Join(cwd, name);
        auto it = files.find(full);
        return it == files.end() ? nullptr : &it->second;
    }
    void chdir(const std::string& path) override { if (dirs.count(path)) cwd = path; }
    void addFile(const std::string& path, const Bytes& data) override {
        if (readOnly) return;
        std::string full = m4Upper(path);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(hostPathFor(full)).parent_path(), ec);
        std::ofstream f(hostPathFor(full), std::ios::binary);
        if (f) f.write((const char*)data.data(), (std::streamsize)data.size());
        files[full] = M4File{ full, m4BaseName(full), data, (int)data.size() };
    }
    void deletePath(const std::string& path) override {
        if (readOnly) return;
        std::string full = m4Upper(path);
        std::error_code ec; std::filesystem::remove(hostPathFor(full), ec);
        files.erase(full); dirs.erase(full);
    }
    bool rename(const std::string& oldName, const std::string& newName) override {
        if (readOnly) return false;
        std::string oldFull = m4Join(cwd, oldName), newFull = m4Join(cwd, newName);
        auto it = files.find(oldFull);
        if (it == files.end()) return false;
        std::error_code ec; std::filesystem::rename(hostPathFor(oldFull), hostPathFor(newFull), ec);
        M4File nf = it->second; nf.path = newFull; nf.name = m4BaseName(newFull);
        files.erase(it); files[newFull] = nf;
        return true;
    }
    void ensureDir(const std::string& path) override {
        if (readOnly) return;
        std::string full = m4Upper(path);
        std::error_code ec; std::filesystem::create_directories(hostPathFor(full), ec);
        dirs.insert(full);
    }
};

} // namespace

void EmuHost::setBeamRenderer(bool on) {
    beamRenderer = on;
    if (!video || !emu) return;
    if (on) {
        video->beamReset();
        emu->onBeamCharacter = [this]() { video->plotBeamCharacter(); };
        emu->onBeamHsync = [this]() { video->beamHsync(); };
        emu->onBeamVsync = [this]() {
            // ACCC §16.6: the retrace height follows the C-VSYNC's position in the line.
            video->beamVsyncAtCharacter(emu->monitorSyncCharacter, emu->monitorSyncLineLength);
            video->beamVsync();
        };
    } else {
        emu->onBeamCharacter = nullptr;
        emu->onBeamHsync = nullptr;
        emu->onBeamVsync = nullptr;
        video->beamMode = false;
    }
}

EmuHost::EmuHost() {
    emu = new GX4000();
    video = new CpcVideo(SCREEN_W, SCREEN_H, emu->memory, emu->asic, emu->crtc, emu->gateArray);
    video->monitor = emu->monitorRenderer;   // where the field sits vertically is the set's answer
    applyMonitorSet();                    // the set the machine shipped with, and its tube
    // A calm dark field before anything is booted.
    std::fill(video->pixels.begin(), video->pixels.end(), 0xff101418u);
    applyAudioRate();
    scanModels();
}

EmuHost::~EmuHost() {
    delete video;
    delete emu;
    delete m4Drive;
}

void EmuHost::applyAudioRate() {
    if (emu && emu->ay) emu->ay->setOutputSampleRate((double)sampleRate);
    if (emu && emu->tape) emu->tape->setOutputSampleRate((double)sampleRate);
}

Bytes EmuHost::readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    Bytes data((size_t)std::max<std::streamoff>(0, n));
    if (n > 0) f.read(reinterpret_cast<char*>(data.data()), n);
    ok = (bool)f || f.eof();
    return data;
}

static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

std::string EmuHost::findRom(const std::string& keyword) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(romDir, ec)) return "";
    // keyword is space-separated tokens that must all be present; a token
    // prefixed with '!' must be ABSENT (e.g. "6128 os !+" matches the plain
    // 6128 OS but not the "464+ and 6128+ OS" Plus ROM).
    std::vector<std::string> need, avoid;
    { std::string cur; auto flush = [&]() { if (!cur.empty()) { if (cur[0] == '!') avoid.push_back(cur.substr(1)); else need.push_back(cur); cur.clear(); } };
      for (char c : lower(keyword)) { if (c == ' ') flush(); else cur += c; } flush(); }
    for (const auto& entry : fs::directory_iterator(romDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = lower(entry.path().filename().string());
        bool ok = true;
        for (const auto& t : need) if (name.find(t) == std::string::npos) { ok = false; break; }
        if (ok) for (const auto& t : avoid) if (name.find(t) != std::string::npos) { ok = false; break; }
        if (ok) return entry.path().string();
    }
    return "";
}

void EmuHost::setRomDir(const std::string& dir) {
    romDir = dir;
    scanModels();
}

void EmuHost::scanModels() {
    models = {
        // id            label                 osKey        basicKey      amsdos ram  crtc plus cartKey
        { "cpc464",     "Amstrad CPC 464",   "464 os !+",  "464 basic !+", false, 64,  1, false, "" },
        { "cpc6128",    "Amstrad CPC 6128",  "6128 os !+", "6128 basic !+",true,  128, 1, false, "" },
        { "cpc464plus", "Amstrad 464 Plus",  "",           "",             false, 64,  3, true,  "burning rubber" },
        { "cpc6128plus","Amstrad 6128 Plus", "",           "",             true,  128, 3, true,  "burning rubber" },
        // GX4000: a 464 Plus with no keyboard/tape and dual joystick input (all
        // other Plus/ASIC features). computer=false → no keyboard, console mode.
        { "gx4000",     "Amstrad GX4000",    "",           "",             false, 64,  3, true,  "burning rubber", false, false },
    };
    for (auto& m : models) {
        if (m.plus) m.available = !findCart(m.cartKey).empty();
        else        m.available = !findRom(m.osKey).empty() && !findRom(m.basicKey).empty();
    }
}

std::string EmuHost::findCart(const std::string& keyword) const {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs = { mediaDir, romDir, "media", "../media", "assets/games" };
    std::vector<std::string> need;
    { std::string cur; for (char c : lower(keyword)) { if (c == ' ') { if (!cur.empty()) need.push_back(cur); cur.clear(); } else cur += c; } if (!cur.empty()) need.push_back(cur); }
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) continue;
        for (const auto& e : fs::directory_iterator(d, ec)) {
            if (!e.is_regular_file()) continue;
            std::string name = lower(e.path().filename().string());
            if (lower(e.path().extension().string()) != ".cpr") continue;
            bool ok = true;
            for (const auto& t : need) if (name.find(t) == std::string::npos) { ok = false; break; }
            if (ok) return e.path().string();
        }
    }
    return "";
}

bool EmuHost::bootModel(int index, int ram, int crtc) {
    if (index < 0 || index >= (int)models.size()) { status = "Invalid model."; return false; }
    ModelProfile& m = models[index];
    if (index != currentModel) { osRomPath.clear(); basicRomPath.clear(); amsdosRomPath.clear(); }  // different model → different firmware

    // A CPC Plus boots from its system cartridge (the localized "Burnin' Rubber"
    // Amstrad Plus System cart) with the ASIC enabled — not loose OS/BASIC ROMs.
    if (m.plus) {
        std::string cartPath = findCart(m.cartKey);
        if (cartPath.empty()) { status = m.label + " needs the Plus system cartridge (Burnin' Rubber .cpr) in " + mediaDir; return false; }
        bool okc = false;
        Bytes data = readFile(cartPath, okc);
        if (!okc || data.empty()) { status = "Could not read system cartridge."; return false; }
        Cartridge cart = parseCartridge(data);
        if (cart.banks.empty()) { status = "Invalid system cartridge."; return false; }
        ramKiB = ram > 0 ? ram : m.defaultRam;
        crtcType = 3;
        GX4000::LoadCartridgeOptions opt;
        opt.plusComputer = m.computer;     // GX4000 = console (no keyboard)
        opt.ram128 = ramKiB >= 128;
        opt.ramKiB = ramKiB;
        emu->loadCartridge(cart, opt);
        emu->hasFdc = m.amsdos;
        applyAudioRate();
        applySettings();
        currentModel = index;
        cartName.clear();
        paused = false;
        status = "Booted " + m.label + "  (" + std::to_string(ramKiB) + "K, " + (m.computer ? "Plus/ASIC" : "GX4000 console") + ")";
        return true;
    }

    bool ok1 = false, ok2 = false, ok3 = true;
    std::string osPath = !osRomPath.empty() ? osRomPath : findRom(m.osKey);
    std::string basicPath = !basicRomPath.empty() ? basicRomPath : findRom(m.basicKey);
    if (osPath.empty() || basicPath.empty()) { status = "ROMs for " + m.label + " not found in " + romDir; return false; }
    Bytes lowerRom = readFile(osPath, ok1);
    Bytes basicRom = readFile(basicPath, ok2);
    Bytes amsdosRom;
    std::string amsPath;
    if (m.amsdos) { amsPath = !amsdosRomPath.empty() ? amsdosRomPath : findRom("amsdos"); if (!amsPath.empty()) amsdosRom = readFile(amsPath, ok3); }
    if (!ok1 || !ok2) { status = "Failed reading ROM files."; return false; }
    osRomName = std::filesystem::path(osPath).filename().string();
    basicRomName = std::filesystem::path(basicPath).filename().string();
    amsdosRomName = amsPath.empty() ? "" : std::filesystem::path(amsPath).filename().string();

    ramKiB = ram > 0 ? ram : m.defaultRam;
    crtcType = crtc >= 0 ? crtc : m.defaultCrtc;

    GX4000::LoadClassicOptions o;
    o.lowerRom = lowerRom;
    o.basicRom = basicRom;
    o.amsdosRom = amsdosRom;
    o.ramKiB = ramKiB;
    o.ram128 = ramKiB >= 128;
    o.crtcType = crtcType;
    o.model = m.label;
    emu->loadClassicFirmware(o);
    emu->hasFdc = m.amsdos;
    applyAudioRate();
    applySettings();
    // applySettings() fitted the M4 ROM (if enabled) AFTER loadClassicFirmware's
    // reset; reset once more so the firmware scans it and registers its RSX.
    if (m4Enabled && !emu->plusHardware) emu->reset();

    // A different machine may have come with a different set (ACCC 15.1: a Plus got a
    // CM14, a CRTC 4 a CTM calibrated like one), so re-take it unless one was chosen.
    applyMonitorSet();
    currentModel = index;
    cartName.clear();
    paused = false;
    status = "Booted " + m.label + "  (" + std::to_string(ramKiB) + "K, CRTC " + std::to_string(crtcType) + ")";
    return true;
}

void EmuHost::applySettings() {
    if (!emu) return;
    if (emu->keyboard) {
        emu->keyboard->setJoystickEnabled(joystickEnabled);
        emu->keyboard->setRegion(keyboardRegion);
    }
    emu->setDacType(dacType);
    applyExpansionRoms();
    applyM4();
    applySymbiface();
    applyAudioRate();
}

void EmuHost::applyM4() {
    if (!m4Enabled || !emu || !emu->m4 || !emu->memory) return;
    if (emu->plusHardware) return;                 // M4 is a classic-CPC device
    std::string romPath = findRom("m4rom");
    if (romPath.empty()) romPath = findRom("m4");
    bool ok = false; Bytes rom = readFile(romPath, ok);
    if (!ok || rom.empty()) return;
    if (!m4Drive) m4Drive = new HostFolderM4Drive(m4Folder);
    emu->m4->setDrive(m4Drive);
    emu->m4->loadRomDefaults(rom);
    emu->memory->setUpperRom(6, rom);
    emu->m4->setEnabled(true);
}

bool EmuHost::setM4(bool enabled, const std::string& folder) {
    if (!emu || !emu->m4 || !emu->memory) return false;
    if (enabled) {
        if (emu->plusHardware) { status = "M4 needs a classic CPC (not Plus/GX4000)."; return false; }
        std::string romPath = findRom("m4rom");
        if (romPath.empty()) romPath = findRom("m4");
        bool ok = false; Bytes rom = readFile(romPath, ok);
        if (!ok || rom.empty()) { status = "M4ROM.ROM not found in " + romDir; return false; }
        delete m4Drive; m4Drive = new HostFolderM4Drive(folder);
        emu->m4->setDrive(m4Drive);
        emu->m4->loadRomDefaults(rom);
        emu->memory->setUpperRom(6, rom);
        emu->m4->setEnabled(true);
        m4Enabled = true; m4Folder = folder;
        emu->reset();   // let the firmware re-scan ROMs and register the M4 RSX (|CD, |DIR…)
        status = "M4 enabled — " + folder + " (" + std::to_string((int)m4Drive->files.size()) + " file(s))";
    } else {
        emu->m4->setEnabled(false);
        emu->m4->setDrive(nullptr);
        delete m4Drive; m4Drive = nullptr;
        emu->memory->setUpperRom(6, Bytes{});
        m4Enabled = false;
        emu->reset();
        status = "M4 disabled";
    }
    return true;
}

void EmuHost::rescanM4() {
    if (m4Drive) { static_cast<HostFolderM4Drive*>(m4Drive)->rescan(); status = "M4 folder rescanned"; }
}

void EmuHost::applySymbiface() {
    if (!emu) return;
    bool sf2 = symbifaceModule == "sf2", sf3 = symbifaceModule == "sf3";
    if (emu->symbifaceMouse) { emu->symbifaceMouse->setEnabled(sf2); emu->symbifaceMouse->setSensitivity(mouseSensitivity); }
    if (emu->sf3) { emu->sf3->setEnabled(sf3); emu->sf3->setSensitivity(mouseSensitivity); }
}

void EmuHost::setSymbiface(const std::string& module) {
    symbifaceModule = module;
    applySymbiface();
    status = "Symbiface: " + (module == "none" ? std::string("off") : module);
}

void EmuHost::setMouseSensitivity(float s) {
    mouseSensitivity = s;
    if (emu && emu->symbifaceMouse) emu->symbifaceMouse->setSensitivity(s);
    if (emu && emu->sf3) emu->sf3->setSensitivity(s);
}

bool EmuHost::symbifaceMouseActive() const {
    if (symbifaceModule == "sf2") return emu && emu->symbifaceMouse && emu->symbifaceMouse->enabled;
    if (symbifaceModule == "sf3") return emu && emu->sf3 && emu->sf3->enabled;
    return false;
}

void EmuHost::mouseMove(float dx, float dy) {
    if (symbifaceModule == "sf2" && emu && emu->symbifaceMouse) emu->symbifaceMouse->move(dx, dy);
    else if (symbifaceModule == "sf3" && emu && emu->sf3) emu->sf3->move(dx, dy);
}
void EmuHost::mouseButton(int index, bool pressed) {
    if (symbifaceModule == "sf2" && emu && emu->symbifaceMouse) emu->symbifaceMouse->button(index, pressed);
    else if (symbifaceModule == "sf3" && emu && emu->sf3) emu->sf3->button(index, pressed);
}
void EmuHost::mouseScroll(float delta) {
    if (symbifaceModule == "sf2" && emu && emu->symbifaceMouse) emu->symbifaceMouse->scroll(delta);
    else if (symbifaceModule == "sf3" && emu && emu->sf3) emu->sf3->scroll(delta);
}

void EmuHost::applyExpansionRoms() {
    if (!emu || !emu->memory) return;
    for (const auto& r : expansionRoms) emu->memory->setUpperRom(r.slot, r.data);
}

bool EmuHost::fitExpansionRom(int slot, const std::string& path) {
    if (slot < 0 || slot > 255) { status = "ROM slot out of range."; return false; }
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read ROM."; return false; }
    std::string name = std::filesystem::path(path).filename().string();
    bool replaced = false;
    for (auto& r : expansionRoms) if (r.slot == slot) { r.name = name; r.data = data; replaced = true; break; }
    if (!replaced) expansionRoms.push_back({ slot, name, data });
    if (emu && emu->memory) emu->memory->setUpperRom(slot, data);
    status = "Fitted " + name + " in ROM slot " + std::to_string(slot);
    return true;
}

void EmuHost::clearExpansionRom(int slot) {
    expansionRoms.erase(std::remove_if(expansionRoms.begin(), expansionRoms.end(),
                        [&](const FittedRom& r) { return r.slot == slot; }), expansionRoms.end());
    if (emu && emu->memory) emu->memory->setUpperRom(slot, Bytes{});
    status = "Cleared ROM slot " + std::to_string(slot);
}

std::string EmuHost::expansionRomName(int slot) const {
    for (const auto& r : expansionRoms) if (r.slot == slot) return r.name;
    return "";
}

void EmuHost::setFirmwareRom(int which, const std::string& path) {
    if (which == 0) osRomPath = path;
    else if (which == 1) basicRomPath = path;
    else if (which == 2) amsdosRomPath = path;
    if (currentModel >= 0) bootModel(currentModel, ramKiB, crtcType);   // apply
}

void EmuHost::clearFirmwareRom(int which) {
    if (which == 0) osRomPath.clear();
    else if (which == 1) basicRomPath.clear();
    else if (which == 2) amsdosRomPath.clear();
    if (currentModel >= 0) bootModel(currentModel, ramKiB, crtcType);
}

void EmuHost::setJoystickEnabled(bool on) {
    joystickEnabled = on;
    if (emu && emu->keyboard) emu->keyboard->setJoystickEnabled(on);
}

void EmuHost::setKeyboardRegion(const std::string& region) {
    keyboardRegion = region;
    if (emu && emu->keyboard) emu->keyboard->setRegion(region);
}

void EmuHost::setDacType(const std::string& type) {
    dacType = type;
    if (emu) emu->setDacType(type);
    applyAudioRate();
}

int EmuHost::readMem(int addr) const {
    if (!emu || !emu->memory) return 0;
    return emu->memory->read(addr & 0xffff) & 0xff;
}

bool EmuHost::loadByExtension(const std::string& path) {
    std::string ext;
    { auto e = std::filesystem::path(path).extension().string(); for (char c : e) ext += (char)std::tolower((unsigned char)c); }
    if (ext == ".cpr") return loadCartridgeFile(path);
    if (ext == ".dsk" || ext == ".edsk") return loadDiskFile(path, 0);
    if (ext == ".cdt" || ext == ".tzx" || ext == ".tap" || ext == ".wav") return loadTapeFile(path);
    if (ext == ".sna") return loadSnapshot(path);
    status = "Unrecognised file type: " + ext;
    return false;
}

bool EmuHost::saveScreenshotBmp(const std::string& path) {
    if (!video) return false;
    const int w = video->width, h = video->height;
    const int rowBytes = w * 4;
    const uint32_t dataSize = (uint32_t)(rowBytes * h);
    const uint32_t fileSize = 54 + dataSize;
    std::ofstream f(path, std::ios::binary);
    if (!f) { status = "Could not write screenshot."; return false; }
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xff)); f.put((char)((v >> 8) & 0xff)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) f.put((char)((v >> (8 * i)) & 0xff)); };
    // BITMAPFILEHEADER
    f.put('B'); f.put('M'); u32(fileSize); u16(0); u16(0); u32(54);
    // BITMAPINFOHEADER (32bpp, top-down via negative height)
    u32(40); u32((uint32_t)w); u32((uint32_t)(-h)); u16(1); u16(32);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    // pixels: BMP 32bpp byte order is B,G,R,A. video->pixels is 0xAABBGGRR.
    std::vector<uint8_t> row((size_t)rowBytes);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = video->pixels[(size_t)y * w + x];
            uint8_t r = (uint8_t)(p & 0xff), g = (uint8_t)((p >> 8) & 0xff), b = (uint8_t)((p >> 16) & 0xff);
            uint8_t* d = &row[(size_t)x * 4];
            d[0] = b; d[1] = g; d[2] = r; d[3] = 0xff;
        }
        f.write((const char*)row.data(), rowBytes);
    }
    status = "Saved screenshot " + std::filesystem::path(path).filename().string();
    return true;
}

bool EmuHost::loadCartridgeFile(const std::string& path) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read cartridge."; return false; }
    Cartridge cart = parseCartridge(data);
    if (cart.banks.empty()) { status = "Not a valid .CPR cartridge."; return false; }
    // Respect the selected machine: a 464/6128 Plus keeps its computer identity
    // (keyboard, RAM, disc); otherwise the cart runs as the selected console, or
    // defaults to the GX4000 when a non-Plus / no model is selected.
    bool haveModel = currentModel >= 0 && models[currentModel].plus;
    if (!haveModel) {
        for (int i = 0; i < (int)models.size(); i++)   // fall back to a GX4000 profile
            if (models[i].id == "gx4000") { currentModel = i; haveModel = true; break; }
    }
    ModelProfile& m = models[currentModel >= 0 ? currentModel : 0];
    GX4000::LoadCartridgeOptions opt;
    opt.plusComputer = m.computer;
    opt.ram128 = m.computer && ramKiB >= 128;
    opt.ramKiB = m.computer ? (ramKiB >= 128 ? 128 : 64) : 64;
    emu->loadCartridge(cart, opt);
    emu->hasFdc = m.computer && m.amsdos;
    applyAudioRate();
    applySettings();
    cartName = std::filesystem::path(path).filename().string();
    paused = false;
    status = "Loaded " + cartName + " into " + m.label;
    return true;
}

bool EmuHost::loadDiskFile(const std::string& path, int unit) {
    if (unit < 0 || unit > 1) return false;
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read disk image."; return false; }
    if (!emu->fdc) { status = "No floppy controller."; return false; }
    auto disk = emu->fdc->mount(data, unit);
    if (!disk) { status = "Not a valid .DSK/.EDSK image."; return false; }
    emu->hasFdc = true;
    diskName[unit] = std::filesystem::path(path).filename().string();
    status = "Inserted " + diskName[unit] + " into drive " + std::string(unit == 0 ? "A" : "B");
    return true;
}

void EmuHost::ejectDisk(int unit) {
    if (unit < 0 || unit > 1 || !emu->fdc) return;
    emu->fdc->eject(unit);
    diskName[unit].clear();
    status = "Ejected drive " + std::string(unit == 0 ? "A" : "B");
}

bool EmuHost::loadTapeFile(const std::string& path) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read tape image."; return false; }
    if (!emu->tape) { status = "No tape drive."; return false; }
    std::string name = std::filesystem::path(path).filename().string();
    if (!emu->tape->load(data, name)) { status = "Not a valid .CDT/.TZX/.WAV tape."; return false; }
    tapeName = name;
    tapePlaying = false;
    status = "Inserted tape " + tapeName + "  (press Play)";
    return true;
}

void EmuHost::tapePlayToggle() {
    if (!emu->tape || tapeName.empty()) return;
    tapePlaying = emu->tape->togglePlay();
    status = tapePlaying ? "Tape playing" : "Tape stopped";
}

void EmuHost::tapeRewind() {
    if (!emu->tape) return;
    emu->tape->rewind();
    status = "Tape rewound";
}

bool EmuHost::saveSnapshot(const std::string& path) {
    if (!booted()) { status = "Nothing to snapshot."; return false; }
    Bytes data = createSna(emu, false);
    std::ofstream f(path, std::ios::binary);
    if (!f) { status = "Could not write snapshot."; return false; }
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    status = "Saved snapshot " + std::filesystem::path(path).filename().string();
    return true;
}

bool EmuHost::loadSnapshot(const std::string& path) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read snapshot."; return false; }
    Snapshot snap = parseSna(data);
    if (snap.ram.empty()) { status = "Not a valid .SNA snapshot."; return false; }
    applySna(emu, snap);
    applyAudioRate();
    paused = false;
    status = "Loaded snapshot " + std::filesystem::path(path).filename().string();
    return true;
}

void EmuHost::reset() {
    if (!booted()) return;
    emu->reset();
    status = "Reset";
}

void EmuHost::stepInstruction() {
    if (!booted()) return;
    emu->memory->watchArmed = watchArmed;
    instructionPc = emu->cpu->pc & 0xffff;
    emu->stepInstruction();
    instructionPc = -1;
    emu->memory->watchArmed = false;
    instructionPc = -1;
    if (emu->debuggerPaused) { emu->debuggerPaused = false; paused = true; }
    render();
}

void EmuHost::runFrame() {
    if (!booted() || paused) { audioOut.clear(); return; }
    emu->memory->watchArmed = watchArmed;
    int elapsedTStates;
    if (!stopAfter && !watchArmed) {
        elapsedTStates = emu->runFrame();     // T-states consumed by this CPC frame
    } else {
        // GX4000::runFrame's loop, with the debugger's question asked after every
        // instruction and each instruction's start kept for the watchpoints. One call is
        // one monitor picture, as there.
        auto picture = [this]() { return emu->plusHardware ? emu->plusMonitorFrame : (long)emu->classicMonitorFrame; };
        const long start = picture();
        if (!emu->plusHardware) emu->videoCaptureRegisters = emu->crtc->registers;
        elapsedTStates = 0;
        while (picture() == start && elapsedTStates < 200000) {
            if (emu->debuggerBreakpointHit()) {
                emu->debuggerPaused = true;
                emu->stepTarget = -1;
                if (emu->onBreakpoint) emu->onBreakpoint();
                break;
            }
            int previousPc = emu->cpu->pc & 0xffff;
            instructionPc = previousPc;
            elapsedTStates += emu->stepInstruction();
            if (emu->debuggerPaused) break;
            if (stopAfter && stopAfter(previousPc)) {
                stopAfter = nullptr;
                emu->debuggerPaused = true;
                char b[64]; std::snprintf(b, sizeof(b), "Stepped out to &%04X", emu->cpu->pc & 0xffff);
                breakReason = b;
                break;
            }
        }
        if (!emu->debuggerPaused && emu->v9990) emu->v9990->endFrame();
    }
    emu->memory->watchArmed = false;
    instructionPc = -1;
    if (emu->debuggerPaused) {
        // One pause, whoever asked for it: the UI's Pause resumes from here.
        emu->debuggerPaused = false;
        paused = true;
        stopAfter = nullptr;
        if (breakReason.empty()) { char b[64]; std::snprintf(b, sizeof(b), "Stopped at &%04X", emu->cpu->pc & 0xffff); breakReason = b; }
        status = breakReason;
    }
    if (elapsedTStates > 0) {
        // CPC display refresh = 4 MHz CPU clock / T-states per VSYNC frame (~50 Hz
        // normally; higher/variable under CRTC rupture). Lightly smoothed.
        double instant = 4000000.0 / elapsedTStates;
        cpcRefreshHz += (instant - cpcRefreshHz) * 0.15;
    }
    drainAudio();
}

void EmuHost::render() {
    if (!video || !booted()) return;
    // The beam renderer covers only the classic monitor path; Plus/ASIC keeps the
    // legacy compositor (sprites etc.). beamMode suppresses render() only when the
    // beam is actually driving the frame.
    video->beamMode = beamRenderer && emu && emu->plusHardware == false;
    if (!video->beamMode) video->render();
}

void EmuHost::drainAudio() {
    audioOut.clear();
    if (!audioEnabled || !emu->ay) return;
    AY38912* ay = emu->ay;
    const double gain = (double)masterVolume * 16000.0;
    int available = (int)ay->sampleQueue.size() - ay->sampleReadIndex;
    if (available <= 0) return;
    audioOut.reserve((size_t)available * 2);
    for (int i = 0; i < available; i++) {
        std::array<double, 2> s = ay->readSample();
        int l = (int)(s[0] * gain);
        int r = (int)(s[1] * gain);
        audioOut.push_back((int16_t)std::clamp(l, -32768, 32767));
        audioOut.push_back((int16_t)std::clamp(r, -32768, 32767));
    }
}

void EmuHost::setKey(SDL_Scancode sc, bool pressed) {
    if (!emu->keyboard) return;
    std::string code = sdlScancodeToBrowserCode(sc);
    if (code.empty()) return;
    emu->keyboard->setKey(code, pressed, "", sdlKeyLocation(sc));
}

void EmuHost::setMonitorMode(const std::string& mode) {
    monitorMode = mode;
    if (video) video->setMonitorMode(mode);
}

// THE SET, not a rendering option. ACCC 15.1 (p.146) makes the horizontal calibration a
// property of the set, and 16.2.2 gives the colour and green families different vertical
// deflection parts -- so this goes to the monitor chip, and the renderer reads the tube
// and the back porch back out of it.
void EmuHost::setMonitorSet(const std::string& id) {
    monitorSetId = id;
    applyMonitorSet();
}

void EmuHost::applyMonitorSet() {
    if (!emu) return;
    const MonitorModel* set = monitorSetId.empty()
        ? emu->shippedMonitorModel()
        : monitorModelFor(monitorSetId);
    emu->setMonitorModel(set);
    if (video) { video->setMonitorModel(set); video->setMonitorMode(monitorMode); }
}

} // namespace cpcse
