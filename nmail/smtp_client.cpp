/*
 * smtp_client.cpp — minimal blocking SMTP client for nmail.
 *
 * Built on the same nmail_socket.h transport as the IMAP client.  No
 * pipelining, no SIZE/8BITMIME negotiation — one message per connection,
 * 8bit UTF-8 bodies (universally accepted in practice).
 */
#include "smtp_client.h"
#include "nmail_socket.h"
#include "imap_client.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

/* RFC 5321 MAIL FROM / RCPT TO take a mailbox, not "Name <addr>". */
static std::string smtp_mailbox(const std::string &s) {
    std::string t = trim(s);
    size_t lt = t.find('<');
    if (lt != std::string::npos) {
        size_t gt = t.find('>', lt);
        t = trim(t.substr(lt + 1,
                          gt == std::string::npos ? std::string::npos
                                                  : gt - lt - 1));
    }
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        t = t.substr(1, t.size() - 2);
    return trim(t);
}

static std::vector<std::string> smtp_mailboxes(const std::string &raw) {
    std::vector<std::string> out;
    for (const MailAddress &a : parse_address_list(raw))
        if (!a.address.empty())
            out.push_back(a.address);
    if (out.empty()) {
        std::string one = smtp_mailbox(raw);
        if (!one.empty())
            out.push_back(one);
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

/* RFC 2047-encode a header value if it isn't plain ASCII. */
static std::string encode_header(const std::string &s) {
    bool ascii = true;
    for (unsigned char c : s)
        if (c >= 0x80) { ascii = false; break; }
    if (ascii) return s;
    return "=?UTF-8?B?" + base64_encode(s) + "?=";
}

/* RFC 5322 date, local timezone: "Wed, 14 Aug 2026 13:27:25 +0200" */
static std::string rfc_date() {
    char buf[64];
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    if (strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S %z", &tmv) == 0)
        return "";
    return buf;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

SmtpClient::~SmtpClient() {
    close();
}

void SmtpClient::close() {
    if (m_fd >= 0) {
        nmail_sock_close(m_fd);
        m_fd = -1;
    }
    m_rbuf.clear();
}

bool SmtpClient::read_line(std::string &out, std::string &err) {
    for (;;) {
        size_t nl = m_rbuf.find('\n');
        if (nl != std::string::npos) {
            out = m_rbuf.substr(0, nl);
            m_rbuf.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        char buf[8192];
        int r = nmail_sock_recv(m_fd, buf, sizeof(buf));
        if (r == -2) { err = "timed out waiting for the server"; return false; }
        if (r <= 0)  { err = "connection to the server was lost"; return false; }
        m_rbuf.append(buf, (size_t)r);
    }
}

bool SmtpClient::read_reply(int &code, std::string &text, std::string &err) {
    code = 0;
    text.clear();
    for (;;) {
        std::string line;
        if (!read_line(line, err)) return false;
        if (line.size() < 4 || !std::isdigit((unsigned char)line[0])) {
            err = "malformed reply from server: " + line;
            return false;
        }
        if (code == 0) code = std::atoi(line.c_str());
        text += line;
        text += '\n';
        if (line[3] == ' ') return true;          // last line of the reply
        if (line[3] != '-') {
            err = "malformed reply from server: " + line;
            return false;
        }
    }
}

bool SmtpClient::command(const std::string &cmd, int expect_class,
                         std::string &reply, std::string &err) {
    std::string wire = cmd + "\r\n";
    if (nmail_sock_send(m_fd, wire.data(), (int)wire.size()) < 0) {
        err = "failed to send command (connection lost)";
        return false;
    }
    int code = 0;
    if (!read_reply(code, reply, err)) return false;
    if (code / 100 != expect_class) {
        err = cmd.substr(0, cmd.find(' ')) + " refused: " + trim(reply);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The send pipeline
// ---------------------------------------------------------------------------

bool SmtpClient::send(const SmtpConfig &cfg,
                      const std::string &from, const std::string &to,
                      const std::string &subject,
                      const std::string &body_text,
                      const std::string &in_reply_to,
                      MailFormat format,
                      std::string &err) {
    close();
    char ebuf[512] = {0};
    m_fd = nmail_sock_connect(cfg.host.c_str(), cfg.port, ebuf, sizeof(ebuf));
    if (m_fd < 0) { err = ebuf; return false; }

    /* Implicit TLS (smtps, port 465) before the greeting. */
    if (cfg.port == 465) {
        if (nmail_sock_starttls(m_fd, ebuf, sizeof(ebuf)) < 0) {
            err = ebuf;
            close();
            return false;
        }
    }

    int code = 0;
    std::string reply;
    if (!read_reply(code, reply, err) || code != 220) {
        err = err.empty() ? "server did not offer SMTP service: " + trim(reply)
                          : err;
        close();
        return false;
    }

    char hostbuf[256] = "localhost";
#ifdef _WIN32
    const char *cn = std::getenv("COMPUTERNAME");
    if (cn && cn[0]) {
        std::strncpy(hostbuf, cn, sizeof(hostbuf) - 1);
        hostbuf[sizeof(hostbuf) - 1] = '\0';
    }
#else
    if (gethostname(hostbuf, sizeof(hostbuf) - 1) != 0)
        strcpy(hostbuf, "localhost");
    hostbuf[sizeof(hostbuf) - 1] = '\0';
#endif

    if (!command(std::string("EHLO ") + hostbuf, 2, reply, err)) {
        close();
        return false;
    }

    /* Scan the EHLO reply for extensions. */
    bool has_starttls = false, has_auth_plain = false,
         has_auth_login = false, has_auth_any = false;
    {
        size_t pos = 0;
        while (pos <= reply.size()) {
            size_t nl = reply.find('\n', pos);
            std::string line = (nl == std::string::npos)
                ? reply.substr(pos) : reply.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? reply.size() + 1 : nl + 1;
            std::string ext = trim(line.size() > 4 ? line.substr(4) : "");
            for (char &c : ext) c = (char)std::toupper((unsigned char)c);
            if (ext == "STARTTLS") has_starttls = true;
            if (ext.rfind("AUTH", 0) == 0) {
                has_auth_any = true;
                if (ext.find("PLAIN") != std::string::npos) has_auth_plain = true;
                if (ext.find("LOGIN") != std::string::npos) has_auth_login = true;
            }
        }
    }

    /* STARTTLS upgrade, then the extension list must be re-read. */
    if (cfg.port != 465 && has_starttls) {
        if (!command("STARTTLS", 2, reply, err)) { close(); return false; }
        if (nmail_sock_starttls(m_fd, ebuf, sizeof(ebuf)) < 0) {
            err = ebuf;
            close();
            return false;
        }
        if (!command(std::string("EHLO ") + hostbuf, 2, reply, err)) {
            close();
            return false;
        }
        has_auth_plain = has_auth_login = has_auth_any = false;
        size_t pos = 0;
        while (pos <= reply.size()) {
            size_t nl = reply.find('\n', pos);
            std::string line = (nl == std::string::npos)
                ? reply.substr(pos) : reply.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? reply.size() + 1 : nl + 1;
            std::string ext = trim(line.size() > 4 ? line.substr(4) : "");
            for (char &c : ext) c = (char)std::toupper((unsigned char)c);
            if (ext.rfind("AUTH", 0) == 0) {
                has_auth_any = true;
                if (ext.find("PLAIN") != std::string::npos) has_auth_plain = true;
                if (ext.find("LOGIN") != std::string::npos) has_auth_login = true;
            }
        }
    }

    /* Authenticate when credentials are configured.  AUTH PLAIN first,
     * then AUTH LOGIN. */
    if (!cfg.username.empty()) {
        bool ok = false;
        std::string auth_err;
        if (has_auth_plain || !has_auth_any) {
            std::string ir = base64_encode(
                std::string(1, '\0') + cfg.username +
                std::string(1, '\0') + cfg.password);
            if (command("AUTH PLAIN " + ir, 2, reply, auth_err))
                ok = true;
        }
        if (!ok && (has_auth_login || !has_auth_any)) {
            int c = 0;
            std::string u = base64_encode(cfg.username) + "\r\n";
            std::string p = base64_encode(cfg.password) + "\r\n";
            if (!command("AUTH LOGIN", 3, reply, auth_err)) {
                /* fall through to error below */
            } else if (nmail_sock_send(m_fd, u.data(), (int)u.size()) < 0 ||
                       !read_reply(c, reply, auth_err) || c != 334) {
                auth_err = "AUTH LOGIN username refused: " + trim(reply);
            } else if (nmail_sock_send(m_fd, p.data(), (int)p.size()) < 0 ||
                       !read_reply(c, reply, auth_err) || c / 100 != 2) {
                auth_err = "AUTH LOGIN password refused: " + trim(reply);
            } else {
                ok = true;
            }
        }
        if (!ok) {
            err = "SMTP authentication failed: " +
                  (auth_err.empty() ? std::string("no supported mechanism")
                                    : auth_err);
            close();
            return false;
        }
    }

    std::string mail_from = smtp_mailbox(from);
    std::vector<std::string> rcpts = smtp_mailboxes(to);
    if (mail_from.empty() || mail_from.find('@') == std::string::npos) {
        err = "invalid From address";
        close();
        return false;
    }
    if (rcpts.empty()) {
        err = "no valid recipient address (use name@host, or Name <name@host>)";
        close();
        return false;
    }
    if (!command("MAIL FROM:<" + mail_from + ">", 2, reply, err)) {
        close();
        return false;
    }
    for (const std::string &rcpt : rcpts) {
        if (!command("RCPT TO:<" + rcpt + ">", 2, reply, err)) {
            close();
            return false;
        }
    }
    if (!command("DATA", 3, reply, err)) {
        close();
        return false;
    }

    /* Build the message: headers + dot-stuffed CRLF body. */
    std::string msg;
    msg += "From: " + from + "\r\n";
    msg += "To: " + to + "\r\n";
    msg += "Subject: " + encode_header(subject) + "\r\n";
    std::string date = rfc_date();
    if (!date.empty()) msg += "Date: " + date + "\r\n";
    {
        char mid[320];
        std::snprintf(mid, sizeof(mid), "<%ld.%d.nmail@%s>",
                      (long)time(nullptr), (int)getpid(), hostbuf);
        msg += std::string("Message-ID: ") + mid + "\r\n";
    }
    if (!in_reply_to.empty()) {
        msg += "In-Reply-To: " + in_reply_to + "\r\n";
        msg += "References: " + in_reply_to + "\r\n";
    }
    msg += "MIME-Version: 1.0\r\n";
    switch (format) {
        case MailFormat::Markdown:
            msg += "Content-Type: text/plain; charset=UTF-8; "
                   "markup=markdown\r\n";
            break;
        case MailFormat::Html:
            msg += "Content-Type: text/html; charset=UTF-8\r\n";
            break;
        case MailFormat::Plain:
        default:
            msg += "Content-Type: text/plain; charset=UTF-8\r\n";
            break;
    }
    msg += "Content-Transfer-Encoding: 8bit\r\n";
    msg += "\r\n";

    /* Body: normalize newlines to CRLF, dot-stuff leading dots. */
    bool at_line_start = true;
    for (size_t i = 0; i < body_text.size(); ++i) {
        char c = body_text[i];
        if (c == '\r') continue;                 // rebuilt as CRLF below
        if (at_line_start && c == '.') msg += '.';
        if (c == '\n') {
            msg += "\r\n";
            at_line_start = true;
        } else {
            msg += c;
            at_line_start = false;
        }
    }
    if (!at_line_start) msg += "\r\n";
    msg += ".\r\n";

    if (nmail_sock_send(m_fd, msg.data(), (int)msg.size()) < 0) {
        err = "failed while sending the message body (connection lost)";
        close();
        return false;
    }
    if (!read_reply(code, reply, err) || code / 100 != 2) {
        err = err.empty() ? "message rejected: " + trim(reply) : err;
        close();
        return false;
    }

    /* QUIT is best-effort; the message is already accepted. */
    std::string quit = "QUIT\r\n";
    nmail_sock_send(m_fd, quit.data(), (int)quit.size());
    close();
    return true;
}
