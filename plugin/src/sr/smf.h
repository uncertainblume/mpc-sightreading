// smf.h - Standard MIDI File reader (format 0 and 1), no dependencies.
//
// Produces a flat list of notes (start/end tick, pitch, velocity, channel, track), plus the
// tempo map, time signatures, key signatures and track names. Running status, sysex and
// unknown meta events are handled; a truncated file yields what was read before the damage.
//
// SPDX-License-Identifier: MIT
#ifndef SR_SMF_H
#define SR_SMF_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sr {

struct SmfNote
{
    int32_t on = 0, off = 0;   // ticks
    uint8_t pitch = 60, vel = 64, chan = 0;
    int16_t track = 0;
};
struct SmfTempo   { int32_t tick; uint32_t usPerQuarter; };
struct SmfTimeSig { int32_t tick; uint8_t num, den; };          // den as a value: 4 means quarter
struct SmfKeySig  { int32_t tick; int8_t sf; uint8_t minor; };  // sf: -7..7 (flats negative)
struct SmfTrack   { std::string name; int notes = 0; double pitchSum = 0; int chanMask = 0; };

struct Smf
{
    int format = 0;
    int ppq = 480;
    std::vector<SmfNote> notes;
    std::vector<SmfTempo> tempos;
    std::vector<SmfTimeSig> timeSigs;
    std::vector<SmfKeySig> keySigs;
    std::vector<SmfTrack> tracks;
    std::string title;          // first track name of a format-1 file, or a text meta event
    int32_t lastTick = 0;
};

