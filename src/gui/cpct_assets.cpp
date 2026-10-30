// CPCSyntaxError GUI — CPCtelera's asset steps from cfg/*.mk. See cpct_assets.h.
#include "cpct_assets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "cpct_img.h"
#include "cpct_media.h"
#include "zx_compress.h"
#include "core/dsk.h"

namespace fs = std::filesystem;

namespace cpcse {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
    return s.substr(a);
}
std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}
std::string stemOf(const std::string& path) {               // "img/court.tmx" -> "court" (to the first '.')
    std::string b = fs::u8path(path).filename().u8string();
    const size_t dot = b.find('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}
std::string noExt(const std::string& path) {                 // bash's ${1%.*}
    const size_t dot = path.rfind('.');
    const size_t slash = path.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? path : path.substr(0, dot);
}

struct Call { std::string macro; std::vector<std::string> args; std::string mk; int line = 0; };

// Make's variables, as these files use them: NAME = / := / ?= / += value; $(NAME) and
// ${NAME} expanded when used (any other $(...) is left as it is).
struct Vars {
    std::map<std::string, std::string> v;
    std::string expand(const std::string& s, int depth = 0) const {
        if (depth > 16) return s;
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '$' && i + 1 < s.size() && (s[i + 1] == '(' || s[i + 1] == '{')) {
                const char close = s[i + 1] == '(' ? ')' : '}';
                const size_t end = s.find(close, i + 2);
                if (end != std::string::npos) {
                    const std::string name = s.substr(i + 2, end - i - 2);
                    auto it = v.find(name);
                    if (it != v.end() && name.find(' ') == std::string::npos) { out += expand(it->second, depth + 1); i = end; continue; }
                }
            }
            out += s[i];
        }
        return out;
    }
};

// The calls of one .mk file, in order, with the variables it sets on the way.
void readMk(const fs::path& file, Vars& vars, std::vector<Call>& calls) {
    std::istringstream in(readAll(file));
    std::string raw, line;
    int n = 0, first = 0;
    while (std::getline(in, raw)) {
        n++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (line.empty()) first = n;
        if (!raw.empty() && raw.back() == '\\') { line += raw.substr(0, raw.size() - 1) + " "; continue; }
        line += raw;
        std::string l = line;
        line.clear();
        const size_t hash = l.find('#');
        if (hash != std::string::npos) l.erase(hash);
        l = trim(l);
        if (l.empty()) continue;
        const size_t call = l.find("$(call ");
        if (l.rfind("$(eval", 0) == 0 && call != std::string::npos) {
            // the arguments of $(call ...): commas at depth 0 split them
            size_t i = call + 7;
            int depth = 0;
            std::string cur;
            std::vector<std::string> parts;
            for (; i < l.size(); i++) {
                const char c = l[i];
                if (c == '(' || c == '{') depth++;
                if (c == ')' || c == '}') { if (depth == 0) break; depth--; }
                if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
                cur += c;
            }
            parts.push_back(cur);
            Call c;
            c.macro = trim(parts[0]);
            for (size_t k = 1; k < parts.size(); k++) c.args.push_back(trim(vars.expand(parts[k])));
            c.mk = file.u8string();
            c.line = first;
            calls.push_back(c);
            continue;
        }
        // an assignment: NAME op value
        size_t eq = l.find('=');
        if (eq == std::string::npos || l.find("$(") < eq) continue;
        std::string op = "=";
        size_t nameEnd = eq;
        if (eq > 0 && (l[eq - 1] == ':' || l[eq - 1] == '?' || l[eq - 1] == '+')) { op = std::string(1, l[eq - 1]) + "="; nameEnd = eq - 1; }
        const std::string name = trim(l.substr(0, nameEnd));
        if (name.empty() || name.find(' ') != std::string::npos) continue;
        const std::string value = trim(l.substr(eq + 1));
        if (op == "?=" && vars.v.count(name)) continue;
        if (op == "+=") vars.v[name] = (vars.v.count(name) ? vars.v[name] + " " : "") + value;
        else if (op == ":=") vars.v[name] = vars.expand(value);
        else vars.v[name] = value;
    }
}

// Run when an output is missing or older than an input.
bool stale(const std::vector<fs::path>& outputs, const std::vector<fs::path>& inputs) {
    std::error_code ec;
    fs::file_time_type oldestOut = fs::file_time_type::max(), newestIn = fs::file_time_type::min();
    for (const fs::path& o : outputs) {
        if (!fs::exists(o, ec)) return true;
        oldestOut = std::min(oldestOut, fs::last_write_time(o, ec));
    }
    for (const fs::path& i : inputs) if (fs::exists(i, ec)) newestIn = std::max(newestIn, fs::last_write_time(i, ec));
    return newestIn > oldestOut;
}

std::vector<int> palette(const std::string& s) {           // "{ 0 24 26 13 }" or "0 24 26 13"
    std::string t = s;
    for (char& c : t) if (c == '{' || c == '}' || c == ',') c = ' ';
    std::vector<int> out;
    for (const std::string& w : words(t)) out.push_back(std::atoi(w.c_str()));
    return out;
}

bool writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
    return (bool)f;
}

// cpct_bin2c: "// File F converted using cpct_bin2c", the include, a const array of 16 bytes a line.
std::string bin2c(const std::vector<uint8_t>& data, const std::string& shownName, const std::string& array, const std::string& include) {
    std::ostringstream o;
    o << "// File " << shownName << " converted using cpct_bin2c\n";
    if (!include.empty()) o << "\n#include <" << include << ">\n\n";
    o << "const unsigned char " << array << "[" << data.size() << "] = {\n";
    for (size_t i = 0; i < data.size(); i++) {
        char b[8];
        std::snprintf(b, sizeof b, " 0x%02x", data[i]);
        if (i) o << ((i % 16) == 0 ? ",\n" : ",");
        o << b;
    }
    o << "\n};\n";
    return o.str();
}

