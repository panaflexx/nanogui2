/**
 * folderview.cpp — Juicy Mail-style folder sidebar demo
 *
 * Demonstrates a custom FolderView widget with:
 *   - Section headers (Favorites, Smart Mailboxes, iCloud)
 *   - Folder items with icons, labels, and unread badges
 *   - Expandable/collapsible folders with SlideUp/SlideDown animation
 *   - Selected-item highlighting with rounded rectangle
 *   - Hover highlighting
 */

#include "nanogui/widget.h"
#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/split.h>
#include <nanogui/layout.h>
#include <nanogui/icons.h>
#include <nanogui/texteditor.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <functional>
#include <vector>
#include <string>
#include <random>
#include <sstream>
#include <array>
#include <algorithm>

using namespace nanogui;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class FolderView;
class FolderSection;
class FolderItem;

// Currently selected item (app-wide)
static FolderItem *g_selected_item = nullptr;

class FolderChildrenContainer : public Widget {
public:
    FolderChildrenContainer(Widget *parent) : Widget(parent) {}

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        int total_h = 0;
        for (auto *child : m_children) {
            if (child->visible())
                total_h += child->preferred_size(ctx).y();
        }
        return Vector2i(m_parent ? m_parent->size().x() : 100, total_h);
    }
};

class FolderContainer : public Widget {
public:
    FolderContainer(Widget *parent) : Widget(parent) {}

    virtual void perform_layout(NVGcontext *ctx) override {
        Vector2i ps = preferred_size(ctx);
        if (ps.y() > m_size.y())
            m_size.y() = ps.y();
        Widget::perform_layout(ctx);
    }
};

// ---------------------------------------------------------------------------
// FolderItem — a single row in the sidebar
// ---------------------------------------------------------------------------
class FolderItem : public Widget {
public:
    FolderItem(Widget *parent, const std::string &caption, int icon,
               int indent = 0, int badge = 0, bool expandable = false)
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

    /* ---- Accessors ---- */
    const std::string &caption() const { return m_caption; }
    void set_caption(const std::string &c) { m_caption = c; }

    int badge() const { return m_badge; }
    void set_badge(int b) { m_badge = b; }

    bool selected() const { return g_selected_item == this; }

    bool expanded() const { return m_expanded; }

    void set_select_callback(std::function<void(FolderItem *)> cb) {
        m_select_callback = cb;
    }

    /* ---- Children container for expandable items ---- */
    Widget *children_container() const { return m_children_container; }

    Widget *ensure_children_container() {
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

    virtual Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    /* ---- Events ---- */
    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        m_hovered = enter;
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button,
                                    bool down, int modifiers) override {
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

    void toggle_expand() {
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

    /* ---- Drawing ---- */
    virtual void draw(NVGcontext *ctx) override {
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
            nvgFillColor(ctx, Color(0, 0, 0, 20));
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
        nvgText(ctx, text_x, y + h * 0.5f, m_caption.c_str(), nullptr);

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

private:
    std::string m_caption;
    int m_icon;
    int m_indent;
    int m_badge;
    bool m_expandable;
    bool m_expanded;
    bool m_hovered;
    Widget *m_children_container;
    std::function<void(FolderItem *)> m_select_callback;
};

// ---------------------------------------------------------------------------
// SectionHeader — a small gray header ("Favorites", "Smart Mailboxes", etc.)
// ---------------------------------------------------------------------------
class SectionHeader : public Widget {
public:
    SectionHeader(Widget *parent, const std::string &title)
        : Widget(parent), m_title(title) {
        int row_h = (int)(font_size() * 1.8f);
        set_min_height(row_h);
        set_height(row_h);
    }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    virtual void draw(NVGcontext *ctx) override {
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

private:
    std::string m_title;
};

// ---------------------------------------------------------------------------
// FolderView — the complete Juicy Mail-style sidebar widget
// ---------------------------------------------------------------------------
class FolderView : public Widget {
public:
    FolderView(Widget *parent, std::function<void(FolderItem *)> on_select)
        : Widget(parent), m_on_select(on_select)
    {
        set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        m_scroll = new ScrollPanel(this);
        m_scroll->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
        m_scroll->set_grow_parent(true);

        m_container = new FolderContainer(m_scroll);
        m_container->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 0));

        build_sidebar();
    }

    virtual void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, m_theme->m_window_fill_unfocused);
        nvgFill(ctx);

        Widget::draw(ctx);
    }

private:
    ScrollPanel *m_scroll;
    Widget *m_container;
    std::function<void(FolderItem *)> m_on_select;

