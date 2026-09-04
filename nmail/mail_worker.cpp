/*
 * nmail/mail_worker.cpp — MailWorker implementation (see mail_worker.h).
 */

#include "mail_worker.h"
#include "mail_debug.h"

#include <nanogui/nanogui.h>   // nanogui::async
#include <GLFW/glfw3.h>        // glfwPostEmptyEvent

#include <algorithm>
#include <utility>

void MailWorker::set_config(const MailConfig &c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = c;
}

void MailWorker::start() {
    m_imap.on_progress = [this](int done, int total) {
        report_fetch_progress(done, total);
    };
    m_thread = std::thread([this]() { run(); });
}

void MailWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    m_imap.abort();   // wake a blocking recv
    if (m_thread.joinable())
        m_thread.join();
}

void MailWorker::select_folder(const std::string &name) {
    if (name.empty()) return;
    bool abort_io = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wanted_folder = name;
        ++m_epoch;
        drop_stale_mailbox_work_locked();
        m_queue.push_back({Type::Select, name, 0, "", m_epoch});
        /* Abort only a long in-flight mailbox command (SELECT of the
         * previous folder, a 150-message FETCH, a full-body read).
         * Prefetch is a single BODY.PEEK[] that finishes in tens of
         * ms — killing TLS for it is what left Trash stuck on
         * "Opening" (dead socket + SSL_shutdown hang). The current
         * prefetch returns, then this Select runs on the live
         * connection. */
        abort_io = m_busy &&
                   m_inflight != Type::Move &&
                   m_inflight != Type::Connect &&
                   m_inflight != Type::Prefetch &&
                   m_inflight != Type::MarkSeen;
        mail_dbg("[mail] select_folder '%s' epoch=%llu busy=%d inflight=%d abort=%d q=%zu\n",
                name.c_str(), (unsigned long long)m_epoch, (int)m_busy,
                (int)m_inflight, (int)abort_io, m_queue.size());
    }
    if (abort_io)
        m_imap.cancel();
    m_cv.notify_one();
}

void MailWorker::fetch_body(int seq) {
    std::string folder;
    { std::lock_guard<std::mutex> l(m_mutex); folder = m_wanted_folder.empty()
          ? m_selected_folder : m_wanted_folder; }
    post(Type::FetchBody, folder, seq);
}

void MailWorker::schedule_seen(const std::string &folder, int seq, double delay_sec) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen_folder = folder;
        m_seen_seq    = seq;
        m_seen_at     = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds((long long)(delay_sec * 1000.0));
    }
    m_cv.notify_one();   // the loop may need to wake earlier than planned
}

void MailWorker::cancel_seen() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen_seq = 0;
    }
    m_cv.notify_one();
}

void MailWorker::move_message(const std::string &folder, int seq,
                              const std::string &dest_folder) {
    post(Type::Move, folder, seq, dest_folder);
}

void MailWorker::ensure_visible_cached(const std::string &folder,
                                       const std::vector<int> &visible_seqs) {
    if (visible_seqs.empty() || folder.empty()) return;
    bool need_post = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (folder != m_prefetch_folder) return;
        if (folder != m_selected_folder) return;
        if (folder != m_wanted_folder) return;
        for (auto it = visible_seqs.rbegin(); it != visible_seqs.rend(); ++it) {
            int s = *it;
            bool inflight = false;
            for (auto &c : m_queue)
                if (c.type == Type::Prefetch && c.seq == s && c.folder == folder) { inflight = true; break; }
            if (inflight) continue;
            auto qit = std::find(m_prefetch_queue.begin(), m_prefetch_queue.end(), s);
            if (qit != m_prefetch_queue.end()) {
                m_prefetch_queue.erase(qit);
                m_prefetch_queue.push_front(s);
            } else if (!m_prefetch_queued.count(s)) {
                m_prefetch_queue.push_front(s);
                m_prefetch_queued.insert(s);
            }
        }
        need_post = !m_prefetch_queue.empty();
    }
    if (need_post) schedule_next_prefetch();
}

void MailWorker::post(Type t, const std::string &folder, int seq,
                      const std::string &dest) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back({t, folder, seq, dest, m_epoch});
    }
    m_cv.notify_one();
}

