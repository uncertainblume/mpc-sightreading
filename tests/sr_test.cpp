// sr_test - desktop checks for the Sight Reader core (no SDK, no MPC needed).
//
//   sr_test selftest                       unit tests (exit code 1 on failure)
//   sr_test dump <file.mid> <out.json> [--pages N] [--transpose N]
//                                          lay a file out and write the first N screens for
//                                          tools/skin/preview.py
//   sr_test info <file.mid>                what the loader made of a file
#include "sr/smf.h"
#include "sr/music.h"
#include "sr/score.h"
#include "sr/engine.h"
#include "sr/generator.h"
#include "sr/inflate.h"
#include "sr/files.h"
#include "sr/linn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <array>

using namespace sr;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::vector<uint8_t> readFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void dumpScreens(const Score& sc, const char* out, int pages)
{
    FILE* f = std::fopen(out, "w");
    if (!f) { std::perror(out); return; }
    std::fprintf(f, "{\"screens\": [\n");
    int start = 0;
    for (int p = 0; p < pages && start < (int)sc.slots.size(); ++p)
    {
        // whole measures that fit in the screen
        int end = start;
        while (end < (int)sc.slots.size())
        {
            int m = sc.slots[(size_t)end].measure;
            int mEnd = sc.measures[(size_t)m].firstSlot + sc.measures[(size_t)m].numSlots;
            if (mEnd - start > gen::kSlots) { if (end == start) end = start + gen::kSlots; break; }
            end = mEnd;
        }
        if (end > (int)sc.slots.size()) end = (int)sc.slots.size();
        std::fprintf(f, "%s{\"keysig\": %d, \"timesig\": %d, \"cells\": [", p ? ",\n" : "", sc.keySf, sc.timeSigFrame());
        for (int s = 0; s < gen::kSlots; ++s)
        {
            std::fprintf(f, "%s[", s ? "," : "");
            for (int r = 0; r < gen::kRows; ++r)
                std::fprintf(f, "%s%d", r ? "," : "", (start + s < end) ? sc.slots[(size_t)(start + s)].cells[r] : 0);
            std::fprintf(f, "]");
        }
        std::fprintf(f, "], \"barlines\": [");
        for (int s = 0; s <= gen::kSlots; ++s)
        {
            int fr = 0;
            if (s > 0 && start + s < end) fr = sc.slots[(size_t)(start + s)].barline;
            if (start + s == end && end == (int)sc.slots.size()) fr = 3;           // final
            else if (start + s == end && s > 0) fr = 1;
            std::fprintf(f, "%s%d", s ? "," : "", fr);
        }
        std::fprintf(f, "], \"overlays\": [");
        for (int s = 0; s < gen::kSlots; ++s) std::fprintf(f, "%s%d", s ? "," : "", (p == 0 && s == 1) ? 1 : 0);
        std::fprintf(f, "]}");
        start = end;
    }
    std::fprintf(f, "\n]}\n");
    std::fclose(f);
}

