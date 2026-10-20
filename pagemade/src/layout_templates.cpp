/*
 * pagemade/layout_templates.cpp — see layout_templates.h.
 *
 * layouts.json is written with JsonOut and read with dict.h, the same as
 * document.json (json_util.h). Fields left out take their defaults, so a
 * hand-written template only says what it means.
 */
#include "layout_templates.h"

#include "geometry.h"
#include "json_util.h"
#include "vector.h"

#include <nanogui/fscompat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace pagemade {

using namespace json;

size_t LayoutTemplate::photo_count() const {
    size_t n = 0;
    for (const LayoutElement &e : elements)
        n += e.kind == LayoutElement::Kind::Photo;
    return n;
}

/* ---- Turning -------------------------------------------------------------- */

namespace {

/* One quarter turn clockwise of a box `w` wide and `h` high: (x, y) lands
 * on (h - y, x) in the turned box, which is h wide. */
LayoutRect turn_rect(const LayoutRect &r, float h) {
    return {h - r.y - r.h, r.x, r.h, r.w};
}

/* A line runs from (x, y) to (x + w, y + h): turn both ends. */
LayoutRect turn_line(const LayoutRect &r, float h) {
    return {h - r.y, r.x, -r.h, r.w};
}

LayoutTemplate turn_once(const LayoutTemplate &t) {
    LayoutTemplate out = t;
    out.width = t.height;
    out.height = t.width;
    out.spread = false;                  // a spread on its side is one tall page
    out.margins.top = t.margins.left;
    out.margins.right = t.margins.top;
    out.margins.bottom = t.margins.right;
    out.margins.left = t.margins.bottom;
    for (LayoutElement &e : out.elements) {
        const bool line = e.kind == LayoutElement::Kind::Shape && e.shape == Shape::Kind::Line;
        e.rect = line ? turn_line(e.rect, t.height) : turn_rect(e.rect, t.height);
    }
    return out;
}

} // namespace

LayoutTemplate rotate_layout(const LayoutTemplate &t, int quarter_turns) {
    const int n = ((quarter_turns % 4) + 4) % 4;
    if (n == 0)
        return t;
    LayoutTemplate out = t;
    for (int i = 0; i < n; ++i)
        out = turn_once(out);
    /* Half a turn keeps a spread a spread (its pages trade places) */
    if (n == 2)
        out.spread = t.spread;
    return out;
}

/* ---- JSON ------------------------------------------------------------------ */

