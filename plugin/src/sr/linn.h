// linn.h - LinnStrument layout, fingering suggestions and lights.
//
// Layout follows the firmware (ls_handleTouches.ino, determineRowOffsetNote/getNoteNumber):
//   note(col, row) = lowest + row * rowOffset + (col - 1), columns 1..25 (or 1..16), rows 0..7
//   from the bottom; left-handed mirrors the columns. "lowest" is 30 (F#1) for fourths and
//   18 for the octave offset by default, plus any transposition the player set.
//
// Lights use two firmware mechanisms, neither of which changes the player's own note-light
// settings:
//   * Note On / Note Off sent to the LinnStrument light every cell of that note in the split's
//     "played" colour (ls_midi.ino highlightPossibleNoteCells). Used for the next-note guide.
//   * CC 20 (column), CC 21 (row), CC 22 (colour) paint one cell in a custom layer above the
//     note lights; colour 7 removes it again. Used for coloured guides, fingering, bar and key
//     highlights.
//
// SPDX-License-Identifier: MIT
#ifndef SR_LINN_H
#define SR_LINN_H

#include "score.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sr {

// LinnStrument colour numbers (midi.md "Color Values")
enum LinnColour : uint8_t { kLinnRed = 1, kLinnYellow = 2, kLinnGreen = 3, kLinnCyan = 4, kLinnBlue = 5, kLinnMagenta = 6,
                            kLinnOff = 7, kLinnWhite = 8, kLinnOrange = 9, kLinnLime = 10, kLinnPink = 11 };
// The plugin's colour choice lists (without "Played") in this order:
constexpr uint8_t kLinnColourChoice[10] = {kLinnRed, kLinnYellow, kLinnGreen, kLinnCyan, kLinnBlue, kLinnMagenta, kLinnWhite, kLinnOrange, kLinnLime, kLinnPink};
// Finger colours: thumb, index, middle, ring, little
constexpr uint8_t kFingerColour[6] = {0, kLinnYellow, kLinnGreen, kLinnCyan, kLinnBlue, kLinnMagenta};

struct LinnLayout
{
    int cols = 25, rows = 8, rowOffset = 5, lowest = 30;
    bool leftHanded = false;
    int note(int col, int row) const
    {
        int nc = leftHanded ? (cols + 1 - col) : col;
        return lowest + row * rowOffset + nc - 1;
    }
    int colFor(int note, int row) const
    {
        int c = note - (lowest + row * rowOffset) + 1;
        if (c < 1 || c > cols) return -1;
        return leftHanded ? cols + 1 - c : c;
    }
    bool operator==(const LinnLayout& o) const
    { return cols == o.cols && rows == o.rows && rowOffset == o.rowOffset && lowest == o.lowest && leftHanded == o.leftHanded; }
};

// ---------------------------------------------------------------------------------------------
// Fingering: choose a cell for every note and a finger, per hand.
// ---------------------------------------------------------------------------------------------
struct Fingering
{
    std::vector<int8_t> col, row, finger;   // per Score::notes index; col -1 when unplayable
};

