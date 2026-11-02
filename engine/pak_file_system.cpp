#include "pak_file_system.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "engine_configuration.h"
#include "file_paths.h"
#include "profiler.h"

namespace
{
    namespace fs = std::filesystem;

    constexpr uint32_t kPakMagic = 0x4B41504Bu; // KPAK
    constexpr uint32_t kPakIndexMagic = 0x58444E49u; // INDX
    constexpr uint32_t kPakVersion = 1u;
    constexpr uint32_t kPakFlagEncrypted = 1u << 0;
    constexpr size_t kPakEncryptionKeySize = 32u;
    constexpr size_t kPakNonceSize = 12u;
    constexpr uint64_t kPakHeaderSize = 40u;

    using PakEncryptionKey = std::array<uint8_t, kPakEncryptionKeySize>;
    using PakNonce = std::array<uint8_t, kPakNonceSize>;

    struct MountedPakEntry
    {
        std::string path;
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    struct MountedPakFile
    {
        std::string pakPath;
        bool encrypted = false;
        PakEncryptionKey key = {};
        PakNonce nonce = {};
        std::vector<MountedPakEntry> entries;
    };

    struct MountedPakLocation
    {
        size_t pakIndex = 0;
        size_t entryIndex = 0;
    };

    std::vector<MountedPakFile> gMountedPaks;
    std::unordered_map<std::string, MountedPakLocation> gMountedPakLookup;
    std::string gPakEncryptionKeyOverride;

    template<typename T>
    bool ReadValue(std::istream& stream, T& value)
    {
        stream.read(reinterpret_cast<char*>(&value), sizeof(T));
        return stream.good();
    }

    template<typename T>
    bool WriteValue(std::ostream& stream, const T& value)
    {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return stream.good();
    }

    bool ReadString(std::istream& stream, std::string& value)
    {
        uint32_t length = 0;
        if (!ReadValue(stream, length))
            return false;

        value.resize(length);
        if (length > 0)
            stream.read(value.data(), length);
        return stream.good();
    }

    bool WriteString(std::ostream& stream, const std::string& value)
    {
        const uint32_t length = static_cast<uint32_t>(value.size());
        if (!WriteValue(stream, length))
            return false;

        if (length > 0)
            stream.write(value.data(), length);
        return stream.good();
    }

    bool PathComponentEquals(const fs::path& a, const fs::path& b)
    {
        const std::string left = a.generic_string();
        const std::string right = b.generic_string();
        if (left.size() != right.size())
            return false;

        for (size_t index = 0; index < left.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>(left[index])) !=
                std::tolower(static_cast<unsigned char>(right[index])))
            {
                return false;
            }
        }

