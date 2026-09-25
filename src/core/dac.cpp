// CPCSyntaxError — DAC and printer device.
#include "dac.h"

namespace cpcse {

CpcDac::CpcDac() {
    mode = "none";
    amdrumValue = 0xff;
    currentSample = 0;
    filteredSample = 0;
    outputSampleRate = 0;
    samplePhase = 0;
    sampleReadIndex = 0;
    lastOutputSample = 0;
    alpha = 0.45;
    printerBuffer = "";
    printerInitSeen = 0;
    printerStrobe = false;
    reset();
}
void CpcDac::reset() {
    amdrumValue = 0xff;
    currentSample = 0;
    filteredSample = 0;
    samplePhase = 0;
    sampleQueue.clear();
    sampleReadIndex = 0;
    lastOutputSample = 0;
}
void CpcDac::setMode(const std::string& mode_) {
    mode = mode_;
    printerInitSeen = 0;
    printerStrobe = false;
    reset();
}
void CpcDac::setOutputSampleRate(double sampleRate) {
    outputSampleRate = std::max(0.0, std::isfinite(sampleRate) ? sampleRate : 0.0);
    samplePhase = 0;
    sampleQueue.clear();
    sampleReadIndex = 0;
}
// ACCC §5.1 (p.32): the CPC decodes I/O addresses only PARTIALLY, and the decode table
// gives the printer exactly one selecting bit -- b12 = 0, every other bit "x", with
// &EF00 quoted only as the conventional address. "A device is affected by an
// Input/Output operation as soon as a few precise bits of the address bus are set to 0
// and/or 1. This implies that if other bits relative to other devices are also set,
// then the Input/Output operation will also affect them." Matching the full &EF00
// instead (and carving &EF7F out of it) missed every shortened form.
bool CpcDac::isPrinterPort(int port) { return (port & 0x1000) == 0; }
void CpcDac::writePort(int port, int value) {
    value &= 0xff;
    if (mode == "amdrum" && (port & 0xff00) == 0xff00) {
        amdrumValue = value;
        currentSample = ((value - 128) / 128.0) * 0.4;
    } else if (mode == "digiblaster" && isPrinterPort(port)) {   // ACCC §5.1: b12=0
        int sample = value ^ 0x80;
        currentSample = ((sample - 128) / 128.0) * 0.4;
    } else if ((mode == "printer" || mode == "matrix") && isPrinterPort(port)) {
        bool strobe = !!(value & 0x80);
        if (strobe && !printerStrobe) {
            int ch = value & 0x7f;
            if (onPrinterStrobe) onPrinterStrobe(ch);
            if (mode == "matrix") { if (onMatrixPrinterByte) onMatrixPrinterByte(ch); }
            else if (ch >= 32 || ch == 10 || ch == 13) { if (onPrinterChar) onPrinterChar(std::string(1, (char)ch)); }
        }
        printerStrobe = strobe;
    }
}
int CpcDac::readPort(int port) {
    if (mode == "amdrum" && (port & 0xff00) == 0xff00) {
        return amdrumValue;
    }
    return 0xff;
}
void CpcDac::advanceTStates(int tStates) {
    if (!outputSampleRate || mode == "none" || mode == "printer" || mode == "matrix") return;
    samplePhase += tStates * outputSampleRate / 4000000.0;
    while (samplePhase >= 1) {
        samplePhase -= 1;
        filteredSample += alpha * (currentSample - filteredSample);
        lastOutputSample = filteredSample;
        sampleQueue.push_back(lastOutputSample);
    }
    int maximum = (int)std::ceil(outputSampleRate / 4);
    if ((int)sampleQueue.size() - sampleReadIndex > maximum) {
        sampleReadIndex = (int)sampleQueue.size() - maximum;
    }
    if (sampleReadIndex > 4096) {
        sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + sampleReadIndex);
        sampleReadIndex = 0;
    }
}
double CpcDac::readSample() {
    if (mode == "none" || mode == "printer" || mode == "matrix") return 0;
    if (sampleReadIndex < (int)sampleQueue.size()) {
        return sampleQueue[sampleReadIndex++];
    }
    return lastOutputSample;
}

} // namespace cpcse
