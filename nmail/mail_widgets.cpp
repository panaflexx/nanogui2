/*
 * nmail/mail_widgets.cpp — implementation of mail_widgets.h.
 */

#include "mail_widgets.h"

#include <nanogui/opengl.h>
#include <nanogui/layout.h>
#include <nanogui/icons.h>
#include <nanogui/screen.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>

using namespace nanogui;

// Currently selected item (app-wide)
static FolderItem *g_selected_item = nullptr;

// ---------------------------------------------------------------------------
// FolderChildrenContainer / FolderContainer
// ---------------------------------------------------------------------------
Vector2i FolderChildrenContainer::preferred_size(NVGcontext *ctx) const {
    int total_h = 0;
    for (auto *child : m_children) {
        if (child->visible())
            total_h += child->preferred_size(ctx).y();
    }
    return Vector2i(m_parent ? m_parent->size().x() : 100, total_h);
}

void FolderContainer::perform_layout(NVGcontext *ctx) {
    Vector2i ps = preferred_size(ctx);
    if (ps.y() > m_size.y())
        m_size.y() = ps.y();
    Widget::perform_layout(ctx);
}

// ---------------------------------------------------------------------------
// FolderItem
// ---------------------------------------------------------------------------
FolderItem::FolderItem(Widget *parent, const std::string &caption, int icon,
                       int indent, int badge, bool expandable)
    : Widget(parent),
      m_caption(caption), m_icon(icon), m_indent(indent),
      m_badge(badge), m_expandable(expandable),
      m_expanded(false), m_hovered(false),
      m_children_container(nullptr),
      m_select_callback(nullptr)
{
    set_cursor(Cursor::Hand);
    int row_h = (int)(font_size() * 1.8f);
    set_min_height(row_h);
    set_height(row_h);
}

bool FolderItem::selected() const { return g_selected_item == this; }

Widget *FolderItem::ensure_children_container() {
    if (!m_children_container) {
        // The container is added as a sibling right after this item
        // in the parent's child list.  It will be animated.
        m_children_container = new FolderChildrenContainer(m_parent);
        m_children_container->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));
        m_children_container->set_visible(false);
    }
    return m_children_container;
}

bool FolderItem::mouse_enter_event(const Vector2i &p, bool enter) {
    m_hovered = enter;
    return Widget::mouse_enter_event(p, enter);
}

bool FolderItem::mouse_button_event(const Vector2i &p, int button,
                                    bool down, int modifiers) {
    if (button == GLFW_MOUSE_BUTTON_1 && down) {
        if (m_expandable && m_children_container) {
            toggle_expand();
        }
        // Select this item
        g_selected_item = this;
        if (m_select_callback)
            m_select_callback(this);
        return true;
    }
    return Widget::mouse_button_event(p, button, down, modifiers);
}

void FolderItem::toggle_expand() {
    if (!m_children_container) return;
    m_expanded = !m_expanded;
    m_children_container->set_animation_duration(0.4f);
    if (m_expanded) {
        m_children_container->set_visible(true);
        m_children_container->start_animation(
            Widget::AnimationType::SlideDown);
    } else {
        m_children_container->start_animation(
            Widget::AnimationType::SlideUp);
    }
    screen()->redraw();
}

