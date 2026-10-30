#include "PluginProcessor.h"
#include "PluginEditor.h"

// =============================================================
//  Text helpers for the beat-based parameters
// =============================================================
namespace
{
    juce::String beatsToText (float beats)
    {
        const float bars = beats / 4.f;

        if (beats >= 4.f && std::abs (bars - std::round (bars)) < 0.001f)
        {
            const int n = static_cast<int> (std::round (bars));
            return juce::String (n) + (n == 1 ? " bar" : " bars");
        }

        if (std::abs (beats - std::round (beats)) < 0.001f)
        {
            const int n = static_cast<int> (std::round (beats));
            return juce::String (n) + (n == 1 ? " beat" : " beats");
        }

        if (beats < 1.f)
        {
            int num = static_cast<int> (std::round (beats * 16.f)), den = 16;
            while (num % 2 == 0 && den > 1) { num /= 2; den /= 2; }
            return juce::String (num) + "/" + juce::String (den) + " beat";
        }

        auto s = juce::String (beats, 3);
        while (s.endsWithChar ('0')) s = s.dropLastCharacters (1);
        if (s.endsWithChar ('.'))    s = s.dropLastCharacters (1);
        return s + " beats";
    }

    float textToBeats (const juce::String& text)
    {
        const auto t   = text.trim().toLowerCase();
        const float m  = t.contains ("bar") ? 4.f : 1.f;
        const auto num = t.retainCharacters ("0123456789./");

        if (num.contains ("/"))
        {
            const float a = num.upToFirstOccurrenceOf ("/", false, false).getFloatValue();
            const float b = num.fromFirstOccurrenceOf ("/", false, false).getFloatValue();
            if (b != 0.f) return m * a / b;
        }
        return m * num.getFloatValue();
    }

    juce::String dbToText (float v, float minDb)
    {
        return v <= minDb + 0.05f ? juce::String ("-inf dB") : juce::String (v, 1) + " dB";
    }

    constexpr float kMinusInfDb = -59.9f;
}

// =============================================================
//  PARAMETER LAYOUT
// =============================================================
juce::AudioProcessorValueTreeState::ParameterLayout
PreverbAudioProcessor::createParameterLayout()
{
    using PID  = juce::ParameterID;
    using Attr = juce::AudioParameterFloatAttributes;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;

    // Source & trigger
    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        PID { ParamID::SOURCE, 1 }, "Source", juce::StringArray { "Live", "File" }, 0));

    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        PID { ParamID::MODE, 1 }, "Trigger Mode", juce::StringArray { "Auto", "Manual" }, 0));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::SENSITIVITY, 1 }, "Sensitivity",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        PID { ParamID::TRIGGER, 1 }, "Trigger", false));

    // Timing (beats)
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::WINDOW_BEATS, 1 }, "Window",
        juce::NormalisableRange<float> (0.0625f, 8.f, 0.0625f, 0.5f), 1.f,
        Attr().withStringFromValueFunction ([] (float v, int) { return beatsToText (v); })
              .withValueFromStringFunction ([] (const juce::String& s) { return textToBeats (s); })));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::SWELL_BEATS, 1 }, "Swell Length",
        juce::NormalisableRange<float> (0.125f, 8.f, 0.125f, 0.5f), 2.f,
        Attr().withStringFromValueFunction ([] (float v, int) { return beatsToText (v); })
              .withValueFromStringFunction ([] (const juce::String& s) { return textToBeats (s); })));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::BPM, 1 }, "BPM",
        juce::NormalisableRange<float> (40.f, 240.f, 0.1f), 120.f,
        Attr().withLabel ("bpm")));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        PID { ParamID::USE_HOST_BPM, 1 }, "Use Host BPM", true));

    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        PID { ParamID::LOOKAHEAD, 1 }, "Lookahead Time",
        juce::StringArray { "1 s", "2 s", "3 s", "4 s", "6 s" }, 1));

    p.push_back (std::make_unique<juce::AudioParameterBool> (
        PID { ParamID::LOOKAHEAD_ON, 1 }, "Lookahead", true));

    // Reverb character
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::SIZE, 1 }, "Size",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.5f));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::DAMPING, 1 }, "Damping",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.4f));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::DIFFUSION, 1 }, "Diffusion",
        juce::NormalisableRange<float> (0.f, 1.f, 0.01f), 0.7f));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::GAP, 1 }, "Gap",
        juce::NormalisableRange<float> (0.f, 250.f, 1.f, 0.6f), 0.f,
        Attr().withLabel ("ms")));

    // Output
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::DRY_DB, 1 }, "Dry",
        juce::NormalisableRange<float> (-60.f, 6.f, 0.1f), 0.f,
        Attr().withStringFromValueFunction ([] (float v, int) { return dbToText (v, -60.f); })));

    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        PID { ParamID::SWELL_DB, 1 }, "Swell",
        juce::NormalisableRange<float> (-24.f, 12.f, 0.1f), -3.f,
        Attr().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })));

    return { p.begin(), p.end() };
}

// =============================================================
//  CONSTRUCTOR / DESTRUCTOR
// =============================================================
PreverbAudioProcessor::PreverbAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PREVERB_STATE", createParameterLayout())
{
    formatManager.registerBasicFormats();

    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    raw.source = get (ParamID::SOURCE);        raw.mode      = get (ParamID::MODE);
    raw.sensitivity = get (ParamID::SENSITIVITY); raw.trigger = get (ParamID::TRIGGER);
    raw.window = get (ParamID::WINDOW_BEATS);  raw.swell     = get (ParamID::SWELL_BEATS);
    raw.bpm = get (ParamID::BPM);              raw.useHost   = get (ParamID::USE_HOST_BPM);
    raw.lookahead = get (ParamID::LOOKAHEAD);  raw.lookaheadOn = get (ParamID::LOOKAHEAD_ON);
    raw.size = get (ParamID::SIZE);
    raw.damping = get (ParamID::DAMPING);      raw.diffusion = get (ParamID::DIFFUSION);
    raw.gap = get (ParamID::GAP);              raw.dryDb     = get (ParamID::DRY_DB);
    raw.swellDb = get (ParamID::SWELL_DB);

    // Anything that changes the file-mode swell triggers a (debounced) re-render.
    for (auto* id : { ParamID::WINDOW_BEATS, ParamID::SWELL_BEATS, ParamID::BPM, ParamID::USE_HOST_BPM,
                      ParamID::SIZE, ParamID::DAMPING, ParamID::DIFFUSION, ParamID::GAP })
        apvts.addParameterListener (id, this);

    worker = std::make_unique<FileWorker> (*this);
    worker->startThread();
}