namespace {

const char *kind_name(LayoutElement::Kind k) {
    switch (k) {
    case LayoutElement::Kind::Photo: return "photo";
    case LayoutElement::Kind::Text: return "text";
    case LayoutElement::Kind::Shape: return "shape";
    case LayoutElement::Kind::Svg: return "svg";
    }
    return "photo";
}

bool kind_from(const std::string &s, LayoutElement::Kind &k) {
    for (LayoutElement::Kind c : {LayoutElement::Kind::Photo, LayoutElement::Kind::Text,
                                  LayoutElement::Kind::Shape, LayoutElement::Kind::Svg})
        if (s == kind_name(c)) {
            k = c;
            return true;
        }
    return false;
}

void write_run_fields(JsonOut &j, const LayoutRun &r) {
    if (!r.family.empty()) j.key("family").value(r.family);
    if (!r.face.empty()) j.key("face").value(r.face);
    if (r.has_bold) j.key("bold").value(r.bold);
    if (r.has_italic) j.key("italic").value(r.italic);
    if (r.has_caps)
        j.key("caps").value(r.caps == Caps::Small ? "small" : r.caps == Caps::All ? "all" : "normal");
    if (r.underline) j.key("underline").value(true);
    if (r.size > 0.f) j.key("size").value(r.size);
    if (r.leading >= 0.f) j.key("leading").value(r.leading);
    if (r.has_tracking) j.key("tracking").value(r.tracking);
    if (!r.swatch.empty()) {
        j.key("swatch").value(r.swatch);
        if (r.tint != 100.f) j.key("tint").value(r.tint);
    }
}

void read_run_fields(const DictValue *o, LayoutRun &r) {
    get(o, "family", r.family);
    get(o, "face", r.face);
    if (field(o, "bold")) { r.has_bold = true; get(o, "bold", r.bold); }
    if (field(o, "italic")) { r.has_italic = true; get(o, "italic", r.italic); }
    std::string caps;
    get(o, "caps", caps);
    if (!caps.empty()) {
        r.has_caps = true;
        r.caps = caps == "small" ? Caps::Small : caps == "all" ? Caps::All : Caps::Normal;
    }
    get(o, "underline", r.underline);
    get(o, "size", r.size);
    get(o, "leading", r.leading);
    if (field(o, "tracking")) { r.has_tracking = true; get(o, "tracking", r.tracking); }
    get(o, "swatch", r.swatch);
    get(o, "tint", r.tint);
    r.tint = std::clamp(r.tint, 0.f, 100.f);
}

void write_paint(JsonOut &j, const char *key, const std::string &swatch, float tint,
                 const float *weight = nullptr, LineStyle style = LineStyle::Solid) {
    if (swatch.empty())
        return;
    j.key(key).begin_object();
    j.key("swatch").value(swatch);
    if (tint != 100.f) j.key("tint").value(tint);
    if (weight) j.key("weight").value(*weight);
    if (style != LineStyle::Solid) j.key("style").value(line_style_name(style));
    j.end_object();
}

void read_paint(const DictValue *o, std::string &swatch, float &tint, float *weight = nullptr,
                LineStyle *style = nullptr) {
    if (!o)
        return;
    if (o->type == DICT_STRING && o->string_value) {     // "fill": "[Black]"
        swatch = o->string_value;
        return;
    }
    get(o, "swatch", swatch);
    get(o, "tint", tint);
    tint = std::clamp(tint, 0.f, 100.f);
    if (weight) {
        get(o, "weight", *weight);
        *weight = std::max(0.f, *weight);
    }
    if (style) {
        std::string s;
        get(o, "style", s);
        *style = line_style_from(s);
    }
}

void write_element(JsonOut &j, const LayoutElement &e) {
    j.begin_object();
    j.key("type").value(kind_name(e.kind));
    j.key("rect").numbers({e.rect.x, e.rect.y, e.rect.w, e.rect.h});
    switch (e.kind) {
    case LayoutElement::Kind::Shape:
        j.key("shape").value(shape_name(e.shape));
        if (e.corner_radius > 0.f) j.key("corner_radius").value(e.corner_radius);
        write_paint(j, "fill", e.fill_swatch, e.fill_tint);
        [[fallthrough]];
    case LayoutElement::Kind::Photo:
        write_paint(j, "stroke", e.stroke_swatch, e.stroke_tint, &e.stroke_weight, e.stroke_style);
        break;
    case LayoutElement::Kind::Text:
        j.key("paragraphs").begin_array();
        for (const LayoutParagraph &p : e.paragraphs) {
            j.begin_object();
            j.key("style").value(p.style);
            if (p.has_align) j.key("align").value(align_name(p.align));
            if (p.space_before >= 0.f) j.key("space_before").value(p.space_before);
            if (p.space_after >= 0.f) j.key("space_after").value(p.space_after);
            write_run_fields(j, p.type);
            if (p.runs.size() == 1 && p.runs[0].family.empty() && p.runs[0].face.empty() &&
                !p.runs[0].has_bold && !p.runs[0].has_italic && !p.runs[0].has_caps &&
                !p.runs[0].underline && p.runs[0].size <= 0.f && p.runs[0].leading < 0.f &&
                !p.runs[0].has_tracking && p.runs[0].swatch.empty()) {
                j.key("text").value(p.runs[0].text);     // one plain run: the shorthand
            } else {
                j.key("runs").begin_array();
                for (const LayoutRun &r : p.runs) {
                    j.begin_object();
                    j.key("text").value(r.text);
                    write_run_fields(j, r);
                    j.end_object();
                }
                j.end_array();
            }
            j.end_object();
        }
        j.end_array();
        break;
    case LayoutElement::Kind::Svg:
        j.key("svg").value(e.svg);
        break;
    }
    j.end_object();
}

bool read_element(const DictValue *o, LayoutElement &e) {
    std::string type;
    get(o, "type", type);
    if (!kind_from(type, e.kind))
        return false;                    // a newer kind: skipped
    float r[4] = {0, 0, 0, 0};
    if (floats(o, "rect", r, 4) != 4)
        return false;
    e.rect = {r[0], r[1], r[2], r[3]};
    switch (e.kind) {
    case LayoutElement::Kind::Shape: {
        std::string shape;
        get(o, "shape", shape);
        e.shape = shape_from(shape);
        get(o, "corner_radius", e.corner_radius);
        read_paint(field(o, "fill"), e.fill_swatch, e.fill_tint);
        read_paint(field(o, "stroke"), e.stroke_swatch, e.stroke_tint, &e.stroke_weight,
                   &e.stroke_style);
        break;
    }
    case LayoutElement::Kind::Photo:
        read_paint(field(o, "stroke"), e.stroke_swatch, e.stroke_tint, &e.stroke_weight,
                   &e.stroke_style);
        break;
    case LayoutElement::Kind::Text: {
        const DictValue *paras = field(o, "paragraphs");
        for (size_t i = 0; i < count(paras); ++i) {
            const DictValue *po = at(paras, i);
            LayoutParagraph p;
            get(po, "style", p.style);
            std::string align;
            get(po, "align", align);
            if (!align.empty()) {
                p.has_align = true;
                p.align = align_from(align);
            }
            get(po, "space_before", p.space_before);
            get(po, "space_after", p.space_after);
            read_run_fields(po, p.type);
            const DictValue *runs = field(po, "runs");
            for (size_t k = 0; k < count(runs); ++k) {
                LayoutRun run;
                get(at(runs, k), "text", run.text);
                read_run_fields(at(runs, k), run);
                p.runs.push_back(std::move(run));
            }
            if (p.runs.empty()) {
                LayoutRun run;
                get(po, "text", run.text);
                p.runs.push_back(std::move(run));
            }
            e.paragraphs.push_back(std::move(p));
        }
        if (e.paragraphs.empty())
            e.paragraphs.emplace_back();
        break;
    }
    case LayoutElement::Kind::Svg:
        get(o, "svg", e.svg);
        if (e.svg.empty())
            return false;
        break;
    }
    return true;
}

} // namespace

std::string layout_templates_json(const std::vector<LayoutTemplate> &templates) {
    JsonOut j;
    j.begin_object();
    j.key("format").value("pagemade-layouts");
    j.key("version").value(1);
    j.key("templates").begin_array();
    for (const LayoutTemplate &t : templates) {
        j.begin_object();
        j.key("name").value(t.name);
        if (!t.category.empty()) j.key("category").value(t.category);
        j.key("size").numbers({t.width, t.height});
        if (t.spread) j.key("spread").value(true);
        const LayoutMargins &m = t.margins;
        j.key("margins").begin_object();
        j.key("top").value(m.top);
        j.key("right").value(m.right);
        j.key("bottom").value(m.bottom);
        j.key("left").value(m.left);
        if (t.spread) j.key("inside").value(m.inside);
        j.end_object();
        j.key("elements").begin_array();
        for (const LayoutElement &e : t.elements)
            write_element(j, e);
        j.end_array();
        j.end_object();
    }
    j.end_array();
    j.end_object();
    j.text += '\n';
    return j.text;
}

