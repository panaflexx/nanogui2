/*
 * imap_client.cpp — minimal blocking IMAP4rev1 client for nmail.
 *
 * Auto-reconnects on idle timeout / server BYE: credentials + selected
 * mailbox are sticky and run() retries once after a silent LOGIN+SELECT.
 */
#include "imap_client.h"
#include "nmail_socket.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

// ---------------------------------------------------------------------------
// Small text utilities
// ---------------------------------------------------------------------------

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::string to_lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool starts_with(const std::string &s, const std::string &prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------------------
// Decoders: base64, quoted-printable, RFC 2047 encoded words
// ---------------------------------------------------------------------------

static std::string base64_decode(const std::string &in) {
    static const signed char T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = T[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

static std::string base64_encode(const std::string &in) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        bool b1 = i + 1 < in.size(), b2 = i + 2 < in.size();
        if (b1) v |= (unsigned char)in[i + 1] << 8;
        if (b2) v |= (unsigned char)in[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += b1 ? T[(v >> 6) & 63] : '=';
        out += b2 ? T[v & 63] : '=';
    }
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string qp_decode(const std::string &in, bool underscore_space) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '=' && i + 2 < in.size()) {
            int hi = hex_val(in[i + 1]), lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
            if (in[i + 1] == '\r' && in[i + 2] == '\n') { i += 2; continue; }
            if (in[i + 1] == '\n') { i += 1; continue; }
        }
        if (underscore_space && c == '_') { out += ' '; continue; }
        out += c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// MD5 / HMAC-MD5 (RFC 1321 / RFC 2104) — for CRAM-MD5 authentication
// ---------------------------------------------------------------------------

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void md5_block(uint32_t state[4], const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const uint8_t S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    uint32_t M[16];
    for (int i = 0; i < 16; ++i)
        M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);   g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);   g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d;            g = (3*i + 5) % 16; }
        else             { f = c ^ (b | ~d);         g = (7*i) % 16; }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + rotl32(a + f + K[i] + M[g], S[i]);
        a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static std::string md5_bin(const std::string &in) {
    uint32_t state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    std::string msg = in;
    uint64_t bitlen = (uint64_t)in.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 0; i < 8; ++i) msg += (char)((bitlen >> (8*i)) & 0xff);
    for (size_t off = 0; off < msg.size(); off += 64)
        md5_block(state, (const uint8_t *)msg.data() + off);
    std::string out(16, '\0');
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i*4 + j] = (char)((state[i] >> (8*j)) & 0xff);
    return out;
}

static std::string to_hex(const std::string &bin) {
    static const char H[] = "0123456789abcdef";
    std::string out;
    out.reserve(bin.size() * 2);
    for (unsigned char c : bin) {
        out += H[c >> 4];
        out += H[c & 15];
    }
    return out;
}

static std::string hmac_md5_hex(const std::string &key, const std::string &msg) {
    std::string k = key;
    if (k.size() > 64) k = md5_bin(k);
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) {
        ipad[i] = (char)(k[i] ^ 0x36);
        opad[i] = (char)(k[i] ^ 0x5c);
    }
    return to_hex(md5_bin(opad + md5_bin(ipad + msg)));
}

/* Decode RFC 2047 encoded words: =?charset?B?...?= / =?charset?Q?...?= */
static std::string decode_encoded_words(const std::string &in) {
    std::string out;
    size_t pos = 0;
    bool last_was_encoded = false;
    while (pos < in.size()) {
        size_t start = in.find("=?", pos);
        if (start == std::string::npos) {
            out += in.substr(pos);
            break;
        }
        size_t enc_q = in.find('?', start + 2);
        if (enc_q == std::string::npos || enc_q + 2 >= in.size()) {
            out += in.substr(pos);
            break;
        }
        char enc = (char)std::toupper((unsigned char)in[enc_q + 1]);
        if (in[enc_q + 2] != '?' || (enc != 'B' && enc != 'Q')) {
            out += in.substr(pos, start - pos + 2);
            pos = start + 2;
            last_was_encoded = false;
            continue;
        }
        size_t end = in.find("?=", enc_q + 3);
        if (end == std::string::npos) {
            out += in.substr(pos);
            break;
        }
        /* Whitespace between adjacent encoded words is decorative. */
        if (!(last_was_encoded && trim(in.substr(pos, start - pos)).empty()))
            out += in.substr(pos, start - pos);
        std::string payload = in.substr(enc_q + 3, end - (enc_q + 3));
        out += (enc == 'B') ? base64_decode(payload)
                            : qp_decode(payload, /*underscore_space=*/true);
        last_was_encoded = true;
        pos = end + 2;
    }
    return out;
}

/* "Display Name <addr@host>" -> "addr@host" (best effort). */
static std::string address_of(const std::string &raw) {
    size_t lt = raw.find('<');
    if (lt != std::string::npos) {
        size_t gt = raw.find('>', lt);
        return trim(raw.substr(lt + 1,
                    gt == std::string::npos ? gt : gt - lt - 1));
    }
    return trim(raw);
}

// ---------------------------------------------------------------------------
// Header helpers
// ---------------------------------------------------------------------------

/* Split an RFC 822 header block into a lowercased-key map, unfolding
 * continuation lines. */
