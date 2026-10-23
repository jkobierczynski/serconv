#include "testdata.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>

namespace testdata {

using namespace serconv;

std::vector<uint8_t> readAll(const path& file) {
    File f(file, "rb");
    std::vector<uint8_t> d(size_t(f.size()));
    f.read(d.data(), d.size());
    return d;
}

void writeAll(const path& file, const std::vector<uint8_t>& data) {
    File f(file, "wb");
    f.write(data.data(), data.size());
}

std::vector<uint16_t> makeFrame(int width, int height, int channels, int bits, uint32_t seed, bool noisy) {
    Rng rng(seed);
    const uint32_t maxv = (1u << bits) - 1;
    std::vector<uint16_t> v(size_t(width) * size_t(height) * size_t(channels));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                double base = 0.5 + 0.35 * std::sin(0.11 * x + 0.07 * y + 0.9 * c + 0.3 * seed);
                uint32_t val = uint32_t(base * maxv);
                if (noisy) val = (val + rng.below(64) - 32) & maxv;
                // Bayer-like structure: alternate positions differ.
                if ((x ^ y) & 1) val = (val * 3 / 4) & maxv;
                v[(size_t(y) * size_t(width) + size_t(x)) * size_t(channels) + size_t(c)] = uint16_t(val);
            }
        }
    }
    return v;
}

std::vector<uint16_t> makeScene(int width, int height, int bits, double cx, double cy, double radius, double moonX,
                                double moonY, uint32_t seed) {
    Rng rng(seed);
    const double maxv = double((1u << bits) - 1);
    const double bg = maxv * 0.05;
    std::vector<uint16_t> v(size_t(width) * size_t(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double val = bg + double(rng.below(41)) - 20.0;
            double r = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (r < radius) val += maxv * 0.7 * std::pow(1.0 - (r / radius) * (r / radius), 0.3);
            double rm = std::hypot(x + 0.5 - moonX, y + 0.5 - moonY);
            if (rm < 2.5) val += maxv * 0.6;
            if ((x & 1) != (y & 1)) val *= 1.1;  // greens a bit brighter
            v[size_t(y) * size_t(width) + size_t(x)] = uint16_t(std::clamp(val, 0.0, maxv));
        }
    }
    return v;
}

// ---- lossless JPEG encoder ---------------------------------------------------

namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int n = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            acc = (acc << 1) | ((value >> i) & 1);
            if (++n == 8) {
                out.push_back(uint8_t(acc));
                if (uint8_t(acc) == 0xFF) out.push_back(0x00);
                acc = 0;
                n = 0;
            }
        }
    }
    void flush() {
        while (n != 0) put(1, 1);
    }
};

struct Code {
    uint16_t code = 0;
    uint8_t size = 0;
};

// JPEG Annex K.2 (libjpeg's jpeg_gen_optimal_table).
void optimalTable(const std::array<long, 17>& freqIn, uint8_t bits[17], std::vector<uint8_t>& huffval) {
    long freq[257] = {0};
    for (int i = 0; i < 17; ++i) freq[i] = freqIn[size_t(i)];
    freq[256] = 1;
    int codesize[257] = {0};
    int others[257];
    for (int& o : others) o = -1;
    for (;;) {
        int c1 = -1;
        long v = LONG_MAX;
        for (int i = 0; i <= 256; ++i)
            if (freq[i] && freq[i] <= v) {
                v = freq[i];
                c1 = i;
            }
        int c2 = -1;
        v = LONG_MAX;
        for (int i = 0; i <= 256; ++i)
            if (freq[i] && freq[i] <= v && i != c1) {
                v = freq[i];
                c2 = i;
            }
        if (c2 < 0) break;
        freq[c1] += freq[c2];
        freq[c2] = 0;
        codesize[c1]++;
        while (others[c1] >= 0) {
            c1 = others[c1];
            codesize[c1]++;
        }
        others[c1] = c2;
        codesize[c2]++;
        while (others[c2] >= 0) {
            c2 = others[c2];
            codesize[c2]++;
        }
    }
    int b[33] = {0};
    for (int i = 0; i <= 256; ++i)
        if (codesize[i]) b[codesize[i]]++;
    for (int i = 32; i > 16; --i) {
        while (b[i] > 0) {
            int j = i - 2;
            while (b[j] == 0) --j;
            b[i] -= 2;
            b[i - 1]++;
            b[j + 1] += 2;
            b[j]--;
        }
    }
    int i = 16;
    while (b[i] == 0) --i;
    b[i]--;
    bits[0] = 0;
    for (int k = 1; k <= 16; ++k) bits[k] = uint8_t(b[k]);
    huffval.clear();
    for (int len = 1; len <= 32; ++len)
        for (int s = 0; s < 256; ++s)
            if (codesize[s] == len) huffval.push_back(uint8_t(s));
}

