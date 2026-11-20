// Sight Reader - a sight-reading trainer that runs on the MPC.
//
// What it does
//   Shows a grand staff on the plugin's page, reads the notes you play on the track's MIDI
//   input (a LinnStrument, a keyboard, the pads), and checks them against a MIDI file or a
//   generated exercise. Wait mode holds on each note until you play it; Rhythm mode runs a
//   clock and grades timing; Flow mode runs the clock but waits a while for missed notes.
//   It makes no sound of its own except a short, quiet sound on mistakes (six timbres with
//   an ADSR envelope, on the SOUND page) and a metronome if you want one: put your instrument
//   on another track listening to the same input. LISTEN plays the piece in time as MIDI
//   through the plugin's own "MIDI Out" port, for any instrument track set to listen to it
//   (the MPC discards a plugin's VST MIDI output, so this is an ALSA sequencer port).
//
// How the screen works
//   The MPC draws plugin pages from JSON skins; a plugin cannot paint. The staff is a grid of
//   film-strip images, one per (slot, staff row), each bound to a "display" parameter. This
//   file picks the frame for every cell and pushes changes to the MPC with audioMasterAutomate
//   from the audio thread, a few dozen per block, the way the community wrapper drives
//   pictures and meters on the device. Text (titles, the file list) goes through parameter
//   display strings and audioMasterUpdateDisplay, at most ten times a second.
//
// Threads
//   audio thread: MIDI in, the practice engine, display parameters, sounds, LinnStrument lights
//   worker thread: library scan, MIDI loading and layout, exercise generation, progress files,
//                  finding the LinnStrument, reading the MPC project's key
//   They meet through atomics and a pointer hand-off; the audio thread never locks or allocates.
//
// SPDX-License-Identifier: MIT
#include "mpc_plugin.h"

#include "sr/gen/layout_gen.h"
#include "sr/gen/params_gen.h"
#include "sr/alsa_out.h"
#include "sr/engine.h"
#include "sr/files.h"
#include "sr/generator.h"
#include "sr/linn.h"
#include "sr/listen.h"
#include "sr/music.h"
#include "sr/score.h"
#include "sr/smf.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sr;

namespace {

constexpr int kVersion = 506;   // 0.5.6

// Set while this plugin writes one of its own parameters, so parameterChanged() ignores it.
// Per thread: the audio thread and the worker both write parameters.
thread_local bool inParamChange_ = false;

mpc::PluginInfo makeInfo()
{
    mpc::PluginInfo i;
    i.name = "Sight Reader";
    i.vendor = "Practice Tools";
    i.uniqueId = MPC_FOURCC('S', 'g', 'R', 'd');
    i.version = kVersion;
    i.isInstrument = true;
    i.numInputs = 0;
    i.numOutputs = 2;
    return i;
}

// Film-strip frame -> parameter value. Frame k of N shows at k/(N-1); a quarter frame of
// headroom keeps it exact whether the MPC rounds or truncates.
inline float frameValue(int frame, int nFrames)
{
    if (nFrames < 2 || frame <= 0) return 0.0f;
    if (frame >= nFrames - 1) return 1.0f;
    return (frame + 0.25f) / (float)(nFrames - 1);
}

inline int rowFrames(int row)
{
    int c = gen::kRowClass[row];
    return c == gen::kSpace ? gen::kFrames_space : (c == gen::kLine ? gen::kFrames_line : gen::kFrames_ledger);
}

std::string noteName(int n, bool flats = false)
{
    if (n < 0 || n > 127) return "-";
    char b[8]; std::snprintf(b, sizeof(b), "%s%d", pcName(n % 12, flats), n / 12 - 1);
    return b;
}

// Everything the worker builds for one piece; handed to the audio thread whole.
struct Loaded
{
    Score score;
    Fingering fing;
    std::vector<uint8_t> stepState, noteState, stepWrong;
    std::string path, progressKey, title;
    bool generated = false;
    std::vector<uint8_t> midiBytes;     // generated exercises, for "Save"
};

// Progress record from the audio thread to the worker.
struct PassRecord { int accuracy; double seconds; int tempo; bool finished; uint8_t sections[16]; };

template <typename T, int N> struct Spsc
{
    T items[N]; std::atomic<int> head{0}, tail{0};
    bool push(const T& v) { int h = head.load(std::memory_order_relaxed); int n = (h + 1) % N; if (n == tail.load(std::memory_order_acquire)) return false; items[h] = v; head.store(n, std::memory_order_release); return true; }
    bool pop(T& v) { int t = tail.load(std::memory_order_relaxed); if (t == head.load(std::memory_order_acquire)) return false; v = items[t]; tail.store((t + 1) % N, std::memory_order_release); return true; }
};

// The plugin's own sounds: the wrong-note sound (six timbres through an ADSR envelope),
// reminders, metronome clicks and the end-of-piece chime. Quiet by default; the SOUND page has
// the controls. No allocation; a handful of voices.
struct Synth
{
    struct Env { double a = 0.002, d = 0.16, s = 0.0, r = 0.06, len = 0.0; };   // seconds; s is a level
    enum Type { Wood = 0, Tick = 1, Thud = 2, Bell = 3, Sine = 4, Square = 5 };
    static constexpr double kBaseHz[6] = {1000.0, 2600.0, 110.0, 880.0, 660.0, 440.0};
    static constexpr float kLevel[6] = {0.9f, 1.4f, 0.93f, 1.13f, 0.92f, 1.12f};   // about equal peaks at amp 1
    struct Voice
    {
        bool on = false; int type = 0; double t = 0, freq = 440, ph = 0, ph2 = 0, gate = 0; float amp = 0;
        Env e; float held = 0, lp = 0, low = 0, band = 0; uint32_t rnd = 1; uint64_t age = 0;
    };
    static constexpr int kVoices = 8;
    Voice v[kVoices];
    uint64_t counter = 0;

    void trigger(int type, double hz, float amp, Env e)
    {
        if (amp <= 0.0001f) return;
        Voice* x = nullptr;
        for (Voice& c : v) if (!c.on) { x = &c; break; }
        if (!x) { x = &v[0]; for (Voice& c : v) if (c.age < x->age) x = &c; }
        *x = Voice();
        x->on = true; x->type = type < 0 || type > 5 ? 0 : type; x->freq = hz; x->amp = amp;
        e.a = std::max(e.a, 0.0005); e.d = std::max(e.d, 0.003); e.r = std::max(e.r, 0.003);
        x->e = e; x->gate = std::max(e.len, e.a + e.d);
        x->age = ++counter; x->rnd = 0x9E3779B9u ^ (uint32_t)(counter * 2654435761u);
    }
    void allOff() { for (Voice& c : v) c.on = false; }

    static float osc(Voice& x, double sr)
    {
        const double kTwoPi = 6.283185307179586;
        const double t = x.t;
        switch (x.type)
        {
            case Wood:
            {   // a struck wooden bar: two inharmonic partials, the upper one short, a slight drop in pitch
                double f = x.freq * (1.0 + 0.08 * std::exp(-t / 0.006));
                x.ph += f / sr; if (x.ph >= 1.0) x.ph -= 1.0;
                x.ph2 += f * 2.76 / sr; if (x.ph2 >= 1.0) x.ph2 -= 1.0;
                return (float)(0.72 * std::sin(kTwoPi * x.ph) + 0.28 * std::exp(-t / 0.012) * std::sin(kTwoPi * x.ph2));
            }
            case Tick:
            {   // noise through a resonant band-pass at the pitch
                x.rnd ^= x.rnd << 13; x.rnd ^= x.rnd >> 17; x.rnd ^= x.rnd << 5;
                float nz = (float)(int32_t)x.rnd * (1.0f / 2147483648.0f);
                float f = (float)(2.0 * std::sin(3.141592653589793 * std::min(x.freq, sr / 6.0) / sr));
                x.low += f * x.band;
                float high = nz - x.low - 0.3f * x.band;
                x.band += f * high;
                return x.band * 0.55f;
            }
            case Thud:
            {   // a low sine that starts higher and drops, like a soft drum
                double f = x.freq * (1.0 + 1.2 * std::exp(-t / 0.025));
                x.ph += f / sr; if (x.ph >= 1.0) x.ph -= 1.0;
                return (float)std::sin(kTwoPi * x.ph);
            }
            case Bell:
            {   // two-operator FM, the brightness fading
                double idx = 2.0 * std::exp(-t / 0.3);
                x.ph2 += x.freq * 3.5 / sr; if (x.ph2 >= 1.0) x.ph2 -= 1.0;
                x.ph += x.freq / sr; if (x.ph >= 1.0) x.ph -= 1.0;
                return (float)(0.8 * std::sin(kTwoPi * x.ph + idx * std::sin(kTwoPi * x.ph2)));
            }
            case Square:
            {   // the 0.1 square, rounded off by a low-pass
                x.ph += x.freq / sr; if (x.ph >= 1.0) x.ph -= 1.0;
                float sq = x.ph < 0.5 ? 0.8f : -0.8f;
                x.lp += (sq - x.lp) * (float)(1.0 - std::exp(-kTwoPi * 1800.0 / sr));
                return x.lp;
            }
            default:
                x.ph += x.freq / sr; if (x.ph >= 1.0) x.ph -= 1.0;
                return (float)std::sin(kTwoPi * x.ph);
        }
    }

    void render(float* l, float* r, int n, double sr, float gain)
    {
        const double dt = 1.0 / sr;
        for (Voice& x : v)
        {
            if (!x.on) continue;
            const float level = kLevel[x.type] * x.amp * gain;
            for (int i = 0; i < n; ++i)
            {
                float env;
                if (x.t < x.gate)
                {
                    if (x.t < x.e.a) env = (float)(x.t / x.e.a);
                    else env = (float)(x.e.s + (1.0 - x.e.s) * std::exp(-5.0 * (x.t - x.e.a) / x.e.d));
                    x.held = env;
                }
                else
                {
                    double k = (x.t - x.gate) / x.e.r;
                    if (k >= 1.0) { x.on = false; break; }
                    env = x.held * (float)(std::exp(-4.0 * k) * (1.0 - k));   // reaches zero at the end
                }
                float smp = osc(x, sr) * env * level;
                l[i] += smp; r[i] += smp;
                x.t += dt;
            }
        }
    }
};
constexpr double Synth::kBaseHz[6];
constexpr float Synth::kLevel[6];

} // namespace