PreverbAudioProcessor::~PreverbAudioProcessor()
{
    // Tell any editor that is still open to let go of our parameters NOW,
    // while they still exist (our members, including apvts, are destroyed after this body).
    std::vector<EditorLink*> links;
    {
        const juce::ScopedLock sl (editorLinkLock);
        links.swap (editorLinks);
    }
    for (auto* l : links)
        l->processorIsBeingDestroyed();

    for (auto* id : { ParamID::WINDOW_BEATS, ParamID::SWELL_BEATS, ParamID::BPM, ParamID::USE_HOST_BPM,
                      ParamID::SIZE, ParamID::DAMPING, ParamID::DIFFUSION, ParamID::GAP })
        apvts.removeParameterListener (id, this);

    cancelPendingUpdate();
    worker->stopThread (4000);
}

// =============================================================
//  PREPARE / RESET
// =============================================================
void PreverbAudioProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    isPrepared = false;
    sampleRate = sr;
    maxBlock   = juce::jmax (samplesPerBlock, 64);
    hostSampleRate.store (sr);

    const int Lmax = static_cast<int> (std::ceil (Arch::MAX_LOOKAHEAD_SECONDS * sr));
    renderCapacity = Lmax;

    histSize  = Lmax + maxBlock + 4096;
    accumSize = static_cast<int> (std::ceil (Arch::ACCUM_SECONDS * sr));

    history.setSize (2, histSize,  false, true, false);
    accum.setSize   (2, accumSize, false, true, false);

    for (auto& j : jobs)
        j.renderer.prepare (sr, Lmax + 16);

    liveStore.allocate (Lmax + 16);
    fileStore.allocate (static_cast<int> (std::ceil ((Arch::MAX_SWELL_SECONDS + Arch::MAX_GAP_SECONDS) * sr)) + 16);

    // Onset detector: band-limited (~150 Hz .. 4.5 kHz) so rumble and sibilance matter less
    auto onePole = [sr] (double hz)     { return static_cast<float> (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * hz / sr)); };
    auto envCoef = [sr] (double seconds){ return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sr))); };
    hpCoef  = onePole (150.0);
    lpCoef  = onePole (4500.0);
    fastAtk = envCoef (0.0005);  fastRel = envCoef (0.012);
    slowAtk = envCoef (0.040);   slowRel = envCoef (0.300);

    drySmoothed.reset   (sr, 0.02);
    swellSmoothed.reset (sr, 0.02);
    const auto p = readParams();
    drySmoothed.setCurrentAndTargetValue   (juce::Decibels::decibelsToGain (p.dryDb,   kMinusInfDb));
    swellSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.swellDb, kMinusInfDb));

    flushPipeline();

    activeLatency = lookaheadSamples (sr);
    latencyTarget.store (activeLatency);
    setLatencySamples (activeLatency);
    telemetry.latencySec.store (static_cast<float> (activeLatency / sr));

    lastPlaying = false;
    haveHostTime = false;
    consistentBlocks = 0;

    ++fileRequestCounter;       // the file swell must be re-rendered for the new sample rate
    isPrepared = true;
}

void PreverbAudioProcessor::releaseResources() { isPrepared = false; }

void PreverbAudioProcessor::reset()
{
    if (isPrepared) flushPipeline();
}

bool PreverbAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& in  = l.getMainInputChannelSet();
    const auto& out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void PreverbAudioProcessor::flushPipeline()
{
    history.clear();
    accum.clear();
    histWrite = accumRead = 0;
    inputPos  = 0;

    for (auto& h : pending) h.active = false;
    for (auto& j : jobs)    { j.renderer.cancel(); j.busy = j.delivered = j.publishPending = false; }

    numFileRetry = 0;
    lastHitOut = lastSwellStart = 0;

    detLow = detBand = envFast = envSlow = 0.f;
    armed = true;
    holdOffUntil = 0;
    levelPeak = onsetPeak = 0.f;

    captureValid = false;
    captureDirty = true;       // tells the UI the capture is gone (published next block)
    prevTriggerHeld = false;

    telemetry.phase.store (static_cast<int> (Phase::Listening));
    telemetry.phaseProgress.store (0.f);
    telemetry.secondsToHit.store (0.f);
}

// =============================================================
//  PARAMETER SNAPSHOT / BUDGET
// =============================================================
PreverbAudioProcessor::Params PreverbAudioProcessor::readParams() const
{
    Params p;
    p.isFile      = raw.source->load()      > 0.5f;
    p.isAuto      = raw.mode->load()        < 0.5f;
    p.useHostBpm  = raw.useHost->load()     > 0.5f;
    p.sensitivity = raw.sensitivity->load();
    p.windowBeats = raw.window->load();
    p.swellBeats  = raw.swell->load();
    p.bpm         = raw.bpm->load();
    p.gapMs       = raw.gap->load();
    p.size        = raw.size->load();
    p.damping     = raw.damping->load();
    p.diffusion   = raw.diffusion->load();
    p.dryDb       = raw.dryDb->load();
    p.swellDb     = raw.swellDb->load();
    p.triggerHeld = raw.trigger->load() > 0.5f;
    return p;
}

int PreverbAudioProcessor::lookaheadSamples (double sr) const
{
    if (raw.lookaheadOn->load() < 0.5f)
        return 0;                                      // print mode: no latency at all

    const int idx = juce::jlimit (0, static_cast<int> (Arch::LOOKAHEAD_SECONDS.size()) - 1,
                                  static_cast<int> (raw.lookahead->load()));
    return static_cast<int> (std::lround (Arch::LOOKAHEAD_SECONDS[static_cast<size_t> (idx)] * sr));
}

