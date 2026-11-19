// inflate.h - a small DEFLATE / gzip decoder (RFC 1951/1952), enough to read MPC project files.
// Written from the RFCs; no dependencies.
//
// SPDX-License-Identifier: MIT
#ifndef SR_INFLATE_H
#define SR_INFLATE_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace sr {

namespace inflatedetail {
struct Bits
{
    const uint8_t* p; const uint8_t* end; uint32_t buf = 0; int cnt = 0; bool bad = false;
    int need(int n)
    {
        while (cnt < n) { if (p >= end) { bad = true; return 0; } buf |= (uint32_t)(*p++) << cnt; cnt += 8; }
        int v = (int)(buf & ((1u << n) - 1)); buf >>= n; cnt -= n; return v;
    }
};
struct Huff { uint16_t count[16]; uint16_t symbol[320]; };
inline bool build(Huff& h, const uint8_t* lengths, int n)
{
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    h.count[0] = 0;
    uint16_t offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; ++i) offs[i + 1] = (uint16_t)(offs[i] + h.count[i]);
    for (int i = 0; i < n; ++i) if (lengths[i]) h.symbol[offs[lengths[i]]++] = (uint16_t)i;
    return true;
}
inline int decode(Bits& b, const Huff& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len)
    {
        code |= b.need(1);
        if (b.bad) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
} // namespace inflatedetail

// Inflates raw DEFLATE data. Stops after maxOut bytes. Returns false on corrupt input.
inline bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxOut)
{
    using namespace inflatedetail;
    static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    Bits b{data, data + size};
    int last = 0;
    while (!last)
    {
        last = b.need(1);
        int type = b.need(2);
        if (b.bad) return false;
        if (type == 0)
        {
            b.buf = 0; b.cnt = 0;
            if (b.end - b.p < 4) return false;
            unsigned len = b.p[0] | (b.p[1] << 8); b.p += 4;
            if ((size_t)(b.end - b.p) < len) return false;
            out.insert(out.end(), b.p, b.p + len); b.p += len;
        }
        else if (type == 1 || type == 2)
        {
            Huff lit, dist;
            uint8_t lengths[320];
            if (type == 1)
            {
                int i = 0;
                for (; i < 144; ++i) lengths[i] = 8;
                for (; i < 256; ++i) lengths[i] = 9;
                for (; i < 280; ++i) lengths[i] = 7;
                for (; i < 288; ++i) lengths[i] = 8;
                build(lit, lengths, 288);
                for (i = 0; i < 30; ++i) lengths[i] = 5;
                build(dist, lengths, 30);
            }
            else
            {
                int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
                static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                uint8_t cl[19] = {0};
                for (int i = 0; i < ncode; ++i) cl[order[i]] = (uint8_t)b.need(3);
                Huff ch; build(ch, cl, 19);
                int idx = 0;
                while (idx < nlen + ndist)
                {
                    int sym = decode(b, ch);
                    if (sym < 0) return false;
                    if (sym < 16) lengths[idx++] = (uint8_t)sym;
                    else
                    {
                        int rep = 0; uint8_t val = 0;
                        if (sym == 16) { if (idx == 0) return false; val = lengths[idx - 1]; rep = 3 + b.need(2); }
                        else if (sym == 17) rep = 3 + b.need(3);
                        else rep = 11 + b.need(7);
                        if (idx + rep > nlen + ndist) return false;
                        while (rep--) lengths[idx++] = val;
                    }
                }
                build(lit, lengths, nlen);
                build(dist, lengths + nlen, ndist);
            }
            for (;;)
            {
                int sym = decode(b, lit);
                if (sym < 0) return false;
                if (sym < 256) { out.push_back((uint8_t)sym); }
                else if (sym == 256) break;
                else
                {
                    sym -= 257; if (sym >= 29) return false;
                    int len = lbase[sym] + b.need(lext[sym]);
                    int ds = decode(b, dist); if (ds < 0 || ds >= 30) return false;
                    size_t d = (size_t)(dbase[ds] + b.need(dext[ds]));
                    if (d > out.size()) return false;
                    size_t from = out.size() - d;
                    for (int k = 0; k < len; ++k) out.push_back(out[from + (size_t)k]);
                }
                if (out.size() >= maxOut) return true;
            }
        }
        else return false;
        if (out.size() >= maxOut) return true;
    }
    return true;
}

// Inflates a gzip member (or passes plain data through when it is not gzip).
inline bool gunzip(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t maxOut = 64u << 20)
{
    out.clear();
    if (size < 18 || data[0] != 0x1F || data[1] != 0x8B) { out.assign(data, data + size); return true; }
    if (data[2] != 8) return false;
    uint8_t flg = data[3];
    size_t pos = 10;
    if (flg & 4) { if (pos + 2 > size) return false; pos += 2 + (size_t)(data[pos] | (data[pos + 1] << 8)); }
    if (flg & 8) { while (pos < size && data[pos]) ++pos; ++pos; }
    if (flg & 16) { while (pos < size && data[pos]) ++pos; ++pos; }
    if (flg & 2) pos += 2;
    if (pos >= size) return false;
    out.reserve(std::min<size_t>(maxOut, size * 8));
    return inflateRaw(data + pos, size - pos, out, maxOut);
}

} // namespace sr
#endif
