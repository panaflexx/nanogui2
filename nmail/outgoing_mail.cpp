#include "outgoing_mail.h"
#include "attachment_widgets.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

bool whitespace_only(const std::string &s) {
    return std::find_if(s.begin(), s.end(), [](unsigned char c) {
        return !std::isspace(c);
    }) == s.end();
}

/* Same prefix test the composer used inline: "Re:" / "Fwd:" in either case,
 * checked only at the start of the subject. */
bool has_re(const std::string &s) {
    return s.size() >= 3 &&
           (s[0] == 'R' || s[0] == 'r') &&
           (s[1] == 'e' || s[1] == 'E') &&
           s[2] == ':';
}

bool has_fwd(const std::string &s) {
    return s.size() >= 4 &&
           (s[0] == 'F' || s[0] == 'f') &&
           (s[1] == 'W' || s[1] == 'w') &&
           (s[2] == 'D' || s[2] == 'd') &&
           s[3] == ':';
}

std::vector<std::string> body_lines(const std::string &body) {
    std::vector<std::string> lines;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        lines.push_back(std::move(line));
    }
    return lines;
}

void copy_visible_attachments(OutgoingMail &mail, const MailMessage &orig) {
    for (const MailAttachment *a : visible_attachments(orig))
        mail.attachments.push_back(*a);
}

} // namespace

OutgoingMail OutgoingMail::fresh(const std::string &account_id) {
    OutgoingMail mail;
    mail.account_id = account_id;
    return mail;
}

OutgoingMail OutgoingMail::reply(const MailMessage &orig, const std::string &account_id) {
    OutgoingMail mail = fresh(account_id);
    mail.to = orig.from_addr.empty() ? orig.from : orig.from_addr;
    mail.subject = orig.subject;
    if (!has_re(mail.subject))
        mail.subject = "Re: " + mail.subject;
    mail.in_reply_to = orig.message_id;
    mail.quote.preface.push_back("On " + orig.date + ", " + orig.from + " wrote:");
    mail.quote.lines = body_lines(orig.body);
    copy_visible_attachments(mail, orig);
    return mail;
}

OutgoingMail OutgoingMail::forward(const MailMessage &orig, const std::string &account_id) {
    OutgoingMail mail = fresh(account_id);
    mail.subject = orig.subject;
    if (!has_fwd(mail.subject))
        mail.subject = "Fwd: " + mail.subject;
    mail.quote.preface.push_back("---------- Forwarded message ----------");
    if (!orig.from.empty())
        mail.quote.preface.push_back("From: " + orig.from);
    if (!orig.date.empty())
        mail.quote.preface.push_back("Date: " + orig.date);
    if (!orig.subject.empty())
        mail.quote.preface.push_back("Subject: " + orig.subject);
    mail.quote.preface.push_back("");
    mail.quote.lines = body_lines(orig.body);
    copy_visible_attachments(mail, orig);
    return mail;
}

OutgoingMail OutgoingMail::again(const MailMessage &orig, const std::string &account_id) {
    OutgoingMail mail = fresh(account_id);
    mail.to = orig.to;
    mail.subject = orig.subject == "(no subject)" ? std::string() : orig.subject;
    /* HTML stays the HTML part. strip_html() turns the document shell
     * into blank lines and drops bold, headings, and lists. */
    if (orig.body_markdown) {
        mail.format = MailFormat::Markdown;
        mail.body = orig.body;
    } else if (!orig.html.empty()) {
        mail.format = MailFormat::Html;
        mail.body = orig.html;
    } else {
        mail.format = MailFormat::Plain;
        mail.body = orig.body;
    }
    copy_visible_attachments(mail, orig);
    return mail;
}

bool OutgoingMail::blank() const {
    return whitespace_only(to) && whitespace_only(subject) &&
           whitespace_only(body) && attachments.empty();
}

std::string OutgoingMail::rfc822(const std::string &from) const {
    return build_rfc822_message(from, to, subject, body, in_reply_to,
                                format, attachments);
}
