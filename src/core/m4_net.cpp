// CPCSyntaxError — the M4 Board's WiFi through the host's network. See m4_net.h.
#include "m4_net.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif
#include <atomic>
#include <cctype>
#include <cstring>
#include <thread>

namespace cpcse {

namespace {

#ifdef _WIN32
using Native = SOCKET;
const Native BAD = INVALID_SOCKET;
constexpr int SEND_FLAGS = 0;
#else
using Native = int;
const Native BAD = -1;
#  if defined(MSG_NOSIGNAL)
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#  else
constexpr int SEND_FLAGS = 0;
#  endif
#endif

using Clock = std::chrono::steady_clock;

// lwIP's err_t as the status byte holds it (m4_net.h).
const uint8_t ERR_RTE = 0xfc;    // -4  no route
const uint8_t ERR_VAL = 0xfa;    // -6  illegal value (a name that does not resolve)
const uint8_t ERR_ABRT = 0xf3;   // -13 connection aborted (lwIP's own timeouts end this way)
const uint8_t ERR_RST = 0xf2;    // -14 connection reset (also a refused connect)

bool startup() {
#ifdef _WIN32
    static const bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
#else
    return true;
#endif
}

void closeNative(Native s) {
    if (s == BAD) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return e == EWOULDBLOCK || e == EAGAIN || e == EINTR;
#endif
}

bool connectPending(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS || e == EINTR;
#endif
}

uint8_t lwipError(int e) {
#ifdef _WIN32
    if (e == WSAECONNREFUSED || e == WSAECONNRESET || e == WSAECONNABORTED) return ERR_RST;
    if (e == WSAENETUNREACH || e == WSAEHOSTUNREACH || e == WSAENETDOWN) return ERR_RTE;
#else
    if (e == ECONNREFUSED || e == ECONNRESET || e == ECONNABORTED || e == EPIPE) return ERR_RST;
    if (e == ENETUNREACH || e == EHOSTUNREACH || e == ENETDOWN) return ERR_RTE;
#endif
    return ERR_ABRT;
}

void setNonBlocking(Native s, bool on) {
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

Native newTcpSocket() {
    if (!startup()) return BAD;
    Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD) return BAD;
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
#if defined(SO_NOSIGPIPE)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));   // macOS has no MSG_NOSIGNAL
#endif
    return s;
}

// a.b.c.d as a<<24 | b<<16 | c<<8 | d.
sockaddr_in address(uint32_t ip, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons((uint16_t)port);
    return a;
}

// What poll() would say about one socket, without waiting.
struct Ready { bool read = false, write = false, error = false; };
Ready readiness(Native s, int waitMs = 0) {
    Ready r;
#ifdef _WIN32
    // select, not WSAPoll: WSAPoll did not report a refused connect before Windows 10 2004.
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s, &rd); FD_SET(s, &wr); FD_SET(s, &ex);
    timeval tv{ waitMs / 1000, (waitMs % 1000) * 1000 };
    if (select(0, &rd, &wr, &ex, &tv) > 0) {
        r.read = FD_ISSET(s, &rd);
        r.write = FD_ISSET(s, &wr);
        r.error = FD_ISSET(s, &ex);
    }
#else
    pollfd p{};
    p.fd = s;
    p.events = POLLIN | POLLOUT;
    if (::poll(&p, 1, waitMs) > 0) {
        r.read = p.revents & (POLLIN | POLLHUP);
        r.write = p.revents & POLLOUT;
        r.error = p.revents & (POLLERR | POLLNVAL);
    }
#endif
    return r;
}

int socketError(Native s) {
    int e = 0;
#ifdef _WIN32
    int n = sizeof(e);
#else
    socklen_t n = sizeof(e);
#endif
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &n);
    return e;
}

bool resolve(const std::string& name, uint32_t& ip) {
    if (!startup() || name.empty() || name.size() > 253) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (addrinfo* a = res; a; a = a->ai_next)
        if (a->ai_family == AF_INET) {
            ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
            ok = true;
            break;
        }
    freeaddrinfo(res);
    return ok;
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

} // namespace

