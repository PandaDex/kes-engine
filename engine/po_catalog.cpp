#include "po_catalog.h"
#include <algorithm>
#include <cctype>
#include <utility>
#include "profiler.h"

namespace
{
    constexpr int kMaxPluralDepth = 64;
    constexpr size_t kMaxPluralNodes = 256;   // bounds evaluation recursion

    std::string_view TrimView(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.remove_suffix(1);
        return text;
    }

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    // Recursive descent over the C subset gettext allows in Plural-Forms.
    class PluralParser
    {
    public:
        PluralParser(std::string_view text, std::vector<PoCatalog::PluralNode> &nodes) : m_text(text), m_nodes(nodes) {}

        bool Parse(std::string *outError)
        {
            m_nodes.clear();
            const int root = ParseTernary(0);
            SkipSpace();
            if (root < 0 || m_pos != m_text.size())
            {
                if (outError)
                    *outError = "invalid plural expression near '" + std::string(m_text.substr(std::min(m_pos, m_text.size()))) + "'";
                m_nodes.clear();
                return false;
            }
            return true;
        }

    private:
        using Op = PoCatalog::PluralNode::Op;

        void SkipSpace()
        {
            while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos])))
                ++m_pos;
        }

        bool Accept(std::string_view token)
        {
            SkipSpace();
            if (m_text.substr(m_pos, token.size()) != token)
                return false;
            m_pos += token.size();
            return true;
        }

        // '!' must not match the start of '!='
        bool AcceptSingle(char ch, char notFollowedBy)
        {
            SkipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ch)
                return false;
            if (m_pos + 1 < m_text.size() && m_text[m_pos + 1] == notFollowedBy)
                return false;
            ++m_pos;
            return true;
        }

        int Add(Op op, int a = -1, int b = -1, int c = -1, uint64_t value = 0)
        {
            if (a < 0 && (op != Op::N && op != Op::Constant))
                return -1;
            if ((op == Op::Ternary && (b < 0 || c < 0)) || (op > Op::Negate && op != Op::Ternary && b < 0))
                return -1;
            if (m_nodes.size() >= kMaxPluralNodes)
                return -1;
            PoCatalog::PluralNode node;
            node.op = op;
            node.a = a;
            node.b = b;
            node.c = c;
            node.value = value;
            m_nodes.push_back(node);
            return static_cast<int>(m_nodes.size()) - 1;
        }

        int ParseTernary(int depth)
        {
            if (depth > kMaxPluralDepth)
                return -1;
            const int condition = ParseBinary(0, depth);
            if (condition < 0 || !Accept("?"))
                return condition;
            const int whenTrue = ParseTernary(depth + 1);
            if (whenTrue < 0 || !Accept(":"))
                return -1;
            const int whenFalse = ParseTernary(depth + 1);
            return Add(Op::Ternary, condition, whenTrue, whenFalse);
        }

        // Precedence levels, lowest first.
        int ParseBinary(int level, int depth)
        {
            if (level == 6)
                return ParseUnary(depth);
            int left = ParseBinary(level + 1, depth);
            while (left >= 0)
            {
                Op op;
                if (!MatchOperator(level, op))
                    break;
                const int right = ParseBinary(level + 1, depth);
                left = Add(op, left, right);
            }
            return left;
        }

        bool MatchOperator(int level, Op &op)
        {
            switch (level)
            {
            case 0: if (Accept("||")) { op = Op::Or; return true; } return false;
            case 1: if (Accept("&&")) { op = Op::And; return true; } return false;
            case 2:
                if (Accept("==")) { op = Op::Equal; return true; }
                if (Accept("!=")) { op = Op::NotEqual; return true; }
                return false;
            case 3:
                if (Accept("<=")) { op = Op::LessEqual; return true; }
                if (Accept(">=")) { op = Op::GreaterEqual; return true; }
                if (Accept("<")) { op = Op::Less; return true; }
                if (Accept(">")) { op = Op::Greater; return true; }
                return false;
            case 4:
                if (Accept("+")) { op = Op::Add; return true; }
                if (Accept("-")) { op = Op::Subtract; return true; }
                return false;
            case 5:
                if (Accept("*")) { op = Op::Multiply; return true; }
                if (Accept("/")) { op = Op::Divide; return true; }
                if (Accept("%")) { op = Op::Modulo; return true; }
                return false;
            default:
                return false;
            }
        }

        int ParseUnary(int depth)
        {
            if (depth > kMaxPluralDepth)
                return -1;
            if (AcceptSingle('!', '='))
                return Add(Op::Not, ParseUnary(depth + 1));
            if (Accept("-"))
                return Add(Op::Negate, ParseUnary(depth + 1));
            if (Accept("("))
            {
                const int inner = ParseTernary(depth + 1);
                return inner >= 0 && Accept(")") ? inner : -1;
            }
            if (Accept("n"))
                return Add(Op::N);

            SkipSpace();
            if (m_pos >= m_text.size() || !std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
                return -1;
            uint64_t value = 0;
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos])))
                value = value * 10 + static_cast<uint64_t>(m_text[m_pos++] - '0');
            return Add(Op::Constant, -1, -1, -1, value);
        }

        std::string_view m_text;
        size_t m_pos = 0;
        std::vector<PoCatalog::PluralNode> &m_nodes;
    };

    uint64_t EvaluatePlural(const std::vector<PoCatalog::PluralNode> &nodes, int index, uint64_t n)
    {
        using Op = PoCatalog::PluralNode::Op;
        const PoCatalog::PluralNode &node = nodes[static_cast<size_t>(index)];
        auto a = [&] { return EvaluatePlural(nodes, node.a, n); };
        auto b = [&] { return EvaluatePlural(nodes, node.b, n); };
        switch (node.op)
        {
        case Op::N: return n;
        case Op::Constant: return node.value;
        case Op::Not: return a() == 0;
        case Op::Negate: return 0 - a();
        case Op::Multiply: return a() * b();
        case Op::Divide: { const uint64_t d = b(); return d ? a() / d : 0; }
        case Op::Modulo: { const uint64_t d = b(); return d ? a() % d : 0; }
        case Op::Add: return a() + b();
        case Op::Subtract: return a() - b();
        case Op::Less: return a() < b();
        case Op::Greater: return a() > b();
        case Op::LessEqual: return a() <= b();
        case Op::GreaterEqual: return a() >= b();
        case Op::Equal: return a() == b();
        case Op::NotEqual: return a() != b();
        case Op::And: return a() && b();
        case Op::Or: return a() || b();
        case Op::Ternary: return a() ? b() : EvaluatePlural(nodes, node.c, n);
        }
        return 0;
    }

    std::string_view HeaderField(std::string_view header, std::string_view key)
    {
        size_t start = 0;
        while (start < header.size())
        {
            size_t end = header.find('\n', start);
            if (end == std::string_view::npos)
                end = header.size();
            const std::string_view line = header.substr(start, end - start);
            const size_t colon = line.find(':');
            if (colon != std::string_view::npos && EqualsIgnoreCase(TrimView(line.substr(0, colon)), key))
                return TrimView(line.substr(colon + 1));
            start = end + 1;
        }
        return {};
    }

    bool ParseQuoted(std::string_view text, size_t &pos, std::string &out, std::string &error)
    {
        ++pos;
        while (pos < text.size())
        {
            const char ch = text[pos++];
            if (ch == '"')
                return true;
            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }
            if (pos >= text.size())
                break;
            const char escape = text[pos++];
            switch (escape)
            {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '\\': case '"': case '\'': case '?': out.push_back(escape); break;
            case 'x':
            {
                int value = 0, digits = 0;
                while (pos < text.size() && std::isxdigit(static_cast<unsigned char>(text[pos])) && digits < 2)
                {
                    const char hex = static_cast<char>(std::tolower(static_cast<unsigned char>(text[pos++])));
                    value = value * 16 + (hex <= '9' ? hex - '0' : hex - 'a' + 10);
                    ++digits;
                }
                if (digits == 0)
                {
                    error = "\\x without hex digits";
                    return false;
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (escape >= '0' && escape <= '7')
                {
                    int value = escape - '0';
                    for (int digits = 1; digits < 3 && pos < text.size() && text[pos] >= '0' && text[pos] <= '7'; ++digits)
                        value = value * 8 + (text[pos++] - '0');
                    out.push_back(static_cast<char>(value));
                    break;
                }
                error = std::string("unknown escape \\") + escape;
                return false;
            }
        }
        if (error.empty())
            error = "unterminated string";
        return false;
    }

    struct PoEntry
    {
        std::string context;
        std::string msgid;
        std::string msgidPlural;
        std::vector<std::string> msgstr;
        bool hasContext = false;
        bool hasMsgid = false;
        bool hasMsgstr = false;
        bool fuzzy = false;
        bool obsolete = false;
    };
}

