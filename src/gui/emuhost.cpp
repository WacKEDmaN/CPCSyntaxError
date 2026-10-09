// CPCSyntaxError GUI — emulator host implementation.
#include "emuhost.h"
#include "../core/monitor_model.h"
#include "../core/monitor_renderer.h"
#include "../core/gate_array.h"
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
#include "core/opl4.h"
#include "core/playcity.h"
#include "core/speech.h"
#include "core/machine_sounds.h"
#include "core/m4.h"
#include "core/symbiface_mouse.h"
#include "core/sf3.h"
#include "core/symbiface_ide.h"
#include "core/sf2_rtc.h"
#include "core/matrix_printer.h"
#include "core/dac.h"
#include "core/ppi.h"
#include "core/keyboard.h"
#include "core/crtc.h"
#include "core/asic.h"
#include "core/tape.h"
#include "core/csl.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace cpcse {

static const int SCREEN_W = 768;
static const int SCREEN_H = 544;


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
    // The printer port's output. The CpcDac lives as long as the machine, so this is wired
    // once: it only delivers bytes while its mode is "printer" or "matrix".
    matrixPrinter = new MatrixPrinter();
    emu->dac->onPrinterChar = [this](const std::string& s) {
        for (char ch : s) { if (ch == 13) continue; printerText.push_back(ch == 10 ? '\n' : ch); }
        printerRevision += 1;
    };
    emu->dac->onMatrixPrinterByte = [this](int byte) { matrixPrinter->writeByte(byte); printerRevision += 1; };
    // A Gunstick reads the brightness of the spot it points at -- off the rendered picture.
    emu->lightgunPixelSampler = [this](int x, int y) -> std::optional<int> {
        if (!video || x < 0 || y < 0 || x >= video->width || y >= video->height) return std::nullopt;
        const uint32_t p = video->pixels[(size_t)y * video->width + x];
        const int rgb = (int)((p & 0xff) << 16 | (p >> 8 & 0xff) << 8 | (p >> 16 & 0xff));
        return video->lightgunRgbBright(rgb) ? 1 : 0;
    };
    scanModels();
}

EmuHost::~EmuHost() {
    delete video;
    delete emu;
    delete matrixPrinter;
}

void EmuHost::applyAudioRate() {
    if (emu && emu->ay) emu->ay->setOutputSampleRate((double)sampleRate);
    if (emu && emu->playcity) emu->playcity->setOutputSampleRate((double)sampleRate);
    if (emu && emu->speech) emu->speech->setOutputSampleRate((double)sampleRate);
    if (emu && emu->dac) emu->dac->setOutputSampleRate((double)sampleRate);
}

Bytes EmuHost::readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    // Nothing the machine takes is near 1 GB (an hour's WAV tape is about 600 MB); a bigger
    // file dropped on the window by mistake would only exhaust memory and end the program.
    if (n > ((std::streamoff)1 << 30)) { ok = false; return {}; }
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
    // A RAM size or CRTC type the core refuses (a hand-edited cpcse.ini, --ram 100) is a
    // message here: the core throws for them, and that would end the program -- at every
    // start, for a value read from the settings.
    if (crtc > 5) { status = "CRTC type " + std::to_string(crtc) + " does not exist (0-4, or 5 for CRTC 1-B)."; return false; }
    if (ram > 0) {
        try { emu->memory->validateRamSize(ram); }
        catch (const std::exception& ex) { status = ex.what(); return false; }
    }
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
        Cartridge cart;
        try { cart = parseCartridge(data); }
        catch (const std::exception& ex) { status = std::string("Invalid system cartridge: ") + ex.what(); return false; }
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
        applyMonitorSet();                 // a Plus came with a CM14 (ACCC 15.1)
        currentModel = index;
        cartName.clear();
        snapshotName.clear();
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
    applyGateArrayPart();                 // loadClassicFirmware fitted the 40010
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
    snapshotName.clear();
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
    applyPrinter();
    applyExpansionRoms();
    applyM4();
    applySymbiface();
    applyLightgun();
    applyTapeOptions();
    if (emu->v9990) emu->v9990->setEnabled(v9990Enabled);
    applyOpl4();
    applyPlayCity();
    applySpeech();
    applyAudioRate();
}

void EmuHost::applyM4Network() {
    if (!emu || !emu->m4) return;
    emu->m4->net->linked = m4Network;
    emu->m4->net->allowLan = m4NetworkLan;
    if (!m4Network) emu->m4->net->closeAll();
}

