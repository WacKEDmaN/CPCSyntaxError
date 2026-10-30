// CPCSyntaxError — SYMBiFACE III expansion card (see sf3.h for the sources).
#include "sf3.h"

namespace cpcse {

static int clampi(double value, int mn, int mx) { return std::max(mn, std::min(mx, (int)value)); }
static int param(const std::deque<int>& p, size_t i) { return i < p.size() ? p[i] : 0; }

Symbiface3::Symbiface3() { enabled = false; sensitivity = 1; reset(); }
void Symbiface3::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
    reset();
    rtc.setEnabled(next);
}
void Symbiface3::reset() {
    dx = dy = dz = 0;
    buttons = { false, false, false };
    buttonChanged = false;
    sf3Echo = 0;
    params.clear(); reply.clear();
    status = errorNumber = 0;
    errorText.clear();
    oledRows = {}; oledTexts.clear(); oledScroll = 0;
    wifiIn.clear(); wifiOut.clear(); wifiStatus = 0; pending = Pending::None;
    for (auto& r : received) r.clear();
    net->closeAll();
    rtc.reset();
    audio->reset();
}
bool Symbiface3::handlesPort(int port) {
    if (!enabled) return false;
    port &= 0xffff;
    return port == SF3_CMD_PORT || port == SF3_DATA_PORT || port == SF3_ECHO_PORT || port == SF3_NET_PORT
        || port == SF3_NET_STATUS_PORT || (port >= SF3_RECEIVE_PORT && port < SF3_RECEIVE_PORT + 4) || Sf3Audio::isPort(port) || audio->isMidiPort(port);
}
void Symbiface3::setSensitivity(double value) {
    sensitivity = std::isfinite(value) ? std::max(0.25, std::min(4.0, value)) : 1;
}
void Symbiface3::move(double dxIn, double dyIn) {
    if (!enabled) return;
    dx += (int)std::lround(dxIn * sensitivity);
    dy += (int)std::lround(dyIn * sensitivity);
}
void Symbiface3::scroll(double delta) {
    if (!enabled) return;
    dz += (int)delta;
}
void Symbiface3::button(int index, bool pressed) {
    if (!enabled || index < 0 || index > 2) return;
    bool next = pressed;
    if (buttons[index] == next) return;
    buttons[index] = next;
    buttonChanged = true;
}

void Symbiface3::text(const std::vector<std::string>& lines) {
    reply.clear();
    for (const std::string& l : lines) { for (char c : l) reply.push_back((unsigned char)c); reply.push_back(13); }
    reply.push_back(10);
}
void Symbiface3::fail(int number, const std::string& why) {
    status = 2;
    errorNumber = number;
    errorText = why;
    reply.clear();
}

void Symbiface3::function(int number) {
    status = 0;
    switch (number) {
        case 20:
            reply.clear();
            reply.push_back(clampi(dx, -128, 127) & 0xff);
            reply.push_back(clampi(dy, -128, 127) & 0xff);
            reply.push_back((buttons[0] ? 1 : 0) | (buttons[1] ? 2 : 0) | (buttons[2] ? 4 : 0));
            reply.push_back(clampi(dz, -128, 127) & 0xff);
            dx = dy = dz = 0;
            buttonChanged = false;
            break;
        case 21: text({ "Mouse:USB HID", "Buttons:3", "Wheel:yes" }); break;
        case 54: {
            const std::string usb = audio->usbFolder;
            const size_t cut = usb.find_last_of("/\\");
            text({ "Drive:USB stick", "Mounted:" + std::string(usb.empty() ? "no" : "yes"),
                   "Folder:" + (usb.empty() ? std::string("-") : usb.substr(cut == std::string::npos ? 0 : cut + 1)) });
            break;
        }
        case 71: {
            static const char* modes[] = { "MP3A", "MP3B", "REG", "SYNTH", "FM" };
            text({ "System:SYMBiFACE III", "Firmware:SF3_CPC_20250422", "Emulator:CPCSyntaxError",
                   std::string("Audio:VS1053 ") + modes[(int)audio->mode], "RTC:on", "Mouse:USB" });
            break;
        }
        // Power supply and RTC battery in mV, the MPU in degrees: the board's nominal values
        // (5 V, a 3 V cell), as there is no board to measure.
        case 125: text({ "5000", "3000", "30.00" }); break;
        case 40: audio->setRoute(param(params, 0)); break;
        case 41: if (param(params, 0) == 1) audio->masterVolume = std::clamp(param(params, 1), 0, 100); break;
        case 72:
            switch (param(params, 0)) {
                case 4: reply.assign(1, errorNumber & 0xff); break;
                case 5: reply.clear(); for (char c : errorText) reply.push_back((unsigned char)c); reply.push_back(10); break;
                default: break;                                // 10 = the headset test: no headset here
            }
            break;
        case 80: audio->setBeep(param(params, 0), param(params, 1)); break;
        case 200: oledRows = {}; oledTexts.clear(); break;
        case 201: oledScroll = param(params, 0); break;
        case 202: case 203: {
            std::string s;
            for (int v : params) if (v >= 32) s.push_back((char)v);
            oledRows[(size_t)(number - 202)] = s;
            break;
        }
        case 210: {
            OledText t;
            t.x = std::clamp(param(params, 0), 0, 127); t.y = std::clamp(param(params, 1), 0, 31); t.font = param(params, 2);
            for (size_t i = 3; i < params.size(); i++) if (params[i] >= 32) t.text.push_back((char)params[i]);
            if (oledTexts.size() < 64) oledTexts.push_back(t);
            break;
        }
        case 100: case 101: case 102: case 110: case 111:
            rtc.writeBuffer.assign(params.begin(), params.end());
            if (number == 102) rtc.reset(); else rtc.issueCommand(number);
            reply.assign(rtc.readQueue.begin(), rtc.readQueue.end());
            rtc.readQueue.clear();
            break;
        case 7: wifiIn.clear(); wifiOut.clear(); break;
        case 90: wifi(); break;
        case 62: case 63: case 66: fail(number, "ROM AND FILE FUNCTIONS NOT EMULATED"); break;
        default: fail(number, "FUNCTION NOT EMULATED"); break;
    }
    params.clear();
}

