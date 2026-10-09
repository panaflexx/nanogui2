/*
 * pagemade/page.h — the page model for the first slice: one page, its
 * guides, and the text flows placed on it. This grows into
 * Publication -> master pages / spreads -> pages -> items.
 */
#pragma once

#include "composer/composer.h"

#include <vector>

namespace pagemade {

/* Document Setup: page size, margins and column guides, in points. */
struct PageSetup {
    float width = 612.f, height = 792.f;    // US Letter
    float margin_top = 54.f, margin_bottom = 54.f;
    float margin_inside = 54.f, margin_outside = 54.f;
    int   columns = 2;
    float gutter = 18.f;
};

/* One story threaded through its text blocks, in reading order. */
struct TextFlow {
    Story              story;
    std::vector<Frame> frames;
};

struct PageDoc {
    PageSetup             setup;
    std::vector<TextFlow> flows;
};

/* A one-page newsletter: a headline flow across the top and a body story
 * threaded through two columns, with the second column short enough that
 * the story oversets. */
PageDoc sample_document();

} // namespace pagemade
