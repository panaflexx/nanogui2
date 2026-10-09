/*
 * pagemade/page.cpp — see page.h.
 */
#include "page.h"

#include <algorithm>
#include <cmath>

namespace pagemade {

/* ---- Transform ---------------------------------------------------------- */

Transform Transform::rotate(float r) {
    const float cs = std::cos(r), sn = std::sin(r);
    return {cs, sn, -sn, cs, 0, 0};
}

Transform Transform::rotate_about(const Transform &t, Point c, float r) {
    return translate(c.x, c.y) * rotate(r) * translate(-c.x, -c.y) * t;
}

Transform Transform::operator*(const Transform &o) const {
    return {a * o.a + c * o.b,     b * o.a + d * o.b,
            a * o.c + c * o.d,     b * o.c + d * o.d,
            a * o.e + c * o.f + e, b * o.e + d * o.f + f};
}

Transform Transform::inverse() const {
    const float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f)
        return {};
    const float id = 1.f / det;
    return {d * id, -b * id, -c * id, a * id,
            (c * f - d * e) * id, (b * e - a * f) * id};
}

float Transform::rotation() const {
    return std::atan2(b, a);
}

/* ---- Swatches ----------------------------------------------------------- */

std::vector<Swatch> default_swatches() {
    return {
        {kPaper,        "[Paper]",        {1, 1, 1, 1}},
        {kBlack,        "[Black]",        {0, 0, 0, 1}},
        {kRegistration, "[Registration]", {0, 0, 0, 1}},
        {4, "Blue",    {0, 0, 1, 1}},
        {5, "Cyan",    {0, 1, 1, 1}},
        {6, "Green",   {0, 1, 0, 1}},
        {7, "Magenta", {1, 0, 1, 1}},
        {8, "Red",     {1, 0, 0, 1}},
        {9, "Yellow",  {1, 1, 0, 1}},
    };
}

const Swatch *PageDoc::find_swatch(SwatchId id) const {
    for (const Swatch &s : swatches)
        if (s.id == id)
            return &s;
    return nullptr;
}

Color PageDoc::resolve(const Paint &p) const {
    const Swatch *s = p.none() ? nullptr : find_swatch(p.swatch);
    if (!s)
        return {0, 0, 0, 0};
    /* A tint is a screen of the ink over paper white. */
    const float t = std::clamp(p.tint, 0.f, 100.f) * 0.01f;
    return {1 - (1 - s->rgb.r) * t, 1 - (1 - s->rgb.g) * t, 1 - (1 - s->rgb.b) * t, s->rgb.a};
}

SwatchId PageDoc::add_swatch(const std::string &name, Color rgb) {
    SwatchId id = kRegistration;
    for (const Swatch &s : swatches)
        id = std::max(id, s.id);
    swatches.push_back({id + 1, name, rgb});
    return id + 1;
}

/* ---- Lookups ------------------------------------------------------------ */

Item *PageDoc::find_item(ItemId id, size_t *page) {
    return const_cast<Item *>(static_cast<const PageDoc *>(this)->find_item(id, page));
}

const Item *PageDoc::find_item(ItemId id, size_t *page) const {
    for (size_t pi = 0; pi < pages.size(); ++pi)
        for (const Item &it : pages[pi].items)
            if (it.id == id) {
                if (page) *page = pi;
                return &it;
            }
    return nullptr;
}

StoryEntry *PageDoc::find_story(StoryId id) {
    return const_cast<StoryEntry *>(static_cast<const PageDoc *>(this)->find_story(id));
}

const StoryEntry *PageDoc::find_story(StoryId id) const {
    for (const StoryEntry &s : stories)
        if (s.id == id)
            return &s;
    return nullptr;
}

size_t PageDoc::story_index(StoryId id) const {
    for (size_t i = 0; i < stories.size(); ++i)
        if (stories[i].id == id)
            return i;
    return SIZE_MAX;
}

const StoryEntry *PageDoc::story_of(ItemId frame, size_t *thread_index) const {
    for (const StoryEntry &s : stories)
        for (size_t i = 0; i < s.thread.size(); ++i)
            if (s.thread[i] == frame) {
                if (thread_index) *thread_index = i;
                return &s;
            }
    return nullptr;
}

