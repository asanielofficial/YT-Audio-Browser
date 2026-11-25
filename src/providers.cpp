#include "providers.h"

#include "json.h"

#include <cctype>
#include <cstdlib>

namespace ytb {
namespace {

std::string replaceLuceneSpecials(const std::string &q) {
   std::string r;
   for (char c : q) {
      if (std::string("+-&|!(){}[]^\"~*?:\\/").find(c) != std::string::npos)
         r += ' ';
      else
         r += c;
   }
   return r;
}

std::string collapse(const std::string &s) {
   std::string r;
   bool space = true;
   for (unsigned char c : s) {
      if (std::isspace(c)) {
         if (!space)
            r += ' ';
         space = true;
      } else {
         r += (char)c;
         space = false;
      }
   }
   while (!r.empty() && r.back() == ' ')
      r.pop_back();
   return r;
}

void appendUtf8(std::string &o, unsigned cp) {
   if (cp < 0x80) {
      o += (char)cp;
   } else if (cp < 0x800) {
      o += (char)(0xC0 | (cp >> 6));
      o += (char)(0x80 | (cp & 0x3F));
   } else if (cp < 0x10000) {
      o += (char)(0xE0 | (cp >> 12));
      o += (char)(0x80 | ((cp >> 6) & 0x3F));
      o += (char)(0x80 | (cp & 0x3F));
   } else {
      o += (char)(0xF0 | (cp >> 18));
      o += (char)(0x80 | ((cp >> 12) & 0x3F));
      o += (char)(0x80 | ((cp >> 6) & 0x3F));
      o += (char)(0x80 | (cp & 0x3F));
   }
}

unsigned namedEntity(const std::string &e) {
   static const struct { const char *n; unsigned cp; } t[] = {
       {"eacute", 0xE9}, {"egrave", 0xE8}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"agrave", 0xE0},
       {"aacute", 0xE1}, {"acirc", 0xE2}, {"auml", 0xE4}, {"aring", 0xE5}, {"ccedil", 0xE7},
       {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"ntilde", 0xF1}, {"oacute", 0xF3},
       {"ocirc", 0xF4}, {"ouml", 0xF6}, {"uacute", 0xFA}, {"ucirc", 0xFB}, {"uuml", 0xFC},
       {"Eacute", 0xC9}, {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"szlig", 0xDF},
       {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"lsquo", 0x2018},
       {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"middot", 0xB7}, {"copy", 0xA9}};
   for (const auto &x : t)
      if (e == x.n)
         return x.cp;
   return 0;
}

std::string htmlDecode(const std::string &s) {
   std::string r;
   for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] != '&') {
         r += s[i];
         continue;
      }
      size_t semi = s.find(';', i);
      if (semi == std::string::npos || semi - i > 10) {
         r += s[i];
         continue;
      }
      std::string ent = s.substr(i + 1, semi - i - 1);
      if (ent == "amp") r += '&';
      else if (ent == "lt") r += '<';
      else if (ent == "gt") r += '>';
      else if (ent == "quot") r += '"';
      else if (ent == "apos" || ent == "#39") r += '\'';
      else if (ent == "nbsp") r += ' ';
      else if (unsigned named = namedEntity(ent)) appendUtf8(r, named);
      else if (!ent.empty() && ent[0] == '#') {
         unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                           ? (unsigned)std::strtoul(ent.c_str() + 2, nullptr, 16)
                           : (unsigned)std::strtoul(ent.c_str() + 1, nullptr, 10);
         if (cp)
            appendUtf8(r, cp);
         else {
            r += s[i];
            continue;
         }
      } else {
         r += s[i];
         continue;
      }
      i = semi;
   }
   return r;
}

std::string stripTags(const std::string &s) {
   std::string r;
   bool inTag = false;
   for (char c : s) {
      if (c == '<')
         inTag = true;
      else if (c == '>')
         inTag = false;
      else if (!inTag)
         r += c;
   }
   return r;
}

std::string cleanText(const std::string &html) { return collapse(htmlDecode(stripTags(html))); }

// Text between the first `marker` at/after `from` and the next `endMarker`.
bool between(const std::string &s, size_t from, const char *marker, const char *endMarker,
             std::string &out, size_t *after = nullptr) {
   size_t a = s.find(marker, from);
   if (a == std::string::npos)
      return false;
   a += std::string(marker).size();
   size_t b = s.find(endMarker, a);
   if (b == std::string::npos)
      return false;
   out = s.substr(a, b - a);
   if (after)
      *after = b;
   return true;
}

} // namespace

std::string percentEncode(const std::string &s) {
   static const char *hex = "0123456789ABCDEF";
   std::string r;
   for (unsigned char c : s) {
      if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
         r += (char)c;
      } else {
         r += '%';
         r += hex[c >> 4];
         r += hex[c & 15];
      }
   }
   return r;
}

