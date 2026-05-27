/*
    src/texteditor.cpp -- Editable text widget (rich text + code).
*/

#include <nanogui/texteditor.h>
#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/theme.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <GLFW/glfw3.h>
#include <cmath>

NAMESPACE_BEGIN(nanogui)

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TextEditor::TextEditor(Widget* parent, Mode mode)
    : WidgetCRTP<TextEditor>(parent),
      m_doc(std::make_shared<Document>()),
      m_mode(mode),
      m_bg_color(30, 30, 34, 255),
      m_caret_color(220, 220, 220, 255),
      m_selection_color(70, 110, 180, 130),
      m_current_line_color(255, 255, 255, 14),
      m_gutter_color(38, 38, 44, 255),
      m_line_number_color(120, 120, 130, 255),
      m_show_line_numbers(true),
      m_expand_tab(true),
      m_read_only(false),
      m_tab_width(4),
      m_padding(6),
      m_scroll_x(0.f),
      m_scroll_y(0.f),
      m_cached_line_h(0.f),
      m_cached_char_adv(0.f),
      m_metrics_valid(false)
{
    // Rich default style: 16pt sans, near-white on dark bg.
    m_default_style.fontSize = 16.f;
    m_default_style.fgColor  = nvgRGBA(220, 220, 220, 255);
    m_default_style.monospace = false;

    // Code default style: 14pt mono, same color.
    m_code_style.fontSize  = 14.f;
    m_code_style.fgColor   = nvgRGBA(220, 220, 220, 255);
    m_code_style.monospace = true;

    // Document defaults
    m_doc->contentWidth = 600.f;
    m_doc->debugDraw    = false;

    // Start with one empty paragraph so caret has somewhere to live.
    if (m_doc->paragraphs.empty()) {
        m_doc->addParagraph();
    }
}

// ---------------------------------------------------------------------------
// Document / plain-text accessors
// ---------------------------------------------------------------------------

void TextEditor::set_document(std::shared_ptr<Document> doc) {
    m_doc = doc ? std::move(doc) : std::make_shared<Document>();
    if (m_doc->paragraphs.empty())
        m_doc->addParagraph();
    m_caret = m_anchor = Position{0, 0};
    m_scroll_x = m_scroll_y = 0.f;
    m_metrics_valid = false;
}

void TextEditor::set_plain_text(const std::string& text, const Style* style) {
    // Pick a sensible default if none is supplied so the text inherits
    // the widget's configured foreground color / font size / face.
    Style use = style ? *style
                      : (m_mode == Mode::Code ? m_code_style
                                              : m_default_style);
    if (m_mode == Mode::Code)
        use.monospace = true;

    m_doc->paragraphs.clear();
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            auto* p = m_doc->addParagraph();
            if (i > start)
                p->addText(text.substr(start, i - start), use);
            start = i + 1;
        }
    }
    if (m_doc->paragraphs.empty())
        m_doc->addParagraph();
    m_caret = m_anchor = Position{0, 0};
    m_scroll_x = m_scroll_y = 0.f;
}

std::string TextEditor::plain_text() const {
    std::string out;
    for (size_t i = 0; i < m_doc->paragraphs.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out += m_doc->paragraphs[i]->plain_text();
    }
    return out;
}

void TextEditor::set_mode(Mode m) {
    if (m == m_mode) return;
    m_mode = m;
    m_metrics_valid = false;
    if (m_mode == Mode::Code) {
        // Force every run to monospace.
        for (auto& p : m_doc->paragraphs)
            for (auto& r : p->runs) {
                r.style.monospace = true;
                r.style.fontSize  = m_code_style.fontSize;
            }
    }
}

// ---------------------------------------------------------------------------
// Geometry helpers (Code mode)
// ---------------------------------------------------------------------------

void TextEditor::ensure_metrics(NVGcontext* ctx) const {
    if (m_metrics_valid) return;
    nvgFontFace(ctx, "mono");
    nvgFontSize(ctx, m_code_style.fontSize);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
    float ascender, descender, lineh;
    nvgTextMetrics(ctx, &ascender, &descender, &lineh);
    m_cached_line_h = lineh;
    float b[4];
    float w = nvgTextBounds(ctx, 0, 0, "M", nullptr, b);
    m_cached_char_adv = w > 0.f ? w : m_code_style.fontSize * 0.6f;
    m_metrics_valid = true;
}

float TextEditor::line_height(NVGcontext* ctx) const {
    ensure_metrics(ctx);
    return m_cached_line_h;
}

float TextEditor::char_advance(NVGcontext* ctx) const {
    ensure_metrics(ctx);
    return m_cached_char_adv;
}

int TextEditor::gutter_width(NVGcontext* ctx) const {
    if (!m_show_line_numbers) return 0;
    int n = (int)paragraph_count();
    int digits = 1;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 3) digits = 3;
    float adv = char_advance(ctx);
    return (int)std::ceil(adv * (digits + 2));
}

size_t TextEditor::paragraph_count() const {
    return m_doc->paragraphs.empty() ? 1 : m_doc->paragraphs.size();
}

size_t TextEditor::visual_col_for_byte(const std::string& line, size_t byte) const {
    size_t vcol = 0;
    size_t n = std::min(byte, line.size());
    for (size_t i = 0; i < n; ++i) {
        if (line[i] == '\t') {
            vcol += (size_t)m_tab_width - (vcol % m_tab_width);
        } else {
            vcol++;
        }
    }
    return vcol;
}

