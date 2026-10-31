// CPCSyntaxError GUI — shared widgets. See gui_widgets.h.
#include "gui_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>   // GetLogicalDrives: the file picker's drives
#endif

namespace cpcse {

void sectionHeading(const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("%s", text);
    ImGui::Separator();
}

ImVec4 rgbToVec(int rgb) {
    return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f);
}

bool beginFacts(const char* id, float nameWidth) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    // the names never take more than 45 % of a narrow pane: both columns' text wraps
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, std::min(nameWidth, ImGui::GetContentRegionAvail().x * 0.45f));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void fact(const char* name, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", name);
    ImGui::TableNextColumn();
    char buf[256];
    va_list args; va_start(args, fmt); std::vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    ImGui::TextUnformatted(buf);
}
void endFacts() { ImGui::EndTable(); }

void led(bool on, const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight() - 3;
    p.y += 1.5f;
    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), on ? IM_COL32(60, 230, 90, 255) : IM_COL32(40, 44, 52, 255));
    dl->AddRect(p, ImVec2(p.x + s, p.y + s), IM_COL32(90, 96, 110, 255));
    ImGui::Dummy(ImVec2(s + 2, s + 1));
    if (label) { ImGui::SameLine(); ImGui::TextUnformatted(label); }
}

// ============================================================== file dialogs
// ---- FlowRow: items on one line while they fit, the rest on the next
void FlowRow::place(float width) {
    if (!first) {
        // Decided before the cursor moves: SameLine followed by NewLine would leave the
        // SameLine position (past the edge) in the window's content width.
        const float gap = spacing >= 0.0f ? spacing : ImGui::GetStyle().ItemSpacing.x;
        const float edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;   // the line's right end
        if (ImGui::GetItemRectMax().x + gap + width <= edge + 0.5f) ImGui::SameLine(0.0f, spacing);
    }
    first = false;
}

static float labelWidth(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x; }

bool FlowRow::button(const char* label, float width) {
    const float w = width > 0.0f ? width : labelWidth(label) + ImGui::GetStyle().FramePadding.x * 2.0f;
    place(w);
    return ImGui::Button(label, ImVec2(width, 0.0f));
}

bool FlowRow::smallButton(const char* label) {
    place(labelWidth(label) + ImGui::GetStyle().FramePadding.x * 2.0f);
    return ImGui::SmallButton(label);
}

bool FlowRow::checkbox(const char* label, bool* v) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float text = labelWidth(label);
    place(ImGui::GetFrameHeight() + (text > 0.0f ? s.ItemInnerSpacing.x + text : 0.0f));
    return ImGui::Checkbox(label, v);
}

bool FlowRow::radio(const char* label, bool active) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float text = labelWidth(label);
    place(ImGui::GetFrameHeight() + (text > 0.0f ? s.ItemInnerSpacing.x + text : 0.0f));
    return ImGui::RadioButton(label, active);
}

// Text in a row: on the line when it fits, else on the next -- and wrapped there at the
// window's edge if even a whole line is too short (a long path).
void FlowRow::text(const char* t) { textColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), t); }
void FlowRow::textDisabled(const char* t) { textColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), t); }

