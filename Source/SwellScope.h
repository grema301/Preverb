#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PreverbLookAndFeel.h"

// Everything the display needs beyond the waveform snapshots.
struct ScopeView
{
    bool      fileMode = false, autoMode = true, dragHover = false, printMode = false;
    Phase     phase = Phase::Listening;
    float     phaseProgress = 0.f, secondsToHit = 0.f;
    float     bpm = 120.f;
    float     latencySec = 2.f, marginSec = 0.25f;
    float     windowReq = 0.f, windowEff = 0.f, swellReq = 0.f, swellEff = 0.f, gapSec = 0.f;
    int       warnings = 0;
    FileState fileState = FileState::Empty;
    float     fileProgress = 0.f;
    uint32_t  hitCount = 0;
    juce::String fileName;
};

// =============================================================
//  SwellScope
//
//     ◄──── swell (builds up) ────┃HIT┃──── source window ────►
//
//  The hit sits on a fixed vertical line. Left of it is the swell that was (or will
//  be) generated; right of it is the part of the source that fed the reverb.
//  In live mode you watch the source "load" in from the hit line as it is captured,
//  a dashed ghost shows the swell to come, and when rendering is done the real swell
//  wipes in and a cursor counts down to the hit. A lookahead budget bar underneath
//  shows how window + render time + swell + gap fit into the reported latency.
// =============================================================
class SwellScope : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }

    void setData (const ScopeSnapshot& s, const CaptureSnapshot& c, const ScopeView& v)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (s.serial != snap.serial)                    revealStart = now;
        if (haveView && v.hitCount != view.hitCount)    flashStart  = now;

        snap = s;  cap = c;  view = v;  haveView = true;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        auto b = getLocalBounds().toFloat();

        g.setColour (Palette::panel);
        g.fillRoundedRectangle (b, 10.f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (b.reduced (0.5f), 10.f, 1.f);

        auto inner      = b.reduced (16.f, 12.f);
        auto header     = inner.removeFromTop (22.f);
        auto budgetArea = inner.removeFromBottom (42.f);
        inner.removeFromBottom (4.f);
        auto rulerArea  = inner.removeFromBottom (16.f);
        auto wave       = inner.reduced (0.f, 6.f);

        // ── What are we looking at? ──────────────────────────
        const bool haveSnap    = snap.valid;
        const bool liveCapture = ! view.fileMode && cap.active
                                 && (view.phase == Phase::Capturing || view.phase == Phase::Rendering);
        const bool fileBusy    = view.fileMode && (view.fileState == FileState::Rendering || view.fileState == FileState::Loading);
        const bool showReal    = haveSnap && ! liveCapture;
        const float stale      = (fileBusy && showReal) ? 0.45f : 1.f;

        float swellTotal = showReal ? snap.swellSeconds + snap.gapSeconds : view.swellEff + view.gapSec;
        float swellLead  = showReal ? snap.swellSeconds : view.swellEff;
        float windowReal = liveCapture ? cap.windowSeconds : showReal ? snap.windowSeconds : view.windowEff;

        swellTotal = juce::jmax (swellTotal, 0.1f);
        swellLead  = juce::jlimit (0.05f, swellTotal, swellLead);
        windowReal = juce::jmax (windowReal, 0.02f);
        const float windowLayout = juce::jmax (windowReal, 0.22f * swellTotal);   // keep the source region visible
        const float totalSec     = swellTotal + windowLayout;

        auto xAt = [&] (float t) { return wave.getX() + (t + swellTotal) / totalSec * wave.getWidth(); };
        const float hitX = xAt (0.f);

        // ── Beat grid + ruler (beats counted from the hit) ───
        {
            const float beatSec   = 60.f / juce::jmax (20.f, view.bpm);
            const float pxPerBeat = beatSec / totalSec * wave.getWidth();
            const int   kMin = -static_cast<int> (std::floor (swellTotal   / beatSec));
            const int   kMax =  static_cast<int> (std::floor (windowLayout / beatSec));
            const int   labelEvery = pxPerBeat >= 34.f ? 1 : (pxPerBeat * 4.f >= 34.f ? 4 : 0);

            g.setFont (makeFont (10.f));
            for (int k = kMin; k <= kMax; ++k)
            {
                const bool bar = (k % 4) == 0;
                if (! bar && pxPerBeat < 7.f) continue;

                const float x = xAt (static_cast<float> (k) * beatSec);
                g.setColour (Palette::border.withAlpha (bar ? 0.95f : 0.45f));
                g.drawVerticalLine (static_cast<int> (x), wave.getY(), wave.getBottom());

                if (labelEvery != 0 && (k % labelEvery) == 0 && x > wave.getX() + 46.f)
                {
                    const auto txt = k == 0 ? juce::String ("0")
                                   : k < 0  ? kMinus + juce::String (-k)
                                            : "+" + juce::String (k);
                    g.setColour (Palette::textDim);
                    g.drawText (txt, juce::Rectangle<float> (x - 20.f, rulerArea.getY(), 40.f, rulerArea.getHeight()),
                                juce::Justification::centred, false);
                }
            }
            g.setColour (Palette::textDim.withAlpha (0.7f));
            g.drawText ("beats", rulerArea.removeFromLeft (40.f), juce::Justification::centredLeft, false);
        }

        g.setColour (Palette::border);
        g.drawHorizontalLine (static_cast<int> (wave.getCentreY()), wave.getX(), wave.getRight());

        const auto swellRect  = juce::Rectangle<float> (xAt (-swellTotal), wave.getY(), hitX - xAt (-swellTotal), wave.getHeight());
        const auto sourceRect = juce::Rectangle<float> (hitX, wave.getY(), windowReal / totalSec * wave.getWidth(), wave.getHeight());

        const bool rendering = view.phase == Phase::Rendering || (view.fileMode && view.fileState == FileState::Rendering);

        // ── Swell: real, or ghost of what is about to be generated ──
        if (showReal)
        {
            const float reveal = 1.f - std::pow (1.f - juce::jlimit (0.f, 1.f, static_cast<float> ((now - revealStart) / 520.0)), 3.f);
            const auto path = makeEnvelope (snap.swell.data(), ScopeSnapshot::kBins, swellRect, 0.55f);

            g.saveState();
            g.reduceClipRegion (swellRect.withWidth (swellRect.getWidth() * reveal).getSmallestIntegerContainer());
            g.setGradientFill (juce::ColourGradient (Palette::amber.withAlpha (0.20f * stale), swellRect.getX(), 0.f,
                                                     Palette::pink.withAlpha  (0.90f * stale), hitX, 0.f, false));
            g.fillPath (path);
            g.setColour (Palette::pink.withAlpha (0.85f * stale));
            g.strokePath (path, juce::PathStrokeType (1.f));
            g.restoreState();
        }
        else
        {
            // Ghost: the natural shape of a reversed reverb tail, −60 dB rising to 0 dB at the hit
            const auto ghost = makeGhost (swellRect, swellLead / swellTotal);
            g.setColour (Palette::amber.withAlpha (0.08f));
            g.fillPath (ghost);

            juce::Path dashed;
            const float dashes[] = { 4.f, 4.f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, ghost, dashes, 2);
            g.setColour (Palette::amber.withAlpha (0.55f));
            g.strokePath (dashed, juce::PathStrokeType (1.2f));

            if (rendering)
            {
                const float bandX = swellRect.getX() + std::fmod (static_cast<float> (now) / 650.f, 1.f) * (swellRect.getWidth() + 80.f) - 80.f;
                g.saveState();
                g.reduceClipRegion (juce::Rectangle<float> (bandX, swellRect.getY(), 80.f, swellRect.getHeight()).getSmallestIntegerContainer());
                g.setColour (Palette::amber.withAlpha (0.55f));
                g.fillPath (ghost);
                g.restoreState();
            }
        }

        // ── Source: filling in live, or the captured / loaded window ──
        if (liveCapture)
        {
            g.setColour (Palette::teal.withAlpha (0.05f));
            g.fillRect (sourceRect);

            float mx = 0.05f;
            for (float v : cap.peaks) mx = juce::jmax (mx, v);
            auto norm = cap.peaks;
            for (auto& v : norm) v = juce::jmin (1.f, v / mx);

            const auto path = makeEnvelope (norm.data(), ScopeSnapshot::kBins, sourceRect, 0.7f);
            g.setColour (Palette::teal.withAlpha (0.45f));
            g.fillPath (path);
            g.setColour (Palette::teal.withAlpha (0.9f));
            g.strokePath (path, juce::PathStrokeType (1.f));

            if (view.phase == Phase::Capturing)
            {
                const float hx = sourceRect.getX() + cap.progress * sourceRect.getWidth();
                g.setColour (Palette::teal);
                g.fillRect (hx - 1.f, sourceRect.getY(), 2.f, sourceRect.getHeight());
            }
        }
        else if (showReal)
        {
            const auto path = makeEnvelope (snap.source.data(), ScopeSnapshot::kBins, sourceRect, 0.7f);
            g.setColour (Palette::teal.withAlpha (0.40f * stale));
            g.fillPath (path);
            g.setColour (Palette::teal.withAlpha (0.90f * stale));
            g.strokePath (path, juce::PathStrokeType (1.f));
        }
        else
        {
            g.setColour (Palette::teal.withAlpha (0.05f));
            g.fillRect (sourceRect);
        }

        // Region captions
        g.setFont (makeFont (10.f, true));
        g.setColour (Palette::amber.withAlpha (0.8f));
        g.drawText ("SWELL  " + juce::String (swellLead, 2) + " s", swellRect.reduced (6.f, 2.f).withHeight (14.f),
                    juce::Justification::centredLeft, false);
        g.setColour (Palette::teal.withAlpha (0.8f));
        g.drawText ((view.fileMode ? "FILE  " : "SOURCE  ") + juce::String (windowReal, 2) + " s",
                    juce::Rectangle<float> (hitX + 14.f, sourceRect.getY() + 2.f, juce::jmax (90.f, sourceRect.getWidth()), 14.f),
                    juce::Justification::centredLeft, false);

        // ── The hit ──────────────────────────────────────────
        {
            const float flash = juce::jlimit (0.f, 1.f, 1.f - static_cast<float> ((now - flashStart) / 450.0));
            g.setColour (Palette::pink.withAlpha (0.10f + 0.25f * flash));
            g.fillRect (hitX - 5.f - 4.f * flash, wave.getY() - 6.f, 10.f + 8.f * flash, wave.getHeight() + 12.f);
            g.setColour (juce::Colours::white.withAlpha (0.65f + 0.35f * flash));
            g.fillRect (hitX - 0.75f, wave.getY() - 6.f, 1.5f, wave.getHeight() + 12.f);

            const auto pill = juce::Rectangle<float> (hitX - 17.f, wave.getY() - 14.f, 34.f, 14.f);
            g.setColour (juce::Colours::white.withAlpha (0.92f));
            g.fillRoundedRectangle (pill, 4.f);
            g.setColour (Palette::bg);
            g.setFont (makeFont (10.f, true));
            g.drawText ("HIT", pill, juce::Justification::centred, false);
        }

        // ── Countdown cursor sweeping toward the hit ─────────
        if ((view.phase == Phase::Scheduled || view.phase == Phase::Swelling) && view.secondsToHit > 0.f
            && view.secondsToHit <= swellTotal)
        {
            const float x = xAt (-view.secondsToHit);
            g.setColour (Palette::amber.withAlpha (0.25f));
            g.fillRect (x - 4.f, wave.getY(), 8.f, wave.getHeight());
            g.setColour (Palette::amber);
            g.fillRect (x - 1.f, wave.getY(), 2.f, wave.getHeight());

            const auto tag = juce::Rectangle<float> (x - 28.f, wave.getBottom() - 18.f, 56.f, 15.f);
            g.setColour (Palette::panel.withAlpha (0.92f));
            g.fillRoundedRectangle (tag, 4.f);
            g.setColour (Palette::amber);
            g.setFont (makeFont (10.f, true));
            g.drawText (kMinus + juce::String (view.secondsToHit, 2) + " s", tag, juce::Justification::centred, false);
        }

        // ── Header ───────────────────────────────────────────
        drawHeader (g, header, swellLead, windowReal);

        // ── File-mode overlays ───────────────────────────────
        if (view.fileMode)
            drawFileOverlay (g, wave, now);

        drawBudget (g, budgetArea);
    }