void codesFrom(const uint8_t bits[17], const std::vector<uint8_t>& huffval, Code codes[17]) {
    uint32_t code = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < bits[len]; ++i) {
            codes[huffval[k]].code = uint16_t(code);
            codes[huffval[k]].size = uint8_t(len);
            ++code;
            ++k;
        }
        code <<= 1;
    }
}

void put16(std::vector<uint8_t>& o, int v) {
    o.push_back(uint8_t(v >> 8));
    o.push_back(uint8_t(v));
}

}  // namespace

std::vector<uint8_t> encodeLj92(const std::vector<uint16_t>& in, const Lj92Spec& sp) {
    const int W = sp.width, H = sp.height, C = sp.components, P = sp.precision, Pt = sp.pointTransform;
    const size_t rowLen = size_t(W) * size_t(C);
    std::vector<int> t(in.size());
    for (size_t i = 0; i < in.size(); ++i) t[i] = in[i] >> Pt;
    std::vector<uint8_t> cat(in.size());
    std::vector<uint16_t> extra(in.size());
    std::vector<char> restartBefore(size_t(W) * size_t(H), 0);

    const int initial = 1 << (P - Pt - 1);
    int mcusLeft = sp.restartInterval;
    bool firstLine = true, reset = true;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (sp.restartInterval) {
                if (mcusLeft == 0) {
                    mcusLeft = sp.restartInterval;
                    reset = true;
                    firstLine = true;
                    restartBefore[size_t(y) * size_t(W) + size_t(x)] = 1;
                }
                --mcusLeft;
            }
            for (int c = 0; c < C; ++c) {
                const size_t i = size_t(y) * rowLen + size_t(x) * size_t(C) + size_t(c);
                int pred;
                if (reset) pred = initial;
                else if (firstLine) pred = t[i - size_t(C)];
                else if (x == 0) pred = t[i - rowLen];
                else {
                    const int ra = t[i - size_t(C)], rb = t[i - rowLen], rc = t[i - rowLen - size_t(C)];
                    switch (sp.predictor) {
                        case 1: pred = ra; break;
                        case 2: pred = rb; break;
                        case 3: pred = rc; break;
                        case 4: pred = ra + rb - rc; break;
                        case 5: pred = ra + ((rb - rc) >> 1); break;
                        case 6: pred = rb + ((ra - rc) >> 1); break;
                        default: pred = (ra + rb) >> 1; break;
                    }
                }
                int d = (t[i] - pred) & 0xFFFF;
                if (d >= 32768) d -= 65536;
                if (d == -32768) {
                    cat[i] = 16;
                    extra[i] = 0;
                } else {
                    int a = d < 0 ? -d : d, s = 0;
                    while (a) {
                        ++s;
                        a >>= 1;
                    }
                    cat[i] = uint8_t(s);
                    extra[i] = uint16_t(d >= 0 ? d : d + (1 << s) - 1);
                }
            }
            reset = false;
        }
        firstLine = false;
    }

    const int tables = sp.perComponentTables ? C : 1;
    std::vector<std::array<long, 17>> freq(static_cast<size_t>(tables));
    for (auto& f : freq) f.fill(0);
    for (size_t i = 0; i < in.size(); ++i) freq[sp.perComponentTables ? i % size_t(C) : 0][cat[i]]++;
    std::vector<std::array<uint8_t, 17>> bits(static_cast<size_t>(tables));
    std::vector<std::vector<uint8_t>> vals(static_cast<size_t>(tables));
    std::vector<std::array<Code, 17>> codes(static_cast<size_t>(tables));
    for (int k = 0; k < tables; ++k) {
        optimalTable(freq[size_t(k)], bits[size_t(k)].data(), vals[size_t(k)]);
        codesFrom(bits[size_t(k)].data(), vals[size_t(k)], codes[size_t(k)].data());
    }

    std::vector<uint8_t> o = {0xFF, 0xD8};
    for (int k = 0; k < tables; ++k) {
        o.push_back(0xFF);
        o.push_back(0xC4);
        put16(o, 2 + 1 + 16 + int(vals[size_t(k)].size()));
        o.push_back(uint8_t(k));
        for (int l = 1; l <= 16; ++l) o.push_back(bits[size_t(k)][size_t(l)]);
        for (uint8_t v : vals[size_t(k)]) o.push_back(v);
    }
    o.push_back(0xFF);
    o.push_back(0xC3);
    put16(o, 8 + 3 * C);
    o.push_back(uint8_t(P));
    put16(o, H);
    put16(o, W);
    o.push_back(uint8_t(C));
    for (int c = 0; c < C; ++c) {
        o.push_back(uint8_t(c + 1));
        o.push_back(0x11);
        o.push_back(0);
    }
    if (sp.restartInterval) {
        o.push_back(0xFF);
        o.push_back(0xDD);
        put16(o, 4);
        put16(o, sp.restartInterval);
    }
    o.push_back(0xFF);
    o.push_back(0xDA);
    put16(o, 6 + 2 * C);
    o.push_back(uint8_t(C));
    for (int c = 0; c < C; ++c) {
        o.push_back(uint8_t(c + 1));
        o.push_back(uint8_t((sp.perComponentTables ? c : 0) << 4));
    }
    o.push_back(uint8_t(sp.predictor));
    o.push_back(0);
    o.push_back(uint8_t(Pt));

    BitWriter bw(o);
    int restarts = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (restartBefore[size_t(y) * size_t(W) + size_t(x)]) {
                bw.flush();
                o.push_back(0xFF);
                o.push_back(uint8_t(0xD0 + (restarts++ & 7)));
            }
            for (int c = 0; c < C; ++c) {
                const size_t i = size_t(y) * rowLen + size_t(x) * size_t(C) + size_t(c);
                const Code& code = codes[sp.perComponentTables ? size_t(c) : 0][cat[i]];
                bw.put(code.code, code.size);
                if (cat[i] && cat[i] < 16) bw.put(extra[i], cat[i]);
            }
        }
    }
    bw.flush();
    o.push_back(0xFF);
    o.push_back(0xD9);
    return o;
}