bool layout_templates_from_json(const std::string &text, std::vector<LayoutTemplate> &out,
                                std::string *error) {
    out.clear();
    Owned root(parse(text, error));
    if (!root.v || root.v->type != DICT_OBJECT) {
        if (error && root.v)
            *error = "not a layouts file";
        return false;
    }
    const DictValue *list = field(root.v, "templates");
    for (size_t i = 0; i < count(list); ++i) {
        const DictValue *o = at(list, i);
        LayoutTemplate t;
        get(o, "name", t.name);
        get(o, "category", t.category);
        float size[2] = {0, 0};
        if (floats(o, "size", size, 2) != 2 || !(size[0] > 0.f) || !(size[1] > 0.f))
            continue;                    // no box: nothing to fit
        t.width = size[0];
        t.height = size[1];
        get(o, "spread", t.spread);
        double all = 0;
        const DictValue *m = field(o, "margins");
        if (number(m, all)) {
            t.margins = {(float) all, (float) all, (float) all, (float) all, (float) all};
        } else if (m) {
            get(m, "top", t.margins.top);
            get(m, "right", t.margins.right);
            get(m, "bottom", t.margins.bottom);
            get(m, "left", t.margins.left);
            t.margins.inside = t.margins.left;
            get(m, "inside", t.margins.inside);
        }
        const DictValue *els = field(o, "elements");
        for (size_t k = 0; k < count(els); ++k) {
            LayoutElement e;
            if (read_element(at(els, k), e))
                t.elements.push_back(std::move(e));
        }
        if (t.name.empty())
            t.name = "Layout " + std::to_string(out.size() + 1);
        out.push_back(std::move(t));
    }
    return true;
}

/* ---- The config file ------------------------------------------------------- */

std::string layout_templates_config_path() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"))
        if (xdg[0])
            return std::string(xdg) + "/pagemade/layouts.json";
    if (const char *home = std::getenv("HOME"))
        if (home[0])
            return std::string(home) + "/.config/pagemade/layouts.json";
    return "pagemade-layouts.json";
}

bool layout_templates_save(const std::vector<LayoutTemplate> &templates, const std::string &path,
                           std::string *error) {
    if (path.empty())
        return false;
    try {
        const std::string dir = nanogui::fs::parent_path(path);
        if (!dir.empty())
            nanogui::fs::create_directories(dir);
    } catch (...) {
    }
    /* Beside the target, then renamed over it: a failed write keeps the old file */
    const std::string tmp = path + ".saving";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error)
                *error = "can't write " + tmp;
            return false;
        }
        const std::string text = layout_templates_json(templates);
        out.write(text.data(), (std::streamsize) text.size());
        if (!out) {
            out.close();
            std::remove(tmp.c_str());
            if (error)
                *error = "can't write " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        if (error)
            *error = "can't replace " + path;
        return false;
    }
    return true;
}

std::vector<LayoutTemplate> layout_templates_load_or_seed(const std::string &path,
                                                          std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::vector<LayoutTemplate> seed = default_layout_templates();
        if (!nanogui::fs::exists(path))
            layout_templates_save(seed, path, error);
        return seed;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::vector<LayoutTemplate> out;
    std::string err;
    if (!layout_templates_from_json(ss.str(), out, &err)) {
        if (error)
            *error = path + ": " + err;
        return default_layout_templates();   // the broken file is left for the user to fix
    }
    return out;
}

/* ---- Fitting and placing ---------------------------------------------------- */

namespace {

struct Area {
    float x = 0, y = 0, w = 0, h = 0;
};

Area live_area(const PageSetup &s, size_t page) {
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    Area a;
    a.x = left;
    a.y = s.margin_top;
    a.w = s.width - left - right;
    a.h = s.height - s.margin_top - s.margin_bottom;
    if (!(a.w > 1.f) || !(a.h > 1.f))
        a = {0.f, 0.f, s.width, s.height};    // margins that leave nothing: the page
    return a;
}

/* A content area that is empty (margins wider than the box): the box. */
Area content(float x0, float x1, float y0, float y1, float bx0, float bx1, float h) {
    if (x1 - x0 > 1.f && y1 - y0 > 1.f)
        return {x0, y0, x1 - x0, y1 - y0};
    return {bx0, 0.f, bx1 - bx0, h};
}

float fit_scale(const Area &c, const Area &live) {
    return std::min(live.w / c.w, live.h / c.h);
}

LayoutFit::Part part_for(size_t page, const Area &c, const Area &live, float scale) {
    LayoutFit::Part p;
    p.page = page;
    p.scale = scale;
    p.dx = live.x + (live.w - c.w * scale) * 0.5f - c.x * scale;
    p.dy = live.y + (live.h - c.h * scale) * 0.5f - c.y * scale;
    return p;
}

float element_center_x(const LayoutElement &e) {
    return e.rect.x + e.rect.w * 0.5f;
}

} // namespace

LayoutFit fit_layout(const PageDoc &doc, const LayoutTemplate &t, size_t page) {
    LayoutFit fit;
    if (page >= doc.pages.size() || !(t.width > 0.f) || !(t.height > 0.f))
        return fit;
    const PageSetup &s = doc.setup;
    const LayoutMargins &m = t.margins;

    /* The spread around `page`: with facing pages, odd indices are left-hand */
    size_t left = SIZE_MAX, right = SIZE_MAX;
    if (t.spread && s.facing) {
        if (page % 2 == 1) {
            left = page;
            if (page + 1 < doc.pages.size())
                right = page + 1;
        } else if (page > 0) {
            left = page - 1;
            right = page;
        }
    }
    if (left != SIZE_MAX && right != SIZE_MAX) {
        const float half = t.width * 0.5f;
        const Area cl = content(m.left, half - m.inside, m.top, t.height - m.bottom, 0.f, half, t.height);
        const Area cr = content(half + m.inside, t.width - m.right, m.top, t.height - m.bottom,
                                half, t.width, t.height);
        const Area ll = live_area(s, left), lr = live_area(s, right);
        /* One scale for both pages, so the spread stays in proportion */
        const float k = std::min(fit_scale(cl, ll), fit_scale(cr, lr));
        LayoutFit::Part pl = part_for(left, cl, ll, k), pr = part_for(right, cr, lr, k);
        for (size_t i = 0; i < t.elements.size(); ++i)
            (element_center_x(t.elements[i]) < half ? pl : pr).elements.push_back(i);
        fit.parts.push_back(std::move(pl));
        fit.parts.push_back(std::move(pr));
        return fit;
    }
    const Area c = content(m.left, t.width - m.right, m.top, t.height - m.bottom, 0.f, t.width, t.height);
    const Area live = live_area(s, page);
    LayoutFit::Part p = part_for(page, c, live, fit_scale(c, live));
    for (size_t i = 0; i < t.elements.size(); ++i)
        p.elements.push_back(i);
    fit.parts.push_back(std::move(p));
    return fit;
}

