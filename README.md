# Samplite Explorer

A Windows 95-style browser for audio samples and synth presets, written in C++ with JUCE 8.

## Build

You need CMake 3.22+, Git and a C++17 compiler. The first configure downloads JUCE.

- **Windows:** Visual Studio 2022 with "Desktop development with C++"
- **macOS:** Xcode command line tools
- **Linux:** `sudo apt install g++ cmake git pkg-config libasound2-dev libx11-dev libxext-dev libxinerama-dev libxrandr-dev libxcursor-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev`

Then, in this folder:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release

The first configure also downloads QM-DSP. The app is under `build/SampliteExplorer_artefacts/`. Tests build as `ExplorerTests`.

## Build with GitHub Actions

Put the contents of this folder at the root of a GitHub repository, including the hidden `.github` folder:

    git init -b main
    git add .
    git commit -m "Add Samplite Explorer"
    git remote add origin https://github.com/YOUR-NAME/YOUR-REPO.git
    git push -u origin main

The **Build** workflow runs on every push and pull request, or by hand from the Actions tab.
It builds and tests on Windows, macOS and Linux, creates an installer on each, and installs it as a check.
Download from the **Artifacts** section of a finished run:

| Platform | Installer | Install with |
|---|---|---|
| Windows 10/11 x64 | `...-Windows-Setup.exe` | Run it |
| macOS 11+ (Intel and Apple Silicon) | `...-macOS.pkg` | Open it; installs to Applications |
| Debian / Ubuntu x64 | `samplite-explorer_..._amd64.deb` | `sudo apt install ./samplite-explorer_*.deb` |

The Linux package is built on Ubuntu 24.04 and needs that release or newer.

The installers are not signed with a developer certificate, so Windows SmartScreen or macOS Gatekeeper
may warn. On Windows choose More info > Run anyway; on macOS allow it under
System Settings > Privacy & Security.

To build an installer locally, build in `build/` first, then run `bash installer/linux.sh`,
`bash installer/macos.sh`, or on Windows `ISCC /DAppVersion=1.2.0 installer\windows.iss`
(needs [Inno Setup 6](https://jrsoftware.org/isinfo.php)). Output goes to `dist/`.

## Use

- **Add Folder...** or drop a folder onto the window.
- The left sidebar shows each added folder as a tree. Select a folder to see everything inside it,
  including subfolders. Folders with no sounds or presets are left out.
- Click or use the arrow keys to audition; **Space** plays and stops; click the waveform to seek.
- Select as in Windows Explorer: **Shift** for a range, **Ctrl** (Cmd on macOS) to add or remove single
  files, **Ctrl+Shift** to add a range, **Ctrl+A** for all.
- Right-click for **Open**, **Open Containing Folder**, **Rename** and **Delete**. **Enter** opens,
  **F2** renames, **Delete** moves to the Recycle Bin or Trash (after asking). On Linux the folder opens
  without the file highlighted.
- **Help > About** lists the supported file formats.
- Drag a row out of the window into your DAW, synth or file manager. Files are copied or referenced, never moved.
- Filter by text, type, BPM range and key. Click column headers to sort, including by tuning.

## Notes

- **BPM and key** come from the filename if present, otherwise from analysis. A `~` marks an estimate.
  - Key: Queen Mary key detector (QM-DSP), using the detected tuning. Drums and noise are left blank.
  - Tempo: files up to 30 s use onset autocorrelation; longer files use the Queen Mary beat tracker.
  - Estimates can be wrong, including half or double tempo, and relative major/minor.
- **Tuning** is the A4 reference in Hz (440, 432 and so on), from the filename (`432Hz`) or estimated.
  Estimates cover about 428 to 453 Hz. Noisy or drum material is usually left blank.
- **Presets** are listed with metadata only; they are not played (that would require hosting the synth).
  - `.vital`: name, author, style
  - `.fxp`: program name and plugin ID (Serum 1 is recognised)
  - `.fxb`, `.vstpreset`: filename and plugin/class ID
  - `.SerumPreset`: based on an assumed file layout; falls back to the filename
- **Formats:** WAV, AIFF, FLAC, Ogg and MP3, plus whatever the operating system's codecs add.
- Settings and the analysis cache live in a `SampliteExplorer` folder in your user application data.
- Read `THIRD-PARTY.md` before distributing: QM-DSP is GPL and JUCE is AGPL or commercial.
