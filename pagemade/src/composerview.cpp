/*
 * pagemade/composerview.cpp — see composerview.h.
 */
#include "composerview.h"
#include "render_nvg.h"
#include "uri.h"

#include <nanogui/screen.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanovg.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "bg_worker.h"

using namespace nanogui;
using namespace pagemade;

namespace {

constexpr float kPasteboard = 216.f;    // pasteboard margin around the pages, points
constexpr float kMinBlock   = 12.f;     // smallest item, points
constexpr float kMinDraw    = 2.f;      // a drawing drag shorter than this is a click
constexpr float kHandlePx   = 7.f;      // handle sizes are in screen pixels
constexpr float kTabWPx     = 18.f, kTabHPx = 9.f;
constexpr float kPickPx     = 4.f;      // how near an outline or line counts as on it
constexpr float kSnapPx     = 5.f;      // how near a guide snaps
constexpr float kPi         = 3.14159265f;

const NVGcolor kPasteboardColor = nvgRGB(214, 214, 214);

size_t page_under(const std::vector<PageSlot> &slots, float x, float y,
                  float pw, float ph, size_t fallback) {
    for (const PageSlot &sl : slots)
        if (x >= sl.x && y >= sl.y && x <= sl.x + pw && y <= sl.y + ph)
            return sl.index;
    size_t best = fallback;
    float best_d = 1e30f;
    for (const PageSlot &sl : slots) {
        const float dx = x - (sl.x + pw * 0.5f);
        const float dy = y - (sl.y + ph * 0.5f);
        const float d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = sl.index;
        }
    }
    return best;
}

void arrangement(const PageDoc &doc, size_t current, PageView view,
                 std::vector<PageSlot> &slots, float &w, float &h) {
    arrange_pages(doc.setup, doc.pages.size(), current, view, slots, w, h);
}

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

ComposerView::~ComposerView() {
    m_load_gate->open.store(false);      // results still on their way are dropped
}

BackgroundWorker &ComposerView::loader() {
    if (!m_loader)
        m_loader = std::make_unique<BackgroundWorker>();
    return *m_loader;
}

BackgroundWorker &ComposerView::importer() {
    if (!m_importer)
        m_importer = std::make_unique<BackgroundWorker>();
    return *m_importer;
}

void ComposerView::set_document(PageDoc doc) {
    /* Reads and imports asked for by the previous document are dropped when
       they come back: its picture ids mean other pictures now. */
    ++m_doc_epoch;
    if (m_loader)
        m_loader->drop_if([](const std::string &) { return true; });
    m_load_want.clear();
    m_load_failed.clear();
    m_probing.clear();
    m_last_live.clear();
    m_doc = std::move(doc);
    m_shown_setup = m_doc.setup;
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
        m_sel = with_groups({id});
    selection_changed();
}

std::vector<ItemId> ComposerView::with_groups(const std::vector<ItemId> &ids) const {
    std::vector<ItemId> out;
    for (ItemId id : ids)
        for (ItemId m : m_doc.group_members(id))
            if (std::find(out.begin(), out.end(), m) == out.end())
                out.push_back(m);
    return out;
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
    if (continues_run())
        bump_version();                  // same undo step, but a changed document
    else
        push_snapshot(m_doc, m_version);
    m_merged_key = m_merge_key;
    m_merged_version = m_version;
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
    m_merged_key = nullptr;
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
    /* Keep a selection whose items still exist. If they all sit on one
     * page, follow them — a cross-page move undoes back to that page. */
    m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(),
                               [&](ItemId id) { return !m_doc.find_item(id); }),
                m_sel.end());
    drop_uneditable();                   // nor ones on a layer hidden or locked since
    size_t home = SIZE_MAX;
    bool one_page = !m_sel.empty();
    for (ItemId id : m_sel) {
        size_t pg = 0;
        m_doc.find_item(id, &pg);
        if (home == SIZE_MAX)
            home = pg;
        else if (pg != home)
            one_page = false;
    }
    if (one_page && home != SIZE_MAX && home != m_page)
        m_page = home;
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
    check_setup_change();                // undoing Document Setup resizes the pasteboard
    if (screen())
        screen()->redraw();
}

void ComposerView::check_setup_change() {
    const PageSetup &s = m_doc.setup, &o = m_shown_setup;
    const bool changed = s.width != o.width || s.height != o.height || s.facing != o.facing ||
                         s.bleed != o.bleed;
    m_shown_setup = s;
    if (!changed)
        return;
    if (on_setup_change)
        on_setup_change();
}

