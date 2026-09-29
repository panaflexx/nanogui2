/*
 * nmail/mail_format.h — message body formatting:
 *
 *  - parse_markdown / document_to_markdown: Markdown <-> nanogui::Document
 *    (compose editor input and markup=markdown send path)
 *  - document_to_html / html_to_document: nanogui::Document <-> HTML email body
 *  - header_html / body_as_html: the parchment header card and the
 *    plain/Markdown body fallback used by the reading pane
 */
#pragma once

#include <nanogui/document.h>

#include <set>
#include <string>

#include "imap_client.h"   // MailMessage

/* Private scheme for the header's name links.  is_allowed_url() only ever
 * lets http/https/mailto reach the browser, so an unhandled click here is
 * inert rather than dangerous. */
extern const char *kAddrScheme;

/* Minimal Markdown -> Document converter.
 * Supports: # / ## / ### headers, **bold**, *italic*, `code`, ```fenced```,
 * "> " blockquotes, "- " / "* " bullet lists, \escapes.
 * Blank lines start a new paragraph. */
void parse_markdown(nanogui::Document &doc, const std::string &md,
                    NVGcolor text_color = nvgRGBA(20, 20, 25, 255),
                    float base_size = 16.0f);

/* Serialize a Document back to Markdown.  Inverse of parse_markdown;
 * used at send time for markup=markdown messages. */
std::string document_to_markdown(const nanogui::Document &doc);

/* Serialize a Document to an HTML email body.  Mirrors
 * document_to_markdown's structure detection (headings by font size,
 * all-monospace paragraphs -> <pre>, isBullet -> <ul>/<li> with nesting by
 * indent level, leftIndent -> <blockquote>, isRule -> <hr>). */
std::string document_to_html(const nanogui::Document &doc);

/* Load HTML the composer itself writes back into a Document: paragraphs,
 * headings, bold/italic/underline, links, lists, quotes, code, rules.
 * html/body wrappers and the whitespace between them do not become blank
 * paragraphs.  Returns false for any other markup (tables, images, styles,
 * layout) and leaves `doc` unchanged — that message is edited as HTML
 * source instead of being flattened. */
bool html_to_document(nanogui::Document &doc, const std::string &html,
                      NVGcolor text_color = nvgRGBA(20, 20, 25, 255),
                      float base_size = 16.0f);

std::string html_escape(const std::string &s);

/* One <div> card: subject and an Edit-for-send slot on the top row, then
 * the From/To/Date rows.  The card carries its own fixed parchment palette
 * (independent of the light/dark theme) and is marked data-nmail-chrome so
 * it stays left-aligned and full width inside a right-aligned message. */
std::string header_html(const MailMessage &msg,
                        const std::set<std::string> &expanded);

/* Plain / Markdown body as an HTML fragment so it can share header_html()
 * with text/html mail.  Newlines stay as <br> (bounce reports, signatures);
 * Markdown goes through parse_markdown then paragraphs_to_html. */
std::string body_as_html(const MailMessage &msg);
