/*
 * nmail/htmldocument.h — HtmlDocument: HTML e-mail viewer widget.
 *
 * Parses a text/html part with Gumbo and builds a widget tree:
 * block containers are plain Widgets driven by FlexLayout (column for
 * normal flow, row for table rows), inline/text content is collected
 * into HtmlText leaves that render a nanogui::Document (styled runs,
 * headings, bullets, <pre>, <hr>, images).
 *
 * Host it in a ScrollPanel; it reports its laid-out content height as
 * its preferred size.
 */
#pragma once

#include <nanogui/widget.h>
#include <nanogui/document.h>
#include <functional>
#include <string>

/* A resolved <img>: an NVG image id plus its intrinsic pixel size.
 * id == 0 means "unresolved" (caller draws a placeholder). */
struct HtmlImageInfo {
    int   id = 0;
    float w  = 0.0f, h = 0.0f;
};

class HtmlDocument : public nanogui::Widget {
public:
    explicit HtmlDocument(nanogui::Widget *parent);

    /* Parse + rebuild the widget tree from an HTML fragment/document. */
    void set_html(const std::string &html);
    /* text/plain fallback: blank-line separated paragraphs, no markup. */
    void set_plain(const std::string &text);
    /* Show a pre-built Document (used for Markdown bodies). */
    void set_document(nanogui::Document &&doc);
    void clear();

    void set_colors(NVGcolor text, NVGcolor meta) { m_text = text; m_meta = meta; }
    void set_background(NVGcolor bg) { m_bg = bg; }

    NVGcolor text_color() const { return m_text; }
    NVGcolor meta_color() const { return m_meta; }

    /* Resolve an <img> src (cid: or http:) to an NVG image.  Called on the
     * GUI thread while building.  Return id == 0 for the placeholder. */
    std::function<HtmlImageInfo(const std::string &src)> image_resolver;

    /* True if the last set_html() saw any http(s) <img> (drives the
     * "Load remote images" button in MailApp). */
    bool has_remote_images() const { return m_has_remote; }
    void note_remote_image() { m_has_remote = true; }

    /* Re-query image_resolver for placeholder <img> blocks (id == 0) and
     * bind any textures that have arrived, without re-parsing HTML.
     * Returns how many images were newly bound. */
    int bind_loaded_images();

    virtual void draw(NVGcontext *ctx) override;
    virtual nanogui::Vector2i preferred_size(NVGcontext *ctx) const override;

    /* Re-run layout + schedule a repaint after content changes. */
    void relayout();

    /* Called by leaf widgets when their measured height changes during
     * paint: schedule one clean relayout at the top of the next draw
     * (never re-enter layout mid-paint — that corrupts the geometry of
     * widgets already drawn this frame). */
    void request_reflow();

    /* One line per leaf widget: rect + text snippet (test harnesses). */
    std::string debug_summary() const;

private:
    NVGcolor m_text = NVGcolor{ { { 0.f, 0.f, 0.f, 1.f } } };
    NVGcolor m_meta = NVGcolor{ { { 0.45f, 0.45f, 0.5f, 1.f } } };
    NVGcolor m_bg   = NVGcolor{ { { 0.f, 0.f, 0.f, 0.f } } };
    bool     m_has_remote = false;
    bool     m_reflow_pending = false;
};