StoryEntry *PageDoc::story_of(ItemId frame, size_t *thread_index) {
    return const_cast<StoryEntry *>(static_cast<const PageDoc *>(this)->story_of(frame, thread_index));
}

std::vector<Frame> PageDoc::thread_frames(const StoryEntry &s) const {
    std::vector<Frame> out;
    for (ItemId id : s.thread) {
        const Item *it = find_item(id);
        out.push_back(it ? Frame{0, 0, it->w, it->h} : Frame{});
    }
    return out;
}

/* ---- Edits -------------------------------------------------------------- */

ItemId PageDoc::add_item(size_t page, Item item) {
    if (page >= pages.size())
        pages.resize(page + 1);
    item.id = next_id++;
    pages[page].items.push_back(std::move(item));
    return pages[page].items.back().id;
}

StoryId PageDoc::add_story(Story story) {
    StoryEntry e;
    e.id = next_id++;
    e.story = std::move(story);
    stories.push_back(std::move(e));
    return stories.back().id;
}

ItemId PageDoc::add_text_frame(size_t page, StoryId story, float w, float h,
                               const Transform &xf, size_t thread_index) {
    Item it;
    it.w = w;
    it.h = h;
    it.xf = xf;
    it.content = TextFrame{};
    ItemId id = add_item(page, std::move(it));
    if (StoryEntry *s = find_story(story)) {
        thread_index = std::min(thread_index, s->thread.size());
        s->thread.insert(s->thread.begin() + (long) thread_index, id);
    }
    return id;
}

void PageDoc::remove_item(ItemId id) {
    for (Page &p : pages)
        p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                     [&](const Item &it) { return it.id == id; }),
                      p.items.end());
    for (StoryEntry &s : stories)
        s.thread.erase(std::remove(s.thread.begin(), s.thread.end(), id), s.thread.end());
    stories.erase(std::remove_if(stories.begin(), stories.end(),
                                 [](const StoryEntry &s) { return s.thread.empty(); }),
                  stories.end());
}

void PageDoc::bring_to_front(ItemId id) {
    for (Page &p : pages) {
        auto it = std::find_if(p.items.begin(), p.items.end(),
                               [&](const Item &i) { return i.id == id; });
        if (it != p.items.end())
            std::rotate(it, it + 1, p.items.end());
    }
}

void PageDoc::send_to_back(ItemId id) {
    for (Page &p : pages) {
        auto it = std::find_if(p.items.begin(), p.items.end(),
                               [&](const Item &i) { return i.id == id; });
        if (it != p.items.end())
            std::rotate(p.items.begin(), it, it + 1);
    }
}

void PageDoc::restack(const std::vector<ItemId> &ids, int dir) {
    if (dir == 0)
        return;
    auto selected = [&](ItemId id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    for (Page &p : pages) {
        if (p.items.size() < 2)
            continue;
        if (dir > 0) {
            /* Walk from the front so a selected run swaps once with the
             * item above it and then stays together. */
            for (size_t i = p.items.size() - 1; i-- > 0;)
                if (selected(p.items[i].id) && !selected(p.items[i + 1].id))
                    std::swap(p.items[i], p.items[i + 1]);
        } else {
            for (size_t i = 1; i < p.items.size(); ++i)
                if (selected(p.items[i].id) && !selected(p.items[i - 1].id))
                    std::swap(p.items[i], p.items[i - 1]);
        }
    }
}

size_t PageDoc::insert_page(size_t index) {
    if (index > pages.size())
        index = pages.size();
    pages.insert(pages.begin() + (long) index, Page{});
    return index;
}

bool PageDoc::remove_page(size_t index) {
    if (index >= pages.size() || pages.size() <= 1)
        return false;
    std::vector<ItemId> ids;
    for (const Item &it : pages[index].items)
        ids.push_back(it.id);
    for (ItemId id : ids)
        remove_item(id);
    if (index < pages.size())
        pages.erase(pages.begin() + (long) index);
    return true;
}

void PageDoc::move_page(size_t from, size_t to) {
    if (from >= pages.size() || to >= pages.size() || from == to)
        return;
    if (to > from)
        std::rotate(pages.begin() + (long) from, pages.begin() + (long) from + 1,
                    pages.begin() + (long) to + 1);
    else
        std::rotate(pages.begin() + (long) to, pages.begin() + (long) from,
                    pages.begin() + (long) from + 1);
}

} // namespace pagemade
