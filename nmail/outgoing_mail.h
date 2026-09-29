/*
 * outgoing_mail.h — one message the user is writing.
 *
 * Reply, forward, and a blank note share this value. The composer window
 * reads it to fill the form and writes the edited fields back before
 * send or draft save. It does not know about widgets.
 */
#ifndef OUTGOING_MAIL_H
#define OUTGOING_MAIL_H

#include "smtp_client.h"

#include <string>
#include <vector>

class OutgoingMail {
public:
    /* Header lines above a reply or forward. An empty string is a blank
     * paragraph. `lines` are the original body, one paragraph each. */
    struct Quote {
        std::vector<std::string> preface;
        std::vector<std::string> lines;
    };

    std::string account_id;
    std::string to;
    std::string subject;
    std::string in_reply_to;
    std::string body;            // editor contents, filled when sending or saving
    MailFormat  format = MailFormat::Markdown;
    std::vector<MailAttachment> attachments;
    Quote quote;
    /* Existing Drafts message this composer should overwrite on save.
     * uid 0 means a new draft. */
    std::string replace_account_id;
    std::string replace_folder;
    uint32_t    replace_uid = 0;

    static OutgoingMail fresh(const std::string &account_id);
    static OutgoingMail reply(const MailMessage &orig, const std::string &account_id);
    static OutgoingMail forward(const MailMessage &orig, const std::string &account_id);
    /* The message itself, ready to edit and send: its recipients, subject,
     * and body, not a reply or a forward quote.  Format follows the part
     * that was stored: Markdown, else HTML, else plain. */
    static OutgoingMail again(const MailMessage &orig, const std::string &account_id);

    /* Nothing the user typed: no recipient, subject, body, or attachment. */
    bool blank() const;

    /* RFC822 bytes for an IMAP APPEND. `from` is the envelope address. */
    std::string rfc822(const std::string &from) const;
};

#endif /* OUTGOING_MAIL_H */
