// A Windows 95/98 style LookAndFeel, drawn by hand so it matches on every OS.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace classic
{
const juce::Colour face (0xffc0c0c0), white (0xffffffff), light (0xffdfdfdf), shadow (0xff808080),
                   dark (0xff000000), navy (0xff000080), titleEnd (0xff1084d0);

// Two-pixel 3D edge: raised for buttons, sunken for fields and lists.
inline void bevel (juce::Graphics& g, juce::Rectangle<int> r, bool sunken)
{
    auto edge = [&g] (juce::Rectangle<int> a, juce::Colour topLeft, juce::Colour bottomRight)
    {
        g.setColour (topLeft);
        g.fillRect (a.getX(), a.getY(), a.getWidth() - 1, 1);
        g.fillRect (a.getX(), a.getY(), 1, a.getHeight() - 1);
        g.setColour (bottomRight);
        g.fillRect (a.getX(), a.getBottom() - 1, a.getWidth(), 1);
        g.fillRect (a.getRight() - 1, a.getY(), 1, a.getHeight());
    };
    edge (r, sunken ? shadow : white, sunken ? white : dark);
    edge (r.reduced (1), sunken ? dark : light, sunken ? light : shadow);
}

inline juce::Font font (bool bold = false)
{
    return juce::Font (juce::FontOptions (13.0f, bold ? juce::Font::bold : juce::Font::plain));
}

class Look : public juce::LookAndFeel_V2
{
public:
    Look()
    {
       #if JUCE_WINDOWS
        setDefaultSansSerifTypefaceName ("Tahoma");
       #elif JUCE_MAC
        setDefaultSansSerifTypefaceName ("Geneva");
       #endif
        using namespace juce;
        setColour (ResizableWindow::backgroundColourId, face);
        setColour (DocumentWindow::textColourId, white);
        setColour (TextButton::textColourOffId, dark);
        setColour (TextButton::textColourOnId, dark);
        setColour (ToggleButton::textColourId, dark);
        setColour (Label::textColourId, dark);
        setColour (ListBox::backgroundColourId, white);
        setColour (ListBox::textColourId, dark);
        setColour (TextEditor::backgroundColourId, white);
        setColour (TextEditor::textColourId, dark);
        setColour (TextEditor::highlightColourId, navy);
        setColour (TextEditor::highlightedTextColourId, white);
        setColour (CaretComponent::caretColourId, dark);
        setColour (ComboBox::backgroundColourId, white);
        setColour (ComboBox::textColourId, dark);
        setColour (PopupMenu::backgroundColourId, face);
        setColour (PopupMenu::textColourId, dark);
        setColour (PopupMenu::highlightedBackgroundColourId, navy);
        setColour (PopupMenu::highlightedTextColourId, white);
        setColour (ScrollBar::thumbColourId, face);
        setColour (AlertWindow::backgroundColourId, face);
        setColour (AlertWindow::textColourId, dark);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool, bool down) override
    {
        g.fillAll (face);
        bevel (g, b.getLocalBounds(), down || b.getToggleState());
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font(); }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool, bool) override
    {
        const juce::Rectangle<int> box (2, (b.getHeight() - 13) / 2, 13, 13);
        g.setColour (white);
        g.fillRect (box);
        bevel (g, box, true);
        g.setColour (dark);
        if (b.getToggleState())
            for (int i = 0; i < 7; ++i) // pixel tick
                g.fillRect (box.getX() + 3 + i, box.getY() + (i < 3 ? 5 + i : 9 - i), 1, 3);
        g.setFont (font());
        g.drawText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (20), juce::Justification::centredLeft);
    }

    void fillTextEditorBackground (juce::Graphics& g, int, int, juce::TextEditor&) override { g.fillAll (white); }
    void drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor&) override
    {
        bevel (g, { 0, 0, w, h }, true);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox&) override
    {
        g.fillAll (white);
        bevel (g, { 0, 0, w, h }, true);
        const juce::Rectangle<int> button (w - 18, 2, 16, h - 4);
        g.setColour (face);
        g.fillRect (button);
        bevel (g, button, down);
        g.setColour (dark);
        for (int i = 0; i < 4; ++i)
            g.fillRect (button.getCentreX() - 3 + i, button.getCentreY() - 1 + i, 7 - 2 * i, 1);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font(); }
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (2, 2, box.getWidth() - 20, box.getHeight() - 4);
        label.setFont (font());
    }

    juce::Font getPopupMenuFont() override { return font(); }
    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.fillAll (face);
        bevel (g, { 0, 0, w, h }, false);
    }

    // Folder tree: dotted-era +/- boxes with connecting lines.
    bool areLinesDrawnForTreeView (juce::TreeView&) override { return true; }
    int getTreeViewIndentSize (juce::TreeView&) override { return 18; }

    void drawTreeviewPlusMinusBox (juce::Graphics& g, const juce::Rectangle<float>& area, juce::Colour,
                                   bool isOpen, bool) override
    {
        const juce::Rectangle<int> box (juce::roundToInt (area.getCentreX()) - 4,
                                        juce::roundToInt (area.getCentreY()) - 4, 9, 9);
        g.setColour (white);
        g.fillRect (box);
        g.setColour (shadow);
        g.drawRect (box);
        g.setColour (dark);
        g.fillRect (box.getX() + 2, box.getY() + 4, 5, 1);
        if (! isOpen) g.fillRect (box.getX() + 4, box.getY() + 2, 1, 5);
    }

    // Dialog boxes: grey face, raised edge, navy caption.
    void drawAlertBox (juce::Graphics& g, juce::AlertWindow& window, const juce::Rectangle<int>& textArea,
                       juce::TextLayout& layout) override
    {
        g.fillAll (face);
        bevel (g, window.getLocalBounds(), false);
        g.setColour (navy);
        g.fillRect (3, 3, window.getWidth() - 6, 5);
        layout.draw (g, textArea.toFloat());
    }

    juce::Font getAlertWindowTitleFont() override { return font (true); }
    juce::Font getAlertWindowMessageFont() override { return font(); }
    juce::Font getAlertWindowFont() override { return font(); }
    int getAlertWindowButtonHeight() override { return 24; }

    int getDefaultScrollbarWidth() override { return 16; }
    int getMinimumScrollbarThumbSize (juce::ScrollBar&) override { return 16; }
    bool areScrollbarButtonsVisible() override { return true; }
    int getScrollbarButtonSize (juce::ScrollBar&) override { return 16; }

    void drawScrollbarButton (juce::Graphics& g, juce::ScrollBar&, int w, int h, int direction,
                              bool, bool, bool down) override
    {
        g.fillAll (face);
        bevel (g, { 0, 0, w, h }, down);
        juce::Path p;
        p.addTriangle (-3.5f, 2.0f, 3.5f, 2.0f, 0.0f, -2.0f); // points up
        p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi * (float) direction)
                              .translated ((float) w * 0.5f, (float) h * 0.5f));
        g.setColour (dark);
        g.fillPath (p);
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                        int thumbStart, int thumbSize, bool, bool) override
    {
        g.setColour (light);
        g.fillRect (x, y, w, h);
        if (thumbSize <= 0) return;
        const auto thumb = vertical ? juce::Rectangle<int> (x, thumbStart, w, thumbSize)
                                    : juce::Rectangle<int> (thumbStart, y, thumbSize, h);
        g.setColour (face);
        g.fillRect (thumb);
        bevel (g, thumb, false);
    }

    juce::ImageEffectFilter* getScrollbarEffect() override { return nullptr; }

    void drawTableHeaderBackground (juce::Graphics& g, juce::TableHeaderComponent& h) override
    {
        g.fillAll (face);
        bevel (g, h.getLocalBounds(), false);
    }

    void drawTableHeaderColumn (juce::Graphics& g, juce::TableHeaderComponent&, const juce::String& name, int,
                                int w, int h, bool, bool down, int flags) override
    {
        g.setColour (face);
        g.fillRect (0, 0, w, h);
        bevel (g, { 0, 0, w, h }, down);
        g.setColour (dark);
        g.setFont (font());
        g.drawText (name, 6, 0, w - 20, h, juce::Justification::centredLeft);
        const bool up = (flags & juce::TableHeaderComponent::sortedForwards) != 0;
        if (up || (flags & juce::TableHeaderComponent::sortedBackwards) != 0)
        {
            juce::Path p;
            const float cx = (float) w - 10.0f, cy = (float) h * 0.5f, s = up ? 1.0f : -1.0f;
            p.addTriangle (cx - 4, cy + 2 * s, cx + 4, cy + 2 * s, cx, cy - 3 * s);
            g.setColour (shadow);
            g.fillPath (p);
        }
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override
    {
        bevel (g, { x, y + h / 2 - 2, w, 4 }, true);
        const juce::Rectangle<int> thumb ((int) pos - 5, y + h / 2 - 10, 11, 20);
        g.setColour (face);
        g.fillRect (thumb);
        bevel (g, thumb, false);
    }

    int getSliderThumbRadius (juce::Slider&) override { return 6; }

    void drawMenuBarBackground (juce::Graphics& g, int, int, bool, juce::MenuBarComponent&) override { g.fillAll (face); }
    juce::Font getMenuBarFont (juce::MenuBarComponent&, int, const juce::String&) override { return font(); }
    void drawMenuBarItem (juce::Graphics& g, int w, int h, int, const juce::String& text, bool over, bool open,
                          bool, juce::MenuBarComponent&) override
    {
        if (over || open) { g.setColour (navy); g.fillRect (0, 0, w, h); }
        g.setColour (over || open ? white : dark);
        g.setFont (font());
        g.drawText (text, 0, 0, w, h, juce::Justification::centred);
    }

    void drawResizableWindowBorder (juce::Graphics& g, int w, int h, const juce::BorderSize<int>&,
                                    juce::ResizableWindow&) override
    {
        bevel (g, { 0, 0, w, h }, false);
    }

    void drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g, int w, int h,
                                     int, int, const juce::Image*, bool) override
    {
        const bool active = window.isActiveWindow();
        g.setGradientFill (juce::ColourGradient::horizontal (active ? navy : shadow, 0.0f,
                                                             active ? titleEnd : face, (float) w));
        g.fillRect (0, 0, w, h);
        g.setColour (white);
        g.setFont (font (true));
        g.drawText (window.getName(), 6, 0, w - 70, h, juce::Justification::centredLeft);
    }

    struct TitleButton : public juce::Button
    {
        explicit TitleButton (int t) : juce::Button ({}), type (t) {}
        void paintButton (juce::Graphics& g, bool, bool down) override
        {
            const auto r = getLocalBounds().reduced (1, 2);
            g.setColour (face);
            g.fillRect (r);
            bevel (g, r, down);
            g.setColour (dark);
            const int x = r.getCentreX() - 4, y = r.getCentreY() - 4;
            if (type == juce::DocumentWindow::minimiseButton) g.fillRect (x, y + 6, 6, 2);
            else if (type == juce::DocumentWindow::maximiseButton) { g.drawRect (x, y, 9, 8); g.fillRect (x, y, 9, 2); }
            else
            {
                g.drawLine ((float) x, (float) y, (float) x + 8, (float) y + 8, 1.7f);
                g.drawLine ((float) x + 8, (float) y, (float) x, (float) y + 8, 1.7f);
            }
        }
        int type;
    };

    juce::Button* createDocumentWindowButton (int type) override { return new TitleButton (type); }

    void positionDocumentWindowButtons (juce::DocumentWindow&, int x, int y, int w, int h,
                                        juce::Button* min, juce::Button* max, juce::Button* close, bool) override
    {
        int right = x + w - 2;
        if (close != nullptr) { close->setBounds (right - 18, y, 18, h); right -= 20; }
        if (max != nullptr)   { max->setBounds (right - 18, y, 18, h); right -= 18; }
        if (min != nullptr)   min->setBounds (right - 18, y, 18, h);
    }
};
} // namespace classic
