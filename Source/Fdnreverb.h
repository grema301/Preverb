#pragma once
#include <array>
#include <vector>

// =============================================================
//  FDNReverb — 8-line stereo Feedback Delay Network
//
//  This engine is only ever used OFFLINE-style (by SwellRenderer),
//  so it can afford to be a bit richer than a live reverb:
//
//    stereo in ─► 4× allpass diffusers (per channel, strength = Diffusion)
//              ─► mid/side injection into 8 delay lines
//    8 lines   ─► read (with tiny LFO modulation, linear interpolation)
//              ─► 8×8 Hadamard mixing matrix (orthogonal, fully mixing)
//              ─► per-line one-pole lowpass (Damping)
//              ─► per-line RT60 gain  g = 10^(-3 L / (sr * T60))
//              ─► back into the lines
//    stereo out = two different ±1 tap patterns over the line outputs
//
//  Differences vs. the first version (and why):
//   * Hadamard instead of Householder. With N = 8 a Householder matrix
//     leaves 75 % of every line feeding straight back into itself, so the
//     network "rings" like 8 separate combs. Hadamard spreads every line
//     equally into all others → much smoother, denser tail.
//   * Line lengths are rounded to the next PRIME after scaling, so they stay
//     mutually coprime at any sample rate / size setting.
//   * Real stereo (stereo in, decorrelated stereo out) instead of two
//     near-identical mono engines.
//   * Diffusion is now a real parameter (it was a no-op placeholder).
//   * No dependency on JUCE: plain C++17, trivially unit-testable.
// =============================================================
class FDNReverb
{
public:
    static constexpr int NUM_LINES = 8;

    struct Params
    {
        float size      = 0.5f;   // 0..1  -> delay lengths x0.5 .. x2.0 (exponential)
        float decaySec  = 1.0f;   // RT60 in seconds
        float damping   = 0.5f;   // 0 = bright .. 1 = dark
        float diffusion = 0.5f;   // 0 = no input diffusion .. 1 = heavy
    };

    // Allocates everything for the worst case. NOT real-time safe.
    void prepare (double sampleRate);

    // No allocation; cheap enough for the audio thread.
    void setParams (const Params& p);
    void reset();

    void process (float inL, float inR, float& outL, float& outR) noexcept;

    // Earliest sample at which an impulse can appear at the output. The renderer
    // subtracts this so the reversed swell lines up exactly with the hit.
    int getMinOutputDelay() const noexcept { return minOutputDelay_; }

private:
    struct Allpass
    {
        std::vector<float> buf;
        int idx = 0;

        void init (int len)            { buf.assign (static_cast<size_t> (len > 1 ? len : 1), 0.f); idx = 0; }
        void clear()                   { std::fill (buf.begin(), buf.end(), 0.f); idx = 0; }

        float process (float x, float g) noexcept
        {
            const float d = buf[static_cast<size_t> (idx)];
            const float v = x + g * d;
            buf[static_cast<size_t> (idx)] = v;
            if (++idx >= static_cast<int> (buf.size())) idx = 0;
            return d - g * v;
        }
    };

    static constexpr int NUM_AP = 4;

    double sampleRate_ = 44100.0;
    double srRatio_    = 1.0;
    float  modDepth_   = 5.f;     // samples

    std::array<std::vector<float>, NUM_LINES> lines_;
    std::array<int,   NUM_LINES> heads_ {};
    std::array<float, NUM_LINES> lenF_  {};     // nominal delay (samples)
    std::array<float, NUM_LINES> gain_  {};     // RT60 gain per line
    std::array<float, NUM_LINES> lp_    {};     // damping filter state
    std::array<float, NUM_LINES> phase_ {};     // LFO phase 0..1
    std::array<float, NUM_LINES> phaseInc_ {};

    std::array<Allpass, NUM_AP> apL_, apR_;
    float apGain_   = 0.f;
    float dampCoef_ = 0.f;
    int   minOutputDelay_ = 0;
};