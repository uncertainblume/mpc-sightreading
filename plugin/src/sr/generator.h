// generator.h - new sight-reading exercises on demand, so there is always unseen music.
//
// A melody is a random walk on the scale's degrees: step sizes come from the Leaps level,
// note values from rhythm patterns for the Rhythm level, phrases of four bars end on a stable
// degree (1, 3 or 5), and the last bar holds the tonic. The left hand follows a I-IV-V-I style
// progression as single notes, intervals, block triads or broken chords.
//
// SPDX-License-Identifier: MIT
#ifndef SR_GENERATOR_H
#define SR_GENERATOR_H

#include "music.h"
#include "smf.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace sr {

struct GenSettings
{
    int keyIndex = 0;        // 0..11 in the order C G D A E B F# Db Ab Eb Bb F; 12 = random
    int scaleIndex = 0;      // see kGenScales
    int rhythm = 2;          // 1..5
    int leaps = 2;           // 1..5
    int hands = 0;           // 0 right, 1 left, 2 both
    int bars = 8;
    int meter = 0;           // 0 4/4, 1 3/4, 2 2/4, 3 6/8
    int range = 1;           // 0 five notes, 1 octave, 2 two octaves, 3 wide
    int lhStyle = 0;         // 0 single, 1 intervals, 2 triads, 3 broken
    uint32_t seed = 1;
};

struct GenScale { const char* name; uint16_t mask; };
constexpr GenScale kGenScales[] = {
    {"Major", 0xAB5}, {"Minor", 0x5AD}, {"Dorian", 0x6AD}, {"Phrygian", 0x5AB}, {"Lydian", 0xAD5},
    {"Mixolydian", 0x6B5}, {"Locrian", 0x56B}, {"Harm Minor", 0x9AD}, {"Maj Pent", 0x295},
    {"Min Pent", 0x4A9}, {"Blues", 0x4E9}, {"Chromatic", 0xFFF}};
constexpr int kNumGenScales = (int)(sizeof(kGenScales) / sizeof(kGenScales[0]));

struct Rng
{
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int n) { return n > 0 ? (int)(next() % (uint32_t)n) : 0; }
    double unit() { return (next() & 0xFFFFFF) / 16777216.0; }
};

// Rhythm patterns per beat group, in 16ths (simple meters: one quarter = 4) or, for 6/8, in
// 16ths of a dotted-quarter group (6). Negative values are rests.
namespace gendetail {
struct Pat { int level; int len; int v[8]; int n; };
// Patterns that fill one half bar of 4/4 (8 sixteenths) - combined to fill bars.
const Pat kHalf[] = {
    {1, 8, {8}, 1},
    {2, 8, {4, 4}, 2},
    {3, 8, {2, 2, 4}, 3}, {3, 8, {4, 2, 2}, 3}, {3, 8, {2, 2, 2, 2}, 4},
    {4, 8, {6, 2}, 2}, {4, 8, {4, -4}, 2}, {4, 8, {2, 4, 2}, 3}, {4, 8, {-2, 2, 4}, 3},
    {5, 8, {1, 1, 2, 4}, 4}, {5, 8, {3, 1, 4}, 3}, {5, 8, {2, 1, 1, 2, 2}, 5}, {5, 8, {1, 1, 1, 1, 4}, 5},
};
const Pat kQuarter[] = {   // one beat, for 3/4 and 2/4 bars
    {1, 4, {4}, 1}, {2, 4, {4}, 1}, {3, 4, {2, 2}, 2}, {4, 4, {3, 1}, 2}, {4, 4, {-2, 2}, 2},
    {5, 4, {1, 1, 2}, 3}, {5, 4, {2, 1, 1}, 3}, {5, 4, {1, 1, 1, 1}, 4},
};
const Pat kCompound[] = {  // one dotted quarter of 6/8 (6 sixteenths)
    {1, 6, {6}, 1}, {2, 6, {4, 2}, 2}, {3, 6, {2, 2, 2}, 3}, {4, 6, {3, 1, 2}, 3}, {4, 6, {2, -2, 2}, 3},
    {5, 6, {1, 1, 2, 2}, 4}, {5, 6, {2, 1, 1, 2}, 4},
};
template <size_t N> const Pat& pick(const Pat (&bank)[N], int level, Rng& r)
{
    int cands[N]; int n = 0;
    for (size_t i = 0; i < N; ++i) if (bank[i].level <= level && bank[i].level >= level - 2) cands[n++] = (int)i;
    if (n == 0) return bank[0];
    // favour the newest material for the level
    int idx = cands[r.range(n)];
    int idx2 = cands[r.range(n)];
    return bank[(size_t)(bank[(size_t)idx2].level > bank[(size_t)idx].level ? idx2 : idx)];
}
} // namespace gendetail

