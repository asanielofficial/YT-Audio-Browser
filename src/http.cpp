#include "http.h"

#include "util.h"

#include <windows.h>
#include <winhttp.h>

namespace ytb {
namespace {

struct Internet {
   HINTERNET h = nullptr;
   Internet() = default;
   explicit Internet(HINTERNET x) : h(x) {}
   Internet(const Internet &) = delete;
   Internet &operator=(const Internet &) = delete;
   ~Internet() {
      if (h)
         WinHttpCloseHandle(h);
   }
};

std::string winError(const char *what) {
   return std::string(what) + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

} // namespace

bool httpGet(const std::string &url, const std::atomic<bool> &cancel, std::string &body,
             std::string &err) {
   body.clear();
   std::wstring wurl = widen(url);

   URL_COMPONENTS uc{};
   uc.dwStructSize = sizeof(uc);
   wchar_t host[256], path[2048];
   uc.lpszHostName = host;
   uc.dwHostNameLength = 256;
   uc.lpszUrlPath = path;
   uc.dwUrlPathLength = 2048;
   wchar_t extra[2048];
   uc.lpszExtraInfo = extra;
   uc.dwExtraInfoLength = 2048;
   if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
      err = winError("Bad URL");
      return false;
   }
   std::wstring target = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                         std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
   const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

   Internet session(WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) YtAudioBrowser/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0));
   if (!session.h) {
      err = winError("WinHttpOpen failed");
      return false;
   }
   WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 15000);

   Internet conn(WinHttpConnect(session.h, std::wstring(uc.lpszHostName, uc.dwHostNameLength).c_str(),
                                uc.nPort, 0));
   if (!conn.h) {
      err = winError("Could not connect");
      return false;
   }
   Internet req(WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
   if (!req.h) {
      err = winError("Could not open request");
      return false;
   }
   static const wchar_t *headers = L"Accept: application/json, text/html;q=0.9\r\nAccept-Language: en\r\n";
   if (!WinHttpSendRequest(req.h, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
       !WinHttpReceiveResponse(req.h, nullptr)) {
      err = winError("Request failed");
      return false;
   }

   DWORD status = 0, size = sizeof(status);
   WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
   if (status < 200 || status >= 300) {
      err = "HTTP " + std::to_string(status) + " from " + narrow(std::wstring(uc.lpszHostName, uc.dwHostNameLength));
      return false;
   }

   constexpr size_t kMaxBody = 4u << 20;
   char buf[8192];
   for (;;) {
      if (cancel.load()) {
         err = "Cancelled.";
         return false;
      }
      DWORD n = 0;
      if (!WinHttpReadData(req.h, buf, sizeof(buf), &n)) {
         err = winError("Read failed");
         return false;
      }
      if (n == 0)
         break;
      body.append(buf, n);
      if (body.size() > kMaxBody)
         break;
   }
   return true;
}

} // namespace ytb
