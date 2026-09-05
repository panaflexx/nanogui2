/*
 * nmail/mail_worker.cpp — MailWorker implementation (see mail_worker.h).
 */

#include "mail_worker.h"
#include "mail_debug.h"
#include "dict.h"

#include <nanogui/nanogui.h>   // nanogui::async
#include <GLFW/glfw3.h>        // glfwPostEmptyEvent

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <utility>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <cstring>
#include <cstdlib>

void MailWorker::set_config(const MailConfig &c) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = c;
}

bool MailWorker::is_compressed() const { return m_imap.is_compressed(); }

// ── QRESYNC persistence helpers ──────────────────────────────────────────
std::string MailWorker::qresync_file_path() const {
    return config_file("qresync.json");
}
std::string MailWorker::qresync_key(const std::string &folder) const {
    MailConfig cfg;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cfg = m_config;
    }
    return cfg.host + "|" + cfg.username + "|" + folder;
}
std::string MailWorker::uids_to_seqset_str(const std::vector<uint32_t> &uids) {
    if (uids.empty()) return "";
    std::vector<uint32_t> s = uids;
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i + 1;
        while (j < s.size() && s[j] == s[j-1] + 1) ++j;
        if (!out.empty()) out += ",";
        if (j - i == 1) out += std::to_string(s[i]);
        else out += std::to_string(s[i]) + ":" + std::to_string(s[j-1]);
        i = j;
    }
    return out;
}
std::vector<uint32_t> MailWorker::seqset_to_uids_vec(const std::string &seqset) {
    std::vector<uint32_t> out;
    size_t p = 0;
    while (p < seqset.size()) {
        while (p < seqset.size() && (seqset[p] == ',' || isspace((unsigned char)seqset[p]))) ++p;
        if (p >= seqset.size()) break;
        size_t q = p;
        while (q < seqset.size() && isdigit((unsigned char)seqset[q])) ++q;
        if (q == p) { ++p; continue; }
        uint32_t a = (uint32_t)std::stoul(seqset.substr(p, q - p));
        p = q;
        if (p < seqset.size() && seqset[p] == ':') {
            ++p;
            size_t r = p;
            while (r < seqset.size() && isdigit((unsigned char)seqset[r])) ++r;
            if (r > p) {
                uint32_t b = (uint32_t)std::stoul(seqset.substr(p, r - p));
                for (uint32_t v = a; v <= b; ++v) out.push_back(v);
                p = r;
            } else out.push_back(a);
        } else out.push_back(a);
    }
    return out;
}
bool MailWorker::load_qresync_state(const std::string &folder,
                                    ImapClient::QResyncState &out_state,
                                    std::vector<uint32_t> &out_uids) {
    out_state = {};
    out_uids.clear();
    std::string path = qresync_file_path();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) return false;
    char err[256] = {0};
    DictValue *root = dict_deserialize_json_len(text.c_str(), text.size(), err, sizeof(err));
    if (!root) return false;
    std::string key = qresync_key(folder);
    DictValue *node = dict_object_get(root, key.c_str());
    if (!node) {
        DictValue *entries = dict_object_get(root, "entries");
        if (entries && entries->type == DICT_ARRAY) {
            for (size_t i = 0; i < entries->array_value.length; ++i) {
                DictValue *e = entries->array_value.items[i];
                if (!e || e->type != DICT_OBJECT) continue;
                DictValue *k = dict_object_get(e, "key");
                if (!k || k->type != DICT_STRING || !k->string_value) continue;
                if (key == k->string_value) { node = e; break; }
            }
        }
    }
    if (!node) { dict_destroy(root); return false; }
    DictValue *v = dict_object_get(node, "uidvalidity");
    if (v) {
        if (v->type == DICT_INT64) out_state.uidvalidity = (uint32_t)v->int64_value;
        else if (v->type == DICT_NUMBER) out_state.uidvalidity = (uint32_t)v->number_value;
    }
    v = dict_object_get(node, "highestmodseq");
    if (v) {
        if (v->type == DICT_INT64) out_state.highestmodseq = (uint64_t)v->int64_value;
        else if (v->type == DICT_NUMBER) out_state.highestmodseq = (uint64_t)v->number_value;
    }
    v = dict_object_get(node, "uidnext");
    if (v) {
        if (v->type == DICT_INT64) out_state.uidnext = (uint32_t)v->int64_value;
        else if (v->type == DICT_NUMBER) out_state.uidnext = (uint32_t)v->number_value;
    }
    v = dict_object_get(node, "messages");
    if (v) {
        if (v->type == DICT_INT64) out_state.messages = (uint32_t)v->int64_value;
        else if (v->type == DICT_NUMBER) out_state.messages = (uint32_t)v->number_value;
    }
    v = dict_object_get(node, "known_uids");
    if (v && v->type == DICT_STRING && v->string_value) {
        out_uids = seqset_to_uids_vec(v->string_value);
    }
    dict_destroy(root);
    if (out_state.uidvalidity == 0) return false;
    return true;
}
bool MailWorker::save_qresync_state(const std::string &folder,
                                    const ImapClient::QResyncState &state,
                                    const std::vector<uint32_t> &uids) {
    std::string path = qresync_file_path();
    std::string key = qresync_key(folder);
    DictValue *root = nullptr;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!text.empty()) {
                char err[256] = {0};
                root = dict_deserialize_json_len(text.c_str(), text.size(), err, sizeof(err));
            }
        }
    }
    if (!root) root = dict_create_object();
    if (!root || root->type != DICT_OBJECT) {
        if (root) dict_destroy(root);
        root = dict_create_object();
    }
    DictValue *node = dict_create_object();
    dict_object_set(node, "uidvalidity", dict_create_int64((int64_t)state.uidvalidity));
    dict_object_set(node, "highestmodseq", dict_create_int64((int64_t)state.highestmodseq));
    dict_object_set(node, "uidnext", dict_create_int64((int64_t)state.uidnext));
    dict_object_set(node, "messages", dict_create_int64((int64_t)state.messages));
    std::string seqset = uids_to_seqset_str(uids);
    dict_object_set(node, "known_uids", dict_create_string(seqset.c_str()));
    dict_object_set(root, key.c_str(), node);
    char *json = dict_serialize_json_heap(root, 1);
    bool ok = false;
    if (json) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out << json;
            ok = (bool)out;
        }
