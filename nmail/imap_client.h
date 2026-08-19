/*
 * imap_client.h — minimal blocking IMAP4rev1 client for nmail.
 *
 * Designed to be driven from a single worker thread; every method blocks
 * until the server answers (or the socket times out).  No caching: each
 * call goes back to the server.
 */
#ifndef IMAP_CLIENT_H
#define IMAP_CLIENT_H

#include <string>
#include <vector>
#include <set>
#include <functional>

struct MailFolder {
    std::string name;
    int messages = 0;
    int unseen   = 0;
};

struct MailSummary {
    int seq = 0;               // IMAP message sequence number
    std::string from;
    std::string subject;
    std::string date;          // formatted for display
    std::string preview;
    bool seen = false;
};

struct MailImage {
    std::string cid;          // Content-ID without angle brackets ("" if none)
    std::string mime;         // e.g. "image/png"
    std::string data;         // decoded image bytes (PNG/JPEG/GIF)
};

struct MailMessage {
    std::string from;
    std::string from_addr;     // bare address of the sender (for replies)
    std::string to;
    std::string subject;
    std::string date;
    std::string body;          // decoded plain text (best effort)
    std::string html;          // decoded text/html part, "" when none
    std::string message_id;    // Message-ID header (for reply threading)
    bool body_markdown = false; // text part declared markup=markdown
                                // (MailMate convention) or text/markdown
    std::vector<MailImage> images;   // inline image/* parts (for cid: srcs)
};

class ImapClient {
public:
    ImapClient() = default;
    ~ImapClient();

    /* Connect, read the greeting, and LOGIN. */
    bool open(const std::string &host, int port,
              const std::string &user, const std::string &pass,
              std::string &err);

    /* LIST all folders and STATUS each one for message/unseen counts. */
    bool list_folders(std::vector<MailFolder> &out, std::string &err);

    /* SELECT a folder; returns the number of messages it contains. */
    bool select_folder(const std::string &name, int &exists, std::string &err);

    /* FETCH header summaries + a small text preview for seq [first, last]. */
    bool fetch_summaries(int first, int last,
                         std::vector<MailSummary> &out, std::string &err);

    /* FETCH the full raw message and extract a readable plain-text body. */
    bool fetch_message(int seq, MailMessage &msg, std::string &err);

    void close();
    bool is_open() const { return m_fd >= 0; }

    /* Wake a recv() blocked in another thread (used when shutting down).
     * Does not touch any other state, so it is safe to call concurrently
     * with an in-flight operation. */
    void abort();

private:
    int m_fd = -1;
    int m_tag = 0;
    std::string m_rbuf;        // pending bytes from the socket
    std::set<std::string> m_caps;  // server capabilities (uppercase)

    /* Send a tagged command, collect untagged responses until the tagged
     * completion.  Returns false on NO/BAD or I/O error (err explains). */
    bool run(const std::string &cmd, std::vector<std::string> &untagged,
             std::string &err);

    /* Lower-level command plumbing (used by multi-step AUTHENTICATE). */
    std::string send_with_tag(const std::string &cmd);
    bool wait_tagged(const std::string &tag,
                     std::vector<std::string> &untagged, std::string &err);

    /* CAPABILITY-driven authentication.  Tries, as advertised and skipping
     * what the server forbids: LOGIN, AUTHENTICATE PLAIN / LOGIN / CRAM-MD5. */
    bool authenticate(const std::string &user, const std::string &pass,
                      std::string &err);
    bool auth_mechanism(const std::string &mech, const std::string &user,
                        const std::string &pass, std::string &err);

    /* Read one logical response: a CRLF line, with {n} literals expanded
     * inline (kept as "{n}\r\n<bytes>" inside the string). */
    bool read_logical_line(std::string &out, std::string &err);
    bool read_crlf_line(std::string &out, std::string &err);
    bool read_bytes(size_t n, std::string &out, std::string &err);

    static std::string quote(const std::string &s);
};

#endif /* IMAP_CLIENT_H */
