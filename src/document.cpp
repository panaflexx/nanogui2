/*
    src/document.cpp -- Rich-text Document / Paragraph rendering.

    This is the same layout algorithm that ships with the
    nanovg-colorfont-atlas demo, lifted into the nanogui library so it
    can back both the markdown / rich-text renderer and the TextEditor
    widget.

    IMPORTANT measurement note (from the original demo):
      nvgTextBounds() returns the *horizontal advance* of the measured
      text as its return value (i.e. where the next glyph would be
      drawn), while the `bounds` array it writes contains the *visual
      bounding box* (x0, y0, x1, y1). Layout uses the advance, visual
      elements (background rectangles, underlines, right-flush in
      justified lines) use the bounds.
*/

#include <nanogui/document.h>
#include <nanogui/opengl.h>

#include <algorithm>
#include <cstring>
#include <string_view>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// Paragraph helpers
// ---------------------------------------------------------------------------

std::string Paragraph::plain_text() const {
    std::string out;
    size_t n = 0;
    for (const Text& r : runs) n += r.content.size();
    out.reserve(n);
    for (const Text& r : runs) out += r.content;
    return out;
}

size_t Paragraph::byte_length() const {
    size_t n = 0;
    for (const Text& r : runs) n += r.content.size();
    return n;
}

static bool style_equal(const Style& a, const Style& b) {
    return a.fontSize == b.fontSize &&
           a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.monospace == b.monospace &&
           a.displayNone == b.displayNone &&
           a.superscript == b.superscript &&
           a.verticalMiddle == b.verticalMiddle &&
           a.strike == b.strike &&
           a.allCaps == b.allCaps &&
           a.whiteSpace == b.whiteSpace &&
           a.lineHeight == b.lineHeight &&
           a.letterSpacing == b.letterSpacing &&
           a.opacity == b.opacity &&
           a.padX == b.padX && a.padY == b.padY &&
           a.borderWidth == b.borderWidth &&
           a.linkUrl == b.linkUrl &&
           std::memcmp(&a.fgColor, &b.fgColor, sizeof(NVGcolor)) == 0 &&
           std::memcmp(&a.bgColor, &b.bgColor, sizeof(NVGcolor)) == 0 &&
           std::memcmp(&a.borderColor, &b.borderColor, sizeof(NVGcolor)) == 0;
}

size_t Paragraph::split_run_at(size_t col) {
    size_t acc = 0;
    for (size_t i = 0; i < runs.size(); ++i) {
        size_t n = runs[i].content.size();
        if (col <= acc)
            return i;                    // already at a run boundary
        if (col < acc + n) {             // strictly inside run i: split it
            Text tail;
            tail.style   = runs[i].style;
            tail.content = runs[i].content.substr(col - acc);
            runs[i].content.erase(col - acc);
            runs.insert(runs.begin() + (ptrdiff_t)i + 1, std::move(tail));
            return i + 1;
        }
        acc += n;
    }
    return runs.size();                  // at/after the end of the paragraph
}

void Paragraph::coalesce_runs() {
    // Drop empty runs first.
    runs.erase(std::remove_if(runs.begin(), runs.end(),
                              [](const Text& t) { return t.content.empty(); }),
               runs.end());
    // Merge neighbors with identical styles.
    for (size_t i = 1; i < runs.size();) {
        if (style_equal(runs[i - 1].style, runs[i].style)) {
            runs[i - 1].content += runs[i].content;
            runs.erase(runs.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

// ---------------------------------------------------------------------------
// Document construction helpers
// ---------------------------------------------------------------------------

Paragraph* Document::addParagraph() {
    paragraphs.emplace_back(std::make_unique<Paragraph>());
    m_layout_dirty = true;
    return paragraphs.back().get();
}

Paragraph* Document::addParagraph(std::string text, const Style& style) {
    auto* p = addParagraph();
    p->addText(std::move(text), style);
    return p;
}

Paragraph* Document::insertParagraph(size_t index) {
    if (index >= paragraphs.size())
        return addParagraph();
    auto it = paragraphs.insert(paragraphs.begin() + (ptrdiff_t)index,
                                std::make_unique<Paragraph>());
    m_layout_dirty = true;
    return it->get();
}

bool Document::removeParagraph(size_t index) {
    if (index >= paragraphs.size()) return false;
    paragraphs.erase(paragraphs.begin() + (ptrdiff_t)index);
    m_layout_dirty = true;
    return true;
}

size_t Document::total_byte_length() const {
    if (paragraphs.empty()) return 0;
    size_t n = 0;
    for (auto& p : paragraphs) n += p->byte_length();
    return n + (paragraphs.size() - 1); // '\n' separators
}

// ---------------------------------------------------------------------------
// Font / measurement helpers
// ---------------------------------------------------------------------------

uint64_t Document::makeKey(const char* face, float size) {
    uint64_t h = 1469598103934665603ULL;
    for (const char* p = face; *p; ++p) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    uint32_t q = (uint32_t)(size * 1024.0f);
    h ^= q;
    h *= 1099511628211ULL;
    return h;
}

const char* Document::faceForStyle(const Style& s) {
    if (s.monospace) return "mono";
    if (s.bold && s.italic) return "sans-bolditalic";
    if (s.bold)             return "sans-bold";
    if (s.italic)           return "sans-italic";
    return "sans";
}

void Document::applyFont(NVGcontext* ctx, const Style& s) const {
    nvgFontSize(ctx, s.fontSize);
    nvgFontFace(ctx, faceForStyle(s));
}

Document::VMetrics Document::metricsFor(NVGcontext* ctx, const Style& s) {
    uint64_t k = makeKey(faceForStyle(s), s.fontSize);
    auto it = m_metricsCache.find(k);
    if (it != m_metricsCache.end()) return it->second;
    applyFont(ctx, s);
    VMetrics m{};
    nvgTextMetrics(ctx, &m.ascender, &m.descender, &m.lineh);
    m_metricsCache.emplace(k, m);
    return m;
}

float Document::spaceWidthFor(NVGcontext* ctx, const Style& s) {
    uint64_t k = makeKey(faceForStyle(s), s.fontSize);
    auto it = m_spaceCache.find(k);
    if (it != m_spaceCache.end()) return it->second;
    applyFont(ctx, s);
    float b[4];
    float w1 = nvgTextBounds(ctx, 0, 0, "x",   nullptr, b);
    float w2 = nvgTextBounds(ctx, 0, 0, "x x", nullptr, b);
    float sp = w2 - 2.0f * w1;
    if (sp <= 0.0f) sp = s.fontSize * 0.25f;
    m_spaceCache.emplace(k, sp);
    return sp;
}

void Document::measureWord(NVGcontext* ctx, const Style& s,
                           const char* begin, const char* end,
                           float& advance, float& leftBearing,
                           float& visualRight) {
    applyFont(ctx, s);
    float b[4];
    advance     = nvgTextBounds(ctx, 0, 0, begin, end, b);
    leftBearing = b[0];
    visualRight = b[2];
}

void Document::clearMeasurementCaches() {
    m_spaceCache.clear();
    m_metricsCache.clear();
}

// ---------------------------------------------------------------------------
// Local helpers (line / word iteration)
// ---------------------------------------------------------------------------

namespace {

template <typename Fn>
void forEachLine(std::string_view s, Fn fn) {
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            fn(s.substr(start, i - start));
            start = i + 1;
        }
    }
    fn(s.substr(start));
}

template <typename Fn>
void forEachWord(std::string_view line, Fn fn) {
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t start = i;
        while (i < n && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) fn(line.substr(start, i - start));
    }
}

/// True when every run of the paragraph is monospace (i.e. it renders as
/// part of a code block).  Empty runs count as code — a blank line inside
/// a code block must not split it — but a run-less paragraph does not.
bool paragraph_is_code(const Paragraph& p) {
    if (p.runs.empty()) return false;
    for (const Text& r : p.runs)
        if (!r.style.monospace) return false;
    return true;
}

} // namespace

float Document::measure_natural_width(NVGcontext* ctx) {
    float maxw = 0.0f;
    for (const auto& para : paragraphs) {
        if (para->isImage) {
            maxw = std::max(maxw, para->image_w);
            continue;
        }
        float lineW = 0.0f;
        bool firstWord = true;
        for (const Text& run : para->runs) {
            if (run.isImageRun) {
                lineW += run.image_w;
                firstWord = false;
                continue;
            }
            const float sp = run.style.monospace ? 0.0f : spaceWidthFor(ctx, run.style);
            bool firstPiece = true;
            forEachLine(run.content, [&](std::string_view piece) {
                if (!firstPiece) {
                    maxw = std::max(maxw, lineW);
                    lineW = 0.0f;
                    firstWord = true;
                }
                firstPiece = false;
                forEachWord(piece, [&](std::string_view word) {
                    float adv, lb, vr;
                    measureWord(ctx, run.style, word.data(),
                                word.data() + word.size(), adv, lb, vr);
                    if (!firstWord) lineW += sp;
                    firstWord = false;
                    lineW += adv;
                });
            });
        }
        maxw = std::max(maxw, lineW);
    }
    return maxw;
}

/* Draw an NVG image into a rect (used by image block paragraphs).
 * id <= 0 is a loading placeholder so layout can reserve space before
 * the texture arrives. */
static void draw_image_block(NVGcontext* ctx, int image,
                             float x, float y, float w, float h) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (image <= 0) {
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, w, h);
        nvgFillColor(ctx, nvgRGBA(128, 128, 136, 40));
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f);
        nvgStrokeColor(ctx, nvgRGBA(128, 128, 136, 90));
        nvgStrokeWidth(ctx, 1.0f);
        nvgStroke(ctx);
        return;
    }
    NVGpaint p = nvgImagePattern(ctx, x, y, w, h, 0.0f, image, 1.0f);
    nvgBeginPath(ctx);
    nvgRect(ctx, x, y, w, h);
    nvgFillPaint(ctx, p);
    nvgFill(ctx);
}

