#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// 128-bit identity of an entity. Map entities keep the GUID the Forge editor gave them when
// they were placed (stored in the .fmapc); entities created at runtime get a random one.
// Byte order follows RFC 4122, so ToString() matches the editor's QUuid text.
struct Guid
{
    uint64_t high = 0;
    uint64_t low = 0;

    bool IsValid() const { return high != 0 || low != 0; }

    static Guid Generate();   // random (version 4)
    // 16 bytes in RFC 4122 (big-endian) order, as written by the map compiler.
    static Guid FromBytes(const uint8_t bytes[16]);
    // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", optionally in braces. False on bad input.
    static bool Parse(std::string_view text, Guid& out);
    std::string ToString() const;

    friend bool operator==(const Guid& a, const Guid& b) { return a.high == b.high && a.low == b.low; }
    friend bool operator!=(const Guid& a, const Guid& b) { return !(a == b); }
};

struct GuidHash
{
    size_t operator()(const Guid& guid) const
    {
        return static_cast<size_t>(guid.high ^ (guid.low * 0x9E3779B97F4A7C15ull));
    }
};