size_t TextEditor::byte_for_visual_col(const std::string& line, size_t vcol) const {
    size_t cur = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        size_t step = (line[i] == '\t')
                       ? ((size_t)m_tab_width - (cur % m_tab_width))
                       : 1;
        if (cur + step > vcol) return i;
        cur += step;
    }
    return line.size();
}

TextEditor::Position
TextEditor::position_from_point(NVGcontext* ctx, const Vector2i& p) const {
    if (m_mode == Mode::RichText) {
        // Convert widget-relative → logical draw-space (undo nvgTranslate scroll)
        float hit_x = (float)m_pos.x() + (float)p.x();
        float hit_y = (float)m_pos.y() + (float)p.y() + m_scroll_y;
        auto [pi, bc] = m_doc->richHitTest(ctx, hit_x, hit_y);
        if (pi >= paragraph_count()) pi = paragraph_count() - 1;
        size_t lim = m_doc->paragraphs[pi]->byte_length();
        if (bc > lim) bc = lim;
        return Position{pi, bc};
    }

    // p is widget-relative.
    const float lh   = line_height(ctx);
    const float adv  = char_advance(ctx);
    const int   gutW = gutter_width(ctx);

    float fx = (float)p.x() - (float)m_padding - (float)gutW + m_scroll_x;
    float fy = (float)p.y() - (float)m_padding + m_scroll_y;

    long line = (long)std::floor(fy / lh);
    if (line < 0) line = 0;
    if ((size_t)line >= paragraph_count())
        line = (long)paragraph_count() - 1;

    const std::string text = m_doc->paragraphs[(size_t)line]->plain_text();

    long vcol = (long)std::floor((fx + adv * 0.5f) / adv);
    if (vcol < 0) vcol = 0;
    size_t bcol = byte_for_visual_col(text, (size_t)vcol);
    return Position{(size_t)line, bcol};
}