// ------------------------------------------------------------------ WiFi

// The M4 network's codes (m4_net.cpp): C_NETSOCKET 4331 ... C_NETHOSTIP 4336, C_GETNETWORK 433B.
static const int NET_CONNECT = 0x4332, NET_CLOSE = 0x4333, NET_SEND = 0x4334, NET_RECV = 0x4335,
                 NET_HOSTIP = 0x4336, NET_GETNETWORK = 0x433b;

int Symbiface3::netStatus(int socket) {
    if (net->socketFree(socket + 1)) return 3;
    uint8_t info[M4Network::SOCK_INFO_SIZE];
    net->sockInfo(info);
    return info[(socket + 1) * 16];
}

void Symbiface3::take(int socket) {
    net->poll();
    std::deque<int>& r = received[(size_t)socket];
    while (r.size() < 0x800) {
        std::vector<uint8_t> out;
        net->command(NET_RECV, { socket + 1, 0x00, 0x08 }, out);
        const size_t n = out.size() >= 3 && out[0] == 0 ? (size_t)(out[1] | out[2] << 8) : 0;
        if (n == 0 || out.size() < 3 + n) break;
        r.insert(r.end(), out.begin() + 3, out.begin() + 3 + (std::ptrdiff_t)n);
    }
}

void Symbiface3::wifi() {
    wifiOut.clear();
    pending = Pending::None;
    errorNumber = 0;
    auto wifiFail = [&](int number, const std::string& why) { wifiStatus = 2; errorNumber = number; errorText = why; };
    auto in = [&](size_t i) { return i < wifiIn.size() ? wifiIn[i] & 0xff : 0; };
    if (wifiIn.empty()) { wifiFail(1, "NO WIFI COMMAND"); return; }
    const int sub = in(0), s = in(1) & 3;
    if (!net->online()) {
        if (sub == 17) wifiOut.push_back(2);           // "WIFI IS OFFLINE !!" (WEB-DK.BAS)
        wifiFail(2, "WIFI OFFLINE");
        return;
    }
    wifiStatus = 0;
    std::vector<uint8_t> out;
    switch (sub) {
        case 13: {                                     // a free socket
            int free = -1;
            for (int i = 0; i < 4 && free < 0; i++) if (net->socketFree(i + 1)) free = i;
            if (free >= 0) { net->openSocket(free + 1); received[(size_t)free].clear(); }
            wifiOut.push_back(free >= 0 ? free : 0xff);
            break;
        }
        case 5: {                                      // a b c d, port high low, socket
            const int sock = in(7) & 3;
            net->openSocket(sock + 1);                 // WEB-DK.BAS takes socket 0 without asking
            received[(size_t)sock].clear();
            net->command(NET_CONNECT, { sock + 1, in(4), in(3), in(2), in(1), in(6), in(5) }, out);
            if (out.empty() || out[0] == 0xff) { wifiFail(4, "CONNECTION FAILED"); break; }
            pending = Pending::Connect;
            pendingSocket = sock;
            wifiStatus = 1;
            wifiPoll();
            break;
        }
        case 10:
            net->command(NET_CLOSE, { s + 1 }, out);
            received[(size_t)s].clear();
            break;
        case 8: {
            const size_t n = wifiIn.size() > 2 ? wifiIn.size() - 2 : 0;
            std::vector<int> data = { s + 1, (int)(n & 0xff), (int)(n >> 8) };
            data.insert(data.end(), wifiIn.begin() + 2, wifiIn.end());
            net->command(NET_SEND, data, out);
            if (out.empty() || out[0] == 0xff) wifiFail(4, "SOCKET NOT CONNECTED");
            break;
        }
        case 17: {
            net->poll();
            const int st = netStatus(s);
            wifiOut.push_back(st == 1 ? 1 : st == 0 || st == 2 ? 0 : 3);
            break;
        }
        case 18: {
            take(s);
            const size_t n = received[(size_t)s].size();
            wifiOut.push_back((int)(n >> 8));
            wifiOut.push_back((int)(n & 0xff));
            break;
        }
        case 21: {                                     // the socket into the MP3 decoder
            take(s);
            std::deque<int>& r = received[(size_t)s];
            for (;;) {
                std::vector<unsigned char> chunk;
                while (!r.empty() && chunk.size() < audio->streamRoom()) { chunk.push_back((unsigned char)r.front()); r.pop_front(); }
                if (chunk.empty()) break;
                audio->feedStream(chunk.data(), chunk.size());
                if (audio->streamRoom() == 0) break;
                take(s);
            }
            break;
        }
        case 4: {                                      // DNS
            std::vector<int> name(wifiIn.begin() + 1, wifiIn.end());
            while (!name.empty() && name.back() == 0) name.pop_back();
            name.push_back(0);
            net->command(NET_HOSTIP, name, out);
            if (out.empty() || out[0] == 0xff) { wifiFail(5, "HOST NOT FOUND"); break; }
            pending = Pending::Dns;
            wifiStatus = 1;
            wifiPoll();
            break;
        }
        case 11: {                                     // MAC, IP, mask, gateway, DNS1, DNS2
            net->command(NET_GETNETWORK, {}, out);
            if (out.size() < 196) { wifiFail(1, "NO NETWORK"); break; }
            for (int i = 0; i < 6; i++) wifiOut.push_back(out[(size_t)(190 + i)]);
            for (int i = 0; i < 20; i++) wifiOut.push_back(out[(size_t)(112 + i)]);
            break;
        }
        default: wifiFail(sub, "WIFI COMMAND NOT KNOWN"); break;
    }
}

