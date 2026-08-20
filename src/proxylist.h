#pragma once
#include "proxy-config.h"

// Starts the background fetch/refresh thread (reads g_cfg.list_url).
void proxylist_start(void);
// Signals and joins the background thread; safe to call if not started.
void proxylist_stop(void);

// Selects a proxy to use for the next connection. Prefers the fetched list
// (round-robin over resolved entries), falling back to the static proxy.
// Returns false if no usable proxy is available. Thread-safe.
bool proxy_pick(struct proxy_entry *out);

// True if `dst` matches any known proxy endpoint (static or list). Thread-safe.
bool proxy_is_known(const struct sockaddr_in *dst);

// True if the calling thread is our own fetch thread (so we don't tunnel the
// list download through a proxy).
bool proxylist_bypass_current_thread(void);
