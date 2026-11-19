// score.h - from a MIDI file to something a player reads and the practice engine checks.
//
//   Smf  --quantise, split hands, measures-->  PlayNotes + Steps (what must be played, when)
//        --notate: values, ties, rests, beams, stems, accidentals-->  Slots (what is drawn)
//
// A Slot is one column of the on-screen grid: a frame index for every staff row (see
// gen/layout_gen.h and tools/skin/geometry.py). The whole piece is laid out once, on the
// loader thread; the audio thread only copies slots to the screen.
//
// Notation is deliberately simple: one voice per hand (notes that start together form a chord,
// held until the next onset in that hand), flat beams, ties across beats and barlines. That
// renders beginner and intermediate repertoire faithfully and keeps every cell in a finite
// catalogue of pre-rendered frames.
//
// SPDX-License-Identifier: MIT
#ifndef SR_SCORE_H
#define SR_SCORE_H

#include "gen/layout_gen.h"
#include "music.h"
#include "smf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sr {

constexpr int kUPQ = 48;                // score units per quarter note
constexpr int kWholeUnits = 4 * kUPQ;

struct PlayNote { int32_t start = 0, end = 0; uint8_t pitch = 60, hand = 0, vel = 80; };

struct Step                              // notes that start together (both hands)
{
    int32_t time = 0;
    uint32_t firstNote = 0, count = 0;   // into Score::stepNotes
    int32_t slot = 0;                    // display slot of this onset
    uint16_t measure = 0;
    uint8_t hands = 0;                   // bit 0 right, bit 1 left
};

struct Measure
{
    int32_t start = 0, len = kWholeUnits;
    int32_t firstSlot = 0, numSlots = 0;
    uint16_t number = 1;
    uint8_t tsNum = 4, tsDen = 4;
    int8_t keySf = 0; bool minor = false;
};

struct Slot
{
    uint8_t cells[gen::kRows];
    uint8_t barline = 0;                 // barline frame drawn at this slot's left edge
    int32_t time = 0;                    // score time at this slot
    int32_t step = -1;                   // step that starts here, or -1
    uint16_t measure = 0;
    bool measureStart = false;
};

struct TempoPoint { int32_t unit; double usPerQuarter; };

struct Score
{
    std::string title, path;
    int keySf = 0; bool minor = false;
    int tsNum = 4, tsDen = 4;
    double bpm = 100.0;                  // first tempo of the file
    std::vector<PlayNote> notes;         // sorted by start
    std::vector<uint32_t> stepNotes;     // note indices grouped by step
    std::vector<Step> steps;
    std::vector<Measure> measures;
    std::vector<Slot> slots;
    std::vector<TempoPoint> tempo;
    int32_t endUnit = 0;
    int low[2] = {127, 127}, high[2] = {0, 0};   // pitch range per hand
    int grid = 12;                        // quantisation grid used, in units

    int timeSigFrame() const
    {
        static const struct { int n, d; } t[] = {{4,4},{3,4},{2,4},{2,2},{0,0},{0,0},{3,8},{6,8},{9,8},{12,8},{5,4},{6,4},{7,8},{5,8}};
        for (int i = 0; i < 14; ++i) if (t[i].n == tsNum && t[i].d == tsDen) return i;
        return 14;
    }
    // Seconds from the start for a score time, following the file's tempo map scaled by factor.
    double secondsAt(int32_t unit) const
    {
        double sec = 0; int32_t last = 0; double us = tempo.empty() ? 600000.0 : tempo[0].usPerQuarter;
        for (const TempoPoint& t : tempo)
        {
            if (t.unit >= unit) break;
            sec += (double)(t.unit - last) / kUPQ * us * 1e-6; last = t.unit; us = t.usPerQuarter;
        }
        return sec + (double)(unit - last) / kUPQ * us * 1e-6;
    }
    int measureAt(int32_t unit) const
    {
        int lo = 0, hi = (int)measures.size() - 1;
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (measures[(size_t)mid].start <= unit) lo = mid; else hi = mid - 1; }
        return lo;
    }
};

struct BuildOptions
{
    int splitPitch = 60;     // single-track files: notes at or above go to the right hand
    int transpose = 0;       // semitones
};