bool MailWorker::folder_wanted(const std::string &folder) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !folder.empty() && folder == m_wanted_folder;
}

std::string MailWorker::wanted_folder_copy() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_wanted_folder.empty() ? m_selected_folder : m_wanted_folder;
}

void MailWorker::drop_stale_mailbox_work_locked() {
    cancel_prefetch_locked();
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
        [this](const Cmd &c) {
            if (c.type == Type::Select || c.type == Type::FetchOlder ||
                c.type == Type::Prefetch || c.type == Type::AutoRefresh)
                return true;
            if ((c.type == Type::FetchBody || c.type == Type::MarkSeen) &&
                c.folder != m_wanted_folder)
                return true;
            return false;
        }), m_queue.end());
}

void MailWorker::deliver(std::function<void()> fn) {
    nanogui::async(std::move(fn));
    glfwPostEmptyEvent();
}

bool MailWorker::quitting() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_quit;
}

void MailWorker::report_error(const std::string &title, const std::string &msg) {
    if (quitting()) return;
    deliver([this, title, msg]() {
        if (cb_error) cb_error(title, msg);
    });
}

void MailWorker::report_fetch_progress(int done, int total) {
    if (total <= 0 || m_progress_quiet) return;
    std::string folder;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        folder = m_wanted_folder;
    }
    if (folder.empty() || !folder_wanted(folder)) return;
    auto now = std::chrono::steady_clock::now();
    if (done > 0 && done < total) {
        int step = std::max(1, total / 24);
        if (done - m_prog_done < step &&
            now - m_prog_at < std::chrono::milliseconds(40))
            return;
    }
    m_prog_done = done;
    m_prog_at = now;
    deliver([this, folder, done, total]() {
        if (cb_progress) cb_progress(folder, done, total);
    });
}

void MailWorker::report_status(const std::string &msg, const std::string &folder) {
    if (quitting()) return;
    /* Drop status for a mailbox the user has already left — otherwise
     * "Opening Trash..." lands after they clicked Inbox. */
    if (!folder.empty() && !folder_wanted(folder))
        return;
    deliver([this, msg, folder]() {
        if (cb_status) cb_status(msg, folder);
    });
}

bool MailWorker::ensure_connected() {
    if (m_imap.is_open())
        return true;
    std::string err;
    if (m_imap.reconnect(err, /*reselect=*/false))
        return true;
    return do_connect();
}

bool MailWorker::do_connect() {
    MailConfig cfg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cfg = m_config;
    }
    report_status("Connecting to " + cfg.host + ":" +
                  std::to_string(cfg.port) + "...");
    std::string err;
    if (!m_imap.open(cfg.host, cfg.port, cfg.username, cfg.password,
                     err)) {
        report_error("Connection failed", err);
        report_status("Not connected");
        return false;
    }
    m_selected_folder.clear();
    m_first_loaded = 0;
    m_last_known_exists = 0;
    return do_list_folders();
}

bool MailWorker::do_list_folders() {
    std::vector<MailFolder> folders;
    std::string err;
    if (!m_imap.list_folders(folders, err)) {
        report_error("Could not list folders", err);
        return false;
    }
    report_status("Connected");
    deliver([this, folders]() {
        if (cb_folders) cb_folders(folders);
    });
    return true;
}

