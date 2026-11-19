// music.h - pitch spelling, staff positions, key signatures and scales.
//
// SPDX-License-Identifier: MIT
#ifndef SR_MUSIC_H
#define SR_MUSIC_H

#include <cstdint>
#include <cstring>
#include <string>

namespace sr {

// Letter 0..6 = C D E F G A B. Accidental -2..2 (flats negative).
struct Spelling { int8_t letter = 0; int8_t acc = 0; int8_t octave = 4; };

constexpr int kLetterPc[7] = {0, 2, 4, 5, 7, 9, 11};

// Pitch classes of the key signature's notes, as accidentals per letter.
// sf > 0: number of sharps (F C G D A E B), sf < 0: flats (B E A D G C F).
inline void keyAccidentals(int sf, int8_t out[7])
{
    for (int i = 0; i < 7; ++i) out[i] = 0;
    static const int sharpOrder[7] = {3, 0, 4, 1, 5, 2, 6};   // F C G D A E B
    static const int flatOrder[7] = {6, 2, 5, 1, 4, 0, 3};    // B E A D G C F
    if (sf > 0) for (int i = 0; i < sf && i < 7; ++i) out[sharpOrder[i]] = 1;
    if (sf < 0) for (int i = 0; i < -sf && i < 7; ++i) out[flatOrder[i]] = -1;
}

// Tonic pitch class of a key signature (major, or relative minor when minor is set).
inline int keyTonicPc(int sf, bool minor)
{
    int major = ((sf * 7) % 12 + 12) % 12;
    return minor ? (major + 9) % 12 : major;
}

// Spell a MIDI pitch in a key. Diatonic notes take their letter; chromatic notes become a
// sharp in sharp keys and on the leading tone, a flat otherwise. pref overrides that for
// chromatic notes: +1 when the melody rises a semitone next (D# -> E), -1 when it falls (Eb -> D).
inline Spelling spellPitch(int pitch, int sf, bool minor, int pref = 0)
{
    int8_t keyAcc[7]; keyAccidentals(sf, keyAcc);
    int pc = ((pitch % 12) + 12) % 12;
    Spelling s;
    // Diatonic?
    for (int l = 0; l < 7; ++l)
    {
        int lp = (kLetterPc[l] + keyAcc[l] + 12) % 12;
        if (lp == pc)
        {
            s.letter = (int8_t)l; s.acc = keyAcc[l];
            int basePitch = pitch - s.acc;            // pitch of the natural letter
            s.octave = (int8_t)(basePitch / 12 - 1);
            if (basePitch < 0) s.octave = (int8_t)((basePitch - 11) / 12 - 1);
            return s;
        }
    }
    int tonic = keyTonicPc(sf, minor);
    bool leading = ((pc + 1) % 12) == tonic;
    bool useSharp = sf > 0 || (sf == 0 && !(pc == 10)) || leading;   // in C, Bb reads better than A#
    if (sf == 0 && pc == 3 && !leading) useSharp = false;             // and Eb than D#
    if (sf == 0 && pc == 8 && !leading) useSharp = minor;             // G# in A minor, Ab in C major
    if (pref > 0) useSharp = true;
    if (pref < 0 && !leading) useSharp = false;
    // Raise or lower a neighbouring letter, taking that letter's key-signature accidental into account.
    for (int l = 0; l < 7; ++l)
    {
        int lp = (kLetterPc[l] + keyAcc[l] + 12) % 12;
        int want = useSharp ? (lp + 1) % 12 : (lp + 11) % 12;
        if (want == pc)
        {
            s.letter = (int8_t)l;
            s.acc = (int8_t)(keyAcc[l] + (useSharp ? 1 : -1));
            int basePitch = pitch - s.acc;
            s.octave = (int8_t)(basePitch / 12 - 1);
            return s;
        }
    }
    // Fallback: plain sharp spelling.
    static const int8_t letterOf[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static const int8_t accOf[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    s.letter = letterOf[pc]; s.acc = accOf[pc]; s.octave = (int8_t)(pitch / 12 - 1);
    return s;
}

// Diatonic step number: C4 = 4*7 + 0 = 28.
inline int diatonicIndex(const Spelling& s) { return s.octave * 7 + s.letter; }

// Staff position (0 = bottom line). Treble bottom line E4 (index 30), bass bottom line G2 (18).
inline int staffPosition(const Spelling& s, int staff) { return diatonicIndex(s) - (staff == 0 ? 30 : 18); }

// --- Scales -----------------------------------------------------------------------------------
struct ScaleDef { const char* name; uint16_t mask; };   // bit n = semitone n above the root

constexpr ScaleDef kScales[] = {
    {"Major", 0xAB5},           // 0 2 4 5 7 9 11
    {"Natural Minor", 0x5AD},   // 0 2 3 5 7 8 10
    {"Dorian", 0x6AD},          // 0 2 3 5 7 9 10
    {"Phrygian", 0x5AB},        // 0 1 3 5 7 8 10
    {"Lydian", 0xAD5},          // 0 2 4 6 7 9 11
    {"Mixolydian", 0x6B5},      // 0 2 4 5 7 9 10
    {"Locrian", 0x56B},         // 0 1 3 5 6 8 10
    {"Harmonic Minor", 0x9AD},  // 0 2 3 5 7 8 11
    {"Major Pentatonic", 0x295},// 0 2 4 7 9
    {"Minor Pentatonic", 0x4A9},// 0 3 5 7 10
    {"Blues", 0x4E9},           // 0 3 5 6 7 10
    {"Chromatic", 0xFFF},
    {"Melodic Minor", 0xAAD},   // 0 2 3 5 7 9 11
    {"Whole Tone", 0x555},
    {"Diminished", 0x6DB},      // 0 1 3 4 6 7 9 10 (half-whole)
    {"Minor", 0x5AD},           // aliases used by the MPC
    {"Aeolian", 0x5AD},
    {"Ionian", 0xAB5},
};
constexpr int kNumScales = (int)(sizeof(kScales) / sizeof(kScales[0]));

inline int pcFromName(const char* s, int* consumed)
{
    static const char letters[] = "CDEFGAB";
    *consumed = 0;
    if (!s || !*s) return -1;
    const char* p = std::strchr(letters, s[0] >= 'a' && s[0] <= 'g' ? s[0] - 32 : s[0]);
    if (!p) return -1;
    int pc = kLetterPc[p - letters];
    int n = 1;
    while (s[n] == '#' || s[n] == 'b') { pc += s[n] == '#' ? 1 : -1; ++n; }
    *consumed = n;
    return (pc % 12 + 12) % 12;
}

// Parses the MPC's key strings: "C Major", "D# Natural Minor", "Bb Dorian".
// Returns false when the scale is not known. mask is relative to the root.
inline bool parseKeyName(const std::string& text, int& rootPc, uint16_t& mask)
{
    int used = 0;
    rootPc = pcFromName(text.c_str(), &used);
    if (rootPc < 0) return false;
    std::string rest = text.substr((size_t)used);
    while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    if (rest.empty()) { mask = kScales[0].mask; return true; }
    for (int i = 0; i < kNumScales; ++i)
        if (rest == kScales[i].name) { mask = kScales[i].mask; return true; }
    // Case-insensitive prefix match ("Harmonic Min", "Maj Pent").
    for (int i = 0; i < kNumScales; ++i)
    {
        const char* a = kScales[i].name; size_t k = 0;
        while (k < rest.size() && a[k] && ((rest[k] | 32) == (a[k] | 32))) ++k;
        if (k >= 3 && k == rest.size()) { mask = kScales[i].mask; return true; }
    }
    return false;
}

// Key signature (sf) that best fits a root + scale, for spelling generated music.
inline int keySigFor(int rootPc, uint16_t mask, bool* minorOut)
{
    // Find the major key whose scale contains the most notes of the mask (ties: fewest accidentals).
    int best = 0, bestScore = -1;
    for (int sf = -6; sf <= 6; ++sf)
    {
        int majorRoot = ((sf * 7) % 12 + 12) % 12;
        int score = 0;
        for (int i = 0; i < 12; ++i)
            if (mask & (1 << i))
            {
                int pc = (rootPc + i) % 12;
                int rel = (pc - majorRoot + 12) % 12;
                if (0xAB5 & (1 << rel)) score += 2;
            }
        score = score * 16 - (sf < 0 ? -sf : sf);
        if (score > bestScore) { bestScore = score; best = sf; }
    }
    if (minorOut) *minorOut = (mask & 0x8) && !(mask & 0x10);   // has a minor third, no major third
    return best;
}

inline const char* pcName(int pc, bool flats)
{
    static const char* sharp[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* flat[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    pc = ((pc % 12) + 12) % 12;
    return flats ? flat[pc] : sharp[pc];
}

} // namespace sr
#endif
