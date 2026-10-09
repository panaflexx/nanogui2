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
constexpr float kHandlePx   = 7.f;      // handle sizes are in screen pixels
constexpr float kTabWPx     = 18.f, kTabHPx = 9.f;
constexpr float kPickPx     = 4.f;      // how near an outline or line counts as on it
constexpr float kPi         = 3.14159265f;

const NVGcolor kPasteboardColor = nvgRGB(214, 214, 214);
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

} // namespace

ComposerView::ComposerView(Widget *parent, const FontLibrary *fonts, const Hyphenator *hyphenator)
    : Widget(parent), m_fonts(fonts), m_hyphenator(hyphenator) {
    set_live(true);   // zoom-dependent hairlines; never bake into a parent's draw list
}

void ComposerView::set_document(PageDoc doc) {
    m_doc = std::move(doc);
    m_page = 0;
    m_sel = 0;
    m_edit_story = 0;
    m_selecting = false;
    m_undo.clear();
    m_redo.clear();
    m_burst_open = false;
    recompose();
}

void ComposerView::select_frame(size_t story, size_t frame) {
    if (story < m_doc.stories.size() && frame < m_doc.stories[story].thread.size())
        m_sel = m_doc.stories[story].thread[frame];
}

/* ---- Undo --------------------------------------------------------------- */

void ComposerView::push_undo() {
    m_undo.push_back(m_doc);
    if (m_undo.size() > 100)
        m_undo.pop_front();
    m_redo.clear();
}

void ComposerView::will_edit(bool burst) {
    if (!burst || !m_burst_open)
        push_undo();
    m_burst_open = burst;
}

void ComposerView::restore_snapshot() {
    m_burst_open = false;
    m_selecting = false;
    m_placing = false;
    m_rotating = false;
    m_drag = Handle::None;
    /* Keep the selection and caret if they still exist in the restored model. */
    if (m_sel && !m_doc.find_item(m_sel))
        m_sel = 0;
    if (m_edit_story && !m_doc.find_story(m_edit_story))
        m_edit_story = 0;
    if (Story *s = edit_story()) {
        m_caret = clamp(*s, m_caret);
        m_anchor = clamp(*s, m_anchor);
    }
    recompose();
    if (screen())
        screen()->redraw();
}

void ComposerView::undo() {
    if (m_undo.empty())
        return;
    m_redo.push_back(m_doc);
    m_doc = m_undo.back();
    m_undo.pop_back();
    restore_snapshot();
}

void ComposerView::redo() {
    if (m_redo.empty())
        return;
    m_undo.push_back(m_doc);
    m_doc = m_redo.back();
    m_redo.pop_back();
    restore_snapshot();
}