void MailWorker::do_select(const std::string &folder) {
    mail_dbg("[mail] do_select begin '%s' imap_open=%d imap_sel='%s'\n",
            folder.c_str(), (int)m_imap.is_open(),
            m_imap.selected_folder().c_str());
    if (!folder_wanted(folder)) {
        mail_dbg("[mail] do_select skip, no longer wanted (now '%s')\n",
                wanted_folder_copy().c_str());
        return;   // a newer Select already replaced this one
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cancel_prefetch_locked();
    }
    if (!m_imap.is_open() && !ensure_connected()) {
        if (folder_wanted(folder))
            report_status("Not connected");
        return;
    }
    if (!folder_wanted(folder))
        return;
    report_status("Opening " + folder + "...", folder);
    std::string err;
    int exists = 0;
    if (!m_imap.select_folder(folder, exists, err)) {
        mail_dbg("[mail] do_select SELECT '%s' failed: %s (wanted=%d)\n",
                folder.c_str(), err.c_str(), (int)folder_wanted(folder));
        if (!folder_wanted(folder))
            return;
        /* Cancel of the *previous* command (or a half-dead socket) must
         * not strand this Open. Reconnect and retry once. */
        if (is_stale_err(err) || ImapClient::is_connection_error(err)) {
            m_imap.close();
            err.clear();
            if (!(ensure_connected() && folder_wanted(folder) &&
                  m_imap.select_folder(folder, exists, err))) {
                mail_dbg("[mail] do_select SELECT '%s' retry failed: %s\n",
                        folder.c_str(), err.c_str());
                if (!folder_wanted(folder) || is_stale_err(err))
                    return;
                report_error("Could not open folder", err);
                report_status("Not connected");
                return;
            }
            mail_dbg("[mail] do_select SELECT '%s' retry OK exists=%d\n",
                    folder.c_str(), exists);
        } else {
            report_error("Could not open folder", err);
            report_status("Ready");
            return;
        }
    }
    if (!folder_wanted(folder))
        return;
    /* SELECT sometimes omits EXISTS (or a desynced parser misses it).
     * STATUS is cheap and keeps us from treating a full mailbox as empty. */
    if (exists <= 0) {
        int unseen = 0;
        std::string se;
        int status_n = 0;
        if (m_imap.status_counts(folder, status_n, unseen, se) && status_n > 0)
            exists = status_n;
        if (!folder_wanted(folder) || is_stale_err(se))
            return;
    }
    m_selected_folder   = folder;
    m_first_loaded      = 0;
    m_last_known_exists = 0;
    if (exists <= 0)
        report_status("Fetching " + folder + " (empty?)...", folder);
    else
        report_status("Fetching " + folder + " (" +
                      std::to_string(exists) + " on server)...", folder);
    do_fetch_summaries(folder, exists);
}

void MailWorker::do_fetch_summaries(const std::string &folder, int exists,
                                    bool is_auto) {
    std::vector<MailSummary> summaries;
    std::string err;
    int first;
    if (is_auto) {
        first = m_last_known_exists + 1;
        if (first > exists) {
            m_last_known_exists = exists;
            return;   // nothing new since the last check
        }
    } else {
        /* First load shows the newest 150 messages; refreshes re-fetch
           the whole window the user has paged back through. */
        first = m_first_loaded > 0 ? m_first_loaded
                                   : std::max(1, exists - 149);
    }
    mail_dbg("[mail] do_fetch_summaries '%s' auto=%d exists=%d first=%d wanted='%s'\n",
            folder.c_str(), (int)is_auto, exists, first,
            wanted_folder_copy().c_str());
    m_progress_quiet = is_auto;
    if (!m_imap.fetch_summaries(first, exists, summaries, err)) {
        m_progress_quiet = false;
        mail_dbg("[mail] FETCH summaries '%s' failed: %s\n",
                folder.c_str(), err.c_str());
        if (!folder_wanted(folder) || is_stale_err(err))
            return;   // cancelled mid-FETCH by a newer folder click
        report_error(is_auto ? "Could not check for new mail"
                             : "Could not fetch messages", err);
        return;
    }
    m_progress_quiet = false;
    if (!folder_wanted(folder)) {
        mail_dbg("[mail] FETCH summaries '%s' dropped, wanted='%s'\n",
                folder.c_str(), wanted_folder_copy().c_str());
        return;   // user clicked away while headers were in flight
    }
    mail_dbg("[mail] FETCH summaries '%s' OK %zu rows, delivering\n",
            folder.c_str(), summaries.size());
    if (!is_auto) m_first_loaded = first;
    m_last_known_exists = std::max(m_last_known_exists, exists);
    /* Newest first. */
    std::reverse(summaries.begin(), summaries.end());
    if (!is_auto)
        report_status(folder + ": " + std::to_string(exists) +
                      (exists == 1 ? " message" : " messages") +
                      (summaries.size() != (size_t)exists
                           ? " (showing " + std::to_string(summaries.size()) + ")"
                           : ""),
                      folder);
    deliver([this, folder, summaries, is_auto]() {
        if (is_auto) { if (cb_auto_summaries) cb_auto_summaries(folder, summaries); }
        else         { if (cb_summaries)      cb_summaries(folder, summaries); }
    });
    // Seed a low-priority backlog with the first 25 (newest first).
    // Further visible rows get enqueued on demand via ensure_visible_cached().
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_prefetch_folder = folder;
        if (!is_auto) {
            m_prefetch_queue.clear();
            m_prefetch_queued.clear();
        }
        for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
            if (m_prefetch_queued.count(summaries[i].seq)) continue;
            m_prefetch_queue.push_back(summaries[i].seq);
            m_prefetch_queued.insert(summaries[i].seq);
        }
    }
    schedule_next_prefetch();
}

