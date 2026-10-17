/*
 * pagemade/plugin/svg.cpp — the SVG decoder.
 *
 * Linked into pagemade, not loaded from disk. The first slice draws solid
 * fills and strokes: a gradient is painted as its middle stop, a clip is
 * omitted and the shape is drawn whole, and text is skipped. Each of those
 * is named once on the scene so Place can say what it left out. Path arcs
 * become cubics. Coordinates are baked into the graphic's point box
 * (96 CSS px = 72 pt), y down.
 */
#include "plugin/xml.h"
#include "vector.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <string>

namespace pagemade {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kKappa = 0.5522847498f;
constexpr float kPxToPt = 72.f / 96.f;

struct Grad {
    bool radial = false;
    std::vector<GradientStop> stops;
};

struct Decl {
    std::string prop, value;
    int spec = 0, order = 0;
};

struct Rule {
    std::string element;
    std::vector<std::string> classes;
    int spec = 0, order = 0;
    std::vector<std::pair<std::string, std::string>> props;
};

struct Viewport {
    float vb_x = 0, vb_y = 0, vb_w = 1, vb_h = 1;
    float vp_w_px = 1, vp_h_px = 1;
    float box_w = 1, box_h = 1;
    Transform map;
};

struct Style {
    VectorPaint fill;
    float fill_opacity = 1.f;
    FillRule rule = FillRule::NonZero;
    VectorPaint stroke;
    float stroke_opacity = 1.f;
    float stroke_width = 1.f;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    float miter = 4.f;
    std::vector<float> dash;
    float dash_offset = 0;
    float opacity = 1.f;
    bool display = true;
    bool visible = true;
    Color current{0, 0, 0, 1};
    float font_px = 16.f;
    std::string transform;
    std::string clip;
    Style() {
        fill.kind = PaintKind::Solid;
        fill.color = {0, 0, 0, 1};
    }
};

void note_skip(VectorScene &scene, const char *token) {
    for (const std::string &s : scene.skipped)
        if (s == token)
            return;
    scene.skipped.emplace_back(token);
}

std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char) s[a]))
        ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char) s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char &c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

bool parse_number(const std::string &s, size_t &i, float &out) {
    while (i < s.size() && (std::isspace((unsigned char) s[i]) || s[i] == ','))
        ++i;
    if (i >= s.size())
        return false;
    const char *b = s.c_str() + i;
    char *e = nullptr;
    out = std::strtof(b, &e);
    if (e == b)
        return false;
    i = (size_t) (e - s.c_str());
    return true;
}

enum class Unit { None, Px, Pt, Pc, In, Mm, Cm, Em, Percent };

struct Len {
    float n = 0;
    Unit unit = Unit::None;
    bool ok = false;
};

Len parse_len(const std::string &raw) {
    Len L;
    const std::string s = trim(raw);
    if (s.empty())
        return L;
    size_t i = 0;
    if (!parse_number(s, i, L.n))
        return L;
    const std::string u = lower(trim(s.substr(i)));
    if (u.empty())
        L.unit = Unit::None;
    else if (u == "px")
        L.unit = Unit::Px;
    else if (u == "pt")
        L.unit = Unit::Pt;
    else if (u == "pc")
        L.unit = Unit::Pc;
    else if (u == "in")
        L.unit = Unit::In;
    else if (u == "mm")
        L.unit = Unit::Mm;
    else if (u == "cm")
        L.unit = Unit::Cm;
    else if (u == "em")
        L.unit = Unit::Em;
    else if (u == "%")
        L.unit = Unit::Percent;
    else
        return L;
    L.ok = true;
    return L;
}

/* A CSS length as pixels. A bare number is a pixel, which is what the
 * root width and height are (the viewport), not a user coordinate. */
bool length_px(const std::string &raw, float font_px, float &px, bool &percent, float &pct) {
    const Len L = parse_len(raw);
    if (!L.ok)
        return false;
    percent = L.unit == Unit::Percent;
    pct = L.n;
    switch (L.unit) {
    case Unit::None:
    case Unit::Px: px = L.n; break;
    case Unit::Pt: px = L.n * 96.f / 72.f; break;
    case Unit::Pc: px = L.n * 16.f; break;
    case Unit::In: px = L.n * 96.f; break;
    case Unit::Mm: px = L.n * 96.f / 25.4f; break;
    case Unit::Cm: px = L.n * 96.f / 2.54f; break;
    case Unit::Em: px = L.n * font_px; break;
    case Unit::Percent: px = 0; break;
    }
    return true;
}

/* A length inside a viewport, in that viewport's user units. A bare
 * number is already a user unit; px and the absolute units are converted
 * through the viewport (1px == one user unit when the view box matches
 * the width in px). */
float length_user(const std::string &raw, float fallback, bool horiz, const Viewport &vp,
                  float font_px) {
    if (trim(raw).empty())
        return fallback;
    const Len L = parse_len(raw);
    if (!L.ok)
        return fallback;
    if (L.unit == Unit::None)
        return L.n;
    float px = 0, pct = 0;
    bool percent = false;
    if (!length_px(raw, font_px, px, percent, pct))
        return fallback;
    const float vp_px = horiz ? vp.vp_w_px : vp.vp_h_px;
    const float vb = horiz ? vp.vb_w : vp.vb_h;
    if (percent)
        px = pct / 100.f * vp_px;
    const float user_per_px = vp_px != 0.f ? vb / vp_px : 1.f;
    return px * user_per_px;
}

