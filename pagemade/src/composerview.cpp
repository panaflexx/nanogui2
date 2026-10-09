/*
 * pagemade/composerview.cpp — see composerview.h.
 */
#include "composerview.h"
#include "render_nvg.h"

#include <nanogui/screen.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanovg.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

using namespace nanogui;
using namespace pagemade;

namespace {

constexpr float kPasteboard = 72.f;     // pasteboard margin around the page, points
constexpr float kMinBlock   = 12.f;     // smallest item, points
constexpr float kMinDraw    = 2.f;      // a drawing drag shorter than this is a click
constexpr float kHandlePx   = 7.f;      // handle sizes are in screen pixels
constexpr float kTabWPx     = 18.f, kTabHPx = 9.f;
constexpr float kPickPx     = 4.f;      // how near an outline or line counts as on it
constexpr float kSnapPx     = 5.f;      // how near a guide snaps
constexpr float kPi         = 3.14159265f;

const NVGcolor kPasteboardColor = nvgRGB(214, 214, 214);

/* Styles of the runs that overlap [a, b). An empty paragraph contributes
 * its one run, which is what the next character typed there will use. */
void visit_styles(const Story &s, TextPos a, TextPos b,
                  const std::function<void(const CharStyle &)> &fn) {
    a = clamp(s, a);
    b = clamp(s, b);
    if (b < a)
        std::swap(a, b);
    for (size_t pi = a.para; pi <= b.para && pi < s.paragraphs.size(); ++pi) {
        const Paragraph &p = s.paragraphs[pi];
        uint32_t from = pi == a.para ? a.byte : 0;
        uint32_t to = pi == b.para ? b.byte : para_length(p);
        if (para_length(p) == 0) {
            if (!p.runs.empty())
                fn(p.runs.front().style);
            continue;
        }
        uint32_t acc = 0;
        for (const Run &r : p.runs) {
            uint32_t next = acc + (uint32_t) r.text.size();
            if (next > from && acc < to)
                fn(r.style);
            acc = next;
        }
    }
}
const NVGcolor kMarginGuide     = nvgRGB(230, 60, 200);    // magenta
const NVGcolor kColumnGuide     = nvgRGB(40, 110, 230);    // blue
const NVGcolor kBlockOutline    = nvgRGBA(0, 0, 0, 40);
const NVGcolor kOversetRed      = nvgRGB(220, 30, 30);

/* How much a transform scales lengths (1 for moves and turns). */
float item_scale(const Transform &t) {
    float s = std::sqrt(std::fabs(t.a * t.d - t.b * t.c));
    return s > 1e-6f ? s : 1.f;
}

float dist_to_segment(Point p, Point a, Point b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float len2 = vx * vx + vy * vy;
    float t = len2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.f;
    t = std::clamp(t, 0.f, 1.f);
    const float dx = p.x - (a.x + t * vx), dy = p.y - (a.y + t * vy);
    return std::sqrt(dx * dx + dy * dy);
}

bool is_line(const Item &it) {
    const Shape *sh = it.shape();
    return sh && sh->kind == Shape::Kind::Line;
}

/* The handle's position in item space. */
Point handle_point(const Item &it, int h) {
    switch (h) {
    case 2: return {it.w, 0};              // TopRight
    case 3: return {0, it.h};              // BottomLeft
    case 4: return {it.w, it.h};           // BottomRight
    case 5: return {it.w * 0.5f, it.h};    // Windowshade
    default: return {0, 0};                // TopLeft
    }
}

} // namespace

ComposerView::ComposerView(Widget *parent, const FontLibrary *fonts, const Hyphenator *hyphenator)
    : Widget(parent), m_fonts(fonts), m_hyphenator(hyphenator) {
    set_live(true);   // zoom-dependent hairlines; never bake into a parent's draw list
}

void ComposerView::set_document(PageDoc doc) {
    m_doc = std::move(doc);
    m_page = 0;
    m_sel.clear();
    m_edit_story = 0;
    m_selecting = false;
    m_undo.clear();
    m_redo.clear();
    m_burst_open = false;
    m_version = m_clean_version = ++m_next_version;   // as loaded: unmodified
    recompose();
    selection_changed();
    if (on_document_change)
        on_document_change();
}

void ComposerView::select(ItemId id) {
    m_sel.clear();
    if (id && m_doc.find_item(id))
        m_sel.push_back(id);
    selection_changed();
}

void ComposerView::select_frame(size_t story, size_t frame) {
    if (story < m_doc.stories.size() && frame < m_doc.stories[story].thread.size())
        select(m_doc.stories[story].thread[frame]);
}

bool ComposerView::is_selected(ItemId id) const {
    return std::find(m_sel.begin(), m_sel.end(), id) != m_sel.end();
}

void ComposerView::selection_changed() {
    if (on_selection_change)
        on_selection_change();
}

Bounds ComposerView::selection_bounds() const {
    Bounds b;
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            b.add(item_bounds(*it));
    return b;
}

/* ---- Undo --------------------------------------------------------------- */

void ComposerView::bump_version() {
    m_version = ++m_next_version;
    if (on_document_change)
        on_document_change();
}

void ComposerView::push_snapshot(PageDoc doc, uint64_t version) {
    m_undo.push_back({std::move(doc), version});
    if (m_undo.size() > 100)
        m_undo.pop_front();
    m_redo.clear();
    bump_version();
}

void ComposerView::push_undo() {
    push_snapshot(m_doc, m_version);
}

void ComposerView::will_edit(bool burst) {
    if (!burst || !m_burst_open)
        push_undo();
    else
        bump_version();                  // same undo step, but a changed document
    m_burst_open = burst;
}

void ComposerView::mark_clean() {
    m_clean_version = m_version;
    m_burst_open = false;                // the next keystroke starts a new step
    if (on_document_change)
        on_document_change();
}

void ComposerView::restore_snapshot() {
    m_burst_open = false;
    m_selecting = false;
    m_placing = false;
    m_rotating = false;
    m_marquee = false;
    m_creating = false;
    m_drag = Handle::None;
    if (m_doc.pages.empty())
        m_doc.pages.emplace_back();
    if (m_page >= m_doc.pages.size())
        m_page = m_doc.pages.size() - 1;
    /* Keep the selection and caret where they still exist, on this page. */
    m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(),
                               [&](ItemId id) {
                                   size_t pg = 0;
                                   return !m_doc.find_item(id, &pg) || pg != m_page;
                               }),
                m_sel.end());
    if (m_edit_story && !m_doc.find_story(m_edit_story))
        m_edit_story = 0;
    if (Story *s = edit_story()) {
        m_caret = clamp(*s, m_caret);
        m_anchor = clamp(*s, m_anchor);
    } else {
        m_typing_on = false;
    }
    recompose();
    selection_changed();
    if (on_page_change)
        on_page_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::undo() {
    if (m_undo.empty())
        return;
    m_redo.push_back({std::move(m_doc), m_version});
    m_doc = std::move(m_undo.back().doc);
    m_version = m_undo.back().version;
    m_undo.pop_back();
    restore_snapshot();
    if (on_document_change)
        on_document_change();
}

void ComposerView::redo() {
    if (m_redo.empty())
        return;
    m_undo.push_back({std::move(m_doc), m_version});
    m_doc = std::move(m_redo.back().doc);
    m_version = m_redo.back().version;
    m_redo.pop_back();
    restore_snapshot();
    if (on_document_change)
        on_document_change();
}