static std::map<std::string, std::string>
parse_headers(const std::string &block) {
    std::map<std::string, std::string> out;
    std::string key, value;
    auto flush = [&]() {
        if (!key.empty() && out.find(key) == out.end())
            out[key] = trim(value);
        key.clear(); value.clear();
    };
    size_t pos = 0;
    while (pos <= block.size()) {
        size_t nl = block.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? block.substr(pos) : block.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? block.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == ' ' || line[0] == '\t') {
            value += ' ';
            value += trim(line);
            continue;
        }
        flush();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        key   = to_lower(line.substr(0, colon));
        value = line.substr(colon + 1);
    }
    flush();
    return out;
}

/* "Display Name <addr@host>" -> "Display Name" (decoded), else the address. */
static std::string display_from(const std::string &raw) {
    std::string s = decode_encoded_words(raw);
    size_t lt = s.find('<');
    if (lt != std::string::npos) {
        std::string name = trim(s.substr(0, lt));
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
            name = name.substr(1, name.size() - 2);
        if (!name.empty())
            return name;
        size_t gt = s.find('>', lt);
        return trim(s.substr(lt + 1,
                    gt == std::string::npos ? gt : gt - lt - 1));
    }
    return trim(s);
}

// ---------------------------------------------------------------------------
// MIME body extraction (best effort: prefer text/plain, else stripped HTML)
// ---------------------------------------------------------------------------

static void split_head_body(const std::string &raw,
                            std::string &head, std::string &body) {
    size_t p = raw.find("\r\n\r\n");
    if (p != std::string::npos) { head = raw.substr(0, p); body = raw.substr(p + 4); return; }
    p = raw.find("\n\n");
    if (p != std::string::npos) { head = raw.substr(0, p); body = raw.substr(p + 2); return; }
    head = raw; body.clear();
}

static std::string cte_decode(const std::string &data, const std::string &cte) {
    std::string e = to_lower(trim(cte));
    if (e == "base64")           return base64_decode(data);
    if (e == "quoted-printable") return qp_decode(data, false);
    return data;
}

static std::string strip_html(const std::string &html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (in_tag) {
            if (c == '>') in_tag = false;
            continue;
        }
        if (c == '<') {
            /* Treat structural tags as line breaks. */
            char n1 = (i + 1 < html.size()) ? (char)std::tolower((unsigned char)html[i + 1]) : 0;
            char n2 = (i + 2 < html.size()) ? (char)std::tolower((unsigned char)html[i + 2]) : 0;
            if (n1 == 'p' || n1 == 'd' || n1 == 't' ||
                (n1 == 'b' && n2 == 'r') || n1 == '/')
                out += '\n';
            in_tag = true;
            continue;
        }
        if (c == '&') {
            if (starts_with(html.substr(i), "&amp;"))  { out += '&';  i += 4; continue; }
            if (starts_with(html.substr(i), "&lt;"))   { out += '<';  i += 3; continue; }
            if (starts_with(html.substr(i), "&gt;"))   { out += '>';  i += 3; continue; }
            if (starts_with(html.substr(i), "&nbsp;")) { out += ' ';  i += 5; continue; }
            if (starts_with(html.substr(i), "&quot;")) { out += '"';  i += 5; continue; }
        }
        out += c;
    }
    return out;
}

static std::string content_type_param(const std::string &ct,
                                      const std::string &param) {
    std::string lct = to_lower(ct);
    std::string needle = to_lower(param) + "=";
    size_t p = lct.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    if (p < ct.size() && ct[p] == '"') {
        size_t e = ct.find('"', p + 1);
        return e == std::string::npos ? "" : ct.substr(p + 1, e - p - 1);
    }
    size_t e = ct.find(';', p);
    return trim(ct.substr(p, e == std::string::npos ? e : e - p));
}

static std::string mime_base_type(const std::string &ct) {
    size_t semi = ct.find(';');
    return to_lower(trim(ct.substr(0, semi)));
}

/* Recursively collect the readable text parts of a MIME entity: the first
 * text/plain part fills `plain`, the first text/html part fills `html`
 * (both content-transfer-decoded, HTML *not* stripped).  plain_markdown is
 * set when the plain part declares markup=markdown (MailMate convention)
 * or is text/markdown outright. */
static void mime_extract_parts(const std::string &head, const std::string &body,
                               std::string &plain, std::string &html,
                               bool &plain_markdown,
                               std::vector<MailImage> &images, int depth) {
    if (depth > 6) return;
    auto headers = parse_headers(head);
    std::string ct;
    auto it = headers.find("content-type");
    if (it != headers.end()) ct = it->second;
    std::string base = ct.empty() ? "text/plain" : mime_base_type(ct);
    std::string cte;
    it = headers.find("content-transfer-encoding");
    if (it != headers.end()) cte = it->second;

    if (starts_with(base, "multipart/")) {
        std::string boundary = content_type_param(ct, "boundary");
        if (boundary.empty()) return;
        std::string delim = "--" + boundary;
        size_t pos = body.find(delim);
        while (pos != std::string::npos) {
            size_t part_start = body.find('\n', pos);
            if (part_start == std::string::npos) break;
            part_start += 1;
            size_t next = body.find(delim, part_start);
            std::string part = (next == std::string::npos)
                ? body.substr(part_start) : body.substr(part_start, next - part_start);
            /* "--boundary--" terminates. */
            if (starts_with(trim(body.substr(pos, part_start - pos)), delim + "--"))
                break;
            std::string ph, pb;
            split_head_body(part, ph, pb);
            mime_extract_parts(ph, pb, plain, html, plain_markdown,
                               images, depth + 1);
            pos = next;
        }
        return;
    }

    /* Inline image part (referenced from the HTML via cid:). */
    if (starts_with(base, "image/")) {
        MailImage img;
        img.mime = base;
        it = headers.find("content-id");
        if (it != headers.end()) {
            std::string cid = trim(it->second);
            if (!cid.empty() && cid.front() == '<') cid = cid.substr(1);
            if (!cid.empty() && cid.back() == '>') cid.pop_back();
            img.cid = cid;
        }
        img.data = cte_decode(body, cte);
        if (!img.data.empty())
            images.push_back(std::move(img));
        return;
    }

    if (base == "text/html") {
        if (html.empty()) html = cte_decode(body, cte);
        return;
    }
    /* text/plain and unknown types: decode and pass through. */
    if (plain.empty()) {
        plain = cte_decode(body, cte);
        plain_markdown =
            base == "text/markdown" ||
            to_lower(content_type_param(ct, "markup")) == "markdown";
    }
}

