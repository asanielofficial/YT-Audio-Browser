#pragma once

// Search sources yt-dlp has no search for: request building and response
// parsing only. No network and no Windows headers, so it can be tested anywhere.

#include "ytdlp.h"

#include <string>
#include <vector>

namespace ytb {

std::string percentEncode(const std::string &s);

std::string archiveSearchUrl(const std::string &query, int n);
std::string mixcloudSearchUrl(const std::string &query, int n);
std::string bandcampSearchUrl(const std::string &query);

// Each parser appends to `out` and returns false when the body is not the
// expected format (so the caller can say "unexpected response" instead of
// "no results"). An empty but valid result list returns true.
bool parseArchive(const std::string &body, int maxHits, std::vector<Hit> &out);
bool parseMixcloud(const std::string &body, int maxHits, std::vector<Hit> &out);
bool parseBandcamp(const std::string &body, int maxHits, std::vector<Hit> &out);

} // namespace ytb