// How much of the requested window + swell fits inside the lookahead?
//
//   L  >=  window + render margin + swell + gap
//
// If the request is too big, window and swell are scaled down TOGETHER (keeping the
// ratio you chose) instead of sacrificing one of them. The UI shows exactly what
// was shortened.
PreverbAudioProcessor::Budget PreverbAudioProcessor::computeBudget (const Params& p) const
{
    Budget b;
    const double secPerBeat = 60.0 / static_cast<double> (p.bpm);

    b.windowReq = static_cast<int> (std::lround (p.windowBeats * secPerBeat * sampleRate));
    b.swellReq  = static_cast<int> (std::lround (p.swellBeats  * secPerBeat * sampleRate));
    b.gap       = static_cast<int> (std::lround (p.gapMs * 0.001 * sampleRate));
    b.margin    = static_cast<int> (std::lround (Arch::RENDER_MARGIN_SECONDS * sampleRate)) + maxBlock;

    const int minSwell  = static_cast<int> (std::lround (Arch::MIN_SWELL_SECONDS * sampleRate));
    const int minWindow = static_cast<int> (std::lround (0.005 * sampleRate));

    // Print mode (lookahead off): nothing has to be ready before a deadline, so the only
    // limit is what one renderer can hold. Live mode: everything must fit inside the latency.
    const bool printMode = activeLatency <= 0;
    if (printMode) b.margin = 0;
    const int avail = printMode ? renderCapacity - b.gap
                                : activeLatency - b.gap - b.margin;

    if (avail < minWindow + minSwell)
    {
        b.noRoom   = true;
        b.warnings = WarnNoRoom;
        return b;
    }

    if (b.windowReq + b.swellReq <= avail)
    {
        b.window = b.windowReq;
        b.swell  = b.swellReq;
    }
    else
    {
        const double f = static_cast<double> (avail) / static_cast<double> (b.windowReq + b.swellReq);
        b.window = juce::jmax (minWindow, static_cast<int> (b.windowReq * f));
        b.swell  = juce::jmax (minSwell,  static_cast<int> (b.swellReq  * f));

        if (b.window + b.swell > avail)                              // the floors pushed us over
        {
            b.swell = avail - b.window;
            if (b.swell < minSwell) { b.swell = minSwell; b.window = avail - minSwell; }
        }
    }

    if (b.window < b.windowReq) b.warnings |= WarnWindowClamped;
    if (b.swell  < b.swellReq)  b.warnings |= WarnSwellClamped;
    return b;
}

// =============================================================
//  TRANSPORT / TEMPO
// =============================================================
void PreverbAudioProcessor::updateTransport (Params& p, int numSamples)
{
    double  hostBpm  = 0.0;
    bool    playing  = true;
    bool    haveTime = false;
    int64_t hostTime = 0;

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm())           hostBpm  = *b;
            playing = pos->getIsPlaying();
            if (auto t = pos->getTimeInSamples()) { haveTime = true; hostTime = *t; }
        }

    // Flush stale audio on transport start or on a clean seek / loop jump.
    // (A jump only counts after the host has behaved consistently for a few blocks,
    //  so an unusual host that reports erratic positions can never wipe our buffers.)
    bool flush = false;
    if (playing && ! lastPlaying && inputPos > 0) flush = true;

    if (haveTime && playing)
    {
        if (haveHostTime && std::llabs (hostTime - expectedHostTime) > 1)
        {
            if (consistentBlocks >= 4) flush = true;
            consistentBlocks = 0;
        }
        else if (haveHostTime)
            ++consistentBlocks;

        haveHostTime     = true;
        expectedHostTime = hostTime + numSamples;
    }
    else
    {
        haveHostTime = false;
        consistentBlocks = 0;
    }

    lastPlaying = playing;
    if (flush) { flushPipeline(); consistentBlocks = 0; }

    // Tempo
    const bool hostBpmOk = hostBpm > 20.0 && hostBpm < 400.0;
    hostBpmLast.store (hostBpmOk ? hostBpm : 0.0);

    bool fromHost = false;
    if (p.useHostBpm && hostBpmOk) { p.bpm = static_cast<float> (hostBpm); fromHost = true; }

    telemetry.bpm.store (p.bpm);
    telemetry.bpmFromHost.store (fromHost);

    if (std::abs (static_cast<double> (p.bpm) - lastBpm) > 0.01)
    {
        lastBpm = static_cast<double> (p.bpm);
        ++fileRequestCounter;            // file swell length depends on tempo
    }
}

