/*
 * nmail/mail_worker.h — MailWorker: worker thread owning the blocking
 * IMAP connection.
 *
 * The GUI never blocks on IMAP: every operation (connect, select folder,
 * fetch summaries/bodies, move, mark-seen, prefetch) is posted as a
 * command to a queue drained by the worker thread, and results are
 * marshalled back to the GUI thread via nanogui::async callbacks.
 */
#pragma once

#include "nmail_config.h"
#include "imap_client.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class MailWorker {
public:
    /* GUI-thread callbacks (set before start(), each invoked via
     * nanogui::async so they always run on the main thread). */
    std::function<void(const std::vector<MailFolder> &)>        cb_folders;
    std::function<void(const std::string &,
                       const std::vector<MailSummary> &)>       cb_summaries;
    /* Periodic background check (see Type::AutoRefresh): delivers the same
     * shape as cb_summaries, but the GUI must merge it in without disturbing
     * the user's scroll position or selection, rather than replacing the
     * list wholesale. */
    std::function<void(const std::string &,
                       const std::vector<MailSummary> &)>       cb_auto_summaries;
    /* Older-message page (appended to the bottom of the list). */
    std::function<void(const std::string &,
                       const std::vector<MailSummary> &)>       cb_older;
    std::function<void(const std::string &, int,
                       const MailMessage &)>                    cb_body;
    /* Background prefetch: folder + seq + full message + derived preview. */
    std::function<void(const std::string &, int,
                       const MailMessage &, const std::string &)> cb_prefetched;
    std::function<void(const std::string &,
                       const std::string &)>                    cb_error;
    std::function<void(const std::string &,
                       const std::string &)>                    cb_status;
    std::function<void(const std::string &, int,
                       const std::string &)>                    cb_moved;
    /* A message was flagged \Seen on the server. */
    std::function<void(const std::string &, int)>               cb_seen;
    /* FETCH summaries progress (worker thread marshals via deliver). */
    std::function<void(const std::string &, int done, int total)> cb_progress;

    void set_config(const MailConfig &c);

    void start();
    void stop();

    void connect()                              { post(Type::Connect); }
    void refresh()                              { post(Type::Refresh); }
    /* Latest mailbox the GUI asked to look at.  Select/Refresh/FetchOlder
     * all key off this so an in-flight INBOX fetch cannot clobber Trash. */
    void select_folder(const std::string &name);
    void fetch_body(int seq);
    void fetch_body(const std::string &folder, int seq) { post(Type::FetchBody, folder, seq); }
    void fetch_older(const std::string &folder)         { post(Type::FetchOlder, folder); }
    /* Flag `seq` in `folder` as \Seen once `delay_sec` has passed without a
     * newer request.  Calling again replaces the pending one, so moving to a
     * different message restarts the clock rather than queueing a second
     * mark.
     * The overload carrying modseq enables CONDSTORE UNCHANGEDSINCE
     * (RFC 4551 §3.3) via UID STORE when the server advertises CONDSTORE. */
    void schedule_seen(const std::string &folder, int seq, double delay_sec);
    void schedule_seen(const std::string &folder, int seq, uint64_t modseq, double delay_sec);
    void schedule_seen_uid(const std::string &folder, int seq, uint32_t uid, uint64_t modseq, double delay_sec);
    void cancel_seen();
    void move_message(const std::string &folder, int seq,
                      const std::string &dest_folder);
    void ensure_visible_cached(const std::string &folder,
                               const std::vector<int> &visible_seqs);
    // UID-aware prefetch: stable across QRESYNC seq shifts; uses UID FETCH.
    void ensure_visible_cached_uid(const std::string &folder,
                                   const std::vector<uint32_t> &uids);
    // UID body fetch (foreground path uses seq; bg uses uid when available).
    bool fetch_message_by_uid(uint32_t uid, MailMessage &msg, std::string &err,
                              std::function<bool()> still_wanted = {});
    bool is_compressed() const;

