/*
 * pagemade/plugin/pdf_parse.h — a PDF file's objects.
 *
 * Enough of ISO 32000 to read what a page draws: the object syntax, the
 * cross-reference (classic tables, cross-reference streams, object
 * streams, incremental updates through /Prev), stream filters (Flate and
 * LZW with their predictors, ASCIIHex, ASCII85, RunLength), and the page
 * tree with inherited attributes. A file whose cross-reference is damaged
 * is read again by scanning for "n g obj". Encrypted files are refused.
 *
 * The importer (pdf_import.cpp) interprets pages from here; a later PDF
 * export that embeds a placed page as a form will copy objects from here.
 * Plain C++ on miniz, the same on every platform.
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pagemade {
namespace pdf {

enum class Type : uint8_t { Null, Bool, Int, Real, String, Name, Array, Dict, Stream, Ref, Keyword };

struct Object;
using Array = std::vector<Object>;
using Dict = std::map<std::string, Object>;

struct Object {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;                   // Int and Real
    std::string str;                     // String bytes, Name (without the slash), Keyword
    std::shared_ptr<Array> array;
    std::shared_ptr<Dict> dict;          // Dict, and a Stream's dictionary
    std::shared_ptr<const std::string> data;   // a Stream's bytes, as stored (still filtered)
    int num = 0, gen = 0;                // Ref

    bool is(Type t) const { return type == t; }
    bool is_number() const { return type == Type::Int || type == Type::Real; }
    bool is_name(const char *n) const { return type == Type::Name && str == n; }
    double as_number(double fallback = 0) const { return is_number() ? number : fallback; }
    int as_int(int fallback = 0) const { return is_number() ? (int) number : fallback; }
    /* A Dict's or Stream's entry, unresolved; null when absent. */
    const Object *get(const char *key) const;
    size_t size() const { return array ? array->size() : 0; }
    const Object &operator[](size_t i) const;
};

/* The null object, for lookups that find nothing. */
const Object &null_object();

/* The lexer and object parser, over a byte range. Content streams use it
 * too (operators come back as Keyword objects). */
class Lexer {
public:
    Lexer(const uint8_t *data, size_t n, size_t pos = 0) : m_d(data), m_n(n), m_pos(pos) {}
    /* The next object; Keyword for operators and the words obj, endobj,
     * stream, R (R is folded into Refs: "1 0 R" comes back as one Ref).
     * False at the end. */
    bool next(Object &out);
    size_t pos() const { return m_pos; }
    void seek(size_t p) { m_pos = p; }
    void skip_space();
    const uint8_t *data() const { return m_d; }
    size_t size() const { return m_n; }

private:
    bool token(Object &out);
    const uint8_t *m_d;
    size_t m_n, m_pos;
    std::vector<Object> m_ahead;         // looked-ahead objects (for "n g R")
};

class Document {
public:
    /* Reads the cross-reference. False (and `error`) when the file isn't a
     * PDF this reader can open; an encrypted one says so. The bytes are
     * copied. */
    bool open(const uint8_t *data, size_t n, std::string *error);

    /* `o` itself, or the object a Ref names (null when it names nothing). */
    const Object &resolve(const Object &o) const;
    const Object &object(int num) const;
    /* A dictionary entry, resolved */
    const Object &get(const Object &dict, const char *key) const;

    /* A stream's bytes with its filters undone. A filter this reader leaves
     * to the caller (DCT, JPX, CCITT, JBIG2) stops the chain: `stopped`
     * names it, and the bytes are what that filter would read. False when
     * a filter fails. */
    bool stream_bytes(const Object &stream, std::string &out, std::string *stopped = nullptr) const;

    int page_count() const { return (int) m_pages.size(); }
    /* Page `i` (0-based), its inherited attributes (Resources, MediaBox,
     * CropBox, Rotate) copied onto it. */
    const Object &page(int i) const;

    const Object &trailer() const { return m_trailer; }

private:
    struct Entry {
        enum Kind : uint8_t { Free, Plain, InStream } kind = Free;
        size_t offset = 0;               // Plain: where "n g obj" starts
        int stream = 0, index = 0;       // InStream: the object stream and the place in it
    };
    bool read_xref(size_t at, std::string *error, int depth);
    bool read_xref_table(size_t at, std::string *error, int depth);
    bool read_xref_stream(size_t at, std::string *error, int depth);
    void merge_trailer(const Object &t);
    bool rebuild();                      // scan for "n g obj" when the xref is broken
    bool parse_at(size_t offset, int num, Object &out) const;
    bool load_object_stream(int num) const;
    void collect_pages(const Object &node, const Object &inherited, int depth);

    std::vector<uint8_t> m_bytes;
    std::map<int, Entry> m_xref;
    Object m_trailer;
    mutable std::map<int, Object> m_cache;
    mutable std::map<int, bool> m_loading;     // cycle guard
    std::vector<Object> m_pages;
};

/* Filters, exposed for tests */
bool flate_decode(const std::string &in, std::string &out);
bool lzw_decode(const std::string &in, std::string &out, bool early_change);
bool ascii_hex_decode(const std::string &in, std::string &out);
bool ascii85_decode(const std::string &in, std::string &out);
bool run_length_decode(const std::string &in, std::string &out);
/* PNG (10 and up) and TIFF (2) predictors */
bool unpredict(std::string &data, int predictor, int colors, int bpc, int columns);

} // namespace pdf
} // namespace pagemade
