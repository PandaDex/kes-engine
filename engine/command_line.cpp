#include "command_line.h"
#include <cctype>
#include <cstdlib>

namespace
{
    std::vector<std::string> gArguments;

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    // "-ResX=1280" / "--resx=1280" -> name "ResX", value "1280", hasValue true.
    bool SplitOption(std::string_view arg, std::string_view& outName, std::string_view& outValue, bool& outHasValue)
    {
        if (arg.size() < 2 || arg[0] != '-')
            return false;
        arg.remove_prefix(arg[1] == '-' ? 2 : 1);
        const size_t eq = arg.find('=');
        outHasValue = eq != std::string_view::npos;
        outName = outHasValue ? arg.substr(0, eq) : arg;
        outValue = outHasValue ? arg.substr(eq + 1) : std::string_view();
        return !outName.empty();
    }
}

void CommandLineInit(int argc, const char* const* argv)
{
    gArguments.clear();
    if (!argv)
        return;
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i])
            gArguments.emplace_back(argv[i]);
    }
}

const std::vector<std::string>& CommandLineGetArguments()
{
    return gArguments;
}

bool CommandLineHasFlag(std::string_view name)
{
    for (const std::string& arg : gArguments)
    {
        std::string_view optionName, value;
        bool hasValue = false;
        if (SplitOption(arg, optionName, value, hasValue) && EqualsIgnoreCase(optionName, name))
            return true;
    }
    return false;
}

bool CommandLineGetValue(std::string_view name, std::string& outValue)
{
    for (size_t i = 0; i < gArguments.size(); ++i)
    {
        std::string_view optionName, value;
        bool hasValue = false;
        if (!SplitOption(gArguments[i], optionName, value, hasValue) || !EqualsIgnoreCase(optionName, name))
            continue;

        if (hasValue)
        {
            outValue.assign(value);
            return true;
        }
        if (i + 1 < gArguments.size() && !gArguments[i + 1].empty() && gArguments[i + 1][0] != '-')
        {
            outValue = gArguments[i + 1];
            return true;
        }
    }
    return false;
}

bool CommandLineGetInt(std::string_view name, int& outValue)
{
    std::string text;
    if (!CommandLineGetValue(name, text) || text.empty())
        return false;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;
    outValue = static_cast<int>(value);
    return true;
}

bool CommandLineGetFloat(std::string_view name, float& outValue)
{
    std::string text;
    if (!CommandLineGetValue(name, text) || text.empty())
        return false;
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (!end || *end != '\0')
        return false;
    outValue = value;
    return true;
}