// ------------------------------------------------------------------ the jobs on threads
struct M4DnsJob {
    std::string name;
    std::atomic<bool> done{false};
    bool ok = false;
    uint32_t ip = 0;
};

struct M4HttpJob {
    std::string request;
    size_t offset = 0, maxBytes = 0;
    std::atomic<bool> done{false};
    bool taken = false;
    M4Network::HttpResult result;
};

struct M4Network::Sock {
    enum Phase { Fresh, Connecting, Connected, Listening, Closed } phase = Fresh;
    Native fd = BAD;
    std::deque<uint8_t> in;
    Bytes out;
    bool eof = false, bound = false, accepting = false;
    uint8_t endCode = 0;           // the status once the data is taken: 3, or an error
    uint8_t status = 0, lastcmd = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    Clock::time_point started;
    ~Sock() { closeNative(fd); }
};

M4Network::M4Network() { socks[0] = std::make_unique<Sock>(); }
M4Network::~M4Network() = default;   // a job still running keeps its own state (shared_ptr)

bool M4Network::active() const {
    if (dns || (http && !http->taken)) return true;
    for (int s = 1; s < SOCKETS; s++) if (socks[s] && socks[s]->fd != BAD) return true;
    return false;
}

void M4Network::closeSocket(int s) {
    if (s >= 1 && s < SOCKETS) socks[s].reset();
}

void M4Network::closeAll() {
    for (int s = 1; s < SOCKETS; s++) socks[s].reset();
    socks[0] = std::make_unique<Sock>();
    dns.reset();                                   // its thread finishes on its own
}

void M4Network::sockInfo(uint8_t* to) const {
    std::memset(to, 0, SOCK_INFO_SIZE);
    for (int s = 0; s < SOCKETS; s++) {
        const Sock* k = socks[s].get();
        if (!k) continue;
        uint8_t* p = to + s * 16;
        const size_t waiting = std::min<size_t>(k->in.size(), 0xffff);
        p[0] = k->status;
        p[1] = k->lastcmd;
        p[2] = (uint8_t)waiting;
        p[3] = (uint8_t)(waiting >> 8);
        for (int i = 0; i < 4; i++) p[4 + i] = (uint8_t)(k->ip >> (8 * i));   // +7 = a of a.b.c.d
        p[8] = (uint8_t)k->port;
        p[9] = (uint8_t)(k->port >> 8);
    }
}

std::string M4Network::hostAddress() {
    // A UDP "connect" only picks the route: nothing is sent.
    if (!startup()) return "";
    Native s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD) return "";
    std::string text;
    sockaddr_in to = address(0x08080808, 53);
    if (::connect(s, (sockaddr*)&to, sizeof to) == 0) {
        sockaddr_in me{};
#ifdef _WIN32
        int n = sizeof me;
#else
        socklen_t n = sizeof me;
#endif
        if (getsockname(s, (sockaddr*)&me, &n) == 0 && me.sin_addr.s_addr != 0) {
            const uint32_t ip = ntohl(me.sin_addr.s_addr);
            text = std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
                   std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
        }
    }
    closeNative(s);
    return text;
}

static bool parseDotted(const std::string& text, uint32_t& ip) {
    unsigned a, b, c, d;
    char tail;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip = a << 24 | b << 16 | c << 8 | d;
    return true;
}

void M4Network::netstat(std::vector<uint8_t>& out) const {
    // m4info: the string, then (v1.1.0) a status byte -- 5 = connected and got an IP.
    std::string text;
    int status;
    if (!wifiOn) { text = "WiFi is off"; status = 0; }
    else if (!linked) { text = "Not connected: the emulator's network is turned off"; status = 4; }
    else if (hostAddress().empty()) { text = "Not connected: the host has no network"; status = 4; }
    else { text = "Connected (through the host's network)"; status = 5; }
    text += "\r\n";
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
    out.push_back((uint8_t)status);
}