class SightReader : public mpc::Plugin
{
public:
    SightReader() : mpc::Plugin(makeInfo())
    {
        for (int i = 0; i < gen::kNumControls; ++i)
        {
            const gen::ControlDef& d = gen::kControls[i];
            mpc::Param p;
            switch (d.kind)
            {
                case gen::kFloat: p = mpc::floatParam(d.id, d.name, d.min, d.max, d.def, d.unit, 1); break;
                case gen::kInt: case gen::kNote: p = mpc::intParam(d.id, d.name, (int)d.min, (int)d.max, (int)d.def, d.unit); break;
                case gen::kBool: case gen::kTrigger: p = mpc::boolParam(d.id, d.name, d.def >= 0.5f); break;
                default:
                {
                    std::vector<std::string> ch; std::string s = d.choices; size_t a = 0;
                    while (true) { size_t b = s.find('|', a); ch.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
                    p = mpc::choiceParam(d.id, d.name, ch, (int)d.def);
                }
            }
            p.automatable = (i == gen::P_TEMPO || i == gen::P_PLAY || i == gen::P_LOOP || i == gen::P_BLIP_VOL);
            addParam(p);
        }
        static const char* textNames[gen::kNumTexts] = {"Title", "Folder", "Row 1", "Row 2", "Row 3", "Row 4", "Row 5", "Row 6", "Row 7", "Row 8",
                                                         "Info 1", "Info 2", "Info 3", "Info 4", "Message", "Stats 1", "Stats 2", "Stats 3", "Stats 4",
                                                         "Linn", "MPC Key", "Exercise",
                                                         "Queue 1", "Queue 2", "Queue 3", "Queue 4", "Queue 5", "Queue 6", "Queue 7", "Queue 8", "Queue"};
        for (int i = 0; i < gen::kNumTexts; ++i)
        {
            char id[16]; std::snprintf(id, sizeof(id), "t%d", i);
            mpc::Param p = mpc::floatParam(id, textNames[i], 0, 1, 0, "", 3); p.automatable = false;
            addParam(p);
        }
        for (int i = gen::kDisplayBase; i < gen::kNumParams; ++i)
        {
            char id[16]; std::snprintf(id, sizeof(id), "d%d", i - gen::kDisplayBase);
            mpc::Param p = mpc::floatParam(id, "Display", 0, 1, 0, "", 3); p.automatable = false;
            addParam(p);
        }
        // Practice presets in the MPC's preset menu (settings only, in parameter order).
        addProgram("Wait - exact", preset(0, 100, 0, 160, 0, 0, 2));
        addProgram("Wait - relaxed", preset(0, 100, 0, 160, 1, 2, 2));
        addProgram("Flow - medium", preset(1, 90, 0, 200, 0, 1, 2));
        addProgram("Rhythm - strict", preset(2, 100, 0, 120, 0, 0, 2));
        addProgram("Rhythm - slow", preset(2, 60, 0, 220, 0, 1, 2));

        std::fill(desired_, desired_ + gen::kNumParams, -1.0f);
        texts_.assign(gen::kNumTexts, std::string());
        texts_[gen::T_TITLE] = "Sight Reader";
        texts_[gen::T_MESSAGE] = "LOAD a piece or GEN an exercise";
        places_.locate(mpc::pluginFolder());
        // the data wheel's first click steps from the default (not from nothing: 0.4.0 jumped)
        for (int i = 0; i < gen::kNumControls; ++i) { lastInt_[i] = (int)std::lround(get(i)); lastNorm_[i] = lastHostNorm_[i] = getNormalised(i); }
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~SightReader() override
    {
        {
            std::lock_guard<std::mutex> lk(wakeMx_);
            quit_ = true;
        }
        wakeCv_.notify_all();
        if (worker_.joinable()) worker_.join();
        delete incoming_.exchange(nullptr);
        delete retired_.exchange(nullptr);
        delete current_;
    }

    void prepare(double sampleRate, int) override { sr_ = sampleRate; }

    // The host calls this on every resume and after a state load. It must not move the player
    // back to the start (0.1 did: an MPC suspend/resume lost your place); a state load reloads
    // the piece through loadExtraState() anyway.
    void reset() override
    {
        accompAllOff();
        synth_.allOff();
        listenStopReq_.store(true);
        forceFullPush_ = true; forceCursor_ = gen::kDisplayBase;
    }

    // ---- parameter side effects (any thread: keep it to flags) --------------------------------
    void parameterChanged(int index, float plain) override
    {
        if (inParamChange_) return;
        if (index >= gen::kDisplayBase) return;
        if (index >= gen::kTextBase) return;
        if (gen::kControls[index].kind == gen::kInt || gen::kControls[index].kind == gen::kNote) nudge(index);
        switch (index)
        {
            case gen::P_RESTART: if (plain >= 0.5f) trig_restart_.store(true); break;
            case gen::P_LOAD: if (plain >= 0.5f) { trig_load_.store(true); wake(); } break;
            case gen::P_RESCAN: if (plain >= 0.5f) { trig_rescan_.store(true); wake(); } break;
            case gen::P_GEN_GO: if (plain >= 0.5f) { trig_gen_.store(true); wake(); } break;
            case gen::P_GEN_SAVE: if (plain >= 0.5f) { trig_save_.store(true); wake(); } break;
            case gen::P_LINN_SECT: if (plain >= 0.5f) trig_section_.store(true); break;
            case gen::P_LINN_CLEAR: if (plain >= 0.5f) trig_clear_.store(true); break;
            case gen::P_LINN_SYNC: if (plain >= 0.5f) { trig_keyread_.store(true); wake(); } break;
            case gen::P_SND_TEST: if (plain >= 0.5f) trig_sndtest_.store(true); break;
            case gen::P_COL_RESET: if (plain >= 0.5f) trig_colreset_.store(true); break;
            case gen::P_FOLDER: case gen::P_SCROLL: case gen::P_SORT: case gen::P_ROW:
                listSrc_.store(1); listGen_.fetch_add(1); listDirty_.store(true); wake(); break;
            case gen::P_SELECT: listSrc_.store(2); listGen_.fetch_add(1); listDirty_.store(true); wake(); break;
            case gen::P_LOOP_FROM: case gen::P_LOOP_TO: case gen::P_LOOP_BARS: if (!loadingState_.load()) loopRule(index); break;
            case gen::P_LOOP_SET_FROM: if (plain >= 0.5f) trig_setfrom_.store(true); break;
            case gen::P_LOOP_SET_TO: if (plain >= 0.5f) trig_setto_.store(true); break;
            case gen::P_GOTO_BAR: case gen::P_GOTO_STEP:
                if (!loadingState_.load()) locateReq_.store(index == gen::P_GOTO_BAR ? 1 : 2); break;
            case gen::P_BPM: bpmAuthority_.store(true); break;
            case gen::P_TEMPO: bpmAuthority_.store(false); break;
            case gen::P_PIECE: pieceSel_.store(true); wake(); break;
            case gen::P_PIECE_GO: if (plain >= 0.5f) { trig_piecego_.store(true); wake(); } break;
            case gen::P_PL_ADD: if (plain >= 0.5f) { trig_pladd_.store(true); wake(); } break;
            case gen::P_PL_REMOVE: if (plain >= 0.5f) { trig_plremove_.store(true); wake(); } break;
            case gen::P_LINN_PORT: trig_linnscan_.store(true); wake(); break;
            case gen::P_LINN_TEST: if (plain >= 0.5f) { trig_linntest_.store(true); trig_linnscan_.store(true); wake(); } break;
            case gen::P_QPANEL: if (plain >= 0.5f) { qpanelOpened_.store(true); wake(); } break;
            case gen::P_TRANSPOSE: case gen::P_SPLIT: case gen::P_OUT_RANGE: case gen::P_RANGE_LO: case gen::P_RANGE_HI:
                if (index == gen::P_TRANSPOSE || index == gen::P_SPLIT || (int)get(gen::P_OUT_RANGE) == 2) { trig_rebuild_.store(true); wake(); }
                break;
            case gen::P_LINN_SIZE: case gen::P_LINN_ROWOFF: case gen::P_LINN_LOW: case gen::P_LINN_LEFT:
            case gen::P_LINN_OCT: case gen::P_LINN_SEMI: case gen::P_LINN_FIT:
                trig_rebuild_.store(true); layoutChanged_.store(true); wake(); break;
            case gen::P_LINN: trig_keyread_.store(true); trig_linnscan_.store(true); trig_rebuild_.store(true); layoutChanged_.store(true); wake(); break;
            case gen::P_LINN_KEY: trig_keyread_.store(true); trig_linnscan_.store(true); wake(); break;
            case gen::P_LINN_FOLLOW: linnAskNow_.store(true); wake(); break;
            default:
                if (index >= gen::P_PICK0 && index <= gen::P_PICK7 && plain >= 0.5f) { pick_.store(index - gen::P_PICK0); listDirty_.store(true); wake(); }
                else if (index >= gen::P_QPICK0 && index <= gen::P_QPICK7 && plain >= 0.5f) { qpick_.store(index - gen::P_QPICK0); wake(); }
                break;
        }
    }

    // The entry point marks a state load (effSetChunk), during which values are restored as saved.
    void setLoadingState(bool on) { loadingState_.store(on); }
    std::mutex& stateMutex() { return stateMx_; }
    // Writes one of our own parameters from inside the plugin, keeping nudge()'s memory in step
    // so the next data-wheel move goes on from the value shown.
    void setTracked(int index, float plain)
    {
        inParamChange_ = true;
        set(index, plain);
        inParamChange_ = false;
        lastInt_[index] = (int)std::lround(get(index));
        lastNorm_[index] = getNormalised(index);
        needNotify_[index].store(true);
    }

    // Loop From, Loop To and Loop Bars follow each other: moving From keeps the length, moving
    // To keeps the start (and pulls From back when To passes it), Bars stretches from From.
    // All stay inside the piece.
    void loopRule(int changed)
    {
        int first = std::max(1, firstBar_.load()), last = std::max(first, lastBar_.load());
        const int maxBars = (int)gen::kControls[gen::P_LOOP_BARS].max;
        int from = (int)std::lround(get(gen::P_LOOP_FROM)), to = (int)std::lround(get(gen::P_LOOP_TO));
        int bars = (int)std::lround(get(gen::P_LOOP_BARS));
        from = std::max(first, std::min(last, from));
        if (changed == gen::P_LOOP_TO)
        {
            to = std::max(first, std::min(last, to));
            if (to < from) from = to;
            bars = to - from + 1;
            if (bars > maxBars) { from = to - maxBars + 1; bars = maxBars; }
        }
        else
        {
            to = from + bars - 1;
            if (to > last) { to = last; if (changed == gen::P_LOOP_BARS) from = std::max(first, to - bars + 1); bars = to - from + 1; }
        }
        if (from != (int)std::lround(get(gen::P_LOOP_FROM))) setTracked(gen::P_LOOP_FROM, (float)from);
        if (to != (int)std::lround(get(gen::P_LOOP_TO))) setTracked(gen::P_LOOP_TO, (float)to);
        if (bars != (int)std::lround(get(gen::P_LOOP_BARS))) setTracked(gen::P_LOOP_BARS, (float)bars);
    }

    // Q-Link and data-wheel moves arrive as the read-back value plus 1/128 (wheel 1/100) of the
    // range; on a long list that skips entries. A small move counts as one step.
    // The MPC's data wheel moves a number by a hundredth of its range per click (the "coarse"
    // increment, Akai's ui-reference 3.3), which lands on uneven values: Tempo 100, 102, 104,
    // 105..., a 1-16 channel needing several clicks per step. A move of that size is taken as one
    // step from where the value was: one unit, or a round number of milliseconds on the long time
    // ranges. Whether the MPC adds its hundredth to the value it reads back or to the one it last
    // sent, the click is recognised. Other moves (a knob dragged, the big editor, a Q-Link turned
    // fast) are taken as they come.
    static bool wheelClick(float d) { float a = std::fabs(d); return a > 0.0093f && a < 0.0107f; }
    int wheelQuantum(int index) const
    {
        const gen::ControlDef& c = gen::kControls[index];
        if (c.kind != gen::kInt || std::strcmp(c.unit, "ms") != 0) return 1;
        const double raw = (c.max - c.min) / 100.0;
        static const int nice[] = {1, 2, 5, 10, 20, 50, 100};
        int best = 1;
        for (int q : nice) if (std::fabs(q - raw) < std::fabs(best - raw)) best = q;
        return best;
    }
    void nudge(int index)
    {
        const float norm = getNormalised(index);
        if (loadingState_.load())
        {   // a project or preset loading: take the value as it comes
            lastInt_[index] = (int)std::lround(get(index));
            lastNorm_[index] = lastHostNorm_[index] = norm;
            return;
        }
        const float dOurs = norm - lastNorm_[index], dHost = norm - lastHostNorm_[index];
        lastHostNorm_[index] = norm;
        const int prev = lastInt_[index];
        int now = (int)std::lround(get(index));
        // a click at either end of the range is cut short by the clamp
        auto clipped = [norm](float d) { return std::fabs(d) > 1e-6f && std::fabs(d) < 0.0107f && ((norm <= 1e-4f && d < 0) || (norm >= 1.0f - 1e-4f && d > 0)); };
        const bool oursClick = wheelClick(dOurs) || clipped(dOurs), hostClick = wheelClick(dHost) || clipped(dHost);
        if (oursClick || hostClick)
        {
            const gen::ControlDef& c = gen::kControls[index];
            const float dir = oursClick ? dOurs : dHost;
            const int q = wheelQuantum(index);
            int stepped = prev + (dir > 0 ? q : -q);
            if (q > 1) stepped = (int)std::lround(stepped / (double)q) * q;
            stepped = std::max((int)c.min, std::min((int)c.max, stepped));
            if (stepped != now)
            {
                inParamChange_ = true;
                set(index, (float)stepped);
                inParamChange_ = false;
                needNotify_[index].store(true);
                now = stepped;
            }
        }
        lastInt_[index] = now;
        lastNorm_[index] = getNormalised(index);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index >= gen::kDisplayBase) { out.clear(); return true; }
        if (index >= gen::kTextBase)
        {
            std::lock_guard<std::mutex> lk(textMx_);
            out = texts_[(size_t)(index - gen::kTextBase)];
            return true;
        }
        if (index < gen::kNumControls && gen::kControls[index].kind == gen::kNote) { out = noteName((int)std::lround(plain)); return true; }
        if (index == gen::P_FOLDER)
        {
            std::lock_guard<std::mutex> lk(textMx_);
            out = texts_[gen::T_FOLDER_NAME];
            return true;
        }
        if (index == gen::P_LOOP_FROM || index == gen::P_LOOP_TO || index == gen::P_GOTO_BAR || index == gen::P_LOOP_BARS)
        {
            int v = (int)std::lround(plain);
            char b[32]; std::snprintf(b, sizeof(b), index == gen::P_LOOP_BARS ? (v == 1 ? "%d bar" : "%d bars") : "Bar %d", v);
            out = b; return true;
        }
        if (index == gen::P_BPM || index == gen::P_PIECE || index == gen::P_SELECT)
        {
            int v = (int)std::lround(plain);
            char b[32];
            if (index == gen::P_BPM) std::snprintf(b, sizeof(b), "%d BPM", v);
            else std::snprintf(b, sizeof(b), index == gen::P_PIECE ? "Piece %d" : "File %d", index == gen::P_SELECT ? v + 1 : v);
            out = b; return true;
        }
        // values with their unit: the MPC shows the display text only, never the unit label
        if (index < gen::kNumControls && gen::kControls[index].kind == gen::kInt)
        {
            int v = (int)std::lround(plain);
            const std::string unit = gen::kControls[index].unit;
            char b[32];
            if (index == gen::P_OUT_CH || index == gen::P_LINN_CH) std::snprintf(b, sizeof(b), "Ch %d", v);
            else if (index == gen::P_LINN_OCT) std::snprintf(b, sizeof(b), v ? "%+d oct" : "0 oct", v);
            else if (index == gen::P_GEN_RHYTHM || index == gen::P_GEN_LEAPS) std::snprintf(b, sizeof(b), "Level %d", v);
            else if (index == gen::P_REMIND_MAX) std::snprintf(b, sizeof(b), v == 1 ? "%d time" : "%d times", v);
            else if (unit == "%") std::snprintf(b, sizeof(b), "%d%%", v);
            else if (unit == "st") std::snprintf(b, sizeof(b), v ? "%+d st" : "0 st", v);
            else if (!unit.empty()) std::snprintf(b, sizeof(b), "%d %s", v, unit.c_str());
            else return false;
            out = b; return true;
        }
        return false;
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> lk(textMx_);
        std::string s = "path=" + currentPath_ + "\n" + "bpm=" + (bpmAuthority_.load() ? "1" : "0") + "\n";
        out.assign(s.begin(), s.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::string s((const char*)data, size);
        size_t b = s.find("bpm=");
        if (b != std::string::npos && b + 4 < s.size()) bpmAuthority_.store(s[b + 4] == '1');
        size_t p = s.find("path=");
        if (p == std::string::npos) return;
        size_t e = s.find('\n', p);
        std::string path = s.substr(p + 5, e == std::string::npos ? std::string::npos : e - p - 5);
        if (!path.empty())
        {
            std::lock_guard<std::mutex> lk(textMx_);
            restorePath_ = path;
        }
        trig_restore_.store(true);
        wake();
    }

    // ---- audio ---------------------------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        ++blockCounter_;
        sampleClock_ += (uint64_t)std::max(0, n);
        // 1. a new piece from the worker
        if (Loaded* in = incoming_.load(std::memory_order_acquire))
        {
            if (retired_.load(std::memory_order_acquire) == nullptr)
            {
                incoming_.store(nullptr, std::memory_order_release);
                accompAllOff();
                guideAllOff();
                if (listening_) { stopListen(); setParamFromEngine(gen::P_LISTEN, 0.0f); }
                Loaded* old = current_;
                current_ = in;
                engine_.setScore(&current_->score, current_->stepState.data(), current_->noteState.data(), current_->stepWrong.data());
                {
                    const Score& sc = current_->score;
                    firstBar_.store(sc.measures.front().number);
                    lastBar_.store(sc.measures.back().number);
                    loopRule(gen::P_LOOP_FROM);
                }
                readSettings();
                engine_.restart();
                {   // back to the bar you were in when a setting rebuilt the piece
                    const int kb = keepBar_.exchange(-1);
                    const Score& sc = current_->score;
                    if (kb > 0)
                        for (size_t mi = 0; mi < sc.measures.size(); ++mi)
                            if (sc.measures[mi].number == kb) { engine_.locateStep(engine_.stepAtOrAfter(sc.measures[mi].start)); break; }
                }
                lastCursorStep_ = -1; autoNextAt_ = 0;
                if (old) retired_.store(old, std::memory_order_release);
                forceFullPush_ = true;
                lightsDirty_ = true;
                if (barsLit_) { barsLit_ = false; lights_.clearLayer(1); }
                wake();
            }
        }
        // 2. settings and triggers
        readSettings();
        if (trig_restart_.exchange(false))
        {
            if (listening_) { stopListen(); setParamFromEngine(gen::P_LISTEN, 0.0f); }
            engine_.restart(); accompAllOff(); resetParam(gen::P_RESTART); setParamFromEngine(gen::P_PLAY, 0.0f);
        }
        if (listenStopReq_.exchange(false) && listening_) { stopListen(); setParamFromEngine(gen::P_LISTEN, 0.0f); }
        {   // LISTEN is play/pause (0.5.5): off while it plays pauses it where it is, on again goes on
            bool wantListen = current_ != nullptr && getBool(gen::P_LISTEN);
            if (wantListen && !listening_) startListen();
            else if (wantListen && listenPaused_) resumeListen();
            else if (!wantListen && listening_ && !listenPaused_) pauseListen();
            if (!current_ && getBool(gen::P_LISTEN)) setParamFromEngine(gen::P_LISTEN, 0.0f);
            if (listening_) followLoopWhileListening();
        }
        if (trig_setfrom_.exchange(false)) { setLoopEndHere(gen::P_LOOP_FROM); resetParam(gen::P_LOOP_SET_FROM); }
        if (trig_setto_.exchange(false)) { setLoopEndHere(gen::P_LOOP_TO); resetParam(gen::P_LOOP_SET_TO); }
        if (trig_sndtest_.exchange(false)) { errorSound(0, 1.0f); resetParam(gen::P_SND_TEST); }
        syncTempoParams();
        if (int lr = locateReq_.exchange(0))
        {
            if (current_)
            {
                const Score& sc = current_->score;
                int st; int32_t unit;
                if (lr == 1)
                {
                    int mi = (int)std::lround(get(gen::P_GOTO_BAR)) - sc.measures.front().number;
                    mi = std::max(0, std::min((int)sc.measures.size() - 1, mi));
                    unit = sc.measures[(size_t)mi].start;
                    st = engine_.stepAtOrAfter(unit);
                }
                else
                {
                    st = std::max(0, std::min((int)sc.steps.size() - 1, (int)std::lround(get(gen::P_GOTO_STEP)) - 1));
                    unit = sc.steps.empty() ? 0 : sc.steps[(size_t)st].time;
                }
                if (listening_) listenSeek(unit);
                else locateTo(st);
            }
        }
        followMpc(ctx.transport);
        if (autoNextAt_ && sampleClock_ >= autoNextAt_) { autoNextAt_ = 0; trig_next_.store(true); wake(); }
        if (trig_colreset_.exchange(false))
        {
            for (int i = 0; i < 12; ++i) setParamFromEngine(gen::P_COL_C + i, gen::kControls[gen::P_COL_C + i].def);
            resetParam(gen::P_COL_RESET);
        }
        bool wantPlay = get(gen::P_PLAY) >= 0.5f;
        if (settings_.startMode == 2 && ctx.transport.valid)
        {
            if (ctx.transport.playing && !lastTransportPlaying_) { wantPlay = true; setParamFromEngine(gen::P_PLAY, 1.0f); }
            if (!ctx.transport.playing && lastTransportPlaying_) { wantPlay = false; setParamFromEngine(gen::P_PLAY, 0.0f); }
        }
        lastTransportPlaying_ = ctx.transport.valid && ctx.transport.playing;
        if (wantPlay && listening_) { stopListen(); setParamFromEngine(gen::P_LISTEN, 0.0f); }   // PLAY takes over
        if (wantPlay != lastPlayParam_)
        {
            engine_.setPlaying(wantPlay, settings_);
            lastPlayParam_ = wantPlay;
            if (!wantPlay) accompAllOff();
        }
        // 3. MIDI in
        Engine::NoteIn notes[256]; int nn = 0;
        for (int i = 0; i < ctx.numMidiEvents && nn < 256; ++i)
        {
            const mpc::MidiEvent& e = ctx.midi[i];
            if (e.isNoteOn())
            {
                if (isEcho(e.note())) continue;
                notes[nn++] = {e.sampleOffset, (uint8_t)e.note(), true};
                lastPlayed_ = e.note();
            }
            else if (e.isNoteOff()) released_[e.note() & 127] = true;
            else if (e.isAllNotesOff()) accompAllOff();
        }
        // 4. engine (what you play while LISTEN plays is not graded)
        engine_.process(settings_, notes, listening_ ? 0 : nn, n, sr_);
        syncHeaderFields();
        double bpmNow = engine_.effectiveBpm(settings_);
        for (int i = 0; i < engine_.numEvents(); ++i) handleEvent(engine_.event(i), bpmNow);
        // keep the Play button in step with the engine (a first note starts it, the end stops it)
        bool engPlaying = engine_.playing();
        if (engPlaying != lastPlayParam_ && settings_.mode != 0)
        {
            setParamFromEngine(gen::P_PLAY, engPlaying ? 1.0f : 0.0f);
            lastPlayParam_ = engPlaying;
        }
        accompOffsDue();
        listenBlock(n);
        // 5. LinnStrument
        lightsBlock(n);
        // 6. screen
        if (flashBlocks_ > 0 && --flashBlocks_ == 0) flashFrame_ = 0;
        buildDisplay();
        pushDisplay();
        if (needUpdateDisplay_.load() && blockCounter_ - lastUpdateDisplayBlock_ > 30)
        {   // at most ~10 a second: every UpdateDisplay rebuilds the MPC's page
            needUpdateDisplay_.store(false);
            lastUpdateDisplayBlock_ = blockCounter_;
            requestDisplayUpdate();
        }
        for (int i = 0; i < gen::kNumControls; ++i)
            if (needNotify_[i].load(std::memory_order_relaxed)) { needNotify_[i].store(false); notifyHostParameterChanged(i); }
        // 7. sound
        if (ctx.numOutputs >= 2)
            synth_.render(ctx.outputs[0], ctx.outputs[1], n, sr_, (float)(get(gen::P_BLIP_VOL) / 100.0) * 0.5f);
    }

private:
    // ---- 0.5.0: moving around the piece ------------------------------------------------------------
    void locateTo(int step)
    {
        if (!current_) return;
        if (listening_)
        {   // the MPC's playhead moved while LISTEN is on: what you hear moves instead
            const Score& sc = current_->score;
            if (!sc.steps.empty()) listenSeek(sc.steps[(size_t)std::max(0, std::min((int)sc.steps.size() - 1, step))].time);
            return;
        }
        accompAllOff();
        engine_.locateStep(step);
        lastCursorStep_ = -1;
        autoNextAt_ = 0;
    }
    // With Start = MPC Play the MPC's transport drives practice: Play starts it, Stop pauses it,
    // and while it is stopped, moving the MPC's playhead (the locate and step buttons, Stop
    // pressed twice for the start) moves the practice position to the same bar and beat.
    void followMpc(const mpc::Transport& t)
    {
        if (settings_.startMode != 2 || !t.valid || !t.ppqValid || !current_) { lastPpq_ = -1; stoppedBlocks_ = 0; return; }
        if (t.playing) { stoppedBlocks_ = 0; lastPpq_ = t.ppqPos; return; }
        if (stoppedBlocks_ < 1000000) ++stoppedBlocks_;
        if (stoppedBlocks_ > 2 && lastPpq_ >= 0 && std::fabs(t.ppqPos - lastPpq_) > 1e-3)
        {
            const Score& sc = current_->score;
            double qpb = t.timeSigNumerator * 4.0 / std::max(1, t.timeSigDenominator);
            double ppq = std::max(0.0, t.ppqPos);
            int bar = (int)std::floor(ppq / qpb + 1e-9);
            double frac = std::max(0.0, std::min(0.999, (ppq - bar * qpb) / qpb));
            int mi = std::max(0, std::min((int)sc.measures.size() - 1, bar));
            const Measure& m = sc.measures[(size_t)mi];
            locateTo(engine_.stepAtOrAfter(m.start + (int32_t)(frac * m.len)));
        }
        lastPpq_ = t.ppqPos;
    }
    // The header's Bar and Note fields follow the cursor, so the data wheel goes on from there.
    void syncHeaderFields()
    {
        if (!current_) return;
        int cs = engine_.currentStep();
        if (listening_) cs = engine_.stepAtOrAfter((int32_t)listen_.position());
        if (cs == lastCursorStep_) return;
        lastCursorStep_ = cs;
        const Score& sc = current_->score;
        if (sc.steps.empty()) return;
        int si = std::max(0, std::min((int)sc.steps.size() - 1, cs));
        if ((int)std::lround(get(gen::P_GOTO_STEP)) != si + 1) setTracked(gen::P_GOTO_STEP, (float)(si + 1));
        int bar = sc.measures[sc.steps[(size_t)si].measure].number;
        if ((int)std::lround(get(gen::P_GOTO_BAR)) != bar) setTracked(gen::P_GOTO_BAR, (float)bar);
    }
    // Tempo % and BPM are two views of one tempo: the one turned last decides, the other follows.
    void syncTempoParams()
    {
        if (!current_) return;
        double fileBpm = std::max(1.0, current_->score.bpm);
        if (bpmAuthority_.load())
        {
            int pct = (int)std::lround(get(gen::P_BPM) / fileBpm * 100.0);
            pct = std::max(20, std::min(200, pct));
            if (pct != (int)std::lround(get(gen::P_TEMPO))) setTracked(gen::P_TEMPO, (float)pct);
        }
        else
        {
            int bpm = (int)std::lround(fileBpm * get(gen::P_TEMPO) / 100.0);
            bpm = std::max(20, std::min(300, bpm));
            if (bpm != (int)std::lround(get(gen::P_BPM))) setTracked(gen::P_BPM, (float)bpm);
        }
    }

    // ---- settings -------------------------------------------------------------------------------
    std::vector<float> preset(int mode, int tempo, int hands, int timing, int pitchTol, int chordTol, int hold)
    {
        std::vector<float> v(gen::kNumControls);
        for (int i = 0; i < gen::kNumControls; ++i) v[(size_t)i] = gen::kControls[i].def;
        v[gen::P_MODE] = (float)mode; v[gen::P_TEMPO] = (float)tempo; v[gen::P_HANDS] = (float)hands;
        v[gen::P_TIMING] = (float)timing; v[gen::P_PITCH_TOL] = (float)pitchTol; v[gen::P_CHORD_TOL] = (float)chordTol; v[gen::P_HOLD] = (float)hold;
        v.resize((size_t)gen::P_GOAL + 1);   // presets touch practice settings only
        return v;
    }

    void readSettings()
    {
        EngineSettings& s = settings_;
        s.mode = (int)get(gen::P_MODE);
        double fileBpm = current_ ? std::max(1.0, current_->score.bpm) : 100.0;
        s.tempoFactor = bpmAuthority_.load() ? get(gen::P_BPM) / fileBpm : get(gen::P_TEMPO) / 100.0;
        s.hands = (int)get(gen::P_HANDS);
        s.view = (int)get(gen::P_VIEW);
        s.loop = getBool(gen::P_LOOP);
        s.loopFrom = (int)get(gen::P_LOOP_FROM);
        s.loopBars = (int)get(gen::P_LOOP_BARS);
        s.timingMs = get(gen::P_TIMING);
        s.pitchTol = (int)get(gen::P_PITCH_TOL);
        s.chordTol = (int)get(gen::P_CHORD_TOL);
        static const double holds[6] = {0.5, 1, 2, 4, 8, -1};
        s.holdBeats = holds[std::max(0, std::min(5, (int)get(gen::P_HOLD)))];
        s.wrong = (int)get(gen::P_WRONG);
        s.countInBars = (int)get(gen::P_COUNT_IN);
        s.metro = (int)get(gen::P_METRO);
        static const int remind[4] = {0, 5, 10, 20};
        s.remindEvery = remind[std::max(0, std::min(3, (int)get(gen::P_REMIND)))];
        s.remindMax = (int)get(gen::P_REMIND_MAX);
        s.rangeLo = (int)get(gen::P_RANGE_LO);
        s.rangeHi = (int)get(gen::P_RANGE_HI);
        s.outRange = (int)get(gen::P_OUT_RANGE) == 0 ? 0 : 1;
        s.linnSkip = getBool(gen::P_LINN) && (int)get(gen::P_LINN_FIT) == 1;
        if (s.linnSkip) linnRange(s.linnLo, s.linnHi);
        s.mask = (int)get(gen::P_MASK);
        s.startMode = (int)get(gen::P_START_MODE);
        static const int ramps[4] = {0, 2, 5, 10};
        s.ramp = ramps[std::max(0, std::min(3, (int)get(gen::P_RAMP)))];
        s.goal = (int)get(gen::P_GOAL);
        bool loopChanged = s.loop != lastLoop_ || (s.loop && (s.loopFrom != lastLoopFrom_ || s.loopBars != lastLoopBars_));
        if (loopChanged)
        {
            lastLoop_ = s.loop; lastLoopFrom_ = s.loopFrom; lastLoopBars_ = s.loopBars;
            engine_.restart();
            if (barsLit_) trig_section_.store(true);   // relight the loop's notes
        }
        if (s.hands != lastHands_ || s.mode != lastMode_)
        {
            lastHands_ = s.hands; lastMode_ = s.mode;
            engine_.restart(); lightsDirty_ = true;
        }
    }

    void resetParam(int index) { setParamFromEngine(index, 0.0f); }
    void setParamFromEngine(int index, float plain)
    {
        inParamChange_ = true;
        set(index, plain);
        inParamChange_ = false;
        if (index == gen::P_PLAY) lastPlayParam_ = plain >= 0.5f;
        notifyHostParameterChanged(index);
    }

    // ---- engine events ----------------------------------------------------------------------------
    void handleEvent(const EngineEvent& ev, double bpm)
    {
        (void)bpm;
        switch (ev.type)
        {
            case EvType::Wrong: case EvType::Miss:
                if (getBool(gen::P_FLASH)) { flashFrame_ = 1; flashBlocks_ = (int)(0.22 * sr_ / 128); }
                if (getBool(gen::P_ERR_BLIP)) errorSound(ev.type == EvType::Wrong ? 0 : -3, 1.0f);
                if (ev.type == EvType::Wrong) hintOnWrong(ev.a, ev.b);
                break;
            case EvType::Hit: hintRun_ = 0; hintKind_ = 0; break;
            case EvType::Reminder: if (getBool(gen::P_ERR_BLIP)) errorSound(0, (float)(get(gen::P_SND_REMIND) / 100.0)); break;
            case EvType::Accent: click(true); break;
            case EvType::Beat: case EvType::CountIn: click(false); break;
            case EvType::PassDone: case EvType::PieceDone:
            {
                if (ev.type == EvType::PassDone && ev.a >= settings_.goal) { flashFrame_ = 2; flashBlocks_ = (int)(0.4 * sr_ / 128); }
                if (ev.type == EvType::PieceDone)
                {
                    setParamFromEngine(gen::P_PLAY, 0.0f);
                    if (getBool(gen::P_AUTO_NEXT) && !settings_.loop) autoNextAt_ = sampleClock_ + (uint64_t)(2.0 * sr_);
                    if (getBool(gen::P_ERR_BLIP))
                    {   // a soft major third
                        Synth::Env e; e.a = 0.003; e.d = 0.9; e.s = 0; e.r = 0.2;
                        float a = (float)(get(gen::P_SND_VOL) / 100.0) * 0.6f;
                        synth_.trigger(Synth::Bell, 1046.5, a, e); synth_.trigger(Synth::Bell, 1318.5, a * 0.8f, e);
                    }
                }
                if (ev.type == EvType::PassDone && (int)get(gen::P_LOOP) == 0) break;   // the PieceDone right after records it
                recordPass(ev.type == EvType::PieceDone);
                break;
            }
            case EvType::Ramp:
            {
                bpmAuthority_.store(false);
                double t = get(gen::P_TEMPO) + settings_.ramp;
                if (t > 200) t = 200;
                setParamFromEngine(gen::P_TEMPO, (float)t);
                break;
            }
            case EvType::AccompOn:
            {
                // Other Hand: the notes of the hand you are not practising; All: also notes left
                // out for being out of range
                const int mode = (int)get(gen::P_ACCOMP);
                if (mode == 0 || !current_ || listening_) break;
                const PlayNote& pn = current_->score.notes[(size_t)ev.b];
                const bool otherHand = (settings_.hands == 1 && pn.hand != 0) || (settings_.hands == 2 && pn.hand != 1);
                if (mode == 2 || otherHand) sendAccomp(ev.a, ev.b);
                break;
            }
            default: break;
        }
    }

    // Hints for the two usual ways to get stuck: the other hand's note is still wanted (a file
    // for both hands with Hands = Both), or the instrument plays an octave away from the music.
    // Two wrong notes in a row of the same kind post a message (written by the worker).
    void hintOnWrong(int pitch, int step)
    {
        if (!current_ || step < 0) return;
        int kind = 0, expected = -1, rhNote = -1, lhNote = -1, octaveOf = -1;
        bool rhOpen = false, lhOpen = false;
        engine_.forEachRequired(step, [&](const PlayNote& pn, uint8_t st) {
            if (st == 1) return;
            if (pn.hand == 0) { rhOpen = true; if (rhNote < 0 || pn.pitch > rhNote) rhNote = pn.pitch; }
            else { lhOpen = true; if (lhNote < 0 || pn.pitch < lhNote) lhNote = pn.pitch; }
            if (pn.pitch % 12 == pitch % 12 && pn.pitch != pitch && octaveOf < 0) octaveOf = pn.pitch;
        });
        if (lhOpen && !rhOpen && pitch >= 60) { kind = 2; expected = lhNote; }        // the right hand is going on alone
        else if (rhOpen && !lhOpen && pitch < 60) { kind = 3; expected = rhNote; }    // the left hand is
        else if (octaveOf >= 0) { kind = 1; expected = octaveOf; }
        if (kind && kind == hintKind_) ++hintRun_;
        else { hintKind_ = kind; hintRun_ = kind ? 1 : 0; }
        if (kind && hintRun_ == 2) { hintCode_.store(kind | (expected << 8) | (pitch << 16)); wake(); }
    }

    // The wrong-note sound with the SOUND page's settings, shifted by some semitones.
    void errorSound(int semitones, float scale)
    {
        Synth::Env e;
        e.a = get(gen::P_SND_ATTACK) / 1000.0; e.d = get(gen::P_SND_DECAY) / 1000.0;
        e.s = get(gen::P_SND_SUSTAIN) / 100.0; e.r = get(gen::P_SND_RELEASE) / 1000.0;
        e.len = get(gen::P_SND_LENGTH) / 1000.0;
        int type = std::max(0, std::min(5, (int)get(gen::P_SND_TYPE)));
        double hz = Synth::kBaseHz[type] * std::pow(2.0, (get(gen::P_SND_PITCH) + semitones) / 12.0);
        synth_.trigger(type, hz, (float)(get(gen::P_SND_VOL) / 100.0) * scale, e);
    }
    // Metronome and count-in: Tick (filtered noise), Wood or Beep; the downbeat a fifth higher.
    void click(bool accent)
    {
        Synth::Env e; e.a = 0.0005; e.s = 0; e.r = 0.01;
        int ct = (int)get(gen::P_CLICK_TYPE);
        int type = ct == 0 ? Synth::Tick : (ct == 1 ? Synth::Wood : Synth::Sine);
        double hz = ct == 0 ? 3200.0 : (ct == 1 ? 1600.0 : 1000.0);
        e.d = ct == 0 ? 0.025 : (ct == 1 ? 0.05 : 0.04);
        if (accent) hz *= 1.5;
        synth_.trigger(type, hz, (float)(get(gen::P_CLICK_VOL) / 100.0) * (accent ? 1.0f : 0.7f), e);
    }

    void recordPass(bool finished)
    {
        if (!current_) return;
        PassRecord pr; pr.accuracy = engine_.accuracyPercent(); pr.seconds = engine_.activeSeconds();
        pr.tempo = (int)get(gen::P_TEMPO); pr.finished = finished;
        const Score& sc = current_->score;
        int nm = (int)sc.measures.size();
        int hit[16] = {0}, tot[16] = {0};
        for (size_t s = 0; s < sc.steps.size(); ++s)
        {
            int sec = nm > 0 ? (int)sc.steps[s].measure * 16 / nm : 0; if (sec > 15) sec = 15;
            const Step& st = sc.steps[s];
            for (uint32_t k = 0; k < st.count; ++k)
            {
                uint32_t ni = sc.stepNotes[st.firstNote + k];
                uint8_t ns = current_->noteState[ni];
                if (ns == 1) { ++hit[sec]; ++tot[sec]; } else if (ns == 2) ++tot[sec];
            }
        }
        for (int i = 0; i < 16; ++i) pr.sections[i] = tot[i] ? (uint8_t)(100 * hit[i] / tot[i]) : 255;
        passQueue_.push(pr);
        wake();
    }

    // ---- accompaniment (other hand) through "MIDI Out" ---------------------------------------------
    struct Sounding { int8_t pitch = -1; uint8_t ch = 0; int32_t endUnit = 0; };
    Sounding sounding_[32];
    struct Sent { int pitch; uint64_t block; };
    Sent recentSent_[32]; int recentIdx_ = 0;

    void sendAccomp(int pitch, int noteIndex)
    {
        if (!alsa_.isOpen() || !current_) return;
        const PlayNote& pn = current_->score.notes[(size_t)noteIndex];
        uint8_t ch = outChannel();
        uint8_t m[3] = {(uint8_t)(0x90 | ch), (uint8_t)pitch, pn.vel ? pn.vel : (uint8_t)80};
        if (alsa_.sendMidi(m))
        {
            for (Sounding& s : sounding_) if (s.pitch < 0) { s.pitch = (int8_t)pitch; s.ch = ch; s.endUnit = pn.end; break; }
            recentSent_[recentIdx_] = {pitch, blockCounter_}; recentIdx_ = (recentIdx_ + 1) % 32;
        }
    }
    void accompOffsDue()
    {
        // the accompaniment's own position: in Wait mode it waits just before the step that wants
        // you, so a note held up to that step keeps sounding until you play it
        double pos = engine_.accompPosition();
        for (Sounding& s : sounding_)
            if (s.pitch >= 0 && pos >= s.endUnit)
            { uint8_t m[3] = {(uint8_t)(0x80 | s.ch), (uint8_t)s.pitch, 0}; alsa_.sendMidi(m); s.pitch = -1; }
    }
    void accompAllOff()
    {
        for (Sounding& s : sounding_) if (s.pitch >= 0) { uint8_t m[3] = {(uint8_t)(0x80 | s.ch), (uint8_t)s.pitch, 0}; alsa_.sendMidi(m); s.pitch = -1; }
    }
    uint8_t outChannel() const { return (uint8_t)std::max(0, std::min(15, (int)get(gen::P_OUT_CH) - 1)); }

    // ---- LISTEN: the piece in time through "MIDI Out", the cursor following ----------------------
    // From the loop's start (round and round while Loop is on), else from the start of the bar the
    // practice cursor is in (the start of the piece once it is done). The practice position stays
    // where it was; PLAY, RESTART, a new piece or LISTEN again stop it.
    double listenFactor() const
    {
        static const double fixed[7] = {0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5};
        int i = std::max(0, std::min(6, (int)get(gen::P_LISTEN_AT)));
        return i == 0 ? settings_.tempoFactor : fixed[i];
    }
    void loopUnits(const Score& sc, int32_t& from, int32_t& to) const
    {
        int m0 = std::max(0, settings_.loopFrom - (sc.measures.empty() ? 1 : sc.measures[0].number));
        m0 = std::min(m0, (int)sc.measures.size() - 1);
        int m1 = std::min((int)sc.measures.size(), m0 + settings_.loopBars);
        from = sc.measures[(size_t)m0].start;
        to = m1 < (int)sc.measures.size() ? sc.measures[(size_t)m1].start : sc.endUnit;
    }
    void startListen()
    {
        const Score& sc = current_->score;
        int32_t from = 0, to = sc.endUnit;
        if (settings_.loop) loopUnits(sc, from, to);
        else if (engine_.state() != RunState::Done && !sc.measures.empty())
            from = sc.measures[(size_t)std::max(0, std::min((int)sc.measures.size() - 1, engine_.currentMeasure()))].start;
        int hands = 3;
        if ((int)get(gen::P_LISTEN_HANDS) == 1) hands = settings_.hands == 1 ? 1 : (settings_.hands == 2 ? 2 : 3);
        if (engine_.playing()) { engine_.setPlaying(false, settings_); setParamFromEngine(gen::P_PLAY, 0.0f); }
        accompAllOff();
        listenCh_ = outChannel();
        listening_ = listen_.start(&sc, from, to, settings_.loop, hands);
        listenPaused_ = false; listenLoop_ = settings_.loop;
        if (!listening_) { setParamFromEngine(gen::P_LISTEN, 0.0f); return; }
        engine_.setListenSlot(sc.slotAtTime(from));
        lightsDirty_ = true;
        int pct = (int)std::lround(listenFactor() * 100.0);
        hintCode_.store(4 | (std::min(255, pct) << 8) | ((listenCh_ + 1) << 16));
        wake();
    }
    void pauseListen()
    {
        listen_.silence([this](int st, int pitch, int vel) { listenSend(st, pitch, vel); });
        listenPaused_ = true;
        lightsDirty_ = true;
        const Score& sc = current_->score;
        int bar = sc.measures[(size_t)sc.measureAt((int32_t)listen_.position())].number;
        hintCode_.store(7 | (std::min(255, bar) << 8) | (std::min(255, bar >> 8) << 16));
        wake();
    }
    void resumeListen() { listenPaused_ = false; lightsDirty_ = true; }
    void listenSeek(int32_t unit)
    {
        listen_.seek(unit, [this](int st, int pitch, int vel) { listenSend(st, pitch, vel); });
        engine_.setListenSlot(current_->score.slotAtTime((int32_t)listen_.position()));
        lightsDirty_ = true;
    }
    // Loop switched on or off, or moved, while LISTEN is on: it plays the new range.
    void followLoopWhileListening()
    {
        const Score& sc = current_->score;
        int32_t from = 0, to = sc.endUnit;
        if (settings_.loop) loopUnits(sc, from, to);
        else from = std::min<int32_t>(listen_.from(), (int32_t)listen_.position());
        if (settings_.loop == listenLoop_ && (!settings_.loop || (from == listen_.from() && to == listen_.to()))) return;
        listenLoop_ = settings_.loop;
        listen_.setRange(from, to, settings_.loop, [this](int st, int pitch, int vel) { listenSend(st, pitch, vel); });
        engine_.setListenSlot(sc.slotAtTime((int32_t)listen_.position()));
    }
    // SET (next to Loop From and Loop To): the bar you hear while LISTEN is on, else the bar the
    // cursor is in.
    void setLoopEndHere(int param)
    {
        if (!current_) return;
        const Score& sc = current_->score;
        int mi = listening_ ? sc.measureAt((int32_t)listen_.position()) : engine_.currentMeasure();
        mi = std::max(0, std::min((int)sc.measures.size() - 1, mi));
        setTracked(param, (float)sc.measures[(size_t)mi].number);
        loopRule(param);
    }
    void stopListen()
    {
        listen_.stop([this](int st, int pitch, int vel) { listenSend(st, pitch, vel); });
        listening_ = false;
        listenPaused_ = false;
        engine_.setListenSlot(-1);
        lightsDirty_ = true;
    }
    void listenSend(int st, int pitch, int vel)
    {
        uint8_t m[3] = {(uint8_t)(st | listenCh_), (uint8_t)pitch, (uint8_t)vel};
        alsa_.sendMidi(m);
        if (st == 0x90) { recentSent_[recentIdx_] = {pitch, blockCounter_}; recentIdx_ = (recentIdx_ + 1) % 32; }
    }
    void listenBlock(int frames)
    {
        if (!listening_ || listenPaused_ || !current_) return;
        bool more = listen_.process(frames, sr_, listenFactor(), [this](int st, int pitch, int vel) { listenSend(st, pitch, vel); });
        if (!more) { stopListen(); setParamFromEngine(gen::P_LISTEN, 0.0f); return; }
        engine_.setListenSlot(current_->score.slotAtTime(listen_.position()));
    }

    bool isEcho(int pitch) const
    {   // our own accompaniment can come back if a track routes MIDI Out into this one
        for (const Sent& s : recentSent_) if (s.pitch == pitch && blockCounter_ - s.block < 40) return true;
        return false;
    }

    // ---- LinnStrument ------------------------------------------------------------------------------
    LinnLayout layoutFromParams() const
    {
        LinnLayout L;
        L.cols = (int)get(gen::P_LINN_SIZE) == 1 ? 16 : 25;
        static const int offs[6] = {5, 4, 3, 6, 7, 12};
        L.rowOffset = offs[std::max(0, std::min(5, (int)get(gen::P_LINN_ROWOFF)))];
        // the instrument's Octave and Transpose settings move every note by the same amount
        L.lowest = (int)get(gen::P_LINN_LOW) + 12 * (int)std::lround(get(gen::P_LINN_OCT)) + (int)std::lround(get(gen::P_LINN_SEMI));
        L.leftHanded = getBool(gen::P_LINN_LEFT);
        return L;
    }
    // The notes the surface can play: bottom-left cell to top-right cell.
    void linnRange(int& lo, int& hi) const
    {
        LinnLayout L = layoutFromParams();
        lo = std::max(0, L.lowest);
        hi = std::min(127, L.lowest + (L.rows - 1) * L.rowOffset + (L.cols - 1));
    }

    void guideAllOff()
    {
        for (int i = 0; i < litCount_; ++i)
        {
            uint8_t m[3] = {(uint8_t)(0x80 | (linnCh_ & 15)), (uint8_t)lit_[i], 0};
            alsa_.sendLinn(m);
            rememberOff(lit_[i]);
        }
        litCount_ = 0;
        lights_.clearLayer(2);
    }

    // Cell paints, paced for the route: the LinnStrument's USB side reaches its processor through
    // a 115200-baud serial link (about 3,800 messages a second), a DIN port runs at 31250 baud
    // (about 1,000). Up to 3,000 messages a second by USB, 900 by DIN.
    void flushLights(int frames)
    {
        // a budget of messages that refills with time, whatever the block size. 0.5.6: gentler
        // (the instrument reads one byte per pass of its main loop into a small buffer) and in
        // small bursts, and nothing for a moment after a request that makes it redraw everything
        const bool usb = (int)get(gen::P_LINN_PORT) == 0;
        const double rate = usb ? 1500.0 : 900.0, cap = 6.0;
        linnBudget_ = std::min(cap, linnBudget_ + frames * rate / std::max(1.0, sr_));
        if (!alsa_.linnConnected() || linnBudget_ < 3.0 || sampleClock_ < linnQuietUntil_) return;
        // Restore = Pattern n: cells to give back are restored by having the instrument reload its
        // stored custom pattern (NRPN 247 "active note lights preset" = 8 + n, ls_midi.ino), then
        // what is still lit is painted again, after a pause while it redraws. At most every
        // 150 ms: each reload redraws the whole surface.
        const int restore = (int)get(gen::P_LINN_RESTORE);
        if (restore >= 2 && lights_.reloadWanted() && sampleClock_ >= nextReloadAt_ && linnBudget_ >= 6.0)
        {
            const uint8_t ch = (uint8_t)(0xB0 | (((int)get(gen::P_LINN_CH) - 1) & 15));
            const int value = 8 + (restore - 1);                 // Pattern 1 -> 9 (custom pattern 0)
            // NRPN 247 = value, then the null NRPN so a later CC 6 is not taken as data
            const uint8_t msgs[6][3] = {{ch, 99, 247 >> 7}, {ch, 98, 247 & 127}, {ch, 6, 0}, {ch, 38, (uint8_t)value},
                                        {ch, 99, 127}, {ch, 98, 127}};
            for (const auto& m : msgs) alsa_.sendLinn(m);
            linnBudget_ -= 6.0;
            lights_.afterReload();
            nextReloadAt_ = sampleClock_ + (uint64_t)(0.15 * sr_);
            linnQuietUntil_ = sampleClock_ + (uint64_t)(0.02 * sr_);
            return;
        }
        int sent = lights_.flush((int)linnBudget_, [this](const uint8_t* m) { alsa_.sendLinn(m); });
        linnBudget_ -= sent;
    }

    // Cells the plugin has lit go back to these when nothing else lights them: the note colours
    // of the COLOR page in the LinnStrument's nearest colours (Restore = Note Colors), or off.
    void updateBaseColours(const LinnLayout& L)
    {
        // palette index (geometry.py PALETTE) -> LinnStrument colour; 0 = nothing
        static const uint8_t kPalToLinn[18] = {0, kLinnRed, kLinnYellow, kLinnGreen, kLinnCyan, kLinnBlue, kLinnMagenta, kLinnWhite,
                                               kLinnOrange, kLinnLime, kLinnPink, kLinnCyan, kLinnMagenta, kLinnPink, kLinnWhite,
                                               kLinnYellow, kLinnBlue, kLinnPink};
        const int restore = (int)get(gen::P_LINN_RESTORE);
        uint32_t sig = 2166136261u;
        auto mix = [&sig](int v) { sig = (sig ^ (uint32_t)(v + 7)) * 16777619u; };
        mix(restore); mix(L.cols); mix(L.rowOffset); mix(L.lowest); mix(L.leftHanded ? 1 : 0);
        for (int i = 0; i < 12; ++i) mix((int)get(gen::P_COL_C + i));
        if (sig == baseSig_) return;
        baseSig_ = sig;
        lights_.setIdle(kLinnClear);
        lights_.setReloadMode(restore >= 2);
        for (int r = 0; r < LinnLights::kRows; ++r)
            for (int c = 1; c < LinnLights::kCols; ++c)
            {
                uint8_t colour = 0;
                if (restore == 1 && c <= L.cols && r < L.rows)   // Note Colors
                {
                    int pal = (int)get(gen::P_COL_C + ((L.note(c, r) % 12) + 12) % 12);
                    colour = kPalToLinn[std::max(0, std::min(17, pal))];
                }
                lights_.setBase(c, r, colour);
            }
    }

    // TEST LIGHTS: a green diagonal (cell paints) and C4 (a Note On on Linn Ch, the played-colour
    // highlight) for 2.5 s, whether or not Linn Mode is on.
    void testLightsBlock(const LinnLayout& L)
    {
        if (trig_linntest_.load() && alsa_.linnConnected())
        {
            trig_linntest_.store(false);
            resetParam(gen::P_LINN_TEST);
            lights_.clearLayer(3);
            for (int c = 1; c <= L.cols; ++c) lights_.setCell(3, c, (c - 1) % L.rows, kLinnGreen);
            int ch = (int)get(gen::P_LINN_CH) - 1;
            uint8_t m[3] = {(uint8_t)(0x90 | (ch & 15)), 60, 100};
            alsa_.sendLinn(m);
            testCh_ = ch;
            testUntil_ = sampleClock_ + (uint64_t)(2.5 * sr_) + 1;
            hintCode_.store(5 | ((int)get(gen::P_LINN_PORT) << 8) | ((ch + 1) << 16));
        }
        else if (trig_linntest_.load())
        {   // the worker looks for the port first; give it a second
            if (!testWaitUntil_) testWaitUntil_ = sampleClock_ + (uint64_t)(1.0 * sr_) + 1;
            else if (sampleClock_ >= testWaitUntil_) { trig_linntest_.store(false); testWaitUntil_ = 0; resetParam(gen::P_LINN_TEST); hintCode_.store(6); }
        }
        if (!trig_linntest_.load()) testWaitUntil_ = 0;
        if (testUntil_ && sampleClock_ >= testUntil_)
        {
            testUntil_ = 0;
            lights_.clearLayer(3);
            bool guideHas60 = false;
            for (int i = 0; i < litCount_; ++i) guideHas60 |= lit_[i] == 60 && linnCh_ == testCh_;
            if (!guideHas60) { uint8_t m[3] = {(uint8_t)(0x80 | (testCh_ & 15)), 60, 0}; alsa_.sendLinn(m); }
        }
    }

    void lightsBlock(int frames)
    {
        bool linnOn = getBool(gen::P_LINN);
        bool connected = alsa_.linnConnected();
        const int linnGen = alsa_.linnGeneration();
        if (connected && (!wasConnected_ || linnGen != linnGenSeen_)) { lights_.invalidate(); lights_.resetCursor(); litCount_ = 0; lightsDirty_ = true; }
        wasConnected_ = connected; linnGenSeen_ = linnGen;
        LinnLayout L = layoutFromParams();
        if (lightsRepaint_.exchange(false)) lightsDirty_ = true;
        if (linnAsk_.exchange(false) && connected)
        {   // NRPN 299 "send me the value of": 36 Octave, 37 Transpose (left split); then the null NRPN
            const uint8_t ch = (uint8_t)(0xB0 | (((int)get(gen::P_LINN_CH) - 1) & 15));
            for (int prm : {36, 37})
            {
                const uint8_t q[4][3] = {{ch, 99, 299 >> 7}, {ch, 98, 299 & 127}, {ch, 6, 0}, {ch, 38, (uint8_t)prm}};
                for (const auto& m : q) alsa_.sendLinn(m);
            }
            const uint8_t z[2][3] = {{ch, 99, 127}, {ch, 98, 127}};
            for (const auto& m : z) alsa_.sendLinn(m);
            linnQuietUntil_ = sampleClock_ + (uint64_t)(0.02 * sr_);
        }
        updateBaseColours(L);
        testLightsBlock(L);
        if (!linnOn)
        {
            if (linnWasOn_) { guideAllOff(); lights_.clearLayer(0); lights_.clearLayer(1); barsLit_ = false; linnWasOn_ = false; }
            flushLights(frames);
            trig_section_.store(false); trig_clear_.store(false);
            return;
        }
        linnWasOn_ = true;
        if (trig_clear_.exchange(false))
        {
            guideAllOff(); lights_.clearLayer(0); lights_.clearLayer(1); barsLit_ = false;
            resetParam(gen::P_LINN_CLEAR);
            keyLightsShown_ = -1;
        }
        if (trig_section_.exchange(false))
        {
            resetParam(gen::P_LINN_SECT);
            lights_.clearLayer(1);
            if (barsLit_ && !settings_.loop && sectionWasManual_) { barsLit_ = false; sectionWasManual_ = false; }
            else if (current_)
            {
                // the loop if one is set, else the bars on screen
                const Score& sc = current_->score;
                int32_t from = 0, to = sc.endUnit;
                if (settings_.loop)
                {
                    int m0 = std::max(0, settings_.loopFrom - (sc.measures.empty() ? 1 : sc.measures[0].number));
                    m0 = std::min(m0, (int)sc.measures.size() - 1);
                    int m1 = std::min((int)sc.measures.size(), m0 + settings_.loopBars);
                    from = sc.measures[(size_t)m0].start;
                    to = m1 < (int)sc.measures.size() ? sc.measures[(size_t)m1].start : sc.endUnit;
                }
                else
                {
                    int lo = 1 << 30, hi = -1;
                    for (int i = 0; i < gen::kSlots; ++i) if (screenSlot_[i] >= 0) { lo = std::min(lo, screenSlot_[i]); hi = std::max(hi, screenSlot_[i]); }
                    if (hi >= 0) { from = sc.slots[(size_t)lo].time; to = hi + 1 < (int)sc.slots.size() ? sc.slots[(size_t)hi + 1].time : sc.endUnit; }
                }
                uint8_t colour = kLinnColourChoice[std::max(0, std::min(9, (int)get(gen::P_LINN_SCOL)))];
                for (const PlayNote& pn : sc.notes)
                    if (pn.start >= from && pn.start < to && engine_.required(pn)) lights_.setNote(1, L, pn.pitch, colour);
                barsLit_ = true; sectionWasManual_ = !settings_.loop;
            }
        }
        // key lights
        int keyMode = (int)get(gen::P_LINN_KEY);
        int keySig = keyMode * 1000 + (int)get(gen::P_LINN_KCOL) * 10 + keyRevision_.load();
        if (keySig != keyLightsShown_ || lightsDirty_)
        {
            keyLightsShown_ = keySig;
            lights_.clearLayer(0);
            int root = -1; uint16_t mask = 0;
            if (keyMode == 1 || keyMode == 2) { root = keyRoot_[keyMode - 1].load(); mask = (uint16_t)keyMask_[keyMode - 1].load(); }
            else if (keyMode == 3 && current_) { root = keyTonicPc(current_->score.keySf, current_->score.minor); mask = current_->score.minor ? 0x5AD : 0xAB5; }
            if (root >= 0 && mask)
            {
                uint8_t colour = kLinnColourChoice[std::max(0, std::min(9, (int)get(gen::P_LINN_KCOL)))];
                for (int i = 0; i < 12; ++i)
                    if (mask & (1 << i)) lights_.setPitchClass(0, L, (root + i) % 12, i == 0 ? (uint8_t)kLinnWhite : colour);
            }
        }
        // guide: the next step's notes (and the one after)
        int guideMode = (int)get(gen::P_LINN_GUIDE);
        int gcol = (int)get(gen::P_LINN_GCOL), gcol2 = (int)get(gen::P_LINN_GCOL2);
        bool fingering = (int)get(gen::P_LINN_FINGER) == 1 || guideMode == 3;
        linnCh_ = (int)get(gen::P_LINN_CH) - 1;
        int want[16]; int wantCount = 0;
        uint8_t wantColour[16];
        int8_t wantCol[16], wantRow[16];
        if (guideMode > 0 && current_ && listening_)
        {   // LISTEN: light the notes as they sound
            listen_.forEachSounding([&](const PlayNote& pn, int32_t ni) {
                if (wantCount >= 16) return;
                want[wantCount] = pn.pitch;
                wantCol[wantCount] = (size_t)ni < current_->fing.col.size() ? current_->fing.col[(size_t)ni] : -1;
                wantRow[wantCount] = (size_t)ni < current_->fing.row.size() ? current_->fing.row[(size_t)ni] : -1;
                int f = (size_t)ni < current_->fing.finger.size() ? current_->fing.finger[(size_t)ni] : 0;
                wantColour[wantCount] = (fingering && f > 0) ? kFingerColour[f] : (gcol == 0 ? 0 : kLinnColourChoice[gcol - 1]);
                ++wantCount;
            });
        }
        else if (guideMode > 0 && current_ && engine_.state() != RunState::Done)
        {
            // the next notes still to press (0.5.5): a step whose notes you have all played, ahead
            // of the clock in Rhythm or Flow, is passed over, so the guide always shows what to
            // press next
            auto allPlayed = [this](int st) {
                bool open = false;
                engine_.forEachRequired(st, [&open](const PlayNote&, uint8_t state) { if (state != 1) open = true; });
                return !open;
            };
            int s = engine_.nextRequiredStep(std::max(0, engine_.currentStep()));
            while (s >= 0 && allPlayed(s)) s = engine_.nextRequiredStep(s + 1);
            for (int pass = 0; pass < (guideMode == 2 ? 2 : 1) && s >= 0; ++pass)
            {
                engine_.forEachRequired(s, [&](const PlayNote& pn, uint8_t state) {
                    if (state == 1 || wantCount >= 16) return;
                    size_t ni = (size_t)(&pn - &current_->score.notes[0]);
                    want[wantCount] = pn.pitch;
                    wantCol[wantCount] = ni < current_->fing.col.size() ? current_->fing.col[ni] : -1;
                    wantRow[wantCount] = ni < current_->fing.row.size() ? current_->fing.row[ni] : -1;
                    int f = ni < current_->fing.finger.size() ? current_->fing.finger[ni] : 0;
                    // the next step in Guide Clr, the one after in After Clr (0.5.5). Played (0) is the
                    // instrument's own highlight, kept apart from your light pattern; a colour
                    // paints the cells
                    const int c = pass == 0 ? gcol : gcol2;
                    wantColour[wantCount] = (fingering && f > 0) ? kFingerColour[f] : (c == 0 ? 0 : kLinnColourChoice[c - 1]);
                    ++wantCount;
                });
                s = engine_.nextRequiredStep(s + 1);
                while (s >= 0 && allPlayed(s)) s = engine_.nextRequiredStep(s + 1);
            }
        }
        // A highlighted note you have just released: the instrument cleared the cell under your
        // finger, so it is lit again if it is still (or again) the note to press.
        bool relight = false;
        for (int i = 0; i < litCount_ && !relight; ++i) relight = released_[lit_[i] & 127];
        std::memset(released_, 0, sizeof(released_));
        // Did the guide change?
        uint32_t sig = 2166136261u;
        for (int i = 0; i < wantCount; ++i) sig = (sig ^ (uint32_t)(want[i] * 31 + wantColour[i])) * 16777619u;
        sig ^= (uint32_t)guideMode * 7 + (fingering ? 1 : 0) + (uint32_t)gcol2 * 131;
        if (sig != guideSig_ || lightsDirty_ || relight)
        {
            guideSig_ = sig;
            // Note On highlights for the notes without a colour; cell paints for the others
            int newLit[16]; int newCount = 0;
            for (int i = 0; i < wantCount; ++i)
                if (wantColour[i] == 0)
                {
                    bool dup = false; for (int k = 0; k < newCount; ++k) dup |= newLit[k] == want[i];
                    if (!dup) newLit[newCount++] = want[i];
                }
            for (int i = 0; i < litCount_; ++i)
            {
                bool keep = false; for (int k = 0; k < newCount; ++k) keep |= newLit[k] == lit_[i];
                if (!keep) { uint8_t m[3] = {(uint8_t)(0x80 | (linnCh_ & 15)), (uint8_t)lit_[i], 0}; alsa_.sendLinn(m); rememberOff(lit_[i]); }
            }
            for (int k = 0; k < newCount; ++k)
            {
                bool was = false; for (int i = 0; i < litCount_; ++i) was |= newLit[k] == lit_[i];
                if (!was || relight) { uint8_t m[3] = {(uint8_t)(0x90 | (linnCh_ & 15)), (uint8_t)newLit[k], 100}; alsa_.sendLinn(m); }
            }
            for (int k = 0; k < newCount; ++k) lit_[k] = newLit[k];
            litCount_ = newCount;
            lights_.clearLayer(2);
            // the step after first, then the next step over it: on a cell both want, the next
            // note's colour wins (0.5.6)
            for (int i = wantCount - 1; i >= 0; --i)
            {
                if (wantColour[i] == 0) continue;
                if (fingering && wantCol[i] > 0) lights_.setCell(2, wantCol[i], wantRow[i], wantColour[i]);
                else lights_.setNote(2, L, want[i], wantColour[i]);
            }
            lightsDirty_ = false;
        }
        healLights(connected);
        flushLights(frames);
    }

    // A message the instrument lost is never answered, so what should be lit is sent again now
    // and then (0.5.6): the cell paints every 0.4 s (lit cells, and cells let go of in the last
    // second), the Note On highlights and the Note Offs of the last second every 1.2 s.
    void rememberOff(int pitch)
    {
        for (OffHeal& o : offHeal_) if (o.pitch == pitch) { o.left = 2; return; }
        for (OffHeal& o : offHeal_) if (o.left == 0) { o.pitch = pitch; o.left = 2; return; }
    }
    void healLights(bool connected)
    {
        if (!connected || sampleClock_ < nextHealAt_) return;
        nextHealAt_ = sampleClock_ + (uint64_t)(0.4 * sr_);
        lights_.heal();
        if (++healCount_ % 3 != 0) return;
        for (int i = 0; i < litCount_; ++i)
        { uint8_t m[3] = {(uint8_t)(0x90 | (linnCh_ & 15)), (uint8_t)lit_[i], 100}; alsa_.sendLinn(m); }
        for (OffHeal& o : offHeal_)
            if (o.left > 0)
            {
                --o.left;
                bool lit = false; for (int i = 0; i < litCount_; ++i) lit |= lit_[i] == o.pitch;
                if (!lit) { uint8_t m[3] = {(uint8_t)(0x80 | (linnCh_ & 15)), (uint8_t)o.pitch, 0}; alsa_.sendLinn(m); }
            }
    }

    // ---- display ------------------------------------------------------------------------------------
    void setD(int index, float v) { desired_[index] = v; }
    void setFrame(int index, int frame, int nFrames) { desired_[index] = frameValue(frame, nFrames); }
    void setDigits(int base, int count, int value, bool blankZeros = true)
    {
        // right-aligned digits, frame 0 blank, frame k+1 digit k
        for (int i = 0; i < count; ++i)
        {
            int d = -1;
            if (value >= 0)
            {
                int div = 1; for (int k = 0; k < count - 1 - i; ++k) div *= 10;
                int digit = (value / div) % 10;
                bool leading = blankZeros && value < div && i < count - 1;
                d = leading ? -1 : digit;
            }
            setFrame(base + i, d < 0 ? 0 : d + 1, gen::kDigitFrames);
        }
    }

    void buildDisplay()
    {
        uint8_t ov[gen::kSlots]; int bar[gen::kSlots + 1]; int mnum[2] = {-1, -1};
        for (int i = 0; i < gen::kSlots; ++i) screenSlot_[i] = -1;
        if (current_) engine_.computeScreen(screenSlot_, ov, bar, mnum);
        else { for (int i = 0; i < gen::kSlots; ++i) ov[i] = 0; for (int i = 0; i <= gen::kSlots; ++i) bar[i] = 0; }
        const Score* sc = current_ ? &current_->score : nullptr;
        for (int s = 0; s < gen::kSlots; ++s)
        {
            int k = screenSlot_[s];
            for (int r = 0; r < gen::kRows; ++r)
            {
                int fr = (sc && k >= 0) ? sc->slots[(size_t)k].cells[r] : 0;
                setFrame(gen::kD_cell + s * gen::kRows + r, fr, rowFrames(r));
            }
            setFrame(gen::kD_overlay + s, ov[s], gen::kOverlayFrames);
        }
        buildMarks(sc, ov);
        for (int i = 0; i <= gen::kSlots; ++i) setFrame(gen::kD_barline + i, bar[i], gen::kBarlineFrames);
        setFrame(gen::kD_keysig, sc ? sc->keySf + 7 : 7, gen::kKeySigFrames);
        setFrame(gen::kD_timesig, sc ? sc->timeSigFrame() : 0, gen::kTimeSigFrames);
        // the hand not practised is dimmed (not while LISTEN plays both hands)
        const bool listenBoth = listening_ && (int)get(gen::P_LISTEN_HANDS) == 0;
        setFrame(gen::kD_mask + 0, settings_.hands == 2 && !listenBoth ? 1 : 0, gen::kStaffMaskFrames);
        setFrame(gen::kD_mask + 1, settings_.hands == 1 && !listenBoth ? 1 : 0, gen::kStaffMaskFrames);
        setFrame(gen::kD_flash, flashFrame_, gen::kFlashFrames);
        // status
        int status = 8;   // NO FILE
        if (loading_.load()) status = 7;
        else if (sc)
        {
            switch (engine_.state())
            {
                case RunState::CountIn: status = 5; break;
                case RunState::Paused: status = 4; break;
                case RunState::Done: status = 6; break;
                default: status = 1 + settings_.mode; break;
            }
            if (settings_.loop && engine_.state() != RunState::CountIn && engine_.state() != RunState::Paused) status = 9;
            if (listening_) status = listenPaused_ ? 4 : 10;
        }
        setFrame(gen::kD_status, status, gen::kStatusFrames);
        setFrame(gen::kD_handicon, settings_.hands, gen::kHandFrames);
        int curMeasureIdx = engine_.currentMeasure();
        if (sc && listening_ && engine_.listenSlot() >= 0) curMeasureIdx = sc->slots[(size_t)engine_.listenSlot()].measure;
        int curMeasure = sc ? sc->measures[(size_t)curMeasureIdx].number : -1;
        if (sc && !listening_) curBar_.store(sc->measures[(size_t)engine_.currentMeasure()].number);
        setDigits(gen::kD_d_bar, 3, sc ? std::max(0, curMeasure) : -1);
        setDigits(gen::kD_d_bars, 3, sc ? (int)sc->measures.back().number : -1);
        int hitsTotal = engine_.hits() + engine_.misses() + engine_.wrongs();
        setDigits(gen::kD_d_acc, 3, hitsTotal ? engine_.accuracyPercent() : -1);
        setDigits(gen::kD_d_bpm, 3, sc ? (int)std::lround(engine_.effectiveBpm(settings_)) : -1);
        setDigits(gen::kD_d_err, 3, sc ? engine_.misses() + engine_.wrongs() : -1);
        for (int h = 0; h < 2; ++h)
        {
            setDigits(gen::kD_mnum + h * 3, 3, mnum[h] > 0 ? mnum[h] : -1);
        }
        // next notes (each hand: the top note of the right hand's chord, the bottom of the left's),
        // the note played, fingers
        int names = (int)get(gen::P_NAMES);
        int nextR = -1, nextL = -1, f0 = 0, f1 = 0;
        bool flats = sc && sc->keySf < 0;
        if (sc && names > 0 && listening_)
        {   // LISTEN: the notes sounding now
            listen_.forEachSounding([&](const PlayNote& pn, int32_t) {
                if (pn.hand == 0) { if (nextR < 0 || pn.pitch > nextR) nextR = pn.pitch; }
                else if (nextL < 0 || pn.pitch < nextL) nextL = pn.pitch;
            });
        }
        else if (sc && names > 0)
        {
            int s = engine_.nextRequiredStep(std::max(0, engine_.currentStep()));
            engine_.forEachRequired(s, [&](const PlayNote& pn, uint8_t st) {
                if (st == 1) return;
                if (pn.hand == 0) { if (nextR < 0 || pn.pitch > nextR) nextR = pn.pitch; }
                else if (nextL < 0 || pn.pitch < nextL) nextL = pn.pitch;
                size_t ni = (size_t)(&pn - &sc->notes[0]);
                if (ni < current_->fing.finger.size())
                {
                    int f = current_->fing.finger[ni];
                    if (pn.hand == 0 && !f0) f0 = f;
                    if (pn.hand == 1 && !f1) f1 = f;
                }
            });
        }
        auto nameFrame = [flats](int p) {
            if (p < gen::kNoteNameLow || p > gen::kNoteNameHigh) return 0;
            return 1 + (p - gen::kNoteNameLow) + (flats ? (gen::kNoteNameHigh - gen::kNoteNameLow + 1) : 0);
        };
        setFrame(gen::kD_nextname, nameFrame(nextR), gen::kNoteNameFrames);
        setFrame(gen::kD_nextname_lh, nameFrame(nextL), gen::kNoteNameFrames);
        setFrame(gen::kD_playedname, names == 2 ? nameFrame(lastPlayed_) : 0, gen::kNoteNameFrames);
        // COLOR page swatches
        for (int i = 0; i < 12; ++i) setFrame(gen::kD_swatch + i, (int)get(gen::P_COL_C + i), gen::kSwatchFrames);
        // LOAD: the selected row; PLAY's piece list: the chosen and the loaded piece
        const int rs = rowSel_.load();
        for (int r = 0; r < 8; ++r) setFrame(gen::kD_rowsel + r, r == rs ? 1 : 0, gen::kF_rowsel);
        const uint32_t qb = qSelBits_.load();
        for (int r = 0; r < 8; ++r) setFrame(gen::kD_qsel + r, (int)((qb >> (2 * r)) & 3u), gen::kF_qsel);
        // readouts under the controls: the text for each control's current value
        for (int i = 0; i < gen::kN_ro; ++i)
        {
            int c = gen::kRoControl[i];
            int v = (int)std::lround(get(c) - gen::kControls[c].min);
            setFrame(gen::kD_ro + i, std::max(0, std::min(gen::kRoFrames[i] - 1, v)), gen::kRoFrames[i]);
        }
        bool showFingers = getBool(gen::P_LINN) && (int)get(gen::P_LINN_FINGER) == 1;
        setFrame(gen::kD_finger + 0, showFingers ? f0 : 0, gen::kFingerFrames);
        setFrame(gen::kD_finger + 1, showFingers ? f1 : 0, gen::kFingerFrames);
        // progress through the piece
        double prog = 0;
        if (sc && !sc->steps.empty()) prog = std::max(0.0, std::min(1.0, engine_.position() / std::max(1, sc->endUnit)));
        if (sc && settings_.mode == 0 && !sc->steps.empty()) prog = (double)engine_.currentStep() / sc->steps.size();
        if (sc && listening_) prog = std::max(0.0, std::min(1.0, listen_.position() / std::max(1, sc->endUnit)));
        setD(gen::kD_progress, (float)prog);
        // LinnStrument status
        int ls = !getBool(gen::P_LINN) ? 3 : (alsa_.linnConnected() ? 2 : 1);
        setFrame(gen::kD_linnstate, ls, gen::kLinnStateFrames);
        // STATS page (from the worker's snapshot)
        for (int i = 0; i < 16; ++i)
        {
            int v = statSections_[i].load();
            setFrame(gen::kD_statbar + i, v > 100 ? 0 : (v * (gen::kBarFrames - 1) + 50) / 100, gen::kBarFrames);
        }
        setDigits(gen::kD_d_best, 3, statBest_.load());
        setDigits(gen::kD_d_last, 3, statLast_.load());
        setDigits(gen::kD_d_passes, 3, statPlays_.load());
        setDigits(gen::kD_d_minutes, 4, statMinutes_.load());
        setDigits(gen::kD_d_streak, 3, statStreak_.load());
    }

    // Marks over the noteheads: the note's colour dot (notes still to come), the head turning
    // green (played) or red (missed), and a red ghost head where a wrong note was played.
    void buildMarks(const Score* sc, const uint8_t ov[gen::kSlots])
    {
        uint8_t mk[gen::kRows];
        const bool colours = getBool(gen::P_NOTE_COLORS);
        const int feedback = (int)get(gen::P_FEEDBACK);
        int colourOf[12];
        for (int i = 0; i < 12; ++i) colourOf[i] = std::max(0, std::min(gen::kDotColours, (int)get(gen::P_COL_C + i)));
        // the hand not practised is dimmed: no marks on its staff (LISTEN with both hands: none dimmed)
        const bool listenBoth = listening_ && (int)get(gen::P_LISTEN_HANDS) == 0;
        const int dimStaff = listenBoth ? -1 : (settings_.hands == 1 ? 1 : (settings_.hands == 2 ? 0 : -1));
        // the step just completed keeps its bright marks for a moment, so a note is seen turning green
        int freshSlot = -1;
        if (sc && engine_.lastDoneStep() >= 0 && engine_.lastDoneStep() < (int)sc->steps.size() && engine_.secondsSinceDone() < 0.6)
            freshSlot = sc->steps[(size_t)engine_.lastDoneStep()].slot;
        for (int s = 0; s < gen::kSlots; ++s)
        {
            std::memset(mk, 0, sizeof(mk));
            int k = screenSlot_[s];
            if (sc && k >= 0 && ov[s] != 4)
            {
                const Slot& sl = sc->slots[(size_t)k];
                const bool passed = (ov[s] == 2 || ov[s] == 3) && k != freshSlot;
                const int trail = passed ? 1 : 0;
                for (uint32_t h = 0; h < sl.headCount; ++h)
                {
                    const HeadRef& hr = sc->heads[sl.headFirst + h];
                    if (hr.staff == dimStaff || hr.note < 0) continue;
                    uint8_t st = listening_ ? 0 : current_->noteState[(size_t)hr.note];   // LISTEN: colour dots only
                    int fr = 0;
                    if (feedback >= 1 && st == 1) fr = gen::kMarkTintBase + ((trail * 2 + hr.place) * 3 + hr.head) * 2 + 0;
                    else if (feedback == 2 && st == 2) fr = gen::kMarkTintBase + ((trail * 2 + hr.place) * 3 + hr.head) * 2 + 1;
                    else if (colours && !passed)
                    {
                        int c = colourOf[sc->notes[(size_t)hr.note].pitch % 12];
                        if (c > 0) fr = gen::kMarkDotBase + (hr.place * 3 + hr.head) * gen::kDotColours + (c - 1);
                    }
                    mk[hr.row] = (uint8_t)fr;
                }
                if (feedback == 2 && !listening_ && sl.step >= 0 && current_->stepWrong[(size_t)sl.step])
                    ghost(*sc, sl, current_->stepWrong[(size_t)sl.step], trail, dimStaff, mk);
                // 8va / 8vb / 15ma / 15mb over or under the staff where notes were moved by octaves
                for (int staff = 0; staff < 2; ++staff)
                {
                    int code = sl.ott[staff];
                    if (!code || staff == dimStaff) continue;
                    int sign = code > 4 ? code - 4 : code, cont = code > 4 ? 1 : 0;
                    bool above = sign == 1 || sign == 3;
                    int row = staff == 0 ? (above ? 0 : scoredetail::rowOf(0, gen::kTrebleLowPos))
                                         : (above ? scoredetail::rowOf(1, gen::kBassTopPos + 7) : gen::kRows - 1);
                    int inward = above ? 1 : -1;
                    for (int tries = 0; tries < 2 && row >= 0 && row < gen::kRows; ++tries, row += inward)
                        if (mk[row] == 0 && sl.cells[row] == 0) { mk[row] = (uint8_t)(gen::kMarkOttavaBase + (sign - 1) * 2 + cont); break; }
                }
            }
            for (int r = 0; r < gen::kRows; ++r) setFrame(gen::kD_mark + s * gen::kRows + r, mk[r], gen::kMarkFrames);
        }
    }

    // A red head where the wrong note sits on the staff, with its accidental and ledger lines.
    void ghost(const Score& sc, const Slot& sl, int pitch, int trail, int dimStaff, uint8_t mk[gen::kRows])
    {
        const Step& st = sc.steps[(size_t)sl.step];
        int staff;
        if (settings_.hands == 1) staff = 0;
        else if (settings_.hands == 2) staff = 1;
        else if (st.hands == 1) staff = 0;
        else if (st.hands == 2) staff = 1;
        else staff = pitch >= 60 ? 0 : 1;
        if (staff == dimStaff) return;
        const Measure& m = sc.measures[sl.measure];
        Spelling sp = spellPitch(pitch, m.keySf, m.minor, 0);
        int pos = staffPosition(sp, staff) + sl.octShift[staff];   // under an 8va line like the written notes
        int row = scoredetail::rowOf(staff, pos);
        if (row < 0) return;
        int8_t keyAcc[7]; keyAccidentals(m.keySf, keyAcc);
        int acc = sp.acc == keyAcc[sp.letter] ? 0 : (sp.acc > 0 ? 1 : (sp.acc < 0 ? 2 : 3));
        bool ledgerRow = gen::kRowClass[row] == gen::kLedger;
        mk[row] = (uint8_t)(gen::kMarkGhostBase + (trail * 2 + (ledgerRow ? 1 : 0)) * 4 + acc);
        // ledger lines between the staff and the ghost, where nothing else is drawn
        auto ledger = [&](int lp) {
            int r = scoredetail::rowOf(staff, lp);
            if (r < 0 || r == row || mk[r] != 0 || sl.cells[r] != 0) return;
            mk[r] = (uint8_t)(gen::kMarkGLedgerBase + trail);
        };
        if (pos >= 10) for (int lp = 10; lp < pos; lp += 2) ledger(lp);
        if (pos <= -2) for (int lp = -2; lp > pos; lp -= 2) ledger(lp);
    }

    void pushDisplay()
    {
        // Each push makes the MPC's screen thread redraw something, so only changes are sent,
        // at most 48 a block (a page turn of ~150 cells takes a few blocks). After a reset
        // or a state load everything is re-sent once, in order, from a moving cursor.
        auto push = [this](int i, float want) {
            inParamChange_ = true;
            setNormalised(i, want);
            inParamChange_ = false;
            notifyHostParameterChanged(i);
        };
        if (forceFullPush_)
        {
            int budget = 96;
            if (forceCursor_ < gen::kDisplayBase) forceCursor_ = gen::kDisplayBase;
            while (budget > 0 && forceCursor_ < gen::kNumParams)
            {
                float want = desired_[forceCursor_];
                if (want >= 0) { push(forceCursor_, want); --budget; }
                ++forceCursor_;
            }
            if (forceCursor_ >= gen::kNumParams) { forceFullPush_ = false; forceCursor_ = gen::kDisplayBase; }
            return;
        }
        int budget = 48;
        auto pushRange = [&](int from, int to) {
            for (int i = from; i < to && budget > 0; ++i)
            {
                float want = desired_[i];
                if (want < 0 || std::fabs(getNormalised(i) - want) < 1e-6f) continue;
                push(i, want);
                --budget;
            }
        };
        // small, frequent things first (overlays, status, counters), then the cells
        pushRange(gen::kD_overlay, gen::kNumParams);
        pushRange(gen::kD_cell, gen::kD_overlay);
    }

    // ---- worker thread --------------------------------------------------------------------------------
    void wake() { wakeCv_.notify_all(); }

    void setText(int t, const std::string& s)
    {
        std::lock_guard<std::mutex> lk(textMx_);
        if (texts_[(size_t)t] != s) { texts_[(size_t)t] = s; needUpdateDisplay_.store(true); }
    }

    void workerLoop()
    {
        alsa_.open("Sight Reader");
        progress_.load(places_.home);
        loadPlaylist();
        rescan();
        refreshList();
        updateStats();
        auto lastLinnScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto lastLinnAsk = lastLinnScan;
        long lastKeyMTime = -1;
        while (true)
        {
            {
                std::unique_lock<std::mutex> lk(wakeMx_);
                wakeCv_.wait_for(lk, std::chrono::milliseconds(250));
                if (quit_) break;
            }
            // retire the previous piece
            if (Loaded* old = retired_.exchange(nullptr)) delete old;
            if (trig_rescan_.exchange(false)) { rescan(); refreshList(); resetTrigger(gen::P_RESCAN); }
            if (listDirty_.exchange(false)) refreshList();
            if (trig_restore_.exchange(false))
            {
                std::string p;
                { std::lock_guard<std::mutex> lk(textMx_); p = restorePath_; }
                if (get(gen::P_QPANEL) >= 0.5f) setTracked(gen::P_QPANEL, 0.0f);
                if (!p.empty()) { queueFromPath(p); loadPath(p, false); updateQueuePanel(); }
            }
            if (trig_load_.exchange(false)) { loadSelected(); resetTrigger(gen::P_LOAD); }
            if (trig_pladd_.exchange(false)) { playlistAdd(); resetTrigger(gen::P_PL_ADD); }
            if (trig_plremove_.exchange(false)) { playlistRemove(); resetTrigger(gen::P_PL_REMOVE); }
            if (qpanelOpened_.exchange(false)) { if (queuePos_ >= 0) setTracked(gen::P_PIECE, (float)(queuePos_ + 1)); updateQueuePanel(); }
            if (pieceSel_.exchange(false))
            {   // the data wheel on the title: open the list so the choice can be seen
                updateQueuePanel();
                if (!queue_.empty() && get(gen::P_QPANEL) < 0.5f) setTracked(gen::P_QPANEL, 1.0f);
            }
            if (trig_piecego_.exchange(false))
            {
                resetTrigger(gen::P_PIECE_GO);
                loadQueue((int)std::lround(get(gen::P_PIECE)) - 1);
                if (get(gen::P_QPANEL) >= 0.5f) setTracked(gen::P_QPANEL, 0.0f);
            }
            {
                int qp = qpick_.exchange(-1);
                if (qp >= 0)
                {
                    resetTrigger(gen::P_QPICK0 + qp);
                    loadQueue(qTop_ + qp);
                    if (get(gen::P_QPANEL) >= 0.5f) setTracked(gen::P_QPANEL, 0.0f);
                }
            }
            if (trig_next_.exchange(false))
            {
                if (queuePos_ >= 0 && queuePos_ + 1 < (int)queue_.size()) loadQueue(queuePos_ + 1);
                else if (!queue_.empty()) setText(gen::T_MESSAGE, "End of " + queueName_);
            }
            if (trig_gen_.exchange(false)) { generate(); resetTrigger(gen::P_GEN_GO); }
            if (trig_save_.exchange(false)) { saveGenerated(); resetTrigger(gen::P_GEN_SAVE); }
            if (trig_rebuild_.exchange(false))
            {
                std::string p; bool gen = false;
                { std::lock_guard<std::mutex> lk(textMx_); p = currentPath_; gen = currentIsGenerated_; }
                keepBar_.store(curBar_.load());   // a rebuild (a setting changed) keeps your place
                if (gen) rebuildGenerated();
                else if (!p.empty()) loadPath(p, false);
            }
            if (int hc = hintCode_.exchange(0))
            {
                int kind = hc & 255, expected = (hc >> 8) & 255, played = (hc >> 16) & 255;
                if (kind == 1)
                    setText(gen::T_MESSAGE, "You play " + noteName(played) + ", the music says " + noteName(expected) + ": try Pitch Tol = Octave");
                else if (kind == 2)
                    setText(gen::T_MESSAGE, "Waiting for the left hand (" + noteName(expected) + "). Hands = Right: RH only");
                else if (kind == 3)
                    setText(gen::T_MESSAGE, "Waiting for the right hand (" + noteName(expected) + "). Hands = Left: LH only");
                else if (kind == 7)   // LISTEN paused: the bar
                    setText(gen::T_MESSAGE, "Paused at bar " + std::to_string(expected | (played << 8)) +
                                            ": LISTEN goes on, Bar moves, SET marks loop");
                else if (kind == 4)   // LISTEN started: speed and channel
                    setText(gen::T_MESSAGE, "Listening at " + std::to_string(expected) + "% (Sight Reader MIDI Out, ch " + std::to_string(played) + ")");
                else if (kind == 5)   // TEST LIGHTS sent: port, channel
                {
                    static const char* ports[3] = {"USB", "MPC Out A", "MPC Out B"};
                    std::string m = std::string("Test sent (") + ports[std::max(0, std::min(2, expected))] + ", ch " + std::to_string(played) + "): green diagonal + C4 for 2 s";
                    setText(gen::T_MESSAGE, m);
                    setText(gen::T_LINN_STATUS, "Test sent. Nothing lit? See LinnStrument in the guide");
                }
                else if (kind == 6)
                {
                    setText(gen::T_MESSAGE, "TEST LIGHTS: nothing connected on this Linn Port");
                    setText(gen::T_LINN_STATUS, "Nothing connected: check Linn Port");
                }
            }
            PassRecord pr;
            bool anyPass = false;
            while (passQueue_.pop(pr)) { applyPass(pr); anyPass = true; }
            if (anyPass) { progress_.save(places_.home); updateStats(); refreshList(); }
            // LinnStrument: look for it every few seconds while Linn Mode is on (and at once when
            // the port changes or TEST LIGHTS is pressed)
            auto now = std::chrono::steady_clock::now();
            bool scanNow = trig_linnscan_.exchange(false);
            if ((getBool(gen::P_LINN) || scanNow) && (scanNow || now - lastLinnScan > std::chrono::seconds(3)))
            {
                lastLinnScan = now;
                int port = std::max(0, std::min(2, (int)get(gen::P_LINN_PORT)));
                bool ok = alsa_.findAndConnectLinn(port);
                static const char* found[3] = {"LinnStrument connected by USB", "Sending to MPC MIDI Out A", "Sending to MPC MIDI Out B"};
                static const char* missing[3] = {"Not found: plug in by USB", "MPC MIDI Out A not found", "MPC MIDI Out B not found"};
                setText(gen::T_LINN_STATUS, !alsa_.isOpen() ? "No MIDI sequencer here" : (ok ? found[port] : missing[port]));
            }
            // the instrument's Octave and Transpose (0.5.5): asked every 3 s while Linn Mode and
            // Read Transp are on (NRPN 299 for 36 and 37, left split); the answers arrive on Linn In
            if (getBool(gen::P_LINN) && getBool(gen::P_LINN_FOLLOW) && alsa_.linnConnected() &&
                (linnAskNow_.exchange(false) || now - lastLinnAsk > std::chrono::seconds(8)))
            { lastLinnAsk = now; linnAsk_.store(true); }
            alsa_.pollLinnNrpn([this](int param, int value) {
                if (param == 36 && value >= 0 && value <= 10) readOct_ = value - 5;
                else if (param == 37 && value >= 0 && value <= 14) readSemi_ = value - 7;
            });
            if (readOct_ > -100 && readSemi_ > -100 && getBool(gen::P_LINN_FOLLOW) &&
                (readOct_ != (int)std::lround(get(gen::P_LINN_OCT)) || readSemi_ != (int)std::lround(get(gen::P_LINN_SEMI))))
            {
                setTracked(gen::P_LINN_OCT, (float)readOct_);
                setTracked(gen::P_LINN_SEMI, (float)readSemi_);
                char b[96];
                std::snprintf(b, sizeof(b), "Read: Octave %+d, Transpose %+d st", readOct_, readSemi_);
                setText(gen::T_LINN_STATUS, b);
                // the cells move: fold and plan the fingering again when they depend on it
                if ((int)get(gen::P_LINN_FIT) == 2 || (int)get(gen::P_LINN_FINGER) == 1 || (int)get(gen::P_LINN_GUIDE) == 3)
                    trig_rebuild_.store(true);
                lightsRepaint_.store(true);
            }
            // the project's key, for Key Lights
            int km = (int)get(gen::P_LINN_KEY);
            if (trig_keyread_.exchange(false) || ((km == 1 || km == 2) && now - lastKeyRead_ > std::chrono::seconds(20)))
            {
                lastKeyRead_ = now;
                resetTrigger(gen::P_LINN_SYNC);
                MpcKey mk = readMpcKey();
                long mt = mk.valid ? 1 : 0;
                (void)lastKeyMTime; lastKeyMTime = mt;
                int root; uint16_t mask;
                if (mk.valid && parseKeyName(mk.projectKey, root, mask)) { keyRoot_[0].store(root); keyMask_[0].store(mask); }
                if (mk.valid && mk.padRoot >= 0 && parseKeyName(mk.padScale, root, mask)) { keyRoot_[1].store(root); keyMask_[1].store(mask); }
                keyRevision_.fetch_add(1);
                setText(gen::T_MPC_KEY, mk.valid ? ("Key " + (mk.projectKey.empty() ? std::string("-") : mk.projectKey) +
                                                    " / Pads " + mk.padScale) : "No project key yet (save once)");
            }
        }
        alsa_.close();
    }

    void resetTrigger(int index)
    {
        if (get(index) >= 0.5f) { inParamChange_ = true; set(index, 0.0f); inParamChange_ = false; needNotify_[index].store(true); }
    }

    void rescan()
    {
        std::vector<LibFolder> f;
        scanLibrary(places_, f);
        std::lock_guard<std::mutex> lk(libMx_);
        folders_.swap(f);
        appendPlaylistFolder();
    }

    // ---- the playlist (0.5.0): files in your order, kept in SightReader/playlist.txt -----------
    void loadPlaylist()
    {
        playlist_.clear();
        std::vector<uint8_t> raw;
        if (!readWholeFile(places_.home + "/playlist.txt", raw, 1u << 20)) return;
        std::string text(raw.begin(), raw.end());
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos); pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] != '#') playlist_.push_back(line);
        }
    }
    void savePlaylist()
    {
        std::string out = "# Sight Reader playlist: one file per line, played in this order\n";
        for (const std::string& p : playlist_) out += p + "\n";
        writeWholeFile(places_.home + "/playlist.txt", out.data(), out.size());
    }
    // Called with libMx_ held: the playlist is the last folder of the list while it has files.
    void appendPlaylistFolder()
    {
        if (!folders_.empty() && folders_.back().playlist) folders_.pop_back();
        if (playlist_.empty()) return;
        LibFolder pl; pl.name = "Playlist"; pl.dir = places_.home; pl.playlist = true;
        for (const std::string& path : playlist_)
        {
            const LibEntry* found = nullptr;
            for (const LibFolder& fo : folders_) { for (const LibEntry& e : fo.entries) if (e.path == path) { found = &e; break; } if (found) break; }
            if (found) { pl.entries.push_back(*found); continue; }
            std::vector<uint8_t> probe;
            if (!readWholeFile(path, probe, 8u << 20)) continue;          // gone: left out (kept in the file)
            LibEntry e; e.path = path; e.file = path.substr(path.rfind('/') + 1); e.title = prettyName(e.file);
            pl.entries.push_back(e);
        }
        if (!pl.entries.empty()) folders_.push_back(pl);
    }
    // The file the LOAD list has selected (empty when none).
    std::string selectedPath(bool* inPlaylist = nullptr)
    {
        std::lock_guard<std::mutex> lk(libMx_);
        int nf = (int)folders_.size();
        if (nf == 0) return std::string();
        int fi = std::max(0, std::min(nf - 1, (int)get(gen::P_FOLDER)));
        std::vector<const LibEntry*> v = sortedEntries(folders_[(size_t)fi]);
        int sel = (int)std::lround(get(gen::P_SELECT));
        if (inPlaylist) *inPlaylist = folders_[(size_t)fi].playlist;
        return sel >= 0 && sel < (int)v.size() ? v[(size_t)sel]->path : std::string();
    }
    void playlistAdd()
    {
        bool inPl = false;
        std::string path = selectedPath(&inPl);
        if (path.empty()) return;
        playlist_.push_back(path);
        savePlaylist();
        { std::lock_guard<std::mutex> lk(libMx_); appendPlaylistFolder(); }
        setText(gen::T_MESSAGE, "Added to Playlist (" + std::to_string(playlist_.size()) + " files; it is the last folder)");
        refreshList();
    }
    void playlistRemove()
    {
        bool inPl = false;
        std::string path = selectedPath(&inPl);
        if (path.empty()) return;
        int at = -1;
        if (inPl)
        {   // the selected row of the Playlist folder (the same file may be in it twice)
            int sel = (int)std::lround(get(gen::P_SELECT)), k = -1;
            for (int i = 0; i < (int)playlist_.size(); ++i)
            {
                bool listed = false;
                { std::lock_guard<std::mutex> lk(libMx_); for (const LibEntry& e : folders_.back().entries) if (e.path == playlist_[(size_t)i]) { listed = true; break; } }
                if (listed && ++k == sel) { at = i; break; }
            }
        }
        else for (int i = (int)playlist_.size() - 1; i >= 0 && at < 0; --i) if (playlist_[(size_t)i] == path) at = i;
        if (at < 0) { setText(gen::T_MESSAGE, "This file is not in the Playlist"); return; }
        playlist_.erase(playlist_.begin() + at);
        savePlaylist();
        { std::lock_guard<std::mutex> lk(libMx_); appendPlaylistFolder(); }
        setText(gen::T_MESSAGE, playlist_.empty() ? std::string("Playlist empty") : "Removed (" + std::to_string(playlist_.size()) + " left)");
        refreshList();
    }

    // Visible entries of the selected folder, in the chosen order (the Playlist keeps its own).
    std::vector<const LibEntry*> sortedEntries(const LibFolder& fo)
    {
        std::vector<const LibEntry*> v;
        for (const LibEntry& e : fo.entries) v.push_back(&e);
        if (fo.playlist) return v;
        int sort = (int)get(gen::P_SORT);
        auto prog = [this](const LibEntry* e) -> const PieceProgress* {
            auto it = progress_.pieces.find(ProgressStore::keyFor(e->path, places_.home, places_.pluginDir));
            return it == progress_.pieces.end() ? nullptr : &it->second;
        };
        if (sort == 1) std::stable_sort(v.begin(), v.end(), [&](const LibEntry* a, const LibEntry* b) { auto pa = prog(a), pb = prog(b); return (pa ? pa->best : -1) > (pb ? pb->best : -1); });
        if (sort == 2) std::stable_sort(v.begin(), v.end(), [&](const LibEntry* a, const LibEntry* b) { auto pa = prog(a), pb = prog(b); return (pa ? pa->lastTime : 0) > (pb ? pb->lastTime : 0); });
        if (sort == 3) std::stable_sort(v.begin(), v.end(), [](const LibEntry* a, const LibEntry* b) { return a->level < b->level; });
        return v;
    }

    void refreshList()
    {
        std::lock_guard<std::mutex> lk(libMx_);
        int nf = (int)folders_.size();
        if (nf == 0)
        {
            setText(gen::T_FOLDER_NAME, "(no library)");
            for (int r = 0; r < 8; ++r) setText(gen::T_ROW0 + r, r == 0 ? "No MIDI files found." : (r == 1 ? ("Put .mid files in " + places_.home + "/Library/<folder>") : (r == 2 ? "or run tools/library.py sync from a computer." : "")));
            setText(gen::T_INFO0, "GEN makes exercises without any files.");
            return;
        }
        int fi = std::max(0, std::min(nf - 1, (int)get(gen::P_FOLDER)));
        const LibFolder& fo = folders_[(size_t)fi];
        std::vector<const LibEntry*> v = sortedEntries(fo);
        // The selection: Select (the data wheel on the list) is the file; Scroll and Row (the
        // older knobs) and a tap on a row move it too. The list scrolls to keep it in view.
        const int n = (int)v.size();
        const int gen0 = listGen_.load();
        int src = listSrc_.exchange(0), pick = pick_.exchange(-1);
        int scroll = std::max(0, std::min(std::max(0, n - 1), (int)std::lround(get(gen::P_SCROLL))));
        int row = std::max(0, std::min(7, (int)std::lround(get(gen::P_ROW))));
        int sel;
        if (pick >= 0) { resetTrigger(gen::P_PICK0 + pick); row = pick; sel = scroll + row; }
        else if (src == 2) sel = (int)std::lround(get(gen::P_SELECT));
        else sel = scroll + row;
        sel = std::max(0, std::min(std::max(0, n - 1), sel));
        if (sel < scroll) scroll = sel;
        if (sel > scroll + 7) scroll = sel - 7;
        row = sel - scroll;
        // written back only for a list with files in it (an empty or missing library, or a
        // project loading, leaves the saved values alone)
        std::unique_lock<std::mutex> stateLock(stateMx_);   // never in the middle of a project loading
        if (n > 0 && listGen_.load() == gen0 && !loadingState_.load())
        {
            if ((int)std::lround(get(gen::P_SCROLL)) != scroll) setTracked(gen::P_SCROLL, (float)scroll);
            if ((int)std::lround(get(gen::P_ROW)) != row) setTracked(gen::P_ROW, (float)row);
            if ((int)std::lround(get(gen::P_SELECT)) != sel) setTracked(gen::P_SELECT, (float)sel);
        }
        stateLock.unlock();
        rowSel_.store(n > 0 && sel < n ? row : -1);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s  (%d/%d)", fo.name.c_str(), fi + 1, nf);
        setText(gen::T_FOLDER_NAME, buf);
        for (int r = 0; r < 8; ++r)
        {
            int i = scroll + r;
            if (i >= (int)v.size()) { setText(gen::T_ROW0 + r, ""); continue; }
            const LibEntry* e = v[(size_t)i];
            auto it = progress_.pieces.find(ProgressStore::keyFor(e->path, places_.home, places_.pluginDir));
            std::string t = e->title;
            if (!e->composer.empty()) t += " - " + e->composer;
            if (t.size() > 46) t = t.substr(0, 45) + "~";
            if (it != progress_.pieces.end() && it->second.plays > 0) std::snprintf(buf, sizeof(buf), "%3d%%  %s", it->second.best, t.c_str());
            else std::snprintf(buf, sizeof(buf), "  --   %s", t.c_str());
            setText(gen::T_ROW0 + r, std::string(buf).substr(0, 60));
        }
        if (sel < (int)v.size())
        {
            const LibEntry* e = v[(size_t)sel];
            auto it = progress_.pieces.find(ProgressStore::keyFor(e->path, places_.home, places_.pluginDir));
            setText(gen::T_INFO0, e->title.substr(0, 60));
            setText(gen::T_INFO1, (e->composer.empty() ? std::string("") : e->composer + "   ") + (e->level ? ("Level " + std::to_string(e->level)) : std::string("")));
            if (it != progress_.pieces.end())
            {
                std::snprintf(buf, sizeof(buf), "Best %d%%  Last %d%%  Played %d  %.0f min", it->second.best, it->second.last, it->second.plays, it->second.minutes);
                setText(gen::T_INFO2, buf);
            }
            else setText(gen::T_INFO2, "Not played yet");
            setText(gen::T_INFO3, e->source.substr(0, 60));
        }
        if (src || pick >= 0 || fi != lastShownFolder_)
        {
            lastShownFolder_ = fi;
            std::snprintf(buf, sizeof(buf), "%d files%s", (int)v.size(), fo.playlist ? " in your order. REMOVE takes one out" : "");
            setText(gen::T_MESSAGE, buf);
        }
    }

    // LOAD: the selected file, and its folder (or the Playlist) becomes the queue: the PLAY
    // page's piece list, Auto Next, and the (k/N) after the title.
    void loadSelected()
    {
        int sel;
        {
            std::lock_guard<std::mutex> lk(libMx_);
            int nf = (int)folders_.size();
            if (nf == 0) return;
            int fi = std::max(0, std::min(nf - 1, (int)get(gen::P_FOLDER)));
            const LibFolder& fo = folders_[(size_t)fi];
            std::vector<const LibEntry*> v = sortedEntries(fo);
            sel = (int)std::lround(get(gen::P_SELECT));
            if (sel < 0 || sel >= (int)v.size()) return;
            queue_.clear();
            for (const LibEntry* e : v) queue_.push_back({e->path, e->title});
            queueName_ = fo.name;
            qTop_ = 0;
        }
        loadQueue(sel);
    }
    void loadQueue(int i)
    {
        if (i < 0 || i >= (int)queue_.size()) return;
        queuePos_ = i;
        pendingTitle_ = queue_[(size_t)i].title;
        loadPath(queue_[(size_t)i].path, true);
        setTracked(gen::P_PIECE, (float)(i + 1));
        updateQueuePanel();
    }
    // After a project load: the restored file's folder is the queue.
    void queueFromPath(const std::string& path)
    {
        std::lock_guard<std::mutex> lk(libMx_);
        for (const LibFolder& fo : folders_)
        {
            if (fo.playlist) continue;
            std::vector<const LibEntry*> v = sortedEntries(fo);
            for (size_t k = 0; k < v.size(); ++k)
                if (v[k]->path == path)
                {
                    queue_.clear();
                    for (const LibEntry* e : v) queue_.push_back({e->path, e->title});
                    queueName_ = fo.name; queuePos_ = (int)k; qTop_ = 0;
                    pendingTitle_ = v[k]->title;
                    return;
                }
        }
    }
    // The PLAY page's piece list: eight rows around the piece the data wheel points at.
    void updateQueuePanel()
    {
        const int n = (int)queue_.size();
        if (n == 0)
        {
            for (int r = 0; r < 8; ++r) setText(gen::T_Q0 + r, r == 0 ? "Load a piece on LOAD: its folder (or the Playlist)" : (r == 1 ? "becomes this list." : ""));
            setText(gen::T_QINFO, "No list yet");
            qSelBits_.store(0);
            return;
        }
        int want = (int)std::lround(get(gen::P_PIECE)) - 1;
        int selQ = std::max(0, std::min(n - 1, want));
        if (selQ != want) setTracked(gen::P_PIECE, (float)(selQ + 1));
        if (selQ < qTop_) qTop_ = selQ;
        if (selQ > qTop_ + 7) qTop_ = selQ - 7;
        qTop_ = std::max(0, std::min(std::max(0, n - 8), qTop_));
        uint32_t bits = 0;
        char buf[160];
        for (int r = 0; r < 8; ++r)
        {
            int i = qTop_ + r;
            if (i >= n) { setText(gen::T_Q0 + r, ""); continue; }
            std::snprintf(buf, sizeof(buf), "%3d   %s", i + 1, queue_[(size_t)i].title.c_str());
            setText(gen::T_Q0 + r, std::string(buf).substr(0, 60));
            uint32_t f = i == selQ ? 2u : (i == queuePos_ ? 1u : 0u);
            bits |= f << (2 * r);
        }
        qSelBits_.store(bits);
        std::snprintf(buf, sizeof(buf), "%s   %d/%d   Wheel: choose. Push or tap: load", queueName_.c_str(), queuePos_ + 1, n);
        setText(gen::T_QINFO, std::string(buf).substr(0, 60));
    }

    BuildOptions buildOptions() const
    {
        BuildOptions o;
        o.splitPitch = (int)get(gen::P_SPLIT);
        o.transpose = (int)get(gen::P_TRANSPOSE);
        return o;
    }

    // Removes notes outside the focus range when Out Range is "Hide".
    void filterRange(Smf& smf) const
    {
        if ((int)get(gen::P_OUT_RANGE) != 2) return;
        int lo = (int)get(gen::P_RANGE_LO) - (int)get(gen::P_TRANSPOSE), hi = (int)get(gen::P_RANGE_HI) - (int)get(gen::P_TRANSPOSE);
        std::vector<SmfNote> keep;
        for (const SmfNote& n : smf.notes) if (n.pitch >= lo && n.pitch <= hi) keep.push_back(n);
        if (!keep.empty()) smf.notes.swap(keep);
    }

    // Fit Range (Linn Mode, 0.5.5): Fold moves a note the LinnStrument cannot play by octaves
    // into its range, so the staff shows it where it is played; a note that lands on another of
    // the same pitch and time in the same track is dropped.
    void fitToLinn(Smf& smf) const
    {
        if (!getBool(gen::P_LINN) || (int)get(gen::P_LINN_FIT) != 2) return;
        int lo, hi; linnRange(lo, hi);
        const int tr = (int)get(gen::P_TRANSPOSE);
        lo -= tr; hi -= tr;
        if (hi - lo < 11) return;
        for (SmfNote& n : smf.notes)
        {
            int p = n.pitch;
            while (p < lo) p += 12;
            while (p > hi) p -= 12;
            n.pitch = (uint8_t)std::max(0, std::min(127, p));
        }
        std::vector<SmfNote> keep;
        keep.reserve(smf.notes.size());
        for (const SmfNote& n : smf.notes)
        {
            bool dup = false;
            for (size_t k = keep.size(); k-- > 0 && !dup;)
            {
                if (keep[k].on != n.on) { if (keep[k].on < n.on) break; continue; }
                dup = keep[k].pitch == n.pitch && keep[k].track == n.track;
                if (dup && n.off > keep[k].off) keep[k].off = n.off;
            }
            if (!dup) keep.push_back(n);
        }
        smf.notes.swap(keep);
    }

    void loadPath(const std::string& path, bool fresh)
    {
        loading_.store(true);
        std::vector<uint8_t> bytes;
        std::string err;
        if (!readWholeFile(path, bytes, 8u << 20)) { setText(gen::T_MESSAGE, "Cannot read " + path); loading_.store(false); return; }
        if (!publish(bytes, path, false, err))
        {
            setText(gen::T_MESSAGE, "Cannot use this file: " + err);
        }
        else if (fresh) setText(gen::T_MESSAGE, "Loaded");
        loading_.store(false);
    }

    bool publish(const std::vector<uint8_t>& bytes, const std::string& path, bool generated, std::string& err)
    {
        Smf smf;
        if (!parseSmf(bytes.data(), bytes.size(), smf, err)) return false;
        filterRange(smf);
        fitToLinn(smf);
        Loaded* L = new Loaded();
        if (!buildScore(smf, buildOptions(), L->score, err)) { delete L; return false; }
        planFingering(L->score, layoutFromParams(), L->fing);
        L->stepState.assign(L->score.steps.size(), 0);
        L->stepWrong.assign(L->score.steps.size(), 0);
        L->noteState.assign(L->score.notes.size(), 0);
        L->path = path; L->generated = generated;
        if (generated) L->midiBytes = bytes;
        std::string title = generated ? L->score.title : pendingTitle_;
        std::string ft = L->score.title;
        bool junk = ft.empty() || ft.find("control track") != std::string::npos || ft.find("Track") == 0 || ft.compare(0, 3, "at ") == 0;
        if (title.empty()) title = junk ? prettyName(path.substr(path.rfind('/') + 1)) : ft;
        L->title = title;
        L->progressKey = generated ? std::string() : ProgressStore::keyFor(path, places_.home, places_.pluginDir);
        // wait for the audio thread to take the previous hand-off
        for (int i = 0; i < 200 && incoming_.load() != nullptr; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (Loaded* stale = incoming_.exchange(L)) delete stale;
        {
            std::lock_guard<std::mutex> lk(textMx_);
            currentPath_ = generated ? std::string() : path;
            currentIsGenerated_ = generated;
            currentKey_ = L->progressKey;
        }
        char buf[200];
        bool inQueue = !generated && queuePos_ >= 0 && queuePos_ < (int)queue_.size() && queue_[(size_t)queuePos_].path == path && queue_.size() > 1;
        std::string shown = title.size() > 46 ? title.substr(0, 45) + "~" : title;
        if (inQueue) std::snprintf(buf, sizeof(buf), "%s  (%d/%d)", shown.c_str(), queuePos_ + 1, (int)queue_.size());
        else std::snprintf(buf, sizeof(buf), "%s", title.c_str());
        setText(gen::T_TITLE, std::string(buf).substr(0, 60));
        updateStats();
        return true;
    }

    GenSettings genSettings(uint32_t seed) const
    {
        GenSettings g;
        g.keyIndex = (int)get(gen::P_GEN_KEY); g.scaleIndex = (int)get(gen::P_GEN_SCALE);
        g.rhythm = (int)get(gen::P_GEN_RHYTHM); g.leaps = (int)get(gen::P_GEN_LEAPS);
        g.hands = (int)get(gen::P_GEN_HANDS);
        static const int bars[4] = {4, 8, 16, 32};
        g.bars = bars[std::max(0, std::min(3, (int)get(gen::P_GEN_BARS)))];
        g.meter = (int)get(gen::P_GEN_METER); g.range = (int)get(gen::P_GEN_RANGE); g.lhStyle = (int)get(gen::P_GEN_LH);
        g.seed = seed;
        return g;
    }

    void generate()
    {
        genSeed_ = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ (genSeed_ * 2654435761u);
        rebuildGenerated();
    }
    void rebuildGenerated()
    {
        std::string title, desc, err;
        std::vector<uint8_t> mid = generateExercise(genSettings(genSeed_), title, desc);
        lastGenBytes_ = mid; lastGenTitle_ = title;
        if (publish(mid, "", true, err)) { setText(gen::T_GEN_DESC, desc); setText(gen::T_MESSAGE, "New exercise: " + desc); }
        else setText(gen::T_MESSAGE, "Generator error: " + err);
    }
    void saveGenerated()
    {
        if (lastGenBytes_.empty()) { setText(gen::T_MESSAGE, "Generate an exercise first"); return; }
        std::string dir = places_.home + "/Generated";
        makeDirs(dir);
        std::time_t t = std::time(nullptr); std::tm tmv;
#if SR_POSIX
        localtime_r(&t, &tmv);
#else
        tmv = *std::localtime(&t);
#endif
        char name[96]; std::strftime(name, sizeof(name), "%Y%m%d-%H%M%S", &tmv);
        std::string base = lastGenTitle_; for (char& c : base) if (c == ' ' || c == '/' || c == '#') c = '_';
        std::string path = dir + "/" + std::string(name) + "_" + base + ".mid";
        if (writeWholeFile(path, lastGenBytes_.data(), lastGenBytes_.size())) { setText(gen::T_MESSAGE, "Saved to Generated/"); rescan(); refreshList(); }
        else setText(gen::T_MESSAGE, "Could not write " + dir);
    }

    void applyPass(const PassRecord& pr)
    {
        std::string key;
        { std::lock_guard<std::mutex> lk(textMx_); key = currentKey_; }
        progress_.markToday();
        if (key.empty()) { lastGenAccuracy_ = pr.accuracy; return; }
        PieceProgress& p = progress_.pieces[key];
        p.plays += 1; p.last = pr.accuracy; p.best = std::max(p.best, pr.accuracy);
        if (pr.accuracy >= (int)get(gen::P_GOAL)) p.bestTempo = std::max(p.bestTempo, pr.tempo);
        p.lastTime = (long)std::time(nullptr); p.minutes += pr.seconds / 60.0;
        for (int i = 0; i < 16; ++i) if (pr.sections[i] != 255) p.sections[i] = pr.sections[i];
    }

    void updateStats()
    {
        std::string key;
        { std::lock_guard<std::mutex> lk(textMx_); key = currentKey_; }
        auto it = progress_.pieces.find(key);
        if (!key.empty() && it != progress_.pieces.end())
        {
            const PieceProgress& p = it->second;
            statBest_.store(p.best); statLast_.store(p.last); statPlays_.store(p.plays); statMinutes_.store((int)p.minutes);
            for (int i = 0; i < 16; ++i) statSections_[i].store(p.sections[i]);
            char buf[120];
            std::snprintf(buf, sizeof(buf), "Best tempo at goal: %d%%", p.bestTempo);
            setText(gen::T_STATS1, buf);
        }
        else
        {
            statBest_.store(-1); statLast_.store(-1); statPlays_.store(-1); statMinutes_.store(-1);
            for (int i = 0; i < 16; ++i) statSections_[i].store(255);
            setText(gen::T_STATS1, key.empty() ? "Exercises are not tracked; save one to track it" : "Not played yet");
        }
        statStreak_.store(progress_.streak());
        double total = 0; int pieces = 0;
        for (const auto& kv : progress_.pieces) { total += kv.second.minutes; ++pieces; }
        char buf[120];
        std::snprintf(buf, sizeof(buf), "All pieces: %d played, %.0f minutes", pieces, total);
        setText(gen::T_STATS0, buf);
        setText(gen::T_STATS2, "Bars show the last pass, one per sixteenth of the piece");
    }

    // ---- members --------------------------------------------------------------------------------------
    double sr_ = 44100.0;
    Engine engine_;
    EngineSettings settings_;
    Loaded* current_ = nullptr;                    // audio thread
    std::atomic<Loaded*> incoming_{nullptr};       // worker -> audio
    std::atomic<Loaded*> retired_{nullptr};        // audio -> worker
    Spsc<PassRecord, 16> passQueue_;
    Synth synth_;
    AlsaOut alsa_;
    LinnLights lights_;
    float desired_[gen::kNumParams];
    int screenSlot_[gen::kSlots] = {};
    bool forceFullPush_ = true;
    int forceCursor_ = gen::kDisplayBase;
    int flashFrame_ = 0, flashBlocks_ = 0;
    int lastPlayed_ = -1;
    int hintRun_ = 0, hintKind_ = 0;
    ListenPlayer listen_;
    bool listening_ = false, listenPaused_ = false, listenLoop_ = false;
    int listenCh_ = 0;
    std::atomic<bool> listenStopReq_{false};
    std::atomic<int> hintCode_{0};                 // audio -> worker: kind | expected << 8 | played << 16
    uint64_t blockCounter_ = 0, lastUpdateDisplayBlock_ = 0;
    bool lastPlayParam_ = false, lastTransportPlaying_ = false;
    bool lastLoop_ = false; int lastLoopFrom_ = -1, lastLoopBars_ = -1, lastHands_ = -1, lastMode_ = -1;
    // LinnStrument (audio thread)
    int lit_[16] = {0}; int litCount_ = 0; int linnCh_ = 0;
    uint32_t guideSig_ = 0;
    bool lightsDirty_ = true, barsLit_ = false, sectionWasManual_ = false, wasConnected_ = false, linnWasOn_ = false;
    int keyLightsShown_ = -1;
    std::atomic<int> keyRoot_[2] = {{-1}, {-1}};
    std::atomic<int> keyMask_[2] = {{0}, {0}};
    std::atomic<int> keyRevision_{0};
    // flags
    std::atomic<bool> trig_restart_{false}, trig_load_{false}, trig_rescan_{false}, trig_gen_{false}, trig_save_{false},
                      trig_section_{false}, trig_clear_{false}, trig_keyread_{false}, trig_rebuild_{false}, trig_restore_{false},
                      listDirty_{false}, layoutChanged_{false}, loading_{false}, needUpdateDisplay_{true},
                      trig_sndtest_{false}, trig_colreset_{false},
                      trig_piecego_{false}, trig_pladd_{false}, trig_plremove_{false}, trig_linnscan_{false},
                      trig_linntest_{false}, trig_next_{false}, pieceSel_{false}, qpanelOpened_{false}, bpmAuthority_{false};
    // 0.5.0
    std::atomic<int> listSrc_{0}, listGen_{0}, pick_{-1}, qpick_{-1}, locateReq_{0}, rowSel_{-1};
    std::atomic<bool> loadingState_{false};
    // 0.5.5
    std::atomic<bool> trig_setfrom_{false}, trig_setto_{false}, linnAsk_{false}, linnAskNow_{false}, lightsRepaint_{false};
    std::atomic<int> keepBar_{-1}, curBar_{-1};
    bool released_[128] = {};                       // audio thread: notes you let go of this block
    int readOct_ = -1000, readSemi_ = -1000;        // worker: what the LinnStrument answered
    std::mutex stateMx_;                           // a state load, against the worker writing list values
    std::atomic<uint32_t> qSelBits_{0};
    std::atomic<int> firstBar_{1}, lastBar_{1};
    int lastCursorStep_ = -1, stoppedBlocks_ = 0, linnGenSeen_ = -1;
    uint64_t sampleClock_ = 0, autoNextAt_ = 0, testUntil_ = 0, testWaitUntil_ = 0;
    double linnBudget_ = 0;
    uint64_t nextReloadAt_ = 0, linnQuietUntil_ = 0, nextHealAt_ = 0;
    int healCount_ = 0;
    struct OffHeal { int pitch = -1; int left = 0; };
    OffHeal offHeal_[16];
    int testCh_ = 0;
    double lastPpq_ = -1;
    uint32_t baseSig_ = 0;
    struct QueueItem { std::string path, title; };
    std::vector<QueueItem> queue_;                 // worker
    int queuePos_ = -1, qTop_ = 0, lastShownFolder_ = -1;
    std::string queueName_;
    std::vector<std::string> playlist_;
    std::atomic<bool> needNotify_[gen::kNumControls] = {};
    int lastInt_[gen::kNumControls];
    float lastNorm_[gen::kNumControls];
    float lastHostNorm_[gen::kNumControls];       // what the host itself last sent (0.5.5)
    // stats snapshot (worker -> audio)
    std::atomic<int> statBest_{-1}, statLast_{-1}, statPlays_{-1}, statMinutes_{-1}, statStreak_{0};
    std::atomic<int> statSections_[16] = {{255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}, {255}};
    // worker
    std::thread worker_;
    std::mutex wakeMx_;
    std::condition_variable wakeCv_;
    bool quit_ = false;
    Places places_;
    ProgressStore progress_;
    std::mutex libMx_;
    std::vector<LibFolder> folders_;
    std::string pendingTitle_;
    uint32_t genSeed_ = 12345;
    std::vector<uint8_t> lastGenBytes_; std::string lastGenTitle_;
    int lastGenAccuracy_ = -1;
    std::chrono::steady_clock::time_point lastKeyRead_ = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    // texts (UI thread reads, worker writes)
    std::mutex textMx_;
    std::vector<std::string> texts_;
    std::string currentPath_, restorePath_, currentKey_;
    bool currentIsGenerated_ = false;
};

