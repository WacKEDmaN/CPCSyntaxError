// CPCSyntaxError — CPC keyboard matrix.
#include "keyboard.h"

namespace cpcse {

static const std::unordered_map<std::string, KeyCell>& keyMap() {
    static const std::unordered_map<std::string, KeyCell> m = {
        {"ArrowUp",{0,0}},{"ArrowRight",{0,1}},{"ArrowDown",{0,2}},{"ArrowLeft",{1,0}},
        {"Enter",{2,2}},{"Space",{5,7}},{"Escape",{8,2}},{"ShiftLeft",{2,5}},{"ShiftRight",{2,5}},
        {"F1",{1,5}},{"F2",{1,6}},{"F3",{0,5}},{"F4",{2,4}},{"F5",{1,4}},
        {"F6",{0,4}},{"F7",{1,2}},{"F8",{1,3}},{"F9",{0,3}},{"F10",{1,7}},
        {"ControlLeft",{2,7}},{"ControlRight",{2,7}},{"AltLeft",{1,1}},{"AltRight",{2,1}},
        {"Tab",{8,4}},{"Backspace",{9,7}},{"CapsLock",{8,6}},
        {"Numpad0",{1,7}},{"Numpad1",{1,5}},{"Numpad2",{1,6}},{"Numpad3",{0,5}},{"Numpad4",{2,4}},
        {"Numpad5",{1,4}},{"Numpad6",{0,4}},{"Numpad7",{1,2}},{"Numpad8",{1,3}},{"Numpad9",{0,3}},
        {"NumpadDecimal",{0,7}},{"NumpadEnter",{0,6}},{"Delete",{2,0}},
        {"KeyA",{8,5}},{"KeyB",{6,6}},{"KeyC",{7,6}},{"KeyD",{7,5}},{"KeyE",{7,2}},{"KeyF",{6,5}},
        {"KeyG",{6,4}},{"KeyH",{5,4}},{"KeyI",{4,3}},{"KeyJ",{5,5}},{"KeyK",{4,5}},{"KeyL",{4,4}},
        {"KeyM",{4,6}},{"KeyN",{5,6}},{"KeyO",{4,2}},{"KeyP",{3,3}},{"KeyQ",{8,3}},{"KeyR",{6,2}},
        {"KeyS",{7,4}},{"KeyT",{6,3}},{"KeyU",{5,2}},{"KeyV",{6,7}},{"KeyW",{7,3}},{"KeyX",{7,7}},
        {"KeyY",{5,3}},{"KeyZ",{8,7}},
        {"Digit1",{8,0}},{"Digit2",{8,1}},{"Digit3",{7,1}},{"Digit4",{7,0}},{"Digit5",{6,1}},
        {"Digit6",{6,0}},{"Digit7",{5,1}},{"Digit8",{5,0}},{"Digit9",{4,1}},{"Digit0",{4,0}},
        {"Comma",{4,7}},{"Period",{3,7}},{"Slash",{3,6}},{"Semicolon",{3,5}},{"Quote",{3,4}},
        {"BracketLeft",{3,2}},{"BracketRight",{2,3}},{"Backslash",{2,6}},{"Minus",{3,1}},{"Equal",{3,0}}
    };
    return m;
}

static const std::unordered_map<std::string, int>& joystickKeys() {
    static const std::unordered_map<std::string, int> m = {
        {"ArrowUp",0},{"ArrowDown",1},{"ArrowLeft",2},{"ArrowRight",3},
        {"ControlLeft",4},{"ControlRight",4},{"AltLeft",5},{"AltRight",5}
    };
    return m;
}

static const std::unordered_map<std::string, CharCell>& symbolMap() {
    static const std::unordered_map<std::string, CharCell> m = {
        {"!",{8,0,1}},{"\"",{8,1,1}},{"#",{7,1,1}},{"$",{7,0,1}},{"%",{6,1,1}},{"&",{6,0,1}},
        {"'",{5,1,1}},{"(",{5,0,1}},{")",{4,1,1}},{"*",{3,5,1}},{"+",{3,4,1}},
        {",",{4,7,0}},{"-",{3,1,0}},{".",{3,7,0}},{"/",{3,6,0}},
        {":",{3,5,0}},{";",{3,4,0}},{"<",{4,7,1}},{"=",{3,1,1}},{">",{3,7,1}},{"?",{3,6,1}},
        {"@",{3,2,0}},{"[",{2,1,0}},{"\\",{2,6,0}},{"]",{2,3,0}},{"^",{3,0,0}},{"_",{4,0,1}},
        {"|",{3,2,1}},{"{",{2,1,1}},{"}",{2,3,1}},{"~",{2,6,1}}
    };
    return m;
}

static bool isCpcPhysicalCode(const std::string& code) { return code == "Minus"; }

static std::string toLowerStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::tolower((unsigned char)c); return r; }
static std::string toUpperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string normalizeCpcKeyboardRegion(const std::string& region) {
    std::string r = region.empty() ? "uk" : region;
    r = toLowerStr(trimStr(r));
    // split on '-' or '_', take first token
    std::string base = r;
    for (size_t i = 0; i < r.size(); i++) if (r[i] == '-' || r[i] == '_') { base = r.substr(0, i); break; }
    return base == "fr" || base == "es" ? base : "uk";
}

