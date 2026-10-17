/*
 * pagemade/plugin/xml.h — a small XML reader for the importers.
 *
 * Enough for SVG (and, later, the XML inside a DOCX package): elements,
 * attributes, text, comments, a doctype, processing instructions and the
 * five predefined entities plus numeric ones. Namespaces are resolved to
 * a URI and a local name. It is not a general XML stack and it does not
 * fetch anything.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pagemade {

struct XmlNode {
    bool text = false;                   // a character-data child, not an element
    std::string ns;                      // namespace URI; empty when none
    std::string local;                   // local name, or the character data
    struct Attr {
        std::string ns;
        std::string local;
        std::string value;
    };
    std::vector<Attr> attrs;
    std::vector<XmlNode> children;
};

/* The document element. False on a truncated or badly formed file. */
bool xml_parse(const uint8_t *data, size_t n, XmlNode *root, std::string *error);

const XmlNode::Attr *xml_attr(const XmlNode &node, const char *local);
std::string xml_attr_value(const XmlNode &node, const char *local);
/* The character data of this element, not of its descendants. */
std::string xml_text(const XmlNode &node);

} // namespace pagemade
