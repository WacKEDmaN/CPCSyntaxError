// cpcse-ctl — the command line's way into a running CPCSyntaxError (its command API,
// src/gui/devserver.h). One command per run, or a script of them:
//
//   cpcse-ctl load build/game.bin &4000 entry      poke it in and start it
//   cpcse-ctl load game.dsk command='RUN"GAME\n'   insert a disc and type
//   cpcse-ctl bp add &4000                          a breakpoint
//   cpcse-ctl wait stop                             until it stops (or 10 s)
//   cpcse-ctl regs                                  the Z80's registers (JSON)
//   cpcse-ctl --field data read &C000 16            just the bytes, as hex
//   cpcse-ctl '{"cmd":"read","addr":"&4000","len":4}'
//   cpcse-ctl - < script.txt                        one command per line
//
// The answer is printed as one JSON line; the exit status is 0 when it says ok, 1 when
// the emulator refused (the reason on stderr), 2 when it could not be reached.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "gui/devcommand.h"
#include "gui/devnet.h"
#include "gui/minijson.h"

using namespace cpcse;

namespace {

void usage() {
    std::fprintf(stderr,
        "usage: cpcse-ctl [--port N] [--field NAME] [--quiet] <command> [args...]\n"
        "       cpcse-ctl [options] '{\"cmd\":...}'      a JSON request as it stands\n"
        "       cpcse-ctl [options] -                    commands from stdin, one per line\n"
        "\n"
        "The emulator must be running with its command API on: Settings > External debugging,\n"
        "or cpcse --api. Commands (cpcse-ctl help asks the emulator):\n"
        "  ping | status | pause | run [addr] | step [n] | over | out | runto addr | reset\n"
        "  regs | setreg reg value | read addr [len] | write addr hexbytes | disasm [addr] [n]\n"
        "  load file [addr] [run]     .bin (addr or AMSDOS header; run = entry, an address or no),\n"
        "                             .sna .dsk .cpr .cdt; also unit= reset=1 command= symbols= wait=1\n"
        "  watch file [addr] [run]    reload whenever it changes | unwatch\n"
        "  symbols file | type text | key KeyCode | screenshot file.png\n"
        "  bp add|remove|clear|list [addr] [condition] | wp add|remove|clear|list [start] [end] [r|w|rw]\n"
        "  wait stop|frames|typed [n] | frames n | quit\n"
        "Addresses: &4000 #4000 $4000 0x4000 4000h 16384, or a label from the symbols.\n");
}

struct Options { int port = 6128; std::string field; bool quiet = false; };

// Sends one request, prints the answer; the exit status it deserves.
int roundTrip(net::Socket s, const std::string& request, const Options& o) {
    if (!net::sendAll(s, request + "\n")) { std::fprintf(stderr, "cpcse-ctl: the emulator closed the connection\n"); return 2; }
    std::string line;
    char buf[4096];
    for (;;) {
        size_t nl = line.find('\n');
        if (nl != std::string::npos) { line.resize(nl); break; }
        if (!net::waitReadable(s, 15 * 60 * 1000)) { std::fprintf(stderr, "cpcse-ctl: no answer\n"); return 2; }
        int n = net::receiveSome(s, buf, sizeof(buf));
        if (n < 0) { std::fprintf(stderr, "cpcse-ctl: the emulator closed the connection\n"); return 2; }
        line.append(buf, (size_t)n);
    }
    json::Value res;
    try { res = json::parse(line); } catch (const std::exception& ex) {
        std::fprintf(stderr, "cpcse-ctl: unreadable answer (%s): %s\n", ex.what(), line.c_str());
        return 2;
    }
    const bool ok = res.flag("ok");
    if (!ok) std::fprintf(stderr, "cpcse-ctl: %s\n", res.str("error", "failed").c_str());
    if (o.quiet) return ok ? 0 : 1;
    if (!o.field.empty()) {
        const json::Value* f = res.find(o.field);
        if (f && f->type == json::Value::String) std::printf("%s\n", f->text.c_str());
        else if (f) std::printf("%s\n", f->dump().c_str());
    } else {
        std::printf("%s\n", line.c_str());
    }
    std::fflush(stdout);
    return ok ? 0 : 1;
}

// The emulator opens files from ITS working folder; a path given here means this one.
void absolutePaths(json::Value& req) {
    std::string cmd = req.str("cmd");
    for (char& c : cmd) c = (char)std::tolower((unsigned char)c);
    std::vector<const char*> keys = { "symbols" };
    if (cmd == "load" || cmd == "watch" || cmd == "symbols" || cmd == "screenshot") keys.push_back("path");
    for (const char* k : keys) {
        const json::Value* v = req.find(k);
        if (!v || v->type != json::Value::String || v->text.empty()) continue;
        std::error_code ec;
        std::filesystem::path p = std::filesystem::absolute(v->text, ec);
        if (!ec) req.set(k, p.lexically_normal().string());
    }
}

// One request from words or a JSON text; false (and a message) when it cannot be made.
bool makeRequest(const std::vector<std::string>& words, std::string& out) {
    try {
        json::Value req = words.size() == 1 && !words[0].empty() && words[0][0] == '{'
                              ? json::parse(words[0]) : devCommandFromWords(words);
        if (!req.isObject()) throw std::runtime_error("a request is a JSON object");
        absolutePaths(req);
        out = req.dump();
        return true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "cpcse-ctl: %s\n", ex.what());
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (words.empty() && a == "--port" && i + 1 < argc) o.port = std::atoi(argv[++i]);
        else if (words.empty() && a.rfind("--port=", 0) == 0) o.port = std::atoi(a.c_str() + 7);
        else if (words.empty() && a == "--field" && i + 1 < argc) o.field = argv[++i];
        else if (words.empty() && (a == "--quiet" || a == "-q")) o.quiet = true;
        else if (words.empty() && (a == "--help" || a == "-h")) { usage(); return 0; }
        else words.push_back(a);
    }
    if (words.empty()) { usage(); return 2; }

    std::string err;
    net::Socket s = net::connectLocal(o.port, err);
    if (s == net::NO_SOCKET) { std::fprintf(stderr, "cpcse-ctl: %s\n", err.c_str()); return 2; }

    int status = 0;
    if (words.size() == 1 && words[0] == "-") {
        // A script: every line a command; stops at the first that fails.
        std::string line;
        while (std::getline(std::cin, line)) {
            size_t a = line.find_first_not_of(" \t\r");
            if (a == std::string::npos || line[a] == '#') continue;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            std::string req;
            std::vector<std::string> w = line[a] == '{' ? std::vector<std::string>{ line.substr(a) } : devSplitWords(line);
            // `type` takes the rest of the line as it stands, as the emulator's own text form does.
            if (line[a] != '{' && !w.empty() && (w[0] == "type" || w[0] == "TYPE")) {
                const std::string raw = line.size() > a + 5 ? line.substr(a + 5) : "";
                std::string text;
                for (size_t k = 0; k < raw.size(); k++) {
                    if (raw[k] == '\\' && k + 1 < raw.size() && raw[k + 1] == 'n') { text += '\n'; k++; }
                    else text += raw[k];
                }
                json::Value t = json::Value::object();
                t.set("cmd", "type");
                t.set("text", text);
                req = t.dump();
            } else if (!makeRequest(w, req)) { status = 1; break; }
            status = roundTrip(s, req, o);
            if (status) break;
        }
    } else {
        std::string req;
        if (!makeRequest(words, req)) { net::closeSocket(s); return 1; }
        status = roundTrip(s, req, o);
    }
    net::closeSocket(s);
    return status;
}
