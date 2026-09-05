/*
 * imap_client.h — minimal blocking IMAP4rev1 client for nmail.
 *
 * Auto-reconnects after an idle timeout: credentials and selected mailbox
 * are sticky and run() will LOGIN+SELECT once on connection loss.
 */
#ifndef IMAP_CLIENT_H
#define IMAP_CLIENT_H

/* Set to 1 (or compile with -DNMAIL_IMAP_DEBUG=1) to log IMAP commands,
 * FETCH progress, and worker folder-switch traces to stderr. */
#ifndef NMAIL_IMAP_DEBUG
#define NMAIL_IMAP_DEBUG 0
#endif

#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <cstdint>

struct MailFolder {
    std::string name;
    int messages = 0;
    int unseen   = 0;
};

struct MailSummary {
    int seq = 0;               // IMAP message sequence number
    uint32_t uid = 0;          // UID (when CONDSTORE/QRESYNC available)
    uint64_t modseq = 0;       // MODSEQ (RFC 4551)
    std::string from;          // display name, or the address if none was given
    std::string from_addr;     // bare address of the sender
    std::string subject;
    std::string date;          // formatted for display
    std::string preview;
    bool seen = false;
};

/* One entry of an RFC 822 address list. */
struct MailAddress {
    std::string name;          // display name (RFC 2047 decoded), "" when absent
    std::string address;       // bare addr-spec, e.g. "jane@example.com"
};

/* Split an address header ("A <a@x>, b@y, \"Doe, J\" <j@z>") into its parts.
 * Commas inside quotes or angle brackets do not split.  Entries without a
 * plausible address are dropped, so the result may be shorter than the input. */
std::vector<MailAddress> parse_address_list(const std::string &raw);

struct MailImage {
    std::string cid;          // Content-ID without angle brackets ("" if none)
    std::string mime;         // e.g. "image/png"
    std::string data;         // decoded image bytes (PNG/JPEG/GIF)
};

struct MailAttachment {
    std::string filename;     // display name (may be empty)
    std::string mime;         // e.g. "application/pdf"
    std::string cid;          // Content-ID without brackets ("" if none)
    std::string data;         // decoded payload
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
    std::string raw;           // original RFC 822 bytes (IMAP BODY[])
    bool body_markdown = false; // text part declared markup=markdown
                                // (MailMate convention) or text/markdown
    std::vector<MailImage> images;   // inline image/* parts (for cid: srcs)
    std::vector<MailAttachment> attachments; // non-body MIME parts
};

/* Parse a complete RFC 822 / MIME message (the IMAP BODY[] payload, or
 * a saved .eml file).  Fills `msg` including `msg.raw`. */
bool parse_rfc822_message(const std::string &raw, MailMessage &msg);

/* Derive a collapsed preview snippet (<=160 chars) from a fully fetched
 * message — prefers the plain body, falls back to stripped HTML. */
