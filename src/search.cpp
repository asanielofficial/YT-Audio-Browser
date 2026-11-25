#include "search.h"

#include "http.h"
#include "providers.h"

namespace ytb {

const std::vector<Site> &sites() {
   // yt-dlp keys and URL patterns checked against yt-dlp 2026.10.26.
   static const std::vector<Site> s = {
       {"YouTube", Site::Prefix, "ytsearch", true},
       {"YouTube Music", Site::UrlTemplate, "https://music.youtube.com/search?q=%Q#songs", false},
       {"SoundCloud", Site::Prefix, "scsearch", true},
       {"Bandcamp", Site::Http, "bandcamp", false},
       {"Mixcloud", Site::Http, "mixcloud", false},
       {"Internet Archive", Site::Http, "archive", false},
       {"Dailymotion", Site::UrlTemplate, "https://www.dailymotion.com/search/%Q/videos", false},
       {"Niconico", Site::Prefix, "nicosearch", false},
       {"Bilibili", Site::Prefix, "bilisearch", false},
   };
   return s;
}

int searchSite(const Tools &t, const Site &site, const std::string &query, int maxResults,
               const std::atomic<bool> &cancel, const std::function<void(Hit &&)> &onHit,
               std::string &err) {
   if (site.kind == Site::Prefix) {
      std::string target = std::string(site.spec) + std::to_string(maxResults) + ":" + query;
      return searchOne(t, site.label, target, maxResults, cancel, onHit, err);
   }
   if (site.kind == Site::UrlTemplate) {
      std::string target = site.spec;
      size_t p = target.find("%Q");
      if (p != std::string::npos)
         target.replace(p, 2, percentEncode(query));
      return searchOne(t, site.label, target, maxResults, cancel, onHit, err);
   }

   const std::string provider = site.spec;
   std::string url;
   if (provider == "archive")
      url = archiveSearchUrl(query, maxResults);
   else if (provider == "mixcloud")
      url = mixcloudSearchUrl(query, maxResults);
   else if (provider == "bandcamp")
      url = bandcampSearchUrl(query);
   else {
      err = "Unknown provider: " + provider;
      return 1;
   }

   std::string body;
   if (!httpGet(url, cancel, body, err))
      return cancel.load() ? -2 : 1;

   std::vector<Hit> hits;
   bool ok = provider == "archive"    ? parseArchive(body, maxResults, hits)
             : provider == "mixcloud" ? parseMixcloud(body, maxResults, hits)
                                      : parseBandcamp(body, maxResults, hits);
   if (!ok) {
      err = std::string(site.label) + " returned an unexpected response (the site may have changed).";
      return 1;
   }
   for (auto &h : hits)
      onHit(std::move(h));
   return 0;
}

} // namespace ytb