void PoCatalog::Clear()
{
    m_entries.clear();
    m_plural.clear();
    m_pluralCount = 2;
    m_language.clear();
    m_skippedCount = 0;
}

bool PoCatalog::ParsePluralForms(std::string_view header, size_t &outCount, std::vector<PluralNode> &outNodes, std::string *outError)
{
    // nplurals=3; plural=(n==1 ? 0 : ...);
    const size_t countPos = header.find("nplurals");
    const size_t pluralPos = header.find("plural", countPos == std::string_view::npos ? 0 : countPos + 8);
    if (countPos == std::string_view::npos || pluralPos == std::string_view::npos)
    {
        if (outError)
            *outError = "Plural-Forms needs nplurals= and plural=";
        return false;
    }

    std::string_view countText = header.substr(countPos + 8);
    countText = TrimView(countText);
    if (countText.empty() || countText.front() != '=')
    {
        if (outError)
            *outError = "Plural-Forms: expected nplurals=";
        return false;
    }
    countText = TrimView(countText.substr(1));
    size_t count = 0;
    size_t digits = 0;
    while (digits < countText.size() && std::isdigit(static_cast<unsigned char>(countText[digits])))
        count = count * 10 + static_cast<size_t>(countText[digits++] - '0');
    if (digits == 0 || count == 0 || count > 32)
    {
        if (outError)
            *outError = "Plural-Forms: nplurals must be 1-32";
        return false;
    }

    std::string_view expression = TrimView(header.substr(pluralPos + 6));
    if (expression.empty() || expression.front() != '=')
    {
        if (outError)
            *outError = "Plural-Forms: expected plural=";
        return false;
    }
    expression = expression.substr(1);
    const size_t semicolon = expression.find(';');
    if (semicolon != std::string_view::npos)
        expression = expression.substr(0, semicolon);

    std::vector<PluralNode> nodes;
    if (!PluralParser(TrimView(expression), nodes).Parse(outError))
        return false;

    outCount = count;
    outNodes = std::move(nodes);
    return true;
}

