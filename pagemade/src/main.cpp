/*
 * pagemade — page layout in the spirit of Aldus PageMaker.
 *
 * One page on a pasteboard with text blocks and shapes. The menu bar is
 * PageMaker's (File, Edit, Layout, Element): pages are inserted, removed,
 * reordered and hidden from Layout, and Arrange lives under Element.
 * Element > Image Assets... browses pictures in this publication and in
 * folders, and Remove Unused drops pictures that are not placed.
 * Page icons along the bottom of the window show the document — click one
 * to turn to it, drag to rearrange. Beside them, three buttons show one
 * page, a side-by-side spread, or every page in a stack. File > New Document
 * asks for the page size, facing pages, margins and columns. The toolbar holds the toolbox
 * (pointer, crop, rotate, text, line, rectangle, ellipse, polygon), snap to
 * guides, undo/redo (Ctrl+Z / Ctrl+Shift+Z), a zoom dropdown, the ¶ button
 * for the Paragraph panel (style, alignment, spacing, indents), T for the
 * Type panel, A for the Character panel, and K/L/B/V toggles for kerning,
 * ligatures, baselines and loose/tight lines. Below it,
 * the control palette
 * switches between the object view (position, size, angle, fill and
 * stroke; with nothing selected, the defaults for new shapes) and the
 * type view (font, size, style, leading and baseline). T on the toolbar,
 * and Element > Type Specs…, open the Type panel: weight, style, caps and
 * the other character attributes. Apply writes them onto the selection.
 * PageMaker's zoom shortcuts: Ctrl/Cmd + 0 fit, 5 50%, 7 75%, 1 actual
 * size, 2 200%, 4 400%, 8 800%.
 *
 *   pagemade
 *   pagemade --screenshot out.png [--export-pdf out.pdf] [--zoom 4 --at 150,400] [--select 1,1]
 *            [--baselines] [--loose] [--no-kerning] [--no-snap]
 *            [--tool pointer|crop|text|rotate|line|rect|ellipse|polygon ...]
 *            [--fill swatch[,tint] ...] [--stroke swatch,weight[,style] ...]
 *            [--drag x0,y0:x1,y1 ...] [--click x,y ...] [--type txt ...]
 *            [--key [mod+]name ...] [--open file.pagemade] [--place picture.png]
 *            [--save file.pagemade ...] [--save-embedded file.pagemade ...]
 *            [--dialog save|changes|new] [--blank-pages n] [--page n] [--page-view one|spread|stack]
 *            [--print-title] [--caption-pictures] [--fit-columns]
 *            [--ui-click x,y ...] [--ui-rclick x,y ...] [--ui-drag x0,y0:x1,y1 ...]
 *            [--ui-scroll x,y:dy ...]
 *
 * --at is a page point (points from the page's top-left) to center on.
 * --select picks a text block by story and thread index ("1,0").
 * --drag presses at one page point and releases at another, through the
 * real event path, before the screenshot is taken. --click, --type and
 * --key (enter, backspace, left, ..., a-z; mods shift/ctrl/alt) exercise
 * the text tool the same way. --tool picks a tool, and --fill / --stroke
 * set the selection's paint as the control palette would (style: solid,
 * dashed, dotted, dashdot). --ui-click, --ui-drag and --ui-scroll act on the window
 * itself, in window pixels (the palettes, menus, dialogs). All of these run
 * in the order given.
 */
#include <nanogui/nanogui.h>
#include <nanogui/menu.h>
#include <nanogui/opengl.h>
#include <nanovg.h>
#include <nanogui/zoomscrollpanel.h>
#include <GLFW/glfw3.h>

#include "composer/hyphenator.h"
#include "composerview.h"
#include "dialogs.h"
#include "image_assets.h"
#include "layers_panel.h"
#include "ruler.h"
#include "docfile.h"
#include "default_fonts.h"
#include "font_menu_ui.h"
#include "system_fonts.h"
#include "page.h"
#include "paragraph_panel.h"
#include "pdf.h"
#include "pdf_dialog.h"
#include "spinbox.h"
#include "swatch_library.h"
#include "swatch_menu.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../ext/glfw/deps/stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <nanogui/fscompat.h>
#include <map>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef PAGEMADE_DATA_DIR
#define PAGEMADE_DATA_DIR "pagemade/resources"
#endif

using namespace nanogui;
using namespace pagemade;

/* A button whose caption is set in bold italic. Button always draws its
 * caption in sans-bold, so this one draws its own. */
class ItalicButton : public Button {
public:
    ItalicButton(Widget *parent, const std::string &caption)
        : Button(parent, ""), m_text(caption) {}

    void draw(NVGcontext *ctx) override {
        Button::draw(ctx);
        const int size = m_font_size == -1 ? m_theme->m_button_font_size : m_font_size;
        nvgFontSize(ctx, (float) size);
        nvgFontFace(ctx, "sans-bolditalic");
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, m_enabled ? m_theme->m_text_color : m_theme->m_disabled_text_color);
        const float down = m_pushed ? 1.f : 0.f;
        nvgText(ctx, m_pos.x() + m_size.x() * 0.5f, m_pos.y() + m_size.y() * 0.5f + down,
                m_text.c_str(), nullptr);
    }

private:
    std::string m_text;
};

/* PageMaker's page icons: a row of little pages at the bottom of the
 * window. Click turns to that page; drag rearranges them (the page lifts
 * and follows the pointer while the others slide aside). The row scrolls
 * with the wheel or the thin bar underneath when it overflows. With
 * facing pages the icons pair into spreads. A hidden page is gray. On the right, "+" inserts a page after the current one
 * and three buttons choose one page, a side-by-side spread, or every
 * page in a stack. */
class PageIconStrip : public Widget {
public:
    std::function<void()> on_turned;   // a click turned to a page
    std::function<void()> on_view;     // a view button changed the pasteboard
    std::function<void(const Vector2i &)> on_page_menu;   // right-click, screen coords

    PageIconStrip(Widget *parent, ComposerView *view) : Widget(parent), m_view(view) {
        set_height(kStripH);
        set_min_height(kStripH);
        set_height_flex(SizeMode::Fixed);
        /* Drawn every frame from the document and animated; a cached
         * parent must not bake it. */
        set_live(true);
        set_tooltip("Pages. Click to turn to a page, drag to rearrange, right-click for more.");
        m_add = new ToolButton(this, FA_PLUS);
        m_add->set_fixed_size(Vector2i(kBtn, kBtn));
        m_add->set_tooltip("Insert a page after this one");
        m_add->set_callback([this] { add_page(); });
        auto add = [&](PageView mode, int icon, const char *tip) {
            ToolButton *b = new ToolButton(this, icon);
            b->set_fixed_size(Vector2i(kBtn, kBtn));
            b->set_tooltip(tip);
            b->set_callback([this, mode] {
                m_view->set_page_view(mode);
                if (on_view)
                    on_view();
            });
            m_modes.push_back({mode, b});
        };
        add(PageView::One, FA_FILE, "One page");
        add(PageView::Spread, FA_COLUMNS, "Side by side");
        add(PageView::Stack, FA_GRIP_LINES, "One-page style");
        m_modes.front().second->set_pushed(true);
    }

    void draw(NVGcontext *ctx) override {
        const double now = glfwGetTime();
        const float dt = m_last_time < 0 ? 0.f : (float) std::min(now - m_last_time, 0.05);
        m_last_time = now;

        sync_buttons();
        place_buttons();
        sync_slots();
        const bool moving = step(dt);

        const int n = (int) m_view->page_count();
        const int cur = (int) m_view->page_index();
        const float left = (float) m_pos.x();
        const float right = (float) (m_pos.x() + view_width());
        const float y = (float) m_pos.y() + kIconTop;

        nvgSave(ctx);
        nvgIntersectScissor(ctx, left, (float) m_pos.y(), right - left, (float) m_size.y());
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 11);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        /* The lifted page draws last, over its neighbors. */
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < n; ++i) {
                const bool lifted = m_dragging && i == cur;
                if (lifted != (pass == 1))
                    continue;
                const float x = left + m_x[(size_t) i] - m_scroll;
                if (x + kW + 4 < left || x - 4 > right)
                    continue;
                float scale = 1.f;
                for (const auto &ap : m_appearing)
                    if (ap.first == m_uids[(size_t) i]) {
                        const float t = std::clamp((float) ((now - ap.second) / kAppearSecs), 0.f, 1.f);
                        scale = 1.f - (1.f - t) * (1.f - t);
                    }
                draw_icon(ctx, i, x, lifted ? y - 2.f : y, scale, i == cur, lifted);
            }
        nvgRestore(ctx);

        draw_scrollbar(ctx);

        nvgSave(ctx);
        nvgTranslate(ctx, (float) m_pos.x(), (float) m_pos.y());
        for (Widget *c : children())
            if (c->visible())
                c->draw(ctx);
        nvgRestore(ctx);

        /* Keep the frames coming only while something is moving. */
        if (moving != self_animating())
            set_self_animating(moving);
    }

    void perform_layout(NVGcontext *ctx) override {
        Widget::perform_layout(ctx);
        place_buttons();
    }

    bool mouse_button_event(const Vector2i &p, int button, bool down, int modifiers) override {
        /* The buttons sit on this strip. Hand them the click before the
         * page-icon logic, which otherwise treats that area as empty. */
        for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
            Widget *child = *it;
            if (child->visible() && child->contains(p - m_pos) &&
                child->mouse_button_event(p - m_pos, button, down, modifiers))
                return true;
        }
        /* Right-click: turn to the page under it, then its menu. */
        if (button == GLFW_MOUSE_BUTTON_2) {
            if (!down || m_press >= 0 || p.x() - m_pos.x() >= view_width())
                return false;
            sync_slots();
            const int i = index_at(p);
            if (i >= 0)
                m_view->show_page((size_t) i);
            if (on_page_menu)
                on_page_menu(parent()->absolute_position() + p);
            return true;
        }
        if (button != GLFW_MOUSE_BUTTON_1)
            return false;
        if (!down) {
            if (m_thumb_grab >= 0) {
                m_thumb_grab = -1;
                return true;
            }
            const bool mine = m_press >= 0;
            const bool turn = mine && !m_dragging;
            m_press = -1;
            m_dragging = false;
            set_self_animating(true);   // the dropped page settles into its slot
            if (turn && on_turned)
                on_turned();
            return mine;
        }
        sync_slots();
        /* The scroll bar: grab the thumb, or page toward a click on the track. */
        if (overflow() > 0 && p.y() - m_pos.y() >= m_size.y() - kBarZone &&
            p.x() - m_pos.x() < view_width()) {
            float tx, tw;
            thumb(tx, tw);
            const float px = (float) (p.x() - m_pos.x());
            if (px >= tx && px <= tx + tw) {
                m_thumb_grab = px - tx;
            } else {
                m_scroll_target = std::clamp(m_scroll_target + (px < tx ? -1.f : 1.f) * view_width() * 0.8f,
                                             0.f, (float) overflow());
                m_follow = false;
                set_self_animating(true);
            }
            return true;
        }
        const int i = index_at(p);
        if (i < 0)
            return false;
        m_press = i;
        m_dragging = false;
        m_moved_once = false;
        m_press_x = p.x();
        m_grab = (float) (p.x() - m_pos.x()) + m_scroll - m_x[(size_t) i];
        m_pointer_x = p.x();
        m_view->show_page((size_t) i);
        return true;
    }

    bool mouse_drag_event(const Vector2i &p, const Vector2i &, int, int) override {
        if (m_thumb_grab >= 0) {
            float tx, tw;
            thumb(tx, tw);
            const float track = (float) view_width() - 2.f * kBarInset - tw;
            const float t = track > 0 ? ((float) (p.x() - m_pos.x()) - m_thumb_grab - kBarInset) / track : 0.f;
            m_scroll = m_scroll_target = std::clamp(t, 0.f, 1.f) * (float) overflow();
            m_follow = false;
            return true;
        }
        if (m_press < 0)
            return false;
        m_pointer_x = p.x();
        if (!m_dragging && std::abs(p.x() - m_press_x) < kDragSlop)
            return true;
        if (!m_dragging) {
            m_dragging = true;
            set_self_animating(true);
        }
        drag_to_pointer();
        return true;
    }

    bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        if (p.x() - m_pos.x() >= view_width())
            return Widget::scroll_event(p, rel);
        if (overflow() <= 0)
            return true;
        /* Wheels scroll vertically; trackpads may send either axis. */
        const float d = std::abs(rel.x()) > std::abs(rel.y()) ? rel.x() : rel.y();
        m_scroll_target = std::clamp(m_scroll_target - d * kStride * 2.f, 0.f, (float) overflow());
        m_follow = false;
        set_self_animating(true);
        return true;
    }

