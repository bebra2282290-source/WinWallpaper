#pragma once

#include <string>

// Optional diagnostic log (off by default). Enabled with the --log command
// line switch or Log=1 in the settings file; written next to the settings.
namespace Log {

void Enable(const std::wstring& path);
bool Enabled();
void Write(const char* format, ...);
std::string Utf8(const std::wstring& text);

} // namespace Log
