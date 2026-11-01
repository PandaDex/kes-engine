#pragma once
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

struct String
{
    std::string value;

    String() = default;
    String(const char* v) : value(v ? v : "") {}
    String(const std::string& v) : value(v) {}

    size_t length() const
    {
        return value.length();
    }

    bool empty() const
    {
        return value.empty();
    }

    const char* c_str() const
    {
        return value.c_str();
    }

    String charAt(int index) const
    {
        if (index < 0 || static_cast<size_t>(index) >= value.size())
            return {};
        return std::string(1, value[static_cast<size_t>(index)]);
    }

    int indexOf(const String& search, int startIndex = 0) const
    {
        if (startIndex < 0)
            startIndex = 0;
        if (static_cast<size_t>(startIndex) > value.size())
            return -1;

        const size_t found = value.find(search.value, static_cast<size_t>(startIndex));
        return found == std::string::npos ? -1 : static_cast<int>(found);
    }

    bool includes(const String& search, int startIndex = 0) const
    {
        return indexOf(search, startIndex) >= 0;
    }

    bool startsWith(const String& search, int startIndex = 0) const
    {
        if (startIndex < 0)
            startIndex = 0;
        if (static_cast<size_t>(startIndex) > value.size())
            return false;
        return value.compare(static_cast<size_t>(startIndex), search.value.size(), search.value) == 0;
    }

    bool endsWith(const String& search) const
    {
        if (search.value.size() > value.size())
            return false;
        return value.compare(value.size() - search.value.size(), search.value.size(), search.value) == 0;
    }

    String substring(int startIndex, int endIndex = -1) const
    {
        if (startIndex < 0)
            startIndex = 0;
        if (endIndex < 0 || static_cast<size_t>(endIndex) > value.size())
            endIndex = static_cast<int>(value.size());
        if (startIndex > endIndex)
        {
            const int temp = startIndex;
            startIndex = endIndex;
            endIndex = temp;
        }

        return value.substr(static_cast<size_t>(startIndex), static_cast<size_t>(endIndex - startIndex));
    }