void M4Network::getNetwork(std::vector<uint8_t>& out) const {
    // m4info's structure (196 bytes); |NETSTAT prints ip..dns2 a first (M4ROM.s disp_ip).
    uint8_t b[196] = {};
    auto text = [&](int at, int size, const std::string& s) { for (int i = 0; i < size - 1 && i < (int)s.size(); i++) b[at + i] = (uint8_t)s[i]; };
    auto ip4 = [&](int at, uint32_t ip) { for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(ip >> (24 - 8 * i)); };
    text(0, 16, name);
    text(16, 32, ssid);
    text(48, 64, password);
    uint32_t ip = 0;
    parseDotted(hostAddress(), ip);
    const uint32_t gw = ip ? ((ip & 0xffffff00u) | 1) : 0;   // the host's own are not asked for
    ip4(112, ip);
    ip4(116, ip ? 0xffffff00u : 0);
    ip4(120, gw);
    ip4(124, gw);
    ip4(128, 0x08080808);
    b[132] = 1;                                    // DHCP
    text(140, 48, "pool.ntp.org");
    const uint8_t mac[6] = { 0x5c, 0xcf, 0x7f, 0x43, 0x50, 0x43 };   // Espressif's prefix, then "CPC"
    std::memcpy(b + 190, mac, 6);
    out.insert(out.end(), b, b + sizeof b);
}