void EmuHost::applyM4() {
    applyM4Network();
    if (!m4Enabled || !emu || !emu->m4 || !emu->memory) return;
    if (emu->plusHardware) return;                 // M4 is a classic-CPC device
    std::string romPath = findRom("m4rom");
    if (romPath.empty()) romPath = findRom("m4");
    bool ok = false; Bytes rom = readFile(romPath, ok);
    if (!ok || rom.empty()) return;
    if (!emu->m4->storage || emu->m4->storage->root != m4Folder) emu->m4->setCard(m4Folder);
    emu->m4->setRom(rom);
    emu->memory->setUpperRom(emu->m4->romSlot, rom);   // for the debugger's views; the board answers reads
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
        emu->m4->setCard(folder);
        emu->m4->setRom(rom);
        emu->memory->setUpperRom(emu->m4->romSlot, rom);
        emu->m4->setEnabled(true);
        applyM4Network();
        m4Enabled = true; m4Folder = folder;
        emu->reset();   // let the firmware re-scan ROMs and register the M4 RSX (|CD, |DIR…)
        status = "M4 enabled — " + folder;
    } else {
        emu->m4->setEnabled(false);
        emu->m4->setCard("");
        emu->memory->setUpperRom(emu->m4->romSlot, Bytes{});
        m4Enabled = false;
        emu->reset();
        status = "M4 disabled";
    }
    return true;
}

void EmuHost::rescanM4() {
    if (emu && emu->m4 && emu->m4->storage) { emu->m4->rescan(); status = "M4 folder rescanned"; }
}

void EmuHost::applySymbiface() {
    if (!emu) return;
    bool sf2 = symbifaceModule == "sf2", sf3 = symbifaceModule == "sf3";
    if (emu->symbifaceMouse) { emu->symbifaceMouse->setEnabled(sf2); emu->symbifaceMouse->setSensitivity(mouseSensitivity); }
    if (emu->sf2Rtc) emu->sf2Rtc->setEnabled(sf2);       // its DS12887 clock at &FD14/&FD15
    if (emu->sf3) { emu->sf3->setEnabled(sf3); emu->sf3->setSensitivity(mouseSensitivity); }
    if (emu->ide) {
        // Both cards carry the IDE/CF interface at &FD06-&FD0F.
        if ((sf2 || sf3) && !ideFolder.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path p = fs::u8path(ideFolder);
            if (!fs::exists(p, ec)) fs::create_directories(p, ec);
            emu->ide->setFolder(ideFolder);
            emu->ide->setEnabled(true);
        } else emu->ide->setEnabled(false);
    }
}

void EmuHost::setIdeFolder(const std::string& folder) {
    ideFolder = folder;
    applySymbiface();
    status = "Symbiface IDE: " + folder;
}

