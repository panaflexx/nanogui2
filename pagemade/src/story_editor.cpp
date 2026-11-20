/*
 * pagemade/story_editor.cpp — ComposerView's story edit mode.
 *
 * The story being edited takes over the view as one column of text, its
 * paragraph styles named in a strip down the left. The view composes the
 * story again for the column: Simple sets it in one screen face at the
 * window's width; Column keeps its type and composes into its own blocks'
 * measures, then on into made-up blocks for the overset text, so its lines
 * break as they do in the layout. Each frame the column composes into is
 * a segment, stacked one under the next with a divider naming it.
 *
 * The caret, selection, typing and the panels work on the real story, as
 * in the layout; only edit_comp() (the composition the caret moves
 * through) is the column's. The document as it was is kept: Cancel puts
 * it back, Update leaves one undo step in place of the session's.
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

constexpr float kStyleColumn = 132.f;    // the style names' strip
constexpr float kColumnGap = 18.f;
constexpr float kTopPad = 22.f;
constexpr float kRightPad = 28.f;
constexpr float kDivider = 24.f;          // a segment's heading
constexpr float kSegGap = 8.f;
const NVGcolor kRed = nvgRGB(210, 40, 40);

/* A run's first character, from the paragraph text */
uint32_t cp_at(const std::string &t, uint32_t byte) {
    if (byte >= t.size())
        return 0;
    const unsigned char c = (unsigned char) t[byte];
    if (c < 0x80)
        return c;
    if ((c >> 5) == 6 && byte + 1 < t.size())
        return ((c & 0x1F) << 6) | (t[byte + 1] & 0x3F);
    if ((c >> 4) == 14 && byte + 2 < t.size())
        return ((c & 0x0F) << 12) | ((t[byte + 1] & 0x3F) << 6) | (t[byte + 2] & 0x3F);
    return 0xFFFD;
}

} // namespace

/* ---- Entering and leaving ---------------------------------------------- */

bool ComposerView::begin_story_edit() {
    if (m_story_mode)
        return true;
    flush_deferred();
    /* The story being typed in; with the pointer, the selected block's */
    StoryId sid = m_tool == Tool::Text ? m_edit_story : 0;
    if (!sid)
        for (ItemId id : m_sel)
            if (const StoryEntry *se = m_doc.story_of(id)) {
                sid = se->id;
                break;
            }
    if (!sid || !m_doc.find_story(sid) || !m_fonts)
        return false;
    if (m_placing)
        end_placement(false);
    m_story_tool = m_tool;
    m_story_sel = m_sel;
    if (m_tool != Tool::Text)
        set_tool(Tool::Text);
    if (sid != m_edit_story) {
        m_edit_story = sid;
        m_caret = m_anchor = TextPos{};
    }
    m_burst_open = false;
    m_typing_on = false;
    m_goal_x = -1.f;
    m_story_saved = m_doc;
    m_story_saved_version = m_version;
    m_redo.clear();                      // a redo would reach past the draft
    m_story_undo_base = m_undo.size();
    if (auto *panel = dynamic_cast<ZoomScrollPanel *>(parent())) {
        m_story_layout_zoom = panel->zoom();
        m_story_layout_pan = panel->pan_offset();
        panel->set_zoom(1.0);
        panel->set_pan_offset(ZoomScrollPanel::Vector2d(0.0, 0.0));
    }
    m_story_mode = true;
    m_story_view_w = 0.f;
    layout_story_view();
    story_panel_relayout();
    m_story_follow = true;
    request_focus();
    if (on_story_mode_change)
        on_story_mode_change();
    selection_changed();
    if (screen())
        screen()->redraw();
    return true;
}

void ComposerView::commit_story_edit() {
    if (!m_story_mode || !story_changed())
        return;
    flush_deferred();
    while (m_undo.size() > m_story_undo_base)
        m_undo.pop_back();
    m_undo.push_back({m_story_saved, m_story_saved_version});
    m_story_undo_base = m_undo.size();
    m_story_saved = m_doc;
    m_story_saved_version = m_version;
    m_burst_open = false;
}