// ---- MLV writer -------------------------------------------------------------

std::vector<uint8_t> packMlv(const std::vector<uint16_t>& samples, int bpp) {
    std::vector<uint8_t> out;
    out.reserve(samples.size() * size_t(bpp) / 8 + 4);
    uint32_t word = 0;
    int nbits = 0;
    auto emitWord = [&](uint16_t w) {
        out.push_back(uint8_t(w));
        out.push_back(uint8_t(w >> 8));
    };
    for (uint16_t s : samples) {
        for (int i = bpp - 1; i >= 0; --i) {
            word = (word << 1) | ((s >> i) & 1u);
            if (++nbits == 16) {
                emitWord(uint16_t(word));
                word = 0;
                nbits = 0;
            }
        }
    }
    if (nbits) emitWord(uint16_t(word << (16 - nbits)));
    return out;
}

namespace {

void le16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
void le32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void le64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void fixed(std::vector<uint8_t>& b, const std::string& s, size_t n) {
    for (size_t i = 0; i < n; ++i) b.push_back(i < s.size() ? uint8_t(s[i]) : 0);
}
void blockHeader(std::vector<uint8_t>& b, const char* type, uint32_t size, uint64_t ts) {
    b.insert(b.end(), type, type + 4);
    le32(b, size);
    le64(b, ts);
}

std::vector<uint8_t> mlviHeader(const MlvSpec& sp, uint16_t fileNum, uint32_t framesInFile) {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'M', 'L', 'V', 'I'});
    le32(b, 52);
    fixed(b, "v2.0", 8);
    le64(b, 0x1234567890ABCDEFull);
    le16(b, fileNum);
    le16(b, uint16_t(sp.chunks));
    le32(b, 0);
    le16(b, uint16_t(1 | (sp.lj92 ? 0x20 : 0)));
    le16(b, 0);
    le32(b, framesInFile);
    le32(b, 0);
    le32(b, sp.fpsNom);
    le32(b, sp.fpsDenom);
    return b;
}

}  // namespace