void FolderItem::draw(NVGcontext *ctx) {
    float fs = (float)font_size();
    float x = (float)m_pos.x();
    float y = (float)m_pos.y();
    float w = (float)m_size.x();
    float h = (float)m_size.y();

    float base_x = fs * 1.5f + m_indent * fs * 1.25f;
    float rounding = 6.0f;

    if (selected()) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 4, y + 1, w - 8, h - 2, rounding);
        nvgFillColor(ctx, Color(0, 102, 255, 255));
        nvgFill(ctx);
    } else if (m_hovered) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 4, y + 1, w - 8, h - 2, rounding);
        bool dark = screen() && screen()->theme_mode() == ThemeMode::Dark;
        nvgFillColor(ctx, dark ? Color(255, 255, 255, 25)
                               : Color(0, 0, 0, 20));
        nvgFill(ctx);
    }

    Color text_col = selected() ? Color(255, 255, 255, 255)
                                : m_theme->m_text_color;
    Color icon_col = selected() ? Color(255, 255, 255, 255)
                                : m_theme->m_icon_color;

    if (m_expandable) {
        float tx = x + base_x - fs * 1.0f;
        float ty = y + h * 0.5f;
        float angle = m_expanded ? NVG_PI * 0.5f : 0.0f;
        nvgSave(ctx);
        nvgTranslate(ctx, tx, ty);
        nvgRotate(ctx, angle);
        nvgFontSize(ctx, fs * 0.75f);
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, m_theme->m_disabled_text_color);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, 0, 0, utf8(FA_CARET_RIGHT).data(), nullptr);
        nvgRestore(ctx);
    }

    float icon_x = x + base_x;
    float icon_y = y + h * 0.5f;
    nvgFontSize(ctx, fs * 0.95f);
    nvgFontFace(ctx, "icons");
    nvgFillColor(ctx, icon_col);
    nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(ctx, icon_x, icon_y, utf8(m_icon).data(), nullptr);

    float text_x = icon_x + fs * 1.1f;
    nvgFontSize(ctx, fs);
    nvgFontFace(ctx, "sans");
    nvgFillColor(ctx, text_col);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    {
        // Clip long folder names so they don't run under the badge
        nvgSave(ctx);
        float badge_rsv = (m_badge > 0) ? fs * 3.2f : fs * 1.2f;
        nvgIntersectScissor(ctx, text_x, y, w - (text_x - x) - badge_rsv, h);
        nvgText(ctx, text_x, y + h * 0.5f, m_caption.c_str(), nullptr);
        nvgRestore(ctx);
    }

    if (m_badge > 0) {
        std::string badge_text = std::to_string(m_badge);
        float badge_fs = fs * 0.7f;
        float bh = badge_fs * 1.6f;
        nvgFontSize(ctx, badge_fs);
        nvgFontFace(ctx, "sans-bold");
        float bounds[4];
        nvgTextBounds(ctx, 0, 0, badge_text.c_str(), nullptr, bounds);
        float bw = (bounds[2] - bounds[0]) + badge_fs * 0.9f;
        if (bw < bh) bw = bh;

        float bx = x + w - bw - fs * 1.5f;
        float by = y + (h - bh) * 0.5f;

        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, bx, by, bw, bh, bh * 0.5f);
        nvgFillColor(ctx, selected() ? Color(255, 255, 255, 200)
                                     : Color(180, 185, 195, 255));
        nvgFill(ctx);

        nvgFillColor(ctx, selected() ? Color(0, 102, 255, 255)
                                     : Color(255, 255, 255, 255));
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, bx + bw * 0.5f, by + bh * 0.5f,
                badge_text.c_str(), nullptr);
    }

    Widget::draw(ctx);
}

// ---------------------------------------------------------------------------
// SectionHeader
// ---------------------------------------------------------------------------
SectionHeader::SectionHeader(Widget *parent, const std::string &title)
    : Widget(parent), m_title(title) {
    int row_h = (int)(font_size() * 1.8f);
    set_min_height(row_h);
    set_height(row_h);
}

void SectionHeader::draw(NVGcontext *ctx) {
    float fs = (float)font_size();
    float x = (float)m_pos.x();
    float y = (float)m_pos.y();
    float h = (float)m_size.y();

    nvgFontSize(ctx, fs);
    nvgFontFace(ctx, "sans-bold");
    nvgFillColor(ctx, m_theme->m_disabled_text_color);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(ctx, x + fs * 0.9f, y + h * 0.5f + 2.0f,
            m_title.c_str(), nullptr);

    Widget::draw(ctx);
}

// ---------------------------------------------------------------------------
// FolderView
// ---------------------------------------------------------------------------
FolderView::FolderView(Widget *parent, std::function<void(FolderItem *)> on_select)
    : Widget(parent), m_on_select(on_select)
{
    set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

    m_scroll = new ScrollPanel(this);
    m_scroll->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
    m_scroll->set_grow_parent(true);

    m_container = new FolderContainer(m_scroll);
    m_container->set_layout(
        new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 0));
}

void FolderView::draw(NVGcontext *ctx) {
    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgFillColor(ctx, m_theme->m_window_fill_unfocused);
    nvgFill(ctx);

    Widget::draw(ctx);
}