void FlowRow::textColored(const ImVec4& colour, const char* t) {
    place(ImGui::CalcTextSize(t).x);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void ledRow(int value, int bits) {
    FlowRow row;
    row.spacing = 1.0f;
    for (int b = bits - 1; b >= 0; b--) { row.place(ImGui::GetTextLineHeight() - 1.0f); led((value >> b) & 1); }
}

void ledGrid(std::initializer_list<LedItem> items, float column) {
    const float startX = ImGui::GetCursorPosX();
    int col = 0;
    bool firstItem = true;
    for (const LedItem& it : items) {
        const float w = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(it.label).x;
        if (!firstItem) {
            // decided before the cursor moves (SameLine then NewLine would count the
            // SameLine position as content, past the edge)
            const float edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            if (startX + column * (float)col + w <= edge) ImGui::SameLine(startX + column * (float)col);
            else col = 0;
        }
        firstItem = false;
        led(it.on, it.label);
        col += 1;
    }
}

// work(begin, end) over 0..n in bands, one band a thread (up to 8), all done on return. Each
// band writes only its own rows, so the result is the same as one pass over 0..n.
//
// The worker threads are made ONCE and wait for work. A thread made and joined for every
// band, every frame, left memory behind in this process each time (measured: the CRT blur
// alone grew it 50 MB a second, to many GB; the same work on one thread grew it by nothing).
namespace {
class BandPool {
public:
    static BandPool& get() { static BandPool pool; return pool; }
    int parts() const { return (int)workers.size() + 1; }
    void run(int n, const std::function<void(int, int)>& work) {
        std::unique_lock<std::mutex> g(lock);           // one caller at a time
        const int p = parts();
        const int step = (n + p - 1) / p;
        job = &work; jobN = n; jobStep = step;
        pending = 0;
        for (int b = step; b < n; b += step) pending++;
        generation++;
        g.unlock();
        wake.notify_all();
        work(0, std::min(n, step));                     // the caller does the first band
        g.lock();
        done.wait(g, [&] { return pending == 0; });
        job = nullptr;
    }
private:
    BandPool() {
        const int n = std::clamp((int)std::thread::hardware_concurrency(), 1, 8) - 1;
        for (int i = 0; i < n; i++) workers.emplace_back([this, i] { loop(i); });
    }
    ~BandPool() {
        { std::lock_guard<std::mutex> g(lock); quit = true; generation++; }
        wake.notify_all();
        for (std::thread& t : workers) t.join();
    }
    void loop(int index) {
        long seen = 0;
        std::unique_lock<std::mutex> g(lock);
        for (;;) {
            wake.wait(g, [&] { return generation != seen; });
            seen = generation;
            if (quit) return;
            const int begin = (index + 1) * jobStep;    // band 0 is the caller's
            if (!job || begin >= jobN) continue;
            const std::function<void(int, int)>* w = job;
            const int end = std::min(jobN, begin + jobStep);
            g.unlock();
            (*w)(begin, end);
            g.lock();
            if (--pending == 0) done.notify_all();
        }
    }
    std::vector<std::thread> workers;
    std::mutex lock;
    std::condition_variable wake, done;
    const std::function<void(int, int)>* job = nullptr;
    int jobN = 0, jobStep = 0, pending = 0;
    long generation = 0;
    bool quit = false;
};
} // namespace

static void inBands(int n, const std::function<void(int, int)>& work) {
    BandPool& pool = BandPool::get();
    if (pool.parts() <= 1 || n < 64) { work(0, n); return; }
    pool.run(n, work);
}

void softenRender(const uint32_t* src, int w, int h, float amount, std::vector<uint32_t>& out) {
    w = std::max(1, w); h = std::max(1, h);
    const size_t n = (size_t)w * (size_t)h;
    out.resize(n);
    const double sigma = std::clamp((double)amount, 0.0, 1.0);
    if (sigma < 0.05) { std::copy(src, src + n, out.begin()); return; }
    // the kernel, out to 3 sigma, in 1/256ths that add up to 256 exactly
    const int r = std::max(1, (int)std::ceil(3.0 * sigma));
    std::vector<uint32_t> k((size_t)(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; i++) sum += std::exp(-(double)(i * i) / (2.0 * sigma * sigma));
    int total = 0;
    for (int i = -r; i <= r; i++) {
        k[(size_t)(i + r)] = (uint32_t)std::lround(256.0 * std::exp(-(double)(i * i) / (2.0 * sigma * sigma)) / sum);
        total += (int)k[(size_t)(i + r)];
    }
    k[(size_t)r] = (uint32_t)((int)k[(size_t)r] + 256 - total);
    // Red and blue in one word (bits 0-15 and 16-31), green in another (bits 8-23): a channel
    // times weights that add up to 256 is at most 65280, so the fields never meet.
    // (each band has its own sums and padded row)
    auto put = [w](uint32_t* to, const std::vector<uint32_t>& rb, const std::vector<uint32_t>& g) {
        for (int x = 0; x < w; x++)
            to[x] = 0xff000000u | ((rb[(size_t)x] + 0x00800080u) >> 8 & 0x00ff00ffu) | ((g[(size_t)x] + 0x00008000u) >> 8 & 0x0000ff00u);
    };
    // across: each row with its edge pixels repeated r times each side
    std::vector<uint32_t> across(n);
    inBands(h, [&](int y0, int y1) {
        std::vector<uint32_t> rb((size_t)w), g((size_t)w), pad((size_t)(w + 2 * r));
        for (int y = y0; y < y1; y++) {
            const uint32_t* row = src + (size_t)y * (size_t)w;
            for (int x = 0; x < r; x++) { pad[(size_t)x] = row[0]; pad[(size_t)(w + r + x)] = row[w - 1]; }
            std::copy(row, row + w, pad.begin() + r);
            std::fill(rb.begin(), rb.end(), 0u); std::fill(g.begin(), g.end(), 0u);
            for (int i = 0; i <= 2 * r; i++) {
                const uint32_t kk = k[(size_t)i];
                const uint32_t* p = pad.data() + i;
                for (int x = 0; x < w; x++) { rb[(size_t)x] += (p[x] & 0x00ff00ffu) * kk; g[(size_t)x] += (p[x] & 0x0000ff00u) * kk; }
            }
            put(across.data() + (size_t)y * (size_t)w, rb, g);
        }
    });
    // down: the rows above and below (the edge rows repeated)
    inBands(h, [&](int y0, int y1) {
        std::vector<uint32_t> rb((size_t)w), g((size_t)w);
        for (int y = y0; y < y1; y++) {
            std::fill(rb.begin(), rb.end(), 0u); std::fill(g.begin(), g.end(), 0u);
            for (int i = -r; i <= r; i++) {
                const uint32_t kk = k[(size_t)(i + r)];
                const uint32_t* p = across.data() + (size_t)std::clamp(y + i, 0, h - 1) * (size_t)w;
                for (int x = 0; x < w; x++) { rb[(size_t)x] += (p[x] & 0x00ff00ffu) * kk; g[(size_t)x] += (p[x] & 0x0000ff00u) * kk; }
            }
            put(out.data() + (size_t)y * (size_t)w, rb, g);
        }
    });
}

void slotMaskRender(const uint32_t* src, int sw, int sh, int w, int h, float strength, int stripe, bool turned,
                    std::vector<uint32_t>& out) {
    w = std::max(1, w); h = std::max(1, h);
    stripe = std::clamp(stripe, 1, 8);
    const double s = std::clamp((double)strength, 0.0, 1.0);
    const int triad = 3 * stripe, slotH = 3 * stripe, period = slotH + stripe;
    const int lit = stripe >= 3 ? stripe - 1 : stripe;             // the black matrix takes a column
    // a colour's light comes from lit/triad of the width and slotH/period of the height: the
    // beams give that back (a slot clips at full)
    const double gain = (double)triad * period / ((double)lit * slotH);
    // the picture as it is, at 1 - strength: the bridges, the matrix and the other colours
    // (integer: k/256 on red and blue together, then green)
    const uint32_t k = (uint32_t)std::lround((1.0 - s) * 256.0);
    std::vector<int> sx((size_t)w);
    for (int x = 0; x < w; x++) sx[(size_t)x] = std::min(sw - 1, (int)((long long)x * sw / w));
    out.resize((size_t)w * (size_t)h);
    inBands(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint32_t* row = src + (size_t)std::min(sh - 1, (int)((long long)y * sh / h)) * (size_t)sw;
            uint32_t* o = out.data() + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                const uint32_t v = row[sx[(size_t)x]];
                o[x] = (((v & 0xff00ffu) * k >> 8) & 0xff00ffu) | (((v & 0x00ff00u) * k >> 8) & 0x00ff00u) | 0xff000000u;
            }
        }
    });
    if (s <= 0.0) return;
    // each pixel's offset in the source and in `out`, split into its across and along parts
    // (the stripes run along; turned swaps the screen's axes)
    const int across = turned ? h : w, along = turned ? w : h;
    std::vector<size_t> srcA((size_t)across), srcL((size_t)along), outA((size_t)across), outL((size_t)along);
    for (int a = 0; a < across; a++) {
        srcA[(size_t)a] = turned ? (size_t)std::min(sh - 1, (int)((long long)a * sh / h)) * (size_t)sw : (size_t)sx[(size_t)a];
        outA[(size_t)a] = turned ? (size_t)a * (size_t)w : (size_t)a;
    }
    for (int l = 0; l < along; l++) {
        srcL[(size_t)l] = turned ? (size_t)sx[(size_t)l] : (size_t)std::min(sh - 1, (int)((long long)l * sh / h)) * (size_t)sw;
        outL[(size_t)l] = turned ? (size_t)l : (size_t)l * (size_t)w;
    }
    // the triads in bands: each writes only its own stripes (a0..a1 never overlap)
    const int triads = (across + triad - 1) / triad;
    inBands(triads, [&](int t0, int t1) {
    for (int t = t0; t < t1; t++)
        for (int c = 0; c < 3; c++) {
            const int a0 = t * triad + c * stripe, a1 = std::min(a0 + lit, across);
            if (a0 >= across) break;
            const int shift = 8 * c, off = (t & 1) * (period / 2);
            for (int l0 = -off; l0 < along; l0 += period) {
                const int lo = std::max(0, l0), hi = std::min(along, l0 + slotH);
                if (hi <= lo) continue;
                // the slot: the average of its colour, then the whole slot at that light
                uint32_t sum = 0;
                for (int a = a0; a < a1; a++)
                    for (int l = lo; l < hi; l++) sum += (src[srcA[(size_t)a] + srcL[(size_t)l]] >> shift) & 255;
                const double light = std::min(255.0, (double)sum / ((double)(a1 - a0) * (hi - lo)) * gain) * s;
                const uint32_t add = (uint32_t)(light + 0.5);
                for (int a = a0; a < a1; a++)
                    for (int l = lo; l < hi; l++) {
                        uint32_t& o = out[outA[(size_t)a] + outL[(size_t)l]];
                        const uint32_t now = std::min(255u, ((o >> shift) & 255) + add);
                        o = (o & ~(255u << shift)) | (now << shift);
                    }
            }
        }
    });
}