    // Convenience: add a section header
    void add_section(const std::string &title) {
        new SectionHeader(m_container, title);
    }

    // Convenience: add a simple (non-expandable) item
    FolderItem *add_item(Widget *parent, const std::string &caption,
                         int icon, int indent = 0, int badge = 0) {
        auto *item = new FolderItem(parent, caption, icon, indent, badge);
        item->set_select_callback(m_on_select);
        return item;
    }

    FolderItem *add_expandable(const std::string &caption, int icon,
                               int indent = 0, int badge = 0) {
        auto *item = new FolderItem(m_container, caption, icon,
                                    indent, badge, /*expandable=*/true);
        item->set_select_callback(m_on_select);
        Widget *kids = item->ensure_children_container();
        kids->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));
        return item;
    }

    void finalize_expandable(FolderItem *item) {
        Widget *kids = item->children_container();
        if (!kids) return;
        int h = 0;
        for (auto *c : kids->children())
            h += c->height();
        kids->set_min_height(h);
        kids->set_height(h);
    }

    // Build the full Juicy Mail-style sidebar content
    void build_sidebar() {
        // Add a little top padding
        auto *top_spacer = new Widget(m_container);
        top_spacer->set_min_height(6);
        top_spacer->set_height(6);

        // ---- Favorites ----
        add_section("Favorites");

        auto *all_inboxes = add_expandable("All Inboxes", FA_INBOX, 0, 34);
        add_item(all_inboxes->children_container(), "iCloud", FA_ENVELOPE, 1, 21);
        add_item(all_inboxes->children_container(), "Google", FA_ENVELOPE, 1, 13);
        finalize_expandable(all_inboxes);

        add_item(m_container, "VIPs", FA_STAR, 0);

        auto *flagged = add_expandable("Flagged", FA_FLAG, 0, 3);
        add_item(flagged->children_container(), "iCloud", FA_FLAG, 1, 2);
        add_item(flagged->children_container(), "Google", FA_FLAG, 1, 1);
        finalize_expandable(flagged);

        add_item(m_container, "Remind Me", FA_CLOCK, 0);

        auto *all_drafts = add_expandable("All Drafts", FA_EDIT, 0);
        add_item(all_drafts->children_container(), "iCloud", FA_EDIT, 1);
        add_item(all_drafts->children_container(), "Google", FA_EDIT, 1);
        finalize_expandable(all_drafts);

        auto *all_sent = add_expandable("All Sent", FA_PAPER_PLANE, 0);
        add_item(all_sent->children_container(), "iCloud", FA_PAPER_PLANE, 1);
        add_item(all_sent->children_container(), "Google", FA_PAPER_PLANE, 1);
        finalize_expandable(all_sent);

        // ---- Smart Mailboxes ----
        add_section("Smart Mailboxes");
        add_item(m_container, "Today", FA_CALENDAR_DAY, 0, 5);
        add_item(m_container, "Family Pictures", FA_IMAGE, 0);

        // ---- iCloud Account ----
        add_section("iCloud");
        add_item(m_container, "Inbox", FA_INBOX, 0, 21);
        add_item(m_container, "Pets", FA_FOLDER, 0);
        add_item(m_container, "School", FA_FOLDER, 0);
        add_item(m_container, "Work", FA_FOLDER, 0);
        add_item(m_container, "Drafts", FA_EDIT, 0);
        add_item(m_container, "Sent", FA_PAPER_PLANE, 0);
        add_item(m_container, "Junk", FA_MAIL_BULK, 0);

        // ---- Google Account ----
        add_section("Google");
        add_item(m_container, "Inbox", FA_INBOX, 0, 13);
        add_item(m_container, "Starred", FA_STAR, 0);
        add_item(m_container, "Important", FA_EXCLAMATION_CIRCLE, 0, 2);
        add_item(m_container, "Sent Mail", FA_PAPER_PLANE, 0);
        add_item(m_container, "Drafts", FA_EDIT, 0);
        add_item(m_container, "Spam", FA_EXCLAMATION_TRIANGLE, 0);
        add_item(m_container, "Trash", FA_TRASH, 0);

        // Bottom spacer
        auto *bot_spacer = new Widget(m_container);
        bot_spacer->set_min_height(20);
        bot_spacer->set_height(20);
    }
};

