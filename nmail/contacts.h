/*
 * contacts.h — address book harvested from message headers.
 *
 * Every summary and every fetched body contributes its correspondents, so the
 * store fills in as folders are listed and messages are read.  It is the
 * completion source behind the To: field in the compose window.
 */
#ifndef NMAIL_CONTACTS_H
#define NMAIL_CONTACTS_H

#include <string>
#include <vector>
#include <unordered_map>

#include "imap_client.h"        // MailAddress, parse_address_list

/* One harvested correspondent. */
struct Contact {
    std::string name;       // best display name seen; "" if only ever a bare address
    std::string address;    // lowercased addr-spec; unique key of the store
    int         hits = 0;   // times observed — ranks equally good matches
};

/*
 * Not thread safe by design: the worker delivers its callbacks on the GUI
 * thread, so harvesting and searching both happen there.
 */
class ContactStore {
public:
    /* Record one correspondent.  Returns true when this added a contact or
     * replaced a placeholder name with a real one. */
    bool observe(const std::string &name, const std::string &address);

    /* Record every address in a From:/To:/Cc: header value. */
    bool observe_header(const std::string &raw_header);

    /* Matches on name or address, best first.  An empty query returns the
     * most frequently seen contacts, which is what an empty To: field wants. */
    std::vector<Contact> search(const std::string &query,
                                size_t limit = 8) const;

    const Contact *find(const std::string &address) const;

    size_t size()  const { return m_by_addr.size(); }
    bool   dirty() const { return m_dirty; }

    /* JSON at <config dir>/contacts.json.  A missing file is not an error;
     * load() simply leaves the store empty and returns false. */
    bool load(const std::string &path);
    bool save(const std::string &path);

private:
    std::unordered_map<std::string, Contact> m_by_addr;
    bool m_dirty = false;
};

/* {"Jane Doe", "jane@x.com"} -> "Jane Doe <jane@x.com>", quoting the display
 * name when it contains characters that would break the address list. */
std::string format_address(const Contact &c);

#endif // NMAIL_CONTACTS_H
