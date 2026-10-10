// CPCSyntaxError — the host's end of an emulated serial line. See host_serial.h.
#include "host_serial.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <termios.h>
#  include <unistd.h>
#endif
#include <cstring>

namespace cpcse {

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock BAD = INVALID_SOCKET;
void closeSock(Sock s) { if (s != BAD) closesocket(s); }
bool wouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS; }
bool startup() { static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); return ok; }
void nonBlocking(Sock s) { u_long v = 1; ioctlsocket(s, FIONBIO, &v); }
#else
using Sock = int;
const Sock BAD = -1;
void closeSock(Sock s) { if (s != BAD) ::close(s); }
bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR || errno == EINPROGRESS; }
bool startup() { return true; }
void nonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif
#if defined(MSG_NOSIGNAL)
const int SEND_FLAGS = MSG_NOSIGNAL;
#else
const int SEND_FLAGS = 0;
#endif

Sock tcpSocket() {
    if (!startup()) return BAD;
    Sock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD) return BAD;
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
#if defined(SO_NOSIGPIPE)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return s;
}

// Readable/writable now, without waiting.
bool ready(Sock s, bool forWrite) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{ 0, 0 };
    return select((int)s + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv) > 0;
}

} // namespace

struct HostSerial::Native {
    Sock sock = BAD;          // tcp: the connection; listen: the client
    Sock server = BAD;        // listen: the listening socket
    bool connecting = false;  // tcp: connect() not finished
#ifdef _WIN32
    HANDLE com = INVALID_HANDLE_VALUE;
#else
    int com = -1;
#endif
    bool comOpen() const {
#ifdef _WIN32
        return com != INVALID_HANDLE_VALUE;
#else
        return com >= 0;
#endif
    }
};

HostSerial::HostSerial() : n(new Native) {}
HostSerial::~HostSerial() { close(); delete n; }

void HostSerial::close() {
    closeSock(n->sock); n->sock = BAD;
    closeSock(n->server); n->server = BAD;
    n->connecting = false;
#ifdef _WIN32
    if (n->com != INVALID_HANDLE_VALUE) CloseHandle(n->com);
    n->com = INVALID_HANDLE_VALUE;
#else
    if (n->com >= 0) ::close(n->com);
    n->com = -1;
#endif
    in.clear(); out.clear();
    kind_ = Kind::None;
    target_.clear();
}

bool HostSerial::open(Kind kind, const std::string& target, std::string& error) {
    close();
    error.clear();
    target_ = target;
    if (kind == Kind::None || kind == Kind::Loopback) { kind_ = kind; return true; }
    if (kind == Kind::Tcp) {
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos || colon == 0) { error = "give the host as name:port"; return false; }
        const std::string host = target.substr(0, colon), port = target.substr(colon + 1);
        if (!startup()) { error = "no network"; return false; }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { error = "cannot find " + host; return false; }
        Sock s = tcpSocket();
        if (s == BAD) { freeaddrinfo(res); error = "no socket"; return false; }
        nonBlocking(s);
        const int r = ::connect(s, res->ai_addr, (int)res->ai_addrlen);
        freeaddrinfo(res);
        if (r != 0 && !wouldBlock()) { closeSock(s); error = "cannot connect to " + target; return false; }
        n->sock = s;
        n->connecting = r != 0;
        kind_ = kind;
        return true;
    }
    if (kind == Kind::Listen) {
        const int port = std::atoi(target.c_str());
        if (port <= 0 || port > 65535) { error = "give a port number"; return false; }
        Sock s = tcpSocket();
        if (s == BAD) { error = "no socket"; return false; }
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(allowLan ? INADDR_ANY : INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(s, 1) != 0) { closeSock(s); error = "port " + target + " is in use"; return false; }
        nonBlocking(s);
        n->server = s;
        kind_ = kind;
        return true;
    }
    // a serial port of the host's
#ifdef _WIN32
    const std::string path = target.rfind("\\\\.\\", 0) == 0 ? target : "\\\\.\\" + target;
    n->com = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (n->com == INVALID_HANDLE_VALUE) { error = "cannot open " + target; return false; }
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;          // a read returns what is there, at once
    SetCommTimeouts(n->com, &t);