// =============================================================
//  PROCESS BLOCK
// =============================================================
void PreverbAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh      = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (! isPrepared || numCh < 1 || numSamples < 1) return;

    Params p = readParams();
    updateTransport (p, numSamples);

    // Lookahead (latency) changed? Everything queued belongs to the old timeline.
    const int wantL = lookaheadSamples (sampleRate);
    if (wantL != activeLatency)
    {
        flushPipeline();
        activeLatency = wantL;
        latencyTarget.store (wantL);
        triggerAsyncUpdate();                   // tells the host (message thread)
    }

    const Budget budget = computeBudget (p);

    // ── Telemetry about the timing in effect ─────────────────
    {
        const float sr = static_cast<float> (sampleRate);
        const double secPerBeat = 60.0 / static_cast<double> (p.bpm);
        const float fileSwell = static_cast<float> (p.swellBeats * secPerBeat);
        const float fileWin   = static_cast<float> (p.windowBeats * secPerBeat);

        telemetry.latencySec.store (static_cast<float> (activeLatency) / sr);
        telemetry.printMode.store (activeLatency <= 0);
        telemetry.marginSec.store  (static_cast<float> (budget.margin) / sr);
        telemetry.gapSec.store     (static_cast<float> (budget.gap) / sr);
        telemetry.windowSecReq.store (static_cast<float> (budget.windowReq) / sr);
        telemetry.swellSecReq.store  (static_cast<float> (budget.swellReq)  / sr);

        if (p.isFile)
        {
            telemetry.windowSecEff.store (fileWin);
            telemetry.swellSecEff.store  (fileSwell);
            int w = workerWarn.load();
            if (activeLatency > 0 && budget.swellReq + budget.gap > activeLatency) w |= WarnFileTruncated;
            telemetry.warnings.store (w);
        }
        else
        {
            telemetry.windowSecEff.store (static_cast<float> (budget.window) / sr);
            telemetry.swellSecEff.store  (static_cast<float> (budget.swell)  / sr);
            telemetry.warnings.store (budget.warnings);
        }
    }

    // ── Collect this block's explicit triggers ───────────────
    int hitOffsets[32];
    int numHits = 0;

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn() && numHits < 32)
            hitOffsets[numHits++] = juce::jlimit (0, numSamples - 1, meta.samplePosition);
    }

    if (manualTriggerPending.exchange (false) && numHits < 32) hitOffsets[numHits++] = 0;
    if (p.triggerHeld && ! prevTriggerHeld && numHits < 32)    hitOffsets[numHits++] = 0;
    prevTriggerHeld = p.triggerHeld;
    std::sort (hitOffsets, hitOffsets + numHits);

    // ── Housekeeping at the top of the block ─────────────────
    for (int i = numFileRetry - 1; i >= 0; --i)
        if (placeFromStore (fileStore, fileRetry[static_cast<size_t> (i)]))
            fileRetry[static_cast<size_t> (i)] = fileRetry[static_cast<size_t> (--numFileRetry)];

    if (auditionPending.load())
        if (playStoreNow (p.isFile ? fileStore : liveStore))
            auditionPending.store (false);

    // ── Per-sample loop ──────────────────────────────────────
    float* chL = buffer.getWritePointer (0);
    float* chR = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
    float* hL  = history.getWritePointer (0);
    float* hR  = history.getWritePointer (1);
    float* aL  = accum.getWritePointer (0);
    float* aR  = accum.getWritePointer (1);

    const float ratioThresh = juce::jmap (p.sensitivity, 0.f, 1.f, 6.f, 1.8f);
    constexpr float absMin  = 0.004f;      // ~ -48 dBFS: never trigger on the noise floor
    const int holdOff = juce::jmax (static_cast<int> (std::lround (0.1 * sampleRate)),
                                    p.isFile ? static_cast<int> (std::lround (0.15 * sampleRate)) : budget.window);

    drySmoothed.setTargetValue   (juce::Decibels::decibelsToGain (p.dryDb,   kMinusInfDb));
    swellSmoothed.setTargetValue (juce::Decibels::decibelsToGain (p.swellDb, kMinusInfDb));

    float blockPeak = 0.f, blockActivity = 0.f;
    int nextHit = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = chL[i];
        const float inR = chR != nullptr ? chR[i] : inL;

        hL[histWrite] = inL;
        hR[histWrite] = inR;

        const float absIn = juce::jmax (std::abs (inL), std::abs (inR));
        blockPeak = juce::jmax (blockPeak, absIn);

        // The window that is currently filling: draw it as it arrives
        if (captureValid)
            captureAdd (inputPos - capture.t0, absIn);

        // ── Auto onset detection ──────────────────────────────
        // Ratio of a fast to a slow envelope (band-limited input). A hit is a
        // sudden jump above the recent average AND above an absolute floor.
        if (p.isAuto)
        {
            const float m = 0.5f * (inL + inR);
            detLow  += hpCoef * (m - detLow);
            const float hp = m - detLow;                       // high-passed
            detBand += lpCoef * (hp - detBand);                // …then low-passed
            const float a = std::abs (detBand);

            envFast += (a > envFast ? fastAtk : fastRel) * (a - envFast);
            envSlow += (a > envSlow ? slowAtk : slowRel) * (a - envSlow);

            const float floorEnv = juce::jmax (envSlow, absMin * 0.5f);
            blockActivity = juce::jmax (blockActivity, envFast / (ratioThresh * floorEnv));

            if (! armed)
            {
                if (envFast <= 1.3f * envSlow + 1.0e-7f) armed = true;     // hit has decayed → re-arm
            }
            else if (inputPos >= holdOffUntil && envFast > absMin && envFast > ratioThresh * envSlow)
            {
                armed = false;
                registerHit (findOnset (inputPos), p, budget);
                holdOffUntil = inputPos + holdOff;
            }
        }

        while (nextHit < numHits && hitOffsets[nextHit] <= i)
        {
            registerHit (inputPos, p, budget);
            ++nextHit;
        }

        // ── Dry (delayed by the lookahead) + scheduled swells ─
        int di = histWrite - activeLatency;
        if (di < 0) di += histSize;

        const float dryL = hL[di], dryR = hR[di];
        const float wetL = aL[accumRead], wetR = aR[accumRead];
        aL[accumRead] = 0.f;
        aR[accumRead] = 0.f;

        const float dg = drySmoothed.getNextValue();
        const float sg = swellSmoothed.getNextValue();

        if (chR != nullptr)
        {
            chL[i] = dryL * dg + wetL * sg;
            chR[i] = dryR * dg + wetR * sg;
        }
        else
        {
            chL[i] = dryL * dg + 0.5f * (wetL + wetR) * sg;
        }

        if (++histWrite >= histSize)   histWrite = 0;
        if (++accumRead >= accumSize)  accumRead = 0;
        ++inputPos;
    }

    // ── After the block: start due renders, advance running ones ──
    startDueJobs();
    advanceJobs (numSamples);
    updatePhaseTelemetry();
    if (captureDirty) publishCapture();

    // Meters (time-based decay, independent of block size)
    const float decay = std::exp (-static_cast<float> (numSamples) / (0.15f * static_cast<float> (sampleRate)));
    levelPeak = juce::jmax (blockPeak, levelPeak * decay);
    onsetPeak = juce::jmax (blockActivity, onsetPeak * decay);
    telemetry.inputLevel.store (levelPeak);
    telemetry.onsetActivity.store (p.isAuto ? onsetPeak : 0.f);
}

void PreverbAudioProcessor::handleAsyncUpdate()
{
    setLatencySamples (latencyTarget.load());
}