#ifndef _WIN32
        if (ok) ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
        free(json);
    }
    dict_destroy(root);
    return ok;
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
    schedule_seen(folder, seq, 0, delay_sec);
}

void MailWorker::schedule_seen(const std::string &folder, int seq, uint64_t modseq, double delay_sec) {
    schedule_seen_uid(folder, seq, 0, modseq, delay_sec);
}

void MailWorker::schedule_seen_uid(const std::string &folder, int seq, uint32_t uid, uint64_t modseq, double delay_sec) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen_folder = folder;
        m_seen_seq    = seq;
        m_seen_uid    = uid;
        m_seen_modseq = modseq;
        m_seen_at     = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds((long long)(delay_sec * 1000.0));
    }
    m_cv.notify_one();   // the loop may need to wake earlier than planned
}

void MailWorker::cancel_seen() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen_seq = 0;
        m_seen_uid = 0;
        m_seen_modseq = 0;
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

void MailWorker::ensure_visible_cached_uid(const std::string &folder,
                                           const std::vector<uint32_t> &uids) {
    if (uids.empty() || folder.empty()) return;
    bool need_post = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (folder != m_prefetch_folder) return;
        if (folder != m_selected_folder) return;
        if (folder != m_wanted_folder) return;
        for (auto it = uids.rbegin(); it != uids.rend(); ++it) {
            uint32_t u = *it;
            if (u == 0) continue;
            bool inflight = false;
            for (auto &c : m_queue)
                if (c.type == Type::PrefetchUid && c.uid == u && c.folder == folder) { inflight = true; break; }
            if (inflight) continue;
            auto qit = std::find(m_prefetch_uid_queue.begin(), m_prefetch_uid_queue.end(), u);
            if (qit != m_prefetch_uid_queue.end()) {
                m_prefetch_uid_queue.erase(qit);
                m_prefetch_uid_queue.push_front(u);
            } else if (!m_prefetch_uid_queued.count(u)) {
                m_prefetch_uid_queue.push_front(u);
                m_prefetch_uid_queued.insert(u);
            }
        }
        need_post = !m_prefetch_uid_queue.empty();
    }
    if (need_post) schedule_next_prefetch_uid();
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
                c.type == Type::Prefetch || c.type == Type::PrefetchUid ||
                c.type == Type::AutoRefresh)
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
    mail_dbg("[mail] do_select begin '%s' imap_open=%d imap_sel='%s' qresync=%d\n",
            folder.c_str(), (int)m_imap.is_open(),
            m_imap.selected_folder().c_str(), (int)m_imap.has_qresync());
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
    bool used_qresync = false;
    ImapClient::QResyncState knownState{}, outState{};
    std::vector<uint32_t> knownUids;
    ImapClient::QResyncDelta delta{};
    bool have_known = false;
    if (m_imap.has_qresync()) {
        have_known = load_qresync_state(folder, knownState, knownUids);
        mail_dbg("[mail] do_select qresync load folder='%s' have=%d uidvalidity=%u modseq=%llu uids=%zu\n",
                folder.c_str(), (int)have_known, knownState.uidvalidity,
                (unsigned long long)knownState.highestmodseq, knownUids.size());
        if (have_known && knownState.uidvalidity != 0 && !knownUids.empty()) {
            std::string qerr;
            // select_qresync is capability-gated and falls back to plain SELECT
            // internally when caps are missing or the server rejects QRESYNC.
            if (m_imap.select_qresync(folder, knownState, knownUids, outState, delta, qerr)) {
                if (!folder_wanted(folder))
                    return;
                // UIDVALIDITY change means mailbox was recreated – cache invalid.
                if (outState.uidvalidity != 0 && outState.uidvalidity != knownState.uidvalidity) {
                    mail_dbg("[mail] do_select QRESYNC uidvalidity changed %u -> %u, invalidating cache\n",
                            knownState.uidvalidity, outState.uidvalidity);
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_summaries_cache.erase(folder);
                        m_qresync_cache.erase(folder);
                        m_qresync_pending = false;
                        m_pending_qresync_delta = {};
                        m_pending_qresync_folder.clear();
                    }
                    exists = (int)outState.messages;
                    used_qresync = true; // still via QRESYNC SELECT, but cache cleared
                    // persist the reset state with empty uid list for next time
                    save_qresync_state(folder, outState, {});
                } else {
                    exists = (int)outState.messages;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_pending_qresync_delta = delta;
                        m_pending_qresync_state = outState;
                        m_pending_qresync_folder = folder;
                        m_qresync_pending = true;
                    }
                    used_qresync = true;
                    mail_dbg("[mail] do_select QRESYNC OK exists=%d vanished=%zu changed=%zu modseq=%llu\n",
                            exists, delta.vanished.size(), delta.changed_flags.size(),
                            (unsigned long long)outState.highestmodseq);
                    // Save intermediate anchor minus vanished (full uid list saved after fetch)
                    if (outState.uidvalidity != 0) {
                        std::vector<uint32_t> filtered_known;
                        filtered_known.reserve(knownUids.size());
                        std::unordered_set<uint32_t> vanished_set(delta.vanished.begin(), delta.vanished.end());
                        for (auto uid : knownUids) if (!vanished_set.count(uid)) filtered_known.push_back(uid);
                        save_qresync_state(folder, outState, filtered_known);
                    }
                }
                // QRESYNC SELECT already did the SELECT; still handle STATUS fallback if needed
                if (exists <= 0) {
                    int unseen = 0;
                    std::string se;
                    int status_n = 0;
                    if (m_imap.status_counts(folder, status_n, unseen, se) && status_n > 0)
                        exists = status_n;
                    if (!folder_wanted(folder) || is_stale_err(se))
                        return;
                    // update outState.messages if STATUS gave better count
                    outState.messages = (uint32_t)exists;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_pending_qresync_state.messages = (uint32_t)exists;
                    }
                    if (outState.uidvalidity != 0) save_qresync_state(folder, outState, knownUids);
                }
            } else {
                mail_dbg("[mail] do_select QRESYNC failed: %s (wanted=%d)\n",
                        qerr.c_str(), (int)folder_wanted(folder));
                if (!folder_wanted(folder))
                    return;
                if (is_stale_err(qerr) || ImapClient::is_connection_error(qerr)) {
                    m_imap.close();
                    qerr.clear();
                    if (!ensure_connected() || !folder_wanted(folder)) {
                        if (folder_wanted(folder)) report_status("Not connected");
                        return;
                    }
                    // After reconnect, fall through to plain SELECT path
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_qresync_pending = false;
                } else {
                    report_error("Could not open folder", qerr);
                    report_status("Ready");
                    return;
                }
                // not used_qresync, will try plain below
            }
        }
    }
    if (!used_qresync) {
        // Plain SELECT path (fallback or no QRESYNC available)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_qresync_pending = false;
        }
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
    int first = 0;
    bool is_qresync_resume = false;
    std::vector<MailSummary> base_summaries;
    ImapClient::QResyncDelta pending_delta;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!is_auto && m_qresync_pending && m_pending_qresync_folder == folder) {
            auto it = m_summaries_cache.find(folder);
            if (it != m_summaries_cache.end() && !it->second.empty()) {
                base_summaries = it->second;
                pending_delta = m_pending_qresync_delta;
                is_qresync_resume = true;
                mail_dbg("[mail] do_fetch_summaries QRESYNC resume folder='%s' cached=%zu vanished=%zu changed=%zu\n",
                        folder.c_str(), base_summaries.size(), pending_delta.vanished.size(), pending_delta.changed_flags.size());
            }
        }
    }
    if (!is_qresync_resume) {
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
    }
    if (is_qresync_resume) {
        // Apply delta to local summaries: remove vanished UIDs and patch flags.
        std::unordered_set<uint32_t> vanished_set(pending_delta.vanished.begin(), pending_delta.vanished.end());
        std::vector<MailSummary> kept;
        kept.reserve(base_summaries.size());
        for (auto &s : base_summaries) {
            if (s.uid && vanished_set.count(s.uid)) continue;
            auto it = pending_delta.changed_flags.find(s.uid);
            if (it != pending_delta.changed_flags.end()) {
                if (!it->second.empty()) {
                    bool has_seen = false;
                    for (auto &tok : it->second) {
                        std::string low = tok;
                        for (auto &c : low) c = (char)tolower((unsigned char)c);
                        if (low == "\\seen") { has_seen = true; break; }
                    }
                    s.seen = has_seen;
                }
                auto msit = pending_delta.modseqs.find(s.uid);
                if (msit != pending_delta.modseqs.end()) s.modseq = msit->second;
            }
            kept.push_back(s);
        }
        base_summaries.swap(kept);
        int have = (int)base_summaries.size();
        int to_fetch = 0;
        int fetch_first = 0, fetch_last = exists;
        bool needs_full_refetch = false;
        if (exists > 0 && have < exists) {
            int deficit = exists - have;
            if (deficit >= 150) {
                needs_full_refetch = true;
            } else {
                fetch_first = std::max(1, exists - deficit + 1);
                fetch_last = exists;
                to_fetch = deficit;
            }
        }
        mail_dbg("[mail] do_fetch_summaries QRESYNC delta applied kept=%zu have=%d exists=%d fetch %d:%d full=%d\n",
                base_summaries.size(), have, exists, fetch_first, fetch_last, (int)needs_full_refetch);
        if (needs_full_refetch) {
            // Large deficit (e.g. mass delete) — fall through to full FETCH at the bottom
            // by treating this as if no resumable cache (clear pending first).
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_qresync_pending = false;
                m_pending_qresync_folder.clear();
            }
            // Recompute fetch window since the earlier is_qresync_resume left `first` at 0.
            if (is_auto) {
                first = m_last_known_exists + 1;
                if (first > exists) {
                    m_last_known_exists = exists;
                    return;
                }
            } else {
                first = m_first_loaded > 0 ? m_first_loaded
                                           : std::max(1, exists - 149);
            }
        } else if (to_fetch > 0) {
            m_progress_quiet = is_auto;
            std::vector<MailSummary> fresh;
            if (!m_imap.fetch_summaries(fetch_first, fetch_last, fresh, err)) {
                m_progress_quiet = false;
                mail_dbg("[mail] FETCH summaries QRESYNC delta '%s' failed: %s\n", folder.c_str(), err.c_str());
                if (!folder_wanted(folder) || is_stale_err(err))
                    return;
                report_error("Could not fetch messages", err);
                return;
            }
            m_progress_quiet = false;
            if (!folder_wanted(folder)) {
                mail_dbg("[mail] FETCH summaries QRESYNC delta '%s' dropped\n", folder.c_str());
                return;
            }
            // Merge: fresh is seq-asc from server; filter to only UIDs not already kept,
            // then splice. Server already filtered VANISHED, so any overlap is duplicate.
            std::unordered_set<uint32_t> seen_uid;
            for (auto &s : base_summaries) if (s.uid) seen_uid.insert(s.uid);
            for (auto &s : fresh) {
                if (s.uid && seen_uid.count(s.uid)) continue;
                base_summaries.push_back(s);
            }

            // For correct seq ordering prior to delivery we sort by seq desc.
            // seq values from fresh are at the top (largest), so sort desc gives right display.
            std::sort(base_summaries.begin(), base_summaries.end(),
                      [](const MailSummary &a, const MailSummary &b){ return a.seq > b.seq; });
            summaries = std::move(base_summaries);
        } else {
            // No new messages: just the delta-patched cache. Also need to handle
            // has_qresync but no growth – no FETCH needed.
            summaries = std::move(base_summaries);
            // If cache was exactly 150 and exists same, we can deliver without FETCH.
            // For correctness, if exists differs but deficit==0 yet have==exists path already.
            mail_dbg("[mail] do_fetch_summaries QRESYNC no FETCH needed, delivering %zu cached\n", summaries.size());
            if (!folder_wanted(folder)) return;
            // m_imap.highestmodseq already updated by SELECT QRESYNC; ensure paging counters.
            if (!is_auto) m_first_loaded = summaries.empty() ? 0 : summaries.back().seq;
            m_last_known_exists = std::max(m_last_known_exists, exists);
            // newest-first already sorted desc above when we applied sort in branch below,
            // but in this branch summaries preserves patched order from cache which was
            // already newest-first. So ensure desc if not fresh-merge path.
            // Cache was stored newest-first, patched in place, still newest-first.
            if (!summaries.empty()) {
                bool is_desc = true;
                for (size_t i = 1; i < summaries.size(); ++i) if (summaries[i-1].seq < summaries[i].seq) { is_desc = false; break; }
                if (!is_desc)
                    std::sort(summaries.begin(), summaries.end(),
                              [](const MailSummary &a, const MailSummary &b){ return a.seq > b.seq; });
            }
            // Record QRESYNC state + uid list back to disk and memory, then deliver.
            {
                std::vector<uint32_t> all_uids;
                all_uids.reserve(summaries.size());
                for (auto &s : summaries) if (s.uid) all_uids.push_back(s.uid);
                // Keep newest 300 UIDs for next QRESYNC round (seqset may grow large otherwise)
                if (all_uids.size() > 300) {
                    std::sort(all_uids.begin(), all_uids.end());
                    all_uids.erase(all_uids.begin(), all_uids.begin() + (all_uids.size() - 300));
                }
                ImapClient::QResyncState cur = m_imap.qresync_state();
                cur.messages = (uint32_t)exists;
                save_qresync_state(folder, cur, all_uids);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_summaries_cache[folder] = summaries;
                    m_qresync_cache[folder] = cur;
                    m_qresync_pending = false;
                    m_pending_qresync_folder.clear();
                }
            }
            report_status(folder + ": " + std::to_string(exists) +
                          (exists == 1 ? " message" : " messages") +
                          (summaries.size() != (size_t)exists
                               ? " (showing " + std::to_string(summaries.size()) + ")"
                               : ""),
                          folder);
            deliver([this, folder, summaries]() {
                if (cb_summaries) cb_summaries(folder, summaries);
            });
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_prefetch_folder = folder;
                m_prefetch_queue.clear();
                m_prefetch_queued.clear();
                m_prefetch_uid_queue.clear();
                m_prefetch_uid_queued.clear();
                bool has_uid = false; for (auto &s : summaries) if (s.uid) { has_uid = true; break; }
                if (has_uid) {
                    for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                        if (!summaries[i].uid) continue;
                        if (m_prefetch_uid_queued.count(summaries[i].uid)) continue;
                        m_prefetch_uid_queue.push_back(summaries[i].uid);
                        m_prefetch_uid_queued.insert(summaries[i].uid);
                    }
                } else {
                    for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                        if (m_prefetch_queued.count(summaries[i].seq)) continue;
                        m_prefetch_queue.push_back(summaries[i].seq);
                        m_prefetch_queued.insert(summaries[i].seq);
                    }
                }
            }
            schedule_next_prefetch();
            schedule_next_prefetch_uid();
            return;
        }
        if (!needs_full_refetch) {
            // After merging fresh slice — deliver and return (only when not falling back).
            m_progress_quiet = false;
            if (!folder_wanted(folder)) {
                mail_dbg("[mail] FETCH summaries QRESYNC merged '%s' dropped\n", folder.c_str());
                return;
            }
            mail_dbg("[mail] FETCH summaries QRESYNC merged '%s' OK %zu rows\n", folder.c_str(), summaries.size());
            if (!is_auto) m_first_loaded = summaries.empty() ? 0 : summaries.back().seq;
            m_last_known_exists = std::max(m_last_known_exists, exists);
            if (!is_auto)
                report_status(folder + ": " + std::to_string(exists) +
                              (exists == 1 ? " message" : " messages") +
                              (summaries.size() != (size_t)exists
                                   ? " (showing " + std::to_string(summaries.size()) + ")"
                                   : ""),
                              folder);
            {
                std::vector<uint32_t> all_uids;
                all_uids.reserve(summaries.size());
                for (auto &s : summaries) if (s.uid) all_uids.push_back(s.uid);
                if (all_uids.size() > 300) {
                    std::sort(all_uids.begin(), all_uids.end());
                    all_uids.erase(all_uids.begin(), all_uids.begin() + (all_uids.size() - 300));
                }
                ImapClient::QResyncState cur = m_imap.qresync_state();
                cur.messages = (uint32_t)exists;
                for (auto &s : summaries) if (s.modseq > cur.highestmodseq) cur.highestmodseq = s.modseq;
                save_qresync_state(folder, cur, all_uids);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_summaries_cache[folder] = summaries;
                    m_qresync_cache[folder] = cur;
                    m_qresync_pending = false;
                    m_pending_qresync_folder.clear();
                }
            }
            deliver([this, folder, summaries, is_auto]() {
                if (is_auto) { if (cb_auto_summaries) cb_auto_summaries(folder, summaries); }
                else         { if (cb_summaries)      cb_summaries(folder, summaries); }
            });
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_prefetch_folder = folder;
                if (!is_auto) {
                    m_prefetch_queue.clear();
                    m_prefetch_queued.clear();
                    m_prefetch_uid_queue.clear();
                    m_prefetch_uid_queued.clear();
                }
                bool has_uid = false; for (auto &s : summaries) if (s.uid) { has_uid = true; break; }
                if (has_uid) {
                    for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                        if (!summaries[i].uid) continue;
                        if (m_prefetch_uid_queued.count(summaries[i].uid)) continue;
                        m_prefetch_uid_queue.push_back(summaries[i].uid);
                        m_prefetch_uid_queued.insert(summaries[i].uid);
                    }
                } else {
                    for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                        if (m_prefetch_queued.count(summaries[i].seq)) continue;
                        m_prefetch_queue.push_back(summaries[i].seq);
                        m_prefetch_queued.insert(summaries[i].seq);
                    }
                }
            }
            schedule_next_prefetch();
            schedule_next_prefetch_uid();
            return;
        }
        // needs_full_refetch: fall through to full FETCH path below (is_qresync_resume handled)
        is_qresync_resume = false;
    }
    // Non-QRESYNC (or no cached resume) full FETCH path.
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
    // Persist QRESYNC anchor when capability is active, so next select_qresync can reuse it.
    if (m_imap.has_qresync()) {
        // Safety net for the "throw away" bug: if we have a larger
        // paged cache (e.g., 1700 after do_fetch_older) but the fresh
        // FETCH only returned the newest 150, keep the larger window
        // when the server state hasn't visibly changed.  QRESYNC resume
        // already handles this for capable servers (have==exists => no
        // FETCH), so this branch is a fallback for needs_full_refetch or
        // plain SELECT after paging.
        if (!is_auto) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_summaries_cache.find(folder);
            if (it != m_summaries_cache.end() && it->second.size() > summaries.size()
                && (int)it->second.size() == exists) {
                // Cached window already spans the whole mailbox; fresh 150
                // is a subset of it (no server growth/shrink). Keep 1700.
                summaries = it->second;
                // Update paging counters to the oldest seq in the kept window.
                m_first_loaded = summaries.empty() ? 0 : summaries.back().seq;
            }
        }
        std::vector<uint32_t> uids;
        uids.reserve(summaries.size());
        for (auto &s : summaries) if (s.uid) uids.push_back(s.uid);
        if (uids.size() > 300) {
            std::sort(uids.begin(), uids.end());
            uids.erase(uids.begin(), uids.begin() + (uids.size() - 300));
        }
        ImapClient::QResyncState cur = m_imap.qresync_state();
        cur.messages = (uint32_t)exists;
        if (!is_auto) save_qresync_state(folder, cur, uids);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!is_auto) {
                m_summaries_cache[folder] = summaries;
                m_qresync_cache[folder] = cur;
            }
            m_qresync_pending = false;
            m_pending_qresync_folder.clear();
        }
    } else {
        // Non-QRESYNC server: still grow cache via paging, and don't
        // throw it away when a plain SELECT returns only 150 newest.
        if (!is_auto) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_summaries_cache.find(folder);
            if (it != m_summaries_cache.end() && it->second.size() > summaries.size()) {
                if ((int)it->second.size() == exists) {
                    summaries = it->second;
                    m_first_loaded = summaries.empty() ? 0 : summaries.back().seq;
                } else {
                    // Server size changed — fresh 150 is authoritative for
                    // non-QRESYNC (seqs shifted); overwrite.
                    m_summaries_cache[folder] = summaries;
                }
            } else {
                m_summaries_cache[folder] = summaries;
            }
            m_qresync_pending = false;
            m_pending_qresync_folder.clear();
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_qresync_pending = false;
        }
    }
    deliver([this, folder, summaries, is_auto]() {
        if (is_auto) { if (cb_auto_summaries) cb_auto_summaries(folder, summaries); }
        else         { if (cb_summaries)      cb_summaries(folder, summaries); }
    });
    // Seed a low-priority backlog with the first 25 (newest first).
    // Further visible rows get enqueued on demand via ensure_visible_cached().
    // Keep both seq and uid queues: uid path is QRESYNC-stable.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_prefetch_folder = folder;
        if (!is_auto) {
            m_prefetch_queue.clear();
            m_prefetch_queued.clear();
            m_prefetch_uid_queue.clear();
            m_prefetch_uid_queued.clear();
        }
        // Prefer UID queue when UIDs are available (QRESYNC path)
        bool has_uid = false;
        for (auto &s : summaries) if (s.uid) { has_uid = true; break; }
        if (has_uid) {
            for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                if (!summaries[i].uid) continue;
                if (m_prefetch_uid_queued.count(summaries[i].uid)) continue;
                m_prefetch_uid_queue.push_back(summaries[i].uid);
                m_prefetch_uid_queued.insert(summaries[i].uid);
            }
        } else {
            for (size_t i = 0; i < summaries.size() && i < 25; ++i) {
                if (m_prefetch_queued.count(summaries[i].seq)) continue;
                m_prefetch_queue.push_back(summaries[i].seq);
                m_prefetch_queued.insert(summaries[i].seq);
            }
        }
    }
    schedule_next_prefetch();
    schedule_next_prefetch_uid();
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
    // FIX: grow RAM cache and QRESYNC anchor so switching back to
    // this folder restores the full paged window (1700) instead of
    // the original 150.  Summaries are newest-first within the page
    // but the whole page is older than the existing cache tail, so
    // append at the end.  Keep full list in RAM, trim to newest 300
    // UIDs on disk for QRESYNC VANISHED advertising.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_summaries_cache.find(folder);
        if (it != m_summaries_cache.end()) {
            if (!it->second.empty() && !summaries.empty() && summaries[0].uid != 0) {
                std::unordered_set<uint32_t> seen;
                seen.reserve(it->second.size() * 2);
                for (auto &s : it->second) if (s.uid) seen.insert(s.uid);
                std::vector<MailSummary> filtered;
                filtered.reserve(summaries.size());
                for (auto &s : summaries) if (!s.uid || !seen.count(s.uid)) filtered.push_back(s);
                if (!filtered.empty())
                    it->second.insert(it->second.end(), filtered.begin(), filtered.end());
            } else {
                it->second.insert(it->second.end(), summaries.begin(), summaries.end());
            }
        } else {
            m_summaries_cache[folder] = summaries;
        }
    }
    if (m_imap.has_qresync() && !summaries.empty()) {
        std::vector<uint32_t> all_uids;
        ImapClient::QResyncState cur;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_summaries_cache.find(folder);
            if (it != m_summaries_cache.end()) {
                all_uids.reserve(it->second.size());
                for (auto &s : it->second) if (s.uid) all_uids.push_back(s.uid);
            }
            cur = m_imap.qresync_state();
            for (auto &s : summaries) if (s.modseq > cur.highestmodseq) cur.highestmodseq = s.modseq;
        }
        std::sort(all_uids.begin(), all_uids.end());
        all_uids.erase(std::unique(all_uids.begin(), all_uids.end()), all_uids.end());
        if (all_uids.size() > 300) {
            all_uids.erase(all_uids.begin(), all_uids.begin() + (all_uids.size() - 300));
        }
        save_qresync_state(folder, cur, all_uids);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_qresync_cache[folder] = cur;
        }
    }
    deliver([this, folder, summaries]() {
        if (cb_older) cb_older(folder, summaries);
    });
}