void softMaskRender(const uint32_t* src, int sw, int sh, int w, int h, float strength, int stripe, bool turned,
                    std::vector<uint32_t>& out) {
    w = std::max(1, w); h = std::max(1, h);
    stripe = std::clamp(stripe, 1, 8);
    const double s = std::clamp((double)strength, 0.0, 1.0);
    const int triad = 3 * stripe, slotPeriod = 4 * stripe;
    // the light the lines take, given back (clipped at full): 1/3 of the width at 1 - s, a
    // quarter of the rows at 1 - s/2
    const double mean = (2.0 + (1.0 - s)) / 3.0 * (3.0 + (1.0 - s * 0.5)) / 4.0;
    const double gain = 1.0 / std::max(0.2, mean);
    // three levels, as 1/256ths with the gain in: lit, a bridge row, a line (and a crossing)
    const uint32_t litK = (uint32_t)std::lround(256.0 * gain);
    const uint32_t bridgeK = (uint32_t)std::lround(256.0 * gain * (1.0 - s * 0.5));
    const uint32_t lineK = (uint32_t)std::lround(256.0 * gain * (1.0 - s));
    std::vector<int> sx((size_t)w);
    for (int x = 0; x < w; x++) sx[(size_t)x] = std::min(sw - 1, (int)((long long)x * sw / w));
    out.resize((size_t)w * (size_t)h);
    auto scaled = [](uint32_t v, uint32_t k) {
        const uint32_t r = std::min(255u, (v & 255) * k >> 8), g = std::min(255u, (v >> 8 & 255) * k >> 8), b = std::min(255u, (v >> 16 & 255) * k >> 8);
        return 0xff000000u | b << 16 | g << 8 | r;
    };
    inBands(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint32_t* row = src + (size_t)std::min(sh - 1, (int)((long long)y * sh / h)) * (size_t)sw;
            uint32_t* o = out.data() + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                // the stripes run along: across is x (y when turned)
                const int a = turned ? y : x, l = turned ? x : y;
                const bool line = a % triad >= 2 * stripe;                       // one line in three stripes
                const int stagger = (a / triad & 1) * (slotPeriod / 2);          // the slots of next triads offset
                const bool bridge = (l + stagger) % slotPeriod >= 3 * stripe;
                o[x] = scaled(row[sx[(size_t)x]], line ? lineK : bridge ? bridgeK : litK);
            }
        }
    });
}

