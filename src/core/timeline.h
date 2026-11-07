// CPCSyntaxError — the machine's timeline: exact snapshots, and the inputs between them.
//
// A MachineSnapshot is the whole machine at one instant, as copies of its chips. It is
// restored IN PLACE: each chip of the live machine is assigned its copy, so the pointers
// the chips hold to each other stay right, and nothing has to be listed field by field.
// What is kept out:
//  - read-only bulk (the ROM images, a cartridge, a tape image): the live machine keeps its
//    own across a restore (a snapshot is not taken across a change of media);
//  - the host's and the debugger's hooks and settings on the chips (breakpoints, the
//    analyser, the renderer's callbacks): the live ones stay;
//  - the pictures already made (rasterFrame): only rendering reads them, and a replay of a
//    frame or two makes them again;
//  - the cards that own host resources (M4, OPL4, SYMBiFACE IDE, RS232C, speech, both
//    Albireo boards and the Albireo UART, HD20): they are not rewound;
//  - the disc in a drive (the controller shares it): what is written to it stays written.
// From the same snapshot with the same inputs (input_event.h) the machine runs the same,
// instruction for instruction: timeline-check proves it.
#pragma once
#include "common.h"
#include "input_event.h"

namespace cpcse {

class GX4000;

class MachineSnapshot {
public:
    static std::unique_ptr<MachineSnapshot> capture(GX4000& machine);
    void restore(GX4000& machine) const;
    ~MachineSnapshot();
    uint64_t instructions = 0;    // GX4000::instructions when it was taken
    size_t bytes = 0;             // about how much memory it holds
private:
    MachineSnapshot() = default;
    struct Parts;
    std::unique_ptr<Parts> parts;
};

} // namespace cpcse
