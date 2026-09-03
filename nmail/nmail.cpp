/**
 * nmail.cpp — nmail: a NanoGUI2 IMAP mail client
 *
 * Based on folderview.cpp.  Pulls folders and messages live from an IMAP
 * server (plain text, no SSL).  Nothing is cached: folders, message lists,
 * and message bodies are fetched on demand over a single connection driven
 * by a worker thread (the GUI never blocks).
 *
 * Account details are entered in the Preferences window and stored in
 * "amail.config" (JSON, via dict.h).
 */

#include "nanogui/widget.h"
#include <nanogui/nanogui.h>
#include <nanogui/opengl.h>
#include <nanogui/scrollpanel.h>
#include <nanogui/zoomscrollpanel.h>
#include <nanogui/split.h>
#include <nanogui/layout.h>
#include <nanogui/icons.h>
#include <nanogui/texteditor.h>
#include <nanogui/textbox.h>
#include <nanogui/menu.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/messagedialog.h>
#include <nanogui/spinner.h>
#include <nanogui/autocomplete.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <fstream>
#include <functional>
#include <vector>
#include <string>
#include <sstream>
#include <array>
#include <initializer_list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <csignal>

#include "dict.h"
#include "imap_client.h"

#if NMAIL_IMAP_DEBUG
#define mail_dbg(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#else
#define mail_dbg(...) ((void)0)
#endif
#include "smtp_client.h"
#include "nmail_socket.h"
#include "http_fetch.h"
#include "htmldocument.h"
#include "saved_email.h"
#include "contacts.h"
#include "gumbo.h"
#include <memory>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#ifdef _WIN32
#include <direct.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#endif
#include <stb_image.h>
#include "nmail_icon.h"
#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#endif

using namespace nanogui;

// ---------------------------------------------------------------------------
// Configuration (stored in amail.config)
// ---------------------------------------------------------------------------
struct MailConfig {
    std::string host;
    int         port = 143;
    std::string username;
    std::string password;
    std::string smtp_host;      // empty -> fall back to the IMAP host
    int         smtp_port = 587;
    bool        dark_mode = false;
    /* Off by default: harvested addresses stay in memory for the session
     * unless the user opts into keeping them on disk. */
    bool        save_contacts = false;
    int         check_interval_min = 15;  // how often to auto-check for new mail
    /* Base font size for the compose/reply editor; headings and code blocks
     * are drawn as ratios of this (see TextEditor::set_base_font_size). */
    int         compose_font_size = 16;
};

/* Writable per-user directory: %APPDATA%\nmail on Windows, ~/.amail elsewhere.
   Program Files (or whatever the CWD happens to be) is not a place for prefs.
   The directory holds amail.config and amail.key. */
static const std::string &config_dir() {
    static const std::string dir = []() -> std::string {
#ifdef _WIN32
        const char *base = std::getenv("APPDATA");
        if (base && base[0]) {
            std::string d = std::string(base) + "\\nmail";
            if (_mkdir(d.c_str()) == 0 || errno == EEXIST)
                return d;
        }
#else
        const char *base = std::getenv("HOME");
        if (base && base[0]) {
            std::string d = std::string(base) + "/.amail";
            if (::mkdir(d.c_str(), 0700) == 0 || errno == EEXIST)
                return d;
        }
#endif
        /* No usable home: keep working out of the current directory. */
        return std::string(".");
    }();
    return dir;
}

static std::string config_file(const char *name) {
#ifdef _WIN32
    return config_dir() + "\\" + name;
#else
    return config_dir() + "/" + name;
#endif
}

static const std::string &config_path() {
    static const std::string path = config_file("amail.config");
    return path;
}

/* Pre-1.0 builds kept the config in the working directory. */
static const char *legacy_config_path() { return "amail.config"; }

// ---------------------------------------------------------------------------
// Password storage.  The password has to be recoverable to log in, so this is
// AES-256-GCM under a random key kept beside the config (amail.key, mode 0600)
// rather than a passphrase.  It keeps the password out of the config file --
// which gets copied into backups, sync folders and bug reports -- but it does
// not defend against someone who can already read the user's home directory.
// ---------------------------------------------------------------------------
#ifdef HAVE_OPENSSL

enum { kKeyBytes = 32, kIvBytes = 12, kTagBytes = 16 };

/* 32 random bytes in amail.key, created on first use. */
static bool config_key(unsigned char key[kKeyBytes]) {
    const std::string path = config_file("amail.key");
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            in.read((char *)key, kKeyBytes);
            if (in.gcount() == kKeyBytes) return true;
        }
    }
    if (RAND_bytes(key, kKeyBytes) != 1) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write((const char *)key, kKeyBytes);
    out.close();
    if (!out) return false;
#ifndef _WIN32
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

static std::string b64_encode(const unsigned char *data, int len) {
    std::string out((size_t)((len + 2) / 3) * 4 + 1, '\0');
    int n = EVP_EncodeBlock((unsigned char *)&out[0], data, len);
    out.resize(n > 0 ? (size_t)n : 0);
    return out;
}

static std::vector<unsigned char> b64_decode(const std::string &s) {
    if (s.empty() || s.size() % 4) return {};
    std::vector<unsigned char> out(s.size() / 4 * 3);
    int n = EVP_DecodeBlock(out.data(), (const unsigned char *)s.data(),
                            (int)s.size());
    if (n < 0) return {};
    /* EVP_DecodeBlock rounds up to a multiple of 3; drop the '=' padding. */
    size_t pad = 0;
    while (pad < 2 && pad < s.size() && s[s.size() - 1 - pad] == '=') ++pad;
    out.resize((size_t)n > pad ? (size_t)n - pad : 0);
    return out;
}

/* base64(iv | tag | ciphertext) */
static bool encrypt_secret(const std::string &plain, std::string &out_b64) {
    unsigned char key[kKeyBytes];
    if (!config_key(key)) return false;

    std::vector<unsigned char> blob(kIvBytes + kTagBytes + plain.size());
    unsigned char *iv  = blob.data();
    unsigned char *tag = iv + kIvBytes;
    unsigned char *ct  = tag + kTagBytes;

    bool ok = RAND_bytes(iv, kIvBytes) == 1;
    EVP_CIPHER_CTX *ctx = ok ? EVP_CIPHER_CTX_new() : NULL;
    int len = 0, total = 0;
    if (ctx) {
        ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1;
        if (ok && !plain.empty())
            ok = EVP_EncryptUpdate(ctx, ct, &len,
                                   (const unsigned char *)plain.data(),
                                   (int)plain.size()) == 1;
        total = len;
        if (ok) ok = EVP_EncryptFinal_ex(ctx, ct + total, &len) == 1;
        total += len;
        if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                         kTagBytes, tag) == 1;
        EVP_CIPHER_CTX_free(ctx);
    } else {
        ok = false;
    }
    OPENSSL_cleanse(key, sizeof(key));
    if (!ok) return false;

    out_b64 = b64_encode(blob.data(), kIvBytes + kTagBytes + total);
    OPENSSL_cleanse(blob.data(), blob.size());
    return true;
}

static bool decrypt_secret(const std::string &b64, std::string &out) {
    std::vector<unsigned char> blob = b64_decode(b64);
    if (blob.size() < (size_t)(kIvBytes + kTagBytes)) return false;

    unsigned char key[kKeyBytes];
    if (!config_key(key)) return false;

    const unsigned char *iv  = blob.data();
    const unsigned char *tag = iv + kIvBytes;
    const unsigned char *ct  = tag + kTagBytes;
    const int ct_len = (int)(blob.size() - kIvBytes - kTagBytes);

    std::string plain((size_t)ct_len + 1, '\0');
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    bool ok = ctx != NULL;
    if (ok) {
        ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1;
        if (ok && ct_len)
            ok = EVP_DecryptUpdate(ctx, (unsigned char *)&plain[0], &len,
                                   ct, ct_len) == 1;
        total = len;
        if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                                         (void *)tag) == 1;
        /* Fails if the blob was tampered with or the key no longer matches. */
        if (ok) ok = EVP_DecryptFinal_ex(ctx, (unsigned char *)&plain[0] + total,
                                         &len) == 1;
        total += len;
        EVP_CIPHER_CTX_free(ctx);
    }
    OPENSSL_cleanse(key, sizeof(key));
    if (!ok) return false;

    plain.resize((size_t)total);
    out = plain;
    return true;
}

#else  /* no OpenSSL: nothing to encrypt with, fall back to plain text */

static bool encrypt_secret(const std::string &, std::string &) { return false; }
static bool decrypt_secret(const std::string &, std::string &) { return false; }

#endif

static std::string config_get_str(const DictValue *root, const char *key) {
    const DictValue *v = dict_object_get(root, key);
    return (v && v->type == DICT_STRING && v->string_value)
               ? v->string_value : "";
}

static bool load_config(MailConfig &c) {
    std::ifstream in(config_path(), std::ios::binary);
    if (!in) {
        in.clear();
        in.open(legacy_config_path(), std::ios::binary);
        if (!in) return false;
        std::cerr << "[nmail] migrating " << legacy_config_path() << " to "
                  << config_path() << "; delete the old file afterwards"
                  << std::endl;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return false;

    char err[256] = {0};
    DictValue *root = dict_deserialize_json_len(text.c_str(), text.size(),
                                            err, sizeof(err));
    if (!root) {
        std::cerr << "[nmail] could not parse " << config_path() << ": "
                  << err << std::endl;
        return false;
    }
    c.host     = config_get_str(root, "host");
    c.username = config_get_str(root, "username");
    c.password = config_get_str(root, "password");   /* legacy plain text */
    const std::string enc = config_get_str(root, "password_enc");
    if (!enc.empty()) {
        std::string plain;
        if (decrypt_secret(enc, plain))
            c.password = plain;
        else
            std::cerr << "[nmail] could not decrypt the stored password ("
                      << config_file("amail.key")
                      << " missing or stale); re-enter it in Preferences"
                      << std::endl;
    }
    c.smtp_host = config_get_str(root, "smtp_host");
    const DictValue *p = dict_object_get(root, "port");
    if (p && p->type == DICT_INT64)  c.port = (int)p->int64_value;
    if (p && p->type == DICT_NUMBER) c.port = (int)p->number_value;
    const DictValue *sp = dict_object_get(root, "smtp_port");
    if (sp && sp->type == DICT_INT64)  c.smtp_port = (int)sp->int64_value;
    if (sp && sp->type == DICT_NUMBER) c.smtp_port = (int)sp->number_value;
    const DictValue *dm = dict_object_get(root, "dark_mode");
    if (dm && dm->type == DICT_BOOL) c.dark_mode = dm->bool_value != 0;
    const DictValue *sc = dict_object_get(root, "save_contacts");
    if (sc && sc->type == DICT_BOOL) c.save_contacts = sc->bool_value != 0;
    const DictValue *ci = dict_object_get(root, "check_interval_min");
    if (ci && ci->type == DICT_INT64)  c.check_interval_min = (int)ci->int64_value;
    if (ci && ci->type == DICT_NUMBER) c.check_interval_min = (int)ci->number_value;
    const DictValue *fs = dict_object_get(root, "compose_font_size");
    if (fs && fs->type == DICT_INT64)  c.compose_font_size = (int)fs->int64_value;
    if (fs && fs->type == DICT_NUMBER) c.compose_font_size = (int)fs->number_value;
    dict_destroy(root);
    return !c.host.empty();
}

static bool save_config(const MailConfig &c) {
    DictValue *root = dict_create_object();
    dict_object_set(root, "host",     dict_create_string(c.host.c_str()));
    dict_object_set(root, "port",     dict_create_int64(c.port));
    dict_object_set(root, "username", dict_create_string(c.username.c_str()));
    std::string enc;
    if (encrypt_secret(c.password, enc))
        dict_object_set(root, "password_enc", dict_create_string(enc.c_str()));
    else
        dict_object_set(root, "password", dict_create_string(c.password.c_str()));
    dict_object_set(root, "smtp_host", dict_create_string(c.smtp_host.c_str()));
    dict_object_set(root, "smtp_port", dict_create_int64(c.smtp_port));
    dict_object_set(root, "dark_mode", dict_create_bool(c.dark_mode ? 1 : 0));
    dict_object_set(root, "save_contacts",
                    dict_create_bool(c.save_contacts ? 1 : 0));
    dict_object_set(root, "check_interval_min", dict_create_int64(c.check_interval_min));
    dict_object_set(root, "compose_font_size", dict_create_int64(c.compose_font_size));

    char buf[8192];
    bool ok = false;
    if (dict_serialize_json(root, buf, sizeof(buf), /*pretty=*/1)) {
        std::ofstream out(config_path(), std::ios::binary | std::ios::trunc);
        if (out) {
            out << buf;
            ok = (bool)out;
        }
#ifndef _WIN32
        if (ok) ::chmod(config_path().c_str(), S_IRUSR | S_IWUSR);
#endif
    }
    dict_destroy(root);
    return ok;
}

/* Give the window the nmail logo.  Works on Windows and X11; GLFW documents
   this as unsupported on macOS (the bundle icon is used) and on Wayland. */
static void set_window_icon(GLFWwindow *win) {
#if !defined(__APPLE__)
    std::vector<GLFWimage> images;
    std::vector<unsigned char *> pixels;
    for (const NmailIconBlob &blob : nmail_icon_pngs) {
        int w = 0, h = 0, ch = 0;
        unsigned char *px = stbi_load_from_memory(blob.data, (int)blob.size,
                                                  &w, &h, &ch, 4);
        if (!px) continue;
        pixels.push_back(px);
        GLFWimage img;
        img.width  = w;
        img.height = h;
        img.pixels = px;
        images.push_back(img);
    }
    if (!images.empty())
        glfwSetWindowIcon(win, (int)images.size(), images.data());
    for (unsigned char *px : pixels)
        stbi_image_free(px);
#else
    (void)win;
#endif
}

/* Launched from Finder a bundle gets "/" as its working directory, but
   theme.cpp loads "resources/NotoColorEmoji.ttf" relative to it.  Point the
   CWD at Contents/Resources, where the CMake bundle rules put the faces. */
static void chdir_to_bundle_resources() {
#ifdef __APPLE__
    char exe[4096];
    uint32_t len = sizeof(exe);
    if (_NSGetExecutablePath(exe, &len) != 0) return;
    /* <bundle>/Contents/MacOS/nmail -> <bundle>/Contents/Resources */
    std::string dir = dirname(exe);            /* .../Contents/MacOS   */
    std::string res = dir + "/../Resources";
    if (::chdir(res.c_str()) != 0)
        std::cerr << "[nmail] could not chdir to " << res << std::endl;
#endif
}

// ---------------------------------------------------------------------------
// MailWorker — worker thread owning the blocking IMAP connection
// ---------------------------------------------------------------------------
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

    void set_config(const MailConfig &c) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = c;
    }

    void start() {
        m_imap.on_progress = [this](int done, int total) {
            report_fetch_progress(done, total);
        };
        m_thread = std::thread([this]() { run(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        m_imap.abort();   // wake a blocking recv
        if (m_thread.joinable())
            m_thread.join();
    }

    void connect()                              { post(Type::Connect); }
    void refresh()                              { post(Type::Refresh); }
    /* Latest mailbox the GUI asked to look at.  Select/Refresh/FetchOlder
     * all key off this so an in-flight INBOX fetch cannot clobber Trash. */
    void select_folder(const std::string &name) {
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
    void fetch_body(int seq) {
        std::string folder;
        { std::lock_guard<std::mutex> l(m_mutex); folder = m_wanted_folder.empty()
              ? m_selected_folder : m_wanted_folder; }
        post(Type::FetchBody, folder, seq);
    }
    void fetch_body(const std::string &folder, int seq) { post(Type::FetchBody, folder, seq); }
    void fetch_older(const std::string &folder)         { post(Type::FetchOlder, folder); }
    /* Flag `seq` in `folder` as \Seen once `delay_sec` has passed without a
     * newer request.  Calling again replaces the pending one, so moving to a
     * different message restarts the clock rather than queueing a second
     * mark. */
    void schedule_seen(const std::string &folder, int seq, double delay_sec) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_seen_folder = folder;
            m_seen_seq    = seq;
            m_seen_at     = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds((long long)(delay_sec * 1000.0));
        }
        m_cv.notify_one();   // the loop may need to wake earlier than planned
    }

    void cancel_seen() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_seen_seq = 0;
        }
        m_cv.notify_one();
    }

    void move_message(const std::string &folder, int seq,
                      const std::string &dest_folder) {
        post(Type::Move, folder, seq, dest_folder);
    }
    void ensure_visible_cached(const std::string &folder,
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

private:
    enum class Type { Connect, Refresh, Select, FetchBody, FetchOlder, Prefetch, Move, MarkSeen, AutoRefresh };
    struct Cmd {
        Type type;
        std::string folder;
        int seq = 0;
        std::string dest_folder; // for Move
        uint64_t epoch = 0;      // mailbox generation; stale cmds are dropped
    };

    void post(Type t, const std::string &folder = "", int seq = 0,
              const std::string &dest = "") {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back({t, folder, seq, dest, m_epoch});
        }
        m_cv.notify_one();
    }

    /* True if `folder` is still the mailbox the GUI wants. */
    bool folder_wanted(const std::string &folder) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !folder.empty() && folder == m_wanted_folder;
    }

    std::string wanted_folder_copy() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_wanted_folder.empty() ? m_selected_folder : m_wanted_folder;
    }

    /* Drop queued work that would SELECT or FETCH against a mailbox the
     * user is no longer looking at.  Move is kept (it names its source). */
    void drop_stale_mailbox_work_locked() {
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

    /* Marshal a function to the GUI thread and wake the main loop. */
    template <typename F> void deliver(F &&fn) {
        nanogui::async(std::function<void()>(std::forward<F>(fn)));
        glfwPostEmptyEvent();
    }

    bool quitting() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_quit;
    }

    void report_error(const std::string &title, const std::string &msg) {
        if (quitting()) return;
        deliver([this, title, msg]() {
            if (cb_error) cb_error(title, msg);
        });
    }

    void report_fetch_progress(int done, int total) {
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

    void report_status(const std::string &msg, const std::string &folder = "") {
        if (quitting()) return;
        /* Drop status for a mailbox the user has already left — otherwise
         * "Opening Trash..." lands after they clicked Inbox. */
        if (!folder.empty() && !folder_wanted(folder))
            return;
        deliver([this, msg, folder]() {
            if (cb_status) cb_status(msg, folder);
        });
    }

    /* Re-LOGIN without LIST (used after cancel() killed the socket). */
    bool ensure_connected() {
        if (m_imap.is_open())
            return true;
        std::string err;
        if (m_imap.reconnect(err, /*reselect=*/false))
            return true;
        return do_connect();
    }

    static bool is_stale_err(const std::string &err) {
        return ImapClient::is_cancelled_error(err);
    }

    /* Connect + LOGIN + LIST.  Returns false on failure (error reported). */
    bool do_connect() {
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

    bool do_list_folders() {
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

    void do_select(const std::string &folder) {
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

    /* is_auto: a periodic background check (Type::AutoRefresh) rather than
     * an explicit Refresh/Select. Fetches only the delta beyond the last
     * known message count (cheap, regardless of how far back the user has
     * paged) and delivers via cb_auto_summaries instead of cb_summaries, so
     * the GUI can merge new mail in without disturbing scroll/selection. */
    void do_fetch_summaries(const std::string &folder, int exists,
                            bool is_auto = false) {
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

    /* Fetch the next older page: the 150 messages just below the oldest
       one currently shown. */
    void do_fetch_older(const Cmd &cmd) {
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

    void cancel_prefetch_locked() {
        m_prefetch_folder.clear();
        m_prefetch_queue.clear();
        m_prefetch_queued.clear();
        m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
            [](const Cmd &c){ return c.type == Type::Prefetch; }), m_queue.end());
    }

    bool do_move(const Cmd &cmd) {
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

    /* Best effort: a failed read-flag is not worth interrupting the user,
     * so this reports nothing on error beyond a status line. */
    void do_mark_seen(const Cmd &cmd) {
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

    bool already_prefetch_queued_locked(int seq, const std::string &folder) const {
        if (m_prefetch_queued.count(seq)) return true;
        for (auto &c : m_queue)
            if (c.type == Type::Prefetch && c.seq == seq && c.folder == folder) return true;
        return false;
    }

    void schedule_next_prefetch() {
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



    void do_prefetch(const Cmd &cmd) {
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

    void run() {
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

    std::thread             m_thread;
    std::mutex              m_mutex;
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

    /* Pending "mark read" request; m_seen_seq == 0 means none. */
    std::string m_seen_folder;
    int         m_seen_seq = 0;
    std::chrono::steady_clock::time_point m_seen_at;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class FolderView;
class FolderSection;
class FolderItem;

// Currently selected item (app-wide)
static FolderItem *g_selected_item = nullptr;

class FolderChildrenContainer : public Widget {
public:
    FolderChildrenContainer(Widget *parent) : Widget(parent) {}

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        int total_h = 0;
        for (auto *child : m_children) {
            if (child->visible())
                total_h += child->preferred_size(ctx).y();
        }
        return Vector2i(m_parent ? m_parent->size().x() : 100, total_h);
    }
};

class FolderContainer : public Widget {
public:
    FolderContainer(Widget *parent) : Widget(parent) {}

    virtual void perform_layout(NVGcontext *ctx) override {
        Vector2i ps = preferred_size(ctx);
        if (ps.y() > m_size.y())
            m_size.y() = ps.y();
        Widget::perform_layout(ctx);
    }
};

// ---------------------------------------------------------------------------
// FolderItem — a single row in the sidebar
// ---------------------------------------------------------------------------
class FolderItem : public Widget {
public:
    FolderItem(Widget *parent, const std::string &caption, int icon,
               int indent = 0, int badge = 0, bool expandable = false)
        : Widget(parent),
          m_caption(caption), m_icon(icon), m_indent(indent),
          m_badge(badge), m_expandable(expandable),
          m_expanded(false), m_hovered(false),
          m_children_container(nullptr),
          m_select_callback(nullptr)
    {
        set_cursor(Cursor::Hand);
        int row_h = (int)(font_size() * 1.8f);
        set_min_height(row_h);
        set_height(row_h);
    }

    /* ---- Accessors ---- */
    const std::string &caption() const { return m_caption; }
    void set_caption(const std::string &c) { m_caption = c; }

    int badge() const { return m_badge; }
    void set_badge(int b) { m_badge = b; }

    bool selected() const { return g_selected_item == this; }

    bool expanded() const { return m_expanded; }

    void set_select_callback(std::function<void(FolderItem *)> cb) {
        m_select_callback = cb;
    }

    /* ---- Children container for expandable items ---- */
    Widget *children_container() const { return m_children_container; }

    Widget *ensure_children_container() {
        if (!m_children_container) {
            // The container is added as a sibling right after this item
            // in the parent's child list.  It will be animated.
            m_children_container = new FolderChildrenContainer(m_parent);
            m_children_container->set_layout(
                new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));
            m_children_container->set_visible(false);
        }
        return m_children_container;
    }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    /* ---- Events ---- */
    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        m_hovered = enter;
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button,
                                    bool down, int modifiers) override {
        if (button == GLFW_MOUSE_BUTTON_1 && down) {
            if (m_expandable && m_children_container) {
                toggle_expand();
            }
            // Select this item
            g_selected_item = this;
            if (m_select_callback)
                m_select_callback(this);
            return true;
        }
        return Widget::mouse_button_event(p, button, down, modifiers);
    }

    void toggle_expand() {
        if (!m_children_container) return;
        m_expanded = !m_expanded;
        m_children_container->set_animation_duration(0.4f);
        if (m_expanded) {
            m_children_container->set_visible(true);
            m_children_container->start_animation(
                Widget::AnimationType::SlideDown);
        } else {
            m_children_container->start_animation(
                Widget::AnimationType::SlideUp);
        }
        screen()->redraw();
    }

    /* ---- Drawing ---- */
    virtual void draw(NVGcontext *ctx) override {
        float fs = (float)font_size();
        float x = (float)m_pos.x();
        float y = (float)m_pos.y();
        float w = (float)m_size.x();
        float h = (float)m_size.y();

        float base_x = fs * 1.5f + m_indent * fs * 1.25f;
        float rounding = 6.0f;

        if (selected()) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 4, y + 1, w - 8, h - 2, rounding);
            nvgFillColor(ctx, Color(0, 102, 255, 255));
            nvgFill(ctx);
        } else if (m_hovered) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 4, y + 1, w - 8, h - 2, rounding);
            bool dark = screen() && screen()->theme_mode() == ThemeMode::Dark;
            nvgFillColor(ctx, dark ? Color(255, 255, 255, 25)
                                   : Color(0, 0, 0, 20));
            nvgFill(ctx);
        }

        Color text_col = selected() ? Color(255, 255, 255, 255)
                                    : m_theme->m_text_color;
        Color icon_col = selected() ? Color(255, 255, 255, 255)
                                    : m_theme->m_icon_color;

        if (m_expandable) {
            float tx = x + base_x - fs * 1.0f;
            float ty = y + h * 0.5f;
            float angle = m_expanded ? NVG_PI * 0.5f : 0.0f;
            nvgSave(ctx);
            nvgTranslate(ctx, tx, ty);
            nvgRotate(ctx, angle);
            nvgFontSize(ctx, fs * 0.75f);
            nvgFontFace(ctx, "icons");
            nvgFillColor(ctx, m_theme->m_disabled_text_color);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, 0, 0, utf8(FA_CARET_RIGHT).data(), nullptr);
            nvgRestore(ctx);
        }

        float icon_x = x + base_x;
        float icon_y = y + h * 0.5f;
        nvgFontSize(ctx, fs * 0.95f);
        nvgFontFace(ctx, "icons");
        nvgFillColor(ctx, icon_col);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(ctx, icon_x, icon_y, utf8(m_icon).data(), nullptr);

        float text_x = icon_x + fs * 1.1f;
        nvgFontSize(ctx, fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, text_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        {
            // Clip long folder names so they don't run under the badge
            nvgSave(ctx);
            float badge_rsv = (m_badge > 0) ? fs * 3.2f : fs * 1.2f;
            nvgIntersectScissor(ctx, text_x, y, w - (text_x - x) - badge_rsv, h);
            nvgText(ctx, text_x, y + h * 0.5f, m_caption.c_str(), nullptr);
            nvgRestore(ctx);
        }

        if (m_badge > 0) {
            std::string badge_text = std::to_string(m_badge);
            float badge_fs = fs * 0.7f;
            float bh = badge_fs * 1.6f;
            nvgFontSize(ctx, badge_fs);
            nvgFontFace(ctx, "sans-bold");
            float bounds[4];
            nvgTextBounds(ctx, 0, 0, badge_text.c_str(), nullptr, bounds);
            float bw = (bounds[2] - bounds[0]) + badge_fs * 0.9f;
            if (bw < bh) bw = bh;

            float bx = x + w - bw - fs * 1.5f;
            float by = y + (h - bh) * 0.5f;

            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, bx, by, bw, bh, bh * 0.5f);
            nvgFillColor(ctx, selected() ? Color(255, 255, 255, 200)
                                         : Color(180, 185, 195, 255));
            nvgFill(ctx);

            nvgFillColor(ctx, selected() ? Color(0, 102, 255, 255)
                                         : Color(255, 255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, bx + bw * 0.5f, by + bh * 0.5f,
                    badge_text.c_str(), nullptr);
        }

        Widget::draw(ctx);
    }

private:
    std::string m_caption;
    int m_icon;
    int m_indent;
    int m_badge;
    bool m_expandable;
    bool m_expanded;
    bool m_hovered;
    Widget *m_children_container;
    std::function<void(FolderItem *)> m_select_callback;
};

// ---------------------------------------------------------------------------
// SectionHeader — a small gray header (account name)
// ---------------------------------------------------------------------------
class SectionHeader : public Widget {
public:
    SectionHeader(Widget *parent, const std::string &title)
        : Widget(parent), m_title(title) {
        int row_h = (int)(font_size() * 1.8f);
        set_min_height(row_h);
        set_height(row_h);
    }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        int row_h = (int)(font_size() * 1.8f);
        return Vector2i(m_min_size.x() > 0 ? m_min_size.x() : 100, row_h);
    }

    virtual void draw(NVGcontext *ctx) override {
        float fs = (float)font_size();
        float x = (float)m_pos.x();
        float y = (float)m_pos.y();
        float h = (float)m_size.y();

        nvgFontSize(ctx, fs);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, m_theme->m_disabled_text_color);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + fs * 0.9f, y + h * 0.5f + 2.0f,
                m_title.c_str(), nullptr);

        Widget::draw(ctx);
    }

