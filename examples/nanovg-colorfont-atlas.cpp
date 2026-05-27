// nanovg-colorfont-atlas.cpp
//
// A small rich-text layout demo built on top of NanoGUI / NanoVG.
//
// The Document/Paragraph/Text classes provide a minimal flow layout with
// per-run styling (size, color, bold, italic, underline, monospace, bg color)
// and four alignment modes (Left, Center, Right, Justify).
//
// IMPORTANT measurement note:
//   nvgTextBounds() returns the *horizontal advance* of the measured text as
//   its return value (i.e. where the next glyph would be drawn), while the
//   `bounds` array it writes contains the *visual bounding box* (x0,y0,x1,y1).
//   For laying out a stream of text we therefore use the advance, but for
//   visual elements (background rectangles, underlines, right-flush in
//   justified lines) we use the bounding box. Mixing the two leads to either
//   gaps or overflow at the right margin.

#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/window.h>

#include "fontstash.h"
#include "nanovg.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>

using namespace nanogui;

// ---------------------------------------------------------------------------
// Public document model
// ---------------------------------------------------------------------------

enum class TextAlignment { Left, Center, Right, Justify };

struct Style {
    float    fontSize  = 18.0f;
    NVGcolor fgColor   = nvgRGBA(0, 0, 0, 255);
    NVGcolor bgColor   = nvgRGBA(0, 0, 0, 0);
    bool     bold      = false;
    bool     italic    = false;
    bool     underline = false;
    bool     monospace = false;
};

class Text {
public:
    std::string content;
    Style       style;

    Text(std::string text, const Style& s = Style{})
        : content(std::move(text)), style(s) {}
};

class Paragraph {
public:
    std::vector<Text> runs;
    float             leftIndent      = 0.0f;
    float             firstLineIndent = 0.0f;
    TextAlignment     alignment       = TextAlignment::Left;

    Paragraph() = default;

    void addText(std::string text, const Style& style = Style{}) {
        runs.emplace_back(std::move(text), style);
    }

    void addText(const Text& t) { runs.push_back(t); }
};

// ---------------------------------------------------------------------------
// Layout / rendering
// ---------------------------------------------------------------------------

class Document {
public:
    std::vector<std::unique_ptr<Paragraph>> paragraphs;
    float contentWidth     = 700.0f;   // wrap width in pixels
    float paragraphSpacing = 12.0f;    // gap between paragraphs
    float lineSpacing      = 4.0f;     // extra gap between lines within a paragraph
    bool  debugDraw        = true;    // overlay word/line debug visualisations

    Paragraph* addParagraph() {
        paragraphs.emplace_back(std::make_unique<Paragraph>());
        return paragraphs.back().get();
    }

    Paragraph* addParagraph(std::string text, const Style& style = Style{}) {
        auto* p = addParagraph();
        p->addText(std::move(text), style);
        return p;
    }

    void draw(NVGcontext* ctx, float originX, float originY) {
        // Reset measurement caches each draw call so changes to fonts/sizes
        // don't get stale (cheap; few entries per frame).
        m_spaceCache.clear();
        m_metricsCache.clear();

        // Lock alignment to baseline so vertical math is well-defined.
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        float y = originY;
        for (auto& para : paragraphs) {
            y = drawParagraph(ctx, *para, originX, y);
            y += paragraphSpacing;
        }
    }

private:
    // ---- internal types ---------------------------------------------------

    struct Word {
        const Text* run;        // owning run (style + font)
        std::string text;       // word text (or single mono char)
        float       advance;    // horizontal advance for layout
        float       leftBearing;// bounds[0] when measured at x=0 (signed)
        float       visualRight;// bounds[2] when measured at x=0
    };

    struct LayoutLine {
        std::vector<Word> words;
        float advanceWidth = 0.0f; // sum of advances + inter-word spaces
        float ascent       = 0.0f; // max ascender on this line
        float descent      = 0.0f; // max |descender|
        bool  hardBreak    = false;// terminated by '\n' (no justify)
    };