void ComposerView::end_story_edit(bool apply) {
    if (!m_story_mode)
        return;
    flush_deferred();
    const bool changed = story_changed();
    while (m_undo.size() > m_story_undo_base)
        m_undo.pop_back();
    m_redo.clear();
    if (apply && changed) {
        m_undo.push_back({std::move(m_story_saved), m_story_saved_version});
        while (m_undo.size() > 100)
            m_undo.pop_front();
    } else if (!apply && changed) {
        m_doc = std::move(m_story_saved);
        m_version = m_story_saved_version;
    }
    m_story_saved = PageDoc();
    m_story_mode = false;
    m_story_comp = Composition();
    m_story_segs.clear();
    m_burst_open = false;
    m_selecting = false;
    if (m_edit_story && !m_doc.find_story(m_edit_story))
        m_edit_story = 0;
    if (const Story *s = edit_story()) {
        m_caret = clamp(*s, m_caret);
        m_anchor = clamp(*s, m_anchor);
    }
    recompose();
    if (auto *panel = dynamic_cast<ZoomScrollPanel *>(parent())) {
        panel->set_zoom(m_story_layout_zoom);
        panel->set_pan_offset(m_story_layout_pan);
    }
    story_panel_relayout();
    /* Back on the caret's block's page, the caret where it was. Overset
     * text has no place in the layout: the last block's page. */
    if (const StoryEntry *se = m_doc.find_story(m_edit_story); se && !se->thread.empty()) {
        const size_t si = m_doc.story_index(se->id);
        ItemId frame = se->thread.back();
        if (si < m_comp.size()) {
            const Caret at = caret_at(m_comp[si], se->story, m_caret);
            if (at.valid() && m_comp[si].lines[at.line].frame < se->thread.size())
                frame = se->thread[m_comp[si].lines[at.line].frame];
        }
        size_t page = m_page;
        if (m_doc.find_item(frame, &page) && page != m_page)
            show_page(page);
    }
    if (on_document_change)
        on_document_change();
    if (on_story_mode_change)
        on_story_mode_change();
    selection_changed();
    if (screen())
        screen()->redraw();
}

void ComposerView::set_story_view(StoryView v) {
    if (m_story_view == v)
        return;
    m_story_view = v;
    if (!m_story_mode)
        return;
    request_focus();                     // typing goes on after the bar's button
    layout_story_view();
    story_panel_relayout();
    m_story_follow = true;
    if (on_story_mode_change)
        on_story_mode_change();
    if (screen())
        screen()->redraw();
}

void ComposerView::set_story_font(const std::string &family, float size) {
    m_story_family = family.empty() ? std::string("Sans") : family;
    m_story_size = std::clamp(size, 6.f, 72.f);
    if (m_story_mode) {
        layout_story_view();
        story_panel_relayout();
        if (screen())
            screen()->redraw();
    }
}

std::string ComposerView::story_title() const {
    const Story *s = edit_story();
    if (!s)
        return {};
    std::string out;
    for (const Paragraph &p : s->paragraphs) {
        std::string t = paragraph_text(p);
        for (char &c : t)
            if ((unsigned char) c < 32)
                c = ' ';
        if (!out.empty() && !t.empty())
            out += ' ';
        out += t;
        if (out.size() > 40)
            break;
    }
    /* Cut at a word, whole characters only */
    if (out.size() > 40) {
        size_t cut = out.rfind(' ', 40);
        if (cut == std::string::npos || cut < 20)
            cut = 40;
        while (cut > 0 && ((unsigned char) out[cut] & 0xC0) == 0x80)
            --cut;
        out = out.substr(0, cut) + "…";
    }
    return out.empty() ? std::string("(empty story)") : out;
}

/* ---- Composing the column ------------------------------------------------ */

Story ComposerView::simple_story(const Story &s) const {
    Story out = s;
    for (Paragraph &p : out.paragraphs) {
        ParaStyle &ps = p.style;
        if (is_justified(ps.align))
            ps.align = Align::Left;
        ps.drop_lines = 0;
        ps.keep_together = ps.keep_with_next = ps.start_new_frame = false;
        ps.orphans = ps.widows = 0;
        ps.leading_method = LeadingMethod::Proportional;
        ps.autoleading = 125.f;
        ps.line_spacing = 1.f;
        ps.extra_spacing = 0.f;
        ps.space_before = std::min(ps.space_before, 12.f);
        ps.space_after = std::min(ps.space_after, 12.f);
        ps.hyphenate = false;
        for (Run &r : p.runs) {
            CharStyle &cs = r.style;
            cs.family = m_story_family;
            cs.face.clear();
            cs.size = m_story_size;
            cs.leading = 0.f;
            cs.tracking = 0.f;
            cs.hscale = 100.f;
            cs.baseline_shift = 0.f;
        }
    }
    return out;
}

