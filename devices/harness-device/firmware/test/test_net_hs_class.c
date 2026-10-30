// W11: which handshake failures count as "the daemon refused us" (net_hs_class.h). Built against the IDF copy of the mbedTLS headers by
// test_net_hs_class.py, so the names are the real ones and the numbers are checked against the codes the TLS wrapper's own test prints.
#include <assert.h>
#include <stdio.h>

#include "net_hs_class.h"

int main(void)
{
    // The two cases the TLS wrapper's test shows for a daemon that says no.
    assert(net_hs_refused(-0x7780));   // unknown identity, no shared suite: a fatal alert
    assert(net_hs_refused(-0x0050));   // wrong key: the peer drops the stream
    assert(net_hs_refused(MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY));
    assert(net_hs_refused(MBEDTLS_ERR_SSL_CONN_EOF));
    assert(net_hs_refused(MBEDTLS_ERR_SSL_INVALID_MAC));
    assert(net_hs_refused(MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE));
    assert(net_hs_refused(MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY));
    // A silent or slow daemon, a broken pipe, a full heap and a success are not refusals.
    assert(!net_hs_refused(0));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_TIMEOUT));
    assert(!net_hs_refused(MBEDTLS_ERR_NET_SEND_FAILED));
    assert(!net_hs_refused(MBEDTLS_ERR_NET_RECV_FAILED));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_ALLOC_FAILED));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_BAD_INPUT_DATA));
    assert(!net_hs_refused(MBEDTLS_ERR_SSL_WANT_READ));
    printf("net_hs_class: PASS (refusal codes: alert, reset, close, bad MAC, handshake failure, unknown identity; timeouts and send errors are not)\n");
    return 0;
}
