// CPCSyntaxError — MIDI out to the host computer. See host_midi.h.
#include "host_midi.h"

#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#else
#include <dlfcn.h>
#include <cstring>
#endif

namespace cpcse {

namespace {

std::mutex lock;
std::function<void(const std::vector<int>&)> sink;
std::vector<int> message;            // the message being assembled
int runningStatus = 0;
bool inSysEx = false;
std::string lastError;

#if defined(_WIN32)
HMIDIOUT out = nullptr;
bool opened = false;
#elif defined(__APPLE__)
// The DLS synth into the default output, in an AUGraph (deprecated, still in every macOS).
AUGraph graph = nullptr;
AudioUnit synth = nullptr;
bool opened = false;
void openMac() {
    opened = true;
    AudioComponentDescription sd{}, od{};
    sd.componentType = kAudioUnitType_MusicDevice; sd.componentSubType = kAudioUnitSubType_DLSSynth;
    sd.componentManufacturer = kAudioUnitManufacturer_Apple;
    od.componentType = kAudioUnitType_Output; od.componentSubType = kAudioUnitSubType_DefaultOutput;
    od.componentManufacturer = kAudioUnitManufacturer_Apple;
    AUNode synthNode = 0, outNode = 0;
    if (NewAUGraph(&graph) != noErr || AUGraphAddNode(graph, &sd, &synthNode) != noErr || AUGraphAddNode(graph, &od, &outNode) != noErr ||
        AUGraphOpen(graph) != noErr || AUGraphConnectNodeInput(graph, synthNode, 0, outNode, 0) != noErr ||
        AUGraphNodeInfo(graph, synthNode, nullptr, &synth) != noErr || AUGraphInitialize(graph) != noErr || AUGraphStart(graph) != noErr) {
        synth = nullptr;
        lastError = "The macOS DLS synthesizer could not be started.";
    }
}
#else
// The ALSA sequencer through libasound, loaded at run time. An event is the library's
// snd_seq_event_t (28 bytes, a stable ABI); only its routing bytes are set here, the
// library's MIDI encoder writes the rest.
struct Alsa {
    void* lib = nullptr;
    void* seq = nullptr;
    void* encoder = nullptr;
    int port = -1;
    int (*seqOpen)(void**, const char*, int, int) = nullptr;
    int (*setName)(void*, const char*) = nullptr;
    int (*makePort)(void*, const char*, unsigned, unsigned) = nullptr;
    int (*connectTo)(void*, int, int, int) = nullptr;
    int (*outputDirect)(void*, void*) = nullptr;
    int (*encoderNew)(size_t, void**) = nullptr;
    long (*encodeByte)(void*, int, void*) = nullptr;
    int (*clientInfoMalloc)(void**) = nullptr;
    void (*clientInfoFree)(void*) = nullptr;
    void (*clientInfoSetClient)(void*, int) = nullptr;
    int (*clientInfoGetClient)(const void*) = nullptr;
    int (*nextClient)(void*, void*) = nullptr;
    int (*portInfoMalloc)(void**) = nullptr;
    void (*portInfoFree)(void*) = nullptr;
    void (*portInfoSetClient)(void*, int) = nullptr;
    void (*portInfoSetPort)(void*, int) = nullptr;
    int (*portInfoGetPort)(const void*) = nullptr;
    unsigned (*portInfoGetCaps)(const void*) = nullptr;
    unsigned (*portInfoGetType)(const void*) = nullptr;
    int (*nextPort)(void*, void*) = nullptr;
    int (*clientId)(void*) = nullptr;
};
Alsa alsa;
bool opened = false;
template <typename F> bool sym(F& f, const char* name) { f = reinterpret_cast<F>(dlsym(alsa.lib, name)); return f != nullptr; }
void openAlsa() {
    opened = true;
    alsa.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!alsa.lib) { lastError = "No MIDI out: libasound (ALSA) is not installed."; return; }
    const bool all = sym(alsa.seqOpen, "snd_seq_open") && sym(alsa.setName, "snd_seq_set_client_name") &&
        sym(alsa.makePort, "snd_seq_create_simple_port") && sym(alsa.connectTo, "snd_seq_connect_to") &&
        sym(alsa.outputDirect, "snd_seq_event_output_direct") && sym(alsa.encoderNew, "snd_midi_event_new") &&
        sym(alsa.encodeByte, "snd_midi_event_encode_byte") && sym(alsa.clientInfoMalloc, "snd_seq_client_info_malloc") &&
        sym(alsa.clientInfoFree, "snd_seq_client_info_free") && sym(alsa.clientInfoSetClient, "snd_seq_client_info_set_client") &&
        sym(alsa.clientInfoGetClient, "snd_seq_client_info_get_client") && sym(alsa.nextClient, "snd_seq_query_next_client") &&
        sym(alsa.portInfoMalloc, "snd_seq_port_info_malloc") && sym(alsa.portInfoFree, "snd_seq_port_info_free") &&
        sym(alsa.portInfoSetClient, "snd_seq_port_info_set_client") && sym(alsa.portInfoSetPort, "snd_seq_port_info_set_port") &&
        sym(alsa.portInfoGetPort, "snd_seq_port_info_get_port") && sym(alsa.portInfoGetCaps, "snd_seq_port_info_get_capability") &&
        sym(alsa.portInfoGetType, "snd_seq_port_info_get_type") && sym(alsa.nextPort, "snd_seq_query_next_port") &&
        sym(alsa.clientId, "snd_seq_client_id");
    if (!all) { lastError = "No MIDI out: this libasound lacks the sequencer functions."; return; }
    const int SND_SEQ_OPEN_OUTPUT = 1, CAP_READ = 1 << 0, CAP_WRITE = 1 << 1, CAP_SUBS_READ = 1 << 5, CAP_SUBS_WRITE = 1 << 6;
    const unsigned TYPE_MIDI_GENERIC = 1u << 1, TYPE_APPLICATION = 1u << 20, TYPE_SYNTH = 1u << 10, TYPE_SYNTHESIZER = 1u << 18;
    if (alsa.seqOpen(&alsa.seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { alsa.seq = nullptr; lastError = "No MIDI out: the ALSA sequencer could not be opened."; return; }
    alsa.setName(alsa.seq, "CPCSyntaxError");
    alsa.port = alsa.makePort(alsa.seq, "MIDI out", CAP_READ | CAP_SUBS_READ, TYPE_MIDI_GENERIC | TYPE_APPLICATION);
    if (alsa.port < 0 || alsa.encoderNew(65536, &alsa.encoder) < 0) { lastError = "No MIDI out: no ALSA port."; return; }
    // the first synthesizer that takes MIDI (not the "Midi Through" port, client 14)
    const int self = alsa.clientId(alsa.seq);
    void* ci = nullptr; void* pi = nullptr;
    alsa.clientInfoMalloc(&ci); alsa.portInfoMalloc(&pi);
    int toClient = -1, toPort = -1;
    alsa.clientInfoSetClient(ci, -1);
    while (toClient < 0 && alsa.nextClient(alsa.seq, ci) >= 0) {
        const int client = alsa.clientInfoGetClient(ci);
        if (client == self || client == 14 || client == 0) continue;
        alsa.portInfoSetClient(pi, client);
        alsa.portInfoSetPort(pi, -1);
        while (alsa.nextPort(alsa.seq, pi) >= 0) {
            const unsigned caps = alsa.portInfoGetCaps(pi), type = alsa.portInfoGetType(pi);
            if ((caps & (CAP_WRITE | CAP_SUBS_WRITE)) == (unsigned)(CAP_WRITE | CAP_SUBS_WRITE) && (type & (TYPE_SYNTH | TYPE_SYNTHESIZER))) {
                toClient = client; toPort = alsa.portInfoGetPort(pi); break;
            }
        }
    }
    alsa.clientInfoFree(ci); alsa.portInfoFree(pi);
    if (toClient >= 0) alsa.connectTo(alsa.seq, alsa.port, toClient, toPort);
    else lastError = "MIDI out is the ALSA port CPCSyntaxError:0; no synthesizer was found to connect it to (start FluidSynth or TiMidity, or use aconnect).";
}
#endif

// Data bytes after a status byte (the MIDI 1.0 specification's table).
int dataCount(int status) {
    if (status < 0xf0) {
        const int kind = status & 0xf0;
        return kind == 0xc0 || kind == 0xd0 ? 1 : 2;
    }
    switch (status) {
        case 0xf1: case 0xf3: return 1;
        case 0xf2: return 2;
        default: return 0;
    }
}

void deliver(const std::vector<int>& m) {
    if (sink) { sink(m); return; }
#if defined(_WIN32)
    if (!opened) {
        opened = true;
        if (midiOutOpen(&out, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { out = nullptr; lastError = "No MIDI out device on this computer."; }
    }
    if (!out) return;
    if (!m.empty() && m[0] == 0xf0) {
        std::vector<char> data(m.begin(), m.end());
        MIDIHDR header{};
        header.lpData = data.data();
        header.dwBufferLength = (DWORD)data.size();
        if (midiOutPrepareHeader(out, &header, sizeof header) == MMSYSERR_NOERROR) {
            midiOutLongMsg(out, &header, sizeof header);
            while (midiOutUnprepareHeader(out, &header, sizeof header) == MIDIERR_STILLPLAYING) Sleep(1);
        }
        return;
    }
    DWORD word = 0;
    for (size_t i = 0; i < m.size() && i < 3; i++) word |= (DWORD)(m[i] & 0xff) << (8 * i);
    midiOutShortMsg(out, word);
#elif defined(__APPLE__)
    if (!opened) openMac();
    if (!synth || m.empty()) return;
    if (m[0] == 0xf0) {
        std::vector<UInt8> data(m.begin(), m.end());
        MusicDeviceSysEx(synth, data.data(), (UInt32)data.size());
        return;
    }
    MusicDeviceMIDIEvent(synth, (UInt32)m[0], m.size() > 1 ? (UInt32)m[1] : 0, m.size() > 2 ? (UInt32)m[2] : 0, 0);
#else
    if (!opened) openAlsa();
    if (!alsa.seq || !alsa.encoder || alsa.port < 0) return;
    alignas(8) unsigned char event[64];
    std::memset(event, 0, sizeof event);
    for (int b : m) {
        if (alsa.encodeByte(alsa.encoder, b & 0xff, event) != 1) continue;
        event[3] = 253;                      // queue: SND_SEQ_QUEUE_DIRECT
        event[13] = (unsigned char)alsa.port; // source port
        event[14] = 254;                     // dest: SND_SEQ_ADDRESS_SUBSCRIBERS
        event[15] = 253;                     //       SND_SEQ_ADDRESS_UNKNOWN
        alsa.outputDirect(alsa.seq, event);
        std::memset(event, 0, sizeof event);
    }
#endif
}

} // namespace

void HostMidi::send(int byte) {
    std::lock_guard<std::mutex> g(lock);
    byte &= 0xff;
    if (byte >= 0xf8) { deliver({ byte }); return; }          // real time: anywhere, alone
    if (inSysEx) {
        message.push_back(byte);
        if (byte == 0xf7 || message.size() > 65536) { deliver(message); message.clear(); inSysEx = false; }
        if (byte < 0x80 || byte == 0xf7) return;
        // another status byte ends the SysEx without its F7, and starts its own message
        message.pop_back(); deliver(message); message.clear(); inSysEx = false;
    }
    if (byte == 0xf0) { message = { 0xf0 }; inSysEx = true; runningStatus = 0; return; }
    if (byte >= 0x80) {
        message = { byte };
        runningStatus = byte < 0xf0 ? byte : 0;
        if (dataCount(byte) == 0) { deliver(message); message.clear(); }
        return;
    }
    // a data byte: after its status, or under running status
    if (message.empty()) {
        if (!runningStatus) return;                              // no status yet: dropped
        message = { runningStatus };
    }
    message.push_back(byte);
    if ((int)message.size() - 1 >= dataCount(message[0])) { deliver(message); message.clear(); }
}

void HostMidi::setSink(std::function<void(const std::vector<int>&)> s) {
    std::lock_guard<std::mutex> g(lock);
    sink = std::move(s);
    message.clear(); runningStatus = 0; inSysEx = false;
}

std::string HostMidi::error() { std::lock_guard<std::mutex> g(lock); return lastError; }

void HostMidi::close() {
    std::lock_guard<std::mutex> g(lock);
#if defined(_WIN32)
    if (out) { midiOutReset(out); midiOutClose(out); out = nullptr; }
    opened = false;
#elif defined(__APPLE__)
    if (graph) { AUGraphStop(graph); DisposeAUGraph(graph); graph = nullptr; synth = nullptr; }
    opened = false;
#endif
}

} // namespace cpcse
