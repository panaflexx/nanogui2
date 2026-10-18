/*
 * pagemade/book_gen.cpp — build a 100-page coffee-table photo book.
 *
 *   pagemade_book [out.pagemade]      (default: tst/photobook.pagemade)
 *
 * A4 landscape. Page 1 is a cover (full-page photo, headline), page 2 a
 * table of contents, then seven chapters of 14 pages: an opener with big
 * type and an SVG ornament, photo grid pages (8 or 6 photos with captions),
 * and big-text "random fact" pages. Photos come from the pagemade image
 * asset library (~/.config/pagemade/assets.conf) and stay linked, never
 * embedded; SVGs come from ~/Pictures/SVG. All text is made up on the spot
 * from name and fact pools (fixed seed, so runs are repeatable).
 */
#include "page.h"
#include "docfile.h"
#include "image.h"
#include "vector.h"
#include "uri.h"
#include "default_fonts.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

using namespace pagemade;
namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* Content pools                                                       */

static const char *kFirst[] = {
    "Mara", "Ellis", "Junie", "Theo", "Ada", "Bruno", "Celeste", "Dexter",
    "Ingrid", "Fabio", "Greta", "Hector", "Iris", "Jonas", "Katya", "Lyle",
    "Mina", "Nils", "Odette", "Pablo", "Quinn", "Rosa", "Silas", "Tessa",
    "Umar", "Vera", "Wendell", "Ximena", "Yusuf", "Zelda", "Ambrose",
    "Beatrix", "Casper", "Delia", "Emmett", "Frida", "Gus", "Hazel", "Ivor", "Josie",
};
static const char *kLast[] = {
    "Ellison", "Marchetti", "Okafor", "Lindqvist", "Draper", "Petrov",
    "Halloway", "Bianchi", "Novak", "Ferreira", "Gould", "Tanaka", "Mercer",
    "Voss", "Appleby", "Richter", "Salinas", "Crowe", "Delgado", "Fontaine",
    "Iversen", "Janssen", "Kowalski", "Laurent", "Mbeki", "Nightingale",
    "Osborne", "Pruitt", "Quimby", "Renshaw", "Sloane", "Thorne",
    "Underwood", "Vale", "Whitfield", "Yates", "Zimmer", "Ashford", "Bellamy",
};
static const char *kFacts[] = {
    "Has never lost at chess.", "Owns forty-two hats.",
    "Sleeps with the windows open, even in January.", "Once ate lunch with a mayor.",
    "Collects door numbers.", "Can whistle two notes at once.",
    "Has read the dictionary twice.", "Keeps a spare umbrella everywhere.",
    "Never sets an alarm clock.", "Grows tomatoes on the roof.",
    "Has a cat named Bureaucracy.", "Walks exactly 6,400 steps a day.",
    "Bakes bread every Sunday.", "Knows all the constellations.",
    "Has never had a cup of coffee.", "Writes letters by hand.",
    "Once found a message in a bottle.", "Can name every capital city.",
    "Fixes radios for fun.", "Swims in the sea all year.",
    "Has met three astronauts.", "Plays the tuba on weekends.",
    "Has kept a journal since 1998.", "Has climbed ten volcanoes.",
    "Never wears matching socks.", "Speaks to plants, out loud.",
    "Can solve a cube blindfolded.", "Has a tattoo of a tram.",
    "Photographs doors, only doors.", "Makes jam from backyard plums.",
    "Rides a bicycle built in 1952.", "Counts fire hydrants on walks.",
    "Has a shelf of found keys.", "Never learned to whistle.",
    "Keeps bees on the balcony.", "Watched every sunset last year.",
    "Collects hotel pens.", "Can recite pi to 200 digits.",
    "Names every houseplant.", "Has never been on an escalator.",
    "Once mailed a coconut, no box.", "Owns a lighthouse lamp.",
    "Dances while doing dishes.", "Keeps train tickets from childhood.",
    "Can tell rain by smell.", "Once outran a goat.",
    "Wears a watch on each wrist.", "Saves fortunes from cookies.",
    "Has planted 300 trees.", "Never uses a microwave.",
    "Can imitate any car alarm.", "Keeps a list of good clouds.",
    "Feeds the same crow every morning.", "Has shaken hands with a grandmaster.",
    "Once won a staring contest with a statue.", "Reads the obituaries first.",
};
static const char *kIntro[] = {
    "In this chapter we meet the people who make ordinary days worth photographing. ",
    "Some of them were stopped on the street; others wrote to us themselves. ",
    "Each portrait here was taken in under five minutes, and each fact is printed exactly as told. ",
    "We asked everyone the same three questions and printed whatever they said. ",
    "The names have been kept, the facts have been checked twice, and the rest is up to the reader. ",
    "Nothing in these pages is exaggerated, though a few details were hard to believe. ",
    "They come from fourteen towns and at least nine professions. ",
    "What they share is a willingness to stand still for one frame. ",
    "Every caption is a small biography; read them in any order. ",
    "If a fact seems unlikely, we assure you it seemed unlikely to us too. ",
};
static const char *kBigFacts[] = {
    "Honey never spoils. Archaeologists have tasted honey sealed in Egyptian tombs for three thousand years — and found it perfectly good.",
    "Octopuses have three hearts, and two of them stop beating when they swim.",
    "There are more possible games of chess than atoms in the observable universe.",
    "Wombat droppings are cube-shaped. Nobody told the wombats.",
    "A day on Venus is longer than a year on Venus.",
    "Sea otters hold hands while they sleep so they don't drift apart.",
    "The Eiffel Tower grows about fifteen centimetres taller in the summer heat.",
    "Bananas are berries. Strawberries are not. Botany has its own rules.",
    "Sharks existed before trees — by about fifty million years.",
    "Cleopatra lived closer in time to the Moon landing than to the building of the Great Pyramid.",
    "There is a town in Norway where the sun sets in November and doesn't rise until January.",
    "The first person to survive going over Niagara Falls in a barrel was a 63-year-old schoolteacher.",
    "Swifts can stay in the air for ten months at a time. They even sleep on the wing.",
    "Your shadow weighs nothing, but it has never once been late.",
};
static const char *kChapterTitles[] = {
    "The Early Risers", "Collectors & Keepers", "The Wanderers",
    "Quiet Achievers", "The Night Owls", "Amateurs & Enthusiasts",
    "The Legends Next Door",
};
static const char *kChapterNumbers[] = {
    "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN",
};
constexpr int kChapters = 7;

