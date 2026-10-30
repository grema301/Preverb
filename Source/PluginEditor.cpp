#include "PluginProcessor.h"
#include "PluginEditor.h"

// =============================================================
//  CONSTRUCTOR
// =============================================================
PreverbAudioProcessorEditor::PreverbAudioProcessorEditor (PreverbAudioProcessor& p)
    : AudioProcessorEditor (p),
      proc (&p),
      sourceSwitch (*p.apvts.getParameter (ParamID::SOURCE), { "LIVE", "FILE" }),
      detectSwitch (*p.apvts.getParameter (ParamID::MODE),   { "AUTO", "MANUAL" }),
      dragHandle (p)
{
    setLookAndFeel (&laf);
    p.addEditorLink (this);

    // ── Header ────────────────────────────────────────────────
    title.setText ("PREVERB", juce::dontSendNotification);
    title.setFont (makeFont (28.f, true));
    title.setColour (juce::Label::textColourId, Palette::amber);
    addAndMakeVisible (title);

    subtitle.setText ("reverse reverb swells", juce::dontSendNotification);
    subtitle.setFont (makeFont (12.f));
    subtitle.setColour (juce::Label::textColourId, Palette::textDim);
    addAndMakeVisible (subtitle);

    for (auto* chip : { &bpmChip, &latencyChip })
    {
        chip->setJustificationType (juce::Justification::centred);
        chip->setFont (makeFont (11.f, true));
        chip->setColour (juce::Label::backgroundColourId, Palette::panel);
        chip->setColour (juce::Label::outlineColourId, Palette::border);
        chip->setColour (juce::Label::textColourId, Palette::text);
        addAndMakeVisible (*chip);
    }
    bpmChip.setTooltip ("Tempo used for beat-based lengths. HOST means it is read from your DAW.");
    latencyChip.setTooltip ("Latency this plugin reports to your DAW (plugin delay compensation). It is the Lookahead setting.");

    lookaheadToggle.setTooltip ("ON: the plugin delays your session by the Lookahead time so swells can lead into hits (needed when you bounce).\n"
                                "OFF: no delay at all. Hits are still captured and swells rendered, so you can drag them out, then your session runs normally.");
    addAndMakeVisible (lookaheadToggle);
    lookaheadOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ParamID::LOOKAHEAD_ON, lookaheadToggle);

    sourceSwitch.setTooltip ("LIVE: build the swell from the audio on this track.  FILE: build it from an audio file you drop in.");
    addAndMakeVisible (sourceSwitch);

    // ── Trigger ───────────────────────────────────────────────
    detectSwitch.setTooltip ("AUTO listens for hits in the incoming audio.  MANUAL only reacts to MIDI notes, the Trigger button or Trigger automation.");
    addAndMakeVisible (detectSwitch);

    sensitivityKnob.setup (proc->apvts, ParamID::SENSITIVITY, "Sensitivity", *this,
                           "How easily Auto mode fires. Raise it for soft sounds, lower it if it triggers on noise.");

    triggerButton.setTooltip ("Fire a hit right now (also automatable from the DAW as 'Trigger').");
    triggerButton.onClick = [this] { if (proc != nullptr) proc->requestManualTrigger(); };
    addAndMakeVisible (triggerButton);

    auditionButton.setTooltip ("Play the latest swell right away, without waiting for a hit.");
    auditionButton.onClick = [this] { if (proc != nullptr) proc->requestAudition(); };
    addAndMakeVisible (auditionButton);

    addAndMakeVisible (onsetBar);
    onsetBar.setTooltip ("Auto-detect activity. When it fills and turns pink, a hit is triggered.");

    // ── Timing ────────────────────────────────────────────────
    swellKnob.setup (proc->apvts, ParamID::SWELL_BEATS, "Swell length", *this,
                     "How long the riser is. This is also the reverb's decay time.");
    windowKnob.setup (proc->apvts, ParamID::WINDOW_BEATS, "Window", *this,
                      "How much of the sound (starting at the hit) is fed into the reverb. Longer = a fuller, more vocal swell.");

    bpmCaption.setFont (makeFont (10.f, true));
    bpmCaption.setColour (juce::Label::textColourId, Palette::textDim);
    addAndMakeVisible (bpmCaption);

    addAndMakeVisible (bpmSlider);
    bpmSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    bpmSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    bpmSlider.setTooltip ("Manual tempo, used when 'Follow host tempo' is off or your DAW doesn't report one.");
    bpmAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc->apvts, ParamID::BPM, bpmSlider);

    hostToggle.setTooltip ("Read the tempo from your DAW.");
    addAndMakeVisible (hostToggle);
    hostAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc->apvts, ParamID::USE_HOST_BPM, hostToggle);

    lookaheadCaption.setFont (makeFont (10.f, true));
    lookaheadCaption.setColour (juce::Label::textColourId, Palette::textDim);
    addAndMakeVisible (lookaheadCaption);

    for (size_t i = 0; i < Arch::LOOKAHEAD_SECONDS.size(); ++i)
        lookaheadBox.addItem (juce::String (static_cast<int> (Arch::LOOKAHEAD_SECONDS[i])) + " s", static_cast<int> (i) + 1);
    lookaheadBox.setTooltip ("Latency reported to your DAW. Window + swell must fit inside it. The bar under the display shows how."
                             " Changing it while playing restarts the delay line.");
    addAndMakeVisible (lookaheadBox);

    fitButton.setTooltip ("Pick the smallest Lookahead that fits the current Window + Swell + Gap.");
    fitButton.onClick = [this] { fitLookahead(); };
    addAndMakeVisible (fitButton);
    lookaheadAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc->apvts, ParamID::LOOKAHEAD, lookaheadBox);

    // ── Reverb ────────────────────────────────────────────────
    sizeKnob.setup      (proc->apvts, ParamID::SIZE,      "Size",      *this, "Room size: spacing of the reverb's echoes.");
    dampingKnob.setup   (proc->apvts, ParamID::DAMPING,   "Damping",   *this, "Darkens the swell: high frequencies die away faster.");
    diffusionKnob.setup (proc->apvts, ParamID::DIFFUSION, "Diffusion", *this, "Smears the sound before the reverb. Higher is smoother, lower is more distinct echoes.");
    gapKnob.setup       (proc->apvts, ParamID::GAP,       "Gap",       *this, "Silence between the end of the swell and the hit, for a breath before the downbeat.");

    // ── Output ────────────────────────────────────────────────
    dryKnob.setup       (proc->apvts, ParamID::DRY_DB,   "Dry",   *this, "Level of the original audio (delayed by the lookahead).");
    swellLevelKnob.setup (proc->apvts, ParamID::SWELL_DB, "Swell", *this, "Level of the swell. 0 dB means its peak matches the loudest part of the source.");
    addAndMakeVisible (levelBar);

    // ── File / export bar ─────────────────────────────────────
    fileLabel.setFont (makeFont (12.f));
    fileLabel.setColour (juce::Label::textColourId, Palette::textDim);
    fileLabel.setMinimumHorizontalScale (1.f);
    addAndMakeVisible (fileLabel);

    loadButton.setTooltip ("Choose an audio file to build the swell from (you can also just drop one onto the plugin).");
    loadButton.onClick = [this] { chooseFile(); };
    addAndMakeVisible (loadButton);

    clearButton.onClick = [this] { if (proc != nullptr) proc->clearFile(); };
    addAndMakeVisible (clearButton);

    saveButton.setTooltip ("Save the current swell as a 24-bit WAV that ends exactly at the hit.");
    saveButton.onClick = [this] { saveSwell(); };
    addAndMakeVisible (saveButton);

    folderButton.setTooltip ("Open the folder where exported swells are saved (Documents/Preverb/Swells).");
    folderButton.onClick = [this]
    {
        if (proc != nullptr) proc->getExportFolder().revealToUser();
    };
    addAndMakeVisible (folderButton);

    dragHandle.onExport = [this] (const juce::File& f)
    {
        if (f.existsAsFile())
            setStatus ("Exported " + f.getFileName() + "  (saved in " + f.getParentDirectory().getFullPathName() + ")", 8000);
        else
            setStatus ("Could not write the swell file. Check that Documents/Preverb is writable.", 8000);
    };
    addAndMakeVisible (dragHandle);

    scope.setTooltip ("Click to audition the latest swell (or to choose a file when none is loaded).");
    scope.onClick = [this]
    {
        if (proc == nullptr) return;
        if (fileNeedsPick)      chooseFile();
        else if (snapValid)     proc->requestAudition();
    };
    addAndMakeVisible (scope);

    setResizable (true, true);
    setResizeLimits (800, 570, 1500, 1070);
    getConstrainer()->setFixedAspectRatio (900.0 / 640.0);
    setSize (900, 640);

    startTimerHz (30);
}