        return true;
    }

    bool PathStartsWith(const fs::path& path, const fs::path& prefix)
    {
        auto pathPart = path.begin();
        auto prefixPart = prefix.begin();
        for (; prefixPart != prefix.end(); ++prefixPart, ++pathPart)
        {
            if (pathPart == path.end() || !PathComponentEquals(*pathPart, *prefixPart))
                return false;
        }

        return true;
    }

    fs::path StripPathPrefix(const fs::path& path, const fs::path& prefix)
    {
        fs::path stripped;
        auto pathPart = path.begin();
        auto prefixPart = prefix.begin();
        while (pathPart != path.end() && prefixPart != prefix.end())
        {
            ++pathPart;
            ++prefixPart;
        }

        for (; pathPart != path.end(); ++pathPart)
            stripped /= *pathPart;
        return stripped;
    }

    fs::path GetConfiguredContentDirectory()
    {
        return fs::path(ENGINE_CONTENT_DIRECTORY).lexically_normal();
    }

    fs::path GetConfiguredContentRoot()
    {
        const std::string overridePath = GetAssetRootOverride();
        if (!overridePath.empty())
            return fs::path(overridePath).lexically_normal();

        const fs::path contentDirectory = GetConfiguredContentDirectory();
        if (contentDirectory.is_absolute())
            return contentDirectory;

        const fs::path executableDirectory = GetExecutableDirectory();
        if (executableDirectory.empty())
            return {};
        if (contentDirectory.empty() || contentDirectory == ".")
            return executableDirectory;
        return (executableDirectory / contentDirectory).lexically_normal();
    }

    fs::path NormalizeAssetRelativePath(const fs::path& path)
    {
        const fs::path normalized = path.lexically_normal();
        const fs::path contentDirectory = GetConfiguredContentDirectory();
        if (!contentDirectory.empty() && PathStartsWith(normalized, contentDirectory))
            return StripPathPrefix(normalized, contentDirectory);

        const fs::path legacyContentRoot = fs::path("game") / contentDirectory;
        if (!contentDirectory.empty() && PathStartsWith(normalized, legacyContentRoot))
            return StripPathPrefix(normalized, legacyContentRoot);

        return normalized;
    }

    std::string NormalizePakEntryPath(const fs::path& path)
    {
        const fs::path normalized = path.lexically_normal();
        std::string generic = normalized.generic_string();
        while (!generic.empty() && generic.front() == '/')
            generic.erase(generic.begin());
        while (generic.size() >= 2 && generic[0] == '.' && generic[1] == '/')
            generic.erase(0, 2);
        std::transform(generic.begin(), generic.end(), generic.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        return generic;
    }

    std::string BuildPakLookupPath(const char* path)
    {
        if (!path || !path[0])
            return {};

        const fs::path requested(path);
        if (requested.is_absolute())
        {
            const fs::path normalized = requested.lexically_normal();
            const fs::path contentRoot = GetConfiguredContentRoot();
            if (!contentRoot.empty() && PathStartsWith(normalized, contentRoot))
                return NormalizePakEntryPath(StripPathPrefix(normalized, contentRoot));
            return {};
        }

        return NormalizePakEntryPath(NormalizeAssetRelativePath(requested));
    }

    bool LoadDiskFileBytes(const fs::path& path, std::vector<uint8_t>& outData)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size < 0)
            return false;

        outData.resize(static_cast<size_t>(size));
        if (size > 0)
            file.read(reinterpret_cast<char*>(outData.data()), size);
        return file.good() || file.eof();
    }

    bool ParseHexNibble(char ch, uint8_t& outValue)
    {
        if (ch >= '0' && ch <= '9')
        {
            outValue = static_cast<uint8_t>(ch - '0');
            return true;
        }
        if (ch >= 'a' && ch <= 'f')
        {
            outValue = static_cast<uint8_t>(10 + (ch - 'a'));
            return true;
        }
        if (ch >= 'A' && ch <= 'F')
        {
            outValue = static_cast<uint8_t>(10 + (ch - 'A'));
            return true;
        }
        return false;
    }

    bool ParsePakEncryptionKeyHex(std::string_view hexValue, PakEncryptionKey& outKey, std::string* outError)
    {
        outKey.fill(0);
        if (hexValue.empty())
            return true;

        if (hexValue.size() != kPakEncryptionKeySize * 2u)
        {
            if (outError)
                *outError = "Encryption key must be 64 hex characters";
            return false;
        }

        for (size_t index = 0; index < outKey.size(); ++index)
        {
            uint8_t high = 0;
            uint8_t low = 0;
            if (!ParseHexNibble(hexValue[index * 2u], high) || !ParseHexNibble(hexValue[index * 2u + 1u], low))
            {
                if (outError)
                    *outError = "Encryption key contains invalid hex digits";
                return false;
            }
            outKey[index] = static_cast<uint8_t>((high << 4u) | low);
        }

        return true;
    }

    std::string GetEffectivePakEncryptionKeyHex()
    {
        if (!gPakEncryptionKeyOverride.empty())
            return gPakEncryptionKeyOverride;
        return ENGINE_PAK_ENCRYPTION_KEY_HEX;
    }

    uint32_t LoadU32LE(const uint8_t* data)
    {
        return
            static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8u) |
            (static_cast<uint32_t>(data[2]) << 16u) |
            (static_cast<uint32_t>(data[3]) << 24u);
    }

    uint32_t RotateLeft(uint32_t value, int bits)
    {
        return (value << bits) | (value >> (32 - bits));
    }

    void QuarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
    {
        a += b; d ^= a; d = RotateLeft(d, 16);
        c += d; b ^= c; b = RotateLeft(b, 12);
        a += b; d ^= a; d = RotateLeft(d, 8);
        c += d; b ^= c; b = RotateLeft(b, 7);
    }

    void GenerateChaCha20Block(const PakEncryptionKey& key, const PakNonce& nonce, uint32_t counter, uint8_t outBlock[64])
    {
        uint32_t state[16] = {
            0x61707865u, 0x3320646Eu, 0x79622D32u, 0x6B206574u,
            0, 0, 0, 0,
            0, 0, 0, 0,
            counter,
            0, 0, 0
        };

        for (int index = 0; index < 8; ++index)
            state[4 + index] = LoadU32LE(key.data() + index * 4);
        state[13] = LoadU32LE(nonce.data() + 0);
        state[14] = LoadU32LE(nonce.data() + 4);
        state[15] = LoadU32LE(nonce.data() + 8);

        uint32_t working[16] = {};
        std::copy(std::begin(state), std::end(state), std::begin(working));

        for (int round = 0; round < 10; ++round)
        {
            QuarterRound(working[0], working[4], working[8], working[12]);
            QuarterRound(working[1], working[5], working[9], working[13]);
            QuarterRound(working[2], working[6], working[10], working[14]);
            QuarterRound(working[3], working[7], working[11], working[15]);

            QuarterRound(working[0], working[5], working[10], working[15]);
            QuarterRound(working[1], working[6], working[11], working[12]);
            QuarterRound(working[2], working[7], working[8], working[13]);
            QuarterRound(working[3], working[4], working[9], working[14]);
        }

        for (int index = 0; index < 16; ++index)
            working[index] += state[index];

        for (int index = 0; index < 16; ++index)
        {
            const uint32_t value = working[index];
            outBlock[index * 4 + 0] = static_cast<uint8_t>(value & 0xFFu);
            outBlock[index * 4 + 1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
            outBlock[index * 4 + 2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
            outBlock[index * 4 + 3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
        }
    }

    void XorChaCha20(const PakEncryptionKey& key, const PakNonce& nonce, uint64_t streamOffset, uint8_t* data, size_t size)
    {
        uint64_t blockIndex = streamOffset / 64u;
        size_t blockOffset = static_cast<size_t>(streamOffset % 64u);

        while (size > 0)
        {
            uint8_t keystream[64] = {};
            GenerateChaCha20Block(key, nonce, static_cast<uint32_t>(blockIndex), keystream);

            const size_t chunk = std::min<size_t>(size, 64u - blockOffset);
            for (size_t index = 0; index < chunk; ++index)
                data[index] ^= keystream[blockOffset + index];

            data += chunk;
            size -= chunk;
            blockOffset = 0;
            ++blockIndex;
        }
    }

    bool ReadPakEntryBytes(const MountedPakFile& pak, const MountedPakEntry& entry, std::vector<uint8_t>& outData, std::string* outError)
    {
        std::ifstream file(pak.pakPath, std::ios::binary);
        if (!file.is_open())
        {
            if (outError)
                *outError = "Failed to open pak file: " + pak.pakPath;
            return false;
        }

        file.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
        if (!file.good())
        {
            if (outError)
                *outError = "Failed to seek pak entry: " + pak.pakPath;
            return false;
        }

        outData.resize(static_cast<size_t>(entry.size));
        if (entry.size > 0)
            file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(entry.size));
        if (!file.good() && !file.eof())
        {
            if (outError)
                *outError = "Failed to read pak entry payload: " + pak.pakPath;
            return false;
        }

        if (pak.encrypted && !outData.empty())
            XorChaCha20(pak.key, pak.nonce, entry.offset, outData.data(), outData.size());

        return true;
    }

    bool TryLoadMountedPakBytes(const std::string& lookupPath, std::vector<uint8_t>& outData, std::string* outError)
    {
        const auto found = gMountedPakLookup.find(lookupPath);
        if (found == gMountedPakLookup.end())
            return false;

        const MountedPakLocation& location = found->second;
        if (location.pakIndex >= gMountedPaks.size())
            return false;

        const MountedPakFile& pak = gMountedPaks[location.pakIndex];
        if (location.entryIndex >= pak.entries.size())
            return false;

        return ReadPakEntryBytes(pak, pak.entries[location.entryIndex], outData, outError);
    }

    bool ReadPakHeader(std::istream& stream, uint32_t& outFlags, uint32_t& outFileCount, uint64_t& outIndexOffset, uint64_t& outIndexSize, PakNonce& outNonce)
    {
        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t reserved = 0;
        if (!ReadValue(stream, magic) ||
            !ReadValue(stream, version) ||
            !ReadValue(stream, outFlags) ||
            !ReadValue(stream, outFileCount) ||
            !ReadValue(stream, outIndexOffset) ||
            !ReadValue(stream, outIndexSize))
        {
            return false;
        }

        stream.read(reinterpret_cast<char*>(outNonce.data()), outNonce.size());
        if (!stream.good() || !ReadValue(stream, reserved))
            return false;

        if (magic != kPakMagic || version != kPakVersion)
            return false;

        return true;
    }

    bool WritePakHeader(std::ostream& stream, uint32_t flags, uint32_t fileCount, uint64_t indexOffset, uint64_t indexSize, const PakNonce& nonce)
    {
        const uint32_t reserved = 0;
        return WriteValue(stream, kPakMagic) &&
               WriteValue(stream, kPakVersion) &&
               WriteValue(stream, flags) &&
               WriteValue(stream, fileCount) &&
               WriteValue(stream, indexOffset) &&
               WriteValue(stream, indexSize) &&
               static_cast<bool>(stream.write(reinterpret_cast<const char*>(nonce.data()), nonce.size())) &&
               WriteValue(stream, reserved);
    }

    void AppendBytes(std::vector<uint8_t>& buffer, const void* data, size_t size)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), bytes, bytes + size);
    }

    template<typename T>
    void AppendValue(std::vector<uint8_t>& buffer, const T& value)
    {
        AppendBytes(buffer, &value, sizeof(T));
    }

    void AppendString(std::vector<uint8_t>& buffer, const std::string& value)
    {
        const uint32_t length = static_cast<uint32_t>(value.size());
        AppendValue(buffer, length);
        if (length > 0)
            AppendBytes(buffer, value.data(), length);
    }
}

