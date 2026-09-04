/*
 * nmail/mail_format.cpp — implementation of mail_format.h.
 */

#include "mail_format.h"

#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

using namespace nanogui;

// ---------------------------------------------------------------------------
// parse_markdown — minimal Markdown → Document converter
// Supports: # / ## / ### headers, **bold**, *italic*, `code`, ```fenced```,
// "> " blockquotes, "- " / "* " bullet lists, \escapes.
// Blank lines start a new paragraph.
// ---------------------------------------------------------------------------
void parse_markdown(Document &doc, const std::string &md,
                    NVGcolor text_color,
                    float base_size)
{
    doc.paragraphs.clear();

    Style normal;  normal.fontSize = base_size;          normal.fgColor = text_color;
    Style code_s   = normal; code_s.monospace = true;
                             code_s.fontSize  = base_size * 0.875f;
                             code_s.bgColor   = nvgRGBA(220, 220, 228, 255);
    Style h1 = normal; h1.fontSize = base_size * 1.625f; h1.bold = true;
    Style h2 = normal; h2.fontSize = base_size * 1.25f;  h2.bold = true;
    Style h3 = normal; h3.fontSize = base_size * 1.0625f; h3.bold = true;

    // Inline-span parser: **bold**, *italic*, `code`, \escapes, plain text.
    // `base` is the paragraph's base style (blockquote paragraphs pass an
    // italic variant).
    auto append_inline = [&](Paragraph *p, const std::string &text,
                             const Style &base) {
        Style b = base; b.bold      = true;
        Style i = base; i.italic    = true;
        Style c = base; c.monospace = true;
                        c.fontSize  = base.fontSize * 0.875f;
                        c.bgColor   = code_s.bgColor;
        size_t i_ = 0;
        while (i_ < text.size()) {
            if (text[i_] == '\\' && i_ + 1 < text.size() &&
                (text[i_+1] == '*' || text[i_+1] == '`' ||
                 text[i_+1] == '\\' || text[i_+1] == '#' ||
                 text[i_+1] == '-' || text[i_+1] == '>')) {
                p->addText(std::string(1, text[i_+1]), base);
                i_ += 2; continue;
            }
            if (text.compare(i_, 3, "<u>") == 0) {
                size_t s = i_ + 3, e = text.find("</u>", s);
                if (e != std::string::npos) {
                    Style u = base; u.underline = true;
                    if (e > s) p->addText(text.substr(s, e - s), u);
                    i_ = e + 4; continue;
                }
            }
            if (i_ + 2 < text.size() && text[i_] == '*' &&
                text[i_+1] == '*' && text[i_+2] == '*') {
                size_t s = i_ + 3, e = text.find("***", s);
                if (e != std::string::npos) {
                    Style bi = base; bi.bold = true; bi.italic = true;
                    if (e > s) p->addText(text.substr(s, e - s), bi);
                    i_ = e + 3; continue;
                }
            }
            if (i_ + 1 < text.size() && text[i_] == '*' && text[i_+1] == '*') {
                size_t s = i_ + 2, e = text.find("**", s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), b);
                    i_ = e + 2; continue;
                }
            } else if (text[i_] == '*' && (i_ == 0 || text[i_-1] != '*')) {
                size_t s = i_ + 1, e = text.find('*', s);
                if (e != std::string::npos && e > s) {
                    p->addText(text.substr(s, e - s), i);
                    i_ = e + 1; continue;
                }
            } else if (text[i_] == '`') {
                size_t s = i_ + 1, e = text.find('`', s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), c);
                    i_ = e + 1; continue;
                }
            }
            size_t s = i_;
            while (i_ < text.size() && text[i_] != '*' && text[i_] != '`'
                   && text[i_] != '\\') ++i_;
            if (i_ > s) p->addText(text.substr(s, i_ - s), base);
            else        ++i_;
        }
    };

    std::istringstream iss(md);
    std::string line;
    Paragraph *cur = nullptr;
    bool inCode = false;
    std::string codeBuf;

    while (std::getline(iss, line)) {
        bool inline_break = (!line.empty() && line.back() == '\\')
                         || (line.size() >= 2
                             && line[line.size()-1] == ' '
                             && line[line.size()-2] == ' ');

        // strip trailing whitespace (and the backslash if present)
        while (!line.empty() && (std::isspace((unsigned char)line.back())
                                 || line.back() == '\\'))
            line.pop_back();

        if (line.empty()) {
            if (inCode) codeBuf += '\n';
            else        cur = nullptr;
            continue;
        }

        // fenced code block
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (inCode) {
                if (!codeBuf.empty()) doc.addParagraph()->addText(codeBuf, code_s);
                codeBuf.clear(); inCode = false;
            } else {
                inCode = true; codeBuf.clear();
            }
            cur = nullptr; continue;
        }
        if (inCode) { codeBuf += line + "\n"; continue; }

        // horizontal rule  --- / *** / ___  (3+ repeated chars, nothing else)
        if (!inCode && line.size() >= 3) {
            char c = line[0];
            if (c == '-' || c == '*' || c == '_') {
                bool all_same = true;
                for (char ch : line) if (ch != c) { all_same = false; break; }
                if (all_same) {
                    auto *p = doc.addParagraph();
                    p->isRule = true;
                    cur = nullptr;
                    continue;
                }
            }
        }

        // blockquote: "> text" — consecutive quote lines join into one
        // indented, italic paragraph (soft-wrapped like normal text).
        if (line[0] == '>' && (line.size() == 1 || line[1] == ' ')) {
            Style qs = normal; qs.italic = true;
            std::string content = line.size() > 2 ? line.substr(2) : "";
            if (!cur || cur->leftIndent <= 0.0f) {
                cur = doc.addParagraph();
                cur->leftIndent = 16.0f;
            } else {
                cur->addText(" ", qs);
            }
            append_inline(cur, content, qs);
            continue;
        }

        // headings
        if (line[0] == '#') {
            size_t lvl = 0;
            while (lvl < line.size() && line[lvl] == '#') ++lvl;
            if (lvl < line.size() && std::isspace((unsigned char)line[lvl])) {
                const Style &hs = (lvl == 1) ? h1 : (lvl == 2) ? h2 : h3;
                doc.addParagraph()->addText(line.substr(lvl + 1), hs);
                cur = nullptr; continue;
            }
        }

        // unordered list item: "- text" / "* text", optionally indented by
        // spaces for sub-levels (2 spaces per level).  One paragraph per
        // item, indented with a drawn bullet marker.
        {
            size_t sp = 0;
            while (sp < line.size() && line[sp] == ' ') ++sp;
            if (line.size() >= sp + 2 &&
                (line[sp] == '-' || line[sp] == '*') && line[sp + 1] == ' ') {
                Paragraph *p = doc.addParagraph();
                p->isBullet   = true;
                p->leftIndent = 16.0f * (float)(sp / 2 + 1);
                append_inline(p, line.substr(sp + 2), normal);
                cur = nullptr; continue;
            }
        }

        if (!cur || cur->leftIndent > 0.0f) cur = doc.addParagraph();
        else      cur->addText(" ", normal);  // soft-wrap join
        append_inline(cur, line, normal);
        if (inline_break)
            cur->addText("\n", normal);  // tight in-paragraph line break
    }

    if (inCode && !codeBuf.empty())
        doc.addParagraph()->addText(codeBuf, code_s);
    if (doc.paragraphs.empty())
        doc.addParagraph();
}

