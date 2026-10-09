/*
 * pagemade/docfile.cpp — see docfile.h.
 *
 * document.json is written with a small writer here (floats in their
 * shortest round-trip form, so files stay readable and diffable) and read
 * with dict.h.
 */
#include "docfile.h"
#include "uri.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "dict.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace pagemade {

namespace {

/* ---- Writing JSON -------------------------------------------------------- */

class JsonOut {
public:
    std::string text;

    JsonOut &key(const char *k) {
        prefix();
        quote(k);
        text += ": ";
        m_after_key = true;
        return *this;
    }
    void begin_object() { prefix(); text += '{'; m_first.push_back(true); }
    void end_object()   { close('}'); }
    void begin_array()  { prefix(); text += '['; m_first.push_back(true); }
    void end_array()    { close(']'); }
    void value(const std::string &s) { prefix(); quote(s); }
    void value(const char *s)        { value(std::string(s)); }
    void value(bool b)               { prefix(); text += b ? "true" : "false"; }
    void value(float f)              { prefix(); number(f); }
    void value(int i)                { prefix(); text += std::to_string(i); }
    void value(uint32_t u)           { prefix(); text += std::to_string(u); }
    /* A short list of numbers on one line: [1, 0, 0, 1, 54, 54]. */
    void numbers(std::initializer_list<float> v) {
        prefix();
        text += '[';
        bool first = true;
        for (float f : v) {
            if (!first) text += ", ";
            first = false;
            number(f);
        }
        text += ']';
    }
    void ids(const std::vector<uint32_t> &v) {
        prefix();
        text += '[';
        for (size_t i = 0; i < v.size(); ++i)
            text += (i ? ", " : "") + std::to_string(v[i]);
        text += ']';
    }

private:
    std::vector<bool> m_first;   // per open container: nothing written yet
    bool m_after_key = false;

    void newline() {
        text += '\n';
        text.append(m_first.size() * 2, ' ');
    }
    void prefix() {
        if (m_after_key) {
            m_after_key = false;
            return;
        }
        if (m_first.empty())
            return;
        if (!m_first.back())
            text += ',';
        m_first.back() = false;
        newline();
    }
    void close(char c) {
        const bool empty = m_first.back();
        m_first.pop_back();
        if (!empty)
            newline();
        text += c;
    }
    void number(float f) {
        if (!std::isfinite(f))
            f = 0.f;
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof buf, f);
        text.append(buf, r.ptr);
    }
    void quote(const std::string &s) {
        static const char digits[] = "0123456789abcdef";
        text += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"':  text += "\\\""; break;
            case '\\': text += "\\\\"; break;
            case '\n': text += "\\n"; break;
            case '\r': text += "\\r"; break;
            case '\t': text += "\\t"; break;
            default:
                if (c < 0x20) {
                    text += "\\u00";
                    text += digits[c >> 4];
                    text += digits[c & 15];
                } else {
                    text += (char) c;    // UTF-8 passes through
                }
            }
        }
        text += '"';
    }
};

/* ---- Names for enums ------------------------------------------------------ */

const char *align_name(Align a) {
    switch (a) {
    case Align::Left: return "left";
    case Align::Center: return "center";
    case Align::Right: return "right";
    case Align::Justify: return "justify";
    case Align::ForceJustify: return "force-justify";
    }
    return "left";
}
Align align_from(const std::string &s) {
    return s == "center" ? Align::Center : s == "right" ? Align::Right :
           s == "justify" ? Align::Justify : s == "force-justify" ? Align::ForceJustify : Align::Left;
}

const char *tab_name(TabAlign a) {
    switch (a) {
    case TabAlign::Left: return "left";
    case TabAlign::Center: return "center";
    case TabAlign::Right: return "right";
    case TabAlign::Decimal: return "decimal";
    }
    return "left";
}
TabAlign tab_from(const std::string &s) {
    return s == "center" ? TabAlign::Center : s == "right" ? TabAlign::Right :
           s == "decimal" ? TabAlign::Decimal : TabAlign::Left;
}