private:
    std::string m_title;
};

// ---------------------------------------------------------------------------
// FolderView — the sidebar widget, populated from the IMAP server
// ---------------------------------------------------------------------------
class FolderView : public Widget {
public:
    FolderView(Widget *parent, std::function<void(FolderItem *)> on_select)
        : Widget(parent), m_on_select(on_select)
    {
        set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill, 0, 0));

        m_scroll = new ScrollPanel(this);
        m_scroll->set_scroll_type(ScrollPanel::ScrollTypes::Vertical);
        m_scroll->set_grow_parent(true);

        m_container = new FolderContainer(m_scroll);
        m_container->set_layout(
            new BoxLayout(Orientation::Vertical, Alignment::Fill, 10, 0));
    }

    virtual void draw(NVGcontext *ctx) override {
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, m_theme->m_window_fill_unfocused);
        nvgFill(ctx);

        Widget::draw(ctx);
    }

    /* Rebuild the sidebar from the server's folder list.
     * `selected_name` (full IMAP name) is re-highlighted without firing
     * the select callback, so a LIST/refresh does not drop the current
     * folder selection. */
    void rebuild(const std::string &account,
                 const std::vector<MailFolder> &folders,
                 const std::string &selected_name = "") {
        g_selected_item = nullptr;
        while (!m_container->children().empty())
            m_container->remove_child_at(0);
        m_scroll->set_scroll(0.0f);

        auto *top_spacer = new Widget(m_container);
        top_spacer->set_min_height(6);
        top_spacer->set_height(6);

        new SectionHeader(m_container, account);

        for (const MailFolder &f : folders) {
            auto *item = new FolderItem(m_container, display_name(f.name),
                                        folder_icon(f.name), 0, f.unseen);
            item->set_select_callback(m_on_select);
            item->set_tooltip(f.name);
            if (!selected_name.empty() && f.name == selected_name)
                g_selected_item = item;
        }

        auto *bot_spacer = new Widget(m_container);
        bot_spacer->set_min_height(20);
        bot_spacer->set_height(20);

        screen()->perform_layout();
    }

private:
    ScrollPanel *m_scroll;
    Widget *m_container;
    std::function<void(FolderItem *)> m_on_select;

    /* Leaf name after the last hierarchy delimiter for display. */
    static std::string display_name(const std::string &name) {
        size_t p = name.find_last_of("/.");
        return p == std::string::npos ? name : name.substr(p + 1);
    }

    static int folder_icon(const std::string &name) {
        std::string leaf;
        size_t p = name.find_last_of("/.");
        leaf = (p == std::string::npos) ? name : name.substr(p + 1);
        for (char &c : leaf) c = (char)std::tolower((unsigned char)c);

        if (leaf == "inbox")                                 return FA_INBOX;
        if (leaf.find("sent") != std::string::npos)          return FA_PAPER_PLANE;
        if (leaf.find("draft") != std::string::npos)         return FA_EDIT;
        if (leaf.find("junk") != std::string::npos ||
            leaf.find("spam") != std::string::npos)          return FA_MAIL_BULK;
        if (leaf.find("trash") != std::string::npos ||
            leaf.find("deleted") != std::string::npos ||
            leaf.find("bin") != std::string::npos)           return FA_TRASH;
        if (leaf.find("archive") != std::string::npos)       return FA_ARCHIVE;
        if (leaf.find("star") != std::string::npos ||
            leaf.find("flag") != std::string::npos)          return FA_STAR;
        return FA_FOLDER;
    }
};

// ---------------------------------------------------------------------------
// EmailData — plain struct describing one message in the list
// ---------------------------------------------------------------------------
struct EmailData {
    int         seq = 0;           // IMAP message sequence number
    std::string sender;
    std::string subject;
    std::string preview;
    std::string date;
    bool        has_attachment = false;
    bool        seen = true;
};

// ---------------------------------------------------------------------------
// EmailListView — virtual-scroll list: O(visible) draw cost, no child widgets
// ---------------------------------------------------------------------------
class EmailListView : public Widget {
public:
    static constexpr float ROW_SCALE = 4.6f;
    static constexpr float SB_W      = 6.0f;
    static constexpr float SB_MARGIN = 3.0f;

    /* Status indicators drawn at the top-right of a row, just left of the
     * date, stacking leftward in table order.  A new flag (starred,
     * has_attachment, ...) only needs a bool on EmailData and a row here.
     * `show_when` is the flag value that displays the glyph, so "unread"
     * is `seen == false`.  Glyphs come from the monochrome FontAwesome
     * "icons" face, so the fill color tints them -- pick per-row colors
     * freely (unread green, starred amber, ...). */
    struct Indicator {
        bool EmailData::*flag;
        bool        show_when;
        const char *glyph;   // UTF-8 literal, drawn with the "icons" (FontAwesome) face
        Color       color;
    };
    inline static const Indicator kIndicators[] = {
        { &EmailData::seen, false, "\xEF\x84\x91" /* FA_CIRCLE */, Color(60, 180, 75, 255) },
    };
    static constexpr float IND_FONT_SCALE = 0.45f;  // of the sender font size
    static constexpr float IND_GAP        = 5.0f;

    EmailListView(Widget *parent,
                  std::function<void(int, const EmailData &)> on_select = nullptr)
        : Widget(parent), m_on_select(std::move(on_select)) {
        /* Virtual-scroll + inertia + spinner: repaint every frame instead of
           being baked into a retained parent display list. */
        set_live(true);
    }

    /* ---- geometry helpers ---- */
    float row_h()      const { return std::floor(font_size() * ROW_SCALE); }
    float total_h()    const { return row_h() * (float)m_emails.size(); }
    float max_scroll() const { return std::max(0.0f, total_h() - (float)m_size.y()); }

    /* ---- events ---- */
    std::function<void()> on_viewport_changed; // MailApp hooks scroll/paging prefetch
    void notify_viewport() { if (on_viewport_changed) on_viewport_changed(); }

    virtual bool mouse_motion_event(const Vector2i &p, const Vector2i &,
                                    int, int) override {
        int idx = idx_at(p.y());
        if (idx != m_hovered) { m_hovered = idx; screen()->redraw(); }
        return false;
    }

    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        if (!enter) { m_hovered = -1; screen()->redraw(); }
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button,
                                    bool down, int mods) override {
        if (button != GLFW_MOUSE_BUTTON_1) return false;
        if (down) {
            request_focus();   // grab keyboard focus on any click
            // Scrollbar thumb hit-test
            auto t = thumb_rect();
            if ((float)p.x() >= t[0] && (float)p.x() <= t[0] + t[2] &&
                (float)p.y() >= t[1] && (float)p.y() <= t[1] + t[3]) {
                m_vel            = 0.0f;   // kill inertia while dragging bar
                m_sb_drag        = true;
                m_sb_drag_start  = (float)p.y();
                m_sb_drag_origin = m_scroll;
                return true;
            }
            // Row hit-test
            int idx = idx_at(p.y());
            if (idx >= 0 && idx < (int)m_emails.size()) {
                m_selected = idx;
                if (m_on_select) m_on_select(idx, m_emails[idx]);
                screen()->redraw();
                return true;
            }
        } else {
            if (m_sb_drag) { m_sb_drag = false; screen()->redraw(); return true; }
        }
        return Widget::mouse_button_event(p, button, down, mods);
    }

    virtual bool mouse_drag_event(const Vector2i &p, const Vector2i &,
                                  int, int) override {
        if (!m_sb_drag) return false;
        float track = (float)m_size.y() - thumb_h();
        float delta = ((float)p.y() - m_sb_drag_start) /
                      (track > 0.0f ? track : 1.0f);
        m_scroll = std::clamp(m_sb_drag_origin + delta * max_scroll(),
                              0.0f, max_scroll());
        m_vel = 0.0f;
        screen()->redraw();
        notify_viewport();
        return true;
    }

    virtual bool scroll_event(const Vector2i &, const Vector2f &rel) override {
        m_vel = std::clamp(m_vel - rel.y() * row_h() * 4.0f, -3500.0f, 3500.0f);
        screen()->redraw();
        notify_viewport();
        return true;
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
        (void)scancode; (void)modifiers;
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return false;
        const int n = (int)m_emails.size();
        if (n == 0) return false;

        if (key == GLFW_KEY_DOWN || key == GLFW_KEY_UP) {
            int next = m_selected + (key == GLFW_KEY_DOWN ? 1 : -1);
            next = std::clamp(next, 0, n - 1);
            if (next != m_selected) {
                m_selected = next;
                scroll_to_show(m_selected);
                if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
                screen()->redraw();
            }
            notify_viewport();
            return true;
        }
        if (key == GLFW_KEY_PAGE_DOWN || key == GLFW_KEY_PAGE_UP) {
            float page = (float)m_size.y();
            m_vel = (key == GLFW_KEY_PAGE_DOWN ? 1.0f : -1.0f) * page * 6.0f;
            screen()->redraw();
            notify_viewport();
            return true;
        }
        if (key == GLFW_KEY_HOME) {
            m_scroll = 0.0f;  m_vel = 0.0f;
            screen()->redraw(); notify_viewport(); return true;
        }
        if (key == GLFW_KEY_END) {
            m_scroll = max_scroll();  m_vel = 0.0f;
            screen()->redraw(); notify_viewport(); return true;
        }
        if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
            if (m_selected >= 0 && m_selected < n)
                if (m_on_select) m_on_select(m_selected, m_emails[m_selected]);
            return true;
        }
        return false;
    }

    /* ---- draw (also drives the inertia animation) ---- */
    virtual void draw(NVGcontext *ctx) override {
        // ---- Inertia integration ----
        {
            double now = glfwGetTime();
            float  dt  = (m_last_t > 0.0)
                         ? std::min((float)(now - m_last_t), 0.05f)
                         : 0.0f;
            m_last_t = now;

            if (std::abs(m_vel) > 0.5f) {
                m_scroll += m_vel * dt;
                m_scroll  = std::clamp(m_scroll, 0.0f, max_scroll());
                // Kill velocity at boundaries so we don't jitter
                if (m_scroll <= 0.0f || m_scroll >= max_scroll())
                    m_vel = 0.0f;
                else {
                    m_vel *= std::exp(-8.0f * dt);   // decay; tau ≈ 125 ms
                    if (std::abs(m_vel) < 0.5f) m_vel = 0.0f;
                }
                screen()->redraw();   // keep animating until vel dies
            }
        }

        // Background
        nvgBeginPath(ctx);
        nvgRect(ctx, m_pos.x(), m_pos.y(), m_size.x(), m_size.y());
        nvgFillColor(ctx, m_dark ? Color(30, 31, 38, 255)
                                 : Color(228, 230, 238, 255));
        nvgFill(ctx);

        if (m_emails.empty()) {
            draw_scrollbar(ctx);
            return;
        }

        const float rh    = row_h();
        const int   first = std::max(0, (int)(m_scroll / rh));
        const int   last  = std::min((int)m_emails.size(),
                             (int)((m_scroll + (float)m_size.y()) / rh) + 2);

        // Clip rows to widget bounds
        nvgSave(ctx);
        nvgIntersectScissor(ctx, (float)m_pos.x(), (float)m_pos.y(),
                            (float)m_size.x(), (float)m_size.y());
        for (int i = first; i < last; ++i)
            draw_row(ctx, i,
                     (float)m_pos.x(),
                     (float)m_pos.y() + (float)i * rh - m_scroll,
                     (float)m_size.x());
        nvgRestore(ctx);

        if (m_loading_more) {
            draw_loading_strip(ctx);
            screen()->redraw();   // keep the spinner animating
        }

        // Notify when the bottom is reached (drives "load older" paging).
        // Skip while a page is already in flight, and never treat an empty
        // list as "at the bottom" — that used to queue FetchOlder for the
        // *previous* folder during a switch.
        if (m_on_hit_bottom && !m_loading_more &&
            m_scroll >= max_scroll() - 2.0f)
            m_on_hit_bottom();

        draw_scrollbar(ctx);  // drawn on top, no clip
    }

    /* Update the preview text for an already-listed row in place. */
    void update_preview(int seq, const std::string &preview) {
        for (auto &e : m_emails) {
            if (e.seq == seq && e.preview != preview) {
                e.preview = preview;
                screen()->redraw();
                break;
            }
        }
    }

    // viewport helpers — used by MailApp to prioritize prefetch
    std::pair<int,int> visible_range() const {
        if (m_emails.empty() || m_size.y() <= 0) return {0,0};
        int first = std::max(0, (int)(m_scroll / row_h()));
        int last  = std::min((int)m_emails.size(), (int)((m_scroll + (float)m_size.y()) / row_h()) + 2);
        return {first, last};
    }
    std::vector<int> visible_seqs(int pad = 6) const {
        auto [first,last] = visible_range();
        int a = std::max(0, first - pad);
        int b = std::min((int)m_emails.size(), last + pad);
        std::vector<int> out; out.reserve(b-a);
        for (int i=a;i<b;++i) out.push_back(m_emails[i].seq);
        return out;
    }
    const std::vector<EmailData>& emails() const { return m_emails; }

    /* ---- data ---- */
    void set_emails(std::vector<EmailData> emails) {
        m_emails   = std::move(emails);
        m_selected = -1;
        m_hovered  = -1;
        m_scroll   = 0.0f;
        m_vel      = 0.0f;
        screen()->redraw();
    }

    /* Append older rows (from a "load more" fetch) without resetting
       scroll or selection. */
    void append_emails(std::vector<EmailData> more) {
        m_emails.insert(m_emails.end(),
                        std::make_move_iterator(more.begin()),
                        std::make_move_iterator(more.end()));
        screen()->redraw();
    }

    /* Splice newly-arrived mail in at the top (background auto-check)
       without disturbing the user's current place: the rows already on
       screen stay on screen (scroll advances by exactly the inserted
       height) and the selected message stays selected (re-resolved by
       seq, since prepending shifts every existing row's index). */
    void prepend_emails(std::vector<EmailData> newer) {
        if (newer.empty()) return;
        int prev_selected_seq = selected_seq();
        float inserted_h = (float)newer.size() * row_h();
        m_emails.insert(m_emails.begin(),
                        std::make_move_iterator(newer.begin()),
                        std::make_move_iterator(newer.end()));
        m_scroll = std::clamp(m_scroll + inserted_h, 0.0f, max_scroll());
        if (prev_selected_seq >= 0) {
            m_selected = -1;
            for (int i = 0; i < (int)m_emails.size(); ++i)
                if (m_emails[i].seq == prev_selected_seq) { m_selected = i; break; }
        }
        m_hovered = -1;
        if (screen()) screen()->redraw();
    }

    /* Spinner strip at the bottom while older messages are fetched. */
    void set_loading_more(bool v) {
        if (m_loading_more == v) return;
        m_loading_more = v;
        screen()->redraw();
    }

    /* Called from draw() whenever the list is scrolled to the bottom. */
    void set_on_hit_bottom(std::function<void()> cb) {
        m_on_hit_bottom = std::move(cb);
    }

    /* ---- appearance ---- */
    void set_dark(bool dark) { m_dark = dark; screen()->redraw(); }

    int selected_index() const { return m_selected; }
    int selected_seq() const {
        if (m_selected >= 0 && m_selected < (int)m_emails.size())
            return m_emails[m_selected].seq;
        return -1;
    }
    const EmailData* selected_data() const {
        if (m_selected >= 0 && m_selected < (int)m_emails.size())
            return &m_emails[m_selected];
        return nullptr;
    }
    // Remove a row by seq, preserving scroll position and viewport.
    // Selects the message below the deleted one, or the last if at end.
    // IMAP sequence numbers shift after EXPUNGE, so remaining seqs > deleted
    // are decremented to stay in sync without a full refresh.
    /* Flip a row's read state in place (the server confirmed a \Seen flag).
     * Returns false when the seq is not in the current view. */
    bool set_seen(int seq, bool seen) {
        for (EmailData &e : m_emails)
            if (e.seq == seq) {
                if (e.seen == seen) return true;
                e.seen = seen;
                return true;
            }
        return false;
    }

    bool remove_seq(int seq) {
        for (int i = 0; i < (int)m_emails.size(); ++i) {
            if (m_emails[i].seq != seq) continue;
            m_emails.erase(m_emails.begin() + i);
            for (auto &e : m_emails)
                if (e.seq > seq) --e.seq;
            if (m_selected == i) {
                if (i < (int)m_emails.size())
                    m_selected = i;
                else
                    m_selected = (int)m_emails.size() - 1;
                m_hovered = -1;
            } else if (m_selected > i) {
                --m_selected;
                if (m_hovered > i) --m_hovered;
            } else if (m_hovered > i) {
                --m_hovered;
            }
            m_scroll = std::clamp(m_scroll, 0.0f, max_scroll());
            if (screen()) screen()->redraw();
            return true;
        }
        return false;
    }
    void clear_selection() { m_selected = -1; m_hovered = -1; if (screen()) screen()->redraw(); }