LayoutRect layout_rect_on_page(const LayoutFit::Part &part, const LayoutRect &r) {
    return {r.x * part.scale + part.dx, r.y * part.scale + part.dy, r.w * part.scale,
            r.h * part.scale};
}

namespace {

SwatchId swatch_named(const PageDoc &doc, const std::string &name, SwatchId fallback) {
    if (name.empty())
        return kNoPaint;
    for (const Swatch &s : doc.swatches)
        if (s.name == name)
            return s.id;
    return fallback;
}

std::string swatch_name(const PageDoc &doc, SwatchId id) {
    if (const Swatch *s = doc.find_swatch(id))
        return s->name;
    return {};
}

/* Type scales with the layout, to a tenth of a point. Within a few
 * percent of its own size the template keeps its type as designed, so a
 * paragraph style isn't shown overridden for a rounding. */
float scaled(float v, float k) {
    return std::round(v * k * 10.f) / 10.f;
}

void apply_run(const PageDoc &doc, CharStyle &cs, const LayoutRun &r) {
    if (!r.family.empty()) {
        cs.family = r.family;
        cs.face.clear();
    }
    if (!r.face.empty()) cs.face = r.face;
    if (r.has_bold) cs.bold = r.bold;
    if (r.has_italic) cs.italic = r.italic;
    if (r.has_caps) cs.caps = r.caps;
    if (r.underline) cs.underline = true;
    if (r.size > 0.f) cs.size = r.size;
    if (r.leading >= 0.f) cs.leading = r.leading;
    if (r.has_tracking) cs.tracking = r.tracking;
    if (!r.swatch.empty()) {
        const SwatchId id = swatch_named(doc, r.swatch, 0);
        if (id) {
            cs.swatch = id;
            cs.tint = r.tint;
        }
    }
}

Story make_story(const PageDoc &doc, const std::vector<LayoutParagraph> &paras, float k) {
    const bool scale = std::fabs(k - 1.f) > 0.03f;
    Story story;
    for (const LayoutParagraph &lp : paras) {
        const StyleDef *def = doc.find_style(lp.style);
        if (!def)
            def = doc.find_style("Normal");
        Paragraph p;
        CharStyle base;
        if (def) {
            p.style = def->para;
            base = def->type;
        }
        if (lp.has_align) p.style.align = lp.align;
        if (lp.space_before >= 0.f) p.style.space_before = lp.space_before;
        if (lp.space_after >= 0.f) p.style.space_after = lp.space_after;
        apply_run(doc, base, lp.type);
        for (const LayoutRun &r : lp.runs) {
            Run run{base, r.text};
            apply_run(doc, run.style, r);
            p.runs.push_back(std::move(run));
        }
        if (p.runs.empty())
            p.runs.push_back(Run{base, ""});
        if (scale) {
            for (Run &r : p.runs) {
                r.style.size = std::max(4.f, scaled(r.style.size, k));
                if (r.style.leading > 0.f)
                    r.style.leading = std::max(4.f, scaled(r.style.leading, k));
                r.style.baseline_shift = scaled(r.style.baseline_shift, k);
            }
            ParaStyle &ps = p.style;
            for (float *v : {&ps.left_indent, &ps.right_indent, &ps.first_indent, &ps.last_indent,
                             &ps.space_before, &ps.space_after, &ps.extra_spacing, &ps.default_tab,
                             &ps.hyphen_zone})
                *v = scaled(*v, k);
            for (TabStop &tab : ps.tabs)
                tab.pos = scaled(tab.pos, k);
        }
        story.paragraphs.push_back(std::move(p));
    }
    if (story.paragraphs.empty()) {
        Paragraph p;
        p.runs.push_back(Run{CharStyle(), ""});
        story.paragraphs.push_back(std::move(p));
    }
    return story;
}

Stroke make_stroke(const PageDoc &doc, const LayoutElement &e, float k) {
    Stroke s;
    s.paint = {swatch_named(doc, e.stroke_swatch, kBlack), e.stroke_tint};   // none when unnamed
    s.weight = e.stroke_swatch.empty() ? 0.f : std::max(0.25f, e.stroke_weight * k);
    s.style = e.stroke_style;
    return s;
}

} // namespace