void MailWorker::do_fetch_older(const Cmd &cmd) {
    std::string folder = cmd.folder.empty() ? m_selected_folder : cmd.folder;
    if (folder.empty() || !folder_wanted(folder))
        return;
    if (m_selected_folder != folder || m_first_loaded <= 1)
        return;   // Select hasn't landed, or nothing older
    if (m_imap.selected_folder() != folder) {
        std::string se;
        if (!m_imap.ensure_selected(folder, se) || !folder_wanted(folder))
            return;
    }
    int last  = m_first_loaded - 1;
    int first = std::max(1, last - 149);

    report_status("Loading older messages in " + folder + "...", folder);
    std::vector<MailSummary> summaries;
    std::string err;
    if (!m_imap.fetch_summaries(first, last, summaries, err)) {
        if (!folder_wanted(folder) || is_stale_err(err))
            return;
        report_error("Could not fetch older messages", err);
        return;
    }
    if (!folder_wanted(folder))
        return;
    m_first_loaded = first;
    /* Newest first, so the GUI can append them after the current
       (newer) page. */
    std::reverse(summaries.begin(), summaries.end());
    deliver([this, folder, summaries]() {
        if (cb_older) cb_older(folder, summaries);
    });
}

void MailWorker::cancel_prefetch_locked() {
    m_prefetch_folder.clear();
    m_prefetch_queue.clear();
    m_prefetch_queued.clear();
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
        [](const Cmd &c){ return c.type == Type::Prefetch; }), m_queue.end());
}

bool MailWorker::do_move(const Cmd &cmd) {
    /* move_message() may EXPUNGE, which renumbers the mailbox — a queued
     * "mark read" seq would then point at an unrelated message. */
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen_seq = 0;
    }
    std::string folder = cmd.folder;
    int seq = cmd.seq;
    std::string dest = cmd.dest_folder;
    if (folder.empty() || dest.empty() || seq <= 0)
        return false;
    if (!m_imap.is_open()) {
        report_error("Not connected",
                     "Set up the server in Preferences first.");
        return false;
    }
    // If user switched folders while this was queued, still operate on
    // the original source folder.
    if (!folder.empty() && m_imap.selected_folder() != folder) {
        std::string se;
        if (!m_imap.ensure_selected(folder, se)) {
            if (!ImapClient::is_connection_error(se)) {
                report_error("Could not open folder", se);
                return false;
            }
            std::string re;
            if (!m_imap.reconnect(re) || !m_imap.ensure_selected(folder, se)) {
                report_error("Connection lost", re.empty() ? se : re);
                return false;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_selected_folder = folder;
        // Remove the moving message from any pending prefetch work.
        m_prefetch_queue.erase(
            std::remove(m_prefetch_queue.begin(), m_prefetch_queue.end(), seq),
            m_prefetch_queue.end());
        m_prefetch_queued.erase(seq);
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
            [&](const Cmd &c){ return c.type == Type::Prefetch && c.seq == seq && c.folder == folder; }),
            m_queue.end());
    }
    report_status("Moving message to " + dest + "...");
    std::string err;
    bool ok = m_imap.move_message(seq, dest, err);
    if (!ok && ImapClient::is_connection_error(err)) {
        std::string re;
        if (m_imap.reconnect(re)) {
            std::string se;
            m_imap.ensure_selected(folder, se);
            err.clear();
            ok = m_imap.move_message(seq, dest, err);
        }
    }
    if (!ok) {
        report_error("Could not move message", err);
        report_status("Ready");
        return false;
    }
    // Invalidate body/prefetch caches for this folder — sequence numbers
    // shift after EXPUNGE, so any stale seq->body mapping is wrong.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cancel_prefetch_locked();
    }
    report_status("Moved to " + dest);
    deliver([this, folder, seq, dest]() {
        if (cb_moved) cb_moved(folder, seq, dest);
    });
    // Keep scroll/position: do NOT re-SELECT the folder. The GUI
    // removes the row locally and adjusts remaining seq numbers
    // (EXPUNGE resequences). Next explicit Refresh will fully
    // reconcile with the server.
    return true;
}

