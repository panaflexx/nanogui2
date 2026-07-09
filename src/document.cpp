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

} // namespace

// ---------------------------------------------------------------------------
// Top-level draw
// ---------------------------------------------------------------------------

void Document::draw(NVGcontext* ctx, float originX, float originY) {
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    const bool need_layout =
        m_layout_dirty ||
        m_laid_content_width != contentWidth ||
        m_laid_origin_x != originX ||
        m_laid_origin_y != originY ||
        m_rich_layout.empty();

    if (need_layout) {
        // Full reflow + paint (measures with nvgTextBounds).
        m_rich_layout.clear();
        m_layout_origin_x = originX;
        m_layout_origin_y = originY;

        float y = originY;
        for (size_t pi = 0; pi < paragraphs.size(); ++pi) {
            auto& para = paragraphs[pi];
            if (para->isRule) {
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

                nvgBeginPath(ctx);
                nvgMoveTo(ctx, originX, ry);
                nvgLineTo(ctx, originX + contentWidth * 0.5f, ry);
                nvgStrokeColor(ctx, para->ruleColor);
                nvgStrokeWidth(ctx, para->ruleThickness);
                nvgStroke(ctx);
                y = ry + pad;
            } else {
                y = drawParagraph(ctx, *para, originX, y, pi);
                y += paragraphSpacing;
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

    // ---- Fast path: content/width/origin unchanged (selection drag, hover).
    // Replay from the layout cache — no nvgTextBounds reflow.
    for (const RichLine& rl : m_rich_layout) {
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
        for (const WordLayout& wl : rl.words) {
            const Style& st = wl.style;
            applyFont(ctx, st);
            if (st.bgColor.a > 0 && !st.monospace) {
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
            nvgText(ctx, wl.x, rl.baseline, wl.text.c_str(), nullptr);
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
    if (para.runs.empty()) {
        if (para_idx != SIZE_MAX) {
            RichLine rl;
            rl.para_idx   = para_idx;
            rl.byte_start = 0;
            rl.byte_end   = 0;
            rl.y_top      = startY;
            rl.y_bottom   = startY + lineSpacing;
            rl.baseline   = startY;
            rl.x_start    = originX;
            m_rich_layout.push_back(std::move(rl));
        }
        return startY;
    }

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
        const float indent = lineIndent();
        const float avail  = contentWidth - indent;

        const float needed = current.words.empty()
                             ? advance
                             : current.advanceWidth + sp + advance;
        if (!current.words.empty() && needed > avail)
            flushLine(false);

        if (current.words.empty()) {
            current.advanceWidth = 0.0f;
        } else if (!run.style.monospace) {
            current.advanceWidth += sp;
        }

        Word w{ &run, std::string(text), advance, leftBearing, visualRight, bstart, bend };
        current.words.push_back(std::move(w));
        current.advanceWidth += advance;

        const VMetrics m = metricsFor(ctx, run.style);
        current.ascent  = std::max(current.ascent, m.ascender);
        current.descent = std::max(current.descent, -m.descender);
    };

    size_t run_byte_base = 0;
    for (const Text& run : para.runs) {
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
    if (lines.empty()) return startY;

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
        const float lineHeight = line.ascent + line.descent;
        const float baseline   = y + line.ascent;

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
                lineX = originX + indent;
                break;
        }

        float drawX = lineX;
        const bool isMonoBg = !line.words.empty() &&
                              line.words.front().run->style.monospace &&
                              line.words.front().run->style.bgColor.a > 0;
        if (isMonoBg)
            drawX -= line.words.front().leftBearing;

        const int  numGaps = (int)line.words.size() - 1;
        const bool justify = (para.alignment == TextAlignment::Justify) &&
                             !isLastLine && numGaps > 0;
        float extraPerGap  = 0.0f;
        if (justify) {
            extraPerGap = (contentWidth - indent - line.advanceWidth) / (float)numGaps;
            if (extraPerGap < 0.0f) extraPerGap = 0.0f;
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
                const float pad = 4.0f;
                nvgBeginPath(ctx);
                nvgFillColor(ctx, fs.bgColor);
                nvgRect(ctx, bgL - pad, y - 1,
                        (bgR - bgL) + 2 * pad, lineHeight + 6);
                nvgFill(ctx);
            }
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

        float x = drawX;
        for (size_t wi = 0; wi < line.words.size(); ++wi) {
            Word& word = line.words[wi];
            const Style& st = word.run->style;
            applyFont(ctx, st);

            // Compute actual draw x for this word (handles justified last word)
            float wx = x;
            if (justify && wi + 1 == line.words.size())
                wx = rightEdge - word.visualRight;

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
                rich_line.words.push_back(std::move(wl));
            }

            if (st.bgColor.a > 0 && !st.monospace) {
                nvgBeginPath(ctx);
                nvgFillColor(ctx, st.bgColor);
                nvgRect(ctx,
                        wx + word.leftBearing - 1,
                        baseline - metricsFor(ctx, st).ascender - 1,
                        (word.visualRight - word.leftBearing) + 2,
                        metricsFor(ctx, st).lineh + 2);
                nvgFill(ctx);
            }

            nvgFillColor(ctx, st.fgColor);
            nvgText(ctx, wx, baseline, word.text.c_str(), nullptr);

            if (st.underline) {
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

            if (!(justify && wi + 1 == line.words.size())) {
                x += word.advance;
                if (wi + 1 < line.words.size() && !st.monospace) {
                    x += spaceWidthFor(ctx, st);
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

    // Before first word
    if (byte_col <= found->words.front().byte_start) {
        info.x = found->words.front().x; return info;
    }
    // After last word
    if (byte_col >= found->words.back().byte_end) {
        const WordLayout& lw = found->words.back();
        info.x = lw.x + lw.advance; return info;
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