LayoutPlacement place_layout(PageDoc &doc, VectorStore *vectors, const LayoutTemplate &t,
                             size_t page) {
    LayoutPlacement out;
    const LayoutFit fit = fit_layout(doc, t, page);
    std::map<std::string, uint32_t> svg_assets;   // one asset for markup used twice
    for (const LayoutFit::Part &part : fit.parts) {
        std::vector<ItemId> ids;
        const float k = part.scale;
        for (size_t ei : part.elements) {
            const LayoutElement &e = t.elements[ei];
            const LayoutRect r = layout_rect_on_page(part, e.rect);
            Item it;
            it.w = r.w;
            it.h = r.h;
            it.xf = Transform::translate(r.x, r.y);
            switch (e.kind) {
            case LayoutElement::Kind::Photo: {
                if (!(r.w > 0.f) || !(r.h > 0.f))
                    continue;
                PlacedImage im;              // asset 0: an empty frame
                im.w = r.w;
                im.h = r.h;
                if (!e.stroke_swatch.empty())
                    im.stroke = make_stroke(doc, e, k);
                it.content = im;
                ids.push_back(doc.add_item(part.page, std::move(it)));
                break;
            }
            case LayoutElement::Kind::Shape: {
                Shape sh;
                sh.kind = e.shape;
                sh.corner_radius = e.corner_radius * k;
                sh.fill = {swatch_named(doc, e.fill_swatch, kBlack), e.fill_tint};
                sh.stroke = make_stroke(doc, e, k);
                if (sh.kind != Shape::Kind::Line && (!(r.w > 0.f) || !(r.h > 0.f)))
                    continue;
                it.content = sh;
                ids.push_back(doc.add_item(part.page, std::move(it)));
                break;
            }
            case LayoutElement::Kind::Text: {
                if (!(r.w > 0.f) || !(r.h > 0.f))
                    continue;
                const StoryId sid = doc.add_story(make_story(doc, e.paragraphs, k));
                ids.push_back(doc.add_text_frame(part.page, sid, r.w, r.h, it.xf));
                break;
            }
            case LayoutElement::Kind::Svg: {
                if (!vectors || !(r.w > 0.f) || !(r.h > 0.f)) {
                    out.skipped.push_back("an SVG graphic");
                    continue;
                }
                std::vector<uint8_t> bytes(e.svg.begin(), e.svg.end());
                VectorScene scene;
                std::string err;
                if (!vector_loader().decode(bytes.data(), bytes.size(), &scene, &err) ||
                    !(scene.width > 0.f) || !(scene.height > 0.f)) {
                    out.skipped.push_back("an SVG graphic (" + (err.empty() ? "unreadable" : err) + ")");
                    continue;
                }
                uint32_t asset = 0;
                auto have = svg_assets.find(e.svg);
                if (have != svg_assets.end()) {
                    asset = have->second;
                } else {
                    asset = vectors->take_id();
                    const float sw = scene.width, shh = scene.height;
                    const std::vector<std::string> skipped = scene.skipped;
                    if (!vectors->add(asset, bytes, std::move(scene))) {
                        out.skipped.push_back("an SVG graphic");
                        continue;
                    }
                    VectorAsset a;
                    a.id = asset;
                    a.name = t.name.empty() ? std::string("layout.svg") : t.name + ".svg";
                    a.width = sw;
                    a.height = shh;
                    a.skipped = skipped;
                    a.source_size = bytes.size();
                    a.hash = image_bytes_hash(bytes.data(), bytes.size());
                    doc.add_vector(a);
                    svg_assets[e.svg] = asset;
                }
                const VectorAsset *a = doc.find_vector(asset);
                if (!a)
                    continue;
                /* Its own proportions, centered in the rectangle */
                const float g = std::min(r.w / a->width, r.h / a->height);
                const float fw = a->width * g, fh = a->height * g;
                it.w = fw;
                it.h = fh;
                it.xf = Transform::translate(r.x + (r.w - fw) * 0.5f, r.y + (r.h - fh) * 0.5f);
                PlacedVector pv;
                pv.asset = asset;
                pv.w = fw;
                pv.h = fh;
                it.content = pv;
                ids.push_back(doc.add_item(part.page, std::move(it)));
                break;
            }
            }
        }
        if (ids.empty())
            continue;
        if (ids.size() >= 2)
            doc.group_items(ids);
        out.items.insert(out.items.end(), ids.begin(), ids.end());
        out.pages.push_back(part.page);
    }
    return out;
}

/* ---- A template from the page -------------------------------------------- */

namespace {

LayoutRun run_from(const PageDoc &doc, const CharStyle &cs, const std::string &text) {
    LayoutRun r;
    r.text = text;
    r.family = cs.family;
    r.face = cs.face;
    r.has_bold = r.has_italic = r.has_caps = r.has_tracking = true;
    r.bold = cs.bold;
    r.italic = cs.italic;
    r.caps = cs.caps;
    r.underline = cs.underline;
    r.size = cs.size;
    r.leading = cs.leading;
    r.tracking = cs.tracking;
    if (cs.swatch) {
        r.swatch = swatch_name(doc, cs.swatch);
        r.tint = cs.tint;
    }
    return r;
}

} // namespace

LayoutTemplate layout_from_items(const PageDoc &doc, const VectorStore *vectors, size_t page,
                                 const std::vector<ItemId> &ids, const std::string &name,
                                 const std::string &category) {
    LayoutTemplate t;
    t.name = name;
    t.category = category;
    if (page >= doc.pages.size())
        return t;
    const PageSetup &s = doc.setup;
    t.width = s.width;
    t.height = s.height;
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    t.margins = {s.margin_top, right, s.margin_bottom, left, left};
    std::vector<StoryId> stories;        // a story's text goes with its first block only
    for (const Item &it : doc.pages[page].items) {
        if (std::find(ids.begin(), ids.end(), it.id) == ids.end())
            continue;
        const Bounds b = item_bounds(it);
        if (b.empty())
            continue;
        LayoutElement e;
        e.rect = {b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0};
        auto stroke_of = [&](const Stroke &st) {
            if (st.none())
                return;
            e.stroke_swatch = swatch_name(doc, st.paint.swatch);
            e.stroke_tint = st.paint.tint;
            e.stroke_weight = st.weight;
            e.stroke_style = st.style;
        };
        if (const PlacedImage *im = it.image()) {
            e.kind = LayoutElement::Kind::Photo;
            stroke_of(im->stroke);
        } else if (const Shape *sh = it.shape()) {
            e.kind = LayoutElement::Kind::Shape;
            e.shape = sh->kind == Shape::Kind::Polygon ? Shape::Kind::Rect : sh->kind;
            e.corner_radius = sh->corner_radius;
            if (!sh->fill.none()) {
                e.fill_swatch = swatch_name(doc, sh->fill.swatch);
                e.fill_tint = sh->fill.tint;
            }
            stroke_of(sh->stroke);
            if (sh->kind == Shape::Kind::Line) {
                /* The line itself, end to end, not its bounds */
                const Point a = it.xf.apply({0.f, 0.f}), z = it.xf.apply({it.w, it.h});
                e.rect = {a.x, a.y, z.x - a.x, z.y - a.y};
            }
        } else if (const PlacedVector *pv = it.vector()) {
            const VectorStore::Bytes bytes = vectors ? vectors->source_of(pv->asset) : nullptr;
            if (!bytes || bytes->empty())
                continue;
            e.kind = LayoutElement::Kind::Svg;
            e.svg.assign(bytes->begin(), bytes->end());
        } else if (it.is_text()) {
            e.kind = LayoutElement::Kind::Text;
            const StoryEntry *se = doc.story_of(it.id);
            if (se && std::find(stories.begin(), stories.end(), se->id) == stories.end()) {
                stories.push_back(se->id);
                for (const Paragraph &p : se->story.paragraphs) {
                    LayoutParagraph lp;
                    lp.style = p.style.name.empty() ? std::string("Normal") : p.style.name;
                    lp.has_align = true;
                    lp.align = p.style.align;
                    lp.space_before = p.style.space_before;
                    lp.space_after = p.style.space_after;
                    for (const Run &r : p.runs)
                        lp.runs.push_back(run_from(doc, r.style, r.text));
                    e.paragraphs.push_back(std::move(lp));
                }
            }
            if (e.paragraphs.empty())
                e.paragraphs.emplace_back();
        } else {
            continue;
        }
        t.elements.push_back(std::move(e));
    }
    return t;
}

