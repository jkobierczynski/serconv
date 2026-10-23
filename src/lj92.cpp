#include "lj92.hpp"

#include <climits>
#include <cstring>

#include "util.hpp"

namespace serconv {

namespace {

constexpr int kFastBits = 9;

struct HuffTable {
    bool defined = false;
    uint8_t counts[17] = {};  // codes per length 1..16
    uint8_t values[256] = {};
    uint16_t fast[1 << kFastBits] = {};  // (length << 8) | value, 0 = longer code
    int32_t maxcode[18] = {};
    int32_t mincode[17] = {};
    int32_t valptr[17] = {};

    void build() {
        uint16_t codes[256];
        uint8_t sizes[256];
        int k = 0;
        uint32_t code = 0;
        for (int len = 1; len <= 16; ++len) {
            for (int i = 0; i < counts[len]; ++i) {
                if (k >= 256) fail("LJ92: Huffman table has too many codes");
                sizes[k] = uint8_t(len);
                codes[k] = uint16_t(code);
                ++code;
                ++k;
            }
            if (code > (1u << len)) fail("LJ92: invalid Huffman table");
            code <<= 1;
        }
        std::memset(fast, 0, sizeof fast);
        int p = 0;
        for (int len = 1; len <= 16; ++len) {
            if (counts[len]) {
                valptr[len] = p;
                mincode[len] = codes[p];
                p += counts[len];
                maxcode[len] = codes[p - 1];
            } else {
                maxcode[len] = -1;
            }
        }
        maxcode[17] = INT32_MAX;
        for (int i = 0; i < k; ++i) {
            if (sizes[i] <= kFastBits) {
                int shift = kFastBits - sizes[i];
                int base = codes[i] << shift;
                for (int fill = 0; fill < (1 << shift); ++fill) {
                    fast[base | fill] = uint16_t((sizes[i] << 8) | values[i]);
                }
            }
        }
        defined = true;
    }
};

// MSB-first bit reader over JPEG entropy-coded data (handles 0xFF00 stuffing,
// stops at markers and feeds zero bits after one).
class BitReader {
public:
    BitReader(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}

    void fill() {
        while (nbits_ <= 56) {
            uint32_t b = 0;
            if (!marker_ && p_ < end_) {
                b = *p_;
                if (b == 0xFF) {
                    uint8_t next = (p_ + 1 < end_) ? p_[1] : 0xD9;
                    if (next == 0x00) {
                        p_ += 2;
                    } else {
                        marker_ = true;
                        b = 0;
                    }
                } else {
                    ++p_;
                }
            } else if (p_ >= end_) {
                exhausted_ = true;
            }
            acc_ |= uint64_t(b) << (56 - nbits_);
            nbits_ += 8;
        }
    }

    uint32_t peek(int n) {
        if (nbits_ < n) fill();
        return uint32_t(acc_ >> (64 - n));
    }
    void skip(int n) {
        acc_ <<= n;
        nbits_ -= n;
    }
    uint32_t get(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        skip(n);
        return v;
    }

    int decode(const HuffTable& h) {
        if (nbits_ < 32) fill();
        uint32_t look = uint32_t(acc_ >> (64 - kFastBits));
        uint16_t e = h.fast[look];
        if (e) {
            skip(e >> 8);
            return e & 0xFF;
        }
        uint32_t bits16 = uint32_t(acc_ >> 48);
        for (int len = kFastBits + 1; len <= 16; ++len) {
            int32_t code = int32_t(bits16 >> (16 - len));
            if (code <= h.maxcode[len]) {
                skip(len);
                return h.values[h.valptr[len] + code - h.mincode[len]];
            }
        }
        fail("LJ92: corrupt data (invalid Huffman code)");
    }

