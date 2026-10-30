// Plain TCP to the daemon. The contract is in cable_xport_tcp.h and cable_xport.h; TLS wraps this in net_link.c.
//
// The sk_* functions are the whole of this file's contact with the socket layer: five lines each over lwIP's BSD calls here, and fakes in
// test/test_cable_xport_tcp.c (compiled with -DCABLE_XPORT_TCP_HOST). Everything above them is the logic under test.
#include "cable_xport_tcp.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#ifdef CABLE_XPORT_TCP_HOST
#include "tcp_host_sock.h"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static inline int sk_socket(void) { return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }
static inline int sk_set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static inline int sk_set_nodelay(int fd)
{
    const int on = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}
static inline int sk_connect(int fd, uint32_t ip_be, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip_be;
    return connect(fd, (struct sockaddr *)&a, sizeof a);
}
static inline int sk_wait(int fd, bool for_write, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    return for_write ? select(fd + 1, NULL, &set, NULL, &tv) : select(fd + 1, &set, NULL, NULL, &tv);
}
static inline int sk_so_error(int fd)
{
    int err = 0;
    socklen_t len = sizeof err;
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 ? errno : err;
}
static inline ssize_t sk_recv(int fd, void *buf, size_t len) { return recv(fd, buf, len, 0); }
static inline ssize_t sk_send(int fd, const void *buf, size_t len) { return send(fd, buf, len, 0); }
static inline void sk_close(int fd) { close(fd); }
static inline bool sk_parse_ipv4(const char *dotted, uint32_t *ip_be)
{
    struct in_addr a;
    if (inet_pton(AF_INET, dotted, &a) != 1) return false;
    *ip_be = a.s_addr;
    return true;
}
static inline void sk_pause_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#endif

static const char *TAG = "tcp";

#define CONNECT_MS 5000       // longest open() waits for the connection
#define CONNECT_SLICE_MS 100  // it waits in slices, so a lost access point is noticed soon

static int s_fd = -1;
static atomic_bool s_up;
static bool s_connected_once;
static int s_last_errno;
static uint32_t s_ip_be;
static uint16_t s_port;
static bool s_have_peer;

bool cable_xport_tcp_set_peer(const char *dotted_ipv4, uint16_t port)
{
    uint32_t ip;
    if (!dotted_ipv4 || port == 0 || !sk_parse_ipv4(dotted_ipv4, &ip)) return false;
    s_ip_be = ip;
    s_port = port;
    s_have_peer = true;
    return true;
}

bool cable_xport_tcp_connected_once(void) { return s_connected_once; }
int  cable_xport_tcp_last_errno(void) { return s_last_errno; }

// The socket is gone and the link with it. Safe to call twice.
static void drop(void)
{
    atomic_store(&s_up, false);
    if (s_fd >= 0) {
        const int fd = s_fd;
        s_fd = -1;
        sk_close(fd);
    }
}

// A failure that ends the link: keep why, close.
static void fail(int err)
{
    s_last_errno = err;
    drop();
}

static bool again(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ENOMEM; }   // ENOMEM: lwIP has no buffer right now

static bool tcpx_open(const cable_xport_t *x)
{
    (void)x;
    drop();   // open() again without close(): the old socket goes first
    s_connected_once = false;
    s_last_errno = 0;
    if (!s_have_peer) {
        s_last_errno = EINVAL;
        return false;
    }
    const int fd = sk_socket();
    if (fd < 0) {
        s_last_errno = errno;
        ESP_LOGW(TAG, "socket failed: errno %d", s_last_errno);
        return false;
    }
    s_fd = fd;
    if (sk_set_nonblock(fd) < 0) {
        fail(errno);
        return false;
    }
    const int64_t start_us = esp_timer_get_time();
    if (sk_connect(fd, s_ip_be, s_port) < 0) {
        if (errno != EINPROGRESS) {
            fail(errno);
            ESP_LOGW(TAG, "connect failed: errno %d", s_last_errno);
            return false;
        }
        for (;;) {
            const int64_t left_ms = (int64_t)CONNECT_MS - (esp_timer_get_time() - start_us) / 1000;
            if (left_ms <= 0) {
                fail(ETIMEDOUT);
                ESP_LOGW(TAG, "connect timed out");
                return false;
            }
            const int r = sk_wait(fd, true, left_ms < CONNECT_SLICE_MS ? (int)left_ms : CONNECT_SLICE_MS);
            if (r > 0) break;
            if (r < 0 && errno != EINTR) {
                fail(errno);
                return false;
            }
        }
        const int err = sk_so_error(fd);
        if (err != 0) {
            fail(err);
            ESP_LOGW(TAG, "connect refused: errno %d", err);
            return false;
        }
    }
    // A failure here costs a little latency, not the link.
    if (sk_set_nodelay(fd) < 0) ESP_LOGW(TAG, "TCP_NODELAY not set: errno %d", errno);
    s_connected_once = true;
    atomic_store(&s_up, true);
    ESP_LOGI(TAG, "connected in %d ms", (int)((esp_timer_get_time() - start_us) / 1000));
    return true;
}

static void tcpx_close(const cable_xport_t *x)
{
    (void)x;
    drop();
}

static int tcpx_read(const cable_xport_t *x, uint8_t *buf, size_t cap, int wait_ms)
{
    (void)x;
    if (wait_ms < 0) wait_ms = 0;
    if (s_fd < 0) {   // a failed stream still waits out its time, or the reader task would spin
        if (wait_ms > 0) sk_pause_ms(wait_ms);
        return -1;
    }
    const int r = sk_wait(s_fd, false, wait_ms);
    if (r < 0) {
        if (errno == EINTR) return 0;
        fail(errno);
        return -1;
    }
    if (r == 0) return 0;
    const ssize_t n = sk_recv(s_fd, buf, cap);
    if (n > 0) return (int)n;
    if (n == 0) {   // the peer closed: not an error, but the stream is over
        drop();
        return -1;
    }
    if (again(errno)) return 0;
    fail(errno);
    return -1;
}

static int tcpx_write(const cable_xport_t *x, const uint8_t *buf, size_t len, int wait_ms)
{
    (void)x;
    if (s_fd < 0) return -1;
    if (len == 0) return 0;
    if (wait_ms > 0) {
        const int r = sk_wait(s_fd, true, wait_ms);
        if (r < 0 && errno != EINTR) {
            fail(errno);
            return -1;
        }
        if (r <= 0) return 0;   // no room within the wait: nothing taken, the link is fine
    }
    const ssize_t n = sk_send(s_fd, buf, len);
    if (n >= 0) return (int)n;   // may be fewer than len: the caller offers the rest again, same buffer
    if (again(errno)) return 0;
    fail(errno);
    return -1;
}

static bool tcpx_is_up(const cable_xport_t *x)
{
    (void)x;
    return atomic_load(&s_up);
}

static const cable_xport_t s_tcp = {
    .name = "tcp",
    .kind = CABLE_XPORT_WIFI,
    .open = tcpx_open,
    .close = tcpx_close,
    .read = tcpx_read,
    .write = tcpx_write,
    .is_up = tcpx_is_up,
};

const cable_xport_t *cable_xport_tcp(void) { return &s_tcp; }
