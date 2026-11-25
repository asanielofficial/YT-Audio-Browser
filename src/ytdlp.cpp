#include "ytdlp.h"

#include "proc.h"
#include "util.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ytb {
namespace {

fs::path moduleDir() {
   HMODULE h = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&moduleDir), &h);
   wchar_t buf[MAX_PATH * 2];
   DWORD n = GetModuleFileNameW(h, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
   return fs::path(std::wstring(buf, n)).parent_path();
}

std::wstring envVar(const wchar_t *name) {
   wchar_t buf[MAX_PATH * 2];
   DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
   return n && n < sizeof(buf) / sizeof(buf[0]) ? std::wstring(buf, n) : std::wstring();
}

fs::path userToolsDir() {
   std::wstring base = envVar(L"LOCALAPPDATA");
   return base.empty() ? fs::path() : fs::path(base) / L"YtAudioBrowser" / L"bin";
}

// ToolsDir written by the installer (HKCU\\Software\\YtAudioBrowser).
fs::path registeredToolsDir() {
   wchar_t buf[MAX_PATH * 2];
   DWORD size = sizeof(buf);
   if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\YtAudioBrowser", L"ToolsDir", RRF_RT_REG_SZ,
                    nullptr, buf, &size) != ERROR_SUCCESS)
      return {};
   return fs::path(buf);
}

bool isFile(const fs::path &p) {
   std::error_code ec;
   return !p.empty() && fs::is_regular_file(p, ec);
}

std::vector<std::string> splitTabs(const std::string &s) {
   std::vector<std::string> out;
   size_t start = 0;
   for (;;) {
      size_t p = s.find('\t', start);
      if (p == std::string::npos) {
         out.push_back(s.substr(start));
         break;
      }
      out.push_back(s.substr(start, p - start));
      start = p + 1;
   }
   return out;
}

bool startsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

} // namespace

bool locateTools(Tools &out, std::wstring &why) {
   std::vector<fs::path> dirs;
   // Folder chosen in the installer, if any.
   if (auto r = registeredToolsDir(); !r.empty())
      dirs.push_back(r / L"bin");
   if (auto u = userToolsDir(); !u.empty())
      dirs.push_back(u);
   dirs.push_back(moduleDir() / L"tools");
   dirs.push_back(moduleDir());

   for (const auto &d : dirs) {
      if (!isFile(d / L"yt-dlp.exe"))
         continue;
      out = Tools{};
      out.dir = d.wstring();
      out.ytdlp = (d / L"yt-dlp.exe").wstring();
      if (isFile(d / L"ffmpeg.exe")) {
         out.ffmpegDir = d.wstring();
      } else {
         wchar_t found[MAX_PATH * 2];
         if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)(sizeof(found) / sizeof(found[0])),
                         found, nullptr))
            out.ffmpegDir = fs::path(found).parent_path().wstring();
      }
      if (isFile(d / L"deno.exe"))
         out.deno = (d / L"deno.exe").wstring();
      return true;
   }
   fs::path target = userToolsDir();
   why = L"yt-dlp.exe not found. Re-run the installer, or run scripts\\fetch-tools.ps1 (installs to " +
         target.wstring() + L").";
   return false;
}

std::wstring downloadDir() {
   std::wstring profile = envVar(L"USERPROFILE");
   fs::path dir = fs::path(profile.empty() ? L"." : profile) / L"Music" / L"YtAudioBrowser";
   std::error_code ec;
   fs::create_directories(dir, ec);
   return dir.wstring();
}

