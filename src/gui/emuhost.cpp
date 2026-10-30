// CPCSyntaxError GUI — emulator host implementation.
#include "emuhost.h"
#include "../core/monitor_model.h"
#include "../core/monitor_renderer.h"
#include "../core/gate_array.h"
#include "keymap.h"
#include "cpc_typing.h"
#include "core/basic.h"
#include "core/emulator.h"
#include "core/video.h"
#include "core/ay.h"
#include "core/keyboard.h"
#include "core/tape.h"
#include "core/fdc.h"
#include "core/dsk.h"
#include "core/hfe.h"
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
#include "core/multiface.h"
#include "core/rs232.h"
#include "core/host_serial.h"
#include "core/sf2_rtc.h"
#include "core/matrix_printer.h"
#include "core/dac.h"
#include "core/ppi.h"
#include "core/keyboard.h"
#include "core/crtc.h"
#include "core/asic.h"
#include "core/tape.h"
#include "core/csl.h"
#include "gui_widgets.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

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
    // the timeline: every input to the keyboard matrix logged; a replay's inputs fed back
    emu->keyboard->onInput = [this](const InputEvent& e) { timelineLog(e); };
    emu->onInputDue = [this]() { timelineApplyDue(); };
    if (emu->fdc) emu->fdc->onDiskModified = [this](std::shared_ptr<Disk> disk, int drive) {
        if (drive >= 0 && drive < 2 && disk && disk == emu->fdc->drives[(size_t)drive]) { diskDirty[drive] = true; diskLastWrite[drive] = framesRun; }
    };
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
    flushDisks();
    flushTape();
    delete video;
    delete emu;
    delete matrixPrinter;
}

void EmuHost::applyAudioRate() {
    if (emu && emu->ay) emu->ay->setOutputSampleRate((double)sampleRate);
    if (emu && emu->playcity) emu->playcity->setOutputSampleRate((double)sampleRate);
    if (emu && emu->speech) emu->speech->setOutputSampleRate((double)sampleRate);
    if (emu && emu->dac) emu->dac->setOutputSampleRate((double)sampleRate);
    if (emu && emu->sf3) emu->sf3->audio->setOutputSampleRate((double)sampleRate);
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
        // The 664: disc drive and AMSDOS, 64K, its own OS and BASIC ROMs. Its board takes a
        // 40010 only (ACCC 1.11 p.46), the Gate Array fitted by default.
        { "cpc664",     "Amstrad CPC 664",   "664 os !+ !amsdos", "664 basic !+", true, 64, 1, false, "" },   // "os": not "Amsdos"
        { "cpc6128",    "Amstrad CPC 6128",  "6128 os !+", "6128 basic !+",true,  128, 1, false, "" },
        // A Plus computer starts from its OS and BASIC ROMs (the system cartridge's pages 0
        // and 1, dumped), or from the system cartridge when those are not in the ROM folder.
        { "cpc464plus", "Amstrad 464 Plus",  "6128+ os",   "6128+ basic",  false, 64,  3, true,  "burning rubber" },
        { "cpc6128plus","Amstrad 6128 Plus", "6128+ os",   "6128+ basic",  true,  128, 3, true,  "burning rubber" },
        // GX4000: a 464 Plus with no keyboard/tape and dual joystick input (all
        // other Plus/ASIC features). computer=false → no keyboard, console mode.
        { "gx4000",     "Amstrad GX4000",    "",           "",             false, 64,  3, true,  "burning rubber", false, false },
    };
    for (auto& m : models) {
        const bool roms = !m.osKey.empty() && !findRom(m.osKey).empty() && !findRom(m.basicKey).empty();
        if (m.plus) m.available = roms || !findCart(m.cartKey).empty();
        else        m.available = roms;
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
    flushDisks();
    timelineEpoch += 1;                   // a new machine: the timeline starts again
    if (!croBoot) { croSet = CroRomSet{}; croGroupLabels.clear(); croGroupNumbers.clear(); }
    ModelProfile& m = models[index];
    if (index != currentModel) { osRomPath.clear(); basicRomPath.clear(); amsdosRomPath.clear(); }  // different model → different firmware

    // A CPC Plus starts from a cartridge with the ASIC enabled. A Plus computer's is built from
    // its OS and BASIC ROMs when the ROM folder has them -- the system cartridge's own layout:
    // page 0 the OS, 1 BASIC, 3 AMSDOS (on the 6128 Plus) -- else it is the system
    // cartridge (the localized "Burnin' Rubber" cart), as the GX4000's always is.
    if (m.plus) {
        std::string plusAmsdosNote;          // where a 6128 Plus's AMSDOS came from, when not its own file
        Cartridge cart;
        const std::string plusOs = m.computer && !m.osKey.empty() ? findRom(m.osKey) : "";
        const std::string plusBasic = m.computer && !m.basicKey.empty() ? findRom(m.basicKey) : "";
        if (!plusOs.empty() && !plusBasic.empty()) {
            bool a = false, b = false, c = true;
            cart.format = "ROMs";
            cart.banks.assign(4, Bytes{});
            cart.banks[0] = readFile(plusOs, a);
            cart.banks[1] = readFile(plusBasic, b);
            // The 6128 Plus's AMSDOS is 0.7 (its own |GAME, |TAPE...), page 3 of the system
            // cartridge; the 6128's AMSDOS 0.5 stops the Plus OS after its banner. So: a Plus
            // AMSDOS file in the ROM folder, else that page read from the system cartridge (the
            // cartridge is not started), else 0.5.
            if (m.amsdos) {
                // its file: named for AMSDOS, or the common dump "464+ Plus A2 OS", which is in
                // fact AMSDOS 0.7 (header 01 00 07 00, the system cartridge's page 3 byte for
                // byte). Taken only when its header says a background ROM.
                std::string ams = findRom("6128+ amsdos");
                if (ams.empty()) ams = findRom("plus a2");
                if (!ams.empty()) {
                    Bytes rom = readFile(ams, c);
                    if (c && rom.size() == 0x4000 && rom[0] == 0x01) cart.banks[3] = rom;
                    else c = true;                   // not AMSDOS: look further
                }
                if (cart.banks[3].empty()) {
                    const std::string systemCart = findCart(m.cartKey);
                    bool okc = false;
                    const Bytes data = systemCart.empty() ? Bytes{} : readFile(systemCart, okc);
                    try {
                        const Cartridge sys = okc ? parseCartridge(data) : Cartridge{};
                        if (sys.banks.size() > 3 && sys.banks[3].size() == 0x4000 && sys.banks[3][0] == 0x01) {   // a background ROM: AMSDOS
                            cart.banks[3] = sys.banks[3];
                            plusAmsdosNote = " (AMSDOS 0.7 from the system cartridge)";
                        }
                    } catch (const std::exception&) {}
                    if (cart.banks[3].empty()) {
                        ams = findRom("amsdos");
                        if (!ams.empty()) cart.banks[3] = readFile(ams, c);
                        plusAmsdosNote = " (AMSDOS 0.5: the 6128 Plus may stop at its banner; add its AMSDOS 0.7)";
                    }
                }
            }
            if (!a || !b || !c || cart.banks[0].size() != 0x4000 || cart.banks[1].size() != 0x4000) { status = "Could not read the Plus ROMs."; return false; }
            cart.bankCount = 4;
        } else {
            const std::string systemCart = findCart(m.cartKey);
            if (systemCart.empty()) { status = m.label + " needs its OS and BASIC ROMs in " + romDir + ", or the Plus system cartridge (Burnin' Rubber .cpr) in " + mediaDir; return false; }
            bool okc = false;
            Bytes data = readFile(systemCart, okc);
            if (!okc || data.empty()) { status = "Could not read system cartridge."; return false; }
            try { cart = parseCartridge(data); }
            catch (const std::exception& ex) { status = std::string("Invalid system cartridge: ") + ex.what(); return false; }
            if (cart.banks.empty()) { status = "Invalid system cartridge."; return false; }
        }
        // A GX4000 has 64K; with the 128K RAM mod (gx4000Ram128) it banks like a 6128.
        ramKiB = !m.computer ? (gx4000Ram128 ? 128 : 64) : ram > 0 ? ram : m.defaultRam;
        crtcType = 3;
        GX4000::LoadCartridgeOptions opt;
        opt.plusComputer = m.computer;     // GX4000 = console (no keyboard)
        opt.ram128 = ramKiB >= 128;
        opt.ramKiB = ramKiB;
        opt.internalRamKiB = m.computer ? m.defaultRam : (gx4000Ram128 ? 128 : 64);   // the machine's own
        opt.classicExpansion = plusExpansionClassic;
        emu->loadCartridge(cart, opt);
        emu->hasFdc = m.amsdos;
        applyAudioRate();
        applySettings();
        applyMonitorSet();                 // a Plus came with a CM14 (ACCC 15.1)
        currentModel = index;
        cartName.clear(); cartPath.clear();
        snapshotName.clear();
        paused = false;
        status = "Booted " + m.label + "  (" + std::to_string(ramKiB) + "K, " + (m.computer ? "Plus/ASIC" : "GX4000 console") + ")" + plusAmsdosNote;
        return true;
    }

    bool ok1 = false, ok2 = false, ok3 = true;
    std::string osPath = !osRomPath.empty() ? osRomPath : findRom(m.osKey);
    std::string basicPath = !basicRomPath.empty() ? basicRomPath : findRom(m.basicKey);
    if (osPath.empty() || basicPath.empty()) { status = "ROMs for " + m.label + " not found in " + romDir; return false; }
    Bytes lowerRom = readFile(osPath, ok1);
    if (!croSet.lower.empty()) lowerRom = croSet.lower;   // a CRO's ROM_LOW is the OS
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
    if (m4Enabled && (!emu->plusHardware || emu->plusComputer)) emu->reset();

    // A different machine may have come with a different set (ACCC 15.1: a Plus got a
    // CM14, a CRTC 4 a CTM calibrated like one), so re-take it unless one was chosen.
    applyMonitorSet();
    currentModel = index;
    cartName.clear(); cartPath.clear();
    snapshotName.clear();
    paused = false;
    status = "Booted " + m.label + "  (" + std::to_string(ramKiB) + "K, CRTC " + std::to_string(crtcType) + ")";
    return true;
}

void EmuHost::applySettings() {
    if (!emu) return;
    if (emu->cpu) emu->cpu->cmos = z80Cmos;
    if (emu->keyboard) {
        emu->keyboard->setJoystickEnabled(joystickEnabled);
        emu->keyboard->setRegion(keyboardRegion);
    }
    applyPrinter();
    applyExpansionRoms();
    applyM4();
    applyMultiface();
    applySerial();
    applySymbiface();
    applyMouseInterface();
    applyLightgun();
    applyTapeOptions();
    if (emu->v9990) emu->v9990->setEnabled(v9990Enabled);
    applyOpl4();
    applyPlayCity();
    applySpeech();
    applyAudioRate();
}

// The Multiface II's ROM: Romantic Robot's, not shipped -- found in roms/ by name, the
// 1988 revision (the one MAME knows, CRC F36086DE) first.
std::string EmuHost::multifaceRom() const {
    for (const char* key : { "multiface 0e", "multiface", "mface", "multface" }) {
        const std::string p = findRom(key);
        if (!p.empty()) return p;
    }
    return "";
}

void EmuHost::applyMultiface() {
    if (!emu || !emu->multiface) return;
    // A CRO's ROM_MF2 is a Multiface fitted with that ROM.
    if (!croSet.mf2.empty() && !emu->plusHardware) { emu->multiface->setEnabled(emu->multiface->setRom(croSet.mf2)); return; }
    if (!multifaceEnabled || emu->plusHardware) { emu->multiface->setEnabled(false); return; }
    bool ok = false;
    const Bytes rom = readFile(multifaceRom(), ok);
    if (!ok || !emu->multiface->setRom(rom)) { emu->multiface->setEnabled(false); return; }
    emu->multiface->setEnabled(true);
}

