// Plain TCP to the daemon: the inner transport of Wi-Fi mode (cable-wifi-link.md section 4.3). No TLS in here: Wi-Fi mode is
// cable_xport_tls_wrap(cable_xport_tcp(), ...), so this file is bytes in, bytes out over one IPv4 socket (cable_xport.h is the contract).
//
//   kind        CABLE_XPORT_WIFI.
//   open        Connects to the peer set by cable_xport_tcp_set_peer(): a non-blocking connect that waits at most 5 s. IPv4 only, no DNS.
//               TCP_NODELAY is on (cable frames are small and the daemon answers them). A second open() without close() closes the first.
//   read        Waits at most wait_ms (select), returns what recv gave. 0 when nothing came; -1 when the peer closed or the socket failed
//               (the socket is closed then, so is_up() says so). A dead transport still waits out wait_ms, as cable_xport.h requires.
//   write       NEVER waits for room beyond wait_ms and never buffers: it hands the bytes to send() and returns what was accepted, which may
//               be fewer than len. 0 means "none now, try again with the SAME buffer"; that is the mbedTLS "want write" rule and the TLS
//               wrapper relies on it. A hard error closes the socket and returns -1.
//   is_up       The socket is connected and has not failed.
//   wait_readable  select() for read for at most wait_ms; consumes and records nothing. 1 readable (data, end of stream or an error is
//               waiting), 0 timeout, -1 no socket or a select error. The TLS wrapper calls it WITHOUT its mutex, so an idle reader costs one
//               select per wait and holds nothing a writer needs. A socket closed during the wait may not end it at once.
//
// One instance: the dial has one link. open/read/write/close are serialised by the TLS wrapper's mutex; is_up, wait_readable and the
// introspection calls may be read from any task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cable_xport.h"

const cable_xport_t *cable_xport_tcp(void);

// The daemon's address: a dotted IPv4 string and a port. Copied. False for anything that does not parse (the transport then refuses open()).
bool cable_xport_tcp_set_peer(const char *dotted_ipv4, uint16_t port);

// About the last open(), for the net task's state machine. Both reset at the start of every open().
bool cable_xport_tcp_connected_once(void);   // the TCP connect succeeded (so a later failure was in the handshake, not the network)
int  cable_xport_tcp_last_errno(void);       // errno of the failure that ended open() or the link; 0 for none
