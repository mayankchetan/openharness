// Host stand-in for the socket calls cable_xport_tcp.c makes (compile with -DCABLE_XPORT_TCP_HOST -I host_tcp). The names are the transport's own
// thin wrappers (sk_*), not libc's, so the test replaces exactly the layer between the transport's logic and lwIP. The fakes live in test_cable_xport_tcp.c.
// Errors are reported through errno, as the real calls do.
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int     sk_socket(void);                                   // an IPv4 stream socket, or -1
int     sk_set_nonblock(int fd);                           // 0 or -1
int     sk_set_nodelay(int fd);                            // 0 or -1
int     sk_connect(int fd, uint32_t ip_be, uint16_t port); // 0, or -1 with errno (EINPROGRESS for a connect under way)
int     sk_wait(int fd, bool for_write, int ms);           // 1 ready, 0 timeout, -1 error (errno)
int     sk_so_error(int fd);                               // the socket's pending error, 0 if none
ssize_t sk_recv(int fd, void *buf, size_t len);            // bytes, 0 for an orderly close, -1 with errno
ssize_t sk_send(int fd, const void *buf, size_t len);      // bytes taken (may be fewer than len), -1 with errno
void    sk_shutdown(int fd);                               // SHUT_RDWR; wakes a select() on the fd in another task
void    sk_close(int fd);
bool    sk_parse_ipv4(const char *dotted, uint32_t *ip_be);
void    sk_pause_ms(int ms);                               // wait out a time with nothing to do
