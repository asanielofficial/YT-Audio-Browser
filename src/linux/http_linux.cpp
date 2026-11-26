#include "../http.h"

#include <curl/curl.h>

#include <mutex>

namespace ytb {
namespace {

constexpr size_t kMaxBody = 4u << 20;

struct Ctx {
   std::string *body;
   const std::atomic<bool> *cancel;
};

size_t onWrite(char *p, size_t size, size_t n, void *user) {
   auto *c = static_cast<Ctx *>(user);
   if (c->body->size() < kMaxBody)
      c->body->append(p, size * n); // keep reading past the cap, but stop storing
   return size * n;
}

int onProgress(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
   return static_cast<Ctx *>(user)->cancel->load() ? 1 : 0; // non-zero aborts the transfer
}

void globalInit() {
   static std::once_flag once;
   std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace

bool httpGet(const std::string &url, const std::atomic<bool> &cancel, std::string &body,
             std::string &err) {
   body.clear();
   globalInit();
   CURL *h = curl_easy_init();
   if (!h) {
      err = "Could not start libcurl.";
      return false;
   }
   Ctx ctx{&body, &cancel};
   curl_slist *headers = curl_slist_append(nullptr, "Accept: application/json, text/html;q=0.9");
   headers = curl_slist_append(headers, "Accept-Language: en");

   char errbuf[CURL_ERROR_SIZE] = "";
   curl_easy_setopt(h, CURLOPT_URL, url.c_str());
   curl_easy_setopt(h, CURLOPT_USERAGENT,
                    "Mozilla/5.0 (X11; Linux x86_64) YtAudioBrowser/1.0");
   curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
   curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
   curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
   curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
   curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
   curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
   curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
   curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
   curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L); // required when called from threads
   curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
   curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
   curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onWrite);
   curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
   curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
   curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onProgress);
   curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);

   CURLcode rc = curl_easy_perform(h);
   long status = 0;
   curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
   curl_slist_free_all(headers);
   curl_easy_cleanup(h);

   if (rc == CURLE_ABORTED_BY_CALLBACK) {
      err = "Cancelled.";
      return false;
   }
   if (rc != CURLE_OK) {
      err = std::string("Request failed: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
      return false;
   }
   if (status < 200 || status >= 300) {
      err = "HTTP " + std::to_string(status);
      return false;
   }
   return true;
}

} // namespace ytb