PreverbAudioProcessorEditor::~PreverbAudioProcessorEditor()
{
    stopTimer();
    if (proc != nullptr)
        proc->removeEditorLink (this);
    setLookAndFeel (nullptr);
}

// FL Studio can destroy the processor BEFORE the editor (switch a mixer slot off while its
// window is open). Parameter attachments unregister from their parameter when they are
// destroyed, so they must all go while the parameters still exist: that is right now.
void PreverbAudioProcessorEditor::processorIsBeingDestroyed()
{
    stopTimer();
    chooser.reset();

    sensitivityKnob.detach();  swellKnob.detach();     windowKnob.detach();
    sizeKnob.detach();         dampingKnob.detach();   diffusionKnob.detach();
    gapKnob.detach();          dryKnob.detach();       swellLevelKnob.detach();

    sourceSwitch.detach();     detectSwitch.detach();  dragHandle.detach();

    bpmAttachment.reset();     hostAttachment.reset(); lookaheadAttachment.reset();
    lookaheadOnAttachment.reset();
    bpmSlider.textFromValueFunction = nullptr;
    bpmSlider.valueFromTextFunction = nullptr;

    proc = nullptr;
    setEnabled (false);
}

// =============================================================
//  PAINT
// =============================================================
void PreverbAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Palette::bg);

    g.setGradientFill (juce::ColourGradient (Palette::amber.withAlpha (0.07f), 0.f, 0.f,
                                             juce::Colours::transparentBlack, 0.f, 150.f, false));
    g.fillRect (0, 0, getWidth(), 150);

    drawPanel (g, triggerPanel, "TRIGGER");
    drawPanel (g, timingPanel,  "TIMING");
    drawPanel (g, reverbPanel,  "REVERB");
    drawPanel (g, outputPanel,  "OUTPUT");
    drawPanel (g, filePanel,    {});
}

