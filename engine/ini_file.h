#pragma once
#include <string>
#include <string_view>
#include <vector>

// Minimal INI document:
//
//   ; comment
//   [Section]
//   Key=Value
//
// Section and key names are case-insensitive. Order of sections and keys is
// preserved, so files written back keep their layout (comments are dropped).
class IniFile
{
public:
    bool LoadFromFile(const std::string& path);
    void LoadFromString(std::string_view text);
    bool SaveToFile(const std::string& path) const;
    std::string ToString() const;

    // Overlay every key of `other` on top of this document.
    void Merge(const IniFile& other);

    bool Has(std::string_view section, std::string_view key) const;
    const std::string* Find(std::string_view section, std::string_view key) const;

    std::string GetString(std::string_view section, std::string_view key, std::string_view fallback = {}) const;
    int GetInt(std::string_view section, std::string_view key, int fallback) const;
    float GetFloat(std::string_view section, std::string_view key, float fallback) const;
    bool GetBool(std::string_view section, std::string_view key, bool fallback) const;

    void SetString(std::string_view section, std::string_view key, std::string_view value);
    void SetInt(std::string_view section, std::string_view key, int value);
    void SetFloat(std::string_view section, std::string_view key, float value);
    void SetBool(std::string_view section, std::string_view key, bool value);

    void RemoveKey(std::string_view section, std::string_view key);
    void Clear() { m_sections.clear(); }

private:
    struct Entry
    {
        std::string key;
        std::string value;
    };
    struct Section
    {
        std::string name;
        std::vector<Entry> entries;
    };

    Section* FindSection(std::string_view name);
    const Section* FindSection(std::string_view name) const;
    Section& GetOrAddSection(std::string_view name);

    std::vector<Section> m_sections;
};