    String toLowerCase() const
    {
        String result = *this;
        std::transform(result.value.begin(), result.value.end(), result.value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    String toUpperCase() const
    {
        String result = *this;
        std::transform(result.value.begin(), result.value.end(), result.value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return result;
    }

    String trim() const
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            ++start;

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
            --end;

        return value.substr(start, end - start);
    }

    std::vector<String> split(const String& separator) const
    {
        std::vector<String> parts;
        if (separator.value.empty())
        {
            parts.reserve(value.size());
            for (char ch : value)
                parts.emplace_back(std::string(1, ch));
            return parts;
        }

        size_t start = 0;
        while (start <= value.size())
        {
            const size_t found = value.find(separator.value, start);
            if (found == std::string::npos)
            {
                parts.emplace_back(value.substr(start));
                break;
            }

            parts.emplace_back(value.substr(start, found - start));
            start = found + separator.value.size();
        }

        return parts;
    }

    String operator+(const String& other) const
    {
        return value + other.value;
    }

    String& operator+=(const String& other)
    {
        value += other.value;
        return *this;
    }

    bool operator==(const String& other) const { return value == other.value; }
    bool operator!=(const String& other) const { return value != other.value; }

    operator std::string() const { return value; }
};

struct Float
{
    float value = 0.0f;

    Float() = default;
    Float(float v) : value(v) {}

    int toInt() const
    {
        return static_cast<int>(value);
    }

    static float random(float max)
    {
        if (max <= 0.0f)
            return 0.0f;
        return (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * max;
    }

    static float randomInRange(float min, float max, float step = 0.0f)
    {
        if (max < min)
        {
            const float temp = min;
            min = max;
            max = temp;
        }

        if (step > 0.0f)
        {
            const int count = static_cast<int>((max - min) / step);
            if (count <= 0)
                return min;
            const int index = std::rand() % (count + 1);
            return min + static_cast<float>(index) * step;
        }

        return min + (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * (max - min);
    }

    operator float() const { return value; }
};

struct Int
{
    int value = 0;

    Int() = default;
    Int(int v) : value(v) {}

    float toFloat() const
    {
        return static_cast<float>(value);
    }

    static int random(int max)
    {
        if (max <= 0)
            return 0;
        return std::rand() % max;
    }

    static int randomInRange(int min, int max)
    {
        if (max < min)
        {
            const int temp = min;
            min = max;
            max = temp;
        }
        return min + (std::rand() % (max - min + 1));
    }

    operator int() const { return value; }
};

struct Boolean
{
    bool value = false;

    Boolean() = default;
    Boolean(bool v) : value(v) {}

    int toInt() const
    {
        return value ? 1 : 0;
    }

    operator bool() const { return value; }
};

struct Vector2
{
    float x = 0.0f;
    float y = 0.0f;

    Vector2() = default;
    Vector2(float vx, float vy) : x(vx), y(vy) {}

    Vector2 operator+(const Vector2& other) const
    {
        return { x + other.x, y + other.y };
    }

    Vector2 operator-(const Vector2& other) const
    {
        return { x - other.x, y - other.y };
    }

    Vector2 operator*(float scalar) const
    {
        return { x * scalar, y * scalar };
    }

    Vector2 operator/(float scalar) const
    {
        if (scalar == 0.0f) return { 0.0f, 0.0f };
        return { x / scalar, y / scalar };
    }

    Vector2& operator+=(const Vector2& other)
    {
        x += other.x;
        y += other.y;
        return *this;
    }

    Vector2& operator-=(const Vector2& other)
    {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    Vector2& operator*=(float scalar)
    {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    float Length() const
    {
        return std::sqrt(x * x + y * y);
    }

    float LengthSq() const
    {
        return x * x + y * y;
    }

    Vector2 Normalized() const
    {
        float len = Length();
        if (len <= 0.00001f)
            return { 0.0f, 0.0f };

        return { x / len, y / len };
    }

    static float Dot(const Vector2& a, const Vector2& b)
    {
        return a.x * b.x + a.y * b.y;
    }

    static float Distance(const Vector2& a, const Vector2& b)
    {
        return (a - b).Length();
    }

    static float DistanceSq(const Vector2& a, const Vector2& b)
    {
        return (a - b).LengthSq();
    }

    static Vector2 Lerp(const Vector2& a, const Vector2& b, float t)
    {
        return a + (b - a) * t;
    }

    static Vector2 Reflect(const Vector2& direction, const Vector2& normal)
    {
        return direction - normal * (2.0f * Dot(direction, normal));
    }
};

inline Vector2 operator*(float scalar, const Vector2& v)
{
    return { v.x * scalar, v.y * scalar };
}

struct Vector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3() = default;
    Vector3(float vx, float vy, float vz) : x(vx), y(vy), z(vz) {}

    Vector3 operator+(const Vector3& other) const
    {
        return { x + other.x, y + other.y, z + other.z };
    }

    Vector3 operator-(const Vector3& other) const
    {
        return { x - other.x, y - other.y, z - other.z };
    }

    Vector3 operator*(float scalar) const
    {
        return { x * scalar, y * scalar, z * scalar };
    }

    Vector3 operator/(float scalar) const
    {
        if (scalar == 0.0f) return { 0.0f, 0.0f, 0.0f };
        return { x / scalar, y / scalar, z / scalar };
    }

    Vector3& operator+=(const Vector3& other)
    {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    Vector3& operator-=(const Vector3& other)
    {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        return *this;
    }

    Vector3& operator*=(float scalar)
    {
        x *= scalar;
        y *= scalar;
        z *= scalar;
        return *this;
    }

    float Length() const
    {
        return std::sqrt(x * x + y * y + z * z);
    }

    float LengthSq() const
    {
        return x * x + y * y + z * z;
    }

    Vector3 Normalized() const
    {
        float len = Length();
        if (len <= 0.00001f)
            return { 0.0f, 0.0f, 0.0f };

        return { x / len, y / len, z / len };
    }

    static float Dot(const Vector3& a, const Vector3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vector3 Cross(const Vector3& a, const Vector3& b)
    {
        return
        {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    static Vector3 Lerp(const Vector3& a, const Vector3& b, float t)
    {
        return a + (b - a) * t;
    }

    static Vector3 Reflect(const Vector3& direction, const Vector3& normal)
    {
        return direction - normal * (2.0f * Dot(direction, normal));
    }

    static float Distance(const Vector3& a, const Vector3& b)
    {
        return (a - b).Length();
    }

    static float DistanceSq(const Vector3& a, const Vector3& b)
    {
        return (a - b).LengthSq();
    }
};

inline Vector3 operator*(float scalar, const Vector3& v)
{
    return { v.x * scalar, v.y * scalar, v.z * scalar };
}

inline float Vector3Dot(const Vector3& a, const Vector3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vector3 Vector3Cross(const Vector3& a, const Vector3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float Vector3LengthSq(const Vector3& v)
{
    return Vector3Dot(v, v);
}

inline float Vector3Length(const Vector3& v)
{
    return std::sqrt(Vector3LengthSq(v));
}

inline Vector3 Vector3Normalize(const Vector3& v)
{
    const float len = Vector3Length(v);
    if (len <= 0.00001f)
        return { 0.0f, 0.0f, 0.0f };
    return v / len;
}

inline Vector3 Vector3DirectionFromRotation(const Vector3& rotation)
{
    const float cp = std::cos(rotation.x);
    const float sp = std::sin(rotation.x);
    const float cy = std::cos(rotation.y);
    const float sy = std::sin(rotation.y);
    return Vector3Normalize({ sy * cp, sp, cy * cp });
}

inline float ClampFloat(float v, float minV, float maxV)
{
    if (v < minV) return minV;
    if (v > maxV) return maxV;
    return v;
}

inline float LerpFloat(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline int ClampInt(int v, int minV, int maxV)
{
    if (v < minV) return minV;
    if (v > maxV) return maxV;
    return v;
}

struct Transform
{
    Vector3 position = {};
    Vector3 rotation = {0.0f, 0.0f, 0.0f};
    Vector3 scale = { 1.0f, 1.0f, 1.0f };
};
