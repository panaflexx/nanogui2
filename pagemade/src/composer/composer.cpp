/*
 * pagemade/composer/composer.cpp — shaping, line breaking, hyphenation,
 * tabs and justification.
 *
 * Per paragraph:
 *   1. Shape each run with HarfBuzz into glyphs measured in points.
 *   2. Ask UAX #14 (libunibreak) where lines may break and cut the glyphs
 *      into segments: a box (word, or word plus punctuation) and the
 *      spaces after it, which hang past the margin when a line ends there.
 *      Breaks after U+00AD are not segment ends; they are hyphenation
 *      points, like the pattern hyphenator's.
 *   3. Fill each line greedily with pieces (segments, or fragments of a
 *      hyphenated word). Justified lines may shrink word spaces to their
 *      minimum, then letter spacing to its minimum, to fit one more piece.
 *      A piece that doesn't fit is hyphenated at its last point that
 *      fits, shaping both fragments again (the first with a hyphen).
 *   4. Where a line starts or ends at a glyph HarfBuzz flags
 *      unsafe-to-break, shape that piece again on its own.
 *   5. Place the line in the current frame; if its slug doesn't fit, move
 *      to the next frame and break again at that frame's width.
 *
 * Not done yet: a paragraph-at-a-time (Knuth-Plass) composer, and right-
 * to-left text.
 */
#include "composer/composer.h"
#include "composer/hyphenator.h"
#include "composer/utf8.h"

#include <hb.h>
#include <linebreak.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>

namespace pagemade {

namespace {

constexpr float kEps = 0.001f;

enum class Kind : uint8_t { Glyph, Space, Tab, Break, SoftHyphen };

struct PGlyph {
    uint32_t gid = 0;
    uint32_t cluster = 0;
    uint32_t run = 0;
    Kind     kind = Kind::Glyph;
    bool     unsafe = false;     // breaking before this glyph would change shaping
    bool     inserted = false;   // a hyphen added at a break
    float    adv = 0;            // points, horizontal scale applied, no tracking
    float    track = 0;          // tracking added after the glyph, points
    float    dx = 0, dy = 0;     // HarfBuzz offsets, points (dy up)
    float    band = 0;           // the run's space width (letter spacing unit)
    float    scale = 1.f;        // synthetic small caps draw smaller than the run's size
};

/* Latin letters we can case without a Unicode library: ASCII, Latin-1,
 * ß and ÿ. Other scripts are left as typed. */
bool is_lower_letter(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp == 0xDF || cp == 0xFF) return true;
    if (cp >= 0xE0 && cp <= 0xF6) return true;
    if (cp >= 0xF8 && cp <= 0xFE) return true;
    return false;
}

void to_upper(uint32_t cp, std::vector<uint32_t> &out) {
    if (cp >= 'a' && cp <= 'z') { out.push_back(cp - 32); return; }
    if (cp == 0xDF) { out.push_back('S'); out.push_back('S'); return; }
    if (cp == 0xFF) { out.push_back(0x178); return; }
    if ((cp >= 0xE0 && cp <= 0xF6) || (cp >= 0xF8 && cp <= 0xFE)) {
        out.push_back(cp - 0x20);
        return;
    }
    out.push_back(cp);
}

bool is_space(uint32_t cp) { return cp == ' ' || cp == 0xA0; }

float leading_of(const CharStyle &cs, const ParaStyle &ps) {
    return cs.leading > 0 ? cs.leading : cs.size * ps.autoleading * 0.01f;
}

/* ---- Shaping ------------------------------------------------------------ */

class Shaper {
public:
    Shaper(const Paragraph &para, const FontLibrary &fonts) : m_para(para) {
        for (const Run &r : para.runs) {
            m_start.push_back((uint32_t) text.size());
            text += r.text;
            m_font.push_back(fonts.find(r.style.family, r.style.face,
                                         r.style.bold, r.style.italic));
        }
        m_buf = hb_buffer_create();
    }
    ~Shaper() { hb_buffer_destroy(m_buf); }
    Shaper(const Shaper &) = delete;
    Shaper &operator=(const Shaper &) = delete;

    std::string text;

    const Font *font(size_t run) const { return m_font[run]; }
    const CharStyle &style(size_t run) const { return m_para.runs[run].style; }

    /* Shape text[a, b) run by run. With `hyphen`, a '-' is shaped after
     * the last character in its run's font, so it kerns against it. */
    void shape(uint32_t a, uint32_t b, bool hyphen, std::vector<PGlyph> &out) {
        for (size_t ri = 0; ri < m_para.runs.size(); ++ri) {
            uint32_t ra = std::max(a, m_start[ri]);
            uint32_t rb = std::min(b, m_start[ri] + (uint32_t) m_para.runs[ri].text.size());
            if (ra >= rb)
                continue;
            bool h = hyphen && rb == b;
            std::string s = text.substr(ra, rb - ra);
            if (h)
                s += '-';
            shape_text(ri, s, ra, rb - ra, out);
        }
    }