private:
    /* ---- scrollbar geometry ---- */
    float thumb_h() const {
        if (total_h() <= 0.0f) return (float)m_size.y();
        float ratio = std::min(1.0f, (float)m_size.y() / total_h());
        return std::max(28.0f, (float)m_size.y() * ratio);
    }

    // Returns {abs_x, abs_y, w, h} of the scrollbar thumb
    std::array<float, 4> thumb_rect() const {
        float th  = thumb_h();
        float trk = (float)m_size.y() - th;
        float ty  = (max_scroll() > 0.0f)
                    ? (m_scroll / max_scroll()) * trk : 0.0f;
        float tx  = (float)m_pos.x() + (float)m_size.x() - SB_W - SB_MARGIN;
        return { tx, (float)m_pos.y() + ty, SB_W, th };
    }

    void draw_scrollbar(NVGcontext *ctx) const {
        if (total_h() <= (float)m_size.y()) return;
        auto t = thumb_rect();
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, t[0], t[1] + 3.0f, t[2], t[3] - 6.0f, t[2] * 0.5f);
        nvgFillColor(ctx, m_sb_drag
            ? (m_dark ? Color(140, 148, 165, 230) : Color(100, 110, 130, 230))
            : (m_dark ? Color( 95, 100, 115, 180) : Color(150, 155, 165, 180)));
        nvgFill(ctx);
    }

    /* Overlay strip with a spinner + caption shown while older messages
       are being fetched. */
    void draw_loading_strip(NVGcontext *ctx) {
        const float h = 34.0f;
        const float x = (float)m_pos.x();
        const float w = (float)m_size.x();
        const float y = (float)m_pos.y() + (float)m_size.y() - h;

        nvgSave(ctx);
        nvgIntersectScissor(ctx, x, y, w, h);
        nvgBeginPath(ctx);
        nvgRect(ctx, x, y, w, h);
        nvgFillColor(ctx, m_dark ? Color( 30,  31,  38, 235)
                                 : Color(228, 230, 238, 235));
        nvgFill(ctx);

        const Color fg = m_dark ? Color(180, 182, 196, 255)
                                : Color( 90,  90, 105, 255);

        // Spinner arc
        const float r  = 8.0f;
        const float cy = y + h * 0.5f;
        float tb[4] = {};
        nvgFontSize(ctx, 14.0f);
        nvgFontFace(ctx, "sans");
        nvgTextBounds(ctx, 0, 0, "Loading older messages...", nullptr, tb);
        const float text_w = tb[2] - tb[0];
        const float cx = x + (w - text_w - r * 2.0f - 10.0f) * 0.5f;
        const float a0 = (float)glfwGetTime() * 6.0f;
        nvgBeginPath(ctx);
        nvgArc(ctx, cx, cy, r, a0, a0 + NVG_PI * 1.5f, NVG_CW);
        nvgStrokeColor(ctx, fg);
        nvgStrokeWidth(ctx, 2.5f);
        nvgStroke(ctx);

        nvgFillColor(ctx, fg);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, cx + r + 10.0f, cy, "Loading older messages...", nullptr);
        nvgRestore(ctx);
    }

    int idx_at(int abs_y) const {
        float iy  = (float)(abs_y - m_pos.y()) + m_scroll;
        int   idx = (int)(iy / row_h());
        return (idx >= 0 && idx < (int)m_emails.size()) ? idx : -1;
    }

    /* ---- row drawing ---- */
    void draw_row(NVGcontext *ctx, int idx,
                  float x, float y, float w) const {
        const bool  sel = (idx == m_selected);
        const bool  hov = (idx == m_hovered) && !sel;
        const auto &d   = m_emails[idx];

        const float fs         = (float)font_size();
        const float h          = row_h();
        const float padx       = 10.0f;
        const float pady       = 6.0f;
        const float scroll_rsv = SB_W + SB_MARGIN;   // rows touch the scrollbar
        const float cw         = w - scroll_rsv;
        const float rounding   = 10.0f;

        const float sender_fs  = fs * 0.85f;
        const float date_fs    = fs * 0.60f;
        const float subject_fs = fs * 0.72f;
        const float preview_fs = fs * 0.63f + 2.0f;
        const float line_gap   = fs * 0.12f;

        const float y1 = y + pady + sender_fs * 0.85f;
        const float y2 = y1 + sender_fs  + line_gap;
        const float y3 = y2 + subject_fs + line_gap;
        const float y4 = y3 + preview_fs * 1.10f;

        // Background
        if (sel) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 3, y + 2, cw - 3, h - 4, rounding);
            nvgFillColor(ctx, Color(58, 90, 210, 255));
            nvgFill(ctx);
        } else if (hov) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x + 3, y + 2, cw - 3, h - 4, rounding);
            nvgFillColor(ctx, m_dark ? Color( 58,  60,  72, 255)
                                     : Color(210, 214, 222, 255));
            nvgFill(ctx);
        }

        Color name_col = sel ? Color(255,255,255,255)
                       : m_dark ? (d.seen ? Color(232,232,238,255)
                                          : Color(110,160,255,255))
                                : (d.seen ? Color( 15, 15, 20,255)
                                          : Color( 10, 80,220,255)); // unread: accent
        Color date_col = sel ? Color(200,218,255,255)
                       : m_dark ? Color(145,147,160,255) : Color(120,120,135,255);
        Color subj_col = sel ? Color(220,232,255,255)
                       : m_dark ? Color(205,206,216,255) : Color( 35, 35, 45,255);
        Color prev_col = sel ? Color(185,208,255,255)
                       : m_dark ? Color(160,162,175,255) : Color( 95, 95,110,255);

        // Date (right-aligned)
        nvgFontSize(ctx, date_fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, date_col);
        nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + cw - padx, y1, d.date.c_str(), nullptr);

        float db[4] = {};
        nvgTextBounds(ctx, 0, 0, d.date.c_str(), nullptr, db);
        const float date_w = (db[2] - db[0]) + padx * 1.8f;

        /* Indicators (unread dot, future flags): right-aligned, starting
         * just left of the date and stacking leftward.  `ind_w` widens the
         * sender clip below so the name never runs under the dots. */
        float ind_w = 0.0f;
        {
            const float ind_fs = sender_fs * IND_FONT_SCALE;
            float ix = x + cw - padx - (db[2] - db[0]) - IND_GAP;
            nvgFontSize(ctx, ind_fs);
            nvgFontFace(ctx, "icons");
            nvgTextAlign(ctx, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            for (const Indicator &ind : kIndicators) {
                if (d.*(ind.flag) != ind.show_when) continue;
                nvgFillColor(ctx, ind.color);
                nvgText(ctx, ix, y1, ind.glyph, nullptr);
                float gb[4] = {};
                nvgTextBounds(ctx, 0, 0, ind.glyph, nullptr, gb);
                const float gw = (gb[2] - gb[0]) + IND_GAP;
                ix    -= gw;
                ind_w += gw;
            }
            if (ind_w > 0.0f) ind_w += IND_GAP;   // space before the sender
        }

        // Sender name (bold, clipped against date + indicators)
        nvgSave(ctx);
        nvgIntersectScissor(ctx, x + padx, y, cw - date_w - ind_w - padx, h);
        nvgFontSize(ctx, sender_fs);
        nvgFontFace(ctx, "sans-bold");
        nvgFillColor(ctx, name_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(ctx, x + padx, y1, d.sender.c_str(), nullptr);
        nvgRestore(ctx);

        // Subject (small bold, clipped)
        {
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x + padx, y, cw - padx - 3.0f, h);
            nvgFontSize(ctx, subject_fs);
            nvgFontFace(ctx, d.seen ? "sans" : "sans-bold");
            nvgFillColor(ctx, subj_col);
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, x + padx, y2, d.subject.c_str(), nullptr);
            nvgRestore(ctx);
        }

        // Preview body — 2 lines, scissor-clipped
        nvgFontSize(ctx, preview_fs);
        nvgFontFace(ctx, "sans");
        nvgFillColor(ctx, prev_col);
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        {
            float clip_h = (y4 - y3) + preview_fs * 1.35f;
            nvgSave(ctx);
            nvgIntersectScissor(ctx, x + padx, y3 - preview_fs * 0.6f,
                                cw - padx - 3.0f, clip_h + preview_fs * 0.3f);
            nvgTextBox(ctx, x + padx, y3 - preview_fs * 0.55f,
                       cw - padx - 3.0f, d.preview.c_str(), nullptr);
            nvgRestore(ctx);
        }

        // Bottom separator (non-selected rows only)
        if (!sel) {
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, x + padx,      y + h - 0.5f);
            nvgLineTo(ctx, x + cw, y + h - 0.5f);
            nvgStrokeColor(ctx, m_dark ? Color( 62,  64,  76, 255)
                                       : Color(195, 198, 208, 255));
            nvgStrokeWidth(ctx, 1.0f);
            nvgStroke(ctx);
        }
    }

    /* ---- scroll-to-show (used by keyboard nav) ---- */
    void scroll_to_show(int idx) {
        const float rh  = row_h();
        const float top = (float)idx * rh;
        const float bot = top + rh;
        m_vel = 0.0f;
        if (top < m_scroll)
            m_scroll = top;
        else if (bot > m_scroll + (float)m_size.y())
            m_scroll = bot - (float)m_size.y();
        m_scroll = std::clamp(m_scroll, 0.0f, max_scroll());
    }

    /* ---- state ---- */
    std::vector<EmailData> m_emails;
    int    m_selected = -1;
    int    m_hovered  = -1;
    float  m_scroll   = 0.0f;
    float  m_vel      = 0.0f;   // inertia velocity (px/s)
    double m_last_t   = 0.0;    // timestamp of last draw (for dt)

    bool  m_sb_drag        = false;
    float m_sb_drag_start  = 0.0f;
    float m_sb_drag_origin = 0.0f;
    bool  m_dark           = false;
    bool  m_loading_more   = false;   // spinner strip while paging older mail

    std::function<void(int, const EmailData &)> m_on_select;
    std::function<void()> m_on_hit_bottom;
};

// ---------------------------------------------------------------------------
// parse_markdown — minimal Markdown → Document converter
// Supports: # / ## / ### headers, **bold**, *italic*, `code`, ```fenced```,
// "> " blockquotes, "- " / "* " bullet lists, \escapes.
// Blank lines start a new paragraph.
// ---------------------------------------------------------------------------
static void parse_markdown(Document &doc, const std::string &md,
                           NVGcolor text_color = nvgRGBA(20, 20, 25, 255),
                           float base_size = 16.0f)
{
    doc.paragraphs.clear();

    Style normal;  normal.fontSize = base_size;          normal.fgColor = text_color;
    Style code_s   = normal; code_s.monospace = true;
                             code_s.fontSize  = base_size * 0.875f;
                             code_s.bgColor   = nvgRGBA(220, 220, 228, 255);
    Style h1 = normal; h1.fontSize = base_size * 1.625f; h1.bold = true;
    Style h2 = normal; h2.fontSize = base_size * 1.25f;  h2.bold = true;
    Style h3 = normal; h3.fontSize = base_size * 1.0625f; h3.bold = true;

    // Inline-span parser: **bold**, *italic*, `code`, \escapes, plain text.
    // `base` is the paragraph's base style (blockquote paragraphs pass an
    // italic variant).
    auto append_inline = [&](Paragraph *p, const std::string &text,
                             const Style &base) {
        Style b = base; b.bold      = true;
        Style i = base; i.italic    = true;
        Style c = base; c.monospace = true;
                        c.fontSize  = base.fontSize * 0.875f;
                        c.bgColor   = code_s.bgColor;
        size_t i_ = 0;
        while (i_ < text.size()) {
            if (text[i_] == '\\' && i_ + 1 < text.size() &&
                (text[i_+1] == '*' || text[i_+1] == '`' ||
                 text[i_+1] == '\\' || text[i_+1] == '#' ||
                 text[i_+1] == '-' || text[i_+1] == '>')) {
                p->addText(std::string(1, text[i_+1]), base);
                i_ += 2; continue;
            }
            if (text.compare(i_, 3, "<u>") == 0) {
                size_t s = i_ + 3, e = text.find("</u>", s);
                if (e != std::string::npos) {
                    Style u = base; u.underline = true;
                    if (e > s) p->addText(text.substr(s, e - s), u);
                    i_ = e + 4; continue;
                }
            }
            if (i_ + 2 < text.size() && text[i_] == '*' &&
                text[i_+1] == '*' && text[i_+2] == '*') {
                size_t s = i_ + 3, e = text.find("***", s);
                if (e != std::string::npos) {
                    Style bi = base; bi.bold = true; bi.italic = true;
                    if (e > s) p->addText(text.substr(s, e - s), bi);
                    i_ = e + 3; continue;
                }
            }
            if (i_ + 1 < text.size() && text[i_] == '*' && text[i_+1] == '*') {
                size_t s = i_ + 2, e = text.find("**", s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), b);
                    i_ = e + 2; continue;
                }
            } else if (text[i_] == '*' && (i_ == 0 || text[i_-1] != '*')) {
                size_t s = i_ + 1, e = text.find('*', s);
                if (e != std::string::npos && e > s) {
                    p->addText(text.substr(s, e - s), i);
                    i_ = e + 1; continue;
                }
            } else if (text[i_] == '`') {
                size_t s = i_ + 1, e = text.find('`', s);
                if (e != std::string::npos) {
                    if (e > s) p->addText(text.substr(s, e - s), c);
                    i_ = e + 1; continue;
                }
            }
            size_t s = i_;
            while (i_ < text.size() && text[i_] != '*' && text[i_] != '`'
                   && text[i_] != '\\') ++i_;
            if (i_ > s) p->addText(text.substr(s, i_ - s), base);
            else        ++i_;
        }
    };

    std::istringstream iss(md);
    std::string line;
    Paragraph *cur = nullptr;
    bool inCode = false;
    std::string codeBuf;

    while (std::getline(iss, line)) {
        bool inline_break = (!line.empty() && line.back() == '\\')
                         || (line.size() >= 2
                             && line[line.size()-1] == ' '
                             && line[line.size()-2] == ' ');

        // strip trailing whitespace (and the backslash if present)
        while (!line.empty() && (std::isspace((unsigned char)line.back())
                                 || line.back() == '\\'))
            line.pop_back();

        if (line.empty()) {
            if (inCode) codeBuf += '\n';
            else        cur = nullptr;
            continue;
        }

        // fenced code block
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (inCode) {
                if (!codeBuf.empty()) doc.addParagraph()->addText(codeBuf, code_s);
                codeBuf.clear(); inCode = false;
            } else {
                inCode = true; codeBuf.clear();
            }
            cur = nullptr; continue;
        }
        if (inCode) { codeBuf += line + "\n"; continue; }

        // horizontal rule  --- / *** / ___  (3+ repeated chars, nothing else)
        if (!inCode && line.size() >= 3) {
            char c = line[0];
            if (c == '-' || c == '*' || c == '_') {
                bool all_same = true;
                for (char ch : line) if (ch != c) { all_same = false; break; }
                if (all_same) {
                    auto *p = doc.addParagraph();
                    p->isRule = true;
                    cur = nullptr;
                    continue;
                }
            }
        }

        // blockquote: "> text" — consecutive quote lines join into one
        // indented, italic paragraph (soft-wrapped like normal text).
        if (line[0] == '>' && (line.size() == 1 || line[1] == ' ')) {
            Style qs = normal; qs.italic = true;
            std::string content = line.size() > 2 ? line.substr(2) : "";
            if (!cur || cur->leftIndent <= 0.0f) {
                cur = doc.addParagraph();
                cur->leftIndent = 16.0f;
            } else {
                cur->addText(" ", qs);
            }
            append_inline(cur, content, qs);
            continue;
        }

        // headings
        if (line[0] == '#') {
            size_t lvl = 0;
            while (lvl < line.size() && line[lvl] == '#') ++lvl;
            if (lvl < line.size() && std::isspace((unsigned char)line[lvl])) {
                const Style &hs = (lvl == 1) ? h1 : (lvl == 2) ? h2 : h3;
                doc.addParagraph()->addText(line.substr(lvl + 1), hs);
                cur = nullptr; continue;
            }
        }

        // unordered list item: "- text" / "* text", optionally indented by
        // spaces for sub-levels (2 spaces per level).  One paragraph per
        // item, indented with a drawn bullet marker.
        {
            size_t sp = 0;
            while (sp < line.size() && line[sp] == ' ') ++sp;
            if (line.size() >= sp + 2 &&
                (line[sp] == '-' || line[sp] == '*') && line[sp + 1] == ' ') {
                Paragraph *p = doc.addParagraph();
                p->isBullet   = true;
                p->leftIndent = 16.0f * (float)(sp / 2 + 1);
                append_inline(p, line.substr(sp + 2), normal);
                cur = nullptr; continue;
            }
        }

        if (!cur || cur->leftIndent > 0.0f) cur = doc.addParagraph();
        else      cur->addText(" ", normal);  // soft-wrap join
        append_inline(cur, line, normal);
        if (inline_break)
            cur->addText("\n", normal);  // tight in-paragraph line break
    }

    if (inCode && !codeBuf.empty())
        doc.addParagraph()->addText(codeBuf, code_s);
    if (doc.paragraphs.empty())
        doc.addParagraph();
}

// ---------------------------------------------------------------------------
// document_to_markdown — serialize a Document back to Markdown.  Inverse of
// parse_markdown; used at send time for markup=markdown messages.
// ---------------------------------------------------------------------------
static std::string md_escape_plain(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '*' || c == '`') out += '\\';
        out += c;
    }
    return out;
}

static std::string document_to_markdown(const Document &doc) {
    auto same_style = [](const Style &a, const Style &b) {
        return a.fontSize == b.fontSize &&
               a.bold == b.bold && a.italic == b.italic &&
               a.underline == b.underline && a.monospace == b.monospace &&
               std::memcmp(&a.fgColor, &b.fgColor, sizeof(NVGcolor)) == 0 &&
               std::memcmp(&a.bgColor, &b.bgColor, sizeof(NVGcolor)) == 0;
    };
    std::string out;
    bool in_code = false;   // inside a fenced ``` block
    for (const auto &para : doc.paragraphs) {
        /* Fenced code block: a paragraph whose runs are all monospace.
         * Consecutive such paragraphs join into one block; a blank code
         * line (empty monospace run) becomes a blank line inside the
         * fence instead of splitting it. */
        bool is_code = !para->isRule && !para->runs.empty();
        for (const Text &r : para->runs)
            if (!r.style.monospace) { is_code = false; break; }
        if (is_code) {
            if (!in_code) { out += "```\n"; in_code = true; }
            std::string t = para->plain_text();
            while (!t.empty() && t.back() == '\n') t.pop_back();
            out += t;
            out += '\n';
            continue;
        }
        if (in_code) { out += "```\n"; in_code = false; }

        std::string line;
        int level = 0;
        if (para->isRule) {
            line = "---";
        } else {
            /* Heading detection mirrors parse_markdown's size ratios at
             * base 16pt (h1 26, h2 20, h3 17). */
            if (!para->runs.empty() && para->runs[0].style.bold) {
                float fs = para->runs[0].style.fontSize;
                if (fs >= 24.0f)       level = 1;
                else if (fs >= 18.5f)  level = 2;
                else if (fs >= 16.75f) level = 3;
            }
            if (level) line = std::string((size_t)level, '#') + " ";

            /* Merge adjacent same-style runs so emphasis markers wrap
             * the longest possible span ("*a b*" not "*a** **b*"). */
            std::vector<Text> merged;
            for (const Text &r : para->runs) {
                if (r.content.empty()) continue;
                if (!merged.empty() &&
                    same_style(merged.back().style, r.style))
                    merged.back().content += r.content;
                else
                    merged.push_back(r);
            }

            for (const Text &r : merged) {
                const std::string &c = r.content;
                if (c.empty()) continue;
                /* Multiline monospace run -> fenced code block. */
                if (r.style.monospace && c.find('\n') != std::string::npos) {
                    line += "```\n" + c + "```";
                    continue;
                }
                bool b = r.style.bold && !level;
                bool i = r.style.italic;
                bool u = r.style.underline;
                bool m = r.style.monospace;
                std::string open, close;
                if (m) { open += '`';    close = "`"    + close; }
                if (b) { open += "**";   close = "**"   + close; }
                if (i) { open += '*';    close = "*"    + close; }
                if (u) { open += "<u>";  close = "</u>" + close; }
                line += open + (m ? c : md_escape_plain(c)) + close;
            }
        }

        /* Escape leading markers that would otherwise be re-parsed as
         * structure (real headings/rules/quotes already handled above). */
        if (!level && !para->isRule && para->leftIndent <= 0.0f &&
            !line.empty() &&
            (line[0] == '#' || line[0] == '>' ||
             (line.size() > 1 && line[0] == '-' && line[1] == ' ')))
            line = '\\' + line;

        /* Blockquote: prefix every line with "> ".  (Bullet items carry
         * leftIndent too, but serialize with "- " instead.) */
        if (para->leftIndent > 0.0f && !para->isBullet) {
            std::string quoted;
            size_t pos = 0;
            for (;;) {
                size_t nl = line.find('\n', pos);
                quoted += "> ";
                if (nl == std::string::npos) {
                    quoted += line.substr(pos);
                    break;
                }
                quoted += line.substr(pos, nl - pos);
                quoted += '\n';
                pos = nl + 1;
            }
            line = quoted;
        }

        /* Bullet list item: "- ", indented 2 spaces per sub-level
         * (level is derived from leftIndent: 16px per level). */
        if (para->isBullet && !para->isRule) {
            int lvl = (int)(para->leftIndent / 16.0f + 0.5f) - 1;
            if (lvl < 0) lvl = 0;
            line = std::string((size_t)lvl * 2, ' ') + "- " + line;
        }

        out += line;
        out += '\n';
    }
    if (in_code) out += "```\n";
    return out;
}

// ---------------------------------------------------------------------------
// document_to_html — serialize a Document to an HTML email body.  Mirrors
// document_to_markdown's structure detection (headings by font size,
// all-monospace paragraphs -> <pre>, isBullet -> <ul>/<li> with nesting by
// indent level, leftIndent -> <blockquote>, isRule -> <hr>).
// ---------------------------------------------------------------------------
static std::string html_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
    return out;
}

/* Inline runs -> HTML spans.  In-paragraph newlines become <br>.
 * `in_heading` suppresses <strong> (headings are already bold). */
static std::string html_inline(const Paragraph &para, bool in_heading = false) {
    std::string out;
    for (const Text &r : para.runs) {
        if (r.content.empty()) continue;
        std::string c = html_escape(r.content);
        size_t pos = 0;
        while ((pos = c.find('\n', pos)) != std::string::npos) {
            c.replace(pos, 1, "<br>");
            pos += 4;
        }
        std::string open, close;
        if (r.style.monospace) { open += "<code>";    close = "</code>"    + close; }
        if (r.style.bold && !in_heading) { open += "<strong>";  close = "</strong>"  + close; }
        if (r.style.italic)    { open += "<em>";      close = "</em>"      + close; }
        if (r.style.underline) { open += "<u>";       close = "</u>"       + close; }
        out += open + c + close;
    }
    return out;
}

static std::string document_to_html(const Document &doc) {
    std::string out = "<!DOCTYPE html>\n<html><body>\n";
    int  list_depth = 0;   // number of open <ul> elements
    bool in_pre     = false;

    auto close_lists = [&]() {
        while (list_depth > 0) { out += "</ul>\n"; --list_depth; }
    };
    auto close_pre = [&]() {
        if (in_pre) { out += "</code></pre>\n"; in_pre = false; }
    };

    for (const auto &para : doc.paragraphs) {
        /* Code block: all-monospace paragraph (blank code lines included).
         * Consecutive ones share a single <pre>. */
        bool is_code = !para->isRule && !para->isBullet &&
                       !para->runs.empty();
        for (const Text &r : para->runs)
            if (!r.style.monospace) { is_code = false; break; }
        if (is_code) {
            close_lists();
            if (!in_pre) { out += "<pre><code>"; in_pre = true; }
            else         out += '\n';
            std::string t = para->plain_text();
            while (!t.empty() && t.back() == '\n') t.pop_back();
            out += html_escape(t);
            continue;
        }
        close_pre();

        /* Bullet list item, nesting by indent level (16px per level). */
        if (para->isBullet) {
            int lvl = (int)(para->leftIndent / 16.0f + 0.5f) - 1;
            if (lvl < 0) lvl = 0;
            int target = lvl + 1;
            while (list_depth < target) { out += "<ul>\n";  ++list_depth; }
            while (list_depth > target) { out += "</ul>\n"; --list_depth; }
            out += "<li>" + html_inline(*para) + "</li>\n";
            continue;
        }
        close_lists();

        if (para->isRule) { out += "<hr>\n"; continue; }

        /* Heading detection: same size ratios as document_to_markdown. */
        int level = 0;
        if (!para->runs.empty() && para->runs[0].style.bold) {
            float fs = para->runs[0].style.fontSize;
            if (fs >= 24.0f)       level = 1;
            else if (fs >= 18.5f)  level = 2;
            else if (fs >= 16.75f) level = 3;
        }

        std::string content = html_inline(*para, level > 0);
        if (content.empty()) continue;   // blank paragraphs add nothing

        if (level) {
            out += "<h" + std::to_string(level) + ">" + content +
                   "</h" + std::to_string(level) + ">\n";
        } else if (para->leftIndent > 0.0f) {
            out += "<blockquote><p>" + content + "</p></blockquote>\n";
        } else {
            out += "<p>" + content + "</p>\n";
        }
    }
    close_pre();
    close_lists();
    out += "</body></html>\n";
    return out;
}


