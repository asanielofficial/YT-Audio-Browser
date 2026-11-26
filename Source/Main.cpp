#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <set>
#include "ClassicLook.h"
#include "FolderTree.h"
#include "FileTable.h"
#include "FileActions.h"
#include "Library.h"

using classic::bevel;

// Waveform for samples, metadata text for presets. Click or drag to seek.
class Preview : public juce::Component, private juce::ChangeListener
{
public:
    Preview (juce::AudioFormatManager& formats, juce::AudioTransportSource& t)
        : transport (t), thumbnail (256, formats, cache)
    {
        thumbnail.addChangeListener (this);
    }

    ~Preview() override { thumbnail.removeChangeListener (this); }

    void show (const Item* item)
    {
        thumbnail.clear();
        text.clear();
        audio = item != nullptr && ! item->isPreset;
        if (audio) thumbnail.setSource (new juce::FileInputSource (item->file));
        else if (item != nullptr)
            text << item->name << "\n" << item->type << " preset (no audio preview)\n" << item->details;
        else text = "Select a sound to preview it.";
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.fillAll (classic::white);
        const auto inner = r.reduced (4);
        g.setFont (classic::font());
        if (audio && thumbnail.getTotalLength() > 0)
        {
            const double length = thumbnail.getTotalLength();
            g.setColour (classic::navy);
            thumbnail.drawChannels (g, inner, 0.0, length, 0.95f);
            g.setColour (juce::Colours::red);
            const double pos = transport.getCurrentPosition() / length;
            g.fillRect (inner.getX() + juce::roundToInt (juce::jlimit (0.0, 1.0, pos) * inner.getWidth()),
                        inner.getY(), 1, inner.getHeight());
        }
        else
        {
            g.setColour (classic::dark);
            g.drawFittedText (text, inner.reduced (4), juce::Justification::topLeft, 8, 1.0f);
        }
        bevel (g, r, true);
    }