void ComposerView::set_tool(Tool t) {
    if (m_tool == t)
        return;
    end_placement(false);
    if (m_creating)
        finish_creation();
    m_tool = t;
    m_burst_open = false;
    m_selecting = false;
    m_rotating = false;
    m_marquee = false;
    m_drag = Handle::None;
    if (t != Tool::Text)
        m_edit_story = 0;                // put the story down
    else
        m_sel.clear();                   // no item selection while editing text
    if (on_tool_change)
        on_tool_change(t);
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::recompose() {
    m_comp.clear();
    for (const StoryEntry &s : m_doc.stories)
        m_comp.push_back(compose(s.story, m_doc.thread_frames(s), *m_fonts, m_hyphenator));
    if (on_recompose)
        on_recompose();
}

const Composition *ComposerView::comp_of_frame(ItemId id, size_t *thread_index) const {
    const StoryEntry *se = m_doc.story_of(id, thread_index);
    if (!se)
        return nullptr;
    size_t i = m_doc.story_index(se->id);
    return i < m_comp.size() ? &m_comp[i] : nullptr;
}

Vector2f ComposerView::page_origin() const {
    /* ZoomScrollPanel centers our preferred size when it's smaller than the
     * viewport, so the page sits a fixed pasteboard margin in. */
    return Vector2f(kPasteboard, kPasteboard);
}

Vector2i ComposerView::preferred_size(NVGcontext *) const {
    return Vector2i((int) std::ceil(m_doc.setup.width + 2 * kPasteboard),
                    (int) std::ceil(m_doc.setup.height + 2 * kPasteboard));
}

float ComposerView::zoom() const {
    auto *zsp = dynamic_cast<const ZoomScrollPanel *>(parent());
    return zsp ? (float) zsp->zoom() : 1.f;
}

Point ComposerView::page_point_from_screen(const Vector2i &screen_p) const {
    Vector2f local;
    if (auto *zsp = dynamic_cast<const ZoomScrollPanel *>(parent())) {
        Vector2i in = screen_p - zsp->absolute_position();
        auto pan = zsp->pan_offset();
        double z = zsp->zoom() != 0.0 ? zsp->zoom() : 1.0;
        local = Vector2f((float) ((in.x() - pan.x()) / z),
                         (float) ((in.y() - pan.y()) / z)) - Vector2f(m_pos);
    } else {
        local = Vector2f(screen_p - absolute_position());
    }
    local -= page_origin();
    return {local.x(), local.y()};
}

/* ---- Snapping ----------------------------------------------------------- */

std::vector<float> ComposerView::guides_x() const {
    const PageSetup &s = m_doc.setup;
    const float l = s.margin_inside, r = s.width - s.margin_outside;
    std::vector<float> g{0, s.width, l, r};
    if (s.columns > 1) {
        const float col = (r - l - s.gutter * (s.columns - 1)) / s.columns;
        for (int c = 1; c < s.columns; ++c) {
            const float x = l + c * col + (c - 1) * s.gutter;
            g.push_back(x);
            g.push_back(x + s.gutter);
        }
    }
    return g;
}

std::vector<float> ComposerView::guides_y() const {
    const PageSetup &s = m_doc.setup;
    return {0, s.height, s.margin_top, s.height - s.margin_bottom};
}

Point ComposerView::snap_point(Point p) const {
    if (!m_snap)
        return p;
    const float tol = kSnapPx / zoom();
    auto snap = [&](float v, const std::vector<float> &gs) {
        float best = v, d = tol;
        for (float g : gs)
            if (std::fabs(g - v) <= d) {
                d = std::fabs(g - v);
                best = g;
            }
        return best;
    };
    return {snap(p.x, guides_x()), snap(p.y, guides_y())};
}

Point ComposerView::snap_move(const Bounds &b, Point d) const {
    if (!m_snap || b.empty())
        return d;
    const float tol = kSnapPx / zoom();
    auto adjust = [&](std::initializer_list<float> edges, const std::vector<float> &gs, float dv) {
        float best = tol + 1;
        for (float e : edges)
            for (float g : gs)
                if (std::fabs(g - (e + dv)) < std::fabs(best))
                    best = g - (e + dv);
        return std::fabs(best) <= tol ? dv + best : dv;
    };
    return {adjust({b.x0, (b.x0 + b.x1) * 0.5f, b.x1}, guides_x(), d.x),
            adjust({b.y0, (b.y0 + b.y1) * 0.5f, b.y1}, guides_y(), d.y)};
}

/* ---- Hit testing (each item in its own space) ---------------------------- */

bool ComposerView::hits_item(const Item &it, const Point &pt) const {
    const Point l = it.xf.inverse().apply(pt);
    const float tol = kPickPx / (zoom() * item_scale(it.xf));
    const bool inside = l.x >= 0 && l.x <= it.w && l.y >= 0 && l.y <= it.h;
    const Shape *sh = it.shape();
    if (!sh)
        return inside;                   // text frames: anywhere in the block
    const float half = sh->stroke.none() ? 0.f : sh->stroke.weight * 0.5f;
    switch (sh->kind) {
    case Shape::Kind::Line:
        return dist_to_segment(l, {0, 0}, {it.w, it.h}) <= tol + half;
    case Shape::Kind::Rect:
    case Shape::Kind::Polygon: {
        const float t = tol + half;
        const bool near = l.x >= -t && l.x <= it.w + t && l.y >= -t && l.y <= it.h + t;
        if (!sh->fill.none())
            return near;
        const bool deep = l.x > t && l.x < it.w - t && l.y > t && l.y < it.h - t;
        return near && !deep;            // an unfilled shape is picked by its outline
    }
    case Shape::Kind::Ellipse: {
        const float rx = it.w * 0.5f, ry = it.h * 0.5f;
        if (rx <= 0 || ry <= 0)
            return false;
        const float nx = (l.x - rx) / rx, ny = (l.y - ry) / ry;
        const float d = std::sqrt(nx * nx + ny * ny);
        const float r = std::min(rx, ry);
        return !sh->fill.none() ? d <= 1.f + (tol + half) / r
                                : std::fabs(d - 1.f) * r <= tol + half;
    }
    }
    return false;
}

ComposerView::Hit ComposerView::hit_test(const Point &pt) const {
    const float px = 1.f / zoom();

    for (auto id = m_sel.rbegin(); id != m_sel.rend(); ++id) {
        const Item *it = m_doc.find_item(*id);
        if (!it)
            continue;
        const Point l = it->xf.inverse().apply(pt);
        const float s = item_scale(it->xf);
        const float hs = kHandlePx * px / s;
        auto near = [&](float x, float y) {
            return std::fabs(l.x - x) <= hs && std::fabs(l.y - y) <= hs;
        };
        Hit hit{*id, Handle::None};
        if (is_line(*it)) {
            if (near(0, 0))            { hit.handle = Handle::TopLeft;     return hit; }
            if (near(it->w, it->h))    { hit.handle = Handle::BottomRight; return hit; }
        } else {
            if (near(0, 0))            { hit.handle = Handle::TopLeft;     return hit; }
            if (near(it->w, 0))        { hit.handle = Handle::TopRight;    return hit; }
            if (near(0, it->h))        { hit.handle = Handle::BottomLeft;  return hit; }
            if (near(it->w, it->h))    { hit.handle = Handle::BottomRight; return hit; }
        }
        if (it->is_text()) {
            const float tw = kTabWPx * 0.5f * px / s, th = kTabHPx * px / s;
            if (std::fabs(l.x - it->w * 0.5f) <= tw && std::fabs(l.y - it->h) <= th) {
                hit.handle = Handle::Windowshade;
                return hit;
            }
            if (std::fabs(l.x - it->w * 0.5f) <= tw && std::fabs(l.y) <= th) {
                hit.handle = Handle::TopTab;
                return hit;
            }
        }
    }
    const auto &its = items();
    for (auto it = its.rbegin(); it != its.rend(); ++it)
        if (hits_item(*it, pt))
            return {it->id, Handle::Move};
    return {};
}

ItemId ComposerView::text_frame_at(const Point &pt) const {
    const auto &its = items();
    for (auto it = its.rbegin(); it != its.rend(); ++it)
        if (it->is_text() && hits_item(*it, pt))
            return it->id;
    return 0;
}

/* ---- Mouse --------------------------------------------------------------- */

bool ComposerView::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    if (m_placing) {
        if (down)
            end_placement(button == GLFW_MOUSE_BUTTON_1);
        return true;
    }
    if (button != GLFW_MOUSE_BUTTON_1)
        return Widget::mouse_button_event(p, button, down, modifiers);
    const Point pt = page_point_from_screen(screen()->mouse_pos());

    if (!down) {
        if (m_creating) {
            finish_creation();
        } else if (m_marquee) {
            /* Select what the marquee encloses, as PageMaker does. */
            Bounds box;
            box.add(m_drag_start);
            box.add(m_marquee_end);
            if (!m_marquee_add)
                m_sel.clear();
            for (const Item &it : items())
                if (box.contains(item_bounds(it)) && !is_selected(it.id))
                    m_sel.push_back(it.id);
            m_marquee = false;
            selection_changed();
            if (screen())
                screen()->redraw();
        } else if (m_tool == Tool::Pointer && m_drag_target &&
                   (m_drag == Handle::Windowshade || m_drag == Handle::TopTab)) {
            /* A click (no drag) on a windowshade tab is a threading gesture:
             * the red overset arrow picks the story up, a child's top tab
             * merges it back into the parent. */
            float travel = std::fabs(pt.x - m_drag_start.x) + std::fabs(pt.y - m_drag_start.y);
            size_t ti = 0;
            const StoryEntry *se = m_doc.story_of(m_drag_target, &ti);
            if (travel <= 2.f && se) {
                const size_t si = m_doc.story_index(se->id);
                const bool last = ti + 1 == se->thread.size();
                if (m_drag == Handle::Windowshade && last && si < m_comp.size() &&
                    m_comp[si].overset) {
                    m_sel = {m_drag_target};
                    begin_placement(pt);
                } else if (m_drag == Handle::TopTab && ti > 0) {
                    push_undo();             // merge into the parent block
                    ItemId parent = se->thread[ti - 1];
                    m_doc.remove_item(m_drag_target);
                    m_sel = {parent};
                    recompose();
                    selection_changed();
                    if (screen())
                        screen()->redraw();
                }
            }
        }
        m_drag = Handle::None;
        m_drag_target = 0;
        m_selecting = false;
        m_rotating = false;
        return true;
    }
    request_focus();
    m_burst_open = false;                // any click ends a typing or nudging run
    const bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;

    if (m_tool == Tool::Text) {
        double now = glfwGetTime();
        bool near = std::fabs(pt.x - m_down_pt.x) + std::fabs(pt.y - m_down_pt.y) <= 4.f;
        ItemId frame = text_frame_at(pt);
        const StoryEntry *se = frame ? m_doc.story_of(frame) : nullptr;
        const bool same_story = se && se->id == m_edit_story;
        if (!shift && m_last_click >= 0.0 && now - m_last_click < 0.4 && near && same_story)
            m_clicks = std::min(m_clicks + 1, 3);
        else
            m_clicks = 1;
        m_last_click = now;
        m_down_pt = pt;
        m_goal_x = -1.f;

        m_typing_on = false;
        if (!se) {
            begin_creation(pt);          // drag out a new text block (a click stops editing)
        } else {
            const bool extend = shift && same_story;
            m_edit_story = se->id;
            TextPos pos = text_pos_at(pt);
            if (extend || m_clicks <= 1) {
                place_caret(pos, extend);
                m_selecting = true;
            } else if (m_clicks == 2) {
                auto w = word_at(se->story, pos);
                m_anchor = w.first;
                m_caret = w.second;
            } else {
                m_anchor = {pos.para, 0};
                m_caret = {pos.para, para_length(se->story.paragraphs[pos.para])};
            }
        }
        selection_changed();
        if (screen())
            screen()->redraw();
        return true;
    }

    m_edit_story = 0;
    m_typing_on = false;
    m_drag_start = pt;
    m_gesture_saved = false;             // a new drag, not yet snapshotted
    m_drag = Handle::None;
    m_drag_target = 0;

    if (m_tool == Tool::Line || m_tool == Tool::Rect || m_tool == Tool::Ellipse ||
        m_tool == Tool::Polygon) {
        begin_creation(pt);
        return true;
    }

    Hit hit = hit_test(pt);
    if (m_tool == Tool::Rotate) {
        if (hit.item && !is_selected(hit.item))
            m_sel = {hit.item};
        else if (!hit.item)
            m_sel.clear();
        m_rotating = hit.item != 0;
        m_drag_target = hit.item;
    } else if (!hit.item) {
        if (!shift)
            m_sel.clear();
        m_marquee = true;
        m_marquee_add = shift;
        m_marquee_end = pt;
    } else if (hit.handle != Handle::Move) {
        m_drag = hit.handle;             // a handle of a selected item
        m_drag_target = hit.item;
    } else if (shift) {
        if (is_selected(hit.item))
            m_sel.erase(std::find(m_sel.begin(), m_sel.end(), hit.item));
        else {
            m_sel.push_back(hit.item);
            m_drag = Handle::Move;
        }
    } else {
        if (!is_selected(hit.item))
            m_sel = {hit.item};
        m_drag = Handle::Move;
    }

    m_drag_items.clear();
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            m_drag_items.push_back(*it);
    m_drag_bounds = selection_bounds();
    selection_changed();
    if (screen())
        screen()->redraw();
    return true;
}