void EmuHost::rescanIde() {
    if (emu && emu->ide && emu->ide->enabled) { emu->ide->rescan(); status = "IDE folder rescanned"; }
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
    applyPrinter();
    applyAudioRate();
}
void EmuHost::applyPrinter() {
    if (!emu) return;
    emu->setDacType(dacType);
    // A printer that is plugged in and switched on answers BUSY low; with nothing there the
    // firmware sees BUSY stuck high and waits (PPI port B bit 6).
    if (emu->ppi) emu->ppi->printerOnline = printerAttached();
}
void EmuHost::clearPrinter() {
    printerText.clear();
    if (matrixPrinter) matrixPrinter->reset();
    printerRevision += 1;
}
bool EmuHost::savePrinterText(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << printerText;
    f.close();   // a full disc shows at the close, where the last of it is written
    return (bool)f;
}
bool EmuHost::savePrinterPageBmp(const std::string& path) const {
    if (!matrixPrinter || matrixPrinter->pageCount() == 0) return false;
    std::vector<uint8_t>& page = matrixPrinter->pageData(matrixPrinter->pageNumber() - 1);
    std::vector<uint32_t> px((size_t)matrixPrinter->width * matrixPrinter->height);
    for (size_t i = 0; i < px.size(); i += 1)
        px[i] = 0xff000000u | (uint32_t)page[i * 4 + 2] << 16 | (uint32_t)page[i * 4 + 1] << 8 | page[i * 4];
    return writeVideoBmp(path, px, matrixPrinter->width, matrixPrinter->height);
}
bool EmuHost::savePrinterPageSvg(const std::string& path) const {
    if (!matrixPrinter || matrixPrinter->pageCount() == 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << matrixPrinter->toSvg();
    f.close();
    return (bool)f;
}

void EmuHost::setLightgun(const std::string& type) {
    lightgunType = (type == "trojan" || type == "gunstick" || type == "westphaser") ? type : "none";
    applyLightgun();
}
void EmuHost::applyLightgun() {
    if (!emu || !emu->keyboard || !emu->crtc) return;
    const bool on = lightgunActive();
    if (on) { emu->keyboard->setLightgunType(lightgunType); emu->crtc->setLightgunType(lightgunType); }
    emu->keyboard->setLightgunEnabled(on);
    emu->crtc->setTrojanLightgunEnabled(on);
}
void EmuHost::lightgunAim(double x, double y, bool trigger) {
    if (!emu || !lightgunActive()) return;
    if (x < 0 || y < 0) { emu->crtc->releaseTrojanLightgun(); emu->keyboard->setLightgunTrigger(false); return; }
    emu->crtc->setTrojanLightgun(x, y, trigger, SCREEN_W, SCREEN_H);
    emu->keyboard->setLightgunTrigger(trigger);
}

void EmuHost::setOpl4(bool on) {
    opl4Enabled = on;
    applyOpl4();
}
void EmuHost::setPlayCity(bool on) {
    playCityEnabled = on;
    applyPlayCity();
}
void EmuHost::setSpeech(const std::string& kind) {
    speechKind = kind;
    applySpeech();
}
// The SP0256-AL2's internal ROM is General Instrument's and is not shipped: it is read
// from the ROM folder (any name with sp0256 and al2 in it), 2 KB.
void EmuHost::applySpeech() {
    if (!emu || !emu->speech) return;
    SpeechSynth& s = *emu->speech;
    s.setKind(speechKind == "ssa1" ? SpeechSynth::Kind::Ssa1
              : speechKind == "dktronics" ? SpeechSynth::Kind::DkTronics
              : speechKind == "lambdaspeak3" ? SpeechSynth::Kind::LambdaSpeak3 : SpeechSynth::Kind::None);
    s.mp3.card = mp3Card;
    if (s.kind != SpeechSynth::Kind::None && !s.hasRom) {
        const std::string path = findRom("sp0256 al2");
        bool ok = false;
        Bytes rom;
        if (!path.empty()) rom = readFile(path, ok);
        if (ok) s.loadRom(rom);
    }
    s.setOutputSampleRate((double)sampleRate);
}
void EmuHost::applyPlayCity() {
    if (!emu || !emu->playcity) return;
    if (emu->playcity->enabled != playCityEnabled) emu->playcity->reset();
    emu->playcity->enabled = playCityEnabled;
    emu->playcity->setOutputSampleRate((double)sampleRate);
}
void EmuHost::applyOpl4() {
    if (!emu || !emu->opl4) return;
    Opl4Card& card = *emu->opl4;
    card.setRamKiB(opl4RamKiB);
    if (opl4Enabled && card.rom.empty()) {
        const std::string path = findRom("yrw801");
        bool ok = false;
        if (!path.empty()) card.rom = readFile(path, ok);
        if (!ok) card.rom.clear();
    }
    card.setEnabled(opl4Enabled);
}
bool EmuHost::installRom(const std::string& source, const std::string& destName, size_t size, std::string& why) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto have = fs::file_size(source, ec);
    if (ec) { why = "cannot read " + source; return false; }
    if (have != size) {
        why = "it is " + std::to_string(have) + " bytes, not " + std::to_string(size) +
              (have < size && have > 0 ? " (a zip? unpack it, then choose the ROM itself)" : "");
        return false;
    }
    fs::create_directories(romDir, ec);
    fs::copy_file(source, fs::path(romDir) / destName, fs::copy_options::overwrite_existing, ec);
    if (ec) { why = "cannot write to " + romDir + ": " + ec.message(); return false; }
    return true;
}
bool EmuHost::opl4HasRom() const { return emu && emu->opl4 && !emu->opl4->rom.empty(); }

