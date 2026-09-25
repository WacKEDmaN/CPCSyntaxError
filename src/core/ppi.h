// CPCSyntaxError — 8255 PPI.
// Connects keyboard, PSG, cassette, printer and machine-identification lines.
#pragma once
#include "common.h"

namespace cpcse {

class AY38912;
class CPCTapeDrive;

class PPI8255 {
public:
    AY38912* ay;
    std::function<bool()> vsyncProvider;
    CPCTapeDrive* tape;
    bool strictPlusBus;
    bool plusMode = true;
    bool printerOnline = false;
    int printerBusyCycles = 0;
    bool printerManualBusy = false;
    int control = 0, portA = 0, portB = 0, portC = 0;
    bool portAInput = false, portBInput = false, portCHighInput = false, portCLowInput = false;

    PPI8255(AY38912* ay, std::function<bool()> vsyncProvider = nullptr, CPCTapeDrive* tape = nullptr);
    void setPlusMode(bool enabled);
    void reset();
    void onPrinterStrobe(int busyCycles = 4000);
    void advanceTStates(int tStates);
    void setMode(int value);
    int portIndex(int port) { return (unsigned)port >> 8 & 3; }
    void write(int port, int value);
    void applyAyBus();
    int read(int port);
};

} // namespace cpcse
