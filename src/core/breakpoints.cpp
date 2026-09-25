// CPCSyntaxError — Conditional breakpoint expressions.
#include "breakpoints.h"
#include "emulator.h"
#include "z80.h"
#include "memory.h"
#include <stdexcept>
#include <memory>

namespace cpcse {

static int precedenceOf(const std::string& op) {
    static const std::unordered_map<std::string, int> P = {
        {"||",1},{"OR",1},{"&&",2},{"AND",2},{"|",3},{"^",4},{"BAND",5},
        {"==",6},{"!=",6},{"<",7},{"<=",7},{">",7},{">=",7},{"<<",8},{">>",8},
        {"+",9},{"-",9},{"*",10},{"/",10},{"%",10}
    };
    auto it = P.find(op); return it != P.end() ? it->second : -1;
}
static bool isPrecedenceOp(const std::string& v) { return precedenceOf(v) >= 0; }

struct Token { std::string type; std::string value; long long number = 0; };

static std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> tokens;
    int at = 0; int n = (int)source.size();
    auto error = [&](const std::string& message) { throw std::runtime_error(message + " at column " + std::to_string(at + 1)); };
    auto isHex = [](char c) { return std::isxdigit((unsigned char)c) != 0; };
    while (at < n) {
        char c = source[at];
        if (std::isspace((unsigned char)c)) { at += 1; continue; }
        std::string two = source.substr(at, 2);
        if (two == "&&" || two == "||" || two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "<<" || two == ">>") {
            tokens.push_back({ "op", two, 0 }); at += 2; continue;
        }
        if (std::string("()+-*/%!~<>[]|^,").find(c) != std::string::npos) { tokens.push_back({ "op", std::string(1, c), 0 }); at += 1; continue; }
        if (c == '&' || c == '$') {
            int start = ++at;
            while (at < n && isHex(source[at])) at += 1;
            if (at == start) error("Expected hexadecimal digits");
            tokens.push_back({ "number", "", (long long)std::stol(source.substr(start, at - start), nullptr, 16) }); continue;
        }
        if (c == '0' && at + 1 < n && (source[at + 1] == 'x' || source[at + 1] == 'X')) {
            at += 2; int start = at;
            while (at < n && isHex(source[at])) at += 1;
            if (at == start) error("Expected hexadecimal digits");
            tokens.push_back({ "number", "", (long long)std::stol(source.substr(start, at - start), nullptr, 16) }); continue;
        }
        if (std::isdigit((unsigned char)c)) {
            int start = at;
            while (at < n && std::isdigit((unsigned char)source[at])) at += 1;
            tokens.push_back({ "number", "", (long long)std::stol(source.substr(start, at - start), nullptr, 10) }); continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            int start = at;
            while (at < n && (std::isalnum((unsigned char)source[at]) || source[at] == '_' || source[at] == '\'')) at += 1;
            std::string value = source.substr(start, at - start);
            for (auto& ch : value) ch = (char)std::toupper((unsigned char)ch);
            tokens.push_back({ (isPrecedenceOp(value) || value == "NOT") ? "op" : "identifier", value, 0 });
            continue;
        }
        error(std::string("Unexpected character ") + c);
    }
    tokens.push_back({ "eof", "", 0 });
    return tokens;
}

struct Node {
    std::string type; // number, identifier, memory, call, unary, binary
    long long value = 0;
    std::string name;
    std::string op;
    std::shared_ptr<Node> left, right, address, child;
    std::vector<std::shared_ptr<Node>> args;
};

struct Parser {
    std::vector<Token> tokens; int index = 0;
    const Token& current() { return tokens[index]; }
    bool take(const std::string& value) { if (current().value == value) { index++; return true; } return false; }
    void expect(const std::string& value) { if (!take(value)) throw std::runtime_error("Expected " + value + ", found " + (current().value.empty() ? std::string("end of expression") : current().value)); }

