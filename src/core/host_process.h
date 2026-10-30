// CPCSyntaxError — a program run with its standard input, output and error as pipes, for
// ffmpeg as a decoder (the SYMBiFACE III's formats minimp3 has not: sf3_audio.cpp).
// Threads move the bytes both ways, so the emulation never waits on the program; the output
// thread stops reading while `outputCap` bytes wait, which holds a fast decoder back.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace cpcse {

class HostProcess {
public:
    HostProcess();
    ~HostProcess();
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;

    size_t outputCap = 4u << 20;
    // args[0] is the program. withInput: its standard input is a pipe (else nothing).
    bool start(const std::vector<std::string>& args, bool withInput);
    void write(const unsigned char* data, size_t n);   // queued for its input
    void closeInput();                                 // the end of its input
    size_t available() const;                          // output bytes waiting
    size_t read(unsigned char* to, size_t n);          // takes up to n of them
    bool finished() const;                             // its output has ended and all is read
    std::string errorText() const;                     // its standard error (the first 64 KB)
    void stop();                                       // ends it now

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cpcse
