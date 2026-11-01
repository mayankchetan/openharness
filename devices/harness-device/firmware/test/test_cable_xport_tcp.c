// W11: the plain-TCP inner transport of Wi-Fi mode (cable_xport_tcp.c) over a fake socket layer. The transport's logic is real; the sk_*
// calls under it (host_tcp/tcp_host_sock.h) are scripted here: connects that succeed, hang or are refused, reads that arrive in pieces, a
// peer that closes, and a send() that takes one byte at a time, then nothing, then an error.
//
//   cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g -fsanitize=undefined,bounds -DCABLE_XPORT_TCP_HOST
//      -I ../main -I host_tcp -I host_stubs test_cable_xport_tcp.c ../main/cable_xport_tcp.c
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_xport_tcp.h"
#include "esp_timer.h"
#include "tcp_host_sock.h"

static unsigned checks;
#define CHECK(x) do { assert(x); checks++; } while (0)

// ── the fake world ─────────────────────────────────────────────────────────────────────────────────

static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }
void host_log_warn(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void host_log_info(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }

typedef struct { const char *data; int err; } rd_t;       // data (partial or whole), or err: EAGAIN, EINTR, ECONNRESET, or -1 for an orderly close
typedef struct { int accept; int err; } wr_t;             // accept up to `accept` bytes, or fail with err
typedef struct { const void *ptr; size_t len; ssize_t got; } send_t;

enum { CONN_NOW, CONN_PENDING_OK, CONN_PENDING_NEVER, CONN_REFUSED_AT_ONCE, CONN_PENDING_THEN_ERROR };
static struct {
    int next_fd, open_fds, closed_fds, sockets_made;
    int seq, shutdown_calls, shutdown_seq, shutdown_fd, close_seq, close_fd;   // order of shutdown() and close()
    int conn_mode;
    unsigned nonblock_calls, nodelay_calls, connect_calls;
    uint32_t connect_ip;
    uint16_t connect_port;
    int wait_calls_write, wait_calls_read;
    int last_wait_ms;
    // reads
    rd_t rd[16];
    unsigned rd_n, rd_at;
    size_t rd_off;
    // writes
    wr_t wr[32];
    unsigned wr_n, wr_at;
    send_t sends[256];
    unsigned send_calls;
    bool writable;
    int pauses_ms;
} fk;

static void world_reset(int conn_mode)
{
    memset(&fk, 0, sizeof fk);
    fk.next_fd = 40;
    fk.conn_mode = conn_mode;
    fk.writable = true;
    now_us = 1000000;
}

// The transport is a single static instance: let go of whatever the last scenario left open before the world is rebuilt under it.
static void fresh(int conn_mode)
{
    cable_xport_tcp()->close(cable_xport_tcp());
    world_reset(conn_mode);
}

int sk_socket(void) { fk.sockets_made++; fk.open_fds++; return fk.next_fd++; }
int sk_set_nonblock(int fd) { (void)fd; fk.nonblock_calls++; return 0; }
int sk_set_nodelay(int fd) { (void)fd; fk.nodelay_calls++; return 0; }
int sk_connect(int fd, uint32_t ip_be, uint16_t port)
{
    (void)fd;
    fk.connect_calls++;
    fk.connect_ip = ip_be;
    fk.connect_port = port;
    if (fk.conn_mode == CONN_NOW) return 0;
    if (fk.conn_mode == CONN_REFUSED_AT_ONCE) { errno = ECONNREFUSED; return -1; }
    errno = EINPROGRESS;
    return -1;
}
int sk_wait(int fd, bool for_write, int ms)
{
    (void)fd;
    fk.last_wait_ms = ms;
    if (for_write) {
        fk.wait_calls_write++;
        if (fk.conn_mode == CONN_PENDING_NEVER || !fk.writable) { now_us += (int64_t)ms * 1000; return 0; }
        return 1;
    }
    fk.wait_calls_read++;
    if (fk.rd_at < fk.rd_n) return 1;
    now_us += (int64_t)ms * 1000;
    return 0;
}
int sk_so_error(int fd) { (void)fd; return fk.conn_mode == CONN_PENDING_THEN_ERROR ? ECONNREFUSED : 0; }
ssize_t sk_recv(int fd, void *buf, size_t len)
{
    (void)fd;
    assert(fk.rd_at < fk.rd_n);
    const rd_t *e = &fk.rd[fk.rd_at];
    if (!e->data) {
        fk.rd_at++;
        if (e->err < 0) return 0;
        errno = e->err;
        return -1;
    }
    const size_t left = strlen(e->data) - fk.rd_off;
    const size_t n = left < len ? left : len;
    memcpy(buf, e->data + fk.rd_off, n);
    fk.rd_off += n;
    if (fk.rd_off == strlen(e->data)) { fk.rd_at++; fk.rd_off = 0; }
    return (ssize_t)n;
}
ssize_t sk_send(int fd, const void *buf, size_t len)
{
    (void)fd;
    assert(fk.send_calls < 256);
    ssize_t got = (ssize_t)len;
    if (fk.wr_at < fk.wr_n) {
        const wr_t e = fk.wr[fk.wr_at++];
        if (e.err) { errno = e.err; got = -1; }
        else got = (size_t)e.accept < len ? e.accept : (ssize_t)len;
    }
    fk.sends[fk.send_calls++] = (send_t){ buf, len, got };
    return got;
}
void sk_shutdown(int fd) { fk.shutdown_calls++; fk.shutdown_seq = ++fk.seq; fk.shutdown_fd = fd; }
void sk_close(int fd) { fk.close_seq = ++fk.seq; fk.close_fd = fd; fk.open_fds--; fk.closed_fds++; assert(fk.open_fds >= 0); }
bool sk_parse_ipv4(const char *s, uint32_t *ip_be)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    *ip_be = (uint32_t)a | (uint32_t)b << 8 | (uint32_t)c << 16 | (uint32_t)d << 24;   // host order is little-endian here; the byte order is the test's own
    return true;
}
void sk_pause_ms(int ms) { fk.pauses_ms += ms; now_us += (int64_t)ms * 1000; }

