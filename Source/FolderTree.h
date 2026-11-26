// One folder in the sidebar tree. A node with no File is "(All folders)".
#pragma once
#include <functional>
#include <map>
#include "ClassicLook.h"

struct FolderState
{
    juce::File selected;                    // empty means all folders
    std::map<juce::String, bool> openness;  // remembered across rescans
    std::function<void()> onSelect;
};

class FolderNode : public juce::TreeViewItem
{
public:
    FolderNode (FolderState& s, const juce::File& f) : state (s), folder (f) {}

    // Returns the child for a subfolder, creating it in sorted position if needed.
    FolderNode& child (const juce::File& dir, bool openByDefault, bool sorted = true)
    {
        int index = 0;
        for (; index < getNumSubItems(); ++index)
        {
            auto* node = static_cast<FolderNode*> (getSubItem (index));
            if (node->folder == dir) return *node;
        }
        if (sorted)
            for (index = 0; index < getNumSubItems(); ++index)
                if (dir.getFileName().compareNatural (static_cast<FolderNode*> (getSubItem (index))->folder.getFileName()) < 0)
                    break;

        auto* node = new FolderNode (state, dir);
        addSubItem (node, index);
        const auto found = state.openness.find (dir.getFullPathName());
        const bool open = found != state.openness.end() ? found->second : openByDefault;
        node->setOpen (open);
        if (state.selected == dir)
            node->setSelected (true, true, juce::dontSendNotification);
        return *node;
    }

    // Adds every folder between this node's folder and the given one.
    void addPath (const juce::File& dir)
    {
        if (dir == folder || ! dir.isAChildOf (folder)) return;
        auto next = dir;
        while (next.getParentDirectory() != folder) next = next.getParentDirectory();
        child (next, false).addPath (dir);
    }

    juce::String label() const { return folder == juce::File() ? juce::String ("(All folders)") : folder.getFileName(); }
    juce::String getUniqueName() const override { return folder.getFullPathName(); }
    bool mightContainSubItems() override { return getNumSubItems() > 0; }
    int getItemHeight() const override { return 18; }
    int getItemWidth() const override
    {
        return juce::roundToInt (juce::GlyphArrangement::getStringWidth (classic::font(), label())) + 10;
    }

    void paintItem (juce::Graphics& g, int w, int h) override
    {
        const int textWidth = juce::jmin (w, getItemWidth());
        if (isSelected())
        {
            g.setColour (classic::navy);
            g.fillRect (0, 1, textWidth, h - 2);
        }
        g.setColour (isSelected() ? classic::white : classic::dark);
        g.setFont (classic::font());
        g.drawText (label(), 4, 0, w - 4, h, juce::Justification::centredLeft);
    }

    void itemOpennessChanged (bool isNowOpen) override
    {
        if (folder != juce::File()) state.openness[folder.getFullPathName()] = isNowOpen;
    }

    void itemSelectionChanged (bool isNowSelected) override
    {
        if (! isNowSelected) return;
        state.selected = folder;
        if (state.onSelect) state.onSelect();
    }

    const juce::File& getFolder() const { return folder; }

private:
    FolderState& state;
    const juce::File folder;
};