// ---------------------------------------------------------------------------
// IMAP response parsing cursor
// ---------------------------------------------------------------------------

struct ImapCursor {
    const std::string &s;
    size_t pos = 0;
    explicit ImapCursor(const std::string &str) : s(str) {}

    void skip_ws() { while (pos < s.size() && s[pos] == ' ') ++pos; }
    bool eof() const { return pos >= s.size(); }
    char peek() const { return pos < s.size() ? s[pos] : '\0'; }
    bool expect(char c) { if (peek() == c) { ++pos; return true; } return false; }

    std::string read_quoted() {
        std::string out;
        if (!expect('"')) return out;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '\\' && pos < s.size()) { out += s[pos++]; continue; }
            if (c == '"') break;
            out += c;
        }
        return out;
    }

    /* Balanced-paren span, quote-aware; includes the outer parens. */
    std::string read_parens() {
        size_t start = pos;
        if (peek() != '(') return "";
        int depth = 0;
        bool in_str = false;
        while (pos < s.size()) {
            char c = s[pos++];
            if (in_str) {
                if (c == '\\' && pos < s.size()) { ++pos; continue; }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '(') ++depth;
            else if (c == ')' && --depth == 0) break;
        }
        return s.substr(start, pos - start);
    }

    /* Literal "{n}\r\n<bytes>", quoted string, parenthesized list, NIL,
     * or bare atom. */
    std::string read_value() {
        skip_ws();
        char c = peek();
        if (c == '"') return read_quoted();
        if (c == '(') return read_parens();
        if (c == '{') {
            ++pos;
            size_t n = 0;
            while (pos < s.size() && std::isdigit((unsigned char)s[pos]))
                n = n * 10 + (size_t)(s[pos++] - '0');
            if (!expect('}')) return "";
            if (!expect('\r')) return "";
            if (!expect('\n')) return "";
            if (pos + n > s.size()) n = s.size() - pos;
            std::string out = s.substr(pos, n);
            pos += n;
            return out;
        }
        size_t start = pos;
        while (pos < s.size() && s[pos] != ' ' && s[pos] != ')') ++pos;
        return s.substr(start, pos - start);
    }

    /* FETCH item key; bracket section may itself contain spaces. */
    std::string read_key() {
        skip_ws();
        size_t start = pos;
        int depth = 0;
        while (pos < s.size()) {
            char c = s[pos];
            if (c == '[') ++depth;
            else if (c == ']') --depth;
            else if (c == ' ' && depth == 0) break;
            else if (c == ')' && depth == 0) break;
            ++pos;
        }
        return s.substr(start, pos - start);
    }
};

/* Parse the item list of a FETCH response into key/value pairs. */
static std::vector<std::pair<std::string, std::string>>
parse_fetch_items(const std::string &line) {
    std::vector<std::pair<std::string, std::string>> items;
    ImapCursor cur(line);
    cur.skip_ws();
    cur.expect('*');                 // untagged marker
    cur.skip_ws();
    while (!cur.eof() && std::isdigit((unsigned char)cur.peek())) ++cur.pos;
    cur.skip_ws();
    /* "FETCH" */
    while (!cur.eof() && cur.peek() != ' ' && cur.peek() != '(') ++cur.pos;
    cur.skip_ws();
    if (!cur.expect('(')) return items;
    while (!cur.eof() && cur.peek() != ')') {
        std::string key = cur.read_key();
        if (key.empty()) { ++cur.pos; continue; }
        cur.skip_ws();
        std::string value = cur.read_value();
        items.emplace_back(key, value);
        cur.skip_ws();
    }
    return items;
}

/* "01-Feb-2024 12:34:56 +0000" -> "2/1/24" */
static std::string format_internaldate(const std::string &s) {
    static const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
    int day = 0, year = 0;
    char mon[8] = {0};
    if (std::sscanf(s.c_str(), "%d-%3[^-]-%d", &day, mon, &year) != 3)
        return s;
    int month = 0;
    for (int i = 0; i < 12; ++i)
        if (starts_with(mon, months[i])) { month = i + 1; break; }
    if (!month) return s;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d/%d/%02d", month, day, year % 100);
    return buf;
}

/* Reduce an HTML snippet to plain text for previews: drop <!...> and
   <script>/<style> blocks, remove all tags, decode the common entities. */