MlvClip writeMlv(const path& mainFile, const MlvSpec& sp) {
    MlvClip clip;
    std::vector<uint32_t> numbers;
    for (uint32_t k = 0; numbers.size() < size_t(sp.frames); ++k) {
        if (std::find(sp.dropFrameNumbers.begin(), sp.dropFrameNumbers.end(), k) == sp.dropFrameNumbers.end()) {
            numbers.push_back(k);
        }
    }
    std::vector<path> files{mainFile};
    for (int c = 1; c < sp.chunks; ++c) {
        path p = mainFile;
        p.replace_extension(strprintf(".M%02d", c - 1));
        files.push_back(p);
    }
    std::vector<std::vector<uint8_t>> body(files.size());
    std::vector<uint32_t> perFile(files.size(), 0);

    // Main file metadata blocks.
    std::vector<uint8_t>& m = body[0];
    blockHeader(m, "RAWI", 16 + 164, 0);
    le16(m, uint16_t(sp.width));
    le16(m, uint16_t(sp.height));
    std::vector<uint8_t> ri;
    le32(ri, 1);                        // api_version
    le32(ri, 0);                        // buffer
    le32(ri, uint32_t(sp.height + 8));  // height of the full buffer
    le32(ri, uint32_t(sp.width + 16));  // width
    le32(ri, uint32_t((sp.width + 16) * sp.bpp / 8));
    le32(ri, 0);
    le32(ri, uint32_t(sp.bpp));
    le32(ri, uint32_t(sp.black));
    le32(ri, uint32_t(sp.white));
    for (int i = 0; i < 4; ++i) le32(ri, 0);  // crop
    for (int i = 0; i < 4; ++i) le32(ri, 0);  // active area
    le32(ri, 0);
    le32(ri, 0);
    le32(ri, sp.cfa);
    le32(ri, 21);
    for (int i = 0; i < 18; ++i) le32(ri, i % 2 ? 10000 : 10000);
    le32(ri, 1100);
    m.insert(m.end(), ri.begin(), ri.end());

    blockHeader(m, "IDNT", 16 + 68, 0);
    fixed(m, sp.camera, 32);
    le32(m, 0x80000285);
    fixed(m, "123456789", 32);
    blockHeader(m, "EXPO", 16 + 24, 0);
    le32(m, 0);
    le32(m, sp.iso);
    le32(m, sp.iso);
    le32(m, 0);
    le64(m, sp.shutterUs);
    blockHeader(m, "LENS", 16 + 80, 0);
    le16(m, 400);
    le16(m, 65535);
    le16(m, 560);
    m.push_back(1);
    m.push_back(0);
    le32(m, 0);
    le32(m, 0x1234);
    fixed(m, sp.lens, 32);
    fixed(m, "", 32);
    if (sp.rtci) {
        blockHeader(m, "RTCI", 16 + 28, sp.rtciTimestampUs);
        le16(m, uint16_t(sp.clock.second));
        le16(m, uint16_t(sp.clock.minute));
        le16(m, uint16_t(sp.clock.hour));
        le16(m, uint16_t(sp.clock.day));
        le16(m, uint16_t(sp.clock.month - 1));
        le16(m, uint16_t(sp.clock.year - 1900));
        le16(m, 0);
        le16(m, 0);
        le16(m, 0);
        le16(m, 0);
        fixed(m, "", 8);
    }

    for (size_t k = 0; k < numbers.size(); ++k) {
        std::vector<uint16_t> frame =
            sp.sceneRadius > 0
                ? makeScene(sp.width, sp.height, sp.bpp, sp.sceneX + 2.0 * numbers[k], sp.sceneY + 1.0 * numbers[k],
                            sp.sceneRadius, 5, 5, sp.seed * 1000 + numbers[k])
                : makeFrame(sp.width, sp.height, 1, sp.bpp, sp.seed * 1000 + numbers[k]);
        std::vector<uint8_t> payload;
        if (sp.lj92) {
            Lj92Spec ls;
            ls.components = sp.lj92Components;
            ls.width = sp.width / sp.lj92Components;
            ls.height = sp.height;
            ls.precision = sp.bpp;
            ls.predictor = sp.lj92Components == 1 ? 1 : 6;
            payload = encodeLj92(frame, ls);
        } else {
            payload = packMlv(frame, sp.bpp);
        }
        const bool last = k + 1 == numbers.size();
        const size_t fileIdx = k % files.size();
        std::vector<uint8_t>& b = body[fileIdx];
        const uint64_t ts = sp.firstFrameUs + uint64_t(numbers[k]) * sp.frameStepUs;
        blockHeader(b, "VIDF", uint32_t(32 + sp.frameSpace + payload.size()), ts);
        le32(b, numbers[k]);
        le16(b, 0);
        le16(b, 0);
        le16(b, 0);
        le16(b, 0);
        le32(b, sp.frameSpace);
        b.insert(b.end(), sp.frameSpace, 0xEE);
        if (last && sp.truncateLast) {
            b.insert(b.end(), payload.begin(), payload.begin() + long(payload.size() / 2));
        } else {
            b.insert(b.end(), payload.begin(), payload.end());
            clip.frames.push_back(frame);
            clip.frameNumbers.push_back(numbers[k]);
            clip.timestampsUs.push_back(ts);
        }
        perFile[fileIdx]++;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        std::vector<uint8_t> all = mlviHeader(sp, uint16_t(i), perFile[i]);
        all.insert(all.end(), body[i].begin(), body[i].end());
        writeAll(files[i], all);
    }
    clip.files = files;
    return clip;
}