void PreverbAudioProcessorEditor::drawPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& titleText) const
{
    const auto f = r.toFloat();
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (f, 10.f);
    g.setColour (Palette::border);
    g.drawRoundedRectangle (f.reduced (0.5f), 10.f, 1.f);

    if (titleText.isNotEmpty())
    {
        g.setColour (Palette::amber);
        g.fillRoundedRectangle (f.getX() + 12.f, f.getY() + 11.f, 3.f, 11.f, 1.5f);
        g.setColour (Palette::textDim);
        g.setFont (makeFont (10.f, true));
        g.drawText (titleText, juce::Rectangle<float> (f.getX() + 20.f, f.getY() + 8.f, 120.f, 16.f),
                    juce::Justification::centredLeft, false);
    }
}

// =============================================================
//  LAYOUT
// =============================================================
void PreverbAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (14);

    // Header
    {
        auto header = area.removeFromTop (46);
        auto left = header.removeFromLeft (220);
        title.setBounds (left.removeFromTop (30));
        subtitle.setBounds (left);

        sourceSwitch.setBounds (header.removeFromRight (150).withSizeKeepingCentre (150, 32));
        header.removeFromRight (14);
        lookaheadToggle.setBounds (header.removeFromRight (104).withSizeKeepingCentre (104, 24));
        header.removeFromRight (8);
        latencyChip.setBounds (header.removeFromRight (140).withSizeKeepingCentre (140, 24));
        header.removeFromRight (8);
        bpmChip.setBounds (header.removeFromRight (170).withSizeKeepingCentre (170, 24));
    }
    area.removeFromTop (8);

    filePanel = area.removeFromBottom (46);
    area.removeFromBottom (8);

    const int controlsH = juce::jlimit (190, 290, area.getHeight() * 44 / 100);
    auto controls = area.removeFromBottom (controlsH);
    area.removeFromBottom (8);

    scope.setBounds (area);

    // Control panels, weighted widths
    const int gap = 8;
    const int totalW = controls.getWidth() - 3 * gap;
    const int wTrig = totalW * 24 / 100, wTime = totalW * 31 / 100, wRev = totalW * 25 / 100;

    triggerPanel = controls.removeFromLeft (wTrig);  controls.removeFromLeft (gap);
    timingPanel  = controls.removeFromLeft (wTime);  controls.removeFromLeft (gap);
    reverbPanel  = controls.removeFromLeft (wRev);   controls.removeFromLeft (gap);
    outputPanel  = controls;

    auto inner = [] (juce::Rectangle<int> panel) { return panel.reduced (10).withTrimmedTop (20); };

    // ── Trigger ──────────────────────────────────────────────
    {
        auto in = inner (triggerPanel);
        detectSwitch.setBounds (in.removeFromTop (28));
        in.removeFromTop (4);
        onsetBar.setBounds (in.removeFromBottom (14));
        in.removeFromBottom (4);

        const int knobW = juce::jmin (110, in.getWidth() * 55 / 100);
        sensitivityKnob.setBounds (in.removeFromLeft (knobW));
        in.removeFromLeft (8);

        auto btns = in.withSizeKeepingCentre (in.getWidth(), juce::jmin (in.getHeight(), 72));
        triggerButton.setBounds (btns.removeFromTop (32));
        btns.removeFromTop (8);
        auditionButton.setBounds (btns.removeFromTop (32));
    }

    // ── Timing ───────────────────────────────────────────────
    {
        auto in = inner (timingPanel);
        auto row2 = in.removeFromBottom (58);
        in.removeFromBottom (2);

        const int half = in.getWidth() / 2;
        swellKnob.setBounds (in.removeFromLeft (half));
        windowKnob.setBounds (in);

        auto left = row2.removeFromLeft (row2.getWidth() * 58 / 100);
        row2.removeFromLeft (10);

        auto line1 = left.removeFromTop (24);
        bpmCaption.setBounds (line1.removeFromLeft (30));
        bpmSlider.setBounds (line1);
        hostToggle.setBounds (left.removeFromTop (24).withTrimmedTop (4));

        lookaheadCaption.setBounds (row2.removeFromTop (16));
        auto lookRow = row2.removeFromTop (26);
        fitButton.setBounds (lookRow.removeFromRight (34));
        lookRow.removeFromRight (4);
        lookaheadBox.setBounds (lookRow);
    }

    // ── Reverb: 2 x 2 ────────────────────────────────────────
    {
        auto in = inner (reverbPanel);
        auto top = in.removeFromTop (in.getHeight() / 2);
        const int half = in.getWidth() / 2;
        sizeKnob.setBounds (top.removeFromLeft (half));
        dampingKnob.setBounds (top);
        diffusionKnob.setBounds (in.removeFromLeft (half));
        gapKnob.setBounds (in);
    }

    // ── Output ───────────────────────────────────────────────
    {
        auto in = inner (outputPanel);
        levelBar.setBounds (in.removeFromBottom (14));
        in.removeFromBottom (4);
        in = in.withSizeKeepingCentre (in.getWidth(), juce::jmin (in.getHeight(), 112));
        const int half = in.getWidth() / 2;
        dryKnob.setBounds (in.removeFromLeft (half));
        swellLevelKnob.setBounds (in);
    }

    // ── File / export bar ────────────────────────────────────
    {
        auto f = filePanel.reduced (8, 7);
        auto right = f.removeFromRight (190 + 8 + 100 + 6 + 62);
        folderButton.setBounds (right.removeFromRight (62));
        right.removeFromRight (6);
        saveButton.setBounds (right.removeFromRight (100));
        right.removeFromRight (8);
        dragHandle.setBounds (right);

        f.removeFromRight (12);
        clearButton.setBounds (f.removeFromRight (64));
        f.removeFromRight (6);
        loadButton.setBounds (f.removeFromRight (80));
        f.removeFromRight (8);
        fileLabel.setBounds (f.withTrimmedLeft (6));
    }
}