bool checkboxWrapped(const char* label, bool* v) {
    const char* end = std::strstr(label, "##");
    ImGui::PushID(label);
    bool changed = ImGui::Checkbox("##box", v);
    if (end != label) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(label, end);
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    }
    ImGui::PopID();
    return changed;
}

void textWrappedDisabled(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}


static std::string lowerCase(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

std::vector<std::string> fileSystemRoots() {
    std::vector<std::string> roots;
#ifdef _WIN32
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++)
        if (drives & (1u << d)) roots.push_back(std::string(1, (char)('A' + d)) + ":\\");
#else
    roots.push_back("/");
#endif
    return roots;
}

bool enterTypedFolder(std::string& cwd, const std::string& typedIn) {
    namespace fs = std::filesystem;
    std::string typed = typedIn;
    while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\t')) typed.pop_back();
    if (typed.empty()) return false;
#ifdef _WIN32
    if (typed.size() == 2 && typed[1] == ':') typed += '\\';     // "D:" is that drive's root
#endif
    std::error_code ec;
    fs::path p = fs::path(cwd) / fs::path(typed);                // an absolute name replaces cwd
    if (!fs::is_directory(p, ec)) return false;
    fs::path full = fs::absolute(p, ec).lexically_normal();
    if (ec) return false;
    cwd = full.string();
    return true;
}