static std::string html_to_text_preview(const std::string &html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    const size_t n = html.size();

    auto tag_name_at = [&](size_t pos) -> std::string {
        /* pos is just after '<' (and optional '/'); read [a-zA-Z]+ */
        std::string name;
        if (pos < n && html[pos] == '/') ++pos;
        while (pos < n && std::isalpha((unsigned char)html[pos]))
            name += (char)std::tolower((unsigned char)html[pos++]);
        return name;
    };
    /* Case-insensitive search for "</name" starting at i. */
    auto find_close = [&](const char *name) -> size_t {
        std::string needle = "</";
        needle += name;
        for (size_t j = i; j + needle.size() <= n; ++j) {
            size_t k = 0;
            while (k < needle.size() &&
                   std::tolower((unsigned char)html[j + k]) == needle[k])
                ++k;
            if (k == needle.size()) return j;
        }
        return n;
    };

    while (i < n && out.size() < 512) {
        if (html[i] == '<') {
            std::string name = tag_name_at(i + 1);
            size_t gt = html.find('>', i);
            if (name == "script" || name == "style" || name == "head") {
                /* Drop the whole element, content included. */
                size_t close = find_close(name.c_str());
                if (close == n) break;
                size_t end = html.find('>', close);
                i = (end == std::string::npos) ? n : end + 1;
            } else {
                i = (gt == std::string::npos) ? n : gt + 1;
            }
        } else if (html[i] == '&') {
            size_t semi = html.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string e = html.substr(i + 1, semi - i - 1);
                for (char &c : e) c = (char)std::tolower((unsigned char)c);
                if      (e == "amp")   out += '&';
                else if (e == "lt")    out += '<';
                else if (e == "gt")    out += '>';
                else if (e == "quot")  out += '"';
                else if (e == "apos" || e == "#39") out += '\'';
                else if (e == "nbsp")  out += ' ';
                else { out += html[i]; semi = i; }   // unknown: keep '&'
                i = (semi == i) ? i + 1 : semi + 1;
            } else {
                out += html[i++];
            }
        } else {
            out += html[i++];
        }
    }
    return out;
}

/* True for lines that are MIME structure rather than message text:
   boundaries, part headers, multipart preambles and base64 payloads. */
static bool is_mime_noise_line(const std::string &line) {
    if (line.empty()) return false;
    if (starts_with(line, "--")) return true;              // boundary
    std::string l;
    l.reserve(line.size());
    for (unsigned char c : line) l += (char)std::tolower(c);
    if (starts_with(l, "content-") || starts_with(l, "mime-version"))
        return true;                                      // part headers
    if (l.find("multi-part message") != std::string::npos ||
        l.find("multipart message") != std::string::npos)
        return true;                                      // preamble
    /* base64 payload: long run of nothing but the base64 alphabet */
    if (line.size() >= 32) {
        bool b64 = true;
        for (unsigned char c : line) {
            if (!std::isalnum(c) && c != '+' && c != '/' && c != '=') {
                b64 = false; break;
            }
        }
        if (b64) return true;
    }
    return false;
}