namespace scoredetail {

struct NoteValue { int8_t base = 2; int8_t dots = 0; bool triplet = false; int32_t units = kUPQ; };

inline int32_t valueUnits(int base, int dots, bool triplet)
{
    int32_t u = kWholeUnits >> base;
    if (dots == 1) u = u * 3 / 2;
    if (triplet) u = u * 2 / 3;
    return u;
}

// Split a duration starting at offset (within its measure) into notatable values.
inline void decompose(int32_t off, int32_t len, int32_t beat, std::vector<NoteValue>& out)
{
    struct Cand { int8_t base, dots; bool trip; };
    static const Cand cands[] = {
        {0,0,false},{1,1,false},{1,0,false},{2,1,false},{2,0,false},{3,1,false},{3,0,false},{4,1,false},{4,0,false},{5,0,false},
        {2,0,true},{3,0,true},{4,0,true},{5,0,true}};
    int guard = 0;
    while (len > 0 && guard++ < 64)
    {
        bool tripletContext = (off % 6) != 0 || (len % 6) != 0;
        bool chosen = false;
        for (const Cand& c : cands)
        {
            if (c.trip != tripletContext && c.trip) continue;
            int32_t u = valueUnits(c.base, c.dots, c.trip);
            if (u <= 0 || u > len) continue;
            int32_t baseU = valueUnits(c.base, 0, c.trip);
            int32_t align = c.dots ? baseU / 2 : std::min(baseU, beat);
            if (c.trip) align = u;
            if (align > 0 && off % align != 0) continue;
            // Starting off the beat, a note must not run over the next beat (it is split and
            // tied there instead), except the usual eighth + dotted quarter that lands on a beat.
            int32_t inBeat = beat > 0 ? off % beat : 0;
            if (!c.trip && inBeat != 0 && inBeat + u > beat)
            {
                bool syncopation = c.dots && (off + u) % beat == 0 && off % (beat / 2 > 0 ? beat / 2 : 1) == 0;
                if (!syncopation) continue;
            }
            NoteValue v; v.base = c.base; v.dots = c.dots; v.triplet = c.trip; v.units = u;
            out.push_back(v); off += u; len -= u; chosen = true; break;
        }
        if (!chosen)
        {   // nothing aligns (odd grid): take the largest value that fits, or the shortest one
            NoteValue v; v.base = 5; v.units = std::min<int32_t>(len, valueUnits(5, 0, false));
            for (int b = 0; b <= 5; ++b) if (valueUnits(b, 0, false) <= len) { v.base = (int8_t)b; v.units = valueUnits(b, 0, false); break; }
            out.push_back(v); off += v.units; len -= v.units;
        }
    }
}

struct Sym
{
    int32_t start = 0;
    NoteValue val;
    bool rest = false, fullMeasureRest = false;
    std::vector<uint8_t> pitches;      // ascending
    bool tieOut = false, tieIn = false;
    int beamLevelL = 0, beamLevelR = 0;
    bool beamed = false;
    int stemDir = 1;                   // +1 up, -1 down
    int stemEnd = 0;                   // staff position where the stem ends
    bool tupletMark = false;
    int slot = 0;                      // first display slot
};

// Cell specification before encoding to a frame index.
struct CellSpec
{
    uint8_t kind = gen::kEmpty, a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    int prio() const
    {
        switch (kind) { case gen::kHead: return 4; case gen::kStem: return 3; case gen::kRest: return 3;
                        case gen::kTie: case gen::kTuplet: return 2; case gen::kLedgerOnly: return 1; default: return 0; }
    }
};

inline int rowOf(int staff, int pos)
{
    using namespace gen;
    if (staff == 0) return (pos <= kTrebleTopPos && pos >= kTrebleLowPos) ? kTrebleTopPos - pos : -1;
    if (pos <= kBassTopPos && pos >= kBassLowPos) return kTrebleRows + (kBassTopPos - pos);
    if (pos > kBassTopPos && pos <= kBassTopPos + 7) return rowOf(0, pos - 16);
    return -1;
}
inline int clampPos(int staff, int pos)
{
    int hi = staff == 0 ? gen::kTrebleTopPos : gen::kBassTopPos + 7;
    int lo = staff == 0 ? gen::kTrebleLowPos : gen::kBassLowPos;
    return std::max(lo, std::min(hi, pos));
}

template <int N>
inline int lookupFrame(const uint32_t (&keys)[N], const uint8_t (&idx)[N], uint32_t key)
{
    int lo = 0, hi = N - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (keys[mid] == key) return idx[mid]; if (keys[mid] < key) lo = mid + 1; else hi = mid - 1; }
    return -1;
}

inline int encodeKey(int cls, uint32_t key)
{
    if (cls == gen::kSpace) return lookupFrame(gen::kKeys_space, gen::kIdx_space, key);
    if (cls == gen::kLine) return lookupFrame(gen::kKeys_line, gen::kIdx_line, key);
    return lookupFrame(gen::kKeys_ledger, gen::kIdx_ledger, key);
}

// Encode with graceful degradation: drop the ledger flag, then flags/beams, then give up.
inline uint8_t encodeCell(int row, CellSpec s)
{
    int cls = gen::kRowClass[row];
    if (cls == gen::kLedger && s.kind == gen::kHead) s.f = 1;
    if (cls != gen::kLedger && s.kind == gen::kHead) s.f = 0;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        int fr = encodeKey(cls, gen::cellKey(s.kind, s.a, s.b, s.c, s.d, s.e, s.f));
        if (fr >= 0) return (uint8_t)fr;
        if (attempt == 0) { if (s.kind == gen::kStem && s.f) { s.f = 0; continue; } }
        if (attempt <= 1 && s.kind == gen::kStem) { s.a = 0; if (s.e >= gen::kUpEnd && s.d == 0) { s.b = 0; s.c = 0; } continue; }
        if (s.kind == gen::kEmpty) break;
        if (cls == gen::kLedger && s.f) return (uint8_t)encodeKey(cls, gen::cellKey(gen::kLedgerOnly));
        s = CellSpec();
    }
    return 0;
}

} // namespace scoredetail