std::string message_preview(const MailMessage &msg);

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

    /* STATUS name (MESSAGES [UNSEEN]); used to cross-check SELECT EXISTS. */
    bool status_counts(const std::string &name, int &messages, int &unseen,
                       std::string &err);

    /* FETCH header summaries + a small text preview for seq [first, last].
     * Uses BODY.PEEK[HEADER.FIELDS ...] + BODY.PEEK[TEXT]<0.512> so a 2 GB
     * attachment is never fetched — only 512 bytes for preview.  Callers
     * that need the full body should use fetch_message(). */
    bool fetch_summaries(int first, int last,
                         std::vector<MailSummary> &out, std::string &err);

    /* FETCH the full raw message and extract a readable plain-text body.
     * If `still_wanted` is set, it is checked after the IMAP round-trip
     * and before MIME decode — so a folder switch can skip a large body.
     * Caps: messages larger than kMaxBodyBytes return an error instead of
     * OOMing the UI; use body_size_guess()/fetch_message_peek() for preview,
     * or BODY.PEEK[TEXT]<0.N> chunked reads for huge messages. */
    bool fetch_message(int seq, MailMessage &msg, std::string &err,
                       const std::function<bool()> &still_wanted = {});
    // UID variant for QRESYNC: stable identifier, uses UID FETCH.
    bool fetch_message_by_uid(uint32_t uid, MailMessage &msg, std::string &err,
                              const std::function<bool()> &still_wanted = {});
    // Cheap RFC822.SIZE probe (no body transfer).  Use to decide whether to
    // fetch_message() or show "too large" placeholder.
    bool body_size_guess(int seq, size_t &bytes, std::string &err);
    bool body_size_guess_uid(uint32_t uid, size_t &bytes, std::string &err);
    static constexpr size_t kMaxBodyBytes = 20ull * 1024 * 1024; // 20 MiB
    static constexpr size_t kPeekLimit    = 512; // preview-only_bytes in FETCH summaries

    /* Move a single message (by sequence number) to another folder.
     * Tries IMAP MOVE when advertised, otherwise falls back to
     * COPY + STORE +FLAGS \Deleted + EXPUNGE.  Creates the
     * destination folder on TRYCREATE.  Caller must have the source
     * folder SELECTed. */
    bool move_message(int seq, const std::string &dest_folder,
                      std::string &err);

    /* STORE +FLAGS.SILENT (\Seen) on one message, marking it read on the
     * server.  Caller must have the containing folder SELECTed.  Sequence
     * numbers shift on EXPUNGE, so only call this with a seq known current
     * for the selected mailbox. */
    bool mark_seen(int seq, std::string &err);
    /* UID variant with optional CONDSTORE UNCHANGEDSINCE.  When modseq != 0
     * and the server advertises CONDSTORE, sends
     *   UID STORE uid (UNCHANGEDSINCE modseq) +FLAGS.SILENT (\\Seen)
     * per RFC 4551 §3.3 so a concurrent flag change is not clobbered.
     * A NO [MODIFIED ...] response means the modseq guard failed — the
     * message was already modified elsewhere; treat as benign success. */
    bool mark_seen_uid(uint32_t uid, uint64_t modseq, std::string &err);
    static bool is_modified_error(const std::string &err);

    void close();
    bool is_open() const { return m_fd >= 0; }
    const std::string &selected_folder() const { return m_selected_folder; }
    /* Re-LOGIN.  When `reselect` is true, SELECT the previously open
     * mailbox afterwards (idle-timeout recovery).  Folder-switch cancel
     * uses reselect=false so we do not reopen the mailbox the user left. */
    bool reconnect(std::string &err, bool reselect = true);
    static bool is_connection_error(const std::string &err);
    static bool is_cancelled_error(const std::string &err);
    bool ensure_selected(const std::string &folder, std::string &err);

    /* Wake a recv() blocked in another thread (used when shutting down).
     * Does not touch any other state, so it is safe to call concurrently
     * with an in-flight operation. */
    void abort();

    /* Abort the in-flight command so a folder switch does not wait for a
     * 150-message FETCH to drain.  Bumps a generation that run() treats as
     * non-retryable "cancelled"; the socket is unusable afterwards and the
     * caller must reconnect.  Safe to call from the GUI thread. */
    void cancel();
    uint64_t op_gen() const { return m_op_gen.load(std::memory_order_acquire); }

    /* Optional FETCH progress: done/total untagged FETCH lines.
     * Called from the worker thread; keep it cheap. */
    std::function<void(int done, int total)> on_progress;
    void expect_progress(int total) { m_progress_total = total; m_progress_done = 0; }
    void clear_progress() { m_progress_total = 0; m_progress_done = 0; }

    // ── RFC 4551 CONDSTORE / RFC 7162 QRESYNC / RFC 4978 COMPRESS=DEFLATE ──
    bool has_compress_deflate() const;
    bool has_condstore() const { return m_caps.count("CONDSTORE"); }
    // QRESYNC requires CONDSTORE + QRESYNC in CAPABILITY and ENABLE QRESYNC
    // in Authenticated state before first SELECT (RFC 7162 §3.1, RFC 5161).
    bool has_qresync() const { return m_caps.count("QRESYNC") && has_condstore(); }
    bool has_enable() const { return m_caps.count("ENABLE"); }
    bool enable_qresync(std::string &err); // idempotent; NO-OP after first OK
    bool enable_condstore(std::string &err);
    bool compress_deflate(std::string &err); // one-shot per connection, before first SELECT
    bool is_compressed() const { return m_compressed; }

    struct QResyncState {
        uint32_t uidvalidity = 0;
        uint64_t highestmodseq = 0;
        uint32_t uidnext = 0;
        uint32_t messages = 0;
    };
    struct QResyncDelta {
        std::vector<uint32_t> vanished;                         // VANISHED (EARLIER)
        std::unordered_map<uint32_t, std::vector<std::string>> // UID -> flag list
            changed_flags;
        std::unordered_map<uint32_t, uint64_t> modseqs; // UID -> MODSEQ
        std::vector<uint32_t> known_uids_missing; // UIDs we thought existed but server dropped without VANISHED (fallback)
    };
    // Persisted per-folder sync anchor.  Pass 0/bad state to fall back to plain SELECT.
    bool select_qresync(const std::string &name,
                        const QResyncState &known,
                        const std::vector<uint32_t> &known_uids,
                        QResyncState &out_state, QResyncDelta &out_delta,
                        std::string &err);
    // Cheap incremental sync after IDLE/NOOP: ask for VANISHED + changed since modseq.
    bool qresync_delta(uint64_t since_modseq, QResyncDelta &out, std::string &err);
    bool fetch_flags_uid(const std::vector<uint32_t> &uids,
                         std::unordered_map<uint32_t, std::vector<std::string>> &out,
                         std::string &err);
    uint32_t uidvalidity() const { return m_qresync.uidvalidity; }
    uint64_t highestmodseq() const { return m_qresync.highestmodseq; }
    const QResyncState& qresync_state() const { return m_qresync; }

