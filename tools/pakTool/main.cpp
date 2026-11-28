#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "pak_file_system.h"
#include "texture_format.h"

namespace
{
    bool StartsWithOption(const std::string& value, std::string_view option)
    {
        return value.size() > option.size() &&
            value.compare(0, option.size(), option) == 0 &&
            value[option.size()] == '=';
    }

    void PrintUsage()
    {
        std::cout << "Usage:\n";
        std::cout << "  Pack:   pak-tool <source-dir> <output.pak> [--encrypt <hex-key> | --generate-key]\n";
        std::cout << "  Unpack: pak-tool --unpack <input.pak> <output-dir> [--decrypt <hex-key>]\n";
        std::cout << "  Strip:  pak-tool --strip-texture-sources <dir>   (removes editor-only source images from every .ktex)\n";
        std::cout << "\nExamples:\n";
        std::cout << "  pak-tool game/assets out/content/game_data0.pak\n";
        std::cout << "  pak-tool game/assets out/content/game_data0.pak --generate-key\n";
        std::cout << "  pak-tool game/assets out/content/game_data0.pak --encrypt 001122...\n";
        std::cout << "  pak-tool --unpack out/content/game_data0.pak extracted/\n";
        std::cout << "  pak-tool --unpack out/content/game_data0.pak extracted/ --decrypt 001122...\n";
    }

    enum class Mode { Pack, Unpack };

    int StripTextureSources(const std::filesystem::path& dir)
    {
        namespace fs = std::filesystem;
        size_t stripped = 0;
        uint64_t saved = 0;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".ktex")
                continue;
            std::vector<uint8_t> bytes;
            {
                std::ifstream in(entry.path(), std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), {});
            }
            TextureFileData texture;
            std::string error;
            if (!ReadKTex(bytes.data(), bytes.size(), texture, &error))
            {
                std::cerr << "Invalid texture " << entry.path().string() << ": " << error << "\n";
                return 1;
            }
            if (texture.version < 2 || texture.sourceData.empty())
                continue;
            std::vector<uint8_t> out;
            if (!WriteKTex(texture, out, &error, true))
            {
                std::cerr << "Failed to rewrite " << entry.path().string() << ": " << error << "\n";
                return 1;
            }
            std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            if (!file.good())
            {
                std::cerr << "Failed to write " << entry.path().string() << "\n";
                return 1;
            }
            ++stripped;
            saved += bytes.size() - out.size();
        }
        std::cout << "Stripped texture sources from " << stripped << " file(s), " << saved / 1024 << " KiB\n";
        return 0;
    }
}

int main(int argc, char* argv[])
{
    std::vector<std::string> positional;
    std::string encryptionKeyHex;
    std::string decryptionKeyHex;
    bool generateKey = false;
    Mode mode = Mode::Pack;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index] ? argv[index] : "";

        if (argument == "--strip-texture-sources")
        {
            if (index + 1 >= argc || !argv[index + 1] || !argv[index + 1][0])
            {
                PrintUsage();
                return 1;
            }
            return StripTextureSources(std::filesystem::path(argv[index + 1]));
        }

        if (argument == "--unpack")
        {
            mode = Mode::Unpack;
            continue;
        }

        if (argument == "--generate-key")
        {
            generateKey = true;
            continue;
        }

        if (argument == "--decrypt" || StartsWithOption(argument, "--decrypt"))
        {
            if (StartsWithOption(argument, "--decrypt"))
            {
                decryptionKeyHex = argument.substr(std::string_view("--decrypt=").size());
            }
            else
            {
                if (index + 1 >= argc || !argv[index + 1] || !argv[index + 1][0])
                {
                    std::cerr << "Missing value for " << argument << "\n";
                    PrintUsage();
                    return 1;
                }
                decryptionKeyHex = argv[++index];
            }
            continue;
        }

        const bool isEncryptFlag = (argument == "--encrypt") || StartsWithOption(argument, "--encrypt");

        if (isEncryptFlag)
        {
            if (StartsWithOption(argument, "--encrypt"))
            {
                const std::string prefix = "--encrypt=";
                encryptionKeyHex = argument.substr(prefix.size());
            }
            else
            {
                if (index + 1 >= argc || !argv[index + 1] || !argv[index + 1][0])
                {
                    std::cerr << "Missing value for " << argument << "\n";
                    PrintUsage();
                    return 1;
                }
                encryptionKeyHex = argv[++index];
            }
            continue;
        }

        if (!argument.empty() && argument[0] == '-')
        {
            std::cerr << "Unknown option: " << argument << "\n";
            PrintUsage();
            return 1;
        }

        positional.push_back(argument);
    }

    if (generateKey && mode == Mode::Unpack)
    {
        std::cerr << "--generate-key cannot be used with --unpack\n";
        return 1;
    }

    if (generateKey && !encryptionKeyHex.empty())
    {
        std::cerr << "Use either --encrypt <hex-key> or --generate-key, not both\n";
        return 1;
    }

    if (positional.size() != 2)
    {
        PrintUsage();
        return 1;
    }

    if (mode == Mode::Unpack)
    {
        std::string error;
        if (!ExtractPakFile(positional[0].c_str(), positional[1].c_str(), &error, decryptionKeyHex.c_str()))
        {
            std::cerr << "Pak unpack failed: " << error << "\n";
            return 2;
        }

        std::cout << "Unpacked " << positional[0] << " -> " << positional[1] << "\n";
        return 0;
    }

    if (generateKey)
        encryptionKeyHex = GeneratePakEncryptionKeyHex();

    std::string error;
    const char* encryptionKeyPtr = encryptionKeyHex.empty() ? nullptr : encryptionKeyHex.c_str();
    if (!CreatePakFile(positional[0].c_str(), positional[1].c_str(), encryptionKeyPtr, &error))
    {
        std::cerr << "Pak build failed: " << error << "\n";
        return 2;
    }

    std::cout << "Packed " << positional[0] << " -> " << positional[1] << "\n";
    if (encryptionKeyHex.empty())
    {
        std::cout << "Encryption: disabled\n";
    }
    else
    {
        std::cout << "Encryption: enabled\n";
        std::cout << "Pak key: " << encryptionKeyHex << "\n";
        std::cout << "Set ENGINE_PAK_ENCRYPTION_KEY_HEX to this value in engine_configuration.\n";
    }

    return 0;
}