/* Render a fetched message into a reading-pane Document.  Used for
 * Markdown and plain-text bodies; text/html bodies go straight to
 * HtmlDocument::set_html (see render_current). */
/* ---------------------------------------------------------------------------
 * Message header card.
 *
 * The header is chrome, not content, but it shares a document with the mail's
 * own HTML — which routinely paints its own background.  Theme-coloured text
 * then lands on whatever the sender chose (light ink on white in dark mode),
 * so the card carries its own parchment palette and stays legible in both
 * modes and against any message background.  Fixed colours on purpose: these
 * do NOT follow the light/dark theme.
 * ------------------------------------------------------------------------ */
namespace parchment {
    static const char *kPaper   = "#f4ecd8";  // aged paper
    static const char *kEdge    = "#ddd0b0";  // slightly darker rule/border
    static const char *kInk     = "#2b2418";  // primary text
    static const char *kInkBold = "#1f1a12";  // subject
    /* Label contrast on kPaper is 5.09:1 — WCAG AA for body text.  The
     * lighter brown this replaced measured 3.76:1 and failed. */
    static const char *kLabel   = "#756040";  // "From:" / "To:" / "Date:"
    static const char *kMeta    = "#6b5d45";  // date value
}

/* Private scheme for the header's name links.  is_allowed_url() only ever
 * lets http/https/mailto reach the browser, so an unhandled click here is
 * inert rather than dangerous. */
static const char *kAddrScheme = "x-nmail-addr:";

/* Render an address header.  Entries that carry a display name become links
 * that swap to the bare address when clicked; entries that are already just
 * an address have nothing to reveal and stay plain text. */
static std::string address_row_html(const std::string &raw,
                                    const std::set<std::string> &expanded) {
    using namespace parchment;
    std::vector<MailAddress> addrs = parse_address_list(raw);
    if (addrs.empty())                       // unparseable: show it verbatim
        return "<span style=\"color:" + std::string(kInk) + "\">" +
               html_escape(raw) + "</span>";

    std::string out;
    for (size_t i = 0; i < addrs.size(); ++i) {
        if (i) out += ", ";
        const MailAddress &a = addrs[i];
        std::string low = a.address;
        for (char &c : low) c = (char)std::tolower((unsigned char)c);

        if (a.name.empty()) {
            out += "<span style=\"color:" + std::string(kInk) + "\">" +
                   html_escape(a.address) + "</span>";
            continue;
        }
        const bool show_addr = expanded.count(low) > 0;
        out += "<a href=\"" + std::string(kAddrScheme) + html_escape(a.address) +
               "\" style=\"color:" + kInk + "\">" +
               html_escape(show_addr ? a.address : a.name) + "</a>";
    }
    return out;
}

/* One <div> card: subject, then the From/To/Date rows. */
static std::string header_html(const MailMessage &msg,
                               const std::set<std::string> &expanded) {
    using namespace parchment;
    std::string h;
    h += std::string("<div style=\"background-color:") + kPaper +
         ";border:1px solid " + kEdge +
         ";border-radius:8px;padding:14px 16px;color:" + kInk + "\">";

    h += std::string("<p style=\"font-size:24px;color:") + kInkBold +
         "\"><b>" + html_escape(msg.subject) + "</b></p>";

    auto label_cell = [&](const char *label) {
        return std::string("<p style=\"font-size:15px\"><b style=\"color:") +
               kLabel + "\">" + label + " </b>";
    };

    h += label_cell("From:") + address_row_html(msg.from_addr.empty()
                                                ? msg.from
                                                : msg.from + " <" +
                                                  msg.from_addr + ">",
                                                expanded) + "</p>";
    if (!msg.to.empty())
        h += label_cell("To:") + address_row_html(msg.to, expanded) + "</p>";
    if (!msg.date.empty())
        h += label_cell("Date:") +
             "<span style=\"color:" + std::string(kMeta) + "\">" +
             html_escape(msg.date) + "</span></p>";

    h += "</div>";
    /* Vertical margin is not supported by the renderer, so separate the card
     * from the message body with an explicit spacer. */
    h += "<div style=\"height:12px\"></div>";
    return h;
}

static void render_message(Document &doc, const MailMessage &msg,
                           NVGcolor text_color, NVGcolor meta_color) {
    doc.paragraphs.clear();

    Style normal; normal.fontSize = 17.0f; normal.fgColor = text_color;
    Style bold   = normal; bold.bold = true;
    Style subj   = normal; subj.fontSize = 24.0f; subj.bold = true;
    Style meta   = normal; meta.fgColor = meta_color;

    doc.addParagraph()->addText(msg.subject, subj);

    auto *pf = doc.addParagraph();
    pf->addText("From: ", bold);
    pf->addText(msg.from, normal);

    if (!msg.to.empty()) {
        auto *pt = doc.addParagraph();
        pt->addText("To: ", bold);
        pt->addText(msg.to, normal);
    }
    if (!msg.date.empty()) {
        auto *pd = doc.addParagraph();
        pd->addText("Date: ", bold);
        pd->addText(msg.date, meta);
    }

    auto *rule = doc.addParagraph();
    rule->isRule = true;

    if (msg.body_markdown) {
        /* MailMate-style markup=markdown (or text/markdown): render the
         * plain body as Markdown.  parse_markdown() clears its target, so
         * parse into a scratch document and move the paragraphs over. */
        Document tmp;
        parse_markdown(tmp, msg.body, text_color, 17.0f);
        for (auto &p : tmp.paragraphs)
            doc.paragraphs.push_back(std::move(p));
    } else {
        // Plain paragraphs for the body (no markup interpretation).
        std::istringstream iss(msg.body);
        std::string line;
        Paragraph *cur = nullptr;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) { cur = nullptr; continue; }
            if (!cur) cur = doc.addParagraph();
            else      cur->addText(" ", normal);
            cur->addText(line, normal);
        }
    }
    if (doc.paragraphs.empty())
        doc.addParagraph();
}

// ---------------------------------------------------------------------------
// Attachments: Mail-style document chips hosted in the HtmlDocument tree.
// ---------------------------------------------------------------------------

static std::string att_lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool html_uses_cid(const std::string &html, const std::string &cid) {
    if (html.empty() || cid.empty()) return false;
    const std::string needle = att_lower("cid:" + cid);
    const std::string hay = att_lower(html);
    size_t p = hay.find(needle);
    while (p != std::string::npos) {
        size_t e = p + needle.size();
        char n = e < hay.size() ? hay[e] : '\0';
        if (!n || n == '"' || n == '\'' || n == ' ' || n == '>' ||
            n == '&' || n == '?' || n == '#')
            return true;
        p = hay.find(needle, p + 1);
    }
    return false;
}

static std::vector<const MailAttachment *>
visible_attachments(const MailMessage &msg) {
    std::vector<const MailAttachment *> out;
    out.reserve(msg.attachments.size());
    for (const MailAttachment &a : msg.attachments) {
        if (a.data.empty()) continue;
        if (!a.cid.empty() && html_uses_cid(msg.html, a.cid))
            continue;
        out.push_back(&a);
    }
    return out;
}

static std::string mime_ext_guess(const std::string &mime) {
    if (mime == "application/pdf") return "pdf";
    if (mime == "image/jpeg" || mime == "image/jpg") return "jpg";
    if (mime == "image/png")  return "png";
    if (mime == "image/gif")  return "gif";
    if (mime == "image/webp") return "webp";
    if (mime == "image/tiff") return "tiff";
    if (mime == "text/plain") return "txt";
    if (mime == "text/html")  return "html";
    if (mime == "text/csv")   return "csv";
    if (mime == "text/calendar") return "ics";
    if (mime == "application/rtf" || mime == "text/rtf") return "rtf";
    if (mime == "application/zip") return "zip";
    if (mime == "application/msword") return "doc";
    if (mime.find("wordprocessingml") != std::string::npos) return "docx";
    if (mime.find("spreadsheetml") != std::string::npos) return "xlsx";
    if (mime.find("presentationml") != std::string::npos) return "pptx";
    if (mime == "application/vnd.ms-excel") return "xls";
    if (mime == "application/vnd.ms-powerpoint") return "ppt";
    if (mime.find("opendocument.text") != std::string::npos) return "odt";
    if (mime.find("opendocument.spreadsheet") != std::string::npos) return "ods";
    if (mime.find("opendocument.presentation") != std::string::npos) return "odp";
    if (mime.rfind("audio/", 0) == 0) return "audio";
    if (mime.rfind("video/", 0) == 0) return "video";
    return "";
}

static std::string attachment_ext(const MailAttachment &a) {
    std::string fn = a.filename;
    size_t dot = fn.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < fn.size() &&
        fn.find('/', dot) == std::string::npos &&
        fn.find('\\', dot) == std::string::npos) {
        std::string e = att_lower(fn.substr(dot + 1));
        if (e.size() <= 8 && e.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos)
            return e;
    }
    return mime_ext_guess(a.mime);
}

static std::string format_bytes(size_t n) {
    char buf[32];
    if (n < 1024)
        std::snprintf(buf, sizeof(buf), "%zu bytes", n);
    else if (n < 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), n < 10 * 1024 ? "%.1f KB" : "%.0f KB",
                      n / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0 * 1024.0));
    return buf;
}

static std::string sanitize_filename(const std::string &raw, const std::string &ext) {
    std::string fn = raw;
    size_t slash = fn.find_last_of("/\\");
    if (slash != std::string::npos) fn = fn.substr(slash + 1);
    std::string out;
    out.reserve(fn.size());
    for (unsigned char c : fn) {
        if (c < 32 || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || c == '\\' || c == '/')
            out += '_';
        else
            out += (char)c;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' '))
        out.erase(out.begin());
    if (out.size() > 120) out.resize(120);
    if (out.empty()) out = "attachment";
    if (!ext.empty()) {
        std::string have;
        size_t dot = out.find_last_of('.');
        if (dot != std::string::npos)
            have = att_lower(out.substr(dot + 1));
        if (have != ext)
            out += "." + ext;
    }
    return out;
}

static bool ext_in(const std::string &ext, std::initializer_list<const char *> list) {
    for (const char *s : list)
        if (ext == s) return true;
    return false;
}

static bool is_exec_ext(const std::string &ext) {
    return ext_in(ext, {
        "exe", "com", "bat", "cmd", "msi", "scr", "pif", "dll", "so",
        "dylib", "app", "bin", "run", "out", "elf", "sh", "bash", "zsh",
        "ps1", "py", "rb", "pl", "js", "jsx", "vbs", "jse", "wsf", "php",
        "lua", "desktop", "lnk", "url", "jar", "apk", "command", "cgi"
    });
}

static bool is_archive_ext(const std::string &ext) {
    return ext_in(ext, {
        "zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "cab",
        "iso", "dmg", "pkg", "zst", "lz", "lzma"
    });
}

static bool is_open_allowlisted(const std::string &ext) {
    return ext_in(ext, {
        "pdf", "txt", "rtf", "csv", "html", "htm",
        "png", "jpg", "jpeg", "gif", "webp", "tif", "tiff", "bmp", "heic", "heif",
        "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods", "odp",
        "mp3", "mp4", "wav", "aac", "m4a", "mov", "webm", "ogg",
        "json", "xml", "vcf", "ics", "svg", "pages", "numbers", "key"
    });
}

/* Magic: PE, ELF, Mach-O. PK is zip — office Open XML is also PK, so the
 * caller combines this with the extension. */
enum class AttMagic { Other, Exec, Zip };
static AttMagic sniff_magic(const std::string &data) {
    if (data.size() < 4) return AttMagic::Other;
    const unsigned char *b = (const unsigned char *)data.data();
    if (b[0] == 'M' && b[1] == 'Z') return AttMagic::Exec;
    if (b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')
        return AttMagic::Exec;
    uint32_t be = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                  ((uint32_t)b[2] << 8) | b[3];
    if (be == 0xFEEDFACEu || be == 0xFEEDFACFu || be == 0xCAFEBABEu ||
        be == 0xCEFAEDFEu || be == 0xCFFAEDFEu)
        return AttMagic::Exec;
    if (b[0] == 'P' && b[1] == 'K') return AttMagic::Zip;
    return AttMagic::Other;
}

static bool attachment_is_exec(const MailAttachment &a) {
    const std::string ext = attachment_ext(a);
    if (is_exec_ext(ext)) return true;
    return sniff_magic(a.data) == AttMagic::Exec;
}

static bool attachment_is_archive(const MailAttachment &a) {
    const std::string ext = attachment_ext(a);
    if (is_archive_ext(ext)) return true;
    if (sniff_magic(a.data) == AttMagic::Zip &&
        !ext_in(ext, { "docx", "xlsx", "pptx", "odt", "ods", "odp",
                       "pages", "numbers", "key", "epub" }))
        return true;
    return false;
}

static NVGcolor att_type_color(const std::string &ext) {
    if (ext == "pdf") return nvgRGB(196, 52, 48);
    if (ext == "doc" || ext == "docx" || ext == "odt" || ext == "pages")
        return nvgRGB(42, 92, 178);
    if (ext == "xls" || ext == "xlsx" || ext == "ods" || ext == "numbers" ||
        ext == "csv")
        return nvgRGB(36, 138, 68);
    if (ext == "ppt" || ext == "pptx" || ext == "odp" || ext == "key")
        return nvgRGB(208, 108, 36);
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "webp" || ext == "tif" || ext == "tiff" || ext == "bmp" ||
        ext == "heic" || ext == "heif" || ext == "svg")
        return nvgRGB(42, 138, 148);
    if (is_archive_ext(ext)) return nvgRGB(140, 102, 48);
    if (ext == "txt" || ext == "rtf") return nvgRGB(96, 98, 110);
    return nvgRGB(88, 112, 156);
}

static std::string att_badge(const std::string &ext) {
    if (ext.empty()) return "";
    std::string b = ext;
    if (b.size() > 4) b.resize(4);
    for (char &c : b) c = (char)std::toupper((unsigned char)c);
    return b;
}

static std::string att_display_name(const MailAttachment &a) {
    if (!a.filename.empty()) {
        std::string fn = a.filename;
        size_t slash = fn.find_last_of("/\\");
        if (slash != std::string::npos) fn = fn.substr(slash + 1);
        if (!fn.empty()) return fn;
    }
    std::string ext = attachment_ext(a);
    return ext.empty() ? "Attachment" : "Attachment." + ext;
}

static std::string ellipsize(NVGcontext *ctx, const std::string &s, float max_w) {
    if (nvgTextBounds(ctx, 0, 0, s.c_str(), nullptr, nullptr) <= max_w)
        return s;
    std::string out = s;
    while (out.size() > 1) {
        out.pop_back();
        std::string t = out + "\xE2\x80\xA6";
        if (nvgTextBounds(ctx, 0, 0, t.c_str(), nullptr, nullptr) <= max_w)
            return t;
    }
    return "\xE2\x80\xA6";
}

/* One Mail-style page silhouette + filename + size. */
class AttachmentChip : public Widget {
public:
    std::function<void()> on_open;
    std::function<void()> on_save;
    std::function<void(const Vector2i &screen_pos)> on_menu;

    AttachmentChip(Widget *parent, const MailAttachment &att, int thumb)
        : Widget(parent), m_att(att), m_thumb(thumb) {
        set_live(true);
        set_cursor(Cursor::Hand);
        std::string tip = att_display_name(att) + "\n" +
                          format_bytes(att.data.size());
        if (!att.mime.empty()) tip += "\n" + att.mime;
        set_tooltip(tip);
        m_ext = attachment_ext(att);
        m_name = att_display_name(att);
        m_size_label = format_bytes(att.data.size());
        m_badge = att_badge(m_ext);
        m_accent = att_type_color(m_ext);
    }

    const MailAttachment &attachment() const { return m_att; }

    virtual Vector2i preferred_size(NVGcontext *) const override {
        return Vector2i(kChipW, kChipH);
    }

    virtual bool mouse_enter_event(const Vector2i &p, bool enter) override {
        m_hover = enter;
        if (Screen *s = screen()) s->redraw();
        return Widget::mouse_enter_event(p, enter);
    }

    virtual bool mouse_button_event(const Vector2i &p, int button, bool down,
                                    int mods) override {
        (void)mods;
        if (!contains(p)) return false;
        if (button == GLFW_MOUSE_BUTTON_RIGHT && down) {
            if (on_menu) on_menu(absolute_position() + (p - m_pos));
            return true;
        }
        if (button == GLFW_MOUSE_BUTTON_1 && down) {
            double now = glfwGetTime();
            bool dbl = m_last_click > 0.0 && (now - m_last_click) < 0.35;
            m_last_click = dbl ? 0.0 : now;
            m_selected = true;
            if (dbl && on_open) on_open();
            if (Screen *s = screen()) s->redraw();
            return true;
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        const float x = (float)m_pos.x(), y = (float)m_pos.y();
        const float w = (float)m_size.x(), h = (float)m_size.y();
        bool dark = false;
        if (Theme *t = theme())
            dark = (t->m_text_color.r() + t->m_text_color.g() +
                    t->m_text_color.b()) > 1.5f;

        if (m_hover || m_selected) {
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x, y, w, h, 8.0f);
            nvgFillColor(ctx, m_selected
                ? (dark ? nvgRGBA(80, 110, 180, 50) : nvgRGBA(40, 90, 180, 28))
                : (dark ? nvgRGBA(255, 255, 255, 18) : nvgRGBA(0, 0, 0, 12)));
            nvgFill(ctx);
        }

        const float pw = 56.0f, ph = 72.0f, fold = 13.0f, rad = 3.5f;
        const float px = x + (w - pw) * 0.5f;
        const float py = y + 8.0f;
        NVGcolor paper = dark ? nvgRGB(58, 59, 68) : nvgRGB(248, 246, 240);
        NVGcolor edge  = dark ? nvgRGB(110, 112, 124) : nvgRGB(196, 188, 172);
        NVGcolor foldc = dark ? nvgRGB(72, 74, 84) : nvgRGB(232, 226, 214);

        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px + rad, py);
        nvgLineTo(ctx, px + pw - fold, py);
        nvgLineTo(ctx, px + pw, py + fold);
        nvgLineTo(ctx, px + pw, py + ph - rad);
        nvgQuadTo(ctx, px + pw, py + ph, px + pw - rad, py + ph);
        nvgLineTo(ctx, px + rad, py + ph);
        nvgQuadTo(ctx, px, py + ph, px, py + ph - rad);
        nvgLineTo(ctx, px, py + rad);
        nvgQuadTo(ctx, px, py, px + rad, py);
        nvgClosePath(ctx);
        nvgFillColor(ctx, paper);
        nvgFill(ctx);
        nvgStrokeWidth(ctx, 1.15f);
        nvgStrokeColor(ctx, edge);
        nvgStroke(ctx);

        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px + pw - fold, py);
        nvgLineTo(ctx, px + pw, py + fold);
        nvgLineTo(ctx, px + pw - fold, py + fold);
        nvgClosePath(ctx);
        nvgFillColor(ctx, foldc);
        nvgFill(ctx);
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, px + pw - fold, py);
        nvgLineTo(ctx, px + pw, py + fold);
        nvgLineTo(ctx, px + pw - fold, py + fold);
        nvgStrokeColor(ctx, edge);
        nvgStroke(ctx);

        if (m_thumb > 0) {
            nvgSave(ctx);
            nvgIntersectScissor(ctx, px + 1, py + fold + 1,
                                pw - 2, ph - fold - 7);
            int iw = 0, ih = 0;
            nvgImageSize(ctx, m_thumb, &iw, &ih);
            float tw = (float)std::max(iw, 1), th = (float)std::max(ih, 1);
            float scale = std::max((pw - 2) / tw, (ph - fold - 7) / th);
            float dw = tw * scale, dh = th * scale;
            float ox = px + 1 + ((pw - 2) - dw) * 0.5f;
            float oy = py + fold + 1 + ((ph - fold - 7) - dh) * 0.5f;
            NVGpaint paint = nvgImagePattern(ctx, ox, oy, dw, dh, 0.0f,
                                             m_thumb, 1.0f);
            nvgBeginPath(ctx);
            nvgRect(ctx, px + 1, py + fold + 1, pw - 2, ph - fold - 7);
            nvgFillPaint(ctx, paint);
            nvgFill(ctx);
            nvgRestore(ctx);
        }

        nvgBeginPath(ctx);
        nvgRect(ctx, px, py + ph - 5.0f, pw, 5.0f);
        nvgFillColor(ctx, m_accent);
        nvgFill(ctx);

        if (!m_badge.empty() && m_thumb <= 0) {
            nvgFontFace(ctx, "sans-bold");
            nvgFontSize(ctx, 9.0f);
            float bw = nvgTextBounds(ctx, 0, 0, m_badge.c_str(), nullptr, nullptr);
            float bh = 13.0f, pad = 5.0f;
            float bx = px + 5.0f, by = py + ph - 22.0f;
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, bx, by, bw + pad * 2, bh, 2.5f);
            nvgFillColor(ctx, m_accent);
            nvgFill(ctx);
            nvgFillColor(ctx, nvgRGB(255, 255, 255));
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgText(ctx, bx + pad, by + bh * 0.5f, m_badge.c_str(), nullptr);
        }

        NVGcolor ink = dark ? nvgRGB(226, 227, 233) : nvgRGB(32, 32, 38);
        NVGcolor meta = dark ? nvgRGB(150, 152, 166) : nvgRGB(110, 110, 125);
        nvgFontFace(ctx, "sans");
        nvgFontSize(ctx, 11.5f);
        nvgFillColor(ctx, ink);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        std::string shown = ellipsize(ctx, m_name, w - 8.0f);
        nvgText(ctx, x + w * 0.5f, py + ph + 6.0f, shown.c_str(), nullptr);
        nvgFontSize(ctx, 10.0f);
        nvgFillColor(ctx, meta);
        nvgText(ctx, x + w * 0.5f, py + ph + 20.0f, m_size_label.c_str(), nullptr);
    }

private:
    static constexpr int kChipW = 88;
    static constexpr int kChipH = 118;
    MailAttachment m_att;
    int            m_thumb = 0;
    std::string    m_ext, m_name, m_size_label, m_badge;
    NVGcolor       m_accent{};
    bool           m_hover = false;
    bool           m_selected = false;
    double         m_last_click = 0.0;
};

class AttachmentStrip : public Widget {
public:
    explicit AttachmentStrip(Widget *parent) : Widget(parent) {
        set_live(true);
        set_height_flex(SizeMode::Preferred);
        set_width_flex(SizeMode::Expanding);
    }

    virtual Vector2i preferred_size(NVGcontext *ctx) const override {
        int inner = m_size.x() > 40 ? m_size.x()
                  : (parent() && parent()->width() > 40 ? parent()->width() : 400);
        return layout_chips(ctx, inner, nullptr);
    }