// What the WiFi is waiting for (a connect, a lookup), asked each time &FD4E is read.
void Symbiface3::wifiPoll() {
    if (wifiStatus != 1) return;
    net->poll();
    if (pending == Pending::Connect) {
        const int st = netStatus(pendingSocket);
        if (st == 1) return;
        if (st == 0 || st == 2) { wifiStatus = 0; errorNumber = 0; }
        else { wifiStatus = 2; errorNumber = st; errorText = "CONNECTION FAILED"; }
    } else if (pending == Pending::Dns) {
        uint8_t info[M4Network::SOCK_INFO_SIZE];
        net->sockInfo(info);
        if (info[0] == 5) return;                      // still looking
        if (info[0] == 0) { wifiStatus = 0; for (int i = 7; i >= 4; i--) wifiOut.push_back(info[i]); }
        else { wifiStatus = 2; errorNumber = 5; errorText = "HOST NOT FOUND"; }
    } else wifiStatus = 0;
    pending = Pending::None;
}

int Symbiface3::readPort(int port) {
    if (!handlesPort(port)) return 0xff;
    port &= 0xffff;
    if (Sf3Audio::isPort(port) || audio->isMidiPort(port)) return audio->readPort(port);
    if (port == SF3_ECHO_PORT) return sf3Echo & 0xff;
    if (port == SF3_CMD_PORT) return status;
    if (port == SF3_NET_STATUS_PORT) { wifiPoll(); return wifiStatus; }
    if (port == SF3_NET_PORT) {
        if (wifiOut.empty()) return 0;
        const int v = wifiOut.front();
        wifiOut.pop_front();
        return v;
    }
    if (port >= SF3_RECEIVE_PORT && port < SF3_RECEIVE_PORT + 4) {
        std::deque<int>& r = received[(size_t)(port - SF3_RECEIVE_PORT)];
        if (r.empty()) return 0;
        const int v = r.front();
        r.pop_front();
        return v;
    }
    if (reply.empty()) return 0;
    const int v = reply.front();
    reply.pop_front();
    return v;
}
bool Symbiface3::writePort(int port, int value) {
    if (!handlesWritePort(port)) return false;
    port &= 0xffff; value &= 0xff;
    if (Sf3Audio::isPort(port) || audio->isMidiPort(port)) { audio->writePort(port, value); return true; }
    if (port == SF3_ECHO_PORT) { sf3Echo = value; rtc.writePort(port, value); return true; }
    if (port == SF3_NET_PORT) { if (wifiIn.size() < 4096) wifiIn.push_back(value); return true; }
    if (port == SF3_NET_STATUS_PORT || (port >= SF3_RECEIVE_PORT && port < SF3_RECEIVE_PORT + 4)) return true;
    if (port == SF3_CMD_PORT) {
        if (value == 0) { params.clear(); reply.clear(); status = 0; }
        else function(value);
        return true;
    }
    if (params.size() < 2048) params.push_back(value);
    return true;
}

} // namespace cpcse
