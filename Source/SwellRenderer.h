#pragma once
#include "FDNReverb.h"
#include <vector>

// =============================================================
//  SwellRenderer — the "reverse reverb" itself.
//
//  Given a source window x[0..W) that STARTS at the hit, it produces the
//  swell that leads INTO the hit:
//
//      1. r = reverse (x)                       time-reverse the source
//      2. y = FDN (r), run for W + S samples    reverb it (S = swell length)
//      3. keep only the reverb tail  y[W .. W+S)  (what rings on after r ends)
//      4. reverse that tail                     → it now builds UP to the hit
//
//  Why only the tail? After the double reversal, the part of y that sat
//  *under* the source ends up on top of the dry hit, smearing it. The classic
//  effect is just the run-up, so we keep (and compute) only that.
//
//  The renderer is RESUMABLE: step (n) advances at most n FDN samples. That lets
//  the audio thread spread a render over many blocks (bounded CPU per block,
//  deterministic in both real-time and offline bounce) while the file loader just
//  calls run() on a background thread. Same code, same result in both places.
//
//  Result layout (valid when finished): swellLength() = S + gap samples.
//  The LAST sample of that buffer is the instant just before the hit, so
//  "place the buffer so that its end lands on the hit" is all a scheduler needs.
// =============================================================

struct SwellSettings
{
    double sampleRate    = 44100.0;
    int    windowSamples = 0;      // W: how much of the source feeds the reverb
    int    swellSamples  = 0;      // S: swell length (also the reverb's RT60)
    int    gapSamples    = 0;      // swell ends this many samples BEFORE the hit
    float  size          = 0.5f;
    float  damping       = 0.5f;
    float  diffusion     = 0.5f;
};

class SwellRenderer
{
public:
    enum class State { Idle, Running, Finished };

    // Allocates. capacity must be >= window + swell + gap. NOT real-time safe.
    void prepare (double sampleRate, int capacitySamples);

    // Start a job from contiguous source audio. Real-time safe (no allocation).
    // Returns false if the settings don't fit the capacity.
    bool begin (const SwellSettings& s, const float* srcL, const float* srcR, int srcLength);

    // Start a job straight from a circular buffer (copies the window out of the ring).
    bool beginFromRing (const SwellSettings& s, const float* ringL, const float* ringR,
                        int ringSize, int startIndex);

    // Advance by at most maxFdnSamples. Returns true once finished.
    bool step (int maxFdnSamples) noexcept;

    // Convenience for background threads.
    void run() { while (isRunning()) step (1 << 14); }

    void cancel() noexcept { state_ = State::Idle; }

    State getState()    const noexcept { return state_; }
    bool  isFinished()  const noexcept { return state_ == State::Finished; }
    bool  isRunning()   const noexcept { return state_ == State::Running; }
    int   getTotalSteps() const noexcept { return total_; }
    float getProgress() const noexcept
    {
        return total_ > 0 ? static_cast<float> (pos_) / static_cast<float> (total_) : 0.f;
    }

    const SwellSettings& getSettings() const noexcept { return settings_; }

    // ── Results (valid when finished) ────────────────────────
    const float* swellL()    const noexcept { return workL_.data() + settings_.windowSamples; }
    const float* swellR()    const noexcept { return workR_.data() + settings_.windowSamples; }
    int          swellLength() const noexcept { return settings_.swellSamples + settings_.gapSamples; }

    const float* windowL()   const noexcept { return workL_.data(); }
    const float* windowR()   const noexcept { return workR_.data(); }
    int          windowLength() const noexcept { return settings_.windowSamples; }

    float getSourcePeak() const noexcept { return sourcePeak_; }
    int   getCapacity()   const noexcept { return static_cast<int> (workL_.size()); }

private:
    bool startCommon (const SwellSettings& s);
    void finalise() noexcept;

    FDNReverb fdn_;
    SwellSettings settings_;

    // One contiguous array per channel:  [ window (W) | tail → swell (S) | gap (G) ]
    std::vector<float> workL_, workR_;

    State state_ = State::Idle;
    int   pos_ = 0, total_ = 0, align_ = 0;
    float sourcePeak_ = 0.f;
    double preparedRate_ = 0.0;
};