// ---------------------------------------------------------------------------
// Top-level draw
// ---------------------------------------------------------------------------

void Document::translate_rich_layout(float dx, float dy) {
    if (dx == 0.f && dy == 0.f)
        return;
    for (RichLine& rl : m_rich_layout) {
        rl.y_top    += dy;
        rl.y_bottom += dy;
        rl.baseline += dy;
        rl.x_start  += dx;
        rl.mono_bg_x += dx;
        rl.mono_bg_y += dy;
        rl.bullet_cx += dx;
        rl.bullet_cy += dy;
        for (WordLayout& w : rl.words)
            w.x += dx;
    }
    m_layout_origin_x += dx;
    m_layout_origin_y += dy;
    m_laid_origin_x   += dx;
    m_laid_origin_y   += dy;
}

void Document::draw(NVGcontext* ctx, float originX, float originY) {
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    const bool need_layout =
        layout_only ||
        m_layout_dirty ||
        m_laid_content_width != contentWidth ||
        m_rich_layout.empty();

    if (need_layout) {
        // Full reflow + paint (measures with nvgTextBounds).
        m_rich_layout.clear();
        m_layout_origin_x = originX;
        m_layout_origin_y = originY;

        float y = originY;
        for (size_t pi = 0; pi < paragraphs.size(); ++pi) {
            auto& para = paragraphs[pi];
            if (para->isImage) {
                float pad = 0.0f;
                /* Fit to the content width, preserve aspect ratio. */
                float scale = (para->image_w > contentWidth && para->image_w > 0.f)
                              ? contentWidth / para->image_w : 1.0f;
                float dw = para->image_w * scale;
                float dh = para->image_h * scale;
                float iy = y + pad;

                /* Sentinel line/word so hit-test and the fast path can
                   recognize the block (mirrors the RULE pattern). */
                RichLine img_line;
                img_line.para_idx   = pi;
                img_line.byte_start = 0;
                img_line.byte_end   = 0;
                img_line.y_top      = y;
                img_line.y_bottom   = iy + dh + pad;
                img_line.baseline   = iy;
                img_line.x_start    = originX;
                WordLayout iw;
                iw.byte_start = 0;
                iw.byte_end   = 0;
                iw.x          = originX;
                iw.advance    = dw;                 // draw width
                iw.style.fontSize = dh;             // draw height (piggy-back)
                iw.image      = para->image;
                iw.text       = "\x01IMAGE";
                iw.linkUrl    = para->linkUrl;
                img_line.words.push_back(std::move(iw));
                m_rich_layout.push_back(std::move(img_line));

                if (!layout_only)
                    draw_image_block(ctx, para->image, originX, iy, dw, dh);
                y = iy + dh + pad;
            } else if (para->isRule) {
                float pad = paragraphSpacing * 0.5f;
                float ry  = y + pad;
                // Capture a zero-word line so hit-test can skip rules cleanly.
                RichLine rule_line;
                rule_line.para_idx   = pi;
                rule_line.byte_start = 0;
                rule_line.byte_end   = 0;
                rule_line.y_top      = y;
                rule_line.y_bottom   = ry + pad;
                rule_line.baseline   = ry;
                rule_line.x_start    = originX;
                // Stash rule draw params in a sentinel WordLayout
                WordLayout rw;
                rw.byte_start = 0;
                rw.byte_end   = 0;
                rw.x          = originX;
                rw.advance    = contentWidth * 0.5f;
                rw.style.fgColor = para->ruleColor;
                rw.style.fontSize = para->ruleThickness; // thickness piggy-back
                rw.text       = "\x01RULE"; // sentinel
                rule_line.words.push_back(std::move(rw));
                m_rich_layout.push_back(std::move(rule_line));

                if (!layout_only) {
                    nvgBeginPath(ctx);
                    nvgMoveTo(ctx, originX, ry);
                    nvgLineTo(ctx, originX + contentWidth * 0.5f, ry);
                    nvgStrokeColor(ctx, para->ruleColor);
                    nvgStrokeWidth(ctx, para->ruleThickness);
                    nvgStroke(ctx);
                }
                y = ry + pad;
            } else {
                y = drawParagraph(ctx, *para, originX, y, pi);
                if (pi + 1 < paragraphs.size()) {
                    const Paragraph& next = *paragraphs[pi + 1];
                    bool tight = (paragraph_is_code(*para) &&
                                  paragraph_is_code(next)) ||
                                 (para->isBullet && next.isBullet) ||
                                 // Single-image -> short-text is a captioned image, not two paragraphs.
                                 (para->isImage && next.runs.size()==1 && next.runs[0].content.size()<80) ||
                                 // Empty spacer paragraph between rows (common in Madeleine's table gaps)
                                 (next.runs.empty() || (next.runs.size()==1 && next.runs[0].content.size()==0));
                    y += tight ? 0.0f : paragraphSpacing;
                }
            }
        }
        last_drawn_height = y - originY;
        m_laid_height = last_drawn_height;
        m_laid_content_width = contentWidth;
        m_laid_origin_x = originX;
        m_laid_origin_y = originY;
        m_layout_dirty = false;
        return;
    }

    /* Origin-only change: the wrap is still valid.  HtmlText measures at
     * (0,0) then paints at the widget position — translating the cache
     * is the compositor-style path, not a reflow. */
    if (originX != m_laid_origin_x || originY != m_laid_origin_y)
        translate_rich_layout(originX - m_laid_origin_x,
                              originY - m_laid_origin_y);

    last_drawn_height = m_laid_height;

    // ---- Fast path: content/width unchanged (selection drag, hover, move).
    // Replay from the layout cache — no nvgTextBounds reflow.
    for (const RichLine& rl : m_rich_layout) {
        if (rl.words.size() == 1 && rl.words[0].text == "\x01IMAGE") {
            const WordLayout& iw = rl.words[0];
            draw_image_block(ctx, iw.image, iw.x, rl.baseline,
                             iw.advance, iw.style.fontSize);
            continue;
        }
        if (rl.words.size() == 1 && rl.words[0].text == "\x01RULE") {
            const WordLayout& rw = rl.words[0];
            float ry = rl.baseline;
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, rw.x, ry);
            nvgLineTo(ctx, rw.x + rw.advance, ry);
            nvgStrokeColor(ctx, rw.style.fgColor);
            nvgStrokeWidth(ctx, rw.style.fontSize > 0.f ? rw.style.fontSize : 1.f);
            nvgStroke(ctx);
            continue;
        }
        // Code-block / mono background (unified width across contiguous lines).
        if (rl.mono_bg && rl.mono_bg_color.a > 0) {
            nvgBeginPath(ctx);
            nvgFillColor(ctx, rl.mono_bg_color);
            if (rl.mono_bg_radius > 0.5f)
                nvgRoundedRect(ctx, rl.mono_bg_x, rl.mono_bg_y,
                               rl.mono_bg_w, rl.mono_bg_h, rl.mono_bg_radius);
            else
                nvgRect(ctx, rl.mono_bg_x, rl.mono_bg_y, rl.mono_bg_w, rl.mono_bg_h);
            nvgFill(ctx);
            if (rl.mono_bg_border_w > 0.f && rl.mono_bg_border_color.a > 0.f) {
                nvgBeginPath(ctx);
                if (rl.mono_bg_radius > 0.5f)
                    nvgRoundedRect(ctx, rl.mono_bg_x + 0.5f, rl.mono_bg_y + 0.5f,
                                   rl.mono_bg_w - 1.f, rl.mono_bg_h - 1.f,
                                   std::max(0.f, rl.mono_bg_radius - 0.5f));
                else
                    nvgRect(ctx, rl.mono_bg_x + 0.5f, rl.mono_bg_y + 0.5f,
                            rl.mono_bg_w - 1.f, rl.mono_bg_h - 1.f);
                nvgStrokeColor(ctx, rl.mono_bg_border_color);
                nvgStrokeWidth(ctx, rl.mono_bg_border_w);
                nvgStroke(ctx);
            }
        }
        // Bullet list marker.
        if (rl.bullet) {
            nvgBeginPath(ctx);
            nvgCircle(ctx, rl.bullet_cx, rl.bullet_cy, rl.bullet_r);
            nvgFillColor(ctx, rl.bullet_color);
            nvgFill(ctx);
        }

        for (const WordLayout& wl : rl.words) {
            /* wl.image may legitimately be 0 (not loaded yet) — the
             * "\x01IMAGE" sentinel text (set at capture time, see below)
             * is what marks this word as an image slot, mirroring the
             * whole-line block-image sentinel a few lines up. */
            if (wl.text == "\x01IMAGE") {
                float ih = wl.style.fontSize;
                float imgY = rl.baseline - ih;
                // Mirror the layout-time centering: small icons on mixed lines sit
                // on the text midline, not the baseline.
                bool mixed = false; for (auto &ww : rl.words) if (ww.text != "\x01IMAGE") { mixed = true; break; }
                if ((wl.style.verticalMiddle || (mixed && ih <= 48.f)) && ih > 0.f) {
                    // find a text style to get ascender for midline
                    const Style *ts = nullptr; for (auto &ww : rl.words) if (ww.text != "\x01IMAGE") { ts = &ww.style; break; }
                    float asc = ts ? metricsFor(ctx, *ts).ascender : ih * 0.7f;
                    float textMid = rl.baseline - asc * 0.35f;
                    imgY = textMid - ih * 0.5f;
                }
                draw_image_block(ctx, wl.image, wl.x, imgY,
                                 wl.advance, ih);
                continue;
            }
            const Style& st = wl.style;
            applyFont(ctx, st);
            float ty = rl.baseline;
            if (st.superscript) {
                float vpad = 0.f;
                for (const auto& w : rl.words)
                    vpad = std::max(vpad, w.style.padY + w.style.borderWidth);
                const float lineAsc = std::max(1.f, rl.baseline - rl.y_top - vpad);
                ty = rl.baseline - (lineAsc - metricsFor(ctx, st).ascender);
            } else if (st.verticalMiddle) {
                float lineH = rl.y_bottom - rl.y_top - 4.f; // approx; matches y+lineHeight+spacing
                // Use stored lineHeight if available via baseline offset
                float vpad = 0.f; for (auto &w : rl.words) vpad = std::max(vpad, w.style.padY + w.style.borderWidth);
                float fontH = metricsFor(ctx, st).lineh > 1.f ? metricsFor(ctx, st).lineh : (metricsFor(ctx, st).ascender - metricsFor(ctx, st).descender);
                // If explicit lineHeight drove the layout, rl.y_bottom - rl.y_top encodes it
                float centered = rl.y_top + vpad + (lineH - fontH) * 0.5f + metricsFor(ctx, st).ascender;
                // Only apply if it moves up (centering), not down
                if (centered < ty) ty = centered;
            }
            /* Padded pills are painted once for the whole run (mono_bg).
             * Per-word fill with padX overlaps neighbours ("Le" / "More"). */
            if (st.bgColor.a > 0 && !st.monospace &&
                st.padX <= 0.f && st.padY <= 0.f) {
                nvgBeginPath(ctx);
                nvgFillColor(ctx, st.bgColor);
                nvgRect(ctx,
                        wl.x + wl.leftBearing - 1,
                        rl.baseline - metricsFor(ctx, st).ascender - 1,
                        (wl.visualRight - wl.leftBearing) + 2,
                        metricsFor(ctx, st).lineh + 2);
                nvgFill(ctx);
            }
            nvgFillColor(ctx, st.fgColor);
            nvgText(ctx, wl.x, ty, wl.text.c_str(), nullptr);
            if (st.underline) {
                const float ux0 = wl.x + wl.leftBearing;
                const float ux1 = wl.x + wl.visualRight;
                const float uy  = rl.baseline + std::max(1.0f, st.fontSize * 0.08f);
                nvgBeginPath(ctx);
                nvgStrokeColor(ctx, st.fgColor);
                nvgStrokeWidth(ctx, std::max(1.0f, st.fontSize * 0.05f));
                nvgMoveTo(ctx, ux0, uy);
                nvgLineTo(ctx, ux1, uy);
                nvgStroke(ctx);
            }
        }
    }
    last_drawn_height = m_laid_height;
}