// ---- DNG writer -------------------------------------------------------------

namespace {

struct TEntry {
    uint16_t tag, type;
    uint32_t count;
    std::vector<uint8_t> data;
};

TEntry shortE(uint16_t tag, std::vector<uint16_t> v) {
    TEntry e{tag, 3, uint32_t(v.size()), {}};
    for (uint16_t x : v) le16(e.data, x);
    return e;
}
TEntry longE(uint16_t tag, std::vector<uint32_t> v) {
    TEntry e{tag, 4, uint32_t(v.size()), {}};
    for (uint32_t x : v) le32(e.data, x);
    return e;
}
TEntry byteE(uint16_t tag, std::vector<uint8_t> v) { return TEntry{tag, 1, uint32_t(v.size()), v}; }
TEntry asciiE(uint16_t tag, const std::string& s) {
    TEntry e{tag, 2, uint32_t(s.size() + 1), {}};
    e.data.assign(s.begin(), s.end());
    e.data.push_back(0);
    return e;
}
TEntry srationalE(uint16_t tag, std::vector<std::pair<int32_t, int32_t>> v) {
    TEntry e{tag, 10, uint32_t(v.size()), {}};
    for (auto& r : v) {
        le32(e.data, uint32_t(r.first));
        le32(e.data, uint32_t(r.second));
    }
    return e;
}

size_t ifdSize(const std::vector<TEntry>& es) {
    size_t s = 2 + es.size() * 12 + 4;
    for (const auto& e : es)
        if (e.data.size() > 4) s += (e.data.size() + 1) & ~size_t(1);
    return s;
}

void writeIfd(std::vector<uint8_t>& out, std::vector<TEntry> es, uint32_t at) {
    std::sort(es.begin(), es.end(), [](const TEntry& a, const TEntry& b) { return a.tag < b.tag; });
    out.resize(at);
    le16(out, uint16_t(es.size()));
    uint32_t dataAt = at + 2 + uint32_t(es.size()) * 12 + 4;
    std::vector<uint8_t> blobs;
    for (const auto& e : es) {
        le16(out, e.tag);
        le16(out, e.type);
        le32(out, e.count);
        if (e.data.size() <= 4) {
            std::vector<uint8_t> v = e.data;
            v.resize(4, 0);
            out.insert(out.end(), v.begin(), v.end());
        } else {
            le32(out, dataAt + uint32_t(blobs.size()));
            blobs.insert(blobs.end(), e.data.begin(), e.data.end());
            if (blobs.size() & 1) blobs.push_back(0);
        }
    }
    le32(out, 0);
    out.insert(out.end(), blobs.begin(), blobs.end());
}

}  // namespace