// =============================================================
//  LIVE UPDATE (30 Hz): pull telemetry + snapshots, feed the display
// =============================================================
void PreverbAudioProcessorEditor::timerCallback()
{
    if (proc == nullptr) return;

    auto& t = proc->telemetry;
    const bool isFile   = proc->isFileSource();
    const bool autoMode = proc->apvts.getRawParameterValue (ParamID::MODE)->load() < 0.5f;
    const bool lookOn   = ! t.printMode.load();

    ScopeView v;
    v.fileMode      = isFile;
    v.autoMode      = autoMode;
    v.printMode     = ! lookOn;
    v.dragHover     = dragHover;
    v.phase         = static_cast<Phase> (t.phase.load());
    v.phaseProgress = t.phaseProgress.load();
    v.secondsToHit  = t.secondsToHit.load();
    v.bpm           = t.bpm.load();
    v.latencySec    = t.latencySec.load();
    v.marginSec     = t.marginSec.load();
    v.windowReq     = t.windowSecReq.load();
    v.windowEff     = t.windowSecEff.load();
    v.swellReq      = isFile ? t.swellSecEff.load() : t.swellSecReq.load();
    v.swellEff      = t.swellSecEff.load();
    v.gapSec        = t.gapSec.load();
    v.warnings      = t.warnings.load();
    v.fileState     = static_cast<FileState> (t.fileState.load());
    v.fileProgress  = t.fileProgress.load();
    v.hitCount      = t.hitCount.load();
    v.fileName      = proc->getFileDisplayName();

    const auto snap = (isFile ? proc->fileStore : proc->liveStore).getSnapshot();

    CaptureSnapshot cap;
    if (! isFile) proc->getCaptureSnapshot (cap);

    scope.setData (snap, cap, v);
    snapValid     = snap.valid;
    fileNeedsPick = isFile && (v.fileState == FileState::Empty || v.fileState == FileState::Error);

    // Header chips
    const bool fromHost = t.bpmFromHost.load();
    bpmChip.setText (juce::String (t.bpm.load(), 1) + " BPM  " + (fromHost ? "HOST" : "MANUAL"), juce::dontSendNotification);
    latencyChip.setText (lookOn ? "LATENCY  " + juce::String (t.latencySec.load(), 2) + " s" : juce::String ("NO LATENCY"),
                         juce::dontSendNotification);
    latencyChip.setColour (juce::Label::textColourId, lookOn ? Palette::text : Palette::good);

    // Controls that don't apply right now are greyed out
    sensitivityKnob.slider.setEnabled (autoMode);
    onsetBar.setEnabled (autoMode);
    bpmSlider.setEnabled (! (hostToggle.getToggleState() && fromHost));
    lookaheadBox.setEnabled (lookOn);
    fitButton.setEnabled (lookOn);

    levelBar.setValue (std::sqrt (t.inputLevel.load()));
    onsetBar.setValue (t.onsetActivity.load(), t.onsetActivity.load() >= 1.f);

    // File bar (a short status message from an export wins for a few seconds)
    const auto name = proc->getFileDisplayName();
    if (juce::Time::getMillisecondCounterHiRes() < statusUntil)
    {
        fileLabel.setText (statusText, juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, Palette::text);
    }
    else if (name.isEmpty())
    {
        fileLabel.setText ("No file loaded. Drag audio onto the display, or press Load...", juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, Palette::textDim);
    }
    else if (v.fileState == FileState::Error)
    {
        fileLabel.setText (name + "  (missing or unreadable)", juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, Palette::warn);
    }
    else
    {
        fileLabel.setText (name, juce::dontSendNotification);
        fileLabel.setColour (juce::Label::textColourId, Palette::text);
    }

    clearButton.setEnabled (name.isNotEmpty());
    saveButton.setEnabled (snap.valid);
    auditionButton.setEnabled (snap.valid);
    dragHandle.setActive (snap.valid);
}

