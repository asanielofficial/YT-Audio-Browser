#pragma once

#include "pathstr.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace ytb {

struct Hit {
   std::string site;
   std::string id;
   std::string title;
   std::string uploader;
   std::string url;
   int durationSec = 0;
};

struct Tools {
   PathStr dir;       // folder the tools were found in
   PathStr ytdlp;     // full path to yt-dlp
   PathStr ffmpegDir; // folder holding ffmpeg (empty if not found)
   PathStr deno;      // full path to deno (empty if not found)
};

// Finds yt-dlp (see the platform's ytdlp source for the search order).
// On failure `why` explains where to put it.
bool locateTools(Tools &out, PathStr &why);

// Where downloads go (Music\YtAudioBrowser), created on demand.
PathStr downloadDir();

// Runs one yt-dlp search (or lists a pasted URL). `target` is either
// "ytsearch8:query" or a URL. At most `maxResults` hits are listed.
// Returns the yt-dlp exit code (-2 if cancelled).
int searchOne(const Tools &t, const std::string &siteLabel, const std::string &target,
              int maxResults, const std::atomic<bool> &cancel, const std::function<void(Hit &&)> &onHit,
              std::string &lastError);

// Downloads `url` as audio in `format` (wav, flac, mp3, m4a, opus).
// `outPath` receives the final file path on success.
int downloadAudio(const Tools &t, const std::string &url, const PathStr &format,
                  const std::atomic<bool> &cancel, const std::function<void(double)> &onProgress,
                  PathStr &outPath, std::string &lastError);

// yt-dlp -U, streaming its output.
int updateYtDlp(const Tools &t, const std::atomic<bool> &cancel,
                const std::function<void(const std::string &)> &onLine);

} // namespace ytb
