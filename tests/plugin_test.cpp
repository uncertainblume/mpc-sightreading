// plugin_test - drives the built plugin through its VST2 interface like the MPC would, with a
// library on disk, and checks what it shows. Writes the screen as JSON for
// tools/skin/preview.py, so the PLAY page can be looked at without an MPC.
//
//   plugin_test <plugin.so> <home dir with Library/> <out.json>
#include "mpc_vst2_abi.h"
#include "sr/gen/layout_gen.h"
#include "sr/gen/params_gen.h"
#include "sr/smf.h"

#include <dlfcn.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <array>
#include <thread>
#include <vector>

using namespace sr;

static int g_automate = 0, g_updates = 0, g_fail = 0;
static VstTimeInfo g_time;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static VstIntPtr host(AEffect*, VstInt32 op, VstInt32, VstIntPtr, void*, float)
{
    switch (op)
    {
        case audioMasterVersion: return 2400;
        case audioMasterAutomate: ++g_automate; return 0;
        case audioMasterUpdateDisplay: ++g_updates; return 1;
        case audioMasterGetTime: return (VstIntPtr)&g_time;
        default: return 0;
    }
}

// 0.7.0: with the fake ALSA (tests/fake_alsa.c, argument 4) every MIDI Out message is written to a
// trace with the audio time this sets before each block
static void (*g_fakeTime)(double) = nullptr;
static std::string g_tracePath;

struct Plug
{
    AEffect* e = nullptr;
    float* out[2]; std::vector<float> l, r;
    std::vector<uint8_t> evbuf;
    VstIntPtr d(VstInt32 op, VstInt32 i = 0, VstIntPtr v = 0, void* p = nullptr, float o = 0) { return e->dispatcher(e, op, i, v, p, o); }
    std::string text(int i) { char b[512] = {0}; d(effGetParamDisplay, i, 0, b); return b; }
    void block(const std::vector<std::array<uint8_t, 3>>& midi = {})
    {
        evbuf.assign(sizeof(VstEvents) + sizeof(VstEvent*) * (midi.size() + 2), 0);
        VstEvents* evs = (VstEvents*)evbuf.data();
        std::vector<VstMidiEvent> me(midi.size());
        for (size_t k = 0; k < midi.size(); ++k)
        {
            std::memset(&me[k], 0, sizeof(VstMidiEvent));
            me[k].type = kVstMidiType; me[k].byteSize = sizeof(VstMidiEvent); me[k].deltaFrames = (VstInt32)k;
            me[k].midiData[0] = (char)midi[k][0]; me[k].midiData[1] = (char)midi[k][1]; me[k].midiData[2] = (char)midi[k][2];
            evs->events[k] = (VstEvent*)&me[k];
        }
        evs->numEvents = (VstInt32)midi.size();
        d(effProcessEvents, 0, 0, evs);
        if (g_fakeTime) g_fakeTime(g_time.samplePos / 44100.0);
        e->processReplacing(e, out, out, 128);
        g_time.samplePos += 128;
    }
    void run(double seconds) { int n = (int)(seconds * 44100 / 128); for (int i = 0; i < n; ++i) block(); }
    void waitWorker(double seconds) { for (int i = 0; i < (int)(seconds * 20); ++i) { for (int k = 0; k < 18; ++k) block(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); } }
    int frame(int idx, int nFrames) { float v = e->getParameter(e, idx); return (int)std::floor(v * (nFrames - 1) + 1e-4); }
};

static int rowFrames(int r)
{
    int c = gen::kRowClass[r];
    return c == gen::kSpace ? gen::kFrames_space : (c == gen::kLine ? gen::kFrames_line : gen::kFrames_ledger);
}

static void dump(Plug& p, FILE* f)
{
    std::fprintf(f, "{\"keysig\": %d, \"timesig\": %d, \"flash\": %d, \"masks\": [%d, %d], \"cells\": [",
                 p.frame(gen::kD_keysig, gen::kKeySigFrames) - 7, p.frame(gen::kD_timesig, gen::kTimeSigFrames),
                 p.frame(gen::kD_flash, gen::kFlashFrames), p.frame(gen::kD_mask, gen::kStaffMaskFrames), p.frame(gen::kD_mask + 1, gen::kStaffMaskFrames));
    for (int s = 0; s < gen::kSlots; ++s)
    {
        std::fprintf(f, "%s[", s ? "," : "");
        for (int r = 0; r < gen::kRows; ++r) std::fprintf(f, "%s%d", r ? "," : "", p.frame(gen::kD_cell + s * gen::kRows + r, rowFrames(r)));
        std::fprintf(f, "]");
    }
    std::fprintf(f, "], \"overlays\": [");
    for (int s = 0; s < gen::kSlots; ++s) std::fprintf(f, "%s%d", s ? "," : "", p.frame(gen::kD_overlay + s, gen::kOverlayFrames));
    std::fprintf(f, "], \"barlines\": [");
    for (int s = 0; s <= gen::kSlots; ++s) std::fprintf(f, "%s%d", s ? "," : "", p.frame(gen::kD_barline + s, gen::kBarlineFrames));
    std::fprintf(f, "], \"marks\": [");
    for (int s = 0; s < gen::kSlots; ++s)
    {
        std::fprintf(f, "%s[", s ? "," : "");
        for (int r = 0; r < gen::kRows; ++r) std::fprintf(f, "%s%d", r ? "," : "", p.frame(gen::kD_mark + s * gen::kRows + r, gen::kMarkFrames));
        std::fprintf(f, "]");
    }
    std::fprintf(f, "]}");
}

static int mark(Plug& p, int slot, int row) { return p.frame(gen::kD_mark + slot * gen::kRows + row, gen::kMarkFrames); }
// treble staff row of a staff position (0 = bottom line E4)
static int trebleRow(int pos) { return gen::kTrebleTopPos - pos; }
static int noteNameFrame(int pitch, bool flats = false)
{
    return 1 + (pitch - gen::kNoteNameLow) + (flats ? (gen::kNoteNameHigh - gen::kNoteNameLow + 1) : 0);
}
static int tintFrame(int trail, int place, int head, int red) { return gen::kMarkTintBase + ((trail * 2 + place) * 3 + head) * 2 + red; }

// Goes to the library folder whose name contains `name`.
static bool openFolder(Plug& p, const char* name)
{
    for (int fi = 0; fi < 40; ++fi)
    {
        if (p.text(gen::kTextBase + gen::T_FOLDER_NAME).find(name) != std::string::npos) return true;
        p.e->setParameter(p.e, gen::P_FOLDER, (fi + 1) / 63.0f);
        p.waitWorker(0.3);
    }
    return false;
}

// Peak level of the plugin's own sound for a while after a trigger.
static float peakAfter(Plug& p, int param, double seconds)
{
    p.e->setParameter(p.e, param, 1.0f);
    float peak = 0;
    for (int b = 0; b < (int)(seconds * 44100 / 128); ++b)
    {
        p.block();
        for (int i = 0; i < 128; ++i) peak = std::max(peak, std::fabs(p.l[(size_t)i]));
    }
    return peak;
}

// Plain values the way the plugin sees them, and the MPC's data wheel. Since 0.6.5 the skin lists
// every value of a number, so a click moves the value the MPC holds by one of them.
static int plainOf(Plug& p, int idx)
{
    const auto& c = gen::kControls[idx];
    return (int)std::lround(c.min + p.e->getParameter(p.e, idx) * (c.max - c.min));
}
static void setPlain(Plug& p, int idx, double v)
{
    const auto& c = gen::kControls[idx];
    p.e->setParameter(p.e, idx, (float)((v - c.min) / (c.max - c.min)));
}
static int numSteps(int idx)
{
    const auto& c = gen::kControls[idx];
    return (int)std::lround((c.max - c.min) / c.step) + 1;
}
static float stepNorm(int idx, int k) { return (float)k / (float)(numSteps(idx) - 1); }
static int stepOf(Plug& p, int idx) { return (int)std::lround(p.e->getParameter(p.e, idx) * (numSteps(idx) - 1)); }
static void wheel(Plug& p, int idx, int steps)
{
    for (int i = 0; i < std::abs(steps); ++i)
    {
        int k = std::max(0, std::min(numSteps(idx) - 1, stepOf(p, idx) + (steps > 0 ? 1 : -1)));
        p.e->setParameter(p.e, idx, stepNorm(idx, k));
        p.block();
    }
}
static void writeMid(const std::string& dir, const std::string& file, const std::vector<int>& rh, const std::vector<int>& lhWhole)
{
    std::vector<SmfWriteTrack> tr(lhWhole.empty() ? 1 : 2);
    tr[0].name = "Right";
    for (size_t i = 0; i < rh.size(); ++i) tr[0].notes.push_back({(int)i * 96, (int)(i + 1) * 96, (uint8_t)rh[i], 80});
    if (!lhWhole.empty()) { tr[1].name = "Left"; for (size_t i = 0; i < lhWhole.size(); ++i) tr[1].notes.push_back({(int)i * 384, (int)(i + 1) * 384, (uint8_t)lhWhole[i], 70}); }
    std::vector<uint8_t> bytes = writeSmf(96, 100, 4, 4, 0, false, file, tr);
    std::string cmd = "mkdir -p '" + dir + "'";
    if (std::system(cmd.c_str()) != 0) std::printf("mkdir failed\n");
    FILE* mf = std::fopen((dir + "/" + file + ".mid").c_str(), "wb");
    if (mf) { std::fwrite(bytes.data(), 1, bytes.size(), mf); std::fclose(mf); }
}

