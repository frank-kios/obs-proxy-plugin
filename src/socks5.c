#include "socks5.h"
#include "proxy-config.h"
#include <string.h>

// select-based IO so we work whether the socket is blocking or non-blocking
static int io_wait(SOCKET s, int for_write, int timeout_sec)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    struct timeval tv = { timeout_sec, 0 };
    return select(0, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv);
}

static int send_all(SOCKET s, const uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = send(s, (const char *)buf + off, len - off, 0);
        if (n > 0) { off += n; continue; }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (io_wait(s, 1, 10) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int recv_all(SOCKET s, uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = recv(s, (char *)buf + off, len - off, 0);
        if (n > 0) { off += n; continue; }
        if (n == 0) return -1; // closed
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            if (io_wait(s, 0, 10) <= 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int socks5_handshake(SOCKET s, const struct proxy_entry *px,
                     struct in_addr target_ip, uint16_t target_port_net)
{
    uint8_t resp[2];

    // 1) greeting: VER=5, NMETHODS=1, METHOD (0x00 none / 0x02 user-pass)
    uint8_t hello[3] = { 0x05, 0x01, px->use_auth ? 0x02 : 0x00 };
    if (send_all(s, hello, sizeof(hello)) != 0) return -1;
    if (recv_all(s, resp, 2) != 0) return -1;
    if (resp[0] != 0x05) return -1;

    // 2) optional username/password auth (RFC 1929)
    if (resp[1] == 0x02) {
        if (!px->use_auth) return -1;
        uint8_t ulen = (uint8_t)strlen(px->username);
        uint8_t plen = (uint8_t)strlen(px->password);
        uint8_t buf[1 + 1 + 255 + 1 + 255];
        int i = 0;
        buf[i++] = 0x01;                 // auth subnegotiation version
        buf[i++] = ulen; memcpy(buf + i, px->username, ulen); i += ulen;
        buf[i++] = plen; memcpy(buf + i, px->password, plen); i += plen;
        if (send_all(s, buf, i) != 0) return -1;
        if (recv_all(s, resp, 2) != 0) return -1;
        if (resp[1] != 0x00) return -1;  // auth failed
    } else if (resp[1] != 0x00) {
        return -1;                       // no acceptable method
    }

    // 3) CONNECT request: VER,CMD=1,RSV,ATYP=1(IPv4),IP(4),PORT(2)
    uint8_t req[10] = { 0x05, 0x01, 0x00, 0x01 };
    memcpy(req + 4, &target_ip.s_addr, 4);   // network order
    memcpy(req + 8, &target_port_net, 2);    // network order
    if (send_all(s, req, sizeof(req)) != 0) return -1;

    // 4) reply: VER,REP,RSV,ATYP,BND.ADDR,BND.PORT (IPv4 => 10 bytes)
    uint8_t reply[10];
    if (recv_all(s, reply, 4) != 0) return -1;
    if (reply[1] != 0x00) return -1;         // REP != success
    int rest = (reply[3] == 0x01) ? 6 : (reply[3] == 0x04) ? 18 : -1;
    if (rest < 0) return -1;
    uint8_t skip[18];
    if (recv_all(s, skip, rest) != 0) return -1;

    return 0;
}
