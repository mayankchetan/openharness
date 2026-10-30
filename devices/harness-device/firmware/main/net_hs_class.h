// Did the daemon refuse our handshake, or did the network fail under it? One question, answered from the mbedTLS error code that ended
// cable_xport_tls.c's handshake (cable_xport_tls_last_error). Header-only so a host test can include it with the IDF copy of the mbedTLS
// headers (test_net_hs_class.c).
//
// REFUSED (the dial reached the daemon's port and the daemon would not talk to it): an alert from the daemon (unknown PSK identity,
// decrypt_error, no shared suite), a close or reset after the TCP connect, a bad MAC on the daemon's Finished. The harness that tests the TLS
// wrapper shows the two common cases: an unknown identity ends in FATAL_ALERT_MESSAGE, and a wrong key ends in CONN_RESET (the peer drops the
// stream; test_cable_xport_tls.c prints both codes). The daemon also drops, without an alert, a peer it does not trust.
//
// NOT REFUSED: a timeout (a silent or slow daemon), a failed send or receive, a failed allocation. Those say "unreachable", and the ordinary
// ladder applies. net_link.c checks that Wi-Fi is still up before it believes a reset: a reset caused by losing the access point is
// reported by the Wi-Fi events, not as a refusal.
#pragma once

#include <stdbool.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

static inline bool net_hs_refused(int mbedtls_error)
{
    switch (mbedtls_error) {
    case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
    case MBEDTLS_ERR_SSL_CONN_EOF:
    case MBEDTLS_ERR_SSL_INVALID_MAC:
    case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE:
    case MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY:
    case MBEDTLS_ERR_NET_CONN_RESET:
        return true;
    default:
        return false;
    }
}