bool EmuHost::setMultiface(bool on) {
    if (on && emu && emu->plusHardware) { status = "The Multiface II is for a classic CPC (not Plus/GX4000)."; return false; }
    if (on && multifaceRom().empty()) { status = "No Multiface II ROM in " + romDir + " (a file with \"multiface\" in its name)"; return false; }
    multifaceEnabled = on;
    applyMultiface();
    status = on ? "Multiface II fitted: F10 is its STOP button" : "Multiface II removed";
    return true;
}

void EmuHost::multifaceStop() {
    if (emu && emu->multiface && emu->multiface->enabled) { emu->multiface->stop(); status = "Multiface II: STOP"; }
}

// The serial card's line lives in the host, so a reboot (a new machine) keeps it connected.
void EmuHost::applySerial() {
    if (!emu || !emu->serial) return;
    if (!serialLink) serialLink = std::make_shared<HostSerial>();
    emu->serial->link = serialLink;
    emu->serial->enabled = serialEnabled && !emu->plusHardware;
}

bool EmuHost::connectSerial() {
    if (!serialLink) serialLink = std::make_shared<HostSerial>();
    HostSerial::Kind kind = HostSerial::Kind::None;
    if (serialKind == "tcp") kind = HostSerial::Kind::Tcp;
    else if (serialKind == "listen") kind = HostSerial::Kind::Listen;
    else if (serialKind == "com") kind = HostSerial::Kind::Com;
    else if (serialKind == "loopback") kind = HostSerial::Kind::Loopback;
    serialLink->allowLan = serialLan;
    std::string error;
    if (!serialLink->open(kind, serialTarget, error)) { status = "Serial line: " + error; return false; }
    status = "Serial line: " + serialLink->describe();
    return true;
}

bool EmuHost::setSerial(bool on) {
    if (on && emu && emu->plusHardware) { status = "The RS232C interface is for a classic CPC (not Plus/GX4000)."; return false; }
    serialEnabled = on;
    bool ok = true;
    if (on) ok = connectSerial();
    else if (serialLink) serialLink->close();
    applySerial();
    if (on && ok) status = "RS232C interface fitted; its line: " + serialLink->describe();
    if (!on) status = "RS232C interface removed";
    return ok;
}

std::string EmuHost::serialStatus() const {
    if (!serialEnabled) return "not fitted";
    if (!serialLink) return "no line";
    std::string s = serialLink->describe();
    if (emu && emu->serial && emu->serial->baud() > 0) s += ", " + std::to_string(emu->serial->baud()) + " baud";
    return s;
}

void EmuHost::applyM4Network() {
    if (!emu || !emu->m4) return;
    emu->m4->net->linked = m4Network;
    emu->m4->net->allowLan = m4NetworkLan;
    if (!m4Network) emu->m4->net->closeAll();
    if (emu->sf3) {                                    // the SF3's WiFi: the same switch
        emu->sf3->net->linked = m4Network;
        if (!m4Network) emu->sf3->net->closeAll();
    }
}

void EmuHost::applyM4() {
    applyM4Network();
    if (!m4Enabled || !emu || !emu->m4 || !emu->memory) return;
    if (emu->plusHardware && !emu->plusComputer) return;   // the GX4000 has no expansion port
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
        if (emu->plusHardware && !emu->plusComputer) { status = "The GX4000 has no expansion port for the M4."; return false; }
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
    bool sf2 = symbifaceModule == "sf2", sf3 = symbifaceModule == "sf3", xmass = symbifaceModule == "xmass";
    if (emu->symbifaceMouse) { emu->symbifaceMouse->setEnabled(sf2); emu->symbifaceMouse->setSensitivity(mouseSensitivity); }
    if (emu->sf2Rtc) emu->sf2Rtc->setEnabled(sf2);       // its DS12887 clock at &FD14/&FD15
    if (emu->sf3) {
        emu->sf3->setEnabled(sf3);
        emu->sf3->setSensitivity(mouseSensitivity);
        emu->sf3->audio->usbFolder = sf3UsbFolder;
        emu->sf3->audio->sdFolder = sf3SdFolder;
        emu->sf3->audio->setOutputSampleRate((double)sampleRate);
        emu->sf3->net->linked = m4Network;
        emu->sf3->audio->ffmpeg = MediaRecorder::findFfmpeg(ffmpegPath);   // AAC, Ogg... (sf3_audio.h)
    }
    if (emu->ide) {
        // Both Symbifaces carry the IDE/CF interface at &FD06-&FD0F; the X-MASS is that
        // interface alone, at &FD08-&FD0F.
        emu->ide->xmass = xmass;
        if ((sf2 || sf3 || xmass) && !ideFolder.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path p = fs::u8path(ideFolder);
            if (!fs::exists(p, ec)) fs::create_directories(p, ec);
            emu->ide->setFolder(ideFolder);
            emu->ide->setEnabled(true);
        } else emu->ide->setEnabled(false);
    }
}

void EmuHost::setSf3Folder(bool usb, const std::string& folder) {
    (usb ? sf3UsbFolder : sf3SdFolder) = folder;
    if (emu && emu->sf3) (usb ? emu->sf3->audio->usbFolder : emu->sf3->audio->sdFolder) = folder;   // the card keeps playing
    status = std::string(usb ? "SF3 USB stick: " : "SF3 SD card: ") + folder;
}

void EmuHost::setIdeFolder(const std::string& folder) {
    ideFolder = folder;
    applySymbiface();
    status = "Symbiface IDE: " + folder;
}

void EmuHost::rescanIde() {
    if (emu && emu->ide && emu->ide->enabled) { emu->ide->rescan(); status = "IDE folder rescanned"; }
}

bool EmuHost::mouseInterfaceActive() const {
    return mouseInterface != "none" && symbifaceModule != "sf2" && symbifaceModule != "sf3";
}

void EmuHost::applyMouseInterface() {
    if (!emu || !emu->keyboard) return;
    const std::string k = mouseInterfaceActive() ? mouseInterface : "none";
    emu->keyboard->amxEnabled = k == "amx";
    if (k != "amx") { emu->keyboard->amxLines = 0xff; emu->keyboard->amxMoveX = emu->keyboard->amxMoveY = 0; }
    const int kind = k == "kempston" ? MouseInterface::Kempston : k == "multiplay-a" ? MouseInterface::MultiplayA
                   : k == "multiplay-b" ? MouseInterface::MultiplayB : MouseInterface::None;
    if (emu->mouseIf.kind != kind) { emu->mouseIf = MouseInterface{}; emu->mouseIf.kind = kind; }
}

void EmuHost::setMouseInterface(const std::string& kind) {
    mouseInterface = (kind == "amx" || kind == "kempston" || kind == "multiplay-a" || kind == "multiplay-b") ? kind : "none";
    applyMouseInterface();
    static const std::pair<const char*, const char*> names[] = {
        { "amx", "AMX mouse (joystick port)" }, { "kempston", "Kempston mouse" },
        { "multiplay-a", "MultiPlay, mouse in port A" }, { "multiplay-b", "MultiPlay, mouse in port B" } };
    std::string name = "none";
    for (const auto& n : names) if (mouseInterface == n.first) name = n.second;
    status = "Mouse interface: " + name;
    if (mouseInterface != "none" && !mouseInterfaceActive()) status += " (off while the Symbiface's own mouse is fitted)";
}

void EmuHost::setSymbiface(const std::string& module) {
    symbifaceModule = module;
    applySymbiface();
    applyMouseInterface();                 // a Symbiface II/III's mouse keeps the interface out
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
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::MouseMove; e.x = dx; e.y = dy; timelineLog(e); }
    if (symbifaceModule == "sf2" && emu && emu->symbifaceMouse) emu->symbifaceMouse->move(dx, dy);
    else if (symbifaceModule == "sf3" && emu && emu->sf3) emu->sf3->move(dx, dy);
    else if (mouseInterfaceActive() && emu && emu->keyboard) {
        // whole counts to the interface, the fractions kept for the next move
        mouseFracX += dx * mouseSensitivity;
        mouseFracY += dy * mouseSensitivity;
        const int ix = (int)mouseFracX, iy = (int)mouseFracY;
        mouseFracX -= ix; mouseFracY -= iy;
        if (ix == 0 && iy == 0) return;
        if (mouseInterface == "amx") emu->keyboard->amxMove(ix, iy);
        else emu->mouseIf.move(ix, iy);
    }
}
void EmuHost::mouseButton(int index, bool pressed) {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::MouseButton; e.a = index; e.b = pressed ? 1 : 0; timelineLog(e); }
    if (symbifaceModule == "sf2" && emu && emu->symbifaceMouse) emu->symbifaceMouse->button(index, pressed);
    else if (symbifaceModule == "sf3" && emu && emu->sf3) emu->sf3->button(index, pressed);
    else if (mouseInterfaceActive() && emu && emu->keyboard) {
        if (mouseInterface == "amx") emu->keyboard->amxButton(index, pressed);
        else emu->mouseIf.button(index, pressed);
    }
}
void EmuHost::mouseScroll(float delta) {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::MouseScroll; e.x = delta; timelineLog(e); }
    if (symbifaceModule == "sf2" && emu && emu->symbifaceMouse) emu->symbifaceMouse->scroll(delta);
    else if (symbifaceModule == "sf3" && emu && emu->sf3) emu->sf3->scroll(delta);
}

void EmuHost::applyExpansionRoms() {
    if (!emu || !emu->memory) return;
    for (const auto& r : expansionRoms) emu->memory->setUpperRom(r.slot, r.data);
    if (!emu->plusHardware) for (const auto& r : croSet.upper) emu->memory->setUpperRom(r.first, r.second);   // a CRO's ROM_HIGH
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
    if (!emu || !lightgunActive() || liveInputBlocked()) return;
    // logged as one input: the trigger it sets inside is not logged again
    const bool outer = !applyingInput;
    if (outer) { InputEvent e; e.kind = InputEvent::LightgunAim; e.x = x; e.y = y; e.a = trigger ? 1 : 0; timelineLog(e); }
    struct Inner { bool& f; bool was; Inner(bool& f) : f(f), was(f) { f = true; } ~Inner() { f = was; } } inner(applyingInput);
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
    if (s.kind == SpeechSynth::Kind::LambdaSpeak3 && s.eepromPath != ls3EepromFile) s.setEepromFile(ls3EepromFile);
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
    emu->tape->fastLoad = tapeFastLoad;
    emu->tape->instantLoad = tapeInstantLoad;
    emu->tape->fastLoadBaud = std::clamp(tapeFastBaud, (int)CPCTapeDrive::FAST_BAUD_MIN, (int)CPCTapeDrive::FAST_BAUD_MAX);
}

int EmuHost::analogue(int channel) const {
    if (!emu || !emu->asic || channel < 0 || channel > 7) return 0;
    return emu->asic->analogueInput[channel] & 0x3f;
}
void EmuHost::setAnalogue(int channel, int value) {
    if (!emu || !emu->asic || channel < 0 || channel > 7 || liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::Analogue; e.a = channel; e.b = value; timelineLog(e); }
    emu->asic->analogueInput[channel] = (uint8_t)std::clamp(value, 0, 63);
}


