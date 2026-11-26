#include "../proc.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

extern char **environ;

namespace ytb {
namespace {

// The host's environment, minus variables that break the standalone tools
// (a DAW often sets LD_LIBRARY_PATH to its own bundled libraries), plus UTF-8
// forcing for the Python-based tools. The host's own environment is not touched.
std::vector<std::string> buildEnv() {
   static const char *const drop[] = {"LD_LIBRARY_PATH=", "LD_PRELOAD=", "PYTHONUTF8=",
                                      "PYTHONIOENCODING=", "PYTHONHOME=", "PYTHONPATH="};
   std::vector<std::string> env;
   for (char **e = environ; e && *e; ++e) {
      bool skip = false;
      for (const char *d : drop)
         if (std::strncmp(*e, d, std::strlen(d)) == 0)
            skip = true;
      if (!skip)
         env.emplace_back(*e);
   }
   env.emplace_back("PYTHONUTF8=1");
   env.emplace_back("PYTHONIOENCODING=utf-8");
   return env;
}

void killGroup(pid_t pid, int sig) { ::kill(-pid, sig); }

} // namespace

int runProcess(const PathStr &exe, const std::vector<PathStr> &args,
               const std::function<void(const std::string &)> &onLine,
               const std::atomic<bool> &cancel, PathStr &err) {
   int fds[2];
   if (pipe2(fds, O_CLOEXEC) != 0) {
      err = std::string("Could not create a pipe: ") + std::strerror(errno);
      return -1;
   }

   posix_spawn_file_actions_t fa;
   posix_spawn_file_actions_init(&fa);
   posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
   posix_spawn_file_actions_adddup2(&fa, fds[1], 1); // dup2 clears close-on-exec
   posix_spawn_file_actions_adddup2(&fa, fds[1], 2);

   // Own process group, so cancelling kills yt-dlp and the ffmpeg it started.
   posix_spawnattr_t at;
   posix_spawnattr_init(&at);
   posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
   posix_spawnattr_setpgroup(&at, 0);

   std::vector<std::string> argvStr;
   argvStr.push_back(exe);
   argvStr.insert(argvStr.end(), args.begin(), args.end());
   std::vector<char *> argv;
   for (auto &a : argvStr)
      argv.push_back(a.data());
   argv.push_back(nullptr);

   std::vector<std::string> envStr = buildEnv();
   std::vector<char *> envp;
   for (auto &e : envStr)
      envp.push_back(e.data());
   envp.push_back(nullptr);

   pid_t pid = 0;
   int rc = posix_spawn(&pid, exe.c_str(), &fa, &at, argv.data(), envp.data());
   posix_spawn_file_actions_destroy(&fa);
   posix_spawnattr_destroy(&at);
   ::close(fds[1]);
   if (rc != 0) {
      ::close(fds[0]);
      err = "Could not start " + exe + ": " + std::strerror(rc);
      return -1;
   }

   std::string buf;
   char tmp[4096];
   bool cancelled = false;
   auto flushLines = [&]() {
      size_t pos;
      while ((pos = buf.find('\n')) != std::string::npos) {
         std::string line = buf.substr(0, pos);
         if (!line.empty() && line.back() == '\r')
            line.pop_back();
         buf.erase(0, pos + 1);
         onLine(line);
      }
   };

   for (;;) {
      if (cancel.load()) {
         cancelled = true;
         break;
      }
      pollfd p{fds[0], POLLIN, 0};
      int pr = ::poll(&p, 1, 15);
      if (pr < 0) {
         if (errno == EINTR)
            continue;
         break;
      }
      if (pr == 0)
         continue;
      if (p.revents & POLLIN) {
         ssize_t n = ::read(fds[0], tmp, sizeof(tmp));
         if (n > 0) {
            buf.append(tmp, (size_t)n);
            flushLines();
            continue;
         }
         if (n < 0 && errno == EINTR)
            continue;
         break; // EOF or error
      }
      if (p.revents & (POLLHUP | POLLERR))
         break;
   }
   ::close(fds[0]);
   if (!buf.empty() && !cancelled)
      onLine(buf);

   if (cancelled) {
      killGroup(pid, SIGTERM);
      for (int i = 0; i < 50; ++i) { // up to 0.5 s to exit cleanly
         if (::waitpid(pid, nullptr, WNOHANG) != 0)
            return -2;
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      killGroup(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
      return -2;
   }

   int status = 0;
   for (int i = 0; i < 500; ++i) { // the pipe closed; give the process 5 s to finish
      pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid)
         return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if (w < 0 && errno != EINTR)
         return 1;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
   }
   killGroup(pid, SIGKILL);
   ::waitpid(pid, &status, 0);
   return 1;
}

} // namespace ytb