int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool parse_color(const std::string &raw, const Color &current, Color &out, bool &none) {
    const std::string s = lower(trim(raw));
    none = false;
    if (s.empty())
        return false;
    if (s == "none") {
        none = true;
        return true;
    }
    if (s == "transparent") {
        out = {0, 0, 0, 0};
        return true;
    }
    if (s == "currentcolor") {
        out = current;
        return true;
    }
    auto named = [&](const char *n, float r, float g, float b) {
        if (s != n)
            return false;
        out = {r, g, b, 1};
        return true;
    };
    if (named("black", 0, 0, 0) || named("white", 1, 1, 1) || named("red", 1, 0, 0) ||
        named("green", 0, 0.5f, 0) || named("lime", 0, 1, 0) || named("blue", 0, 0, 1) ||
        named("yellow", 1, 1, 0) || named("cyan", 0, 1, 1) || named("aqua", 0, 1, 1) ||
        named("magenta", 1, 0, 1) || named("fuchsia", 1, 0, 1) || named("gray", 0.5f, 0.5f, 0.5f) ||
        named("grey", 0.5f, 0.5f, 0.5f) || named("orange", 1, 0.647f, 0) ||
        named("purple", 0.5f, 0, 0.5f) || named("navy", 0, 0, 0.5f) || named("teal", 0, 0.5f, 0.5f) ||
        named("silver", 0.75f, 0.75f, 0.75f) || named("maroon", 0.5f, 0, 0) ||
        named("olive", 0.5f, 0.5f, 0))
        return true;
    if (!s.empty() && s[0] == '#') {
        const std::string h = s.substr(1);
        int r = 0, g = 0, b = 0, a = 255;
        if (h.size() == 3 || h.size() == 4) {
            int v[4];
            for (size_t i = 0; i < h.size(); ++i) {
                v[i] = hex_nibble(h[i]);
                if (v[i] < 0)
                    return false;
                v[i] = v[i] * 16 + v[i];
            }
            r = v[0];
            g = v[1];
            b = v[2];
            if (h.size() == 4)
                a = v[3];
        } else if (h.size() == 6 || h.size() == 8) {
            int v[4] = {0, 0, 0, 255};
            for (size_t i = 0; i < h.size() / 2; ++i) {
                const int hi = hex_nibble(h[i * 2]), lo = hex_nibble(h[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                    return false;
                v[i] = hi * 16 + lo;
            }
            r = v[0];
            g = v[1];
            b = v[2];
            a = v[3];
        } else
            return false;
        out = {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
        return true;
    }
    if (s.compare(0, 3, "rgb") == 0) {
        const size_t p = s.find('(');
        if (p == std::string::npos)
            return false;
        float n[4] = {0, 0, 0, 1};
        size_t i = p + 1;
        int got = 0;
        while (got < 4 && i < s.size()) {
            while (i < s.size() && (std::isspace((unsigned char) s[i]) || s[i] == ','))
                ++i;
            if (i >= s.size() || s[i] == ')')
                break;
            char *e = nullptr;
            n[got] = std::strtof(s.c_str() + i, &e);
            if (e == s.c_str() + i)
                return false;
            i = (size_t) (e - s.c_str());
            if (i < s.size() && s[i] == '%') {
                n[got] /= 100.f;
                ++i;
            } else if (got < 3)
                n[got] /= 255.f;
            ++got;
        }
        if (got < 3)
            return false;
        out = {n[0], n[1], n[2], n[3]};
        return true;
    }
    return false;
}

bool parse_url_id(const std::string &raw, std::string &id) {
    const std::string s = trim(raw);
    const size_t u = lower(s).find("url(");
    if (u == std::string::npos)
        return false;
    size_t i = u + 4;
    while (i < s.size() && std::isspace((unsigned char) s[i]))
        ++i;
    if (i < s.size() && (s[i] == '\'' || s[i] == '"'))
        ++i;
    if (i < s.size() && s[i] == '#')
        ++i;
    const size_t b = i;
    while (i < s.size() && s[i] != ')' && s[i] != '\'' && s[i] != '"' &&
           !std::isspace((unsigned char) s[i]))
        ++i;
    id = s.substr(b, i - b);
    return !id.empty();
}

Transform viewbox_map(float minx, float miny, float vw, float vh, float bw, float bh,
                      const std::string &par) {
    if (!(vw > 0.f) || !(vh > 0.f) || !(bw > 0.f) || !(bh > 0.f))
        return {};
    const std::string p = lower(par);
    const float sx = bw / vw, sy = bh / vh;
    if (p.find("none") != std::string::npos)
        return Transform::translate(-minx * sx, -miny * sy) * Transform::scale(sx, sy);
    const bool slice = p.find("slice") != std::string::npos;
    const float s = slice ? std::max(sx, sy) : std::min(sx, sy);
    float ax = 0.5f, ay = 0.5f;
    if (p.find("xmin") != std::string::npos)
        ax = 0;
    else if (p.find("xmax") != std::string::npos)
        ax = 1;
    if (p.find("ymin") != std::string::npos)
        ay = 0;
    else if (p.find("ymax") != std::string::npos)
        ay = 1;
    const float tx = (bw - vw * s) * ax - minx * s;
    const float ty = (bh - vh * s) * ay - miny * s;
    return Transform::translate(tx, ty) * Transform::scale(s, s);
}

bool parse_viewbox(const std::string &s, float &x, float &y, float &w, float &h) {
    size_t i = 0;
    return parse_number(s, i, x) && parse_number(s, i, y) && parse_number(s, i, w) &&
           parse_number(s, i, h) && w > 0.f && h > 0.f;
}

Viewport make_viewport(const XmlNode &svg, const Viewport *parent) {
    Viewport vp;
    const float font_px = 16.f;
    float w_px = 0, h_px = 0, w_pct = 0, h_pct = 0;
    bool w_is_pct = false, h_is_pct = false;
    const bool has_w = length_px(xml_attr_value(svg, "width"), font_px, w_px, w_is_pct, w_pct);
    const bool has_h = length_px(xml_attr_value(svg, "height"), font_px, h_px, h_is_pct, h_pct);
    if (w_is_pct)
        w_px = parent ? w_pct / 100.f * parent->vp_w_px : 300.f * w_pct / 100.f;
    if (h_is_pct)
        h_px = parent ? h_pct / 100.f * parent->vp_h_px : 150.f * h_pct / 100.f;
    float vx = 0, vy = 0, vw = 0, vh = 0;
    const bool has_vb = parse_viewbox(xml_attr_value(svg, "viewBox"), vx, vy, vw, vh);
    if (!has_w && !has_h) {
        w_px = has_vb ? vw : 300.f;
        h_px = has_vb ? vh : 150.f;
    } else if (has_w && !has_h)
        h_px = has_vb && vw > 0.f ? w_px * vh / vw : w_px;
    else if (!has_w && has_h)
        w_px = has_vb && vh > 0.f ? h_px * vw / vh : h_px;
    if (!has_vb) {
        vx = 0;
        vy = 0;
        vw = w_px > 0.f ? w_px : 1.f;
        vh = h_px > 0.f ? h_px : 1.f;
    }
    vp.vb_x = vx;
    vp.vb_y = vy;
    vp.vb_w = vw;
    vp.vb_h = vh;
    vp.vp_w_px = w_px > 0.f ? w_px : 1.f;
    vp.vp_h_px = h_px > 0.f ? h_px : 1.f;
    vp.box_w = vp.vp_w_px * kPxToPt;
    vp.box_h = vp.vp_h_px * kPxToPt;
    vp.map = viewbox_map(vx, vy, vw, vh, vp.box_w, vp.box_h, xml_attr_value(svg, "preserveAspectRatio"));
    return vp;
}

Transform parse_transform(const std::string &s) {
    Transform m;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (std::isspace((unsigned char) s[i]) || s[i] == ','))
            ++i;
        if (i >= s.size())
            break;
        const size_t b = i;
        while (i < s.size() && std::isalpha((unsigned char) s[i]))
            ++i;
        const std::string name = s.substr(b, i - b);
        while (i < s.size() && std::isspace((unsigned char) s[i]))
            ++i;
        if (i >= s.size() || s[i] != '(')
            break;
        ++i;
        float a[6] = {};
        int n = 0;
        while (n < 6 && parse_number(s, i, a[n]))
            ++n;
        while (i < s.size() && s[i] != ')')
            ++i;
        if (i < s.size() && s[i] == ')')
            ++i;
        Transform t;
        if (name == "matrix" && n >= 6)
            t = {a[0], a[1], a[2], a[3], a[4], a[5]};
        else if (name == "translate" && n >= 1)
            t = Transform::translate(a[0], n >= 2 ? a[1] : 0.f);
        else if (name == "scale" && n >= 1)
            t = Transform::scale(a[0], n >= 2 ? a[1] : a[0]);
        else if (name == "rotate" && n >= 1) {
            const float rad = a[0] * kPi / 180.f;
            t = Transform::rotate(rad);
            if (n >= 3)
                t = Transform::translate(a[1], a[2]) * t * Transform::translate(-a[1], -a[2]);
        } else if (name == "skewX" && n >= 1)
            t = {1, 0, std::tan(a[0] * kPi / 180.f), 1, 0, 0};
        else if (name == "skewY" && n >= 1)
            t = {1, std::tan(a[0] * kPi / 180.f), 0, 1, 0, 0};
        else
            continue;
        /* Left to right: the rightmost function hits the point first. */
        m = m * t;
    }
    return m;
}

PathCmd cmd_move(float x, float y) { return {PathCmd::Move, 0, 0, 0, 0, x, y}; }
PathCmd cmd_line(float x, float y) { return {PathCmd::Line, 0, 0, 0, 0, x, y}; }
PathCmd cmd_cubic(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    return {PathCmd::Cubic, c1x, c1y, c2x, c2y, x, y};
}
PathCmd cmd_close() { return {PathCmd::Close, 0, 0, 0, 0, 0, 0}; }

void arc_to_cubics(Path &path, float x0, float y0, float rx, float ry, float phi_deg, bool large,
                   bool sweep, float x, float y) {
    if (!(std::fabs(rx) > 1e-6f) || !(std::fabs(ry) > 1e-6f)) {
        path.push_back(cmd_line(x, y));
        return;
    }
    if (std::hypot(x - x0, y - y0) < 1e-6f)
        return;
    const float phi = phi_deg * kPi / 180.f;
    const float cosp = std::cos(phi), sinp = std::sin(phi);
    const float dx = (x0 - x) * 0.5f, dy = (y0 - y) * 0.5f;
    const float x1 = cosp * dx + sinp * dy;
    const float y1 = -sinp * dx + cosp * dy;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1.f) {
        const float s = std::sqrt(lam);
        rx *= s;
        ry *= s;
    }
    const float rx2 = rx * rx, ry2 = ry * ry, x12 = x1 * x1, y12 = y1 * y1;
    float num = rx2 * ry2 - rx2 * y12 - ry2 * x12;
    float den = rx2 * y12 + ry2 * x12;
    float coef = den > 0.f ? std::sqrt(std::max(0.f, num / den)) : 0.f;
    if (large == sweep)
        coef = -coef;
    const float cx1 = coef * rx * y1 / ry;
    const float cy1 = coef * -ry * x1 / rx;
    const float cx = cosp * cx1 - sinp * cy1 + (x0 + x) * 0.5f;
    const float cy = sinp * cx1 + cosp * cy1 + (y0 + y) * 0.5f;
    auto ang = [](float ux, float uy, float vx, float vy) {
        const float dot = ux * vx + uy * vy;
        const float len = std::hypot(ux, uy) * std::hypot(vx, vy);
        float a = len > 0.f ? std::acos(std::clamp(dot / len, -1.f, 1.f)) : 0.f;
        if (ux * vy - uy * vx < 0.f)
            a = -a;
        return a;
    };
    const float theta1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    float dtheta = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && dtheta > 0.f)
        dtheta -= 2.f * kPi;
    else if (sweep && dtheta < 0.f)
        dtheta += 2.f * kPi;
    const int nseg = std::max(1, (int) std::ceil(std::fabs(dtheta) / (kPi * 0.5f + 1e-4f)));
    const float delta = dtheta / (float) nseg;
    const float k = (4.f / 3.f) * std::tan(delta * 0.25f);
    auto at = [&](float th) {
        const float lx = rx * std::cos(th), ly = ry * std::sin(th);
        return Point{cosp * lx - sinp * ly + cx, sinp * lx + cosp * ly + cy};
    };
    auto deriv = [&](float th) {
        const float lx = -rx * std::sin(th), ly = ry * std::cos(th);
        return Point{cosp * lx - sinp * ly, sinp * lx + cosp * ly};
    };
    for (int i = 0; i < nseg; ++i) {
        const float t1 = theta1 + delta * (float) i;
        const float t2 = t1 + delta;
        const Point p1 = at(t1), p2 = at(t2), d1 = deriv(t1), d2 = deriv(t2);
        path.push_back(cmd_cubic(p1.x + k * d1.x, p1.y + k * d1.y, p2.x - k * d2.x, p2.y - k * d2.y,
                                 p2.x, p2.y));
    }
}