// Every value, text and name, for tools/skin/render_page.py.
static void writeState(Plug& p, const std::string& statePath)
{
    FILE* sf = std::fopen(statePath.c_str(), "w");
    if (!sf) return;
    auto esc = [](const std::string& t) { std::string o; for (char c : t) { if (c == '"' || c == '\\') o.push_back('\\'); if ((unsigned char)c >= 32) o.push_back(c); } return o; };
    std::fprintf(sf, "{\"values\": [");
    for (int i = 0; i < gen::kNumParams; ++i) std::fprintf(sf, "%s%.6f", i ? "," : "", (double)p.e->getParameter(p.e, i));
    std::fprintf(sf, "], \"text\": {");
    for (int i = 0; i < gen::kDisplayBase; ++i) std::fprintf(sf, "%s\"%d\": \"%s\"", i ? "," : "", i, esc(p.text(i)).c_str());
    std::fprintf(sf, "}, \"name\": {");
    for (int i = 0; i < gen::kTextBase; ++i) { char b[256] = {0}; p.d(effGetParamName, i, 0, b); std::fprintf(sf, "%s\"%d\": \"%s\"", i ? "," : "", i, esc(b).c_str()); }
    std::fprintf(sf, "}}\n");
    std::fclose(sf);
}

static int digits(Plug& p, int base, int n)
{
    int v = 0; bool any = false;
    for (int i = 0; i < n; ++i) { int fr = p.frame(base + i, gen::kDigitFrames); if (fr > 0) { v = v * 10 + (fr - 1); any = true; } else if (any) v *= 10; }
    return any ? v : -1;
}

struct MidiLine { double t; std::string port, kind; int ch, note, vel; };
// MIDI Out (or Linn Out) messages in the fake ALSA's trace, from line `from` on
static std::vector<MidiLine> traceLines(size_t from, const char* port = "MIDI_Out")
{
    std::vector<MidiLine> out;
    FILE* f = std::fopen(g_tracePath.c_str(), "r");
    if (!f) return out;
    char line[256]; size_t n = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        if (n++ < from) continue;
        MidiLine m; char pt[64] = {0}, kind[16] = {0};
        if (std::sscanf(line, "%lf %63s %15s %d %d %d", &m.t, pt, kind, &m.ch, &m.note, &m.vel) == 6 && std::strcmp(pt, port) == 0)
        { m.port = pt; m.kind = kind; out.push_back(m); }
    }
    std::fclose(f);
    return out;
}
static size_t traceCount()
{
    FILE* f = std::fopen(g_tracePath.c_str(), "r");
    if (!f) return 0;
    size_t n = 0; char line[256];
    while (std::fgets(line, sizeof(line), f)) ++n;
    std::fclose(f);
    return n;
}
static double nowSec() { return g_time.samplePos / 44100.0; }
// when `note` was switched on (kind "on") or off in `lines`, the first time; -1 if never
static double whenOf(const std::vector<MidiLine>& lines, int note, const char* kind)
{
    for (const MidiLine& m : lines) if (m.note == note && m.kind == kind) return m.t;
    return -1;
}