bool LoadAssetFileBytes(const char* path, std::vector<uint8_t>& outData, std::string* outError)
{
    KES_ZONE_FN(IO);
    KES_ZONE_TEXTF(IO, "%s", path ? path : "<null>");
    outData.clear();
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing file path";
        return false;
    }

    const fs::path requested(path);
    if (requested.is_absolute() && fs::exists(requested))
    {
        if (LoadDiskFileBytes(requested.lexically_normal(), outData))
            return true;
        if (outError)
            *outError = "Failed to read file: " + requested.lexically_normal().string();
        return false;
    }

    if (!requested.is_absolute() && fs::exists(requested))
    {
        if (LoadDiskFileBytes(requested.lexically_normal(), outData))
            return true;
        if (outError)
            *outError = "Failed to read file: " + requested.lexically_normal().string();
        return false;
    }

    const std::string resolvedPath = ResolveAssetPath(path);
    if (!resolvedPath.empty())
    {
        const fs::path resolved(resolvedPath);
        if (fs::exists(resolved))
        {
            if (LoadDiskFileBytes(resolved.lexically_normal(), outData))
                return true;
            if (outError)
                *outError = "Failed to read file: " + resolved.lexically_normal().string();
            return false;
        }
    }

    std::string pakLookupPath = BuildPakLookupPath(path);
    if (pakLookupPath.empty() && !resolvedPath.empty())
        pakLookupPath = BuildPakLookupPath(resolvedPath.c_str());

    if (!pakLookupPath.empty() && TryLoadMountedPakBytes(pakLookupPath, outData, outError))
        return true;

    if (outError)
        *outError = "Failed to load file: " + std::string(path);
    return false;
}

