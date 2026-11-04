#include "text_block_file.h"
#include "pak_file_system.h"
#include "text_parse_utils.h"
#include <sstream>

namespace
{
    std::string StripComments(const std::string& value)
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
}

bool LoadTextBlockFile(const char* path, std::vector<TextBlock>& outBlocks, std::string* outError)
{
    outBlocks.clear();

    std::string fileText;
    if (!LoadAssetFileText(path, fileText, outError))
    {
        return false;
    }

    if (fileText.rfind("\xEF\xBB\xBF", 0) == 0)
        fileText.erase(0, 3);   // UTF-8 BOM

    std::istringstream file(fileText);
    bool inBlock = false;
    bool waitingForBrace = false;
    TextBlock current = {};
    std::string pendingType;
    int pendingLine = 0;

    std::string rawLine;
    int lineNumber = 0;
    while (std::getline(file, rawLine))
    {
        ++lineNumber;
        const std::string line = textparse::Trim(StripComments(rawLine));
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
            while (split < line.size() && !std::isspace(static_cast<unsigned char>(line[split])) && line[split] != '{')
                ++split;

            const std::string type = line.substr(0, split);
            const std::string remainder = textparse::Trim(line.substr(split));
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

        current.properties.push_back({
            textparse::Trim(line.substr(0, equals)),
            textparse::Trim(line.substr(equals + 1))
        });
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
            *outError = "Unclosed block in file";
        return false;
    }

    return true;
}

const std::string* FindTextBlockProperty(const TextBlock& block, const char* key)
{
    if (!key)
        return nullptr;

    for (const TextBlockProperty& property : block.properties)
    {
        if (property.key == key)
            return &property.value;
    }

    return nullptr;
}