// ---------------------------------------------------------------------------
// EmailData — plain struct describing one message in the list
// ---------------------------------------------------------------------------
struct EmailData {
    std::string sender;
    std::string subject;
    std::string preview;
    std::string date;
    bool        has_attachment = false;
};

// ---------------------------------------------------------------------------
// EmailListView — virtual-scroll list: O(visible) draw cost, no child widgets
// ---------------------------------------------------------------------------
class EmailListView : public Widget {
public:
    static constexpr float ROW_SCALE = 4.6f;
    static constexpr float SB_W      = 6.0f;
    static constexpr float SB_MARGIN = 3.0f;

    EmailListView(Widget *parent,
                  std::function<void(int, const EmailData &)> on_select = nullptr)
        : Widget(parent), m_on_select(std::move(on_select)) {}

    /* ---- geometry helpers ---- */
    float row_h()      const { return std::floor(font_size() * ROW_SCALE); }
    float total_h()    const { return row_h() * (float)m_emails.size(); }
    float max_scroll() const { return std::max(0.0f, total_h() - (float)m_size.y()); }

    /* ---- events ---- */
    virtual bool scroll_event(const Vector2i &, const Vector2f &rel) override {
        // Add a velocity impulse; clamp so rapid flicking can't go infinite
        m_vel = std::clamp(m_vel - rel.y() * row_h() * 4.0f, -3500.0f, 3500.0f);
        screen()->redraw();
        return true;
    }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &,
                                    int, int) override {
        int idx = idx_at(p.y());
        if (idx != m_hovered) { m_hovered = idx; screen()->redraw(); }
        return false;
    }

    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        if (!enter) { m_hovered = -1; screen()->redraw(); }
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button,
                                    bool down, int mods) override {
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

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &,
                                  int, int) override {
        if (!m_sb_drag) return false;
        float track = (float)m_size.y() - thumb_h();
        float delta = ((float)p.y() - m_sb_drag_start) /
                      (track > 0.0f ? track : 1.0f);
        m_scroll = std::clamp(m_sb_drag_origin + delta * max_scroll(),
                              0.0f, max_scroll());
        m_vel = 0.0f;
        screen()->redraw();
        return true;
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
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
            return true;
        }
        if (key == GLFW_KEY_PAGE_DOWN || key == GLFW_KEY_PAGE_UP) {
            float page = (float)m_size.y();
            m_vel = (key == GLFW_KEY_PAGE_DOWN ? 1.0f : -1.0f) * page * 6.0f;
            screen()->redraw();
            return true;
        }
        if (key == GLFW_KEY_HOME) {
            m_scroll = 0.0f;  m_vel = 0.0f;
            screen()->redraw();  return true;
        }
        if (key == GLFW_KEY_END) {
            m_scroll = max_scroll();  m_vel = 0.0f;
            screen()->redraw();  return true;
        }
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            if (m_selected >= 0 && m_selected < n)
                if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
            return true;
        }
        return false;
    }

    /* ---- draw (also drives the inertia animation) ---- */
    virtual void draw(NVGcontext *ctx) override {
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
        nvgFillColor(ctx, Color(228, 230, 238, 255));
        nvgFill(ctx);

        if (m_emails.empty()) return;

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

        draw_scrollbar(ctx);  // drawn on top, no clip
    }

    /* ---- data ---- */
    void add_email(const EmailData &d) { m_emails.push_back(d); }

    // Generate `count` random emails with lorem-ipsum bodies
    void build_random_emails(int count = 1000) {
        static const char *s_first[] = {
            "James","Mary","John","Patricia","Robert","Jennifer","Michael",
            "Linda","William","Barbara","David","Elizabeth","Richard",
            "Susan","Joseph","Jessica","Thomas","Sarah","Charles","Karen",
            "Christopher","Lisa","Daniel","Nancy","Matthew","Betty",
            "Anthony","Margaret","Mark","Sandra","Donald","Ashley",
            "Steven","Dorothy","Paul","Kimberly","Andrew","Emily",
            "Kenneth","Donna","Joshua","Michelle","Kevin","Carol",
            "Guillermo","Sophie","Elisha","Priya","Kofi","Yuki",
            "Aisha","Luca","Fatima","Diego","Mei","Tariq","Ingrid",
            "Bao","Chiara","Ravi","Zoe","Hamid","Xiaomeng","Olga"
        };
        static const char *s_last[] = {
            "Smith","Johnson","Williams","Brown","Jones","Garcia","Miller",
            "Davis","Wilson","Anderson","Taylor","Thomas","Jackson","White",
            "Harris","Martin","Thompson","Young","Robinson","Walker",
            "Hall","Allen","King","Wright","Scott","Green","Baker",
            "Adams","Nelson","Carter","Mitchell","Perez","Roberts","Turner",
            "Castillo","Nguyen","Patel","Kim","Chen","Wu","Murguia",
            "Santos","Okonkwo","Tanaka","Al-Rashid","Eriksson","Johansson",
            "Mueller","Rossi","Dubois","Fernandez","Nakamura","Singh",
            "St. Denis","Park","Gonzalez","Herrera","Yamamoto","Abboud"
        };
        static const char *s_subjects[] = {
            "Quick question","Following up","Re: Meeting notes","Invitation",
            "Important update","Check this out!","Weekend plans?","Hello!",
            "Action required","Invoice attached","Your order has shipped",
            "Team lunch on Friday","Project update","New assignment",
            "Reminder: deadline approaching","Catching up","Great news!",
            "Heads up","Re: Your request","Thanks for yesterday",
            "Can we reschedule?","Feedback needed","New photos",
            "Question about the proposal","See you soon!","Happy birthday!",
            "Out of office","Monthly digest","Security alert",
            "Your account","Subscription renewal","Event this weekend",
            "Collaboration opportunity","Checking in","Urgent: please read",
            "Re: Re: Plans","Introducing myself","Vacation recap",
            "Meeting tomorrow","Thoughts on the draft?","Park Photos",
            "Season finale","Nature Reserve Update","The best vacation"
        };
        static const char *s_words[] = {
            "lorem","ipsum","dolor","sit","amet","consectetur",
            "adipiscing","elit","sed","do","eiusmod","tempor",
            "incididunt","ut","labore","et","dolore","magna","aliqua",
            "enim","ad","minim","veniam","quis","nostrud","exercitation",
            "ullamco","laboris","nisi","aliquip","ex","ea","commodo",
            "consequat","duis","aute","irure","reprehenderit",
            "voluptate","velit","esse","cillum","fugiat","nulla",
            "pariatur","excepteur","sint","occaecat","cupidatat","non",
            "proident","sunt","culpa","qui","officia","deserunt","mollit",
            "anim","id","est","laborum","curabitur","pretium","tincidunt",
            "lacus","nunc","pulvinar","sapien","ligula","eget","semper",
            "augue","hendrerit","nisl","massa","volutpat","condimentum",
            "aliquam","blandit","viverra","maecenas","pellentesque",
            "porttitor","feugiat","vehicula","malesuada","faucibus"
        };

        std::mt19937 rng(42); // fixed seed — same list every run
        auto pick = [&](auto &arr) -> const char * {
            std::uniform_int_distribution<int> d(0, (int)std::size(arr) - 1);
            return arr[d(rng)];
        };
        std::uniform_int_distribution<int> wc_dist(18, 42);
        std::uniform_int_distribution<int> word_dist(0, (int)std::size(s_words) - 1);
        std::uniform_int_distribution<int> mon_dist(1, 12);
        std::uniform_int_distribution<int> day_dist(1, 28);
        std::uniform_int_distribution<int> yr_dist(22, 25);
        std::uniform_int_distribution<int> attach_dist(0, 4); // ~20 % chance

        for (int i = 0; i < count; ++i) {
            // Sender
            std::string sender = std::string(pick(s_first)) + " " + pick(s_last);

            // Subject
            std::string subject = pick(s_subjects);

            // Lorem-ipsum preview
            int wc = wc_dist(rng);
            std::string preview;
            preview.reserve(wc * 7);
            for (int w = 0; w < wc; ++w) {
                if (w > 0) preview += ' ';
                const char *word = s_words[word_dist(rng)];
                if (w == 0) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%c%s",
                                  (char)std::toupper((unsigned char)word[0]),
                                  word + 1);
                    preview += buf;
                } else {
                    preview += word;
                }
            }
            preview += '.';

            // Date  m/d/yy
            std::string date = std::to_string(mon_dist(rng)) + "/" +
                               std::to_string(day_dist(rng)) + "/" +
                               std::to_string(yr_dist(rng));

            add_email({sender, subject, preview, date,
                       attach_dist(rng) == 0});
        }
    }

    // Populate with the sample messages visible in the screenshot
    void build_sample_emails() {
        add_email({"Xiaomeng J.", "Park Photos",
            "Hi Danny, I took some great photos of the kids the other day. "
            "They got pretty goofy! xm",
            "4/2/25", false});
        add_email({"Elisha St. Denis", "Community Repair Fair!",
            "Hello everyone! You may have heard about our new program, but "
            "consider this your formal invite to come and join us.",
            "2/3/25", true});
        add_email({"Guillermo Castillo", "Send pics please!",
            "Hi Danny, I was thinking about that road trip we took a few years "
            "ago. I found these photos, and I remembered all your fun travel games :)",
            "1/31/25", true});
        add_email({"Anthony Wu", "Nature Reserve Update",
            "Danny! Welcome back! Hope you had the best time catching up with "
            "family and everyone. I kn...",
            "1/28/25", false});
        add_email({"Sophie Sun", "The best vacation",
            "Remember this outing? Nothing beats a day with friends. Here are "
            "two photos from our favorite spot.",
            "1/28/25", true});
        add_email({"Sarah Murguia", "Following up",
            "Dear Danny, I'm following up regarding the 4th quarter plan for "
            "our Go-to-Market strategy for the upcoming release.",
            "1/27/25", false});
        add_email({"Guillermo Castillo", "Season finale",
            "Did you see the final episode last night? I screamed at the TV "
            "at the last scene. I can't believe they did that!",
            "1/26/25", false});
        add_email({"Mom", "Holiday plans",
            "Hi sweetheart, just checking in about Thanksgiving. Are you coming "
            "home this year? Let us know so we can plan the menu!",
            "1/24/25", false});
        add_email({"GitHub", "Your pull request was merged",
            "Congratulations! Your pull request \"Fix rendering artifacts on "
            "Retina displays\" has been merged into main.",
            "1/22/25", false});
    }

