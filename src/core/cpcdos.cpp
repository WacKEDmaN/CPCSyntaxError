// CPCSyntaxError — CPCDOS virtual-drive bridge.
#include "cpcdos.h"
#include "emulator.h"
#include "gate_array.h"
#include "z80.h"
#include "memory.h"
#include "asic.h"
#include <regex>

namespace cpcse {

static const int ROM_WORK_OFFSET = 0x0800;
static const int ROM_WORK_SIZE = 0x4000 - ROM_WORK_OFFSET;

static std::string upperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static std::vector<std::string> splitStr(const std::string& s, char sep) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur); return out;
}

static std::string parentPath(const std::string& path);
static std::string normalisePath(const std::string& pathIn) {
    std::string value = pathIn.empty() ? "/" : pathIn;
    value = std::regex_replace(value, std::regex("\\\\+"), "/");
    value = trimStr(value);
    if (std::regex_search(value, std::regex("^[A-Za-z]:"))) value = value.substr(2);
    if (value.empty() || value[0] != '/') value = "/" + value;
    std::vector<std::string> parts;
    for (const std::string& raw : splitStr(value, '/')) {
        std::string part = trimStr(raw);
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else parts.push_back(upperStr(std::regex_replace(part, std::regex("[\\x00-\\x1f<>:\"|?*]"), "_")));
    }
    std::string joined;
    for (size_t i = 0; i < parts.size(); i++) { joined += parts[i]; if (i + 1 < parts.size()) joined += "/"; }
    return "/" + joined;
}
static std::string parentPath(const std::string& path) {
    std::string full = normalisePath(path); size_t idx = full.rfind('/');
    return (idx == std::string::npos || idx == 0) ? "/" : full.substr(0, idx);
}
static std::string toCpcChar(int byte) { return byte == 0xa3 ? std::string("~") : std::string(1, (char)(byte & 0xff)); }
static std::string cpcTrimName(const std::string& nameIn) {
    std::string out;
    for (unsigned char c : nameIn) {
        if (c == '"' || c == '\'') continue;
        if (c == 0xa3) { out += '~'; continue; }
        if (c == 0xef || c == 0xeb) continue;
        out += (char)c;
    }
    return upperStr(trimStr(out));
}
static bool hasWildcard(const std::string& pattern) { return pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos; }

static std::string shortenName(const std::string& fileIn, const std::vector<std::string>& previousNames, bool cat) {
    std::string value;
    for (unsigned char c : fileIn) value += (c == 0xa3) ? '~' : (char)c; // replace £ with ~
    if (cat) { std::string t; for (char c : value) t += (c == '~') ? (char)0xa3 : c; value = t; } // ~ -> £
    const std::string marker = "\x1f\x1f\x1f\x1f";
    { size_t dot = value.find('.'); if (dot != std::string::npos) value = value.substr(0, dot) + marker + value.substr(dot + 1); }
    int index = 1;
    if (value.find(marker) != std::string::npos) {
        std::vector<std::string> spl;
        { size_t at = value.find(marker); spl.push_back(value.substr(0, at)); spl.push_back(value.substr(at + marker.size())); }
        std::string base = spl.size() > 0 ? spl[0] : "";
        std::string ext = (spl.size() > 1 ? spl[1] : "").substr(0, 3);
        if (base.size() > 8) {
            while (base.size() > 6) base.pop_back();
            for (const std::string& prev : previousNames) if (upperStr(prev).find(upperStr(base)) != std::string::npos) index += 1;
            if (index > 9) base.pop_back();
            if (index > 99) base.pop_back();
            base += (cat ? std::string(1, (char)0xa3) : std::string("~")) + std::to_string(index);
        }
        while (base.size() < 8) base += ' ';
        return base + "." + ext;
    }
    if (value.size() > 8) {
        while (value.size() > 6) value.pop_back();
        for (const std::string& prev : previousNames) if (upperStr(prev).find(upperStr(value)) != std::string::npos) index += 1;
        if (index > 9) value.pop_back();
        if (index > 99) value.pop_back();
        value += (cat ? std::string(1, (char)0xa3) : std::string("~")) + std::to_string(index);
    }
    while (value.size() < 12) value += ' ';
    return value;
}