bool ComposerView::mouse_drag_event(const Vector2i &p, const Vector2i &, int, int modifiers) {
    /* Screen hands drags to us relative to our parent and without the
     * panel's zoom. Its mouse_pos() still holds the previous event here, so
     * rebuild this event's screen point instead. */
    const Point pt = page_point_from_screen(p + parent()->absolute_position());
    const bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;
    if (m_selecting && editing()) {
        m_caret = text_pos_at(pt);
        m_goal_x = -1.f;
        if (screen())
            screen()->redraw();
        return true;
    }
    if (m_creating) {
        update_creation(pt, shift);
        return true;
    }
    if (m_marquee) {
        m_marquee_end = pt;
        return true;
    }
    if (!m_rotating && (m_drag == Handle::None || m_drag == Handle::TopTab))
        return true;
    if (!m_gesture_saved) {
        push_undo();                     // one undo step for the whole drag
        m_gesture_saved = true;
    }
    const Point d{pt.x - m_drag_start.x, pt.y - m_drag_start.y};

    if (m_rotating) {
        /* Turn the whole selection about the center of its bounds. */
        const Point c{(m_drag_bounds.x0 + m_drag_bounds.x1) * 0.5f,
                      (m_drag_bounds.y0 + m_drag_bounds.y1) * 0.5f};
        float delta = std::atan2(pt.y - c.y, pt.x - c.x) -
                      std::atan2(m_drag_start.y - c.y, m_drag_start.x - c.x);
        if (shift && !m_drag_items.empty()) {
            const float step = kPi / 12.f, r0 = m_drag_items.front().xf.rotation();
            delta = std::round((r0 + delta) / step) * step - r0;
        }
        for (const Item &o : m_drag_items)
            if (Item *it = m_doc.find_item(o.id))
                it->xf = Transform::rotate_about(o.xf, c, delta);
        selection_changed();
        return true;                     // turning doesn't change the composition
    }

    if (m_drag == Handle::Move) {
        const Point sd = snap_move(m_drag_bounds, d);
        for (const Item &o : m_drag_items)
            if (Item *it = m_doc.find_item(o.id))
                it->xf = Transform::translate(sd.x, sd.y) * o.xf;
        selection_changed();
        return true;                     // nor does moving
    }

    /* Resize one item in its own space: snap the dragged handle, move edges
     * of (0, 0)-(w, h), then fold the new origin into the transform. */
    auto orig = std::find_if(m_drag_items.begin(), m_drag_items.end(),
                             [&](const Item &i) { return i.id == m_drag_target; });
    Item *it = m_doc.find_item(m_drag_target);
    if (orig == m_drag_items.end() || !it)
        return true;
    const Item &o = *orig;
    const int hidx = m_drag == Handle::TopLeft ? 1 : m_drag == Handle::TopRight ? 2 :
                     m_drag == Handle::BottomLeft ? 3 : m_drag == Handle::BottomRight ? 4 : 5;
    const Point h0 = o.xf.apply(handle_point(o, hidx));
    const Point h1 = snap_point({h0.x + d.x, h0.y + d.y});
    const Point dl = o.xf.inverse().apply_vector({h1.x - h0.x, h1.y - h0.y});
    float x0 = 0, y0 = 0, x1 = o.w, y1 = o.h;
    if (is_line(o)) {
        if (m_drag == Handle::TopLeft)          { x0 += dl.x; y0 += dl.y; }
        else if (m_drag == Handle::BottomRight) { x1 += dl.x; y1 += dl.y; }
    } else {
        switch (m_drag) {
        case Handle::TopLeft:     x0 = std::min(x0 + dl.x, x1 - kMinBlock); y0 = std::min(y0 + dl.y, y1 - kMinBlock); break;
        case Handle::TopRight:    x1 = std::max(x1 + dl.x, x0 + kMinBlock); y0 = std::min(y0 + dl.y, y1 - kMinBlock); break;
        case Handle::BottomLeft:  x0 = std::min(x0 + dl.x, x1 - kMinBlock); y1 = std::max(y1 + dl.y, y0 + kMinBlock); break;
        case Handle::BottomRight: x1 = std::max(x1 + dl.x, x0 + kMinBlock); y1 = std::max(y1 + dl.y, y0 + kMinBlock); break;
        case Handle::Windowshade: y1 = std::max(y1 + dl.y, y0 + kMinBlock); break;
        default: break;
        }
    }
    it->w = x1 - x0;
    it->h = y1 - y0;
    it->xf = o.xf * Transform::translate(x0, y0);
    if (it->is_text())
        recompose();
    selection_changed();
    return true;
}

