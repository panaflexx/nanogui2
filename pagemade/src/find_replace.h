/*
 * pagemade/find_replace.h — finding text in a publication's stories, and
 * keeping track of each find through replace, revert, undo and typing.
 *
 * A search runs paragraph by paragraph (a match never spans a paragraph
 * break). Without Match Case, letters compare by a simple case fold that
 * covers Latin, Greek and Cyrillic. Whole Word wants no letter or digit
 * either side of the match.
 *
 * Each find remembers its story, its paragraph, and how many bytes of
 * unchanged text lie between it and the find before it in that paragraph
 * (or the paragraph's start). Replacing or reverting one find changes its
 * own length only, so the finds after it can be located again from those
 * gaps. resolve_found() does that after any change, and reads what each
 * find now holds: its original text, its replacement, or something else
 * (stale: edited since, or undone past the search).
 */
#pragma once

#include "page.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pagemade {

struct FindOptions {
    bool match_case = false;
    bool whole_word = false;
};

/* One match in a story: paragraph, byte offset and length in its text. */
struct StoryMatch {
    size_t   para = 0;
    uint32_t byte = 0, len = 0;
};

/* Every match of `needle` (UTF-8) in `s`, in reading order. Matches don't
 * overlap. An empty needle finds nothing. */
std::vector<StoryMatch> find_matches(const Story &s, const std::string &needle,
                                     const FindOptions &opt);

struct FoundText {
    enum class State { Found, Replaced, Stale };
    StoryId     story = 0;
    size_t      para = 0;
    uint32_t    gap = 0;                 // bytes after the previous find in the paragraph
    std::string found;                   // the text as found
    std::string replacement;             // what Replace puts there
    State       state = State::Found;
    uint32_t    byte = 0;                // where it is now (resolve_found)
    /* The text now there: `found`, or `replacement` once replaced. */
    const std::string &current() const { return state == State::Replaced ? replacement : found; }
};

/* Every match in every story, in story order. Each find's replacement is
 * `replacement`. */
std::vector<FoundText> find_all(const PageDoc &doc, const std::string &needle,
                                const std::string &replacement, const FindOptions &opt);

/* Locate each find again and read its state (see the top of this file).
 * Finds must be in the order find_all made them. */
void resolve_found(const PageDoc &doc, std::vector<FoundText> &finds);

/* Replace one find (`replace`), or put its original text back. The new
 * text takes the style of the text it replaces (its first character).
 * Only the story changes: the caller makes the undo step before, and
 * recomposes after. False when the find is stale, or already that way.
 * Call resolve_found first if anything changed since it last ran. */
bool apply_found(PageDoc &doc, FoundText &find, bool replace);

/* Text around a find for a list: up to `before` bytes ahead of it and
 * `after` bytes behind it in its paragraph, cut at character boundaries,
 * with tabs and breaks shown as spaces. */
void found_context(const PageDoc &doc, const FoundText &find, size_t before, size_t after,
                   std::string &head, std::string &tail);

} // namespace pagemade