const char *line_style_name(LineStyle l) {
    switch (l) {
    case LineStyle::Solid: return "solid";
    case LineStyle::Dashed: return "dashed";
    case LineStyle::Dotted: return "dotted";
    case LineStyle::DashDot: return "dash-dot";
    }
    return "solid";
}
LineStyle line_style_from(const std::string &s) {
    return s == "dashed" ? LineStyle::Dashed : s == "dotted" ? LineStyle::Dotted :
           s == "dash-dot" ? LineStyle::DashDot : LineStyle::Solid;
}

const char *shape_name(Shape::Kind k) {
    switch (k) {
    case Shape::Kind::Rect: return "rectangle";
    case Shape::Kind::Ellipse: return "ellipse";
    case Shape::Kind::Line: return "line";
    case Shape::Kind::Polygon: return "polygon";
    }
    return "rectangle";
}
Shape::Kind shape_from(const std::string &s) {
    return s == "ellipse" ? Shape::Kind::Ellipse : s == "line" ? Shape::Kind::Line :
           s == "polygon" ? Shape::Kind::Polygon : Shape::Kind::Rect;
}

/* ---- Model -> JSON (only what differs from the defaults) ------------------ */

void write_color(JsonOut &j, const char *key, const Color &c) {
    j.key(key).numbers({c.r, c.g, c.b, c.a});
}

void write_char_style(JsonOut &j, const CharStyle &cs) {
    const CharStyle d;
    j.begin_object();
    if (cs.family != d.family) j.key("family").value(cs.family);
    if (cs.bold) j.key("bold").value(true);
    if (cs.italic) j.key("italic").value(true);
    if (cs.size != d.size) j.key("size").value(cs.size);
    if (cs.leading != d.leading) j.key("leading").value(cs.leading);
    if (cs.tracking != d.tracking) j.key("tracking").value(cs.tracking);
    if (cs.hscale != d.hscale) j.key("hscale").value(cs.hscale);
    if (cs.baseline_shift != d.baseline_shift) j.key("baseline_shift").value(cs.baseline_shift);
    if (!cs.kerning) j.key("kerning").value(false);
    if (!cs.ligatures) j.key("ligatures").value(false);
    if (!(cs.color == d.color)) write_color(j, "color", cs.color);
    j.end_object();
}

void write_para_style(JsonOut &j, const ParaStyle &ps) {
    const ParaStyle d;
    j.begin_object();
    if (!ps.name.empty()) j.key("name").value(ps.name);
    if (ps.align != d.align) j.key("align").value(align_name(ps.align));
    if (ps.left_indent != d.left_indent) j.key("left_indent").value(ps.left_indent);
    if (ps.right_indent != d.right_indent) j.key("right_indent").value(ps.right_indent);
    if (ps.first_indent != d.first_indent) j.key("first_indent").value(ps.first_indent);
    if (ps.space_before != d.space_before) j.key("space_before").value(ps.space_before);
    if (ps.space_after != d.space_after) j.key("space_after").value(ps.space_after);
    if (ps.autoleading != d.autoleading) j.key("autoleading").value(ps.autoleading);
    if (ps.word_min != d.word_min || ps.word_desired != d.word_desired || ps.word_max != d.word_max)
        j.key("word_spacing").numbers({ps.word_min, ps.word_desired, ps.word_max});
    if (ps.letter_min != d.letter_min || ps.letter_desired != d.letter_desired ||
        ps.letter_max != d.letter_max)
        j.key("letter_spacing").numbers({ps.letter_min, ps.letter_desired, ps.letter_max});
    if (!ps.tabs.empty()) {
        j.key("tabs").begin_array();
        for (const TabStop &t : ps.tabs) {
            j.begin_object();
            j.key("pos").value(t.pos);
            if (t.align != TabAlign::Left) j.key("align").value(tab_name(t.align));
            if (!t.leader.empty()) j.key("leader").value(t.leader);
            j.end_object();
        }
        j.end_array();
    }
    if (ps.default_tab != d.default_tab) j.key("default_tab").value(ps.default_tab);
    if (ps.hyphenate != d.hyphenate) j.key("hyphenate").value(ps.hyphenate);
    if (ps.hyphen_limit != d.hyphen_limit) j.key("hyphen_limit").value(ps.hyphen_limit);
    if (ps.hyphen_zone != d.hyphen_zone) j.key("hyphen_zone").value(ps.hyphen_zone);
    j.end_object();
}