void MailWorker::do_mark_seen(const Cmd &cmd) {
    if (cmd.seq <= 0 || cmd.folder.empty() || !m_imap.is_open())
        return;
    if (!folder_wanted(cmd.folder))
        return;   // don't SELECT a mailbox the user just left
    std::string err;
    if (m_imap.selected_folder() != cmd.folder &&
        !m_imap.ensure_selected(cmd.folder, err))
        return;
    if (!m_imap.mark_seen(cmd.seq, err)) {
        if (!ImapClient::is_connection_error(err))
            return;
        std::string re;
        if (!m_imap.reconnect(re) ||
            !m_imap.ensure_selected(cmd.folder, err) ||
            !m_imap.mark_seen(cmd.seq, err))
            return;
    }
    std::string folder = cmd.folder;
    int seq = cmd.seq;
    deliver([this, folder, seq]() { if (cb_seen) cb_seen(folder, seq); });
}

bool MailWorker::already_prefetch_queued_locked(int seq, const std::string &folder) const {
    if (m_prefetch_queued.count(seq)) return true;
    for (auto &c : m_queue)
        if (c.type == Type::Prefetch && c.seq == seq && c.folder == folder) return true;
    return false;
}

void MailWorker::schedule_next_prefetch() {
    std::string folder; int seq = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_prefetch_queue.empty()) return;
        if (!m_selected_folder.empty() && m_selected_folder != m_prefetch_folder)
            return;
        folder = m_prefetch_folder;
        seq = m_prefetch_queue.front();
        for (auto &c : m_queue)
            if (c.type == Type::Prefetch && c.seq == seq && c.folder == folder) return;
    }
    post(Type::Prefetch, folder, seq);
}

void MailWorker::do_prefetch(const Cmd &cmd) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool has_priority = false;
        for (auto &c : m_queue) if (c.type != Type::Prefetch) { has_priority = true; break; }
        if (has_priority) {
            m_queue.push_back(cmd);
            return;
        }
        if (cmd.folder != m_prefetch_folder) return;
        if (cmd.folder != m_selected_folder) return;
        if (cmd.folder != m_wanted_folder) return;
    }
    if (!m_imap.is_open()) {
        std::string re; m_imap.reconnect(re);
        if (!m_imap.is_open()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_prefetch_queue.empty() && m_prefetch_queue.front() == cmd.seq) {
                m_prefetch_queue.pop_front(); m_prefetch_queued.erase(cmd.seq);
            }
            return;
        }
    }
    if (!cmd.folder.empty() && m_imap.selected_folder() != cmd.folder) {
        std::string se; m_imap.ensure_selected(cmd.folder, se);
    }
    MailMessage msg; std::string err;
    bool ok = m_imap.fetch_message(cmd.seq, msg, err,
        [this, folder = cmd.folder]() { return folder_wanted(folder); });
    mail_dbg("[mail] prefetch seq=%d folder='%s' done ok=%d wanted='%s'\n",
            cmd.seq, cmd.folder.c_str(), (int)ok, wanted_folder_copy().c_str());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_prefetch_queue.empty() && m_prefetch_queue.front() == cmd.seq) {
            m_prefetch_queue.pop_front(); m_prefetch_queued.erase(cmd.seq);
        } else {
            auto qit = std::find(m_prefetch_queue.begin(), m_prefetch_queue.end(), cmd.seq);
            if (qit != m_prefetch_queue.end()) { m_prefetch_queue.erase(qit); m_prefetch_queued.erase(cmd.seq); }
        }
    }
    if (!ok) {
        mail_dbg("[mail] prefetch seq=%d folder='%s' failed: %s\n",
                cmd.seq, cmd.folder.c_str(), err.c_str());
        if (!is_stale_err(err) && folder_wanted(cmd.folder))
            schedule_next_prefetch();
        return;
    }
    std::string preview = message_preview(msg);
    deliver([this, folder = cmd.folder, seq = cmd.seq, msg, preview]() {
        if (cb_prefetched) cb_prefetched(folder, seq, msg, preview);
    });
    if (folder_wanted(cmd.folder))
        schedule_next_prefetch();
}