private:
    static constexpr int kW = 18, kH = 24, kGap = 6, kPad = 8, kStride = kW + kGap;
    static constexpr int kSpreadGap = 8;     // between facing spreads
    static constexpr int kStripH = 36, kIconTop = 4, kBarZone = 7;
    static constexpr int kBtn = 26, kBtnGap = 2, kAddGap = 8;
    static constexpr int kDragSlop = 3;
    static constexpr float kBarH = 3.f, kBarInset = 4.f;
    static constexpr float kSlideRate = 18.f;   // per second, exponential
    static constexpr float kEdge = 28.f;        // autoscroll zone while dragging
    static constexpr double kAppearSecs = 0.18;

    void draw_icon(NVGcontext *ctx, int i, float x, float y, float scale, bool on, bool lifted) {
        const bool hidden = m_view->page_hidden((size_t) i);
        nvgSave(ctx);
        if (scale < 1.f) {
            nvgTranslate(ctx, x + kW * 0.5f, y + kH);
            nvgScale(ctx, scale, scale);
            nvgTranslate(ctx, -(x + kW * 0.5f), -(y + kH));
        }
        if (lifted) {
            NVGpaint shadow = nvgBoxGradient(ctx, x + 1, y + 3, kW, kH, 2, 6, nvgRGBA(0, 0, 0, 90),
                                             nvgRGBA(0, 0, 0, 0));
            nvgBeginPath(ctx);
            nvgRect(ctx, x - 6, y - 4, kW + 12, kH + 14);
            nvgFillPaint(ctx, shadow);
            nvgFill(ctx);
        } else {
            nvgBeginPath(ctx);
            nvgRect(ctx, x + 1, y + 1, kW, kH);
            nvgFillColor(ctx, nvgRGBA(0, 0, 0, 40));
            nvgFill(ctx);
        }
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, kW, kH);
        nvgFillColor(ctx, on ? nvgRGB(0, 0, 0) : hidden ? nvgRGB(196, 196, 196) : nvgRGB(255, 255, 255));
        nvgFill(ctx);
        nvgStrokeColor(ctx, nvgRGB(0, 0, 0));
        nvgStrokeWidth(ctx, 1);
        nvgStroke(ctx);
        char num[16];
        std::snprintf(num, sizeof num, "%d", i + 1);
        nvgFillColor(ctx, on ? nvgRGB(255, 255, 255) : nvgRGB(0, 0, 0));
        nvgText(ctx, x + kW * 0.5f, y + kH * 0.5f, num, nullptr);
        nvgRestore(ctx);
    }

    void draw_scrollbar(NVGcontext *ctx) {
        if (overflow() <= 0)
            return;
        float tx, tw;
        thumb(tx, tw);
        const float by = (float) (m_pos.y() + m_size.y()) - kBarH - 2.f;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + kBarInset, by, view_width() - 2.f * kBarInset, kBarH, kBarH * 0.5f);
        nvgFillColor(ctx, nvgRGBA(0, 0, 0, 25));
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + tx, by, tw, kBarH, kBarH * 0.5f);
        nvgFillColor(ctx, nvgRGBA(0, 0, 0, m_thumb_grab >= 0 ? 150 : 100));
        nvgFill(ctx);
    }

    /* Thumb x (strip-local) and width. */
    void thumb(float &x, float &w) const {
        const float track = (float) view_width() - 2.f * kBarInset;
        const float content = (float) content_width();
        w = std::max(20.f, track * (float) view_width() / std::max(content, 1.f));
        const float t = overflow() > 0 ? m_scroll / (float) overflow() : 0.f;
        x = kBarInset + t * (track - w);
    }

    void add_page() {
        m_view->insert_page(true);
        sync_slots();   // the new page grows in; the ones after it slide over
        if (on_turned)
            on_turned();
    }

    /* Keep one display position per page, following each page by its uid,
     * so any reorder (a drag, a menu, undo) slides the icons to their new
     * slots and new pages grow in. A whole new document snaps into place.
     * The strip follows the current page when it changes. */
    void sync_slots() {
        const size_t n = m_view->page_count();
        bool same = m_uids.size() == n;
        for (size_t i = 0; same && i < n; ++i)
            same = m_uids[i] == m_view->page_uid(i);
        if (!same) {
            std::unordered_map<uint32_t, float> was;
            for (size_t i = 0; i < m_uids.size() && i < m_x.size(); ++i)
                was[m_uids[i]] = m_x[i];
            bool known = false;
            for (size_t i = 0; i < n && !known; ++i)
                known = was.count(m_view->page_uid(i)) != 0;
            const double now = glfwGetTime();
            std::vector<uint32_t> uids(n);
            std::vector<float> xs(n);
            for (size_t i = 0; i < n; ++i) {
                uids[i] = m_view->page_uid(i);
                auto it = was.find(uids[i]);
                if (it != was.end()) {
                    xs[i] = it->second;
                } else {
                    xs[i] = (float) slot_x((int) i);
                    if (known)
                        m_appearing.push_back({uids[i], now});
                }
            }
            m_uids = std::move(uids);
            m_x = std::move(xs);
            set_self_animating(true);
        }
        const int cur = (int) m_view->page_index();
        if (cur != m_last_cur || (int) n != m_last_count) {
            if (!m_dragging)
                m_follow = true;
            m_last_cur = cur;
            m_last_count = (int) n;
        }
    }

    /* Advance the slide, scroll and drag animations; true while any of
     * them still has somewhere to go. */
    bool step(float dt) {
        const int n = (int) m_x.size();
        const int cur = (int) m_view->page_index();
        const float k = 1.f - std::exp(-kSlideRate * dt);
        bool moving = false;

        /* Dragging near either end scrolls, and the page keeps up. */
        if (m_dragging) {
            const float px = (float) (m_pointer_x - m_pos.x());
            float v = 0.f;
            if (px < kEdge)
                v = -(kEdge - px) * 20.f;
            else if (px > view_width() - kEdge)
                v = (px - (view_width() - kEdge)) * 20.f;
            if (v != 0.f && overflow() > 0) {
                const float before = m_scroll;
                m_scroll = m_scroll_target = std::clamp(m_scroll + v * dt, 0.f, (float) overflow());
                if (m_scroll != before) {
                    drag_to_pointer();
                    moving = true;
                }
            }
        } else if (m_follow) {
            ensure_visible(cur);
        }

        for (int i = 0; i < n; ++i) {
            if (m_dragging && i == cur)
                continue;   // drag_to_pointer places it
            float &x = m_x[(size_t) i];
            const float target = (float) slot_x(i);
            if (std::abs(target - x) < 0.3f) {
                x = target;
            } else {
                x += (target - x) * k;
                moving = true;
            }
        }

        if (std::abs(m_scroll_target - m_scroll) < 0.3f) {
            m_scroll = m_scroll_target;
        } else {
            m_scroll += (m_scroll_target - m_scroll) * k;
            moving = true;
        }
        const float max_scroll = (float) std::max(overflow(), 0);
        m_scroll = std::clamp(m_scroll, 0.f, max_scroll);
        m_scroll_target = std::clamp(m_scroll_target, 0.f, max_scroll);

        const double now = glfwGetTime();
        m_appearing.erase(std::remove_if(m_appearing.begin(), m_appearing.end(),
                                         [&](const std::pair<uint32_t, double> &ap) {
                                             return now - ap.second >= kAppearSecs;
                                         }),
                          m_appearing.end());
        if (!m_appearing.empty())
            moving = true;
        return moving || m_dragging;
    }

    /* The lifted page follows the pointer; when its center crosses into
     * another slot, the document moves it there. */
    void drag_to_pointer() {
        const int n = (int) m_x.size();
        int cur = (int) m_view->page_index();
        if (n <= 0 || cur >= n)
            return;
        const float lo = (float) slot_x(0), hi = (float) slot_x(n - 1);
        const float x = std::clamp((float) (m_pointer_x - m_pos.x()) + m_scroll - m_grab, lo, hi);
        int slot = 0;
        for (int i = 1; i < n; ++i)
            if (std::abs(slot_x(i) - x) < std::abs(slot_x(slot) - x))
                slot = i;
        if (slot != cur) {
            m_view->move_page_to((size_t) slot, !m_moved_once);
            m_moved_once = true;
            sync_slots();   // the display positions travel with their pages
            cur = (int) m_view->page_index();
            m_press = cur;
        }
        if ((size_t) cur < m_x.size())
            m_x[(size_t) cur] = x;
    }

    void ensure_visible(int i) {
        const float view = (float) view_width();
        const float x = (float) slot_x(i);
        float target = m_scroll_target;
        if (x - kPad < target)
            target = x - kPad;
        else if (x + kW + kPad > target + view)
            target = x + kW + kPad - view;
        m_scroll_target = std::clamp(target, 0.f, (float) std::max(overflow(), 0));
        m_follow = false;
    }

    bool facing() const { return m_view->document().setup.facing; }

    /* Icon x, content coords. Facing pages group into spreads like the
     * pasteboard: page 1 alone, then 2-3, 4-5, ... touching at the spine. */
    int slot_x(int i) const {
        if (!facing())
            return kPad + i * kStride;
        return kPad + i * kW + (i + 1) / 2 * kSpreadGap;
    }

    int content_width() const {
        const int n = (int) m_view->page_count();
        return n > 0 ? slot_x(n - 1) + kW + kPad : kPad * 2;
    }

    int view_width() const { return std::max(m_size.x() - chrome(), 1); }

    int overflow() const { return content_width() - view_width(); }

    int chrome() const {
        const int n = (int) m_modes.size();
        return kPad + kBtn + kAddGap + n * kBtn + std::max(0, n - 1) * kBtnGap + kPad;
    }

    void place_buttons() {
        const int n = (int) m_modes.size();
        int x = m_size.x() - kPad - n * kBtn - std::max(0, n - 1) * kBtnGap;
        const int y = std::max(0, (m_size.y() - kBtn) / 2);
        m_add->set_position(Vector2i(x - kAddGap - kBtn, y));
        m_add->set_size(Vector2i(kBtn, kBtn));
        for (auto &mb : m_modes) {
            mb.second->set_position(Vector2i(x, y));
            mb.second->set_size(Vector2i(kBtn, kBtn));
            x += kBtn + kBtnGap;
        }
    }

    void sync_buttons() {
        const PageView v = m_view->page_view();
        for (auto &mb : m_modes)
            if (mb.second->pushed() != (mb.first == v))
                mb.second->set_pushed(mb.first == v);
    }

    /* The page icon under `p` (parent coordinates), or -1. */
    int index_at(const Vector2i &p) const {
        const int n = (int) m_view->page_count();
        if (n <= 0 || p.x() - m_pos.x() >= view_width())
            return -1;
        const float x = (float) (p.x() - m_pos.x()) + m_scroll;
        for (int i = 0; i < n; ++i)
            if (x >= slot_x(i) && x <= slot_x(i) + kW)
                return i;
        return -1;
    }

    ComposerView *m_view;
    ToolButton *m_add = nullptr;
    std::vector<std::pair<PageView, ToolButton *>> m_modes;
    std::vector<float> m_x;          // display x per page, content coords
    std::vector<uint32_t> m_uids;    // Page::uid per slot when m_x was synced
    float m_scroll = 0.f, m_scroll_target = 0.f;
    bool m_follow = true;            // scroll the current page into view
    int m_last_cur = -1, m_last_count = -1;
    double m_last_time = -1.0;
    int m_press = -1;                // page under the button, -1 when up
    int m_press_x = 0, m_pointer_x = 0;
    float m_grab = 0.f;              // pointer offset into the lifted page
    bool m_dragging = false;
    bool m_moved_once = false;       // one undo step per drag
    float m_thumb_grab = -1.f;       // pointer offset into the thumb
    std::vector<std::pair<uint32_t, double>> m_appearing;   // new pages growing in
};