void write_paint(JsonOut &j, const Paint &p) {
    j.begin_object();
    j.key("swatch").value(p.swatch);
    if (p.tint != 100.f) j.key("tint").value(p.tint);
    j.end_object();
}

void write_item(JsonOut &j, const Item &it) {
    j.begin_object();
    j.key("id").value(it.id);
    j.key("w").value(it.w);
    j.key("h").value(it.h);
    j.key("xf").numbers({it.xf.a, it.xf.b, it.xf.c, it.xf.d, it.xf.e, it.xf.f});
    if (const Shape *sh = it.shape()) {
        const Shape d;
        j.key("type").value("shape");
        j.key("shape").value(shape_name(sh->kind));
        if (sh->corner_radius != d.corner_radius) j.key("corner_radius").value(sh->corner_radius);
        if (sh->kind == Shape::Kind::Polygon) {
            j.key("sides").value(sh->sides);
            if (sh->star_inset != 0.f) j.key("star_inset").value(sh->star_inset);
        }
        j.key("fill");
        write_paint(j, sh->fill);
        j.key("stroke").begin_object();
        j.key("swatch").value(sh->stroke.paint.swatch);
        if (sh->stroke.paint.tint != 100.f) j.key("tint").value(sh->stroke.paint.tint);
        j.key("weight").value(sh->stroke.weight);
        if (sh->stroke.style != LineStyle::Solid) j.key("style").value(line_style_name(sh->stroke.style));
        j.end_object();
    } else {
        j.key("type").value("text");
    }
    j.end_object();
}

/* ---- Fonts the document uses --------------------------------------------- */

struct FontRef {
    std::string family, style;
    const Font *font = nullptr;
};

std::vector<FontRef> used_fonts(const PageDoc &doc, const FontLibrary &fonts) {
    std::vector<FontRef> out;
    for (const StoryEntry &se : doc.stories)
        for (const Paragraph &p : se.story.paragraphs)
            for (const Run &r : p.runs) {
                const std::string style = FontLibrary::style_name(r.style.bold, r.style.italic);
                bool seen = false;
                for (const FontRef &f : out)
                    seen |= f.family == r.style.family && f.style == style;
                if (!seen)
                    out.push_back({r.style.family, style,
                                   fonts.face_for(r.style.family, r.style.bold, r.style.italic)});
            }
    return out;
}

/* Where each font goes: embedded entries and every font's URI. */
struct FontPlan {
    struct Embedded { const Font *font; std::string entry; };
    std::vector<Embedded> embedded;
    std::map<const Font *, std::string> uri;
};

std::string safe_file_name(std::string name) {
    for (char &c : name)
        if (!std::isalnum((unsigned char) c) && c != '.' && c != '-' && c != '_')
            c = '_';
    return name.empty() ? std::string("font") : name;
}

FontPlan plan_fonts(const std::vector<FontRef> &refs, const SaveOptions &opts) {
    FontPlan plan;
    std::set<std::string> names;
    for (const FontRef &r : refs) {
        if (!r.font || plan.uri.count(r.font))
            continue;
        if (opts.embed_assets || r.font->path().empty()) {
            std::string name = safe_file_name(r.font->name());
            const std::string base = name;
            for (int n = 2; names.count(name); ++n) {
                const size_t dot = base.find_last_of('.');
                name = dot == std::string::npos ? base + "-" + std::to_string(n)
                                                : base.substr(0, dot) + "-" + std::to_string(n) +
                                                  base.substr(dot);
            }
            names.insert(name);
            plan.embedded.push_back({r.font, "assets/fonts/" + name});
            plan.uri[r.font] = package_ref("assets/fonts", name);
        } else {
            plan.uri[r.font] = file_uri(r.font->path());
        }
    }
    return plan;
}

