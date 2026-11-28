#include "texture_import_api.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <windows.h>
#include <wincodec.h>
#include "texture_cook.h"
#include "texture_file.h"

namespace
{
    std::string Lowercase(std::string value)
    {
        for (char& ch : value)
        {
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
        }
        return value;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};

        const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
        if (count <= 0)
            return {};

        std::wstring wide((size_t)count, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), wide.data(), count);
        return wide;
    }

    template<typename T>
    void SafeRelease(T*& object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    }
}

bool CanImportTextureSource(const std::string& path)
{
    const std::string extension = Lowercase(std::filesystem::path(path).extension().string());
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".tif" || extension == ".tiff";
}

static std::vector<TextureGroup> LoadProjectTextureGroups(const std::string& groupsPath)
{
    std::vector<TextureGroup> groups = DefaultTextureGroups();
    if (!groupsPath.empty())
    {
        std::ifstream file(std::filesystem::path(groupsPath), std::ios::binary);
        if (file)
            ParseTextureGroups(std::string(std::istreambuf_iterator<char>(file), {}), groups);
    }
    return groups;
}

bool ImportTextureToKTex(const std::string& inputPath, const std::string& outputPath, std::string& error,
    const TextureImportOptions& options)
{
    if (!CanImportTextureSource(inputPath))
    {
        error = "Unsupported source format: " + Lowercase(std::filesystem::path(inputPath).extension().string());
        return false;
    }

    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initResult == S_OK || initResult == S_FALSE;
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
    {
        error = "Failed to initialize COM";
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    const std::wstring inputPathWide = Utf8ToWide(inputPath);
    if (inputPathWide.empty())
    {
        error = "Failed to convert input path";
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        error = "Failed to create WIC imaging factory";
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    hr = factory->CreateDecoderFromFilename(inputPathWide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr))
    {
        error = "Failed to open source image";
        SafeRelease(factory);
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        error = "Failed to decode image frame";
        SafeRelease(decoder);
        SafeRelease(factory);
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        error = "Failed to create image converter";
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    hr = converter->Initialize(
        frame,
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        error = "Failed to convert source image to RGBA8";
        SafeRelease(converter);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        error = "Invalid source image dimensions";
        SafeRelease(converter);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    std::vector<uint8_t> pixels((size_t)width * (size_t)height * 4u);
    hr = converter->CopyPixels(nullptr, width * 4u, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr))
    {
        error = "Failed to read converted image pixels";
        SafeRelease(converter);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    SafeRelease(converter);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(factory);
    if (shouldUninitialize)
        CoUninitialize();

    TextureFileData texture = {};
    texture.sourcePath = std::filesystem::path(inputPath).filename().string();
    {
        std::ifstream source(std::filesystem::path(inputPath), std::ios::binary);
        texture.sourceData.assign(std::istreambuf_iterator<char>(source), {});
        if (texture.sourceData.empty())
        {
            error = "Failed to read source image";
            return false;
        }
    }

    // A re-import keeps the texture's settings, as in UE.
    TextureFileData existing;
    if (options.group.empty() && std::filesystem::exists(std::filesystem::path(outputPath)) &&
        LoadKTexFile(outputPath.c_str(), existing) && existing.version >= 2)
        texture.settings = existing.settings;
    else
        texture.settings.group = options.group.empty() ? GuessTextureGroup(inputPath) : options.group;

    const std::vector<TextureGroup> groups = LoadProjectTextureGroups(options.groupsPath);
    const TextureGroupSettings resolved = ResolveTextureSettings(texture.settings, groups);
    if (!CookTexture(pixels.data(), width, height, resolved, texture, &error))
        return false;
    return SaveKTexFile(outputPath.c_str(), texture, &error);
}

