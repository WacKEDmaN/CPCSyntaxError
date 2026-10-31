// CPCSyntaxError — video and audio recording.
//
// What the machine shows and plays, frame by frame, into a file:
//  - AVI, MJPEG: every frame a JPEG (4:4:4, so colours stay on their pixels). Most players
//    play it, the one Windows has among them.
//  - AVI, ZMBV: lossless (DOSBox's Zip Motion Blocks Video): a key frame now and then, and
//    between them only the 16x16 blocks that changed, XORed with the frame before and
//    deflated. Exact pixels and small files; VLC, ffmpeg and video editors built on them
//    play it.
//  - MP4, H.264: by ffmpeg, when it is installed (its program named by `ffmpeg`): the frames
//    go to it raw through a pipe while the sound is written to a WAV beside; at the end
//    ffmpeg joins the two (AAC sound) into the .mp4 and the two temporary files go. Small
//    files (x264, CRF 18) that every player plays.
//  - WAV: the sound only.
// The sound in an AVI is 16-bit stereo PCM. An AVI is split into numbered files at 1 GB
// (the old AVI index cannot point further).
//
// No library: the JPEG encoder (baseline, Annex K tables), the deflate compressor (fixed
// Huffman codes, LZ77) and the AVI writer are here. Frames are encoded on worker threads;
// the machine waits only when they fall a few frames behind.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cpcse {

// A baseline JPEG of an image (pixels 0xAABBGGRR, w and h multiples of 8 not needed).
std::vector<uint8_t> encodeJpeg(const uint32_t* pixels, int width, int height, int quality);
// A raw deflate stream of `data` (fixed Huffman blocks, not final), ended by a sync flush
// (an empty stored block), so the next call's output can follow it in the same stream.
void deflateSyncFlush(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

class MediaRecorder {
public:
    enum class Format { Mjpeg, Zmbv, Wav, H264 };
    std::string ffmpeg = "ffmpeg";           // H264: the ffmpeg program (a path, or found on the PATH)
    // ffmpeg's program when it can be run: the one given, else on the PATH; "" when none.
    static std::string findFfmpeg(const std::string& given);
    MediaRecorder();
    ~MediaRecorder();
    MediaRecorder(const MediaRecorder&) = delete;
    MediaRecorder& operator=(const MediaRecorder&) = delete;

    // frameMicroseconds: one emulated frame (19968 on a CPC). False and `error` if the
    // file cannot be made.
    bool start(const std::string& path, Format format, int width, int height, int sampleRate, double frameMicroseconds, int jpegQuality = 92);
    // One emulated frame: its picture (width x height, as given to start) and its sound
    // (interleaved stereo; empty when the sound is off: silence of the frame's length).
    void addFrame(const uint32_t* pixels, const int16_t* audio, size_t audioValues);
    // Every frame written, the file closed. Safe to call twice. For H264 the end (ffmpeg
    // finishing the video, then joining the sound) runs on a thread of its own and stop()
    // returns at once: finishing() while it runs, takeFinished() gives its result once (for
    // the status line), waitFinished() waits for every one (at exit).
    void stop();
    bool finishing() const;
    bool takeFinished(std::string& message);
    void waitFinished();

    bool active() const { return running; }
    Format format() const { return fmt; }
    std::string path() const { return basePath; }
    std::string error;
    uint64_t frames() const { return framesAdded; }
    uint64_t bytesWritten() const { return totalBytes.load(); }
    double seconds() const { return framesAdded * frameUs / 1e6; }
    int filesWritten() const { return fileIndex; }

private:
    struct Job {
        uint64_t index = 0;
        bool key = false, newFile = false;
        std::shared_ptr<std::vector<uint32_t>> picture, previous;
        std::vector<int16_t> audio;
        std::vector<uint8_t> video;          // encoded
    };
    struct IndexEntry { char id[4]; uint32_t flags, offset, size; };

    Format fmt = Format::Mjpeg;
    std::string basePath;
    int w = 0, h = 0, rate = 44100, quality = 92;
    double frameUs = 19968.0;
    double silenceCarry = 0.0;
    std::atomic<bool> running{ false };
    uint64_t framesAdded = 0;
    std::shared_ptr<std::vector<uint32_t>> lastPicture;
    std::atomic<uint64_t> totalBytes{ 0 };

    // workers
    std::mutex lock;
    std::condition_variable wake, done;
    std::deque<std::shared_ptr<Job>> todo;
    std::map<uint64_t, std::shared_ptr<Job>> finished;
    uint64_t nextToWrite = 0;
    bool quit = false;
    bool splitQueued = false;               // a frame that opens the next file is on its way
    std::vector<std::thread> workers;
    void work();
    void encode(Job& j);
    void writeReady();                       // with `lock` held

    // H264: ffmpeg's process (its stdin takes the frames), the two temporary files
    struct Encoder;
    std::unique_ptr<Encoder> encoder;
    std::string tempVideo, tempAudio;
    // ffmpeg ends, the two joined into `out`; the status line's text
    static std::string finishH264(Encoder& encoder, const std::string& program, const std::string& video,
                                  const std::string& audio, const std::string& out, double seconds);
    struct Finish;
    std::vector<std::shared_ptr<Finish>> finishes;
    mutable std::mutex finishLock;

    // the file
    std::FILE* file = nullptr;
    int fileIndex = 0;
    uint64_t fileBytes = 0;
    long moviStart = 0;                      // where the 'movi' fourcc is
    uint32_t fileVideoFrames = 0, fileAudioSamples = 0;
    std::vector<IndexEntry> fileIndexEntries;
    bool openFile();
    void closeFile();
    void writeChunk(const char id[4], const void* data, uint32_t size, bool key);
};

} // namespace cpcse