int searchOne(const Tools &t, const std::string &siteLabel, const std::string &target,
              int maxResults, const std::atomic<bool> &cancel, const std::function<void(Hit &&)> &onHit,
              std::string &lastError) {
   std::vector<std::wstring> args = {
       L"--ignore-errors", L"--no-warnings", L"--no-colors", L"--flat-playlist",
       L"--playlist-end",  std::to_wstring(maxResults), L"--encoding",  L"utf-8",
       L"--print",
       L"RES\t%(id)s\t%(title)s\t%(channel,uploader)s\t%(duration)s\t%(webpage_url,url)s",
       widen(target)};

   std::wstring err;
   int rc = runProcess(
       t.ytdlp, args,
       [&](const std::string &line) {
          if (startsWith(line, "RES\t")) {
             auto f = splitTabs(line);
             if (f.size() != 6 || f[5].empty() || f[5] == "NA")
                return;
             Hit h;
             h.site = siteLabel;
             h.id = f[1];
             h.title = f[2];
             h.uploader = f[3] == "NA" ? std::string() : f[3];
             h.durationSec = f[4] == "NA" ? 0 : (int)std::atof(f[4].c_str());
             h.url = f[5];
             onHit(std::move(h));
          } else if (startsWith(line, "ERROR:")) {
             lastError = line;
          }
       },
       cancel, err);
   if (rc == -1)
      lastError = narrow(err);
   return rc;
}

int downloadAudio(const Tools &t, const std::string &url, const std::wstring &format,
                  const std::atomic<bool> &cancel, const std::function<void(double)> &onProgress,
                  std::wstring &outPath, std::string &lastError) {
   if (t.ffmpegDir.empty()) {
      lastError = "ffmpeg.exe not found next to yt-dlp.exe. Run scripts\\fetch-tools.ps1.";
      return -1;
   }
   const std::wstring dir = downloadDir();

   std::set<std::wstring> before;
   {
      std::error_code ec;
      for (auto &e : fs::directory_iterator(dir, ec))
         before.insert(e.path().filename().wstring());
   }

   std::vector<std::wstring> args = {
       L"--no-playlist", L"--no-warnings", L"--no-colors", L"--newline", L"--progress",
       L"--windows-filenames", L"--encoding", L"utf-8",
       L"--progress-template", L"download:PROG\t%(progress._percent_str)s",
       L"--print", L"after_move:FILE\t%(filepath)s",
       L"--playlist-items", L"1",
       L"-f", L"bestaudio/best", L"-x", L"--audio-format", format,
       L"--ffmpeg-location", t.ffmpegDir,
       L"-P", dir, L"-o", L"%(title).120B [%(id)s].%(ext)s"};
   if (format == L"mp3") {
      args.push_back(L"--audio-quality");
      args.push_back(L"0");
   }
   if (!t.deno.empty()) {
      args.push_back(L"--js-runtimes");
      args.push_back(L"deno:" + t.deno);
   }
   args.push_back(widen(url));

   std::wstring err;
   outPath.clear();
   int rc = runProcess(
       t.ytdlp, args,
       [&](const std::string &line) {
          if (startsWith(line, "PROG\t")) {
             onProgress(std::atof(line.c_str() + 5));
          } else if (startsWith(line, "FILE\t")) {
             outPath = widen(line.substr(5));
          } else if (startsWith(line, "ERROR:")) {
             lastError = line;
          }
       },
       cancel, err);
   if (rc == -1) {
      lastError = narrow(err);
      return rc;
   }
   if (rc != 0)
      return rc;

   std::error_code ec;
   if (outPath.empty() || !fs::exists(outPath, ec)) {
      // Fallback: the one new file with the requested extension.
      outPath.clear();
      for (auto &e : fs::directory_iterator(dir, ec)) {
         if (before.count(e.path().filename().wstring()))
            continue;
         if (e.path().extension().wstring() == L"." + format)
            outPath = e.path().wstring();
      }
   }
   if (outPath.empty()) {
      lastError = "Download finished but the output file was not found.";
      return 1;
   }
   return 0;
}

int updateYtDlp(const Tools &t, const std::atomic<bool> &cancel,
                const std::function<void(const std::string &)> &onLine) {
   std::wstring err;
   int rc = runProcess(t.ytdlp, {L"--no-colors", L"-U"}, onLine, cancel, err);
   if (rc == -1)
      onLine(narrow(err));
   return rc;
}

} // namespace ytb