/* ------------------------------------------------------------------ */

static std::string leaf_name(const std::string &path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

/* The photo library: the first directory in pagemade's asset config. */
static std::string asset_library_dir() {
    const char *home = std::getenv("HOME");
    std::string conf = std::string(home ? home : ".") + "/.config/pagemade/assets.conf";
    FILE *f = std::fopen(conf.c_str(), "r");
    if (!f)
        return {};
    char line[4096];
    std::string dir;
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        std::error_code ec;
        if (!s.empty() && s[0] != '#' && fs::is_directory(s, ec)) {
            dir = s;
            break;
        }
    }
    std::fclose(f);
    return dir;
}

static bool is_photo(const fs::path &p) {
    std::string e = p.extension().string();
    for (char &c : e)
        c = (char) std::tolower((unsigned char) c);
    return e == ".jpg" || e == ".jpeg" || e == ".png";
}

int main(int argc, char **argv) {
    std::string out = argc > 1 ? argv[1] : "tst/photobook.pagemade";
    std::mt19937 rng(20261017u);

    /* ---- Gather photos from the image asset library (linked, probed only) */
    const std::string lib = asset_library_dir();
    if (lib.empty()) {
        std::fprintf(stderr, "no image asset library found\n");
        return 1;
    }
    std::printf("asset library: %s\n", lib.c_str());
    std::vector<std::string> paths;
    {
        std::error_code ec;
        fs::recursive_directory_iterator it(lib, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            if (it->is_regular_file(ec) && is_photo(it->path()))
                paths.push_back(it->path().string());
            it.increment(ec);
        }
    }
    std::printf("found %zu photo files\n", paths.size());
    /* Prefer real camera photos (IMG_*) over saved memes and screenshots. */
    std::vector<std::string> camera, other;
    for (std::string &p : paths) {
        std::string b = leaf_name(p);
        bool cam = b.size() > 4 && (b.rfind("IMG_", 0) == 0 || b.rfind("img_", 0) == 0);
        (cam ? camera : other).push_back(std::move(p));
    }
    std::shuffle(camera.begin(), camera.end(), rng);
    std::shuffle(other.begin(), other.end(), rng);
    camera.insert(camera.end(), other.begin(), other.end());
    paths = std::move(camera);
    std::printf("camera photos preferred: %zu available\n",
                paths.size() - 0);

    /* ---- Document setup: A4 landscape --------------------------------- */
    PageSetup setup;
    setup.width = 841.89f;
    setup.height = 595.28f;
    setup.margin_top = setup.margin_bottom = 36.f;
    setup.margin_inside = setup.margin_outside = 36.f;
    setup.columns = 1;
    PageDoc doc = new_publication(setup, 100);
    doc.pdf.title = "People & Other Facts";
    doc.pdf.author = "pagemade";
    const float PW = setup.width, PH = setup.height;
    const float MX = 36.f, MY = 36.f;                 // margins
    const float LW = PW - 2 * MX, LH = PH - 2 * MY;   // live area

    /* ---- Type ---------------------------------------------------------- */
    CharStyle display_big;  display_big.family = "Display"; display_big.bold = true;
                            display_big.size = 48; display_big.leading = 52;
    CharStyle sub;          sub.family = "Serif"; sub.italic = true;
                            sub.size = 15; sub.leading = 20;
    CharStyle toc_head;     toc_head.family = "Display"; toc_head.bold = true;
                            toc_head.size = 30; toc_head.leading = 36;
    CharStyle toc_entry;    toc_entry.family = "Serif"; toc_entry.size = 14; toc_entry.leading = 30;
    CharStyle chap_num;     chap_num.family = "Sans"; chap_num.bold = true; chap_num.size = 11;
                            chap_num.leading = 14; chap_num.tracking = 220;
                            chap_num.color = {0.45f, 0.45f, 0.45f, 1};
    CharStyle chap_title;   chap_title.family = "Display"; chap_title.bold = true;
                            chap_title.size = 40; chap_title.leading = 44;
    CharStyle intro;        intro.family = "Serif"; intro.size = 11.5f; intro.leading = 16;
    CharStyle cap_name;     cap_name.family = "Sans"; cap_name.bold = true;
                            cap_name.size = 8; cap_name.leading = 10;
    CharStyle cap_fact;     cap_fact.family = "Serif"; cap_fact.size = 8; cap_fact.leading = 10;
                            cap_fact.color = {0.35f, 0.35f, 0.35f, 1};
    CharStyle big_fact;     big_fact.family = "Serif"; big_fact.italic = true;
                            big_fact.size = 26; big_fact.leading = 36;
    CharStyle big_attr;     big_attr.family = "Sans"; big_attr.size = 10; big_attr.leading = 14;
                            big_attr.tracking = 150; big_attr.color = {0.45f, 0.45f, 0.45f, 1};

    ParaStyle center;   center.name = "Centered"; center.align = Align::Center;
    ParaStyle toc_ps;   toc_ps.name = "Contents"; toc_ps.tabs = {{0, TabAlign::Right, ".", true}};
    ParaStyle intro_ps; intro_ps.name = "Chapter intro"; intro_ps.align = Align::Justify;
    intro_ps.drop_lines = 3;
    ParaStyle plain;    plain.name = "Normal";

    auto define = [&](const std::string &name, const ParaStyle &ps, const CharStyle &cs) {
        ParaStyle p = ps; p.name = name;
        StyleDef d{name, p, cs};
        d.type.color = Color();
        if (StyleDef *old = doc.find_style(name))
            *old = d;
        else
            doc.styles.push_back(d);
    };
    define("Centered", center, intro);
    define("Contents", toc_ps, toc_entry);
    define("Chapter intro", intro_ps, intro);

    /* ---- Helpers -------------------------------------------------------- */
    ImageStore images;
    VectorStore vectors;
    size_t next_photo = 0;

    auto take_photo = [&](uint32_t &asset_id, ImageMetadata &meta) -> bool {
        while (next_photo < paths.size()) {
            const std::string &p = paths[next_photo++];
            ImageImport imp = probe_image_file(p);
            if (!imp.ok || imp.meta.width_px <= 0 || imp.meta.height_px <= 0)
                continue;
            const uint32_t id = images.take_id();
            images.add_link(id, p, imp.meta);
            ImageAsset asset;
            asset.id = id;
            asset.name = leaf_name(p);
            asset.source = file_uri(p);
            asset.meta = imp.meta;
            asset.source_size = imp.stamp.size;
            asset.source_mtime = imp.stamp.mtime;
            asset.hash = imp.stamp.hash;
            doc.add_image(std::move(asset));
            asset_id = id;
            meta = imp.meta;
            return true;
        }
        return false;
    };

    /* A photo frame at (fx, fy) sized (fw, fh), the picture scaled to cover
     * the frame and centered (the frame clips the overflow). */
    auto add_photo = [&](size_t page, float fx, float fy, float fw, float fh) -> bool {
        uint32_t id;
        ImageMetadata meta;
        if (!take_photo(id, meta))
            return false;
        float pw = image_print_points(meta.width_px, meta.ppi_x);
        float ph = image_print_points(meta.height_px, meta.ppi_y);
        if (pw <= 0 || ph <= 0) { pw = fw; ph = fh; }
        const float s = std::max(fw / pw, fh / ph);
        Item it;
        it.w = fw;
        it.h = fh;
        it.xf = Transform::translate(fx, fy);
        PlacedImage im;
        im.asset = id;
        im.w = pw * s;
        im.h = ph * s;
        im.x = (fw - im.w) * 0.5f;
        im.y = (fh - im.h) * 0.5f;
        it.content = im;
        doc.add_item(page, it);
        return true;
    };

    auto add_text = [&](size_t page, float x, float y, float w, float h, Story story) {
        StoryId id = doc.add_story(std::move(story));
        doc.add_text_frame(page, id, w, h, Transform::translate(x, y));
    };

    auto one_para = [&](const ParaStyle &ps, const CharStyle &cs, const std::string &text) {
        Story s;
        Paragraph p;
        p.style = ps;
        p.runs.push_back({cs, text});
        s.paragraphs.push_back(p);
        return s;
    };

    auto person = [&]() {
        return std::string(kFirst[rng() % std::size(kFirst)]) + " " +
               kLast[rng() % std::size(kLast)];
    };
    auto fact = [&]() { return std::string(kFacts[rng() % std::size(kFacts)]); };

    /* ---- Cover ---------------------------------------------------------- */
    add_photo(0, 0, 0, PW, PH);
    {
        /* A translucent band behind the title for readability. */
        const float band_y = 360, band_h = 130;
        Item band;
        band.w = PW;
        band.h = band_h;
        band.xf = Transform::translate(0, band_y);
        Shape sh;
        sh.fill = {doc.add_swatch("Cover band", {0.f, 0.f, 0.f, 0.45f}), 100};
        sh.stroke.weight = 0;
        band.content = sh;
        doc.add_item(0, band);

        CharStyle title = display_big;
        title.color = {1, 1, 1, 1};
        CharStyle sub_w = sub;
        sub_w.color = {1, 1, 1, 1};
        Story s;
        {
            Paragraph p;
            p.style = center;
            p.runs.push_back({title, "PEOPLE & OTHER FACTS"});
            s.paragraphs.push_back(p);
        }
        {
            ParaStyle ps = center;
            ps.space_before = 10;
            Paragraph p;
            p.style = ps;
            p.runs.push_back({sub_w, "One hundred pages of strangers, snapshots, "
                                     "and things that happen to be true"});
            s.paragraphs.push_back(p);
        }
        add_text(0, MX + 60, band_y + 26, LW - 120, band_h - 20, std::move(s));
    }

    /* ---- Table of contents ---------------------------------------------- */
    {
        Story s;
        {
            Paragraph p;
            p.style = plain;
            p.runs.push_back({toc_head, "Contents"});
            s.paragraphs.push_back(p);
        }
        for (int ch = 0; ch < kChapters; ++ch) {
            ParaStyle ps = toc_ps;
            ps.space_before = ch == 0 ? 26 : 4;
            Paragraph p;
            p.style = ps;
            const int page_no = 3 + ch * 14;   // 1-based
            std::string label = "Chapter " + std::to_string(ch + 1) + "  ·  " +
                                kChapterTitles[ch];
            p.runs.push_back({toc_entry, label + "\t" + std::to_string(page_no)});
            s.paragraphs.push_back(p);
        }
        add_text(1, 140, 120, LW - 280, 380, std::move(s));
    }

    /* ---- Chapters -------------------------------------------------------- */
    const char *svgs[] = {
        "mermaid13_1761040772.svg",
        "MermaidFrameEllipse.svg",
        "johnny-automatic-daisy-border.svg",
        "mermaid-character-vectorportal.svg",
        "johnny-automatic-heart-and-branches-border.svg",
        "TigerGreenOnRed.svg",
        "VirgoIcon.svg",
    };
    const char *home = std::getenv("HOME");
    int quote = 0;
    size_t page = 2;
    for (int ch = 0; ch < kChapters; ++ch) {
        /* Opener: chapter number, big title, rule, intro with a drop cap,
         * and an SVG ornament on the right. */
        {
            Story num;
            {
                Paragraph p;
                p.style = plain;
                p.runs.push_back({chap_num, std::string("CHAPTER ") + kChapterNumbers[ch]});
                num.paragraphs.push_back(p);
            }
            add_text(page, MX, 120, 400, 20, std::move(num));
            add_text(page, MX, 148, 500, 60, one_para(plain, chap_title, kChapterTitles[ch]));

            Item rule;
            rule.w = 300;
            rule.h = 0;
            rule.xf = Transform::translate(MX, 226);
            Shape rs;
            rs.kind = Shape::Kind::Line;
            rule.content = rs;
            doc.add_item(page, rule);

            std::string body;
            for (int k = 0; k < 5; ++k)
                body += kIntro[(rng() % std::size(kIntro))];
            add_text(page, MX, 260, 380, 240, one_para(intro_ps, intro, body));

            const std::string svg_path =
                std::string(home ? home : ".") + "/Pictures/SVG/" + svgs[ch];
            std::vector<uint8_t> bytes;
            std::string err;
            VectorScene scene;
            if (read_file(svg_path, &bytes, &err) &&
                vector_loader().decode(bytes.data(), bytes.size(), &scene, &err) &&
                scene.width > 0 && scene.height > 0) {
                const uint32_t id = vectors.take_id();
                vectors.add(id, bytes, scene);
                VectorAsset va;
                va.id = id;
                va.name = leaf_name(svg_path);
                va.source = file_uri(svg_path);
                va.width = scene.width;
                va.height = scene.height;
                va.skipped = scene.skipped;
                va.source_size = bytes.size();
                doc.add_vector(std::move(va));
                float w = 260.f, h = w * scene.height / scene.width;
                if (h > 320.f) { h = 320.f; w = h * scene.width / scene.height; }
                Item it;
                it.w = w;
                it.h = h;
                it.xf = Transform::translate(PW - MX - w, 150);
                PlacedVector pv;
                pv.asset = id;
                pv.w = w;
                pv.h = h;
                it.content = pv;
                doc.add_item(page, it);
            } else {
                std::printf("svg skipped: %s (%s)\n", svg_path.c_str(), err.c_str());
            }
            ++page;
        }
        /* 13 more pages: two big-fact pages, eleven photo pages. */
        int photo_pages = 0;
        for (int i = 1; i < 14; ++i, ++page) {
            if ((i == 4 || i == 10) && quote < (int) std::size(kBigFacts)) {
                Story s;
                {
                    Paragraph p;
                    p.style = center;
                    p.runs.push_back({big_fact, kBigFacts[quote]});
                    s.paragraphs.push_back(p);
                }
                {
                    ParaStyle ps = center;
                    ps.space_before = 22;
                    Paragraph p;
                    p.style = ps;
                    p.runs.push_back({big_attr, "— RANDOM FACT No. " + std::to_string(quote + 1)});
                    s.paragraphs.push_back(p);
                }
                add_text(page, 120, 200, LW - 240, 200, std::move(s));
                ++quote;
                continue;
            }
            /* Photo grid: 4×2 = 8 photos, every third page 3×2 = 6. */
            ++photo_pages;
            const int cols = (photo_pages % 3 == 0) ? 3 : 4;
            const int rows = 2;
            const float gap = 14, cap_h = 24;
            const float cw = (LW - gap * (cols - 1)) / cols;
            const float ch = (LH - gap * (rows - 1)) / rows;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c) {
                    const float x = MX + c * (cw + gap);
                    const float y = MY + r * (ch + gap);
                    if (!add_photo(page, x, y, cw, ch - cap_h))
                        break;
                    Story cap;
                    {
                        Paragraph p;
                        p.style = plain;
                        p.runs.push_back({cap_name, person()});
                        cap.paragraphs.push_back(p);
                    }
                    {
                        Paragraph p;
                        p.style = plain;
                        p.runs.push_back({cap_fact, fact()});
                        cap.paragraphs.push_back(p);
                    }
                    add_text(page, x, y + ch - cap_h + 2, cw, cap_h - 2, std::move(cap));
                }
        }
    }

    /* ---- Save (linked assets) ------------------------------------------- */
    FontLibrary fonts;
    register_default_fonts(fonts);
    SaveOptions opts;                    // embed_assets = false: references only
    std::string error;
    if (!save_document(doc, fonts, out, opts, &error, &images, &vectors)) {
        std::fprintf(stderr, "save FAILED: %s\n", error.c_str());
        return 1;
    }
    std::printf("saved %s: %zu pages, %zu photos used, %zu stories\n",
                out.c_str(), doc.pages.size(), next_photo, doc.stories.size());

    /* ---- Reopen as a sanity check --------------------------------------- */
    OpenResult re = open_document(out, fonts, /*load_linked=*/false);
    if (!re.ok) {
        std::fprintf(stderr, "reopen FAILED: %s\n", re.error.c_str());
        return 1;
    }
    size_t items = 0;
    for (const Page &p : re.doc.pages)
        items += p.items.size();
    std::printf("reopened ok: %zu pages, %zu items, %zu images, %zu vectors\n",
                re.doc.pages.size(), items, re.doc.images.size(), re.doc.vectors.size());
    return 0;
}