    std::shared_ptr<Node> primary() {
        const Token& token = current();
        if (token.type == "number") { long long v = token.number; index += 1; auto nd = std::make_shared<Node>(); nd->type = "number"; nd->value = v; return nd; }
        if (take("(")) { auto node = expression(0); expect(")"); return node; }
        if (take("[")) { auto address = expression(0); expect("]"); auto nd = std::make_shared<Node>(); nd->type = "memory"; nd->address = address; return nd; }
        if (token.type == "identifier") {
            index += 1;
            std::string name = token.value;
            if (take("(")) {
                auto nd = std::make_shared<Node>(); nd->type = "call"; nd->name = name;
                if (!take(")")) { do { nd->args.push_back(expression(0)); } while (take(",")); expect(")"); }
                return nd;
            }
            auto nd = std::make_shared<Node>(); nd->type = "identifier"; nd->name = name; return nd;
        }
        throw std::runtime_error("Expected a number, register or sub-expression, found " + (token.value.empty() ? std::string("end of expression") : token.value));
    }
    std::shared_ptr<Node> unary() {
        const Token& token = current();
        if (token.type == "op" && (token.value == "!" || token.value == "NOT" || token.value == "~" || token.value == "+" || token.value == "-")) {
            index += 1; auto nd = std::make_shared<Node>(); nd->type = "unary"; nd->op = token.value; nd->child = unary(); return nd;
        }
        return primary();
    }
    std::shared_ptr<Node> expression(int minPrecedence) {
        auto left = unary();
        while (true) {
            const Token& token = current();
            int precedence = token.type == "op" ? precedenceOf(token.value) : -1;
            if (precedence < 0 || precedence < minPrecedence) break;
            std::string op = token.value; index += 1;
            auto right = expression(precedence + 1);
            auto nd = std::make_shared<Node>(); nd->type = "binary"; nd->op = op; nd->left = left; nd->right = right; left = nd;
        }
        return left;
    }
};

static std::shared_ptr<Node> parseExpr(const std::string& source) {
    Parser p; p.tokens = tokenize(source);
    auto tree = p.expression(0);
    if (p.current().type != "eof") throw std::runtime_error("Unexpected token " + p.current().value);
    return tree;
}

static long long valueOf(const std::shared_ptr<Node>& node, const BreakpointContext& context) {
    if (node->type == "number") return node->value;
    if (node->type == "identifier") {
        auto it = context.values.find(node->name);
        if (it == context.values.end()) throw std::runtime_error("Unknown register/value " + node->name);
        return it->second;
    }
    if (node->type == "memory") return (context.read ? context.read(valueOf(node->address, context) & 0xffff) : 0xff) & 0xff;
    if (node->type == "call") {
        std::vector<long long> args; for (auto& a : node->args) args.push_back(valueOf(a, context));
        auto rd = [&](int addr) { return (context.read ? context.read(addr & 0xffff) : 0xff) & 0xff; };
        if ((node->name == "MEM" || node->name == "BYTE") && args.size() == 1) return rd((int)args[0]);
        if (node->name == "WORD" && args.size() == 1) return rd((int)args[0]) | (rd((int)args[0] + 1) << 8);
        if (node->name == "BIT" && args.size() == 2) return ((unsigned long long)args[0] >> (args[1] & 31)) & 1;
        throw std::runtime_error("Unknown function or wrong argument count: " + node->name);
    }
    if (node->type == "unary") {
        long long value = valueOf(node->child, context);
        if (node->op == "!" || node->op == "NOT") return value ? 0 : 1;
        if (node->op == "~") return ~value;
        if (node->op == "-") return -value;
        return value;
    }
    if (node->type == "binary") {
        if (node->op == "&&" || node->op == "AND") return valueOf(node->left, context) ? (valueOf(node->right, context) ? 1 : 0) : 0;
        if (node->op == "||" || node->op == "OR") return valueOf(node->left, context) ? 1 : (valueOf(node->right, context) ? 1 : 0);
        long long left = valueOf(node->left, context);
        long long right = valueOf(node->right, context);
        const std::string& op = node->op;
        if (op == "|") return left | right;
        if (op == "^") return left ^ right;
        if (op == "BAND") return left & right;
        if (op == "==") return left == right ? 1 : 0;
        if (op == "!=") return left != right ? 1 : 0;
        if (op == "<") return left < right ? 1 : 0;
        if (op == "<=") return left <= right ? 1 : 0;
        if (op == ">") return left > right ? 1 : 0;
        if (op == ">=") return left >= right ? 1 : 0;
        if (op == "<<") return left << (right & 31);
        if (op == ">>") return left >> (right & 31);
        if (op == "+") return left + right;
        if (op == "-") return left - right;
        if (op == "*") return left * right;
        if (op == "/") return right == 0 ? 0 : (long long)std::trunc((double)left / right);
        if (op == "%") return right == 0 ? 0 : left % right;
        throw std::runtime_error("Unsupported operator " + op);
    }
    throw std::runtime_error("Unsupported expression node " + node->type);
}

