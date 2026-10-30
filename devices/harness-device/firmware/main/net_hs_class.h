// Did the daemon refuse our handshake, or did something else end it? One question, answered from the mbedTLS error code that ended
// cable_xport_tls.c's handshake (cable_xport_tls_last_error). Header-only so a host test can include it with the IDF copy of the mbedTLS
// headers (test_net_hs_class.c).
//
// REFUSED means exactly one thing: the daemon sent a fatal TLS ALERT (MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE, -0x7780). On a client that is the only way
// an alert shows up, whatever its type (decrypt_error, unknown_psk_identity, handshake_failure). Measured, mbedTLS client (the dial's wrapper,
// host build) against an OpenSSL PSK server, the library under Node's tls module:
//     wrong key                         -0x7780  fatal alert (decrypt_error after the client Finished)
//     unknown identity at the daemon    -0x7780  the daemon answers an unknown identity with a random key, so it is the wrong-key case
//     TCP FIN or RST during handshake   -0x0050  MBEDTLS_ERR_NET_CONN_RESET: the transport read failed, nobody said "no"
//     silent peer                       -0x6800  MBEDTLS_ERR_SSL_TIMEOUT
// The test harness server in test_cable_xport_tls.c closes the stream without an alert for a wrong key, which is why that test prints -0x0050 there:
// a real daemon does not behave like that, and a bare reset, an EOF or a close_notify must not be read as a refusal (a Wi-Fi blip, a daemon
// restart and a router reset all end that way).
//
// Everything else (reset, EOF, close_notify, bad MAC, local handshake failure, timeouts, send and receive errors, allocation failures) is an
// ordinary failed handshake: net_link.c posts TLS_FAIL on the 1..60 s ladder. Even an alert is only promoted to AUTH_FAILED on the third in a
// row (net_state.h net_handshake_event). net_link.c also checks that Wi-Fi is still up before it classifies anything.
#pragma once

#include <stdbool.h>

#include "mbedtls/ssl.h"

static inline bool net_hs_refused(int mbedtls_error)
{
    return mbedtls_error == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE;
}