private:
    const juce::String kMinus = juce::String::fromUTF8 ("\xe2\x88\x92");
    const juce::String kDot   = juce::String::fromUTF8 (" \xc2\xb7 ");

    bool haveSnap() const { return snap.valid; }

    ScopeSnapshot   snap;
    CaptureSnapshot cap;
    ScopeView       view;
    bool   haveView = false;
    double revealStart = 0.0, flashStart = -10000.0;

    // Mirrored peak envelope, with mild gamma so quiet detail stays visible.
    static juce::Path makeEnvelope (const float* bins, int n, juce::Rectangle<float> r, float gamma)
    {
        juce::Path p;
        const float cy = r.getCentreY(), half = r.getHeight() * 0.5f * 0.92f;
        p.startNewSubPath (r.getX(), cy);
        for (int i = 0; i < n; ++i)
            p.lineTo (r.getX() + r.getWidth() * (static_cast<float> (i) + 0.5f) / static_cast<float> (n),
                      cy - std::pow (juce::jlimit (0.f, 1.f, bins[i]), gamma) * half);
        p.lineTo (r.getRight(), cy);
        for (int i = n - 1; i >= 0; --i)
            p.lineTo (r.getX() + r.getWidth() * (static_cast<float> (i) + 0.5f) / static_cast<float> (n),
                      cy + std::pow (juce::jlimit (0.f, 1.f, bins[i]), gamma) * half);
        p.closeSubPath();
        return p;
    }

    static juce::Path makeGhost (juce::Rectangle<float> r, float leadFraction)
    {
        constexpr int n = 160;
        float bins[n];
        for (int i = 0; i < n; ++i)
        {
            const float f = (static_cast<float> (i) + 0.5f) / static_cast<float> (n);
            bins[i] = f > leadFraction ? 0.f : 0.92f * std::pow (10.f, -3.f * (1.f - f / leadFraction));
        }
        return makeEnvelope (bins, n, r, 0.55f);
    }

    void drawHeader (juce::Graphics& g, juce::Rectangle<float> r, float swellLead, float windowReal) const
    {
        juce::String text;
        juce::Colour col = Palette::textDim;
        const auto pct = juce::String (juce::roundToInt (view.phaseProgress * 100.f)) + "%";

        const bool idleLike = view.phase == Phase::Listening;
        const bool justRendered = view.printMode && ! view.fileMode && haveSnap()
                                  && (juce::Time::getMillisecondCounterHiRes() - revealStart) < 6000.0;

        if (view.fileMode && view.fileState != FileState::Ready && idleLike)
        {
            switch (view.fileState)
            {
                case FileState::Empty:     text = "DROP AN AUDIO FILE ON THE DISPLAY"; break;
                case FileState::Loading:   text = "LOADING FILE"; col = Palette::teal; break;
                case FileState::Rendering: text = "RENDERING SWELL  " + juce::String (juce::roundToInt (view.fileProgress * 100.f)) + "%"; col = Palette::amber; break;
                case FileState::Error:     text = "COULD NOT LOAD THIS FILE"; col = Palette::warn; break;
                default: break;
            }
        }
        else
        {
            switch (view.phase)
            {
                case Phase::Listening:
                    if (view.printMode)
                    {
                        text = justRendered ? "SWELL READY" + kDot + "drag it to your DAW"
                             : view.fileMode ? "PRINT MODE" + kDot + "press Audition, or drag the swell out"
                             : view.autoMode ? "PRINT MODE" + kDot + "listening for hits, no latency added"
                                             : "PRINT MODE" + kDot + "waiting for a MIDI note or Trigger";
                        col = justRendered ? Palette::pink : Palette::good;
                        break;
                    }
                    text = view.fileMode ? "READY" + kDot + "trigger it, or press Audition"
                         : view.autoMode ? "LISTENING" + kDot + "waiting for a hit"
                                         : "ARMED" + kDot + "waiting for a MIDI note or Trigger";
                    col = Palette::good;
                    break;
                case Phase::Capturing:  text = "CAPTURING SOURCE  " + pct; col = Palette::teal; break;
                case Phase::Rendering:  text = "RENDERING SWELL  " + pct;  col = Palette::amber; break;
                case Phase::Scheduled:  text = "SWELL QUEUED" + kDot + "hit in " + juce::String (view.secondsToHit, 2) + " s"; col = Palette::pink; break;
                case Phase::Swelling:   text = "SWELLING" + kDot + "hit in " + juce::String (view.secondsToHit, 2) + " s"; col = Palette::pink; break;
            }
        }

        auto dot = r.removeFromLeft (14.f).withSizeKeepingCentre (8.f, 8.f);
        g.setColour (col);
        g.fillEllipse (dot);

        g.setFont (makeFont (12.f, true));
        g.drawText (text, r.withTrimmedLeft (2.f), juce::Justification::centredLeft, false);

        g.setFont (makeFont (11.f));
        g.setColour (Palette::textDim);
        g.drawText ("swell " + juce::String (swellLead, 2) + " s" + kDot + "window " + juce::String (windowReal, 2) + " s"
                    + kDot + "gap " + juce::String (juce::roundToInt (view.gapSec * 1000.f)) + " ms",
                    r, juce::Justification::centredRight, false);
    }

    void drawFileOverlay (juce::Graphics& g, juce::Rectangle<float> wave, double now) const
    {
        const bool empty = view.fileState == FileState::Empty || view.fileState == FileState::Error;

        if (empty || view.dragHover)
        {
            auto box = wave.reduced (wave.getWidth() * 0.12f, wave.getHeight() * 0.16f);
            const auto col = view.dragHover ? Palette::amber : Palette::textDim;

            g.setColour (Palette::panel.withAlpha (view.dragHover ? 0.82f : 0.70f));
            g.fillRoundedRectangle (box, 12.f);

            juce::Path outline;
            outline.addRoundedRectangle (box, 12.f);
            juce::Path dashed;
            const float dashes[] = { 7.f, 5.f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (col);
            g.strokePath (dashed, juce::PathStrokeType (1.5f));

            g.setFont (makeFont (17.f, true));
            g.drawText (view.dragHover ? "Release to load" : (view.fileState == FileState::Error ? "That file could not be read" : "Drop an audio file here"),
                        box.withTrimmedBottom (box.getHeight() * 0.35f), juce::Justification::centred, false);
            g.setFont (makeFont (12.f));
            g.setColour (Palette::textDim);
            g.drawText ("WAV, AIFF, FLAC or OGG  " + kDot + "  or use Load below. The swell is built from its first moments.",
                        box.withTrimmedTop (box.getHeight() * 0.55f), juce::Justification::centred, false);
        }
        else if (view.fileState == FileState::Rendering || view.fileState == FileState::Loading)
        {
            auto bar = wave.removeFromBottom (3.f);
            g.setColour (Palette::border);
            g.fillRect (bar);
            g.setColour (Palette::amber);
            const float p = view.fileState == FileState::Loading
                          ? std::fmod (static_cast<float> (now) / 900.f, 1.f)
                          : view.fileProgress;
            g.fillRect (view.fileState == FileState::Loading
                        ? juce::Rectangle<float> (bar.getX() + p * bar.getWidth() * 0.8f, bar.getY(), bar.getWidth() * 0.2f, bar.getHeight())
                        : bar.withWidth (bar.getWidth() * p));
        }
    }

    // ── Lookahead budget ─────────────────────────────────────
    // The whole bar is the reported latency L: from the moment a hit is detected
    // until it is HEARD. Everything has to fit inside it.
    void drawBudget (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        // Print mode has no latency: the bar shows what will be captured and rendered instead.
        const float L = view.printMode
            ? juce::jmax (0.1f, (view.fileMode ? 0.f : view.windowEff + 0.25f) + view.swellEff + view.gapSec)
            : juce::jmax (view.latencySec, 0.1f);

        auto top = area.removeFromTop (16.f);
        g.setFont (makeFont (10.f, true));
        g.setColour (view.printMode ? Palette::good : Palette::textDim);
        g.drawText (view.printMode ? juce::String ("PRINT MODE  no latency, swells are rendered for export")
                                   : "LOOKAHEAD  " + juce::String (L, 1) + " s  (reported latency)",
                    top.removeFromLeft (view.printMode ? 330.f : 230.f), juce::Justification::centredLeft, false);

        // Warning text (right of the title)
        juce::String warn;
        const int w = view.warnings;
        if (view.printMode && (w & (WarnWindowClamped | WarnSwellClamped)))
            warn = "Window + swell limited to 6 s in print mode (" + juce::String (view.windowEff, 2) + " s + " + juce::String (view.swellEff, 2) + " s)";
        else if (w & WarnNoRoom)        warn = "Not enough lookahead: raise it, or shorten Window / Swell / Gap";
        else if ((w & WarnSwellClamped) && (w & WarnWindowClamped))
            warn = "Window and swell shortened to fit the lookahead (" + juce::String (view.windowEff, 2) + " s + " + juce::String (view.swellEff, 2) + " s). Raise Lookahead for full length";
        else if (w & WarnSwellClamped)  warn = "Swell limited to " + juce::String (view.swellEff, 2) + " s (wanted " + juce::String (view.swellReq, 2) + " s). Raise Lookahead for more";
        else if (w & WarnWindowClamped) warn = "Window limited to " + juce::String (view.windowEff, 2) + " s (wanted " + juce::String (view.windowReq, 2) + " s)";
        else if (w & WarnFileTruncated) warn = "Playback starts part-way into the swell (lookahead is shorter). Export stays full length";
        else if (w & WarnFileShort)     warn = "File is shorter than the window";

        if (warn.isNotEmpty())
        {
            g.setColour (Palette::warn);
            g.setFont (makeFont (10.f));
            g.drawText (warn, top, juce::Justification::centredRight, false);
        }

        auto bar = area.removeFromBottom (18.f).reduced (0.f, 1.f);
        g.setColour (Palette::bg);
        g.fillRoundedRectangle (bar, 4.f);

        struct Seg { float sec; juce::Colour col; const char* name; };
        std::vector<Seg> segs;

        if (view.printMode)
        {
            segs = { { view.fileMode ? 0.f : view.windowEff, Palette::teal.withAlpha (0.85f),      "window" },
                     { view.fileMode ? 0.f : 0.25f,          Palette::textDim.withAlpha (0.55f),   "render" },
                     { view.swellEff,                        Palette::amber.withAlpha (0.90f),     "swell" },
                     { view.gapSec,                          Palette::pink.withAlpha (0.35f),      "gap" } };
        }
        else if (! view.fileMode)
        {
            const float used  = view.windowEff + view.marginSec + view.swellEff + view.gapSec;
            const float spare = juce::jmax (0.f, L - used);
            segs = { { view.windowEff, Palette::teal.withAlpha (0.85f),  "window" },
                     { view.marginSec, Palette::textDim.withAlpha (0.55f), "render" },
                     { spare,          Palette::border.withAlpha (0.5f),  "" },
                     { view.swellEff,  Palette::amber.withAlpha (0.90f),  "swell" },
                     { view.gapSec,    Palette::pink.withAlpha (0.35f),   "gap" } };
        }
        else
        {
            const float total = view.swellReq + view.gapSec;
            const float idle  = juce::jmax (0.f, L - total);
            const float cut   = juce::jmax (0.f, total - L);
            segs = { { idle,                              Palette::border.withAlpha (0.5f), "" },
                     { cut,                               Palette::warn.withAlpha (0.55f),  "cut" },
                     { juce::jmax (0.f, view.swellReq - cut), Palette::amber.withAlpha (0.90f), "swell" },
                     { view.gapSec,                       Palette::pink.withAlpha (0.35f),  "gap" } };
        }

        float sum = 0.f;
        for (auto& s : segs) sum += s.sec;
        sum = juce::jmax (sum, L);

        float x = bar.getX();
        g.setFont (makeFont (10.f, true));
        for (auto& s : segs)
        {
            const float wpx = s.sec / sum * bar.getWidth();
            if (wpx < 0.5f) continue;

            const auto seg = juce::Rectangle<float> (x, bar.getY(), wpx, bar.getHeight()).reduced (0.5f, 0.f);
            g.setColour (s.col);
            g.fillRect (seg);

            if (wpx > 44.f && s.name[0] != '\0')
            {
                g.setColour (Palette::bg);
                g.drawText (s.name, seg, juce::Justification::centred, false);
            }
            x += wpx;
        }

        g.setColour (Palette::border);
        g.drawRoundedRectangle (bar, 4.f, 1.f);
    }
};