class PagemadeApp : public Screen {
public:
    PagemadeApp() : Screen(Vector2i(1280, 940), "pagemade") {
        inc_ref();
        s_instance = this;
        /* The window's close button asks to save first, like File > Quit. */
        glfwSetWindowCloseCallback(glfw_window(), [](GLFWwindow *w) {
            if (!s_instance || s_instance->m_quitting)
                return;
            glfwSetWindowShouldClose(w, GLFW_FALSE);
            if (!ModalDialog::any_open())
                s_instance->quit();
        });
        set_theme_mode(ThemeMode::Light);
        register_default_fonts(m_fonts);
        register_system_fonts(m_fonts);
        font_menu_load_or_seed(m_font_menu, font_menu_config_path(), m_fonts.families());
        swatch_library_load_or_seed(m_swatch_lib, swatch_library_config_path());
        const std::string hyph = std::string(PAGEMADE_DATA_DIR) + "/hyphenation/hyph-en-us";
        if (!m_hyphenator.load(hyph + ".pat.txt", hyph + ".hyp.txt"))
            std::fprintf(stderr, "pagemade: no hyphenation patterns at %s.pat.txt\n", hyph.c_str());

        auto *root_flex = new FlexLayout(FlexDirection::Column, JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);
        m_root_window = window;
        build_menus(window);

        Widget *toolbar = new Widget(window);
        toolbar->set_min_height(40);
        toolbar->set_height(40);
        toolbar->set_height_flex(SizeMode::Fixed);
        toolbar->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 6));

        using Tool = ComposerView::Tool;
        auto tool_btn = [&](Tool t, int icon, const char *tip) {
            ToolButton *b = new ToolButton(toolbar, icon);
            b->set_tooltip(tip);
            b->set_callback([this, t] { apply_tool(t); });
            m_tool_btns.push_back({t, b});
        };
        tool_btn(Tool::Pointer, FA_MOUSE_POINTER,
                 "Pointer tool (Shift+click adds, drag a marquee; Delete removes; "
                 "arrows nudge; Ctrl+F front, Ctrl+] forward, Ctrl+[ backward, Ctrl+B back)");
        tool_btn(Tool::Crop, FA_CROP,
                 "Crop tool (drag a handle to trim the frame, drag inside to slide; "
                 "Shift locks one axis)");
        tool_btn(Tool::Rotate, FA_SYNC_ALT, "Rotate tool (Shift snaps to 15\u00B0)");
        tool_btn(Tool::Text, FA_I_CURSOR, "Text tool (drag on empty space for a new block)");
        tool_btn(Tool::Line, FA_SLASH, "Line tool (Shift: 45\u00B0 steps)");
        tool_btn(Tool::Rect, FA_SQUARE, "Rectangle tool (Shift: square)");
        tool_btn(Tool::Ellipse, FA_CIRCLE, "Ellipse tool (Shift: circle)");
        tool_btn(Tool::Polygon, FA_DRAW_POLYGON, "Polygon tool (Shift: regular)");
        m_tool_btns.front().second->set_pushed(true);

        m_snap_btn = new Button(toolbar, "", FA_MAGNET);
        m_snap_btn->set_flags(Button::ToggleButton);
        m_snap_btn->set_pushed(true);
        m_snap_btn->set_tooltip("Snap to guides");
        m_snap_btn->set_change_callback([this](bool on) { set_snap(on); });

        m_undo_btn = new Button(toolbar, "", FA_UNDO);
        m_redo_btn = new Button(toolbar, "", FA_REDO);
        m_undo_btn->set_tooltip("Undo (Ctrl+Z)");
        m_redo_btn->set_tooltip("Redo (Ctrl+Shift+Z)");
        m_undo_btn->set_callback([this] { m_view->undo(); });
        m_redo_btn->set_callback([this] { m_view->redo(); });

        auto *pdf_btn = new Button(toolbar, "", FA_FILE_PDF);
        pdf_btn->set_tooltip("Export as PDF");
        pdf_btn->set_callback([this] { export_pdf_dialog(); });
        auto *print_btn = new Button(toolbar, "", FA_PRINT);
        print_btn->set_tooltip("Print (Ctrl+P, via CUPS lp)");
        print_btn->set_callback([this] { print_document(); });

        /* The zoom levels PageMaker keeps in the View menu, as one dropdown.
         * 0 means Fit in Window. The caption follows the actual zoom. */
        m_zoom_menu = new Dropdown(toolbar, {"Fit", "50%", "100%", "200%", "400%", "800%"},
                                   {}, Dropdown::ComboBox, "Fit");
        m_zoom_menu->set_tooltip("Zoom (Ctrl+0 fit, Ctrl+5/7/1/2/4/8)");
        m_zoom_menu->set_selected_callback([this](int i) {
            static const double zv[] = {0, 0.5, 1, 2, 4, 8};
            zv[i] > 0 ? zoom_to(zv[i]) : fit_page();
        });

        /* ¶ pops open the Paragraph panel (also Element > Paragraph…). */
        m_para_btn = new PopupButton(toolbar, "\u00B6");
        m_para_btn->set_side(Popup::Bottom);
        m_para_btn->set_chevron_icon(0);        // after set_side, which sets one
        m_para_btn->set_font_size(20);
        m_para_btn->set_fixed_size(Vector2i(32, 28));
        m_para_btn->set_tooltip("Paragraph: style, alignment, spacing and indents (Ctrl+M)");
        Popup *para_popup = m_para_btn->popup();
        para_popup->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));
        ParagraphPanel::Hooks hooks;
        hooks.apply = [this](const std::function<void(ParaStyle &)> &fn, const void *spin) {
            m_view->set_undo_merge(spin);
            m_view->apply_para(fn);
            m_view->set_undo_merge(nullptr);
        };
        /* The panel aligns whole paragraphs. */
        hooks.align = [this](Align a) {
            m_view->apply_para([a](ParaStyle &ps) { ps.align = a; });
        };
        hooks.apply_style = [this](const std::string &name) { m_view->apply_style(name); };
        hooks.delete_style = [this](const std::string &name) { m_view->delete_style(name); };
        hooks.suggest_style_name = [this] {
            const auto paras = m_view->selected_paragraphs();
            std::string name = paras.empty() ? std::string() : paras.front()->style.name;
            return name.empty() ? std::string("New style") : name;
        };
        hooks.define_style = [this](const std::string &n) { m_view->define_style(n); };
        m_para_panel = new ParagraphPanel(para_popup, std::move(hooks));
        /* A resizable window keeps its size as its preferred size, so the
         * popup would stay at PopupButton's 320 x 250 and squeeze the rows
         * until they clip. Fixed, it fits the panel. */
        para_popup->set_resizable(false);

        /* T opens the Type Specifications dialog (also Element > Type Specs…). */
        m_specs = new ItalicButton(toolbar, "T");
        m_specs->set_font_size(18);
        m_specs->set_fixed_size(Vector2i(32, 28));
        m_specs->set_tooltip("Type specifications (Ctrl+T)");
        m_specs->set_callback([this] { type_specs(); });

        /* The Character panel: character styles, text color and tint. */
        m_char_btn = new PopupButton(toolbar, "A");
        m_char_btn->set_side(Popup::Bottom);
        m_char_btn->set_chevron_icon(0);
        m_char_btn->set_font_size(18);
        m_char_btn->set_fixed_size(Vector2i(32, 28));
        m_char_btn->set_tooltip("Character: character styles, text color and tint");
        Popup *char_popup = m_char_btn->popup();
        char_popup->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));
        CharacterPanel::Hooks ch;
        ch.apply_style = [this](const std::string &name) { m_view->apply_char_style(name); };
        ch.delete_style = [this](const std::string &name) { m_view->delete_char_style(name); };
        ch.swatch = [this](uint32_t id) {
            m_view->apply_type([id](CharStyle &cs) { cs.swatch = id; });
        };
        ch.tint = [this](float t, const void *spin) {
            m_view->set_undo_merge(spin);
            m_view->apply_type([t](CharStyle &cs) { cs.tint = t; });
            m_view->set_undo_merge(nullptr);
        };
        ch.apply_spacing = [this](const std::function<void(ParaStyle &)> &fn, const void *spin) {
            m_view->set_undo_merge(spin);
            m_view->apply_para(fn);
            m_view->set_undo_merge(nullptr);
        };
        /* Strict loose/tight turns the highlighting (V) on as well */
        ch.strict = [this](bool on) {
            m_view->set_strict_loose_tight(on);
            if (on && !m_view->show_loose_tight()) {
                m_view->set_show_loose_tight(true);
                if (m_loose)
                    m_loose->set_pushed(true);
            }
            redraw();
        };
        ch.suggest_style_name = [this] {
            ComposerView::TypeStyle t;
            m_view->type_style(t);
            const std::string name = t.mix_char_style || t.char_style.empty() ? "New style"
                                                                               : t.char_style;
            return name;
        };
        ch.define_style = [this](const std::string &n) { m_view->define_char_style(n); };
        m_char_panel = new CharacterPanel(char_popup, std::move(ch));
        char_popup->set_resizable(false);

        /* Kerning, ligatures, baselines, loose/tight: one toggle each. */
        Widget *flags = new Widget(toolbar);
        flags->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 2));
        auto toggle = [&](const char *letter, const char *tip, bool on,
                          std::function<void(bool)> fn) {
            Button *b = new Button(flags, letter);
            b->set_flags(Button::ToggleButton);
            b->set_pushed(on);
            b->set_fixed_size(Vector2i(28, 28));
            b->set_tooltip(tip);
            b->set_change_callback(std::move(fn));
            return b;
        };
        m_kern = toggle("K", "Kerning", true, [this](bool on) {
            for_each_run([on](CharStyle &cs) { cs.kerning = on; });
        });
        toggle("L", "Ligatures", true, [this](bool on) {
            for_each_run([on](CharStyle &cs) { cs.ligatures = on; });
        });
        m_baselines = toggle("B", "Baselines", false, [this](bool on) {
            m_view->set_show_baselines(on);
        });
        m_loose = toggle("V", "Loose/tight lines", false, [this](bool on) {
            m_view->set_show_loose_tight(on);
        });

        PageDoc doc = sample_document();
        seed_swatches(doc);
        build_palette(window, doc);

        /* The pages, with the rulers along the top and left (View > Rulers) */
        m_rulers = new RulerFrame(window);
        m_rulers->set_height_flex(SizeMode::Expanding);
        m_rulers->on_layout = [this] { place_docked(); };   // follows window resizes
        root_flex->set_flex_item(m_rulers, FlexLayout::FlexItem(1.0f));
        m_scroll = new ZoomScrollPanel(m_rulers, ZoomScrollPanel::ScrollTypes::Both);
        m_scroll->set_zoom_range(0.1, 16.0);
        m_scroll->set_zoom_enabled(true);
        m_scroll->set_child_fills_view(true);   // the pasteboard is clickable wherever it shows

        m_view = new ComposerView(m_scroll, &m_fonts, &m_hyphenator);
        m_rulers->set_content(m_scroll, m_view);
        m_view->set_image_store(&m_images);
        m_view->set_vector_store(&m_vectors);
        m_view->on_recompose = [this] { update_status(); };
        m_view->on_tool_change = [this](ComposerView::Tool t) {
            sync_tool_buttons(t);
            show_type_palette(t == ComposerView::Tool::Text);
        };
        m_view->on_selection_change = [this] {
            sync_palette();
            update_status();
            if (m_layers && m_layers->visible())
                m_layers->refresh();     // Move Selection Here follows the selection
        };
        m_view->on_note = [this](const std::string &note) { set_note(note); };
        m_view->on_setup_change = [this] {
            perform_layout();            // the pasteboard is a new size
            fit_page();
            update_status();
        };
        m_view->on_context_menu = [this](const Vector2i &at) { show_context_menu(at); };
        m_view->on_page_change = [this] { update_status(); };
        m_view->on_document_change = [this] {
            if (m_layers && m_layers->visible())
                m_layers->refresh();
            update_title();
            if (m_assets && m_assets->visible())   // a hidden one catches up when shown
                m_assets->refresh();
        };
        m_view->on_live_change = [this] {
            if (m_assets && m_assets->visible())
                redraw();                // the asset badges show what is live
        };
        /* Files dropped on a page are placed at once as stand-ins and read
         * off the UI thread. A run of files steps down the page so they
         * don't stack exactly; the size hint is the first file's. */
        m_view->on_drop_files = [this](const std::vector<std::string> &paths, size_t page, Point at,
                                       Vector2i px_hint) {
            for (size_t i = 0; i < paths.size(); ++i) {
                Point where = at;
                where.x += 12.f * (float) i;
                where.y += 12.f * (float) i;
                const std::string path = paths[i];
                const std::string name = nanogui::fs::filename(path);
                m_view->place_image_file(path, page, &where, [this, name](const std::string &error) {
                    if (!error.empty())
                        show_alert(this, "The picture couldn't be placed.", error);
                    else
                        set_note("placed " + name);
                }, i == 0 ? px_hint : Vector2i(0, 0));
            }
        };

        m_pages = new PageIconStrip(window, m_view);
        m_pages->on_view = [this] { fit_page(); };
        m_pages->on_turned = [this] {
            if (m_view->page_view() != PageView::One)
                frame_view();
        };
        m_pages->on_page_menu = [this](const Vector2i &at) { show_page_menu(at); };

        m_status = new Label(window, "", "sans", 16);
        m_status->set_min_height(24);
        m_status->set_height(24);
        m_status->set_height_flex(SizeMode::Fixed);

        m_view->set_document(std::move(doc));
        sync_palette();
        update_title();
        perform_layout();
    }

    ~PagemadeApp() override {
        /* The asset window's thumbnail loader reads m_images. Dispose it
         * before the members die: Screen destroys its children afterwards. */
        std::vector<ImageAssetsPanel *> panels;
        for (Widget *child : children())
            if (auto *panel = dynamic_cast<ImageAssetsPanel *>(child))
                panels.push_back(panel);
        for (ImageAssetsPanel *panel : panels) {
            panel->set_close_callback({});
            panel->dispose();
        }
        /* dispose() only queues the window; destroy it now, so its grid's
           loader thread is joined while m_images still exists */
        do_widget_cleanup();
        m_assets = nullptr;
        if (s_instance == this)
            s_instance = nullptr;
    }

    /* ---- Publications: New, Open, Save ------------------------------ */

    std::string doc_name() const {
        return m_doc_path.empty() ? std::string("Untitled")
                                  : nanogui::fs::stem(m_doc_path);
    }

    void update_title() {
        set_caption((m_view->modified() ? "*" : "") + doc_name() + " \u2014 pagemade");
    }

    /* Run `then` once the open publication may go: it's unchanged, or saved,
     * or the user chose Don't Save. Cancel runs nothing. */
    void close_document_then(std::function<void()> then) {
        if (!m_view->modified()) {
            then();
            return;
        }
        ask_save_changes(this, doc_name(), [this, then](SaveChoice c) {
            if (c == SaveChoice::DontSave)
                then();
            else if (c == SaveChoice::Save)
                save([then](bool saved) { if (saved) then(); });
        });
    }

    /* Save where it was last saved, or ask where the first time. */
    void save(std::function<void(bool)> done = {}) {
        if (m_doc_path.empty()) {
            save_as(std::move(done));
            return;
        }
        const bool ok = write_to(m_doc_path, m_embed_assets);
        if (done)
            done(ok);
    }

    void save_as(std::function<void(bool)> done = {}) {
        ask_save_as(this, m_doc_path, m_embed_assets,
                    [this, done](const std::string &path, bool embed) {
                        const bool ok = write_to(path, embed);
                        if (done)
                            done(ok);
                    });
    }

    bool write_to(const std::string &path, bool embed) {
        std::string error;
        if (!save_document(m_view->document(), m_fonts, path, SaveOptions{embed}, &error,
                           &m_images, &m_vectors)) {
            show_alert(this, "The publication couldn't be saved.", error);
            return false;
        }
        m_doc_path = path;
        m_embed_assets = embed;
        m_view->mark_clean();
        set_note("saved " + path + (embed ? " (assets embedded)" : ""));
        return true;
    }

    /* New publications start with the global Colors palette. Ids that
     * clash and names the document already has are left alone, so every
     * reference keeps resolving to the same color. */
    void seed_swatches(PageDoc &doc) { swatch_library_seed(doc, m_swatch_lib); }

    void new_document() {
        close_document_then([this] {
            ask_new_document(this, [this](const NewDocument &spec) {
                m_view->invalidate_compositions();   // before the fonts go
                m_fonts.clear_document_fonts();
                m_images.clear();
                m_vectors.clear();
                PageDoc doc = new_publication(spec.setup, spec.pages);
                seed_swatches(doc);
                m_view->set_document(std::move(doc));
                m_doc_path.clear();
                m_embed_assets = false;
                after_document_switch();
                set_note("new publication");
            });
        });
    }

    void open_document_dialog() {
        close_document_then([this] {
            const std::string folder = m_doc_path.empty()
                ? std::string() : nanogui::fs::parent_path(m_doc_path);
            auto paths = file_dialog({{kPublicationExtension, "pagemade publication"}}, false,
                                     false, folder);
            if (!paths.empty() && !paths[0].empty())
                open_path(paths[0]);
        });
    }

    void place_dialog() {
        const std::string folder = m_doc_path.empty()
            ? std::string() : nanogui::fs::parent_path(m_doc_path);
        auto types = image_file_types();
        for (const auto &t : vector_file_types())
            types.push_back(t);
        auto paths = file_dialog(types, false, false, folder);
        if (paths.empty() || paths[0].empty())
            return;
        const std::string path = paths[0];
        const std::string name = nanogui::fs::filename(path);
        m_view->place_file(path, m_view->page_index(), nullptr,
                           [this, name](const std::string &error, const std::string &note) {
            if (!error.empty())
                show_alert(this, "The file couldn't be placed.", error);
            else
                set_note(note.empty() ? "placed " + name : note);
        });
    }

    /* Open without asking about the current publication (callers do that). */
    bool open_path(const std::string &path) {
        /* Linked pictures are read as they come on screen, not here */
        OpenResult r = open_document(path, m_fonts, false);
        if (!r.ok) {
            show_alert(this, "\u201C" + nanogui::fs::filename(path) +
                             "\u201D couldn't be opened.", r.error);
            return false;
        }
        /* The old publication's embedded fonts go, this one's arrive; the
         * view recomposes with them straight away. */
        m_view->invalidate_compositions();   // their GlyphRuns name the old fonts
        m_fonts.clear_document_fonts();
        for (const DocumentFont &f : r.fonts)
            m_fonts.add_document_font(f.family, f.style, f.font);
        m_images = std::move(r.images);
        m_vectors = std::move(r.vectors);
        m_view->set_document(std::move(r.doc));
        m_doc_path = path;
        m_embed_assets = r.embed_assets;
        after_document_switch();
        std::string note = "opened " + path;
        if (!r.missing_fonts.empty()) {
            note += "   |   missing fonts (substituted):";
            for (const std::string &f : r.missing_fonts)
                note += " " + f + ";";
            note.pop_back();
        }
        for (const std::string &w : r.warnings)
            note += "   |   " + w;
        set_note(note);
        if (!r.damage.empty())
            show_alert(this, "“" + nanogui::fs::filename(path) +
                             "” was damaged and only partly opened.", r.damage);
        return true;
    }

    void after_document_switch() {
        rebuild_swatch_menus();
        sync_palette();
        update_title();
        fit_page();
    }

    void quit() {
        close_document_then([this] {
            m_quitting = true;
            glfwSetWindowShouldClose(glfw_window(), GLFW_TRUE);
            glfwPostEmptyEvent();
        });
    }

    /* The swatch menus follow the open publication's Colors palette. */
    void rebuild_swatch_menus() {
        std::vector<std::string> names{"None"};
        std::vector<SwatchId> ids{kNoPaint};
        for (const Swatch &sw : m_view->document().swatches) {
            names.push_back(sw.name);
            ids.push_back(sw.id);
        }
        if (ids == m_swatch_ids || !m_fill)
            return;
        m_swatch_ids = ids;
        for (Dropdown *d : {m_fill, m_stroke}) {
            while (d->popup()->child_count() > 0)
                d->remove_item(d->popup()->child_count() - 1);
            for (const std::string &n : names)
                d->add_item({n, ""}, 0, {}, {}, true);
            d->set_selected_index(0);
        }
        if (m_colors && !m_colors->popup()->visible())
            refill_colors();
    }

    /* The Colors dropdown lists the publication's swatches. Picking one
     * paints the selection: fill for shapes, ink for text, border for
     * pictures; with nothing selected it sets the defaults for new
     * shapes and text. */
    void refill_colors() {
        if (!m_colors || !m_view)
            return;
        SwatchMenuView view;
        view.swatches = m_view->document().swatches;
        view.current = current_paint();
        refill_swatch_menu(
            m_colors, view, [this](SwatchId id) { pick_color(id); },
            [this](SwatchId id) { edit_color(id); }, [this] { add_color(); });
    }

    /* The paint picking a swatch would change, for the check mark. */
    SwatchId current_paint() {
        if (m_view->shapes_selected())
            return m_view->shape_style().fill.swatch;
        ComposerView::TypeStyle t;
        if (m_view->editing() || !m_view->selected_paragraphs().empty()) {
            if (m_view->type_style(t))
                return t.swatch;
        }
        return kNoPaint;
    }

    void pick_color(SwatchId id) {
        if (!m_view || !m_view->document().find_swatch(id))
            return;
        /* Text at the caret first; then shapes take fill and pictures a
         * border (their fill tint is opacity); text frames take ink; with
         * nothing selected both defaults follow, as PageMaker does. */
        if (m_view->editing() || (!m_view->shapes_selected() &&
                                  !m_view->pictures_selected() &&
                                  !m_view->selected_paragraphs().empty())) {
            m_view->apply_type([id](CharStyle &cs) { cs.swatch = id; });
            return;
        }
        if (m_view->pictures_selected() && !m_view->shapes_selected()) {
            m_view->apply_item_style(
                [](Shape &) {}, [id](PlacedImage &im) { im.stroke.paint.swatch = id; });
            return;
        }
        if (m_view->shapes_selected()) {
            m_view->apply_shape_style([id](Shape &sh) { sh.fill.swatch = id; });
            return;
        }
        m_view->apply_item_style([id](Shape &sh) { sh.fill.swatch = id; }, nullptr);
        m_view->apply_type([id](CharStyle &cs) { cs.swatch = id; });
    }

    void edit_color(SwatchId id) {
        const Swatch *sw = m_view->document().find_swatch(id);
        if (!sw)
            return;
        /* Close the menu first; the editor is modal. */
        if (m_colors)
            m_colors->set_pushed(false);
        const bool locked = sw->id == kPaper || sw->id == kBlack || sw->id == kRegistration;
        SwatchEdit initial;
        initial.name = sw->name;
        initial.cmyk = sw->cmyk;
        initial.in_library = swatch_library_find(m_swatch_lib, sw->name) != nullptr;
        async([this, initial, locked, id] {
            ask_swatch(
                this, initial, locked,
                [this, id](const std::string &name, const Cmyk &cmyk) {
                    if (m_view->update_doc_swatch(id, name, cmyk_to_rgb(cmyk), cmyk))
                        after_swatch_change();
                },
                locked ? std::function<void()>()
                       : std::function<void()>([this, id] {
                             if (m_view->remove_doc_swatch(id))
                                 after_swatch_change();
                         }),
                [this, old_name = initial.name](const std::string &name, const Cmyk &cmyk,
                                                bool global) {
                    set_library_swatch(old_name, name, cmyk, global);
                });
        });
    }

    void add_color() {
        if (m_colors)
            m_colors->set_pushed(false);
        SwatchEdit initial;
        initial.name = "New color";
        async([this, initial] {
            ask_swatch(
                this, initial, false,
                [this](const std::string &name, const Cmyk &cmyk) {
                    m_view->add_doc_swatch(name, cmyk_to_rgb(cmyk), cmyk);
                    after_swatch_change();
                },
                {},
                [this](const std::string &name, const Cmyk &cmyk, bool global) {
                    set_library_swatch(name, name, cmyk, global);
                });
        });
    }

    /* The swatch editor's globe on Save: on adds or recolors `name` in
     * colors.conf; off removes the entry the dialog opened with. */
    void set_library_swatch(const std::string &old_name, const std::string &name,
                            const Cmyk &cmyk, bool global) {
        const std::string path = swatch_library_config_path();
        bool changed;
        if (global) {
            changed = swatch_library_upsert(m_swatch_lib, path, m_view->document().swatches,
                                            name, cmyk);
        } else {
            const Swatch *sw = swatch_library_find(m_swatch_lib, old_name);
            changed = sw && swatch_library_remove(m_swatch_lib, path, sw->id);
        }
        if (changed)
            refill_colors();
    }

    /* A swatch changed: every menu follows the table, then the palette
     * re-reads the selection. */
    void after_swatch_change() {
        m_swatch_ids.clear();   // force rebuild_swatch_menus past its guard
        rebuild_swatch_menus();
        refill_colors();
        sync_palette();
    }

    /* ---- Menus (PageMaker: File, Edit, Layout, Element) ------------- */

    MenuItem *add_cmd(Dropdown *menu, const char *caption, const std::vector<Shortcut> &keys,
                      const std::function<void()> &fn) {
        MenuItem *item = menu->add_item(caption, 0, keys);
        item->set_callback(fn);
        return item;
    }

    void build_menus(Widget *window) {
        m_menubar = new MenuBar(window, "");
        m_menubar->set_height_flex(SizeMode::Fixed);

        Dropdown *file = m_menubar->add_menu("File");
        add_cmd(file, "New Document", {{SYSTEM_COMMAND_MOD, GLFW_KEY_N}}, [this] { new_document(); });
        add_cmd(file, "Open...", {{SYSTEM_COMMAND_MOD, GLFW_KEY_O}}, [this] { open_document_dialog(); });
        add_cmd(file, "Place...", {{SYSTEM_COMMAND_MOD, GLFW_KEY_D}}, [this] { place_dialog(); });
        new Separator(file->popup());
        add_cmd(file, "Save", {{SYSTEM_COMMAND_MOD, GLFW_KEY_S}}, [this] { save(); });
        add_cmd(file, "Save As...", {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_S}},
                [this] { save_as(); });
        new Separator(file->popup());
        add_cmd(file, "Document Setup...", {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_P}},
                [this] { document_setup(); });
        new Separator(file->popup());
        add_cmd(file, "PDF Settings...", {}, [this] { pdf_settings_dialog(); });
        add_cmd(file, "Export PDF...", {{SYSTEM_COMMAND_MOD, GLFW_KEY_E}}, [this] { export_pdf_dialog(); });
        add_cmd(file, "Print...", {{SYSTEM_COMMAND_MOD, GLFW_KEY_P}}, [this] { print_document(); });
        new Separator(file->popup());
        add_cmd(file, "Quit", {{SYSTEM_COMMAND_MOD, GLFW_KEY_Q}}, [this] { quit(); });

        Dropdown *edit = m_menubar->add_menu("Edit");
        add_cmd(edit, "Undo", {{SYSTEM_COMMAND_MOD, GLFW_KEY_Z}}, [this] { m_view->undo(); });
        add_cmd(edit, "Redo", {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_Z},
                               {SYSTEM_COMMAND_MOD, GLFW_KEY_Y}},
                [this] { m_view->redo(); });
        new Separator(edit->popup());
        /* Text while editing it, items otherwise. The page view takes the
         * keys first; these items name them and serve a menu click. */
        add_cmd(edit, "Cut", {{SYSTEM_COMMAND_MOD, GLFW_KEY_X}}, [this] { m_view->edit_cut(); });
        add_cmd(edit, "Copy", {{SYSTEM_COMMAND_MOD, GLFW_KEY_C}}, [this] { m_view->edit_copy(); });
        add_cmd(edit, "Paste", {{SYSTEM_COMMAND_MOD, GLFW_KEY_V}}, [this] { m_view->edit_paste(); });

        /* Type: typographer's quotes, and the characters a keyboard lacks.
         * Shortcuts follow InDesign's where it has them. Line and frame
         * breaks are typed in the text (Shift+Return, Ctrl+Return); their
         * items only name the key, so the key does not insert twice. */
        /* View: rulers and ruler guides (PageMaker's Ctrl+R, Ctrl+J, Ctrl+U) */
        Dropdown *view = m_menubar->add_menu("View");
        m_rulers_item = add_cmd(view, "Rulers", {{SYSTEM_COMMAND_MOD, GLFW_KEY_R}}, [this] {
            m_rulers->set_rulers_shown(!m_rulers->rulers_shown());
            m_rulers_item->set_icon(m_rulers->rulers_shown() ? FA_CHECK : 0);
        });
        m_rulers_item->set_icon(FA_CHECK);
        m_guides_item = add_cmd(view, "Guides", {{SYSTEM_COMMAND_MOD, GLFW_KEY_J}}, [this] {
            m_view->set_show_guides(!m_view->show_guides());
            m_guides_item->set_icon(m_view->show_guides() ? FA_CHECK : 0);
        });
        m_guides_item->set_icon(FA_CHECK);
        m_snap_item = add_cmd(view, "Snap to Guides", {{SYSTEM_COMMAND_MOD, GLFW_KEY_U}},
                              [this] { set_snap(!m_view->snap()); });
        m_snap_item->set_icon(FA_CHECK);
        m_lock_guides_item = add_cmd(view, "Lock Guides", {}, [this] {
            m_view->set_lock_guides(!m_view->lock_guides());
            m_lock_guides_item->set_icon(m_view->lock_guides() ? FA_CHECK : 0);
        });
        new Separator(view->popup());
        add_cmd(view, "Clear Page Guides", {}, [this] { m_view->clear_guides(false); });
        add_cmd(view, "Clear All-Pages Guides", {}, [this] { m_view->clear_guides(true); });

        Dropdown *type = m_menubar->add_menu("Type");
        m_smart_quotes = add_cmd(type, "Smart Quotes", {}, [this] {
            m_view->set_smart_quotes(!m_view->smart_quotes());
            m_smart_quotes->set_icon(m_view->smart_quotes() ? FA_CHECK : 0);
        });
        m_smart_quotes->set_icon(FA_CHECK);
        new Separator(type->popup());
        const int C = SYSTEM_COMMAND_MOD, S = GLFW_MOD_SHIFT, A = GLFW_MOD_ALT;
        struct Special { const char *caption, *text; std::vector<Shortcut> keys; };
        const Special specials[] = {
            {"Em Dash \u2014", "\u2014", {{C | S, GLFW_KEY_MINUS}}},
            {"En Dash \u2013", "\u2013", {{C | A, GLFW_KEY_MINUS}}},
            {"Em Space", "\u2003", {{C | S, GLFW_KEY_M}}},
            {"En Space", "\u2002", {{C | S, GLFW_KEY_N}}},
            {"Thin Space", "\u2009", {{C | A | S, GLFW_KEY_M}}},
            {"Hair Space", "\u200A", {}},
            {"Nonbreaking Space", "\u00A0", {{C | A, GLFW_KEY_X}}},
            {"Nonbreaking Hyphen", "\u2011", {}},
            {"Discretionary Hyphen", "\u00AD", {}},
            {"Bullet \u2022", "\u2022", {}},
            {"Ellipsis \u2026", "\u2026", {}},
            {"Copyright \u00A9", "\u00A9", {}},
            {"Registered \u00AE", "\u00AE", {}},
            {"Trademark \u2122", "\u2122", {}},
            {"Section \u00A7", "\u00A7", {}},
            {"Paragraph \u00B6", "\u00B6", {}},
            {"Degree \u00B0", "\u00B0", {}},
            /* Markers that show the page's number, or how many pages there
             * are, wherever the text lands */
            {"Page Number", "\uE000", {{C | A, GLFW_KEY_P}}},
            {"Page Count", "\uE001", {}},
        };
        for (const Special &sp : specials) {
            const std::string text = sp.text;
            add_cmd(type, sp.caption, sp.keys, [this, text] { m_view->insert_at_caret(text); });
        }
        new Separator(type->popup());
        add_cmd(type, "Line Break (Shift+Return)", {},
                [this] { m_view->insert_at_caret("\u2028"); });
        add_cmd(type, "Tab (Tab)", {}, [this] { m_view->insert_at_caret("\t"); });
        add_cmd(type, "Right Indent Tab (Shift+Tab)", {},
                [this] { m_view->insert_at_caret("\uE002"); });
        add_cmd(type, "Frame Break (Ctrl+Return)", {},
                [this] { m_view->insert_at_caret("\x0C"); });

        Dropdown *layout = m_menubar->add_menu("Layout");
        add_cmd(layout, "Insert Page", {}, [this] { m_view->insert_page(true); });
        add_cmd(layout, "Insert Page Before", {}, [this] { m_view->insert_page(false); });
        add_cmd(layout, "Duplicate Page", {}, [this] {
            m_view->duplicate_page(false);
            after_page_command();
        });
        m_dup_spread = add_cmd(layout, "Duplicate Spread", {}, [this] {
            m_view->duplicate_page(true);
            after_page_command();
        });
        m_remove_page = add_cmd(layout, "Remove Page", {}, [this] { m_view->remove_page(); });
        new Separator(layout->popup());
        m_move_earlier = add_cmd(layout, "Move Page Earlier", {}, [this] { m_view->move_page_by(-1); });
        m_move_later = add_cmd(layout, "Move Page Later", {}, [this] { m_view->move_page_by(1); });
        new Separator(layout->popup());
        m_hide_page = add_cmd(layout, "Hide Page", {}, [this] {
            m_view->set_page_hidden(!m_view->page_hidden(m_view->page_index()));
        });

        Dropdown *element = m_menubar->add_menu("Element");
        m_to_front = add_cmd(element, "Bring to Front",
                             {{SYSTEM_COMMAND_MOD, GLFW_KEY_F},
                              {SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_RIGHT_BRACKET}},
                             [this] { m_view->arrange(ComposerView::Stack::Front); });
        m_forward = add_cmd(element, "Bring Forward",
                            {{SYSTEM_COMMAND_MOD, GLFW_KEY_RIGHT_BRACKET}},
                            [this] { m_view->arrange(ComposerView::Stack::Forward); });
        m_backward = add_cmd(element, "Send Backward",
                             {{SYSTEM_COMMAND_MOD, GLFW_KEY_LEFT_BRACKET}},
                             [this] { m_view->arrange(ComposerView::Stack::Backward); });
        m_to_back = add_cmd(element, "Send to Back",
                            {{SYSTEM_COMMAND_MOD, GLFW_KEY_B},
                             {SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_LEFT_BRACKET}},
                            [this] { m_view->arrange(ComposerView::Stack::Back); });
        new Separator(element->popup());
        m_group = add_cmd(element, "Group", {{SYSTEM_COMMAND_MOD, GLFW_KEY_G}},
                          [this] { m_view->group_selection(); });
        m_ungroup = add_cmd(element, "Ungroup", {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_G}},
                            [this] { m_view->ungroup_selection(); });
        new Separator(element->popup());
        add_cmd(element, "Type Specs...", {{SYSTEM_COMMAND_MOD, GLFW_KEY_T}},
                [this] { type_specs(); });
        add_cmd(element, "Paragraph...", {{SYSTEM_COMMAND_MOD, GLFW_KEY_M}},
                [this] { toggle_paragraph_panel(); });
        add_cmd(element, "Character...", {{SYSTEM_COMMAND_MOD | GLFW_MOD_SHIFT, GLFW_KEY_T}},
                [this] {
                    m_char_btn->set_pushed(!m_char_btn->pushed());
                    perform_layout();
                });
        new Separator(element->popup());
        add_cmd(element, "Image Assets...", {}, [this] { show_image_assets(); });
        add_cmd(element, "Layers...", {}, [this] { show_layers(); });
        m_caption_item = add_cmd(element, "Caption Placed Pictures", {}, [this] {
            m_view->set_caption_pictures(!m_view->caption_pictures());
            m_caption_item->set_icon(m_view->caption_pictures() ? FA_CHECK : 0);
        });
        m_columns_item = add_cmd(element, "Fit Placed Pictures to Columns", {}, [this] {
            m_view->set_fit_to_columns(!m_view->fit_to_columns());
            m_columns_item->set_icon(m_view->fit_to_columns() ? FA_CHECK : 0);
        });
        add_cmd(element, "Remove Unused", {}, [this] { m_view->remove_unused_images(); });
    }

    /* ---- Control palette ------------------------------------------- */

    static constexpr float kTints[] = {100, 90, 80, 70, 60, 50, 40, 30, 20, 10};
    static constexpr float kWeights[] = {0.25f, 0.5f, 1, 2, 4, 6, 8, 12};

    void show_type_palette(bool type) {
        m_type_mode = type;
        if (m_mode_btn) {
            m_mode_btn->set_icon(type ? FA_FONT : FA_SQUARE);
            m_mode_btn->set_tooltip(type
                ? "Type view: font, size, weight, leading, baseline. Click for objects."
                : "Object view: position, size, fill, stroke. Click for type.");
        }
        if (m_object_box)
            m_object_box->set_visible(!type);
        if (m_type_box)
            m_type_box->set_visible(type);
        perform_layout();
    }

    void build_palette(Widget *window, const PageDoc &doc) {
        Widget *outer = new Widget(window);
        outer->set_min_height(34);
        outer->set_height(34);
        outer->set_height_flex(SizeMode::Fixed);
        outer->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 5));

        /* PageMaker's control palette switches view. The icon is the view
         * showing now. */
        m_mode_btn = new Button(outer, "", FA_SQUARE);
        m_mode_btn->set_fixed_size(Vector2i(28, 26));
        m_mode_btn->set_tooltip("Object view: position, size, fill, stroke. Click for type.");
        m_mode_btn->set_callback([this] { show_type_palette(!m_type_mode); });

        m_object_box = new Widget(outer);
        m_object_box->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));
        m_type_box = new Widget(outer);
        m_type_box->set_layout(new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 5));
        m_type_box->set_visible(false);
        Widget *bar = m_object_box;

        /* Number fields spin: the arrows, the scroll wheel or a right-drag
         * step them (spinbox.h), and a run of steps is one undo step. */
        auto number = [&](const char *label, const char *units, int field, float width,
                          float step, float lo, float hi) {
            new Label(bar, label, "sans-bold");
            auto *box = new FloatSpin(bar, 0.f, step, lo, hi);
            box->number_format("%.2f");
            box->set_units(units);
            box->set_fixed_size(Vector2i((int) width, 26));
            box->set_callback([this, box, field](float v) {
                palette_change(box, [&] { apply_geometry(field, v); });
            });
            m_geom[field] = box;
        };
        number("X", "pt", 0, 80, 1.f, -1e5f, 1e5f);
        number("Y", "pt", 1, 80, 1.f, -1e5f, 1e5f);
        number("W", "pt", 2, 80, 1.f, 0.f, 1e5f);
        number("H", "pt", 3, 80, 1.f, 0.f, 1e5f);
        number("Rot", "\u00B0", 4, 70, 1.f, -360.f, 360.f);

        /* The Colors palette's swatches, "None" first, in both paint menus. */
        std::vector<std::string> swatches{"None"};
        m_swatch_ids = {kNoPaint};
        for (const Swatch &sw : doc.swatches) {
            swatches.push_back(sw.name);
            m_swatch_ids.push_back(sw.id);
        }
        auto menu = [&](const std::vector<std::string> &items, int width) {
            auto *d = new Dropdown(bar, items, {}, Dropdown::ComboBox, items.front());
            d->set_fixed_size(Vector2i(width, 26));
            d->set_max_size(Vector2i(width, 26));    // the theme's minimum is wider
            return d;
        };

        /* Pictures take the fill tint as their opacity, and the stroke as a
         * border on the frame. Their fill color stays None. */
        new Label(bar, " Fill", "sans-bold");
        m_fill = menu(swatches, 90);
        m_fill->set_selected_callback([this](int i) {
            const SwatchId id = m_swatch_ids[i];
            m_view->apply_shape_style([id](Shape &sh) { sh.fill.swatch = id; });
        });
        std::vector<std::string> tints;
        for (float t : kTints)
            tints.push_back(std::to_string((int) t) + "%");
        m_fill_tint = menu(tints, 62);
        m_fill_tint->set_tooltip("Fill tint. For a picture, its opacity.");
        m_fill_tint->set_selected_callback([this](int i) {
            const float t = kTints[i];
            m_view->apply_item_style([t](Shape &sh) { sh.fill.tint = t; },
                                     [t](PlacedImage &im) { im.opacity = t; });
        });

        /* A picture starts with no border; picking a weight or style
         * gives it a black one so the choice shows. */
        auto border = [](PlacedImage &im) {
            if (im.stroke.paint.none())
                im.stroke.paint = {kBlack, 100.f};
        };
        new Label(bar, " Stroke", "sans-bold");
        m_stroke = menu(swatches, 90);
        m_stroke->set_selected_callback([this](int i) {
            const SwatchId id = m_swatch_ids[i];
            m_view->apply_item_style([id](Shape &sh) { sh.stroke.paint.swatch = id; },
                                     [id](PlacedImage &im) { im.stroke.paint.swatch = id; });
        });
        m_weight = menu({"Hairline", "0.5 pt", "1 pt", "2 pt", "4 pt", "6 pt", "8 pt", "12 pt"}, 76);
        m_weight->set_selected_callback([this, border](int i) {
            const float w = kWeights[i];
            m_view->apply_item_style([w](Shape &sh) { sh.stroke.weight = w; },
                                     [w, border](PlacedImage &im) {
                                         im.stroke.weight = w;
                                         border(im);
                                     });
        });
        m_style = menu({"Solid", "Dashed", "Dotted", "Dash-dot", "Double", "Thick-thin",
                        "Thin-thick", "Thin-thick-thin"}, 110);
        m_style->set_selected_callback([this, border](int i) {
            const LineStyle ls = (LineStyle) i;
            m_view->apply_item_style([ls](Shape &sh) { sh.stroke.style = ls; },
                                     [ls, border](PlacedImage &im) {
                                         im.stroke.style = ls;
                                         border(im);
                                     });
        });

        new Label(bar, " Sides", "sans-bold");
        m_sides = new IntSpin(bar, 6, 1, 3, 100);
        m_sides->set_fixed_size(Vector2i(52, 26));
        m_sides->set_callback([this](int v) {
            palette_change(m_sides, [&] {
                m_view->apply_shape_style([v](Shape &sh) { sh.sides = std::max(3, v); });
            });
        });
        new Label(bar, "Star", "sans-bold");
        m_star = new IntSpin(bar, 0, 5, 0, 100);
        m_star->set_units("%");
        m_star->set_fixed_size(Vector2i(62, 26));
        m_star->set_callback([this](int v) {
            palette_change(m_star, [&] {
                m_view->apply_shape_style([v](Shape &sh) { sh.star_inset = (float) v; });
            });
        });

        /* Type view. Leading 0 is PageMaker's automatic leading ("Auto").
         * With no text selected these are the defaults for a new block.
         * The font menu lists this publication, recent faces and favorites;
         * typing searches every installed family. Style is the family's
         * faces (Light, Condensed Bold, …). Type opens the rest. */
        Widget *type = m_type_box;
        auto type_number = [&](const char *label, const char *units, int width,
                               float step, float lo, float hi) {
            new Label(type, label, "sans-bold");
            auto *box = new FloatSpin(type, 0.f, step, lo, hi);
            box->number_format("%.2f");
            box->set_default_value("");     // left blank (mixed): no change
            box->set_units(units);
            box->set_fixed_size(Vector2i(width, 26));
            return box;
        };

        new Label(type, "Font", "sans-bold");
        m_font = new Dropdown(type, Dropdown::ComboBox, "Serif");
        m_font->set_min_size(Vector2i(150, 30));
        m_font->set_max_size(Vector2i(300, 40));
        m_font->set_tooltip("Font family. Type to search all fonts. Click the star to keep a favorite.");
        m_font_menu.set_listener([this] {
            async([this] {
                if (m_font)
                    refill_palette_font();
            });
        });
        m_font->set_open_callback([this] {
            m_font_query.clear();
            refill_palette_font();
        });
        m_font->set_close_callback([this] { m_font_query.clear(); });
        m_font->set_query_callback([this](unsigned codepoint) {
            if (!font_menu_query_edit(m_font_query, codepoint))
                return false;
            refill_palette_font();
            return true;
        });

        /* A mixed selection shows blank; spinning starts from the first
         * character's value. Auto leading spins from the size it gives. */
        auto type_now = [this] {
            ComposerView::TypeStyle t;
            m_view->type_style(t);
            return t;
        };
        m_size = type_number("Size", "pt", 76, 1.f, 1.f, 720.f);
        m_size->set_tooltip("Type size, in points");
        m_size->set_blank_value([type_now] { return type_now().size; });
        m_size->TextBox::set_callback([this](const std::string &s) {
            char *end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (end == s.c_str())
                return false;
            const float size = std::clamp(v, 1.f, 720.f);
            m_size->set_value(size);
            palette_change(m_size, [&] {
                m_view->apply_type([size](CharStyle &cs) { cs.size = size; });
            });
            return true;
        });

        new Label(type, "Style", "sans-bold");
        m_face = new Dropdown(type, {"Regular"}, {}, Dropdown::ComboBox, "Regular");
        m_face->set_min_size(Vector2i(120, 30));
        m_face->set_max_size(Vector2i(220, 40));
        m_face->set_tooltip("Type style");
        m_face->set_selected_callback([this](int i) {
            if (m_syncing_type || i < 0 || i >= (int) m_styles.size())
                return;
            const FaceDesc f = m_styles[(size_t) i];
            const bool bold = f.weight >= 600;
            m_view->apply_type([f, bold](CharStyle &cs) {
                cs.face = f.style;
                cs.bold = bold;
                cs.italic = f.italic;
            });
        });

        /* The Colors palette, no label: the button shows the current paint
         * (chip plus name). Picking a row paints the selection; the pencil
         * mark edits, "+" adds. */
        m_colors = new ColorDropdown(type);
        m_colors->set_min_size(Vector2i(120, 30));
        m_colors->set_max_size(Vector2i(220, 40));
        m_colors->set_tooltip("Colors: paint the selection, add and edit swatches");
        m_colors->set_open_callback([this] { refill_colors(); });

        m_leading = type_number("Leading", "pt", 84, 1.f, 0.f, 1300.f);
        m_leading->set_tooltip("Line height. Auto (or 0) is proportional leading.");
        m_leading->set_blank_value([type_now] {
            const ComposerView::TypeStyle t = type_now();
            return t.leading > 0.f ? t.leading : t.size * ParaStyle().autoleading * 0.01f;
        });
        m_leading->set_format("([Aa]uto)|([-+]?[0-9]*\\.?[0-9]+)");
        m_leading->TextBox::set_callback([this](const std::string &s) {
            if (s.empty())
                return false;
            float lead = 0;
            if (s != "Auto" && s != "auto") {
                char *end = nullptr;
                lead = std::strtof(s.c_str(), &end);
                if (end == s.c_str())
                    return false;
            }
            lead = std::max(0.f, lead);
            if (lead <= 0.f)
                m_leading->TextBox::set_value("Auto");
            else
                m_leading->set_value(lead);
            palette_change(m_leading, [&] {
                m_view->apply_type([lead](CharStyle &cs) { cs.leading = lead; });
            });
            return true;
        });

        m_baseline = type_number("Baseline", "pt", 84, 0.5f, -500.f, 500.f);
        m_baseline->set_tooltip("Baseline shift. Positive raises the type.");
        m_baseline->set_blank_value([type_now] { return type_now().baseline; });
        m_baseline->TextBox::set_callback([this](const std::string &s) {
            char *end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (end == s.c_str())
                return false;
            const float shift = std::clamp(v, -500.f, 500.f);
            m_baseline->set_value(shift);
            palette_change(m_baseline, [&] {
                m_view->apply_type([shift](CharStyle &cs) { cs.baseline_shift = shift; });
            });
            return true;
        });

        /* Line spacing is the paragraph's: a multiple of each line's
         * leading. */
        m_spacing = type_number("Spacing", "\u00D7", 76, 0.1f, kMinLineSpacing, kMaxLineSpacing);
        m_spacing->set_tooltip("Line spacing, a multiple of the leading: 1 single, 2 double");
        m_spacing->set_blank_value([this] {
            const auto paras = m_view->selected_paragraphs();
            return paras.empty() ? 1.f : paras.front()->style.line_spacing;
        });
        m_spacing->TextBox::set_callback([this](const std::string &s) {
            char *end = nullptr;
            const float v = std::strtof(s.c_str(), &end);
            if (end == s.c_str())
                return false;
            const float spacing = std::clamp(v, kMinLineSpacing, kMaxLineSpacing);
            m_spacing->set_value(spacing);
            palette_change(m_spacing, [&] {
                m_view->apply_para([spacing](ParaStyle &ps) { ps.line_spacing = spacing; });
            });
            return true;
        });

    }

    void pick_font(const std::string &fam) {
        if (m_syncing_type || m_applying_font || fam.empty() || !m_view)
            return;
        m_applying_font = true;
        m_font_menu.note_use(fam);
        ComposerView::TypeStyle t;
        if (!m_view->type_style(t)) {
            m_applying_font = false;
            return;
        }
        int weight = t.bold ? 700 : 400;
        bool italic = t.italic;
        std::string named = t.face;
        if (named.empty())
            named = m_fonts.resolved_style(t.family, "", t.bold, t.italic);
        int described = weight;
        bool described_italic = italic;
        if (!named.empty() && m_fonts.describe(t.family, named, described, described_italic)) {
            weight = described;
            italic = described_italic;
        }
        const std::string width = FontLibrary::width_of(named);
        std::string face;
        if (m_fonts.match(fam, weight, italic, width, &face)) {
            int w = weight;
            bool it = italic;
            if (m_fonts.describe(fam, face, w, it)) {
                weight = w;
                italic = it;
            }
            const bool bold = weight >= 600;
            m_view->apply_type([fam, face, bold, italic](CharStyle &cs) {
                cs.family = fam;
                cs.face = face;
                cs.bold = bold;
                cs.italic = italic;
            });
        } else {
            m_view->apply_type([fam](CharStyle &cs) {
                cs.family = fam;
                cs.face.clear();
            });
        }
        m_applying_font = false;
    }

    void refill_palette_font() {
        if (!m_font || !m_view)
            return;
        ComposerView::TypeStyle t;
        if (!m_view->type_style(t))
            return;
        FontMenuView view;
        view.model = &m_font_menu;
        view.installed = m_fonts.families();
        if (view.installed.empty())
            view.installed.push_back("Serif");
        view.document = document_font_families(m_view->document());
        view.query = m_font_query;
        view.mixed = t.mix_family;
        view.current = t.mix_family ? std::string() : t.family;
        refill_font_menu(m_font, view,
                         [this](const std::string &fam) { pick_font(fam); },
                         [this](const std::string &fam) { m_font_menu.toggle_star(fam); });
    }

    void load_styles(const std::string &family) {
        m_style_family = family;
        m_styles = m_fonts.faces(family);
        std::vector<std::string> names;
        names.reserve(m_styles.size());
        for (const FaceDesc &f : m_styles)
            names.push_back(f.style);
        if (names.empty()) {
            names.push_back("Regular");
            m_styles.push_back(FaceDesc{"Regular", 400, false});
        }
        m_face->set_items(names);
    }

    /* What the palette is showing, as a CharStyle the specs dialog can edit.
     * Color is left alone: the dialog does not change it. */
    static CharStyle style_from(const ComposerView::TypeStyle &t) {
        CharStyle cs;
        cs.family = t.family;
        cs.face = t.face;
        cs.bold = t.bold;
        cs.italic = t.italic;
        cs.caps = t.caps;
        cs.underline = t.underline;
        cs.strike = t.strike;
        cs.kerning = t.kerning;
        cs.ligatures = t.ligatures;
        cs.no_break = t.no_break;
        cs.size = t.size;
        cs.leading = t.leading;
        cs.baseline_shift = t.baseline;
        cs.tracking = t.tracking;
        cs.hscale = t.hscale;
        return cs;
    }

    static bool type_is_mixed(const ComposerView::TypeStyle &t) {
        return t.mix_family || t.mix_style || t.mix_size || t.mix_leading ||
               t.mix_baseline || t.mix_caps || t.mix_deco || t.mix_track ||
               t.mix_hscale || t.mix_kerning || t.mix_ligatures || t.mix_no_break;
    }

    void apply_type_specs(const CharStyle &before, const CharStyle &after) {
        if (!m_view || after == before)
            return;
        m_view->apply_type([&](CharStyle &cs) {
            if (after.family != before.family) cs.family = after.family;
            if (after.face != before.face) cs.face = after.face;
            if (after.bold != before.bold) cs.bold = after.bold;
            if (after.italic != before.italic) cs.italic = after.italic;
            if (after.caps != before.caps) cs.caps = after.caps;
            if (after.underline != before.underline) cs.underline = after.underline;
            if (after.strike != before.strike) cs.strike = after.strike;
            if (after.size != before.size) cs.size = after.size;
            if (after.leading != before.leading) cs.leading = after.leading;
            if (after.tracking != before.tracking) cs.tracking = after.tracking;
            if (after.hscale != before.hscale) cs.hscale = after.hscale;
            if (after.baseline_shift != before.baseline_shift)
                cs.baseline_shift = after.baseline_shift;
            if (after.kerning != before.kerning) cs.kerning = after.kerning;
            if (after.ligatures != before.ligatures) cs.ligatures = after.ligatures;
            if (after.no_break != before.no_break) cs.no_break = after.no_break;
        });
    }

    /* The Image Assets window is made once and only hidden when closed, so
     * its folders, library and thumbnails are all there when it reopens. */
    void show_image_assets() {
        if (!m_assets) {
            m_assets = new ImageAssetsPanel(this, m_view, &m_images);
            m_assets->set_close_callback([this] {
                if (m_assets) {
                    m_assets->set_visible(false);
                    apply_docks();               // a docked one gives its room back
                    redraw();
                }
            });
            m_assets->on_dock = [this](DockableWindow::Dock side) { dock_window(m_assets, side); };
            m_assets->on_dock_resize = [this] { apply_docks(); };
            m_assets->center();
        } else {
            m_assets->set_visible(true);
            move_window_to_front(m_assets);
            apply_docks();               // reopened docked: take the room again
        }
        m_assets->refresh();             // catch up on changes made while hidden
        m_assets->request_focus();
    }

    /* ---- The page's right-click menu --------------------------------- *
     * Built for what was clicked: text being edited gets Cut, Copy, Paste;
     * items get those and Remove, Group or Ungroup, Align (two or more
     * items or groups), Arrange, Reload Picture and the layers to move to;
     * empty space gets Paste. */
    /* The shared right-click menu, emptied for refilling. */
    bool reset_context_popup() {
        if (!m_root_window)
            return false;
        if (!m_context_popup)
            m_context_popup = new PopupMenu(this, m_root_window, nullptr, false);
        m_context_popup->set_visible(false);
        while (m_context_popup->child_count() > 0)
            m_context_popup->remove_child_at(m_context_popup->child_count() - 1);
        return true;
    }

    /* Show the filled menu at `at`, kept on screen. */
    void open_context_popup(const Vector2i &at) {
        NVGcontext *ctx = nvg_context();
        const Vector2i pref = m_context_popup->preferred_size(ctx);
        m_context_popup->set_size(pref);
        m_context_popup->perform_layout(ctx);
        Vector2i pos = at;
        pos.x() = std::min(pos.x(), std::max(0, width() - pref.x()));
        if (pos.y() + pref.y() > height())
            pos.y() = std::max(0, pos.y() - pref.y());
        m_context_popup->set_position(pos);
        m_context_popup->set_visible(true);
        set_popup_visible(m_context_popup);
        m_context_popup->request_focus();
        redraw();
    }

    /* A menu item that runs once the menu has closed (the command may
     * rebuild the menu). */
    void context_item(const std::string &caption, int icon, bool enabled,
                      std::vector<Shortcut> keys, std::function<void()> fn) {
        if (keys.empty())
            keys = {{0, 0}};
        auto *mi = new MenuItem(m_context_popup, caption, icon, keys);
        mi->set_enabled(enabled);
        mi->set_callback([this, fn] {
            m_context_popup->set_visible(false);
            async([fn] { fn(); });
        });
    }

    /* Right-click on the page strip: commands for the current page. */
    void show_page_menu(const Vector2i &at) {
        if (!reset_context_popup())
            return;
        ComposerView *v = m_view;
        const size_t page = v->page_index();
        context_item("Duplicate Page", FA_CLONE, true, {}, [this, v] {
            v->duplicate_page(false);
            after_page_command();
        });
        if (v->document().setup.facing)
            context_item("Duplicate Spread", FA_BOOK_OPEN, v->can_duplicate_spread(), {}, [this, v] {
                v->duplicate_page(true);
                after_page_command();
            });
        new Separator(m_context_popup);
        context_item("Insert Page After", FA_PLUS, true, {}, [this, v] {
            v->insert_page(true);
            after_page_command();
        });
        context_item("Insert Page Before", 0, true, {}, [this, v] {
            v->insert_page(false);
            after_page_command();
        });
        new Separator(m_context_popup);
        const bool hidden = v->page_hidden(page);
        context_item(hidden ? "Show Page" : "Hide Page", hidden ? FA_EYE : FA_EYE_SLASH, true, {},
                     [v, hidden] { v->set_page_hidden(!hidden); });
        context_item("Remove Page", FA_TRASH_ALT, v->page_count() > 1, {}, [this, v] {
            v->remove_page();
            after_page_command();
        });
        open_context_popup(at);
    }

    /* A page added or removed: the spread and stack views reframe. */
    void after_page_command() {
        if (m_view->page_view() != PageView::One)
            frame_view();
    }

    void show_context_menu(const Vector2i &at) {
        if (!reset_context_popup())
            return;

        const int C = SYSTEM_COMMAND_MOD, S = GLFW_MOD_SHIFT;
        bool any = false, need_sep = false;
        auto item = [&](const std::string &caption, int icon, bool enabled,
                        std::vector<Shortcut> keys, std::function<void()> fn) {
            if (need_sep && any)
                new Separator(m_context_popup);
            need_sep = false;
            context_item(caption, icon, enabled, std::move(keys), std::move(fn));
            any = true;
        };
        auto section = [&] { need_sep = true; };

        ComposerView *v = m_view;
        const Point here = v->context_point();
        if (v->text_context()) {
            item("Cut", FA_CUT, v->can_copy(), {{C, GLFW_KEY_X}}, [v] { v->edit_cut(); });
            item("Copy", FA_COPY, v->can_copy(), {{C, GLFW_KEY_C}}, [v] { v->edit_copy(); });
            item("Paste", FA_PASTE, v->can_paste(), {{C, GLFW_KEY_V}}, [v] { v->edit_paste(); });
        } else if (v->selection().empty()) {
            item("Paste Here", FA_PASTE, v->can_paste(), {}, [v, here] { v->edit_paste(&here); });
        } else {
            item("Cut", FA_CUT, true, {{C, GLFW_KEY_X}}, [v] { v->edit_cut(); });
            item("Copy", FA_COPY, true, {{C, GLFW_KEY_C}}, [v] { v->edit_copy(); });
            item("Paste Here", FA_PASTE, v->can_paste(), {}, [v, here] { v->edit_paste(&here); });
            item("Remove", FA_TRASH_ALT, true, {}, [v] { v->remove_selection(); });
            section();
            if (v->can_group())
                item("Group", FA_OBJECT_GROUP, true, {{C, GLFW_KEY_G}}, [v] { v->group_selection(); });
            if (v->can_ungroup())
                item("Ungroup", FA_OBJECT_UNGROUP, true, {{C | S, GLFW_KEY_G}},
                     [v] { v->ungroup_selection(); });
            if (v->can_align()) {
                section();
                using A = ComposerView::AlignTo;
                item("Align Left", FA_ALIGN_LEFT, true, {}, [v] { v->align_selection(A::Left); });
                item("Align Center", FA_ALIGN_CENTER, true, {}, [v] { v->align_selection(A::Center); });
                item("Align Right", FA_ALIGN_RIGHT, true, {}, [v] { v->align_selection(A::Right); });
                item("Align Top", FA_ARROW_UP, true, {}, [v] { v->align_selection(A::Top); });
                item("Align Middle", FA_ARROWS_ALT_V, true, {}, [v] { v->align_selection(A::Middle); });
                item("Align Bottom", FA_ARROW_DOWN, true, {}, [v] { v->align_selection(A::Bottom); });
            }
            section();
            item("Bring to Front", 0, true, {{C, GLFW_KEY_F}},
                 [v] { v->arrange(ComposerView::Stack::Front); });
            item("Send to Back", 0, true, {}, [v] { v->arrange(ComposerView::Stack::Back); });
            if (v->can_reload_pictures()) {
                section();
                item("Reload Picture", FA_SYNC_ALT, true, {}, [v] { v->reload_pictures(); });
            }
            if (v->layers().size() > 1) {
                section();
                for (const Layer &l : v->layers())
                    if (l.visible && !l.locked) {
                        const LayerId id = l.id;
                        item("Move to " + l.name, FA_LAYER_GROUP, true, {},
                             [v, id] { v->move_selection_to_layer(id); });
                    }
            }
        }
        if (!any)
            return;
        open_context_popup(at);
    }

    /* The Layers window, like Image Assets: made once, hidden when closed. */
    void show_layers() {
        if (!m_layers) {
            m_layers = new LayersPanel(this, m_view);
            m_layers->set_close_callback([this] {
                if (m_layers) {
                    m_layers->set_visible(false);
                    apply_docks();
                    redraw();
                }
            });
            m_layers->on_dock = [this](DockableWindow::Dock side) { dock_window(m_layers, side); };
            m_layers->on_dock_resize = [this] { apply_docks(); };
            m_layers->fit_height(240);
            m_layers->center();
        } else {
            m_layers->set_visible(true);
            move_window_to_front(m_layers);
            apply_docks();
        }
        m_layers->refresh();
        m_layers->request_focus();
    }

    /* ---- Docking tool windows beside the pages ---------------------- *
     * A docked window fills a strip the full height of the page area on
     * one side; the rulers and pages give it that room. Two on one side sit
     * next to each other, Image Assets nearest the edge. Undocked, a window
     * goes back to the size it had floating, where it is. */
    std::vector<DockableWindow *> docked_windows() const {
        std::vector<DockableWindow *> out;
        for (DockableWindow *w : {(DockableWindow *) m_assets, (DockableWindow *) m_layers,
                                  (DockableWindow *) m_type_panel})
            if (w && w->visible() && w->docked() != DockableWindow::Dock::None)
                out.push_back(w);
        return out;
    }

    void dock_window(DockableWindow *w, DockableWindow::Dock side) {
        using Dock = DockableWindow::Dock;
        if (!w)
            return;
        const Dock was = w->docked();
        if (was == Dock::None && side != Dock::None)
            m_float_size[w] = w->size();
        w->set_docked(side);
        if (side == Dock::None && was != Dock::None) {
            auto it = m_float_size.find(w);
            if (it != m_float_size.end() && it->second.x() > 0) {
                w->set_size(it->second);
                if (NVGcontext *ctx = nvg_context())
                    w->perform_layout(ctx);
            }
        }
        apply_docks();
        redraw();
    }

    /* The room for each docked window (its width, within reason), then
     * the windows in it */
    void apply_docks() {
        using Dock = DockableWindow::Dock;
        const int cap = m_rulers->width() / 2;
        int left = 0, right = 0;
        m_dock_w.clear();
        for (DockableWindow *w : docked_windows()) {
            const int floor = std::max(150, w->min_size().x());
            const int dw = std::clamp(w->width(), floor, std::max(floor, cap));
            m_dock_w[w] = dw;
            (w->docked() == Dock::Left ? left : right) += dw;
        }
        m_rulers->set_side_insets(left, right);
        place_docked();
    }

    void place_docked() {
        using Dock = DockableWindow::Dock;
        const Vector2i a = m_rulers->absolute_position(), s = m_rulers->size();
        int left = 0, right = 0;
        bool moved = false;
        for (DockableWindow *w : docked_windows()) {
            auto it = m_dock_w.find(w);
            const int dw = it != m_dock_w.end() ? it->second : w->width();
            Vector2i pos;
            if (w->docked() == Dock::Left) {
                pos = Vector2i(a.x() + left, a.y());
                left += dw;
            } else {
                right += dw;
                pos = Vector2i(a.x() + s.x() - right, a.y());
            }
            const Vector2i size(dw, s.y());
            if (pos == w->position() && size == w->size())
                continue;
            w->set_position(pos);
            w->set_size(size);
            if (NVGcontext *ctx = nvg_context())
                w->perform_layout(ctx);
            moved = true;
        }
        if (moved)
            redraw();
    }

    void type_specs() {
        ComposerView::TypeStyle t;
        if (!m_view || !m_view->type_style(t))
            return;
        if (!m_type_panel) {
            m_type_panel = new TypeSpecsPanel(this, m_fonts, m_font_menu,
                                              [this](const CharStyle &before, const CharStyle &after) {
                apply_type_specs(before, after);
            });
            m_type_panel->on_dock = [this](DockableWindow::Dock side) {
                dock_window(m_type_panel, side);
            };
            m_type_panel->on_dock_resize = [this] { apply_docks(); };
            /* The panel is disposed when closed. Its dock side and width
             * are kept, so it reopens where it was. */
            m_type_panel->set_close_callback([this] {
                if (!m_type_panel)
                    return;
                m_type_dock = m_type_panel->docked();
                m_type_dock_w = m_type_panel->width();
                auto it = m_float_size.find(m_type_panel);
                m_type_float_size = it != m_float_size.end() ? it->second : m_type_panel->size();
                m_float_size.erase(m_type_panel);
                m_dock_w.erase(m_type_panel);
                m_type_panel = nullptr;
                apply_docks();
                redraw();
            });
        } else {
            move_window_to_front(m_type_panel);
        }
        m_type_panel->load(style_from(t), type_is_mixed(t),
                           document_font_families(m_view->document()));
        m_type_panel->set_apply_enabled(true);
        if (m_type_panel->size() == Vector2i(0, 0)) {
            if (m_type_float_size.x() > 0) {
                m_type_panel->set_size(m_type_float_size);
                m_type_panel->relayout();
            }
            m_type_panel->center();
            if (m_type_dock != DockableWindow::Dock::None) {
                m_float_size[m_type_panel] = m_type_panel->size();
                m_type_panel->set_size(Vector2i(m_type_dock_w, m_type_panel->height()));
                m_type_panel->set_docked(m_type_dock);
                apply_docks();
            }
        }
        m_type_panel->request_focus();
    }

    /* A change from a palette field. The steps of one spin are one undo
     * step; a typed value is a step of its own. */
    template <typename Box> void palette_change(Box *box, const std::function<void()> &change) {
        m_view->set_undo_merge(box->spinning() ? box : nullptr);
        change();
        m_view->set_undo_merge(nullptr);
    }

    void apply_geometry(int field, float v) {
        ComposerView::Geometry g;
        if (!m_view->geometry(g))
            return;
        float *f[] = {&g.x, &g.y, &g.w, &g.h, &g.angle};
        *f[field] = v;
        m_view->set_geometry(g);
    }

    void sync_palette() {
        if (!m_fill)
            return;
        ComposerView::Geometry g;
        const bool one = m_view->geometry(g);
        const float vals[] = {g.x, g.y, g.w, g.h, g.angle};
        for (int i = 0; i < 5; ++i) {
            m_geom[i]->set_enabled(one);
            if (m_geom[i]->focused())
                continue;
            if (one)
                m_geom[i]->set_value(vals[i]);
            else
                m_geom[i]->TextBox::set_value("");
        }
        const Shape sh = m_view->shape_style();
        auto swatch_index = [&](SwatchId id) {
            for (size_t i = 0; i < m_swatch_ids.size(); ++i)
                if (m_swatch_ids[i] == id)
                    return (int) i;
            return 0;
        };
        auto nearest = [](const float *v, int n, float x) {
            int best = 0;
            for (int i = 1; i < n; ++i)
                if (std::fabs(v[i] - x) < std::fabs(v[best] - x))
                    best = i;
            return best;
        };
        m_fill->set_selected_index(swatch_index(sh.fill.swatch));
        m_fill->set_enabled(m_view->shapes_selected() || !m_view->pictures_selected());
        m_fill_tint->set_selected_index(nearest(kTints, (int) std::size(kTints), sh.fill.tint));
        m_stroke->set_selected_index(swatch_index(sh.stroke.paint.swatch));
        m_weight->set_selected_index(nearest(kWeights, 8, sh.stroke.weight));
        m_style->set_selected_index((int) sh.stroke.style);
        if (!m_sides->focused())
            m_sides->set_value(sh.sides);
        if (!m_star->focused())
            m_star->set_value((int) sh.star_inset);
        sync_type();
        sync_spacing();
        if (m_para_panel)
            m_para_panel->load(m_view->selected_paragraphs(), m_view->document().styles);
        sync_character_panel();
    }

    void sync_character_panel() {
        if (!m_char_panel)
            return;
        ComposerView::TypeStyle t;
        CharacterPanel::State st;
        st.enabled = m_view->type_style(t);
        st.style = t.char_style;
        st.mixed_style = t.mix_char_style;
        st.overridden = t.char_overridden;
        st.swatch = t.swatch;
        st.mixed_color = t.mix_color;
        st.tint = t.tint;
        st.mixed_tint = t.mix_tint;
        const PageDoc &doc = m_view->document();
        st.shown = doc.text_color(t.swatch, t.tint, t.rgb);
        m_char_panel->load(st, doc.char_styles, doc.swatches, m_view->selected_paragraphs());
    }

    void toggle_paragraph_panel() {
        if (!m_para_btn)
            return;
        m_para_btn->set_pushed(!m_para_btn->pushed());
        perform_layout();
    }

    void sync_spacing() {
        if (!m_spacing)
            return;
        const auto paras = m_view->selected_paragraphs();
        m_spacing->set_enabled(!paras.empty());
        if (m_spacing->focused() || m_spacing->spinning())
            return;                      // it shows the value being set
        bool mixed = false;
        for (const Paragraph *p : paras)
            mixed |= p->style.line_spacing != paras.front()->style.line_spacing;
        if (paras.empty() || mixed)
            m_spacing->TextBox::set_value("");
        else
            m_spacing->set_value(paras.front()->style.line_spacing);
    }

    void sync_type() {
        if (!m_font)
            return;
        ComposerView::TypeStyle t;
        const bool ok = m_view->type_style(t);
        m_font->set_enabled(ok);
        m_face->set_enabled(ok);
        if (m_colors)
            m_colors->set_enabled(ok);
        m_size->set_enabled(ok);
        m_leading->set_enabled(ok);
        m_baseline->set_enabled(ok);
        if (m_specs)
            m_specs->set_enabled(ok);
        if (m_type_panel)
            m_type_panel->set_apply_enabled(ok);
        /* The Colors button follows the current paint without opening. */
        if (m_colors && (!m_colors->popup() || !m_colors->popup()->visible())) {
            const SwatchId paint = current_paint();
            if (const Swatch *sw = m_view->document().find_swatch(paint))
                m_colors->set_swatch(sw->name, sw->rgb);
            else
                m_colors->clear_swatch();
        }
        if (!ok)
            return;
        m_syncing_type = true;
        /* Choosing a row runs this while that row still exists. Rebuilding
           the open menu would free it. A search refill is the query callback. */
        if (!m_applying_font && m_font->popup() && !m_font->popup()->visible())
            refill_palette_font();
        if (m_style_family != t.family)
            load_styles(t.family);
        if (t.mix_style) {
            m_face->set_selected_index(-1);
            m_face->set_caption("\u2014");
        } else {
            std::string want = t.face;
            if (want.empty())
                want = m_fonts.resolved_style(t.family, "", t.bold, t.italic);
            int idx = -1;
            for (int i = 0; i < (int) m_styles.size(); ++i)
                if (m_styles[(size_t) i].style == want)
                    idx = i;
            if (idx >= 0)
                m_face->set_selected_index(idx);
            else {
                m_face->set_selected_index(-1);
                m_face->set_caption(want.empty() ? std::string("\u2014") : want);
            }
        }
        m_syncing_type = false;
        if (!m_size->focused() && !m_size->spinning()) {
            if (t.mix_size)
                m_size->TextBox::set_value("");
            else
                m_size->set_value(t.size);
        }
        if (!m_leading->focused() && !m_leading->spinning()) {
            if (t.mix_leading)
                m_leading->TextBox::set_value("");
            else if (t.leading <= 0.f)
                m_leading->TextBox::set_value("Auto");
            else
                m_leading->set_value(t.leading);
        }
        if (!m_baseline->focused() && !m_baseline->spinning()) {
            if (t.mix_baseline)
                m_baseline->TextBox::set_value("");
            else
                m_baseline->set_value(t.baseline);
        }
        if (m_type_panel)
            m_type_panel->load(style_from(t), type_is_mixed(t),
                               document_font_families(m_view->document()));
    }

    /* The view's tool, mirrored in the toolbar's radio buttons. */
    void apply_tool(ComposerView::Tool t) {
        m_view->set_tool(t);
        sync_tool_buttons(t);
    }

    void sync_tool_buttons(ComposerView::Tool t) {
        for (auto &tb : m_tool_btns)
            tb.second->set_pushed(tb.first == t);
    }

    void zoom_to(double z) {
        m_scroll->set_zoom_about(z, m_scroll->size() / 2);
        update_status();
    }

    /* The dropdown mirrors the actual zoom: a preset when it matches one,
     * the plain percentage otherwise. */
    void sync_zoom_menu() {
        if (!m_zoom_menu)
            return;
        const double z = m_scroll->zoom();
        static const double zv[] = {0.5, 1, 2, 4, 8};
        for (int i = 0; i < 5; ++i)
            if (std::fabs(z - zv[i]) < 0.005) {
                if (m_zoom_menu->selected_index() != i + 1)
                    m_zoom_menu->set_selected_index(i + 1);
                return;
            }
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.0f%%", z * 100.0);
        if (m_zoom_menu->caption() != buf)
            m_zoom_menu->set_caption(buf);
    }

    void fit_page() {
        float w, h;
        m_view->fit_extent(w, h);
        Vector2i panel = m_scroll->size();
        double z = std::min((panel.x() - 24) / (w + 24.0),
                            (panel.y() - 24) / (h + 24.0));
        if (!(z > 0.02))
            z = 1;
        m_scroll->set_zoom(z);
        frame_view();
        update_status();
    }

    /* Center the spread, or the current page (its spread when facing) when
     * the pages are stacked. */
    void frame_view() {
        perform_layout();
        double z = m_scroll->zoom();
        const Vector2f v = m_view->page_origin() + m_view->fit_center();
        m_scroll->set_pan_offset(ZoomScrollPanel::Vector2d(m_scroll->size().x() * 0.5 - v.x() * z,
                                                           m_scroll->size().y() * 0.5 - v.y() * z));
    }

    /* Put a page point of the current page at the middle of the panel. */
    void center_on(const Vector2f &page_pt) {
        perform_layout();
        double z = m_scroll->zoom();
        Vector2f v = m_view->page_origin() + m_view->page_slot_offset(m_view->page_index()) + page_pt;
        m_scroll->set_pan_offset(ZoomScrollPanel::Vector2d(m_scroll->size().x() * 0.5 - v.x() * z,
                                                           m_scroll->size().y() * 0.5 - v.y() * z));
    }

    template <typename F> void for_each_run(F fn) {
        m_view->push_undo();
        for (StoryEntry &se : m_view->document().stories)
            for (Paragraph &p : se.story.paragraphs)
                for (Run &r : p.runs)
                    fn(r.style);
        m_view->recompose();
    }

    void update_status() {
        if (!m_status)
            return;
        const auto &comps = m_view->compositions();
        size_t lines = 0;
        double ms = 0;
        bool overset = false;
        for (const Composition &c : comps) {
            lines += c.lines.size();
            ms += c.compose_ms;
            overset |= c.overset;
        }
        char buf[240];
        const PageDoc &doc = m_view->document();
        const size_t page = m_view->page_index();
        const size_t np = doc.pages.size();
        const size_t nitems = page < np ? doc.pages[page].items.size() : 0;
        const bool hidden = m_view->page_hidden(page);
        std::snprintf(buf, sizeof buf,
                      "  Page %zu of %zu%s   |   %zu items, %zu stories, %zu lines, composed in %.2f ms%s   |   zoom %.0f%%",
                      page + 1, np, hidden ? " (hidden)" : "", nitems, comps.size(), lines, ms,
                      overset ? "   |   story overset (red arrow)" : "", m_scroll->zoom() * 100.0);
        std::string picture;
        if (m_view->selection().size() == 1)
            if (const Item *it = doc.find_item(m_view->selection().front()))
                if (const PlacedImage *im = it->image())
                    if (const ImageAsset *a = doc.find_image(im->asset)) {
                        const int ppi = a->meta.ppi_x > 0 ? (int) std::lround(a->meta.ppi_x) : 72;
                        picture = "   |   " + a->name + "  " + std::to_string(a->meta.width_px) +
                                  "\u00D7" + std::to_string(a->meta.height_px) + " px  " +
                                  std::to_string(ppi) + " ppi";
                    }
        m_status->set_caption(std::string(buf) + picture + m_note);
        if (m_remove_page) {
            m_remove_page->set_enabled(np > 1);
            m_move_earlier->set_enabled(page > 0);
            m_move_later->set_enabled(page + 1 < np);
            m_hide_page->set_caption(hidden ? "Show Page" : "Hide Page");
            m_dup_spread->set_enabled(m_view->can_duplicate_spread());
        }
        if (m_to_front) {
            const bool can = !m_view->selection().empty();
            m_to_front->set_enabled(can);
            m_forward->set_enabled(can);
            m_backward->set_enabled(can);
            m_to_back->set_enabled(can);
            m_group->set_enabled(m_view->can_group());
            m_ungroup->set_enabled(m_view->can_ungroup());
        }
        sync_zoom_menu();
        m_undo_btn->set_enabled(m_view->can_undo());
        m_redo_btn->set_enabled(m_view->can_redo());
    }

    /* A trailing note in the status bar ("wrote out.pdf"), kept until the
     * next one. */
    void set_note(const std::string &note) {
        m_note = "   |   " + note;
        update_status();
    }

    /* `settings` null: the publication's own */
    bool export_pdf_to(const std::string &path, const PdfSettings *settings = nullptr) {
        /* Off-screen linked pictures are unloaded. Export needs the bytes,
         * or the page is drawn with a gray stand-in. */
        const PageDoc &doc = m_view->document();
        for (const Page &page : doc.pages)
            for (const Item &it : page.items)
                if (const PlacedImage *im = it.image())
                    m_images.load(im->asset, nullptr);
        return export_pdf(path, doc, m_view->compositions(), &m_images, settings, &m_vectors);
    }

    /* File > Document Setup: page size, margins, columns, bleed, and what
     * happens to the items already placed */
    void document_setup() {
        const PageDoc &doc = m_view->document();
        ask_document_setup(this, doc.setup, doc.pages.size(),
                           [this](const PageSetup &setup, SetupAdjust how) {
            m_view->change_setup(setup, how);
            set_note(how == SetupAdjust::Adjust ? "document setup changed; layout adjusted"
                     : how == SetupAdjust::Scale ? "document setup changed; layout scaled"
                     : "document setup changed");
        });
    }

    /* File > PDF Settings: kept with the publication, no export */
    void pdf_settings_dialog() {
        const PageDoc &doc = m_view->document();
        ask_pdf_settings(this, doc.pdf, (int) doc.pages.size(), doc.setup.bleed, "OK",
                         [this](const PdfSettings &s) { m_view->set_pdf_settings(s); });
    }

    /* File > Export PDF: the settings, the check, then where to write it */
    void export_pdf_dialog() {
        const PageDoc &doc = m_view->document();
        ask_pdf_settings(this, doc.pdf, (int) doc.pages.size(), doc.setup.bleed,
                         "Export\u2026", [this](const PdfSettings &s) {
            m_view->set_pdf_settings(s);
            auto write = [this, s] {
                auto paths = file_dialog({{"pdf", "PDF document"}}, true, false, "");
                if (paths.empty() || paths[0].empty())
                    return;
                std::string path = paths[0];
                if (path.size() < 4 || path.compare(path.size() - 4, 4, ".pdf") != 0)
                    path += ".pdf";
                const bool ok = export_pdf_to(path, &s);
                set_note(ok ? "wrote " + path : "PDF export FAILED: " + path);
                if (ok && s.open_after)
                    open_with_desktop(path);
            };
            if (!s.preflight) {
                write();
                return;
            }
            const std::vector<std::string> warnings =
                pdf_preflight(m_view->document(), m_view->compositions(), &m_images, s);
            if (warnings.empty()) {
                write();
                return;
            }
            /* What may print badly, and the choice to go on */
            auto *d = new ModalDialog(this, "Before Exporting");
            Widget *body = d->body();
            body->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Minimum, 0, 6));
            Label *intro = new Label(body, "These may not print as you expect:", "sans-bold");
            intro->set_fixed_size(Vector2i(520, 0));
            const size_t shown = std::min<size_t>(warnings.size(), 12);
            for (size_t i = 0; i < shown; ++i) {
                Label *l = new Label(body, "\u2022 " + warnings[i], "sans");
                l->set_fixed_size(Vector2i(520, 0));
            }
            if (warnings.size() > shown)
                new Label(body, "\u2026 and " + std::to_string(warnings.size() - shown) + " more",
                          "sans");
            d->add_button("Cancel", {}, ModalDialog::Role::Cancel);
            d->add_button("Export Anyway", write, ModalDialog::Role::Default);
            d->open();
        });
    }

    /* Hand a file to the desktop's viewer (xdg-open) */
    void open_with_desktop(const std::string &path) {
#if !defined(_WIN32)
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
#if defined(__APPLE__)
            execlp("open", "open", path.c_str(), (char *) nullptr);
#else
            execlp("xdg-open", "xdg-open", path.c_str(), (char *) nullptr);
#endif
            _exit(127);
        }