bool LoadAssetFileText(const char* path, std::string& outText, std::string* outError)
{
    KES_ZONE_FN(IO);
    KES_ZONE_TEXTF(IO, "%s", path ? path : "<null>");
    outText.clear();

    std::vector<uint8_t> bytes;
    if (!LoadAssetFileBytes(path, bytes, outError))
        return false;

    outText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

void ClearMountedPakFiles()
{
    gMountedPaks.clear();
    gMountedPakLookup.clear();
}

void SetPakEncryptionKeyOverride(const char* hexKey)
{
    gPakEncryptionKeyOverride = hexKey ? hexKey : "";
}

std::string GetPakEncryptionKeyOverride()
{
    return gPakEncryptionKeyOverride;
}

bool MountPakFile(const char* pakPath, std::string* outError)
{
    KES_ZONE_FN(IO);
    KES_ZONE_TEXTF(IO, "%s", pakPath ? pakPath : "<null>");
    if (!pakPath || !pakPath[0])
    {
        if (outError)
            *outError = "Missing pak path";
        return false;
    }

    std::ifstream file(pakPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open pak file";
        return false;
    }

    uint32_t flags = 0;
    uint32_t fileCount = 0;
    uint64_t indexOffset = 0;
    uint64_t indexSize = 0;
    PakNonce nonce = {};
    if (!ReadPakHeader(file, flags, fileCount, indexOffset, indexSize, nonce))
    {
        if (outError)
            *outError = "Invalid pak header";
        return false;
    }

    PakEncryptionKey key = {};
    const bool encrypted = (flags & kPakFlagEncrypted) != 0;
    if (encrypted)
    {
        const std::string keyHex = GetEffectivePakEncryptionKeyHex();
        if (keyHex.empty())
        {
            if (outError)
                *outError = "Pak is encrypted but ENGINE_PAK_ENCRYPTION_KEY_HEX is empty";
            return false;
        }

        if (!ParsePakEncryptionKeyHex(keyHex, key, outError))
            return false;
    }

    file.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(file.tellg());
    if (indexOffset > fileSize || indexSize > fileSize || indexOffset + indexSize > fileSize)
    {
        if (outError)
            *outError = "Pak index range is invalid";
        return false;
    }

    file.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    std::vector<uint8_t> indexBytes(static_cast<size_t>(indexSize));
    if (indexSize > 0)
        file.read(reinterpret_cast<char*>(indexBytes.data()), static_cast<std::streamsize>(indexSize));
    if (!file.good() && !file.eof())
    {
        if (outError)
            *outError = "Failed to read pak index";
        return false;
    }

    if (encrypted && !indexBytes.empty())
        XorChaCha20(key, nonce, indexOffset, indexBytes.data(), indexBytes.size());

    std::string indexText(reinterpret_cast<const char*>(indexBytes.data()), indexBytes.size());
    std::istringstream indexStream(indexText, std::ios::binary);
    uint32_t indexMagic = 0;
    uint32_t indexedFileCount = 0;
    if (!ReadValue(indexStream, indexMagic) || !ReadValue(indexStream, indexedFileCount))
    {
        if (outError)
            *outError = "Failed to read pak index header";
        return false;
    }

    if (indexMagic != kPakIndexMagic || indexedFileCount != fileCount)
    {
        if (outError)
            *outError = "Pak index header is invalid";
        return false;
    }

    MountedPakFile pak = {};
    pak.pakPath = fs::path(pakPath).lexically_normal().string();
    pak.encrypted = encrypted;
    pak.key = key;
    pak.nonce = nonce;
    pak.entries.resize(fileCount);

    for (MountedPakEntry& entry : pak.entries)
    {
        if (!ReadString(indexStream, entry.path) ||
            !ReadValue(indexStream, entry.offset) ||
            !ReadValue(indexStream, entry.size))
        {
            if (outError)
                *outError = "Failed to parse pak index";
            return false;
        }

        entry.path = NormalizePakEntryPath(entry.path);
        if (entry.path.empty())
        {
            if (outError)
                *outError = "Pak contains an empty file path";
            return false;
        }

        if (entry.offset > fileSize || entry.size > fileSize || entry.offset + entry.size > fileSize || entry.offset < kPakHeaderSize)
        {
            if (outError)
                *outError = "Pak entry range is invalid for " + entry.path;
            return false;
        }
    }

    const size_t pakIndex = gMountedPaks.size();
    gMountedPaks.push_back(std::move(pak));
    for (size_t entryIndex = 0; entryIndex < gMountedPaks[pakIndex].entries.size(); ++entryIndex)
        gMountedPakLookup[gMountedPaks[pakIndex].entries[entryIndex].path] = { pakIndex, entryIndex };
    return true;
}

bool MountContentPakFiles(std::string* outError, String executablePath)
{
    KES_ZONE_FN(IO);
    ClearMountedPakFiles();
    // <exe dir>/<content dir>. (Was a plain string append, which produced
    // "...\\Releasecontent" and never found any pak.)
    const fs::path pakRoot = executablePath.value.empty()
        ? fs::path(ENGINE_CONTENT_DIRECTORY)
        : fs::path(executablePath.value) / ENGINE_CONTENT_DIRECTORY;

    if (pakRoot.empty())
        return true;

    std::error_code error;
    if (!fs::exists(pakRoot, error) || error)
        return true;

    std::vector<fs::path> pakFiles;
    for (const fs::directory_entry& entry : fs::directory_iterator(pakRoot, error))
    {
        if (error)
            break;
        if (!entry.is_regular_file())
            continue;

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch)
                       {
                           return static_cast<char>(std::tolower(ch));
                       });
        if (extension == ".pak")
            pakFiles.push_back(entry.path());
    }

    if (error)
    {
        if (outError)
            *outError = "Failed to enumerate pak directory: " + pakRoot.string();
        ClearMountedPakFiles();
        return false;
    }

    std::sort(pakFiles.begin(), pakFiles.end());
    for (const fs::path& pakFile : pakFiles)
    {
        if (!MountPakFile(pakFile.string().c_str(), outError))
        {
            ClearMountedPakFiles();
            return false;
        }
    }

    return true;
}

