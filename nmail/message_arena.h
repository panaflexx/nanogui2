/*
 * message_arena.h — one flat buffer for cached RFC822 messages.
 *
 * The slab grows with use and never exceeds the configured cap (512MB
 * unless Preferences says otherwise). New messages are appended; when
 * the bump pointer hits the cap, live entries are packed to the front
 * and the least recently used ones are dropped until the new message
 * fits. A single message larger than the cap is refused.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class MessageArena {
public:
    static constexpr size_t kDefault = 512ull * 1024 * 1024;

    void set_limit(size_t n) {
        if (n < 256ull * 1024 * 1024) n = 256ull * 1024 * 1024;
        m_limit = n;
        evict_until(0);
        if (m_high > m_limit || m_slab.size() > m_live)
            compact();
    }
    size_t limit() const { return m_limit; }

    bool put(const std::string &key, const std::string &raw) {
        if (key.empty() || raw.empty() || raw.size() > m_limit) return false;
        erase(key);
        evict_until(raw.size());
        if (m_live + raw.size() > m_limit) return false;
        /* Erase/evict leave holes but do not rewind the bump pointer, so
         * once the slab has been filled every later put looks like it needs
         * room. Pack only then, and only when the live bytes actually fit. */
        if (m_high + raw.size() > m_limit || m_slab.size() < m_high + raw.size())
            compact();
        if (m_high + raw.size() > m_limit) return false;
        /* Grow the one slab, never past the cap. resize() alone may
         * double capacity and blow the limit; reserve() asks for an
         * exact size. */
        size_t need = m_high + raw.size();
        if (m_slab.capacity() < need) {
            size_t grown = m_slab.capacity() ? m_slab.capacity() * 2 : need;
            if (grown < need) grown = need;
            if (grown > m_limit) grown = m_limit;
            m_slab.reserve(grown);
        }
        if (m_slab.size() < need)
            m_slab.resize(need);
        std::memcpy(m_slab.data() + m_high, raw.data(), raw.size());
        m_slots.push_back(Slot{key, m_high, raw.size(), ++m_clock});
        m_high += raw.size();
        m_live += raw.size();
        return true;
    }

    bool contains(const std::string &key) const {
        return find(key) != m_slots.end();
    }

    /* Copies the bytes out so a later eviction cannot invalidate the caller. */
    bool get(const std::string &key, std::string &raw) const {
        auto it = find(key);
        if (it == m_slots.end()) return false;
        it->stamp = ++m_clock;
        raw.assign(m_slab.data() + it->off, it->len);
        return true;
    }

    void erase(const std::string &key) {
        auto it = find(key);
        if (it == m_slots.end()) return;
        m_live -= it->len;
        m_slots.erase(it);
    }

    void erase_prefix(const std::string &prefix) {
        if (prefix.empty()) return;
        auto it = m_slots.begin();
        while (it != m_slots.end()) {
            if (it->key.compare(0, prefix.size(), prefix) == 0) {
                m_live -= it->len;
                it = m_slots.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        m_slots.clear();
        m_slab.clear();
        m_high = 0;
        m_live = 0;
    }

    size_t bytes_live() const { return m_live; }

private:
    struct Slot {
        std::string key;
        size_t off = 0;
        size_t len = 0;
        mutable uint64_t stamp = 0;
    };

    std::vector<Slot>::iterator find(const std::string &key) const {
        return std::find_if(m_slots.begin(), m_slots.end(),
            [&](const Slot &s) { return s.key == key; });
    }

    void evict_until(size_t need) {
        while (!m_slots.empty() && m_live + need > m_limit) {
            auto oldest = std::min_element(m_slots.begin(), m_slots.end(),
                [](const Slot &a, const Slot &b) { return a.stamp < b.stamp; });
            m_live -= oldest->len;
            m_slots.erase(oldest);
        }
    }

    /* Slide live ranges down over holes. A second buffer the size of the
     * cache was allocating and freeing hundreds of MB on the UI thread
     * (prefetch -> body_put -> compact) and froze the window. Dest is
     * always <= src, so memmove is safe. */
    void compact() {
        if (m_slots.empty()) {
            m_high = 0;
            m_live = 0;
            return;
        }
        std::sort(m_slots.begin(), m_slots.end(),
                  [](const Slot &a, const Slot &b) { return a.off < b.off; });
        size_t at = 0;
        for (Slot &s : m_slots) {
            if (s.len && s.off != at)
                std::memmove(m_slab.data() + at, m_slab.data() + s.off, s.len);
            s.off = at;
            at += s.len;
        }
        m_high = at;
    }

    std::vector<char> m_slab;
    mutable std::vector<Slot> m_slots;
    size_t m_high = 0;
    size_t m_live = 0;
    size_t m_limit = kDefault;
    mutable uint64_t m_clock = 1;
};
