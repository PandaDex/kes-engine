#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

// Bounds-checked little-endian reader over bytes in memory. A failed read
// leaves the reader failed; later reads return zeroes, so callers can read a
// whole record and check ok() once.
class BinaryReader
{
public:
    BinaryReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
    explicit BinaryReader(const std::vector<uint8_t>& bytes) : m_data(bytes.data()), m_size(bytes.size()) {}

    bool ok() const { return m_ok; }
    size_t position() const { return m_pos; }
    size_t remaining() const { return m_ok ? m_size - m_pos : 0; }

    bool bytes(void* out, size_t count)
    {
        if (!m_ok || count > m_size - m_pos)
        {
            m_ok = false;
            return false;
        }
        if (count > 0)
            std::memcpy(out, m_data + m_pos, count);
        m_pos += count;
        return true;
    }

    template<typename T>
    T read()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        bytes(&value, sizeof(T));
        return value;
    }

    template<typename T>
    bool read(T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        return bytes(&value, sizeof(T));
    }

    float f32() { return read<float>(); }
    uint32_t u32() { return read<uint32_t>(); }
    int32_t i32() { return read<int32_t>(); }
    uint8_t u8() { return read<uint8_t>(); }

    std::string string()
    {
        const uint32_t length = u32();
        if (!m_ok || length > remaining())
        {
            m_ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(m_data + m_pos), length);
        m_pos += length;
        return value;
    }

    // Guards an element count against the bytes actually left, so a corrupt
    // count can't trigger a huge allocation.
    bool count(uint64_t value, size_t minBytesPerElement)
    {
        if (minBytesPerElement > 0 && value > remaining() / minBytesPerElement)
            m_ok = false;
        return m_ok;
    }

    void skip(size_t count)
    {
        if (!m_ok || count > m_size - m_pos)
        {
            m_ok = false;
            return;
        }
        m_pos += count;
    }

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos = 0;
    bool m_ok = true;
};
