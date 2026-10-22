#include "import/xml.hpp"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace azl {

std::string XmlNode::attr(const std::string& key) const
{
    for (const auto& [k, v] : attributes)
        if (k == key)
            return v;
    return {};
}

bool XmlNode::has(const std::string& key) const
{
    for (const auto& [k, v] : attributes)
        if (k == key)
            return true;
    return false;
}

const XmlNode* XmlNode::child(const std::string& element, const std::string& nameAttr) const
{
    for (const auto& c : children)
        if (c->name == element && (nameAttr.empty() || c->attr("name") == nameAttr))
            return c.get();
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::all(const std::string& element) const
{
    std::vector<const XmlNode*> found;
    for (const auto& c : children)
        if (c->name == element)
            found.push_back(c.get());
    return found;
}

const XmlNode* XmlNode::at(const std::string& path) const
{
    const XmlNode* node = this;
    size_t start = 0;
    while (node && start <= path.size()) {
        const size_t end = path.find('/', start);
        node = node->child(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return node;
}

namespace {

constexpr int kMaxDepth = 256;

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    std::unique_ptr<XmlNode> document()
    {
        std::unique_ptr<XmlNode> root;
        while (skipMisc()) {
            if (root)
                fail("more than one root element");
            root = element(1);
        }
        if (!root)
            fail("no root element");
        return root;
    }

private:
    [[noreturn]] void fail(const std::string& what) const
    {
        throw std::runtime_error("XML: " + what + " at offset " + std::to_string(i_));
    }

    bool starts(const char* token) const { return s_.compare(i_, std::strlen(token), token) == 0; }

    void skipPast(const char* token)
    {
        const size_t end = s_.find(token, i_);
        if (end == std::string::npos)
            fail(std::string("missing ") + token);
        i_ = end + std::strlen(token);
    }

    void skipSpace()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }

    // Skips text, comments, declarations and processing instructions; true when an element starts.
    bool skipMisc()
    {
        while (i_ < s_.size()) {
            if (s_[i_] != '<') {
                ++i_;
            } else if (starts("<?")) {
                skipPast("?>");
            } else if (starts("<!--")) {
                skipPast("-->");
            } else if (starts("<![CDATA[")) {
                skipPast("]]>");
            } else if (starts("<!")) {
                skipPast(">");
            } else {
                return !starts("</");
            }
        }
        return false;
    }

    std::string name()
    {
        const size_t start = i_;
        while (i_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i_])) && s_[i_] != '>' && s_[i_] != '/' &&
               s_[i_] != '=')
            ++i_;
        if (i_ == start)
            fail("expected a name");
        return s_.substr(start, i_ - start);
    }

    std::string unescape(const std::string& raw) const
    {
        std::string out;
        for (size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] != '&') {
                out += raw[k];
                continue;
            }
            const size_t end = raw.find(';', k);
            if (end == std::string::npos)
                fail("unterminated entity");
            const std::string entity = raw.substr(k + 1, end - k - 1);
            if (entity == "amp") out += '&';
            else if (entity == "lt") out += '<';
            else if (entity == "gt") out += '>';
            else if (entity == "quot") out += '"';
            else if (entity == "apos") out += '\'';
            else if (!entity.empty() && entity[0] == '#') appendUtf8(out, entity);
            else fail("unknown entity &" + entity + ";");
            k = end;
        }
        return out;
    }

    void appendUtf8(std::string& out, const std::string& entity) const
    {
        const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
        const std::string digits = entity.substr(hex ? 2 : 1);
        const size_t valid = digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789");
        if (digits.empty() || valid != std::string::npos || digits.size() > 8)
            fail("bad character reference &" + entity + ";");
        const unsigned long c = std::stoul(digits, nullptr, hex ? 16 : 10);
        if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            fail("bad character reference &" + entity + ";");
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }

    std::unique_ptr<XmlNode> element(int depth)
    {
        if (depth > kMaxDepth)
            fail("elements nested deeper than " + std::to_string(kMaxDepth));
        ++i_;
        auto node = std::make_unique<XmlNode>();
        node->name = name();
        for (;;) {
            skipSpace();
            if (i_ >= s_.size())
                fail("unterminated element " + node->name);
            if (starts("/>")) {
                i_ += 2;
                return node;
            }
            if (s_[i_] == '>') {
                ++i_;
                break;
            }
            std::string key = name();
            skipSpace();
            if (i_ >= s_.size() || s_[i_] != '=')
                fail("expected = after " + key);
            ++i_;
            skipSpace();
            const char quote = i_ < s_.size() ? s_[i_] : '\0';
            if (quote != '"' && quote != '\'')
                fail("expected a quoted value for " + key);
            const size_t end = s_.find(quote, i_ + 1);
            if (end == std::string::npos)
                fail("unterminated value for " + key);
            node->attributes.emplace_back(std::move(key), unescape(s_.substr(i_ + 1, end - i_ - 1)));
            i_ = end + 1;
        }
        while (skipMisc())
            node->children.push_back(element(depth + 1));
        if (!starts("</"))
            fail("unterminated element " + node->name);
        i_ += 2;
        if (name() != node->name)
            fail("mismatched end of " + node->name);
        skipSpace();
        if (i_ >= s_.size() || s_[i_] != '>')
            fail("expected >");
        ++i_;
        return node;
    }

    const std::string& s_;
    size_t i_ = 0;
};

} // namespace

std::unique_ptr<XmlNode> parseXml(const std::string& text)
{
    return Parser(text).document();
}

std::unique_ptr<XmlNode> parseXmlFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path);
    std::stringstream text;
    text << in.rdbuf();
    return parseXml(text.str());
}

} // namespace azl