void FolderView::rebuild(const std::string &account,
                         const std::vector<MailFolder> &folders,
                         const std::string &selected_name) {
    g_selected_item = nullptr;
    while (!m_container->children().empty())
        m_container->remove_child_at(0);
    m_scroll->set_scroll(0.0f);

    auto *top_spacer = new Widget(m_container);
    top_spacer->set_min_height(6);
    top_spacer->set_height(6);

    new SectionHeader(m_container, account);

    for (const MailFolder &f : folders) {
        auto *item = new FolderItem(m_container, display_name(f.name),
                                    folder_icon(f.name), 0, f.unseen);
        item->set_select_callback(m_on_select);
        item->set_tooltip(f.name);
        if (!selected_name.empty() && f.name == selected_name)
            g_selected_item = item;
    }

    auto *bot_spacer = new Widget(m_container);
    bot_spacer->set_min_height(20);
    bot_spacer->set_height(20);

    screen()->perform_layout();
}

std::string FolderView::display_name(const std::string &name) {
    size_t p = name.find_last_of("/.");
    return p == std::string::npos ? name : name.substr(p + 1);
}

int FolderView::folder_icon(const std::string &name) {
    std::string leaf;
    size_t p = name.find_last_of("/.");
    leaf = (p == std::string::npos) ? name : name.substr(p + 1);
    for (char &c : leaf) c = (char)std::tolower((unsigned char)c);

    if (leaf == "inbox")                                 return FA_INBOX;
    if (leaf.find("sent") != std::string::npos)          return FA_PAPER_PLANE;
    if (leaf.find("draft") != std::string::npos)         return FA_EDIT;
    if (leaf.find("junk") != std::string::npos ||
        leaf.find("spam") != std::string::npos)          return FA_MAIL_BULK;
    if (leaf.find("trash") != std::string::npos ||
        leaf.find("deleted") != std::string::npos ||
        leaf.find("bin") != std::string::npos)           return FA_TRASH;
    if (leaf.find("archive") != std::string::npos)       return FA_ARCHIVE;
    if (leaf.find("star") != std::string::npos ||
        leaf.find("flag") != std::string::npos)          return FA_STAR;
    return FA_FOLDER;
}

// ---------------------------------------------------------------------------
// EmailListView
// ---------------------------------------------------------------------------
bool EmailListView::mouse_motion_event(const Vector2i &p, const Vector2i &,
                                       int, int) {
    int idx = idx_at(p.y());
    if (idx != m_hovered) { m_hovered = idx; screen()->redraw(); }
    return false;
}

bool EmailListView::mouse_enter_event(const Vector2i &p, bool enter) {
    if (!enter) { m_hovered = -1; screen()->redraw(); }
    return Widget::mouse_enter_event(p, enter);
}

bool EmailListView::mouse_button_event(const Vector2i &p, int button,
                                       bool down, int mods) {
    if (button != GLFW_MOUSE_BUTTON_1) return false;
    if (down) {
        request_focus();   // grab keyboard focus on any click
        // Scrollbar thumb hit-test
        auto t = thumb_rect();
        if ((float)p.x() >= t[0] && (float)p.x() <= t[0] + t[2] &&
            (float)p.y() >= t[1] && (float)p.y() <= t[1] + t[3]) {
            m_vel            = 0.0f;   // kill inertia while dragging bar
            m_sb_drag        = true;
            m_sb_drag_start  = (float)p.y();
            m_sb_drag_origin = m_scroll;
            return true;
        }
        // Row hit-test
        int idx = idx_at(p.y());
        if (idx >= 0 && idx < (int)m_emails.size()) {
            m_selected = idx;
            if (m_on_select) m_on_select(idx, m_emails[idx]);
            screen()->redraw();
            return true;
        }
    } else {
        if (m_sb_drag) { m_sb_drag = false; screen()->redraw(); return true; }
    }
    return Widget::mouse_button_event(p, button, down, mods);
}

bool EmailListView::mouse_drag_event(const Vector2i &p, const Vector2i &,
                                     int, int) {
    if (!m_sb_drag) return false;
    float track = (float)m_size.y() - thumb_h();
    float delta = ((float)p.y() - m_sb_drag_start) /
                  (track > 0.0f ? track : 1.0f);
    m_scroll = std::clamp(m_sb_drag_origin + delta * max_scroll(),
                          0.0f, max_scroll());
    m_vel = 0.0f;
    screen()->redraw();
    notify_viewport();
    return true;
}