bool M4Network::command(int cmd, const std::vector<int>& p, std::vector<uint8_t>& out) {
    auto arg = [&](size_t i) { return i < p.size() ? p[i] & 0xff : 0; };
    auto str = [&](size_t from) { std::string s; for (size_t i = from; i < p.size() && p[i]; i++) s += (char)p[i]; return s; };
    auto user = [&](int s) -> Sock* { return s >= 1 && s < SOCKETS ? socks[s].get() : nullptr; };
    switch (cmd) {
        case 0x4321: {                             // C_SETNETWORK "name=..., ssid=..., pw=..."
            const std::string all = str(0);
            size_t at = 0;
            while (at < all.size()) {
                size_t end = all.find(',', at);
                if (end == std::string::npos) end = all.size();
                std::string item = all.substr(at, end - at);
                at = end + 1;
                const size_t eq = item.find('=');
                if (eq == std::string::npos) continue;
                std::string key = item.substr(0, eq), value = item.substr(eq + 1);
                while (!key.empty() && key.front() == ' ') key.erase(key.begin());
                while (!key.empty() && key.back() == ' ') key.pop_back();
                while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                while (!value.empty() && value.back() == ' ') value.pop_back();
                key = lower(key);
                if (key == "name") name = value.substr(0, 15);
                else if (key == "ssid") ssid = value.substr(0, 31);
                else if (key == "pw") password = value.substr(0, 63);
            }
            return true;
        }
        case 0x4323: netstat(out); return true;    // C_NETSTAT
        case 0x4337: out.push_back(online() && !hostAddress().empty() ? 0xc9 : 31); return true;   // C_NETRSSI: -55 dBm; 31 = fail
        case 0x433b: getNetwork(out); return true; // C_GETNETWORK
        case 0x433c:                               // C_WIFIPOW
            wifiOn = arg(0) != 0;
            if (!wifiOn) closeAll();
            return true;
        case 0x4331: {                             // C_NETSOCKET (TCP whatever the arguments)
            int s = 0xff;
            if (online())
                for (int i = 1; i < SOCKETS; i++) if (!socks[i]) { socks[i] = std::make_unique<Sock>(); s = i; break; }
            out.push_back((uint8_t)s);
            return true;
        }
        case 0x4332: {                             // C_NETCONNECT socket, ip (last octet first), port
            Sock* k = user(arg(0));
            if (!online() || !k || k->phase != Sock::Fresh || p.size() < 7) { out.push_back(0xff); return true; }
            const uint32_t ip = (uint32_t)arg(1) | (uint32_t)arg(2) << 8 | (uint32_t)arg(3) << 16 | (uint32_t)arg(4) << 24;
            const int port = arg(5) | arg(6) << 8;
            k->fd = newTcpSocket();
            if (k->fd == BAD || port == 0) { closeNative(k->fd); k->fd = BAD; out.push_back(0xff); return true; }
            setNonBlocking(k->fd, true);
            const sockaddr_in to = address(ip, port);
            k->lastcmd = 3;
            k->started = Clock::now();
            if (::connect(k->fd, (const sockaddr*)&to, sizeof to) == 0) { k->phase = Sock::Connected; k->status = 0; }
            else if (connectPending(lastError())) { k->phase = Sock::Connecting; k->status = 1; }
            else { const int e = lastError(); closeNative(k->fd); k->fd = BAD; k->phase = Sock::Closed; k->status = lwipError(e); k->lastcmd = 6; }
            out.push_back(0);
            return true;
        }
        case 0x4333:                               // C_NETCLOSE
            closeSocket(arg(0));
            out.push_back(0);
            return true;
        case 0x4334: {                             // C_NETSEND socket, size, data
            Sock* k = user(arg(0));
            if (!k || k->phase != Sock::Connected || k->eof) { out.push_back(0xff); return true; }
            const size_t size = (size_t)(arg(1) | arg(2) << 8);
            for (size_t i = 3; i < p.size() && i - 3 < size; i++) k->out.push_back((uint8_t)p[i]);
            k->status = 2;
            k->lastcmd = 1;
            poll();                                // most of it goes at once
            out.push_back(0);
            return true;
        }
        case 0x4335: {                             // C_NETRECV socket, size (max 0x800)
            Sock* k = user(arg(0));
            if (!k) { out.push_back(0xff); return true; }
            const size_t want = std::min<size_t>((size_t)(arg(1) | arg(2) << 8), 0x800);
            const size_t n = std::min(want, k->in.size());
            out.push_back(0);
            out.push_back((uint8_t)n);
            out.push_back((uint8_t)(n >> 8));
            out.insert(out.end(), k->in.begin(), k->in.begin() + (std::ptrdiff_t)n);
            k->in.erase(k->in.begin(), k->in.begin() + (std::ptrdiff_t)n);
            k->lastcmd = 5;
            poll();                                // room again: take more from the host
            return true;
        }
        case 0x4336: {                             // C_NETHOSTIP name -> socket 0
            const std::string host = str(0);
            if (!online() || dns || host.empty()) { out.push_back(0xff); return true; }
            Sock& k = *socks[0];
            k.status = 5;
            k.lastcmd = 2;
            k.ip = 0;
            auto job = std::make_shared<M4DnsJob>();
            job->name = host;
            dns = job;
            std::thread([job] { job->ok = resolve(job->name, job->ip); job->done = true; }).detach();
            out.push_back(1);                      // "lookup in progress"
            return true;
        }
        case 0x4338: {                             // C_NETBIND socket, ip, port
            Sock* k = user(arg(0));
            if (!online() || !k || k->phase != Sock::Fresh || k->bound || p.size() < 7) { out.push_back(0xff); return true; }
            const uint32_t ip = (uint32_t)arg(1) | (uint32_t)arg(2) << 8 | (uint32_t)arg(3) << 16 | (uint32_t)arg(4) << 24;
            const int port = arg(5) | arg(6) << 8;
            k->fd = newTcpSocket();
            if (k->fd == BAD) { out.push_back(0xff); return true; }
#ifndef _WIN32
            int on = 1;
            setsockopt(k->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);   // a server run twice
#endif
            // The board listens on its LAN; here that is the host's, so only when allowed.
            const sockaddr_in at = address(allowLan ? ip : 0x7f000001, port);
            if (::bind(k->fd, (const sockaddr*)&at, sizeof at) != 0) { closeNative(k->fd); k->fd = BAD; out.push_back(0xff); return true; }
            setNonBlocking(k->fd, true);
            k->bound = true;
            out.push_back(0);
            return true;
        }
        case 0x4339: {                             // C_NETLISTEN
            Sock* k = user(arg(0));
            if (!k || !k->bound || k->phase != Sock::Fresh || ::listen(k->fd, 4) != 0) { out.push_back(0xff); return true; }
            k->phase = Sock::Listening;
            out.push_back(0);
            return true;
        }
        case 0x433a: {                             // C_NETACCEPT
            Sock* k = user(arg(0));
            if (!k || k->phase != Sock::Listening) { out.push_back(0xff); return true; }
            k->accepting = true;
            k->status = 4;
            k->lastcmd = 4;
            out.push_back(0);
            poll();
            return true;
        }
        default:
            return false;
    }
}