namespace smfdetail {
struct Reader
{
    const uint8_t* p; const uint8_t* end;
    bool ok() const { return p < end; }
    uint8_t u8() { return p < end ? *p++ : 0; }
    uint32_t be(int n) { uint32_t v = 0; for (int i = 0; i < n; ++i) v = (v << 8) | u8(); return v; }
    uint32_t vlq()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) { uint8_t b = u8(); v = (v << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        return v;
    }
};
inline std::string cleanText(const uint8_t* s, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n && out.size() < 120; ++i)
    {
        unsigned char c = s[i];
        if (c >= 32 && c != 127) out.push_back((char)c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
} // namespace smfdetail

// Parses a whole file held in memory. Returns false only when nothing usable was found.
inline bool parseSmf(const uint8_t* data, size_t size, Smf& out, std::string& err)
{
    using namespace smfdetail;
    out = Smf();
    if (data == nullptr || size < 14) { err = "file too short"; return false; }
    Reader r{data, data + size};
    // Some files carry a RIFF ("RMID") wrapper; find the MThd chunk.
    const uint8_t* mthd = nullptr;
    for (size_t i = 0; i + 4 <= size && i < 64; ++i)
        if (std::memcmp(data + i, "MThd", 4) == 0) { mthd = data + i; break; }
    if (!mthd) { err = "not a MIDI file"; return false; }
    r.p = mthd + 4;
    uint32_t hlen = r.be(4);
    const uint8_t* hdrEnd = r.p + hlen;
    out.format = (int)r.be(2);
    int ntrk = (int)r.be(2);
    int division = (int)(int16_t)r.be(2);
    if (division > 0) out.ppq = division;
    else
    {
        // SMPTE time: frames per second * ticks per frame ticks each second. Treat 120 bpm.
        int fps = -(division >> 8); int tpf = division & 0xFF;
        out.ppq = std::max(24, (fps * tpf) / 2);
    }
    r.p = std::min(hdrEnd, r.end);

    struct Pending { int32_t tick; uint8_t vel; bool active; };
    for (int t = 0; t < ntrk && r.p + 8 <= r.end; ++t)
    {
        // Skip foreign chunks.
        while (r.p + 8 <= r.end && std::memcmp(r.p, "MTrk", 4) != 0)
        {
            r.p += 4; uint32_t skip = r.be(4);
            if (skip > (uint32_t)(r.end - r.p)) { r.p = r.end; break; }
            r.p += skip;
        }
        if (r.p + 8 > r.end) break;
        r.p += 4;
        uint32_t len = r.be(4);
        const uint8_t* trkEnd = (len > (uint32_t)(r.end - r.p)) ? r.end : r.p + len;
        Reader tr{r.p, trkEnd};
        r.p = trkEnd;

        SmfTrack info;
        Pending pend[16][128];
        std::memset(pend, 0, sizeof(pend));
        int32_t tick = 0; uint8_t status = 0;
        int16_t trackIndex = (int16_t)out.tracks.size();
        while (tr.ok())
        {
            uint32_t dt = tr.vlq();
            tick += (int32_t)std::min<uint32_t>(dt, 0x0FFFFFFF);
            if (!tr.ok()) break;
            uint8_t b = *tr.p;
            if (b & 0x80) { ++tr.p; if (b < 0xF0) status = b; }
            else if (status == 0) { ++tr.p; continue; }    // data byte with no status: skip
            else b = status;                                 // running status

            if (b == 0xFF)
            {
                uint8_t type = tr.u8(); uint32_t mlen = tr.vlq();
                const uint8_t* m = tr.p;
                if (mlen > (uint32_t)(tr.end - tr.p)) mlen = (uint32_t)(tr.end - tr.p);
                tr.p += mlen;
                if (type == 0x2F) break;                                         // end of track
                else if (type == 0x51 && mlen >= 3) out.tempos.push_back({tick, (uint32_t)(m[0] << 16 | m[1] << 8 | m[2])});
                else if (type == 0x58 && mlen >= 2)
                {
                    uint8_t num = m[0]; uint8_t den = (uint8_t)(m[1] < 7 ? (1 << m[1]) : 4);
                    if (num >= 1 && num <= 32) out.timeSigs.push_back({tick, num, den});
                }
                else if (type == 0x59 && mlen >= 2)
                {
                    int8_t sf = (int8_t)m[0]; if (sf < -7 || sf > 7) sf = 0;
                    out.keySigs.push_back({tick, sf, (uint8_t)(m[1] ? 1 : 0)});
                }
                else if (type == 0x03 && info.name.empty()) info.name = cleanText(m, mlen);
                else if ((type == 0x01 || type == 0x02) && out.title.empty() && t == 0 && mlen > 2 && mlen < 80)
                {
                    std::string s = cleanText(m, mlen);
                    // Ignore typical tool banners.
                    if (s.find("Creator") == std::string::npos && s.find("LilyPond") == std::string::npos &&
                        s.find("creator") == std::string::npos && s.find("http") == std::string::npos)
                        out.title = s;
                }
                continue;
            }
            if (b == 0xF0 || b == 0xF7) { uint32_t l = tr.vlq(); tr.p += std::min<uint32_t>(l, (uint32_t)(tr.end - tr.p)); continue; }
            if (b >= 0xF1) { continue; }   // system common/realtime inside a file: ignore

            uint8_t hi = b & 0xF0, ch = b & 0x0F;
            uint8_t d1 = tr.u8() & 0x7F;
            uint8_t d2 = (hi == 0xC0 || hi == 0xD0) ? 0 : (tr.u8() & 0x7F);
            if (hi == 0x90 && d2 > 0)
            {
                Pending& pn = pend[ch][d1];
                if (pn.active && tick > pn.tick)   // re-struck while held: close the previous note
                {
                    SmfNote n; n.on = pn.tick; n.off = tick; n.pitch = d1; n.vel = pn.vel; n.chan = ch; n.track = trackIndex;
                    out.notes.push_back(n);
                }
                pn.active = true; pn.tick = tick; pn.vel = d2;
                info.notes++; info.pitchSum += d1; info.chanMask |= 1 << ch;
            }
            else if (hi == 0x80 || (hi == 0x90 && d2 == 0))
            {
                Pending& pn = pend[ch][d1];
                if (pn.active)
                {
                    SmfNote n; n.on = pn.tick; n.off = std::max(tick, pn.tick + 1); n.pitch = d1; n.vel = pn.vel; n.chan = ch; n.track = trackIndex;
                    out.notes.push_back(n);
                    pn.active = false;
                }
            }
        }
        // Close notes left hanging at the end of the track.
        for (int ch = 0; ch < 16; ++ch)
            for (int k = 0; k < 128; ++k)
                if (pend[ch][k].active)
                {
                    SmfNote n; n.on = pend[ch][k].tick; n.off = std::max(tick, pend[ch][k].tick + out.ppq / 4);
                    n.pitch = (uint8_t)k; n.vel = pend[ch][k].vel; n.chan = (uint8_t)ch; n.track = trackIndex;
                    out.notes.push_back(n);
                }
        out.lastTick = std::max(out.lastTick, tick);
        out.tracks.push_back(info);
        if (out.format == 1 && t == 0 && out.title.empty() && info.notes == 0 && !info.name.empty())
            out.title = info.name;
    }
    std::stable_sort(out.notes.begin(), out.notes.end(), [](const SmfNote& a, const SmfNote& b) {
        return a.on != b.on ? a.on < b.on : a.pitch < b.pitch; });
    auto byTick = [](const auto& a, const auto& b) { return a.tick < b.tick; };
    std::stable_sort(out.tempos.begin(), out.tempos.end(), byTick);
    std::stable_sort(out.timeSigs.begin(), out.timeSigs.end(), byTick);
    std::stable_sort(out.keySigs.begin(), out.keySigs.end(), byTick);
    if (out.notes.empty()) { err = "no notes"; return false; }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Writer (used by the generator to save exercises, and by the tests).
// ---------------------------------------------------------------------------------------------
struct SmfWriteNote { int32_t on, off; uint8_t pitch, vel; };
struct SmfWriteTrack { std::string name; uint8_t channel = 0; std::vector<SmfWriteNote> notes; };

inline void smfPutVlq(std::vector<uint8_t>& o, uint32_t v)
{
    uint8_t buf[5]; int n = 0;
    buf[n++] = v & 0x7F;
    while ((v >>= 7) != 0) buf[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n--) o.push_back(buf[n]);
}

inline std::vector<uint8_t> writeSmf(int ppq, int bpm, int tsNum, int tsDen, int keySf, bool minor,
                                     const std::string& title, const std::vector<SmfWriteTrack>& tracks)
{
    std::vector<uint8_t> o;
    auto put32 = [&o](uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i))); };
    auto put16 = [&o](uint32_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); };
    o.insert(o.end(), {'M', 'T', 'h', 'd'}); put32(6); put16(1); put16((uint32_t)tracks.size() + 1); put16((uint32_t)ppq);

    auto chunk = [&](const std::vector<uint8_t>& body) { o.insert(o.end(), {'M', 'T', 'r', 'k'}); put32((uint32_t)body.size()); o.insert(o.end(), body.begin(), body.end()); };
    {   // conductor track
        std::vector<uint8_t> b;
        auto meta = [&b](uint8_t type, const std::vector<uint8_t>& d) { smfPutVlq(b, 0); b.push_back(0xFF); b.push_back(type); smfPutVlq(b, (uint32_t)d.size()); b.insert(b.end(), d.begin(), d.end()); };
        meta(0x03, std::vector<uint8_t>(title.begin(), title.end()));
        uint32_t us = (uint32_t)(60000000 / std::max(20, bpm));
        meta(0x51, {(uint8_t)(us >> 16), (uint8_t)(us >> 8), (uint8_t)us});
        int dp = 0; while ((1 << dp) < tsDen && dp < 6) ++dp;
        meta(0x58, {(uint8_t)tsNum, (uint8_t)dp, 24, 8});
        meta(0x59, {(uint8_t)(int8_t)keySf, (uint8_t)(minor ? 1 : 0)});
        smfPutVlq(b, 0); b.push_back(0xFF); b.push_back(0x2F); b.push_back(0);
        chunk(b);
    }
    for (const SmfWriteTrack& t : tracks)
    {
        struct Ev { int32_t tick; uint8_t st, d1, d2; };
        std::vector<Ev> evs;
        for (const SmfWriteNote& n : t.notes)
        {
            evs.push_back({n.on, (uint8_t)(0x90 | t.channel), n.pitch, n.vel});
            evs.push_back({n.off, (uint8_t)(0x80 | t.channel), n.pitch, 64});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return (a.st & 0xF0) < (b.st & 0xF0);   // note-offs (0x80) before note-ons (0x90)
        });
        std::vector<uint8_t> b;
        smfPutVlq(b, 0); b.push_back(0xFF); b.push_back(0x03); smfPutVlq(b, (uint32_t)t.name.size()); b.insert(b.end(), t.name.begin(), t.name.end());
        int32_t last = 0;
        for (const Ev& e : evs) { smfPutVlq(b, (uint32_t)(e.tick - last)); last = e.tick; b.push_back(e.st); b.push_back(e.d1); b.push_back(e.d2); }
        smfPutVlq(b, 0); b.push_back(0xFF); b.push_back(0x2F); b.push_back(0);
        chunk(b);
    }
    return o;
}

} // namespace sr
#endif