// ---------------------------------------------------------------------------
// document_to_markdown — serialize a Document back to Markdown.  Inverse of
// parse_markdown; used at send time for markup=markdown messages.
// ---------------------------------------------------------------------------
static std::string md_escape_plain(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '*' || c == '`') out += '\\';
        out += c;
    }
    return out;
}

std::string document_to_markdown(const Document &doc) {
    auto same_style = [](const Style &a, const Style &b) {
        return a.fontSize == b.fontSize &&
               a.bold == b.bold && a.italic == b.italic &&
               a.underline == b.underline && a.monospace == b.monospace &&
               std::memcmp(&a.fgColor, &b.fgColor, sizeof(NVGcolor)) == 0 &&
               std::memcmp(&a.bgColor, &b.bgColor, sizeof(NVGcolor)) == 0;
    };
    std::string out;
    bool in_code = false;   // inside a fenced ``` block
    for (const auto &para : doc.paragraphs) {
        /* Fenced code block: a paragraph whose runs are all monospace.
         * Consecutive such paragraphs join into one block; a blank code
         * line (empty monospace run) becomes a blank line inside the
         * fence instead of splitting it. */
        bool is_code = !para->isRule && !para->runs.empty();
        for (const Text &r : para->runs)
            if (!r.style.monospace) { is_code = false; break; }
        if (is_code) {
            if (!in_code) { out += "```\n"; in_code = true; }
            std::string t = para->plain_text();
            while (!t.empty() && t.back() == '\n') t.pop_back();
            out += t;
            out += '\n';
            continue;
        }
        if (in_code) { out += "```\n"; in_code = false; }

        std::string line;
        int level = 0;
        if (para->isRule) {
            line = "---";
        } else {
            /* Heading detection mirrors parse_markdown's size ratios at
             * base 16pt (h1 26, h2 20, h3 17). */
            if (!para->runs.empty() && para->runs[0].style.bold) {
                float fs = para->runs[0].style.fontSize;
                if (fs >= 24.0f)       level = 1;
                else if (fs >= 18.5f)  level = 2;
                else if (fs >= 16.75f) level = 3;
            }
            if (level) line = std::string((size_t)level, '#') + " ";

            /* Merge adjacent same-style runs so emphasis markers wrap
             * the longest possible span ("*a b*" not "*a** **b*"). */
            std::vector<Text> merged;
            for (const Text &r : para->runs) {
                if (r.content.empty()) continue;
                if (!merged.empty() &&
                    same_style(merged.back().style, r.style))
                    merged.back().content += r.content;
                else
                    merged.push_back(r);
            }

            for (const Text &r : merged) {
                const std::string &c = r.content;
                if (c.empty()) continue;
                /* Multiline monospace run -> fenced code block. */
                if (r.style.monospace && c.find('\n') != std::string::npos) {
                    line += "```\n" + c + "```";
                    continue;
                }
                bool b = r.style.bold && !level;
                bool i = r.style.italic;
                bool u = r.style.underline;
                bool m = r.style.monospace;
                std::string open, close;
                if (m) { open += '`';    close = "`"    + close; }
                if (b) { open += "**";   close = "**"   + close; }
                if (i) { open += '*';    close = "*"    + close; }
                if (u) { open += "<u>";  close = "</u>" + close; }
                line += open + (m ? c : md_escape_plain(c)) + close;
            }
        }

        /* Escape leading markers that would otherwise be re-parsed as
         * structure (real headings/rules/quotes already handled above). */
        if (!level && !para->isRule && para->leftIndent <= 0.0f &&
            !line.empty() &&
            (line[0] == '#' || line[0] == '>' ||
             (line.size() > 1 && line[0] == '-' && line[1] == ' ')))
            line = '\\' + line;

        /* Blockquote: prefix every line with "> ".  (Bullet items carry
         * leftIndent too, but serialize with "- " instead.) */
        if (para->leftIndent > 0.0f && !para->isBullet) {
            std::string quoted;
            size_t pos = 0;
            for (;;) {
                size_t nl = line.find('\n', pos);
                quoted += "> ";
                if (nl == std::string::npos) {
                    quoted += line.substr(pos);
                    break;
                }
                quoted += line.substr(pos, nl - pos);
                quoted += '\n';
                pos = nl + 1;
            }
            line = quoted;
        }

        /* Bullet list item: "- ", indented 2 spaces per sub-level
         * (level is derived from leftIndent: 16px per level). */
        if (para->isBullet && !para->isRule) {
            int lvl = (int)(para->leftIndent / 16.0f + 0.5f) - 1;
            if (lvl < 0) lvl = 0;
            line = std::string((size_t)lvl * 2, ' ') + "- " + line;
        }

        out += line;
        out += '\n';
    }
    if (in_code) out += "```\n";
    return out;
}