    void mouseDown (const juce::MouseEvent& e) override { seek (e); }
    void mouseDrag (const juce::MouseEvent& e) override { seek (e); }

private:
    void seek (const juce::MouseEvent& e)
    {
        if (! audio) return;
        const double p = juce::jlimit (0.0, 1.0, (e.x - 4) / (double) juce::jmax (1, getWidth() - 8));
        transport.setPosition (p * thumbnail.getTotalLength());
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    juce::AudioTransportSource& transport;
    juce::AudioThumbnailCache cache { 32 };
    juce::AudioThumbnail thumbnail;
    juce::String text { "Select a sound to preview it." };
    bool audio = false;
};

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::FileDragAndDropTarget,
                      private juce::MenuBarModel,
                      private juce::TableListBoxModel,
                      private juce::Timer
{
    enum Column { nameCol = 1, typeCol, lengthCol, bpmCol, keyCol, tuningCol, folderCol };
    enum Command { addCmd = 1, removeCmd, rescanCmd, quitCmd, autoCmd, loopCmd, aboutCmd };

public:
    explicit MainComponent (const juce::StringArray& args)
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "settings";
        settings = std::make_unique<juce::PropertiesFile> (Library::dataFolder().getChildFile ("settings.xml"), options);

        deviceManager.initialiseWithDefaultDevices (0, 2);
        deviceManager.addAudioCallback (&player);
        player.setSource (&transport);
        readAhead.startThread();

        for (auto* c : std::initializer_list<juce::Component*> { &menu, &addButton, &search, &typeBox, &keyBox,
                 &bpmMin, &bpmMax, &folderTree, &table, &preview, &playButton, &stopButton, &autoPlay,
                 &loop, &volume, &searchLabel, &bpmLabel, &toLabel, &status })
            addAndMakeVisible (c);

        menu.setModel (this);
        addButton.onClick = [this] { chooseFolder(); };
        playButton.onClick = [this] { play(); };
        stopButton.onClick = [this] { transport.stop(); transport.setPosition (0); preview.repaint(); };

        for (auto* e : { &search, &bpmMin, &bpmMax })
        {
            e->setFont (classic::font());
            e->onTextChange = [this] { applyFilter(); };
        }
        bpmMin.setInputRestrictions (3, "0123456789");
        bpmMax.setInputRestrictions (3, "0123456789");

        typeBox.addItemList ({ "All files", "Samples", "Presets", "Serum", "Vital", "Other VST" }, 1);
        keyBox.addItem ("Any key", 1);
        keyBox.addItemList (analysis::keyNames(), 2);
        for (auto* b : { &typeBox, &keyBox })
        {
            b->setSelectedId (1, juce::dontSendNotification);
            b->onChange = [this] { applyFilter(); };
        }

        autoPlay.setToggleState (settings->getBoolValue ("auto", true), juce::dontSendNotification);
        loop.setToggleState (settings->getBoolValue ("loop", false), juce::dontSendNotification);
        loop.onClick = [this] { if (source != nullptr) source->setLooping (loop.getToggleState()); };
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        volume.setRange (0.0, 1.0);
        volume.onValueChange = [this] { transport.setGain ((float) volume.getValue()); };
        volume.setValue (settings->getDoubleValue ("volume", 0.8), juce::sendNotificationSync);

        auto& header = table.getHeader();
        const std::pair<const char*, int> columns[] = { { "Name", 220 }, { "Type", 80 }, { "Length", 65 },
                                                        { "BPM", 55 }, { "Key", 70 }, { "Tuning", 75 }, { "Folder", 110 } };
        int id = 1;
        for (auto& [title, width] : columns) header.addColumn (title, id++, width, 40);
        header.setSortColumnId (nameCol, true);
        table.setModel (this);
        table.onCursorMoved = [this] (int row) { previewRow (row); };
        table.onRename = [this] { renameSelected(); };
        table.setRowHeight (18);
        table.setHeaderHeight (20);
        table.setOutlineThickness (2);
        table.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
        table.getViewport()->getVerticalScrollBar().setAutoHide (false);
        folderState.onSelect = [this] { applyFilter(); };
        folderTree.setColour (juce::TreeView::backgroundColourId, classic::white);
        folderTree.setColour (juce::TreeView::linesColourId, classic::shadow);
        folderTree.setDefaultOpenness (false);
        folderTree.setRootItem (&allFolders);
        allFolders.setOpen (true);
        for (auto* l : { &searchLabel, &bpmLabel, &toLabel, &status }) l->setFont (classic::font());
        // Keep Space for play/stop rather than clicking whichever button was used last.
        for (auto* b : std::initializer_list<juce::Component*> { &addButton, &playButton, &stopButton, &autoPlay, &loop })
            b->setWantsKeyboardFocus (false);

        library.loadCache();
        juce::StringArray saved;
        saved.addLines (settings->getValue ("folders"));
        saved.addArray (args);
        for (auto& path : saved)
            if (juce::File::isAbsolutePath (path) && juce::File (path).isDirectory())
                roots.addIfNotAlreadyThere (juce::File (path));
        rescan();

        setSize (900, 600);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        settings->setValue ("auto", autoPlay.getToggleState());
        settings->setValue ("loop", loop.getToggleState());
        settings->setValue ("volume", volume.getValue());
        settings->saveIfNeeded();
        menu.setModel (nullptr);
        folderTree.setRootItem (nullptr);
        transport.setSource (nullptr);
        player.setSource (nullptr);
        deviceManager.removeAudioCallback (&player);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (classic::face);
        for (int y : { 20, 54 }) // etched separators
        {
            g.setColour (classic::shadow); g.fillRect (0, y, getWidth(), 1);
            g.setColour (classic::white);  g.fillRect (0, y + 1, getWidth(), 1);
        }
        bevel (g, status.getBounds().expanded (2, 1), true);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        bevel (g, table.getBounds(), true);
        bevel (g, folderTree.getBounds().expanded (2), true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        menu.setBounds (r.removeFromTop (20));
        auto bar = r.removeFromTop (36).reduced (6, 7);
        addButton.setBounds (bar.removeFromLeft (90));
        bar.removeFromLeft (10);
        keyBox.setBounds (bar.removeFromRight (90));
        bar.removeFromRight (8);
        bpmMax.setBounds (bar.removeFromRight (42));
        toLabel.setBounds (bar.removeFromRight (22));
        bpmMin.setBounds (bar.removeFromRight (42));
        bpmLabel.setBounds (bar.removeFromRight (38));
        bar.removeFromRight (8);
        typeBox.setBounds (bar.removeFromRight (105));
        bar.removeFromRight (8);
        searchLabel.setBounds (bar.removeFromLeft (50));
        search.setBounds (bar);

        status.setBounds (r.removeFromBottom (22).reduced (4, 2));
        r.reduce (6, 4);
        auto transportRow = r.removeFromBottom (30).withTrimmedTop (5);
        playButton.setBounds (transportRow.removeFromLeft (75));
        transportRow.removeFromLeft (4);
        stopButton.setBounds (transportRow.removeFromLeft (75));
        transportRow.removeFromLeft (12);
        autoPlay.setBounds (transportRow.removeFromLeft (110));
        loop.setBounds (transportRow.removeFromLeft (70));
        volume.setBounds (transportRow.removeFromRight (140));
        preview.setBounds (r.removeFromBottom (130).withTrimmedTop (5));
        folderTree.setBounds (r.removeFromLeft (190).reduced (2));
        r.removeFromLeft (5);
        table.setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key != juce::KeyPress::spaceKey) return false;
        if (transport.isPlaying()) transport.stop();
        else play();
        return true;
    }