/* Strip MIME scaffolding line by line, keeping only message text. */
static std::string strip_mime_scaffolding(const std::string &text) {
    /* Quick check: does this look like a raw multipart dump at all? */
    std::string head;
    for (size_t i = 0; i < text.size() && i < 512; ++i)
        head += (char)std::tolower((unsigned char)text[i]);
    if (head.find("content-type") == std::string::npos &&
        head.find("content-transfer") == std::string::npos &&
        !starts_with(head, "--") && head.find("\n--") == std::string::npos)
        return text;

    bool qp = head.find("quoted-printable") != std::string::npos;

    std::string out;
    size_t i = 0, n = text.size();
    bool in_part_headers = false;   // previous line was a Content-* header
    while (i < n) {
        size_t eol = text.find('\n', i);
        std::string line = (eol == std::string::npos)
                           ? text.substr(i) : text.substr(i, eol - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        /* Folded header continuation (e.g. ` charset="utf-8"` or
           ` boundary="----..."` on its own line): whitespace-led lines
           directly following a part header are still header.  A blank
           line always ends the header block. */
        bool continuation = in_part_headers &&
                            !line.empty() &&
                            (line[0] == ' ' || line[0] == '\t');
        if (!continuation)
            in_part_headers = false;

        if (qp) {
            /* Light quoted-printable decode: =XX bytes, soft breaks. */
            std::string dec;
            for (size_t k = 0; k < line.size(); ++k) {
                if (line[k] == '=' && k + 2 < line.size() &&
                    std::isxdigit((unsigned char)line[k + 1]) &&
                    std::isxdigit((unsigned char)line[k + 2])) {
                    dec += (char)std::strtol(line.substr(k + 1, 2).c_str(),
                                             nullptr, 16);
                    k += 2;
                } else if (line[k] == '=' && k + 1 == line.size()) {
                    break;   // soft break at end of line
                } else {
                    dec += line[k];
                }
            }
            line.swap(dec);
        }
        bool noise = is_mime_noise_line(line);
        if (noise) {
            std::string l;
            for (unsigned char c : line) l += (char)std::tolower(c);
            if (starts_with(l, "content-") || starts_with(l, "mime-version"))
                in_part_headers = true;
        }
        if (!noise && !continuation) {
            out += line;
            out += '\n';
        }
        if (eol == std::string::npos) break;
        i = eol + 1;
    }
    return out;
}

static std::string sanitize_preview(const std::string &text) {
    /* BODY.PEEK[TEXT] of a multipart mail returns the whole MIME
       structure; strip boundaries/part headers first. */
    std::string plain = strip_mime_scaffolding(text);

    /* HTML (direct or one part of a multipart) -> plain text. */
    std::string head;
    head.reserve(plain.size());
    for (size_t i = 0; i < plain.size() && i < 512; ++i)
        head += (char)std::tolower((unsigned char)plain[i]);
    if (head.find("<!doctype") != std::string::npos ||
        head.find("<html") != std::string::npos)
        plain = html_to_text_preview(plain);

    std::string out;
    out.reserve(plain.size());
    bool ws = true;
    for (unsigned char c : plain) {
        if (std::isspace(c)) {
            if (!ws) out += ' ';
            ws = true;
        } else {
            out += (char)c;
            ws = false;
        }
        if (out.size() >= 160) break;
    }
    return trim(out);
}

std::string message_preview(const MailMessage &msg) {
    std::string src;
    if (!msg.body.empty())
        src = msg.body;
    else if (!msg.html.empty())
        src = html_to_text_preview(msg.html);
    else
        return "";
    // collapse whitespace + trim, cap 160 (same as sanitize_preview tail)
    std::string out;
    out.reserve(src.size());
    bool ws = true;
    for (unsigned char c : src) {
        if (std::isspace(c)) {
            if (!ws) out += ' ';
            ws = true;
        } else {
            out += (char)c;
            ws = false;
        }
        if (out.size() >= 160) break;
    }
    return trim(out);
}

// ---------------------------------------------------------------------------
// ImapClient — connection and transport
// ---------------------------------------------------------------------------

ImapClient::~ImapClient() {
    close();
}

void ImapClient::abort() {
    nmail_sock_abort(m_fd);
}

void ImapClient::close() {
    if (m_fd >= 0) {
        nmail_sock_close(m_fd);
        m_fd = -1;
    }
    m_rbuf.clear();
    m_selected_folder.clear();
}

bool ImapClient::is_connection_error(const std::string &err) {
    std::string l = to_lower(err);
    if (l.find("connection") != std::string::npos) return true;
    if (l.find("timed out") != std::string::npos)  return true;
    if (l.find("closed") != std::string::npos)     return true;
    if (l.find("lost") != std::string::npos)       return true;
    if (l.find("bye") != std::string::npos)        return true;
    return false;
}

bool ImapClient::reconnect(std::string &err) {
    if (m_host.empty() || m_user.empty()) {
        err = "no saved credentials for reconnect";
        return false;
    }
    std::string saved_folder = m_selected_folder;
    // Must not call open() which would clear m_selected_folder we want to save.
    std::string e;
    {
        // open() closes + resets state; stash folder first then re-select.
        close();
        m_caps.clear();
        char ebuf[512] = {0};
        m_fd = nmail_sock_connect(m_host.c_str(), m_port, ebuf, sizeof(ebuf));
        if (m_fd < 0) { e = ebuf; err = e; return false; }
        if (m_port == 993) {
            if (nmail_sock_starttls(m_fd, ebuf, sizeof(ebuf)) < 0) {
                err = ebuf; close(); return false;
            }
        }
        std::string greeting;
        if (!read_logical_line(greeting, e)) { close(); err = e; return false; }
        if (!starts_with(greeting, "* OK") && !starts_with(greeting, "* PREAUTH")) {
            err = "server did not offer IMAP service: " + greeting; close(); return false;
        }
        auto read_caps = [&]() {
            m_caps.clear();
            std::vector<std::string> un;
            std::string cap_err;
            if (run_once("CAPABILITY", un, cap_err)) {
                for (const std::string &line : un) {
                    if (!starts_with(line, "* CAPABILITY")) continue;
                    std::istringstream iss(line.substr(12));
                    std::string tok;
                    while (iss >> tok) { for (char &c : tok) c = (char)std::toupper((unsigned char)c); m_caps.insert(tok); }
                }
            }
        };
        read_caps();
        if (m_port != 993 && m_caps.count("LOGINDISABLED") && m_caps.count("STARTTLS")) {
            std::vector<std::string> un; std::string tls_err;
            if (!run_once("STARTTLS", un, tls_err)) { err = "server requires TLS but refused STARTTLS: " + tls_err; close(); return false; }
            if (nmail_sock_starttls(m_fd, ebuf, sizeof(ebuf)) < 0) { err = ebuf; close(); return false; }
            read_caps();
        }
        if (!starts_with(greeting, "* PREAUTH")) {
            if (!authenticate(m_user, m_pass, e)) { close(); err = e; return false; }
        }
    }
    if (!saved_folder.empty()) {
        std::vector<std::string> un; std::string se;
        if (run_once("SELECT " + quote(saved_folder), un, se))
            m_selected_folder = saved_folder;
    }
    err.clear();
    return true;
}

bool ImapClient::ensure_selected(const std::string &folder, std::string &err) {
    if (folder.empty()) return true;
    if (m_selected_folder == folder) return true;
    int exists = 0;
    if (!select_folder(folder, exists, err)) return false;
    m_selected_folder = folder;
    return true;
}

std::string ImapClient::quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + '"';
}

bool ImapClient::read_bytes(size_t n, std::string &out, std::string &err) {
    out.clear();
    while (out.size() < n) {
        if (!m_rbuf.empty()) {
            size_t take = std::min(n - out.size(), m_rbuf.size());
            out += m_rbuf.substr(0, take);
            m_rbuf.erase(0, take);
            continue;
        }
        char buf[16384];
        int r = nmail_sock_recv(m_fd, buf, sizeof(buf));
        if (r == -2) { err = "timed out waiting for the server"; return false; }
        if (r <= 0)  { err = "connection to the server was lost"; return false; }
        m_rbuf.append(buf, (size_t)r);
    }
    return true;
}