// ---------------------------------------------------------------------------
// Paragraph layout + draw  (identical to nanovg-colorfont-atlas.cpp)
// ---------------------------------------------------------------------------

float Document::drawParagraph(NVGcontext* ctx, const Paragraph& para,
                              float originX, float startY, size_t para_idx) {
    /* Empty paragraph (no runs, or runs with no words — e.g. the empty run
     * left behind when Enter splits a paragraph): reserve one full line of
     * height and emit a zero-word RichLine so the caret stays visible and
     * the line remains clickable. */
    auto emit_empty_line = [&]() -> float {
        const Style es = para.runs.empty() ? Style{}
                                           : para.runs.front().style;
        const VMetrics m = metricsFor(ctx, es);
        const float lineHeight = m.ascender - m.descender;  // descender < 0
        const float baseline   = startY + m.ascender;
        const float indent     = para.leftIndent + para.firstLineIndent;
        /* Blank line inside a code block: paint a one-column background
         * stripe so the block reads as continuous. */
        const bool mono_bg = es.monospace && es.bgColor.a > 0;
        float bg_x = 0.f, bg_y = 0.f, bg_w = 0.f, bg_h = 0.f;
        if (mono_bg) {
            const float pad = 4.0f;
            bg_x = originX + indent - pad;
            bg_y = startY - 1;
            bg_w = spaceWidthFor(ctx, es) + 2 * pad;
            bg_h = lineHeight + 6;
            if (!layout_only) {
                nvgBeginPath(ctx);
                nvgFillColor(ctx, es.bgColor);
                nvgRect(ctx, bg_x, bg_y, bg_w, bg_h);
                nvgFill(ctx);
            }
        }
        if (para.isBullet && !layout_only) {
            const float br  = std::max(1.5f, lineHeight * 0.10f);
            const float bcx = originX + para.leftIndent - br * 3.0f;
            const float bcy = baseline - m.ascender * 0.35f;
            nvgBeginPath(ctx);
            nvgCircle(ctx, bcx, bcy, br);
            nvgFillColor(ctx, es.fgColor);
            nvgFill(ctx);
        }
        if (para_idx != SIZE_MAX) {
            RichLine rl;
            rl.para_idx   = para_idx;
            rl.byte_start = 0;
            rl.byte_end   = 0;
            rl.y_top      = startY;
            rl.y_bottom   = startY + lineHeight + lineSpacing;
            rl.baseline   = baseline;
            rl.x_start    = originX + indent;
            if (mono_bg) {
                rl.mono_bg       = true;
                rl.mono_bg_x     = bg_x;
                rl.mono_bg_y     = bg_y;
                rl.mono_bg_w     = bg_w;
                rl.mono_bg_h     = bg_h;
                rl.mono_bg_color = es.bgColor;
            }
            if (para.isBullet) {
                const float br = std::max(1.5f, lineHeight * 0.10f);
                rl.bullet       = true;
                rl.bullet_cx    = originX + para.leftIndent - br * 3.0f;
                rl.bullet_cy    = baseline - m.ascender * 0.35f;
                rl.bullet_r     = br;
                rl.bullet_color = es.fgColor;
            }
            m_rich_layout.push_back(std::move(rl));
        }
        return startY + lineHeight + lineSpacing;
    };

    if (para.runs.empty())
        return emit_empty_line();

    std::vector<LayoutLine> lines;
    LayoutLine current;

    auto lineIndent = [&]() {
        const bool firstLine = lines.empty();
        return para.leftIndent + (firstLine ? para.firstLineIndent : 0.0f);
    };

    auto flushLine = [&](bool hard) {
        current.hardBreak = hard;
        lines.push_back(std::move(current));
        current = LayoutLine{};
    };

    auto pushWord = [&](const Text& run, std::string_view text,
                        float advance, float leftBearing, float visualRight,
                        size_t bstart, size_t bend) {
        const float sp     = run.style.monospace ? 0.0f
                                                 : spaceWidthFor(ctx, run.style);
        /* Only insert inter-word spacing when the source text actually has
         * whitespace before this word: run boundaries produced by mid-word
         * style toggles (or per-keystroke typing-attribute runs) must not
         * open phantom gaps ("hello" split into runs must not draw as
         * "h e l l o"). Byte offsets are paragraph-global and contiguous
         * across runs, so adjacency is exact. */
        const bool sep = !current.words.empty() &&
                         current.words.back().byte_end != bstart;
        const float indent = lineIndent();
        const float avail  = contentWidth - indent;

        float lsExtra = 0.f;
        if (run.style.letterSpacing != 0.f && !run.isImageRun && text.size()>1) {
            lsExtra = run.style.letterSpacing * (float)(text.size() - 1);
        }
        float effAdv = advance + lsExtra;
        bool noWrap = (run.style.whiteSpace == WhiteSpace::Nowrap);
        const float needed = current.words.empty()
                             ? effAdv
                             : current.advanceWidth + (sep ? sp : 0.0f) + effAdv;
        if (!noWrap && !current.words.empty() && needed > avail)
            flushLine(false);

        bool spaceBefore = false;
        if (!current.words.empty() && sep && !run.style.monospace) {
            current.advanceWidth += sp;
            spaceBefore = true;
        }

        Word w{ &run, std::string(text), advance, leftBearing, visualRight, bstart, bend, spaceBefore };
        w.advance = effAdv; // store letter-spaced advance
        current.words.push_back(std::move(w));
        current.advanceWidth += effAdv;

        if (run.isImageRun) {
            /* Bottom-align to baseline (CSS default vertical-align for
             * inline <img>): the image needs `image_h` of ascent and no
             * descent, not the surrounding text's font metrics. */
            current.ascent = std::max(current.ascent, run.image_h);
        } else {
            const VMetrics m = metricsFor(ctx, run.style);
            current.ascent  = std::max(current.ascent, m.ascender);
            current.descent = std::max(current.descent, -m.descender);
        }
    };

    size_t run_byte_base = 0;
    for (const Text& run : para.runs) {
        if (run.isImageRun) {
            pushWord(run, std::string_view(), run.image_w, 0.0f, run.image_w,
                     run_byte_base, run_byte_base);
            run_byte_base += run.content.size();
            continue;
        }
        bool firstPiece = true;
        forEachLine(run.content, [&](std::string_view piece) {
            if (!firstPiece) flushLine(true);
            firstPiece = false;
            if (piece.empty()) return;

            // byte offset of this piece within the paragraph
            size_t piece_byte_base = run_byte_base +
                                     (size_t)(piece.data() - run.content.data());

            if (run.style.monospace) {
                for (size_t i = 0; i < piece.size(); ++i) {
                    std::string_view ch = piece.substr(i, 1);
                    float adv, lb, vr;
                    measureWord(ctx, run.style, ch.data(),
                                ch.data() + ch.size(), adv, lb, vr);
                    pushWord(run, ch, adv, lb, vr,
                             piece_byte_base + i,
                             piece_byte_base + i + 1);
                }
            } else {
                forEachWord(piece, [&](std::string_view word) {
                    float adv, lb, vr;
                    measureWord(ctx, run.style, word.data(),
                                word.data() + word.size(), adv, lb, vr);
                    size_t wbs = piece_byte_base + (size_t)(word.data() - piece.data());
                    pushWord(run, word, adv, lb, vr, wbs, wbs + word.size());
                });
            }
        });
        run_byte_base += run.content.size();
    }
    if (!current.words.empty()) flushLine(true);
    if (lines.empty()) return emit_empty_line();

    // ----- mono background block width pass --------------------------------
    std::vector<float> blockWidth(lines.size(), 0.0f);
    {
        size_t i = 0;
        while (i < lines.size()) {
            if (lines[i].words.empty() ||
                !lines[i].words.front().run->style.monospace ||
                lines[i].words.front().run->style.bgColor.a == 0) {
                ++i; continue;
            }
            size_t j = i;
            float maxW = 0.0f;
            while (j < lines.size() &&
                   !lines[j].words.empty() &&
                   lines[j].words.front().run->style.monospace &&
                   lines[j].words.front().run->style.bgColor.a > 0) {
                const auto& wfront = lines[j].words.front();
                const auto& wback  = lines[j].words.back();
                const float leftEdge  = wfront.leftBearing;
                const float rightEdge = (lines[j].advanceWidth - wback.advance)
                                        + wback.visualRight;
                maxW = std::max(maxW, rightEdge - leftEdge);
                ++j;
            }
            for (size_t k = i; k < j; ++k) blockWidth[k] = maxW;
            i = j;
        }
    }

    // ----- render every line ------------------------------------------------
    float y = startY;
    const float rightEdge = originX + contentWidth;

    for (size_t li = 0; li < lines.size(); ++li) {
        LayoutLine& line = lines[li];
        const bool isFirstLine = (li == 0);
        const bool isLastLine  = (li + 1 == lines.size()) || line.hardBreak;

        const float indent     = para.leftIndent +
                                 (isFirstLine ? para.firstLineIndent : 0.0f);
        /* Only runs that paint a background (CTA pills, code blocks)
         * reserve pad in the line box.  Inherited TD padding used to
         * inflate every line and clip the fill drawn around the text. */
        float boxPadX = 0.f, boxPadY = 0.f;
        float lineHeightOverride = 0.f;
        bool hasMiddle = false;
        for (const auto& w : line.words) {
            const Style& s = w.run->style;
            if (s.bgColor.a > 0.f) {
                boxPadX = std::max(boxPadX, s.padX + s.borderWidth);
                boxPadY = std::max(boxPadY, s.padY + s.borderWidth);
            }
            if (s.lineHeight > 0.f) lineHeightOverride = std::max(lineHeightOverride, s.lineHeight);
            if (s.verticalMiddle) hasMiddle = true;
        }
        float lineHeight = line.ascent + line.descent + 2.f * boxPadY;
        if (lineHeightOverride > lineHeight) lineHeight = lineHeightOverride;
        float baseline = y + boxPadY + line.ascent;
        // If line-height > metrics or vertical-align:middle on a mixed line
        // (pill icon + text), center the baseline in the line box so the
        // pill fill and icon sit on the same midline.  Superscript keeps its
        // existing raised-ty path and is excluded.
        bool needsCenter = (lineHeightOverride > line.ascent + line.descent + 1e-3f) || hasMiddle;
        // If the line contains an isImageRun, force middle so heart+"1" align.
        if (!needsCenter) for (auto &w : line.words) if (w.run->isImageRun) { needsCenter = true; break; }
        if (needsCenter && !line.words.empty()) {
            bool hasSuper = false; for (auto &w : line.words) if (w.run->style.superscript) hasSuper = true;
            if (!hasSuper) baseline = y + (lineHeight - (line.ascent + line.descent)) * 0.5f + line.ascent;
        }

        float lineX;
        switch (para.alignment) {
            case TextAlignment::Center:
                lineX = originX + (contentWidth - line.advanceWidth) * 0.5f;
                break;
            case TextAlignment::Right:
                lineX = rightEdge - line.advanceWidth;
                break;
            case TextAlignment::Left:
            case TextAlignment::Justify:
            default:
                lineX = originX + indent + boxPadX;
                break;
        }

        float drawX = lineX;
        const bool isMonoBg = !line.words.empty() &&
                              line.words.front().run->style.monospace &&
                              line.words.front().run->style.bgColor.a > 0;
        if (isMonoBg)
            drawX -= line.words.front().leftBearing;

        int numGaps = 0;
        for (size_t wi = 1; wi < line.words.size(); ++wi)
            if (line.words[wi].spaceBefore) ++numGaps;
        const bool justify = (para.alignment == TextAlignment::Justify) &&
                             !isLastLine && numGaps > 0;
        float extraPerGap  = 0.0f;
        if (justify) {
            extraPerGap = (contentWidth - indent - line.advanceWidth) / (float)numGaps;
            if (extraPerGap < 0.0f) extraPerGap = 0.0f;
        }

        // --- build RichLine for layout cache ---
        RichLine rich_line;
        const bool capture_layout = (para_idx != SIZE_MAX);
        if (capture_layout) {
            rich_line.para_idx   = para_idx;
            rich_line.byte_start = line.words.empty() ? 0 : line.words.front().byte_start;
            rich_line.byte_end   = line.words.empty() ? 0 : line.words.back().byte_end;
            rich_line.y_top      = y;
            rich_line.y_bottom   = y + lineHeight + lineSpacing;
            rich_line.baseline   = baseline;
            rich_line.x_start    = lineX;
        }

        if (!line.words.empty()) {
            const Style& fs = line.words.front().run->style;
            if (fs.bgColor.a > 0) {
                float bgL, bgR;
                if (fs.monospace && blockWidth[li] > 0.0f) {
                    bgL = drawX;
                    bgR = bgL + blockWidth[li];
                } else {
                    const Word& wf = line.words.front();
                    const Word& wb = line.words.back();
                    bgL = lineX + wf.leftBearing;
                    const float wbX = lineX + (line.advanceWidth - wb.advance);
                    bgR = wbX + wb.visualRight;
                }
                const float padX = fs.padX > 0.f ? fs.padX : (fs.monospace ? 4.f : 1.f);
                const float padY = fs.padY > 0.f ? fs.padY : 1.f;
                /* lineHeight already includes 2*boxPadY.  Keep the fill
                 * inside the line box — HtmlText::measure() lays out at
                 * (0,0) with layout_only, and the next real paint is the
                 * fast path.  If we skip capturing here, the gold pill
                 * never appears. */
                float bgX, bgY, bgW, bgH;
                if (fs.monospace) {
                    bgX = bgL - padX;
                    bgY = y - padY;
                    bgW = (bgR - bgL) + 2.f * padX;
                    bgH = lineHeight + 2.f * padY;
                } else {
                    bgX = bgL - padX;
                    bgY = y;
                    bgW = (bgR - bgL) + 2.f * padX;
                    bgH = lineHeight;
                }
                float rad = 0.f;
                if (fs.padX > 4.f)
                    rad = std::min(bgW, bgH) * 0.5f;
                if (!layout_only) {
                    nvgBeginPath(ctx);
                    nvgFillColor(ctx, fs.bgColor);
                    if (rad > 0.5f)
                        nvgRoundedRect(ctx, bgX, bgY, bgW, bgH, rad);
                    else
                        nvgRect(ctx, bgX, bgY, bgW, bgH);
                    nvgFill(ctx);
                    if (fs.borderWidth > 0.f && fs.borderColor.a > 0.f) {
                        nvgBeginPath(ctx);
                        if (rad > 0.5f)
                            nvgRoundedRect(ctx, bgX + 0.5f, bgY + 0.5f,
                                           bgW - 1.f, bgH - 1.f,
                                           std::max(0.f, rad - 0.5f));
                        else
                            nvgRect(ctx, bgX + 0.5f, bgY + 0.5f,
                                    bgW - 1.f, bgH - 1.f);
                        nvgStrokeColor(ctx, fs.borderColor);
                        nvgStrokeWidth(ctx, fs.borderWidth);
                        nvgStroke(ctx);
                    }
                }

                if (capture_layout && (fs.monospace || fs.padX > 0.f ||
                                       fs.padY > 0.f || fs.borderWidth > 0.f)) {
                    rich_line.mono_bg        = true;
                    rich_line.mono_bg_x      = bgX;
                    rich_line.mono_bg_y      = bgY;
                    rich_line.mono_bg_w      = bgW;
                    rich_line.mono_bg_h      = bgH;
                    rich_line.mono_bg_radius  = rad;
                    rich_line.mono_bg_color  = fs.bgColor;
                    rich_line.mono_bg_border_w = fs.borderWidth;
                    rich_line.mono_bg_border_color = fs.borderColor;
                }
            }
        }

        // Bullet list marker: a small filled circle left of the first line.
        if (isFirstLine && para.isBullet && !layout_only) {
            const float br = std::max(1.5f, lineHeight * 0.10f);
            const float bcx = originX + indent - br * 3.0f;
            const float bcy = baseline - line.ascent * 0.35f;
            const NVGcolor bc = line.words.empty()
                ? para.ruleColor
                : line.words.front().run->style.fgColor;
            nvgBeginPath(ctx);
            nvgCircle(ctx, bcx, bcy, br);
            nvgFillColor(ctx, bc);
            nvgFill(ctx);
            if (capture_layout) {
                rich_line.bullet       = true;
                rich_line.bullet_cx    = bcx;
                rich_line.bullet_cy    = bcy;
                rich_line.bullet_r     = br;
                rich_line.bullet_color = bc;
            }
        }

        float x = drawX;
        for (size_t wi = 0; wi < line.words.size(); ++wi) {
            Word& word = line.words[wi];
            const Style& st = word.run->style;
            applyFont(ctx, st);

            // Compute actual draw x for this word (handles justified last word)
            float wx = x;
            if (justify && wi + 1 == line.words.size())
                wx = rightEdge - word.visualRight;

            if (word.run->isImageRun) {
                const float ih = word.run->image_h;
                // Center small icons with text's midline when mixed; otherwise bottom-align.
                float imgY = baseline - ih;
                bool mixedLine = false; for (auto &ww : line.words) if (!ww.run->isImageRun) { mixedLine = true; break; }
                bool useMiddle = st.verticalMiddle || (mixedLine && ih <= 48.f);
                if (useMiddle) {
                    // align icon's vertical center with text's x-height center
                    float textMid = baseline - metricsFor(ctx, st).ascender * 0.35f;
                    imgY = textMid - ih * 0.5f;
                }
                if (capture_layout) {
                    WordLayout wl;
                    wl.byte_start     = word.byte_start;
                    wl.byte_end       = word.byte_end;
                    wl.x              = wx;
                    wl.advance        = word.advance;
                    wl.image          = word.run->image;
                    wl.style          = st;
                    wl.style.fontSize = ih; // piggy-back height, like the
                                            // block-image/rule sentinels
                    wl.text           = "\x01IMAGE";
                    rich_line.words.push_back(std::move(wl));
                }
                if (!layout_only)
                    draw_image_block(ctx, word.run->image, wx, imgY,
                                     word.advance, ih);
            } else {
            // Capture word position + draw data for cheap re-paint
            if (capture_layout) {
                WordLayout wl;
                wl.byte_start   = word.byte_start;
                wl.byte_end     = word.byte_end;
                wl.x            = wx;
                wl.advance      = word.advance;
                wl.leftBearing  = word.leftBearing;
                wl.visualRight  = word.visualRight;
                wl.style        = st;
                wl.text         = word.text;
                wl.linkUrl      = word.run->linkUrl;
                if (wl.linkUrl.empty()) wl.linkUrl = st.linkUrl;
                rich_line.words.push_back(std::move(wl));
            }

            if (st.bgColor.a > 0 && !st.monospace &&
                st.padX <= 0.f && st.padY <= 0.f && !layout_only) {
                nvgBeginPath(ctx);
                nvgFillColor(ctx, st.bgColor);
                nvgRect(ctx,
                        wx + word.leftBearing - 1,
                        baseline - metricsFor(ctx, st).ascender - 1,
                        (word.visualRight - word.leftBearing) + 2,
                        metricsFor(ctx, st).lineh + 2);
                nvgFill(ctx);
            }

            if (!layout_only) {
                NVGcolor fg = st.fgColor;
                fg.a = (unsigned char)std::lround((float)fg.a * std::clamp(st.opacity, 0.f, 1.f));
                nvgFillColor(ctx, fg);
                float ty = baseline;
                if (st.superscript)
                    ty = baseline - (line.ascent - metricsFor(ctx, st).ascender);
                else if (st.verticalMiddle) {
                    // center text's cap-height in the line box
                    float fontH = metricsFor(ctx, st).lineh > 1.f ? metricsFor(ctx, st).lineh : (metricsFor(ctx, st).ascender - metricsFor(ctx, st).descender);
                    float centered = y + (lineHeight - fontH) * 0.5f + metricsFor(ctx, st).ascender;
                    if (centered < baseline) ty = centered;
                }
                nvgText(ctx, wx, ty, word.text.c_str(), nullptr);
            }

            if (st.underline && !layout_only) {
                const float ux0 = wx + word.leftBearing;
                const float ux1 = wx + word.visualRight;
                const float uy  = baseline + std::max(1.0f, st.fontSize * 0.08f);
                nvgBeginPath(ctx);
                nvgStrokeColor(ctx, st.fgColor);
                nvgStrokeWidth(ctx, std::max(1.0f, st.fontSize * 0.05f));
                nvgMoveTo(ctx, ux0, uy);
                nvgLineTo(ctx, ux1, uy);
                nvgStroke(ctx);
            }
            if (st.strike && !layout_only) {
                const float sx0 = wx + word.leftBearing;
                const float sx1 = wx + word.visualRight;
                const float sy  = baseline - metricsFor(ctx, st).ascender * 0.35f;
                nvgBeginPath(ctx);
                nvgStrokeColor(ctx, st.fgColor);
                nvgStrokeWidth(ctx, std::max(1.0f, st.fontSize * 0.06f));
                nvgMoveTo(ctx, sx0, sy);
                nvgLineTo(ctx, sx1, sy);
                nvgStroke(ctx);
            }

            if (debugDraw) {
                nvgBeginPath(ctx);
                nvgStrokeColor(ctx, nvgRGBA(0, 200, 0, 200));
                nvgStrokeWidth(ctx, 0.5f);
                nvgRect(ctx,
                        wx + word.leftBearing,
                        baseline - metricsFor(ctx, st).ascender,
                        word.visualRight - word.leftBearing,
                        metricsFor(ctx, st).lineh);
                nvgStroke(ctx);
            }
            }

            if (!(justify && wi + 1 == line.words.size())) {
                x += word.advance;
                if (wi + 1 < line.words.size() &&
                    line.words[wi + 1].spaceBefore) {
                    x += spaceWidthFor(ctx, line.words[wi + 1].run->style);
                    if (justify) x += extraPerGap;
                }
            }
        }
        if (capture_layout)
            m_rich_layout.push_back(std::move(rich_line));

        if (debugDraw && para.alignment == TextAlignment::Justify) {
            nvgBeginPath(ctx);
            nvgStrokeColor(ctx, nvgRGBA(255, 0, 0, 200));
            nvgStrokeWidth(ctx, 1.0f);
            nvgMoveTo(ctx, rightEdge - 6.0f, baseline);
            nvgLineTo(ctx, rightEdge + 1.0f, baseline);
            nvgStroke(ctx);
        }

        y += lineHeight + lineSpacing;
    }

    if (debugDraw) {
        nvgBeginPath(ctx);
        nvgStrokeColor(ctx, nvgRGBA(200, 0, 200, 180));
        nvgStrokeWidth(ctx, 1.0f);
        nvgRect(ctx, originX, startY - 2, contentWidth, y - startY + 2);
        nvgStroke(ctx);
    }

    return y;
}

