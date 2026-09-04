/*
 * nmail/mail_debug.h — mail_dbg() debug logging macro, shared by the
 * IMAP worker and the application shell.  Enable with -DNMAIL_IMAP_DEBUG=1
 * (imap_client.h has the matching protocol-level logging).
 */
#pragma once

#include <cstdio>

#ifndef NMAIL_IMAP_DEBUG
#define NMAIL_IMAP_DEBUG 0
#endif

#if NMAIL_IMAP_DEBUG
#define mail_dbg(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#else
#define mail_dbg(...) ((void)0)
#endif
