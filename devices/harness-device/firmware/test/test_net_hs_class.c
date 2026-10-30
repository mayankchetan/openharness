// W11: which handshake failures count as "the daemon refused us" (net_hs_class.h). Built against the IDF copy of the mbedTLS headers by
// test_net_hs_class.py, so the names are the real ones and the numbers are checked against the codes the TLS wrapper's own test prints.
#include <assert.h>
#include <stdio.h>

#include "mbedtls/net_sockets.h"
#include "net_hs_class.h"

int main(void)
{
    // Only a fatal alert from the daemon is a refusal (-0x7780: what a wrong key and an unknown identity give against OpenSSL).
    assert(net_hs_refused(-0x7780));
    assert(net_hs_refused(MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE));
    // A bare reset or EOF (-0x0050: FIN or RST during the handshake), a close_notify, a bad MAC, a local handshake failure and an unknown identity
    // code are not: nobody told the dial "no".
    assert(!net_hs_refused(-0x0050));
    assert(!net_hs_refused(MBEDTLS_ERR_NET_CONN_RESET));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_CONN_EOF));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_INVALID_MAC));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY));
    // A silent or slow daemon, a broken pipe, a full heap and a success are not refusals either.
    assert(!net_hs_refused(0));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_TIMEOUT));
    assert(!net_hs_refused(MBEDTLS_ERR_NET_SEND_FAILED));
    assert(!net_hs_refused(MBEDTLS_ERR_NET_RECV_FAILED));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_ALLOC_FAILED));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_BAD_INPUT_DATA));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_WANT_READ));
    printf("net_hs_class: PASS (only a fatal alert is a refusal; reset, EOF, close_notify, bad MAC, timeouts and send errors are not)\n");
    return 0;
}
