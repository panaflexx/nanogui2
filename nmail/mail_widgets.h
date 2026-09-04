/*
 * nmail/mail_widgets.h — custom NanoGUI widgets for the mail client:
 * the folder sidebar (FolderView / FolderItem / SectionHeader) and the
 * virtual-scrolling message list (EmailListView).  Pure GUI code; the
 * only mail dependency is the MailFolder struct used by FolderView::rebuild.
 */
#pragma once

#include <nanogui/widget.h>
#include <nanogui/scrollpanel.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "imap_client.h"   // MailFolder

class FolderItem;

class FolderChildrenContainer : public nanogui::Widget {
public:
    FolderChildrenContainer(nanogui::Widget *parent) : nanogui::Widget(parent) {}

    nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;
};

class FolderContainer : public nanogui::Widget {
public:
    FolderContainer(nanogui::Widget *parent) : nanogui::Widget(parent) {}

    void perform_layout(NVGcontext *ctx) override;
};

// ---------------------------------------------------------------------------
// FolderItem — a single row in the sidebar
// ---------------------------------------------------------------------------
class FolderItem : public nanogui::Widget {
public:
    FolderItem(nanogui::Widget *parent, const std::string &caption, int icon,
               int indent = 0, int badge = 0, bool expandable = false);

    /* ---- Accessors ---- */
    const std::string &caption() const { return m_caption; }
    void set_caption(const std::string &c) { m_caption = c; }

    int badge() const { return m_badge; }
    void set_badge(int b) { m_badge = b; }

    bool selected() const;

    bool expanded() const { return m_expanded; }

    void set_select_callback(std::function<void(FolderItem *)> cb) {
        m_select_callback = cb;
    }

    /* ---- Children container for expandable items ---- */
    nanogui::Widget *children_container() const { return m_children_container; }
    nanogui::Widget *ensure_children_container();

    nanogui::Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return nanogui::Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    /* ---- Events ---- */
    bool mouse_enter_event(const nanogui::Vector2i &p, bool enter) override;
    bool mouse_button_event(const nanogui::Vector2i &p, int button,
                            bool down, int modifiers) override;
    void toggle_expand();

    /* ---- Drawing ---- */
    void draw(NVGcontext *ctx) override;

private:
    std::string m_caption;
    int m_icon;
    int m_indent;
    int m_badge;
    bool m_expandable;
    bool m_expanded;
    bool m_hovered;
    nanogui::Widget *m_children_container;
    std::function<void(FolderItem *)> m_select_callback;
};

// ---------------------------------------------------------------------------
// SectionHeader — a small gray header (account name)
// ---------------------------------------------------------------------------
class SectionHeader : public nanogui::Widget {
public:
    SectionHeader(nanogui::Widget *parent, const std::string &title);

    nanogui::Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return nanogui::Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    void draw(NVGcontext *ctx) override;

private:
    std::string m_title;
};

// ---------------------------------------------------------------------------
// FolderView — the sidebar widget, populated from the IMAP server
// ---------------------------------------------------------------------------
class FolderView : public nanogui::Widget {
public:
    FolderView(nanogui::Widget *parent, std::function<void(FolderItem *)> on_select);

    void draw(NVGcontext *ctx) override;

    /* Rebuild the sidebar from the server's folder list.
     * `selected_name` (full IMAP name) is re-highlighted without firing
     * the select callback, so a LIST/refresh does not drop the current
     * folder selection. */
    void rebuild(const std::string &account,
                 const std::vector<MailFolder> &folders,
                 const std::string &selected_name = "");

private:
    nanogui::ScrollPanel *m_scroll;
    nanogui::Widget *m_container;
    std::function<void(FolderItem *)> m_on_select;

    /* Leaf name after the last hierarchy delimiter for display. */
    static std::string display_name(const std::string &name);
    static int folder_icon(const std::string &name);
};

// ---------------------------------------------------------------------------
// EmailData — plain struct describing one message in the list
// ---------------------------------------------------------------------------
struct EmailData {
    int         seq = 0;           // IMAP message sequence number
    std::string sender;
    std::string subject;
    std::string preview;
    std::string date;
    bool        has_attachment = false;
    bool        seen = true;
};

// ---------------------------------------------------------------------------
// EmailListView — virtual-scroll list: O(visible) draw cost, no child widgets
// ---------------------------------------------------------------------------
class EmailListView : public nanogui::Widget {
public:
    static constexpr float ROW_SCALE = 4.6f;
    static constexpr float SB_W      = 6.0f;
    static constexpr float SB_MARGIN = 3.0f;

    /* Status indicators drawn at the top-right of a row, just left of the
     * date, stacking leftward in table order.  A new flag (starred,
     * has_attachment, ...) only needs a bool on EmailData and a row here.
     * `show_when` is the flag value that displays the glyph, so "unread"
     * is `seen == false`.  Glyphs come from the monochrome FontAwesome
     * "icons" face, so the fill color tints them -- pick per-row colors
     * freely (unread green, starred amber, ...). */
    struct Indicator {
        bool EmailData::*flag;
        bool        show_when;
        const char *glyph;   // UTF-8 literal, drawn with the "icons" (FontAwesome) face
        nanogui::Color color;
    };
    inline static const Indicator kIndicators[] = {
        { &EmailData::seen, false, "\xEF\x84\x91" /* FA_CIRCLE */, nanogui::Color(60, 180, 75, 255) },
    };
    static constexpr float IND_FONT_SCALE = 0.45f;  // of the sender font size
    static constexpr float IND_GAP        = 5.0f;

