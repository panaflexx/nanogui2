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
    return it->get();
}

bool Document::removeParagraph(size_t index) {
    if (index >= paragraphs.size()) return false;
    paragraphs.erase(paragraphs.begin() + (ptrdiff_t)index);
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
    // Reset measurement caches each draw call so font/size changes are
    // picked up. They're cheap (a handful of entries per frame).
    m_spaceCache.clear();
    m_metricsCache.clear();

    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    float y = originY;
    for (auto& para : paragraphs) {
        if (para->isRule) {
            // Draw a horizontal separator line; uses half paragraph-spacing
            // as padding above and below so it sits comfortably in the flow.
            float pad = paragraphSpacing * 0.5f;
            float ry  = y + pad;
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, originX, ry);
            nvgLineTo(ctx, originX + contentWidth * 0.5f, ry);
            nvgStrokeColor(ctx, para->ruleColor);
            nvgStrokeWidth(ctx, para->ruleThickness);
            nvgStroke(ctx);
            y = ry + pad;
        } else {
            y = drawParagraph(ctx, *para, originX, y);
            y += paragraphSpacing;
        }
    }
    last_drawn_height = y - originY;
}

// ---------------------------------------------------------------------------
// Paragraph layout + draw  (identical to nanovg-colorfont-atlas.cpp)
// ---------------------------------------------------------------------------

float Document::drawParagraph(NVGcontext* ctx, const Paragraph& para,
                              float originX, float startY) {
    if (para.runs.empty()) return startY;

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
                        float advance, float leftBearing, float visualRight) {
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

        Word w{ &run, std::string(text), advance, leftBearing, visualRight };
        current.words.push_back(std::move(w));
        current.advanceWidth += advance;

        const VMetrics m = metricsFor(ctx, run.style);
        current.ascent  = std::max(current.ascent, m.ascender);
        current.descent = std::max(current.descent, -m.descender);
    };

    for (const Text& run : para.runs) {
        bool firstPiece = true;
        forEachLine(run.content, [&](std::string_view piece) {
            if (!firstPiece) flushLine(true);
            firstPiece = false;
            if (piece.empty()) return;

            if (run.style.monospace) {
                for (size_t i = 0; i < piece.size(); ++i) {
                    std::string_view ch = piece.substr(i, 1);
                    float adv, lb, vr;
                    measureWord(ctx, run.style, ch.data(),
                                ch.data() + ch.size(), adv, lb, vr);
                    pushWord(run, ch, adv, lb, vr);
                }
            } else {
                forEachWord(piece, [&](std::string_view word) {
                    float adv, lb, vr;
                    measureWord(ctx, run.style, word.data(),
                                word.data() + word.size(), adv, lb, vr);
                    pushWord(run, word, adv, lb, vr);
                });
            }
        });
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

        float x = drawX;
        for (size_t wi = 0; wi < line.words.size(); ++wi) {
            Word& word = line.words[wi];
            const Style& st = word.run->style;
            applyFont(ctx, st);

            if (justify && wi + 1 == line.words.size())
                x = rightEdge - word.visualRight;

            if (st.bgColor.a > 0 && !st.monospace) {
                nvgBeginPath(ctx);
                nvgFillColor(ctx, st.bgColor);
                nvgRect(ctx,
                        x + word.leftBearing - 1,
                        baseline - metricsFor(ctx, st).ascender - 1,
                        (word.visualRight - word.leftBearing) + 2,
                        metricsFor(ctx, st).lineh + 2);
                nvgFill(ctx);
            }

            nvgFillColor(ctx, st.fgColor);
            nvgText(ctx, x, baseline, word.text.c_str(), nullptr);

            if (st.underline) {
                const float ux0 = x + word.leftBearing;
                const float ux1 = x + word.visualRight;
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
                        x + word.leftBearing,
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

NAMESPACE_END(nanogui)
