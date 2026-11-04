#pragma once
#include <string>

// Identity of this build, generated at build time (cmake/KesBuildInfo.cmake).
const char* BuildInfoVersion();   // KES_PROJECT_VERSION
const char* BuildInfoCommit();    // git describe, with -dirty for local changes
const char* BuildInfoConfig();    // Debug, Release, ...

std::string BuildInfoDescription();   // "kes 1.0.0.0 (db344c6, Release, Windows)"
std::string BuildInfoReleaseName();   // "kes@1.0.0.0+db344c6", the crash report release