// Builds the exercise as a MIDI file in memory (ppq 96), plus a short description.
inline std::vector<uint8_t> generateExercise(const GenSettings& gs, std::string& title, std::string& desc)
{
    using namespace gendetail;
    Rng rng(gs.seed);
    static const int keyRoots[12] = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
    int keyIdx = gs.keyIndex >= 12 ? rng.range(12) : gs.keyIndex;
    int root = keyRoots[keyIdx];
    const GenScale& scale = kGenScales[gs.scaleIndex < 0 || gs.scaleIndex >= kNumGenScales ? 0 : gs.scaleIndex];
    bool minor = false;
    int sf = keySigFor(root, scale.mask, &minor);
    // scale degrees as pitch classes relative to C
    int degPc[12]; int nDeg = 0;
    for (int i = 0; i < 12; ++i) if (scale.mask & (1 << i)) degPc[nDeg++] = (root + i) % 12;

    const int ppq = 96, s16 = ppq / 4;
    int tsNum = 4, tsDen = 4;
    if (gs.meter == 1) tsNum = 3;
    if (gs.meter == 2) tsNum = 2;
    if (gs.meter == 3) { tsNum = 6; tsDen = 8; }
    int bar16 = gs.meter == 3 ? 12 : tsNum * 4;

    // Pitch ladders: every scale note in a register.
    auto ladder = [&](int lo, int hi) {
        std::vector<int> v;
        for (int p = lo; p <= hi; ++p) for (int d = 0; d < nDeg; ++d) if (p % 12 == degPc[d]) { v.push_back(p); break; }
        return v;
    };
    // registers: right hand from the tonic at or above C4, left hand an octave and a half lower
    int rhTonic = 60 + root; if (gs.range >= 2 && root >= 5) rhTonic -= 12;
    int lhTonic = rhTonic - 24; if (lhTonic < 36) lhTonic += 12;
    int span[4] = {7, 12, 24, 31};
    int rhHi = rhTonic + span[gs.range < 0 ? 0 : (gs.range > 3 ? 3 : gs.range)];
    if (gs.range == 0) rhHi = rhTonic + 7;   // five scale notes (up to the fifth)
    std::vector<int> rhLad = ladder(rhTonic - (gs.range >= 3 ? 5 : 0), rhHi);
    if (gs.range == 0) { std::vector<int> v; for (int p : rhLad) if ((int)v.size() < 5 && p >= rhTonic) v.push_back(p); rhLad = v; }
    std::vector<int> lhLad = ladder(lhTonic - (gs.range >= 2 ? 5 : 0), lhTonic + (gs.range == 0 ? 7 : 12));
    if (gs.range == 0) { std::vector<int> v; for (int p : lhLad) if ((int)v.size() < 5 && p >= lhTonic) v.push_back(p); lhLad = v; }

    auto melody = [&](const std::vector<int>& lad, int tonicPitch, bool lowHand) {
        std::vector<SmfWriteNote> out;
        if (lad.empty()) return out;
        int idx = 0;
        for (size_t i = 0; i < lad.size(); ++i) if (lad[i] == tonicPitch) idx = (int)i;
        if (lowHand && gs.hands == 2) idx = 0;
        int t = 0;
        for (int bar = 0; bar < gs.bars; ++bar)
        {
            bool last = bar == gs.bars - 1, phraseEnd = (bar % 4) == 3;
            std::vector<int> durs;
            if (last) durs.push_back(bar16);
            else if (gs.meter == 0)
            {
                for (int h = 0; h < 2; ++h)
                {
                    const Pat& p = pick(kHalf, phraseEnd && h == 1 ? 1 : gs.rhythm, rng);
                    for (int k = 0; k < p.n; ++k) durs.push_back(p.v[k]);
                }
                if (gs.rhythm <= 1 && rng.range(3) == 0) { durs.clear(); durs.push_back(16); }
            }
            else if (gs.meter == 3)
            {
                for (int h = 0; h < 2; ++h) { const Pat& p = pick(kCompound, phraseEnd && h == 1 ? 1 : gs.rhythm, rng); for (int k = 0; k < p.n; ++k) durs.push_back(p.v[k]); }
            }
            else
            {
                for (int b = 0; b < tsNum; ++b) { const Pat& p = pick(kQuarter, phraseEnd && b == tsNum - 1 ? 1 : gs.rhythm, rng); for (int k = 0; k < p.n; ++k) durs.push_back(p.v[k]); }
                if (gs.rhythm <= 1) { durs.clear(); if (tsNum == 3) { durs.push_back(8); durs.push_back(4); } else durs.push_back(8); }
            }
            for (size_t k = 0; k < durs.size(); ++k)
            {
                int d = durs[k];
                if (d < 0) { t += -d * s16; continue; }
                bool finalNote = last || (phraseEnd && k == durs.size() - 1);
                if (finalNote)
                {   // land on a stable degree near the current position
                    int best = idx, bestDist = 1 << 20;
                    for (int c = 0; c < (int)lad.size(); ++c)
                    {
                        int rel = ((lad[(size_t)c] - (root % 12)) % 12 + 12) % 12;
                        bool stable = last ? rel == 0 : (rel == 0 || rel == 3 || rel == 4 || rel == 7);
                        if (!stable) continue;
                        int dist = std::abs(c - idx);
                        if (dist < bestDist) { bestDist = dist; best = c; }
                    }
                    idx = best;
                }
                else if (!(bar == 0 && k == 0))
                {
                    // step size by level
                    static const int maxStep[6] = {0, 1, 2, 4, 7, 12};
                    int lvl = gs.leaps < 1 ? 1 : (gs.leaps > 5 ? 5 : gs.leaps);
                    int step;
                    double r = rng.unit();
                    if (r < 0.15 && lvl <= 2) step = 0;                         // repeated note
                    else if (r < 0.70) step = 1;                                 // a step
                    else step = 1 + rng.range(maxStep[lvl]);
                    if (step > maxStep[lvl] && lvl > 0) step = maxStep[lvl];
                    int dir = rng.range(2) ? 1 : -1;
                    int ni = idx + dir * step;
                    if (ni < 0 || ni >= (int)lad.size()) ni = idx - dir * step;
                    if (ni < 0) ni = 0;
                    if (ni >= (int)lad.size()) ni = (int)lad.size() - 1;
                    idx = ni;
                }
                out.push_back({t, t + d * s16, (uint8_t)lad[(size_t)idx], 84});
                t += d * s16;
            }
        }
        return out;
    };

    // Left-hand accompaniment: I IV V I per four bars (minor: i iv v i), as asked.
    auto accompaniment = [&]() {
        std::vector<SmfWriteNote> out;
        static const int prog[4] = {0, 3, 4, 0};      // scale degrees (0-based)
        int t = 0;
        for (int bar = 0; bar < gs.bars; ++bar)
        {
            int deg = bar == gs.bars - 1 ? 0 : prog[bar % 4];
            if (bar == gs.bars - 2) deg = 4;
            int rootPitch = lhTonic + ((degPc[deg % nDeg] - degPc[0] + 12) % 12);
            if (rootPitch > lhTonic + 7) rootPitch -= 12;
            int p3 = rootPitch + ((degPc[(deg + 2) % nDeg] - degPc[deg % nDeg] + 12) % 12);
            int p5 = rootPitch + ((degPc[(deg + 4) % nDeg] - degPc[deg % nDeg] + 12) % 12);
            if (nDeg < 7) { p3 = rootPitch + (minor ? 3 : 4); p5 = rootPitch + 7; }
            int barTicks = bar16 * s16;
            switch (gs.lhStyle)
            {
                case 0: out.push_back({t, t + barTicks, (uint8_t)rootPitch, 70}); break;
                case 1: out.push_back({t, t + barTicks, (uint8_t)rootPitch, 70}); out.push_back({t, t + barTicks, (uint8_t)p5, 70}); break;
                case 2:
                {
                    int half = gs.meter == 0 ? 2 : 1;
                    int seg = barTicks / half;
                    for (int h = 0; h < half; ++h)
                        for (int p : {rootPitch, p3, p5}) out.push_back({t + h * seg, t + (h + 1) * seg, (uint8_t)p, 70});
                    break;
                }
                default:
                {   // broken chord in eighths (or quarters at rhythm level 1-2)
                    int step = gs.rhythm <= 2 ? 4 * s16 / 1 : 2 * s16;
                    if (gs.meter == 3) step = 2 * s16;
                    int pat[4] = {rootPitch, p5, p3, p5};
                    int k = 0;
                    for (int x = 0; x + step <= barTicks; x += step, ++k)
                        out.push_back({t + x, t + x + step, (uint8_t)pat[k % 4], 66});
                    break;
                }
            }
            t += barTicks;
        }
        return out;
    };

    std::vector<SmfWriteTrack> tracks;
    if (gs.hands == 0 || gs.hands == 2) { SmfWriteTrack tr; tr.name = "Right"; tr.notes = melody(rhLad, rhTonic, false); tracks.push_back(tr); }
    if (gs.hands == 1) { SmfWriteTrack tr; tr.name = "Left"; tr.channel = 1; tr.notes = melody(lhLad, lhTonic, true); tracks.push_back(tr); }
    if (gs.hands == 2) { SmfWriteTrack tr; tr.name = "Left"; tr.channel = 1; tr.notes = accompaniment(); tracks.push_back(tr); }

    static const char* rootNames[12] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    static const char* meters[4] = {"4/4", "3/4", "2/4", "6/8"};
    static const char* handNames[3] = {"RH", "LH", "Both"};
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Exercise %s %s", rootNames[root], scale.name);
    title = buf;
    std::snprintf(buf, sizeof(buf), "%d bars %s, rhythm %d, leaps %d, %s", gs.bars, meters[gs.meter & 3], gs.rhythm, gs.leaps, handNames[gs.hands % 3]);
    desc = buf;
    int bpm = 60 + 10 * (gs.rhythm <= 2 ? 3 : 1);
    return writeSmf(ppq, bpm, tsNum, tsDen, sf, minor, title, tracks);
}

} // namespace sr
#endif
