// CPCSyntaxError — the far end of an emulated serial port: the host's own.
//
// What a CPC's RS232 line is plugged into, chosen in the settings:
//   tcp       a TCP connection to host:port (a BBS, a telnet server, another emulator)
//   listen    a TCP port on the host that one client at a time can connect to
//   com       a serial port of the host's (COM3, /dev/ttyUSB0), at the CPC's own baud rate
//   loopback  TXD wired back to RXD, RTS to CTS, DTR to DCD (a loopback plug)
// It moves bytes only; the chip that drives the line paces them to its baud rate. The modem
// lines a TCP link has: CTS and DCD are up while connected.
#pragma once
#include "common.h"

#include <deque>
#include <string>

namespace cpcse {

class HostSerial {
public:
    enum class Kind { None, Tcp, Listen, Com, Loopback };
    HostSerial();
    ~HostSerial();
    HostSerial(const HostSerial&) = delete;
    HostSerial& operator=(const HostSerial&) = delete;

    // target: "host:port" (tcp), "port" (listen), "COM3" or "/dev/ttyUSB0" (com).
    bool open(Kind kind, const std::string& target, std::string& error);
    void close();
    Kind kind() const { return kind_; }
    std::string describe() const;      // for the status line

    // Bytes in and out; poll() moves them between these queues and the host.
    void poll();
    bool readable() const { return !in.empty(); }
    int read();                        // -1: nothing
    void write(uint8_t byte);

    // Modem lines, as the CPC's side sees them (inputs) and drives them (outputs).
    bool cts() const;
    bool dcd() const;
    bool ri() const;
    void setRts(bool on);
    void setDtr(bool on);
    void setBaud(int baud, int dataBits, int stopHalves, int parity);   // a COM port's own speed

    bool allowLan = false;             // listen on every interface, not only 127.0.0.1

private:
    Kind kind_ = Kind::None;
    std::string target_;
    std::deque<uint8_t> in, out;
    bool rts = false, dtr = false;
    struct Native;
    Native* n;
};

} // namespace cpcse
