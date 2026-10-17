/*
 * pagemade/plugin/xml.cpp — see xml.h.
 */
#include "plugin/xml.h"

#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace pagemade {

namespace {

struct Reader {
    const char *p;
    const char *end;
    std::string error;

    bool fail(const char *why) {
        if (error.empty())
            error = why;
        return false;
    }
    bool eof() const { return p >= end; }
    void skip_ws() {
        while (p < end && std::isspace((unsigned char) *p))
            ++p;
    }
    bool starts(const char *s) const {
        const char *q = p;
        for (; *s; ++s, ++q)
            if (q >= end || *q != *s)
                return false;
        return true;
    }
};

struct Ns {
    std::string def;
    std::map<std::string, std::string> prefix;
};

bool decode_entity(Reader &r, std::string &out) {
    if (r.eof() || *r.p != '&')
        return false;
    const char *s = r.p + 1;
    if (s >= r.end)
        return r.fail("truncated entity");
    if (*s == '#') {
        ++s;
        int base = 10;
        if (s < r.end && (*s == 'x' || *s == 'X')) {
            base = 16;
            ++s;
        }
        unsigned long cp = 0;
        const char *d = s;
        while (s < r.end && *s != ';') {
            int v;
            unsigned char c = (unsigned char) *s;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else
                break;
            cp = cp * (unsigned) base + (unsigned) v;
            ++s;
        }
        if (s == d || s >= r.end || *s != ';' || cp == 0 || cp > 0x10FFFF)
            return r.fail("bad numeric entity");
        ++s;
        /* UTF-8 */
        if (cp < 0x80)
            out.push_back((char) cp);
        else if (cp < 0x800) {
            out.push_back((char) (0xC0 | (cp >> 6)));
            out.push_back((char) (0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char) (0xE0 | (cp >> 12)));
            out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char) (0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char) (0xF0 | (cp >> 18)));
            out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char) (0x80 | (cp & 0x3F)));
        }
        r.p = s;
        return true;
    }
    const char *e = s;
    while (e < r.end && *e != ';' && !std::isspace((unsigned char) *e))
        ++e;
    if (e >= r.end || *e != ';')
        return r.fail("truncated entity");
    const std::string name(s, e);
    const char *rep = nullptr;
    if (name == "lt")
        rep = "<";
    else if (name == "gt")
        rep = ">";
    else if (name == "amp")
        rep = "&";
    else if (name == "quot")
        rep = "\"";
    else if (name == "apos")
        rep = "'";
    if (!rep)
        return r.fail("unknown entity");
    out += rep;
    r.p = e + 1;
    return true;
}

bool parse_chars(Reader &r, std::string &out, char stop) {
    while (r.p < r.end && *r.p != stop && *r.p != '<') {
        if (*r.p == '&') {
            if (!decode_entity(r, out))
                return false;
        } else {
            out.push_back(*r.p);
            ++r.p;
        }
    }
    return true;
}

bool parse_name(Reader &r, std::string &prefix, std::string &local) {
    if (r.eof())
        return r.fail("expected a name");
    const char *s = r.p;
    auto ok = [](unsigned char c, bool first) {
        return std::isalpha(c) || c == '_' || (!first && (std::isdigit(c) || c == '-' || c == '.'));
    };
    if (!ok((unsigned char) *r.p, true))
        return r.fail("expected a name");
    ++r.p;
    while (r.p < r.end && (ok((unsigned char) *r.p, false) || *r.p == ':'))
        ++r.p;
    std::string raw(s, r.p);
    const size_t colon = raw.find(':');
    if (colon == std::string::npos) {
        prefix.clear();
        local = raw;
    } else {
        prefix = raw.substr(0, colon);
        local = raw.substr(colon + 1);
    }
    return !local.empty() || r.fail("expected a name");
}

bool skip_comment(Reader &r) {
    r.p += 4;                            // <!--
    while (r.p + 2 < r.end && !(r.p[0] == '-' && r.p[1] == '-' && r.p[2] == '>'))
        ++r.p;
    if (r.p + 2 >= r.end)
        return r.fail("truncated comment");
    r.p += 3;
    return true;
}

bool skip_pi(Reader &r) {
    r.p += 2;
    while (r.p + 1 < r.end && !(r.p[0] == '?' && r.p[1] == '>'))
        ++r.p;
    if (r.p + 1 >= r.end)
        return r.fail("truncated processing instruction");
    r.p += 2;
    return true;
}

bool skip_doctype(Reader &r) {
    r.p += 2;
    int depth = 0;
    for (; r.p < r.end; ++r.p) {
        if (*r.p == '[')
            ++depth;
        else if (*r.p == ']' && depth)
            --depth;
        else if (*r.p == '>' && depth == 0) {
            ++r.p;
            return true;
        }
    }
    return r.fail("truncated doctype");
}

bool skip_misc(Reader &r) {
    for (;;) {
        r.skip_ws();
        if (r.eof() || *r.p != '<')
            return true;
        if (r.starts("<!--")) {
            if (!skip_comment(r))
                return false;
        } else if (r.starts("<?")) {
            if (!skip_pi(r))
                return false;
        } else if (r.starts("<!")) {
            if (!skip_doctype(r))
                return false;
        } else
            return true;
    }
}

bool parse_element(Reader &r, XmlNode &node, const Ns &parent);