bool EmailListView::scroll_event(const Vector2i &, const Vector2f &rel) {
    m_vel = std::clamp(m_vel - rel.y() * row_h() * 4.0f, -3500.0f, 3500.0f);
    screen()->redraw();
    notify_viewport();
    return true;
}

bool EmailListView::keyboard_event(int key, int scancode,
                                   int action, int modifiers) {
    (void)scancode; (void)modifiers;
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return false;
    const int n = (int)m_emails.size();
    if (n == 0) return false;

    if (key == GLFW_KEY_DOWN || key == GLFW_KEY_UP) {
        int next = m_selected + (key == GLFW_KEY_DOWN ? 1 : -1);
        next = std::clamp(next, 0, n - 1);
        if (next != m_selected) {
            m_selected = next;
            scroll_to_show(m_selected);
            if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
            screen()->redraw();
        }
        notify_viewport();
        return true;
    }
    if (key == GLFW_KEY_PAGE_DOWN || key == GLFW_KEY_PAGE_UP) {
        float page = (float)m_size.y();
        m_vel = (key == GLFW_KEY_PAGE_DOWN ? 1.0f : -1.0f) * page * 6.0f;
        screen()->redraw();
        notify_viewport();
        return true;
    }
    if (key == GLFW_KEY_HOME) {
        m_scroll = 0.0f;  m_vel = 0.0f;
        screen()->redraw(); notify_viewport(); return true;
    }
    if (key == GLFW_KEY_END) {
        m_scroll = max_scroll();  m_vel = 0.0f;
        screen()->redraw(); notify_viewport(); return true;
    }
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        if (m_selected >= 0 && m_selected < n)
            if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
        return true;
    }
    return false;
}

void EmailListView::draw(NVGcontext *ctx) {
    // ---- Inertia integration ----
    {
        double now = glfwGetTime();
        float  dt  = (m_last_t > 0.0)
                     ? std::min((float)(now - m_last_t), 0.05f)
                     : 0.0f;
        m_last_t = now;

        if (std::abs(m_vel) > 0.5f) {
            m_scroll += m_vel * dt;
            m_scroll  = std::clamp(m_scroll, 0.0f, max_scroll());
            // Kill velocity at boundaries so we don't jitter
            if (m_scroll <= 0.0f || m_scroll >= max_scroll())
                m_vel = 0.0f;
            else {
                m_vel *= std::exp(-8.0f * dt);   // decay; tau ≈ 125 ms
                if (std::abs(m_vel) < 0.5f) m_vel = 0.0f;
            }
            screen()->redraw();   // keep animating until vel dies
        }
    }

    // Background
    nvgBeginPath(ctx);
    nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
    nvgFillColor(ctx, m_dark ? Color(30, 31, 38, 255)
                             : Color(228, 230, 238, 255));
    nvgFill(ctx);

    if (m_emails.empty()) {
        draw_scrollbar(ctx);
        return;
    }

    const float rh    = row_h();
    const int   first = std::max(0, (int)(m_scroll / rh));
    const int   last  = std::min((int)m_emails.size(),
                         (int)((m_scroll + (float)m_size.y()) / rh) + 2);

    // Clip rows to widget bounds
    nvgSave(ctx);
    nvgIntersectScissor(ctx, (float)m_pos.x(), (float)m_pos.y(),
                        (float)m_size.x(), (float)m_size.y());
    for (int i = first; i < last; ++i)
        draw_row(ctx, i,
                 (float)m_pos.x(),
                 (float)m_pos.y() + (float)i * rh - m_scroll,
                 (float)m_size.x());
    nvgRestore(ctx);

    if (m_loading_more) {
        draw_loading_strip(ctx);
        screen()->redraw();   // keep the spinner animating
    }

    // Notify when the bottom is reached (drives "load older" paging).
    // Skip while a page is already in flight, and never treat an empty
    // list as "at the bottom" — that used to queue FetchOlder for the
    // *previous* folder during a switch.
    if (m_on_hit_bottom && !m_loading_more &&
        m_scroll >= max_scroll() - 2.0f)
        m_on_hit_bottom();

    draw_scrollbar(ctx);  // drawn on top, no clip
}

void EmailListView::update_preview(int seq, const std::string &preview) {
    for (auto &e : m_emails) {
        if (e.seq == seq && e.preview != preview) {
            e.preview = preview;
            if (screen()) screen()->redraw();
            break;
        }
    }
}

