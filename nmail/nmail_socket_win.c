/*
 * nmail_socket_win.c — blocking client sockets for nmail on Windows.
 *
 * Mirrors nmail_socket.c (POSIX + socket_server.h) using Winsock2 and,
 * when HAVE_OPENSSL is defined, the same OpenSSL STARTTLS path.
 *
 * Public API is identical: nmail_socket.h.
 */
#include "nmail_socket.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/provider.h>
#endif

#pragma comment(lib, "ws2_32.lib")

/* nmail_socket.h uses int fds.  On Win64 SOCKET is UINT_PTR; client
 * handles fit in 32 bits in practice, matching OpenSSL's SSL_set_fd. */
#define NMAIL_INVALID_FD ((int)INVALID_SOCKET)

#ifdef HAVE_OPENSSL
#define NMAIL_MAX_TLS 64
struct nmail_tls_slot {
    int   fd;
    SSL  *ssl;
};
static struct nmail_tls_slot g_tls[NMAIL_MAX_TLS];
static CRITICAL_SECTION g_tls_lock;
static int g_tls_lock_inited;

static void tls_lock_init(void) {
    if (!g_tls_lock_inited) {
        InitializeCriticalSection(&g_tls_lock);
        g_tls_lock_inited = 1;
    }
}

static SSL *tls_get(int fd) {
    SSL *ssl = NULL;
    int i;
    tls_lock_init();
    EnterCriticalSection(&g_tls_lock);
    for (i = 0; i < NMAIL_MAX_TLS; i++) {
        if (g_tls[i].fd == fd && g_tls[i].ssl) {
            ssl = g_tls[i].ssl;
            break;
        }
    }
    LeaveCriticalSection(&g_tls_lock);
    return ssl;
}

static int tls_put(int fd, SSL *ssl) {
    int i, empty = -1;
    tls_lock_init();
    EnterCriticalSection(&g_tls_lock);
    for (i = 0; i < NMAIL_MAX_TLS; i++) {
        if (g_tls[i].fd == fd) {
            g_tls[i].ssl = ssl;
            LeaveCriticalSection(&g_tls_lock);
            return 0;
        }
        if (empty < 0 && g_tls[i].ssl == NULL)
            empty = i;
    }
    if (empty >= 0) {
        g_tls[empty].fd = fd;
        g_tls[empty].ssl = ssl;
        LeaveCriticalSection(&g_tls_lock);
        return 0;
    }
    LeaveCriticalSection(&g_tls_lock);
    return -1;
}

static SSL *tls_take(int fd) {
    SSL *ssl = NULL;
    int i;
    tls_lock_init();
    EnterCriticalSection(&g_tls_lock);
    for (i = 0; i < NMAIL_MAX_TLS; i++) {
        if (g_tls[i].fd == fd && g_tls[i].ssl) {
            ssl = g_tls[i].ssl;
            g_tls[i].ssl = NULL;
            g_tls[i].fd = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_tls_lock);
    return ssl;
}
#endif

static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK wsa_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    WSADATA wsa;
    (void)once;
    (void)param;
    (void)ctx;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return FALSE;
#ifdef HAVE_OPENSSL
    tls_lock_init();
#endif
    return TRUE;
}

static int wsa_ensure(char *errbuf, int errlen) {
    if (!InitOnceExecuteOnce(&g_wsa_once, wsa_init_cb, NULL, NULL)) {
        snprintf(errbuf, (size_t)errlen, "WSAStartup failed");
        return -1;
    }
    return 0;
}

static const char *nmail_wsa_strerror(int err) {
    switch (err) {
    case WSAETIMEDOUT:       return "timed out";
    case WSAECONNREFUSED:    return "connection refused";
    case WSAEHOSTUNREACH:    return "host unreachable";
    case WSAENETUNREACH:     return "network unreachable";
    case WSAECONNRESET:      return "connection reset";
    case WSAEWOULDBLOCK:     return "would block";
    case WSAEADDRNOTAVAIL:   return "address not available";
    default:                 return NULL;
    }
}

int nmail_sock_connect(const char *host, int port, char *errbuf, int errlen) {
    char portstr[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL, *p;
    SOCKET s = INVALID_SOCKET;
    int e, saved = 0;
    DWORD timeout_ms;
    BOOL one;

    if (wsa_ensure(errbuf, errlen) < 0)
        return -1;

    snprintf(portstr, sizeof(portstr), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    e = getaddrinfo(host, portstr, &hints, &res);
    if (e != 0) {
        snprintf(errbuf, (size_t)errlen, "DNS lookup failed for '%s': %s",
                 host, gai_strerror(e));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0)
            break;
        saved = WSAGetLastError();
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        const char *why = nmail_wsa_strerror(saved);
        if (why)
            snprintf(errbuf, (size_t)errlen, "could not connect to %s:%d: %s",
                     host, port, why);
        else
            snprintf(errbuf, (size_t)errlen, "could not connect to %s:%d: WSA error %d",
                     host, port, saved);
        return -1;
    }

    one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    /* Safety net so a hung server cannot block the worker forever.
     * Windows SO_RCVTIMEO takes a DWORD millisecond count, not timeval. */
    timeout_ms = 60000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
               sizeof(timeout_ms));

    return (int)s;
}