void writeDng(const path& file, const DngSpec& sp, const std::vector<uint16_t>& data) {
    std::vector<uint8_t> image;
    if (sp.lj92) {
        Lj92Spec ls;
        ls.components = sp.mono ? 1 : sp.lj92Components;
        ls.width = sp.width / ls.components;
        ls.height = sp.height;
        ls.precision = 16;
        ls.predictor = 1;
        image = encodeLj92(data, ls);
    } else {
        for (uint16_t v : data) le16(image, v);
    }

    std::vector<TEntry> exif;
    if (!sp.dateTimeOriginal.empty()) exif.push_back(asciiE(36867, sp.dateTimeOriginal));
    if (!sp.subSec.empty()) exif.push_back(asciiE(37521, sp.subSec));
    if (!sp.offsetTime.empty()) exif.push_back(asciiE(36881, sp.offsetTime));
    if (!sp.dateTimeDigitized.empty()) exif.push_back(asciiE(36868, sp.dateTimeDigitized));

    auto ifd0 = [&](uint32_t stripOffset, uint32_t exifOffset) {
        std::vector<TEntry> e;
        e.push_back(longE(254, {0}));
        e.push_back(longE(256, {uint32_t(sp.width)}));
        e.push_back(longE(257, {uint32_t(sp.height)}));
        e.push_back(shortE(258, {16}));
        e.push_back(shortE(259, {uint16_t(sp.lj92 ? 7 : 1)}));
        e.push_back(shortE(262, {uint16_t(sp.mono ? 34892 : 32803)}));
        e.push_back(asciiE(271, sp.make));
        e.push_back(asciiE(272, sp.model));
        e.push_back(longE(273, {stripOffset}));
        if (!sp.dateTime.empty()) e.push_back(asciiE(306, sp.dateTime));
        e.push_back(shortE(277, {1}));
        e.push_back(longE(278, {uint32_t(sp.height)}));
        e.push_back(longE(279, {uint32_t(image.size())}));
        e.push_back(shortE(284, {1}));
        if (!sp.mono) {
            e.push_back(shortE(33421, {2, 2}));
            std::vector<uint8_t> pat;
            for (char c : std::string(cfaName(sp.cfa))) pat.push_back(c == 'R' ? 0 : c == 'G' ? 1 : 2);
            e.push_back(byteE(33422, pat));
            e.push_back(srationalE(50721, {{10000, 10000}, {0, 10000}, {0, 10000}, {0, 10000}, {10000, 10000},
                                           {0, 10000}, {0, 10000}, {0, 10000}, {10000, 10000}}));
            e.push_back(shortE(50778, {21}));
        }
        if (!exif.empty()) e.push_back(longE(34665, {exifOffset}));
        e.push_back(byteE(50706, {1, 4, 0, 0}));
        e.push_back(byteE(50707, {1, 1, 0, 0}));
        e.push_back(asciiE(50708, sp.make + " " + sp.model));
        e.push_back(longE(50714, {uint32_t(sp.black)}));
        e.push_back(longE(50717, {uint32_t(sp.white)}));
        if (sp.frameRate > 0) e.push_back(srationalE(51044, {{int32_t(std::lround(sp.frameRate * 1000)), 1000}}));
        return e;
    };

    const uint32_t ifd0At = 8;
    const size_t ifd0Bytes = ifdSize(ifd0(0, 0));
    const uint32_t exifAt = uint32_t(ifd0At + ifd0Bytes);
    const size_t exifBytes = exif.empty() ? 0 : ifdSize(exif);
    const uint32_t stripAt = uint32_t((exifAt + exifBytes + 15) & ~size_t(15));

    std::vector<uint8_t> out = {'I', 'I', 42, 0};
    le32(out, ifd0At);
    writeIfd(out, ifd0(stripAt, exifAt), ifd0At);
    if (!exif.empty()) writeIfd(out, exif, exifAt);
    out.resize(stripAt, 0);
    out.insert(out.end(), image.begin(), image.end());
    writeAll(file, out);
}