// ---------------------------------------------------------------------------
// document_to_html — serialize a Document to an HTML email body.  Mirrors
// document_to_markdown's structure detection (headings by font size,
// all-monospace paragraphs -> <pre>, isBullet -> <ul>/<li> with nesting by
// indent level, leftIndent -> <blockquote>, isRule -> <hr>).
// ---------------------------------------------------------------------------
std::string html_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
    return out;
}

/* Inline runs -> HTML spans.  In-paragraph newlines become <br>.
 * `in_heading` suppresses <strong> (headings are already bold). */
static std::string html_inline(const Paragraph &para, bool in_heading = false) {
    std::string out;
    for (const Text &r : para.runs) {
        if (r.content.empty()) continue;
        std::string c = html_escape(r.content);
        size_t pos = 0;
        while ((pos = c.find('\n', pos)) != std::string::npos) {
            c.replace(pos, 1, "<br>");
            pos += 4;
        }
        std::string open, close;
        if (r.style.monospace) { open += "<code>";    close = "</code>"    + close; }
        if (r.style.bold && !in_heading) { open += "<strong>";  close = "</strong>"  + close; }
        if (r.style.italic)    { open += "<em>";      close = "</em>"      + close; }
        if (r.style.underline) { open += "<u>";       close = "</u>"       + close; }
        out += open + c + close;
    }
    return out;
}

