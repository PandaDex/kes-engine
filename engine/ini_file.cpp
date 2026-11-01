#include "ini_file.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
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

    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }
}

bool IniFile::LoadFromFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    LoadFromString(stream.str());
    return true;
}

void IniFile::LoadFromString(std::string_view text)
{
    m_sections.clear();
    Section* current = nullptr;

    size_t start = 0;
    while (start <= text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = Trim(text.substr(start, end - start));
        start = end + 1;

        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        if (line.front() == '[' && line.back() == ']')
        {
            current = &GetOrAddSection(Trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || !current)
            continue;

        const std::string_view key = Trim(line.substr(0, eq));
        std::string_view value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (!key.empty())
            SetString(current->name, key, value);
    }
}

std::string IniFile::ToString() const
{
    std::string out;
    for (const Section& section : m_sections)
    {
        if (!out.empty())
            out += '\n';
        out += '[';
        out += section.name;
        out += "]\n";
        for (const Entry& entry : section.entries)
        {
            out += entry.key;
            out += '=';
            out += entry.value;
            out += '\n';
        }
    }
    return out;
}

bool IniFile::SaveToFile(const std::string& path) const
{
    std::error_code error;
    const std::filesystem::path target(path);
    if (target.has_parent_path())
        std::filesystem::create_directories(target.parent_path(), error);

    // Write to a temporary file first so a crash mid-write never leaves a
    // truncated settings file behind.
    const std::filesystem::path temp = target.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;
        const std::string text = ToString();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file.good())
            return false;
    }
    std::filesystem::rename(temp, target, error);
    if (error)
    {
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temp, target, error);
    }
    return !error;
}

void IniFile::Merge(const IniFile& other)
{
    for (const Section& section : other.m_sections)
    {
        for (const Entry& entry : section.entries)
            SetString(section.name, entry.key, entry.value);
    }
}

IniFile::Section* IniFile::FindSection(std::string_view name)
{
    for (Section& section : m_sections)
    {
        if (EqualsIgnoreCase(section.name, name))
            return &section;
    }
    return nullptr;
}

const IniFile::Section* IniFile::FindSection(std::string_view name) const
{
    for (const Section& section : m_sections)
    {
        if (EqualsIgnoreCase(section.name, name))
            return &section;
    }
    return nullptr;
}

IniFile::Section& IniFile::GetOrAddSection(std::string_view name)
{
    if (Section* existing = FindSection(name))
        return *existing;
    m_sections.push_back({ std::string(name), {} });
    return m_sections.back();
}

bool IniFile::Has(std::string_view section, std::string_view key) const
{
    return Find(section, key) != nullptr;
}

const std::string* IniFile::Find(std::string_view sectionName, std::string_view key) const
{
    const Section* section = FindSection(sectionName);
    if (!section)
        return nullptr;
    for (const Entry& entry : section->entries)
    {
        if (EqualsIgnoreCase(entry.key, key))
            return &entry.value;
    }
    return nullptr;
}

std::string IniFile::GetString(std::string_view section, std::string_view key, std::string_view fallback) const
{
    const std::string* value = Find(section, key);
    return value ? *value : std::string(fallback);
}

int IniFile::GetInt(std::string_view section, std::string_view key, int fallback) const
{
    const std::string* value = Find(section, key);
    if (!value || value->empty())
        return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    return (end && *end == '\0') ? static_cast<int>(parsed) : fallback;
}

float IniFile::GetFloat(std::string_view section, std::string_view key, float fallback) const
{
    const std::string* value = Find(section, key);
    if (!value || value->empty())
        return fallback;
    char* end = nullptr;
    const float parsed = std::strtof(value->c_str(), &end);
    return (end && *end == '\0') ? parsed : fallback;
}

bool IniFile::GetBool(std::string_view section, std::string_view key, bool fallback) const
{
    const std::string* value = Find(section, key);
    if (!value)
        return fallback;
    if (EqualsIgnoreCase(*value, "true") || EqualsIgnoreCase(*value, "1") || EqualsIgnoreCase(*value, "yes") || EqualsIgnoreCase(*value, "on"))
        return true;
    if (EqualsIgnoreCase(*value, "false") || EqualsIgnoreCase(*value, "0") || EqualsIgnoreCase(*value, "no") || EqualsIgnoreCase(*value, "off"))
        return false;
    return fallback;
}

void IniFile::SetString(std::string_view sectionName, std::string_view key, std::string_view value)
{
    Section& section = GetOrAddSection(sectionName);
    for (Entry& entry : section.entries)
    {
        if (EqualsIgnoreCase(entry.key, key))
        {
            entry.value.assign(value);
            return;
        }
    }
    section.entries.push_back({ std::string(key), std::string(value) });
}

void IniFile::SetInt(std::string_view section, std::string_view key, int value)
{
    SetString(section, key, std::to_string(value));
}

void IniFile::SetFloat(std::string_view section, std::string_view key, float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    SetString(section, key, buffer);
}

void IniFile::SetBool(std::string_view section, std::string_view key, bool value)
{
    SetString(section, key, value ? "True" : "False");
}

void IniFile::RemoveKey(std::string_view sectionName, std::string_view key)
{
    Section* section = FindSection(sectionName);
    if (!section)
        return;
    for (size_t i = 0; i < section->entries.size(); ++i)
    {
        if (EqualsIgnoreCase(section->entries[i].key, key))
        {
            section->entries.erase(section->entries.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}