void EmailListView::update_preview_by_uid(uint32_t uid, const std::string &preview) {
    if (uid == 0) return;
    for (auto &e : m_emails) {
        if (e.uid == uid && e.preview != preview) {
            e.preview = preview;
            if (screen()) screen()->redraw();
            break;
        }
    }
}

void EmailListView::set_emails(std::vector<EmailData> emails) {
    m_emails   = std::move(emails);
    m_selected = -1;
    m_hovered  = -1;
    m_scroll   = 0.0f;
    m_vel      = 0.0f;
    screen()->redraw();
}

void EmailListView::append_emails(std::vector<EmailData> more) {
    m_emails.insert(m_emails.end(),
                    std::make_move_iterator(more.begin()),
                    std::make_move_iterator(more.end()));
    screen()->redraw();
}

void EmailListView::prepend_emails(std::vector<EmailData> newer) {
    if (newer.empty()) return;
    int prev_selected_seq = selected_seq();
    float inserted_h = (float)newer.size() * row_h();
    m_emails.insert(m_emails.begin(),
                    std::make_move_iterator(newer.begin()),
                    std::make_move_iterator(newer.end()));
    m_scroll = std::clamp(m_scroll + inserted_h, 0.0f, max_scroll());
    if (prev_selected_seq >= 0) {
        m_selected = -1;
        for (int i = 0; i < (int)m_emails.size(); ++i)
            if (m_emails[i].seq == prev_selected_seq) { m_selected = i; break; }
    }
    m_hovered = -1;
    if (screen()) screen()->redraw();
}

void EmailListView::set_loading_more(bool v) {
    if (m_loading_more == v) return;
    m_loading_more = v;
    screen()->redraw();
}

void EmailListView::set_dark(bool dark) { m_dark = dark; screen()->redraw(); }

void EmailListView::clear_selection() { m_selected = -1; m_hovered = -1; if (screen()) screen()->redraw(); }

bool EmailListView::remove_seq(int seq) {
    for (int i = 0; i < (int)m_emails.size(); ++i) {
        if (m_emails[i].seq != seq) continue;
        m_emails.erase(m_emails.begin() + i);
        for (auto &e : m_emails)
            if (e.seq > seq) --e.seq;
        if (m_selected == i) {
            if (i < (int)m_emails.size())
                m_selected = i;
            else
                m_selected = (int)m_emails.size() - 1;
            m_hovered = -1;
        } else if (m_selected > i) {
            --m_selected;
            if (m_hovered > i) --m_hovered;
        } else if (m_hovered > i) {
            --m_hovered;
        }
        m_scroll = std::clamp(m_scroll, 0.0f, max_scroll());
        if (screen()) screen()->redraw();
        return true;
    }
    return false;
}

bool EmailListView::remove_by_uid(uint32_t uid) {
    if (uid == 0) return false;
    for (int i = 0; i < (int)m_emails.size(); ++i) {
        if (m_emails[i].uid != uid) continue;
        m_emails.erase(m_emails.begin() + i);
        // UIDs are stable (QRESYNC) — no shifting of remaining uids/seqs.
        if (m_selected == i) {
            if (i < (int)m_emails.size())
                m_selected = i;
            else
                m_selected = (int)m_emails.size() - 1;
            m_hovered = -1;
        } else if (m_selected > i) {
            --m_selected;
            if (m_hovered > i) --m_hovered;
        } else if (m_hovered > i) {
            --m_hovered;
        }
        m_scroll = std::clamp(m_scroll, 0.0f, max_scroll());
        if (screen()) screen()->redraw();
        return true;
    }
    return false;
}

float EmailListView::thumb_h() const {
    if (total_h() <= 0.0f) return (float)m_size.y();
    float ratio = std::min(1.0f, (float)m_size.y() / total_h());
    return std::max(28.0f, (float)m_size.y() * ratio);
}

std::array<float, 4> EmailListView::thumb_rect() const {
    float th  = thumb_h();
    float trk = (float)m_size.y() - th;
    float ty  = (max_scroll() > 0.0f)
                ? (m_scroll / max_scroll()) * trk : 0.0f;
    float tx  = (float)m_pos.x() + (float)m_size.x() - SB_W - SB_MARGIN;
    return { tx, (float)m_pos.y() + ty, SB_W, th };
}