using RegionMap = std::unordered_map<std::string, CharCell>;

static RegionMap buildRegionalCharacterMap(const std::string& region) {
    RegionMap map;
    auto pairFn = [&](int row, int bit, const std::string& normal, const std::string& shifted) {
        if (!normal.empty()) map[normal] = { row, bit, 0 };
        if (!shifted.empty()) map[shifted] = { row, bit, 1 };
    };
    auto letter = [&](int row, int bit, const std::string& value) {
        map[toLowerStr(value)] = { row, bit, 0 };
        map[toUpperStr(value)] = { row, bit, 1 };
    };
    if (region == "fr") {
        pairFn(8,0,"&","1"); pairFn(8,1,"é","2"); pairFn(7,1,"\"","3"); pairFn(7,0,"'","4");
        pairFn(6,1,"(","5"); pairFn(6,0,"]","6"); pairFn(5,1,"è","7"); pairFn(5,0,"!","8");
        pairFn(4,1,"ç","9"); pairFn(4,0,"à","0"); pairFn(3,1,")","["); pairFn(3,0,"-","_");
        const std::array<std::tuple<int,int,std::string>,26> letters = {{ {8,3,"A"},{7,3,"Z"},{7,2,"E"},{6,2,"R"},{6,3,"T"},{5,3,"Y"},{5,2,"U"},{4,3,"I"},{4,2,"O"},{3,3,"P"},{8,5,"Q"},{7,4,"S"},{7,5,"D"},{6,5,"F"},{6,4,"G"},{5,4,"H"},{5,5,"J"},{4,5,"K"},{4,4,"L"},{3,5,"M"},{8,7,"W"},{7,7,"X"},{7,6,"C"},{6,7,"V"},{6,6,"B"},{5,6,"N"} }};
        for (auto& t : letters) letter(std::get<0>(t), std::get<1>(t), std::get<2>(t));
        pairFn(3,2,"^","¦"); pairFn(2,1,"*","<"); pairFn(3,4,"ù","%"); pairFn(2,3,"#",">");
        pairFn(4,6,",","?"); pairFn(4,7,";","."); pairFn(3,7,":","/"); pairFn(3,6,"=","+"); pairFn(2,6,"$","@");
    } else if (region == "es") {
        const std::array<std::tuple<int,int,std::string>,27> letters = {{ {8,3,"Q"},{7,3,"W"},{7,2,"E"},{6,2,"R"},{6,3,"T"},{5,3,"Y"},{5,2,"U"},{4,3,"I"},{4,2,"O"},{3,3,"P"},{8,5,"A"},{7,4,"S"},{7,5,"D"},{6,5,"F"},{6,4,"G"},{5,4,"H"},{5,5,"J"},{4,5,"K"},{4,4,"L"},{8,7,"Z"},{7,7,"X"},{7,6,"C"},{6,7,"V"},{6,6,"B"},{5,6,"N"},{4,6,"M"} }};
        for (auto& t : letters) letter(std::get<0>(t), std::get<1>(t), std::get<2>(t));
        const std::array<std::tuple<int,int,std::string,std::string>,20> pairs = {{ {8,0,"1","!"},{8,1,"2","\""},{7,1,"3","#"},{7,0,"4","$"},{6,1,"5","%"},{6,0,"6","&"},{5,1,"7","'"},{5,0,"8","("},{4,1,"9",")"},{4,0,"0","_"},{3,1,"-","="},{3,0,"^","₧"},{3,2,"@","¦"},{2,1,"[","*"},{3,4,";",":"},{2,3,"]","+"},{4,7,",","<"},{3,7,".",">"},{3,6,"/","?"},{2,6,"\\","`"} }};
        for (auto& t : pairs) pairFn(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::get<3>(t));
        map["ñ"] = { 3,5,0 }; map["Ñ"] = { 3,5,1 };
    }
    return map;
}