bool EmuHost::loadByExtension(const std::string& path) {
    std::string ext;
    { auto e = std::filesystem::path(path).extension().string(); for (char c : e) ext += (char)std::tolower((unsigned char)c); }
    if (ext == ".cpr" || ext == ".cro") return loadCartridgeFile(path);
    if (ext == ".dsk" || ext == ".edsk" || ext == ".hfe" || ext == ".ipf") return loadDiskFile(path, 0);
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
    std::string croWhat;                  // the CRO group, for the status line
    if (isCro(data)) {
        std::vector<CroGroup> groups;
        try { groups = parseCro(data); }
        catch (const std::exception& ex) { status = std::string("Not a valid .CRO file: ") + ex.what(); return false; }
        int pick = croDefaultGroup(groups);
        std::string missing;
        if (croGroup >= 0) {
            int want = -1;
            for (int i = 0; i < (int)groups.size(); i++) if (groups[i].number == (uint32_t)croGroup) want = i;
            if (want >= 0) pick = want;
            else missing = "; it has no group " + std::to_string(croGroup);
        }
        std::vector<std::string> labels;
        std::vector<uint32_t> numbers;
        for (const CroGroup& g : groups) {
            labels.push_back(std::to_string(g.number) + (g.label.empty() ? "" : ": " + g.label));
            numbers.push_back(g.number);
        }
        const CroGroup& g = groups[(size_t)pick];
        croWhat = " (CRO group " + labels[(size_t)pick] + missing + ")";
        if (!g.hasBankable()) {
            CroRomSet set;
            for (const CroRom& r : g.roms) {
                if (r.type == CRO_ROM_LOW) set.lower = r.data;
                else if (r.type == CRO_ROM_HIGH && r.logical <= 255) set.upper.push_back({ (int)r.logical, r.data });
                else if (r.type == CRO_ROM_MF2) set.mf2 = r.data;
            }
            if (set.lower.empty() && set.upper.empty() && set.mf2.empty()) { status = "The CRO group " + labels[(size_t)pick] + " holds no ROM this emulator knows."; return false; }
            if (!loadCroRomSet(set, path, croWhat)) return false;
            croGroupLabels = labels;
            croGroupNumbers = numbers;
            return true;
        }
        try { cart = croCartridge(g); }
        catch (const std::exception& ex) { status = std::string("Not a valid .CRO cartridge: ") + ex.what(); return false; }
        croSet = CroRomSet{};
        croGroupLabels = labels;
        croGroupNumbers = numbers;
    } else {
        croGroupLabels.clear(); croGroupNumbers.clear(); croSet = CroRomSet{};
        try { cart = parseCartridge(data); }
        catch (const std::exception& ex) { status = std::string("Not a valid .CPR cartridge: ") + ex.what(); return false; }
    }
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
    // A Plus computer keeps its RAM (its expansion too); a GX4000 has 64K, or 128K with the
    // RAM mod.
    const bool ram128 = m.computer ? ramKiB >= 128 : gx4000Ram128;
    opt.ram128 = ram128;
    opt.ramKiB = m.computer ? std::max(64, ramKiB) : (ram128 ? 128 : 64);
    opt.internalRamKiB = m.computer ? m.defaultRam : (gx4000Ram128 ? 128 : 64);
    opt.classicExpansion = plusExpansionClassic;
    if (!m.computer) ramKiB = opt.ramKiB;
    emu->loadCartridge(cart, opt);
    cartPath = path;
    emu->hasFdc = m.computer && m.amsdos;
    applyAudioRate();
    applySettings();
    applyMonitorSet();
    cartName = std::filesystem::path(path).filename().string();
    lastProgram = std::filesystem::path(path).stem().string();
    noteRecent(path);
    loadCheatsBeside(path);
    snapshotName.clear();
    paused = false;
    status = "Loaded " + cartName + croWhat + " into " + m.label;
    return true;
}

// A CRO group of CPC ROMs: a classic CPC (the one selected, else a 6128) boots with them.
bool EmuHost::loadCroRomSet(const CroRomSet& set, const std::string& path, const std::string& what) {
    int index = currentModel >= 0 && !models[(size_t)currentModel].plus ? currentModel : -1;
    if (index < 0)
        for (int i = 0; i < (int)models.size(); i++) if (models[i].id == "cpc6128" && models[i].available) { index = i; break; }
    if (index < 0) { status = "A CRO of CPC ROMs needs a classic CPC model (its ROMs in " + romDir + ")."; return false; }
    croSet = set;
    croBoot = true;
    const bool ok = bootModel(index, currentModel == index ? ramKiB : -1, currentModel == index ? crtcType : -1);
    croBoot = false;
    if (!ok) { croSet = CroRomSet{}; return false; }
    cartPath = path;
    cartName = std::filesystem::path(path).filename().string();
    lastProgram = std::filesystem::path(path).stem().string();
    noteRecent(path);
    std::string fitted;
    if (!set.lower.empty()) fitted += " OS";
    for (const auto& r : set.upper) fitted += " ROM " + std::to_string(r.first);
    if (!set.mf2.empty()) fitted += " Multiface II";
    status = "Booted " + models[(size_t)index].label + " with " + cartName + what + ":" + fitted;
    return true;
}

bool EmuHost::loadCroGroup(uint32_t number) {
    if (cartPath.empty()) return false;
    croGroup = (int)number;               // kept: a restart of the cartridge keeps the group
    return loadCartridgeFile(cartPath);
}

// A disc's writes, back into the file it came from -- in its own format (standard or
// extended DSK, HFE), written beside it and renamed over it, so a failure leaves the old one.
void EmuHost::flushDisk(int unit) {
    if (unit < 0 || unit > 1 || !diskDirty[unit]) return;
    diskDirty[unit] = false;
    if (!diskWriteBack || diskPath[unit].empty() || !emu || !emu->fdc || !emu->fdc->drives[(size_t)unit]) return;
    const Disk& disk = *emu->fdc->drives[(size_t)unit];
    if (disk.writeProtected) return;
    Bytes image;
    try { image = serializeDiskImage(disk); }
    catch (const std::exception& ex) { diskWriteError = ex.what(); status = "Could not save drive " + std::string(unit ? "B" : "A") + ": " + diskWriteError; return; }
    namespace fs = std::filesystem;
    const fs::path target = fs::u8path(diskPath[unit]);
    fs::path temp = target;
    temp += ".cpcse-tmp";
    bool ok = false;
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (f) { f.write((const char*)image.data(), (std::streamsize)image.size()); f.close(); ok = !f.fail(); }
    }
    std::error_code ec;
    if (ok) { fs::rename(temp, target, ec); ok = !ec; }
    if (!ok) {
        fs::remove(temp, ec);
        diskWriteError = "cannot write " + diskPath[unit];
        status = "Could not save drive " + std::string(unit ? "B" : "A") + " back to its file (read-only?)";
        return;
    }
    diskWriteError.clear();
}

bool EmuHost::loadDiskFile(const std::string& path, int unit) {
    if (unit < 0 || unit > 1) return false;
    flushDisk(unit);
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read disk image."; return false; }
    if (!emu->fdc) { status = "No floppy controller."; return false; }
    std::shared_ptr<Disk> disk;
    try { disk = emu->fdc->mount(data, unit); }
    catch (const std::exception& ex) { status = std::string("Not a disc image this can read: ") + ex.what(); return false; }
    if (!disk) { status = "Not a disc image this can read."; return false; }
    emu->hasFdc = true;
    diskName[unit] = std::filesystem::path(path).filename().string();
    diskPath[unit] = path;
    if (unit == 0) { loadCheatsBeside(path); lastProgram = std::filesystem::path(path).stem().string(); }
    noteRecent(path);
    status = "Inserted " + diskName[unit] + " into drive " + std::string(unit == 0 ? "A" : "B");
    return true;
}

bool EmuHost::insertDisk(std::shared_ptr<Disk> disk, int unit, const std::string& name) {
    if (unit < 0 || unit > 1 || !emu || !emu->fdc || !disk) return false;
    flushDisk(unit);
    diskPath[unit].clear();               // no file of its own: the DSK editor saves it
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
    flushDisk(unit);
    emu->fdc->eject(unit);
    diskName[unit].clear();
    diskPath[unit].clear();
    status = "Ejected drive " + std::string(unit == 0 ? "A" : "B");
}

bool EmuHost::loadTapeFile(const std::string& path) {
    flushTape();                              // a recording on the tape going out: into its file
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { status = "Could not read tape image."; return false; }
    if (!emu->tape) { status = "No tape drive."; return false; }
    std::string name = std::filesystem::path(path).filename().string();
    try {
        if (!emu->tape->load(data, name)) { status = "Not a valid .CDT/.TZX tape."; return false; }
    } catch (const std::exception& ex) { status = std::string("Not a valid .CDT/.TZX tape: ") + ex.what(); return false; }
    tapeName = name;
    tapePath = path;
    lastProgram = std::filesystem::path(path).stem().string();
    noteRecent(path);
    loadCheatsBeside(path);
    status = "Inserted tape " + tapeName + "  (press Play)";
    return true;
}

CPCTapeDrive* EmuHost::tapeDeck() const { return emu && emu->tape && !tapeName.empty() && emu->tape->loaded ? emu->tape : nullptr; }
bool EmuHost::tapePlaying() const { CPCTapeDrive* d = tapeDeck(); return d && d->playing && !d->paused; }
bool EmuHost::tapePaused() const { CPCTapeDrive* d = tapeDeck(); return d && d->playing && d->paused; }


void EmuHost::tapePlay() {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_PLAY; timelineLog(e); }
    if (CPCTapeDrive* d = tapeDeck()) { d->play(); status = d->playing ? "Tape playing" : "The tape is at its end: rewind it"; }
}

void EmuHost::tapePause() {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_PAUSE; timelineLog(e); }
    CPCTapeDrive* d = tapeDeck();
    if (!d || !d->playing) return;
    d->setPaused(!d->paused);
    status = d->paused ? "Tape paused" : "Tape playing";
}

void EmuHost::tapeRewind() {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_REWIND; timelineLog(e); }
    if (CPCTapeDrive* d = tapeDeck()) { d->rewind(); status = "Tape rewound"; }
}

void EmuHost::tapeRewindBlock() {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_REWIND_BLOCK; timelineLog(e); }
    if (CPCTapeDrive* d = tapeDeck()) { d->rewindBlock(); status = "Tape at block " + std::to_string(d->blockAtPosition() + 1); }
}

void EmuHost::tapeFastForward() {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_FAST_FORWARD; timelineLog(e); }
    if (CPCTapeDrive* d = tapeDeck()) {
        d->fastForward();
        status = d->tapeEnded ? std::string("Tape at its end") : "Tape at block " + std::to_string(d->blockAtPosition() + 1);
    }
}

void EmuHost::tapeSeekBlock(int index) {
    if (liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_SEEK; e.b = index; timelineLog(e); }
    if (CPCTapeDrive* d = tapeDeck()) { d->seekBlock(index); status = "Tape at block " + std::to_string(d->blockAtPosition() + 1); }
}

void EmuHost::tapeEject() {
    if (!emu || !emu->tape) return;
    flushTape();
    emu->tape->eject();
    status = tapeName.empty() ? "No tape" : "Ejected " + tapeName;
    tapeName.clear();
    tapePath.clear();
}

Bytes* EmuHost::ram() { return emu && emu->memory ? &emu->memory->ram : nullptr; }

void EmuHost::loadCheatsBeside(const std::string& mediaPath) {
    namespace fs = std::filesystem;
    fs::path p = fs::u8path(mediaPath);
    p.replace_extension(".pok");
    cheatsPath = p.u8string();
    std::error_code ec;
    if (fs::exists(p, ec)) loadCheats(cheatsPath);
}

bool EmuHost::loadCheats(const std::string& path) {
    bool ok = false;
    const Bytes text = readFile(path, ok);
    if (!ok) { status = "Could not read " + path; return false; }
    std::vector<Cheat> found;
    std::string error;
    if (!CheatList::parse(std::string(text.begin(), text.end()), found, error)) { status = "Cheats: " + error; return false; }
    cheatList.cheats = found;                 // switched off until asked for
    cheatsPath = path;
    status = "Cheats: " + std::to_string(found.size()) + " from " + std::filesystem::u8path(path).filename().u8string();
    return true;
}

bool EmuHost::saveCheats(const std::string& path) {
    const std::string target = path.empty() ? cheatsPath : path;
    if (target.empty()) { status = "Cheats: no file to save to (load a game, or use Save as)"; return false; }
    std::ofstream f(std::filesystem::u8path(target), std::ios::binary | std::ios::trunc);
    const std::string text = CheatList::serialize(cheatList.cheats);
    if (f) f.write(text.data(), (std::streamsize)text.size());
    if (!f) { status = "Could not write " + target; return false; }
    cheatsPath = target;
    status = "Cheats saved to " + std::filesystem::u8path(target).filename().u8string();
    return true;
}

