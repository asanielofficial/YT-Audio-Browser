# YT Audio Browser

A Windows CLAP plugin that searches YouTube, SoundCloud, Bandcamp and more through
[yt-dlp](https://github.com/yt-dlp/yt-dlp), downloads audio in your chosen format, and lets you
drag the files straight into your DAW.

## Download

**[Get the latest release](../../releases/latest)** (currently 0.1.0, an early release).

Pick one installer from the release page:

- `YtAudioBrowser-Setup-Slim.exe` is small and downloads yt-dlp, ffmpeg and deno (about 150 MB)
  while installing.
- `YtAudioBrowser-Setup.exe` has the tools bundled and installs offline.

Then rescan plugins in your DAW and add **YT Audio Browser** to a track. Audio passes through
the plugin unchanged; it exists for its window.

Requires Windows 10 or 11 (64-bit), a DAW that hosts CLAP plugins with embedded Windows GUIs,
and an internet connection.

## Features

- **Search nine sites at once:** YouTube, YouTube Music, SoundCloud, Bandcamp (tracks),
  Mixcloud, Internet Archive, Dailymotion, Niconico and Bilibili. Paste any URL yt-dlp
  supports to list or download it.
- **Download as flac (default), wav, mp3, m4a or opus.** Select several results and they
  download one after another into `Music\YtAudioBrowser`.
- **Drag into your DAW** from the Downloads list, one file or many.
- **Explorer-style lists:** Shift/Ctrl selection, Ctrl+A, Ctrl+C, Enter, Delete, right-click
  menus (Copy link, Open in browser, Open, Delete to the Recycle Bin).
- **Click any column header to sort** ascending, descending, then back to default.
- **Remembers** ticked sites, format, sort orders and column widths.
- **Refresh** re-runs the last search, **Stop** cancels, **Update yt-dlp** fixes most
  "site stopped working" problems.

## Using it

1. Type a query and press Enter. YouTube and SoundCloud are ticked by default; tick more sites
   as needed.
2. Select one or more results and press **Download selected** (or double-click one).
3. Drag the finished file from the Downloads list into your arrangement or sampler.

Right-click a result for **Copy link** and **Open in browser**. Right-click a download for
**Open** (your default audio app) and **Delete**.

## Installing

No admin rights needed. The installer asks for two folders:

- **Tools folder** (default `%LOCALAPPDATA%\YtAudioBrowser`): yt-dlp, ffmpeg, deno and the
  uninstaller. yt-dlp updates itself here, so pick a folder you can write to.
- **Plugin folder** (default `%LOCALAPPDATA%\Programs\Common\CLAP`, which most DAWs already scan):
  if you change it, add the new folder to your DAW's CLAP search paths.

Reinstalling starts from the folders you chose last time. Silent install:
`Setup.exe /S /PLUGINDIR=D:\Plugins\CLAP /D=D:\Tools\YtAudioBrowser` (`/D=` must be last).

Close your DAW before reinstalling, since a loaded plugin file is locked. Uninstall from
Apps & features; your downloaded audio is kept.

## Known limitations

- Early release: built and checked against sample data, but not yet tested across DAWs or every
  site. Expect rough edges.
- Windows only. The window is embedded only and fixed size.
- Dragging files out depends on your DAW. Some hosts do not accept a file drag from a plugin
  window, and some capture keystrokes so typing in the search box may not work. If a drag fails,
  use **Open folder** and drag from there.
- Bandcamp search reads Bandcamp's web page, so it breaks when the layout changes. If a site
  fails, the status line names it and the others keep working.
- Internet Archive items with several files download only the first file.
- No preview before download and no search filters beyond sorting.
- Spotify, Apple Music and Deezer are not supported: yt-dlp cannot search them and their audio
  is DRM-protected.
- Sites change often. If downloads start failing, press **Update yt-dlp** first.

## Legal

You are responsible for having the right to download and use what you fetch. Many sites restrict
downloading in their terms, and most music is copyrighted. Internet Archive and Creative Commons
material is the safest source for anything you plan to release.

The installer includes yt-dlp (Unlicense), an LGPL build of ffmpeg and deno (MIT), each under its
own license. Check them before redistributing the plugin with the tools included.

## Building from source

Needs Visual Studio 2022 (or any C++17 compiler) and CMake 3.20+. CLAP headers and clap-helpers
are fetched by CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\scripts\install.ps1          # copies the plugin to your CLAP folder and fetches the tools
```

To build the installers, install NSIS (`winget install NSIS.NSIS`) and run:

```powershell
.\scripts\build-installer.ps1          # dist\YtAudioBrowser-Setup.exe (tools bundled)
.\scripts\build-installer.ps1 -Slim    # dist\YtAudioBrowser-Setup-Slim.exe
```

`.github/workflows/build.yml` does the same on a Windows runner and uploads the plugin and both
installers as artifacts on every push.

### How it works

- The plugin looks for `yt-dlp.exe` in the folder the installer recorded under
  `HKCU\Software\YtAudioBrowser\ToolsDir`, then `%LOCALAPPDATA%\YtAudioBrowser\bin`, then a
  `tools` folder next to the `.clap`. `scripts\fetch-tools.ps1` downloads the tools.
- YouTube needs yt-dlp plus a JavaScript runtime (deno).
- Settings live in `%LOCALAPPDATA%\YtAudioBrowser\settings.ini`.
- Sites are defined in `sites()` in `src/search.cpp`. yt-dlp sites are a `<key>search` prefix or
  a search-page URL that yt-dlp has an extractor for (`yt-dlp --list-extractors`). Bandcamp,
  Mixcloud and Internet Archive have built-in providers in `src/providers.cpp`.
