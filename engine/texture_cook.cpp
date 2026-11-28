#include "texture_cook.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <cmp_core.h>

namespace
{
    struct Image
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    float SrgbToLinear(uint8_t value)
    {
        static const auto table = []
        {
            std::vector<float> t(256);
            for (int i = 0; i < 256; ++i)
            {
                const float c = i / 255.0f;
                t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            return t;
        }();
        return table[value];
    }

    uint8_t ToByte(float c)
    {
        return static_cast<uint8_t>(std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    uint8_t LinearToSrgb(float c)
    {
        c = std::clamp(c, 0.0f, 1.0f);
        return ToByte(c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    }

    enum class MipMode
    {
        Linear,
        Srgb,
        Normal,
    };

    Image Downsample(const Image& src, MipMode mode)
    {
        Image dst;
        dst.width = std::max(src.width / 2, 1u);
        dst.height = std::max(src.height / 2, 1u);
        dst.rgba.resize(static_cast<size_t>(dst.width) * dst.height * 4);
        for (uint32_t y = 0; y < dst.height; ++y)
        {
            for (uint32_t x = 0; x < dst.width; ++x)
            {
                float sum[4] = {};
                for (uint32_t dy = 0; dy < 2; ++dy)
                {
                    for (uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const uint32_t sx = std::min(x * 2 + dx, src.width - 1);
                        const uint32_t sy = std::min(y * 2 + dy, src.height - 1);
                        const uint8_t* p = &src.rgba[(static_cast<size_t>(sy) * src.width + sx) * 4];
                        for (int c = 0; c < 3; ++c)
                        {
                            if (mode == MipMode::Srgb)
                                sum[c] += SrgbToLinear(p[c]);
                            else if (mode == MipMode::Normal)
                                sum[c] += p[c] / 127.5f - 1.0f;
                            else
                                sum[c] += p[c] / 255.0f;
                        }
                        sum[3] += p[3] / 255.0f;
                    }
                }
                uint8_t* out = &dst.rgba[(static_cast<size_t>(y) * dst.width + x) * 4];
                if (mode == MipMode::Normal)
                {
                    const float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
                    for (int c = 0; c < 3; ++c)
                        out[c] = ToByte(sum[c] * inv * 0.5f + 0.5f);
                }
                else
                {
                    for (int c = 0; c < 3; ++c)
                        out[c] = mode == MipMode::Srgb ? LinearToSrgb(sum[c] * 0.25f) : ToByte(sum[c] * 0.25f);
                }
                out[3] = ToByte(sum[3] * 0.25f);
            }
        }
        return dst;
    }

    template<typename Fn>
    void ParallelFor(uint32_t count, Fn fn)
    {
        const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, std::max(count, 1u));
        if (threads <= 1)
        {
            for (uint32_t i = 0; i < count; ++i)
                fn(i);
            return;
        }
        std::atomic<uint32_t> next{ 0 };
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (uint32_t t = 0; t < threads; ++t)
        {
            workers.emplace_back([&]
            {
                for (uint32_t i = next++; i < count; i = next++)
                    fn(i);
            });
        }
        for (std::thread& worker : workers)
            worker.join();
    }

    // Edge pixels repeat past the image.
    void GatherBlock(const Image& image, uint32_t bx, uint32_t by, uint8_t out[64])
    {
        for (uint32_t y = 0; y < 4; ++y)
        {
            const uint32_t sy = std::min(by * 4 + y, image.height - 1);
            for (uint32_t x = 0; x < 4; ++x)
            {
                const uint32_t sx = std::min(bx * 4 + x, image.width - 1);
                std::memcpy(&out[(y * 4 + x) * 4], &image.rgba[(static_cast<size_t>(sy) * image.width + sx) * 4], 4);
            }
        }
    }

    void CompressMip(const Image& image, TexturePixelFormat format, uint8_t* out)
    {
        const uint32_t blocksX = (image.width + 3) / 4;
        const uint32_t blocksY = (image.height + 3) / 4;
        const uint32_t blockBytes = TextureBlockBytes(format);
        ParallelFor(blocksY, [&](uint32_t by)
        {
            void* options = nullptr;
            switch (format)
            {
            case TexturePixelFormat::BC1: CreateOptionsBC1(&options); break;
            case TexturePixelFormat::BC4: CreateOptionsBC4(&options); break;
            case TexturePixelFormat::BC5: CreateOptionsBC5(&options); break;
            case TexturePixelFormat::BC7:
                CreateOptionsBC7(&options);
                SetQualityBC7(options, 0.1f);
                break;
            default: break;
            }
            uint8_t block[64];
            uint8_t red[16];
            uint8_t green[16];
            for (uint32_t bx = 0; bx < blocksX; ++bx)
            {
                GatherBlock(image, bx, by, block);
                uint8_t* dst = out + (static_cast<size_t>(by) * blocksX + bx) * blockBytes;
                switch (format)
                {
                case TexturePixelFormat::BC1: CompressBlockBC1(block, 16, dst, options); break;
                case TexturePixelFormat::BC7: CompressBlockBC7(block, 16, dst, options); break;
                case TexturePixelFormat::BC4:
                    for (int i = 0; i < 16; ++i)
                        red[i] = block[i * 4];
                    CompressBlockBC4(red, 4, dst, options);
                    break;
                case TexturePixelFormat::BC5:
                    for (int i = 0; i < 16; ++i)
                    {
                        red[i] = block[i * 4];
                        green[i] = block[i * 4 + 1];
                    }
                    CompressBlockBC5(red, 4, green, 4, dst, options);
                    break;
                default: break;
                }
            }
            switch (format)
            {
            case TexturePixelFormat::BC1: DestroyOptionsBC1(options); break;
            case TexturePixelFormat::BC4: DestroyOptionsBC4(options); break;
            case TexturePixelFormat::BC5: DestroyOptionsBC5(options); break;
            case TexturePixelFormat::BC7: DestroyOptionsBC7(options); break;
            default: break;
            }
        });
    }

    bool DecodeMip(const TextureFileData& texture, uint32_t mip, bool raw, std::vector<uint8_t>& outRgba)
    {
        outRgba.clear();
        if (mip >= texture.mipCount || !ValidateTextureData(texture, nullptr))
            return false;
        const uint32_t width = TextureMipWidth(texture.width, mip);
        const uint32_t height = TextureMipWidth(texture.height, mip);
        const uint8_t* src = texture.pixels.data() + TextureMipOffset(texture, mip);
        outRgba.resize(static_cast<size_t>(width) * height * 4);
        if (texture.format == TexturePixelFormat::RGBA8)
        {
            std::memcpy(outRgba.data(), src, outRgba.size());
            return true;
        }

        const uint32_t blocksX = (width + 3) / 4;
        const uint32_t blocksY = (height + 3) / 4;
        const uint32_t blockBytes = TextureBlockBytes(texture.format);
        ParallelFor(blocksY, [&](uint32_t by)
        {
            uint8_t block[64];
            uint8_t red[16];
            uint8_t green[16];
            for (uint32_t bx = 0; bx < blocksX; ++bx)
            {
                const uint8_t* cmp = src + (static_cast<size_t>(by) * blocksX + bx) * blockBytes;
                switch (texture.format)
                {
                case TexturePixelFormat::BC1: DecompressBlockBC1(cmp, block, nullptr); break;
                case TexturePixelFormat::BC7: DecompressBlockBC7(cmp, block, nullptr); break;
                case TexturePixelFormat::BC4:
                    DecompressBlockBC4(cmp, red, nullptr);
                    for (int i = 0; i < 16; ++i)
                    {
                        block[i * 4 + 0] = red[i];
                        block[i * 4 + 1] = raw ? 0 : red[i];
                        block[i * 4 + 2] = raw ? 0 : red[i];
                        block[i * 4 + 3] = 255;
                    }
                    break;
                case TexturePixelFormat::BC5:
                    DecompressBlockBC5(cmp, red, green, nullptr);
                    for (int i = 0; i < 16; ++i)
                    {
                        block[i * 4 + 0] = red[i];
                        block[i * 4 + 1] = green[i];
                        uint8_t z = 0;
                        if (!raw)
                        {
                            const float nx = red[i] / 127.5f - 1.0f;
                            const float ny = green[i] / 127.5f - 1.0f;
                            z = ToByte(std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny)) * 0.5f + 0.5f);
                        }
                        block[i * 4 + 2] = z;
                        block[i * 4 + 3] = 255;
                    }
                    break;
                default: break;
                }
                for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                {
                    const uint32_t x0 = bx * 4;
                    const uint32_t count = std::min(4u, width - x0);
                    std::memcpy(&outRgba[(static_cast<size_t>(by * 4 + y) * width + x0) * 4], &block[y * 16], count * 4);
                }
            }
        });
        return true;
    }
}

TexturePixelFormat CookedTextureFormat(TextureCompression compression)
{
    switch (compression)
    {
    case TextureCompression::Default:
    case TextureCompression::Masks: return TexturePixelFormat::BC7;
    case TextureCompression::DefaultSmall: return TexturePixelFormat::BC1;
    case TextureCompression::NormalMap: return TexturePixelFormat::BC5;
    case TextureCompression::Grayscale: return TexturePixelFormat::BC4;
    case TextureCompression::UserInterface:
    case TextureCompression::Uncompressed: return TexturePixelFormat::RGBA8;
    }
    return TexturePixelFormat::RGBA8;
}

bool CookTexture(const uint8_t* rgba, uint32_t width, uint32_t height, const TextureGroupSettings& settings,
    TextureFileData& texture, std::string* outError)
{
    if (!rgba || width == 0 || height == 0)
    {
        if (outError)
            *outError = "Empty source image";
        return false;
    }

    const bool normalMap = settings.compression == TextureCompression::NormalMap;
    const MipMode mode = normalMap ? MipMode::Normal : settings.srgb ? MipMode::Srgb : MipMode::Linear;

    std::vector<Image> mips(1);
    mips[0].width = width;
    mips[0].height = height;
    mips[0].rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    uint32_t drop = static_cast<uint32_t>(std::max(0, settings.lodBias));
    while (settings.maxSize > 0 && std::max(mips[0].width, mips[0].height) > settings.maxSize && (mips[0].width > 1 || mips[0].height > 1))
        mips[0] = Downsample(mips[0], mode);
    for (; drop > 0 && (mips[0].width > 1 || mips[0].height > 1); --drop)
        mips[0] = Downsample(mips[0], mode);
    if (settings.mipGen != TextureMipGen::NoMipmaps)
    {
        while (mips.back().width > 1 || mips.back().height > 1)
            mips.push_back(Downsample(mips.back(), mode));
    }

    const TexturePixelFormat format = CookedTextureFormat(settings.compression);
    texture.width = mips[0].width;
    texture.height = mips[0].height;
    texture.format = format;
    texture.mipCount = static_cast<uint32_t>(mips.size());

    uint32_t flags = 0;
    if (settings.srgb)
        flags |= TextureFileFlag_Srgb;
    if (normalMap)
        flags |= TextureFileFlag_NormalMap;
    if (settings.addressU == TextureAddress::Clamp) flags |= TextureFileFlag_ClampU;
    if (settings.addressV == TextureAddress::Clamp) flags |= TextureFileFlag_ClampV;
    if (settings.addressU == TextureAddress::Mirror) flags |= TextureFileFlag_MirrorU;
    if (settings.addressV == TextureAddress::Mirror) flags |= TextureFileFlag_MirrorV;
    switch (settings.filter)
    {
    case TextureFilter::Nearest: flags |= TextureFileFlag_PointFiltered | TextureFileFlag_NoAnisotropy; break;
    case TextureFilter::Bilinear: flags |= TextureFileFlag_NoAnisotropy | TextureFileFlag_NoMipFilter; break;
    case TextureFilter::Trilinear: flags |= TextureFileFlag_NoAnisotropy; break;
    case TextureFilter::Default: break;
    }
    texture.flags = flags;

    texture.pixels.assign(TextureMipOffset(texture, texture.mipCount), 0);
    for (uint32_t mip = 0; mip < texture.mipCount; ++mip)
    {
        uint8_t* dst = texture.pixels.data() + TextureMipOffset(texture, mip);
        if (format == TexturePixelFormat::RGBA8)
            std::memcpy(dst, mips[mip].rgba.data(), mips[mip].rgba.size());
        else
            CompressMip(mips[mip], format, dst);
    }
    return true;
}

bool DecodeTextureMip(const TextureFileData& texture, uint32_t mip, std::vector<uint8_t>& outRgba)
{
    return DecodeMip(texture, mip, false, outRgba);
}

bool DecodeTextureMipRaw(const TextureFileData& texture, uint32_t mip, std::vector<uint8_t>& outRgba)
{
    return DecodeMip(texture, mip, true, outRgba);
}
