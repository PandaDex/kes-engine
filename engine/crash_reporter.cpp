#include "crash_reporter.h"

#if KES_CRASH_REPORTER

#include <filesystem>
#include <sentry.h>
#include "build_info.h"
#include "file_paths.h"
#include "logger.h"

namespace
{
    namespace fs = std::filesystem;

    bool g_active = false;

#if PLATFORM_WINDOWS
    constexpr const wchar_t* kHandlerName = L"crashpad_handler.exe";
#else
    constexpr const char* kHandlerName = "crashpad_handler";
#endif

    // The narrow sentry functions use the ANSI code page on Windows, which
    // can't hold every user folder name.
    void SetPaths(sentry_options_t* options, const fs::path& database, const fs::path& handler, const fs::path& logFile)
    {
#if PLATFORM_WINDOWS
        sentry_options_set_database_pathw(options, database.c_str());
        sentry_options_set_handler_pathw(options, handler.c_str());
        if (!logFile.empty())
            sentry_options_add_attachmentw(options, logFile.c_str());
#else
        sentry_options_set_database_path(options, database.c_str());
        sentry_options_set_handler_path(options, handler.c_str());
        if (!logFile.empty())
            sentry_options_add_attachment(options, logFile.c_str());
#endif
    }
}

bool CrashReporterInitialize(const CrashReporterConfig& config)
{
    const fs::path handler = GetExecutableDirectory() / kHandlerName;
    std::error_code error;
    if (!fs::exists(handler, error))
    {
        EngineLogf("Crash reporter off: %s not found.", handler.string().c_str());
        return false;
    }

    const std::string& utf8Directory = config.databaseDirectory;
    const fs::path database(std::u8string(utf8Directory.begin(), utf8Directory.end()));

    sentry_options_t* options = sentry_options_new();
    if (sizeof(KES_CRASH_DSN) > 1)
        sentry_options_set_dsn(options, KES_CRASH_DSN);
    const std::string release = BuildInfoReleaseName();
    sentry_options_set_release(options, release.c_str());
    sentry_options_set_environment(options, BuildInfoConfig());
    sentry_options_set_require_user_consent(options, 1);
    sentry_options_set_auto_session_tracking(options, 0);
    SetPaths(options, database, handler, fs::path(config.logFilePath));

    // sentry_init takes ownership of options, even on failure.
    if (sentry_init(options) != 0)
    {
        EngineLog("Crash reporter failed to start.");
        return false;
    }

    g_active = true;
    EngineLogf("Crash reporter running, minidumps in %s.", database.string().c_str());
    return true;
}

void CrashReporterShutdown()
{
    if (!g_active)
        return;
    sentry_close();
    g_active = false;
}

bool CrashReporterCrashedLastRun()
{
    return g_active && sentry_get_crashed_last_run() == 1;
}

void CrashReporterSetUploadConsent(bool allowed)
{
    if (!g_active)
        return;
    if (allowed)
        sentry_user_consent_give();
    else
        sentry_user_consent_revoke();
}

void CrashReporterSetTag(const char* key, const char* value)
{
    if (g_active)
        sentry_set_tag(key, value);
}

void CrashReporterReportFatal(const std::string& message)
{
    if (g_active)
        sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_FATAL, "engine", message.c_str()));
}

#else

bool CrashReporterInitialize(const CrashReporterConfig&) { return false; }
void CrashReporterShutdown() {}
bool CrashReporterCrashedLastRun() { return false; }
void CrashReporterSetUploadConsent(bool) {}
void CrashReporterSetTag(const char*, const char*) {}
void CrashReporterReportFatal(const std::string&) {}

#endif
