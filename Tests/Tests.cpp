#include <cstdio>
#include <cstring>
#include "../Source/Library.h"
#include "../Source/Detect.h"
#include "../Source/FileActions.h"

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

int main()
{
    using namespace analysis;
    check (bpmFromName ("drum_loop_128bpm") == 128, "bpm: suffix");
    check (bpmFromName ("Break 92 BPM") == 92, "bpm: separate word");
    check (bpmFromName ("kick_01") == 0, "bpm: index numbers ignored");
    check (keyFromName ("pad_Am") == 21, "key: Am");
    check (keyFromName ("bass_F#min_120") == 18, "key: F#min");
    check (keyFromName ("chord Bbmaj") == 10, "key: Bbmaj");
    check (keyFromName ("Dog_bark") == -1, "key: ordinary words ignored");

    const double sr = 44100;
    for (double bpm : { 90.0, 120.0, 140.0 })
    {
        std::vector<float> x ((size_t) (sr * 8), 0.0f);
        juce::Random random (1);
        const double step = sr * 60.0 / bpm;
        for (double start = 0; start < (double) x.size(); start += step)
            for (int i = 0; i < 2000 && (size_t) start + (size_t) i < x.size(); ++i)
                x[(size_t) start + (size_t) i] = (random.nextFloat() * 2 - 1) * std::exp (-i / 300.0f);
        const double got = detectBpm (x.data(), (int) x.size(), sr);
        std::printf ("      click track %.0f -> %.0f\n", bpm, got);
        check (std::abs (got - bpm) <= 2, "bpm: detected from click track");
    }

    auto chord = [sr] (std::initializer_list<double> notes)
    {
        std::vector<float> x ((size_t) (sr * 3), 0.0f);
        for (double f : notes)
            for (size_t i = 0; i < x.size(); ++i)
                x[i] += 0.2f * (float) std::sin (juce::MathConstants<double>::twoPi * f * (double) i / sr);
        return x;
    };
    auto c = chord ({ 261.63, 329.63, 392.00 });
    auto am = chord ({ 220.00, 261.63, 329.63 });
    auto tone = chord ({ 440.0 });
    check (detectKey (c.data(), (int) c.size(), sr) == 0, "key: C major triad");
    check (detectKey (am.data(), (int) am.size(), sr) == 21, "key: A minor triad");
    check (detectKey (tone.data(), (int) tone.size(), sr) == -1, "key: single tone left blank");

    check (detectBpm (c.data(), (int) c.size(), sr) == 0, "bpm: sustained chord left blank");
    check (detectBpm (am.data(), (int) am.size(), sr) == 0, "bpm: sustained minor chord left blank");

    check (detect::key (c.data(), (int) c.size(), sr, 440.0) == 0, "qm key: C major triad");
    check (detect::key (am.data(), (int) am.size(), sr, 440.0) == 21, "qm key: A minor triad");
    check (detect::key (tone.data(), (int) tone.size(), sr, 440.0) == -1, "qm key: single tone left blank");
    {
        auto low = chord ({ 261.63 * 432 / 440, 329.63 * 432 / 440, 392.00 * 432 / 440 });
        check (detect::key (low.data(), (int) low.size(), sr, 432.0) == 0, "qm key: C major tuned to 432");
    }
    for (double bpm : { 90.0, 128.0, 150.0 }) // 45 s: the long-material path
    {
        std::vector<float> x ((size_t) (sr * 45), 0.0f);
        juce::Random random (1);
        for (double start = 0; start < (double) x.size(); start += sr * 60.0 / bpm)
            for (int i = 0; i < 2000 && (size_t) start + (size_t) i < x.size(); ++i)
                x[(size_t) start + (size_t) i] = (random.nextFloat() * 2 - 1) * std::exp (-i / 300.0f);
        const double got = detect::qmBpm (x.data(), (int) x.size(), sr);
        std::printf ("      QM click track %.0f -> %.2f\n", bpm, got);
        check (std::abs (got - bpm) <= 1.5, "qm bpm: long click track");
        check (std::abs (detect::bpm (x.data(), (int) x.size(), sr) - bpm) <= 1.5, "bpm: long file");
    }

    {
        std::vector<float> hits ((size_t) (sr * 4));
        juce::Random random (3);
        for (size_t i = 0; i < hits.size(); ++i)
            hits[i] = (random.nextFloat() - 0.5f) * std::exp (-(float) (i % 11025) / 1500.0f);
        check (detect::key (hits.data(), (int) hits.size(), sr, 440.0) == -1, "qm key: noise hits left blank");
    }
    check (tuningFromName ("pad_432Hz") == 432, "tuning: from name");
    check (tuningFromName ("pad 440 Hz") == 440, "tuning: separate unit");
    check (tuningFromName ("sub_100hz") == 0, "tuning: other frequencies ignored");
    for (double a4 : { 432.0, 440.0, 446.0 })
    {
        const double r = a4 / 440.0;
        auto t = chord ({ 220.00 * r, 261.63 * r, 329.63 * r });
        const double got = detectTuning (t.data(), (int) t.size(), sr);
        std::printf ("      chord at %.0f -> %.2f\n", a4, got);
        check (std::abs (got - a4) < 1.0, "tuning: detected from chord");
        auto one = chord ({ a4 });
        check (std::abs (detectTuning (one.data(), (int) (sr * 0.3), sr) - a4) < 1.0, "tuning: short single note");
    }
    std::vector<float> noise ((size_t) (sr * 3));
    juce::Random rng (7);
    for (auto& v : noise) v = rng.nextFloat() - 0.5f;
    check (detectTuning (noise.data(), (int) noise.size(), sr) == 0, "tuning: noise left blank");

    auto dir = juce::File::createTempFile ("presets");
    dir.createDirectory();
    auto vital = dir.getChildFile ("a.vital");
    vital.replaceWithText (R"({"preset_name":"Warm Pad","author":"me","preset_style":"Pad"})");
    check (presets::read (vital).name == "Warm Pad", "preset: Vital");

    char fxp[60] = {};
    std::memcpy (fxp, "CcnK", 4);
    std::memcpy (fxp + 16, "XfsX", 4);
    std::memcpy (fxp + 28, "Big Lead", 8);
    auto serum = dir.getChildFile ("b.fxp");
    serum.replaceWithData (fxp, sizeof (fxp));
    check (presets::read (serum).name == "Big Lead" && presets::read (serum).synth == "Serum", "preset: Serum fxp");

    auto serum2 = dir.getChildFile ("c.SerumPreset");
    serum2.replaceWithText (R"(XferJson..{"presetName":"Glass {Keys}","presetAuthor":"me"}..binary)");
    check (presets::read (serum2).name == "Glass {Keys}", "preset: SerumPreset (assumed layout)");

    auto broken = dir.getChildFile ("d.fxp");
    broken.replaceWithText ("junk");
    check (presets::read (broken).name == "d", "preset: damaged file falls back to filename");
    dir.deleteRecursively();

    {
        auto folder = juce::File::createTempFile ("rename");
        folder.createDirectory();
        auto a = folder.getChildFile ("a.wav"), b = folder.getChildFile ("b.wav");
        a.replaceWithText ("A");
        b.replaceWithText ("B");
        juce::File out;
        check (fileactions::rename (a, "b", out).isNotEmpty() && b.loadFileAsString() == "B" && a.existsAsFile(),
               "rename: never overwrites another file");
        check (fileactions::rename (a, "../x", out).isNotEmpty() && a.existsAsFile(), "rename: no path separators");
        check (fileactions::rename (a, "  ", out).isNotEmpty(), "rename: empty name refused");
        check (fileactions::rename (a, "c_v1.2", out).isEmpty() && out.getFileName() == "c_v1.2.wav"
                   && out.existsAsFile() && ! a.existsAsFile(), "rename: keeps extension");
        folder.deleteRecursively();
    }
   #if JUCE_LINUX
    {
        auto home = juce::File::createTempFile ("trash");
        home.createDirectory();
        setenv ("XDG_DATA_HOME", home.getFullPathName().toRawUTF8(), 1);
        auto victim = home.getChildFile ("my song #1.wav");
        for (auto* expected : { "my song #1.wav", "my song #1 2.wav" })
        {
            victim.replaceWithText ("x");
            check (fileactions::moveToTrash (victim) && ! victim.exists()
                       && home.getChildFile ("Trash/files").getChildFile (expected).existsAsFile(), "trash: file moved");
            const auto record = home.getChildFile ("Trash/info").getChildFile (juce::String (expected) + ".trashinfo");
            std::printf ("%s", record.loadFileAsString().toRawUTF8());
            check (record.loadFileAsString().contains ("my%20song%20%231.wav"), "trash: restore record written");
        }
        home.deleteRecursively();
    }
   #endif
    std::printf ("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