private:
    /* ---- scrollbar geometry ---- */
    float thumb_h() const {
        if (total_h() <= 0.0f) return (float)m_size.y();
        float ratio = std::min(1.0f, (float)m_size.y() / total_h());
        return std::max(28.0f, (float)m_size.y() * ratio);
    }

    // Returns {abs_x, abs_y, w, h} of the scrollbar thumb
    std::array<float, 4> thumb_rect() const {
        float th  = thumb_h();
        float trk = (float)m_size.y() - th;
        float ty  = (max_scroll() > 0.0f)
                    ? (m_scroll / max_scroll()) * trk : 0.0f;
        float tx  = (float)m_pos.x() + (float)m_size.x() - SB_W - SB_MARGIN;
        return { tx, (float)m_pos.y() + ty, SB_W, th };
    }

    void draw_scrollbar(NVGcontext *ctx) const {
        if (total_h() <= (float)m_size.y()) return;
        auto t = thumb_rect();
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, t[0], t[1] + 3.0f, t[2], t[3] - 6.0f, t[2] * 0.5f);
        nvgFillColor(ctx, m_sb_drag ? Color(100, 110, 130, 230)
                                    : Color(150, 155, 165, 180));
        nvgFill(ctx);
    }

    int idx_at(int abs_y) const {
        float iy  = (float)(abs_y - m_pos.y()) + m_scroll;
        int   idx = (int)(iy / row_h());
        return (idx >= 0 && idx < (int)m_emails.size()) ? idx : -1;
    }

    /* ---- row drawing ---- */
    void draw_row(NVGcontext *ctx, int idx,
                  float x, float y, float w) const {
        const bool  sel = (idx == m_selected);
        const bool  hov = (idx == m_hovered) && !sel;
        const auto &d   = m_emails[idx];

        const float fs         = (float)font_size();
        const float h          = row_h();
        const float padx       = 10.0f;
        const float pady       = 6.0f;
        const float scroll_rsv = SB_W + SB_MARGIN * 2.0f + 3.0f;
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
            nvgRoundedRect(ctx, x + 3, y + 2, cw - 6, h - 4, rounding);
            nvgFillColor(ctx, Color(58, 90, 210, 255));
            nvgFill(ctx);
        } else if (hov) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 3, y + 2, cw - 6, h - 4, rounding);
            nvgFillColor(ctx, Color(210, 214, 222, 255));
            nvgFill(ctx);
        }

        Color name_col = sel ? Color(255,255,255,255) : Color( 15, 15, 20,255);
        Color date_col = sel ? Color(200,218,255,255) : Color(120,120,135,255);
        Color subj_col = sel ? Color(220,232,255,255) : Color( 35, 35, 45,255);
        Color prev_col = sel ? Color(185,208,255,255) : Color( 95, 95,110,255);

        // Date (right-aligned)
        nvgFontSize(ctx, date_fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, date_col);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + cw - padx, y1, d.date.c_str(), nullptr);

        float db[4] = {};
        nvgTextBounds(ctx, 0, 0, d.date.c_str(), nullptr, db);
        const float date_w = (db[2] - db[0]) + padx * 1.8f;

        // Sender name (bold, clipped against date)
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x + padx, y, cw - date_w - padx, h);
        nvgFontSize(ctx, sender_fs);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, name_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + padx, y1, d.sender.c_str(), nullptr);
        nvgRestore(ctx);

        // Attachment icon
        if (d.has_attachment) {
            nvgFontSize(ctx, subject_fs * 0.9f);
            nvgFontFace(ctx, "icons");
            nvgFillColor(ctx, date_col);
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, x + cw - padx, y2, utf8(FA_PAPERCLIP).data(), nullptr);
        }

        // Subject (small bold, clipped)
        {
            float arv = d.has_attachment ? subject_fs * 1.4f : 0.0f;
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x + padx, y, cw - arv - padx * 2.0f, h);
            nvgFontSize(ctx, subject_fs);
            nvgFontFace(ctx, "sans-bold");
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
                                cw - padx * 2.0f, clip_h + preview_fs * 0.3f);
            nvgTextBox(ctx, x + padx, y3 - preview_fs * 0.55f,
                       cw - padx * 2.0f, d.preview.c_str(), nullptr);
            nvgRestore(ctx);
        }

        // Bottom separator (non-selected rows only)
        if (!sel) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, x + padx,      y + h - 0.5f);
            nvgLineTo(ctx, x + cw - padx, y + h - 0.5f);
            nvgStrokeColor(ctx, Color(195, 198, 208, 255));
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);
        }
    }

    /* ---- scroll-to-show (used by keyboard nav) ---- */
    void scroll_to_show(int idx) {
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

    /* ---- state ---- */
    std::vector<EmailData> m_emails;
    int    m_selected = -1;
    int    m_hovered  = -1;
    float  m_scroll   = 0.0f;
    float  m_vel      = 0.0f;   // inertia velocity (px/s)
    double m_last_t   = 0.0;    // timestamp of last draw (for dt)

    bool  m_sb_drag        = false;
    float m_sb_drag_start  = 0.0f;
    float m_sb_drag_origin = 0.0f;

    std::function<void(int, const EmailData &)> m_on_select;
};