inline void planFingering(const Score& sc, const LinnLayout& L, Fingering& out)
{
    // A hand position is the column of the index finger, h. One finger per column, like a
    // fretting hand: right hand thumb h-1, index h, middle h+1, ring h+2, little h+3 (the left
    // hand mirrors). Rows are reached by moving the fingers up and down, which is cheap; moving
    // the hand along the rows is what costs. Viterbi over (cell of the step's anchor note,
    // hand position) finds the cheapest path; the other notes of a chord go where the hand is.
    size_t n = sc.notes.size();
    out.col.assign(n, -1); out.row.assign(n, -1); out.finger.assign(n, 0);
    for (int hand = 0; hand < 2; ++hand)
    {
        std::vector<std::vector<uint32_t>> steps;
        for (const Step& st : sc.steps)
        {
            std::vector<uint32_t> v;
            for (uint32_t k = 0; k < st.count; ++k) { uint32_t ni = sc.stepNotes[st.firstNote + k]; if (sc.notes[ni].hand == hand) v.push_back(ni); }
            if (!v.empty()) steps.push_back(v);
        }
        if (steps.empty()) continue;
        const int dir = ((hand == 0) != L.leftHanded) ? 1 : -1;   // which way the fingers spread
        auto fingerOf = [dir](int col, int h) { return (col - h) * dir + 2; };   // 1 thumb .. 5 little
        const int homeCol = hand == 0 ? (L.cols * 2) / 3 : L.cols / 3;
        struct State { int col, row, h; };
        std::vector<std::vector<State>> states(steps.size());
        for (size_t s = 0; s < steps.size(); ++s)
        {
            int p = sc.notes[steps[s][0]].pitch;
            for (int r = 0; r < L.rows; ++r)
            {
                int c = L.colFor(p, r);
                if (c <= 0) continue;
                for (int f = 1; f <= 5; ++f)
                {
                    int h = c - (f - 2) * dir;               // hand position that puts finger f on c
                    if (h >= 1 && h <= L.cols) states[s].push_back({c, r, h});
                }
            }
            if (states[s].empty()) states[s].push_back({-1, -1, homeCol});
        }
        auto local = [&](size_t s, const State& a) {
            if (a.col < 0) return 30.0;
            double v = 0.12 * std::abs(a.row - 3) + 0.02 * std::abs(a.h - homeCol);
            int f = fingerOf(a.col, a.h);
            if (f == 1) v += 0.35;                           // the thumb is the awkward one
            // chord notes must fit the hand at this position
            for (size_t k = 1; k < steps[s].size(); ++k)
            {
                int bestF = 99;
                for (int r = 0; r < L.rows; ++r)
                {
                    int c = L.colFor(sc.notes[steps[s][k]].pitch, r);
                    if (c <= 0) continue;
                    int ff = fingerOf(c, a.h);
                    if (ff >= 1 && ff <= 5 && std::abs(r - a.row) <= 3) bestF = std::min(bestF, ff);
                }
                v += bestF == 99 ? 4.0 : 0.0;
            }
            return v;
        };
        auto move = [&](const State& a, const State& b) {
            if (a.col < 0 || b.col < 0) return 0.0;
            double v = 0;
            int dh = std::abs(a.h - b.h);
            if (dh) v += 1.2 + 0.8 * dh;                    // shifting the hand
            int dr = std::abs(a.row - b.row);
            v += 0.25 * dr + (dr > 2 ? 1.5 * (dr - 2) : 0.0);
            if (a.col != b.col || a.row != b.row)
                if (fingerOf(a.col, a.h) == fingerOf(b.col, b.h)) v += 1.0;   // same finger, new cell
            return v;
        };
        const double kInf = 1e18;
        std::vector<std::vector<double>> cost(steps.size());
        std::vector<std::vector<int>> back(steps.size());
        for (size_t s = 0; s < steps.size(); ++s)
        {
            cost[s].assign(states[s].size(), kInf); back[s].assign(states[s].size(), 0);
            for (size_t j = 0; j < states[s].size(); ++j)
            {
                double lc = local(s, states[s][j]);
                if (s == 0) { cost[s][j] = lc; continue; }
                for (size_t i = 0; i < states[s - 1].size(); ++i)
                {
                    double v = cost[s - 1][i] + move(states[s - 1][i], states[s][j]) + lc;
                    if (v < cost[s][j]) { cost[s][j] = v; back[s][j] = (int)i; }
                }
            }
        }
        std::vector<int> pick(steps.size(), 0);
        size_t last = steps.size() - 1;
        double best = kInf;
        for (size_t j = 0; j < states[last].size(); ++j) if (cost[last][j] < best) { best = cost[last][j]; pick[last] = (int)j; }
        for (size_t s = last; s > 0; --s) pick[s - 1] = back[s][(size_t)pick[s]];
        for (size_t s = 0; s < steps.size(); ++s)
        {
            const State& a = states[s][(size_t)pick[s]];
            for (size_t k = 0; k < steps[s].size(); ++k)
            {
                uint32_t ni = steps[s][k];
                int c = a.col, r = a.row;
                if (k > 0)
                {   // nearest cell inside the hand
                    int bestD = 1 << 20; c = -1; r = -1;
                    for (int rr = 0; rr < L.rows; ++rr)
                    {
                        int cc = L.colFor(sc.notes[ni].pitch, rr);
                        if (cc <= 0 || a.col < 0) continue;
                        int ff = fingerOf(cc, a.h);
                        int d = (ff >= 1 && ff <= 5 ? 0 : 20) + std::abs(rr - a.row) * 2;
                        if (d < bestD) { bestD = d; c = cc; r = rr; }
                    }
                }
                out.col[ni] = (int8_t)c; out.row[ni] = (int8_t)r;
                if (c > 0) { int f = fingerOf(c, a.h); out.finger[ni] = (int8_t)(f < 1 ? 1 : (f > 5 ? 5 : f)); }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Light compositor: guide > fingering > bars > key, sent as differences, a few per block.
// ---------------------------------------------------------------------------------------------
class LinnLights
{
public:
    static constexpr int kCols = 26, kRows = 8;

    LinnLights() { std::memset(want_, 0, sizeof(want_)); std::memset(sent_, 0, sizeof(sent_)); std::memset(layer_, 0, sizeof(layer_)); }

    // Layers: 0 key, 1 bars, 2 guide. Colour 0 = nothing from that layer.
    void clearLayer(int layer) { for (int c = 0; c < kCols; ++c) for (int r = 0; r < kRows; ++r) layer_[layer][c][r] = 0; dirty_ = true; }
    void setCell(int layer, int col, int row, uint8_t colour)
    {
        if (col < 0 || col >= kCols || row < 0 || row >= kRows) return;
        if (layer_[layer][col][row] != colour) { layer_[layer][col][row] = colour; dirty_ = true; }
    }
    // All cells of a pitch on the surface.
    void setNote(int layer, const LinnLayout& L, int pitch, uint8_t colour)
    { for (int r = 0; r < L.rows; ++r) { int c = L.colFor(pitch, r); if (c > 0) setCell(layer, c, r, colour); } }
    void setPitchClass(int layer, const LinnLayout& L, int pc, uint8_t colour)
    {
        for (int r = 0; r < L.rows; ++r)
            for (int c = 1; c <= L.cols; ++c)
                if (((L.note(c, r) % 12) + 12) % 12 == pc) setCell(layer, c, r, colour);
    }
    // Forget what the instrument shows (it was unplugged or switched off): repaint everything.
    void invalidate() { for (int c = 0; c < kCols; ++c) for (int r = 0; r < kRows; ++r) sent_[c][r] = 0xFF; dirty_ = true; }

    // Writes up to maxMsgs MIDI messages (3 bytes each) to out; returns the count.
    template <typename Send> int flush(int maxMsgs, Send send)
    {
        if (!dirty_) return 0;
        int msgs = 0;
        bool pending = false;
        for (int r = 0; r < kRows; ++r)
            for (int c = 1; c < kCols; ++c)
            {
                uint8_t w = layer_[2][c][r] ? layer_[2][c][r] : (layer_[1][c][r] ? layer_[1][c][r] : layer_[0][c][r]);
                uint8_t target = w ? w : (uint8_t)kLinnOff;
                if (sent_[c][r] == target || (sent_[c][r] == 0 && target == kLinnOff)) continue;
                if (msgs + 3 > maxMsgs) { pending = true; continue; }
                uint8_t m[3];
                if (cursorRow_ != r) { m[0] = 0xB0; m[1] = 21; m[2] = (uint8_t)r; send(m); ++msgs; cursorRow_ = r; }
                if (cursorCol_ != c) { m[0] = 0xB0; m[1] = 20; m[2] = (uint8_t)c; send(m); ++msgs; cursorCol_ = c; }
                m[0] = 0xB0; m[1] = 22; m[2] = target; send(m); ++msgs;
                sent_[c][r] = target == kLinnOff ? 0 : target;
            }
        dirty_ = pending;
        return msgs;
    }
    void resetCursor() { cursorCol_ = cursorRow_ = -1; }

private:
    uint8_t layer_[3][kCols][kRows];
    uint8_t want_[kCols][kRows];
    uint8_t sent_[kCols][kRows];
    bool dirty_ = false;
    int cursorCol_ = -1, cursorRow_ = -1;
};

} // namespace sr
#endif
