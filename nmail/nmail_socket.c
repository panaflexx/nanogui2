/*
 * nmail_socket.c — blocking client sockets built on socket_server.h.
 *
 * socket_server.h is a server-oriented header, but it also provides the
 * connection bookkeeping (conn_add/conn_del) and socket_write() used here.
 * It must be compiled as C (it uses compound literals and `#ifdef linux`).
 */
#include "nmail_socket.h"
#define STRINGBUF_IMPLEMENTATION  /* one TU must provide stringbuf's impl */
#include "socket_server.h"
#include <pthread.h>

static pthread_once_t g_sock_init_once = PTHREAD_ONCE_INIT;

int nmail_sock_connect(const char *host, int port, char *errbuf, int errlen) {
    pthread_once(&g_sock_init_once, socket_server_init_hash);

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int e = getaddrinfo(host, portstr, &hints, &res);
    if (e != 0) {
        snprintf(errbuf, (size_t)errlen, "DNS lookup failed for '%s': %s",
                 host, gai_strerror(e));
        return -1;
    }

    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        snprintf(errbuf, (size_t)errlen, "could not connect to %s:%d: %s",
                 host, port,
                 saved_errno ? strerror(saved_errno) : "no address found");
        return -1;
    }

    {   int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }

    /* Safety net so a hung server cannot block the worker forever. */
    {   struct timeval tv;
        tv.tv_sec = 60;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); }

    /* Register in socket_server.h's connection table so socket_write()
     * and conn_del() work on this fd.  loopfd is unused bookkeeping. */
    if (conn_add(0, fd, false) < 0) {
        snprintf(errbuf, (size_t)errlen, "connection table full");
        close(fd);
        return -1;
    }
    return fd;
}

int nmail_sock_send(int fd, const char *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = socket_write(fd, buf + total, (size_t)(len - total));
        if (n <= 0)
            return -1;
        total += n;
    }
    return total;
}

int nmail_sock_recv(int fd, char *buf, int maxlen) {
#ifdef HAVE_OPENSSL
    int idx = get_conn(fd);
    if (idx >= 0 && clients[idx].ssl) {
        int n = SSL_read(clients[idx].ssl, buf, maxlen);
        if (n > 0)
            return n;
        int e = SSL_get_error(clients[idx].ssl, n);
        if (e == SSL_ERROR_ZERO_RETURN)
            return 0;   /* orderly TLS close */
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
            return -2;  /* timeout */
        return -1;
    }
#endif
    ssize_t n = recv(fd, buf, (size_t)maxlen, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -2; /* receive timeout */
        return -1;
    }
    return (int)n;
}

#ifdef HAVE_OPENSSL
/* Process-wide client context, built exactly once (concurrent fetch
 * threads raced the previous lazy init).  Certificate verification
 * intentionally disabled for now — same "don't worry about SSL" policy
 * as the rest of nmail. */
static SSL_CTX *g_tls_ctx = NULL;
static pthread_once_t g_tls_ctx_once = PTHREAD_ONCE_INIT;
static void tls_ctx_init(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                     OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (g_tls_ctx)
        SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_NONE, NULL);
}
#endif

int nmail_sock_starttls(int fd, char *errbuf, int errlen) {
    return nmail_sock_starttls_host(fd, NULL, errbuf, errlen);
}

int nmail_sock_starttls_host(int fd, const char *hostname,
                             char *errbuf, int errlen) {
#ifdef HAVE_OPENSSL
    pthread_once(&g_tls_ctx_once, tls_ctx_init);
    if (!g_tls_ctx) {
        snprintf(errbuf, (size_t)errlen, "SSL_CTX_new failed");
        return -1;
    }
    int idx = get_conn(fd);
    if (idx < 0) {
        snprintf(errbuf, (size_t)errlen, "unknown socket");
        return -1;
    }
    SSL *ssl = SSL_new(g_tls_ctx);
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
    /* socket_write() and nmail_sock_recv() pick up ->ssl automatically;
     * conn_del() shuts it down and frees it. */
    clients[idx].ssl = ssl;
    return 0;
#else
    (void)fd;
    (void)hostname;
    snprintf(errbuf, (size_t)errlen, "this build has no TLS support");
    return -1;
#endif
}

void nmail_sock_abort(int fd) {
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}

void nmail_sock_close(int fd) {
    if (fd < 0)
        return;
    shutdown(fd, SHUT_RDWR); /* wakes a recv() blocked in another thread */
    conn_del(fd);            /* closes fd and frees bookkeeping */
}