    virtual void perform_layout(NVGcontext *ctx) override {
        layout_chips(ctx, std::max(40, m_size.x()), this);
        int need = preferred_size(ctx).y();
        if (need > 0 && std::abs(need - m_size.y()) > 2) {
            for (Widget *p = parent(); p; p = p->parent())
                if (auto *hd = dynamic_cast<HtmlDocument *>(p)) {
                    hd->request_reflow();
                    break;
                }
        }
    }

private:
    static constexpr int kGap = 10, kPad = 8;
    Vector2i layout_chips(NVGcontext *ctx, int inner, Widget *place) const {
        int x = kPad, y = kPad, row_h = 0, max_x = kPad;
        for (Widget *c : m_children) {
            if (!c->visible()) continue;
            Vector2i ps = c->preferred_size(ctx);
            if (x > kPad && x + ps.x() + kPad > inner) {
                y += row_h + kGap;
                x = kPad;
                row_h = 0;
            }
            if (place) {
                c->set_position(Vector2i(x, y));
                c->set_size(ps);
                c->perform_layout(ctx);
            }
            x += ps.x() + kGap;
            row_h = std::max(row_h, ps.y());
            max_x = std::max(max_x, x);
        }
        return Vector2i(std::max(inner, max_x + kPad - kGap),
                        y + row_h + kPad);
    }
};

/* Widget::absolute_position walks m_pos and does not see ZoomScrollPanel's
 * pan/zoom draw transform. Map a logical screen point to where it is drawn. */
static Vector2i visual_screen_pos(const Widget *w, const Vector2i &logical_abs) {
    const ZoomScrollPanel *zsp = nullptr;
    for (const Widget *p = w; p && !zsp; p = p->parent())
        zsp = dynamic_cast<const ZoomScrollPanel *>(p);
    if (!zsp)
        return logical_abs;
    Vector2i zsp_abs = zsp->absolute_position();
    Vector2i rel = logical_abs - zsp_abs;
    double z = zsp->zoom();
    auto pan = zsp->pan_offset();
    return zsp_abs + Vector2i(
        (int)std::lround(pan.x() + rel.x() * z),
        (int)std::lround(pan.y() + rel.y() * z));
}

static std::string with_attachment_slots(std::string html, const MailMessage &msg) {
    if (visible_attachments(msg).empty()) return html;
    const std::string slot =
        "<div style=\"height:16px\"></div>"
        "<nmail-widget id=\"nmail-attachments\"></nmail-widget>";
    std::string low = att_lower(html);
    size_t p = low.rfind("</body>");
    if (p != std::string::npos)
        html.insert(p, slot);
    else
        html += slot;
    return html;
}

// ---------------------------------------------------------------------------
// MailApp — the application
// ---------------------------------------------------------------------------
class MailApp : public Screen {
public:
    FolderView   *m_folder_view = nullptr;
    EmailListView *m_email_list = nullptr;
    ZoomScrollPanel *m_view_scroll = nullptr;
    HtmlDocument *m_view        = nullptr;
    Label        *m_status      = nullptr;
    IndeterminateBar *m_load_bar = nullptr;
    Button       *m_theme_btn   = nullptr;
    Button       *m_compose_btn = nullptr;
    Button       *m_reply_btn   = nullptr;
    Button       *m_save_btn    = nullptr;
    Button       *m_images_btn  = nullptr;
    Button       *m_trash_btn   = nullptr;
    Button       *m_junk_btn    = nullptr;
    Button       *m_restore_btn = nullptr;

    MailConfig  m_config;
    MailWorker  m_worker;
    bool        m_config_loaded = false;
    bool        m_dark          = false;

    std::vector<MailSummary> m_summaries;   // current folder, newest first
    std::string              m_current_folder;  // folder whose list is on screen
    std::string              m_wanted_folder;   // folder the user last clicked
    bool                     m_folder_loading = false;
    std::string              m_filter;
    int                      m_loading_seq = -1;
    bool                     m_older_inflight = false;  // fetch_older posted
    MailMessage              m_current_message;
    bool                     m_has_message = false;
    /* Preview is not built on GLFW_REPEAT: the list highlight moves
     * immediately, HTML parse / IMAP FETCH wait until the selected seq
     * has been idle for kPreviewSettleSec. */
    int                      m_pending_seq = -1;
    int                      m_rendered_seq = -1;
    double                   m_preview_settle_at = 0.0;
    EmailData                m_pending_email;
    static constexpr double  kPreviewSettleSec = 0.10;
    /* Dwell time before a viewed message is flagged \Seen on the server. */
    static constexpr double  kMarkReadSec = 5.0;

    /* ---- inline/remote images in the reading pane ---- */
    std::unordered_map<std::string, int>         m_img_tex;        // src -> nvg id
    std::unordered_map<std::string, std::string> m_remote_bytes;   // url -> bytes
    std::unordered_set<std::string>              m_remote_pending;
    std::unordered_set<std::string>              m_remote_failed;
    std::unordered_set<std::string>              m_doc_remotes;
    std::deque<std::string>                      m_fetch_queue;
    int                                          m_fetch_inflight = 0;
    static const int                             kMaxInflight = 4;
    std::shared_ptr<bool>                        m_alive = std::make_shared<bool>(true);
    bool                     m_show_remote_images = false;  // user opt-in
    bool                     m_has_remote_images  = false;  // current msg refs

    /* Session-only caches; dropped on Refresh or reconnect. */
    std::map<std::string, std::vector<MailSummary>> m_summary_cache;
    std::map<std::string, MailMessage>              m_body_cache;
    std::vector<MailFolder> m_folders;
    bool m_move_inflight = false;
    std::string m_status_base;

    ContactStore m_contacts;

    static const std::string &contacts_path() {
        static const std::string p = config_file("contacts.json");
        return p;
    }

    /* Every correspondent we see becomes a completion candidate. */
    void harvest(const std::vector<MailSummary> &sums) {
        for (const MailSummary &s : sums)
            m_contacts.observe(s.from, s.from_addr);
    }
    void harvest(const MailMessage &msg) {
        m_contacts.observe(msg.from, msg.from_addr);
        m_contacts.observe_header(msg.to);
    }
    std::string m_hover_url;
    /* Header recipients the user has clicked to reveal, lowercased.
     * Reset whenever a different message is rendered. */
    std::set<std::string> m_expanded_addrs;

    PopupMenu *m_att_popup = nullptr;
    struct AttTemp { std::string file; std::string dir; };
    std::vector<AttTemp> m_att_temps;

    /* Taskbar: one button per open dialog, bottom-right of the root window.
     * Windows opt in by carrying one of the ids below. */
    Widget *m_taskbar = nullptr;
    std::vector<Window *> m_task_order;   // stable creation order

    static bool taskbar_kind(const std::string &id, int &icon,
                             std::string &tip) {
        if (id == "nmail-prefs")   { icon = FA_SLIDERS_H; tip = "Preferences";  return true; }
        if (id == "nmail-compose") { icon = FA_PEN;       tip = "New Message";  return true; }
        if (id == "nmail-reply")   { icon = FA_REPLY;     tip = "Reply";        return true; }
        return false;
    }

    /* Single exit for the dialogs the taskbar tracks.  Dispose once the event
     * stack has unwound, then refresh the buttons and ask for a frame: the
     * taskbar syncs during draw(), and closing a window does not by itself
     * request a redraw. */
    void close_dialog(Window *win) {
        if (!win) return;
        auto alive = m_alive;
        nanogui::async([this, win, alive] {
            if (!*alive) return;
            if (win && win->parent()) win->dispose();
            sync_taskbar();
            redraw();
        });
        redraw();
        glfwPostEmptyEvent();
    }

    bool is_live_window(const Window *w) const {
        for (const Widget *c : children())
            if (c == w) return true;
        return false;
    }

    /* Rebuild the buttons when the set of open dialogs changes.  Driven off
     * the live child list rather than close callbacks, so a window disposed
     * by any route simply stops appearing. */
    void sync_taskbar() {
        if (!m_taskbar) return;

        std::vector<Window *> live;
        for (Widget *c : children()) {
            auto *w = dynamic_cast<Window *>(c);
            if (!w || w->is_root() || dynamic_cast<Popup *>(w) || !w->visible())
                continue;
            int icon; std::string tip;
            if (taskbar_kind(w->id(), icon, tip)) live.push_back(w);
        }

        /* children() gets reordered every time a window is raised, so keep
         * our own order: drop the closed, append the newly opened. */
        std::vector<Window *> order;
        for (Window *w : m_task_order)
            if (std::find(live.begin(), live.end(), w) != live.end())
                order.push_back(w);
        for (Window *w : live)
            if (std::find(order.begin(), order.end(), w) == order.end())
                order.push_back(w);

        if (order == m_task_order &&
            m_taskbar->child_count() == (int)order.size())
            return;                                  // nothing changed
        m_task_order = order;

        while (m_taskbar->child_count() > 0)
            m_taskbar->remove_child_at(m_taskbar->child_count() - 1);

        for (Window *w : m_task_order) {
            int icon = 0; std::string tip;
            taskbar_kind(w->id(), icon, tip);
            Button *b = new Button(m_taskbar, "", icon);
            b->set_tooltip(tip);
            b->set_fixed_size(Vector2i(30, 22));
            b->set_callback([this, w] {
                /* The window may have been disposed between the rebuild that
                 * created this button and the click. */
                if (!is_live_window(w)) { sync_taskbar(); return; }
                w->set_visible(true);
                move_window_to_front(w);
                w->request_focus();
                sync_taskbar();
                redraw();
            });
        }
        perform_layout();
    }

    // helpers for Trash/Junk moves
    std::string resolve_dest_folder(const std::string &kind) const {
        auto lower = [](std::string s) {
            for (char &c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        if (m_folders.empty()) return kind;
        std::string want = lower(kind);
        // exact leaf match first
        for (const auto &f : m_folders) {
            std::string low = lower(f.name);
            size_t p = low.find_last_of("/.");
            std::string leaf = (p == std::string::npos) ? low : low.substr(p + 1);
            if (leaf == want) return f.name;
        }
        std::vector<std::string> keys;
        if (want == "trash") keys = {"trash","deleted","deleted messages","bin"};
        else if (want == "junk") keys = {"junk","spam","junk email","bulk mail"};
        else keys = {want};
        for (const std::string &k : keys) {
            for (const auto &f : m_folders) {
                std::string low = lower(f.name);
                size_t p = low.find_last_of("/.");
                std::string leaf = (p == std::string::npos) ? low : low.substr(p + 1);
                if (leaf.find(k) != std::string::npos) return f.name;
                if (low.find(k) != std::string::npos) return f.name;
            }
        }
        return kind;
    }
    static std::string link_domain(const std::string &url) {
        std::string s = url;
        // trim whitespace
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        s = s.substr(b, e - b + 1);
        std::string low; low.reserve(s.size());
        for (char c : s) low.push_back((char)std::tolower((unsigned char)c));
        std::string host;
        if (low.rfind("http://", 0) == 0) host = s.substr(7);
        else if (low.rfind("https://", 0) == 0) host = s.substr(8);
        else if (low.rfind("mailto:", 0) == 0) {
            size_t at = s.find('@');
            if (at != std::string::npos) {
                size_t end = s.find_first_of(" ?#", at);
                host = s.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
                // strip trailing > etc
                while (!host.empty() && (host.back() == '>' || host.back() == '"' || host.back() == '\'')) host.pop_back();
                if (host.rfind("www.", 0) == 0) host = host.substr(4);
                return host;
            }
            return s.substr(7);
        } else {
            // unknown scheme - return as-is up to slash
            host = s;
        }
        // strip userinfo if present
        size_t at = host.rfind('@');
        if (at != std::string::npos) host = host.substr(at + 1);
        // cut at / ? #
        size_t end = host.find_first_of("/?#");
        if (end != std::string::npos) host = host.substr(0, end);
        // strip port
        size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
        if (host.rfind("www.", 0) == 0) host = host.substr(4);
        return host;
    }
    void set_status(const std::string &s) {
        m_status_base = s;
        if (m_hover_url.empty() && m_status) m_status->set_caption(s);
    }
    /* Folder-open busy flag + optional status-bar indeterminate bar. */
    void set_folder_busy(bool busy, const std::string &msg = "") {
        m_folder_loading = busy;
        if (m_load_bar) {
            if (busy) m_load_bar->start();
            else      m_load_bar->stop();
        }
        if (!msg.empty()) set_status(msg);
        redraw();
    }
    void handle_link_hover(const std::string &url) {
        if (url.empty()) {
            m_hover_url.clear();
            if (m_status) m_status->set_caption(m_status_base.empty() ? "Ready" : m_status_base);
            return;
        }
        if (url.compare(0, std::strlen(kAddrScheme), kAddrScheme) == 0) {
            const std::string addr = url.substr(std::strlen(kAddrScheme));
            std::string low_a = addr;
            for (char &c : low_a) c = (char)std::tolower((unsigned char)c);
            m_hover_url.clear();
            if (m_status)
                m_status->set_caption(m_expanded_addrs.count(low_a)
                                          ? "Click to show the display name"
                                          : "Click to show " + addr);
            return;
        }
        std::string low; low.reserve(url.size());
        for (char c : url) low.push_back((char)std::tolower((unsigned char)c));
        bool is_http = (low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0);
        if (!is_http) {
            // non-HTTP links (e.g. mailto) keep normal status; don't show hover
            m_hover_url.clear();
            if (m_status) m_status->set_caption(m_status_base.empty() ? "Ready" : m_status_base);
            return;
        }
        std::string dom = link_domain(url);
        if (dom.empty()) dom = url;
        m_hover_url = url;
        if (m_status) m_status->set_caption("Open link to " + dom);
    }
    void update_move_buttons() {
        bool has_sel = m_email_list && m_email_list->selected_seq() != -1
                       && !m_current_folder.empty() && !m_move_inflight;
        auto lower = [](std::string s) {
            for (char &c : s) c = (char)std::tolower((unsigned char)c);
            return s;
        };
        std::string curLow = lower(m_current_folder);
        std::string trashDest = m_current_folder.empty() ? "" : resolve_dest_folder("Trash");
        std::string junkDest  = m_current_folder.empty() ? "" : resolve_dest_folder("Junk");
        bool trashSame = !trashDest.empty() && lower(trashDest) == curLow;
        bool junkSame  = !junkDest.empty() && lower(junkDest) == curLow;
        bool in_trash_or_junk = trashSame || junkSame;
        if (m_trash_btn) m_trash_btn->set_enabled(has_sel && !trashSame);
        if (m_junk_btn)  m_junk_btn->set_enabled(has_sel && !junkSame);
        if (m_restore_btn) m_restore_btn->set_enabled(has_sel && in_trash_or_junk);
        /* While viewing Trash/Junk, hide the trash/junk buttons and show
         * a single "restore to Inbox" button instead. */
        bool vis_changed = false;
        if (m_trash_btn && m_trash_btn->visible() == in_trash_or_junk) {
            m_trash_btn->set_visible(!in_trash_or_junk); vis_changed = true;
        }
        if (m_junk_btn && m_junk_btn->visible() == in_trash_or_junk) {
            m_junk_btn->set_visible(!in_trash_or_junk); vis_changed = true;
        }
        if (m_restore_btn && m_restore_btn->visible() != in_trash_or_junk) {
            m_restore_btn->set_visible(in_trash_or_junk); vis_changed = true;
        }
        if (vis_changed) perform_layout();
        // keep tooltips reflecting resolved destination
        if (m_trash_btn) m_trash_btn->set_tooltip(has_sel && !trashDest.empty()
            ? "Move to " + trashDest + " (Delete)" : "Move to Trash (Delete)");
        if (m_junk_btn)  m_junk_btn->set_tooltip(has_sel && !junkDest.empty()
            ? "Move to " + junkDest : "Move to Junk / Spam");
    }
    void move_selected_to(const std::string &kind) {
        if (m_move_inflight) return;
        if (!m_email_list) return;
        int seq = m_email_list->selected_seq();
        if (seq <= 0) { set_status("No message selected"); return; }
        if (m_current_folder.empty()) return;
        std::string dest = resolve_dest_folder(kind);
        if (dest.empty()) { set_status("No " + kind + " folder found"); return; }
        auto lower = [](std::string s){ for(char &c:s) c=(char)std::tolower((unsigned char)c); return s; };
        if (lower(dest) == lower(m_current_folder)) {
            set_status("Already in " + dest);
            return;
        }
        m_move_inflight = true;
        update_move_buttons();
        set_status("Moving to " + dest + "...");
        m_worker.move_message(m_current_folder, seq, dest);
    }
    void on_moved(const std::string &folder, int seq, const std::string &dest) {
        m_move_inflight = false;
        auto remove_from_vec = [&](std::vector<MailSummary> &vec){
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [&](const MailSummary &s){ return s.seq == seq; }), vec.end());
        };
        const bool viewing_source =
            folder == m_wanted_folder && folder == m_current_folder;
        if (viewing_source) {
            remove_from_vec(m_summaries);
            for (auto &s : m_summaries)
                if (s.seq > seq) --s.seq;
        }
        auto it = m_summary_cache.find(folder);
        if (it != m_summary_cache.end()) {
            remove_from_vec(it->second);
            for (auto &s : it->second)
                if (s.seq > seq) --s.seq;
        }
        // body caches use seq numbers, which shift after EXPUNGE -> drop all for folder
        std::vector<std::string> drop;
        for (auto &kv : m_body_cache)
            if (kv.first.rfind(folder + ":", 0) == 0) drop.push_back(kv.first);
        for (auto &k : drop) m_body_cache.erase(k);
        if (!viewing_source) {
            set_status("Moved to " + dest);
            update_move_buttons();
            redraw();
            return;
        }
        if (m_rendered_seq == seq) {
            m_has_message = false;
            m_rendered_seq = -1;
            m_reply_btn->set_enabled(false);
            if (m_save_btn) m_save_btn->set_enabled(false);
            m_images_btn->set_enabled(m_show_remote_images);
            Document doc;
            parse_markdown(doc, "*Message moved to " + dest + "*", text_color(), 18.f);
            m_view->set_document(std::move(doc));
            m_view_scroll->set_scroll(0.0f);
        } else if (m_rendered_seq > seq) {
            --m_rendered_seq;
        }
        if (m_loading_seq == seq) m_loading_seq = -1;
        else if (m_loading_seq > seq) --m_loading_seq;
        if (m_pending_seq == seq) { m_pending_seq = -1; m_preview_settle_at = 0; }
        else if (m_pending_seq > seq) { --m_pending_seq; --m_pending_email.seq; }
        bool removed = false;
        if (m_email_list) removed = m_email_list->remove_seq(seq);
        if (removed && m_email_list) {
            const EmailData* nd = m_email_list->selected_data();
            if (nd) {
                int nidx = m_email_list->selected_index();
                on_email_selected(nidx, *nd);
            }
        }
        if (m_email_list && m_email_list->emails().empty()) {
            m_has_message = false;
            m_rendered_seq = -1;
            m_pending_seq = -1;
            Document doc;
            parse_markdown(doc, "*No messages*", text_color(), 18.f);
            m_view->set_document(std::move(doc));
        }
        set_status("Moved to " + dest);
        update_move_buttons();
        redraw();
    }