    EmailListView(nanogui::Widget *parent,
                  std::function<void(int, const EmailData &)> on_select = nullptr)
        : nanogui::Widget(parent), m_on_select(std::move(on_select)) {
        /* Virtual-scroll + inertia + spinner: repaint every frame instead of
           being baked into a retained parent display list. */
        set_live(true);
    }

    /* ---- geometry helpers ---- */
    float row_h()      const { return std::floor(font_size() * ROW_SCALE); }
    float total_h()    const { return row_h() * (float)m_emails.size(); }
    float max_scroll() const { return std::max(0.0f, total_h() - (float)m_size.y()); }

    /* ---- events ---- */
    std::function<void()> on_viewport_changed; // MailApp hooks scroll/paging prefetch
    void notify_viewport() { if (on_viewport_changed) on_viewport_changed(); }

    bool mouse_motion_event(const nanogui::Vector2i &p, const nanogui::Vector2i &,
                            int, int) override;
    bool mouse_enter_event(const nanogui::Vector2i &p, bool enter) override;
    bool mouse_button_event(const nanogui::Vector2i &p, int button,
                            bool down, int mods) override;
    bool mouse_drag_event(const nanogui::Vector2i &p, const nanogui::Vector2i &,
                          int, int) override;
    bool scroll_event(const nanogui::Vector2i &, const nanogui::Vector2f &rel) override;
    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    /* ---- draw (also drives the inertia animation) ---- */
    void draw(NVGcontext *ctx) override;

    /* Update the preview text for an already-listed row in place. */
    void update_preview(int seq, const std::string &preview);

    // viewport helpers — used by MailApp to prioritize prefetch
    std::pair<int,int> visible_range() const {
        if (m_emails.empty() || m_size.y() <= 0) return {0,0};
        int first = std::max(0, (int)(m_scroll / row_h()));
        int last  = std::min((int)m_emails.size(), (int)((m_scroll + (float)m_size.y()) / row_h()) + 2);
        return {first, last};
    }
    std::vector<int> visible_seqs(int pad = 6) const {
        auto [first,last] = visible_range();
        int a = std::max(0, first - pad);
        int b = std::min((int)m_emails.size(), last + pad);
        std::vector<int> out; out.reserve(b-a);
        for (int i=a;i<b;++i) out.push_back(m_emails[i].seq);
        return out;
    }
    const std::vector<EmailData>& emails() const { return m_emails; }

    /* ---- data ---- */
    void set_emails(std::vector<EmailData> emails);

    /* Append older rows (from a "load more" fetch) without resetting
       scroll or selection. */
    void append_emails(std::vector<EmailData> more);

    /* Splice newly-arrived mail in at the top (background auto-check)
       without disturbing the user's current place: the rows already on
       screen stay on screen (scroll advances by exactly the inserted
       height) and the selected message stays selected (re-resolved by
       seq, since prepending shifts every existing row's index). */
    void prepend_emails(std::vector<EmailData> newer);

    /* Spinner strip at the bottom while older messages are fetched. */
    void set_loading_more(bool v);

    /* Called from draw() whenever the list is scrolled to the bottom. */
    void set_on_hit_bottom(std::function<void()> cb) {
        m_on_hit_bottom = std::move(cb);
    }

    /* ---- appearance ---- */
    void set_dark(bool dark);

    int selected_index() const { return m_selected; }
    int selected_seq() const {
        if (m_selected >= 0 && m_selected < (int)m_emails.size())
            return m_emails[m_selected].seq;
        return -1;
    }
    const EmailData* selected_data() const {
        if (m_selected >= 0 && m_selected < (int)m_emails.size())
            return &m_emails[m_selected];
        return nullptr;
    }
    /* Flip a row's read state in place (the server confirmed a \Seen flag).
     * Returns false when the seq is not in the current view. */
    bool set_seen(int seq, bool seen) {
        for (EmailData &e : m_emails)
            if (e.seq == seq) {
                if (e.seen == seen) return true;
                e.seen = seen;
                return true;
            }
        return false;
    }

    // Remove a row by seq, preserving scroll position and viewport.
    // Selects the message below the deleted one, or the last if at end.
    // IMAP sequence numbers shift after EXPUNGE, so remaining seqs > deleted
    // are decremented to stay in sync without a full refresh.
    bool remove_seq(int seq);

    void clear_selection();

private:
    /* ---- scrollbar geometry ---- */
    float thumb_h() const;
    // Returns {abs_x, abs_y, w, h} of the scrollbar thumb
    std::array<float, 4> thumb_rect() const;
    void draw_scrollbar(NVGcontext *ctx) const;

    /* Overlay strip with a spinner + caption shown while older messages
       are being fetched. */
    void draw_loading_strip(NVGcontext *ctx);

    int idx_at(int abs_y) const;

    /* ---- row drawing ---- */
    void draw_row(NVGcontext *ctx, int idx, float x, float y, float w) const;

    /* ---- scroll-to-show (used by keyboard nav) ---- */
    void scroll_to_show(int idx);

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
    bool  m_dark           = false;
    bool  m_loading_more   = false;   // spinner strip while paging older mail

    std::function<void(int, const EmailData &)> m_on_select;
    std::function<void()> m_on_hit_bottom;
};