Vector2f TextEditor::point_for_position(NVGcontext* ctx, Position pos) const {
    if (m_mode == Mode::RichText) {
        auto info = m_doc->richCaretInfo(ctx, pos.paragraph, pos.column);
        if (info.valid) {
            // logical → widget-relative: undo m_pos offset and scroll
            float wx = info.x     - (float)m_pos.x();
            float wy = info.y_top - (float)m_pos.y() - m_scroll_y;
            return Vector2f(wx, wy);
        }
        return Vector2f((float)m_padding, (float)m_padding);
    }

    const float lh  = line_height(ctx);
    const float adv = char_advance(ctx);
    const int   gW  = gutter_width(ctx);

    if (pos.paragraph >= paragraph_count())
        pos.paragraph = paragraph_count() - 1;
    const std::string text = m_doc->paragraphs[pos.paragraph]->plain_text();
    if (pos.column > text.size()) pos.column = text.size();
    size_t vcol = visual_col_for_byte(text, pos.column);

    float x = (float)m_padding + (float)gW + adv * (float)vcol - m_scroll_x;
    float y = (float)m_padding + lh * (float)pos.paragraph - m_scroll_y;
    return Vector2f(x, y);
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------

std::pair<TextEditor::Position, TextEditor::Position>
TextEditor::selection() const {
    if (m_caret < m_anchor) return { m_caret, m_anchor };
    return { m_anchor, m_caret };
}

std::string TextEditor::selected_text() const {
    if (!has_selection()) return {};
    auto [a, b] = selection();
    if (a.paragraph == b.paragraph) {
        std::string t = m_doc->paragraphs[a.paragraph]->plain_text();
        return t.substr(a.column, b.column - a.column);
    }
    std::string out;
    out.reserve(64);
    {
        std::string first = m_doc->paragraphs[a.paragraph]->plain_text();
        out += first.substr(a.column);
    }
    for (size_t i = a.paragraph + 1; i < b.paragraph; ++i) {
        out.push_back('\n');
        out += m_doc->paragraphs[i]->plain_text();
    }
    out.push_back('\n');
    out += m_doc->paragraphs[b.paragraph]->plain_text().substr(0, b.column);
    return out;
}

void TextEditor::set_caret(Position p, bool extend_selection) {
    // Clamp
    if (p.paragraph >= paragraph_count())
        p.paragraph = paragraph_count() - 1;
    size_t lim = m_doc->paragraphs[p.paragraph]->byte_length();
    if (p.column > lim) p.column = lim;

    m_caret = p;
    if (!extend_selection) {
		/*
        printf("set_caret ANCHOR RESET to {%zu,%zu} (was anchor={%zu,%zu} caret={%zu,%zu})\n",
               p.paragraph, p.column,
               m_anchor.paragraph, m_anchor.column,
               m_caret.paragraph,  m_caret.column);
		*/
        m_anchor = m_caret;
    }
    if (caret_callback) caret_callback(m_caret);
}

// ---------------------------------------------------------------------------
// Editing primitives
// ---------------------------------------------------------------------------

// Find the (run_index, offset_in_run) for byte column `col`.
namespace {
struct RunHit { size_t run_idx; size_t in_run; };

RunHit hit_run(const Paragraph& p, size_t col) {
    size_t acc = 0;
    for (size_t i = 0; i < p.runs.size(); ++i) {
        size_t n = p.runs[i].content.size();
        if (col <= acc + n) return { i, col - acc };
        acc += n;
    }
    if (p.runs.empty()) return { 0, 0 };
    return { p.runs.size() - 1, p.runs.back().content.size() };
}
} // namespace

void TextEditor::insert_string(Position at, const std::string& s) {
    if (s.empty()) return;
    if (at.paragraph >= paragraph_count())
        at.paragraph = paragraph_count() - 1;

    // Split `s` on '\n'.
    std::vector<std::string> chunks;
    {
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '\n') {
                chunks.emplace_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
    }

    Paragraph* p = m_doc->paragraphs[at.paragraph].get();
    if (p->runs.empty())
        p->addText(std::string{}, m_mode == Mode::Code ? m_code_style
                                                       : m_default_style);

    auto inject = [&](Paragraph* par, size_t col, const std::string& chunk) {
        RunHit h = hit_run(*par, col);
        if (chunk.empty()) return;
        par->runs[h.run_idx].content.insert(h.in_run, chunk);
    };

    if (chunks.size() == 1) {
        inject(p, at.column, chunks[0]);
        m_caret.paragraph = at.paragraph;
        m_caret.column    = at.column + chunks[0].size();
    } else {
        // Cut the current paragraph at `at.column` into head + tail.
        RunHit h = hit_run(*p, at.column);
        std::string tail = p->runs[h.run_idx].content.substr(h.in_run);
        p->runs[h.run_idx].content.erase(h.in_run);
        // Drop any runs after the split point.
        if (p->runs.size() > h.run_idx + 1) {
            // The leftover content becomes the new tail's first run; but we
            // need to gather those runs into the LAST chunk's paragraph.
            // For simplicity we serialize remaining runs as plain text.
            for (size_t i = h.run_idx + 1; i < p->runs.size(); ++i)
                tail += p->runs[i].content;
            p->runs.resize(h.run_idx + 1);
        }

        Style baseStyle = p->runs[h.run_idx].style;

        // Append first chunk to current paragraph.
        p->runs[h.run_idx].content += chunks.front();

        // Create new paragraphs for the middle and last chunks.
        for (size_t i = 1; i < chunks.size(); ++i) {
            Paragraph* np = m_doc->insertParagraph(at.paragraph + i);
            np->addText(chunks[i], baseStyle);
        }

        // Append tail to the last new paragraph.
        Paragraph* last = m_doc->paragraphs[at.paragraph + chunks.size() - 1].get();
        if (!tail.empty()) {
            if (last->runs.empty())
                last->addText(tail, baseStyle);
            else
                last->runs.back().content += tail;
        }

        m_caret.paragraph = at.paragraph + chunks.size() - 1;
        m_caret.column    = chunks.back().size();
    }
    m_anchor = m_caret;
}

void TextEditor::delete_range(Position a, Position b) {
    if (a == b) return;
    if (b < a) std::swap(a, b);
    if (a.paragraph >= paragraph_count()) return;
    if (b.paragraph >= paragraph_count()) {
        b.paragraph = paragraph_count() - 1;
        b.column    = m_doc->paragraphs[b.paragraph]->byte_length();
    }

    if (a.paragraph == b.paragraph) {
        Paragraph* p = m_doc->paragraphs[a.paragraph].get();
        // Walk runs and erase [a.column, b.column).
        size_t acc = 0;
        for (size_t i = 0; i < p->runs.size(); ++i) {
            std::string& s = p->runs[i].content;
            size_t lo = acc, hi = acc + s.size();
            size_t ea = std::max(a.column, lo);
            size_t eb = std::min(b.column, hi);
            if (ea < eb)
                s.erase(ea - lo, eb - ea);
            acc = lo + (hi - lo); // unchanged total; loop uses original size
            if (s.empty() && p->runs.size() > 1) {
                p->runs.erase(p->runs.begin() + (ptrdiff_t)i);
                --i;
                // do not advance acc; we already consumed `lo`
            }
            if (acc >= b.column) {
                // can't break early because acc accounting above is incorrect
                // after run erasure; just continue – it's cheap.
            }
        }
    } else {
        // Multi-paragraph: keep head of `a.paragraph` and tail of `b.paragraph`,
        // discard everything between, then merge.
        Paragraph* pa = m_doc->paragraphs[a.paragraph].get();
        Paragraph* pb = m_doc->paragraphs[b.paragraph].get();

        // Truncate pa to a.column.
        {
            size_t acc = 0;
            for (size_t i = 0; i < pa->runs.size();) {
                std::string& s = pa->runs[i].content;
                size_t hi = acc + s.size();
                if (hi <= a.column) { acc = hi; ++i; continue; }
                if (acc >= a.column) {
                    pa->runs.erase(pa->runs.begin() + (ptrdiff_t)i);
                    continue;
                }
                s.erase(a.column - acc);
                acc = a.column;
                ++i;
            }
        }

        // Build tail of pb starting at b.column.
        std::vector<Text> tail_runs;
        {
            size_t acc = 0;
            for (auto& r : pb->runs) {
                size_t hi = acc + r.content.size();
                if (hi <= b.column) { acc = hi; continue; }
                size_t start = b.column > acc ? (b.column - acc) : 0;
                Text t;
                t.style   = r.style;
                t.content = r.content.substr(start);
                if (!t.content.empty()) tail_runs.push_back(std::move(t));
                acc = hi;
            }
        }

        // Append tail runs to pa.
        for (auto& t : tail_runs) pa->runs.push_back(std::move(t));
        if (pa->runs.empty())
            pa->addText(std::string{}, m_mode == Mode::Code ? m_code_style
                                                            : m_default_style);

        // Erase paragraphs (a.paragraph+1 .. b.paragraph] inclusive.
        m_doc->paragraphs.erase(
            m_doc->paragraphs.begin() + (ptrdiff_t)(a.paragraph + 1),
            m_doc->paragraphs.begin() + (ptrdiff_t)(b.paragraph + 1));
    }

    m_caret  = a;
    m_anchor = a;
}

void TextEditor::fire_changed() {
    m_metrics_valid = false; // line count may have changed (gutter width)
    if (change_callback) change_callback();
    if (caret_callback) caret_callback(m_caret);
}

void TextEditor::insert_text(const std::string& s) {
    if (m_read_only || s.empty()) return;
    if (has_selection()) {
        auto [a, b] = selection();
        delete_range(a, b);
    }
    insert_string(m_caret, s);
    scroll_to_caret();
    fire_changed();
}

void TextEditor::delete_at_caret(bool forward) {
    if (m_read_only) return;
    if (has_selection()) {
        auto [a, b] = selection();
        delete_range(a, b);
        scroll_to_caret();
        fire_changed();
        return;
    }
    Position a = m_caret, b = m_caret;
    if (forward) {
        size_t lim = m_doc->paragraphs[a.paragraph]->byte_length();
        if (a.column < lim) {
            b.column = a.column + 1;
        } else if (a.paragraph + 1 < paragraph_count()) {
            b.paragraph = a.paragraph + 1;
            b.column    = 0;
        } else return;
    } else {
        if (a.column > 0) {
            a.column = a.column - 1;
        } else if (a.paragraph > 0) {
            a.paragraph = a.paragraph - 1;
            a.column    = m_doc->paragraphs[a.paragraph]->byte_length();
        } else return;
    }
    delete_range(a, b);
    scroll_to_caret();
    fire_changed();
}

void TextEditor::insert_newline() {
    if (m_read_only) return;
    if (has_selection()) {
        auto [a, b] = selection();
        delete_range(a, b);
    }
    insert_string(m_caret, "\n");
    scroll_to_caret();
    fire_changed();
}

void TextEditor::insert_tab() {
    if (m_read_only) return;
    if (m_expand_tab) {
        // Visual-column-based: pad to next tab stop.
        const std::string line = m_doc->paragraphs[m_caret.paragraph]->plain_text();
        size_t vcol = visual_col_for_byte(line, m_caret.column);
        size_t pad  = (size_t)m_tab_width - (vcol % m_tab_width);
        insert_text(std::string(pad, ' '));
    } else {
        insert_text("\t");
    }
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

void TextEditor::scroll_to_caret() {
    Screen* s = screen();
    if (!s) return;
    NVGcontext* ctx = s->nvg_context();
    if (!ctx) return;

    float caretXContent, caretYContent, caretH;
    const int gW   = (m_mode == Mode::Code) ? gutter_width(ctx) : 0;
    const int visW = std::max(0, m_size.x() - 2 * m_padding - gW);
    const int visH = std::max(0, m_size.y() - 2 * m_padding);

    if (m_mode == Mode::RichText) {
        auto info = m_doc->richCaretInfo(ctx, m_caret.paragraph, m_caret.column);
        if (!info.valid) return;  // layout not yet populated (first frame)
        // logical → content-space (relative to widget top-left + padding, no scroll)
        caretXContent = info.x     - (float)m_pos.x() - (float)m_padding;
        caretYContent = info.y_top - (float)m_pos.y() - (float)m_padding;
        caretH        = info.y_bottom - info.y_top;
    } else {
        const float lh  = line_height(ctx);
        const float adv = char_advance(ctx);
        const std::string text =
            m_doc->paragraphs[m_caret.paragraph]->plain_text();
        caretXContent = adv * (float)visual_col_for_byte(text, m_caret.column);
        caretYContent = lh  * (float)m_caret.paragraph;
        caretH        = lh;
    }

    // Vertical scroll
    if (caretYContent < m_scroll_y)
        m_scroll_y = caretYContent;
    else if (caretYContent + caretH > m_scroll_y + (float)visH)
        m_scroll_y = caretYContent + caretH - (float)visH;

    // Horizontal scroll (code mode only — rich mode wraps)
    if (m_mode != Mode::RichText) {
        const float adv = char_advance(ctx);
        if (caretXContent < m_scroll_x)
            m_scroll_x = std::max(0.f, caretXContent - adv);
        else if (caretXContent + adv > m_scroll_x + (float)visW)
            m_scroll_x = caretXContent + adv - (float)visW;
        if (m_scroll_x < 0.f) m_scroll_x = 0.f;
    }
    if (m_scroll_y < 0.f) m_scroll_y = 0.f;

    // Kill any coasting so the inertia block in draw() doesn't overwrite
    // the position we just computed, and flag that we set scroll explicitly.
    m_vel_x = 0.0f;
    m_vel_y = 0.0f;
    m_scroll_caret_pending = true;
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

bool TextEditor::mouse_button_event(const Vector2i& p, int button, bool down,
                                    int modifiers) {
    Widget::mouse_button_event(p, button, down, modifiers);
    if (button == GLFW_MOUSE_BUTTON_1 && down) {
        request_focus();
        Screen* s = screen();
        NVGcontext* ctx = s ? s->nvg_context() : nullptr;
        if (ctx) {
            Vector2i rel = p - m_pos;
            Position np = position_from_point(ctx, rel);
            set_caret(np, (modifiers & GLFW_MOD_SHIFT) != 0);
        }
        return true;
    }
    return false;
}

bool TextEditor::mouse_drag_event(const Vector2i& p, const Vector2i& /*rel*/,
                                  int button, int /*modifiers*/) {
    if (!(button & (1 << GLFW_MOUSE_BUTTON_1))) return false;
    Screen* s = screen();
    NVGcontext* ctx = s ? s->nvg_context() : nullptr;
    if (!ctx) return false;
    Vector2i wp = p - m_pos;
    Position np = position_from_point(ctx, wp);
    set_caret(np, /*extend*/ true);
    s->redraw();
    //printf("mouse_drag_event: p=%d,%d np=%zu,%zu\n", p.x(), p.y(), np.paragraph, np.column);
    return true;
}

bool TextEditor::scroll_event(const Vector2i& /*p*/, const Vector2f& rel) {
    const float lh  = m_cached_line_h  > 0.f ? m_cached_line_h  : 16.f;
    const float adv = m_cached_char_adv > 0.f ? m_cached_char_adv : 8.f;
    // Impulse = 24x line/char so inertia totals ~3 lines per notch at tau=125ms
    if (rel.y() != 0.f)
        m_vel_y = std::clamp(m_vel_y - rel.y() * lh * 24.f, -3500.f, 3500.f);
    if (rel.x() != 0.f)
        m_vel_x = std::clamp(m_vel_x - rel.x() * adv * 24.f, -3500.f, 3500.f);
    screen()->redraw();
    return true;
}

bool TextEditor::focus_event(bool focused) {
    Widget::focus_event(focused);
    return true;
}

bool TextEditor::keyboard_event(int key, int scancode, int action, int mods) {
    if (key_filter && key_filter(key, scancode, action, mods))
        return true;

    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return false;

    const bool shift = (mods & GLFW_MOD_SHIFT)   != 0;
    const bool ctrl  = (mods & GLFW_MOD_CONTROL) != 0;

    auto move_h = [&](int delta) {
        Position np = m_caret;
        long c = (long)np.column + delta;
        if (c < 0) {
            if (np.paragraph > 0) {
                np.paragraph--;
                np.column = m_doc->paragraphs[np.paragraph]->byte_length();
            } else {
                np.column = 0;
            }
        } else {
            size_t lim = m_doc->paragraphs[np.paragraph]->byte_length();
            if ((size_t)c > lim) {
                if (np.paragraph + 1 < paragraph_count()) {
                    np.paragraph++;
                    np.column = 0;
                } else {
                    np.column = lim;
                }
            } else {
                np.column = (size_t)c;
            }
        }
        set_caret(np, shift);
        scroll_to_caret();
    };

    auto move_v = [&](int delta) {
        if (m_mode == Mode::RichText && !m_doc->m_rich_layout.empty()) {
            // Move by visual line (a paragraph may wrap into multiple lines)
            Screen* vs = screen();
            NVGcontext* vctx = vs ? vs->nvg_context() : nullptr;
            if (vctx) {
                size_t li = m_doc->richLineIndex(m_caret.paragraph, m_caret.column);
                const auto& rl = m_doc->m_rich_layout;
                long tli = std::clamp((long)li + delta, 0L, (long)rl.size() - 1);
                // Preserve the current x across vertical moves
                auto cur_info = m_doc->richCaretInfo(vctx, m_caret.paragraph, m_caret.column);
                float target_x = cur_info.valid ? cur_info.x
                                                : (float)m_pos.x() + (float)m_padding;
                const Document::RichLine& tline = rl[(size_t)tli];
                float hit_y = (tline.y_top + tline.y_bottom) * 0.5f;
                auto [pi, bc] = m_doc->richHitTest(vctx, target_x, hit_y);
                set_caret(Position{pi, bc}, shift);
                scroll_to_caret();
                return;
            }
        }
        // Code mode (or rich mode before first draw)
        Position np = m_caret;
        long pn = (long)np.paragraph + delta;
        if (pn < 0) pn = 0;
        if (pn >= (long)paragraph_count()) pn = (long)paragraph_count() - 1;
        np.paragraph = (size_t)pn;
        size_t lim = m_doc->paragraphs[np.paragraph]->byte_length();
        if (np.column > lim) np.column = lim;
        set_caret(np, shift);
        scroll_to_caret();
    };

    switch (key) {
        case GLFW_KEY_LEFT:      move_h(-1); return true;
        case GLFW_KEY_RIGHT:     move_h(+1); return true;
        case GLFW_KEY_UP:        move_v(-1); return true;
        case GLFW_KEY_DOWN:      move_v(+1); return true;
        case GLFW_KEY_HOME: {
            Position np = m_caret; np.column = 0;
            set_caret(np, shift); scroll_to_caret(); return true;
        }
        case GLFW_KEY_END: {
            Position np = m_caret;
            np.column = m_doc->paragraphs[np.paragraph]->byte_length();
            set_caret(np, shift); scroll_to_caret(); return true;
        }
        case GLFW_KEY_PAGE_UP:   move_v(-12); return true;
        case GLFW_KEY_PAGE_DOWN: move_v(+12); return true;
        case GLFW_KEY_BACKSPACE: delete_at_caret(false); return true;
        case GLFW_KEY_DELETE:    delete_at_caret(true);  return true;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:  insert_newline(); return true;
        case GLFW_KEY_TAB:       insert_tab();     return true;
        case GLFW_KEY_A:
            if (ctrl) {
                Position start{0, 0};
                Position end{paragraph_count() - 1,
                             m_doc->paragraphs.back()->byte_length()};
                m_anchor = start;
                m_caret  = end;
                if (caret_callback) caret_callback(m_caret);
                return true;
            }
            break;
        default: break;
    }
    return false;
}

bool TextEditor::keyboard_character_event(unsigned int codepoint) {
    if (character_filter && character_filter(codepoint))
        return true;
    if (m_read_only) return false;
    // Encode UTF-32 -> UTF-8.
    char buf[5] = {0};
    if (codepoint < 0x80) {
        buf[0] = (char)codepoint;
    } else if (codepoint < 0x800) {
        buf[0] = (char)(0xC0 | (codepoint >> 6));
        buf[1] = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        buf[0] = (char)(0xE0 | (codepoint >> 12));
        buf[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (codepoint & 0x3F));
    } else {
        buf[0] = (char)(0xF0 | (codepoint >> 18));
        buf[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (codepoint & 0x3F));
    }
    insert_text(buf);
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void TextEditor::draw(NVGcontext* ctx) {
    // ---- Helpers ----
    auto content_height = [&]() -> float {
        if (m_mode == Mode::Code && m_metrics_valid)
            return m_cached_line_h * (float)paragraph_count();
        return m_content_h;   // updated by draw_rich each frame
    };
    auto max_scroll_y = [&]() -> float {
        const float visH = (float)std::max(0, m_size.y() - 2 * m_padding);
        return std::max(0.f, content_height() - visH);
    };

    // ---- Inertia integration ----
    {
        double now = glfwGetTime();
        float  dt  = (m_last_t > 0.0)
                     ? std::min((float)(now - m_last_t), 0.05f) : 0.0f;
        m_last_t = now;
        bool moving = false;

        const float maxY = max_scroll_y();

        if (std::abs(m_vel_y) > 0.5f) {
            m_scroll_y += m_vel_y * dt;
            m_scroll_y  = std::clamp(m_scroll_y, 0.f, maxY);
            if (m_scroll_y <= 0.f || m_scroll_y >= maxY) m_vel_y = 0.f;
            else {
                m_vel_y *= std::exp(-8.0f * dt);
                if (std::abs(m_vel_y) < 0.5f) m_vel_y = 0.f;
            }
            moving = true;
        } else if (!m_scroll_caret_pending) {
            // Only clamp on "free" frames; skip when scroll_to_caret() just
            // set an explicit position (maxY may still be stale this frame).
            m_scroll_y = std::clamp(m_scroll_y, 0.f, maxY);
        }
        m_scroll_caret_pending = false; // consume the flag

        if (std::abs(m_vel_x) > 0.5f) {
            m_scroll_x += m_vel_x * dt;
            if (m_scroll_x < 0.f) { m_scroll_x = 0.f; m_vel_x = 0.f; }
            else {
                m_vel_x *= std::exp(-8.0f * dt);
                if (std::abs(m_vel_x) < 0.5f) m_vel_x = 0.f;
            }
            moving = true;
        } else {
            if (m_scroll_x < 0.f) m_scroll_x = 0.f;
        }

        if (moving) screen()->redraw();
    }

    // ---- Background ----
    nvgBeginPath(ctx);
    nvgRect(ctx, (float)m_pos.x(), (float)m_pos.y(),
                 (float)m_size.x(), (float)m_size.y());
    nvgFillColor(ctx, m_bg_color);
    nvgFill(ctx);

    nvgSave(ctx);
    nvgIntersectScissor(ctx, (float)m_pos.x(), (float)m_pos.y(),
                             (float)m_size.x(), (float)m_size.y());
    if (m_mode == Mode::Code) draw_code(ctx);
    else                       draw_rich(ctx);
    nvgRestore(ctx);

    // ---- Pill-style overlay scrollbar ----
    {
        const float visH     = (float)std::max(0, m_size.y() - 2 * m_padding);
        const float contentH = content_height();
        if (contentH > visH + 1.f) {
            constexpr float SB_W   = 6.f;
            constexpr float SB_M   = 3.f;
            constexpr float SB_MIN = 28.f;
            const float maxY  = contentH - visH;
            const float vis_r = visH / contentH;
            const float th    = std::max(SB_MIN, (float)m_size.y() * vis_r);
            const float track = (float)m_size.y() - th;
            const float frac  = (maxY > 0.f)
                                ? std::clamp(m_scroll_y / maxY, 0.f, 1.f)
                                : 0.f;
            const float ty = (float)m_pos.y() + frac * track;
            const float tx = (float)m_pos.x() + (float)m_size.x() - SB_W - SB_M;
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, tx, ty + 3.f, SB_W, th - 6.f, SB_W * 0.5f);
            nvgFillColor(ctx, nvgRGBA(150, 155, 165, 180));
            nvgFill(ctx);
        }
    }

    Widget::draw(ctx);
}

Vector2i TextEditor::preferred_size(NVGcontext* ctx) const {
    const float lh  = line_height(ctx);
    const float adv = char_advance(ctx);
    int w = (int)std::ceil(adv * 80) + gutter_width(ctx) + 2 * m_padding;

    // Return a modest intrinsic height (~5 lines). FlexItem grow will expand it.
    constexpr int kMinLines = 5;
    int h = (int)std::ceil(lh * kMinLines) + 2 * m_padding;
    return {w, h};
}

// ---------------------------------------------------------------------------
// Code-mode rendering
// ---------------------------------------------------------------------------

void TextEditor::draw_code(NVGcontext* ctx) {
    ensure_metrics(ctx);
    const float lh   = m_cached_line_h;
    const float adv  = m_cached_char_adv;
    const int   gW   = gutter_width(ctx);

    const float originX = (float)m_pos.x() + (float)m_padding + (float)gW
                        - m_scroll_x;
    const float originY = (float)m_pos.y() + (float)m_padding - m_scroll_y;

    // Determine visible line range.
    const float visTop = (float)m_pos.y() + (float)m_padding;
    const float visBot = (float)m_pos.y() + (float)m_size.y() - (float)m_padding;
    long firstLine = (long)std::floor((visTop - originY) / lh);
    long lastLine  = (long)std::ceil ((visBot - originY) / lh);
    if (firstLine < 0) firstLine = 0;
    if (lastLine  > (long)paragraph_count()) lastLine = (long)paragraph_count();

    // Gutter
    if (m_show_line_numbers && gW > 0) {
        nvgBeginPath(ctx);
        nvgRect(ctx, (float)m_pos.x(), (float)m_pos.y(),
                     (float)(m_padding + gW), (float)m_size.y());
        nvgFillColor(ctx, m_gutter_color);
        nvgFill(ctx);

        nvgFontFace(ctx, "mono");
        nvgFontSize(ctx, m_code_style.fontSize);
        nvgFillColor(ctx, m_line_number_color);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
        for (long li = firstLine; li < lastLine; ++li) {
            char num[32];
            std::snprintf(num, sizeof(num), "%ld", li + 1);
            float baseline = originY + (float)li * lh + lh * 0.8f;
            float rightX   = (float)m_pos.x() + (float)m_padding
                           + (float)gW - adv * 0.8f;
            nvgText(ctx, rightX, baseline, num, nullptr);
        }
    }

    // Current line highlight
    if (focused() && m_caret.paragraph < paragraph_count()) {
        float y = originY + (float)m_caret.paragraph * lh;
        if (y + lh > visTop && y < visBot) {
            nvgBeginPath(ctx);
            nvgRect(ctx, (float)m_pos.x() + (float)m_padding + (float)gW,
                         y,
                         (float)m_size.x() - (float)m_padding - (float)gW,
                         lh);
            nvgFillColor(ctx, m_current_line_color);
            nvgFill(ctx);
        }
    }

    // Selection
    if (has_selection()) {
        auto [a, b] = selection();
        nvgFillColor(ctx, m_selection_color);
        for (size_t li = a.paragraph; li <= b.paragraph && li < paragraph_count(); ++li) {
            const std::string ltxt = m_doc->paragraphs[li]->plain_text();
            size_t lo = (li == a.paragraph) ? a.column : 0;
            size_t hi = (li == b.paragraph) ? b.column : ltxt.size();
            size_t vlo = visual_col_for_byte(ltxt, lo);
            size_t vhi = visual_col_for_byte(ltxt, hi);
            float x0 = originX + adv * (float)vlo;
            float x1 = originX + adv * (float)vhi;
            float y  = originY + (float)li * lh;
            if (li < b.paragraph) x1 += adv * 0.5f; // hint for wrap
            nvgBeginPath(ctx);
            nvgRect(ctx, x0, y, std::max(2.f, x1 - x0), lh);
            nvgFill(ctx);
        }
    }

    // Text: one line per paragraph, drawing each run with its own color.
    nvgFontFace(ctx, "mono");
    nvgFontSize(ctx, m_code_style.fontSize);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    for (long li = firstLine; li < lastLine; ++li) {
        const Paragraph& para = *m_doc->paragraphs[(size_t)li];
        const float baseline  = originY + (float)li * lh + lh * 0.8f;

        // Walk runs in order, expanding tabs to spaces at the visual-col
        // level so the caret math agrees with the painted glyphs.
        size_t vcol = 0;
        for (const Text& run : para.runs) {
            nvgFillColor(ctx, run.style.fgColor);
            const std::string& s = run.content;
            size_t i = 0;
            while (i < s.size()) {
                if (s[i] == '\t') {
                    size_t step = (size_t)m_tab_width - (vcol % m_tab_width);
                    vcol += step;
                    i += 1;
                    continue;
                }
                size_t j = i;
                while (j < s.size() && s[j] != '\t') ++j;
                float x = originX + adv * (float)vcol;
                nvgText(ctx, x, baseline,
                        s.data() + i, s.data() + j);
                vcol += (j - i);
                i = j;
            }
        }
    }

    // Caret
    if (focused()) {
        const std::string ctext =
            m_doc->paragraphs[m_caret.paragraph]->plain_text();
        size_t cvcol = visual_col_for_byte(ctext, m_caret.column);
        float cx = originX + adv * (float)cvcol;
        float cy = originY + lh  * (float)m_caret.paragraph;
        nvgBeginPath(ctx);
        nvgRect(ctx, cx, cy, 1.5f, lh);
        nvgFillColor(ctx, m_caret_color);
        nvgFill(ctx);
    }
}

// ---------------------------------------------------------------------------
// Rich-text mode rendering (delegates to Document::draw)
// ---------------------------------------------------------------------------

void TextEditor::draw_rich(NVGcontext* ctx) {
    if (!m_doc) return;
    m_doc->contentWidth = (float)m_size.x() - 2.f * (float)m_padding;
    nvgTranslate(ctx, 0.f, -m_scroll_y);
    m_doc->draw(ctx,
                (float)m_pos.x() + (float)m_padding,
                (float)m_pos.y() + (float)m_padding);
    m_content_h = m_doc->last_drawn_height;

    // ---- Selection highlight (semi-transparent, drawn over text) ----
	/*
    printf("draw_rich: has_sel=%d anchor={%zu,%zu} caret={%zu,%zu} layout_lines=%zu\n",
           (int)has_selection(),
           m_anchor.paragraph, m_anchor.column,
           m_caret.paragraph,  m_caret.column,
           m_doc->m_rich_layout.size());
	*/
    if (has_selection()) {
        auto [sel_a, sel_b] = selection();
		/*
        printf("  sel=[{%zu,%zu}->{%zu,%zu}]\n",
               sel_a.paragraph, sel_a.column,
               sel_b.paragraph, sel_b.column);
		*/
        nvgFillColor(ctx, m_selection_color);
        int rect_count = 0;
        for (const auto& rl : m_doc->m_rich_layout) {
            // Skip lines outside the selection paragraph range
            if (rl.para_idx < sel_a.paragraph || rl.para_idx > sel_b.paragraph)
                continue;

            // Byte range of the selection on this visual line
            size_t line_sel_start, line_sel_end;
            if (rl.para_idx == sel_a.paragraph && rl.para_idx == sel_b.paragraph) {
                line_sel_start = std::max(sel_a.column, rl.byte_start);
                line_sel_end   = std::min(sel_b.column, rl.byte_end);
            } else if (rl.para_idx == sel_a.paragraph) {
                line_sel_start = std::max(sel_a.column, rl.byte_start);
                line_sel_end   = rl.byte_end;
            } else if (rl.para_idx == sel_b.paragraph) {
                line_sel_start = rl.byte_start;
                line_sel_end   = std::min(sel_b.column, rl.byte_end);
            } else {
                // Fully inside selection
                line_sel_start = rl.byte_start;
                line_sel_end   = rl.byte_end;
            }

            if (line_sel_start > line_sel_end) continue;

            float x0, x1;
            if (rl.words.empty()) {
                x0 = rl.x_start;
                x1 = rl.x_start + 4.f;
            } else {
                auto x_for = [&](size_t bc) -> float {
                    auto ci = m_doc->richCaretInfo(ctx, rl.para_idx, bc);
                    return ci.valid ? ci.x : rl.x_start;
                };
                x0 = x_for(line_sel_start);
                x1 = x_for(line_sel_end);
                // For fully-selected lines (not the last paragraph in sel),
                // extend slightly past last word to hint continuation
                bool full_line = (rl.para_idx < sel_b.paragraph &&
                                  line_sel_end == rl.byte_end &&
                                  !rl.words.empty());
                if (full_line)
                    x1 = rl.words.back().x + rl.words.back().advance + 6.f;
            }

            float w = std::max(2.f, x1 - x0);
            /*
			printf("  rect[%d] para=%zu bytes=[%zu,%zu] x=[%.1f,%.1f] y=[%.1f,%.1f]\n",
                   rect_count++, rl.para_idx,
                   line_sel_start, line_sel_end,
                   x0, x1, rl.y_top, rl.y_bottom);
			*/
            nvgBeginPath(ctx);
            nvgRect(ctx, x0, rl.y_top, w, rl.y_bottom - rl.y_top);
            nvgFill(ctx);
        }
        //printf("  total rects drawn: %d\n", rect_count);
    }

    // ---- I-beam caret ----
    if (focused()) {
        auto ci = m_doc->richCaretInfo(ctx, m_caret.paragraph, m_caret.column);
        if (ci.valid) {
            nvgBeginPath(ctx);
            nvgRect(ctx, std::round(ci.x) - 0.75f, ci.y_top,
                    1.5f, ci.y_bottom - ci.y_top);
            nvgFillColor(ctx, m_caret_color);
            nvgFill(ctx);
        }
    }

    nvgTranslate(ctx, 0.f, m_scroll_y);
}

NAMESPACE_END(nanogui)