std::size_t GetMountedPakFileCount()
{
    return gMountedPaks.size();
}

std::vector<std::string> GetMountedPakFilePaths()
{
    std::vector<std::string> paths;
    paths.reserve(gMountedPaks.size());
    for (const MountedPakFile& pak : gMountedPaks)
        paths.push_back(pak.pakPath);
    return paths;
}

std::string GeneratePakEncryptionKeyHex()
{
    std::random_device randomDevice;
    static constexpr char kHexDigits[] = "0123456789abcdef";

    std::string hexValue;
    hexValue.resize(kPakEncryptionKeySize * 2u);
    for (size_t index = 0; index < kPakEncryptionKeySize; ++index)
    {
        const uint8_t value = static_cast<uint8_t>(randomDevice() & 0xFFu);
        hexValue[index * 2u + 0u] = kHexDigits[(value >> 4u) & 0x0Fu];
        hexValue[index * 2u + 1u] = kHexDigits[value & 0x0Fu];
    }
    return hexValue;
}

bool CreatePakFile(const char* sourceDirectory, const char* outputPath, const char* encryptionKeyHex, std::string* outError)
{
    if (!sourceDirectory || !sourceDirectory[0] || !outputPath || !outputPath[0])
    {
        if (outError)
            *outError = "Usage requires a source directory and output pak path";
        return false;
    }

    const fs::path sourceRoot = fs::absolute(fs::path(sourceDirectory)).lexically_normal();
    if (!fs::exists(sourceRoot) || !fs::is_directory(sourceRoot))
    {
        if (outError)
            *outError = "Source directory does not exist";
        return false;
    }

    const fs::path pakOutputPath = fs::absolute(fs::path(outputPath)).lexically_normal();
    if (!pakOutputPath.parent_path().empty())
        fs::create_directories(pakOutputPath.parent_path());

    const std::string keyHex = encryptionKeyHex ? std::string(encryptionKeyHex) : std::string();
    PakEncryptionKey key = {};
    if (!keyHex.empty() && !ParsePakEncryptionKeyHex(keyHex, key, outError))
        return false;

    std::vector<std::pair<fs::path, std::string>> filesToPack;
    std::error_code walkError;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(sourceRoot, walkError))
    {
        if (walkError)
            break;
        if (!entry.is_regular_file())
            continue;

        const fs::path filePath = entry.path().lexically_normal();
        if (filePath == pakOutputPath)
            continue;

        std::error_code relativeError;
        fs::path relativePath = fs::relative(filePath, sourceRoot, relativeError);
        if (relativeError)
            continue;

        const std::string archivePath = NormalizePakEntryPath(relativePath);
        if (!archivePath.empty())
            filesToPack.emplace_back(filePath, archivePath);
    }

    if (walkError)
    {
        if (outError)
            *outError = "Failed to enumerate source directory";
        return false;
    }

    std::sort(filesToPack.begin(), filesToPack.end(),
              [](const auto& left, const auto& right)
              {
                  return left.second < right.second;
              });

    std::ofstream file(pakOutputPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open pak output path";
        return false;
    }

    const bool encrypted = !keyHex.empty();
    const uint32_t flags = encrypted ? kPakFlagEncrypted : 0u;
    PakNonce nonce = {};
    if (encrypted)
    {
        std::random_device randomDevice;
        for (uint8_t& value : nonce)
            value = static_cast<uint8_t>(randomDevice() & 0xFFu);
    }

    if (!WritePakHeader(file, flags, static_cast<uint32_t>(filesToPack.size()), 0, 0, nonce))
    {
        if (outError)
            *outError = "Failed to write pak header";
        return false;
    }

    std::vector<MountedPakEntry> entries;
    entries.reserve(filesToPack.size());
    for (const auto& [filePath, archivePath] : filesToPack)
    {
        std::vector<uint8_t> data;
        if (!LoadDiskFileBytes(filePath, data))
        {
            if (outError)
                *outError = "Failed to read source file for packing: " + filePath.string();
            return false;
        }

        MountedPakEntry entry = {};
        entry.path = archivePath;
        entry.offset = static_cast<uint64_t>(file.tellp());
        entry.size = static_cast<uint64_t>(data.size());

        if (encrypted && !data.empty())
            XorChaCha20(key, nonce, entry.offset, data.data(), data.size());

        if (!data.empty())
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file.good())
        {
            if (outError)
                *outError = "Failed to write pak payload";
            return false;
        }

        entries.push_back(std::move(entry));
    }

    std::vector<uint8_t> indexBytes;
    AppendValue(indexBytes, kPakIndexMagic);
    AppendValue(indexBytes, static_cast<uint32_t>(entries.size()));
    for (const MountedPakEntry& entry : entries)
    {
        AppendString(indexBytes, entry.path);
        AppendValue(indexBytes, entry.offset);
        AppendValue(indexBytes, entry.size);
    }

    const uint64_t indexOffset = static_cast<uint64_t>(file.tellp());
    const uint64_t indexSize = static_cast<uint64_t>(indexBytes.size());
    if (encrypted && !indexBytes.empty())
        XorChaCha20(key, nonce, indexOffset, indexBytes.data(), indexBytes.size());

    if (!indexBytes.empty())
        file.write(reinterpret_cast<const char*>(indexBytes.data()), static_cast<std::streamsize>(indexBytes.size()));
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write pak index";
        return false;
    }

    file.seekp(0, std::ios::beg);
    if (!WritePakHeader(file, flags, static_cast<uint32_t>(entries.size()), indexOffset, indexSize, nonce))
    {
        if (outError)
            *outError = "Failed to finalize pak header";
        return false;
    }

    return true;
}