// ── tests ──────────────────────────────────────────────────────────────────────────────────────────

static const cable_xport_t *tcp(void) { return cable_xport_tcp(); }

static void open_ok(void)
{
    const cable_xport_t *x = tcp();
    CHECK(x->open(x) && x->is_up(x));
}

static void test_shape(void)
{
    fresh(CONN_NOW);
    const cable_xport_t *x = tcp();
    CHECK(x == cable_xport_tcp() && !strcmp(x->name, "tcp") && x->kind == CABLE_XPORT_WIFI && x->inner == NULL);
    CHECK(x->open && x->close && x->read && x->write && x->is_up && x->wait_readable);
    CHECK(!x->is_up(x));
    // Before any peer is set open() fails without making a socket; a bad address is refused too and keeps the old one unset.
    CHECK(!x->open(x) && fk.sockets_made == 0 && cable_xport_tcp_last_errno() != 0);
    CHECK(!cable_xport_tcp_set_peer("not-an-ip", 18480) && !cable_xport_tcp_set_peer("300.1.1.1", 18480) && !cable_xport_tcp_set_peer("", 18480));
    CHECK(!cable_xport_tcp_set_peer("10.0.0.5", 0));
    CHECK(!x->open(x) && fk.sockets_made == 0);
}

static void test_open(void)
{
    // Connected at once: the socket is non-blocking, TCP_NODELAY is set once, the peer is the one given.
    fresh(CONN_NOW);
    const cable_xport_t *x = tcp();
    CHECK(cable_xport_tcp_set_peer("192.168.7.9", 18480));
    CHECK(x->open(x) && x->is_up(x) && cable_xport_tcp_connected_once() && cable_xport_tcp_last_errno() == 0);
    CHECK(fk.nonblock_calls == 1 && fk.nodelay_calls == 1 && fk.connect_calls == 1 && fk.connect_port == 18480);
    uint32_t want;
    CHECK(sk_parse_ipv4("192.168.7.9", &want) && fk.connect_ip == want);
    x->close(x);
    CHECK(!x->is_up(x) && fk.open_fds == 0 && fk.closed_fds == 1);
    // shutdown() goes first, on the same fd, so a reader blocked in select() in wait_readable wakes at once.
    CHECK(fk.shutdown_calls == 1 && fk.shutdown_fd == fk.close_fd && fk.shutdown_seq < fk.close_seq);
    x->close(x);   // safe twice
    CHECK(fk.closed_fds == 1 && fk.shutdown_calls == 1);   // nothing shut down twice

    // Connect under way, then writable: up, after one wait.
    fresh(CONN_PENDING_OK);
    CHECK(x->open(x) && x->is_up(x) && cable_xport_tcp_connected_once() && fk.wait_calls_write >= 1);
    // open() again without close(): the old socket goes first, so nothing leaks.
    CHECK(x->open(x) && fk.open_fds == 1 && fk.closed_fds == 1);
    x->close(x);

    // Refused at once, or after the wait: false, nothing leaked, the errno kept, "connected once" false (a TCP failure, not a handshake one).
    fresh(CONN_REFUSED_AT_ONCE);
    CHECK(!x->open(x) && !x->is_up(x) && fk.open_fds == 0 && cable_xport_tcp_last_errno() == ECONNREFUSED && !cable_xport_tcp_connected_once());
    fresh(CONN_PENDING_THEN_ERROR);
    CHECK(!x->open(x) && fk.open_fds == 0 && cable_xport_tcp_last_errno() == ECONNREFUSED && !cable_xport_tcp_connected_once());

    // A connect that never completes gives up at 5 s, not before and not long after.
    fresh(CONN_PENDING_NEVER);
    const int64_t t0 = now_us;
    CHECK(!x->open(x) && fk.open_fds == 0 && cable_xport_tcp_last_errno() == ETIMEDOUT);
    const int64_t took_ms = (now_us - t0) / 1000;
    CHECK(took_ms >= 5000 && took_ms <= 5200);
    CHECK(fk.last_wait_ms <= 100);   // waits in short slices, so a Wi-Fi loss is noticed

    // The introspection resets at every open(): a good open after a bad one clears the old errno.
    fresh(CONN_NOW);
    CHECK(x->open(x) && cable_xport_tcp_last_errno() == 0 && cable_xport_tcp_connected_once());
    x->close(x);
}