void EmuHost::setV9990(bool on) {
    v9990Enabled = on;
    if (emu && emu->v9990) emu->v9990->setEnabled(on);
}
bool EmuHost::gfx9000OnMainScreen() const {
    if (!v9990Enabled || gfx9000Monitor != "switch" || !emu || !emu->v9990 || !emu->v9990->displaying()) return false;
    const V9990& v = *emu->v9990;
    if (!(v.registers[8] & 0x80)) return false;                       // DISP off: nothing to show
    return !(v.outputControlWritten && (v.outputControl & 0x10));
}
int EmuHost::video9000Control() const {
    if (!emu || !emu->v9990) return 0x10;
    return emu->v9990->outputControlWritten ? emu->v9990->outputControl : 0x10;
}

bool EmuHost::video9000Picture(std::vector<uint32_t>& out, int& width, int& height) const {
    if (!emu || !emu->v9990 || !video) return false;
    const int ctl = video9000Control();
    const bool gen = ctl & 0x10, tran = ctl & 0x08, ymix = ctl & 0x02, ym = ctl & 0x01;
    const bool rgbInput = !(ctl & 0x40);                  // S1 = 0: the computer's RGB
    const V9990Picture* pic = v9990Picture();
    V9990Picture blank;                                    // the GFX9000 showing nothing: black
    if (!pic) { blank.width = 568; blank.height = 290; blank.pixels.assign(568 * 290, 0xff000000u); blank.ys.assign(568 * 290, 0); pic = &blank; }
    width = pic->width; height = pic->height;
    out.resize((size_t)width * height);
    if (!gen) { out = pic->pixels; return true; }
    // The input, lined up on the sync the genlock shares: the CPC's picture is 16 texture
    // pixels a microsecond and two rows a line; both are centred on the line and the field.
    const int cw = video->width, ch = video->height;
    const double gfxMidUs = pic->startUs + pic->spanUs * 0.5;
    const int rowsPerLine = std::max(1, height / std::max(1, pic->lines));
    auto input = [&](int x, int y) -> uint32_t {
        if (!rgbInput) return 0xff000000u;                // CVBS / S-VHS: nothing plugged in
        const double us = pic->startUs + (x + 0.5) * pic->spanUs / width;
        const int cx = (int)std::floor(cw * 0.5 + (us - gfxMidUs) * 16.0);
        const double line = (double)y / rowsPerLine;
        const int cy = (int)std::floor(ch * 0.5 + (line - pic->lines * 0.5) * 2.0);
        if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) return 0xff000000u;
        return video->pixels[(size_t)cy * cw + cx] | 0xff000000u;
    };
    auto half = [](uint32_t c) { return 0xff000000u | ((c >> 1) & 0x007f7f7fu); };
    auto mix = [](uint32_t a, uint32_t b) {
        return 0xff000000u | (((a & 0x00fefefeu) >> 1) + ((b & 0x00fefefeu) >> 1));
    };
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const size_t i = (size_t)y * width + x;
            const uint32_t ext = input(x, y);
            if (!tran || pic->ys[i]) out[i] = ym ? half(ext) : ext;
            else out[i] = ymix ? mix(pic->pixels[i], ext) : pic->pixels[i];
        }
    return true;
}

const V9990Picture* EmuHost::v9990Picture() const {
    if (!emu || !emu->v9990 || !v9990Enabled || !emu->v9990->displaying()) return nullptr;
    const V9990Picture& p = emu->v9990->picture();
    return p.width > 0 && p.height > 0 ? &p : nullptr;
}

void EmuHost::applyTapeOptions() {
    if (!emu || !emu->tape) return;
    emu->tape->requireMotor = tapeFollowsMotor;
    emu->tape->tapeRelayDelay = tapeRelayDelay;
}

int EmuHost::analogue(int channel) const {
    if (!emu || !emu->asic || channel < 0 || channel > 7) return 0;
    return emu->asic->analogueInput[channel] & 0x3f;
}
void EmuHost::setAnalogue(int channel, int value) {
    if (!emu || !emu->asic || channel < 0 || channel > 7) return;
    emu->asic->analogueInput[channel] = (uint8_t)std::clamp(value, 0, 63);
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
    f.close();
    if (!f) { status = "Could not write screenshot (disc full?)"; return false; }
    status = "Saved screenshot " + std::filesystem::path(path).filename().string();
    return true;
}

