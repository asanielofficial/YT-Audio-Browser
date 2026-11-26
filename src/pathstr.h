#pragma once

#include <string>

namespace ytb {

// The platform's native path/argument string: UTF-16 on Windows, UTF-8 elsewhere.
#ifdef _WIN32
using PathStr = std::wstring;
#else
using PathStr = std::string;
#endif

} // namespace ytb
