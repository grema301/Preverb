#pragma once
#include <JuceHeader.h>
#include <array>
#include <vector>

// =============================================================
//  ScopeSnapshot — everything the display needs to draw ONE swell event.
//  Small (≈4 KB) and copied by value, so the UI never touches live audio buffers.
// =============================================================
struct ScopeSnapshot
{
    static constexpr int kBins = 512;

    std::array<float, kBins> source {};     // peak envelope of the source window (0..1)
    std::array<float, kBins> swell  {};     // peak envelope of swell lead-in + gap (0..1)

    float    windowSeconds = 0.f;
    float    swellSeconds  = 0.f;           // lead-in only
    float    gapSeconds    = 0.f;
    uint32_t serial        = 0;             // bumps on every publish → UI knows when to redraw
    bool     valid         = false;
};

// Reduce n samples (2 channels) to `bins` peak values. Safe for n < bins.
namespace ScopeMath
{
    inline void computePeaks (const float* l, const float* r, int n, float* out, int bins, float scale)
    {
        for (int b = 0; b < bins; ++b)
        {
            if (n <= 0) { out[b] = 0.f; continue; }

            const int s0 = static_cast<int> ((static_cast<int64_t> (n) * b)       / bins);
            int       s1 = static_cast<int> ((static_cast<int64_t> (n) * (b + 1)) / bins);
            if (s1 <= s0) s1 = juce::jmin (n, s0 + 1);

            float pk = 0.f;
            for (int i = s0; i < s1; ++i)
                pk = juce::jmax (pk, juce::jmax (std::abs (l[i]), std::abs (r[i])));

            out[b] = pk * scale;
        }
    }
}

// =============================================================
//  SwellStore — "the latest swell", shared between threads.
//
//  Writers : the audio thread (live mode) or the file worker thread.
//  Readers : the audio thread (to play it), the UI (to draw / export it).
//
//  A SpinLock guards copies only. The audio thread NEVER waits: it uses
//  try-lock variants and simply retries on the next block if the UI or worker
//  happens to be mid-copy (hold times are a memcpy, i.e. well under a millisecond).
//  Storage is allocated up front, so publishing never allocates.
// =============================================================
class SwellStore
{
public:
    void allocate (int maxSamples)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        left.assign  (static_cast<size_t> (juce::jmax (1, maxSamples)), 0.f);
        right.assign (left.size(), 0.f);
        length = 0;
        snap   = {};
    }

    int getCapacity() const noexcept { return static_cast<int> (left.size()); }

    // ── Writers ──────────────────────────────────────────────
    // Audio-thread safe: returns false if the lock was busy (caller retries later).
    bool tryPublish (const float* l, const float* r, int n, double sr, const ScopeSnapshot& s)
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked()) return false;
        copyIn (l, r, n, sr, s);
        return true;
    }

    // Worker-thread version (may spin for a moment).
    void publish (const float* l, const float* r, int n, double sr, const ScopeSnapshot& s)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        copyIn (l, r, n, sr, s);
    }

    void clear()
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        length = 0;
        snap   = {};
        snap.serial = ++serial;
    }

    // ── Readers ──────────────────────────────────────────────
    // Audio thread: run f (left, right, length, sampleRate) while holding the lock.
    // Returns false if the lock was busy. f must be short and must not allocate.
    template <typename F>
    bool tryVisit (F&& f) const
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked()) return false;
        if (length > 0) f (left.data(), right.data(), length, sampleRate);
        return true;
    }

    // UI thread.
    ScopeSnapshot getSnapshot() const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        return snap;
    }

    // UI thread: copy the audio out (for export / drag-out). Returns false if empty.
    bool copyAudio (juce::AudioBuffer<float>& dest, double& sr) const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (length <= 0) return false;

        dest.setSize (2, length, false, false, true);
        dest.copyFrom (0, 0, left.data(),  length);
        dest.copyFrom (1, 0, right.data(), length);
        sr = sampleRate;
        return true;
    }

private:
    void copyIn (const float* l, const float* r, int n, double sr, const ScopeSnapshot& s)
    {
        n = juce::jlimit (0, static_cast<int> (left.size()), n);
        std::copy (l, l + n, left.begin());
        std::copy (r, r + n, right.begin());
        length     = n;
        sampleRate = sr;
        snap       = s;
        snap.valid = n > 0;
        snap.serial = ++serial;
    }

    mutable juce::SpinLock lock;
    std::vector<float> left, right;
    int      length     = 0;
    double   sampleRate = 44100.0;
    uint32_t serial     = 0;
    ScopeSnapshot snap;
};