#else
        (void) path;
#endif
    }

    /* Print through CUPS: export, then hand the PDF to lp. */
    void print_document() {
        const std::string path = "/tmp/pagemade-print.pdf";
        /* The publication's settings, without what a print shop needs:
         * a desk printer gets every page at its trim */
        PdfSettings print = m_view->document().pdf;
        print.all_pages = true;
        print.bleed = false;
        print.crop_marks = false;
        if (!export_pdf_to(path, &print)) {
            set_note("print failed: could not write " + path);
            return;
        }
#if defined(_WIN32)
        set_note("printing is not wired up on Windows yet; use Export PDF");
#else
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
            execlp("lp", "lp", path.c_str(), (char *) nullptr);
            _exit(127);
        }
        set_note(pid > 0 ? "sent to the default printer (lp)" : "print failed (fork)");
#endif
    }

    bool keyboard_event(int key, int scancode, int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (ModalDialog::any_open())
            return false;                // no menu shortcuts behind a dialog
        if (action == GLFW_PRESS && m_menubar && m_menubar->process_shortcuts(modifiers, key))
            return true;
        if (action != GLFW_PRESS || !(modifiers & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER)))
            return false;
        switch (key) {
        case GLFW_KEY_Z:
            if (modifiers & GLFW_MOD_SHIFT) m_view->redo(); else m_view->undo();
            return true;
        case GLFW_KEY_Y:
            m_view->redo();
            return true;
        case GLFW_KEY_P: print_document(); return true;
        case GLFW_KEY_N: new_document(); return true;
        case GLFW_KEY_O: open_document_dialog(); return true;
        case GLFW_KEY_D: place_dialog(); return true;
        case GLFW_KEY_S:
            if (modifiers & GLFW_MOD_SHIFT) save_as(); else save();
            return true;
        case GLFW_KEY_Q: quit(); return true;
        case GLFW_KEY_0: fit_page(); return true;
        case GLFW_KEY_5: zoom_to(0.5); return true;
        case GLFW_KEY_7: zoom_to(0.75); return true;
        case GLFW_KEY_1: zoom_to(1); return true;
        case GLFW_KEY_2: zoom_to(2); return true;
        case GLFW_KEY_4: zoom_to(4); return true;
        case GLFW_KEY_8: zoom_to(8); return true;
        }
        return false;
    }

    bool scroll_event(const Vector2i &p, const Vector2f &rel) override {
        bool r = Screen::scroll_event(p, rel);
        update_status();
        return r;
    }

    bool save_png(const std::string &path) {
        int w = 0, h = 0;
        glfwGetFramebufferSize(glfw_window(), &w, &h);
        std::vector<unsigned char> buf((size_t) w * h * 4);
        draw_setup();
        draw_contents();
        draw_widgets();
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        stbi_flip_vertically_on_write(1);
        return stbi_write_png(path.c_str(), w, h, 4, buf.data(), w * 4) != 0;
    }

    /* A page point as the screen coordinates GLFW callbacks expect. */
    Vector2f screen_point(const Vector2f &pt) {
        Vector2f v = m_view->page_origin() + pt;
        auto pan = m_scroll->pan_offset();
        double z = m_scroll->zoom();
        Vector2i a = m_scroll->absolute_position();
        /* cursor_pos_callback_event() shifts by (-1, -2) and, on X11 and
         * Windows, divides by the pixel ratio. */
        Vector2f s((float) (a.x() + pan.x() + v.x() * z) + 1,
                   (float) (a.y() + pan.y() + v.y() * z) + 2);
#if !defined(NANOGUI_WAYLAND) && !defined(__APPLE__)
        s = s * pixel_ratio();
#endif
        return s;
    }

    /* A left-button drag between two page points, fed through Screen's
     * GLFW callbacks so it takes the real path (Screen -> ZoomScrollPanel
     * -> ComposerView). Used by --drag to test the pointer tool headlessly. */
    void synthetic_drag(const Vector2f &from, const Vector2f &to) {
        Vector2f a = screen_point(from), b = screen_point(to);
        cursor_pos_callback_event(a.x(), a.y());
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0);
        for (int i = 1; i <= 8; ++i) {
            Vector2f p = a + (b - a) * (i / 8.f);
            cursor_pos_callback_event(p.x(), p.y());
        }
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0);
    }

    /* A plain left click at a page point. */
    void synthetic_click(const Vector2f &pt) {
        Vector2f s = screen_point(pt);
        cursor_pos_callback_event(s.x(), s.y());
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0);
        mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0);
    }

    /* UTF-8 text as character events, as if typed. */
    void synthetic_type(const std::string &utf8) {
        for (size_t i = 0; i < utf8.size();) {
            unsigned char c = (unsigned char) utf8[i];
            int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            unsigned int cp = c & (0x7F >> n);
            for (int k = 1; k <= n && i + k < utf8.size(); ++k)
                cp = (cp << 6) | ((unsigned char) utf8[i + k] & 0x3F);
            char_callback_event(cp);
            i += (size_t) n + 1;
        }
    }

    /* A key press and release with modifiers. */
    void synthetic_key(int key, int mods) {
        key_callback_event(key, 0, GLFW_PRESS, mods);
        key_callback_event(key, 0, GLFW_RELEASE, mods);
    }

    /* --tool, --fill and --stroke: what the toolbox and palette would do. */
    void scripted(const std::string &what, const std::string &arg) {
        std::vector<std::string> f;
        for (size_t i = 0; i <= arg.size();) {
            size_t c = arg.find(',', i);
            f.push_back(arg.substr(i, c == std::string::npos ? std::string::npos : c - i));
            if (c == std::string::npos) break;
            i = c + 1;
        }
        if (what == "tool") {
            static const std::pair<const char *, ComposerView::Tool> tools[] = {
                {"pointer", ComposerView::Tool::Pointer}, {"crop", ComposerView::Tool::Crop},
                {"text", ComposerView::Tool::Text},
                {"rotate", ComposerView::Tool::Rotate},   {"line", ComposerView::Tool::Line},
                {"rect", ComposerView::Tool::Rect},       {"ellipse", ComposerView::Tool::Ellipse},
                {"polygon", ComposerView::Tool::Polygon},
            };
            for (const auto &t : tools)
                if (f[0] == t.first)
                    apply_tool(t.second);
            return;
        }
        SwatchId id = kNoPaint;
        for (const Swatch &sw : m_view->document().swatches)
            if (sw.name == f[0])
                id = sw.id;
        if (what == "fill") {
            const float tint = f.size() > 1 ? (float) std::atof(f[1].c_str()) : 100.f;
            m_view->apply_item_style([&](Shape &sh) { sh.fill = {id, tint}; },
                                     [&](PlacedImage &im) { im.opacity = tint; });
        } else {
            const float w = f.size() > 1 ? (float) std::atof(f[1].c_str()) : 1.f;
            const std::string st = f.size() > 2 ? f[2] : "solid";
            static const std::pair<const char *, LineStyle> styles[] = {
                {"dashed", LineStyle::Dashed},        {"dotted", LineStyle::Dotted},
                {"dashdot", LineStyle::DashDot},      {"double", LineStyle::Double},
                {"thickthin", LineStyle::ThickThin},  {"thinthick", LineStyle::ThinThick},
                {"thinthickthin", LineStyle::ThinThickThin},
            };
            LineStyle ls = LineStyle::Solid;
            for (const auto &s : styles)
                if (st == s.first)
                    ls = s.second;
            const Stroke stroke{{id, 100.f}, w, ls};
            m_view->apply_item_style([&](Shape &sh) { sh.stroke = stroke; },
                                     [&](PlacedImage &im) { im.stroke = stroke; });
        }
    }

    ComposerView *view() { return m_view; }
    void set_snap(bool on) {
        m_view->set_snap(on);
        m_snap_btn->set_pushed(on);
        if (m_snap_item)
            m_snap_item->set_icon(on ? FA_CHECK : 0);
    }
    Button *baselines_box() { return m_baselines; }
    Button *loose_box() { return m_loose; }
    Button *kern_box() { return m_kern; }