    // Dropping folders onto the window adds them to the library.
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return std::any_of (files.begin(), files.end(), [] (auto& f) { return juce::File (f).isDirectory(); });
    }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        for (auto& f : files)
            if (juce::File (f).isDirectory()) roots.addIfNotAlreadyThere (juce::File (f));
        foldersChanged();
    }

private:
    // Drag out: hand the real files to the OS once the drag leaves the window.
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override
    {
        dragFiles.clear();
        for (int i = 0; i < rows.size(); ++i)
            if (juce::isPositiveAndBelow (rows[i], (int) visible.size()))
                dragFiles.add (items[visible[(size_t) rows[i]]].file.getFullPathName());
        return dragFiles.isEmpty() ? juce::var() : juce::var ("files");
    }

    bool shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails&,
                                               juce::StringArray& files, bool& canMoveFiles) override
    {
        files = dragFiles;
        canMoveFiles = false; // never let the target move the originals
        return ! files.isEmpty();
    }

    int getNumRows() override { return (int) visible.size(); }

    void paintRowBackground (juce::Graphics& g, int, int, int, bool selected) override
    {
        g.fillAll (selected ? classic::navy : classic::white);
    }

    void paintCell (juce::Graphics& g, int row, int column, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) visible.size())) return;
        g.setColour (selected ? classic::white : classic::dark);
        g.setFont (classic::font());
        g.drawText (cellText (items[visible[(size_t) row]], column), 6, 0, w - 8, h,
                    juce::Justification::centredLeft);
    }

    static juce::String cellText (const Item& it, int column)
    {
        switch (column)
        {
            case nameCol:   return it.name;
            case typeCol:   return it.type;
            case lengthCol: return it.isPreset || it.seconds <= 0 ? juce::String()
                                 : juce::String (it.seconds, 2) + " s";
            case bpmCol:    return it.bpm > 0 ? (it.bpmGuessed ? "~" : "") + juce::String (juce::roundToInt (it.bpm))
                                              : juce::String();
            case keyCol:    return it.key >= 0 ? (it.keyGuessed ? "~" : "") + analysis::keyNames()[it.key]
                                               : juce::String();
            case tuningCol: return it.tuning > 0 ? (it.tuningGuessed ? "~" : "") + juce::String (juce::roundToInt (it.tuning)) + " Hz"
                                                  : juce::String();
            case folderCol: return it.file.getParentDirectory().getFileName();
            default:        return {};
        }
    }

    void sortOrderChanged (int, bool) override { applyFilter(); }
    void selectedRowsChanged (int row) override
    {
        if (refreshing || juce::ModifierKeys::currentModifiers.isPopupMenu()) return;
        if (table.isRowSelected (row)) previewRow (row); // not when Ctrl just deselected it
    }

    void previewRow (int row)
    {
        if (! juce::isPositiveAndBelow (row, (int) visible.size())) return;
        load (items[visible[(size_t) row]]);
        if (autoPlay.getToggleState()) play();
    }

    void cellClicked (int row, int, const juce::MouseEvent& e) override
    {
        table.rowClicked (row, e.mods);
        if (e.mods.isPopupMenu()) { showFileMenu(); return; }
        if (e.mods.isShiftDown() || e.mods.isCommandDown()) return;
        // Clicking the already selected sample plays it again.
        if (juce::isPositiveAndBelow (row, (int) visible.size())
             && items[visible[(size_t) row]].file == loaded && autoPlay.getToggleState())
            play();
    }

    void returnKeyPressed (int) override { openSelected(); }
    void deleteKeyPressed (int) override { confirmDelete(); }

    juce::Array<juce::File> selectedFiles() const
    {
        juce::Array<juce::File> files;
        const auto rows = table.getSelectedRows();
        for (int i = 0; i < rows.size(); ++i)
            if (juce::isPositiveAndBelow (rows[i], (int) visible.size()))
                files.add (items[visible[(size_t) rows[i]]].file);
        return files;
    }

    void showFileMenu()
    {
        const int count = selectedFiles().size();
        if (count == 0) return;
        juce::PopupMenu m;
        m.addItem (1, "Open", count <= 15);
        m.addItem (2, "Open Containing Folder");
        m.addSeparator();
        m.addItem (3, "Rename...", count == 1);
        m.addItem (4, count == 1 ? juce::String ("Delete...") : "Delete " + juce::String (count) + " Files...");
        m.showMenuAsync (juce::PopupMenu::Options(),
                         [safe = juce::Component::SafePointer<MainComponent> (this)] (int choice)
                         {
                             if (safe == nullptr) return;
                             if (choice == 1) safe->openSelected();
                             else if (choice == 2) safe->revealSelected();
                             else if (choice == 3) safe->renameSelected();
                             else if (choice == 4) safe->confirmDelete();
                         });
    }

    // Opens each file in whatever application the system associates with it.
    void openSelected()
    {
        const auto files = selectedFiles();
        if (files.size() > 15) return;
        juce::StringArray failed;
        for (auto& f : files)
            if (! f.startAsProcess()) failed.add (f.getFileName());
        if (! failed.isEmpty())
            warn ("Open", "No application could open:\n" + failed.joinIntoString ("\n"));
    }

    void revealSelected()
    {
        juce::StringArray done; // one window per folder, at most five
        for (auto& f : selectedFiles())
            if (done.size() < 5 && done.addIfNotAlreadyThere (f.getParentDirectory().getFullPathName()))
                f.revealToUser();
    }

    void warn (const juce::String& title, const juce::String& message)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle (title).withMessage (message)
                                          .withButton ("OK").withAssociatedComponent (this), nullptr);
    }

    // Closes the previewed file so the system will let us rename or delete it.
    void releasePreview()
    {
        transport.stop();
        transport.setSource (nullptr);
        source.reset();
        loaded = juce::File();
        preview.show (nullptr);
    }

    void forget (const juce::File& file)
    {
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            table.deselectAllRows(); // row numbers are about to change
            visible.clear();
        }
        seen.erase (file.getFullPathName());
        items.erase (std::remove_if (items.begin(), items.end(),
                                     [&file] (const Item& it) { return it.file.getFullPathName() == file.getFullPathName(); }),
                     items.end());
    }

    void renameSelected()
    {
        const auto files = selectedFiles();
        if (files.size() != 1) return;
        const auto file = files[0];
        juce::String message = "New name for " + file.getFileName()
                                 + "  (" + file.getFileExtension() + " is kept)";
        if (presets::isPresetFile (file))
            message << "\nThe preset name stored inside the file is not changed.";

        dialog = std::make_unique<juce::AlertWindow> ("Rename", message, juce::MessageBoxIconType::NoIcon, this);
        dialog->addTextEditor ("name", file.getFileNameWithoutExtension());
        dialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        dialog->enterModalState (true, juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer<MainComponent> (this), file] (int result)
            {
                if (safe == nullptr) return;
                const auto name = safe->dialog->getTextEditorContents ("name");
                safe->dialog->setVisible (false);
                if (result == 1) safe->applyRename (file, name);
            }), false);
        if (auto* editor = dialog->getTextEditor ("name"))
        {
            editor->grabKeyboardFocus();
            editor->selectAll();
        }
    }

    void applyRename (const juce::File& file, const juce::String& name)
    {
        releasePreview();
        juce::File renamed;
        const auto error = fileactions::rename (file, name, renamed);
        if (error.isNotEmpty()) { warn ("Rename", error); return; }
        if (renamed.getFileName() == file.getFileName()) return;
        forget (file);
        seen.insert (renamed.getFullPathName());
        items.push_back (library.describe (renamed)); // reads BPM, key and tuning hints in the new name
        applyFilter ({ renamed });
    }

    void confirmDelete()
    {
        const auto files = selectedFiles();
        if (files.isEmpty()) return;
       #if JUCE_WINDOWS
        const juce::String bin = "Recycle Bin";
       #else
        const juce::String bin = "Trash";
       #endif
        juce::String message = files.size() == 1 ? "Move " + files[0].getFileName() + " to the " + bin + "?"
                                                 : "Move these " + juce::String (files.size()) + " files to the " + bin + "?\n";
        if (files.size() > 1)
        {
            for (int i = 0; i < juce::jmin (8, files.size()); ++i) message << "\n" << files[i].getFileName();
            if (files.size() > 8) message << "\n...and " << files.size() - 8 << " more";
        }
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Delete").withMessage (message)
                                          .withButton ("Delete").withButton ("Cancel").withAssociatedComponent (this),
                                      [safe = juce::Component::SafePointer<MainComponent> (this), files] (int result)
                                      {
                                          if (safe != nullptr && result == 1) safe->deleteFiles (files);
                                      });
    }

    void deleteFiles (const juce::Array<juce::File>& files)
    {
        releasePreview();
        juce::StringArray failed;
        for (auto& f : files)
        {
            if (fileactions::moveToTrash (f)) forget (f);
            else failed.add (f.getFileName());
        }
        applyFilter();
        if (! failed.isEmpty())
            warn ("Delete", "These files were not deleted:\n" + failed.joinIntoString ("\n", 0, 10));
    }

    juce::String formatSummary()
    {
        juce::String text = "Audio files (preview, BPM, key, tuning):\n";
        for (int i = 0; i < library.formats.getNumKnownFormats(); ++i)
        {
            auto* format = library.formats.getKnownFormat (i);
            auto extensions = format->getFileExtensions();
            extensions.removeDuplicates (true);
            text << "  " << format->getFormatName().replace (" file", "") << ":  "
                 << extensions.joinIntoString (" ") << "\n";
        }
        return text + "\nSynth presets (details only, no audio):\n"
                      "  Serum:  .fxp  .SerumPreset\n"
                      "  Vital:  .vital\n"
                      "  VST2:  .fxp  .fxb\n"
                      "  VST3:  .vstpreset";
    }

    void load (const Item& it)
    {
        if (it.file == loaded) return;
        transport.stop();
        transport.setSource (nullptr);
        source.reset();
        loaded = it.file;
        preview.show (&it);
        if (it.isPreset) return;
        if (auto* reader = library.formats.createReaderFor (it.file))
        {
            const double rate = reader->sampleRate;
            source = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
            source->setLooping (loop.getToggleState());
            transport.setSource (source.get(), 32768, &readAhead, rate);
        }
    }

    void play()
    {
        if (source == nullptr) return;
        transport.setPosition (0);
        transport.start();
    }

    bool matches (const Item& it) const
    {
        if (folderState.selected != juce::File() && ! it.file.isAChildOf (folderState.selected)) return false;

        switch (typeBox.getSelectedId())
        {
            case 2: if (it.isPreset) return false; break;
            case 3: if (! it.isPreset) return false; break;
            case 4: if (! it.isPreset || it.type != "Serum") return false; break;
            case 5: if (! it.isPreset || it.type != "Vital") return false; break;
            case 6: if (! it.isPreset || ! it.type.startsWith ("VST")) return false; break;
            default: break;
        }

        const int key = keyBox.getSelectedId() - 2;
        if (key >= 0 && it.key != key) return false;

        const int lo = bpmMin.getText().getIntValue(), hi = bpmMax.getText().getIntValue();
        if ((lo > 0 || hi > 0) && (it.bpm <= 0 || it.bpm < lo || (hi > 0 && it.bpm > hi))) return false;

        const auto haystack = it.name + " " + it.type + " " + it.file.getFullPathName() + " " + it.details;
        for (auto& word : juce::StringArray::fromTokens (search.getText(), true))
            if (! haystack.containsIgnoreCase (word)) return false;
        return true;
    }

    // Rebuilds the visible rows, keeping the selection (or selecting the files given).
    void applyFilter (juce::Array<juce::File> selected = {})
    {
        if (selected.isEmpty()) selected = selectedFiles();
        std::set<juce::String> wanted;
        for (auto& f : selected) wanted.insert (f.getFullPathName());

        visible.clear();
        for (size_t i = 0; i < items.size(); ++i)
            if (matches (items[i])) visible.push_back (i);

        const int column = table.getHeader().getSortColumnId();
        const bool forwards = table.getHeader().isSortedForwards();
        std::stable_sort (visible.begin(), visible.end(), [&] (size_t a, size_t b)
        {
            const auto& x = items[forwards ? a : b];
            const auto& y = items[forwards ? b : a];
            if (column == lengthCol) return x.seconds < y.seconds;
            if (column == bpmCol) return x.bpm < y.bpm;
            if (column == keyCol) return x.key < y.key;
            if (column == tuningCol) return x.tuning < y.tuning;
            return cellText (x, column).compareNatural (cellText (y, column)) < 0;
        });

        // Keep the same file selected without replaying it.
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        table.deselectAllRows();
        table.updateContent();
        juce::SparseSet<int> rows;
        for (size_t i = 0; i < visible.size(); ++i)
            if (wanted.count (items[visible[i]].file.getFullPathName()) > 0)
                rows.addRange ({ (int) i, (int) i + 1 });
        table.setSelectedRows (rows, juce::dontSendNotification);
        table.resetAnchor (rows.isEmpty() ? -1 : rows[0]);
        table.repaint();
        updateStatus();
    }

    void updateStatus()
    {
        status.setText (juce::String ((int) visible.size()) + " of " + juce::String ((int) items.size()) + " items"
                          + (library.isScanning() ? "   Scanning..." : "")
                          + (roots.isEmpty() ? "   Add a folder, or drop one onto this window." : "")
                          + (deviceManager.getCurrentAudioDevice() == nullptr ? "   No audio device." : ""),
                        juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const bool scanning = library.isScanning();
        auto found = library.drain();
        if (! found.empty())
        {
            for (auto& it : found)
                if (it.file.existsAsFile() && seen.insert (it.file.getFullPathName()).second)
                {
                    addToTree (it.file);
                    items.push_back (std::move (it));
                }
            applyFilter();
        }
        else if (scanning != wasScanning) updateStatus();
        wasScanning = scanning;
        if (transport.isPlaying()) preview.repaint();
    }

    void rescan()
    {
        table.deselectAllRows();
        items.clear();
        visible.clear();
        seen.clear();
        allFolders.clearSubItems();
        for (auto& root : roots) allFolders.child (root, true, false);
        if (folderState.selected == juce::File())
            allFolders.setSelected (true, true, juce::dontSendNotification);
        library.scan (roots);
        applyFilter();
    }

    // Only folders that hold sounds or presets (or lead to some) appear in the tree.
    void addToTree (const juce::File& file)
    {
        for (auto& root : roots)
            if (file.isAChildOf (root))
                allFolders.child (root, true, false).addPath (file.getParentDirectory());
    }

    void foldersChanged()
    {
        juce::StringArray paths;
        for (auto& f : roots) paths.add (f.getFullPathName());
        settings->setValue ("folders", paths.joinIntoString ("\n"));
        settings->saveIfNeeded();
        folderState.selected = juce::File();
        rescan();
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add a sample or preset folder");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& c)
                              {
                                  if (c.getResult().isDirectory())
                                  {
                                      roots.addIfNotAlreadyThere (c.getResult());
                                      foldersChanged();
                                  }
                              });
    }

    juce::StringArray getMenuBarNames() override { return { "File", "Options", "Help" }; }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        if (index == 0)
        {
            m.addItem (addCmd, "Add Folder...");
            m.addItem (removeCmd, "Remove Selected Folder", roots.contains (folderState.selected));
            m.addItem (rescanCmd, "Rescan");
            m.addSeparator();
            m.addItem (quitCmd, "Exit");
        }
        else if (index == 1)
        {
            m.addItem (autoCmd, "Auto-preview", true, autoPlay.getToggleState());
            m.addItem (loopCmd, "Loop", true, loop.getToggleState());
        }
        else m.addItem (aboutCmd, "About...");
        return m;
    }

    void menuItemSelected (int command, int) override
    {
        if (command == addCmd) chooseFolder();
        else if (command == removeCmd && roots.contains (folderState.selected))
        {
            roots.removeFirstMatchingValue (folderState.selected); // removes from the list only
            foldersChanged();
        }
        else if (command == rescanCmd) rescan();
        else if (command == quitCmd) juce::JUCEApplication::getInstance()->systemRequestedQuit();
        else if (command == autoCmd) autoPlay.triggerClick();
        else if (command == loopCmd) loop.triggerClick();
        else if (command == aboutCmd)
            warn ("About", "Samplite Explorer " APP_VERSION "\n\n" + formatSummary()
                             + "\n\n~ marks an estimated BPM, key or tuning.");
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer player;
    juce::TimeSliceThread readAhead { "audio read-ahead" };
    std::unique_ptr<juce::AudioFormatReaderSource> source;
    juce::AudioTransportSource transport;

    Library library;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Array<juce::File> roots;
    std::vector<Item> items;
    std::vector<size_t> visible;
    std::set<juce::String> seen;
    juce::StringArray dragFiles;
    juce::File loaded;
    bool refreshing = false, wasScanning = false;

    juce::MenuBarComponent menu;
    juce::TextButton addButton { "Add Folder..." }, playButton { "Play" }, stopButton { "Stop" };
    juce::TextEditor search, bpmMin, bpmMax;
    juce::ComboBox typeBox, keyBox;
    juce::Label searchLabel { {}, "Search:" }, bpmLabel { {}, "BPM:" }, toLabel { {}, "to" }, status;
    juce::ToggleButton autoPlay { "Auto-preview" }, loop { "Loop" };
    juce::Slider volume;
    FolderState folderState;
    FolderNode allFolders { folderState, juce::File() };
    juce::TreeView folderTree;
    FileTable table;
    std::unique_ptr<juce::AlertWindow> dialog;
    Preview preview { library.formats, transport };
};

class App : public juce::JUCEApplication
{
    struct Window : public juce::DocumentWindow
    {
        explicit Window (const juce::StringArray& args)
            : juce::DocumentWindow ("Samplite Explorer", classic::face, allButtons)
        {
            setUsingNativeTitleBar (false);
            setTitleBarHeight (20);
            setTitleBarTextCentred (false);
            setContentOwned (new MainComponent (args), true);
            setResizable (true, false);
            setResizeLimits (760, 460, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

public:
    const juce::String getApplicationName() override { return "Samplite Explorer"; }
    const juce::String getApplicationVersion() override { return APP_VERSION; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&look);
        window = std::make_unique<Window> (getCommandLineParameterArray());
    }

    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

private:
    classic::Look look;
    std::unique_ptr<Window> window;
};

START_JUCE_APPLICATION (App)