void PreverbAudioProcessor::parameterChanged (const juce::String&, float)
{
    ++fileRequestCounter;
}

// =============================================================
//  HITS
// =============================================================
void PreverbAudioProcessor::registerHit (int64_t t0, const Params& p, const Budget& b)
{
    telemetry.hitCount.fetch_add (1, std::memory_order_relaxed);

    // FILE source: the swell already exists. Schedule it so it ENDS at t0 + L.
    if (p.isFile)
    {
        if (activeLatency <= 0) return;      // print mode: nothing can lead into a hit without lookahead (use Audition / export)
        const int64_t hitOut = t0 + activeLatency;
        if (! placeFromStore (fileStore, hitOut) && numFileRetry < static_cast<int> (fileRetry.size()))
            fileRetry[static_cast<size_t> (numFileRetry++)] = hitOut;
        return;
    }

    // LIVE source: remember the hit; once its window has fully arrived we render.
    if (b.noRoom) return;

    for (auto& h : pending)
    {
        if (h.active) continue;

        h.active   = true;
        h.t0       = t0;
        h.fireAt   = t0 + b.window;
        h.hitOut   = t0 + activeLatency;
        h.window   = b.window;
        h.swell    = b.swell;
        h.gap      = b.gap;
        h.size     = p.size;
        h.damping  = p.damping;
        h.diffusion = p.diffusion;

        // Start the on-screen capture; back-fill the few ms we already passed (onset back-dating)
        capture.t0     = t0;
        capture.window = juce::jmax (1, b.window);
        capture.peaks.fill (0.f);
        captureValid = true;
        captureDirty = true;

        const int64_t already = juce::jmin<int64_t> (inputPos - t0, histSize - 2);
        for (int64_t k = already; k >= 0; --k)
        {
            int idx = histWrite - static_cast<int> (k);
            if (idx < 0) idx += histSize;
            captureAdd (inputPos - k - t0,
                        juce::jmax (std::abs (history.getSample (0, idx)), std::abs (history.getSample (1, idx))));
        }
        return;
    }
    // all slots busy: drop the hit
}

// The detector fires a few ms AFTER the real onset. Walk back through the history
// until the signal drops to ~12 % of its recent peak, so the window starts on the
// actual attack and not in the middle of it.
int64_t PreverbAudioProcessor::findOnset (int64_t detectPos) const
{
    const float* hl = history.getReadPointer (0);
    const float* hr = history.getReadPointer (1);

    auto absAt = [&] (int k)
    {
        int idx = histWrite - k;
        while (idx < 0) idx += histSize;
        return juce::jmax (std::abs (hl[idx]), std::abs (hr[idx]));
    };

    const int maxBack = static_cast<int> (0.030 * sampleRate);

    float ref = 0.f;
    for (int k = 0; k < 128; ++k) ref = juce::jmax (ref, absAt (k));

    const float lim = 0.12f * ref;
    int onsetBack = 0, quiet = 0;

    for (int k = 0; k < maxBack; ++k)
    {
        if (absAt (k) > lim) { onsetBack = k; quiet = 0; }
        else if (++quiet >= 48) break;
    }
    return detectPos - onsetBack;
}

void PreverbAudioProcessor::captureAdd (int64_t rel, float v) noexcept
{
    if (rel < 0 || rel >= capture.window) return;
    const int bin = juce::jmin (ScopeSnapshot::kBins - 1,
                                static_cast<int> ((static_cast<double> (rel) * ScopeSnapshot::kBins) / capture.window));
    auto& pk = capture.peaks[static_cast<size_t> (bin)];
    if (v > pk) { pk = v; captureDirty = true; }
}

void PreverbAudioProcessor::publishCapture()
{
    const juce::SpinLock::ScopedTryLockType sl (captureLock);
    if (! sl.isLocked()) return;                       // try again next block

    captureShared.peaks         = capture.peaks;
    captureShared.active        = captureValid;
    captureShared.windowSeconds = static_cast<float> (capture.window / sampleRate);
    captureShared.progress      = captureValid
        ? juce::jlimit (0.f, 1.f, static_cast<float> (inputPos - capture.t0) / static_cast<float> (capture.window))
        : 0.f;
    captureDirty = captureValid && captureShared.progress < 1.f;   // keep publishing until full
}

bool PreverbAudioProcessor::getCaptureSnapshot (CaptureSnapshot& out) const
{
    const juce::SpinLock::ScopedLockType sl (captureLock);
    out = captureShared;
    return out.active;
}

// =============================================================
//  SCHEDULING
// =============================================================
// Overlap-add a swell into the output ring so that its LAST sample lands just
// before absolute position hitOutAbs. Anything that would already be in the past
// (earliestAbs) is skipped; if that cuts into the swell, the new start fades in.
// Cutting the head costs the least: it is the quietest part of the swell.
void PreverbAudioProcessor::placeSwell (const float* l, const float* r, int len,
                                        int64_t hitOutAbs, int64_t earliestAbs)
{
    const int64_t startAbs = hitOutAbs - len;
    int64_t from = juce::jmax (startAbs, earliestAbs);
    from = juce::jmax (from, hitOutAbs - (accumSize - 2));        // never wrap onto ourselves
    if (from >= hitOutAbs) return;

    const int skip = static_cast<int> (from - startAbs);
    const int n    = len - skip;
    const int fade = skip > 0 ? juce::jmin (n, static_cast<int> (0.010 * sampleRate)) : 0;

    int idx = static_cast<int> ((accumRead + (from - inputPos)) % accumSize);
    float* aL = accum.getWritePointer (0);
    float* aR = accum.getWritePointer (1);

    for (int j = 0; j < n; ++j)
    {
        const float w = j < fade
            ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * static_cast<float> (j) / static_cast<float> (fade))
            : 1.f;

        aL[idx] += l[skip + j] * w;
        aR[idx] += r[skip + j] * w;
        if (++idx >= accumSize) idx = 0;
    }

    lastSwellStart = from;
    lastHitOut     = hitOutAbs;
}

