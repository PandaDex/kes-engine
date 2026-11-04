#include "test_framework.h"
#include <chrono>
#include <cstdio>
#include <exception>
#include <iterator>
#include <string_view>

namespace kestest
{
    namespace fs = std::filesystem;

    static int g_failures = 0;

    std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> tests;
        return tests;
    }

    void ReportFailure(const char* file, int line, const std::string& message)
    {
        ++g_failures;
        std::printf("  %s:%d: FAILED: %s\n", file, line, message.c_str());
    }

    TempDir::TempDir()
    {
        static uint32_t counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / ("kes_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        fs::create_directories(m_path);
    }

    TempDir::~TempDir()
    {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    std::vector<uint8_t> ReadBytes(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void WriteBytes(const std::string& path, const uint8_t* data, size_t size)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
}

int main(int argc, char** argv)
{
    const std::string_view filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    int failedTests = 0;

    for (const kestest::TestCase& test : kestest::Registry())
    {
        if (std::string_view(test.name).find(filter) == std::string_view::npos)
            continue;

        ++ran;
        const int failuresBefore = kestest::g_failures;
        std::printf("[ RUN ] %s\n", test.name);
        std::fflush(stdout);
        try
        {
            test.run();
        }
        catch (const kestest::AbortTest&)
        {
        }
        catch (const std::exception& error)
        {
            kestest::ReportFailure(test.name, 0, std::string("uncaught exception: ") + error.what());
        }
        if (kestest::g_failures != failuresBefore)
            ++failedTests;
    }

    std::printf("%d test(s) run, %d failed.\n", ran, failedTests);
    return failedTests == 0 && ran > 0 ? 0 : 1;
}