// ---------------------------------------------------------------------------
// Rich layout hit-testing (used by TextEditor in RichText mode)
// ---------------------------------------------------------------------------

size_t Document::richLineIndex(size_t para_idx, size_t byte_col) const {
    for (size_t li = 0; li < m_rich_layout.size(); ++li) {
        const RichLine& rl = m_rich_layout[li];
        if (rl.para_idx != para_idx) continue;
        bool is_last = (li + 1 >= m_rich_layout.size() ||
                        m_rich_layout[li + 1].para_idx != para_idx);
        if (byte_col >= rl.byte_start && (byte_col < rl.byte_end || is_last))
            return li;
    }
    // Fallback: first line of this paragraph
    for (size_t li = 0; li < m_rich_layout.size(); ++li)
        if (m_rich_layout[li].para_idx == para_idx)
            return li;
    return m_rich_layout.size();
}

std::pair<size_t,size_t>
Document::richHitTest(NVGcontext* ctx, float x, float y) const {
    if (m_rich_layout.empty()) return {0, 0};

    // Clamp to document extents
    if (y < m_rich_layout.front().y_top)
        return {m_rich_layout.front().para_idx,
                m_rich_layout.front().byte_start};
    if (y >= m_rich_layout.back().y_bottom) {
        const RichLine& last = m_rich_layout.back();
        return {last.para_idx, last.byte_end};
    }

    // Find line by y
    const RichLine* found = nullptr;
    for (const auto& rl : m_rich_layout) {
        if (y >= rl.y_top && y < rl.y_bottom) { found = &rl; break; }
    }
    if (!found) {
        float best = 1e9f;
        for (const auto& rl : m_rich_layout) {
            float mid = (rl.y_top + rl.y_bottom) * 0.5f;
            float d = std::abs(y - mid);
            if (d < best) { best = d; found = &rl; }
        }
    }
    if (!found) return {0, 0};
    if (found->words.empty()) return {found->para_idx, found->byte_start};

    size_t para_idx = found->para_idx;

    // Click before first word
    if (x <= found->words.front().x)
        return {para_idx, found->words.front().byte_start};
    // Click after last word
    if (x >= found->words.back().x + found->words.back().advance)
        return {para_idx, found->words.back().byte_end};

    // Find word that contains x
    for (size_t wi = 0; wi < found->words.size(); ++wi) {
        const WordLayout& w = found->words[wi];
        float w_right = w.x + w.advance;

        if (x >= w.x && x < w_right) {
            // Within word: use glyph positions for character precision
            if (para_idx < paragraphs.size()) {
                const Paragraph& para_ref = *paragraphs[para_idx];
                size_t run_byte = 0;
                for (const Text& run : para_ref.runs) {
                    size_t run_end = run_byte + run.content.size();
                    if (w.byte_start >= run_byte && w.byte_end <= run_end) {
                        applyFont(ctx, run.style);
                        const char* wb = run.content.data() + (w.byte_start - run_byte);
                        const char* we = run.content.data() + (w.byte_end   - run_byte);
                        NVGglyphPosition glyphs[512];
                        int n = nvgTextGlyphPositions(ctx, w.x, found->baseline,
                                                      wb, we, glyphs, 512);
                        for (int gi = 0; gi < n; ++gi) {
                            float gnext = (gi + 1 < n)
                                          ? glyphs[gi + 1].x
                                          : (w.x + w.advance);
                            if (x < (glyphs[gi].x + gnext) * 0.5f)
                                return {para_idx,
                                        w.byte_start + (size_t)(glyphs[gi].str - wb)};
                        }
                        return {para_idx, w.byte_end};
                    }
                    run_byte = run_end;
                }
            }
            return {para_idx, w.byte_start};
        }

        // x is in inter-word space
        if (wi + 1 < found->words.size()) {
            const WordLayout& wn = found->words[wi + 1];
            if (x < wn.x) {
                float mid = (w_right + wn.x) * 0.5f;
                return {para_idx, (x < mid) ? w.byte_end : wn.byte_start};
            }
        }
    }
    return {para_idx, found->words.back().byte_end};
}

