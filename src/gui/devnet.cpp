// CPCSyntaxError — TCP for the development servers. See devnet.h.
#include "devnet.h"

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
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif
#include <cstring>

namespace cpcse::net {

namespace {

#ifdef _WIN32
using Native = SOCKET;
constexpr int SEND_FLAGS = 0;
#else
using Native = int;
#  if defined(MSG_NOSIGNAL)
constexpr int SEND_FLAGS = MSG_NOSIGNAL;   // a client that has gone must not raise SIGPIPE
#  else
constexpr int SEND_FLAGS = 0;
#  endif
#endif

Native native(Socket s) { return (Native)s; }

bool startup() {
#ifdef _WIN32
    static bool done = false, ok = false;
    if (!done) { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; done = true; }
    return ok;
#else
    return true;
#endif
}

bool validSocket(Native s) {
#ifdef _WIN32
    return s != INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

// The last call failed only because it would have had to wait (or a signal cut it short).
bool onlyWouldWait() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}

void setNonBlocking(Native s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

void noDelay(Native s) {
    int on = 1;   // small packets, answered at once
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

// Waits up to `ms` for the socket to be readable (or writable). select() on Windows,
// poll() elsewhere: select's descriptor sets stop at FD_SETSIZE on POSIX.
bool waitFor(Socket s, bool write, int ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(native(s), &set);
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    return select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv) > 0;
#else
    pollfd p{ native(s), (short)(write ? POLLOUT : POLLIN), 0 };
    return poll(&p, 1, ms) > 0;
#endif
}

sockaddr_in loopback(int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

} // namespace

Socket listenLocal(int port, std::string& error) {
    if (!startup()) { error = "the socket library did not start"; return NO_SOCKET; }
    if (port < 0 || port > 65535) { error = "port " + std::to_string(port) + " is not a port"; return NO_SOCKET; }
    Native s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!validSocket(s)) { error = "no socket"; return NO_SOCKET; }
#ifndef _WIN32
    // A restarted emulator can take its port back at once, past the last session's closing
    // connections. (Windows allows that without being asked, and still refuses a second
    // listener; its SO_REUSEADDR would let one in, and SO_EXCLUSIVEADDRUSE would make
    // closing connections block the port.)
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in a = loopback(port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0 || listen(s, 4) != 0) {
        // The system's own reason: "in use" is only one of them (Windows also refuses ports
        // it has reserved, with "access denied").
#ifdef _WIN32
        const int code = WSAGetLastError();
        error = "port " + std::to_string(port) + (code == WSAEADDRINUSE ? " is in use" : code == WSAEACCES
                ? " is reserved by the system (access denied)" : " cannot be opened (Winsock error " + std::to_string(code) + ")");
#else
        const int code = errno;
        error = "port " + std::to_string(port) + (code == EADDRINUSE ? " is in use" : std::string(": ") + std::strerror(code));
#endif
        closeSocket((Socket)s);
        return NO_SOCKET;
    }
    setNonBlocking(s);
    return (Socket)s;
}

int localPort(Socket s) {
    if (s == NO_SOCKET) return 0;
    sockaddr_in a{};
#ifdef _WIN32
    int len = sizeof(a);
#else
    socklen_t len = sizeof(a);
#endif
    if (getsockname(native(s), (sockaddr*)&a, &len) != 0) return 0;
    return ntohs(a.sin_port);
}

Socket acceptClient(Socket listener) {
    if (listener == NO_SOCKET) return NO_SOCKET;
    Native c = accept(native(listener), nullptr, nullptr);
    if (!validSocket(c)) return NO_SOCKET;
    setNonBlocking(c);
    noDelay(c);
    return (Socket)c;
}

int receiveSome(Socket s, char* buffer, int size) {
    if (s == NO_SOCKET) return -1;
    int n = (int)recv(native(s), buffer, size, 0);
    if (n > 0) return n;
    if (n < 0 && onlyWouldWait()) return 0;
    return -1;   // 0: the peer closed; < 0: it failed
}

bool sendAll(Socket s, const std::string& data) {
    if (s == NO_SOCKET) return false;
    size_t done = 0;
    int stalls = 0;
    while (done < data.size()) {
        int n = (int)send(native(s), data.data() + done, (int)(data.size() - done), SEND_FLAGS);
        if (n > 0) { done += (size_t)n; stalls = 0; continue; }
        // A full send buffer: wait for room, 10 s at most (a client that stopped reading).
        if (n < 0 && onlyWouldWait() && stalls++ < 2000) { waitFor(s, true, 5); continue; }
        return false;
    }
    return true;
}

void closeSocket(Socket s) {
    if (s == NO_SOCKET) return;
#ifdef _WIN32
    closesocket(native(s));
#else
    close(native(s));
#endif
}

Socket connectLocal(int port, std::string& error) {
    if (!startup()) { error = "the socket library did not start"; return NO_SOCKET; }
    Native s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!validSocket(s)) { error = "no socket"; return NO_SOCKET; }
    sockaddr_in a = loopback(port);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        error = "nothing is listening on port " + std::to_string(port) + " (is the emulator running with its command API on?)";
        closeSocket((Socket)s);
        return NO_SOCKET;
    }
    noDelay(s);
    return (Socket)s;
}

bool waitReadable(Socket s, int ms) { return s != NO_SOCKET && waitFor(s, false, ms); }

} // namespace cpcse::net
