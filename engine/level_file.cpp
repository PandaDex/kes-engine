#include "level_file.h"
#include <cctype>
#include <sstream>

#include "pak_file_system.h"

namespace
{
    static std::string Trim(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace((unsigned char)value[start]))
            ++start;

        size_t end = value.size();
        while (end > start && std::isspace((unsigned char)value[end - 1]))
            --end;

        return value.substr(start, end - start);
    }

    static std::string StripComments(const std::string& value)
    {
        bool inQuotes = false;
        bool escape = false;

        for (size_t i = 0; i < value.size(); ++i)
        {
            const char ch = value[i];

            if (escape)
            {
                escape = false;
                continue;
            }

            if (ch == '\\')
            {
                escape = true;
                continue;
            }

            if (ch == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }

            if (!inQuotes)
            {
                if (ch == '#')
                    return value.substr(0, i);
                if (ch == '/' && i + 1 < value.size() && value[i + 1] == '/')
                    return value.substr(0, i);
            }
        }

        return value;
    }

    static bool ParseBlockType(const std::string& token, LevelBlockType& outType)
    {
        if (token == "Object") { outType = LevelBlockType::Object; return true; }
        if (token == "Entity") { outType = LevelBlockType::Entity; return true; }
        if (token == "Collider") { outType = LevelBlockType::Collider; return true; }
        if (token == "Prefab") { outType = LevelBlockType::Prefab; return true; }
        return false;
    }
}

bool LoadLevelFile(const char* path, std::vector<LevelBlock>& outBlocks, std::string* outError)
{
    outBlocks.clear();

    std::string fileText;
    if (!LoadAssetFileText(path, fileText, outError))
    {
        return false;
    }

    std::istringstream file(fileText);
    bool inBlock = false;
    bool waitingForBrace = false;
    LevelBlock current = {};
    LevelBlockType pendingType = LevelBlockType::Object;
    int pendingLine = 0;

    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(file, rawLine))
    {
        ++lineNumber;
        const std::string line = Trim(StripComments(rawLine));
        if (line.empty())
            continue;

        if (waitingForBrace)
        {
            if (line != "{")
            {
                if (outError)
                    *outError = "Expected '{' after block header at line " + std::to_string(pendingLine);
                return false;
            }

            waitingForBrace = false;
            inBlock = true;
            current = {};
            current.type = pendingType;
            current.line = pendingLine;
            continue;
        }

        if (!inBlock)
        {
            size_t split = 0;
            while (split < line.size() && !std::isspace((unsigned char)line[split]) && line[split] != '{')
                ++split;

            LevelBlockType type = LevelBlockType::Object;
            if (!ParseBlockType(line.substr(0, split), type))
            {
                if (outError)
                    *outError = "Unknown block type at line " + std::to_string(lineNumber);
                return false;
            }

            const std::string remainder = Trim(line.substr(split));
            if (remainder == "{")
            {
                inBlock = true;
                current = {};
                current.type = type;
                current.line = lineNumber;
                continue;
            }

            if (remainder.empty())
            {
                waitingForBrace = true;
                pendingType = type;
                pendingLine = lineNumber;
                continue;
            }

            if (outError)
                *outError = "Unexpected content after block header at line " + std::to_string(lineNumber);
            return false;
        }

        if (line == "}")
        {
            outBlocks.push_back(current);
            current = {};
            inBlock = false;
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            if (outError)
                *outError = "Expected key=value at line " + std::to_string(lineNumber);
            return false;
        }

        current.properties.push_back({ Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)) });
    }

    if (waitingForBrace)
    {
        if (outError)
            *outError = "Expected '{' after block header at end of file";
        return false;
    }

    if (inBlock)
    {
        if (outError)
            *outError = "Unclosed block in level file";
        return false;
    }

    return true;
}

const char* GetLevelBlockTypeName(LevelBlockType type)
{
    switch (type)
    {
    case LevelBlockType::Object: return "Object";
    case LevelBlockType::Entity: return "Entity";
    case LevelBlockType::Collider: return "Collider";
    case LevelBlockType::Prefab: return "Prefab";
    default: return "Unknown";
    }
}

const std::string* FindLevelProperty(const LevelBlock& block, const char* key)
{
    if (!key)
        return nullptr;

    for (const LevelProperty& property : block.properties)
    {
        if (property.key == key)
            return &property.value;
    }

    return nullptr;
}

std::vector<std::string> FindLevelProperties(const LevelBlock& block, const char* key)
{
    std::vector<std::string> values;
    if (!key)
        return values;

    for (const LevelProperty& property : block.properties)
    {
        if (property.key == key)
            values.push_back(property.value);
    }

    return values;
}
