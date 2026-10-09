// CPCSyntaxError — the M4 Board's WiFi, passed through to the host's own network.
//
// On the board the CPC never talks to the WiFi module: the Cortex M4 takes the CPC's
// commands, drives the ESP over its AT link, and reports back in two places the Z80 can
// read (m4info.txt, "Developer information"):
//   - the response of each command (C_NETSOCKET ... C_NETACCEPT, C_HTTPGET ...);
//   - sock_info, 5 x 16 bytes at &FE00 of the M4 ROM (link table &FF06), which "works
//     like read only hardware registers": programs send C_NETCONNECT and then sit in a
//     loop reading the status byte until it changes (M4examples tcp.s, httpget.s).
// So this class is the M4's side of that link with the host's sockets in place of the
// ESP: it keeps the five structures and the board copies them into its ROM.
//
// sock_info, per socket (socket 0 belongs to C_NETHOSTIP, 1-4 to C_NETSOCKET):
//   +0 status   0 idle/ok, 1 connect in progress, 2 send in progress, 3 remote closed,
//               4 waiting for an incoming connection, 5 DNS lookup in progress,
//               240-255 an error code
//   +1 lastcmd  0 none, 1 send, 2 dnslookup, 3 connect, 4 accept, 5 recv, 6 error handler
//   +2 received (2 bytes) bytes waiting to be taken with C_NETRECV
//   +4 ip       (4 bytes) the resolved address (socket 0) or the client's (accept),
//               last octet first: +7 is the first number of a.b.c.d (lookup.s prints
//               (ix+7) first; tcp.s stores a typed address the same way for C_NETCONNECT)
//   +8 port     (2 bytes) low byte first (tcpserv.s: dw &1234 = port 4660)
//
// The error codes: "240-255 = error code" is lwIP's err_t (the ESP's TCP/IP stack) as a
// byte -- ERR_ABRT -13 = 243, ERR_RST -14 = 242, ERR_CLSD -15 = 241, ERR_ARG -16 = 240.
// Which one the ESP firmware puts there for which failure is not public; the mapping in
// m4_net.cpp follows lwIP's own use of them.
#pragma once
#include "common.h"

#include <array>
#include <chrono>
#include <deque>
#include <memory>

namespace cpcse {

struct M4HttpJob;
struct M4DnsJob;

class M4Network {
public:
    M4Network();
    ~M4Network();

    static const int SOCKETS = 5;                  // 0 = DNS, 1-4 for programs
    static const int SOCK_INFO_SIZE = SOCKETS * 16;
    static const size_t RECEIVE_BUFFER = 0x2000;   // per socket, before TCP pushes back

    bool wifiOn = true;            // |WIFI,0 / C_WIFIPOW (on again at power-on)
    bool linked = true;            // the emulator's setting: the host's network or none
    bool allowLan = false;         // listening sockets on every interface, not only 127.0.0.1
    bool online() const { return wifiOn && linked; }
    // What |NETSET last gave (C_SETNETWORK): only shown back by C_GETNETWORK.
    std::string name = "CPC", ssid = "host network", password;

    // The network commands, given the command's data; `out` is the response after its
    // 3-byte header. Returns false for a command that is not a network one.
    bool command(int cmd, const std::vector<int>& data, std::vector<uint8_t>& out);
    // Moves data and finished connects/lookups between the host and the structures.
    void poll();
    // Anything that needs poll(): an open socket, a lookup, a download.
    bool active() const;
    void closeAll();
    void sockInfo(uint8_t* to) const;   // the 80 bytes at &FE00

    // A blocking HTTP GET run on a thread (|HTTPGET, |HTTPMEM): the board holds the Z80
    // until it is done, as the real one does (its ROM reads the answer straight after
    // the kick).
    struct HttpResult {
        bool ok = false;
        std::string request;       // as it was asked for
        std::string error;         // when !ok
        std::string fileName;      // Content-Disposition's attachment filename, or ""
        Bytes body;
    };
    void startHttp(const std::string& request, size_t offset, size_t maxBytes);
    bool httpRunning() const;
    bool httpTake(HttpResult& result);   // true once, when it has finished

    // "host:port/path" (an http:// in front is dropped, as |HTTPGET does) -> parts.
    static bool splitUrl(const std::string& url, std::string& host, int& port, std::string& path);
    static std::string hostAddress();    // the host's own IPv4 address, "" if offline

private:
    struct Sock;
    std::array<std::unique_ptr<Sock>, SOCKETS> socks;
    std::shared_ptr<M4DnsJob> dns;
    std::shared_ptr<M4HttpJob> http;
    void netstat(std::vector<uint8_t>& out) const;
    void getNetwork(std::vector<uint8_t>& out) const;
    void closeSocket(int s);
};

} // namespace cpcse