    // (face name, fontSize quantised) cache keys
    static uint64_t makeKey(const char* face, float size) {
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

    static const char* faceForStyle(const Style& s) {
        if (s.monospace) return "mono";
        if (s.bold && s.italic) return "sans-bolditalic";
        if (s.bold)             return "sans-bold";
        if (s.italic)           return "sans-italic";
        return "sans";
    }

    void applyFont(NVGcontext* ctx, const Style& s) const {
        nvgFontSize(ctx, s.fontSize);
        nvgFontFace(ctx, faceForStyle(s));
    }

    struct VMetrics { float ascender, descender, lineh; };

    VMetrics metricsFor(NVGcontext* ctx, const Style& s) {
        uint64_t k = makeKey(faceForStyle(s), s.fontSize);
        auto it = m_metricsCache.find(k);
        if (it != m_metricsCache.end()) return it->second;
        applyFont(ctx, s);
        VMetrics m{};
        nvgTextMetrics(ctx, &m.ascender, &m.descender, &m.lineh);
        m_metricsCache.emplace(k, m);
        return m;
    }

    float spaceWidthFor(NVGcontext* ctx, const Style& s) {
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

    void measureWord(NVGcontext* ctx, const Style& s, std::string_view text,
                     float& advance, float& leftBearing, float& visualRight) {
        applyFont(ctx, s);
        float b[4];
        const char* begin = text.data();
        const char* end   = begin + text.size();
        advance     = nvgTextBounds(ctx, 0, 0, begin, end, b);
        leftBearing = b[0];
        visualRight = b[2];
    }

    template <typename Fn>
    static void forEachLine(std::string_view s, Fn fn) {
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
    static void forEachWord(std::string_view line, Fn fn) {
        size_t i = 0, n = line.size();
        while (i < n) {
            while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i;
            size_t start = i;
            while (i < n && line[i] != ' ' && line[i] != '\t') ++i;
            if (i > start) fn(line.substr(start, i - start));
        }
    }

    float drawParagraph(NVGcontext* ctx, const Paragraph& para,
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
            const float sp     = run.style.monospace
                                   ? 0.0f
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
                        measureWord(ctx, run.style, ch, adv, lb, vr);
                        pushWord(run, ch, adv, lb, vr);
                    }
                } else {
                    forEachWord(piece, [&](std::string_view word) {
                        float adv, lb, vr;
                        measureWord(ctx, run.style, word, adv, lb, vr);
                        pushWord(run, word, adv, lb, vr);
                    });
                }
            });
        }
        if (!current.words.empty()) flushLine(true);

        if (lines.empty()) return startY;

        std::vector<float> blockWidth(lines.size(), 0.0f);
        {
            size_t i = 0;
            while (i < lines.size()) {
                if (lines[i].words.empty() ||
                    !lines[i].words.front().run->style.monospace ||
                    lines[i].words.front().run->style.bgColor.a == 0) {
                    ++i;
                    continue;
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
            if (isMonoBg) {
                drawX -= line.words.front().leftBearing;
            }

            const int numGaps    = (int)line.words.size() - 1;
            const bool justify   = (para.alignment == TextAlignment::Justify) &&
                                   !isLastLine && numGaps > 0;
            float extraPerGap    = 0.0f;
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

                if (justify && wi + 1 == line.words.size()) {
                    x = rightEdge - word.visualRight;
                }

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

private:
    std::unordered_map<uint64_t, float>    m_spaceCache;
    std::unordered_map<uint64_t, VMetrics> m_metricsCache;
};

// ---------------------------------------------------------------------------
// Simple Markdown-like parser
// ---------------------------------------------------------------------------
static void parseMarkdown(Document& doc, const std::string& markdown) {
    std::istringstream iss(markdown);
    std::string line;
    Paragraph* currentPara = nullptr;
    bool inCodeBlock = false;
    std::string codeBuffer;

    Style normal;
    Style bold = normal; bold.bold = true;
    Style italic = normal; italic.italic = true;
    Style code = normal; code.monospace = true; code.fontSize = 15.0f;
    Style header = normal; header.fontSize = 24.0f; header.bold = true;

    while (std::getline(iss, line)) {
        // Trim trailing whitespace
        while (!line.empty() && std::isspace(line.back())) line.pop_back();

        if (line.empty()) {
            if (inCodeBlock) {
                codeBuffer += "\n";
            } else {
                currentPara = nullptr;
            }
            continue;
        }

        // Code block
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (inCodeBlock) {
                if (!codeBuffer.empty()) {
                    auto* p = doc.addParagraph();
                    p->addText(codeBuffer, code);
                }
                codeBuffer.clear();
                inCodeBlock = false;
            } else {
                inCodeBlock = true;
                codeBuffer.clear();
            }
            continue;
        }

        if (inCodeBlock) {
            codeBuffer += line + "\n";
            continue;
        }

        // Headers
        if (!line.empty() && line[0] == '#') {
            size_t level = 0;
            while (level < line.size() && line[level] == '#') ++level;
            if (level > 0 && level < line.size() && std::isspace(line[level])) {
                std::string text = line.substr(level + 1);
                auto* p = doc.addParagraph();
                p->addText(text, header);
                currentPara = nullptr;
                continue;
            }
        }

        // Start new paragraph
        if (!currentPara) {
            currentPara = doc.addParagraph();
        }

        // --- Robust Inline Parser ---
        const std::string& text = line;
        size_t i = 0;

        while (i < text.size()) {
            // **bold**
            if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
                size_t start = i + 2;
                size_t end = text.find("**", start);
                if (end != std::string::npos) {
                    if (start < end) {
                        currentPara->addText(text.substr(start, end - start), bold);
                    }
                    i = end + 2;
                    continue;
                }
            }
            // *italic*
            else if (text[i] == '*' && (i == 0 || text[i - 1] != '*')) {
                size_t start = i + 1;
                size_t end = text.find('*', start);
                if (end != std::string::npos && end > start) {
                    currentPara->addText(text.substr(start, end - start), italic);
                    i = end + 1;
                    continue;
                }
            }
            // `monospace`
            else if (text[i] == '`') {
                size_t start = i + 1;
                size_t end = text.find('`', start);
                if (end != std::string::npos) {
                    if (start < end) {
                        currentPara->addText(text.substr(start, end - start), code);
                    }
                    i = end + 1;

                    // Force add space after closing backtick
                    if (i < text.size() && std::isspace(text[i])) {
                        currentPara->addText(" ", code);
                        while (i < text.size() && std::isspace(text[i])) ++i;
                    }
                    continue;
                }
            }

            // Normal text
            size_t start = i;
            while (i < text.size() && text[i] != '*' && text[i] != '`') {
                ++i;
            }
            if (i > start) {
                currentPara->addText(text.substr(start, i - start), normal);
            } else {
                ++i;
            }
        }
    }

    // Final code block if still open
    if (inCodeBlock && !codeBuffer.empty()) {
        auto* p = doc.addParagraph();
        p->addText(codeBuffer, code);
    }
}

