#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PreverbLookAndFeel.h"
#include "SwellScope.h"

// =============================================================
//  Small reusable widgets
// =============================================================

// Two (or more) option switch bound to a choice parameter.
class SegmentedChoice : public juce::Component, public juce::SettableTooltipClient
{
public:
    SegmentedChoice (juce::RangedAudioParameter& param, juce::StringArray labels)
        : names (std::move (labels))
    {
        attachment = std::make_unique<juce::ParameterAttachment> (param,
                        [this] (float v) { selected = juce::roundToInt (v); repaint(); }, nullptr);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        attachment->sendInitialUpdate();
    }

    // Must be called if the parameter is about to be destroyed before this component.
    void detach() { attachment.reset(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (Palette::bg);      g.fillRoundedRectangle (r, 8.f);
        g.setColour (Palette::border);  g.drawRoundedRectangle (r, 8.f, 1.f);

        const int n = names.size();
        const float w = r.getWidth() / static_cast<float> (n);
        for (int i = 0; i < n; ++i)
        {
            const auto seg = juce::Rectangle<float> (r.getX() + w * static_cast<float> (i), r.getY(), w, r.getHeight()).reduced (3.f);
            if (i == selected)
            {
                g.setColour (isEnabled() ? Palette::amber : Palette::textDim);
                g.fillRoundedRectangle (seg, 6.f);
            }
            g.setColour (i == selected ? Palette::bg : Palette::textDim);
            g.setFont (makeFont (12.f, true));
            g.drawText (names[i], seg, juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int n   = names.size();
        const int idx = juce::jlimit (0, n - 1, static_cast<int> (e.position.x / (static_cast<float> (getWidth()) / static_cast<float> (n))));
        if (attachment != nullptr)
            attachment->setValueAsCompleteGesture (static_cast<float> (idx));
    }

private:
    juce::StringArray names;
    int selected = 0;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

// Caption on top, knob in the middle, value text below.
struct LabelledKnob
{
    juce::Slider slider;
    juce::Label  caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    void setup (juce::AudioProcessorValueTreeState& apvts, const char* paramID, const juce::String& text,
                juce::Component& parent, const juce::String& tooltip = {})
    {
        parent.addAndMakeVisible (slider);       // first: the text box must inherit the parent's look and feel
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 16);
        slider.setTooltip (tooltip);

        caption.setText (text.toUpperCase(), juce::dontSendNotification);
        caption.setFont (makeFont (10.f, true));
        caption.setColour (juce::Label::textColourId, Palette::textDim);
        caption.setJustificationType (juce::Justification::centred);
        caption.setInterceptsMouseClicks (false, false);
        parent.addAndMakeVisible (caption);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, paramID, slider);
    }

    void setBounds (juce::Rectangle<int> r)
    {
        caption.setBounds (r.removeFromTop (14));
        slider.setBounds (r);
    }

    // The attachment installs text callbacks that capture the parameter; both must go
    // before the parameter is destroyed.
    void detach()
    {
        attachment.reset();
        slider.textFromValueFunction = nullptr;
        slider.valueFromTextFunction = nullptr;
        slider.setEnabled (false);
    }
};

// A thin horizontal meter with a caption (input level, onset detector).
class ActivityBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    ActivityBar (juce::String cap, juce::Colour c) : caption (std::move (cap)), colour (c) {}

    void setValue (float v, bool alert = false)
    {
        v = juce::jlimit (0.f, 1.f, v);
        if (std::abs (v - value) > 0.004f || alert != alerting) { value = v; alerting = alert; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Palette::textDim);
        g.setFont (makeFont (10.f, true));
        g.drawText (caption, r.removeFromLeft (46.f), juce::Justification::centredLeft, false);

        auto bar = r.withSizeKeepingCentre (r.getWidth(), 6.f);
        g.setColour (Palette::bg);
        g.fillRoundedRectangle (bar, 3.f);
        g.setColour (alerting ? Palette::pink : (isEnabled() ? colour : Palette::textDim.withAlpha (0.4f)));
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (0.f, bar.getWidth() * value)), 3.f);
    }

private:
    juce::String caption;
    juce::Colour colour;
    float value = 0.f;
    bool  alerting = false;
};

