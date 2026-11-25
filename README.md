# YT Audio Browser (CLAP, Windows)

A CLAP effect plugin whose window searches several sites through yt-dlp, downloads
the audio, and lets you drag the file straight into your DAW. Audio passes through
the plugin unchanged; it exists for its window.

## Build

Needs Visual Studio 2022 (or any C++17 compiler) and CMake 3.20+. CLAP headers and
clap-helpers are fetched by CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\scripts\install.ps1
```

`install.ps1` copies `YtAudioBrowser.clap` to `%LOCALAPPDATA%\Programs\Common\CLAP`
and runs `fetch-tools.ps1` if the tools are missing. Rescan plugins in the DAW.

## Installer

`scripts\build-installer.ps1` builds the plugin, downloads the tools and produces
`dist\YtAudioBrowser-Setup.exe` with NSIS (`winget install NSIS.NSIS`); add `-Slim` for a
small installer that downloads the tools when it runs. Per-user, no admin needed.

The installer has two folder pages:

- **Tools folder** (default `%LOCALAPPDATA%\YtAudioBrowser`): yt-dlp, ffmpeg, deno in its
  `bin` subfolder, plus the uninstaller. Pick a folder you can write to; yt-dlp updates
  itself there. The choice is saved under `HKCU\Software\YtAudioBrowser\ToolsDir`, which
  is how the plugin finds the tools.
- **Plugin folder** (default `%LOCALAPPDATA%\Programs\Common\CLAP`): if you change it,
  add the new folder to your DAW's CLAP search paths.

Reinstalling starts from the folders you chose last time. Silent install:
`Setup.exe /S /PLUGINDIR=D:\Plugins\CLAP /D=D:\Tools\YtAudioBrowser` (`/D=` must be last).

Uninstall from Apps & features; it removes only the files it installed, and downloaded
audio is kept. Close your DAW before reinstalling, since a loaded .clap is locked.

## Building on GitHub

`.github/workflows/build.yml` builds on a Windows runner with MSVC and uploads the plugin
and both installers as artifacts on every push. The first run is the first time this code
is built with MSVC (it was only cross-compiled with MinGW), so expect to fix a compile
error or two.

## Tools

`fetch-tools.ps1` puts yt-dlp.exe, ffmpeg.exe, ffprobe.exe and deno.exe in
`%LOCALAPPDATA%\YtAudioBrowser\bin`. The plugin also looks in a `tools` folder next to
the .clap file. YouTube needs yt-dlp plus a JavaScript runtime (deno), and sites change
often, so use the **Update yt-dlp** button when a search or download starts failing.

## Use

- Type a query and press Enter. Ticked sites are searched in parallel. YouTube and
  SoundCloud are on by default; tick more as needed:
  - Through yt-dlp: YouTube, YouTube Music, SoundCloud, Dailymotion, Niconico, Bilibili.
  - Built in (the plugin queries them itself, then yt-dlp downloads): Bandcamp (tracks
    only), Mixcloud, Internet Archive (audio items).
  - To add a yt-dlp site, add a row to `sites()` in `src/search.cpp`: a `<key>search`
    prefix or a search-page URL that yt-dlp has an extractor for (`yt-dlp --list-extractors`).
- Paste any URL yt-dlp supports to list or download it (this is how you reach Bandcamp,
  Vimeo and the rest; yt-dlp has no search prefix for them).
- **Refresh** (next to Stop) re-runs the last search with the same sites and re-reads the
  downloads folder, so files added or removed outside the plugin show up.
- Click a column header to sort either list (ascending, then descending, then back to the
  default order: results as found, downloads newest first). The arrow shows the sorted
  column. Text sorts case-insensitively with numbers in order; unknown lengths always sort last.
- The plugin remembers ticked sites, format, both sort orders and column widths in
  `%LOCALAPPDATA%\YtAudioBrowser\settings.ini`. The last query is not remembered.
- Both lists work like Explorer: click, Shift+click for a range, Ctrl+click to add or
  remove single rows, Ctrl+A for all.
- Results: double-click or **Download selected** downloads every selected result, one
  after another, in the format picked at the top (flac by default). Right-click (or the
  menu key) for **Copy link(s)** and **Open in browser**; Ctrl+C also copies links.
- Downloads (bottom list): drag the selection into your DAW. Double-click or right-click
  > **Open** opens files in your default audio app; right-click > **Delete** (or the
  Delete key) moves them to the Recycle Bin.
- Files land in `%USERPROFILE%\Music\YtAudioBrowser`.

## Status and limits

- Compiles and links as a Windows x64 DLL exporting `clap_entry` (cross-compiled with
  zig/mingw for the check). **Not run inside a DAW and not run against live sites**: the
  build sandbox had no access to them. Expect small fixes on first run.
- The three built-in searches (Bandcamp, Mixcloud, Internet Archive) parse sample
  responses correctly in tests but were not run against the live sites. Bandcamp reads
  its search page HTML, so it breaks whenever Bandcamp changes the page; if a site fails
  the status line names it while the others still show results.
- Internet Archive items with several files download only the first file.
- yt-dlp has no search for Spotify, Apple Music or Deezer, and they are DRM-protected,
  so they are not supported.
- Embedded window only (no floating window), fixed size, classic control styling.
- Drag-out uses a standard Windows file drag. Hosts differ in whether they accept it
  from a plugin window; Reaper and Bitwig are the likeliest to work.
- Some hosts intercept keyboard input in plugin windows, which can swallow typing in
  the search box.
- Download and conversion progress comes from yt-dlp's output; if it changes its output
  format the bar may stop moving while the download still works.
- Downloading content you do not have the rights to use is your responsibility.
- Bundled tools keep their own licenses (yt-dlp: Unlicense, ffmpeg LGPL build, deno: MIT).
  Check them before redistributing the plugin with the tools included.