void ComposerView::set_tool(Tool t) {
    if (m_tool == t)
        return;
    end_placement(false);
    m_tool = t;
    m_burst_open = false;
    m_selecting = false;
    m_rotating = false;
    m_drag = Handle::None;
    if (t != Tool::Text)
        m_edit_story = 0;                // put the story down
    else
        m_sel = 0;                       // no item selection while editing text
    if (on_tool_change)
        on_tool_change(t);
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

/* ---- Hit testing (each item in its own space) ---------------------------- */

bool ComposerView::hits_item(const Item &it, const Point &pt) const {
    const Point l = it.xf.inverse().apply(pt);
    const float tol = kPickPx / (zoom() * item_scale(it.xf));
    const bool inside = l.x >= 0 && l.x <= it.w && l.y >= 0 && l.y <= it.h;
    const Shape *sh = it.shape();
    if (!sh)
        return inside;                   // text frames: anywhere in the block
    switch (sh->kind) {
    case Shape::Kind::Line:
        return dist_to_segment(l, {0, 0}, {it.w, it.h}) <= tol + sh->stroke_width * 0.5f;
    case Shape::Kind::Rect: {
        const bool near = l.x >= -tol && l.x <= it.w + tol && l.y >= -tol && l.y <= it.h + tol;
        if (sh->filled)
            return near;
        const bool deep = l.x > tol && l.x < it.w - tol && l.y > tol && l.y < it.h - tol;
        return near && !deep;            // an unfilled box is picked by its outline
    }
    case Shape::Kind::Ellipse: {
        const float rx = it.w * 0.5f, ry = it.h * 0.5f;
        if (rx <= 0 || ry <= 0)
            return false;
        const float nx = (l.x - rx) / rx, ny = (l.y - ry) / ry;
        const float d = std::sqrt(nx * nx + ny * ny);
        const float r = std::min(rx, ry);
        return sh->filled ? d <= 1.f + tol / r : std::fabs(d - 1.f) * r <= tol;
    }
    }
    return false;
}

ComposerView::Hit ComposerView::hit_test(const Point &pt) const {
    const float px = 1.f / zoom();

    if (const Item *it = m_doc.find_item(m_sel)) {
        const Point l = it->xf.inverse().apply(pt);
        const float s = item_scale(it->xf);
        const float hs = kHandlePx * px / s;
        auto near = [&](float x, float y) {
            return std::fabs(l.x - x) <= hs && std::fabs(l.y - y) <= hs;
        };
        Hit hit{m_sel, Handle::None};
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
    if (!down) {
        /* A click (no drag) on a windowshade tab is a threading gesture:
         * the red overset arrow picks the story up, a child's top tab
         * merges it back into the parent. */
        if (m_tool == Tool::Pointer && m_sel &&
            (m_drag == Handle::Windowshade || m_drag == Handle::TopTab)) {
            Point pt = page_point_from_screen(screen()->mouse_pos());
            float travel = std::fabs(pt.x - m_drag_start.x) + std::fabs(pt.y - m_drag_start.y);
            size_t ti = 0;
            const StoryEntry *se = m_doc.story_of(m_sel, &ti);
            if (travel <= 2.f && se) {
                const size_t si = m_doc.story_index(se->id);
                const bool last = ti + 1 == se->thread.size();
                if (m_drag == Handle::Windowshade && last && si < m_comp.size() &&
                    m_comp[si].overset) {
                    begin_placement(pt);
                } else if (m_drag == Handle::TopTab && ti > 0) {
                    push_undo();             // merge into the parent block
                    ItemId parent = se->thread[ti - 1];
                    m_doc.remove_item(m_sel);
                    m_sel = parent;
                    recompose();
                    if (screen())
                        screen()->redraw();
                }
            }
        }
        m_drag = Handle::None;
        m_selecting = false;
        m_rotating = false;
        return true;
    }
    request_focus();
    Point pt = page_point_from_screen(screen()->mouse_pos());

    if (m_tool == Tool::Text) {
        bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;
        double now = glfwGetTime();
        bool near = std::fabs(pt.x - m_down_pt.x) + std::fabs(pt.y - m_down_pt.y) <= 4.f;
        ItemId frame = text_frame_at(pt);
        const StoryEntry *se = frame ? m_doc.story_of(frame) : nullptr;
        const bool same_story = se && se->id == m_edit_story;
        m_burst_open = false;            // a click ends any typing run
        if (!shift && m_last_click >= 0.0 && now - m_last_click < 0.4 && near && same_story)
            m_clicks = std::min(m_clicks + 1, 3);
        else
            m_clicks = 1;
        m_last_click = now;
        m_down_pt = pt;
        m_goal_x = -1.f;

        if (!se) {
            m_edit_story = 0;            // clicked outside any text block: stop editing
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
        if (screen())
            screen()->redraw();
        return true;
    }

    m_edit_story = 0;
    Hit hit = hit_test(pt);
    m_sel = hit.item;
    m_drag = m_tool == Tool::Rotate ? Handle::None : hit.handle;
    m_rotating = m_tool == Tool::Rotate && hit.item != 0;
    m_drag_start = pt;
    m_gesture_saved = false;             // a new drag, not yet snapshotted
    if (const Item *it = m_doc.find_item(hit.item))
        m_drag_item = *it;
    if (screen())
        screen()->redraw();
    return true;
}

bool ComposerView::mouse_drag_event(const Vector2i &, const Vector2i &, int, int modifiers) {
    /* Screen hands drags to us in unzoomed coordinates; ask it where the
     * mouse really is. */
    const Point pt = page_point_from_screen(screen()->mouse_pos());
    if (m_selecting && editing()) {
        m_caret = text_pos_at(pt);
        m_goal_x = -1.f;
        if (screen())
            screen()->redraw();
        return true;
    }
    Item *it = m_doc.find_item(m_sel);
    if (!it || (!m_rotating && (m_drag == Handle::None || m_drag == Handle::TopTab)))
        return true;
    if (!m_gesture_saved) {
        push_undo();                     // one undo step for the whole drag
        m_gesture_saved = true;
    }
    const Item &o = m_drag_item;

    if (m_rotating) {
        const Point c = o.xf.apply({o.w * 0.5f, o.h * 0.5f});
        float delta = std::atan2(pt.y - c.y, pt.x - c.x) -
                      std::atan2(m_drag_start.y - c.y, m_drag_start.x - c.x);
        if (modifiers & GLFW_MOD_SHIFT) {
            const float step = kPi / 12.f, r0 = o.xf.rotation();
            delta = std::round((r0 + delta) / step) * step - r0;
        }
        it->xf = Transform::rotate_about(o.xf, c, delta);
        return true;                     // turning doesn't change the composition
    }

    const Point d{pt.x - m_drag_start.x, pt.y - m_drag_start.y};
    if (m_drag == Handle::Move) {
        it->xf = Transform::translate(d.x, d.y) * o.xf;
        return true;                     // nor does moving
    }

    /* Resize in the item's own space: move edges of (0, 0)-(w, h), then
     * fold the new origin into the transform. */
    const Point dl = o.xf.inverse().apply_vector(d);
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
    return true;
}

bool ComposerView::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button,
                                      int modifiers) {
    if (m_placing) {
        /* The picked-up story follows the pointer, text flowed in live.
         * Use the event point (the panel already applied the zoom); the
         * screen's mouse_pos still holds the previous event's point. */
        Vector2f v = Vector2f((float) p.x(), (float) p.y()) - Vector2f(m_pos) - page_origin();
        if (Item *it = m_doc.find_item(m_sel))
            it->xf = Transform::translate(v.x() - it->w * 0.5f, v.y() + 8.f);
        set_cursor(Cursor::Crosshair);
        return true;
    }
    const Point pt = page_point_from_screen(screen()->mouse_pos());
    if (m_tool == Tool::Text) {
        set_cursor(text_frame_at(pt) ? Cursor::IBeam : Cursor::Arrow);
        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }
    if (m_tool == Tool::Rotate) {
        set_cursor(Cursor::Crosshair);
        return Widget::mouse_motion_event(p, rel, button, modifiers);
    }
    switch (hit_test(pt).handle) {
    case Handle::TopLeft: case Handle::TopRight:
    case Handle::BottomLeft: case Handle::BottomRight:
        set_cursor(Cursor::HVResize); break;
    case Handle::Windowshade: set_cursor(Cursor::VResize); break;
    case Handle::TopTab: set_cursor(Cursor::Hand); break;
    default: set_cursor(Cursor::Arrow); break;
    }
    return Widget::mouse_motion_event(p, rel, button, modifiers);
}

bool ComposerView::focus_event(bool focused) {
    Widget::focus_event(focused);        // sets m_focused; the Screen gates on it
    if (screen())
        screen()->redraw();              // the caret comes and goes with focus
    return true;
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
    size_t ti = 0;
    const StoryEntry *se = m_doc.story_of(m_sel, &ti);
    if (!se)
        return;
    m_placement_saved = m_doc;           // becomes the undo step on commit
    m_placement_parent = m_sel;
    const StoryId story = se->id;
    const Frame f = measure_child_frame(*se, ti, pt);
    size_t page = m_page;
    m_doc.find_item(m_sel, &page);
    m_sel = m_doc.add_text_frame(page, story, f.w, f.h, Transform::translate(f.x, f.y), ti + 1);
    m_placing = true;
    recompose();
    if (screen())
        screen()->redraw();
}

void ComposerView::end_placement(bool commit) {
    if (!m_placing)
        return;
    m_placing = false;
    if (commit) {
        m_undo.push_back(m_placement_saved);
        if (m_undo.size() > 100)
            m_undo.pop_front();
        m_redo.clear();
    } else {
        m_doc = m_placement_saved;       // the text goes back where it was
        m_sel = m_placement_parent;
    }
    recompose();
    if (screen())
        screen()->redraw();
}

/* ---- Pointer tool -------------------------------------------------------- */

void ComposerView::delete_selected_item() {
    if (!m_doc.find_item(m_sel))
        return;
    /* A threaded block's text flows on into the rest of its thread; the
     * story goes only with its last block. */
    push_undo();
    m_doc.remove_item(m_sel);
    m_sel = 0;
    recompose();
    if (screen())
        screen()->redraw();
}

void ComposerView::arrange(bool to_front) {
    if (!m_doc.find_item(m_sel))
        return;
    push_undo();
    if (to_front)
        m_doc.bring_to_front(m_sel);
    else
        m_doc.send_to_back(m_sel);
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
    CharStyle st = style_at(*s, m_caret);
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
    m_burst_open = false;                // a caret move ends the typing run
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
        /* Pointer and rotate tools: Delete, and PageMaker's Arrange keys. */
        if (m_tool == Tool::Text || !m_sel || action != GLFW_PRESS)
            return false;
        if (!cmd && (key == GLFW_KEY_DELETE || key == GLFW_KEY_BACKSPACE)) {
            delete_selected_item();
            return true;
        }
        if (cmd && (key == GLFW_KEY_F || key == GLFW_KEY_B)) {
            arrange(key == GLFW_KEY_F);
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

void ComposerView::draw_text_frame(NVGcontext *ctx, const Item &it, float px) {
    size_t ti = 0;
    const Composition *c = comp_of_frame(it.id, &ti);
    const StoryEntry *se = m_doc.story_of(it.id);
    if (!c || !se)
        return;
    nvgSave(ctx);
    apply_transform(ctx, it.xf);
    const float lpx = px / item_scale(it.xf);

    for (const ComposedLine &l : c->lines) {
        if (l.frame != ti || !m_show_loose_tight || !(l.loose || l.tight))
            continue;
        nvgBeginPath(ctx);
        nvgRect(ctx, l.left, l.top, l.width, l.leading);
        nvgFillColor(ctx, l.tight ? nvgRGBA(255, 90, 90, 90) : nvgRGBA(255, 200, 0, 90));
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

    const bool edited = se->id == m_edit_story;
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
            nvgFillColor(ctx, nvgRGBA(70, 110, 180, 130));
            nvgFill(ctx);
        }
    }

    draw_frame_lines(ctx, *c, ti);

    if (edited && !has_selection() && focused()) {
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
        const bool selected = it.id == m_sel;
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
    draw_page(ctx, px);
    for (const Item &it : items()) {
        if (it.is_text())
            draw_text_frame(ctx, it, px);
        else
            draw_shape(ctx, it);
    }
    draw_chrome(ctx, px);

    nvgRestore(ctx);
}
