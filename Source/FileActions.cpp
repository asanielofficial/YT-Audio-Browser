#include "FileActions.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>
 #include <vector>
#endif

namespace fileactions
{
bool moveToTrash (const juce::File& file)
{
   #if JUCE_WINDOWS
    // juce::File::moveToTrash deletes silently on drives with no Recycle Bin.
    // FOF_WANTNUKEWARNING makes Windows ask before it does that.
    const auto path = file.getFullPathName();
    std::vector<wchar_t> from (path.toWideCharPointer(), path.toWideCharPointer() + path.length());
    from.push_back (0);
    from.push_back (0); // the list must end with two nulls

    SHFILEOPSTRUCTW operation = {};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.data();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
    return SHFileOperationW (&operation) == 0 && ! operation.fAnyOperationsAborted && ! file.exists();
   #elif JUCE_LINUX || JUCE_BSD
    // juce::File::moveToTrash writes no .trashinfo, so file managers cannot restore the file.
    // This follows the freedesktop.org Trash specification.
    if (! file.existsAsFile()) return false;
    const auto xdg = juce::SystemStats::getEnvironmentVariable ("XDG_DATA_HOME", {});
    const auto data = juce::File::isAbsolutePath (xdg)
                        ? juce::File (xdg)
                        : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/share");
    const auto files = data.getChildFile ("Trash/files"), info = data.getChildFile ("Trash/info");
    if (! files.createDirectory() || ! info.createDirectory()) return false;

    auto name = file.getFileName();
    for (int i = 2; files.getChildFile (name).exists() || info.getChildFile (name + ".trashinfo").exists(); ++i)
        name = file.getFileNameWithoutExtension() + " " + juce::String (i) + file.getFileExtension();

    const auto record = info.getChildFile (name + ".trashinfo");
    const auto path = juce::URL::addEscapeChars (file.getFullPathName(), false).replace ("%2F", "/").replace ("%2f", "/");
    if (! record.replaceWithText ("[Trash Info]\nPath=" + path + "\nDeletionDate="
                                      + juce::Time::getCurrentTime().formatted ("%Y-%m-%dT%H:%M:%S") + "\n",
                                  false, false, "\n"))
        return false;
    if (file.moveFileTo (files.getChildFile (name))) return true;
    record.deleteFile();
    return false;
   #else
    return file.moveToTrash();
   #endif
}

juce::String rename (const juce::File& file, const juce::String& newStem, juce::File& renamed)
{
    renamed = file;
    const auto stem = newStem.trim();
    if (stem.isEmpty() || stem.startsWithChar ('.') || stem.endsWithChar ('.')
         || juce::File::createLegalFileName (stem) != stem || stem.containsAnyOf ("/\\:*?\"<>|"))
        return "That is not a valid file name.";

    const auto target = file.getSiblingFile (stem + file.getFileExtension());
    if (target.getFileName() == file.getFileName()) return {};
    // moveFileTo overwrites, so refuse an existing name (a change of case is the same file).
    if (target.exists() && target != file) return "A file with that name already exists.";
    if (! file.moveFileTo (target)) return "The file could not be renamed. It may be in use or read-only.";
    renamed = target;
    return {};
}
} // namespace fileactions