void M4Network::poll() {
    if (dns && dns->done) {
        Sock& k = *socks[0];
        k.status = dns->ok ? 0 : ERR_VAL;
        k.ip = dns->ok ? dns->ip : 0;
        dns.reset();
    }
    for (int s = 1; s < SOCKETS; s++) {
        Sock* k = socks[s].get();
        if (!k || k->fd == BAD) continue;
        if (k->phase == Sock::Connecting) {
            const Ready r = readiness(k->fd);
            if (r.write || r.error) {
                const int e = socketError(k->fd);
                if (e == 0 && !r.error) { k->phase = Sock::Connected; k->status = 0; }
                else { closeNative(k->fd); k->fd = BAD; k->phase = Sock::Closed; k->status = lwipError(e); k->lastcmd = 6; continue; }
            } else if (Clock::now() - k->started > std::chrono::seconds(20)) {
                closeNative(k->fd); k->fd = BAD; k->phase = Sock::Closed; k->status = ERR_ABRT; k->lastcmd = 6;
                continue;
            } else continue;
        }
        if (k->phase == Sock::Listening) {
            if (!k->accepting) continue;
            sockaddr_in from{};
#ifdef _WIN32
            int n = sizeof from;
#else
            socklen_t n = sizeof from;
#endif
            const Native c = ::accept(k->fd, (sockaddr*)&from, &n);
            if (c == BAD) continue;
            // The socket number is now the connection (tcpserv.s goes on with it).
            closeNative(k->fd);
            k->fd = c;
            setNonBlocking(c, true);
            int on = 1;
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
#if defined(SO_NOSIGPIPE)
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            k->phase = Sock::Connected;
            k->accepting = false;
            k->status = 0;
            k->ip = ntohl(from.sin_addr.s_addr);
            k->port = ntohs(from.sin_port);
        }
        if (k->phase != Sock::Connected) continue;
        // out
        while (!k->out.empty() && !k->eof) {
            const int n = ::send(k->fd, (const char*)k->out.data(), (int)std::min<size_t>(k->out.size(), 0x4000), SEND_FLAGS);
            if (n > 0) { k->out.erase(k->out.begin(), k->out.begin() + n); continue; }
            const int e = lastError();
            if (!wouldBlock(e)) { k->eof = true; k->endCode = lwipError(e); k->out.clear(); }
            break;
        }
        if (k->out.empty() && k->status == 2) k->status = 0;
        // in, while there is room: past that TCP's window holds the sender back
        uint8_t buf[0x1000];
        while (!k->eof && k->in.size() < RECEIVE_BUFFER) {
            const int n = ::recv(k->fd, (char*)buf, (int)std::min(sizeof buf, RECEIVE_BUFFER - k->in.size()), 0);
            if (n > 0) { k->in.insert(k->in.end(), buf, buf + n); continue; }
            if (n == 0) { k->eof = true; k->endCode = 3; break; }
            const int e = lastError();
            if (!wouldBlock(e)) { k->eof = true; k->endCode = lwipError(e); }
            break;
        }
        // "remote closed" once what came before the close has been taken: tcp.s and
        // tcpserv.s stop reading the moment they see it.
        if (k->eof && k->in.empty() && k->out.empty()) k->status = k->endCode;
    }
}