static void test_read(void)
{
    const cable_xport_t *x = tcp();
    uint8_t buf[8];
    // Data in pieces; a read that finds nothing waits the time it was given and returns 0.
    fresh(CONN_NOW);
    open_ok();
    fk.rd[0].data = "abcdefghij";   // 10 bytes, read 8 + 2
    fk.rd[1].err = EAGAIN;
    fk.rd[2].err = EINTR;
    fk.rd[3].data = "z";
    fk.rd_n = 4;
    CHECK(x->read(x, buf, sizeof buf, 50) == 8 && !memcmp(buf, "abcdefgh", 8));
    CHECK(x->read(x, buf, sizeof buf, 50) == 2 && !memcmp(buf, "ij", 2));
    CHECK(x->read(x, buf, sizeof buf, 50) == 0 && x->is_up(x));   // readable but EAGAIN: not an error
    CHECK(x->read(x, buf, sizeof buf, 50) == 0 && x->is_up(x));   // EINTR: the same
    CHECK(x->read(x, buf, sizeof buf, 50) == 1 && buf[0] == 'z');
    const int64_t t0 = now_us;
    CHECK(x->read(x, buf, sizeof buf, 70) == 0 && fk.last_wait_ms == 70 && (now_us - t0) / 1000 == 70 && x->is_up(x));
    CHECK(x->read(x, buf, sizeof buf, 0) == 0 && fk.last_wait_ms == 0);   // poll
    CHECK(x->read(x, buf, sizeof buf, -5) == 0 && fk.last_wait_ms == 0);   // a negative wait is a poll
    // The peer closes: -1, the socket is closed, is_up says so, and the errno is left alone (an orderly close is not an error).
    fk.rd[0] = (rd_t){ NULL, -1 };
    fk.rd_n = 1; fk.rd_at = 0;
    CHECK(x->read(x, buf, sizeof buf, 50) == -1 && !x->is_up(x) && fk.open_fds == 0);
    // A dead transport still waits out its time (cable_xport.h), then says -1 again.
    fk.pauses_ms = 0;
    CHECK(x->read(x, buf, sizeof buf, 40) == -1 && fk.pauses_ms == 40);
    CHECK(x->read(x, buf, sizeof buf, 0) == -1 && fk.pauses_ms == 40);

    // A reset: -1, closed, errno kept for the net task.
    fresh(CONN_NOW);
    open_ok();
    fk.rd[0].err = ECONNRESET;
    fk.rd_n = 1;
    CHECK(x->read(x, buf, sizeof buf, 50) == -1 && !x->is_up(x) && fk.open_fds == 0 && cable_xport_tcp_last_errno() == ECONNRESET);
    x->close(x);   // after a failure: still safe, closes nothing twice
    CHECK(fk.closed_fds == 1);
}