bool EmuHost::loadCartridgeFile(const std::string& path) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read cartridge."; return false; }
    // The readers throw for a file they cannot take: a bad file is a message, not the end
    // of the program.
    Cartridge cart;
    try { cart = parseCartridge(data); }
    catch (const std::exception& ex) { status = std::string("Not a valid .CPR cartridge: ") + ex.what(); return false; }
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
    applyMonitorSet();
    cartName = std::filesystem::path(path).filename().string();
    snapshotName.clear();
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
    std::shared_ptr<Disk> disk;
    try { disk = emu->fdc->mount(data, unit); }
    catch (const std::exception& ex) { status = std::string("Not a valid .DSK/.EDSK image: ") + ex.what(); return false; }
    if (!disk) { status = "Not a valid .DSK/.EDSK image."; return false; }
    emu->hasFdc = true;
    diskName[unit] = std::filesystem::path(path).filename().string();
    diskPath[unit] = path;
    status = "Inserted " + diskName[unit] + " into drive " + std::string(unit == 0 ? "A" : "B");
    return true;
}

bool EmuHost::insertDisk(std::shared_ptr<Disk> disk, int unit, const std::string& name) {
    if (unit < 0 || unit > 1 || !emu || !emu->fdc || !disk) return false;
    emu->fdc->insert(disk, unit);
    emu->hasFdc = true;
    diskName[unit] = name;
    status = "Inserted " + name + " into drive " + std::string(unit == 0 ? "A" : "B");
    return true;
}

std::shared_ptr<Disk> EmuHost::driveDisk(int unit) const {
    if (unit < 0 || unit > 1 || !emu || !emu->fdc) return nullptr;
    return emu->fdc->drives[(size_t)unit];
}

void EmuHost::ejectDisk(int unit) {
    if (unit < 0 || unit > 1 || !emu->fdc) return;
    emu->fdc->eject(unit);
    diskName[unit].clear();
    diskPath[unit].clear();
    status = "Ejected drive " + std::string(unit == 0 ? "A" : "B");
}

bool EmuHost::loadTapeFile(const std::string& path) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read tape image."; return false; }
    if (!emu->tape) { status = "No tape drive."; return false; }
    std::string name = std::filesystem::path(path).filename().string();
    try {
        if (!emu->tape->load(data, name)) { status = "Not a valid .CDT/.TZX tape."; return false; }
    } catch (const std::exception& ex) { status = std::string("Not a valid .CDT/.TZX tape: ") + ex.what(); return false; }
    tapeName = name;
    status = "Inserted tape " + tapeName + "  (press Play)";
    return true;
}

CPCTapeDrive* EmuHost::tapeDeck() const { return emu && emu->tape && !tapeName.empty() && emu->tape->loaded ? emu->tape : nullptr; }
bool EmuHost::tapePlaying() const { CPCTapeDrive* d = tapeDeck(); return d && d->playing && !d->paused; }
bool EmuHost::tapePaused() const { CPCTapeDrive* d = tapeDeck(); return d && d->playing && d->paused; }


void EmuHost::tapePlay() {
    if (CPCTapeDrive* d = tapeDeck()) { d->play(); status = d->playing ? "Tape playing" : "The tape is at its end: rewind it"; }
}

void EmuHost::tapePause() {
    CPCTapeDrive* d = tapeDeck();
    if (!d || !d->playing) return;
    d->setPaused(!d->paused);
    status = d->paused ? "Tape paused" : "Tape playing";
}

void EmuHost::tapeStop() {
    if (CPCTapeDrive* d = tapeDeck()) { d->stop(); status = "Tape stopped"; }
}

void EmuHost::tapeRewind() {
    if (CPCTapeDrive* d = tapeDeck()) { d->rewind(); status = "Tape rewound"; }
}

void EmuHost::tapeRewindBlock() {
    if (CPCTapeDrive* d = tapeDeck()) { d->rewindBlock(); status = "Tape at block " + std::to_string(d->blockAtPosition() + 1); }
}

void EmuHost::tapeFastForward() {
    if (CPCTapeDrive* d = tapeDeck()) {
        d->fastForward();
        status = d->tapeEnded ? std::string("Tape at its end") : "Tape at block " + std::to_string(d->blockAtPosition() + 1);
    }
}

void EmuHost::tapeSeekBlock(int index) {
    if (CPCTapeDrive* d = tapeDeck()) { d->seekBlock(index); status = "Tape at block " + std::to_string(d->blockAtPosition() + 1); }
}

void EmuHost::tapeEject() {
    if (!emu || !emu->tape) return;
    emu->tape->eject();
    status = tapeName.empty() ? "No tape" : "Ejected " + tapeName;
    tapeName.clear();
}

void EmuHost::tapeResetCounter() {
    if (CPCTapeDrive* d = tapeDeck()) d->resetCounter();
}