/* ---- The built-in set ------------------------------------------------------- */

namespace {

constexpr float kGap = 12.f;

/* A paragraph to build in one expression: P("Caption", "Text").center() */
struct P {
    LayoutParagraph p;
    P(const char *style, const char *text) {
        p.style = style;
        LayoutRun r;
        r.text = text;
        p.runs.push_back(std::move(r));
    }
    P &align(Align a) { p.has_align = true; p.align = a; return *this; }
    P &center() { return align(Align::Center); }
    P &right() { return align(Align::Right); }
    P &size(float s, float leading = -1.f) { p.type.size = s; p.type.leading = leading; return *this; }
    P &family(const char *f) { p.type.family = f; return *this; }
    P &italic(bool on = true) { p.type.has_italic = true; p.type.italic = on; return *this; }
    P &bold(bool on = true) { p.type.has_bold = true; p.type.bold = on; return *this; }
    /* Spaced capitals, the way the sample spreads set their titles */
    P &spaced(float tracking = 300.f) {
        p.type.has_caps = true;
        p.type.caps = Caps::All;
        p.type.has_tracking = true;
        p.type.tracking = tracking;
        return *this;
    }
    P &after(float pts) { p.space_after = pts; return *this; }
    P &before(float pts) { p.space_before = pts; return *this; }
    operator LayoutParagraph() const { return p; }
};

/* SVG ornaments. Width and height are given so the graphic has a size. */
const char *kDivider =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"20\" viewBox=\"0 0 200 20\">"
    "<path d=\"M0 10H86M114 10H200\" fill=\"none\" stroke=\"#555555\" stroke-width=\"1\"/>"
    "<path d=\"M100 3L107 10L100 17L93 10Z\" fill=\"#555555\"/></svg>";
const char *kCornerTL =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 20 20\">"
    "<path d=\"M1 20V1H20\" fill=\"none\" stroke=\"#444444\" stroke-width=\"1.5\"/></svg>";
const char *kCornerTR =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 20 20\">"
    "<path d=\"M0 1H19V20\" fill=\"none\" stroke=\"#444444\" stroke-width=\"1.5\"/></svg>";
const char *kCornerBL =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 20 20\">"
    "<path d=\"M1 0V19H20\" fill=\"none\" stroke=\"#444444\" stroke-width=\"1.5\"/></svg>";
const char *kCornerBR =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\" viewBox=\"0 0 20 20\">"
    "<path d=\"M0 19H19V0\" fill=\"none\" stroke=\"#444444\" stroke-width=\"1.5\"/></svg>";
const char *kSprig =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"60\" height=\"24\" viewBox=\"0 0 60 24\">"
    "<path d=\"M2 12C20 12 40 12 58 12\" fill=\"none\" stroke=\"#666666\" stroke-width=\"1\"/>"
    "<path d=\"M30 12C26 4 18 3 14 6C18 10 24 12 30 12Z\" fill=\"#666666\"/>"
    "<path d=\"M30 12C34 20 42 21 46 18C42 14 36 12 30 12Z\" fill=\"#666666\"/></svg>";

class Builder {
public:
    Builder(std::vector<LayoutTemplate> &out, const char *name, const char *category,
            bool spread = false)
        : m_out(out) {
        m_t.name = name;
        m_t.category = category;
        m_t.spread = spread;
        m_t.width = spread ? 1224.f : 612.f;
        m_t.height = 792.f;
    }
    ~Builder() { m_out.push_back(std::move(m_t)); }

    /* The content area of the page (0: the page, or a spread's left; 1: right) */
    LayoutRect area(int side = 0) const {
        const LayoutMargins &m = m_t.margins;
        if (!m_t.spread)
            return {m.left, m.top, m_t.width - m.left - m.right, m_t.height - m.top - m.bottom};
        const float half = m_t.width * 0.5f;
        if (side == 0)
            return {m.left, m.top, half - m.left - m.inside, m_t.height - m.top - m.bottom};
        return {half + m.inside, m.top, half - m.inside - m.right, m_t.height - m.top - m.bottom};
    }
    /* Cells of a cols x rows grid over `r`, kGap apart */
    static LayoutRect cell(const LayoutRect &r, int cols, int rows, int c, int row,
                           int cs = 1, int rs = 1) {
        const float cw = (r.w - kGap * (float) (cols - 1)) / (float) cols;
        const float ch = (r.h - kGap * (float) (rows - 1)) / (float) rows;
        return {r.x + (float) c * (cw + kGap), r.y + (float) row * (ch + kGap),
                cw * (float) cs + kGap * (float) (cs - 1), ch * (float) rs + kGap * (float) (rs - 1)};
    }
    /* Every cell of a grid */
    void grid(const LayoutRect &r, int cols, int rows) {
        for (int row = 0; row < rows; ++row)
            for (int c = 0; c < cols; ++c)
                photo(cell(r, cols, rows, c, row));
    }

