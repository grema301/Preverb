#include "FDNReverb.h"
#include <algorithm>
#include <climits>
#include <cmath>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Base delay lengths at 44.1 kHz (≈ 32–68 ms). They are re-rounded to the next
    // prime after every size / sample-rate scaling so they never share factors.
    constexpr std::array<int, 8> kBaseLen { 1423, 1607, 1861, 2063, 2293, 2531, 2749, 2999 };

    constexpr std::array<int, 4> kApLenL { 113, 181, 263, 397 };
    constexpr std::array<int, 4> kApLenR { 127, 193, 277, 419 };

    // Slow, slightly different modulation rate per line. Breaks up metallic resonances.
    constexpr std::array<float, 8> kLfoHz { 0.31f, 0.43f, 0.53f, 0.61f, 0.71f, 0.79f, 0.89f, 0.97f };

    // Output tap patterns (decorrelated left/right) and side-injection pattern.
    constexpr std::array<float, 8> kTapL { 1.f, -1.f,  1.f, -1.f, 1.f, -1.f,  1.f, -1.f };
    constexpr std::array<float, 8> kTapR { 1.f,  1.f, -1.f, -1.f, 1.f,  1.f, -1.f, -1.f };
    constexpr std::array<float, 8> kSide { 1.f, -1.f, -1.f,  1.f, 1.f, -1.f, -1.f,  1.f };

    constexpr float kNorm   = 0.35355339f;   // 1 / sqrt (8): makes Hadamard orthonormal
    constexpr float kInGain = 0.35f;

    bool isPrime (int n)
    {
        if (n < 2)      return false;
        if (n % 2 == 0) return n == 2;
        for (int d = 3; d * d <= n; d += 2)
            if (n % d == 0) return false;
        return true;
    }

    int nextPrime (int n)
    {
        while (! isPrime (n)) ++n;
        return n;
    }
}

// =============================================================
void FDNReverb::prepare (double sampleRate)
{
    sampleRate_ = sampleRate;
    srRatio_    = sampleRate / 44100.0;
    modDepth_   = static_cast<float> (5.0 * srRatio_);

    for (int k = 0; k < NUM_LINES; ++k)
    {
        // Worst case: size = 1  ->  x2.0
        const int maxLen = nextPrime (static_cast<int> (std::ceil (kBaseLen[static_cast<size_t> (k)] * 2.0 * srRatio_)))
                         + static_cast<int> (std::ceil (modDepth_)) + 4;

        lines_[static_cast<size_t> (k)].assign (static_cast<size_t> (maxLen + 2), 0.f);
        phaseInc_[static_cast<size_t> (k)] = static_cast<float> (kLfoHz[static_cast<size_t> (k)] / sampleRate);
    }

    for (int s = 0; s < NUM_AP; ++s)
    {
        apL_[static_cast<size_t> (s)].init (std::max (2, static_cast<int> (std::lround (kApLenL[static_cast<size_t> (s)] * srRatio_))));
        apR_[static_cast<size_t> (s)].init (std::max (2, static_cast<int> (std::lround (kApLenR[static_cast<size_t> (s)] * srRatio_))));
    }

    setParams ({});
    reset();
}

// =============================================================
void FDNReverb::setParams (const Params& p)
{
    const float size    = std::min (1.f, std::max (0.f, p.size));
    const float damping = std::min (1.f, std::max (0.f, p.damping));
    const float decay   = std::max (0.05f, p.decaySec);
    const double mul    = 0.5 * std::pow (4.0, static_cast<double> (size));   // 0.5 .. 2.0

    int minLen = INT_MAX;

    for (size_t k = 0; k < static_cast<size_t> (NUM_LINES); ++k)
    {
        const int len = nextPrime (static_cast<int> (std::lround (kBaseLen[k] * mul * srRatio_)));
        lenF_[k]  = static_cast<float> (len);
        gain_[k]  = static_cast<float> (std::pow (10.0, -3.0 * len / (sampleRate_ * decay)));
        minLen    = std::min (minLen, len);
    }

    minOutputDelay_ = std::max (0, minLen - static_cast<int> (std::ceil (modDepth_)) - 1);

    // Damping: 16 kHz (bright) -> ~480 Hz (dark) one-pole lowpass inside the loop.
    const double fc = std::min (16000.0, 0.45 * sampleRate_) * std::pow (0.03, static_cast<double> (damping));
    dampCoef_ = static_cast<float> (std::exp (-2.0 * kPi * fc / sampleRate_));

    apGain_ = 0.7f * std::min (1.f, std::max (0.f, p.diffusion));
}

