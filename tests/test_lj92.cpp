// Lossless JPEG decoder against the test encoder over predictors, precisions,
// component counts, restart intervals and point transforms.
#include <cstddef>

#include "lj92.hpp"
#include "test_util.hpp"
#include "testdata.hpp"

using namespace serconv;

static bool roundTrip(int w, int h, int comps, int precision, int predictor, int restart, bool perComp, int pt,
                      bool noisy, uint32_t seed) {
    std::vector<uint16_t> data = testdata::makeFrame(w, h, comps, precision, seed, noisy);
    if (noisy && precision == 16) {
        // Full-range random samples exercise the 16-bit (SSSS = 16) difference.
        testdata::Rng rng(seed);
        for (auto& v : data) v = uint16_t(rng.next());
    }
    if (pt) {
        for (auto& v : data) v = uint16_t((v >> pt) << pt);
    }
    testdata::Lj92Spec sp;
    sp.width = w;
    sp.height = h;
    sp.components = comps;
    sp.precision = precision;
    sp.predictor = predictor;
    sp.restartInterval = restart;
    sp.perComponentTables = perComp;
    sp.pointTransform = pt;
    std::vector<uint8_t> enc = testdata::encodeLj92(data, sp);
    Lj92Image img;
    try {
        decodeLj92(enc.data(), enc.size(), img);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "decode error (%dx%dx%d P%d pred%d rst%d): %s\n", w, h, comps, precision, predictor,
                     restart, e.what());
        return false;
    }
    if (img.width != w || img.height != h || img.components != comps || img.precision != precision ||
        img.predictor != predictor) {
        std::fprintf(stderr, "header mismatch\n");
        return false;
    }
    if (img.samples != data) {
        size_t i = 0;
        while (i < data.size() && img.samples[i] == data[i]) ++i;
        std::fprintf(stderr, "sample mismatch at %zu (%dx%dx%d P%d pred%d rst%d pt%d): %u vs %u\n", i, w, h, comps,
                     precision, predictor, restart, pt, img.samples[i], data[i]);
        return false;
    }
    return true;
}

int main() {
    int cases = 0;
    for (int precision : {8, 10, 12, 14, 16}) {
        for (int predictor = 1; predictor <= 7; ++predictor) {
            for (int comps : {1, 2, 4}) {
                for (bool noisy : {false, true}) {
                    const int w = 17 + comps, h = 9;
                    CHECK(roundTrip(w, h, comps, precision, predictor, 0, comps > 1 && noisy, 0, noisy,
                                    uint32_t(precision * 100 + predictor * 10 + comps)));
                    ++cases;
                }
            }
        }
    }
    // Restart intervals: whole rows, partial rows, single pixels.
    for (int restart : {1, 5, 19, 38}) {
        for (int predictor : {1, 4, 6}) {
            CHECK(roundTrip(19, 7, 2, 14, predictor, restart, false, 0, true, uint32_t(restart * 7 + predictor)));
            ++cases;
        }
    }
    // Point transform.
    CHECK(roundTrip(16, 8, 1, 14, 1, 0, false, 2, true, 99));
    CHECK(roundTrip(16, 8, 2, 16, 6, 0, false, 3, true, 98));
    // Degenerate sizes.
    CHECK(roundTrip(1, 1, 1, 12, 1, 0, false, 0, true, 5));
    CHECK(roundTrip(1, 13, 1, 12, 7, 0, false, 0, true, 6));
    CHECK(roundTrip(13, 1, 2, 12, 5, 0, false, 0, true, 7));
    // A larger image (fast Huffman path with many symbols).
    CHECK(roundTrip(400, 120, 2, 14, 1, 0, false, 0, true, 8));
    cases += 7;

    // Corrupt input is reported, not crashed on.
    {
        std::vector<uint16_t> data = testdata::makeFrame(32, 8, 1, 12, 3, true);
        testdata::Lj92Spec sp;
        sp.width = 32;
        sp.height = 8;
        sp.precision = 12;
        std::vector<uint8_t> enc = testdata::encodeLj92(data, sp);
        Lj92Image img;
        std::vector<uint8_t> noSoi(enc.begin() + 2, enc.end());
        CHECK_THROWS(decodeLj92(noSoi.data(), noSoi.size(), img));
        std::vector<uint8_t> cut(enc.begin(), enc.begin() + 20);
        CHECK_THROWS(decodeLj92(cut.data(), cut.size(), img));
        // A scan header cut to nothing at the very end of the data.
        size_t sos = 0;
        while (sos + 1 < enc.size() && !(enc[sos] == 0xFF && enc[sos + 1] == 0xDA)) ++sos;
        CHECK(sos + 1 < enc.size());
        std::vector<uint8_t> emptySos(enc.begin(), enc.begin() + std::ptrdiff_t(sos));
        emptySos.insert(emptySos.end(), {0xFF, 0xDA, 0x00, 0x02});
        const std::vector<uint8_t> exact(emptySos.begin(), emptySos.end());  // no spare capacity (ASan)
        CHECK_THROWS(decodeLj92(exact.data(), exact.size(), img));
        // Random garbage after a valid header must not crash.
        std::vector<uint8_t> garbage(enc);
        testdata::Rng rng(11);
        for (size_t i = enc.size() / 2; i < garbage.size(); ++i) garbage[i] = uint8_t(rng.next());
        try {
            decodeLj92(garbage.data(), garbage.size(), img);
        } catch (const std::exception&) {
        }
    }

    std::printf("%d round trips\n", cases);
    return testutil::finish("test_lj92");
}
