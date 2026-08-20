#pragma once
#include <winsock2.h>
#include <stdint.h>
#include "proxy-config.h"

// Performs the SOCKS5 greeting/auth/CONNECT handshake on an already-
// connected socket `s` (connected to `px`). Target given in network byte
// order. Uses credentials from `px`. Returns 0 on success, -1 on failure.
int socks5_handshake(SOCKET s, const struct proxy_entry *px,
                     struct in_addr target_ip, uint16_t target_port_net);