// wait_readable: one select, nothing consumed, no state changed (the TLS wrapper calls it without its mutex).
static void test_wait_readable(void)
{
    const cable_xport_t *x = tcp();
    uint8_t buf[8];
    // No socket yet: -1, and no select.
    fresh(CONN_NOW);
    CHECK(x->wait_readable(x, 50) == -1 && fk.wait_calls_read == 0);
    open_ok();
    // Nothing to read: one select of the whole wait, 0, the link is fine.
    int64_t t0 = now_us;
    CHECK(x->wait_readable(x, 70) == 0 && fk.wait_calls_read == 1 && fk.last_wait_ms == 70 && (now_us - t0) / 1000 == 70 && x->is_up(x));
    CHECK(x->wait_readable(x, -5) == 0 && fk.last_wait_ms == 0);   // a negative wait is a poll
    // Data waiting: 1 at once, and the data is still there for read().
    fk.rd[0].data = "abc";
    fk.rd_n = 1;
    const int waits = fk.wait_calls_read;
    t0 = now_us;
    CHECK(x->wait_readable(x, 500) == 1 && fk.wait_calls_read == waits + 1 && now_us == t0);
    CHECK(fk.rd_at == 0 && fk.rd_off == 0);                         // nothing was received by the wait
    CHECK(x->read(x, buf, sizeof buf, 0) == 3 && !memcmp(buf, "abc", 3));
    // An end of stream also counts as readable (the read() that follows reports it); the wait itself does not close anything.
    fk.rd[0] = (rd_t){ NULL, -1 };
    fk.rd_n = 1; fk.rd_at = 0;
    CHECK(x->wait_readable(x, 500) == 1 && x->is_up(x) && fk.open_fds == 1 && fk.closed_fds == 0);
    CHECK(x->read(x, buf, sizeof buf, 0) == -1 && !x->is_up(x));
    // After the socket is gone: -1 again, no select.
    const int waits2 = fk.wait_calls_read;
    CHECK(x->wait_readable(x, 50) == -1 && fk.wait_calls_read == waits2);
}

// What the TLS wrapper's BIO does: keep offering the rest of the same record until it is all taken or the link dies.
static int drain(const cable_xport_t *x, const uint8_t *rec, size_t len, int wait_ms, int max_calls)
{
    size_t done = 0;
    for (int i = 0; i < max_calls && done < len; i++) {
        const int n = x->write(x, rec + done, len - done, wait_ms);
        if (n < 0) return -1;
        done += (size_t)n;
    }
    return done == len ? (int)len : (int)done;
}

