// CPCSyntaxError — SYMBiFACE III expansion card.
//
// Its functions, as the CPC calls them (TMTLOGIC's SF3-RSX.BIN ROM, Prodatron's SF3Monitor
// App-SF3.asm, the SF3's BASIC examples): OUT &FD41,0 empties the parameter buffer, the
// parameters go to &FD42, then the function number to &FD41. &FD41 reads 1 while the SF3
// works, 0 when it is done, 2 on an error; the answer is read from &FD42. The SF3 empties the
// buffer after each function too (the ROM's |OSCROLL sends its parameters without the 0).
//
//    20  mouse: dx, dy, buttons, wheel (MOUSE.BAS)
//    21  mouse info, 54 USB drive info, 71 specifications, 125 measurements: text lines,
//        each ended by 13, the end by 10 (SF3Monitor: "Sys" starts 71's answer)
//    40  audio route (5 = web radio, 6 = the EMR MIDI interface), 41 volume (1, 0..100)
//    72  4 = the error number, 5 = the error text (BG-TEST.BAS); 10 = headset test
//    80  buzzer: 2 bytes, 0..3 each; 0,0 = off (|BUZZ)
//   100 101 102 110 111  the clock (sf3_rtc.h)
//   200 OLED clear, 201 scroll, 202 / 203 text in the upper / lower row, 210 text at
//       x (0-127), y (0-31), font 10/18/26 px (SF3Monitor's OLED dialogue)
// and the SE-ONE sound card at &FF20-&FF27 (sf3_audio.h).
//
// WiFi: OUT &FD41,7 starts a command, its bytes go to &FD49, OUT &FD41,90 runs it; &FD4E
// reads 1 while the WiFi works, 0 when done, 2 on an error (the number by F72,4); answers
// are read from &FD49, a socket's received data from &FD45 + socket. The commands (the
// SF3-RSX ROM's hidden RSXs, Prodatron's Dmn-Network-SF3.asm, WEB-DK.BAS):
//     4 name           DNS: 4 bytes of IP, the first number first
//     5 a b c d hi lo s  connect socket s (0-3) to a.b.c.d, port hi*256+lo
//     8 s data...      send          10 s  close          11  IP config (MAC, IP, mask,
//    13                a free socket, &FF none                gateway, DNS1, DNS2)
//    17 s              0 open, 1 connecting, 3 closed, 2 WiFi offline (WEB-DK.BAS)
//    18 s              bytes received: high, low; then read them from &FD45 + s
//    21 s              the socket's data into the MP3 decoder (web radio, route 5)
// The WiFi is the host's network, through the M4 Board's sockets (m4_net.h).
//
// Not emulated, answered with status 2 and an error text: the ROM banks and file functions
// (62, 63, 66), DFU (70), ROM set (3).
#pragma once
#include "common.h"
#include "m4_net.h"
#include "sf3_audio.h"
#include "sf3_rtc.h"
#include <memory>

namespace cpcse {

inline constexpr int SF3_CMD_PORT = 0xfd41;
inline constexpr int SF3_DATA_PORT = 0xfd42;
inline constexpr int SF3_RECEIVE_PORT = 0xfd45;   // + socket
inline constexpr int SF3_NET_PORT = 0xfd49;
inline constexpr int SF3_NET_STATUS_PORT = 0xfd4e;
inline constexpr int SF3_ECHO_PORT = 0xfd4f;

class Symbiface3 {
public:
    bool enabled = false;
    double sensitivity = 1;
    Symbiface3Rtc rtc;
    int dx = 0, dy = 0, dz = 0;
    std::array<bool, 3> buttons{ false, false, false };
    bool buttonChanged = false;
    int sf3Echo = 0;
    // A rewind's copy shares the sound card: its stream and files are not rewound.
    std::shared_ptr<Sf3Audio> audio = std::make_shared<Sf3Audio>();
    std::shared_ptr<M4Network> net = std::make_shared<M4Network>();   // shared the same way

    // the WiFi command being written, its answer, its state (&FD4E)
    std::vector<int> wifiIn;
    std::deque<int> wifiOut;
    int wifiStatus = 0;
    enum class Pending { None, Connect, Dns } pending = Pending::None;
    int pendingSocket = 0;
    std::array<std::deque<int>, 4> received;   // &FD45-&FD48

    std::deque<int> params, reply;
    int status = 0, errorNumber = 0;
    std::string errorText;
    // The OLED (128 x 32): two rows of text, and texts placed with function 210.
    struct OledText { int x = 0, y = 0, font = 10; std::string text; };
    std::array<std::string, 2> oledRows;
    std::vector<OledText> oledTexts;
    int oledScroll = 0;

    Symbiface3();
    void setEnabled(bool enabled = false);
    void reset();
    bool handlesPort(int port);
    bool handlesWritePort(int port) { return handlesPort(port); }
    void setSensitivity(double value = 1);
    void move(double dx, double dy);
    void scroll(double delta);
    void button(int index, bool pressed);
    int readPort(int port);
    bool writePort(int port, int value);
    void advanceMicrosecond() { if (enabled) audio->advanceMicrosecond(); }

private:
    void function(int number);
    void text(const std::vector<std::string>& lines);
    void fail(int number, const std::string& why);
    void wifi();
    void wifiPoll();
    int netStatus(int socket);                 // the M4 sock_info status of SF3 socket 0-3
    void take(int socket);                     // the host's data into `received`
};

} // namespace cpcse