void EmailListView::draw_scrollbar(NVGcontext *ctx) const {
    if (total_h() <= (float)m_size.y()) return;
    auto t = thumb_rect();
    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, t[0], t[1] + 3.0f, t[2], t[3] - 6.0f, t[2] * 0.5f);
    nvgFillColor(ctx, m_sb_drag
        ? (m_dark ? Color(140, 148, 165, 230) : Color(100, 110, 130, 230))
        : (m_dark ? Color( 95, 100, 115, 180) : Color(150, 155, 165, 180)));
    nvgFill(ctx);
}

void EmailListView::draw_loading_strip(NVGcontext *ctx) {
    const float h = 34.0f;
    const float x = (float)m_pos.x();
    const float w = (float)m_size.x();
    const float y = (float)m_pos.y() + (float)m_size.y() - h;

    nvgSave(ctx);
    nvgIntersectScissor(ctx, x, y, w, h);
    nvgBeginPath(ctx);
    nvgRect(ctx, x, y, w, h);
    nvgFillColor(ctx, m_dark ? Color( 30,  31,  38, 235)
                             : Color(228, 230, 238, 235));
    nvgFill(ctx);

    const Color fg = m_dark ? Color(180, 182, 196, 255)
                            : Color( 90,  90, 105, 255);

    // Spinner arc
    const float r  = 8.0f;
    const float cy = y + h * 0.5f;
    float tb[4] = {};
    nvgFontSize(ctx, 14.0f);
    nvgFontFace(ctx, "sans");
    nvgTextBounds(ctx, 0, 0, "Loading older messages...", nullptr, tb);
    const float text_w = tb[2] - tb[0];
    const float cx = x + (w - text_w - r * 2.0f - 10.0f) * 0.5f;
    const float a0 = (float)glfwGetTime() * 6.0f;
    nvgBeginPath(ctx);
    nvgArc(ctx, cx, cy, r, a0, a0 + NVG_PI * 1.5f, NVG_CW);
    nvgStrokeColor(ctx, fg);
    nvgStrokeWidth(ctx, 2.5f);
    nvgStroke(ctx);

    nvgFillColor(ctx, fg);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(ctx, cx + r + 10.0f, cy, "Loading older messages...", nullptr);
    nvgRestore(ctx);
}

int EmailListView::idx_at(int abs_y) const {
    float iy  = (float)(abs_y - m_pos.y()) + m_scroll;
    int   idx = (int)(iy / row_h());
    return (idx >= 0 && idx < (int)m_emails.size()) ? idx : -1;
}