bool FolderView::draw(std::string& cwd, const std::vector<std::string>& exts, const char* selected,
                      float reserve, std::string& clicked, bool& doubleClicked) {
    namespace fs = std::filesystem;
    bool picked = false;
    // the drives: on Windows the only way off the one cwd started on
    const std::vector<std::string> roots = fileSystemRoots();
    if (roots.size() > 1) {
        const std::string here = lowerCase(fs::path(cwd).root_path().string());
        FlowRow drives;
        drives.spacing = 4.0f;
        for (const std::string& r : roots) {
            const std::string label = r.substr(0, r.size() - 1);  // "C:"
            const bool on = lowerCase(r) == here;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (drives.smallButton(label.c_str())) cwd = r;
            if (on) ImGui::PopStyleColor();
        }
    }
    {
        WrapText wrap;
        ImGui::TextUnformatted(cwd.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("list", ImVec2(0, -reserve), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("[..]")) {
        const fs::path here(cwd), parent = here.parent_path();
        if (!parent.empty() && parent != here && here != here.root_path()) cwd = parent.string();
    }
    if (cwd != listedDir || ImGui::GetTime() - listedAt > 1.0) {
        // Stepped with the error code: a range-for's ++ throws on an entry it cannot read
        // (a protected folder, a file gone mid-listing, an empty card reader), and nothing
        // would catch it.
        auto matches = [&](const fs::path& p) {
            if (exts.empty()) return true;
            const std::string e = lowerCase(p.extension().string());
            for (const auto& x : exts) if (e == x) return true;
            return false;
        };
        dirNames.clear(); fileNames.clear();
        std::error_code ec;
        fs::directory_iterator it(cwd, ec), end;
        for (int n = 0; !ec && it != end && n < 100000; it.increment(ec), n++) {
            std::error_code ec2;
            if (it->is_directory(ec2)) dirNames.push_back(it->path().filename().string());
            else if (it->is_regular_file(ec2) && matches(it->path())) fileNames.push_back(it->path().filename().string());
        }
        auto byName = [](const std::string& a, const std::string& b) { return lowerCase(a) < lowerCase(b); };
        std::sort(dirNames.begin(), dirNames.end(), byName); std::sort(fileNames.begin(), fileNames.end(), byName);
        listedDir = cwd;
        listedAt = ImGui::GetTime();
    }
    std::string into;
    for (const auto& d : dirNames) { std::string l = "[" + d + "]"; if (ImGui::Selectable(l.c_str())) into = d; }
    for (const auto& name : fileNames) {
        if (ImGui::Selectable(name.c_str(), selected && name == selected)) { clicked = name; picked = true; doubleClicked = false; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { clicked = name; picked = true; doubleClicked = true; }
    }
    ImGui::EndChild();
    if (!into.empty()) cwd = (fs::path(cwd) / into).string();
    return picked;
}

std::map<std::string, std::string>& rememberedFolders() {
    static std::map<std::string, std::string> folders;
    return folders;
}
std::string dialogFolderKey(const std::string& title, const std::vector<std::string>& exts) {
    std::string key = exts.empty() ? title : exts.front();
    // an .ini key: no '=', no spaces, lower case
    for (char& ch : key) ch = (ch == '=' || ch == ' ' || ch == '\t') ? '_' : (char)std::tolower((unsigned char)ch);
    return key;
}
std::string dialogStartFolder(const std::string& key, const std::string& fallback) {
    std::error_code ec;
    auto it = rememberedFolders().find(key);
    const std::string& want = it != rememberedFolders().end() && std::filesystem::is_directory(it->second, ec) ? it->second : fallback;
    return std::filesystem::exists(want, ec) ? std::filesystem::absolute(want, ec).string()
                                             : std::filesystem::current_path(ec).string();
}
void loadRememberedFolders(const std::map<std::string, std::string>& ini) {
    for (const auto& kv : ini)
        if (kv.first.rfind("dir.", 0) == 0 && kv.first.size() > 4 && !kv.second.empty())
            rememberedFolders()[kv.first.substr(4)] = kv.second;
}
void saveRememberedFolders(std::ostream& out) {
    for (const auto& kv : rememberedFolders()) out << "dir." << kv.first << "=" << kv.second << "\n";
}

void FileBrowser::open(const std::string& t, const std::string& startDir,
                       std::vector<std::string> extensions, std::function<void(const std::string&)> cb) {
    title = t; exts = std::move(extensions); onPick = std::move(cb); pickDir = false;
    folderKey = dialogFolderKey(title, exts);
    cwd = dialogStartFolder(folderKey, startDir);
    nameBuf[0] = 0; visible = true; requestOpen = true;
    view.relist();                      // listed afresh: these extensions
}
void FileBrowser::openDir(const std::string& t, const std::string& startDir, std::function<void(const std::string&)> cb) {
    title = t; exts.clear(); onPick = std::move(cb); pickDir = true;
    folderKey = dialogFolderKey(title, exts);
    cwd = dialogStartFolder(folderKey, startDir);
    nameBuf[0] = 0; visible = true; requestOpen = true;
    view.relist();
}
void FileBrowser::draw() {
    if (requestOpen) { ImGui::OpenPopup(title.c_str()); requestOpen = false; }
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(title.c_str(), &visible)) return;
    namespace fs = std::filesystem;
    std::string clicked;
    bool twice = false;
    const bool listed = view.draw(cwd, exts, nameBuf, ImGui::GetFrameHeightWithSpacing() * 2, clicked, twice);
    rememberedFolders()[folderKey] = cwd;          // where this kind of dialog is now
    if (listed) {
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", clicked.c_str());
        if (twice && !pickDir) {
            if (onPick) onPick((fs::path(cwd) / clicked).string());
            visible = false; ImGui::CloseCurrentPopup();
        }
    }
    if (pickDir) {
        if (ImGui::Button("Select this folder", ImVec2(-80, 0))) {
            if (onPick) onPick(cwd);
            visible = false; ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::SetNextItemWidth(-(52 + 56 + ImGui::GetStyle().ItemSpacing.x * 2 + 2));
        const bool enter = ImGui::InputTextWithHint("##name", "a file, or a folder to go to (D:\\games)", nameBuf, sizeof(nameBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Open", ImVec2(52, 0)) || enter) && nameBuf[0]) {
            if (enterTypedFolder(cwd, nameBuf)) nameBuf[0] = 0;   // a folder: go into it
            else {
                if (onPick) onPick((fs::path(cwd) / nameBuf).string());
                visible = false; ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(56, 0))) { visible = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void SaveDialog::open(const std::string& t, const std::string& startDir, const std::string& defName,
                      std::function<void(const std::string&)> cb) {
    title = t;
    const std::string ext = std::filesystem::path(defName).extension().string();
    folderKey = dialogFolderKey(title, ext.empty() ? std::vector<std::string>{} : std::vector<std::string>{ ext });
    dir = dialogStartFolder(folderKey, startDir);
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", defName.c_str());
    onSave = std::move(cb); visible = true; requestOpen = true;
    view.relist();
}
void SaveDialog::draw() {
    if (requestOpen) { ImGui::OpenPopup(title.c_str()); requestOpen = false; }
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(title.c_str(), &visible)) return;
    namespace fs = std::filesystem;
    // the folder to save in, browsed as the Open dialog's; a file clicked takes its name
    std::string clicked;
    bool twice = false;
    if (view.draw(dir, {}, nameBuf, ImGui::GetFrameHeightWithSpacing() * 2, clicked, twice))
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", clicked.c_str());
    rememberedFolders()[folderKey] = dir;          // where this kind of dialog is now
    {   // the name field takes what its label and the two buttons leave
        const ImGuiStyle& s = ImGui::GetStyle();
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("File name").x + s.ItemInnerSpacing.x + 56 * 2 + s.ItemSpacing.x * 2 + 2));
    }
    const bool enter = ImGui::InputText("File name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Save", ImVec2(56, 0)) || enter) && nameBuf[0]) {
        if (enterTypedFolder(dir, nameBuf)) nameBuf[0] = 0;       // a folder: go into it
        else {
            if (onSave) onSave((fs::path(dir) / nameBuf).string());
            visible = false; ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(56, 0))) { visible = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

} // namespace cpcse
