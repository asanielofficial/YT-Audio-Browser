// Background folder scanner with an on-disk cache of analysis results.
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>
#include "Analysis.h"
#include "Detect.h"
#include "Presets.h"

struct Item
{
    juce::File file;
    juce::String name, type, details;
    double seconds = 0, bpm = 0, tuning = 0; // tuning: A4 in Hz
    int key = -1;
    bool isPreset = false, bpmGuessed = false, keyGuessed = false, tuningGuessed = false;
};

class Library : private juce::Thread
{
public:
    Library() : juce::Thread ("scanner") { formats.registerBasicFormats(); }
    ~Library() override { stopThread (10000); saveCache(); }

    juce::AudioFormatManager formats;

    static juce::File dataFolder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("SampliteExplorer");
        dir.createDirectory();
        return dir;
    }

    void loadCache()
    {
        auto xml = juce::XmlDocument::parse (dataFolder().getChildFile ("cache.xml"));
        if (xml != nullptr && xml->getIntAttribute ("engine") == engineVersion) // else analyse again
            for (auto* e : xml->getChildIterator())
            {
                Item it;
                it.file = juce::File (e->getStringAttribute ("path"));
                it.name = e->getStringAttribute ("name");
                it.type = e->getStringAttribute ("type");
                it.details = e->getStringAttribute ("details");
                it.seconds = e->getDoubleAttribute ("seconds");
                it.bpm = e->getDoubleAttribute ("bpm");
                it.key = e->getIntAttribute ("key", -1);
                it.tuning = e->getDoubleAttribute ("tuning");
                it.tuningGuessed = e->getBoolAttribute ("tuningGuessed");
                it.isPreset = e->getBoolAttribute ("preset");
                it.bpmGuessed = e->getBoolAttribute ("bpmGuessed");
                it.keyGuessed = e->getBoolAttribute ("keyGuessed");
                cache[it.file.getFullPathName()] = { e->getStringAttribute ("stamp"), it };
            }
    }

    // Replaces any scan in progress. Results are collected with drain().
    void scan (const juce::Array<juce::File>& folders)
    {
        stopThread (10000);
        { const juce::ScopedLock sl (lock); pending.clear(); }
        roots = folders;
        startThread();
    }

    std::vector<Item> drain()
    {
        const juce::ScopedLock sl (lock);
        std::vector<Item> out;
        out.swap (pending);
        return out;
    }

    bool isScanning() const { return isThreadRunning(); }

    // Reads one file now, bypassing the cache. Safe to call while a scan runs.
    Item describe (const juce::File& file)
    {
        return presets::isPresetFile (file) ? readPreset (file) : readSample (file);
    }

private:
    static constexpr int engineVersion = 2; // bump when detection changes
    struct Cached { juce::String stamp; Item item; };

    void saveCache() const
    {
        juce::XmlElement xml ("cache");
        xml.setAttribute ("engine", engineVersion);
        for (auto& [path, c] : cache)
        {
            auto* e = xml.createNewChildElement ("file");
            e->setAttribute ("path", path);
            e->setAttribute ("stamp", c.stamp);
            e->setAttribute ("name", c.item.name);
            e->setAttribute ("type", c.item.type);
            e->setAttribute ("details", c.item.details);
            e->setAttribute ("seconds", c.item.seconds);
            e->setAttribute ("bpm", c.item.bpm);
            e->setAttribute ("key", c.item.key);
            e->setAttribute ("tuning", c.item.tuning);
            e->setAttribute ("tuningGuessed", c.item.tuningGuessed);
            e->setAttribute ("preset", c.item.isPreset);
            e->setAttribute ("bpmGuessed", c.item.bpmGuessed);
            e->setAttribute ("keyGuessed", c.item.keyGuessed);
        }
        xml.writeTo (dataFolder().getChildFile ("cache.xml"));
    }

    void run() override
    {
        juce::StringArray seen;
        for (auto& root : roots)
            for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
            {
                if (threadShouldExit()) return;
                const auto file = entry.getFile();
                const bool preset = presets::isPresetFile (file);
                if (file.getFileName().startsWithChar ('.')
                     || (! preset && formats.findFormatForFileExtension (file.getFileExtension()) == nullptr))
                    continue;

                const auto path = file.getFullPathName();
                const auto stamp = juce::String (entry.getFileSize()) + ":"
                                 + juce::String (entry.getModificationTime().toMilliseconds());
                auto found = cache.find (path);
                if (found == cache.end() || found->second.stamp != stamp)
                    found = cache.insert_or_assign (path, Cached { stamp, preset ? readPreset (file)
                                                                                : readSample (file) }).first;
                const juce::ScopedLock sl (lock);
                pending.push_back (found->second.item);
            }
    }

    static Item readPreset (const juce::File& file)
    {
        const auto info = presets::read (file);
        Item it;
        it.file = file;
        it.isPreset = true;
        it.name = info.name;
        it.type = info.synth;
        it.details = info.details;
        return it;
    }

    Item readSample (const juce::File& file)
    {
        Item it;
        it.file = file;
        it.name = file.getFileName();
        it.type = file.getFileExtension().substring (1).toUpperCase();
        const auto stem = file.getFileNameWithoutExtension();
        it.bpm = analysis::bpmFromName (stem);
        it.key = analysis::keyFromName (stem);
        it.tuning = analysis::tuningFromName (stem);

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->sampleRate <= 0) { it.details = "Unreadable file\n"; return it; }

        it.seconds = (double) reader->lengthInSamples / reader->sampleRate;
        it.details << "Sample rate: " << juce::String (reader->sampleRate, 0) << " Hz\n"
                   << "Channels: " << (int) reader->numChannels << "\n"
                   << "Bit depth: " << (int) reader->bitsPerSample << "\n";

        // Analyse at most two minutes, mixed to mono. Tuning only needs 30 seconds.
        const int n = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 120.0));
        if (n <= 0) return it;
        juce::AudioBuffer<float> buffer (juce::jmin (2, (int) reader->numChannels), n);
        reader->read (&buffer, 0, n, 0, true, true);
        if (buffer.getNumChannels() == 2)
        {
            buffer.addFrom (0, 0, buffer, 1, 0, n);
            buffer.applyGain (0, 0, n, 0.5f);
        }
        auto* mono = buffer.getReadPointer (0);
        if (it.tuning <= 0) // tuning first: the key detector uses it
        {
            it.tuning = analysis::detectTuning (mono, juce::jmin (n, (int) (reader->sampleRate * 30.0)),
                                                reader->sampleRate);
            it.tuningGuessed = it.tuning > 0;
        }
        if (it.bpm <= 0)
        {
            it.bpm = detect::bpm (mono, n, reader->sampleRate);
            it.bpmGuessed = it.bpm > 0;
        }
        if (it.key < 0 && it.seconds >= 1.0) // short one-shots rarely have a key
        {
            it.key = detect::key (mono, n, reader->sampleRate, it.tuning);
            it.keyGuessed = it.key >= 0;
        }
        return it;
    }

    juce::Array<juce::File> roots;
    std::map<juce::String, Cached> cache; // only touched by the scanner while it runs
    std::vector<Item> pending;
    juce::CriticalSection lock;
};
