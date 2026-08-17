/*
 * smtp_client.h — minimal blocking SMTP client for nmail.
 *
 * Same driving model as ImapClient: one worker thread, every call blocks
 * until the server answers (or the socket times out).  Supports plain
 * SMTP, STARTTLS (587), and implicit TLS (465); AUTH PLAIN / LOGIN.
 */
#ifndef SMTP_CLIENT_H
#define SMTP_CLIENT_H

#include <string>

struct SmtpConfig {
    std::string host;
    int         port = 587;
    std::string username;
    std::string password;
};

/* Body format for SmtpClient::send. */
enum class MailFormat {
    Plain,      /* text/plain */
    Markdown,   /* text/plain; markup=markdown (MailMate convention: the
                   body is plain text holding Markdown) */
    Html        /* text/html */
};

class SmtpClient {
public:
    SmtpClient() = default;
    ~SmtpClient();

    /* Connect, authenticate if credentials are given, and send one
     * message.  from/to are bare addresses.  in_reply_to is an optional
     * Message-ID the message replies to (sets In-Reply-To and
     * References).  format selects the Content-Type (see MailFormat).
     * Returns false with a human-readable err on failure. */
    bool send(const SmtpConfig &cfg,
              const std::string &from, const std::string &to,
              const std::string &subject, const std::string &body_text,
              const std::string &in_reply_to, MailFormat format,
              std::string &err);

private:
    int m_fd = -1;
    std::string m_rbuf;

    void close();

    /* Read a (possibly multi-line) SMTP reply.  code gets the 3-digit
     * status, text the full reply for error reporting. */
    bool read_reply(int &code, std::string &text, std::string &err);
    bool read_line(std::string &out, std::string &err);

    /* Send cmd + CRLF, read the reply, require code / 100 == expect_class
     * (e.g. 2 for any 2xx). */
    bool command(const std::string &cmd, int expect_class,
                 std::string &reply, std::string &err);
};

#endif /* SMTP_CLIENT_H */