void ComposerView::layout_story_view() {
    m_story_segs.clear();
    m_story_comp = Composition();
    m_story_overset_y = -1.f;
    const StoryEntry *se = m_doc.find_story(m_edit_story);
    if (!se || !m_fonts) {
        m_story_w = m_story_h = 0.f;
        return;
    }
    const float tx = kStyleColumn + kColumnGap;
    /* Each frame break, and in Column each paragraph that starts a new
     * block, moves the text on a frame: the column has one for each */
    size_t breaks = 0, new_frames = 0;
    for (const Paragraph &p : se->story.paragraphs) {
        new_frames += p.style.start_new_frame ? 1 : 0;
        for (const Run &r : p.runs)
            breaks += (size_t) std::count(r.text.begin(), r.text.end(), '\x0C');
    }

    std::vector<Frame> frames;
    std::vector<std::string> labels;
    size_t blocks = 0;
    float widest = 0.f;
    const bool column = m_story_view == StoryView::Column;
    if (column) {
        frames = m_doc.thread_frames(*se);
        blocks = frames.size();
        for (size_t i = 0; i < frames.size(); ++i) {
            frames[i].valign = VAlign::Top;
            std::string label = "Block " + std::to_string(i + 1);
            if (frames[i].page > 0)
                label += "  —  page " + m_doc.page_label((size_t) frames[i].page - 1);
            labels.push_back(label);
        }
        Frame extra;
        if (!frames.empty())
            extra = frames.back();
        else
            extra.w = 360.f;
        extra.h = 1.0e6f;
        for (size_t i = 0; i <= breaks + new_frames; ++i) {
            frames.push_back(extra);
            labels.push_back("Overset  —  text that doesn't fit the blocks");
        }
        m_story_comp = compose(se->story, frames, *m_fonts, m_hyphenator);
    } else {
        const auto *panel = dynamic_cast<const ZoomScrollPanel *>(parent());
        const float z = panel && panel->zoom() > 0.0 ? (float) panel->zoom() : 1.f;
        m_story_view_w = panel ? (float) panel->width() / z : (float) width();
        Frame f;
        const std::vector<Frame> real = m_doc.thread_frames(*se);
        if (!real.empty()) {
            f.page = real.front().page;
            f.pages = real.front().pages;
            f.page_label = real.front().page_label;
            f.section_name = real.front().section_name;
        }
        f.w = std::max(144.f, m_story_view_w - tx - kRightPad);
        f.h = 1.0e6f;
        for (size_t i = 0; i <= breaks; ++i) {
            frames.push_back(f);
            labels.push_back(i == 0 ? std::string() : std::string("Frame break"));
        }
        m_story_comp = compose(simple_story(se->story), frames, *m_fonts, m_hyphenator);
    }

    float y = kTopPad;
    for (size_t i = 0; i < frames.size(); ++i) {
        float top = INFINITY, bottom = -INFINITY;
        for (const ComposedLine &l : m_story_comp.lines)
            if (l.frame == i) {
                top = std::min(top, l.top);
                bottom = std::max(bottom, l.top + l.leading);
            }
        const bool has = top <= bottom;
        const bool overset = column && i >= blocks;
        if (overset && !has)
            continue;                    // nothing left over (or no more breaks)
        StorySeg sg;
        sg.frame = i;
        sg.overset = overset;
        sg.width = frames[i].w;
        /* The overset's frames after the first carry on under one heading */
        if (!(overset && i > blocks && !m_story_segs.empty() && m_story_segs.back().overset))
            sg.label = labels[i];
        else
            sg.label = "Frame break";
        if (!sg.label.empty())
            y += kDivider;
        if (!has) {
            top = frames[i].y;
            bottom = top + 4.f;          // an empty block: its heading and a sliver
        }
        sg.x = tx - frames[i].x;
        sg.shift = y - top;
        sg.top = y;
        sg.bottom = y + (bottom - top);
        y = sg.bottom + kSegGap;
        widest = std::max(widest, frames[i].w);
        m_story_segs.push_back(sg);
    }
    m_story_h = y + kTopPad * 2.f;
    m_story_w = column ? tx + widest + kRightPad : m_story_view_w;

    /* Simple sets everything in one column: the overset point is where the
     * layout's blocks run out, marked across it */
    const size_t si = m_doc.story_index(se->id);
    if (!column && si < m_comp.size() && m_comp[si].overset) {
        const TextPos at{m_comp[si].overset_para, m_comp[si].overset_byte};
        const Caret c = caret_at(m_story_comp, se->story, at);
        if (c.valid())
            if (const StorySeg *sg = story_seg_of_line(c.line))
                m_story_overset_y = m_story_comp.lines[c.line].top + sg->shift;
    }
}

