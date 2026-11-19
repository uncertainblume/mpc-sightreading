// plugin_test - drives the built plugin through its VST2 interface like the MPC would, with a
// library on disk, and checks what it shows. Writes the screen as JSON for
// tools/skin/preview.py, so the PLAY page can be looked at without an MPC.
//
//   plugin_test <plugin.so> <home dir with Library/> <out.json>
#include "mpc_vst2_abi.h"
#include "sr/gen/layout_gen.h"
#include "sr/gen/params_gen.h"

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
    std::fprintf(f, "]}");
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
    for (int fi = 0; fi < 40; ++fi)
    {
        if (p.text(gen::kTextBase + gen::T_FOLDER_NAME).find("Classics") != std::string::npos) break;
        p.e->setParameter(p.e, gen::P_FOLDER, (fi + 1) / 63.0f);
        p.waitWorker(0.3);
    }
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

    FILE* f = std::fopen(argv[3], "w");
    std::fprintf(f, "{\"screens\": [");
    dump(p, f);
    // play the first two steps right (D5 with the LH chord G3 B3 D4), then a wrong note
    p.block({{0x90, 74, 100}, {0x90, 55, 90}, {0x90, 59, 90}, {0x90, 62, 90}});
    p.run(0.1);
    p.block({{0x90, 67, 100}});                                         // G4
    p.run(0.1);
    p.block({{0x90, 61, 100}});                                         // wrong
    p.run(0.05);
    int flash = p.frame(gen::kD_flash, gen::kFlashFrames);
    std::printf("after wrong note: flash %d, errors %d, accuracy %d, next-name frame %d\n", flash, digits(p, gen::kD_d_err, 3),
                digits(p, gen::kD_d_acc, 3), p.frame(gen::kD_nextname, gen::kNoteNameFrames));
    CHECK(flash == 1);
    CHECK(digits(p, gen::kD_d_err, 3) == 1);
    std::fprintf(f, ",\n");
    dump(p, f);
    // the rest of the first bar, to see the played overlay
    p.block({{0x90, 69, 100}}); p.run(0.05);                            // A4
    p.block({{0x90, 71, 100}, {0x90, 57, 90}}); p.run(0.05);            // B4 + A3
    p.block({{0x90, 72, 100}}); p.run(0.3);                             // C5
    std::printf("accuracy after the bar: %d\n", digits(p, gen::kD_d_acc, 3));
    CHECK(digits(p, gen::kD_d_acc, 3) == 90);                           // 9 right, 1 wrong
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
    std::printf("automate %d updateDisplay %d\n", g_automate, g_updates);
    p.d(effMainsChanged, 0, 0); p.d(effClose);
    std::printf("plugin_test: %s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