// Grab it and drag it onto the DAW timeline: the exported swell WAV ENDS at the hit.
class SwellDragHandle : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SwellDragHandle (PreverbAudioProcessor& p) : proc (&p)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        setTooltip ("Drag this into your DAW's timeline. The WAV ends exactly at the hit, so line up its right edge with the downbeat or vocal start.");
    }

    void setActive (bool a) { if (a != active) { active = a; repaint(); } }
    void detach()           { proc = nullptr; active = false; }

    std::function<void (const juce::File&)> onExport;     // called with the file (or an empty File on failure)

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (active ? Palette::amber.withAlpha (0.16f) : Palette::panelHi);
        g.fillRoundedRectangle (r, 7.f);
        g.setColour (active ? Palette::amber : Palette::border);
        g.drawRoundedRectangle (r, 7.f, 1.f);

        // grip dots
        g.setColour (active ? Palette::amber : Palette::textDim);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillEllipse (r.getX() + 12.f + static_cast<float> (col) * 6.f,
                               r.getCentreY() - 8.f + static_cast<float> (row) * 6.f, 3.f, 3.f);

        g.setColour (active ? Palette::text : Palette::textDim);
        g.setFont (makeFont (12.f, true));
        g.drawText ("DRAG SWELL TO DAW", r.withTrimmedLeft (34.f), juce::Justification::centredLeft, false);
    }

    void mouseDown (const juce::MouseEvent&) override { started = false; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (started || ! active || proc == nullptr || e.getDistanceFromDragStart() < 6) return;
        started = true;

        const auto f = proc->writeSwellForDrag();
        if (onExport) onExport (f);

        if (f.existsAsFile())
            juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
    }

private:
    PreverbAudioProcessor* proc;
    bool active = false, started = false;
};

// =============================================================
//  PLUGIN EDITOR
// =============================================================
class PreverbAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    public juce::FileDragAndDropTarget,
                                    public PreverbAudioProcessor::EditorLink,
                                    private juce::Timer
{
public:
    explicit PreverbAudioProcessorEditor (PreverbAudioProcessor&);
    ~PreverbAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Drop an audio file anywhere on the plugin
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; }
    void fileDragExit  (const juce::StringArray&) override           { dragHover = false; }
    void filesDropped  (const juce::StringArray& files, int, int) override;

    // Host destroyed the processor while we are still alive: let go of its parameters.
    void processorIsBeingDestroyed() override;

private:
    void timerCallback() override;
    void loadFileAndSwitch (const juce::File&);
    void chooseFile();
    void saveSwell();
    void fitLookahead();
    void setStatus (const juce::String& text, int milliseconds = 6000);
    void drawPanel (juce::Graphics&, juce::Rectangle<int>, const juce::String& title) const;

    PreverbAudioProcessor* proc;       // null once the processor has been destroyed
    PreverbLookAndFeel     laf;
    juce::TooltipWindow    tooltips { this, 600 };

    // Header
    juce::Label title, subtitle, bpmChip, latencyChip;
    juce::ToggleButton lookaheadToggle { "Lookahead" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lookaheadOnAttachment;
    SegmentedChoice sourceSwitch;

    // Display
    SwellScope scope;

    // Trigger panel
    SegmentedChoice detectSwitch;
    LabelledKnob    sensitivityKnob;
    juce::TextButton triggerButton { "Trigger" }, auditionButton { "Audition" };
    ActivityBar onsetBar { "ONSET", Palette::teal };

    // Timing panel
    LabelledKnob swellKnob, windowKnob;
    juce::Label  bpmCaption { {}, "BPM" }, lookaheadCaption { {}, "LOOKAHEAD" };
    juce::Slider bpmSlider;
    juce::ToggleButton hostToggle { "Follow host tempo" };
    juce::ComboBox lookaheadBox;
    juce::TextButton fitButton { "Fit" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   bpmAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   hostAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> lookaheadAttachment;

    // Reverb panel
    LabelledKnob sizeKnob, dampingKnob, diffusionKnob, gapKnob;

    // Output panel
    LabelledKnob dryKnob, swellLevelKnob;
    ActivityBar  levelBar { "INPUT", Palette::good };

    // File / export bar
    juce::Label      fileLabel;
    juce::TextButton loadButton { "Load..." }, clearButton { "Clear" }, saveButton { "Save WAV..." }, folderButton { "Folder" };
    SwellDragHandle  dragHandle;
    std::unique_ptr<juce::FileChooser> chooser;

    // Layout rectangles (computed in resized, drawn in paint)
    juce::Rectangle<int> triggerPanel, timingPanel, reverbPanel, outputPanel, filePanel;

    juce::String statusText;
    double       statusUntil = 0.0;
    bool         dragHover = false, snapValid = false, fileNeedsPick = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreverbAudioProcessorEditor)
};