const ComposerView::StorySeg *ComposerView::story_seg_of_line(size_t line) const {
    if (line >= m_story_comp.lines.size())
        return nullptr;
    const size_t f = m_story_comp.lines[line].frame;
    for (const StorySeg &sg : m_story_segs)
        if (sg.frame == f)
            return &sg;
    return nullptr;
}

/* The scroll panel sizes the view from its preferred size when it lays
 * out; a new zoom asks it to */
void ComposerView::story_panel_relayout() {
    if (auto *panel = dynamic_cast<ZoomScrollPanel *>(parent()))
        panel->set_zoom(panel->zoom());
}

TextPos ComposerView::story_pos_at(const Vector2f &p) const {
    const Story *s = edit_story();
    if (!s || m_story_segs.empty())
        return {};
    const StorySeg *best = &m_story_segs.front();
    float best_d = INFINITY;
    for (const StorySeg &sg : m_story_segs) {
        const float d = p.y() < sg.top ? sg.top - p.y() : p.y() > sg.bottom ? p.y() - sg.bottom : 0.f;
        if (d < best_d) {
            best_d = d;
            best = &sg;
        }
    }
    return hit_test_frame(m_story_comp, *s, best->frame, p.x() - best->x, p.y() - best->shift);
}

void ComposerView::follow_story_caret() {
    m_story_follow = false;
    const Story *s = edit_story();
    auto *panel = dynamic_cast<ZoomScrollPanel *>(parent());
    if (!s || !panel)
        return;
    const Caret at = caret_at(m_story_comp, *s, m_caret);
    const StorySeg *sg = at.valid() ? story_seg_of_line(at.line) : nullptr;
    if (!sg)
        return;
    const double z = panel->zoom() > 0.0 ? panel->zoom() : 1.0;
    const double top = (at.top + sg->shift + m_pos.y()) * z, bottom = (at.bottom + sg->shift + m_pos.y()) * z;
    const double x = (at.x + sg->x + m_pos.x()) * z;
    ZoomScrollPanel::Vector2d pan = panel->pan_offset();
    const double h = panel->height(), w = panel->width();
    const double margin = std::min(48.0, h * 0.2);
    if (top + pan.y() < margin)
        pan.y() = margin - top;
    else if (bottom + pan.y() > h - margin)
        pan.y() = h - margin - bottom;
    if (x + pan.x() < 24.0)
        pan.x() = 24.0 - x;
    else if (x + pan.x() > w - 24.0)
        pan.x() = w - 24.0 - x;
    panel->set_pan_offset(pan);
}

/* ---- Rulers -------------------------------------------------------------- */

float ComposerView::ruler_extent(bool horizontal) const {
    if (!m_story_mode)
        return horizontal ? m_doc.setup.width : m_doc.setup.height;
    if (!horizontal)
        return std::max(0.f, m_story_h - kTopPad * 3.f);
    const Story *s = edit_story();
    const Caret at = s ? caret_at(m_story_comp, *s, m_caret) : Caret();
    const StorySeg *sg = at.valid() ? story_seg_of_line(at.line) : nullptr;
    if (!sg && !m_story_segs.empty())
        sg = &m_story_segs.front();
    return sg ? sg->width : 0.f;
}

