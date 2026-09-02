#include "contacts.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

#include "dict.h"

namespace {

std::string to_lower(std::string s) {
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/* A name that is just the address back again carries no extra information. */
bool name_is_useful(const std::string &name, const std::string &address) {
    if (name.empty()) return false;
    return to_lower(name) != address;
}

/* Ranking buckets, lower is better.  kNoMatch drops the candidate. */
enum { kExactAddr = 0, kExactName, kPrefixAddr, kPrefixName,
       kPrefixWord, kSubstrAddr, kSubstrName, kNoMatch };

int match_score(const Contact &c, const std::string &q) {
    const std::string name = to_lower(c.name);
    if (c.address == q)                    return kExactAddr;
    if (!name.empty() && name == q)        return kExactName;
    if (c.address.compare(0, q.size(), q) == 0) return kPrefixAddr;
    if (name.compare(0, q.size(), q) == 0 && !name.empty()) return kPrefixName;

    /* "doe" should find "Jane Doe" — match the start of any word. */
    for (size_t i = 0; i + 1 < name.size(); ++i) {
        if ((name[i] == ' ' || name[i] == '.' || name[i] == '-') &&
            name.compare(i + 1, q.size(), q) == 0)
            return kPrefixWord;
    }
    if (c.address.find(q) != std::string::npos) return kSubstrAddr;
    if (!name.empty() && name.find(q) != std::string::npos) return kSubstrName;
    return kNoMatch;
}

} // namespace

bool ContactStore::observe(const std::string &name, const std::string &address) {
    const std::string addr = to_lower(trim(address));
    if (addr.empty() || addr.find('@') == std::string::npos)
        return false;

    const std::string nm = trim(name);
    auto it = m_by_addr.find(addr);
    if (it == m_by_addr.end()) {
        Contact c;
        c.address = addr;
        c.name    = name_is_useful(nm, addr) ? nm : "";
        c.hits    = 1;
        m_by_addr.emplace(addr, c);
        m_dirty = true;
        return true;
    }

    ++it->second.hits;
    /* Fill in a name we did not have before; otherwise keep the first one so
     * a single oddly-formatted message cannot rewrite a good entry. */
    if (it->second.name.empty() && name_is_useful(nm, addr)) {
        it->second.name = nm;
        m_dirty = true;
        return true;
    }
    m_dirty = true;
    return false;
}

bool ContactStore::observe_header(const std::string &raw_header) {
    bool added = false;
    for (const MailAddress &a : parse_address_list(raw_header))
        added |= observe(a.name, a.address);
    return added;
}

const Contact *ContactStore::find(const std::string &address) const {
    auto it = m_by_addr.find(to_lower(trim(address)));
    return it == m_by_addr.end() ? nullptr : &it->second;
}

std::vector<Contact> ContactStore::search(const std::string &query,
                                          size_t limit) const {
    const std::string q = to_lower(trim(query));

    std::vector<std::pair<int, const Contact *>> scored;
    scored.reserve(m_by_addr.size());
    for (const auto &kv : m_by_addr) {
        int score = q.empty() ? 0 : match_score(kv.second, q);
        if (score == kNoMatch) continue;
        scored.emplace_back(score, &kv.second);
    }

    std::sort(scored.begin(), scored.end(),
              [](const std::pair<int, const Contact *> &a,
                 const std::pair<int, const Contact *> &b) {
                  if (a.first != b.first) return a.first < b.first;
                  if (a.second->hits != b.second->hits)
                      return a.second->hits > b.second->hits;
                  /* Stable, predictable order for equally good matches. */
                  return a.second->address < b.second->address;
              });

    std::vector<Contact> out;
    for (const auto &s : scored) {
        if (out.size() >= limit) break;
        out.push_back(*s.second);
    }
    return out;
}

std::string format_address(const Contact &c) {
    if (c.name.empty())
        return c.address;
    /* Characters that would otherwise split or confuse an address list. */
    const bool needs_quotes =
        c.name.find_first_of(",;:<>@\"()[]\\") != std::string::npos;
    if (!needs_quotes)
        return c.name + " <" + c.address + ">";

    std::string escaped;
    for (char ch : c.name) {
        if (ch == '"' || ch == '\\') escaped += '\\';
        escaped += ch;
    }
    return "\"" + escaped + "\" <" + c.address + ">";
}

bool ContactStore::load(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return false;

    char err[256] = {0};
    DictValue *root = dict_deserialize_json_len(text.c_str(), text.size(),
                                                err, sizeof(err));
    if (!root) return false;

    const DictValue *list = dict_object_get(root, "contacts");
    if (list && list->type == DICT_ARRAY) {
        for (size_t i = 0; i < list->array_value.length; ++i) {
            const DictValue *e = list->array_value.items[i];
            if (!e || e->type != DICT_OBJECT) continue;

            const DictValue *a = dict_object_get(e, "address");
            const DictValue *n = dict_object_get(e, "name");
            const DictValue *h = dict_object_get(e, "hits");
            if (!a || a->type != DICT_STRING || !a->string_value) continue;

            Contact c;
            c.address = to_lower(trim(a->string_value));
            if (c.address.empty() || c.address.find('@') == std::string::npos)
                continue;
            if (n && n->type == DICT_STRING && n->string_value)
                c.name = n->string_value;
            if (h && h->type == DICT_INT64)  c.hits = (int)h->int64_value;
            if (h && h->type == DICT_NUMBER) c.hits = (int)h->number_value;
            if (c.hits < 1) c.hits = 1;
            m_by_addr[c.address] = c;
        }
    }
    dict_destroy(root);
    m_dirty = false;
    return true;
}

bool ContactStore::save(const std::string &path) {
    /* Most-seen first, so a truncated or hand-edited file keeps the useful
     * entries and the JSON diffs sensibly between runs. */
    std::vector<const Contact *> all;
    all.reserve(m_by_addr.size());
    for (const auto &kv : m_by_addr) all.push_back(&kv.second);
    std::sort(all.begin(), all.end(),
              [](const Contact *a, const Contact *b) {
                  if (a->hits != b->hits) return a->hits > b->hits;
                  return a->address < b->address;
              });

    DictValue *root = dict_create_object();
    DictValue *arr  = dict_create_array();
    for (const Contact *c : all) {
        DictValue *e = dict_create_object();
        dict_object_set(e, "name",    dict_create_string(c->name.c_str()));
        dict_object_set(e, "address", dict_create_string(c->address.c_str()));
        dict_object_set(e, "hits",    dict_create_int64(c->hits));
        dict_array_append(arr, e);
    }
    dict_object_set(root, "contacts", arr);

    /* dict_serialize_json needs a caller-provided buffer; size it for the
     * worst case rather than guessing with a fixed array. */
    size_t cap = 256;
    for (const Contact *c : all)
        cap += c->name.size() + c->address.size() + 96;
    std::vector<char> buf(cap);

    bool ok = false;
    if (dict_serialize_json(root, buf.data(), buf.size(), /*pretty=*/1)) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out << buf.data();
            ok = (bool)out;
        }
    }
    dict_destroy(root);
    if (ok) m_dirty = false;
    return ok;
}