Path parse_path_d(const std::string &d) {
    Path path;
    size_t i = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0;
    char prev = 0;
    bool have_ctrl = false;
    auto reflect = [&](float &ox, float &oy) {
        if (!have_ctrl) {
            ox = cx;
            oy = cy;
            return;
        }
        ox = 2.f * cx - lx;
        oy = 2.f * cy - ly;
    };
    while (i < d.size()) {
        while (i < d.size() && (std::isspace((unsigned char) d[i]) || d[i] == ','))
            ++i;
        if (i >= d.size())
            break;
        char op = d[i];
        if (std::isalpha((unsigned char) op))
            ++i;
        else if (prev)
            op = prev;
        else
            break;
        const bool rel = std::islower((unsigned char) op);
        const char cmd = (char) std::toupper((unsigned char) op);
        auto num = [&](float &o) { return parse_number(d, i, o); };
        if (cmd == 'M') {
            float x, y;
            if (!num(x) || !num(y))
                break;
            if (rel) {
                x += cx;
                y += cy;
            }
            path.push_back(cmd_move(x, y));
            cx = sx = x;
            cy = sy = y;
            have_ctrl = false;
            prev = rel ? 'l' : 'L';
            while (parse_number(d, i, x) && parse_number(d, i, y)) {
                if (rel) {
                    x += cx;
                    y += cy;
                }
                path.push_back(cmd_line(x, y));
                cx = x;
                cy = y;
                have_ctrl = false;
            }
            continue;
        }
        if (cmd == 'Z') {
            path.push_back(cmd_close());
            cx = sx;
            cy = sy;
            have_ctrl = false;
            prev = 0;
            continue;
        }
        if (cmd == 'L') {
            float x, y;
            while (num(x) && num(y)) {
                if (rel) {
                    x += cx;
                    y += cy;
                }
                path.push_back(cmd_line(x, y));
                cx = x;
                cy = y;
                have_ctrl = false;
            }
            prev = op;
            continue;
        }
        if (cmd == 'H') {
            float x;
            while (num(x)) {
                if (rel)
                    x += cx;
                path.push_back(cmd_line(x, cy));
                cx = x;
                have_ctrl = false;
            }
            prev = op;
            continue;
        }
        if (cmd == 'V') {
            float y;
            while (num(y)) {
                if (rel)
                    y += cy;
                path.push_back(cmd_line(cx, y));
                cy = y;
                have_ctrl = false;
            }
            prev = op;
            continue;
        }
        if (cmd == 'C') {
            float c1x, c1y, c2x, c2y, x, y;
            while (num(c1x) && num(c1y) && num(c2x) && num(c2y) && num(x) && num(y)) {
                if (rel) {
                    c1x += cx;
                    c1y += cy;
                    c2x += cx;
                    c2y += cy;
                    x += cx;
                    y += cy;
                }
                path.push_back(cmd_cubic(c1x, c1y, c2x, c2y, x, y));
                lx = c2x;
                ly = c2y;
                cx = x;
                cy = y;
                have_ctrl = true;
            }
            prev = op;
            continue;
        }
        if (cmd == 'S') {
            float c2x, c2y, x, y;
            while (num(c2x) && num(c2y) && num(x) && num(y)) {
                float c1x, c1y;
                reflect(c1x, c1y);
                if (rel) {
                    c2x += cx;
                    c2y += cy;
                    x += cx;
                    y += cy;
                }
                path.push_back(cmd_cubic(c1x, c1y, c2x, c2y, x, y));
                lx = c2x;
                ly = c2y;
                cx = x;
                cy = y;
                have_ctrl = true;
            }
            prev = op;
            continue;
        }
        if (cmd == 'Q') {
            float c1x, c1y, x, y;
            while (num(c1x) && num(c1y) && num(x) && num(y)) {
                if (rel) {
                    c1x += cx;
                    c1y += cy;
                    x += cx;
                    y += cy;
                }
                /* One quadratic as a cubic, so every backend sees cubics. */
                path.push_back(cmd_cubic(cx + 2.f / 3.f * (c1x - cx), cy + 2.f / 3.f * (c1y - cy),
                                         x + 2.f / 3.f * (c1x - x), y + 2.f / 3.f * (c1y - y), x, y));
                lx = c1x;
                ly = c1y;
                cx = x;
                cy = y;
                have_ctrl = true;
            }
            prev = op;
            continue;
        }
        if (cmd == 'T') {
            float x, y;
            while (num(x) && num(y)) {
                float c1x, c1y;
                reflect(c1x, c1y);
                if (rel) {
                    x += cx;
                    y += cy;
                }
                path.push_back(cmd_cubic(cx + 2.f / 3.f * (c1x - cx), cy + 2.f / 3.f * (c1y - cy),
                                         x + 2.f / 3.f * (c1x - x), y + 2.f / 3.f * (c1y - y), x, y));
                lx = c1x;
                ly = c1y;
                cx = x;
                cy = y;
                have_ctrl = true;
            }
            prev = op;
            continue;
        }
        if (cmd == 'A') {
            float rx, ry, phi, x, y;
            float large, sweep;
            while (num(rx) && num(ry) && num(phi) && num(large) && num(sweep) && num(x) && num(y)) {
                if (rel) {
                    x += cx;
                    y += cy;
                }
                arc_to_cubics(path, cx, cy, rx, ry, phi, large != 0.f, sweep != 0.f, x, y);
                cx = x;
                cy = y;
                have_ctrl = false;
            }
            prev = op;
            continue;
        }
        break;
    }
    return path;
}

