#include "../ytdlp.h"

#include "../proc.h"

#include <dlfcn.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace ytb {
namespace {

fs::path homeDir() {
   if (const char *h = std::getenv("HOME"); h && *h)
      return h;
   if (passwd *pw = getpwuid(getuid()); pw && pw->pw_dir)
      return pw->pw_dir;
   return "/tmp";
}

fs::path xdgDir(const char *envName, const char *fallbackUnderHome) {
   if (const char *v = std::getenv(envName); v && *v && v[0] == '/')
      return v;
   return homeDir() / fallbackUnderHome;
}

fs::path moduleDir() {
   Dl_info info{};
   if (dladdr(reinterpret_cast<void *>(&moduleDir), &info) && info.dli_fname)
      return fs::path(info.dli_fname).parent_path();
   return {};
}

bool isExecutable(const fs::path &p) {
   std::error_code ec;
   return !p.empty() && fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}

fs::path findInPath(const char *name) {
   const char *path = std::getenv("PATH");
   if (!path)
      return {};
   std::stringstream ss(path);
   std::string dir;
   while (std::getline(ss, dir, ':')) {
      if (dir.empty())
         continue;
      fs::path p = fs::path(dir) / name;
      if (isExecutable(p))
         return p;
   }
   return {};
}

std::string trim(std::string s) {
   while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
      s.pop_back();
   size_t i = 0;
   while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
      ++i;
   return s.substr(i);
}

// Value of `key` in `[section]` of a simple ini file; "" if absent.
std::string iniValue(const fs::path &file, const std::string &section, const std::string &key) {
   std::ifstream in(file);
   std::string line, cur;
   while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line[0] == ';' || line[0] == '#')
         continue;
      if (line.front() == '[' && line.back() == ']') {
         cur = line.substr(1, line.size() - 2);
         continue;
      }
      size_t eq = line.find('=');
      if (eq != std::string::npos && cur == section && trim(line.substr(0, eq)) == key)
         return trim(line.substr(eq + 1));
   }
   return {};
}

