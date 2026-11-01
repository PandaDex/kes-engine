#pragma once
#include <string>
#include <string_view>
#include <vector>

// Process command line, stored once at startup (EngineSetCommandLine).
//
// Options are matched case-insensitively and may start with '-' or '--':
//   -windowed            CommandLineHasFlag("windowed")
//   -ResX=1280           CommandLineGetValue("ResX", out)
//   -renderer d3d11      CommandLineGetValue("renderer", out) (value as next argument)

void CommandLineInit(int argc, const char* const* argv);
const std::vector<std::string>& CommandLineGetArguments();

bool CommandLineHasFlag(std::string_view name);
bool CommandLineGetValue(std::string_view name, std::string& outValue);
bool CommandLineGetInt(std::string_view name, int& outValue);
bool CommandLineGetFloat(std::string_view name, float& outValue);