void EmuHost::enableCheat(size_t index, bool on) {
    if (index >= cheatList.cheats.size()) return;
    Cheat& c = cheatList.cheats[index];
    c.enabled = on;
    if (on && !c.freeze) if (Bytes* r = ram()) { CheatList::poke(*r, c); timelineEdited(); }
}

bool EmuHost::tapeTurboActive() const {
    CPCTapeDrive* d = tapeDeck();
    return tapeTurbo > 1 && d && d->playing && !d->paused && d->isMotorActive();
}

bool EmuHost::tapeRecording() const { CPCTapeDrive* d = tapeDeck(); return d && d->recording; }

void EmuHost::tapeNewBlank() {
    if (!emu || !emu->tape) return;
    flushTape();
    emu->tape->newBlank("blank.cdt");
    tapeName = "blank.cdt";
    tapePath.clear();
    status = "A blank tape in the deck: press Record, then SAVE";
}

void EmuHost::tapeRecord() {
    if (!emu || !emu->tape) return;
    timelineEdited();                     // a recording writes the tape image
    if (!tapeDeck()) tapeNewBlank();
    CPCTapeDrive* d = tapeDeck();
    if (!d) return;
    if (!d->startRecording()) { status = "This tape cannot be recorded on (a .TAP): start a blank one"; return; }
    status = "Recording: REC and PLAY down -- SAVE, then Stop";
}

void EmuHost::tapeStop() {
    if (liveInputBlocked()) return;
    CPCTapeDrive* d = tapeDeck();
    if (!d) return;
    const bool wasRecording = d->recording;
    // a recording ending rebuilds the tape image: the timeline starts again; else a button
    if (wasRecording) timelineEdited();
    else if (!applyingInput) { InputEvent e; e.kind = InputEvent::TapeDeck; e.a = InputEvent::DECK_STOP; timelineLog(e); }
    d->stop();
    if (wasRecording) {
        d->stopRecording();
        flushTape();
        if (status.rfind("Could not", 0) != 0)
            status = d->modified ? "Recorded onto " + tapeName + (tapePath.empty() ? " (not saved yet: Save tape as)" : "") : "Recording stopped: nothing was written";
        return;
    }
    status = "Tape stopped";
}

// A recording into the tape's file, in its own format (TZX; a .cdt is one), written beside
// it and renamed over it -- only with the write-back setting on, and only a .cdt/.tzx file
// (a .wav or .tap stays as it was).
void EmuHost::flushTape() {
    if (!emu || !emu->tape || !emu->tape->loaded) return;
    CPCTapeDrive& d = *emu->tape;
    if (d.recording) d.stopRecording();
    if (!d.modified || !diskWriteBack || tapePath.empty()) return;
    std::string ext;
    for (char c : std::filesystem::path(tapePath).extension().string()) ext += (char)std::tolower((unsigned char)c);
    if (ext != ".cdt" && ext != ".tzx") return;
    if (tapeSaveAs(tapePath)) d.modified = false;
}

bool EmuHost::tapeSaveAs(const std::string& path) {
    if (!emu || !emu->tape || !emu->tape->loaded) { status = "No tape to save."; return false; }
    CPCTapeDrive& d = *emu->tape;
    if (d.recording) d.stopRecording();
    namespace fs = std::filesystem;
    const fs::path target = fs::u8path(path);
    fs::path temp = target;
    temp += ".cpcse-tmp";
    bool ok = false;
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (f) { f.write((const char*)d.data.data(), (std::streamsize)d.data.size()); f.close(); ok = !f.fail(); }
    }
    std::error_code ec;
    if (ok) { fs::rename(temp, target, ec); ok = !ec; }
    if (!ok) { fs::remove(temp, ec); status = "Could not write " + path; return false; }
    tapePath = path;
    tapeName = target.filename().string();
    d.name = tapeName;
    d.modified = false;
    status = "Saved the tape as " + tapeName;
    return true;
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

bool EmuHost::applySnaFile(const std::string& path, std::string& why) {
    bool ok = false;
    Bytes data = readFile(path, ok);
    if (!ok || data.empty()) { why = "Could not read snapshot."; return false; }
    Snapshot snap;
    try { snap = parseSna(data); }
    catch (const std::exception& ex) { why = std::string("Not a valid .SNA snapshot: ") + ex.what(); return false; }
    if (snap.ram.empty()) { why = "Not a valid .SNA snapshot."; return false; }
    timelineEpoch += 1;
    applySna(emu, snap);
    applyAudioRate();
    paused = false;
    return true;
}

bool EmuHost::loadSnapshot(const std::string& path) {
    std::string why;
    if (!applySnaFile(path, why)) { status = why; return false; }
    snapshotName = std::filesystem::path(path).filename().string();
    lastProgram = std::filesystem::path(path).stem().string();
    loadCheatsBeside(path);
    noteRecent(path);
    status = "Loaded snapshot " + snapshotName;
    return true;
}

// ============================================================== recent files
void EmuHost::noteRecent(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string full = fs::absolute(fs::u8path(path), ec).lexically_normal().u8string();
    if (ec || full.empty()) full = path;
    auto same = [&](const std::string& other) {
#ifdef _WIN32
        if (other.size() != full.size()) return false;
        for (size_t i = 0; i < full.size(); i++)
            if (std::tolower((unsigned char)other[i]) != std::tolower((unsigned char)full[i])) return false;
        return true;
#else
        return other == full;
#endif
    };
    recentFiles.erase(std::remove_if(recentFiles.begin(), recentFiles.end(), same), recentFiles.end());
    recentFiles.insert(recentFiles.begin(), full);
    if (recentFiles.size() > RECENT_MAX) recentFiles.resize(RECENT_MAX);
}

// ============================================================== quick save slots
// A small 32-bit picture file, top-down (what saveScreenshotBmp writes), and back.
static bool writeBmp32(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xff)); f.put((char)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) f.put((char)((v >> (8 * i)) & 0xff)); };
    const uint32_t dataSize = (uint32_t)(w * h * 4);
    f.put('B'); f.put('M'); u32(54 + dataSize); u16(0); u16(0); u32(54);
    u32(40); u32((uint32_t)w); u32((uint32_t)(-h)); u16(1); u16(32);
    u32(0); u32(dataSize); u32(2835); u32(2835); u32(0); u32(0);
    for (uint32_t p : px) { f.put((char)(p >> 16 & 0xff)); f.put((char)(p >> 8 & 0xff)); f.put((char)(p & 0xff)); f.put((char)0xff); }
    return (bool)f;
}
static bool readBmp32(const std::string& path, std::vector<uint32_t>& px, int& w, int& h) {
    bool ok = false;
    const Bytes b = EmuHost::readFile(path, ok);
    if (!ok || b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    auto u32 = [&](size_t at) { return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24; };
    const uint32_t offset = u32(10);
    const int width = (int)u32(18), height = -(int32_t)u32(22);
    if (width <= 0 || width > 1024 || height <= 0 || height > 1024 || (b[28] | b[29] << 8) != 32) return false;
    if ((size_t)offset + (size_t)width * height * 4 > b.size()) return false;
    px.resize((size_t)width * height);
    for (size_t i = 0; i < px.size(); i++) {
        const uint8_t* s = &b[offset + i * 4];
        px[i] = 0xff000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
    }
    w = width; h = height;
    return true;
}

std::string EmuHost::slotProgram() const {
    std::string name = lastProgram;
    if (name.empty()) name = currentModel >= 0 ? models[(size_t)currentModel].id : "machine";
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) c = '_';
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    return name.empty() ? "machine" : name;
}

std::string EmuHost::slotFile(int slot, const char* extension) const {
    return (std::filesystem::u8path(stateDir) / std::filesystem::u8path(slotProgram()) / ("slot" + std::to_string(slot) + extension)).u8string();
}

void EmuHost::readSlot(int slot) {
    namespace fs = std::filesystem;
    SlotInfo& s = slots[(size_t)slot];
    s = SlotInfo{};
    s.revision = ++slotRevision;
    std::error_code ec;
    const fs::path sna = fs::u8path(slotFile(slot, ".sna"));
    if (!fs::is_regular_file(sna, ec)) return;
    s.used = true;
    const auto written = fs::last_write_time(sna, ec);
    if (!ec) {
        // the file clock to the system clock, to print it
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(written - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t t = std::chrono::system_clock::to_time_t(sys);
        if (const std::tm* tm = std::localtime(&t)) {
            char b[32];
            std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", tm);
            s.when = b;
        }
    }
    readBmp32(slotFile(slot, ".bmp"), s.thumb, s.thumbW, s.thumbH);
}

const EmuHost::SlotInfo& EmuHost::slotInfo(int slot) {
    slot = std::clamp(slot, 1, SLOT_COUNT);
    if (slotsFor != slotProgram()) {
        slotsFor = slotProgram();
        for (int i = 1; i <= SLOT_COUNT; i++) readSlot(i);
    }
    return slots[(size_t)slot];
}

bool EmuHost::saveSlot(int slot) {
    if (slot < 1 || slot > SLOT_COUNT || !booted()) return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::u8path(slotFile(slot, ".sna")).parent_path(), ec);
    const Bytes data = createSna(emu, false);
    if (!writeFileSafely(slotFile(slot, ".sna"), data.data(), data.size())) {
        status = "Could not write slot " + std::to_string(slot) + " (" + slotFile(slot, ".sna") + ")";
        return false;
    }
    // its picture, a quarter of the screen each way (each pixel the mean of 16)
    if (video) {
        const int w = video->width / 4, h = video->height / 4;
        std::vector<uint32_t> thumb((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++) {
                        const uint32_t p = video->pixels[(size_t)(y * 4 + dy) * video->width + x * 4 + dx];
                        r += p & 0xff; g += p >> 8 & 0xff; b += p >> 16 & 0xff;
                    }
                thumb[(size_t)y * w + x] = 0xff000000u | (b / 16) << 16 | (g / 16) << 8 | r / 16;
            }
        writeBmp32(slotFile(slot, ".bmp"), thumb, w, h);
    }
    // ...and, for this session, the machine exactly
    ExactSlot& e = exactSlots[(size_t)slot];
    e.machine = MachineSnapshot::capture(*emu);
    e.setup = slotProgram() + "#" + timelineFingerprint();
    if (video) e.picture = video->pixels;
    if (slotsFor == slotProgram()) readSlot(slot);
    status = "Saved slot " + std::to_string(slot) + " (" + slotProgram() + ")";
    return true;
}

void EmuHost::restoreExact(const MachineSnapshot& s, const std::vector<uint32_t>& picture) {
    s.restore(*emu);
    if (video && picture.size() == video->pixels.size()) { video->pixels = picture; pictureChanged = true; }
}

bool EmuHost::loadSlot(int slot) {
    if (slot < 1 || slot > SLOT_COUNT || !booted()) return false;
    if (!slotInfo(slot).used) { status = "Slot " + std::to_string(slot) + " is empty"; return false; }
    if (moviePlay_ || movieRec) movieStop();
    // what it was, for Undo
    undoLoad = MachineSnapshot::capture(*emu);
    undoSetup = timelineFingerprint();
    undoPicture = video ? video->pixels : std::vector<uint32_t>{};
    const ExactSlot& e = exactSlots[(size_t)slot];
    const bool exact = e.machine && e.setup == slotProgram() + "#" + timelineFingerprint();
    if (exact) restoreExact(*e.machine, e.picture);
    else {
        std::string why;
        if (!applySnaFile(slotFile(slot, ".sna"), why)) { undoLoad.reset(); status = "Slot " + std::to_string(slot) + ": " + why; return false; }
    }
    timelineEdited();                     // not an input a replay could repeat: it starts again
    // the keys held when the slot was saved (its shortcut among them) are not held now
    if (emu->keyboard) emu->keyboard->reset();
    keyHeld.fill(false);
    status = "Loaded slot " + std::to_string(slot) + (exact ? "" : " (from its .sna file)");
    return true;
}