void MailWorker::run() {
    /* Only advances when a wait actually times out (a real interval
       elapsed), not on every unrelated command — so an active user
       doesn't perpetually postpone the background mail check. */
    auto next_check = std::chrono::steady_clock::now();
    for (;;) {
        Cmd cmd;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            /* Sleep until there is work: a queued command, shutdown, or
             * a deadline (the periodic mail check or a pending "mark
             * read" timer).  The deadline is recomputed after EVERY wake
             * because schedule_seen() notifies precisely so a newly armed
             * timer can shorten a wait already in progress — folding the
             * deadline into a pred-based wait_until would instead keep
             * sleeping until the ORIGINAL deadline (up to
             * check_interval_min away), leaving \Seen unset for minutes
             * on an otherwise idle connection. */
            for (;;) {
                /* Must test the queue BEFORE waiting. Clicking Trash
                 * while a prefetch is in IMAP notifies the CV, but we
                 * are not waiting then — the notify is lost. After
                 * prefetch returns, Select is already queued; waiting
                 * first slept until check_interval (minutes) and the
                 * UI stayed on "Opening Trash...". */
                if (m_quit || !m_queue.empty())
                    break;
                auto deadline = next_check;
                if (m_seen_seq > 0 && m_seen_at < deadline)
                    deadline = m_seen_at;
                m_cv.wait_until(lock, deadline);
                if (m_quit || !m_queue.empty() ||
                    std::chrono::steady_clock::now() >= deadline)
                    break;
                /* Woke early on a notify (e.g. schedule_seen) with an
                 * empty queue: loop to recompute the deadline. */
            }
            if (m_quit) return;
            bool got_cmd = !m_queue.empty();
            if (!got_cmd) {
                auto now = std::chrono::steady_clock::now();
                if (m_seen_seq > 0 && now >= m_seen_at) {
                    cmd.type   = Type::MarkSeen;
                    cmd.folder = m_seen_folder;
                    cmd.seq    = m_seen_seq;
                    m_seen_seq = 0;             // consume the request
                } else if (now >= next_check) {
                    int interval_min = std::max(1, m_config.check_interval_min);
                    next_check = now + std::chrono::minutes(interval_min);
                    if (m_config.host.empty() ||
                        (m_wanted_folder.empty() && m_selected_folder.empty()))
                        continue;   // nothing configured/selected to check yet
                    cmd.type = Type::AutoRefresh;
                    cmd.folder = m_wanted_folder.empty() ? m_selected_folder
                                                         : m_wanted_folder;
                } else {
                    continue;       // woke early; nothing due yet
                }
            } else {
                cmd = m_queue.front();
                m_queue.pop_front();
            }
            m_inflight = cmd.type;
            m_busy = true;
        }

        switch (cmd.type) {
        case Type::Connect:
            do_connect();
            break;
        case Type::AutoRefresh: {
            if (!m_imap.is_open()) {
                if (!do_connect()) break;
            }
            {
                std::string want = wanted_folder_copy();
                if (want.empty()) break;
                /* A folder switch is in flight — don't SELECT the old box. */
                if (!folder_wanted(want)) break;
                std::string err;
                int exists = 0;
                if (m_imap.select_folder(want, exists, err) &&
                    folder_wanted(want))
                    do_fetch_summaries(want, exists, /*is_auto=*/true);
            }
            break;
        }
        case Type::Refresh: {
            if (!m_imap.is_open()) {
                if (!do_connect()) break;
            } else if (!do_list_folders()) {
                break;
            }
            std::string want = wanted_folder_copy();
            if (!want.empty() && folder_wanted(want)) {
                std::string err;
                int exists = 0;
                if (m_imap.select_folder(want, exists, err) &&
                    folder_wanted(want)) {
                    if (exists <= 0) {
                        int unseen = 0, status_n = 0;
                        std::string se;
                        if (m_imap.status_counts(want, status_n, unseen, se) &&
                            status_n > 0)
                            exists = status_n;
                    }
                    m_selected_folder   = want;
                    m_first_loaded      = 0;
                    m_last_known_exists = 0;
                    report_status("Fetching " + want + " (" +
                                  std::to_string(exists) + " on server)...",
                                  want);
                    do_fetch_summaries(want, exists);
                }
            }
            break;
        }
        case Type::Select:
            mail_dbg("[mail] queue: running Select '%s'\n",
                    cmd.folder.c_str());
            if (!m_imap.is_open()) {
                if (!ensure_connected()) break;
            }
            do_select(cmd.folder);
            break;
        case Type::FetchBody: {
            if (!m_imap.is_open()) {
                report_error("Not connected",
                             "Set up the server in Preferences first.");
                break;
            }
            // Preserve the folder the FETCH belongs to across a silent
            // reconnect (selected_folder may reset briefly).
            std::string want_folder = cmd.folder.empty() ? m_selected_folder : cmd.folder;
            if (!want_folder.empty() && !folder_wanted(want_folder))
                break;   // user switched folders; don't SELECT the old one back
            if (!want_folder.empty() && m_imap.selected_folder() != want_folder) {
                std::string se;
                if (!m_imap.ensure_selected(want_folder, se)) {
                    if (is_stale_err(se) || !folder_wanted(want_folder))
                        break;
                    if (!ImapClient::is_connection_error(se)) {
                        report_error("Could not open folder", se);
                        break;
                    }
                    std::string re;
                    if (!m_imap.reconnect(re, /*reselect=*/false)) {
                        if (is_stale_err(re) || !folder_wanted(want_folder))
                            break;
                        report_error("Connection lost", re);
                        break;
                    }
                    if (!m_imap.ensure_selected(want_folder, se)) {
                        if (is_stale_err(se) || !folder_wanted(want_folder))
                            break;
                        report_error("Could not open folder", se);
                        break;
                    }
                }
            }
            report_status("Fetching message...", want_folder);
            MailMessage msg;
            std::string err;
            if (!m_imap.fetch_message(cmd.seq, msg, err)) {
                if (is_stale_err(err) || !folder_wanted(want_folder))
                    break;
                if (ImapClient::is_connection_error(err)) {
                    std::string re;
                    if (m_imap.reconnect(re, /*reselect=*/false)) {
                        if (!want_folder.empty())
                            m_imap.ensure_selected(want_folder, re);
                        err.clear();
                        if (m_imap.fetch_message(cmd.seq, msg, err)) {
                            report_status("Ready (reconnected)", want_folder);
                            deliver([this, folder = want_folder, seq = cmd.seq, msg]() {
                                if (cb_body) cb_body(folder, seq, msg);
                            });
                            break;
                        }
                    }
                }
                if (is_stale_err(err) || !folder_wanted(want_folder))
                    break;
                report_error("Could not fetch message", err);
                report_status("Ready");
                break;
            }
            report_status("Ready", want_folder);
            deliver([this, folder = want_folder, seq = cmd.seq, msg]() {
                if (cb_body) cb_body(folder, seq, msg);
            });
            break;
        }
        case Type::FetchOlder:
            if (!m_imap.is_open()) {
                report_error("Not connected",
                             "Set up the server in Preferences first.");
                break;
            }
            do_fetch_older(cmd);
            break;
        case Type::Prefetch:
            do_prefetch(cmd);
            break;
        case Type::Move:
            do_move(cmd);
            break;
        case Type::MarkSeen:
            do_mark_seen(cmd);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_busy = false;
        }
    }
}