void PreverbAudioProcessorEditor::setStatus (const juce::String& text, int milliseconds)
{
    statusText  = text;
    statusUntil = juce::Time::getMillisecondCounterHiRes() + milliseconds;
}

// Smallest Lookahead that holds window + swell + gap (+ the render margin in live mode).
void PreverbAudioProcessorEditor::fitLookahead()
{
    if (proc == nullptr) return;

    auto& t = proc->telemetry;
    const float need = proc->isFileSource()
        ? t.swellSecEff.load() + t.gapSec.load()
        : t.windowSecReq.load() + t.swellSecReq.load() + t.gapSec.load() + t.marginSec.load();

    int pick = static_cast<int> (Arch::LOOKAHEAD_SECONDS.size()) - 1;
    for (size_t i = 0; i < Arch::LOOKAHEAD_SECONDS.size(); ++i)
        if (static_cast<float> (Arch::LOOKAHEAD_SECONDS[i]) >= need + 0.01f) { pick = static_cast<int> (i); break; }

    lookaheadBox.setSelectedId (pick + 1, juce::sendNotificationSync);
}

// =============================================================
//  FILES
// =============================================================
bool PreverbAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (proc == nullptr) return false;
    for (const auto& f : files)
        if (proc->canLoadFile (juce::File (f)))
            return true;
    return false;
}

void PreverbAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    if (proc == nullptr) return;

    for (const auto& f : files)
        if (proc->canLoadFile (juce::File (f)))
        {
            loadFileAndSwitch (juce::File (f));
            break;
        }
}

void PreverbAudioProcessorEditor::loadFileAndSwitch (const juce::File& f)
{
    if (proc == nullptr) return;
    proc->loadFile (f);

    // Dropping a file means "use this file": flip the Source switch for the user.
    if (auto* prm = proc->apvts.getParameter (ParamID::SOURCE))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (1.f);
        prm->endChangeGesture();
    }
}

void PreverbAudioProcessorEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose an audio file",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg");

    juce::Component::SafePointer<PreverbAudioProcessorEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (safe != nullptr && f.existsAsFile())
                                  safe->loadFileAndSwitch (f);
                          });
}

void PreverbAudioProcessorEditor::saveSwell()
{
    chooser = std::make_unique<juce::FileChooser> ("Save swell as",
                                                   juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                                       .getChildFile ("Preverb_Swell.wav"),
                                                   "*.wav");

    juce::Component::SafePointer<PreverbAudioProcessorEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (safe == nullptr || safe->proc == nullptr || f == juce::File()) return;

                              f = f.withFileExtension ("wav");
                              safe->setStatus (safe->proc->writeSwellWav (f) ? "Saved " + f.getFullPathName()
                                                                              : "Could not save " + f.getFullPathName());
                          });
}

// =============================================================
//  FACTORY (called by PreverbAudioProcessor::createEditor)
// =============================================================
juce::AudioProcessorEditor* PreverbAudioProcessor::createEditor()
{
    return new PreverbAudioProcessorEditor (*this);
}