bool ComposerView::ruler_indents(RulerIndents &out) const {
    const Story *s = edit_story();
    if (!m_story_mode || !s || m_caret.para >= s->paragraphs.size())
        return false;
    const ParaStyle &ps = s->paragraphs[m_caret.para].style;
    const float w = ruler_extent(true);
    out.left = ps.left_indent;
    out.first = ps.left_indent + ps.first_indent;
    out.right = w - ps.right_indent;
    out.tabs = ps.tabs;
    return true;
}

/* ---- Mouse ---------------------------------------------------------------- */

bool ComposerView::story_mouse_button(int button, bool down, int modifiers) {
    if (button == GLFW_MOUSE_BUTTON_2) {
        /* The text's menu: a click outside the selection moves the caret */
        if (down && on_context_menu) {
            request_focus();
            if (!has_selection())
                place_caret(story_pos_at(local_from_screen(screen()->mouse_pos())), false);
            on_context_menu(screen()->mouse_pos());
        }
        return true;
    }
    if (button != GLFW_MOUSE_BUTTON_1)
        return true;
    const Vector2f p = local_from_screen(screen()->mouse_pos());
    Story *s = edit_story();
    if (!down) {
        m_selecting = false;
        m_pointer_down = false;
        return true;
    }
    if (!s)
        return true;
    request_focus();
    m_pointer_down = true;
    m_burst_open = false;
    m_merged_key = nullptr;
    m_typing_on = false;
    m_goal_x = -1.f;
    const bool shift = (modifiers & GLFW_MOD_SHIFT) != 0;
    const double now = glfwGetTime();
    const bool near = std::fabs(p.x() - m_story_down.x()) + std::fabs(p.y() - m_story_down.y()) <= 4.f;
    if (!shift && m_last_click >= 0.0 && now - m_last_click < 0.4 && near)
        m_clicks = std::min(m_clicks + 1, 3);
    else
        m_clicks = 1;
    m_last_click = now;
    m_story_down = p;
    const TextPos pos = story_pos_at(p);
    if (p.x() < kStyleColumn && !shift) {
        /* The style strip: the whole paragraph */
        m_anchor = {pos.para, 0};
        m_caret = {pos.para, para_length(s->paragraphs[pos.para])};
    } else if (shift || m_clicks <= 1) {
        place_caret(pos, shift);
        m_selecting = true;
    } else if (m_clicks == 2) {
        const auto w = word_at(*s, pos);
        m_anchor = w.first;
        m_caret = w.second;
    } else {
        m_anchor = {pos.para, 0};
        m_caret = {pos.para, para_length(s->paragraphs[pos.para])};
    }
    selection_changed();
    if (screen())
        screen()->redraw();
    return true;
}

/* ---- Drawing ---------------------------------------------------------------- */