Path round_rect(float x, float y, float w, float h, float rx, float ry) {
    rx = std::max(0.f, std::min(rx, std::fabs(w) * 0.5f));
    ry = std::max(0.f, std::min(ry, std::fabs(h) * 0.5f));
    if (rx <= 0.f && ry <= 0.f)
        return rect_path(x, y, w, h);
    if (std::fabs(rx - ry) < 1e-3f)
        return rect_path(x, y, w, h, rx);
    const float kx = rx * kKappa, ky = ry * kKappa;
    const float x1 = x + w, y1 = y + h;
    return {cmd_move(x + rx, y),
            cmd_line(x1 - rx, y),
            cmd_cubic(x1 - rx + kx, y, x1, y + ry - ky, x1, y + ry),
            cmd_line(x1, y1 - ry),
            cmd_cubic(x1, y1 - ry + ky, x1 - rx + kx, y1, x1 - rx, y1),
            cmd_line(x + rx, y1),
            cmd_cubic(x + rx - kx, y1, x, y1 - ry + ky, x, y1 - ry),
            cmd_line(x, y + ry),
            cmd_cubic(x, y + ry - ky, x + rx - kx, y, x + rx, y),
            cmd_close()};
}

Path parse_points(const std::string &s, bool close_it) {
    Path path;
    size_t i = 0;
    float x, y;
    bool first = true;
    while (parse_number(s, i, x) && parse_number(s, i, y)) {
        path.push_back(first ? cmd_move(x, y) : cmd_line(x, y));
        first = false;
    }
    if (close_it && !path.empty())
        path.push_back(cmd_close());
    return path;
}

