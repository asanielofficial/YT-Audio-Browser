#pragma once

#include <atomic>
#include <string>

namespace ytb {

// HTTPS GET through WinHTTP (system proxy settings apply). Body is capped at
// 4 MB. Returns true only for a 2xx response; otherwise `err` says why.
bool httpGet(const std::string &url, const std::atomic<bool> &cancel, std::string &body,
             std::string &err);

} // namespace ytb