bool EmuHost::saveSnapshot(const std::string& path) {
    if (!booted()) { status = "Nothing to snapshot."; return false; }
    Bytes data = createSna(emu, false);
    std::ofstream f(path, std::ios::binary);
    if (!f) { status = "Could not write snapshot."; return false; }
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    f.close();
    if (!f) { status = "Could not write snapshot (disc full?)"; return false; }
    status = "Saved snapshot " + std::filesystem::path(path).filename().string();
    return true;
}

bool EmuHost::loadSnapshot(const std::string& path) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read snapshot."; return false; }
    Snapshot snap;
    try { snap = parseSna(data); }
    catch (const std::exception& ex) { status = std::string("Not a valid .SNA snapshot: ") + ex.what(); return false; }
    if (snap.ram.empty()) { status = "Not a valid .SNA snapshot."; return false; }
    applySna(emu, snap);
    applyAudioRate();
    paused = false;
    snapshotName = std::filesystem::path(path).filename().string();
    status = "Loaded snapshot " + snapshotName;
    return true;
}

bool EmuHost::ejectCartridge() {
    if (cartName.empty() || currentModel < 0) return false;
    const std::string name = cartName;
    if (!bootModel(currentModel, ramKiB, crtcType)) return false;
    status = "Ejected " + name + "; " + models[(size_t)currentModel].label + " booted without it";
    return true;
}

bool EmuHost::ejectSnapshot() {
    if (snapshotName.empty() || currentModel < 0) return false;
    const std::string name = snapshotName;
    if (!bootModel(currentModel, ramKiB, crtcType)) return false;
    status = "Let go of " + name + "; " + models[(size_t)currentModel].label + " booted afresh";
    return true;
}

void EmuHost::reset() {
    if (!booted()) return;
    emu->reset();
    status = "Reset";
}

void EmuHost::stepInstruction(bool redraw) {
    if (!booted()) return;
    emu->memory->watchArmed = watchArmed;
    instructionPc = emu->cpu->pc & 0xffff;
    emu->stepInstruction();
    instructionPc = -1;
    emu->memory->watchArmed = false;
    instructionPc = -1;
    if (emu->debuggerPaused) { emu->debuggerPaused = false; paused = true; }
    if (redraw) render();
}