// ---------------------------------------------------------------------------
// parse_markdown — minimal Markdown → Document converter
// Supports: # / ## / ### headers, **bold**, *italic*, `code`, ```fenced```
// Blank lines start a new paragraph.
// ---------------------------------------------------------------------------
static void parse_markdown(Document &doc, const std::string &md,
                           NVGcolor text_color = nvgRGBA(20, 20, 25, 255),
                           float base_size = 16.0f)
{
    doc.paragraphs.clear();

    Style normal;  normal.fontSize = base_size;          normal.fgColor = text_color;
    Style bold     = normal; bold.bold      = true;
    Style italic_s = normal; italic_s.italic = true;
    Style code_s   = normal; code_s.monospace = true;
                             code_s.fontSize  = base_size * 0.875f;
                             code_s.bgColor   = nvgRGBA(220, 220, 228, 255);
    Style h1 = normal; h1.fontSize = base_size * 1.625f; h1.bold = true;
    Style h2 = normal; h2.fontSize = base_size * 1.25f;  h2.bold = true;
    Style h3 = normal; h3.fontSize = base_size * 1.0625f; h3.bold = true;

    // Inline-span parser: **bold**, *italic*, `code`, plain text
    auto append_inline = [&](Paragraph *p, const std::string &text) {
        size_t i = 0;
        while (i < text.size()) {
            if (i + 1 < text.size() && text[i] == '*' && text[i+1] == '*') {
                size_t s = i + 2, e = text.find("**", s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), bold);
                    i = e + 2; continue;
                }
            } else if (text[i] == '*' && (i == 0 || text[i-1] != '*')) {
                size_t s = i + 1, e = text.find('*', s);
                if (e != std::string::npos && e > s) {
                    p->addText(text.substr(s, e - s), italic_s);
                    i = e + 1; continue;
                }
            } else if (text[i] == '`') {
                size_t s = i + 1, e = text.find('`', s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), code_s);
                    i = e + 1; continue;
                }
            }
            size_t s = i;
            while (i < text.size() && text[i] != '*' && text[i] != '`') ++i;
            if (i > s) p->addText(text.substr(s, i - s), normal);
            else       ++i;
        }
    };

    std::istringstream iss(md);
    std::string line;
    Paragraph *cur = nullptr;
    bool inCode = false;
    std::string codeBuf;

    while (std::getline(iss, line)) {
        // Detect in-paragraph line-break markers BEFORE stripping whitespace:
        //   trailing \  (backslash)     — Pandoc / CommonMark style
        //   trailing    (two spaces)    — GFM style
        // Both produce a tight \n within the current paragraph rather than
        // a new paragraph, matching the HTML <br> semantic.
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

        if (!cur) cur = doc.addParagraph();
        else      cur->addText(" ", normal);  // soft-wrap join
        append_inline(cur, line);
        if (inline_break)
            cur->addText("\n", normal);  // tight in-paragraph line break
    }

    if (inCode && !codeBuf.empty())
        doc.addParagraph()->addText(codeBuf, code_s);
    if (doc.paragraphs.empty())
        doc.addParagraph();
}