std::function<bool(const BreakpointContext&)> compileBreakpointCondition(const std::string& source) {
    std::string condition = source;
    { size_t a = 0, b = condition.size(); while (a < b && std::isspace((unsigned char)condition[a])) a++; while (b > a && std::isspace((unsigned char)condition[b - 1])) b--; condition = condition.substr(a, b - a); }
    if (condition.empty()) return [](const BreakpointContext&) { return true; };
    auto tree = parseExpr(condition);
    return [tree](const BreakpointContext& context) { return !!valueOf(tree, context); };
}

BreakpointContext breakpointContext(GX4000* emulator, int hits, const std::unordered_map<std::string, long long>& extra) {
    BreakpointContext ctx;
    Z80* cpu = emulator ? emulator->cpu : nullptr;
    int flags = cpu ? cpu->f : 0;
    MemoryIdentity identity; bool hasIdentity = false;
    if (emulator && emulator->memory && cpu) { identity = emulator->memory->debugIdentity(cpu->pc); hasIdentity = true; }
    auto& v = ctx.values;
    if (cpu) {
        v["A"] = cpu->a; v["F"] = cpu->f; v["B"] = cpu->b; v["C"] = cpu->c; v["D"] = cpu->d; v["E"] = cpu->e; v["H"] = cpu->h; v["L"] = cpu->l;
        v["AF"] = cpu->af(); v["BC"] = cpu->bc(); v["DE"] = cpu->de(); v["HL"] = cpu->hl(); v["IX"] = cpu->ix; v["IY"] = cpu->iy;
        v["SP"] = cpu->sp; v["PC"] = cpu->pc; v["I"] = cpu->i; v["R"] = cpu->r; v["IM"] = cpu->im;
        v["AF2"] = (cpu->ap << 8) | cpu->fp; v["BC2"] = (cpu->bp << 8) | cpu->cp; v["DE2"] = (cpu->dp << 8) | cpu->ep; v["HL2"] = (cpu->hp << 8) | cpu->lp;
    }
    v["SF"] = !!(flags & 0x80); v["ZF"] = !!(flags & 0x40); v["HF"] = !!(flags & 0x10);
    v["PF"] = !!(flags & 0x04); v["VF"] = !!(flags & 0x04); v["NF"] = !!(flags & 0x02); v["CF"] = !!(flags & 0x01);
    v["HITS"] = hits;
    v["BANK"] = (hasIdentity && identity.kind == "ram") ? identity.bank : -1;
    v["ROM"] = (hasIdentity && identity.kind == "rom") ? identity.bank : -1;
    v["MMR"] = emulator && emulator->memory ? emulator->memory->ramConfig : 0;
    v["TRUE"] = 1; v["FALSE"] = 0;
    for (auto& kv : extra) v[kv.first] = kv.second;
    GX4000* em = emulator;
    ctx.read = [em](int address) { return (em && em->memory) ? em->memory->read(address & 0xffff) : 0xff; };
    return ctx;
}

} // namespace cpcse