// =============================================================
void FDNReverb::reset()
{
    for (size_t k = 0; k < static_cast<size_t> (NUM_LINES); ++k)
    {
        std::fill (lines_[k].begin(), lines_[k].end(), 0.f);
        heads_[k] = 0;
        lp_[k]    = 0.f;
        phase_[k] = static_cast<float> (k) / static_cast<float> (NUM_LINES);   // deterministic
    }

    for (auto& a : apL_) a.clear();
    for (auto& a : apR_) a.clear();
}

// =============================================================
void FDNReverb::process (float inL, float inR, float& outL, float& outR) noexcept
{
    // ── Input diffusion ───────────────────────────────────────
    float aL = inL, aR = inR;
    for (size_t s = 0; s < static_cast<size_t> (NUM_AP); ++s)
    {
        aL = apL_[s].process (aL, apGain_);
        aR = apR_[s].process (aR, apGain_);
    }

    const float mid  = 0.5f * (aL + aR);
    const float side = 0.5f * (aL - aR);

    // ── Read the 8 (modulated) line outputs ───────────────────
    float y[NUM_LINES];

    for (size_t k = 0; k < static_cast<size_t> (NUM_LINES); ++k)
    {
        float& ph = phase_[k];
        ph += phaseInc_[k];
        if (ph >= 1.f) ph -= 1.f;

        const float t   = 4.f * std::fabs (ph - 0.5f) - 1.f;          // triangle −1..1
        const float lfo = t * (1.5f - 0.5f * t * t);                  // smoothed ≈ sine

        const float d    = lenF_[k] + lfo * modDepth_;
        const int   d0   = static_cast<int> (d);
        const float frac = d - static_cast<float> (d0);

        const auto& buf  = lines_[k];
        const int   size = static_cast<int> (buf.size());

        int i0 = heads_[k] - d0;  if (i0 < 0) i0 += size;
        int i1 = i0 - 1;          if (i1 < 0) i1 += size;

        y[k] = buf[static_cast<size_t> (i0)] + frac * (buf[static_cast<size_t> (i1)] - buf[static_cast<size_t> (i0)]);
    }

    // ── Stereo output taps ────────────────────────────────────
    float oL = 0.f, oR = 0.f;
    for (size_t k = 0; k < static_cast<size_t> (NUM_LINES); ++k)
    {
        oL += kTapL[k] * y[k];
        oR += kTapR[k] * y[k];
    }
    outL = oL * kNorm;
    outR = oR * kNorm;

    // ── 8×8 Hadamard mix (fast Walsh–Hadamard, 24 add/sub) ────
    float f[NUM_LINES];
    for (int k = 0; k < NUM_LINES; ++k) f[k] = y[k];

    auto bfly = [&f] (int i, int j) { const float a = f[i], b = f[j]; f[i] = a + b; f[j] = a - b; };
    bfly (0, 1); bfly (2, 3); bfly (4, 5); bfly (6, 7);
    bfly (0, 2); bfly (1, 3); bfly (4, 6); bfly (5, 7);
    bfly (0, 4); bfly (1, 5); bfly (2, 6); bfly (3, 7);

    // ── Damp, decay, inject, write ────────────────────────────
    for (size_t k = 0; k < static_cast<size_t> (NUM_LINES); ++k)
    {
        // The tiny constant keeps the filter state out of denormal range.
        lp_[k] = dampCoef_ * lp_[k] + (1.f - dampCoef_) * (f[k] * kNorm) + 1.0e-20f;

        auto& buf = lines_[k];
        buf[static_cast<size_t> (heads_[k])] = lp_[k] * gain_[k] + (mid + side * kSide[k]) * kInGain;

        if (++heads_[k] >= static_cast<int> (buf.size()))
            heads_[k] = 0;
    }
}