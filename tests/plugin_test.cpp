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

static int digits(Plug& p, int base, int n)
{
    int v = 0; bool any = false;
    for (int i = 0; i < n; ++i) { int fr = p.frame(base + i, gen::kDigitFrames); if (fr > 0) { v = v * 10 + (fr - 1); any = true; } else if (any) v *= 10; }
    return any ? v : -1;
}

int main(int argc, char** argv)
{
    if (argc < 4) { std::printf("usage: plugin_test plugin.so home out.json\n"); return 2; }
    setenv("SIGHTREADER_HOME", argv[2], 1);
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
    std::string statePath = std::string(argv[3]) + ".state.json";
    FILE* sf = std::fopen(statePath.c_str(), "w");
    auto esc = [](const std::string& t) { std::string o; for (char c : t) { if (c == '"' || c == '\\') o.push_back('\\'); if ((unsigned char)c >= 32) o.push_back(c); } return o; };
    std::fprintf(sf, "{\"values\": [");
    for (int i = 0; i < gen::kNumParams; ++i) std::fprintf(sf, "%s%.6f", i ? "," : "", (double)p.e->getParameter(p.e, i));
    std::fprintf(sf, "], \"text\": {");
    for (int i = 0; i < gen::kDisplayBase; ++i) std::fprintf(sf, "%s\"%d\": \"%s\"", i ? "," : "", i, esc(p.text(i)).c_str());
    std::fprintf(sf, "}, \"name\": {");
    for (int i = 0; i < gen::kTextBase; ++i) { char b[256] = {0}; p.d(effGetParamName, i, 0, b); std::fprintf(sf, "%s\"%d\": \"%s\"", i ? "," : "", i, esc(b).c_str()); }
    std::fprintf(sf, "}}\n");
    std::fclose(sf);
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
    CHECK(std::fabs(p.e->getParameter(p.e, gen::P_COL_C) - cyan / 15.0f) < 0.01f);
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
        CHECK(p.frame(gen::kD_status, gen::kStatusFrames) == 9);         // LOOP (practice) again
        p.e->setParameter(p.e, gen::P_LOOP, 0.0f); p.run(0.1);
    }
    std::printf("automate %d updateDisplay %d\n", g_automate, g_updates);
    p.d(effMainsChanged, 0, 0); p.d(effClose);
    std::printf("plugin_test: %s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