// ---------------------------------------------------------------------------
// MailApp — the demo application
// ---------------------------------------------------------------------------
class MailApp : public Screen {
public:
    Window        *m_rootWindow    = nullptr;
    EmailListView *m_email_list     = nullptr;
    TextEditor    *m_editor         = nullptr;

    MailApp() : Screen(Vector2i(1100, 700), "Juicy Mail — Folder View Demo") {
        inc_ref();

        // Theme — light background like macOS
        Theme *theme = m_theme;
        theme->m_window_fill_unfocused = Color(242, 242, 247, 255);
        theme->m_window_fill_focused   = Color(245, 245, 250, 255);
        theme->m_text_color            = Color(30, 30, 30, 255);
        theme->m_icon_color            = Color(80, 130, 210, 255);
        theme->m_disabled_text_color   = Color(120, 120, 130, 255);
        theme->m_split_divider_width    = 2;
        theme->m_standard_font_size     = 16.0f;


        // Root borderless window
        Window *window = new Window(this, "", true);
        m_rootWindow = window;
        window->set_position(Vector2i(0, 0));
        window->set_size(this->size());
        window->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        // Horizontal split: sidebar | content
        Split *split = new Split(window, Split::Orientation::Horizontal);
        split->set_max_size({2048, 2048});
        split->set_min_size(100);
        split->set_grow_parent(true);

        // ---- Left: FolderView sidebar ----
        auto fv = new FolderView(split, [this](FolderItem *item) {
            on_folder_selected(item);
        });
        fv->set_min_width(250);

        // ---- Right side: inner Split  (email list | message pane) ----
        Split *inner_split = new Split(split, Split::Orientation::Horizontal);
        inner_split->set_max_size({2048, 2048});
        inner_split->set_min_size(100);
        // NOTE: do NOT call set_grow_parent(true) here.
        // The outer Split already sizes inner_split explicitly via set_size() before
        // calling perform_layout(). set_grow_parent triggers apply_fill_parent() which
        // uses a symmetric-margin formula (width = parent_w - 2*pos_x) and chops off
        // exactly one sidebar-width from the right edge.

        // ---- Middle: email list ----
        m_email_list = new EmailListView(inner_split,
            [this](int idx, const EmailData &d) { on_email_selected(idx, d); });
        m_email_list->set_min_width(280);
        m_email_list->set_font_size(26);  // ← tune this to resize all email rows
        m_email_list->build_random_emails(1000);

        // ---- Right: message area (FlexLayout so editor fills remaining height) ----
        Widget *right = new Widget(inner_split);
        auto *rflex = new FlexLayout(FlexDirection::Column,
                                     JustifyContent::FlexStart,
                                     AlignItems::Stretch, 0, 0);
        right->set_layout(rflex);
        right->set_min_width(100);

        // Toolbar (fixed height, flex_grow=0 by default)
        Widget *toolbar = new Widget(right);
        toolbar->set_layout(
            new BoxLayout(Orientation::Horizontal, Alignment::Middle, 8, 6));
        toolbar->set_min_height(40);
        toolbar->set_height(40);

        auto make_tool = [&](const std::string &cap, int icon) {
            Button *btn = new Button(toolbar, cap, icon);
            btn->set_font_size(13);
            btn->set_transparent(true);
            return btn;
        };

        make_tool("", FA_REPLY);
        make_tool("", FA_REPLY_ALL);
        make_tool("", FA_ARROW_RIGHT);
        make_tool("", FA_TRASH);
        make_tool("", FA_ARCHIVE);
        make_tool("", FA_FLAG);

        // Hairline separator
        Widget *sep = new Widget(right);
        sep->set_min_height(1);
        sep->set_height(1);

        // Message body editor — flex_grow:1 fills all remaining vertical space
        m_editor = new TextEditor(right, TextEditor::Mode::RichText);
        m_editor->set_background_color(Color(250, 250, 252, 255));
        m_editor->set_padding(16);
        m_editor->set_read_only(true);
        Style dark_style;
        dark_style.fgColor = nvgRGBA(20, 20, 20, 255);
        dark_style.fontSize = 16.f;
        m_editor->set_default_style(dark_style);
        m_editor->set_height_flex(SizeMode::Expanding);
        rflex->set_flex_item(m_editor, FlexLayout::FlexItem(1.0f));
        parse_markdown(*m_editor->document(),
            "*Select a message from the list to read it here.*",
            nvgRGBA(20, 20, 25, 255), 32.0f);

        split->set_drag_position(0.22f);
        inner_split->set_drag_position(0.38f);
        window->set_size(this->size());
        perform_layout();
    }