// One map layer of a TMX file (CSV encoding, as all CPCtelera's are).
struct TmxLayer { int width = 0, height = 0; bool visible = true; std::vector<int> tiles; };
bool readTmx(const fs::path& file, std::vector<TmxLayer>& layers, int& mapW, int& mapH, std::string& why) {
    const std::string x = readAll(file);
    auto attr = [](const std::string& tag, const char* name) -> std::string {
        const std::string key = std::string(" ") + name + "=\"";
        const size_t a = tag.find(key);
        if (a == std::string::npos) return "";
        const size_t b = tag.find('"', a + key.size());
        return b == std::string::npos ? "" : tag.substr(a + key.size(), b - a - key.size());
    };
    const size_t m = x.find("<map");
    if (m == std::string::npos) { why = "not a TMX map"; return false; }
    const std::string mapTag = x.substr(m, x.find('>', m) - m);
    mapW = std::atoi(attr(mapTag, "width").c_str());
    mapH = std::atoi(attr(mapTag, "height").c_str());
    for (size_t at = x.find("<layer", m); at != std::string::npos; at = x.find("<layer", at + 1)) {
        const std::string tag = x.substr(at, x.find('>', at) - at);
        TmxLayer l;
        l.width = std::atoi(attr(tag, "width").c_str());
        l.height = std::atoi(attr(tag, "height").c_str());
        l.visible = attr(tag, "visible") != "0";
        const size_t d = x.find("<data", at);
        if (d == std::string::npos) break;
        const std::string dataTag = x.substr(d, x.find('>', d) - d);
        if (attr(dataTag, "encoding") != "csv") { why = "only CSV-encoded layers are read (save the map with CSV layer format)"; return false; }
        const size_t from = x.find('>', d) + 1, to = x.find("</data>", from);
        std::string csv = x.substr(from, to - from);
        for (char& c : csv) if (c == ',' || c == '\r' || c == '\n') c = ' ';
        for (const std::string& w : words(csv)) l.tiles.push_back((int)std::strtoul(w.c_str(), nullptr, 10));
        layers.push_back(l);
        at = to;
    }
    if (layers.empty()) { why = "no layer"; return false; }
    return true;
}
// The project's cfg/*.mk files in the order make reads them: those cfg/build_config.mk
// includes, then those the Makefile includes after it (diskSectors' disk_sector_manager.mk).
std::vector<fs::path> projectMks(const fs::path& root) {
    std::vector<fs::path> mks;
    for (const fs::path& from : { root / "cfg" / "build_config.mk", root / "Makefile" }) {
        std::istringstream in(readAll(from));
        std::string l;
        while (std::getline(in, l)) {
            l = trim(l);
            if (l.rfind("include cfg/", 0) != 0) continue;
            const fs::path mk = root / trim(l.substr(8));
            if (mk.filename() != "build_config.mk" && std::find(mks.begin(), mks.end(), mk) == mks.end()) mks.push_back(mk);
        }
    }
    return mks;
}

// make's 16-bit address as cpc2cdt and iDSK read it: 0x hex or decimal; -1 if neither
int address16(const std::string& s) {
    char* end = nullptr;
    long v;
    if (s.size() > 2 && s[0] == '0' && s[1] == 'x') v = std::strtol(s.c_str() + 2, &end, 16);
    else v = std::strtol(s.c_str(), &end, 10);
    return !s.empty() && end && !*end && v >= 0 && v <= 0xffff ? (int)v : -1;
}

} // namespace

