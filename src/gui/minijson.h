// CPCSyntaxError — a small JSON value, enough for the command API (devserver.h) and its
// client (tools/ctl): objects, arrays, strings, numbers, true/false/null. Parsing throws
// std::runtime_error with the offset of the first thing it could not read.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpcse::json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> fields;   // in their written order

    Value() = default;
    Value(bool b) : type(Bool), boolean(b) {}
    Value(int n) : type(Number), number(n) {}
    Value(long long n) : type(Number), number((double)n) {}
    Value(double n) : type(Number), number(n) {}
    Value(const char* s) : type(String), text(s) {}
    Value(std::string s) : type(String), text(std::move(s)) {}
    static Value object() { Value v; v.type = Object; return v; }
    static Value array() { Value v; v.type = Array; return v; }

    bool isObject() const { return type == Object; }
    bool has(const std::string& key) const { return find(key) != nullptr; }
    const Value* find(const std::string& key) const {
        for (const auto& f : fields) if (f.first == key) return &f.second;
        return nullptr;
    }
    Value& set(const std::string& key, Value v) {
        for (auto& f : fields) if (f.first == key) { f.second = std::move(v); return f.second; }
        fields.emplace_back(key, std::move(v));
        return fields.back().second;
    }
    Value& push(Value v) { items.push_back(std::move(v)); return items.back(); }
    std::string str(const std::string& key, const std::string& otherwise = "") const {
        const Value* v = find(key);
        if (!v) return otherwise;
        if (v->type == String) return v->text;
        if (v->type == Number) { char b[32]; std::snprintf(b, sizeof(b), "%.17g", v->number); return b; }
        if (v->type == Bool) return v->boolean ? "true" : "false";
        return otherwise;
    }
    bool flag(const std::string& key, bool otherwise = false) const {
        const Value* v = find(key);
        if (!v) return otherwise;
        if (v->type == Bool) return v->boolean;
        if (v->type == Number) return v->number != 0;
        if (v->type == String) return v->text == "1" || v->text == "true" || v->text == "yes";
        return otherwise;
    }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

private:
    static void quote(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                    else out += (char)c;
            }
        }
        out += '"';
    }
    void write(std::string& out) const {
        switch (type) {
            case Null: out += "null"; break;
            case Bool: out += boolean ? "true" : "false"; break;
            case Number: {
                char b[32];
                if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9e15)
                    std::snprintf(b, sizeof(b), "%.0f", number);
                else std::snprintf(b, sizeof(b), "%.17g", number);
                out += b;
                break;
            }
            case String: quote(out, text); break;
            case Array:
                out += '[';
                for (size_t i = 0; i < items.size(); i++) { if (i) out += ','; items[i].write(out); }
                out += ']';
                break;
            case Object:
                out += '{';
                for (size_t i = 0; i < fields.size(); i++) {
                    if (i) out += ',';
                    quote(out, fields[i].first);
                    out += ':';
                    fields[i].second.write(out);
                }
                out += '}';
                break;
        }
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s(s) {}
    Value parse() {
        Value v = value();
        space();
        if (i != s.size()) fail("text after the value");
        return v;
    }

private:
    const std::string& s;
    size_t i = 0;
    [[noreturn]] void fail(const char* what) {
        throw std::runtime_error(std::string("JSON: ") + what + " at offset " + std::to_string(i));
    }
    void space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
    bool word(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (s.compare(i, n, w) == 0) { i += n; return true; }
        return false;
    }
    int depth = 0;
    struct Nest {   // objects and arrays inside each other, 64 deep at most
        Parser& p;
        explicit Nest(Parser& p) : p(p) { if (++p.depth > 64) p.fail("nested too deeply"); }
        ~Nest() { p.depth--; }
    };
    Value value() {
        Nest nest(*this);
        space();
        if (i >= s.size()) fail("missing value");
        char c = s[i];
        if (c == '{') {
            i++;
            Value o = Value::object();
            space();
            if (i < s.size() && s[i] == '}') { i++; return o; }
            for (;;) {
                space();
                if (i >= s.size() || s[i] != '"') fail("expected a key");
                std::string k = string();
                space();
                if (i >= s.size() || s[i] != ':') fail("expected ':'");
                i++;
                o.set(k, value());
                space();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return o; }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++;
            Value a = Value::array();
            space();
            if (i < s.size() && s[i] == ']') { i++; return a; }
            for (;;) {
                a.push(value());
                space();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return a; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') return Value(string());
        if (word("true")) return Value(true);
        if (word("false")) return Value(false);
        if (word("null")) return Value();
        size_t start = i;
        if (s[i] == '-') i++;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) i++;
        if (i == start) fail("unexpected character");
        const std::string text = s.substr(start, i - start);
        size_t used = 0;
        double d = 0;
        try { d = std::stod(text, &used); } catch (...) { used = 0; }
        if (used != text.size() || !std::isdigit((unsigned char)text.back())) { i = start; fail("bad number"); }
        return Value(d);
    }
    std::string string() {
        i++;   // the opening quote
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) fail("short \\u escape");
                    unsigned cp = (unsigned)std::stoul(s.substr(i, 4), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) out += (char)cp;   // UTF-8 for the rest
                    else if (cp < 0x800) { out += (char)(0xc0 | cp >> 6); out += (char)(0x80 | (cp & 0x3f)); }
                    else { out += (char)(0xe0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f)); }
                    break;
                }
                default: out += e;
            }
        }
        if (i >= s.size()) fail("unterminated string");
        i++;
        return out;
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

} // namespace cpcse::json