private:
    FontLibrary     m_fonts;
    ImageStore      m_images;
    VectorStore     m_vectors;
    Hyphenator      m_hyphenator;
    MenuBar         *m_menubar = nullptr;
    ZoomScrollPanel *m_scroll = nullptr;
    RulerFrame      *m_rulers = nullptr;
    MenuItem        *m_rulers_item = nullptr, *m_guides_item = nullptr, *m_snap_item = nullptr;
    MenuItem        *m_lock_guides_item = nullptr;
    ComposerView    *m_view = nullptr;
    PageIconStrip   *m_pages = nullptr;
    Dropdown        *m_zoom_menu = nullptr;
    Button          *m_undo_btn = nullptr, *m_redo_btn = nullptr;
    Button          *m_kern = nullptr, *m_baselines = nullptr, *m_loose = nullptr;
    std::vector<std::pair<ComposerView::Tool, ToolButton *>> m_tool_btns;
    Button          *m_snap_btn = nullptr;
    FloatSpin       *m_geom[5] = {};
    Dropdown        *m_fill = nullptr, *m_fill_tint = nullptr, *m_stroke = nullptr;
    Dropdown        *m_weight = nullptr, *m_style = nullptr;
    IntSpin         *m_sides = nullptr, *m_star = nullptr;
    std::vector<SwatchId> m_swatch_ids;
    Button          *m_mode_btn = nullptr;
    Widget          *m_object_box = nullptr, *m_type_box = nullptr;
    bool            m_type_mode = false;
    FontMenuModel   m_font_menu;
    SwatchLibrary   m_swatch_lib;
    ColorDropdown   *m_colors = nullptr;
    std::string     m_font_query;
    bool            m_applying_font = false;
    std::vector<FaceDesc> m_styles;
    std::string     m_style_family;
    bool            m_syncing_type = false;
    PopupButton     *m_para_btn = nullptr;
    ParagraphPanel  *m_para_panel = nullptr;
    PopupButton     *m_char_btn = nullptr;
    MenuItem        *m_smart_quotes = nullptr;
    MenuItem        *m_caption_item = nullptr, *m_columns_item = nullptr;
    CharacterPanel  *m_char_panel = nullptr;
    Dropdown        *m_font = nullptr, *m_face = nullptr;
    Button          *m_specs = nullptr;
    TypeSpecsPanel  *m_type_panel = nullptr;
    ImageAssetsPanel *m_assets = nullptr;
    LayersPanel      *m_layers = nullptr;
    Window           *m_root_window = nullptr;
    PopupMenu        *m_context_popup = nullptr;   // the page's right-click menu
    /* Docked tool windows: the size each had floating, and the width it
     * has docked. The Type panel's dock is kept across closing it. */
    std::map<DockableWindow *, Vector2i> m_float_size;
    std::map<DockableWindow *, int> m_dock_w;
    DockableWindow::Dock m_type_dock = DockableWindow::Dock::None;
    int              m_type_dock_w = 0;
    Vector2i         m_type_float_size{0, 0};
    FloatSpin       *m_size = nullptr, *m_leading = nullptr, *m_baseline = nullptr;
    FloatSpin       *m_spacing = nullptr;
    MenuItem        *m_dup_spread = nullptr;
    MenuItem        *m_remove_page = nullptr, *m_move_earlier = nullptr, *m_move_later = nullptr;
    MenuItem        *m_hide_page = nullptr;
    MenuItem        *m_to_front = nullptr, *m_forward = nullptr, *m_backward = nullptr, *m_to_back = nullptr;
    MenuItem        *m_group = nullptr, *m_ungroup = nullptr;
    Label           *m_status = nullptr;
    std::string     m_note;
    std::string     m_doc_path;          // empty: Untitled
    bool            m_embed_assets = false;
    bool            m_quitting = false;
    static inline PagemadeApp *s_instance = nullptr;
};