void ComposerView::change_setup(const PageSetup &setup, SetupAdjust how) {
    push_undo();
    pagemade::change_setup(m_doc, setup, how);
    recompose();
    selection_changed();
    check_setup_change();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::set_pdf_settings(const PdfSettings &settings) {
    m_doc.pdf = settings;
    bump_version();                      // saved with the file: it needs saving
}

/* Undo and redo stay on the page they were on, wherever it now sits (a
 * page move undoes back to the moved page). */
void ComposerView::follow_page(uint32_t uid) {
    for (size_t i = 0; i < m_doc.pages.size(); ++i)
        if (m_doc.pages[i].uid == uid) {
            m_page = i;
            return;
        }
}

void ComposerView::undo() {
    if (m_undo.empty())
        return;
    const uint32_t uid = page_uid(m_page);
    m_redo.push_back({std::move(m_doc), m_version});
    m_doc = std::move(m_undo.back().doc);
    m_version = m_undo.back().version;
    m_undo.pop_back();
    follow_page(uid);
    restore_snapshot();
    if (on_document_change)
        on_document_change();
}

void ComposerView::redo() {
    if (m_redo.empty())
        return;
    const uint32_t uid = page_uid(m_page);
    m_undo.push_back({std::move(m_doc), m_version});
    m_doc = std::move(m_redo.back().doc);
    m_version = m_redo.back().version;
    m_redo.pop_back();
    follow_page(uid);
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
     * viewport, so the pages sit a fixed pasteboard margin in. */
    return Vector2f(kPasteboard, kPasteboard);
}

Vector2i ComposerView::preferred_size(NVGcontext *) const {
    std::vector<PageSlot> slots;
    float w, h;
    arrangement(m_doc, m_page, m_page_view, slots, w, h);
    return Vector2i((int) std::ceil(w + 2 * kPasteboard),
                    (int) std::ceil(h + 2 * kPasteboard));
}

void ComposerView::set_page_view(PageView view) {
    if (m_page_view == view)
        return;
    m_page_view = view;
    if (screen()) {
        screen()->perform_layout();
        screen()->redraw();
    }
}

Vector2f ComposerView::page_slot_offset(size_t index) const {
    std::vector<PageSlot> slots;
    float w, h;
    arrangement(m_doc, m_page, m_page_view, slots, w, h);
    for (const PageSlot &sl : slots)
        if (sl.index == index)
            return {sl.x, sl.y};
    return {0.f, 0.f};
}

void ComposerView::fit_extent(float &width, float &height) const {
    if (m_page_view == PageView::Stack) {
        std::vector<PageSlot> slots;
        float aw, ah;
        arrangement(m_doc, m_page, m_page_view, slots, aw, ah);
        width = aw;              // a page, or a facing spread
        height = m_doc.setup.height > 1.f ? m_doc.setup.height : 1.f;
        return;
    }
    std::vector<PageSlot> slots;
    arrangement(m_doc, m_page, m_page_view, slots, width, height);
}

Vector2f ComposerView::fit_center() const {
    float w, h;
    fit_extent(w, h);
    if (m_page_view != PageView::Stack)
        return {w * 0.5f, h * 0.5f};
    /* The current page's row: the stack is one page or one spread wide. */
    return {w * 0.5f, page_slot_offset(m_page).y() + h * 0.5f};
}

float ComposerView::zoom() const {
    auto *zsp = dynamic_cast<const ZoomScrollPanel *>(parent());
    return zsp ? (float) zsp->zoom() : 1.f;
}

Vector2f ComposerView::local_from_screen(const Vector2i &screen_p) const {
    if (auto *zsp = dynamic_cast<const ZoomScrollPanel *>(parent())) {
        Vector2i in = screen_p - zsp->absolute_position();
        auto pan = zsp->pan_offset();
        double z = zsp->zoom() != 0.0 ? zsp->zoom() : 1.0;
        return Vector2f((float) ((in.x() - pan.x()) / z),
                        (float) ((in.y() - pan.y()) / z)) - Vector2f(m_pos);
    }
    return Vector2f(screen_p - absolute_position());
}

Point ComposerView::page_point_from_screen(const Vector2i &screen_p) const {
    return page_point_from_local(local_from_screen(screen_p));
}

Point ComposerView::page_point_from_local(const Vector2f &local_in) const {
    Vector2f local = local_in - page_origin();
    std::vector<PageSlot> slots;
    float aw, ah;
    arrangement(m_doc, m_page, m_page_view, slots, aw, ah);
    size_t page = m_page;
    if (m_pointer_down)
        page = m_gesture_page;
    else if (!m_placing)
        page = page_under(slots, local.x(), local.y(), m_doc.setup.width, m_doc.setup.height, m_page);
    if (!m_doc.pages.empty() && page >= m_doc.pages.size())
        page = m_doc.pages.size() - 1;
    m_point_page = page;
    float sx = 0.f, sy = 0.f;
    for (const PageSlot &sl : slots)
        if (sl.index == page) {
            sx = sl.x;
            sy = sl.y;
            break;
        }
    return {local.x() - sx, local.y() - sy};
}

/* ---- Snapping ----------------------------------------------------------- */

std::vector<float> ComposerView::guides_x(size_t page) const {
    std::vector<float> g = base_guides_x(page);
    auto add = [&](const std::vector<Guide> &gs) {
        for (const Guide &rg : gs)
            if (rg.vertical)
                g.push_back(rg.pos);
    };
    add(m_doc.guides);
    if (page < m_doc.pages.size())
        add(m_doc.pages[page].guides);
    return g;
}

std::vector<float> ComposerView::guides_y(size_t page) const {
    std::vector<float> g = base_guides_y();
    auto add = [&](const std::vector<Guide> &gs) {
        for (const Guide &rg : gs)
            if (!rg.vertical)
                g.push_back(rg.pos);
    };
    add(m_doc.guides);
    if (page < m_doc.pages.size())
        add(m_doc.pages[page].guides);
    return g;
}

std::vector<float> ComposerView::base_guides_x(size_t page) const {
    const PageSetup &s = m_doc.setup;
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const float l = left, r = s.width - right;
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

std::vector<float> ComposerView::base_guides_y() const {
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

/* ---- Ruler guides ------------------------------------------------------- */

namespace {

const NVGcolor kPageGuide = nvgRGB(0, 140, 255);      // this page: blue
const NVGcolor kAllPagesGuide = nvgRGB(150, 60, 220); // every page: purple

} // namespace

void ComposerView::set_show_guides(bool on) {
    m_show_guides = on;
    if (screen())
        screen()->redraw();
}

Point ComposerView::point_on_page(const Vector2i &screen_p, size_t page) const {
    const Vector2f local = local_from_screen(screen_p) - page_origin();
    std::vector<PageSlot> slots;
    float aw, ah;
    arrangement(m_doc, m_page, m_page_view, slots, aw, ah);
    for (const PageSlot &sl : slots)
        if (sl.index == page)
            return {local.x() - sl.x, local.y() - sl.y};
    return {local.x(), local.y()};
}

bool ComposerView::screen_over_pages(const Vector2i &screen_p) const {
    const Widget *view = parent() ? parent() : this;
    const Vector2i a = view->absolute_position(), s = view->size();
    return screen_p.x() >= a.x() && screen_p.y() >= a.y() &&
           screen_p.x() < a.x() + s.x() && screen_p.y() < a.y() + s.y();
}

bool ComposerView::ruler_mapping(Vector2f &origin, float &scale) const {
    std::vector<PageSlot> slots;
    float aw, ah;
    arrangement(m_doc, m_page, m_page_view, slots, aw, ah);
    Vector2f local = page_origin();
    for (const PageSlot &sl : slots)
        if (sl.index == m_page)
            local += Vector2f(sl.x, sl.y);
    if (auto *zsp = dynamic_cast<const ZoomScrollPanel *>(parent())) {
        const double z = zsp->zoom() != 0.0 ? zsp->zoom() : 1.0;
        const auto pan = zsp->pan_offset();
        origin = Vector2f(zsp->absolute_position()) + Vector2f((float) pan.x(), (float) pan.y()) +
                 (local + Vector2f(m_pos)) * (float) z;
        scale = (float) z;
    } else {
        origin = Vector2f(absolute_position()) + local;
        scale = 1.f;
    }
    return width() > 0 && height() > 0;
}

int ComposerView::guide_at(const Point &pt, size_t page, bool &all_pages) const {
    const float tol = kPickPx / zoom();
    auto find = [&](const std::vector<Guide> &gs) {
        for (int i = (int) gs.size() - 1; i >= 0; --i)
            if (std::fabs((gs[i].vertical ? pt.x : pt.y) - gs[i].pos) <= tol)
                return i;
        return -1;
    };
    if (page < m_doc.pages.size()) {     // a page's own guides lie on top
        int i = find(m_doc.pages[page].guides);
        if (i >= 0) {
            all_pages = false;
            return i;
        }
    }
    all_pages = true;
    return find(m_doc.guides);
}

void ComposerView::begin_guide_drag(bool vertical) {
    m_guide_drag = GuideDrag();
    m_guide_drag.active = true;
    m_guide_drag.vertical = vertical;
}

void ComposerView::guide_drag_to(const Vector2i &screen_p, bool all_pages) {
    GuideDrag &g = m_guide_drag;
    if (!g.active)
        return;
    g.over = screen_over_pages(screen_p);
    if (g.index < 0) {                   // a new guide: the page under the pointer
        g.all_pages = all_pages;
        (void) page_point_from_screen(screen_p);
        g.page = m_point_page;
    }
    const Point pt = point_on_page(screen_p, g.page);
    float pos = g.vertical ? pt.x : pt.y;
    if (m_snap) {                        // onto the page edges, margins and columns
        const std::vector<float> base = g.vertical ? base_guides_x(g.page) : base_guides_y();
        const float tol = kSnapPx / zoom();
        float best = tol;
        for (float b : base)
            if (std::fabs(b - pos) <= best) {
                best = std::fabs(b - pos);
                pos = b;
            }
    }
    g.pos = pos;
    if (screen())
        screen()->redraw();
}

void ComposerView::end_guide_drag(const Vector2i &screen_p, bool all_pages) {
    if (!m_guide_drag.active)
        return;
    guide_drag_to(screen_p, all_pages);
    const GuideDrag g = m_guide_drag;
    m_guide_drag = GuideDrag();
    if (g.page >= m_doc.pages.size() || (g.index < 0 && !g.over)) {
        if (screen())
            screen()->redraw();          // a new guide dropped off the pages: nothing
        return;
    }
    push_undo();
    std::vector<Guide> &list = g.all_pages ? m_doc.guides : m_doc.pages[g.page].guides;
    if (g.index < 0)
        list.push_back({g.vertical, g.pos});
    else if (g.index < (int) list.size()) {
        if (g.over)
            list[(size_t) g.index].pos = g.pos;
        else
            list.erase(list.begin() + g.index);   // dragged back onto a ruler
    }
    if (screen())
        screen()->redraw();
}

void ComposerView::clear_guides(bool all_pages) {
    if (all_pages ? m_doc.guides.empty()
                  : (m_page >= m_doc.pages.size() || m_doc.pages[m_page].guides.empty()))
        return;
    push_undo();
    if (all_pages)
        m_doc.guides.clear();
    else
        m_doc.pages[m_page].guides.clear();
    if (screen())
        screen()->redraw();
}

/* Every-page guides, then the page's own, over the items. The one being
 * dragged is drawn where it is going instead. */
void ComposerView::draw_ruler_guides(NVGcontext *ctx, float px, size_t page) {
    if (page >= m_doc.pages.size())
        return;
    const PageSetup &s = m_doc.setup;
    const GuideDrag &d = m_guide_drag;
    auto line = [&](const Guide &g, NVGcolor color) {
        nvgBeginPath(ctx);
        if (g.vertical) {
            nvgMoveTo(ctx, g.pos, 0);
            nvgLineTo(ctx, g.pos, s.height);
        } else {
            nvgMoveTo(ctx, 0, g.pos);
            nvgLineTo(ctx, s.width, g.pos);
        }
        nvgStrokeColor(ctx, color);
        nvgStrokeWidth(ctx, px);
        nvgStroke(ctx);
    };
    auto list = [&](const std::vector<Guide> &gs, bool all_pages, NVGcolor color) {
        for (size_t i = 0; i < gs.size(); ++i) {
            const bool moving = d.active && d.index == (int) i && d.all_pages == all_pages &&
                                (all_pages || d.page == page);
            if (!moving)
                line(gs[i], color);
        }
    };
    nvgSave(ctx);
    if (m_show_guides) {
        list(m_doc.guides, true, kAllPagesGuide);
        list(m_doc.pages[page].guides, false, kPageGuide);
    }
    if (d.active && d.over && (d.all_pages || d.page == page))
        line({d.vertical, d.pos}, d.all_pages ? kAllPagesGuide : kPageGuide);
    nvgRestore(ctx);
}

/* ---- Hit testing (each item in its own space) ---------------------------- */

bool ComposerView::hits_item(const Item &it, const Point &pt) const {
    if (!m_doc.item_editable(it))
        return false;                    // hidden or locked layer
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

std::map<uint32_t, Bounds> ComposerView::selected_groups(size_t page) const {
    std::map<uint32_t, Bounds> out;
    if (page >= m_doc.pages.size())
        return out;
    std::map<uint32_t, bool> whole;
    for (const Item &it : m_doc.pages[page].items) {
        if (!it.group)
            continue;
        const bool sel = is_selected(it.id);
        auto w = whole.find(it.group);
        whole[it.group] = (w == whole.end() ? true : w->second) && sel;
        out[it.group].add(item_bounds(it));
    }
    for (const auto &w : whole)
        if (!w.second)
            out.erase(w.first);
    return out;
}

ComposerView::Hit ComposerView::hit_test(const Point &pt, size_t page) const {
    const float px = 1.f / zoom();

    /* A selected group resizes from the corners of its box */
    {
        const float hs = kHandlePx * px;
        for (const auto &g : selected_groups(page)) {
            const Bounds &b = g.second;
            auto near = [&](float x, float y) {
                return std::fabs(pt.x - x) <= hs && std::fabs(pt.y - y) <= hs;
            };
            Handle h = near(b.x0, b.y0) ? Handle::TopLeft : near(b.x1, b.y0) ? Handle::TopRight
                     : near(b.x0, b.y1) ? Handle::BottomLeft : near(b.x1, b.y1) ? Handle::BottomRight
                     : Handle::None;
            if (h != Handle::None)
                for (const Item &it : m_doc.pages[page].items)
                    if (it.group == g.first)
                        return {it.id, h, g.first};
        }
    }

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
        if (it->group) {
            /* Its group's box has the corners */
        } else if (is_line(*it)) {
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
    static const std::vector<Item> none;
    const std::vector<Item> &its =
        page < m_doc.pages.size() ? static_cast<const std::vector<Item> &>(m_doc.pages[page].items) : none;
    for (auto it = its.rbegin(); it != its.rend(); ++it)
        if (hits_item(*it, pt))
            return {it->id, Handle::Move};
    return {};
}

ItemId ComposerView::text_frame_at(const Point &pt, size_t page) const {
    static const std::vector<Item> none;
    const std::vector<Item> &its =
        page < m_doc.pages.size() ? static_cast<const std::vector<Item> &>(m_doc.pages[page].items) : none;
    for (auto it = its.rbegin(); it != its.rend(); ++it)
        if (it->is_text() && hits_item(*it, pt))
            return it->id;
    return 0;
}

/* ---- Mouse --------------------------------------------------------------- */

/* A modifier as it is now. The screen refreshes the modifiers it passes
 * along only on clicks, so one pressed mid-drag is read from the keyboard. */
bool ComposerView::key_held(int modifiers, int mod, int key_left, int key_right) const {
    if (modifiers & mod)
        return true;
    const Screen *s = screen();
    GLFWwindow *win = s ? s->glfw_window() : nullptr;
    return win && (glfwGetKey(win, key_left) == GLFW_PRESS ||
                   glfwGetKey(win, key_right) == GLFW_PRESS);
}

bool ComposerView::mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) {
    if (m_placing) {
        if (down)
            end_placement(button == GLFW_MOUSE_BUTTON_1);
        return true;
    }
    if (button == GLFW_MOUSE_BUTTON_2) {
        /* The right-click menu: on what is under the pointer */
        if (!down || !on_context_menu)
            return true;
        if (m_point_page != m_page)
            show_page(m_point_page);
        const Point at = page_point_from_screen(screen()->mouse_pos());
        m_context_pt = at;
        if (!editing()) {
            const Hit hit = hit_test(at, m_page);
            if (!hit.item)
                m_sel.clear();
            else if (!is_selected(hit.item))
                m_sel = with_groups({hit.item});
            selection_changed();
            if (screen())
                screen()->redraw();
        }
        request_focus();
        on_context_menu(screen()->mouse_pos());
        return true;
    }
    if (button != GLFW_MOUSE_BUTTON_1)
        return Widget::mouse_button_event(p, button, down, modifiers);
    if (!down && m_guide_drag.active) {
        /* A guide moved with the pointer tool (a ruler's own drag ends on
           the ruler, before this) */
        m_pointer_down = false;
        end_guide_drag(screen()->mouse_pos(), false);
        return true;
    }
    const Point pt = page_point_from_screen(screen()->mouse_pos());

    if (!down) {
        m_pointer_down = false;
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
                if (box.contains(item_bounds(it)) && !is_selected(it.id) &&
                    m_doc.item_editable(it))
                    m_sel.push_back(it.id);
            m_sel = with_groups(m_sel);  // a group comes whole
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
        if (m_drag == Handle::Move && m_gesture_saved && m_tool != Tool::Crop)
            adopt_moved_items();
        m_drag = Handle::None;
        m_drag_target = 0;
        m_selecting = false;
        m_rotating = false;
        return true;
    }
    request_focus();
    m_gesture_page = m_point_page;
    m_pointer_down = true;
    if (m_gesture_page != m_page)
        show_page(m_gesture_page);
    m_burst_open = false;                // any click ends a typing or nudging run
    m_merged_key = nullptr;              // ... or a palette spin
    const bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;

    if (m_tool == Tool::Text) {
        double now = glfwGetTime();
        bool near = std::fabs(pt.x - m_down_pt.x) + std::fabs(pt.y - m_down_pt.y) <= 4.f;
        ItemId frame = text_frame_at(pt, m_page);
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
    m_drag_group = 0;

    if (m_tool == Tool::Line || m_tool == Tool::Rect || m_tool == Tool::Ellipse ||
        m_tool == Tool::Polygon) {
        begin_creation(pt);
        return true;
    }

    Hit hit = hit_test(pt, m_page);
    bool all_pages = false;
    const int guide = m_tool == Tool::Pointer && !hit.item && m_show_guides && !m_lock_guides
                      ? guide_at(pt, m_page, all_pages) : -1;
    if (guide >= 0) {
        /* Pick up a ruler guide: it moves along its axis, and dropped off
           the pages it goes */
        const std::vector<Guide> &gs = all_pages ? m_doc.guides : m_doc.pages[m_page].guides;
        m_guide_drag = GuideDrag();
        m_guide_drag.active = true;
        m_guide_drag.vertical = gs[(size_t) guide].vertical;
        m_guide_drag.all_pages = all_pages;
        m_guide_drag.index = guide;
        m_guide_drag.page = m_page;
        m_guide_drag.pos = gs[(size_t) guide].pos;
        m_guide_drag.over = true;
        return true;
    }
    if (m_tool == Tool::Rotate) {
        if (hit.item && !is_selected(hit.item))
            m_sel = with_groups({hit.item});
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
        m_drag = hit.handle;             // a handle of a selected item (or group)
        m_drag_target = hit.item;
        m_drag_group = hit.group;
    } else if (shift) {
        /* Shift-click adds or takes away the item's whole group */
        const std::vector<ItemId> members = m_doc.group_members(hit.item);
        if (is_selected(hit.item)) {
            for (ItemId m : members)
                m_sel.erase(std::remove(m_sel.begin(), m_sel.end(), m), m_sel.end());
        } else {
            for (ItemId m : members)
                if (!is_selected(m))
                    m_sel.push_back(m);
            m_drag = Handle::Move;
        }
    } else {
        if (!is_selected(hit.item))
            m_sel = with_groups({hit.item});
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
    if (m_guide_drag.active) {
        guide_drag_to(p + parent()->absolute_position(), m_guide_drag.all_pages);
        return true;
    }
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
        /* Crop slides the picture inside the frame. The frame stays. */
        if (m_tool == Tool::Crop) {
            bool panned = false;
            for (const Item &o : m_drag_items) {
                Item *it = m_doc.find_item(o.id);
                if (!it || !o.image() || !it->image())
                    continue;
                Point dl = o.xf.inverse().apply_vector(d);
                if (shift) {
                    if (std::fabs(dl.x) >= std::fabs(dl.y)) dl.y = 0;
                    else dl.x = 0;
                }
                PlacedImage im = *o.image();
                im.x += dl.x;
                im.y += dl.y;
                it->content = im;
                panned = true;
            }
            if (panned) {
                selection_changed();
                return true;
            }
        }
        const Point sd = snap_move(m_drag_bounds, d);
        for (const Item &o : m_drag_items)
            if (Item *it = m_doc.find_item(o.id))
                it->xf = Transform::translate(sd.x, sd.y) * o.xf;
        selection_changed();
        return true;                     // nor does moving
    }

    if (m_drag_group) {
        resize_group(d, modifiers);
        return true;
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
        /* A picture's corners keep its proportions, by whichever side moved
         * further, about the opposite corner; Ctrl resizes freely. */
        const bool corner = m_drag != Handle::Windowshade;
        if (o.image() && m_tool == Tool::Pointer && corner && o.w > 0 && o.h > 0 &&
            !key_held(modifiers, GLFW_MOD_CONTROL, GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL)) {
            const float s = std::max({(x1 - x0) / o.w, (y1 - y0) / o.h,
                                      kMinBlock / std::min(o.w, o.h)});
            const float w = o.w * s, h = o.h * s;
            const bool left = m_drag == Handle::TopLeft || m_drag == Handle::BottomLeft;
            const bool top = m_drag == Handle::TopLeft || m_drag == Handle::TopRight;
            if (left) x0 = x1 - w; else x1 = x0 + w;
            if (top)  y0 = y1 - h; else y1 = y0 + h;
        }
    }
    it->w = x1 - x0;
    it->h = y1 - y0;
    it->xf = o.xf * Transform::translate(x0, y0);
    if (const PlacedImage *im = o.image()) {
        /* Crop moves the frame and leaves the picture on the page.
         * The pointer scales the picture with the frame. */
        if (m_tool == Tool::Crop)
            it->content = crop_placement(*im, x0, y0);
        else
            it->content = scale_placement(*im, o.w, o.h, it->w, it->h);
    }
    if (it->is_text())
        recompose();
    selection_changed();
    return true;
}

/* The group's box, as it was at the press, goes from its fixed corner to
 * the dragged one. Every member's origin moves with the box; its width and
 * height scale by how much the box stretched along the item's own axes,
 * so a turned item keeps its angle. Pictures scale with their frames and
 * text reflows in its new frame (the type keeps its size). As with a
 * picture, the box keeps its proportions unless Ctrl is held. */
void ComposerView::resize_group(Point d, int modifiers) {
    Bounds b;
    for (const Item &o : m_drag_items)
        if (o.group == m_drag_group)
            b.add(item_bounds(o));
    if (b.empty() || b.x1 - b.x0 <= 0.f || b.y1 - b.y0 <= 0.f)
        return;
    const bool left = m_drag == Handle::TopLeft || m_drag == Handle::BottomLeft;
    const bool top = m_drag == Handle::TopLeft || m_drag == Handle::TopRight;
    const Point c0{left ? b.x0 : b.x1, top ? b.y0 : b.y1};
    const Point c1 = snap_point({c0.x + d.x, c0.y + d.y});
    const float w0 = b.x1 - b.x0, h0 = b.y1 - b.y0;
    float sx = (left ? b.x1 - std::min(c1.x, b.x1 - kMinBlock)
                     : std::max(c1.x, b.x0 + kMinBlock) - b.x0) / w0;
    float sy = (top ? b.y1 - std::min(c1.y, b.y1 - kMinBlock)
                    : std::max(c1.y, b.y0 + kMinBlock) - b.y0) / h0;
    if (!key_held(modifiers, GLFW_MOD_CONTROL, GLFW_KEY_LEFT_CONTROL, GLFW_KEY_RIGHT_CONTROL))
        sx = sy = std::max(sx, sy);
    /* The fixed corner stays put */
    const float nx0 = left ? b.x1 - w0 * sx : b.x0;
    const float ny0 = top ? b.y1 - h0 * sy : b.y0;
    auto map = [&](Point p) { return Point{nx0 + (p.x - b.x0) * sx, ny0 + (p.y - b.y0) * sy}; };
    auto stretch = [&](Point v) {
        const float len = std::hypot(v.x, v.y);
        return len > 0.f ? std::hypot(v.x * sx, v.y * sy) / len : 1.f;
    };

    bool text = false;
    for (const Item &o : m_drag_items) {
        if (o.group != m_drag_group)
            continue;
        Item *it = m_doc.find_item(o.id);
        if (!it)
            continue;
        it->w = o.w * stretch(o.xf.apply_vector({1.f, 0.f}));
        it->h = o.h * stretch(o.xf.apply_vector({0.f, 1.f}));
        const Point origin = map(o.xf.apply({0.f, 0.f}));
        it->xf = o.xf;
        it->xf.e = origin.x;
        it->xf.f = origin.y;
        if (const PlacedImage *im = o.image())
            it->content = scale_placement(*im, o.w, o.h, it->w, it->h);
        text = text || it->is_text();
    }
    if (text)
        recompose();
    selection_changed();
}

bool ComposerView::mouse_motion_event(const Vector2i &p, const Vector2i &rel, int button,
                                      int modifiers) {
    if (m_placing) {
        /* The picked-up story follows the pointer, text flowed in live.
         * Use the event point (the panel already applied the zoom); the
         * screen's mouse_pos still holds the previous event's point. */
        const Point at = page_point_from_local(Vector2f((float) p.x(), (float) p.y()) - Vector2f(m_pos));
        if (!m_sel.empty())
            if (Item *it = m_doc.find_item(m_sel.front()))
                it->xf = Transform::translate(at.x - it->w * 0.5f, at.y + 8.f);
        set_cursor(Cursor::Crosshair);
        return true;
    }
    /* The panel already applied the zoom to the event point. */
    const Point pt = page_point_from_local(Vector2f((float) p.x(), (float) p.y()) - Vector2f(m_pos));
    const size_t page = m_point_page;
    switch (m_tool) {
    case Tool::Text:
        set_cursor(text_frame_at(pt, page) ? Cursor::IBeam : Cursor::Crosshair);
        break;
    case Tool::Pointer:
        switch (hit_test(pt, page).handle) {
        case Handle::TopLeft: case Handle::TopRight:
        case Handle::BottomLeft: case Handle::BottomRight:
            set_cursor(Cursor::HVResize); break;
        case Handle::Windowshade: set_cursor(Cursor::VResize); break;
        case Handle::TopTab: set_cursor(Cursor::Hand); break;
        case Handle::None: {
            /* Over a ruler guide (and no item): it can be dragged */
            bool all_pages = false;
            const int g = m_show_guides && !m_lock_guides ? guide_at(pt, page, all_pages) : -1;
            if (g >= 0) {
                const std::vector<Guide> &gs = all_pages ? m_doc.guides : m_doc.pages[page].guides;
                set_cursor(gs[(size_t) g].vertical ? Cursor::HResize : Cursor::VResize);
            } else {
                set_cursor(Cursor::Arrow);
            }
            break;
        }
        default: set_cursor(Cursor::Arrow); break;
        }
        break;
    case Tool::Crop: {
        Hit hit = hit_test(pt, page);
        switch (hit.handle) {
        case Handle::TopLeft: case Handle::TopRight:
        case Handle::BottomLeft: case Handle::BottomRight:
            set_cursor(Cursor::HVResize); break;
        case Handle::Move:
            if (const Item *it = m_doc.find_item(hit.item))
                if (it->is_image()) { set_cursor(Cursor::Hand); break; }
            set_cursor(Cursor::Arrow); break;
        default:
            set_cursor(Cursor::Arrow); break;
        }
        break;
    }
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

bool ComposerView::check_active_layer() {
    if (m_doc.active_layer_editable())
        return true;
    const Layer *l = m_doc.find_layer(m_doc.active_layer);
    if (on_note)
        on_note("Layer \"" + (l ? l->name : std::string()) + "\" is " +
                (l && !l->visible ? "hidden" : "locked") + ": choose another layer to add to");
    return false;
}

void ComposerView::begin_creation(const Point &pt) {
    if (!check_active_layer())
        return;
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
    const float ow = it->w, oh = it->h;
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
    if (const PlacedImage *im = it->image())
        it->content = scale_placement(*im, ow, oh, it->w, it->h);
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
        if (const Item *it = m_doc.find_item(id)) {
            if (const Shape *sh = it->shape())
                return *sh;
            if (const PlacedImage *im = it->image()) {
                Shape look = m_default_shape;
                look.fill = {kNoPaint, im->opacity};
                look.stroke = im->stroke;
                return look;
            }
        }
    return m_default_shape;
}

bool ComposerView::shapes_selected() const {
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (it->shape())
                return true;
    return false;
}

bool ComposerView::pictures_selected() const {
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (it->image())
                return true;
    return false;
}

void ComposerView::apply_shape_style(const std::function<void(Shape &)> &fn) {
    apply_item_style(fn, nullptr);
}

void ComposerView::apply_item_style(const std::function<void(Shape &)> &shape,
                                    const std::function<void(PlacedImage &)> &image) {
    const bool pictures = image && pictures_selected();
    if (shapes_selected() || pictures) {
        push_undo();
        for (ItemId id : m_sel)
            if (Item *it = m_doc.find_item(id)) {
                if (Shape *sh = it->shape())
                    shape(*sh);
                else if (PlacedImage *im = it->image(); im && image)
                    image(*im);
            }
    } else if (!pictures_selected()) {
        shape(m_default_shape);          // nothing selected: set the defaults
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
    o.face = a.face;
    o.bold = a.bold;
    o.italic = a.italic;
    o.caps = a.caps;
    o.underline = a.underline;
    o.strike = a.strike;
    o.kerning = a.kerning;
    o.ligatures = a.ligatures;
    o.no_break = a.no_break;
    o.size = a.size;
    o.leading = a.leading;
    o.baseline = a.baseline_shift;
    o.tracking = a.tracking;
    o.hscale = a.hscale;
    o.mix_family = o.mix_style = o.mix_size = o.mix_leading = o.mix_baseline = false;
    o.mix_caps = o.mix_deco = o.mix_track = o.mix_hscale = false;
    o.mix_kerning = o.mix_ligatures = o.mix_no_break = false;
    for (size_t i = 1; i < styles.size(); ++i) {
        const CharStyle &b = styles[i];
        o.mix_family |= b.family != a.family;
        o.mix_style |= b.face != a.face || b.bold != a.bold || b.italic != a.italic;
        o.mix_size |= b.size != a.size;
        o.mix_leading |= b.leading != a.leading;
        o.mix_baseline |= b.baseline_shift != a.baseline_shift;
        o.mix_caps |= b.caps != a.caps;
        o.mix_deco |= b.underline != a.underline || b.strike != a.strike;
        o.mix_track |= b.tracking != a.tracking;
        o.mix_hscale |= b.hscale != a.hscale;
        o.mix_kerning |= b.kerning != a.kerning;
        o.mix_ligatures |= b.ligatures != a.ligatures;
        o.mix_no_break |= b.no_break != a.no_break;
        o.mix_color |= b.swatch != a.swatch || !(b.color == a.color);
        o.mix_tint |= b.tint != a.tint;
        o.mix_char_style |= b.char_style != a.char_style;
    }
    o.swatch = a.swatch;
    o.tint = a.tint;
    o.rgb = a.color;
    o.char_style = a.char_style;
    o.char_overridden = false;
    if (!o.mix_char_style && !a.char_style.empty())
        if (const CharStyleDef *d = m_doc.find_char_style(a.char_style))
            for (const CharStyle &cs : styles)
                o.char_overridden |= char_style_overridden(cs, *d);
    return true;
}

bool ComposerView::selection_char_style(CharStyle &cs, CharStyle &base) const {
    const Paragraph *para = nullptr;
    if (const Story *s = edit_story()) {
        const TextPos a = m_anchor < m_caret ? m_anchor : m_caret;
        if (a.para >= s->paragraphs.size())
            return false;
        para = &s->paragraphs[a.para];
        if (has_selection()) {
            /* The first selected character: the run holding byte a. */
            uint32_t at = 0;
            cs = para->runs.empty() ? CharStyle() : para->runs.back().style;
            for (const Run &r : para->runs) {
                if (a.byte < at + (uint32_t) r.text.size()) {
                    cs = r.style;
                    break;
                }
                at += (uint32_t) r.text.size();
            }
        } else {
            cs = m_typing_on ? m_typing : style_at(*s, m_caret);
        }
    } else {
        for (ItemId id : m_sel) {
            size_t ti = 0;
            const StoryEntry *se = m_doc.story_of(id, &ti);
            const size_t si = se ? m_doc.story_index(se->id) : SIZE_MAX;
            if (si >= m_comp.size())
                continue;
            for (const ComposedLine &l : m_comp[si].lines)
                if (l.frame == ti && l.para < se->story.paragraphs.size()) {
                    para = &se->story.paragraphs[l.para];
                    cs = style_at(se->story, {l.para, l.byte_start + 1});
                    break;
                }
            if (para)
                break;
        }
        if (!para)
            return false;
    }
    const Run *b = base_run(*para);
    base = b ? b->style : cs;
    return true;
}

void ComposerView::apply_char_style(const std::string &name) {
    if (name.empty()) {
        apply_type([](CharStyle &cs) { cs.char_style.clear(); });
        return;
    }
    const CharStyleDef *def = m_doc.find_char_style(name);
    if (!def)
        return;
    const CharStyleDef d = *def;
    apply_type([d](CharStyle &cs) { pagemade::apply_char_style(cs, d); });
}

void ComposerView::define_char_style(const std::string &name) {
    CharStyle cs, base;
    if (name.empty() || !selection_char_style(cs, base))
        return;
    if (editing())
        will_edit(false);
    else
        push_undo();
    /* From the formatting without the old style's name in the way. */
    cs.char_style.clear();
    base.char_style.clear();
    const CharStyleDef d = char_style_from(name, cs, base);
    if (CharStyleDef *old = m_doc.find_char_style(name)) {
        const CharStyleDef before = *old;
        *old = d;
        for (StoryEntry &se : m_doc.stories)
            for (Paragraph &p : se.story.paragraphs)
                for (Run &r : p.runs)
                    if (r.style.char_style == name && !char_style_overridden(r.style, before))
                        pagemade::apply_char_style(r.style, d);
    } else {
        m_doc.char_styles.push_back(d);
    }
    apply_type([d](CharStyle &c) { pagemade::apply_char_style(c, d); }, false);
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::delete_char_style(const std::string &name) {
    auto it = std::find_if(m_doc.char_styles.begin(), m_doc.char_styles.end(),
                           [&](const CharStyleDef &s) { return s.name == name; });
    if (it == m_doc.char_styles.end())
        return;
    if (editing())
        will_edit(false);
    else
        push_undo();
    m_doc.char_styles.erase(it);
    for (StoryEntry &se : m_doc.stories)
        for (Paragraph &p : se.story.paragraphs) {
            for (Run &r : p.runs)
                if (r.style.char_style == name)
                    r.style.char_style.clear();
            normalize(p);
        }
    if (m_typing.char_style == name)
        m_typing.char_style.clear();
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::collect_align_hits(std::vector<AlignHit> &out) const {
    out.clear();
    auto add_line = [&](size_t si, const ComposedLine &l) {
        out.push_back({si, {l.para, l.byte_start}, {l.para, l.byte_end}});
    };
    /* Snap a story range out to the composed lines it touches, so a
     * selection in the middle of a line still aligns that whole line. */
    auto add_expanded = [&](size_t si, const Story &story, const Composition &comp,
                            TextPos a, TextPos b) {
        if (b < a)
            std::swap(a, b);
        if (a == b) {
            Caret at = caret_at(comp, story, a);
            if (at.valid() && at.line < comp.lines.size())
                add_line(si, comp.lines[at.line]);
            else if (a.para < story.paragraphs.size()) {
                const uint32_t n = para_length(story.paragraphs[a.para]);
                out.push_back({si, {a.para, 0}, {a.para, n}});
            }
            return;
        }
        for (size_t pi = a.para; pi <= b.para && pi < story.paragraphs.size(); ++pi) {
            const uint32_t len = para_length(story.paragraphs[pi]);
            uint32_t from = pi == a.para ? a.byte : 0;
            uint32_t to = pi == b.para ? std::min(b.byte, len) : len;
            for (const ComposedLine &l : comp.lines) {
                if (l.para != pi)
                    continue;
                const bool inside = l.byte_end > from && l.byte_start < to;
                const bool empty_inside = l.byte_start == l.byte_end &&
                                          from <= l.byte_start && l.byte_start <= to;
                if (!inside && !empty_inside)
                    continue;
                from = std::min(from, l.byte_start);
                to = std::max(to, l.byte_end);
            }
            if (len == 0 || from < to)
                out.push_back({si, {pi, from}, {pi, to}});
        }
    };

    if (const Story *s = edit_story()) {
        const size_t si = m_doc.story_index(m_edit_story);
        const Composition *c = edit_comp();
        if (!c || si == SIZE_MAX)
            return;
        add_expanded(si, *s, *c, m_anchor, m_caret);
        return;
    }
    for (ItemId id : m_sel) {
        size_t ti = 0;
        const StoryEntry *se = m_doc.story_of(id, &ti);
        const Item *it = m_doc.find_item(id);
        if (!se || !it || !it->is_text())
            continue;
        const size_t si = m_doc.story_index(se->id);
        if (si >= m_comp.size())
            continue;
        for (const ComposedLine &l : m_comp[si].lines)
            if (l.frame == ti)
                add_line(si, l);
    }
}

bool ComposerView::text_align(Align &align, bool &mixed) const {
    std::vector<AlignHit> hits;
    collect_align_hits(hits);
    mixed = false;
    bool any = false;
    for (const AlignHit &h : hits) {
        if (h.story >= m_doc.stories.size())
            continue;
        const Story &s = m_doc.stories[h.story].story;
        if (h.a.para >= s.paragraphs.size())
            continue;
        const Align al = s.paragraphs[h.a.para].style.align;
        if (!any)
            align = al;
        else if (al != align)
            mixed = true;
        any = true;
    }
    return any;
}

void ComposerView::apply_align(Align align) {
    Align current = Align::Left;
    bool mixed = false;
    if (!text_align(current, mixed) || (!mixed && current == align))
        return;
    std::vector<AlignHit> hits;
    collect_align_hits(hits);
    std::vector<std::vector<std::pair<TextPos, TextPos>>> by(m_doc.stories.size());
    for (const AlignHit &h : hits)
        if (h.story < by.size())
            by[h.story].push_back({h.a, h.b});
    if (editing())
        will_edit(false);
    else
        push_undo();
    for (size_t i = 0; i < by.size(); ++i) {
        if (by[i].empty())
            continue;
        TextPos *caret = nullptr, *anchor = nullptr;
        if (editing() && m_doc.stories[i].id == m_edit_story) {
            caret = &m_caret;
            anchor = &m_anchor;
        }
        set_align_ranges(m_doc.stories[i].story, by[i], align, caret, anchor);
    }
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

std::vector<std::pair<size_t, size_t>> ComposerView::selected_para_indices() const {
    std::vector<AlignHit> hits;
    collect_align_hits(hits);
    std::vector<std::pair<size_t, size_t>> out;
    for (const AlignHit &h : hits) {
        if (h.story >= m_doc.stories.size())
            continue;
        const Story &s = m_doc.stories[h.story].story;
        const size_t last = std::max(h.a.para, h.b.para);
        for (size_t pi = std::min(h.a.para, h.b.para); pi <= last && pi < s.paragraphs.size(); ++pi)
            if (std::find(out.begin(), out.end(), std::make_pair(h.story, pi)) == out.end())
                out.push_back({h.story, pi});
    }
    return out;
}

std::vector<const Paragraph *> ComposerView::selected_paragraphs() const {
    std::vector<const Paragraph *> out;
    for (auto [si, pi] : selected_para_indices())
        out.push_back(&m_doc.stories[si].story.paragraphs[pi]);
    return out;
}

void ComposerView::apply_style(const std::string &name) {
    const StyleDef *def = m_doc.find_style(name);
    const auto paras = selected_para_indices();
    if (!def || paras.empty())
        return;
    const StyleDef d = *def;
    if (editing())
        will_edit(false);
    else
        push_undo();
    for (auto [si, pi] : paras)
        pagemade::apply_style(m_doc.stories[si].story.paragraphs[pi], d, m_doc.char_styles);
    if (editing())
        m_typing_on = false;         // type at the caret follows the new style
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::define_style(const std::string &name) {
    const auto paras = selected_para_indices();
    if (name.empty() || paras.empty())
        return;
    if (editing())
        will_edit(false);
    else
        push_undo();
    const auto [fs, fp] = paras.front();
    const StyleDef d = style_from(name, m_doc.stories[fs].story.paragraphs[fp]);
    if (StyleDef *old = m_doc.find_style(name)) {
        const StyleDef before = *old;
        *old = d;
        for (StoryEntry &se : m_doc.stories)
            for (Paragraph &p : se.story.paragraphs)
                if (p.style.name == name && !style_overridden(p, before))
                    pagemade::apply_style(p, d, m_doc.char_styles);
    } else {
        m_doc.styles.push_back(d);
    }
    for (auto [si, pi] : paras)
        m_doc.stories[si].story.paragraphs[pi].style.name = name;
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Colors palette ------------------------------------------------------ */

SwatchId ComposerView::add_doc_swatch(const std::string &name, pagemade::Color rgb,
                                     pagemade::Cmyk cmyk) {
    if (editing())
        will_edit(false);
    else
        push_undo();
    Swatch s;
    s.id = kRegistration;
    for (const Swatch &have : m_doc.swatches)
        s.id = std::max(s.id, have.id);
    s.id += 1;
    s.name = name;
    s.rgb = rgb;
    s.cmyk = cmyk;
    m_doc.swatches.push_back(s);
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
    return s.id;
}

bool ComposerView::update_doc_swatch(SwatchId id, const std::string &name, pagemade::Color rgb,
                                     pagemade::Cmyk cmyk) {
    if (id == kNoPaint || id == kPaper || id == kBlack || id == kRegistration || name.empty())
        return false;
    Swatch *found = nullptr;
    for (Swatch &s : m_doc.swatches)
        if (s.id == id)
            found = &s;
    if (!found)
        return false;
    for (const Swatch &s : m_doc.swatches)
        if (s.name == name && s.id != id)
            return false;   // another swatch already answers to it
    if (editing())
        will_edit(false);
    else
        push_undo();
    found->name = name;
    found->rgb = rgb;
    found->cmyk = cmyk;
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
    return true;
}

bool ComposerView::remove_doc_swatch(SwatchId id) {
    if (id == kNoPaint || id == kPaper || id == kBlack || id == kRegistration)
        return false;
    auto it = m_doc.swatches.begin();
    while (it != m_doc.swatches.end() && it->id != id)
        ++it;
    if (it == m_doc.swatches.end())
        return false;
    if (editing())
        will_edit(false);
    else
        push_undo();
    m_doc.swatches.erase(it);
    /* Its users go black rather than transparent. */
    auto blacken = [id](Paint &p) {
        if (p.swatch == id)
            p.swatch = kBlack;
    };
    auto blacken_type = [id](CharStyle &cs) {
        if (cs.swatch == id)
            cs.swatch = kBlack;
    };
    for (StoryEntry &se : m_doc.stories)
        for (Paragraph &p : se.story.paragraphs)
            for (Run &r : p.runs)
                blacken_type(r.style);
    for (StyleDef &d : m_doc.styles)
        blacken_type(d.type);
    for (CharStyleDef &d : m_doc.char_styles)
        blacken_type(d.type);
    for (Page &page : m_doc.pages)
        for (Item &item : page.items) {
            if (Shape *sh = item.shape()) {
                blacken(sh->fill);
                blacken(sh->stroke.paint);
            } else if (PlacedImage *im = item.image()) {
                blacken(im->stroke.paint);
            }
        }
    blacken(m_default_shape.fill);
    blacken(m_default_shape.stroke.paint);
    if (m_default_type.swatch == id)
        m_default_type.swatch = kBlack;
    if (m_typing.swatch == id)
        m_typing.swatch = kBlack;
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
    return true;
}

void ComposerView::delete_style(const std::string &name) {
    auto it = std::find_if(m_doc.styles.begin(), m_doc.styles.end(),
                           [&](const StyleDef &s) { return s.name == name; });
    if (it == m_doc.styles.end())
        return;
    if (editing())
        will_edit(false);
    else
        push_undo();
    m_doc.styles.erase(it);
    for (StoryEntry &se : m_doc.stories)
        for (Paragraph &p : se.story.paragraphs)
            if (p.style.name == name)
                p.style.name.clear();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::apply_para(const std::function<void(ParaStyle &)> &fn) {
    if (!continues_run())
        m_run_paras = selected_para_indices();
    if (m_run_paras.empty())
        return;
    if (editing())
        will_edit(false);
    else
        push_undo();
    for (auto [si, pi] : m_run_paras)
        if (si < m_doc.stories.size() && pi < m_doc.stories[si].story.paragraphs.size())
            fn(m_doc.stories[si].story.paragraphs[pi].style);
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::apply_type(const std::function<void(CharStyle &)> &fn, bool undo) {
    if (Story *s = edit_story()) {
        if (has_selection()) {
            if (undo)
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
    bool saw_frame = false;
    const bool run = continues_run();
    if (!run)
        m_run_type.clear();
    for (ItemId id : m_sel) {
        size_t ti = 0;
        StoryEntry *se = m_doc.story_of(id, &ti);
        const Item *it = m_doc.find_item(id);
        if (!se || !it || !it->is_text())
            continue;
        saw_frame = true;
        size_t si = m_doc.story_index(se->id);
        if (run || si >= m_comp.size())
            continue;
        for (const ComposedLine &l : m_comp[si].lines)
            if (l.frame == ti)
                m_run_type.push_back({si, {l.para, l.byte_start}, {l.para, l.byte_end}});
    }
    if (m_run_type.empty()) {
        if (saw_frame)
            return;
        fn(m_default_type);
        selection_changed();
        return;
    }
    if (undo)
        push_undo();
    for (const TypeRange &r : m_run_type)
        if (r.story < m_doc.stories.size())
            restyle(m_doc.stories[r.story].story, r.a, r.b, fn);
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
    m_doc.tidy_groups();                 // a group left with one item ends
    m_sel.clear();
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Layers ---------------------------------------------------------- */

void ComposerView::drop_uneditable() {
    m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(), [&](ItemId id) {
                    const Item *it = m_doc.find_item(id);
                    return !it || !m_doc.item_editable(*it);
                }),
                m_sel.end());
    if (const StoryEntry *se = m_edit_story ? m_doc.find_story(m_edit_story) : nullptr) {
        bool any = false;
        for (ItemId f : se->thread)
            if (const Item *it = m_doc.find_item(f))
                any = any || m_doc.item_editable(*it);
        if (!any)
            m_edit_story = 0;
    }
}

void ComposerView::set_active_layer(LayerId id) {
    if (!m_doc.find_layer(id) || m_doc.active_layer == id)
        return;
    m_doc.active_layer = id;
    if (on_document_change)
        on_document_change();
}

bool ComposerView::add_layer(const std::string &name) {
    if (m_doc.layers.size() >= kMaxLayers)
        return false;
    push_undo();
    m_doc.add_layer(name);
    if (on_document_change)
        on_document_change();
    return true;
}

void ComposerView::remove_layer(LayerId id) {
    if (!m_doc.find_layer(id) || m_doc.layers.size() <= 1)
        return;
    push_undo();
    m_doc.remove_layer(id);
    drop_uneditable();
    selection_changed();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::move_layer(LayerId id, int dir) {
    const size_t i = m_doc.layer_index(id);
    if (i == SIZE_MAX || (dir < 0 && i == 0) || (dir > 0 && i + 1 >= m_doc.layers.size()))
        return;
    push_undo();
    m_doc.move_layer(id, dir);
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::rename_layer(LayerId id, const std::string &name) {
    Layer *l = m_doc.find_layer(id);
    if (!l || name.empty() || l->name == name)
        return;
    push_undo();
    m_doc.find_layer(id)->name = name;
    if (on_document_change)
        on_document_change();
}

void ComposerView::set_layer_visible(LayerId id, bool visible) {
    Layer *l = m_doc.find_layer(id);
    if (!l || l->visible == visible)
        return;
    push_undo();
    m_doc.find_layer(id)->visible = visible;
    drop_uneditable();
    selection_changed();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::set_layer_locked(LayerId id, bool locked) {
    Layer *l = m_doc.find_layer(id);
    if (!l || l->locked == locked)
        return;
    push_undo();
    m_doc.find_layer(id)->locked = locked;
    drop_uneditable();
    selection_changed();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::move_selection_to_layer(LayerId id) {
    const Layer *l = m_doc.find_layer(id);
    if (!l || m_sel.empty())
        return;
    push_undo();
    m_doc.set_items_layer(with_groups(m_sel), id);
    drop_uneditable();                   // onto a hidden or locked layer: gone from view
    selection_changed();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

size_t ComposerView::layer_item_count(LayerId id) const {
    size_t n = 0;
    for (const Page &p : m_doc.pages)
        for (const Item &it : p.items)
            n += m_doc.layer_of(it) == id;
    return n;
}

/* ---- Clipboard ------------------------------------------------------- */

bool ComposerView::can_copy() const {
    return editing() ? has_selection() : !m_sel.empty();
}

bool ComposerView::can_paste() const {
    if (editing()) {
        GLFWwindow *win = screen() ? screen()->glfw_window() : nullptr;
        const char *cb = win ? glfwGetClipboardString(win) : nullptr;
        return cb && *cb;
    }
    return !m_clip.items.empty();
}

void ComposerView::edit_copy() {
    if (editing()) {
        text_clipboard(GLFW_KEY_C);
        return;
    }
    const std::vector<ItemId> ids = with_groups(m_sel);
    size_t page = 0;
    if (ids.empty() || !m_doc.find_item(ids.front(), &page))
        return;
    m_clip = ItemClip();
    m_clip.page = page;
    std::vector<StoryId> stories;
    auto copied = [&](ItemId id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    for (const Item &it : m_doc.pages[page].items) {
        if (!copied(it.id))
            continue;
        m_clip.items.push_back(it);
        m_clip.bounds.add(item_bounds(it));
        if (const StoryEntry *se = m_doc.story_of(it.id))
            if (std::find(stories.begin(), stories.end(), se->id) == stories.end())
                stories.push_back(se->id);
        if (const PlacedImage *im = it.image())
            if (const ImageAsset *a = m_doc.find_image(im->asset)) {
                bool have = false;
                for (const ImageAsset &k : m_clip.assets)
                    have = have || k.id == a->id;
                if (!have)
                    m_clip.assets.push_back(*a);
            }
    }
    /* Each story with only its copied blocks threaded, in reading order */
    for (StoryId sid : stories) {
        StoryEntry e = *m_doc.find_story(sid);
        std::vector<ItemId> thread;
        for (ItemId f : e.thread)
            if (copied(f))
                thread.push_back(f);
        e.thread = std::move(thread);
        m_clip.stories.push_back(std::move(e));
    }
}

void ComposerView::edit_cut() {
    if (editing()) {
        text_clipboard(GLFW_KEY_X);
        return;
    }
    if (m_sel.empty())
        return;
    edit_copy();
    m_sel = with_groups(m_sel);
    delete_selection_items();            // its own undo step
}

void ComposerView::edit_paste(const Point *at) {
    if (editing()) {
        text_clipboard(GLFW_KEY_V);
        return;
    }
    if (m_clip.items.empty() || !check_active_layer())
        return;
    push_undo();
    /* At the point asked for; else a step down and right of the originals
     * (further with each paste), or where they were on another page */
    float dx = 0.f, dy = 0.f;
    if (at) {
        dx = at->x - m_clip.bounds.x0;
        dy = at->y - m_clip.bounds.y0;
    } else if (m_clip.page == m_page) {
        dx = dy = 12.f * (float) ++m_clip.pastes;
    }
    for (const ImageAsset &a : m_clip.assets)
        if (!m_doc.find_image(a.id))
            m_doc.add_image(a);          // pasted into another publication
    std::map<ItemId, ItemId> new_id;
    std::map<uint32_t, uint32_t> new_group;
    m_sel.clear();
    for (const Item &o : m_clip.items) {
        Item it = o;
        it.layer = 0;                    // the active layer
        if (it.group) {
            auto g = new_group.find(o.group);
            if (g == new_group.end())
                g = new_group.emplace(o.group, m_doc.next_id++).first;
            it.group = g->second;
        }
        it.xf = Transform::translate(dx, dy) * it.xf;
        const ItemId id = m_doc.add_item(m_page, it);
        new_id[o.id] = id;
        m_sel.push_back(id);
    }
    for (const StoryEntry &e : m_clip.stories) {
        const StoryId sid = m_doc.add_story(e.story);
        if (StoryEntry *ne = m_doc.find_story(sid))
            for (ItemId f : e.thread)
                if (new_id.count(f))
                    ne->thread.push_back(new_id[f]);
    }
    recompose();
    selection_changed();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

/* ---- Align -------------------------------------------------------------- */

namespace {

/* The selection as units that move together: a whole group, or one item. */
struct AlignUnit {
    Bounds bounds;
    std::vector<ItemId> ids;
};

} // namespace

static std::vector<AlignUnit> align_units(const PageDoc &doc, const std::vector<ItemId> &sel,
                                          const std::function<Bounds(const Item &)> &bounds_of) {
    std::vector<AlignUnit> units;
    std::map<uint32_t, size_t> group_unit;
    for (ItemId id : sel) {
        const Item *it = doc.find_item(id);
        if (!it)
            continue;
        size_t u;
        if (it->group && group_unit.count(it->group)) {
            u = group_unit[it->group];
        } else {
            u = units.size();
            units.emplace_back();
            if (it->group)
                group_unit[it->group] = u;
        }
        units[u].bounds.add(bounds_of(*it));
        units[u].ids.push_back(id);
    }
    return units;
}

bool ComposerView::can_align() const {
    if (editing())
        return false;
    return align_units(m_doc, m_sel, [this](const Item &it) { return item_bounds(it); }).size() >= 2;
}

void ComposerView::align_selection(AlignTo how) {
    const std::vector<AlignUnit> units =
        align_units(m_doc, with_groups(m_sel), [this](const Item &it) { return item_bounds(it); });
    if (units.size() < 2)
        return;
    Bounds all;
    for (const AlignUnit &u : units)
        all.add(u.bounds);
    push_undo();
    for (const AlignUnit &u : units) {
        const Bounds &b = u.bounds;
        float dx = 0.f, dy = 0.f;
        switch (how) {
        case AlignTo::Left:   dx = all.x0 - b.x0; break;
        case AlignTo::Right:  dx = all.x1 - b.x1; break;
        case AlignTo::Center: dx = (all.x0 + all.x1 - b.x0 - b.x1) * 0.5f; break;
        case AlignTo::Top:    dy = all.y0 - b.y0; break;
        case AlignTo::Bottom: dy = all.y1 - b.y1; break;
        case AlignTo::Middle: dy = (all.y0 + all.y1 - b.y0 - b.y1) * 0.5f; break;
        }
        for (ItemId id : u.ids)
            if (Item *it = m_doc.find_item(id))
                it->xf = Transform::translate(dx, dy) * it->xf;
    }
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Pictures ------------------------------------------------------------ */

/* The file a picture is read from: the store's path, else its source URI. */
static std::string picture_path(const PageDoc &doc, const ImageStore *images, uint32_t asset) {
    std::string path = images ? images->path_of(asset) : std::string();
    if (path.empty())
        if (const ImageAsset *a = doc.find_image(asset))
            file_path_from_uri(a->source, path);
    return path;
}

bool ComposerView::can_reload_pictures() const {
    if (editing())
        return false;
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (const PlacedImage *im = it->image())
                if (!picture_path(m_doc, m_images, im->asset).empty())
                    return true;
    return false;
}

void ComposerView::reload_pictures() {
    if (!m_images)
        return;
    std::vector<uint32_t> assets;
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (const PlacedImage *im = it->image())
                if (std::find(assets.begin(), assets.end(), im->asset) == assets.end())
                    assets.push_back(im->asset);
    int done = 0;
    std::string failed;
    for (uint32_t id : assets) {
        const std::string path = picture_path(m_doc, m_images, id);
        if (path.empty())
            continue;
        ImageImport imported = read_image_import(path);
        ImageMetadata meta;
        std::string err;
        if (!imported.ok || !m_images->add(id, std::move(imported.bytes), &meta, &err)) {
            failed = path;
            continue;
        }
        m_images->set_path(id, path);
        if (ImageAsset *a = m_doc.find_image(id)) {
            a->meta = meta;
            a->source_size = imported.stamp.size;
            a->source_mtime = imported.stamp.mtime;
            a->hash = imported.stamp.hash;
        }
        m_load_want.erase(id);           // the screen asks for it afresh
        m_load_failed.erase(id);
        ++done;
    }
    if (on_note)
        on_note(!failed.empty() ? "couldn't read " + failed
                : done == 1 ? "reloaded the picture" : "reloaded " + std::to_string(done) + " pictures");
    if (done)
        bump_version();                  // the stamps changed: the file needs saving
    if (screen())
        screen()->redraw();
}

bool ComposerView::can_group() const {
    if (m_sel.size() < 2)
        return false;
    size_t page = SIZE_MAX;
    uint32_t group = 0;
    bool one_group = true;
    for (ItemId id : m_sel) {
        size_t pg = 0;
        const Item *it = m_doc.find_item(id, &pg);
        if (!it)
            return false;
        if (page == SIZE_MAX) {
            page = pg;
            group = it->group;
        } else if (pg != page) {
            return false;
        }
        one_group = one_group && it->group != 0 && it->group == group;
    }
    return !one_group;
}

bool ComposerView::can_ungroup() const {
    for (ItemId id : m_sel)
        if (const Item *it = m_doc.find_item(id))
            if (it->group)
                return true;
    return false;
}

void ComposerView::group_selection() {
    if (!can_group())
        return;
    push_undo();
    m_doc.group_items(m_sel);
    m_sel = with_groups(m_sel);
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::ungroup_selection() {
    if (!can_ungroup())
        return;
    push_undo();
    m_doc.ungroup_items(m_sel);
    selection_changed();                 // the items stay selected, now apart
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
    recompose();                         // page numbers after it move up
    page_changed();
    if (m_page_view == PageView::Stack && screen())
        screen()->perform_layout();
}

/* The current page's facing spread: page 1 alone, then 2-3, 4-5, ... */
static void spread_of(const PageDoc &doc, size_t page, size_t &first, size_t &count) {
    const size_t n = doc.pages.size();
    if (!doc.setup.facing || page == 0 || page >= n) {
        first = page;
        count = 1;
        return;
    }
    first = 1 + ((page - 1) / 2) * 2;
    count = std::min<size_t>(2, n - first);
}

bool ComposerView::can_duplicate_spread() const {
    size_t first, count;
    spread_of(m_doc, m_page, first, count);
    return count == 2;
}

void ComposerView::duplicate_page(bool spread) {
    end_placement(false);
    if (m_creating)
        finish_creation();
    size_t first = m_page, count = 1;
    if (spread)
        spread_of(m_doc, m_page, first, count);
    if (first >= m_doc.pages.size())
        return;
    push_undo();
    m_page = m_doc.duplicate_pages(first, count);
    recompose();                         // page numbers after it move up
    page_changed();
    if (on_document_change)
        on_document_change();
    if (m_page_view == PageView::Stack && screen())
        screen()->perform_layout();
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
    if (m_page_view == PageView::Stack && screen())
        screen()->perform_layout();
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
    recompose();                         // page numbers follow the new order
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
        size_t ipage = 0;
        const Item *it = m_doc.find_item(se->thread[i], &ipage);
        if (!it)
            continue;
        const Point l = it->xf.inverse().apply(pt);
        const float dx = std::max({-l.x, 0.f, l.x - it->w});
        const float dy = std::max({-l.y, 0.f, l.y - it->h});
        /* A point is in one page's space. Frames on that page win. */
        const float d = dx * dx + dy * dy + (ipage == m_point_page ? 0.f : 1.0e6f);
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
    /* Option on the Mac, Ctrl elsewhere; either works on both. */
    bool word = (modifiers & (GLFW_MOD_ALT | GLFW_MOD_CONTROL)) != 0;
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
    auto on = key == GLFW_KEY_B ? +[](const CharStyle &cs) { return cs.bold; }
                               : +[](const CharStyle &cs) { return cs.italic; };
    bool all = true;
    restyle(*s, m_anchor, m_caret, [&](CharStyle &cs) { all &= on(cs); });
    const bool want = !all;
    restyle(*s, m_anchor, m_caret, [&](CharStyle &cs) {
        int weight = cs.bold ? 700 : 400;
        bool italic = cs.italic;
        std::string width;
        if (!cs.face.empty() && m_fonts->describe(cs.family, cs.face, weight, italic))
            width = FontLibrary::width_of(cs.face);
        if (key == GLFW_KEY_B)
            weight = want ? 700 : 400;
        else
            italic = want;
        std::string chosen;
        if (m_fonts->match(cs.family, weight, italic, width, &chosen)) {
            cs.face = chosen;
            int w = 400;
            bool it = false;
            if (m_fonts->describe(cs.family, chosen, w, it)) {
                cs.bold = w >= 600;
                cs.italic = it;
            }
        } else {
            if (key == GLFW_KEY_B) cs.bold = want; else cs.italic = want;
            cs.face.clear();
        }
    });
    recompose();
    selection_changed();
    if (screen())
        screen()->redraw();
}

/* ---- Keyboard ------------------------------------------------------------ */

void ComposerView::text_clipboard(int key) {
    Story *s = edit_story();
    GLFWwindow *win = screen() ? screen()->glfw_window() : nullptr;
    if (!s)
        return;
    switch (key) {
        case GLFW_KEY_C:
            if (has_selection() && win) {
                std::string t = copy_text(*s, m_anchor, m_caret);
                glfwSetClipboardString(win, t.c_str());
            }
            break;
        case GLFW_KEY_X:
            if (has_selection() && win) {
                will_edit(false);        // cut is its own step
                std::string t = copy_text(*s, m_anchor, m_caret);
                glfwSetClipboardString(win, t.c_str());
                delete_selection();
            }
            break;
        case GLFW_KEY_V:
            if (win)
                if (const char *cb = glfwGetClipboardString(win))
                    insert_string(cb, false);   // paste is its own step
            break;
    default: break;
    }
}

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
    if (!editing() && cmd && (key == GLFW_KEY_C || key == GLFW_KEY_X || key == GLFW_KEY_V)) {
        /* Items to and from the item clipboard */
        if (action == GLFW_PRESS) {
            if (key == GLFW_KEY_C) edit_copy();
            else if (key == GLFW_KEY_X) edit_cut();
            else edit_paste();
        }
        return true;
    }
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
    /* Cmd/Ctrl with the arrows, Home and End is caret movement (by word,
     * to the story's ends), handled with the plain navigation keys. */
    const bool nav = key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT ||
                     key == GLFW_KEY_HOME || key == GLFW_KEY_END;
    if (cmd && !nav) {
        switch (key) {
        case GLFW_KEY_A: case GLFW_KEY_C: case GLFW_KEY_X:
        case GLFW_KEY_V: case GLFW_KEY_B: case GLFW_KEY_I:
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
            if (action != GLFW_PRESS)
                return true;             // swallow the release
            break;
        default:
            return false;                // app-level zoom shortcuts etc.
        }
        if (key == GLFW_KEY_C || key == GLFW_KEY_X || key == GLFW_KEY_V) {
            text_clipboard(key);
            return true;
        }
        Story *s = edit_story();
        switch (key) {
        case GLFW_KEY_A:
            m_anchor = {};
            m_caret = story_end(*s);
            m_burst_open = false;
            if (screen()) screen()->redraw();
            return true;
        case GLFW_KEY_B: case GLFW_KEY_I:
            toggle_style(key);
            return true;
        case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
            /* Cmd/Ctrl+Return: a frame break, the text goes on in the next
             * text block. */
            will_edit(true);
            delete_selection();
            insert_string("\x0C", true);
            return true;
        }
        return true;
    }

    switch (key) {
    case GLFW_KEY_BACKSPACE: case GLFW_KEY_DELETE:
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER:
    case GLFW_KEY_TAB:
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
    case GLFW_KEY_TAB:
        /* A tab; Shift+Tab a right-indent tab (to the right indent) */
        will_edit(true);
        delete_selection();
        insert_string((modifiers & GLFW_MOD_SHIFT) ? "\xEE\x80\x82" : "\t", true);
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

bool ComposerView::insert_at_caret(const std::string &utf8) {
    if (!editing())
        return false;
    insert_string(utf8, false);
    return true;
}

bool ComposerView::keyboard_character_event(unsigned int codepoint) {
    if (!editing() || codepoint < 32)
        return false;
    if (m_smart_quotes && (codepoint == '"' || codepoint == '\'')) {
        /* What precedes the text that replaces the selection. */
        const TextPos at = has_selection() ? (m_anchor < m_caret ? m_anchor : m_caret) : m_caret;
        codepoint = smart_quote(codepoint, char_before(*edit_story(), at));
    }
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

void ComposerView::draw_page(NVGcontext *ctx, float px, size_t page) {
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
    /* Facing pages: a soft fold shade on the spine side (left of a
     * right-hand page, right of a left-hand page). Screen only. */
    const bool verso = s.facing && page % 2 == 1;
    if (s.facing) {
        const float fold = std::min(14.f, s.width * 0.04f);
        const float sx = verso ? s.width : 0.f;
        const float ex = verso ? s.width - fold : fold;
        nvgBeginPath(ctx);
        nvgRect(ctx, std::min(sx, ex), 0, fold, s.height);
        nvgFillPaint(ctx, nvgLinearGradient(ctx, sx, 0, ex, 0, nvgRGBA(0, 0, 0, 28),
                                            nvgRGBA(0, 0, 0, 0)));
        nvgFill(ctx);
    }
    /* The bleed: how far past the trim pictures and fills should run. A
     * facing spread has none at the spine, where the pages meet. */
    if (s.bleed > 0.f) {
        const float b = s.bleed, w = s.width, h = s.height;
        const bool spine = s.facing && m_page_view != PageView::One;
        nvgBeginPath(ctx);
        if (!spine) {
            nvgRect(ctx, -b, -b, w + 2 * b, h + 2 * b);
        } else if (verso) {      // open on the right
            nvgMoveTo(ctx, w, -b);
            nvgLineTo(ctx, -b, -b);
            nvgLineTo(ctx, -b, h + b);
            nvgLineTo(ctx, w, h + b);
        } else {                 // open on the left
            nvgMoveTo(ctx, 0, -b);
            nvgLineTo(ctx, w + b, -b);
            nvgLineTo(ctx, w + b, h + b);
            nvgLineTo(ctx, 0, h + b);
        }
        nvgStrokeWidth(ctx, px);
        nvgStrokeColor(ctx, nvgRGBA(220, 60, 60, 160));
        nvgStroke(ctx);
    }
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const float l = left, t = s.margin_top;
    const float r = s.width - right, b = s.height - s.margin_bottom;
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
        const bool loose = m_strict_loose_tight ? l.word_loose : l.loose;
        const bool tight = m_strict_loose_tight ? l.word_tight : l.tight;
        if (l.frame != ti || !m_show_loose_tight || !(loose || tight))
            continue;
        nvgBeginPath(ctx);
        nvgRect(ctx, l.left, l.top, l.width, l.leading);
        nvgFillColor(ctx, tight ? nvgRGBA(255, 90, 90, 80) : nvgRGBA(255, 200, 0, 80));
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

namespace {

bool keep_item(const std::vector<ItemId> *which, bool only, ItemId id) {
    if (!which)
        return true;
    for (ItemId x : *which)
        if (x == id)
            return only;
    return !only;
}

} // namespace

void ComposerView::lifted_ids(std::vector<ItemId> &ids) const {
    ids.clear();
    if (m_placing) {
        ids = m_sel;
        return;
    }
    if (m_pointer_down && m_drag == Handle::Move && m_tool != Tool::Crop)
        for (const Item &it : m_drag_items)
            ids.push_back(it.id);
}

void ComposerView::adopt_moved_items() {
    std::vector<PageSlot> slots;
    float aw, ah;
    arrangement(m_doc, m_page, m_page_view, slots, aw, ah);
    if (slots.size() < 2)
        return;
    const float pw = m_doc.setup.width > 1.f ? m_doc.setup.width : 1.f;
    const float ph = m_doc.setup.height > 1.f ? m_doc.setup.height : 1.f;
    std::vector<ItemId> ids;
    for (const Item &o : m_drag_items)
        ids.push_back(o.id);
    /* A group changes page as one, by where its middle landed */
    std::map<uint32_t, Bounds> group_bounds;
    for (ItemId id : ids)
        if (const Item *it = m_doc.find_item(id))
            if (it->group)
                group_bounds[it->group].add(item_bounds(*it));
    bool moved = false;
    for (ItemId id : ids) {
        size_t src = 0;
        Item *it = m_doc.find_item(id, &src);
        if (!it)
            continue;
        const Bounds b = it->group ? group_bounds[it->group] : item_bounds(*it);
        if (b.empty())
            continue;
        float sx = 0.f, sy = 0.f;
        bool on_screen = false;
        for (const PageSlot &sl : slots)
            if (sl.index == src) {
                sx = sl.x;
                sy = sl.y;
                on_screen = true;
            }
        if (!on_screen)
            continue;
        const float cx = (b.x0 + b.x1) * 0.5f + sx;
        const float cy = (b.y0 + b.y1) * 0.5f + sy;
        size_t dest = src;
        float dx = 0.f, dy = 0.f;
        for (const PageSlot &sl : slots)
            if (cx >= sl.x && cy >= sl.y && cx <= sl.x + pw && cy <= sl.y + ph) {
                dest = sl.index;
                dx = sx - sl.x;
                dy = sy - sl.y;
                break;
            }
        if (dest == src)
            continue;
        const float e = it->xf.e, f = it->xf.f;
        it->xf.e = e + dx;
        it->xf.f = f + dy;
        if (m_doc.transfer_item(id, dest))
            moved = true;
        else {
            it->xf.e = e;
            it->xf.f = f;
        }
    }
    if (!moved)
        return;
    recompose();                         // a page number moved with its block
    if (m_drag_target) {
        size_t pg = m_page;
        if (m_doc.find_item(m_drag_target, &pg) && pg != m_page) {
            m_page = pg;
            if (on_page_change)
                on_page_change();
        }
    }
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::draw_chrome(NVGcontext *ctx, float px, size_t page,
                               const std::vector<ItemId> *which, bool only) {
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

    static const std::vector<Item> none;
    const std::vector<Item> &page_items =
        page < m_doc.pages.size() ? static_cast<const std::vector<Item> &>(m_doc.pages[page].items) : none;
    for (const Item &it : page_items) {
        if (!keep_item(which, only, it.id) || !m_doc.item_visible(it))
            continue;
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

        if (selected && !it.group) {   // a group's corners are on its box
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

    /* A selected group: its box, with the corner handles that resize it */
    if (!only) {
        for (const auto &g : selected_groups(page)) {
            const Bounds &b = g.second;
            nvgBeginPath(ctx);
            nvgRect(ctx, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
            nvgStrokeWidth(ctx, px);
            nvgStrokeColor(ctx, nvgRGBA(60, 120, 255, 170));
            nvgStroke(ctx);
            const float hs = kHandlePx * px;
            nvgBeginPath(ctx);
            for (const Point &c : {Point{b.x0, b.y0}, Point{b.x1, b.y0},
                                   Point{b.x0, b.y1}, Point{b.x1, b.y1}})
                nvgRect(ctx, c.x - hs * 0.5f, c.y - hs * 0.5f, hs, hs);
            nvgFillColor(ctx, nvgRGB(0, 0, 0));
            nvgFill(ctx);
        }
    }

    if (!only && m_marquee && page == m_page) {
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

void ComposerView::set_image_store(ImageStore *images) {
    m_images = images;
    m_tex_gen = 0;                       // the next draw drops whatever was cached
}

void ComposerView::frame_size(const ImageMetadata &meta, size_t page, float &pw, float &ph) const {
    const PageSetup &s = m_doc.setup;
    pw = image_print_points(meta.width_px, meta.ppi_x);
    ph = image_print_points(meta.height_px, meta.ppi_y);
    if (!(pw > 0.f)) pw = 36.f;
    if (!(ph > 0.f)) ph = 36.f;
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const float live_w = std::max(36.f, s.width - left - right);
    const float live_h = std::max(36.f, s.height - s.margin_top - s.margin_bottom);
    if (pw > live_w || ph > live_h) {
        const float scale = std::min(live_w / pw, live_h / ph);
        pw *= scale;
        ph *= scale;
    }
}

void ComposerView::place_frame(uint32_t asset, const ImageMetadata &meta,
                               size_t page, const Point *at) {
    if (page >= m_doc.pages.size())
        return;
    float pw, ph;
    frame_size(meta, page, pw, ph);
    float left = 0.f, right = 0.f;
    page_side_margins(m_doc.setup, page, left, right);
    Item it;
    it.w = pw;
    it.h = ph;
    it.xf = at ? Transform::translate(at->x, at->y)
               : Transform::translate(left, m_doc.setup.margin_top);
    m_last_column = false;
    if (m_fit_columns && pw > 0.f) {
        /* The column under the drop, as wide as it, and kept on the page
         * with its caption: moved up rather than past the bottom margin */
        const PageSetup &ps = m_doc.setup;
        float col_x, col_w, x;
        column_at(page, at ? &at->x : nullptr, col_x, col_w);
        fit_column(page, col_x, col_w, ph / pw, x, pw, ph);
        const float top = ps.margin_top;
        const float lowest = ps.height - ps.margin_bottom - ph - caption_depth();
        const float y = std::max(top, std::min(at ? at->y : top, lowest));
        it.w = pw;
        it.h = ph;
        it.xf = Transform::translate(x, y);
        m_last_column = true;
        m_last_col_x = col_x;
        m_last_col_w = col_w;
    }
    PlacedImage im;
    im.asset = asset;
    im.x = 0;
    im.y = 0;
    im.w = pw;
    im.h = ph;
    it.content = im;
    const ItemId id = m_doc.add_item(page, it);
    m_last_caption = 0;
    if (const Item *placed = m_doc.find_item(id))
        m_last_caption = add_caption(page, *placed);
    if (m_last_caption) {
        m_doc.group_items({id, m_last_caption});   // they move as one
        recompose();
    }
    if (page != m_page)
        show_page(page);
    m_sel = with_groups({id});
    m_edit_story = 0;
    if (m_tool != Tool::Pointer)
        set_tool(Tool::Pointer);
    else
        selection_changed();
    if (screen())
        screen()->redraw();
}

/* The space between a picture and its caption, points. */
static constexpr float kCaptionGap = 4.f;

float ComposerView::caption_depth() const {
    if (!m_caption_pictures)
        return 0.f;
    float lead = 12.f;
    if (const StyleDef *def = m_doc.find_style("Caption"))
        lead = def->type.leading > 0.f ? def->type.leading : def->type.size * 1.2f;
    return kCaptionGap + std::ceil(lead * 2.f + 2.f);
}

void ComposerView::column_at(size_t page, const float *x, float &col_x, float &col_w) const {
    const PageSetup &s = m_doc.setup;
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const int n = std::max(1, s.columns);
    const float live = std::max(1.f, s.width - left - right);
    const float gutter = n > 1 ? std::min(s.gutter, live / (float) (n - 1) * 0.5f) : 0.f;
    col_w = std::max(1.f, (live - gutter * (float) (n - 1)) / (float) n);
    int i = 0;
    if (x) {
        /* The column whose middle is nearest: a gutter or margin drop
         * goes to the column beside it */
        float best = INFINITY;
        for (int k = 0; k < n; ++k) {
            const float mid = left + (float) k * (col_w + gutter) + col_w * 0.5f;
            if (std::fabs(*x - mid) < best) {
                best = std::fabs(*x - mid);
                i = k;
            }
        }
    }
    col_x = left + (float) i * (col_w + gutter);
}

void ComposerView::fit_column(size_t, float col_x, float col_w, float aspect,
                              float &x, float &w, float &h) const {
    const PageSetup &s = m_doc.setup;
    const float live_h = std::max(36.f, s.height - s.margin_top - s.margin_bottom - caption_depth());
    w = col_w;
    h = col_w * aspect;
    if (h > live_h) {
        h = live_h;
        w = aspect > 0.f ? h / aspect : col_w;
    }
    x = col_x + (col_w - w) * 0.5f;
}

ItemId ComposerView::add_caption(size_t page, const Item &picture) {
    const PlacedImage *im = picture.image();
    if (!m_caption_pictures || !im || page >= m_doc.pages.size())
        return 0;
    /* The file name without its extension: something to type over. */
    std::string text = "Caption";
    if (const ImageAsset *a = m_doc.find_image(im->asset)) {
        text = a->name;
        const size_t dot = text.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
            text.erase(dot);
        if (text.empty())
            text = "Caption";
    }
    Paragraph para;
    para.style.name = "Caption";
    para.runs.push_back(Run{m_default_type, text});
    float lead = 12.f;
    if (const StyleDef *def = m_doc.find_style("Caption")) {
        pagemade::apply_style(para, *def, m_doc.char_styles);
        lead = def->type.leading > 0.f ? def->type.leading : def->type.size * 1.2f;
    }
    Story story;
    story.paragraphs.push_back(std::move(para));
    const StoryId sid = m_doc.add_story(std::move(story));
    /* Two lines deep, the picture's width, just under it (in the
     * picture's own space, so a turned picture's caption turns with it) */
    return m_doc.add_text_frame(page, sid, picture.w, std::ceil(lead * 2.f + 2.f),
                                picture.xf * Transform::translate(0.f, picture.h + kCaptionGap));
}

void ComposerView::fit_caption(ItemId caption, const Item &picture) {
    Item *c = caption ? m_doc.find_item(caption) : nullptr;
    if (!c || !c->is_text())
        return;
    c->w = picture.w;
    c->xf = picture.xf * Transform::translate(0.f, picture.h + kCaptionGap);
}

namespace {

std::string image_leaf_name(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool ComposerView::commit_import(uint32_t id, const std::string &path, ImageImport imported,
                                 size_t page, bool place, const Point *at, std::string *error) {
    if (!m_images) {
        if (error) *error = "pictures aren't available";
        return false;
    }
    if (!imported.ok) {
        if (error) *error = imported.error.empty() ? "couldn't read the picture" : imported.error;
        return false;
    }
    ImageMetadata meta = imported.meta;
    if (!imported.bytes.empty()) {
        if (!m_images->add(id, std::move(imported.bytes), &meta, error))
            return false;
        m_images->set_path(id, path);
    } else {
        m_images->add_link(id, path, meta);
    }
    ImageAsset asset;
    asset.id = id;
    asset.name = image_leaf_name(path);
    asset.source = file_uri(path);
    asset.meta = meta;
    asset.source_size = imported.stamp.size;
    asset.source_mtime = imported.stamp.mtime;
    asset.hash = imported.stamp.hash;
    push_undo();
    m_doc.add_image(std::move(asset));
    if (place)
        place_frame(id, meta, page, at);
    if (on_document_change)
        on_document_change();
    return true;
}

bool ComposerView::place_image_file(const std::string &path, std::string *error) {
    if (!m_doc.active_layer_editable()) {
        if (error) *error = "the active layer is hidden or locked";
        return false;
    }
    if (!m_images) {
        if (error) *error = "pictures aren't available";
        return false;
    }
    /* Taken before the read, and not given back: undo must not hand this
     * id to a later picture while a redo still names it. */
    const uint32_t id = m_images->take_id();
    return commit_import(id, path, read_image_import(path), m_page, true, nullptr, error);
}

void ComposerView::place_image_file(const std::string &path, size_t page, const Point *at,
                                    std::function<void(const std::string &error)> done,
                                    Vector2i px_hint) {
    if (!check_active_layer()) {
        if (done) done(std::string());   // said already, in the status bar
        return;
    }
    if (!m_images) {
        if (done) done("pictures aren't available");
        return;
    }
    if (page >= m_doc.pages.size())
        page = m_page;
    /* Taken now and never given back (see the other form) */
    const uint32_t id = m_images->take_id();

    /* The stand-in: a link not yet read, at the hinted size (or an inch and
     * a half square), placed as one undo step */
    ImageMetadata guess;
    const bool hinted = px_hint.x() > 0 && px_hint.y() > 0;
    guess.width_px = guess.stored_width_px = hinted ? px_hint.x() : 108;
    guess.height_px = guess.stored_height_px = hinted ? px_hint.y() : 108;
    m_images->add_link(id, path, guess);
    ImageAsset asset;
    asset.id = id;
    asset.name = image_leaf_name(path);
    asset.source = file_uri(path);
    asset.meta = guess;
    push_undo();
    m_doc.add_image(std::move(asset));
    place_frame(id, guess, page, at);
    Probing probing;
    probing.caption = m_last_caption;
    probing.column = m_last_column;
    probing.col_x = m_last_col_x;
    probing.col_w = m_last_col_w;
    if (!m_sel.empty())
        if (const Item *it = m_doc.find_item(m_sel.front())) {
            probing.item = it->id;
            probing.w = it->w;
            probing.h = it->h;
        }
    m_probing[id] = probing;
    if (on_document_change)
        on_document_change();

    auto gate = m_load_gate;
    ComposerView *view = this;
    const uint64_t epoch = m_doc_epoch;
    importer().post([gate, view, epoch, path, id, done]() {
        auto held = std::make_shared<ImageImport>(probe_image_file(path));
        async([gate, view, epoch, id, done, held]() {
            if (!gate->open || epoch != view->m_doc_epoch)
                return;                  // the view, or its document, is gone
            view->finish_probe(id, *held, done);
        });
    });
}

void ComposerView::finish_probe(uint32_t id, const ImageImport &imported,
                                const std::function<void(const std::string &)> &done) {
    auto pending = m_probing.find(id);
    if (pending == m_probing.end())
        return;
    const Probing probing = pending->second;
    m_probing.erase(pending);
    ImageAsset *asset = m_doc.find_image(id);
    if (!asset) {                        // undone before the header came back
        if (done)
            done(std::string());
        return;
    }
    if (!imported.ok) {
        m_load_failed.insert(id);        // the frame shows the missing picture
        if (screen())
            screen()->redraw();
        if (done)
            done(imported.error.empty() ? "couldn't read the picture" : imported.error);
        return;
    }

    /* The real metadata, part of the placement's undo step */
    asset->meta = imported.meta;
    asset->source_size = imported.stamp.size;
    asset->source_mtime = imported.stamp.mtime;
    if (!m_images->is_loaded(id))
        m_images->add_link(id, m_images->path_of(id), imported.meta);

    /* The frame takes the print size, unless it was resized meanwhile */
    size_t page = 0;
    Item *it = probing.item ? m_doc.find_item(probing.item, &page) : nullptr;
    if (it && it->image() && it->image()->asset == id &&
        std::fabs(it->w - probing.w) < 1e-3f && std::fabs(it->h - probing.h) < 1e-3f) {
        float w, h;
        frame_size(imported.meta, page, w, h);
        if (probing.column && w > 0.f) {
            /* Still the column's width, at the picture's real proportions */
            float x;
            fit_column(page, probing.col_x, probing.col_w, h / w, x, w, h);
            it->xf.e = x;
        }
        it->content = scale_placement(*it->image(), it->w, it->h, w, h);
        it->w = w;
        it->h = h;
        fit_caption(probing.caption, *it);
        if (probing.caption)
            recompose();
        selection_changed();
    }
    m_load_want.erase(id);               // ask again at the real size
    m_load_failed.erase(id);
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
    if (done)
        done(std::string());
}

void ComposerView::import_asset(const std::string &path,
                                std::function<void(const std::string &error)> done) {
    if (!m_images) {
        if (done) done("pictures aren't available");
        return;
    }
    const uint32_t id = m_images->take_id();
    auto gate = m_load_gate;
    ComposerView *view = this;
    const uint64_t epoch = m_doc_epoch;
    importer().post([gate, view, epoch, path, id, done]() {
        ImageImport imported = read_image_import(path);
        imported.bytes.clear();          // a link: the file is read when placed
        auto held = std::make_shared<ImageImport>(std::move(imported));
        async([gate, view, epoch, path, id, done, held]() {
            if (!gate->open || epoch != view->m_doc_epoch)
                return;
            std::string err;
            view->commit_import(id, path, std::move(*held), view->m_page, false, nullptr, &err);
            if (done)
                done(err);
        });
    });
}

void ComposerView::add_asset(ImageAsset asset) {
    if (!asset.id || m_doc.find_image(asset.id))
        return;
    push_undo();
    m_doc.add_image(std::move(asset));
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::remove_unused_images() {
    bool any = false;
    for (const ImageAsset &a : m_doc.images)
        if (!m_doc.image_placed(a.id)) {
            any = true;
            break;
        }
    if (!any)
        return;
    push_undo();
    m_doc.remove_unused_images();
    if (on_document_change)
        on_document_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::place_image(uint32_t asset) {
    if (!m_images || !check_active_layer())
        return;
    const ImageStore::Entry *e = m_images->find(asset);
    if (!e)
        return;
    const ImageMetadata meta = e->meta;
    push_undo();
    if (!m_doc.find_image(asset)) {
        ImageAsset a;
        a.id = asset;
        a.name = "image";
        a.meta = meta;
        m_doc.add_image(std::move(a));
    }
    place_frame(asset, meta, m_page);
    if (on_document_change)
        on_document_change();
}

void ComposerView::place_image(uint32_t asset, const Point &at) {
    place_image_at(asset, m_page, at);
}

void ComposerView::place_image_at(uint32_t asset, size_t page, const Point &at) {
    const ImageAsset *a = m_doc.find_image(asset);
    if (!a || page >= m_doc.pages.size() || !check_active_layer())
        return;
    const ImageMetadata meta = a->meta;
    push_undo();
    place_frame(asset, meta, page, &at);
    if (on_document_change)
        on_document_change();
}

/* ---- Picture loading ------------------------------------------------------
 *
 * A picture is live while a frame showing it is on screen. texture_for()
 * notes the live pictures as the view draws and asks for the display view it
 * needs; one background thread reads the file (a linked picture that isn't
 * loaded) and decodes it, so the UI thread never blocks on a share or a
 * decoder. Pictures that leave the screen stay loaded until the linked bytes
 * pass the budget; then the ones off screen longest go first.
 */

struct ComposerView::LoadResult {
    bool ok = false;
    std::string error;
    ImageStore::Bytes bytes;             // what was read (or the loaded bytes)
    ImageMetadata meta;                  // probed, when read here
    std::vector<uint8_t> proxy;
    int proxy_w = 0, proxy_h = 0;
};

void ComposerView::request_display(uint32_t id, int want) {
    if (!m_images)
        return;
    /* One request per size: a finished or failed one is not asked again
     * (the file may be smaller than its metadata says) */
    auto asked = m_load_want.find(id);
    if (asked != m_load_want.end() && asked->second >= want)
        return;
    ImageStore::Bytes bytes = m_images->source_of(id);
    const std::string path = bytes ? std::string() : m_images->path_of(id);
    if (!bytes && path.empty())
        return;                          // nothing to read it from
    m_load_want[id] = want;

    auto gate = m_load_gate;
    ComposerView *view = this;
    const uint64_t epoch = m_doc_epoch;
    loader().post([gate, view, epoch, id, want, bytes, path]() {
        auto r = std::make_shared<LoadResult>();
        r->bytes = bytes;
        if (!r->bytes) {
            ImageImport imported = read_image_import(path);
            if (!imported.ok) {
                r->error = imported.error;
            } else {
                r->meta = imported.meta;
                r->bytes = std::make_shared<const std::vector<uint8_t>>(std::move(imported.bytes));
            }
        }
        if (r->bytes)
            r->ok = make_display(r->bytes->data(), r->bytes->size(), want, &r->proxy,
                                 &r->proxy_w, &r->proxy_h, &r->error);
        async([gate, view, epoch, id, path, r]() {
            if (!gate->open || epoch != view->m_doc_epoch)
                return;
            view->finish_display(id, path, *r);
        });
    }, std::to_string(id));
}

void ComposerView::finish_display(uint32_t id, const std::string &path, LoadResult &r) {
    if (!m_images)
        return;
    if (!r.ok) {
        m_load_failed.insert(id);        // drawn as missing, not retried
        if (screen())
            screen()->redraw();
        return;
    }
    if (!path.empty()) {
        /* Read here: install unless it was relinked, loaded another way
         * (Export), or left the screen while the file was read */
        if (m_images->path_of(id) != path || m_images->is_loaded(id) || !m_live.count(id)) {
            m_load_want.erase(id);
            return;
        }
        m_images->install(id, r.bytes, r.meta, std::move(r.proxy), r.proxy_w, r.proxy_h);
    } else {
        if (m_images->source_of(id) != r.bytes)
            return;                      // unloaded or replaced meanwhile
        m_images->set_proxy(id, std::move(r.proxy), r.proxy_w, r.proxy_h);
    }
    if (screen())
        screen()->redraw();
}

void ComposerView::drop_texture(uint32_t id) {
    auto it = m_tex.find(id);
    if (it == m_tex.end())
        return;
    if (it->second.id && screen() && screen()->nvg_context())
        nvgDeleteImage(screen()->nvg_context(), it->second.id);
    m_tex.erase(it);
}

/* After a draw: the pictures drawn on screen are the live set. Queued reads
 * for pictures that left are dropped, and linked bytes past the budget are
 * unloaded, longest off screen first. */
void ComposerView::update_live_images() {
    const uint64_t tick = ++m_live_tick;
    for (uint32_t id : m_live_frame)
        m_last_live[id] = tick;
    if (m_live_frame == m_live) {
        m_live_frame.clear();
        return;
    }
    m_live.swap(m_live_frame);
    m_live_frame.clear();

    if (m_loader)
        m_loader->drop_if([this](const std::string &key) {
            const uint32_t id = (uint32_t) std::strtoul(key.c_str(), nullptr, 10);
            if (m_live.count(id))
                return false;
            m_load_want.erase(id);       // never ran: ask again when it returns
            return true;
        });

    if (m_images && m_images->unloadable_bytes() > m_image_budget) {
        std::vector<std::pair<uint64_t, uint32_t>> idle;   // (last live tick, id)
        for (const ImageAsset &a : m_doc.images)
            if (!m_live.count(a.id) && m_images->is_loaded(a.id) && !m_images->path_of(a.id).empty())
                idle.emplace_back(m_last_live.count(a.id) ? m_last_live[a.id] : 0, a.id);
        std::sort(idle.begin(), idle.end());
        for (const auto &it : idle) {
            if (m_images->unloadable_bytes() <= m_image_budget)
                break;
            m_images->unload(it.second);
            drop_texture(it.second);
            m_load_want.erase(it.second);
        }
    }
    if (on_live_change)
        on_live_change();
}

/* Does the frame being drawn (current transform, clip_w x clip_h) touch the
 * scroll panel's window? */
bool ComposerView::frame_on_screen(NVGcontext *ctx, const DrawImage &im) const {
    float xf[6];
    nvgCurrentTransform(ctx, xf);
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    const float cx[4] = {0.f, im.clip_w, 0.f, im.clip_w};
    const float cy[4] = {0.f, 0.f, im.clip_h, im.clip_h};
    for (int i = 0; i < 4; ++i) {
        const float sx = xf[0] * cx[i] + xf[2] * cy[i] + xf[4];
        const float sy = xf[1] * cx[i] + xf[3] * cy[i] + xf[5];
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    const Widget *view = parent() ? parent() : this;
    const Vector2i a = view->absolute_position(), s = view->size();
    return x1 >= a.x() && x0 <= a.x() + s.x() && y1 >= a.y() && y0 <= a.y() + s.y();
}

/* ---- Drag and drop ------------------------------------------------------- */

uint32_t ComposerView::dragged_asset(const DragData &data) const {
    const std::string *id = data.get(kImageAssetDragType);
    if (!id)
        return 0;
    uint32_t asset = (uint32_t) std::strtoul(id->c_str(), nullptr, 10);
    return m_doc.find_image(asset) ? asset : 0;
}

void ComposerView::drop_target(const DragData &data, Point &at, size_t &page,
                               float &w, float &h) const {
    /* The screen's pointer is current during a drag (full precision, zoom
       applied); event points are rounded */
    at = page_point_from_screen(screen()->mouse_pos());
    page = m_point_page;
    w = h = 0.f;
    const uint32_t asset = dragged_asset(data);
    if (asset) {
        frame_size(m_doc.find_image(asset)->meta, page, w, h);
    } else if (const std::string *px = data.get(kImagePixelsDragType)) {
        ImageMetadata guess;             // the outline of the stand-in it will place
        if (std::sscanf(px->c_str(), "%d %d", &guess.width_px, &guess.height_px) == 2)
            frame_size(guess, page, w, h);
    }
    if (!m_snap)
        return;

    /* Snap like a moved frame, to that page's guides: any edge or the
       center when the size is real (a publication picture), else the
       top-left (a file's frame takes its size once its header is read) */
    const float tol = kSnapPx / zoom();
    auto nudge = [&](std::initializer_list<float> edges, const std::vector<float> &gs) {
        float best = tol + 1.f;
        for (float e : edges)
            for (float g : gs)
                if (std::fabs(g - e) < std::fabs(best))
                    best = g - e;
        return std::fabs(best) <= tol ? best : 0.f;
    };
    const std::vector<float> gx = guides_x(page), gy = guides_y(page);
    if (asset && w > 0.f && h > 0.f) {
        at.x += nudge({at.x, at.x + w * 0.5f, at.x + w}, gx);
        at.y += nudge({at.y, at.y + h * 0.5f, at.y + h}, gy);
    } else {
        at.x += nudge({at.x}, gx);
        at.y += nudge({at.y}, gy);
    }
}

bool ComposerView::drag_motion_event(const Vector2i &, DragEvent &e) {
    const uint32_t asset = dragged_asset(e.data);
    const bool files = !asset && on_drop_files && e.data.has("text/uri-list");
    if (!(asset || files) || m_placing || !e.accept(this, DropCopy)) {
        drag_leave_event();
        return false;
    }
    drop_target(e.data, m_drop_at, m_drop_page, m_drop_w, m_drop_h);
    m_drop_hover = true;
    screen()->redraw();
    return true;
}

void ComposerView::drag_leave_event() {
    if (!m_drop_hover)
        return;
    m_drop_hover = false;
    if (screen())
        screen()->redraw();
}

bool ComposerView::drag_drop_event(const Vector2i &, DragEvent &e) {
    drag_leave_event();
    Point at;
    size_t page = 0;
    float w, h;
    drop_target(e.data, at, page, w, h);   // where the outline showed it
    if (const uint32_t asset = dragged_asset(e.data)) {
        place_image_at(asset, page, at);
    } else if (on_drop_files && e.data.has("text/uri-list")) {
        std::vector<std::string> paths = e.data.file_paths();
        if (paths.empty())
            return false;
        Vector2i hint(0, 0);
        if (const std::string *px = e.data.get(kImagePixelsDragType))
            if (std::sscanf(px->c_str(), "%d %d", &hint.x(), &hint.y()) != 2)
                hint = Vector2i(0, 0);
        on_drop_files(paths, page, at, hint);
    } else {
        return false;
    }
    request_focus();
    return e.accept(this, DropCopy);
}

void ComposerView::sync_textures(NVGcontext *ctx) {
    const uint64_t gen = m_images ? m_images->generation() : 0;
    if (gen == m_tex_gen)
        return;
    for (auto &kv : m_tex)
        if (kv.second.id)
            nvgDeleteImage(ctx, kv.second.id);
    m_tex.clear();
    m_tex_gen = gen;
}

int ComposerView::texture_for(NVGcontext *ctx, const DrawImage &im, float px) {
    if (!m_images || !im.asset)
        return 0;
    const ImageStore::Entry *e = m_images->find(im.asset);
    if (!e)
        return 0;
    if (!frame_on_screen(ctx, im)) {     // off screen: not live, nothing to load
        auto it = m_tex.find(im.asset);
        return it != m_tex.end() && it->second.id ? it->second.id : -1;
    }
    m_live_frame.insert(im.asset);
    int src_long = std::max(e->meta.width_px, e->meta.height_px);
    if (src_long < 1)
        src_long = 1;
    const float pts = std::max(std::fabs(im.w), std::fabs(im.h));
    int want = px > 0.f ? (int) std::ceil(pts / px) : src_long;
    int q = 64;
    while (q < want && q < 8192)
        q *= 2;
    want = std::min(q, src_long);
    const uint8_t *rgba = nullptr;
    int w = 0, h = 0;
    if (m_probing.count(im.asset))
        return -1;                        // placed, its header still on the way
    const bool failed = m_load_failed.count(im.asset) != 0;
    const bool have = m_images->cached_display(im.asset, &rgba, &w, &h) && rgba && w > 0 && h > 0;
    if (!failed && (!have || std::max(w, h) < want))
        request_display(im.asset, want);  // a smaller view stands in meanwhile
    if (!have)
        return failed ? 0 : -1;           // 0 draws the missing cross, -1 a loading tile
    auto it = m_tex.find(im.asset);
    if (it != m_tex.end() && it->second.id && it->second.w == w && it->second.h == h)
        return it->second.id;
    if (it != m_tex.end() && it->second.id)
        nvgDeleteImage(ctx, it->second.id);
    const int id = nvgCreateImageRGBA(ctx, w, h, 0, rgba);
    m_tex[im.asset] = {id, w, h};
    return id;
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

    std::vector<PageSlot> slots;
    float aw, ah;
    arrangement(m_doc, m_page, m_page_view, slots, aw, ah);
    const Vector2f o = page_origin();
    sync_textures(ctx);
    std::vector<ItemId> lifted;
    lifted_ids(lifted);
    const std::vector<ItemId> *hand = lifted.empty() ? nullptr : &lifted;

    /* Paper for every page first. An item that crosses onto the next page
     * is then drawn over that page, not under its sheet. */
    for (const PageSlot &sl : slots) {
        nvgSave(ctx);
        nvgTranslate(ctx, o.x() + sl.x, o.y() + sl.y);
        if (sl.index < m_doc.pages.size() && m_doc.pages[sl.index].hidden) {
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 11.f * px);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
            nvgFillColor(ctx, nvgRGB(140, 40, 40));
            nvgText(ctx, 0, -4.f * px, "Hidden \u2014 not printed", nullptr);
        }
        draw_page(ctx, px, sl.index);
        nvgRestore(ctx);
    }

    auto paint_items = [&](bool only) {
        for (const PageSlot &sl : slots) {
            nvgSave(ctx);
            nvgTranslate(ctx, o.x() + sl.x, o.y() + sl.y);
            draw_list(ctx, build_page(m_doc, sl.index, m_comp, hand, only), px,
                      [&](const DrawImage &im) { return texture_for(ctx, im, px); });
            if (sl.index < m_doc.pages.size())
                for (const Item &it : m_doc.pages[sl.index].items)
                    if (it.is_text() && keep_item(hand, only, it.id) && m_doc.item_visible(it))
                        draw_text_overlays(ctx, it, px);
            if (!only)
                draw_ruler_guides(ctx, px, sl.index);
            draw_chrome(ctx, px, sl.index, hand, only);
            nvgRestore(ctx);
        }
    };
    paint_items(false);
    if (hand)
        paint_items(true);               // the item in hand, above both pages

    /* A drag over the view: the frame it would place, or a place-gun mark
     * while its size isn't known yet */
    if (m_drop_hover)
        for (const PageSlot &sl : slots) {
            if (sl.index != m_drop_page)
                continue;
            nvgSave(ctx);
            nvgTranslate(ctx, o.x() + sl.x + m_drop_at.x, o.y() + sl.y + m_drop_at.y);
            nvgBeginPath(ctx);
            if (m_drop_w > 0.f && m_drop_h > 0.f) {
                nvgRect(ctx, 0, 0, m_drop_w, m_drop_h);
            } else {
                nvgRect(ctx, 0, 0, 24.f * px, 18.f * px);
                nvgMoveTo(ctx, 0, 0);
                nvgLineTo(ctx, 24.f * px, 18.f * px);
            }
            nvgStrokeColor(ctx, kColumnGuide);
            nvgStrokeWidth(ctx, 1.5f * px);
            nvgStroke(ctx);
            nvgRestore(ctx);
        }

    nvgRestore(ctx);
    update_live_images();
}