bool EmuHost::undoSlotLoad() {
    if (!undoLoad || !booted()) return false;
    if (undoSetup != timelineFingerprint()) { undoLoad.reset(); status = "The machine or its media changed: nothing to undo"; return false; }
    restoreExact(*undoLoad, undoPicture);
    undoLoad.reset();
    undoPicture.clear();
    timelineEdited();
    if (emu->keyboard) emu->keyboard->reset();
    keyHeld.fill(false);
    status = "Undid the last load";
    return true;
}

bool EmuHost::crtSlotMask() const {
    return crtPattern == "slot" || (crtPattern == "auto" && video && video->monitorColourTube);
}
bool EmuHost::crtScanlines() const {
    return crtPattern == "scanlines" || (crtPattern == "auto" && !(video && video->monitorColourTube));
}

void EmuHost::setPlusExpansionClassic(bool classic) {
    plusExpansionClassic = classic;
    if (emu && emu->memory) { emu->memory->classicExpansion = classic; emu->memory->remapRam(); }   // live: no restart needed
}

void EmuHost::setGx4000Ram128(bool on) {
    gx4000Ram128 = on;
    if (currentModel < 0 || models[(size_t)currentModel].computer) return;   // takes effect on a GX4000
    if (!cartPath.empty()) loadCartridgeFile(cartPath);                     // the same cartridge, restarted
    else bootModel(currentModel, -1, crtcType);
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
    if (!booted() || liveInputBlocked()) return;
    if (!applyingInput) { InputEvent e; e.kind = InputEvent::Reset; timelineLog(e); }
    emu->reset();
    status = "Reset";
}
// MC START PROGRAM (6128 OS &061C: DI, IM 1, the firmware set up again, SP &C000, EI) ends
// at &0077, which makes HL and C a far address and FAR CALLs it (RST 3). The far address's
// ROM byte &FF leaves the selection alone and disables both ROMs: a program in RAM. (C = 0
// would select BASIC's ROM and enable it, and a program's reads of &C000-&FFFF would read
// BASIC: CPCtelera's cpct_clearScreen filled the screen with it.)
static void mcStartProgram(Z80& cpu, int run) {
    cpu.iff1 = cpu.iff2 = false;
    cpu.sp = 0xc000;
    cpu.h = (run >> 8) & 0xff; cpu.l = run & 0xff;
    cpu.c = 0xff;
    cpu.pc = 0xbd16;
    cpu.halted = false;
}

std::function<bool(const std::vector<uint8_t>&, int, int, std::vector<uint8_t>&, std::string&)> EmuHost::programSnapshotMaker() {
    int index = currentModel >= 0 && models[(size_t)currentModel].computer && !models[(size_t)currentModel].plus ? currentModel : -1;
    for (size_t i = 0; index < 0 && i < models.size(); i++)
        if (models[i].computer && !models[i].plus && models[i].id == "cpc6128") index = (int)i;
    if (index < 0) return nullptr;
    const ModelProfile& m = models[(size_t)index];
    const bool same = index == currentModel;
    const std::string osPath = same && !osRomPath.empty() ? osRomPath : findRom(m.osKey);
    const std::string basicPath = same && !basicRomPath.empty() ? basicRomPath : findRom(m.basicKey);
    const std::string amsPath = !m.amsdos ? "" : same && !amsdosRomPath.empty() ? amsdosRomPath : findRom("amsdos");
    bool a = false, b = false, c = true;
    if (osPath.empty() || basicPath.empty()) return nullptr;
    GX4000::LoadClassicOptions o;
    o.lowerRom = readFile(osPath, a);
    o.basicRom = readFile(basicPath, b);
    if (!amsPath.empty()) o.amsdosRom = readFile(amsPath, c);
    if (!a || !b || !c) return nullptr;
    o.ramKiB = m.defaultRam;
    o.ram128 = m.defaultRam >= 128;
    o.crtcType = m.defaultCrtc;
    o.model = m.label;
    const bool fdc = m.amsdos;
    const std::string label = m.label;
    return [o, fdc, label](const std::vector<uint8_t>& bytes, int load, int run, std::vector<uint8_t>& sna, std::string& why) {
        try {
            GX4000 machine;
            machine.loadClassicFirmware(o);
            machine.hasFdc = fdc;
            for (int i = 0; i < 170; i++) machine.runFrame();     // to "Ready", as startProgram
            for (size_t i = 0; i < bytes.size(); i++) machine.memory->write((load + (int)i) & 0xffff, bytes[i]);
            mcStartProgram(*machine.cpu, run);
            sna = createSna(&machine, false);
            return !sna.empty();
        } catch (const std::exception& ex) {
            why = label + ": " + ex.what();
            return false;
        }
    };
}

bool EmuHost::startProgram(const std::vector<uint8_t>& bytes, int load, int run, std::string& how) {
    if (!booted() || !emu || bytes.empty()) return false;
    const bool firmware = currentModel >= 0 && models[(size_t)currentModel].computer;
    if (firmware) {
        // A clean machine at the BASIC prompt: what is left of an earlier program (the
        // firmware switched off, its jumpblock overwritten) cannot stop MC START PROGRAM.
        emu->reset();
        const bool wasPaused = paused;                 // runFrame does nothing while paused (the debugger)
        paused = false;
        for (int i = 0; i < 170; i++) runFrame();     // a 6128 is at "Ready" after some 150 frames
        paused = wasPaused;
    }
    for (size_t i = 0; i < bytes.size(); i++) emu->memory->write((load + (int)i) & 0xffff, bytes[i]);
    Z80* cpu = emu->cpu;
    if (firmware) {
        mcStartProgram(*cpu, run);
        how = "MC START PROGRAM";
    } else {
        // the RMR (&7Fxx, bits 7-6 = 10): bit 2 lower ROM off, bit 3 upper ROM off, mode kept
        const int first = load & 0xffff, last = std::min(0xffff, first + (int)bytes.size() - 1);
        int rmr = emu->gateArray->romConfig & 0x0f;
        if (first < 0x4000) rmr |= 0x04;
        if (last >= 0xc000) rmr |= 0x08;
        emu->writePort(0x7f00, 0x80 | rmr);
        cpu->pc = run & 0xffff;
        how = "jump (no firmware)";
    }
    cpu->halted = false;
    timelineEdited();
    return true;
}

void EmuHost::timelineEdited() {
    timelineEpoch += 1;
    if (movieRec) { movieRec = false; movieStatus = "An edit stopped the movie's recording (it could not be played back)."; }
}