bool ImapClient::read_crlf_line(std::string &out, std::string &err) {
    for (;;) {
        size_t nl = m_rbuf.find('\n');
        if (nl != std::string::npos) {
            out = m_rbuf.substr(0, nl);
            m_rbuf.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        char buf[16384];
        int r = nmail_sock_recv(m_fd, buf, sizeof(buf));
        if (r == -2) { err = "timed out waiting for the server"; return false; }
        if (r <= 0)  { err = "connection to the server was lost"; return false; }
        m_rbuf.append(buf, (size_t)r);
    }
}

bool ImapClient::read_logical_line(std::string &out, std::string &err) {
    if (!read_crlf_line(out, err)) return false;
    /* While the line ends in "{n}", append the literal and continue. */
    for (;;) {
        size_t close = out.rfind('}');
        if (close == std::string::npos || close + 1 != out.size()) return true;
        size_t open = out.rfind('{', close);
        if (open == std::string::npos) return true;
        size_t n = 0;
        bool digits = true;
        for (size_t i = open + 1; i < close; ++i) {
            if (!std::isdigit((unsigned char)out[i])) { digits = false; break; }
            n = n * 10 + (size_t)(out[i] - '0');
        }
        if (!digits || close == open + 1) return true;

        std::string lit, rest;
        if (!read_bytes(n, lit, err)) return false;
        out += "\r\n" + lit;
        /* The remainder of the response follows on the next line. */
        std::string tail;
        if (!read_crlf_line(tail, err)) return false;
        out += tail;
    }
}

std::string ImapClient::send_with_tag(const std::string &cmd) {
    std::string tag = "a" + std::to_string(++m_tag);
    std::string wire = tag + " " + cmd + "\r\n";
    if (nmail_sock_send(m_fd, wire.data(), (int)wire.size()) < 0)
        return "";
    return tag;
}

bool ImapClient::wait_tagged(const std::string &tag,
                             std::vector<std::string> &untagged,
                             std::string &err) {
    for (;;) {
        std::string line;
        if (!read_logical_line(line, err)) return false;
        // Server-initiated BYE (idle timeout) — treat as connection loss
        // so the caller can reconnect rather than surfacing "BYE" as a
        // command failure.
        if (starts_with(line, "* BYE")) {
            err = "connection to the server was lost (" + line + ")";
            return false;
        }
        if (starts_with(line, tag + " ")) {
            std::string rest = line.substr(tag.size() + 1);
            if (starts_with(rest, "OK")) return true;
            err = rest;           // NO / BAD + server message
            return false;
        }
        untagged.push_back(line);
    }
}

bool ImapClient::run_once(const std::string &cmd,
                          std::vector<std::string> &untagged, std::string &err) {
    untagged.clear();
    std::string tag = send_with_tag(cmd);
    if (tag.empty()) {
        err = "failed to send command (connection lost)";
        return false;
    }
    return wait_tagged(tag, untagged, err);
}

bool ImapClient::run(const std::string &cmd,
                     std::vector<std::string> &untagged, std::string &err) {
    std::string err1;
    if (run_once(cmd, untagged, err1)) return true;
    if (!is_connection_error(err1)) { err = err1; return false; }
    std::string re_err;
    if (!reconnect(re_err)) {
        err = err1 + " (reconnect failed: " + re_err + ")";
        return false;
    }
    untagged.clear();
    if (!run_once(cmd, untagged, err)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// ImapClient — authentication
// ---------------------------------------------------------------------------

/* One AUTHENTICATE mechanism.  Each step function receives the base64
 * challenge text ("" when absent) and returns the base64 response line.
 * Handles the initial-response (SASL-IR) form first, then the two-step
 * challenge/response form if the server rejects it. */
bool ImapClient::auth_mechanism(const std::string &mech,
                                const std::string &user,
                                const std::string &pass, std::string &err) {
    using StepFn = std::function<std::string(const std::string &)>;
    std::vector<StepFn> steps;

    if (mech == "PLAIN") {
        std::string ir = base64_encode(std::string(1, '\0') + user +
                                       std::string(1, '\0') + pass);
        /* Try single-line SASL-IR form. */
        std::string tag = send_with_tag("AUTHENTICATE PLAIN " + ir);
        if (tag.empty()) { err = "connection lost"; return false; }
        std::vector<std::string> un;
        std::string ir_err;
        if (wait_tagged(tag, un, ir_err)) return true;
        /* Fall through to the two-step form below. */
        steps.push_back([ir](const std::string &) { return ir; });
    } else if (mech == "LOGIN") {
        steps.push_back([user](const std::string &) {
            return base64_encode(user);
        });
        steps.push_back([pass](const std::string &) {
            return base64_encode(pass);
        });
    } else if (mech == "CRAM-MD5") {
        steps.push_back([user, pass](const std::string &challenge_b64) {
            std::string challenge = base64_decode(challenge_b64);
            std::string digest = hmac_md5_hex(pass, challenge);
            return base64_encode(user + " " + digest);
        });
    } else {
        err = "unsupported mechanism";
        return false;
    }

    /* Two-step challenge/response form. */
    std::string tag = send_with_tag("AUTHENTICATE " + mech);
    if (tag.empty()) { err = "connection lost"; return false; }
    for (const StepFn &step : steps) {
        std::string line;
        if (!read_logical_line(line, err)) return false;
        if (starts_with(line, tag + " ")) {
            err = line.substr(tag.size() + 1);   // tagged NO/BAD early
            return false;
        }
        if (line.empty() || line[0] != '+') {
            err = "unexpected server response: " + line;
            return false;
        }
        std::string challenge = trim(line.substr(1));
        std::string resp = step(challenge) + "\r\n";
        if (nmail_sock_send(m_fd, resp.data(), (int)resp.size()) < 0) {
            err = "connection lost";
            return false;
        }
    }
    std::vector<std::string> un;
    return wait_tagged(tag, un, err);
}

bool ImapClient::authenticate(const std::string &user,
                              const std::string &pass, std::string &err) {
    std::string last_err;

    /* 1. LOGIN command, unless the server forbids it. */
    if (!m_caps.count("LOGINDISABLED")) {
        std::vector<std::string> un;
        std::string e;
        if (run_once("LOGIN " + quote(user) + " " + quote(pass), un, e))
            return true;
        last_err = "LOGIN: " + e;
    }

    /* 2. AUTHENTICATE with the mechanisms the server advertises. */
    static const char *mechs[] = { "PLAIN", "LOGIN", "CRAM-MD5" };
    for (const char *mech : mechs) {
        if (!m_caps.count(std::string("AUTH=") + mech)) continue;
        std::string e;
        if (auth_mechanism(mech, user, pass, e))
            return true;
        last_err = std::string("AUTHENTICATE ") + mech + ": " + e;
    }

    /* Nothing worked — report what the server actually offers. */
    std::string caps;
    for (const auto &c : m_caps) caps += (caps.empty() ? "" : " ") + c;
    err = last_err.empty() ? "no supported authentication method" : last_err;
    if (m_caps.count("AUTH=XOAUTH2") || m_caps.count("AUTH=OAUTHBEARER"))
        err += " (the server requires OAuth2, which nmail does not "
               "support yet)";
    err += "\nServer capabilities: " + (caps.empty() ? "(none)" : caps);
    return false;
}

// ---------------------------------------------------------------------------
// ImapClient — protocol operations
// ---------------------------------------------------------------------------

bool ImapClient::open(const std::string &host, int port,
                      const std::string &user, const std::string &pass,
                      std::string &err) {
    close();
    m_caps.clear();
    m_selected_folder.clear();
    char ebuf[512] = {0};
    m_fd = nmail_sock_connect(host.c_str(), port, ebuf, sizeof(ebuf));
    if (m_fd < 0) { err = ebuf; return false; }

    /* Implicit TLS (imaps, port 993) before the greeting. */
    if (port == 993) {
        if (nmail_sock_starttls(m_fd, ebuf, sizeof(ebuf)) < 0) {
            err = ebuf;
            close();
            return false;
        }
    }

    std::string greeting;
    if (!read_logical_line(greeting, err)) { close(); return false; }
    if (!starts_with(greeting, "* OK") && !starts_with(greeting, "* PREAUTH")) {
        err = "server did not offer IMAP service: " + greeting;
        close();
        return false;
    }

    /* Discover what the server allows before picking an auth method. */
    auto read_caps = [&]() {
        m_caps.clear();
        std::vector<std::string> un;
        std::string cap_err;
        if (run_once("CAPABILITY", un, cap_err)) {
            for (const std::string &line : un) {
                if (!starts_with(line, "* CAPABILITY")) continue;
                std::istringstream iss(line.substr(12));
                std::string tok;
                while (iss >> tok) {
                    for (char &c : tok) c = (char)std::toupper((unsigned char)c);
                    m_caps.insert(tok);
                }
            }
        }
    };
    read_caps();

    /* Plaintext auth disabled but STARTTLS offered: upgrade, then the
     * capabilities must be re-read (they differ once TLS is active). */
    if (port != 993 && m_caps.count("LOGINDISABLED") &&
        m_caps.count("STARTTLS")) {
        std::vector<std::string> un;
        std::string tls_err;
        if (!run_once("STARTTLS", un, tls_err)) {
            err = "server requires TLS but refused STARTTLS: " + tls_err;
            close();
            return false;
        }
        if (nmail_sock_starttls(m_fd, ebuf, sizeof(ebuf)) < 0) {
            err = ebuf;
            close();
            return false;
        }
        read_caps();
    }

    /* PREAUTH means the connection is already authenticated. */
    if (starts_with(greeting, "* PREAUTH")) {
        m_host = host; m_port = port; m_user = user; m_pass = pass;
        m_selected_folder.clear();
        return true;
    }

    if (!authenticate(user, pass, err)) {
        close();
        return false;
    }
    m_host = host; m_port = port; m_user = user; m_pass = pass;
    m_selected_folder.clear();
    return true;
}

bool ImapClient::list_folders(std::vector<MailFolder> &out, std::string &err) {
    out.clear();
    std::vector<std::string> untagged;
    if (!run("LIST \"\" *", untagged, err)) return false;

    for (const std::string &line : untagged) {
        if (!starts_with(line, "* LIST")) continue;
        ImapCursor cur(line);
        cur.skip_ws(); cur.expect('*'); cur.skip_ws();
        while (!cur.eof() && cur.peek() != ' ') ++cur.pos;   // "LIST"
        cur.skip_ws();
        std::string flags = cur.read_parens();
        if (to_lower(flags).find("\\noselect") != std::string::npos)
            continue;
        cur.skip_ws();
        if (cur.peek() == '"') cur.read_quoted();            // delimiter
        else while (!cur.eof() && cur.peek() != ' ') ++cur.pos; // NIL
        cur.skip_ws();
        std::string name = (cur.peek() == '"')
            ? cur.read_quoted() : trim(line.substr(cur.pos));
        if (name.empty()) continue;
        MailFolder f;
        f.name = name;
        out.push_back(f);
    }

    /* INBOX first, then alphabetical. */
    std::sort(out.begin(), out.end(), [](const MailFolder &a,
                                         const MailFolder &b) {
        bool ai = to_lower(a.name) == "inbox";
        bool bi = to_lower(b.name) == "inbox";
        if (ai != bi) return ai;
        return to_lower(a.name) < to_lower(b.name);
    });

    /* Message/unseen counts; a folder that rejects STATUS is left at 0. */
    for (MailFolder &f : out) {
        std::vector<std::string> st;
        std::string sterr;
        if (!run("STATUS " + quote(f.name) + " (MESSAGES UNSEEN)", st, sterr))
            continue;
        for (const std::string &line : st) {
            if (!starts_with(line, "* STATUS")) continue;
            size_t lp = line.rfind('(');
            size_t rp = line.rfind(')');
            if (lp == std::string::npos || rp <= lp) continue;
            std::string inner = line.substr(lp + 1, rp - lp - 1);
            ImapCursor c2(inner);
            while (!c2.eof()) {
                c2.skip_ws();
                size_t start = c2.pos;
                while (!c2.eof() && c2.peek() != ' ') ++c2.pos;
                std::string key = to_lower(inner.substr(start, c2.pos - start));
                c2.skip_ws();
                int val = 0;
                while (!c2.eof() && std::isdigit((unsigned char)c2.peek()))
                    val = val * 10 + (c2.s[c2.pos++] - '0');
                if (key == "messages") f.messages = val;
                else if (key == "unseen") f.unseen = val;
            }
        }
    }
    return true;
}

bool ImapClient::select_folder(const std::string &name, int &exists,
                               std::string &err) {
    exists = 0;
    std::vector<std::string> untagged;
    if (!run("SELECT " + quote(name), untagged, err)) return false;
    for (const std::string &line : untagged) {
        if (line.size() > 2 && line[0] == '*' &&
            line.find(" EXISTS") != std::string::npos)
            exists = std::atoi(line.c_str() + 1);
    }
    m_selected_folder = name;
    return true;
}

static bool parse_summaries(const std::vector<std::string> &untagged,
                            std::vector<MailSummary> &out) {
    for (const std::string &line : untagged) {
        if (!starts_with(line, "* ") || line.find(" FETCH") == std::string::npos)
            continue;
        MailSummary sum;
        sum.seq = std::atoi(line.c_str() + 2);
        std::string headers, text;
        for (auto &kv : parse_fetch_items(line)) {
            const std::string &key = kv.first;
            const std::string &val = kv.second;
            if (key == "FLAGS") {
                if (to_lower(val).find("\\seen") != std::string::npos)
                    sum.seen = true;
            } else if (key == "INTERNALDATE") {
                sum.date = format_internaldate(val);
            } else if (starts_with(key, "BODY[HEADER")) {
                headers = val;
            } else if (starts_with(key, "BODY[TEXT")) {
                text = val;
            }
        }
        auto h = parse_headers(headers);
        auto get = [&](const char *k) -> std::string {
            auto it = h.find(k);
            return it == h.end() ? "" : it->second;
        };
        sum.from    = display_from(get("from"));
        sum.subject = decode_encoded_words(get("subject"));
        if (sum.subject.empty()) sum.subject = "(no subject)";
        sum.preview = sanitize_preview(text);
        out.push_back(sum);
    }
    return true;
}

bool ImapClient::fetch_summaries(int first, int last,
                                 std::vector<MailSummary> &out,
                                 std::string &err) {
    out.clear();
    if (first > last) return true;

    std::string range = std::to_string(first) + ":" + std::to_string(last);
    std::vector<std::string> untagged;
    /* Some servers reject partial body fetches; fall back if needed. */
    if (!run("FETCH " + range +
             " (FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)]"
             " BODY.PEEK[TEXT]<0.512>)", untagged, err)) {
        err.clear();
        if (!run("FETCH " + range +
                 " (FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])",
                 untagged, err))
            return false;
    }
    return parse_summaries(untagged, out);
}

bool ImapClient::fetch_message(int seq, MailMessage &msg, std::string &err) {
    std::vector<std::string> untagged;
    if (!run("FETCH " + std::to_string(seq) + " (BODY.PEEK[])",
             untagged, err))
        return false;

    std::string raw;
    for (const std::string &line : untagged) {
        if (!starts_with(line, "* ") || line.find(" FETCH") == std::string::npos)
            continue;
        for (auto &kv : parse_fetch_items(line)) {
            if (starts_with(kv.first, "BODY[")) { raw = kv.second; break; }
        }
        if (!raw.empty()) break;
    }
    if (raw.empty()) {
        err = "the server returned no message data";
        return false;
    }

    std::string head, body;
    split_head_body(raw, head, body);
    auto h = parse_headers(head);
    auto get = [&](const char *k) -> std::string {
        auto it = h.find(k);
        return it == h.end() ? "" : it->second;
    };
    msg.from    = display_from(get("from"));
    msg.from_addr = address_of(get("from"));
    msg.to      = display_from(get("to"));
    msg.subject = decode_encoded_words(get("subject"));
    if (msg.subject.empty()) msg.subject = "(no subject)";
    msg.date       = decode_encoded_words(get("date"));
    msg.message_id = trim(get("message-id"));

    std::string plain, html;
    bool plain_markdown = false;
    mime_extract_parts(head, body, plain, html, plain_markdown,
                       msg.images, 0);
    msg.html = html;
    msg.body = !plain.empty() ? plain
             : !html.empty()  ? strip_html(html)
                              : body;
    msg.body_markdown = plain_markdown && !plain.empty();
    return true;
}
