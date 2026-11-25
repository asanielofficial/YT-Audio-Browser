#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace ytb {

// Runs `exe` with `args` and delivers merged stdout+stderr line by line (UTF-8).
// Blocks until the process ends or `cancel` becomes true.
// Returns the exit code, -1 if the process could not start (`err` is filled),
// or -2 if it was cancelled. The child and anything it spawns are killed when
// the call ends early or when the host process exits.
int runProcess(const std::wstring &exe, const std::vector<std::wstring> &args,
               const std::function<void(const std::string &)> &onLine,
               const std::atomic<bool> &cancel, std::wstring &err);

} // namespace ytb