bool PreverbAudioProcessor::placeFromStore (const SwellStore& store, int64_t hitOutAbs)
{
    return store.tryVisit ([&] (const float* l, const float* r, int n, double sr)
    {
        if (std::abs (sr - sampleRate) < 0.5)
            placeSwell (l, r, n, hitOutAbs, inputPos);
    });
}

// "Audition": play the latest swell right now, without waiting for a hit.
bool PreverbAudioProcessor::playStoreNow (const SwellStore& store)
{
    return store.tryVisit ([&] (const float* l, const float* r, int n, double sr)
    {
        if (std::abs (sr - sampleRate) < 0.5)
            placeSwell (l, r, n, inputPos + n, inputPos);
    });
}

int PreverbAudioProcessor::histIndexForAbs (int64_t absPos) const
{
    const int64_t back = inputPos - absPos;
    int idx = static_cast<int> ((static_cast<int64_t> (histWrite) - back) % histSize);
    if (idx < 0) idx += histSize;
    return idx;
}

// =============================================================
//  LIVE RENDER JOBS
// =============================================================
void PreverbAudioProcessor::startDueJobs()
{
    for (auto& h : pending)
    {
        if (! h.active || h.fireAt > inputPos) continue;
        h.active = false;

        if (inputPos - h.t0 >= histSize - 2) continue;         // window already scrolled out

        Job* job = nullptr;
        for (auto& j : jobs) if (! j.busy) { job = &j; break; }
        if (job == nullptr) continue;                          // all renderers busy: drop

        SwellSettings s;
        s.sampleRate    = sampleRate;
        s.windowSamples = h.window;
        s.swellSamples  = h.swell;
        s.gapSamples    = h.gap;
        s.size          = h.size;
        s.damping       = h.damping;
        s.diffusion     = h.diffusion;

        if (! job->renderer.beginFromRing (s, history.getReadPointer (0), history.getReadPointer (1),
                                           histSize, histIndexForAbs (h.t0)))
            continue;

        if (job->renderer.getSourcePeak() < 0.0005f)           // silent window: nothing to swell
        {
            job->renderer.cancel();
            continue;
        }

        job->busy = true;
        job->delivered = job->publishPending = false;
        job->hitOut = h.hitOut;

        // Spread the FDN work over the time left before the swell has to start
        const int64_t swellStart = h.hitOut - h.swell - h.gap;
        const double  slack = static_cast<double> (swellStart - inputPos - maxBlock);
        job->renderSpan = activeLatency > 0 ? juce::jlimit (0.10 * sampleRate, 0.5 * sampleRate, slack)
                                            : 0.25 * sampleRate;
    }
}

void PreverbAudioProcessor::advanceJobs (int blockSamples)
{
    for (auto& job : jobs)
    {
        if (! job.busy) continue;
        auto& r = job.renderer;

        if (r.isRunning())
        {
            // Time-sliced: a bounded amount of reverb per block, so no block ever
            // takes a CPU spike. In an offline bounce this is simply deterministic.
            const double perSample = static_cast<double> (r.getTotalSteps()) / juce::jmax (1.0, job.renderSpan);
            r.step (juce::jmax (64, static_cast<int> (std::ceil (perSample * blockSamples))));
        }

        if (r.isFinished() && ! job.delivered)
        {
            if (activeLatency > 0)       // print mode only renders for display / export
                placeSwell (r.swellL(), r.swellR(), r.swellLength(), job.hitOut, inputPos);
            job.delivered = job.publishPending = true;

            const float scale = 1.f / juce::jmax (r.getSourcePeak(), 1.0e-9f);
            ScopeMath::computePeaks (r.windowL(), r.windowR(), r.windowLength(),
                                     job.snap.source.data(), ScopeSnapshot::kBins, scale);
            ScopeMath::computePeaks (r.swellL(), r.swellR(), r.swellLength(),
                                     job.snap.swell.data(), ScopeSnapshot::kBins, scale);

            const float sr = static_cast<float> (sampleRate);
            job.snap.windowSeconds = static_cast<float> (r.windowLength()) / sr;
            job.snap.swellSeconds  = static_cast<float> (r.getSettings().swellSamples) / sr;
            job.snap.gapSeconds    = static_cast<float> (r.getSettings().gapSamples) / sr;
        }

        if (job.delivered && job.publishPending)
            if (liveStore.tryPublish (r.swellL(), r.swellR(), r.swellLength(), sampleRate, job.snap))
                job.publishPending = false;                    // else: retry next block

        if (job.delivered && ! job.publishPending)
            job.busy = false;
    }
}

void PreverbAudioProcessor::updatePhaseTelemetry()
{
    const int64_t now = inputPos;
    const float sr = static_cast<float> (sampleRate);

    Phase ph = Phase::Listening;
    float progress = 0.f, toHit = 0.f;

    const Hit* newest = nullptr;
    for (const auto& h : pending)
        if (h.active && (newest == nullptr || h.t0 > newest->t0)) newest = &h;

    float renderProgress = -1.f;
    for (const auto& j : jobs)
        if (j.busy && j.renderer.isRunning())
            renderProgress = juce::jmax (renderProgress, j.renderer.getProgress());

    if (newest != nullptr)
    {
        ph = Phase::Capturing;
        progress = juce::jlimit (0.f, 1.f, static_cast<float> (now - newest->t0) / static_cast<float> (newest->window));
    }
    else if (renderProgress >= 0.f)
    {
        ph = Phase::Rendering;
        progress = renderProgress;
    }
    else if (lastHitOut > now)
    {
        ph = now < lastSwellStart ? Phase::Scheduled : Phase::Swelling;
        toHit = static_cast<float> (lastHitOut - now) / sr;
        progress = juce::jlimit (0.f, 1.f, static_cast<float> (now - lastSwellStart)
                                              / static_cast<float> (juce::jmax<int64_t> (1, lastHitOut - lastSwellStart)));
    }

    if (ph == Phase::Capturing || ph == Phase::Rendering)
        toHit = lastHitOut > now ? static_cast<float> (lastHitOut - now) / sr : 0.f;

    telemetry.phase.store (static_cast<int> (ph));
    telemetry.phaseProgress.store (progress);
    telemetry.secondsToHit.store (toHit);
    telemetry.swellLeadSec.store (static_cast<float> (lastHitOut - lastSwellStart) / sr);
}

