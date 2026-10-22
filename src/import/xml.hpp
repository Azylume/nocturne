#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace azl {

// An XML element with its attributes and child elements, as Resolume writes them: text content, comments and
// processing instructions are skipped.
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<XmlNode>> children;

    // Empty when the attribute is missing.
    std::string attr(const std::string& key) const;
    bool has(const std::string& key) const;
    // The first child element with this name (and, when given, this "name" attribute), or null.
    const XmlNode* child(const std::string& element, const std::string& nameAttr = {}) const;
    std::vector<const XmlNode*> all(const std::string& element) const;
    // Follows child() along a path of element names separated by '/'.
    const XmlNode* at(const std::string& path) const;
};

// Throws std::runtime_error on malformed input, and on elements nested more than 256 deep.
std::unique_ptr<XmlNode> parseXml(const std::string& text);
std::unique_ptr<XmlNode> parseXmlFile(const std::string& path);

} // namespace azl