void MailWorker::cancel_prefetch_locked() {
    m_prefetch_folder.clear();
    m_prefetch_queue.clear();
    m_prefetch_queued.clear();
    m_prefetch_uid_queue.clear();
    m_prefetch_uid_queued.clear();
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
        [](const Cmd &c){ return c.type == Type::Prefetch || c.type == Type::PrefetchUid; }), m_queue.end());
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
        // Remove the moving message from any pending prefetch work (both seq and uid paths).
        m_prefetch_queue.erase(
            std::remove(m_prefetch_queue.begin(), m_prefetch_queue.end(), seq),
            m_prefetch_queue.end());
        m_prefetch_queued.erase(seq);
        // Also drop UID prefetch for this message if known
        uint32_t uid_moving = 0;
        auto itc = m_summaries_cache.find(folder);
        if (itc != m_summaries_cache.end())
            for (auto &s : itc->second) if (s.seq == seq && s.uid) { uid_moving = s.uid; break; }
        if (uid_moving) {
            m_prefetch_uid_queue.erase(
                std::remove(m_prefetch_uid_queue.begin(), m_prefetch_uid_queue.end(), uid_moving),
                m_prefetch_uid_queue.end());
            m_prefetch_uid_queued.erase(uid_moving);
        }
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
            [&](const Cmd &c){
                if (c.folder != folder) return false;
                if (c.type == Type::Prefetch && c.seq == seq) return true;
                if (c.type == Type::PrefetchUid && uid_moving && c.uid == uid_moving) return true;
                return false;
            }),
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

    // Resolve UID/MODSEQ for the CONDSTORE path from the summaries cache.
    // The timer may have been armed with an explicit uid/modseq; otherwise
    // look it up by seq from the last fetched summaries for this folder.
    uint32_t uid = cmd.uid;
    uint64_t modseq = cmd.modseq;
    if (uid == 0 || modseq == 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_summaries_cache.find(cmd.folder);
        if (it != m_summaries_cache.end()) {
            for (auto &s : it->second) if (s.seq == cmd.seq) {
                if (uid == 0)    uid = s.uid;
                if (modseq == 0) modseq = s.modseq;
                break;
            }
        }
    }

    bool use_uid = (uid != 0 && (m_imap.has_condstore() || m_imap.has_qresync()));
    // If we have a UID path, prefer UID STORE (stable across EXPUNGE).
    // When CONDSTORE is available and modseq != 0, UNCHANGEDSINCE is added
    // and a [MODIFIED] response is treated as success inside mark_seen_uid.
    auto do_store = [&]() -> bool {
        if (use_uid) return m_imap.mark_seen_uid(uid, modseq, err);
        return m_imap.mark_seen(cmd.seq, err);
    };

    if (!do_store()) {
        if (ImapClient::is_modified_error(err)) {
            // Benign: concurrent modification reported as error by
            // a non-CONDSTORE path (should not happen via mark_seen_uid
            // which already converts it to success), but handle anyway.
        } else if (!ImapClient::is_connection_error(err))
            return;
        else {
            std::string re;
            if (!m_imap.reconnect(re) ||
                !m_imap.ensure_selected(cmd.folder, err) ||
                !do_store()) {
                if (ImapClient::is_modified_error(err)) {
                    // fall through to success
                } else
                    return;
            }
        }
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

bool MailWorker::already_prefetch_uid_queued_locked(uint32_t uid, const std::string &folder) const {
    if (m_prefetch_uid_queued.count(uid)) return true;
    for (auto &c : m_queue)
        if (c.type == Type::PrefetchUid && c.uid == uid && c.folder == folder) return true;
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

void MailWorker::schedule_next_prefetch_uid() {
    std::string folder; uint32_t uid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_prefetch_uid_queue.empty()) return;
        if (!m_selected_folder.empty() && m_selected_folder != m_prefetch_folder)
            return;
        folder = m_prefetch_folder;
        uid = m_prefetch_uid_queue.front();
        for (auto &c : m_queue)
            if (c.type == Type::PrefetchUid && c.uid == uid && c.folder == folder) return;
    }
    Cmd c; c.type = Type::PrefetchUid; c.folder = folder; c.uid = uid; c.epoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        c.epoch = m_epoch;
        m_queue.push_back(c);
    }
    m_cv.notify_one();
}

