/*
 * pagemade/vector.cpp — the vector loader, the store, and placement.
 * The SVG decoder itself is plugin/svg.cpp; this file only calls it.
 */
#include "vector.h"

#include "image.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pagemade {

namespace {

float clamp01(float a) {
    if (a < 0.f)
        return 0.f;
    if (a > 1.f)
        return 1.f;
    return a;
}

} // namespace

void VectorLoader::add(std::unique_ptr<VectorDecoder> decoder) {
    if (decoder)
        m_decoders.push_back(std::move(decoder));
}

const VectorDecoder *VectorLoader::match(const uint8_t *data, size_t n) const {
    for (const auto &d : m_decoders)
        if (d->sniff(data, n))
            return d.get();
    return nullptr;
}

bool VectorLoader::decode(const uint8_t *data, size_t n, VectorScene *out, std::string *error) const {
    const VectorDecoder *d = match(data, n);
    if (!d) {
        if (error)
            *error = "no vector decoder accepted this file";
        return false;
    }
    return d->decode(data, n, out, error);
}

VectorLoader &vector_loader() {
    static VectorLoader loader;
    static bool ready = (register_svg_decoder(loader), true);
    (void) ready;
    return loader;
}

std::vector<std::pair<std::string, std::string>> vector_file_types() {
    return {{"svg", "SVG"}};
}

std::string vector_place_note(const std::string &name, const std::vector<std::string> &skipped) {
    std::string s = "placed " + (name.empty() ? std::string("graphic") : name);
    if (skipped.empty())
        return s;
    s += " (";
    for (size_t i = 0; i < skipped.size(); ++i) {
        if (i)
            s += ", ";
        s += skipped[i];
    }
    s += " skipped)";
    return s;
}

VectorStore::VectorStore(VectorStore &&o) noexcept
    : m_entries(std::move(o.m_entries)), m_next_id(o.m_next_id) {
    o.m_next_id = 0;
}

VectorStore &VectorStore::operator=(VectorStore &&o) noexcept {
    if (this != &o) {
        m_entries = std::move(o.m_entries);
        m_next_id = o.m_next_id;
        o.m_next_id = 0;
    }
    return *this;
}

void VectorStore::clear() {
    m_entries.clear();
    m_next_id = 0;
}

uint32_t VectorStore::take_id() { return ++m_next_id; }

void VectorStore::note_id(uint32_t id) {
    if (id > m_next_id)
        m_next_id = id;
}

bool VectorStore::add(uint32_t id, std::vector<uint8_t> bytes, VectorScene scene) {
    if (id == 0)
        return false;
    Entry e;
    e.source = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    e.scene = std::move(scene);
    note_id(id);
    m_entries[id] = std::move(e);
    return true;
}

bool VectorStore::add(uint32_t id, std::vector<uint8_t> bytes, std::string *error) {
    if (id == 0) {
        if (error)
            *error = "graphic id is missing";
        return false;
    }
    VectorScene scene;
    if (!vector_loader().decode(bytes.data(), bytes.size(), &scene, error))
        return false;
    return add(id, std::move(bytes), std::move(scene));
}

void VectorStore::set_path(uint32_t id, const std::string &path) {
    auto it = m_entries.find(id);
    if (it != m_entries.end())
        it->second.path = path;
}

const VectorStore::Entry *VectorStore::find(uint32_t id) const {
    auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : &it->second;
}

VectorStore::Bytes VectorStore::source_of(uint32_t id) const {
    const Entry *e = find(id);
    return e ? e->source : Bytes();
}

const VectorScene *VectorStore::scene(uint32_t id) const {
    const Entry *e = find(id);
    return e && e->loaded() ? &e->scene : nullptr;
}

void vector_frame(const PageSetup &s, size_t page, float gw, float gh, const Point *at,
                  float &x, float &y, float &w, float &h) {
    float left = 0.f, right = 0.f;
    page_side_margins(s, page, left, right);
    const int n = std::max(1, s.columns);
    const float live = std::max(1.f, s.width - left - right);
    const float gutter = n > 1 ? std::min(s.gutter, live / (float) (n - 1) * 0.5f) : 0.f;
    const float col_w = std::max(1.f, (live - gutter * (float) (n - 1)) / (float) n);
    int col = 0;
    if (at) {
        float best = 1e30f;
        for (int k = 0; k < n; ++k) {
            const float mid = left + (float) k * (col_w + gutter) + col_w * 0.5f;
            const float d = std::fabs(at->x - mid);
            if (d < best) {
                best = d;
                col = k;
            }
        }
    }
    const float col_x = left + (float) col * (col_w + gutter);
    w = gw > 0.f ? gw : 36.f;
    h = gh > 0.f ? gh : 36.f;
    const float stated_w = w;
    if (w > col_w && w > 0.f) {
        h *= col_w / w;
        w = col_w;
    }
    const float live_h = std::max(36.f, s.height - s.margin_top - s.margin_bottom);
    if (h > live_h && h > 0.f) {
        w *= live_h / h;
        h = live_h;
    }
    const bool fitted = w < stated_w - 0.01f;
    x = fitted ? col_x + (col_w - w) * 0.5f : (at ? at->x : left);
    const float y0 = at ? at->y : s.margin_top;
    const float lowest = s.height - s.margin_bottom - h;
    y = std::max(s.margin_top, std::min(y0, std::max(s.margin_top, lowest)));
}

