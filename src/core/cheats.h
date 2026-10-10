// CPCSyntaxError — cheats: finding where a game keeps a number, and POKEs that change it or
// hold it (freeze).
//
// Addresses are places in the machine's RAM, not in the Z80's view of it: 0-&FFFF is the
// base 64K (where a magazine's POKE &1234,0 goes -- a game runs with it mapped in), &10000
// on the expansion RAM's 16K pages in order (a 6128's second 64K, a 512K card's). So a
// search covers every bank, and a cheat on a value a 128K game keeps in another bank holds.
//
// The finder: every RAM address is a candidate, in each of the ways a game stores a number
// it shows -- as itself (binary), as one less (lives counted from 0, "3" kept as 2), or as
// BCD (each nibble a digit: scores, and lives on many games); 8 or 16 bits. Each step keeps
// the candidates the step allows: the value now equal to what the player says the screen
// shows, or changed / unchanged / up / down since the step before (for a bar, a value there
// is no number for). A few steps leave one or two places.
#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace cpcse {

class CheatFinder {
public:
    enum Encoding : uint8_t { BINARY = 0, ONE_LESS = 1, BCD = 2 };
    enum Change { CHANGED, UNCHANGED, INCREASED, DECREASED };
    struct Candidate { uint32_t address; uint8_t encoding; };

    int width = 1;                       // 1 or 2 bytes (2: little-endian, as the Z80 keeps them)
    bool started = false;

    // every address, every encoding (or only `encodings`, a bit each), from this RAM
    void start(const Bytes& ram, unsigned encodings = 7);
    void keepEqual(const Bytes& ram, int value);           // the number as the game shows it
    void keepChange(const Bytes& ram, Change change);
    void keepChangedBy(const Bytes& ram, int delta);       // shown number went up (or down) by delta
    bool undo();                                           // back one step
    size_t size() const { return candidates.size(); }
    const std::vector<Candidate>& list() const { return candidates; }
    // the value at a candidate, as the number the game shows (-1: not a valid one, e.g. BCD
    // with a nibble over 9)
    int shown(const Bytes& ram, const Candidate& c) const;
    int stored(const Bytes& ram, uint32_t address) const;  // raw, `width` bytes
    // the raw bytes that make `value` show, in this candidate's encoding
    int encode(const Candidate& c, int value) const;

private:
    std::vector<Candidate> candidates;
    std::vector<std::vector<Candidate>> history;
    Bytes previous;                      // RAM as the last step saw it
    std::vector<Bytes> previousHistory;
    void step(const Bytes& ram);
};

struct Poke { uint32_t address = 0; int value = 0; int width = 1; };
struct Cheat {
    std::string name;
    std::vector<Poke> pokes;
    bool freeze = true;                  // held every frame; false: written once when switched on
    bool enabled = false;
};

class CheatList {
public:
    std::vector<Cheat> cheats;
    void apply(Bytes& ram);              // every frame: the enabled freezes
    static void poke(Bytes& ram, const Cheat& cheat);
    // POKE text: a cheat per [name] (or "# name") line, then its POKE &addr,value lines --
    // decimal or &hex, a 16-bit value with POKE16 or "w"; "once" on a POKE makes the cheat
    // written once rather than held. Addresses past &FFFF are expansion RAM (see above).
    static bool parse(const std::string& text, std::vector<Cheat>& out, std::string& error);
    static std::string serialize(const std::vector<Cheat>& cheats);
    static std::string addressText(uint32_t address);   // "&1234", or "&1C234 (page 7 &0234)"
};

} // namespace cpcse
