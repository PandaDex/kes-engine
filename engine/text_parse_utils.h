#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine_types.h"

namespace textparse
{
    inline std::string Trim(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            ++start;

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
            --end;

        return value.substr(start, end - start);
    }

    inline std::string Unquote(const std::string& value)
    {
        if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            return value;

        std::string out;
        out.reserve(value.size() - 2);
        for (size_t i = 1; i + 1 < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 1 < value.size() - 1)
            {
                ++i;
                out.push_back(value[i]);
                continue;
            }

            out.push_back(value[i]);
        }

        return out;
    }

    inline std::string Quote(const std::string& value)
    {
        return "\"" + value + "\"";
    }

    inline std::string ParseString(const std::string& value)
    {
        return Unquote(Trim(value));
    }

    inline std::vector<std::string> SplitList(const std::string& value)
    {
        std::vector<std::string> parts;
        std::string current;
        for (char ch : Unquote(value))
        {
            if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))
            {
                if (!current.empty())
                {
                    parts.push_back(current);
                    current.clear();
                }
                continue;
            }

            current.push_back(ch);
        }

        if (!current.empty())
            parts.push_back(current);

        return parts;
    }

    inline bool ParseBool(const std::string& value, bool fallback)
    {
        const std::string cleaned = ParseString(value);
        if (cleaned == "true" || cleaned == "1")
            return true;
        if (cleaned == "false" || cleaned == "0")
            return false;
        return fallback;
    }

    inline float ParseFloat(const std::string& value, float fallback)
    {
        const std::string cleaned = ParseString(value);
        char* end = nullptr;
        const float parsed = std::strtof(cleaned.c_str(), &end);
        if (end == cleaned.c_str())
            return fallback;
        return parsed;
    }

    inline Vector2 ParseVector2(const std::string& value, const Vector2& fallback)
    {
        const std::vector<std::string> parts = SplitList(value);
        if (parts.size() < 2)
            return fallback;

        return {
            std::strtof(parts[0].c_str(), nullptr),
            std::strtof(parts[1].c_str(), nullptr)
        };
    }

    inline Vector3 ParseVector3(const std::string& value, const Vector3& fallback)
    {
        const std::vector<std::string> parts = SplitList(value);
        if (parts.size() < 3)
            return fallback;

        return {
            std::strtof(parts[0].c_str(), nullptr),
            std::strtof(parts[1].c_str(), nullptr),
            std::strtof(parts[2].c_str(), nullptr)
        };
    }

    inline void ParseColor4(const std::string& value, float out[4], const float fallback[4])
    {
        for (int i = 0; i < 4; ++i)
            out[i] = fallback[i];

        const std::vector<std::string> parts = SplitList(value);
        for (size_t i = 0; i < parts.size() && i < 4; ++i)
            out[i] = std::strtof(parts[i].c_str(), nullptr);
    }
}
