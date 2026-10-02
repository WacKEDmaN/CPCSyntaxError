// CPCSyntaxError — the machine's own mechanical noises: the disc drive (motor, head steps,
// a disc going in and coming out) and the keyboard. None of it is on the CPC's sound chip;
// on a real machine it reaches the ear from the drive and the keys, so the host mixes it
// in beside the AY. They are RECORDINGS: WAV files in a `sounds` folder beside the
// program (see the list below), played back -- nothing here is synthesised.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace cpcse {

class MachineSounds {
public:
    // Off unless a host turns them on: the headless runners and checkers never drain the
    // events, and an off source records none.
    bool driveEnabled = false;
    bool keysEnabled = false;
    int sampleRate = 44100;

    // The recordings, looked up in `dir` by name. A missing file is simply silent for that
    // event; a name with numbered variants (drive_step1.wav, drive_step2.wav, ...) is played
    // round-robin so a train of steps or a run of key presses does not repeat one sample.
    //   drive_motor_start.wav   the spindle running up            (optional)
    //   drive_motor.wav         the motor running -- looped while it is on
    //   drive_motor_stop.wav    the spindle running down          (optional)
    //   drive_step*.wav         the head moving one track
    //   drive_insert*.wav       a disc pushed in
    //   drive_eject*.wav        a disc ejected
    //   key_press*.wav          a key going down
    //   key_release*.wav        a key coming back up              (optional)
    //   key_space_press*.wav, key_return_press*.wav   the long keys (optional; else key_press)
    // Returns how many files were found.
    int load(const std::string& dir);
    int loadedCount() const { return loaded; }
    std::string folder() const { return dir; }

    // ---- events, from the FDC and the host ----------------------------------------
    void motor(bool on);              // the drive motor line (&FA7E bit 0)
    void steps(int count);            // the head moved `count` tracks, one step each
    void insert();
    void eject();
    enum class Key { Normal, Space, Return };
    void key(bool pressed, Key which = Key::Normal);

    // The next sample (mono), -1..1. One call per output sample.
    float next();
    bool silent() const;
    void reset();

private:
    struct Sample { std::vector<float> data; int rate = 44100; };
    struct Bank { std::vector<Sample> variants; size_t nextVariant = 0; const Sample* pick(); };
    Bank motorStart, motorLoop, motorStop, step, insertBank, ejectBank,
         keyPress, keyRelease, keySpace, keyReturn;
    int loaded = 0;
    std::string dir;

    // A sample being played: position in its own samples, advanced at its rate.
    struct Voice { const Sample* s; double pos; float gain; bool loop; };
    std::vector<Voice> voices;
    int motorVoice = -1;              // index of the looping motor voice, if any
    bool motorOn = false;
    std::deque<long> pendingSteps;    // output-sample times still to step (a seek's train)
    long clock = 0, nextStepFree = 0;
    void start(const Sample* s, float gain = 1.0f, bool loop = false);
    static bool readWav(const std::string& path, Sample& out);
    void loadBank(Bank& bank, const std::string& base);
};

extern MachineSounds machineSounds;

} // namespace cpcse
