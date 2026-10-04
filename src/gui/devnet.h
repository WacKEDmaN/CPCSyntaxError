// CPCSyntaxError — the few TCP calls the development servers need (devserver.h), for
// Winsock and BSD sockets alike. Everything listens on 127.0.0.1 only: a debugger or a
// build script on this computer can reach the emulator, nothing on the network can.
#pragma once
#include <cstdint>
#include <string>

namespace cpcse::net {

using Socket = intptr_t;
constexpr Socket NO_SOCKET = -1;

// A listening socket on 127.0.0.1:port, non-blocking (port 0: one the system picks).
// NO_SOCKET and `error` on failure.
Socket listenLocal(int port, std::string& error);
// The port a socket is bound to (what port 0 became), or 0.
int localPort(Socket s);
// The next waiting client (non-blocking), or NO_SOCKET when none is waiting.
Socket acceptClient(Socket listener);
// Up to `size` bytes: > 0 read, 0 nothing waiting, -1 the peer closed or failed.
int receiveSome(Socket s, char* buffer, int size);
// All of `data`, waiting while the socket's buffer is full. false if the peer is gone.
bool sendAll(Socket s, const std::string& data);
void closeSocket(Socket s);
// A blocking connection to 127.0.0.1:port (for the cpcse-ctl client), or NO_SOCKET.
Socket connectLocal(int port, std::string& error);
// Waits up to `ms` for data to read (blocking clients). false on timeout.
bool waitReadable(Socket s, int ms);

} // namespace cpcse::net