static bool parse_pair(const char *s, float &a, float &b) {
    return std::sscanf(s, "%f,%f", &a, &b) == 2;
}

/* A headless input action, replayed through Screen's GLFW callbacks. */
struct Action {
    enum Kind { Drag, Click, Type, Key, Tool, Fill, Stroke, Save, SaveEmbedded,
                UiClick, UiRightClick, UiDrag, UiScroll } kind;
    std::array<float, 4> d{};
    std::string text;
    int key = 0, mods = 0;
};

/* "shift+ctrl+a", "enter", "b" -> GLFW key and modifiers. */
static bool parse_key(const std::string &spec, int &key, int &mods) {
    static const std::pair<const char *, int> named[] = {
        {"enter", GLFW_KEY_ENTER},       {"backspace", GLFW_KEY_BACKSPACE},
        {"delete", GLFW_KEY_DELETE},     {"left", GLFW_KEY_LEFT},
        {"right", GLFW_KEY_RIGHT},       {"up", GLFW_KEY_UP},
        {"down", GLFW_KEY_DOWN},         {"home", GLFW_KEY_HOME},
        {"end", GLFW_KEY_END},           {"escape", GLFW_KEY_ESCAPE},
        {"space", GLFW_KEY_SPACE},       {"tab", GLFW_KEY_TAB},
        {"minus", GLFW_KEY_MINUS},
    };
    mods = 0;
    size_t i = 0;
    for (;;) {
        size_t plus = spec.find('+', i);
        std::string tok = spec.substr(i, plus == std::string::npos ? plus : plus - i);
        for (char &c : tok) c = (char) std::tolower((unsigned char) c);
        if (plus != std::string::npos) {
            if (tok == "shift") mods |= GLFW_MOD_SHIFT;
            else if (tok == "ctrl" || tok == "cmd" || tok == "super") mods |= SYSTEM_COMMAND_MOD;
            else if (tok == "alt") mods |= GLFW_MOD_ALT;
            else return false;
            i = plus + 1;
            continue;
        }
        if (tok.size() == 1 && tok[0] >= 'a' && tok[0] <= 'z') {
            key = GLFW_KEY_A + (tok[0] - 'a');
            return true;
        }
        for (const auto &n : named)
            if (tok == n.first) {
                key = n.second;
                return true;
            }
        return false;
    }
}