static std::string paragraphs_to_html(const Document &doc) {
    std::string out;
    int  list_depth = 0;   // number of open <ul> elements
    bool in_pre     = false;

    auto close_lists = [&]() {
        while (list_depth > 0) { out += "</ul>\n"; --list_depth; }
    };
    auto close_pre = [&]() {
        if (in_pre) { out += "</code></pre>\n"; in_pre = false; }
    };

    for (const auto &para : doc.paragraphs) {
        /* Code block: all-monospace paragraph (blank code lines included).
         * Consecutive ones share a single <pre>. */
        bool is_code = !para->isRule && !para->isBullet &&
                       !para->runs.empty();
        for (const Text &r : para->runs)
            if (!r.style.monospace) { is_code = false; break; }
        if (is_code) {
            close_lists();
            if (!in_pre) { out += "<pre><code>"; in_pre = true; }
            else         out += '\n';
            std::string t = para->plain_text();
            while (!t.empty() && t.back() == '\n') t.pop_back();
            out += html_escape(t);
            continue;
        }
        close_pre();

        /* Bullet list item, nesting by indent level (16px per level). */
        if (para->isBullet) {
            int lvl = (int)(para->leftIndent / 16.0f + 0.5f) - 1;
            if (lvl < 0) lvl = 0;
            int target = lvl + 1;
            while (list_depth < target) { out += "<ul>\n";  ++list_depth; }
            while (list_depth > target) { out += "</ul>\n"; --list_depth; }
            out += "<li>" + html_inline(*para) + "</li>\n";
            continue;
        }
        close_lists();

        if (para->isRule) { out += "<hr>\n"; continue; }

        /* Heading detection: same size ratios as document_to_markdown. */
        int level = 0;
        if (!para->runs.empty() && para->runs[0].style.bold) {
            float fs = para->runs[0].style.fontSize;
            if (fs >= 24.0f)       level = 1;
            else if (fs >= 18.5f)  level = 2;
            else if (fs >= 16.75f) level = 3;
        }

        std::string content = html_inline(*para, level > 0);
        if (content.empty()) continue;   // blank paragraphs add nothing

        if (level) {
            out += "<h" + std::to_string(level) + ">" + content +
                   "</h" + std::to_string(level) + ">\n";
        } else if (para->leftIndent > 0.0f) {
            out += "<blockquote><p>" + content + "</p></blockquote>\n";
        } else {
            out += "<p>" + content + "</p>\n";
        }
    }
    close_pre();
    close_lists();
    return out;
}

std::string document_to_html(const Document &doc) {
    return "<!DOCTYPE html>\n<html><body>\n" + paragraphs_to_html(doc) +
           "</body></html>\n";
}


/* ---------------------------------------------------------------------------
 * Message header card.
 *
 * The header is chrome, not content, but it shares a document with the mail's
 * own HTML — which routinely paints its own background.  Theme-coloured text
 * then lands on whatever the sender chose (light ink on white in dark mode),
 * so the card carries its own parchment palette and stays legible in both
 * modes and against any message background.  Fixed colours on purpose: these
 * do NOT follow the light/dark theme.
 * ------------------------------------------------------------------------ */
