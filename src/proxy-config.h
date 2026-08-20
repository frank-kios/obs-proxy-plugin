#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

// How a proxy is chosen from the fetched list.
enum {
    PROXY_SELECT_ROUNDROBIN = 0,
    PROXY_SELECT_RANDOM     = 1,
    PROXY_SELECT_MANUAL     = 2,
};

// A single SOCKS5 proxy (static config entry or one line of a fetched list).
struct proxy_entry {
    char host[256];
    uint16_t port;
    bool use_auth;
    char username[256];
    char password[256];

    struct sockaddr_in resolved;
    bool resolved_ok;
};

struct proxy_config {
    volatile bool enabled;      // master on/off
    bool proxy_browser;         // also proxy CEF browser sources

    // Static proxy, used when the list is disabled or empty.
    struct proxy_entry single;

    // Proxy list fetched from a URL.
    bool use_list;
    char list_url[1024];
    int refresh_sec;            // re-fetch interval; 0 = fetch once

    struct proxy_entry *list;   // heap-allocated array of parsed entries
    int list_count;
    volatile LONG rr_index;     // round-robin cursor

    int select_mode;            // PROXY_SELECT_*
    int selected_index;         // used when select_mode == PROXY_SELECT_MANUAL

    CRITICAL_SECTION lock;      // guards list / list_count / single
    bool lock_ready;
};

extern struct proxy_config g_cfg;
