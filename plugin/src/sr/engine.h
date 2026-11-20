// engine.h - the practice engine: what the player must play now, whether they did, and what
// the screen shows. Runs on the audio thread: no allocation, no locks.
//
// Modes
//   Wait    the score waits on each step until the right notes are played (no clock).
//   Rhythm  a clock runs at the set tempo; notes count when they land inside the timing
//           window around their step; steps the clock passes unplayed are misses.
//   Flow    the clock runs, but a step that is not complete when its window closes holds the
//           clock for up to Hold beats before it is marked missed and the music moves on.
// Slack: pitch tolerance (exact, octave, a semitone), chord tolerance (all, all but one, any),
// the timing window, and the hold time.
//
// SPDX-License-Identifier: MIT
#ifndef SR_ENGINE_H
#define SR_ENGINE_H

#include "score.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sr {

struct EngineSettings
{
    int mode = 0;            // 0 wait, 1 flow, 2 rhythm
    double tempoFactor = 1.0;
    int hands = 0;           // 0 both, 1 right, 2 left
    int view = 0;            // 0 halves, 1 page, 2 bars, 3 steps
    bool loop = false; int loopFrom = 1; int loopBars = 4;
    double timingMs = 160;
    int pitchTol = 0;        // 0 exact, 1 octave, 2 within a semitone
    int chordTol = 0;        // 0 all, 1 all but one, 2 any
    double holdBeats = 2;    // < 0: forever
    int wrong = 1;           // 0 ignore, 1 count, 2 stop (rhythm/flow: wrong notes hold the clock)
    int countInBars = 1;
    int metro = 0;           // 0 off, 1 on, 2 accent
    int remindEvery = 10;    // 0 off
    int remindMax = 3;
    int rangeLo = 21, rangeHi = 108;
    int outRange = 1;        // 0 play everything, 1 skip notes outside the range
    bool linnSkip = false;   // 0.5.5: notes the LinnStrument cannot play are not asked for
    int linnLo = 0, linnHi = 127;
    int mask = 0;            // 0 off, 1 hide played, 2 hide the current bar
    int startMode = 0;       // 0 first note, 1 play button, 2 MPC transport
    int ramp = 0;            // tempo ramp per clean loop pass, percent
    int goal = 90;
};

enum class EvType : uint8_t { Wrong, Miss, Hit, StepDone, Reminder, Beat, Accent, CountIn, PassDone, PieceDone, Started, Stopped, AccompOn, Ramp };
struct EngineEvent { EvType type; int32_t a = 0, b = 0; };

enum class RunState : uint8_t { Idle, CountIn, Running, Holding, Paused, Done };

class Engine
{
public:
    static constexpr int kMaxEvents = 256;
    static constexpr int kScreen = gen::kSlots;

    // ---- score -------------------------------------------------------------------------------
    // The score must outlive its use here; stepState and stepWrong must have one byte per step
    // and noteState one byte per note (allocated by the loader).
    void setScore(const Score* s, uint8_t* stepState, uint8_t* noteState, uint8_t* stepWrong = nullptr)
    {
        score_ = s; stepState_ = stepState; noteState_ = noteState; stepWrong_ = stepWrong;
        resetPosition(true);
    }
    const Score* score() const { return score_; }

    // ---- control -----------------------------------------------------------------------------
    void restart() { resetPosition(true); }

    // Moves the practice position to step s (a bar or note picked in the header, the MPC's
    // playhead): from s on everything is open again, what came before keeps its marks. A clock
    // that runs keeps running from there.
    void locateStep(int s)
    {
        if (!score_ || score_->steps.empty()) return;
        const int n = (int)score_->steps.size();
        s = s < 0 ? 0 : (s >= n ? n - 1 : s);
        for (int k = s; k < n; ++k)
        {
            stepState_[k] = 0;
            if (stepWrong_) stepWrong_[k] = 0;
            const Step& st = score_->steps[(size_t)k];
            for (uint32_t j = 0; j < st.count; ++j) noteState_[score_->stepNotes[st.firstNote + j]] = 0;
        }
        step_ = s;
        pos_ = score_->steps[(size_t)s].time - 1;
        accompStep_ = s;
        accPos_ = score_->steps[(size_t)s].time;
        lastDoneStep_ = -1;
        holdLeft_ = 0;
        if (state_ == RunState::Done) state_ = RunState::Idle;
        if (state_ == RunState::Holding) state_ = RunState::Running;
        winStart_[0] = winStart_[1] = -1;
        skipUnrequired(false, false);
    }
    // The first step at or after a score time (the last step when none is).
    int stepAtOrAfter(int32_t unit) const
    {
        if (!score_ || score_->steps.empty()) return 0;
        int lo = 0, hi = (int)score_->steps.size() - 1;
        while (lo < hi) { int mid = (lo + hi) / 2; if (score_->steps[(size_t)mid].time < unit) lo = mid + 1; else hi = mid; }
        return lo;
    }
    void setPlaying(bool on, const EngineSettings& st)
    {
        if (!score_) return;
        if (on)
        {
            if (state_ == RunState::Done) resetPosition(true);
            if (state_ == RunState::Idle || state_ == RunState::Paused)
            {
                if (st.mode != 0 && st.countInBars > 0 && state_ == RunState::Idle) beginCountIn(st);
                else state_ = RunState::Running;
                push(EvType::Started);
            }
        }
        else if (state_ == RunState::Running || state_ == RunState::Holding || state_ == RunState::CountIn)
        {
            state_ = RunState::Paused; push(EvType::Stopped);
        }
    }
    RunState state() const { return state_; }
    bool playing() const { return state_ == RunState::Running || state_ == RunState::Holding || state_ == RunState::CountIn; }

