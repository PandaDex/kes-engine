#pragma once
#include <string>

// Crash capture through sentry-native + Crashpad (docs/crash-reporting.md).
// Every function is a no-op when the reporter is compiled out or not running,
// so a missing crashpad_handler never stops the game.

struct CrashReporterConfig
{
    std::string databaseDirectory;   // writable, UTF-8; minidumps land under it
    std::string logFilePath;         // attached to uploaded reports, may be empty
};

bool CrashReporterInitialize(const CrashReporterConfig& config);
void CrashReporterShutdown();

// True if the previous run crashed. Reported once per crash.
bool CrashReporterCrashedLastRun();

// Reports only leave the machine with a DSN and the player's consent.
void CrashReporterSetUploadConsent(bool allowed);

void CrashReporterSetTag(const char* key, const char* value);

// For failures that end the game without crashing it, such as a lost GPU.
void CrashReporterReportFatal(const std::string& message);