bool ComposerView::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button,
                                      int modifiers) {
    if (m_placing) {
        /* The picked-up story follows the pointer, text flowed in live.
         * Use the event point (the panel already applied the zoom); the
         * screen's mouse_pos still holds the previous event's point. */
        Vector2f v = Vector2f((float) p.x(), (float) p.y()) - Vector2f(m_pos) - page_origin();
        if (!m_sel.empty())
            if (Item *it = m_doc.find_item(m_sel.front()))
                it->xf = Transform::translate(v.x() - it->w * 0.5f, v.y() + 8.f);
        set_cursor(Cursor::Crosshair);
        return true;
    }
    /* The panel already applied the zoom to the event point. */
    const Vector2f ev = Vector2f((float) p.x(), (float) p.y()) - Vector2f(m_pos) - page_origin();
    const Point pt{ev.x(), ev.y()};
    switch (m_tool) {
    case Tool::Text:
        set_cursor(text_frame_at(pt) ? Cursor::IBeam : Cursor::Crosshair);
        break;
    case Tool::Pointer:
        switch (hit_test(pt).handle) {
        case Handle::TopLeft: case Handle::TopRight:
        case Handle::BottomLeft: case Handle::BottomRight:
            set_cursor(Cursor::HVResize); break;
        case Handle::Windowshade: set_cursor(Cursor::VResize); break;
        case Handle::TopTab: set_cursor(Cursor::Hand); break;
        default: set_cursor(Cursor::Arrow); break;
        }
        break;
    default:
        set_cursor(Cursor::Crosshair);   // rotate and the drawing tools
        break;
    }
    return Widget::mouse_motion_event(p, rel, button, modifiers);
}

bool ComposerView::focus_event(bool focused) {
    Widget::focus_event(focused);        // sets m_focused; the Screen gates on it
    if (screen())
        screen()->redraw();              // the caret comes and goes with focus
    return true;
}

/* ---- Drawing tools ------------------------------------------------------- */

void ComposerView::begin_creation(const Point &pt) {
    m_creation_saved = m_doc;            // the undo step, once something is drawn
    m_creation_version = m_version;
    const Point p = snap_point(pt);
    m_drag_start = p;
    if (m_tool == Tool::Text) {
        /* A new story: one empty "Body text" paragraph in the default type. */
        Paragraph para;
        para.style.name = "Body text";
        para.runs.push_back(Run{m_default_type, ""});
        Story s;
        s.paragraphs.push_back(para);
        StoryId sid = m_doc.add_story(std::move(s));
        m_created = m_doc.add_text_frame(m_page, sid, 0, 0, Transform::translate(p.x, p.y));
    } else {
        Item it;
        it.xf = Transform::translate(p.x, p.y);
        Shape sh = m_default_shape;
        sh.kind = m_tool == Tool::Line ? Shape::Kind::Line :
                  m_tool == Tool::Ellipse ? Shape::Kind::Ellipse :
                  m_tool == Tool::Polygon ? Shape::Kind::Polygon : Shape::Kind::Rect;
        it.content = sh;
        m_created = m_doc.add_item(m_page, it);
        m_sel = {m_created};
    }
    m_creating = true;
}

void ComposerView::update_creation(Point pt, bool constrain) {
    Item *it = m_doc.find_item(m_created);
    if (!it)
        return;
    const Point a = m_drag_start;
    Point b = snap_point(pt);
    float dx = b.x - a.x, dy = b.y - a.y;
    if (is_line(*it)) {
        if (constrain) {                 // 45 degree steps
            const float ang = std::round(std::atan2(dy, dx) / (kPi / 4)) * (kPi / 4);
            const float len = std::hypot(dx, dy);
            dx = len * std::cos(ang);
            dy = len * std::sin(ang);
        }
        it->xf = Transform::translate(a.x, a.y);
        it->w = dx;
        it->h = dy;
    } else {
        if (constrain) {                 // square, circle, regular polygon
            const float s = std::max(std::fabs(dx), std::fabs(dy));
            dx = std::copysign(s, dx);
            dy = std::copysign(s, dy);
        }
        it->xf = Transform::translate(std::min(a.x, a.x + dx), std::min(a.y, a.y + dy));
        it->w = std::fabs(dx);
        it->h = std::fabs(dy);
    }
    if (it->is_text())
        recompose();
    selection_changed();
}