MediaResult makeProjectMedia(const std::string& projectRoot, const std::string& cpctFolder, const std::string& projName,
                             const std::vector<uint8_t>& bin, int load, int run) {
    MediaResult r;
    std::error_code ec;
    const fs::path root = fs::u8path(projectRoot);
    auto problem = [&](const Call* c, const std::string& text, bool error) {
        CBuildMessage m;
        if (c) { m.file = c->mk; m.line = c->line; }
        m.text = (c ? c->macro + ": " : std::string()) + text;
        m.error = error;
        r.messages.push_back(m);
        if (error) r.ok = false;
    };
    // build_config.mk's names and TARGET
    Vars vars;
    vars.v["PROJNAME"] = projName;
    vars.v["OBJDIR"] = "obj";
    vars.v["DSKFILESDIR"] = "dsk";
    if (!cpctFolder.empty()) { vars.v["CPCT_PATH"] = cpctFolder + "/"; vars.v["CPCT_SRC"] = cpctFolder + "/src"; }
    std::vector<Call> ignored;
    readMk(root / "cfg" / "build_config.mk", vars, ignored);
    auto var = [&](const char* name, const std::string& fallback) {
        auto it = vars.v.find(name);
        return it == vars.v.end() ? fallback : trim(vars.expand(it->second));
    };
    const std::string dskName = var("DSK", projName + ".dsk"), cdtName = var("CDT", projName + ".cdt"), snaName = var("SNA", projName + ".sna");
    const std::string dskInc = var("DSKINC", "obj/" + dskName + ".dskinc");
    const std::vector<std::string> target = words(var("TARGET", cdtName + " " + dskName + " " + dskInc + " " + snaName));
    auto wants = [&](const std::string& n) { return std::find(target.begin(), target.end(), n) != target.end(); };

    std::vector<Call> calls;
    Vars mkVars = vars;
    for (const fs::path& mk : projectMks(root)) if (fs::exists(mk, ec)) readMk(mk, mkVars, calls);
    auto readBytes = [](const fs::path& p, bool& ok) {
        std::ifstream f(p, std::ios::binary);
        ok = (bool)f;
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };

    // ---- the disc: the program, the DSKFILESDIR files, then FILE2SECTOR
    if (wants(dskName)) {
        std::shared_ptr<Disk> disc = cpctNewDisc();
        std::string why = cpctDiscImport(*disc, projName + ".bin", bin, 1, load, run);
        if (!why.empty()) problem(nullptr, dskName + ": " + why, true);
        if (wants(dskInc)) {
            std::vector<fs::path> files;
            for (auto it = fs::directory_iterator(root / var("DSKFILESDIR", "dsk"), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) files.push_back(it->path());
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) {
                bool ok = false;
                const std::vector<uint8_t> data = readBytes(f, ok);
                const bool text = cpctLooksAscii(data);
                why = ok ? cpctDiscImport(*disc, f.filename().u8string(), data, text ? 0 : 1, 0, 0) : "could not be read";
                if (!why.empty()) problem(nullptr, dskName + ": " + f.filename().u8string() + ": " + why, true);
                else { r.log += "disc: added " + f.filename().u8string() + (text ? " (text)" : "") + "\n"; r.discNeeded = true; }
            }
        }
        std::string sectorDisc;
        for (const Call& c : calls) {
            if (c.macro != "FILE2SECTOR") continue;
            const std::string cmd = c.args.empty() ? "" : c.args[0];
            if (cmd == "SET_DSK_FILE") sectorDisc = c.args.size() > 1 ? c.args[1] : "";
            else if (cmd == "ADD_DATA_FILE") {
                if (sectorDisc != dskName) { problem(&c, "only the project's own disc (" + dskName + ") is written here; SET_DSK_FILE names '" + sectorDisc + "'", true); continue; }
                const std::string file = c.args.size() > 1 ? c.args[1] : "", where = c.args.size() > 2 ? c.args[2] : "";
                bool ok = false;
                const std::vector<uint8_t> data = readBytes(root / fs::u8path(file), ok);
                why = !ok ? file + " could not be read" : data.empty() ? file + " is empty" : data.size() > 65536 ? file + " is over 64K" : cpctDiscWriteSectors(*disc, data, where);
                if (!why.empty()) problem(&c, why, true);
                else { r.log += "disc: " + file + " (" + std::to_string(data.size()) + " bytes) written to " + where + "\n"; r.discNeeded = true; }
            } else problem(&c, "unknown command " + cmd, true);
        }
        r.dskPath = (root / fs::u8path(dskName)).u8string();
        const Bytes image = cpctDiscImage(*disc);
        std::ofstream f(fs::u8path(r.dskPath), std::ios::binary);
        f.write((const char*)image.data(), (std::streamsize)image.size());
        char addresses[40];
        std::snprintf(addresses, sizeof addresses, "load &%04X, run &%04X", load & 0xffff, run & 0xffff);
        if (!f) problem(nullptr, "could not write " + r.dskPath, true);
        else r.log += "disc: " + dskName + " (" + projName + ".bin, " + addresses + ")\n";
    }

    // ---- the tape: CDTMAN's list of files, else the program as a firmware file "Game"
    if (wants(cdtName)) {
        struct Entry { std::string file, type, name; int load = -1, run = -1; const Call* c = nullptr; };
        std::vector<Entry> list;
        std::string filename = "Game";
        for (const Call& c : calls) {
            if (c.macro != "CDTMAN") continue;
            const auto arg = [&](size_t i) { return i < c.args.size() ? c.args[i] : std::string(); };
            const std::string cmd = arg(0);
            if (cmd == "SET_FILENAME") {
                filename.clear();
                for (char ch : arg(1)) if (ch != '\'') filename += ch;
                if (filename.size() > 16) problem(&c, "'" + filename + "' is longer than the 16 characters a tape name can have", true);
            } else if (cmd == "ADDFILE") {
                Entry e{ arg(2), arg(1), filename, address16(arg(3)), address16(arg(4)), &c };
                if (e.type == "firmware" && (e.load < 0 || e.run < 0)) { problem(&c, "a firmware file needs its load and run addresses", true); continue; }
                list.push_back(e);
            } else if (cmd == "ADD_GENERATED_BIN") {
                Entry e{ "", arg(1) == "miniload" ? "generated_ml" : "generated_fw", filename, address16(arg(2)), address16(arg(3)), &c };
                list.push_back(e);
            } else problem(&c, cmd + " (CPCtelera's miniloader) is not made here: its loader is CPCtelera's own Z80 code", true);
        }
        if (list.empty()) list.push_back(Entry{ "", "firmware_bin", filename });
        std::vector<uint8_t> cdt;
        for (const Entry& e : list) {
            std::vector<uint8_t> data = bin;
            if (!e.file.empty()) {
                bool ok = false;
                data = readBytes(root / fs::u8path(e.file), ok);
                if (!ok) { problem(e.c, e.file + " could not be read", true); continue; }
            }
            if (e.type == "generated_fw" || e.type == "firmware_bin") cpctTapeAddFile(cdt, data, e.name, "cpc", e.load >= 0 ? e.load : load, e.run >= 0 ? e.run : run, 3000);
            else if (e.type == "firmware") cpctTapeAddFile(cdt, data, e.name, "cpc", e.load, e.run, 3000);
            else if (e.type == "basic") cpctTapeAddFile(cdt, data, e.name, "cpc", -1, -1, 3000);
            else if (e.type == "ascii") cpctTapeAddFile(cdt, data, e.name, "cpctxt", -1, -1, -1);
            else { problem(e.c, "format '" + e.type + "' (CPCtelera's miniloader) is not made here", true); continue; }
            r.log += "tape: " + (e.file.empty() ? projName + ".bin" : e.file) + " as '" + e.name + "' (" + e.type + ")\n";
        }
        r.cdtPath = (root / fs::u8path(cdtName)).u8string();
        std::ofstream f(fs::u8path(r.cdtPath), std::ios::binary);
        f.write((const char*)cdt.data(), (std::streamsize)cdt.size());
        if (!f) problem(nullptr, "could not write " + r.cdtPath, true);
    }

    if (wants(snaName)) { r.wantSna = true; r.snaPath = (root / fs::u8path(snaName)).u8string(); }
    return r;
}