// =============================================================
//  FILE MODE — worker thread
//  Loads, trims, resamples and renders the dropped file. Changing any
//  relevant knob re-renders (debounced), so the picture and the sound
//  follow the knobs live while you drag them.
// =============================================================
void PreverbAudioProcessor::workerLoop()
{
    uint32_t handled = 0;

    while (! juce::Thread::currentThreadShouldExit())
    {
        worker->wait (40);
        const uint32_t c = fileRequestCounter.load();
        if (c == handled) continue;

        worker->wait (40);                                  // debounce: let a knob drag settle
        if (juce::Thread::currentThreadShouldExit()) break;
        if (fileRequestCounter.load() != c) continue;       // still changing

        handled = c;
        workerRender (c);
    }
}

double PreverbAudioProcessor::effectiveBpmForWorker() const
{
    const double h = hostBpmLast.load();
    return (raw.useHost->load() > 0.5f && h > 20.0) ? h : static_cast<double> (raw.bpm->load());
}

bool PreverbAudioProcessor::workerLoadRaw (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples < 16 || reader->numChannels < 1 || reader->sampleRate < 8000.0)
        return false;

    const int n = static_cast<int> (juce::jmin<int64_t> (reader->lengthInSamples,
                                    static_cast<int64_t> (std::floor (Arch::MAX_FILE_SECONDS * reader->sampleRate))));
    const int readCh = static_cast<int> (juce::jmin (2u, reader->numChannels));

    juce::AudioBuffer<float> tmp (readCh, n);
    if (! reader->read (&tmp, 0, n, 0, true, readCh > 1))
        return false;

    // Trim leading silence so "the hit" is the real start of the sound
    int first = 0;
    for (; first < n; ++first)
    {
        float a = std::abs (tmp.getSample (0, first));
        if (readCh > 1) a = juce::jmax (a, std::abs (tmp.getSample (1, first)));
        if (a > 0.002f) break;
    }
    if (first >= n) return false;                           // silent file

    first = juce::jmax (0, first - static_cast<int> (0.002 * reader->sampleRate));
    const int len = n - first;

    rawSource.setSize (2, len);
    rawSource.copyFrom (0, 0, tmp, 0, first, len);
    rawSource.copyFrom (1, 0, tmp, readCh > 1 ? 1 : 0, first, len);
    rawRate = reader->sampleRate;
    return true;
}

void PreverbAudioProcessor::workerRender (uint32_t requestId)
{
    juce::ScopedNoDenormals noDenormals;

    // 1. (Re)load if asked to
    juce::File toLoad;
    bool reload = false;
    {
        const juce::ScopedLock sl (fileLock);
        reload = reloadRequested;
        toLoad = pendingFile;
        reloadRequested = false;
    }

    if (reload)
    {
        rawSource.setSize (0, 0);          rawRate = 0.0;
        hostRateSource.setSize (0, 0);     hostRateSourceRate = 0.0;

        if (toLoad == juce::File())
        {
            fileStore.clear();
            telemetry.fileState.store (static_cast<int> (FileState::Empty));
            return;
        }

        telemetry.fileState.store (static_cast<int> (FileState::Loading));
        if (! workerLoadRaw (toLoad))
        {
            fileStore.clear();
            telemetry.fileState.store (static_cast<int> (FileState::Error));
            return;
        }
    }

    if (rawSource.getNumSamples() == 0) return;             // nothing loaded

    // 2. Resample to the host rate if needed
    const double sr = hostSampleRate.load();
    if (std::abs (hostRateSourceRate - sr) > 0.5)
    {
        if (std::abs (rawRate - sr) < 0.5)
        {
            hostRateSource = rawSource;
        }
        else
        {
            const double ratio  = rawRate / sr;
            const int    outLen = juce::jmax (16, static_cast<int> (std::floor ((rawSource.getNumSamples() - 8) / ratio)));
            hostRateSource.setSize (2, outLen);

            for (int ch = 0; ch < 2; ++ch)
            {
                juce::LagrangeInterpolator interp;
                interp.process (ratio, rawSource.getReadPointer (ch), hostRateSource.getWritePointer (ch), outLen);
            }
        }
        hostRateSourceRate = sr;
    }

    // 3. Work out the swell for the current knobs
    telemetry.fileState.store (static_cast<int> (FileState::Rendering));
    telemetry.fileProgress.store (0.f);

    const double secPerBeat = 60.0 / effectiveBpmForWorker();
    const int fileLen = hostRateSource.getNumSamples();

    int warn = 0;
    int W = static_cast<int> (std::lround (raw.window->load() * secPerBeat * sr));
    if (W > fileLen) { W = fileLen; warn |= WarnFileShort; }
    W = juce::jmax (W, 1);

    const int S = juce::jlimit (static_cast<int> (std::lround (Arch::MIN_SWELL_SECONDS * sr)),
                                static_cast<int> (std::lround (Arch::MAX_SWELL_SECONDS * sr)),
                                static_cast<int> (std::lround (raw.swell->load() * secPerBeat * sr)));
    const int G = juce::jlimit (0, static_cast<int> (std::lround (Arch::MAX_GAP_SECONDS * sr)),
                                static_cast<int> (std::lround (raw.gap->load() * 0.001 * sr)));

    SwellSettings s;
    s.sampleRate = sr;  s.windowSamples = W;  s.swellSamples = S;  s.gapSamples = G;
    s.size = raw.size->load();  s.damping = raw.damping->load();  s.diffusion = raw.diffusion->load();

    SwellRenderer renderer;
    renderer.prepare (sr, W + S + G + 16);

    if (! renderer.begin (s, hostRateSource.getReadPointer (0), hostRateSource.getReadPointer (1), fileLen))
    {
        telemetry.fileState.store (static_cast<int> (FileState::Error));
        return;
    }

    while (renderer.isRunning())
    {
        renderer.step (1 << 14);
        telemetry.fileProgress.store (renderer.getProgress());

        if (juce::Thread::currentThreadShouldExit() || fileRequestCounter.load() != requestId)
            return;                                         // a newer request supersedes this one
    }

    // 4. Publish for playback, display and export
    auto snap = std::make_unique<ScopeSnapshot>();
    const float scale = 1.f / juce::jmax (renderer.getSourcePeak(), 1.0e-9f);
    ScopeMath::computePeaks (renderer.windowL(), renderer.windowR(), renderer.windowLength(),
                             snap->source.data(), ScopeSnapshot::kBins, scale);
    ScopeMath::computePeaks (renderer.swellL(), renderer.swellR(), renderer.swellLength(),
                             snap->swell.data(), ScopeSnapshot::kBins, scale);
    snap->windowSeconds = static_cast<float> (W / sr);
    snap->swellSeconds  = static_cast<float> (S / sr);
    snap->gapSeconds    = static_cast<float> (G / sr);

    workerWarn.store (warn);
    fileStore.publish (renderer.swellL(), renderer.swellR(), renderer.swellLength(), sr, *snap);
    telemetry.fileState.store (static_cast<int> (FileState::Ready));
}