void ComposerView::finish_creation() {
    m_creating = false;
    Item *it = m_doc.find_item(m_created);
    const float extent = it ? std::hypot(it->w, it->h) : 0.f;
    if (!it || extent < kMinDraw) {
        /* A click, not a drag: nothing was drawn, nothing to undo. */
        m_doc = m_creation_saved;
        m_created = 0;
        if (m_tool == Tool::Text)
            m_edit_story = 0;            // a click outside the text stops editing
        else
            m_sel.clear();
        recompose();
        selection_changed();
        if (screen())
            screen()->redraw();
        return;
    }
    push_snapshot(std::move(m_creation_saved), m_creation_version);
    if (it->is_text()) {
        it->w = std::max(it->w, kMinBlock);
        it->h = std::max(it->h, kMinBlock);
        if (const StoryEntry *se = m_doc.story_of(it->id)) {
            m_edit_story = se->id;       // ready to type into the new block
            m_caret = m_anchor = {};
        }
        m_sel.clear();
    }
    m_created = 0;
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Control palette ----------------------------------------------------- */

bool ComposerView::geometry(Geometry &g) const {
    if (m_sel.size() != 1)
        return false;
    const Item *it = m_doc.find_item(m_sel.front());
    if (!it)
        return false;
    const Point o = it->xf.apply({0, 0});
    g.x = o.x;
    g.y = o.y;
    g.w = it->w;
    g.h = it->h;
    float a = -it->xf.rotation() * 180.f / kPi;
    if (std::fabs(a) < 1e-4f)
        a = 0.f;                         // no "-0"
    g.angle = a;
    return true;
}

void ComposerView::set_geometry(const Geometry &g) {
    if (m_sel.size() != 1)
        return;
    Item *it = m_doc.find_item(m_sel.front());
    if (!it)
        return;
    push_undo();
    const float target = -g.angle * kPi / 180.f;
    const float delta = target - it->xf.rotation();
    if (std::fabs(delta) > 1e-6f)
        it->xf = Transform::rotate_about(it->xf, it->xf.apply({it->w * 0.5f, it->h * 0.5f}), delta);
    if (is_line(*it)) {
        it->w = g.w;
        it->h = g.h;
    } else {
        it->w = std::max(g.w, 1.f);
        it->h = std::max(g.h, 1.f);
    }
    const Point o = it->xf.apply({0, 0});
    it->xf = Transform::translate(g.x - o.x, g.y - o.y) * it->xf;
    if (it->is_text())
        recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

Shape ComposerView::shape_style() const {
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (const Shape *sh = it->shape())
                return *sh;
    return m_default_shape;
}

bool ComposerView::shapes_selected() const {
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (it->shape())
                return true;
    return false;
}

void ComposerView::apply_shape_style(const std::function<void(Shape &)> &fn) {
    if (shapes_selected()) {
        push_undo();
        for (ItemId id : m_sel)
            if (Item *it = m_doc.find_item(id))
                if (Shape *sh = it->shape())
                    fn(*sh);
    } else {
        fn(m_default_shape);             // nothing selected: set the defaults
    }
    selection_changed();
    if (screen())
        screen()->redraw();
}

bool ComposerView::type_style(TypeStyle &o) const {
    std::vector<CharStyle> styles;
    bool saw_frame = false;
    if (const Story *s = edit_story()) {
        if (has_selection())
            visit_styles(*s, m_anchor, m_caret, [&](const CharStyle &cs) { styles.push_back(cs); });
        else
            styles.push_back(m_typing_on ? m_typing : style_at(*s, m_caret));
    } else {
        for (ItemId id : m_sel) {
            size_t ti = 0;
            const StoryEntry *se = m_doc.story_of(id, &ti);
            const Item *it = m_doc.find_item(id);
            if (!se || !it || !it->is_text())
                continue;
            saw_frame = true;
            size_t si = m_doc.story_index(se->id);
            if (si >= m_comp.size())
                continue;
            for (const ComposedLine &l : m_comp[si].lines)
                if (l.frame == ti)
                    visit_styles(se->story, {l.para, l.byte_start}, {l.para, l.byte_end},
                                 [&](const CharStyle &cs) { styles.push_back(cs); });
        }
    }
    if (styles.empty()) {
        if (saw_frame)
            return false;
        styles.push_back(m_default_type);
    }
    const CharStyle &a = styles.front();
    o.family = a.family;
    o.bold = a.bold;
    o.italic = a.italic;
    o.size = a.size;
    o.leading = a.leading;
    o.baseline = a.baseline_shift;
    o.mix_family = o.mix_style = o.mix_size = o.mix_leading = o.mix_baseline = false;
    for (size_t i = 1; i < styles.size(); ++i) {
        const CharStyle &b = styles[i];
        o.mix_family |= b.family != a.family;
        o.mix_style |= b.bold != a.bold || b.italic != a.italic;
        o.mix_size |= b.size != a.size;
        o.mix_leading |= b.leading != a.leading;
        o.mix_baseline |= b.baseline_shift != a.baseline_shift;
    }
    return true;
}

void ComposerView::apply_type(const std::function<void(CharStyle &)> &fn) {
    if (Story *s = edit_story()) {
        if (has_selection()) {
            will_edit(false);
            restyle(*s, m_anchor, m_caret, fn);
            recompose();
            selection_changed();
            if (screen())
                screen()->redraw();
            return;
        }
        if (!m_typing_on) {
            m_typing = style_at(*s, m_caret);
            m_typing_on = true;
        }
        fn(m_typing);
        selection_changed();
        return;
    }
    struct Range { Story *story; TextPos a, b; };
    std::vector<Range> ranges;
    bool saw_frame = false;
    for (ItemId id : m_sel) {
        size_t ti = 0;
        StoryEntry *se = m_doc.story_of(id, &ti);
        const Item *it = m_doc.find_item(id);
        if (!se || !it || !it->is_text())
            continue;
        saw_frame = true;
        size_t si = m_doc.story_index(se->id);
        if (si >= m_comp.size())
            continue;
        for (const ComposedLine &l : m_comp[si].lines)
            if (l.frame == ti)
                ranges.push_back({&se->story, {l.para, l.byte_start}, {l.para, l.byte_end}});
    }
    if (ranges.empty()) {
        if (saw_frame)
            return;
        fn(m_default_type);
        selection_changed();
        return;
    }
    push_undo();
    for (Range &r : ranges)
        restyle(*r.story, r.a, r.b, fn);
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Threading --------------------------------------------------------- */

Frame ComposerView::measure_child_frame(const StoryEntry &s, size_t after, const Point &pt) const {
    const Item *parent = m_doc.find_item(s.thread[after]);
    const float w = parent ? parent->w : 200.f;
    /* Compose once with a very tall block after `after` and measure what
     * lands in it (every block starts at y = 0 in its own space). */
    std::vector<Frame> probe = m_doc.thread_frames(s);
    probe.insert(probe.begin() + (long) after + 1, Frame{0, 0, w, 1e6f});
    Composition c = compose(s.story, probe, *m_fonts, m_hyphenator);
    float h = kMinBlock;
    for (const ComposedLine &l : c.lines)
        if (l.frame == after + 1)
            h = std::max(h, l.top + l.leading);
    const PageSetup &ps = m_doc.setup;
    h = std::min(h, ps.height - ps.margin_top - ps.margin_bottom);
    return {pt.x - w * 0.5f, pt.y + 8.f, w, h};
}

void ComposerView::begin_placement(const Point &pt) {
    if (m_sel.size() != 1)
        return;
    size_t ti = 0;
    const StoryEntry *se = m_doc.story_of(m_sel.front(), &ti);
    if (!se)
        return;
    m_placement_saved = m_doc;           // becomes the undo step on commit
    m_placement_version = m_version;
    m_placement_parent = m_sel.front();
    const StoryId story = se->id;
    const Frame f = measure_child_frame(*se, ti, pt);
    size_t page = m_page;
    m_doc.find_item(m_placement_parent, &page);
    m_sel = {m_doc.add_text_frame(page, story, f.w, f.h, Transform::translate(f.x, f.y), ti + 1)};
    m_placing = true;
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::end_placement(bool commit) {
    if (!m_placing)
        return;
    m_placing = false;
    if (commit) {
        push_snapshot(std::move(m_placement_saved), m_placement_version);
    } else {
        m_doc = m_placement_saved;       // the text goes back where it was
        m_sel = {m_placement_parent};
    }
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Pointer tool -------------------------------------------------------- */

void ComposerView::delete_selection_items() {
    if (m_sel.empty())
        return;
    /* A threaded block's text flows on into the rest of its thread; the
     * story goes only with its last block. */
    push_undo();
    for (ItemId id : m_sel)
        m_doc.remove_item(id);
    m_sel.clear();
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::arrange(Stack how) {
    if (m_sel.empty())
        return;
    push_undo();
    /* Stacking order on the page, so a group keeps its own order. */
    std::vector<ItemId> order;
    for (const Item &it : items())
        if (is_selected(it.id))
            order.push_back(it.id);
    switch (how) {
    case Stack::Front:
        for (ItemId id : order)
            m_doc.bring_to_front(id);
        break;
    case Stack::Back:
        for (auto id = order.rbegin(); id != order.rend(); ++id)
            m_doc.send_to_back(*id);
        break;
    case Stack::Forward:  m_doc.restack(order, +1); break;
    case Stack::Backward: m_doc.restack(order, -1); break;
    }
    if (screen())
        screen()->redraw();
}

/* ---- Pages --------------------------------------------------------------- */

void ComposerView::page_changed() {
    m_sel.clear();
    m_edit_story = 0;
    m_typing_on = false;
    m_burst_open = false;
    m_selecting = false;
    selection_changed();
    if (on_page_change)
        on_page_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::show_page(size_t i) {
    if (m_doc.pages.empty())
        return;
    if (i >= m_doc.pages.size())
        i = m_doc.pages.size() - 1;
    if (i == m_page)
        return;
    end_placement(false);
    if (m_creating)
        finish_creation();
    m_page = i;
    page_changed();
}

void ComposerView::insert_page(bool after) {
    end_placement(false);
    if (m_creating)
        finish_creation();
    push_undo();
    size_t at = std::min(m_page + (after ? 1 : 0), m_doc.pages.size());
    m_page = m_doc.insert_page(at);
    page_changed();
}

void ComposerView::remove_page() {
    if (m_doc.pages.size() <= 1)
        return;
    end_placement(false);
    if (m_creating)
        finish_creation();
    push_undo();
    m_doc.remove_page(m_page);
    if (m_page >= m_doc.pages.size())
        m_page = m_doc.pages.size() - 1;
    recompose();
    page_changed();
}

void ComposerView::move_page_by(int delta) {
    if (delta == 0)
        return;
    long to = (long) m_page + delta;
    if (to < 0 || to >= (long) m_doc.pages.size())
        return;
    move_page_to((size_t) to, true);
}

void ComposerView::move_page_to(size_t index, bool record_undo) {
    if (index >= m_doc.pages.size() || index == m_page)
        return;
    if (record_undo)
        push_undo();
    m_doc.move_page(m_page, index);
    m_page = index;
    if (on_page_change)
        on_page_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::set_page_hidden(bool hidden) {
    if (m_page >= m_doc.pages.size() || m_doc.pages[m_page].hidden == hidden)
        return;
    push_undo();
    m_doc.pages[m_page].hidden = hidden;
    if (on_page_change)
        on_page_change();
    if (screen())
        screen()->redraw();
}

/* ---- Text tool: model glue ---------------------------------------------- */

Story *ComposerView::edit_story() {
    StoryEntry *se = m_doc.find_story(m_edit_story);
    return se ? &se->story : nullptr;
}

const Story *ComposerView::edit_story() const {
    const StoryEntry *se = m_doc.find_story(m_edit_story);
    return se ? &se->story : nullptr;
}

const Composition *ComposerView::edit_comp() const {
    size_t i = m_doc.story_index(m_edit_story);
    return i < m_comp.size() ? &m_comp[i] : nullptr;
}

TextPos ComposerView::text_pos_at(const Point &pt) const {
    const StoryEntry *se = m_doc.find_story(m_edit_story);
    const Composition *c = edit_comp();
    if (!se || !c || se->thread.empty())
        return {};
    /* The block under the point, else the story's nearest block, measured
     * in each block's own space. */
    size_t best = 0;
    float best_d = INFINITY;
    Point best_l;
    for (size_t i = 0; i < se->thread.size(); ++i) {
        const Item *it = m_doc.find_item(se->thread[i]);
        if (!it)
            continue;
        const Point l = it->xf.inverse().apply(pt);
        const float dx = std::max({-l.x, 0.f, l.x - it->w});
        const float dy = std::max({-l.y, 0.f, l.y - it->h});
        const float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = i;
            best_l = l;
        }
    }
    return hit_test_frame(*c, se->story, best, best_l.x, best_l.y);
}

void ComposerView::place_caret(TextPos p, bool extend) {
    Story *s = edit_story();
    if (!s)
        return;
    m_caret = clamp(*s, p);
    if (!extend)
        m_anchor = m_caret;
    m_typing_on = false;
}

void ComposerView::after_edit() {
    Story *s = edit_story();
    if (s) {
        m_caret = clamp(*s, m_caret);
        m_anchor = clamp(*s, m_anchor);
    }
    m_goal_x = -1.f;
    recompose();
    if (screen())
        screen()->redraw();
}

bool ComposerView::delete_selection() {
    Story *s = edit_story();
    if (!s || !has_selection())
        return false;
    m_caret = erase(*s, m_anchor, m_caret);
    m_anchor = m_caret;
    after_edit();
    return true;
}

void ComposerView::insert_string(const std::string &utf8, bool burst) {
    Story *s = edit_story();
    if (!s || utf8.empty())
        return;
    will_edit(burst);
    CharStyle st = m_typing_on ? m_typing : style_at(*s, m_caret);
    delete_selection();
    m_caret = insert_text(*s, m_caret, utf8, st);
    m_anchor = m_caret;
    after_edit();
}

void ComposerView::move_caret_key(int key, int modifiers) {
    Story *s = edit_story();
    const Composition *c = edit_comp();
    if (!s || !c)
        return;
    bool extend = (modifiers & GLFW_MOD_SHIFT) != 0;
    bool cmd = (modifiers & SYSTEM_COMMAND_MOD) != 0;
    bool word = (modifiers & GLFW_MOD_ALT) != 0;
    TextPos p = m_caret;

    switch (key) {
    case GLFW_KEY_LEFT:  p = word ? prev_word(*s, p) : prev_grapheme(*s, p); break;
    case GLFW_KEY_RIGHT: p = word ? next_word(*s, p) : next_grapheme(*s, p); break;
    case GLFW_KEY_HOME:
    case GLFW_KEY_END: {
        if (cmd) {
            p = key == GLFW_KEY_HOME ? TextPos{} : story_end(*s);
            break;
        }
        Caret at = caret_at(*c, *s, m_caret);
        if (!at.valid())
            return;
        p = key == GLFW_KEY_HOME ? line_start(*c, at.line) : line_end(*c, *s, at.line);
        break;
    }
    case GLFW_KEY_UP:
    case GLFW_KEY_DOWN: {
        Caret at = caret_at(*c, *s, m_caret);
        if (!at.valid())
            return;
        if (m_goal_x < 0.f)
            m_goal_x = at.x;
        long target = (long) at.line + (key == GLFW_KEY_UP ? -1 : 1);
        if (target < 0 || target >= (long) c->lines.size())
            return;
        p = pos_at_x(*c, *s, (size_t) target, m_goal_x);
        break;
    }
    default: return;
    }
    if (key != GLFW_KEY_UP && key != GLFW_KEY_DOWN)
        m_goal_x = -1.f;
    m_caret = p;
    if (!extend)
        m_anchor = p;
    m_typing_on = false;
    m_burst_open = false;                // a caret move ends the typing run
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::toggle_style(int key) {
    Story *s = edit_story();
    if (!s || !has_selection())
        return;
    will_edit(false);                    // one step per restyle
    /* Like nmail: set when any part of the selection is unset, else clear. */
    auto get = key == GLFW_KEY_B ? +[](const CharStyle &cs) { return cs.bold; }
                                 : +[](const CharStyle &cs) { return cs.italic; };
    bool all = true;
    restyle(*s, m_anchor, m_caret, [&](CharStyle &cs) { all &= get(cs); });
    restyle(*s, m_anchor, m_caret, [&](CharStyle &cs) {
        if (key == GLFW_KEY_B) cs.bold = !all; else cs.italic = !all;
    });
    recompose();
    if (screen())
        screen()->redraw();
}

/* ---- Keyboard ------------------------------------------------------------ */

bool ComposerView::keyboard_event(int key, int, int action, int modifiers) {
    if (m_placing) {
        if (key == GLFW_KEY_ESCAPE) {
            if (action == GLFW_PRESS)
                end_placement(false);
            return true;
        }
        return false;
    }
    bool cmd = (modifiers & SYSTEM_COMMAND_MOD) != 0;
    if (!editing()) {
        /* Item tools: Delete, arrow-key nudges, PageMaker's Arrange keys. */
        if (m_tool == Tool::Text || m_sel.empty())
            return false;
        if (action != GLFW_PRESS && action != GLFW_REPEAT)
            return false;
        if (!cmd && (key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE)) {
            delete_selection_items();
            return true;
        }
        if (cmd && (key == GLFW_KEY_F || key == GLFW_KEY_B ||
                    key == GLFW_KEY_LEFT_BRACKET || key == GLFW_KEY_RIGHT_BRACKET)) {
            const bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;
            Stack how = Stack::Front;
            if (key == GLFW_KEY_B || (key == GLFW_KEY_LEFT_BRACKET && shift))
                how = Stack::Back;
            else if (key == GLFW_KEY_LEFT_BRACKET)
                how = Stack::Backward;
            else if (key == GLFW_KEY_RIGHT_BRACKET && !shift)
                how = Stack::Forward;
            arrange(how);
            return true;
        }
        if (!cmd && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT ||
                     key == GLFW_KEY_UP || key == GLFW_KEY_DOWN)) {
            /* A run of nudges is one undo step, like a run of typing. */
            const float step = (modifiers & GLFW_MOD_SHIFT) ? 10.f : 1.f;
            const float dx = key == GLFW_KEY_LEFT ? -step : key == GLFW_KEY_RIGHT ? step : 0.f;
            const float dy = key == GLFW_KEY_UP ? -step : key == GLFW_KEY_DOWN ? step : 0.f;
            will_edit(true);
            for (ItemId id : m_sel)
                if (Item *it = m_doc.find_item(id))
                    it->xf = Transform::translate(dx, dy) * it->xf;
            selection_changed();
            if (screen())
                screen()->redraw();
            return true;
        }
        return false;
    }
    if (cmd) {
        switch (key) {
        case GLFW_KEY_A: case GLFW_KEY_C: case GLFW_KEY_X:
        case GLFW_KEY_V: case GLFW_KEY_B: case GLFW_KEY_I:
            if (action != GLFW_PRESS)
                return true;             // swallow the release
            break;
        default:
            return false;                // app-level zoom shortcuts etc.
        }
        Story *s = edit_story();
        GLFWwindow *win = screen() ? screen()->glfw_window() : nullptr;
        switch (key) {
        case GLFW_KEY_A:
            m_anchor = {};
            m_caret = story_end(*s);
            m_burst_open = false;
            if (screen()) screen()->redraw();
            return true;
        case GLFW_KEY_C:
            if (has_selection() && win) {
                std::string t = copy_text(*s, m_anchor, m_caret);
                glfwSetClipboardString(win, t.c_str());
            }
            return true;
        case GLFW_KEY_X:
            if (has_selection() && win) {
                will_edit(false);        // cut is its own step
                std::string t = copy_text(*s, m_anchor, m_caret);
                glfwSetClipboardString(win, t.c_str());
                delete_selection();
            }
            return true;
        case GLFW_KEY_V:
            if (win)
                if (const char *cb = glfwGetClipboardString(win))
                    insert_string(cb, false);   // paste is its own step
            return true;
        case GLFW_KEY_B: case GLFW_KEY_I:
            toggle_style(key);
            return true;
        }
        return true;
    }

    switch (key) {
    case GLFW_KEY_BACKSPACE: case GLFW_KEY_DELETE:
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
    case GLFW_KEY_LEFT: case GLFW_KEY_RIGHT:
    case GLFW_KEY_UP: case GLFW_KEY_DOWN:
    case GLFW_KEY_HOME: case GLFW_KEY_END:
    case GLFW_KEY_ESCAPE:
        break;
    default:
        return false;
    }
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return true;                     // swallow the release of handled keys

    Story *s = edit_story();
    switch (key) {
    case GLFW_KEY_ESCAPE:
        m_burst_open = false;
        if (has_selection()) {
            m_caret = m_anchor = m_caret < m_anchor ? m_caret : m_anchor;
            if (screen()) screen()->redraw();
        } else {
            set_tool(Tool::Pointer);
        }
        return true;
    case GLFW_KEY_BACKSPACE:
        if (has_selection() || m_caret != TextPos{}) {
            will_edit(true);             // a run of deletes is one step
            if (!delete_selection()) {
                TextPos p = prev_grapheme(*s, m_caret);
                m_caret = erase(*s, p, m_caret);
                m_anchor = m_caret;
                after_edit();
            }
        }
        return true;
    case GLFW_KEY_DELETE:
        if (has_selection() || m_caret != story_end(*s)) {
            will_edit(true);
            if (!delete_selection()) {
                TextPos p = next_grapheme(*s, m_caret);
                erase(*s, m_caret, p);
                after_edit();
            }
        }
        return true;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        will_edit(true);                 // Return joins the typing run
        delete_selection();
        if (modifiers & GLFW_MOD_SHIFT)
            insert_string("\xE2\x80\xA8", true);  // U+2028: break, same paragraph
        else {
            m_caret = split_paragraph(*s, m_caret);
            m_anchor = m_caret;
            after_edit();
        }
        return true;
    default:
        move_caret_key(key, modifiers);
        return true;
    }
}

bool ComposerView::keyboard_character_event(unsigned int codepoint) {
    if (!editing() || codepoint < 32)
        return false;
    char buf[5] = {0};
    if (codepoint < 0x80) {
        buf[0] = (char) codepoint;
    } else if (codepoint < 0x800) {
        buf[0] = (char) (0xC0 | (codepoint >> 6));
        buf[1] = (char) (0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        buf[0] = (char) (0xE0 | (codepoint >> 12));
        buf[1] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (char) (0x80 | (codepoint & 0x3F));
    } else {
        buf[0] = (char) (0xF0 | (codepoint >> 18));
        buf[1] = (char) (0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (char) (0x80 | (codepoint & 0x3F));
    }
    insert_string(buf, true);            // typing coalesces into one step
    return true;
}

/* ---- Drawing ----------------------------------------------------------- */

void ComposerView::draw_page(NVGcontext *ctx, float px) {
    const PageSetup &s = m_doc.setup;

    /* Classic PageMaker drop shadow: a solid offset slab. */
    nvgBeginPath(ctx);
    nvgRect(ctx, 4 * px, 4 * px, s.width, s.height);
    nvgFillColor(ctx, nvgRGBA(0, 0, 0, 110));
    nvgFill(ctx);

    nvgBeginPath(ctx);
    nvgRect(ctx, 0, 0, s.width, s.height);
    nvgFillColor(ctx, nvgRGB(255, 255, 255));
    nvgFill(ctx);
    nvgStrokeWidth(ctx, px);
    nvgStrokeColor(ctx, nvgRGB(0, 0, 0));
    nvgStroke(ctx);

    if (!m_show_guides)
        return;
    const float l = s.margin_inside, t = s.margin_top;
    const float r = s.width - s.margin_outside, b = s.height - s.margin_bottom;
    nvgBeginPath(ctx);
    nvgRect(ctx, l, t, r - l, b - t);
    nvgStrokeColor(ctx, kMarginGuide);
    nvgStroke(ctx);

    if (s.columns > 1) {
        const float col = (r - l - s.gutter * (s.columns - 1)) / s.columns;
        nvgBeginPath(ctx);
        for (int c = 1; c < s.columns; ++c) {
            float x = l + c * col + (c - 1) * s.gutter;
            nvgMoveTo(ctx, x, t);              nvgLineTo(ctx, x, b);
            nvgMoveTo(ctx, x + s.gutter, t);   nvgLineTo(ctx, x + s.gutter, b);
        }
        nvgStrokeColor(ctx, kColumnGuide);
        nvgStroke(ctx);
    }
}

void ComposerView::draw_text_overlays(NVGcontext *ctx, const Item &it, float px) {
    size_t ti = 0;
    const Composition *c = comp_of_frame(it.id, &ti);
    const StoryEntry *se = m_doc.story_of(it.id);
    if (!c || !se)
        return;
    const bool edited = se->id == m_edit_story;
    if (!m_show_loose_tight && !m_show_baselines && !edited)
        return;
    nvgSave(ctx);
    apply_transform(ctx, it.xf);
    const float lpx = px / item_scale(it.xf);

    for (const ComposedLine &l : c->lines) {
        if (l.frame != ti || !m_show_loose_tight || !(l.loose || l.tight))
            continue;
        nvgBeginPath(ctx);
        nvgRect(ctx, l.left, l.top, l.width, l.leading);
        nvgFillColor(ctx, l.tight ? nvgRGBA(255, 90, 90, 80) : nvgRGBA(255, 200, 0, 80));
        nvgFill(ctx);
    }
    if (m_show_baselines) {
        nvgBeginPath(ctx);
        for (const ComposedLine &l : c->lines)
            if (l.frame == ti) {
                nvgMoveTo(ctx, l.left, l.baseline);
                nvgLineTo(ctx, l.left + l.width, l.baseline);
            }
        nvgStrokeWidth(ctx, lpx);
        nvgStrokeColor(ctx, nvgRGBA(0, 170, 220, 160));
        nvgStroke(ctx);
    }

    /* The text selection is a translucent band over the type, so it shows
     * through shapes stacked above the block too. */
    if (edited && has_selection()) {
        TextPos lo = m_anchor, hi = m_caret;
        if (hi < lo)
            std::swap(lo, hi);
        for (const ComposedLine &l : c->lines) {
            if (l.frame != ti || l.para < lo.para || l.para > hi.para)
                continue;
            uint32_t from = l.byte_start, to = l.byte_end;
            if (l.para == lo.para)
                from = std::max(from, lo.byte);
            if (l.para == hi.para)
                to = std::min(to, hi.byte);
            if (from > to)
                continue;
            const std::string t = paragraph_text(se->story.paragraphs[l.para]);
            float x0 = x_at(l, t, from);
            float x1 = x_at(l, t, to);
            if (from == to) {
                /* An empty line inside the selection still shows a stub. */
                if (l.para >= hi.para)
                    continue;
                x1 = x0 + 6.f;
            } else if (to == l.byte_end && l.para_end && l.para < hi.para) {
                x1 += 6.f;               // the paragraph mark is selected too
            }
            nvgBeginPath(ctx);
            nvgRect(ctx, x0, l.top, std::max(1.f, x1 - x0), l.leading);
            nvgFillColor(ctx, nvgRGBA(60, 120, 255, 90));
            nvgFill(ctx);
        }
    } else if (edited && focused()) {
        Caret at = caret_at(*c, se->story, m_caret);   // invalid in overset text
        if (at.valid() && c->lines[at.line].frame == ti) {
            nvgBeginPath(ctx);
            nvgRect(ctx, at.x, at.top, std::max(lpx, 1.f / item_scale(it.xf)), at.bottom - at.top);
            nvgFillColor(ctx, nvgRGB(0, 0, 0));
            nvgFill(ctx);
        }
    }
    nvgRestore(ctx);
}

void ComposerView::draw_chrome(NVGcontext *ctx, float px) {
    enum Tab { Empty, Plus, Overset };
    float lpx = px;
    auto tab = [&](float cx, float cy, Tab kind) {
        const float w = kTabWPx * lpx, h = kTabHPx * lpx;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, cx - w * 0.5f, cy - h * 0.5f, w, h, 2 * lpx);
        nvgFillColor(ctx, nvgRGB(255, 255, 255));
        nvgFill(ctx);
        nvgStrokeWidth(ctx, lpx);
        nvgStrokeColor(ctx, nvgRGB(0, 0, 0));
        nvgStroke(ctx);
        if (kind == Plus) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, cx - 3 * lpx, cy); nvgLineTo(ctx, cx + 3 * lpx, cy);
            nvgMoveTo(ctx, cx, cy - 3 * lpx); nvgLineTo(ctx, cx, cy + 3 * lpx);
            nvgStroke(ctx);
        } else if (kind == Overset) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, cx - 4.5f * lpx, cy - 2.5f * lpx);
            nvgLineTo(ctx, cx + 4.5f * lpx, cy - 2.5f * lpx);
            nvgLineTo(ctx, cx, cy + 3 * lpx);
            nvgClosePath(ctx);
            nvgFillColor(ctx, kOversetRed);
            nvgFill(ctx);
        }
    };

    for (const Item &it : items()) {
        const bool selected = is_selected(it.id);
        nvgSave(ctx);
        apply_transform(ctx, it.xf);
        lpx = px / item_scale(it.xf);
        nvgStrokeWidth(ctx, lpx);

        if (it.is_text()) {
            size_t ti = 0;
            const StoryEntry *se = m_doc.story_of(it.id, &ti);
            const size_t si = se ? m_doc.story_index(se->id) : SIZE_MAX;
            const bool overset = si < m_comp.size() && m_comp[si].overset;
            const bool last = se && ti + 1 == se->thread.size();

            nvgBeginPath(ctx);
            nvgRect(ctx, 0, 0, it.w, it.h);
            nvgStrokeColor(ctx, kBlockOutline);
            nvgStroke(ctx);

            /* PageMaker only shows windowshades on the selected block; the
             * overset arrow is shown regardless so a short story is obvious. */
            if (selected) {
                nvgBeginPath(ctx);
                nvgMoveTo(ctx, 0, 0);     nvgLineTo(ctx, it.w, 0);
                nvgMoveTo(ctx, 0, it.h);  nvgLineTo(ctx, it.w, it.h);
                nvgStrokeColor(ctx, nvgRGB(0, 0, 0));
                nvgStroke(ctx);
                tab(it.w * 0.5f, 0, ti == 0 ? Empty : Plus);
            }
            if (selected || (last && overset))
                tab(it.w * 0.5f, it.h, last ? (overset ? Overset : Empty) : Plus);
        }

        if (selected) {
            const float hs = kHandlePx * lpx;
            nvgBeginPath(ctx);
            std::vector<Point> corners;
            if (is_line(it))
                corners = {{0, 0}, {it.w, it.h}};
            else
                corners = {{0, 0}, {it.w, 0}, {0, it.h}, {it.w, it.h}};
            for (const Point &c : corners)
                nvgRect(ctx, c.x - hs * 0.5f, c.y - hs * 0.5f, hs, hs);
            nvgFillColor(ctx, nvgRGB(0, 0, 0));
            nvgFill(ctx);
        }
        nvgRestore(ctx);
    }

    if (m_marquee) {
        Bounds b;
        b.add(m_drag_start);
        b.add(m_marquee_end);
        nvgBeginPath(ctx);
        nvgRect(ctx, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
        nvgFillColor(ctx, nvgRGBA(60, 120, 255, 30));
        nvgFill(ctx);
        nvgStrokeWidth(ctx, px);
        nvgStrokeColor(ctx, nvgRGBA(60, 120, 255, 200));
        nvgStroke(ctx);
    }
}

void ComposerView::draw(NVGcontext *ctx) {
    float xf[6];
    nvgCurrentTransform(ctx, xf);
    const float scale = std::sqrt(xf[0] * xf[0] + xf[1] * xf[1]);
    const float px = scale > 0 ? 1.f / scale : 1.f;   // one screen pixel, in points

    nvgSave(ctx);
    nvgTranslate(ctx, (float) m_pos.x(), (float) m_pos.y());

    /* The panel scissors to its viewport, so an oversized fill covers it. */
    nvgBeginPath(ctx);
    nvgRect(ctx, -1e5f, -1e5f, 2e5f, 2e5f);
    nvgFillColor(ctx, kPasteboardColor);
    nvgFill(ctx);

    Vector2f o = page_origin();
    nvgTranslate(ctx, o.x(), o.y());
    if (m_page < m_doc.pages.size() && m_doc.pages[m_page].hidden) {
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 11.f * px);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
        nvgFillColor(ctx, nvgRGB(140, 40, 40));
        nvgText(ctx, 0, -4.f * px, "Hidden \u2014 not printed", nullptr);
    }
    draw_page(ctx, px);
    draw_list(ctx, build_page(m_doc, m_page, m_comp), px);
    for (const Item &it : items())
        if (it.is_text())
            draw_text_overlays(ctx, it, px);
    draw_chrome(ctx, px);

    nvgRestore(ctx);
}