void MailWorker::do_prefetch(const Cmd &cmd) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool has_priority = false;
        for (auto &c : m_queue) if (c.type != Type::Prefetch && c.type != Type::PrefetchUid) { has_priority = true; break; }
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
        if (!is_stale_err(err) && folder_wanted(cmd.folder)) {
            schedule_next_prefetch();
            schedule_next_prefetch_uid();
        }
        return;
    }
    std::string preview = message_preview(msg);
    deliver([this, folder = cmd.folder, seq = cmd.seq, msg, preview]() {
        if (cb_prefetched) cb_prefetched(folder, seq, msg, preview);
    });
    if (folder_wanted(cmd.folder)) {
        schedule_next_prefetch();
        schedule_next_prefetch_uid();
    }
}

void MailWorker::do_prefetch_uid(const Cmd &cmd) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool has_priority = false;
        for (auto &c : m_queue) if (c.type != Type::Prefetch && c.type != Type::PrefetchUid) { has_priority = true; break; }
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
            if (!m_prefetch_uid_queue.empty() && m_prefetch_uid_queue.front() == cmd.uid) {
                m_prefetch_uid_queue.pop_front(); m_prefetch_uid_queued.erase(cmd.uid);
            }
            return;
        }
    }
    if (!cmd.folder.empty() && m_imap.selected_folder() != cmd.folder) {
        std::string se; m_imap.ensure_selected(cmd.folder, se);
    }
    MailMessage msg; std::string err;
    bool ok = m_imap.fetch_message_by_uid(cmd.uid, msg, err,
        [this, folder = cmd.folder]() { return folder_wanted(folder); });
    mail_dbg("[mail] prefetch uid=%u folder='%s' done ok=%d wanted='%s'\n",
            cmd.uid, cmd.folder.c_str(), (int)ok, wanted_folder_copy().c_str());
    // Resolve seq for legacy callback (seq may be 0 if server resequenced since summary).
    int seq_for_cb = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_summaries_cache.find(cmd.folder);
        if (it != m_summaries_cache.end()) {
            for (auto &s : it->second) if (s.uid == cmd.uid) { seq_for_cb = s.seq; break; }
        }
        if (!m_prefetch_uid_queue.empty() && m_prefetch_uid_queue.front() == cmd.uid) {
            m_prefetch_uid_queue.pop_front(); m_prefetch_uid_queued.erase(cmd.uid);
        } else {
            auto qit = std::find(m_prefetch_uid_queue.begin(), m_prefetch_uid_queue.end(), cmd.uid);
            if (qit != m_prefetch_uid_queue.end()) { m_prefetch_uid_queue.erase(qit); m_prefetch_uid_queued.erase(cmd.uid); }
        }
    }
    if (!ok) {
        mail_dbg("[mail] prefetch uid=%u folder='%s' failed: %s\n",
                cmd.uid, cmd.folder.c_str(), err.c_str());
        if (!is_stale_err(err) && folder_wanted(cmd.folder)) {
            schedule_next_prefetch();
            schedule_next_prefetch_uid();
        }
        return;
    }
    std::string preview = message_preview(msg);
    deliver([this, folder = cmd.folder, seq = seq_for_cb, msg, preview]() {
        if (cb_prefetched) cb_prefetched(folder, seq, msg, preview);
    });
    if (folder_wanted(cmd.folder)) {
        schedule_next_prefetch();
        schedule_next_prefetch_uid();
    }
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
                    cmd.uid    = m_seen_uid;
                    cmd.modseq = m_seen_modseq;
                    m_seen_seq = 0;             // consume the request
                    m_seen_uid = 0;
                    m_seen_modseq = 0;
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
        case Type::PrefetchUid:
            do_prefetch_uid(cmd);
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