bool parse_content(Reader &r, XmlNode &node, const Ns &ns) {
    while (r.p < r.end) {
        if (*r.p == '<') {
            if (r.starts("</"))
                return true;
            if (r.starts("<!--")) {
                if (!skip_comment(r))
                    return false;
                continue;
            }
            if (r.starts("<?")) {
                if (!skip_pi(r))
                    return false;
                continue;
            }
            if (r.starts("<![CDATA[")) {
                r.p += 9;
                XmlNode text;
                text.text = true;
                const char *s = r.p;
                while (r.p + 2 < r.end && !(r.p[0] == ']' && r.p[1] == ']' && r.p[2] == '>'))
                    ++r.p;
                if (r.p + 2 >= r.end)
                    return r.fail("truncated CDATA");
                text.local.assign(s, r.p);
                r.p += 3;
                node.children.push_back(std::move(text));
                continue;
            }
            if (r.starts("<!"))
                return r.fail("unexpected declaration");
            XmlNode child;
            if (!parse_element(r, child, ns))
                return false;
            node.children.push_back(std::move(child));
            continue;
        }
        XmlNode text;
        text.text = true;
        if (!parse_chars(r, text.local, '<'))
            return false;
        if (!text.local.empty())
            node.children.push_back(std::move(text));
    }
    return r.fail("truncated element");
}

bool parse_element(Reader &r, XmlNode &node, const Ns &parent) {
    if (!r.starts("<"))
        return r.fail("expected an element");
    ++r.p;
    std::string prefix, local;
    if (!parse_name(r, prefix, local))
        return false;
    Ns ns = parent;
    struct RawAttr {
        std::string prefix, local, value;
    };
    std::vector<RawAttr> raw;
    bool empty = false;
    for (;;) {
        r.skip_ws();
        if (r.eof())
            return r.fail("truncated element");
        if (*r.p == '>') {
            ++r.p;
            break;
        }
        if (*r.p == '/' && r.p + 1 < r.end && r.p[1] == '>') {
            r.p += 2;
            empty = true;
            break;
        }
        RawAttr a;
        if (!parse_name(r, a.prefix, a.local))
            return false;
        r.skip_ws();
        if (r.eof() || *r.p != '=')
            return r.fail("expected '='");
        ++r.p;
        r.skip_ws();
        if (r.eof() || (*r.p != '"' && *r.p != '\''))
            return r.fail("expected a quoted attribute");
        const char quote = *r.p++;
        if (!parse_chars(r, a.value, quote))
            return false;
        if (r.eof() || *r.p != quote)
            return r.fail("truncated attribute");
        ++r.p;
        raw.push_back(std::move(a));
    }
    for (const RawAttr &a : raw) {
        if (a.prefix.empty() && a.local == "xmlns")
            ns.def = a.value;
        else if (a.prefix == "xmlns")
            ns.prefix[a.local] = a.value;
    }
    auto resolve = [&](const std::string &pre) -> std::string {
        if (pre.empty())
            return ns.def;
        auto it = ns.prefix.find(pre);
        if (it != ns.prefix.end())
            return it->second;
        auto it2 = parent.prefix.find(pre);
        if (it2 != parent.prefix.end())
            return it2->second;
        return {};
    };
    node.local = local;
    node.ns = resolve(prefix);
    for (const RawAttr &a : raw) {
        if ((a.prefix.empty() && a.local == "xmlns") || a.prefix == "xmlns")
            continue;
        XmlNode::Attr attr;
        /* Unprefixed attributes are not in the default namespace. */
        attr.ns = a.prefix.empty() ? std::string() : resolve(a.prefix);
        attr.local = a.local;
        attr.value = a.value;
        node.attrs.push_back(std::move(attr));
    }
    if (empty)
        return true;
    if (!parse_content(r, node, ns))
        return false;
    r.skip_ws();
    if (!r.starts("</"))
        return r.fail("expected an end tag");
    r.p += 2;
    std::string ep, el;
    if (!parse_name(r, ep, el))
        return false;
    if (el != local || ep != prefix)
        return r.fail("end tag does not match");
    r.skip_ws();
    if (r.eof() || *r.p != '>')
        return r.fail("truncated end tag");
    ++r.p;
    return true;
}

} // namespace

bool xml_parse(const uint8_t *data, size_t n, XmlNode *root, std::string *error) {
    if (!root)
        return false;
    *root = XmlNode();
    Reader r{data ? (const char *) data : "", data ? (const char *) data + n : "", {}};
    if (n >= 3 && (unsigned char) r.p[0] == 0xEF && (unsigned char) r.p[1] == 0xBB &&
        (unsigned char) r.p[2] == 0xBF)
        r.p += 3;
    if (!skip_misc(r)) {
        if (error)
            *error = r.error;
        return false;
    }
    if (r.eof() || *r.p != '<') {
        if (error)
            *error = "no document element";
        return false;
    }
    Ns ns;
    if (!parse_element(r, *root, ns)) {
        if (error)
            *error = r.error.empty() ? "couldn't read the XML" : r.error;
        return false;
    }
    return true;
}

const XmlNode::Attr *xml_attr(const XmlNode &node, const char *local) {
    for (const XmlNode::Attr &a : node.attrs)
        if (a.local == local)
            return &a;
    return nullptr;
}

std::string xml_attr_value(const XmlNode &node, const char *local) {
    const XmlNode::Attr *a = xml_attr(node, local);
    return a ? a->value : std::string();
}

std::string xml_text(const XmlNode &node) {
    std::string s;
    for (const XmlNode &c : node.children)
        if (c.text)
            s += c.local;
    return s;
}

} // namespace pagemade