    void on_folder_selected(FolderItem *item) {
        // Could filter the email list here; for now just show folder name
        (void)item;
    }

    void on_email_selected(int /*idx*/, const EmailData &d) {
        if (!m_editor) return;
        // Format as a small markdown document
        std::string md;
        md += "## " + d.sender + "\n\n";
        md += "**Subject:** " + d.subject + "  \n";  // two spaces = hard break
        md += "**Date:** "    + d.date;
        if (d.has_attachment) md += "   **(attachment)**";
        md += "\n\n---\n\n";
        md += d.preview + "\n";
        parse_markdown(*m_editor->document(), md,
                       nvgRGBA(20, 20, 25, 255), 32.0f);
        m_editor->set_caret({0, 0});  // scroll back to top
        screen()->redraw();
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            set_visible(false);
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        // Light background gradient
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        NVGpaint bg = nvgLinearGradient(ctx, 0, 0, 0, (float)m_size.y(),
                                        nvgRGBA(235, 237, 242, 255),
                                        nvgRGBA(225, 228, 235, 255));
        nvgFillPaint(ctx, bg);
        nvgFill(ctx);
        nvgRestore(ctx);
        Screen::draw(ctx);
    }

    virtual bool resize_event(const Vector2i &size) override {
        if (m_rootWindow) {
            m_rootWindow->set_size(size);
            perform_layout();
        }
        Screen::resize_event(size);
        return true;
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    try {
        nanogui::init();
        {
            ref<MailApp> app = new MailApp();
            app->dec_ref();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop(-1);
        }
        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string error_msg =
            std::string("Caught a fatal error: ") + std::string(e.what());
        std::cerr << error_msg << std::endl;
        return -1;
    }
    return 0;
}
