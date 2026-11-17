#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// msgid is the key, msgstr the text. No engine dependencies: the localization
// editor compiles this file to validate what it saves.
class PoCatalog
{
public:
    struct Entry
    {
        std::vector<std::string> translations;  // empty when untranslated or fuzzy
    };

    // Obsolete (#~) entries and entries with a msgctxt are skipped.
    bool Parse(std::string_view text, std::string *outError = nullptr);
    void Clear();

    const Entry *FindEntry(std::string_view key) const;
    const std::string *Find(std::string_view key) const;
    const std::string *FindPlural(std::string_view key, uint64_t n) const;

    size_t PluralIndex(uint64_t n) const;
    size_t GetPluralCount() const { return m_pluralCount; }
    size_t GetEntryCount() const { return m_entries.size(); }
    size_t GetSkippedCount() const { return m_skippedCount; }
    const std::string &GetLanguage() const { return m_language; }

    // Plural-Forms compiled to a flat tree; the root is the last node.
    struct PluralNode
    {
        enum class Op : uint8_t
        {
            N, Constant, Not, Negate,
            Multiply, Divide, Modulo, Add, Subtract,
            Less, Greater, LessEqual, GreaterEqual, Equal, NotEqual,
            And, Or, Ternary,
        };
        Op op = Op::Constant;
        uint64_t value = 0;
        int32_t a = -1, b = -1, c = -1;
    };
    static bool ParsePluralForms(std::string_view header, size_t &outCount, std::vector<PluralNode> &outNodes, std::string *outError = nullptr);

private:
    struct StringHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
    };

    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> m_entries;
    std::vector<PluralNode> m_plural;
    size_t m_pluralCount = 2;
    size_t m_skippedCount = 0;
    std::string m_language;
};

std::string_view LocalizationStripKey(std::string_view key);
// '#', a letter, then letters, digits, '.', '_' or '-'
bool LocalizationIsKeyReference(std::string_view text);
