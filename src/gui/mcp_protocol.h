// CPCSyntaxError: the Model Context Protocol (MCP) face of the command API.
//
// An AI agent (Claude Code, Claude Desktop, Cursor, VS Code ...) connects to
// http://127.0.0.1:<port>/mcp with the "Streamable HTTP" transport and calls TOOLS.
// Every tool here is one command-API request (devserver.cpp apiHandle), so the agent can
// do what cpcse-ctl does: load and run code, type, read and write memory and registers,
// set breakpoints, step, wait for a stop and look at the screen.
//
// This header is only the protocol: the tool list with its input schemas, the mapping of
// a tools/call to an API request, and of the API's answer to a tool result. DevServer runs
// them; tools/dev_check tests them without a socket.
#pragma once
#include "minijson.h"

#include <string>

namespace cpcse::mcp {

// The protocol versions this server answers with; the first is the newest.
inline const char* const PROTOCOL_VERSIONS[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

inline std::string negotiateVersion(const std::string& asked) {
    for (const char* v : PROTOCOL_VERSIONS) if (asked == v) return v;
    return PROTOCOL_VERSIONS[0];
}

inline const char* instructions() {
    return "CPCSyntaxError is an Amstrad CPC / CPC Plus / GX4000 emulator. These tools drive the running "
           "machine. Addresses accept &4000, #4000, $4000, 0x4000, 16384 or a symbol label. Typical loop: "
           "load_file (a .bin with run, or a .dsk/.cpr/.sna), type_text to give BASIC commands (\\n is Enter), "
           "run_frames to let it run (50 frames = 1 second), screenshot to see the screen, read_memory and "
           "get_registers to inspect, breakpoint + resume + wait_for_stop to debug. pause stops the machine; "
           "run_frames needs it running.";
}

struct Tool { const char* name; const char* description; const char* schema; };

// The input schemas are JSON Schema objects, written as JSON text.
inline const Tool* tools(int& count) {
    static const Tool list[] = {
        { "emulator_status", "The machine model, whether it runs or is paused (and why), the PC, frames run, servers.",
          R"J({"type":"object","properties":{}})J" },
        { "pause", "Stop the machine (as the debugger's pause). Answers the PC.",
          R"J({"type":"object","properties":{}})J" },
        { "resume", "Let the machine run again, optionally from a new PC.",
          R"J({"type":"object","properties":{"addr":{"type":"string","description":"start here (sets the PC)"}}})J" },
        { "step", "Execute instructions with the machine paused and answer the registers.",
          R"J({"type":"object","properties":{"count":{"type":"integer","minimum":1,"maximum":1000000,"default":1}}})J" },
        { "step_over", "Run over the next instruction (a CALL, RST or block loop runs to its return) and wait for the stop.",
          R"J({"type":"object","properties":{"timeout_ms":{"type":"integer","default":10000}}})J" },
        { "step_out", "Run until the current routine returns and wait for the stop.",
          R"J({"type":"object","properties":{"timeout_ms":{"type":"integer","default":10000}}})J" },
        { "run_to", "Run until the PC reaches an address and wait for the stop.",
          R"J({"type":"object","properties":{"addr":{"type":"string"},"timeout_ms":{"type":"integer","default":10000}},"required":["addr"]})J" },
        { "reset", "Reset the machine (as the power switch). By default it runs on afterwards.",
          R"J({"type":"object","properties":{"run":{"type":"boolean","default":true}}})J" },
        { "get_registers", "The Z80 registers (pc sp af bc de hl ix iy af' bc' de' hl' i r im iff1 iff2) and flags.",
          R"J({"type":"object","properties":{}})J" },
        { "set_registers", "Set Z80 registers, for example {\"regs\":{\"pc\":\"&4000\",\"a\":\"5\"}}.",
          R"J({"type":"object","properties":{"regs":{"type":"object","additionalProperties":{"type":"string"}}},"required":["regs"]})J" },
        { "read_memory", "Read memory as the Z80 sees it now (current ROM/RAM mapping). Answers hex and a hex dump.",
          R"J({"type":"object","properties":{"addr":{"type":"string"},"length":{"type":"integer","minimum":1,"maximum":65536,"default":64}},"required":["addr"]})J" },
        { "write_memory", "Write bytes (hex text, \"3E 01 C9\") at an address.",
          R"J({"type":"object","properties":{"addr":{"type":"string"},"data":{"type":"string"}},"required":["addr","data"]})J" },
        { "load_file", "Load a file from this computer: .bin (at addr or its AMSDOS header address, optionally run), .sna, .dsk/.hfe (drive A or B), .cpr/.cro cartridge, tape (.cdt/.tzx/.wav). Path is on the host.",
          R"J({"type":"object","properties":{"path":{"type":"string"},"addr":{"type":"string","description":".bin load address"},"run":{"type":"string","description":"'entry', an address, or empty for no run"},"reset":{"type":"boolean"},"command":{"type":"string","description":"typed after loading, \\n = Enter"},"unit":{"type":"integer","enum":[0,1]},"symbols":{"type":"string","description":"symbol file path"}},"required":["path"]})J" },
        { "load_symbols", "Read a symbol file (RASM, sjasmplus, pasmo, WinAPE, name EQU value) for labels.",
          R"J({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})J" },
        { "type_text", "Type text on the CPC keyboard (\\n is Enter). With wait, answer when it is all typed.",
          R"J({"type":"object","properties":{"text":{"type":"string"},"wait":{"type":"boolean","default":true},"timeout_ms":{"type":"integer","default":60000}},"required":["text"]})J" },
        { "press_key", "Press one key: Space, Enter, Escape, KeyA, Digit1, F1, ArrowUp, Copy, ...",
          R"J({"type":"object","properties":{"key":{"type":"string"},"shift":{"type":"boolean"}},"required":["key"]})J" },
        { "screenshot", "The screen as a PNG image. With path, it is also saved there.",
          R"J({"type":"object","properties":{"path":{"type":"string"}}})J" },
        { "breakpoint", "Breakpoints: action add (addr, optional condition such as \"a==5\"), remove (addr), clear or list.",
          R"J({"type":"object","properties":{"action":{"type":"string","enum":["add","remove","clear","list"]},"addr":{"type":"string"},"condition":{"type":"string"}},"required":["action"]})J" },
        { "watchpoint", "Watchpoints on memory: action add (addr, length or end, mode r/w/rw), remove (addr), clear or list.",
          R"J({"type":"object","properties":{"action":{"type":"string","enum":["add","remove","clear","list"]},"addr":{"type":"string"},"end":{"type":"string"},"length":{"type":"integer"},"mode":{"type":"string","enum":["r","w","rw"]}},"required":["action"]})J" },
        { "disassemble", "Disassemble Z80 code (default: from the PC).",
          R"J({"type":"object","properties":{"addr":{"type":"string"},"count":{"type":"integer","minimum":1,"maximum":1000,"default":16}}})J" },
        { "wait_for_stop", "Wait until the machine stops (breakpoint, watchpoint, step end). Answers PC and reason.",
          R"J({"type":"object","properties":{"timeout_ms":{"type":"integer","default":10000}}})J" },
        { "run_frames", "Let the machine run this many frames (50 per second) and answer after them.",
          R"J({"type":"object","properties":{"frames":{"type":"integer","minimum":0,"default":50},"timeout_ms":{"type":"integer","default":600000}}})J" },
        { "api_command", "Any command-API request in its short form, as cpcse-ctl takes it (\"help\" lists them).",
          R"J({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})J" },
    };
    count = (int)(sizeof(list) / sizeof(list[0]));
    return list;
}

inline json::Value toolList() {
    json::Value arr = json::Value::array();
    int n = 0;
    const Tool* t = tools(n);
    for (int i = 0; i < n; i++) {
        json::Value o = json::Value::object();
        o.set("name", t[i].name);
        o.set("description", t[i].description);
        o.set("inputSchema", json::parse(t[i].schema));
        arr.push(o);
    }
    return arr;
}

// An argument as the API takes it: numbers and strings pass through.
inline void copyArg(const json::Value& args, const char* from, json::Value& req, const char* to) {
    if (const json::Value* v = args.find(from)) req.set(to, *v);
}

// The API request for a tools/call. False with the reason for an unknown tool. A request
// that is a short-form command line comes back in `shortForm` instead (api_command).
inline bool toApiRequest(const std::string& name, const json::Value& args, json::Value& req,
                         std::string& shortForm, std::string& error) {
    req = json::Value::object();
    auto cmd = [&](const char* c) { req.set("cmd", c); };
    if (name == "emulator_status") cmd("status");
    else if (name == "pause") cmd("pause");
    else if (name == "resume") { cmd("run"); copyArg(args, "addr", req, "addr"); }
    else if (name == "step") { cmd("step"); copyArg(args, "count", req, "count"); }
    else if (name == "step_over") { cmd("over"); copyArg(args, "timeout_ms", req, "timeout"); }
    else if (name == "step_out") { cmd("out"); copyArg(args, "timeout_ms", req, "timeout"); }
    else if (name == "run_to") { cmd("runto"); copyArg(args, "addr", req, "addr"); copyArg(args, "timeout_ms", req, "timeout"); }
    else if (name == "reset") { cmd("reset"); copyArg(args, "run", req, "run"); }
    else if (name == "get_registers") cmd("regs");
    else if (name == "set_registers") { cmd("setreg"); copyArg(args, "regs", req, "regs"); }
    else if (name == "read_memory") { cmd("read"); copyArg(args, "addr", req, "addr"); req.set("len", 64); copyArg(args, "length", req, "len"); }
    else if (name == "write_memory") { cmd("write"); copyArg(args, "addr", req, "addr"); copyArg(args, "data", req, "data"); }
    else if (name == "load_file") {
        cmd("load");
        for (const char* k : { "path", "addr", "reset", "command", "unit", "symbols" }) copyArg(args, k, req, k);
        if (const json::Value* r = args.find("run")) { if (!(r->type == json::Value::String && r->text.empty())) req.set("run", *r); }
    }
    else if (name == "load_symbols") { cmd("symbols"); copyArg(args, "path", req, "path"); }
    else if (name == "type_text") { cmd("type"); copyArg(args, "text", req, "text"); req.set("wait", true); copyArg(args, "wait", req, "wait"); copyArg(args, "timeout_ms", req, "timeout"); }
    else if (name == "press_key") { cmd("key"); copyArg(args, "key", req, "key"); copyArg(args, "shift", req, "shift"); }
    else if (name == "screenshot") { cmd("screenshot"); copyArg(args, "path", req, "path"); }
    else if (name == "breakpoint") { cmd("bp"); for (const char* k : { "action", "addr", "condition" }) copyArg(args, k, req, k); }
    else if (name == "watchpoint") { cmd("wp"); for (const char* k : { "action", "addr", "end", "mode" }) copyArg(args, k, req, k); copyArg(args, "length", req, "len"); }
    else if (name == "disassemble") { cmd("disasm"); copyArg(args, "addr", req, "addr"); req.set("count", 16); copyArg(args, "count", req, "count"); }
    else if (name == "wait_for_stop") { cmd("wait"); req.set("for", "stop"); copyArg(args, "timeout_ms", req, "timeout"); }
    else if (name == "run_frames") { cmd("wait"); req.set("for", "frames"); req.set("n", 50); copyArg(args, "frames", req, "n"); copyArg(args, "timeout_ms", req, "timeout"); }
    else if (name == "api_command") { shortForm = args.str("command"); if (shortForm.empty()) { error = "command is empty"; return false; } }
    else { error = "unknown tool '" + name + "'"; return false; }
    return true;
}

// 16 bytes per line: "4000  3E 01 C9 ...  >..<"
inline std::string hexDump(int addr, const std::string& hex) {
    std::string out;
    const size_t n = hex.size() / 2;
    for (size_t row = 0; row < n; row += 16) {
        char head[8];
        std::snprintf(head, sizeof(head), "%04X ", (unsigned)((addr + (int)row) & 0xffff));
        std::string bytes, text;
        for (size_t k = row; k < std::min(n, row + 16); k++) {
            const int v = std::stoi(hex.substr(k * 2, 2), nullptr, 16);
            bytes += ' '; bytes += hex.substr(k * 2, 2);
            text += (v >= 32 && v < 127) ? (char)v : '.';
        }
        out += head + bytes + std::string((16 - std::min<size_t>(16, n - row)) * 3, ' ') + "  " + text + "\n";
    }
    return out;
}

// A tool result from the API's answer: the answer as JSON text (and a hex dump for
// read_memory), isError when the API said ok:false.
inline json::Value toolResult(const std::string& name, const json::Value& apiRes) {
    json::Value body = json::Value::object();
    for (const auto& f : apiRes.fields) if (f.first != "ok" && f.first != "id") body.set(f.first, f.second);
    const bool ok = apiRes.flag("ok", true);
    json::Value content = json::Value::array();
    json::Value text = json::Value::object();
    text.set("type", "text");
    std::string t = ok ? body.dump() : "error: " + apiRes.str("error", "failed");
    if (ok && name == "read_memory" && apiRes.has("data")) t += "\n" + hexDump((int)apiRes.find("addr")->number, apiRes.str("data"));
    text.set("text", t);
    content.push(text);
    json::Value r = json::Value::object();
    r.set("content", content);
    if (!ok) r.set("isError", true);
    return r;
}

} // namespace cpcse::mcp
