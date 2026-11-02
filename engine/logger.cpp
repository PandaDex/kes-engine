#include "logger.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "engine_types.h"
#include "profiler.h"

namespace
{
    namespace fs = std::filesystem;

    constexpr uintmax_t kMaxLogDirectorySizeBytes = 50ull * 1024ull * 1024ull;
    constexpr size_t kCleanupBatchSize = 3;

    enum class LogSource
    {
        Engine,
        Game
    };

    struct LogFileInfo
    {
        fs::path path;
        fs::file_time_type lastWriteTime = fs::file_time_type::min();
        uintmax_t size = 0;
    };

    struct CleanupResult
    {
        size_t removedCount = 0;
        uintmax_t removedBytes = 0;
        uintmax_t remainingBytes = 0;
    };

    struct LoggerState
    {
        std::mutex mutex;
        std::FILE* file = nullptr;
        fs::path directory;
        fs::path currentFile;
        bool initialized = false;
    };

    LoggerState g_logger;

    std::tm GetLocalTime()
    {
        const std::time_t now = std::time(nullptr);
        std::tm localTime{};

#if PLATFORM_WINDOWS
        localtime_s(&localTime, &now);
#elif PLATFORM_LINUX
        localtime_r(&now, &localTime);
#endif

        return localTime;
    }

    std::string FormatFilenameTimestamp(const std::tm& localTime)
    {
        char buffer[64] = {};

        std::snprintf(
            buffer,
            sizeof(buffer),
            "log_%02d-%02d-%04d-%02d-%02d-%02d.txt",
            localTime.tm_mday,
            localTime.tm_mon + 1,
            localTime.tm_year + 1900,
            localTime.tm_hour,
            localTime.tm_min,
            localTime.tm_sec);

        return std::string(buffer);
    }

    std::string FormatLinePrefix(LogSource source)
    {
        const std::tm localTime = GetLocalTime();
        char buffer[64] = {};

        std::snprintf(
            buffer,
            sizeof(buffer),
            "[%02d:%02d:%02d][%s] ",
            localTime.tm_hour,
            localTime.tm_min,
            localTime.tm_sec,
            source == LogSource::Engine ? "Engine" : "Game");

        return std::string(buffer);
    }

    std::string FormatStringV(const char* fmt, va_list args)
    {
        if (!fmt)
            return {};

        va_list argsCopy;
        va_copy(argsCopy, args);

        const int length = std::vsnprintf(nullptr, 0, fmt, argsCopy);

        va_end(argsCopy);

        if (length <= 0)
            return {};

        std::vector<char> buffer(static_cast<size_t>(length) + 1u);

        va_list argsCopy2;
        va_copy(argsCopy2, args);

        std::vsnprintf(
            buffer.data(),
            buffer.size(),
            fmt,
            argsCopy2);

        va_end(argsCopy2);

        return std::string(buffer.data(), static_cast<size_t>(length));
    }

    void CloseCurrentFileNoLock()
    {
        if (g_logger.file)
        {
            std::fclose(g_logger.file);
            g_logger.file = nullptr;
        }

        g_logger.currentFile.clear();
        g_logger.initialized = false;
    }

    void WriteLineNoLock(LogSource source, const std::string& line)
    {
        const std::string prefix = FormatLinePrefix(source);
        const std::string output = prefix + line + '\n';

        std::fwrite(output.data(), 1, output.size(), stdout);
        std::fflush(stdout);

        if (g_logger.file)
        {
            std::fwrite(output.data(), 1, output.size(), g_logger.file);
            std::fflush(g_logger.file);
        }
    }

    void WriteMessageNoLock(LogSource source, const std::string& message)
    {
        size_t start = 0;

        while (start <= message.size())
        {
            const size_t end = message.find('\n', start);

            std::string line = message.substr(
                start,
                end == std::string::npos
                    ? std::string::npos
                    : end - start);

            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            WriteLineNoLock(source, line);

            if (end == std::string::npos)
                break;

            start = end + 1;
        }
    }

