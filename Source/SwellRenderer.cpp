#include "SwellRenderer.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kPi = 3.14159265358979f;
}

// =============================================================
void SwellRenderer::prepare (double sampleRate, int capacitySamples)
{
    fdn_.prepare (sampleRate);
    preparedRate_ = sampleRate;

    workL_.assign (static_cast<size_t> (std::max (capacitySamples, 16)), 0.f);
    workR_.assign (static_cast<size_t> (std::max (capacitySamples, 16)), 0.f);

    state_ = State::Idle;
    pos_ = total_ = align_ = 0;
    sourcePeak_ = 0.f;
}

// =============================================================
bool SwellRenderer::startCommon (const SwellSettings& s)
{
    if (s.windowSamples < 1 || s.swellSamples < 1 || s.gapSamples < 0)
        return false;

    if (static_cast<size_t> (s.windowSamples) + static_cast<size_t> (s.swellSamples)
          + static_cast<size_t> (s.gapSamples) > workL_.size())
        return false;

    if (std::fabs (s.sampleRate - preparedRate_) > 0.5)
        return false;

    settings_ = s;

    FDNReverb::Params p;
    p.size      = s.size;
    p.damping   = s.damping;
    p.diffusion = s.diffusion;
    p.decaySec  = static_cast<float> (s.swellSamples / s.sampleRate);   // RT60 == swell length

    fdn_.setParams (p);
    fdn_.reset();

    align_ = fdn_.getMinOutputDelay();
    total_ = align_ + s.windowSamples + s.swellSamples;
    pos_   = 0;
    return true;
}

// =============================================================
bool SwellRenderer::begin (const SwellSettings& s, const float* srcL, const float* srcR, int srcLength)
{
    state_ = State::Idle;
    if (srcL == nullptr || srcR == nullptr || srcLength < s.windowSamples) return false;
    if (! startCommon (s)) return false;

    std::copy (srcL, srcL + s.windowSamples, workL_.begin());
    std::copy (srcR, srcR + s.windowSamples, workR_.begin());

    sourcePeak_ = 0.f;
    for (int i = 0; i < s.windowSamples; ++i)
        sourcePeak_ = std::max (sourcePeak_, std::max (std::fabs (workL_[static_cast<size_t> (i)]),
                                                       std::fabs (workR_[static_cast<size_t> (i)])));

    state_ = State::Running;
    return true;
}

bool SwellRenderer::beginFromRing (const SwellSettings& s, const float* ringL, const float* ringR,
                                   int ringSize, int startIndex)
{
    state_ = State::Idle;
    if (ringL == nullptr || ringR == nullptr || ringSize < s.windowSamples || startIndex < 0 || startIndex >= ringSize)
        return false;
    if (! startCommon (s)) return false;

    sourcePeak_ = 0.f;
    int idx = startIndex;
    for (int i = 0; i < s.windowSamples; ++i)
    {
        const float l = ringL[idx], r = ringR[idx];
        workL_[static_cast<size_t> (i)] = l;
        workR_[static_cast<size_t> (i)] = r;
        sourcePeak_ = std::max (sourcePeak_, std::max (std::fabs (l), std::fabs (r)));
        if (++idx >= ringSize) idx = 0;
    }

    state_ = State::Running;
    return true;
}

// =============================================================
bool SwellRenderer::step (int maxFdnSamples) noexcept
{
    if (state_ == State::Finished) return true;
    if (state_ != State::Running)  return false;

    const int W = settings_.windowSamples;
    float* L = workL_.data();
    float* R = workR_.data();

    const int end = std::min (total_, pos_ + std::max (1, maxFdnSamples));
    int n = pos_;

    for (; n < end; ++n)
    {
        // Feed the REVERSED window, then silence (the reverb tail).
        float inL = 0.f, inR = 0.f;
        if (n < W)
        {
            inL = L[W - 1 - n];
            inR = R[W - 1 - n];
        }

        float oL, oR;
        fdn_.process (inL, inR, oL, oR);

        // Output index with the reverb's minimum delay removed, so a reversed
        // sample's response starts exactly at its own position.
        const int idx = n - align_;

        // Keep only the tail (idx >= W). It lands at work[W ..< W+S), i.e. right
        // after the window, never overwriting the part of the window we still read.
        if (idx >= W)
        {
            L[idx] = oL;
            R[idx] = oR;
        }
    }

    pos_ = n;

    if (pos_ >= total_)
    {
        finalise();
        state_ = State::Finished;
        return true;
    }

    return false;
}

// =============================================================
void SwellRenderer::finalise() noexcept
{
    const int W = settings_.windowSamples;
    const int S = settings_.swellSamples;
    const int G = settings_.gapSamples;

    float* L = workL_.data() + W;
    float* R = workR_.data() + W;

    // Step 4 of the pipeline: reverse the tail so it builds UP into the hit.
    std::reverse (L, L + S);
    std::reverse (R, R + S);

    // Gap: silence between the end of the swell and the hit.
    std::fill (L + S, L + S + G, 0.f);
    std::fill (R + S, R + S + G, 0.f);

    // Level: the swell's loudest point matches the loudest point of the source,
    // so the effect behaves the same on a quiet vocal and a hot snare.
    float swellPeak = 0.f;
    for (int i = 0; i < S; ++i)
        swellPeak = std::max (swellPeak, std::max (std::fabs (L[i]), std::fabs (R[i])));

    const float gain = swellPeak > 1.0e-12f ? sourcePeak_ / swellPeak : 0.f;
    for (int i = 0; i < S; ++i) { L[i] *= gain; R[i] *= gain; }

    // Fade-in (the swell starts at about −60 dB, but never leave a hard edge)
    const int fadeIn = std::min (S / 2, static_cast<int> (0.010 * settings_.sampleRate));
    for (int i = 0; i < fadeIn; ++i)
    {
        const float w = 0.5f - 0.5f * std::cos (kPi * static_cast<float> (i) / static_cast<float> (fadeIn));
        L[i] *= w;  R[i] *= w;
    }

    // Short fade-out right at the hit: avoids a step discontinuity where the swell stops.
    const int fadeOut = std::min (S / 4, static_cast<int> (0.003 * settings_.sampleRate));
    for (int i = 0; i < fadeOut; ++i)
    {
        const int j = S - fadeOut + i;
        const float w = 0.5f + 0.5f * std::cos (kPi * static_cast<float> (i + 1) / static_cast<float> (fadeOut + 1));
        L[j] *= w;  R[j] *= w;
    }
}