// listen.h - LISTEN: plays the loaded piece as MIDI, in time, at a chosen speed, so you can hear
// how it goes before (or between) practising it. The notes go out through the plugin's own
// "MIDI Out" port to whatever instrument track listens to it; the plugin moves the cursor along.
//
// Plays the score's notes (the quantised version that is on screen) with their velocities,
// following the file's tempo map scaled by a factor, from `from` to `to` (score units), once or
// round and round. Runs on the audio thread: no allocation, no locks. Events are sent at block
// boundaries (128 samples is under 3 ms at 44.1 kHz).
//
// SPDX-License-Identifier: MIT
#ifndef SR_LISTEN_H
#define SR_LISTEN_H

#include "score.h"

#include <algorithm>
#include <cstdint>

namespace sr {

class ListenPlayer
{
public:
    static constexpr int kVoices = 32;
    struct Voice { int16_t pitch = -1; int32_t end = 0; int32_t note = -1; };

    // hands: bit 0 right, bit 1 left. Returns false when there is nothing to play.
    bool start(const Score* sc, int32_t from, int32_t to, bool loop, int hands)
    {
        score_ = sc; from_ = from; to_ = to; loop_ = loop; hands_ = hands;
        for (Voice& v : voices_) v = Voice();
        playing_ = sc != nullptr && to > from && !sc->notes.empty();
        pos_ = from; next_ = firstFrom(from);
        return playing_;
    }
    // send(status, pitch, velocity): 0x90 note on, 0x80 note off (channel added by the caller).
    template <typename Send> void stop(Send send) { allOff(send); playing_ = false; }
    // 0.5.5 transport. Pause: the caller stops calling process() after silence(); resuming is
    // calling it again, from where it was.
    template <typename Send> void silence(Send send) { allOff(send); }
    // Moves to a score time: inside the loop while looping; otherwise anywhere up to the end
    // (seeking back before where it started plays from there).
    template <typename Send> void seek(int32_t unit, Send send)
    {
        allOff(send);
        if (loop_) unit = std::max(from_, std::min(to_ - 1, unit));
        else { if (unit < from_) from_ = unit; if (unit >= to_) unit = to_ - 1; }
        if (unit < 0) unit = 0;
        pos_ = unit; next_ = firstFrom(unit);
    }
    // Loop switched on or off, or moved, while it plays.
    template <typename Send> void setRange(int32_t from, int32_t to, bool loop, Send send)
    {
        if (to <= from) return;
        from_ = from; to_ = to; loop_ = loop;
        if (pos_ < from_ || pos_ >= to_) seek(from_, send);
    }
    bool playing() const { return playing_; }
    double position() const { return pos_; }
    int32_t from() const { return from_; }
    int32_t to() const { return to_; }

    // Moves on by `frames`. Returns false once the end is reached (not looping); every note is
    // off by then.
    template <typename Send> bool process(int frames, double sampleRate, double tempoFactor, Send send)
    {
        if (!playing_ || !score_) return false;
        double unitsPerSec = score_->bpmAt(pos_) * tempoFactor / 60.0 * kUPQ;
        double newPos = pos_ + frames / sampleRate * unitsPerSec;
        double upto = std::min<double>(newPos, to_);
        for (Voice& v : voices_)
            if (v.pitch >= 0 && v.end <= upto) { send(0x80, v.pitch, 0); v.pitch = -1; }
        const std::vector<PlayNote>& notes = score_->notes;
        while (next_ < notes.size() && notes[next_].start < upto)
        {
            const PlayNote& pn = notes[next_];
            if (pn.start >= from_ && (hands_ & (1 << pn.hand))) noteOn(pn, (int32_t)next_, send);
            ++next_;
        }
        pos_ = newPos;
        if (pos_ < to_) return true;
        allOff(send);
        if (loop_) { pos_ = from_; next_ = firstFrom(from_); return true; }
        playing_ = false;
        return false;
    }

    // The notes sounding now: f(const PlayNote&, noteIndex).
    template <typename F> void forEachSounding(F f) const
    {
        if (!score_) return;
        for (const Voice& v : voices_)
            if (v.pitch >= 0 && v.note >= 0) f(score_->notes[(size_t)v.note], v.note);
    }

private:
    size_t firstFrom(int32_t unit) const
    {
        if (!score_) return 0;
        const std::vector<PlayNote>& n = score_->notes;
        return (size_t)(std::lower_bound(n.begin(), n.end(), unit, [](const PlayNote& a, int32_t u) { return a.start < u; }) - n.begin());
    }
    template <typename Send> void noteOn(const PlayNote& pn, int32_t index, Send send)
    {
        // the same pitch still sounding (a repeated or overlapping note): end it first
        for (Voice& v : voices_) if (v.pitch == pn.pitch) { send(0x80, v.pitch, 0); v.pitch = -1; }
        Voice* slot = nullptr;
        for (Voice& v : voices_) if (v.pitch < 0) { slot = &v; break; }
        if (!slot)
        {   // more than kVoices at once: end the one closest to ending
            slot = &voices_[0];
            for (Voice& v : voices_) if (v.end < slot->end) slot = &v;
            send(0x80, slot->pitch, 0);
        }
        send(0x90, pn.pitch, pn.vel ? pn.vel : 80);
        slot->pitch = pn.pitch; slot->end = std::min(pn.end, to_); slot->note = index;
    }
    template <typename Send> void allOff(Send send)
    {
        for (Voice& v : voices_) if (v.pitch >= 0) { send(0x80, v.pitch, 0); v.pitch = -1; }
    }

    const Score* score_ = nullptr;
    int32_t from_ = 0, to_ = 0;
    bool loop_ = false, playing_ = false;
    int hands_ = 3;
    double pos_ = 0;
    size_t next_ = 0;
    Voice voices_[kVoices];
};

} // namespace sr
#endif
