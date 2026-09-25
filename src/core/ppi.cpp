// CPCSyntaxError — 8255 PPI.
#include "ppi.h"
#include "ay.h"
#include "keyboard.h"
#include "tape.h"

namespace cpcse {

PPI8255::PPI8255(AY38912* ay, std::function<bool()> vsyncProvider, CPCTapeDrive* tape)
    : ay(ay), vsyncProvider(vsyncProvider ? vsyncProvider : []() { return false; }), tape(tape) {
    strictPlusBus = tape != nullptr;
    plusMode = true; printerOnline = false; printerBusyCycles = 0; printerManualBusy = false; reset();
}

void PPI8255::setPlusMode(bool enabled) { plusMode = enabled; setMode(control); }
void PPI8255::reset() { portA = portB = portC = 0; printerBusyCycles = 0; printerManualBusy = false; control = plusMode ? 0x12 : 0x9b; setMode(control); }
void PPI8255::onPrinterStrobe(int busyCycles) {
    if (printerOnline && busyCycles > 0) printerBusyCycles = std::max(printerBusyCycles, busyCycles);
}
void PPI8255::advanceTStates(int tStates) {
    if (printerBusyCycles > 0) printerBusyCycles = std::max(0, printerBusyCycles - tStates);
}
void PPI8255::setMode(int value) {
    control = value & 0xff;
    if (plusMode) {
        control |= 0x02;
        if (strictPlusBus) control &= 0xf6;
    }
    portAInput = !!(control & 0x10);
    portBInput = !!(control & 0x02);
    portCHighInput = !!(control & 0x08); portCLowInput = !!(control & 0x01);
    if (!plusMode) {
        portA = 0;
        portB = 0;
        portC = 0;
    }
    applyAyBus();
}
void PPI8255::write(int port, int value) {
    value &= 0xff;
    switch (portIndex(port)) {
        case 0:
            portA = portAInput ? 0xff : value;
            applyAyBus();
            break;
        case 1:
            if (!portBInput) portB = value;
            break;
        case 2: {
            portC = value;
            if (tape) tape->setMotor(!!(value & 0x10));
            if (ay) ay->beeperNoise = (value & 0x20) ? 0.22 : 0;
            applyAyBus();
            break;
        }
        default:
            if (value & 0x80) {
                setMode(value);
            } else {
                int bit = (unsigned)value >> 1 & 7;
                if (value & 1) portC |= 1 << bit; else portC &= ~(1 << bit);
                if (tape && bit == 4) tape->setMotor(!!(portC & 0x10));
                if (ay && bit == 5) ay->beeperNoise = (portC & 0x20) ? 0.22 : 0;
                applyAyBus();
            }
            break;
    }
}
void PPI8255::applyAyBus() {
    if (!portCLowInput) ay->keyboard->selectRow(portC & 0x0f);
    if (portCHighInput) return;
    int bus = portC & 0xc0;
    if (bus == 0xc0) ay->select(portA);
    else if (bus == 0x80) ay->write(portA);
}
int PPI8255::read(int port) {
    switch (portIndex(port)) {
        case 0:
            if (portAInput) {
                return (portC & 0xc0) == 0x40 ? ay->read() : 0xff;
            }
            return portA;
        case 1: {
            // ACCC §7.3 FAKE VSYNC (p.42): "Port B of the PPI is designed on the CPC to
            // work as input... It is possible to reprogram this port as output, by
            // setting to 0 the bit 1 of the control register of the PPI located at the
            // address of I/O #F700. Writing to port B (#F500) a value with the bit 0=1
            // will therefore put pin 25 of the circuit in the high state." An 8255 port
            // in mode 0 output is LATCHED, and reading it gives that latch back rather
            // than whatever the pins carry -- port A is already read that way here.
            //
            // What the chapter describes doing WITH that -- driving VSYNC back at the
            // GATE ARRAY -- is deliberately not modelled: "I have found, like Kevin
            // Thacker (ArnoldEmu) before me, that this FAKE VSYNC does not work on some
            // CPC's. On other CPC's, the result is incorrect."
            if (!portBInput) return portB;
            int tapeBit = tape ? tape->getPortBBit() : 0x00;   // no deck, no signal
            int printerBusy = (!printerOnline || printerBusyCycles > 0 || printerManualBusy) ? 0x40 : 0;
            int boardMask = strictPlusBus ? 0x5e : 0x7e;
            return ((boardMask & ~0x40) | printerBusy | (vsyncProvider() ? 1 : 0)) | tapeBit;
        }
        case 2: {
            int value = portC;
            if (portCHighInput) value |= 0xf0;
            if (portCLowInput) value |= 0x0f;
            return value;
        }
        default: return plusMode ? ((control <= 0x7f || (control & 0x10)) ? 0xff : 0x00) : control | 0x80;
    }
}

} // namespace cpcse