void EmailListView::draw_row(NVGcontext *ctx, int idx,
                             float x, float y, float w) const {
    const bool  sel = (idx == m_selected);
    const bool  hov = (idx == m_hovered) && !sel;
    const auto &d   = m_emails[idx];

    const float fs         = (float)font_size();
    const float h          = row_h();
    const float padx       = 10.0f;
    const float pady       = 6.0f;
    const float scroll_rsv = SB_W + SB_MARGIN;   // rows touch the scrollbar
    const float cw         = w - scroll_rsv;
    const float rounding   = 10.0f;

    const float sender_fs  = fs * 0.85f;
    const float date_fs    = fs * 0.60f;
    const float subject_fs = fs * 0.72f;
    const float preview_fs = fs * 0.63f + 2.0f;
    const float line_gap   = fs * 0.12f;

    const float y1 = y + pady + sender_fs * 0.85f;
    const float y2 = y1 + sender_fs  + line_gap;
    const float y3 = y2 + subject_fs + line_gap;
    const float y4 = y3 + preview_fs * 1.10f;

    // Background
    if (sel) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 3, y + 2, cw - 3, h - 4, rounding);
        nvgFillColor(ctx, Color(58, 90, 210, 255));
        nvgFill(ctx);
    } else if (hov) {
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x + 3, y + 2, cw - 3, h - 4, rounding);
        nvgFillColor(ctx, m_dark ? Color( 58,  60,  72, 255)
                                 : Color(210, 214, 222, 255));
        nvgFill(ctx);
    }

    Color name_col = sel ? Color(255,255,255,255)
                   : m_dark ? (d.seen ? Color(232,232,238,255)
                                      : Color(110,160,255,255))
                            : (d.seen ? Color( 15, 15, 20,255)
                                      : Color( 10, 80,220,255)); // unread: accent
    Color date_col = sel ? Color(200,218,255,255)
                   : m_dark ? Color(145,147,160,255) : Color(120,120,135,255);
    Color subj_col = sel ? Color(220,232,255,255)
                   : m_dark ? Color(205,206,216,255) : Color( 35, 35, 45,255);
    Color prev_col = sel ? Color(185,208,255,255)
                   : m_dark ? Color(160,162,175,255) : Color( 95, 95,110,255);

    // Date (right-aligned)
    nvgFontSize(ctx, date_fs);
    nvgFontFace(ctx, "sans");
    nvgFillColor(ctx, date_col);
    nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgText(ctx, x + cw - padx, y1, d.date.c_str(), nullptr);

    float db[4] = {};
    nvgTextBounds(ctx, 0, 0, d.date.c_str(), nullptr, db);
    const float date_w = (db[2] - db[0]) + padx * 1.8f;

    /* Indicators (unread dot, future flags): right-aligned, starting
     * just left of the date and stacking leftward.  `ind_w` widens the
     * sender clip below so the name never runs under the dots. */
    float ind_w = 0.0f;
    {
        const float ind_fs = sender_fs * IND_FONT_SCALE;
        float ix = x + cw - padx - (db[2] - db[0]) - IND_GAP;
        nvgFontSize(ctx, ind_fs);
        nvgFontFace(ctx, "icons");
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        for (const Indicator &ind : kIndicators) {
            if (d.*(ind.flag) != ind.show_when) continue;
            nvgFillColor(ctx, ind.color);
            nvgText(ctx, ix, y1, ind.glyph, nullptr);
            float gb[4] = {};
            nvgTextBounds(ctx, 0, 0, ind.glyph, nullptr, gb);
            const float gw = (gb[2] - gb[0]) + IND_GAP;
            ix    -= gw;
            ind_w += gw;
        }
        if (ind_w > 0.0f) ind_w += IND_GAP;   // space before the sender
    }

    // Sender name (bold, clipped against date + indicators)
    nvgSave(ctx);
    nvgIntersectScissor(ctx, x + padx, y, cw - date_w - ind_w - padx, h);
    nvgFontSize(ctx, sender_fs);
    nvgFontFace(ctx, "sans-bold");
    nvgFillColor(ctx, name_col);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(ctx, x + padx, y1, d.sender.c_str(), nullptr);
    nvgRestore(ctx);

    // Subject (small bold, clipped)
    {
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x + padx, y, cw - padx - 3.0f, h);
        nvgFontSize(ctx, subject_fs);
        nvgFontFace(ctx, d.seen ? "sans" : "sans-bold");
        nvgFillColor(ctx, subj_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + padx, y2, d.subject.c_str(), nullptr);
        nvgRestore(ctx);
    }

    // Preview body — 2 lines, scissor-clipped
    nvgFontSize(ctx, preview_fs);
    nvgFontFace(ctx, "sans");
    nvgFillColor(ctx, prev_col);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    {
        float clip_h = (y4 - y3) + preview_fs * 1.35f;
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x + padx, y3 - preview_fs * 0.6f,
                            cw - padx - 3.0f, clip_h + preview_fs * 0.3f);
        nvgTextBox(ctx, x + padx, y3 - preview_fs * 0.55f,
                   cw - padx - 3.0f, d.preview.c_str(), nullptr);
        nvgRestore(ctx);
    }

    // Bottom separator (non-selected rows only)
    if (!sel) {
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, x + padx,      y + h - 0.5f);
        nvgLineTo(ctx, x + cw, y + h - 0.5f);
        nvgStrokeColor(ctx, m_dark ? Color( 62,  64,  76, 255)
                                   : Color(195, 198, 208, 255));
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
    }
}

void EmailListView::scroll_to_show(int idx) {
    const float rh  = row_h();
    const float top = (float)idx * rh;
    const float bot = top + rh;
    m_vel = 0.0f;
    if (top < m_scroll)
        m_scroll = top;
    else if (bot > m_scroll + (float)m_size.y())
        m_scroll = bot - (float)m_size.y();
    m_scroll = std::clamp(m_scroll, 0.0f, max_scroll());
}