void strip_css_comments(std::string &s) {
    for (;;) {
        const size_t a = s.find("/*");
        if (a == std::string::npos)
            return;
        const size_t b = s.find("*/", a + 2);
        if (b == std::string::npos) {
            s.erase(a);
            return;
        }
        s.erase(a, b + 2 - a);
    }
}

std::vector<std::pair<std::string, std::string>> parse_decls(const std::string &body) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < body.size()) {
        const size_t semi = body.find(';', i);
        std::string one = body.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        i = semi == std::string::npos ? body.size() : semi + 1;
        const size_t colon = one.find(':');
        if (colon == std::string::npos)
            continue;
        std::string prop = lower(trim(one.substr(0, colon)));
        std::string value = trim(one.substr(colon + 1));
        const size_t bang = value.find("!important");
        if (bang != std::string::npos)
            value = trim(value.substr(0, bang));
        if (!prop.empty() && !value.empty())
            out.emplace_back(prop, value);
    }
    return out;
}

bool simple_selector(const std::string &raw, Rule &rule) {
    const std::string s = trim(raw);
    if (s.empty())
        return false;
    for (char c : s)
        if (c == ' ' || c == '>' || c == '+' || c == '~' || c == '[' || c == ':' || c == '#' || c == '*')
            return false;
    rule.element.clear();
    rule.classes.clear();
    rule.spec = 0;
    size_t i = 0;
    if (s[0] != '.') {
        const size_t dot = s.find('.');
        rule.element = s.substr(0, dot);
        rule.spec += 1;
        if (dot == std::string::npos)
            return !rule.element.empty();
        i = dot;
    }
    while (i < s.size()) {
        if (s[i] != '.')
            return false;
        ++i;
        const size_t next = s.find('.', i);
        const std::string cls = s.substr(i, next == std::string::npos ? std::string::npos : next - i);
        if (cls.empty())
            return false;
        rule.classes.push_back(cls);
        rule.spec += 10;
        if (next == std::string::npos)
            break;
        i = next;
    }
    return true;
}

void parse_stylesheet(const std::string &css, std::vector<Rule> &rules) {
    std::string s = css;
    strip_css_comments(s);
    size_t i = 0;
    int order = 1;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char) s[i]))
            ++i;
        if (i >= s.size())
            break;
        if (s[i] == '@') {
            /* One level of braces: the inner rules of an at-rule are skipped. */
            const size_t open = s.find('{', i);
            if (open == std::string::npos)
                break;
            int depth = 1;
            size_t j = open + 1;
            for (; j < s.size() && depth; ++j) {
                if (s[j] == '{')
                    ++depth;
                else if (s[j] == '}')
                    --depth;
            }
            i = j;
            continue;
        }
        const size_t open = s.find('{', i);
        if (open == std::string::npos)
            break;
        const std::string sels = s.substr(i, open - i);
        const size_t close = s.find('}', open + 1);
        if (close == std::string::npos)
            break;
        const auto props = parse_decls(s.substr(open + 1, close - open - 1));
        size_t k = 0;
        while (k < sels.size()) {
            const size_t comma = sels.find(',', k);
            const std::string one = sels.substr(k, comma == std::string::npos ? std::string::npos : comma - k);
            k = comma == std::string::npos ? sels.size() : comma + 1;
            Rule rule;
            if (!simple_selector(one, rule))
                continue;
            rule.order = order;
            rule.props = props;
            rules.push_back(std::move(rule));
        }
        ++order;
        i = close + 1;
    }
}

std::vector<std::string> class_list(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char) s[i]))
            ++i;
        const size_t b = i;
        while (i < s.size() && !std::isspace((unsigned char) s[i]))
            ++i;
        if (i > b)
            out.push_back(s.substr(b, i - b));
    }
    return out;
}