    LayoutElement &photo(LayoutRect r) {
        LayoutElement e;
        e.kind = LayoutElement::Kind::Photo;
        e.rect = r;
        return add(std::move(e));
    }
    LayoutElement &text(LayoutRect r, std::initializer_list<LayoutParagraph> paras) {
        LayoutElement e;
        e.kind = LayoutElement::Kind::Text;
        e.rect = r;
        e.paragraphs = paras;
        return add(std::move(e));
    }
    LayoutElement &svg(LayoutRect r, const char *markup) {
        LayoutElement e;
        e.kind = LayoutElement::Kind::Svg;
        e.rect = r;
        e.svg = markup;
        return add(std::move(e));
    }
    /* A hairline rule from (x0, y) to (x1, y) */
    LayoutElement &rule(float x0, float y, float x1, float weight = 0.5f) {
        LayoutElement e;
        e.kind = LayoutElement::Kind::Shape;
        e.shape = Shape::Kind::Line;
        e.rect = {x0, y, x1 - x0, 0.f};
        e.stroke_swatch = "[Black]";
        e.stroke_weight = weight;
        return add(std::move(e));
    }
    /* An outlined box: a mat around a picture */
    LayoutElement &frame(LayoutRect r, float weight = 0.5f) {
        LayoutElement e;
        e.kind = LayoutElement::Kind::Shape;
        e.shape = Shape::Kind::Rect;
        e.rect = r;
        e.stroke_swatch = "[Black]";
        e.stroke_weight = weight;
        return add(std::move(e));
    }
    /* Four corner brackets just inside `r` */
    void brackets(LayoutRect r, float size = 20.f) {
        svg({r.x, r.y, size, size}, kCornerTL);
        svg({r.x + r.w - size, r.y, size, size}, kCornerTR);
        svg({r.x, r.y + r.h - size, size, size}, kCornerBL);
        svg({r.x + r.w - size, r.y + r.h - size, size, size}, kCornerBR);
    }

private:
    LayoutElement &add(LayoutElement e) {
        m_t.elements.push_back(std::move(e));
        return m_t.elements.back();
    }
    std::vector<LayoutTemplate> &m_out;
    LayoutTemplate m_t;
};

const char *kBody =
    "Tell the story behind these pictures: where you were, who was there, and what made "
    "the day worth remembering. Replace this text with your own words.";
const char *kCaption = "A few words about this moment";

} // namespace