std::string make_json(const PageDoc &doc, const SaveOptions &opts, const std::vector<FontRef> &refs,
                      const FontPlan &plan) {
    JsonOut j;
    j.begin_object();
    j.key("format").value("pagemade");
    j.key("version").value(kFormatVersion);
    j.key("embed_assets").value(opts.embed_assets);

    const PageSetup &s = doc.setup;
    j.key("setup").begin_object();
    j.key("width").value(s.width);
    j.key("height").value(s.height);
    j.key("margins").numbers({s.margin_top, s.margin_bottom, s.margin_inside, s.margin_outside});
    j.key("columns").value(s.columns);
    j.key("gutter").value(s.gutter);
    j.end_object();

    j.key("swatches").begin_array();
    for (const Swatch &sw : doc.swatches) {
        j.begin_object();
        j.key("id").value(sw.id);
        j.key("name").value(sw.name);
        write_color(j, "rgb", sw.rgb);
        j.end_object();
    }
    j.end_array();

    j.key("assets").begin_array();
    for (const FontRef &r : refs) {
        if (!r.font)
            continue;
        j.begin_object();
        j.key("kind").value("font");
        j.key("family").value(r.family);
        j.key("style").value(r.style);
        j.key("postscript").value(r.font->postscript_name());
        j.key("uri").value(plan.uri.at(r.font));
        if (!r.font->path().empty())
            j.key("source").value(file_uri(r.font->path()));
        j.end_object();
    }
    j.end_array();

    j.key("stories").begin_array();
    for (const StoryEntry &se : doc.stories) {
        j.begin_object();
        j.key("id").value(se.id);
        j.key("thread").ids(se.thread);
        j.key("paragraphs").begin_array();
        for (const Paragraph &p : se.story.paragraphs) {
            j.begin_object();
            j.key("style");
            write_para_style(j, p.style);
            j.key("runs").begin_array();
            for (const Run &r : p.runs) {
                j.begin_object();
                j.key("style");
                write_char_style(j, r.style);
                j.key("text").value(r.text);
                j.end_object();
            }
            j.end_array();
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();

    j.key("pages").begin_array();
    for (const Page &p : doc.pages) {
        j.begin_object();
        if (p.hidden) j.key("hidden").value(true);
        j.key("items").begin_array();
        for (const Item &it : p.items)
            write_item(j, it);
        j.end_array();
        j.end_object();
    }
    j.end_array();

    j.key("next_id").value(doc.next_id);
    j.end_object();
    j.text += '\n';
    return j.text;
}

/* ---- JSON -> model -------------------------------------------------------- */

const DictValue *field(const DictValue *o, const char *k) {
    return o && o->type == DICT_OBJECT ? dict_object_get(o, k) : nullptr;
}
size_t count(const DictValue *a) {
    return a && a->type == DICT_ARRAY ? a->array_value.length : 0;
}
const DictValue *at(const DictValue *a, size_t i) { return a->array_value.items[i]; }

bool number(const DictValue *v, double &out) {
    if (v && v->type == DICT_NUMBER) { out = v->number_value; return true; }
    if (v && v->type == DICT_INT64) { out = (double) v->int64_value; return true; }
    return false;
}
void get(const DictValue *o, const char *k, float &out) {
    double d;
    if (number(field(o, k), d) && std::isfinite(d)) out = (float) d;
}
void get(const DictValue *o, const char *k, int &out) {
    double d;
    if (number(field(o, k), d) && std::isfinite(d)) out = (int) d;
}
void get(const DictValue *o, const char *k, uint32_t &out) {
    double d;
    if (number(field(o, k), d) && d >= 0 && d <= 4294967295.0) out = (uint32_t) d;
}
void get(const DictValue *o, const char *k, bool &out) {
    const DictValue *v = field(o, k);
    if (v && v->type == DICT_BOOL) out = v->bool_value != 0;
}
void get(const DictValue *o, const char *k, std::string &out) {
    const DictValue *v = field(o, k);
    if (v && v->type == DICT_STRING && v->string_value) out = v->string_value;
}
/* Up to n numbers from an array field. */
size_t floats(const DictValue *o, const char *k, float *out, size_t n) {
    const DictValue *a = field(o, k);
    size_t m = std::min(n, count(a));
    for (size_t i = 0; i < m; ++i) {
        double d;
        if (number(at(a, i), d)) out[i] = (float) d;
    }
    return m;
}
void get(const DictValue *o, const char *k, Color &c) {
    float v[4] = {c.r, c.g, c.b, c.a};
    if (floats(o, k, v, 4) >= 3)
        c = {v[0], v[1], v[2], v[3]};
}

CharStyle read_char_style(const DictValue *o) {
    CharStyle cs;
    get(o, "family", cs.family);
    get(o, "bold", cs.bold);
    get(o, "italic", cs.italic);
    get(o, "size", cs.size);
    get(o, "leading", cs.leading);
    get(o, "tracking", cs.tracking);
    get(o, "hscale", cs.hscale);
    get(o, "baseline_shift", cs.baseline_shift);
    get(o, "kerning", cs.kerning);
    get(o, "ligatures", cs.ligatures);
    get(o, "color", cs.color);
    if (!(cs.size > 0)) cs.size = CharStyle().size;
    return cs;
}

ParaStyle read_para_style(const DictValue *o) {
    ParaStyle ps;
    std::string align;
    get(o, "name", ps.name);
    get(o, "align", align);
    if (!align.empty()) ps.align = align_from(align);
    get(o, "left_indent", ps.left_indent);
    get(o, "right_indent", ps.right_indent);
    get(o, "first_indent", ps.first_indent);
    get(o, "space_before", ps.space_before);
    get(o, "space_after", ps.space_after);
    get(o, "autoleading", ps.autoleading);
    float w[3] = {ps.word_min, ps.word_desired, ps.word_max};
    if (floats(o, "word_spacing", w, 3) == 3) {
        ps.word_min = w[0]; ps.word_desired = w[1]; ps.word_max = w[2];
    }
    float l[3] = {ps.letter_min, ps.letter_desired, ps.letter_max};
    if (floats(o, "letter_spacing", l, 3) == 3) {
        ps.letter_min = l[0]; ps.letter_desired = l[1]; ps.letter_max = l[2];
    }
    const DictValue *tabs = field(o, "tabs");
    for (size_t i = 0; i < count(tabs); ++i) {
        TabStop t;
        std::string a;
        get(at(tabs, i), "pos", t.pos);
        get(at(tabs, i), "align", a);
        get(at(tabs, i), "leader", t.leader);
        t.align = tab_from(a);
        ps.tabs.push_back(t);
    }
    std::sort(ps.tabs.begin(), ps.tabs.end(),
              [](const TabStop &a, const TabStop &b) { return a.pos < b.pos; });
    get(o, "default_tab", ps.default_tab);
    get(o, "hyphenate", ps.hyphenate);
    get(o, "hyphen_limit", ps.hyphen_limit);
    get(o, "hyphen_zone", ps.hyphen_zone);
    return ps;
}

Paint read_paint(const DictValue *o, Paint p) {
    get(o, "swatch", p.swatch);
    get(o, "tint", p.tint);
    return p;
}

/* False for item types this version doesn't know (they're skipped). */
bool read_item(const DictValue *o, Item &it) {
    get(o, "id", it.id);
    get(o, "w", it.w);
    get(o, "h", it.h);
    float xf[6] = {1, 0, 0, 1, 0, 0};
    floats(o, "xf", xf, 6);
    it.xf = {xf[0], xf[1], xf[2], xf[3], xf[4], xf[5]};
    std::string type;
    get(o, "type", type);
    if (type == "text") {
        it.content = TextFrame{};
        return true;
    }
    if (type != "shape")
        return false;
    Shape sh;
    std::string kind;
    get(o, "shape", kind);
    sh.kind = shape_from(kind);
    get(o, "corner_radius", sh.corner_radius);
    get(o, "sides", sh.sides);
    sh.sides = std::max(3, sh.sides);
    get(o, "star_inset", sh.star_inset);
    sh.fill = read_paint(field(o, "fill"), Paint{});
    if (const DictValue *st = field(o, "stroke")) {
        sh.stroke.paint = read_paint(st, sh.stroke.paint);
        get(st, "weight", sh.stroke.weight);
        std::string style;
        get(st, "style", style);
        sh.stroke.style = line_style_from(style);
    }
    it.content = sh;
    return true;
}

/* Keep the model's promises whatever the file says: unique ids, every
 * text frame in exactly one thread, threads naming only text frames,
 * at least one page, the built-in swatches. */
void repair(PageDoc &d, std::vector<std::string> &warnings) {
    if (d.pages.empty())
        d.pages.resize(1);
    for (const Swatch &s : default_swatches())
        if (s.id <= kRegistration && !d.find_swatch(s.id))
            d.swatches.push_back(s);

    uint32_t max_id = 0;
    std::set<ItemId> ids;
    size_t dropped = 0;
    for (Page &p : d.pages) {
        auto bad = [&](const Item &it) { return it.id == 0 || !ids.insert(it.id).second; };
        auto end = std::remove_if(p.items.begin(), p.items.end(), bad);
        dropped += (size_t) (p.items.end() - end);
        p.items.erase(end, p.items.end());
    }
    if (dropped)
        warnings.push_back(std::to_string(dropped) + " items with missing or repeated ids were dropped");
    for (ItemId id : ids)
        max_id = std::max(max_id, id);

    std::set<ItemId> threaded;
    std::set<StoryId> story_ids;
    for (StoryEntry &se : d.stories) {
        std::vector<ItemId> keep;
        for (ItemId id : se.thread) {
            const Item *it = d.find_item(id);
            if (it && it->is_text() && threaded.insert(id).second)
                keep.push_back(id);
        }
        se.thread = keep;
        if (se.story.paragraphs.empty())
            se.story.paragraphs.emplace_back();
        if (se.id == 0 || !story_ids.insert(se.id).second)
            se.id = 0;                   // renumbered below
        max_id = std::max(max_id, se.id);
    }
    const size_t before = d.stories.size();
    d.stories.erase(std::remove_if(d.stories.begin(), d.stories.end(),
                                   [](const StoryEntry &s) { return s.thread.empty(); }),
                    d.stories.end());
    if (d.stories.size() != before)
        warnings.push_back(std::to_string(before - d.stories.size()) +
                           " stories with no text blocks were dropped");
    for (StoryEntry &se : d.stories)
        if (se.id == 0)
            se.id = ++max_id;

    /* A text block no story threads gets an empty story of its own. */
    for (Page &p : d.pages)
        for (const Item &it : p.items)
            if (it.is_text() && !threaded.count(it.id)) {
                StoryEntry se;
                se.id = ++max_id;
                se.story.paragraphs.emplace_back();
                se.thread.push_back(it.id);
                d.stories.push_back(std::move(se));
            }
    d.next_id = std::max(d.next_id, max_id + 1);
}

std::vector<char> extract(mz_zip_archive &zip, const std::string &entry, bool *found) {
    size_t n = 0;
    void *p = mz_zip_reader_extract_file_to_heap(&zip, entry.c_str(), &n, 0);
    if (found)
        *found = p != nullptr;
    std::vector<char> out;
    if (p) {
        out.assign((const char *) p, (const char *) p + n);
        mz_free(p);
    }
    return out;
}

} // namespace

/* ---- Public API ------------------------------------------------------------ */

std::string document_json(const PageDoc &doc, const FontLibrary &fonts, const SaveOptions &opts) {
    const std::vector<FontRef> refs = used_fonts(doc, fonts);
    return make_json(doc, opts, refs, plan_fonts(refs, opts));
}

bool save_document(const PageDoc &doc, const FontLibrary &fonts, const std::string &path,
                   const SaveOptions &opts, std::string *error) {
    const std::vector<FontRef> refs = used_fonts(doc, fonts);
    const FontPlan plan = plan_fonts(refs, opts);
    const std::string json = make_json(doc, opts, refs, plan);
    const std::string tmp = path + ".saving";

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    auto fail = [&](const std::string &why) {
        if (error)
            *error = why;
        std::remove(tmp.c_str());
        return false;
    };
    if (!mz_zip_writer_init_file(&zip, tmp.c_str(), 0))
        return fail("couldn't create " + tmp + ": " +
                    mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    bool ok = mz_zip_writer_add_mem(&zip, "mimetype", kPublicationMimeType,
                                    std::strlen(kPublicationMimeType), MZ_NO_COMPRESSION);
    ok = ok && mz_zip_writer_add_mem(&zip, "document.json", json.data(), json.size(),
                                     MZ_DEFAULT_COMPRESSION);
    for (const auto &e : plan.embedded) {
        size_t n = 0;
        const char *data = e.font->data(&n);
        ok = ok && mz_zip_writer_add_mem(&zip, e.entry.c_str(), data, n, MZ_DEFAULT_COMPRESSION);
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    const std::string zip_error = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    mz_zip_writer_end(&zip);
    if (!ok)
        return fail("couldn't write the publication: " + zip_error);
#if defined(_WIN32)
    std::remove(path.c_str());           // rename doesn't replace on Windows
#endif
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return fail("couldn't replace " + path + ": " + std::strerror(errno));
    return true;
}

OpenResult open_document(const std::string &path, const FontLibrary &installed) {
    OpenResult r;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        r.error = "not a pagemade publication (it isn't a zip package)";
        return r;
    }
    struct Closer {
        mz_zip_archive &z;
        ~Closer() { mz_zip_reader_end(&z); }
    } closer{zip};

    bool found = false;
    std::vector<char> mime = extract(zip, "mimetype", &found);
    if (found && std::string(mime.begin(), mime.end()) != kPublicationMimeType) {
        r.error = "not a pagemade publication (" + std::string(mime.begin(), mime.end()) + ")";
        return r;
    }
    std::vector<char> json = extract(zip, "document.json", &found);
    if (!found || json.empty()) {
        r.error = "the package has no document.json";
        return r;
    }
    char err[256] = {0};
    DictValue *root = dict_deserialize_json(json.data(), json.size(), json.size(), err, sizeof err);
    if (!root) {
        r.error = std::string("document.json is damaged: ") + err;
        return r;
    }
    struct Freer {
        DictValue *v;
        ~Freer() { dict_destroy(v); }
    } freer{root};

    std::string format;
    int version = 0;
    get(root, "format", format);
    get(root, "version", version);
    if (format != "pagemade") {
        r.error = "document.json isn't a pagemade document";
        return r;
    }
    if (version > kFormatVersion)
        r.warnings.push_back("made by a newer pagemade (format " + std::to_string(version) +
                             "); anything this version doesn't know is left out");
    get(root, "embed_assets", r.embed_assets);

    PageDoc &d = r.doc;
    const DictValue *setup = field(root, "setup");
    get(setup, "width", d.setup.width);
    get(setup, "height", d.setup.height);
    float m[4] = {d.setup.margin_top, d.setup.margin_bottom, d.setup.margin_inside,
                  d.setup.margin_outside};
    floats(setup, "margins", m, 4);
    d.setup.margin_top = m[0];
    d.setup.margin_bottom = m[1];
    d.setup.margin_inside = m[2];
    d.setup.margin_outside = m[3];
    get(setup, "columns", d.setup.columns);
    d.setup.columns = std::max(1, d.setup.columns);
    get(setup, "gutter", d.setup.gutter);

    if (const DictValue *sw = field(root, "swatches")) {
        d.swatches.clear();
        for (size_t i = 0; i < count(sw); ++i) {
            Swatch s;
            get(at(sw, i), "id", s.id);
            get(at(sw, i), "name", s.name);
            get(at(sw, i), "rgb", s.rgb);
            if (s.id != kNoPaint && !d.find_swatch(s.id))
                d.swatches.push_back(s);
        }
    }

    const DictValue *stories = field(root, "stories");
    for (size_t i = 0; i < count(stories); ++i) {
        const DictValue *so = at(stories, i);
        StoryEntry se;
        get(so, "id", se.id);
        const DictValue *thread = field(so, "thread");
        for (size_t t = 0; t < count(thread); ++t) {
            double id;
            if (number(at(thread, t), id) && id > 0)
                se.thread.push_back((ItemId) id);
        }
        const DictValue *paras = field(so, "paragraphs");
        for (size_t p = 0; p < count(paras); ++p) {
            Paragraph para;
            para.style = read_para_style(field(at(paras, p), "style"));
            const DictValue *runs = field(at(paras, p), "runs");
            for (size_t k = 0; k < count(runs); ++k) {
                Run run;
                run.style = read_char_style(field(at(runs, k), "style"));
                get(at(runs, k), "text", run.text);
                para.runs.push_back(std::move(run));
            }
            se.story.paragraphs.push_back(std::move(para));
        }
        d.stories.push_back(std::move(se));
    }

    const DictValue *pages = field(root, "pages");
    if (count(pages))
        d.pages.clear();
    size_t unknown = 0;
    for (size_t i = 0; i < count(pages); ++i) {
        Page page;
        get(at(pages, i), "hidden", page.hidden);
        const DictValue *items = field(at(pages, i), "items");
        for (size_t k = 0; k < count(items); ++k) {
            Item it;
            if (read_item(at(items, k), it))
                page.items.push_back(std::move(it));
            else
                ++unknown;
        }
        d.pages.push_back(std::move(page));
    }
    if (unknown)
        r.warnings.push_back(std::to_string(unknown) + " items of kinds this version doesn't know were left out");
    get(root, "next_id", d.next_id);
    repair(d, r.warnings);

    /* Fonts: embedded copies always; linked files only for families that
     * aren't installed here. */
    std::set<std::string> provided;      // "family/style" found
    const DictValue *assets = field(root, "assets");
    for (size_t i = 0; i < count(assets); ++i) {
        const DictValue *a = at(assets, i);
        std::string kind, family, style, uri;
        get(a, "kind", kind);
        get(a, "family", family);
        get(a, "style", style);
        get(a, "uri", uri);
        if (kind != "font" || family.empty())
            continue;
        std::shared_ptr<Font> font;
        bool embedded = false;
        std::string file_path;
        if (is_package_ref(uri)) {
            const std::string entry = package_entry(uri);
            std::vector<char> bytes = extract(zip, entry, &found);
            const size_t slash = entry.find_last_of('/');
            font = found ? Font::load_bytes(std::move(bytes), entry.substr(slash + 1)) : nullptr;
            embedded = true;
            if (!font)
                r.warnings.push_back("the embedded font " + entry + " couldn't be read");
        } else if (!installed.has_installed_family(family) && file_path_from_uri(uri, file_path)) {
            font = Font::load_file(file_path);
        }
        if (font) {
            r.fonts.push_back({family, style, font, embedded});
            provided.insert(family + "/" + style);
        }
    }
    std::set<std::string> reported;
    for (const StoryEntry &se : d.stories)
        for (const Paragraph &p : se.story.paragraphs)
            for (const Run &run : p.runs) {
                const std::string style = FontLibrary::style_name(run.style.bold, run.style.italic);
                if (provided.count(run.style.family + "/" + style) ||
                    installed.has_installed_family(run.style.family))
                    continue;
                if (reported.insert(run.style.family + " " + style).second)
                    r.missing_fonts.push_back(run.style.family + " " + style);
            }

    r.ok = true;
    return r;
}

} // namespace pagemade
