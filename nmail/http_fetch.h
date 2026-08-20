/*
 * nmail/http_fetch.h — blocking HTTP(S) GET for image (and similar) fetches.
 *
 * Worker-thread only.  Follows a handful of redirects, asks for identity
 * encoding so the body is a raw image, and uses SNI on HTTPS.
 */
#pragma once

#include "nmail_socket.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

inline bool nmail_parse_url(const std::string &url, bool &https,
                            std::string &host, int &port,
                            std::string &path, std::string &hostport) {
    https = url.rfind("https://", 0) == 0;
    bool http = url.rfind("http://", 0) == 0;
    if (!https && !http)
        return false;
    size_t host_b = url.find("://");
    if (host_b == std::string::npos)
        return false;
    host_b += 3;
    size_t slash = url.find('/', host_b);
    hostport = url.substr(host_b, slash == std::string::npos
                                      ? std::string::npos
                                      : slash - host_b);
    path = (slash == std::string::npos) ? "/" : url.substr(slash);
    host = hostport;
    port = https ? 443 : 80;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = std::atoi(hostport.substr(colon + 1).c_str());
        if (port <= 0)
            return false;
    }
    return !host.empty();
}

inline std::string nmail_resolve_url(const std::string &base,
                                     const std::string &loc) {
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0)
        return loc;
    if (loc.rfind("//", 0) == 0) {
        bool https = base.rfind("https://", 0) == 0;
        return std::string(https ? "https:" : "http:") + loc;
    }
    bool https = false;
    std::string host, path, hostport;
    int port = 0;
    if (!nmail_parse_url(base, https, host, port, path, hostport))
        return loc;
    std::string origin = std::string(https ? "https://" : "http://") + hostport;
    if (!loc.empty() && loc[0] == '/')
        return origin + loc;
    size_t slash = path.rfind('/');
    std::string dir = (slash == std::string::npos) ? "/"
                                                   : path.substr(0, slash + 1);
    return origin + dir + loc;
}

inline bool nmail_http_get_once(const std::string &url, std::string &out,
                                int &status, std::string &location) {
    status = 0;
    location.clear();
    out.clear();

    bool https = false;
    std::string host, path, hostport;
    int port = 0;
    if (!nmail_parse_url(url, https, host, port, path, hostport))
        return false;

    char ebuf[256];
    int fd = nmail_sock_connect(host.c_str(), port, ebuf, sizeof(ebuf));
    if (fd < 0)
        return false;
    if (https &&
        nmail_sock_starttls_host(fd, host.c_str(), ebuf, sizeof(ebuf)) < 0) {
        nmail_sock_close(fd);
        return false;
    }

    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + hostport + "\r\n"
        "User-Agent: nmail_view\r\n"
        "Accept: image/*,*/*;q=0.8\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n";
    if (nmail_sock_send(fd, req.c_str(), (int)req.size()) < 0) {
        nmail_sock_close(fd);
        return false;
    }

    std::string raw;
    char buf[16384];
    for (;;) {
        int r = nmail_sock_recv(fd, buf, sizeof(buf));
        if (r <= 0)
            break;
        raw.append(buf, (size_t)r);
    }
    nmail_sock_close(fd);

    size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos)
        return false;
    std::string head = raw.substr(0, he);
    size_t sp = head.find(' ');
    if (sp == std::string::npos || sp + 1 >= head.size())
        return false;
    status = std::atoi(head.c_str() + sp + 1);
    if (status <= 0)
        return false;

    std::string lhead;
    lhead.reserve(head.size());
    for (unsigned char c : head)
        lhead += (char)std::tolower(c);

    auto loc_at = lhead.find("\nlocation:");
    if (loc_at != std::string::npos) {
        size_t vs = loc_at + 10;
        while (vs < lhead.size() && (lhead[vs] == ' ' || lhead[vs] == '\t'))
            ++vs;
        size_t ve = lhead.find('\r', vs);
        if (ve == std::string::npos)
            ve = lhead.find('\n', vs);
        if (ve == std::string::npos)
            ve = lhead.size();
        /* Location value is case-sensitive; slice the original header. */
        location = head.substr(vs, ve - vs);
        while (!location.empty() &&
               (location.back() == ' ' || location.back() == '\t'))
            location.pop_back();
    }

    std::string body = raw.substr(he + 4);
    if (lhead.find("transfer-encoding: chunked") != std::string::npos) {
        std::string dec;
        size_t pos = 0;
        while (pos < body.size()) {
            size_t eol = body.find("\r\n", pos);
            if (eol == std::string::npos)
                break;
            long n = std::strtol(body.substr(pos, eol - pos).c_str(),
                                 nullptr, 16);
            if (n <= 0)
                break;
            pos = eol + 2;
            if (pos + (size_t)n > body.size())
                break;
            dec.append(body, pos, (size_t)n);
            pos += (size_t)n + 2;
        }
        out = std::move(dec);
    } else {
        out = std::move(body);
    }
    return true;
}

inline bool nmail_http_get(const std::string &url, std::string &out) {
    std::string cur = url;
    for (int hop = 0; hop < 5; ++hop) {
        int status = 0;
        std::string loc;
        if (!nmail_http_get_once(cur, out, status, loc))
            return false;
        if (status >= 200 && status < 300)
            return !out.empty();
        if (status >= 300 && status < 400 && !loc.empty()) {
            cur = nmail_resolve_url(cur, loc);
            continue;
        }
        return false;
    }
    return false;
}
