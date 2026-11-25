#pragma once

#include "ytdlp.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace ytb {

struct Site {
   enum Kind {
      Prefix,      // yt-dlp "<spec>N:query" search key
      UrlTemplate, // yt-dlp search URL; %Q is replaced by the percent-encoded query
      Http,        // built-in provider (spec: "bandcamp", "mixcloud" or "archive")
   };
   const char *label;
   Kind kind;
   const char *spec;
   bool defaultOn;
};

const std::vector<Site> &sites();

// Searches one site and streams hits through `onHit`. Returns 0 on success
// (even with no hits), -2 if cancelled, anything else on failure with a
// message in `err`.
int searchSite(const Tools &t, const Site &site, const std::string &query, int maxResults,
               const std::atomic<bool> &cancel, const std::function<void(Hit &&)> &onHit,
               std::string &err);

} // namespace ytb
