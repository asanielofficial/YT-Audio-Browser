#include "proc.h"

#include <windows.h>

#include <algorithm>

namespace ytb {
namespace {

std::wstring quoteArg(const std::wstring &a) {
   if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
      return a;
   std::wstring r = L"\"";
   size_t backslashes = 0;
   for (wchar_t c : a) {
      if (c == L'\\') {
         ++backslashes;
      } else if (c == L'"') {
         r.append(backslashes * 2 + 1, L'\\');
         r.push_back(L'"');
         backslashes = 0;
      } else {
         r.append(backslashes, L'\\');
         backslashes = 0;
         r.push_back(c);
      }
   }
   r.append(backslashes * 2, L'\\');
   r.push_back(L'"');
   return r;
}

// Copy of the host environment plus UTF-8 forcing for the Python-based tools.
// Built as a block so the DAW's own environment is never modified.
std::wstring buildEnvBlock() {
   std::wstring blk;
   if (wchar_t *env = GetEnvironmentStringsW()) {
      for (const wchar_t *p = env; *p;) {
         size_t len = wcslen(p);
         blk.append(p, len + 1);
         p += len + 1;
      }
      FreeEnvironmentStringsW(env);
   }
   for (const wchar_t *extra : {L"PYTHONUTF8=1", L"PYTHONIOENCODING=utf-8"}) {
      blk += extra;
      blk.push_back(L'\0');
   }
   blk.push_back(L'\0');
   return blk;
}

struct Handle {
   HANDLE h = nullptr;
   ~Handle() { reset(); }
   void reset() {
      if (h && h != INVALID_HANDLE_VALUE)
         CloseHandle(h);
      h = nullptr;
   }
};

} // namespace

int runProcess(const std::wstring &exe, const std::vector<std::wstring> &args,
               const std::function<void(const std::string &)> &onLine,
               const std::atomic<bool> &cancel, std::wstring &err) {
   SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

   Handle rd, wr, nul;
   if (!CreatePipe(&rd.h, &wr.h, &sa, 0)) {
      err = L"CreatePipe failed";
      return -1;
   }
   SetHandleInformation(rd.h, HANDLE_FLAG_INHERIT, 0);
   nul.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, 0, nullptr);

   std::wstring cmd = quoteArg(exe);
   for (const auto &a : args) {
      cmd.push_back(L' ');
      cmd += quoteArg(a);
   }

   STARTUPINFOW si{};
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
   si.wShowWindow = SW_HIDE;
   si.hStdInput = nul.h;
   si.hStdOutput = wr.h;
   si.hStdError = wr.h;

   std::wstring env = buildEnvBlock();
   PROCESS_INFORMATION pi{};
   if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                       env.data(), nullptr, &si, &pi)) {
      err = L"Could not start " + exe + L" (error " + std::to_wstring(GetLastError()) + L")";
      return -1;
   }
   Handle proc, thread;
   proc.h = pi.hProcess;
   thread.h = pi.hThread;
   wr.reset(); // parent's copy of the write end, so EOF can be seen

   // One job per run: closing it kills yt-dlp and any ffmpeg it started.
   Handle job;
   job.h = CreateJobObjectW(nullptr, nullptr);
   if (job.h) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &li, sizeof(li));
      AssignProcessToJobObject(job.h, proc.h);
   }
   ResumeThread(thread.h);

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
      DWORD avail = 0;
      if (!PeekNamedPipe(rd.h, nullptr, 0, nullptr, &avail, nullptr))
         break;
      if (avail) {
         DWORD n = 0;
         if (!ReadFile(rd.h, tmp, std::min<DWORD>(avail, sizeof(tmp)), &n, nullptr) || !n)
            break;
         buf.append(tmp, n);
         flushLines();
         continue;
      }
      if (cancel.load()) {
         cancelled = true;
         break;
      }
      if (WaitForSingleObject(proc.h, 15) == WAIT_OBJECT_0) {
         DWORD left = 0;
         if (!PeekNamedPipe(rd.h, nullptr, 0, nullptr, &left, nullptr) || left == 0)
            break;
      }
   }
   if (!buf.empty())
      onLine(buf);

   if (cancelled) {
      job.reset(); // kills the whole tree
      return -2;
   }
   WaitForSingleObject(proc.h, 5000);
   DWORD code = 0;
   GetExitCodeProcess(proc.h, &code);
   return (int)code;
}

} // namespace ytb