    CleanupResult CleanupLogsNoLock(const fs::path& logDirectory)
    {
        CleanupResult result{};
        std::error_code error;

        if (!fs::exists(logDirectory, error) || error)
            return result;

        std::vector<LogFileInfo> files;

        fs::directory_iterator it(logDirectory, error);
        fs::directory_iterator end;

        while (!error && it != end)
        {
            const fs::directory_entry& entry = *it;
            std::error_code entryError;

            if (entry.is_regular_file(entryError) && !entryError)
            {
                const uintmax_t size = entry.file_size(entryError);

                if (!entryError)
                {
                    LogFileInfo info;
                    info.path = entry.path();
                    info.size = size;
                    info.lastWriteTime = entry.last_write_time(entryError);

                    if (entryError)
                        info.lastWriteTime = fs::file_time_type::min();

                    files.push_back(info);
                    result.remainingBytes += size;
                }
            }

            it.increment(error);
        }

        if (result.remainingBytes < kMaxLogDirectorySizeBytes)
            return result;

        std::sort(
            files.begin(),
            files.end(),
            [](const LogFileInfo& a, const LogFileInfo& b)
            {
                if (a.lastWriteTime == b.lastWriteTime)
                    return a.path.filename().string() <
                           b.path.filename().string();

                return a.lastWriteTime < b.lastWriteTime;
            });

        size_t removedInBatch = 0;

        for (const LogFileInfo& file : files)
        {
            if (result.remainingBytes < kMaxLogDirectorySizeBytes &&
                removedInBatch > 0)
            {
                break;
            }

            std::error_code removeError;

            if (!fs::remove(file.path, removeError) || removeError)
                continue;

            result.remainingBytes =
                result.remainingBytes >= file.size
                    ? result.remainingBytes - file.size
                    : 0;

            result.removedBytes += file.size;
            ++result.removedCount;
            ++removedInBatch;

            if (removedInBatch >= kCleanupBatchSize &&
                result.remainingBytes < kMaxLogDirectorySizeBytes)
            {
                break;
            }

            if (removedInBatch >= kCleanupBatchSize &&
                result.remainingBytes >= kMaxLogDirectorySizeBytes)
            {
                removedInBatch = 0;
            }
        }

        return result;
    }

    void WriteMessage(LogSource source, const std::string& message)
    {
        // Log lines also show up in Tracy's message list, on the timeline.
        KES_PROFILE_MESSAGE(message.data(), message.size());
        KES_ZONE(IO, "Log write");
        std::lock_guard<std::mutex> lock(g_logger.mutex);
        WriteMessageNoLock(source, message);
    }
}

bool LoggerInitialize(String executablePath)
{
    std::lock_guard<std::mutex> lock(g_logger.mutex);

    CloseCurrentFileNoLock();

    if (executablePath.empty())
        return false;

    const fs::path executableDirectory(executablePath.c_str());

    if (executableDirectory.empty())
        return false;

    g_logger.directory = executableDirectory / "logs";

    std::error_code error;

    fs::create_directories(g_logger.directory, error);

    if (error)
        return false;

    const CleanupResult cleanup =
        CleanupLogsNoLock(g_logger.directory);

    const std::tm startupTime = GetLocalTime();

    g_logger.currentFile =
        g_logger.directory / FormatFilenameTimestamp(startupTime);

#if PLATFORM_WINDOWS
    if (fopen_s(
            &g_logger.file,
            g_logger.currentFile.string().c_str(),
            "a") != 0)
    {
        g_logger.file = nullptr;
    }
#elif PLATFORM_LINUX
    g_logger.file = std::fopen(
        g_logger.currentFile.string().c_str(),
        "a");
#endif

    if (!g_logger.file)
    {
        g_logger.currentFile.clear();
        return false;
    }

    g_logger.initialized = true;

    WriteMessageNoLock(
        LogSource::Engine,
        "Logger initialized.");

    WriteMessageNoLock(
        LogSource::Engine,
        "Active log file: " +
        g_logger.currentFile.string());

    if (cleanup.removedCount > 0)
    {
        WriteMessageNoLock(
            LogSource::Engine,
            "Log retention removed " +
            std::to_string(cleanup.removedCount) +
            " old file(s), freed " +
            std::to_string(cleanup.removedBytes) +
            " bytes. Remaining log size: " +
            std::to_string(cleanup.remainingBytes) +
            " bytes.");
    }
    else
    {
        WriteMessageNoLock(
            LogSource::Engine,
            "Log retention check complete. Current log directory size: " +
            std::to_string(cleanup.remainingBytes) +
            " bytes.");
    }

    return true;
}

void LoggerShutdown()
{
    std::lock_guard<std::mutex> lock(g_logger.mutex);
    CloseCurrentFileNoLock();
}

void EngineLog(const std::string& message)
{
    WriteMessage(LogSource::Engine, message);
}

void EngineLogf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    const std::string message = FormatStringV(fmt, args);

    va_end(args);

    WriteMessage(LogSource::Engine, message);
}

void GameLog(const std::string& message)
{
    WriteMessage(LogSource::Game, message);
}

void GameLogf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    const std::string message = FormatStringV(fmt, args);

    va_end(args);

    WriteMessage(LogSource::Game, message);
}

std::string GetCurrentLogFilePath()
{
    std::lock_guard<std::mutex> lock(g_logger.mutex);
    return g_logger.currentFile.string();
}