    // Processes a restart marker: drops buffered bits and skips RSTn.
    void restart() {
        acc_ = 0;
        nbits_ = 0;
        marker_ = false;
        // The marker should be next; tolerate fill bytes and a little junk.
        const uint8_t* q = p_;
        while (q + 1 < end_) {
            if (q[0] == 0xFF && q[1] >= 0xD0 && q[1] <= 0xD7) {
                p_ = q + 2;
                return;
            }
            if (q[0] == 0xFF && q[1] == 0xFF) {
                ++q;
                continue;
            }
            if (q - p_ > 16) break;
            ++q;
        }
        fail("LJ92: corrupt data (restart marker missing)");
    }

    bool exhausted() const { return exhausted_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t acc_ = 0;
    int nbits_ = 0;
    bool marker_ = false;
    bool exhausted_ = false;
};

inline int decodeDiff(BitReader& br, const HuffTable& h) {
    int s = br.decode(h);
    if (s == 0) return 0;
    if (s == 16) return 32768;
    if (s > 16) fail("LJ92: corrupt data (difference size > 16)");
    int v = int(br.get(s));
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
}

}  // namespace

void decodeLj92(const uint8_t* data, size_t size, Lj92Image& out) {
    HuffTable tables[4];
    int precision = 0, width = 0, height = 0, nf = 0;
    int compIds[4] = {0, 0, 0, 0};
    int restartInterval = 0;
    bool haveFrame = false;
    size_t pos = 0;

    auto need = [&](size_t n) {
        if (pos + n > size) fail("LJ92: truncated data");
    };

    need(2);
    if (data[0] != 0xFF || data[1] != 0xD8) fail("LJ92: not a JPEG stream (no SOI marker)");
    pos = 2;

    for (;;) {
        // Find the next marker.
        while (pos < size && data[pos] != 0xFF) ++pos;
        while (pos < size && data[pos] == 0xFF) ++pos;
        if (pos >= size) fail("LJ92: no image data found");
        const int marker = data[pos++];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
        if (marker == 0xD9) fail("LJ92: no scan before end of image");
        need(2);
        const size_t len = rd16be(data + pos);
        if (len < 2) fail("LJ92: corrupt marker segment");
        need(len);
        const uint8_t* seg = data + pos + 2;
        const size_t segLen = len - 2;

        if (marker == 0xC4) {  // DHT
            size_t i = 0;
            while (i < segLen) {
                int tc = seg[i] >> 4, th = seg[i] & 15;
                if (th > 3) fail("LJ92: Huffman table id out of range");
                (void)tc;
                if (i + 17 > segLen) fail("LJ92: truncated Huffman table");
                HuffTable& t = tables[th];
                int total = 0;
                for (int l = 1; l <= 16; ++l) {
                    t.counts[l] = seg[i + size_t(l)];
                    total += t.counts[l];
                }
                if (total > 256 || i + 17 + size_t(total) > segLen) fail("LJ92: invalid Huffman table");
                std::memcpy(t.values, seg + i + 17, size_t(total));
                t.build();
                i += 17 + size_t(total);
            }
        } else if (marker == 0xC3) {  // SOF3, lossless
            if (segLen < 6) fail("LJ92: truncated frame header");
            precision = seg[0];
            height = rd16be(seg + 1);
            width = rd16be(seg + 3);
            nf = seg[5];
            if (precision < 2 || precision > 16) fail(strprintf("LJ92: unsupported precision %d", precision));
            if (nf < 1 || nf > 4) fail(strprintf("LJ92: unsupported component count %d", nf));
            if (width == 0 || height == 0) fail("LJ92: image size missing (DNL is not supported)");
            if (segLen < 6 + size_t(nf) * 3) fail("LJ92: truncated frame header");
            for (int c = 0; c < nf; ++c) {
                compIds[c] = seg[6 + c * 3];
                int hv = seg[7 + c * 3];
                if (hv != 0x11) fail("LJ92: subsampled components are not supported");
            }
            haveFrame = true;
        } else if (marker == 0xDD) {  // DRI
            if (segLen < 2) fail("LJ92: truncated restart interval");
            restartInterval = rd16be(seg);
        } else if (marker == 0xDA) {  // SOS
            if (!haveFrame) fail("LJ92: scan before frame header");
            if (segLen < 1) fail("LJ92: truncated scan header");
            int ns = seg[0];
            if (ns != nf) fail("LJ92: non-interleaved scans are not supported");
            if (segLen < 1 + size_t(ns) * 2 + 3) fail("LJ92: truncated scan header");
            const HuffTable* compTables[4] = {nullptr, nullptr, nullptr, nullptr};
            for (int s = 0; s < ns; ++s) {
                int cs = seg[1 + s * 2];
                int td = seg[2 + s * 2] >> 4;
                if (td > 3 || !tables[td].defined) fail("LJ92: scan uses an undefined Huffman table");
                int idx = -1;
                for (int c = 0; c < nf; ++c)
                    if (compIds[c] == cs) idx = c;
                if (idx < 0) fail("LJ92: scan component not in frame");
                compTables[idx] = &tables[td];
            }
            for (int c = 0; c < nf; ++c)
                if (!compTables[c]) fail("LJ92: scan does not cover all components");
            const int predictor = seg[1 + ns * 2];
            const int pointTransform = seg[3 + ns * 2] & 15;
            if (predictor < 1 || predictor > 7) fail(strprintf("LJ92: unsupported predictor %d", predictor));
            if (pointTransform >= precision) fail("LJ92: invalid point transform");

            pos += len;
            out.width = width;
            out.height = height;
            out.components = nf;
            out.precision = precision;
            out.predictor = predictor;
            const size_t rowLen = size_t(width) * size_t(nf);
            out.samples.assign(rowLen * size_t(height), 0);

            BitReader br(data + pos, data + size);
            const int initial = 1 << (precision - pointTransform - 1);
            int mcusLeft = restartInterval;
            bool firstLine = true;
            bool resetPrediction = true;

            for (int y = 0; y < height; ++y) {
                uint16_t* row = out.samples.data() + rowLen * size_t(y);
                const uint16_t* above = y > 0 ? row - rowLen : nullptr;
                for (int x = 0; x < width; ++x) {
                    if (restartInterval) {
                        if (mcusLeft == 0) {
                            br.restart();
                            mcusLeft = restartInterval;
                            resetPrediction = true;
                            firstLine = true;
                        }
                        --mcusLeft;
                    }
                    const size_t base = size_t(x) * size_t(nf);
                    for (int c = 0; c < nf; ++c) {
                        int pred;
                        if (resetPrediction) {
                            pred = initial;
                        } else if (firstLine) {
                            pred = row[base - size_t(nf) + size_t(c)];
                        } else if (x == 0) {
                            pred = above[c];
                        } else {
                            const int ra = row[base - size_t(nf) + size_t(c)];
                            const int rb = above[base + size_t(c)];
                            const int rc = above[base - size_t(nf) + size_t(c)];
                            switch (predictor) {
                                case 1: pred = ra; break;
                                case 2: pred = rb; break;
                                case 3: pred = rc; break;
                                case 4: pred = ra + rb - rc; break;
                                case 5: pred = ra + ((rb - rc) >> 1); break;
                                case 6: pred = rb + ((ra - rc) >> 1); break;
                                default: pred = (ra + rb) >> 1; break;
                            }
                        }
                        row[base + size_t(c)] = uint16_t((pred + decodeDiff(br, *compTables[c])) & 0xFFFF);
                    }
                    resetPrediction = false;
                }
                firstLine = false;
            }
            if (pointTransform) {
                for (uint16_t& v : out.samples) v = uint16_t(v << pointTransform);
            }
            return;
        } else if ((marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            fail(strprintf("LJ92: JPEG process 0x%02X is not lossless", marker));
        }
        pos += len;
    }
}

}  // namespace serconv
