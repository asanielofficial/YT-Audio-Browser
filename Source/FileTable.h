// A table that selects rows the way Windows Explorer does:
// Shift extends from a fixed anchor, Ctrl toggles, Ctrl+Shift adds a range.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

class FileTable : public juce::TableListBox
{
public:
    std::function<void (int)> onCursorMoved; // after Shift+arrows, to preview that row
    std::function<void()> onRename;          // F2

    FileTable() { setMultipleSelectionEnabled (true); }

    // Call from the model's cellClicked, after the built-in selection has run.
    void rowClicked (int row, juce::ModifierKeys mods)
    {
        if (! mods.isPopupMenu() && mods.isShiftDown() && anchor >= 0)
            selectSpan (row, mods.isCommandDown());
        else if (! mods.isPopupMenu() || anchor < 0)
            anchor = row;
        cursor = row;
        if (mods.isPopupMenu() || ! mods.isShiftDown() || mods.isCommandDown())
            committed = getSelectedRows();
    }

    // After the rows have been rebuilt or selected from code.
    void resetAnchor (int row)
    {
        anchor = cursor = row;
        committed = getSelectedRows();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.isKeyCode (juce::KeyPress::F2Key)) { if (onRename) onRename(); return true; }
        if (key.isKeyCode (juce::KeyPress::backspaceKey)) return true; // must not delete files

        const int rows = getTableListBoxModel() != nullptr ? getTableListBoxModel()->getNumRows() : 0;
        const int page = juce::jmax (1, getViewport()->getHeight() / getRowHeight() - 1);
        int next = 0;
        if (key.isKeyCode (juce::KeyPress::upKey))            next = cursor - 1;
        else if (key.isKeyCode (juce::KeyPress::downKey))     next = cursor + 1;
        else if (key.isKeyCode (juce::KeyPress::pageUpKey))   next = cursor - page;
        else if (key.isKeyCode (juce::KeyPress::pageDownKey)) next = cursor + page;
        else if (key.isKeyCode (juce::KeyPress::homeKey))     next = 0;
        else if (key.isKeyCode (juce::KeyPress::endKey))      next = rows - 1;
        else
        {
            const bool used = juce::TableListBox::keyPressed (key); // Enter, Delete, Ctrl+A
            if (used) committed = getSelectedRows();
            return used;
        }
        if (rows == 0) return true;
        cursor = juce::jlimit (0, rows - 1, cursor < 0 ? 0 : next);

        if (key.getModifiers().isShiftDown() && anchor >= 0)
        {
            selectSpan (cursor, false);
            if (onCursorMoved) onCursorMoved (cursor);
        }
        else
        {
            anchor = cursor;
            selectRow (cursor);
            committed = getSelectedRows();
        }
        scrollToEnsureRowIsOnscreen (cursor);
        return true;
    }

private:
    void selectSpan (int row, bool keepOthers)
    {
        auto rows = keepOthers ? committed : juce::SparseSet<int>();
        rows.addRange ({ juce::jmin (anchor, row), juce::jmax (anchor, row) + 1 });
        setSelectedRows (rows, juce::dontSendNotification);
        repaint();
    }

    int anchor = -1, cursor = -1;
    juce::SparseSet<int> committed;
};