    MailApp() : Screen(Vector2i(1100, 700), "nmail") {
        inc_ref();
        set_window_icon(glfw_window());
        // Theme — light background like macOS Mail (toggle with Ctrl/Cmd+T)
        set_theme_mode(ThemeMode::Light);
        m_theme->m_split_divider_width = 2;

        auto *root_flex = new FlexLayout(FlexDirection::Column,
                                         JustifyContent::FlexStart,
                                         AlignItems::Stretch, 0, 0);
        RootWindow *window = new RootWindow(this, root_flex);

        // ---- Toolbar ----
        Widget *toolbar = new Widget(window);
        toolbar->set_min_height(60);
        toolbar->set_height(60);
        toolbar->set_min_size(Vector2i(0, 60));
        toolbar->set_height_flex(SizeMode::Fixed);
        toolbar->set_layout(
            new BoxLayout(Orientation::Horizontal, Alignment::Middle, 0, 0));

        auto make_button_tool = [&](int icon, const std::string &tip) {
            Button *btn = new Button(toolbar, "", icon);
            btn->set_font_size(52);
            btn->set_transparent(true);
            btn->set_tooltip(tip);
            return btn;
        };

        Button *refresh_btn = make_button_tool(FA_SYNC, "Refresh");
        refresh_btn->set_callback([this]() { do_refresh(); });

        m_compose_btn = make_button_tool(FA_PEN, "Compose a new message");
        m_compose_btn->set_callback([this]() { show_compose(false); });

        m_reply_btn = make_button_tool(FA_REPLY, "Reply to this message");
        m_reply_btn->set_enabled(false);
        m_reply_btn->set_callback([this]() { show_compose(true); });

        m_save_btn = make_button_tool(FA_SAVE,
            "Save this email as HTML for nmail_view (Ctrl+S)");
        m_save_btn->set_enabled(false);
        m_save_btn->set_callback([this]() { save_current_email(); });

        m_images_btn = make_button_tool(FA_IMAGE,
            "Load remote images (off by default to block tracking pixels)");
        m_images_btn->set_flags(Button::ToggleButton);
        m_images_btn->set_enabled(false);
        /* change_callback, not callback: a ToggleButton only fires
         * callback() on the click that pushes it IN — the un-push click
         * would never reach us. */
        m_images_btn->set_change_callback([this](bool on) {
            m_show_remote_images = on;
            /* The transparent toolbar buttons have no visible toggle state,
             * so light up a solid green pill while loading is on (alpha 0
             * falls back to the normal transparent look when off). */
            m_images_btn->set_background_color(on ? Color(40, 160, 60, 255)
                                                  : Color(0, 0, 0, 0));
            m_images_btn->set_tooltip(on
                ? "Remote images on -- click to stop loading (cached stay)"
                : "Load remote images (off by default to block tracking pixels)");
            if (on) {
                /* Do not re-parse the HTML: bind_loaded_images() re-runs the
                 * resolver, which queues HTTP GETs and leaves placeholders
                 * until each texture arrives. */
                if (m_view)
                    m_view->bind_loaded_images();
                update_image_status();
            } else {
                /* Stop future loading: drop queued fetches that have not
                 * started.  In-flight ones still land in the cache, and
                 * everything already cached keeps showing — resolve_image()
                 * consults m_img_tex before the m_show_remote_images gate. */
                for (const std::string &u : m_fetch_queue)
                    m_remote_pending.erase(u);
                m_fetch_queue.clear();
            }
            redraw();
        });

        m_trash_btn = make_button_tool(FA_TRASH, "Move to Trash (Delete)");
        m_trash_btn->set_enabled(false);
        m_trash_btn->set_callback([this]() { move_selected_to("Trash"); });

        m_junk_btn = make_button_tool(FA_BROOM, "Move to Junk / Spam");
        m_junk_btn->set_enabled(false);
        m_junk_btn->set_callback([this]() { move_selected_to("Junk"); });

        /* Shown in place of trash/junk while viewing Trash or Junk. */
        m_restore_btn = make_button_tool(FA_ARROW_UP, "Move back to Inbox");
        m_restore_btn->set_enabled(false);
        m_restore_btn->set_visible(false);
        m_restore_btn->set_callback([this]() { move_selected_to("Inbox"); });

        Button *prefs_btn = make_button_tool(FA_COG, "Preferences");
        prefs_btn->set_callback([this]() { show_preferences(); });

        m_theme_btn = make_button_tool(FA_MOON, "Toggle dark mode (Ctrl+T)");
        m_theme_btn->set_callback([this]() {
            apply_theme_mode(m_dark ? ThemeMode::Light : ThemeMode::Dark);
        });

        // Spacer
        Widget *spacer = new Widget(toolbar);
        spacer->set_min_width(10);
        spacer->set_width(10);

        // Search box — press Enter to filter the message list
        TextBox *search_box = new TextBox(toolbar);
        search_box->set_min_width(200);
        search_box->set_placeholder("Search sender or subject...");
        search_box->set_callback([this](const std::string &value) {
            m_filter = value;
            apply_filter();
            return true;
        });

        // ---- Horizontal split: sidebar | content ----
        Split *split = new Split(window, Split::Orientation::Horizontal);
        split->set_min_size(100);
        split->set_max_size({2048, 2048});
        split->set_keep_size_on_resize(true);
        root_flex->set_flex_item(split, FlexLayout::FlexItem(1.0f, 1.0f, 0));

        // ---- Left: FolderView sidebar ----
        m_folder_view = new FolderView(split, [this](FolderItem *item) {
            on_folder_selected(item);
        });
        m_folder_view->set_min_width(250);

        // ---- Right side: inner Split  (email list | message pane) ----
        Split *inner_split = new Split(split, Split::Orientation::Horizontal);
        inner_split->set_min_size(100);
        inner_split->set_max_size({2048, 2048});
        inner_split->set_keep_size_on_resize(true);

        // ---- Middle: email list ----
        m_email_list = new EmailListView(inner_split,
            [this](int idx, const EmailData &d) { on_email_selected(idx, d); });
        m_email_list->set_min_width(280);
        m_email_list->set_font_size(26);
        m_email_list->set_on_hit_bottom([this]() { maybe_fetch_older(); });
        m_email_list->on_viewport_changed = [this]() {
            if (m_current_folder.empty() || !m_email_list) return;
            // debounce: only throttle with a flag — draw() already limits rate
            static double last = 0; double now = glfwGetTime();
            if (now - last < 0.15 && m_email_list->visible_seqs().size() < 30) return;
            last = now;
            // skip prefetch for rows already cached
            auto seqs = m_email_list->visible_seqs(6);
            std::vector<int> need; need.reserve(seqs.size());
            for (int s : seqs) {
                if (m_body_cache.find(m_current_folder + ":" + std::to_string(s)) != m_body_cache.end()) continue;
                need.push_back(s);
            }
            if (!need.empty()) m_worker.ensure_visible_cached(m_current_folder, need);
        };

        // ---- Right: message area ----
        Widget *right = new Widget(inner_split);
        auto *rflex = new FlexLayout(FlexDirection::Column,
                                     JustifyContent::FlexStart,
                                     AlignItems::Stretch, 0, 0);
        right->set_layout(rflex);
        right->set_min_width(100);

        Widget *sep = new Widget(right);
        sep->set_min_height(1);
        sep->set_height(1);

        m_view_scroll = new ZoomScrollPanel(right, ZoomScrollPanel::ScrollTypes::Both);
        m_view_scroll->set_reflow_on_zoom(false);
        m_view_scroll->set_zoom_range(0.5, 3.0);
        m_view_scroll->set_zoom_enabled(true);
        m_view = new HtmlDocument(m_view_scroll);
        m_view->image_resolver = [this](const std::string &src) {
            return resolve_image(src);
        };
        m_view->on_link_click = [this](const std::string &url) -> bool {
            const size_t n = std::strlen(kAddrScheme);
            if (url.compare(0, n, kAddrScheme) != 0) return false;
            toggle_expanded_addr(url.substr(n));
            return true;                     // handled: do not open a browser
        };
        m_view->on_link_hover = [this](const std::string &url) {
            handle_link_hover(url);
        };
        m_view->embed_widget = [this](Widget *parent, const HtmlEmbedSpec &spec)
                -> Widget * {
            if (spec.id != "nmail-attachments")
                return nullptr;
            return make_attachment_strip(parent);
        };
        style_editor();
        m_view_scroll->set_height_flex(SizeMode::Expanding);
        rflex->set_flex_item(m_view_scroll, FlexLayout::FlexItem(1.0f));

        // ---- Status bar (left) + window taskbar (right) ----
        Widget *statusbar = new Widget(window);
        statusbar->set_min_height(26);
        statusbar->set_height(26);
        statusbar->set_height_flex(SizeMode::Fixed);
        statusbar->set_layout(new FlexLayout(FlexDirection::Row,
                                             JustifyContent::SpaceBetween,
                                             AlignItems::Center, 0, 6));
        Widget *status_left = new Widget(statusbar);
        status_left->set_layout(new BoxLayout(Orientation::Horizontal,
                                              Alignment::Middle, 0, 8));
        m_status = new Label(status_left, "Not connected", "sans", 16);
        m_load_bar = new IndeterminateBar(status_left);
        m_load_bar->set_fixed_size(Vector2i(180, 8));
        m_load_bar->set_visible(false);
        m_taskbar = new Widget(statusbar);
        m_taskbar->set_layout(new BoxLayout(Orientation::Horizontal,
                                            Alignment::Middle, 0, 4));
        m_status_base = "Not connected";

        split->set_drag_position(0.22f);
        inner_split->set_drag_position(0.38f);

        show_welcome();

        perform_layout();

        // ---- Worker callbacks (always invoked on the GUI thread) ----
        m_worker.cb_folders = [this](const std::vector<MailFolder> &folders) {
            on_folders(folders);
        };
        m_worker.cb_summaries = [this](const std::string &folder,
                                       const std::vector<MailSummary> &sums) {
            on_summaries(folder, sums);
        };
        m_worker.cb_auto_summaries = [this](const std::string &folder,
                                            const std::vector<MailSummary> &sums) {
            on_auto_summaries(folder, sums);
        };
        m_worker.cb_older = [this](const std::string &folder,
                                   const std::vector<MailSummary> &sums) {
            on_older(folder, sums);
        };
        m_worker.cb_body = [this](const std::string &folder, int seq,
                                  const MailMessage &msg) {
            on_body(folder, seq, msg);
        };
        m_worker.cb_prefetched = [this](const std::string &folder, int seq,
                                        const MailMessage &msg,
                                        const std::string &preview) {
            on_prefetched(folder, seq, msg, preview);
        };
        m_worker.cb_error = [this](const std::string &title,
                                   const std::string &msg) {
            on_worker_error(title, msg);
        };
        m_worker.cb_status = [this](const std::string &msg,
                                    const std::string &folder) {
            /* A late "Opening Trash..." must not overwrite Inbox after a
             * folder click.  While a folder is loading, ignore untagged
             * status (Connected/Ready) as well. */
            if (!folder.empty() && folder != m_wanted_folder)
                return;
            if (m_folder_loading && folder.empty())
                return;
            set_status(msg);
        };
        m_worker.cb_progress = [this](const std::string &folder, int done, int total) {
            if (folder != m_wanted_folder || !m_folder_loading) return;
            if (m_load_bar && total > 0)
                m_load_bar->set_progress((float)done / (float)total);
            set_status(folder + ": " + std::to_string(done) + " / " +
                       std::to_string(total));
            redraw();
        };
        m_worker.cb_seen = [this](const std::string &folder, int seq) {
            on_seen(folder, seq);
        };
        m_worker.cb_moved = [this](const std::string &folder, int seq,
                                   const std::string &dest) {
            on_moved(folder, seq, dest);
        };
        m_worker.start();

        // ---- Load saved account, or ask for it ----
        if (load_config(m_config)) {
            m_config_loaded = true;
            m_worker.set_config(m_config);
            m_worker.connect();
        } else {
            show_preferences();
        }
        /* Only touch contacts.json when the user has opted in. */
        if (m_config.save_contacts)
            m_contacts.load(contacts_path());
        apply_theme_mode(m_config.dark_mode ? ThemeMode::Dark
                                            : ThemeMode::Light);
    }

    virtual ~MailApp() override {
        *m_alive = false;
        clear_image_textures();
        cleanup_att_temps();
        m_worker.stop();
        if (m_config.save_contacts && m_contacts.dirty())
            m_contacts.save(contacts_path());
    }

    /* ---- appearance ---- */

    NVGcolor text_color() const {
        return m_dark ? nvgRGBA(226, 227, 233, 255) : nvgRGBA(20, 20, 25, 255);
    }
    NVGcolor meta_color() const {
        return m_dark ? nvgRGBA(150, 152, 166, 255) : nvgRGBA(110, 110, 125, 255);
    }

    void style_editor() {
        m_view->set_background(m_dark ? nvgRGBA(30, 31, 38, 255)
                                      : nvgRGBA(250, 250, 252, 255));
        m_view->set_colors(text_color(), meta_color());
    }

    void show_welcome() {
        Document doc;
        parse_markdown(doc,
            "# nmail\n\n"
            "Set up your IMAP account in **Preferences** (the gear icon) "
            "to begin.\n\n"
            "Messages and folders are pulled live from the server; "
            "nothing is cached locally.",
            text_color(), 18.0f);
        m_view->set_document(std::move(doc));
    }

    /* Switch light/dark appearance (mirrors example1's apply_theme_mode). */
    void apply_theme_mode(ThemeMode mode) {
        m_dark = (mode == ThemeMode::Dark);
        set_theme_mode(mode);
        m_theme->m_split_divider_width = 2;
        m_theme_btn->set_icon(m_dark ? FA_SUN : FA_MOON);
        m_email_list->set_dark(m_dark);
        style_editor();
        if (m_has_message)
            render_current();
        else
            show_welcome();
        perform_layout();
        redraw();
        /* Persist alongside the account settings (only once a config
         * exists, so a first-run toggle doesn't create an empty one). */
        if (m_config_loaded && m_config.dark_mode != m_dark) {
            m_config.dark_mode = m_dark;
            save_config(m_config);
        }
    }

    /* ---- GUI-side handlers ---- */

    void on_folders(const std::vector<MailFolder> &folders) {
        m_folders = folders;
        m_move_inflight = false;
        /* Fresh connection / explicit refresh: drop all cached state. */
        m_summary_cache.clear();
        m_body_cache.clear();
        update_move_buttons();

        std::string account = m_config.username.empty()
            ? m_config.host
            : m_config.username + " @ " + m_config.host;

        std::string highlight = !m_wanted_folder.empty() ? m_wanted_folder
                                                         : m_current_folder;

        // Auto-open the INBOX after the first connect.
        if (highlight.empty()) {
            for (const auto &f : folders) {
                std::string lower = f.name;
                for (char &c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower == "inbox") {
                    highlight = f.name;
                    m_wanted_folder = f.name;
                    set_folder_busy(true, "Opening " + f.name + "...");
                    m_folder_view->rebuild(account, folders, highlight);
                    m_worker.select_folder(f.name);
                    return;
                }
            }
        }

        m_folder_view->rebuild(account, folders, highlight);
    }

    void on_summaries(const std::string &folder,
                      const std::vector<MailSummary> &sums) {
        mail_dbg("[mail] UI on_summaries folder='%s' wanted='%s' n=%zu loading=%d\n",
                folder.c_str(), m_wanted_folder.c_str(), sums.size(),
                (int)m_folder_loading);
        harvest(sums);
        if (folder != m_wanted_folder) {
            /* Stale: INBOX headers arriving after a Trash click.  Keep the
             * cache for that folder unless the payload is empty and we
             * already have a better list. */
            if (!sums.empty() || m_summary_cache.find(folder) == m_summary_cache.end())
                m_summary_cache[folder] = sums;
            return;
        }

        /* A SELECT that missed EXISTS used to replace a good cached list
         * with nothing ("flash then clear").  Keep what we have and say so. */
        if (sums.empty() && !m_summaries.empty() && m_current_folder == folder) {
            set_folder_busy(false, folder + ": keeping " +
                std::to_string(m_summaries.size()) +
                " cached (server sent none)");
            return;
        }
        m_summary_cache[folder] = sums;

        m_current_folder = folder;
        m_summaries      = sums;
        m_older_inflight = false;
        m_move_inflight = false;
        m_email_list->set_loading_more(false);
        apply_filter();
        update_move_buttons();
        set_folder_busy(false,
            folder + ": " + std::to_string(sums.size()) +
            (sums.size() == 1 ? " message" : " messages"));
        // after layout, kick viewport prefetch so rows actually on screen win
        redraw();
        nanogui::async([this, folder]() {
            if (folder != m_wanted_folder || folder != m_current_folder ||
                !m_email_list)
                return;
            auto seqs = m_email_list->visible_seqs(6);
            std::vector<int> need; need.reserve(seqs.size());
            for (int s : seqs)
                if (m_body_cache.find(folder + ":" + std::to_string(s)) == m_body_cache.end())
                    need.push_back(s);
            if (!need.empty()) m_worker.ensure_visible_cached(folder, need);
        });
        glfwPostEmptyEvent();
    }

    /* Periodic background check (see MailWorker::Type::AutoRefresh):
       splice newly-arrived mail in at the top instead of replacing the
       list, so the user's scroll position and selection are undisturbed.
       Announces the count on the status bar rather than jumping the view
       to show it. */
    void on_auto_summaries(const std::string &folder,
                           const std::vector<MailSummary> &sums) {
        if (sums.empty()) return;
        // Merge into the cache for *this* folder.  Never splice into
        // m_summaries unless the user is still looking at `folder` —
        // otherwise an INBOX auto-check would pollute the Trash list.
        auto merge_fresh = [](std::vector<MailSummary> &dst,
                              const std::vector<MailSummary> &incoming) {
            std::unordered_set<int> known;
            known.reserve(dst.size());
            for (const MailSummary &s : dst) known.insert(s.seq);
            std::vector<MailSummary> fresh;
            fresh.reserve(incoming.size());
            for (const MailSummary &s : incoming)
                if (!known.count(s.seq)) fresh.push_back(s);
            if (!fresh.empty())
                dst.insert(dst.begin(), fresh.begin(), fresh.end());
            return fresh;
        };

        if (folder != m_wanted_folder) {
            auto &cache = m_summary_cache[folder];
            auto fresh = merge_fresh(cache, sums);
            harvest(fresh);
            return;
        }

        auto fresh = merge_fresh(m_summaries, sums);
        m_summary_cache[folder] = m_summaries;
        harvest(fresh);
        if (fresh.empty()) return;
        if (folder != m_current_folder) return;   // not looking at this folder

        /* Splice in only the rows passing the active filter; unlike
           apply_filter() this leaves scroll position and selection alone
           (see EmailListView::prepend_emails). */
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);
        std::vector<EmailData> rows;
        rows.reserve(fresh.size());
        for (const MailSummary &s : fresh) {
            if (!needle.empty()) {
                std::string hay = s.from + "\n" + s.subject;
                for (char &c : hay) c = (char)std::tolower((unsigned char)c);
                if (hay.find(needle) == std::string::npos) continue;
            }
            EmailData d;
            d.seq     = s.seq;
            d.sender  = s.from;
            d.subject = s.subject;
            d.preview = s.preview;
            d.date    = s.date;
            d.seen    = s.seen;
            rows.push_back(d);
        }
        if (!rows.empty())
            m_email_list->prepend_emails(std::move(rows));

