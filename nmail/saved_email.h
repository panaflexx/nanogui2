/*
 * nmail/saved_email.h — single-file dump of an email for sharing render bugs.
 *
 * Layout:
 *   <!-- nmail-saved
 *   From: ...
 *   To: ...
 *   Subject: ...
 *   Date: ...
 *   Content-Type: text/html   (or text/plain)
 *   -->
 *   <raw body>
 *
 * A file without the preamble is treated as raw HTML.
 */
#pragma once

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

struct SavedEmail {
    std::string from, to, subject, date;
    std::string html;   // set when Content-Type is text/html (or unknown)
    std::string body;   // plain text
};

inline std::string nmail_comment_safe(std::string s) {
    for (size_t i = 0; i + 1 < s.size(); ++i)
        if (s[i] == '-' && s[i + 1] == '-')
            s[i + 1] = '_';
    return s;
}

inline std::string nmail_serialize_email(const SavedEmail &e) {
    bool as_html = !e.html.empty();
    std::ostringstream out;
    out << "<!-- nmail-saved\n"
        << "From: " << nmail_comment_safe(e.from) << "\n"
        << "To: " << nmail_comment_safe(e.to) << "\n"
        << "Subject: " << nmail_comment_safe(e.subject) << "\n"
        << "Date: " << nmail_comment_safe(e.date) << "\n"
        << "Content-Type: " << (as_html ? "text/html" : "text/plain") << "\n"
        << "-->\n";
    out << (as_html ? e.html : e.body);
    return out.str();
}

inline bool nmail_parse_email(const std::string &raw, SavedEmail &e) {
    e = SavedEmail{};
    const char *preamble = "<!-- nmail-saved";
    if (raw.compare(0, 16, preamble) != 0) {
        e.html = raw;
        return true;
    }
    size_t end = raw.find("-->", 16);
    if (end == std::string::npos) {
        e.html = raw;
        return true;
    }
    std::string head = raw.substr(16, end - 16);
    std::string rest = raw.substr(end + 3);
    if (!rest.empty() && rest[0] == '\n')
        rest.erase(0, 1);

    bool html = true;
    std::istringstream hs(head);
    std::string line;
    auto starts = [](const std::string &ln, const char *key) {
        size_t k = std::strlen(key);
        return ln.size() >= k && ln.compare(0, k, key) == 0;
    };
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (starts(line, "From: ")) e.from = line.substr(6);
        else if (starts(line, "To: ")) e.to = line.substr(4);
        else if (starts(line, "Subject: ")) e.subject = line.substr(9);
        else if (starts(line, "Date: ")) e.date = line.substr(6);
        else if (starts(line, "Content-Type: ")) {
            std::string v = line.substr(14);
            for (char &c : v) c = (char)std::tolower((unsigned char)c);
            html = v.find("html") != std::string::npos;
        }
    }
    if (html) e.html = rest;
    else      e.body = rest;
    return true;
}

inline bool nmail_load_email_file(const std::string &path, SavedEmail &e,
                                  std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Could not open " + path;
        return false;
    }
    std::string raw((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return nmail_parse_email(raw, e);
}

inline std::string nmail_default_filename(const std::string &subject,
                                          bool html) {
    std::string s;
    for (unsigned char c : subject) {
        if (std::isalnum(c) || c == '-' || c == '_')
            s += (char)c;
        else if (std::isspace(c) || c == '.' || c == ',')
            s += '_';
    }
    while (!s.empty() && s.back() == '_')
        s.pop_back();
    if (s.empty())
        s = "email";
    if (s.size() > 80)
        s.resize(80);
    s += html ? ".html" : ".eml";
    return s;
}