    // ---- per block ---------------------------------------------------------------------------
    struct NoteIn { int offset; uint8_t pitch; bool on; };

    void process(const EngineSettings& st, const NoteIn* notes, int numNotes, int frames, double sampleRate)
    {
        numEvents_ = 0;
        if (!score_ || score_->steps.empty()) { clock_ += frames / sampleRate; return; }
        settings_ = st;
        double unitsPerSec = currentBpm() * st.tempoFactor / 60.0 * kUPQ;
        int done = 0;
        for (int i = 0; i <= numNotes; ++i)
        {
            int until = i < numNotes ? notes[i].offset : frames;
            until = until < done ? done : (until > frames ? frames : until);
            advanceClock((until - done) / sampleRate, unitsPerSec);
            clock_ += (until - done) / sampleRate;
            done = until;
            if (i < numNotes)
            {
                if (notes[i].on) noteOn(notes[i].pitch);
            }
        }
        activeSeconds_ += playing() ? frames / sampleRate : 0.0;
    }

    // ---- results -----------------------------------------------------------------------------
    int numEvents() const { return numEvents_; }
    const EngineEvent& event(int i) const { return events_[i]; }

    int currentStep() const { return step_; }
    int lastDoneStep() const { return lastDoneStep_; }
    double secondsSinceDone() const { return clock_ - lastDoneAt_; }
    double position() const { return pos_; }
    // Where the accompaniment is (Wait mode has its own clock for it), for its note-offs.
    double accompPosition() const { return settings_.mode == 0 ? accPos_ : pos_; }
    int hits() const { return hits_; }
    int misses() const { return misses_; }
    int wrongs() const { return wrongs_; }
    int streak() const { return streak_; }
    double activeSeconds() const { return activeSeconds_; }
    int accuracyPercent() const
    {
        int total = hits_ + misses_ + wrongs_;
        return total ? (int)std::lround(100.0 * hits_ / total) : 0;
    }
    double meanTimingMs() const { return timedHits_ ? timingSumMs_ / timedHits_ : 0.0; }
    double effectiveBpm(const EngineSettings& st) const { return currentBpm() * st.tempoFactor; }

    // Required notes of a step under the current settings.
    bool required(const PlayNote& n) const
    {
        const EngineSettings& st = settings_;
        if (st.hands == 1 && n.hand != 0) return false;
        if (st.hands == 2 && n.hand != 1) return false;
        if (st.outRange == 1 && (n.pitch < st.rangeLo || n.pitch > st.rangeHi)) return false;
        if (st.linnSkip && (n.pitch < st.linnLo || n.pitch > st.linnHi)) return false;
        return true;
    }
    // The notes of step s that must be played (for LinnStrument guide lights, note names).
    template <typename F> void forEachRequired(int s, F f) const
    {
        if (!score_ || s < 0 || s >= (int)score_->steps.size()) return;
        const Step& stp = score_->steps[(size_t)s];
        for (uint32_t k = 0; k < stp.count; ++k)
        {
            uint32_t ni = score_->stepNotes[stp.firstNote + k];
            const PlayNote& n = score_->notes[ni];
            if (required(n)) f(n, noteState_ ? noteState_[ni] : 0);
        }
    }
    int nextRequiredStep(int from) const
    {
        if (!score_) return -1;
        for (int s = from; s < (int)score_->steps.size(); ++s)
        {
            if (s >= loopEndStep_ && settings_.loop) return -1;
            bool any = false;
            forEachRequired(s, [&any](const PlayNote&, uint8_t) { any = true; });
            if (any) return s;
        }
        return -1;
    }