private:
    enum class Type { Connect, Refresh, Select, FetchBody, FetchOlder, Prefetch, PrefetchUid, Move, MarkSeen, AutoRefresh };
    struct Cmd {
        Type type;
        std::string folder;
        int seq = 0;
        std::string dest_folder; // for Move
        uint64_t epoch = 0;      // mailbox generation; stale cmds are dropped
        uint32_t uid = 0;        // for PrefetchUid / MarkSeen uid path
        uint64_t modseq = 0;     // for MarkSeen CONDSTORE UNCHANGEDSINCE
    };

    void post(Type t, const std::string &folder = "", int seq = 0,
              const std::string &dest = "");

    /* True if `folder` is still the mailbox the GUI wants. */
    bool folder_wanted(const std::string &folder);
    std::string wanted_folder_copy();

    /* Drop queued work that would SELECT or FETCH against a mailbox the
     * user is no longer looking at.  Move is kept (it names its source). */
    void drop_stale_mailbox_work_locked();

    /* Marshal a function to the GUI thread and wake the main loop. */
    void deliver(std::function<void()> fn);

    bool quitting();
    void report_error(const std::string &title, const std::string &msg);
    void report_fetch_progress(int done, int total);
    void report_status(const std::string &msg, const std::string &folder = "");

    /* Re-LOGIN without LIST (used after cancel() killed the socket). */
    bool ensure_connected();
    static bool is_stale_err(const std::string &err) {
        return ImapClient::is_cancelled_error(err);
    }

    /* Connect + LOGIN + LIST.  Returns false on failure (error reported). */
    bool do_connect();
    bool do_list_folders();
    void do_select(const std::string &folder);
    /* is_auto: a periodic background check (Type::AutoRefresh) rather than
     * an explicit Refresh/Select. Fetches only the delta beyond the last
     * known message count (cheap, regardless of how far back the user has
     * paged) and delivers via cb_auto_summaries instead of cb_summaries, so
     * the GUI can merge new mail in without disturbing scroll/selection. */
    void do_fetch_summaries(const std::string &folder, int exists,
                            bool is_auto = false);
    /* Fetch the next older page: the 150 messages just below the oldest
       one currently shown. */
    void do_fetch_older(const Cmd &cmd);
    void cancel_prefetch_locked();
    bool do_move(const Cmd &cmd);
    /* Best effort: a failed read-flag is not worth interrupting the user,
     * so this reports nothing on error beyond a status line. */
    void do_mark_seen(const Cmd &cmd);
    bool already_prefetch_queued_locked(int seq, const std::string &folder) const;
    bool already_prefetch_uid_queued_locked(uint32_t uid, const std::string &folder) const;
    void schedule_next_prefetch();
    void schedule_next_prefetch_uid();
    void do_prefetch(const Cmd &cmd);
    void do_prefetch_uid(const Cmd &cmd);
    void run();

    // QRESYNC persistence (RFC 7162).  Per-folder anchor stored at
    // config_dir()/qresync.json keyed by host|username|folder.
    std::string qresync_key(const std::string &folder) const;
    std::string qresync_file_path() const;
    bool load_qresync_state(const std::string &folder,
                            ImapClient::QResyncState &out_state,
                            std::vector<uint32_t> &out_uids);
    bool save_qresync_state(const std::string &folder,
                            const ImapClient::QResyncState &state,
                            const std::vector<uint32_t> &uids);
    static std::string uids_to_seqset_str(const std::vector<uint32_t> &uids);
    static std::vector<uint32_t> seqset_to_uids_vec(const std::string &seqset);

    std::thread             m_thread;
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::deque<Cmd>         m_queue;
    bool                    m_quit = false;
    MailConfig              m_config;
    ImapClient              m_imap;
    std::string             m_selected_folder;
    /* Latest folder the GUI asked to open.  Distinct from m_selected_folder
     * (the mailbox IMAP currently has SELECTED) so in-flight fetches for the
     * previous folder can be ignored after a click. */
    std::string             m_wanted_folder;
    uint64_t                m_epoch = 0;
    int                     m_prog_done = 0;
    std::chrono::steady_clock::time_point m_prog_at{};
    bool                    m_progress_quiet = false;
    bool                    m_busy = false;   // worker is inside a blocking IMAP cmd
    Type                    m_inflight = Type::Connect;
    /* Oldest sequence number currently shown in m_selected_folder
       (0 = nothing loaded).  Drives "load older" paging. */
    int                     m_first_loaded = 0;
    /* Highest sequence number fetched so far in m_selected_folder — lets
       Type::AutoRefresh fetch only the delta since the last check. */
    int                     m_last_known_exists = 0;
    // background prefetch backlog (low priority, viewport-aware)
    std::string             m_prefetch_folder;
    std::deque<int>         m_prefetch_queue;
    std::unordered_set<int> m_prefetch_queued;
    // UID-stable prefetch queue (QRESYNC): UIDs don't shift on EXPUNGE.
    std::deque<uint32_t>         m_prefetch_uid_queue;
    std::unordered_set<uint32_t> m_prefetch_uid_queued;

    /* Pending "mark read" request; m_seen_seq == 0 means none. */
    std::string m_seen_folder;
    int         m_seen_seq = 0;
    uint32_t    m_seen_uid = 0;
    uint64_t    m_seen_modseq = 0;
    std::chrono::steady_clock::time_point m_seen_at;

    // QRESYNC delta + cached summaries.  Capability-gated: only used
    // when ImapClient::has_qresync() is true.  Per-folder cache keeps
    // Inbox summaries while viewing Trash so switching back can apply
    // VANISHED/changed delta without re-fetching 150 msgs.
    std::unordered_map<std::string, std::vector<MailSummary>> m_summaries_cache;
    std::unordered_map<std::string, ImapClient::QResyncState> m_qresync_cache;
    // Pending QRESYNC delta from the most recent select_qresync(),
    // consumed by the immediately following do_fetch_summaries().
    ImapClient::QResyncDelta m_pending_qresync_delta{};
    ImapClient::QResyncState m_pending_qresync_state{};
    std::string m_pending_qresync_folder;
    bool m_qresync_pending = false;
};