// The plugin's saved state, as the MPC keeps it in a project
static std::vector<uint8_t> getChunk(Plug& p)
{
    void* ptr = nullptr;
    VstIntPtr n = p.d(effGetChunk, 0, 0, &ptr);
    return ptr && n > 0 ? std::vector<uint8_t>((uint8_t*)ptr, (uint8_t*)ptr + n) : std::vector<uint8_t>();
}
static void setChunk(Plug& p, std::vector<uint8_t> c) { p.d(effSetChunk, 0, (VstIntPtr)c.size(), c.data()); }
// The same state as a version before 0.7.0 would have saved it: no "ver=" line in the extra state
static std::vector<uint8_t> withoutVersion(std::vector<uint8_t> c)
{
    std::string s(c.begin(), c.end());
    size_t extra = s.find("path="), v = s.find("ver=");
    if (extra == std::string::npos || v == std::string::npos || extra < 4) return c;
    size_t e = s.find('\n', v);
    size_t cut = (e == std::string::npos ? s.size() : e + 1) - v;
    c.erase(c.begin() + (long)v, c.begin() + (long)(v + cut));
    uint32_t len = (uint32_t)(c.size() - extra);
    for (int i = 0; i < 4; ++i) c[extra - 4 + (size_t)i] = (uint8_t)(len >> (8 * i));
    return c;
}

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: plugin_test plugin.so home out.json\n"); return 2; }
    setenv("SIGHTREADER_HOME", argv[2], 1);
    if (argc > 4)
    {   // the fake ALSA, loaded before the plugin so the plugin's dlopen("libasound.so.2") finds it
        g_tracePath = std::string(argv[3]) + ".midi.txt";
        setenv("FAKE_ALSA_TRACE", g_tracePath.c_str(), 1);
        void* fa = dlopen(argv[4], RTLD_NOW | RTLD_GLOBAL);
        if (!fa) { std::printf("fake ALSA: %s\n", dlerror()); return 1; }
        g_fakeTime = (void (*)(double))dlsym(fa, "fake_alsa_time");
    }
    // A long one-hand piece for the scrolling test: 64 quarter notes, C major up and down.
    std::vector<int> longNotes;
    {
        const int up[] = {60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84, 83};
        for (int i = 0; i < 64; ++i)
        {
            int k = i % 30; int idx = k < 16 ? k : 30 - k;
            longNotes.push_back(up[idx < 16 ? idx : 15]);
        }
        std::vector<SmfWriteTrack> tr(1);
        tr[0].name = "Right";
        for (size_t i = 0; i < longNotes.size(); ++i) tr[0].notes.push_back({(int)i * 96, (int)(i + 1) * 96, (uint8_t)longNotes[i], 80});
        std::vector<uint8_t> bytes = writeSmf(96, 100, 4, 4, 0, false, "Long scale", tr);
        std::string dir = std::string(argv[2]) + "/Library/Test";
        std::string cmd = "mkdir -p '" + dir + "'";
        if (std::system(cmd.c_str()) != 0) std::printf("mkdir failed\n");
        FILE* mf = std::fopen((dir + "/long_scale.mid").c_str(), "wb");
        if (mf) { std::fwrite(bytes.data(), 1, bytes.size(), mf); std::fclose(mf); }
    }
    // 0.5.0: a folder of three short drills (the queue), and a piece that needs 8va and 8vb
    const std::vector<int> drill = {60, 62, 64, 65, 67, 69, 71, 72};
    {
        std::string q = std::string(argv[2]) + "/Library/Queue";
        writeMid(q, "a_first", drill, {});
        writeMid(q, "b_second", drill, {});
        writeMid(q, "c_third", drill, {});
        writeMid(std::string(argv[2]) + "/Library/Ottava", "ottava", {88, 89, 91, 93, 72, 74, 76, 77}, {48, 33});
    }
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    auto entry = (AEffect* (*)(audioMasterCallback))dlsym(h, "VSTPluginMain");
    Plug p; p.e = entry(host);
    CHECK(p.e && p.e->numParams == gen::kNumParams);
    p.l.assign(512, 0); p.r.assign(512, 0); p.out[0] = p.l.data(); p.out[1] = p.r.data();
    std::memset(&g_time, 0, sizeof(g_time)); g_time.sampleRate = 44100; g_time.tempo = 120; g_time.flags = kVstTempoValid;
    p.d(effOpen); p.d(effSetSampleRate, 0, 0, nullptr, 44100); p.d(effSetBlockSize, 0, 128); p.d(effMainsChanged, 0, 1);

    p.waitWorker(1.0);
    // built-in drill folders come first; find the library folder
    CHECK(openFolder(p, "Classics"));
    std::string row0 = p.text(gen::kTextBase + gen::T_ROW0), folder = p.text(gen::kTextBase + gen::T_FOLDER_NAME);
    std::printf("folder '%s' row0 '%s'\n", folder.c_str(), row0.c_str());
    CHECK(row0.find("Minuet") != std::string::npos);
    CHECK(g_updates > 0);

    // LOAD the first row
    p.e->setParameter(p.e, gen::P_LOAD, 1.0f);
    p.waitWorker(1.0);
    std::string title = p.text(gen::kTextBase + gen::T_TITLE);
    std::printf("title '%s'\n", title.c_str());
    CHECK(title.find("Minuet") != std::string::npos);
    CHECK(p.e->getParameter(p.e, gen::P_LOAD) < 0.5f);                 // the trigger was released
    p.run(0.3);
    CHECK(p.frame(gen::kD_keysig, gen::kKeySigFrames) == 8);            // G major: one sharp
    CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 1);            // WAIT
    CHECK(digits(p, gen::kD_d_bars, 3) == 32);
    CHECK(p.frame(gen::kD_overlay + 0, gen::kOverlayFrames) == 1);      // cursor on the first note

    // values read with their units; readouts follow their controls
    std::printf("value texts: '%s' '%s' '%s' '%s' '%s'\n", p.text(gen::P_TIMING).c_str(), p.text(gen::P_TEMPO).c_str(),
                p.text(gen::P_SND_PITCH).c_str(), p.text(gen::P_OUT_CH).c_str(), p.text(gen::P_GEN_RHYTHM).c_str());
    CHECK(p.text(gen::P_TIMING) == "160 ms" && p.text(gen::P_TEMPO) == "100%" && p.text(gen::P_SND_PITCH) == "0 st");
    CHECK(p.text(gen::P_OUT_CH) == "Ch 1" && p.text(gen::P_GEN_RHYTHM) == "Level 2");
    {
        int ri = -1; for (int i = 0; i < gen::kN_ro; ++i) if (gen::kRoControl[i] == gen::P_WRONG) ri = i;
        CHECK(ri >= 0 && p.frame(gen::kD_ro + ri, gen::kRoFrames[ri]) == 1);   // Count
        p.e->setParameter(p.e, gen::P_WRONG, 1.0f); p.run(0.05);
        CHECK(p.frame(gen::kD_ro + ri, gen::kRoFrames[ri]) == 2);              // Stop
        p.e->setParameter(p.e, gen::P_WRONG, 0.5f); p.run(0.05);
    }
    CHECK(gen::kControls[gen::P_COL_DS].def == 16);                        // D# is Navy
    // the next notes of both hands: D5 over G3 B3 D4 (the bottom of the left hand's chord)
    CHECK(p.frame(gen::kD_nextname, gen::kNoteNameFrames) == noteNameFrame(74));
    CHECK(p.frame(gen::kD_nextname_lh, gen::kNoteNameFrames) == noteNameFrame(55));
    // colour dots on notes still to come: C5 (an eighth, slot 4) is cyan by default
    const int cyan = 4;
    CHECK(gen::kControls[gen::P_COL_C].def == cyan);
    CHECK(mark(p, 4, trebleRow(5)) == gen::kMarkDotBase + 0 * gen::kDotColours + (cyan - 1));
    CHECK(p.frame(gen::kD_swatch + 0, gen::kSwatchFrames) == cyan);

    FILE* f = std::fopen(argv[3], "w");
    std::fprintf(f, "{\"screens\": [");
    dump(p, f);
    // play the first two steps right (D5 with the LH chord G3 B3 D4), then a wrong note
    p.block({{0x90, 74, 100}, {0x90, 55, 90}, {0x90, 59, 90}, {0x90, 62, 90}});
    p.run(0.1);
    // D5 has turned green, brightly while it is the note just played
    std::printf("D5 mark %d (green %d)\n", mark(p, 0, trebleRow(6)), tintFrame(0, 0, 0, 0));
    CHECK(mark(p, 0, trebleRow(6)) == tintFrame(0, 0, 0, 0));
    p.block({{0x90, 67, 100}});                                         // G4
    p.run(0.1);
    p.block({{0x90, 67, 100}});                                         // G4 again at once: a re-strike, not a mistake
    p.run(0.05);
    CHECK(digits(p, gen::kD_d_err, 3) <= 0);
    CHECK(mark(p, 0, trebleRow(6)) == tintFrame(1, 0, 0, 0));          // D5 is now in the played trail
    p.block({{0x90, 61, 100}});                                         // wrong: C#4 for A4
    p.run(0.05);
    int flash = p.frame(gen::kD_flash, gen::kFlashFrames);
    std::printf("after wrong note: flash %d, errors %d, accuracy %d, next-name frame %d, ghost %d\n", flash, digits(p, gen::kD_d_err, 3),
                digits(p, gen::kD_d_acc, 3), p.frame(gen::kD_nextname, gen::kNoteNameFrames), mark(p, 2, trebleRow(-2)));
    CHECK(flash == 1);
    CHECK(digits(p, gen::kD_d_err, 3) == 1);
    // a red C#4 where it was played: on middle C's ledger line, with a sharp, in A4's column
    CHECK(mark(p, 2, trebleRow(-2)) == gen::kMarkGhostBase + (0 * 2 + 1) * 4 + 1);
    std::fprintf(f, ",\n");
    dump(p, f);
    // the rest of the first bar, to see the played overlay
    p.block({{0x90, 69, 100}}); p.run(0.05);                            // A4
    p.block({{0x90, 71, 100}, {0x90, 57, 90}}); p.run(0.05);            // B4 + A3
    p.block({{0x90, 72, 100}}); p.run(0.3);                             // C5
    std::printf("accuracy after the bar: %d\n", digits(p, gen::kD_d_acc, 3));
    CHECK(digits(p, gen::kD_d_acc, 3) == 90);                           // 9 right, 1 wrong
    CHECK(mark(p, 2, trebleRow(-2)) == gen::kMarkGhostBase + (1 * 2 + 1) * 4 + 1);   // the ghost stays, lighter
    std::fprintf(f, ",\n");
    dump(p, f);
    std::fprintf(f, "]}\n");
    std::fclose(f);
    // full state for tools/skin/render_page.py
    writeState(p, std::string(argv[3]) + ".state.json");
    // a resume from the host keeps the place in the piece (0.1 went back to the start)
    int stepBefore = digits(p, gen::kD_d_acc, 3);
    p.d(effMainsChanged, 0, 0); p.d(effMainsChanged, 0, 1); p.run(0.3);
    CHECK(digits(p, gen::kD_d_acc, 3) == stepBefore);
    CHECK(p.frame(gen::kD_overlay + 0, gen::kOverlayFrames) == 2);      // the first column is still played
    // colours: C off removes C5's dot; RESET COLORS brings the scheme back
    p.e->setParameter(p.e, gen::P_COL_C, 0.0f); p.run(0.1);
    int c5slot = -1;
    for (int sl = 0; sl < gen::kSlots && c5slot < 0; ++sl) if (p.frame(gen::kD_overlay + sl, gen::kOverlayFrames) == 1) c5slot = sl;
    std::printf("current column %d\n", c5slot);
    p.e->setParameter(p.e, gen::P_COL_RESET, 1.0f); p.run(0.1);
    CHECK(std::fabs(p.e->getParameter(p.e, gen::P_COL_C) - cyan / gen::kControls[gen::P_COL_C].max) < 0.01f);
    CHECK(p.e->getParameter(p.e, gen::P_COL_RESET) < 0.5f);

    // ---- stuck on a both-hands step: the message says the left hand is wanted -----------------
    {
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        p.block({{0x90, 74, 100}}); p.run(0.05);                        // D5, the right hand's part of step 1
        p.block({{0x90, 67, 100}}); p.run(0.05);                        // G4: going on with the right hand
        p.block({{0x90, 69, 100}}); p.waitWorker(0.5);                  // A4
        std::string msg = p.text(gen::kTextBase + gen::T_MESSAGE);
        std::printf("message '%s'\n", msg.c_str());
        CHECK(msg.find("left hand (G3)") != std::string::npos);
    }

    // ---- sounds: every type within sensible levels; the default is quiet ----------------------
    {
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        float def = peakAfter(p, gen::P_SND_TEST, 0.6);
        std::printf("default error sound peak %.3f\n", def);
        CHECK(def > 0.01f && def < 0.12f);
        p.e->setParameter(p.e, gen::P_BLIP_VOL, 1.0f); p.e->setParameter(p.e, gen::P_SND_VOL, 1.0f);
        for (int t = 0; t < 6; ++t)
        {
            p.e->setParameter(p.e, gen::P_SND_TYPE, t / 5.0f); p.run(0.05);
            float pk = peakAfter(p, gen::P_SND_TEST, 0.8);
            std::printf("  sound %d at full volume: peak %.3f\n", t, pk);
            CHECK(pk > 0.15f && pk < 0.9f);
        }
        // a held tone: sustain and length
        p.e->setParameter(p.e, gen::P_SND_TYPE, 4 / 5.0f);
        p.e->setParameter(p.e, gen::P_SND_SUSTAIN, 0.5f);
        p.e->setParameter(p.e, gen::P_SND_LENGTH, (500.0f - 0) / 2000.0f);
        p.e->setParameter(p.e, gen::P_SND_TEST, 1.0f);
        p.run(0.4);
        float held = 0; for (int i = 0; i < 128; ++i) held = std::max(held, std::fabs(p.l[(size_t)i]));
        p.run(0.6);
        float after = 0; for (int i = 0; i < 128; ++i) after = std::max(after, std::fabs(p.l[(size_t)i]));
        std::printf("  held tone %.3f, after release %.4f\n", held, after);
        CHECK(held > 0.1f && after < 0.001f);
        p.e->setParameter(p.e, gen::P_BLIP_VOL, 0.5f);
        p.e->setParameter(p.e, gen::P_SND_VOL, 0.3f); p.e->setParameter(p.e, gen::P_SND_SUSTAIN, 0.0f); p.e->setParameter(p.e, gen::P_SND_LENGTH, 0.0f);
        p.e->setParameter(p.e, gen::P_SND_TYPE, 0.0f);
    }

    // ---- a long piece, played through: the screen turns, the piece finishes -------------------
    {
        CHECK(openFolder(p, "Test"));
        p.e->setParameter(p.e, gen::P_ROW, 0.0f);
        p.e->setParameter(p.e, gen::P_LOAD, 1.0f);
        p.waitWorker(1.0);
        std::printf("loaded '%s'\n", p.text(gen::kTextBase + gen::T_TITLE).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_TITLE).find("ong scale") != std::string::npos);
        int bars = digits(p, gen::kD_d_bars, 3);
        CHECK(bars == 16);
        // an instrument an octave off: C5 twice where the music says C4
        p.block({{0x90, 72, 100}}); p.run(0.05);
        p.block({{0x90, 72, 100}}); p.waitWorker(0.5);
        std::printf("message '%s'\n", p.text(gen::kTextBase + gen::T_MESSAGE).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("Pitch Tol = Octave") != std::string::npos);
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        std::vector<int> seenFirstHalf;
        int errorsSeen = 0;
        for (size_t i = 0; i < longNotes.size(); ++i)
        {
            p.block({{0x90, (uint8_t)longNotes[i], 100}});
            p.run(0.08);
            p.block({{0x80, (uint8_t)longNotes[i], 0}});
            int m0 = digits(p, gen::kD_mnum, 3);
            if (seenFirstHalf.empty() || seenFirstHalf.back() != m0) seenFirstHalf.push_back(m0);
            errorsSeen = std::max(errorsSeen, digits(p, gen::kD_d_err, 3));
        }
        p.run(0.3);
        std::printf("first-half bar numbers:");
        for (int m : seenFirstHalf) std::printf(" %d", m);
        std::printf("; status %d, accuracy %d, errors %d\n", p.frame(gen::kD_status, gen::kStatusFrames), digits(p, gen::kD_d_acc, 3), errorsSeen);
        CHECK(seenFirstHalf.size() >= 3);                                // the halves were rewritten as the piece went on
        CHECK(seenFirstHalf.back() > 0);                                 // and the music stays on screen at the end
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 6);         // DONE
        CHECK(digits(p, gen::kD_d_acc, 3) == 100);
        CHECK(errorsSeen <= 0);
    }
    // ---- LISTEN: the piece in time, the cursor following; the practice position stays ----------
    {
        auto cursor = [&p]() { for (int sl = 0; sl < gen::kSlots; ++sl) if (p.frame(gen::kD_overlay + sl, gen::kOverlayFrames) == 1) return sl; return -1; };
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        p.e->setParameter(p.e, gen::P_TEMPO, 1.0f);                      // 200 %: 64 beats at 200 bpm, 19.2 s
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f);
        p.waitWorker(0.3);
        std::string msg = p.text(gen::kTextBase + gen::T_MESSAGE);
        std::printf("listen: status %d, message '%s'\n", p.frame(gen::kD_status, gen::kStatusFrames), msg.c_str());
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 10);        // LISTEN
        CHECK(msg.find("Listening at 200%") != std::string::npos);
        int last = cursor(), moves = 0; std::vector<int> halves; double secs = 0;
        while (secs < 25 && p.e->getParameter(p.e, gen::P_LISTEN) >= 0.5f)
        {
            p.run(0.05); secs += 0.05;
            int c = cursor(); if (c != last) { ++moves; last = c; }
            int m0 = digits(p, gen::kD_mnum, 3); if (halves.empty() || halves.back() != m0) halves.push_back(m0);
        }
        std::printf("listen: %.1f s, cursor moved %d times, first-half bars", secs, moves);
        for (int m : halves) std::printf(" %d", m);
        std::printf("\n");
        CHECK(secs > 17.5 && secs < 21.0);                                 // 19.2 s at 200 %
        CHECK(moves >= 60);
        CHECK(halves.size() >= 4);
        p.run(0.2);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 1);         // WAIT again
        CHECK(cursor() == 0);                                              // the practice cursor never moved
        CHECK(digits(p, gen::kD_d_err, 3) <= 0 && digits(p, gen::kD_d_acc, 3) < 0);
        // notes played while it plays are not graded; PLAY stops it
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f); p.run(0.3);
        p.block({{0x90, 61, 100}}); p.run(0.1);
        CHECK(digits(p, gen::kD_d_err, 3) <= 0);
        p.e->setParameter(p.e, gen::P_PLAY, 1.0f); p.run(0.1);
        CHECK(p.e->getParameter(p.e, gen::P_LISTEN) < 0.5f);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 1);
        // with Loop on (bar 1, one bar = 1.2 s) it goes round until stopped
        p.e->setParameter(p.e, gen::P_PLAY, 0.0f);
        p.e->setParameter(p.e, gen::P_LOOP_BARS, 0.0f);
        p.e->setParameter(p.e, gen::P_LOOP, 1.0f); p.run(0.1);
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f); p.run(3.0);
        CHECK(p.e->getParameter(p.e, gen::P_LISTEN) >= 0.5f);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 10);
        p.e->setParameter(p.e, gen::P_LISTEN, 0.0f); p.run(0.1);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 4);         // 0.5.5: LISTEN off pauses it
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f); p.run(0.2);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 10);        // and on again goes on
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.1);        // RESTART stops it
        CHECK(p.e->getParameter(p.e, gen::P_LISTEN) < 0.5f);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 9);         // LOOP (practice) again
        p.e->setParameter(p.e, gen::P_LOOP, 0.0f); p.run(0.1);
    }
    // ---- 0.5.0: the header's Bar, Note and BPM fields; loops ------------------------------------
    {
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        std::printf("bpm field %d at tempo %d%%\n", plainOf(p, gen::P_BPM), plainOf(p, gen::P_TEMPO));
        CHECK(plainOf(p, gen::P_TEMPO) == 200 && plainOf(p, gen::P_BPM) == 200);   // BPM follows Tempo %
        wheel(p, gen::P_GOTO_BAR, 3); p.run(0.1);                                   // Bar 1 -> 4
        std::printf("bar field: bar %d, note %d\n", digits(p, gen::kD_d_bar, 3), plainOf(p, gen::P_GOTO_STEP));
        CHECK(digits(p, gen::kD_d_bar, 3) == 4 && plainOf(p, gen::P_GOTO_STEP) == 13);
        wheel(p, gen::P_GOTO_STEP, -1); p.run(0.1);                                 // one note back, into bar 3
        CHECK(digits(p, gen::kD_d_bar, 3) == 3 && plainOf(p, gen::P_GOTO_BAR) == 3);
        p.block({{0x90, (uint8_t)longNotes[11], 100}}); p.run(0.05); p.block({{0x80, (uint8_t)longNotes[11], 0}}); p.run(0.05);
        CHECK(plainOf(p, gen::P_GOTO_STEP) == 13);                                  // played from there on
        setPlain(p, gen::P_BPM, 80); p.run(0.1);
        CHECK(plainOf(p, gen::P_TEMPO) == 80 && digits(p, gen::kD_d_bpm, 3) == 80);
        setPlain(p, gen::P_TEMPO, 150); p.run(0.1);
        CHECK(plainOf(p, gen::P_BPM) == 150);
        // loop: From moves To with it, To pulls From back, Bars stretches and stays in the piece
        setPlain(p, gen::P_LOOP_BARS, 4); p.run(0.05);                              // (LISTEN's test left it at 1)
        CHECK(plainOf(p, gen::P_LOOP_FROM) == 1 && plainOf(p, gen::P_LOOP_TO) == 4);
        wheel(p, gen::P_LOOP_FROM, 2); p.run(0.05);
        std::printf("loop %d-%d (%d bars)\n", plainOf(p, gen::P_LOOP_FROM), plainOf(p, gen::P_LOOP_TO), plainOf(p, gen::P_LOOP_BARS));
        CHECK(plainOf(p, gen::P_LOOP_FROM) == 3 && plainOf(p, gen::P_LOOP_TO) == 6 && plainOf(p, gen::P_LOOP_BARS) == 4);
        wheel(p, gen::P_LOOP_TO, -4); p.run(0.05);
        CHECK(plainOf(p, gen::P_LOOP_FROM) == 2 && plainOf(p, gen::P_LOOP_TO) == 2 && plainOf(p, gen::P_LOOP_BARS) == 1);
        setPlain(p, gen::P_LOOP_FROM, 15); p.run(0.05);
        setPlain(p, gen::P_LOOP_BARS, 4); p.run(0.05);
        std::printf("loop at the end: %d-%d (%d bars)\n", plainOf(p, gen::P_LOOP_FROM), plainOf(p, gen::P_LOOP_TO), plainOf(p, gen::P_LOOP_BARS));
        CHECK(plainOf(p, gen::P_LOOP_FROM) == 13 && plainOf(p, gen::P_LOOP_TO) == 16 && plainOf(p, gen::P_LOOP_BARS) == 4);
        setPlain(p, gen::P_TEMPO, 100); p.run(0.05);
        // Start = MPC Play: the MPC's playhead, moved while stopped, moves the practice position
        p.e->setParameter(p.e, gen::P_START_MODE, 1.0f);
        g_time.flags = kVstTempoValid | kVstPpqPosValid | kVstTimeSigValid; g_time.ppqPos = 0;
        g_time.timeSigNumerator = 4; g_time.timeSigDenominator = 4;
        p.run(0.1);
        g_time.ppqPos = 8.0; p.run(0.1);                                            // bar 3
        std::printf("mpc playhead at bar 3: bar %d, note %d\n", digits(p, gen::kD_d_bar, 3), plainOf(p, gen::P_GOTO_STEP));
        CHECK(digits(p, gen::kD_d_bar, 3) == 3 && plainOf(p, gen::P_GOTO_STEP) == 9);
        g_time.ppqPos = 9.0; p.run(0.1);                                            // a beat on
        CHECK(plainOf(p, gen::P_GOTO_STEP) == 10);
        g_time.ppqPos = 0.0; p.run(0.1);                                            // Stop twice: the start
        CHECK(digits(p, gen::kD_d_bar, 3) == 1 && plainOf(p, gen::P_GOTO_STEP) == 1);
        g_time.flags = kVstTempoValid; p.e->setParameter(p.e, gen::P_START_MODE, 0.0f); p.run(0.05);
    }
    // ---- 0.5.0: the LOAD list by data wheel; the piece list; Auto Next ---------------------------
    {
        CHECK(openFolder(p, "Queue"));
        p.e->setParameter(p.e, gen::P_PICK0, 1.0f); p.waitWorker(0.3);
        wheel(p, gen::P_SELECT, 1); p.waitWorker(0.3);
        std::printf("select %d row %d, highlight %d %d\n", plainOf(p, gen::P_SELECT), plainOf(p, gen::P_ROW),
                    p.frame(gen::kD_rowsel + 0, gen::kF_rowsel), p.frame(gen::kD_rowsel + 1, gen::kF_rowsel));
        CHECK(plainOf(p, gen::P_SELECT) == 1 && plainOf(p, gen::P_ROW) == 1);
        CHECK(p.frame(gen::kD_rowsel + 1, gen::kF_rowsel) == 1 && p.frame(gen::kD_rowsel + 0, gen::kF_rowsel) == 0);
        CHECK(p.e->getParameter(p.e, gen::P_PICK0) < 0.5f);
        p.e->setParameter(p.e, gen::P_LOAD, 1.0f); p.waitWorker(1.0);              // the wheel's push
        std::string t = p.text(gen::kTextBase + gen::T_TITLE);
        std::printf("title '%s'\n", t.c_str());
        CHECK(t.find("b second") != std::string::npos && t.find("(2/3)") != std::string::npos);
        // the piece list: open it, the wheel chooses, push loads
        p.e->setParameter(p.e, gen::P_QPANEL, 1.0f); p.waitWorker(0.3);
        std::printf("list: '%s' / '%s' / '%s'\n", p.text(gen::kTextBase + gen::T_QINFO).c_str(), p.text(gen::kTextBase + gen::T_Q0).c_str(),
                    p.text(gen::kTextBase + gen::T_Q2).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_Q0).find("a first") != std::string::npos);
        CHECK(p.frame(gen::kD_qsel + 1, gen::kF_qsel) == 2);
        wheel(p, gen::P_PIECE, 1); p.waitWorker(0.3);
        CHECK(p.frame(gen::kD_qsel + 2, gen::kF_qsel) == 2 && p.frame(gen::kD_qsel + 1, gen::kF_qsel) == 1);
        writeState(p, std::string(argv[3]) + ".queue.state.json");                 // the list open, for the docs
        p.e->setParameter(p.e, gen::P_PIECE_GO, 1.0f); p.waitWorker(1.0);
        CHECK(p.text(gen::kTextBase + gen::T_TITLE).find("(3/3)") != std::string::npos);
        CHECK(p.e->getParameter(p.e, gen::P_QPANEL) < 0.5f);                       // the list closed
        p.e->setParameter(p.e, gen::P_QPICK0, 1.0f); p.waitWorker(1.0);             // a tap on its first row
        CHECK(p.text(gen::kTextBase + gen::T_TITLE).find("(1/3)") != std::string::npos);
        // play it through: two seconds later the next one loads by itself
        p.run(0.2);
        for (int n : drill) { p.block({{0x90, (uint8_t)n, 100}}); p.run(0.05); p.block({{0x80, (uint8_t)n, 0}}); p.run(0.05); }
        p.run(2.5); p.waitWorker(1.0);
        std::printf("after the piece: '%s'\n", p.text(gen::kTextBase + gen::T_TITLE).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_TITLE).find("b second  (2/3)") != std::string::npos);
    }
    // ---- 0.5.0: the playlist ----------------------------------------------------------------------
    {
        CHECK(openFolder(p, "Queue"));
        p.e->setParameter(p.e, gen::P_PICK2, 1.0f); p.waitWorker(0.3);
        CHECK(plainOf(p, gen::P_SELECT) == 2);
        p.e->setParameter(p.e, gen::P_PL_ADD, 1.0f); p.waitWorker(0.5);
        p.e->setParameter(p.e, gen::P_PICK0, 1.0f); p.waitWorker(0.3);
        p.e->setParameter(p.e, gen::P_PL_ADD, 1.0f); p.waitWorker(0.5);
        std::printf("playlist: '%s'\n", p.text(gen::kTextBase + gen::T_MESSAGE).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("Added to Playlist (2") != std::string::npos);
        CHECK(openFolder(p, "Playlist"));
        CHECK(p.text(gen::kTextBase + gen::T_ROW0).find("c third") != std::string::npos);
        CHECK(p.text(gen::kTextBase + gen::T_ROW1).find("a first") != std::string::npos);
        p.e->setParameter(p.e, gen::P_PICK0, 1.0f); p.waitWorker(0.3);
        p.e->setParameter(p.e, gen::P_LOAD, 1.0f); p.waitWorker(1.0);
        CHECK(p.text(gen::kTextBase + gen::T_TITLE).find("c third  (1/2)") != std::string::npos);
        p.e->setParameter(p.e, gen::P_PL_REMOVE, 1.0f); p.waitWorker(0.5);
        CHECK(p.text(gen::kTextBase + gen::T_ROW0).find("a first") != std::string::npos);
        std::vector<uint8_t> pl;
        FILE* pf = std::fopen((std::string(argv[2]) + "/playlist.txt").c_str(), "rb");
        CHECK(pf != nullptr);
        if (pf) { int c; while ((c = std::fgetc(pf)) != EOF) pl.push_back((uint8_t)c); std::fclose(pf); }
        std::string pls(pl.begin(), pl.end());
        CHECK(pls.find("a_first.mid") != std::string::npos && pls.find("c_third.mid") == std::string::npos);
        p.e->setParameter(p.e, gen::P_PL_REMOVE, 1.0f); p.waitWorker(0.5);
        std::printf("after emptying it: folder '%s'\n", p.text(gen::kTextBase + gen::T_FOLDER_NAME).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_FOLDER_NAME).find("Playlist") == std::string::npos);
    }
    // ---- 0.5.0: notes beyond the staff under 8va / 8vb; TEST LIGHTS with nothing connected ---------
    {
        CHECK(openFolder(p, "Ottava"));
        p.e->setParameter(p.e, gen::P_PICK0, 1.0f); p.waitWorker(0.3);
        p.e->setParameter(p.e, gen::P_LOAD, 1.0f); p.waitWorker(1.0);
        p.run(0.3);
        int sign8va = mark(p, 0, 0), sign8vb = -1;
        for (int sl = 0; sl < gen::kSlots; ++sl) if (mark(p, sl, gen::kRows - 1) == gen::kMarkOttavaBase + 2) { sign8vb = sl; break; }
        std::printf("8va sign %d (want %d), 8vb sign in slot %d\n", sign8va, gen::kMarkOttavaBase, sign8vb);
        CHECK(sign8va == gen::kMarkOttavaBase);
        CHECK(sign8vb > 0);
        CHECK(mark(p, 1, 0) == gen::kMarkOttavaBase + 1);                           // the line goes on over the bar
        p.e->setParameter(p.e, gen::P_LINN_TEST, 1.0f); p.waitWorker(1.5);
        std::printf("test lights: '%s'\n", p.text(gen::kTextBase + gen::T_MESSAGE).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("nothing connected") != std::string::npos);
        CHECK(p.e->getParameter(p.e, gen::P_LINN_TEST) < 0.5f);
    }
    // ---- 0.5.5: the data wheel steps evenly ------------------------------------------------------
    {
        setPlain(p, gen::P_TEMPO, 100); p.run(0.02);
        wheel(p, gen::P_TEMPO, 1); p.run(0.02);
        std::printf("wheel: tempo %d", plainOf(p, gen::P_TEMPO));
        CHECK(plainOf(p, gen::P_TEMPO) == 101);
        wheel(p, gen::P_TEMPO, 1); wheel(p, gen::P_TEMPO, 1); p.run(0.02);
        std::printf(" -> %d", plainOf(p, gen::P_TEMPO));
        CHECK(plainOf(p, gen::P_TEMPO) == 103);
        // a fast turn: the MPC moves three steps at once, and that is what happens (0.6.0 jumped)
        p.e->setParameter(p.e, gen::P_TEMPO, stepNorm(gen::P_TEMPO, stepOf(p, gen::P_TEMPO) + 3)); p.run(0.02);
        CHECK(plainOf(p, gen::P_TEMPO) == 106);
        setPlain(p, gen::P_TIMING, 160); p.run(0.02);
        wheel(p, gen::P_TIMING, 1); p.run(0.02);
        std::printf(", timing %d", plainOf(p, gen::P_TIMING));
        CHECK(plainOf(p, gen::P_TIMING) == 165);                              // round 5 ms steps
        setPlain(p, gen::P_OUT_CH, 1); p.run(0.02);
        wheel(p, gen::P_OUT_CH, 1); p.run(0.02);
        std::printf(", out ch %d", plainOf(p, gen::P_OUT_CH));
        CHECK(plainOf(p, gen::P_OUT_CH) == 2);                                // one click, one channel
        // the MPC behind the plugin: BPM follows Tempo, so BPM changed under the MPC. Its click is
        // a step from the BPM it last sent; the plugin makes the step from the BPM shown
        setPlain(p, gen::P_BPM, 120); p.run(0.02);
        const int bpmBefore = stepOf(p, gen::P_BPM);
        setPlain(p, gen::P_TEMPO, 150); p.run(0.05);
        const int bpmShown = plainOf(p, gen::P_BPM);
        p.e->setParameter(p.e, gen::P_BPM, stepNorm(gen::P_BPM, bpmBefore + 1)); p.run(0.02);
        std::printf(", BPM %d -> %d (the MPC still at 120)", bpmShown, plainOf(p, gen::P_BPM));
        CHECK(bpmShown != 120 && plainOf(p, gen::P_BPM) == bpmShown + 1);
        // a preset is taken as it comes, even far from the value shown
        setPlain(p, gen::P_TIMING, 300); p.run(0.02);
        p.d(effSetProgram, 0, 3); p.run(0.02);                                 // Rhythm - strict: 120 ms
        std::printf(", preset timing %d\n", plainOf(p, gen::P_TIMING));
        CHECK(plainOf(p, gen::P_TIMING) == 120);
        p.d(effSetProgram, 0, 0); p.run(0.02);
        setPlain(p, gen::P_OUT_CH, 1); setPlain(p, gen::P_TIMING, 160); setPlain(p, gen::P_TEMPO, 100); p.run(0.02);
    }
    // ---- 0.5.5: LISTEN pauses, moves with the Bar field, and SET marks the loop --------------------
    {
        CHECK(openFolder(p, "Test"));
        p.e->setParameter(p.e, gen::P_PICK0, 1.0f); p.waitWorker(0.3);
        p.e->setParameter(p.e, gen::P_LOAD, 1.0f); p.waitWorker(1.0);
        p.e->setParameter(p.e, gen::P_LOOP, 0.0f);
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f); p.run(1.0);
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 10);
        p.e->setParameter(p.e, gen::P_LISTEN, 0.0f); p.waitWorker(0.3);
        int barPaused = digits(p, gen::kD_d_bar, 3);
        p.run(1.5);
        std::printf("listen paused at bar %d (still %d), status %d, message '%s'\n", barPaused, digits(p, gen::kD_d_bar, 3),
                    p.frame(gen::kD_status, gen::kStatusFrames), p.text(gen::kTextBase + gen::T_MESSAGE).c_str());
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 4 && digits(p, gen::kD_d_bar, 3) == barPaused);
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("Paused at bar") != std::string::npos);
        wheel(p, gen::P_GOTO_BAR, 2); p.run(0.1);                              // the Bar field moves what you hear
        std::printf("after two clicks on Bar: bar %d\n", digits(p, gen::kD_d_bar, 3));
        CHECK(digits(p, gen::kD_d_bar, 3) == barPaused + 2);
        // 0.6.5: the transport bar's arrows move what you hear by a bar too
        p.e->setParameter(p.e, gen::P_BAR_FWD, 1.0f); p.run(0.1);
        CHECK(digits(p, gen::kD_d_bar, 3) == barPaused + 3);
        CHECK(p.e->getParameter(p.e, gen::P_BAR_FWD) < 0.5f);                  // the button lets go
        p.e->setParameter(p.e, gen::P_BAR_BACK, 1.0f); p.run(0.1);
        std::printf("transport: >> then << from bar %d: bar %d\n", barPaused + 2, digits(p, gen::kD_d_bar, 3));
        CHECK(digits(p, gen::kD_d_bar, 3) == barPaused + 2);
        p.e->setParameter(p.e, gen::P_LOOP_SET_FROM, 1.0f); p.run(0.1);       // SET: the loop starts here
        CHECK(plainOf(p, gen::P_LOOP_FROM) == barPaused + 2);
        CHECK(p.e->getParameter(p.e, gen::P_LOOP_SET_FROM) < 0.5f);
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f); p.run(1.0);              // goes on from there
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 10 && digits(p, gen::kD_d_bar, 3) >= barPaused + 2);
        p.e->setParameter(p.e, gen::P_PLAY, 1.0f); p.run(0.1);                // PLAY stops it
        CHECK(p.e->getParameter(p.e, gen::P_LISTEN) < 0.5f);
        p.e->setParameter(p.e, gen::P_PLAY, 0.0f); p.run(0.1);
        // without LISTEN the arrows move the practice cursor, and stop at the first bar
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        const int bar0 = digits(p, gen::kD_d_bar, 3);
        p.e->setParameter(p.e, gen::P_BAR_FWD, 1.0f); p.run(0.1);
        p.e->setParameter(p.e, gen::P_BAR_FWD, 1.0f); p.run(0.1);
        CHECK(digits(p, gen::kD_d_bar, 3) == bar0 + 2);
        for (int i = 0; i < 4; ++i) { p.e->setParameter(p.e, gen::P_BAR_BACK, 1.0f); p.run(0.1); }
        std::printf("practice cursor: from bar %d, >> >> then << x4: bar %d\n", bar0, digits(p, gen::kD_d_bar, 3));
        CHECK(digits(p, gen::kD_d_bar, 3) == bar0);
    }
    // ---- 0.5.5: Fit Range = Fold moves notes the LinnStrument cannot play onto it -----------------
    {
        p.e->setParameter(p.e, gen::P_LINN, 1.0f); p.waitWorker(0.5);
        CHECK(openFolder(p, "Ottava"));
        p.e->setParameter(p.e, gen::P_PICK0, 1.0f); p.waitWorker(0.3);
        p.e->setParameter(p.e, gen::P_LOAD, 1.0f); p.waitWorker(1.0);
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        p.block({{0x90, 88, 100}, {0x90, 48, 90}}); p.run(0.05);              // E6 (on the surface: up to F6) and C3
        p.block({{0x90, 89, 100}}); p.run(0.05);                               // F6
        std::printf("next after F6 with Fold: frame %d (G5 %d, G6 %d)\n", p.frame(gen::kD_nextname, gen::kNoteNameFrames),
                    noteNameFrame(79), noteNameFrame(91));
        CHECK(p.frame(gen::kD_nextname, gen::kNoteNameFrames) == noteNameFrame(79));   // G6 folded to G5
        // the instrument an octave down (Linn Octave -1): the surface is F#0-F5, so E6 F6 G6 fold to E5 F5 G4
        setPlain(p, gen::P_LINN_FOLLOW, 0); setPlain(p, gen::P_LINN_OCT, -1); p.waitWorker(1.0);
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        p.block({{0x90, 76, 100}, {0x90, 48, 90}}); p.run(0.05);
        p.block({{0x90, 77, 100}}); p.run(0.05);
        std::printf("next with the LinnStrument an octave down: frame %d (G4 %d)\n", p.frame(gen::kD_nextname, gen::kNoteNameFrames), noteNameFrame(67));
        CHECK(p.frame(gen::kD_nextname, gen::kNoteNameFrames) == noteNameFrame(67));
        setPlain(p, gen::P_LINN_OCT, 0); setPlain(p, gen::P_LINN_FOLLOW, 1); p.waitWorker(0.5);
        p.e->setParameter(p.e, gen::P_LINN, 0.0f); p.waitWorker(1.0);         // Linn Mode off: as written again
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.2);
        p.block({{0x90, 88, 100}, {0x90, 48, 90}}); p.run(0.05);
        p.block({{0x90, 89, 100}}); p.run(0.05);
        CHECK(p.frame(gen::kD_nextname, gen::kNoteNameFrames) == noteNameFrame(91));
    }
    // ---- 0.6.0: the MIDI loader changes the library; the plugin rescans by itself ---------------
    {
        CHECK(openFolder(p, "Queue"));
        p.e->setParameter(p.e, gen::P_PICK1, 1.0f); p.waitWorker(0.3);
        CHECK(plainOf(p, gen::P_SELECT) == 1);                                   // b_second
        const int queueAt = plainOf(p, gen::P_FOLDER);
        // a new folder ahead of Queue and a new file ahead of b_second, then the marker
        writeMid(std::string(argv[2]) + "/Library/Aardvark", "new_piece", drill, {});
        writeMid(std::string(argv[2]) + "/Library/Queue", "a_zero", drill, {});
        FILE* mk = std::fopen((std::string(argv[2]) + "/Library/.rescan").c_str(), "wb");
        CHECK(mk != nullptr);
        if (mk) { std::fputs("1 test\n", mk); std::fclose(mk); }
        p.waitWorker(2.8);
        std::printf("after the loader: folder '%s' (was at %d, now %d), select %d, message '%s'\n",
                    p.text(gen::kTextBase + gen::T_FOLDER_NAME).c_str(), queueAt, plainOf(p, gen::P_FOLDER),
                    plainOf(p, gen::P_SELECT), p.text(gen::kTextBase + gen::T_MESSAGE).c_str());
        CHECK(p.text(gen::kTextBase + gen::T_FOLDER_NAME).find("Queue") != std::string::npos);
        CHECK(plainOf(p, gen::P_FOLDER) == queueAt + 1);                          // Aardvark came in ahead
        CHECK(plainOf(p, gen::P_SELECT) == 2);                                    // still b_second
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("Library updated from your computer") != std::string::npos);
        CHECK(p.text(gen::kTextBase + gen::T_ROW1).find("a zero") != std::string::npos);
        // without the marker the plugin does not look: a new file stays unseen ...
        writeMid(std::string(argv[2]) + "/Library/Queue", "a_aa", drill, {});
        p.waitWorker(2.6);
        CHECK(p.text(gen::kTextBase + gen::T_ROW0).find("a first") != std::string::npos);
        CHECK(plainOf(p, gen::P_SELECT) == 2);
        // ... until RESCAN, which keeps the place too
        p.e->setParameter(p.e, gen::P_RESCAN, 1.0f); p.waitWorker(0.6);
        std::printf("after RESCAN: row0 '%s', select %d\n", p.text(gen::kTextBase + gen::T_ROW0).c_str(), plainOf(p, gen::P_SELECT));
        CHECK(p.text(gen::kTextBase + gen::T_ROW0).find("a aa") != std::string::npos);
        CHECK(plainOf(p, gen::P_SELECT) == 3);                                    // still b_second
    }
    // ---- 0.7.0: one hand practised; the other plays in rhythm to MIDI Out (with the fake ALSA) -------
    if (g_fakeTime)
    {
        auto show = [&](const char* what, const std::vector<MidiLine>& o) {
            std::printf("%s:", what);
            for (const MidiLine& m : o) std::printf(" %s%d@%.2f", m.kind == "on" ? "+" : "-", m.note, m.t);
            std::printf("\n");
        };
        auto play = [&](int note, double hold, double after) {
            p.block({{0x90, (uint8_t)note, 100}}); p.run(hold); p.block({{0x80, (uint8_t)note, 0}}); p.run(after);
        };
        auto near = [](double t, double want) { return t >= 0 && std::fabs(t - want) < 0.06; };
        // Duet, 120 BPM, two bars: right-hand halves (C5 E5 | G5 C6), left-hand quarters on every
        // beat (C3 E3 G3 C4 | C3 E3 G3 C4), so a left-hand note falls with each of yours and one
        // between
        std::vector<SmfWriteTrack> tr(2);
        tr[0].name = "Right"; tr[1].name = "Left";
        const int rh[4] = {72, 76, 79, 84}, lh[4] = {48, 52, 55, 60};
        for (int i = 0; i < 4; ++i) tr[0].notes.push_back({i * 192, i * 192 + 192, (uint8_t)rh[i], 90});
        for (int i = 0; i < 8; ++i) tr[1].notes.push_back({i * 96, i * 96 + 96, (uint8_t)lh[i % 4], 70});
        // Intro, 3/4: two left-hand quarters, then the right hand comes in over a held left-hand C
        std::vector<SmfWriteTrack> ti(2);
        ti[0].name = "Right"; ti[1].name = "Left";
        ti[1].notes.push_back({0, 96, 43, 70}); ti[1].notes.push_back({96, 192, 47, 70}); ti[1].notes.push_back({192, 576, 48, 70});
        ti[0].notes.push_back({192, 384, 72, 90}); ti[0].notes.push_back({384, 576, 74, 90});
        std::string dir = std::string(argv[2]) + "/Library/Duet";
        if (std::system(("mkdir -p '" + dir + "'").c_str()) != 0) std::printf("mkdir failed\n");
        for (int k = 0; k < 2; ++k)
        {
            std::vector<uint8_t> bytes = k == 0 ? writeSmf(96, 120, 4, 4, 0, false, "Duet", tr) : writeSmf(96, 120, 3, 4, 0, false, "Intro", ti);
            FILE* mf = std::fopen((dir + (k == 0 ? "/duet.mid" : "/intro.mid")).c_str(), "wb");
            if (mf) { std::fwrite(bytes.data(), 1, bytes.size(), mf); std::fclose(mf); }
        }
        p.e->setParameter(p.e, gen::P_RESCAN, 1.0f); p.waitWorker(0.6);
        auto load = [&](int row) {
            CHECK(openFolder(p, "Duet"));
            p.e->setParameter(p.e, gen::P_PICK0 + row, 1.0f); p.waitWorker(0.3);
            p.e->setParameter(p.e, gen::P_LOAD, 1.0f); p.waitWorker(1.0);
        };
        load(0);
        p.e->setParameter(p.e, gen::P_LOOP, 0.0f);
        p.e->setParameter(p.e, gen::P_MODE, 0.0f);                              // Wait
        p.e->setParameter(p.e, gen::P_HANDS, 0.5f); p.waitWorker(0.3);          // Right
        setPlain(p, gen::P_TEMPO, 100);
        std::printf("accomp '%s', message '%s'\n", p.text(gen::P_ACCOMP).c_str(), p.text(gen::kTextBase + gen::T_MESSAGE).c_str());
        CHECK(p.text(gen::P_ACCOMP) == "Other Hand");
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("left hand on Sight Reader MIDI Out") != std::string::npos);
        // Wait: the left hand runs at tempo from your note, holds just before your next one, and goes
        // on when you play it
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.5);
        size_t from = traceCount();
        const double t1 = nowSec(); play(72, 0.2, 2.3);
        const double t2 = nowSec(); play(76, 0.2, 1.0);
        std::vector<MidiLine> out = traceLines(from);
        std::printf("Wait, right hand: C5 at %.2f, E5 at %.2f;", t1, t2); show(" MIDI Out", out);
        CHECK(near(whenOf(out, 48, "on"), t1));                                // C3 with your C5
        CHECK(near(whenOf(out, 52, "on"), t1 + 0.5));                          // E3 a beat later, in time
        CHECK(near(whenOf(out, 52, "off"), t2));                               // held while it waits for E5
        CHECK(near(whenOf(out, 55, "on"), t2));                                // G3 with your E5
        CHECK(near(whenOf(out, 60, "on"), t2 + 0.5));                          // C4 a beat later
        CHECK(whenOf(out, 76, "on") < 0 && whenOf(out, 72, "on") < 0);         // never your own hand
        // Flow: the clock carries it; when Flow holds for a note you missed, it holds too
        p.e->setParameter(p.e, gen::P_MODE, 0.5f);
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.5);
        from = traceCount();
        const double tp = nowSec();
        p.e->setParameter(p.e, gen::P_PLAY, 1.0f); p.run(2.06);                // a bar of count-in
        play(72, 0.2, 4.0);                                                      // C5 on the downbeat; no E5
        out = traceLines(from);
        std::printf("Flow: downbeat %.2f;", tp + 2.0); show(" MIDI Out", out);
        CHECK(near(whenOf(out, 48, "on"), tp + 2.0));                          // on the downbeat, with the clock
        CHECK(near(whenOf(out, 52, "on"), tp + 2.5));
        CHECK(near(whenOf(out, 55, "on"), tp + 3.0));                          // G3 on E5's beat: the clock is on time
        CHECK(whenOf(out, 60, "on") > tp + 4.2 && whenOf(out, 60, "on") < tp + 5.0);   // C4 after the hold for E5
        p.e->setParameter(p.e, gen::P_PLAY, 0.0f); p.run(0.2);
        p.e->setParameter(p.e, gen::P_MODE, 0.0f);
        // Hands = Left: the right hand plays, and the message says so
        p.e->setParameter(p.e, gen::P_HANDS, 1.0f); p.waitWorker(0.3);
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("right hand on Sight Reader MIDI Out") != std::string::npos);
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.5);
        from = traceCount();
        const double t3 = nowSec(); play(48, 0.2, 1.0);                        // C3, the first left-hand note
        out = traceLines(from);
        std::printf("Wait, left hand: C3 at %.2f;", t3); show(" MIDI Out", out);
        CHECK(near(whenOf(out, 72, "on"), t3));                                // the right hand's C5 with your C3
        CHECK(whenOf(out, 76, "on") < 0);                                      // E5 waits for your E3 and G3
        CHECK(whenOf(out, 48, "on") < 0);
        // Accomp Off says what it means
        p.e->setParameter(p.e, gen::P_ACCOMP, 0.0f); p.waitWorker(0.3);
        CHECK(p.text(gen::kTextBase + gen::T_MESSAGE).find("Accomp is Off") != std::string::npos);
        p.e->setParameter(p.e, gen::P_ACCOMP, 0.5f); p.waitWorker(0.2);
        p.e->setParameter(p.e, gen::P_HANDS, 0.5f); p.waitWorker(0.2);
        // A left-hand intro, started by just playing: the notes that are over are passed over (0.5.5
        // sent them all at once); the one still sounding comes in with you
        load(1);
        std::printf("loaded '%s'\n", p.text(gen::kTextBase + gen::T_TITLE).c_str());
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.5);
        from = traceCount();
        const double t4 = nowSec(); play(72, 0.2, 1.0);
        out = traceLines(from);
        std::printf("Intro without PLAY: C5 at %.2f;", t4); show(" MIDI Out", out);
        CHECK(whenOf(out, 43, "on") < 0 && whenOf(out, 47, "on") < 0);
        CHECK(near(whenOf(out, 48, "on"), t4));
        // ... and started with PLAY, the intro plays in time and waits for your first note
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.5);
        from = traceCount();
        const double t5 = nowSec(); p.e->setParameter(p.e, gen::P_PLAY, 1.0f); p.run(2.0);
        const double t6 = nowSec(); play(72, 0.2, 1.0);
        out = traceLines(from);
        std::printf("Intro with PLAY at %.2f, C5 at %.2f;", t5, t6); show(" MIDI Out", out);
        CHECK(near(whenOf(out, 43, "on"), t5) && near(whenOf(out, 47, "on"), t5 + 0.5));
        CHECK(near(whenOf(out, 48, "on"), t6));
        p.e->setParameter(p.e, gen::P_PLAY, 0.0f); p.run(0.2);
        // LISTEN paused, then a note: practice takes over at the bar LISTEN was at, and the other hand
        // comes in with you
        load(0);
        p.e->setParameter(p.e, gen::P_RESTART, 1.0f); p.run(0.3);
        p.e->setParameter(p.e, gen::P_LISTEN, 1.0f); p.run(0.6);
        p.e->setParameter(p.e, gen::P_LISTEN, 0.0f); p.waitWorker(0.3);       // pause in bar 1
        p.e->setParameter(p.e, gen::P_BAR_FWD, 1.0f); p.waitWorker(0.3);      // >>: bar 2, still paused
        const int pausedBar = digits(p, gen::kD_d_bar, 3);
        from = traceCount();
        const double t7 = nowSec(); play(79, 0.2, 1.0);                        // G5, bar 2's first right-hand note
        out = traceLines(from);
        std::printf("After LISTEN paused at bar %d: G5 at %.2f, LISTEN %s, bar %d;", pausedBar, t7,
                    p.e->getParameter(p.e, gen::P_LISTEN) < 0.5f ? "off" : "on", digits(p, gen::kD_d_bar, 3));
        show(" MIDI Out", out);
        CHECK(pausedBar == 2);
        CHECK(p.e->getParameter(p.e, gen::P_LISTEN) < 0.5f);
        CHECK(digits(p, gen::kD_d_bar, 3) == 2);                               // practice went on at bar 2
        CHECK(near(whenOf(out, 48, "on"), t7));                                // bar 2's C3 with your G5
        CHECK(near(whenOf(out, 52, "on"), t7 + 0.5));                          // then E3 in time
        // A project saved before 0.7.0 with Accomp Off gets Other Hand; a newer one keeps Off
        p.e->setParameter(p.e, gen::P_ACCOMP, 0.0f); p.run(0.05);
        std::vector<uint8_t> chunk = getChunk(p);
        setChunk(p, chunk); p.run(0.05);
        CHECK(p.text(gen::P_ACCOMP) == "Off");
        std::vector<uint8_t> old = withoutVersion(chunk);
        CHECK(old.size() < chunk.size());
        setChunk(p, old); p.run(0.05);
        std::printf("a pre-0.7.0 project with Accomp Off loads as '%s'\n", p.text(gen::P_ACCOMP).c_str());
        CHECK(p.text(gen::P_ACCOMP) == "Other Hand");
        p.waitWorker(1.0);
        p.e->setParameter(p.e, gen::P_HANDS, 0.0f); p.waitWorker(0.2);
    }
    std::printf("automate %d updateDisplay %d\n", g_automate, g_updates);
    p.d(effMainsChanged, 0, 0); p.d(effClose);
    std::printf("plugin_test: %s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
