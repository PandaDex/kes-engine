#include "file_paths.h"
#include <filesystem>
#include <string_view>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include "engine_configuration.h"

namespace
{
    std::filesystem::path gAssetRootOverride;

    bool PathComponentEquals(const std::filesystem::path& a, const std::filesystem::path& b)
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

    bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& prefix)
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

    std::filesystem::path StripPathPrefix(const std::filesystem::path& path, const std::filesystem::path& prefix)
    {
        std::filesystem::path stripped;
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

    std::filesystem::path GetConfiguredContentDirectory()
    {
        return std::filesystem::path(ENGINE_CONTENT_DIRECTORY).lexically_normal();
    }

    std::filesystem::path NormalizeContentRelativePath(const std::filesystem::path& path)
    {
        const std::filesystem::path normalized = path.lexically_normal();
        const std::filesystem::path contentDirectory = GetConfiguredContentDirectory();
        if (!contentDirectory.empty() && PathStartsWith(normalized, contentDirectory))
            return StripPathPrefix(normalized, contentDirectory);

        const std::filesystem::path legacyContentRoot = std::filesystem::path("game") / contentDirectory;
        if (!contentDirectory.empty() && PathStartsWith(normalized, legacyContentRoot))
            return StripPathPrefix(normalized, legacyContentRoot);

        return normalized;
    }

    std::filesystem::path GetConfiguredContentRoot()
    {
        if (!gAssetRootOverride.empty())
            return gAssetRootOverride;

        const std::filesystem::path contentDirectory = GetConfiguredContentDirectory();
        if (contentDirectory.is_absolute())
            return contentDirectory;

        const std::filesystem::path executableDirectory = GetExecutableDirectory();
        if (executableDirectory.empty())
            return {};
        if (contentDirectory.empty() || contentDirectory == ".")
            return executableDirectory;

        return (executableDirectory / contentDirectory).lexically_normal();
    }

    std::filesystem::path BuildContentPath(const std::filesystem::path& path)
    {
        const std::filesystem::path relativePath = NormalizeContentRelativePath(path);
        const std::filesystem::path contentRoot = GetConfiguredContentRoot();
        if (contentRoot.empty())
            return relativePath;

        return (contentRoot / relativePath).lexically_normal();
    }

    bool StartsWithOption(std::string_view argument, std::string_view option)
    {
        return argument.size() > option.size() &&
               argument.compare(0, option.size(), option) == 0 &&
               argument[option.size()] == '=';
    }
}

void SetAssetRootOverride(const char* path)
{
    namespace fs = std::filesystem;
    if (!path || !path[0])
    {
        gAssetRootOverride.clear();
        return;
    }

    fs::path overridePath(path);
    if (!overridePath.is_absolute())
        overridePath = fs::absolute(overridePath);
    gAssetRootOverride = overridePath.lexically_normal();
    if (!gAssetRootOverride.has_filename())
        gAssetRootOverride = gAssetRootOverride.parent_path();   // no empty last element from a trailing separator
}

std::string GetAssetRootOverride()
{
    return gAssetRootOverride.string();
}

std::string ResolveAssetPath(const char* path)
{
    if (!path || !path[0])
        return {};

    namespace fs = std::filesystem;
    const fs::path requested(path);

    if (requested.is_absolute())
        return requested.lexically_normal().string();

    if (fs::exists(requested))
        return requested.lexically_normal().string();

    return BuildContentPath(requested).string();
}

std::filesystem::path GetUserDataDirectory()
{
    if (char* pref = SDL_GetPrefPath(ENGINE_COMPANY_NAME, ENGINE_PROJECT_NAME))
    {
        const std::string utf8 = pref;
        SDL_free(pref);
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }
    return GetExecutableDirectory() / "Saved";
}

std::string ResolveAssetWritePath(const char* path)
{
    if (!path || !path[0])
        return {};

    namespace fs = std::filesystem;
    const fs::path requested(path);
    if (requested.is_absolute())
        return requested.lexically_normal().string();

    return BuildContentPath(requested).string();
}

int ApplyAssetRootOverrideArgument(int argc, const char* const* argv, int index)
{
    if (!argv || index < 0 || index >= argc || !argv[index] || !argv[index][0])
        return 0;

    const std::string_view argument(argv[index]);
    constexpr std::string_view kShortOption = "-assetpath";
    constexpr std::string_view kLongOption = "--assetpath";

    if (StartsWithOption(argument, kShortOption))
    {
        SetAssetRootOverride(argument.substr(kShortOption.size() + 1).data());
        return 1;
    }

    if (StartsWithOption(argument, kLongOption))
    {
        SetAssetRootOverride(argument.substr(kLongOption.size() + 1).data());
        return 1;
    }

    if (argument != kShortOption && argument != kLongOption)
        return 0;

    if (index + 1 < argc && argv[index + 1] && argv[index + 1][0])
    {
        SetAssetRootOverride(argv[index + 1]);
        return 2;
    }

    return 1;
}

std::vector<std::string> CollectPositionalCommandLineArguments(int argc, const char* const* argv, int firstIndex)
{
    std::vector<std::string> positionals;
    if (!argv || argc <= 0)
        return positionals;

    const int startIndex = firstIndex > 0 ? firstIndex : 0;
    for (int index = startIndex; index < argc; ++index)
    {
        const int consumed = ApplyAssetRootOverrideArgument(argc, argv, index);
        if (consumed > 0)
        {
            index += consumed - 1;
            continue;
        }

        if (argv[index] && argv[index][0])
            positionals.emplace_back(argv[index]);
    }

    return positionals;
}