std::vector<LayoutTemplate> default_layout_templates() {
    std::vector<LayoutTemplate> out;
    /* Every page is 8.5 x 11 in with half-inch margins, the publication
     * default; the content area is 504 x 684 points. */

    /* ---- One photo ---- */
    { Builder b(out, "Full Page", "1 Photo");
      b.photo(b.area()); }
    { Builder b(out, "Photo and Caption", "1 Photo");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 1, 12, 0, 0, 1, 11));
      b.text(Builder::cell(a, 1, 12, 0, 11), {P("Caption", kCaption).center()}); }
    { Builder b(out, "Square and Title", "1 Photo");
      b.photo({90, 90, 432, 432});
      b.svg({231, 552, 150, 15}, kDivider);
      b.text({54, 584, 504, 48}, {P("Headline", "Your Title Here").size(30, 34)});
      b.text({54, 640, 504, 30}, {P("Caption", "Summer 2026").center().spaced().italic(false)}); }
    { Builder b(out, "Matted Portrait", "1 Photo");
      b.frame({90, 72, 432, 612});
      b.photo({126, 108, 360, 480});
      b.text({126, 606, 360, 48}, {P("Caption", kCaption).center().size(10, 13)}); }

    /* ---- Two photos ---- */
    { Builder b(out, "Top Heavy", "2 Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 1, 3, 0, 0, 1, 2));
      b.photo(Builder::cell(a, 1, 3, 0, 2)); }
    { Builder b(out, "Two Stacked", "2 Photos");
      b.grid(b.area(), 1, 2); }
    { Builder b(out, "Side by Side", "2 Photos");
      const LayoutRect band{54, 54, 504, 624};
      b.photo(Builder::cell(band, 2, 1, 0, 0));
      b.photo(Builder::cell(band, 2, 1, 1, 0));
      b.text({54, 690, 504, 48}, {P("Caption", kCaption).center()}); }
    { Builder b(out, "Offset Pair", "2 Photos");
      b.photo({54, 108, 300, 420});
      b.photo({366, 228, 192, 276});
      b.text({366, 516, 192, 72}, {P("Caption", kCaption).center().spaced(150).italic(false).size(7.5f, 11)}); }

    /* ---- Three photos ---- */
    { Builder b(out, "Hero and Two", "3 Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 2, 5, 0, 0, 2, 3));
      b.photo(Builder::cell(a, 2, 5, 0, 3, 1, 2));
      b.photo(Builder::cell(a, 2, 5, 1, 3, 1, 2)); }
    { Builder b(out, "Column and Two", "3 Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 2, 2, 0, 0, 1, 2));
      b.photo(Builder::cell(a, 2, 2, 1, 0));
      b.photo(Builder::cell(a, 2, 2, 1, 1)); }
    { Builder b(out, "Triptych", "3 Photos");
      const LayoutRect band{54, 216, 504, 300};
      b.grid(band, 3, 1);
      b.text({54, 534, 504, 30}, {P("Caption", "A day to remember").center().spaced().italic(false)}); }
    { Builder b(out, "Three Bands", "3 Photos");
      b.grid(b.area(), 1, 3); }

    /* ---- Four photos ---- */
    { Builder b(out, "Grid 2 × 2", "4 Photos");
      b.grid(b.area(), 2, 2); }
    { Builder b(out, "Hero and Three", "4 Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 3, 3, 0, 0, 3, 2));
      for (int i = 0; i < 3; ++i)
          b.photo(Builder::cell(a, 3, 3, i, 2)); }
    { Builder b(out, "Feature and Strip", "4 Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 3, 3, 0, 0, 2, 3));
      for (int i = 0; i < 3; ++i)
          b.photo(Builder::cell(a, 3, 3, 2, i)); }
    { Builder b(out, "Tall and Three", "4 Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 2, 4, 0, 0, 1, 4));
      b.photo(Builder::cell(a, 2, 4, 1, 0, 1, 2));
      b.photo(Builder::cell(a, 2, 4, 1, 2));
      b.photo(Builder::cell(a, 2, 4, 1, 3)); }

    /* ---- Five and more ---- */
    { Builder b(out, "Big Middle", "5+ Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 2, 4, 0, 0));
      b.photo(Builder::cell(a, 2, 4, 1, 0));
      b.photo(Builder::cell(a, 2, 4, 0, 1, 2, 2));
      b.photo(Builder::cell(a, 2, 4, 0, 3));
      b.photo(Builder::cell(a, 2, 4, 1, 3)); }
    { Builder b(out, "Mosaic Five", "5+ Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 3, 4, 0, 0, 3, 2));
      b.photo(Builder::cell(a, 3, 4, 0, 2, 2, 1));
      b.photo(Builder::cell(a, 3, 4, 2, 2));
      b.photo(Builder::cell(a, 3, 4, 0, 3));
      b.photo(Builder::cell(a, 3, 4, 1, 3, 2, 1)); }
    { Builder b(out, "Gallery Wall", "5+ Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 4, 2, 0, 0));
      b.photo(Builder::cell(a, 4, 2, 0, 1));
      b.photo(Builder::cell(a, 4, 2, 1, 0, 2, 2));
      b.photo(Builder::cell(a, 4, 2, 3, 0));
      b.photo(Builder::cell(a, 4, 2, 3, 1)); }
    { Builder b(out, "Grid 2 × 3", "5+ Photos");
      b.grid(b.area(), 2, 3); }
    { Builder b(out, "Staggered Six", "5+ Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 2, 4, 0, 0));
      b.photo(Builder::cell(a, 2, 4, 0, 1, 1, 2));
      b.photo(Builder::cell(a, 2, 4, 0, 3));
      b.photo(Builder::cell(a, 2, 4, 1, 0, 1, 2));
      b.photo(Builder::cell(a, 2, 4, 1, 2));
      b.photo(Builder::cell(a, 2, 4, 1, 3)); }
    { Builder b(out, "Center Stage", "5+ Photos");
      const LayoutRect a = b.area();
      b.photo(Builder::cell(a, 4, 4, 0, 0, 2, 1));
      b.photo(Builder::cell(a, 4, 4, 2, 0, 2, 1));
      b.photo(Builder::cell(a, 4, 4, 0, 1));
      b.photo(Builder::cell(a, 4, 4, 0, 2));
      b.photo(Builder::cell(a, 4, 4, 1, 1, 2, 2));
      b.photo(Builder::cell(a, 4, 4, 3, 1));
      b.photo(Builder::cell(a, 4, 4, 3, 2));
      b.photo(Builder::cell(a, 4, 4, 0, 3, 2, 1));
      b.photo(Builder::cell(a, 4, 4, 2, 3, 2, 1)); }

    /* ---- Words and pictures ---- */
    { Builder b(out, "Title Page", "Text & Photos");
      b.text({54, 72, 504, 48}, {P("Headline", "Your Title Here")});
      b.svg({231, 128, 150, 15}, kDivider);
      b.text({54, 152, 504, 30}, {P("Caption", "Subtitle or date").center().spaced(200).italic(false)});
      b.photo({54, 198, 504, 540}); }
    { Builder b(out, "Story Page", "Text & Photos");
      b.photo({54, 54, 504, 336});
      b.text({54, 402, 504, 336}, {P("Subhead 1", "A Day to Remember"),
                                   P("Body first", kBody),
                                   P("Body text", kBody)}); }
    { Builder b(out, "Quote Page", "Text & Photos");
      b.photo({54, 54, 504, 408});
      const LayoutRect q{126, 498, 360, 180};
      b.brackets(q);
      b.text({150, 528, 312, 120}, {P("Normal", "Some moments are worth keeping forever.")
                                        .center().family("Serif").italic().size(18, 24).after(10),
                                    P("Caption", "Your name").center().spaced(200).italic(false)}); }
    { Builder b(out, "Photo and Column", "Text & Photos");
      b.photo({54, 54, 336, 684});
      b.text({402, 54, 156, 684}, {P("Subhead 2", "The Journey"),
                                   P("Body first", kBody).align(Align::Left),
                                   P("Caption", kCaption).before(12)});
      b.rule(402, 738, 558); }

    /* ---- Spreads (two facing pages) ---- */
    { Builder b(out, "Spread: Photo, Words, Photo", "Spreads", true);
      const LayoutRect l = b.area(0);
      b.photo({l.x, l.y, 360, l.h});
      b.text({l.x + 372, l.y, 132, 300}, {P("Caption", kBody).italic(false).size(7.5f, 10)});
      b.photo(b.area(1)); }
    { Builder b(out, "Spread: Four Across", "Spreads", true);
      const LayoutRect l = b.area(0), r = b.area(1);
      const LayoutRect lb{l.x, 234, l.w, 252}, rb{r.x, 234, r.w, 252};
      b.grid(lb, 2, 1);
      b.grid(rb, 2, 1);
      b.text({l.x, 498, l.w, 40}, {P("Caption", kCaption).italic(false).bold()}); }
    { Builder b(out, "Spread: Collage and Story", "Spreads", true);
      const LayoutRect l = b.area(0), r = b.area(1);
      b.photo(Builder::cell(l, 2, 2, 0, 0));
      b.photo(Builder::cell(l, 2, 2, 0, 1));
      b.photo(Builder::cell(l, 2, 2, 1, 0, 1, 2));
      b.photo({r.x, r.y, r.w, 420});
      b.text({r.x, r.y + 432, r.w, 252}, {P("Subhead 1", "Our Story"), P("Body first", kBody)}); }
    { Builder b(out, "Spread: Portrait and Pair", "Spreads", true);
      b.photo(b.area(0));
      const LayoutRect r = b.area(1);
      const LayoutRect band{r.x + 60, 216, r.w - 120, 264};
      b.grid(band, 2, 1);
      b.text({band.x, 492, band.w, 72}, {P("Caption", kCaption).center().spaced(150).italic(false)}); }
    { Builder b(out, "Spread: Panorama", "Spreads", true);
      const LayoutRect band{54, 246, 1116, 288};
      b.grid(band, 3, 1);
      b.text({54, 552, 1116, 30}, {P("Caption", "Come as you are").center().spaced(400).italic(false)}); }
    { Builder b(out, "Spread: Quote", "Spreads", true);
      b.photo({54, 54, 360, 684});
      const LayoutRect r = b.area(1);
      const LayoutRect q{r.x + 96, 276, r.w - 192, 240};
      b.brackets(q);
      b.text({q.x + 24, q.y + 40, q.w - 48, q.h - 80},
             {P("Normal", "You smile, I smile.\nLike love, sweet happiness.").center()
                  .family("Sans").spaced(120).size(9, 16)});
      b.svg({q.x + q.w * 0.5f - 30, q.y + q.h + 18, 60, 24}, kSprig); }

    return out;
}

} // namespace pagemade