static bool amsdosHeaderValid(const Bytes& bytes) {
    if (bytes.size() < 128) return false;
    int sum = 0;
    for (int i = 0; i < 67; i += 1) sum = (sum + (bytes[i] & 0xff)) & 0xffff;
    int stored = (bytes[67] & 0xff) | ((bytes[68] & 0xff) << 8);
    return stored != 0 && stored == sum;
}
static std::string padEnd(const std::string& s, int len, char c = ' ') { std::string r = s; while ((int)r.size() < len) r += c; return r; }
static Bytes createAmsdosHeader(const std::string& filename, int type, int loadAddress, int length, int entryAddress) {
    Bytes header(128, 0);
    std::string up = upperStr(filename.empty() ? "CPCSE.BIN" : filename);
    size_t dot = up.find('.');
    std::string rawBase = dot != std::string::npos ? up.substr(0, dot) : up;
    std::string rawExt = dot != std::string::npos ? up.substr(dot + 1) : "";
    std::string base = padEnd(std::regex_replace(rawBase.empty() ? "CPCSE" : rawBase, std::regex("[^A-Z0-9_~$!#&-]"), "").substr(0, 8), 8);
    std::string ext = padEnd(std::regex_replace(rawExt.empty() ? "BIN" : rawExt, std::regex("[^A-Z0-9]"), "").substr(0, 3), 3);
    for (int i = 0; i < 8; i += 1) header[1 + i] = (uint8_t)(base[i] & 0xff);
    for (int i = 0; i < 3; i += 1) header[9 + i] = (uint8_t)(ext[i] & 0xff);
    header[17] = 0xff; header[18] = (uint8_t)(type & 0xff);
    header[19] = (uint8_t)(length & 0xff); header[20] = (uint8_t)((unsigned)length >> 8 & 0xff);
    header[21] = (uint8_t)(loadAddress & 0xff); header[22] = (uint8_t)((unsigned)loadAddress >> 8 & 0xff);
    header[23] = 0xff; header[24] = (uint8_t)(length & 0xff); header[25] = (uint8_t)((unsigned)length >> 8 & 0xff);
    header[26] = (uint8_t)(entryAddress & 0xff); header[27] = (uint8_t)((unsigned)entryAddress >> 8 & 0xff);
    header[64] = (uint8_t)(length & 0xff); header[65] = (uint8_t)((unsigned)length >> 8 & 0xff); header[66] = (uint8_t)((unsigned)length >> 16 & 0xff);
    int sum = 0;
    for (int i = 0; i <= 66; i += 1) sum = (sum + header[i]) & 0xffff;
    header[67] = (uint8_t)(sum & 0xff); header[68] = (uint8_t)((unsigned)sum >> 8 & 0xff);
    return header;
}
static Bytes withAmsdosHeader(const Bytes& data, const std::string& filename, int type, int loadAddress, int entryAddress) {
    Bytes header = createAmsdosHeader(filename, type, loadAddress, (int)data.size(), entryAddress);
    Bytes out(128 + data.size());
    for (int i = 0; i < 128; i++) out[i] = header[i];
    for (int i = 0; i < (int)data.size(); i++) out[128 + i] = data[i];
    return out;
}