// ---------------------------------------------------------------------------------------------
// Entry point. The framework copies at most 31 characters of parameter text; titles and list
// rows need more. JUCE hosts (the MPC) pass a 256-byte buffer, so text parameters get 63.
// ---------------------------------------------------------------------------------------------
namespace {
VstIntPtr srDispatcher(AEffect* e, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt)
{
    if (opcode == effGetParamDisplay && e && e->object && ptr && index >= gen::kTextBase && index < gen::kDisplayBase)
    {
        mpc::Plugin* p = static_cast<mpc::Plugin*>(e->object);
        mpc::copyString(static_cast<char*>(ptr), 64, p->displayText(index).c_str());
        return 0;
    }
    if (opcode == effSetChunk && e && e->object)
    {
        SightReader* sr = static_cast<SightReader*>(static_cast<mpc::Plugin*>(e->object));
        std::lock_guard<std::mutex> lk(sr->stateMutex());
        sr->setLoadingState(true);
        VstIntPtr r = mpc::detail::dispatcher(e, opcode, index, value, ptr, opt);
        sr->setLoadingState(false);
        return r;
    }
    return mpc::detail::dispatcher(e, opcode, index, value, ptr, opt);
}
} // namespace

extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)
{
    if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0) return nullptr;
    AEffect* e = mpc::detail::createEffect(new SightReader(), host);
    if (e) e->dispatcher = &srDispatcher;
    return e;
}
#if defined(__APPLE__)
extern "C" MPC_EXPORT AEffect* main_macho(audioMasterCallback host) { return VSTPluginMain(host); }
#endif