    // ---- screen ------------------------------------------------------------------------------
    // While LISTEN plays, the screen follows its position instead of the practice cursor
    // (-1: back to practice). The practice position itself does not move.
    void setListenSlot(int slot) { listenSlot_ = slot; }
    int listenSlot() const { return listenSlot_; }

    // Which score slot each screen column shows (-1: blank), with overlay frames.
    void computeScreen(int screenSlot[kScreen], uint8_t overlay[kScreen], int barline[kScreen + 1], int measureNum[2])
    {
        for (int i = 0; i < kScreen; ++i) { screenSlot[i] = -1; overlay[i] = 0; }
        for (int i = 0; i <= kScreen; ++i) barline[i] = 0;
        measureNum[0] = measureNum[1] = -1;
        if (!score_ || score_->slots.empty()) return;
        const Score& sc = *score_;
        const bool listening = listenSlot_ >= 0 && listenSlot_ < (int)sc.slots.size();
        int cur = listening ? listenSlot_ : currentSlot();
        const int view = settings_.view;
        if (view == 0)
        {   // halves: the other half refills while you read this one
            int half = kScreen / 2;
            ensureWindows(cur, half);
            fillWindow(0, winStart_[0], half, screenSlot, barline, measureNum[0]);
            fillWindow(half, winStart_[1], half, screenSlot, barline, measureNum[1]);
        }
        else if (view == 1)
        {   // page: turn when the cursor leaves the page
            if (cur < winStart_[0] || cur >= winEnd(winStart_[0], kScreen)) winStart_[0] = measureStartSlot(cur);
            fillWindow(0, winStart_[0], kScreen, screenSlot, barline, measureNum[0]);
        }
        else if (view == 2)
        {   // bars: the current bar is always the first on screen
            winStart_[0] = measureStartSlot(cur);
            fillWindow(0, winStart_[0], kScreen, screenSlot, barline, measureNum[0]);
        }
        else
        {   // steps: the cursor stays near the left edge and the music moves under it
            int start = cur - 2; if (start < 0) start = 0;
            fillWindow(0, start, kScreen, screenSlot, barline, measureNum[0]);
        }
        // overlays
        if (listening)
        {   // just the moving band: nothing has been played
            for (int i = 0; i < kScreen; ++i) overlay[i] = screenSlot[i] == cur ? 1 : 0;
            return;
        }
        int curStepSlot = (step_ >= 0 && step_ < (int)sc.steps.size()) ? sc.steps[(size_t)step_].slot : (int)sc.slots.size();
        int curMeasure = cur >= 0 && cur < (int)sc.slots.size() ? sc.slots[(size_t)cur].measure : -1;
        for (int i = 0; i < kScreen; ++i)
        {
            int k = screenSlot[i];
            if (k < 0) continue;
            const Slot& sl = sc.slots[(size_t)k];
            uint8_t ov = 0;
            bool passed = settings_.mode == 0 ? (k < curStepSlot) : (sl.time + 1 < (int32_t)pos_ && k < curStepSlot);
            if (passed)
            {
                ov = 2;   // played
                if (sl.step >= 0 && stepState_ && stepState_[sl.step] == 2) ov = 3;   // missed
                if (settings_.mask == 1) ov = 4;
            }
            if (k == cur && state_ != RunState::Done) ov = 1;
            if (settings_.mask == 2 && sl.measure == curMeasure && (playing() || state_ == RunState::Idle) && k >= cur) ov = 4;
            overlay[i] = ov;
        }
    }

    int currentSlot() const
    {
        if (!score_ || score_->slots.empty()) return 0;
        const Score& sc = *score_;
        if (settings_.mode == 0 || state_ == RunState::Idle)
            return (step_ >= 0 && step_ < (int)sc.steps.size()) ? sc.steps[(size_t)step_].slot : (int)sc.slots.size() - 1;
        // clock modes: the last slot whose time has been reached
        int lo = 0, hi = (int)sc.slots.size() - 1;
        int32_t p = (int32_t)std::floor(pos_ + 0.5);
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (sc.slots[(size_t)mid].time <= p) lo = mid; else hi = mid - 1; }
        return lo;
    }
    int currentMeasure() const
    {
        int s = currentSlot();
        return score_ && s >= 0 && s < (int)score_->slots.size() ? score_->slots[(size_t)s].measure : 0;
    }
    int loopStartStep() const { return loopStartStep_; }