bool rule_matches(const Rule &rule, const std::string &element, const std::vector<std::string> &classes) {
    if (!rule.element.empty() && rule.element != element)
        return false;
    for (const std::string &c : rule.classes) {
        bool found = false;
        for (const std::string &h : classes)
            found = found || h == c;
        if (!found)
            return false;
    }
    return true;
}

const char *kPaintProps[] = {"fill",        "fill-opacity",      "fill-rule",         "stroke",
                             "stroke-width", "stroke-opacity",    "stroke-linecap",    "stroke-linejoin",
                             "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "opacity",
                             "display",      "visibility",        "color",             "font-size",
                             "clip-path",    "transform"};

void consider(std::map<std::string, Decl> &best, const std::string &prop, const std::string &value,
              int spec, int order) {
    if (lower(trim(value)) == "inherit")
        return;
    auto it = best.find(prop);
    if (it == best.end() || spec > it->second.spec ||
        (spec == it->second.spec && order >= it->second.order))
        best[prop] = Decl{prop, value, spec, order};
}

float parse_opacity(const std::string &s, float fallback) {
    const Len L = parse_len(s);
    if (!L.ok)
        return fallback;
    return L.unit == Unit::Percent ? L.n / 100.f : L.n;
}

void apply_prop(Style &st, const std::string &prop, const std::string &value, const Viewport &vp,
                const std::map<std::string, Grad> &grads, VectorScene &scene) {
    if (prop == "fill" || prop == "stroke" || prop == "color") {
        std::string id;
        if (parse_url_id(value, id)) {
            note_skip(scene, "gradients");
            auto it = grads.find(id);
            VectorPaint paint;
            if (it != grads.end() && !it->second.stops.empty()) {
                paint.kind = it->second.radial ? PaintKind::Radial : PaintKind::Linear;
                paint.stops = it->second.stops;
                float best = 1e9f;
                int idx = 0;
                for (int i = 0; i < (int) paint.stops.size(); ++i) {
                    const float d = std::fabs(paint.stops[i].offset - 0.5f);
                    /* Later stop wins a tie. A slightly worse later stop does not. */
                    if (i == 0 || d < best - 1e-4f ||
                        (std::fabs(d - best) <= 1e-4f && d <= best)) {
                        best = d;
                        idx = i;
                    }
                }
                paint.color = paint.stops[idx].color;
            } else {
                paint.kind = PaintKind::Solid;
                paint.color = {0, 0, 0, 1};
            }
            if (prop == "fill")
                st.fill = paint;
            else if (prop == "stroke")
                st.stroke = paint;
            else
                st.current = paint.color;
            return;
        }
        Color c;
        bool none = false;
        if (!parse_color(value, st.current, c, none))
            return;
        if (prop == "color") {
            if (!none)
                st.current = c;
            return;
        }
        VectorPaint paint;
        if (!none) {
            paint.kind = PaintKind::Solid;
            paint.color = c;
        }
        if (prop == "fill")
            st.fill = paint;
        else
            st.stroke = paint;
        return;
    }
    if (prop == "fill-opacity")
        st.fill_opacity = parse_opacity(value, st.fill_opacity);
    else if (prop == "stroke-opacity")
        st.stroke_opacity = parse_opacity(value, st.stroke_opacity);
    else if (prop == "opacity")
        st.opacity = parse_opacity(value, st.opacity);
    else if (prop == "fill-rule")
        st.rule = lower(trim(value)) == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
    else if (prop == "stroke-width")
        st.stroke_width = length_user(value, st.stroke_width, true, vp, st.font_px);
    else if (prop == "stroke-linecap") {
        const std::string v = lower(trim(value));
        st.cap = v == "round" ? LineCap::Round : v == "square" ? LineCap::Square : LineCap::Butt;
    } else if (prop == "stroke-linejoin") {
        const std::string v = lower(trim(value));
        st.join = v == "round" ? LineJoin::Round : v == "bevel" ? LineJoin::Bevel : LineJoin::Miter;
    } else if (prop == "stroke-miterlimit") {
        const Len L = parse_len(value);
        if (L.ok)
            st.miter = L.n;
    } else if (prop == "stroke-dasharray") {
        const std::string v = lower(trim(value));
        st.dash.clear();
        if (v != "none") {
            size_t i = 0;
            float n;
            std::string rest = value;
            while (parse_number(rest, i, n))
                st.dash.push_back(length_user(std::to_string(n), n, true, vp, st.font_px));
        }
    } else if (prop == "stroke-dashoffset")
        st.dash_offset = length_user(value, st.dash_offset, true, vp, st.font_px);
    else if (prop == "display")
        st.display = lower(trim(value)) != "none";
    else if (prop == "visibility") {
        const std::string v = lower(trim(value));
        st.visible = v != "hidden" && v != "collapse";
    } else if (prop == "font-size") {
        float px = 0, pct = 0;
        bool percent = false;
        if (length_px(value, st.font_px, px, percent, pct))
            st.font_px = percent ? pct / 100.f * st.font_px : px;
    } else if (prop == "clip-path")
        st.clip = value;
    else if (prop == "transform")
        st.transform = value;
}

Style compute_style(const XmlNode &el, const Style &inherited, const std::vector<Rule> &rules,
                    const Viewport &vp, const std::map<std::string, Grad> &grads, VectorScene &scene) {
    Style st = inherited;
    st.opacity = 1.f;
    st.display = true;
    st.transform.clear();
    st.clip.clear();
    std::map<std::string, Decl> best;
    for (const char *prop : kPaintProps) {
        const std::string v = xml_attr_value(el, prop);
        if (!v.empty())
            consider(best, prop, v, 0, 0);
    }
    const auto classes = class_list(xml_attr_value(el, "class"));
    for (const Rule &rule : rules) {
        if (!rule_matches(rule, el.local, classes))
            continue;
        for (const auto &p : rule.props)
            consider(best, p.first, p.second, rule.spec, rule.order);
    }
    int n = 0;
    for (const auto &p : parse_decls(xml_attr_value(el, "style")))
        consider(best, p.first, p.second, 1000, 1000000 + n++);
    for (const auto &kv : best)
        apply_prop(st, kv.first, kv.second.value, vp, grads, scene);
    return st;
}

