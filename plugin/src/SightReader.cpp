// Sight Reader - a sight-reading trainer that runs on the MPC.
//
// What it does
//   Shows a grand staff on the plugin's page, reads the notes you play on the track's MIDI
//   input (a LinnStrument, a keyboard, the pads), and checks them against a MIDI file or a
//   generated exercise. Wait mode holds on each note until you play it; Rhythm mode runs a
//   clock and grades timing; Flow mode runs the clock but waits a while for missed notes.
//   It makes no sound of its own except a short, quiet sound on mistakes (six timbres with
//   an ADSR envelope, on the SOUND page) and a metronome if you want one: put your instrument
//   on another track listening to the same input.
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

constexpr int kVersion = 200;   // 0.2.0

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
                                                         "Linn", "MPC Key", "Exercise"};
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
        for (int i = 0; i < gen::kNumControls; ++i) { lastInt_[i] = -1; lastNorm_[i] = 0.0f; }
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
        forceFullPush_ = true; forceCursor_ = gen::kDisplayBase;
    }

    // ---- parameter side effects (any thread: keep it to flags) --------------------------------
    void parameterChanged(int index, float plain) override
    {
        if (inParamChange_) return;
        if (index >= gen::kDisplayBase) return;
        if (index >= gen::kTextBase) return;
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
                if (index != gen::P_ROW) nudge(index);
                listDirty_.store(true); wake(); break;
            case gen::P_LOOP_FROM: nudge(index); break;
            case gen::P_TRANSPOSE: case gen::P_SPLIT: case gen::P_OUT_RANGE: case gen::P_RANGE_LO: case gen::P_RANGE_HI:
                if (index == gen::P_TRANSPOSE || index == gen::P_SPLIT || (int)get(gen::P_OUT_RANGE) == 2) { trig_rebuild_.store(true); wake(); }
                break;
            case gen::P_LINN_SIZE: case gen::P_LINN_ROWOFF: case gen::P_LINN_LOW: case gen::P_LINN_LEFT:
                trig_rebuild_.store(true); layoutChanged_.store(true); wake(); break;
            case gen::P_LINN: case gen::P_LINN_KEY: trig_keyread_.store(true); wake(); break;
            default: break;
        }
    }

    // Q-Link and data-wheel moves arrive as the read-back value plus 1/128 (wheel 1/100) of the
    // range; on a long list that skips entries. A small move counts as one step.
    void nudge(int index)
    {
        int prev = lastInt_[index];
        float norm = getNormalised(index);
        float prevNorm = lastNorm_[index];
        lastNorm_[index] = norm;
        int now = (int)std::lround(get(index));
        const mpc::Param& p = param(index);
        float span = p.max - p.min;
        if (prev >= 0 && span > 20 && std::fabs(norm - prevNorm) > 0 && std::fabs(norm - prevNorm) <= 0.0105f && now != prev)
        {
            int stepped = prev + (norm > prevNorm ? 1 : -1);
            inParamChange_ = true;
            set(index, (float)stepped);
            inParamChange_ = false;
            lastNorm_[index] = getNormalised(index);
            now = stepped;
            needNotify_[index].store(true);
        }
        lastInt_[index] = now;
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
        if (index == gen::P_LOOP_FROM || index == gen::P_LOOP_BARS)
        {
            char b[32]; std::snprintf(b, sizeof(b), index == gen::P_LOOP_FROM ? "Bar %d" : "%d bars", (int)std::lround(plain));
            out = b; return true;
        }
        return false;
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> lk(textMx_);
        std::string s = "path=" + currentPath_ + "\n";
        out.assign(s.begin(), s.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::string s((const char*)data, size);
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
        // 1. a new piece from the worker
        if (Loaded* in = incoming_.load(std::memory_order_acquire))
        {
            if (retired_.load(std::memory_order_acquire) == nullptr)
            {
                incoming_.store(nullptr, std::memory_order_release);
                accompAllOff();
                guideAllOff();
                Loaded* old = current_;
                current_ = in;
                engine_.setScore(&current_->score, current_->stepState.data(), current_->noteState.data(), current_->stepWrong.data());
                readSettings();
                engine_.restart();
                if (old) retired_.store(old, std::memory_order_release);
                forceFullPush_ = true;
                lightsDirty_ = true;
                if (barsLit_) { barsLit_ = false; lights_.clearLayer(1); }
                wake();
            }
        }
        // 2. settings and triggers
        readSettings();
        if (trig_restart_.exchange(false)) { engine_.restart(); accompAllOff(); resetParam(gen::P_RESTART); setParamFromEngine(gen::P_PLAY, 0.0f); }
        if (trig_sndtest_.exchange(false)) { errorSound(0, 1.0f); resetParam(gen::P_SND_TEST); }
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
            else if (e.isAllNotesOff()) accompAllOff();
        }
        // 4. engine
        engine_.process(settings_, notes, nn, n, sr_);
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
        // 5. LinnStrument
        lightsBlock();
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
        s.tempoFactor = get(gen::P_TEMPO) / 100.0;
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
                double t = get(gen::P_TEMPO) + settings_.ramp;
                if (t > 200) t = 200;
                setParamFromEngine(gen::P_TEMPO, (float)t);
                break;
            }
            case EvType::AccompOn:
                if ((int)get(gen::P_ACCOMP) != 0 && current_) sendAccomp(ev.a, ev.b);
                break;
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
    struct Sounding { int8_t pitch = -1; int32_t endUnit = 0; };
    Sounding sounding_[32];
    struct Sent { int pitch; uint64_t block; };
    Sent recentSent_[32]; int recentIdx_ = 0;

    void sendAccomp(int pitch, int noteIndex)
    {
        if (!alsa_.isOpen() || !current_) return;
        const PlayNote& pn = current_->score.notes[(size_t)noteIndex];
        uint8_t m[3] = {0x90, (uint8_t)pitch, pn.vel ? pn.vel : (uint8_t)80};
        if (alsa_.sendMidi(m))
        {
            for (Sounding& s : sounding_) if (s.pitch < 0) { s.pitch = (int8_t)pitch; s.endUnit = pn.end; break; }
            recentSent_[recentIdx_] = {pitch, blockCounter_}; recentIdx_ = (recentIdx_ + 1) % 32;
        }
    }
    void accompOffsDue()
    {
        double pos = engine_.position();
        for (Sounding& s : sounding_)
            if (s.pitch >= 0 && (pos >= s.endUnit || (settings_.mode == 0 && pos + 0.5 >= s.endUnit)))
            { uint8_t m[3] = {0x80, (uint8_t)s.pitch, 0}; alsa_.sendMidi(m); s.pitch = -1; }
    }
    void accompAllOff()
    {
        for (Sounding& s : sounding_) if (s.pitch >= 0) { uint8_t m[3] = {0x80, (uint8_t)s.pitch, 0}; alsa_.sendMidi(m); s.pitch = -1; }
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
        L.lowest = (int)get(gen::P_LINN_LOW);
        L.leftHanded = getBool(gen::P_LINN_LEFT);
        return L;
    }

    void guideAllOff()
    {
        for (int i = 0; i < litCount_; ++i)
        {
            uint8_t m[3] = {(uint8_t)(0x80 | (linnCh_ & 15)), (uint8_t)lit_[i], 0};
            alsa_.sendLinn(m);
        }
        litCount_ = 0;
        lights_.clearLayer(2);
    }

    void lightsBlock()
    {
        bool linnOn = getBool(gen::P_LINN);
        bool connected = alsa_.linnConnected();
        if (connected && !wasConnected_) { lights_.invalidate(); lights_.resetCursor(); litCount_ = 0; lightsDirty_ = true; }
        wasConnected_ = connected;
        if (!linnOn)
        {
            if (linnWasOn_) { guideAllOff(); lights_.clearLayer(0); lights_.clearLayer(1); barsLit_ = false; linnWasOn_ = false; }
            if (connected) lights_.flush(24, [this](const uint8_t* m) { alsa_.sendLinn(m); });
            trig_section_.store(false); trig_clear_.store(false);
            return;
        }
        linnWasOn_ = true;
        LinnLayout L = layoutFromParams();
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
        int gcol = (int)get(gen::P_LINN_GCOL);
        bool fingering = (int)get(gen::P_LINN_FINGER) == 1 || guideMode == 3;
        linnCh_ = (int)get(gen::P_LINN_CH) - 1;
        int want[16]; int wantCount = 0;
        uint8_t wantColour[16];
        int8_t wantCol[16], wantRow[16];
        if (guideMode > 0 && current_ && engine_.state() != RunState::Done)
        {
            int s = engine_.nextRequiredStep(std::max(0, engine_.currentStep()));
            for (int pass = 0; pass < (guideMode == 2 ? 2 : 1) && s >= 0; ++pass)
            {
                engine_.forEachRequired(s, [&](const PlayNote& pn, uint8_t state) {
                    if (state == 1 || wantCount >= 16) return;
                    size_t ni = (size_t)(&pn - &current_->score.notes[0]);
                    want[wantCount] = pn.pitch;
                    wantCol[wantCount] = ni < current_->fing.col.size() ? current_->fing.col[ni] : -1;
                    wantRow[wantCount] = ni < current_->fing.row.size() ? current_->fing.row[ni] : -1;
                    int f = ni < current_->fing.finger.size() ? current_->fing.finger[ni] : 0;
                    wantColour[wantCount] = (fingering && f > 0) ? kFingerColour[f] : (pass == 1 ? (uint8_t)kLinnWhite : (gcol == 0 ? 0 : kLinnColourChoice[gcol - 1]));
                    ++wantCount;
                });
                s = engine_.nextRequiredStep(s + 1);
            }
        }
        // Did the guide change?
        uint32_t sig = 2166136261u;
        for (int i = 0; i < wantCount; ++i) sig = (sig ^ (uint32_t)(want[i] * 31 + wantColour[i])) * 16777619u;
        sig ^= (uint32_t)guideMode * 7 + (fingering ? 1 : 0);
        if (sig != guideSig_ || lightsDirty_)
        {
            guideSig_ = sig;
            bool notesMode = gcol == 0 && !fingering;
            // Note On highlights (played colour) for notesMode; custom cells otherwise
            int newLit[16]; int newCount = 0;
            if (notesMode) for (int i = 0; i < wantCount; ++i) if (wantColour[i] == 0) newLit[newCount++] = want[i];
            for (int i = 0; i < litCount_; ++i)
            {
                bool keep = false; for (int k = 0; k < newCount; ++k) keep |= newLit[k] == lit_[i];
                if (!keep) { uint8_t m[3] = {(uint8_t)(0x80 | (linnCh_ & 15)), (uint8_t)lit_[i], 0}; alsa_.sendLinn(m); }
            }
            for (int k = 0; k < newCount; ++k)
            {
                bool was = false; for (int i = 0; i < litCount_; ++i) was |= newLit[k] == lit_[i];
                if (!was) { uint8_t m[3] = {(uint8_t)(0x90 | (linnCh_ & 15)), (uint8_t)newLit[k], 100}; alsa_.sendLinn(m); }
            }
            for (int k = 0; k < newCount; ++k) lit_[k] = newLit[k];
            litCount_ = newCount;
            lights_.clearLayer(2);
            for (int i = 0; i < wantCount; ++i)
            {
                if (notesMode && wantColour[i] == 0) continue;
                uint8_t c = wantColour[i] ? wantColour[i] : (uint8_t)kLinnWhite;
                if (fingering && wantCol[i] > 0) lights_.setCell(2, wantCol[i], wantRow[i], c);
                else lights_.setNote(2, L, want[i], c);
            }
            lightsDirty_ = false;
        }
        if (connected) lights_.flush(24, [this](const uint8_t* m) { alsa_.sendLinn(m); });
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
        setFrame(gen::kD_mask + 0, settings_.hands == 2 ? 1 : 0, gen::kStaffMaskFrames);
        setFrame(gen::kD_mask + 1, settings_.hands == 1 ? 1 : 0, gen::kStaffMaskFrames);
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
        }
        setFrame(gen::kD_status, status, gen::kStatusFrames);
        setFrame(gen::kD_handicon, settings_.hands, gen::kHandFrames);
        int curMeasure = sc ? sc->measures[(size_t)engine_.currentMeasure()].number : -1;
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
        if (sc && names > 0)
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
        bool showFingers = getBool(gen::P_LINN) && (int)get(gen::P_LINN_FINGER) == 1;
        setFrame(gen::kD_finger + 0, showFingers ? f0 : 0, gen::kFingerFrames);
        setFrame(gen::kD_finger + 1, showFingers ? f1 : 0, gen::kFingerFrames);
        // progress through the piece
        double prog = 0;
        if (sc && !sc->steps.empty()) prog = std::max(0.0, std::min(1.0, engine_.position() / std::max(1, sc->endUnit)));
        if (sc && settings_.mode == 0 && !sc->steps.empty()) prog = (double)engine_.currentStep() / sc->steps.size();
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
        // the hand not practised is dimmed: no marks on its staff
        const int dimStaff = settings_.hands == 1 ? 1 : (settings_.hands == 2 ? 0 : -1);
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
                    uint8_t st = current_->noteState[(size_t)hr.note];
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
                if (feedback == 2 && sl.step >= 0 && current_->stepWrong[(size_t)sl.step])
                    ghost(*sc, sl, current_->stepWrong[(size_t)sl.step], trail, dimStaff, mk);
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
        int pos = staffPosition(sp, staff);
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
        rescan();
        refreshList();
        updateStats();
        auto lastLinnScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
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
                if (!p.empty()) loadPath(p, false);
            }
            if (trig_load_.exchange(false)) { loadSelected(); resetTrigger(gen::P_LOAD); }
            if (trig_gen_.exchange(false)) { generate(); resetTrigger(gen::P_GEN_GO); }
            if (trig_save_.exchange(false)) { saveGenerated(); resetTrigger(gen::P_GEN_SAVE); }
            if (trig_rebuild_.exchange(false))
            {
                std::string p; bool gen = false;
                { std::lock_guard<std::mutex> lk(textMx_); p = currentPath_; gen = currentIsGenerated_; }
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
            }
            PassRecord pr;
            bool anyPass = false;
            while (passQueue_.pop(pr)) { applyPass(pr); anyPass = true; }
            if (anyPass) { progress_.save(places_.home); updateStats(); refreshList(); }
            // LinnStrument: look for it every few seconds while Linn Mode is on
            auto now = std::chrono::steady_clock::now();
            if (getBool(gen::P_LINN) && now - lastLinnScan > std::chrono::seconds(3))
            {
                lastLinnScan = now;
                bool ok = alsa_.findAndConnectLinn();
                setText(gen::T_LINN_STATUS, !alsa_.isOpen() ? "No MIDI sequencer on this system"
                                            : (ok ? "LinnStrument connected" : "LinnStrument not found - plug it in by USB"));
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
                setText(gen::T_MPC_KEY, mk.valid ? ("Project key: " + (mk.projectKey.empty() ? std::string("-") : mk.projectKey) +
                                                    "   Pad scale: " + mk.padScale) : "MPC project key not found (save the project once)");
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
    }

    // Visible entries of the selected folder, in the chosen order.
    std::vector<const LibEntry*> sortedEntries(const LibFolder& fo)
    {
        std::vector<const LibEntry*> v;
        for (const LibEntry& e : fo.entries) v.push_back(&e);
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
        int scroll = std::max(0, std::min((int)v.size() - 1, (int)get(gen::P_SCROLL)));
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
        int sel = scroll + (int)get(gen::P_ROW);
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
        std::snprintf(buf, sizeof(buf), "%d files", (int)v.size());
        setText(gen::T_MESSAGE, buf);
    }

    void loadSelected()
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lk(libMx_);
            int nf = (int)folders_.size();
            if (nf == 0) return;
            int fi = std::max(0, std::min(nf - 1, (int)get(gen::P_FOLDER)));
            std::vector<const LibEntry*> v = sortedEntries(folders_[(size_t)fi]);
            int sel = std::max(0, (int)get(gen::P_SCROLL)) + (int)get(gen::P_ROW);
            if (sel >= (int)v.size()) return;
            path = v[(size_t)sel]->path;
            pendingTitle_ = v[(size_t)sel]->title;
        }
        loadPath(path, true);
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
        std::snprintf(buf, sizeof(buf), "%s", title.c_str());
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
                      trig_sndtest_{false}, trig_colreset_{false};
    std::atomic<bool> needNotify_[gen::kNumControls] = {};
    int lastInt_[gen::kNumControls];
    float lastNorm_[gen::kNumControls];
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