// ============================================================== the timeline
std::string EmuHost::timelineFingerprint() const {
    std::string f = std::to_string(currentModel) + "|" + std::to_string(ramKiB) + "|" + std::to_string(emu && emu->crtc ? emu->crtc->type : -1)
        + "|" + std::to_string(gateArrayPart) + "|" + (z80Cmos ? "c" : "n") + "|" + dacType + "|" + lightgunType + "|"
        + (v9990Enabled ? "v" : "") + (opl4Enabled ? "o" : "") + (playCityEnabled ? "p" : "") + (m4Enabled ? "m" : "")
        + (serialEnabled ? "s" : "") + (multifaceEnabled ? "f" : "") + (gx4000Ram128 ? "g" : "") + (plusExpansionClassic ? "e" : "")
        + "|" + speechKind + "|" + symbifaceModule + "|" + mouseInterface + "|" + diskName[0] + "|" + diskName[1] + "|" + tapeName + "|" + cartName;
    for (const auto& r : expansionRoms) f += "|" + std::to_string(r.slot) + ":" + r.name;
    return f;
}
void EmuHost::timelineClear() {
    snapshots.clear();
    inputLog.clear();
    frameStarts.clear();
    replayAt = 0;
    snapshotBytes = 0;
    framesSinceSnapshot = timelineInterval;      // the next frame's end takes one
    liveEnd = emu ? emu->instructions : 0;
    if (emu) emu->inputDueAt = UINT64_MAX;
}
EmuHost::TimelineInfo EmuHost::timelineInfo() const {
    TimelineInfo i;
    i.oldest = snapshots.empty() ? 0 : snapshots.front()->instructions;
    i.now = emu ? emu->instructions : 0;
    i.liveEnd = std::max(liveEnd, i.now);
    i.snapshots = snapshots.size(); i.inputs = inputLog.size(); i.bytes = snapshotBytes;
    return i;
}
void EmuHost::timelineFrameStart() {
    if (!emu) return;
    if (!timelineEnabled && !moviePlay_) { if (!snapshots.empty() || !inputLog.empty()) timelineClear(); return; }
    // a different machine, other media, a reset: what came before does not lead here
    const std::string setup = timelineFingerprint();
    if (setup != timelineSetup || timelineEpoch != timelineEpochSeen) {
        timelineClear();
        timelineSetup = setup;
        timelineEpochSeen = timelineEpoch;
    }
    const uint64_t at = emu->instructions;
    if (frameStarts.empty() || at > frameStarts.back()) frameStarts.push_back(at);
    else {
        auto it = std::lower_bound(frameStarts.begin(), frameStarts.end(), at);
        if (it == frameStarts.end() || *it != at) frameStarts.insert(it, at);
    }
}
void EmuHost::timelineFrameEnd() {
    if (!emu || !timelineEnabled) return;
    liveEnd = std::max(liveEnd, emu->instructions);
    if (++framesSinceSnapshot < timelineInterval && !snapshots.empty()) return;
    if (!snapshots.empty() && emu->instructions <= snapshots.back()->instructions) return;   // replaying: there is one ahead
    std::unique_ptr<MachineSnapshot> s = MachineSnapshot::capture(*emu);
    snapshotBytes += s->bytes;
    snapshots.push_back(std::move(s));
    framesSinceSnapshot = 0;
    // the budget: the oldest go, and the inputs and frames before the new oldest with them
    const size_t budget = (size_t)std::max(16, timelineBudgetMB) * 1024 * 1024;
    while (snapshotBytes > budget && snapshots.size() > 2) {
        snapshotBytes -= snapshots.front()->bytes;
        snapshots.pop_front();
    }
    const uint64_t oldest = snapshots.front()->instructions;
    size_t drop = 0;
    while (drop < inputLog.size() && inputLog[drop].at < oldest) drop++;
    if (drop > 0) { inputLog.erase(inputLog.begin(), inputLog.begin() + (long)drop); replayAt = replayAt > drop ? replayAt - drop : 0; }
    while (!frameStarts.empty() && frameStarts.front() < oldest) frameStarts.pop_front();
}
void EmuHost::timelineTruncate() {
    // a new input after going back: the inputs, snapshots and frames of the old future go
    const uint64_t now = emu->instructions;
    if (replayAt < inputLog.size()) inputLog.resize(replayAt);
    while (!snapshots.empty() && snapshots.back()->instructions > now) { snapshotBytes -= snapshots.back()->bytes; snapshots.pop_back(); }
    while (!frameStarts.empty() && frameStarts.back() > now) frameStarts.pop_back();
    liveEnd = now;
    emu->inputDueAt = UINT64_MAX;
}
void EmuHost::timelineLog(InputEvent e) {
    if (applyingInput || !emu) return;
    e.at = emu->instructions;
    if (movieRec) movieEvents.push_back(e);
    if (!timelineEnabled) return;
    if (emu->instructions < liveEnd || replayAt < inputLog.size()) timelineTruncate();
    inputLog.push_back(e);
    replayAt = inputLog.size();
    liveEnd = std::max(liveEnd, e.at);
}
void EmuHost::timelineApply(const InputEvent& e) {
    KeyboardMatrix& k = *emu->keyboard;
    switch (e.kind) {
        case InputEvent::Key: k.setKey(e.code, e.a != 0, e.text, e.b); break;
        case InputEvent::Joystick: k.setJoystick(e.a, e.b != 0, e.code, e.c); break;
        case InputEvent::MatrixDirect: k.setMatrixDirect(e.a, e.b, e.c != 0); break;
        case InputEvent::LightgunTrigger: k.setLightgunTrigger(e.a != 0); break;
        case InputEvent::LightgunAim: lightgunAim(e.x, e.y, e.a != 0); break;
        case InputEvent::MouseMove: mouseMove((float)e.x, (float)e.y); break;
        case InputEvent::MouseButton: mouseButton(e.a, e.b != 0); break;
        case InputEvent::MouseScroll: mouseScroll((float)e.x); break;
        case InputEvent::Cheats: if (!cheatList.cheats.empty()) cheatList.apply(emu->memory->ram); break;
        case InputEvent::Reset: reset(); break;
        case InputEvent::Analogue: setAnalogue(e.a, e.b); break;
        case InputEvent::TapeDeck:
            switch (e.a) {
                case InputEvent::DECK_PLAY: tapePlay(); break;
                case InputEvent::DECK_PAUSE: tapePause(); break;
                case InputEvent::DECK_REWIND: tapeRewind(); break;
                case InputEvent::DECK_REWIND_BLOCK: tapeRewindBlock(); break;
                case InputEvent::DECK_FAST_FORWARD: tapeFastForward(); break;
                case InputEvent::DECK_SEEK: tapeSeekBlock(e.b); break;
                case InputEvent::DECK_STOP: tapeStop(); break;
                default: break;
            }
            break;
    }
}
void EmuHost::timelineApplyDue() {
    const bool was = applyingInput;
    applyingInput = true;
    while (replayAt < inputLog.size() && inputLog[replayAt].at <= emu->instructions) {
        const InputEvent& e = inputLog[replayAt++];
        if (movieRec) movieEvents.push_back(e);          // recording after going back: the history replayed is the movie's
        timelineApply(e);
    }
    applyingInput = was;
    emu->inputDueAt = replayAt < inputLog.size() ? inputLog[replayAt].at : UINT64_MAX;
}
bool EmuHost::timelineGoTo(uint64_t target) {
    if (!booted()) return false;
    if (!timelineEnabled || snapshots.empty()) { timelineMessage = "The timeline is off, or has no snapshot yet."; return false; }
    const uint64_t end = std::max(liveEnd, emu->instructions);
    if (target > end) target = end;
    // the snapshot: the last one at or before the point -- one more back when that leaves
    // less than two frames to run, so the picture is made again on the way
    int pick = -1;
    for (int i = (int)snapshots.size() - 1; i >= 0; i--) if (snapshots[(size_t)i]->instructions <= target) { pick = i; break; }
    if (pick < 0) { timelineMessage = "That is before the oldest snapshot."; return false; }
    {
        const uint64_t from = snapshots[(size_t)pick]->instructions;
        const long frames = std::count_if(frameStarts.begin(), frameStarts.end(), [&](uint64_t f) { return f > from && f <= target; });
        if (frames < 2 && pick > 0) pick -= 1;
    }
    const MachineSnapshot& s = *snapshots[(size_t)pick];
    if (movieRec) {
        while (!movieEvents.empty() && movieEvents.back().at >= s.instructions) movieEvents.pop_back();
        while (!movieChecks.empty() && movieChecks.back().at > s.instructions) movieChecks.pop_back();
    }
    s.restore(*emu);
    replayAt = (size_t)(std::lower_bound(inputLog.begin(), inputLog.end(), s.instructions,
                                         [](const InputEvent& e, uint64_t v) { return e.at < v; }) - inputLog.begin());
    emu->inputDueAt = replayAt < inputLog.size() ? inputLog[replayAt].at : UINT64_MAX;
    // on to the point: the frames' starts as they came (the cheats' freezes went in there)
    auto frame = std::lower_bound(frameStarts.begin(), frameStarts.end(), s.instructions);
    while (emu->instructions < target) {
        while (frame != frameStarts.end() && *frame < emu->instructions) ++frame;
        if (frame != frameStarts.end() && *frame == emu->instructions) { if (!cheatList.cheats.empty()) cheatList.apply(emu->memory->ram); ++frame; }
        emu->stepInstruction();
    }
    emu->debuggerPaused = false;        // a trigger on the way (the logic analyser) is not a stop
    drainAudio();
    audioOut.clear();
    render();
    char b[96];
    std::snprintf(b, sizeof b, "Back to instruction %llu (PC &%04X)", (unsigned long long)target, emu->cpu->pc & 0xffff);
    timelineMessage = b;
    return true;
}
// ============================================================== input movies
uint64_t EmuHost::movieStateHash() const {
    uint64_t h = 1469598103934665603ull;
    auto word = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; h ^= h >> 29; };
    const Bytes& ram = emu->memory->ram;
    for (size_t i = 0; i + 8 <= ram.size(); i += 8) { uint64_t v; std::memcpy(&v, ram.data() + i, 8); word(v); }
    const Z80& z = *emu->cpu;
    for (int v : { z.a, z.f, z.b, z.c, z.d, z.e, z.h, z.l, z.ix, z.iy, z.sp, z.pc, z.i, (int)z.iff1, (int)z.halted }) word((uint64_t)v);
    for (int v : { emu->crtc->horizontal, emu->crtc->vertical, emu->crtc->raster, emu->gateArray->mode }) word((uint64_t)v);
    word((uint64_t)(emu->machineCycles - movieStartCycles));
    word(emu->instructions - movieStartAt);
    return h;
}
void EmuHost::moviePowerOn() {
    // the reset, the drives and the tape as they are at a power-on: the same every time
    const bool was = applyingInput;
    applyingInput = true;                 // none of this is an input of the movie
    paused = false;
    emu->reset();
    if (emu->fdc) emu->fdc->powerOn();
    if (CPCTapeDrive* d = tapeDeck()) { d->stop(); d->rewind(); }
    applyingInput = was;
    typed.clear(); typedAt = 0; typedPhase = 0;
    timelineEpoch += 1;                   // a new start: the timeline begins again here
    timelineClear();
    timelineSetup = timelineFingerprint();
    timelineEpochSeen = timelineEpoch;
    movieStartAt = emu->instructions;
    movieStartCycles = emu->machineCycles;
    movieStartHash = movieStateHash();
}
bool EmuHost::movieRecord(std::string& why) {
    if (!booted()) { why = "No machine is running."; return false; }
    movieStop();
    moviePowerOn();
    movieEvents.clear(); movieChecks.clear(); movieFrames = 0;
    movieRec = true;
    movieStatus = "Recording a movie from power-on";
    status = movieStatus;
    return true;
}
static std::string movieHex(const std::string& s) {
    if (s.empty()) return "-";
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}
static std::string movieUnhex(const std::string& s) {
    if (s == "-") return "";
    std::string o;
    for (size_t i = 0; i + 1 < s.size(); i += 2) o += (char)std::strtol(s.substr(i, 2).c_str(), nullptr, 16);
    return o;
}
bool EmuHost::movieSave(const std::string& path, std::string& why) {
    if (!movieRec) { why = "No movie is being recorded."; return false; }
    movieEndAt = emu->instructions;
    std::string text = "CPCSE-MOVIE 1\n";
    char b[512];
    text += "setup " + movieHex(timelineFingerprint()) + "\n";
    std::snprintf(b, sizeof b, "start %016llx\nend %llu\n", (unsigned long long)movieStartHash, (unsigned long long)(movieEndAt - movieStartAt));
    text += b;
    for (const InputEvent& e : movieEvents) {
        std::snprintf(b, sizeof b, "e %llu %d %d %d %d %.17g %.17g ", (unsigned long long)(e.at - movieStartAt), (int)e.kind, e.a, e.b, e.c, e.x, e.y);
        text += b + movieHex(e.code) + " " + movieHex(e.text) + "\n";
    }
    for (const MovieCheck& c : movieChecks) {
        std::snprintf(b, sizeof b, "c %llu %016llx\n", (unsigned long long)(c.at - movieStartAt), (unsigned long long)c.hash);
        text += b;
    }
    {
        std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        if (!f) { why = "Could not write " + path; return false; }
        f.write(text.data(), (std::streamsize)text.size());
        f.close();
        if (!f) { why = "Could not write " + path + " (disc full?)"; return false; }
    }
    movieRec = false;
    char s[160];
    std::snprintf(s, sizeof s, "Movie saved: %.1f s, %zu inputs", (double)(emu->machineCycles - movieStartCycles) / 4000000.0, movieEvents.size());
    movieStatus = s;
    status = s;
    return true;
}
bool EmuHost::moviePlay(const std::string& path, std::string& why) {
    if (!booted()) { why = "No machine is running."; return false; }
    bool ok = false;
    const Bytes raw = readFile(path, ok);
    if (!ok) { why = "Could not read " + path; return false; }
    std::istringstream in(std::string(raw.begin(), raw.end()));
    std::string line, setup;
    unsigned long long startHash = 0, end = 0;
    std::vector<InputEvent> events;
    std::vector<MovieCheck> checks;
    if (!std::getline(in, line) || line.rfind("CPCSE-MOVIE 1", 0) != 0) { why = "Not a movie of this program."; return false; }
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string tag;
        l >> tag;
        if (tag == "setup") { std::string h; l >> h; setup = movieUnhex(h); }
        else if (tag == "start") l >> std::hex >> startHash >> std::dec;
        else if (tag == "end") l >> end;
        else if (tag == "e") {
            InputEvent e; unsigned long long at = 0; int kind = 0; std::string code, text;
            l >> at >> kind >> e.a >> e.b >> e.c >> e.x >> e.y >> code >> text;
            if (!l) { why = "The movie's file is damaged."; return false; }
            e.at = at; e.kind = (InputEvent::Kind)kind; e.code = movieUnhex(code); e.text = movieUnhex(text);
            events.push_back(e);
        } else if (tag == "c") {
            unsigned long long at = 0, hash = 0;
            l >> at >> std::hex >> hash >> std::dec;
            checks.push_back({ at, hash });
        }
    }
    if (setup != timelineFingerprint()) {
        why = "This movie was made with another machine or other media. It needs: " + setup + "  (now: " + timelineFingerprint() + ")";
        return false;
    }
    movieStop();
    moviePowerOn();
    const bool sameStart = movieStartHash == startHash;
    for (InputEvent& e : events) e.at += movieStartAt;
    for (MovieCheck& c : checks) c.at += movieStartAt;
    inputLog = std::move(events);
    replayAt = 0;
    emu->inputDueAt = inputLog.empty() ? UINT64_MAX : inputLog[0].at;
    movieEndAt = movieStartAt + end;
    liveEnd = movieEndAt;
    movieChecks = std::move(checks);
    movieCheckAt = 0; movieChecked = 0; movieOutOfStep = false; movieFrames = 0;
    moviePlay_ = true;
    movieStatus = sameStart ? "Playing a movie" : "Playing a movie (the machine at power-on is not the same as when it was made)";
    status = movieStatus;
    return true;
}
void EmuHost::movieStop() {
    if (movieRec) { movieRec = false; movieStatus = "Movie recording stopped (not saved)"; }
    if (moviePlay_) { moviePlay_ = false; movieStatus = "Movie stopped"; }
}
void EmuHost::movieFrameEnd() {
    if (!movieRec && !moviePlay_) return;
    if (paused || emu->debuggerPaused) return;            // a stop part-way through a frame: no check here
    const uint64_t at = emu->instructions;
    char s[200];
    if (movieRec) {
        if (++movieFrames % 50 == 0) movieChecks.push_back({ at, movieStateHash() });
        std::snprintf(s, sizeof s, "Recording a movie: %.0f s, %zu inputs", (double)(emu->machineCycles - movieStartCycles) / 4000000.0, movieEvents.size());
        movieStatus = s;
        return;
    }
    // playing: every 50th whole frame, the check made at the same one -- at the same
    // instruction with the same hash, or the run is out of step (a run that is out of step
    // ends its frames at other instructions, so the count is compared as well)
    if (++movieFrames % 50 == 0 && movieCheckAt < movieChecks.size()) {
        const MovieCheck& c = movieChecks[movieCheckAt++];
        if ((c.at != at || movieStateHash() != c.hash) && !movieOutOfStep) {
            movieOutOfStep = true;
            std::snprintf(s, sizeof s, "The movie went out of step %.1f s in", (double)(emu->machineCycles - movieStartCycles) / 4000000.0);
            movieStatus = s;
            status = s;
        } else if (!movieOutOfStep) movieChecked++;
    }
    if (at >= movieEndAt) {
        moviePlay_ = false;
        std::snprintf(s, sizeof s, movieOutOfStep ? "Movie ended: OUT OF STEP (%d checks in step before)" : "Movie ended: in step to the end (%d checks)", movieChecked);
        movieStatus = s;
        status = s;
        return;
    }
    if (!movieOutOfStep) {
        std::snprintf(s, sizeof s, "Playing a movie: %.0f of %.0f s, in step (%d checks)", (double)(emu->machineCycles - movieStartCycles) / 4000000.0,
                      (double)(movieEndAt - movieStartAt) / std::max(1.0, (double)(at - movieStartAt) / std::max(1.0, (double)(emu->machineCycles - movieStartCycles) / 4000000.0)),
                      movieChecked);
        movieStatus = s;
    }
}