static const RegionMap& regionalMap(const std::string& region) {
    static const RegionMap fr = buildRegionalCharacterMap("fr");
    static const RegionMap es = buildRegionalCharacterMap("es");
    static const RegionMap empty;
    if (region == "fr") return fr;
    if (region == "es") return es;
    return empty;
}

std::optional<CharCell> cpcCharacterMapping(const std::string& character, const std::string& code, const std::string& regionIn) {
    std::string region = normalizeCpcKeyboardRegion(regionIn);
    if (character.empty()) return std::nullopt;
    if (region != "uk") {
        const RegionMap& m = regionalMap(region);
        auto it = m.find(character);
        return it != m.end() ? std::optional<CharCell>(it->second) : std::nullopt;
    }
    if (isCpcPhysicalCode(code)) return std::nullopt;
    if (code == "Digit0" && character == "=") {
        const KeyCell& cell = keyMap().at("Digit0");
        return CharCell{ cell[0], cell[1], 1 };
    }
    auto sit = symbolMap().find(character);
    if (sit != symbolMap().end()) return sit->second;
    if (character.size() == 1) {
        std::string upper = toUpperStr(character);
        char u = upper[0];
        if (u >= 'A' && u <= 'Z') {
            auto it = keyMap().find(std::string("Key") + u);
            if (it != keyMap().end()) return CharCell{ it->second[0], it->second[1], character == upper ? 1 : 0 };
            return std::nullopt;
        }
        char ch = character[0];
        if (ch >= '0' && ch <= '9') {
            auto it = keyMap().find(std::string("Digit") + ch);
            if (it != keyMap().end()) return CharCell{ it->second[0], it->second[1], 0 };
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static const std::unordered_map<std::string, std::string>& numpadNavOff() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Numpad8","ArrowUp"},{"Numpad2","ArrowDown"},{"Numpad4","ArrowLeft"},{"Numpad6","ArrowRight"},
        {"Numpad7","Home"},{"Numpad1","End"},{"Numpad9","PageUp"},{"Numpad3","PageDown"},
        {"Numpad0","Insert"},{"NumpadDecimal","Delete"},{"Numpad5","Clear"}
    };
    return m;
}
static bool numpadNavChar(const std::string& c) {
    static const std::unordered_set<std::string> s = { "ArrowUp","ArrowDown","ArrowLeft","ArrowRight","Home","End","PageUp","PageDown","Insert","Delete","Clear" };
    return s.count(c) != 0;
}

KeyboardMatrix::KeyboardMatrix() { joystickEnabled = true; region = "uk"; reset(); }

void KeyboardMatrix::reset() {
    std::string savedLightgunMode = lightgunMode;
    std::string savedRegion = normalizeCpcKeyboardRegion(region);
    rows.fill(0xff); keyboardJoystick = 0xff; gamepadJoystick = 0xff; keyboardJoystick1 = 0xff; gamepadJoystick1 = 0xff; lightgunJoystick = 0xff;
    bool savedLightgunEnabled = lightgunEnabled;
    lightgunMode = savedLightgunMode; region = savedRegion; lightgunEnabled = savedLightgunEnabled;
    lightgunBeamHit = false; lightgunTrigger = false; gunstickTriggerPending = false; gunstickReadCounter = 0; selectedRow = 0;
    pressedKeys.clear(); pressedSymbols.clear(); directBits.clear(); codeToEffective.clear();
}

std::string KeyboardMatrix::setRegion(const std::string& regionIn) {
    std::string selected = normalizeCpcKeyboardRegion(regionIn);
    if (selected == region) return selected;
    region = selected;
    pressedKeys.clear(); pressedSymbols.clear(); directBits.clear(); codeToEffective.clear();
    rebuildKeyboardState();
    return selected;
}
void KeyboardMatrix::setJoystickEnabled(bool enabled) {
    joystickEnabled = enabled;
    keyboardJoystick = 0xff; gamepadJoystick = 0xff; keyboardJoystick1 = 0xff; gamepadJoystick1 = 0xff;
    rebuildKeyboardState();
}
void KeyboardMatrix::rebuildKeyboardState() {
    rows.fill(0xff); keyboardJoystick = 0xff; keyboardJoystick1 = 0xff;
    bool symbolActive = pressedSymbols.size() != 0;
    for (const std::string& code : pressedKeys) {
        if (symbolActive && (code == "ShiftLeft" || code == "ShiftRight")) continue;
        auto jit = joystickKeys().find(code);
        if (joystickEnabled && jit != joystickKeys().end()) {
            keyboardJoystick &= ~(1 << jit->second);
            continue;
        }
        auto kit = keyMap().find(code);
        if (kit != keyMap().end()) rows[kit->second[0]] &= ~(1 << kit->second[1]);
    }
    for (auto& kv : pressedSymbols) { rows[kv.second[0]] &= ~(1 << kv.second[1]); }
    bool anyShift = false; for (auto& kv : pressedSymbols) if (kv.second[2]) { anyShift = true; break; }
    if (anyShift) rows[2] &= ~(1 << 5);
    applyDirectBits();
}
void KeyboardMatrix::setMatrixDirect(int row, int bit, bool pressed) {
    int key = (row << 3) | bit;
    if (pressed) {
        directBits.insert(key);
    } else {
        directBits.erase(key);
        rows[row] |= (1 << bit);
    }
    applyDirectBits();
}
void KeyboardMatrix::applyDirectBits() {
    for (int key : directBits) {
        int row = key >> 3, bit = key & 7;
        rows[row] &= ~(1 << bit);
    }
}
bool KeyboardMatrix::setKey(const std::string& code, bool pressed, const std::string& characterIn, int location) {
    std::string character = characterIn;
    std::string effectiveCode = code;
    bool numpadLocation = location == 3;
    if (numpadLocation && code == "Enter") effectiveCode = "NumpadEnter";
    bool isNumpad = effectiveCode.rfind("Numpad", 0) == 0;

    if (pressed && codeToEffective.count(code)) {
        std::string heldCode = codeToEffective[code];
        return pressedSymbols.count(code) || keyMap().count(heldCode) || joystickKeys().count(heldCode);
    }

    std::optional<CharCell> characterMap = !isNumpad ? cpcCharacterMapping(character, effectiveCode, region) : std::nullopt;
    if (isNumpad) {
        auto nit = numpadNavOff().find(effectiveCode);
        if (nit != numpadNavOff().end() && numpadNavChar(character)) effectiveCode = nit->second;
    }

    if (pressed) {
        codeToEffective[code] = effectiveCode;
        if (characterMap) pressedSymbols[code] = *characterMap;
        else pressedKeys.insert(effectiveCode);
    } else {
        bool wasCharacterMapped = pressedSymbols.count(code) != 0;
        auto cit = codeToEffective.find(code);
        if (cit != codeToEffective.end()) effectiveCode = cit->second;
        codeToEffective.erase(code);
        pressedSymbols.erase(code);
        if (!wasCharacterMapped) pressedKeys.erase(effectiveCode);
        characterMap = wasCharacterMapped ? std::optional<CharCell>(CharCell{ 0,0,0 }) : std::nullopt;
    }
    bool hasKey = keyMap().count(effectiveCode) != 0;
    bool hasJoy = joystickKeys().count(effectiveCode) != 0;
    rebuildKeyboardState();
    return (bool)characterMap || hasKey || hasJoy;
}
void KeyboardMatrix::setJoystick(int bit, bool pressed, const std::string& source, int player) {
    if (!joystickEnabled) return;
    bool gp = source == "gamepad";
    int& prop = player == 1 ? (gp ? gamepadJoystick1 : keyboardJoystick1)
                            : (gp ? gamepadJoystick : keyboardJoystick);
    if (pressed) prop &= ~(1 << bit); else prop |= 1 << bit;
}
void KeyboardMatrix::setLightgunEnabled(bool enabled) {
    lightgunEnabled = enabled;
    if (!lightgunEnabled) { lightgunBeamHit = false; lightgunTrigger = false; gunstickTriggerPending = false; gunstickReadCounter = 0; }
}
void KeyboardMatrix::setLightgunType(const std::string& type) {
    lightgunMode = (type == "trojan" || type == "gunstick" || type == "westphaser") ? type : "trojan";
    lightgunBeamHit = false;
    lightgunTrigger = false;
    gunstickTriggerPending = false;
    gunstickReadCounter = 0;
    lightgunJoystick = 0xff;
}
void KeyboardMatrix::setLightgunBeamHit(bool hit) { lightgunBeamHit = hit; }
void KeyboardMatrix::setLightgunTrigger(bool pressed) {
    bool next = pressed;
    if (lightgunMode == "gunstick" && next && !lightgunTrigger) {
        gunstickTriggerPending = true;
        lightgunBeamHit = false;
    }
    if (!next) {
        gunstickTriggerPending = false;
        lightgunBeamHit = false;
    }
    lightgunTrigger = next;
}
int KeyboardMatrix::read(int row) {
    row &= 0x0f;
    int keys = row < 10 ? rows[row] : 0xff;
    if (row == 6) return keys & keyboardJoystick1 & gamepadJoystick1;   // second joystick
    if (row != 9) { if (onRowRead) onRowRead(row, keys); return keys; }
    int value = keys & keyboardJoystick & gamepadJoystick;
    if (lightgunEnabled) {
        value = (keys & 0x80) | 0x7f;
        if (onLightgunRead) onLightgunRead(lightgunMode);
        if (lightgunMode == "trojan") {
            if (lightgunTrigger) value &= ~(1 << 4);
        } else if (lightgunMode == "gunstick") {
            if (lightgunTrigger) {
                bool triggerOnly = gunstickTriggerPending;
                value &= ~(1 << 4);
                gunstickReadCounter = (gunstickReadCounter + 1) & 0xff;
                if (!triggerOnly && lightgunBeamHit && gunstickReadCounter != 0) value &= ~(1 << 1);
                gunstickTriggerPending = false;
            }
        } else if (lightgunMode == "westphaser") {
            if (lightgunTrigger) value &= ~(1 << 5);
            if (lightgunBeamHit) value &= ~1;
        }
    }
    return value;
}

} // namespace cpcse
