// File operations that need care to avoid losing data.
#pragma once
#include <juce_core/juce_core.h>

namespace fileactions
{
// Moves to the Recycle Bin or Trash. Never deletes permanently without the OS asking first.
bool moveToTrash (const juce::File& file);

// Renames within the same folder, keeping the extension. Returns an error message, or empty.
juce::String rename (const juce::File& file, const juce::String& newStem, juce::File& renamed);
} // namespace fileactions