PathCmd bake(const Transform &t, PathCmd c) {
    auto P = [&](float x, float y) { return t.apply({x, y}); };
    if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
        const Point c1 = P(c.cx1, c.cy1);
        c.cx1 = c1.x;
        c.cy1 = c1.y;
    }
    if (c.kind == PathCmd::Cubic) {
        const Point c2 = P(c.cx2, c.cy2);
        c.cx2 = c2.x;
        c.cy2 = c2.y;
    }
    if (c.kind != PathCmd::Close) {
        const Point p = P(c.x, c.y);
        c.x = p.x;
        c.y = p.y;
    }
    return c;
}

void emit_shape(VectorScene &scene, const Path &user, const Transform &xf, const Style &st, float alpha,
                bool fill_it) {
    if (user.empty() || !st.visible)
        return;
    if (!trim(st.clip).empty() && lower(trim(st.clip)) != "none")
        note_skip(scene, "clips");
    const float det = xf.a * xf.d - xf.b * xf.c;
    const float scale = std::sqrt(std::fabs(det));
    const float s = scale > 1e-8f ? scale : 1.f;
    VectorShape shape;
    shape.rule = st.rule;
    shape.path.reserve(user.size());
    for (const PathCmd &c : user)
        shape.path.push_back(bake(xf, c));
    auto tint = [&](Color c, float extra) {
        c.a *= alpha * extra;
        if (c.a < 0.f)
            c.a = 0.f;
        if (c.a > 1.f)
            c.a = 1.f;
        return c;
    };
    if (fill_it && !st.fill.none()) {
        shape.fill = st.fill;
        shape.fill.color = tint(st.fill.color, st.fill_opacity);
    }
    if (!st.stroke.none() && st.stroke_width > 0.f) {
        shape.stroke.paint = st.stroke;
        shape.stroke.paint.color = tint(st.stroke.color, st.stroke_opacity);
        shape.stroke.width = st.stroke_width * s;
        shape.stroke.cap = st.cap;
        shape.stroke.join = st.join;
        shape.stroke.miter_limit = st.miter;
        shape.stroke.dash = st.dash;
        for (float &d : shape.stroke.dash)
            d *= s;
        shape.stroke.dash_offset = st.dash_offset * s;
    }
    if (shape.fill.none() && shape.stroke.none())
        return;
    scene.shapes.push_back(std::move(shape));
}

float attr_user(const XmlNode &el, const char *name, float fallback, bool horiz, const Viewport &vp,
                float font_px) {
    return length_user(xml_attr_value(el, name), fallback, horiz, vp, font_px);
}

void collect(const XmlNode &el, std::vector<Rule> &rules, std::map<std::string, Grad> &grads) {
    if (el.text)
        return;
    if (el.local == "style")
        parse_stylesheet(xml_text(el), rules);
    if (el.local == "linearGradient" || el.local == "radialGradient") {
        const std::string id = xml_attr_value(el, "id");
        if (!id.empty()) {
            Grad g;
            g.radial = el.local == "radialGradient";
            for (const XmlNode &c : el.children) {
                if (c.local != "stop")
                    continue;
                GradientStop stop;
                std::string off = xml_attr_value(c, "offset");
                std::string color = xml_attr_value(c, "stop-color");
                std::string op = xml_attr_value(c, "stop-opacity");
                for (const auto &p : parse_decls(xml_attr_value(c, "style"))) {
                    if (p.first == "stop-color")
                        color = p.second;
                    else if (p.first == "stop-opacity")
                        op = p.second;
                    else if (p.first == "offset")
                        off = p.second;
                }
                const Len L = parse_len(off);
                stop.offset = L.ok ? (L.unit == Unit::Percent ? L.n / 100.f : L.n) : 0.f;
                bool none = false;
                Color col{0, 0, 0, 1};
                if (!color.empty())
                    parse_color(color, col, col, none);
                if (!op.empty())
                    col.a *= parse_opacity(op, 1.f);
                stop.color = col;
                g.stops.push_back(stop);
            }
            grads[id] = std::move(g);
        }
    }
    for (const XmlNode &c : el.children)
        collect(c, rules, grads);
}

bool is_skippable(const std::string &local) {
    return local == "defs" || local == "symbol" || local == "clipPath" || local == "linearGradient" ||
           local == "radialGradient" || local == "pattern" || local == "mask" || local == "marker" ||
           local == "style" || local == "title" || local == "desc" || local == "metadata" ||
           local == "script" || local == "filter";
}