// ---- SER reading (independent of serconv's reader) ------------------------------

SerContents readSerFile(const path& file) {
    std::vector<uint8_t> d = readAll(file);
    SerContents s;
    if (d.size() < 178 || std::memcmp(d.data(), "LUCAM-RECORDER", 14) != 0) return s;
    auto i32 = [&](size_t o) { return int32_t(rd32le(&d[o])); };
    s.colorId = i32(18);
    s.littleEndian = i32(22);
    s.width = i32(26);
    s.height = i32(30);
    s.depth = i32(34);
    s.frameCount = i32(38);
    s.observer = fixedString(&d[42], 40);
    s.instrument = fixedString(&d[82], 40);
    s.telescope = fixedString(&d[122], 40);
    s.dateTime = int64_t(rd64le(&d[162]));
    s.dateTimeUtc = int64_t(rd64le(&d[170]));
    const size_t planes = s.colorId >= 100 ? 3 : 1;
    const size_t bps = s.depth > 8 ? 2 : 1;
    const size_t fb = size_t(s.width) * size_t(s.height) * planes * bps;
    size_t pos = 178;
    for (int f = 0; f < s.frameCount && pos + fb <= d.size(); ++f) {
        std::vector<uint16_t> fr(fb / bps);
        for (size_t i = 0; i < fr.size(); ++i) {
            if (bps == 1) fr[i] = d[pos + i];
            else fr[i] = s.littleEndian == 0 ? rd16le(&d[pos + i * 2]) : rd16be(&d[pos + i * 2]);
        }
        s.frames.push_back(std::move(fr));
        pos += fb;
    }
    if (d.size() - pos == size_t(s.frameCount) * 8) {
        for (int f = 0; f < s.frameCount; ++f) s.timestamps.push_back(int64_t(rd64le(&d[pos + size_t(f) * 8])));
    }
    return s;
}

}  // namespace testdata