private:
    int m_fd = -1;
    int m_tag = 0;
    std::string m_rbuf;        // pending bytes from the socket (decompressed if m_compressed)
    std::string m_compress_rbuf; // raw deflated bytes waiting to inflate (unused, kept for compat)
    std::set<std::string> m_caps;  // server capabilities (uppercase)
    // RFC 4978 COMPRESS=DEFLATE (miniz inflate/deflate state; valid only when m_compressed)
    void *m_inflate_state = nullptr;  // mz_stream*
    void *m_deflate_state = nullptr;
    bool m_compressed = false;
    bool m_qresync_enabled = false;
    QResyncState m_qresync; // last SELECT's UIDVALIDITY/HIGHESTMODSEQ
    // COMPRESS=DEFLATE helpers (raw DEFLATE, RFC 4978)
    bool deflate_and_send(const std::string &wire, std::string &err);
    bool inflate_more(std::string &err); // read compressed chunk and inflate into m_rbuf
    static std::string uids_to_seqset(const std::vector<uint32_t> &uids);
    static std::vector<uint32_t> seqset_to_uids(const std::string &seqset);
    static bool parse_vanished_line(const std::string &line, std::vector<uint32_t> &out, bool &earlier);
    static bool extract_code_number(const std::string &line, const char *code, uint64_t &out);
    void update_qresync_from_select(const std::vector<std::string> &untagged, int exists);
    // Credentials from the last successful open(), kept to allow a
    // silent reconnect after an idle timeout / server BYE.
    std::string m_host;
    int         m_port = 0;
    std::string m_user;
    std::string m_pass;
    std::string m_selected_folder; // last successfully SELECTed mailbox
    std::atomic<uint64_t> m_op_gen{0}; // bumped by cancel() to drop in-flight cmds
    int m_progress_total = 0;
    int m_progress_done = 0;

    /* Send a tagged command, collect untagged responses until the tagged
     * completion.  Returns false on NO/BAD or I/O error (err explains).
     * The public run() will attempt one silent reconnect on connection
     * loss and retry the command once. */
    bool run(const std::string &cmd, std::vector<std::string> &untagged,
             std::string &err);
    bool run_once(const std::string &cmd, std::vector<std::string> &untagged,
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