// ---------------------------------------------------------------------------
// Demo screen
// ---------------------------------------------------------------------------

class AtlasScreen : public Screen {
public:
    std::unique_ptr<Document> doc;
    std::string markdownSource;

    AtlasScreen() : Screen(Vector2i(1000, 800), "NanoGUI Document Layout Demo") {
        // Load fonts
        auto loadFont = [&](const char* alias, const char* path) {
            int id = nvgCreateFont(m_nvg_context, alias, path);
            if (id == -1)
                throw std::runtime_error(std::string("Failed to load font: ") + path);
            return id;
        };

        loadFont("sans",  "resources/Roboto-Regular.ttf");
        loadFont("mono",  "resources/NotoMono-Regular.ttf");
        loadFont("emoji", "resources/NotoColorEmoji.ttf");
        nvgSetEmojiFont(m_nvg_context, "emoji");

        // Default markdown content
        markdownSource = R"MD(
# Main Title

This is a **bold** paragraph with *italic* text and `monospace` code.

## Second Level Header

This paragraph has **bold**, *italic*, and `inline code` all mixed together.

### Third Level Header

- Unordered list item 1
- Unordered list item 2 with **bold**
- Item with `code` and *emphasis*

1. Ordered list item
2. Another item with **bold text**

Here's a code block:

```cpp
#include <iostream>

int main() {
    std::cout << "Hello from Markdown!\n";
    return 0;
}
)MD";

        // Try to load external file if provided
		/*
        if (__argc > 1) {
            std::ifstream f(__argv[1]);
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                markdownSource = ss.str();
            }
        }
		*/

        doc = std::make_unique<Document>();
        doc->contentWidth = 800.0f;
        doc->debugDraw = false;

        parseMarkdown(*doc, markdownSource);

        perform_layout();
    }

    void draw_contents() override {
        nvgBeginPath(m_nvg_context);
        nvgRect(m_nvg_context, 0, 0, (float)width(), (float)height());
        nvgFillColor(m_nvg_context, nvgRGB(245, 245, 245));
        nvgFill(m_nvg_context);

        Screen::draw_contents();

        if (doc) {
            const float margin = 50.0f;
            doc->contentWidth = (float)width() - 2.0f * margin;
            doc->draw(m_nvg_context, margin, 60.0f);
        }
    }
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    try {
        nanogui::init();
        {
            AtlasScreen screen;
            screen.set_visible(true);
            screen.perform_layout();
            nanogui::mainloop();
        }
        nanogui::shutdown();
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return -1;
    }
    return 0;
}