std::string archiveSearchUrl(const std::string &query, int n) {
   std::string q = collapse(replaceLuceneSpecials(query));
   return "https://archive.org/advancedsearch.php?q=" + percentEncode("(" + q + ") AND mediatype:audio") +
          "&fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D=creator&rows=" + std::to_string(n) +
          "&page=1&output=json";
}

std::string mixcloudSearchUrl(const std::string &query, int n) {
   return "https://api.mixcloud.com/search/?type=cloud&limit=" + std::to_string(n) +
          "&q=" + percentEncode(query);
}

std::string bandcampSearchUrl(const std::string &query) {
   return "https://bandcamp.com/search?item_type=t&q=" + percentEncode(query);
}

bool parseArchive(const std::string &body, int maxHits, std::vector<Hit> &out) {
   Json root;
   if (!JsonParser::parse(body, root))
      return false;
   const Json *resp = root.get("response");
   const Json *docs = resp ? resp->get("docs") : nullptr;
   if (!docs || docs->type != Json::Arr)
      return false;
   int count = 0;
   for (const Json &d : docs->a) {
      if (count >= maxHits)
         break;
      std::string id = d.str("identifier");
      if (id.empty())
         continue;
      Hit h;
      h.site = "Internet Archive";
      h.id = id;
      h.title = d.str("title");
      if (h.title.empty())
         h.title = id;
      h.uploader = d.str("creator");
      h.url = "https://archive.org/details/" + id;
      out.push_back(std::move(h));
      ++count;
   }
   return true;
}

bool parseMixcloud(const std::string &body, int maxHits, std::vector<Hit> &out) {
   Json root;
   if (!JsonParser::parse(body, root))
      return false;
   const Json *data = root.get("data");
   if (!data || data->type != Json::Arr)
      return false;
   int count = 0;
   for (const Json &d : data->a) {
      if (count >= maxHits)
         break;
      std::string url = d.str("url");
      if (url.empty()) {
         std::string key = d.str("key");
         if (key.empty())
            continue;
         url = "https://www.mixcloud.com" + key;
      }
      Hit h;
      h.site = "Mixcloud";
      h.id = d.str("slug");
      h.title = d.str("name");
      if (h.title.empty())
         h.title = url;
      if (const Json *u = d.get("user"))
         h.uploader = u->str("name");
      h.durationSec = (int)d.num("audio_length", 0);
      h.url = url;
      out.push_back(std::move(h));
      ++count;
   }
   return true;
}

bool parseBandcamp(const std::string &body, int maxHits, std::vector<Hit> &out) {
   const std::string marker = "class=\"searchresult";
   size_t pos = body.find(marker);
   if (pos == std::string::npos)
      // A valid page with no matches has a results container and no items.
      return body.find("result-items") != std::string::npos ||
             body.find("searchresults") != std::string::npos;
   int count = 0;
   while (pos != std::string::npos && count < maxHits) {
      size_t next = body.find(marker, pos + marker.size());
      std::string block = body.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
      pos = next;

      std::string type;
      if (!between(block, 0, "class=\"itemtype\">", "<", type))
         continue;
      type = cleanText(type);
      for (auto &c : type)
         c = (char)std::tolower((unsigned char)c);
      if (type != "track") // albums are playlists; the plugin saves one file per download
         continue;

      std::string href, title;
      size_t h = block.find("class=\"heading\"");
      if (h == std::string::npos)
         continue;
      size_t after = 0;
      if (!between(block, h, "href=\"", "\"", href, &after))
         continue;
      size_t gt = block.find('>', after);
      size_t end = block.find("</a>", gt == std::string::npos ? after : gt);
      if (gt == std::string::npos || end == std::string::npos)
         continue;
      title = cleanText(block.substr(gt + 1, end - gt - 1));
      href = htmlDecode(href);
      size_t q = href.find('?');
      if (q != std::string::npos)
         href.resize(q);
      if (href.rfind("http", 0) != 0 || title.empty())
         continue;

      std::string sub, artist;
      if (between(block, 0, "class=\"subhead\">", "</div>", sub)) {
         artist = cleanText(sub);
         size_t by = artist.rfind(" by ");
         if (by != std::string::npos)
            artist = artist.substr(by + 4);
         else if (artist.rfind("by ", 0) == 0)
            artist = artist.substr(3);
      }

      Hit hit;
      hit.site = "Bandcamp";
      hit.id = href;
      hit.title = title;
      hit.uploader = artist;
      hit.url = href;
      out.push_back(std::move(hit));
      ++count;
   }
   return true;
}

} // namespace ytb