bool ExtractPakFile(const char* pakPath, const char* outputDirectory, std::string* outError)
{
    if (!pakPath || !pakPath[0] || !outputDirectory || !outputDirectory[0])
    {
        if (outError)
            *outError = "Usage requires an input pak path and output directory";
        return false;
    }

    std::ifstream file(pakPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open pak file: " + std::string(pakPath);
        return false;
    }

    uint32_t flags = 0;
    uint32_t fileCount = 0;
    uint64_t indexOffset = 0;
    uint64_t indexSize = 0;
    PakNonce nonce = {};
    if (!ReadPakHeader(file, flags, fileCount, indexOffset, indexSize, nonce))
    {
        if (outError)
            *outError = "Invalid pak header: " + std::string(pakPath);
        return false;
    }

    if (flags & kPakFlagEncrypted)
    {
        if (outError)
            *outError = "Extraction is not supported for encrypted pak files";
        return false;
    }

    file.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(file.tellg());
    if (indexOffset > fileSize || indexSize > fileSize || indexOffset + indexSize > fileSize)
    {
        if (outError)
            *outError = "Pak index range is invalid";
        return false;
    }

    file.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    std::vector<uint8_t> indexBytes(static_cast<size_t>(indexSize));
    if (indexSize > 0)
        file.read(reinterpret_cast<char*>(indexBytes.data()), static_cast<std::streamsize>(indexSize));
    if (!file.good() && !file.eof())
    {
        if (outError)
            *outError = "Failed to read pak index";
        return false;
    }

    std::istringstream indexStream(
        std::string(reinterpret_cast<const char*>(indexBytes.data()), indexBytes.size()),
        std::ios::binary);

    uint32_t indexMagic = 0;
    uint32_t indexedFileCount = 0;
    if (!ReadValue(indexStream, indexMagic) || !ReadValue(indexStream, indexedFileCount))
    {
        if (outError)
            *outError = "Failed to read pak index header";
        return false;
    }

    if (indexMagic != kPakIndexMagic || indexedFileCount != fileCount)
    {
        if (outError)
            *outError = "Pak index header is invalid";
        return false;
    }

    std::vector<MountedPakEntry> entries(fileCount);
    for (MountedPakEntry& entry : entries)
    {
        if (!ReadString(indexStream, entry.path) ||
            !ReadValue(indexStream, entry.offset) ||
            !ReadValue(indexStream, entry.size))
        {
            if (outError)
                *outError = "Failed to parse pak index entry";
            return false;
        }

        entry.path = NormalizePakEntryPath(entry.path);
        if (entry.path.empty())
        {
            if (outError)
                *outError = "Pak contains an entry with an empty path";
            return false;
        }

        if (entry.offset < kPakHeaderSize ||
            entry.offset > fileSize ||
            entry.size > fileSize ||
            entry.offset + entry.size > fileSize)
        {
            if (outError)
                *outError = "Pak entry range is invalid for: " + entry.path;
            return false;
        }
    }

    const fs::path outputRoot = fs::absolute(fs::path(outputDirectory)).lexically_normal();

    MountedPakFile pakDesc;
    pakDesc.pakPath = fs::path(pakPath).lexically_normal().string();
    pakDesc.encrypted = false;

    for (const MountedPakEntry& entry : entries)
    {
        const fs::path entryRelative = fs::path(entry.path).lexically_normal();
        if (entryRelative.is_absolute() || entryRelative.begin()->string() == "..")
        {
            if (outError)
                *outError = "Pak entry path is unsafe: " + entry.path;
            return false;
        }

        const fs::path destPath = (outputRoot / entryRelative).lexically_normal();
        if (!PathStartsWith(destPath, outputRoot))
        {
            if (outError)
                *outError = "Pak entry path escapes output directory: " + entry.path;
            return false;
        }

        std::error_code mkdirError;
        fs::create_directories(destPath.parent_path(), mkdirError);
        if (mkdirError)
        {
            if (outError)
                *outError = "Failed to create output directory for: " + entry.path;
            return false;
        }

        std::vector<uint8_t> data;
        if (!ReadPakEntryBytes(pakDesc, entry, data, outError))
            return false;

        std::ofstream outFile(destPath, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open())
        {
            if (outError)
                *outError = "Failed to open output file: " + destPath.string();
            return false;
        }

        if (!data.empty())
            outFile.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!outFile.good())
        {
            if (outError)
                *outError = "Failed to write output file: " + destPath.string();
            return false;
        }
    }
    return true;
}