// The folder the installer recorded (settings.ini [Paths] ToolsDir).
fs::path configuredToolsDir() {
   std::string v = iniValue(xdgDir("XDG_CONFIG_HOME", ".config") / "YtAudioBrowser" / "settings.ini",
                            "Paths", "ToolsDir");
   return v.empty() ? fs::path() : fs::path(v);
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

bool locateTools(Tools &out, PathStr &why) {
   std::vector<fs::path> dirs;
   if (auto c = configuredToolsDir(); !c.empty())
      dirs.push_back(c / "bin");
   dirs.push_back(xdgDir("XDG_DATA_HOME", ".local/share") / "YtAudioBrowser" / "bin");
   if (auto m = moduleDir(); !m.empty()) {
      dirs.push_back(m / "tools");
      dirs.push_back(m);
   }

   for (const auto &d : dirs) {
      if (!isExecutable(d / "yt-dlp"))
         continue;
      out = Tools{};
      out.dir = d.string();
      out.ytdlp = (d / "yt-dlp").string();
      if (isExecutable(d / "ffmpeg"))
         out.ffmpegDir = d.string();
      else if (auto f = findInPath("ffmpeg"); !f.empty())
         out.ffmpegDir = f.parent_path().string();
      if (isExecutable(d / "deno"))
         out.deno = (d / "deno").string();
      return true;
   }

   // Last resort: a yt-dlp the distribution installed. It cannot update itself.
   if (auto y = findInPath("yt-dlp"); !y.empty()) {
      out = Tools{};
      out.dir = y.parent_path().string();
      out.ytdlp = y.string();
      if (auto f = findInPath("ffmpeg"); !f.empty())
         out.ffmpegDir = f.parent_path().string();
      if (auto dn = findInPath("deno"); !dn.empty())
         out.deno = dn.string();
      return true;
   }
   why = "yt-dlp not found. Run the installer (or ytaudiobrowser-setup), or put yt-dlp in " +
         (xdgDir("XDG_DATA_HOME", ".local/share") / "YtAudioBrowser" / "bin").string() + ".";
   return false;
}

PathStr downloadDir() {
   fs::path music = homeDir() / "Music";
   // XDG_MUSIC_DIR from ~/.config/user-dirs.dirs, e.g.  XDG_MUSIC_DIR="$HOME/Music"
   std::ifstream in(xdgDir("XDG_CONFIG_HOME", ".config") / "user-dirs.dirs");
   std::string line;
   while (std::getline(in, line)) {
      line = trim(line);
      if (line.rfind("XDG_MUSIC_DIR=", 0) != 0)
         continue;
      std::string v = trim(line.substr(14));
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
         v = v.substr(1, v.size() - 2);
      const std::string home = "$HOME";
      if (v.rfind(home, 0) == 0)
         v = homeDir().string() + v.substr(home.size());
      if (!v.empty() && v[0] == '/')
         music = v;
      break;
   }
   fs::path dir = music / "YtAudioBrowser";
   std::error_code ec;
   fs::create_directories(dir, ec);
   return dir.string();
}

int searchOne(const Tools &t, const std::string &siteLabel, const std::string &target,
              int maxResults, const std::atomic<bool> &cancel, const std::function<void(Hit &&)> &onHit,
              std::string &lastError) {
   std::vector<std::string> args = {
       "--ignore-errors", "--no-warnings", "--no-colors", "--flat-playlist",
       "--playlist-end",  std::to_string(maxResults), "--encoding", "utf-8",
       "--print",
       "RES\t%(id)s\t%(title)s\t%(channel,uploader)s\t%(duration)s\t%(webpage_url,url)s",
       target};

   std::string err;
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
      lastError = err;
   return rc;
}

int downloadAudio(const Tools &t, const std::string &url, const PathStr &format,
                  const std::atomic<bool> &cancel, const std::function<void(double)> &onProgress,
                  PathStr &outPath, std::string &lastError) {
   if (t.ffmpegDir.empty()) {
      lastError = "ffmpeg not found. Install it, or run the installer to download one.";
      return -1;
   }
   const std::string dir = downloadDir();

   std::set<std::string> before;
   {
      std::error_code ec;
      for (auto &e : fs::directory_iterator(dir, ec))
         before.insert(e.path().filename().string());
   }

   std::vector<std::string> args = {
       "--no-playlist", "--no-warnings", "--no-colors", "--newline", "--progress",
       "--encoding", "utf-8",
       "--progress-template", "download:PROG\t%(progress._percent_str)s",
       "--print", "after_move:FILE\t%(filepath)s",
       "--playlist-items", "1",
       "-f", "bestaudio/best", "-x", "--audio-format", format,
       "--ffmpeg-location", t.ffmpegDir,
       "-P", dir, "-o", "%(title).120B [%(id)s].%(ext)s"};
   if (format == "mp3") {
      args.push_back("--audio-quality");
      args.push_back("0");
   }
   if (!t.deno.empty()) {
      args.push_back("--js-runtimes");
      args.push_back("deno:" + t.deno);
   }
   args.push_back(url);

   std::string err;
   outPath.clear();
   int rc = runProcess(
       t.ytdlp, args,
       [&](const std::string &line) {
          if (startsWith(line, "PROG\t")) {
             onProgress(std::atof(line.c_str() + 5));
          } else if (startsWith(line, "FILE\t")) {
             outPath = line.substr(5);
          } else if (startsWith(line, "ERROR:")) {
             lastError = line;
          }
       },
       cancel, err);
   if (rc == -1) {
      lastError = err;
      return rc;
   }
   if (rc != 0)
      return rc;

   std::error_code ec;
   if (outPath.empty() || !fs::exists(outPath, ec)) {
      // Fallback: the one new file with the requested extension.
      outPath.clear();
      for (auto &e : fs::directory_iterator(dir, ec)) {
         if (before.count(e.path().filename().string()))
            continue;
         if (e.path().extension().string() == "." + format)
            outPath = e.path().string();
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
   std::string err;
   int rc = runProcess(t.ytdlp, {"--no-colors", "-U"}, onLine, cancel, err);
   if (rc == -1)
      onLine(err);
   return rc;
}

} // namespace ytb