static void test_write(void)
{
    const cable_xport_t *x = tcp();
    uint8_t rec[64];
    for (size_t i = 0; i < sizeof rec; i++) rec[i] = (uint8_t)(i * 5 + 1);

    // All taken: the count is len, one send() call, with the caller's pointer (no copy kept).
    fresh(CONN_NOW);
    open_ok();
    CHECK(x->write(x, rec, sizeof rec, 0) == (int)sizeof rec && fk.send_calls == 1 && fk.sends[0].ptr == rec && fk.sends[0].len == sizeof rec);
    CHECK(fk.wait_calls_write == 0);   // a poll write (wait 0) never selects
    CHECK(x->write(x, rec, 0, 0) == 0 && fk.send_calls == 1);

    // Partial: the count is what was taken, never more; the rest is offered again, same bytes, same place in the buffer.
    fresh(CONN_NOW);
    open_ok();
    fk.wr[0] = (wr_t){ 1, 0 };    // one byte
    fk.wr[1] = (wr_t){ 0, EAGAIN };   // then no room
    fk.wr[2] = (wr_t){ 0, ENOMEM };   // lwIP out of buffers: the same as no room
    fk.wr[3] = (wr_t){ 7, 0 };
    fk.wr[4] = (wr_t){ 0, EINTR };
    fk.wr_n = 5;                              // after the script: everything
    CHECK(x->write(x, rec, sizeof rec, 0) == 1);
    CHECK(x->write(x, rec + 1, sizeof rec - 1, 0) == 0 && x->is_up(x));
    CHECK(x->write(x, rec + 1, sizeof rec - 1, 0) == 0 && x->is_up(x));
    CHECK(x->write(x, rec + 1, sizeof rec - 1, 0) == 7);
    CHECK(x->write(x, rec + 8, sizeof rec - 8, 0) == 0 && x->is_up(x));
    CHECK(x->write(x, rec + 8, sizeof rec - 8, 0) == (int)sizeof rec - 8);
    // The pointer and length of the retries after a stall were the same ones.
    CHECK(fk.sends[1].ptr == rec + 1 && fk.sends[2].ptr == rec + 1 && fk.sends[3].ptr == rec + 1 && fk.sends[2].len == sizeof rec - 1);

    // One byte at a time with a stall between each: the loop a TLS record goes through. 64 bytes, 128 calls, every byte once and in order.
    fresh(CONN_NOW);
    open_ok();
    for (unsigned i = 0; i < 32; i++) { fk.wr[i] = (wr_t){ i % 2 ? 0 : 1, i % 2 ? EAGAIN : 0 }; }
    fk.wr_n = 32;
    CHECK(drain(x, rec, sizeof rec, 0, 200) == (int)sizeof rec && x->is_up(x));
    size_t seen = 0;
    for (unsigned i = 0; i < fk.send_calls; i++) {
        if (fk.sends[i].got > 0) {
            CHECK((const uint8_t *)fk.sends[i].ptr == rec + seen);   // exactly the next unsent byte, from the caller's own buffer
            seen += (size_t)fk.sends[i].got;
        }
    }
    CHECK(seen == sizeof rec);

    // A wait on a socket that has no room: select first, then 0 (nothing taken, link fine) without calling send.
    fresh(CONN_NOW);
    open_ok();
    fk.writable = false;
    const int64_t t0 = now_us;
    CHECK(x->write(x, rec, sizeof rec, 30) == 0 && fk.send_calls == 0 && fk.last_wait_ms == 30 && (now_us - t0) / 1000 == 30 && x->is_up(x));
    fk.writable = true;
    CHECK(x->write(x, rec, sizeof rec, 30) == (int)sizeof rec && fk.send_calls == 1);

    // A hard error ends the link: -1, closed, errno kept, and every later write says -1 without touching the socket.
    fresh(CONN_NOW);
    open_ok();
    fk.wr[0] = (wr_t){ 0, EPIPE };
    fk.wr_n = 1;
    CHECK(x->write(x, rec, sizeof rec, 0) == -1 && !x->is_up(x) && fk.open_fds == 0 && cable_xport_tcp_last_errno() == EPIPE);
    CHECK(x->write(x, rec, sizeof rec, 0) == -1 && fk.send_calls == 1);

    // Not open at all: -1.
    fresh(CONN_NOW);
    CHECK(x->write(x, rec, sizeof rec, 0) == -1 && fk.send_calls == 0);
}

int main(void)
{
    test_shape();
    test_open();
    test_read();
    test_wait_readable();
    test_write();
    printf("cable_xport_tcp: PASS (%u checks; connect success, hang, refusal; reads in pieces, close, reset; partial writes keep the same buffer, EAGAIN is 0, errors close)\n", checks);
    return 0;
}
