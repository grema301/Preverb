#pragma once
#include <JuceHeader.h>
#include "SwellRenderer.h"
#include "SwellStore.h"
#include <array>
#include <atomic>

// =============================================================
//  PARAMETER IDs
//  (sensitivity / mode / windowBeats / bpm / size / damping / diffusion / predelay
//   keep their old IDs. decayTime and mix were replaced: swell length is now in
//   BEATS, and "mix" became two clear level controls, Dry and Swell.)
// =============================================================
namespace ParamID
{
    // Source & trigger
    inline constexpr const char* SOURCE       = "source";        // 0 = Live (host audio), 1 = File
    inline constexpr const char* MODE         = "mode";          // 0 = Auto detect, 1 = Manual only
    inline constexpr const char* SENSITIVITY  = "sensitivity";
    inline constexpr const char* TRIGGER      = "trigger";       // momentary, automatable

    // Timing
    inline constexpr const char* WINDOW_BEATS = "windowBeats";   // how much of the source feeds the reverb
    inline constexpr const char* SWELL_BEATS  = "swellBeats";    // length of the riser (= reverb RT60)
    inline constexpr const char* BPM          = "bpm";
    inline constexpr const char* USE_HOST_BPM = "useHostBpm";
    inline constexpr const char* LOOKAHEAD    = "lookahead";     // reported latency (PDC)
    inline constexpr const char* LOOKAHEAD_ON = "lookaheadOn";   // OFF = zero latency ("print mode")

    // Reverb character
    inline constexpr const char* SIZE         = "size";
    inline constexpr const char* DAMPING      = "damping";
    inline constexpr const char* DIFFUSION    = "diffusion";
    inline constexpr const char* GAP          = "predelay";      // ms between swell end and hit

    // Output
    inline constexpr const char* DRY_DB       = "dryDb";
    inline constexpr const char* SWELL_DB     = "swellDb";
}

// =============================================================
//  ARCHITECTURE CONSTANTS
// =============================================================
namespace Arch
{
    constexpr double MAX_LOOKAHEAD_SECONDS = 6.0;     // largest choice in the Lookahead menu
    constexpr double MAX_SWELL_SECONDS     = 12.0;    // 8 beats @ 40 BPM (file mode / export)
    constexpr double MAX_GAP_SECONDS       = 0.25;
    constexpr double ACCUM_SECONDS         = 13.0;    // output scheduling ring
    constexpr double MAX_FILE_SECONDS      = 30.0;    // longest file we keep in memory
    constexpr double RENDER_MARGIN_SECONDS = 0.25;    // time reserved between window end and swell start
    constexpr double MIN_SWELL_SECONDS     = 0.05;

    constexpr int MAX_JOBS    = 3;                    // concurrent live renders
    constexpr int MAX_PENDING = 8;                    // hits waiting for their window to fill

    inline constexpr std::array<double, 5> LOOKAHEAD_SECONDS { 1.0, 2.0, 3.0, 4.0, 6.0 };
}

// =============================================================
//  What the UI is told about the audio thread (all lock-free)
// =============================================================
enum class Phase : int { Listening = 0, Capturing, Rendering, Scheduled, Swelling };

enum Warning : int
{
    WarnWindowClamped = 1,   // live: window shortened to fit the lookahead
    WarnSwellClamped  = 2,   // live: swell shortened to fit the lookahead
    WarnNoRoom        = 4,   // live: nothing fits, raise Lookahead
    WarnFileTruncated = 8,   // file: swell + gap longer than lookahead, playback loses its quiet start
    WarnFileShort     = 16   // file: file shorter than the window
};

enum class FileState : int { Empty = 0, Loading, Rendering, Ready, Error };

struct Telemetry
{
    // Detector / meters
    std::atomic<float>    inputLevel    { 0.f };    // 0..1 peak, smoothed
    std::atomic<float>    onsetActivity { 0.f };    // 0..~2, >= 1 means "would trigger"
    std::atomic<uint32_t> hitCount      { 0 };      // increments on every trigger

    // What is the pipeline doing right now?
    std::atomic<int>      phase         { 0 };      // Phase
    std::atomic<float>    phaseProgress { 0.f };    // 0..1 within the phase
    std::atomic<float>    secondsToHit  { 0.f };    // while Scheduled / Swelling
    std::atomic<float>    swellLeadSec  { 0.f };    // length of the swell currently scheduled

    // Timing in effect
    std::atomic<float>    bpm           { 120.f };
    std::atomic<bool>     bpmFromHost   { false };
    std::atomic<float>    latencySec    { 2.f };
    std::atomic<bool>     printMode     { false };  // lookahead off: no latency, swells are rendered for export only
    std::atomic<float>    windowSecReq  { 0.f }, windowSecEff { 0.f };
    std::atomic<float>    swellSecReq   { 0.f }, swellSecEff  { 0.f };
    std::atomic<float>    gapSec        { 0.f };
    std::atomic<float>    marginSec     { 0.f };
    std::atomic<int>      warnings      { 0 };