// Builds a Score from a parsed MIDI file. Runs on the loader thread (allocates freely).
inline bool buildScore(const Smf& smf, const BuildOptions& opt, Score& sc, std::string& err)
{
    using namespace scoredetail;
    sc = Score();
    if (smf.notes.empty()) { err = "no notes"; return false; }
    const double toUnits = (double)kUPQ / (double)smf.ppq;

    // ---- 1. quantisation grid: the coarsest grid the onsets fit -------------------------------
    static const int grids[] = {12, 16, 8, 6, 4, 3, 2};
    int grid = 2;
    for (int g : grids)
    {
        double errSum = 0; int n = 0;
        for (const SmfNote& nn : smf.notes)
        {
            double u = nn.on * toUnits;
            double q = std::round(u / g) * g;
            errSum += std::fabs(u - q); ++n;
        }
        if (n && errSum / n <= g * 0.08) { grid = g; break; }
    }
    sc.grid = grid;
    auto quant = [grid, toUnits](int32_t tick) { return (int32_t)(std::lround(tick * toUnits / grid) * grid); };

    // ---- 2. hands -------------------------------------------------------------------------------
    // Track names first, then track pitch averages, then a pitch split.
    int nt = (int)smf.tracks.size();
    std::vector<int> trackHand((size_t)std::max(nt, 1), -1);
    int noteTracks = 0;
    for (int t = 0; t < nt; ++t)
    {
        if (smf.tracks[(size_t)t].notes == 0) continue;
        ++noteTracks;
        std::string n = smf.tracks[(size_t)t].name;
        for (char& c : n) c = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        if (n.find("right") != std::string::npos || n == "rh" || n.find("treble") != std::string::npos || n.find("upper") != std::string::npos || n.find("melody") != std::string::npos) trackHand[(size_t)t] = 0;
        else if (n.find("left") != std::string::npos || n == "lh" || n.find("bass") != std::string::npos || n.find("lower") != std::string::npos) trackHand[(size_t)t] = 1;
    }
    if (noteTracks >= 2)
    {
        // Unnamed tracks: compare their average pitch to the others.
        double avgAll = 0; int cnt = 0;
        for (int t = 0; t < nt; ++t) if (smf.tracks[(size_t)t].notes) { avgAll += smf.tracks[(size_t)t].pitchSum / smf.tracks[(size_t)t].notes; ++cnt; }
        avgAll /= std::max(1, cnt);
        double hiAvg = -1, loAvg = 999; int hiT = -1, loT = -1;
        for (int t = 0; t < nt; ++t) if (smf.tracks[(size_t)t].notes)
        {
            double a = smf.tracks[(size_t)t].pitchSum / smf.tracks[(size_t)t].notes;
            if (a > hiAvg) { hiAvg = a; hiT = t; }
            if (a < loAvg) { loAvg = a; loT = t; }
        }
        for (int t = 0; t < nt; ++t)
        {
            if (!smf.tracks[(size_t)t].notes || trackHand[(size_t)t] >= 0) continue;
            double a = smf.tracks[(size_t)t].pitchSum / smf.tracks[(size_t)t].notes;
            if (t == hiT && hiT != loT) trackHand[(size_t)t] = 0;
            else if (t == loT && hiT != loT) trackHand[(size_t)t] = 1;
            else trackHand[(size_t)t] = a >= avgAll ? 0 : 1;
        }
    }

    // ---- 3. play notes --------------------------------------------------------------------------
    for (const SmfNote& n : smf.notes)
    {
        int p = n.pitch + opt.transpose;
        if (p < 0 || p > 127) continue;
        PlayNote pn;
        pn.start = quant(n.on);
        pn.end = std::max(pn.start + grid, quant(n.off));
        pn.pitch = (uint8_t)p; pn.vel = n.vel;
        int h = (n.track >= 0 && n.track < nt) ? trackHand[(size_t)n.track] : -1;
        if (h < 0) h = (p >= opt.splitPitch) ? 0 : 1;
        pn.hand = (uint8_t)h;
        sc.notes.push_back(pn);
    }
    if (sc.notes.empty()) { err = "no notes in range"; return false; }
    std::stable_sort(sc.notes.begin(), sc.notes.end(), [](const PlayNote& a, const PlayNote& b) {
        return a.start != b.start ? a.start < b.start : (a.hand != b.hand ? a.hand < b.hand : a.pitch < b.pitch); });
    // Drop exact duplicates (same start, hand, pitch).
    sc.notes.erase(std::unique(sc.notes.begin(), sc.notes.end(), [](const PlayNote& a, const PlayNote& b) {
        return a.start == b.start && a.pitch == b.pitch && a.hand == b.hand; }), sc.notes.end());
    // A note re-struck while still sounding in the same hand ends where the next one starts.
    for (size_t i = 0; i < sc.notes.size(); ++i)
        for (size_t j = i + 1; j < sc.notes.size() && sc.notes[j].start < sc.notes[i].end; ++j)
            if (sc.notes[j].pitch == sc.notes[i].pitch && sc.notes[j].start > sc.notes[i].start) { sc.notes[i].end = sc.notes[j].start; break; }
    int32_t lastEnd = 0;
    for (const PlayNote& n : sc.notes)
    {
        lastEnd = std::max(lastEnd, n.end);
        sc.low[n.hand] = std::min<int>(sc.low[n.hand], n.pitch);
        sc.high[n.hand] = std::max<int>(sc.high[n.hand], n.pitch);
    }

    // ---- 4. tempo, measures, keys --------------------------------------------------------------
    for (const SmfTempo& t : smf.tempos) sc.tempo.push_back({quant(t.tick), (double)std::max<uint32_t>(t.usPerQuarter, 100000)});
    if (sc.tempo.empty() || sc.tempo[0].unit > 0) sc.tempo.insert(sc.tempo.begin(), {0, 500000.0});
    sc.bpm = 60000000.0 / sc.tempo[0].usPerQuarter;
    if (sc.tempo.size() > 1 && sc.tempo[1].unit == 0) sc.bpm = 60000000.0 / sc.tempo[1].usPerQuarter;
    {
        std::vector<SmfTimeSig> ts;
        for (const SmfTimeSig& t : smf.timeSigs)   // every track may repeat the same signature
            if (ts.empty() || quant(t.tick) != quant(ts.back().tick)) ts.push_back(t);
            else ts.back() = t;
        if (ts.empty() || ts[0].tick > 0) ts.insert(ts.begin(), {0, 4, 4});
        size_t ti = 0; int num = ts[0].num, den = ts[0].den;
        // Anacrusis: MIDI has no barlines, so a piece that starts with a pickup would have every
        // barline in the wrong place. Score each possible pickup length by how much weight
        // (long notes, bass notes) falls on the resulting downbeats and beats.
        int32_t pickup = 0;
        {
            int32_t L = std::max(kUPQ / 4, num * (kWholeUnits / std::max(1, den)));
            int32_t beat = (den == 8 && num % 3 == 0 && num > 3) ? 72 : kWholeUnits / std::max(1, den);
            int32_t horizon = L * 16;
            auto metric = [&](int32_t s) {
                double total = 0;
                for (const PlayNote& n : sc.notes)
                {
                    if (n.start > horizon) break;
                    if (n.start < s) continue;
                    double w = std::min<double>(n.end - n.start, L) / L + (n.hand == 1 ? 0.6 : 0.0);
                    int32_t rel = n.start - s;
                    if (rel % L == 0) total += w;                 // only downbeats discriminate:
                    else if (rel % beat == 0) total += 0.05 * w;  // beats line up for most shifts
                }
                return total;
            };
            bool notesAtZero = !sc.notes.empty() && sc.notes[0].start == 0;
            bool steadyMeter = ts.size() == 1 || quant(ts[1].tick) > 4 * L;
            if (notesAtZero && steadyMeter)
            {
                // The accompaniment usually enters on a downbeat: when the left hand starts after
                // the right, its first onset fixes the barlines. Otherwise only a clear metric win counts.
                int32_t firstLH = -1;
                for (const PlayNote& n : sc.notes) if (n.hand == 1) { firstLH = n.start; break; }
                double base = metric(0);
                int32_t cand = (firstLH > 0) ? firstLH % L : 0;
                if (cand > 0 && cand <= L * 3 / 4 && metric(cand) >= base) pickup = cand;
                else
                {
                    double best = base;
                    for (int32_t s = std::max(grid, 6); s < L; s += std::max(grid, 6))
                    {
                        double v = metric(s);
                        if (v > best) { best = v; pickup = s; }
                    }
                    if (best < base * 1.5 + 0.5) pickup = 0;
                }
            }
        }
        int32_t pos = 0; uint16_t number = pickup > 0 ? 0 : 1;
        while (pos < lastEnd && sc.measures.size() < 4000)
        {
            while (ti + 1 < ts.size() && quant(ts[ti + 1].tick) <= pos) { ++ti; num = ts[ti].num; den = ts[ti].den; }
            Measure m; m.start = pos; m.tsNum = (uint8_t)num; m.tsDen = (uint8_t)den;
            m.len = std::max(kUPQ / 4, num * (kWholeUnits / std::max(1, den)));
            if (pos == 0 && pickup > 0) m.len = pickup;
            // Key at the measure start
            int sf = 0; bool mi = false;
            for (const SmfKeySig& k : smf.keySigs) if (quant(k.tick) <= pos) { sf = k.sf; mi = k.minor != 0; }
            if (opt.transpose != 0)
            {   // move the key by the transposition, keeping it within 6 accidentals
                int shift = ((opt.transpose % 12) + 12) % 12;
                sf += ((shift * 7) % 12);
                while (sf > 6) sf -= 12;
                while (sf < -6) sf += 12;
            }
            m.keySf = (int8_t)sf; m.minor = mi;
            m.number = number++;
            sc.measures.push_back(m);
            pos += m.len;
        }
        if (sc.measures.empty()) { Measure m; sc.measures.push_back(m); }
        sc.endUnit = sc.measures.back().start + sc.measures.back().len;
        sc.tsNum = sc.measures[0].tsNum; sc.tsDen = sc.measures[0].tsDen;
        sc.keySf = sc.measures[0].keySf; sc.minor = sc.measures[0].minor;
    }
    // A pickup (anacrusis): the first notes start late in a short first measure. Leave as is.

    // ---- 5. steps --------------------------------------------------------------------------------
    for (size_t i = 0; i < sc.notes.size();)
    {
        size_t j = i;
        Step st; st.time = sc.notes[i].start; st.firstNote = (uint32_t)sc.stepNotes.size();
        while (j < sc.notes.size() && sc.notes[j].start == st.time) { sc.stepNotes.push_back((uint32_t)j); st.hands |= (uint8_t)(1 << sc.notes[j].hand); ++j; }
        st.count = (uint32_t)(j - i);
        st.measure = (uint16_t)sc.measureAt(st.time);
        sc.steps.push_back(st);
        i = j;
    }

    // ---- 6. notation per hand -------------------------------------------------------------------
    std::vector<Sym> syms[2];
    for (int hand = 0; hand < 2; ++hand)
    {
        // onsets of this hand
        struct Ev { int32_t start, end; std::vector<uint8_t> pitches; };
        std::vector<Ev> evs;
        for (const PlayNote& n : sc.notes)
        {
            if (n.hand != hand) continue;
            if (!evs.empty() && evs.back().start == n.start) { evs.back().pitches.push_back(n.pitch); evs.back().end = std::max(evs.back().end, n.end); }
            else evs.push_back({n.start, n.end, {n.pitch}});
        }
        // timeline segments: chord or rest
        struct Seg { int32_t a, b; bool rest; const std::vector<uint8_t>* p; };
        std::vector<Seg> segs;
        int32_t t = 0;
        for (size_t i = 0; i < evs.size(); ++i)
        {
            if (evs[i].start > t) segs.push_back({t, evs[i].start, true, nullptr});
            int32_t next = i + 1 < evs.size() ? evs[i + 1].start : sc.endUnit;
            int32_t end = std::min(evs[i].end, next);
            int32_t len = end - evs[i].start;
            int32_t gap = next - end;
            if (gap > 0 && gap < std::max<int32_t>(6, len / 3)) end = next;   // articulation gap: treat as legato
            segs.push_back({evs[i].start, end, false, &evs[i].pitches});
            t = end;
        }
        if (t < sc.endUnit) segs.push_back({t, sc.endUnit, true, nullptr});
        // split at barlines and decompose
        for (const Seg& s : segs)
        {
            int32_t a = s.a;
            bool firstPiece = true;
            while (a < s.b)
            {
                int mi = sc.measureAt(a);
                const Measure& m = sc.measures[(size_t)mi];
                int32_t mEnd = m.start + m.len;
                int32_t b = std::min(s.b, mEnd);
                int32_t beat = (m.tsDen == 8 && m.tsNum % 3 == 0 && m.tsNum > 3) ? 72 : (kWholeUnits / std::max<int>(1, m.tsDen));
                if (s.rest && m.number == 0) { a = b; continue; }   // pickup bar: no rests for a silent hand
                if (s.rest && a == m.start && b == mEnd)
                {   // whole-measure rest
                    Sym r; r.start = a; r.rest = true; r.fullMeasureRest = true; r.val.base = 0; r.val.units = m.len;
                    syms[hand].push_back(r);
                    a = b; continue;
                }
                std::vector<NoteValue> vals;
                decompose(a - m.start, b - a, beat, vals);
                int32_t pos = a;
                for (size_t k = 0; k < vals.size(); ++k)
                {
                    Sym y; y.start = pos; y.val = vals[k]; y.rest = s.rest;
                    if (!s.rest)
                    {
                        y.pitches = *s.p; std::sort(y.pitches.begin(), y.pitches.end());
                        y.tieIn = !firstPiece;
                        y.tieOut = (pos + vals[k].units) < s.b;
                    }
                    syms[hand].push_back(y);
                    pos += vals[k].units; firstPiece = false;
                }
                a = b;
            }
        }
    }

    // ---- 7. slots: one column per onset, wider for long notes ----------------------------------
    for (size_t mi = 0; mi < sc.measures.size(); ++mi)
    {
        Measure& m = sc.measures[mi];
        std::vector<int32_t> onsets;
        for (int h = 0; h < 2; ++h)
            for (const Sym& y : syms[h]) if (y.start >= m.start && y.start < m.start + m.len) onsets.push_back(y.start);
        if (onsets.empty()) onsets.push_back(m.start);
        std::sort(onsets.begin(), onsets.end());
        onsets.erase(std::unique(onsets.begin(), onsets.end()), onsets.end());
        m.firstSlot = (int32_t)sc.slots.size();
        for (size_t k = 0; k < onsets.size(); ++k)
        {
            int32_t next = k + 1 < onsets.size() ? onsets[k + 1] : m.start + m.len;
            int32_t gap = next - onsets[k];
            int width = gap < 72 ? 1 : (gap < 144 ? 2 : 3);
            for (int w = 0; w < width; ++w)
            {
                Slot sl; std::fill(std::begin(sl.cells), std::end(sl.cells), (uint8_t)0);
                sl.time = onsets[k] + (int32_t)((int64_t)gap * w / width);
                sl.measure = (uint16_t)mi;
                sl.measureStart = (k == 0 && w == 0);
                if (sl.measureStart && mi > 0) sl.barline = 1;
                sc.slots.push_back(sl);
            }
        }
        m.numSlots = (int32_t)sc.slots.size() - m.firstSlot;
    }
    auto slotAt = [&sc](int32_t time) -> int {
        // first slot whose time equals the onset (slots are in time order)
        int lo = 0, hi = (int)sc.slots.size() - 1;
        while (lo < hi) { int mid = (lo + hi) / 2; if (sc.slots[(size_t)mid].time < time) lo = mid + 1; else hi = mid; }
        return lo;
    };
    for (Step& st : sc.steps) { st.slot = slotAt(st.time); sc.slots[(size_t)st.slot].step = (int32_t)(&st - &sc.steps[0]); }

    // ---- 8. per-symbol layout: positions, stems, beams, accidentals -----------------------------
    std::vector<std::vector<CellSpec>> grid2((size_t)sc.slots.size(), std::vector<CellSpec>(gen::kRows));
    auto put = [&grid2](int slot, int row, const CellSpec& s) {
        if (slot < 0 || slot >= (int)grid2.size() || row < 0 || row >= gen::kRows) return;
        CellSpec& c = grid2[(size_t)slot][(size_t)row];
        if (s.prio() > c.prio() || (s.prio() == c.prio() && s.kind == c.kind)) c = s;
    };
    auto ledgerOn = [&grid2](int slot, int row) {
        if (slot < 0 || slot >= (int)grid2.size() || row < 0 || row >= gen::kRows) return;
        if (gen::kRowClass[row] != gen::kLedger) return;
        CellSpec& c = grid2[(size_t)slot][(size_t)row];
        if (c.kind == gen::kEmpty) c.kind = gen::kLedgerOnly;
        else if (c.kind == gen::kStem && (c.e == gen::kThruUp || c.e == gen::kThruDown) && c.a == 0) c.f = 1;
    };

    for (int hand = 0; hand < 2; ++hand)
    {
        std::vector<Sym>& ys = syms[hand];
        const int staff = hand;
        for (Sym& y : ys) y.slot = slotAt(y.start);

        // Chromatic notes are spelled by where the melody goes: D# before E, Eb before D.
        auto prefOf = [&ys](size_t yi, uint8_t pitch) -> int {
            const Sym& y = ys[yi];
            if (y.pitches.size() != 1) return 0;
            for (size_t k = yi + 1; k < ys.size() && k <= yi + 2; ++k)
            {
                if (ys[k].rest) break;
                if (ys[k].pitches.size() != 1) break;
                int nx = ys[k].pitches[0];
                if (nx == pitch) continue;            // tied or repeated: look further
                if (nx == pitch + 1) return 1;
                if (nx == pitch - 1) return -1;
                break;
            }
            if (yi > 0 && !ys[yi - 1].rest && ys[yi - 1].pitches.size() == 1)
            {
                int pv = ys[yi - 1].pitches[0];
                if (pv == pitch - 1) return 1;
                if (pv == pitch + 1) return -1;
            }
            return 0;
        };
        // positions of every pitch (spelled in the measure's key)
        auto posOf = [&](size_t yi, uint8_t pitch) {
            const Sym& y = ys[yi];
            const Measure& m = sc.measures[(size_t)sc.measureAt(y.start)];
            return staffPosition(spellPitch(pitch, m.keySf, m.minor, prefOf(yi, pitch)), staff);
        };

        // beams: consecutive beamable notes inside one beat group
        for (size_t i = 0; i < ys.size();)
        {
            auto beamable = [](const Sym& s) { return !s.rest && s.val.base >= 3; };
            if (!beamable(ys[i])) { ++i; continue; }
            const Measure& m = sc.measures[(size_t)sc.measureAt(ys[i].start)];
            int32_t span = (m.tsDen == 8 && m.tsNum % 3 == 0 && m.tsNum > 3) ? 72 : (m.tsDen == 8 ? 24 * 3 : kUPQ);
            int32_t group = (ys[i].start - m.start) / span;
            size_t j = i + 1;
            while (j < ys.size() && beamable(ys[j]) && sc.measureAt(ys[j].start) == sc.measureAt(ys[i].start) &&
                   (ys[j].start - m.start) / span == group && ys[j].start == ys[j - 1].start + ys[j - 1].val.units)
                ++j;
            if (j - i >= 2)
            {
                std::vector<int> lv;
                for (size_t k = i; k < j; ++k) lv.push_back(std::min(2, ys[k].val.base - 2));
                int n = (int)lv.size();
                for (int k = 0; k < n; ++k)
                {
                    Sym& s = ys[i + (size_t)k];
                    s.beamed = true;
                    s.beamLevelL = k > 0 ? std::min(lv[(size_t)k], lv[(size_t)k - 1]) : 0;
                    s.beamLevelR = k < n - 1 ? std::min(lv[(size_t)k], lv[(size_t)k + 1]) : 0;
                    if (lv[(size_t)k] > std::max(s.beamLevelL, s.beamLevelR))
                    { if (k == n - 1) s.beamLevelL = lv[(size_t)k]; else s.beamLevelR = lv[(size_t)k]; }
                }
                // shared stem direction and flat beam height
                int far = 0; int maxPos = -99, minPos = 99;
                for (size_t k = i; k < j; ++k)
                    for (uint8_t p : ys[k].pitches) { int ps = posOf(k, p); maxPos = std::max(maxPos, ps); minPos = std::min(minPos, ps); }
                far = (maxPos - 4 > 4 - minPos) ? -1 : 1;
                int end = far > 0 ? -99 : 99;
                for (size_t k = i; k < j; ++k)
                {
                    int hi = -99, lo = 99;
                    for (uint8_t p : ys[k].pitches) { int ps = posOf(k, p); hi = std::max(hi, ps); lo = std::min(lo, ps); }
                    if (far > 0) end = std::max(end, std::max(hi + gen::kStemLen, std::min(4, hi + 2 * gen::kStemLen)));
                    else end = std::min(end, std::min(lo - gen::kStemLen, std::max(4, lo - 2 * gen::kStemLen)));
                }
                for (size_t k = i; k < j; ++k) { ys[k].stemDir = far; ys[k].stemEnd = end; }
                i = j;
            }
            else ++i;
        }
        // triplet marks: the middle of each run of three triplet values
        for (size_t i = 0; i < ys.size(); ++i)
        {
            if (!ys[i].val.triplet) continue;
            size_t j = i; while (j < ys.size() && ys[j].val.triplet) ++j;
            for (size_t k = i + 1; k < j; k += 3) ys[k].tupletMark = true;
            i = j - 1;
        }

        // accidentals and cells
        int curMeasure = -1; int8_t state[128] = {0}; int8_t keyAcc[7] = {0};
        for (size_t yi = 0; yi < ys.size(); ++yi)
        {
            Sym& y = ys[yi];
            int mi = sc.measureAt(y.start);
            const Measure& m = sc.measures[(size_t)mi];
            if (mi != curMeasure)
            {
                curMeasure = mi; keyAccidentals(m.keySf, keyAcc);
                for (int d = 0; d < 128; ++d) state[d] = keyAcc[d % 7];
            }
            if (y.rest)
            {
                int restType = y.fullMeasureRest ? 0 : y.val.base;
                int rp = gen::kRestPos[std::min(5, std::max(0, restType))];
                CellSpec c; c.kind = gen::kRest; c.a = (uint8_t)restType; c.d = (uint8_t)(y.val.dots ? 1 : 0);
                put(y.slot, rowOf(staff, rp), c);
                continue;
            }
            // positions, sorted ascending
            struct H { int pos; int acc; bool showAcc; bool displaced; };
            std::vector<H> heads;
            for (uint8_t p : y.pitches)
            {
                Spelling sp = spellPitch(p, m.keySf, m.minor, prefOf(yi, p));
                int pos = staffPosition(sp, staff);
                int di = std::max(0, std::min(127, diatonicIndex(sp)));
                H h; h.pos = clampPos(staff, pos); h.acc = sp.acc; h.displaced = false;
                h.showAcc = !y.tieIn && state[di] != sp.acc;
                state[di] = sp.acc;
                heads.push_back(h);
            }
            std::sort(heads.begin(), heads.end(), [](const H& a, const H& b) { return a.pos < b.pos; });
            int lo = heads.front().pos, hi = heads.back().pos;
            int head = y.val.base == 0 ? 2 : (y.val.base == 1 ? 1 : 0);
            bool hasStem = head != 2;
            if (!y.beamed)
            {
                y.stemDir = (hi - 4 > 4 - lo) ? -1 : 1;
                y.stemEnd = y.stemDir > 0 ? std::max(hi + gen::kStemLen, std::min(4, hi + 2 * gen::kStemLen))
                                          : std::min(lo - gen::kStemLen, std::max(4, lo - 2 * gen::kStemLen));
            }
            y.stemEnd = clampPos(staff, y.stemEnd);
            // seconds: displace every other note of a cluster
            if (y.stemDir > 0) { for (size_t k = 1; k < heads.size(); ++k) if (heads[k].pos - heads[k - 1].pos == 1 && !heads[k - 1].displaced) heads[k].displaced = true; }
            else { for (size_t k = heads.size() - 1; k-- > 0;) if (heads[k + 1].pos - heads[k].pos == 1 && !heads[k + 1].displaced) heads[k].displaced = true; }

            int flags = (!y.beamed && y.val.base >= 3) ? y.val.base - 2 : 0;
            // heads
            for (const H& h : heads)
            {
                CellSpec c; c.kind = gen::kHead; c.a = (uint8_t)head; c.b = h.displaced ? 1 : 0;
                c.c = (uint8_t)(!h.showAcc ? 0 : (h.acc > 0 ? 1 : (h.acc < 0 ? 2 : 3)));
                c.d = (uint8_t)(y.val.dots ? 1 : 0);
                if (hasStem)
                {
                    bool startNote = y.stemDir > 0 ? (h.pos == lo) : (h.pos == hi);
                    c.e = startNote ? (y.stemDir > 0 ? gen::kUpStart : gen::kDownStart) : (y.stemDir > 0 ? gen::kThruUp : gen::kThruDown);
                }
                put(y.slot, rowOf(staff, h.pos), c);
                // ledger lines between the staff and this note
                if (h.pos >= 10) for (int lp = 10; lp <= h.pos; lp += 2) ledgerOn(y.slot, rowOf(staff, lp));
                if (h.pos <= -2) for (int lp = -2; lp >= h.pos; lp -= 2) ledgerOn(y.slot, rowOf(staff, lp));
            }
            // stem body, end, flag or beam
            if (hasStem)
            {
                int from = y.stemDir > 0 ? lo : hi;
                int to = y.stemEnd;
                int dir = y.stemDir;
                if ((to - from) * dir <= 0) to = from + dir;   // clamped at the grid edge: shortest stem
                for (int pp = from + dir; pp != to && std::abs(pp - from) < 40; pp += dir)
                {
                    int row = rowOf(staff, pp);
                    if (row < 0) continue;
                    CellSpec& cur = grid2[(size_t)y.slot][(size_t)row];
                    if (cur.kind == gen::kHead) continue;   // a chord note: its frame already has the stem
                    CellSpec c; c.kind = gen::kStem; c.e = dir > 0 ? gen::kThruUp : gen::kThruDown;
                    if (flags && pp == to - 3 * dir) c.a = (uint8_t)std::min(3, flags);
                    if (cur.kind == gen::kLedgerOnly) c.f = 1;
                    put(y.slot, row, c);
                }
                CellSpec e; e.kind = gen::kStem; e.e = dir > 0 ? gen::kUpEnd : gen::kDownEnd;
                if (y.beamed) { e.b = (uint8_t)y.beamLevelL; e.c = (uint8_t)y.beamLevelR; }
                put(y.slot, rowOf(staff, to), e);
                // beam through slots between this note and the next beamed one (other hand's onsets)
                if (y.beamed && y.beamLevelR > 0 && yi + 1 < ys.size())
                {
                    for (int s2 = y.slot + 1; s2 < ys[yi + 1].slot; ++s2)
                    {
                        CellSpec bt; bt.kind = gen::kStem; bt.e = e.e; bt.b = bt.c = (uint8_t)std::min(y.beamLevelR, 2); bt.d = 1;
                        put(s2, rowOf(staff, to), bt);
                    }
                }
                if (y.tupletMark)
                {
                    CellSpec t; t.kind = gen::kTuplet; t.b = dir > 0 ? 1 : 0;
                    put(y.slot, rowOf(staff, clampPos(staff, to + 2 * dir)), t);
                }
            }
            // ties to the next symbol
            if (y.tieOut && yi + 1 < ys.size())
            {
                int side = y.stemDir > 0 ? 0 : 1;            // below for stem up, above for stem down
                int nextSlot = ys[yi + 1].slot;
                for (const H& h : heads)
                {
                    int row = rowOf(staff, clampPos(staff, h.pos + (side ? 1 : -1)));
                    if (row < 0) continue;
                    CellSpec st; st.kind = gen::kTie; st.a = 1; st.b = (uint8_t)side;
                    put(y.slot, row, st);
                    for (int s2 = y.slot + 1; s2 < nextSlot; ++s2) { CellSpec th = st; th.a = 3; put(s2, row, th); }
                    CellSpec en = st; en.a = 2;
                    CellSpec& tgt = grid2[(size_t)std::min<int>(nextSlot, (int)grid2.size() - 1)][(size_t)row];
                    if (tgt.kind == gen::kTie && tgt.a == 1) tgt.a = 3; else put(nextSlot, row, en);
                }
            }
        }
    }
    // ---- 9. encode -------------------------------------------------------------------------------
    for (size_t s = 0; s < sc.slots.size(); ++s)
        for (int r = 0; r < gen::kRows; ++r)
            sc.slots[s].cells[r] = encodeCell(r, grid2[s][(size_t)r]);

    sc.title = smf.title;
    return true;
}

} // namespace sr
#endif
