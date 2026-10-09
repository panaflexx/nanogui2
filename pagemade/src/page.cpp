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

} // namespace pagemade