private:
    // ---- helpers -----------------------------------------------------------------------------
    void push(EvType t, int32_t a = 0, int32_t b = 0)
    {
        if (numEvents_ < kMaxEvents) { events_[numEvents_].type = t; events_[numEvents_].a = a; events_[numEvents_].b = b; ++numEvents_; }
    }

    double currentBpm() const
    {
        if (!score_ || score_->tempo.empty()) return 100.0;
        double us = score_->tempo[0].usPerQuarter;
        for (const TempoPoint& t : score_->tempo) { if (t.unit > (int32_t)pos_) break; us = t.usPerQuarter; }
        return 60000000.0 / us;
    }

    void computeLoop()
    {
        loopStartStep_ = 0; loopEndStep_ = score_ ? (int)score_->steps.size() : 0;
        loopStartUnit_ = 0; loopEndUnit_ = score_ ? score_->endUnit : 0;
        if (!score_ || !settings_.loop) return;
        const Score& sc = *score_;
        int firstNum = sc.measures.empty() ? 1 : sc.measures[0].number;
        int m0 = settings_.loopFrom - firstNum; if (m0 < 0) m0 = 0;
        if (m0 >= (int)sc.measures.size()) m0 = (int)sc.measures.size() - 1;
        int m1 = m0 + settings_.loopBars; if (m1 > (int)sc.measures.size()) m1 = (int)sc.measures.size();
        loopStartUnit_ = sc.measures[(size_t)m0].start;
        loopEndUnit_ = m1 < (int)sc.measures.size() ? sc.measures[(size_t)m1].start : sc.endUnit;
        loopStartStep_ = (int)sc.steps.size(); loopEndStep_ = (int)sc.steps.size();
        for (int s = 0; s < (int)sc.steps.size(); ++s)
        {
            if (sc.steps[(size_t)s].time >= loopStartUnit_ && loopStartStep_ == (int)sc.steps.size()) loopStartStep_ = s;
            if (sc.steps[(size_t)s].time >= loopEndUnit_) { loopEndStep_ = s; break; }
        }
    }

    void resetPosition(bool clearStats)
    {
        state_ = RunState::Idle;
        computeLoop();
        if (score_)
        {
            int from = settings_.loop ? loopStartStep_ : 0;
            for (size_t s = (size_t)from; s < score_->steps.size(); ++s) if (stepState_) stepState_[s] = 0;
            for (size_t s = (size_t)from; s < score_->steps.size(); ++s) if (stepWrong_) stepWrong_[s] = 0;
            for (size_t s = (size_t)from; s < score_->steps.size(); ++s)
            {
                const Step& stp = score_->steps[s];
                for (uint32_t k = 0; k < stp.count; ++k) if (noteState_) noteState_[score_->stepNotes[stp.firstNote + k]] = 0;
            }
            step_ = from;
            pos_ = settings_.loop ? loopStartUnit_ : 0;
            if (from < (int)score_->steps.size()) pos_ = std::min<double>(pos_, score_->steps[(size_t)from].time);
            // a clock mode starts slightly before the first note so it can be played on time
            pos_ -= 1;
        }
        else { step_ = 0; pos_ = 0; }
        holdLeft_ = 0; countInElapsed_ = 0; countInNext_ = 0;
        accompStep_ = step_;
        accPos_ = step_ < (int)(score_ ? score_->steps.size() : 0) ? score_->steps[(size_t)step_].time : 0;
        lastDoneStep_ = -1; lastDoneAt_ = -1e9;
        winStart_[0] = winStart_[1] = -1;
        if (clearStats) { hits_ = misses_ = wrongs_ = streak_ = 0; timedHits_ = 0; timingSumMs_ = 0; activeSeconds_ = 0; remindCountdown_ = 0; remindersLeft_ = 0; }
        skipUnrequired(false, false);
    }

    void beginCountIn(const EngineSettings& st)
    {
        state_ = RunState::CountIn;
        const Measure& m = score_->measures[(size_t)score_->measureAt(std::max<int32_t>(0, (int32_t)pos_))];
        int beatsPerBar = (m.tsDen == 8 && m.tsNum % 3 == 0 && m.tsNum > 3) ? m.tsNum / 3 : m.tsNum;
        countInBeats_ = beatsPerBar * st.countInBars;
        countInElapsed_ = 0; countInNext_ = 0;
    }

    double beatUnits() const
    {
        if (!score_) return kUPQ;
        const Measure& m = score_->measures[(size_t)score_->measureAt(std::max<int32_t>(0, (int32_t)pos_))];
        if (m.tsDen == 8 && m.tsNum % 3 == 0 && m.tsNum > 3) return 72;
        return (double)kWholeUnits / (m.tsDen ? m.tsDen : 4);
    }

    // Wait mode skips steps nothing is required in (the other hand, notes out of range). Their
    // notes are the accompaniment: since 0.5.5 they are sent in time by the accompaniment clock
    // (runAccompClock), not all at once as they are skipped.
    void skipUnrequired(bool emit, bool mayFinish = true)
    {
        if (!score_) return;
        int n = (int)score_->steps.size();
        while (step_ < n && (!settings_.loop || step_ < loopEndStep_))
        {
            bool any = false;
            forEachRequired(step_, [&any](const PlayNote&, uint8_t) { any = true; });
            if (any) break;
            if (emit) emitAccomp(step_);
            if (stepState_) stepState_[step_] = 1;
            ++step_;
        }
        if (mayFinish && (step_ >= n || (settings_.loop && step_ >= loopEndStep_))) finishPass();
    }

    // Wait mode's accompaniment clock: moves at the tempo from the last step you played towards
    // the next one that wants you, sending the notes you are not asked for as it passes them, and
    // stops just short of that step (so a held accompaniment note sounds while it waits). After
    // the last step that wants you it runs on to the end of the piece.
    void runAccompClock(double dt, double unitsPerSec)
    {
        const int n = (int)score_->steps.size();
        if (accompStep_ >= n) return;
        const bool tail = state_ == RunState::Done || step_ >= n;
        const double limit = tail ? (double)score_->endUnit : score_->steps[(size_t)step_].time - 1.0;
        if (accPos_ < limit) accPos_ = std::min(limit, accPos_ + dt * unitsPerSec);
        const int stop = tail ? n : step_;
        while (accompStep_ < stop && score_->steps[(size_t)accompStep_].time <= accPos_) emitAccomp(accompStep_++);
    }
    void emitAccompUpTo(double t)
    {
        const int n = (int)score_->steps.size();
        while (accompStep_ < n && score_->steps[(size_t)accompStep_].time <= t) emitAccomp(accompStep_++);
    }
    void emitAccomp(int s)
    {
        if (!score_) return;
        const Step& stp = score_->steps[(size_t)s];
        for (uint32_t k = 0; k < stp.count; ++k)
        {
            uint32_t ni = score_->stepNotes[stp.firstNote + k];
            const PlayNote& n = score_->notes[ni];
            if (!required(n)) push(EvType::AccompOn, n.pitch, (int32_t)ni);
        }
    }

    bool pitchMatches(int played, int expected) const
    {
        if (played == expected) return true;
        if (settings_.pitchTol == 1) return (played % 12) == (expected % 12);
        if (settings_.pitchTol == 2) return std::abs(played - expected) <= 1;
        return false;
    }

    // Try to mark a note of step s as played. Returns true when it matched.
    bool matchInStep(int s, int pitch, double timingUnits)
    {
        const Step& stp = score_->steps[(size_t)s];
        int best = -1;
        for (uint32_t k = 0; k < stp.count; ++k)
        {
            uint32_t ni = score_->stepNotes[stp.firstNote + k];
            const PlayNote& n = score_->notes[ni];
            if (!required(n) || noteState_[ni] == 1) continue;
            if (n.pitch == pitch) { best = (int)ni; break; }
            if (best < 0 && pitchMatches(pitch, n.pitch)) best = (int)ni;
        }
        if (best < 0) return false;
        noteState_[best] = 1;
        ++hits_; ++streak_;
        if (timingUnits == timingUnits)   // not NaN: clock modes record timing
        {
            double ms = timingUnits / (currentBpm() * settings_.tempoFactor / 60.0 * kUPQ) * 1000.0;
            timingSumMs_ += ms; ++timedHits_;
        }
        push(EvType::Hit, pitch, s);
        tickReminder();
        return true;
    }

    bool stepComplete(int s) const
    {
        const Step& stp = score_->steps[(size_t)s];
        int need = 0, got = 0;
        for (uint32_t k = 0; k < stp.count; ++k)
        {
            uint32_t ni = score_->stepNotes[stp.firstNote + k];
            if (!required(score_->notes[ni])) continue;
            ++need; if (noteState_[ni] == 1) ++got;
        }
        if (need == 0) return true;
        if (settings_.chordTol == 1) return got >= (need > 1 ? need - 1 : 1);
        if (settings_.chordTol == 2) return got >= 1;
        return got >= need;
    }

    void markStepMissed(int s)
    {
        const Step& stp = score_->steps[(size_t)s];
        int missed = 0;
        for (uint32_t k = 0; k < stp.count; ++k)
        {
            uint32_t ni = score_->stepNotes[stp.firstNote + k];
            if (required(score_->notes[ni]) && noteState_[ni] != 1) { noteState_[ni] = 2; ++missed; }
        }
        if (missed) { misses_ += missed; stepState_[s] = 2; error(EvType::Miss, s); }
        else stepState_[s] = 1;
    }

    void error(EvType t, int32_t a, int32_t b = 0)
    {
        streak_ = 0;
        push(t, a, b);
        // reminders: every Nth note after an error, a few times
        if (settings_.remindEvery > 0) { remindCountdown_ = settings_.remindEvery; remindersLeft_ = settings_.remindMax; }
    }
    void tickReminder()
    {
        if (remindersLeft_ <= 0 || remindCountdown_ <= 0) return;
        if (--remindCountdown_ == 0)
        {
            push(EvType::Reminder);
            if (--remindersLeft_ > 0) remindCountdown_ = settings_.remindEvery;
        }
    }

    // A note already played right in the current step, or in the step just completed (within
    // kRestrikeSeconds), is a re-strike: a repeated chord, a bounce, a finger landing twice.
    // It is ignored rather than counted wrong.
    static constexpr double kRestrikeSeconds = 0.6;
    bool playedIn(int s, int pitch) const
    {
        if (s < 0 || s >= (int)score_->steps.size()) return false;
        const Step& stp = score_->steps[(size_t)s];
        for (uint32_t k = 0; k < stp.count; ++k)
        {
            uint32_t ni = score_->stepNotes[stp.firstNote + k];
            if (noteState_[ni] == 1 && pitchMatches(pitch, score_->notes[ni].pitch)) return true;
        }
        return false;
    }
    bool isRestrike(int pitch) const
    {
        if (playedIn(step_, pitch)) return true;
        return lastDoneStep_ >= 0 && clock_ - lastDoneAt_ <= kRestrikeSeconds && playedIn(lastDoneStep_, pitch);
    }
    // The step a wrong note belongs to: the current one in Wait mode, the nearest in time otherwise.
    int stepForWrong() const
    {
        const int n = (int)score_->steps.size();
        if (n == 0) return -1;
        int s = step_ < n ? step_ : n - 1;
        if (settings_.mode != 0 && s > 0 && std::fabs(score_->steps[(size_t)s - 1].time - pos_) < std::fabs(score_->steps[(size_t)s].time - pos_)) --s;
        return s;
    }
    void wrongNote(int pitch)
    {
        ++wrongs_;
        int s = stepForWrong();
        if (stepWrong_ && s >= 0) stepWrong_[s] = (uint8_t)pitch;
        error(EvType::Wrong, pitch, s);
    }

    void noteOn(int pitch)
    {
        if (!score_) return;
        const int n = (int)score_->steps.size();
        if (state_ == RunState::Done) return;
        if (settings_.mode == 0)
        {
            // Wait mode: the first correct note also starts the session.
            if (state_ == RunState::Idle || state_ == RunState::Paused) { state_ = RunState::Running; push(EvType::Started); }
            if (step_ >= n) return;
            if (matchInStep(step_, pitch, NAN))
            {
                if (stepComplete(step_)) completeStep(true);
            }
            else if (isRestrike(pitch)) {}
            else if (settings_.wrong != 0) wrongNote(pitch);
            return;
        }
        // Clock modes
        if (state_ == RunState::Idle)
        {
            if (settings_.startMode == 0)
            {   // the first note starts the clock, on time
                if (step_ < n && matchInStep(step_, pitch, 0.0))
                {
                    pos_ = score_->steps[(size_t)step_].time;
                    state_ = RunState::Running; push(EvType::Started);
                    if (stepComplete(step_)) completeStep(true);
                    return;
                }
                return;   // wrong notes before the start do not count
            }
            return;
        }
        if (state_ == RunState::CountIn || state_ == RunState::Paused) return;
        double winUnits = settings_.timingMs / 1000.0 * (currentBpm() * settings_.tempoFactor / 60.0 * kUPQ);
        // earliest open step whose window holds the note
        for (int s = step_; s < n; ++s)
        {
            double t = score_->steps[(size_t)s].time;
            if (t - winUnits > pos_) break;
            if (stepState_[s] != 0) continue;
            if (matchInStep(s, pitch, pos_ - t))
            {
                if (s == step_ && stepComplete(s))
                {
                    if (state_ == RunState::Holding) state_ = RunState::Running;
                    completeStep(true);
                }
                return;
            }
        }
        if (isRestrike(pitch)) return;
        for (int s = step_ - 1; s >= 0 && s >= step_ - 4; --s)
        {   // a note of a step passed moments ago, played again inside its window
            if (score_->steps[(size_t)s].time + winUnits < pos_) break;
            if (playedIn(s, pitch)) return;
        }
        if (settings_.wrong != 0)
        {
            wrongNote(pitch);
            if (settings_.wrong == 2 && state_ == RunState::Running) { state_ = RunState::Holding; holdLeft_ = -1; }
        }
    }

    void completeStep(bool ok)
    {
        stepState_[step_] = ok ? 1 : 2;
        push(EvType::StepDone, step_);
        lastDoneStep_ = step_; lastDoneAt_ = clock_;
        ++step_;
        const int n = (int)score_->steps.size();
        // In clock modes, steps already complete (played early) are passed over.
        while (settings_.mode != 0 && step_ < n && stepState_[step_] == 0 && stepComplete(step_) &&
               score_->steps[(size_t)step_].time <= pos_)
        { stepState_[step_] = 1; ++step_; }
        if (settings_.mode == 0)
        {
            // the accompaniment catches up to the step just played (its other-hand notes sound
            // with it), then runs in time towards the next step that wants you, and waits there
            const double done = score_->steps[(size_t)(step_ - 1)].time;
            emitAccompUpTo(done);
            accPos_ = done;
            if (step_ < n) pos_ = score_->steps[(size_t)step_].time;
            skipUnrequired(false);
        }
        else if (step_ >= n || (settings_.loop && step_ >= loopEndStep_))
        {
            // wait for the clock to reach the end of the bar before finishing
        }
    }

    void finishPass()
    {
        if (finishing_) return;
        finishing_ = true;
        int acc = accuracyPercent();
        push(EvType::PassDone, acc, (int32_t)activeSeconds_);
        if (settings_.loop && (hits_ + misses_ + wrongs_) > 0)
        {
            if (settings_.ramp > 0 && acc >= settings_.goal) push(EvType::Ramp, acc);
            resetPosition(false);
            hits_ = misses_ = wrongs_ = 0;
            if (settings_.mode != 0)
            {
                state_ = RunState::Running;
                pos_ = loopStartUnit_ - 1;
            }
            else state_ = RunState::Running;
        }
        else { state_ = RunState::Done; push(EvType::PieceDone, acc); }
        finishing_ = false;
    }

    // Moves the clock; handles count-in, metronome, misses, holds and the end of a pass.
    void advanceClock(double dt, double unitsPerSec)
    {
        if (dt <= 0 || !score_) return;
        if (state_ == RunState::CountIn)
        {
            double beatSec = beatUnits() / unitsPerSec;
            countInElapsed_ += dt / beatSec;
            while (countInNext_ < countInBeats_ && countInElapsed_ >= countInNext_)
            {
                push(countInNext_ == 0 ? EvType::Accent : EvType::CountIn, countInBeats_ - countInNext_);
                ++countInNext_;
            }
            if (countInElapsed_ >= countInBeats_) state_ = RunState::Running;
            return;
        }
        if (settings_.mode == 0 && (state_ == RunState::Running || state_ == RunState::Done)) runAccompClock(dt, unitsPerSec);
        if (settings_.mode == 0 || state_ != RunState::Running)
        {
            if (state_ == RunState::Holding && holdLeft_ > 0)
            {
                holdLeft_ -= dt;
                if (holdLeft_ <= 0)
                {   // gave up waiting: the step is missed and the clock moves on
                    if (step_ < (int)score_->steps.size()) { markStepMissed(step_); ++step_; }
                    state_ = RunState::Running;
                }
            }
            return;
        }
        const int n = (int)score_->steps.size();
        double winUnits = settings_.timingMs / 1000.0 * unitsPerSec;
        double newPos = pos_ + dt * unitsPerSec;
        // metronome
        if (settings_.metro > 0)
        {
            double bu = beatUnits();
            int64_t b0 = (int64_t)std::floor(pos_ / bu), b1 = (int64_t)std::floor(newPos / bu);
            if (b1 > b0 && newPos >= 0)
            {
                const Measure& m = score_->measures[(size_t)score_->measureAt((int32_t)newPos)];
                bool downbeat = std::fmod(b1 * bu - m.start, (double)m.len) < 0.5;
                push(downbeat && settings_.metro == 2 ? EvType::Accent : EvType::Beat);
            }
        }
        // steps whose window closes now
        while (step_ < n && (!settings_.loop || step_ < loopEndStep_))
        {
            double closeAt = score_->steps[(size_t)step_].time + winUnits;
            if (closeAt > newPos) break;
            if (stepState_[step_] == 0 && !stepComplete(step_))
            {
                if (settings_.mode == 1 && settings_.holdBeats != 0)
                {   // Flow: stop the clock at the window's end and wait
                    pos_ = closeAt;
                    state_ = RunState::Holding;
                    holdLeft_ = settings_.holdBeats < 0 ? 1e9 : settings_.holdBeats * beatUnits() / unitsPerSec;
                    return;
                }
                markStepMissed(step_);
            }
            else if (stepState_[step_] == 0) stepState_[step_] = 1;
            ++step_;
        }
        pos_ = newPos;
        while (accompStep_ < n && score_->steps[(size_t)accompStep_].time <= pos_) emitAccomp(accompStep_++);
        double endUnit = settings_.loop ? loopEndUnit_ : score_->endUnit;
        if (pos_ >= endUnit + (settings_.loop ? 0 : winUnits)) finishPass();
    }

    // ---- windows -----------------------------------------------------------------------------
    int measureStartSlot(int slot) const
    {
        if (!score_ || slot < 0 || slot >= (int)score_->slots.size()) return 0;
        return score_->measures[score_->slots[(size_t)slot].measure].firstSlot;
    }
    // End (exclusive) of a window of whole measures starting at slot start, at most cap slots.
    int winEnd(int start, int cap) const
    {
        const Score& sc = *score_;
        int n = (int)sc.slots.size();
        if (start >= n) return n;
        int end = start;
        int m = sc.slots[(size_t)start].measure;
        while (m < (int)sc.measures.size())
        {
            const Measure& ms = sc.measures[(size_t)m];
            int mEnd = ms.firstSlot + ms.numSlots;
            if (mEnd - start > cap) { if (end == start) end = start + cap; break; }
            end = mEnd; ++m;
        }
        return end > n ? n : end;
    }
    void ensureWindows(int cur, int half)
    {
        // windows hold consecutive stretches of music: A then B then A again further on
        if (winStart_[0] < 0 || cur < std::min(winStart_[0], winStart_[1]))
        {
            winStart_[0] = measureStartSlot(cur);
            winStart_[1] = winEnd(winStart_[0], half);
            return;
        }
        int a = winStart_[0] <= winStart_[1] ? 0 : 1;      // the earlier window
        int b = 1 - a;
        int aEnd = winEnd(winStart_[a], half), bEnd = winEnd(winStart_[b], half);
        if (cur >= winStart_[b] && cur < bEnd)
        {   // reading the later window: refill the earlier one with what follows (at the end of
            // the piece nothing follows, and the earlier music stays)
            if (bEnd < (int)score_->slots.size()) winStart_[a] = bEnd;
            return;
        }
        if (cur >= winStart_[a] && cur < aEnd) return;
        // jumped (loop, restart): start over
        winStart_[0] = measureStartSlot(cur);
        winStart_[1] = winEnd(winStart_[0], half);
    }
    void fillWindow(int screen0, int start, int cap, int screenSlot[], int barline[], int& measureNum) const
    {
        const Score& sc = *score_;
        int n = (int)sc.slots.size();
        if (start < 0 || start >= n) return;
        int end = winEnd(start, cap);
        for (int k = start; k < end; ++k)
        {
            int i = screen0 + (k - start);
            screenSlot[i] = k;
            if (k > start) barline[i] = sc.slots[(size_t)k].barline;
        }
        int lastScreen = screen0 + (end - start);
        if (end >= n) barline[lastScreen] = 3;                         // final barline
        else if (end > start && sc.slots[(size_t)end].measureStart) barline[lastScreen] = 1;
        if (settings_.loop && end > start && sc.slots[(size_t)(end - 1)].time < loopEndUnit_ && end < n && sc.slots[(size_t)end].time >= loopEndUnit_)
            barline[lastScreen] = 5;                                   // repeat end at the loop's end
        int m = sc.slots[(size_t)start].measure;
        measureNum = sc.measures[(size_t)m].number;
    }

    // ---- state -------------------------------------------------------------------------------
    const Score* score_ = nullptr;
    uint8_t* stepState_ = nullptr;     // 0 open, 1 done, 2 missed
    uint8_t* noteState_ = nullptr;     // 0 open, 1 played, 2 missed
    uint8_t* stepWrong_ = nullptr;     // last wrong pitch played at each step (0: none)
    double clock_ = 0;                 // seconds of audio processed
    int lastDoneStep_ = -1; double lastDoneAt_ = -1e9;
    EngineSettings settings_;
    RunState state_ = RunState::Idle;
    int step_ = 0;
    double pos_ = 0;                   // score units (clock modes)
    double holdLeft_ = 0;              // seconds
    int countInBeats_ = 0, countInNext_ = 0; double countInElapsed_ = 0;
    int accompStep_ = 0;
    double accPos_ = 0;
    bool finishing_ = false;
    int hits_ = 0, misses_ = 0, wrongs_ = 0, streak_ = 0;
    int timedHits_ = 0; double timingSumMs_ = 0;
    double activeSeconds_ = 0;
    int remindCountdown_ = 0, remindersLeft_ = 0;
    int loopStartStep_ = 0, loopEndStep_ = 0; int32_t loopStartUnit_ = 0, loopEndUnit_ = 0;
    mutable int winStart_[2] = {-1, -1};
    int listenSlot_ = -1;
    EngineEvent events_[kMaxEvents];
    int numEvents_ = 0;
};

} // namespace sr
#endif