int nmail_sock_send(int fd, const char *buf, int len) {
    int total = 0;
#ifdef HAVE_OPENSSL
    SSL *ssl = tls_get(fd);
    if (ssl) {
        while (total < len) {
            int n = SSL_write(ssl, buf + total, len - total);
            if (n <= 0)
                return -1;
            total += n;
        }
        return total;
    }
#endif
    while (total < len) {
        int n = send((SOCKET)fd, buf + total, len - total, 0);
        if (n <= 0)
            return -1;
        total += n;
    }
    return total;
}

int nmail_sock_recv(int fd, char *buf, int maxlen) {
#ifdef HAVE_OPENSSL
    SSL *ssl = tls_get(fd);
    if (ssl) {
        int n = SSL_read(ssl, buf, maxlen);
        if (n > 0)
            return n;
        {
            int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_ZERO_RETURN)
                return 0;   /* orderly TLS close */
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
                return -2;  /* timeout */
            return -1;
        }
    }
#endif
    {
        int n = recv((SOCKET)fd, buf, maxlen, 0);
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK)
                return -2; /* receive timeout */
            return -1;
        }
        return n;
    }
}

#ifdef HAVE_OPENSSL
static SSL_CTX *g_tls_ctx = NULL;
static INIT_ONCE g_tls_ctx_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tls_ctx_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    /* Do not load C:\Program Files\OpenSSL-Win64\*.cnf — that directory
       exists on the build machine but not on a typical install. */
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                     OPENSSL_INIT_NO_LOAD_CONFIG, NULL);
    OSSL_PROVIDER_load(NULL, "default");
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (g_tls_ctx)
        SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_NONE, NULL);
    return TRUE;
}
#endif

int nmail_sock_starttls(int fd, char *errbuf, int errlen) {
    return nmail_sock_starttls_host(fd, NULL, errbuf, errlen);
}

int nmail_sock_starttls_host(int fd, const char *hostname,
                             char *errbuf, int errlen) {
#ifdef HAVE_OPENSSL
    SSL *ssl;
    InitOnceExecuteOnce(&g_tls_ctx_once, tls_ctx_init_cb, NULL, NULL);
    if (!g_tls_ctx) {
        snprintf(errbuf, (size_t)errlen, "SSL_CTX_new failed");
        return -1;
    }
    ssl = SSL_new(g_tls_ctx);
    if (!ssl || SSL_set_fd(ssl, fd) <= 0) {
        snprintf(errbuf, (size_t)errlen, "SSL setup failed");
        if (ssl) SSL_free(ssl);
        return -1;
    }
    if (hostname && hostname[0])
        SSL_set_tlsext_host_name(ssl, hostname);
    SSL_set_connect_state(ssl);
    if (SSL_connect(ssl) != 1) {
        int e = SSL_get_error(ssl, -1);
        snprintf(errbuf, (size_t)errlen, "TLS handshake failed (SSL error %d)", e);
        SSL_free(ssl);
        return -1;
    }
    if (tls_put(fd, ssl) < 0) {
        snprintf(errbuf, (size_t)errlen, "TLS slot table full");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        return -1;
    }
    return 0;
#else
    (void)fd;
    (void)hostname;
    snprintf(errbuf, (size_t)errlen, "this build has no TLS support");
    return -1;
#endif
}

void nmail_sock_abort(int fd) {
    if (fd < 0)
        return;
    /* 1 ms recv timeout so a blocking SSL_read/recv wakes on cancel.
     * shutdown() alone does not reliably interrupt OpenSSL. */
    DWORD ms = 1;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms,
               sizeof(ms));
    shutdown((SOCKET)fd, SD_BOTH);
}

void nmail_sock_close(int fd) {
#ifdef HAVE_OPENSSL
    SSL *ssl;
#endif
    if (fd < 0)
        return;
    shutdown((SOCKET)fd, SD_BOTH);
#ifdef HAVE_OPENSSL
    ssl = tls_take(fd);
    if (ssl) {
        SSL_set_quiet_shutdown(ssl, 1);
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
#endif
    closesocket((SOCKET)fd);
}
