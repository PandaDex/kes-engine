#include "test_framework.h"
#include <algorithm>
#include "pak_file_system.h"

namespace
{
    namespace fs = std::filesystem;

    struct PakFixture
    {
        kestest::TempDir dir;
        std::string source = dir.File("source");
        std::string pak = dir.File("test.pak");
        std::vector<uint8_t> hello = { 'h', 'e', 'l', 'l', 'o' };
        std::vector<uint8_t> binary = { 0, 1, 2, 3, 255, 254 };

        PakFixture()
        {
            fs::create_directories(fs::path(source) / "nested");
            kestest::WriteBytes((fs::path(source) / "hello.txt").string(), hello);
            kestest::WriteBytes((fs::path(source) / "nested" / "data.bin").string(), binary);
        }

        ~PakFixture()
        {
            ClearMountedPakFiles();
            SetPakEncryptionKeyOverride("");
        }
    };
}

TEST_CASE("pak: extract returns the packed files")
{
    PakFixture fixture;
    std::string error;
    REQUIRE(CreatePakFile(fixture.source.c_str(), fixture.pak.c_str(), nullptr, &error));

    const fs::path output = fixture.dir.Path("extracted");
    REQUIRE(ExtractPakFile(fixture.pak.c_str(), output.string().c_str(), &error));
    CHECK(kestest::ReadBytes((output / "hello.txt").string()) == fixture.hello);
    CHECK(kestest::ReadBytes((output / "nested" / "data.bin").string()) == fixture.binary);
}

TEST_CASE("pak: mounted files load by asset path")
{
    PakFixture fixture;
    REQUIRE(CreatePakFile(fixture.source.c_str(), fixture.pak.c_str(), nullptr));

    std::string error;
    REQUIRE(MountPakFile(fixture.pak.c_str(), &error));
    CHECK(GetMountedPakFileCount() == 1);

    std::vector<uint8_t> bytes;
    CHECK(LoadAssetFileBytes("nested/data.bin", bytes, &error));
    CHECK(bytes == fixture.binary);
    CHECK(!LoadAssetFileBytes("nested/missing.bin", bytes));

    ClearMountedPakFiles();
    CHECK(GetMountedPakFileCount() == 0);
    CHECK(!LoadAssetFileBytes("nested/data.bin", bytes));
}

TEST_CASE("pak: encrypted files need the right key")
{
    PakFixture fixture;
    const std::string key = GeneratePakEncryptionKeyHex();
    REQUIRE(key.size() == 64);
    REQUIRE(CreatePakFile(fixture.source.c_str(), fixture.pak.c_str(), key.c_str()));

    const std::vector<uint8_t> stored = kestest::ReadBytes(fixture.pak);
    CHECK(std::search(stored.begin(), stored.end(), fixture.hello.begin(), fixture.hello.end()) == stored.end());

    SetPakEncryptionKeyOverride(GeneratePakEncryptionKeyHex().c_str());
    CHECK(!MountPakFile(fixture.pak.c_str()));

    SetPakEncryptionKeyOverride(key.c_str());
    std::string error;
    REQUIRE(MountPakFile(fixture.pak.c_str(), &error));

    std::vector<uint8_t> bytes;
    CHECK(LoadAssetFileBytes("hello.txt", bytes, &error));
    CHECK(bytes == fixture.hello);
}

TEST_CASE("pak: rejects damaged headers and missing sources")
{
    PakFixture fixture;
    REQUIRE(CreatePakFile(fixture.source.c_str(), fixture.pak.c_str(), nullptr));

    std::vector<uint8_t> damaged = kestest::ReadBytes(fixture.pak);
    REQUIRE(!damaged.empty());
    damaged[0] ^= 0xFF;
    kestest::WriteBytes(fixture.pak, damaged);
    CHECK(!MountPakFile(fixture.pak.c_str()));

    const std::string missing = fixture.dir.File("missing");
    CHECK(!CreatePakFile(missing.c_str(), fixture.pak.c_str(), nullptr));
    CHECK(!MountPakFile(missing.c_str()));
}