#else
    n->com = ::open(target.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (n->com < 0) { error = "cannot open " + target; return false; }
    termios tio{};
    tcgetattr(n->com, &tio);
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tcsetattr(n->com, TCSANOW, &tio);
#endif
    kind_ = kind;
    setBaud(9600, 8, 2, 0);
    return true;
}

std::string HostSerial::describe() const {
    switch (kind_) {
    case Kind::None: return "not connected";
    case Kind::Loopback: return "loopback plug";
    case Kind::Tcp: return (n->sock == BAD ? "closed: " : n->connecting ? "connecting to " : "connected to ") + target_;
    case Kind::Listen: return (n->sock == BAD ? "listening on port " : "client on port ") + target_;
    case Kind::Com: return target_;
    }
    return "";
}

void HostSerial::poll() {
    if (kind_ == Kind::Loopback) {
        while (!out.empty()) { in.push_back(out.front()); out.pop_front(); }
        return;
    }
    if (kind_ == Kind::Listen && n->sock == BAD && n->server != BAD && ready(n->server, false)) {
        Sock c = ::accept(n->server, nullptr, nullptr);
        if (c != BAD) { nonBlocking(c); n->sock = c; }
    }
    if (kind_ == Kind::Tcp || kind_ == Kind::Listen) {
        if (n->sock == BAD) { out.clear(); return; }   // nobody on the line: the bytes go nowhere
        if (n->connecting) {
            if (!ready(n->sock, true)) return;
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(n->sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            if (err != 0) { closeSock(n->sock); n->sock = BAD; n->connecting = false; return; }
            n->connecting = false;
        }
        while (!out.empty()) {
            char buf[512];
            size_t k = 0;
            while (k < sizeof buf && k < out.size()) { buf[k] = (char)out[k]; k++; }
            const int sent = (int)::send(n->sock, buf, (int)k, SEND_FLAGS);
            if (sent <= 0) {
                if (!wouldBlock()) { closeSock(n->sock); n->sock = BAD; }
                break;
            }
            out.erase(out.begin(), out.begin() + sent);
        }
        if (n->sock == BAD) return;
        for (;;) {
            char buf[512];
            const int got = (int)::recv(n->sock, buf, sizeof buf, 0);
            if (got > 0) { in.insert(in.end(), buf, buf + got); if (in.size() > 65536) break; continue; }
            if (got == 0 || !wouldBlock()) { closeSock(n->sock); n->sock = BAD; }   // closed by the far end
            break;
        }
        return;
    }
    if (kind_ == Kind::Com && n->comOpen()) {
        uint8_t buf[512];
#ifdef _WIN32
        while (!out.empty()) {
            DWORD k = 0, done = 0;
            while (k < sizeof buf && k < out.size()) { buf[k] = out[k]; k++; }
            if (!WriteFile(n->com, buf, k, &done, nullptr) || done == 0) break;
            out.erase(out.begin(), out.begin() + done);
        }
        DWORD got = 0;
        while (ReadFile(n->com, buf, sizeof buf, &got, nullptr) && got > 0) in.insert(in.end(), buf, buf + got);
#else
        while (!out.empty()) {
            size_t k = 0;
            while (k < sizeof buf && k < out.size()) { buf[k] = out[k]; k++; }
            const ssize_t done = ::write(n->com, buf, k);
            if (done <= 0) break;
            out.erase(out.begin(), out.begin() + done);
        }
        ssize_t got;
        while ((got = ::read(n->com, buf, sizeof buf)) > 0) in.insert(in.end(), buf, buf + got);
#endif
    }
}

int HostSerial::read() {
    if (in.empty()) return -1;
    const int v = in.front();
    in.pop_front();
    return v;
}

void HostSerial::write(uint8_t byte) {
    if (kind_ == Kind::None) return;
    if (out.size() < 65536) out.push_back(byte);
}

bool HostSerial::cts() const {
    switch (kind_) {
    case Kind::Loopback: return rts;
    case Kind::Tcp: case Kind::Listen: return n->sock != BAD && !n->connecting;
    case Kind::Com: {
#ifdef _WIN32
        DWORD s = 0;
        return n->comOpen() && GetCommModemStatus(n->com, &s) && (s & MS_CTS_ON);
#else
        int s = 0;
        return n->comOpen() && ioctl(n->com, TIOCMGET, &s) == 0 && (s & TIOCM_CTS);
#endif
    }
    default: return false;
    }
}

bool HostSerial::dcd() const {
    switch (kind_) {
    case Kind::Loopback: return dtr;
    case Kind::Tcp: case Kind::Listen: return n->sock != BAD && !n->connecting;
    case Kind::Com: {
#ifdef _WIN32
        DWORD s = 0;
        return n->comOpen() && GetCommModemStatus(n->com, &s) && (s & MS_RLSD_ON);
#else
        int s = 0;
        return n->comOpen() && ioctl(n->com, TIOCMGET, &s) == 0 && (s & TIOCM_CD);
#endif
    }
    default: return false;
    }
}

bool HostSerial::ri() const {
    if (kind_ != Kind::Com || !n->comOpen()) return false;
#ifdef _WIN32
    DWORD s = 0;
    return GetCommModemStatus(n->com, &s) && (s & MS_RING_ON);
#else
    int s = 0;
    return ioctl(n->com, TIOCMGET, &s) == 0 && (s & TIOCM_RI);
#endif
}

void HostSerial::setRts(bool on) {
    if (on == rts) return;
    rts = on;
    if (kind_ != Kind::Com || !n->comOpen()) return;
#ifdef _WIN32
    EscapeCommFunction(n->com, on ? SETRTS : CLRRTS);
#else
    int bit = TIOCM_RTS;
    ioctl(n->com, on ? TIOCMBIS : TIOCMBIC, &bit);
#endif
}

void HostSerial::setDtr(bool on) {
    if (on == dtr) return;
    dtr = on;
    if (kind_ != Kind::Com || !n->comOpen()) return;
#ifdef _WIN32
    EscapeCommFunction(n->com, on ? SETDTR : CLRDTR);
#else
    int bit = TIOCM_DTR;
    ioctl(n->com, on ? TIOCMBIS : TIOCMBIC, &bit);
#endif
}

void HostSerial::setBaud(int baud, int dataBits, int stopHalves, int parity) {
    if (kind_ != Kind::Com || !n->comOpen() || baud <= 0) return;
#ifdef _WIN32
    DCB d{};
    d.DCBlength = sizeof d;
    if (!GetCommState(n->com, &d)) return;
    d.BaudRate = (DWORD)baud;
    d.ByteSize = (BYTE)dataBits;
    d.StopBits = stopHalves >= 4 ? TWOSTOPBITS : stopHalves == 3 ? ONE5STOPBITS : ONESTOPBIT;
    d.Parity = parity == 1 ? ODDPARITY : parity == 2 ? EVENPARITY : NOPARITY;
    d.fParity = parity != 0;
    d.fBinary = TRUE;
    d.fOutxCtsFlow = FALSE; d.fOutxDsrFlow = FALSE;
    d.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    d.fRtsControl = rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    d.fOutX = FALSE; d.fInX = FALSE;
    SetCommState(n->com, &d);
#else
    termios tio{};
    if (tcgetattr(n->com, &tio) != 0) return;
    static const struct { int baud; speed_t code; } speeds[] = {
        { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 }, { 200, B200 }, { 300, B300 },
        { 600, B600 }, { 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
        { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 } };
    speed_t code = B9600;
    int best = 1 << 30;
    for (const auto& s : speeds) { const int d = s.baud > baud ? s.baud - baud : baud - s.baud; if (d < best) { best = d; code = s.code; } }
    cfsetispeed(&tio, code);
    cfsetospeed(&tio, code);
    tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
    tio.c_cflag |= dataBits == 5 ? CS5 : dataBits == 6 ? CS6 : dataBits == 7 ? CS7 : CS8;
    if (stopHalves >= 3) tio.c_cflag |= CSTOPB;
    if (parity) tio.c_cflag |= PARENB | (parity == 1 ? PARODD : 0);
    tcsetattr(n->com, TCSANOW, &tio);
#endif
}

} // namespace cpcse