Document::CaretInfo
Document::richCaretInfo(NVGcontext* ctx, size_t para_idx, size_t byte_col) const {
    CaretInfo info;
    if (m_rich_layout.empty()) return info;

    // Find the visual line for this position
    const RichLine* found = nullptr;
    for (size_t li = 0; li < m_rich_layout.size(); ++li) {
        const RichLine& rl = m_rich_layout[li];
        if (rl.para_idx != para_idx) continue;
        bool is_last = (li + 1 >= m_rich_layout.size() ||
                        m_rich_layout[li + 1].para_idx != para_idx);
        if (byte_col >= rl.byte_start && (byte_col < rl.byte_end || is_last)) {
            found = &rl; break;
        }
    }
    if (!found) {
        for (const auto& rl : m_rich_layout)
            if (rl.para_idx == para_idx) { found = &rl; break; }
    }
    if (!found) return info;

    info.valid    = true;
    info.y_top    = found->y_top;
    info.y_bottom = found->y_bottom;

    if (found->words.empty()) { info.x = found->x_start; return info; }

    /* Measure the width of text bytes [from, to) that produced no layout
     * words (whitespace skipped by the word splitter), summing per-run
     * advances so trailing/inter-run spaces still move the caret. */
    auto measure_gap = [&](size_t from, size_t to) -> float {
        if (to <= from || para_idx >= paragraphs.size()) return 0.f;
        const Paragraph& para_ref = *paragraphs[para_idx];
        float adv = 0.f;
        size_t run_byte = 0;
        for (const Text& run : para_ref.runs) {
            size_t run_end = run_byte + run.content.size();
            size_t lo = std::max(from, run_byte);
            size_t hi = std::min(to, run_end);
            if (lo < hi) {
                applyFont(ctx, run.style);
                float b[4];
                adv += nvgTextBounds(ctx, 0.f, 0.f,
                                     run.content.data() + (lo - run_byte),
                                     run.content.data() + (hi - run_byte), b);
            }
            run_byte = run_end;
        }
        return adv;
    };

    // Before first word
    if (byte_col <= found->words.front().byte_start) {
        info.x = found->words.front().x; return info;
    }
    // After last word (e.g. caret past a trailing space the layout elided)
    if (byte_col >= found->words.back().byte_end) {
        const WordLayout& lw = found->words.back();
        info.x = lw.x + lw.advance + measure_gap(lw.byte_end, byte_col);
        return info;
    }

    // Within or between words
    for (size_t wi = 0; wi < found->words.size(); ++wi) {
        const WordLayout& w = found->words[wi];
        if (byte_col >= w.byte_start && byte_col <= w.byte_end) {
            if (byte_col == w.byte_start) { info.x = w.x; return info; }
            if (byte_col == w.byte_end)   { info.x = w.x + w.advance; return info; }
            // Within word: measure prefix with nvgTextBounds
            if (para_idx < paragraphs.size()) {
                const Paragraph& para_ref = *paragraphs[para_idx];
                size_t run_byte = 0;
                for (const Text& run : para_ref.runs) {
                    size_t run_end = run_byte + run.content.size();
                    if (w.byte_start >= run_byte && w.byte_end <= run_end) {
                        applyFont(ctx, run.style);
                        const char* wb = run.content.data() + (w.byte_start - run_byte);
                        const char* cp = run.content.data() + (byte_col     - run_byte);
                        float b[4];
                        float adv = nvgTextBounds(ctx, 0.f, 0.f, wb, cp, b);
                        info.x = w.x + adv; return info;
                    }
                    run_byte = run_end;
                }
            }
            info.x = w.x; return info;
        }
        // In space between words[wi] and words[wi+1]
        if (wi + 1 < found->words.size()) {
            const WordLayout& wn = found->words[wi + 1];
            if (byte_col > w.byte_end && byte_col < wn.byte_start) {
                float t = (float)(byte_col - w.byte_end) /
                          (float)(wn.byte_start - w.byte_end);
                info.x = (w.x + w.advance) + t * (wn.x - (w.x + w.advance));
                return info;
            }
        }
    }

    info.x = found->x_start;
    return info;
}

NAMESPACE_END(nanogui)
