#pragma once
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace kestest
{
    struct TestCase
    {
        const char* name;
        void (*run)();
    };

    struct AbortTest
    {
    };

    std::vector<TestCase>& Registry();
    void ReportFailure(const char* file, int line, const std::string& message);

    struct Registrar
    {
        Registrar(const char* name, void (*run)()) { Registry().push_back({ name, run }); }
    };

    class TempDir
    {
    public:
        TempDir();
        ~TempDir();
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& Root() const { return m_path; }
        std::filesystem::path Path(const char* name) const { return m_path / name; }
        std::string File(const char* name) const { return Path(name).string(); }

    private:
        std::filesystem::path m_path;
    };

    std::vector<uint8_t> ReadBytes(const std::string& path);
    void WriteBytes(const std::string& path, const uint8_t* data, size_t size);
    inline void WriteBytes(const std::string& path, const std::vector<uint8_t>& data) { WriteBytes(path, data.data(), data.size()); }
}

#define KES_TEST_CONCAT_(a, b) a##b
#define KES_TEST_CONCAT(a, b) KES_TEST_CONCAT_(a, b)
#define KES_TEST_CASE_(fn, name)                                               \
    static void fn();                                                          \
    static const kestest::Registrar KES_TEST_CONCAT(fn, _registrar)(name, fn); \
    static void fn()
#define TEST_CASE(name) KES_TEST_CASE_(KES_TEST_CONCAT(kes_test_, __COUNTER__), name)

#define CHECK(expr)                                            \
    do                                                         \
    {                                                          \
        if (!(expr))                                           \
            kestest::ReportFailure(__FILE__, __LINE__, #expr); \
    } while (false)

#define REQUIRE(expr)                                          \
    do                                                         \
    {                                                          \
        if (!(expr))                                           \
        {                                                      \
            kestest::ReportFailure(__FILE__, __LINE__, #expr); \
            throw kestest::AbortTest{};                        \
        }                                                      \
    } while (false)

#define CHECK_NEAR(a, b, epsilon) CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (epsilon))
