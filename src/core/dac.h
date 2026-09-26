// CPCSyntaxError — DAC and printer device.
// DigiBlaster, AmDrum, text-printer and matrix-printer port output.
#pragma once
#include "common.h"

namespace cpcse {

class CpcDac {
public:
    std::string mode = "none"; // 'none','digiblaster','amdrum','printer','matrix'
    int amdrumValue = 0xff;
    double currentSample = 0;
    double filteredSample = 0;
    double outputSampleRate = 0;
    double samplePhase = 0;
    std::vector<double> sampleQueue;
    int sampleReadIndex = 0;
    double lastOutputSample = 0;
    double alpha = 0.45;
    std::string printerBuffer;
    int printerInitSeen = 0;
    bool printerStrobe = false;
    int printerHighBit = 0;    // D7 of the printer, driven by the host (CRTC 3: R12 bit 3)
    std::function<void(const std::string&)> onPrinterChar;
    std::function<void(int)> onMatrixPrinterByte;
    std::function<void(int)> onPrinterStrobe;

    CpcDac();
    void reset();
    void setMode(const std::string& mode = "none");
    void setOutputSampleRate(double sampleRate);
    bool isPrinterPort(int port);
    void writePort(int port, int value);
    int readPort(int port);
    void advanceTStates(int tStates);
    double readSample();
};

} // namespace cpcse