    // File mode
    std::atomic<int>      fileState     { 0 };      // FileState
    std::atomic<float>    fileProgress  { 0.f };
};

// The window being captured RIGHT NOW (so the UI can draw it filling up)
struct CaptureSnapshot
{
    std::array<float, ScopeSnapshot::kBins> peaks {};
    float progress      = 0.f;
    float windowSeconds = 0.f;
    bool  active        = false;
};

// =============================================================
//  PLUGIN PROCESSOR
//
//  Timeline of one LIVE swell (all positions in input samples, t0 = the hit):
//
//    t0 ........... t0+W ..... t0+W+render ........ t0+L-G-S ======= t0+L-G .. t0+L
//    hit           window     render finished       swell starts      swell ends   hit is
//    detected      complete   (time-sliced)                                         HEARD
//
//  L (the reported latency) is the delay applied to the dry signal. Because the
//  host compensates for L, the dry hit is heard at t0 again, and the swell, which
//  we scheduled to END at t0+L, lands exactly on it.
// =============================================================
class PreverbAudioProcessor : public juce::AudioProcessor,
                              private juce::AsyncUpdater,
                              private juce::AudioProcessorValueTreeState::Listener
{
public:
    PreverbAudioProcessor();
    ~PreverbAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Preverb"; }
    bool acceptsMidi()  const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return static_cast<double> (telemetry.latencySec.load()) + 0.1; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ── Parameters ───────────────────────────────────────────
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ── UI API (message thread) ──────────────────────────────
    Telemetry  telemetry;
    SwellStore liveStore;      // latest swell captured from the host
    SwellStore fileStore;      // swell rendered from the dropped file

    bool getCaptureSnapshot (CaptureSnapshot& out) const;

    void requestManualTrigger() { manualTriggerPending.store (true); }
    void requestAudition()      { auditionPending.store (true); }

    void         loadFile (const juce::File& f);
    void         clearFile();
    juce::String getFileDisplayName() const;
    bool         canLoadFile (const juce::File& f) const;

    // The editor registers here so that, if the host destroys the processor FIRST
    // (FL Studio does this when a mixer slot is switched off with the window open),
    // the editor can drop its parameter attachments before the parameters disappear.
    struct EditorLink
    {
        virtual ~EditorLink() = default;
        virtual void processorIsBeingDestroyed() = 0;
    };
    void addEditorLink (EditorLink* e);
    void removeEditorLink (EditorLink* e);

    juce::File   getExportFolder() const;
    bool         isFileSource() const;
    bool         hasSwellToExport() const;
    bool         writeSwellWav (const juce::File& dest) const;   // 24-bit WAV, level = as heard
    juce::File   writeSwellForDrag() const;                      // temp file for drag-to-DAW

private:
    // ── Parameter snapshot for one block ─────────────────────
    struct Params
    {
        bool  isFile = false, isAuto = true, useHostBpm = true;
        float sensitivity = 0.5f, windowBeats = 1.f, swellBeats = 2.f, bpm = 120.f;
        float gapMs = 0.f, size = 0.5f, damping = 0.5f, diffusion = 0.5f;
        float dryDb = 0.f, swellDb = 0.f;
        bool  triggerHeld = false;
    };

    // How the requested times fit into the lookahead (in samples)
    struct Budget
    {
        int  windowReq = 0, swellReq = 0, window = 0, swell = 0, gap = 0, margin = 0;
        bool noRoom = false;
        int  warnings = 0;
    };

    struct Hit
    {
        bool    active = false;
        int64_t t0 = 0, fireAt = 0, hitOut = 0;
        int     window = 0, swell = 0, gap = 0;
        float   size = 0.5f, damping = 0.5f, diffusion = 0.5f;
    };

    struct Job
    {
        SwellRenderer renderer;
        ScopeSnapshot snap;
        bool    busy = false, delivered = false, publishPending = false;
        int64_t hitOut = 0;
        double  renderSpan = 1.0;     // samples over which the work is spread
    };

    // ── Pipeline ─────────────────────────────────────────────
    void flushPipeline();
    Params readParams() const;
    Budget computeBudget (const Params&) const;
    void   updateTransport (Params&, int numSamples);
    void   registerHit (int64_t t0, const Params&, const Budget&);
    int64_t findOnset (int64_t detectPos) const;
    void   placeSwell (const float* l, const float* r, int len, int64_t hitOutAbs, int64_t earliestAbs);
    bool   placeFromStore (const SwellStore&, int64_t hitOutAbs);
    bool   playStoreNow (const SwellStore&);
    void   startDueJobs();
    void   advanceJobs (int blockSamples);
    void   updatePhaseTelemetry();
    int    histIndexForAbs (int64_t absPos) const;
    int    lookaheadSamples (double sr) const;
    void   handleAsyncUpdate() override;
    void   parameterChanged (const juce::String&, float) override;

    // ── File worker (loads, resamples and renders the dropped file off the audio thread) ──
    class FileWorker : public juce::Thread
    {
    public:
        explicit FileWorker (PreverbAudioProcessor& o) : juce::Thread ("Preverb file render"), owner (o) {}
        void run() override { owner.workerLoop(); }
    private:
        PreverbAudioProcessor& owner;
    };

    void workerLoop();
    void workerRender (uint32_t requestId);
    bool workerLoadRaw (const juce::File&);
    double effectiveBpmForWorker() const;

    // ── State ────────────────────────────────────────────────
    // Raw parameter pointers, cached once (reading them is lock-free and audio-thread safe)
    struct Raw
    {
        std::atomic<float> *source = nullptr, *mode = nullptr, *sensitivity = nullptr, *trigger = nullptr,
                           *window = nullptr, *swell = nullptr, *bpm = nullptr, *useHost = nullptr,
                           *lookahead = nullptr, *lookaheadOn = nullptr, *size = nullptr, *damping = nullptr, *diffusion = nullptr,
                           *gap = nullptr, *dryDb = nullptr, *swellDb = nullptr;
    } raw;
    double sampleRate = 44100.0;
    int    maxBlock   = 512;
    int    renderCapacity = 0;                  // largest window + swell + gap a renderer can hold
    bool   isPrepared = false;

    int                activeLatency = 0;       // samples, audio thread
    std::atomic<int>   latencyTarget { 0 };     // handed to the message thread

    juce::AudioBuffer<float> history;           // input ring (dry delay line + window source)
    int     histSize = 0, histWrite = 0;
    juce::AudioBuffer<float> accum;             // output ring (scheduled swells, overlap-add)
    int     accumSize = 0, accumRead = 0;
    int64_t inputPos = 0;                       // absolute index of the NEXT input/output sample

    std::array<Hit, Arch::MAX_PENDING> pending;
    std::array<Job, Arch::MAX_JOBS>    jobs;
    std::array<int64_t, 8> fileRetry {};
    int     numFileRetry = 0;

    int64_t lastHitOut = 0, lastSwellStart = 0;

    // Onset detector
    float detLow = 0.f, detBand = 0.f, envFast = 0.f, envSlow = 0.f;
    float hpCoef = 0.f, lpCoef = 0.f, fastAtk = 0.f, fastRel = 0.f, slowAtk = 0.f, slowRel = 0.f;
    bool  armed = true;
    int64_t holdOffUntil = 0;
    float onsetPeak = 0.f, levelPeak = 0.f;

    // Triggers
    std::atomic<bool> manualTriggerPending { false };
    std::atomic<bool> auditionPending      { false };
    bool prevTriggerHeld = false;

    // Transport
    bool    lastPlaying = false;
    int64_t expectedHostTime = 0;
    bool    haveHostTime = false;
    int     consistentBlocks = 0;
    double  lastBpm = 0.0;
    std::atomic<double> hostBpmLast { 0.0 };

    // Capture tracker (the window currently filling)
    struct Capture
    {
        int64_t t0 = 0;
        int     window = 1;
        std::array<float, ScopeSnapshot::kBins> peaks {};
    } capture;
    mutable juce::SpinLock captureLock;
    CaptureSnapshot captureShared;
    bool captureValid = false, captureDirty = false;
    void captureAdd (int64_t rel, float v) noexcept;
    void publishCapture();

    // Output smoothing
    juce::SmoothedValue<float> drySmoothed, swellSmoothed;

    // ── File mode ────────────────────────────────────────────
    juce::AudioFormatManager formatManager;
    mutable juce::CriticalSection fileLock;
    juce::File   pendingFile;
    bool         reloadRequested = false;
    juce::String currentFilePath, currentFileName;

    juce::AudioBuffer<float> rawSource;                     // worker-owned
    double rawRate = 0.0;
    juce::AudioBuffer<float> hostRateSource;                // worker-owned
    double hostRateSourceRate = 0.0;

    std::atomic<uint32_t> fileRequestCounter { 0 };
    std::atomic<double>   hostSampleRate { 44100.0 };
    std::atomic<int>      workerWarn { 0 };
    std::unique_ptr<FileWorker> worker;

    // Editor lifetime (see EditorLink) and export cache
    juce::CriticalSection editorLinkLock;
    std::vector<EditorLink*> editorLinks;

    mutable juce::CriticalSection exportLock;
    mutable juce::File lastExportFile;
    mutable uint32_t   lastExportSerial = 0;
    mutable float      lastExportDb = 0.f;
    mutable bool       lastExportWasFile = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreverbAudioProcessor)
};