    /* Shape arbitrary text in run `ri`'s style. Glyphs from bytes at or
     * past `inserted_from` are marked inserted; clusters are offset by
     * `base` so they index the paragraph. */
    void shape_text(size_t ri, const std::string &s, uint32_t base, uint32_t inserted_from,
                    std::vector<PGlyph> &out) {
        const CharStyle &cs = m_para.runs[ri].style;
        const Font *font = m_font[ri];
        if (!font || s.empty())
            return;

        /* Caps is a style, not a change to the story. Clusters stay the
         * original byte offsets so carets and copy still see the typed text.
         * Small caps use the face's smcp feature, or capitals drawn at the
         * x-height when the face has none. */
        const bool all_caps = cs.caps == Caps::All;
        const bool synth = cs.caps == Caps::Small && !font->has_small_caps();
        const float sm = synth ? font->small_cap_scale() : 1.f;
        std::vector<float> scale_at(s.size() + 1, 1.f);
        hb_buffer_clear_contents(m_buf);
        /* clear_contents leaves a shaped buffer typed as glyphs. add()
         * requires Unicode (or an empty invalid buffer). */
        hb_buffer_set_content_type(m_buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
        for (size_t i = 0; i < s.size(); ) {
            const uint32_t cp = utf8_decode(s, i);
            const size_t n = std::min(utf8_len(s, i), s.size() - i);
            const bool lower = is_lower_letter(cp);
            std::vector<uint32_t> cps;
            if ((all_caps || synth) && lower)
                to_upper(cp, cps);
            else
                cps.push_back(cp);
            float sc = 1.f;
            if (synth && (lower || cp == '-'))
                sc = sm;
            if (i < scale_at.size())
                scale_at[i] = sc;
            for (uint32_t o : cps)
                hb_buffer_add(m_buf, o, (unsigned) i);
            i += n;
        }
        hb_buffer_set_direction(m_buf, HB_DIRECTION_LTR);
        hb_buffer_set_language(m_buf, hb_language_from_string("en", -1));
        hb_buffer_guess_segment_properties(m_buf);
        hb_buffer_set_cluster_level(m_buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

        hb_feature_t feats[8];
        unsigned nfeat = 0;
        auto feat = [&](const char *tag, uint32_t value) {
            feats[nfeat++] = {hb_tag_from_string(tag, -1), value,
                              HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
        };
        if (!cs.kerning) feat("kern", 0);
        if (!cs.ligatures) { feat("liga", 0); feat("clig", 0); }
        if (cs.caps == Caps::Small && font->has_small_caps())
            feat("smcp", 1);
        hb_shape(font->hb(), m_buf, feats, nfeat);

        unsigned n = 0;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(m_buf, &n);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(m_buf, &n);
        const float em = cs.size / (float) font->units_per_em();
        const float hs = cs.hscale * 0.01f;
        const float band = font->advance(font->space_glyph()) * em * hs;
        const float track = cs.tracking * 0.001f * cs.size;

        for (unsigned i = 0; i < n; ++i) {
            const uint32_t local = info[i].cluster;
            const uint32_t cp = utf8_decode(s, local);
            const float scale = local < scale_at.size() ? scale_at[local] : 1.f;
            const float gem = em * scale;
            PGlyph g;
            g.gid = info[i].codepoint;
            g.cluster = base + std::min(local, inserted_from);
            g.run = (uint32_t) ri;
            g.inserted = local >= inserted_from;
            g.unsafe = hb_glyph_info_get_glyph_flags(&info[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK;
            g.kind = is_break(cp) ? Kind::Break : is_space(cp) ? Kind::Space
                   : cp == '\t' ? Kind::Tab : cp == 0xAD ? Kind::SoftHyphen : Kind::Glyph;
            g.adv = pos[i].x_advance * gem * hs;
            g.track = track;
            g.dx = pos[i].x_offset * gem * hs;
            g.dy = pos[i].y_offset * gem;
            g.band = band;
            g.scale = scale;
            if (g.kind == Kind::Break || g.kind == Kind::SoftHyphen || g.kind == Kind::Tab) {
                g.adv = 0;           // tabs are resolved during layout
                g.track = 0;
            }
            out.push_back(g);
        }
    }

private:
    const Paragraph &m_para;
    std::vector<uint32_t> m_start;
    std::vector<const Font *> m_font;
    hb_buffer_t *m_buf = nullptr;
};

/* ---- Layout of one line's glyphs ---------------------------------------- */

struct Spacing {
    float word_pct, letter_pct;
    float extra_space = 0, extra_gap = 0;
};

struct TabFill {
    size_t tab;              // index of the tab glyph in the line
    float x0, x1;
    std::string leader;
};

struct LineCtx {
    const ParaStyle   *ps;
    const std::string *text;
    float frame_x;
};

/* The first tab stop past x: explicit stops, then defaults every
 * default_tab from the block's left edge. */
TabStop next_stop(const ParaStyle &ps, float frame_x, float x) {
    const TabStop *best = nullptr;
    for (const TabStop &t : ps.tabs)
        if (frame_x + t.pos > x + kEps && (!best || t.pos < best->pos))
            best = &t;
    if (best) {
        TabStop t = *best;
        t.pos += frame_x;
        return t;
    }
    const float dt = ps.default_tab > 1 ? ps.default_tab : 36.f;
    TabStop t;
    t.pos = frame_x + (std::floor((x - frame_x) / dt + kEps) + 1) * dt;
    return t;
}

/* Pen positions for g[0, hang) from x0. Returns the pen x after the last
 * glyph (its tracking excluded). Tab stops are resolved here: left stops
 * move the pen; right, center and decimal stops shift the text after them
 * once its width is known. */
float layout(const std::vector<PGlyph> &g, size_t hang, float x0, const LineCtx &lc,
             const Spacing &sp, std::vector<float> &xs, std::vector<TabFill> *fills) {
    xs.assign(hang, 0.f);
    float x = x0;
    struct Pending {
        bool active = false;
        TabStop stop;
        size_t tab = 0;
        float start_x = 0;
    } pend;

    auto resolve = [&](size_t upto) {
        if (!pend.active)
            return;
        pend.active = false;
        const float seg_w = x - pend.start_x;
        float anchor = seg_w;                  // right: the whole segment ends at the stop
        if (pend.stop.align == TabAlign::Center) {
            anchor = seg_w * 0.5f;
        } else if (pend.stop.align == TabAlign::Decimal) {
            for (size_t k = pend.tab + 1; k < upto; ++k)
                if (!g[k].inserted && (*lc.text)[g[k].cluster] == '.') {
                    anchor = xs[k] - pend.start_x;
                    break;
                }
        }
        const float shift = std::max(0.f, pend.stop.pos - anchor - pend.start_x);
        for (size_t k = pend.tab + 1; k < upto; ++k)
            xs[k] += shift;
        x += shift;
        if (fills)
            fills->push_back({pend.tab, pend.start_x, pend.start_x + shift, pend.stop.leader});
    };

    for (size_t i = 0; i < hang; ++i) {
        const PGlyph &gl = g[i];
        if (gl.kind == Kind::Tab) {
            resolve(i);
            xs[i] = x;
            TabStop stop = next_stop(*lc.ps, lc.frame_x, x);
            if (stop.align == TabAlign::Left) {
                if (fills)
                    fills->push_back({i, x, stop.pos, stop.leader});
                x = stop.pos;
            } else {
                pend.active = true;
                pend.stop = stop;
                pend.tab = i;
                pend.start_x = x;
            }
            continue;
        }
        xs[i] = x;
        switch (gl.kind) {
        case Kind::Space:
            x += gl.adv * sp.word_pct * 0.01f + gl.track + sp.extra_space;
            break;
        case Kind::Glyph:
            x += gl.adv;
            if (i + 1 < hang) {
                x += gl.track;
                if (g[i + 1].kind == Kind::Glyph)
                    x += g[i + 1].band * sp.letter_pct * 0.01f + sp.extra_gap;
            }
            break;
        default:
            break;
        }
    }
    resolve(hang);
    return x;
}

struct Measure {
    float natural = 0;           // at desired spacing, after tabs
    float word_shrink = 0, word_stretch = 0;
    float letter_shrink = 0, letter_stretch = 0;
    int   spaces = 0, gaps = 0;
    bool  has_tab = false;
};

Measure measure(const std::vector<PGlyph> &g, size_t hang, float x0, const LineCtx &lc,
                std::vector<float> &xs) {
    const ParaStyle &ps = *lc.ps;
    Measure m;
    m.natural = layout(g, hang, x0, lc, {ps.word_desired, ps.letter_desired}, xs, nullptr) - x0;
    for (size_t i = 0; i < hang; ++i) {
        const PGlyph &gl = g[i];
        if (gl.kind == Kind::Tab) {
            m.has_tab = true;
        } else if (gl.kind == Kind::Space) {
            ++m.spaces;
            m.word_shrink += gl.adv * (ps.word_desired - ps.word_min) * 0.01f;
            m.word_stretch += gl.adv * (ps.word_max - ps.word_desired) * 0.01f;
        } else if (gl.kind == Kind::Glyph && i + 1 < hang && g[i + 1].kind == Kind::Glyph) {
            ++m.gaps;
            m.letter_shrink += g[i + 1].band * (ps.letter_desired - ps.letter_min) * 0.01f;
            m.letter_stretch += g[i + 1].band * (ps.letter_max - ps.letter_desired) * 0.01f;
        }
    }
    return m;
}

/* ---- Segments and pieces ------------------------------------------------ */

struct Seg {
    size_t   g0 = 0, g1 = 0;     // glyphs in the paragraph
    uint32_t start = 0, stop = 0;
    bool     forced = false;
};

struct Piece {
    std::vector<PGlyph> glyphs;
    uint32_t start = 0, stop = 0;
    bool     forced = false;     // ends with a mandatory break
    bool     hyphen = false;     // ends at a hyphenation point, with an inserted hyphen
    bool     reshaped = false;
    size_t   seg = 0;
};

/* Hang index of the first `n` glyphs: trailing spaces and breaks are excluded. */
size_t prefix_hang(const std::vector<PGlyph> &g, size_t n) {
    while (n > 0 && (g[n - 1].kind == Kind::Space || g[n - 1].kind == Kind::Break))
        --n;
    return n;
}

/* Index of the first trailing glyph that hangs (spaces, a break). */
size_t hang_start(const std::vector<PGlyph> &g) {
    return prefix_hang(g, g.size());
}

void flatten(const std::vector<const Piece *> &pieces, std::vector<PGlyph> &g, size_t &hang) {
    g.clear();
    for (const Piece *p : pieces)
        g.insert(g.end(), p->glyphs.begin(), p->glyphs.end());
    hang = pieces.empty() ? 0 : g.size() - (pieces.back()->glyphs.size() -
                                            hang_start(pieces.back()->glyphs));
}

struct LineCandidate {
    std::vector<Piece>   pieces;
    size_t               next_seg = 0;
    std::optional<Piece> carry;
    bool                 hyphenated = false;
};

class Breaker {
public:
    Breaker(Shaper &sh, const Paragraph &para, const Hyphenator *hy)
        : m_sh(sh), m_para(para), m_hy(hy) {
        sh.shape(0, (uint32_t) sh.text.size(), false, m_glyphs);
        segment();
        m_points.resize(m_segs.size());
    }

    bool done() const { return !m_carry && m_next >= m_segs.size() && !m_need_empty; }

    uint32_t position() const {
        if (m_carry) return m_carry->start;
        if (m_next < m_segs.size()) return m_segs[m_next].start;
        return (uint32_t) m_sh.text.size();
    }

    LineCandidate next_line(float x0, float avail, float frame_x) {
        LineCandidate c;
        c.next_seg = m_next;
        c.carry = m_carry;
        const ParaStyle &ps = m_para.style;
        const bool justify = is_justified(ps.align);
        const LineCtx lc{&ps, &m_sh.text, frame_x};

        /* Accepted pieces stay flattened. A try appends, measures, then drops
         * the extra. The cached measure is the hyphen zone of the line so far. */
        std::vector<PGlyph> flat;
        size_t acc_hang = 0;
        Measure acc;
        bool have_acc = false;

        auto fits = [&](const Piece *extra) {
            const size_t base = flat.size();
            size_t hang = acc_hang;
            if (extra) {
                flat.insert(flat.end(), extra->glyphs.begin(), extra->glyphs.end());
                hang = flat.size() - (extra->glyphs.size() - hang_start(extra->glyphs));
            }
            const Measure m = measure(flat, hang, x0, lc, m_xs);
            flat.resize(base);
            const float shrink = (justify && !m.has_tab) ? m.word_shrink + m.letter_shrink : 0.f;
            return m.natural - shrink <= avail + kEps;
        };
        auto accept = [&](Piece p) {
            flat.insert(flat.end(), p.glyphs.begin(), p.glyphs.end());
            acc_hang = flat.size() - (p.glyphs.size() - hang_start(p.glyphs));
            acc = measure(flat, acc_hang, x0, lc, m_xs);
            have_acc = true;
            c.pieces.push_back(std::move(p));
        };
        auto consume = [&] {
            if (c.carry) c.carry.reset();
            else ++c.next_seg;
        };

        while (c.carry || c.next_seg < m_segs.size()) {
            Piece p = c.carry ? *c.carry : from_seg(c.next_seg);
            if (fits(&p)) {
                consume();
                accept(std::move(p));
                if (c.pieces.back().forced)
                    break;
                continue;
            }

            /* Hyphenate the piece that overflows, at the last point that fits. */
            bool zone_ok = true;
            if (!justify && have_acc)
                zone_ok = avail - acc.natural > ps.hyphen_zone;
            const bool limit_ok = ps.hyphen_limit <= 0 || m_hyphen_run < ps.hyphen_limit;
            if (zone_ok && limit_ok) {
                const std::vector<uint32_t> &pts = points(p.seg);
                for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
                    if (*it <= p.start || *it >= p.stop)
                        continue;
                    Piece pre = make_piece(p.start, *it, true, false, p.seg);
                    if (!fits(&pre))
                        continue;
                    consume();
                    c.carry = make_piece(*it, p.stop, false, p.forced, p.seg);
                    c.pieces.push_back(std::move(pre));
                    c.hyphenated = true;
                    finish(c);
                    return c;
                }
            }

            if (c.pieces.empty()) {
                /* Nothing fits: split at the last cluster boundary that fits,
                 * keeping at least one cluster. Tabs are measured in place;
                 * a tab-free piece is one forward pass at desired spacing. */
                consume();
                const size_t box = hang_start(p.glyphs);
                size_t cut = 0, smallest = 0;
                bool any_tab = false;
                for (size_t i = 0; i < box; ++i)
                    any_tab |= p.glyphs[i].kind == Kind::Tab;
                if (any_tab) {
                    for (size_t i = box; i > 0; --i) {
                        if (i < p.glyphs.size() && p.glyphs[i].cluster == p.glyphs[i - 1].cluster)
                            continue;
                        smallest = i;
                        const Measure m = measure(p.glyphs, prefix_hang(p.glyphs, i), x0, lc, m_xs);
                        const float shrink =
                            (justify && !m.has_tab) ? m.word_shrink + m.letter_shrink : 0.f;
                        if (m.natural - shrink <= avail + kEps) {
                            cut = i;
                            break;
                        }
                    }
                } else {
                    float natural = 0, word_shrink = 0, letter_shrink = 0;
                    size_t measured = 0;
                    bool saw = false;
                    for (size_t i = 1; i <= box; ++i) {
                        if (i < p.glyphs.size() && p.glyphs[i].cluster == p.glyphs[i - 1].cluster)
                            continue;
                        if (!saw) { smallest = i; saw = true; }
                        const size_t hang = prefix_hang(p.glyphs, i);
                        while (measured < hang) {
                            const PGlyph &gl = p.glyphs[measured];
                            if (measured > 0 && p.glyphs[measured - 1].kind == Kind::Glyph) {
                                natural += p.glyphs[measured - 1].track;
                                if (gl.kind == Kind::Glyph) {
                                    natural += gl.band * ps.letter_desired * 0.01f;
                                    letter_shrink += gl.band *
                                        (ps.letter_desired - ps.letter_min) * 0.01f;
                                }
                            }
                            if (gl.kind == Kind::Space) {
                                natural += gl.adv * ps.word_desired * 0.01f + gl.track;
                                word_shrink += gl.adv * (ps.word_desired - ps.word_min) * 0.01f;
                            } else if (gl.kind == Kind::Glyph) {
                                natural += gl.adv;
                            }
                            ++measured;
                        }
                        const float shrink = justify ? word_shrink + letter_shrink : 0.f;
                        if (natural - shrink <= avail + kEps)
                            cut = i;
                    }
                }
                if (cut == 0)
                    cut = smallest;
                if (cut == 0 || cut >= box) {
                    c.pieces.push_back(std::move(p));
                } else {
                    c.pieces.push_back(slice(p, 0, cut));
                    c.carry = slice(p, cut, p.glyphs.size());
                }
            }
            break;
        }
        finish(c);
        return c;
    }

    void commit(LineCandidate &&c) {
        m_need_empty = false;
        if (!c.pieces.empty() && c.pieces.back().forced && !c.carry &&
            c.next_seg >= m_segs.size())
            m_need_empty = true;      // a break at the very end starts an empty line
        m_next = c.next_seg;
        m_carry = std::move(c.carry);
        m_hyphen_run = c.hyphenated ? m_hyphen_run + 1 : 0;
    }

    bool last_after(const LineCandidate &c) const {
        bool need_empty = !c.pieces.empty() && c.pieces.back().forced && !c.carry &&
                          c.next_seg >= m_segs.size();
        return !c.carry && c.next_seg >= m_segs.size() && !need_empty;
    }

private:
    void segment() {
        static std::once_flag once;
        std::call_once(once, init_linebreak);
        const std::string &t = m_sh.text;
        std::vector<char> brks(t.size());
        if (!t.empty())
            set_linebreaks_utf8((const utf8_t *) t.data(), t.size(), "en", brks.data());

        const size_t n = m_glyphs.size();
        size_t g0 = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i + 1 < n && m_glyphs[i + 1].cluster == m_glyphs[i].cluster)
                continue;                      // not the end of a cluster
            uint32_t end = i + 1 < n ? m_glyphs[i + 1].cluster : (uint32_t) t.size();
            if (end >= t.size() || end == 0)
                continue;
            char b = brks[end - 1];
            if (b != LINEBREAK_MUSTBREAK && b != LINEBREAK_ALLOWBREAK)
                continue;
            if (utf8_decode(t, utf8_prev(t, end)) == 0xAD)
                continue;                      // discretionary hyphen: a hyphenation point
            m_segs.push_back({g0, i + 1, m_glyphs[g0].cluster, end, b == LINEBREAK_MUSTBREAK});
            g0 = i + 1;
        }
        /* The loop never records a break at the last byte: `end` there is
         * t.size() and the iteration is skipped. A paragraph that ends in a
         * mandatory break (Shift+Return) still has to force that last
         * segment, so the empty line after it is produced. */
        if (g0 < n) {
            const bool forced = !t.empty() && brks[t.size() - 1] == LINEBREAK_MUSTBREAK;
            m_segs.push_back({g0, n, m_glyphs[g0].cluster, (uint32_t) t.size(), forced});
        }
    }

    Piece from_seg(size_t si) const {
        const Seg &s = m_segs[si];
        Piece p;
        p.glyphs.assign(m_glyphs.begin() + (long) s.g0, m_glyphs.begin() + (long) s.g1);
        p.start = s.start;
        p.stop = s.stop;
        p.forced = s.forced;
        p.seg = si;
        return p;
    }

    Piece make_piece(uint32_t a, uint32_t b, bool hyphen, bool forced, size_t seg) {
        Piece p;
        m_sh.shape(a, b, hyphen, p.glyphs);
        p.start = a;
        p.stop = b;
        p.forced = forced;
        p.hyphen = hyphen;
        p.reshaped = true;
        p.seg = seg;
        return p;
    }

    static Piece slice(const Piece &p, size_t i0, size_t i1) {
        Piece q;
        q.glyphs.assign(p.glyphs.begin() + (long) i0, p.glyphs.begin() + (long) i1);
        q.start = i0 == 0 ? p.start : p.glyphs[i0].cluster;
        q.stop = i1 >= p.glyphs.size() ? p.stop : p.glyphs[i1].cluster;
        q.forced = i1 >= p.glyphs.size() && p.forced;
        q.reshaped = p.reshaped;
        q.seg = p.seg;
        return q;
    }

    bool unsafe_at(uint32_t byte) const {
        auto it = std::lower_bound(m_glyphs.begin(), m_glyphs.end(), byte,
                                   [](const PGlyph &g, uint32_t b) { return g.cluster < b; });
        return it != m_glyphs.end() && it->cluster == byte && it->unsafe;
    }

    /* Shape the line's end pieces again where HarfBuzz says the break
     * would change their shaping. */
    void finish(LineCandidate &c) {
        if (c.pieces.empty())
            return;
        Piece &first = c.pieces.front();
        if (!first.reshaped && first.start > 0 && unsafe_at(first.start))
            first = make_piece(first.start, first.stop, first.hyphen, first.forced, first.seg);
        Piece &last = c.pieces.back();
        if (!last.reshaped && last.stop < m_sh.text.size() && unsafe_at(last.stop))
            last = make_piece(last.start, last.stop, false, last.forced, last.seg);
        if (c.carry && !c.carry->reshaped && unsafe_at(c.carry->start))
            *c.carry = make_piece(c.carry->start, c.carry->stop, false, c.carry->forced,
                                  c.carry->seg);
    }

    /* Hyphenation points of a segment, as byte offsets. Discretionary
     * hyphens win; otherwise each word of five letters or more gets the
     * pattern points, unless it's all capitals (an acronym). */
    const std::vector<uint32_t> &points(size_t si) {
        if (m_points[si])
            return *m_points[si];
        std::vector<uint32_t> out;
        const Seg &s = m_segs[si];
        const std::string &t = m_sh.text;

        struct Cp { char32_t c; uint32_t byte; };
        std::vector<Cp> cps;
        for (uint32_t i = s.start; i < s.stop;) {
            uint32_t cp = utf8_decode(t, i);
            cps.push_back({cp, i});
            i += (uint32_t) utf8_len(t, i);
        }
        for (size_t k = 0; k + 1 < cps.size(); ++k)
            if (cps[k].c == 0xAD)
                out.push_back(cps[k + 1].byte);

        if (out.empty() && m_para.style.hyphenate && m_hy && !m_hy->empty()) {
            for (size_t k = 0; k < cps.size();) {
                if (!is_letter(cps[k].c)) { ++k; continue; }
                size_t e = k;
                std::u32string word;
                bool any_lower = false;
                while (e < cps.size() && is_letter(cps[e].c)) {
                    any_lower |= to_lower(cps[e].c) == cps[e].c;
                    word.push_back(to_lower(cps[e].c));
                    ++e;
                }
                if (word.size() >= 5 && any_lower)
                    for (size_t pt : m_hy->points(word))
                        out.push_back(cps[k + pt].byte);
                k = e;
            }
        }
        m_points[si] = std::move(out);
        return *m_points[si];
    }

    Shaper &m_sh;
    const Paragraph &m_para;
    const Hyphenator *m_hy;
    std::vector<PGlyph> m_glyphs;
    std::vector<Seg> m_segs;
    std::vector<std::optional<std::vector<uint32_t>>> m_points;
    size_t m_next = 0;
    std::optional<Piece> m_carry;
    int  m_hyphen_run = 0;
    bool m_need_empty = false;
    std::vector<float> m_xs;
};

/* ---- Placing a line ----------------------------------------------------- */

void place_line(Shaper &sh, const Paragraph &para, const LineCandidate &c, float frame_x,
                bool last_line, ComposedLine &line) {
    const ParaStyle &ps = para.style;
    const LineCtx lc{&ps, &sh.text, frame_x};
    std::vector<const Piece *> ptrs;
    for (const Piece &p : c.pieces) ptrs.push_back(&p);
    std::vector<PGlyph> g;
    size_t hang;
    flatten(ptrs, g, hang);

    std::vector<float> xs;
    const Measure m = measure(g, hang, line.left, lc, xs);
    const float avail = line.width;
    line.natural = m.natural;
    line.hyphenated = !c.pieces.empty() && c.pieces.back().hyphen;

    /* Lines with tabs are set flush left. The last line of a justified
     * paragraph is set left, centered or right (by the alignment) unless
     * it only fits by tightening; force justify stretches it as well. */
    const bool justify = !m.has_tab &&
        (ps.align == Align::ForceJustify ||
         (is_justified(ps.align) && (!last_line || m.natural > avail + kEps)));
    const bool right = ps.align == Align::Right || ps.align == Align::JustifyRight;
    const bool center = ps.align == Align::Center || ps.align == Align::JustifyCenter;

    Spacing sp{ps.word_desired, ps.letter_desired};
    float x0 = line.left;
    if (justify) {
        float word_r = 0, letter_r = 0;
        float diff = avail - m.natural;
        if (diff >= 0) {
            if (m.word_stretch > 0 && diff <= m.word_stretch) {
                word_r = diff / m.word_stretch;
            } else {
                word_r = m.word_stretch > 0 ? 1.f : 0.f;
                float rem = diff - m.word_stretch;
                if (m.letter_stretch > 0) {
                    letter_r = std::min(1.f, rem / m.letter_stretch);
                    rem -= letter_r * m.letter_stretch;
                }
                if (rem > kEps) {
                    if (m.spaces > 0)
                        sp.extra_space = rem / m.spaces;
                    else if (ps.align == Align::ForceJustify && m.gaps > 0)
                        sp.extra_gap = rem / m.gaps;
                    line.loose = true;
                }
            }
        } else {
            float need = -diff;
            if (m.word_shrink > 0 && need <= m.word_shrink) {
                word_r = -need / m.word_shrink;
            } else {
                word_r = m.word_shrink > 0 ? -1.f : 0.f;
                float rem = need - m.word_shrink;
                letter_r = m.letter_shrink > 0 ? -std::min(1.f, rem / m.letter_shrink) : 0.f;
                line.tight = rem > m.letter_shrink + kEps;
            }
        }
        sp.word_pct = ps.word_desired +
            (word_r >= 0 ? word_r * (ps.word_max - ps.word_desired)
                         : word_r * (ps.word_desired - ps.word_min));
        sp.letter_pct = ps.letter_desired +
            (letter_r >= 0 ? letter_r * (ps.letter_max - ps.letter_desired)
                           : letter_r * (ps.letter_desired - ps.letter_min));
    } else {
        if (!m.has_tab && right)
            x0 += avail - m.natural;
        else if (!m.has_tab && center)
            x0 += (avail - m.natural) * 0.5f;
        if (m.natural > avail + kEps)
            line.tight = true;
    }

    std::vector<TabFill> fills;
    const float x_end = layout(g, hang, x0, lc, sp, xs, &fills);
    line.x_start = x0;
    line.x_end = x_end;

    /* Emit glyphs grouped by run. Hanging spaces and the break are kept,
     * invisible, past x_end so carets can sit on them. */
    std::vector<size_t> run_of(g.size());
    int cur = -1;
    float hang_x = x_end;
    for (size_t i = 0; i < g.size(); ++i) {
        const PGlyph &gl = g[i];
        const CharStyle &cs = sh.style(gl.run);
        const float gsize = cs.size * (gl.scale > 0.f ? gl.scale : 1.f);
        const bool split = line.runs.empty() || (int) gl.run != cur ||
                           std::fabs(line.runs.back().size - gsize) > 0.01f;
        if (split) {
            line.runs.emplace_back();
            GlyphRun &r = line.runs.back();
            r.font = sh.font(gl.run);
            r.size = gsize;
            r.hscale = cs.hscale;
            r.color = cs.color;
            r.underline = cs.underline;
            r.strike = cs.strike;
            cur = gl.run;
        }
        run_of[i] = line.runs.size() - 1;

        PlacedGlyph pg;
        pg.gid = gl.gid;
        pg.cluster = gl.cluster;
        float x;
        if (i < hang) {
            x = xs[i];
            pg.adv = (i + 1 < hang ? xs[i + 1] : x_end) - x;
        } else {
            x = hang_x;
            pg.adv = gl.kind == Kind::Space ? gl.adv * ps.word_desired * 0.01f + gl.track : 0.f;
            hang_x += pg.adv;
            pg.flags |= PlacedGlyph::Hanging | PlacedGlyph::Invisible;
        }
        if (gl.inserted)
            pg.flags |= PlacedGlyph::Inserted;
        if (gl.kind == Kind::Tab || gl.kind == Kind::Break || gl.kind == Kind::SoftHyphen)
            pg.flags |= PlacedGlyph::Invisible;
        pg.x = x + gl.dx;
        pg.y = line.baseline - gl.dy - cs.baseline_shift;
        line.runs.back().glyphs.push_back(pg);
    }

    /* Tab leaders: the leader glyph repeated on a grid from the block's
     * left edge, so leaders line up from one line to the next. */
    for (const TabFill &f : fills) {
        if (f.leader.empty() || f.x1 - f.x0 < 1)
            continue;
        const PGlyph &tab = g[f.tab];
        std::vector<PGlyph> lg;
        sh.shape_text(tab.run, f.leader, tab.cluster, 0, lg);
        float step = 0;
        for (const PGlyph &q : lg) step += q.adv;
        if (step <= 0.1f)
            continue;
        const float y = line.baseline - sh.style(tab.run).baseline_shift;
        for (float x = frame_x + std::ceil((f.x0 + step * 0.5f - frame_x) / step) * step;
             x + step <= f.x1 - step * 0.25f; x += step) {
            float pen = x;
            for (const PGlyph &q : lg) {
                PlacedGlyph pg;
                pg.gid = q.gid;
                pg.cluster = tab.cluster;
                pg.x = pen + q.dx;
                pg.y = y - q.dy;
                pg.adv = q.adv;
                pg.flags = PlacedGlyph::Inserted;
                line.runs[run_of[f.tab]].glyphs.push_back(pg);
                pen += q.adv;
            }
        }
    }
}

} // namespace

std::string paragraph_text(const Paragraph &p) {
    std::string t;
    for (const Run &r : p.runs)
        t += r.text;
    return t;
}

Composition compose(const Story &story, const std::vector<Frame> &frames,
                    const FontLibrary &fonts, const Hyphenator *hyphenator) {
    auto t0 = std::chrono::steady_clock::now();
    Composition out;
    size_t fi = 0;
    float y = frames.empty() ? 0.f : frames[0].y;
    bool at_frame_top = true;

    auto finish = [&] {
        out.compose_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return out;
    };

    for (size_t pi = 0; pi < story.paragraphs.size(); ++pi) {
        const Paragraph &para = story.paragraphs[pi];
        const ParaStyle &st = para.style;
        Shaper sh(para, fonts);
        Breaker br(sh, para, hyphenator);

        if (!at_frame_top)
            y += st.space_before;

        const CharStyle empty_style = para.runs.empty() ? CharStyle() : para.runs.front().style;
        bool first_line = true;
        do {
            for (;;) {
                if (fi >= frames.size()) {
                    out.overset = true;
                    out.overset_para = pi;
                    out.overset_byte = br.position();
                    return finish();
                }
                const Frame &f = frames[fi];
                ComposedLine line;
                line.frame = fi;
                line.para = pi;
                line.left = f.x + st.left_indent + (first_line ? st.first_indent : 0.f);
                line.width = f.w - st.left_indent - st.right_indent -
                             (first_line ? st.first_indent : 0.f);

                LineCandidate c = br.next_line(line.left, line.width, f.x);

                /* The last line takes the last-line indent. When the line
                 * no longer fits there, the break comes earlier and the
                 * words left over make a new, indented last line. */
                if (st.last_indent != 0.f && br.last_after(c) &&
                    line.width - st.last_indent > 0.f) {
                    LineCandidate in = br.next_line(line.left + st.last_indent,
                                                    line.width - st.last_indent, f.x);
                    if (br.last_after(in)) {
                        line.left += st.last_indent;
                        line.width -= st.last_indent;
                    }
                    c = std::move(in);
                }

                /* Slug height: the largest leading on the line, hanging
                 * spaces included. */
                float leading = 0;
                for (const Piece &p : c.pieces)
                    for (const PGlyph &g : p.glyphs)
                        leading = std::max(leading, leading_of(sh.style(g.run), st));
                if (leading == 0)
                    leading = leading_of(empty_style, st);
                leading = std::max(0.f, leading * std::clamp(st.line_spacing, kMinLineSpacing,
                                                             kMaxLineSpacing) +
                                            st.extra_spacing);

                if (y + leading > f.y + f.h + kEps) {
                    ++fi;
                    if (fi < frames.size())
                        y = frames[fi].y;
                    at_frame_top = true;
                    continue;
                }

                line.top = y;
                line.leading = leading;
                line.baseline = y + leading * (2.f / 3.f);
                line.para_end = br.last_after(c);
                line.byte_start = c.pieces.empty() ? br.position() : c.pieces.front().start;
                line.byte_end = c.pieces.empty() ? br.position() : c.pieces.back().stop;
                const bool last_line = line.para_end ||
                                       (!c.pieces.empty() && c.pieces.back().forced);
                if (c.pieces.empty()) {
                    const bool center = st.align == Align::Center ||
                                        st.align == Align::JustifyCenter;
                    const bool right = st.align == Align::Right || st.align == Align::JustifyRight;
                    line.x_start = line.x_end = center ? line.left + line.width * 0.5f :
                                                right  ? line.left + line.width : line.left;
                } else {
                    place_line(sh, para, c, f.x, last_line, line);
                }
                br.commit(std::move(c));
                out.lines.push_back(std::move(line));

                y += leading;
                at_frame_top = false;
                first_line = false;
                break;
            }
        } while (!br.done());

        y += st.space_after;
    }
    return finish();
}

} // namespace pagemade
