// Crashes a child process on purpose and checks that the real crash handler
// wrote a minidump and that the next run notices. Needs no GPU or window.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include "crash_reporter.h"
#include "file_paths.h"

namespace fs = std::filesystem;

namespace
{
    int RunSelf(const std::string& exe, const char* mode, const fs::path& database)
    {
        std::string command = "\"" + exe + "\" " + mode + " \"" + database.string() + "\"";
#if PLATFORM_WINDOWS
        command = "\"" + command + "\"";   // cmd.exe strips the outer pair
#endif
        return std::system(command.c_str());
    }

    bool HasMinidump(const fs::path& database)
    {
        std::error_code error;
        for (fs::recursive_directory_iterator it(database, error), end; !error && it != end; it.increment(error))
        {
            if (it->path().extension() == ".dmp" && it->file_size(error) > 0)
                return true;
        }
        return false;
    }

    bool StartReporter(const char* database)
    {
        CrashReporterConfig config;
        config.databaseDirectory = database;
        return CrashReporterInitialize(config);
    }
}

int main(int argc, char* argv[])
{
    if (argc == 3 && std::strcmp(argv[1], "--crash") == 0)
    {
        if (!StartReporter(argv[2]))
            return 3;
        volatile int* volatile target = nullptr;
        *target = 1;
        return 4;
    }
    if (argc == 3 && std::strcmp(argv[1], "--check") == 0)
    {
        if (!StartReporter(argv[2]))
            return 3;
        const bool crashed = CrashReporterCrashedLastRun();
        CrashReporterShutdown();
        return crashed ? 0 : 5;
    }

    const fs::path database = fs::temp_directory_path() / "kes_crash_smoke";
    std::error_code error;
    fs::remove_all(database, error);
    fs::create_directories(database);

    const std::string exe = (GetExecutableDirectory() / fs::path(argv[0]).filename()).string();
    const int crashResult = RunSelf(exe, "--crash", database);
    std::printf("crashing child exited with %d\n", crashResult);

    bool found = false;
    for (int i = 0; i < 300 && !found; ++i)
    {
        found = HasMinidump(database);
        if (!found)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!found)
    {
        std::printf("FAILED: no minidump under %s\n", database.string().c_str());
        return 1;
    }

    const int checkResult = RunSelf(exe, "--check", database);
    if (checkResult != 0)
    {
        std::printf("FAILED: the next run did not report the crash (%d)\n", checkResult);
        return 1;
    }

    fs::remove_all(database, error);
    std::printf("minidump written and crash detected on the next run\n");
    return 0;
}