void EmuHost::runFrame() {
    if (!booted() || paused) { audioOut.clear(); return; }
    framesRun++;
    emu->memory->watchArmed = watchArmed;
    int elapsedTStates;
    if (!stopAfter && !watchArmed) {
        elapsedTStates = emu->runFrame();     // T-states consumed by this CPC frame
    } else {
        // GX4000::runFrame's loop, with the debugger's question asked after every
        // instruction and each instruction's start kept for the watchpoints. One call is
        // one monitor picture, as there.
        auto picture = [this]() { return emu->classicMonitorFrame; };
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
    // The OPL4 card's 44.1 kHz, as the same stretch of CPC time made it, laid over the
    // frame's samples (stretched to them, so the two never drift apart).
    Opl4Card* opl = emu->opl4 && emu->opl4->enabled ? emu->opl4 : nullptr;
    size_t oplPairs = 0;
    if (opl) { opl->advanceTo(emu->machineCycles); oplPairs = opl->samples.size() / 2; }
    const double oplGain = (double)masterVolume * 0.75;
    audioOut.reserve((size_t)available * 2);
    // The PlayCity's two YMZ294: the left chip on the left, the right on the right, each
    // folded to mono, made by the same stretch of CPC time as the AY's samples.
    PlayCity* pc = emu->playcity && emu->playcity->enabled ? emu->playcity : nullptr;
    const bool dacOn = emu->dac && (emu->dac->mode == "digiblaster" || emu->dac->mode == "amdrum");
    // The speech synthesiser, mono, from its own host-rate queue.
    SpeechSynth* sp = emu->speech && emu->speech->kind != SpeechSynth::Kind::None ? emu->speech : nullptr;
    for (int i = 0; i < available; i++) {
        std::array<double, 2> s = ay->readSample();
        double l = s[0] * gain, r = s[1] * gain;
        // The printer-port DACs (DigiBlaster, AmDrum): mono, from their own queue. They were
        // never mixed in -- the DAC filled its queue and nothing read it, so both were silent.
        if (dacOn) { const double v = emu->dac->readSample() * gain * 1.6; l += v; r += v; }
        if (sp) {
            const double v = sp->readSample() * gain * 0.9; l += v; r += v;
            // LambdaSpeak 3's MP3 module, stereo, at line level next to the AY.
            if (sp->kind == SpeechSynth::Kind::LambdaSpeak3) {
                float ml, mr; sp->mp3.readSample(ml, mr);
                l += ml * masterVolume * 26000.0; r += mr * masterVolume * 26000.0;
            }
        }
        if (pc) {
            const bool haveL = pc->left.sampleReadIndex < (int)pc->left.sampleQueue.size();
            const bool haveR = pc->right.sampleReadIndex < (int)pc->right.sampleQueue.size();
            if (haveL) { auto v = pc->left.readSample(); l += (v[0] + v[1]) * 0.5 * gain; }
            if (haveR) { auto v = pc->right.readSample(); r += (v[0] + v[1]) * 0.5 * gain; }
        }
        if (oplPairs) {
            const size_t k = std::min(oplPairs - 1, (size_t)((double)i * oplPairs / available));
            l += opl->samples[k * 2] * oplGain;
            r += opl->samples[k * 2 + 1] * oplGain;
        }
        // clamped as doubles before the cast: (int) of a NaN or of 1e12 is undefined
        auto s16 = [](double v) { return v == v ? (int16_t)std::clamp(v, -32768.0, 32767.0) : (int16_t)0; };
        audioOut.push_back(s16(l));
        audioOut.push_back(s16(r));
    }
    if (opl) opl->samples.clear();
    // The drive and the keys, at the device's rate, one sample per output sample.
    machineSounds.sampleRate = sampleRate;
    machineSounds.driveEnabled = driveSounds;
    machineSounds.keysEnabled = keySounds;
    if (emu->fdc) machineSounds.motor(emu->fdc->motor);   // follows the line, whenever it was switched
    if (!machineSounds.silent()) {
        const double mechGain = (double)masterVolume * mechanicsVolume * 32000.0;   // the recordings are full scale
        for (size_t i = 0; i + 1 < audioOut.size(); i += 2) {
            const int m = (int)(machineSounds.next() * mechGain);
            audioOut[i] = (int16_t)std::clamp((int)audioOut[i] + m, -32768, 32767);
            audioOut[i + 1] = (int16_t)std::clamp((int)audioOut[i + 1] + m, -32768, 32767);
        }
    }
}

void EmuHost::setKey(SDL_Scancode sc, bool pressed) {
    if (!emu->keyboard) return;
    std::string code = sdlScancodeToBrowserCode(sc);
    if (code.empty()) return;
    emu->keyboard->setKey(code, pressed, "", sdlKeyLocation(sc));
    // One click a press and one a release, however long the host's key repeat runs.
    const int idx = (int)sc;
    if (idx >= 0 && idx < (int)keyHeld.size() && keyHeld[idx] != pressed) {
        keyHeld[idx] = pressed;
        const MachineSounds::Key which = sc == SDL_SCANCODE_SPACE ? MachineSounds::Key::Space
            : (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) ? MachineSounds::Key::Return
            : MachineSounds::Key::Normal;
        machineSounds.key(pressed, which);
    }
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
    // No set chosen is nullptr, not the shipped set: the core then re-takes the machine's
    // own at every reset, so a later model change gets its own monitor.
    emu->setMonitorModel(monitorSetId.empty() ? nullptr : monitorModelFor(monitorSetId));
    if (video) { video->setMonitorModel(emu->monitorRenderer->model); video->setMonitorMode(monitorMode); }
}

} // namespace cpcse

namespace cpcse {
void EmuHost::setGateArrayPart(int part) {
    gateArrayPart = (part == 40007 || part == 40008 || part == 40010) ? part : 0;
    applyGateArrayPart();
}
void EmuHost::applyGateArrayPart() {
    if (!emu || !emu->gateArray) return;
    if (crtcType == 3 || crtcType == 4 || emu->plusHardware) return;   // the ASIC is the GATE ARRAY
    emu->gateArray->model = gateArrayPart == 40007 ? gateArrayModel40007()
                          : gateArrayPart == 40008 ? gateArrayModel40008()
                          : gateArrayModel40010();
}
int EmuHost::fittedGateArrayPart() const {
    if (!emu || !emu->gateArray || !emu->gateArray->model) return 40010;
    return std::atoi(emu->gateArray->model->name());
}
} // namespace cpcse