bool EmuHost::timelineStepBack() {
    if (!emu || emu->instructions == 0) return false;
    return timelineGoTo(emu->instructions - 1);
}
bool EmuHost::timelineFrameBack() {
    if (!emu) return false;
    const uint64_t now = emu->instructions;
    uint64_t target = UINT64_MAX;
    for (auto it = frameStarts.rbegin(); it != frameStarts.rend(); ++it) if (*it < now) { target = *it; break; }
    if (target == UINT64_MAX) { timelineMessage = "No earlier frame is recorded."; return false; }
    return timelineGoTo(target);
}
bool EmuHost::timelineRewind(double seconds) {
    if (!emu || frameStarts.empty()) { timelineMessage = "No earlier frame is recorded."; return false; }
    const uint64_t now = emu->instructions;
    long idx = (long)(std::upper_bound(frameStarts.begin(), frameStarts.end(), now) - frameStarts.begin()) - 1;
    idx -= std::max(1L, (long)std::lround(seconds * 50.0));
    idx = std::max(0L, idx);
    uint64_t target = frameStarts[(size_t)idx];
    if (!snapshots.empty()) target = std::max(target, snapshots.front()->instructions);
    return timelineGoTo(target);
}
// The recorded time before now, newest stretch first, each replayed from its snapshot:
// beforeStep asked before every instruction, and (watchStart >= 0) every write to
// watchStart..watchEnd marked. lastHit: the last instruction before now so marked. The
// machine is left wherever the search stopped (the callers go back to a point after it).
bool EmuHost::timelineSearch(const std::function<void(uint64_t& hit)>& beforeStep, int watchStart, int watchEnd, uint64_t& lastHit) {
    const uint64_t now = emu->instructions;
    lastHit = UINT64_MAX;
    auto keepHook = emu->memory->watchHook;
    const bool keepArmed = emu->memory->watchArmed;
    for (int i = (int)snapshots.size() - 1; i >= 0 && lastHit == UINT64_MAX; i--) {
        const uint64_t from = snapshots[(size_t)i]->instructions;
        if (from >= now) continue;
        const uint64_t to = i + 1 < (int)snapshots.size() ? std::min(now, snapshots[(size_t)i + 1]->instructions) : now;
        snapshots[(size_t)i]->restore(*emu);
        replayAt = (size_t)(std::lower_bound(inputLog.begin(), inputLog.end(), from,
                                             [](const InputEvent& e, uint64_t v) { return e.at < v; }) - inputLog.begin());
        emu->inputDueAt = replayAt < inputLog.size() ? inputLog[replayAt].at : UINT64_MAX;
        auto frame = std::lower_bound(frameStarts.begin(), frameStarts.end(), from);
        uint64_t hit = UINT64_MAX;
        if (watchStart >= 0) {
            // the step that writes: GX4000::instructions counts it at its start, so the
            // machine before it is instructions - 1
            emu->memory->watchHook = [this, &hit, watchStart, watchEnd](int address, int, bool write) {
                if (write && address >= watchStart && address <= watchEnd) hit = emu->instructions - 1;
            };
            emu->memory->watchArmed = true;
        }
        while (emu->instructions < to) {
            while (frame != frameStarts.end() && *frame < emu->instructions) ++frame;
            if (frame != frameStarts.end() && *frame == emu->instructions) { if (!cheatList.cheats.empty()) cheatList.apply(emu->memory->ram); ++frame; }
            if (beforeStep) beforeStep(hit);
            emu->stepInstruction();
        }
        emu->memory->watchHook = keepHook;
        emu->memory->watchArmed = keepArmed;
        if (hit != UINT64_MAX && hit < now) lastHit = hit;
    }
    emu->debuggerPaused = false;
    return lastHit != UINT64_MAX;
}
bool EmuHost::timelineRunBack() {
    if (!booted() || snapshots.empty()) { timelineMessage = "The timeline has no snapshot yet."; return false; }
    if (!emu->breakpoints || emu->breakpoints->empty()) { timelineMessage = "There is no breakpoint to run back to."; return false; }
    const uint64_t now = emu->instructions;
    uint64_t lastHit = UINT64_MAX;
    // before each instruction, as the debugger asks going forward: the PC on a breakpoint
    // (its condition is not asked: that would count hits and change the debugger's state)
    const bool found = timelineSearch([this](uint64_t& hit) {
        if (emu->breakpoints->count(emu->cpu->pc & 0xffff)) hit = emu->instructions;
    }, -1, -1, lastHit);
    if (!found) { timelineGoTo(now); timelineMessage = "No breakpoint was hit in the recorded time."; return false; }
    timelineGoTo(lastHit);
    char b[96];
    std::snprintf(b, sizeof b, "Back to the last breakpoint hit: &%04X", emu->cpu->pc & 0xffff);
    timelineMessage = b;
    return true;
}
bool EmuHost::timelineLastWrite(int start, int end) {
    if (!booted() || snapshots.empty()) { timelineMessage = "The timeline has no snapshot yet."; return false; }
    start &= 0xffff; end &= 0xffff;
    if (end < start) std::swap(start, end);
    const uint64_t now = emu->instructions;
    uint64_t lastHit = UINT64_MAX;
    const bool found = timelineSearch(nullptr, start, end, lastHit);
    if (!found) { timelineGoTo(now); timelineMessage = "Nothing wrote there in the recorded time."; return false; }
    timelineGoTo(lastHit);
    char b[128];
    std::snprintf(b, sizeof b, "The last write to &%04X-&%04X: the instruction at &%04X", start, end, emu->cpu->pc & 0xffff);
    timelineMessage = b;
    return true;
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

bool EmuHost::basicProgramEnd(int& end, bool& basic10) const {
    if (!emu || !emu->memory || emu->memory->ram.size() < 0x10000) return false;
    const Bytes& ram = emu->memory->ram;
    auto word = [&](int a) { return ram[(size_t)a] | ram[(size_t)a + 1] << 8; };
    int a = BASIC_PROGRAM_START;
    for (int lines = 0; lines < 20000; lines++) {
        const int len = word(a);
        if (len == 0) break;
        if (len < 5 || a + len >= 0xa000) return false;   // not a program
        a += len;
    }
    end = a + 2;
    for (bool b10 : { false, true })
        if (word(basicEndPointers(b10)) == end) { basic10 = b10; return true; }
    return false;
}

bool EmuHost::injectBasic(const Bytes& program, std::string& why) {
    timelineEdited();
    int end = 0;
    bool basic10 = false;
    if (!basicProgramEnd(end, basic10)) {
        why = "BASIC is not at its Ready prompt (its pointers do not hold the end of the program in memory).";
        return false;
    }
    const int newEnd = BASIC_PROGRAM_START + (int)program.size();
    if (program.size() < 2 || newEnd >= 0xa000) { why = "The program is too long for BASIC's memory."; return false; }
    Bytes& ram = emu->memory->ram;
    ram[BASIC_PROGRAM_START - 1] = 0;
    std::copy(program.begin(), program.end(), ram.begin() + BASIC_PROGRAM_START);
    // the variables, the arrays and free space start where the program ends: none yet
    for (int k = 0; k < 4; k++) {
        ram[(size_t)(basicEndPointers(basic10) + 2 * k)] = (uint8_t)(newEnd & 0xff);
        ram[(size_t)(basicEndPointers(basic10) + 2 * k + 1)] = (uint8_t)(newEnd >> 8);
    }
    return true;
}

void EmuHost::runFrame() {
    if (!booted() || paused) { audioOut.clear(); return; }
    framesRun++;
    // the advanced CRT filter's signal stage spreads the colour itself
    if (video) video->chromaSpread = !(crtFilter.enabled && crtFilter.signal);
    timelineFrameStart();
    if (typedAt < typed.size() && emu->keyboard && !moviePlay_) {   // typeText's queue: a key at a time
        const char c = typed[typedAt];
        if (typedPhase == 0 && !cpcTypeKey(*emu->keyboard, c, true)) typedPhase = 7;   // no key types it: skipped
        else if (typedPhase == 4) cpcTypeKey(*emu->keyboard, c, false);
        if (++typedPhase == 8) {
            typedPhase = 0;
            if (++typedAt == typed.size()) { typed.clear(); typedAt = 0; }
        }
    }
    for (int u = 0; u < 2; u++) if (diskDirty[u] && framesRun - diskLastWrite[u] > 50) flushDisk(u);   // a second idle
    if (!cheatList.cheats.empty()) cheatList.apply(emu->memory->ram);   // the freezes, every frame
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
    timelineFrameEnd();
    movieFrameEnd();
    drainAudio();
    // recording: every frame's picture, so each one is drawn now
    if (recorder.active() && video) {
        render();
        recorder.addFrame(recordPicture(), audioOut.data(), audioOut.size());
    }
}

// ============================================================== recording
bool EmuHost::startRecording(const std::string& path, std::string& why) {
    if (!booted() || !video) { why = "Nothing to record: no machine."; return false; }
    std::string ext = std::filesystem::u8path(path).extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    const MediaRecorder::Format f = ext == ".wav" ? MediaRecorder::Format::Wav
                                  : ext == ".mp4" || recordFormat == "h264" ? MediaRecorder::Format::H264
                                  : recordFormat == "zmbv" ? MediaRecorder::Format::Zmbv : MediaRecorder::Format::Mjpeg;
    recorder.ffmpeg = ffmpegPath.empty() ? "ffmpeg" : ffmpegPath;
    // two pictures side by side: "both", or as shown when both are on the screen: the
    // GFX9000's beside the CPC's, or in its own GFX9000 screen window
    const bool wide = f != MediaRecorder::Format::Wav &&
                      (recordSource == "both" || (recordSource == "shown" && v9990Enabled && (gfx9000Monitor == "beside" || gfx9000Monitor == "window")));
    recordWidth = video->width * (wide ? 2 : 1);
    recordHeight = video->height;
    if (recordSource == "screen" && f != MediaRecorder::Format::Wav) {
        // the picture as drawn now, even sizes (H.264's 4:2:0 wants them); a window too
        // small for one: the CPC's own size
        recordWidth = screenPictureW >= 64 ? screenPictureW & ~1 : video->width;
        recordHeight = screenPictureH >= 64 ? screenPictureH & ~1 : video->height;
        screenCapture.assign((size_t)recordWidth * recordHeight, 0xff000000u);
    }
    // one CPC frame: 312 lines of 64 usec
    if (!recorder.start(path, f, recordWidth, recordHeight, sampleRate, 19968.0)) { why = "Could not record: " + recorder.error; return false; }
    status = "Recording to " + std::filesystem::u8path(path).filename().u8string();
    return true;
}

// The recorder's picture for this frame, recordWidth x recordHeight: the CPC's as it is, the
// GFX9000's (or the Video9000's mix) fitted at 4:3 into a CPC-sized box with black bars, or the
// two side by side.
const uint32_t* EmuHost::recordPicture() {
    if (recordSource == "screen") {
        if (screenCapture.size() != (size_t)recordWidth * recordHeight) screenCapture.assign((size_t)recordWidth * recordHeight, 0xff000000u);
        return screenCapture.data();
    }
    const int cw = video->width, ch = video->height;
    const bool wide = recordWidth >= cw * 2;
    std::string what = recordSource;
    if (what == "shown") {
        if (wide) what = "both";
        else if (v9990Enabled && gfx9000Monitor == "video9000") what = "mix";
        else what = gfx9000OnMainScreen() ? "gfx" : "cpc";
    }
    if (what == "cpc" && !wide) return video->pixels.data();
    recordCanvas.assign((size_t)recordWidth * recordHeight, 0xff000000u);
    auto copyCpc = [&](int x0) {
        for (int y = 0; y < ch && y < recordHeight; y++)
            std::copy(video->pixels.begin() + (size_t)y * cw, video->pixels.begin() + (size_t)y * cw + cw, recordCanvas.begin() + (size_t)y * recordWidth + x0);
    };
    auto fit = [&](const uint32_t* src, int sw, int sh, int x0) {
        if (!src || sw <= 0 || sh <= 0) return;
        const int bh = ch, bw = std::min(cw, ch * 4 / 3);            // a 4:3 box in the CPC's frame
        const int bx = x0 + (cw - bw) / 2;
        for (int y = 0; y < bh; y++) {
            const uint32_t* row = src + (size_t)(y * sh / bh) * sw;
            uint32_t* out = &recordCanvas[(size_t)y * recordWidth + bx];
            for (int x = 0; x < bw; x++) out[x] = row[x * sw / bw] | 0xff000000u;
        }
    };
    std::vector<uint32_t> mixed;
    auto gfx = [&](int x0) {
        if (v9990Enabled && gfx9000Monitor == "video9000") {
            int mw = 0, mh = 0;
            if (video9000Picture(mixed, mw, mh)) fit(mixed.data(), mw, mh, x0);
        } else if (const V9990Picture* pic = v9990Picture()) {
            fit(pic->pixels.data(), pic->width, pic->height, x0);
        }
    };
    if (what == "both" || wide) { copyCpc(0); gfx(cw); }
    else if (what == "mix" || what == "gfx") gfx(0);
    else copyCpc(0);
    return recordCanvas.data();
}

void EmuHost::stopRecording() {
    if (!recorder.active()) return;
    if (recorder.format() == MediaRecorder::Format::H264) status = "Finishing the MP4 (ffmpeg)...";
    recorder.stop();
    char b[160];
    std::snprintf(b, sizeof b, "Recorded %.1f s (%.1f MB%s)", recorder.seconds(), recorder.bytesWritten() / (1024.0 * 1024.0),
                  recorder.filesWritten() > 1 ? (", " + std::to_string(recorder.filesWritten()) + " files").c_str() : "");
    status = recorder.error.empty() ? b : "Recording stopped: " + recorder.error;
}

void EmuHost::setRasterOverlay(bool on) {
    rasterOverlay = on;
    if (!emu) return;
    emu->ioOverlay = on;
    void (*note)(void*, int, int) = [](void* m, int address, int value) {
        static_cast<GX4000*>(m)->noteIo(0x6400 + (address - 0x2400), value, RasterLine::IO_PLUS_PALETTE);
    };
    emu->asic->onPaletteWrite = on ? note : nullptr;
    emu->asic->onPaletteWriteContext = emu;
    if (video) video->drawIoMarks = on;
}

void EmuHost::render() {
    if (!video || !booted()) return;
    pictureChanged = true;
    // The beam renderer covers only the classic monitor path; Plus/ASIC keeps the
    // legacy compositor (sprites etc.). beamMode suppresses render() only when the
    // beam is actually driving the frame.
    video->beamMode = beamRenderer && emu && emu->plusHardware == false;
    if (!video->beamMode) video->render();
}

const std::vector<EmuHost::SoundLevel>& EmuHost::soundLevels() {
    static const std::vector<SoundLevel> list = {
        { "ay", "CPC (AY)", &EmuHost::volAy },
        { "dac", "DigiBlaster / AmDrum", &EmuHost::volDac },
        { "speech", "Speech", &EmuHost::volSpeech },
        { "ls3mp3", "LambdaSpeak 3 MP3", &EmuHost::volLs3Mp3 },
        { "sf3", "SYMBiFACE III", &EmuHost::volSf3 },
        { "playcity", "PlayCity", &EmuHost::volPlayCity },
        { "opl4", "OPL4", &EmuHost::volOpl4 },
    };
    return list;
}

bool EmuHost::soundFitted(const char* key) const {
    const std::string k = key;
    if (!emu) return false;
    if (k == "ay") return true;
    if (k == "dac") return emu->dac && (emu->dac->mode == "digiblaster" || emu->dac->mode == "amdrum");
    if (k == "speech") return emu->speech && emu->speech->kind != SpeechSynth::Kind::None;
    if (k == "ls3mp3") return emu->speech && emu->speech->kind == SpeechSynth::Kind::LambdaSpeak3;
    if (k == "sf3") return emu->sf3 && emu->sf3->enabled;
    if (k == "playcity") return emu->playcity && emu->playcity->enabled;
    if (k == "opl4") return emu->opl4 && emu->opl4->enabled;
    return false;
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
    const double oplGain = (double)masterVolume * 0.75 * volOpl4;
    audioOut.reserve((size_t)available * 2);
    // The PlayCity's two YMZ294: the left chip on the left, the right on the right, each
    // folded to mono, made by the same stretch of CPC time as the AY's samples.
    PlayCity* pc = emu->playcity && emu->playcity->enabled ? emu->playcity : nullptr;
    const bool dacOn = emu->dac && (emu->dac->mode == "digiblaster" || emu->dac->mode == "amdrum");
    // The speech synthesiser, mono, from its own host-rate queue.
    SpeechSynth* sp = emu->speech && emu->speech->kind != SpeechSynth::Kind::None ? emu->speech : nullptr;
    const bool sf3Sound = emu->sf3 && emu->sf3->enabled;
    for (int i = 0; i < available; i++) {
        std::array<double, 2> s = ay->readSample();
        double l = s[0] * gain * volAy, r = s[1] * gain * volAy;
        // The printer-port DACs (DigiBlaster, AmDrum): mono, from their own queue. They were
        // never mixed in -- the DAC filled its queue and nothing read it, so both were silent.
        if (dacOn) { const double v = emu->dac->readSample() * gain * 1.6 * volDac; l += v; r += v; }
        if (sp) {
            const double v = sp->readSample() * gain * 0.9 * volSpeech; l += v; r += v;
            // LambdaSpeak 3's MP3 module, stereo, at line level next to the AY.
            if (sp->kind == SpeechSynth::Kind::LambdaSpeak3) {
                float ml, mr; sp->mp3.readSample(ml, mr);
                l += ml * masterVolume * 26000.0 * volLs3Mp3; r += mr * masterVolume * 26000.0 * volLs3Mp3;
            }
        }
        // The SYMBiFACE III's VS1053 (SymAmp's MP3, the SE-ONE player) and its buzzer, stereo.
        if (sf3Sound) {
            float ml, mr; emu->sf3->audio->readSample(ml, mr);
            l += ml * masterVolume * 26000.0 * volSf3; r += mr * masterVolume * 26000.0 * volSf3;
        }
        if (pc) {
            const bool haveL = pc->left.sampleReadIndex < (int)pc->left.sampleQueue.size();
            const bool haveR = pc->right.sampleReadIndex < (int)pc->right.sampleQueue.size();
            if (haveL) { auto v = pc->left.readSample(); l += (v[0] + v[1]) * 0.5 * gain * volPlayCity; }
            if (haveR) { auto v = pc->right.readSample(); r += (v[0] + v[1]) * 0.5 * gain * volPlayCity; }
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

void EmuHost::setKeyChar(SDL_Scancode sc, const std::string& text) {
    if (!emu->keyboard || liveInputBlocked()) return;
    std::string code = sdlScancodeToBrowserCode(sc);
    if (code.empty()) return;
    // the character's CPC key (with or without SHIFT, as it types it), else the key's place
    emu->keyboard->setKey(code, true, text, sdlKeyLocation(sc));
    const int idx = (int)sc;
    if (idx >= 0 && idx < (int)keyHeld.size() && !keyHeld[idx]) { keyHeld[idx] = true; machineSounds.key(true, MachineSounds::Key::Normal); }
}
void EmuHost::pollPads() {
    if (!emu || !emu->keyboard || liveInputBlocked()) return;   // a movie plays its own
    pads.captureStep();
    for (int p = 0; p < 2; p++) {
        int lines = pads.lines(p);
        // a Plus in analogue mode: the stick to the ADC, only the fire buttons digital
        int x = 32, y = 32;
        if (pads.analogue[p] && emu->plusHardware && pads.stick(p, x, y)) {
            lines &= 0x30;
            if (analogue(p * 2) != x) setAnalogue(p * 2, x);
            if (analogue(p * 2 + 1) != y) setAnalogue(p * 2 + 1, y);
        }
        for (int bit = 0; bit < PAD_LINES; bit++) emu->keyboard->setJoystick(bit, (lines >> bit & 1) != 0, "gamepad", p);
    }
}

void EmuHost::setKey(SDL_Scancode sc, bool pressed) {
    if (!emu->keyboard || liveInputBlocked()) return;
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

// The chip swap takes effect at the next instruction; a real swap needs the power off, but
// nothing in the Z80's state depends on the process, so no reset is forced.
void EmuHost::setZ80Cmos(bool cmos) {
    z80Cmos = cmos;
    if (emu && emu->cpu) emu->cpu->cmos = z80Cmos;
}

// The front knobs, with the output intensity of the set plugged in now.
static TubeKnobs knobsForVideo(const TubeKnobs& front, const std::array<int, 3>& intensity) {
    TubeKnobs k = front;
    k.intensity = intensity;
    return k;
}
void EmuHost::setMonitorKnobs(const TubeKnobs& knobs) {
    monitorKnobs = knobs;
    monitorKnobs.brightness = std::max(0, std::min(100, knobs.brightness));
    monitorKnobs.contrast = std::max(0, std::min(100, knobs.contrast));
    if (video) video->setMonitorKnobs(knobsForVideo(monitorKnobs, activeMonitorIntensity()));
}
std::string EmuHost::activeMonitorSetId() const {
    if (emu && emu->monitorRenderer && emu->monitorRenderer->model) return emu->monitorRenderer->model->id;
    return monitorSetId.empty() ? std::string("ctm644") : monitorSetId;
}
std::array<int, 3> EmuHost::activeMonitorIntensity() const {
    auto it = monitorIntensity.find(activeMonitorSetId());
    return it != monitorIntensity.end() ? it->second : TubeKnobs().intensity;
}
void EmuHost::setMonitorIntensity(const std::array<int, 3>& rgb) {
    std::array<int, 3> v;
    for (size_t k = 0; k < 3; k++) v[k] = std::max(0, std::min(200, rgb[k]));
    if (v == TubeKnobs().intensity) monitorIntensity.erase(activeMonitorSetId());
    else monitorIntensity[activeMonitorSetId()] = v;
    if (video) video->setMonitorKnobs(knobsForVideo(monitorKnobs, activeMonitorIntensity()));
}

void EmuHost::setMonitorHolds(int hHold, int vHold) {
    monitorHHold = hHold;
    monitorVHold = vHold;
    applyMonitorSet();
}

void EmuHost::applyMonitorSet() {
    if (!emu) return;
    // The two hold controls go to the monitor chip. It takes them at its next field and
    // pulls in by itself, so the chip is not reset.
    emu->monitorRenderer->hHold = monitorHHold;
    emu->monitorRenderer->vHold = monitorVHold;
    // No set chosen is nullptr, not the shipped set: the core then re-takes the machine's
    // own at every reset, so a later model change gets its own monitor.
    emu->setMonitorModel(monitorSetId.empty() ? nullptr : monitorModelFor(monitorSetId));
    if (video) {
        video->setMonitorModel(emu->monitorRenderer->model);
        video->setMonitorMode(monitorMode);
        video->setMonitorKnobs(knobsForVideo(monitorKnobs, activeMonitorIntensity()));   // this set's intensity
    }
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
