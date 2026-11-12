#include "guid.h"
#include <chrono>
#include <random>

namespace
{
    int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
}

Guid Guid::Generate()
{
    thread_local std::mt19937_64 rng = []
    {
        std::random_device device;
        std::seed_seq seed{ device(), device(), device(), device(),
                            static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) };
        return std::mt19937_64(seed);
    }();

    Guid guid;
    do
    {
        guid.high = rng();
        guid.low = rng();
        guid.high = (guid.high & ~0xF000ull) | 0x4000ull;                           // version 4
        guid.low = (guid.low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;      // RFC 4122 variant
    } while (!guid.IsValid());
    return guid;
}

Guid Guid::FromBytes(const uint8_t bytes[16])
{
    Guid guid;
    for (int i = 0; i < 8; ++i)
    {
        guid.high = (guid.high << 8) | bytes[i];
        guid.low = (guid.low << 8) | bytes[8 + i];
    }
    return guid;
}

bool Guid::Parse(std::string_view text, Guid& out)
{
    if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        text = text.substr(1, 36);
    if (text.size() != 36)
        return false;

    uint8_t bytes[16] = {};
    size_t byte = 0;
    for (size_t i = 0; i < text.size();)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (text[i] != '-')
                return false;
            ++i;
            continue;
        }
        const int hi = HexValue(text[i]);
        const int lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        bytes[byte++] = static_cast<uint8_t>((hi << 4) | lo);
        i += 2;
    }
    out = FromBytes(bytes);
    return true;
}

std::string Guid::ToString() const
{
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (int i = 0; i < 32; ++i)
    {
        if (i == 8 || i == 12 || i == 16 || i == 20)
            text.push_back('-');
        const uint64_t half = i < 16 ? high : low;
        const int shift = 60 - (i % 16) * 4;
        text.push_back(kDigits[(half >> shift) & 0xF]);
    }
    return text;
}
