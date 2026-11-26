// Best-effort metadata readers for synth preset files.
#pragma once
#include <juce_core/juce_core.h>
#include <cstring>

namespace presets
{
struct Info
{
    juce::String synth, name, details; // details: "Label: value" lines
};

inline bool isPresetFile (const juce::File& f)
{
    return f.hasFileExtension ("fxp;fxb;vstpreset;vital;serumpreset");
}

inline void addDetail (Info& info, const juce::String& label, const juce::var& v)
{
    juce::String text;
    if (auto* a = v.getArray())
    {
        juce::StringArray parts;
        for (auto& e : *a) parts.add (e.toString());
        text = parts.joinIntoString (", ");
    }
    else if (! v.isVoid()) text = v.toString();
    if (text.isNotEmpty()) info.details << label << ": " << text << "\n";
}

// Returns the first balanced {...} block in the buffer, or an empty string.
inline juce::String firstJsonObject (const char* d, size_t size)
{
    size_t start = 0;
    while (start < size && d[start] != '{') ++start;
    int depth = 0;
    bool inString = false;
    for (size_t i = start; i < size; ++i)
    {
        const char c = d[i];
        if (inString) { if (c == '\\') ++i; else if (c == '"') inString = false; }
        else if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return juce::String::fromUTF8 (d + start, (int) (i - start + 1));
    }
    return {};
}

inline Info read (const juce::File& file)
{
    Info info;
    info.name = file.getFileNameWithoutExtension();

    juce::MemoryBlock data;
    if (auto in = file.createInputStream())
        in->readIntoMemoryBlock (data, 4 * 1024 * 1024);
    auto* d = static_cast<const char*> (data.getData());
    const auto size = data.getSize();

    if (file.hasFileExtension ("fxp;fxb"))
    {
        info.synth = "VST2";
        if (size >= 28 && std::memcmp (d, "CcnK", 4) == 0)
        {
            const juce::String id (d + 16, 4);
            info.synth = id == "XfsX" ? juce::String ("Serum") : "VST2 (" + id + ")";
            addDetail (info, "Plugin ID", id);
            if (file.hasFileExtension ("fxp") && size >= 56)
            {
                const auto length = (int) (std::find (d + 28, d + 56, '\0') - (d + 28));
                if (length > 0) info.name = juce::String::fromUTF8 (d + 28, length);
            }
        }
    }
    else if (file.hasFileExtension ("vstpreset"))
    {
        info.synth = "VST3";
        if (size >= 40 && std::memcmp (d, "VST3", 4) == 0)
            addDetail (info, "Class ID", juce::String (d + 8, 32));
    }
    else if (file.hasFileExtension ("vital"))
    {
        info.synth = "Vital";
        auto json = juce::JSON::parse (juce::String::fromUTF8 (d, (int) size));
        if (json["preset_name"].toString().isNotEmpty()) info.name = json["preset_name"].toString();
        addDetail (info, "Author", json["author"]);
        addDetail (info, "Style", json["preset_style"]);
        addDetail (info, "Comments", json["comments"]);
    }
    else if (file.hasFileExtension ("serumpreset"))
    {
        info.synth = "Serum";
        auto json = juce::JSON::parse (firstJsonObject (d, size));
        if (json["presetName"].toString().isNotEmpty()) info.name = json["presetName"].toString();
        addDetail (info, "Author", json["presetAuthor"]);
        addDetail (info, "Description", json["presetDescription"]);
        addDetail (info, "Tags", json["tags"]);
    }
    return info;
}
} // namespace presets