namespace parchment {
    static const char *kPaper   = "#f4ecd8";  // aged paper
    static const char *kEdge    = "#ddd0b0";  // slightly darker rule/border
    static const char *kInk     = "#2b2418";  // primary text
    static const char *kInkBold = "#1f1a12";  // subject
    /* Label contrast on kPaper is 5.09:1 — WCAG AA for body text.  The
     * lighter brown this replaced measured 3.76:1 and failed. */
    static const char *kLabel   = "#756040";  // "From:" / "To:" / "Date:"
    static const char *kMeta    = "#6b5d45";  // date value
}

/* Private scheme for the header's name links.  is_allowed_url() only ever
 * lets http/https/mailto reach the browser, so an unhandled click here is
 * inert rather than dangerous. */
const char *kAddrScheme = "x-nmail-addr:";

/* Render an address header.  Entries that carry a display name become links
 * that swap to the bare address when clicked; entries that are already just
 * an address have nothing to reveal and stay plain text. */
static std::string address_row_html(const std::string &raw,
                                    const std::set<std::string> &expanded) {
    using namespace parchment;
    std::vector<MailAddress> addrs = parse_address_list(raw);
    if (addrs.empty())                       // unparseable: show it verbatim
        return "<span style=\"color:" + std::string(kInk) + "\">" +
               html_escape(raw) + "</span>";

    std::string out;
    for (size_t i = 0; i < addrs.size(); ++i) {
        if (i) out += ", ";
        const MailAddress &a = addrs[i];
        std::string low = a.address;
        for (char &c : low) c = (char)std::tolower((unsigned char)c);

        if (a.name.empty()) {
            out += "<span style=\"color:" + std::string(kInk) + "\">" +
                   html_escape(a.address) + "</span>";
            continue;
        }
        const bool show_addr = expanded.count(low) > 0;
        out += "<a href=\"" + std::string(kAddrScheme) + html_escape(a.address) +
               "\" style=\"color:" + kInk + "\">" +
               html_escape(show_addr ? a.address : a.name) + "</a>";
    }
    return out;
}

/* One <div> card: subject, then the From/To/Date rows. */
std::string header_html(const MailMessage &msg,
                               const std::set<std::string> &expanded) {
    using namespace parchment;
    std::string h;
    h += std::string("<div style=\"background-color:") + kPaper +
         ";border:1px solid " + kEdge +
         ";border-radius:8px;padding:14px 16px;color:" + kInk + "\">";

    h += std::string("<p style=\"font-size:24px;color:") + kInkBold +
         "\"><b>" + html_escape(msg.subject) + "</b></p>";

    auto label_cell = [&](const char *label) {
        return std::string("<p style=\"font-size:15px\"><b style=\"color:") +
               kLabel + "\">" + label + " </b>";
    };

    h += label_cell("From:") + address_row_html(msg.from_addr.empty()
                                                ? msg.from
                                                : msg.from + " <" +
                                                  msg.from_addr + ">",
                                                expanded) + "</p>";
    if (!msg.to.empty())
        h += label_cell("To:") + address_row_html(msg.to, expanded) + "</p>";
    if (!msg.date.empty())
        h += label_cell("Date:") +
             "<span style=\"color:" + std::string(kMeta) + "\">" +
             html_escape(msg.date) + "</span></p>";

    h += "</div>";
    /* Vertical margin is not supported by the renderer, so separate the card
     * from the message body with an explicit spacer. */
    h += "<div style=\"height:12px\"></div>";
    return h;
}

/* Plain / Markdown body as an HTML fragment so it can share header_html()
 * with text/html mail.  Newlines stay as <br> (bounce reports, signatures);
 * Markdown goes through parse_markdown then paragraphs_to_html. */
std::string body_as_html(const MailMessage &msg) {
    if (msg.body.empty())
        return {};
    if (msg.body_markdown) {
        Document tmp;
        parse_markdown(tmp, msg.body);
        return paragraphs_to_html(tmp);
    }
    std::string esc = html_escape(msg.body);
    std::string out;
    out.reserve(esc.size() + 16);
    out += "<p>";
    for (char c : esc) {
        if (c == '\r') continue;
        if (c == '\n') out += "<br>";
        else           out += c;
    }
    out += "</p>";
    return out;
}