CpcDos::CpcDos(GX4000* emulator, CpcDosDrive* drive) : emulator(emulator), drive(drive) { enabled = false; reset(); }
void CpcDos::reset() {
    readOpen = false; writeOpen = false; readPos = 0; readHead.assign(128, 0); readStream.clear();
    writeBuffer.clear(); writeStart = 0; writeName = ""; writeAscii = false; fWhere = ROM_WORK_OFFSET; storeIY = 0; eof = false;
}
bool CpcDos::handleEdff(Z80* cpu) {
    if (!enabled || !drive) return false;
    if (cpu->read(cpu->pc) != 0x50 || cpu->read((cpu->pc + 1) & 0xffff) != 0x43 || cpu->read((cpu->pc + 2) & 0xffff) != 0x46) return false;
    execute(cpu);
    cpu->pc = (cpu->pc + 3) & 0xffff;
    return true;
}
int CpcDos::memRead(int address) { return emulator->memory->read(address & 0xffff) & 0xff; }
int CpcDos::memReadRam(int address) {
    GXMemory* memory = emulator->memory;
    address &= 0xffff;
    int slot = (unsigned)address >> 13;
    uint32_t offset = memory->writeMap[slot];
    if (offset == 0xffffffff) {
        int asicAddress = (address - memory->asicRamLocation * 0x2000) & 0x3fff;
        return asicAddress >= 0 && asicAddress < (int)memory->asicRam.size() ? memory->asicRam[asicAddress] : 0xff;
    }
    size_t idx = (size_t)offset + (address & 0x1fff);
    return idx < memory->ram.size() ? memory->ram[idx] : 0xff;
}
void CpcDos::memWrite(int address, int value) { emulator->memory->write(address & 0xffff, value & 0xff); }
std::string CpcDos::readCpcString(int address, int length, bool keepSpaces) {
    std::string out;
    for (int i = 0; i < length; i += 1) { std::string ch = toCpcChar(memRead(address + i)); if (keepSpaces || ch != " ") out += ch; }
    return cpcTrimName(out);
}
void CpcDos::flushRom() { fWhere = ROM_WORK_OFFSET; writeRom(Bytes(ROM_WORK_SIZE, 0)); fWhere = ROM_WORK_OFFSET; }
void CpcDos::writeRom(const Bytes& bytes) {
    if (bytes.empty()) return;
    Bytes& rom = emulator->memory->upperRoms[6];
    if (rom.empty()) return;
    int len = std::min((int)bytes.size(), ROM_WORK_SIZE - (fWhere - ROM_WORK_OFFSET));
    if (len <= 0) return;
    for (int i = 0; i < len; i++) if (fWhere + i < (int)rom.size()) rom[fWhere + i] = bytes[i];
    fWhere += len;
}
void CpcDos::fString(const std::string& text) {
    int len = std::min(255, (int)text.size());
    Bytes bytes(1 + len);
    bytes[0] = (uint8_t)len;
    for (int i = 0; i < len; i += 1) bytes[i + 1] = (uint8_t)(text[i] & 0xff);
    writeRom(bytes);
}
void CpcDos::execute(Z80* cpu) {
    if (drive) drive->noteActivity(220);
    switch (cpu->a & 0xff) {
        case 0: fOpenIn(cpu); break;
        case 1: fOpenOut(cpu); break;
        case 2: fCloseIn(cpu); break;
        case 3: fCloseOut(cpu); break;
        case 4: fRead(cpu, cpu->hl(), cpu->bc()); break;
        case 5: fWrite(cpu, cpu->hl(), cpu->de()); break;
        case 6: cpu->f = eof ? 0 : 1; break;
        case 7: fErase(cpu); break;
        case 8: fRename(cpu); break;
        case 9: fGetCat(cpu); break;
        case 10: fGetDir(cpu); break;
        case 11: fChDir(cpu); break;
        case 12: cpu->iy = storeIY & 0xffff; break;
        case 13: flushRom(); hiddenCat(); cpu->f = 1; break;
        default: cpu->f = 0; break;
    }
}
std::vector<CpcDosRow> CpcDos::hiddenCat() { return drive ? drive->list(drive->cwd) : std::vector<CpcDosRow>(); }
Bytes CpcDos::makeFakeHeader(const Bytes& data, int start, const std::string& name) {
    return withAmsdosHeader(data, name, 2, start, start);
}
void CpcDos::fOpenIn(Z80* cpu) {
    flushRom(); eof = false; readPos = 0; readOpen = false;
    std::string name = readCpcString(cpu->hl(), cpu->b);
    CpcDosFile* file = drive->find(name);
    storeIY = cpu->iy;
    if (!file) { cpu->f = 0x40; CpcDosRow r; r.name = name; fString("\r\n" + catFileField(r, {}, 0) + " not found\r\n"); return; }
    Bytes data = file->data;
    if (!amsdosHeaderValid(data)) data = makeFakeHeader(data, 0, file->name);
    readHead = Bytes(data.begin(), data.begin() + std::min((size_t)128, data.size()));
    if (readHead.size() < 128) readHead.resize(128, 0);
    readStream = data.size() > 128 ? Bytes(data.begin() + 128, data.end()) : Bytes();
    readOpen = true;
    int start = readHead[21] | (readHead[22] << 8);
    for (int i = 0; i < 0x45; i += 1) memWrite((cpu->ix + i) & 0xffff, readHead[i]);
    cpu->setHl(cpu->ix); cpu->setDe(start); cpu->setBc(readHead[24] | (readHead[25] << 8));
    cpu->setAf((cpu->af() & 0x00be) | (readHead[18] << 8) | 1);
}
void CpcDos::fOpenOut(Z80* cpu) {
    if (drive && drive->readOnly) { cpu->f = 0; fString("CPCDOS read-only"); return; }
    flushRom(); writeBuffer.clear(); writeStart = 0; writeName = readCpcString(cpu->hl(), cpu->b, true);
    writeOpen = true; writeAscii = false; eof = false; cpu->f = 1;
}
void CpcDos::fCloseIn(Z80* cpu) { cpu->f = 0; readOpen = false; eof = false; readPos = 0; }
void CpcDos::fCloseOut(Z80* cpu) {
    flushRom();
    if (!writeOpen) { cpu->f = 0; return; }
    std::string name = cpcTrimName(writeName);
    { std::string t; for (char c : name) t += ((unsigned char)c == 0xa3) ? '~' : c; name = t; }
    if (name.find('.') == std::string::npos) name += writeStart == 0x170 ? ".BAS" : ".BIN";
    Bytes body(writeBuffer.begin(), writeBuffer.end());
    Bytes data = writeAscii ? body : withAmsdosHeader(body, name, writeStart == 0x170 ? 0 : 2, writeStart, writeStart == 0x170 ? 0x170 : writeStart);
    if (drive) drive->addFile(upperStr(name), data);
    writeOpen = false; writeBuffer.clear(); cpu->f = 1;
}
void CpcDos::fRead(Z80* cpu, int hl, int bc) {
    if (!readOpen) { cpu->f = 0; return; }
    int len = bc & 0xffff;
    for (int i = 0; i < len; i += 1) {
        if (readPos >= (int)readStream.size()) { eof = true; memWrite(hl + i, 0x1a); }
        else memWrite(hl + i, readStream[readPos++]);
    }
    cpu->f = 1;
}
void CpcDos::fWrite(Z80* cpu, int hl, int de) {
    if (!writeOpen) { cpu->f = 0; return; }
    if (writeBuffer.empty()) writeStart = hl & 0xffff;
    int len = de & 0xffff;
    for (int i = 0; i < len; i += 1) writeBuffer.push_back(memReadRam(hl + i));
    cpu->f = 1;
}
void CpcDos::fErase(Z80* cpu) {
    flushRom(); hiddenCat();
    std::string pattern = readCpcString(cpu->hl(), cpu->b);
    if (drive && drive->readOnly) { cpu->f = 0; fString("CPCDOS read-only"); return; }
    std::vector<CpcDosFile*> matches = drive ? drive->matchFiles(pattern) : std::vector<CpcDosFile*>();
    if (!matches.empty()) {
        for (CpcDosFile* file : matches) drive->deletePath(file->path);
        cpu->f = 1;
        fString(hasWildcard(pattern) ? std::to_string(matches.size()) + " file(s) erased" : pattern + " erased");
    } else { cpu->f = 0; fString(pattern + " not found"); }
}
void CpcDos::fRename(Z80* cpu) {
    flushRom(); hiddenCat();
    std::string newName = readCpcString(cpu->hl(), cpu->b);
    std::string oldName = readCpcString(cpu->de(), cpu->c ? cpu->c : cpu->b);
    if (drive && drive->readOnly) { cpu->f = 0; fString("CPCDOS read-only"); return; }
    std::vector<CpcDosFile*> matches = drive ? drive->matchFiles(oldName) : std::vector<CpcDosFile*>();
    if (hasWildcard(oldName) && matches.size() != 1) { cpu->f = 0; fString(matches.size() ? oldName + " ambiguous" : oldName + " not found"); return; }
    CpcDosFile* source = !matches.empty() ? matches[0] : (drive ? drive->find(oldName) : nullptr);
    bool ok = source ? drive->rename(source->path, newName) : false;
    cpu->f = ok ? 1 : 0;
    if (!ok) fString(oldName + " not found");
}
std::string CpcDos::virtualDosHeader() {
    std::string path = drive ? drive->cwd : "/";
    std::string shown = path == "/" ? "" : std::regex_replace(path, std::regex("^/"), "");
    return "Drive: CPCDOS/\r\n Path: " + shown;
}
std::vector<CpcDosRow> CpcDos::directoryRows() {
    std::vector<CpcDosRow> rows;
    CpcDosRow a; a.kind = "dir"; a.path = drive ? drive->cwd : "/"; a.name = "."; a.dot = true; rows.push_back(a);
    CpcDosRow b; b.kind = "dir"; b.path = parentPath(drive ? drive->cwd : "/"); b.name = ".."; b.dot = true; rows.push_back(b);
    for (auto& r : hiddenCat()) rows.push_back(r);
    return rows;
}
std::string CpcDos::catFileField(const CpcDosRow& row, const std::vector<CpcDosRow>& rows, int index) {
    if (row.dot) return padEnd(row.name == ".." ? ".." : ".", 12);
    std::vector<std::string> prev; for (int i = 0; i < index && i < (int)rows.size(); i++) prev.push_back(rows[i].name);
    std::string file = shortenName(row.name, prev, true);
    while (file.size() < 12) file += ' ';
    return file;
}
std::string CpcDos::dirFileField(const CpcDosRow& row) {
    if (row.dot) return padEnd(row.name == ".." ? ".." : ".", 12);
    std::string file; for (unsigned char c : row.name) file += (c == 0xa3) ? '~' : (char)c;
    const std::string marker = "\x1f\x1f\x1f\x1f";
    if (row.kind != "dir") { size_t dot = file.find('.'); if (dot != std::string::npos) file = file.substr(0, dot) + marker + file.substr(dot + 1); }
    if (file.find(marker) != std::string::npos) {
        size_t at = file.find(marker);
        std::string base = file.substr(0, at);
        std::string ext = file.substr(at + marker.size());
        if (base.size() > 8) { while (base.size() > 6) base.pop_back(); base += std::string(1, (char)0xa3) + "1"; }
        while (base.size() < 8) base += ' ';
        while (ext.size() > 3) ext.pop_back();
        file = base + "." + ext;
    } else {
        if (file.size() > 8) { while (file.size() > 6) file.pop_back(); file += std::string(1, (char)0xa3) + "1"; }
        while (file.size() < 12) file += ' ';
    }
    while (file.size() < 12) file += ' ';
    return file;
}
std::string CpcDos::sizeTextForRow(const CpcDosRow& row) {
    if (row.kind == "dir") return "[dir]";
    long long size = std::max(1LL, (long long)std::ceil((row.size ? row.size : 0) / 1024.0));
    std::string unit = "K";
    if (std::to_string(size).size() > 4) {
        size = size / 1024;
        unit = std::to_string(size).size() > 4 ? "G" : "M";
        if (unit == "G") size = size / 1024;
    }
    std::string s = std::to_string(size); while (s.size() < 4) s = " " + s;
    return s + unit;
}
int CpcDos::screenMode() { return (emulator && emulator->asic) ? emulator->gateArray->mode : 0; }
void CpcDos::fGetCat(Z80* cpu) {
    flushRom();
    std::vector<CpcDosRow> rows = directoryRows();
    fString(virtualDosHeader());
    int total = 0;
    for (int i = 0; i < (int)rows.size(); i += 1) {
        const CpcDosRow& row = rows[i];
        std::string file = catFileField(row, rows, i) + " ";
        std::string len = sizeTextForRow(row);
        if (row.kind != "dir") total += (int)std::max(1LL, (long long)std::ceil((row.size ? row.size : 0) / 1024.0));
        if (fWhere + (int)file.size() > ROM_WORK_SIZE - 34) { fString("*ERR* DIR too long\n"); break; }
        fString(file);
        fString(len);
    }
    int files = (int)rows.size();
    if ((files % 2) == 1 && screenMode() != 0) fString("  \r\n");
    fString("  \r\n");
    std::string fc = std::to_string(files); while (fc.size() < 6) fc = " " + fc;
    fString(fc + " file(s)  ");
    fString("  Total " + std::to_string(total) + "K");
    cpu->f = 1;
}
void CpcDos::fGetDir(Z80* cpu) {
    flushRom();
    hiddenCat();
    std::string addon; { std::string a = readCpcString(cpu->hl(), cpu->b); for (unsigned char c : a) addon += (c == 0xa3) ? '~' : (char)c; }
    bool filter = addon.size() > 0;
    std::vector<CpcDosRow> rows = directoryRows();
    fString(virtualDosHeader());
    int files = 0, total = 0;
    for (int i = 0; i < (int)rows.size(); i += 1) {
        const CpcDosRow& row = rows[i];
        std::string file = dirFileField(row);
        int fsize = row.kind == "dir" ? 0 : (int)std::max(1LL, (long long)std::ceil((row.size ? row.size : 0) / 1024.0));
        if (fWhere + (int)file.size() > ROM_WORK_SIZE - 34) { fString("*ERR* DIR too long\n"); break; }
        if (filter) {
            std::string filtername = std::regex_replace(file, std::regex(" "), "");
            addon = std::regex_replace(addon, std::regex("\\*"), "xxxxxxxxxxxxxxxxxx");
            if (addon.find("xxxxxxxxxxxxxxxxxx") != std::string::npos) {
                std::vector<std::string> add; { std::string tmp = addon; std::string tok; const std::string sep = "xxxxxxxxxxxxxxxxxx"; size_t pos = 0, f2; while ((f2 = tmp.find(sep, pos)) != std::string::npos) { add.push_back(tmp.substr(pos, f2 - pos)); pos = f2 + sep.size(); } add.push_back(tmp.substr(pos)); }
                int addcount = 0;
                for (size_t k = 0; k < add.size(); k += 1) if (upperStr(filtername).find(upperStr(add[k])) != std::string::npos) addcount += 1;
                if (addcount != (int)add.size()) continue;
            } else if (upperStr(filtername) != upperStr(addon)) continue;
        }
        file += "  ";
        int mode = screenMode();
        if (mode == 1 && (files % 2) == 1) file += std::string("\r\n") + (char)8;
        if (mode == 2 && (files % 5) == 4) file += std::string("\r\n") + (char)8;
        if (mode == 0) file += std::string("\r\n") + (char)8;
        total += fsize;
        fString(file);
        files += 1;
    }
    if ((files % 2) == 1) fString("                    ");
    fString("");
    std::string fc = std::to_string(files); while (fc.size() < 6) fc = " " + fc;
    fString(fc + " file(s)      Total " + std::to_string(total) + "K");
    cpu->f = 1;
}
void CpcDos::fChDir(Z80* cpu) {
    flushRom(); hiddenCat();
    std::string dir = readCpcString(cpu->hl(), cpu->b, true);
    if (drive) drive->chdir(dir.empty() ? "/" : dir);
    cpu->f = 1;
}

} // namespace cpcse