AssetResult convertProjectAssets(const std::string& projectRoot, const std::string& srcDir, const std::string& cpctFolder, bool force) {
    AssetResult r;
    std::error_code ec;
    const fs::path root = fs::u8path(projectRoot);
    auto note = [&](const std::string& s) { r.log += s + "\n"; };
    auto problem = [&](const Call* c, const std::string& text, bool error) {
        CBuildMessage m;
        if (c) { m.file = c->mk; m.line = c->line; }
        m.text = (c ? c->macro + ": " : std::string()) + text;
        m.error = error;
        r.messages.push_back(m);
        if (error) r.ok = false;
    };

    const std::vector<fs::path> mks = projectMks(root);
    Vars vars;
    vars.v["SRCDIR"] = "src";
    vars.v["OBJDIR"] = "obj";
    if (!cpctFolder.empty()) { vars.v["CPCT_PATH"] = cpctFolder + "/"; vars.v["CPCT_SRC"] = cpctFolder + "/src"; }
    std::vector<Call> calls;
    for (const fs::path& mk : mks) if (fs::exists(mk, ec)) readMk(mk, vars, calls);

    // module state, with each module's defaults
    std::vector<int> i2sPal = { 1, 24, 20, 6, 26, 0, 2, 8, 10, 12, 14, 16, 18, 22, 24, 16 };  // cpct_img2tileset's
    int i2sMode = 0;
    bool i2sMask = false, i2sBin = false, i2sZg = false, i2sScr = false;
    std::string i2sFolder = "src/", i2sExtra;
    std::string t2dFolder = "src/";
    bool t2dC = true, t2dH = true;
    std::string a2dFolder = "src", a2dPlayer = "akm", a2dExtra;
    bool a2dBin = true, a2dBinFile = true;                   // "-gs -gb -gf": with an address, binary wins
    std::map<std::string, std::vector<std::string>> packs;   // ADD2PACK: the files of each pack, in order

    auto extraOptions = [](ImgConvert& job, const std::string& extra) {
        const std::vector<std::string> w = words(extra);
        for (size_t i = 0; i < w.size(); i++) {
            if (w[i] == "-t" && i + 1 < w.size()) job.transparentIndex = std::atoi(w[++i].c_str());
            else if (w[i] == "-im") { job.interlacedMasks = true; job.transparentIndex = 0; }
            else if (w[i] == "-scr") job.screen = true;
            else if (w[i] == "-z") job.zigZag = true;
            else if (w[i] == "-g") job.grayCode = true;
            else if (w[i] == "-nt") job.tileset = false;
        }
    };
    auto runImage = [&](const Call& c, ImgConvert job, const std::string& source) {
        const fs::path mkPath = fs::u8path(c.mk);
        std::vector<fs::path> outs;
        for (const char* e : job.binary ? std::vector<const char*>{ ".bin", ".h", ".h.s" } : std::vector<const char*>{ ".c", ".h" })
            outs.push_back(fs::u8path(job.outBase + e));
        if (!force && !stale(outs, { root / source, mkPath })) { r.upToDate++; return; }
        fs::create_directories(fs::u8path(job.outBase).parent_path(), ec);
        std::string why;
        if (!convertImage(job, why)) { problem(&c, why, true); return; }
        note("converted " + source + " -> " + fs::relative(fs::u8path(job.outBase), root, ec).u8string() + (job.binary ? ".bin" : ".c"));
        r.converted++;
    };

    for (const Call& c : calls) {
        const auto arg = [&](size_t i) { return i < c.args.size() ? c.args[i] : std::string(); };
        if (c.macro == "IMG2SPRITES") {
            // (file, mode, prefix, width, height, palette, mask|zgtiles|tileset, folder, hwpalette, extra)
            ImgConvert job;
            job.image = (root / arg(0)).u8string();
            job.label = noExt(arg(0));
            job.mode = std::atoi(arg(1).c_str());
            job.baseName = arg(2);
            job.tileW = std::atoi(arg(3).c_str());
            job.tileH = std::atoi(arg(4).c_str());
            job.firmwarePalette = palette(arg(5));
            const std::string kind = arg(6);
            job.tileset = kind == "tileset";
            if (kind == "mask") { job.interlacedMasks = true; job.transparentIndex = 0; }
            if (kind == "zgtiles") { job.zigZag = true; job.grayCode = true; }
            job.hardwarePalette = !arg(8).empty();
            extraOptions(job, arg(9));
            job.outBase = (arg(7).empty() ? root / noExt(arg(0)) : root / arg(7) / fs::u8path(noExt(arg(0))).filename()).u8string();
            runImage(c, job, arg(0));
        } else if (c.macro == "IMG2SP") {
            const std::string cmd = arg(0);
            if (cmd == "SET_PALETTE_FW") i2sPal = palette(arg(1));
            else if (cmd == "SET_MODE") i2sMode = std::atoi(arg(1).c_str());
            else if (cmd == "SET_MASK") i2sMask = arg(1) == "interlaced";
            else if (cmd == "SET_OUTPUT") i2sBin = arg(1) == "bin";
            else if (cmd == "SET_FOLDER") i2sFolder = arg(1);
            else if (cmd == "SET_IMG_FORMAT") { i2sZg = arg(1) == "zgtiles"; i2sScr = arg(1) == "screen"; }
            else if (cmd == "SET_EXTRAPAR") i2sExtra = arg(1);
            else if (cmd == "CONVERT") {
                // (file, width, height, name, palette name, tileset name)
                ImgConvert job;
                job.image = (root / arg(1)).u8string();
                job.label = noExt(arg(1));
                job.mode = i2sMode;
                job.firmwarePalette = i2sPal;
                job.tileW = std::atoi(arg(2).c_str());
                job.tileH = std::atoi(arg(3).c_str());
                job.baseName = arg(4);
                job.absoluteBaseName = true;
                job.hardwarePalette = !arg(5).empty();
                job.paletteName = arg(5);
                job.binary = i2sBin;
                job.tileset = !i2sBin && !arg(6).empty();
                job.tilesetName = arg(6);
                if (i2sMask) { job.interlacedMasks = true; job.transparentIndex = 0; }
                job.zigZag = job.grayCode = i2sZg;
                job.screen = i2sScr;
                extraOptions(job, i2sExtra);
                job.outBase = (root / i2sFolder / fs::u8path(noExt(arg(1))).filename()).u8string();
                runImage(c, job, arg(1));
            } else if (cmd == "CONVERT_PALETTE") {
                // (palette, C identifier): cid.c / cid.h with the hardware values
                const std::vector<int> pal = palette(arg(1));
                const std::string cid = arg(2);
                const fs::path cFile = root / i2sFolder / (cid + ".c"), hFile = root / i2sFolder / (cid + ".h");
                if (!force && !stale({ cFile, hFile }, { fs::u8path(c.mk) })) { r.upToDate++; continue; }
                std::string hw, fwText;
                for (size_t i = 0; i < pal.size(); i++) {
                    char b[8];
                    std::snprintf(b, sizeof b, "0x%02X", firmwareToHardware(pal[i]));
                    hw += (i ? ", " : "") + std::string(b);
                    fwText += (i ? " " : "") + std::to_string(pal[i]);
                }
                writeText(hFile, "// File generated by CONVERT_PALETTE macro from CPCtelera\n// Converted firmware palette declaration\n// \n#ifndef _" + cid +
                                 "_H_\n#define _" + cid + "_H_\n\n#include <types.h>\n\nextern const u8 " + cid + "[" + std::to_string(pal.size()) + "];\n\n#endif\n");
                writeText(cFile, "// File generated by CONVERT_PALETTE macro from CPCtelera\n// Firmware Palette Converted: " + fwText + "\n// \n#include \"" + cid +
                                 ".h\"\n \nconst u8 " + cid + "[" + std::to_string(pal.size()) + "] = {\n   " + hw + "\n};\n");
                note("palette " + cid);
                r.converted++;
            } else problem(&c, "unknown command " + cmd, true);
        } else if (c.macro == "TMX2C" || (c.macro == "TMX2DATA" && arg(0) == "CONVERT")) {
            const bool data = c.macro == "TMX2DATA";
            const std::string file = data ? arg(1) : arg(0);
            const std::string cid = data ? (arg(2).empty() ? "g_map" : arg(2)) : arg(1);
            const std::string folder = data ? t2dFolder : arg(2);
            const int bits = data ? 8 : (arg(3).empty() ? 8 : std::atoi(arg(3).c_str()));
            const fs::path cFile = (folder.empty() ? root / fs::u8path(file).parent_path() : root / folder) / (stemOf(file) + ".c");
            const fs::path hFile = fs::path(cFile).replace_extension(".h");
            std::vector<fs::path> outs;
            if (!data || t2dC) outs.push_back(cFile);
            if (!data || t2dH) outs.push_back(hFile);
            if (!force && !stale(outs, { root / file, fs::u8path(c.mk) })) { r.upToDate++; continue; }
            std::vector<TmxLayer> layers;
            int mw = 0, mh = 0;
            std::string why;
            if (!readTmx(root / file, layers, mw, mh, why)) { problem(&c, file + ": " + why, true); continue; }
            auto adjusted = [](int id) { return id ? id - 1 : 0; };     // Tiled numbers tiles from 1, 0 = no tile
            if (!data) {
                // cpct_tmx2csv: the first layer
                const TmxLayer& l = layers[0];
                if (bits != 1 && bits != 2 && bits != 4 && bits != 6 && bits != 8) { problem(&c, "bits per item must be 1, 2, 4, 6 or 8", true); continue; }
                const int wBytes = bits < 8 ? l.width * bits / 8 : l.width;
                if (bits < 8 && wBytes * 8 / bits != l.width) { problem(&c, "the map's width does not fit whole bytes at " + std::to_string(bits) + " bits per item", true); continue; }
                std::string body = bits < 8 ? "  0b" : "";
                int convertedBits = 0, lineItems = 0;
                for (int id : l.tiles) {
                    const int item = adjusted(id);
                    std::string s;
                    if (bits < 8) {
                        const int v = item & ((1 << bits) - 1);
                        for (int b = bits - 1; b >= 0; b--) s += (v >> b & 1) ? '1' : '0';
                        convertedBits += bits;
                        if (convertedBits > 8) {
                            convertedBits -= 8;
                            const int comp = bits - convertedBits;
                            s = comp ? s.substr(0, (size_t)comp) + ", 0b" + s.substr((size_t)comp) : ", 0b" + s;
                        }
                    } else s = std::to_string(item) + ",";
                    body += s;
                    if (++lineItems == l.width) { lineItems = 0; body += "\n"; }
                }
                const std::string header = "//\n// File " + file + " converted to csv using cpct_tmx2csv\n//   * Width:  " + std::to_string(l.width) + " columns (" +
                                           std::to_string(wBytes) + " bytes, " + std::to_string(bits) + " bits per column)\n//   * Height: " + std::to_string(l.height) +
                                           " rows\n//   * Bytes:  " + std::to_string(wBytes * l.height) + " bytes (" + std::to_string(wBytes) + " x " + std::to_string(l.height) + ")\n//\n";
                const std::string decl = "const u8 " + cid + "[" + std::to_string(wBytes) + "*" + std::to_string(l.height) + "]";
                writeText(cFile, header + "#include <types.h>\n\n// Generated CSV tilemap from " + file + "\n" + decl + " = {\n" + body + "\n};\n");
                writeText(hFile, header + "#include <types.h>\n\n// Generated CSV tilemap from " + file + "\n//   " + std::to_string(wBytes * l.height) + " bytes (" +
                                 std::to_string(wBytes) + " x " + std::to_string(l.height) + ")\n//   " + std::to_string(l.width) + "*" + std::to_string(l.height) +
                                 " items (" + std::to_string(bits) + " bits per item)\n//\n#define " + cid + "_W  " + std::to_string(wBytes) + "\n#define " + cid +
                                 "_H  " + std::to_string(l.height) + "\nextern " + decl + ";\n");
            } else {
                // tmx2data, 8 bits per tile: every visible layer
                std::vector<const TmxLayer*> vis;
                for (const TmxLayer& l : layers) if (l.visible) vis.push_back(&l);
                if (vis.empty()) { problem(&c, file + " has no visible layer", true); continue; }
                int maxId = 0;
                for (const TmxLayer* l : vis) for (int id : l->tiles) maxId = std::max(maxId, adjusted(id));
                const int digits = maxId >= 100 ? 3 : maxId >= 10 ? 2 : 1;
                const int total = mw * mh;
                std::ostringstream hd;
                hd << "//\n// File " << file << " converted to csv using cpct_tmx2data\n//   * Visible Layers:  " << vis.size() << "\n//   * Layer Width:     " << mw
                   << "\n//   * Layer Height:    " << mh << "\n//   * Bits per tile:   8\n//   * Layer Bytes:     " << total << " (" << mw << " x " << mh
                   << " items, 8 bits per item)\n//   * Total Bytes:     " << total * (int)vis.size() << " (" << total << " x " << vis.size() << ", bytes per layer times layers)\n//";
                const std::string layersDim = vis.size() > 1 ? "[" + std::to_string(vis.size()) + "]" : "";
                if (t2dH) {
                    std::ostringstream h;
                    h << hd.str() << "\n#include <cpctelera.h>\n\n//#### Width and height constants ####\n#define " << cid << "_W  " << mw << "\n#define " << cid << "_H  " << mh
                      << "\n\n//#### Converted layer tilemaps ####\n//   Visible layers: " << vis.size() << "\n//";
                    if (vis.size() == 1) h << "\nextern const u8 " << cid << "[" << total << "];";
                    else {
                        h << "\n\n// General layer array\nextern const u8 " << cid << layersDim << "[" << total << "];\n\n// Constant pointers for immediate access";
                        for (size_t k = 0; k < vis.size(); k++) h << "\n#define " << cid << "_layer_" << k << "   (u8*)(&layers[" << k << "][0])";
                    }
                    h << "\n";
                    writeText(hFile, h.str());
                }
                if (t2dC) {
                    std::ostringstream o;
                    o << hd.str() << "\n#include <cpctelera.h>\n\n//#### Converted layer tilemaps ####\n//   Visible layers: " << vis.size() << "\n//";
                    o << "\nconst u8 " << cid << layersDim << "[" << total << "] = {\n";
                    char comma = ' ';
                    for (const TmxLayer* l : vis) {
                        o << "   " << comma << (vis.size() > 1 ? '{' : ' ') << "\n";
                        char sep = ' ';
                        for (int y = 0; y < mh; y++) {
                            o << "   " << sep;
                            for (int x = 0; x < mw; x++) {
                                const size_t i = (size_t)y * mw + x;
                                char b[16];
                                std::snprintf(b, sizeof b, "%*d", digits, i < l->tiles.size() ? adjusted(l->tiles[i]) : 0);
                                o << (x ? "," : "") << b;
                            }
                            o << "\n";
                            sep = ',';
                        }
                        o << "   " << (vis.size() > 1 ? '}' : ' ') << "\n";
                        comma = ',';
                    }
                    o << "};\n";
                    writeText(cFile, o.str());
                }
            }
            note("converted " + file + " -> " + fs::relative(cFile, root, ec).u8string());
            r.converted++;
        } else if (c.macro == "TMX2DATA") {
            const std::string cmd = arg(0);
            if (cmd == "SET_FOLDER") t2dFolder = arg(1);
            else if (cmd == "SET_OUTPUTS") {
                const std::vector<std::string> w = words(arg(1));
                t2dC = std::find(w.begin(), w.end(), "c") != w.end();
                t2dH = std::find(w.begin(), w.end(), "h") != w.end();
                for (const std::string& o : w) if (o != "c" && o != "h") problem(&c, "output '" + o + "' is not made here yet (only c and h)", false);
            } else if (cmd == "SET_BITSPERITEM" && arg(1) != "8") problem(&c, "only 8 bits per tile are made here yet", true);
            else if (cmd == "SET_BASE" || cmd == "SET_USEMACROS" || cmd == "SET_ASMVARPREFIX" || cmd == "SET_EXTRAPAR" || cmd == "SET_BITSPERITEM") {
            } else problem(&c, "unknown command " + cmd, true);
        } else if (c.macro == "AKS2DATA") {
            const std::string cmd = arg(0);
            if (cmd == "SET_FOLDER") a2dFolder = arg(1);
            else if (cmd == "SET_PLAYER") a2dPlayer = arg(1);
            else if (cmd == "SET_EXTRAPAR") a2dExtra = arg(1);
            else if (cmd == "SET_OUTPUTS") {
                const std::vector<std::string> w = words(arg(1));
                a2dBin = std::find(w.begin(), w.end(), "bin") != w.end();
                a2dBinFile = std::find(w.begin(), w.end(), "bin_file") != w.end();
            } else if (cmd == "CONVERT") {
                // cpct_aks2c with Arkos Tracker 3's own converters
                const std::string file = arg(1), id = arg(2), addr = arg(3);
                const fs::path tools = fs::u8path(cpctFolder) / "tools" / "ArkosTracker-3";
                const char* exe = a2dPlayer == "akg" ? "SongToAkg.exe" : a2dPlayer == "fx" ? "SongToSoundEffects.exe" : "SongToAkm.exe";
                const std::string base = noExt(fs::u8path(file).filename().u8string());
                const fs::path outDir = root / a2dFolder;
                const fs::path sFile = outDir / (base + ".s"), hFile = outDir / (base + ".h"), cFile = outDir / (base + ".c"), binFile = outDir / (base + ".bin");
                std::vector<fs::path> outs;
                if (a2dBin) outs = { cFile, hFile };
                else if (a2dBinFile) outs = { binFile };
                else outs = { sFile, hFile };
                if (!force && !stale(outs, { root / file, fs::u8path(c.mk) })) { r.upToDate++; continue; }
#ifndef _WIN32
                problem(&c, "Arkos Tracker 3's converters in CPCtelera are Windows programs", true);
                continue;
#endif
                if (!fs::exists(tools / "tools" / exe, ec)) { problem(&c, (tools / "tools" / exe).u8string() + " is not there", true); continue; }
                if ((a2dBin || a2dBinFile) && addr.empty()) { problem(&c, "binary output needs the memory address (third parameter)", true); continue; }
                fs::create_directories(outDir, ec);
                std::vector<std::string> args = { (tools / "tools" / exe).u8string(), (root / file).u8string() };
                const bool binary = a2dBin || a2dBinFile;
                if (binary) for (const std::string& a : { binFile.u8string(), std::string("-bin"), std::string("--sourceProfile"), std::string("z80"), std::string("-adr"), addr }) args.push_back(a);
                else for (const std::string& a : { sFile.u8string(), std::string("--labelPrefix"), id, std::string("--customSourceProfileFile"), (tools / "cpctelera" / "SDCC_Z80.xml").u8string() }) args.push_back(a);
                for (const std::string& w : words(a2dExtra)) args.push_back(w);
                std::string out;
                const int code = runProgram(args, root.u8string(), out);
                note(std::string(exe) + " " + file + "\n" + out);
                if (code != 0) { problem(&c, std::string(exe) + " failed: " + trim(out).substr(0, 300), true); continue; }
                const std::string label = a2dPlayer == "fx" ? id + "SoundEffects" : id + "Start";
                if (a2dBin) {
                    std::ifstream bf(binFile, std::ios::binary);
                    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
                    bf.close();
                    const long a = std::strtol(addr.c_str(), nullptr, 0);
                    char head[160];
                    std::snprintf(head, sizeof head, "// Generated by CPCTelera for ArkosPlayer3\n#include <cpctelera.h>\nCPCT_ABSOLUTE_LOCATION_AREA(0x%lX); // Array at 0x%lX \n\n", a - 1, a);
                    writeText(cFile, head + bin2c(bytes, binFile.u8string(), "g_" + id, "") + "CPCT_RELOCATABLE_AREA();");
                    fs::remove(binFile, ec);
                    writeText(hFile, "// Generated by CPCTelera for ArkosPlayer3\nextern u8 g_" + id + "[];");
                } else if (!a2dBinFile) {
                    // the script's sed edits: the label made global, and for AKM "$ +" and "#"
                    std::string s = readAll(sFile);
                    auto swap = [&](const std::string& from, const std::string& to) {
                        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
                    };
                    swap(label + ":", "_" + label + "::");
                    if (a2dPlayer == "akm") { swap("$ +", ". +"); swap("#", "0x"); }
                    writeText(sFile, s);
                    writeText(hFile, "// Generated by CPCTelera for ArkosPlayer3\nextern u8 " + label + "[];");
                }
                r.converted++;
            } else problem(&c, "unknown command " + cmd, true);
        } else if (c.macro == "ADD2PACK") {
            if (arg(0).empty()) problem(&c, "requires a non-empty pack name as first parameter", true);
            else for (const std::string& w : words(arg(1))) packs[arg(0)].push_back(w);
        } else if (c.macro.rfind("PACKZX", 0) == 0) {
            // cpct_pack -ghs -gh -gs -c <method> [-org address] <folder>/<name> <files>
            std::string method = c.macro.substr(4);
            for (char& ch : method) ch = (char)std::tolower((unsigned char)ch);
            const std::string name = arg(0);
            if (name.empty()) { problem(&c, "requires a PACK filename as first parameter", true); continue; }
            if (method != "zx0" && method != "zx0b" && method != "zx1" && method != "zx1b" && method != "zx7b") { problem(&c, "unknown compressor", true); continue; }
            const std::vector<std::string>& files = packs[name];
            if (files.empty()) { problem(&c, "PACK filename '" + name + "' does not contain any file to be packed. Is the PACK filename correctly spelled?", true); continue; }
            std::string folder = arg(1);
            while (!folder.empty() && folder.back() == '/') folder.pop_back();
            const std::string outfile = folder.empty() ? name : folder + "/" + name;
            const fs::path outBase = root / fs::u8path(outfile);
            std::vector<fs::path> ins = { root / "cfg" / "compression.mk", fs::u8path(c.mk) };
            for (const std::string& f : files) ins.push_back(root / fs::u8path(f));
            const std::vector<fs::path> outs = { fs::u8path(outBase.u8string() + ".s"), fs::u8path(outBase.u8string() + ".h"), fs::u8path(outBase.u8string() + ".h.s") };
            if (!force && !stale(outs, ins)) { r.upToDate++; continue; }
            std::string org;
            if (!arg(2).empty()) {
                char* end = nullptr;
                const long a = std::strtol(arg(2).c_str(), &end, 0);
                if (!end || *end || a < 0 || a > 0xffff) { problem(&c, "'" + arg(2) + "' is not a valid origin address (0-65535)", true); continue; }
                char b[16];
                std::snprintf(b, sizeof b, "0x%lx", a);
                org = b;
            }
            // all the files one after the other, then compressed
            std::vector<uint8_t> packed, z;
            std::vector<size_t> sizes;
            bool missing = false;
            for (const std::string& f : files) {
                std::ifstream in(root / fs::u8path(f), std::ios::binary);
                if (!in) { problem(&c, f + " is not there", true); missing = true; break; }
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                packed.insert(packed.end(), bytes.begin(), bytes.end());
                sizes.push_back(bytes.size());
            }
            if (missing) continue;
            if (packed.empty()) { problem(&c, "the files to pack are empty", true); continue; }
            // CPCtelera's own compressor when it is built (tools/<zx>/bin), else the same one here
            const std::string tool = method.substr(0, method == "zx7b" ? 4 : 3);
            fs::path exe = fs::u8path(cpctFolder) / "tools" / tool / "bin" / tool;
            if (!fs::exists(exe, ec)) exe += ".exe";
            if (!cpctFolder.empty() && fs::exists(exe, ec)) {
                const fs::path tmpIn = root / "obj" / (name + ".pack"), tmpOut = root / "obj" / (name + ".pack." + method);
                fs::create_directories(tmpIn.parent_path(), ec);
                { std::ofstream t(tmpIn, std::ios::binary); t.write((const char*)packed.data(), (std::streamsize)packed.size()); }
                fs::remove(tmpOut, ec);
                std::vector<std::string> args = { exe.u8string() };
                if (method != "zx7b") args.push_back("-f");
                if (method == "zx0b" || method == "zx1b") args.push_back("-b");
                args.push_back(tmpIn.u8string());
                args.push_back(tmpOut.u8string());
                std::string out;
                const int code = runProgram(args, root.u8string(), out);
                note(exe.filename().u8string() + " " + name + "\n" + out);
                std::ifstream zf(tmpOut, std::ios::binary);
                z.assign(std::istreambuf_iterator<char>(zf), std::istreambuf_iterator<char>());
                if (code != 0 || z.empty()) { problem(&c, exe.u8string() + " failed: " + trim(out).substr(0, 300), true); continue; }
            } else {
                std::string why;
                if (!zxCompress(method, packed, z, why)) { problem(&c, why, true); continue; }
            }
            const std::string arrayName = noExt(fs::u8path(outfile).filename().u8string());
            const size_t un = packed.size(), cz = z.size();
            const std::string unS = std::to_string(un), czS = std::to_string(cz);
            std::string fileList;
            for (const std::string& f : files) fileList += " '" + f + "'";
            auto infoHeader = [&](const std::string& pre, const std::string& shown) {
                return pre + " File '" + shown + "' generated using cpct_pack\n" + pre + " Compressor used:  " + method + "\n" + pre + " Files compressed: [" + fileList +
                       " ]\n" + pre + " Uncompressed:     " + unS + " bytes\n" + pre + " Compressed:       " + czS + " bytes\n" + pre + " Space saved:      " +
                       std::to_string((long long)un - (long long)cz) + " bytes\n" + pre + "\n\n";
            };
            auto offsetLines = [&](const char* fmt, const char* constant) {
                std::string s;
                size_t off = 0;
                for (size_t k = 0; k < files.size(); k++) {
                    char b[600];
                    if (constant) std::snprintf(b, sizeof b, fmt, arrayName.c_str(), (int)k, constant, std::to_string(off).c_str(), files[k].c_str());
                    else std::snprintf(b, sizeof b, fmt, arrayName.c_str(), (int)k, std::to_string(off).c_str(), files[k].c_str());
                    s += b;
                    off += sizes[k];
                }
                return s;
            };
            std::string h = infoHeader("//", outfile + ".h") + "#ifndef " + arrayName + "_" + czS + "_H\n#define " + arrayName + "_" + czS + "_H\n\n" +
                            "\n// Declaration of the compressed array\nextern const unsigned char " + arrayName + "[" + czS + "];\n\n" +
                            "// Address of the latest byte of the compressed array (for unpacking purposes)\n#define " + arrayName + "_end      (" + arrayName + " + " +
                            czS + " - 1)\n\n// Compressed and uncompressed sizes\n#define " + arrayName + "_size_z   " + czS + "\n#define " + arrayName + "_size     " + unS + "\n\n";
            if (files.size() > 1) h += "\n// Define constants for starting offsets of files in the uncompressed array\n" + offsetLines("#define %s_OFF_%03d   %6s   // Starting offset for %s\n", nullptr);
            h += "\n#endif\n";
            std::string hs = infoHeader(";;", outfile + ".h.s") + ";; Declaration of the compressed array and\n;; the address of the latest byte of the compressed array (for unpacking purposes)\n" +
                             ".globl _" + arrayName + "\n.globl _" + arrayName + "_end\n\n;; Compressed and uncompressed sizes\n_" + arrayName + "_size_z == " + czS + "\n_" +
                             arrayName + "_size   == " + unS + "\n\n";
            if (files.size() > 1) hs += "\n;; Define constants for starting offsets of files in the uncompressed array\n" + offsetLines("_%s_OFF_%03d %s %6s   ;; Starting offset for %s\n", "==");
            hs += "\n";
            // with an origin the script starts the file again ("cat >"), so its comment header goes
            std::string s = org.empty() ? infoHeader(";;", outfile + ".s")
                                        : ";; Absolute data location: '" + org + "'\n.area _" + arrayName + "_area_ (ABS)\n.org " + org + "\n\n";
            s += ";; Data array definition\n_" + arrayName + "::\n";
            for (size_t k = 0; k < cz; k++) {
                char b[8];
                std::snprintf(b, sizeof b, " 0x%02x", z[k]);
                s += (k == 0 ? "   .db " : (k % 16) == 0 ? "\n   .db " : ",") + std::string(b);
            }
            s += "\n;; Address of the latest byte of the compressed array (for unpacking purposes)\n_" + arrayName + "_end == . - 1\n\n";
            writeText(outs[1], h);
            writeText(outs[2], hs);
            writeText(outs[0], s);
            note("packed " + std::to_string(files.size()) + " file(s) with " + method + " -> " + outfile + ".s (" + unS + " -> " + czS + " bytes)");
            r.converted++;
        } else if (c.macro == "CDTMAN" || c.macro == "FILE2SECTOR" || c.macro == "APKMAN") {
            // the tape and the disc are made after the link (makeProjectMedia); APKMAN packs
            // an Android APK, which has no use here
        } else {
            problem(&c, "not a step this build knows; left out", false);
        }
    }

    // BIN2C: every .bin under the source folder becomes a .c beside it (cfg/global_functions.mk:
    // "$(BIN2C) $(2) -i "cpctelera.h" > $(1)"), named G_<file's name>.
    const fs::path src = fs::u8path(srcDir);
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".bin") continue;
        const fs::path bin = it->path(), cfile = fs::path(bin).replace_extension(".c");
        if (!force && !stale({ cfile }, { bin })) { r.upToDate++; continue; }
        std::ifstream bf(bin, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
        const std::string rel = fs::relative(bin, root, ec).generic_u8string();
        writeText(cfile, bin2c(bytes, rel, "G_" + noExt(bin.filename().u8string()), "cpctelera.h"));
        note("BIN2C " + rel);
        r.converted++;
    }
    return r;
}

} // namespace cpcse