// ------------------------------------------------------------------ HTTP (|HTTPGET, |HTTPMEM)
bool M4Network::splitUrl(const std::string& urlIn, std::string& host, int& port, std::string& path) {
    std::string url = urlIn;
    while (!url.empty() && url.front() == ' ') url.erase(url.begin());
    while (!url.empty() && (url.back() == ' ' || url.back() == '\r' || url.back() == '\n')) url.pop_back();
    if (lower(url.substr(0, 7)) == "http://") url = url.substr(7);   // m4info v2.0.x: stripped
    const size_t slash = url.find('/');
    std::string hostPort = url.substr(0, slash);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    port = 80;
    const size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        const std::string digits = hostPort.substr(colon + 1);
        hostPort = hostPort.substr(0, colon);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return false;
        port = std::stoi(digits);
        if (port < 1 || port > 65535) return false;
    }
    host = hostPort;
    if (host.empty() || host.size() > 253) return false;
    for (char c : host) if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return false;
    for (char c : path) if ((unsigned char)c <= ' ' || c == 127) return false;   // no header injection
    return true;
}

namespace {

std::string headerValue(const std::string& headers, const std::string& key) {
    const std::string l = lower(headers), k = "\r\n" + lower(key) + ":";
    const size_t at = l.find(k);
    if (at == std::string::npos) return "";
    size_t from = at + k.size();
    const size_t end = headers.find("\r\n", from);
    std::string v = headers.substr(from, end == std::string::npos ? std::string::npos : end - from);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
}

bool unchunk(const Bytes& in, Bytes& out) {
    size_t i = 0;
    out.clear();
    while (i < in.size()) {
        size_t lineEnd = i;
        while (lineEnd + 1 < in.size() && !(in[lineEnd] == '\r' && in[lineEnd + 1] == '\n')) lineEnd++;
        if (lineEnd + 1 >= in.size()) return false;
        size_t size = 0;
        for (size_t j = i; j < lineEnd; j++) {
            const int c = in[j];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;                      // a chunk extension
            if (size > (SIZE_MAX >> 4)) return false;
            size = size * 16 + (size_t)d;
        }
        i = lineEnd + 2;
        if (size == 0) return true;
        if (size > in.size() - i) { out.insert(out.end(), in.begin() + (std::ptrdiff_t)i, in.end()); return false; }
        out.insert(out.end(), in.begin() + (std::ptrdiff_t)i, in.begin() + (std::ptrdiff_t)(i + size));
        i += size + 2;
    }
    return false;
}

// One GET; `limit` caps the bytes kept after the headers.
bool fetch(const std::string& host, int port, const std::string& path, size_t offset, size_t limit,
           int& code, std::string& headers, Bytes& body, std::string& error) {
    uint32_t ip = 0;
    if (!parseDotted(host, ip) && !resolve(host, ip)) { error = "Can't find " + host; return false; }
    Native s = newTcpSocket();
    if (s == BAD) { error = "No network"; return false; }
    setNonBlocking(s, true);
    const sockaddr_in to = address(ip, port);
    bool connected = ::connect(s, (const sockaddr*)&to, sizeof to) == 0;
    if (!connected && connectPending(lastError())) {
        const Ready r = readiness(s, 10000);
        connected = (r.write || r.error) && socketError(s) == 0 && !r.error;
    }
    if (!connected) { closeNative(s); error = "Can't connect to " + host; return false; }
    setNonBlocking(s, false);
#ifdef _WIN32
    DWORD ms = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
#else
    timeval tv{ 15, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + (port != 80 ? ":" + std::to_string(port) : "") +
                      "\r\nUser-Agent: M4 Board (CPCSyntaxError)\r\nAccept: */*\r\n";
    if (offset) req += "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(offset + limit - 1) + "\r\n";
    req += "Connection: close\r\n\r\n";
    for (size_t sent = 0; sent < req.size();) {
        const int n = ::send(s, req.data() + sent, (int)(req.size() - sent), SEND_FLAGS);
        if (n <= 0) { closeNative(s); error = "Lost the connection"; return false; }
        sent += (size_t)n;
    }
    // A server that answers 200 to a Range sends the whole file: keep reading to offset+limit.
    const size_t cap = 0x10000 + offset + limit;
    Bytes all;
    char buf[0x4000];
    bool timedOut = false;
    while (all.size() < cap) {
        const int n = ::recv(s, buf, (int)std::min(sizeof buf, cap - all.size()), 0);
        if (n == 0) break;
        if (n < 0) { timedOut = true; break; }
        all.insert(all.end(), buf, buf + n);
    }
    closeNative(s);
    const std::string head(all.begin(), all.begin() + (std::ptrdiff_t)std::min<size_t>(all.size(), 0x10000));
    const size_t end = head.find("\r\n\r\n");
    if (end == std::string::npos) { error = timedOut ? "Timed out" : "Not an HTTP answer"; return false; }
    headers = head.substr(0, end + 2);
    code = 0;
    if (std::sscanf(headers.c_str(), "HTTP/%*d.%*d %d", &code) != 1) { error = "Not an HTTP answer"; return false; }
    body.assign(all.begin() + (std::ptrdiff_t)(end + 4), all.end());
    if (lower(headerValue(headers, "Transfer-Encoding")).find("chunked") != std::string::npos) {
        Bytes plain;
        unchunk(body, plain);
        body.swap(plain);
    }
    if (timedOut && body.empty()) { error = "Timed out"; return false; }
    return true;
}

void runHttp(M4HttpJob& job) {
    M4Network::HttpResult& r = job.result;
    std::string host, path;
    int port = 80;
    if (lower(job.request.substr(0, 8)) == "https://") { r.error = "The M4 has no HTTPS: use http://"; return; }
    if (!M4Network::splitUrl(job.request, host, port, path)) { r.error = "Bad address: " + job.request; return; }
    for (int hop = 0; hop < 5; hop++) {
        int code = 0;
        std::string headers;
        Bytes body;
        if (!fetch(host, port, path, job.offset, job.maxBytes, code, headers, body, r.error)) return;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            const std::string where = headerValue(headers, "Location");
            if (lower(where.substr(0, 8)) == "https://") { r.error = "Moved to HTTPS, which the M4 has not: " + where; return; }
            if (!where.empty() && where[0] == '/') path = where;
            else if (!M4Network::splitUrl(where, host, port, path)) { r.error = "Bad redirect: " + where; return; }
            continue;
        }
        if (code == 206) { /* already from the offset */ }
        else if (code == 200) {
            if (job.offset >= body.size()) body.clear();
            else body.erase(body.begin(), body.begin() + (std::ptrdiff_t)job.offset);
        } else if (code == 416) body.clear();       // an offset past the end
        else { r.error = "HTTP error " + std::to_string(code); return; }
        if (body.size() > job.maxBytes) body.resize(job.maxBytes);
        // "attachment; filename=\"x.dsk\"" (m4info v2.0.x: used instead of the URL's)
        const std::string cd = headerValue(headers, "Content-Disposition");
        const size_t fn = lower(cd).find("filename=");
        if (fn != std::string::npos) {
            std::string f = cd.substr(fn + 9);
            if (!f.empty() && f[0] == '"') { f = f.substr(1); f = f.substr(0, f.find('"')); }
            else f = f.substr(0, f.find(';'));
            r.fileName = f;
        }
        r.body.swap(body);
        r.ok = true;
        return;
    }
    r.error = "Too many redirects";
}

} // namespace

void M4Network::startHttp(const std::string& request, size_t offset, size_t maxBytes) {
    auto job = std::make_shared<M4HttpJob>();
    job->request = request;
    job->result.request = request;
    job->offset = offset;
    job->maxBytes = std::max<size_t>(maxBytes, 1);
    http = job;
    if (!online()) { job->result.error = wifiOn ? "No network (turned off in the emulator)" : "WiFi is off"; job->done = true; return; }
    std::thread([job] { runHttp(*job); job->done = true; }).detach();
}

bool M4Network::httpRunning() const { return http && !http->done; }

bool M4Network::httpTake(HttpResult& result) {
    if (!http || !http->done || http->taken) return false;
    http->taken = true;
    result = std::move(http->result);
    http.reset();
    return true;
}

} // namespace cpcse
