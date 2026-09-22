/*
 * nmail/nmail_config.cpp — implementation of nmail_config.h.
 * JSON config via dict.h; password encryption via OpenSSL AES-256-GCM.
 */

#include "nmail_config.h"
#include "dict.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifdef HAVE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#endif

/* Writable per-user directory: %APPDATA%\nmail on Windows, ~/.amail elsewhere.
   Program Files (or whatever the CWD happens to be) is not a place for prefs.
   The directory holds amail.config and amail.key. */
const std::string &config_dir() {
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

std::string config_file(const char *name) {
#ifdef _WIN32
    return config_dir() + "\\" + name;
#else
    return config_dir() + "/" + name;
#endif
}

const std::string &config_path() {
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

static int config_get_int(const DictValue *root, const char *key, int fallback) {
    const DictValue *v = dict_object_get(root, key);
    if (v && v->type == DICT_INT64)  return (int)v->int64_value;
    if (v && v->type == DICT_NUMBER) return (int)v->number_value;
    return fallback;
}

static bool config_get_bool(const DictValue *root, const char *key, bool fallback) {
    const DictValue *v = dict_object_get(root, key);
    return (v && v->type == DICT_BOOL) ? (v->bool_value != 0) : fallback;
}

/* One account object -> MailAccount, decrypting password_enc when present. */
static MailAccount parse_account(const DictValue *obj) {
    MailAccount a;
    a.name       = config_get_str(obj, "name");
    a.host       = config_get_str(obj, "host");
    a.port       = config_get_int(obj, "port", 143);
    a.username   = config_get_str(obj, "username");
    a.password   = config_get_str(obj, "password");   /* legacy plain text */
    a.smtp_host  = config_get_str(obj, "smtp_host");
    a.smtp_port  = config_get_int(obj, "smtp_port", 587);
    const std::string enc = config_get_str(obj, "password_enc");
    if (!enc.empty()) {
        std::string plain;
        if (decrypt_secret(enc, plain))
            a.password = plain;
        else
            std::cerr << "[nmail] could not decrypt the stored password for "
                      << (a.name.empty() ? a.username : a.name) << " ("
                      << config_file("amail.key")
                      << " missing or stale); re-enter it in Preferences"
                      << std::endl;
    }
    return a;
}

static DictValue *serialize_account(const MailAccount &a) {
    DictValue *obj = dict_create_object();
    dict_object_set(obj, "name",     dict_create_string(a.name.c_str()));
    dict_object_set(obj, "host",     dict_create_string(a.host.c_str()));
    dict_object_set(obj, "port",     dict_create_int64(a.port));
    dict_object_set(obj, "username", dict_create_string(a.username.c_str()));
    std::string enc;
    if (encrypt_secret(a.password, enc))
        dict_object_set(obj, "password_enc", dict_create_string(enc.c_str()));
    else
        dict_object_set(obj, "password", dict_create_string(a.password.c_str()));
    dict_object_set(obj, "smtp_host", dict_create_string(a.smtp_host.c_str()));
    dict_object_set(obj, "smtp_port", dict_create_int64(a.smtp_port));
    return obj;
}

bool load_config(MailConfig &c) {
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

    c.accounts.clear();
    const DictValue *accounts = dict_object_get(root, "accounts");
    if (accounts && accounts->type == DICT_ARRAY) {
        for (size_t i = 0; i < accounts->array_value.length; ++i) {
            const DictValue *obj = accounts->array_value.items[i];
            if (obj && obj->type == DICT_OBJECT)
                c.accounts.push_back(parse_account(obj));
        }
    } else {
        /* Legacy single-account config: top-level host/username/... with no
         * "accounts" array. Migrate it into a one-element accounts vector. */
        MailAccount a = parse_account(root);
        if (!a.host.empty())
            c.accounts.push_back(a);
    }

    c.dark_mode          = config_get_bool(root, "dark_mode", false);
    c.save_contacts      = config_get_bool(root, "save_contacts", false);
    c.check_interval_min = config_get_int(root, "check_interval_min", 15);
    c.cache_limit_mb     = config_get_int(root, "cache_limit_mb", 512);
    c.compose_font_size  = config_get_int(root, "compose_font_size", 16);
    if (c.cache_limit_mb != 256 && c.cache_limit_mb != 512 &&
        c.cache_limit_mb != 1024 && c.cache_limit_mb != 2048)
        c.cache_limit_mb = 512;
    dict_destroy(root);
    return !c.accounts.empty();
}

bool save_config(const MailConfig &c) {
    DictValue *root = dict_create_object();
    DictValue *accounts = dict_create_array();
    for (const MailAccount &a : c.accounts)
        dict_array_append(accounts, serialize_account(a));
    dict_object_set(root, "accounts", accounts);
    dict_object_set(root, "dark_mode", dict_create_bool(c.dark_mode ? 1 : 0));
    dict_object_set(root, "save_contacts",
                    dict_create_bool(c.save_contacts ? 1 : 0));
    dict_object_set(root, "check_interval_min", dict_create_int64(c.check_interval_min));
    dict_object_set(root, "cache_limit_mb", dict_create_int64(c.cache_limit_mb));
    dict_object_set(root, "compose_font_size", dict_create_int64(c.compose_font_size));

    bool ok = false;
    char *buf = dict_serialize_json_heap(root, /*pretty=*/1);
    if (buf) {
        std::ofstream out(config_path(), std::ios::binary | std::ios::trunc);
        if (out) {
            out << buf;
            ok = (bool)out;
        }
#ifndef _WIN32
        if (ok) ::chmod(config_path().c_str(), S_IRUSR | S_IWUSR);
#endif
        free(buf);
    }
    dict_destroy(root);
    return ok;
}
