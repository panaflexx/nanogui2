/*
 * nmail_socket.h — thin blocking client-side socket API for nmail.
 *
 * Implemented in nmail_socket.c on top of socket_server.h from nanoproxy.
 */
#ifndef NMAIL_SOCKET_H
#define NMAIL_SOCKET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Blocking TCP connect to host:port.  Returns a socket fd >= 0 on success,
 * or -1 on failure with a human-readable message in errbuf. */
int  nmail_sock_connect(const char *host, int port, char *errbuf, int errlen);

/* Write the whole buffer.  Returns bytes written, or -1 on error. */
int  nmail_sock_send(int fd, const char *buf, int len);

/* Blocking read.  Returns bytes read (> 0), 0 on orderly close, -1 on error. */
int  nmail_sock_recv(int fd, char *buf, int maxlen);

/* Shut the socket down without closing it; wakes a recv() blocked in
 * another thread so a worker can be stopped promptly. */
void nmail_sock_abort(int fd);

/* Upgrade a connected socket to TLS (client side, for STARTTLS or implicit
 * TLS on port 993).  Returns 0 on success, -1 with a message in errbuf.
 * Only works when the build has OpenSSL enabled (HAVE_OPENSSL). */
int nmail_sock_starttls(int fd, char *errbuf, int errlen);

/* Like nmail_sock_starttls, but send `hostname` as SNI.  Image CDNs
 * (CloudFront, etc.) reject handshakes without it.  hostname may be NULL. */
int nmail_sock_starttls_host(int fd, const char *hostname,
                             char *errbuf, int errlen);

/* Shut down and close the socket, releasing bookkeeping state. */
void nmail_sock_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* NMAIL_SOCKET_H */
