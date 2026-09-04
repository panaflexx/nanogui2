/*
 * nmail/nmail_config.h — account configuration (stored in amail.config).
 *
 * The password has to be recoverable to log in, so it is stored AES-256-GCM
 * encrypted under a random key kept beside the config (amail.key, mode 0600)
 * rather than under a passphrase.  It keeps the password out of the config
 * file -- which gets copied into backups, sync folders and bug reports --
 * but it does not defend against someone who can already read the user's
 * home directory.  Without OpenSSL the password falls back to plain text.
 */
#pragma once

#include <string>

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

/* Writable per-user directory: %APPDATA%\nmail on Windows, ~/.amail
 * elsewhere (created on first use; "." when there is no usable home). */
const std::string &config_dir();

/* config_dir() + "/" + name. */
std::string config_file(const char *name);

/* config_file("amail.config"). */
const std::string &config_path();

/* Load the account config; returns false when no usable config exists
 * (or no host is set).  Migrates a legacy ./amail.config if found. */
bool load_config(MailConfig &c);

/* Write the config (JSON, mode 0600 on POSIX). */
bool save_config(const MailConfig &c);