void ComposerView::draw_story_view(NVGcontext *ctx, float px) {
    if (m_story_view == StoryView::Simple) {
        const auto *panel = dynamic_cast<const ZoomScrollPanel *>(parent());
        const float z = panel && panel->zoom() > 0.0 ? (float) panel->zoom() : 1.f;
        const float vw = panel ? (float) panel->width() / z : (float) width();
        if (std::fabs(vw - m_story_view_w) > 1.f) {
            layout_story_view();         // the window or the zoom changed the measure
            story_panel_relayout();
        }
    }
    if (m_story_follow)
        follow_story_caret();
    const StoryEntry *se = m_doc.find_story(m_edit_story);
    const float tx = kStyleColumn + kColumnGap;

    /* What shows: the panel's viewport in this view's space */
    float y0 = -1e9f, y1 = 1e9f;
    {
        float cur[6], inv[6];
        nvgCurrentTransform(ctx, cur);
        if (nvgTransformInverse(inv, cur)) {
            const Widget *port = parent() ? parent() : this;
            const Vector2i pa = port->absolute_position(), ps = port->size();
            y0 = inv[1] * pa.x() + inv[3] * pa.y() + inv[5];
            y1 = inv[1] * pa.x() + inv[3] * (pa.y() + ps.y()) + inv[5];
        }
    }

    const float full_h = std::max(m_story_h, (float) height()) + 2e4f;
    nvgBeginPath(ctx);
    nvgRect(ctx, -1e5f, -1e4f, 2e5f, full_h + 2e4f);
    nvgFillColor(ctx, nvgRGB(255, 255, 255));
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgRect(ctx, -1e5f, -1e4f, 1e5f + kStyleColumn, full_h + 2e4f);
    nvgFillColor(ctx, nvgRGB(240, 240, 236));
    nvgFill(ctx);
    nvgBeginPath(ctx);
    nvgMoveTo(ctx, kStyleColumn + 0.5f * px, -1e4f);
    nvgLineTo(ctx, kStyleColumn + 0.5f * px, full_h);
    nvgStrokeColor(ctx, nvgRGB(205, 205, 200));
    nvgStrokeWidth(ctx, px);
    nvgStroke(ctx);
    if (!se) {
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 13.f);
        nvgFillColor(ctx, nvgRGB(120, 120, 120));
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgText(ctx, tx, kTopPad, "The story is gone.", nullptr);
        return;
    }
    const Story &story = se->story;

    /* Segment headings */
    for (const StorySeg &sg : m_story_segs) {
        if (sg.label.empty() || sg.top < y0 - 40.f || sg.top - kDivider > y1)
            continue;
        const float hy = sg.top - kDivider * 0.5f;
        const NVGcolor c = sg.overset ? kRed : nvgRGB(130, 130, 150);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, tx, hy + 0.5f);
        nvgLineTo(ctx, tx + sg.width, hy + 0.5f);
        nvgStrokeColor(ctx, c);
        nvgStrokeWidth(ctx, sg.overset ? 1.5f * px : px);
        nvgStroke(ctx);
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 10.f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        float b[4];
        nvgTextBounds(ctx, 0, 0, sg.label.c_str(), nullptr, b);
        nvgBeginPath(ctx);
        nvgRect(ctx, tx + 8.f, hy - 7.f, b[2] - b[0] + 8.f, 14.f);
        nvgFillColor(ctx, nvgRGB(255, 255, 255));
        nvgFill(ctx);
        nvgFillColor(ctx, c);
        nvgText(ctx, tx + 12.f, hy, sg.label.c_str(), nullptr);
    }
    if (m_story_overset_y >= 0.f) {
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, 0, m_story_overset_y);
        nvgLineTo(ctx, m_story_w, m_story_overset_y);
        nvgStrokeColor(ctx, kRed);
        nvgStrokeWidth(ctx, 1.5f * px);
        nvgStroke(ctx);
        nvgFontFace(ctx, "sans-bold");
        nvgFontSize(ctx, 10.f);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(ctx, kRed);
        nvgText(ctx, 8.f, m_story_overset_y + 2.f, "Overset below", nullptr);
    }

    /* The type, black on white whatever its color, and the markers for
     * what doesn't print as itself */
    DrawList list;
    std::vector<std::string> texts(story.paragraphs.size());
    std::vector<char> have(story.paragraphs.size(), 0);
    auto text_of = [&](size_t para) -> const std::string & {
        if (!have[para]) {
            texts[para] = paragraph_text(story.paragraphs[para]);
            have[para] = 1;
        }
        return texts[para];
    };
    const pagemade::Color black{0.f, 0.f, 0.f, 1.f};
    size_t last_para = SIZE_MAX;
    for (size_t li = 0; li < m_story_comp.lines.size(); ++li) {
        const ComposedLine &l = m_story_comp.lines[li];
        const StorySeg *sg = story_seg_of_line(li);
        if (!sg || l.para >= story.paragraphs.size())
            continue;
        const float top = l.top + sg->shift, bottom = top + l.leading;
        const bool first_line = l.para != last_para;
        last_para = l.para;
        if (bottom < y0 || top > y1)
            continue;
        const Transform xf = Transform::translate(sg->x, sg->shift);
        for (const GlyphRun &r : l.runs)
            list.push_back(DrawGlyphs{&r, xf, black});
        const std::string &t = text_of(l.para);
        float band_top, band_bottom;
        line_band(l, band_top, band_bottom);
        band_top += sg->shift;
        band_bottom += sg->shift;
        /* Page markers and right-indent tabs: a tint behind; tabs and
         * breaks: a mark in their place */
        for (const GlyphRun &r : l.runs)
            for (const PlacedGlyph &g : r.glyphs) {
                const uint32_t cp = cp_at(t, g.cluster);
                const float gx = g.x + sg->x;
                if (cp >= 0xE000 && cp <= 0xE003) {
                    nvgBeginPath(ctx);
                    nvgRect(ctx, gx, band_top, std::max(3.f, g.adv), band_bottom - band_top);
                    nvgFillColor(ctx, nvgRGBA(70, 140, 255, 50));
                    nvgFill(ctx);
                } else if (cp == '\t' || cp == 0x2028) {
                    nvgFontFace(ctx, "sans");
                    nvgFontSize(ctx, std::max(8.f, (band_bottom - band_top) * 0.7f));
                    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
                    nvgFillColor(ctx, nvgRGB(120, 150, 210));
                    nvgText(ctx, gx + 1.f, g.y + sg->shift, cp == '\t' ? "→" : "↵", nullptr);
                }
            }
        if (l.para_end) {
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, std::max(8.f, (band_bottom - band_top) * 0.7f));
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
            nvgFillColor(ctx, nvgRGB(185, 185, 200));
            nvgText(ctx, l.x_end + sg->x + 2.f, l.baseline + sg->shift, "¶", nullptr);
        }
        if (first_line && l.byte_start == 0) {
            /* The paragraph's style, in the strip */
            const std::string &name = story.paragraphs[l.para].style.name;
            std::string shown = name.empty() ? std::string("(no style)") : name;
            nvgFontFace(ctx, "sans");
            nvgFontSize(ctx, 11.f);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
            const float room = kStyleColumn - 14.f;
            float b[4];
            while (shown.size() > 1 && nvgTextBounds(ctx, 0, 0, shown.c_str(), nullptr, b) > room) {
                size_t cut = shown.size() - 1;
                while (cut > 0 && ((unsigned char) shown[cut] & 0xC0) == 0x80)
                    --cut;
                if (shown.size() >= 3 && shown.compare(shown.size() - 3, 3, "…") == 0) {
                    shown.erase(shown.size() - 3);
                    cut = shown.size() - 1;
                    while (cut > 0 && ((unsigned char) shown[cut] & 0xC0) == 0x80)
                        --cut;
                }
                shown = shown.substr(0, cut) + "…";
            }
            nvgFillColor(ctx, name.empty() ? nvgRGB(150, 150, 150) : nvgRGB(70, 70, 80));
            nvgText(ctx, kStyleColumn - 8.f, l.baseline + sg->shift, shown.c_str(), nullptr);
        }
    }
    draw_list(ctx, list, px, [](const DrawImage &) { return -1; });

    /* The selection, or the caret */
    if (has_selection()) {
        TextPos lo = m_anchor, hi = m_caret;
        if (hi < lo)
            std::swap(lo, hi);
        nvgBeginPath(ctx);
        for (size_t li = 0; li < m_story_comp.lines.size(); ++li) {
            const ComposedLine &l = m_story_comp.lines[li];
            if (l.para < lo.para || l.para > hi.para)
                continue;
            const StorySeg *sg = story_seg_of_line(li);
            if (!sg)
                continue;
            uint32_t from = l.byte_start, to = l.byte_end;
            if (l.para == lo.para)
                from = std::max(from, lo.byte);
            if (l.para == hi.para)
                to = std::min(to, hi.byte);
            if (from > to)
                continue;
            const std::string &t = text_of(l.para);
            float x0 = x_at(l, t, from), x1 = x_at(l, t, to);
            if (from == to) {
                if (l.para >= hi.para)
                    continue;
                x1 = x0 + 6.f;
            } else if (to == l.byte_end && l.para_end && l.para < hi.para) {
                x1 += 6.f;
            }
            float top, bottom;
            line_band(l, top, bottom);
            nvgRect(ctx, x0 + sg->x, top + sg->shift, std::max(1.f, x1 - x0), bottom - top);
        }
        nvgFillColor(ctx, nvgRGBA(60, 120, 255, 90));
        nvgFill(ctx);
    } else if (focused()) {
        const Caret at = caret_at(m_story_comp, story, m_caret);
        const StorySeg *sg = at.valid() ? story_seg_of_line(at.line) : nullptr;
        if (sg) {
            m_blink_wanted.store(true);
            if (caret_visible_now()) {
                nvgBeginPath(ctx);
                nvgRect(ctx, at.x + sg->x, at.top + sg->shift, std::max(px, 1.f), at.bottom - at.top);
                nvgFillColor(ctx, nvgRGB(0, 0, 0));
                nvgFill(ctx);
            }
        }
    }
}