// =============================================================
//  FILE MODE — message-thread API
// =============================================================
bool PreverbAudioProcessor::canLoadFile (const juce::File& f) const
{
    return f.existsAsFile() && formatManager.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

void PreverbAudioProcessor::loadFile (const juce::File& f)
{
    {
        const juce::ScopedLock sl (fileLock);
        pendingFile     = f;
        reloadRequested = true;
        currentFilePath = f.getFullPathName();
        currentFileName = f.getFileName();
    }
    telemetry.fileState.store (static_cast<int> (FileState::Loading));
    fileStore.clear();
    ++fileRequestCounter;
}

void PreverbAudioProcessor::clearFile()
{
    {
        const juce::ScopedLock sl (fileLock);
        pendingFile     = juce::File();
        reloadRequested = true;
        currentFilePath = {};
        currentFileName = {};
    }
    fileStore.clear();
    telemetry.fileState.store (static_cast<int> (FileState::Empty));
    ++fileRequestCounter;
}

juce::String PreverbAudioProcessor::getFileDisplayName() const
{
    const juce::ScopedLock sl (fileLock);
    return currentFileName;
}

bool PreverbAudioProcessor::isFileSource() const { return raw.source->load() > 0.5f; }

bool PreverbAudioProcessor::hasSwellToExport() const
{
    return (isFileSource() ? fileStore : liveStore).getSnapshot().valid;
}

// =============================================================
//  EXPORT
// =============================================================
bool PreverbAudioProcessor::writeSwellWav (const juce::File& dest) const
{
    juce::AudioBuffer<float> buf;
    double sr = 0.0;
    if (! (isFileSource() ? fileStore : liveStore).copyAudio (buf, sr))
        return false;

    buf.applyGain (juce::Decibels::decibelsToGain (raw.swellDb->load(), kMinusInfDb));   // "as heard"

    dest.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (dest.createOutputStream());
    if (stream == nullptr) return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr) return false;

    stream.release();                                           // the writer owns the stream now
    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

juce::File PreverbAudioProcessor::getExportFolder() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("Preverb").getChildFile ("Swells");

    if (! dir.createDirectory().wasOk() || ! dir.isDirectory())
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Preverb");

    dir.createDirectory();
    return dir;
}

// The exported file ENDS exactly at the hit: line its right edge up with the
// downbeat / vocal start on the timeline and it is in the right place.
//
// Files live in Documents/Preverb/Swells (a DAW keeps a REFERENCE to a dropped sample,
// so it must not vanish like a temp file would), every new swell gets a fresh name
// (a DAW may still have the previous file open, and overwriting it fails), and the
// same swell dragged twice reuses the same file.
juce::File PreverbAudioProcessor::writeSwellForDrag() const
{
    const bool fromFile = isFileSource();
    const auto serial   = (fromFile ? fileStore : liveStore).getSnapshot().serial;
    const float db      = raw.swellDb->load();

    {
        const juce::ScopedLock sl (exportLock);
        if (lastExportFile.existsAsFile() && lastExportSerial == serial
            && lastExportWasFile == fromFile && std::abs (lastExportDb - db) < 0.001f)
            return lastExportFile;
    }

    const auto beatsText = juce::String (raw.swell->load(), 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    const auto base = "Preverb_Swell_" + beatsText.replaceCharacter ('.', 'p') + "beats_"
                    + juce::String (juce::roundToInt (telemetry.bpm.load())) + "bpm_"
                    + juce::Time::getCurrentTime().formatted ("%H%M%S");

    const auto f = getExportFolder().getNonexistentChildFile (base, ".wav", false);
    if (! writeSwellWav (f)) return {};

    const juce::ScopedLock sl (exportLock);
    lastExportFile = f;  lastExportSerial = serial;  lastExportWasFile = fromFile;  lastExportDb = db;
    return f;
}

void PreverbAudioProcessor::addEditorLink (EditorLink* e)
{
    const juce::ScopedLock sl (editorLinkLock);
    editorLinks.push_back (e);
}

void PreverbAudioProcessor::removeEditorLink (EditorLink* e)
{
    const juce::ScopedLock sl (editorLinkLock);
    editorLinks.erase (std::remove (editorLinks.begin(), editorLinks.end(), e), editorLinks.end());
}

// =============================================================
//  STATE
// =============================================================
void PreverbAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    {
        const juce::ScopedLock sl (fileLock);
        state.setProperty ("filePath", currentFilePath, nullptr);
    }

    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void PreverbAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xml));

    const juce::String path = xml->getStringAttribute ("filePath");
    if (path.isEmpty()) return;

    const juce::File f (path);
    if (canLoadFile (f))
    {
        loadFile (f);
    }
    else
    {
        // File moved or deleted since the project was saved: keep the name, show an error.
        {
            const juce::ScopedLock sl (fileLock);
            currentFilePath = path;
            currentFileName = f.getFileName();
        }
        telemetry.fileState.store (static_cast<int> (FileState::Error));
    }
}

// =============================================================
//  FACTORY
// =============================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PreverbAudioProcessor();
}