        set_status(std::to_string(fresh.size()) + " New email" +
                  (fresh.size() == 1 ? "" : "s"));
        glfwPostEmptyEvent();
    }

    /* Ask the worker for the next older page when the list hits bottom. */
    void maybe_fetch_older() {
        if (m_folder_loading || m_older_inflight ||
            m_wanted_folder.empty() || m_summaries.empty())
            return;
        if (m_wanted_folder != m_current_folder)
            return;
        int oldest = m_summaries.back().seq;   // list is newest-first
        if (oldest <= 1) return;               // already at the first message
        m_older_inflight = true;
        m_email_list->set_loading_more(true);
        set_status("Loading older messages in " + m_wanted_folder + "...");
        m_worker.fetch_older(m_wanted_folder);
    }

    void on_older(const std::string &folder,
                  const std::vector<MailSummary> &sums) {
        if (folder != m_wanted_folder || folder != m_current_folder) return;
        m_older_inflight = false;
        m_email_list->set_loading_more(false);
        if (sums.empty()) return;

        m_summaries.insert(m_summaries.end(), sums.begin(), sums.end());
        m_summary_cache[folder] = m_summaries;
        harvest(sums);
        // make newly paged-in older rows eligible for viewport prefetch too
        {
            std::vector<int> seqs; seqs.reserve(sums.size());
            for (auto &s : sums) seqs.push_back(s.seq);
            m_worker.ensure_visible_cached(folder, seqs);
        }

        /* Append only the rows passing the active filter; unlike
           apply_filter() this leaves scroll position and selection alone. */
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);
        std::vector<EmailData> rows;
        rows.reserve(sums.size());
        for (const MailSummary &s : sums) {
            if (!needle.empty()) {
                std::string hay = s.from + "\n" + s.subject;
                for (char &c : hay) c = (char)std::tolower((unsigned char)c);
                if (hay.find(needle) == std::string::npos) continue;
            }
            EmailData d;
            d.seq     = s.seq;
            d.sender  = s.from;
            d.subject = s.subject;
            d.preview = s.preview;
            d.date    = s.date;
            d.seen    = s.seen;
            rows.push_back(d);
        }
        m_email_list->append_emails(std::move(rows));
        set_status(folder + ": showing " +
                              std::to_string(m_summaries.size()) +
                              " messages");
        redraw();
    }

    void on_body(const std::string &folder, int seq, const MailMessage &msg) {
        harvest(msg);
        std::string key_folder = folder.empty() ? m_wanted_folder : folder;
        // Always enrich the preview + cache, even if this wasn't the
        // foreground fetch — background prefetches land here too when
        // the user happens to be looking at that message.
        std::string preview = message_preview(msg);
        if (!preview.empty() && key_folder == m_current_folder) {
            for (auto &s : m_summaries)
                if (s.seq == seq && s.preview != preview) { s.preview = preview; break; }
            auto it = m_summary_cache.find(key_folder);
            if (it != m_summary_cache.end())
                for (auto &s : it->second)
                    if (s.seq == seq && s.preview != preview) { s.preview = preview; break; }
            if (m_email_list) m_email_list->update_preview(seq, preview);
        }
        if (m_body_cache.size() > 256) m_body_cache.clear();
        // every full fetch is cacheable; on_prefetched also caches, so this
        // is idempotent — just keep the freshest copy
        if (!key_folder.empty())
            m_body_cache[key_folder + ":" + std::to_string(seq)] = msg;
        if (key_folder != m_wanted_folder) return;
        if (seq != m_loading_seq) return;   // not the foreground fetch
        if (m_pending_seq >= 0 && seq != m_pending_seq)
            return;   // still scrubbing a different message
        m_loading_seq = -1;
        m_current_message = msg;
        m_has_message     = true;
        m_rendered_seq    = seq;
        m_expanded_addrs.clear();   // reveals belong to the message shown
        arm_read_timer(seq);
        m_reply_btn->set_enabled(true);
        if (m_save_btn) m_save_btn->set_enabled(true);
        render_current();
    }

    void on_prefetched(const std::string &folder, int seq,
                       const MailMessage &msg, const std::string &preview) {
        if (m_body_cache.size() > 256) m_body_cache.clear();
        m_body_cache[folder + ":" + std::to_string(seq)] = msg;
        harvest(msg);
        if (folder != m_wanted_folder || folder != m_current_folder) return;
        // only enrich empty/thin previews — never clobber a real one with
        // a shorter derived snippet from a failed decode edge case
        bool enriched = false;
        for (auto &s : m_summaries) {
            if (s.seq != seq) continue;
            if (preview.size() > s.preview.size()) { s.preview = preview; enriched = true; }
            break;
        }
        auto it = m_summary_cache.find(folder);
        if (it != m_summary_cache.end())
            for (auto &s : it->second)
                if (s.seq == seq && preview.size() > s.preview.size()) { s.preview = preview; break; }
        if (enriched && m_email_list && preview.size() > 0)
            m_email_list->update_preview(seq, preview);
    }

    void on_worker_error(const std::string &title, const std::string &msg) {
        m_older_inflight = false;
        m_move_inflight = false;
        if (m_email_list) m_email_list->set_loading_more(false);
        set_folder_busy(false, title);
        update_move_buttons();
        auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                                      title, msg, "OK", "", false);
        dlg->center();
    }

    void on_folder_selected(FolderItem *item) {
        // The tooltip carries the full folder name (caption is the leaf).
        std::string folder = item->tooltip();
        if (folder.empty()) folder = item->caption();
        /* Already opening this folder — ignore the duplicate click. */
        if (folder == m_wanted_folder && m_folder_loading) return;
        /* Already showing this folder with a populated list. */
        if (folder == m_wanted_folder && folder == m_current_folder &&
            !m_folder_loading && !m_summaries.empty())
            return;
        /* A mark-read timer armed in the old folder must not fire here --
         * the message is no longer on screen (and seqs may shift). */
        m_worker.cancel_seen();
        m_loading_seq  = -1;
        m_pending_seq  = -1;
        m_rendered_seq = -1;
        m_has_message  = false;
        m_older_inflight = false;
        m_move_inflight = false;
        m_email_list->set_loading_more(false);
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);

        /* Pin the wanted folder *before* any async IMAP callback can land,
         * so a late INBOX summary cannot hijack the Trash view. */
        m_wanted_folder  = folder;
        m_current_folder = folder;

        /* Serve the last-known list instantly, then refresh from the
         * server in the background.  Do not clear the widget first —
         * that one-frame empty list was the "flash then clear". */
        auto cached = m_summary_cache.find(folder);
        if (cached != m_summary_cache.end()) {
            m_summaries = cached->second;
            apply_filter();
            set_folder_busy(true, folder + ": " +
                std::to_string(m_summaries.size()) +
                " cached, fetching latest...");
        } else {
            m_summaries.clear();
            m_email_list->set_emails({});
            Document doc;
            parse_markdown(doc, "*Loading " + folder + "...*", text_color(), 18.0f);
            m_view->set_document(std::move(doc));
            set_folder_busy(true, "Opening " + folder + "...");
        }
        update_move_buttons();
        redraw();
        m_worker.select_folder(folder);
    }

    void on_email_selected(int idx, const EmailData &d) {
        update_move_buttons();
        /* List highlight already moved.  Do not parse HTML or FETCH on
         * every GLFW_REPEAT — wait until this seq sits still. */
        if (d.seq == m_pending_seq)
            return;
        if (d.seq == m_rendered_seq && m_pending_seq < 0)
            return;
        // speculatively prioritize neighbors of the selection — the user is
        // walking the list sequentially, so ±6 around idx are most likely next.
        if (m_email_list && idx >= 0) {
            std::vector<int> around; around.reserve(13);
            auto &rows = m_email_list->emails();
            for (int i = std::max(0, idx-6); i <= std::min((int)rows.size()-1, idx+6); ++i) {
                int s = rows[i].seq;
                if (s == d.seq) continue;
                if (m_body_cache.find(m_current_folder + ":" + std::to_string(s)) != m_body_cache.end()) continue;
                around.push_back(s);
            }
            if (!around.empty()) m_worker.ensure_visible_cached(m_current_folder, around);
        }
        m_loading_seq = -1;   // drop in-flight body for a previous seq
        /* Cancel now rather than waiting for the preview to settle, so a
         * near-expired timer on the previous message cannot still fire. */
        if (d.seq != m_rendered_seq) m_worker.cancel_seen();
        const bool switched = (d.seq != m_rendered_seq);
        m_pending_seq       = d.seq;
        m_pending_email     = d;
        m_preview_settle_at = glfwGetTime() + kPreviewSettleSec;
        if (switched)
            show_preview_stub(d);
        redraw();
    }

    void show_preview_stub(const EmailData &d) {
        // Keep header/body styling identical to render_message() so the
        // preview does not visually jump when the full message arrives.
        Document doc;
        Style normal; normal.fontSize = 17.0f; normal.fgColor = text_color();
        Style bold   = normal; bold.bold = true;
        Style subj   = normal; subj.fontSize = 24.0f; subj.bold = true;
        Style meta   = normal; meta.fgColor = meta_color();
        doc.addParagraph()->addText(d.subject.empty() ? "(no subject)"
                                                      : d.subject, subj);
        auto *pf = doc.addParagraph();
        pf->addText("From: ", bold);
        pf->addText(d.sender, normal);
        if (!d.date.empty()) {
            auto *pd = doc.addParagraph();
            pd->addText("Date: ", bold);
            pd->addText(d.date, meta);
        }
        auto *rule = doc.addParagraph();
        rule->isRule = true;
        if (!d.preview.empty())
            doc.addParagraph()->addText(d.preview, normal);
        m_view->set_document(std::move(doc));
        m_has_message = false;
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);
        m_view_scroll->set_scroll(0.0f);
    }

    // Build a stub doc that appends a subtle "Loading…" row to the same
    // header/preview content that show_preview_stub shows, so the user
    // still sees what they selected while the FETCH is in flight.
    void show_preview_stub_with_loading(const EmailData &d) {
        Document doc;
        Style normal; normal.fontSize = 17.0f; normal.fgColor = text_color();
        Style bold   = normal; bold.bold = true;
        Style subj   = normal; subj.fontSize = 24.0f; subj.bold = true;
        Style meta   = normal; meta.fgColor = meta_color();
        Style loading = meta; loading.italic = true;
        loading.fontSize = 14.0f;
        doc.addParagraph()->addText(d.subject.empty() ? "(no subject)"
                                                      : d.subject, subj);
        auto *pf = doc.addParagraph();
        pf->addText("From: ", bold);
        pf->addText(d.sender, normal);
        if (!d.date.empty()) {
            auto *pd = doc.addParagraph();
            pd->addText("Date: ", bold);
            pd->addText(d.date, meta);
        }
        auto *rule = doc.addParagraph(); rule->isRule = true;
        if (!d.preview.empty()) doc.addParagraph()->addText(d.preview, normal);
        auto *rule2 = doc.addParagraph(); rule2->isRule = true;
        doc.addParagraph()->addText("Loading message\u2026", loading);
        m_view->set_document(std::move(doc));
        m_has_message = false;
        m_reply_btn->set_enabled(false);
        if (m_save_btn) m_save_btn->set_enabled(false);
        // Keep scroll at top — the stub is the loading view, not a separate page.
        m_view_scroll->set_scroll(0.0f);
    }

    /* Start the read clock for the message now on screen.  Already-read mail
     * needs no STORE, and a message with no folder cannot be addressed. */
    void arm_read_timer(int seq) {
        if (seq <= 0 || m_current_folder.empty()) { m_worker.cancel_seen(); return; }
        for (const MailSummary &s : m_summaries)
            if (s.seq == seq && s.seen) { m_worker.cancel_seen(); return; }
        m_worker.schedule_seen(m_current_folder, seq, kMarkReadSec);
    }

    /* The server confirmed the flag: mirror it locally so the row stops
     * rendering as unread. */
    void on_seen(const std::string &folder, int seq) {
        auto mark = [seq](std::vector<MailSummary> &v) {
            for (MailSummary &s : v)
                if (s.seq == seq) { s.seen = true; break; }
        };
        auto it = m_summary_cache.find(folder);
        if (it != m_summary_cache.end()) mark(it->second);
        if (folder != m_wanted_folder || folder != m_current_folder) return;
        mark(m_summaries);
        if (m_email_list) m_email_list->set_seen(seq, true);
        redraw();
    }

    void commit_pending_preview() {
        if (m_pending_seq < 0)
            return;
        const EmailData d = m_pending_email;
        const int seq = m_pending_seq;
        m_pending_seq = -1;
        auto cached = m_body_cache.find(m_current_folder + ":" +
                                        std::to_string(seq));
        if (cached != m_body_cache.end()) {
            m_loading_seq     = -1;
            m_current_message = cached->second;
            m_has_message     = true;
            m_rendered_seq    = seq;
            m_expanded_addrs.clear();
            m_reply_btn->set_enabled(true);
            if (m_save_btn) m_save_btn->set_enabled(true);
            render_current();
            arm_read_timer(seq);
            return;
        }
        m_loading_seq = seq;
        show_preview_stub_with_loading(d);
        m_worker.fetch_body(seq);
        redraw();
    }

    void pump_preview() {
        if (m_pending_seq < 0)
            return;
        if (glfwGetTime() < m_preview_settle_at) {
            /* Need another frame after the settle deadline; WaitEvents
             * would otherwise sleep until the next key. */
            redraw();
            return;
        }
        commit_pending_preview();
    }

    void do_refresh() {
        if (m_config.host.empty()) {
            show_preferences();
            return;
        }
        /* Explicit refresh means "forget everything I know". */
        m_summary_cache.clear();
        m_body_cache.clear();
        m_worker.cancel_seen();   // seqs may renumber; don't flag a stranger
        if (!m_wanted_folder.empty())
            set_folder_busy(true, "Refreshing " + m_wanted_folder + "...");
        else
            set_folder_busy(true, "Refreshing folders...");
        m_worker.refresh();
    }

    /* Filter the current folder's summaries into the list widget. */
    void apply_filter() {
        std::string needle = m_filter;
        for (char &c : needle) c = (char)std::tolower((unsigned char)c);

        std::vector<EmailData> rows;
        rows.reserve(m_summaries.size());
        for (const MailSummary &s : m_summaries) {
            if (!needle.empty()) {
                std::string hay = s.from + "\n" + s.subject;
                for (char &c : hay) c = (char)std::tolower((unsigned char)c);
                if (hay.find(needle) == std::string::npos) continue;
            }
            EmailData d;
            d.seq     = s.seq;
            d.sender  = s.from;
            d.subject = s.subject;
            d.preview = s.preview;
            d.date    = s.date;
            d.seen    = s.seen;
            rows.push_back(d);
        }
        m_email_list->set_emails(std::move(rows));
        update_move_buttons();
    }

    /* ---- inline / remote images in the reading pane ---- */

    void clear_image_textures() {
        for (auto &kv : m_img_tex)
            nvgDeleteImage(nvg_context(), kv.second);
        m_img_tex.clear();
    }

    HtmlImageInfo make_image_info(int id) {
        HtmlImageInfo ri;
        if (id <= 0)
            return ri;
        ri.id = id;
        int w = 0, h = 0;
        nvgImageSize(nvg_context(), id, &w, &h);
        ri.w = (float)w;
        ri.h = (float)h;
        return ri;
    }

    int create_image_texture(const std::string &src, const std::string &bytes) {
        if (bytes.empty())
            return 0;
        int id = nvgCreateImageMem(nvg_context(), NVG_IMAGE_PREMULTIPLIED,
                                   (unsigned char *)bytes.data(),
                                   (int)bytes.size());
        if (id <= 0)
            return 0;
        m_img_tex[src] = id;
        return id;
    }

    /* Resolve an <img> src to an NVG image, creating the texture on first
       use.  Remote URLs load only after the user opts in; a cache miss
       kicks an async fetch and resolves to id 0 (placeholder) this pass. */
    HtmlImageInfo resolve_image(const std::string &src) {
        auto cached = m_img_tex.find(src);
        if (cached != m_img_tex.end())
            return make_image_info(cached->second);

        if (src.rfind("cid:", 0) == 0) {
            std::string cid = src.substr(4);
            for (const MailImage &img : m_current_message.images) {
                if (img.cid == cid)
                    return make_image_info(
                        create_image_texture(src, img.data));
            }
            return HtmlImageInfo{};
        }

        if (src.rfind("http://", 0) == 0 || src.rfind("https://", 0) == 0) {
            m_has_remote_images = true;
            m_doc_remotes.insert(src);
            if (!m_show_remote_images)
                return HtmlImageInfo{};
            auto it = m_remote_bytes.find(src);
            if (it != m_remote_bytes.end()) {
                int id = create_image_texture(src, it->second);
                if (id <= 0)
                    m_remote_failed.insert(src);
                return make_image_info(id);
            }
            if (!m_remote_failed.count(src))
                queue_remote_fetch(src);
            return HtmlImageInfo{};
        }
        return HtmlImageInfo{};
    }

    void queue_remote_fetch(const std::string &url) {
        if (m_remote_pending.count(url) || m_remote_bytes.count(url) ||
            m_remote_failed.count(url))
            return;
        m_remote_pending.insert(url);
        m_fetch_queue.push_back(url);
        pump_fetches();
    }

    void pump_fetches() {
        while (m_fetch_inflight < kMaxInflight && !m_fetch_queue.empty()) {
            std::string url = m_fetch_queue.front();
            m_fetch_queue.pop_front();
            ++m_fetch_inflight;
            auto alive = m_alive;
            std::thread([this, url, alive]() {
                std::string bytes;
                bool ok = nmail_http_get(url, bytes);
                nanogui::async([this, url, ok, bytes = std::move(bytes),
                                alive]() mutable {
                    if (!alive || !*alive)
                        return;
                    on_fetch_done(url, ok, std::move(bytes));
                });
                glfwPostEmptyEvent();
            }).detach();
        }
    }

    void on_fetch_done(const std::string &url, bool ok, std::string &&bytes) {
        --m_fetch_inflight;
        m_remote_pending.erase(url);
        if (ok && !bytes.empty()) {
            m_remote_bytes[url] = std::move(bytes);
            if (!m_img_tex.count(url)) {
                int id = create_image_texture(url, m_remote_bytes[url]);
                if (id <= 0)
                    m_remote_failed.insert(url);
            }
        } else {
            m_remote_failed.insert(url);
        }

        if (m_has_message && m_view) {
            Vector2f sc = m_view_scroll ? m_view_scroll->scroll()
                                        : Vector2f(0.f, 0.f);
            m_view->bind_loaded_images();
            if (m_view_scroll)
                m_view_scroll->set_scroll(sc.y());
        }
        update_image_status();
        pump_fetches();
    }

    void update_image_status() {
        if (m_doc_remotes.empty() || !m_show_remote_images)
            return;
        int have = 0, fail = 0;
        int n = (int)m_doc_remotes.size();
        for (const auto &u : m_doc_remotes) {
            if (m_img_tex.count(u))
                ++have;
            else if (m_remote_failed.count(u))
                ++fail;
        }
        char buf[256];
        if (have + fail >= n) {
            if (fail)
                std::snprintf(buf, sizeof(buf),
                              "Images %d/%d (%d failed)", have, n, fail);
            else
                std::snprintf(buf, sizeof(buf), "Images %d/%d", have, n);
        } else {
            std::snprintf(buf, sizeof(buf), "Loading images %d/%d", have, n);
        }
        set_status(buf);
    }

    /* (Re-)render the current message — on select, body arrival, theme
       change.  Image bytes bind in place via bind_loaded_images(). */
    /* Re-render the message with the current set of revealed addresses.
     * Cheap enough: this is the same path a message switch already takes. */
    void toggle_expanded_addr(const std::string &addr) {
        std::string low = addr;
        for (char &c : low) c = (char)std::tolower((unsigned char)c);
        if (!m_expanded_addrs.erase(low))
            m_expanded_addrs.insert(low);
        /* render_current() resets the scroll to the top; the user clicked a
         * name, they did not ask to be sent back to the start of the mail. */
        Vector2f keep = m_view_scroll ? m_view_scroll->scroll() : Vector2f(0.f, 0.f);
        render_current();
        if (m_view_scroll) m_view_scroll->set_scroll(keep);
    }

    void render_current() {
        if (!m_has_message) return;
        hide_att_popup();
        clear_image_textures();
        m_has_remote_images = false;
        m_doc_remotes.clear();
        const MailMessage &msg = m_current_message;
        if (!msg.html.empty()) {
            /* Rich render of the HTML part (preferred, like other
             * clients), with the header fields as a small HTML fragment
             * on top. */
            m_view->set_html(with_attachment_slots(
                header_html(msg, m_expanded_addrs) + msg.html, msg));
            m_has_remote_images = m_view->has_remote_images();
        } else {
            Document doc;
            render_message(doc, msg, text_color(), meta_color());
            m_view->set_document(std::move(doc));
            make_attachment_strip(m_view);
        }
        /* Enabled when this message has remote images, or whenever loading
         * is on so it can always be switched back off.  The pushed state
         * follows the global opt-in, not the message. */
        m_images_btn->set_pushed(m_show_remote_images);
        m_images_btn->set_enabled(m_has_remote_images || m_show_remote_images);
        m_view_scroll->set_scroll(0.0f);
        redraw();
    }

    Widget *make_attachment_strip(Widget *parent) {
        auto vis = visible_attachments(m_current_message);
        if (vis.empty() || !parent) return nullptr;
        auto *strip = new AttachmentStrip(parent);
        for (size_t i = 0; i < vis.size(); ++i) {
            const MailAttachment &a = *vis[i];
            int thumb = 0;
            if (a.mime.rfind("image/", 0) == 0 && !a.data.empty()) {
                std::string key = "att:" + std::to_string(i) + ":" + a.filename;
                thumb = create_image_texture(key, a.data);
            }
            auto *chip = new AttachmentChip(strip, a, thumb);
            chip->on_open = [this, chip] { open_attachment(chip->attachment()); };
            chip->on_save = [this, chip] { save_attachment(chip->attachment()); };
            chip->on_menu = [this, chip](const Vector2i &p) {
                show_attachment_menu(chip, p);
            };
        }
        return strip;
    }

    void hide_att_popup() {
        if (!m_att_popup) return;
        m_att_popup->set_visible(false);
        if (Screen *s = screen()) {
            /* Leave it parented; next right-click rebuilds the rows. */
            (void)s;
        }
    }

    void show_attachment_menu(AttachmentChip *chip, const Vector2i &screen_pos) {
        if (!chip) return;
        Screen *s = screen();
        Window *w = chip->window();
        if (!s || !w) return;
        if (!m_att_popup)
            m_att_popup = new PopupMenu(s, w, nullptr, false);
        while (m_att_popup->child_count() > 0)
            m_att_popup->remove_child_at(m_att_popup->child_count() - 1);

        const MailAttachment att = chip->attachment();
        const bool can_open = !attachment_is_exec(att) &&
                              !attachment_is_archive(att) &&
                              is_open_allowlisted(attachment_ext(att));
        auto *open_item = new MenuItem(m_att_popup, "Open");
        open_item->set_enabled(can_open);
        open_item->set_callback([this, att] {
            hide_att_popup();
            open_attachment(att);
        });
        auto *save_item = new MenuItem(m_att_popup, "Save As\u2026");
        save_item->set_callback([this, att] {
            hide_att_popup();
            save_attachment(att);
        });

        NVGcontext *ctx = s->nvg_context();
        Vector2i pref = m_att_popup->preferred_size(ctx);
        m_att_popup->set_size(pref);
        m_att_popup->perform_layout(ctx);
        Vector2i pos = visual_screen_pos(chip, screen_pos);
        pos.x() = std::min(pos.x(), std::max(0, s->width() - pref.x()));
        if (pos.y() + pref.y() > s->height())
            pos.y() = std::max(0, pos.y() - pref.y());
        m_att_popup->set_position(pos);
        m_att_popup->set_visible(true);
        s->set_popup_visible(m_att_popup);
        redraw();
    }

    void cleanup_att_temps() {
        for (const AttTemp &t : m_att_temps) {
#ifndef _WIN32
            ::unlink(t.file.c_str());
            if (!t.dir.empty()) ::rmdir(t.dir.c_str());
#else
            DeleteFileA(t.file.c_str());
            if (!t.dir.empty()) RemoveDirectoryA(t.dir.c_str());
#endif
        }
        m_att_temps.clear();
    }

    bool write_temp_attachment(const MailAttachment &att, std::string &path_out,
                               std::string &err) {
        const std::string ext = attachment_ext(att);
        const std::string name = sanitize_filename(att.filename, ext);
#ifdef _WIN32
        char tmp[MAX_PATH];
        if (!GetTempPathA(MAX_PATH, tmp)) {
            err = "no temp directory";
            return false;
        }
        char dir[MAX_PATH];
        std::snprintf(dir, sizeof(dir), "%snmail-att-%u", tmp, (unsigned)GetTickCount());
        if (!CreateDirectoryA(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = "could not create temp folder";
            return false;
        }
        std::string path = std::string(dir) + "\\" + name;
#else
        char tmpl[] = "/tmp/nmail-att-XXXXXX";
        if (!mkdtemp(tmpl)) {
            err = "could not create temp folder";
            return false;
        }
        std::string path = std::string(tmpl) + "/" + name;
#endif
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "could not write " + path;
            return false;
        }
        out.write(att.data.data(), (std::streamsize)att.data.size());
        out.close();
        if (!out) {
            err = "could not write " + path;
            return false;
        }
#ifndef _WIN32
        ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
        m_att_temps.push_back({ path,
#ifdef _WIN32
            dir
#else
            tmpl
#endif
        });
        path_out = path;
        return true;
    }

    bool desktop_open_file(const std::string &path) {
        if (path.empty()) return false;
        for (unsigned char c : path)
            if (c < 32) return false;
#ifndef _WIN32
        if (path.front() != '/') return false;
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
#ifdef __APPLE__
            execlp("open", "open", path.c_str(), (char *)nullptr);
#else
            execlp("xdg-open", "xdg-open", path.c_str(), (char *)nullptr);
            execlp("gio", "gio", "open", path.c_str(), (char *)nullptr);
#endif
            _exit(127);
        }
        return pid > 0;
#else
        return (int)(intptr_t)ShellExecuteA(NULL, "open", path.c_str(),
                                            NULL, NULL, SW_SHOWNORMAL) > 32;
