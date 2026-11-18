#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include "file_paths.h"
#include "model_import_api.h"
#include "sound_import_api.h"
#include "texture_import_api.h"

namespace
{
    enum class ImportKind
    {
        None,
        Model,
        Texture,
        Sound
    };

    ImportKind DetectImportKind(const std::string& path)
    {
        if (CanImportModelSource(path))
            return ImportKind::Model;
        if (CanImportTextureSource(path))
            return ImportKind::Texture;
        if (CanImportSoundSource(path))
            return ImportKind::Sound;
        return ImportKind::None;
    }

    const char* GetImportedExtension(ImportKind kind)
    {
        switch (kind)
        {
        case ImportKind::Model: return ".kmodel";
        case ImportKind::Texture: return ".ktex";
        case ImportKind::Sound: return ".ksound";
        default: return "";
        }
    }
}

int main(int argc, char* argv[])
{
    std::vector<std::string> args = CollectPositionalCommandLineArguments(argc, argv);
    const auto takeFlag = [&args](const char* flag)
    {
        const auto found = std::find(args.begin(), args.end(), flag);
        if (found == args.end())
            return false;
        args.erase(found);
        return true;
    };
    // --pcm keeps sounds as uncompressed float PCM instead of QOA.
    const bool soundPcm = takeFlag("--pcm");
    ModelImportOptions modelOptions;
    modelOptions.mesh = !takeFlag("--no-mesh");
    modelOptions.animations = !takeFlag("--no-anims");
    if (args.size() < 1 || args.size() > 2)
    {
        std::cout << "Usage: model-importer [-assetpath <content-root>] [--pcm] [--no-mesh] [--no-anims] <input> [output]\n"
                     "  --no-mesh   write only the .kanim clips; if <output> exists they are matched to its nodes by name\n"
                     "  --no-anims  write only the .kmodel\n";
        return 1;
    }

    const std::string inputPath = ResolveAssetPath(args[0].c_str());
    const ImportKind importKind = DetectImportKind(inputPath);
    if (importKind == ImportKind::None)
    {
        std::cerr << "Import failed: unsupported source format.\n";
        return 2;
    }

    std::filesystem::path outputPath = args.size() >= 2
        ? std::filesystem::path(ResolveAssetWritePath(args[1].c_str()))
        : std::filesystem::path(inputPath).replace_extension(GetImportedExtension(importKind));
    if (!outputPath.has_extension())
        outputPath.replace_extension(GetImportedExtension(importKind));

    std::string error;
    bool imported = false;
    switch (importKind)
    {
    case ImportKind::Model:
        imported = ImportModelToKModel(inputPath, outputPath.string(), error, modelOptions);
        break;
    case ImportKind::Texture:
        imported = ImportTextureToKTex(inputPath, outputPath.string(), error);
        break;
    case ImportKind::Sound:
        imported = ImportSoundToKSound(inputPath, outputPath.string(), error, !soundPcm);
        break;
    default:
        break;
    }

    if (!imported)
    {
        std::cerr << "Import failed: " << error << "\n";
        return 2;
    }

    std::cout << "Imported " << inputPath << " -> " << outputPath.string() << "\n";
    return 0;
}
