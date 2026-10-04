// CPCSyntaxError — the command API's short form: "load game.bin &4000 entry" -> a JSON
// request. Shared by the server (a line that is not JSON) and tools/ctl (its arguments),
// so the two can never disagree about which word is which field.
#pragma once
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

#include "minijson.h"

namespace cpcse {

// The fields each command's bare words fill, in order. A free-text last field (`type`'s
// text, `bp`'s condition) takes every word left over, joined by spaces, so `type PRINT 6*7`
// and `bp add &4000 A==3 && B==0` need no quotes; any other command given more words than
// it has fields is refused.
inline std::vector<std::string> devCommandPositionals(const std::string& cmd) {
    if (cmd == "load" || cmd == "watch") return { "path", "addr", "run" };
    if (cmd == "read") return { "addr", "len" };
    if (cmd == "write") return { "addr", "data" };
    if (cmd == "setreg") return { "reg", "value" };
    if (cmd == "step" || cmd == "over" || cmd == "out") return { "count" };
    if (cmd == "run" || cmd == "runto") return { "addr" };
    if (cmd == "type") return { "text" };
    if (cmd == "key") return { "key" };
    if (cmd == "screenshot" || cmd == "symbols") return { "path" };
    if (cmd == "bp") return { "action", "addr", "condition" };
    if (cmd == "wp") return { "action", "start", "end", "mode" };
    if (cmd == "wait") return { "for", "n" };
    if (cmd == "disasm") return { "addr", "count" };
    if (cmd == "frames") return { "n" };
    return {};
}

// words[0] is the command; name=value words set a field by name; "\n" in text and
// command fields becomes Enter. Throws std::runtime_error for words left over.
inline json::Value devCommandFromWords(const std::vector<std::string>& words) {
    json::Value req = json::Value::object();
    if (words.empty()) return req;
    std::string cmd = words[0];
    for (char& c : cmd) c = (char)std::tolower((unsigned char)c);
    req.set("cmd", cmd);
    const std::vector<std::string> names = devCommandPositionals(cmd);
    const bool freeText = !names.empty() && (names.back() == "text" || names.back() == "condition");
    const size_t single = freeText ? names.size() - 1 : names.size();   // fields of one word each
    size_t next = 0;
    std::string rest;
    // Only the API's own field names count as name=value, and not once the free text has
    // begun: `type PRINT A=3` types A=3, `bp add &4000 A==3` is a condition.
    static const char* const fields[] = { "path", "addr", "run", "unit", "reset", "command", "symbols",
        "len", "data", "reg", "value", "count", "text", "key", "action", "condition", "start", "end",
        "mode", "for", "n", "timeout", "format", "shift", "wait", "id" };
    for (size_t i = 1; i < words.size(); i++) {
        const std::string& w = words[i];
        size_t eq = w.find('=');
        bool named = false;
        if (rest.empty() && eq != std::string::npos && (eq + 1 >= w.size() || w[eq + 1] != '='))
            for (const char* f : fields) if (w.compare(0, eq, f) == 0 && std::char_traits<char>::length(f) == eq) named = true;
        if (named) { req.set(w.substr(0, eq), w.substr(eq + 1)); continue; }
        if (next < single) { req.set(names[next++], w); continue; }
        if (freeText) { if (!rest.empty()) rest += ' '; rest += w; continue; }
        throw std::runtime_error("'" + w + "': " + cmd + " takes " +
                                 (names.empty() ? std::string("no more words") : std::to_string(names.size()) + " at most") +
                                 " (name=value for the others)");
    }
    if (!rest.empty()) req.set(names.back(), rest);
    for (const char* k : { "text", "command" }) {
        const json::Value* v = req.find(k);
        if (!v || v->type != json::Value::String) continue;
        std::string t;
        for (size_t i = 0; i < v->text.size(); i++) {
            if (v->text[i] == '\\' && i + 1 < v->text.size() && v->text[i + 1] == 'n') { t += '\n'; i++; }
            else t += v->text[i];
        }
        req.set(k, t);
    }
    return req;
}

// A command line split into words: spaces separate, "double quotes" group (a quote
// inside a word stays in it: RUN"GAME is one word).
inline std::vector<std::string> devSplitWords(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inWord = false, quoted = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (quoted) { if (c == '"') quoted = false; else cur += c; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (inWord) { out.push_back(cur); cur.clear(); inWord = false; }
            continue;
        }
        if (c == '"' && !inWord) { quoted = true; inWord = true; continue; }
        cur += c;
        inWord = true;
    }
    if (inWord) out.push_back(cur);
    return out;
}

} // namespace cpcse