bool PoCatalog::Parse(std::string_view text, std::string *outError)
{
    KES_ZONE_FN(IO);
    Clear();
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);

    std::string error;
    int lineNumber = 0;
    int entryLine = 0;
    PoEntry entry;
    std::string *current = nullptr;   // string continued by a "..." line
    bool haveHeader = false;

    auto fail = [&](const std::string &message) {
        if (outError)
            *outError = "line " + std::to_string(lineNumber) + ": " + message;
        Clear();
        return false;
    };

    auto finishEntry = [&]() -> bool {
        PoEntry done = std::move(entry);
        entry = PoEntry{};
        current = nullptr;
        if (!done.hasMsgid && !done.hasMsgstr && !done.hasContext)
            return true;
        if (!done.hasMsgid || !done.hasMsgstr)
        {
            lineNumber = entryLine;
            return fail(done.hasMsgid ? "msgid without msgstr" : "msgstr without msgid");
        }
        if (done.obsolete)
            return true;

        if (done.msgid.empty() && !done.hasContext)
        {
            if (haveHeader)
                return true;
            haveHeader = true;
            const std::string &header = done.msgstr.empty() ? std::string() : done.msgstr[0];
            m_language = std::string(HeaderField(header, "Language"));
            const std::string_view contentType = HeaderField(header, "Content-Type");
            const size_t charsetPos = contentType.find("charset=");
            if (charsetPos != std::string_view::npos)
            {
                const std::string_view charset = TrimView(contentType.substr(charsetPos + 8));
                if (!EqualsIgnoreCase(charset, "UTF-8") && !EqualsIgnoreCase(charset, "UTF8") && charset != "CHARSET")
                {
                    lineNumber = entryLine;
                    return fail("charset " + std::string(charset) + " is not supported; save the file as UTF-8");
                }
            }
            const std::string_view pluralForms = HeaderField(header, "Plural-Forms");
            if (!pluralForms.empty())
            {
                std::string pluralError;
                if (!ParsePluralForms(pluralForms, m_pluralCount, m_plural, &pluralError))
                {
                    lineNumber = entryLine;
                    return fail(pluralError);
                }
            }
            return true;
        }

        if (done.hasContext)
        {
            ++m_skippedCount;
            return true;
        }
        Entry &stored = m_entries[std::string(LocalizationStripKey(done.msgid))];
        stored.translations.clear();
        const bool translated = !done.fuzzy && !done.msgstr.empty() &&
                                std::all_of(done.msgstr.begin(), done.msgstr.end(), [](const std::string &s) { return !s.empty(); });
        if (translated)
            stored.translations = std::move(done.msgstr);
        return true;
    };

    size_t lineStart = 0;
    while (lineStart <= text.size())
    {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string_view::npos)
            lineEnd = text.size();
        std::string_view line = TrimView(text.substr(lineStart, lineEnd - lineStart));
        lineStart = lineEnd + 1;
        ++lineNumber;

        if (line.empty())
            continue;

        bool obsolete = false;
        if (line[0] == '#')
        {
            if (line.size() >= 2 && line[1] == ',')
            {
                if (entry.hasMsgstr && !finishEntry())
                    return false;
                if (line.find("fuzzy") != std::string_view::npos)
                    entry.fuzzy = true;
                continue;
            }
            if (line.size() >= 2 && line[1] == '~')
            {
                obsolete = true;
                line = TrimView(line.substr(2));
                if (line.empty() || line[0] == '|')
                    continue;
            }
            else
            {
                current = nullptr;
                continue;
            }
        }

        if (line[0] == '"')
        {
            if (!current)
                return fail("string without a keyword");
            size_t pos = 0;
            if (!ParseQuoted(line, pos, *current, error))
                return fail(error);
            if (!TrimView(line.substr(pos)).empty())
                return fail("unexpected text after string");
            continue;
        }

        size_t keywordEnd = 0;
        while (keywordEnd < line.size() && !std::isspace(static_cast<unsigned char>(line[keywordEnd])) && line[keywordEnd] != '"')
            ++keywordEnd;
        const std::string_view keyword = line.substr(0, keywordEnd);
        std::string_view rest = TrimView(line.substr(keywordEnd));
        if (rest.empty() || rest[0] != '"')
            return fail("expected a quoted string after " + std::string(keyword));

        std::string *target = nullptr;
        if (keyword == "msgctxt" || keyword == "msgid")
        {
            const bool startsNew = entry.hasMsgstr || (keyword == "msgid" && entry.hasMsgid) || (keyword == "msgctxt" && (entry.hasContext || entry.hasMsgid));
            if (startsNew)
            {
                const bool fuzzy = entry.hasMsgstr ? false : entry.fuzzy;
                if (!finishEntry())
                    return false;
                entry.fuzzy = fuzzy;
            }
            if (!entry.hasContext && !entry.hasMsgid)
                entryLine = lineNumber;
            entry.obsolete = obsolete;
            if (keyword == "msgctxt")
            {
                entry.hasContext = true;
                target = &entry.context;
            }
            else
            {
                entry.hasMsgid = true;
                target = &entry.msgid;
            }
        }
        else if (keyword == "msgid_plural")
        {
            if (!entry.hasMsgid || entry.hasMsgstr)
                return fail("msgid_plural must follow msgid");
            target = &entry.msgidPlural;
        }
        else if (keyword == "msgstr")
        {
            if (!entry.hasMsgid || entry.hasMsgstr)
                return fail("msgstr must follow msgid");
            entry.hasMsgstr = true;
            entry.msgstr.emplace_back();
            target = &entry.msgstr.back();
        }
        else if (keyword.substr(0, 7) == "msgstr[" && keyword.back() == ']')
        {
            const std::string_view indexText = keyword.substr(7, keyword.size() - 8);
            size_t index = 0;
            if (indexText.empty() || indexText.size() > 2 ||
                !std::all_of(indexText.begin(), indexText.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                return fail("bad plural index in " + std::string(keyword));
            for (char c : indexText)
                index = index * 10 + static_cast<size_t>(c - '0');
            if (!entry.hasMsgid)
                return fail(std::string(keyword) + " must follow msgid");
            if (index != entry.msgstr.size())
                return fail(std::string(keyword) + " is out of order");
            entry.hasMsgstr = true;
            entry.msgstr.emplace_back();
            target = &entry.msgstr.back();
        }
        else
        {
            return fail("unknown keyword " + std::string(keyword));
        }

        size_t pos = 0;
        if (!ParseQuoted(rest, pos, *target, error))
            return fail(error);
        if (!TrimView(rest.substr(pos)).empty())
            return fail("unexpected text after string");
        current = target;
    }
    return finishEntry();
}