int main(int argc, char **argv) {
    std::string shot, pdf, open_path, place_path, dialog;
    bool print_title = false;
    double zoom = 0;
    float at_x = -1, at_y = -1, sel_a = -1, sel_b = -1;
    bool baselines = false, loose = false, no_kern = false, no_snap = false;
    bool caption_pictures = false, fit_columns = false;
    int blank_pages = 0, goto_page = 0;
    std::string page_view;
    std::vector<Action> actions;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        Action act;
        if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--export-pdf" && i + 1 < argc) pdf = argv[++i];
        else if (a == "--open" && i + 1 < argc) open_path = argv[++i];
        else if (a == "--place" && i + 1 < argc) place_path = argv[++i];
        else if (a == "--print-title") print_title = true;
        else if (a == "--dialog" && i + 1 < argc) dialog = argv[++i];
        else if (a == "--blank-pages" && i + 1 < argc) blank_pages = std::atoi(argv[++i]);
        else if (a == "--page-view" && i + 1 < argc) page_view = argv[++i];
        else if (a == "--page" && i + 1 < argc) goto_page = std::atoi(argv[++i]);
        else if ((a == "--save" || a == "--save-embedded") && i + 1 < argc) {
            act.kind = a == "--save" ? Action::Save : Action::SaveEmbedded;
            act.text = argv[++i];
            actions.push_back(act);
        }
        else if (a == "--drag" && i + 1 < argc &&
                 std::sscanf(argv[i + 1], "%f,%f:%f,%f", &act.d[0], &act.d[1], &act.d[2], &act.d[3]) == 4) {
            act.kind = Action::Drag;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--click" && i + 1 < argc && parse_pair(argv[i + 1], act.d[0], act.d[1])) {
            act.kind = Action::Click;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--ui-click" && i + 1 < argc && parse_pair(argv[i + 1], act.d[0], act.d[1])) {
            act.kind = Action::UiClick;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--ui-rclick" && i + 1 < argc && parse_pair(argv[i + 1], act.d[0], act.d[1])) {
            act.kind = Action::UiRightClick;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--ui-drag" && i + 1 < argc &&
                 std::sscanf(argv[i + 1], "%f,%f:%f,%f", &act.d[0], &act.d[1], &act.d[2], &act.d[3]) == 4) {
            act.kind = Action::UiDrag;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--ui-scroll" && i + 1 < argc &&
                 std::sscanf(argv[i + 1], "%f,%f:%f", &act.d[0], &act.d[1], &act.d[2]) == 3) {
            act.kind = Action::UiScroll;
            actions.push_back(act);
            ++i;
        }
        else if (a == "--type" && i + 1 < argc) {
            act.kind = Action::Type;
            act.text = argv[++i];
            actions.push_back(act);
        }
        else if (a == "--key" && i + 1 < argc && parse_key(argv[i + 1], act.key, act.mods)) {
            act.kind = Action::Key;
            actions.push_back(act);
            ++i;
        }
        else if ((a == "--tool" || a == "--fill" || a == "--stroke") && i + 1 < argc) {
            act.kind = a == "--tool" ? Action::Tool : a == "--fill" ? Action::Fill : Action::Stroke;
            act.text = argv[++i];
            actions.push_back(act);
        }
        else if (a == "--zoom" && i + 1 < argc) zoom = std::atof(argv[++i]);
        else if (a == "--at" && i + 1 < argc) parse_pair(argv[++i], at_x, at_y);
        else if (a == "--select" && i + 1 < argc) parse_pair(argv[++i], sel_a, sel_b);
        else if (a == "--baselines") baselines = true;
        else if (a == "--loose") loose = true;
        else if (a == "--no-kerning") no_kern = true;
        else if (a == "--no-snap") no_snap = true;
        else if (a == "--caption-pictures") caption_pictures = true;
        else if (a == "--fit-columns") fit_columns = true;
    }

    nanogui::init();
    {
        ref<PagemadeApp> app = new PagemadeApp();
        app->dec_ref();
        app->set_visible(true);
        for (int k = 0; k < 3; ++k) { app->perform_layout(); app->draw_all(); }
        if (!open_path.empty() && !app->open_path(open_path))
            std::printf("open FAILED %s\n", open_path.c_str());
        if (caption_pictures)
            app->view()->set_caption_pictures(true);
        if (fit_columns)
            app->view()->set_fit_to_columns(true);
        if (!place_path.empty()) {
            std::string error;
            if (app->view()->place_file(place_path, &error))
                std::printf("place ok\n");
            else
                std::printf("place FAILED %s\n", error.c_str());
        }

        if (goto_page > 0 && (size_t) goto_page <= app->view()->page_count())
            app->view()->show_page((size_t) goto_page - 1);
        if (blank_pages > 1) {
            while ((int) app->view()->page_count() < blank_pages)
                app->view()->insert_page(true);
            app->view()->show_page(0);
        }
        if (page_view == "spread")
            app->view()->set_page_view(PageView::Spread);
        else if (page_view == "stack" || page_view == "continuous")
            app->view()->set_page_view(PageView::Stack);
        if (baselines) { app->baselines_box()->set_pushed(true); app->view()->set_show_baselines(true); }
        if (loose)     { app->loose_box()->set_pushed(true); app->view()->set_show_loose_tight(true); }
        if (no_kern)   { app->kern_box()->set_pushed(false);
                         app->for_each_run([](CharStyle &cs) { cs.kerning = false; }); }
        if (sel_a >= 0) app->view()->select_frame((size_t) sel_a, (size_t) sel_b);
        if (zoom > 0) {
            app->zoom_to(zoom);
            if (at_x >= 0) app->center_on(Vector2f(at_x, at_y));
        } else {
            app->fit_page();
        }
        if (no_snap)
            app->set_snap(false);

        for (const Action &act : actions) {
            nanogui::process_async();    // as the main loop does between events
            app->do_widget_cleanup();
            app->perform_layout();
            app->draw_all();
            switch (act.kind) {
            case Action::Drag:
                app->synthetic_drag(Vector2f(act.d[0], act.d[1]), Vector2f(act.d[2], act.d[3]));
                break;
            case Action::Click:
                app->synthetic_click(Vector2f(act.d[0], act.d[1]));
                break;
            case Action::UiClick:
                app->cursor_pos_callback_event(act.d[0], act.d[1]);
                app->mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0);
                app->mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0);
                break;
            case Action::UiRightClick:
                app->cursor_pos_callback_event(act.d[0], act.d[1]);
                app->mouse_button_callback_event(GLFW_MOUSE_BUTTON_2, GLFW_PRESS, 0);
                app->mouse_button_callback_event(GLFW_MOUSE_BUTTON_2, GLFW_RELEASE, 0);
                break;
            case Action::UiDrag: {
                /* Press, move in steps (a window resize follows each), release */
                app->cursor_pos_callback_event(act.d[0], act.d[1]);
                app->mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_PRESS, 0);
                const int steps = 10;
                for (int k = 1; k <= steps; ++k) {
                    const double t = (double) k / steps;
                    app->cursor_pos_callback_event(act.d[0] + (act.d[2] - act.d[0]) * t,
                                                   act.d[1] + (act.d[3] - act.d[1]) * t);
                }
                app->mouse_button_callback_event(GLFW_MOUSE_BUTTON_1, GLFW_RELEASE, 0);
                break;
            }
            case Action::UiScroll:
                app->cursor_pos_callback_event(act.d[0], act.d[1]);
                app->scroll_callback_event(0, act.d[2]);
                break;
            case Action::Type:
                app->synthetic_type(act.text);
                break;
            case Action::Key:
                app->synthetic_key(act.key, act.mods);
                break;
            case Action::Tool:
            case Action::Fill:
            case Action::Stroke:
                app->scripted(act.kind == Action::Tool ? "tool" : act.kind == Action::Fill
                              ? "fill" : "stroke", act.text);
                break;
            case Action::Save:
            case Action::SaveEmbedded:
                std::printf("save %s %s\n",
                            app->write_to(act.text, act.kind == Action::SaveEmbedded) ? "ok" : "FAILED",
                            act.text.c_str());
                break;
            }
        }

        if (dialog == "save")
            app->save_as();
        else if (dialog == "changes")
            ask_save_changes(app, app->doc_name(), [](SaveChoice) {});
        else if (dialog == "new")
            app->new_document();

        if (print_title)
            std::printf("title %s\n", app->caption().c_str());
        if (!pdf.empty())
            std::printf("pdf %s %s\n", app->export_pdf_to(pdf) ? "saved" : "FAILED",
                        pdf.c_str());

        if (!shot.empty()) {
            nanogui::process_async();    // what the last action queued
            for (int k = 0; k < 2; ++k) { app->perform_layout(); app->draw_all(); }
            bool ok = app->save_png(shot);
            std::printf("screenshot %s %s\n", ok ? "saved" : "FAILED", shot.c_str());
        } else {
            nanogui::mainloop(-1);
        }
    }
    nanogui::shutdown();
    return 0;
}
