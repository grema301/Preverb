#pragma once
#include <JuceHeader.h>

// =============================================================
//  Palette — one place to re-skin the whole plugin
// =============================================================
namespace Palette
{
    inline const juce::Colour bg       { 0xFF12121B };
    inline const juce::Colour panel    { 0xFF1B1B28 };
    inline const juce::Colour panelHi  { 0xFF232334 };
    inline const juce::Colour border   { 0xFF2F2F45 };
    inline const juce::Colour text     { 0xFFE8E8F0 };
    inline const juce::Colour textDim  { 0xFF8B8BA6 };
    inline const juce::Colour amber    { 0xFFE89B5C };   // swell / accent (kept close to the original amber)
    inline const juce::Colour pink     { 0xFFFF6F91 };   // swell peak, right at the hit
    inline const juce::Colour teal     { 0xFF5FD4C8 };   // source audio
    inline const juce::Colour good     { 0xFF6BD68A };
    inline const juce::Colour warn     { 0xFFE5534B };
}

// JUCE 8 deprecated the old Font constructors; JUCE 7 doesn't have FontOptions.
// One helper keeps the code building on both.
inline juce::Font makeFont (float height, bool bold = false)
{
   #if JUCE_MAJOR_VERSION >= 8
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
   #else
    return juce::Font (height, bold ? juce::Font::bold : juce::Font::plain);
   #endif
}

// =============================================================
//  Flat dark look: arc knobs, pill toggles, soft rounded buttons
// =============================================================
class PreverbLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PreverbLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId,   Palette::bg);
        setColour (juce::Label::textColourId,                   Palette::text);
        setColour (juce::Slider::textBoxTextColourId,           Palette::text);
        setColour (juce::Slider::textBoxOutlineColourId,        juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId,     juce::Colours::transparentBlack);
        setColour (juce::Slider::rotarySliderFillColourId,      Palette::amber);
        setColour (juce::Slider::rotarySliderOutlineColourId,   Palette::border);
        setColour (juce::Slider::trackColourId,                 Palette::amber);
        setColour (juce::Slider::backgroundColourId,            Palette::border);
        setColour (juce::Slider::thumbColourId,                 Palette::text);

        setColour (juce::TextButton::buttonColourId,            Palette::panelHi);
        setColour (juce::TextButton::buttonOnColourId,          Palette::amber);
        setColour (juce::TextButton::textColourOffId,           Palette::text);
        setColour (juce::TextButton::textColourOnId,            Palette::bg);

        setColour (juce::ComboBox::backgroundColourId,          Palette::panelHi);
        setColour (juce::ComboBox::textColourId,                Palette::text);
        setColour (juce::ComboBox::outlineColourId,             Palette::border);
        setColour (juce::ComboBox::arrowColourId,               Palette::textDim);

        setColour (juce::PopupMenu::backgroundColourId,         Palette::panelHi);
        setColour (juce::PopupMenu::textColourId,               Palette::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::amber);
        setColour (juce::PopupMenu::highlightedTextColourId,    Palette::bg);

        setColour (juce::TooltipWindow::backgroundColourId,     Palette::panelHi);
        setColour (juce::TooltipWindow::textColourId,           Palette::text);
        setColour (juce::TooltipWindow::outlineColourId,        Palette::border);
    }

    // ── Fonts ────────────────────────────────────────────────
    // Labels use the font they were given. Slider text boxes (which are Labels owned
    // by a Slider) get a compact 12 px font.
    juce::Font getLabelFont (juce::Label& l) override
    {
        if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
            return makeFont (12.f);
        return l.getFont();
    }
    juce::Font getTextButtonFont (juce::TextButton&, int) override   { return makeFont (12.f, true); }
    juce::Font getComboBoxFont (juce::ComboBox&) override            { return makeFont (12.f); }
    juce::Font getPopupMenuFont() override                           { return makeFont (13.f); }

    // ── Rotary knob ──────────────────────────────────────────
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        const auto area   = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                                    static_cast<float> (width), static_cast<float> (height)).reduced (5.f);
        const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
        const auto  c      = area.getCentre();
        const float angle  = startAngle + sliderPos * (endAngle - startAngle);
        const bool  on     = s.isEnabled();

        const float arcW = juce::jmax (3.f, radius * 0.15f);
        const float arcR = radius - arcW * 0.5f;

        juce::Path track;
        track.addCentredArc (c.x, c.y, arcR, arcR, 0.f, startAngle, endAngle, true);
        g.setColour (Palette::border);
        g.strokePath (track, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto accent = on ? s.findColour (juce::Slider::rotarySliderFillColourId) : Palette::textDim.withAlpha (0.5f);

        juce::Path value;
        value.addCentredArc (c.x, c.y, arcR, arcR, 0.f, startAngle, angle, true);
        g.setColour (accent);
        g.strokePath (value, juce::PathStrokeType (arcW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Knob body
        const float bodyR = radius * 0.66f;
        g.setGradientFill (juce::ColourGradient (Palette::panelHi.brighter (0.15f), c.x, c.y - bodyR,
                                                 Palette::panel.darker (0.3f),      c.x, c.y + bodyR, false));
        g.fillEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.f, bodyR * 2.f);
        g.setColour (Palette::border);
        g.drawEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.f, bodyR * 2.f, 1.f);

        // Pointer
        const float p0 = bodyR * 0.30f, p1 = bodyR * 0.88f;
        g.setColour (on ? Palette::text : Palette::textDim);
        g.drawLine (c.x + p0 * std::sin (angle), c.y - p0 * std::cos (angle),
                    c.x + p1 * std::sin (angle), c.y - p1 * std::cos (angle), 2.2f);
    }

    // ── Pill switch ──────────────────────────────────────────
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool /*down*/) override
    {
        auto r = b.getLocalBounds().toFloat();
        const float h = 16.f, w = 30.f;
        auto pill = juce::Rectangle<float> (r.getX(), r.getCentreY() - h * 0.5f, w, h);

        const bool on = b.getToggleState();
        g.setColour (on ? Palette::amber.withAlpha (highlighted ? 1.f : 0.9f) : Palette::border);
        g.fillRoundedRectangle (pill, h * 0.5f);

        const float knob = h - 6.f;
        const float kx = on ? pill.getRight() - 3.f - knob : pill.getX() + 3.f;
        g.setColour (on ? Palette::bg : Palette::textDim);
        g.fillEllipse (kx, pill.getY() + 3.f, knob, knob);

        g.setColour (b.isEnabled() ? Palette::text : Palette::textDim);
        g.setFont (makeFont (12.f));
        g.drawText (b.getButtonText(), r.withTrimmedLeft (w + 8.f), juce::Justification::centredLeft);
    }

    // ── Buttons ──────────────────────────────────────────────
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                               bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = bg;
        if (! b.isEnabled()) c = c.withAlpha (0.4f);
        else if (down)       c = c.darker (0.2f);
        else if (highlighted) c = c.brighter (0.15f);

        g.setColour (c);
        g.fillRoundedRectangle (r, 6.f);
        g.setColour (Palette::border);
        g.drawRoundedRectangle (r, 6.f, 1.f);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.f, 0.f, static_cast<float> (w), static_cast<float> (h)).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 6.f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 6.f, 1.f);

        juce::Path arrow;
        const float cx = static_cast<float> (w) - 14.f, cy = static_cast<float> (h) * 0.5f;
        arrow.addTriangle (cx - 4.f, cy - 2.f, cx + 4.f, cy - 2.f, cx, cy + 3.f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.fillPath (arrow);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 28, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }
};