VectorPlacement place_svg_bytes(PageDoc &doc, VectorStore &store, std::vector<uint8_t> bytes,
                                const std::string &name, const std::string &source,
                                size_t page, const Point *at) {
    VectorPlacement r;
    if (page >= doc.pages.size()) {
        r.error = "no such page";
        return r;
    }
    if (bytes.empty()) {
        r.error = "the SVG is empty";
        return r;
    }
    VectorScene scene;
    if (!vector_loader().decode(bytes.data(), bytes.size(), &scene, &r.error)) {
        if (r.error.empty())
            r.error = "couldn't read the SVG";
        return r;
    }
    if (!(scene.width > 0.f) || !(scene.height > 0.f)) {
        r.error = "the SVG has no size";
        return r;
    }
    const uint32_t id = store.take_id();
    const std::string leaf = name.empty() ? std::string("graphic.svg") : name;
    if (!store.add(id, bytes, scene)) {
        r.error = "couldn't keep the SVG";
        return r;
    }
    VectorAsset asset;
    asset.id = id;
    asset.name = leaf;
    asset.source = source;
    asset.width = scene.width;
    asset.height = scene.height;
    asset.skipped = scene.skipped;
    asset.source_size = bytes.size();
    asset.hash = image_bytes_hash(bytes.data(), bytes.size());
    doc.add_vector(asset);
    float x, y, w, h;
    vector_frame(doc.setup, page, scene.width, scene.height, at, x, y, w, h);
    Item it;
    it.w = w;
    it.h = h;
    it.xf = Transform::translate(x, y);
    PlacedVector pv;
    pv.asset = id;
    pv.w = w;
    pv.h = h;
    it.content = pv;
    r.item = doc.add_item(page, std::move(it));
    r.asset = id;
    r.note = vector_place_note(leaf, scene.skipped);
    r.ok = true;
    return r;
}

void append_placed_vector(DrawList &out, const PageDoc &, const Item &it, const VectorScene &scene) {
    const PlacedVector *pv = it.vector();
    if (!pv || !(scene.width > 0.f) || !(scene.height > 0.f))
        return;
    const float sx = pv->w / scene.width;
    const float sy = pv->h / scene.height;
    const float ss = std::sqrt(std::fabs(sx * sy));
    const float opacity = std::clamp(pv->opacity, 0.f, 100.f) / 100.f;
    auto map_pt = [&](float x, float y) { return Point{pv->x + x * sx, pv->y + y * sy}; };
    for (const VectorShape &sh : scene.shapes) {
        Path path;
        path.reserve(sh.path.size());
        for (PathCmd c : sh.path) {
            if (c.kind == PathCmd::Quad || c.kind == PathCmd::Cubic) {
                const Point c1 = map_pt(c.cx1, c.cy1);
                c.cx1 = c1.x;
                c.cy1 = c1.y;
            }
            if (c.kind == PathCmd::Cubic) {
                const Point c2 = map_pt(c.cx2, c.cy2);
                c.cx2 = c2.x;
                c.cy2 = c2.y;
            }
            if (c.kind != PathCmd::Close) {
                const Point p = map_pt(c.x, c.y);
                c.x = p.x;
                c.y = p.y;
            }
            path.push_back(c);
        }
        if (!sh.fill.none()) {
            DrawFill f;
            f.path = path;
            f.color = sh.fill.color;
            f.color.a = clamp01(f.color.a * opacity);
            f.xf = it.xf;
            f.rule = sh.rule;
            f.clip_w = it.w;
            f.clip_h = it.h;
            out.push_back(std::move(f));
        }
        if (!sh.stroke.none()) {
            DrawStroke s;
            s.path = path;
            s.color = sh.stroke.paint.color;
            s.color.a = clamp01(s.color.a * opacity);
            s.width = sh.stroke.width * (ss > 1e-8f ? ss : 1.f);
            s.dash = sh.stroke.dash;
            for (float &d : s.dash)
                d *= ss;
            s.dash_offset = sh.stroke.dash_offset * ss;
            s.cap = sh.stroke.cap;
            s.join = sh.stroke.join;
            s.xf = it.xf;
            s.clip_w = it.w;
            s.clip_h = it.h;
            out.push_back(std::move(s));
        }
    }
}

} // namespace pagemade