const PoCatalog::Entry *PoCatalog::FindEntry(std::string_view key) const
{
    key = LocalizationStripKey(key);
    auto found = m_entries.find(key);
    return found != m_entries.end() ? &found->second : nullptr;
}

const std::string *PoCatalog::Find(std::string_view key) const
{
    const Entry *entry = FindEntry(key);
    return entry && !entry->translations.empty() ? &entry->translations.front() : nullptr;
}

const std::string *PoCatalog::FindPlural(std::string_view key, uint64_t n) const
{
    const Entry *entry = FindEntry(key);
    if (!entry || entry->translations.empty())
        return nullptr;
    const size_t index = PluralIndex(n);
    return index < entry->translations.size() ? &entry->translations[index] : nullptr;
}

size_t PoCatalog::PluralIndex(uint64_t n) const
{
    if (m_plural.empty())
        return n != 1 ? 1 : 0;
    return static_cast<size_t>(EvaluatePlural(m_plural, static_cast<int>(m_plural.size()) - 1, n));
}

std::string_view LocalizationStripKey(std::string_view key)
{
    key = TrimView(key);
    if (!key.empty() && key.front() == '#')
        key.remove_prefix(1);
    return key;
}

bool LocalizationIsKeyReference(std::string_view text)
{
    if (text.size() < 2 || text.front() != '#' || !std::isalpha(static_cast<unsigned char>(text[1])))
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-';
    });
}