#endif
    }

    void open_attachment(const MailAttachment &att) {
        if (attachment_is_exec(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Blocked",
                "This file looks like a program or script and will not be opened.",
                "OK", "", false);
            dlg->center();
            return;
        }
        if (attachment_is_archive(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Archive",
                "Archive files are not opened automatically. Save it instead?",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att](int i) {
                if (i == 0) save_attachment(att);
            });
            dlg->center();
            return;
        }
        const std::string ext = attachment_ext(att);
        if (!is_open_allowlisted(ext)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Can't open",
                "This file type is not opened automatically. Save it instead?",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att](int i) {
                if (i == 0) save_attachment(att);
            });
            dlg->center();
            return;
        }
        std::string path, err;
        if (!write_temp_attachment(att, path, err)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Open failed", err, "OK", "", false);
            dlg->center();
            return;
        }
        if (!desktop_open_file(path)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Open failed", "Could not open " + att_display_name(att),
                "OK", "", false);
            dlg->center();
        }
    }

    void save_attachment(const MailAttachment &att) {
        const std::string ext = attachment_ext(att);
        std::string name = sanitize_filename(att.filename, ext);
        if (attachment_is_exec(att)) {
            if (name.size() < 9 || name.compare(name.size() - 9, 9, ".download") != 0)
                name += ".download";
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Executable",
                "This file looks like a program or script. It will be saved with a "
                ".download suffix so it is not run by accident.",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att, name, ext](int i) {
                if (i == 0) save_attachment_to(att, name, ext.empty() ? "bin" : ext);
            });
            dlg->center();
            return;
        }
        if (attachment_is_archive(att)) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Question,
                "Save archive",
                "Save " + att_display_name(att) + " to disk?",
                "Save As\u2026", "Cancel", true);
            dlg->set_callback([this, att, name, ext](int i) {
                if (i == 0) save_attachment_to(att, name, ext.empty() ? "zip" : ext);
            });
            dlg->center();
            return;
        }
        save_attachment_to(att, name, ext.empty() ? "dat" : ext);
    }

    void save_attachment_to(const MailAttachment &att, const std::string &name,
                            const std::string &ext) {
        auto paths = file_dialog(
            { { ext, att.mime.empty() ? ext : att.mime } },
            true, false, "");
        if (paths.empty() || paths[0].empty())
            return;
        std::string path = paths[0];
        std::string low = att_lower(path);
        std::string suffix = "." + att_lower(ext);
        if (low.size() < suffix.size() ||
            low.compare(low.size() - suffix.size(), suffix.size(), suffix) != 0)
            path += suffix;
        (void)name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        out.write(att.data.data(), (std::streamsize)att.data.size());
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
        }
    }

    void save_current_email() {
        if (!m_has_message) return;
        const MailMessage &msg = m_current_message;
        bool as_html = !msg.html.empty();
        auto paths = file_dialog(
            { {"html", "HTML email"}, {"txt", "Plain text"} },
            true, false, "");
        if (paths.empty() || paths[0].empty())
            return;
        std::string path = paths[0];
        bool has_ext = path.size() >= 5 &&
            (path.rfind(".html") == path.size() - 5 ||
             path.rfind(".htm") == path.size() - 4 ||
             path.rfind(".txt") == path.size() - 4);
        if (!has_ext)
            path += as_html ? ".html" : ".txt";

        SavedEmail e;
        e.from    = msg.from;
        e.to      = msg.to;
        e.subject = msg.subject;
        e.date    = msg.date;
        e.html    = msg.html;
        e.body    = msg.body;

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                "Save failed", "Could not write " + path, "OK", "", false);
            dlg->center();
            return;
        }
        std::string blob = nmail_serialize_email(e);
        out.write(blob.data(), (std::streamsize)blob.size());
        out.close();
        set_status("Saved " + path);
    }

    /* ---- Preferences window ---- */
    void show_preferences() {
        Window *win = new Window(this, "IMAP Preferences", false);
        win->set_id("nmail-prefs");
        win->set_close_callback([this, win] { close_dialog(win); });
        win->set_traffic_lights_mask(0x1);   // close (red) button only
        win->set_layout(new BoxLayout(Orientation::Vertical, Alignment::Fill,
                                      12, 10));
        win->set_min_width(420);

        Widget *form = new Widget(win);
        auto *form_layout = new GridLayout(Orientation::Horizontal, 2,
                                           Alignment::Middle, 0, 8);
        /* Middle centers each field at its own preferred width, so the
         * narrower ones (port numbers, the check-interval dropdown) sit
         * indented instead of lining up with the wider text fields —
         * fill the entry column so every field shares the same width. */
        form_layout->set_col_alignment(
            std::vector<Alignment>{Alignment::Middle, Alignment::Fill});
        form->set_layout(form_layout);

        new Label(form, "IMAP server:", "sans-bold");
        TextBox *host = new TextBox(form);
        host->set_value(m_config.host);
        host->set_placeholder("imap.example.com");
        host->set_editable(true);
        host->set_alignment(TextBox::Alignment::Right);

        new Label(form, "Port:", "sans-bold");
        IntBox<int> *port = new IntBox<int>(form);
        port->set_value(m_config.port);
        port->set_editable(true);

        new Label(form, "Username:", "sans-bold");
        TextBox *user = new TextBox(form);
        user->set_value(m_config.username);
        user->set_placeholder("you@example.com");
        user->set_editable(true);
        user->set_alignment(TextBox::Alignment::Right);

        new Label(form, "Password:", "sans-bold");
        TextBox *pass = new TextBox(form);
        pass->set_value(m_config.password);
        pass->set_editable(true);
        pass->set_alignment(TextBox::Alignment::Right);

        new Label(form, "SMTP server:", "sans-bold");
        TextBox *smtp_host = new TextBox(form);
        smtp_host->set_value(m_config.smtp_host);
        smtp_host->set_placeholder("(same as IMAP server)");
        smtp_host->set_editable(true);
        smtp_host->set_alignment(TextBox::Alignment::Right);

        new Label(form, "SMTP port:", "sans-bold");
        IntBox<int> *smtp_port = new IntBox<int>(form);
        smtp_port->set_value(m_config.smtp_port);
        smtp_port->set_editable(true);

        new Label(form, "Check for mail:", "sans-bold");
        Dropdown *check_interval = new Dropdown(form, Dropdown::ComboBox,
                                                "Check for mail");
        static const int kIntervalMinutes[] = {5, 15, 30, 60};
        check_interval->add_item({"Every 5 minutes", "check_5"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        check_interval->add_item({"Every 15 minutes", "check_15"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        check_interval->add_item({"Every 30 minutes", "check_30"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        check_interval->add_item({"Hourly", "check_60"}, FA_CLOCK,
                                 [] {}, {{0, 0}}, true);
        {
            int idx = 1;   // default: 15 minutes
            for (int i = 0; i < 4; ++i)
                if (kIntervalMinutes[i] == m_config.check_interval_min) idx = i;
            check_interval->set_selected_index(idx);
        }

        new Label(form, "Contacts:", "sans-bold");
        CheckBox *save_contacts = new CheckBox(form, "Remember on disk");
        save_contacts->set_checked(m_config.save_contacts);
        save_contacts->set_tooltip(
            "Keep addresses harvested from your mail in " +
            contacts_path() + " so completions survive a restart. "
            "When off they are kept only for this session.");

        Widget *buttons = new Widget(win);
        buttons->set_layout(new BoxLayout(Orientation::Horizontal,
                                          Alignment::Middle, 0, 8));

        Button *save = new Button(buttons, "Save && Connect", FA_CHECK);
        save->set_callback([this, win, host, port, user, pass,
                            smtp_host, smtp_port, check_interval,
                            save_contacts]() {
            m_config.host     = host->value();
            m_config.port     = port->value();
            m_config.username = user->value();
            m_config.password = pass->value();
            m_config.smtp_host = smtp_host->value();
            m_config.smtp_port = smtp_port->value();
            int idx = check_interval->selected_index();
            m_config.check_interval_min =
                kIntervalMinutes[(idx >= 0 && idx < 4) ? idx : 1];
            m_config.save_contacts = save_contacts->checked();
            if (!save_config(m_config)) {
                auto *dlg = new MessageDialog(this, MessageDialog::Type::Warning,
                    "Save failed",
                    "Could not write " + config_path(), "OK", "", false);
                dlg->center();
                return;
            }
            m_config_loaded = true;
            /* Write straight away so enabling the option survives a crash. */
            if (m_config.save_contacts && m_contacts.dirty())
                m_contacts.save(contacts_path());
            if (PopupMenu *pop = check_interval->popup())
                pop->set_visible(false);
            m_worker.set_config(m_config);
            m_worker.connect();
            /* Destroy the prefs window after this callback returns so we
               do not free the Save button while it is still running. */
            close_dialog(win);
            glfwPostEmptyEvent();
        });

        Button *cancel = new Button(buttons, "Cancel", FA_TIMES);
        cancel->set_callback([this, win]() { close_dialog(win); });

        win->center();
        win->request_focus();
    }

    /* ---- Reply compose window ---- */
    /* reply=false: compose a fresh message (empty To/Subject, no quote). */
    void show_compose(bool reply = true) {
        if (reply && !m_has_message) return;
        const MailMessage orig = reply ? m_current_message : MailMessage{};

        Window *win = new Window(this, reply ? "Reply" : "New Message", true);
        win->set_id(reply ? "nmail-reply" : "nmail-compose");
        win->set_close_callback([this, win] { close_dialog(win); });
        /* A single-column AdvancedGridLayout instead of a Vertical BoxLayout:
         * BoxLayout never grows children past their preferred size on the
         * main axis, so the message body would stay a fixed height no
         * matter how tall the window got. Row 4 (the body) is the only row
         * with stretch, so it alone absorbs extra height on resize.  Row 6
         * is the send-status row (hidden until a send starts). */
        auto *win_layout = new AdvancedGridLayout({0}, {0, 10, 0, 10, 0, 8, 0, 8, 0}, 12);
        win_layout->set_col_stretch(0, 1.0f);
        win_layout->set_row_stretch(4, 1.0f);
        win->set_layout(win_layout);
        /* Width only: a floor smaller than the layout's own natural size is
         * safe (there's a single column, so any slack just goes to it).
         * Deliberately no set_min_height() — the fixed chrome rows (form,
         * toolbar, buttons) can't shrink below their natural size, and the
         * body row is already floored at its own min_height(300), so the
         * layout's natural/intrinsic height *is* the right minimum; forcing
         * a smaller one would just make something else get clipped. */
        win->set_min_width(760);

        Widget *form = new Widget(win);
        win_layout->set_anchor(form, AdvancedGridLayout::Anchor(0, 0));
        /* Advanced grid so the entry column (stretch=1) absorbs all extra
         * width when the window is resized, while the label column stays
         * pinned to its natural width, flush left. */
        auto *form_layout = new AdvancedGridLayout({0, 8, 0}, {0, 8, 0}, 0);
        form_layout->set_col_stretch(2, 1.0f);
        form->set_layout(form_layout);

        Label *to_lbl = new Label(form, "To:", "sans-bold");
        AutoCompleteBox *to = new AutoCompleteBox(form);
        to->set_value(reply ? (orig.from_addr.empty() ? orig.from
                                                        : orig.from_addr)
                            : "");
        to->set_editable(true);
        /* Complete one recipient at a time so "a@x.com, ja" offers Jane. */
        to->set_token_separator(',');
        to->set_provider([this](const std::string &q) {
            std::vector<AutoCompleteBox::Item> out;
            for (const Contact &c : m_contacts.search(q, 8)) {
                AutoCompleteBox::Item it;
                it.label  = c.name.empty() ? c.address : c.name;
                it.detail = c.name.empty() ? "" : c.address;
                it.value  = format_address(c);
                out.push_back(it);
            }
            return out;
        });
        form_layout->set_anchor(to_lbl,
            AdvancedGridLayout::Anchor(0, 0, Alignment::Minimum, Alignment::Middle));
        form_layout->set_anchor(to,
            AdvancedGridLayout::Anchor(2, 0, Alignment::Fill, Alignment::Middle));

        Label *subj_lbl = new Label(form, "Subject:", "sans-bold");
        TextBox *subj = new TextBox(form);
        std::string s = reply ? orig.subject : "";
        if (reply && (s.size() < 3 || (s[0] != 'R' && s[0] != 'r') ||
            (s[1] != 'e' && s[1] != 'E') || s[2] != ':'))
            s = "Re: " + s;
        subj->set_value(s);
        subj->set_editable(true);
        form_layout->set_anchor(subj_lbl,
            AdvancedGridLayout::Anchor(0, 2, Alignment::Minimum, Alignment::Middle));
        form_layout->set_anchor(subj,
            AdvancedGridLayout::Anchor(2, 2, Alignment::Fill, Alignment::Middle));

        /* Format toolbar: WYSIWYG style toggles (Ctrl+B/I/U also work). */
        Widget *fmt = new Widget(win);
        fmt->set_layout(new BoxLayout(Orientation::Horizontal,
                                      Alignment::Middle, 0, 4));
        win_layout->set_anchor(fmt, AdvancedGridLayout::Anchor(0, 2));

        TextEditor *body = new TextEditor(win, TextEditor::Mode::RichText);
        body->set_read_only(false);
        body->set_background_color(m_dark ? Color(30, 31, 38, 255)
                                          : Color(250, 250, 252, 255));
        Style bs;
        bs.fgColor = text_color();
        bs.fontSize = (float)m_config.compose_font_size;
        body->set_default_style(bs);
        body->set_min_height(300);
        body->set_padding(10);
        win_layout->set_anchor(body, AdvancedGridLayout::Anchor(0, 4));

        /* Busy overlay shown over the body while a send is in flight:
         * same grid cell as the body, added after it so it draws on top
         * and gets input events first (children are hit-tested in reverse
         * order).  Hidden until Spinner::start(). */
        Spinner *spinner = new Spinner(win, "Sending...");
        win_layout->set_anchor(spinner, AdvancedGridLayout::Anchor(0, 4));

        /* Send-status row: label + indeterminate bar, hidden until needed. */
        Widget *status_row = new Widget(win);
        status_row->set_layout(new BoxLayout(Orientation::Horizontal,
                                             Alignment::Middle, 0, 8));
        win_layout->set_anchor(status_row,
                               AdvancedGridLayout::Anchor(0, 6));
        new Label(status_row, "Sending...", "sans", 14);
        IndeterminateBar *send_bar = new IndeterminateBar(status_row);
        send_bar->set_fixed_size(Vector2i(140, 10));
        status_row->set_visible(false);

        auto make_fmt = [&](int icon, TextEditor::StyleFlag f,
                            const std::string &tip) {
            Button *b = new Button(fmt, "", icon);
            b->set_flags(Button::Flags::ToggleButton);
            b->set_font_size(20);
            b->set_tooltip(tip);
            b->set_callback([body, f]() {
                body->toggle_style(f);
                /* Widget::mouse_button_event() hands this button focus on
                 * mouse-down (any unfocused widget gets it on click); give
                 * it back so the caret stays live and the pending typing
                 * style toggle_style() just set for an empty selection is
                 * not wiped by having to click back into the editor. */
                body->request_focus();
            });
            return b;
        };
        Button *fmt_b = make_fmt(FA_BOLD,      TextEditor::StyleFlag::Bold,
                                 "Bold (Ctrl+B)");
        Button *fmt_i = make_fmt(FA_ITALIC,    TextEditor::StyleFlag::Italic,
                                 "Italic (Ctrl+I)");
        Button *fmt_u = make_fmt(FA_UNDERLINE, TextEditor::StyleFlag::Underline,
                                 "Underline (Ctrl+U)");

        /* Paragraph-level formatting: headings, code block, bullet list.
         * These restyle the whole caret paragraph (see TextEditor). */
        auto make_par = [&](const std::string &caption, int icon,
                            const std::string &tip,
                            std::function<void()> fn) {
            Button *b = icon ? new Button(fmt, "", icon)
                             : new Button(fmt, caption);
            b->set_flags(Button::Flags::ToggleButton);
            b->set_font_size(icon ? 20 : 15);
            b->set_tooltip(tip);
            b->set_callback([fn, body]() {
                fn();
                body->request_focus();   // see make_fmt's callback for why
            });
            return b;
        };
        Button *fmt_h1 = make_par("H1", 0, "Heading 1",
                                  [body]() { body->set_paragraph_header(1); });
        Button *fmt_h2 = make_par("H2", 0, "Heading 2",
                                  [body]() { body->set_paragraph_header(2); });
        Button *fmt_h3 = make_par("H3", 0, "Heading 3",
                                  [body]() { body->set_paragraph_header(3); });
        Button *fmt_cb = make_par("</>", 0, "Code block",
                                  [body]() { body->toggle_paragraph_code(); });
        Button *fmt_ls = make_par("", FA_LIST_UL, "Bullet list",
                                  [body]() { body->toggle_paragraph_bullet(); });

        /* Base font size for the whole document.  Headings/code scale off
         * this (TextEditor::set_base_font_size), so raising it grows H1 etc.
         * too instead of leaving them frozen at whatever size they were
         * originally applied at. */
        Widget *fmt_spacer = new Widget(fmt);
        fmt_spacer->set_min_width(10);
        fmt_spacer->set_width(10);
        new Label(fmt, "Size:", "sans-bold");
        IntBox<int> *font_size = new IntBox<int>(fmt);
        font_size->set_editable(true);
        font_size->set_spinnable(true);
        font_size->set_min_max_values(8, 36);
        font_size->set_value_increment(1);
        font_size->set_fixed_size(Vector2i(56, 0));
        font_size->set_value(m_config.compose_font_size);
        auto apply_font_size = [this, body, font_size](int v) {
            font_size->set_value(v);   // clamps to [8, 36]
            v = font_size->value();
            m_config.compose_font_size = v;
            save_config(m_config);
            body->set_base_font_size((float)v);
        };
        font_size->set_callback(apply_font_size);

        /* Ctrl/Cmd +/- bumps the compose font size, mirroring the main
         * window's viewer zoom shortcut. key_filter runs before
         * TextEditor::keyboard_event's own handling and, if it returns
         * true, before MailApp::keyboard_event's fallback chain ever sees
         * the key -- so this keeps the reply window's +/- from being
         * stolen by the HTML viewer zoom behind it. */
        body->key_filter = [font_size, apply_font_size](int key, int /*scancode*/,
                                                        int action, int mods) {
            if (!(mods & SYSTEM_COMMAND_MOD) ||
                (action != GLFW_PRESS && action != GLFW_REPEAT))
                return false;
            if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
                apply_font_size(font_size->value() + 1);
                return true;
            }
            if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
                apply_font_size(font_size->value() - 1);
                return true;
            }
            return false;
        };

        /* Toolbar state follows the caret. */
        std::function<void()> refresh_fmt =
            [body, fmt_b, fmt_i, fmt_u,
             fmt_h1, fmt_h2, fmt_h3, fmt_cb, fmt_ls]() {
            Style st = body->style_at_caret();
            fmt_b->set_pushed(st.bold);
            fmt_i->set_pushed(st.italic);
            fmt_u->set_pushed(st.underline);
            int h = body->paragraph_header();
            fmt_h1->set_pushed(h == 1);
            fmt_h2->set_pushed(h == 2);
            fmt_h3->set_pushed(h == 3);
            fmt_cb->set_pushed(body->paragraph_code());
            fmt_ls->set_pushed(body->paragraph_bullet());
        };
        body->caret_callback = [refresh_fmt](TextEditor::Position) {
            refresh_fmt();
        };
        body->change_callback = refresh_fmt;

        /* Prefill (reply only): empty paragraph for the reply, then the
         * quoted original as indented paragraphs (serialized back to
         * "> " lines). */
        {
            Document *doc = body->document().get();
            doc->paragraphs.clear();
            doc->addParagraph();   // reply goes here

            if (reply) {
                doc->addParagraph();   // spacer

                Style meta_s = bs; meta_s.fgColor = meta_color();
                doc->addParagraph("On " + orig.date + ", " + orig.from +
                                  " wrote:", meta_s);

                std::istringstream iss(orig.body);
                std::string qline;
                while (std::getline(iss, qline)) {
                    if (!qline.empty() && qline.back() == '\r') qline.pop_back();
                    if (qline.empty()) continue;
                    Paragraph *qp = doc->addParagraph(qline, bs);
                    qp->leftIndent = 16.0f;
                }
            }
            doc->markLayoutDirty();
        }
        body->set_caret({0, 0});
        refresh_fmt();

        Widget *buttons = new Widget(win);
        /* Middle column has all the stretch, so it swallows the extra
         * width and pushes the action group flush against the right edge
         * of the window while the format group stays flush left. */
        auto *buttons_layout = new AdvancedGridLayout({0, 0, 0}, {0}, 0);
        buttons_layout->set_col_stretch(1, 1.0f);
        buttons->set_layout(buttons_layout);
        win_layout->set_anchor(buttons, AdvancedGridLayout::Anchor(0, 8));

        Widget *fmt_group = new Widget(buttons);
        fmt_group->set_layout(new BoxLayout(Orientation::Horizontal,
                                            Alignment::Middle, 0, 8));
        buttons_layout->set_anchor(fmt_group,
            AdvancedGridLayout::Anchor(0, 0, Alignment::Minimum, Alignment::Middle));

        /* Send format: plain text, Markdown (MailMate-style markup=
         * markdown), or a generated HTML body. */
        new Label(fmt_group, "Format:", "sans-bold");
        Dropdown *fmt_box = new Dropdown(fmt_group, Dropdown::ComboBox,
                                         "Format");
        /* NB: use the 5-arg add_item — the 2-arg overload installs no
         * callback, so clicking an item would never update the selection. */
        fmt_box->add_item({"Plain text", "fmt_plain"}, FA_FONT,
                          [] {}, {{0, 0}}, true);
        fmt_box->add_item({"Markdown", "fmt_markdown"}, FA_HASHTAG,
                          [] {}, {{0, 0}}, true);
        fmt_box->add_item({"HTML", "fmt_html"}, FA_CODE,
                          [] {}, {{0, 0}}, true);
        fmt_box->set_selected_index(1);   // Markdown
        fmt_box->set_tooltip(
            "Plain: raw text.  Markdown: plain text with markup=markdown; "
            "aware clients render it styled.  HTML: generated text/html");

        Widget *action_group = new Widget(buttons);
        action_group->set_layout(new BoxLayout(Orientation::Horizontal,
                                               Alignment::Middle, 0, 8));
        buttons_layout->set_anchor(action_group,
            AdvancedGridLayout::Anchor(2, 0, Alignment::Maximum, Alignment::Middle));

        Button *send = new Button(action_group, "Send", FA_PAPER_PLANE);
        send->set_callback([this, win, send, to, subj, body, fmt_box, spinner,
                           status_row, send_bar,
                           irt = reply ? orig.message_id : ""]() {
            std::string to_s  = to->value();
            std::string sub_s = subj->value();
            if (to_s.empty()) {
                auto *dlg = new MessageDialog(this,
                    MessageDialog::Type::Warning, "Missing recipient",
                    "Enter a recipient address first.", "OK", "", false);
                dlg->center();
                return;
            }
            send->set_enabled(false);
            /* Lock the composer and show busy feedback while SMTP runs. */
            to->set_editable(false);
            subj->set_editable(false);
            body->set_read_only(true);
            status_row->set_visible(true);
            send_bar->start();
            perform_layout();
            spinner->start();
            int fmt = fmt_box->selected_index();
            if (fmt < 0) fmt = 1;   // default to Markdown
            MailFormat format = fmt == 0 ? MailFormat::Plain
                              : fmt == 2 ? MailFormat::Html
                                         : MailFormat::Markdown;
            std::string text = fmt == 0 ? body->plain_text()
                             : fmt == 2 ? document_to_html(*body->document())
                                        : document_to_markdown(*body->document());
            send_reply(win, send, spinner, status_row, send_bar, to, subj, body,
                       to_s, sub_s, text, irt, format);
        });

        Button *cancel = new Button(action_group, "Cancel", FA_TIMES);
        cancel->set_callback([this, win]() { close_dialog(win); });

        win->center();
        win->request_focus();
    }

    /* Send on a one-shot thread (SMTP is a separate connection from the
     * IMAP worker); the result is marshalled back with nanogui::async. */
    void send_reply(Window *win, Button *send_btn, Spinner *spinner,
                    Widget *status_row, IndeterminateBar *send_bar,
                    TextBox *to_box, TextBox *subj_box,
                    TextEditor *editor,
                    const std::string &to, const std::string &subject,
                    const std::string &body, const std::string &irt,
                    MailFormat format) {
        SmtpConfig sc;
        sc.host     = m_config.smtp_host.empty() ? m_config.host
                                                 : m_config.smtp_host;
        sc.port     = m_config.smtp_port;
        sc.username = m_config.username;
        sc.password = m_config.password;
        std::string from = m_config.username;

        set_status("Sending...");
        std::thread([this, win, send_btn, spinner, status_row, send_bar, to_box,
                     subj_box, editor, sc, from, to, subject, body,
                     irt, format]() {
            SmtpClient smtp;
            std::string err;
            bool ok = smtp.send(sc, from, to, subject, body, irt, format,
                                err);
            nanogui::async(std::function<void()>(
                [this, win, send_btn, spinner, status_row, send_bar, to_box,
                 subj_box, editor, ok, err]() {
                    if (ok) {
                        set_status("Sent");
                        win->dispose();
                        sync_taskbar();     // drop its taskbar button
                        redraw();
                    } else {
                        set_status("Send failed");
                        /* Restore the composer so the user can retry. */
                        spinner->stop();
                        if (send_bar) send_bar->stop();
                        status_row->set_visible(false);
                        send_btn->set_enabled(true);
                        to_box->set_editable(true);
                        subj_box->set_editable(true);
                        editor->set_read_only(false);
                        perform_layout();
                        auto *dlg = new MessageDialog(this,
                            MessageDialog::Type::Warning,
                            "Could not send message", err, "OK", "", false);
                        dlg->center();
                    }
                }));
            glfwPostEmptyEvent();
        }).detach();
    }

    virtual bool keyboard_event(int key, int scancode,
                                int action, int modifiers) override {
        if (Screen::keyboard_event(key, scancode, action, modifiers))
            return true;
        // Ctrl/Cmd+T toggles light/dark theme at runtime
        if (key == GLFW_KEY_T && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            apply_theme_mode(m_dark ? ThemeMode::Light : ThemeMode::Dark);
            return true;
        }
        // Ctrl/Cmd+R refreshes from the server
        if (key == GLFW_KEY_R && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            do_refresh();
            return true;
        }
        // Ctrl/Cmd+S saves the current message as HTML for nmail_view
        if (key == GLFW_KEY_S && action == GLFW_PRESS &&
            (modifiers & SYSTEM_COMMAND_MOD)) {
            save_current_email();
            return true;
        }
        // Ctrl/Cmd +/-/0 zoom (20% per step, visual via ZoomScrollPanel, not reflow)
        if ((modifiers & SYSTEM_COMMAND_MOD) && action == GLFW_PRESS && m_view_scroll) {
            Vector2i anchor = Vector2i(m_view_scroll->width()/2, m_view_scroll->height()/2);
            if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
                m_view_scroll->zoom_by(1.2, anchor);
                char buf[64]; std::snprintf(buf, sizeof(buf), "Zoom %.0f%%", m_view_scroll->zoom()*100.0);
                set_status(buf); redraw(); return true;
            }
            if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
                m_view_scroll->zoom_by(1.0/1.2, anchor);
                char buf[64]; std::snprintf(buf, sizeof(buf), "Zoom %.0f%%", m_view_scroll->zoom()*100.0);
                set_status(buf); redraw(); return true;
            }
            if (key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) {
                m_view_scroll->reset_view();
                set_status("Zoom 100%"); redraw(); return true;
            }
        }
        return false;
    }

    virtual void draw(NVGcontext *ctx) override {
        pump_preview();
        sync_taskbar();
        // Background gradient
        nvgSave(ctx);
        nvgBeginPath(ctx);
        nvgRect(ctx, 0, 0, m_size.x(), m_size.y());
        NVGpaint bg = m_dark
            ? nvgLinearGradient(ctx, 0, 0, 0, (float)m_size.y(),
                                nvgRGBA(42, 44, 52, 255),
                                nvgRGBA(30, 31, 38, 255))
            : nvgLinearGradient(ctx, 0, 0, 0, (float)m_size.y(),
                                nvgRGBA(235, 237, 242, 255),
                                nvgRGBA(225, 228, 235, 255));
        nvgFillPaint(ctx, bg);
        nvgFill(ctx);
        nvgRestore(ctx);
        Screen::draw(ctx);
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    chdir_to_bundle_resources();
    try {
        /* The IMAP worker writes to a socket the server may have closed;
         * nanoproxy's socket_write handles EPIPE, but only if SIGPIPE
         * doesn't kill the process first.  Windows has no SIGPIPE. */
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        nanogui::init();
        {
            ref<MailApp> app = new MailApp();
            app->dec_ref();
            app->set_visible(true);
            app->draw_all();
            nanogui::mainloop(-1);
        }
        nanogui::shutdown();
    } catch (const std::exception &e) {
        std::string error_msg =
            std::string("Caught a fatal error: ") + std::string(e.what());
        std::cerr << error_msg << std::endl;
#ifdef _WIN32
        /* /SUBSYSTEM:WINDOWS means stderr goes nowhere -- say it in a dialog. */
        MessageBoxA(NULL, error_msg.c_str(), "nmail",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
        return -1;
    }
    return 0;
}