static int selftest()
{
    // --- spelling ------------------------------------------------------------------------------
    Spelling s = spellPitch(61, 2, false);   // C# in D major
    CHECK(s.letter == 0 && s.acc == 1 && s.octave == 4);
    s = spellPitch(70, -1, false);           // Bb in F major
    CHECK(s.letter == 6 && s.acc == -1 && s.octave == 4);
    s = spellPitch(68, 0, true);             // G# in A minor (leading tone)
    CHECK(s.letter == 4 && s.acc == 1);
    s = spellPitch(61, -1, true);            // C# in D minor (leading tone)
    CHECK(s.letter == 0 && s.acc == 1);
    s = spellPitch(70, 0, false);            // Bb in C major
    CHECK(s.letter == 6 && s.acc == -1);
    s = spellPitch(60, 0, false);
    CHECK(staffPosition(s, 0) == -2);        // middle C below the treble staff
    CHECK(staffPosition(s, 1) == 10);        // and above the bass staff
    s = spellPitch(43, 0, false);            // G2: bottom line of the bass staff
    CHECK(staffPosition(s, 1) == 0);
    int root; uint16_t mask;
    CHECK(parseKeyName("D# Natural Minor", root, mask) && root == 3 && mask == 0x5AD);
    CHECK(parseKeyName("C Major", root, mask) && root == 0 && mask == 0xAB5);
    CHECK(parseKeyName("Bb Dorian", root, mask) && root == 10);

    // --- a two-hand file round trip ------------------------------------------------------------
    std::vector<SmfWriteTrack> tr(2);
    tr[0].name = "Right"; tr[1].name = "Left";
    int ppq = 96;
    // RH: C D E F (quarters) | G half, A 8th B 8th... ; LH: C whole
    int rh[] = {72, 74, 76, 77};
    for (int i = 0; i < 4; ++i) tr[0].notes.push_back({i * ppq, (i + 1) * ppq, (uint8_t)rh[i], 80});
    tr[0].notes.push_back({4 * ppq, 6 * ppq, 79, 80});
    tr[0].notes.push_back({6 * ppq, 6 * ppq + ppq / 2, 81, 80});
    tr[0].notes.push_back({6 * ppq + ppq / 2, 7 * ppq, 83, 80});
    tr[0].notes.push_back({7 * ppq, 8 * ppq + ppq, 84, 80});    // tied over the barline
    tr[1].notes.push_back({0, 4 * ppq, 48, 70});
    tr[1].notes.push_back({4 * ppq, 8 * ppq, 43, 70});
    tr[1].notes.push_back({4 * ppq, 8 * ppq, 55, 70});
    std::vector<uint8_t> bytes = writeSmf(ppq, 90, 4, 4, 0, false, "Test", tr);
    Smf smf; std::string err;
    CHECK(parseSmf(bytes.data(), bytes.size(), smf, err));
    CHECK(smf.ppq == ppq);
    CHECK(smf.notes.size() == 11);
    CHECK(smf.title == "Test");
    Score sc; BuildOptions opt;
    CHECK(buildScore(smf, opt, sc, err));
    CHECK(sc.measures.size() == 3);
    CHECK(sc.notes.size() == 11);
    CHECK(sc.steps.size() == 8);             // onsets: 0 1 2 3 4 6 6.5 7 (beats)
    CHECK((int)sc.bpm == 90);
    int lh = 0; for (const PlayNote& n : sc.notes) lh += n.hand == 1;
    CHECK(lh == 3);
    CHECK(sc.steps[0].count == 2 && sc.steps[0].hands == 3);
    CHECK(sc.steps.back().time == 7 * kUPQ);
    for (const Step& st : sc.steps) CHECK(sc.slots[(size_t)st.slot].time == st.time);
    // every cell frame is inside its strip
    for (const Slot& sl : sc.slots)
        for (int r = 0; r < gen::kRows; ++r)
        {
            int cls = gen::kRowClass[r];
            int n = cls == gen::kSpace ? gen::kFrames_space : (cls == gen::kLine ? gen::kFrames_line : gen::kFrames_ledger);
            CHECK(sl.cells[r] < n);
        }
    // --- engine: wait mode ----------------------------------------------------------------------
    {
        std::vector<uint8_t> stepState(sc.steps.size()), noteState(sc.notes.size());
        Engine eng; EngineSettings st; st.mode = 0;
        eng.setScore(&sc, stepState.data(), noteState.data());
        Engine::NoteIn in[2];
        eng.process(st, in, 0, 128, 44100);
        CHECK(eng.currentStep() == 0);
        in[0] = {0, 72, true}; eng.process(st, in, 1, 128, 44100);     // first half of the C chord
        CHECK(eng.currentStep() == 0);
        in[0] = {0, 49, true}; eng.process(st, in, 1, 128, 44100);     // wrong note
        CHECK(eng.wrongs() == 1);
        in[0] = {0, 48, true}; eng.process(st, in, 1, 128, 44100);     // completes step 0
        CHECK(eng.currentStep() == 1);
        in[0] = {0, 74, true}; in[1] = {5, 76, true};
        eng.process(st, in, 2, 128, 44100);                              // D then E: two steps
        CHECK(eng.currentStep() == 3);
        CHECK(eng.hits() == 4);
        // right hand only: the left-hand steps are skipped
        st.hands = 1; eng.restart(); eng.process(st, in, 0, 128, 44100);
        in[0] = {0, 72, true}; eng.process(st, in, 1, 128, 44100);
        CHECK(eng.currentStep() == 1);
        // octave tolerance
        st.hands = 0; st.pitchTol = 1; eng.restart();
        in[0] = {0, 84, true}; in[1] = {1, 36, true}; eng.process(st, in, 2, 128, 44100);
        CHECK(eng.currentStep() == 1);
        // chord tolerance "any": one note of the chord is enough
        st.pitchTol = 0; st.chordTol = 2; eng.restart();
        in[0] = {0, 48, true}; eng.process(st, in, 1, 128, 44100);
        CHECK(eng.currentStep() == 1);
        // a left-hand-only setting on everything works without recursing
        st.chordTol = 0; st.hands = 2; st.loop = true; st.loopFrom = 2; st.loopBars = 1; eng.restart();
        eng.process(st, in, 0, 128, 44100);
        CHECK(eng.currentStep() >= 0);
    }
    // --- engine: rhythm mode -----------------------------------------------------------------
    {
        std::vector<uint8_t> stepState(sc.steps.size()), noteState(sc.notes.size());
        Engine eng; EngineSettings st; st.mode = 2; st.countInBars = 0; st.startMode = 1;
        eng.setScore(&sc, stepState.data(), noteState.data());
        eng.setPlaying(true, st);
        Engine::NoteIn in[2];
        // 90 bpm: one beat = 0.6667 s = 29400 samples. Play C chord on time, skip D, play E late-ish.
        in[0] = {0, 72, true}; in[1] = {0, 48, true};
        eng.process(st, in, 2, 128, 44100);
        int frames = 0;
        while (frames < 29400 * 2 + 1500) { eng.process(st, in, 0, 128, 44100); frames += 128; }
        in[0] = {0, 76, true}; eng.process(st, in, 1, 128, 44100);      // E at beat 2 + ~34 ms
        CHECK(eng.hits() == 3);
        CHECK(eng.misses() == 1);                                         // D was missed
        while (frames < 29400 * 13) { eng.process(st, in, 0, 128, 44100); frames += 128; }
        CHECK(eng.state() == RunState::Done);
    }
    // --- LinnStrument layout (firmware: note = lowest + row * offset + col - 1) ------------------
    {
        LinnLayout L;                                  // 200, fourths, lowest F#1 (30)
        CHECK(L.note(1, 0) == 30 && L.note(25, 0) == 54 && L.note(1, 1) == 35 && L.note(1, 7) == 65);
        CHECK(L.colFor(60, 0) == -1 && L.colFor(60, 2) == 21 && L.colFor(60, 6) == 1);
        L.leftHanded = true;
        CHECK(L.note(25, 0) == 30 && L.colFor(30, 0) == 25);
        L.leftHanded = false; L.cols = 16;
        CHECK(L.colFor(50, 0) == -1 && L.colFor(45, 0) == 16);
        // Fingering a C major scale in the right hand: one finger per column, rising
        std::vector<SmfWriteTrack> t(1); t[0].name = "Right";
        int scale[8] = {60, 62, 64, 65, 67, 69, 71, 72};
        for (int i = 0; i < 8; ++i) t[0].notes.push_back({i * 96, (i + 1) * 96, (uint8_t)scale[i], 80});
        std::vector<uint8_t> b = writeSmf(96, 100, 4, 4, 0, false, "Scale", t);
        Smf sm; Score s2; std::string e3;
        CHECK(parseSmf(b.data(), b.size(), sm, e3) && buildScore(sm, BuildOptions(), s2, e3));
        LinnLayout L2; Fingering fg; planFingering(s2, L2, fg);
        int jumps = 0;
        for (size_t i = 0; i < s2.notes.size(); ++i)
        {
            CHECK(fg.col[i] > 0 && fg.finger[i] >= 1 && fg.finger[i] <= 5);
            CHECK(L2.note(fg.col[i], fg.row[i]) == s2.notes[i].pitch);
            if (i && std::abs(fg.col[i] - fg.col[i - 1]) > 4) ++jumps;
        }
        CHECK(jumps == 0);
        // Light compositor: one note in a colour, then cleared
        LinnLights lights; std::vector<std::array<uint8_t, 3>> sent;
        lights.setNote(1, L2, 60, kLinnBlue);
        lights.flush(1000, [&](const uint8_t* m) { sent.push_back({m[0], m[1], m[2]}); });
        int painted = 0; for (auto& m : sent) if (m[1] == 22 && m[2] == kLinnBlue) ++painted;
        int cells = 0; for (int r = 0; r < 8; ++r) if (L2.colFor(60, r) > 0) ++cells;
        CHECK(painted == cells && cells >= 3);
        sent.clear(); lights.clearLayer(1);
        lights.flush(1000, [&](const uint8_t* m) { sent.push_back({m[0], m[1], m[2]}); });
        int off = 0; for (auto& m : sent) if (m[1] == 22 && m[2] == kLinnOff) ++off;
        CHECK(off == cells);
        sent.clear(); lights.flush(1000, [&](const uint8_t* m) { sent.push_back({m[0], m[1], m[2]}); });
        CHECK(sent.empty());                          // nothing changes, nothing is sent
        std::printf("fingering:");
        for (size_t i = 0; i < s2.notes.size(); ++i) std::printf(" %d@c%dr%d", fg.finger[i], fg.col[i], fg.row[i]);
        std::printf("\n");
    }
    // --- generator: every combination parses and lays out ---------------------------------------
    for (int sc_i = 0; sc_i < kNumGenScales; ++sc_i)
        for (int meter = 0; meter < 4; ++meter)
            for (int hands = 0; hands < 3; ++hands)
            {
                GenSettings gs; gs.scaleIndex = sc_i; gs.meter = meter; gs.hands = hands; gs.keyIndex = (sc_i * 5 + meter) % 13;
                gs.rhythm = 1 + (sc_i + meter) % 5; gs.leaps = 1 + (sc_i + hands) % 5; gs.lhStyle = (sc_i + meter + hands) % 4;
                gs.range = (sc_i + hands) % 4; gs.seed = 1234u + (uint32_t)(sc_i * 100 + meter * 10 + hands);
                std::string title, desc;
                std::vector<uint8_t> mid = generateExercise(gs, title, desc);
                Smf g; Score gsco; std::string e2;
                bool ok = parseSmf(mid.data(), mid.size(), g, e2) && buildScore(g, BuildOptions(), gsco, e2);
                CHECK(ok);
                if (ok) CHECK((int)gsco.measures.size() >= gs.bars - 1 && (int)gsco.measures.size() <= gs.bars + 1);
            }
    std::printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "selftest")) return selftest();
    if (argc >= 3 && !std::strcmp(argv[1], "gunzip"))
    {   // sr_test gunzip file.gz : prints the size and first bytes, and key fields of an .xpj
        std::vector<uint8_t> raw = readFile(argv[2]), out;
        bool ok = gunzip(raw.data(), raw.size(), out);
        std::string text(out.begin(), out.end());
        size_t data = text.find("\"data\"");
        std::printf("ok %d size %zu key '%s' scaleType '%s'\n", ok, out.size(), jsonStringAfter(text, "key", data == std::string::npos ? 0 : data).c_str(),
                    jsonStringAfter(text, "scaleType", text.find("padPerformSettings")).c_str());
        return ok ? 0 : 1;
    }
    if (argc >= 3 && !std::strcmp(argv[1], "gen"))
    {   // sr_test gen out.mid key scale rhythm leaps hands bars meter range lh seed
        GenSettings gs; int* f[] = {&gs.keyIndex, &gs.scaleIndex, &gs.rhythm, &gs.leaps, &gs.hands, &gs.bars, &gs.meter, &gs.range, &gs.lhStyle};
        for (int i = 0; i < 9 && 3 + i < argc; ++i) *f[i] = std::atoi(argv[3 + i]);
        if (argc > 12) gs.seed = (uint32_t)std::atoi(argv[12]);
        std::string title, desc;
        std::vector<uint8_t> mid = generateExercise(gs, title, desc);
        FILE* o = std::fopen(argv[2], "wb"); if (!o) return 1; std::fwrite(mid.data(), 1, mid.size(), o); std::fclose(o);
        std::printf("%s - %s\n", title.c_str(), desc.c_str());
        return 0;
    }
    if (argc >= 3 && (!std::strcmp(argv[1], "dump") || !std::strcmp(argv[1], "info")))
    {
        std::vector<uint8_t> data = readFile(argv[2]);
        Smf smf; std::string err;
        if (!parseSmf(data.data(), data.size(), smf, err)) { std::printf("parse error: %s\n", err.c_str()); return 1; }
        BuildOptions opt; int pages = 4;
        for (int i = 3; i < argc; ++i)
        {
            if (!std::strcmp(argv[i], "--pages") && i + 1 < argc) pages = std::atoi(argv[++i]);
            if (!std::strcmp(argv[i], "--transpose") && i + 1 < argc) opt.transpose = std::atoi(argv[++i]);
        }
        Score sc;
        if (!buildScore(smf, opt, sc, err)) { std::printf("build error: %s\n", err.c_str()); return 1; }
        std::printf("title '%s' ppq %d grid %d  notes %zu steps %zu measures %zu slots %zu  key %d%s  %d/%d  %.0f bpm  RH %d-%d LH %d-%d\n",
                    sc.title.c_str(), smf.ppq, sc.grid, sc.notes.size(), sc.steps.size(), sc.measures.size(), sc.slots.size(),
                    sc.keySf, sc.minor ? "m" : "", sc.tsNum, sc.tsDen, sc.bpm, sc.low[0], sc.high[0], sc.low[1], sc.high[1]);
        for (size_t t = 0; t < smf.tracks.size(); ++t)
            std::printf("  track %zu '%s' notes %d avg %.1f\n", t, smf.tracks[t].name.c_str(), smf.tracks[t].notes,
                        smf.tracks[t].notes ? smf.tracks[t].pitchSum / smf.tracks[t].notes : 0.0);
        if (!std::strcmp(argv[1], "dump") && argc >= 4) dumpScreens(sc, argv[3], pages);
        return 0;
    }
    std::printf("usage: sr_test selftest | dump <file.mid> <out.json> [--pages N] | info <file.mid>\n");
    return 2;
}
