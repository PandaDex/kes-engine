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
    const std::vector<std::string> args = CollectPositionalCommandLineArguments(argc, argv);
    if (args.size() < 1 || args.size() > 2)
    {
        std::cout << "Usage: model-importer [-assetpath <content-root>] <input> [output]\n";
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
        imported = ImportModelToKModel(inputPath, outputPath.string(), error);
        break;
    case ImportKind::Texture:
        imported = ImportTextureToKTex(inputPath, outputPath.string(), error);
        break;
    case ImportKind::Sound:
        imported = ImportSoundToKSound(inputPath, outputPath.string(), error);
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