void walk(const XmlNode &el, const Transform &parent, float parent_a, const Style &inherited,
          const Viewport &vp, const std::vector<Rule> &rules, const std::map<std::string, Grad> &grads,
          VectorScene &scene) {
    if (el.text || is_skippable(el.local))
        return;
    if (el.local == "text" || el.local == "tspan" || el.local == "textPath") {
        note_skip(scene, "text");
        return;
    }
    const Style st = compute_style(el, inherited, rules, vp, grads, scene);
    if (!st.display)
        return;
    const Transform local = parse_transform(st.transform.empty() ? xml_attr_value(el, "transform") : st.transform);
    Transform world = parent * local;
    const float alpha = parent_a * st.opacity;
    if (el.local == "g" || el.local == "a" || el.local == "switch") {
        for (const XmlNode &c : el.children)
            walk(c, world, alpha, st, vp, rules, grads, scene);
        return;
    }
    if (el.local == "svg") {
        const Viewport nested = make_viewport(el, &vp);
        const float x = attr_user(el, "x", 0, true, vp, st.font_px);
        const float y = attr_user(el, "y", 0, false, vp, st.font_px);
        /* Nested view box, then the parent's map. x,y are in the parent. */
        const Transform into = Transform::translate(x, y) *
                               viewbox_map(nested.vb_x, nested.vb_y, nested.vb_w, nested.vb_h,
                                           attr_user(el, "width", nested.vb_w, true, vp, st.font_px),
                                           attr_user(el, "height", nested.vb_h, false, vp, st.font_px),
                                           xml_attr_value(el, "preserveAspectRatio"));
        for (const XmlNode &c : el.children)
            walk(c, world * into, alpha, st, nested, rules, grads, scene);
        return;
    }
    if (el.local == "use" || el.local == "image" || el.local == "foreignObject")
        return;
    Path user;
    bool fill_it = true;
    if (el.local == "path")
        user = parse_path_d(xml_attr_value(el, "d"));
    else if (el.local == "rect") {
        const float w = attr_user(el, "width", 0, true, vp, st.font_px);
        const float h = attr_user(el, "height", 0, false, vp, st.font_px);
        if (w > 0.f && h > 0.f) {
            float rx = attr_user(el, "rx", -1, true, vp, st.font_px);
            float ry = attr_user(el, "ry", -1, false, vp, st.font_px);
            if (rx < 0 && ry < 0)
                rx = ry = 0;
            else if (rx < 0)
                rx = ry;
            else if (ry < 0)
                ry = rx;
            user = round_rect(attr_user(el, "x", 0, true, vp, st.font_px),
                              attr_user(el, "y", 0, false, vp, st.font_px), w, h, rx, ry);
        }
    } else if (el.local == "circle") {
        const float r = attr_user(el, "r", 0, true, vp, st.font_px);
        if (r > 0.f)
            user = ellipse_path(attr_user(el, "cx", 0, true, vp, st.font_px),
                                attr_user(el, "cy", 0, false, vp, st.font_px), r, r);
    } else if (el.local == "ellipse") {
        const float rx = attr_user(el, "rx", 0, true, vp, st.font_px);
        const float ry = attr_user(el, "ry", 0, false, vp, st.font_px);
        if (rx > 0.f && ry > 0.f)
            user = ellipse_path(attr_user(el, "cx", 0, true, vp, st.font_px),
                                attr_user(el, "cy", 0, false, vp, st.font_px), rx, ry);
    } else if (el.local == "line") {
        user = {cmd_move(attr_user(el, "x1", 0, true, vp, st.font_px),
                         attr_user(el, "y1", 0, false, vp, st.font_px)),
                cmd_line(attr_user(el, "x2", 0, true, vp, st.font_px),
                         attr_user(el, "y2", 0, false, vp, st.font_px))};
        fill_it = false;
    } else if (el.local == "polyline" || el.local == "polygon")
        user = parse_points(xml_attr_value(el, "points"), el.local == "polygon");
    else
        return;
    emit_shape(scene, user, world, st, alpha, fill_it);
}

bool sniff_svg(const uint8_t *data, size_t n) {
    size_t i = 0;
    if (n >= 3 && data && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        i = 3;
    auto ws = [&] {
        while (i < n && data && std::isspace(data[i]))
            ++i;
    };
    ws();
    while (i < n && data[i] == '<') {
        if (i + 1 < n && data[i + 1] == '?') {
            i += 2;
            while (i + 1 < n && !(data[i] == '?' && data[i + 1] == '>'))
                ++i;
            if (i + 1 >= n)
                return false;
            i += 2;
        } else if (i + 3 < n && data[i + 1] == '!' && data[i + 2] == '-' && data[i + 3] == '-') {
            i += 4;
            while (i + 2 < n && !(data[i] == '-' && data[i + 1] == '-' && data[i + 2] == '>'))
                ++i;
            if (i + 2 >= n)
                return false;
            i += 3;
        } else if (i + 1 < n && data[i + 1] == '!') {
            i += 2;
            int depth = 0;
            for (; i < n; ++i) {
                if (data[i] == '[')
                    ++depth;
                else if (data[i] == ']' && depth)
                    --depth;
                else if (data[i] == '>' && depth == 0) {
                    ++i;
                    break;
                }
            }
        } else
            break;
        ws();
    }
    if (!data || i >= n || data[i] != '<')
        return false;
    ++i;
    if (i < n && (data[i] == '/' || data[i] == '!' || data[i] == '?'))
        return false;
    std::string name;
    while (i < n && (std::isalnum(data[i]) || data[i] == ':' || data[i] == '_' || data[i] == '-' ||
                     data[i] == '.'))
        name.push_back((char) data[i++]);
    const size_t colon = name.rfind(':');
    const std::string local = colon == std::string::npos ? name : name.substr(colon + 1);
    return local == "svg";
}

class SvgDecoder : public VectorDecoder {
public:
    const char *id() const override { return "svg"; }
    bool sniff(const uint8_t *data, size_t n) const override { return sniff_svg(data, n); }
    std::vector<std::pair<std::string, std::string>> types() const override {
        return {{"svg", "SVG"}};
    }
    bool decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error) const override {
        if (!out)
            return false;
        *out = VectorScene();
        XmlNode root;
        if (!xml_parse(data, n, &root, error))
            return false;
        if (root.local != "svg") {
            if (error)
                *error = "the file is not an SVG";
            return false;
        }
        std::vector<Rule> rules;
        std::map<std::string, Grad> grads;
        collect(root, rules, grads);
        const Viewport vp = make_viewport(root, nullptr);
        out->width = vp.box_w;
        out->height = vp.box_h;
        Style base;
        const Style root_style = compute_style(root, base, rules, vp, grads, *out);
        if (!root_style.display)
            return true;
        const Transform world = vp.map * parse_transform(root_style.transform);
        const float alpha = root_style.opacity;
        for (const XmlNode &c : root.children)
            walk(c, world, alpha, root_style, vp, rules, grads, *out);
        return true;
    }
};

} // namespace

void register_svg_decoder(VectorLoader &loader) {
    loader.add(std::make_unique<SvgDecoder>());
}

} // namespace pagemade
