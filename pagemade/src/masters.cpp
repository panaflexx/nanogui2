/*
 * pagemade/masters.cpp — master pages, sections and page labels (page.h).
 *
 * A page shows its master through copies: sync_masters makes a copy of
 * every item on the master's page (the side the page sits on) at the
 * bottom of the Master layer, marked with the master item's id. Copies are
 * locked (item_editable) until overridden; an overridden copy is the
 * page's own from then on, and a sync leaves it be. The master items a page
 * has let go of (an overridden copy deleted, or detached) are listed in
 * Page::master_hidden, so they don't come back until the page's local
 * overrides are removed.
 *
 * A master based on another is synced the same way first, parents before
 * children, so its pages hold the parent's items as copies too, and a page
 * copying it gets both.
 */
#include "page.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace pagemade {

/* ---- Numbers ------------------------------------------------------------- */

std::string format_page_number(int n, PageNumbering style) {
    switch (style) {
    case PageNumbering::RomanLower:
    case PageNumbering::RomanUpper: {
        if (n < 1 || n > 3999)
            break;
        static const struct { int v; const char *s; } kRoman[] = {
            {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
            {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
        std::string out;
        for (const auto &r : kRoman)
            while (n >= r.v) {
                out += r.s;
                n -= r.v;
            }
        if (style == PageNumbering::RomanUpper)
            for (char &c : out)
                c = (char) (c - 'a' + 'A');
        return out;
    }
    case PageNumbering::AlphaLower:
    case PageNumbering::AlphaUpper: {
        if (n < 1)
            break;
        /* a .. z, then aa .. zz, aaa ...: the letter repeated */
        const char base = style == PageNumbering::AlphaUpper ? 'A' : 'a';
        const int letter = (n - 1) % 26, times = (n - 1) / 26 + 1;
        return std::string((size_t) std::min(times, 64), (char) (base + letter));
    }
    case PageNumbering::Arabic:
        break;
    }
    return std::to_string(n);
}

const Section &PageDoc::section_at(size_t index, size_t *first) const {
    static const Section kDefault;
    if (editing_master || pages.empty()) {
        if (first) *first = 0;
        return kDefault;
    }
    index = std::min(index, pages.size() - 1);
    for (size_t i = index + 1; i-- > 0;)
        if (pages[i].section_start) {
            if (first) *first = i;
            return pages[i].section;
        }
    if (first) *first = 0;
    return kDefault;
}

std::string PageDoc::page_label(size_t index) const {
    if (editing_master) {
        const MasterPage *m = find_master(editing_master);
        return m ? m->prefix : std::string("A");
    }
    size_t first = 0;
    const Section &s = section_at(index, &first);
    return s.prefix + format_page_number(s.start + (int) (index - first), s.style);
}

std::string PageDoc::section_name(size_t index) const {
    if (editing_master)
        return "Section";                // a stand-in, as InDesign shows it
    return section_at(index).name;
}

PageMargins PageDoc::page_margins(size_t index) const {
    PageMargins out;
    float inside = setup.margin_inside, outside = setup.margin_outside;
    out.top = setup.margin_top;
    out.bottom = setup.margin_bottom;
    out.columns = setup.columns;
    out.gutter = setup.gutter;
    const MasterPage *m = master_of(index);
    if (m && m->own_margins) {
        out.top = m->margin_top;
        out.bottom = m->margin_bottom;
        inside = m->margin_inside;
        outside = m->margin_outside;
        out.columns = m->columns;
        out.gutter = m->gutter;
    }
    const bool verso = is_verso(index);  // a left-hand page: inside on the right
    out.left = verso ? outside : inside;
    out.right = verso ? inside : outside;
    out.columns = std::max(1, out.columns);
    return out;
}

PageSetup PageDoc::setup_for(size_t index) const {
    const PageMargins m = page_margins(index);
    PageSetup s = setup;
    s.margin_top = m.top;
    s.margin_bottom = m.bottom;
    s.margin_inside = m.left;
    s.margin_outside = m.right;
    s.columns = m.columns;
    s.gutter = m.gutter;
    s.facing = false;
    return s;
}

bool PageDoc::is_verso(size_t index) const {
    if (!setup.facing)
        return false;
    if (editing_master) {
        const MasterPage *m = find_master(editing_master);
        return m && m->spread() && index == 0;
    }
    return index % 2 == 1;
}

/* ---- Masters ------------------------------------------------------------- */

const MasterPage *PageDoc::find_master(MasterId id) const {
    if (!id)
        return nullptr;
    for (const MasterPage &m : masters)
        if (m.id == id)
            return &m;
    return nullptr;
}

MasterPage *PageDoc::find_master(MasterId id) {
    return const_cast<MasterPage *>(static_cast<const PageDoc *>(this)->find_master(id));
}

const std::vector<Page> &PageDoc::master_pages(const MasterPage &m) const {
    return editing_master == m.id ? pages : m.pages;
}

const MasterPage *PageDoc::master_of(size_t index) const {
    if (editing_master)
        return find_master(editing_master);
    return index < pages.size() ? find_master(pages[index].master) : nullptr;
}

LayerId PageDoc::master_layer() const {
    for (const Layer &l : layers)
        if (l.master)
            return l.id;
    return 0;
}

LayerId PageDoc::ensure_master_layer() {
    if (LayerId id = master_layer())
        return id;
    if (layers.size() >= kMaxLayers)
        return 0;
    Layer l;
    l.id = next_layer++;
    while (find_layer(l.id))
        l.id = next_layer++;
    l.name = "Master";
    l.master = true;
    layers.insert(layers.begin(), l);
    sort_by_layer();
    return l.id;
}

std::string PageDoc::next_master_prefix() const {
    auto used = [&](const std::string &p) {
        for (const MasterPage &m : masters)
            if (m.prefix == p)
                return true;
        return false;
    };
    for (int times = 1; times < 8; ++times)
        for (char c = 'A'; c <= 'Z'; ++c) {
            const std::string p((size_t) times, c);
            if (!used(p))
                return p;
        }
    return "M" + std::to_string(next_master);
}

MasterId PageDoc::add_master(const std::string &name, const std::string &prefix, bool spread,
                             MasterId based_on) {
    if (!ensure_master_layer())
        return 0;
    MasterPage m;
    m.id = next_master++;
    while (find_master(m.id))
        m.id = next_master++;
    m.name = name.empty() ? std::string("Master") : name;
    m.prefix = prefix.empty() ? next_master_prefix() : prefix;
    m.based_on = find_master(based_on) ? based_on : 0;
    m.pages.resize(spread ? 2 : 1);
    m.margin_top = setup.margin_top;
    m.margin_bottom = setup.margin_bottom;
    m.margin_inside = setup.margin_inside;
    m.margin_outside = setup.margin_outside;
    m.columns = setup.columns;
    m.gutter = setup.gutter;
    masters.push_back(std::move(m));
    return masters.back().id;
}

/* Take `gone` out of every thread; stories left with no frames go. */
static void drop_from_threads(PageDoc &doc, const std::set<ItemId> &gone) {
    if (gone.empty())
        return;
    for (StoryEntry &s : doc.stories) {
        const size_t before = s.thread.size();
        s.thread.erase(std::remove_if(s.thread.begin(), s.thread.end(),
                                      [&](ItemId id) { return gone.count(id) != 0; }),
                       s.thread.end());
        if (s.thread.empty() && before)
            s.thread.push_back(0);       // marked: emptied here
    }
    doc.stories.erase(std::remove_if(doc.stories.begin(), doc.stories.end(),
                                     [](const StoryEntry &s) {
                                         return s.thread.size() == 1 && s.thread[0] == 0;
                                     }),
                      doc.stories.end());
}

bool PageDoc::remove_master(MasterId id, bool keep_items, MasterId replacement) {
    if (editing_master)
        return false;
    end_linked_edit();
    auto mi = std::find_if(masters.begin(), masters.end(),
                           [&](const MasterPage &m) { return m.id == id; });
    if (mi == masters.end())
        return false;
    if (replacement == id || !find_master(replacement))
        replacement = 0;
    if (replacement)
        keep_items = false;              // else the new master's copies double them
    for (Page &p : pages) {
        if (p.master != id)
            continue;
        p.master = replacement;
        p.master_hidden.clear();
        for (Item &it : p.items)
            if (it.master_item && keep_items) {
                it.master_item = 0;
                it.overridden = false;
            }
    }
    std::set<ItemId> gone;
    for (const Page &p : mi->pages)
        for (const Item &it : p.items)
            gone.insert(it.id);
    masters.erase(mi);
    drop_from_threads(*this, gone);
    for (MasterPage &m : masters)
        if (m.based_on == id)
            m.based_on = 0;
    if (default_master == id)
        default_master = replacement;
    sync_masters();
    return true;
}

size_t PageDoc::master_pages_with_content(MasterId id) const {
    size_t n = 0;
    for (const Page &p : pages) {
        if (p.master != id)
            continue;
        n += std::any_of(p.items.begin(), p.items.end(),
                         [](const Item &it) { return !it.master_item || it.overridden; });
    }
    return n;
}

size_t PageDoc::master_side(size_t index, const MasterPage &m) const {
    if (!m.spread())
        return 0;
    if (!setup.facing)
        return 1;
    return index % 2 == 1 ? 0 : 1;       // page 1 is a right-hand page
}

namespace {

/* Make `target` show `source` (null: no master): copies that aren't
 * overridden are made again, at the bottom, on `layer`. Removed items go
 * into `gone` (for the threads). */
void sync_page(PageDoc &doc, Page &target, const Page *source, LayerId layer,
               std::set<ItemId> &gone) {
    std::set<ItemId> source_ids;
    if (source)
        for (const Item &it : source->items)
            source_ids.insert(it.id);
    /* Out with the old copies; an overridden one whose master item is
     * gone is the page's own now */
    std::set<ItemId> kept;               // master items an overridden copy stands for
    std::vector<Item> rest;
    rest.reserve(target.items.size());
    for (Item &it : target.items) {
        if (it.master_item && !it.overridden) {
            gone.insert(it.id);
            continue;
        }
        if (it.master_item && !source_ids.count(it.master_item)) {
            it.master_item = 0;
            it.overridden = false;
        }
        if (it.master_item)
            kept.insert(it.master_item);
        rest.push_back(std::move(it));
    }
    target.master_hidden.erase(std::remove_if(target.master_hidden.begin(),
                                              target.master_hidden.end(),
                                              [&](ItemId id) { return !source_ids.count(id); }),
                               target.master_hidden.end());
    std::vector<Item> copies;
    std::map<ItemId, ItemId> new_id;
    if (source) {
        std::map<uint32_t, uint32_t> new_group;
        auto remap_group = [&](uint32_t g) {
            if (!g)
                return 0u;
            auto found = new_group.find(g);
            if (found == new_group.end())
                found = new_group.emplace(g, doc.next_id++).first;
            return found->second;
        };
        for (const Item &o : source->items) {
            if (kept.count(o.id) ||
                std::find(target.master_hidden.begin(), target.master_hidden.end(), o.id) !=
                    target.master_hidden.end())
                continue;
            Item it = o;
            it.id = doc.next_id++;
            it.master_item = o.id;
            it.overridden = false;
            if (layer)
                it.layer = layer;
            it.group = remap_group(o.group);
            for (uint32_t &g : it.outer_groups)
                g = remap_group(g);
            new_id[o.id] = it.id;
            copies.push_back(std::move(it));
        }
    }
    /* At the bottom, in the master's order; the layers sort them in */
    copies.insert(copies.end(), std::make_move_iterator(rest.begin()),
                  std::make_move_iterator(rest.end()));
    target.items = std::move(copies);
    std::stable_sort(target.items.begin(), target.items.end(), [&](const Item &a, const Item &b) {
        return doc.layer_index(doc.layer_of(a)) < doc.layer_index(doc.layer_of(b));
    });
    if (new_id.empty())
        return;
    /* Each story threading copied frames gets a copy threading just those */
    const size_t story_count = doc.stories.size();
    for (size_t si = 0; si < story_count; ++si) {
        std::vector<ItemId> thread;
        for (ItemId f : doc.stories[si].thread) {
            auto it = new_id.find(f);
            if (it != new_id.end())
                thread.push_back(it->second);
        }
        if (thread.empty())
            continue;
        Story story = doc.stories[si].story;
        const StoryId sid = doc.add_story(std::move(story));
        if (StoryEntry *e = doc.find_story(sid))
            e->thread = std::move(thread);
    }
}

/* The page of `parent` that `child`'s page `side` takes. */
size_t parent_side(const MasterPage &child, size_t side, const MasterPage &parent) {
    if (!parent.spread())
        return 0;
    return child.spread() ? side : 1;
}

} // namespace

void PageDoc::sync_masters() {
    if (editing_master)
        return;
    if (linked_editing()) {
        end_linked_edit();               // its edits reach the master first; it syncs
        return;
    }
    const LayerId layer = master_layer();
    std::set<ItemId> gone;
    /* Parents first. A loop in based_on is cut where it is found. */
    std::set<MasterId> done, visiting;
    std::function<void(MasterPage &)> visit = [&](MasterPage &m) {
        if (done.count(m.id))
            return;
        if (visiting.count(m.id)) {
            m.based_on = 0;
            return;
        }
        visiting.insert(m.id);
        MasterPage *parent = find_master(m.based_on);
        if (parent == &m)
            parent = nullptr;
        if (parent)
            visit(*parent);
        if (parent && !find_master(m.based_on))
            parent = nullptr;            // the loop was cut on the way
        if (m.pages.empty())
            m.pages.resize(1);
        for (size_t side = 0; side < m.pages.size(); ++side) {
            const Page *src = parent ? &parent->pages[parent_side(m, side, *parent)] : nullptr;
            sync_page(*this, m.pages[side], src, layer, gone);
        }
        visiting.erase(m.id);
        done.insert(m.id);
    };
    for (MasterPage &m : masters)
        visit(m);
    for (size_t i = 0; i < pages.size(); ++i) {
        const MasterPage *m = find_master(pages[i].master);
        if (!m && pages[i].master)
            pages[i].master = 0;
        const Page *src = m ? &m->pages[std::min(master_side(i, *m), m->pages.size() - 1)] : nullptr;
        sync_page(*this, pages[i], src, layer, gone);
    }
    drop_from_threads(*this, gone);
    tidy_groups();
}

/* `ids` with the copies grouped with them on that page (a group overrides
 * as a whole). */
static std::set<ItemId> with_copy_groups(const Page &page, const std::vector<ItemId> &ids) {
    std::set<ItemId> out(ids.begin(), ids.end());
    std::set<uint32_t> roots;
    for (const Item &it : page.items)
        if (out.count(it.id) && it.group)
            roots.insert(it.root_group());
    for (const Item &it : page.items)
        if (it.group && roots.count(it.root_group()))
            out.insert(it.id);
    return out;
}

size_t PageDoc::override_master_items(size_t page, const std::vector<ItemId> &ids) {
    if (page >= pages.size())
        return 0;
    Page &p = pages[page];
    const std::set<ItemId> which = with_copy_groups(p, ids);
    size_t n = 0;
    for (Item &it : p.items)
        if (it.master_copy() && (ids.empty() || which.count(it.id))) {
            it.overridden = true;
            ++n;
        }
    return n;
}

size_t PageDoc::remove_overrides(size_t page, const std::vector<ItemId> &ids) {
    if (page >= pages.size())
        return 0;
    Page &p = pages[page];
    std::set<ItemId> gone;
    for (const Item &it : p.items)
        if (it.master_item && it.overridden &&
            (ids.empty() || std::find(ids.begin(), ids.end(), it.id) != ids.end()))
            gone.insert(it.id);
    if (ids.empty()) {
        const size_t hidden = p.master_hidden.size();
        p.master_hidden.clear();         // what the page let go of comes back too
        if (gone.empty())
            return hidden;
    }
    p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                 [&](const Item &it) { return gone.count(it.id) != 0; }),
                  p.items.end());
    drop_from_threads(*this, gone);
    return gone.size();
}

size_t PageDoc::detach_master_items(const std::vector<ItemId> &ids) {
    size_t n = 0;
    for (Page &p : pages)
        for (Item &it : p.items)
            if (it.master_item && std::find(ids.begin(), ids.end(), it.id) != ids.end()) {
                p.master_hidden.push_back(it.master_item);   // the master's doesn't come back
                it.master_item = 0;
                it.overridden = false;
                ++n;
            }
    return n;
}

bool PageDoc::begin_master_edit(MasterId id) {
    if (editing_master == id && id)
        return true;
    end_linked_edit();
    if (editing_master)
        end_master_edit();
    MasterPage *m = find_master(id);
    if (!m)
        return false;
    if (m->pages.empty())
        m->pages.resize(1);
    std::swap(pages, m->pages);
    editing_master = id;
    return true;
}

void PageDoc::end_master_edit() {
    if (!editing_master)
        return;
    MasterPage *m = find_master(editing_master);
    editing_master = 0;
    if (!m)
        return;                          // can't happen: removing is refused meanwhile
    std::swap(pages, m->pages);
    if (m->pages.empty())
        m->pages.resize(1);
    if (m->pages.size() > 2)
        m->pages.resize(2);
    sync_masters();
}

/* ---- Edit Master Here ---------------------------------------------------- */

bool PageDoc::begin_linked_edit(size_t page) {
    if (editing_master)
        return false;
    end_linked_edit();
    if (page >= pages.size())
        return false;
    const MasterPage *m = find_master(pages[page].master);
    if (!m || m->pages.empty())
        return false;
    const Page &side = m->pages[std::min(master_side(page, *m), m->pages.size() - 1)];
    /* The master's own items; what it shows from a parent is the parent's */
    std::set<ItemId> own;
    for (const Item &it : side.items)
        if (!it.master_copy())
            own.insert(it.id);
    linked_masters.clear();
    for (Item &it : pages[page].items)
        if (it.master_copy() && own.count(it.master_item)) {
            it.linked = true;
            linked_masters.push_back(it.master_item);
        }
    linked_page = page;
    linked_first_new = next_id;
    return true;
}

void PageDoc::end_linked_edit() {
    if (!linked_editing())
        return;
    const size_t page = linked_page;
    linked_page = SIZE_MAX;
    const std::set<ItemId> start(linked_masters.begin(), linked_masters.end());
    linked_masters.clear();
    MasterPage *m = page < pages.size() ? find_master(pages[page].master) : nullptr;
    if (m && !m->pages.empty()) {
        Page &p = pages[page];
        Page &side = m->pages[std::min(master_side(page, *m), m->pages.size() - 1)];
        const LayerId ml = master_layer();
        std::map<ItemId, const Item *> originals;
        for (const Item &it : side.items)
            originals[it.id] = &it;

        /* What the page holds for the master now, in its stacking order:
         * the copies (by their master item), and the new items */
        std::vector<std::pair<ItemId, const Item *>> now;
        std::set<ItemId> present, fresh;
        for (const Item &it : p.items) {
            const bool copy = it.linked && it.master_copy() && start.count(it.master_item) &&
                              originals.count(it.master_item);
            if (copy && it.id < linked_first_new && !present.count(it.master_item)) {
                now.push_back({it.master_item, &it});
                present.insert(it.master_item);
            } else if (it.id >= linked_first_new && ml && layer_of(it) == ml &&
                       (copy || !it.master_item)) {
                now.push_back({it.id, &it});     // new here, or a copy of a copy
                fresh.insert(it.id);
            }
        }

        /* The copies' groups are their own ids; the master's are its */
        std::map<uint32_t, uint32_t> group_back;
        for (const auto &e : now) {
            if (fresh.count(e.first))
                continue;
            const Item &c = *e.second, &o = *originals[e.first];
            if (c.group && o.group)
                group_back[c.group] = o.group;
            for (size_t k = 0; k < c.outer_groups.size() && k < o.outer_groups.size(); ++k)
                group_back[c.outer_groups[k]] = o.outer_groups[k];
        }
        auto back = [&](uint32_t g) {
            auto f = group_back.find(g);
            return f == group_back.end() ? g : f->second;
        };

        /* The master's page: what wasn't edited here as it was (its parent's
         * items, what this page doesn't show), then the edited and new ones */
        std::vector<Item> out;
        out.reserve(side.items.size() + fresh.size());
        for (const Item &it : side.items)
            if (!start.count(it.id))
                out.push_back(it);
        std::set<ItemId> gone(start);
        for (const auto &e : now) {
            Item x = *e.second;
            x.linked = false;
            x.group = back(x.group);
            for (uint32_t &g : x.outer_groups)
                g = back(g);
            if (fresh.count(e.first)) {
                x.layer = ml;            // moves, id and story thread and all
                x.master_item = 0;
                x.overridden = false;
            } else {
                const Item &o = *originals[e.first];
                gone.erase(e.first);
                if (const StoryEntry *sc = story_of(x.id))
                    if (StoryEntry *sm = story_of(o.id); sm && sm != sc)
                        sm->story = sc->story;   // retyped here
                x.id = o.id;
                x.master_item = o.master_item;
                x.overridden = o.overridden;
                x.layer = o.layer;
            }
            out.push_back(std::move(x));
        }
        side.items = std::move(out);
        p.items.erase(std::remove_if(p.items.begin(), p.items.end(),
                                     [&](const Item &it) { return fresh.count(it.id) != 0; }),
                      p.items.end());
        /* Deleted here: gone from the master, not just hidden on the page */
        p.master_hidden.erase(std::remove_if(p.master_hidden.begin(), p.master_hidden.end(),
                                             [&](ItemId id) { return start.count(id) != 0; }),
                              p.master_hidden.end());
        drop_from_threads(*this, gone);
    }
    for (Page &pg : pages)
        for (Item &it : pg.items)
            it.linked = false;
    sync_masters();
}

} // namespace pagemade
